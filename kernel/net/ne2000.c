/*
 * NE2000-compatible Ethernet cards, built around the National
 * Semiconductor DP8390: the PCI RTL8029 (QEMU's ne2k_pci) and its
 * clones, or an ISA card (QEMU's ne2k_isa) at 0x300 or 0x280.
 *
 * The card has 16 KiB of buffer memory, split into one transmit buffer
 * and a ring of 256-byte pages that the card fills with received frames.
 * Frames move between that memory and the CPU through the "remote DMA"
 * data port, one word at a time.
 *
 * Kernel options: ne2k.io=0x300 and ne2k.irq=9 locate an ISA card;
 * ne2k.off disables the driver.
 */
#include "kernel.h"
#include "arch.h"
#include "net.h"
#include "pci.h"
#include "x86.h"

/* Registers (page 0 unless noted) */
#define NE_CR     0x00
#define NE_PSTART 0x01          /* write */
#define NE_PSTOP  0x02          /* write */
#define NE_BNRY   0x03
#define NE_TPSR   0x04          /* write */
#define NE_TBCR0  0x05          /* write */
#define NE_TBCR1  0x06          /* write */
#define NE_ISR    0x07
#define NE_RSAR0  0x08          /* write */
#define NE_RSAR1  0x09          /* write */
#define NE_RBCR0  0x0A          /* write */
#define NE_RBCR1  0x0B          /* write */
#define NE_RCR    0x0C          /* write */
#define NE_TCR    0x0D          /* write */
#define NE_DCR    0x0E          /* write */
#define NE_IMR    0x0F          /* write */
#define NE_PAR0   0x01          /* page 1: our MAC address */
#define NE_CURR   0x07          /* page 1: the page the card writes next */
#define NE_MAR0   0x08          /* page 1: multicast filter */
#define NE_DATA   0x10          /* remote DMA data port */
#define NE_RESET  0x1F

#define CR_STP    0x01
#define CR_STA    0x02
#define CR_TXP    0x04
#define CR_RREAD  0x08
#define CR_RWRITE 0x10
#define CR_NODMA  0x20
#define CR_PAGE1  0x40

#define ISR_PRX 0x01            /* frame received */
#define ISR_PTX 0x02            /* frame sent */
#define ISR_RXE 0x04            /* receive error */
#define ISR_TXE 0x08            /* transmit error */
#define ISR_OVW 0x10            /* receive ring overflow */
#define ISR_CNT 0x20            /* error counter overflow */
#define ISR_RDC 0x40            /* remote DMA complete */
#define ISR_RST 0x80            /* reset (or stopped) */

#define DCR_BYTE 0x48           /* normal operation, 8-byte FIFO threshold */
#define DCR_WORD 0x49           /* the same with 16-bit transfers */
#define RCR_BROADCAST 0x04
#define RCR_MONITOR   0x20
#define TCR_LOOPBACK  0x02

/* Buffer memory, in 256-byte pages: a 1536-byte transmit buffer, then the ring. */
#define TX_PAGE  0x40
#define RX_START 0x46
#define RX_STOP  0x80

static uint16_t io;
static int wordmode;
static uint8_t next_pkt;                /* the ring page holding the next unread frame */
static struct netdev dev;
static uint8_t rxbuf[ETH_FRAME_MAX + 8];
static uint8_t txbuf[ETH_FRAME_MAX + 2];

static const struct {
    uint16_t vendor, device;
    const char *name;
} pci_ids[] = {
    { 0x10EC, 0x8029, "Realtek RTL8029" },
    { 0x1050, 0x0940, "Winbond 89C940" },
    { 0x1050, 0x5A5A, "Winbond W89C940F" },
    { 0x11F6, 0x1401, "Compex RL2000" },
    { 0x8E2E, 0x3000, "KTI ET32P2" },
    { 0x4A14, 0x5000, "NetVin NV5000SC" },
    { 0x1106, 0x0926, "VIA 86C926" },
    { 0x10BD, 0x0E34, "SureCom NE34" },
    { 0x12C3, 0x0058, "Holtek HT80232" },
    { 0x12C3, 0x5598, "Holtek HT80229" },
};

