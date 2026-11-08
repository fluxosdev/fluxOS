/*
 * /bin/wavemedia: Wave's media engine. It is Wave's sound player (webaudio.c, which still plays
 * <audio> files and mixes everything for /dev/dsp) with video added: it plays <video> elements,
 * from a file or address (progressive download) or from what a page appends through Media Source
 * Extensions, with FFmpeg. Wave starts it in place of "wave --audio" when it is there.
 *
 * What Wave tells it, on its input, one line each (PID: a player, which is one media element;
 * SBID: a SourceBuffer; times in ms unless they say us):
 *   vhdr PID HEADER          a request header for the next vopen ("Referer: ...")
 *   vopen PID URL            play an address or a file
 *   vmse PID                 play what SourceBuffers are given
 *   vsize PID W H            the box the picture is drawn in (frames are scaled to fit it)
 *   vplay PID MS             play (from MS; -1: from where it is)
 *   vpause PID / vseek PID MS / vvol PID PER_MILLE / vclose PID
 *   vbufok PID SHMID         Wave has mapped the frame buffers
 *   sbadd PID SBID MIME      a SourceBuffer of that type
 *   sbappend SBID SEQ PATH   bytes appended (in a file, which the engine reads and deletes)
 *   sbremove SBID SEQ S_US E_US / sbabort SBID / sboffset SBID US / sbdel SBID
 *   mseend PID 0|1           endOfStream() was called (1), or more is coming after all (0)
 * What it answers, on its output:
 *   vmeta PID DUR_MS W H AUDIO   what a file or address holds
 *   vbuf PID SHMID W H N         frames will be in this shared memory: N buffers of W x H pixels
 *   vframe PID SHMID I W H MS    a new frame, w x h, in buffer I
 *   vfirst PID                   the first frame is there (the element can play)
 *   vtime PID MS / vwait PID / vplaying PID / vseeked PID MS / vended PID
 *   vbuffered PID N S E...       a progressive download's buffered spans (ms)
 *   verror PID WHY
 *   sbinit SBID V A W H CODECS   an initialization segment: picture (V) and sound (A) tracks
 *   sbdone SBID SEQ N S E...     an append or remove has finished: the buffered spans (us)
 *   sberr SBID SEQ               what was appended could not be read
 *
 * Each player has a store (store.c) of coded frames for its picture and one for its sound, filled
 * by a demuxing thread (one reading the file or address, or one per SourceBuffer reading what is
 * appended), and two decoding threads walking them. The sound goes through a ring the mixer
 * empties, and what the card has played of it is the clock (without sound, the wall's clock);
 * pictures are shown when the clock reaches them, scaled into shared memory Wave draws from.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <flux.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "desktop.h"
#include "mediaio.h"
#include "segments.h"
#include "store.h"
#include "webaudio.h"

#define MAX_PLAYERS 16
#define MAX_SB      4
#define MAX_CFG     32
#define MAX_HDR     8
#define NBUF        2                   /* frame buffers in each player's shared memory */
#define RING        8192                /* sound frames decoded ahead of the mixer (~0.19 s) */
#define TOL         50000               /* gaps up to this (us) are not gaps */
#define AHEAD_US    (30 * US)           /* a download is read this far ahead of the clock */
#define BEHIND_US   (5 * US)            /* ...and kept this far behind it */
#define LATE_US     80000               /* a frame later than this is dropped */
#define V_BUDGET    (100u << 20)        /* coded picture bytes kept for a player at most */
#define A_BUDGET    (16u << 20)
#define MAX_W       1920
#define MAX_H       1080
#define US          1000000LL
#define IO_BUF      32768

static const AVRational US_TB = { 1, 1000000 };

struct cfgs {
    AVCodecParameters *p[MAX_CFG];
    int n;
};

/* Bytes appended to a SourceBuffer that one demuxer reads: one initialization segment and the media
 * segments after it. */
struct group {
    uint8_t *d;
    size_t len, cap, rd;
    int closed;                         /* nothing more comes to it: its demuxer reaches the end */
    struct group *next;
};

struct player;

struct sbuf {
    int id;
    struct player *pl;
    int kind;                           /* SEG_* */
    struct group *head;                 /* head: what the demuxer reads */
    uint8_t *init;                      /* the last initialization segment */
    size_t init_len;
    int fresh;                          /* the next media bytes need init in front (a new demuxer) */
    int seq, busy;                      /* the append under way, answered once its bytes are used up */
    int64_t offset;                     /* timestampOffset */
    int feeds_v, feeds_a;               /* the player's stores it fills */
    int quit;
    pthread_t th;
    int started;
};

struct player {
    int id, mse;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int quit;
    struct store vs, as;
    struct cfgs vc, ac;
    int has_v, has_a;
    int vw, vh;                         /* the picture's own size */
    /* a file or address */
    char *url;
    char *hdr[MAX_HDR];
    int nhdr;
    struct media_input in;
    int src_eof, seek_src;
    int64_t src_seek_us;
    pthread_t tsrc;
    int src_started;
    /* Media Source Extensions */
    struct sbuf *sb[MAX_SB];
    int nsb;
    int eos;                            /* no more is coming: the end of what is there is the end */
    /* playing */
    int paused, serial;
    int64_t target_us;                  /* where it was last sought to: what is before it is not shown */
    int show_one;                       /* show the next frame at once (after a seek, or at the start) */
    int seek_pending;                   /* "vseeked" once something is there */
    int64_t pos_us;                     /* the wall clock: where it was at clk_at */
    uint32_t clk_at;
    int vstall, astall, vdone, adone;
    int frames_shown;
    /* sound */
    int16_t ring[RING * 2];
    int rhead, rfill;
    int64_t ring_end_us;                /* the time of the end of what is in the ring */
    int a_have;                         /* the ring has had sound since the last seek */
    int volume;                         /* per mille */
    int delay;                          /* frames the card holds */
    /* pictures */
    int box_w, box_h;
    int shmid, shm_w, shm_h;
    uint8_t *shm;
    int fidx;
    pthread_t tv, ta;
    int dec_started;
    /* what the loop has said */
    int said_stall, said_end, playing_seen;
    uint32_t stall_since;
    uint32_t said_time_at, said_buf_at;
    char said_buf[256];
};

static struct player *players[MAX_PLAYERS];
static int out_rate = 44100;
static int debug;                       /* WAVEMEDIA_DEBUG: why it waits, on stderr */

#define DBG(...) do { if (debug) { fprintf(stderr, "wavemedia: " __VA_ARGS__); } } while (0)

#define say webaudio_say

/* ---- small things ---- */

static void free_pkt(void *p)
{
    AVPacket *pkt = p;
    av_packet_free(&pkt);
}

static void timed_wait(struct player *p, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&p->cv, &p->mu, &ts);
}

static int stalled(const struct player *p)
{
    return (p->has_v && p->vstall && !p->vdone) || (p->has_a && p->astall && !p->adone && p->rfill == 0);
}

static int sound_clock(const struct player *p)
{
    return p->has_a && !p->adone && p->a_have;
}

/* Where playback is (with mu). */
static int64_t clock_us(const struct player *p)
{
    if (sound_clock(p)) {
        int64_t t = p->ring_end_us - (int64_t)(p->rfill + p->delay) * US / out_rate;
        return t < p->target_us - 50000 ? p->target_us : t;
    }
    int64_t t = p->pos_us;
    if (!p->paused && !stalled(p) && !(p->has_a && !p->adone && !p->a_have))
        t += (int64_t)(uint32_t)(now_ms() - p->clk_at) * 1000;
    return t;
}

