/* Descriptor tables, interrupts and the trap frame. */
#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>
#include <flux/abi.h>

/*
 * The kernel is 64-bit code. So are most programs, in SEG_UCODE64; a
 * 32-bit program runs in the processor's compatibility mode, in the
 * segments such programs always had (the selectors are part of the ABI:
 * <flux/abi.h>).
 */
#define SEG_KCODE 0x08                  /* 64-bit code */
#define SEG_KDATA 0x10
#define SEG_UCODE (0x18 | 3)            /* 32-bit code */
#define SEG_UDATA (0x20 | 3)
#define SEG_UCODE64 SEG_USER_CODE64     /* 64-bit code: a 64-bit program's (<flux/abi.h>) */
#define SEG_WCODE SEG_WIN_CODE          /* Win32 programs: see <flux/abi.h> */
#define SEG_WDATA SEG_WIN_DATA
#define SEG_WFS   SEG_WIN_FS
#define SEG_TSS   0xC8                  /* the first processor's; each takes two entries (gdt.c) */

#define IRQ_BASE    32
#define IRQ_TIMER   0
#define IRQ_KBD     1
#define IRQ_COM1    4
#define SYSCALL_VEC 0x80

/* A register of the frame under both its names: all 64 bits, and the 32 a
 * 32-bit program sees. Code that works for one (its signals) uses the
 * short names, with the arithmetic of the 32-bit registers they are. */
#define REG(r64, r32) union { uint64_t r64; uint32_t r32; }

/* Layout produced by isr_common in entry.S, lowest address first. */
struct trapframe {
    uint64_t gs, fs, es, ds;            /* selectors; the kernel itself uses none of them */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    REG(rdi, edi);
    REG(rsi, esi);
    REG(rbp, ebp);
    REG(rbx, ebx);
    REG(rdx, edx);
    REG(rcx, ecx);
    REG(rax, eax);
    uint64_t int_no, err_code;
    /* pushed by the CPU, all of it whatever was interrupted */
    REG(rip, eip);
    uint64_t cs;
    REG(rflags, eflags);
    REG(rsp, useresp);
    uint64_t ss;
};
_Static_assert(sizeof(struct trapframe) == 26 * 8, "the frame entry.S builds");

typedef void (*irq_handler_t)(struct trapframe *tf);

void gdt_init(void);
void gdt_init_ap(int cpu);              /* another processor: the same GDT, its own TSS */
void tss_set_kernel_stack(uint64_t rsp0);

void idt_init(void);
void idt_init_ap(void);
void irq_register(int irq, irq_handler_t handler);
int irq_registered(int irq);           /* another driver has it */
void irq_unmask(int irq);

/* A processor's state across suspend to RAM (entry.S). context_save()
 * returns 0, then 1 again when context_restore() brings it back. */
struct cpu_context {
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip;
    uint64_t cr0, cr3, cr4, rflags;
    uint8_t gdtr[16], idtr[16];         /* 10 bytes each, as SGDT and SIDT store them */
};
_Static_assert(__builtin_offsetof(struct cpu_context, rflags) == 88 &&
               __builtin_offsetof(struct cpu_context, gdtr) == 96 &&
               __builtin_offsetof(struct cpu_context, idtr) == 112, "the offsets entry.S uses");
int context_save(struct cpu_context *c) __attribute__((returns_twice));
void context_restore(struct cpu_context *c) __attribute__((noreturn));

/* Across suspend (idt.c, gdt.c): the 8259s' masks and trigger modes,
 * and the task register, which must be loaded again. */
void pic_suspend(void);
void pic_resume(void);
void gdt_load_tr(int cpu);
void gdt_set_win_teb(uint32_t base);
void gdt_set_tls(uint32_t base);          /* this processor's SEG_USER_TLS */

void trapret(void);
void forkret(void);                     /* a new process's first way out of the kernel */
void switch_context(uint64_t *old_sp, uint64_t new_sp);

/* A copy that survives a missing user page (entry.S): 0, or -EFAULT. The
 * page fault handler turns a fault at uaccess_copy_ip into uaccess_fixup. */
int uaccess_copy(void *dst, const void *src, uint32_t n);
extern char uaccess_copy_ip[], uaccess_fixup[];

#endif
