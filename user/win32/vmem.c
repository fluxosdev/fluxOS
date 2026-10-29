/*
 * The program's address space: what VirtualAlloc, VirtualQuery and the
 * loader see. Regions are kept in a sorted list; memory is really mapped
 * (read, write and execute) when reserved, so reserve and commit only
 * differ in what VirtualQuery reports. Below GUEST_IMAGE is the loader's
 * own memory on fluxOS; above GUEST_HIGH the thread block, the loader's
 * stack and the kernel's device window.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32.h"
#include "os.h"

#define GRANULARITY 0x10000u
#define PAGE 0x1000u

struct region {
    gp_t base;
    uint32_t size;
    uint32_t state;                     /* MEM_RESERVE or MEM_COMMIT */
    uint32_t protect;
    uint32_t type;                      /* MEM_PRIVATE, MEM_IMAGE, MEM_MAPPED */
    gp_t alloc_base;                    /* the reservation this is part of */
    uint32_t alloc_protect;
    int loader;                         /* not the program's: never freed */
    struct region *next;
};

static struct region *regions;          /* sorted by base */
static gp_t low_limit, high_limit;

static uint32_t round_up(uint32_t v, uint32_t g)
{
    return (v + g - 1) & ~(g - 1);
}

static struct region *region_new(gp_t base, uint32_t size)
{
    struct region *r = calloc(1, sizeof(*r));
    if (!r)
        wdie("out of memory");
    r->base = base;
    r->size = size;
    struct region **pp = &regions;
    while (*pp && (*pp)->base < base)
        pp = &(*pp)->next;
    r->next = *pp;
    *pp = r;
    return r;
}

static struct region *region_find(gp_t addr)
{
    for (struct region *r = regions; r; r = r->next)
        if (addr >= r->base && addr < r->base + r->size)
            return r;
    return NULL;
}

static int range_free(gp_t base, uint32_t size)
{
    if (base < low_limit || base + size > high_limit || base + size < base)
        return 0;
    for (struct region *r = regions; r; r = r->next)
        if (r->base < base + size && base < r->base + r->size)
            return 0;
    return 1;
}

void vm_init(gp_t loader_end)
{
    low_limit = GUEST_LOW;
    high_limit = GUEST_HIGH;
    if (loader_end > low_limit) {
        struct region *r = region_new(0, loader_end);
        r->state = MEM_COMMIT;
        r->protect = PAGE_READWRITE;
        r->type = MEM_PRIVATE;
        r->loader = 1;
        low_limit = loader_end;
    }
}

gp_t vm_alloc(gp_t addr, uint32_t size, uint32_t protect, uint32_t type, int fixed)
{
    int top_down = (type & MEM_TOP_DOWN) != 0;
    type &= ~(uint32_t)MEM_TOP_DOWN;
    size = round_up(size, PAGE);
    if (!size)
        return 0;
    if (addr) {
        addr &= ~(PAGE - 1);
        if (!range_free(addr, size)) {
            if (fixed)
                return 0;
            addr = 0;
        }
    }
    if (!addr && top_down) {
        /* the highest gap that fits, for the loader's own things */
        gp_t gap_start = round_up(low_limit, GRANULARITY), best = 0;
        for (struct region *r = regions; ; r = r->next) {
            gp_t gap_end = r ? r->base : high_limit;
            if (gap_end > gap_start && gap_end - gap_start >= size) {
                gp_t c = (gap_end - size) & ~(GRANULARITY - 1);
                if (c >= gap_start)
                    best = c;
            }
            if (!r)
                break;
            gp_t e = round_up(r->base + r->size, GRANULARITY);
            if (e > gap_start)
                gap_start = e;
        }
        if (!best)
            return 0;
        addr = best;
    }
    if (!addr) {
        /* bottom up, on 64 KiB boundaries, as Windows does */
        gp_t cand = round_up(low_limit, GRANULARITY);
        for (struct region *r = regions; ; r = r->next) {
            if (r && r->base + r->size <= cand)
                continue;
            gp_t end = r ? r->base : high_limit;
            if (end >= cand + size)
                break;
            if (!r)
                return 0;
            cand = round_up(r->base + r->size, GRANULARITY);
        }
        if (cand + size > high_limit)
            return 0;
        addr = cand;
    }
    if (os_map(addr + WIN_BASE, size) < 0)
        return 0;
    struct region *r = region_new(addr, size);
    r->state = MEM_COMMIT;
    r->protect = protect ? protect : PAGE_NOACCESS;
    r->type = type ? type : MEM_PRIVATE;
    r->alloc_base = addr;
    r->alloc_protect = r->protect;
    return addr;
}

/* Split r at addr (inside it): the part from addr on becomes a new region. */
static struct region *split(struct region *r, gp_t addr)
{
    if (addr == r->base)
        return r;
    struct region *n = region_new(addr, r->base + r->size - addr);
    n->state = r->state;
    n->protect = r->protect;
    n->type = r->type;
    n->alloc_base = r->alloc_base;
    n->alloc_protect = r->alloc_protect;
    n->loader = r->loader;
    r->size = addr - r->base;
    return n;
}

