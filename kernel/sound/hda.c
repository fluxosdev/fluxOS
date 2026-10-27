/*
 * Intel High Definition Audio: the controller (Intel ICH/PCH, nVidia
 * MCP, ATI/AMD, VIA and QEMU's intel-hda) and the codec behind it, such
 * as the Realtek ALC888/ALC888S on nForce boards, as /dev/dsp and
 * /dev/mixer with the same OSS calls as the Sound Blaster driver.
 *
 * The controller talks to codecs through two rings in memory, CORB for
 * commands ("verbs") and RIRB for their answers, with the immediate
 * command registers as a fallback. A codec is a graph of widgets: DACs,
 * mixers, selectors and pin complexes (the jacks). The driver finds the
 * output jacks the BIOS describes (line out, speaker, headphone), walks
 * from each back to a DAC, and sets every widget on the way: connection
 * selected, amplifiers unmuted, pin output on, external amplifier (EAPD)
 * enabled. Every DAC used gets the same stream, so all the jacks play the
 * same sound. The ALC888 also gets the coefficient Linux sets on it.
 *
 * The codec always runs at 48 kHz (or 44.1 kHz), 16-bit stereo; what
 * write() is given (8 or 16 bits, mono or stereo, 5000 to 48000 Hz) is
 * converted on the way into a ring, with linear interpolation. The
 * controller plays from a DMA buffer in two halves; when it moves on to
 * one half the other is refilled from the ring. The halves are checked
 * at each buffer interrupt and on every timer tick, so a lost interrupt
 * does not stop the sound.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "syscall.h"
#include "vfs.h"

/* Controller registers */
#define GCAP      0x00
#define GCTL      0x08
#define STATESTS  0x0E
#define INTCTL    0x20
#define INTSTS    0x24
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4A
#define CORBCTL   0x4C
#define CORBSIZE  0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5A
#define RIRBCTL   0x5C
#define RIRBSTS   0x5D
#define RIRBSIZE  0x5E
#define ICOI      0x60
#define ICII      0x64
#define ICIS      0x68

#define GCTL_CRST     0x1
#define INTCTL_GIE    (1u << 31)
#define CORBRP_RST    0x8000
#define CORBCTL_RUN   0x02
#define RIRBWP_RST    0x8000
#define RIRBCTL_DMAEN 0x02
#define ICIS_BUSY     0x1
#define ICIS_VALID    0x2

/* Stream descriptor registers */
#define SD_CTL   0x00                   /* 24 bits, then SD_STS */
#define SD_STS   0x03
#define SD_LPIB  0x04
#define SD_CBL   0x08
#define SD_LVI   0x0C
#define SD_FMT   0x12
#define SD_BDPL  0x18
#define SD_BDPU  0x1C

#define SD_SRST  0x01
#define SD_RUN   0x02
#define SD_IOCE  0x04
#define SD_DIR   (1u << 19)
#define SD_STS_ACK 0x1C                 /* buffer complete, FIFO error, descriptor error */

/* Verbs (12-bit ones carry 8 bits, 4-bit ones 16) and parameters */
#define V_GET_PARAM     0xF0000
#define V_GET_CONN_LIST 0xF0200
#define V_SET_CONN_SEL  0x70100
#define V_SET_POWER     0x70500
#define V_SET_STREAM    0x70600
#define V_SET_PIN_CTL   0x70700
#define V_GET_EAPD      0xF0C00
#define V_SET_EAPD      0x70C00
#define V_GET_CONFIG    0xF1C00
#define V_SET_FORMAT    0x20000
#define V_SET_AMP       0x30000
#define V_SET_COEF_IDX  0x50000
#define V_SET_COEF      0x40000
#define V_GET_COEF      0xC0000

#define P_VENDOR    0x00
#define P_REVISION  0x02
#define P_NODES     0x04
#define P_FUNC_TYPE 0x05
#define P_WCAPS     0x09
#define P_PCM       0x0A
#define P_PINCAP    0x0C
#define P_AMP_IN    0x0D
#define P_CONN_LEN  0x0E
#define P_AMP_OUT   0x12

#define W_DAC      0
#define W_MIXER    2
#define W_SELECTOR 3
#define W_PIN      4

#define WC_IN_AMP   (1u << 1)
#define WC_OUT_AMP  (1u << 2)
#define WC_AMP_OVR  (1u << 3)
#define WC_CONN     (1u << 8)
#define WC_DIGITAL  (1u << 9)
#define WC_POWER    (1u << 10)

#define PC_OUT  (1u << 4)
#define PC_HP   (1u << 3)
#define PC_EAPD (1u << 16)

#define PIN_OUT 0x40
#define PIN_HP  0x80

#define AMP_OUT   0x8000
#define AMP_IN    0x4000
#define AMP_LR    0x3000
#define AMP_MUTE  0x80

#define MAX_NID   128
#define MAX_CONN  32
#define MAX_PATH  6
#define MAX_PINS  8
#define MAX_DACS  8
#define STREAM_TAG 1

#define HALF      7680                  /* 40 ms at 48 kHz, 16-bit stereo; a multiple of 128 */
#define RING_SIZE 65536
#define FRAME     4                     /* the codec's frame: 16-bit stereo */
#define MIN_RATE  5000
#define MAX_RATE  48000
#define MAX_PER_IN 12                   /* codec frames one input frame can become */

struct widget {
    uint32_t caps, pincap, config;
    uint32_t amp_in, amp_out;           /* amplifier capabilities */
    uint8_t type, nconn;
    uint8_t conn[MAX_CONN];
};