/* Fix the wall clock where it is before something that changes how it runs (with mu). */
static void rebase(struct player *p)
{
    p->pos_us = clock_us(p);
    p->clk_at = now_ms();
}

static void set_vstall(struct player *p, int on)
{
    if (p->vstall != on) {
        DBG("%d: picture %s at %lld ms\n", p->id, on ? "waits" : "goes on", (long long)(clock_us(p) / 1000));
        rebase(p);
        p->vstall = on;
    }
}

static void set_astall(struct player *p, int on)
{
    if (p->astall != on) {
        DBG("%d: sound %s at %lld ms (ring %d)\n", p->id, on ? "waits" : "goes on", (long long)(clock_us(p) / 1000), p->rfill);
        rebase(p);
        p->astall = on;
    }
}

/* A codec setup for a stream, the same as the last if it is the same. Its index, or -1. */
static int add_cfg(struct cfgs *c, const AVCodecParameters *par)
{
    if (c->n) {
        const AVCodecParameters *last = c->p[c->n - 1];
        if (last->codec_id == par->codec_id && last->width == par->width && last->height == par->height &&
            last->extradata_size == par->extradata_size && last->sample_rate == par->sample_rate &&
            (!par->extradata_size || !memcmp(last->extradata, par->extradata, (size_t)par->extradata_size)))
            return c->n - 1;
    }
    if (c->n == MAX_CFG)
        return c->n - 1;                /* a stream that changes this often: try the last */
    AVCodecParameters *copy = avcodec_parameters_alloc();
    if (!copy || avcodec_parameters_copy(copy, par) < 0) {
        avcodec_parameters_free(&copy);
        return c->n ? c->n - 1 : -1;
    }
    c->p[c->n++] = copy;
    return c->n - 1;
}

static AVCodecContext *open_decoder(const AVCodecParameters *par, int threads)
{
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!ctx)
        return NULL;
    if (avcodec_parameters_to_context(ctx, par) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    ctx->pkt_timebase = US_TB;
    ctx->thread_count = threads;
    if (avcodec_open2(ctx, codec, NULL) < 0)
        avcodec_free_context(&ctx);
    return ctx;
}

/* A packet as the stores keep it: times in microseconds, moved by off. */
static AVPacket *own_packet(AVPacket *pkt, AVRational tb, int64_t off, int64_t *pts, int64_t *dts, int64_t *dur)
{
    AVPacket *own = av_packet_alloc();
    if (!own) {
        av_packet_unref(pkt);
        return NULL;
    }
    av_packet_move_ref(own, pkt);
    int64_t p = own->pts, d = own->dts;
    if (p == AV_NOPTS_VALUE)
        p = d;
    if (d == AV_NOPTS_VALUE)
        d = p;
    if (p == AV_NOPTS_VALUE) {
        av_packet_free(&own);
        return NULL;
    }
    *pts = av_rescale_q(p, tb, US_TB) + off;
    *dts = av_rescale_q(d, tb, US_TB) + off;
    *dur = own->duration > 0 ? av_rescale_q(own->duration, tb, US_TB) : 0;
    own->pts = *pts;
    own->dts = *dts;
    own->duration = *dur;
    own->time_base = US_TB;
    return own;
}

static int find_index(int id)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (players[i] && players[i]->id == id)
            return i;
    return -1;
}

static struct player *find_player(int id)
{
    int i = find_index(id);
    return i < 0 ? NULL : players[i];
}

static struct sbuf *find_sb(int id)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
        for (int k = 0; players[i] && k < players[i]->nsb; k++)
            if (players[i]->sb[k]->id == id)
                return players[i]->sb[k];
    return NULL;
}

/* ---- frames to Wave ---- */

/* Shared memory for frames of up to w x h (with mu released: the video thread's). */
static int frame_memory(struct player *p, int w, int h)
{
    if (p->shm && p->shm_w == w && p->shm_h == h)
        return 0;
    int id = shmget(IPC_PRIVATE, (size_t)w * (size_t)h * 4 * NBUF, IPC_CREAT | 0600);
    void *m = id >= 0 ? shmat(id, NULL, 0) : (void *)-1;
    if (m == (void *)-1) {
        if (id >= 0)
            shmctl(id, IPC_RMID, NULL);
        return -1;
    }
    pthread_mutex_lock(&p->mu);
    int old = p->shmid;
    uint8_t *old_m = p->shm;
    p->shmid = id;
    p->shm = m;
    p->shm_w = w;
    p->shm_h = h;
    p->fidx = 0;
    pthread_mutex_unlock(&p->mu);
    if (old_m) {                        /* Wave keeps what it has mapped of it while it needs it */
        shmctl(old, IPC_RMID, NULL);
        shmdt(old_m);
    }
    say("vbuf %d %d %d %d %d\n", p->id, id, w, h, NBUF);
    return 0;
}

/* ---- the picture ---- */

struct vout {
    struct SwsContext *sws;
    int sw, sh, sf, dw, dh;
    int dropped;
};

/* Fit w x h (a sample aspect of sar) in bw x bh. */
static void fit(int w, int h, AVRational sar, int bw, int bh, int *ow, int *oh)
{
    if (sar.num > 0 && sar.den > 0)
        w = (int)((int64_t)w * sar.num / sar.den);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    *ow = bw;
    *oh = (int)((int64_t)bw * h / w);
    if (*oh > bh) {
        *oh = bh;
        *ow = (int)((int64_t)bh * w / h);
    }
    *ow &= ~1;
    *oh &= ~1;
    if (*ow < 2)
        *ow = 2;
    if (*oh < 2)
        *oh = 2;
}

