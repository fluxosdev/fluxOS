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

/* void tss_flush(void) */
.global tss_flush
tss_flush:
    movw $0x28, %ax
    ltr %ax
    ret

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
