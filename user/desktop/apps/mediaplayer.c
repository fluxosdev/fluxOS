/*
 * The Media Player: plays the sound and video files FFmpeg (third_party/ffmpeg) can read: MP4,
 * Matroska, WebM, AVI, MPEG, Ogg, FLAC, MP3, AAC, WAV and the rest. A file opens paused, showing
 * its first picture (or its cover art, or its tags); Play, Pause, Stop and the bar, which can be
 * clicked or dragged, seek.
 *
 * Besides files it plays http:// and https:// addresses (read through media/httpio.c), and YouTube
 * links (media/ytresolve.c finds the video's streams): one stream with both picture and sound if
 * YouTube offers one, or else a picture stream and a sound stream, read side by side.
 *
 * Playback is four threads of this program's, so the desktop's loop never waits on a decoder:
 *   demux   reads packets and queues them for the sound and the picture, and seeks;
 *   sound   decodes, resamples to what /dev/dsp can play (16 bit stereo, at most 48000 Hz) and
 *           writes it; the card's delay (SNDCTL_DSP_GETODELAY) turns what was written into what
 *           is heard, which is the clock;
 *   video   decodes, waits until the clock reaches a frame's time (a file without sound has the
 *           clock of the wall), drops the frames it is late for, and scales the rest into the
 *           buffer the window draws from.
 * After a seek the packets before it are told apart by a serial number, and what is decoded
 * before the target is thrown away, so playback resumes at the time asked for.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/soundcard.h>
#include <unistd.h>
#include <flux.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "desktop.h"
#include "httpio.h"
#include "mediaio.h"
#include "ytresolve.h"

#define CTRL_H    56                    /* the strip under the picture */
#define QMAX      64                    /* packets queued per stream */
#define LATE_MS   80                    /* a frame later than this is dropped, if more are waiting */
#define MAX_RATE  48000                 /* what /dev/dsp plays */
#define YT_MAX_H  480                   /* the tallest YouTube picture asked for: software decoding */

enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };
enum { BT_OPEN, BT_PLAY, BT_STOP, NBT };

struct pqueue {
    AVPacket *p[QMAX];
    int head, n;
};

/* One open file and the threads playing it. Shared fields are under mu. */
struct sess {
    struct media_input in, ain;         /* what is read; ain: a separate sound stream (afmt) */
    AVFormatContext *fmt, *afmt;
    int veof, aeof;                     /* with afmt: each input has ended */
    int64_t last_v_ms, last_a_ms;       /* ...and the time it has been read to */
    AVCodecContext *vdec, *adec;
    int vi, ai;                         /* stream numbers, or -1 */
    int cover;                          /* vi is a picture attached to a sound file */
    pthread_t tdem, tvid, taud;
    int nthreads;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct pqueue vq, aq;
    int quit, paused;
    int eof, vdone, adone;              /* the file ended; each decoder has drained */
    int seek_req;
    int64_t seek_ms;
    int serial;                         /* the epoch the queued packets belong to */
    int64_t target_ms;                  /* decoded output before this is dropped */
    int show_one;                       /* show the next frame at once (after a seek, or at the start) */
    int64_t clk_ms;                     /* the position at clk_at (now_ms), while running */
    uint32_t clk_at;
    int running;
    int dsp, rate;                      /* /dev/dsp (or -1), and its rate */
    int inited;                         /* mu and cv are set up */
    /* the picture, scaled to fit_w x fit_h by the video thread */
    uint32_t *vbuf;
    int vbw, vbh, vb_new;
    int fit_w, fit_h;
    int64_t a_base_ms, a_frames;        /* the sound: its time at the first frame after a flush, and frames since */
    int a_have_base;
};

struct player {
    struct window *win;
    struct sess *s;
    char path[PATH_MAX];
    char title[96], info[128], msg[96];
    int have_art;
    int64_t dur_ms;
    int state;
    int64_t pos_ms;                     /* while not playing */
    int pressed, seeking, seek_resume;
    int64_t seek_to;
    uint32_t painted_at;
};

static struct player *cur;
static int nprocs = 1;

/* ---- the clock ---- */

static int64_t position_locked(const struct sess *s)
{
    int64_t t = s->clk_ms;
    if (s->running)
        t += (int64_t)(now_ms() - s->clk_at);
    return t < 0 ? 0 : t;
}

static int64_t position(struct player *p)
{
    if (!p->s)
        return p->pos_ms;
    if (p->state != ST_PLAYING)
        return p->pos_ms;
    pthread_mutex_lock(&p->s->mu);
    int64_t t = position_locked(p->s);
    pthread_mutex_unlock(&p->s->mu);
    return p->dur_ms && t > p->dur_ms ? p->dur_ms : t;
}

static void set_clock(struct sess *s, int64_t ms, int running)     /* under mu */
{
    s->clk_ms = ms;
    s->clk_at = now_ms();
    s->running = running;
}

/* ---- packet queues ---- */

static void q_push(struct pqueue *q, AVPacket *pkt)                /* under mu, with room */
{
    q->p[(q->head + q->n++) % QMAX] = pkt;
}

static AVPacket *q_pop(struct pqueue *q)
{
    AVPacket *pkt = q->p[q->head];
    q->head = (q->head + 1) % QMAX;
    q->n--;
    return pkt;
}

static void q_clear(struct pqueue *q)
{
    while (q->n) {
        AVPacket *pkt = q_pop(q);
        av_packet_free(&pkt);
    }
}