/* A decoded frame: wait for its time, then show it (or drop it if it is late). */
static void show_frame(struct player *p, AVFrame *fr, int serial, int64_t skip_to, struct vout *vo)
{
    int64_t pts = fr->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE)
        pts = fr->pts;
    pthread_mutex_lock(&p->mu);
    if (pts == AV_NOPTS_VALUE || pts < p->target_us - 1000 || pts <= skip_to) {
        pthread_mutex_unlock(&p->mu);   /* before where it was sought to */
        return;
    }
    while (!p->quit && serial == p->serial && !p->show_one) {
        if (p->paused || stalled(p)) {
            pthread_cond_wait(&p->cv, &p->mu);
            continue;
        }
        int64_t ahead = pts - clock_us(p);
        if (ahead <= 2000)
            break;
        pthread_mutex_unlock(&p->mu);
        msleep((unsigned)(ahead > 20000 ? 20 : ahead / 1000 + 1));
        pthread_mutex_lock(&p->mu);
    }
    if (p->quit || serial != p->serial) {
        pthread_mutex_unlock(&p->mu);
        return;
    }
    int first = p->show_one;
    p->show_one = 0;
    int late = !first && clock_us(p) - pts > LATE_US;
    int bw = p->box_w, bh = p->box_h;
    pthread_mutex_unlock(&p->mu);
    if (late && vo->dropped < 8) {
        vo->dropped++;
        return;
    }
    vo->dropped = 0;
    if (bw < 2 || bh < 2) {             /* no box yet: the picture's own size, within reason */
        bw = fr->width > 1280 ? 1280 : fr->width;
        bh = fr->height > 720 ? 720 : fr->height;
    }
    bw = bw > MAX_W ? MAX_W : bw;
    bh = bh > MAX_H ? MAX_H : bh;
    int ow, oh;
    fit(fr->width, fr->height, fr->sample_aspect_ratio, bw & ~1, bh & ~1, &ow, &oh);
    if (frame_memory(p, bw & ~1, bh & ~1) < 0)
        return;
    if (!vo->sws || vo->sw != fr->width || vo->sh != fr->height || vo->sf != fr->format || vo->dw != ow || vo->dh != oh) {
        sws_freeContext(vo->sws);
        vo->sws = sws_getContext(fr->width, fr->height, fr->format, ow, oh, AV_PIX_FMT_BGR0, SWS_FAST_BILINEAR,
                                 NULL, NULL, NULL);
        vo->sw = fr->width;
        vo->sh = fr->height;
        vo->sf = fr->format;
        vo->dw = ow;
        vo->dh = oh;
    }
    if (!vo->sws)
        return;
    pthread_mutex_lock(&p->mu);
    int idx = (p->fidx + 1) % NBUF, shmid = p->shmid;
    uint8_t *dst0 = p->shm + (size_t)idx * (size_t)p->shm_w * (size_t)p->shm_h * 4;
    pthread_mutex_unlock(&p->mu);
    uint8_t *dst[4] = { dst0, NULL, NULL, NULL };
    int stride[4] = { ow * 4, 0, 0, 0 };
    sws_scale(vo->sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, stride);
    pthread_mutex_lock(&p->mu);
    if (serial == p->serial && shmid == p->shmid) {
        p->fidx = idx;
        say("vframe %d %d %d %d %d %lld\n", p->id, shmid, idx, ow, oh, (long long)(pts / 1000));
        if (!p->frames_shown++)
            say("vfirst %d\n", p->id);
        if (p->seek_pending) {
            p->seek_pending = 0;
            say("vseeked %d %lld\n", p->id, (long long)(p->target_us / 1000));
        }
    }
    pthread_mutex_unlock(&p->mu);
}

static void *video_main(void *arg)
{
    struct player *p = arg;
    AVCodecContext *dec = NULL;
    AVFrame *fr = av_frame_alloc();
    struct vout vo = { 0 };
    struct scursor cur = { 0 };
    int dec_cfg = -1, serial = -1, placed = 0, drained = 0;
    int64_t seek_t = 0, skip_to = INT64_MIN;
    pthread_mutex_lock(&p->mu);
    while (!p->quit) {
        if (serial != p->serial) {
            serial = p->serial;
            placed = drained = 0;
            seek_t = p->target_us;
            skip_to = INT64_MIN;
            p->vdone = 0;
            if (dec) {
                pthread_mutex_unlock(&p->mu);
                avcodec_flush_buffers(dec);
                pthread_mutex_lock(&p->mu);
            }
            continue;
        }
        if (!p->has_v || !p->vc.n) {
            pthread_cond_wait(&p->cv, &p->mu);
            continue;
        }
        if (!placed) {
            if (store_seek(&p->vs, &cur, seek_t, TOL) == 0) {
                placed = 1;
            } else {
                if (p->eos && !store_has(&p->vs, seek_t, TOL) && (p->vs.ng == 0 || p->vs.g[p->vs.ng - 1].end <= seek_t + TOL))
                    p->vdone = 1;       /* sought past the end */
                else
                    set_vstall(p, 1);
                timed_wait(p, 100);
                continue;
            }
        }
        const struct sframe *f;
        int r = store_next(&p->vs, &cur, TOL, &f);
        if (r == SC_LOST) {             /* what it was on was removed: find the place again */
            seek_t = skip_to = cur.last_pts;
            placed = 0;
            continue;
        }
        if (r == SC_WAIT) {
            if (p->eos && !drained && dec) {
                drained = 1;
                pthread_mutex_unlock(&p->mu);
                if (avcodec_send_packet(dec, NULL) >= 0)
                    while (avcodec_receive_frame(dec, fr) >= 0) {
                        show_frame(p, fr, serial, skip_to, &vo);
                        av_frame_unref(fr);
                    }
                avcodec_flush_buffers(dec);
                pthread_mutex_lock(&p->mu);
                continue;
            }
            if (p->eos) {
                if (!p->vdone) {
                    rebase(p);
                    p->vdone = 1;
                    pthread_cond_broadcast(&p->cv);
                }
            } else {
                set_vstall(p, 1);
            }
            timed_wait(p, 100);
            continue;
        }
        if (p->vstall) {
            set_vstall(p, 0);
            pthread_cond_broadcast(&p->cv);
        }
        AVPacket *pkt = av_packet_clone(f->pkt);
        int cfg = f->cfg;
        const AVCodecParameters *par = cfg >= 0 && cfg < p->vc.n ? p->vc.p[cfg] : NULL;
        pthread_mutex_unlock(&p->mu);
        if (cfg != dec_cfg && par) {
            avcodec_free_context(&dec);
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            dec = open_decoder(par, n > 4 ? 4 : n > 0 ? (int)n : 1);
            dec_cfg = cfg;
        }
        if (dec && pkt && avcodec_send_packet(dec, pkt) >= 0)
            while (avcodec_receive_frame(dec, fr) >= 0) {
                show_frame(p, fr, serial, skip_to, &vo);
                av_frame_unref(fr);
            }
        av_packet_free(&pkt);
        pthread_mutex_lock(&p->mu);
    }
    pthread_mutex_unlock(&p->mu);
    avcodec_free_context(&dec);
    av_frame_free(&fr);
    sws_freeContext(vo.sws);
    return NULL;
}

/* ---- the sound ---- */

static void ring_flush(struct player *p)
{
    p->rhead = p->rfill = 0;
    p->a_have = 0;
}

/* Put n stereo frames that end at end_us into the ring, waiting for room. 0 if a seek or the end
 * came first. */
static int ring_put(struct player *p, const int16_t *s, int n, int64_t pts, int serial)
{
    pthread_mutex_lock(&p->mu);
    int done = 0;
    while (done < n) {
        if (p->quit || serial != p->serial) {
            pthread_mutex_unlock(&p->mu);
            return 0;
        }
        if (p->rfill == RING) {
            pthread_cond_wait(&p->cv, &p->mu);
            continue;
        }
        int at = (p->rhead + p->rfill) % RING, k = n - done;
        if (k > RING - p->rfill)
            k = RING - p->rfill;
        if (k > RING - at)
            k = RING - at;
        memcpy(&p->ring[at * 2], s + done * 2, (size_t)k * 4);
        p->rfill += k;
        done += k;
        if (!p->a_have) {
            rebase(p);
            p->a_have = 1;
            if (p->seek_pending && !p->has_v) {
                p->seek_pending = 0;
                say("vseeked %d %lld\n", p->id, (long long)(p->target_us / 1000));
            }
        }
        p->ring_end_us = pts + (int64_t)done * US / out_rate;
    }
    pthread_mutex_unlock(&p->mu);
    return 1;
}

