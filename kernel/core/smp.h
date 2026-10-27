/*
 * Multiprocessing (smp.c): the processors and the interrupts they send
 * each other. Every processor runs kernel and user code at once; the
 * locks of lock.h keep what they share consistent.
 *
 * this_cpu() names the processor it ran on, which may already be another
 * once it returns if interrupts are on (the process can be preempted and
 * go on elsewhere): use it with interrupts off, or holding a spinlock.
 */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "arch.h"
#include "lock.h"

#define MAX_CPUS 16
#define CPU_CACHE_LINE 64
#define CPU_HELD_NAMES 4

/* Interrupts between processors (and the local APIC's spurious one). */
#define VEC_RESCHED  0xF0               /* something to run: look */
#define VEC_TICK     0xF1               /* the timer ticked (on the boot processor) */
#define VEC_TLB      0xF2               /* flush the TLB; needs no lock */
#define VEC_STOP     0xF3               /* panic: halt */
#define VEC_PARK     0xF4               /* suspend to RAM: save your state and halt */
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
    int nlocks;                         /* spinlocks held here */
    const char *held[CPU_HELD_NAMES];   /* the first few of them, for panics */
    volatile int kicked;                /* sent VEC_RESCHED, not yet scheduled */
    volatile int tlb_flush;             /* asked to flush its TLB */
    uint32_t idle_ticks;                /* timer ticks it found itself idle */
} __attribute__((aligned(CPU_CACHE_LINE)));

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

/* Switch processes on the way out of the kernel, on whatever processor
 * the caller is on by then. */
void set_resched(void);

void smp_add_cpu(uint8_t apic_id);      /* from the ACPI MADT */
void smp_init(void);                    /* find the processors; before the first process */
void smp_start(void);                   /* start the others; after proc_init() */
void smp_tick(void);                    /* the timer ticked: pass it on */
int smp_wake_idle(int preferred);       /* prefer its last CPU if idle; -1 means any */
void smp_kick_proc(struct proc *p);     /* p, running elsewhere, has a signal */
void smp_flush_tlb(uint32_t *pd);       /* pd's mappings changed */
void smp_tlb_ipi(void);
void smp_tlb_check(void);               /* waiting with interrupts off: flush if asked */
void smp_stop_others(void);
void smp_stopped(void) __attribute__((noreturn));
/* Suspend to RAM: the others save their state and halt (park), and are
 * started again into it (unpark); interrupts off on the caller, the boot
 * processor. smp_park() is their side, from VEC_PARK. */
int smp_park_others(void);             /* 0, or -1 (and nothing parked) */
void smp_unpark_others(void);
void smp_park(void);
void lapic_eoi(void);

#endif
