ENTRY(Reset_Handler)
MEMORY
{
    /* Verified from installed S140 metadata + CURRENT.UF2; see docs/layout.md. */
    FLASH (rx)  : ORIGIN = 0x00027000, LENGTH = 0x000C3000
    /* Preserve low SRAM incl. MBR bookkeeping and bootloader double-reset word. */
    RAM (rwx)   : ORIGIN = 0x20008000, LENGTH = 0x00038000
}
__stack_top = ORIGIN(RAM) + LENGTH(RAM);
__stack_limit = __stack_top - 8192;
SECTIONS
{
    .vectors : { KEEP(*(.vectors)) } > FLASH
    .text : { *(.text*) *(.rodata*) KEEP(*(.experiment_identity)) . = ALIGN(4); } > FLASH
    .ARM.extab : { *(.ARM.extab*) } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH
    .data : ALIGN(4)
    {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);
    __flash_end = LOADADDR(.data) + SIZEOF(.data);
    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM
    /DISCARD/ : { *(.comment) *(.note*) }
}
ASSERT(ADDR(.vectors) == 0x27000, "Wrong application vector address")
ASSERT(SIZEOF(.vectors) == 256, "Wrong vector table size")
ASSERT(__flash_end <= 0xEA000, "Image overlaps reserved application data")
ASSERT(__bss_end <= __stack_limit, "RAM overlaps reserved 8 KiB stack")
ASSERT((__stack_top & 7) == 0, "Stack must be 8-byte aligned")