static void *audio_main(void *arg)
{
    struct player *p = arg;
    AVCodecContext *dec = NULL;
    AVFrame *fr = av_frame_alloc();
    SwrContext *swr = NULL;
    AVChannelLayout stereo;
    av_channel_layout_default(&stereo, 2);
    int16_t *out = NULL;
    int out_cap = 0, swr_rate = 0, swr_in_rate = 0, swr_fmt = -1, swr_ch = 0;
    struct scursor cur = { 0 };
    int dec_cfg = -1, serial = -1, placed = 0, drained = 0;
    int64_t seek_t = 0, skip_to = INT64_MIN;
    pthread_mutex_lock(&p->mu);
    while (!p->quit) {
        if (serial != p->serial) {
            serial = p->serial;
            placed = drained = 0;
            seek_t = p->target_us;
            skip_to = INT64_MIN;
            p->adone = 0;
            ring_flush(p);
            if (dec) {
                pthread_mutex_unlock(&p->mu);
                avcodec_flush_buffers(dec);
                swr_free(&swr);
                pthread_mutex_lock(&p->mu);
            }
            continue;
        }
        if (!p->has_a || !p->ac.n) {
            pthread_cond_wait(&p->cv, &p->mu);
            continue;
        }
        if (!placed) {
            if (store_seek(&p->as, &cur, seek_t, TOL) == 0) {
                placed = 1;
            } else {
                if (p->eos && (p->as.ng == 0 || p->as.g[p->as.ng - 1].end <= seek_t + TOL)) {
                    if (!p->adone) {
                        rebase(p);
                        p->adone = 1;
                    }
                } else {
                    set_astall(p, 1);
                }
                timed_wait(p, 100);
                continue;
            }
        }
        const struct sframe *f;
        int r = store_next(&p->as, &cur, TOL, &f);
        if (r == SC_LOST) {
            seek_t = skip_to = cur.last_pts;
            placed = 0;
            continue;
        }
        if (r == SC_WAIT) {
            if (p->eos) {
                if (!p->adone && p->rfill == 0) {
                    rebase(p);          /* the wall's clock from here (the picture may go on) */
                    p->adone = 1;
                    pthread_cond_broadcast(&p->cv);
                }
                (void)drained;
            } else {
                set_astall(p, 1);
            }
            timed_wait(p, 50);
            continue;
        }
        set_astall(p, 0);
        AVPacket *pkt = av_packet_clone(f->pkt);
        int cfg = f->cfg;
        int64_t pkt_pts = f->pts;
        const AVCodecParameters *par = cfg >= 0 && cfg < p->ac.n ? p->ac.p[cfg] : NULL;
        int rate = out_rate;
        pthread_mutex_unlock(&p->mu);
        if (cfg != dec_cfg && par) {
            avcodec_free_context(&dec);
            swr_free(&swr);
            dec = open_decoder(par, 1);
            dec_cfg = cfg;
        }
        if (dec && pkt && pkt_pts > skip_to && avcodec_send_packet(dec, pkt) >= 0)
            while (avcodec_receive_frame(dec, fr) >= 0) {
                int64_t pts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : pkt_pts;
                int64_t end = pts + (fr->sample_rate ? (int64_t)fr->nb_samples * US / fr->sample_rate : 0);
                if (end < seek_t || fr->sample_rate <= 0) {
                    av_frame_unref(fr);
                    continue;
                }
                if (!swr || swr_rate != rate || swr_in_rate != fr->sample_rate || swr_fmt != fr->format ||
                    swr_ch != fr->ch_layout.nb_channels) {
                    swr_free(&swr);
                    if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, rate, &fr->ch_layout, fr->format,
                                            fr->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) {
                        swr_free(&swr);
                        av_frame_unref(fr);
                        break;
                    }
                    swr_rate = rate;
                    swr_in_rate = fr->sample_rate;
                    swr_fmt = fr->format;
                    swr_ch = fr->ch_layout.nb_channels;
                }
                int cap = swr_get_out_samples(swr, fr->nb_samples);
                if (cap > out_cap) {
                    free(out);
                    out = malloc((size_t)cap * 4);
                    out_cap = out ? cap : 0;
                }
                int n = out ? swr_convert(swr, (uint8_t **)&out, out_cap, (const uint8_t **)fr->extended_data,
                                           fr->nb_samples) : 0;
                av_frame_unref(fr);
                if (n > 0 && !ring_put(p, out, n, pts, serial))
                    break;
            }
        av_packet_free(&pkt);
        pthread_mutex_lock(&p->mu);
    }
    pthread_mutex_unlock(&p->mu);
    avcodec_free_context(&dec);
    swr_free(&swr);
    free(out);
    av_frame_free(&fr);
    av_channel_layout_uninit(&stereo);
    return NULL;
}

/* ---- a file or an address ---- */

static void *src_main(void *arg)
{
    struct player *p = arg;
    char err[160];
    int r = media_input_open(&p->in, p->url, (const char *const *)p->hdr, p->nhdr, err, sizeof(err));
    pthread_mutex_lock(&p->mu);
    if (r < 0) {
        if (!p->quit)
            say("verror %d %s\n", p->id, err);
        pthread_mutex_unlock(&p->mu);
        return NULL;
    }
    AVFormatContext *fmt = p->in.fmt;
    int vi = -1, ai = -1, vcfg = -1, acfg = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream *st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC) && vi < 0 &&
            avcodec_find_decoder(st->codecpar->codec_id))
            vi = (int)i;
    }
    ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
    if (ai >= 0 && !avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id))
        ai = -1;
    if (vi >= 0) {
        vcfg = add_cfg(&p->vc, fmt->streams[vi]->codecpar);
        p->has_v = vcfg >= 0;
        p->vw = fmt->streams[vi]->codecpar->width;
        p->vh = fmt->streams[vi]->codecpar->height;
    }
    if (ai >= 0) {
        acfg = add_cfg(&p->ac, fmt->streams[ai]->codecpar);
        p->has_a = acfg >= 0;
    }
    if (!p->has_v && !p->has_a) {
        say("verror %d No sound or picture in it can be played.\n", p->id);
        pthread_mutex_unlock(&p->mu);
        return NULL;
    }
    say("vmeta %d %lld %d %d %d\n", p->id, fmt->duration > 0 ? (long long)(fmt->duration / 1000) : -1LL, p->vw, p->vh,
        p->has_a);
    pthread_cond_broadcast(&p->cv);
    AVPacket *pkt = av_packet_alloc();
    while (!p->quit && pkt) {
        if (p->seek_src) {
            p->seek_src = 0;
            int64_t t = p->src_seek_us;
            store_clear(&p->vs);
            store_clear(&p->as);
            pthread_mutex_unlock(&p->mu);
            if (av_seek_frame(fmt, -1, t, AVSEEK_FLAG_BACKWARD) < 0)
                av_seek_frame(fmt, -1, t, AVSEEK_FLAG_ANY);
            pthread_mutex_lock(&p->mu);
            p->src_eof = 0;
            store_reset_append(&p->vs);
            store_reset_append(&p->as);
            continue;
        }
        int64_t now = p->seek_pending ? p->target_us : clock_us(p);
        int64_t va = p->has_v ? store_end_from(&p->vs, now, TOL) - now : AHEAD_US * 2;
        int64_t aa = p->has_a ? store_end_from(&p->as, now, TOL) - now : AHEAD_US * 2;
        if (p->src_eof || (va > AHEAD_US && aa > AHEAD_US) || p->vs.bytes > V_BUDGET || p->as.bytes > A_BUDGET) {
            store_evict_before(&p->vs, now - BEHIND_US, 0);
            store_evict_before(&p->as, now - BEHIND_US, 0);
            timed_wait(p, 200);
            continue;
        }
        int serial = p->serial;
        pthread_mutex_unlock(&p->mu);
        r = av_read_frame(fmt, pkt);
        pthread_mutex_lock(&p->mu);
        if (r < 0) {
            if (r == AVERROR_EXIT || p->quit)
                break;
            if (!p->seek_src) {
                p->src_eof = 1;
                p->eos = 1;
            }
            pthread_cond_broadcast(&p->cv);
            continue;
        }
        if (p->seek_src || (serial != p->serial && p->seek_src)) {
            av_packet_unref(pkt);
            continue;
        }
        int video = pkt->stream_index == vi, audio = pkt->stream_index == ai;
        if (!video && !audio) {
            av_packet_unref(pkt);
            continue;
        }
        int64_t pts, dts, dur;
        AVPacket *own = own_packet(pkt, fmt->streams[pkt->stream_index]->time_base, 0, &pts, &dts, &dur);
        if (own) {
            struct sframe f = { own, pts, dts, dur, !!(own->flags & AV_PKT_FLAG_KEY), video ? vcfg : acfg, (size_t)own->size };
            store_add(video ? &p->vs : &p->as, &f);
            pthread_cond_broadcast(&p->cv);
        }
    }
    av_packet_free(&pkt);
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

