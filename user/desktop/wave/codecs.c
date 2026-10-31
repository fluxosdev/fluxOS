/*
 * Sound files for Wave's <audio>: WAV (8, 16, 24 or 32-bit PCM, or 32-bit float), MP3 (MPEG-1 and 2
 * layer III, through minimp3) and Ogg Vorbis (through stb_vorbis), each decoded from memory to
 * signed 16-bit frames, mono or stereo. An MP3's frames are found once when it is opened, which
 * gives its length and where to seek to.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "codecs.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wshadow"
#include "minimp3.h"                    /* (its implementation is in mp3dec.c: the two share names) */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#pragma GCC diagnostic pop

enum { K_WAV, K_MP3, K_VORBIS };

struct codec {
    int kind, rate, channels;
    uint32_t frames;
    const unsigned char *data;
    size_t len;
    /* WAV */
    size_t start, end, at;              /* the samples' bytes, and the next one */
    int bits, is_float, wav_channels;   /* (more than two: the first two are played) */
    /* MP3: each frame's offset, so seeking is a look-up; decoded samples not yet handed out */
    mp3dec_t mp3;
    uint32_t *offsets;
    int nframes, next, spf;             /* frames, the next to decode, samples a frame (per channel) */
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int have, used;                     /* frames in pcm, and handed out of it */
    int skip;                           /* frames to drop after a seek */
    /* Vorbis */
    stb_vorbis *ogg;
};

static uint32_t le16(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

/* An MP3 file starts with an ID3v2 tag, or a frame header: eleven sync bits, layer III. */
static size_t id3_size(const unsigned char *d, size_t len)
{
    if (len < 10 || memcmp(d, "ID3", 3))
        return 0;
    size_t n = ((size_t)(d[6] & 127) << 21 | (size_t)(d[7] & 127) << 14 | (size_t)(d[8] & 127) << 7 | (d[9] & 127)) + 10;
    if (d[5] & 0x10)
        n += 10;                        /* a footer */
    return n < len ? n : len;
}

static int mp3_header(const unsigned char *d, size_t len)
{
    return len >= 4 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0 && (d[1] & 6) == 2 && (d[2] & 0xF0) != 0xF0 &&
           (d[2] & 0x0C) != 0x0C;
}

const char *codec_sniff(const unsigned char *d, size_t len)
{
    if (len >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4))
        return "audio/wav";
    if (len >= 4 && !memcmp(d, "OggS", 4))
        return "audio/ogg";
    size_t skip = id3_size(d, len);
    if (skip || mp3_header(d, len))
        return "audio/mpeg";
    return NULL;
}

int codec_type_playable(const char *type)
{
    char t[128];
    size_t n = strcspn(type, ";");
    while (n && (type[n - 1] == ' ' || type[n - 1] == '\t'))
        n--;
    if (n >= sizeof(t))
        return 0;
    memcpy(t, type, n);
    t[n] = 0;
    const char *codecs = strstr(type, "codecs=");
    static const char *const mp3[] = { "audio/mpeg", "audio/mp3", "audio/mpeg3", "audio/x-mpeg", "audio/x-mp3", NULL };
    static const char *const wav[] = { "audio/wav", "audio/wave", "audio/x-wav", "audio/vnd.wave", NULL };
    for (int i = 0; mp3[i]; i++)
        if (!strcasecmp(t, mp3[i]))
            return !codecs || strcasestr(codecs, "mp3") ? 2 : 0;
    for (int i = 0; wav[i]; i++)
        if (!strcasecmp(t, wav[i]))
            return !codecs || strstr(codecs, "1") ? 2 : 0;
    if (!strcasecmp(t, "audio/ogg") || !strcasecmp(t, "application/ogg") || !strcasecmp(t, "audio/vorbis"))
        return !codecs ? 1 : strcasestr(codecs, "vorbis") && !strcasestr(codecs, "opus") ? 2 : 0;
    return 0;
}

/* ---- WAV ---- */