static int64_t to_ms(int64_t ts, AVRational tb)
{
    return av_rescale_q(ts, tb, (AVRational){ 1, 1000 });
}

/* The sound's stream, in whichever input has it. */
static AVStream *astream(const struct sess *s)
{
    return (s->afmt ? s->afmt : s->fmt)->streams[s->ai];
}

/* ---- the demux thread ---- */

static void push_cover(struct sess *s)                              /* under mu */
{
    if (s->cover && s->vq.n < QMAX) {
        AVPacket *pkt = av_packet_alloc();
        if (pkt && av_packet_ref(pkt, &s->fmt->streams[s->vi]->attached_pic) == 0) {
            pkt->opaque = (void *)(intptr_t)s->serial;
            q_push(&s->vq, pkt);
        } else {
            av_packet_free(&pkt);
        }
    }
}

static void *demux_main(void *arg)
{
    struct sess *s = arg;
    pthread_mutex_lock(&s->mu);
    push_cover(s);
    while (!s->quit) {
        if (s->seek_req) {
            int64_t ms = s->seek_ms;
            s->seek_req = 0;
            pthread_mutex_unlock(&s->mu);
            int r = av_seek_frame(s->fmt, -1, ms * 1000, AVSEEK_FLAG_BACKWARD);
            if (r < 0)
                r = av_seek_frame(s->fmt, -1, ms * 1000, AVSEEK_FLAG_ANY);
            if (s->afmt && av_seek_frame(s->afmt, -1, ms * 1000, AVSEEK_FLAG_BACKWARD) < 0)
                av_seek_frame(s->afmt, -1, ms * 1000, AVSEEK_FLAG_ANY);
            pthread_mutex_lock(&s->mu);
            q_clear(&s->vq);
            q_clear(&s->aq);
            s->serial++;
            s->eof = s->vdone = s->adone = 0;
            s->veof = s->aeof = 0;
            s->last_v_ms = s->last_a_ms = ms;
            s->target_ms = ms;
            push_cover(s);
            pthread_cond_broadcast(&s->cv);
            continue;
        }
        AVFormatContext *from = s->fmt;
        if (s->afmt) {                  /* two inputs: read the one that is behind, if it has room */
            if (s->veof && s->aeof)
                s->eof = 1;
            int vok = !s->veof && s->vq.n < QMAX - 1, aok = !s->aeof && s->aq.n < QMAX - 1;
            if (s->eof || (!vok && !aok)) {
                pthread_cond_wait(&s->cv, &s->mu);
                continue;
            }
            from = vok && (!aok || s->last_v_ms <= s->last_a_ms) ? s->fmt : s->afmt;
        } else if (s->eof || (s->vq.n >= QMAX - 1) || (s->aq.n >= QMAX - 1)) {
            pthread_cond_wait(&s->cv, &s->mu);
            continue;
        }
        int serial = s->serial;
        pthread_mutex_unlock(&s->mu);
        AVPacket *pkt = av_packet_alloc();
        int r = pkt ? av_read_frame(from, pkt) : AVERROR(ENOMEM);
        pthread_mutex_lock(&s->mu);
        if (r < 0) {
            av_packet_free(&pkt);
            if (serial == s->serial && !s->seek_req) {
                if (!s->afmt)
                    s->eof = 1;
                else if (from == s->afmt)
                    s->aeof = 1;
                else
                    s->veof = 1;
            }
            pthread_cond_broadcast(&s->cv);
            continue;
        }
        if (serial != s->serial || s->seek_req) {       /* read before a seek */
            av_packet_free(&pkt);
            continue;
        }
        pkt->opaque = (void *)(intptr_t)serial;
        int audio = pkt->stream_index == s->ai && (s->afmt ? from == s->afmt : 1);
        int video = pkt->stream_index == s->vi && from == s->fmt;
        int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        if (ts != AV_NOPTS_VALUE) {
            int64_t ms = to_ms(ts, from->streams[pkt->stream_index]->time_base);
            if (from == s->fmt)
                s->last_v_ms = ms;
            else
                s->last_a_ms = ms;
        }
        if (audio && s->adec)
            q_push(&s->aq, pkt);
        else if (video && s->vdec && !s->cover)
            q_push(&s->vq, pkt);
        else
            av_packet_free(&pkt);
        pthread_cond_broadcast(&s->cv);
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

/* ---- the sound thread ---- */

static void write_all(struct sess *s, const uint8_t *buf, int n)
{
    while (n > 0 && !s->quit) {
        int w = (int)write(s->dsp, buf, (size_t)n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            s->dsp = -1;
            return;
        }
        buf += w;
        n -= w;
    }
}

static void *audio_main(void *arg)
{
    struct sess *s = arg;
    AVCodecContext *dec = s->adec;
    AVStream *st = astream(s);
    AVFrame *frame = av_frame_alloc();
    SwrContext *swr = NULL;
    int16_t *out = NULL;
    int out_cap = 0, dec_serial = -1, drained = 0;
    AVChannelLayout stereo;
    av_channel_layout_default(&stereo, 2);

    pthread_mutex_lock(&s->mu);
    while (!s->quit) {
        if (s->paused || (!s->aq.n && !(s->eof && !drained))) {
            if (!s->aq.n && s->eof && drained)
                s->adone = 1;
            pthread_cond_wait(&s->cv, &s->mu);
            continue;
        }
        AVPacket *pkt = s->aq.n ? q_pop(&s->aq) : NULL;
        int serial = s->serial;
        pthread_cond_broadcast(&s->cv);
        if (pkt && (int)(intptr_t)pkt->opaque != serial) {
            av_packet_free(&pkt);
            continue;
        }
        if (dec_serial != serial) {
            avcodec_flush_buffers(dec);
            swr_free(&swr);
            dec_serial = serial;
            drained = 0;
            s->a_have_base = 0;
            s->a_frames = 0;
        }
        int64_t target = s->target_ms;
        pthread_mutex_unlock(&s->mu);

        if (!pkt)
            drained = 1;
        int r = avcodec_send_packet(dec, pkt);          /* NULL drains */
        av_packet_free(&pkt);
        while (r >= 0 || r == AVERROR(EAGAIN)) {
            if (avcodec_receive_frame(dec, frame) < 0)
                break;
            int64_t pts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? -1 : to_ms(frame->best_effort_timestamp, st->time_base);
            if (pts >= 0 && pts + (int64_t)frame->nb_samples * 1000 / (frame->sample_rate ? frame->sample_rate : 1) < target) {
                av_frame_unref(frame);                  /* before the seek's target */
                continue;
            }
            if (!swr) {
                int rate = frame->sample_rate > MAX_RATE ? MAX_RATE : frame->sample_rate;
                if (rate < 4000)
                    rate = 4000;
                if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, rate, &frame->ch_layout, frame->format,
                                        frame->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) {
                    swr_free(&swr);
                    av_frame_unref(frame);
                    break;
                }
                if (s->dsp >= 0) {
                    int fmt = AFMT_S16_LE, ch = 2, rt = rate;
                    ioctl(s->dsp, SNDCTL_DSP_SETFMT, &fmt);
                    ioctl(s->dsp, SNDCTL_DSP_CHANNELS, &ch);
                    ioctl(s->dsp, SNDCTL_DSP_SPEED, &rt);
                    s->rate = rt;
                    if (rt != rate) {               /* the card chose another: resample to that */
                        swr_free(&swr);
                        swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, rt, &frame->ch_layout, frame->format,
                                            frame->sample_rate, 0, NULL);
                        swr_init(swr);
                    }
                } else {
                    s->rate = rate;
                }
            }
            int cap = swr_get_out_samples(swr, frame->nb_samples);
            if (cap > out_cap) {
                free(out);
                out = malloc((size_t)cap * 4);
                out_cap = out ? cap : 0;
            }
            int n = out ? swr_convert(swr, (uint8_t **)&out, out_cap, (const uint8_t **)frame->extended_data,
                                       frame->nb_samples) : 0;
            pthread_mutex_lock(&s->mu);
            int stale = dec_serial != s->serial || s->quit;
            if (!stale && !s->a_have_base) {
                s->a_have_base = 1;
                s->a_base_ms = pts >= 0 ? pts : target;
                s->a_frames = 0;
            }
            pthread_mutex_unlock(&s->mu);
            av_frame_unref(frame);
            if (stale)
                break;
            if (n > 0) {
                if (s->dsp >= 0)
                    write_all(s, (const uint8_t *)out, n * 4);
                else
                    msleep((unsigned)(n * 1000 / s->rate));
                int delay = 0;
                if (s->dsp >= 0 && ioctl(s->dsp, SNDCTL_DSP_GETODELAY, &delay) < 0)
                    delay = 0;
                pthread_mutex_lock(&s->mu);
                s->a_frames += n;
                int64_t heard = s->a_base_ms + s->a_frames * 1000 / s->rate - (int64_t)delay / 4 * 1000 / s->rate;
                if (!s->paused && dec_serial == s->serial)
                    set_clock(s, heard, 1);
                pthread_mutex_unlock(&s->mu);
            }
            pthread_mutex_lock(&s->mu);
            while (s->paused && !s->quit && dec_serial == s->serial)
                pthread_cond_wait(&s->cv, &s->mu);
            pthread_mutex_unlock(&s->mu);
        }
        pthread_mutex_lock(&s->mu);
    }
    pthread_mutex_unlock(&s->mu);
    swr_free(&swr);
    free(out);
    av_frame_free(&frame);
    av_channel_layout_uninit(&stereo);
    return NULL;
}

