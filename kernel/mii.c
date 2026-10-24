/*
 * Ethernet PHYs on an MII management bus: the chip between a network
 * controller (MAC) and the cable, which negotiates the link with the
 * other end. The MAC driver supplies register access; this file finds
 * the PHY, resets it, advertises what we can do (10 and 100 Mbit/s, half
 * and full duplex) and reads back what the link settled on.
 *
 * The Realtek RTL8201 family (RTL8201CP/CL/N: single-port 10/100) is
 * recognised by name, and gets the extra setup the nForce MACs need with
 * it (as Linux's forcedeth does). Any other PHY that follows the IEEE
 * 802.3 clause 22 registers works the same way; a gigabit PHY is kept at
 * 100 Mbit/s, which is all the MAC drivers here set up.
 */
#include "kernel.h"
#include "drivers.h"
#include "net.h"

#define MII_BMCR     0x00
#define MII_BMSR     0x01
#define MII_PHYSID1  0x02
#define MII_PHYSID2  0x03
#define MII_ADVERTISE 0x04
#define MII_LPA      0x05
#define MII_CTRL1000 0x09

#define BMCR_RESET     0x8000
#define BMCR_ANENABLE  0x1000
#define BMCR_PDOWN     0x0800
#define BMCR_ISOLATE   0x0400
#define BMCR_ANRESTART 0x0200

#define BMSR_LSTATUS   0x0004
#define BMSR_ANEGDONE  0x0020
#define BMSR_ESTATEN   0x0100           /* extended status: a gigabit PHY */

#define ADV_10HALF   0x0020
#define ADV_10FULL   0x0040
#define ADV_100HALF  0x0080
#define ADV_100FULL  0x0100
#define ADV_PAUSE    0x0400
#define ADV_ASYM     0x0800
#define ADV_ALL      (ADV_10HALF | ADV_10FULL | ADV_100HALF | ADV_100FULL)

/* RTL8201 (on an nForce MAC): register 0x11, bit 12, as forcedeth sets it. */
#define RTL8201_REG11     0x11
#define RTL8201_NV_INIT   0x1000

/*
 * Realtek's OUI is 0x000732, which PHYSID1/2 hold as 0x001C and the top six
 * bits of 0xC8xx; the model is bits 4-9 of PHYSID2. The RTL8201 behind
 * nForce MACs reports model 0x20 (Linux's PHY_MODEL_REALTEK_8201), the
 * RTL8201CP/CL 0x0000:0x8201, and the RTL8201F/FL model 0x01.
 */
static int is_rtl8201(uint32_t id)
{
    uint32_t id1 = id >> 16, id2 = id & 0xFFFF;
    if (id == 0x00008201u)
        return 1;
    if (id1 != 0x001C || (id2 & 0xFC00) != 0xC800)
        return 0;
    uint32_t model = (id2 >> 4) & 0x3F;
    return model == 0x20 || model == 0x01;
}

int mii_probe(struct mii_phy *phy)
{
    /* Addresses 1 to 31 first, then 0, which some PHYs answer as a broadcast. */
    for (int i = 1; i <= 32; i++) {
        int addr = i & 0x1F;
        int id1 = phy->read(addr, MII_PHYSID1);
        if (id1 < 0 || id1 == 0xFFFF)
            continue;
        int id2 = phy->read(addr, MII_PHYSID2);
        if (id2 < 0 || id2 == 0xFFFF || (id1 == 0 && id2 == 0))
            continue;
        phy->addr = addr;
        phy->id = (uint32_t)id1 << 16 | (uint32_t)id2;
        phy->rtl8201 = is_rtl8201(phy->id);
        snprintf(phy->name, sizeof(phy->name), "%s", phy->rtl8201 ? "Realtek RTL8201" :
                 (id1 == 0x001C && (id2 & 0xFC00) == 0xC800) ? "Realtek" : "PHY");
        return 0;
    }
    return -1;
}

