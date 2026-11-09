/*
 * The kernel's side of uACPI (third_party/uacpi): the services its
 * kernel_api.h asks for, built on what the kernel has.
 *
 * Memory: the first GiB is identity-mapped already; firmware memory
 * above it (the tables and ACPI NVS on a machine with more RAM, and
 * chipset registers) goes into the kernel's late window, and stays
 * mapped: uACPI maps the same few ranges again and again, so a small list
 * of what is mapped serves the repeats, and unmapping does nothing.
 *
 * Time: the cycle counter, calibrated against the PIT at boot, so that
 * time passes with interrupts off too, as it must for AML's timeouts while
 * the namespace is set up. Its zero moves across S3, when the counter
 * starts over.
 *
 * Locks: uACPI's mutexes and events wait as a process would, asleep, or
 * spin where nothing can sleep (at boot, with interrupts off).
 *
 * Deferred work: two kernel threads, as Linux has kacpid and
 * kacpi_notify. GPE methods (and the EC's queries) run on the boot
 * processor, as firmware written for Windows expects; notifications run
 * anywhere. Each queue is a ring that interrupt handlers can add to.
 */
#include <uacpi/kernel_api.h>
#include "kernel.h"
#include "acpi.h"
#include "arch.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "smp.h"
#include "x86.h"

/* ---- memory ---- */

#define NMAPS 48

static struct {
    uint32_t phys, len;                 /* page-aligned */
    uint8_t *virt;
} maps[NMAPS];
static int nmaps;
static spinlock_t map_lock = SPINLOCK_INIT("acpi-map");

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out)
{
    uint32_t pa = acpi_rsdp_phys();
    if (!pa)
        return UACPI_STATUS_NOT_FOUND;
    *out = pa;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    if (!len)
        len = 1;
    if (addr >> 32 || addr + len > 0x100000000ull)
        return UACPI_MAP_FAILED;        /* no PAE: out of reach */
    uint32_t pa = (uint32_t)addr;
    if (pa + len <= KERNEL_SPACE_END) {
        void *p = (void *)pa;
        __asm__("" : "+r"(p));          /* (past the compiler's idea of a null pointer) */
        return p;
    }
    uint32_t base = ALIGN_DOWN(pa, PAGE_SIZE);
    uint32_t span = ALIGN_UP(pa + (uint32_t)len, PAGE_SIZE) - base;
    spin_lock(&map_lock);
    for (int i = 0; i < nmaps; i++) {
        if (base >= maps[i].phys && base + span <= maps[i].phys + maps[i].len) {
            uint8_t *v = maps[i].virt + (pa - maps[i].phys);
            spin_unlock(&map_lock);
            return v;
        }
    }
    spin_unlock(&map_lock);
    /* Write-back: the processor's MTRRs make device registers uncached. */
    uint8_t *v = vmm_map_late(base, span, 0);
    if (!v) {
        klog("acpi: cannot map %08x+%x: the window is full\n", pa, (uint32_t)len);
        return UACPI_MAP_FAILED;
    }
    spin_lock(&map_lock);
    if (nmaps < NMAPS) {
        maps[nmaps].phys = base;
        maps[nmaps].len = span;
        maps[nmaps].virt = v;
        nmaps++;
    }
    spin_unlock(&map_lock);
    return v + (pa - base);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len)
{
    (void)addr;
    (void)len;
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *msg)
{
    static const char *const names[] = { "", "error: ", "warning: ", "", "", "" };
    klog("acpi: %s%s", level < ARRAY_SIZE(names) ? names[level] : "", msg);
}

void *uacpi_kernel_alloc(uacpi_size size)
{
    return kmalloc(size ? size : 1);
}

void uacpi_kernel_free(void *mem)
{
    kfree(mem);
}

/* ---- PCI configuration space and I/O ports ---- */

/* A device is its address, plus one so that 00:00.0 is not NULL. */
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out)
{
    if (a.segment || a.device > 31 || a.function > 7)
        return UACPI_STATUS_UNIMPLEMENTED;     /* only segment 0, by ports 0xCF8/0xCFC */
    *out = (uacpi_handle)(uintptr_t)(((uint32_t)a.bus << 8 | a.device << 3 | a.function) + 1);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle h)
{
    (void)h;
}

