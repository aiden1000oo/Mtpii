# Ensure devkitPPC path is verified
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to devkitPPC>")
endif

# Map out standard Wii hardware rules from devkitPro configuration bases
include $(DEVKITPPC)/wii_rules

TARGET   := wii_mtp_client
SOURCES  := source
INCLUDES := 

# Track and link include directory structures
INCLUDE  := -I$(DEVKITPRO)/libogc/include $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir))

# Compilation architecture flags targeting the PowerPC chip
CFLAGS   := -g -O2 -mrvl -Wall $(MACHDEP) $(INCLUDE)
LDFLAGS  := -g $(MACHDEP) -mrvl -L$(DEVKITPRO)/libogc/lib/wii

# Link explicit system subsystem dependencies modules sequentially 
LIBS     := -lfat -lwiiuse -lbte -logc -lm

CFILES   := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c))
OBJS     := $(CFILES:.c=.o)

all: $(TARGET).dol

$(TARGET).dol: $(TARGET).elf
	$(ELF2DOL) $< $@

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).dol
