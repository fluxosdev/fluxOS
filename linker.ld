/* fluxOS kernel link script: a 64-bit kernel in the top 2 GiB of the address
 * space, where the Limine boot protocol wants it (and -mcmodel=kernel). Limine
 * puts it wherever it finds physical memory and maps it here; the kernel then
 * makes its own page tables (vmm.c). */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
    stack  PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    . = 0xFFFFFFFF80000000;
    kernel_start = .;

    .text : ALIGN(4K)
    {
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    } :rodata

    /* What the kernel asks of Limine (bootinfo.c): between the two markers, so
     * that the loader need not search the whole image for them. */
    .data : ALIGN(4K)
    {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
        *(.data .data.*)
    } :data

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    . = ALIGN(4K);
    kernel_end = .;

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.comment)
        *(.note.gnu.property)
    }
}
