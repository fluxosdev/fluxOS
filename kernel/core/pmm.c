/*
 * Physical frame allocator: one bit per 4 KiB frame, for all the memory
 * there is. The bitmap and the frames' share counts are sized to the
 * machine at boot and put in its memory, the highest that has room.
 *
 * Low memory, the first GiB, is the kernel's own: its page tables, heap,
 * stacks and DMA buffers come from there (pmm_alloc, pmm_alloc_contig),
 * the lowest of it last, where the oldest devices must have their buffers.
 * A kernel frame's address fits 32 bits, which is what the device drivers
 * give their hardware and the rest of the kernel keeps them in.
 *
 * Memory above it holds only user pages (pmm_alloc_user), and those come
 * from the top down: from above 4 GiB first, then from below it, and from
 * low memory when all else has run out. A user page's frame is a 64-bit
 * address. One that a device with 32-bit addresses must be able to reach
 * (a shared memory segment, a page lent to the GPU) comes from below
 * 4 GiB (pmm_alloc_user_low). The kernel reaches every frame through the
 * direct map (phys_to_virt).
 */
#include "kernel.h"
#include "boot.h"
#include "mm.h"

#define LOW_FRAMES (LOW_MEM_END / PAGE_SIZE)
#define MID_FRAMES ((uint32_t)(0x100000000ull / PAGE_SIZE)) /* 4 GiB: where 32-bit addresses end */
#define MAX_FRAMES 0xFFFFFFE0u          /* frame numbers are 32 bits: 16 TiB */

static uint32_t *bitmap;                /* a bit per frame: set, in use (or not memory at all) */
static uint8_t *share_count;
static uint32_t nframes;                /* frames the two cover (a multiple of 32): up to the top of RAM */
static uint64_t meta_phys;              /* where the two are */
static uint32_t total_frames;
static uint32_t free_frames, low_free_frames, top_free_frames;  /* all, below 1 GiB, above 4 GiB */
static uint32_t (*reclaim_pages)(uint32_t);
static uint32_t search_hint, high_hint, top_hint;
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
        if (f >= MID_FRAMES) top_free_frames--;
    }
}

static inline void frame_clear(uint32_t f)
{
    if (frame_used(f)) {
        bitmap[f / 32] &= ~(1u << (f % 32));
        share_count[f] = 0;
        free_frames++;
        if (f < LOW_FRAMES) low_free_frames++;
        if (f >= MID_FRAMES) top_free_frames++;
    }
}

/* The bitmap (frames / 8 bytes) and the share counts (a byte a frame) for
 * this many frames, at these addresses: nothing is memory until added. */
static void pmm_setup(uint32_t *bits, uint8_t *shares, uint32_t frames)
{
    bitmap = bits;
    share_count = shares;
    nframes = frames;
    memset(bitmap, 0xFF, frames / 8);
    memset(share_count, 0, frames);
    total_frames = free_frames = low_free_frames = top_free_frames = 0;
    search_hint = high_hint = top_hint = 0;
}

/* Mark [start, end) available; partial frames at the edges are skipped,
 * and so is what the bitmap does not reach. */
static void add_region(uint64_t start, uint64_t end)
{
    uint64_t s = (start + PAGE_SIZE - 1) / PAGE_SIZE, e = MIN(end / PAGE_SIZE, (uint64_t)nframes);
    for (uint64_t f = s; f < e; f++) {
        if (frame_used((uint32_t)f)) {
            total_frames++;
            frame_clear((uint32_t)f);
        }
    }
}

void pmm_reserve(uint64_t start, uint64_t end)
{
    uint64_t s = start / PAGE_SIZE, e = MIN((end + PAGE_SIZE - 1) / PAGE_SIZE, (uint64_t)nframes);
    spin_lock(&pmm_lock);
    for (uint64_t f = s; f < e; f++)
        frame_set((uint32_t)f);
    spin_unlock(&pmm_lock);
}

void pmm_release(uint64_t start, uint64_t end)
{
    uint64_t s = (start + PAGE_SIZE - 1) / PAGE_SIZE, e = MIN(end / PAGE_SIZE, (uint64_t)nframes);
    spin_lock(&pmm_lock);
    for (uint64_t f = s; f < e; f++)
        frame_clear((uint32_t)f);
    spin_unlock(&pmm_lock);
}

