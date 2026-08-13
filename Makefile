# Ensure devkitPPC path is verified
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to devkitPPC>")
endif

# Map out standard Wii hardware rules from devkitPro configuration bases
include $(DEVKITPPC)/wii_rules

TARGET   := wii_mtp_client
SOURCES  := source
INCLUDES := 

# Fix: Dynamically track and link correct include directory structures
INCLUDE  := -I$(DEVKITPRO)/libogc/include $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir))

# Compilation architectures flags configurations targeting the PowerPC chip
CFLAGS   := -g -O2 -mrvl -Wall $(MACHDEP) $(INCLUDE)
LDFLAGS  := -g $(MACHDEP) -mrvl -L$(DEVKITPRO)/libogc/lib/wii

# Link explicit system subsystems dependencies modules sequentially 
LIBS     := -lfat -lwiiuse -lbte -logc -lm

CFILES   := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c))
OBJS     := $(CFILES:.c=.o)

all: $(TARGET).dol

$(TARGET).dol: $(TARGET).elf
$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@
	$(ELF2DOL) $@ $(TARGET).dol

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).dol
