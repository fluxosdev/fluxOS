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
        *(.gcc_except_table .gcc_except_table.*)
    }

    /* C++ exceptions (and GCC's crtbegin.o registers these frames) */
    .eh_frame : { KEEP(*(.eh_frame)) }

    . = ALIGN(4096);

    /* the desktop's script engine scans globals for pointers (jsgc.c) */
    __gc_data_start = .;

    /* constructors and destructors, run by the C library's startup code */
    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    }
    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    }
    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }

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
        *(.eh_frame_hdr)
    }
}