static struct {
    int present;
    volatile uint8_t *regs;
    int irq;
    uint32_t *corb, *rirb;
    uint32_t rirb_rp;
    int immediate;                      /* use the immediate command registers */
    int cad;                            /* the codec's address */
    int afg;
    uint32_t vendor, revision;
    char name[32];
    struct widget w[MAX_NID];
    int dacs[MAX_DACS], ndacs;
    int pins[MAX_PINS], npins;
    int nouts;                          /* jacks with a path to a DAC */
    uint32_t fmt;
    int out_rate;

    volatile uint8_t *sd;               /* the output stream's registers */
    int sindex, bidir;
    uint32_t *bdl;
    uint8_t *buf;                       /* the DMA buffer: two halves */
    int open_pid;

    int rate, channels, fmt_user;       /* what write() is given */
    uint8_t carry[4];
    uint32_t ncarry;
    int32_t prev_l, prev_r;             /* the resampler */
    uint32_t pos, step;

    int running, playing;
    uint32_t real[2];
    uint32_t progress;
    uint32_t rhead, rtail;
    uint8_t ring[RING_SIZE];

    int master, pcm;                    /* volumes, 0-100 */
    int hw_master;                      /* the DACs have a volume control */
} hda;

static inline uint32_t rd32(int r) { return *(volatile uint32_t *)(hda.regs + r); }
static inline uint16_t rd16(int r) { return *(volatile uint16_t *)(hda.regs + r); }
static inline uint8_t rd8(int r) { return *(volatile uint8_t *)(hda.regs + r); }
static inline void wr32(int r, uint32_t v) { *(volatile uint32_t *)(hda.regs + r) = v; }
static inline void wr16(int r, uint16_t v) { *(volatile uint16_t *)(hda.regs + r) = v; }
static inline void wr8(int r, uint8_t v) { *(volatile uint8_t *)(hda.regs + r) = v; }
static inline uint32_t sd_rd32(int r) { return *(volatile uint32_t *)(hda.sd + r); }
static inline void sd_wr32(int r, uint32_t v) { *(volatile uint32_t *)(hda.sd + r) = v; }
static inline void sd_wr16(int r, uint16_t v) { *(volatile uint16_t *)(hda.sd + r) = v; }
static inline void sd_wr8(int r, uint8_t v) { *(volatile uint8_t *)(hda.sd + r) = v; }

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* ---- talking to the codec ---- */

static int corb_cmd(uint32_t verb, uint32_t *resp)
{
    uint16_t wp = (uint16_t)((rd16(CORBWP) + 1) & 0xFF);
    hda.corb[wp] = verb;
    barrier();
    wr16(CORBWP, wp);
    for (int i = 0; i < 2500; i++) {    /* 50 ms */
        int got = 0;
        uint32_t rwp = rd16(RIRBWP) & 0xFF;
        while (hda.rirb_rp != rwp) {
            hda.rirb_rp = (hda.rirb_rp + 1) & 0xFF;
            uint32_t r = hda.rirb[hda.rirb_rp * 2], ex = hda.rirb[hda.rirb_rp * 2 + 1];
            if (ex & 0x10)
                continue;               /* unsolicited */
            *resp = r;
            got = 1;
        }
        if (got) {
            wr8(RIRBSTS, 0x05);
            return 0;
        }
        udelay(20);
    }
    return -1;
}

static int immediate_cmd(uint32_t verb, uint32_t *resp)
{
    for (int i = 0; i < 1000 && (rd16(ICIS) & ICIS_BUSY); i++)
        udelay(1);
    wr16(ICIS, ICIS_VALID);
    wr32(ICOI, verb);
    wr16(ICIS, ICIS_BUSY);
    for (int i = 0; i < 2500; i++) {
        if ((rd16(ICIS) & (ICIS_BUSY | ICIS_VALID)) == ICIS_VALID) {
            *resp = rd32(ICII);
            return 0;
        }
        udelay(20);
    }
    return -1;
}

/* Send a verb (with its payload, 20 bits) to node nid; 0xFFFFFFFF if no answer. */
static uint32_t cmd(int nid, uint32_t verb)
{
    uint32_t v = (uint32_t)hda.cad << 28 | (uint32_t)nid << 20 | (verb & 0xFFFFF), r = 0;
    if (!hda.immediate) {
        if (corb_cmd(v, &r) == 0)
            return r;
        klog("hda: no answer through the command ring, using immediate commands\n");
        hda.immediate = 1;
        wr8(CORBCTL, 0);                /* the two must not run together */
        wr8(RIRBCTL, 0);
    }
    return immediate_cmd(v, &r) == 0 ? r : 0xFFFFFFFFu;
}

static uint32_t param(int nid, int p)
{
    return cmd(nid, V_GET_PARAM | (uint32_t)p);
}

static uint32_t coef_read(int nid, int idx)
{
    cmd(nid, V_SET_COEF_IDX | (uint32_t)idx);
    return cmd(nid, V_GET_COEF) & 0xFFFF;
}

static void coef_write(int nid, int idx, uint32_t v)
{
    cmd(nid, V_SET_COEF_IDX | (uint32_t)idx);
    cmd(nid, V_SET_COEF | (v & 0xFFFF));
}

/* ---- the controller ---- */

static int controller_reset(void)
{
    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);
    for (int i = 0; i < 100 && ((rd8(CORBCTL) & CORBCTL_RUN) || (rd8(RIRBCTL) & RIRBCTL_DMAEN)); i++)
        udelay(10);
    wr16(STATESTS, 0x7FFF);
    wr32(GCTL, rd32(GCTL) & ~GCTL_CRST);
    for (int i = 0; i < 1000 && (rd32(GCTL) & GCTL_CRST); i++)
        udelay(100);
    udelay(200);
    wr32(GCTL, rd32(GCTL) | GCTL_CRST);
    for (int i = 0; i < 1000 && !(rd32(GCTL) & GCTL_CRST); i++)
        udelay(100);
    if (!(rd32(GCTL) & GCTL_CRST))
        return -1;
    /* Codecs announce themselves within 521 us of the reset; give them more. */
    for (int i = 0; i < 100 && !rd16(STATESTS); i++)
        mdelay(1);
    return 0;
}

