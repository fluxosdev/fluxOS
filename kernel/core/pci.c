/* Minimal PCI bus enumeration, enough to find the storage, USB and network controllers. */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "x86.h"

#define CONFIG_ADDR 0xCF8
#define CONFIG_DATA 0xCFC

#define PCI_STATUS      0x06
#define PCI_CAP_LIST    0x10            /* status: there is a capability list */
#define PCI_CAP_PTR     0x34
#define PCI_HEADER_TYPE 0x0E
#define PCI_BUS_NUMBERS 0x18            /* bridge: primary, secondary, subordinate */
#define PCI_MEM_WINDOW  0x20            /* bridge: memory base and limit */
#define PCI_PREF_WINDOW 0x24            /* bridge: prefetchable base and limit */
#define PCI_CAP_ID_PM   0x01
#define PCI_CAP_ID_EXP  0x10
#define PCI_PM_CTRL     0x04            /* PMCSR, in the capability */
#define PCI_PM_STATE_MASK 0x0003
#define PCI_PM_PME_STATUS 0x8000        /* write 1 to clear */

/* Mechanism #1 reaches the first 256 bytes of a function only; the PCIe
 * extended space past them needs ECAM. Rather than wrap round onto the
 * header, reads there return all ones, as from a missing register, and
 * writes are dropped. Out of line: inlined, the test leads GCC to warn
 * about array indexes in callers' loops that no offset there can reach. */
static __attribute__((noinline)) int out_of_reach(int off)
{
    return (unsigned)off > 0xFF;
}

/* Each access is two port writes or reads: they must not interleave. */
static spinlock_t pci_lock = SPINLOCK_INIT("pci");

static uint32_t config_addr(const struct pci_dev *p, int off)
{
    return 0x80000000u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 |
           (uint32_t)p->fn << 8 | (uint32_t)(off & 0xFC);
}

uint32_t pci_read32(const struct pci_dev *p, int off)
{
    if (out_of_reach(off))
        return (uint32_t)~0u;
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    uint32_t v = inl(CONFIG_DATA);
    spin_unlock(&pci_lock);
    return v;
}

void pci_write32(const struct pci_dev *p, int off, uint32_t v)
{
    if (out_of_reach(off))
        return;
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    outl(CONFIG_DATA, v);
    spin_unlock(&pci_lock);
}

uint16_t pci_read16(const struct pci_dev *p, int off)
{
    if (out_of_reach(off))
        return (uint16_t)~0u;
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    uint16_t v = inw((uint16_t)(CONFIG_DATA + (off & 2)));
    spin_unlock(&pci_lock);
    return v;
}

void pci_write16(const struct pci_dev *p, int off, uint16_t v)
{
    if (out_of_reach(off))
        return;
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    outw((uint16_t)(CONFIG_DATA + (off & 2)), v);
    spin_unlock(&pci_lock);
}

uint8_t pci_read8(const struct pci_dev *p, int off)
{
    if (out_of_reach(off))
        return (uint8_t)~0u;
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    uint8_t v = inb((uint16_t)(CONFIG_DATA + (off & 3)));
    spin_unlock(&pci_lock);
    return v;
}

void pci_write8(const struct pci_dev *p, int off, uint8_t v)
{
    if (out_of_reach(off))
        return;
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    outb((uint16_t)(CONFIG_DATA + (off & 3)), v);
    spin_unlock(&pci_lock);
}

