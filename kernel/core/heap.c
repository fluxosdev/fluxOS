/*
 * Kernel heap: a first-fit allocator over one physically contiguous
 * region. Blocks are kept in an address-ordered list so neighbouring
 * free blocks can be merged on kfree().
 */
#include "kernel.h"
#include "boot.h"
#include "mm.h"

#define HEAP_MAGIC 0xF10C0DE5u
#define HEAP_ALIGN 16

struct block {
    uint32_t magic;
    uint32_t size;          /* payload bytes */
    uint32_t free;
    uint32_t pad;
    struct block *prev, *next;
};
_Static_assert(sizeof(struct block) == 32, "the header is 32 bytes: payload stays aligned");

static struct block *heap_head;
static uint32_t heap_size;
static uint32_t heap_used;
static spinlock_t heap_lock = SPINLOCK_INIT("heap");    /* interrupt handlers allocate too */

/* A quarter of RAM, between 4 MiB and 256 MiB. Files that live in memory
 * have to fit in it, so where the initrd is larger than that leaves room for
 * (the whole system with its compiler, as "make run-kernel" and the QEMU
 * tests boot it), the heap is what the initrd unpacks to and something
 * beside. Never more than half of the low memory the kernel's page tables,
 * stacks and DMA buffers share with it, though: under 512 MiB. (In KiB: a
 * quarter of RAM in bytes need not fit 32 bits.) */
void heap_init(void)
{
    uint32_t size_kb = MIN(pmm_total_kb() / 4, 256u << 10);
    uint64_t initrd_kb = boot.initrd_size / 1024;
    uint64_t need_kb = initrd_kb ? initrd_kb + initrd_kb / 4 + (32u << 10) : 0;
    if (size_kb < need_kb)
        size_kb = (uint32_t)MIN(need_kb, (uint64_t)UINT32_MAX);
    size_kb = MIN(size_kb, pmm_low_free_kb() / 2);
    size_kb = MAX(size_kb, 4u << 10);

    /* One run of whole frames; a shorter one where low memory is in pieces. */
    uint32_t frames = size_kb / (PAGE_SIZE / 1024), base;
    while (!(base = pmm_alloc_contig(frames)) && frames > 1024)
        frames -= frames / 8;
    if (!base)
        panic("heap: cannot reserve %u KiB", frames * (PAGE_SIZE / 1024));
    uint32_t size = frames * PAGE_SIZE;

    heap_head = phys_to_virt(base);
    heap_head->magic = HEAP_MAGIC;
    heap_head->size = size - sizeof(struct block);
    heap_head->free = 1;
    heap_head->prev = heap_head->next = NULL;
    heap_size = size;
    heap_used = 0;
}

static void split(struct block *b, uint32_t size)
{
    if (b->size < size + sizeof(struct block) + HEAP_ALIGN)
        return;
    struct block *n = (struct block *)((uint8_t *)(b + 1) + size);
    n->magic = HEAP_MAGIC;
    n->size = b->size - size - sizeof(struct block);
    n->free = 1;
    n->prev = b;
    n->next = b->next;
    if (b->next)
        b->next->prev = n;
    b->next = n;
    b->size = size;
}

/* Absorb b->next into b. */
static void merge_next(struct block *b)
{
    struct block *n = b->next;
    b->size += sizeof(struct block) + n->size;
    b->next = n->next;
    if (n->next)
        n->next->prev = b;
    n->magic = 0;
}

void *kmalloc(size_t n)
{
    if (!n)
        n = 1;
    if (n > UINT32_MAX - (HEAP_ALIGN - 1u) || n > heap_size)
        return NULL;
    uint32_t size = ALIGN_UP((uint32_t)n, HEAP_ALIGN);
    spin_lock(&heap_lock);
    for (struct block *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split(b, size);
            b->free = 0;
            heap_used += b->size + sizeof(struct block);
            spin_unlock(&heap_lock);
            return b + 1;
        }
    }
    spin_unlock(&heap_lock);
    klog("kmalloc: out of memory (%u bytes)\n", (uint32_t)n);
    return NULL;
}

void *kzalloc(size_t n)
{
    void *p = kmalloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void kfree(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)p - 1;
    spin_lock(&heap_lock);
    if (b->magic != HEAP_MAGIC || b->free) {
        spin_unlock(&heap_lock);
        panic("kfree: bad pointer %p", p);
    }
    b->free = 1;
    heap_used -= b->size + sizeof(struct block);
    if (b->next && b->next->free)
        merge_next(b);
    if (b->prev && b->prev->free)
        merge_next(b->prev);
    spin_unlock(&heap_lock);
}

void *krealloc(void *p, size_t n)
{
    if (n > UINT32_MAX - (HEAP_ALIGN - 1u) || n > heap_size)
        return NULL;
    if (!p)
        return kmalloc(n);
    struct block *b = (struct block *)p - 1;
    uint32_t size = ALIGN_UP((uint32_t)n, HEAP_ALIGN);
    spin_lock(&heap_lock);
    if (b->size >= size) {
        spin_unlock(&heap_lock);
        return p;
    }
    if (b->next && b->next->free &&
        b->size + sizeof(struct block) + b->next->size >= size) {
        heap_used -= b->size;
        merge_next(b);
        split(b, size);
        heap_used += b->size;
        spin_unlock(&heap_lock);
        return p;
    }
    spin_unlock(&heap_lock);
    void *q = kmalloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, b->size);
    kfree(p);
    return q;
}

void heap_stats(uint32_t *total, uint32_t *used)
{
    spin_lock(&heap_lock);
    *total = heap_size;
    *used = heap_used;
    spin_unlock(&heap_lock);
}