static void rings_init(uint8_t *page)
{
    hda.corb = (uint32_t *)page;                        /* 256 x 4 bytes */
    hda.rirb = (uint32_t *)(page + 0x400);              /* 256 x 8 bytes */
    uint8_t cs = rd8(CORBSIZE), rs = rd8(RIRBSIZE);
    if (cs & 0x40)
        wr8(CORBSIZE, (uint8_t)((cs & ~3) | 2));        /* 256 entries */
    if (rs & 0x40)
        wr8(RIRBSIZE, (uint8_t)((rs & ~3) | 2));
    wr32(CORBLBASE, (uint32_t)hda.corb);
    wr32(CORBUBASE, 0);
    wr32(RIRBLBASE, (uint32_t)hda.rirb);
    wr32(RIRBUBASE, 0);

    /* Reset the CORB read pointer: set the bit, see it set (some, such as
     * nVidia's, clear it by themselves), clear it. */
    wr16(CORBRP, CORBRP_RST);
    for (int i = 0; i < 1000 && !(rd16(CORBRP) & CORBRP_RST); i++)
        udelay(1);
    wr16(CORBRP, 0);
    for (int i = 0; i < 1000 && (rd16(CORBRP) & CORBRP_RST); i++)
        udelay(1);
    wr16(CORBWP, 0);
    wr16(RIRBWP, RIRBWP_RST);
    wr16(RINTCNT, 1);
    hda.rirb_rp = 0;
    wr8(CORBCTL, CORBCTL_RUN);
    wr8(RIRBCTL, RIRBCTL_DMAEN);
}

/* Chipset settings Linux makes: no traffic class, and snooped DMA. */
static void pci_quirks(const struct pci_dev *pci)
{
    uint32_t id = pci_read32(pci, 0);
    uint16_t vendor = (uint16_t)id, device = (uint16_t)(id >> 16);
    pci_write8(pci, 0x44, pci_read8(pci, 0x44) & ~7);  /* TCSEL */
    if (vendor == 0x10DE) {
        pci_write8(pci, 0x4E, (uint8_t)((pci_read8(pci, 0x4E) & ~0x0F) | 0x0F));
        pci_write8(pci, 0x4D, pci_read8(pci, 0x4D) | 0x01);
        pci_write8(pci, 0x4C, pci_read8(pci, 0x4C) | 0x01);
    } else if (vendor == 0x1002 && (device == 0x437B || device == 0x4383)) {
        pci_write8(pci, 0x42, (uint8_t)((pci_read8(pci, 0x42) & ~0x07) | 0x02));
    }
}

/* ---- the codec's widgets ---- */

static uint32_t amp_caps(int nid, int out)
{
    struct widget *w = &hda.w[nid];
    int p = out ? P_AMP_OUT : P_AMP_IN;
    return (w->caps & WC_AMP_OVR) ? param(nid, p) : param(hda.afg, p);
}

static void read_connections(int nid)
{
    struct widget *w = &hda.w[nid];
    uint32_t len = param(nid, P_CONN_LEN);
    int lng = (len & 0x80) != 0, n = (int)(len & 0x7F), per = lng ? 2 : 4;
    int prev = 0;
    for (int i = 0; i < n; i += per) {
        uint32_t r = cmd(nid, V_GET_CONN_LIST | (uint32_t)i);
        for (int k = 0; k < per && i + k < n; k++) {
            uint32_t e = lng ? (r >> (16 * k)) & 0xFFFF : (r >> (8 * k)) & 0xFF;
            int range = lng ? (e & 0x8000) != 0 : (e & 0x80) != 0;
            int v = (int)(e & (lng ? 0x7FFF : 0x7F));
            if (range && prev) {
                for (int x = prev + 1; x <= v && w->nconn < MAX_CONN; x++)
                    w->conn[w->nconn++] = (uint8_t)x;
            } else if (w->nconn < MAX_CONN) {
                w->conn[w->nconn++] = (uint8_t)v;
            }
            prev = v;
        }
    }
}

static int scan_widgets(void)
{
    uint32_t nodes = param(hda.afg, P_NODES);
    int first = (int)((nodes >> 16) & 0xFF), count = (int)(nodes & 0xFF);
    if (!count)
        return -1;
    for (int nid = first; nid < first + count && nid < MAX_NID; nid++) {
        struct widget *w = &hda.w[nid];
        w->caps = param(nid, P_WCAPS);
        w->type = (uint8_t)((w->caps >> 20) & 0xF);
        if (w->caps & WC_IN_AMP)
            w->amp_in = amp_caps(nid, 0);
        if (w->caps & WC_OUT_AMP)
            w->amp_out = amp_caps(nid, 1);
        if (w->type == W_PIN) {
            w->pincap = param(nid, P_PINCAP);
            w->config = cmd(nid, V_GET_CONFIG);
        }
        if (w->caps & WC_CONN)
            read_connections(nid);
    }
    return 0;
}

static int is_analog_dac(int nid)
{
    struct widget *w = &hda.w[nid];
    return w->type == W_DAC && w->caps && !(w->caps & WC_DIGITAL);
}

/* Depth-first from a pin to a DAC (want, or any if 0), through mixers and
 * selectors. path[] gets the nodes and idx[] the connection taken at each. */
