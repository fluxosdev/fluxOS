/*
 * Low-level CPU entry and exit paths: interrupt stubs, the common trap
 * frame builder, trap return, context switching and descriptor loading.
 * 64-bit code; what it interrupts is the kernel itself or a 32-bit program
 * in compatibility mode, and the processor pushes the same frame for both.
 */
.section .text

/* Exceptions that do not push an error code get a dummy 0 so every
 * trap frame has the same layout (see struct trapframe). */
.macro ISR_NOERR num
.global isr\num
isr\num:
    pushq $0
    pushq $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    pushq $\num
    jmp isr_common
.endm

.irp num, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
ISR_NOERR \num
.endr

.irp num, 8,10,11,12,13,14,17,21,29,30
ISR_ERR \num
.endr

/* Hardware IRQs 0-15, remapped to vectors 32-47. */
.irp num, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
ISR_NOERR \num
.endr

/* System call gate. */
ISR_NOERR 128

/* Interrupts between processors, and the local APIC's spurious one (smp.h). */
.irp num, 240,241,242,243,244,255
ISR_NOERR \num
.endr

/*
 * The processor has pushed SS, RSP, RFLAGS, CS and RIP (and the stub the
 * error code and vector) onto a stack it aligned to 16 bytes first; the
 * rest of struct trapframe goes below, which leaves the stack aligned for
 * the call. The segment registers are kept as the program had them: the
 * kernel needs none of them in 64-bit mode, and each process's are loaded
 * again on its own way out.
 */
isr_common:
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $32, %rsp
    movl %ds, %eax
    movq %rax, 24(%rsp)
    movl %es, %eax
    movq %rax, 16(%rsp)
    movl %fs, %eax
    movq %rax, 8(%rsp)
    movl %gs, %eax
    movq %rax, 0(%rsp)
    cld
    movq %rsp, %rdi                 /* struct trapframe * */
    call trap

/* Also the first "return address" of every new process: the scheduler
 * switches to a fresh kernel stack whose context frame returns here.
 * FS and GS take their bases from this processor's GDT as they are loaded:
 * the thread block of the thread now running here (gdt.c). */
.global trapret
trapret:
    movq 0(%rsp), %rax
    movw %ax, %gs
    movq 8(%rsp), %rax
    movw %ax, %fs
    movq 16(%rsp), %rax
    movw %ax, %es
    movq 24(%rsp), %rax
    movw %ax, %ds
    addq $32, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    addq $16, %rsp                  /* int_no, err_code */
    iretq

/* Where a new process first runs: off the scheduler's lock, which the
 * switch to it left held (proc_start), then out through trapret like
 * any trap. */
.global forkret
forkret:
    call proc_start
    jmp trapret

/*
 * int uaccess_copy(void *dst, const void *src, uint32_t n): a copy where
 * either side may be a process's memory that turns out not to be there.
 * One instruction can fault; the page fault handler (idt.c) knows its
 * address, uaccess_copy_ip, and resumes at uaccess_fixup instead, which
 * returns -EFAULT with the stack as the fault left it (the string
 * instruction pushes nothing). Returns 0 when the whole copy went.
 */
.global uaccess_copy
.global uaccess_copy_ip
.global uaccess_fixup
uaccess_copy:
    movl %edx, %ecx                 /* (and RCX's high half zero) */
    cld
uaccess_copy_ip:
    rep movsb
    xorl %eax, %eax
    ret
uaccess_fixup:
    movl $-14, %eax                 /* -EFAULT */
    ret

/*
 * void switch_context(uint64_t *old_sp, uint64_t new_sp)
 * Saves callee-saved registers on the current kernel stack, stores the
 * stack pointer in *old_sp and resumes the context saved at new_sp.
 */
.global switch_context
switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

/* void gdt_flush(struct gdt_ptr *): the table, and the kernel's segments
 * from it (CS by a far return). */
.global gdt_flush
gdt_flush:
    lgdt (%rdi)
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorl %eax, %eax
    movw %ax, %fs
    movw %ax, %gs
    popq %rax
    pushq $0x08
    pushq %rax
    lretq

/* void tss_flush(uint32_t selector) */
.global tss_flush
tss_flush:
    ltr %di
    ret

/*
 * A processor's state across a power cut (suspend to RAM): what it needs
 * to go on where it left off, as setjmp() keeps it, plus the control and
 * descriptor table registers. Offsets as in struct cpu_context (arch.h).
 */
#define C_RBX    0
#define C_RBP    8
#define C_R12    16
#define C_R13    24
#define C_R14    32
#define C_R15    40
#define C_RSP    48
#define C_RIP    56
#define C_CR0    64
#define C_CR3    72
#define C_CR4    80
#define C_RFLAGS 88
#define C_GDT    96
#define C_IDT    112

/* int context_save(struct cpu_context *): 0, and 1 when context_restore()
 * comes back here. */
.global context_save
context_save:
    movq %rbx, C_RBX(%rdi)
    movq %rbp, C_RBP(%rdi)
    movq %r12, C_R12(%rdi)
    movq %r13, C_R13(%rdi)
    movq %r14, C_R14(%rdi)
    movq %r15, C_R15(%rdi)
    leaq 8(%rsp), %rax              /* the stack once this returns */
    movq %rax, C_RSP(%rdi)
    movq (%rsp), %rax
    movq %rax, C_RIP(%rdi)
    movq %cr0, %rax
    movq %rax, C_CR0(%rdi)
    movq %cr3, %rax
    movq %rax, C_CR3(%rdi)
    movq %cr4, %rax
    movq %rax, C_CR4(%rdi)
    pushfq
    popq C_RFLAGS(%rdi)
    sgdt C_GDT(%rdi)
    sidt C_IDT(%rdi)
    xorl %eax, %eax
    ret

/*
 * void context_restore(struct cpu_context *): back to where context_save()
 * was, from long mode on any page tables that map the kernel (a processor
 * coming back up: trampoline.S, wakeup.S). The stack it is called on may
 * be one only those tables map, so the saved one is taken as soon as the
 * saved tables are loaded. The task register is the caller's to load again.
 */
.global context_restore
context_restore:
    movq %rdi, %rax
    movq C_CR4(%rax), %rcx
    movq %rcx, %cr4
    movq C_CR3(%rax), %rcx
    movq %rcx, %cr3
    movq C_RSP(%rax), %rsp
    movq C_CR0(%rax), %rcx
    movq %rcx, %cr0
    lgdt C_GDT(%rax)
    lidt C_IDT(%rax)
    movw $0x10, %cx
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %ss
    xorl %ecx, %ecx
    movw %cx, %fs
    movw %cx, %gs
    pushq $0x08
    leaq 1f(%rip), %rcx
    pushq %rcx
    lretq
1:  movq C_RBX(%rax), %rbx
    movq C_RBP(%rax), %rbp
    movq C_R12(%rax), %r12
    movq C_R13(%rax), %r13
    movq C_R14(%rax), %r14
    movq C_R15(%rax), %r15
    pushq C_RFLAGS(%rax)
    popfq
    movq C_RIP(%rax), %rcx
    movl $1, %eax
    jmp *%rcx

/* void idt_load(struct idt_ptr *) */
.global idt_load
idt_load:
    lidt (%rdi)
    ret

/* Table of the 48 exception/IRQ stubs, indexed by vector. */
.section .rodata
.align 8
.global isr_stub_table
isr_stub_table:
.irp num, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
.quad isr\num
.endr
.irp num, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
.quad isr\num
.endr

.section .note.GNU-stack, "", @progbits
