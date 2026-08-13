ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to devkitPPC>")
endif

include $(DEVKITPPC)/wii_rules

TARGET   := wii_mtp_client
SOURCES  := source
INCLUDES := 

CFLAGS   := -g -O2 -mrvl -Wall $(MACHDEP) $(INCLUDE)
LDFLAGS  := -g $(MACHDEP) -mrvl

# Linking against standard Wii subsystem modules
LIBS     := -lfat -lwiiuse -lbte -logc -lm

CFILES   := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c))
OBJS     := $(CFILES:.c=.o)

all: $(TARGET).dol

$(TARGET).dol: $(TARGET).elf
$(TARGET).elf: $(OBJS)

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).dol