static int find_path(int nid, int want, int depth, int *path, int *idx, int *len)
{
    if (nid <= 0 || nid >= MAX_NID)
        return 0;
    struct widget *w = &hda.w[nid];
    if (is_analog_dac(nid)) {
        if (want && nid != want)
            return 0;
        path[depth] = nid;
        *len = depth + 1;
        return 1;
    }
    if (depth >= MAX_PATH - 1 || (w->type == W_PIN ? depth != 0 :
                                  w->type != W_MIXER && w->type != W_SELECTOR))
        return 0;
    for (int i = 0; i < w->nconn; i++) {
        if (find_path(w->conn[i], want, depth + 1, path, idx, len)) {
            path[depth] = nid;
            idx[depth] = i;
            return 1;
        }
    }
    return 0;
}

static uint32_t amp_steps(uint32_t caps)
{
    return (caps >> 8) & 0x7F;
}

/* The gain for 0 dB, or the top if the amplifier only attenuates. */
static uint32_t amp_unity(uint32_t caps)
{
    uint32_t off = caps & 0x7F;
    return off && off <= amp_steps(caps) ? off : amp_steps(caps);
}

static void set_amp(int nid, int out, int index, uint32_t gain, int mute)
{
    cmd(nid, V_SET_AMP | (out ? AMP_OUT : AMP_IN) | AMP_LR | (uint32_t)index << 8 |
             (mute ? AMP_MUTE : 0) | (gain & 0x7F));
}

/* The DACs' volume: mute at 0, then the top three quarters of their range. */
static void apply_master(void)
{
    for (int i = 0; i < hda.ndacs; i++) {
        int nid = hda.dacs[i];
        uint32_t caps = hda.w[nid].amp_out, top = amp_unity(caps);
        if (!(hda.w[nid].caps & WC_OUT_AMP))
            continue;
        uint32_t gain = top - (uint32_t)(100 - hda.master) * (top * 3 / 4) / 100;
        set_amp(nid, 1, 0, gain, hda.master == 0);
    }
}

static void setup_path(const int *path, const int *idx, int len, int hp)
{
    for (int k = 0; k < len; k++) {
        int nid = path[k];
        struct widget *w = &hda.w[nid];
        if (w->caps & WC_POWER)
            cmd(nid, V_SET_POWER | 0);
        if (k < len - 1) {
            if (w->type == W_MIXER) {
                /* Only the input on the path: the others (the analog
                 * loopback of the inputs, on Realtek codecs) stay muted. */
                for (int i = 0; i < w->nconn && (w->caps & WC_IN_AMP); i++)
                    set_amp(nid, 0, i, amp_unity(w->amp_in), i != idx[k]);
            } else {
                if (w->nconn > 1)
                    cmd(nid, V_SET_CONN_SEL | (uint32_t)idx[k]);
                if (w->caps & WC_IN_AMP)
                    set_amp(nid, 0, idx[k], amp_unity(w->amp_in), 0);
            }
        }
        if ((w->caps & WC_OUT_AMP) && w->type != W_DAC)
            set_amp(nid, 1, 0, amp_unity(w->amp_out), 0);
    }
    int pin = path[0];
    struct widget *p = &hda.w[pin];
    cmd(pin, V_SET_PIN_CTL | PIN_OUT | (hp && (p->pincap & PC_HP) ? PIN_HP : 0));
    if (p->pincap & PC_EAPD)
        cmd(pin, V_SET_EAPD | ((cmd(pin, V_GET_EAPD) & 0xFF) | 0x02));
}

/* Output jacks, in order: line outs, speakers, headphones; by the BIOS's
 * association and sequence within each. */
static void find_pins(int any)
{
    hda.npins = 0;
    int keys[MAX_PINS];
    for (int nid = 1; nid < MAX_NID; nid++) {
        struct widget *w = &hda.w[nid];
        if (w->type != W_PIN || !w->caps || !(w->pincap & PC_OUT) || (w->caps & WC_DIGITAL))
            continue;
        int conn = (int)(w->config >> 30), dev = (int)((w->config >> 20) & 0xF);
        if (!any && (conn == 1 || dev > 2))
            continue;                   /* not connected, or not a speaker output */
        int key = (any ? 0 : dev) << 8 | (int)(w->config & 0xFF);
        if (hda.npins == MAX_PINS)
            break;
        int i = hda.npins++;
        while (i > 0 && keys[i - 1] > key) {
            keys[i] = keys[i - 1];
            hda.pins[i] = hda.pins[i - 1];
            i--;
        }
        keys[i] = key;
        hda.pins[i] = nid;
    }
}

static void add_dac(int nid)
{
    for (int i = 0; i < hda.ndacs; i++)
        if (hda.dacs[i] == nid)
            return;
    if (hda.ndacs < MAX_DACS)
        hda.dacs[hda.ndacs++] = nid;
}

/* The Realtek ALC888: EAPD high through coefficient 7 on the -VA and -VB
 * revisions, as Linux's alc888_coef_init does. */
static void realtek_init(void)
{
    if (hda.vendor != 0x10EC0888)
        return;
    uint32_t coef0 = coef_read(0x20, 0);
    if ((coef0 & 0xF0) == 0x00 || (coef0 & 0xF0) == 0x10)
        coef_write(0x20, 7, coef_read(0x20, 7) | 0x2030);
}

static void codec_name(void)
{
    uint16_t v = (uint16_t)(hda.vendor >> 16), d = (uint16_t)hda.vendor;
    if (v == 0x10EC)
        snprintf(hda.name, sizeof(hda.name), "Realtek ALC%x", d);
    else if (v == 0x1AF4)
        snprintf(hda.name, sizeof(hda.name), "QEMU HDA codec");
    else if (v == 0x10DE)
        snprintf(hda.name, sizeof(hda.name), "nVidia HDA codec");
    else
        snprintf(hda.name, sizeof(hda.name), "HDA codec %04x:%04x", v, d);
}

