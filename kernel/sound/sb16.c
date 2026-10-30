/*
 * Creative Sound Blaster 16: digital audio out (/dev/dsp) and the mixer
 * (/dev/mixer). Older Sound Blasters (DSP versions below 4) are left alone.
 *
 * The card is found by resetting its DSP at 220h, 240h, 260h or 280h; its
 * mixer says which IRQ and DMA channels it uses (registers 80h and 81h).
 *
 * Playback runs from a DMA buffer in two halves, with the DSP in auto-init
 * mode and an interrupt each time a half has played; the interrupt refills
 * that half from a ring of samples that write() fills. With a 16-bit DMA
 * channel the card is always fed signed 16-bit samples (8-bit ones are
 * widened as they are written), otherwise unsigned 8-bit ones. A half lasts
 * about 60 ms at any rate, which is the latency. When the ring runs dry
 * the halves are filled with silence, and once nothing but silence is left
 * to play the DSP is stopped. Closing /dev/dsp lets what was written play
 * out; one process at a time may have it open.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "proc.h"
#include "syscall.h"
#include "vfs.h"
#include "x86.h"

#define DSP_MIXER_ADDR 0x4
#define DSP_MIXER_DATA 0x5
#define DSP_RESET      0x6
#define DSP_READ       0xA
#define DSP_WRITE      0xC              /* read: bit 7 set while the DSP is busy */
#define DSP_RSTATUS    0xE              /* read: bit 7 set when data waits; acks 8-bit IRQs */
#define DSP_ACK16      0xF              /* read: acks 16-bit IRQs */

#define CMD_RATE        0x41
#define CMD_DMA16_AUTO  0xB6            /* 16-bit output, auto-init, FIFO */
#define CMD_DMA8_AUTO   0xC6            /* 8-bit output, auto-init, FIFO */
#define CMD_PAUSE8      0xD0
#define CMD_SPEAKER_ON  0xD1
#define CMD_PAUSE16     0xD5
#define CMD_VERSION     0xE1
#define MODE_SIGNED     0x10
#define MODE_STEREO     0x20

#define MIX_IRQ    0x80
#define MIX_DMA    0x81
#define MIX_STATUS 0x82                 /* bit 0: 8-bit DMA IRQ, bit 1: 16-bit */

#define DMA_SIZE  32768                 /* both halves; aligned, so no page is crossed */
#define RING_SIZE 65536
#define HALF_MS   60
#define MIN_RATE  5000
#define MAX_RATE  44100

/* ISA DMA modes: single transfers, auto-init, memory to device. */
#define DMA_PLAY_AUTO 0x58

spinlock_t snd_lock = SPINLOCK_INIT("sound");    /* drivers.h */

static uint8_t dma_buf[DMA_SIZE] __attribute__((aligned(DMA_SIZE)));
static uint8_t ring[RING_SIZE];

static struct {
    int present;
    uint16_t base;
    uint8_t irq, dma8, dma16;           /* dma16 0xFF: none */
    uint16_t version;
    int dev16;                          /* the card gets 16-bit samples */
    int open_pid;                       /* who has /dev/dsp, or 0 */

    int rate, channels, fmt;            /* what write() is given */
    uint8_t carry[4];                   /* part of a frame left from the last write */
    uint32_t ncarry;

    int running;
    uint32_t half;                      /* bytes in each half of dma_buf */
    int next_half;                      /* the half the next interrupt finishes */
    uint32_t real[2];                   /* bytes of written samples in each half */
    int draining;                       /* play what is left even if it is short */
    uint32_t rhead, rtail;              /* the ring, free-running */
    uint32_t irqs;                      /* interrupts so far */
} sb;

/* ---- the DSP and the mixer ---- */

static int dsp_out(uint8_t v)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(sb.base + DSP_WRITE) & 0x80)) {
            outb(sb.base + DSP_WRITE, v);
            return 0;
        }
    }
    return -1;
}

