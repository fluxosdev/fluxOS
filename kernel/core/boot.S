/*
 * The kernel's entry point. Limine (the Limine boot protocol) loads the
 * kernel into the top 2 GiB of the address space and starts it here in
 * 64-bit long mode: interrupts off, its own page tables and descriptor
 * tables loaded, on a stack of its own. The kernel takes its own stack at
 * once, and replaces the rest as it comes up (kmain.c).
 */
.section .bss
.align 16
.global boot_stack_bottom
boot_stack_bottom:
.skip 32768
.global boot_stack_top
boot_stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    cld
    movq $boot_stack_top, %rsp
    xorl %ebp, %ebp
    call kmain
1:  cli
    hlt
    jmp 1b
.size _start, . - _start

.section .note.GNU-stack, "", @progbits