/* Set up codec cad for output. Returns 0 if it has a usable output. */
static int codec_init(int cad)
{
    memset(hda.w, 0, sizeof(hda.w));
    hda.cad = cad;
    hda.afg = 0;
    hda.vendor = param(0, P_VENDOR);
    if (hda.vendor == 0xFFFFFFFFu || !hda.vendor)
        return -1;
    hda.revision = param(0, P_REVISION);
    uint32_t nodes = param(0, P_NODES);
    for (int nid = (int)((nodes >> 16) & 0xFF), n = (int)(nodes & 0xFF); n > 0; nid++, n--)
        if ((param(nid, P_FUNC_TYPE) & 0xFF) == 1)
            hda.afg = nid;              /* the audio function group */
    if (!hda.afg)
        return -1;
    cmd(hda.afg, V_SET_POWER | 0);      /* D0 */
    mdelay(10);
    if (scan_widgets() < 0)
        return -1;
    codec_name();
    realtek_init();

    find_pins(0);
    if (!hda.npins)
        find_pins(1);                   /* the BIOS described nothing: try every output */
    hda.ndacs = 0;
    hda.nouts = 0;
    int main_dac = 0;
    for (int i = 0; i < hda.npins; i++) {
        int path[MAX_PATH], idx[MAX_PATH], len = 0;
        if (!(main_dac && find_path(hda.pins[i], main_dac, 0, path, idx, &len)) &&
            !find_path(hda.pins[i], 0, 0, path, idx, &len))
            continue;
        int dac = path[len - 1];
        if (!main_dac)
            main_dac = dac;
        add_dac(dac);
        int hp = ((hda.w[hda.pins[i]].config >> 20) & 0xF) == 2;
        setup_path(path, idx, len, hp);
        hda.nouts++;
    }
    if (!hda.ndacs)
        return -1;

    /* 48 kHz if the DAC can, else 44.1; always 16-bit stereo. */
    uint32_t pcm = param(main_dac, P_PCM);
    if (!pcm || pcm == 0xFFFFFFFFu)
        pcm = param(hda.afg, P_PCM);
    hda.out_rate = (pcm & (1u << 6)) || !(pcm & (1u << 5)) ? 48000 : 44100;
    hda.fmt = (hda.out_rate == 44100 ? 0x4000 : 0) | 1 << 4 | 1;
    hda.hw_master = 0;
    for (int i = 0; i < hda.ndacs; i++) {
        int dac = hda.dacs[i];
        if (hda.w[dac].caps & WC_POWER)
            cmd(dac, V_SET_POWER | 0);
        cmd(dac, V_SET_FORMAT | hda.fmt);
        cmd(dac, V_SET_STREAM | STREAM_TAG << 4);
        if ((hda.w[dac].caps & WC_OUT_AMP) && amp_steps(hda.w[dac].amp_out))
            hda.hw_master = 1;
    }
    apply_master();
    return 0;
}

/* ---- playback ---- */

static uint32_t ring_used(void)
{
    return hda.rhead - hda.rtail;
}

static void push_sample(int32_t s)
{
    hda.ring[hda.rhead++ % RING_SIZE] = (uint8_t)s;
    hda.ring[hda.rhead++ % RING_SIZE] = (uint8_t)(s >> 8);
}

static uint32_t user_frame(void)
{
    return (uint32_t)hda.channels * (hda.fmt_user == AFMT_S16_LE ? 2 : 1);
}

static int32_t volume(void)
{
    int v = hda.pcm * (hda.hw_master ? 100 : hda.master) / 100;
    return v * 256 / 100;
}

/* One input frame into the ring as codec frames, interpolating between it
 * and the one before at the codec's rate. */
static void put_frame(const uint8_t *in)
{
    int32_t s[2] = { 0, 0 };
    for (int c = 0; c < hda.channels; c++) {
        if (hda.fmt_user == AFMT_S16_LE) {
            s[c] = (int16_t)(in[0] | in[1] << 8);
            in += 2;
        } else {
            s[c] = (*in++ - 128) << 8;
        }
    }
    if (hda.channels == 1)
        s[1] = s[0];
    int32_t vol = volume();
    int32_t l = s[0] * vol / 256, r = s[1] * vol / 256;
    while (hda.pos < 0x10000) {
        int32_t f = (int32_t)(hda.pos >> 2);            /* 14 bits: no overflow below */
        push_sample(hda.prev_l + (((l - hda.prev_l) * f) >> 14));
        push_sample(hda.prev_r + (((r - hda.prev_r) * f) >> 14));
        hda.pos += hda.step;
    }
    hda.pos -= 0x10000;
    hda.prev_l = l;
    hda.prev_r = r;
}

static void fill_half(int h)
{
    uint8_t *dst = hda.buf + h * HALF;
    uint32_t n = MIN(ring_used(), (uint32_t)HALF);
    n -= n % FRAME;
    for (uint32_t i = 0; i < n; i++)
        dst[i] = hda.ring[hda.rtail++ % RING_SIZE];
    memset(dst + n, 0, HALF - n);
    hda.real[h] = n;
}

