/*
 * USB audio playback (USB Audio Class 1.0: headsets, DACs, speakers, and
 * QEMU's usb-audio), as /dev/dsp and /dev/mixer like the Sound Blaster
 * and HD Audio drivers, with the same calls (SNDCTL_DSP_*, MIXER_*).
 *
 * A device has an AudioControl interface, whose descriptors describe its
 * terminals and feature units (the volume and mute controls), and
 * AudioStreaming interfaces, each alternate setting of which offers one
 * format (channels, sample size, rates) on an isochronous endpoint. The
 * first AudioControl interface is taken, the best playback format among
 * the streaming interfaces chosen (16-bit stereo at 48 kHz if it has it),
 * its alternate setting selected, and the endpoint told the rate.
 *
 * Playing is a stream of isochronous packets, one a service interval
 * (every millisecond at full speed), each holding the frames that
 * interval is due at the device's rate (a fraction carries over, so
 * 44.1 kHz works). A few transfers of several packets each are kept in
 * flight; each one is refilled from the ring of frames write() leaves,
 * silence when it is empty, and queued again when it completes, from the
 * controller's interrupt. A device that runs its own clock sends how
 * many frames it wants on a feedback endpoint, which is followed.
 *
 * Locking: write() and the ioctls hold snd_lock, as the other sound
 * drivers do; the packet refills run under usb_lock from interrupts, and
 * never take snd_lock. The ring between them is one writer, one reader.
 * Nothing waits for the controller while holding snd_lock (the controller
 * commands behind cancelling a transfer may sleep), so stopping the
 * stream is done before it is taken. Control transfers (the volume) are
 * sent from process context without it.
 *
 * While a device is plugged in it is /dev/dsp: the built-in card's node
 * (if there is one, and it is not open) is pointed at this driver and
 * given back when the device goes; the device also has /dev/dsp1 of its
 * own. The option usbaudio=secondary leaves /dev/dsp alone.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "syscall.h"
#include "vfs.h"
#include "usb.h"

#define USB_CLASS_AUDIO   1
#define AUDIO_CONTROL     1
#define AUDIO_STREAMING   2

#define CS_INTERFACE      0x24
#define CS_ENDPOINT       0x25
#define AC_HEADER         0x01
#define AC_FEATURE_UNIT   0x06
#define AS_GENERAL        0x01
#define AS_FORMAT_TYPE    0x02
#define EP_GENERAL        0x01

#define FORMAT_PCM        1
#define FORMAT_PCM8       2             /* unsigned 8-bit */

#define REQ_SET_CUR       0x01
#define REQ_GET_CUR       0x81
#define REQ_GET_MIN       0x82
#define REQ_GET_MAX       0x83
#define REQ_GET_RES       0x84
#define FU_MUTE           0x01
#define FU_VOLUME         0x02
#define EP_SAMPLING_FREQ  0x01
#define EP_ATTR_FREQ_CTL  0x01          /* the endpoint takes a sampling frequency */

#define SYNC_MASK         0x0C          /* an endpoint's bmAttributes: synchronisation */
#define SYNC_ASYNC        0x04          /* the device's own clock: it sends feedback */

#define NX            4                 /* transfers in flight */
#define XFER_BYTES    4096              /* each one's buffer: one page, so no 64 KiB crossing */
#define RING_SIZE     65536u            /* bytes of device frames waiting to go */
#define RING_PAGES    (RING_SIZE / PAGE_SIZE)
#define MAX_PER_IN    16                /* device frames one input frame can become (4 kHz in) */
#define MIN_RATE      4000
#define MAX_RATE      48000
#define MAX_DEV_RATE  192000
#define STALL_TICKS   (2 * HZ)          /* no packet completed for this long: the stream is dead */

/* One playback format a device offers. */
struct format {
    uint8_t iface, alt;
    uint8_t channels, subframe, bits;
    int unsigned8;
    uint32_t rate;
    struct usb_ep_desc ep;              /* the data endpoint (its first 7 bytes) */
    uint8_t ep_attrs;                   /* bmAttributes of its class descriptor */
    int has_fb;
    struct usb_ep_desc fb;              /* the feedback endpoint, if has_fb */
};

static struct {
    struct usb_dev *dev;                /* NULL: no device */
    volatile int gone;                  /* it went away (or was never there) */
    uint8_t ac_iface;
    struct format f;
    uint32_t df;                        /* a device frame's bytes */
    struct usb_ep *ep, *fb_ep;
    int fb_bytes;                       /* 3: 10.14 per frame; 4: 16.16 per microframe */
    uint32_t pps;                       /* packets a second */
    char name[48];

    /* the feature unit: which channels (bit 0 the master) have the controls */
    uint8_t fu, fu_mute, fu_volume;
    int16_t vol_min, vol_max, vol_res;

    /* streaming, under usb_lock */
    struct usb_xfer x[NX];
    uint8_t *buf[NX];
    uint16_t plen[NX][USB_ISO_MAX_PACKETS];
    int npk;                            /* packets a transfer */
    volatile int streaming;
    int inflight;
    uint32_t ff, acc;                   /* frames a packet, 16.16, and the fraction carried */
    uint32_t fb_ff;                     /* what the device asks for (0: nothing yet) */
    struct usb_xfer fx;
    uint8_t *fbuf;
    uint16_t fblen[1];
    int fb_running;
    uint32_t progress, progress_tick, underruns, errors;
    int need_reset;                     /* the stream stalled: its transfers must be cancelled first */

