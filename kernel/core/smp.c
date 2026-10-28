/*
 * Multiprocessing: finding the other processors, starting them, and what
 * lets them share one kernel.
 *
 * The processors come from the ACPI MADT (acpi.c), or else from the older
 * MultiProcessor Specification table. Each has a local APIC. The boot
 * processor's goes on taking the 8259 PIC's interrupts (ExtINT on LINT0,
 * as Linux does with "noapic"), and the I/O APIC is left alone, so every
 * device interrupt still arrives on the boot processor. The others are
 * woken by INIT and startup IPIs into trampoline.S and wait in their own
 * idle loops. The boot CPU accounts all timer ticks under sched_lock;
 * other CPUs receive VEC_RESCHED only for expired slices or new work.
 *
 * Every processor runs kernel code at once; the locks of lock.h keep
 * what they share consistent, the scheduler's (sched_lock, proc.c) the
 * process table and the per-processor fields here.
 *
 * Every processor's TLB must forget a mapping that changes. Only the
 * processor running a process has its address space loaded, so a change
 * made from elsewhere sends VEC_TLB to that one and waits for it; the
 * interrupt needs no lock, and a processor spinning with interrupts off
 * (for a lock, or for another's flush) looks for the request as it spins.
 *
 * "nosmp" on the kernel command line keeps to the boot processor, and
 * "maxcpus=N" to N processors.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "smp.h"
#include "x86.h"

#define TRAMP_BASE 0x8000               /* below 1 MiB, page aligned: as in trampoline.S */

#define MSR_APIC_BASE    0x1B
#define APIC_BASE_X2APIC (1u << 10)
#define APIC_BASE_ENABLE (1u << 11)
#define CPUID_APIC       (1u << 9)      /* CPUID 1, EDX */

/* Local APIC registers */
#define LAPIC_ID     0x020
#define LAPIC_VER    0x030
#define LAPIC_TPR    0x080
#define LAPIC_EOI    0x0B0
#define LAPIC_SVR    0x0F0
#define LAPIC_ESR    0x280
#define LAPIC_ICR_LO 0x300
#define LAPIC_ICR_HI 0x310
#define LAPIC_TIMER  0x320
#define LAPIC_THERM  0x330
#define LAPIC_PERF   0x340
#define LAPIC_LINT0  0x350
#define LAPIC_LINT1  0x360
#define LAPIC_ERROR  0x370

#define SVR_ENABLE   0x100
#define LVT_MASKED   0x10000
#define DM_NMI       0x400
#define DM_INIT      0x500
#define DM_STARTUP   0x600
#define DM_EXTINT    0x700
#define ICR_PENDING  0x1000
#define ICR_ASSERT   0x4000
#define ICR_LEVEL    0x8000

#define CR0_WP  (1u << 16)
#define CR0_PG  (1u << 31)
#define CR4_PSE (1u << 4)
#define CR4_PGE (1u << 7)

struct cpu cpus[MAX_CPUS];
int ncpus = 1;

static uint8_t apic_ids[MAX_CPUS];      /* the processors found */
static int napic;
static volatile uint32_t *lapic;        /* mapped only when there are others to start */

static struct proc ap_idle[MAX_CPUS];
uint32_t ap_stack_top;                  /* trampoline.S takes its stack from here */
static volatile int ap_index;           /* the processor starting */
static volatile int ap_abort;           /* one did not start in time: stop if it ever does */

static volatile int unparking;          /* ap_main() goes back to a parked state */
static struct cpu_context park_ctx[MAX_CPUS];
static uint8_t unpark_stack[4096] __attribute__((aligned(16)));
extern char ap_trampoline[], ap_trampoline_end[];
void ap_main(void) __attribute__((noreturn));

static void flush_local(struct cpu *c)
{
    write_cr3(read_cr3());
    __atomic_store_n(&c->tlb_flush, 0, __ATOMIC_RELEASE);
}

/* Interrupts off: VEC_TLB cannot arrive, so look for the request here. */
void smp_tlb_check(void)
{
    struct cpu *c = this_cpu();
    if (__atomic_load_n(&c->tlb_flush, __ATOMIC_ACQUIRE))
        flush_local(c);
}

void set_resched(void)
{
    uint32_t flags = irq_save();
    this_cpu()->resched = 1;
    irq_restore(flags);
}

/* ---- the local APIC ---- */