static void ne_out(int reg, uint8_t v) { outb((uint16_t)(io + reg), v); }
static uint8_t ne_in(int reg) { return inb((uint16_t)(io + reg)); }

static void wait_rdc(void)
{
    for (int i = 0; i < 100000 && !(ne_in(NE_ISR) & ISR_RDC); i++)
        ;
    ne_out(NE_ISR, ISR_RDC);
}

static void set_remote(uint16_t addr, uint16_t count)
{
    ne_out(NE_RBCR0, (uint8_t)count);
    ne_out(NE_RBCR1, (uint8_t)(count >> 8));
    ne_out(NE_RSAR0, (uint8_t)addr);
    ne_out(NE_RSAR1, (uint8_t)(addr >> 8));
}

/* Copy count bytes of card memory at addr into buf. */
static void remote_read(uint16_t addr, uint8_t *buf, uint16_t count)
{
    set_remote(addr, wordmode ? (uint16_t)((count + 1) & ~1) : count);
    ne_out(NE_CR, CR_STA | CR_RREAD);
    if (wordmode) {
        insw((uint16_t)(io + NE_DATA), buf, count / 2u);
        if (count & 1)
            buf[count - 1] = (uint8_t)inw((uint16_t)(io + NE_DATA));
    } else {
        for (uint16_t i = 0; i < count; i++)
            buf[i] = inb((uint16_t)(io + NE_DATA));
    }
    wait_rdc();
}

/* count must be even in word mode. */
static void remote_write(uint16_t addr, const uint8_t *buf, uint16_t count)
{
    ne_out(NE_ISR, ISR_RDC);
    set_remote(addr, count);
    ne_out(NE_CR, CR_STA | CR_RWRITE);
    if (wordmode) {
        outsw((uint16_t)(io + NE_DATA), buf, count / 2u);
    } else {
        for (uint16_t i = 0; i < count; i++)
            outb((uint16_t)(io + NE_DATA), buf[i]);
    }
    wait_rdc();
}

static int ne_transmit(const void *frame, uint32_t len)
{
    if (len > ETH_FRAME_MAX)
        return -EMSGSIZE;
    /* There is one transmit buffer: wait for the previous frame to leave. */
    for (int i = 0; i < 1000000 && (ne_in(NE_CR) & CR_TXP); i++)
        ;
    uint32_t n = len < 60 ? 60 : len;  /* Ethernet's minimum frame */
    memcpy(txbuf, frame, len);
    memset(txbuf + len, 0, n + 1 - len);
    remote_write(TX_PAGE << 8, txbuf, (uint16_t)(wordmode ? (n + 1) & ~1u : n));
    ne_out(NE_TPSR, TX_PAGE);
    ne_out(NE_TBCR0, (uint8_t)n);
    ne_out(NE_TBCR1, (uint8_t)(n >> 8));
    ne_out(NE_CR, CR_STA | CR_TXP | CR_NODMA);
    return 0;
}

static uint8_t current_page(void)
{
    ne_out(NE_CR, CR_PAGE1 | CR_STA | CR_NODMA);
    uint8_t curr = ne_in(NE_CURR);
    ne_out(NE_CR, CR_STA | CR_NODMA);
    return curr;
}

/* Pass every frame in the ring up to the network stack. */
static void receive(void)
{
    uint8_t curr;
    while (next_pkt != (curr = current_page())) {
        /* Each frame starts with a header: status, next page, byte count. */
        uint8_t hdr[4];
        remote_read((uint16_t)(next_pkt << 8), hdr, 4);
        uint8_t status = hdr[0], next = hdr[1];
        uint32_t count = (uint32_t)hdr[2] | (uint32_t)hdr[3] << 8;
        if (next < RX_START || next >= RX_STOP || count < 4 + ETH_HLEN ||
            count > 4 + sizeof(rxbuf)) {
            /* The ring is inconsistent: skip everything in it. */
            klog("ne2000: bad ring header (page %02x next %02x count %u)\n", next_pkt, next, count);
            netif.rx_errors++;
            next = curr;
        } else if (status & 0x01) {
            uint32_t len = count - 4, addr = (uint32_t)next_pkt * 256 + 4;
            uint32_t first = MIN(len, (uint32_t)RX_STOP * 256 - addr);
            remote_read((uint16_t)addr, rxbuf, (uint16_t)first);
            if (first < len)            /* the frame wraps around the end of the ring */
                remote_read(RX_START << 8, rxbuf + first, (uint16_t)(len - first));
            net_receive(rxbuf, len);
        } else {
            netif.rx_errors++;
        }
        next_pkt = next;
        ne_out(NE_BNRY, next == RX_START ? RX_STOP - 1 : next - 1);
    }
}

