/*
 * Windows heaps (HeapCreate, HeapAlloc...), in the program's memory. A
 * heap is one reservation, used from the bottom up; blocks carry a
 * header with their size and that of the block before them, and free
 * ones are kept on lists by size class and merged with free neighbours.
 * Blocks are 8-byte aligned, as the Windows heap's are.
 */
#include <stdio.h>
#include <string.h>
#include "win32.h"

#define HEAP_MAGIC 0x50414548u          /* "HEAP" */
#define BLOCK_USED 1u
#define NCLASSES 32
#define HDR 8u
#define MIN_BLOCK 24u                   /* header, the free links, 8 bytes of payload */

struct heap {
    uint32_t magic;
    uint32_t flags;
    gp_t base;
    uint32_t size;                      /* the reservation */
    uint32_t max;                       /* the limit asked for (0: none) */
    uint32_t top;                       /* first unused byte, relative to base */
    gp_t last;                          /* the last block, or 0 */
    gp_t free_lists[NCLASSES];
    uint32_t allocs;
};

struct block {
    uint32_t size_flags;                /* size including the header; bit 0: in use */
    uint32_t prev_size;                 /* the block before (0: this is the first) */
    gp_t next_free, prev_free;          /* while free */
};

gp_t process_heap;

static int size_class(uint32_t size)
{
    if (size <= 256)
        return (int)((size - 1) / 16);
    int c = 16;
    uint32_t s = 512;
    while (s < size && c < NCLASSES - 1) {
        s <<= 1;
        c++;
    }
    return c;
}

static struct heap *H(gp_t h)
{
    if (h < GUEST_LOW || h >= GUEST_TOP || (h & 7))
        return NULL;
    struct heap *hp = GP(h);
    return hp->magic == HEAP_MAGIC ? hp : NULL;
}

static struct block *B(gp_t b) { return GP(b); }
static uint32_t bsize(gp_t b) { return B(b)->size_flags & ~7u; }
static int bused(gp_t b) { return B(b)->size_flags & BLOCK_USED; }
static uint32_t first_off(void) { return (sizeof(struct heap) + 15) & ~15u; }

static gp_t next_block(struct heap *hp, gp_t b)
{
    gp_t n = b + bsize(b);
    return n < hp->base + hp->top ? n : 0;
}

static gp_t prev_block(gp_t b)
{
    uint32_t ps = B(b)->prev_size;
    return ps ? b - ps : 0;
}

static void free_insert(struct heap *hp, gp_t b)
{
    struct block *blk = B(b);
    int c = size_class(bsize(b));
    blk->size_flags &= ~BLOCK_USED;
    blk->prev_free = 0;
    blk->next_free = hp->free_lists[c];
    if (blk->next_free)
        B(blk->next_free)->prev_free = b;
    hp->free_lists[c] = b;
}

static void free_remove(struct heap *hp, gp_t b)
{
    struct block *blk = B(b);
    int c = size_class(bsize(b));
    if (blk->prev_free)
        B(blk->prev_free)->next_free = blk->next_free;
    else
        hp->free_lists[c] = blk->next_free;
    if (blk->next_free)
        B(blk->next_free)->prev_free = blk->prev_free;
    blk->next_free = blk->prev_free = 0;
}

gp_t heap_create(uint32_t initial, uint32_t max, uint32_t flags)
{
    uint32_t reserve = max ? (max + 0xFFFFu) & ~0xFFFFu : 0x4000000u;    /* 64 MiB to grow into */
    if (reserve < 0x10000)
        reserve = 0x10000;
    gp_t base = vm_alloc(0, reserve, PAGE_READWRITE, MEM_PRIVATE, 0);
    while (!base && reserve > 0x100000) {
        reserve >>= 1;
        base = vm_alloc(0, reserve, PAGE_READWRITE, MEM_PRIVATE, 0);
    }
    if (!base)
        return 0;
    struct heap *hp = GP(base);
    memset(hp, 0, sizeof(*hp));
    hp->magic = HEAP_MAGIC;
    hp->flags = flags;
    hp->base = base;
    hp->size = reserve;
    hp->max = max;
    hp->top = first_off();
    (void)initial;
    return base;
}

int heap_destroy(gp_t h)
{
    struct heap *hp = H(h);
    if (!hp || h == process_heap)
        return -1;
    hp->magic = 0;
    vm_free(h, 0);
    return 0;
}