    /* write()'s side, under snd_lock */
    int open_pid;
    int rate, channels, fmt_user;       /* what write() is given */
    uint8_t carry[4];
    uint32_t ncarry;
    int32_t prev_l, prev_r;             /* the resampler */
    uint32_t pos, step;
    volatile uint32_t rhead, rtail;     /* the ring: written at rhead, read at rtail */
    uint8_t *ring;
    int master, pcm;                    /* volumes, 0-100 */

    /* the device nodes: ours, and the built-in card's while we have them */
    struct vnode *dsp, *mixer;
    int primary;                        /* ours are /dev/dsp and /dev/mixer */
    struct vnode *taken_dsp, *taken_mixer;
    dev_open_t saved_open;
    dev_close_t saved_close;
    dev_write_t saved_write;
    dev_ioctl_t saved_ioctl, saved_mixer_ioctl;
    dev_poll_t saved_poll;
} ua = { .gone = 1 };

/* ---- the ring and the resampler (write()'s side) ---- */

static uint32_t ring_used(void)
{
    return ua.rhead - ua.rtail;
}

static uint32_t user_frame(void)
{
    return (uint32_t)ua.channels * (ua.fmt_user == AFMT_S16_LE ? 2 : 1);
}

static int32_t volume(void)
{
    int v = ua.pcm * (ua.fu_volume ? 100 : ua.master) / 100;
    return v * 256 / 100;
}

/* A device frame into the ring, from 16-bit left and right (other channels silent). */
static void push_dev_frame(int32_t l, int32_t r)
{
    uint32_t h = ua.rhead;
    for (int c = 0; c < ua.f.channels; c++) {
        int32_t s = c == 0 ? l : c == 1 ? r : 0;
        uint32_t v;
        switch (ua.f.subframe) {
        case 1: v = (uint32_t)((s >> 8) + (ua.f.unsigned8 ? 128 : 0)) & 0xFF; break;
        case 2: v = (uint32_t)s & 0xFFFF; break;
        case 3: v = (uint32_t)s << 8; break;
        default: v = (uint32_t)s << 16; break;
        }
        for (int b = 0; b < ua.f.subframe; b++)
            ua.ring[h++ % RING_SIZE] = (uint8_t)(v >> (8 * b));
    }
    __asm__ volatile("" ::: "memory");
    ua.rhead = h;                       /* whole frames only: the reader never sees part of one */
}

/* One input frame as device frames, interpolating between it and the one before. */
static void put_frame(const uint8_t *in)
{
    int32_t s[2] = { 0, 0 };
    for (int c = 0; c < ua.channels; c++) {
        if (ua.fmt_user == AFMT_S16_LE) {
            s[c] = (int16_t)(in[0] | in[1] << 8);
            in += 2;
        } else {
            s[c] = (*in++ - 128) << 8;
        }
    }
    if (ua.channels == 1)
        s[1] = s[0];
    int32_t vol = volume();
    int32_t l = s[0] * vol / 256, r = s[1] * vol / 256;
    while (ua.pos < 0x10000) {
        int32_t f = (int32_t)(ua.pos >> 2);
        push_dev_frame(ua.prev_l + (((l - ua.prev_l) * f) >> 14),
                       ua.prev_r + (((r - ua.prev_r) * f) >> 14));
        ua.pos += ua.step;
    }
    ua.pos -= 0x10000;
    ua.prev_l = l;
    ua.prev_r = r;
}

static void reset_resampler(void)
{
    ua.pos = 0;
    ua.prev_l = ua.prev_r = 0;
    ua.step = (uint32_t)(((uint32_t)ua.rate << 16) / ua.f.rate);
}

static void set_defaults(void)
{
    ua.rate = 8000;
    ua.channels = 1;
    ua.fmt_user = AFMT_U8;
    ua.ncarry = 0;
    reset_resampler();
}

/* ---- the stream (under usb_lock) ---- */

/* How many frames the next packet carries: the device's rate, or what it asks for. */
static uint32_t next_frames(void)
{
    ua.acc += ua.fb_ff ? ua.fb_ff : ua.ff;
    uint32_t n = ua.acc >> 16;
    ua.acc &= 0xFFFF;
    return n;
}

static uint32_t xfer_bytes(void)
{
    return (ua.ff >> 16) * ua.df * (uint32_t)ua.npk;
}

/* Fill a transfer's packets from the ring, silence where it runs dry. */
static void fill_xfer(struct usb_xfer *x)
{
    int k = (int)(x - ua.x);
    uint8_t *p = ua.buf[k];
    uint32_t off = 0, mps = ua.ep->maxpacket;
    for (int i = 0; i < ua.npk; i++) {
        uint32_t n = next_frames() * ua.df;
        if (n > mps)
            n = mps - mps % ua.df;
        if (off + n > XFER_BYTES)
            n = 0;
        uint32_t avail = ring_used();
        avail -= avail % ua.df;
        uint32_t take = MIN(n, avail);
        uint32_t t = ua.rtail;
        for (uint32_t b = 0; b < take; b++)
            p[off + b] = ua.ring[t++ % RING_SIZE];
        __asm__ volatile("" ::: "memory");
        ua.rtail = t;
        if (take < n) {
            memset(p + off + take, ua.f.unsigned8 ? 0x80 : 0, n - take);
            if (ua.open_pid && ++ua.underruns <= 5)
                klog("usb: audio underrun %u (%u of %u bytes ready)\n", ua.underruns, take, n);
        }
        ua.plen[k][i] = (uint16_t)n;
        off += n;
    }
    x->len = off;
    x->npackets = (uint16_t)ua.npk;
    x->iso_len = ua.plen[k];
}

