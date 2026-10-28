/*
 * Program startup/exit, the malloc family, the environment and number
 * conversion.
 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char **environ;

void __stdio_init(void);
int main(int argc, char **argv, char **envp);

#define MAX_ATEXIT 16
static void (*atexit_fns[MAX_ATEXIT])(void);
static int atexit_count;

void __libc_start(int argc, char **argv, char **envp)
{
    environ = envp;
    __stdio_init();
    exit(main(argc, argv, envp));
}

int atexit(void (*fn)(void))
{
    if (atexit_count >= MAX_ATEXIT)
        return -1;
    atexit_fns[atexit_count++] = fn;
    return 0;
}

void exit(int status)
{
    while (atexit_count > 0)
        atexit_fns[--atexit_count]();
    fflush(NULL);
    _exit(status);
}

void abort(void)
{
    fflush(NULL);
    raise(SIGABRT);
    _exit(127);
}

/*
 * ---- malloc: boundary tags and size-class bins ----
 *
 * Blocks are 16-byte multiples with a 16-byte header, so payloads stay
 * 16-byte aligned. The header holds the block's size and two flags: in
 * use, and whether the block just below is in use. A free block also
 * keeps its size in its last word (a footer) and its list links in its
 * payload, so free() finds both neighbours at once and merges with them:
 * no two free blocks are ever adjacent. Free blocks wait in bins by size,
 * exact below 1 KiB and four to each power of two above, with a bitmap
 * of the bins in use: malloc() looks at one bin, then jumps to the next
 * non-empty one. Neither walks the heap, which a long-running program
 * (the desktop) makes large.
 *
 * Each stretch from sbrk() ends in a fence, a header marking 16 bytes in
 * use, so merging stops there; the next stretch, if it follows on, takes
 * the fence's place.
 */

#define ALIGN     16u
#define HDR       16u                   /* header, before the payload */
#define MIN_BLOCK 32u                   /* header, two links and a footer */
#define CHUNK     (64u * 1024)
#define INUSE     1u
#define PINUSE    2u                    /* the block below is in use */
#define SMALL     1024u                 /* exact bins below this */
#define NBINS     160
#define MAX_REQ   0x7FF00000u           /* what sbrk(int) can be asked for */

struct block {
    size_t head;                        /* size | INUSE | PINUSE */
    uint32_t pad[3];
    struct block *next, *prev;          /* free blocks only: their bin */
};

/* The links fit in the smallest block, ahead of its footer. */
_Static_assert(HDR + 2 * sizeof(void *) + sizeof(size_t) <= MIN_BLOCK, "block too small");

static struct block *bins[NBINS];
static uint32_t binmap[(NBINS + 31) / 32];
static struct block *fence;             /* the end of the latest stretch */

static inline size_t bsize(const struct block *b) { return b->head & ~(size_t)(ALIGN - 1); }
static inline struct block *after(struct block *b) { return (struct block *)((char *)b + bsize(b)); }
static inline void set_footer(struct block *b) { *(size_t *)((char *)b + bsize(b) - sizeof(size_t)) = bsize(b); }

static int bin_of(size_t size)
{
    if (size < SMALL)
        return (int)(size >> 4);
    int lg = 31 - __builtin_clz((unsigned)size);
    int i = 64 + (lg - 10) * 4 + (int)((size >> (lg - 2)) & 3);
    return i < NBINS ? i : NBINS - 1;
}

static void bin_insert(struct block *b)
{
    int i = bin_of(bsize(b));
    b->prev = NULL;
    b->next = bins[i];
    if (b->next)
        b->next->prev = b;
    bins[i] = b;
    binmap[i / 32] |= 1u << (i % 32);
}

static void bin_remove(struct block *b)
{
    int i = bin_of(bsize(b));
    if (b->prev)
        b->prev->next = b->next;
    else
        bins[i] = b->next;
    if (b->next)
        b->next->prev = b->prev;
    if (!bins[i])
        binmap[i / 32] &= ~(1u << (i % 32));
}

/* The first non-empty bin from i on, or -1. */
static int bin_next(int i)
{
    for (int w = i / 32; w < (NBINS + 31) / 32; w++) {
        uint32_t m = binmap[w];
        if (w == i / 32)
            m &= ~0u << (i % 32);
        if (m)
            return w * 32 + __builtin_ctz(m);
    }
    return -1;
}

