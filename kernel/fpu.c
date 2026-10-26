/*
 * Lazy FPU switching. The FPU here means the x87, MMX and SSE registers
 * together, the state FXSAVE stores. They hold the registers of one
 * process, fpu_owner. Whenever another process runs, CR0.TS is set, so its
 * first FPU or SSE instruction raises "device not available" (#NM); the
 * handler saves the owner's registers in its struct proc and loads the
 * current process's (or a clean state for a process that never used them).
 * Processes that never touch the FPU never pay for it.
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

static struct proc *fpu_owner;
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

void fpu_init(void)
{
    uint32_t cr0 = read_cr0();
    cr0 &= ~CR0_EM;
    cr0 |= CR0_MP | CR0_NE | CR0_TS;
    write_cr0(cr0);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);

    uint16_t fcw = FCW_DEFAULT;
    uint32_t mxcsr = MXCSR_DEFAULT;
    memcpy(fpu_clean + FX_FCW, &fcw, sizeof(fcw));
    memcpy(fpu_clean + FX_MXCSR, &mxcsr, sizeof(mxcsr));
    kernel_sse = 1;
}

/* #NM from user mode: give the FPU to the current process. */
void fpu_trap(void)
{
    clts();
    if (fpu_owner == current)
        return;
    if (fpu_owner)
        fpu_save(fpu_owner);
    fpu_restore(current->fpu_used ? current->fpu : fpu_clean);
    current->fpu_used = 1;
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
        /* Unlike FNSAVE, FXSAVE leaves the registers loaded: the parent keeps them. */
        clts();
        fpu_save(current);
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
