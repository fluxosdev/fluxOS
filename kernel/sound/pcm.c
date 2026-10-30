/*
 * The software mixer behind /dev/dsp, so that several programs can play
 * at once: the desktop's sounds over a music player, a DOS game's sound
 * effects over both.
 *
 * Every open of /dev/dsp is a stream of its own. The node in /dev only
 * clones (dev_clone): opening it makes an anonymous device for the new
 * stream, as /dev/ptmx-style devices do, so each opener has its own
 * format (8 or 16 bits, mono or stereo, 4000 to 48000 Hz, OSS's 8 kHz
 * unsigned 8-bit mono to begin with) and its own ring of what write()
 * gave it, kept as written.
 *
 * A card's driver is the mixer's backend (struct pcm_backend). It plays
 * 16-bit stereo at one rate of its own from a ring of its own, and the
 * mixer feeds that ring: each stream is resampled to the card's rate (by
 * linear interpolation, as the drivers did before) and added to the
 * others, then the sum is scaled by the software volume and clipped.
 * Mixed frames cannot be taken back, so the card's ring is only kept a
 * little ahead of what is heard (the backend's lead); the rest waits in
 * the streams' rings, where a stream that joins later is mixed in at
 * once. The mixer runs when a stream is written to, when a card that
 * refills under snd_lock is about to (pcm_pull: the Sound Blaster, HD
 * Audio), and on every timer tick (USB audio, whose refills run under
 * usb_lock). A stream with nothing to give is silent while the others
 * play on: a writer that falls behind hears a gap, not a stall.
 *
 * A card starts once its ring holds the lead, or sooner when a stream
 * asks (closing, SNDCTL_DSP_SYNC or _POST), and stops by itself when only
 * silence is left. Closing a stream lets what was written play out; the
 * stream is freed once that has been heard, or at once if its card will
 * not start. SNDCTL_DSP_RESET forgets what the stream has; what the card
 * was given is mixed, so the card is stopped and emptied only when all of
 * that is the stream's own.
 *
 * Which mixer /dev/dsp opens is its node's dev_priv: USB audio points it
 * at the USB device's mixer while one is plugged in, and back after.
 * Streams already open play on where they were.
 *
 * Everything here is under snd_lock.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "syscall.h"
#include "vfs.h"

#define MAX_PCMS    4
#define MAX_STREAMS 16                  /* a card's, open or playing out */
#define RING_SIZE   65536u              /* a stream's, in write()'s format */
#define FRAG_SIZE   4096                /* what SNDCTL_DSP_GETOSPACE calls a fragment */
#define CHUNK       256                 /* frames mixed at a time */
#define MIN_RATE    4000
#define MAX_RATE    48000

struct pcm_stream {
    struct pcm *pcm;
    struct pcm_stream *next;
    int closed;                         /* its file is gone; it plays out */
    int flush;                          /* play what it has, however little */
    int rate, channels, fmt;            /* what write() is given */
    uint32_t uf;                        /* bytes in one of write()'s frames */
    int out_rate;                       /* the card's rate step is for */
    uint32_t pos, step;                 /* the resampler, 16.16 from prev to cur */
    int32_t prev_l, prev_r, cur_l, cur_r;
    uint32_t end;                       /* the mixer's count after this stream's last frame;
                                           never more than 2^30 behind, so it compares */
    uint32_t rhead, rtail;              /* free-running */
    uint8_t ring[RING_SIZE];
};

struct pcm {
    struct pcm_backend *be;
    struct pcm_stream *streams;
    int nstreams;
    int active;                         /* streams still open */
    uint32_t mixed;                     /* frames given to the card */
    uint32_t progress;                  /* times frames were */
};

static struct pcm pcms[MAX_PCMS];
static int npcms;
static int32_t acc[CHUNK * 2];          /* the sum, before the volume and clipping */
static int16_t mix_out[CHUNK * 2];

static uint32_t ring_used(const struct pcm_stream *s)
{
    return s->rhead - s->rtail;
}

