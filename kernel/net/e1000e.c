/*
 * Intel I217 gigabit Ethernet ("e1000e", after the Linux driver whose
 * register names and bring-up order this follows): the MAC built into
 * Intel's 8-series (Lynx Point) chipsets, beside a Haswell processor,
 * and the I217-LM or I217-V PHY it drives over a PCIe-like link.
 *
 * The -LM part comes with Intel's Management Engine (AMT), which shares
 * the PHY with us: the EXTCNF_CTRL software flag is the semaphore around
 * PHY access and resets, and while the ME is in charge it may keep the
 * link to the PHY in SMBus mode or forbid resetting the PHY. CTRL_EXT's
 * DRV_LOAD bit tells it a driver now runs the port.
 *
 * Frames move through two rings of 16-byte "legacy" descriptors: the
 * controller writes received frames into the buffers up to the tail
 * index we give it and sets their "done" bits; we hand it transmit
 * descriptors by moving the transmit tail, and it sets "done" when each
 * is sent.
 *
 * The PHY is reached through MDIC. Its standard registers are at MDIO
 * address 2; Intel's own pages from 768 up are at address 1, chosen by
 * writing the page to register 31 there (Linux's "HV" PHY access). The
 * MAC learns the negotiated speed and duplex from the PHY by itself, and
 * the STATUS register reports the link, so after setup only the MAC's
 * registers are read. The PHY is never reset by hardware here (that
 * would need its configuration reloaded from the NVM by hand, as Linux
 * does); the BIOS left it configured, and a soft reset restarts it.
 *
 * The interrupt handler passes received frames up; the timer tick looks
 * at the rings and the link as well, so a lost interrupt costs only
 * latency.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "mm.h"
#include "net.h"
#include "pci.h"

#define R_CTRL        0x00000
#define R_STATUS      0x00008
#define R_CTRL_EXT    0x00018
#define R_MDIC        0x00020
#define R_FEXTNVM3    0x0003C
#define R_ICR         0x000C0
#define R_ITR         0x000C4
#define R_IMS         0x000D0
#define R_IMC         0x000D8
#define R_LTRV        0x000F8
#define R_RCTL        0x00100
#define R_TCTL        0x00400
#define R_TIPG        0x00410
#define R_EXTCNF_CTRL 0x00F00
#define R_PHY_CTRL    0x00F10
#define R_PBA         0x01000
#define R_PBECCSTS    0x0100C
#define R_WUC         0x05800
#define R_RDBAL       0x02800
#define R_RDBAH       0x02804
#define R_RDLEN       0x02808
#define R_RDH         0x02810
#define R_RDT         0x02818
#define R_RDTR        0x02820
#define R_RADV        0x0282C
#define R_KABGTXD     0x03004
#define R_TDBAL       0x03800
#define R_TDBAH       0x03804
#define R_TDLEN       0x03808
#define R_TDH         0x03810
#define R_TDT         0x03818
#define R_TXDCTL0     0x03828
#define R_TARC0       0x03840
#define R_TXDCTL1     0x03928
#define R_TARC1       0x03940
#define R_MPC         0x04010           /* missed packets, cleared when read */
#define R_RFCTL       0x05008
#define R_MTA         0x05200           /* 32 words on these chips */
#define R_RAL0        0x05400
#define R_RAH0        0x05404
#define R_GCR         0x05B00
#define R_FWSM        0x05B54

#define CTRL_GIO_MASTER_DISABLE (1u << 2)
#define CTRL_SLU          (1u << 6)
#define CTRL_FRCSPD       (1u << 11)
#define CTRL_FRCDPX       (1u << 12)
#define CTRL_LANPHYPC_OVERRIDE (1u << 16)
#define CTRL_LANPHYPC_VALUE    (1u << 17)
#define CTRL_MEHE         (1u << 19)    /* memory error handling */
#define CTRL_RST          (1u << 26)
#define CTRL_RFCE         (1u << 27)    /* obey received pause frames */
#define CTRL_TFCE         (1u << 28)

#define STATUS_FD         (1u << 0)
#define STATUS_LU         (1u << 1)
#define STATUS_SPEED      (3u << 6)
#define STATUS_SPEED_100  (1u << 6)
#define STATUS_GIO_MASTER_ENABLE (1u << 19)