static void stream_reset(void)
{
    uint32_t ctl = sd_rd32(SD_CTL) & 0x00FFFFFF;
    sd_wr32(SD_CTL, ctl & ~SD_RUN);
    for (int i = 0; i < 1000 && (sd_rd32(SD_CTL) & SD_RUN); i++)
        udelay(10);
    sd_wr32(SD_CTL, (ctl & ~SD_RUN) | SD_SRST);
    for (int i = 0; i < 1000 && !(sd_rd32(SD_CTL) & SD_SRST); i++)
        udelay(10);
    sd_wr32(SD_CTL, ctl & ~(SD_RUN | SD_SRST));
    for (int i = 0; i < 1000 && (sd_rd32(SD_CTL) & SD_SRST); i++)
        udelay(10);
    sd_wr8(SD_STS, SD_STS_ACK);
}

static void stop(void)
{
    if (!hda.running)
        return;
    sd_wr32(SD_CTL, sd_rd32(SD_CTL) & 0x00FFFFFF & ~SD_RUN);
    for (int i = 0; i < 1000 && (sd_rd32(SD_CTL) & SD_RUN); i++)
        udelay(10);
    sd_wr8(SD_STS, SD_STS_ACK);
    hda.running = 0;
    hda.real[0] = hda.real[1] = 0;
    wakeup(&hda);
    poll_wakeup();
}

static void start(void)
{
    if (hda.running || !ring_used())
        return;
    fill_half(0);
    fill_half(1);
    stream_reset();
    for (int h = 0; h < 2; h++) {
        hda.bdl[h * 4] = (uint32_t)(hda.buf + h * HALF);
        hda.bdl[h * 4 + 1] = 0;
        hda.bdl[h * 4 + 2] = HALF;
        hda.bdl[h * 4 + 3] = 1;         /* interrupt on completion */
    }
    sd_wr32(SD_BDPL, (uint32_t)hda.bdl);
    sd_wr32(SD_BDPU, 0);
    sd_wr32(SD_CBL, 2 * HALF);
    sd_wr16(SD_LVI, 1);
    sd_wr16(SD_FMT, (uint16_t)hda.fmt);
    uint32_t ctl = (uint32_t)STREAM_TAG << 20 | SD_IOCE | (hda.bidir ? SD_DIR : 0);
    sd_wr32(SD_CTL, ctl);
    barrier();
    sd_wr32(SD_CTL, ctl | SD_RUN);
    hda.playing = 0;
    hda.running = 1;
}

/* The controller moved on to the other half: refill the one it left. */
static void service(void)
{
    if (!hda.running)
        return;
    int h = sd_rd32(SD_LPIB) >= HALF ? 1 : 0;
    if (h == hda.playing)
        return;
    fill_half(hda.playing);
    hda.playing = h;
    hda.progress++;
    if (!hda.real[0] && !hda.real[1] && !ring_used())
        stop();                         /* only silence left to play */
    wakeup(&hda);
    poll_wakeup();
}

static void hda_irq_locked(struct trapframe *tf)
{
    (void)tf;
    uint32_t st = rd32(INTSTS);
    if (!(st & (1u << hda.sindex)))
        return;
    sd_wr8(SD_STS, SD_STS_ACK);
    service();
}

/* From the timer interrupt; the card's own interrupt does it too. */
void hda_tick(void)
{
    if (!hda.present || !hda.running || !spin_trylock(&snd_lock))
        return;
    service();
    spin_unlock(&snd_lock);
}

/* Sleep until the buffer moves on; -EIO if it has stopped moving. */
static int wait_progress(void)
{
    uint32_t seen = hda.progress;
    if (!hda.running)
        return -EIO;
    sleep_on_lock_timeout(&hda, &snd_lock, ticks + HZ);
    if (signal_pending(current))
        return -EINTR;
    if (hda.running && hda.progress == seen) {
        klog("hda: the stream does not advance, stopping playback\n");
        stop();
        hda.rtail = hda.rhead;
        return -EIO;
    }
    return 0;
}

static int drain(void)
{
    while (hda.running || ring_used()) {
        start();
        if (!hda.running)
            break;
        int r = wait_progress();
        if (r < 0)
            return r;
    }
    return 0;
}

/* ---- /dev/dsp ---- */

static void reset_resampler(void)
{
    hda.pos = 0;
    hda.prev_l = hda.prev_r = 0;
    hda.step = (uint32_t)(((uint32_t)hda.rate << 16) / (uint32_t)hda.out_rate);
}

static void set_defaults(void)
{
    hda.rate = 8000;
    hda.channels = 1;
    hda.fmt_user = AFMT_U8;
    hda.ncarry = 0;
    reset_resampler();
}

static int dsp_open_locked(struct vnode *vn, int flags)
{
    (void)vn;
    if ((flags & O_ACCMODE) == O_RDONLY)
        return -EINVAL;                 /* no recording */
    if (hda.open_pid && hda.open_pid != current->pid) {
        struct proc *p = proc_find(hda.open_pid);
        if (p && p->state != P_ZOMBIE)
            return -EBUSY;
    }
    hda.open_pid = current->pid;
    if (!hda.running && !ring_used())
        set_defaults();
    return 0;
}

static void dsp_close_locked(struct vnode *vn, int flags)
{
    (void)vn; (void)flags;
    hda.open_pid = 0;
    hda.ncarry = 0;
    start();                            /* the rest plays out by itself */
}

static int make_room(void)
{
    while (RING_SIZE - ring_used() < MAX_PER_IN * FRAME) {
        start();
        int r = wait_progress();
        if (r < 0)
            return r;
    }
    return 0;
}