static inline uint32_t lapic_read(uint32_t reg)
{
    return lapic[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t v)
{
    lapic[reg / 4] = v;
}

void lapic_eoi(void)
{
    if (lapic)
        lapic_write(LAPIC_EOI, 0);
}

static void lapic_wait_icr(void)
{
    for (int i = 0; i < 1000000 && (lapic_read(LAPIC_ICR_LO) & ICR_PENDING); i++)
        cpu_relax();
}

static void lapic_ipi(uint8_t apic_id, uint32_t icr)
{
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, icr);
}

static void send_ipi(struct cpu *c, uint32_t vec)
{
    lapic_ipi(c->apic_id, ICR_ASSERT | vec);
}

/* Enable this processor's APIC. Only the boot processor takes the PIC's
 * interrupts (and NMIs from LINT1); the timer is not used. */
static void lapic_setup(int boot)
{
    lapic_write(LAPIC_SVR, SVR_ENABLE | VEC_SPURIOUS);
    lapic_write(LAPIC_TIMER, LVT_MASKED);
    uint32_t maxlvt = (lapic_read(LAPIC_VER) >> 16) & 0xFF;
    if (maxlvt >= 4)
        lapic_write(LAPIC_PERF, LVT_MASKED);
    if (maxlvt >= 5)
        lapic_write(LAPIC_THERM, LVT_MASKED);
    lapic_write(LAPIC_LINT0, boot ? DM_EXTINT : DM_EXTINT | LVT_MASKED);
    lapic_write(LAPIC_LINT1, boot ? DM_NMI : DM_NMI | LVT_MASKED);
    lapic_write(LAPIC_ERROR, LVT_MASKED);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_TPR, 0);
}

/* ---- finding the processors ---- */

void smp_add_cpu(uint8_t apic_id)
{
    if (apic_id == 0xFF || napic == MAX_CPUS)
        return;
    for (int i = 0; i < napic; i++)
        if (apic_ids[i] == apic_id)
            return;
    apic_ids[napic++] = apic_id;
}

/* Low physical memory through the identity map (and past the compiler,
 * which takes small constant addresses for null pointers). */
static void *low(uint32_t pa)
{
    void *p = (void *)pa;
    __asm__("" : "+r"(p));
    return p;
}

static int sum_ok(const void *p, uint32_t len)
{
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++)
        s = (uint8_t)(s + ((const uint8_t *)p)[i]);
    return s == 0;
}

struct mp_float {
    char sig[4];                        /* "_MP_" */
    uint32_t config;
    uint8_t length, rev, checksum;
    uint8_t feature[5];
} __attribute__((packed));

struct mp_config {
    char sig[4];                        /* "PCMP" */
    uint16_t length;
    uint8_t rev, checksum;
    char oem[8], product[12];
    uint32_t oem_table;
    uint16_t oem_size, count;
    uint32_t lapic;
    uint16_t ext_length;
    uint8_t ext_checksum, reserved;
} __attribute__((packed));

static const struct mp_float *mp_search(uint32_t start, uint32_t len)
{
    for (uint32_t a = start; a + 16 <= start + len; a += 16) {
        const struct mp_float *f = low(a);
        if (!memcmp(f->sig, "_MP_", 4) && f->length == 1 && sum_ok(f, 16))
            return f;
    }
    return NULL;
}

/* The MultiProcessor Specification's table, for machines without a MADT. */
static void read_mp_table(void)
{
    const uint8_t *bda = low(0x400);
    uint32_t ebda = (uint32_t)(bda[0x0E] | bda[0x0F] << 8) << 4;
    uint32_t base_kb = (uint32_t)(bda[0x13] | bda[0x14] << 8);
    const struct mp_float *f = NULL;
    if (ebda >= 0x80000 && ebda < 0xA0000)
        f = mp_search(ebda, 1024);
    if (!f && base_kb >= 512 && base_kb <= 640)
        f = mp_search(base_kb * 1024 - 1024, 1024);
    if (!f)
        f = mp_search(0xF0000, 0x10000);
    if (!f)
        return;
    if (f->feature[0]) {                /* a default configuration: two processors */
        smp_add_cpu(0);
        smp_add_cpu(1);
        return;
    }
    if (!f->config || f->config >= KERNEL_SPACE_END - 0x10000)
        return;
    const struct mp_config *c = low(f->config);
    if (memcmp(c->sig, "PCMP", 4) || c->length < sizeof(*c) || !sum_ok(c, c->length))
        return;
    const uint8_t *p = (const uint8_t *)(c + 1), *end = (const uint8_t *)c + c->length;
    for (int i = 0; i < c->count && p < end; i++) {
        if (p[0] == 0) {                /* a processor: 20 bytes */
            if (p + 20 > end)
                break;
            if (p[3] & 1)               /* enabled */
                smp_add_cpu(p[1]);
            p += 20;
        } else if (p[0] <= 4) {         /* buses, I/O APICs, interrupts: 8 bytes */
            p += 8;
        } else {
            break;
        }
    }
}