/* Call match on every function present; stop at the first that returns 1. */
static int pci_scan(int (*match)(const struct pci_dev *p, const void *arg), const void *arg,
                    struct pci_dev *out)
{
    /* Is there a PCI host bridge answering mechanism #1 at all? Under the
     * lock: another processor's access in between would use our address. */
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, 0x80000000u);
    int present = inl(CONFIG_ADDR) == 0x80000000u;
    spin_unlock(&pci_lock);
    if (!present)
        return -1;

    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            struct pci_dev p = { (uint8_t)bus, (uint8_t)dev, 0 };
            if ((pci_read32(&p, 0x00) & 0xFFFF) == 0xFFFF)
                continue;                               /* no device here */
            int nfn = (pci_read32(&p, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (int fn = 0; fn < nfn; fn++) {
                p.fn = (uint8_t)fn;
                if ((pci_read32(&p, 0x00) & 0xFFFF) == 0xFFFF)
                    continue;
                if (match(&p, arg)) {
                    *out = p;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int class_matches(const struct pci_dev *p, const void *arg)
{
    uint32_t cls = pci_read32(p, 0x08) >> 8;            /* class, sub, prog-if */
    return (cls >> 8) == *(const uint16_t *)arg;
}

int pci_find_class(uint16_t class_sub, struct pci_dev *out, uint8_t *prog_if)
{
    if (pci_scan(class_matches, &class_sub, out) < 0)
        return -1;
    *prog_if = (uint8_t)(pci_read32(out, 0x08) >> 8);
    return 0;
}

static int id_matches(const struct pci_dev *p, const void *arg)
{
    return pci_read32(p, 0x00) == *(const uint32_t *)arg;
}

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    uint32_t id = (uint32_t)device << 16 | vendor;
    return pci_scan(id_matches, &id, out);
}

static int log_device(const struct pci_dev *p, const void *arg)
{
    (void)arg;
    uint32_t id = pci_read32(p, 0x00), cls = pci_read32(p, 0x08);
    klog("pci %02x:%02x.%d %04x:%04x class %04x prog-if %02x rev %02x irq %d\n", p->bus, p->dev,
         p->fn, id & 0xFFFF, id >> 16, cls >> 16, (cls >> 8) & 0xFF, cls & 0xFF, pci_irq(p));
    return 0;
}

void pci_log_devices(void)
{
    struct pci_dev unused;
    pci_scan(log_device, NULL, &unused);
}

/* Raw registers only: nothing is changed, and no bridge is reset, since
 * one may also carry storage. */
static void log_network_fn(const struct pci_dev *p)
{
    uint32_t id = pci_read32(p, 0x00), cls = pci_read32(p, 0x08) >> 16;
    klog("pci-net: %02x:%02x.%u %04x:%04x class %04x command %04x\n", p->bus, p->dev, p->fn,
         id & 0xFFFF, id >> 16, cls, pci_read16(p, PCI_COMMAND));
    if ((pci_read8(p, PCI_HEADER_TYPE) & 0x7F) == 1) {
        uint32_t buses = pci_read32(p, PCI_BUS_NUMBERS);
        uint32_t mem = pci_read32(p, PCI_MEM_WINDOW), pref = pci_read32(p, PCI_PREF_WINDOW);
        int pref64 = (pref & 0xF) == 1;
        klog("pci-net: bridge primary %u secondary %u subordinate %u\n", buses & 0xFF,
             (buses >> 8) & 0xFF, (buses >> 16) & 0xFF);
        /* A window whose base is above its limit is closed. */
        klog("pci-net: bridge memory %08x-%08x prefetchable %08x%08x-%08x%08x\n",
             (mem & 0xFFF0) << 16, (mem & 0xFFF00000) | 0xFFFFF,
             pref64 ? pci_read32(p, 0x28) : 0, (pref & 0xFFF0) << 16,
             pref64 ? pci_read32(p, 0x2C) : 0, (pref & 0xFFF00000) | 0xFFFFF);
    } else {
        klog("pci-net: BAR0 %08x BAR1 %08x\n", pci_read32(p, PCI_BAR0), pci_read32(p, PCI_BAR1));
    }
    if (!(pci_read16(p, PCI_STATUS) & PCI_CAP_LIST))
        return;
    uint8_t cap = pci_read8(p, PCI_CAP_PTR) & 0xFC;
    for (int n = 0; cap >= 0x40 && n < 48; n++) {
        uint8_t kind = pci_read8(p, cap);
        if (kind == 0xFF)
            break;
        if (kind == PCI_CAP_ID_PM)
            klog("pci-net: PMCSR %04x\n", pci_read16(p, cap + PCI_PM_CTRL));
        if (kind == PCI_CAP_ID_EXP) {
            klog("pci-net: PCIe link control/status %08x\n", pci_read32(p, cap + 0x10));
            /* A root or downstream port with a slot: bit 6 is presence detect. */
            if (pci_read16(p, cap + 2) & 0x100) {
                uint16_t slot = pci_read16(p, cap + 0x1A);
                klog("pci-net: PCIe slot status %04x (card %s)\n", slot,
                     slot & 0x40 ? "present" : "absent");
            }
        }
        cap = pci_read8(p, cap + 1) & 0xFC;
    }
}

static int log_network_path(const struct pci_dev *p, const void *arg)
{
    (void)arg;
    uint32_t cls = pci_read32(p, 0x08) >> 16;
    if ((cls >> 8) == 2 || cls == 0x0604)
        log_network_fn(p);
    return 0;
}

void pci_log_network_diagnostics(const struct pci_dev *dev)
{
    struct pci_dev b;
    if (!dev) {
        pci_scan(log_network_path, NULL, &b);
        return;
    }
    log_network_fn(dev);
    for (b = *dev; pci_parent_bridge(&b, &b) == 0;)
        log_network_fn(&b);
}

static int bridge_to(const struct pci_dev *p, const void *arg)
{
    if ((pci_read8(p, PCI_HEADER_TYPE) & 0x7F) != 1)
        return 0;
    uint8_t secondary = (uint8_t)(pci_read32(p, PCI_BUS_NUMBERS) >> 8);
    /* A bridge sits on a lower bus than the one behind it; insisting on
     * that skips unconfigured bridges (all zeros) and ends every walk up. */
    return p->bus < secondary && secondary == *(const uint8_t *)arg;
}

int pci_parent_bridge(const struct pci_dev *p, struct pci_dev *out)
{
    uint8_t bus = p->bus;
    return bus ? pci_scan(bridge_to, &bus, out) : -1;
}

void pci_enable_upstream(const struct pci_dev *p, uint32_t cmd_bits)
{
    struct pci_dev b = *p;
    while (pci_parent_bridge(&b, &b) == 0)
        pci_enable(&b, cmd_bits);
}

struct nth {
    uint16_t class_sub;
    int n;
};

static int class_nth_matches(const struct pci_dev *p, const void *arg)
{
    struct nth *a = (struct nth *)arg;
    uint32_t cls = pci_read32(p, 0x08) >> 8;
    return (cls >> 8) == a->class_sub && a->n-- == 0;
}

int pci_find_class_nth(uint16_t class_sub, int n, struct pci_dev *out, uint8_t *prog_if)
{
    struct nth a = { class_sub, n };
    if (pci_scan(class_nth_matches, &a, out) < 0)
        return -1;
    *prog_if = (uint8_t)(pci_read32(out, 0x08) >> 8);
    return 0;
}

void pci_enable(const struct pci_dev *p, uint32_t cmd_bits)
{
    uint32_t cmd = pci_read32(p, PCI_COMMAND) & 0xFFFF;         /* keep RW1C status bits clear */
    pci_write32(p, PCI_COMMAND, cmd | cmd_bits);
}

int pci_irq(const struct pci_dev *p)
{
    uint8_t line = (uint8_t)pci_read32(p, PCI_INTERRUPT);
    return line == 0 || line >= 16 ? -1 : line;
}

void *pci_map_bar(const struct pci_dev *p, int bar)
{
    uint32_t v = pci_read32(p, bar);
    if (v & 1)
        return NULL;                                    /* an I/O port BAR */
    if ((v & 6) == 4 && pci_read32(p, bar + 4))
        return NULL;                                    /* 64-bit, above 4 GiB */
    uint32_t base = v & ~0xFu;
    if (!base)
        return NULL;

    /* Size it: write all ones with decoding off, see which bits stick. */
    uint32_t cmd = pci_read32(p, PCI_COMMAND) & 0xFFFF;
    pci_write32(p, PCI_COMMAND, cmd & ~PCI_CMD_MEM);
    pci_write32(p, bar, 0xFFFFFFFFu);
    uint32_t mask = pci_read32(p, bar) & ~0xFu;
    pci_write32(p, bar, v);
    pci_write32(p, PCI_COMMAND, cmd | PCI_CMD_MEM);
    uint32_t size = mask ? ~mask + 1 : PAGE_SIZE;

    return vmm_map_mmio(base, size, PTE_PCD | PTE_PWT);
}

/* ---- suspend to RAM ---- */

/*
 * Devices lose their configuration with the power, and the firmware puts
 * back only some of it. The standard header (the BARs, the command
 * register, a bridge's bus numbers and windows) of every function is
 * kept, and for IDE controllers their timing registers too (0x40-0x5F on
 * Intel's and most others'), which the drives' DMA modes depend on, and
 * for the ISA (LPC) bridge 0x40-0x7F, where Intel's chipsets keep the
 * routing of the PCI interrupt pins to the PIC's lines (QEMU's firmware
 * does not put it back on waking, and no PCI device interrupts then).
 * Bridges come before what is behind them, in the order they were found.
 * A function the firmware left in a low power state (D1-D3) is brought
 * back to D0 first, as Linux does: in D3 its memory and I/O decoding is
 * off whatever the command register says, and its registers are blank.
 */
#define MAX_SAVED 96
#define IDE_DWORDS 8
#define ISA_DWORDS 16

static struct saved_fn {
    struct pci_dev d;
    int extra;                          /* dwords kept from 0x40 on */
    uint8_t pm;                         /* the power management capability, or 0 */
    uint32_t cfg[16 + ISA_DWORDS];
} saved[MAX_SAVED];
static int nsaved;

static uint8_t find_pm_cap(const struct pci_dev *p)
{
    if (!(pci_read16(p, PCI_STATUS) & PCI_CAP_LIST) || (pci_read8(p, PCI_HEADER_TYPE) & 0x7F) > 1)
        return 0;
    uint8_t pos = pci_read8(p, PCI_CAP_PTR) & 0xFC;
    for (int n = 0; pos >= 0x40 && n < 48; n++) {
        uint8_t id = pci_read8(p, pos);
        if (id == 0xFF)
            break;
        if (id == PCI_CAP_ID_PM)
            return pos;
        pos = pci_read8(p, pos + 1) & 0xFC;
    }
    return 0;
}

int pci_set_d0(const struct pci_dev *p)
{
    uint8_t pm = find_pm_cap(p);
    if (!pm)
        return -1;
    uint16_t pmcsr = pci_read16(p, pm + PCI_PM_CTRL);
    int state = pmcsr & PCI_PM_STATE_MASK;
    if (!state)
        return 0;
    uint32_t cfg[16];
    for (int i = 1; i < 16; i++)
        cfg[i] = pci_read32(p, i * 4);
    pci_write16(p, pm + PCI_PM_CTRL, pmcsr & ~(PCI_PM_STATE_MASK | PCI_PM_PME_STATUS));
    udelay(10000);                      /* D3hot to D0: 10 ms before it is addressed */
    /* Without No_Soft_Reset it comes back blank: BARs before the command register. */
    for (int i = 15; i >= 1; i--)
        if (pci_read32(p, i * 4) != cfg[i])
            pci_write32(p, i * 4, cfg[i]);
    return state;
}

static int save_fn(const struct pci_dev *p, const void *arg)
{
    (void)arg;
    if (nsaved == MAX_SAVED)
        return 1;
    struct saved_fn *f = &saved[nsaved++];
    f->d = *p;
    for (int i = 0; i < 16; i++)
        f->cfg[i] = pci_read32(p, i * 4);
    uint16_t class = (uint16_t)(f->cfg[2] >> 16);
    f->extra = class == 0x0101 ? IDE_DWORDS : class == 0x0601 ? ISA_DWORDS : 0;
    f->pm = find_pm_cap(p);
    for (int i = 0; i < f->extra; i++)
        f->cfg[16 + i] = pci_read32(p, 0x40 + i * 4);
    return 0;
}

void pci_suspend(void)
{
    struct pci_dev unused;
    nsaved = 0;
    pci_scan(save_fn, NULL, &unused);
}

/* Interrupts off, before any driver touches its device again. Last to
 * first, as Linux does, so the command register (decoding, bus mastering)
 * comes after the BARs; only what changed is written. */
void pci_resume(void)
{
    for (int n = 0; n < nsaved; n++) {
        struct saved_fn *f = &saved[n];
        if (pci_read32(&f->d, 0) != f->cfg[0])
            continue;                   /* not there now */
        if (f->pm) {
            uint16_t pmcsr = pci_read16(&f->d, f->pm + PCI_PM_CTRL);
            if ((pmcsr & PCI_PM_STATE_MASK) != 0) {
                pci_write16(&f->d, f->pm + PCI_PM_CTRL, pmcsr & ~PCI_PM_STATE_MASK);
                udelay(10000);          /* D3hot to D0: 10 ms before it is addressed */
            }
        }
        for (int i = f->extra - 1; i >= 0; i--)
            if (pci_read32(&f->d, 0x40 + i * 4) != f->cfg[16 + i])
                pci_write32(&f->d, 0x40 + i * 4, f->cfg[16 + i]);
        for (int i = 15; i >= 1; i--)
            if (pci_read32(&f->d, i * 4) != f->cfg[i])
                pci_write32(&f->d, i * 4, f->cfg[i]);
    }
}