static int dsp_in(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(sb.base + DSP_RSTATUS) & 0x80)
            return inb(sb.base + DSP_READ);
    return -1;
}

static int dsp_reset(void)
{
    outb(sb.base + DSP_RESET, 1);
    for (int i = 0; i < 5; i++)
        io_wait();                      /* at least 3 us */
    outb(sb.base + DSP_RESET, 0);
    for (int i = 0; i < 1000; i++) {    /* the DSP answers within 100 us */
        if ((inb(sb.base + DSP_RSTATUS) & 0x80) && inb(sb.base + DSP_READ) == 0xAA)
            return 0;
        io_wait();
    }
    return -1;
}

static void mix_write(uint8_t reg, uint8_t v)
{
    outb(sb.base + DSP_MIXER_ADDR, reg);
    outb(sb.base + DSP_MIXER_DATA, v);
}

static uint8_t mix_read(uint8_t reg)
{
    outb(sb.base + DSP_MIXER_ADDR, reg);
    return inb(sb.base + DSP_MIXER_DATA);
}

/* ---- formats ---- */

static uint32_t user_frame(void)
{
    return (uint32_t)sb.channels * (sb.fmt == AFMT_S16_LE ? 2 : 1);
}

static uint32_t dev_frame(void)
{
    return (uint32_t)sb.channels * (sb.dev16 ? 2 : 1);
}

/* About HALF_MS of sound, in whole frames on a 4-byte boundary. */
static void choose_half(void)
{
    uint32_t bytes = (uint32_t)sb.rate * dev_frame() * HALF_MS / 1000;
    if (bytes < 256)
        bytes = 256;
    if (bytes > DMA_SIZE / 2)
        bytes = DMA_SIZE / 2;
    sb.half = bytes & ~3u;
}

static uint32_t ring_used(void)
{
    return sb.rhead - sb.rtail;
}

/* Append one frame of user samples to the ring in the card's format. */
static void put_frame(const uint8_t *in)
{
    for (int c = 0; c < sb.channels; c++) {
        int16_t s;
        if (sb.fmt == AFMT_S16_LE) {
            s = (int16_t)(in[0] | in[1] << 8);
            in += 2;
        } else {
            s = (int16_t)((*in++ - 128) << 8);
        }
        if (sb.dev16) {
            ring[sb.rhead++ % RING_SIZE] = (uint8_t)s;
            ring[sb.rhead++ % RING_SIZE] = (uint8_t)(s >> 8);
        } else {
            ring[sb.rhead++ % RING_SIZE] = (uint8_t)((s >> 8) + 128);
        }
    }
}

/* Fill half h of the DMA buffer from the ring, then with silence. */
static void fill_half(int h)
{
    uint8_t *dst = dma_buf + h * sb.half;
    uint32_t n = MIN(ring_used(), sb.half);
    n -= n % dev_frame();
    for (uint32_t i = 0; i < n; i++)
        dst[i] = ring[sb.rtail++ % RING_SIZE];
    memset(dst + n, sb.dev16 ? 0 : 0x80, sb.half - n);
    sb.real[h] = n;
}

/* ---- playback ---- */

static void stop(void)
{
    if (!sb.running)
        return;
    dsp_out(sb.dev16 ? CMD_PAUSE16 : CMD_PAUSE8);
    isa_dma_stop(sb.dev16 ? sb.dma16 : sb.dma8);
    sb.running = 0;
    sb.real[0] = sb.real[1] = 0;
    wakeup(&sb);
    poll_wakeup();
}

