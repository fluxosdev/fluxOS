/*
 * Lazy x87 FPU switching. The FPU keeps the registers of one process,
 * fpu_owner. Whenever another process runs, CR0.TS is set, so its first
 * FPU instruction raises "device not available" (#NM); the handler saves
 * the owner's registers in its struct proc and loads the current
 * process's (or initialises the FPU for a process that never used it).
 * Processes that never touch the FPU never pay for it.
 *
 * The kernel itself is built with -mgeneral-regs-only and never uses the
 * FPU. SSE is not enabled (CR4.OSFXSR stays clear), so SSE instructions
 * still raise #UD; only the x87 state needs saving, with FNSAVE/FRSTOR.
 */
#include "kernel.h"
#include "proc.h"
#include "x86.h"

#define CR0_MP (1u << 1)                /* WAIT honours TS */
#define CR0_EM (1u << 2)                /* emulate: every FPU instruction traps */
#define CR0_TS (1u << 3)                /* task switched */
#define CR0_NE (1u << 5)                /* native FPU errors (#MF), not IRQ 13 */

static struct proc *fpu_owner;

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
    __asm__ volatile("fnsave %0" : "=m"(p->fpu) : : "memory");
}

static inline void fpu_restore(struct proc *p)
{
    __asm__ volatile("frstor %0" : : "m"(p->fpu) : "memory");
}

void fpu_init(void)
{
    uint32_t cr0 = read_cr0();
    cr0 &= ~CR0_EM;
    cr0 |= CR0_MP | CR0_NE | CR0_TS;
    write_cr0(cr0);
}

/* #NM from user mode: give the FPU to the current process. */
void fpu_trap(void)
{
    clts();
    if (fpu_owner == current)
        return;
    if (fpu_owner)
        fpu_save(fpu_owner);
    if (current->fpu_used) {
        fpu_restore(current);
    } else {
        __asm__ volatile("fninit");     /* all exceptions masked, 64-bit precision */
        current->fpu_used = 1;
    }
    fpu_owner = current;
}

/* The scheduler is about to run next: trap its FPU use unless it owns the FPU. */
void fpu_switch(struct proc *next)
{
    if (next == fpu_owner)
        clts();
    else
        stts();
}

/* fork: the child starts with a copy of the current process's FPU registers. */
void fpu_fork(struct proc *child)
{
    if (fpu_owner == current) {
        /* FNSAVE also reinitialises the FPU, so the state now lives only in
         * memory; the parent reloads it on its next FPU instruction. */
        clts();
        fpu_save(current);
        fpu_owner = NULL;
        stts();
    }
    memcpy(child->fpu, current->fpu, sizeof(child->fpu));
    child->fpu_used = current->fpu_used;
}

/* exec or exit: the process's FPU registers are no longer wanted. */
void fpu_release(struct proc *p)
{
    if (fpu_owner == p) {
        fpu_owner = NULL;
        stts();
    }
    p->fpu_used = 0;
}
