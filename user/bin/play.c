/*
 * play - play sound files on the Sound Blaster
 *
 *   play [-q] FILE...
 *   play [-q] -t HZ [-l MS]
 *
 * Plays WAV files (PCM, 8, 16, 24 or 32 bit, mono or stereo) and Creative
 * Voice files (.VOC: 8-bit and 16-bit PCM, silence blocks, stereo). Rates
 * the card cannot do (it goes from 5000 to 44100 Hz) are resampled. -t
 * plays a test tone (a sine wave, 2 seconds unless -l says otherwise),
 * and -q keeps quiet instead of showing the format and the time.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/soundcard.h>
#include <unistd.h>

#define CHUNK 4096                      /* frames decoded at a time */

static int quiet;
static int dsp = -1;
static int out_rate, out_channels;      /* what /dev/dsp is set to */

/* ---- reading ---- */

struct source {
    int fd;
    int voc;
    int rate, channels, bits;           /* bits 8: unsigned; more: signed */
    uint32_t left;                      /* bytes left in the current data block */
    uint32_t silence;                   /* VOC: frames of silence still to give */
    int pending_rate, pending_channels; /* VOC: a type 8 block's format for the next */
    uint32_t total_frames;              /* 0 if unknown */
    uint8_t buf[CHUNK * 8];
};

/* a * 65536 / b in 32 bits (nothing here may need libgcc's 64-bit division). */
static uint32_t ratio16(uint32_t a, uint32_t b)
{
    return (a / b << 16) + (a % b << 16) / b;
}

static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le24(const uint8_t *p) { return le16(p) | (uint32_t)p[2] << 16; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

static int read_full(int fd, void *buf, uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        int r = (int)read(fd, (uint8_t *)buf + got, n - got);
        if (r <= 0)
            return r < 0 ? -1 : (int)got;
        got += (uint32_t)r;
    }
    return (int)got;
}

static int skip(int fd, uint32_t n)
{
    return lseek(fd, (long)n, SEEK_CUR) < 0 ? -1 : 0;
}