static void start(void)
{
    if (sb.running || !ring_used())
        return;
    fill_half(0);
    fill_half(1);
    sb.next_half = 0;
    int ch = sb.dev16 ? sb.dma16 : sb.dma8;
    if (isa_dma_start(ch, (uint32_t)dma_buf, 2 * sb.half, DMA_PLAY_AUTO) < 0)
        return;
    uint32_t count = (sb.dev16 ? sb.half / 2 : sb.half) - 1;   /* samples per block, less one */
    uint8_t mode = (uint8_t)((sb.channels == 2 ? MODE_STEREO : 0) | (sb.dev16 ? MODE_SIGNED : 0));
    dsp_out(CMD_RATE);
    dsp_out((uint8_t)(sb.rate >> 8));
    dsp_out((uint8_t)sb.rate);
    dsp_out(sb.dev16 ? CMD_DMA16_AUTO : CMD_DMA8_AUTO);
    dsp_out(mode);
    dsp_out((uint8_t)count);
    dsp_out((uint8_t)(count >> 8));
    sb.running = 1;
}

static void sb_irq_locked(struct trapframe *tf)
{
    (void)tf;
    uint8_t st = mix_read(MIX_STATUS);
    if (st & 1)
        (void)inb(sb.base + DSP_RSTATUS);
    if (st & 2)
        (void)inb(sb.base + DSP_ACK16);
    if (!(st & 3) || !sb.running)
        return;
    sb.irqs++;
    int h = sb.next_half;               /* this half has played: refill it */
    fill_half(h);
    sb.next_half = h ^ 1;
    if (!sb.real[0] && !sb.real[1] && !ring_used())
        stop();                         /* only silence left in the buffer */
    wakeup(&sb);
    poll_wakeup();
}

/* Sleep until the interrupt handler moves on; -EIO if the card never interrupts. */
static int wait_for_irq(void)
{
    uint32_t seen = sb.irqs;
    int was_running = sb.running;
    if (!was_running)
        return -EIO;                    /* nothing will wake us: the DMA did not start */
    sleep_on_lock_timeout(&sb, &snd_lock, ticks + HZ);
    if (signal_pending(current))
        return -EINTR;
    if (was_running && sb.running && sb.irqs == seen) {
        klog("sb16: no interrupt on IRQ %d, stopping playback\n", sb.irq);
        stop();
        sb.rtail = sb.rhead;
        return -EIO;
    }
    return 0;
}

/* Wait until everything written has played. */
static int drain(void)
{
    sb.draining = 1;
    while (sb.running || ring_used()) {
        start();
        if (!sb.running)
            break;                      /* the DMA would not start */
        int r = wait_for_irq();
        if (r < 0) {
            sb.draining = 0;
            return r;
        }
    }
    sb.draining = 0;
    return 0;
}

/* ---- /dev/dsp ---- */

static void set_defaults(void)
{
    sb.rate = 8000;
    sb.channels = 1;
    sb.fmt = AFMT_U8;
    sb.ncarry = 0;
    choose_half();
}

static int dsp_open_locked(struct vnode *vn, int flags)
{
    (void)vn;
    if ((flags & O_ACCMODE) == O_RDONLY)
        return -EINVAL;                 /* no recording */
    if (sb.open_pid && sb.open_pid != current->leader->pid) {
        struct proc *p = proc_find(sb.open_pid);
        if (p && p->state != P_ZOMBIE)
            return -EBUSY;
    }
    sb.open_pid = current->leader->pid;
    if (!sb.running && !ring_used())
        set_defaults();
    return 0;
}

static void dsp_close_locked(struct vnode *vn, int flags)
{
    (void)vn; (void)flags;
    sb.open_pid = 0;
    sb.ncarry = 0;
    start();                            /* the rest plays out by itself */
}