#define CTRL_EXT_LPCD     (1u << 2)     /* LANPHYPC cycle done */
#define CTRL_EXT_FORCE_SMBUS (1u << 11)
#define CTRL_EXT_RO_DIS   (1u << 17)    /* no relaxed ordering */
#define CTRL_EXT_PHYPDEN  (1u << 20)
#define CTRL_EXT_BIT22    (1u << 22)
#define CTRL_EXT_DRV_LOAD (1u << 28)

#define MDIC_REG_SHIFT    16
#define MDIC_PHY_SHIFT    21
#define MDIC_OP_WRITE     (1u << 26)
#define MDIC_OP_READ      (2u << 26)
#define MDIC_READY        (1u << 28)
#define MDIC_ERROR        (1u << 30)

#define FEXTNVM3_PHY_CFG_COUNTER_MASK  (3u << 26)
#define FEXTNVM3_PHY_CFG_COUNTER_50MS  (2u << 26)

#define EXTCNF_SWFLAG     (1u << 5)

#define PHY_CTRL_D0A_LPLU    (1u << 1)
#define PHY_CTRL_GBE_DISABLE (1u << 6)

#define FWSM_RSPCIPHY     (1u << 6)     /* resetting the PHY is allowed */
#define FWSM_FW_VALID     (1u << 15)    /* the ME firmware runs */

#define ICR_TXDW          (1u << 0)
#define ICR_LSC           (1u << 2)
#define ICR_RXSEQ         (1u << 3)
#define ICR_RXDMT0        (1u << 4)
#define ICR_RXO           (1u << 6)
#define ICR_RXT0          (1u << 7)
#define IMS_ENABLE        (ICR_RXT0 | ICR_TXDW | ICR_RXDMT0 | ICR_RXSEQ | ICR_LSC | ICR_RXO)

#define RCTL_EN           (1u << 1)
#define RCTL_BAM          (1u << 15)    /* broadcasts; 2048-byte buffers with BSIZE 0 */
#define RCTL_SECRC        (1u << 26)    /* strip the CRC */

#define TCTL_EN           (1u << 1)
#define TCTL_PSP          (1u << 3)     /* pad short frames */
#define TCTL_CT           (15u << 4)    /* collision threshold */
#define TCTL_COLD         (63u << 12)   /* collision distance */
#define TCTL_RTLC         (1u << 24)

#define TIPG_IPGT_MASK    0x3FF

#define TXDCTL_PTHRESH    0x0000003Fu
#define TXDCTL_WTHRESH    0x003F0000u
#define TXDCTL_FULL_WB    0x01010000u   /* write back every descriptor */
#define TXDCTL_PREFETCH   0x0000001Fu
#define TXDCTL_BIT22      (1u << 22)

#define RFCTL_NFSW_DIS    (1u << 6)
#define RFCTL_NFSR_DIS    (1u << 7)
#define PBECCSTS_ECC      (1u << 16)
#define KABGTXD_BGSQLBIAS 0x00050000u
#define GCR_NO_SNOOP_ALL  0x3Fu

#define LTRV_SCALE_SHIFT  10
#define LTRV_REQ          (1u << 15)
#define LTRV_NOSNOOP_SHIFT 16
#define LTRV_SEND         (1u << 30)
#define PCI_LTR_CAP       0xA8          /* the BIOS's maximum snoop and no-snoop latencies */
#define PBA_26K           26            /* KiB of the packet buffer for receiving, as Linux sets */

/* The PHY: standard registers at address 2, Intel's pages 768 on at 1. */
#define PHY_ADDR          2
#define PHY_ADDR_HV       1
#define PHY_PAGE_SELECT   0x1F
#define PHY_PAGE_SHIFT    5
#define HV_PAGE_START     768
#define PHY_REG(page, reg) ((page) << 5 | (reg))

#define MII_BMCR          0x00
#define BMCR_ANENABLE     0x1000
#define BMCR_ANRESTART    0x0200
#define MII_PHYSID1       0x02
#define MII_PHYSID2       0x03
#define I82577_CFG_REG    PHY_REG(0, 22)
#define CFG_CRS_ON_TX     (1u << 15)    /* carrier sense on transmit: needed for half duplex */
#define CFG_DOWNSHIFT     (3u << 10)    /* 1000 -> 100 Mbit/s on a two-pair cable */
#define HV_OEM_BITS       PHY_REG(768, 25)
#define OEM_LPLU          0x0004
#define OEM_GBE_DIS       0x0040
#define OEM_RESTART_AN    0x0400
#define CV_SMB_CTRL       PHY_REG(769, 23)
#define SMB_FORCE_SMBUS   0x0001