static void xfer_done(struct usb_xfer *x)
{
    if (ua.inflight > 0)
        ua.inflight--;
    if (x->status == -ENODEV || ua.gone)
        return;
    if (x->status < 0 && ++ua.errors <= 3)
        klog("usb: audio transfer failed (%d)\n", x->status);
    ua.progress++;
    ua.progress_tick = ticks;
    if (ua.streaming && (ring_used() || ua.open_pid)) {
        fill_xfer(x);
        if (usb_submit(x) == 0)
            ua.inflight++;
    }
    if (!ua.inflight)
        ua.streaming = 0;               /* only silence was left, and nobody writes */
    wakeup(&ua);
    poll_wakeup();
}

/* The device's own clock: frames per packet from the feedback endpoint. */
static void fb_done(struct usb_xfer *x)
{
    ua.fb_running = 0;
    if (x->status == -ENODEV || ua.gone || !ua.streaming)
        return;
    if (x->status == 0 && ua.fblen[0] >= 3) {
        const uint8_t *b = ua.fbuf;
        uint32_t v;
        if (ua.fb_bytes == 3)
            v = ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16) << 2;   /* 10.14 */
        else
            v = ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24) *
                (8000 / ua.pps);                                                      /* 16.16 */
        /* Only a rate near the nominal one: anything else is noise. */
        if (v > ua.ff - ua.ff / 8 && v < ua.ff + ua.ff / 8)
            ua.fb_ff = v;
    }
    ua.fblen[0] = (uint16_t)ua.fb_bytes;
    x->npackets = 1;
    x->iso_len = ua.fblen;
    x->len = ua.fb_ep->maxpacket;
    if (usb_submit(x) == 0)
        ua.fb_running = 1;
}

/* Start the stream (snd_lock held: nothing here waits). */
static void start(void)
{
    if (ua.streaming || !ring_used() || ua.gone || ua.need_reset)
        return;
    int t = usb_enter();
    if (!ua.gone && !ua.streaming) {
        ua.streaming = 1;
        ua.acc = 0;
        ua.inflight = 0;
        ua.progress_tick = ticks;
        for (int k = 0; k < NX; k++) {
            fill_xfer(&ua.x[k]);
            if (usb_submit(&ua.x[k]) < 0)
                break;
            ua.inflight++;
        }
        if (!ua.inflight)
            ua.streaming = 0;
        if (ua.fb_ep && !ua.fb_running) {
            ua.fblen[0] = (uint16_t)ua.fb_bytes;
            ua.fx.npackets = 1;
            ua.fx.iso_len = ua.fblen;
            ua.fx.len = ua.fb_ep->maxpacket;
            if (usb_submit(&ua.fx) == 0)
                ua.fb_running = 1;
        }
    }
    usb_leave(t);
}

/* Stop it, dropping what is in flight. Not under snd_lock: the controller
 * may have to be told, which can wait. */
static void stop(void)
{
    int t = usb_enter();
    ua.streaming = 0;
    if (!ua.gone) {
        if (ua.inflight)
            usb_cancel(&ua.x[0]);       /* cancels every transfer queued on the endpoint */
        if (ua.fb_running)
            usb_cancel(&ua.fx);
    }
    for (int k = 0; k < NX; k++)
        if (ua.x[k].status == USB_PENDING)
            ua.x[k].status = -ETIMEDOUT;
    ua.inflight = 0;
    ua.fb_running = 0;
    ua.fb_ff = 0;
    ua.need_reset = 0;
    usb_leave(t);
    wakeup(&ua);
    poll_wakeup();
}

/* From usb_tick: a stream whose packets stopped completing is given up.
 * Its transfers are still the controller's, so nothing starts again until
 * stop() has taken them back (at the next open, or SNDCTL_DSP_RESET). */
void usbaudio_tick(void)
{
    if (!ua.streaming || ua.gone || (int32_t)(ticks - ua.progress_tick) < STALL_TICKS)
        return;
    klog("usb: audio stream stalled, stopping playback\n");
    ua.streaming = 0;
    ua.need_reset = 1;
    ua.rtail = ua.rhead;
    wakeup(&ua);
    poll_wakeup();
}

/* ---- /dev/dsp (under snd_lock) ---- */

/* Sleep until a transfer completes; -EIO if the stream has stopped. */
static int wait_progress(void)
{
    uint32_t seen = ua.progress;
    if (!ua.streaming || ua.gone)
        return -EIO;
    sleep_on_lock_timeout(&ua, &snd_lock, ticks + HZ);
    if (signal_pending(current))
        return -EINTR;
    if (ua.gone)
        return -ENODEV;
    if (ua.streaming && ua.progress == seen)
        return -EIO;
    return 0;
}

static int drain(void)
{
    while (!ua.gone && (ua.streaming || ring_used())) {
        start();
        if (!ua.streaming)
            break;
        int r = wait_progress();
        if (r < 0)
            return r;
    }
    return ua.gone ? -ENODEV : 0;
}