void smp_init(void)
{
    if (!napic)
        read_mp_table();
    if (napic < 2)
        return;                         /* one processor: the APIC stays as it is */
    if (cmdline_has("nosmp")) {
        boot_ok("SMP: %d processors found, using one (nosmp)", napic);
        return;
    }
    uint32_t r[4];
    cpuid(1, r);
    uint64_t base = (r[3] & CPUID_APIC) ? rdmsr(MSR_APIC_BASE) : 0;
    if (!(base & APIC_BASE_ENABLE) || (base & APIC_BASE_X2APIC)) {
        boot_warn("SMP: %d processors found, but the local APIC is %s: using one", napic,
                  (base & APIC_BASE_X2APIC) ? "in x2APIC mode" : "off");
        return;
    }
    lapic = vmm_map_mmio((uint32_t)base & 0xFFFFF000u, PAGE_SIZE, PTE_PCD | PTE_PWT);
    if (!lapic)
        boot_warn("SMP: cannot map the local APIC: using one processor");
}

/* ---- starting the others ---- */

static void cmos_write(uint8_t reg, uint8_t val)
{
    outb(0x70, reg);
    outb(0x71, val);
}

static int wake_ap(struct cpu *c, uint32_t stack_top);

/* Wake the processor with this APIC ID as cpus[ncpus]; 0 if it does not answer. */
static int start_ap(uint8_t apic_id)
{
    int i = ncpus;
    struct cpu *c = &cpus[i];
    uint8_t *stack = kmalloc(KSTACK_SIZE);
    if (!stack)
        return 0;
    struct proc *idle = &ap_idle[i];
    memset(idle, 0, sizeof(*idle));
    idle->state = P_RUNNING;
    idle->pd = kernel_pd;
    idle->fpu_cpu = -1;
    strcpy(idle->name, "idle");
    c->index = i;
    c->apic_id = apic_id;
    c->idle = idle;
    c->pd = kernel_pd;
    c->slice = QUANTUM;
    if (!wake_ap(c, (uint32_t)stack + KSTACK_SIZE)) {
        ap_abort = 1;                   /* (its stack stays: it might still come) */
        return 0;
    }
    ncpus++;
    return 1;
}

/* INIT, then two startup IPIs, as the MultiProcessor Specification has it,
 * into trampoline.S and ap_main() on the stack given. 1 once it is up. */
static int wake_ap(struct cpu *c, uint32_t stack_top)
{
    uint8_t apic_id = c->apic_id;
    ap_index = c->index;
    ap_stack_top = stack_top;
    lapic_write(LAPIC_ESR, 0);
    lapic_ipi(apic_id, ICR_LEVEL | ICR_ASSERT | DM_INIT);
    lapic_wait_icr();
    udelay(10000);
    lapic_ipi(apic_id, ICR_LEVEL | DM_INIT);
    lapic_wait_icr();
    for (int n = 0; n < 2 && !c->online; n++) {
        lapic_write(LAPIC_ESR, 0);
        lapic_ipi(apic_id, ICR_ASSERT | DM_STARTUP | (TRAMP_BASE >> 12));
        udelay(300);
        lapic_wait_icr();
        udelay(200);
    }
    for (int t = 0; t < 10000 && !c->online; t++)       /* up to a second */
        udelay(100);
    return c->online;
}

/* The trampoline below 1 MiB, and the warm reset vector (shutdown code
 * 0Ah: jump to 40:67), which the oldest APICs start through instead. */
static void trampoline_in(void)
{
    memcpy(low(TRAMP_BASE), ap_trampoline, (size_t)(ap_trampoline_end - ap_trampoline));
    cmos_write(0x0F, 0x0A);
    *(volatile uint16_t *)low(0x467) = 0;
    *(volatile uint16_t *)low(0x469) = TRAMP_BASE >> 4;
}

