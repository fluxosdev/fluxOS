/* Minimal PCI bus enumeration, enough to find the storage, USB and network controllers. */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "x86.h"

#define CONFIG_ADDR 0xCF8
#define CONFIG_DATA 0xCFC

/* Each access is two port writes or reads: they must not interleave. */
static spinlock_t pci_lock = SPINLOCK_INIT("pci");

static uint32_t config_addr(const struct pci_dev *p, int off)
{
    return 0x80000000u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 |
           (uint32_t)p->fn << 8 | (uint32_t)(off & 0xFC);
}

uint32_t pci_read32(const struct pci_dev *p, int off)
{
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    uint32_t v = inl(CONFIG_DATA);
    spin_unlock(&pci_lock);
    return v;
}

void pci_write32(const struct pci_dev *p, int off, uint32_t v)
{
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    outl(CONFIG_DATA, v);
    spin_unlock(&pci_lock);
}

uint16_t pci_read16(const struct pci_dev *p, int off)
{
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    uint16_t v = inw((uint16_t)(CONFIG_DATA + (off & 2)));
    spin_unlock(&pci_lock);
    return v;
}

void pci_write16(const struct pci_dev *p, int off, uint16_t v)
{
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    outw((uint16_t)(CONFIG_DATA + (off & 2)), v);
    spin_unlock(&pci_lock);
}

uint8_t pci_read8(const struct pci_dev *p, int off)
{
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    uint8_t v = inb((uint16_t)(CONFIG_DATA + (off & 3)));
    spin_unlock(&pci_lock);
    return v;
}

void pci_write8(const struct pci_dev *p, int off, uint8_t v)
{
    spin_lock(&pci_lock);
    outl(CONFIG_ADDR, config_addr(p, off));
    outb((uint16_t)(CONFIG_DATA + (off & 3)), v);
    spin_unlock(&pci_lock);
}

/* Call match on every function present; stop at the first that returns 1. */
static int pci_scan(int (*match)(const struct pci_dev *p, const void *arg), const void *arg,
                    struct pci_dev *out)
{
    /* Is there a PCI host bridge answering mechanism #1 at all? */
    outl(CONFIG_ADDR, 0x80000000u);
    if (inl(CONFIG_ADDR) != 0x80000000u)
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
 * Intel's and most others'), which the drives' DMA modes depend on.
 * Bridges come before what is behind them, in the order they were found.
 */
#define MAX_SAVED 96
#define IDE_DWORDS 8

static struct saved_fn {
    struct pci_dev d;
    int ide;
    uint32_t cfg[16 + IDE_DWORDS];
} saved[MAX_SAVED];
static int nsaved;

static int save_fn(const struct pci_dev *p, const void *arg)
{
    (void)arg;
    if (nsaved == MAX_SAVED)
        return 1;
    struct saved_fn *f = &saved[nsaved++];
    f->d = *p;
    for (int i = 0; i < 16; i++)
        f->cfg[i] = pci_read32(p, i * 4);
    f->ide = (f->cfg[2] >> 16) == 0x0101;
    for (int i = 0; f->ide && i < IDE_DWORDS; i++)
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
        for (int i = f->ide ? IDE_DWORDS - 1 : -1; i >= 0; i--)
            if (pci_read32(&f->d, 0x40 + i * 4) != f->cfg[16 + i])
                pci_write32(&f->d, 0x40 + i * 4, f->cfg[16 + i]);
        for (int i = 15; i >= 1; i--)
            if (pci_read32(&f->d, i * 4) != f->cfg[i])
                pci_write32(&f->d, i * 4, f->cfg[i]);
    }
}