static int rtl8201_nv_fix(struct mii_phy *phy)
{
    int v = phy->read(phy->addr, RTL8201_REG11);
    if (v < 0)
        return -1;
    return phy->write(phy->addr, RTL8201_REG11, (uint16_t)(v | RTL8201_NV_INIT));
}

int mii_init(struct mii_phy *phy)
{
    if (phy->rtl8201 && phy->nv_fix && rtl8201_nv_fix(phy) < 0)
        return -1;

    int adv = phy->read(phy->addr, MII_ADVERTISE);
    if (adv < 0)
        return -1;
    adv |= ADV_ALL | ADV_PAUSE | ADV_ASYM;
    phy->write(phy->addr, MII_ADVERTISE, (uint16_t)adv);

    /* A gigabit PHY: not 1000 Mbit/s, the MAC is not set up for it. */
    int bmsr = phy->read(phy->addr, MII_BMSR);
    if (bmsr >= 0 && (bmsr & BMSR_ESTATEN)) {
        int c1000 = phy->read(phy->addr, MII_CTRL1000);
        if (c1000 >= 0)
            phy->write(phy->addr, MII_CTRL1000, (uint16_t)(c1000 & ~0x0300));
    }

    /* Reset it (with autonegotiation on: some PHYs want BMCR set up with
     * the reset), and wait for the reset bit to clear. */
    int bmcr = phy->read(phy->addr, MII_BMCR);
    if (bmcr < 0)
        return -1;
    bmcr = (bmcr | BMCR_ANENABLE) & ~(BMCR_PDOWN | BMCR_ISOLATE);
    if (phy->write(phy->addr, MII_BMCR, (uint16_t)(bmcr | BMCR_RESET)) < 0)
        return -1;
    mdelay(500);
    for (int tries = 0; ; tries++) {
        bmcr = phy->read(phy->addr, MII_BMCR);
        if (bmcr >= 0 && !(bmcr & BMCR_RESET))
            break;
        if (tries > 100)
            return -1;
        mdelay(10);
    }

    if (phy->rtl8201 && phy->nv_fix && rtl8201_nv_fix(phy) < 0)
        return -1;
    /* Some PHYs forget the pause bits on reset. */
    phy->write(phy->addr, MII_ADVERTISE, (uint16_t)adv);
    bmcr = phy->read(phy->addr, MII_BMCR);
    if (bmcr < 0)
        return -1;
    bmcr = (bmcr | BMCR_ANENABLE | BMCR_ANRESTART) & ~BMCR_PDOWN;
    return phy->write(phy->addr, MII_BMCR, (uint16_t)bmcr);
}

void mii_power_up(struct mii_phy *phy)
{
    int bmcr = phy->read(phy->addr, MII_BMCR);
    if (bmcr >= 0 && (bmcr & BMCR_PDOWN))
        phy->write(phy->addr, MII_BMCR, (uint16_t)(bmcr & ~BMCR_PDOWN));
}

/*
 * Is the link up, and at what speed? While autonegotiation is still
 * going the link counts as down. A partner that does not negotiate is
 * found by parallel detection, which shows up in the partner's abilities
 * like a negotiated one.
 */
int mii_link(struct mii_phy *phy, int *speed, int *full)
{
    *speed = 10;
    *full = 0;
    phy->read(phy->addr, MII_BMSR);     /* the link bit latches low: read it twice */
    int bmsr = phy->read(phy->addr, MII_BMSR);
    if (bmsr < 0 || !(bmsr & BMSR_LSTATUS) || !(bmsr & BMSR_ANEGDONE))
        return 0;
    int adv = phy->read(phy->addr, MII_ADVERTISE), lpa = phy->read(phy->addr, MII_LPA);
    if (adv < 0 || lpa < 0)
        return 0;
    int common = adv & lpa;
    if (common & ADV_100FULL) {
        *speed = 100;
        *full = 1;
    } else if (common & ADV_100HALF) {
        *speed = 100;
    } else if (common & ADV_10FULL) {
        *full = 1;
    }
    return 1;
}
