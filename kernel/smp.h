/*
 * Multiprocessing (smp.c): the processors, the kernel lock, and the
 * interrupts they send each other.
 *
 * The kernel runs on one processor at a time: whichever holds the kernel
 * lock, taken on every way in (trap) and let go on the way back to user
 * mode or to the idle loop. User processes run on all of them at once.
 */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "arch.h"

#define MAX_CPUS 16

/* Interrupts between processors (and the local APIC's spurious one). */
#define VEC_RESCHED  0xF0               /* something to run: look */
#define VEC_TICK     0xF1               /* the timer ticked (on the boot processor) */
#define VEC_TLB      0xF2               /* flush the TLB; taken without the kernel lock */
#define VEC_STOP     0xF3               /* panic: halt */
#define VEC_SPURIOUS 0xFF

struct proc;

struct cpu {
    int index;
    uint8_t apic_id;
    volatile int online;
    struct proc *proc;                  /* running here */
    struct proc *idle;                  /* runs when nothing else can */
    struct proc *fpu_owner;             /* whose registers the FPU may hold (fpu.c) */
    uint32_t *pd;                       /* the page directory in CR3 */
    int slice;                          /* timer ticks left in this time slice */
    volatile int resched;               /* schedule() on the way out of the kernel */
    int lock_depth;                     /* kernel lock, taken this many times here */
    volatile int kicked;                /* sent VEC_RESCHED, not yet scheduled */
    volatile int tlb_flush;             /* asked to flush its TLB */
    uint32_t idle_ticks;                /* timer ticks it found itself idle */
};

extern struct cpu cpus[MAX_CPUS];
extern int ncpus;                       /* processors running */

/* This processor: each has its own TSS, so the task register tells them apart. */
static inline struct cpu *this_cpu(void)
{
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    uint32_t i = ((uint32_t)tr - SEG_TSS) >> 3;
    return &cpus[i < MAX_CPUS ? i : 0];
}

#define need_resched (this_cpu()->resched)

void kernel_lock(void);
void kernel_unlock(void);

void smp_add_cpu(uint8_t apic_id);      /* from the ACPI MADT */
void smp_init(void);                    /* find the processors; before the first process */
void smp_start(void);                   /* start the others; after proc_init() */
void smp_tick(void);                    /* the timer ticked: pass it on */
int smp_wake_idle(void);                /* a process became ready: 1 if an idle processor takes it */
void smp_kick_proc(struct proc *p);     /* p, running elsewhere, has a signal */
void smp_flush_tlb(uint32_t *pd);       /* pd's mappings changed */
void smp_tlb_ipi(void);
void smp_stop_others(void);
void smp_stopped(void) __attribute__((noreturn));
void lapic_eoi(void);

#endif