static void trampoline_out(void)
{
    cmos_write(0x0F, 0);                /* a reset POSTs as usual again */
    *(volatile uint32_t *)low(0x467) = 0;
}

void smp_start(void)
{
    if (!lapic)
        return;
    struct cpu *boot = &cpus[0];
    boot->apic_id = (uint8_t)(lapic_read(LAPIC_ID) >> 24);
    boot->pd = kernel_pd;
    lapic_setup(1);
    boot->online = 1;

    int max = MAX_CPUS;
    char arg[8];
    if (cmdline_value("maxcpus", arg, sizeof(arg))) {
        max = 0;
        for (const char *s = arg; *s >= '0' && *s <= '9'; s++)
            max = max * 10 + (*s - '0');
        if (max < 1)
            max = 1;
    }

    trampoline_in();

    for (int i = 0; i < napic && ncpus < max; i++) {
        if (apic_ids[i] == boot->apic_id)
            continue;
        if (!start_ap(apic_ids[i])) {
            boot_warn("SMP: the processor with APIC ID %u did not start", apic_ids[i]);
            break;
        }
    }

    trampoline_out();
    if (ncpus > 1)
        boot_ok("SMP: %d processors running", ncpus);
    else
        boot_warn("SMP: running on one processor");
}

/* Another processor, from trampoline.S: protected mode, no paging yet,
 * interrupts off, on its own stack. */
void ap_main(void)
{
    if (ap_abort)
        smp_stopped();
    struct cpu *c = &cpus[ap_index];
    write_cr4(read_cr4() | CR4_PSE | CR4_PGE);
    write_cr3((uint32_t)kernel_pd);
    write_cr0(read_cr0() | CR0_PG | CR0_WP);
    gdt_init_ap(c->index);
    idt_init_ap();
    cpu_init_ap();
    fpu_init_cpu();
    lapic_setup(0);
    if (unparking)
        context_restore(&park_ctx[c->index]);  /* back into smp_park() */
    c->proc = c->idle;
    __atomic_store_n(&c->online, 1, __ATOMIC_RELEASE);
    /* The idle loop: interrupts call schedule() (idt.c). */
    for (;;)
        __asm__ volatile("sti; hlt" ::: "memory");
}

/* ---- interrupts between processors ---- */

/* With sched_lock, as are all of these that look at other processors:
 * coalesce wakeups, signals and quantum expiration until the target schedules. */
static void resched_cpu(struct cpu *c)
{
    c->resched = 1;
    if (!c->kicked) {
        c->kicked = 1;
        send_ipi(c, VEC_RESCHED);
    }
}

/* sched_lock makes remote process/slice accounting safe. This avoids
 * waking idle CPUs and reduces busy-CPU timer IPIs from HZ to HZ / QUANTUM. */
void smp_tick(void)
{
    struct cpu *self = this_cpu();
    for (int i = 0; i < ncpus; i++) {
        struct cpu *c = &cpus[i];
        if (c == self || !c->online)
            continue;
        proc_tick_on_cpu(c);
        if (c->proc != c->idle && c->resched)
            resched_cpu(c);
    }
}

/* A process became ready: 1 if an idle processor will run it, this one
 * (which looks for work on its way out of the kernel) or another, woken
 * by VEC_RESCHED; 0 if every processor is busy. */
int smp_wake_idle(int preferred)
{
    struct cpu *self = this_cpu();
    if (preferred >= 0 && preferred < ncpus) {
        struct cpu *c = &cpus[preferred];
        if (c == self && c->proc == c->idle)
            return 1;
        if (c != self && c->online && c->proc == c->idle && !c->kicked) {
            resched_cpu(c);
            return 1;
        }
    }
    if (self->proc == self->idle)
        return 1;
    for (int i = 0; i < ncpus; i++) {
        struct cpu *c = &cpus[i];
        if (c != self && c->online && c->proc == c->idle && !c->kicked) {
            resched_cpu(c);
            return 1;
        }
    }
    return 0;
}

/* A process that may only run on processor cpu is ready: make that one
 * look, whether it is idle (an IPI wakes it) or busy (it switches on its
 * way out of the kernel). 1 if it was told, or is this processor. */
