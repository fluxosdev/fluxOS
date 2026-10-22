/*
 * Multiboot (v1) entry point. GRUB loads us at 1 MiB in 32-bit protected
 * mode with paging off, eax = magic and ebx = multiboot info pointer.
 */
.set MB_MAGIC,    0x1BADB002
.set MB_ALIGN,    1 << 0            /* page-align modules */
.set MB_MEMINFO,  1 << 1            /* provide a memory map */
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

.section .bss
.align 16
.global boot_stack_bottom
boot_stack_bottom:
.skip 16384
.global boot_stack_top
boot_stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    cld
    movl $boot_stack_top, %esp
    xorl %ebp, %ebp
    pushl $0                        /* keep 16-byte alignment */
    pushl $0
    pushl %ebx                      /* struct multiboot_info * */
    pushl %eax                      /* magic */
    call kmain
1:  cli
    hlt
    jmp 1b
.size _start, . - _start

.section .note.GNU-stack, "", @progbits