static int dsp_write_locked(struct vnode *vn, const void *buf, uint64_t off, uint32_t n)
{
    (void)vn; (void)off;
    const uint8_t *in = buf;
    uint32_t uf = user_frame(), used = 0;
    while (hda.ncarry && used < n) {
        hda.carry[hda.ncarry++] = in[used++];
        if (hda.ncarry == uf) {
            int r = make_room();
            if (r < 0)
                return r;
            put_frame(hda.carry);
            hda.ncarry = 0;
        }
    }
    while (n - used >= uf) {
        if (RING_SIZE - ring_used() < MAX_PER_IN * FRAME) {
            int r = make_room();
            if (r < 0)
                return used ? (int)used : r;
        }
        while (n - used >= uf && RING_SIZE - ring_used() >= MAX_PER_IN * FRAME) {
            put_frame(in + used);
            used += uf;
        }
        if (!hda.running && ring_used() >= 2 * HALF)
            start();
    }
    while (used < n)
        hda.carry[hda.ncarry++] = in[used++];
    return (int)n;
}

static int dsp_poll_locked(struct vnode *vn)
{
    (void)vn;
    return RING_SIZE - ring_used() >= HALF ? POLLOUT : 0;
}

static int set_format(int *field, int value)
{
    if (*field == value)
        return 0;
    int r = drain();
    if (r < 0)
        return r;
    *field = value;
    hda.ncarry = 0;
    reset_resampler();
    return 0;
}

static int get_int(uint32_t arg, int *v)
{
    if (!user_access_ok((void *)arg, sizeof(int)))
        return -EFAULT;
    *v = *(int *)arg;
    return 0;
}

/* Codec bytes as the bytes of write()'s format that make them. */
static uint32_t user_bytes(uint32_t codec_bytes)
{
    uint32_t frames = codec_bytes / FRAME;
    return frames * (uint32_t)hda.rate / (uint32_t)hda.out_rate * user_frame();
}

static int dsp_ioctl_locked(struct vnode *vn, uint32_t req, uint32_t arg)
{
    (void)vn;
    int v, r;
    switch (req) {
    case SNDCTL_DSP_RESET:
        stop();
        hda.rtail = hda.rhead;
        hda.ncarry = 0;
        reset_resampler();
        return 0;
    case SNDCTL_DSP_SYNC:
        return drain();
    case SNDCTL_DSP_POST:
        start();
        return 0;
    case SNDCTL_DSP_SPEED:
        if ((r = get_int(arg, &v)) < 0)
            return r;
        v = v < MIN_RATE ? MIN_RATE : v > MAX_RATE ? MAX_RATE : v;
        if ((r = set_format(&hda.rate, v)) < 0)
            return r;
        *(int *)arg = hda.rate;
        return 0;
    case SNDCTL_DSP_CHANNELS:
        if ((r = get_int(arg, &v)) < 0)
            return r;
        if ((r = set_format(&hda.channels, v >= 2 ? 2 : 1)) < 0)
            return r;
        *(int *)arg = hda.channels;
        return 0;
    case SNDCTL_DSP_SETFMT:
        if ((r = get_int(arg, &v)) < 0)
            return r;
        if ((r = set_format(&hda.fmt_user, v == AFMT_S16_LE ? AFMT_S16_LE : AFMT_U8)) < 0)
            return r;
        *(int *)arg = hda.fmt_user;
        return 0;
    case SNDCTL_DSP_GETOSPACE: {
        if (!user_access_ok((void *)arg, sizeof(struct audio_buf_info)))
            return -EFAULT;
        struct audio_buf_info *bi = (struct audio_buf_info *)arg;
        uint32_t room = RING_SIZE - ring_used();
        room = room > MAX_PER_IN * FRAME ? room - MAX_PER_IN * FRAME : 0;
        uint32_t frag = user_bytes(HALF);
        bi->bytes = (int32_t)user_bytes(room);
        bi->fragsize = (int32_t)frag;
        bi->fragstotal = RING_SIZE / HALF;
        bi->fragments = frag ? bi->bytes / (int32_t)frag : 0;
        return 0;
    }
    case SNDCTL_DSP_GETODELAY: {
        if (!user_access_ok((void *)arg, sizeof(int)))
            return -EFAULT;
        uint32_t queued = ring_used() + (hda.running ? hda.real[0] + hda.real[1] : 0);
        *(int *)arg = (int)user_bytes(queued);
        return 0;
    }
    case SOUND_INFO:
        if (!user_access_ok((void *)arg, sizeof(struct sound_info)))
            return -EFAULT;
        sound_get_info((struct sound_info *)arg);
        return 0;
    }
    return -EINVAL;
}

/* ---- /dev/mixer: master (the DACs' amplifiers) and pcm (in software) ---- */

static int mixer_ioctl_locked(struct vnode *vn, uint32_t req, uint32_t arg)
{
    (void)vn;
    if (req == SOUND_INFO) {
        if (!user_access_ok((void *)arg, sizeof(struct sound_info)))
            return -EFAULT;
        sound_get_info((struct sound_info *)arg);
        return 0;
    }
    if (req != MIXER_READ && req != MIXER_WRITE)
        return -EINVAL;
    if (!user_access_ok((void *)arg, sizeof(struct mixer_level)))
        return -EFAULT;
    struct mixer_level *ml = (struct mixer_level *)arg;
    int *level = ml->channel == MIXER_MASTER ? &hda.master :
                 ml->channel == MIXER_PCM ? &hda.pcm : NULL;
    if (!level)
        return -EINVAL;
    if (req == MIXER_READ) {
        ml->left = ml->right = (uint32_t)*level;
        return 0;
    }
    uint32_t v = (ml->left + ml->right) / 2;
    *level = (int)MIN(v, 100u);
    if (ml->channel == MIXER_MASTER && hda.hw_master)
        apply_master();
    return 0;
}

/* ---- the device calls and the interrupt, under snd_lock ---- */