/* ---- SourceBuffers ---- */

/* a (*na spans, room for cap) becomes its intersection with b: up to *na + nb - 1 spans */
static void intersect(int64_t *a, int *na, const int64_t *b, int nb, int cap)
{
    int64_t out[64];
    int n = 0;
    if (cap > 32)
        cap = 32;
    for (int i = 0, j = 0; i < *na && j < nb && n < cap;) {
        int64_t s = a[2 * i] > b[2 * j] ? a[2 * i] : b[2 * j];
        int64_t e = a[2 * i + 1] < b[2 * j + 1] ? a[2 * i + 1] : b[2 * j + 1];
        if (s < e) {
            out[2 * n] = s;
            out[2 * n + 1] = e;
            n++;
        }
        if (a[2 * i + 1] < b[2 * j + 1])
            i++;
        else
            j++;
    }
    memcpy(a, out, sizeof(int64_t) * 2 * (size_t)n);
    *na = n;
}

/* Answer an append or remove: the SourceBuffer's buffered spans (with mu). */
static void sb_answer(struct sbuf *sb, int seq)
{
    struct player *p = sb->pl;
    int64_t r[64], o[64];
    int n = 0;
    if (sb->feeds_v)
        n = store_ranges(&p->vs, TOL, r, 32);
    if (sb->feeds_a) {
        int m = store_ranges(&p->as, TOL, o, 32);
        if (sb->feeds_v)
            intersect(r, &n, o, m, 32);
        else {
            memcpy(r, o, sizeof(int64_t) * 2 * (size_t)m);
            n = m;
        }
    }
    char line[1024];
    int len = snprintf(line, sizeof(line), "sbdone %d %d %d", sb->id, seq, n);
    for (int i = 0; i < n && len < (int)sizeof(line) - 48; i++)
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %lld %lld", (long long)r[2 * i], (long long)r[2 * i + 1]);
    say("%s\n", line);
}

/* The demuxer has used up what was appended: the append is done (with mu). */
static void sb_idle(struct sbuf *sb)
{
    if (sb->busy) {
        sb->busy = 0;
        sb_answer(sb, sb->seq);
    }
}

static int sb_read(void *opaque, uint8_t *buf, int n)
{
    struct sbuf *sb = opaque;
    struct player *p = sb->pl;
    pthread_mutex_lock(&p->mu);
    struct group *g = sb->head;
    for (;;) {
        if (sb->quit || p->quit || !g) {
            pthread_mutex_unlock(&p->mu);
            return AVERROR_EXIT;
        }
        if (g->rd < g->len)
            break;
        if (g->closed) {
            pthread_mutex_unlock(&p->mu);
            return AVERROR_EOF;
        }
        sb_idle(sb);
        pthread_cond_wait(&p->cv, &p->mu);
    }
    size_t k = g->len - g->rd < (size_t)n ? g->len - g->rd : (size_t)n;
    memcpy(buf, g->d + g->rd, k);
    g->rd += k;
    pthread_mutex_unlock(&p->mu);
    return (int)k;
}

static void pop_group(struct sbuf *sb)
{
    struct group *g = sb->head;
    if (g) {
        sb->head = g->next;
        free(g->d);
        free(g);
    }
}

/* Say what an initialization segment holds (with mu). */
static void sb_init_said(struct sbuf *sb, AVFormatContext *fmt)
{
    int v = 0, a = 0, w = 0, h = 0;
    char codecs[96] = "";
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVCodecParameters *par = fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && !v) {
            v = 1;
            w = par->width;
            h = par->height;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && !a) {
            a = 1;
        } else {
            continue;
        }
        size_t l = strlen(codecs);
        snprintf(codecs + l, sizeof(codecs) - l, "%s%s", l ? "," : "", avcodec_get_name(par->codec_id));
    }
    say("sbinit %d %d %d %d %d %s\n", sb->id, v, a, w, h, codecs[0] ? codecs : "-");
}

static void *sb_main(void *arg)
{
    struct sbuf *sb = arg;
    struct player *p = sb->pl;
    const AVInputFormat *ifmt = av_find_input_format(sb->kind == SEG_MP4 ? "mov" : "matroska");
    AVPacket *pkt = av_packet_alloc();
    pthread_mutex_lock(&p->mu);
    while (!sb->quit && !p->quit && pkt) {
        struct group *g = sb->head;
        if (!g || (g->rd == g->len && !g->closed)) {
            sb_idle(sb);
            pthread_cond_wait(&p->cv, &p->mu);
            continue;
        }
        if (g->rd == g->len && g->closed) {
            pop_group(sb);
            continue;
        }
        pthread_mutex_unlock(&p->mu);
        AVFormatContext *fmt = avformat_alloc_context();
        uint8_t *buf = av_malloc(IO_BUF);
        AVIOContext *pb = buf ? avio_alloc_context(buf, IO_BUF, 0, sb, sb_read, NULL, NULL) : NULL;
        int r = -1;
        if (fmt && pb) {
            pb->seekable = 0;
            fmt->pb = pb;
            fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
            r = avformat_open_input(&fmt, "", ifmt, NULL);  /* frees fmt when it fails */
        } else {
            avformat_free_context(fmt);
            av_free(buf);
        }
        pthread_mutex_lock(&p->mu);
        if (r < 0) {
            if (pb) {
                av_freep(&pb->buffer);
                avio_context_free(&pb);
            }
            if (sb->quit || p->quit)
                break;
            say("sberr %d %d\n", sb->id, sb->seq);
            sb->busy = 0;
            pop_group(sb);
            sb->fresh = 1;
            continue;
        }
        /* its tracks: the first picture and sound, if no other SourceBuffer has them */
        int kind[64] = { 0 }, cfg[64], got_v = 0, got_a = 0;
        for (unsigned i = 0; i < fmt->nb_streams && i < 64; i++) {
            const AVCodecParameters *par = fmt->streams[i]->codecpar;
            int taken_v = 0, taken_a = 0;
            for (int k = 0; k < p->nsb; k++) {
                taken_v |= p->sb[k] != sb && p->sb[k]->feeds_v;
                taken_a |= p->sb[k] != sb && p->sb[k]->feeds_a;
            }
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && !taken_v && !got_v) {
                kind[i] = 1;
                got_v = 1;
                cfg[i] = add_cfg(&p->vc, par);
                sb->feeds_v = 1;
                p->has_v = 1;
                p->vw = par->width;
                p->vh = par->height;
                store_reset_append(&p->vs);
            } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && !taken_a && !got_a) {
                kind[i] = 2;
                got_a = 1;
                cfg[i] = add_cfg(&p->ac, par);
                sb->feeds_a = 1;
                p->has_a = 1;
                store_reset_append(&p->as);
            }
        }
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
        while ((r = av_read_frame(fmt, pkt)) >= 0) {
            int si = pkt->stream_index;
            if (si < 0 || si >= 64 || !kind[si] || cfg[si] < 0) {
                av_packet_unref(pkt);
                continue;
            }
            pthread_mutex_lock(&p->mu);
            int64_t off = sb->offset, pts, dts, dur;
            pthread_mutex_unlock(&p->mu);
            AVPacket *own = own_packet(pkt, fmt->streams[si]->time_base, off, &pts, &dts, &dur);
            if (!own)
                continue;
            struct sframe f = { own, pts, dts, dur, !!(own->flags & AV_PKT_FLAG_KEY), cfg[si], (size_t)own->size };
            pthread_mutex_lock(&p->mu);
            struct store *st = kind[si] == 1 ? &p->vs : &p->as;
            store_add(st, &f);
            if (st->bytes > (kind[si] == 1 ? V_BUDGET : A_BUDGET))   /* too much: what has been played goes */
                store_evict_before(st, clock_us(p) - 2 * US, kind[si] == 1 ? V_BUDGET : A_BUDGET);
            pthread_cond_broadcast(&p->cv);
            pthread_mutex_unlock(&p->mu);
        }
        avformat_close_input(&fmt);
        av_freep(&pb->buffer);
        avio_context_free(&pb);
        pthread_mutex_lock(&p->mu);
        if (sb->quit || p->quit)
            break;
        if (r != AVERROR_EOF && r != AVERROR_EXIT) {    /* bytes it could not make sense of */
            say("sberr %d %d\n", sb->id, sb->seq);
            sb->busy = 0;
            sb->fresh = 1;
        }
        if (sb->head == g)
            pop_group(sb);
    }
    av_packet_free(&pkt);
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

