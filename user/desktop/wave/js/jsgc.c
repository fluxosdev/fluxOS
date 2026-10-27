/*
 * The JavaScript engine's garbage collector: mark and sweep, conservative.
 *
 * The engine's C code keeps pointers to objects, strings and the rest in
 * its local variables, so roots are found by scanning: the C stack and
 * the coroutines' stacks (from js.c), the program's data and bss, the
 * struct js, and memory hosts register (the jsdom). Anything there that
 * points into or just past the end of a block keeps it alive; so does any
 * such word inside a block that is alive, except in strings, which are
 * allocated as having no pointers in them. Hosts that point into the heap
 * from memory the collector cannot see (DOM nodes and their wrappers)
 * hook into each collection.
 *
 * Small blocks come in size classes, a 4 KiB page of one size at a time;
 * big ones get pages of their own. A two-level map from addresses to page
 * descriptors tells what any word points at. Freed slots go back on their
 * class's free list; pages left empty go to any class that needs one.
 *
 * Collections happen as memory is allocated: when what was allocated
 * since the last one passes what was alive after it (8 MiB at least),
 * and before the scripts are told they have run out. Never while the
 * engine is parsing: the lexer keeps strings where scans cannot see.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "jsint.h"

#define PAGE        4096u
#define PAGE_SHIFT  12
#define CHUNK_PAGES 64                  /* small pages come from malloc this many at a time */
#define MAX_SMALL   2048u
#define MAX_SLOTS   (PAGE / 8)
#define MIN_GC      (8u << 20)          /* collect after this much at least */

static const uint16_t class_sizes[] = {
    8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256, 320, 384,
    512, 680, 816, 1024, 1360, 2048,
};
#define NCLASSES ((int)(sizeof(class_sizes) / sizeof(class_sizes[0])))

struct gcpage {
    struct gcheap *heap;
    char *base;
    uint32_t size;              /* small: the slot size (0: the page is empty); large: the block's */
    uint32_t nslots;
    uint8_t large, atomic, marked;      /* marked: large blocks */
    void *mem;                  /* large: what malloc gave */
    uint32_t npages;
    uint32_t alloc[MAX_SLOTS / 32], mark[MAX_SLOTS / 32];
    struct gcpage *next;        /* every page of the heap */
    struct gcpage *next_empty;
};

struct span {
    const char *p;
    size_t n;
};

struct root {
    const void *p;
    size_t n;
};

struct gcheap {
    struct gcpage *pages;
    struct gcpage *empty;               /* pages with nothing in them */
    void *free[2][NCLASSES];            /* free slots, linked through their first word */
    void **chunks;                      /* what malloc gave for small pages */
    int nchunks, chunk_cap;
    uintptr_t lo, hi;                   /* every page lies in [lo, hi) */
    size_t live;                        /* bytes in use after the last collection */
    size_t since;                       /* bytes allocated since */
    size_t threshold;
    struct root *roots;
    int nroots, root_cap;
    struct js_gc_hooks hooks;
    int has_hooks;
    struct span *stack;                 /* marking: blocks whose insides are still to scan */
    int sp, stack_cap;
    int overflow;                       /* the mark stack could not grow: rescan */
};

static uint8_t size_class[MAX_SMALL / 8 + 1];

/* ---- the page map ---- */

static struct gcpage **page_dir[1024];  /* by address >> 22: a table for each 4 MiB */

static struct gcpage *page_of(uintptr_t p)
{
    struct gcpage **t = page_dir[p >> 22];
    return t ? t[(p >> PAGE_SHIFT) & 1023] : NULL;
}

static int map_page(uintptr_t p, struct gcpage *pg)
{
    struct gcpage ***t = &page_dir[p >> 22];
    if (!*t && !(*t = calloc(1024, sizeof(**t))))
        return -1;
    (*t)[(p >> PAGE_SHIFT) & 1023] = pg;
    return 0;
}

static void cover(struct gcheap *h, uintptr_t lo, uintptr_t hi)
{
    if (!h->lo || lo < h->lo)
        h->lo = lo;
    if (hi > h->hi)
        h->hi = hi;
}

/* ---- the heap ---- */

struct gcheap *gc_new(void)
{
    if (!size_class[1]) {
        int c = 0;
        for (unsigned n = 1; n <= MAX_SMALL / 8; n++) {
            while (class_sizes[c] < n * 8)
                c++;
            size_class[n] = (uint8_t)c;
        }
    }
    struct gcheap *h = calloc(1, sizeof(*h));
    if (h)
        h->threshold = MIN_GC;
    return h;
}

