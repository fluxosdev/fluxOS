/*
 * nVidia nForce network controller ("forcedeth", after the Linux driver
 * whose reverse-engineered register map this follows): the MAC built into
 * nForce, nForce2/3 and the MCP04 to MCP89 chipsets. Boards pair it with
 * an external PHY on its MII bus, often a Realtek RTL8201N (mii.c).
 *
 * Frames move through two rings of descriptors in memory: the controller
 * fills receive buffers and clears their "available" bit, and sends the
 * transmit descriptors we mark valid, clearing the bit when done. Three
 * descriptor layouts exist across the chip generations: the 8-byte one of
 * nForce/nForce2, the same size with other flag bits on nForce3, and a
 * 16-byte one with a high address word on everything newer.
 *
 * The interrupt handler passes received frames up; the timer tick looks
 * at the rings and the link as well, so a lost interrupt costs only
 * latency. Link changes (cable, speed, duplex) reprogram the MAC.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "mm.h"
#include "net.h"
#include "pci.h"

#define R_IRQSTATUS   0x000
#define R_IRQMASK     0x004
#define R_SETUP6      0x008
#define R_POLLINT     0x00C
#define R_MACRESET    0x034
#define R_MISC1       0x080
#define R_XMITCTL     0x084
#define R_XMITSTAT    0x088
#define R_PFF         0x08C
#define R_OFFLOAD     0x090
#define R_RCVCTL      0x094
#define R_RCVSTAT     0x098
#define R_SLOTTIME    0x09C
#define R_TXDEFER     0x0A0
#define R_RXDEFER     0x0A4
#define R_MACADDRA    0x0A8
#define R_MACADDRB    0x0AC
#define R_MCASTADDRA  0x0B0
#define R_MCASTADDRB  0x0B4
#define R_MCASTMASKA  0x0B8
#define R_MCASTMASKB  0x0BC
#define R_PHYIFACE    0x0C0
#define R_TXRING      0x100
#define R_RXRING      0x104
#define R_RINGSIZES   0x108
#define R_TXPOLL      0x10C
#define R_LINKSPEED   0x110
#define R_SETUP5      0x130
#define R_TXWM        0x13C
#define R_TXRXCTL     0x144
#define R_TXRINGHI    0x148
#define R_RXRINGHI    0x14C
#define R_TXPAUSE     0x170
#define R_MIISTATUS   0x180
#define R_MIIMASK     0x184
#define R_ADAPTCTL    0x188
#define R_MIISPEED    0x18C
#define R_MIICTL      0x190
#define R_MIIDATA     0x194
#define R_WAKEUP      0x200
#define R_POWERSTATE  0x26C
#define R_VLANCTL     0x300
#define R_POWERSTATE2 0x600

#define NVI_RX_ERROR  0x0001
#define NVI_RX        0x0002
#define NVI_RX_NOBUF  0x0004
#define NVI_TX_ERR    0x0008
#define NVI_TX_OK     0x0010
#define NVI_TIMER     0x0020
#define NVI_LINK      0x0040
#define NVI_RX_FORCED 0x0080
#define NVI_TX_FORCED 0x0100
#define NVI_RECOVER   0x8200
#define NVI_STAT_MASK  0x83FF
#define NVI_RX_ALL    (NVI_RX_ERROR | NVI_RX | NVI_RX_NOBUF | NVI_RX_FORCED)
#define NVI_TX_ALL    (NVI_TX_ERR | NVI_TX_OK | NVI_TX_FORCED)
#define NVI_THROUGHPUT 0x00DF

#define MISC1_HD      0x02
#define MISC1_FORCE   0x3B0F3C
#define XMITCTL_START 0x01
#define XMITSTAT_BUSY 0x01
#define RCVCTL_START  0x01
#define RCVSTAT_BUSY  0x01
#define PFF_ALWAYS    0x7F0000
#define PFF_MYADDR    0x20
#define MCASTADDRA_FORCE 0x01
#define SLOTTIME_LEGBF   0x80000000u
#define SLOTTIME_10_100  0x00007F00u
#define SLOTTIME_DEFAULT 0x00007F00u
#define TXDEFER_DEFAULT  0x15050F
#define RXDEFER_DEFAULT  0x16
#define POLL_THROUGHPUT  65535
#define SETUP6_VAL       3
#define TXPOLL_MAC_REV   0x00008000u
#define LINKSPEED_FORCE  0x10000
#define LINKSPEED_10     1000
#define LINKSPEED_100    100
#define TXWM_DESC1       0x0200010
#define TXWM_DESC23      0x0E08000      /* 10/100; 0x1E08000 is for gigabit */
#define TXPAUSE_OFF      0x0FFF0080
#define MISC1_PAUSE_TX   0x01
#define TXRXCTL_KICK     0x0001
#define TXRXCTL_BIT1     0x0002
#define TXRXCTL_BIT2     0x0004
#define TXRXCTL_RESET    0x0010
#define TXRXCTL_DESC_1   0
#define TXRXCTL_DESC_2   0x002100
#define TXRXCTL_DESC_3   0xC02200
#define SETUP5_BIT31     (1u << 31)
#define MIISTAT_ERROR    0x0001
#define MIISTAT_LINKCHANGE 0x0008
#define MIISTAT_MASK_RW  0x0007
#define MIISTAT_MASK_ALL 0x000F
#define MII_LINKCHANGE   0x0008
#define ADAPTCTL_PHYVALID 0x40000
#define ADAPTCTL_RUNNING  0x100000
#define ADAPTCTL_PHYSHIFT 24
#define MIISPEED_BIT8    (1 << 8)
#define MIIDELAY         5
#define MIICTL_INUSE     0x08000
#define MIICTL_WRITE     0x00400
#define MIICTL_ADDRSHIFT 5
#define POWER_UP         0x8000
#define POWER_VALID      0x0100
#define POWER2_UP_MASK   0x0F15
#define POWER2_UP_REV_A3 0x0001
#define POWER2_GATE      0x0F00
#define MACRESET_ASSERT  0x0F3
#define PHY_HALF         0x100
#define PHY_100          0x1
#define PHY_1000         0x2