static int gone(const struct pcm *p)
{
    return p->be->gone && p->be->gone();
}

static void reset_resampler(struct pcm_stream *s)
{
    s->pos = 0x10000;                   /* the first frame is read before anything is made */
    s->prev_l = s->prev_r = s->cur_l = s->cur_r = 0;
    s->out_rate = 0;
}

static void set_format(struct pcm_stream *s, int rate, int channels, int fmt)
{
    s->rate = rate;
    s->channels = channels;
    s->fmt = fmt;
    s->uf = (uint32_t)channels * (fmt == AFMT_S16_LE ? 2 : 1);
    s->rtail = s->rhead;                /* a part frame of the old format */
    reset_resampler(s);
}

/* ---- mixing ---- */

/* The next frame of write()'s as cur, the last one as prev; 0 if a whole one is not there. */
static int next_frame(struct pcm_stream *s)
{
    if (ring_used(s) < s->uf)
        return 0;
    int32_t v[2] = { 0, 0 };
    for (int c = 0; c < s->channels; c++) {
        if (s->fmt == AFMT_S16_LE) {
            uint8_t lo = s->ring[s->rtail++ % RING_SIZE];
            uint8_t hi = s->ring[s->rtail++ % RING_SIZE];
            v[c] = (int16_t)(lo | hi << 8);
        } else {
            v[c] = (s->ring[s->rtail++ % RING_SIZE] - 128) << 8;
        }
    }
    if (s->channels == 1)
        v[1] = v[0];
    s->prev_l = s->cur_l;
    s->prev_r = s->cur_r;
    s->cur_l = v[0];
    s->cur_r = v[1];
    return 1;
}

/* Add up to n of s's frames at the card's rate into acc; how many it had. */
static uint32_t stream_mix(struct pcm_stream *s, int out_rate, uint32_t n)
{
    if (s->out_rate != out_rate) {
        s->out_rate = out_rate;
        s->step = (uint32_t)(((uint32_t)s->rate << 16) / (uint32_t)out_rate);
    }
    for (uint32_t i = 0; i < n; i++) {
        while (s->pos >= 0x10000) {
            if (!next_frame(s))
                return i;
            s->pos -= 0x10000;
        }
        int32_t f = (int32_t)(s->pos >> 2);             /* 14 bits: no overflow below */
        acc[2 * i] += s->prev_l + (((s->cur_l - s->prev_l) * f) >> 14);
        acc[2 * i + 1] += s->prev_r + (((s->cur_r - s->prev_r) * f) >> 14);
        s->pos += s->step;
    }
    return n;
}

/* Frames of s's the card was given and has not played yet. */
static uint32_t unplayed(const struct pcm_stream *s)
{
    const struct pcm *p = s->pcm;
    uint32_t played = p->mixed - p->be->queued();
    int32_t d = (int32_t)(s->end - played);
    return d > 0 ? (uint32_t)d : 0;
}

/* Free the streams that were closed and have been heard, or whose card
 * went or would not start (dead): theirs will never play. */
static void reap(struct pcm *p, int dead)
{
    struct pcm_stream **pp = &p->streams;
    while (*pp) {
        struct pcm_stream *s = *pp;
        if (s->closed && (dead || gone(p) || (ring_used(s) < s->uf && !unplayed(s)))) {
            *pp = s->next;
            p->nstreams--;
            kfree(s);
        } else {
            pp = &s->next;
        }
    }
}