static struct pci_dev pci_of(uacpi_handle h)
{
    uint32_t v = (uint32_t)(uintptr_t)h - 1;
    struct pci_dev d = { (uint8_t)(v >> 8), (uint8_t)((v >> 3) & 31), (uint8_t)(v & 7) };
    return d;
}

/* The ports reach the first 256 bytes; past them reads are all ones. */
#define PCI_RW(bits, type)                                                              \
uacpi_status uacpi_kernel_pci_read##bits(uacpi_handle h, uacpi_size off, type *out)     \
{                                                                                       \
    struct pci_dev d = pci_of(h);                                                       \
    *out = off + sizeof(type) <= 256 ? pci_read##bits(&d, (int)off) : (type)~(type)0;  \
    return UACPI_STATUS_OK;                                                             \
}                                                                                       \
uacpi_status uacpi_kernel_pci_write##bits(uacpi_handle h, uacpi_size off, type v)       \
{                                                                                       \
    struct pci_dev d = pci_of(h);                                                       \
    if (off + sizeof(type) <= 256)                                                      \
        pci_write##bits(&d, (int)off, v);                                               \
    return UACPI_STATUS_OK;                                                             \
}
PCI_RW(8, uint8_t)
PCI_RW(16, uint16_t)
PCI_RW(32, uint32_t)

/* A port range is its base port, with a bit above the 16 so that port 0 is not NULL. */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out)
{
    if (base + len > 0x10000)
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out = (uacpi_handle)(uintptr_t)(0x10000u | (uint32_t)base);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle h)
{
    (void)h;
}

#define PORT(h, off) ((uint16_t)(((uint32_t)(uintptr_t)(h) & 0xFFFF) + (uint32_t)(off)))

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *out)
{
    *out = inb(PORT(h, off));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *out)
{
    *out = inw(PORT(h, off));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *out)
{
    *out = inl(PORT(h, off));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v)
{
    outb(PORT(h, off), v);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v)
{
    outw(PORT(h, off), v);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v)
{
    outl(PORT(h, off), v);
    return UACPI_STATUS_OK;
}

/* ---- time ---- */

static uint32_t tsc_khz;                /* cycles per millisecond */
static uint64_t tsc_zero;               /* the counter at "boot" */
static uint64_t ns_before;              /* nanoseconds counted before the last S3 */

void osl_init(void)
{
    /* 20 ms of PIT time (pit_init has run) against the cycle counter. */
    uint64_t t0 = rdtsc();
    udelay(20000);
    uint64_t cycles = rdtsc() - t0;
    tsc_khz = (uint32_t)(cycles / 20);
    if (!tsc_khz)
        tsc_khz = 1;
    tsc_zero = t0;
}

static uint64_t now_ns(void)
{
    if (!tsc_khz)
        return (uint64_t)ticks * (1000000000u / HZ);
    uint64_t c = rdtsc() - tsc_zero;
    /* c * 10^6 / khz, without overflowing for the first few centuries */
    return ns_before + c / tsc_khz * 1000000u + c % tsc_khz * 1000000u / tsc_khz;
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    return now_ns();
}

/* Across S3 the counter starts over from zero. */
void osl_suspend(void)
{
    ns_before = now_ns();
}

void osl_resume(void)
{
    tsc_zero = rdtsc();
}

void uacpi_kernel_stall(uacpi_u8 usec)
{
    udelay(usec);
}

/* A process with interrupts on can sleep. (Interrupts on means no
 * spinlock is held here.) */
int osl_can_sleep(void)
{
    if (!(read_eflags() & EFLAGS_IF))
        return 0;
    struct proc *p = current;
    return p && p->pid > 0;
}

void uacpi_kernel_sleep(uacpi_u64 msec)
{
    if (msec > 0xFFFFFFFFu / 1000)
        msec = 0xFFFFFFFFu / 1000;
    if (osl_can_sleep())
        mdelay((uint32_t)msec);
    else
        udelay((uint32_t)msec * 1000);
}

/* ---- mutexes and events ---- */

/* Both are counters: a mutex holds one unit while free, an event one per
 * signal. Taking a unit waits up to `timeout` ms (0xFFFF: for ever). */
struct sem {
    spinlock_t lk;
    uint32_t count;
};

