/*
 * Where another processor starts. smp.c copies this below 1 MiB, to
 * TRAMP_BASE, and points the startup IPI at it, so the processor begins
 * here in real mode at TRAMP_BASE:0 (CS = TRAMP_BASE >> 4). It turns on
 * its caches (INIT may leave CD and NW set), goes through 32-bit protected
 * mode on a flat GDT of its own into 64-bit long mode, and calls ap_main()
 * on the stack smp.c chose.
 *
 * Long mode needs paging: the page tables are vmm_boot_cr3()'s, the
 * kernel's with the first 2 MiB mapped one to one as well, where this
 * code is. smp.c writes them, the stack, ap_main's address and the boot
 * processor's EFER bits into the copy (the words at the end).
 */
#define TRAMP_BASE 0x8000
#define T(x) ((x) - ap_trampoline + TRAMP_BASE)

#define CR4_PAE  0x20
#define MSR_EFER 0xC0000080
#define CR0_PG_WP 0x80010000

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
    movl %cr4, %eax
    orl $CR4_PAE, %eax
    movl %eax, %cr4
    movl T(tramp_cr3), %eax
    movl %eax, %cr3
    movl $MSR_EFER, %ecx
    rdmsr
    orl T(tramp_efer), %eax         /* long mode (and what else the boot processor has on) */
    wrmsr
    movl %cr0, %eax
    orl $CR0_PG_WP, %eax            /* paging on: long mode is active */
    movl %eax, %cr0
    ljmpl $0x18, $T(tramp64)

.code64
tramp64:
    xorl %eax, %eax
    movw %ax, %fs
    movw %ax, %gs
    movq T(tramp_stack), %rsp
    xorl %ebp, %ebp
    movq T(tramp_entry), %rax
    call *%rax
1:  cli
    hlt
    jmp 1b

.align 8
tramp_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF        /* 0x08 code: flat, ring 0, 32-bit */
    .quad 0x00CF92000000FFFF        /* 0x10 data: flat, ring 0 */
    .quad 0x00AF9A000000FFFF        /* 0x18 code: ring 0, 64-bit */
tramp_gdt_desc:
    .word 4 * 8 - 1
    .long T(tramp_gdt)

.align 8
.global tramp_cr3, tramp_efer, tramp_stack, tramp_entry
tramp_cr3:
    .long 0
tramp_efer:
    .long 0
tramp_stack:
    .quad 0
tramp_entry:
    .quad 0

.global ap_trampoline_end
ap_trampoline_end:

.section .note.GNU-stack, "", @progbits