static int open_wav(struct source *s, const char *name)
{
    uint8_t h[12], ck[8], fmt[40];
    if (read_full(s->fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return -1;
    int have_fmt = 0;
    for (;;) {
        if (read_full(s->fd, ck, 8) != 8) {
            fprintf(stderr, "play: %s: no sound data\n", name);
            return -2;
        }
        uint32_t len = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            uint32_t n = len < sizeof(fmt) ? len : sizeof(fmt);
            if (n < 16 || read_full(s->fd, fmt, n) != (int)n || skip(s->fd, (len - n) + (len & 1)) < 0)
                return -1;
            uint32_t tag = le16(fmt);
            if (tag == 0xFFFE && n >= 26)
                tag = le16(fmt + 24);   /* WAVE_FORMAT_EXTENSIBLE: the subformat */
            s->channels = (int)le16(fmt + 2);
            s->rate = (int)le32(fmt + 4);
            s->bits = (int)le16(fmt + 14);
            if (tag != 1 || s->channels < 1 || s->channels > 2 || s->rate < 1000 ||
                (s->bits != 8 && s->bits != 16 && s->bits != 24 && s->bits != 32)) {
                fprintf(stderr, "play: %s: only PCM WAV files in mono or stereo can be played\n", name);
                return -2;
            }
            have_fmt = 1;
        } else if (!memcmp(ck, "data", 4)) {
            if (!have_fmt)
                return -1;
            s->left = len;
            s->total_frames = len / (uint32_t)(s->channels * s->bits / 8);
            return 0;
        } else if (skip(s->fd, len + (len & 1)) < 0) {
            return -1;
        }
    }
}

/* Move to the next VOC block that holds samples or silence; 0 at the end. */
static int voc_next(struct source *s)
{
    for (;;) {
        uint8_t h[4], b[12];
        if (read_full(s->fd, h, 1) != 1 || h[0] == 0)
            return 0;
        if (read_full(s->fd, h + 1, 3) != 3)
            return 0;
        uint32_t len = le24(h + 1);
        switch (h[0]) {
        case 1:                         /* sound: time constant, packing */
            if (len < 2 || read_full(s->fd, b, 2) != 2)
                return 0;
            if (b[1] != 0)
                goto unsupported;
            if (s->pending_rate) {
                s->rate = s->pending_rate;
                s->channels = s->pending_channels;
                s->pending_rate = 0;
            } else {
                s->rate = (int)(1000000 / (256 - b[0]));
                s->channels = 1;
            }
            s->bits = 8;
            s->left = len - 2;
            return 1;
        case 2:                         /* more of the same */
            s->left = len;
            return 1;
        case 3:                         /* silence: length - 1, time constant */
            if (len < 3 || read_full(s->fd, b, 3) != 3)
                return 0;
            s->silence = le16(b) + 1;
            skip(s->fd, len - 3);
            return 1;
        case 8:                         /* the next block's format: tc16, packing, stereo */
            if (len < 4 || read_full(s->fd, b, 4) != 4)
                return 0;
            s->pending_channels = b[3] ? 2 : 1;
            s->pending_rate = (int)(256000000u / (65536u - le16(b)) / (uint32_t)s->pending_channels);
            skip(s->fd, len - 4);
            break;
        case 9:                         /* rate, bits, channels, format */
            if (len < 12 || read_full(s->fd, b, 12) != 12)
                return 0;
            s->rate = (int)le32(b);
            s->bits = b[4];
            s->channels = b[5];
            if ((le16(b + 6) != 0 && le16(b + 6) != 4) || (s->bits != 8 && s->bits != 16) ||
                s->channels < 1 || s->channels > 2)
                goto unsupported;
            s->left = len - 12;
            return 1;
        default:                        /* markers, text, repeats: skipped */
            skip(s->fd, len);
            break;
        }
        continue;
    unsupported:
        fprintf(stderr, "play: compressed VOC data is not supported\n");
        return 0;
    }
}

static int open_voc(struct source *s)
{
    uint8_t h[26];
    if (read_full(s->fd, h, 26) != 26 || memcmp(h, "Creative Voice File\x1A", 20))
        return -1;
    uint32_t start = le16(h + 20);
    if (start > 26)
        skip(s->fd, start - 26);
    s->voc = 1;
    return voc_next(s) ? 0 : -2;
}

/* Decode up to max frames as signed 16-bit; 0 at the end of the file. */
static int decode(struct source *s, int16_t *out, int max)
{
    while (s->voc && !s->left && !s->silence)
        if (!voc_next(s))
            return 0;
    if (s->silence) {
        int n = s->silence < (uint32_t)max ? (int)s->silence : max;
        memset(out, 0, (size_t)n * (size_t)s->channels * 2);
        s->silence -= (uint32_t)n;
        return n;
    }
    int bps = s->bits / 8, frame = bps * s->channels;
    uint32_t want = (uint32_t)max * (uint32_t)frame;
    if (want > s->left)
        want = s->left - s->left % (uint32_t)frame;
    if (want > sizeof(s->buf))
        want = sizeof(s->buf) - sizeof(s->buf) % (uint32_t)frame;
    if (!want)
        return 0;
    int got = read_full(s->fd, s->buf, want);
    if (got <= 0)
        return 0;
    s->left -= (uint32_t)got;
    int frames = got / frame;
    const uint8_t *p = s->buf;
    for (int i = 0; i < frames * s->channels; i++, p += bps) {
        switch (bps) {
        case 1: out[i] = (int16_t)((p[0] - 128) << 8); break;
        case 2: out[i] = (int16_t)le16(p); break;
        default: out[i] = (int16_t)le16(p + bps - 2); break;   /* keep the top 16 bits */
        }
    }
    return frames;
}

/* ---- the card ---- */

static int set_format(int rate, int channels)
{
    int fmt = AFMT_S16_LE, ch = channels, r = rate;
    if (ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt) < 0 || ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch) < 0 ||
        ioctl(dsp, SNDCTL_DSP_SPEED, &r) < 0)
        return -1;
    out_rate = r;
    out_channels = ch;
    return 0;
}

