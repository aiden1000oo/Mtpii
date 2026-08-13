#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>

// Extended MTP Operation Codes
#define MTP_OP_OPEN_SESSION         0x1002
#define MTP_OP_GET_OBJECT_HANDLES   0x1007
#define MTP_OP_GET_OBJECT_INFO      0x1008
#define MTP_OP_GET_OBJECT           0x1009

// Alignment matching Wii hardware architecture constraints
typedef struct {
    u32 length;
    u16 type; 
    u16 code;
    u32 transaction;
} __attribute__((packed)) MTP_Header;

// MTP Object Info Structure (Stripped to relevant metadata fields)
typedef struct {
    u32 storage_id;
    u16 format_code;
    u16 protection;
    u64 file_size;
    // Followed by variable length strings for filename
} __attribute__((packed)) MTP_ObjectInfo;

static s32 usb_device = -1;
static u8 ep_in = 0x81;
static u8 ep_out = 0x02;
static u32 global_tx = 1;

// Global container to track files discovered on the Android phone
static u32 phone_file_handles[256];
static u32 total_files_found = 0;

void PrintUpdate(const char *msg) {
    printf("%s\n", msg);
    VIDEO_WaitVSync();
}

bool InitUSBAndFindAndroid() {
    u8 dev_count = 0;
    usb_device_description dev_list;
    USB_Initialize();

    s32 ret = USB_GetDeviceList(dev_list, 8, USB_CLASS_PER_INTERFACE, &dev_count);
    if (ret < 0 || dev_count == 0) return false;

    for (int i = 0; i < dev_count; i++) {
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

// Scans the phone filesystem and loads unique ID handles into an array
void BrowseAndroidFiles() {
    u8 *raw_buffer = iosAllocAligned(0, 4096, 32);
    if (!raw_buffer) return;

    PrintUpdate("Querying file structures from Android storage...");
    
    // Command 0x1007 requests an array of all storage elements
    s32 read_bytes = TransactionMTP(MTP_OP_GET_OBJECT_HANDLES, raw_buffer, 4096, false);
    
    if (read_bytes > 12) {
        // Strip the 12-byte MTP container header to reach the array data
        u32 *elements = (u32*)(raw_buffer + 12);
        u32 array_len = elements[0]; // First 4 bytes define total items
        
        total_files_found = (array_len > 15) ? 15 : array_len; // Limit to 15 items for display
        
        for(u32 i = 0; i < total_files_found; i++) {
            phone_file_handles[i] = elements[i + 1];
        }
        printf("Discovered %u items inside root partition.\n", array_len);
    } else {
        PrintUpdate("Failed parsing objects or partition is empty.");
    }
    
    iosFree(0, raw_buffer);
}

// Downloads selected elements directly to the SD card root mount path
bool SyncSelectedFile(u32 object_id, const char* out_filename) {
    char target_path[128];
    snprintf(target_path, sizeof(target_path), "sd:/%s", out_filename);
    
    FILE *target_file = fopen(target_path, "wb");
    if (!target_file) return false;

    u32 chunk_size = 64 * 1024;
    u8 *io_buffer = iosAllocAligned(0, chunk_size, 32);
    
    // Request raw data stream for chosen file ID
    TransactionMTP(MTP_OP_GET_OBJECT, NULL, 0, false); 

    s32 read_bytes = 0;
    u32 total_saved = 0;

    while ((read_bytes = USB_ReadBlkMsg(usb_device, ep_in, chunk_size, io_buffer)) > 0) {
        u32 write_offset = 0;
        if (total_saved == 0 && read_bytes >= 12) {
            write_offset = 12; // Drop initial MTP sequence wrapper
            read_bytes -= 12;
        }
        if (read_bytes > 0) {
            fwrite(io_buffer + write_offset, 1, read_bytes, target_file);
            total_saved += read_bytes;
        }
    }

    iosFree(0, io_buffer);
    fclose(target_file);
    return true;
}

int main(int argc, char **argv) {
    // Basic video system configuration
    void *frame_buffer;
    GXM_Init(); 
    VIDEO_Init();
    
    // Configure Wiimote pointer tracking
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, 640, 480);

    frame_buffer = SYS_AllocateFramebuffer(VIDEO_GetPreferredMode(NULL));
    VIDEO_Configure(VIDEO_GetPreferredMode(NULL));
    VIDEO_SetNextFramebuffer(frame_buffer);
    VIDEO_SetBlack(false);
    VIDEO_Flush();

    if (!fatInitDefault()) return 0;

    PrintUpdate("Wii MTP Browser Ready. Looking for phone...");
    if (!InitUSBAndFindAndroid()) {
        PrintUpdate("Device connection absent or non-responsive.");
        return 0;
    }

    if (TransactionMTP(MTP_OP_OPEN_SESSION, NULL, 0, false) >= 0) {
        BrowseAndroidFiles();
    }

    PrintUpdate("\nPoint Wiimote at screen. Press A to sync an item. HOME to Exit.\n");

    int active_selection = 0;

    // Core Interaction Engine Loop
    while (1) {
        WPAD_ScanPads();
        u32 down = WPAD_ButtonsDown(0);
        
        // Read Infrared bounding coords mapping directly from sensor bar
        ir_t ir_pointer;
        WPAD_IR(WPAD_CHAN_0, &ir_pointer);

        if (down & WPAD_BUTTON_HOME) break;

        // Render simple user menu interface on the screen console grid
        if (total_files_found > 0) {
            printf("\x1b[10;0H"); // Lock terminal cursor position
            for (u32 i = 0; i < total_files_found; i++) {
                if ((int)i == active_selection) {
                    printf(" -> [MTP File Object ID: 0x%08X] <-\n", phone_file_handles[i]);
                } else {
                    printf("    [MTP File Object ID: 0x%08X]\n", phone_file_handles[i]);
                }
            }
        }

        // Translate Wiimote structural positioning across menu options
        if (ir_pointer.valid) {
            int calculated_y = ir_pointer.y / 32; // Scale height mapping matrix
            if (calculated_y >= 0 && calculated_y < (int)total_files_found) {
                active_selection = calculated_y;
            }
            // Optional visual feedback cursor token tracking positioning markers
            printf("\x1b[25;0HCursor Position Vector: X:%3d Y:%3d", (int)ir_pointer.x, (int)ir_pointer.y);
        }

        // Execute download operation block if user confirms option selection
        if (down & WPAD_BUTTON_A && total_files_found > 0) {
            printf("\nInitializing background sync loop for target ID...\n");
            char out_name[32];
            snprintf(out_name, sizeof(out_name), "mtp_sync_%04x.bin", active_selection);
            
            if (SyncSelectedFile(phone_file_handles[active_selection], out_name)) {
                printf("Success! Saved file onto SD root partition path.\n");
            } else {
                printf("Error writing to storage card media array targets.\n");
            }
        }

        VIDEO_WaitVSync();
    }

    if (usb_device != -1) USB_CloseDevice(&usb_device);
    return 0;
}
