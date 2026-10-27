/*
 * Physical frame allocator: one bit per 4 KiB frame of the first 4 GiB
 * (without PAE, what a 32-bit address reaches).
 *
 * Low memory, the first GiB, is the kernel's identity-mapped memory: its
 * page tables, heap, stacks and DMA buffers come from there (pmm_alloc,
 * pmm_alloc_contig). High memory, above it, holds only user pages
 * (pmm_alloc_user, which falls back to low memory when high runs out);
 * the kernel reaches those through vmm.c's temporary mappings.
 */
#include "kernel.h"
#include "mm.h"
#include "multiboot.h"

#define LOW_FRAMES (KERNEL_SPACE_END / PAGE_SIZE)
#define MAX_FRAMES (0x100000000ull / PAGE_SIZE)

static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t total_frames;
static uint32_t free_frames, low_free_frames;
static uint32_t (*reclaim_pages)(uint32_t);
static uint32_t search_hint, high_hint;
static spinlock_t pmm_lock = SPINLOCK_INIT("pmm");

void pmm_set_reclaimer(uint32_t (*reclaim)(uint32_t))
{
    reclaim_pages = reclaim;
}

static inline int frame_used(uint32_t f)
{
    return bitmap[f / 32] & (1u << (f % 32));
}

static inline void frame_set(uint32_t f)
{
    if (!frame_used(f)) {
        bitmap[f / 32] |= 1u << (f % 32);
        free_frames--;
        if (f < LOW_FRAMES) low_free_frames--;
    }
}

static inline void frame_clear(uint32_t f)
{
    if (frame_used(f)) {
        bitmap[f / 32] &= ~(1u << (f % 32));
        free_frames++;
        if (f < LOW_FRAMES) low_free_frames++;
    }
}

/* Mark [start, end) available; partial frames at the edges are skipped,
 * and memory past 4 GiB is out of reach. */
static void add_region(uint64_t start, uint64_t end)
{
    if (start >= 0x100000000ull)
        return;
    if (end > 0x100000000ull)
        end = 0x100000000ull;
    uint32_t s = (uint32_t)((start + PAGE_SIZE - 1) / PAGE_SIZE);
    uint32_t e = (uint32_t)(end / PAGE_SIZE);
    for (uint32_t f = s; f < e; f++) {
        if (frame_used(f)) {
            total_frames++;
            frame_clear(f);
        }
    }
}

void pmm_reserve(uint32_t start, uint32_t end)
{
    uint32_t s = ALIGN_DOWN(start, PAGE_SIZE) / PAGE_SIZE;
    uint32_t e = MIN(ALIGN_UP(end, PAGE_SIZE) / PAGE_SIZE, MAX_FRAMES);
    spin_lock(&pmm_lock);
    for (uint32_t f = s; f < e; f++)
        frame_set(f);
    spin_unlock(&pmm_lock);
}

void pmm_release(uint32_t start, uint32_t end)
{
    uint32_t s = ALIGN_UP(start, PAGE_SIZE) / PAGE_SIZE;
    uint32_t e = MIN(ALIGN_DOWN(end, PAGE_SIZE) / PAGE_SIZE, MAX_FRAMES);
    spin_lock(&pmm_lock);
    for (uint32_t f = s; f < e; f++)
        frame_clear(f);
    spin_unlock(&pmm_lock);
}

void pmm_init(struct multiboot_info *mbi)
{
    memset(bitmap, 0xFF, sizeof(bitmap));
    free_frames = low_free_frames = 0;
    total_frames = 0;

    if (mbi->flags & MB_INFO_MMAP) {
        uint32_t p = mbi->mmap_addr;
        uint32_t end = mbi->mmap_addr + mbi->mmap_length;
        while (p < end) {
            struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry *)p;
            if (e->type == MULTIBOOT_MEMORY_AVAILABLE)
                add_region(e->addr, e->addr + e->len);
            p += e->size + sizeof(e->size);
        }
    } else if (mbi->flags & MB_INFO_MEMORY) {
        add_region(0x100000, 0x100000 + (uint64_t)mbi->mem_upper * 1024);
    } else {
        panic("bootloader provided no memory information");
    }

    /* Low memory (BIOS, VGA), the kernel image and boot modules. */
    pmm_reserve(0, 0x100000);
    pmm_reserve((uint32_t)kernel_start, (uint32_t)kernel_end);
    if (mbi->flags & MB_INFO_MODS) {
        struct multiboot_module *mods = (struct multiboot_module *)mbi->mods_addr;
        for (uint32_t i = 0; i < mbi->mods_count; i++)
            pmm_reserve(mods[i].mod_start, mods[i].mod_end);
    }
}

