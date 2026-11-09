/*
 * Low-level CPU entry and exit paths: interrupt stubs, the common trap
 * frame builder, trap return, context switching and descriptor loading.
 */
.section .text

/* Exceptions that do not push an error code get a dummy 0 so every
 * trap frame has the same layout (see struct trapframe). */
.macro ISR_NOERR num
.global isr\num
isr\num:
    pushl $0
    pushl $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    pushl $\num
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

isr_common:
    pushal
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    cld
    pushl %esp                      /* struct trapframe * */
    call trap
    addl $4, %esp

/* Also the first "return address" of every new process: the scheduler
 * switches to a fresh kernel stack whose context frame returns here. */
.global trapret
trapret:
    popl %gs
    popl %fs
    popl %es
    popl %ds
    popal
    addl $8, %esp                   /* int_no, err_code */
    iret

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
    pushl %esi
    pushl %edi
    movl 12(%esp), %edi
    movl 16(%esp), %esi
    movl 20(%esp), %ecx
    cld
uaccess_copy_ip:
    rep movsb
    xorl %eax, %eax
1:  popl %edi
    popl %esi
    ret
uaccess_fixup:
    movl $-14, %eax                 /* -EFAULT */
    jmp 1b

/*
 * void switch_context(uint32_t *old_esp, uint32_t new_esp)
 * Saves callee-saved registers on the current kernel stack, stores the
 * stack pointer in *old_esp and resumes the context saved at new_esp.
 */
.global switch_context
switch_context:
    movl 4(%esp), %eax
    movl 8(%esp), %edx
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, (%eax)
    movl %edx, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

/* void gdt_flush(struct gdt_ptr *) */
.global gdt_flush
gdt_flush:
    movl 4(%esp), %eax
    lgdt (%eax)
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    ljmp $0x08, $1f
1:  ret

/* void tss_flush(uint32_t selector) */
.global tss_flush
tss_flush:
    movl 4(%esp), %eax
    ltr %ax
    ret

/*
 * A processor's state across a power cut (suspend to RAM): what it needs
 * to go on where it left off, as setjmp() keeps it, plus the control and
 * descriptor table registers. Offsets as in struct cpu_context (arch.h).
 */
#define C_EBX    0
#define C_ESI    4
#define C_EDI    8
#define C_EBP    12
#define C_ESP    16
#define C_EIP    20
#define C_CR0    24
#define C_CR3    28
#define C_CR4    32
#define C_EFLAGS 36
#define C_GDT    40
#define C_IDT    48

/* int context_save(struct cpu_context *): 0, and 1 when context_restore()
 * comes back here. */
.global context_save
context_save:
    movl 4(%esp), %eax
    movl %ebx, C_EBX(%eax)
    movl %esi, C_ESI(%eax)
    movl %edi, C_EDI(%eax)
    movl %ebp, C_EBP(%eax)
    leal 4(%esp), %ecx              /* the stack once this returns */
    movl %ecx, C_ESP(%eax)
    movl (%esp), %ecx
    movl %ecx, C_EIP(%eax)
    movl %cr0, %ecx
    movl %ecx, C_CR0(%eax)
    movl %cr3, %ecx
    movl %ecx, C_CR3(%eax)
    movl %cr4, %ecx
    movl %ecx, C_CR4(%eax)
    pushfl
    popl C_EFLAGS(%eax)
    sgdt C_GDT(%eax)
    sidt C_IDT(%eax)
    xorl %eax, %eax
    ret

/*
 * void context_restore(struct cpu_context *): back to where context_save()
 * was, from 32-bit protected mode with paging on or off (the kernel's
 * addresses are its physical ones). The task register is the caller's
 * to load again.
 */
.global context_restore
context_restore:
    movl 4(%esp), %eax
    movl C_CR4(%eax), %ecx
    movl %ecx, %cr4
    movl C_CR3(%eax), %ecx
    movl %ecx, %cr3
    movl C_CR0(%eax), %ecx
    movl %ecx, %cr0
    jmp 1f
1:  lgdt C_GDT(%eax)
    lidt C_IDT(%eax)
    ljmp $0x08, $2f
2:  movw $0x10, %cx
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %fs
    movw %cx, %gs
    movw %cx, %ss
    movl C_ESP(%eax), %esp
    movl C_EBX(%eax), %ebx
    movl C_ESI(%eax), %esi
    movl C_EDI(%eax), %edi
    movl C_EBP(%eax), %ebp
    pushl C_EFLAGS(%eax)
    popfl
    movl C_EIP(%eax), %ecx
    movl $1, %eax
    jmp *%ecx

/* void idt_load(struct idt_ptr *) */
.global idt_load
idt_load:
    movl 4(%esp), %eax
    lidt (%eax)
    ret

/* Table of the 48 exception/IRQ stubs, indexed by vector. */
.section .rodata
.global isr_stub_table
isr_stub_table:
.irp num, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
.long isr\num
.endr
.irp num, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
.long isr\num
.endr

.section .note.GNU-stack, "", @progbits