static int dsp_open_locked(struct vnode *vn, int flags)
{
    (void)vn;
    if (ua.gone)
        return -ENODEV;
    if ((flags & O_ACCMODE) == O_RDONLY)
        return -EINVAL;                 /* no recording */
    if (ua.open_pid && ua.open_pid != current->leader->pid) {
        struct proc *p = proc_find(ua.open_pid);
        if (p && p->state != P_ZOMBIE)
            return -EBUSY;
    }
    ua.open_pid = current->leader->pid;
    if (!ua.streaming && !ring_used())
        set_defaults();
    return 0;
}

static void dsp_close_locked(struct vnode *vn, int flags)
{
    (void)vn; (void)flags;
    ua.open_pid = 0;
    ua.ncarry = 0;
    start();                            /* the rest plays out by itself */
}

static int make_room(void)
{
    while (RING_SIZE - ring_used() < MAX_PER_IN * ua.df) {
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
    if (ua.gone)
        return -ENODEV;
    const uint8_t *in = buf;
    uint32_t uf = user_frame(), used = 0;
    while (ua.ncarry && used < n) {
        ua.carry[ua.ncarry++] = in[used++];
        if (ua.ncarry == uf) {
            int r = make_room();
            if (r < 0)
                return r;
            put_frame(ua.carry);
            ua.ncarry = 0;
        }
    }
    while (n - used >= uf) {
        if (RING_SIZE - ring_used() < MAX_PER_IN * ua.df) {
            int r = make_room();
            if (r < 0)
                return used ? (int)used : r;
        }
        while (n - used >= uf && RING_SIZE - ring_used() >= MAX_PER_IN * ua.df) {
            put_frame(in + used);
            used += uf;
        }
        if (!ua.streaming && ring_used() >= NX * xfer_bytes())
            start();
    }
    while (used < n)
        ua.carry[ua.ncarry++] = in[used++];
    return (int)n;
}

static int dsp_poll_locked(struct vnode *vn)
{
    (void)vn;
    if (ua.gone)
        return POLLOUT | POLLERR;
    return RING_SIZE - ring_used() >= xfer_bytes() + MAX_PER_IN * ua.df ? POLLOUT : 0;
}

static int set_format(int *field, int value)
{
    if (*field == value)
        return 0;
    int r = drain();
    if (r < 0)
        return r;
    *field = value;
    ua.ncarry = 0;
    reset_resampler();
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

/* Device bytes as the bytes of write()'s format that make them. */
static uint32_t user_bytes(uint32_t dev_bytes)
{
    uint32_t frames = dev_bytes / ua.df;
    return (uint32_t)((uint64_t)frames * (uint32_t)ua.rate / ua.f.rate) * user_frame();
}

static int dsp_ioctl_locked(struct vnode *vn, uint32_t req, uint32_t arg)
{
    (void)vn;
    int v, r;
    if (ua.gone && req != SOUND_INFO)
        return -ENODEV;
    switch (req) {
    case SNDCTL_DSP_RESET:              /* the stream was stopped before the lock was taken */
        ua.rtail = ua.rhead;
        ua.ncarry = 0;
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
        if ((r = set_format(&ua.rate, v)) < 0)
            return r;
        return put_int(arg, ua.rate);
    case SNDCTL_DSP_CHANNELS:
        if ((r = get_int(arg, &v)) < 0)
            return r;
        if ((r = set_format(&ua.channels, v >= 2 ? 2 : 1)) < 0)
            return r;
        return put_int(arg, ua.channels);
    case SNDCTL_DSP_SETFMT:
        if ((r = get_int(arg, &v)) < 0)
            return r;
        if ((r = set_format(&ua.fmt_user, v == AFMT_S16_LE ? AFMT_S16_LE : AFMT_U8)) < 0)
            return r;
        return put_int(arg, ua.fmt_user);
    case SNDCTL_DSP_GETOSPACE: {
        struct audio_buf_info bi;
        uint32_t room = RING_SIZE - ring_used();
        room = room > MAX_PER_IN * ua.df ? room - MAX_PER_IN * ua.df : 0;
        uint32_t frag = user_bytes(xfer_bytes());
        bi.bytes = (int32_t)user_bytes(room);
        bi.fragsize = (int32_t)frag;
        bi.fragstotal = (int32_t)(RING_SIZE / xfer_bytes());
        bi.fragments = frag ? bi.bytes / (int32_t)frag : 0;
        return copy_to_user((void *)arg, &bi, sizeof(bi));
    }
    case SNDCTL_DSP_GETODELAY: {
        uint32_t queued = ring_used() + (ua.streaming ? (uint32_t)ua.inflight * xfer_bytes() : 0);
        return put_int(arg, (int)user_bytes(queued));
    }
    case SOUND_INFO: {
        struct sound_info si;
        sound_get_info(&si);
        return copy_to_user((void *)arg, &si, sizeof(si));
    }
    }
    return -EINVAL;
}

/* ---- the volume controls, by control transfer (process context, no snd_lock) ---- */

static int fu_request(uint8_t req, uint8_t control, uint8_t channel, void *data, uint16_t len)
{
    int t = usb_enter();
    int r = ua.gone ? -ENODEV :
            usb_control(ua.dev, (req & 0x80) | USB_RT_CLASS | USB_RT_INTERFACE, req,
                        (uint16_t)(control << 8 | channel), (uint16_t)(ua.fu << 8 | ua.ac_iface),
                        data, len);
    usb_leave(t);
    return r;
}

/* The master volume (0-100) onto the feature unit: every channel that has the control. */
static void apply_master(int level)
{
    if (!ua.fu)
        return;
    int16_t vol = (int16_t)(ua.vol_min + (int32_t)(ua.vol_max - ua.vol_min) * level / 100);
    if (ua.vol_res > 0)
        vol = (int16_t)(ua.vol_min + (vol - ua.vol_min) / ua.vol_res * ua.vol_res);
    uint8_t mute = level == 0;
    for (int ch = 0; ch < 8; ch++) {
        if (ua.fu_volume & (1 << ch)) {
            uint8_t v[2] = { (uint8_t)vol, (uint8_t)(vol >> 8) };
            fu_request(REQ_SET_CUR, FU_VOLUME, (uint8_t)ch, v, 2);
        }
        if (ua.fu_mute & (1 << ch))
            fu_request(REQ_SET_CUR, FU_MUTE, (uint8_t)ch, &mute, 1);
    }
}

static int mixer_ioctl_locked(struct vnode *vn, uint32_t req, uint32_t arg, int *hw_level)
{
    (void)vn;
    *hw_level = -1;
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
    int *level = ml.channel == MIXER_MASTER ? &ua.master : ml.channel == MIXER_PCM ? &ua.pcm : NULL;
    if (!level)
        return -EINVAL;
    if (req == MIXER_READ) {
        ml.left = ml.right = (uint32_t)*level;
        return copy_to_user((void *)arg, &ml, sizeof(ml));
    }
    uint32_t v = (ml.left + ml.right) / 2;
    *level = (int)MIN(v, 100u);
    if (ml.channel == MIXER_MASTER && ua.fu_volume)
        *hw_level = *level;
    return 0;
}

/* ---- the device calls, under snd_lock ---- */

static int dsp_open(struct vnode *vn, int flags)
{
    if (ua.need_reset && !ua.gone)
        stop();
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
    if (req == SNDCTL_DSP_RESET)
        stop();
    spin_lock(&snd_lock);
    int r = dsp_ioctl_locked(vn, req, arg);
    spin_unlock(&snd_lock);
    return r;
}

static int mixer_ioctl(struct vnode *vn, uint32_t req, uint32_t arg)
{
    int hw_level;
    spin_lock(&snd_lock);
    int r = mixer_ioctl_locked(vn, req, arg, &hw_level);
    spin_unlock(&snd_lock);
    if (r == 0 && hw_level >= 0)
        apply_master(hw_level);
    return r;
}

static int dsp_write(struct vnode *vn, const void *buf, uint64_t off, uint32_t n)
{
    spin_lock(&snd_lock);
    int r = dsp_write_locked(vn, buf, off, n);
    spin_unlock(&snd_lock);
    return r;
}

/* ---- the device nodes ---- */

/* Our own nodes, made once: /dev/dsp and /dev/mixer, or dsp1 and mixer1
 * when a built-in card has those. Under the VFS lock. */
static void nodes_make(void)
{
    if (ua.dsp)
        return;
    struct vnode *devdir, *other;
    if (vfs_resolve("/dev", NULL, &devdir) < 0)
        return;
    int second = vfs_resolve("/dev/dsp", NULL, &other) == 0;
    ua.primary = !second;
    ua.dsp = vfs_mknod(devdir, second ? "dsp1" : "dsp",
                       MKDEV(SOUND_MAJOR, SOUND_MINOR_DSP + (second ? 0x80 : 0)), NULL, dsp_write);
    if (ua.dsp) {
        ua.dsp->dev_open = dsp_open;
        ua.dsp->dev_close = dsp_close;
        ua.dsp->dev_ioctl = dsp_ioctl;
        ua.dsp->dev_poll = dsp_poll;
    }
    ua.mixer = vfs_mknod(devdir, second ? "mixer1" : "mixer",
                         MKDEV(SOUND_MAJOR, SOUND_MINOR_MIXER + (second ? 0x80 : 0)), NULL, NULL);
    if (ua.mixer)
        ua.mixer->dev_ioctl = mixer_ioctl;
}

/* Point the built-in card's /dev/dsp and /dev/mixer here, unless they are open. */
static void nodes_take(void)
{
    struct vnode *dsp, *mixer;
    if (ua.primary || ua.taken_dsp || cmdline_has("usbaudio=secondary"))
        return;
    if (vfs_resolve("/dev/dsp", NULL, &dsp) < 0 || dsp->refs > 0 || dsp->dev_write == dsp_write)
        return;
    ua.saved_open = dsp->dev_open;
    ua.saved_close = dsp->dev_close;
    ua.saved_write = dsp->dev_write;
    ua.saved_ioctl = dsp->dev_ioctl;
    ua.saved_poll = dsp->dev_poll;
    dsp->dev_open = dsp_open;
    dsp->dev_close = dsp_close;
    dsp->dev_write = dsp_write;
    dsp->dev_ioctl = dsp_ioctl;
    dsp->dev_poll = dsp_poll;
    ua.taken_dsp = dsp;
    if (vfs_resolve("/dev/mixer", NULL, &mixer) == 0 && mixer->refs == 0 &&
        mixer->dev_ioctl != mixer_ioctl) {
        ua.saved_mixer_ioctl = mixer->dev_ioctl;
        mixer->dev_ioctl = mixer_ioctl;
        ua.taken_mixer = mixer;
    }
}

static void nodes_give_back(void)
{
    struct vnode *dsp = ua.taken_dsp, *mixer = ua.taken_mixer;
    if (dsp && dsp->dev_write == dsp_write) {
        dsp->dev_open = ua.saved_open;
        dsp->dev_close = ua.saved_close;
        dsp->dev_write = ua.saved_write;
        dsp->dev_ioctl = ua.saved_ioctl;
        dsp->dev_poll = ua.saved_poll;
    }
    if (mixer && mixer->dev_ioctl == mixer_ioctl)
        mixer->dev_ioctl = ua.saved_mixer_ioctl;
    ua.taken_dsp = ua.taken_mixer = NULL;
}

/* The VFS lock comes before usb_lock, which the hub thread holds here. */
static void with_vfs(void (*fn)(void))
{
    spin_unlock(&usb_lock);
    vfs_lock();
    spin_lock(&usb_lock);
    fn();
    vfs_unlock();
}

int usbaudio_get_info(struct sound_info *si)
{
    if (ua.gone || !(ua.primary || ua.taken_dsp))
        return 0;
    snprintf(si->card, sizeof(si->card), "%s", ua.name);
    si->flags |= SOUNDF_DSP | SOUNDF_USB;
    return 1;
}

/* ---- the descriptors ---- */

static uint32_t rd24(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

/* How good a format is for us: 16-bit stereo at 48 kHz best, then by what resampling costs. */
static int score(const struct format *f)
{
    int s = f->subframe == 2 ? 1000 : f->subframe == 3 ? 700 : f->subframe == 4 ? 600 : 300;
    s += f->channels == 2 ? 500 : f->channels == 1 ? 300 : 100;
    s += f->rate == 48000 ? 300 : f->rate == 44100 ? 250 : f->rate < 48000 ? 100 + (int)(f->rate / 1000) : 50;
    return s;
}

/* A streaming interface's alternate setting as the parser gathers it. */
struct setting {
    struct format f;
    int have_ep, have_fmt, tag;
    uint32_t rates[16], nrates, lower, upper;
};

/* A gathered setting: each rate it offers is a candidate, unless its
 * packets would not hold a service interval's frames. */
static void consider(struct usb_dev *dev, const struct setting *st, struct format *best, int *found)
{
    const struct format *f = &st->f;
    if (!st->have_ep || !st->have_fmt || (st->tag != FORMAT_PCM && st->tag != FORMAT_PCM8))
        return;
    if (f->channels < 1 || f->channels > 8 || f->subframe < 1 || f->subframe > 4)
        return;
    uint32_t cands[18], nc = 0;
    if (st->nrates) {
        for (uint32_t i = 0; i < st->nrates; i++)
            cands[nc++] = st->rates[i];
    } else if (st->upper) {
        cands[nc++] = 48000 >= st->lower && 48000 <= st->upper ? 48000
                    : 44100 >= st->lower && 44100 <= st->upper ? 44100
                    : st->upper < 48000 ? st->upper : st->lower;
    }
    for (uint32_t i = 0; i < nc; i++) {
        struct format c = *f;
        c.rate = cands[i];
        c.unsigned8 = st->tag == FORMAT_PCM8;
        if (c.rate < 1000 || c.rate > MAX_DEV_RATE)
            continue;
        uint32_t pps = dev->speed >= USB_SPEED_HIGH ? 8000u >> MIN((c.ep.bInterval ? c.ep.bInterval : 1) - 1, 7)
                                                    : 1000;
        uint32_t need = (c.rate + pps - 1) / pps * c.channels * c.subframe;
        if (need > (c.ep.wMaxPacketSize & 0x7FFu))
            continue;
        if (!*found || score(&c) > score(best)) {
            *best = c;
            *found = 1;
        }
    }
}

/* The playback formats in the configuration: the best one into *best; 0 if there is none. */
static int find_format(struct usb_dev *dev, struct format *best)
{
    const uint8_t *d = dev->config;
    uint32_t len = dev->config_len;
    int found = 0, in_as = 0;
    struct setting st;
    memset(&st, 0, sizeof(st));
    for (uint32_t off = 0; off + 2 <= len && d[off] >= 2; off += d[off]) {
        if (off + d[off] > len)
            break;
        const uint8_t *p = d + off;
        if (p[1] == USB_DT_INTERFACE && d[off] >= 9) {
            const struct usb_iface_desc *ifd = (const void *)p;
            if (in_as)
                consider(dev, &st, best, &found);
            in_as = ifd->bInterfaceClass == USB_CLASS_AUDIO && ifd->bInterfaceSubClass == AUDIO_STREAMING &&
                    ifd->bAlternateSetting != 0;
            memset(&st, 0, sizeof(st));
            st.f.iface = ifd->bInterfaceNumber;
            st.f.alt = ifd->bAlternateSetting;
            continue;
        }
        if (!in_as)
            continue;
        if (p[1] == CS_INTERFACE && p[2] == AS_GENERAL && d[off] >= 7) {
            st.tag = p[5] | p[6] << 8;
        } else if (p[1] == CS_INTERFACE && p[2] == AS_FORMAT_TYPE && d[off] >= 8 && p[3] == 1) {
            st.f.channels = p[4];
            st.f.subframe = p[5];
            st.f.bits = p[6];
            uint32_t n = p[7];
            if (!n && d[off] >= 14) {
                st.lower = rd24(p + 8);
                st.upper = rd24(p + 11);
            }
            for (uint32_t i = 0; i < n && i < 16 && 8 + 3 * (i + 1) <= d[off]; i++)
                st.rates[st.nrates++] = rd24(p + 8 + 3 * i);
            st.have_fmt = 1;
        } else if (p[1] == USB_DT_ENDPOINT && d[off] >= 7) {
            const struct usb_ep_desc *e = (const void *)p;
            if ((e->bmAttributes & 3) != USB_EP_ISO)
                continue;
            if (!(e->bEndpointAddress & USB_DIR_IN) && !st.have_ep) {
                memcpy(&st.f.ep, e, sizeof(st.f.ep));
                st.have_ep = 1;
            } else if ((e->bEndpointAddress & USB_DIR_IN) && st.have_ep && (e->wMaxPacketSize & 0x7FF) <= 4) {
                memcpy(&st.f.fb, e, sizeof(st.f.fb));
                st.f.has_fb = 1;
            }
        } else if (p[1] == CS_ENDPOINT && p[2] == EP_GENERAL && d[off] >= 4 && st.have_ep) {
            st.f.ep_attrs = p[3];
        }
    }
    if (in_as)
        consider(dev, &st, best, &found);
    return found;
}

/* The feature unit the AudioControl interface describes: which channels have mute and volume. */
static void find_feature_unit(struct usb_dev *dev)
{
    const uint8_t *d = dev->config;
    uint32_t len = dev->config_len;
    int in_ac = 0;
    ua.fu = ua.fu_mute = ua.fu_volume = 0;
    for (uint32_t off = 0; off + 2 <= len && d[off] >= 2; off += d[off]) {
        if (off + d[off] > len)
            break;
        const uint8_t *p = d + off;
        if (p[1] == USB_DT_INTERFACE && d[off] >= 9) {
            const struct usb_iface_desc *ifd = (const void *)p;
            in_ac = ifd->bInterfaceNumber == ua.ac_iface;
            continue;
        }
        if (!in_ac || p[1] != CS_INTERFACE || p[2] != AC_FEATURE_UNIT || d[off] < 7)
            continue;
        uint32_t csize = p[5];
        if (!csize)
            continue;
        uint8_t mute = 0, vol = 0;
        for (uint32_t ch = 0; ch < 8 && 6 + csize * (ch + 1) <= d[off]; ch++) {
            uint8_t c = p[6 + csize * ch];
            if (c & FU_MUTE)
                mute |= (uint8_t)(1 << ch);
            if (c & FU_VOLUME)
                vol |= (uint8_t)(1 << ch);
        }
        if (!vol && !mute)
            continue;
        /* The master channel's controls stand for all; else each channel's. */
        if (vol & 1)
            vol = 1;
        if (mute & 1)
            mute = 1;
        ua.fu = p[3];
        ua.fu_mute = mute;
        ua.fu_volume = vol;
        return;                         /* the first with controls: the usual one on the way out */
    }
}

/* ---- the driver ---- */

static int alloc_buffers(void)
{
    if (!ua.ring) {
        uint32_t pages = pmm_alloc_contig(RING_PAGES);
        if (!pages)
            return -1;
        ua.ring = (uint8_t *)pages;
    }
    for (int k = 0; k < NX; k++)
        if (!ua.buf[k] && !(ua.buf[k] = usb_dma_alloc(XFER_BYTES)))
            return -1;
    if (!ua.fbuf && !(ua.fbuf = usb_dma_alloc(32)))
        return -1;
    return 0;
}

static int audio_probe(struct usb_dev *dev, const struct usb_iface_desc *ifd,
                       const uint8_t *desc, uint32_t len, void **priv)
{
    (void)desc; (void)len;
    if (ifd->bInterfaceClass != USB_CLASS_AUDIO)
        return -1;
    if (ifd->bInterfaceSubClass == AUDIO_STREAMING) {
        if (ua.dev != dev)
            return -1;
        *priv = &ua;                    /* ours too: the control interface set it up */
        return 0;
    }
    if (ifd->bInterfaceSubClass != AUDIO_CONTROL || cmdline_has("usbaudio=off"))
        return -1;
    if (ifd->bInterfaceProtocol == 0x20) {
        klog("usb: %s is a USB Audio 2.0 device, which is not supported yet\n", dev->product);
        return -1;
    }
    if (ua.dev) {
        klog("usb: another USB audio device is already in use; ignoring %s\n", dev->product);
        return -1;
    }
    struct format f;
    if (!find_format(dev, &f)) {
        klog("usb: %s offers no playback format this driver can use\n", dev->product);
        return -1;
    }
    if (alloc_buffers() < 0)
        return -1;

    ua.ac_iface = ifd->bInterfaceNumber;
    ua.f = f;
    ua.df = (uint32_t)f.channels * f.subframe;
    find_feature_unit(dev);

    /* The streaming interface's setting, its endpoints, the rate. */
    int r = usb_control(dev, USB_RT_INTERFACE, USB_REQ_SET_INTERFACE, f.alt, f.iface, NULL, 0);
    if (r < 0) {
        klog("usb: %s will not select its audio format (%d)\n", dev->product, r);
        return -1;
    }
    ua.ep = usb_ep_enable(dev, &f.ep);
    ua.fb_ep = f.has_fb && (f.ep.bmAttributes & SYNC_MASK) == SYNC_ASYNC ? usb_ep_enable(dev, &f.fb) : NULL;
    if (!ua.ep) {
        klog("usb: %s: the controller would not set up the audio endpoint\n", dev->product);
        return -1;
    }
    if (f.ep_attrs & EP_ATTR_FREQ_CTL) {
        uint8_t rate[3] = { (uint8_t)f.rate, (uint8_t)(f.rate >> 8), (uint8_t)(f.rate >> 16) };
        r = usb_control(dev, USB_RT_CLASS | USB_RT_ENDPOINT, REQ_SET_CUR, EP_SAMPLING_FREQ << 8,
                        f.ep.bEndpointAddress, rate, 3);
        if (r < 0)
            klog("usb: %s did not take the sampling rate %u (%d)\n", dev->product, f.rate, r);
    }
    ua.pps = dev->speed >= USB_SPEED_HIGH ? 8000u >> MIN((f.ep.bInterval ? f.ep.bInterval : 1) - 1, 7) : 1000;
    ua.ff = (uint32_t)(((uint64_t)f.rate << 16) / ua.pps);
    ua.fb_bytes = ua.fb_ep ? (ua.fb_ep->maxpacket >= 4 ? 4 : 3) : 0;
    uint32_t mps = ua.ep->maxpacket ? ua.ep->maxpacket : 8;
    ua.npk = (int)MIN(MIN((uint32_t)USB_ISO_MAX_PACKETS, XFER_BYTES / mps), ua.pps >= 8000 ? 16u : 8u);
    if (ua.npk < 1)
        ua.npk = 1;

    /* The volume range, and a starting level. */
    ua.dev = dev;
    ua.gone = 0;
    if (ua.fu_volume) {
        uint8_t ch = 0;
        while (ch < 8 && !(ua.fu_volume & (1 << ch)))
            ch++;
        uint8_t v[2];
        ua.vol_min = -32768;
        ua.vol_max = 0;
        ua.vol_res = 256;
        if (fu_request(REQ_GET_MIN, FU_VOLUME, ch, v, 2) == 2)
            ua.vol_min = (int16_t)(v[0] | v[1] << 8);
        if (fu_request(REQ_GET_MAX, FU_VOLUME, ch, v, 2) == 2)
            ua.vol_max = (int16_t)(v[0] | v[1] << 8);
        if (fu_request(REQ_GET_RES, FU_VOLUME, ch, v, 2) == 2)
            ua.vol_res = (int16_t)(v[0] | v[1] << 8);
        if (ua.vol_max <= ua.vol_min) {
            ua.vol_min = -32768;
            ua.vol_max = 0;
        }
    }
    if (!ua.master)
        ua.master = 80;
    if (!ua.pcm)
        ua.pcm = 100;
    apply_master(ua.master);

    snprintf(ua.name, sizeof(ua.name), "%s%s%s", dev->manufacturer, dev->manufacturer[0] ? " " : "",
             dev->product[0] ? dev->product : "USB audio");
    for (int k = 0; k < NX; k++) {
        memset(&ua.x[k], 0, sizeof(ua.x[k]));
        ua.x[k].dev = dev;
        ua.x[k].ep = ua.ep;
        ua.x[k].buf = ua.buf[k];
        ua.x[k].complete = xfer_done;
    }
    memset(&ua.fx, 0, sizeof(ua.fx));
    ua.fx.dev = dev;
    ua.fx.ep = ua.fb_ep;
    ua.fx.buf = ua.fbuf;
    ua.fx.complete = fb_done;
    ua.streaming = 0;
    ua.inflight = 0;
    ua.fb_running = 0;
    ua.fb_ff = 0;
    ua.underruns = ua.errors = 0;
    ua.rhead = ua.rtail = 0;
    ua.open_pid = 0;
    set_defaults();

    with_vfs(nodes_make);
    with_vfs(nodes_take);
    klog("usb: audio: %s: %u Hz, %d channel%s, %d-bit, %s packets of up to %u bytes%s%s\n",
         ua.name, f.rate, f.channels, f.channels == 1 ? "" : "s", f.bits,
         dev->speed >= USB_SPEED_HIGH ? "high-speed" : "full-speed", mps,
         ua.fb_ep ? ", rate feedback" : "", ua.fu_volume ? ", volume control" : "");
    klog("usb: audio: %s is /dev/%s%s\n", ua.name, ua.primary ? "dsp" : "dsp1",
         ua.taken_dsp ? " and /dev/dsp while it is plugged in" : "");
    *priv = &ua;
    return 0;
}

static void audio_disconnect(struct usb_dev *dev, void *priv)
{
    (void)priv;
    if (ua.dev != dev)
        return;                         /* each of its interfaces says so once */
    ua.gone = 1;
    ua.streaming = 0;
    if (ua.inflight)
        usb_cancel(&ua.x[0]);
    if (ua.fb_running)
        usb_cancel(&ua.fx);
    ua.inflight = 0;
    ua.fb_running = 0;
    ua.dev = NULL;
    ua.ep = ua.fb_ep = NULL;
    ua.need_reset = 0;
    ua.rtail = ua.rhead;
    int had = ua.taken_dsp != NULL;
    with_vfs(nodes_give_back);
    wakeup(&ua);
    poll_wakeup();
    klog("usb: audio: %s is gone%s\n", ua.name, had ? "; /dev/dsp is the built-in card's again" : "");
}

const struct usb_driver usbaudio_driver = {
    .name = "audio",
    .probe = audio_probe,
    .disconnect = audio_disconnect,
};