static int wav_open(struct codec *c)
{
    const unsigned char *d = c->data;
    size_t len = c->len, at = 12;
    int fmt_ok = 0;
    while (at + 8 <= len) {
        uint32_t size = le32(d + at + 4);
        size_t body = at + 8;
        if (!memcmp(d + at, "fmt ", 4) && size >= 16 && body + 16 <= len) {
            uint32_t tag = le16(d + body);
            if (tag == 0xFFFE && size >= 40 && body + 26 <= len)
                tag = le16(d + body + 24);      /* WAVE_FORMAT_EXTENSIBLE: the subformat */
            c->channels = c->wav_channels = (int)le16(d + body + 2);
            c->rate = (int)le32(d + body + 4);
            c->bits = (int)le16(d + body + 14);
            c->is_float = tag == 3;
            fmt_ok = (tag == 1 && (c->bits == 8 || c->bits == 16 || c->bits == 24 || c->bits == 32)) ||
                     (tag == 3 && c->bits == 32);
        } else if (!memcmp(d + at, "data", 4) && fmt_ok) {
            c->start = c->at = body;
            c->end = size > len - body ? len : body + size;
            break;
        }
        at = body + size + (size & 1);
    }
    if (!fmt_ok || !c->start || c->channels < 1 || c->channels > 8 || c->rate < 1000 || c->rate > 192000)
        return -1;
    c->frames = (uint32_t)((c->end - c->start) / ((size_t)c->bits / 8 * (size_t)c->wav_channels));
    return 0;
}

static int16_t wav_sample(const struct codec *c, const unsigned char *p)
{
    switch (c->bits) {
    case 8: return (int16_t)((p[0] - 128) << 8);
    case 16: return (int16_t)le16(p);
    case 24: return (int16_t)le16(p + 1);
    default:
        if (c->is_float) {
            float f;
            memcpy(&f, p, 4);
            f = f > 1 ? 1 : f < -1 ? -1 : f;
            return (int16_t)(f * 32767);
        }
        return (int16_t)le16(p + 2);
    }
}

static int wav_read(struct codec *c, int16_t *out, int max)
{
    int bps = c->bits / 8, frame = bps * c->wav_channels, outch = c->channels, n = 0;
    while (n < max && c->at + (size_t)frame <= c->end) {
        const unsigned char *p = c->data + c->at;
        out[n * outch] = wav_sample(c, p);
        if (outch == 2)
            out[n * 2 + 1] = wav_sample(c, p + bps);
        c->at += (size_t)frame;
        n++;
    }
    return n;
}

/* ---- MP3 ---- */

static int mp3_open(struct codec *c)
{
    size_t at = id3_size(c->data, c->len);
    int cap = 0;
    mp3dec_init(&c->mp3);
    mp3dec_frame_info_t info;
    while (at < c->len) {
        int samples = mp3dec_decode_frame(&c->mp3, c->data + at, (int)(c->len - at > 0x7FFFFFFF ? 0x7FFFFFFF : c->len - at),
                                          NULL, &info);
        if (!info.frame_bytes)
            break;
        at += (size_t)info.frame_offset;
        if (samples) {
            if (!c->nframes) {
                c->rate = info.hz;
                c->channels = info.channels > 1 ? 2 : 1;
                c->spf = samples;
            }
            if (c->nframes == cap) {
                cap = cap ? cap * 2 : 1024;
                uint32_t *more = realloc(c->offsets, sizeof(*more) * (size_t)cap);
                if (!more)
                    return -1;
                c->offsets = more;
            }
            c->offsets[c->nframes++] = (uint32_t)at;
        }
        at += (size_t)info.frame_bytes;
    }
    if (!c->nframes || c->rate <= 0)
        return -1;
    c->frames = (uint32_t)c->nframes * (uint32_t)c->spf;
    mp3dec_init(&c->mp3);
    return 0;
}

