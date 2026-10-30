/*
 * Intel PRO/1000 gigabit Ethernet ("e1000", after the Linux driver): the
 * 8254x PCI and PCI-X controllers (82540, 82541, 82543, 82544, 82545,
 * 82546, 82547) and the 82574L, the card QEMU, VirtualBox and many
 * emulated machines offer. Intel's I217 is e1000e.c's.
 *
 * Reset leaves the MAC and its PHY negotiating on their own: setting
 * "link up" in the device control register is all the PHY needs. Frames
 * move through two rings of 16-byte legacy descriptors in memory: the
 * card writes received frames into our buffers and sets their "done" bit,
 * and sends the transmit descriptors between its head and the tail we
 * move, reporting each done as it goes (RS).
 *
 * The interrupt handler passes received frames up; the timer looks at the
 * rings and the link as well, so a lost interrupt costs only latency.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "mm.h"
#include "net.h"
#include "pci.h"

#define R_CTRL      0x0000
#define R_STATUS    0x0008
#define R_EERD      0x0014
#define R_CTRL_EXT  0x0018
#define R_ICR       0x00C0
#define R_ITR       0x00C4
#define R_IMS       0x00D0
#define R_IMC       0x00D8
#define R_RCTL      0x0100
#define R_TCTL      0x0400
#define R_TIPG      0x0410
#define R_RDBAL     0x2800
#define R_RDBAH     0x2804
#define R_RDLEN     0x2808
#define R_RDH       0x2810
#define R_RDT       0x2818
#define R_RDTR      0x2820
#define R_TDBAL     0x3800
#define R_TDBAH     0x3804
#define R_TDLEN     0x3808
#define R_TDH       0x3810
#define R_TDT       0x3818
#define R_MTA       0x5200
#define R_RAL0      0x5400
#define R_RAH0      0x5404

#define CTRL_FD     (1u << 0)
#define CTRL_LRST   (1u << 3)
#define CTRL_ASDE   (1u << 5)
#define CTRL_SLU    (1u << 6)
#define CTRL_ILOS   (1u << 7)
#define CTRL_RST    (1u << 26)
#define CTRL_VME    (1u << 30)
#define CTRL_PHY_RST (1u << 31)

#define STATUS_FD   (1u << 0)
#define STATUS_LU   (1u << 1)

#define ICR_TXDW    (1u << 0)
#define ICR_LSC     (1u << 2)
#define ICR_RXDMT0  (1u << 4)
#define ICR_RXO     (1u << 6)
#define ICR_RXT0    (1u << 7)
#define IRQ_MASK    (ICR_TXDW | ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXT0)

#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15)          /* broadcasts */
#define RCTL_SECRC  (1u << 26)          /* strip the CRC */
                                        /* (buffer size 2048: BSIZE 0, BSEX 0) */
#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)
#define TCTL_CT     (0x0Fu << 4)
#define TCTL_COLD   (0x40u << 12)       /* full duplex */

#define TX_EOP      0x01
#define TX_IFCS     0x02
#define TX_RS       0x08
#define DESC_DD     0x01                /* status: done */
#define RX_EOP      0x02

#define RX_RING     32
#define TX_RING     32
#define BUF_SIZE    2048

struct rx_desc {
    volatile uint32_t addr_lo, addr_hi;
    volatile uint16_t length, csum;
    volatile uint8_t status, errors;
    volatile uint16_t special;
};

struct tx_desc {
    volatile uint32_t addr_lo, addr_hi;
    volatile uint16_t length;
    volatile uint8_t cso, cmd, status, css;
    volatile uint16_t special;
};

