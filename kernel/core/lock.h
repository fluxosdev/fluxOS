/*
 * Locks (lock.c). The kernel is preemptible: wherever interrupts are on,
 * the timer may switch to another process in the middle of kernel code,
 * and the other processors run kernel code at the same time. Shared data
 * is kept consistent by two kinds of lock.
 *
 * A spinlock turns interrupts off on this processor while it is held, so
 * its holder is never preempted and no interrupt handler here can want
 * it; other processors spin until it is free. It is the lock for data an
 * interrupt handler also touches, and for short stretches. Nothing may
 * sleep while holding one: schedule() panics if anything tries. The
 * interrupt state it found is put back when it is let go, so spinlocks
 * nest, let go in reverse order.
 *
 * A mutex is for long work that sleeps, such as disk I/O. Whoever waits
 * for it sleeps, and waiters get it in the order they came. Only
 * processes take mutexes, never interrupt handlers. Before the first
 * process runs (boot) they do nothing, as nothing can compete.
 *
 * Lock order, outermost first (take an inner one while holding an outer
 * one, never the other way round):
 *   mutexes: vfs, display
 *   subsystem spinlocks: usb, net, sound, tty, input, pipe, ata, ahci, floppy, acpi
 *   then console, then heap and pmm, then sched (innermost: wakeup()
 *   may be called holding anything), with the small hardware locks
 *   (pci, cmos, pic, pit, random, log) as leaves anywhere.
 */
#ifndef LOCK_H
#define LOCK_H

#include <stdint.h>

#define EFLAGS_IF 0x200u

typedef struct spinlock {
    volatile uint32_t next, serving;    /* tickets: handed out, and now served */
    volatile int cpu;                   /* the holder's processor, -1 when free */
    uint32_t flags;                     /* EFLAGS when it was taken */
    const char *name;
} spinlock_t;

#define SPINLOCK_INIT(n) { 0, 0, -1, 0, n }

void lock_init(void);                   /* once cpu_init() has checked for a 486 and more */
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
int spin_trylock(spinlock_t *l);        /* 1 if taken; never waits */
int spin_held(const spinlock_t *l);     /* by this processor */

struct proc;

struct mutex {
    spinlock_t lk;
    uint32_t next, serving;             /* tickets, as a spinlock's */
    struct proc *owner;
    const char *name;
};

#define MUTEX_INIT(n) { SPINLOCK_INIT(n), 0, 0, 0, n }

void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
int mutex_held(const struct mutex *m);  /* by the calling process */

/* Interrupts off, and back as they were. */
uint32_t irq_save(void);
void irq_restore(uint32_t flags);

/* Set by panic(): locks stop waiting, so the panic screen can be drawn
 * whatever was held when it happened. */
extern volatile int panicking;

#endif