int smp_wake_cpu(int cpu)
{
    if (cpu < 0 || cpu >= ncpus)
        return 0;
    struct cpu *c = &cpus[cpu];
    if (c == this_cpu()) {
        c->resched = 1;
        return 1;
    }
    if (!c->online)
        return 0;
    resched_cpu(c);
    return 1;
}

/* p runs on another processor: make it come into the kernel (for a signal). */
void smp_kick_proc(struct proc *p)
{
    struct cpu *self = this_cpu();
    for (int i = 0; i < ncpus; i++)
        if (&cpus[i] != self && cpus[i].proc == p)
            resched_cpu(&cpus[i]);
}

/* The mappings of pd changed. Two processors may do this at once, each
 * waiting for the other's flush, so each flushes for the other meanwhile. */
void smp_flush_tlb(uint32_t *pd)
{
    if (ncpus < 2)
        return;
    uint32_t flags = irq_save();
    struct cpu *self = this_cpu();
    for (int i = 0; i < ncpus; i++) {
        struct cpu *c = &cpus[i];
        if (c != self && c->pd == pd) {
            __atomic_store_n(&c->tlb_flush, 1, __ATOMIC_RELEASE);
            send_ipi(c, VEC_TLB);
        }
    }
    for (int i = 0; i < ncpus; i++)
        while (&cpus[i] != self && __atomic_load_n(&cpus[i].tlb_flush, __ATOMIC_ACQUIRE)) {
            if (panicking)
                smp_stopped();
            smp_tlb_check();
            cpu_relax();
        }
    irq_restore(flags);
}

void smp_tlb_ipi(void)
{
    flush_local(this_cpu());
    lapic_eoi();
}

/* Panic or halt: the others stop where they are. */
void smp_stop_others(void)
{
    if (!lapic)
        return;
    struct cpu *self = this_cpu();
    for (int i = 0; i < ncpus; i++)
        if (&cpus[i] != self)
            send_ipi(&cpus[i], VEC_STOP);
}

void smp_stopped(void)
{
    for (;;)
        __asm__ volatile("cli; hlt" ::: "memory");
}

/* ---- suspend to RAM ---- */

/*
 * VEC_PARK, with interrupts off. Whatever ran here is left as it was,
 * its state on its stack and the processor's in park_ctx; the processor
 * halts with its caches written back. smp_unpark_others() starts it
 * again, through ap_main(), which puts it back here to return from the
 * interrupt.
 */
void smp_park(void)
{
    lapic_eoi();
    fpu_suspend_cpu();
    if (context_save(&park_ctx[this_cpu()->index])) {
        __atomic_store_n(&this_cpu()->online, 1, __ATOMIC_RELEASE);
        return;                         /* back where the interrupt came */
    }
    __asm__ volatile("wbinvd" ::: "memory");
    __atomic_store_n(&this_cpu()->online, 0, __ATOMIC_RELEASE);
    for (;;)
        __asm__ volatile("cli; hlt" ::: "memory");
}

/* Park every other processor. If one does not stop within a second (it
 * cannot be holding a spinlock, which would only delay the interrupt),
 * the rest are started again: then -1. */
int smp_park_others(void)
{
    if (ncpus < 2)
        return 0;
    struct cpu *self = this_cpu();
    for (int i = 0; i < ncpus; i++)
        if (&cpus[i] != self)
            send_ipi(&cpus[i], VEC_PARK);
    for (int i = 0; i < ncpus; i++) {
        for (int t = 0; &cpus[i] != self && __atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE); t++) {
            if (t == 100000) {
                klog("smp: processor %d did not stop for suspend\n", i);
                smp_unpark_others();
                return -1;
            }
            smp_tlb_check();
            udelay(10);
        }
    }
    return 0;
}

/* Interrupts off, on the boot processor: its local APIC again (the power
 * went), and the parked processors back to work. */
void smp_unpark_others(void)
{
    if (!lapic)
        return;
    lapic_setup(1);
    struct cpu *self = this_cpu();
    trampoline_in();
    unparking = 1;
    for (int i = 0; i < ncpus; i++) {
        struct cpu *c = &cpus[i];
        if (c == self || c->online)
            continue;
        /* The stack only until context_restore() leaves it: one at a time. */
        if (!wake_ap(c, (uint32_t)unpark_stack + sizeof(unpark_stack)))
            klog("smp: processor %d did not come back from suspend\n", i);
    }
    unparking = 0;
    trampoline_out();
}
