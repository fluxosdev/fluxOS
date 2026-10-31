/*
 * Sound for Wave's <audio>: the player process ("wave --audio") and the browser's side of it.
 *
 * The browser writes lines to the player's input: "open ID PATH" (a sound file, which the player
 * reads and deletes), "play ID MS" (MS < 0: from where it is), "pause ID", "seek ID MS", "volume ID
 * PER_MILLE", "loop ID 0|1" and "close ID". The player answers on its output: "meta ID MS" (the
 * length, -1 if not known), "time ID MS" (four times a second while it plays, and when it pauses),
 * "ended ID" and "error ID".
 *
 * The player decodes each stream that plays, resamples it to the card's rate (linear
 * interpolation), mixes them at their volumes and writes the mix to /dev/dsp, whose writes block
 * until the card has room: that is the clock. With nothing to play it gives the device back after
 * a moment, for other programs.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <unistd.h>
#include <flux.h>
#include "codecs.h"
#include "desktop.h"
#include "webaudio.h"

#define WAVE_BIN     "/bin/wave"
#define OUT_RATE     44100
#define CHUNK        1024               /* frames mixed and written at a time */
#define MAX_STREAMS  16
#define MAX_FILE     (32 << 20)
#define IDLE_MS      1500               /* how long the device is kept with nothing playing */

extern char **environ;

/* ---- the browser's side ---- */

static int to_player = -1, from_player = -1;
static webaudio_handler handler;
static char answers[2048];
static int nanswers;

void webaudio_set_handler(webaudio_handler fn)
{
    handler = fn;
}

static void player_gone(void)
{
    if (from_player >= 0) {
        loop_unwatch(from_player);
        close(from_player);
    }
    if (to_player >= 0)
        close(to_player);
    to_player = from_player = -1;
    nanswers = 0;
    if (handler)
        handler(-1, "gone", 0);         /* every stream it had is gone with it */
}

static void player_said(void *ctx, int revents)
{
    (void)ctx, (void)revents;
    int r = (int)read(from_player, answers + nanswers, sizeof(answers) - 1 - (size_t)nanswers);
    if (r <= 0) {
        if (r == 0 || errno != EINTR)
            player_gone();
        return;
    }
    nanswers += r;
    answers[nanswers] = 0;
    char *line = answers, *nl;
    while ((nl = strchr(line, '\n'))) {
        *nl = 0;
        char what[16];
        int id = 0, a = 0, n = 0;
        if (sscanf(line, "%15s %d %n", what, &id, &n) >= 2) {
            a = n ? atoi(line + n) : 0;
            if (handler)
                handler(id, what, a);
        }
        line = nl + 1;
    }
    nanswers -= (int)(line - answers);
    memmove(answers, line, (size_t)nanswers);
    if (nanswers >= (int)sizeof(answers) - 1)
        nanswers = 0;                   /* a line too long to be one of ours */
}

static int player_start(void)
{
    if (to_player >= 0)
        return 0;
    int in[2], out[2];
    if (pipe(in) < 0)
        return -1;
    if (pipe(out) < 0) {
        close(in[0]);
        close(in[1]);
        return -1;
    }
    int pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0)
            dup2(null, 2);
        for (int fd = 3; fd < 32; fd++)
            close(fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        char *argv[] = { "wave", "--audio", NULL };
        execve(WAVE_BIN, argv, environ);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]);
        close(out[0]);
        return -1;
    }
    to_player = in[1];
    from_player = out[0];
    loop_watch(from_player, player_said, NULL);
    return 0;
}

static int tell(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof(line) || player_start() < 0)
        return -1;
    if (write(to_player, line, (size_t)n) != n) {
        player_gone();
        return -1;
    }
    return 0;
}

int webaudio_open(int id, const char *path) { return tell("open %d %s\n", id, path); }
void webaudio_play(int id, int from_ms) { tell("play %d %d\n", id, from_ms); }
void webaudio_pause(int id) { tell("pause %d\n", id); }
void webaudio_seek(int id, int ms) { tell("seek %d %d\n", id, ms); }
void webaudio_volume(int id, int per_mille) { tell("volume %d %d\n", id, per_mille); }
void webaudio_loop(int id, int on) { tell("loop %d %d\n", id, on ? 1 : 0); }
void webaudio_close(int id) { if (to_player >= 0) tell("close %d\n", id); }

/* ---- the player ---- */

struct stream {
    int id;
    struct codec *c;
    unsigned char *data;
    int rate, ch;
    int playing, loop, volume;          /* volume: per mille */
    uint32_t pos;                       /* source frames gone by (where it is) */
    uint32_t step, frac;                /* source frames an output frame (16.16), and how far past buf[0] */
    int16_t buf[(CHUNK * 8 + 2) * 2];   /* decoded frames not yet used up, as stereo */
    int have;
    int eof;
    uint32_t since_report;              /* output frames since the last "time" */
};