void pmm_init(void)
{
    /* How far memory goes: what is usable, and what will be (the boot
     * loader's own, the initrd). */
    uint64_t top = 0;
    for (int i = 0; i < boot.nmem; i++) {
        const struct boot_mem *m = &boot.mem[i];
        if (m->type == BOOT_MEM_USABLE || m->type == BOOT_MEM_LOADER || m->type == BOOT_MEM_KERNEL)
            top = MAX(top, m->base + m->len);
    }
    uint64_t frames = MIN((top / PAGE_SIZE + 31) & ~31ull, (uint64_t)MAX_FRAMES);
    /* The bitmap and the share counts: in usable memory, the highest
     * stretch with room for them (the boot loader's direct map has it). */
    uint64_t bytes = ALIGN_UP(frames / 8 + frames, (uint64_t)PAGE_SIZE);
    meta_phys = 0;
    for (int i = boot.nmem - 1; i >= 0 && !meta_phys; i--) {
        const struct boot_mem *m = &boot.mem[i];
        uint64_t base = MAX(ALIGN_UP(m->base, (uint64_t)PAGE_SIZE), 0x100000ull);
        uint64_t end = ALIGN_DOWN(m->base + m->len, (uint64_t)PAGE_SIZE);
        if (m->type == BOOT_MEM_USABLE && end > base && end - base >= bytes)
            meta_phys = end - bytes;
    }
    if (!frames || !meta_phys)
        panic("the boot loader reported no usable memory");
    uint8_t *meta = phys_to_virt(meta_phys);
    pmm_setup((uint32_t *)meta, meta + frames / 8, (uint32_t)frames);

    /* What the loader calls usable: not the kernel image, the initrd or the
     * loader's own memory, which it lists apart. */
    for (int i = 0; i < boot.nmem; i++)
        if (boot.mem[i].type == BOOT_MEM_USABLE)
            add_region(boot.mem[i].base, boot.mem[i].base + boot.mem[i].len);
    pmm_reserve(meta_phys, meta_phys + bytes);

    /* The first MiB stays out of it: the firmware's data, and the pages the
     * other processors and a waking machine start in (smp.c, sleep.c). */
    pmm_reserve(0, 0x100000);

    /* The ISA devices' buffers, before anything else takes the memory they
     * can reach: the lowest free stretch, on a 64 KiB boundary. */
    for (uint32_t f = 0x100000 / PAGE_SIZE; f + ISA_POOL_FRAMES <= MIN(ISA_DMA_END / PAGE_SIZE, nframes) && !isa_pool; f += 16) {
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

/* The direct map has moved (paging_init(), to the kernel's own page
 * tables): the bitmap and the share counts are where they were, there. */
void pmm_remap(void)
{
    uint8_t *meta = phys_to_virt(meta_phys);
    bitmap = (uint32_t *)meta;
    share_count = meta + nframes / 8;
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

/* A free frame among the bitmap's words [first, end), looked for from
 * *hint on (a word of the range, relative to its start), and taken; 0 if
 * there is none. With the lock. */
static uint64_t take_frame(uint32_t first, uint32_t end, uint32_t *hint)
{
    uint32_t n = end > first ? end - first : 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = first + (*hint + i) % n;
        if (bitmap[w] != 0xFFFFFFFF) {
            uint32_t f = w * 32 + (uint32_t)__builtin_ctz(~bitmap[w]);
            frame_set(f);
            *hint = w - first;
            return (uint64_t)f * PAGE_SIZE;
        }
    }
    return 0;
}

/* A low frame with a caller-selected reserve; no callbacks or recursive reclaim. */
uint32_t pmm_alloc_reserved(uint32_t reserve)
{
    spin_lock(&pmm_lock);
    uint32_t page = 0;
    if (low_free_frames > reserve)
        page = (uint32_t)take_frame(0, MIN(LOW_FRAMES, nframes) / 32, &search_hint);
    spin_unlock(&pmm_lock);
    return page;
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

/* A frame for a user page, below 4 GiB: from above the first GiB if there
 * is any free, so that low memory stays for the kernel. */
uint32_t pmm_alloc_user_low(void)
{
    spin_lock(&pmm_lock);
    uint32_t page = 0;
    if (free_frames - top_free_frames > low_free_frames)    /* (else none there: skip the scan) */
        page = (uint32_t)take_frame(LOW_FRAMES / 32, MIN(MID_FRAMES, nframes) / 32, &high_hint);
    spin_unlock(&pmm_lock);
    return page ? page : pmm_alloc();
}

/* A frame for a user page, anywhere: the memory above 4 GiB first, which
 * nothing else can use. */
uint64_t pmm_alloc_user(void)
{
    spin_lock(&pmm_lock);
    uint64_t page = 0;
    if (top_free_frames)
        page = take_frame(MID_FRAMES / 32, nframes / 32, &top_hint);
    spin_unlock(&pmm_lock);
    return page ? page : pmm_alloc_user_low();
}

/* Contiguous low memory (DMA buffers, the heap). */
static uint32_t try_contig(uint32_t frames)
{
    uint32_t run = 0;
    spin_lock(&pmm_lock);
    for (uint32_t f = 0; f < MIN(LOW_FRAMES, nframes); f++) {
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

int pmm_ref(uint64_t phys)
{
    uint64_t f = phys / PAGE_SIZE;
    if (f >= nframes)
        return 0;
    spin_lock(&pmm_lock);
    if (!frame_used((uint32_t)f) || share_count[f] == 255) {
        spin_unlock(&pmm_lock);
        return 0;
    }
    share_count[f]++;
    spin_unlock(&pmm_lock);
    return 1;
}

int pmm_claim_sole(uint64_t phys)
{
    uint64_t f = phys / PAGE_SIZE;
    if (f >= nframes)
        return 0;
    spin_lock(&pmm_lock);
    int sole = frame_used((uint32_t)f) != 0 && share_count[f] == 0;
    spin_unlock(&pmm_lock);
    return sole;
}

void pmm_free(uint64_t phys)
{
    uint64_t f = phys / PAGE_SIZE;
    if (f >= nframes)
        return;
    spin_lock(&pmm_lock);
    if (share_count[f] > 0)
        share_count[f]--;
    else
        frame_clear((uint32_t)f);
    spin_unlock(&pmm_lock);
}

/* (In KiB, so that 32 bits count 4 TiB.) */
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
