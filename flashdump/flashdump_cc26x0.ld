/*
 * CC26x0 (CC2650F128): FrSky's bootloader occupies 0x00000000-0x00007FFF and
 * starts the application at 0x00008000 (vector table first). The last flash
 * page holds the CCFG, which this image must never reach.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00008000, LENGTH = 92K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
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