/* ---- the video thread ---- */

static void *video_main(void *arg)
{
    struct sess *s = arg;
    AVCodecContext *dec = s->vdec;
    AVStream *st = s->fmt->streams[s->vi];
    AVFrame *frame = av_frame_alloc();
    struct SwsContext *sws = NULL;
    int sw = 0, sh = 0, sfmt = -1, dw = 0, dh = 0;
    int dec_serial = -1, drained = 0;

    pthread_mutex_lock(&s->mu);
    while (!s->quit) {
        if (!s->vq.n && !(s->eof && !drained)) {
            if (s->eof && drained)
                s->vdone = 1;
            pthread_cond_wait(&s->cv, &s->mu);
            continue;
        }
        AVPacket *pkt = s->vq.n ? q_pop(&s->vq) : NULL;
        int serial = s->serial;
        pthread_cond_broadcast(&s->cv);
        if (pkt && (int)(intptr_t)pkt->opaque != serial) {
            av_packet_free(&pkt);
            continue;
        }
        if (dec_serial != serial) {
            avcodec_flush_buffers(dec);
            dec_serial = serial;
            drained = 0;
        }
        int64_t target = s->target_ms;
        pthread_mutex_unlock(&s->mu);

        if (!pkt)
            drained = 1;
        int r = avcodec_send_packet(dec, pkt);
        av_packet_free(&pkt);
        while (r >= 0 || r == AVERROR(EAGAIN)) {
            if (avcodec_receive_frame(dec, frame) < 0)
                break;
            int64_t pts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? -1
                          : to_ms(frame->best_effort_timestamp, st->time_base);
            int still = s->cover || pts < 0;
            if (!still && pts < target) {
                av_frame_unref(frame);
                continue;
            }
            /* wait for the frame's time */
            int drop = 0;
            pthread_mutex_lock(&s->mu);
            while (!s->quit && dec_serial == s->serial && !s->show_one) {
                if (still || s->paused) {
                    if (still)
                        break;
                    pthread_cond_wait(&s->cv, &s->mu);
                    continue;
                }
                int64_t ahead = pts - position_locked(s);
                if (ahead <= 2)
                    break;
                pthread_mutex_unlock(&s->mu);
                msleep((unsigned)(ahead > 20 ? 20 : ahead));
                pthread_mutex_lock(&s->mu);
            }
            int stale = dec_serial != s->serial || s->quit;
            if (!stale && !s->show_one && !still && position_locked(s) - pts > LATE_MS && s->vq.n > 0)
                drop = 1;
            s->show_one = 0;
            int fw = s->fit_w, fh = s->fit_h;
            pthread_mutex_unlock(&s->mu);
            if (stale) {
                av_frame_unref(frame);
                break;
            }
            if (drop || fw < 2 || fh < 2) {
                av_frame_unref(frame);
                continue;
            }
            /* fit the frame in fw x fh, keeping its shape */
            int w = frame->width, h = frame->height;
            if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0)
                w = (int)((int64_t)w * frame->sample_aspect_ratio.num / frame->sample_aspect_ratio.den);
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            int ow = fw, oh = (int)((int64_t)fw * h / w);
            if (oh > fh) {
                oh = fh;
                ow = (int)((int64_t)fh * w / h);
            }
            ow &= ~1;
            oh &= ~1;
            if (ow < 2 || oh < 2)
                ow = oh = 2;
            if (!sws || sw != frame->width || sh != frame->height || sfmt != frame->format || dw != ow || dh != oh) {
                sws_freeContext(sws);
                sws = sws_getContext(frame->width, frame->height, frame->format, ow, oh, AV_PIX_FMT_BGR0,
                                     SWS_FAST_BILINEAR, NULL, NULL, NULL);
                sw = frame->width;
                sh = frame->height;
                sfmt = frame->format;
                dw = ow;
                dh = oh;
            }
            if (sws) {
                pthread_mutex_lock(&s->mu);
                if (s->vbw != ow || s->vbh != oh) {
                    free(s->vbuf);
                    s->vbuf = malloc((size_t)ow * (size_t)oh * 4);
                    s->vbw = s->vbuf ? ow : 0;
                    s->vbh = s->vbuf ? oh : 0;
                }
                if (s->vbuf) {
                    uint8_t *dst[4] = { (uint8_t *)s->vbuf, NULL, NULL, NULL };
                    int stride[4] = { ow * 4, 0, 0, 0 };
                    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, dst, stride);
                    s->vb_new = 1;
                }
                pthread_mutex_unlock(&s->mu);
            }
            av_frame_unref(frame);
        }
        pthread_mutex_lock(&s->mu);
    }
    pthread_mutex_unlock(&s->mu);
    sws_freeContext(sws);
    av_frame_free(&frame);
    return NULL;
}

