/* fluxOS user programs: static ELF32 executables at 1 GiB. */
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text :
    {
        *(.text.start)
        *(.text .text.*)
    }

    .rodata :
    {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);

    /* the desktop's script engine scans globals for pointers (jsgc.c) */
    __gc_data_start = .;

    .data :
    {
        *(.data .data.*)
    }

    .bss :
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    __gc_data_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