static const struct {
    uint16_t id;
    const char *name;
    int new_eerd;                       /* EERD's address at bit 2 and done at bit 1 (else 8 and 4) */
} chips[] = {
    { 0x1000, "82542", 0 },  { 0x1001, "82543GC", 0 }, { 0x1004, "82543GC", 0 }, { 0x1008, "82544EI", 0 },
    { 0x1009, "82544EI", 0 }, { 0x100C, "82544GC", 0 }, { 0x100D, "82544GC", 0 }, { 0x100E, "82540EM", 0 },
    { 0x100F, "82545EM", 0 }, { 0x1010, "82546EB", 0 }, { 0x1011, "82545EM", 0 }, { 0x1012, "82546EB", 0 },
    { 0x1013, "82541EI", 1 }, { 0x1014, "82541ER", 1 }, { 0x1015, "82540EM", 0 }, { 0x1016, "82540EP", 0 },
    { 0x1017, "82540EP", 0 }, { 0x1018, "82541EI", 1 }, { 0x1019, "82547EI", 1 }, { 0x101A, "82547EI", 1 },
    { 0x101D, "82546EB", 0 }, { 0x101E, "82540EP", 0 }, { 0x1026, "82545GM", 0 }, { 0x1027, "82545GM", 0 },
    { 0x1028, "82545GM", 0 }, { 0x1075, "82547GI", 1 }, { 0x1076, "82541GI", 1 }, { 0x1077, "82541GI", 1 },
    { 0x1078, "82541ER", 1 }, { 0x1079, "82546GB", 0 }, { 0x107A, "82546GB", 0 }, { 0x107B, "82546GB", 0 },
    { 0x107C, "82541PI", 1 }, { 0x108A, "82546GB", 0 }, { 0x1099, "82546GB", 0 }, { 0x10B5, "82546GB", 0 },
    { 0x10D3, "82574L", 1 },  { 0x10F6, "82574LA", 1 },
};

static struct {
    volatile uint8_t *regs;
    const char *chip;
    int new_eerd, irq, link_up, polls;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t *rx_buf[RX_RING], *tx_buf[TX_RING];
    uint32_t rx_next;                   /* the next descriptor the card fills */
    uint32_t tx_next, tx_done;          /* free-running */
    struct netdev dev;
} e;

static inline uint32_t rd(int reg)
{
    return *(volatile uint32_t *)(e.regs + reg);
}

static inline void wr(int reg, uint32_t v)
{
    *(volatile uint32_t *)(e.regs + reg) = v;
}

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* A word of the EEPROM through EERD; -1 if it does not answer. */
static int eeprom_read(int word)
{
    uint32_t start = e.new_eerd ? (uint32_t)word << 2 | 1 : (uint32_t)word << 8 | 1;
    uint32_t done = e.new_eerd ? 1u << 1 : 1u << 4;
    wr(R_EERD, start);
    for (int i = 0; i < 1000; i++) {
        uint32_t v = rd(R_EERD);
        if (v & done)
            return (int)(v >> 16);
        udelay(5);
    }
    return -1;
}