/* Descriptor flags: version 1 (nForce, nForce2), and 2/3 (nForce3 on). */
#define F_VALID          (1u << 31)     /* TX valid and RX available, in every layout */
#define F_ERROR          (1u << 30)
#define TX1_LAST         (1u << 16)
#define TX2_LAST         (1u << 29)
#define RX1_DESCVALID    (1u << 16)
#define RX1_SUBTRACT1    (1u << 18)
#define RX1_ERROR4       (1u << 26)
#define RX1_FRAMING      (1u << 29)
#define RX1_ERRMASK      (0x7Fu << 23)
#define RX2_DESCVALID    (1u << 29)
#define RX2_SUBTRACT1    (1u << 25)
#define RX2_ERROR4       (1u << 21)
#define RX2_FRAMING      (1u << 24)
#define RX2_ERRMASK      (0x7Fu << 18)
#define LEN_MASK         0x3FFF

#define RX_RING   32
#define TX_RING   16
#define BUF_SIZE  2048
#define RX_BUF_SZ (ETH_MTU + 64)

/* Per chip: descriptor layout and quirks. */
#define C_POWER   0x1                   /* power and clock gating registers */
#define C_MACOK   0x2                   /* the MAC address registers are in order */
#define C_PHYFIX  0x4                   /* RTL8201 PHYs need the init fix */
#define C_TIMER   0x8                   /* the timer interrupt is needed */
#define C_PAUSE   0x10                  /* can send pause frames (MCP55 on) */