static int write_all(const int16_t *buf, int frames)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t n = (uint32_t)frames * (uint32_t)out_channels * 2;
    while (n) {
        int w = (int)write(dsp, p, n);
        if (w < 0) {
            if (errno == EINTR)
                return -1;
            perror("play: /dev/dsp");
            return -1;
        }
        p += w;
        n -= (uint32_t)w;
    }
    return 0;
}

/* Linear interpolation from in_rate to out_rate, carried across chunks. */
struct resampler {
    uint32_t step, pos;                 /* 16.16 frames */
    int16_t last[2];                    /* the frame before this chunk */
    int primed;
};

static int resample(struct resampler *rs, const int16_t *in, int n, int ch, int16_t *out, int max)
{
    int k = 0;
    if (!rs->primed) {
        for (int c = 0; c < ch; c++)
            rs->last[c] = in[c];
        rs->primed = 1;
    }
    /* Frame i of the stream: -1 is last[], 0..n-1 are in[]. pos counts from -1. */
    while (k < max) {
        uint32_t i = rs->pos >> 16, frac = rs->pos & 0xFFFF;
        if ((int)i + 1 > n)
            break;
        for (int c = 0; c < ch; c++) {
            int a = i == 0 ? rs->last[c] : in[(i - 1) * (uint32_t)ch + (uint32_t)c];
            int b = in[i * (uint32_t)ch + (uint32_t)c];
            out[k * ch + c] = (int16_t)(a + (int)(((int64_t)(b - a) * frac) >> 16));
        }
        k++;
        rs->pos += rs->step;
    }
    for (int c = 0; c < ch; c++)
        rs->last[c] = in[(n - 1) * ch + c];
    rs->pos -= (uint32_t)n << 16;
    return k;
}

static void show_time(uint32_t frames, uint32_t total, int rate)
{
    uint32_t s = frames / (uint32_t)rate, t = total / (uint32_t)rate;
    if (total)
        printf("\r  %u:%02u / %u:%02u ", s / 60, s % 60, t / 60, t % 60);
    else
        printf("\r  %u:%02u ", s / 60, s % 60);
    fflush(stdout);
}

static int play_source(struct source *s, const char *name)
{
    static int16_t in[CHUNK * 2], out[CHUNK * 2 * 10];
    struct resampler rs;
    int cur_rate = 0, cur_ch = 0;
    uint32_t played = 0, last_shown = (uint32_t)-1;
    if (!quiet) {
        printf("%s: %d Hz, %d bit, %s", name, s->rate, s->bits, s->channels == 2 ? "stereo" : "mono");
        if (s->total_frames) {
            uint32_t t = s->total_frames / (uint32_t)s->rate;
            printf(", %u:%02u", t / 60, t % 60);
        }
        printf("\n");
    }
    for (;;) {
        int n = decode(s, in, CHUNK);
        if (n <= 0)
            break;
        if (s->rate != cur_rate || s->channels != cur_ch) {
            if (set_format(s->rate, s->channels) < 0) {
                perror("play: /dev/dsp");
                return -1;
            }
            cur_rate = s->rate;
            cur_ch = s->channels;
            memset(&rs, 0, sizeof(rs));
            rs.step = ratio16((uint32_t)cur_rate, (uint32_t)out_rate);
            rs.pos = 1u << 16;          /* start on in[0] */
        }
        if (out_channels != s->channels) {     /* the card would not take stereo */
            for (int i = 0; i < n; i++)
                in[i] = (int16_t)((in[2 * i] + in[2 * i + 1]) / 2);
        }
        int ch = out_channels;
        if (out_rate == cur_rate) {
            if (write_all(in, n) < 0)
                return -1;
        } else {
            int k = resample(&rs, in, n, ch, out, CHUNK * 10);
            if (write_all(out, k) < 0)
                return -1;
        }
        played += (uint32_t)n;
        if (!quiet && played / (uint32_t)cur_rate != last_shown) {
            last_shown = played / (uint32_t)cur_rate;
            show_time(played, s->total_frames, cur_rate);
        }
    }
    ioctl(dsp, SNDCTL_DSP_SYNC);
    if (!quiet) {
        show_time(played, s->total_frames, cur_rate ? cur_rate : 1);
        printf("\n");
    }
    return 0;
}