gp_t heap_alloc(gp_t h, uint32_t size, int zero)
{
    struct heap *hp = H(h);
    if (!hp || size > 0x7FFF0000u)
        return 0;
    uint32_t need = size + HDR;
    if (need < MIN_BLOCK)
        need = MIN_BLOCK;
    need = (need + 7) & ~7u;
    gp_t b = 0;
    for (int c = size_class(need); c < NCLASSES && !b; c++) {
        for (gp_t f = hp->free_lists[c]; f; f = B(f)->next_free) {
            if (bsize(f) >= need) {
                free_remove(hp, f);
                b = f;
                break;
            }
        }
    }
    if (b) {
        uint32_t have = bsize(b);
        if (have >= need + MIN_BLOCK) {     /* split: the rest stays free */
            gp_t rest = b + need;
            B(rest)->size_flags = have - need;
            B(rest)->prev_size = need;
            B(b)->size_flags = need;
            gp_t after = next_block(hp, rest);
            if (after)
                B(after)->prev_size = have - need;
            if (hp->last == b)
                hp->last = rest;
            free_insert(hp, rest);
        }
        B(b)->size_flags |= BLOCK_USED;
    } else {
        if (hp->top + need > hp->size)
            return 0;
        b = hp->base + hp->top;
        B(b)->size_flags = need | BLOCK_USED;
        B(b)->prev_size = hp->last ? bsize(hp->last) : 0;
        hp->last = b;
        hp->top += need;
    }
    hp->allocs++;
    gp_t p = b + HDR;
    if (zero)
        memset(GP(p), 0, bsize(b) - HDR);
    return p;
}

static int valid_block(struct heap *hp, gp_t p)
{
    if (p < hp->base + first_off() + HDR || p >= hp->base + hp->top || (p & 7))
        return 0;
    gp_t b = p - HDR;
    uint32_t s = bsize(b);
    return s >= MIN_BLOCK && b + s <= hp->base + hp->top && bused(b);
}

int heap_free(gp_t h, gp_t p)
{
    struct heap *hp = H(h);
    if (!hp)
        return -1;
    if (!p)
        return 0;
    if (!valid_block(hp, p)) {
        wlog("HeapFree: bad pointer %08x", p);
        return -1;
    }
    gp_t b = p - HDR;
    B(b)->size_flags &= ~BLOCK_USED;
    gp_t n = next_block(hp, b);
    if (n && !bused(n)) {
        free_remove(hp, n);
        B(b)->size_flags = bsize(b) + bsize(n);
        if (hp->last == n)
            hp->last = b;
    }
    gp_t pv = prev_block(b);
    if (pv && !bused(pv)) {
        free_remove(hp, pv);
        B(pv)->size_flags = bsize(pv) + bsize(b);
        if (hp->last == b)
            hp->last = pv;
        b = pv;
    }
    n = next_block(hp, b);
    if (n)
        B(n)->prev_size = bsize(b);
    if (hp->last == b) {                /* at the top: give it back */
        hp->top = b - hp->base;
        hp->last = prev_block(b);
    } else {
        free_insert(hp, b);
    }
    hp->allocs--;
    return 0;
}

uint32_t heap_size(gp_t h, gp_t p)
{
    struct heap *hp = H(h);
    if (!hp || !valid_block(hp, p))
        return 0xFFFFFFFFu;
    return bsize(p - HDR) - HDR;
}

int heap_validate(gp_t h, gp_t p)
{
    struct heap *hp = H(h);
    if (!hp)
        return 0;
    return p ? valid_block(hp, p) : 1;
}

gp_t heap_realloc(gp_t h, gp_t p, uint32_t size, int zero, int in_place)
{
    struct heap *hp = H(h);
    if (!hp)
        return 0;
    if (!p)
        return heap_alloc(h, size, zero);
    if (!valid_block(hp, p))
        return 0;
    uint32_t old = bsize(p - HDR) - HDR;
    if (size <= old) {
        if (zero && size < old)
            memset((char *)GP(p) + size, 0, old - size);
        return p;
    }
    if (in_place)
        return 0;
    gp_t q = heap_alloc(h, size, 0);
    if (!q)
        return 0;
    memcpy(GP(q), GP(p), old);
    if (zero)
        memset((char *)GP(q) + old, 0, size - old);
    heap_free(h, p);
    return q;
}

gp_t galloc(uint32_t size)
{
    return heap_alloc(process_heap, size, 1);
}

void gfree(gp_t p)
{
    heap_free(process_heap, p);
}

gp_t gstrdup(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    gp_t p = heap_alloc(process_heap, n, 0);
    if (p)
        memcpy(GP(p), s, n);
    return p;
}

gp_t gwstrdup(const uint16_t *s)
{
    uint32_t n = ((uint32_t)wstrlen(s) + 1) * 2;
    gp_t p = heap_alloc(process_heap, n, 0);
    if (p)
        memcpy(GP(p), s, n);
    return p;
}