static const struct {
    uint16_t id;
    uint8_t ver, flags;
    const char *name;
} chips[] = {
    { 0x01C3, 1, C_TIMER, "nForce" },
    { 0x0066, 1, 0, "nForce2" }, { 0x00D6, 1, 0, "nForce2" },
    { 0x0086, 2, 0, "nForce3" }, { 0x008C, 2, 0, "nForce3" },
    { 0x00E6, 2, 0, "nForce3" }, { 0x00DF, 2, 0, "nForce3" },
    { 0x0056, 3, 0, "CK804" }, { 0x0057, 3, 0, "CK804" },
    { 0x0037, 3, 0, "MCP04" }, { 0x0038, 3, 0, "MCP04" },
    { 0x0268, 3, C_POWER, "MCP51" }, { 0x0269, 3, C_POWER, "MCP51" },
    { 0x0372, 3, C_POWER | C_PAUSE, "MCP55" }, { 0x0373, 3, C_POWER | C_PAUSE, "MCP55" },
    { 0x03E5, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP61" }, { 0x03E6, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP61" },
    { 0x03EE, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP61" }, { 0x03EF, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP61" },
    { 0x0450, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP65" },
    { 0x0451, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP65" },
    { 0x0452, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP65" },
    { 0x0453, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP65" },
    { 0x054C, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP67" },
    { 0x054D, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP67" },
    { 0x054E, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP67" },
    { 0x054F, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP67" },
    { 0x07DC, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP73" },
    { 0x07DD, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP73" },
    { 0x07DE, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP73" },
    { 0x07DF, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP73" },
    { 0x0760, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP77" },
    { 0x0761, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP77" },
    { 0x0762, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP77" },
    { 0x0763, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP77" },
    { 0x0AB0, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP79" },
    { 0x0AB1, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP79" },
    { 0x0AB2, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP79" },
    { 0x0AB3, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP79" },
    { 0x0D7D, 3, C_POWER | C_PAUSE | C_PHYFIX | C_MACOK, "MCP89" },
};

struct desc1 {
    volatile uint32_t buf, flaglen;
};

struct desc3 {
    volatile uint32_t bufhigh, buflow, txvlan, flaglen;
};

static struct {
    volatile uint8_t *regs;
    int ver, flags;
    uint32_t txrxctl;
    uint32_t irqmask;
    void *rx_ring, *tx_ring;
    uint8_t *rx_buf[RX_RING], *tx_buf[TX_RING];
    uint32_t rx_next;                   /* free-running */
    uint32_t tx_next, tx_done;
    uint32_t linkspeed;
    int duplex, link_up, polls;
    struct mii_phy phy;
    struct netdev dev;
} fd;

static inline uint32_t rd(int reg)
{
    return *(volatile uint32_t *)(fd.regs + reg);
}

static inline void wr(int reg, uint32_t v)
{
    *(volatile uint32_t *)(fd.regs + reg) = v;
}

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* Wait up to max_us for (reg & mask) == want. Returns 0, or -1. */
static int reg_wait(int reg, uint32_t mask, uint32_t want, uint32_t step_us, uint32_t max_us)
{
    for (uint32_t t = 0; t <= max_us; t += step_us) {
        if ((rd(reg) & mask) == want)
            return 0;
        udelay(step_us);
    }
    return -1;
}

/* ---- the MII bus ---- */

static int mii_rw(int addr, int reg, int value, int write)
{
    wr(R_MIISTATUS, MIISTAT_MASK_RW);
    if (rd(R_MIICTL) & MIICTL_INUSE) {
        wr(R_MIICTL, MIICTL_INUSE);
        udelay(50);
    }
    uint32_t ctl = (uint32_t)addr << MIICTL_ADDRSHIFT | (uint32_t)reg;
    if (write) {
        wr(R_MIIDATA, (uint32_t)value);
        ctl |= MIICTL_WRITE;
    }
    wr(R_MIICTL, ctl);
    if (reg_wait(R_MIICTL, MIICTL_INUSE, 0, 10, 100000) < 0)
        return -1;
    if (write)
        return 0;
    if (rd(R_MIISTATUS) & MIISTAT_ERROR)
        return -1;
    return (int)(rd(R_MIIDATA) & 0xFFFF);
}

static int mii_read(int addr, int reg)
{
    return mii_rw(addr, reg, 0, 0);
}

static int mii_write(int addr, int reg, uint16_t v)
{
    return mii_rw(addr, reg, v, 1);
}

/* ---- descriptors ---- */

static volatile uint32_t *flaglen(void *ring, uint32_t i)
{
    return fd.ver == 3 ? &((struct desc3 *)ring)[i].flaglen : &((struct desc1 *)ring)[i].flaglen;
}

static void desc_set(void *ring, uint32_t i, void *buf, uint32_t flags)
{
    if (fd.ver == 3) {
        struct desc3 *d = &((struct desc3 *)ring)[i];
        d->bufhigh = 0;
        d->buflow = (uint32_t)buf;
        d->txvlan = 0;
    } else {
        ((struct desc1 *)ring)[i].buf = (uint32_t)buf;
    }
    barrier();
    *flaglen(ring, i) = flags;
}

/* ---- the engines ---- */

static void txrx_reset(void)
{
    wr(R_TXRXCTL, TXRXCTL_BIT2 | TXRXCTL_RESET | fd.txrxctl);
    udelay(4);
    wr(R_TXRXCTL, TXRXCTL_BIT2 | fd.txrxctl);
}

/* Reset the MAC; its address and the address-order bit survive. */
static void mac_reset(void)
{
    wr(R_TXRXCTL, TXRXCTL_BIT2 | TXRXCTL_RESET | fd.txrxctl);
    uint32_t a = rd(R_MACADDRA), b = rd(R_MACADDRB), poll = rd(R_TXPOLL);
    wr(R_MACRESET, MACRESET_ASSERT);
    udelay(64);
    wr(R_MACRESET, 0);
    udelay(64);
    wr(R_MACADDRA, a);
    wr(R_MACADDRB, b);
    wr(R_TXPOLL, poll);
    wr(R_TXRXCTL, TXRXCTL_BIT2 | fd.txrxctl);
}

static void start_rx(void)
{
    if (rd(R_RCVCTL) & RCVCTL_START)
        wr(R_RCVCTL, rd(R_RCVCTL) & ~RCVCTL_START);
    wr(R_LINKSPEED, fd.linkspeed);
    wr(R_RCVCTL, rd(R_RCVCTL) | RCVCTL_START);
}

static void stop_rx(void)
{
    wr(R_RCVCTL, rd(R_RCVCTL) & ~RCVCTL_START);
    reg_wait(R_RCVSTAT, RCVSTAT_BUSY, 0, 10, 500000);
    udelay(100);
    wr(R_LINKSPEED, 0);
}

static void start_tx(void)
{
    wr(R_XMITCTL, rd(R_XMITCTL) | XMITCTL_START);
}

static void stop_tx(void)
{
    wr(R_XMITCTL, rd(R_XMITCTL) & ~XMITCTL_START);
    reg_wait(R_XMITSTAT, XMITSTAT_BUSY, 0, 10, 500000);
    udelay(100);
    wr(R_TXPOLL, rd(R_TXPOLL) & TXPOLL_MAC_REV);
}

/*
 * Read the link from the PHY and set the MAC to match. Both engines stop
 * while the speed and duplex change. Down counts as 10 Mbit/s half duplex.
 */
static void update_link(int force)
{
    int speed, full;
    int up = mii_link(&fd.phy, &speed, &full);
    uint32_t ls = LINKSPEED_FORCE | (speed == 100 ? LINKSPEED_100 : LINKSPEED_10);
    if (!force && up == fd.link_up && ls == fd.linkspeed && full == fd.duplex)
        return;
    if (up != fd.link_up || force) {
        if (up)
            klog("%s: link up, %d Mbit/s, %s duplex\n", fd.dev.name, speed, full ? "full" : "half");
        else if (!force)
            klog("%s: link down\n", fd.dev.name);
    }
    fd.link_up = up;
    fd.linkspeed = ls;
    fd.duplex = full;

    int restart_tx = rd(R_XMITCTL) & XMITCTL_START, restart_rx = rd(R_RCVCTL) & RCVCTL_START;
    if (restart_tx)
        stop_tx();
    if (restart_rx)
        stop_rx();
    uint32_t phyreg = rd(R_PHYIFACE) & ~(PHY_HALF | PHY_100 | PHY_1000);
    if (!full)
        phyreg |= PHY_HALF;
    if (speed == 100)
        phyreg |= PHY_100;
    wr(R_PHYIFACE, phyreg);
    wr(R_TXDEFER, TXDEFER_DEFAULT);
    wr(R_TXWM, fd.ver == 1 ? TXWM_DESC1 : TXWM_DESC23);
    wr(R_MISC1, MISC1_FORCE | (full ? 0 : MISC1_HD));
    wr(R_LINKSPEED, fd.linkspeed);
    if (restart_rx)
        start_rx();
    if (restart_tx)
        start_tx();
}

/* ---- sending and receiving ---- */

static void tx_reclaim(void)
{
    while (fd.tx_done != fd.tx_next) {
        uint32_t f = *flaglen(fd.tx_ring, fd.tx_done % TX_RING);
        if (f & F_VALID)
            break;
        if (f & F_ERROR)
            netif.tx_errors++;
        fd.tx_done++;
    }
}

static int fd_transmit(const void *frame, uint32_t len)
{
    tx_reclaim();
    if (fd.tx_next - fd.tx_done >= TX_RING || len > ETH_FRAME_MAX)
        return -EAGAIN;
    uint32_t i = fd.tx_next % TX_RING;
    uint32_t n = len < 60 ? 60 : len;   /* Ethernet's minimum frame */
    memcpy(fd.tx_buf[i], frame, len);
    memset(fd.tx_buf[i] + len, 0, n - len);
    desc_set(fd.tx_ring, i, fd.tx_buf[i], (n - 1) | F_VALID | (fd.ver == 1 ? TX1_LAST : TX2_LAST));
    fd.tx_next++;
    wr(R_TXRXCTL, TXRXCTL_KICK | fd.txrxctl);
    return 0;
}

/* A received descriptor: the frame's length, or -1 to drop it. As in
 * Linux, "error 4" frames are fine and a framing error is a soft one. */
static int rx_length(uint32_t f)
{
    uint32_t valid = fd.ver == 1 ? RX1_DESCVALID : RX2_DESCVALID;
    uint32_t mask = fd.ver == 1 ? RX1_ERRMASK : RX2_ERRMASK;
    if (!(f & valid))
        return -1;
    int len = (int)(f & LEN_MASK);
    if (f & F_ERROR) {
        uint32_t err = f & mask;
        if (err == (fd.ver == 1 ? RX1_ERROR4 : RX2_ERROR4))
            return len;
        if (err == (fd.ver == 1 ? RX1_FRAMING : RX2_FRAMING))
            return (f & (fd.ver == 1 ? RX1_SUBTRACT1 : RX2_SUBTRACT1)) ? len - 1 : len;
        return -1;
    }
    return len;
}

static void rx_process(void)
{
    for (int n = 0; n < RX_RING; n++) {
        uint32_t i = fd.rx_next % RX_RING;
        uint32_t f = *flaglen(fd.rx_ring, i);
        if (f & F_VALID)
            break;                      /* still the controller's */
        int len = rx_length(f);
        if (len >= ETH_HLEN && len <= RX_BUF_SZ)
            net_receive(fd.rx_buf[i], (uint32_t)len);
        else
            netif.rx_errors++;
        desc_set(fd.rx_ring, i, fd.rx_buf[i], RX_BUF_SZ | F_VALID);
        fd.rx_next++;
    }
}

static void fd_irq(struct trapframe *tf)
{
    (void)tf;
    spin_lock(&net_lock);
    for (int round = 0; round < 8; round++) {
        uint32_t ev = rd(R_IRQSTATUS) & NVI_STAT_MASK;
        if (!ev)
            break;
        wr(R_IRQSTATUS, ev);
        if (ev & NVI_RX_ALL)
            rx_process();
        if (ev & NVI_TX_ALL)
            tx_reclaim();
        if (ev & NVI_LINK) {
            uint32_t st = rd(R_MIISTATUS);
            wr(R_MIISTATUS, MIISTAT_MASK_ALL);
            if (st & MIISTAT_LINKCHANGE)
                update_link(0);
        }
        if (ev & NVI_RECOVER)
            netif.rx_errors++;
    }
    spin_unlock(&net_lock);
}

/* From the timer: anything the interrupt missed, and the link every 2 s. */
static void fd_poll(void)
{
    rx_process();
    tx_reclaim();
    if (++fd.polls >= 20) {
        fd.polls = 0;
        update_link(0);
    }
}

/* ---- setup ---- */

static int valid_mac(const uint8_t *m)
{
    return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

/*
 * The factory address, which the BIOS leaves in the address registers:
 * in order on newer chips (or when the order bit says so), reversed on
 * older ones. If the order we expect gives nonsense and the other does
 * not, take the other.
 */
static void read_mac(uint8_t *mac)
{
    uint32_t a = rd(R_MACADDRA), b = rd(R_MACADDRB);
    uint8_t fwd[6] = { (uint8_t)a, (uint8_t)(a >> 8), (uint8_t)(a >> 16), (uint8_t)(a >> 24),
                       (uint8_t)b, (uint8_t)(b >> 8) };
    uint8_t rev[6] = { (uint8_t)(b >> 8), (uint8_t)b, (uint8_t)(a >> 24), (uint8_t)(a >> 16),
                       (uint8_t)(a >> 8), (uint8_t)a };
    int in_order = (fd.flags & C_MACOK) || (rd(R_TXPOLL) & TXPOLL_MAC_REV);
    const uint8_t *pick = in_order ? fwd : rev, *other = in_order ? rev : fwd;
    if (!valid_mac(pick) && valid_mac(other))
        pick = other;
    memcpy(mac, pick, 6);
    if (!valid_mac(mac)) {
        random_bytes(mac, 6);
        mac[0] = (uint8_t)((mac[0] & 0xFE) | 0x02);     /* locally administered */
        klog("eth0: no usable address in the controller, using a random one\n");
    }
    if (pick == rev)
        wr(R_TXPOLL, rd(R_TXPOLL) | TXPOLL_MAC_REV);    /* the registers are in order now */
}

static void write_mac(const uint8_t *mac)
{
    wr(R_MACADDRA, (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 |
                   (uint32_t)mac[3] << 24);
    wr(R_MACADDRB, (uint32_t)mac[4] | (uint32_t)mac[5] << 8);
}

static int alloc_buffers(void)
{
    uint32_t page = pmm_alloc();
    if (!page)
        return -1;
    memset((void *)page, 0, PAGE_SIZE);
    fd.rx_ring = (void *)page;                          /* 32 x 16 bytes */
    fd.tx_ring = (void *)(page + 2048);
    for (int i = 0; i < RX_RING + TX_RING; i += 2) {
        uint32_t p = pmm_alloc();
        if (!p)
            return -1;
        uint8_t **slot = i < RX_RING ? &fd.rx_buf[i] : &fd.tx_buf[i - RX_RING];
        slot[0] = (uint8_t *)p;
        slot[1] = (uint8_t *)p + BUF_SIZE;
    }
    return 0;
}

/* Bring the controller up, as Linux's nv_open does. */
static void start(void)
{
    mii_power_up(&fd.phy);
    if (fd.flags & C_POWER) {
        wr(R_POWERSTATE2, rd(R_POWERSTATE2) & ~POWER2_GATE);    /* clocks on */
        mac_reset();
    }
    wr(R_MCASTADDRA, MCASTADDRA_FORCE);
    wr(R_MCASTADDRB, 0);
    wr(R_MCASTMASKA, 0xFFFFFFFFu);
    wr(R_MCASTMASKB, 0xFFFF);
    wr(R_PFF, 0);
    wr(R_XMITCTL, 0);
    wr(R_RCVCTL, 0);
    wr(R_ADAPTCTL, 0);

    for (uint32_t i = 0; i < RX_RING; i++)
        desc_set(fd.rx_ring, i, fd.rx_buf[i], RX_BUF_SZ | F_VALID);
    for (uint32_t i = 0; i < TX_RING; i++)
        desc_set(fd.tx_ring, i, fd.tx_buf[i], 0);
    fd.rx_next = fd.tx_next = fd.tx_done = 0;

    wr(R_LINKSPEED, 0);
    wr(R_TXPOLL, rd(R_TXPOLL) & TXPOLL_MAC_REV);
    txrx_reset();
    wr(R_SETUP6, 0);
    wr(R_TXRING, (uint32_t)fd.tx_ring);
    wr(R_RXRING, (uint32_t)fd.rx_ring);
    if (fd.ver == 3) {
        wr(R_TXRINGHI, 0);
        wr(R_RXRINGHI, 0);
    }
    wr(R_RINGSIZES, (RX_RING - 1) << 16 | (TX_RING - 1));

    fd.linkspeed = LINKSPEED_FORCE | LINKSPEED_10;
    wr(R_LINKSPEED, fd.linkspeed);
    wr(R_TXWM, fd.ver == 1 ? TXWM_DESC1 : TXWM_DESC23);
    wr(R_TXRXCTL, fd.txrxctl);
    wr(R_VLANCTL, 0);
    wr(R_TXRXCTL, TXRXCTL_BIT1 | fd.txrxctl);
    if (reg_wait(R_SETUP5, SETUP5_BIT31, SETUP5_BIT31, 5, 50000) < 0)
        klog("%s: setup register 5 bit 31 stayed off\n", fd.dev.name);

    wr(R_MIIMASK, 0);
    wr(R_IRQSTATUS, NVI_STAT_MASK);
    wr(R_MIISTATUS, MIISTAT_MASK_ALL);
    wr(R_MISC1, MISC1_FORCE | MISC1_HD);
    wr(R_XMITSTAT, rd(R_XMITSTAT));
    wr(R_PFF, PFF_ALWAYS);
    wr(R_OFFLOAD, RX_BUF_SZ);
    wr(R_RCVSTAT, rd(R_RCVSTAT));

    uint8_t low;
    random_bytes(&low, 1);
    if (fd.ver == 1)
        wr(R_SLOTTIME, SLOTTIME_DEFAULT | low);
    else
        wr(R_SLOTTIME, SLOTTIME_LEGBF | SLOTTIME_10_100 | low);
    wr(R_TXDEFER, TXDEFER_DEFAULT);
    wr(R_RXDEFER, RXDEFER_DEFAULT);
    wr(R_POLLINT, POLL_THROUGHPUT);
    wr(R_SETUP6, SETUP6_VAL);
    wr(R_ADAPTCTL, (uint32_t)fd.phy.addr << ADAPTCTL_PHYSHIFT | ADAPTCTL_PHYVALID | ADAPTCTL_RUNNING);
    wr(R_MIISPEED, MIISPEED_BIT8 | MIIDELAY);
    wr(R_MIIMASK, MII_LINKCHANGE);

    uint32_t ps = rd(R_POWERSTATE);
    if (!(ps & POWER_UP))
        wr(R_POWERSTATE, ps | POWER_UP);
    udelay(10);
    wr(R_POWERSTATE, rd(R_POWERSTATE) | POWER_VALID);

    wr(R_IRQMASK, 0);
    wr(R_MIISTATUS, MIISTAT_MASK_ALL);
    wr(R_IRQSTATUS, NVI_STAT_MASK);

    /* Our address, and broadcasts through the multicast filter. */
    wr(R_MCASTADDRA, 0xFFFFFFFFu);
    wr(R_MCASTADDRB, 0xFFFF);
    wr(R_MCASTMASKA, 0xFFFFFFFFu);
    wr(R_MCASTMASKB, 0xFFFF);
    wr(R_PFF, PFF_ALWAYS | PFF_MYADDR);

    rd(R_MIISTATUS);
    wr(R_MIISTATUS, MIISTAT_MASK_ALL);
    update_link(1);
    start_rx();
    start_tx();
}

int forcedeth_init(void)
{
    if (cmdline_has("forcedeth=off"))
        return -1;
    /* By ID first: from CK804 on, the controller calls itself a bridge
     * (class 0x0680), not an Ethernet controller. */
    struct pci_dev pci;
    uint8_t prog_if;
    int found = 0;
    for (uint32_t i = 0; i < ARRAY_SIZE(chips) && !found; i++)
        found = pci_find_device(0x10DE, chips[i].id, &pci) == 0;
    for (int i = 0; !found && pci_find_class_nth(0x0200, i, &pci, &prog_if) == 0; i++)
        found = (pci_read32(&pci, 0) & 0xFFFF) == 0x10DE;
    if (!found)
        return -1;

    uint16_t id = (uint16_t)(pci_read32(&pci, 0) >> 16);
    uint8_t rev = (uint8_t)pci_read32(&pci, 0x08);
    const char *chip = "nForce";
    fd.ver = 3;
    fd.flags = C_POWER;
    for (uint32_t i = 0; i < ARRAY_SIZE(chips); i++) {
        if (chips[i].id == id) {
            fd.ver = chips[i].ver;
            fd.flags = chips[i].flags;
            chip = chips[i].name;
        }
    }
    fd.txrxctl = fd.ver == 1 ? TXRXCTL_DESC_1 : fd.ver == 2 ? TXRXCTL_DESC_2 : TXRXCTL_DESC_3;
    fd.irqmask = NVI_THROUGHPUT | ((fd.flags & C_TIMER) ? NVI_TIMER : 0);
    strcpy(fd.dev.name, "eth0");

    fd.regs = pci_map_bar(&pci, PCI_BAR0);
    int irq = pci_irq(&pci);
    if (!fd.regs || alloc_buffers() < 0) {
        boot_warn("Network: cannot set up the nVidia %s controller", chip);
        return -1;
    }
    pci_enable(&pci, PCI_CMD_MEM | PCI_CMD_MASTER);

    if (fd.flags & C_POWER) {           /* the PHY and the MAC out of low power */
        uint32_t ps = rd(R_POWERSTATE2) & ~POWER2_UP_MASK;
        if ((id == 0x0268 || id == 0x0269) && rev >= 0xA3)
            ps |= POWER2_UP_REV_A3;
        wr(R_POWERSTATE2, ps);
    }
    wr(R_WAKEUP, 0);
    wr(R_IRQMASK, 0);
    read_mac(fd.dev.mac);
    write_mac(fd.dev.mac);

    /* Pause frames left on while the MAC sat idle (by the BIOS, or another
     * OS before a warm reboot) can freeze it: off, then a transmitter start
     * and stop recovers it, as Linux does. We never turn them on. */
    if (fd.flags & C_PAUSE) {
        wr(R_TXPAUSE, TXPAUSE_OFF);
        wr(R_MISC1, rd(R_MISC1) & ~MISC1_PAUSE_TX);
        start_tx();
        stop_tx();
    }

    /* Quiet the PHY polling while we look for the PHY. */
    wr(R_MIIMASK, 0);
    wr(R_ADAPTCTL, rd(R_ADAPTCTL) & ~ADAPTCTL_RUNNING);
    wr(R_MIISTATUS, MIISTAT_MASK_ALL);
    fd.phy.read = mii_read;
    fd.phy.write = mii_write;
    fd.phy.nv_fix = (fd.flags & C_PHYFIX) != 0;
    if (mii_probe(&fd.phy) < 0) {
        boot_warn("Network: no PHY answers on the nVidia %s controller's MII bus", chip);
        return -1;
    }
    if (mii_init(&fd.phy) < 0)
        boot_warn("Network: PHY %s at address %d did not reset", fd.phy.name, fd.phy.addr);

    start();
    snprintf(fd.dev.driver, sizeof(fd.dev.driver), "forcedeth (%s, %s)", chip, fd.phy.name);
    fd.dev.irq = irq >= 0 ? (uint32_t)irq : 0;
    fd.dev.transmit = fd_transmit;
    fd.dev.poll = fd_poll;
    net_register(&fd.dev);
    if (irq >= 0) {
        irq_register(irq, fd_irq);
        wr(R_IRQMASK, fd.irqmask);
    }
    uint8_t *m = fd.dev.mac;
    boot_ok("Network: nVidia %s Ethernet (PHY %s %04x:%04x), IRQ %d, MAC "
            "%02x:%02x:%02x:%02x:%02x:%02x, link %s", chip, fd.phy.name, fd.phy.id >> 16,
            fd.phy.id & 0xFFFF, irq, m[0], m[1], m[2], m[3], m[4], m[5],
            fd.link_up ? "up" : "down (negotiating)");
    return 0;
}
