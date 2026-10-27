/*
 * Where another processor starts. smp.c copies this below 1 MiB, to
 * TRAMP_BASE, and points the startup IPI at it, so the processor begins
 * here in real mode at TRAMP_BASE:0 (CS = TRAMP_BASE >> 4). It turns on
 * its caches (INIT may leave CD and NW set) and 32-bit protected mode on
 * a flat GDT of its own, then calls ap_main() on the stack smp.c left in
 * ap_stack_top. Paging is still off; ap_main() turns it on.
 */
#define TRAMP_BASE 0x8000
#define T(x) ((x) - ap_trampoline + TRAMP_BASE)

.section .text
.align 16
.code16
.global ap_trampoline
ap_trampoline:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    lgdtl T(tramp_gdt_desc)
    movl %cr0, %eax
    andl $0x9FFFFFFF, %eax          /* caches on: clear CD and NW */
    orl $1, %eax                    /* PE */
    movl %eax, %cr0
    ljmpl $0x08, $T(tramp32)

.code32
tramp32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl ap_stack_top, %esp
    xorl %ebp, %ebp
    movl $ap_main, %eax
    call *%eax
1:  cli
    hlt
    jmp 1b

.align 8
tramp_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF        /* code: flat, ring 0 */
    .quad 0x00CF92000000FFFF        /* data: flat, ring 0 */
tramp_gdt_desc:
    .word 3 * 8 - 1
    .long T(tramp_gdt)

.global ap_trampoline_end
ap_trampoline_end:

.section .note.GNU-stack, "", @progbits