/* The ring filled up: the DP8390 data sheet's recovery procedure. */
static void overflow(void)
{
    int was_sending = ne_in(NE_CR) & CR_TXP;
    ne_out(NE_CR, CR_STP | CR_NODMA);
    for (int i = 0; i < 2000; i++)      /* at least 1.6 ms */
        io_wait();
    ne_out(NE_RBCR0, 0);
    ne_out(NE_RBCR1, 0);
    int resend = was_sending && !(ne_in(NE_ISR) & (ISR_PTX | ISR_TXE));
    ne_out(NE_TCR, TCR_LOOPBACK);
    ne_out(NE_CR, CR_STA | CR_NODMA);
    receive();
    ne_out(NE_ISR, ISR_OVW);
    ne_out(NE_TCR, 0);
    if (resend)
        ne_out(NE_CR, CR_STA | CR_TXP | CR_NODMA);
    netif.rx_dropped++;
}

static void ne_irq(struct trapframe *tf)
{
    (void)tf;
    spin_lock(&net_lock);
    uint8_t isr;
    /* PCI interrupts are level-triggered: clear every cause before returning. */
    for (int round = 0; round < 16 && (isr = ne_in(NE_ISR) & 0x3F); round++) {
        if (isr & ISR_OVW) {
            overflow();
        } else if (isr & (ISR_PRX | ISR_RXE)) {
            ne_out(NE_ISR, isr & (ISR_PRX | ISR_RXE));
            if (isr & ISR_RXE)
                netif.rx_errors++;
            receive();
        }
        if (isr & ISR_TXE)
            netif.tx_errors++;
        ne_out(NE_ISR, isr & (ISR_PTX | ISR_TXE | ISR_CNT));
    }
    spin_unlock(&net_lock);
}

/* Reset the card and read its MAC address. Returns 0, or -1 if nothing answers. */
static int ne_reset(int pci)
{
    if (ne_in(NE_CR) == 0xFF)
        return -1;                      /* an empty ISA port */
    outb((uint16_t)(io + NE_RESET), inb((uint16_t)(io + NE_RESET)));
    int i;
    for (i = 0; i < 100000 && !(ne_in(NE_ISR) & ISR_RST); i++)
        ;
    if (i == 100000)
        return -1;
    ne_out(NE_ISR, 0xFF);
    ne_out(NE_CR, CR_STP | CR_NODMA);
    ne_out(NE_DCR, DCR_BYTE);
    ne_out(NE_RBCR0, 0);
    ne_out(NE_RBCR1, 0);
    ne_out(NE_IMR, 0);
    ne_out(NE_RCR, RCR_MONITOR);
    ne_out(NE_TCR, TCR_LOOPBACK);

    /* The station address PROM: 16 bytes, each one doubled in byte mode. */
    uint8_t prom[32];
    wordmode = 0;
    remote_read(0, prom, sizeof(prom));
    for (int k = 0; k < ETH_ALEN; k++)
        dev.mac[k] = prom[2 * k];
    /* Bytes 14 and 15 say "WW" on a 16-bit NE2000, "BB" on an 8-bit NE1000. */
    if (prom[28] == 0x57 && prom[30] == 0x57)
        wordmode = 1;
    else if (prom[28] == 0x42 && prom[30] == 0x42)
        wordmode = 0;
    else if (pci)
        wordmode = 1;
    else
        return -1;
    return 0;
}