static void merge(void)
{
    for (struct region *r = regions; r && r->next; ) {
        struct region *n = r->next;
        if (r->base + r->size == n->base && r->alloc_base == n->alloc_base && r->state == n->state &&
            r->protect == n->protect && r->type == n->type && r->loader == n->loader) {
            r->size += n->size;
            r->next = n->next;
            free(n);
        } else {
            r = n;
        }
    }
}

/* Apply fn to every piece of [addr, addr + size) that is mapped. */
static int apply(gp_t addr, uint32_t size, void (*fn)(struct region *r, void *ctx), void *ctx)
{
    addr &= ~(PAGE - 1);
    size = round_up(size, PAGE);
    gp_t end = addr + size;
    int any = 0;
    for (struct region *r = regions; r; r = r->next) {
        if (r->loader || r->base + r->size <= addr)
            continue;
        if (r->base >= end)
            break;
        if (r->base < addr)
            r = split(r, addr);
        if (r->base + r->size > end)
            split(r, end);
        fn(r, ctx);
        any = 1;
    }
    merge();
    return any;
}

int vm_free(gp_t addr, uint32_t size)
{
    struct region *r = region_find(addr);
    if (!r || r->loader)
        return -1;
    gp_t base = r->alloc_base;
    (void)size;
    for (struct region *q = regions; q; ) {
        struct region *n = q->next;
        if (q->alloc_base == base && !q->loader) {
            os_unmap(q->base + WIN_BASE, q->size);
            struct region **pp = &regions;
            while (*pp != q)
                pp = &(*pp)->next;
            *pp = n;
            free(q);
        }
        q = n;
    }
    return 0;
}

struct commit_arg { uint32_t state, protect; };

static void set_state(struct region *r, void *ctx)
{
    struct commit_arg *c = ctx;
    r->state = c->state;
    if (c->protect)
        r->protect = c->protect;
}

int vm_commit(gp_t addr, uint32_t size, uint32_t protect)
{
    struct commit_arg c = { MEM_COMMIT, protect };
    return apply(addr, size, set_state, &c) ? 0 : -1;
}

int vm_decommit(gp_t addr, uint32_t size)
{
    struct commit_arg c = { MEM_RESERVE, PAGE_NOACCESS };
    if (!apply(addr, size, set_state, &c))
        return -1;
    addr &= ~(PAGE - 1);
    os_map(addr + WIN_BASE, round_up(size, PAGE));  /* fresh zeroes when committed again */
    return 0;
}

struct protect_arg { uint32_t protect, old; int first; };

static void set_protect(struct region *r, void *ctx)
{
    struct protect_arg *p = ctx;
    if (p->first) {
        p->old = r->protect;
        p->first = 0;
    }
    r->protect = p->protect;
}

int vm_protect(gp_t addr, uint32_t size, uint32_t protect, uint32_t *old)
{
    struct protect_arg p = { protect, PAGE_NOACCESS, 1 };
    if (!apply(addr, size, set_protect, &p))
        return -1;
    if (old)
        *old = p.old;
    return 0;
}

int vm_query(gp_t addr, struct vm_info *out)
{
    memset(out, 0, sizeof(*out));
    if (addr >= GUEST_TOP)
        return -1;
    struct region *r = region_find(addr);
    if (r) {
        out->base = r->base;
        out->alloc_base = r->alloc_base;
        out->alloc_protect = r->alloc_protect;
        out->size = r->size;
        out->state = r->state;
        out->protect = r->state == MEM_COMMIT ? r->protect : 0;
        out->type = r->type;
        if (r->loader) {
            out->alloc_protect = PAGE_READWRITE;
            out->protect = PAGE_READWRITE;
        }
        return 0;
    }
    gp_t end = GUEST_TOP;
    for (struct region *q = regions; q; q = q->next)
        if (q->base > addr) {
            end = q->base;
            break;
        }
    out->base = addr & ~(PAGE - 1);
    out->size = end - out->base;
    out->state = MEM_FREE;
    out->protect = PAGE_NOACCESS;
    return 0;
}

static int accessible(gp_t addr, uint32_t size, int write)
{
    if (!size)
        return 1;
    if (addr + size < addr || addr + size > GUEST_TOP)
        return 0;
    gp_t p = addr;
    while (p < addr + size) {
        struct region *r = region_find(p);
        if (!r)
            return 0;
        if (!r->loader) {
            if (r->state != MEM_COMMIT)
                return 0;
            uint32_t pr = r->protect & 0xFF;
            if (pr == PAGE_NOACCESS)
                return 0;
            if (write && !(pr & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)))
                return 0;
        }
        p = r->base + r->size;
    }
    return 1;
}

int vm_readable(gp_t addr, uint32_t size)
{
    return accessible(addr, size, 0);
}

int vm_writable(gp_t addr, uint32_t size)
{
    return accessible(addr, size, 1);
}

/* Small things the loader keeps in guest memory, never freed. */
gp_t vm_alloc_loader(uint32_t size)
{
    static gp_t pool;
    static uint32_t left;
    size = (size + 15) & ~15u;
    if (size > left) {
        uint32_t chunk = size > 0x10000 ? round_up(size, PAGE) : 0x10000;
        pool = vm_alloc(0, chunk, PAGE_READWRITE, MEM_PRIVATE, 0);
        if (!pool)
            wdie("out of guest memory");
        left = chunk;
    }
    gp_t p = pool;
    pool += size;
    left -= size;
    return p;
}