/* ---- sessions ---- */

static AVCodecContext *open_decoder(AVStream *st, int threads)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!ctx)
        return NULL;
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    ctx->pkt_timebase = st->time_base;
    ctx->thread_count = threads;
    if (avcodec_open2(ctx, codec, NULL) < 0)
        avcodec_free_context(&ctx);
    return ctx;
}

static void sess_free(struct sess *s)
{
    if (!s)
        return;
    if (s->nthreads) {
        pthread_mutex_lock(&s->mu);
        s->quit = 1;
        pthread_cond_broadcast(&s->cv);
        pthread_mutex_unlock(&s->mu);
    }
    media_input_abort(&s->in);          /* a read waiting on the network gives up */
    media_input_abort(&s->ain);
    if (s->dsp >= 0)
        ioctl(s->dsp, SNDCTL_DSP_RESET);
    for (int i = 0; i < s->nthreads; i++)
        pthread_join(i == 0 ? s->tdem : i == 1 ? s->tvid : s->taud, NULL);
    if (s->dsp >= 0)
        close(s->dsp);
    q_clear(&s->vq);
    q_clear(&s->aq);
    avcodec_free_context(&s->vdec);
    avcodec_free_context(&s->adec);
    media_input_close(&s->in);
    media_input_close(&s->ain);
    free(s->vbuf);
    if (s->nthreads || s->inited) {
        pthread_cond_destroy(&s->cv);
        pthread_mutex_destroy(&s->mu);
    }
    free(s);
}

