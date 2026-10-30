/* PCI configuration space access (mechanism #1, I/O ports 0xCF8/0xCFC). */
#ifndef PCI_H
#define PCI_H

#include <stdint.h>

struct pci_dev {
    uint8_t bus, dev, fn;
};

#define PCI_COMMAND   0x04
#define PCI_BAR0      0x10
#define PCI_BAR1      0x14
#define PCI_BAR2      0x18
#define PCI_BAR4      0x20
#define PCI_BAR5      0x24
#define PCI_INTERRUPT 0x3C          /* low byte: the IRQ line the BIOS routed */

#define PCI_CMD_IO      0x1
#define PCI_CMD_MEM     0x2
#define PCI_CMD_MASTER  0x4

uint32_t pci_read32(const struct pci_dev *p, int off);
void pci_write32(const struct pci_dev *p, int off, uint32_t v);
uint16_t pci_read16(const struct pci_dev *p, int off);
void pci_write16(const struct pci_dev *p, int off, uint16_t v);
uint8_t pci_read8(const struct pci_dev *p, int off);
void pci_write8(const struct pci_dev *p, int off, uint8_t v);

/* Find the first device whose class code (class << 8 | subclass) matches.
 * Fills in the programming interface byte. Returns 0, or -1 if none. */
int pci_find_class(uint16_t class_sub, struct pci_dev *out, uint8_t *prog_if);

/* Find the first device with this vendor and device ID. Returns 0, or -1. */
int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out);

/* List every device in the kernel log (dmesg | grep pci). */
void pci_log_devices(void);
/* Read-only diagnostics: with a device, it and the bridges above it;
 * with NULL, every network controller and every PCI bridge. */
void pci_log_network_diagnostics(const struct pci_dev *dev);

/* The bridge whose secondary bus the device is on. Returns 0, or -1 on
 * bus 0 (behind the host bridge) or when no configured bridge leads there. */
int pci_parent_bridge(const struct pci_dev *p, struct pci_dev *out);

/* The n-th (from 0) device of a class, for drivers that take them all. */
int pci_find_class_nth(uint16_t class_sub, int n, struct pci_dev *out, uint8_t *prog_if);

/* Set bits in the command register (memory/I/O decoding, bus mastering). */
void pci_enable(const struct pci_dev *p, uint32_t cmd_bits);
/* The same on every bridge between the device and the host bridge, which
 * must forward its MMIO and DMA; firmware may have left them off. */
void pci_enable_upstream(const struct pci_dev *p, uint32_t cmd_bits);

/* Bring a function in D1-D3hot to D0, putting back its standard header,
 * which it loses on waking unless it has No_Soft_Reset. Returns the state
 * it was in (0-3), or -1 if it has no power management. Waits 10 ms. */
int pci_set_d0(const struct pci_dev *p);

/* The IRQ line the firmware routed to the device, or -1 if it has none. */
int pci_irq(const struct pci_dev *p);

/*
 * Map a memory BAR (32-bit, or 64-bit below 4 GiB) into the kernel's
 * device memory window, uncached, and turn on memory decoding. Returns
 * NULL if the BAR is not memory or lies out of reach. Boot time only.
 */
void *pci_map_bar(const struct pci_dev *p, int bar);

/* Suspend to RAM: every function's configuration, kept and put back. */
void pci_suspend(void);
void pci_resume(void);

#endif
