/*
 * EFR32xG22: FrSky's bootloader occupies 0x00000000-0x00007FFF and starts
 * the application at 0x00008000 (vector table first). Flash pages are 8 KB:
 * keeping the image within the first one leaves the rest of the installed
 * application in place, and in the dump.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00008000, LENGTH = 8K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .data :
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    _sidata = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.comment)
    }
}