static int dsp_dev_write_locked(struct vnode *vn, const void *buf, uint64_t off, uint32_t n)
{
    (void)vn; (void)off;
    const uint8_t *in = buf;
    uint32_t uf = user_frame(), df = dev_frame(), used = 0;

    /* Complete a frame begun by the last write. */
    /* A full frame stays in carry until there is room for it: a wait that
     * fails (a signal) keeps it for the next write, which must not add to it. */
    while (sb.ncarry) {
        if (sb.ncarry < uf) {
            if (used == n)
                break;
            sb.carry[sb.ncarry++] = in[used++];
            continue;
        }
        while (RING_SIZE - ring_used() < df) {
            start();
            int r = wait_for_irq();
            if (r < 0)
                return used ? (int)used : r;
        }
        put_frame(sb.carry);
        sb.ncarry = 0;
    }
    while (n - used >= uf) {
        uint32_t room = (RING_SIZE - ring_used()) / df;
        if (!room) {
            start();
            int r = wait_for_irq();
            if (r < 0)
                return used ? (int)used : r;
            continue;
        }
        uint32_t frames = MIN(room, (n - used) / uf);
        for (uint32_t i = 0; i < frames; i++, used += uf)
            put_frame(in + used);
        if (!sb.running && ring_used() >= RING_SIZE / 2)
            start();      /* a deep head start rides out a busy desktop */
    }
    while (used < n)                    /* keep a part frame for next time */
        sb.carry[sb.ncarry++] = in[used++];
    return (int)n;
}

static int dsp_poll_locked(struct vnode *vn)
{
    (void)vn;
    return RING_SIZE - ring_used() >= sb.half ? POLLOUT : 0;
}

/* Change the format after what was written in the old one has played. */
static int set_format(int *field, int value)
{
    if (*field == value)
        return 0;
    int r = drain();
    if (r < 0)
        return r;
    *field = value;
    sb.ncarry = 0;
    choose_half();
    return 0;
}

static int get_int(uint32_t arg, int *v)
{
    return copy_from_user(v, (const void *)arg, sizeof(*v));
}

static int put_int(uint32_t arg, int v)
{
    return copy_to_user((void *)arg, &v, sizeof(v));
}

static int dsp_ioctl_locked(struct vnode *vn, uint32_t req, uint32_t arg)
{
    (void)vn;
    int v, r;
    switch (req) {
    case SNDCTL_DSP_RESET:
        stop();
        sb.rtail = sb.rhead;
        sb.ncarry = 0;
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
        if ((r = set_format(&sb.rate, v)) < 0)
            return r;
        return put_int(arg, sb.rate);
    case SNDCTL_DSP_CHANNELS:
        if ((r = get_int(arg, &v)) < 0)
            return r;
        v = v >= 2 ? 2 : 1;
        if ((r = set_format(&sb.channels, v)) < 0)
            return r;
        return put_int(arg, sb.channels);
    case SNDCTL_DSP_SETFMT:
        if ((r = get_int(arg, &v)) < 0)
            return r;
        v = v == AFMT_S16_LE ? AFMT_S16_LE : AFMT_U8;
        if ((r = set_format(&sb.fmt, v)) < 0)
            return r;
        return put_int(arg, sb.fmt);
    case SNDCTL_DSP_GETOSPACE: {
        struct audio_buf_info bi;
        uint32_t frames = (RING_SIZE - ring_used()) / dev_frame();
        uint32_t frag = sb.half / dev_frame() * user_frame();
        bi.bytes = (int32_t)(frames * user_frame());
        bi.fragsize = (int32_t)frag;
        bi.fragstotal = (int32_t)(RING_SIZE / sb.half);
        bi.fragments = frag ? bi.bytes / (int32_t)frag : 0;
        return copy_to_user((void *)arg, &bi, sizeof(bi));
    }
    case SNDCTL_DSP_GETODELAY: {
        uint32_t queued = ring_used() + (sb.running ? sb.real[0] + sb.real[1] : 0);
        return put_int(arg, (int)(queued / dev_frame() * user_frame()));
    }
    case SOUND_INFO: {
        struct sound_info si;
        sound_get_info(&si);
        return copy_to_user((void *)arg, &si, sizeof(si));
    }
    }
    return -EINVAL;
}

/* ---- /dev/mixer ---- */

