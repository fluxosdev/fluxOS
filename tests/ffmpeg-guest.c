/*
 * FFmpeg on the real kernel: runs as init in the disposable QEMU environment
 * (make test-ffmpeg-qemu). Runs FFmpeg's own tests listed in /test/fate.txt
 * (tests/ffmpeg/fate.txt) with /usr/bin/ffmpeg on the FATE samples in
 * /test/fate-suite, compares what each printed with FATE's reference in
 * /test/ref (or the decoded sound with FATE's reference sound, as FATE's
 * oneoff and stddev comparisons do), checks that ffprobe describes a file,
 * and prints FFMPEG REGRESSIONS PASSED if every one did.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS 64
#define SAMPLES "/test/fate-suite/"

static void stop(void)
{
    fflush(stdout);
    reboot(REBOOT_POWEROFF);
    for (;;)
        msleep(1000);
}

#define CHECK(x) do { if (!(x)) { \
    printf("FFMPEG REGRESSION FAILED line %d: %s (errno %d)\n", __LINE__, #x, errno); \
    stop(); \
} } while (0)

struct buf {
    char *p;
    size_t n, cap;
};

static void buf_add(struct buf *b, const char *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        CHECK((b->p = realloc(b->p, b->cap)) != NULL);
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

/* Run argv, its standard output into out and its errors into err; its exit status. */
static int run(char *const *argv, struct buf *out, struct buf *err)
{
    int o[2], e[2];
    CHECK(pipe(o) == 0 && pipe(e) == 0);
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(o[1], 1);
        dup2(e[1], 2);
        close(o[0]);
        close(o[1]);
        close(e[0]);
        close(e[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(o[1]);
    close(e[1]);
    out->n = err->n = 0;
    struct pollfd fds[2] = { { o[0], POLLIN, 0 }, { e[0], POLLIN, 0 } };
    int open_fds = 2;
    static char chunk[65536];
    while (open_fds) {
        if (poll(fds, 2, -1) < 0) {
            CHECK(errno == EINTR);
            continue;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !fds[i].revents)
                continue;
            ssize_t r = read(fds[i].fd, chunk, sizeof(chunk));
            if (r > 0) {
                buf_add(i ? err : out, chunk, (size_t)r);
            } else {
                close(fds[i].fd);
                fds[i].fd = -1;
                open_fds--;
            }
        }
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int read_file(const char *path, struct buf *b)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    b->n = 0;
    static char chunk[65536];
    int r;
    while ((r = (int)read(fd, chunk, sizeof(chunk))) > 0)
        buf_add(b, chunk, (size_t)r);
    close(fd);
    return 0;
}

/* Split s at spaces into argv (from *n on), SAMPLES/ becoming the samples' folder. */
static void split(char *s, char **argv, int *n)
{
    for (char *t = strtok(s, " \t"); t && *n < MAX_ARGS - 8; t = strtok(NULL, " \t")) {
        if (!strncmp(t, "SAMPLES/", 8)) {
            char *p = malloc(strlen(SAMPLES) + strlen(t));
            CHECK(p != NULL);
            sprintf(p, "%s%s", SAMPLES, t + 8);
            t = p;
        }
        argv[(*n)++] = t;
    }
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

/* The first line where a and b differ, to show. */
static void show_difference(const char *a, const char *b)
{
    for (int line = 1;; line++) {
        const char *ea = strchr(a, '\n'), *eb = strchr(b, '\n');
        size_t la = ea ? (size_t)(ea - a) : strlen(a), lb = eb ? (size_t)(eb - b) : strlen(b);
        if (la != lb || memcmp(a, b, la)) {
            printf("  line %d: got      %.*s\n  line %d: expected %.*s\n", line, (int)la, a, line, (int)lb, b);
            return;
        }
        if (!ea || !eb)
            return;
        a = ea + 1;
        b = eb + 1;
    }
}

/* FATE's tiny_psnr on 16-bit sound: the largest difference and the deviation, sizes equal. */
static int compare_sound(const struct buf *got, const char *ref_path, int oneoff, double target, double fuzz)
{
    struct buf ref = { 0 };
    char path[256];
    snprintf(path, sizeof(path), "%s%s", SAMPLES, ref_path);
    if (read_file(path, &ref) < 0) {
        printf("  cannot read %s\n", path);
        return 0;
    }
    size_t n = (got->n < ref.n ? got->n : ref.n) / 2;
    const int16_t *a = (const int16_t *)got->p, *b = (const int16_t *)ref.p;
    long maxdiff = 0;
    double sse = 0;
    for (size_t i = 0; i < n; i++) {
        long d = labs((long)a[i] - b[i]);
        if (d > maxdiff)
            maxdiff = d;
        sse += (double)d * d;
    }
    double stddev = n ? sqrt(sse / n) : 0;
    printf("  stddev %.2f, maxdiff %ld, bytes %u/%u\n", stddev, maxdiff, (unsigned)got->n, (unsigned)ref.n);
    int ok = got->n == ref.n && (oneoff ? maxdiff <= (long)fuzz : fabs(stddev - target) <= fuzz);
    free(ref.p);
    return ok;
}

static int run_test(char *line)
{
    char *name = trim(strtok(line, "|"));
    char *cmp = trim(strtok(NULL, "|"));
    char *args = trim(strtok(NULL, ""));
    char *argv[MAX_ARGS];
    int n = 0;
    argv[n++] = "/usr/bin/ffmpeg";
    argv[n++] = "-nostdin";
    argv[n++] = "-nostats";
    argv[n++] = "-noauto_conversion_filters";
    argv[n++] = "-cpuflags";
    argv[n++] = "all";
    int threads = strstr(args, "-threads ") != NULL;
    char *words[MAX_ARGS];
    int nw = 0;
    split(args, words, &nw);
    for (int i = 0; i < nw && n < MAX_ARGS - 12; i++) {
        if (!strcmp(words[i], "-i") && !threads) {
            static char *dec_opts[] = { "-hwaccel", "none", "-threads", "1", "-thread_type", "frame+slice" };
            for (int j = 0; j < 6; j++)
                argv[n++] = dec_opts[j];
        }
        argv[n++] = words[i];
    }
    char kind[16] = "", file[128] = "";
    double target = 0, fuzz = 0;
    int sound = 0;
    sscanf(cmp, "%15s", kind);
    if (!strcmp(kind, "framecrc") || !strcmp(kind, "framemd5")) {
        argv[n++] = "-bitexact";
        argv[n++] = "-f";
        argv[n++] = kind;
    } else if (!strcmp(kind, "md5")) {
        argv[n++] = "-f";
        argv[n++] = "md5";
    } else if (!strcmp(kind, "oneoff") && sscanf(cmp, "%*s %lf %127s", &fuzz, file) == 2) {
        sound = 1;
    } else if (!strcmp(kind, "stddev") && sscanf(cmp, "%*s %lf %lf %127s", &target, &fuzz, file) == 3) {
        sound = 2;
    } else {
        printf("%s: unknown comparison '%s'\n", name, cmp);
        return 0;
    }
    if (sound) {
        argv[n++] = "-f";
        argv[n++] = "s16le";
    }
    argv[n++] = "-";
    argv[n] = NULL;

    static struct buf out, err, ref;
    unsigned int t0 = uptime_ms();
    int st = run(argv, &out, &err);
    unsigned int ms = uptime_ms() - t0;
    int ok = 0;
    if (st != 0) {
        printf("%s: ffmpeg exited with %d\n", name, st);
    } else if (sound) {
        ok = compare_sound(&out, file, sound == 1, target, fuzz);
    } else {
        char path[256];
        snprintf(path, sizeof(path), "/test/ref/%s", name);
        CHECK(read_file(path, &ref) == 0);
        if (!strcmp(kind, "md5") && !strncmp(out.p, "MD5=", 4))
            memmove(out.p, out.p + 4, out.n - 3);
        ok = out.n && !strcmp(out.p, ref.p);
        if (!ok)
            show_difference(out.n ? out.p : "", ref.p);
    }
    printf("%s %s (%s%s, %u ms)\n", ok ? "PASS" : "FAIL", name, kind, threads ? ", threads" : "", ms);
    if (!ok && err.n <= 6000)
        printf("  ffmpeg said: %s\n", err.p ? err.p : "");
    else if (!ok)
        printf("  ffmpeg said: %.4000s\n  ...\n%s\n", err.p, err.p + err.n - 2000);
    return ok;
}

int main(void)
{
    setenv("PATH", "/bin:/usr/bin", 1);
    printf("guest: %d CPUs online\n", (int)sysconf(_SC_NPROCESSORS_ONLN));

    static struct buf out, err, list;
    char *version[] = { "/usr/bin/ffmpeg", "-hide_banner", "-version", NULL };
    CHECK(run(version, &out, &err) == 0);
    printf("%.*s", (int)(strchr(out.p, '\n') ? strchr(out.p, '\n') - out.p + 1 : out.n), out.p);
    CHECK(strstr(out.p, "ffmpeg version 9.0.2"));

    /* ffprobe: the streams of an MP4, as key=value lines */
    char *probe[] = { "/usr/bin/ffprobe", "-v", "error", "-show_entries", "stream=codec_name,width,height",
                      "-of", "default=noprint_wrappers=1", SAMPLES "mov/mov-3elist.mov", NULL };
    CHECK(run(probe, &out, &err) == 0);
    printf("ffprobe: %s", out.p);
    CHECK(strstr(out.p, "codec_name=h264") && strstr(out.p, "width="));

    CHECK(read_file("/test/fate.txt", &list) == 0);
    int passed = 0, failed = 0;
    for (char *line = list.p, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *t = trim(line);
        if (!*t || *t == '#')
            continue;
        char copy[1024];
        snprintf(copy, sizeof(copy), "%s", t);
        if (run_test(copy))
            passed++;
        else
            failed++;
    }
    printf("%d of %d FATE tests passed\n", passed, passed + failed);
    if (!failed)
        printf("FFMPEG REGRESSIONS PASSED\n");
    stop();
}
