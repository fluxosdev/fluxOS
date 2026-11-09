/*
 * Wave's media engine on the real kernel: runs as init in the disposable QEMU environment
 * (make test-media-qemu), starts /bin/wavemedia as Wave does and talks to it as Wave does (see
 * user/desktop/media/engine.c), playing real YouTube streams (tools/media-samples.py):
 *   - a progressive MP4 from a file: metadata, frames in shared memory, the clock, pause, seek, end;
 *   - Media Source Extensions with fragmented MP4 (H.264 + AAC): initialization segments, appends
 *     cut anywhere, buffered ranges, playing, remove(), seeking, endOfStream();
 *   - the same with WebM (VP9 + Opus), and abort() then appending media segments alone;
 *   - bytes that are no media segment;
 *   - with a network (--net): a YouTube video found through its API (ytresolve.c), read over HTTPS
 *     with Range requests (httpio.c), and played by the engine from its address.
 * Prints MEDIA REGRESSIONS PASSED if all went as it should.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "httpio.h"
#include "segments.h"
#include "ytresolve.h"

static void stop(void)
{
    fflush(stdout);
    reboot(REBOOT_POWEROFF);
    for (;;)
        msleep(1000);
}

#define CHECK(x) do { if (!(x)) { \
    printf("MEDIA REGRESSION FAILED line %d: %s (errno %d)\n", __LINE__, #x, errno); \
    stop(); \
} } while (0)

static unsigned now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---- the engine ---- */

static int to_eng = -1, from_eng = -1;
static char inbuf[65536];
static int nin;

/* What frames have come, per player. */
struct frames {
    int first;                          /* "vfirst" has come */
    int shmid, w, h;
    uint8_t *mem;
    int count;
    int last_w, last_h, last_idx, last_ms;
};
static struct frames fr[16];