/* A low frame with a caller-selected reserve; no callbacks or recursive reclaim. */
uint32_t pmm_alloc_reserved(uint32_t reserve)
{
    spin_lock(&pmm_lock);
    if (low_free_frames <= reserve) { spin_unlock(&pmm_lock); return 0; }
    for (uint32_t i = 0; i < LOW_FRAMES / 32; i++) {
        uint32_t w = (search_hint + i) % (LOW_FRAMES / 32);
        if (bitmap[w] != 0xFFFFFFFF) {
            uint32_t bit = (uint32_t)__builtin_ctz(~bitmap[w]);
            uint32_t f = w * 32 + bit;
            frame_set(f);
            search_hint = w;
            spin_unlock(&pmm_lock);
            return f * PAGE_SIZE;
        }
    }
    spin_unlock(&pmm_lock);
    return 0;
}

/* Reclaim outside the allocator lock. The callback must not allocate or wait.
 * Restore a small low-memory cushion before allocations start failing. */
uint32_t pmm_alloc(void)
{
    if (reclaim_pages && __atomic_load_n(&low_free_frames, __ATOMIC_RELAXED) < 2048)
        reclaim_pages(64);
    uint32_t page = pmm_alloc_reserved(0);
    if (!page && reclaim_pages && reclaim_pages(1)) page = pmm_alloc_reserved(0);
    return page;
}

/* A frame for a user page: high memory first, so low memory stays for the kernel. */
uint32_t pmm_alloc_user(void)
{
    spin_lock(&pmm_lock);
    const uint32_t first = LOW_FRAMES / 32, n = (MAX_FRAMES - LOW_FRAMES) / 32;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = first + (high_hint + i) % n;
        if (bitmap[w] != 0xFFFFFFFF) {
            uint32_t f = w * 32 + (uint32_t)__builtin_ctz(~bitmap[w]);
            frame_set(f);
            high_hint = w - first;
            spin_unlock(&pmm_lock);
            return f * PAGE_SIZE;
        }
    }
    spin_unlock(&pmm_lock);
    return pmm_alloc();
}

/* Contiguous low memory (DMA buffers, the heap). */
static uint32_t try_contig(uint32_t frames)
{
    uint32_t run = 0;
    spin_lock(&pmm_lock);
    for (uint32_t f = 0; f < LOW_FRAMES; f++) {
        run = frame_used(f) ? 0 : run + 1;
        if (run == frames) {
            uint32_t first = f + 1 - frames;
            for (uint32_t k = first; k <= f; k++)
                frame_set(k);
            spin_unlock(&pmm_lock);
            return first * PAGE_SIZE;
        }
    }
    spin_unlock(&pmm_lock);
    return 0;
}

uint32_t pmm_alloc_contig(uint32_t frames)
{
    if (!frames || frames > LOW_FRAMES) return 0;
    uint32_t page = try_contig(frames);
    if (!page && reclaim_pages && reclaim_pages(frames)) page = try_contig(frames);
    return page;
}

void pmm_free(uint32_t phys)
{
    uint32_t f = phys / PAGE_SIZE;
    if (f >= MAX_FRAMES)
        return;
    spin_lock(&pmm_lock);
    frame_clear(f);
    spin_unlock(&pmm_lock);
}

uint32_t pmm_total_kb(void)
{
    return total_frames * (PAGE_SIZE / 1024);
}

uint32_t pmm_free_kb(void)
{
    return __atomic_load_n(&free_frames, __ATOMIC_RELAXED) * (PAGE_SIZE / 1024);
}
