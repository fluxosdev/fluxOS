/* Minimal PCI bus enumeration, enough to find the IDE controller. */
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

int pci_find_class(uint16_t class_sub, struct pci_dev *out, uint8_t *prog_if)
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
                uint32_t cls = pci_read32(&p, 0x08) >> 8;   /* class, sub, prog-if */
                if ((cls >> 8) == class_sub) {
                    *out = p;
                    *prog_if = (uint8_t)cls;
                    return 0;
                }
            }
        }
    }
    return -1;
}