/* Probe an initialization segment on its own, to say at once what it holds. */
struct probe_io {
    const uint8_t *d;
    size_t len, rd;
};

static int probe_read(void *opaque, uint8_t *buf, int n)
{
    struct probe_io *io = opaque;
    size_t k = io->len - io->rd < (size_t)n ? io->len - io->rd : (size_t)n;
    if (!k)
        return AVERROR_EOF;
    memcpy(buf, io->d + io->rd, k);
    io->rd += k;
    return (int)k;
}

static void probe_init(struct sbuf *sb, const uint8_t *d, size_t len)
{
    /* Matroska reads its header up to the first Cluster: after an initialization segment alone,
     * an empty one (of unknown size) is put for it to stop at */
    static const uint8_t cluster[] = { 0x1F, 0x43, 0xB6, 0x75, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t *copy = NULL;
    if (sb->kind == SEG_WEBM && seg_init_length(d, len, SEG_WEBM) == len && (copy = malloc(len + sizeof(cluster)))) {
        memcpy(copy, d, len);
        memcpy(copy + len, cluster, sizeof(cluster));
        d = copy;
        len += sizeof(cluster);
    }
    struct probe_io io = { d, len, 0 };
    AVFormatContext *fmt = avformat_alloc_context();
    uint8_t *buf = av_malloc(IO_BUF);
    AVIOContext *pb = buf ? avio_alloc_context(buf, IO_BUF, 0, &io, probe_read, NULL, NULL) : NULL;
    if (!fmt || !pb) {
        avformat_free_context(fmt);
        av_free(buf);
        free(copy);
        return;
    }
    pb->seekable = 0;
    fmt->pb = pb;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    const AVInputFormat *ifmt = av_find_input_format(sb->kind == SEG_MP4 ? "mov" : "matroska");
    if (avformat_open_input(&fmt, "", ifmt, NULL) == 0) {
        pthread_mutex_lock(&sb->pl->mu);
        sb_init_said(sb, fmt);
        pthread_mutex_unlock(&sb->pl->mu);
        avformat_close_input(&fmt);
    } else {
        say("sbinit %d 0 0 0 0 -\n", sb->id);
    }
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    free(copy);
}

static struct group *new_group(void)
{
    return calloc(1, sizeof(struct group));
}

static int group_add(struct group *g, const uint8_t *d, size_t n)
{
    if (g->len + n > g->cap) {
        size_t cap = g->cap ? g->cap : 65536;
        while (cap < g->len + n)
            cap *= 2;
        uint8_t *more = realloc(g->d, cap);
        if (!more)
            return -1;
        g->d = more;
        g->cap = cap;
    }
    memcpy(g->d + g->len, d, n);
    g->len += n;
    return 0;
}

/* The bytes used up already are let go, so that a long stream does not grow without end. */
static void group_compact(struct group *g)
{
    if (g->rd > (4u << 20) && g->rd * 2 > g->len) {
        memmove(g->d, g->d + g->rd, g->len - g->rd);
        g->len -= g->rd;
        g->rd = 0;
    }
}

/* Text from a command line that is used as a name, an address or a header: no control characters
 * (a line break in it would be another command). */
static int clean_text(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7F)
            return 0;
    return 1;
}

/* What Wave appends is in a file it made with mkstemp("/tmp/wave-mse-XXXXXX") for the engine to
 * read and delete: only such a name, a plain file of this user's, is opened (and then deleted). */
static int mse_file(const char *path)
{
    static const char prefix[] = "/tmp/wave-mse-";
    size_t n = sizeof(prefix) - 1;
    if (strncmp(path, prefix, n) || strlen(path + n) != 6)
        return 0;
    for (const char *c = path + n; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')))
            return 0;
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == (uint32_t)getuid();
}

static unsigned char *read_whole(const char *path, size_t *len)
{
    *len = 0;
    if (!mse_file(path))
        return NULL;                    /* not a file of Wave's: neither read nor deleted */
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    unsigned char *d = NULL;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
        (d = malloc((size_t)st.st_size + 1))) {
        while (*len < (size_t)st.st_size) {
            int r = (int)read(fd, d + *len, (size_t)st.st_size - *len);
            if (r <= 0)
                break;
            *len += (size_t)r;
        }
    }
    if (fd >= 0)
        close(fd);
    unlink(path);
    return d;
}

