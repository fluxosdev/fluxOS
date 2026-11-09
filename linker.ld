/* fluxOS kernel link script: loaded by GRUB at the 1 MiB mark. */
ENTRY(_start)

/* Reserve the kernel in one operation. Separate adjacent LOAD segments can
 * leave GRUB relocator free-list headers in the next segment's destination.
 * The kernel establishes its own page permissions after the handoff. */
PHDRS
{
    kernel PT_LOAD FILEHDR PHDRS FLAGS(7);
    stack PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    . = 1M;
    kernel_start = .;
    /* Keep the Multiboot header in the first 8 KiB of the file even when a
     * DMA buffer raises the segment alignment above a page. */
    . += SIZEOF_HEADERS;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text .text.*)
    } :kernel

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    } :kernel

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data .data.*)
    } :kernel

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss .bss.*)
    } :kernel

    . = ALIGN(4K);
    kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
