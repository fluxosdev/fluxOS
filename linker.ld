/* fluxOS kernel link script: loaded by GRUB at the 1 MiB mark. */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    kernel_start = .;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text .text.*)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data .data.*)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    . = ALIGN(4K);
    kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
