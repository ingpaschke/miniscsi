# miniscsi — build driver.bin from the C body + asm shim.
#
# Fully self-contained and position-independent: head.s + body.c link with no
# library (head.s provides memset), no Retro68 flat machinery, no relocation
# pass.  Set RETRO68 to your toolchain prefix if it isn't /opt/retro68.

RETRO68 ?= /opt/retro68
PFX      = $(RETRO68)/bin/m68k-apple-macos
AS       = $(PFX)-as
GCC      = $(PFX)-gcc
LD       = $(PFX)-ld
OBJCOPY  = $(PFX)-objcopy

CFLAGS   = -m68000 -mpcrel -fno-zero-initialized-in-bss -Os \
           -ffunction-sections -fdata-sections
LDFLAGS  = -T boot.ld --gc-sections -e _start --no-warn-rwx-segments

BUILD    = build

all: driver.bin

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/head.o: head.s | $(BUILD)
	$(AS) -m68000 $< -o $@

$(BUILD)/body.o: body.c | $(BUILD)
	$(GCC) $(CFLAGS) -c $< -o $@

driver.bin: $(BUILD)/head.o $(BUILD)/body.o boot.ld
	$(LD) $(LDFLAGS) $(BUILD)/head.o $(BUILD)/body.o -o $(BUILD)/driver.elf
	$(OBJCOPY) -O binary $(BUILD)/driver.elf $@
	@echo "driver.bin: $$(stat -c%s $@) bytes"

clean:
	rm -rf $(BUILD) driver.bin

.PHONY: all clean