static void sess_start(struct sess *s)
{
    pthread_create(&s->tdem, NULL, demux_main, s);
    s->nthreads = 1;
    if (s->vdec && pthread_create(&s->tvid, NULL, video_main, s) == 0)
        s->nthreads = 2;
    if (s->adec && s->nthreads == 2 && pthread_create(&s->taud, NULL, audio_main, s) == 0)
        s->nthreads = 3;
    else if (s->adec && s->nthreads == 1) {
        /* thread slots are tdem, tvid, taud in order; a sound-only file puts the sound in tvid's */
        pthread_create(&s->tvid, NULL, audio_main, s);
        s->nthreads = 2;
    }
}

static const char *tag(AVFormatContext *f, const char *key)
{
    AVDictionaryEntry *e = av_dict_get(f->metadata, key, NULL, 0);
    if (!e && f->nb_streams)
        for (unsigned i = 0; !e && i < f->nb_streams; i++)
            e = av_dict_get(f->streams[i]->metadata, key, NULL, 0);
    return e && e->value[0] ? e->value : NULL;
}

/* Open what a YouTube link names: one stream with both picture and sound if there is one, or else
 * a picture stream and a sound stream. 0, or -1 with why in err. */
static int open_youtube(struct sess *s, const char *id, char *title, size_t tsize, int64_t *dur_ms,
                        char *err, size_t errsize)
{
    struct yt_video v;
    if (yt_resolve(id, httpio_request, NULL, &v) < 0) {
        snprintf(err, errsize, "%s", v.error);
        yt_free(&v);
        return -1;
    }
    char ua[220];
    snprintf(ua, sizeof(ua), "User-Agent: %s", v.user_agent[0] ? v.user_agent : "Mozilla/5.0");
    const char *headers[] = { ua };
    const struct yt_format *m = yt_pick_muxed(&v, YT_MAX_H), *fv = NULL, *fa = NULL;
    int r = -1;
    if (m)
        r = media_input_open(&s->in, m->url, headers, 1, err, errsize);
    if (r < 0 && yt_pick_split(&v, YT_MAX_H, &fv, &fa) == 0) {
        r = media_input_open(&s->in, fv->url, headers, 1, err, errsize);
        if (r == 0 && media_input_open(&s->ain, fa->url, headers, 1, err, errsize) < 0) {
            media_input_close(&s->in);
            r = -1;
        }
    } else if (r < 0 && !m) {
        snprintf(err, errsize, "YouTube offers no streams this player can decode.");
    }
    if (r == 0) {
        if (v.author[0])
            snprintf(title, tsize, "%s - %s", v.author, v.title);
        else
            snprintf(title, tsize, "%s", v.title);
        *dur_ms = (int64_t)v.seconds * 1000;
    }
    yt_free(&v);
    return r;
}

/* Open path (a file, an address or a YouTube link) paused at its start; the error message in
 * p->msg if it cannot be played. */
static void load(struct player *p, const char *path)
{
    sess_free(p->s);
    p->s = NULL;
    p->state = ST_STOPPED;
    p->pos_ms = 0;
    p->dur_ms = 0;
    p->have_art = 0;
    p->msg[0] = p->info[0] = p->title[0] = 0;
    wm_set_title(p->win, "Media Player");

    struct sess *s = calloc(1, sizeof(*s));
    if (!s)
        return;
    s->vi = s->ai = -1;
    s->dsp = -1;
    s->rate = 44100;
    char err[160] = "", yt_title[200] = "", id[16];
    int64_t yt_dur = 0;
    int r = yt_video_id(path, id) ? open_youtube(s, id, yt_title, sizeof(yt_title), &yt_dur, err, sizeof(err))
                                  : media_input_open(&s->in, path, NULL, 0, err, sizeof(err));
    if (r < 0) {
        if (media_is_url(path) || yt_title[0] || strstr(err, "YouTube"))
            snprintf(p->msg, sizeof(p->msg), "%s", err);
        else
            snprintf(p->msg, sizeof(p->msg), "Cannot open %s.", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
        sess_free(s);
        return;
    }
    s->fmt = s->in.fmt;
    s->afmt = s->ain.fmt;
    int pic = -1;
    for (unsigned i = 0; i < s->fmt->nb_streams; i++) {
        AVStream *st = s->fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if (pic < 0)
                    pic = (int)i;
            } else if (s->vi < 0) {
                s->vi = (int)i;
            }
        }
    }
    s->ai = s->afmt ? av_find_best_stream(s->afmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0)
                    : av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, s->vi, NULL, 0);
    if (s->ai < 0)
        s->ai = -1;
    if (s->vi < 0 && pic >= 0) {
        s->vi = pic;
        s->cover = 1;
    }
    if (s->vi >= 0)
        s->vdec = open_decoder(s->fmt->streams[s->vi], nprocs > 4 ? 4 : nprocs);
    if (s->ai >= 0)
        s->adec = open_decoder(astream(s), 1);
    if (!s->vdec && !s->adec) {
        snprintf(p->msg, sizeof(p->msg), "No sound or picture in this file can be played.");
        sess_free(s);
        return;
    }
    if (s->adec) {
        for (int tries = 0; s->dsp < 0 && tries < 20; tries++) {
            s->dsp = open("/dev/dsp", O_WRONLY);
            if (s->dsp < 0 && errno == EBUSY)
                msleep(50);
            else
                break;
        }
        if (s->dsp < 0)
            snprintf(p->msg, sizeof(p->msg), "No sound card, or another program is using it.");
    }
    p->dur_ms = s->fmt->duration > 0 ? s->fmt->duration / 1000 : 0;
    if (!p->dur_ms && s->afmt && s->afmt->duration > 0)
        p->dur_ms = s->afmt->duration / 1000;
    if (!p->dur_ms)
        p->dur_ms = yt_dur;

    /* the description line */
    char info[128] = "";
    if (s->vdec && !s->cover)
        snprintf(info, sizeof(info), "%s %dx%d", avcodec_get_name(s->vdec->codec_id), s->vdec->width, s->vdec->height);
    if (s->adec) {
        char a[64];
        snprintf(a, sizeof(a), "%s%s %d Hz, %s", info[0] ? ", " : "", avcodec_get_name(s->adec->codec_id),
                 s->adec->sample_rate, s->adec->ch_layout.nb_channels == 1 ? "mono"
                 : s->adec->ch_layout.nb_channels == 2 ? "stereo" : "surround");
        strncat(info, a, sizeof(info) - strlen(info) - 1);
    }
    snprintf(p->info, sizeof(p->info), "%s", info);

    const char *title = tag(s->fmt, "title"), *artist = tag(s->fmt, "artist");
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (yt_title[0])
        snprintf(p->title, sizeof(p->title), "%s", yt_title);
    else if (title && artist)
        snprintf(p->title, sizeof(p->title), "%s - %s", artist, title);
    else
        snprintf(p->title, sizeof(p->title), "%s", title ? title : base);
    p->have_art = s->vdec != NULL;
    char wt[160];
    snprintf(wt, sizeof(wt), "%s - Media Player", p->title);
    wm_set_title(p->win, wt);
    snprintf(p->path, sizeof(p->path), "%s", path);

    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->inited = 1;
    s->paused = 1;
    s->show_one = 1;
    s->fit_w = p->win->content.w;
    s->fit_h = p->win->content.h - CTRL_H;
    p->s = s;
    sess_start(s);
}

