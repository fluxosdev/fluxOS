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

/* ---- malloc: address-ordered free list with coalescing ---- */

#define ALIGN 16
#define CHUNK (64 * 1024)

struct header {
    size_t size;                /* payload bytes */
    int free;
    struct header *next;
    uint32_t pad;
};

static struct header *heap_first, *heap_last;

static struct header *grow(size_t size)
{
    size_t total = size + sizeof(struct header);
    if (total < CHUNK)
        total = CHUNK;
    struct header *h = sbrk((int)total);
    if (h == (void *)-1)
        return NULL;
    h->size = total - sizeof(struct header);
    h->free = 1;
    h->next = NULL;
    if (heap_last && heap_last->free &&
        (char *)(heap_last + 1) + heap_last->size == (char *)h) {
        heap_last->size += total;
        return heap_last;
    }
    if (heap_last)
        heap_last->next = h;
    else
        heap_first = h;
    heap_last = h;
    return h;
}

static void split(struct header *h, size_t size)
{
    if (h->size < size + sizeof(struct header) + ALIGN)
        return;
    struct header *n = (struct header *)((char *)(h + 1) + size);
    n->size = h->size - size - sizeof(struct header);
    n->free = 1;
    n->next = h->next;
    h->next = n;
    h->size = size;
    if (heap_last == h)
        heap_last = n;
}

void *malloc(size_t n)
{
    if (!n)
        n = 1;
    size_t size = (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    struct header *h;
    for (h = heap_first; h; h = h->next)
        if (h->free && h->size >= size)
            break;
    if (!h) {
        h = grow(size);
        if (!h) {
            errno = ENOMEM;
            return NULL;
        }
    }
    split(h, size);
    h->free = 0;
    return h + 1;
}

void free(void *p)
{
    if (!p)
        return;
    struct header *h = (struct header *)p - 1;
    h->free = 1;
    /* Merge with physically adjacent free neighbours. */
    for (struct header *c = heap_first; c; c = c->next) {
        while (c->free && c->next && c->next->free &&
               (char *)(c + 1) + c->size == (char *)c->next) {
            if (heap_last == c->next)
                heap_last = c;
            c->size += sizeof(struct header) + c->next->size;
            c->next = c->next->next;
        }
    }
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
    struct header *h = (struct header *)p - 1;
    if (h->size >= n)
        return p;
    void *q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, h->size);
    free(p);
    return q;
}

/* ---- environment ---- */

static int env_owned;

static int env_count(void)
{
    int n = 0;
    while (environ && environ[n])
        n++;
    return n;
}

/* Move environ to the heap before the first modification. */
static int env_own(int extra)
{
    int n = env_count();
    char **e = malloc(sizeof(char *) * (size_t)(n + extra + 1));
    if (!e)
        return -1;
    for (int i = 0; i < n; i++)
        e[i] = environ[i];
    e[n] = NULL;
    if (env_owned)
        free(environ);
    environ = e;
    env_owned = 1;
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