/* Legacy descriptors */
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

#define RXD_DD            0x01
#define RXD_EOP           0x02
#define RXE_FRAME         0x97          /* CRC, symbol, sequence, carrier extension, data errors */
#define TXD_EOP           0x01
#define TXD_IFCS          0x02          /* append the CRC */
#define TXD_RS            0x08          /* report status: set DD when sent */
#define TXD_DD            0x01
#define TXD_EC            0x02          /* excess collisions */
#define TXD_LC            0x04          /* late collision */

#define RX_RING   32                    /* the ring lengths are multiples of 128 bytes */
#define TX_RING   16
#define BUF_SIZE  2048

static const struct {
    uint16_t id;
    const char *name;
} chips[] = {
    { 0x153A, "I217-LM" },
    { 0x153B, "I217-V" },
};

static struct {
    volatile uint8_t *regs;
    struct pci_dev pci;
    const char *chip;
    struct rx_desc *rx_ring;
    struct tx_desc *tx_ring;
    uint8_t *rx_buf[RX_RING], *tx_buf[TX_RING];
    uint32_t rx_next;                   /* the next descriptor to look at */
    uint32_t tx_next, tx_done;          /* free-running */
    int link_up, speed, full, polls;
    uint32_t max_ltr;                   /* the most latency the BIOS lets us report, encoded */
    int irq;
    struct mii_phy phy;
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

static inline void flush(void)
{
    (void)rd(R_STATUS);
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

static int me_active(void)
{
    return (rd(R_FWSM) & FWSM_FW_VALID) != 0;
}

/* Does the ME forbid resetting the PHY? It may say so for a while. */
static int reset_blocked(void)
{
    for (int i = 0; i < 30; i++) {
        if (rd(R_FWSM) & FWSM_RSPCIPHY)
            return 0;
        mdelay(10);
    }
    return 1;
}

/* ---- the semaphore shared with the ME ---- */

static int swflag_take(void)
{
    if (reg_wait(R_EXTCNF_CTRL, EXTCNF_SWFLAG, 0, 1000, 100000) < 0)
        return -1;                      /* someone holds it */
    wr(R_EXTCNF_CTRL, rd(R_EXTCNF_CTRL) | EXTCNF_SWFLAG);
    if (reg_wait(R_EXTCNF_CTRL, EXTCNF_SWFLAG, EXTCNF_SWFLAG, 1000, 1000000) < 0) {
        wr(R_EXTCNF_CTRL, rd(R_EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
        return -1;                      /* the firmware or the hardware has it */
    }
    return 0;
}

static void swflag_give(void)
{
    wr(R_EXTCNF_CTRL, rd(R_EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
}

/* ---- the PHY ---- */

/* One MDIC transaction, the semaphore held: the value read, or -1. */
static int mdic(int addr, int reg, int value, int write)
{
    uint32_t cmd = (uint32_t)(reg & 0x1F) << MDIC_REG_SHIFT | (uint32_t)addr << MDIC_PHY_SHIFT;
    cmd |= write ? MDIC_OP_WRITE | (uint16_t)value : MDIC_OP_READ;
    wr(R_MDIC, cmd);
    uint32_t v = 0;
    for (int i = 0; i < 1920; i++) {    /* 96 ms, as Linux allows */
        udelay(50);
        v = rd(R_MDIC);
        if (v & MDIC_READY)
            break;
    }
    if (!(v & MDIC_READY) || (v & MDIC_ERROR))
        return -1;
    if (write)
        return 0;
    if ((v >> MDIC_REG_SHIFT & 0x1F) != (uint32_t)(reg & 0x1F))
        return -1;                      /* an answer to something else */
    return (int)(v & 0xFFFF);
}

/* A register by page and number (PHY_REG), the semaphore held. */
static int hv_rw(int preg, int value, int write)
{
    int page = preg >> 5, reg = preg & 0x1F;
    int addr = page >= HV_PAGE_START ? PHY_ADDR_HV : PHY_ADDR;
    if (page > 0 && mdic(PHY_ADDR_HV, PHY_PAGE_SELECT, page << PHY_PAGE_SHIFT, 1) < 0)
        return -1;
    return mdic(addr, reg, value, write);
}

static int hv_read(int preg)
{
    if (swflag_take() < 0)
        return -1;
    int v = hv_rw(preg, 0, 0);
    swflag_give();
    return v;
}

static int hv_write(int preg, uint16_t value)
{
    if (swflag_take() < 0)
        return -1;
    int r = hv_rw(preg, value, 1);
    swflag_give();
    return r;
}

/* mii.c's access: standard registers, at the address it is given. */
static int mii_read(int addr, int reg)
{
    if (swflag_take() < 0)
        return -1;
    int v = mdic(addr, reg, 0, 0);
    swflag_give();
    return v;
}

static int mii_write(int addr, int reg, uint16_t v)
{
    if (swflag_take() < 0)
        return -1;
    int r = mdic(addr, reg, v, 1);
    swflag_give();
    return r;
}

/* Does the PHY answer? Then, unless the ME wants it, out of SMBus mode. */
static int phy_accessible(void)
{
    uint32_t id = 0;
    for (int retry = 0; retry < 2 && !id; retry++) {
        int id1 = mii_read(PHY_ADDR, MII_PHYSID1);
        int id2 = id1 < 0 || id1 == 0xFFFF ? -1 : mii_read(PHY_ADDR, MII_PHYSID2);
        if (id2 >= 0 && id2 != 0xFFFF)
            id = (uint32_t)id1 << 16 | (uint32_t)id2;
    }
    if (!id)
        return 0;
    e.phy.id = id;
    snprintf(e.phy.name, sizeof(e.phy.name), "%s", id >> 16 == 0x0154 ? "Intel I217" : "PHY");
    if (!me_active()) {
        int v = hv_read(CV_SMB_CTRL);
        if (v >= 0 && (v & SMB_FORCE_SMBUS))
            hv_write(CV_SMB_CTRL, (uint16_t)(v & ~SMB_FORCE_SMBUS));
        wr(R_CTRL_EXT, rd(R_CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
    }
    return 1;
}

/* Power-cycle the PHY with the LANPHYPC pin, which also brings its link
 * to the MAC out of SMBus mode. */
static void toggle_lanphypc(void)
{
    uint32_t v = rd(R_FEXTNVM3) & ~FEXTNVM3_PHY_CFG_COUNTER_MASK;
    wr(R_FEXTNVM3, v | FEXTNVM3_PHY_CFG_COUNTER_50MS);
    uint32_t ctrl = (rd(R_CTRL) | CTRL_LANPHYPC_OVERRIDE) & ~CTRL_LANPHYPC_VALUE;
    wr(R_CTRL, ctrl);
    flush();
    udelay(20);
    wr(R_CTRL, ctrl & ~CTRL_LANPHYPC_OVERRIDE);
    flush();
    for (int i = 0; i < 20 && !(rd(R_CTRL_EXT) & CTRL_EXT_LPCD); i++)
        mdelay(5);
    mdelay(30);
}

/*
 * Make sure the PHY can be reached, as Linux's
 * e1000_init_phy_workarounds_pchlan does: the link to it may have been
 * left in SMBus mode (by the ME, or by the BIOS or another OS going to
 * sleep). Try forcing the MAC into SMBus mode first; failing that, cycle
 * the PHY's power, unless the ME forbids it.
 */
static int phy_wake(void)
{
    if (phy_accessible())
        return 0;
    wr(R_CTRL_EXT, rd(R_CTRL_EXT) | CTRL_EXT_FORCE_SMBUS);
    mdelay(50);
    if (phy_accessible())
        return 0;
    if (reset_blocked()) {
        klog("%s: the PHY does not answer and the ME firmware does not let it be reset\n",
             e.dev.name);
        return -1;
    }
    toggle_lanphypc();
    if (phy_accessible())
        return 0;
    wr(R_CTRL_EXT, rd(R_CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
    if (phy_accessible())
        return 0;
    klog("%s: the PHY does not answer, even after a power cycle\n", e.dev.name);
    return -1;
}

/*
 * Out of the low-power link modes the BIOS or a sleeping OS may have
 * set (low-power link-up and gigabit off), advertise everything, and
 * have the PHY negotiate the link. Linux leaves the PHY alone while the
 * ME forbids resetting it, and so do we.
 */
static void phy_setup(void)
{
    wr(R_PHY_CTRL, rd(R_PHY_CTRL) & ~(PHY_CTRL_D0A_LPLU | PHY_CTRL_GBE_DISABLE));
    if (reset_blocked()) {
        klog("%s: the ME firmware runs the PHY; leaving its link settings alone\n", e.dev.name);
        return;
    }
    int oem = hv_read(HV_OEM_BITS);
    if (oem >= 0)
        hv_write(HV_OEM_BITS, (uint16_t)((oem & ~(OEM_LPLU | OEM_GBE_DIS)) | OEM_RESTART_AN));

    mii_power_up(&e.phy);
    if (mii_init(&e.phy) < 0)
        klog("%s: PHY %s did not reset\n", e.dev.name, e.phy.name);
    int cfg = hv_read(I82577_CFG_REG);
    if (cfg >= 0 && (cfg & (CFG_CRS_ON_TX | CFG_DOWNSHIFT)) != (CFG_CRS_ON_TX | CFG_DOWNSHIFT)) {
        hv_write(I82577_CFG_REG, (uint16_t)(cfg | CFG_CRS_ON_TX | CFG_DOWNSHIFT));
        int bmcr = mii_read(PHY_ADDR, MII_BMCR);
        if (bmcr >= 0)
            mii_write(PHY_ADDR, MII_BMCR, (uint16_t)(bmcr | BMCR_ANENABLE | BMCR_ANRESTART));
    }
}

/* ---- the MAC ---- */

/*
 * Reset the MAC, not the PHY: resetting both, as Linux does, would need
 * the PHY's configuration loaded again from the NVM by hand. A MAC alone
 * is what Linux resets when the ME forbids a PHY reset, so it is a state
 * the hardware expects.
 */
static void mac_reset(void)
{
    wr(R_CTRL, rd(R_CTRL) | CTRL_GIO_MASTER_DISABLE);   /* no DMA under way */
    if (reg_wait(R_STATUS, STATUS_GIO_MASTER_ENABLE, 0, 100, 80000) < 0)
        klog("%s: DMA did not stop before the reset\n", e.dev.name);
    wr(R_PBA, PBA_26K);
    wr(R_IMC, 0xFFFFFFFFu);
    wr(R_RCTL, 0);
    wr(R_TCTL, TCTL_PSP);
    flush();
    mdelay(10);

    int locked = swflag_take() == 0;
    wr(R_CTRL, rd(R_CTRL) | CTRL_RST);
    mdelay(20);                         /* no register reads meanwhile: they hang it */
    if (locked)
        swflag_give();

    wr(R_IMC, 0xFFFFFFFFu);
    (void)rd(R_ICR);
    wr(R_KABGTXD, rd(R_KABGTXD) | KABGTXD_BGSQLBIAS);
}

/* The bits Linux's e1000_initialize_hw_bits_ich8lan and init_hw set. */
static void init_hw_bits(void)
{
    wr(R_CTRL_EXT, rd(R_CTRL_EXT) | CTRL_EXT_BIT22 | CTRL_EXT_PHYPDEN | CTRL_EXT_RO_DIS |
                   CTRL_EXT_DRV_LOAD);
    for (int q = 0; q < 2; q++) {
        int reg = q ? R_TXDCTL1 : R_TXDCTL0;
        uint32_t v = rd(reg) | TXDCTL_BIT22;
        v = (v & ~TXDCTL_WTHRESH) | TXDCTL_FULL_WB;
        v = (v & ~TXDCTL_PTHRESH) | TXDCTL_PREFETCH;
        wr(reg, v);
    }
    wr(R_TARC0, rd(R_TARC0) | 1u << 23 | 1u << 24 | 1u << 26 | 1u << 27);
    wr(R_TARC1, (rd(R_TARC1) | 1u << 24 | 1u << 26 | 1u << 28 | 1u << 30));  /* TCTL.MULR is off */
    wr(R_RFCTL, rd(R_RFCTL) | RFCTL_NFSW_DIS | RFCTL_NFSR_DIS);
    wr(R_PBECCSTS, rd(R_PBECCSTS) | PBECCSTS_ECC);
    wr(R_CTRL, rd(R_CTRL) | CTRL_MEHE);
    wr(R_GCR, rd(R_GCR) & ~GCR_NO_SNOOP_ALL);   /* DMA snoops the processor's caches */
}

static void write_mac(const uint8_t *m)
{
    wr(R_RAL0, (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24);
    flush();
    wr(R_RAH0, (uint32_t)m[4] | (uint32_t)m[5] << 8 | 1u << 31);   /* address valid */
    flush();
}

static int valid_mac(const uint8_t *m)
{
    return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

/* The factory address, which the controller loads from the NVM into
 * receive address 0 at reset. */
static void read_mac(uint8_t *mac)
{
    uint32_t lo = rd(R_RAL0), hi = rd(R_RAH0);
    uint8_t m[6] = { (uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)(lo >> 16), (uint8_t)(lo >> 24),
                     (uint8_t)hi, (uint8_t)(hi >> 8) };
    memcpy(mac, m, 6);
    if (!valid_mac(mac)) {
        random_bytes(mac, 6);
        mac[0] = (uint8_t)((mac[0] & 0xFE) | 0x02);     /* locally administered */
        klog("eth0: no usable address in the controller, using a random one\n");
    }
}

/*
 * Latency Tolerance Reporting: how long the platform may take to answer
 * the controller's memory requests (the time to fill the receive buffer
 * at link speed), so the processor sleeps no deeper than that. Linux's
 * e1000_platform_pm_pch_lpt; no requirement while the link is down.
 */
static void set_ltr(int speed)
{
    uint32_t reg = LTRV_SEND;
    uint32_t lat = 0;
    if (speed) {
        reg |= LTRV_REQ | LTRV_REQ << LTRV_NOSNOOP_SHIFT;
        uint32_t rxa = rd(R_PBA) & 0xFFFF;              /* receive buffer, KiB */
        uint32_t ns = rxa * 1024 > ETH_FRAME_MAX + 4 ?
                      (rxa * 1024 - (ETH_FRAME_MAX + 4)) * 8 * 1000 / (uint32_t)speed : 0;
        uint32_t scale = 0;
        while (ns > 0x3FF && scale < 5) {
            scale++;
            ns = (ns + 31) / 32;
        }
        lat = scale << LTRV_SCALE_SHIFT | MIN(ns, 0x3FFu);
        if (lat > e.max_ltr)
            lat = e.max_ltr;
    }
    wr(R_LTRV, reg | lat | lat << LTRV_NOSNOOP_SHIFT);
}

/*
 * The link from the MAC's status register; on a change, the settings
 * that depend on it: the gap between frames (wider at 10 Mbit/s half
 * duplex, where these PHYs collide too eagerly), obeying pause frames in
 * full duplex, and the latency the platform may add.
 */
static void update_link(int force)
{
    uint32_t st = rd(R_STATUS);
    int up = (st & STATUS_LU) != 0;
    int speed = !up ? 0 : !(st & STATUS_SPEED) ? 10 : (st & STATUS_SPEED) == STATUS_SPEED_100 ? 100 : 1000;
    int full = up && (st & STATUS_FD);
    if (!force && up == e.link_up && speed == e.speed && full == e.full)
        return;
    if (up)
        klog("%s: link up, %d Mbit/s, %s duplex\n", e.dev.name, speed, full ? "full" : "half");
    else if (!force)
        klog("%s: link down\n", e.dev.name);
    e.link_up = up;
    e.speed = speed;
    e.full = full;

    uint32_t tipg = rd(R_TIPG) & ~TIPG_IPGT_MASK;
    wr(R_TIPG, tipg | (speed == 10 && !full ? 0xFF : 0x08));
    uint32_t ctrl = rd(R_CTRL) & ~(CTRL_RFCE | CTRL_TFCE);
    wr(R_CTRL, full ? ctrl | CTRL_RFCE : ctrl);
    set_ltr(speed);
}

/* ---- sending and receiving ---- */

static void tx_reclaim(void)
{
    while (e.tx_done != e.tx_next) {
        struct tx_desc *d = &e.tx_ring[e.tx_done % TX_RING];
        if (!(d->status & TXD_DD))
            break;
        if (d->status & (TXD_EC | TXD_LC))
            netif.tx_errors++;
        e.tx_done++;
    }
}

static int e_transmit(const void *frame, uint32_t len)
{
    tx_reclaim();
    /* One descriptor stays empty: a full ring would look empty to the
     * controller (tail equal to head). */
    if (e.tx_next - e.tx_done >= TX_RING - 1 || len > ETH_FRAME_MAX)
        return -EAGAIN;
    uint32_t i = e.tx_next % TX_RING;
    uint32_t n = len < 60 ? 60 : len;   /* Ethernet's minimum frame */
    memcpy(e.tx_buf[i], frame, len);
    memset(e.tx_buf[i] + len, 0, n - len);
    struct tx_desc *d = &e.tx_ring[i];
    d->addr_lo = (uint32_t)e.tx_buf[i];
    d->addr_hi = 0;
    d->length = (uint16_t)n;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = TXD_EOP | TXD_IFCS | TXD_RS;
    barrier();
    e.tx_next++;
    wr(R_TDT, e.tx_next % TX_RING);
    return 0;
}

static void rx_process(void)
{
    for (int n = 0; n < RX_RING; n++) {
        uint32_t i = e.rx_next;
        struct rx_desc *d = &e.rx_ring[i];
        uint8_t status = d->status;
        if (!(status & RXD_DD))
            break;                      /* still the controller's */
        barrier();
        uint32_t len = d->length;
        if ((status & RXD_EOP) && !(d->errors & RXE_FRAME) && len >= ETH_HLEN && len <= BUF_SIZE)
            net_receive(e.rx_buf[i], len);
        else
            netif.rx_errors++;
        d->status = 0;
        d->errors = 0;
        barrier();
        wr(R_RDT, i);                   /* the buffer is the controller's again */
        e.rx_next = (i + 1) % RX_RING;
    }
}

static void e_irq(struct trapframe *tf)
{
    (void)tf;
    spin_lock(&net_lock);
    for (int round = 0; round < 8; round++) {
        uint32_t icr = rd(R_ICR);       /* reading it clears it */
        if (!icr)
            break;
        if (icr & ICR_LSC)
            update_link(0);
        rx_process();
        tx_reclaim();
    }
    spin_unlock(&net_lock);
}

/* From the timer: anything the interrupt missed; the link and the
 * frames dropped for want of buffers every 2 s. */
static void e_poll(void)
{
    rx_process();
    tx_reclaim();
    if (++e.polls >= 20) {
        e.polls = 0;
        update_link(0);
        netif.rx_dropped += rd(R_MPC);
    }
}

/* ---- setup ---- */

static int alloc_buffers(void)
{
    uint32_t page = pmm_alloc();
    if (!page)
        return -1;
    memset((void *)page, 0, PAGE_SIZE);
    e.rx_ring = (struct rx_desc *)page;                 /* 32 x 16 bytes */
    e.tx_ring = (struct tx_desc *)(page + 2048);        /* 16 x 16 bytes */
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

static void start_rings(void)
{
    for (uint32_t i = 0; i < RX_RING; i++) {
        e.rx_ring[i].addr_lo = (uint32_t)e.rx_buf[i];
        e.rx_ring[i].addr_hi = 0;
        e.rx_ring[i].status = 0;
        e.rx_ring[i].errors = 0;
    }
    memset(e.tx_ring, 0, TX_RING * sizeof(struct tx_desc));
    e.rx_next = 0;
    e.tx_next = e.tx_done = 0;

    wr(R_TDBAL, (uint32_t)e.tx_ring);
    wr(R_TDBAH, 0);
    wr(R_TDLEN, TX_RING * sizeof(struct tx_desc));
    wr(R_TDH, 0);
    wr(R_TDT, 0);
    wr(R_TXDCTL1, rd(R_TXDCTL0));       /* an erratum: both queues set the same */
    wr(R_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | TCTL_RTLC);

    wr(R_RDTR, 0);                      /* an interrupt for each frame, */
    wr(R_RADV, 8);
    wr(R_ITR, 195);                     /* but no more than 20000 a second */
    wr(R_RDBAL, (uint32_t)e.rx_ring);
    wr(R_RDBAH, 0);
    wr(R_RDLEN, RX_RING * sizeof(struct rx_desc));
    wr(R_RDH, 0);
    wr(R_RDT, RX_RING - 1);             /* every buffer but one */
    wr(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
}

/* Bring the controller up, at boot and after suspend. */
static int start(void)
{
    wr(R_CTRL_EXT, rd(R_CTRL_EXT) | CTRL_EXT_DRV_LOAD);    /* ours now, the ME is told */
    if (phy_wake() < 0)
        return -1;
    mac_reset();
    init_hw_bits();
    wr(R_WUC, 0);                       /* no wake-up events */
    write_mac(e.dev.mac);
    for (int i = 0; i < 32; i++)
        wr(R_MTA + 4 * i, 0);

    uint32_t ctrl = rd(R_CTRL) & ~(CTRL_FRCSPD | CTRL_FRCDPX);
    wr(R_CTRL, ctrl | CTRL_SLU);        /* the speed and duplex come from the PHY */
    phy_setup();
    start_rings();
    e.link_up = e.speed = e.full = 0;
    update_link(1);
    return 0;
}

static void e_suspend(void)
{
    wr(R_IMC, 0xFFFFFFFFu);
    wr(R_RCTL, rd(R_RCTL) & ~RCTL_EN);
    wr(R_TCTL, rd(R_TCTL) & ~TCTL_EN);
    flush();
    mdelay(10);
    wr(R_CTRL_EXT, rd(R_CTRL_EXT) & ~CTRL_EXT_DRV_LOAD);   /* the ME may have the port */
}

static void e_resume(void)
{
    if (start() < 0) {
        klog("%s: the controller did not come back after suspend\n", e.dev.name);
        return;
    }
    if (e.irq >= 0)
        wr(R_IMS, IMS_ENABLE);
}

int e1000e_init(void)
{
    if (cmdline_has("e1000e=off"))
        return -1;
    int found = 0;
    for (uint32_t i = 0; i < ARRAY_SIZE(chips) && !found; i++) {
        if (pci_find_device(0x8086, chips[i].id, &e.pci) == 0) {
            found = 1;
            e.chip = chips[i].name;
        }
    }
    if (!found)
        return -1;
    strcpy(e.dev.name, "eth0");

    e.regs = pci_map_bar(&e.pci, PCI_BAR0);
    int irq = pci_irq(&e.pci);
    if (!e.regs || alloc_buffers() < 0) {
        boot_warn("Network: cannot set up the Intel %s controller", e.chip);
        return -1;
    }
    pci_enable(&e.pci, PCI_CMD_MEM | PCI_CMD_MASTER);
    pci_write16(&e.pci, PCI_COMMAND, pci_read16(&e.pci, PCI_COMMAND) & ~0x400);   /* INTx on */
    int me = me_active();
    e.max_ltr = MAX(pci_read16(&e.pci, PCI_LTR_CAP), pci_read16(&e.pci, PCI_LTR_CAP + 2));

    read_mac(e.dev.mac);
    e.phy.read = mii_read;
    e.phy.write = mii_write;
    e.phy.addr = PHY_ADDR;
    e.phy.gigabit = 1;
    if (start() < 0) {
        wr(R_CTRL_EXT, rd(R_CTRL_EXT) & ~CTRL_EXT_DRV_LOAD);
        boot_warn("Network: the Intel %s controller's PHY does not answer", e.chip);
        return -1;
    }
    snprintf(e.dev.driver, sizeof(e.dev.driver), "e1000e (Intel %s)", e.chip);
    e.dev.irq = irq >= 0 ? (uint32_t)irq : 0;
    e.dev.transmit = e_transmit;
    e.dev.poll = e_poll;
    e.dev.suspend = e_suspend;
    e.dev.resume = e_resume;
    e.irq = irq;
    net_register(&e.dev);
    if (irq >= 0) {
        irq_register(irq, e_irq);
        wr(R_IMS, IMS_ENABLE);
    }
    uint8_t *m = e.dev.mac;
    boot_ok("Network: Intel %s Gigabit Ethernet (PHY %08x)%s, IRQ %d, MAC "
            "%02x:%02x:%02x:%02x:%02x:%02x, link %s", e.chip, e.phy.id,
            me ? ", shared with the ME" : "", irq, m[0], m[1], m[2], m[3], m[4], m[5],
            e.link_up ? "up" : "down (negotiating)");
    return 0;
}
