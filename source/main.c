#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>

// Extended MTP Operation Codes
#define MTP_OP_OPEN_SESSION         0x1002
#define MTP_OP_GET_OBJECT_HANDLES   0x1007
#define MTP_OP_GET_OBJECT_INFO      0x1008
#define MTP_OP_GET_OBJECT           0x1009

#define MAX_ITEMS_DISPLAYED         12

// Alignment matching Wii hardware architecture constraints
typedef struct {
    u32 length;
    u16 type; 
    u16 code;
    u32 transaction;
} __attribute__((packed)) MTP_Header;

// Cache structure to hold mapped files locally
typedef struct {
    u32 handle;
    char filename[64]; // Fix: Explicitly sized string array container
} FileCacheEntry;

static s32 usb_device = -1;
static u8 ep_in = 0x81;
static u8 ep_out = 0x02;
static u32 global_tx = 1;

static FileCacheEntry discovered_files[MAX_ITEMS_DISPLAYED];
static u32 total_files_found = 0;

void PrintUpdate(const char *msg) {
    printf("%s\n", msg);
    VIDEO_WaitVSync();
}

bool InitUSBAndFindAndroid() {
    u8 dev_count = 0;
    // Fix: Declaring an explicit array list allocation for libogc structures
    usb_device_entry dev_list[8];
    memset(dev_list, 0, sizeof(dev_list));

    USB_Initialize();

    // Fix: Pass array reference down properly to retrieve elements
    s32 ret = USB_GetDeviceList(dev_list, 8, 0, &dev_count);
    if (ret < 0 || dev_count == 0) return false;

    for (int i = 0; i < dev_count; i++) {
        // Generic Android Vendor IDs (Google, MediaTek, Samsung)
        if (dev_list[i].vid == 0x18d1 || dev_list[i].vid == 0x0e8d || dev_list[i].vid == 0x04e8) {
            if (USB_OpenDevice(dev_list[i].device_id, dev_list[i].vid, dev_list[i].pid, &usb_device) == 0) {
                return true;
            }
        }
    }
    return false;
}

s32 TransactionMTP(u16 op_code, void* data_buf, u32 data_len, bool is_write) {
    MTP_Header cmd;
    cmd.length = sizeof(MTP_Header) + (is_write ? data_len : 0);
    cmd.type = 1; 
    cmd.code = op_code;
    cmd.transaction = global_tx++;

    s32 ret = USB_WriteBlkMsg(usb_device, ep_out, sizeof(MTP_Header), &cmd);
    if (ret < 0) return ret;

    if (data_buf && data_len > 0) {
        if (is_write) {
            ret = USB_WriteBlkMsg(usb_device, ep_out, data_len, data_buf);
        } else {
            ret = USB_ReadBlkMsg(usb_device, ep_in, data_len, data_buf);
        }
    }
    return ret;
}

// Interrogates a file handle to unpack its string metadata descriptor
void FetchFilename(u32 handle, char* out_name, size_t max_len) {
    u8 *info_buffer = memalign(32, 1024);
    if (!info_buffer) return;
    
    memset(info_buffer, 0, 1024);
    strncpy(out_name, "Unknown File Asset", max_len);

    global_tx++; 
    MTP_Header cmd;
    cmd.length = sizeof(MTP_Header);
    cmd.type = 1; 
    cmd.code = MTP_OP_GET_OBJECT_INFO;
    cmd.transaction = global_tx;

    if (USB_WriteBlkMsg(usb_device, ep_out, sizeof(MTP_Header), &cmd) >= 0) {
        USB_WriteBlkMsg(usb_device, ep_out, sizeof(u32), &handle);
        
        s32 read_bytes = USB_ReadBlkMsg(usb_device, ep_in, 1024, info_buffer);
        if (read_bytes > 64) {
            // Read character length byte from the standard MTP layout payload offset position
            u8 name_len_chars = info_buffer[52];
            u16 *utf16_ptr = (u16*)(info_buffer + 53);

            if (name_len_chars > 0 && read_bytes > (53 + (name_len_chars * 2))) {
                size_t out_idx = 0;
                for (out_idx = 0; out_idx < name_len_chars && out_idx < (max_len - 1); out_idx++) {
                    out_name[out_idx] = (char)(utf16_ptr[out_idx] & 0x00FF);
                }
                out_name[out_idx] = '\0';
            }
        }
    }
    free(info_buffer);
}