static void sb_append(struct sbuf *sb, int seq, const char *path)
{
    struct player *p = sb->pl;
    size_t len;
    unsigned char *d = read_whole(path, &len);
    if (!d) {
        say("sberr %d %d\n", sb->id, seq);
        return;
    }
    size_t init = seg_init_length(d, len, sb->kind);
    if (init)
        probe_init(sb, d, len);         /* "sbinit" before the append is answered */
    pthread_mutex_lock(&p->mu);
    sb->seq = seq;
    struct group *tail = sb->head;
    while (tail && tail->next)
        tail = tail->next;
    int ok = 0;
    if (init) {
        uint8_t *copy = malloc(init);
        if (copy) {
            memcpy(copy, d, init);
            free(sb->init);
            sb->init = copy;
            sb->init_len = init;
        }
        if (tail)
            tail->closed = 1;           /* a new initialization segment: a new demuxer */
        struct group *g = new_group();
        if (g && group_add(g, d, len) == 0) {
            if (tail)
                tail->next = g;
            else
                sb->head = g;
            ok = 1;
        } else {
            free(g);
        }
        sb->fresh = 0;
    } else if (!tail || tail->closed || sb->fresh) {
        if (tail)
            tail->closed = 1;
        struct group *g = sb->init ? new_group() : NULL;
        if (g && group_add(g, sb->init, sb->init_len) == 0 && group_add(g, d, len) == 0) {
            if (tail)
                tail->next = g;
            else
                sb->head = g;
            ok = 1;
            sb->fresh = 0;
        } else if (g) {
            free(g->d);
            free(g);
        }
    } else {
        group_compact(tail);
        ok = group_add(tail, d, len) == 0;
    }
    free(d);
    if (ok) {
        sb->busy = 1;
        pthread_cond_broadcast(&p->cv);
    } else {
        say("sberr %d %d\n", sb->id, seq);
    }
    pthread_mutex_unlock(&p->mu);
}

static void sb_abort(struct sbuf *sb)
{
    struct player *p = sb->pl;
    pthread_mutex_lock(&p->mu);
    for (struct group *g = sb->head; g; g = g->next) {
        g->closed = 1;                  /* what is not parsed yet is thrown away */
        g->rd = g->len;
    }
    sb->fresh = 1;
    sb->busy = 0;
    if (sb->feeds_v)
        store_reset_append(&p->vs);
    if (sb->feeds_a)
        store_reset_append(&p->as);
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

static void sb_free(struct sbuf *sb)
{
    struct player *p = sb->pl;
    pthread_mutex_lock(&p->mu);
    sb->quit = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    if (sb->started)
        pthread_join(sb->th, NULL);
    while (sb->head)
        pop_group(sb);
    free(sb->init);
    free(sb);
}

/* ---- players ---- */

static void start_decoders(struct player *p)
{
    if (p->dec_started)
        return;
    pthread_create(&p->tv, NULL, video_main, p);
    pthread_create(&p->ta, NULL, audio_main, p);
    p->dec_started = 1;
}

static struct player *player_new(int id)
{
    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS && slot < 0; i++)
        if (!players[i])
            slot = i;
    struct player *p = slot >= 0 ? calloc(1, sizeof(*p)) : NULL;
    if (!p) {
        say("verror %d Too many videos are open.\n", id);
        return NULL;
    }
    p->id = id;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    store_init(&p->vs, 0, free_pkt);
    store_init(&p->as, 1, free_pkt);
    p->paused = 1;
    p->show_one = 1;
    p->volume = 1000;
    p->shmid = -1;
    p->clk_at = now_ms();
    players[slot] = p;
    return p;
}

static void player_free(struct player *p)
{
    pthread_mutex_lock(&p->mu);
    p->quit = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    media_input_abort(&p->in);
    for (int i = 0; i < p->nsb; i++)
        sb_free(p->sb[i]);
    if (p->src_started)
        pthread_join(p->tsrc, NULL);
    if (p->dec_started) {
        pthread_join(p->tv, NULL);
        pthread_join(p->ta, NULL);
    }
    media_input_close(&p->in);
    store_clear(&p->vs);
    store_clear(&p->as);
    for (int i = 0; i < p->vc.n; i++)
        avcodec_parameters_free(&p->vc.p[i]);
    for (int i = 0; i < p->ac.n; i++)
        avcodec_parameters_free(&p->ac.p[i]);
    for (int i = 0; i < p->nhdr; i++)
        free(p->hdr[i]);
    if (p->shm) {
        shmctl(p->shmid, IPC_RMID, NULL);
        shmdt(p->shm);
    }
    free(p->url);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
    free(p);
}

static void player_close(int id)
{
    int i = find_index(id);
    if (i >= 0) {
        struct player *p = players[i];
        players[i] = NULL;
        player_free(p);
    }
}

/* Seek (with mu). */
static void seek_locked(struct player *p, int64_t t)
{
    if (t < 0)
        t = 0;
    DBG("%d: seek to %lld ms\n", p->id, (long long)(t / 1000));
    p->serial++;
    p->target_us = t;
    p->pos_us = t;
    p->clk_at = now_ms();
    p->show_one = 1;
    p->seek_pending = 1;
    p->vdone = p->adone = 0;
    p->vstall = p->astall = 0;
    p->said_end = 0;
    ring_flush(p);
    if (!p->mse && p->src_started) {
        int need = (p->has_v && !store_has(&p->vs, t, TOL)) || (p->has_a && !store_has(&p->as, t, TOL));
        if (need) {                     /* not downloaded: from there */
            p->seek_src = 1;
            p->src_seek_us = t;
            p->src_eof = 0;
            p->eos = 0;
        }
    }
    pthread_cond_broadcast(&p->cv);
}

/* ---- the loop's hooks ---- */