static struct sem *sem_new(uint32_t count)
{
    struct sem *s = kzalloc(sizeof(*s));
    if (s) {
        spinlock_t init = SPINLOCK_INIT("acpi-sem");
        s->lk = init;
        s->count = count;
    }
    return s;
}

static int sem_take(struct sem *s, uint16_t timeout)
{
    int sleep = osl_can_sleep();
    uint64_t deadline = now_ns() + (uint64_t)timeout * 1000000u;
    uint32_t tick_deadline = ticks + (uint32_t)MIN(((uint64_t)timeout * HZ + 999) / 1000, 0x7FFFFFFEu) + 1;
    spin_lock(&s->lk);
    for (;;) {
        if (s->count) {
            s->count--;
            spin_unlock(&s->lk);
            return 1;
        }
        if (timeout == 0 || (timeout != 0xFFFF && now_ns() >= deadline))
            break;
        if (sleep) {
            sleep_on_lock_timeout(s, &s->lk, timeout == 0xFFFF ? ticks + HZ : tick_deadline);
        } else {
            spin_unlock(&s->lk);
            udelay(50);
            spin_lock(&s->lk);
        }
    }
    spin_unlock(&s->lk);
    return 0;
}

static void sem_give(struct sem *s)
{
    spin_lock(&s->lk);
    s->count++;
    wakeup(s);
    spin_unlock(&s->lk);
}

uacpi_handle uacpi_kernel_create_mutex(void)
{
    return sem_new(1);
}

void uacpi_kernel_free_mutex(uacpi_handle h)
{
    kfree(h);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 timeout)
{
    return sem_take(h, timeout) ? UACPI_STATUS_OK : UACPI_STATUS_TIMEOUT;
}

void uacpi_kernel_release_mutex(uacpi_handle h)
{
    sem_give(h);
}

uacpi_handle uacpi_kernel_create_event(void)
{
    return sem_new(0);
}

void uacpi_kernel_free_event(uacpi_handle h)
{
    kfree(h);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout)
{
    return sem_take(h, timeout) ? UACPI_TRUE : UACPI_FALSE;
}

void uacpi_kernel_signal_event(uacpi_handle h)
{
    sem_give(h);
}

void uacpi_kernel_reset_event(uacpi_handle h)
{
    struct sem *s = h;
    spin_lock(&s->lk);
    s->count = 0;
    spin_unlock(&s->lk);
}

/* The running process; before processes exist, the boot processor. */
uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
    struct proc *p = current;
    return p ? (uacpi_thread_id)p : (uacpi_thread_id)&cpus[0];
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)
{
    return irq_save();
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state)
{
    irq_restore((uint32_t)state);
}

uacpi_handle uacpi_kernel_create_spinlock(void)
{
    spinlock_t *l = kmalloc(sizeof(*l));
    if (l) {
        spinlock_t init = SPINLOCK_INIT("acpi");
        *l = init;
    }
    return l;
}

void uacpi_kernel_free_spinlock(uacpi_handle h)
{
    kfree(h);
}

