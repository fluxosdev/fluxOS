/* Minimal PCI bus enumeration, enough to find the IDE controller and the network card. */
#include "kernel.h"
#include "pci.h"
#include "x86.h"

#define CONFIG_ADDR 0xCF8
#define CONFIG_DATA 0xCFC

static uint32_t config_addr(const struct pci_dev *p, int off)
{
    return 0x80000000u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 |
           (uint32_t)p->fn << 8 | (uint32_t)(off & 0xFC);
}

uint32_t pci_read32(const struct pci_dev *p, int off)
{
    outl(CONFIG_ADDR, config_addr(p, off));
    return inl(CONFIG_DATA);
}

void pci_write32(const struct pci_dev *p, int off, uint32_t v)
{
    outl(CONFIG_ADDR, config_addr(p, off));
    outl(CONFIG_DATA, v);
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