/* b (in use, any size) becomes free: merge with free neighbours and bin it. */
static void release(struct block *b)
{
    size_t size = bsize(b);
    struct block *n = after(b);
    if (!(n->head & INUSE)) {
        bin_remove(n);
        size += bsize(n);
    }
    if (!(b->head & PINUSE)) {
        size_t psize = *(size_t *)((char *)b - sizeof(size_t));
        b = (struct block *)((char *)b - psize);
        bin_remove(b);
        size += psize;
    }
    b->head = size | PINUSE;            /* what is below a free block is in use */
    set_footer(b);
    after(b)->head &= ~PINUSE;
    bin_insert(b);
}

/* Room for at least `need` more: a free block from sbrk(). */
static int grow(size_t need)
{
    size_t total = need < CHUNK ? CHUNK : need;
    if (total > MAX_REQ - HDR)
        return -1;
    char *p = sbrk((int)(total + HDR));
    if (p == (void *)-1)
        return -1;
    struct block *b;
    if (fence && (char *)fence + HDR == p) {
        b = fence;                      /* follows on: the old fence is the new block's header */
        b->head = (total + HDR) | INUSE | (fence->head & PINUSE);
    } else {
        b = (struct block *)p;          /* a new stretch: nothing below to merge with */
        b->head = total | INUSE | PINUSE;
    }
    fence = after(b);
    fence->head = HDR | INUSE | PINUSE;
    release(b);
    return 0;
}

/* The block size for n bytes of payload, or 0 if too big. */
static size_t block_size(size_t n)
{
    if (n > MAX_REQ - 2 * HDR)
        return 0;
    size_t size = (n + HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    return size < MIN_BLOCK ? MIN_BLOCK : size;
}

/* b (in use) keeps `size` bytes; the rest, if a block's worth, is freed. */
static void trim(struct block *b, size_t size)
{
    size_t rest = bsize(b) - size;
    if (rest < MIN_BLOCK)
        return;
    b->head = size | (b->head & (INUSE | PINUSE));
    struct block *r = after(b);
    r->head = rest | INUSE | PINUSE;
    release(r);
}

static struct block *find(size_t size)
{
    int i = bin_of(size);
    if (size >= SMALL)                  /* a range: look for one that fits */
        for (struct block *b = bins[i]; b; b = b->next)
            if (bsize(b) >= size)
                return b;
    if (size < SMALL && bins[i])
        return bins[i];
    i = bin_next(i + 1);                /* anything in a bigger bin fits */
    return i < 0 ? NULL : bins[i];
}

void *malloc(size_t n)
{
    size_t size = block_size(n ? n : 1);
    if (!size) {
        errno = ENOMEM;
        return NULL;
    }
    struct block *b = find(size);
    if (!b) {
        if (grow(size) < 0) {
            errno = ENOMEM;
            return NULL;
        }
        b = find(size);
    }
    bin_remove(b);
    b->head |= INUSE;
    after(b)->head |= PINUSE;
    trim(b, size);
    return (char *)b + HDR;
}

void free(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)((char *)p - HDR);
    if (!(b->head & INUSE))
        return;                         /* freed already */
    release(b);
}

void *calloc(size_t count, size_t size)
{
    if (size && count > (size_t)-1 / size)
        return NULL;
    void *p = malloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return NULL;
    }
    struct block *b = (struct block *)((char *)p - HDR);
    size_t size = block_size(n);
    if (!size) {
        errno = ENOMEM;
        return NULL;
    }
    if (bsize(b) >= size) {
        trim(b, size);
        return p;
    }
    struct block *nb = after(b);        /* grow into a free block above */
    if (!(nb->head & INUSE) && bsize(b) + bsize(nb) >= size) {
        bin_remove(nb);
        b->head += bsize(nb);
        after(b)->head |= PINUSE;
        trim(b, size);
        return p;
    }
    void *q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, bsize(b) - HDR);
    free(p);
    return q;
}

/*
 * The environment starts on the stack, as the kernel laid it out. Before
 * the first change it is moved to the heap, array and strings alike, so
 * that entries can be freed when they are replaced or removed; the array
 * grows by doubling. A program that points environ at an array of its
 * own gets that array left alone (and copied again on the next change).
 */