static struct stream *streams[MAX_STREAMS];
static int dsp = -1, out_rate, out_ch;
static unsigned idle_since;

static void say(const char *fmt, ...)
{
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0 && write(1, line, (size_t)n) < 0)
        _exit(0);                       /* the browser has gone */
}

static uint32_t ratio16(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a << 16) / b);
}

static int ms_of(const struct stream *s)
{
    return (int)((uint64_t)s->pos * 1000 / (uint32_t)s->rate);
}

static struct stream *find(int id)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] && streams[i]->id == id)
            return streams[i];
    return NULL;
}

static void drop(int i)
{
    codec_close(streams[i]->c);
    free(streams[i]->data);
    free(streams[i]);
    streams[i] = NULL;
}

static void seek_to(struct stream *s, int ms)
{
    uint32_t frame = (uint32_t)((uint64_t)(ms < 0 ? 0 : ms) * (uint32_t)s->rate / 1000);
    uint32_t total = codec_frames(s->c);
    if (total && frame > total)
        frame = total;
    codec_seek(s->c, frame);
    s->pos = frame;
    s->have = 0;
    s->frac = 0;
    s->eof = 0;
}

static void open_stream(int id, const char *path)
{
    int slot = -1;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] && streams[i]->id == id)
            drop(i);
    for (int i = 0; i < MAX_STREAMS && slot < 0; i++)
        if (!streams[i])
            slot = i;
    struct stat st;
    int fd = open(path, O_RDONLY);
    unsigned char *data = NULL;
    size_t got = 0;
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size <= MAX_FILE &&
        (data = malloc((size_t)st.st_size))) {
        while (got < (size_t)st.st_size) {
            int r = (int)read(fd, data + got, (size_t)st.st_size - got);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
    }
    if (fd >= 0)
        close(fd);
    unlink(path);
    struct codec *c = data && slot >= 0 ? codec_open(data, got) : NULL;
    struct stream *s = c ? calloc(1, sizeof(*s)) : NULL;
    if (!s) {
        codec_close(c);
        free(data);
        say("error %d\n", id);
        return;
    }
    s->id = id;
    s->c = c;
    s->data = data;
    s->rate = codec_rate(c);
    s->ch = codec_channels(c);
    s->volume = 1000;
    streams[slot] = s;
    uint32_t total = codec_frames(c);
    say("meta %d %d\n", id, total ? (int)((uint64_t)total * 1000 / (uint32_t)s->rate) : -1);
}

static void command(char *line)
{
    char op[16];
    int id, n = 0;
    if (sscanf(line, "%15s %d %n", op, &id, &n) < 2)
        return;
    const char *rest = n ? line + n : "";
    if (!strcmp(op, "open")) {
        open_stream(id, rest);
        return;
    }
    struct stream *s = find(id);
    if (!s)
        return;
    int v = atoi(rest);
    if (!strcmp(op, "play")) {
        if (v >= 0)
            seek_to(s, v);
        else if (s->eof && !s->have)
            seek_to(s, 0);              /* played to the end: again from the start */
        s->playing = 1;
    } else if (!strcmp(op, "pause")) {
        s->playing = 0;
        say("time %d %d\n", id, ms_of(s));
    } else if (!strcmp(op, "seek")) {
        seek_to(s, v);
        say("time %d %d\n", id, ms_of(s));
    } else if (!strcmp(op, "volume")) {
        s->volume = v < 0 ? 0 : v > 1000 ? 1000 : v;
    } else if (!strcmp(op, "loop")) {
        s->loop = v != 0;
    } else if (!strcmp(op, "close")) {
        for (int i = 0; i < MAX_STREAMS; i++)
            if (streams[i] == s)
                drop(i);
    }
}

static int device_open(void)
{
    if (dsp >= 0)
        return 0;
    dsp = open("/dev/dsp", O_WRONLY);
    if (dsp < 0)
        return -1;
    int fmt = AFMT_S16_LE, ch = 2, rate = OUT_RATE;
    if (ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt) < 0 || ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch) < 0 ||
        ioctl(dsp, SNDCTL_DSP_SPEED, &rate) < 0 || rate <= 0) {
        close(dsp);
        dsp = -1;
        return -1;
    }
    out_rate = rate;
    out_ch = ch == 1 ? 1 : 2;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i])
            streams[i]->step = ratio16((uint32_t)streams[i]->rate, (uint32_t)out_rate);
    return 0;
}

/* Decode more of s into its buffer (as stereo), after what is left of it. */
static void refill(struct stream *s)
{
    static int16_t raw[CHUNK * 4 * 2];
    int room = (int)(sizeof(s->buf) / sizeof(s->buf[0]) / 2) - s->have;
    if (room > CHUNK * 4)
        room = CHUNK * 4;
    if (room <= 0 || s->eof)
        return;
    int got = codec_read(s->c, raw, room);
    if (got <= 0) {
        s->eof = 1;
        return;
    }
    int16_t *to = s->buf + s->have * 2;
    for (int i = 0; i < got; i++) {
        to[2 * i] = raw[i * s->ch];
        to[2 * i + 1] = raw[i * s->ch + (s->ch > 1)];
    }
    s->have += got;
}