static int dsp_open(struct vnode *vn, int flags)
{
    spin_lock(&snd_lock);
    int r = dsp_open_locked(vn, flags);
    spin_unlock(&snd_lock);
    return r;
}

static void dsp_close(struct vnode *vn, int flags)
{
    spin_lock(&snd_lock);
    dsp_close_locked(vn, flags);
    spin_unlock(&snd_lock);
}

static int dsp_poll(struct vnode *vn)
{
    spin_lock(&snd_lock);
    int r = dsp_poll_locked(vn);
    spin_unlock(&snd_lock);
    return r;
}

static int dsp_ioctl(struct vnode *vn, uint32_t req, uint32_t arg)
{
    spin_lock(&snd_lock);
    int r = dsp_ioctl_locked(vn, req, arg);
    spin_unlock(&snd_lock);
    return r;
}

static int mixer_ioctl(struct vnode *vn, uint32_t req, uint32_t arg)
{
    spin_lock(&snd_lock);
    int r = mixer_ioctl_locked(vn, req, arg);
    spin_unlock(&snd_lock);
    return r;
}

static int dsp_write(struct vnode *vn, const void *buf, uint64_t off, uint32_t n)
{
    spin_lock(&snd_lock);
    int r = dsp_write_locked(vn, buf, off, n);
    spin_unlock(&snd_lock);
    return r;
}

static void hda_irq(struct trapframe *tf)
{
    spin_lock(&snd_lock);
    hda_irq_locked(tf);
    spin_unlock(&snd_lock);
}

/* ---- setup ---- */

int hda_get_info(struct sound_info *si)
{
    if (!hda.present)
        return 0;
    snprintf(si->card, sizeof(si->card), "%s", hda.name);
    si->irq = (uint8_t)(hda.irq >= 0 ? hda.irq : 0);
    si->flags |= SOUNDF_DSP | SOUNDF_HDA;
    return 1;
}

static int controller_init(const struct pci_dev *pci, uint8_t *page)
{
    hda.regs = pci_map_bar(pci, PCI_BAR0);
    if (!hda.regs)
        return -1;
    memset(page, 0, PAGE_SIZE);
    hda.bdl = (uint32_t *)(page + 0xC00);               /* 128-byte aligned */
    hda.immediate = 0;
    hda.bidir = 0;
    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);
    pci_quirks(pci);
    wr32(INTCTL, 0);
    if (controller_reset() < 0) {
        klog("hda: controller did not come out of reset\n");
        return -1;
    }
    rings_init(page);

    uint16_t gcap = rd16(GCAP);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF, bss = (gcap >> 3) & 0x1F;
    if (oss) {
        hda.sindex = iss;
    } else if (bss) {
        hda.sindex = iss + oss;
        hda.bidir = 1;
    } else {
        return -1;
    }
    hda.sd = hda.regs + 0x80 + 0x20 * hda.sindex;

    uint16_t codecs = rd16(STATESTS);
    for (int cad = 0; cad < 15; cad++)
        if ((codecs & (1 << cad)) && codec_init(cad) == 0)
            return 0;
    klog("hda: no codec with an analog output (codecs present: %04x)\n", codecs);
    return -1;
}

void hda_init(struct vnode *devdir)
{
    if (cmdline_has("hda=off"))
        return;
    /* The first controller with an analog codec: a graphics card's
     * (HDMI only) may come before the chipset's. */
    struct pci_dev pci;
    uint8_t prog_if;
    uint32_t page = 0, buf = 0;
    int found = 0;
    for (int i = 0; !found && pci_find_class_nth(0x0403, i, &pci, &prog_if) == 0; i++) {
        if (!page && (!(page = pmm_alloc()) || !(buf = pmm_alloc_contig(4))))
            return;
        hda.irq = pci_irq(&pci);
        found = controller_init(&pci, (uint8_t *)page) == 0;
        if (!found && hda.regs)
            wr32(GCTL, rd32(GCTL) & ~GCTL_CRST);        /* leave it in reset */
    }
    if (!found)
        return;
    hda.buf = (uint8_t *)buf;
    memset(hda.buf, 0, 4 * PAGE_SIZE);
    hda.master = 80;
    hda.pcm = 100;
    apply_master();
    set_defaults();
    hda.present = 1;

    /* /dev/dsp, unless a Sound Blaster has it already. */
    struct vnode *old;
    int second = vfs_resolve("/dev/dsp", NULL, &old) == 0;
    struct vnode *dsp = vfs_mknod(devdir, second ? "dsp1" : "dsp",
                                  MKDEV(SOUND_MAJOR, SOUND_MINOR_DSP + (second ? 0x40 : 0)),
                                  NULL, dsp_write);
    if (dsp) {
        dsp->dev_open = dsp_open;
        dsp->dev_close = dsp_close;
        dsp->dev_ioctl = dsp_ioctl;
        dsp->dev_poll = dsp_poll;
    }
    struct vnode *mixer = vfs_mknod(devdir, second ? "mixer1" : "mixer",
                                    MKDEV(SOUND_MAJOR, SOUND_MINOR_MIXER + (second ? 0x40 : 0)),
                                    NULL, NULL);
    if (mixer)
        mixer->dev_ioctl = mixer_ioctl;
    if (hda.irq >= 0) {
        irq_register(hda.irq, hda_irq);
        wr32(INTCTL, INTCTL_GIE | 1u << hda.sindex);
    }
    boot_ok("HD Audio: %s (rev %06x) on codec %d, %d output%s, %d Hz, IRQ %d%s", hda.name,
            hda.revision >> 8, hda.cad, hda.nouts, hda.nouts == 1 ? "" : "s", hda.out_rate,
            hda.irq, second ? " (/dev/dsp1)" : "");
}
