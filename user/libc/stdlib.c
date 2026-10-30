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
#include "pthread_impl.h"

char **environ;

void __stdio_init(void);
int main(int argc, char **argv, char **envp);

/* atexit() and __cxa_atexit() (what C++ registers static destructors with): run in reverse. */
struct exit_fn {
    void (*fn)(void *);
    void *arg;
    void *dso;
};
static struct exit_fn *atexit_fns;
static int atexit_count, atexit_cap;

/* Constructors and destructors from the linker's .init_array and .fini_array (user.ld, or
 * the linker's own script): what GCC's crtbegin.o and C++ static objects put there. */
typedef void (*init_fn)(int, char **, char **);
extern init_fn __preinit_array_start[] __attribute__((weak, visibility("hidden")));
extern init_fn __preinit_array_end[] __attribute__((weak, visibility("hidden")));
extern init_fn __init_array_start[] __attribute__((weak, visibility("hidden")));
extern init_fn __init_array_end[] __attribute__((weak, visibility("hidden")));
extern void (*__fini_array_start[])(void) __attribute__((weak, visibility("hidden")));
extern void (*__fini_array_end[])(void) __attribute__((weak, visibility("hidden")));

static void run_init_array(init_fn *start, init_fn *end, int argc, char **argv, char **envp)
{
    if (!start || !end)
        return;
    for (init_fn *f = start; f < end; f++)
        (*f)(argc, argv, envp);
}

/* The program's name, for err() and warn(): argv[0] without its directory. */
const char *__progname = "";
char *program_invocation_name = "";
char *program_invocation_short_name = "";

void __libc_start(int argc, char **argv, char **envp)
{
    __init_tls();                       /* errno is the thread's: before anything can set it */
    environ = envp;
    if (argc > 0 && argv[0]) {
        const char *slash = strrchr(argv[0], '/');
        __progname = slash ? slash + 1 : argv[0];
        program_invocation_name = argv[0];
        program_invocation_short_name = (char *)__progname;
    }
    __stdio_init();
    run_init_array(__preinit_array_start, __preinit_array_end, argc, argv, envp);
    run_init_array(__init_array_start, __init_array_end, argc, argv, envp);
    exit(main(argc, argv, envp));
}

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    if (atexit_count == atexit_cap) {
        int cap = atexit_cap ? atexit_cap * 2 : 32;
        struct exit_fn *n = realloc(atexit_fns, sizeof(*n) * (size_t)cap);
        if (!n)
            return -1;
        atexit_fns = n;
        atexit_cap = cap;
    }
    atexit_fns[atexit_count].fn = fn;
    atexit_fns[atexit_count].arg = arg;
    atexit_fns[atexit_count].dso = dso;
    atexit_count++;
    return 0;
}

int atexit(void (*fn)(void))
{
    return __cxa_atexit((void (*)(void *))fn, NULL, NULL);
}

/* Run what was registered for dso (every one when NULL), latest first. */
void __cxa_finalize(void *dso)
{
    for (int i = atexit_count - 1; i >= 0; i--) {
        if (dso && atexit_fns[i].dso != dso)
            continue;
        struct exit_fn e = atexit_fns[i];
        atexit_fns[i].fn = NULL;
        if (e.fn)
            e.fn(e.arg);
    }
    if (!dso)
        atexit_count = 0;
}

void exit(int status)
{
    __cxa_finalize(NULL);
    if (__fini_array_start && __fini_array_end)
        for (void (**f)(void) = __fini_array_end; f > __fini_array_start; f--)
            (*(f - 1))();
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

/* Threads take turns at the heap (the lock is one its holder may take again: memalign and realloc
 * call malloc and free). */
struct __rlock __malloc_lock;

static void *malloc_locked(size_t n)
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

static void free_locked(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)((char *)p - HDR);
    if (!(b->head & INUSE))
        return;                         /* freed already */
    release(b);
}

/* A block whose payload is align-aligned: a bigger one, with the part
 * before the aligned payload split off and freed. */
static void *memalign_locked(size_t align, size_t n)
{
    if (align <= ALIGN)
        return malloc(n);
    if (align & (align - 1)) {
        errno = EINVAL;
        return NULL;
    }
    size_t size = block_size(n ? n : 1);
    if (!size || n > MAX_REQ - align - MIN_BLOCK) {
        errno = ENOMEM;
        return NULL;
    }
    char *p = malloc(n + align + MIN_BLOCK);
    if (!p)
        return NULL;
    struct block *b = (struct block *)(p - HDR);
    if (!((uintptr_t)p & (align - 1))) {
        trim(b, size);
        return p;
    }
    /* at least MIN_BLOCK in, so the part before is a block of its own */
    char *q = (char *)(((uintptr_t)p + MIN_BLOCK + align - 1) & ~(uintptr_t)(align - 1));
    struct block *nb = (struct block *)(q - HDR);
    size_t lead = (size_t)((char *)nb - (char *)b);
    nb->head = (bsize(b) - lead) | INUSE;
    b->head = lead | INUSE | (b->head & PINUSE);
    release(b);                         /* marks nb's PINUSE clear, merges b downwards */
    trim(nb, size);
    return q;
}

void *malloc(size_t n)
{
    __rlock_take(&__malloc_lock);
    void *p = malloc_locked(n);
    __rlock_give(&__malloc_lock);
    return p;
}

void free(void *p)
{
    __rlock_take(&__malloc_lock);
    free_locked(p);
    __rlock_give(&__malloc_lock);
}

void *memalign(size_t align, size_t n)
{
    __rlock_take(&__malloc_lock);
    void *p = memalign_locked(align, n);
    __rlock_give(&__malloc_lock);
    return p;
}

int posix_memalign(void **out, size_t align, size_t n)
{
    if (align < sizeof(void *) || (align & (align - 1)))
        return EINVAL;
    int saved = errno;
    void *p = memalign(align, n);
    if (!p) {
        int e = errno;
        errno = saved;
        return e;
    }
    *out = p;
    return 0;
}

void *aligned_alloc(size_t align, size_t n)
{
    return memalign(align, n);
}

void *valloc(size_t n)
{
    return memalign(4096, n);
}

void *reallocarray(void *p, size_t count, size_t size)
{
    if (size && count > (size_t)-1 / size) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(p, count * size);
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

static void *realloc_locked(void *p, size_t n)
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

void *realloc(void *p, size_t n)
{
    __rlock_take(&__malloc_lock);
    void *q = realloc_locked(p, n);
    __rlock_give(&__malloc_lock);
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