static char **owned_env;                /* the array we allocated, if environ still is it */
static int env_cap;

static int env_count(void)
{
    int n = 0;
    while (environ && environ[n])
        n++;
    return n;
}

/* Room for `extra` more entries in an environ that is ours. */
static int env_own(int extra)
{
    int n = env_count();
    int owned = environ && environ == owned_env;
    if (owned && n + extra + 1 <= env_cap)
        return 0;
    int cap = n + extra + 1;
    if (owned && cap < 2 * env_cap)
        cap = 2 * env_cap;
    if (cap < 16)
        cap = 16;
    char **e = malloc(sizeof(char *) * (size_t)cap);
    if (!e)
        return -1;
    for (int i = 0; i < n; i++) {
        e[i] = owned ? environ[i] : strdup(environ[i]);
        if (!e[i]) {
            while (i-- > 0)
                free(e[i]);
            free(e);
            return -1;
        }
    }
    e[n] = NULL;
    if (owned)
        free(environ);
    environ = owned_env = e;
    env_cap = cap;
    return 0;
}

static int env_find(const char *name, size_t len)
{
    for (int i = 0; environ && environ[i]; i++)
        if (!strncmp(environ[i], name, len) && environ[i][len] == '=')
            return i;
    return -1;
}

char *getenv(const char *name)
{
    int i = env_find(name, strlen(name));
    return i < 0 ? NULL : environ[i] + strlen(name) + 1;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t nlen = strlen(name);
    if (!nlen || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    int i = env_find(name, nlen);
    if (i >= 0 && !overwrite)
        return 0;
    char *entry = malloc(nlen + strlen(value) + 2);
    if (!entry)
        return -1;
    strcpy(entry, name);
    entry[nlen] = '=';
    strcpy(entry + nlen + 1, value);
    if (env_own(1) < 0) {
        free(entry);
        return -1;
    }
    if (i >= 0) {
        free(environ[i]);               /* ours, once env_own() has run */
        environ[i] = entry;
    } else {
        int n = env_count();
        environ[n] = entry;
        environ[n + 1] = NULL;
    }
    return 0;
}

int unsetenv(const char *name)
{
    int i = env_find(name, strlen(name));
    if (i < 0)
        return 0;
    if (env_own(0) < 0)
        return -1;
    free(environ[i]);
    for (; environ[i]; i++)
        environ[i] = environ[i + 1];
    return 0;
}

int putenv(char *str)
{
    char *eq = strchr(str, '=');
    if (!eq)
        return unsetenv(str);
    char name[128];
    size_t len = (size_t)(eq - str);
    if (len >= sizeof(name))
        return -1;
    memcpy(name, str, len);
    name[len] = 0;
    return setenv(name, eq + 1, 1);
}

/* ---- numbers ---- */

unsigned long strtoul(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace(*p))
        p++;
    if (*p == '+')
        p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    unsigned long v = 0;
    const char *start = p;
    for (;; p++) {
        int d;
        if (isdigit(*p)) d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base)
            break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end)
        *end = (char *)(p == start ? s : p);
    return v;
}

long strtol(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace(*p))
        p++;
    int neg = 0;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    char *e;
    unsigned long v = strtoul(p, &e, base);
    if (end)
        *end = e == p ? (char *)s : e;
    return neg ? -(long)v : (long)v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

static unsigned int rand_state = 1;

int rand(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 1) & RAND_MAX);
}

void srand(unsigned int seed) { rand_state = seed; }

static void swap(char *x, char *y, size_t size)
{
    char tmp[64];
    while (size) {
        size_t k = size < sizeof(tmp) ? size : sizeof(tmp);
        memcpy(tmp, x, k);
        memcpy(x, y, k);
        memcpy(y, tmp, k);
        x += k;
        y += k;
        size -= k;
    }
}

/* Shell sort: small, in-place and good enough for directory listings. */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *a = base;
    for (size_t gap = n / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < n; i++)
            for (size_t j = i; j >= gap && cmp(a + (j - gap) * size, a + j * size) > 0; j -= gap)
                swap(a + (j - gap) * size, a + j * size, size);
}