/* Add up to n frames of s, resampled, into acc; returns how many it had (fewer: it ended). */
static int mix(struct stream *s, int32_t *acc, int n)
{
    if (!s->step)
        s->step = ratio16((uint32_t)s->rate, (uint32_t)out_rate);
    int k = 0;
    while (k < n) {
        uint32_t i = s->frac >> 16;
        if ((int)i + 1 >= s->have) {
            if (i) {                    /* what has been passed goes */
                int keep = s->have - (int)i;
                if (keep > 0)
                    memmove(s->buf, s->buf + i * 2, sizeof(int16_t) * 2 * (size_t)keep);
                s->have = keep > 0 ? keep : 0;
                s->frac -= i << 16;
                s->pos += i;
            }
            refill(s);
            if ((int)(s->frac >> 16) + 1 >= s->have) {
                if (!s->eof)
                    continue;
                break;
            }
            continue;
        }
        uint32_t f = s->frac & 0xFFFF;
        for (int c = 0; c < 2; c++) {
            int a = s->buf[i * 2 + (uint32_t)c], b = s->buf[(i + 1) * 2 + (uint32_t)c];
            int v = a + (int)(((int64_t)(b - a) * f) >> 16);
            acc[k * 2 + c] += v * s->volume / 1000;
        }
        k++;
        s->frac += s->step;
    }
    return k;
}

static void play_chunk(void)
{
    static int32_t acc[CHUNK * 2];
    static int16_t out[CHUNK * 2];
    memset(acc, 0, sizeof(acc));
    for (int i = 0; i < MAX_STREAMS; i++) {
        struct stream *s = streams[i];
        if (!s || !s->playing)
            continue;
        int done = mix(s, acc, CHUNK);
        if (done < CHUNK && s->loop) {
            seek_to(s, 0);
            mix(s, acc + done * 2, CHUNK - done);
            done = CHUNK;
        }
        s->since_report += (uint32_t)CHUNK;
        if (done < CHUNK) {
            s->playing = 0;
            s->pos = codec_frames(s->c) ? codec_frames(s->c) : s->pos + (uint32_t)s->have;
            say("time %d %d\n", s->id, ms_of(s));
            say("ended %d\n", s->id);
        } else if (s->since_report >= (uint32_t)out_rate / 4) {
            s->since_report = 0;
            say("time %d %d\n", s->id, ms_of(s));
        }
    }
    for (int i = 0; i < CHUNK * 2; i++)
        out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
    if (out_ch == 1)
        for (int i = 0; i < CHUNK; i++)
            out[i] = (int16_t)((out[2 * i] + out[2 * i + 1]) / 2);
    const uint8_t *p = (const uint8_t *)out;
    size_t left = (size_t)CHUNK * (size_t)out_ch * 2;
    while (left) {
        int w = (int)write(dsp, p, left);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            close(dsp);
            dsp = -1;
            return;
        }
        p += w;
        left -= (size_t)w;
    }
}

int webaudio_main(void)
{
    signal(SIGPIPE, SIG_DFL);
    static char in[4096];
    int nin = 0;
    for (;;) {
        int playing = 0;
        for (int i = 0; i < MAX_STREAMS; i++)
            playing |= streams[i] && streams[i]->playing;
        if (!playing && dsp >= 0 && now_ms() - idle_since >= IDLE_MS) {
            close(dsp);                 /* the others may have the card now */
            dsp = -1;
        }
        struct pollfd pf = { 0, POLLIN, 0 };
        int wait = playing ? 0 : dsp >= 0 ? IDLE_MS : -1;
        if (poll(&pf, 1, wait) > 0 && (pf.revents & (POLLIN | POLLHUP | POLLERR))) {
            int r = (int)read(0, in + nin, sizeof(in) - 1 - (size_t)nin);
            if (r <= 0)
                return 0;               /* the browser has gone */
            nin += r;
            in[nin] = 0;
            char *line = in, *nl;
            while ((nl = strchr(line, '\n'))) {
                *nl = 0;
                command(line);
                line = nl + 1;
            }
            nin -= (int)(line - in);
            memmove(in, line, (size_t)nin);
            if (nin >= (int)sizeof(in) - 1)
                nin = 0;
            continue;                   /* what it said may have started or stopped something */
        }
        if (!playing)
            continue;
        if (device_open() < 0) {        /* no card, or another program has it: the time goes by silently */
            for (int i = 0; i < MAX_STREAMS; i++)
                if (streams[i] && streams[i]->playing) {
                    streams[i]->playing = 0;
                    say("error %d\n", streams[i]->id);
                }
            continue;
        }
        play_chunk();
        idle_since = now_ms();
    }
}