static int ext_command(char *line)
{
    char op[16];
    int id = 0, n = 0;
    if (sscanf(line, "%15s %d %n", op, &id, &n) < 2)
        return 0;
    const char *rest = n ? line + n : "";
    if (op[0] != 'v' && strncmp(op, "sb", 2) && strncmp(op, "mse", 3))
        return 0;
    if (!clean_text(rest))
        return 1;                       /* a name or address with control characters: ignored */
    if (!strcmp(op, "vopen") || !strcmp(op, "vmse")) {
        struct player *keep = find_player(id);
        char *hdr[MAX_HDR];
        int nhdr = 0;
        if (keep) {                     /* headers given before it opens */
            nhdr = keep->nhdr;
            memcpy(hdr, keep->hdr, sizeof(hdr));
            keep->nhdr = 0;
            player_close(id);
        }
        struct player *p = player_new(id);
        if (!p) {
            for (int i = 0; i < nhdr; i++)
                free(hdr[i]);
            return 1;
        }
        memcpy(p->hdr, hdr, sizeof(hdr));
        p->nhdr = nhdr;
        start_decoders(p);
        if (!strcmp(op, "vmse")) {
            p->mse = 1;
        } else {
            p->url = strdup(rest);
            if (p->url && pthread_create(&p->tsrc, NULL, src_main, p) == 0)
                p->src_started = 1;
        }
        return 1;
    }
    if (!strcmp(op, "vhdr")) {
        struct player *p = find_player(id);
        if (!p && (p = player_new(id)))
            p->mse = -1;                /* a placeholder for its headers */
        if (p && p->nhdr < MAX_HDR)
            p->hdr[p->nhdr++] = strdup(rest);
        return 1;
    }
    if (!strncmp(op, "sb", 2)) {
        if (!strcmp(op, "sbadd")) {
            int sbid = 0, m = 0;
            struct player *p = find_player(id);
            if (sscanf(rest, "%d %n", &sbid, &m) < 1 || !p || p->nsb == MAX_SB)
                return 1;
            struct sbuf *sb = calloc(1, sizeof(*sb));
            if (!sb)
                return 1;
            sb->id = sbid;
            sb->pl = p;
            sb->kind = seg_kind(rest + m);
            pthread_mutex_lock(&p->mu);
            p->sb[p->nsb++] = sb;
            pthread_mutex_unlock(&p->mu);
            if (pthread_create(&sb->th, NULL, sb_main, sb) == 0)
                sb->started = 1;
            return 1;
        }
        struct sbuf *sb = find_sb(id);
        if (!sb)
            return 1;
        struct player *p = sb->pl;
        if (!strcmp(op, "sbappend")) {
            int seq = 0, m = 0;
            if (sscanf(rest, "%d %n", &seq, &m) >= 1)
                sb_append(sb, seq, rest + m);
        } else if (!strcmp(op, "sbremove")) {
            int seq = 0;
            long long s = 0, e = 0;
            if (sscanf(rest, "%d %lld %lld", &seq, &s, &e) == 3) {
                pthread_mutex_lock(&p->mu);
                if (sb->feeds_v)
                    store_remove(&p->vs, s, e);
                if (sb->feeds_a)
                    store_remove(&p->as, s, e);
                pthread_cond_broadcast(&p->cv);
                sb_answer(sb, seq);
                pthread_mutex_unlock(&p->mu);
            }
        } else if (!strcmp(op, "sbabort")) {
            sb_abort(sb);
        } else if (!strcmp(op, "sboffset")) {
            pthread_mutex_lock(&p->mu);
            sb->offset = atoll(rest);
            pthread_mutex_unlock(&p->mu);
        } else if (!strcmp(op, "sbdel")) {
            pthread_mutex_lock(&p->mu);
            for (int i = 0; i < p->nsb; i++)
                if (p->sb[i] == sb) {
                    memmove(&p->sb[i], &p->sb[i + 1], sizeof(p->sb[0]) * (size_t)(p->nsb - i - 1));
                    p->nsb--;
                    break;
                }
            pthread_mutex_unlock(&p->mu);
            sb_free(sb);
        }
        return 1;
    }
    struct player *p = find_player(id);
    if (!strcmp(op, "vclose")) {
        player_close(id);
        return 1;
    }
    if (!p)
        return 1;
    int v = atoi(rest);
    pthread_mutex_lock(&p->mu);
    if (!strcmp(op, "mseend")) {
        p->eos = v != 0;
        pthread_cond_broadcast(&p->cv);
    } else if (!strcmp(op, "vsize")) {
        int w = 0, h = 0;
        sscanf(rest, "%d %d", &w, &h);
        p->box_w = w > 0 ? w : 0;
        p->box_h = h > 0 ? h : 0;
        if (p->paused && p->frames_shown)
            seek_locked(p, clock_us(p));        /* the still picture again, at the new size */
        p->seek_pending = 0;
    } else if (!strcmp(op, "vplay")) {
        if (v >= 0)
            seek_locked(p, (int64_t)v * 1000);
        rebase(p);
        p->paused = 0;
        p->said_end = 0;
        p->playing_seen = 1;
        pthread_cond_broadcast(&p->cv);
    } else if (!strcmp(op, "vpause")) {
        rebase(p);
        p->paused = 1;
        say("vtime %d %lld\n", p->id, (long long)(clock_us(p) / 1000));
        pthread_cond_broadcast(&p->cv);
    } else if (!strcmp(op, "vseek")) {
        seek_locked(p, (int64_t)v * 1000);
    } else if (!strcmp(op, "vvol")) {
        p->volume = v < 0 ? 0 : v > 1000 ? 1000 : v;
    } else if (!strcmp(op, "vbufok")) {
        if (v == p->shmid && p->shm)
            shmctl(p->shmid, IPC_RMID, NULL);   /* the memory lasts while it is mapped */
    }
    pthread_mutex_unlock(&p->mu);
    return 1;
}

static int ext_want_sound(void)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        struct player *p = players[i];
        if (p && p->has_a && !p->paused && !p->adone)
            return 1;
    }
    return 0;
}

static int ext_busy(void)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (players[i] && !players[i]->paused)
            return 1;
    return 0;
}

static void ext_mix(int32_t *acc, int frames, int rate, int delay)
{
    out_rate = rate > 0 ? rate : 44100;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        struct player *p = players[i];
        if (!p || !p->has_a)
            continue;
        pthread_mutex_lock(&p->mu);
        p->delay = delay;
        if (!p->paused && !stalled(p) && !p->adone) {
            int k = frames < p->rfill ? frames : p->rfill;
            for (int j = 0; j < k; j++) {
                const int16_t *s = &p->ring[((p->rhead + j) % RING) * 2];
                acc[2 * j] += s[0] * p->volume / 1000;
                acc[2 * j + 1] += s[1] * p->volume / 1000;
            }
            p->rhead = (p->rhead + k) % RING;
            p->rfill -= k;
            if (k)
                pthread_cond_broadcast(&p->cv);
        }
        pthread_mutex_unlock(&p->mu);
    }
}

static void ext_tick(void)
{
    uint32_t now = now_ms();
    for (int i = 0; i < MAX_PLAYERS; i++) {
        struct player *p = players[i];
        if (!p || p->mse < 0)
            continue;
        pthread_mutex_lock(&p->mu);
        /* a stall is said once it has lasted a moment: the decoders take a little after each seek */
        int st = !p->paused && stalled(p);
        if (st && !p->stall_since)
            p->stall_since = now ? now : 1;
        else if (!st)
            p->stall_since = 0;
        int say_st = st && now - p->stall_since >= 150;
        if (p->playing_seen && (say_st ? !p->said_stall : !st && p->said_stall)) {
            say(say_st ? "vwait %d\n" : "vplaying %d\n", p->id);
            p->said_stall = say_st;
        }
        int ended = p->eos && (p->has_v || p->has_a) && (!p->has_v || p->vdone) && (!p->has_a || p->adone);
        if (ended && !p->said_end && !p->paused) {
            rebase(p);
            p->paused = 1;
            p->said_end = 1;
            say("vtime %d %lld\n", p->id, (long long)(clock_us(p) / 1000));
            say("vended %d\n", p->id);
        } else if (!p->paused && now - p->said_time_at >= 250) {
            p->said_time_at = now;
            say("vtime %d %lld\n", p->id, (long long)(clock_us(p) / 1000));
        }
        if (!p->mse && p->src_started && now - p->said_buf_at >= 500) {
            p->said_buf_at = now;
            int64_t r[16], o[16];
            int nr = p->has_v ? store_ranges(&p->vs, TOL, r, 8) : 0;
            if (p->has_a) {
                int no = store_ranges(&p->as, TOL, o, 8);
                if (p->has_v)
                    intersect(r, &nr, o, no, 8);
                else {
                    memcpy(r, o, sizeof(int64_t) * 2 * (size_t)no);
                    nr = no;
                }
            }
            char line[256];
            int len = snprintf(line, sizeof(line), "vbuffered %d %d", p->id, nr);
            for (int k = 0; k < nr && len < (int)sizeof(line) - 32; k++)
                len += snprintf(line + len, sizeof(line) - (size_t)len, " %lld %lld", (long long)(r[2 * k] / 1000),
                                (long long)(r[2 * k + 1] / 1000));
            if (strcmp(line, p->said_buf)) {
                snprintf(p->said_buf, sizeof(p->said_buf), "%s", line);
                say("%s\n", line);
            }
        }
        pthread_mutex_unlock(&p->mu);
    }
}

static const struct webaudio_ext ext = {
    .command = ext_command,
    .want_sound = ext_want_sound,
    .busy = ext_busy,
    .mix = ext_mix,
    .tick = ext_tick,
};

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    const char *d = getenv("WAVEMEDIA_DEBUG");
    debug = d && *d && *d != '0';
    av_log_set_level(debug ? AV_LOG_WARNING : AV_LOG_QUIET);
    webaudio_set_ext(&ext);
    return webaudio_main();
}