static int mp3_read(struct codec *c, int16_t *out, int max)
{
    int n = 0;
    while (n < max) {
        if (c->used >= c->have) {
            c->have = c->used = 0;
            if (c->next >= c->nframes)
                break;
            mp3dec_frame_info_t info;
            uint32_t at = c->offsets[c->next++];
            int got = mp3dec_decode_frame(&c->mp3, c->data + at, (int)(c->len - at), c->pcm, &info);
            if (got > 0 && info.channels != c->channels) {      /* keep to the first frame's channels */
                if (info.channels == 1)
                    for (int i = got - 1; i >= 0; i--)
                        c->pcm[2 * i] = c->pcm[2 * i + 1] = c->pcm[i];
                else
                    for (int i = 0; i < got; i++)
                        c->pcm[i] = (int16_t)((c->pcm[2 * i] + c->pcm[2 * i + 1]) / 2);
            }
            c->have = got;
            if (c->skip) {
                int drop = c->skip < got ? c->skip : got;
                c->used = drop;
                c->skip -= drop;
            }
            continue;
        }
        int take = c->have - c->used < max - n ? c->have - c->used : max - n;
        memcpy(out + n * c->channels, c->pcm + c->used * c->channels, sizeof(int16_t) * (size_t)(take * c->channels));
        c->used += take;
        n += take;
    }
    return n;
}

/* From the frame before (its bits are what the one wanted may need), dropping what comes before. */
static int mp3_seek(struct codec *c, uint32_t frame)
{
    int k = (int)(frame / (uint32_t)c->spf);
    if (k >= c->nframes) {
        c->next = c->nframes;
        c->have = c->used = c->skip = 0;
        return 0;
    }
    int from = k > 0 ? k - 1 : 0;
    mp3dec_init(&c->mp3);
    c->next = from;
    c->have = c->used = 0;
    c->skip = (k - from) * c->spf + (int)(frame % (uint32_t)c->spf);
    return 0;
}

/* ---- the codecs ---- */

struct codec *codec_open(const unsigned char *data, size_t len)
{
    const char *kind = codec_sniff(data, len);
    if (!kind)
        return NULL;
    struct codec *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->data = data;
    c->len = len;
    int ok = -1;
    if (!strcmp(kind, "audio/wav")) {
        c->kind = K_WAV;
        ok = wav_open(c);
    } else if (!strcmp(kind, "audio/mpeg")) {
        c->kind = K_MP3;
        ok = mp3_open(c);
    } else if (len <= 0x7FFFFFFF) {
        int err = 0;
        c->kind = K_VORBIS;
        c->ogg = stb_vorbis_open_memory(data, (int)len, &err, NULL);
        if (c->ogg) {
            stb_vorbis_info info = stb_vorbis_get_info(c->ogg);
            c->rate = (int)info.sample_rate;
            c->channels = info.channels > 1 ? 2 : 1;
            c->frames = stb_vorbis_stream_length_in_samples(c->ogg);
            ok = c->rate > 0 ? 0 : -1;
        }
    }
    if (ok < 0) {
        codec_close(c);
        return NULL;
    }
    if (c->channels > 2)
        c->channels = 2;                /* WAV's others are left out */
    return c;
}

int codec_rate(const struct codec *c) { return c->rate; }
int codec_channels(const struct codec *c) { return c->channels; }
uint32_t codec_frames(const struct codec *c) { return c->frames; }

int codec_read(struct codec *c, int16_t *out, int max)
{
    switch (c->kind) {
    case K_WAV: return wav_read(c, out, max);
    case K_MP3: return mp3_read(c, out, max);
    default: return stb_vorbis_get_samples_short_interleaved(c->ogg, c->channels, out, max * c->channels);
    }
}

int codec_seek(struct codec *c, uint32_t frame)
{
    switch (c->kind) {
    case K_WAV: {
        size_t step = (size_t)c->bits / 8 * (size_t)c->wav_channels;
        if (frame > c->frames)
            frame = c->frames;
        c->at = c->start + (size_t)frame * step;
        return 0;
    }
    case K_MP3: return mp3_seek(c, frame);
    default:
        if (frame >= c->frames && c->frames)
            frame = c->frames - 1;
        return stb_vorbis_seek(c->ogg, frame) ? 0 : -1;
    }
}

void codec_close(struct codec *c)
{
    if (!c)
        return;
    if (c->ogg)
        stb_vorbis_close(c->ogg);
    free(c->offsets);
    free(c);
}
