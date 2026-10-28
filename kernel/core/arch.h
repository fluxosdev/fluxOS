/* Descriptor tables, interrupts and the trap frame. */
#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>

#define SEG_KCODE 0x08
#define SEG_KDATA 0x10
#define SEG_UCODE (0x18 | 3)
#define SEG_UDATA (0x20 | 3)
#define SEG_TSS   0x28

#define IRQ_BASE    32
#define IRQ_TIMER   0
#define IRQ_KBD     1
#define IRQ_COM1    4
#define SYSCALL_VEC 0x80

/* Layout produced by isr_common in entry.S, lowest address first. */
struct trapframe {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    /* pushed by the CPU */
    uint32_t eip, cs, eflags;
    /* only present when the trap came from ring 3 */
    uint32_t useresp, ss;
};

typedef void (*irq_handler_t)(struct trapframe *tf);

void gdt_init(void);
void gdt_init_ap(int cpu);              /* another processor: the same GDT, its own TSS */
void tss_set_kernel_stack(uint32_t esp0);

void idt_init(void);
void idt_init_ap(void);
void irq_register(int irq, irq_handler_t handler);
int irq_registered(int irq);           /* another driver has it */
void irq_unmask(int irq);

/* A processor's state across suspend to RAM (entry.S). context_save()
 * returns 0, then 1 again when context_restore() brings it back. */
struct cpu_context {
    uint32_t ebx, esi, edi, ebp, esp, eip;
    uint32_t cr0, cr3, cr4, eflags;
    uint8_t gdtr[8], idtr[8];           /* 6 bytes each, as SGDT and SIDT store them */
};
_Static_assert(__builtin_offsetof(struct cpu_context, eflags) == 36 &&
               __builtin_offsetof(struct cpu_context, gdtr) == 40 &&
               __builtin_offsetof(struct cpu_context, idtr) == 48, "the offsets entry.S uses");
int context_save(struct cpu_context *c) __attribute__((returns_twice));
void context_restore(struct cpu_context *c) __attribute__((noreturn));

/* Across suspend (idt.c, gdt.c): the 8259s' masks and trigger modes,
 * and the task register, which must be loaded again. */
void pic_suspend(void);
void pic_resume(void);
void gdt_load_tr(int cpu);

void trapret(void);
void forkret(void);                     /* a new process's first way out of the kernel */
void switch_context(uint32_t *old_esp, uint32_t new_esp);

/* A copy that survives a missing user page (entry.S): 0, or -EFAULT. The
 * page fault handler turns a fault at uaccess_copy_ip into uaccess_fixup. */
int uaccess_copy(void *dst, const void *src, uint32_t n);
extern char uaccess_copy_ip[], uaccess_fixup[];

#endif