/* spin_lock() keeps the interrupt state in the lock itself. */
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h)
{
    spin_lock(h);
    return 0;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags flags)
{
    (void)flags;
    spin_unlock(h);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req)
{
    if (req->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        klog("acpi: the firmware reports a fatal error: type %u, code %08x\n",
             req->fatal.type, req->fatal.code);
    return UACPI_STATUS_OK;
}

/* ---- the SCI ---- */

static uacpi_interrupt_handler sci_fn;
static uacpi_handle sci_ctx;
static volatile int sci_running;

static void sci_irq(struct trapframe *tf)
{
    (void)tf;
    uacpi_interrupt_handler fn = sci_fn;
    if (!fn)
        return;
    __atomic_add_fetch(&sci_running, 1, __ATOMIC_ACQ_REL);
    fn(sci_ctx);
    __atomic_sub_fetch(&sci_running, 1, __ATOMIC_ACQ_REL);
}

/*
 * The SCI is level-triggered and shared, active low. The 8259 takes it
 * that way only if its edge/level control register says so for the
 * line, which the firmware usually sets, but not always.
 */
static void set_level_triggered(int irq)
{
    if (irq == 0 || irq == 1 || irq == 2 || irq == 8 || irq == 13 || irq > 15)
        return;
    uint16_t port = irq < 8 ? 0x4D0 : 0x4D1;
    outb(port, (uint8_t)(inb(port) | 1u << (irq & 7)));
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler fn,
                                                    uacpi_handle ctx, uacpi_handle *out)
{
    if (irq > 15 || sci_fn)
        return UACPI_STATUS_UNIMPLEMENTED;     /* the PIC's lines, and the one SCI */
    sci_ctx = ctx;
    sci_fn = fn;
    set_level_triggered((int)irq);
    irq_register((int)irq, sci_irq);
    *out = (uacpi_handle)(uintptr_t)(irq + 1);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn, uacpi_handle h)
{
    (void)h;
    if (sci_fn == fn)
        sci_fn = NULL;
    return UACPI_STATUS_OK;
}

/* ---- deferred work ---- */

#define QLEN 64

struct workq {
    spinlock_t lk;
    struct {
        void (*fn)(void *);
        void *arg;
    } ring[QLEN];
    uint32_t head, tail;                /* free-running */
    int busy;                           /* an item is running */
    struct proc *thread;
    int cpu;                            /* -1: anywhere */
    const char *name;
};

static struct workq gpe_q = { SPINLOCK_INIT("acpi-gpe"), .cpu = 0, .name = "kacpid" };
static struct workq notify_q = { SPINLOCK_INIT("acpi-notify"), .cpu = -1, .name = "kacpi_notify" };
static int workers;                     /* the threads run */

static int enqueue(struct workq *q, void (*fn)(void *), void *arg)
{
    spin_lock(&q->lk);
    if (q->head - q->tail == QLEN) {
        spin_unlock(&q->lk);
        klog("acpi: %s queue full, work dropped\n", q->name);
        return -1;
    }
    q->ring[q->head % QLEN].fn = fn;
    q->ring[q->head % QLEN].arg = arg;
    q->head++;
    wakeup(q);
    spin_unlock(&q->lk);
    return 0;
}

/* Run the queue's items here until it is empty (with the lock held on
 * entry and exit). */
static void drain(struct workq *q)
{
    while (q->tail != q->head) {
        void (*fn)(void *) = q->ring[q->tail % QLEN].fn;
        void *arg = q->ring[q->tail % QLEN].arg;
        q->tail++;
        q->busy = 1;
        spin_unlock(&q->lk);
        fn(arg);
        spin_lock(&q->lk);
        q->busy = 0;
    }
    wakeup(&q->busy);
}

static void worker(struct workq *q)
{
    q->thread = current;
    proc_pin(q->cpu);
    for (;;) {
        spin_lock(&q->lk);
        while (q->tail == q->head)
            sleep_on_lock(q, &q->lk);
        drain(q);
        spin_unlock(&q->lk);
        current->sig_pending = 0;
    }
}

static void gpe_worker(void)
{
    worker(&gpe_q);
}

static void notify_worker(void)
{
    worker(&notify_q);
}

void osl_start(void)
{
    proc_spawn_kthread(gpe_q.name, gpe_worker);
    proc_spawn_kthread(notify_q.name, notify_worker);
    workers = 1;
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler fn, uacpi_handle ctx)
{
    struct workq *q = type == UACPI_WORK_GPE_EXECUTION ? &gpe_q : &notify_q;
    return enqueue(q, (void (*)(void *))fn, ctx) ? UACPI_STATUS_OUT_OF_MEMORY : UACPI_STATUS_OK;
}

int osl_defer(void (*fn)(void *), void *arg)
{
    return enqueue(&gpe_q, fn, arg);
}

static void wait_queue(struct workq *q)
{
    struct proc *me = current;
    spin_lock(&q->lk);
    if (!workers || !osl_can_sleep()) {
        drain(q);                       /* nothing else will run it */
    } else if (q->thread != me) {
        while (q->tail != q->head || q->busy)
            sleep_on_lock(&q->busy, &q->lk);
    }
    spin_unlock(&q->lk);
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    while (__atomic_load_n(&sci_running, __ATOMIC_ACQUIRE))
        cpu_relax();
    wait_queue(&gpe_q);
    wait_queue(&notify_q);
    return UACPI_STATUS_OK;
}
