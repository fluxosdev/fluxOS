/*
 * Physical frame allocator: one bit per 4 KiB frame of the first 4 GiB.
 * Frame addresses fit 32 bits, which is what the device drivers give their
 * hardware and what the rest of the kernel keeps them in; memory above
 * 4 GiB is left unused.
 *
 * Low memory, the first GiB, is the kernel's own: its page tables, heap,
 * stacks and DMA buffers come from there (pmm_alloc, pmm_alloc_contig),
 * the lowest of it last, where the oldest devices must have their buffers.
 * High memory, above it, holds only user pages (pmm_alloc_user, which
 * falls back to low memory when high runs out). The kernel reaches every
 * frame through the direct map (phys_to_virt).
 */
#include "kernel.h"
#include "boot.h"
#include "mm.h"

#define LOW_FRAMES (LOW_MEM_END / PAGE_SIZE)
#define MAX_FRAMES (0x100000000ull / PAGE_SIZE)

static uint32_t bitmap[MAX_FRAMES / 32];
static uint8_t share_count[MAX_FRAMES];
static uint32_t total_frames;
static uint32_t free_frames, low_free_frames;
static uint32_t (*reclaim_pages)(uint32_t);
static uint32_t search_hint, high_hint;
static spinlock_t pmm_lock = SPINLOCK_INIT("pmm");

#define ISA_DMA_END     0x1000000u      /* what an ISA DMA controller's 24 address bits reach */
#define ISA_BLOCK       32768u
#define ISA_POOL_FRAMES 32              /* 128 KiB: four blocks */
static uint32_t isa_pool, isa_pool_used;

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
        share_count[f] = 0;
        free_frames--;
        if (f < LOW_FRAMES) low_free_frames--;
    }
}

static inline void frame_clear(uint32_t f)
{
    if (frame_used(f)) {
        bitmap[f / 32] &= ~(1u << (f % 32));
        share_count[f] = 0;
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

void pmm_init(void)
{
    memset(bitmap, 0xFF, sizeof(bitmap));
    memset(share_count, 0, sizeof(share_count));
    free_frames = low_free_frames = 0;
    total_frames = 0;

    /* What the loader calls usable: not the kernel image, the initrd or the
     * loader's own memory, which it lists apart. */
    for (int i = 0; i < boot.nmem; i++)
        if (boot.mem[i].type == BOOT_MEM_USABLE)
            add_region(boot.mem[i].base, boot.mem[i].base + boot.mem[i].len);
    if (!total_frames)
        panic("the boot loader reported no usable memory");

    /* The first MiB stays out of it: the firmware's data, and the pages the
     * other processors and a waking machine start in (smp.c, sleep.c). */
    pmm_reserve(0, 0x100000);

    /* The ISA devices' buffers, before anything else takes the memory they
     * can reach: the lowest free stretch, on a 64 KiB boundary. */
    for (uint32_t f = 0x100000 / PAGE_SIZE; f + ISA_POOL_FRAMES <= ISA_DMA_END / PAGE_SIZE && !isa_pool; f += 16) {
        uint32_t n = 0;
        while (n < ISA_POOL_FRAMES && !frame_used(f + n))
            n++;
        if (n == ISA_POOL_FRAMES) {
            for (uint32_t k = 0; k < n; k++)
                frame_set(f + k);
            isa_pool = f * PAGE_SIZE;
        }
    }
}

/*
 * Memory an ISA DMA controller can reach (the first 16 MiB) for a floppy
 * or Sound Blaster driver's buffer: 32 KiB blocks on 32 KiB boundaries, so
 * that none crosses the 64 KiB pages of that controller. Kept for good;
 * 0 when there is none left.
 */
uint32_t pmm_alloc_isa(void)
{
    spin_lock(&pmm_lock);
    uint32_t addr = 0;
    if (isa_pool && isa_pool_used + ISA_BLOCK <= ISA_POOL_FRAMES * PAGE_SIZE) {
        addr = isa_pool + isa_pool_used;
        isa_pool_used += ISA_BLOCK;
    }
    spin_unlock(&pmm_lock);
    return addr;
}

/* The boot loader's memory (its page tables and stack, and what it told
 * the kernel, all copied by now: bootinfo.c): ordinary memory from here on. */
void pmm_reclaim_loader(void)
{
    spin_lock(&pmm_lock);
    for (int i = 0; i < boot.nmem; i++)
        if (boot.mem[i].type == BOOT_MEM_LOADER && boot.mem[i].base >= 0x100000)
            add_region(boot.mem[i].base, boot.mem[i].base + boot.mem[i].len);
    spin_unlock(&pmm_lock);
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
    if (free_frames == low_free_frames) {
        /* None free past 1 GiB (or none there at all): skip the scan. */
        spin_unlock(&pmm_lock);
        return pmm_alloc();
    }
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

int pmm_ref(uint32_t phys)
{
    uint32_t f = phys / PAGE_SIZE;
    if (f >= MAX_FRAMES)
        return 0;
    spin_lock(&pmm_lock);
    if (!frame_used(f) || share_count[f] == 255) {
        spin_unlock(&pmm_lock);
        return 0;
    }
    share_count[f]++;
    spin_unlock(&pmm_lock);
    return 1;
}

int pmm_claim_sole(uint32_t phys)
{
    uint32_t f = phys / PAGE_SIZE;
    if (f >= MAX_FRAMES)
        return 0;
    spin_lock(&pmm_lock);
    int sole = frame_used(f) != 0 && share_count[f] == 0;
    spin_unlock(&pmm_lock);
    return sole;
}

void pmm_free(uint32_t phys)
{
    uint32_t f = phys / PAGE_SIZE;
    if (f >= MAX_FRAMES)
        return;
    spin_lock(&pmm_lock);
    if (share_count[f] > 0)
        share_count[f]--;
    else
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

uint32_t pmm_low_free_kb(void)
{
    return __atomic_load_n(&low_free_frames, __ATOMIC_RELAXED) * (PAGE_SIZE / 1024);
}