/* Each channel's left register (right is the next one), and its width. */
static const struct {
    uint8_t reg;
    uint8_t stereo, bits;
} mixer_regs[MIXER_CHANNELS] = {
    [MIXER_MASTER] = { 0x30, 1, 5 },
    [MIXER_PCM] = { 0x32, 1, 5 },
    [MIXER_SYNTH] = { 0x34, 1, 5 },
    [MIXER_CD] = { 0x36, 1, 5 },
    [MIXER_LINE] = { 0x38, 1, 5 },
    [MIXER_MIC] = { 0x3A, 0, 5 },
    [MIXER_SPEAKER] = { 0x3B, 0, 2 },
};

static uint32_t level_from_reg(uint8_t v, int bits)
{
    uint32_t max = (1u << bits) - 1;
    return ((uint32_t)(v >> (8 - bits)) * 100 + max / 2) / max;
}

static uint8_t reg_from_level(uint32_t level, int bits)
{
    uint32_t max = (1u << bits) - 1;
    if (level > 100)
        level = 100;
    return (uint8_t)(((level * max + 50) / 100) << (8 - bits));
}

static int mixer_ioctl_locked(struct vnode *vn, uint32_t req, uint32_t arg)
{
    (void)vn;
    if (req == SOUND_INFO) {
        struct sound_info si;
        sound_get_info(&si);
        return copy_to_user((void *)arg, &si, sizeof(si));
    }
    if (req != MIXER_READ && req != MIXER_WRITE)
        return -EINVAL;
    struct mixer_level ml;
    if (copy_from_user(&ml, (const void *)arg, sizeof(ml)) < 0)
        return -EFAULT;
    if (ml.channel >= MIXER_CHANNELS)
        return -EINVAL;
    uint8_t reg = mixer_regs[ml.channel].reg;
    int bits = mixer_regs[ml.channel].bits, stereo = mixer_regs[ml.channel].stereo;
    if (req == MIXER_READ) {
        ml.left = level_from_reg(mix_read(reg), bits);
        ml.right = stereo ? level_from_reg(mix_read((uint8_t)(reg + 1)), bits) : ml.left;
        return copy_to_user((void *)arg, &ml, sizeof(ml));
    }
    mix_write(reg, reg_from_level(stereo ? ml.left : (ml.left + ml.right) / 2, bits));
    if (stereo)
        mix_write((uint8_t)(reg + 1), reg_from_level(ml.right, bits));
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

static int dsp_dev_write(struct vnode *vn, const void *buf, uint64_t off, uint32_t n)
{
    spin_lock(&snd_lock);
    int r = dsp_dev_write_locked(vn, buf, off, n);
    spin_unlock(&snd_lock);
    return r;
}

static void sb_irq(struct trapframe *tf)
{
    spin_lock(&snd_lock);
    sb_irq_locked(tf);
    spin_unlock(&snd_lock);
}

/* ---- setup ---- */

void sound_get_info(struct sound_info *si)
{
    memset(si, 0, sizeof(*si));
    si->flags = SOUNDF_SPEAKER;
    si->dma16 = 0xFF;
    if (sb.present) {
        strlcpy(si->card, "Sound Blaster 16", sizeof(si->card));
        si->dsp_version = sb.version;
        si->base = sb.base;
        si->irq = sb.irq;
        si->dma8 = sb.dma8;
        si->dma16 = sb.dma16;
        si->flags |= SOUNDF_DSP;
    } else if (!usbaudio_get_info(si) && !hda_get_info(si)) {
        strlcpy(si->card, "PC speaker", sizeof(si->card));
    }
    int opl = opl_present();
    if (opl) {
        si->opl_base = 0x388;
        si->flags |= SOUNDF_OPL2 | (opl == 3 ? SOUNDF_OPL3 : 0);
    }
}

/* The IRQ and DMA channels the card's mixer reports; 0 if they make no sense. */
static int read_resources(void)
{
    uint8_t irqs = mix_read(MIX_IRQ), dmas = mix_read(MIX_DMA);
    static const uint8_t irq_of[4] = { 9, 5, 7, 10 };
    sb.irq = 0;
    for (int i = 0; i < 4; i++)
        if (irqs & (1 << i))
            sb.irq = irq_of[i];
    sb.dma8 = 0xFF;
    sb.dma16 = 0xFF;
    for (int ch = 0; ch < 4; ch++)
        if (ch != 2 && (dmas & (1 << ch)))
            sb.dma8 = (uint8_t)ch;
    for (int ch = 5; ch < 8; ch++)
        if (dmas & (1 << ch))
            sb.dma16 = (uint8_t)ch;
    return sb.irq && sb.dma8 != 0xFF && irqs != 0xFF;
}

void sb16_init(struct vnode *devdir)
{
    if (cmdline_has("sb16=off"))
        return;
    if ((uint32_t)dma_buf + DMA_SIZE > 0x1000000u)
        return;                         /* ISA DMA cannot reach it */
    static const uint16_t bases[] = { 0x220, 0x240, 0x260, 0x280 };
    for (uint32_t i = 0; i < ARRAY_SIZE(bases) && !sb.present; i++) {
        sb.base = bases[i];
        if (dsp_reset() < 0 || dsp_out(CMD_VERSION) < 0)
            continue;
        int major = dsp_in(), minor = dsp_in();
        if (major < 0 || minor < 0)
            continue;
        sb.version = (uint16_t)(major << 8 | minor);
        if (major < 4) {
            kprintf("sb16: a Sound Blaster with DSP %d.%02d at %xh is too old, ignoring it\n",
                    major, minor, sb.base);
            return;
        }
        sb.present = 1;
    }
    if (!sb.present)
        return;
    if (!read_resources()) {
        /* Not configured: the usual settings, A220 I5 D1 H5. */
        mix_write(MIX_IRQ, 0x02);
        mix_write(MIX_DMA, 0x22);
        if (!read_resources()) {
            sb.irq = 5;
            sb.dma8 = 1;
            sb.dma16 = 5;
        }
    }
    sb.dev16 = sb.dma16 != 0xFF;
    set_defaults();
    dsp_out(CMD_SPEAKER_ON);
    /* IRQ handlers are not shared: without its IRQ the card can still mix. */
    int irq_ok = !irq_registered(sb.irq);
    if (irq_ok)
        irq_register(sb.irq, sb_irq);
    else
        kprintf("sb16: IRQ %d is taken by another device, so there is no /dev/dsp\n", sb.irq);

    struct vnode *dsp = irq_ok ? vfs_mknod(devdir, "dsp", MKDEV(SOUND_MAJOR, SOUND_MINOR_DSP), NULL,
                                           dsp_dev_write) : NULL;
    if (dsp) {
        dsp->dev_open = dsp_open;
        dsp->dev_close = dsp_close;
        dsp->dev_ioctl = dsp_ioctl;
        dsp->dev_poll = dsp_poll;
    }
    struct vnode *mixer = vfs_mknod(devdir, "mixer", MKDEV(SOUND_MAJOR, SOUND_MINOR_MIXER), NULL, NULL);
    if (mixer)
        mixer->dev_ioctl = mixer_ioctl;
    if (sb.dev16)
        boot_ok("Sound Blaster 16 (DSP %d.%02d) at %xh, IRQ %d, DMA %d and %d", sb.version >> 8,
                sb.version & 0xFF, sb.base, sb.irq, sb.dma8, sb.dma16);
    else
        boot_ok("Sound Blaster 16 (DSP %d.%02d) at %xh, IRQ %d, DMA %d", sb.version >> 8,
                sb.version & 0xFF, sb.base, sb.irq, sb.dma8);
}

/* All the sound devices, from kmain once /dev exists. */
void sound_init(void)
{
    struct vnode *dev;
    if (vfs_resolve("/dev", NULL, &dev) < 0)
        return;
    spkr_init(dev);
    sb16_init(dev);
    hda_init(dev);
    opl_init(dev);
}