static int play_file(const char *name)
{
    static struct source s;
    memset(&s, 0, sizeof(s));
    s.fd = open(name, O_RDONLY);
    if (s.fd < 0) {
        fprintf(stderr, "play: %s: %s\n", name, strerror(errno));
        return -1;
    }
    int r = open_wav(&s, name);
    if (r == -1) {
        lseek(s.fd, 0, SEEK_SET);
        memset(&s.voc, 0, sizeof(s) - sizeof(s.fd));
        r = open_voc(&s);
    }
    if (r < 0) {
        if (r == -1)
            fprintf(stderr, "play: %s: not a WAV or VOC file\n", name);
        close(s.fd);
        return -1;
    }
    r = play_source(&s, name);
    close(s.fd);
    return r;
}

/* A sine wave from a quarter-wave table (sin * 32767, in steps of 1/256 turn). */
static int play_tone(int hz, int ms)
{
    static const int16_t quarter[65] = {
        0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
        12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787,
        21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683,
        28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785,
        31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767,
    };
    static int16_t buf[CHUNK];
    if (set_format(22050, 1) < 0) {
        perror("play: /dev/dsp");
        return -1;
    }
    if (!quiet)
        printf("A %d Hz tone for %d.%03d seconds\n", hz, ms / 1000, ms % 1000);
    /* The phase is a 32-bit fraction of a turn. */
    uint32_t phase = 0, step = ratio16((uint32_t)hz << 8, (uint32_t)out_rate) << 8;
    uint32_t frames = (uint32_t)out_rate * ((uint32_t)ms / 1000) +
                      (uint32_t)out_rate * ((uint32_t)ms % 1000) / 1000;
    while (frames) {
        int n = frames < CHUNK ? (int)frames : CHUNK;
        for (int i = 0; i < n; i++, phase += step) {
            uint32_t idx = phase >> 24, q = idx & 63;      /* 256 steps a turn */
            int v = (idx & 64) ? quarter[64 - q] : quarter[q];
            buf[i] = (int16_t)((idx & 128) ? -v / 2 : v / 2);   /* half volume */
        }
        if (write_all(buf, n) < 0)
            return -1;
        frames -= (uint32_t)n;
    }
    ioctl(dsp, SNDCTL_DSP_SYNC);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: play [-q] FILE...\n       play [-q] -t HZ [-l MS]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int tone = 0, ms = 2000, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            tone = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc)
            ms = atoi(argv[++i]);
        else
            usage();
    }
    if (!tone && i >= argc)
        usage();
    dsp = open("/dev/dsp", O_WRONLY);
    if (dsp < 0) {
        fprintf(stderr, "play: /dev/dsp: %s\n",
                errno == ENOENT ? "no sound card (play needs a Sound Blaster 16)" :
                errno == EBUSY ? "another program is playing sound" : strerror(errno));
        return 1;
    }
    int status = 0;
    if (tone) {
        if (tone < 20 || tone > 20000 || ms <= 0)
            usage();
        status = play_tone(tone, ms) < 0;
    }
    for (; i < argc; i++)
        if (play_file(argv[i]) < 0)
            status = 1;
    close(dsp);
    return status;
}