void BrowseAndroidFiles() {
    u8 *raw_buffer = memalign(32, 4096);
    if (!raw_buffer) return;

    PrintUpdate("Querying file structures from Android storage...");
    s32 read_bytes = TransactionMTP(MTP_OP_GET_OBJECT_HANDLES, raw_buffer, 4096, false);
    
    if (read_bytes > 12) {
        u32 *elements = (u32*)(raw_buffer + 12);
        u32 array_len = elements[0]; 
        
        total_files_found = (array_len > MAX_ITEMS_DISPLAYED) ? MAX_ITEMS_DISPLAYED : array_len; 
        
        for(u32 i = 0; i < total_files_found; i++) {
            discovered_files[i].handle = elements[i + 1];
            printf("Parsing item %u of %u...\n", i + 1, total_files_found);
            FetchFilename(discovered_files[i].handle, discovered_files[i].filename, 64);
        }
    } else {
        PrintUpdate("Failed parsing objects or partition is empty.");
    }
    free(raw_buffer);
}

bool SyncSelectedFile(u32 object_id, const char* out_filename) {
    char target_path[128]; // Fix: Explicitly sized array configuration bounds
    snprintf(target_path, sizeof(target_path), "sd:/%s", out_filename);
    
    FILE *target_file = fopen(target_path, "wb");
    if (!target_file) return false;

    u32 chunk_size = 64 * 1024;
    u8 *io_buffer = memalign(32, chunk_size);
    if (!io_buffer) {
        fclose(target_file);
        return false;
    }
    
    global_tx++;
    MTP_Header cmd;
    cmd.length = sizeof(MTP_Header);
    cmd.type = 1; 
    cmd.code = MTP_OP_GET_OBJECT;
    cmd.transaction = global_tx;

    USB_WriteBlkMsg(usb_device, ep_out, sizeof(MTP_Header), &cmd);
    USB_WriteBlkMsg(usb_device, ep_out, sizeof(u32), &object_id);

    s32 read_bytes = 0;
    u32 total_saved = 0;

    while ((read_bytes = USB_ReadBlkMsg(usb_device, ep_in, chunk_size, io_buffer)) > 0) {
        u32 write_offset = 0;
        if (total_saved == 0 && read_bytes >= 12) {
            write_offset = 12; 
            read_bytes -= 12;
        }
        if (read_bytes > 0) {
            fwrite(io_buffer + write_offset, 1, read_bytes, target_file);
            total_saved += read_bytes;
        }
    }

    free(io_buffer);
    fclose(target_file);
    return true;
}

int main(int argc, char **argv) {
    void *frame_buffer;
    VIDEO_Init();
    
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, 640, 480);

    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    frame_buffer = SYS_AllocateFramebuffer(rmode);
    
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(frame_buffer);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    
    console_init(frame_buffer, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);

    if (!fatInitDefault()) return 0;

    PrintUpdate("Wii MTP Browser Ready. Looking for phone...");
    if (!InitUSBAndFindAndroid()) {
        PrintUpdate("Device connection absent or non-responsive.");
        return 0;
    }

    global_tx++;
    MTP_Header open_cmd = { sizeof(MTP_Header), 1, MTP_OP_OPEN_SESSION, global_tx };
    if (USB_WriteBlkMsg(usb_device, ep_out, sizeof(MTP_Header), &open_cmd) >= 0) {
        u32 param = 1; 
        USB_WriteBlkMsg(usb_device, ep_out, sizeof(u32), &param);
        
        u8 dummy_resp[32]; // Fix: Explicit array buffer definition targets
        USB_ReadBlkMsg(usb_device, ep_in, 32, dummy_resp); 
        BrowseAndroidFiles();
    }

    PrintUpdate("\nPoint Wiimote at screen. Press A to sync an item. HOME to Exit.\n");

    int active_selection = 0;

    while (1) {
        WPAD_ScanPads();
        u32 down = WPAD_ButtonsDown(0);
        
        ir_t ir_pointer;
        WPAD_IR(WPAD_CHAN_0, &ir_pointer);

        if (down & WPAD_BUTTON_HOME) break;

        if (total_files_found > 0) {
            printf("\x1b[15;0H"); 
            for (u32 i = 0; i < total_files_found; i++) {
                if ((int)i == active_selection) {
                    printf(" -> [%s] <-\n", discovered_files[i].filename);
                } else {
                    printf("    [%s]\n", discovered_files[i].filename);
                }
            }
        }

        if (ir_pointer.valid) {
            int calculated_y = (ir_pointer.y - 120) / 24; 
            if (calculated_y >= 0 && calculated_y < (int)total_files_found) {
                active_selection = calculated_y;
            }
            printf("\x1b[28;0HCursor Position Vector: X:%3d Y:%3d", (int)ir_pointer.x, (int)ir_pointer.y);
        }

        if ((down & WPAD_BUTTON_A) && total_files_found > 0) {
            printf("\nSyncing: %s...\n", discovered_files[active_selection].filename);
            
            if (SyncSelectedFile(discovered_files[active_selection].handle, discovered_files[active_selection].filename)) {
                printf("Success! Saved onto SD card.\n");
            } else {
                printf("Error writing targeted elements to media storage.\n");
            }
        }

        VIDEO_WaitVSync();
    }

    if (usb_device != -1) USB_CloseDevice(&usb_device);
    return 0;
}
