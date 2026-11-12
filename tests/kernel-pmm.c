#define KTEST_OWN_SPIN
#include "kernel-test.h"
#include "../kernel/core/pmm.c"
static int locked;
static unsigned reclaimed_calls;
static uint32_t release_first, release_count;
void spin_lock(spinlock_t *l) { (void)l; CHECK(!locked); locked=1; }
void spin_unlock(spinlock_t *l) { (void)l; CHECK(locked); locked=0; }
/* The allocator's bitmap and share counts for 5 GiB of frames: all in use until released. */
struct boot_info boot;
static void reset(void)
{
    static uint32_t *bits;
    static uint8_t *shares;
    uint32_t frames = (uint32_t)((5ull << 30) / PAGE_SIZE);
    if (!bits) {
        bits = malloc(frames / 8);
        shares = malloc(frames);
        CHECK(bits && shares);
    }
    pmm_setup(bits, shares, frames);
    reclaim_pages=NULL;
    reclaimed_calls=0;release_count=0;
}
static uint32_t release_cache(uint32_t pages)
{
    CHECK(!locked);reclaimed_calls++;
    uint32_t n=MIN(pages,release_count);
    for(uint32_t i=0;i<n;i++)pmm_free((release_first+i)*PAGE_SIZE);
    release_first+=n;release_count-=n;
    return n;
}
int main(void)
{
    reset();
    pmm_release(100*PAGE_SIZE,104*PAGE_SIZE);
    CHECK(free_frames==4 && low_free_frames==4);
    CHECK(!pmm_alloc_reserved(4));
    CHECK(pmm_alloc_reserved(3)==100*PAGE_SIZE && low_free_frames==3);
    CHECK(!pmm_alloc_reserved(3));
    CHECK(pmm_alloc()==101*PAGE_SIZE);
    CHECK(pmm_alloc_contig(2)==102*PAGE_SIZE && !low_free_frames);
    CHECK(!pmm_alloc() && !pmm_alloc_contig(0) && !pmm_alloc_contig(LOW_FRAMES+1));
    /* High frames remain preferred for user memory and do not count as low. */
    pmm_free((LOW_FRAMES+5)*PAGE_SIZE);
    CHECK(free_frames==1 && !low_free_frames);
    CHECK(pmm_alloc_user()==(LOW_FRAMES+5)*PAGE_SIZE && !free_frames);
    /* Reclaim may free pages: allocator lock must already have been dropped. */
    pmm_set_reclaimer(release_cache);release_first=200;release_count=4;
    CHECK(pmm_alloc_contig(4)==200*PAGE_SIZE);
    CHECK(reclaimed_calls==1 && !free_frames && !release_count);
    release_first=300;release_count=2;
    CHECK(pmm_alloc()==300*PAGE_SIZE && low_free_frames==1);
    CHECK(pmm_alloc_user()==301*PAGE_SIZE && !low_free_frames);
    CHECK(!pmm_alloc());
    /* Cache allocations never recursively reclaim and respect their reserve. */
    unsigned calls=reclaimed_calls;
    CHECK(!pmm_alloc_reserved(0) && reclaimed_calls==calls);
    pmm_free(500*PAGE_SIZE);pmm_free(501*PAGE_SIZE);
    CHECK(!pmm_alloc_reserved(2));
    CHECK(pmm_alloc_reserved(1)==500*PAGE_SIZE && low_free_frames==1);
    /* COW share reference counts delay freeing until the last holder drops it. */
    uint32_t cow=501*PAGE_SIZE;
    CHECK(pmm_alloc_reserved(0)==cow && low_free_frames==0);
    pmm_ref(cow);
    pmm_ref(cow);
    CHECK(!pmm_claim_sole(cow));
    pmm_free(cow);
    CHECK(free_frames==0);
    CHECK(!pmm_claim_sole(cow));
    pmm_free(cow);
    CHECK(free_frames==0);
    CHECK(pmm_claim_sole(cow));
    pmm_free(cow);
    CHECK(free_frames==1 && low_free_frames==1);
    /* Memory above 4 GiB: user pages come from it first, and a page a device
     * must reach never does; its frames keep their whole addresses. */
    reset();
    uint64_t top=(uint64_t)(MID_FRAMES+7)*PAGE_SIZE;
    pmm_release(top,top+2*PAGE_SIZE);
    pmm_free((uint64_t)(LOW_FRAMES+9)*PAGE_SIZE);
    pmm_free(700*PAGE_SIZE);
    CHECK(free_frames==4 && low_free_frames==1 && top_free_frames==2);
    CHECK(pmm_alloc_user_low()==(LOW_FRAMES+9)*PAGE_SIZE && top_free_frames==2);
    CHECK(pmm_alloc_user()==top && pmm_alloc_user()==top+PAGE_SIZE && !top_free_frames);
    CHECK(pmm_ref(top) && !pmm_claim_sole(top));
    pmm_free(top);pmm_free(top);
    CHECK(top_free_frames==1 && free_frames==2);
    CHECK(pmm_alloc_user()==top);
    CHECK(pmm_alloc_user()==700*PAGE_SIZE && !free_frames);   /* then below 4 GiB, then low memory */
    CHECK(!pmm_alloc_user() && !pmm_alloc_user_low());
    pmm_free((uint64_t)(5ull<<30)+PAGE_SIZE);                  /* past what the bitmap covers: ignored */
    CHECK(!free_frames);
    puts("kernel physical memory: low reserve, high pages, memory above 4 GiB, pressure reclaim and contiguous retry passed");
}