/* ---- the controls ---- */

static void seek_to(struct player *p, int64_t ms)
{
    struct sess *s = p->s;
    if (!s)
        return;
    if (ms < 0)
        ms = 0;
    if (p->dur_ms && ms > p->dur_ms)
        ms = p->dur_ms;
    pthread_mutex_lock(&s->mu);
    s->seek_req = 1;
    s->seek_ms = ms;
    s->show_one = 1;
    set_clock(s, ms, p->state == ST_PLAYING);
    q_clear(&s->aq);
    q_clear(&s->vq);
    s->serial++;                        /* what the decoders hold is stale at once */
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    if (s->dsp >= 0)
        ioctl(s->dsp, SNDCTL_DSP_RESET);
    p->pos_ms = ms;
}

static void set_paused(struct player *p, int paused)
{
    struct sess *s = p->s;
    pthread_mutex_lock(&s->mu);
    s->paused = paused;
    set_clock(s, p->pos_ms, !paused);
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

static void do_play(struct player *p)
{
    if (!p->s || p->state == ST_PLAYING)
        return;
    if (p->state == ST_STOPPED || (p->dur_ms && p->pos_ms + 500 >= p->dur_ms))
        seek_to(p, 0);
    p->state = ST_PLAYING;
    p->msg[0] = 0;
    set_paused(p, 0);
}

static void do_pause(struct player *p)
{
    if (!p->s || p->state != ST_PLAYING)
        return;
    p->pos_ms = position(p);
    p->state = ST_PAUSED;
    set_paused(p, 1);
}

static void do_stop(struct player *p)
{
    if (!p->s)
        return;
    p->state = ST_STOPPED;
    p->pos_ms = 0;
    set_paused(p, 1);
    seek_to(p, 0);
}

static void toggle(struct player *p)
{
    if (p->state == ST_PLAYING)
        do_pause(p);
    else
        do_play(p);
}

/* ---- the window ---- */

static struct rect video_rect(const struct player *p)
{
    return mkrect(0, 0, p->win->content.w, p->win->content.h - CTRL_H);
}
static struct rect bar_rect(const struct player *p) { return mkrect(8, p->win->content.h - CTRL_H + 10, p->win->content.w - 16, 8); }
static struct rect button_rect(const struct player *p, int i)
{
    return mkrect(8 + i * 72, p->win->content.h - CTRL_H + 26, 66, 24);
}

static int64_t bar_pos(const struct player *p, int x)
{
    struct rect r = bar_rect(p);
    int off = x - r.x;
    off = off < 0 ? 0 : off > r.w ? r.w : off;
    return r.w ? p->dur_ms * off / r.w : 0;
}

static void fmt_time(char *out, int size, int64_t ms)
{
    int sec = (int)(ms / 1000);
    if (sec >= 3600)
        snprintf(out, (size_t)size, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else
        snprintf(out, (size_t)size, "%d:%02d", sec / 60, sec % 60);
}

static void paint_controls(struct player *p)
{
    struct surface *s = &p->win->content;
    int y0 = s->h - CTRL_H;
    gfx_noclip(s);
    gfx_fill(s, 0, y0, s->w, CTRL_H, C_FACE);
    struct rect bar = bar_rect(p);
    gfx_fill(s, bar.x, bar.y, bar.w, bar.h, C_WHITE);
    gfx_bevel(s, bar, BEVEL_SUNKEN);
    int64_t pos = position(p);
    int px = p->dur_ms ? (int)((int64_t)(bar.w - 2) * pos / p->dur_ms) : 0;
    if (px > 0)
        gfx_fill(s, bar.x + 1, bar.y + 1, px, bar.h - 2, C_NAVY);
    struct rect th = mkrect(bar.x + px - 3, bar.y - 5, 8, 18);
    gfx_fill(s, th.x, th.y, th.w, th.h, C_FACE);
    gfx_bevel(s, th, BEVEL_RAISED);

    static const char *const labels[NBT] = { "Open...", "Play", "Stop" };
    for (int i = 0; i < NBT; i++) {
        const char *label = i == BT_PLAY && p->state == ST_PLAYING ? "Pause" : labels[i];
        draw_button(s, button_rect(p, i), label, p->pressed == i, 0, 0);
    }
    char a[24], b[24], t[64];
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), p->dur_ms);
    snprintf(t, sizeof(t), "%s / %s", a, b);
    gfx_text(s, &font_sans, 8 + NBT * 72 + 8, y0 + 32, t, C_BLACK);
    wm_damage_client(p->win, mkrect(0, y0, s->w, CTRL_H));
}

