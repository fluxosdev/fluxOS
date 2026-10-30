/* More of <stdlib.h>: arithmetic, searching, random numbers, temporary
 * files, realpath and system. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

long long llabs(long long x) { return x < 0 ? -x : x; }
intmax_t imaxabs(intmax_t x) { return x < 0 ? -x : x; }

div_t div(int num, int den)
{
    div_t r = { num / den, num % den };
    return r;
}

ldiv_t ldiv(long num, long den)
{
    ldiv_t r = { num / den, num % den };
    return r;
}

lldiv_t lldiv(long long num, long long den)
{
    lldiv_t r = { num / den, num % den };
    return r;
}

imaxdiv_t imaxdiv(intmax_t num, intmax_t den)
{
    imaxdiv_t r = { num / den, num % den };
    return r;
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    const char *lo = base;
    while (n) {
        const char *mid = lo + (n / 2) * size;
        int c = cmp(key, mid);
        if (!c)
            return (void *)mid;
        if (c > 0) {
            lo = mid + size;
            n -= n / 2 + 1;
        } else {
            n /= 2;
        }
    }
    return NULL;
}

void _Exit(int status)
{
    _exit(status);
}

int clearenv(void)
{
    static char *empty[1];
    environ = empty;
    return 0;
}

/* ---- random numbers ---- */

int rand_r(unsigned int *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (int)((*seed >> 1) & RAND_MAX);
}

/* random(): a xorshift generator, so the low bits are as random as the high */
static uint32_t random_state = 0x2545F491u;

long random(void)
{
    uint32_t x = random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    random_state = x;
    return (long)(x & 0x7FFFFFFFu);
}

void srandom(unsigned int seed)
{
    random_state = seed ? seed : 0x2545F491u;
}

/* drand48 and friends: the 48-bit linear congruence POSIX specifies */
static uint64_t rand48 = 0x1234ABCD330Eull;

static uint64_t next48(void)
{
    rand48 = (rand48 * 0x5DEECE66Dull + 0xB) & 0xFFFFFFFFFFFFull;
    return rand48;
}

double drand48(void) { return (double)next48() / 281474976710656.0; }
long lrand48(void) { return (long)(next48() >> 17); }
long mrand48(void) { return (long)(int32_t)(next48() >> 16); }
void srand48(long seed) { rand48 = ((uint64_t)(uint32_t)seed << 16) | 0x330E; }

/* ---- temporary files ---- */

/* Replace the six X's before suffixlen characters with letters; returns
 * the descriptor (or 0 for mkdtemp's directory), -1 if none could be made. */
static int make_temp(char *tmpl, int suffixlen, int kind, int flags)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t len = strlen(tmpl);
    if (len < 6 + (size_t)suffixlen || memcmp(tmpl + len - suffixlen - 6, "XXXXXX", 6)) {
        errno = EINVAL;
        return -1;
    }
    char *x = tmpl + len - suffixlen - 6;
    static uint32_t counter;
    uint32_t seed = (uint32_t)getpid() * 2654435761u ^ (uint32_t)time(NULL) ^ (counter += 7919);
    for (int tries = 0; tries < 10000; tries++) {
        uint32_t v = seed + (uint32_t)tries * 40503u;
        for (int i = 0; i < 6; i++) {
            v = v * 1103515245u + 12345u;
            x[i] = letters[(v >> 16) % 62];
        }
        if (kind == 0) {
            int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL | flags, 0600);
            if (fd >= 0)
                return fd;
        } else if (kind == 1) {
            if (mkdir(tmpl, 0700) == 0)
                return 0;
        } else {
            struct stat st;
            if (stat(tmpl, &st) < 0)
                return 0;
            continue;
        }
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

int mkstemp(char *tmpl) { return make_temp(tmpl, 0, 0, 0); }
int mkostemp(char *tmpl, int flags) { return make_temp(tmpl, 0, 0, flags & O_CLOEXEC); }
int mkstemps(char *tmpl, int suffixlen) { return make_temp(tmpl, suffixlen, 0, 0); }

char *mkdtemp(char *tmpl)
{
    return make_temp(tmpl, 0, 1, 0) < 0 ? NULL : tmpl;
}

char *mktemp(char *tmpl)
{
    if (make_temp(tmpl, 0, 2, 0) < 0)
        tmpl[0] = 0;
    return tmpl;
}

/* ---- realpath: an absolute path without ".", ".." or doubled slashes ---- */

char *realpath(const char *path, char *resolved)
{
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    if (!*path) {
        errno = ENOENT;
        return NULL;
    }
    char work[PATH_MAX];
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(work)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        strcpy(work, path);
    } else {
        if (!getcwd(work, sizeof(work)))
            return NULL;
        size_t n = strlen(work);
        if (n + 1 + strlen(path) >= sizeof(work)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (n > 1)
            work[n++] = '/';
        strcpy(work + n, path);
    }
    char out[PATH_MAX];
    size_t o = 0;
    out[o++] = '/';
    for (char *p = work; *p;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == 1 && p[0] == '.') {
            ;
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (o > 1 && out[o - 1] != '/')
                o--;
            if (o > 1)
                o--;                    /* the slash too, unless it is the root */
        } else {
            if (o > 1)
                out[o++] = '/';
            memcpy(out + o, p, len);
            o += len;
            out[o] = 0;
            struct stat st;
            if (stat(out, &st) < 0)     /* each part must exist */
                return NULL;
            if (end && !S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return NULL;
            }
        }
        p += len;
    }
    out[o] = 0;
    char *r = resolved ? resolved : malloc(o + 1);
    if (!r)
        return NULL;
    memcpy(r, out, o + 1);
    return r;
}

/* ---- system ---- */

int system(const char *command)
{
    if (!command)
        return access("/bin/sh", X_OK) == 0;
    struct sigaction ign, old_int, old_quit;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ign, &old_int);
    sigaction(SIGQUIT, &ign, &old_quit);
    int pid = fork();
    if (pid == 0) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    int status = -1;
    if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                status = -1;
                break;
            }
        }
    }
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);
    return status;
}
