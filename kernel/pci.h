/* PCI configuration space access (mechanism #1, I/O ports 0xCF8/0xCFC). */
#ifndef PCI_H
#define PCI_H

#include <stdint.h>

struct pci_dev {
    uint8_t bus, dev, fn;
};

#define PCI_COMMAND   0x04
#define PCI_BAR4      0x20

#define PCI_CMD_IO      0x1
#define PCI_CMD_MASTER  0x4

uint32_t pci_read32(const struct pci_dev *p, int off);
void pci_write32(const struct pci_dev *p, int off, uint32_t v);

/* Find the first device whose class code (class << 8 | subclass) matches.
 * Fills in the programming interface byte. Returns 0, or -1 if none. */
int pci_find_class(uint16_t class_sub, struct pci_dev *out, uint8_t *prog_if);

#endif
