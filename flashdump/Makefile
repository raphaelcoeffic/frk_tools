# Needs only arm-none-eabi-gcc/binutils and python3.
#
#   make                         -> flashdump.frk (STM32F1: dumps the 8 KB bootloader)
#   make DUMP_START=0x08002000 DUMP_LENGTH=0xE000
#                                -> dumps something else
#   make TARGET=cc26x0           -> flashdump_cc26x0.frk (CC2650: dumps the whole
#                                   128 KB flash, bootloader and CCFG included)
#   make TARGET=cc26x0 TX=25
#                                -> same, for an RX6R
#   make TARGET=efr32xg22        -> flashdump_efr32xg22.frk (EFR32xG22, e.g. SR10+:
#                                   dumps the whole flash, size from DEVINFO)

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size
PYTHON  ?= python3

TARGET ?= stm32f1
CPU    := cortex-m3

ifeq ($(TARGET),stm32f1)
NAME        := flashdump
SRC         := main.c
APP_ADDR    := 0x08002000
DUMP_START  ?= 0x08000000
DUMP_LENGTH ?= 0x2000
else ifeq ($(TARGET),cc26x0)
NAME        := flashdump_cc26x0
SRC         := main_cc26x0.c
APP_ADDR    := 0x00008000
DUMP_START  ?= 0x00000000
DUMP_LENGTH ?= 0x20000
# S.Port pins (-1: none): XJT-Lite by default, TX=25 for an RX6R
TX          ?= 29
TX2         ?= -1
TX_ENABLE   ?= 24
CFLAGS_TARGET := -DDIO_TX=$(TX) -DDIO_TX2=$(TX2) -DDIO_TX_ENABLE=$(TX_ENABLE)
else ifeq ($(TARGET),efr32xg22)
NAME        := flashdump_efr32xg22
SRC         := main_efr32xg22.c
CPU         := cortex-m33
APP_ADDR    := 0x00008000
DUMP_START  ?= 0x00000000
# 0: the whole flash, size from DEVINFO
DUMP_LENGTH ?= 0
# S.Port pins as port * 16 + pin (-1: none): SR10+ by default
TX          ?= 0x10
TX_ENABLE   ?= 0x14
CFLAGS_TARGET := -DPIN_TX=$(TX) -DPIN_TX_ENABLE=$(TX_ENABLE)
else
$(error TARGET must be stm32f1, cc26x0 or efr32xg22)
endif

CFLAGS := -mcpu=$(CPU) -mthumb -Os -std=c99 -Wall -Wextra \
          -ffreestanding -fno-tree-loop-distribute-patterns \
          -ffunction-sections -fdata-sections \
          -fno-delete-null-pointer-checks \
          -DDUMP_START=$(DUMP_START)u -DDUMP_LENGTH=$(DUMP_LENGTH)u \
          $(CFLAGS_TARGET)
LDFLAGS := -nostdlib -nostartfiles -T $(NAME).ld -Wl,--gc-sections

all: $(NAME).frk

$(NAME).elf: $(SRC) $(NAME).ld Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC)
	$(SIZE) $@

$(NAME).bin: $(NAME).elf
	$(OBJCOPY) -O binary $< $@

$(NAME).frk: $(NAME).bin ../packer.py
	$(PYTHON) ../packer.py $(APP_ADDR) $< $@

clean:
	rm -f $(NAME).elf $(NAME).bin $(NAME).frk

.PHONY: all clean