static void ne_start(void)
{
    ne_out(NE_DCR, wordmode ? DCR_WORD : DCR_BYTE);
    ne_out(NE_TPSR, TX_PAGE);
    ne_out(NE_PSTART, RX_START);
    ne_out(NE_BNRY, RX_START);
    ne_out(NE_PSTOP, RX_STOP);
    ne_out(NE_CR, CR_PAGE1 | CR_STP | CR_NODMA);
    for (int k = 0; k < ETH_ALEN; k++)
        ne_out(NE_PAR0 + k, dev.mac[k]);
    for (int k = 0; k < 8; k++)
        ne_out(NE_MAR0 + k, 0);
    ne_out(NE_CURR, RX_START + 1);
    next_pkt = RX_START + 1;
    ne_out(NE_CR, CR_STA | CR_NODMA);
    ne_out(NE_ISR, 0xFF);
    ne_out(NE_IMR, ISR_PRX | ISR_RXE | ISR_TXE | ISR_OVW);
    ne_out(NE_TCR, 0);
    ne_out(NE_RCR, RCR_BROADCAST);
}

static uint32_t parse_number(const char *s)
{
    uint32_t v = 0, base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (; *s; s++) {
        uint32_t d = *s >= '0' && *s <= '9' ? (uint32_t)(*s - '0')
                   : *s >= 'a' && *s <= 'f' ? (uint32_t)(*s - 'a' + 10)
                   : *s >= 'A' && *s <= 'F' ? (uint32_t)(*s - 'A' + 10) : 99;
        if (d >= base)
            break;
        v = v * base + d;
    }
    return v;
}

static int find_pci(const char **name, uint32_t *irq)
{
    for (uint32_t i = 0; i < ARRAY_SIZE(pci_ids); i++) {
        struct pci_dev p;
        if (pci_find_device(pci_ids[i].vendor, pci_ids[i].device, &p) < 0)
            continue;
        uint32_t bar = pci_read32(&p, PCI_BAR0);
        uint32_t line = pci_read32(&p, PCI_INTERRUPT) & 0xFF;
        if (!(bar & 1) || line == 0 || line >= 16) {
            klog("ne2000: %s has no usable I/O port or IRQ\n", pci_ids[i].name);
            continue;
        }
        pci_write32(&p, PCI_COMMAND, pci_read32(&p, PCI_COMMAND) | PCI_CMD_IO);
        io = (uint16_t)(bar & ~3u);
        *name = pci_ids[i].name;
        *irq = line;
        if (ne_reset(1) == 0)
            return 0;
        klog("ne2000: %s at %04x does not respond\n", *name, io);
    }
    return -1;
}

static int find_isa(uint32_t *irq)
{
    static const uint16_t ports[] = { 0x300, 0x280 };
    char opt[16];
    *irq = cmdline_value("ne2k.irq", opt, sizeof(opt)) ? parse_number(opt) : 9;
    if (cmdline_value("ne2k.io", opt, sizeof(opt))) {
        io = (uint16_t)parse_number(opt);
        return ne_reset(0);
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
        io = ports[i];
        if (ne_reset(0) == 0)
            return 0;
    }
    return -1;
}

int ne2000_init(void)
{
    if (cmdline_has("ne2k.off"))
        return -1;
    const char *name = "ISA NE2000";
    uint32_t irq;
    if (find_pci(&name, &irq) < 0 && find_isa(&irq) < 0)
        return -1;
    if (irq >= 16)
        return -1;
    ne_start();

    snprintf(dev.driver, sizeof(dev.driver), "ne2000 (%s)", name);
    dev.io_base = io;
    dev.irq = irq;
    dev.transmit = ne_transmit;
    net_register(&dev);
    irq_register((int)irq, ne_irq);
    boot_ok("Network: %s at I/O %04x, IRQ %u, MAC %02x:%02x:%02x:%02x:%02x:%02x%s",
            name, io, irq, dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5],
            wordmode ? "" : " (8-bit)");
    return 0;
}
