/* The software mixer (pcm.c) over a fake card: streams opened through the
 * /dev/dsp node, mixed, clipped, resampled, drained and played out. */
#include "kernel-test.h"
#include "../kernel/sound/pcm.c"

struct proc *current;
volatile uint32_t ticks;
spinlock_t snd_lock;
static int sleeps;

void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
void wakeup(void *chan) { (void)chan; }
void poll_wakeup(void) {}
int signal_pending(struct proc *p) { (void)p; return 0; }
void klog(const char *fmt, ...) { (void)fmt; }
void sound_get_info(struct sound_info *si) { memset(si, 0, sizeof(*si)); }
/* ioctl arguments are 32 bits: on a 64-bit host they are offsets into this "user memory". */
static uint8_t umem[64];
int copy_from_user(void *dst, const void *src, uint32_t n) { memcpy(dst, umem + (uintptr_t)src, n); return 0; }
int copy_to_user(void *dst, const void *src, uint32_t n) { memcpy(umem + (uintptr_t)dst, src, n); return 0; }

static int ioctl_int(struct vnode *vn, uint32_t req, int *v)
{
    memcpy(umem, v, sizeof(*v));
    int r = vn->dev_ioctl(vn, req, 0);
    memcpy(v, umem, sizeof(*v));
    return r;
}
static struct vnode *node;             /* /dev/dsp, as pcm_create made it */

struct vnode *vfs_mknod(struct vnode *dir, const char *name, uint32_t rdev, dev_read_t rd, dev_write_t wr)
{
    (void)name;
    struct vnode *vn = calloc(1, sizeof(*vn));
    if (dir)
        node = vn;
    vn->type = VN_CHR;
    vn->rdev = rdev;
    vn->dev_read = rd;
    vn->dev_write = wr;
    vn->mode = 0666;
    return vn;
}
struct vnode *vfs_mknod_anon(uint32_t rdev, dev_read_t rd, dev_write_t wr)
{
    return vfs_mknod(NULL, "", rdev, rd, wr);   /* no directory: not the node */
}
/* The last reference to a stream's vnode: what file_close does in the end. */
void vfs_unref(struct vnode *vn)
{
    if (vn->dev_release)
        vn->dev_release(vn);
    free(vn);
}

/* ---- the fake card: a ring of frames it "plays" when told to ---- */
#define CARD_FRAMES 8192
static int16_t card[CARD_FRAMES * 2];
static uint32_t chead, ctail;
static int crunning, cdead, drops, recovers;

static uint32_t c_room(void) { return CARD_FRAMES - (chead - ctail); }
static uint32_t c_queued(void) { return chead - ctail; }
static void c_put(const int16_t *lr, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, chead++) {
        card[2 * (chead % CARD_FRAMES)] = lr[2 * i];
        card[2 * (chead % CARD_FRAMES) + 1] = lr[2 * i + 1];
    }
}
static void c_start(void) { if (chead != ctail && !cdead) crunning = 1; }
static int c_running(void) { return crunning; }
static void c_drop(void) { drops++; ctail = chead; crunning = 0; }
static void c_recover(void) { recovers++; }
static struct pcm_backend card_be = {
    .rate = 48000, .lead = 64,
    .room = c_room, .queued = c_queued, .put = c_put, .start = c_start, .running = c_running,
    .drop = c_drop, .recover = c_recover,
};

/* The card plays n frames; the mixer is asked for more, as a refill would. */
static void play(uint32_t n)
{
    ctail += MIN(n, chead - ctail);
    if (chead == ctail)
        crunning = 0;
    pcm_tick();
}

/* Sleeping (a full ring, SNDCTL_DSP_SYNC): time passes and the card plays on. */
void sleep_on_lock_timeout(void *chan, spinlock_t *lk, uint32_t deadline)
{
    (void)chan; (void)lk; (void)deadline;
    sleeps++;
    play(32);
}

static struct vnode *open_dsp(int rate, int channels, int fmt)
{
    struct vnode *vn;
    CHECK(node->dev_clone(node, O_WRONLY, &vn) == 0 && vn != node);
    CHECK(ioctl_int(vn, SNDCTL_DSP_SPEED, &rate) == 0);
    CHECK(ioctl_int(vn, SNDCTL_DSP_CHANNELS, &channels) == 0);
    CHECK(ioctl_int(vn, SNDCTL_DSP_SETFMT, &fmt) == 0);
    return vn;
}

