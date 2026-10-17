# Needs only arm-none-eabi-gcc/binutils and python3.
#
#   make                         -> flashdump.frk (dumps the 8 KB bootloader)
#   make DUMP_START=0x08002000 DUMP_LENGTH=0xE000
#                                -> dumps something else

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size
PYTHON  ?= python3

APP_ADDR    := 0x08002000
DUMP_START  ?= 0x08000000
DUMP_LENGTH ?= 0x2000

CFLAGS := -mcpu=cortex-m3 -mthumb -Os -std=c99 -Wall -Wextra \
          -ffreestanding -fno-tree-loop-distribute-patterns \
          -ffunction-sections -fdata-sections \
          -DDUMP_START=$(DUMP_START)u -DDUMP_LENGTH=$(DUMP_LENGTH)u
LDFLAGS := -nostdlib -nostartfiles -T flashdump.ld -Wl,--gc-sections

all: flashdump.frk

flashdump.elf: main.c flashdump.ld Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main.c
	$(SIZE) $@

flashdump.bin: flashdump.elf
	$(OBJCOPY) -O binary $< $@

flashdump.frk: flashdump.bin ../packer.py
	$(PYTHON) ../packer.py $(APP_ADDR) $< $@

clean:
	rm -f flashdump.elf flashdump.bin flashdump.frk

.PHONY: all clean
