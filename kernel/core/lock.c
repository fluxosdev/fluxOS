/*
 * Spinlocks and mutexes (see lock.h).
 *
 * A spinlock is a ticket lock, so processors get it in the order they
 * asked. One spinning with interrupts off cannot take VEC_TLB, so it
 * looks for a flush request itself as it waits: the processor that sent
 * it may be the holder, waiting for the flush before it lets go.
 *
 * The console takes a spinlock before cpu_init() has checked the
 * processor, so this file is built for any 386 (see the Makefile), and
 * until lock_init() a spinlock only turns interrupts off: XADD and
 * CMPXCHG need a 486, and nothing else runs yet anyway.
 */
#include "kernel.h"
#include "proc.h"
#include "smp.h"
#include "x86.h"

volatile int panicking;
static int live;                        /* lock_init() was called */

void lock_init(void)
{
    live = 1;
}

static inline uint32_t fetch_add(volatile uint32_t *p, uint32_t v)
{
    __asm__ volatile("lock; xaddl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
    return v;
}

static inline int compare_swap(volatile uint32_t *p, uint32_t old, uint32_t new)
{
    uint32_t prev;
    __asm__ volatile("lock; cmpxchgl %2, %1" : "=a"(prev), "+m"(*p) : "r"(new), "0"(old) : "memory");
    return prev == old;
}

uint32_t irq_save(void)
{
    uint32_t flags = read_eflags();
    cli();
    return flags;
}

void irq_restore(uint32_t flags)
{
    if (flags & EFLAGS_IF)
        sti();
}

static void took(spinlock_t *l, struct cpu *c, uint32_t flags)
{
    l->cpu = c->index;
    l->flags = flags;
    if (c->nlocks < CPU_HELD_NAMES)
        c->held[c->nlocks] = l->name;
    c->nlocks++;
}

void spin_lock(spinlock_t *l)
{
    uint32_t flags = irq_save();
    if (!live) {
        l->flags = flags;
        return;
    }
    if (panicking)
        return;                         /* the others are stopped, whatever they held */
    struct cpu *c = this_cpu();
    if (l->cpu == c->index)
        panic("spinlock %s taken twice on CPU %d", l->name, c->index);
    uint32_t ticket = fetch_add(&l->next, 1);
    while (__atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) != ticket) {
        if (panicking)
            smp_stopped();              /* VEC_STOP cannot come in with interrupts off */
        smp_tlb_check();
        cpu_relax();
    }
    took(l, c, flags);
}

int spin_trylock(spinlock_t *l)
{
    uint32_t flags = irq_save();
    if (!live) {
        l->flags = flags;
        return 1;
    }
    if (panicking)
        return 1;
    struct cpu *c = this_cpu();
    if (l->cpu == c->index)
        panic("spinlock %s taken twice on CPU %d", l->name, c->index);
    uint32_t serving = __atomic_load_n(&l->serving, __ATOMIC_ACQUIRE);
    if (!compare_swap(&l->next, serving, serving + 1)) {
        irq_restore(flags);
        return 0;
    }
    took(l, c, flags);
    return 1;
}

void spin_unlock(spinlock_t *l)
{
    if (!live) {
        irq_restore(l->flags);
        return;
    }
    if (panicking)
        return;                         /* interrupts stay off for good */
    struct cpu *c = this_cpu();
    if (l->cpu != c->index)
        panic("spinlock %s let go on CPU %d, which does not hold it", l->name, c->index);
    uint32_t flags = l->flags;
    c->nlocks--;
    l->cpu = -1;
    __atomic_store_n(&l->serving, l->serving + 1, __ATOMIC_RELEASE);
    irq_restore(flags);
}

int spin_held(const spinlock_t *l)
{
    uint32_t flags = irq_save();
    int held = l->cpu == this_cpu()->index;
    irq_restore(flags);
    return held;
}

/* Boot code runs alone, before any process: mutexes need not do anything. */
static int no_processes(struct proc *p)
{
    return !p || p->pid == 0;
}

void mutex_lock(struct mutex *m)
{
    struct proc *p = current;
    if (no_processes(p))
        return;
    spin_lock(&m->lk);
    if (m->owner == p)
        panic("mutex %s taken twice by pid %d", m->name, p->pid);
    uint32_t ticket = m->next++;
    /* Uninterruptible: a signal only takes effect once the holder is done. */
    while (m->serving != ticket)
        sleep_on_lock(&m->serving, &m->lk);
    m->owner = p;
    spin_unlock(&m->lk);
}

void mutex_unlock(struct mutex *m)
{
    struct proc *p = current;
    if (no_processes(p))
        return;
    spin_lock(&m->lk);
    if (m->owner != p)
        panic("mutex %s let go by pid %d, which does not hold it", m->name, p->pid);
    m->owner = NULL;
    m->serving++;
    wakeup(&m->serving);
    spin_unlock(&m->lk);
}

int mutex_held(const struct mutex *m)
{
    struct proc *p = current;
    return !no_processes(p) && m->owner == p;
}