/* Mix up to n frames into the card; how many went. */
static uint32_t pump(struct pcm *p, uint32_t n)
{
    struct pcm_backend *be = p->be;
    uint32_t done = 0;
    if (gone(p))
        return 0;
    while (done < n && p->streams) {
        uint32_t c = MIN(MIN(n - done, (uint32_t)CHUNK), be->room());
        if (!c)
            break;
        memset(acc, 0, c * 2 * sizeof(acc[0]));
        uint32_t got = 0;
        for (struct pcm_stream *s = p->streams; s; s = s->next) {
            uint32_t k = stream_mix(s, be->rate, c);
            if (k)
                s->end = p->mixed + k;
            else if (p->mixed - s->end > 1u << 30)
                s->end = p->mixed - (1u << 30);         /* idle: long heard */
            got = MAX(got, k);
        }
        if (!got)
            break;
        int32_t gain = be->gain ? be->gain() : 256;
        for (uint32_t i = 0; i < 2 * got; i++) {
            int32_t v = acc[i] * gain / 256;
            mix_out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        be->put(mix_out, got);
        p->mixed += got;
        p->progress++;
        done += got;
        if (got < c)
            break;                      /* every stream ran dry */
    }
    reap(p, 0);
    if (done) {
        wakeup(p);
        poll_wakeup();
    }
    return done;
}

/* Top the card's ring up to its lead, and start it when that is reached or a stream asks. */
static void kick(struct pcm *p, int force)
{
    struct pcm_backend *be = p->be;
    if (gone(p))
        return;
    uint32_t q = be->queued();
    if (q < be->lead)
        pump(p, be->lead - q);
    if (!be->running() && (force || be->queued() >= be->lead))
        be->start();
}

static int flushing(const struct pcm *p)
{
    for (struct pcm_stream *s = p->streams; s; s = s->next)
        if ((s->closed || s->flush) && ring_used(s) >= s->uf)
            return 1;
    return 0;
}

/* Does a stream other than s have frames the card has not played? */
static int others_queued(const struct pcm_stream *s)
{
    for (const struct pcm_stream *o = s->pcm->streams; o; o = o->next)
        if (o != s && (ring_used(o) >= o->uf || unplayed(o)))
            return 1;
    return 0;
}

/* Sleep until the card takes or plays something; -EIO when a second passes without it. */
static int wait_card(struct pcm_stream *s)
{
    struct pcm *p = s->pcm;
    struct pcm_backend *be = p->be;
    kick(p, 1);
    if (gone(p))
        return -ENODEV;
    if (!be->running())
        return -EIO;                    /* it would not start */
    /* The tick wakes us every 10 ms; only a whole second of nothing is a stall. */
    uint32_t seen = p->progress, q = be->queued(), since = ticks;
    for (;;) {
        sleep_on_lock_timeout(p, &snd_lock, since + HZ);
        if (signal_pending(current))
            return -EINTR;
        if (gone(p))
            return -ENODEV;
        if (!be->running() || p->progress != seen || be->queued() != q)
            return 0;
        if ((int32_t)(ticks - since) >= HZ)
            break;
    }
    klog("pcm: the card has stopped playing, giving up on the stream\n");
    if (be->stalled)
        be->stalled();
    s->rtail = s->rhead;
    return -EIO;
}

/* Wait until everything written to s has been heard. */
static int drain(struct pcm_stream *s)
{
    s->flush = 1;
    while (ring_used(s) >= s->uf || unplayed(s)) {
        int r = wait_card(s);
        if (r < 0)
            return r;
    }
    return 0;
}

/* ---- a stream's device calls ---- */

static int stream_write(struct vnode *vn, const void *buf, uint64_t off, uint32_t n)
{
    (void)off;
    struct pcm_stream *s = vn->dev_priv;
    struct pcm *p = s->pcm;
    const uint8_t *in = buf;
    uint32_t used = 0;
    int r = 0;
    spin_lock(&snd_lock);
    s->flush = 0;
    while (used < n) {
        if (gone(p)) {
            r = -ENODEV;
            break;
        }
        uint32_t room = RING_SIZE - ring_used(s);
        if (!room) {
            if ((r = wait_card(s)) < 0)
                break;
            continue;
        }
        uint32_t k = MIN(room, n - used), at = s->rhead % RING_SIZE;
        uint32_t first = MIN(k, RING_SIZE - at);
        memcpy(s->ring + at, in + used, first);
        memcpy(s->ring, in + used + first, k - first);
        s->rhead += k;
        used += k;
        kick(p, 0);
    }
    spin_unlock(&snd_lock);
    return used ? (int)used : r;
}

static int stream_poll(struct vnode *vn)
{
    struct pcm_stream *s = vn->dev_priv;
    spin_lock(&snd_lock);
    int r = gone(s->pcm) ? POLLOUT | POLLERR : RING_SIZE - ring_used(s) >= FRAG_SIZE ? POLLOUT : 0;
    spin_unlock(&snd_lock);
    return r;
}

static int get_int(uint32_t arg, int *v)
{
    return copy_from_user(v, (const void *)arg, sizeof(*v));
}

static int put_int(uint32_t arg, int v)
{
    return copy_to_user((void *)arg, &v, sizeof(v));
}

/* A new format, once what was written in the old one has played. */
static int change_format(struct pcm_stream *s, int rate, int channels, int fmt)
{
    if (rate == s->rate && channels == s->channels && fmt == s->fmt)
        return 0;
    int r = drain(s);
    if (r < 0)
        return r;
    set_format(s, rate, channels, fmt);
    return 0;
}

static int stream_ioctl_locked(struct pcm_stream *s, uint32_t req, uint32_t arg)
{
    struct pcm *p = s->pcm;
    int v, r;
    if (gone(p) && req != SOUND_INFO)
        return -ENODEV;
    switch (req) {
    case SNDCTL_DSP_RESET:
        s->rtail = s->rhead;
        s->flush = 0;
        reset_resampler(s);
        /* What the card was given is mixed: it can be taken back only if it
         * is all this stream's. Otherwise it plays (at most the lead). */
        if (p->be->drop && unplayed(s) && !others_queued(s))
            p->be->drop();
        return 0;
    case SNDCTL_DSP_SYNC:
        return drain(s);
    case SNDCTL_DSP_POST:
        s->flush = 1;
        kick(p, 1);
        return 0;
    case SNDCTL_DSP_SPEED:
        if ((r = get_int(arg, &v)) < 0)
            return r;
        v = v < MIN_RATE ? MIN_RATE : v > MAX_RATE ? MAX_RATE : v;
        if ((r = change_format(s, v, s->channels, s->fmt)) < 0)
            return r;
        return put_int(arg, s->rate);
    case SNDCTL_DSP_CHANNELS:
        if ((r = get_int(arg, &v)) < 0)
            return r;
        if ((r = change_format(s, s->rate, v >= 2 ? 2 : 1, s->fmt)) < 0)
            return r;
        return put_int(arg, s->channels);
    case SNDCTL_DSP_SETFMT:
        if ((r = get_int(arg, &v)) < 0)
            return r;
        if ((r = change_format(s, s->rate, s->channels, v == AFMT_S16_LE ? AFMT_S16_LE : AFMT_U8)) < 0)
            return r;
        return put_int(arg, s->fmt);
    case SNDCTL_DSP_GETOSPACE: {
        struct audio_buf_info bi;
        uint32_t room = RING_SIZE - ring_used(s);
        bi.bytes = (int32_t)(room - room % s->uf);
        bi.fragsize = FRAG_SIZE;
        bi.fragstotal = RING_SIZE / FRAG_SIZE;
        bi.fragments = bi.bytes / FRAG_SIZE;
        return copy_to_user((void *)arg, &bi, sizeof(bi));
    }
    case SNDCTL_DSP_GETODELAY: {
        uint64_t frames = (uint64_t)unplayed(s) * (uint32_t)s->rate / (uint32_t)p->be->rate;
        return put_int(arg, (int)(ring_used(s) + frames * s->uf));
    }
    case SOUND_INFO: {
        struct sound_info si;
        sound_get_info(&si);
        return copy_to_user((void *)arg, &si, sizeof(si));
    }
    }
    return -EINVAL;
}

static int stream_ioctl(struct vnode *vn, uint32_t req, uint32_t arg)
{
    struct pcm_stream *s = vn->dev_priv;
    spin_lock(&snd_lock);
    int r = stream_ioctl_locked(s, req, arg);
    spin_unlock(&snd_lock);
    if (req == SNDCTL_DSP_RESET && s->pcm->be->recover)
        s->pcm->be->recover();          /* after a stall or drop(), as at an open */
    return r;
}

/* Its last file was closed: what it has plays out, then it goes. */
static void stream_release(struct vnode *vn)
{
    struct pcm_stream *s = vn->dev_priv;
    struct pcm *p = s->pcm;
    spin_lock(&snd_lock);
    s->closed = 1;
    p->active--;
    kick(p, 1);
    reap(p, !gone(p) && !p->be->running());    /* asked to start, it did not */
    spin_unlock(&snd_lock);
}

/* Opening /dev/dsp: a stream of the opener's own on the node's mixer. */
static int pcm_clone(struct vnode *node, int flags, struct vnode **out)
{
    struct pcm *p = node->dev_priv;
    if ((flags & O_ACCMODE) == O_RDONLY)
        return -EINVAL;                 /* no recording */
    if (p->be->recover)
        p->be->recover();
    struct pcm_stream *s = kzalloc(sizeof(*s));
    if (!s)
        return -ENOMEM;
    struct vnode *vn = vfs_mknod_anon(node->rdev, NULL, stream_write);
    if (!vn) {
        kfree(s);
        return -ENOMEM;
    }
    s->pcm = p;
    set_format(s, 8000, 1, AFMT_U8);
    spin_lock(&snd_lock);
    reap(p, 0);                         /* what recover() let go */
    int r = gone(p) ? -ENODEV : p->nstreams >= MAX_STREAMS ? -EBUSY : 0;
    if (!r) {
        s->end = p->mixed - p->be->queued();    /* nothing of its own unplayed */
        s->next = p->streams;
        p->streams = s;
        p->nstreams++;
        p->active++;
    }
    spin_unlock(&snd_lock);
    if (r < 0) {
        vfs_unref(vn);                  /* no hooks yet: it just goes */
        kfree(s);
        return r;
    }
    vn->mode = node->mode;
    vn->dev_priv = s;
    vn->dev_poll = stream_poll;
    vn->dev_ioctl = stream_ioctl;
    vn->dev_release = stream_release;
    *out = vn;
    return 0;
}

/* ---- the drivers' side ---- */

struct pcm *pcm_create(struct vnode *dir, const char *name, uint32_t rdev, struct pcm_backend *be)
{
    if (npcms == MAX_PCMS)
        return NULL;
    struct vnode *vn = vfs_mknod(dir, name, rdev, NULL, NULL);
    if (!vn)
        return NULL;
    struct pcm *p = &pcms[npcms];
    p->be = be;
    vn->dev_clone = pcm_clone;
    vn->dev_priv = p;
    __atomic_store_n(&npcms, npcms + 1, __ATOMIC_RELEASE);         /* pcm_tick may look now */
    return p;
}

void pcm_pull(struct pcm *p, uint32_t frames)
{
    if (!p)
        return;
    if (frames)
        pump(p, frames);
    wakeup(p);                          /* the card moved on: drain() looks again */
}

int pcm_active(const struct pcm *p)
{
    return p ? __atomic_load_n(&p->active, __ATOMIC_RELAXED) : 0;
}

struct pcm *pcm_of(struct vnode *vn)
{
    return vn && vn->dev_clone == pcm_clone ? vn->dev_priv : NULL;
}

void pcm_point(struct vnode *vn, struct pcm *p)
{
    if (pcm_of(vn) && p)
        vn->dev_priv = p;
}

/* From the timer: keep every card fed, start one a closing stream is waiting on. */
void pcm_tick(void)
{
    int n = __atomic_load_n(&npcms, __ATOMIC_ACQUIRE);
    if (!n || !spin_trylock(&snd_lock))
        return;
    for (int i = 0; i < n; i++) {
        struct pcm *p = &pcms[i];
        if (!p->streams)
            continue;
        if (gone(p)) {
            reap(p, 0);
        } else {
            int f = flushing(p);
            kick(p, f);
            if (f && !p->be->running())
                reap(p, 1);             /* it would not start */
        }
        wakeup(p);
    }
    spin_unlock(&snd_lock);
}