/* An empty page: one left over, or a new chunk of them from malloc. */
static struct gcpage *new_page(struct gcheap *h)
{
    if (h->empty) {
        struct gcpage *pg = h->empty;
        h->empty = pg->next_empty;
        return pg;
    }
    if (h->nchunks == h->chunk_cap) {
        int cap = h->chunk_cap ? h->chunk_cap * 2 : 64;
        void **nc = realloc(h->chunks, sizeof(*nc) * (size_t)cap);
        if (!nc)
            return NULL;
        h->chunks = nc;
        h->chunk_cap = cap;
    }
    char *mem = malloc(CHUNK_PAGES * PAGE + PAGE);
    if (!mem)
        return NULL;
    h->chunks[h->nchunks++] = mem;
    char *base = (char *)(((uintptr_t)mem + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
    struct gcpage *first = NULL;
    for (unsigned i = 0; i < CHUNK_PAGES; i++) {
        struct gcpage *pg = calloc(1, sizeof(*pg));
        if (!pg || map_page((uintptr_t)base + i * PAGE, pg) < 0) {
            free(pg);
            break;
        }
        pg->heap = h;
        pg->base = base + i * PAGE;
        pg->next = h->pages;
        h->pages = pg;
        if (!first) {
            first = pg;
        } else {
            pg->next_empty = h->empty;
            h->empty = pg;
        }
    }
    cover(h, (uintptr_t)base, (uintptr_t)base + CHUNK_PAGES * PAGE);
    return first;
}

/* A page of one size class, its slots all on the free list. */
static int refill(struct gcheap *h, int atomic, int cls)
{
    struct gcpage *pg = new_page(h);
    if (!pg)
        return -1;
    pg->size = class_sizes[cls];
    pg->nslots = PAGE / pg->size;
    pg->atomic = (uint8_t)atomic;
    memset(pg->alloc, 0, sizeof(pg->alloc));
    memset(pg->mark, 0, sizeof(pg->mark));
    void *list = h->free[atomic][cls];
    for (int i = (int)pg->nslots - 1; i >= 0; i--) {
        void **slot = (void **)(pg->base + (uint32_t)i * pg->size);
        *slot = list;
        list = slot;
    }
    h->free[atomic][cls] = list;
    return 0;
}

static void *alloc_large(struct gcheap *h, size_t n, int atomic)
{
    size_t np = (n + PAGE - 1) / PAGE;
    if (np > (1u << 18))                /* a GiB: no */
        return NULL;
    char *mem = malloc(np * PAGE + PAGE);
    struct gcpage *pg = calloc(1, sizeof(*pg));
    if (!mem || !pg) {
        free(mem);
        free(pg);
        return NULL;
    }
    char *base = (char *)(((uintptr_t)mem + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
    for (size_t i = 0; i < np; i++)
        if (map_page((uintptr_t)base + i * PAGE, pg) < 0) {
            while (i--)
                map_page((uintptr_t)base + i * PAGE, NULL);
            free(mem);
            free(pg);
            return NULL;
        }
    pg->heap = h;
    pg->base = base;
    pg->size = (uint32_t)n;
    pg->large = 1;
    pg->atomic = (uint8_t)atomic;
    pg->mem = mem;
    pg->npages = (uint32_t)np;
    pg->next = h->pages;
    h->pages = pg;
    cover(h, (uintptr_t)base, (uintptr_t)base + np * PAGE);
    memset(base, 0, n);
    return base;
}

/* n bytes, zeroed. atomic: they will never hold pointers (strings). NULL if out of memory. */
void *gc_alloc(struct js *J, size_t n, int atomic)
{
    struct gcheap *h = J->heap;
    if (!n)
        n = 1;
    if (h->since > h->threshold && !J->gc_inhibit)
        gc_collect(J);
    void *p;
    size_t got;
    if (n > MAX_SMALL) {
        p = alloc_large(h, n, atomic);
        got = n;
    } else {
        int cls = size_class[(n + 7) / 8];
        void **list = &h->free[atomic][cls];
        if (!*list && refill(h, atomic, cls) < 0)
            return NULL;
        p = *list;
        *list = *(void **)p;
        struct gcpage *pg = page_of((uintptr_t)p);
        uint32_t idx = (uint32_t)((char *)p - pg->base) / pg->size;
        pg->alloc[idx >> 5] |= 1u << (idx & 31);
        got = pg->size;
        memset(p, 0, got);
    }
    if (p)
        h->since += got;
    return p;
}

size_t gc_heap_bytes(struct js *J)
{
    return J->heap->live + J->heap->since;
}

/* ---- marking ---- */

static void push(struct gcheap *h, const char *p, size_t n)
{
    if (h->sp == h->stack_cap) {
        int cap = h->stack_cap ? h->stack_cap * 2 : 4096;
        struct span *ns = realloc(h->stack, sizeof(*ns) * (size_t)cap);
        if (!ns) {
            h->overflow = 1;            /* marked, but not scanned: found again later */
            return;
        }
        h->stack = ns;
        h->stack_cap = cap;
    }
    h->stack[h->sp++] = (struct span){ p, n };
}

static void mark_slot(struct gcheap *h, struct gcpage *pg, uint32_t idx)
{
    uint32_t bit = 1u << (idx & 31);
    if (!(pg->alloc[idx >> 5] & bit) || (pg->mark[idx >> 5] & bit))
        return;
    pg->mark[idx >> 5] |= bit;
    if (!pg->atomic)
        push(h, pg->base + idx * pg->size, pg->size);
}

/* A word that may be a pointer: mark what it points into, or just past. */
static void mark_word(struct gcheap *h, uintptr_t w)
{
    if (w < h->lo || w >= h->hi)
        return;
    struct gcpage *pg = page_of(w);
    if (!pg || pg->heap != h)
        return;
    if (pg->large) {
        if (w > (uintptr_t)pg->base + pg->size || pg->marked)
            return;
        pg->marked = 1;
        if (!pg->atomic)
            push(h, pg->base, pg->size);
        return;
    }
    if (!pg->size)
        return;
    uint32_t off = (uint32_t)(w - (uintptr_t)pg->base), idx = off / pg->size;
    if (idx < pg->nslots)
        mark_slot(h, pg, idx);
    if (idx && idx <= pg->nslots && off == idx * pg->size)
        mark_slot(h, pg, idx - 1);
}

static void scan_range(struct gcheap *h, const void *lo, const void *hi)
{
    const uintptr_t *w = (const uintptr_t *)(((uintptr_t)lo + 3) & ~(uintptr_t)3);
    for (; (const char *)(w + 1) <= (const char *)hi; w++)
        mark_word(h, *w);
}

static void scan_cb(struct js *J, const void *lo, const void *hi)
{
    scan_range(J->heap, lo, hi);
}

static void drain(struct gcheap *h)
{
    for (;;) {
        while (h->sp) {
            struct span s = h->stack[--h->sp];
            scan_range(h, s.p, s.p + s.n);
        }
        if (!h->overflow)
            return;
        /* Some marked blocks were never scanned: scan every marked block again. */
        h->overflow = 0;
        for (struct gcpage *pg = h->pages; pg; pg = pg->next) {
            if (pg->atomic)
                continue;
            if (pg->large) {
                if (pg->marked)
                    scan_range(h, pg->base, pg->base + pg->size);
                continue;
            }
            for (uint32_t i = 0; i < pg->nslots && pg->size; i++)
                if (pg->mark[i >> 5] & (1u << (i & 31)))
                    scan_range(h, pg->base + i * pg->size, pg->base + (i + 1) * pg->size);
        }
    }
}

extern char __gc_data_start[], __gc_data_end[];

/* Scan everything from this frame up: mark_roots's, with the registers it saved, and above. */
static int __attribute__((noinline)) scan_roots(struct js *J)
{
    struct gcheap *h = J->heap;
    const char *sp = __builtin_frame_address(0);
    if (js_gc_scan_stacks(J, sp, scan_cb) < 0)
        return -1;
    scan_range(h, __gc_data_start, __gc_data_end);
    scan_range(h, J, J + 1);
    for (int i = 0; i < h->nroots; i++)
        scan_range(h, h->roots[i].p, (const char *)h->roots[i].p + h->roots[i].n);
    return 0;
}

/*
 * The roots. Every register a caller might keep a pointer in is saved in
 * this frame first, and scan_roots's frame lies below it, so the scan
 * sees them. (Not a tail call: that would give the registers back first.)
 */
static int __attribute__((noinline)) mark_roots(struct js *J)
{
    __builtin_unwind_init();
    volatile int r = scan_roots(J);
    return r;
}

void js_gc_mark(struct js *J, const void *p)
{
    mark_word(J->heap, (uintptr_t)p);
}

int js_gc_marked(struct js *J, const void *p)
{
    struct gcpage *pg = page_of((uintptr_t)p);
    if (!pg || pg->heap != J->heap)
        return 1;                       /* not ours: as good as alive */
    if (pg->large)
        return pg->marked;
    if (!pg->size)
        return 0;
    uint32_t idx = (uint32_t)((const char *)p - pg->base) / pg->size;
    return idx < pg->nslots && (pg->mark[idx >> 5] & (1u << (idx & 31)));
}

/* ---- sweeping ---- */

static uint32_t bits_set(uint32_t v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}

static void sweep(struct gcheap *h)
{
    memset(h->free, 0, sizeof(h->free));
    size_t live = 0;
    struct gcpage **pp = &h->pages;
    while (*pp) {
        struct gcpage *pg = *pp;
        if (pg->large) {
            if (pg->marked) {
                pg->marked = 0;
                live += pg->size;
                pp = &pg->next;
                continue;
            }
            *pp = pg->next;
            for (uint32_t i = 0; i < pg->npages; i++)
                map_page((uintptr_t)pg->base + i * PAGE, NULL);
            free(pg->mem);
            free(pg);
            continue;
        }
        pp = &pg->next;
        if (!pg->size)
            continue;                   /* already empty */
        uint32_t nlive = 0;
        for (int k = 0; k < (int)(MAX_SLOTS / 32); k++) {
            pg->alloc[k] &= pg->mark[k];
            nlive += bits_set(pg->alloc[k]);
            pg->mark[k] = 0;
        }
        if (!nlive) {
            pg->size = 0;
            pg->nslots = 0;
            pg->next_empty = h->empty;
            h->empty = pg;
            continue;
        }
        live += nlive * pg->size;
        int cls = size_class[pg->size / 8];
        void *list = h->free[pg->atomic][cls];
        for (int i = (int)pg->nslots - 1; i >= 0; i--) {
            if (pg->alloc[i >> 5] & (1u << (i & 31)))
                continue;
            void **slot = (void **)(pg->base + (uint32_t)i * pg->size);
            *slot = list;
            list = slot;
        }
        h->free[pg->atomic][cls] = list;
    }
    h->live = live;
}

void gc_collect(struct js *J)
{
    struct gcheap *h = J->heap;
    if (!h || J->gc_inhibit)
        return;
    J->gc_inhibit++;                    /* hooks must not allocate; nor start another */
    js_gc_before(J);
    h->sp = 0;
    h->overflow = 0;
    if (mark_roots(J) < 0) {
        /* Running on a stack it does not know: unmark and leave it for later. */
        for (struct gcpage *pg = h->pages; pg; pg = pg->next) {
            pg->marked = 0;
            memset(pg->mark, 0, sizeof(pg->mark));
        }
        h->since = 0;
        J->gc_inhibit--;
        return;
    }
    drain(h);
    if (h->has_hooks) {
        if (h->hooks.mark)
            h->hooks.mark(J, h->hooks.ctx);
        drain(h);
        while (h->hooks.more && h->hooks.more(J, h->hooks.ctx))
            drain(h);
        if (h->hooks.sweep)
            h->hooks.sweep(J, h->hooks.ctx);
    }
    sweep(h);
    h->since = 0;
    h->threshold = h->live > MIN_GC ? h->live : MIN_GC;
    J->gc_inhibit--;
}

void gc_destroy(struct js *J)
{
    struct gcheap *h = J->heap;
    if (!h)
        return;
    while (h->pages) {
        struct gcpage *pg = h->pages;
        h->pages = pg->next;
        if (pg->large) {
            for (uint32_t i = 0; i < pg->npages; i++)
                map_page((uintptr_t)pg->base + i * PAGE, NULL);
            free(pg->mem);
        } else {
            map_page((uintptr_t)pg->base, NULL);
        }
        free(pg);
    }
    for (int i = 0; i < h->nchunks; i++)
        free(h->chunks[i]);
    free(h->chunks);
    free(h->roots);
    free(h->stack);
    free(h);
    J->heap = NULL;
}

/* ---- for hosts ---- */

void js_gc_root(struct js *J, const void *p, size_t n)
{
    struct gcheap *h = J->heap;
    if (h->nroots == h->root_cap) {
        int cap = h->root_cap ? h->root_cap * 2 : 8;
        struct root *nr = realloc(h->roots, sizeof(*nr) * (size_t)cap);
        if (!nr) {
            J->gc_inhibit++;            /* cannot keep it alive: never collect */
            return;
        }
        h->roots = nr;
        h->root_cap = cap;
    }
    h->roots[h->nroots++] = (struct root){ p, n };
}

void js_gc_unroot(struct js *J, const void *p)
{
    struct gcheap *h = J->heap;
    for (int i = 0; i < h->nroots; i++)
        if (h->roots[i].p == p) {
            h->roots[i] = h->roots[--h->nroots];
            return;
        }
}

void js_gc_hooks(struct js *J, const struct js_gc_hooks *hooks)
{
    J->heap->hooks = *hooks;
    J->heap->has_hooks = 1;
}

void js_gc_pause(struct js *J, int pause)
{
    J->gc_inhibit += pause ? 1 : -1;
}
