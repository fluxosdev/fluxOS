/* A test program's ELF image, laid out as a PE at 0x400000: objcopy -O pei-i386 converts it. */
ENTRY(start)
SECTIONS
{
    . = 0x401000;
    .text : { *(.text*) }
    . = ALIGN(0x1000);
    .rdata : { *(.rodata*) }
    . = ALIGN(0x1000);
    .data : { *(.data*) *(.bss*) *(COMMON) }
    . = ALIGN(0x1000);
    .idata : { *(.idata) }
    . = ALIGN(0x1000);
    .rsrc : { *(.rsrc) }
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