static void paint_video(struct player *p, int full)
{
    struct surface *s = &p->win->content;
    struct rect v = video_rect(p);
    struct sess *ss = p->s;
    gfx_noclip(s);
    int drew = 0;
    if (ss && ss->vdec) {
        pthread_mutex_lock(&ss->mu);
        if (ss->vbuf && ss->vbw > 0 && v.w > 0 && v.h > 0) {
            /* A picture scaled for a bigger window (just shrunk; the next frame fits) shows its
             * middle: what is copied never goes outside the view. */
            int w = ss->vbw < v.w ? ss->vbw : v.w, h = ss->vbh < v.h ? ss->vbh : v.h;
            int x0 = (v.w - w) / 2, y0 = (v.h - h) / 2, sx = (ss->vbw - w) / 2, sy = (ss->vbh - h) / 2;
            if (full || x0 > 0 || y0 > 0)
                gfx_fill(s, 0, 0, v.w, v.h, C_BLACK);
            for (int y = 0; y < h; y++)
                memcpy(s->px + (size_t)(y0 + y) * (size_t)s->stride + (size_t)x0,
                       ss->vbuf + (size_t)(sy + y) * (size_t)ss->vbw + (size_t)sx, (size_t)w * 4);
            ss->vb_new = 0;
            drew = 1;
        }
        pthread_mutex_unlock(&ss->mu);
    }
    if (!drew) {
        gfx_fill(s, 0, 0, v.w, v.h, C_BLACK);
        /* nothing to show: the tags, or what to do */
        const char *line = p->msg[0] ? p->msg : p->s ? p->title : "Open a sound or video file.";
        char fit[160];
        gfx_ellipsize(&font_sans_bold, line, v.w - 24, fit, sizeof(fit));
        gfx_text(s, &font_sans_bold, 12, v.h / 2 - 16, fit, C_WHITE);
        if (p->s && p->info[0]) {
            gfx_ellipsize(&font_sans, p->info, v.w - 24, fit, sizeof(fit));
            gfx_text(s, &font_sans, 12, v.h / 2 + 4, fit, RGB(0xB0, 0xB0, 0xB0));
        }
        if (p->s && p->msg[0]) {
            gfx_ellipsize(&font_sans, p->title, v.w - 24, fit, sizeof(fit));
            gfx_text(s, &font_sans, 12, v.h / 2 + 24, fit, RGB(0xB0, 0xB0, 0xB0));
        }
    }
    wm_damage_client(p->win, v);
}

static void paint_all(struct player *p)
{
    paint_video(p, 1);
    paint_controls(p);
}

static void ask_open_done(void *ctx, const char *text);

static void open_path(struct player *p, const char *path)
{
    char id[16];
    if (media_is_url(path)) {           /* the network can take a moment: say so first */
        snprintf(p->msg, sizeof(p->msg), yt_video_id(path, id) ? "Finding the YouTube video..." : "Opening...");
        paint_video(p, 1);
        wm_flush();
    }
    load(p, path);
    if (p->s)
        do_play(p);
    paint_all(p);
}

static void ask_open_done(void *ctx, const char *text)
{
    if (cur && cur == ctx && text && *text)
        open_path(cur, text);
}

static void ask_open(struct player *p)
{
    const char *home = getenv("HOME");
    char def[PATH_MAX];
    snprintf(def, sizeof(def), "%s", p->path[0] ? p->path : home ? home : "/");
    ask_text("Open Media", "The file, web address or YouTube link to open:", def, ask_open_done, p);
}

static void mp_key(struct window *w, const struct input_event *ev)
{
    struct player *p = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (ev->code == KEY_SPACE || ev->code == KEY_PLAYPAUSE) {
        toggle(p);
    } else if (ev->code == KEY_LEFT || ev->code == KEY_RIGHT) {
        int64_t step = (ev->mods & KMOD_SHIFT) ? 60000 : 10000;
        if (p->s) {
            int64_t t = position(p) + (ev->code == KEY_LEFT ? -step : step);
            seek_to(p, t);
        }
    } else if (ev->code == KEY_HOME) {
        seek_to(p, 0);
    } else if (ev->code == KEY_STOPCD) {
        do_stop(p);
    } else if (ev->len == 1 && (ev->mods & KMOD_CTRL) && (ev->text[0] == 'o' || ev->text[0] == 'O' || ev->text[0] == 0x0F)) {
        ask_open(p);
    }
    paint_controls(p);
}