static void start_engine(void)
{
    int in[2], out[2];
    CHECK(pipe(in) == 0 && pipe(out) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        char *argv[] = { "wavemedia", NULL };
        setenv("WAVEMEDIA_DEBUG", "1", 1);
        execv("/bin/wavemedia", argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    to_eng = in[1];
    from_eng = out[0];
}

static void sendf(const char *fmt, ...)
{
    static char line[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    CHECK(n < (int)sizeof(line));
    CHECK(write(to_eng, line, (size_t)n) == n);
}

/* Keep track of frame buffers and frames as they are announced. */
static void note(const char *line)
{
    int id, shmid, w, h, n, idx, ms;
    if (sscanf(line, "vfirst %d", &id) == 1 && id >= 0 && id < 16)
        fr[id].first = 1;
    if (sscanf(line, "vbuf %d %d %d %d %d", &id, &shmid, &w, &h, &n) == 5 && id >= 0 && id < 16) {
        struct frames *f = &fr[id];
        if (f->mem)
            shmdt(f->mem);
        f->mem = shmat(shmid, NULL, 0);
        CHECK(f->mem != (void *)-1);
        f->shmid = shmid;
        f->w = w;
        f->h = h;
        sendf("vbufok %d %d\n", id, shmid);
    } else if (sscanf(line, "vframe %d %d %d %d %d %d", &id, &shmid, &idx, &w, &h, &ms) == 6 && id >= 0 && id < 16) {
        struct frames *f = &fr[id];
        if (shmid == f->shmid) {
            f->count++;
            f->last_w = w;
            f->last_h = h;
            f->last_idx = idx;
            f->last_ms = ms;
        }
    }
}

/* The next line from the engine within ms, or 0. Frame lines are noted, not returned, unless
 * frames is set. */
static int next_line(char *out, size_t size, int ms, int frames)
{
    unsigned until = now() + (unsigned)ms;
    for (;;) {
        char *nl = memchr(inbuf, '\n', (size_t)nin);
        if (nl) {
            *nl = 0;
            snprintf(out, size, "%s", inbuf);
            int used = (int)(nl - inbuf) + 1;
            memmove(inbuf, inbuf + used, (size_t)(nin - used));
            nin -= used;
            note(out);
            if (!frames && (!strncmp(out, "vframe", 6) || !strncmp(out, "vbuf ", 5)))
                continue;
            return 1;
        }
        int left = (int)(until - now());
        if (left <= 0)
            return 0;
        struct pollfd pf = { from_eng, POLLIN, 0 };
        if (poll(&pf, 1, left) <= 0)
            continue;
        int r = (int)read(from_eng, inbuf + nin, sizeof(inbuf) - 1 - (size_t)nin);
        CHECK(r > 0);                   /* the engine died */
        nin += r;
    }
}

/* Wait for a line starting with prefix (others are shown); its text into out. */
static int expect(const char *prefix, int ms, char *out, size_t size)
{
    char line[1024];
    unsigned until = now() + (unsigned)ms;
    for (;;) {
        int left = (int)(until - now());
        if (left <= 0 || !next_line(line, sizeof(line), left, 0)) {
            printf("  (no \"%s\" within %d ms)\n", prefix, ms);
            return 0;
        }
        if (!strncmp(line, prefix, strlen(prefix))) {
            if (out)
                snprintf(out, size, "%s", line);
            return 1;
        }
        if (strncmp(line, "vtime", 5) && strncmp(line, "vbuffered", 9))
            printf("  engine: %s\n", line);
    }
}

/* Wait for two lines, in either order. */
static int expect_both(const char *a, const char *b, int ms)
{
    char line[1024];
    int got_a = 0, got_b = 0;
    unsigned until = now() + (unsigned)ms;
    while (!(got_a && got_b)) {
        int left = (int)(until - now());
        if (left <= 0 || !next_line(line, sizeof(line), left, 0)) {
            printf("  (no \"%s\" or \"%s\" within %d ms)\n", got_a ? b : a, got_a ? a : b, ms);
            return 0;
        }
        if (!strncmp(line, a, strlen(a)))
            got_a = 1;
        else if (!strncmp(line, b, strlen(b)))
            got_b = 1;
        else if (strncmp(line, "vtime", 5) && strncmp(line, "vbuffered", 9))
            printf("  engine: %s\n", line);
    }
    return 1;
}

/* Wait until a player has shown its first frame. */
static int expect_first(int id, int ms)
{
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "vfirst %d", id);
    return fr[id].first || expect(prefix, ms, NULL, 0);
}

/* Lines for ms, shown but for frames and times. */
static void drain(int ms)
{
    char line[1024];
    unsigned until = now() + (unsigned)ms;
    for (;;) {
        int left = (int)(until - now());
        if (left <= 0 || !next_line(line, sizeof(line), left, 0))
            return;
        if (strncmp(line, "vtime", 5) && strncmp(line, "vbuffered", 9))
            printf("  engine: %s\n", line);
    }
}

/* Wait until the player's clock reaches ms; the time it said. */
static int wait_time(int id, int ms, int timeout)
{
    char prefix[32], line[1024];
    snprintf(prefix, sizeof(prefix), "vtime %d ", id);
    unsigned until = now() + (unsigned)timeout;
    int t = -1;
    while ((int)(until - now()) > 0) {
        if (!expect(prefix, (int)(until - now()), line, sizeof(line)))
            break;
        t = atoi(line + strlen(prefix));
        if (t >= ms)
            return t;
    }
    return t;
}

/* Whether the last frame of a player shows a picture (not one color). */
static int frame_has_picture(int id)
{
    struct frames *f = &fr[id];
    if (!f->mem || f->last_w < 2)
        return 0;
    const uint32_t *px = (const uint32_t *)(f->mem + (size_t)f->last_idx * (size_t)f->w * (size_t)f->h * 4);
    uint32_t seen[16];
    int nseen = 0;
    for (int y = 0; y < f->last_h; y += 7)
        for (int x = 0; x < f->last_w; x += 7) {
            uint32_t c = px[y * f->last_w + x] & 0xF0F0F0;
            int k;
            for (k = 0; k < nseen && seen[k] != c; k++)
                ;
            if (k == nseen && nseen < 16)
                seen[nseen++] = c;
        }
    return nseen >= 8;
}

/* ---- files ---- */

static unsigned char *slurp(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    CHECK(fd >= 0 && fstat(fd, &st) == 0);
    unsigned char *d = malloc((size_t)st.st_size);
    CHECK(d != NULL);
    *len = 0;
    while (*len < (size_t)st.st_size) {
        int r = (int)read(fd, d + *len, (size_t)st.st_size - *len);
        CHECK(r > 0);
        *len += (size_t)r;
    }
    close(fd);
    return d;
}

static int appends;

static void append(int sbid, int seq, const unsigned char *d, size_t n)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/append-%d", ++appends);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, d, n) == (ssize_t)n);
    close(fd);
    sendf("sbappend %d %d %s\n", sbid, seq, path);
}

/* An "sbdone SBID SEQ N S E..." line's ranges, in ms. */
static int parse_ranges(const char *line, long long *r, int max)
{
    int sbid, seq, n, off = 0;
    if (sscanf(line, "sbdone %d %d %d %n", &sbid, &seq, &n, &off) < 3)
        return -1;
    const char *p = line + off;
    for (int i = 0; i < n && i < max; i++) {
        long long s, e;
        int used;
        if (sscanf(p, "%lld %lld %n", &s, &e, &used) < 2)
            return -1;
        r[2 * i] = s / 1000;
        r[2 * i + 1] = e / 1000;
        p += used;
    }
    return n;
}

static void show_ranges(const char *what, const long long *r, int n)
{
    printf("%s:", what);
    for (int i = 0; i < n; i++)
        printf(" [%lld, %lld)", r[2 * i], r[2 * i + 1]);
    printf("\n");
}

/* ---- the tests ---- */

static void test_progressive(void)
{
    char line[1024];
    printf("progressive: /test/yt-18.mp4\n");
    sendf("vsize 1 320 240\n");
    sendf("vopen 1 /test/yt-18.mp4\n");
    CHECK(expect("vmeta 1 ", 15000, line, sizeof(line)));
    long long dur;
    int w, h, audio;
    CHECK(sscanf(line, "vmeta 1 %lld %d %d %d", &dur, &w, &h, &audio) == 4);
    printf("  %s\n", line);
    CHECK(dur > 18000 && dur < 20500 && w > 0 && h > 0 && audio == 1);
    /* paused at the start, it shows its first picture */
    CHECK(expect_first(1, 10000));
    CHECK(frame_has_picture(1));
    unsigned t0 = now();
    sendf("vplay 1 0\n");
    int t = wait_time(1, 3000, 8000);
    unsigned wall = now() - t0;
    printf("  played to %d ms in %u ms, %d frames (%dx%d)\n", t, wall, fr[1].count, fr[1].last_w, fr[1].last_h);
    CHECK(t >= 3000);
    CHECK(wall > 2300 && wall < 5000);  /* in time, not as fast as it can decode */
    CHECK(fr[1].count >= 40);           /* ~30 a second */
    CHECK(fr[1].last_w <= 320 && fr[1].last_h <= 240);
    CHECK(frame_has_picture(1));

    /* paused, nothing moves */
    sendf("vpause 1\n");
    drain(400);
    int frames = fr[1].count;
    drain(800);
    CHECK(fr[1].count == frames);
    sendf("vplay 1 -1\n");
    drain(500);
    CHECK(fr[1].count > frames);

    /* seek near the end; it ends */
    sendf("vseek 1 16000\n");
    CHECK(expect("vseeked 1 16000", 5000, NULL, 0));
    CHECK(fr[1].last_ms >= 15900 && fr[1].last_ms < 16500);
    CHECK(expect("vended 1", 8000, NULL, 0));
    /* played again from the start after the end */
    sendf("vplay 1 0\n");
    CHECK(wait_time(1, 500, 4000) >= 500);
    sendf("vclose 1\n");
    printf("progressive: ok\n");
}

static void test_mse_mp4(void)
{
    char line[1024];
    long long r[16];
    printf("mse: fragmented MP4, H.264 and AAC\n");
    size_t vlen, alen;
    unsigned char *v = slurp("/test/yt-133.mp4", &vlen), *a = slurp("/test/yt-140.mp4", &alen);
    size_t vinit = seg_init_length(v, vlen, SEG_MP4), ainit = seg_init_length(a, alen, SEG_MP4);
    CHECK(vinit > 0 && vinit < vlen && ainit > 0 && ainit < alen);
    sendf("vmse 2\n");
    sendf("vsize 2 320 240\n");
    sendf("sbadd 2 20 video/mp4; codecs=\"avc1.4D400C\"\n");
    sendf("sbadd 2 21 audio/mp4; codecs=\"mp4a.40.2\"\n");
    append(20, 1, v, vinit);
    CHECK(expect("sbinit 20 ", 5000, line, sizeof(line)));
    printf("  %s\n", line);
    CHECK(!strncmp(line, "sbinit 20 1 0 ", 14) && strstr(line, "h264"));
    CHECK(expect("sbdone 20 1 0", 5000, NULL, 0));
    append(21, 1, a, ainit);
    CHECK(expect("sbinit 21 0 1 ", 5000, line, sizeof(line)));
    CHECK(strstr(line, "aac"));
    CHECK(expect("sbdone 21 1 0", 5000, NULL, 0));

    /* the media segments, cut in three anywhere */
    size_t rest = vlen - vinit, cut1 = vinit + rest / 3 + 17, cut2 = vinit + 2 * rest / 3 + 5;
    append(20, 2, v + vinit, cut1 - vinit);
    CHECK(expect("sbdone 20 2 ", 8000, line, sizeof(line)));
    int n = parse_ranges(line, r, 8);
    show_ranges("  after a third", r, n);
    CHECK(n >= 1 && r[0] < 100);
    long long first_end = r[1];
    append(20, 3, v + cut1, cut2 - cut1);
    CHECK(expect("sbdone 20 3 ", 8000, line, sizeof(line)));
    n = parse_ranges(line, r, 8);
    CHECK(n == 1 && r[1] > first_end);
    append(20, 4, v + cut2, vlen - cut2);
    CHECK(expect("sbdone 20 4 ", 8000, line, sizeof(line)));
    n = parse_ranges(line, r, 8);
    show_ranges("  video", r, n);
    CHECK(n == 1 && r[0] < 100 && r[1] > 18500);
    append(21, 2, a + ainit, alen - ainit);
    CHECK(expect("sbdone 21 2 ", 8000, line, sizeof(line)));
    n = parse_ranges(line, r, 8);
    show_ranges("  sound", r, n);
    CHECK(n == 1 && r[0] < 100 && r[1] > 18500);

    CHECK(expect_first(2, 5000));
    CHECK(frame_has_picture(2));
    sendf("vplay 2 0\n");
    int t = wait_time(2, 2000, 6000);
    printf("  played to %d ms, %d frames\n", t, fr[2].count);
    CHECK(t >= 2000 && fr[2].count >= 30);

    /* remove() the first five seconds of the picture, under the playhead: it waits */
    sendf("sbremove 20 5 0 5000000\n");
    CHECK(expect("sbdone 20 5 ", 3000, line, sizeof(line)));
    n = parse_ranges(line, r, 8);
    show_ranges("  after remove(0, 5)", r, n);
    CHECK(n == 1 && r[0] >= 4900 && r[1] > 18500);
    CHECK(expect("vwait 2", 3000, NULL, 0));
    /* seek into what is there: it plays on */
    sendf("vseek 2 12000\n");
    CHECK(expect_both("vseeked 2 12000", "vplaying 2", 5000));
    /* and ends once endOfStream() said there is no more */
    sendf("mseend 2 1\n");
    CHECK(expect("vended 2", 12000, NULL, 0));
    sendf("vclose 2\n");
    free(v);
    free(a);
    printf("mse mp4: ok\n");
}

static void test_mse_webm(void)
{
    char line[1024];
    long long r[16];
    printf("mse: WebM, VP9 and Opus\n");
    size_t vlen, alen;
    unsigned char *v = slurp("/test/yt-278.webm", &vlen), *a = slurp("/test/yt-249.webm", &alen);
    size_t vinit = seg_init_length(v, vlen, SEG_WEBM), ainit = seg_init_length(a, alen, SEG_WEBM);
    CHECK(vinit > 0 && vinit < vlen && ainit > 0 && ainit < alen);
    sendf("vmse 3\n");
    sendf("vsize 3 256 144\n");
    sendf("sbadd 3 30 video/webm; codecs=\"vp9\"\n");
    sendf("sbadd 3 31 audio/webm; codecs=\"opus\"\n");
    append(30, 1, v, vlen);             /* initialization and media segments in one */
    CHECK(expect("sbinit 30 1 0 ", 5000, line, sizeof(line)));
    CHECK(strstr(line, "vp9"));
    CHECK(expect("sbdone 30 1 ", 8000, line, sizeof(line)));
    {
        int n = parse_ranges(line, r, 8);
        show_ranges("  video", r, n);
        CHECK(n == 1 && r[1] > 18500);
    }
    append(31, 1, a, ainit);
    CHECK(expect("sbinit 31 0 1 ", 5000, line, sizeof(line)));
    CHECK(strstr(line, "opus"));
    CHECK(expect("sbdone 31 1 ", 5000, NULL, 0));
    append(31, 2, a + ainit, alen - ainit);
    CHECK(expect("sbdone 31 2 ", 8000, line, sizeof(line)));
    int n = parse_ranges(line, r, 8);
    show_ranges("  sound", r, n);
    CHECK(n == 1 && r[1] > 18500);
    sendf("vplay 3 0\n");
    CHECK(expect_first(3, 5000));
    int t = wait_time(3, 1500, 6000);
    printf("  played to %d ms, %d frames\n", t, fr[3].count);
    CHECK(t >= 1500 && fr[3].count >= 15 && frame_has_picture(3));

    /* abort(), then media segments alone: the last initialization segment is put back in front */
    sendf("sbabort 30\n");
    sendf("sbremove 30 9 10000000 20000000\n");
    CHECK(expect("sbdone 30 9 ", 3000, line, sizeof(line)));
    n = parse_ranges(line, r, 8);
    CHECK(n == 1 && r[1] <= 10100);
    append(30, 10, v + vinit, vlen - vinit);
    CHECK(expect("sbdone 30 10 ", 8000, line, sizeof(line)));
    n = parse_ranges(line, r, 8);
    show_ranges("  appended again after abort()", r, n);
    CHECK(n == 1 && r[1] > 18500);
    sendf("vclose 3\n");
    free(v);
    free(a);
    printf("mse webm: ok\n");
}

static void test_garbage(void)
{
    printf("mse: bytes that are no media segment\n");
    static unsigned char junk[20000];
    for (size_t i = 0; i < sizeof(junk); i++)
        junk[i] = (unsigned char)(i * 7919 + 13);
    sendf("vmse 4\n");
    sendf("sbadd 4 40 video/mp4; codecs=\"avc1.4D400C\"\n");
    append(40, 1, junk, sizeof(junk));
    CHECK(expect("sberr 40 1", 5000, NULL, 0));
    sendf("vclose 4\n");
    printf("garbage: ok\n");
}

/* ---- with a network ---- */

static void test_network(void)
{
    struct yt_video v;
    int r = -1;
    for (int tries = 0; tries < 10 && r < 0; tries++) {
        r = yt_resolve("jNQXAC9IVRw", httpio_request, NULL, &v);
        if (r < 0) {
            printf("network: %s\n", v.error);
            yt_free(&v);
            msleep(2000);
        }
    }
    if (r < 0) {
        printf("network: YouTube cannot be reached; skipped\n");
        return;
    }
    printf("network: \"%s\" by %s, %d s, %d streams\n", v.title, v.author, v.seconds, v.nf);
    const struct yt_format *m = yt_pick_muxed(&v, 480);
    CHECK(m != NULL);
    char ua[240], err[160];
    snprintf(ua, sizeof(ua), "User-Agent: %s", v.user_agent);
    const char *headers[] = { ua };
    unsigned t0 = now();
    struct httpio *h = httpio_open(m->url, headers, 1, err, sizeof(err));
    if (!h)
        printf("network: %s\n", err);
    CHECK(h != NULL);
    unsigned char buf[16];
    CHECK(httpio_read(h, buf, 8) == 8 && !memcmp(buf + 4, "ftyp", 4));
    int64_t size = httpio_size(h);
    printf("network: itag %d, %lld bytes, first answer in %u ms\n", m->itag, (long long)size, now() - t0);
    CHECK(size > 500000);
    /* a jump far ahead and back: fetched again from there */
    CHECK(httpio_seek(h, size - 16, SEEK_SET) == size - 16);
    CHECK(httpio_read(h, buf, 16) == 16);
    CHECK(httpio_seek(h, 0, SEEK_SET) == 0);
    CHECK(httpio_read(h, buf, 8) == 8 && !memcmp(buf + 4, "ftyp", 4));
    CHECK(httpio_read(h, buf, 0) == 0);
    httpio_close(h);

    /* the engine plays it from its address */
    char line[1024];
    sendf("vhdr 5 %s\n", ua);
    sendf("vsize 5 320 240\n");
    sendf("vopen 5 %s\n", m->url);
    CHECK(expect("vmeta 5 ", 20000, line, sizeof(line)));
    printf("  %s\n", line);
    sendf("vplay 5 0\n");
    CHECK(expect_first(5, 10000));
    int t = wait_time(5, 2000, 10000);
    printf("network: played to %d ms, %d frames\n", t, fr[5].count);
    CHECK(t >= 2000 && frame_has_picture(5));
    sendf("vseek 5 15000\n");
    CHECK(expect("vseeked 5 15000", 10000, NULL, 0));
    sendf("vclose 5\n");
    yt_free(&v);
    printf("network: ok\n");
}

int main(void)
{
    setenv("PATH", "/bin:/usr/bin", 1);
    printf("guest: %d CPUs online\n", (int)sysconf(_SC_NPROCESSORS_ONLN));
    mkdir("/tmp", 01777);
    start_engine();
    char line[256];
    CHECK(next_line(line, sizeof(line), 5000, 0) && !strcmp(line, "engine"));
    test_progressive();
    test_mse_mp4();
    test_mse_webm();
    test_garbage();
    test_network();
    close(to_eng);                      /* the engine ends when Wave does */
    int st;
    CHECK(wait(&st) > 0);
    printf("MEDIA REGRESSIONS PASSED\n");
    stop();
}
