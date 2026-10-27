/*
 * Lazy FPU switching. The FPU here means the x87, MMX and SSE registers
 * together, the state FXSAVE stores. Each processor's hold the registers of
 * one process, its fpu_owner. Whenever another process runs, CR0.TS is set,
 * so its first FPU or SSE instruction raises "device not available" (#NM);
 * the handler loads the current process's registers from its struct proc
 * (or a clean state for a process that never used them). Processes that
 * never touch the FPU never pay for it.
 *
 * A process may go on on another processor, so the owner's registers are
 * saved as it stops running, not when the next one wants the FPU: its
 * struct proc always has them while it is not running. They are still in
 * the FPU, so if it runs next on the same processor (fpu_cpu) and nothing
 * else used the FPU there meanwhile, it need not load them again.
 *
 * CR4.OSFXSR lets processes use SSE, and CR4.OSXMMEXCPT reports unmasked
 * SIMD floating-point exceptions as #XM, which becomes SIGFPE. The kernel's
 * C code is built with -mgeneral-regs-only and never uses these registers;
 * sse.S borrows a few XMM registers with interrupts off and puts them back.
 */
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "x86.h"

#define CR0_MP (1u << 1)                /* WAIT honours TS */
#define CR0_EM (1u << 2)                /* emulate: every FPU instruction traps */
#define CR0_TS (1u << 3)                /* task switched */
#define CR0_NE (1u << 5)                /* native FPU errors (#MF), not IRQ 13 */

#define CR4_OSFXSR     (1u << 9)        /* FXSAVE/FXRSTOR include SSE; SSE allowed */
#define CR4_OSXMMEXCPT (1u << 10)       /* SIMD exceptions raise #XM, not #UD */

/* Offsets in the FXSAVE image */
#define FX_FCW   0
#define FX_MXCSR 24

#define FCW_DEFAULT   0x037F            /* FNINIT's: all masked, 64-bit precision */
#define MXCSR_DEFAULT 0x1F80            /* all masked, round to nearest */

int kernel_sse;

/* What a process starts with: the reset control words and every register
 * zero, so nothing of an earlier process shows through. */
static uint8_t fpu_clean[512] __attribute__((aligned(16)));

static inline void clts(void)
{
    __asm__ volatile("clts" ::: "memory");
}

static inline void stts(void)
{
    write_cr0(read_cr0() | CR0_TS);
}

static inline void fpu_save(struct proc *p)
{
    __asm__ volatile("fxsave %0" : "=m"(p->fpu) : : "memory");
}

static inline void fpu_restore(const uint8_t *image)
{
    __asm__ volatile("fxrstor %0" : : "m"(*(const uint8_t (*)[512])image) : "memory");
}

void fpu_init_cpu(void)
{
    uint32_t cr0 = read_cr0();
    cr0 &= ~CR0_EM;
    cr0 |= CR0_MP | CR0_NE | CR0_TS;
    write_cr0(cr0);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
}

void fpu_init(void)
{
    fpu_init_cpu();
    uint16_t fcw = FCW_DEFAULT;
    uint32_t mxcsr = MXCSR_DEFAULT;
    memcpy(fpu_clean + FX_FCW, &fcw, sizeof(fcw));
    memcpy(fpu_clean + FX_MXCSR, &mxcsr, sizeof(mxcsr));
    kernel_sse = 1;
}

/* Does this processor's FPU hold p's registers? */
static int fpu_holds(struct cpu *c, struct proc *p)
{
    return c->fpu_owner == p && p->fpu_cpu == c->index;
}

/* #NM from user mode, with interrupts off: give the FPU to the current
 * process. Whoever had it saved its registers when it stopped running
 * (fpu_switch, with sched_lock held). The rest here keep interrupts off
 * too: preempted, the process could go on on another processor, with
 * another's registers in its FPU. */
void fpu_trap(void)
{
    struct cpu *c = this_cpu();
    clts();
    if (fpu_holds(c, current))
        return;
    fpu_restore(current->fpu_used ? current->fpu : fpu_clean);
    current->fpu_used = 1;
    current->fpu_cpu = c->index;
    c->fpu_owner = current;
}

/* The scheduler is about to run next instead of prev: keep prev's
 * registers, and trap next's FPU use unless they are already loaded. */
void fpu_switch(struct proc *prev, struct proc *next)
{
    struct cpu *c = this_cpu();
    if (fpu_holds(c, prev)) {
        clts();
        fpu_save(prev);                 /* FXSAVE leaves them loaded too */
    }
    if (fpu_holds(c, next))
        clts();
    else
        stts();
}

/* fork: the child starts with a copy of the current process's FPU registers. */
void fpu_fork(struct proc *child)
{
    uint32_t flags = irq_save();
    struct proc *p = this_cpu()->proc;
    if (fpu_holds(this_cpu(), p)) {
        /* Unlike FNSAVE, FXSAVE leaves the registers loaded: the parent keeps them. */
        clts();
        fpu_save(p);
    }
    memcpy(child->fpu, p->fpu, sizeof(child->fpu));
    child->fpu_used = p->fpu_used;
    child->fpu_cpu = -1;
    irq_restore(flags);
}

/* exec or exit: the process's FPU registers are no longer wanted. Another
 * processor may still name p its owner, but fpu_cpu says it no longer is. */
void fpu_release(struct proc *p)
{
    uint32_t flags = irq_save();
    struct cpu *c = this_cpu();
    if (c->fpu_owner == p) {
        c->fpu_owner = NULL;
        stts();
    }
    p->fpu_used = 0;
    p->fpu_cpu = -1;
    irq_restore(flags);
}