static void mp_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct player *p = w->priv;
    int left = buttons & MOUSE_LEFT;
    if ((changed & MOUSE_LEFT) && left) {
        struct rect bar = bar_rect(p);
        bar.y -= 6;
        bar.h += 12;
        p->pressed = -1;
        for (int i = 0; i < NBT; i++)
            if (rect_contains(button_rect(p, i), x, y))
                p->pressed = i;
        if (p->pressed < 0 && p->s && p->dur_ms && rect_contains(bar, x, y)) {
            p->seeking = 1;
            p->seek_resume = p->state == ST_PLAYING;
            p->seek_to = bar_pos(p, x);
            seek_to(p, p->seek_to);
        } else if (p->pressed < 0 && rect_contains(video_rect(p), x, y)) {
            toggle(p);
        }
    } else if ((changed & MOUSE_LEFT) && !left) {
        int i = p->pressed;
        p->pressed = -1;
        if (i >= 0 && rect_contains(button_rect(p, i), x, y)) {
            if (i == BT_OPEN)
                ask_open(p);
            else if (i == BT_PLAY)
                toggle(p);
            else
                do_stop(p);
        }
        p->seeking = 0;
    } else if (p->seeking) {
        int64_t t = bar_pos(p, x);
        if (t / 250 != p->seek_to / 250) {      /* not on every pixel of motion */
            p->seek_to = t;
            seek_to(p, t);
        }
    } else {
        return;
    }
    paint_controls(p);
}

static void resized(struct player *p)
{
    struct surface *c = &p->win->content;
    if (p->s) {
        pthread_mutex_lock(&p->s->mu);
        p->s->fit_w = c->w;
        p->s->fit_h = c->h - CTRL_H;
        if (p->state != ST_PLAYING)
            p->s->show_one = 1;
        pthread_cond_broadcast(&p->s->cv);
        pthread_mutex_unlock(&p->s->mu);
        if (p->state != ST_PLAYING)
            seek_to(p, p->pos_ms);          /* the picture again, at the new size */
    }
    paint_all(p);
}

static void mp_resize(struct window *w) { struct player *p = w->priv; if (p->win) resized(p); }
static void mp_restyle(struct window *w) { struct player *p = w->priv; if (p->win) paint_all(p); }
static void mp_focus(struct window *w, int focused)
{
    struct player *p = w->priv;
    if (p->win)                         /* wm_create focuses it before p->win is set */
        paint_controls(p);
}

static void mp_destroy(struct window *w)
{
    struct player *p = w->priv;
    sess_free(p->s);
    if (cur == p)
        cur = NULL;
    free(p);
}

static int mp_drop(struct window *w, int x, int y, const struct drag_data *d, int phase, int copy)
{
    (void)x; (void)y; (void)copy;
    if (phase == DROP_LEAVE || d->npaths != 1)
        return DROP_NONE;
    if (phase == DROP_OVER) {
        wm_drag_hint("Play");
        return DROP_OPEN;
    }
    open_path(w->priv, d->paths[0]);
    return DROP_OPEN;
}

static const struct window_ops mp_ops = {
    .drop = mp_drop,
    .key = mp_key,
    .mouse = mp_mouse,
    .resize = mp_resize,
    .restyle = mp_restyle,
    .focus = mp_focus,
    .destroy = mp_destroy,
};

/* ---- the loop's hooks ---- */

int mediaplayer_timeout(void)
{
    return cur && cur->state == ST_PLAYING ? 10 : -1;
}

void mediaplayer_frame(void)
{
    struct player *p = cur;
    if (!p || !p->s)
        return;
    struct sess *s = p->s;
    int fresh, ended = 0;
    pthread_mutex_lock(&s->mu);
    fresh = s->vb_new;
    if (p->state == ST_PLAYING && s->eof && (!s->vdec || s->vdone) && (!s->adec || s->adone))
        ended = 1;
    pthread_mutex_unlock(&s->mu);
    if (fresh)
        paint_video(p, 0);
    /* a picture that arrived while stopped or paused is drawn too (above); the bar 4 times a second */
    uint32_t now = now_ms();
    if (ended) {
        do_stop(p);
        paint_all(p);
    } else if (p->state == ST_PLAYING && now - p->painted_at >= 250) {
        p->painted_at = now;
        paint_controls(p);
    }
    if (!ended && p->state != ST_PLAYING && fresh)
        paint_controls(p);
}

struct window *mediaplayer_open(const char *path)
{
    if (cur) {
        wm_focus(cur->win);
    } else {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nprocs = n > 0 ? (int)n : 1;
        av_log_set_level(AV_LOG_QUIET);
        struct player *p = calloc(1, sizeof(*p));
        if (!p)
            return NULL;
        p->pressed = -1;
        struct window *w = wm_create("Media Player", ICON_VIDEO, 480, 270 + CTRL_H, WF_MINIMIZE | WF_MAXIMIZE, &mp_ops, p);
        if (!w) {
            free(p);
            return NULL;
        }
        wm_set_min_size(w, 320, 120 + CTRL_H);
        p->win = w;
        cur = p;
    }
    if (path && *path)
        open_path(cur, path);
    else
        paint_all(cur);
    return cur->win;
}