static void write_s16(struct vnode *vn, int16_t l, int16_t r, int frames)
{
    int16_t buf[2 * 512];
    for (int i = 0; i < frames; i++) {
        buf[2 * i] = l;
        buf[2 * i + 1] = r;
    }
    CHECK(vn->dev_write(vn, buf, 0, (uint32_t)frames * 4) == frames * 4);
}

static int16_t frame_l(uint32_t i) { return card[2 * (i % CARD_FRAMES)]; }
static int16_t frame_r(uint32_t i) { return card[2 * (i % CARD_FRAMES) + 1]; }

int main(void)
{
    static struct vnode devdir;
    struct pcm *p = pcm_create(&devdir, "dsp", 0, &card_be);
    CHECK(p && node && node->dev_clone);
    CHECK(pcm_of(node) == p);

    /* Recording is refused; OSS's 8 kHz U8 mono is where a stream starts. */
    struct vnode *vn;
    CHECK(node->dev_clone(node, O_RDONLY, &vn) == -EINVAL);

    /* Two streams at the card's rate add up. */
    struct vnode *a = open_dsp(48000, 2, AFMT_S16_LE), *b = open_dsp(48000, 2, AFMT_S16_LE);
    CHECK(pcm_active(p) == 2);
    write_s16(a, 1000, -1000, 200);
    write_s16(b, 2000, 500, 200);
    /* a alone filled the card to its lead, which started it; b joins at the next refill. */
    CHECK(chead == card_be.lead && crunning);
    CHECK(frame_l(0) == 0 && frame_l(1) == 1000);   /* the resampler starts from silence */
    play(64);
    CHECK(frame_l(64) == 1000);                     /* b's own first frame is silence */
    CHECK(frame_l(65) == 3000 && frame_r(65) == -500 && frame_l(127) == 3000);

    /* Clipping, not wrapping. */
    struct vnode *c = open_dsp(48000, 2, AFMT_S16_LE);
    write_s16(c, 32000, -32700, 200);            /* sums: 35000 and -33200 */
    play(64);
    CHECK(frame_l(chead - 1) == 32767 && frame_r(chead - 1) == -32768);

    /* A stream that has nothing does not hold up the others. Its frames
     * the card has are mixed with theirs, so they are not taken back. */
    CHECK(c->dev_ioctl(c, SNDCTL_DSP_RESET, 0) == 0);
    CHECK(drops == 0 && chead - ctail == 64);
    play(64);
    CHECK(frame_l(chead - 64) == 3000);             /* c is silent; a and b play on */
    CHECK(frame_l(chead - 1) == 2000);              /* a ran dry: b goes on alone */
    for (int i = 0; i < 8; i++)
        play(64);                                   /* a and b run dry */
    uint32_t before = chead;
    write_s16(b, 700, 700, 100);
    play(64);
    CHECK(chead > before && frame_l(chead - 1) == 700);

    /* 24 kHz mono 8-bit becomes twice the frames at 48 kHz, both channels. */
    vfs_unref(a);
    vfs_unref(b);
    vfs_unref(c);
    for (int i = 0; i < 8; i++)
        play(64);
    CHECK(p->nstreams == 0 && pcm_active(p) == 0);  /* closed and played out: freed */
    struct vnode *m = open_dsp(24000, 1, AFMT_U8);
    uint8_t u8[100];
    memset(u8, 128 + 64, sizeof(u8));               /* 64 << 8 = 16384 */
    before = chead;
    CHECK(m->dev_write(m, u8, 0, sizeof(u8)) == 100);
    crunning = 1;
    for (int i = 0; i < 8; i++)
        play(64);
    CHECK(chead - before >= 196 && chead - before <= 200);
    CHECK(frame_l(chead - 2) == 16384 && frame_r(chead - 2) == 16384);

    /* SNDCTL_DSP_SYNC waits until the card has played the stream's frames. */
    CHECK(m->dev_write(m, u8, 0, sizeof(u8)) == 100);
    int odelay;
    CHECK(ioctl_int(m, SNDCTL_DSP_GETODELAY, &odelay) == 0 && odelay > 0);
    sleeps = 0;
    CHECK(m->dev_ioctl(m, SNDCTL_DSP_SYNC, 0) == 0 && sleeps > 0);
    CHECK(ioctl_int(m, SNDCTL_DSP_GETODELAY, &odelay) == 0 && odelay == 0);

    /* A full ring makes write() wait for the card, not fail. */
    struct audio_buf_info bi;
    CHECK(m->dev_ioctl(m, SNDCTL_DSP_GETOSPACE, 0) == 0);
    memcpy(&bi, umem, sizeof(bi));
    CHECK(bi.bytes == RING_SIZE && bi.fragsize == FRAG_SIZE);
    static uint8_t big[RING_SIZE + 4096];
    memset(big, 128, sizeof(big));
    sleeps = 0;
    CHECK(m->dev_write(m, big, 0, sizeof(big)) == (int)sizeof(big) && sleeps > 0);

    /* SNDCTL_DSP_RESET on the only stream with anything: the card stops and
     * forgets it, and is recovered as at an open (a USB stall). */
    vfs_unref(m);
    for (int i = 0; i < 10000 && p->nstreams; i++)
        play(64);
    CHECK(p->nstreams == 0 && chead == ctail && !crunning);
    struct vnode *r = open_dsp(48000, 2, AFMT_S16_LE), *idle = open_dsp(48000, 2, AFMT_S16_LE);
    write_s16(r, 1234, 1234, 200);
    CHECK(crunning && chead - ctail == card_be.lead);
    int recovered = recovers;
    CHECK(r->dev_ioctl(r, SNDCTL_DSP_RESET, 0) == 0);
    CHECK(drops == 1 && recovers == recovered + 1 && chead == ctail && !crunning);
    CHECK(ioctl_int(r, SNDCTL_DSP_GETODELAY, &odelay) == 0 && odelay == 0);
    play(64);
    CHECK(chead == ctail);                          /* none of it comes back */

    /* After 2^31 mixed frames (12 hours at 48 kHz) a new stream, and one that
     * sat idle all along, still drain at once: their ends are not 2^31 behind. */
    vfs_unref(r);
    p->mixed += 0x90000000u;                        /* that many were heard */
    struct vnode *late = open_dsp(44100, 1, AFMT_U8);
    int v = AFMT_S16_LE;
    CHECK(ioctl_int(late, SNDCTL_DSP_SETFMT, &v) == 0 && v == AFMT_S16_LE);
    for (int k = 0; k < 3; k++) {
        p->mixed += 0x50000000u;
        write_s16(late, 100, 100, 100);             /* the mixer runs; idle has nothing */
        for (int i = 0; i < 8; i++)
            play(64);
    }
    v = 22050;
    CHECK(ioctl_int(idle, SNDCTL_DSP_SPEED, &v) == 0 && v == 22050);
    CHECK(ioctl_int(idle, SNDCTL_DSP_GETODELAY, &odelay) == 0 && odelay == 0);
    CHECK(idle->dev_ioctl(idle, SNDCTL_DSP_SYNC, 0) == 0);
    vfs_unref(late);
    vfs_unref(idle);
    for (int i = 0; i < 8; i++)
        play(64);
    CHECK(p->nstreams == 0);

    /* A card that will not start: closed streams are freed all the same, so
     * opening goes on working (MAX_STREAMS of them would make it EBUSY). */
    cdead = 1;
    for (int i = 0; i < 2 * MAX_STREAMS; i++) {
        struct vnode *d;
        CHECK(node->dev_clone(node, O_WRONLY, &d) == 0);
        memset(u8, 200, sizeof(u8));
        CHECK(d->dev_write(d, u8, 0, sizeof(u8)) == 100);
        CHECK(!crunning);
        vfs_unref(d);
        CHECK(p->nstreams == 0);
    }
    /* ...and so are ones closed while it played that it then would not. */
    cdead = 0;
    ctail = chead;
    struct vnode *d = open_dsp(48000, 2, AFMT_S16_LE);
    write_s16(d, 5, 5, 400);
    vfs_unref(d);
    CHECK(crunning && p->nstreams == 1);
    cdead = 1;
    ctail = chead;                                  /* it stopped, as after a stall */
    crunning = 0;
    play(0);
    CHECK(p->nstreams == 0);
    cdead = 0;

    puts("kernel pcm: mixing, clipping, idle streams, resampling, close, sync, full rings, reset, "
         "2^31 frames and dead cards passed");
    return 0;
}