static int valid_mac(const uint8_t *m)
{
    return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

/* The address in the first receive-address register (the BIOS or the card's reset loads it there),
 * or else from the EEPROM. */
static void read_mac(uint8_t *mac)
{
    uint32_t lo = rd(R_RAL0), hi = rd(R_RAH0);
    for (int i = 0; i < 4; i++)
        mac[i] = (uint8_t)(lo >> (8 * i));
    mac[4] = (uint8_t)hi, mac[5] = (uint8_t)(hi >> 8);
    if (valid_mac(mac))
        return;
    for (int i = 0; i < 3; i++) {
        int w = eeprom_read(i);
        if (w < 0)
            return;
        mac[2 * i] = (uint8_t)w, mac[2 * i + 1] = (uint8_t)(w >> 8);
    }
}

static void write_mac(const uint8_t *mac)
{
    wr(R_RAL0, (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24);
    wr(R_RAH0, (uint32_t)mac[4] | (uint32_t)mac[5] << 8 | 1u << 31);      /* address valid */
}

static void update_link(void)
{
    int up = (rd(R_STATUS) & STATUS_LU) != 0;
    if (up != e.link_up) {
        e.link_up = up;
        net_link_changed(up);
    }
}

/* ---- sending and receiving ---- */

static void tx_reclaim(void)
{
    while (e.tx_done != e.tx_next) {
        struct tx_desc *d = &e.tx[e.tx_done % TX_RING];
        if (!(d->status & DESC_DD))
            break;
        e.tx_done++;
    }
}

static int e1000_transmit(const void *frame, uint32_t len)
{
    tx_reclaim();
    if (e.tx_next - e.tx_done >= TX_RING - 1 || len > ETH_FRAME_MAX)
        return -EAGAIN;
    uint32_t i = e.tx_next % TX_RING, n = len < 60 ? 60 : len;     /* Ethernet's minimum frame */
    memcpy(e.tx_buf[i], frame, len);
    memset(e.tx_buf[i] + len, 0, n - len);
    struct tx_desc *d = &e.tx[i];
    d->length = (uint16_t)n;
    d->cso = d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = TX_EOP | TX_IFCS | TX_RS;
    barrier();
    e.tx_next++;
    wr(R_TDT, e.tx_next % TX_RING);
    return 0;
}

static void rx_process(void)
{
    for (int n = 0; n < RX_RING; n++) {
        uint32_t i = e.rx_next;
        struct rx_desc *d = &e.rx[i];
        if (!(d->status & DESC_DD))
            break;
        barrier();
        uint32_t len = d->length;
        if ((d->status & RX_EOP) && !d->errors && len >= ETH_HLEN && len <= BUF_SIZE)
            net_receive(e.rx_buf[i], len);
        else
            netif.rx_errors++;
        d->status = 0;
        barrier();
        wr(R_RDT, i);                   /* the card may fill it again */
        e.rx_next = (i + 1) % RX_RING;
    }
}

static void e1000_irq(struct trapframe *tf)
{
    (void)tf;
    uint32_t cause = rd(R_ICR);         /* reading it acknowledges it */
    if (!cause)
        return;                         /* another device on a shared line */
    spin_lock(&net_lock);
    if (cause & (ICR_RXT0 | ICR_RXDMT0 | ICR_RXO))
        rx_process();
    if (cause & ICR_RXO)
        netif.rx_dropped++;
    if (cause & ICR_TXDW)
        tx_reclaim();
    if (cause & ICR_LSC)
        update_link();
    spin_unlock(&net_lock);
}

/* From the timer: anything the interrupt missed, and the link every 2 s. */
static void e1000_poll(void)
{
    rx_process();
    tx_reclaim();
    if (++e.polls >= 20) {
        e.polls = 0;
        update_link();
    }
}

/* ---- setup ---- */

static int alloc_buffers(void)
{
    uint32_t page = pmm_alloc();
    if (!page)
        return -1;
    memset((void *)page, 0, PAGE_SIZE);
    e.rx = (struct rx_desc *)page;                      /* 32 x 16 bytes each */
    e.tx = (struct tx_desc *)(page + 2048);
    for (int i = 0; i < RX_RING + TX_RING; i += 2) {
        uint32_t p = pmm_alloc();
        if (!p)
            return -1;
        uint8_t **slot = i < RX_RING ? &e.rx_buf[i] : &e.tx_buf[i - RX_RING];
        slot[0] = (uint8_t *)p;
        slot[1] = (uint8_t *)p + BUF_SIZE;
    }
    return 0;
}

/* Reset the controller and bring it up with empty rings (at start, and after suspend to RAM). */
static void start(void)
{
    wr(R_IMC, 0xFFFFFFFF);
    wr(R_RCTL, 0);
    wr(R_TCTL, TCTL_PSP);
    udelay(10000);
    wr(R_CTRL, rd(R_CTRL) | CTRL_RST);
    udelay(10000);
    for (int i = 0; i < 100 && (rd(R_CTRL) & CTRL_RST); i++)
        udelay(1000);
    wr(R_IMC, 0xFFFFFFFF);
    rd(R_ICR);
    uint32_t ctrl = rd(R_CTRL);
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS | CTRL_VME);
    wr(R_CTRL, ctrl | CTRL_SLU | CTRL_ASDE);            /* the PHY negotiates; the MAC follows it */
    write_mac(e.dev.mac);
    for (int i = 0; i < 128; i++)
        wr(R_MTA + 4 * i, 0);

    for (int i = 0; i < RX_RING; i++) {
        e.rx[i] = (struct rx_desc){ .addr_lo = (uint32_t)e.rx_buf[i] };
    }
    for (int i = 0; i < TX_RING; i++)
        e.tx[i] = (struct tx_desc){ .addr_lo = (uint32_t)e.tx_buf[i], .status = DESC_DD };
    e.rx_next = 0;
    e.tx_next = e.tx_done = 0;

    wr(R_RDBAL, (uint32_t)e.rx);
    wr(R_RDBAH, 0);
    wr(R_RDLEN, RX_RING * sizeof(struct rx_desc));
    wr(R_RDH, 0);
    wr(R_RDT, RX_RING - 1);
    wr(R_RDTR, 0);
    wr(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    wr(R_TDBAL, (uint32_t)e.tx);
    wr(R_TDBAH, 0);
    wr(R_TDLEN, TX_RING * sizeof(struct tx_desc));
    wr(R_TDH, 0);
    wr(R_TDT, 0);
    wr(R_TIPG, 10 | 8 << 10 | 6 << 20);                 /* the copper inter-packet gaps */
    wr(R_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    wr(R_ITR, 0);
    e.link_up = -1;
    update_link();
    if (e.irq >= 0)
        wr(R_IMS, IRQ_MASK);
}

static void e1000_suspend(void)
{
    wr(R_IMC, 0xFFFFFFFF);
    wr(R_RCTL, 0);
    wr(R_TCTL, TCTL_PSP);
}

static void e1000_resume(void)
{
    start();
}

int e1000_init(void)
{
    if (cmdline_has("e1000=off"))
        return -1;
    struct pci_dev pci;
    int found = -1;
    for (uint32_t i = 0; i < ARRAY_SIZE(chips) && found < 0; i++)
        if (pci_find_device(0x8086, chips[i].id, &pci) == 0)
            found = (int)i;
    if (found < 0)
        return -1;
    e.chip = chips[found].name;
    e.new_eerd = chips[found].new_eerd;
    strcpy(e.dev.name, "eth0");
    e.regs = pci_map_bar(&pci, PCI_BAR0);
    int irq = pci_irq(&pci);
    if (!e.regs || alloc_buffers() < 0) {
        boot_warn("Network: cannot set up the Intel %s controller", e.chip);
        return -1;
    }
    pci_enable(&pci, PCI_CMD_MEM | PCI_CMD_MASTER);
    read_mac(e.dev.mac);
    e.irq = irq;
    start();
    snprintf(e.dev.driver, sizeof(e.dev.driver), "e1000 (Intel %s)", e.chip);
    e.dev.irq = irq >= 0 ? (uint32_t)irq : 0;
    e.dev.transmit = e1000_transmit;
    e.dev.poll = e1000_poll;
    e.dev.suspend = e1000_suspend;
    e.dev.resume = e1000_resume;
    net_register(&e.dev);
    if (irq >= 0) {
        irq_register(irq, e1000_irq);
        wr(R_IMS, IRQ_MASK);
    }
    uint8_t *m = e.dev.mac;
    boot_ok("Network: Intel %s Ethernet, IRQ %d, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s", e.chip, irq,
            m[0], m[1], m[2], m[3], m[4], m[5], e.link_up ? "up" : "down (negotiating)");
    return 0;
}
