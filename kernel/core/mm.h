/* Physical memory, paging and the kernel heap. */
#ifndef MM_H
#define MM_H

#include <stddef.h>
#include <stdint.h>

struct multiboot_info;

/* ---- physical frame allocator ---- */
void pmm_init(struct multiboot_info *mbi);
void pmm_reserve(uint32_t start, uint32_t end);
void pmm_release(uint32_t start, uint32_t end);
uint32_t pmm_alloc(void);                   /* below 1 GiB; 0 when out of memory */
uint32_t pmm_alloc_user(void);              /* anywhere, for user pages (vmm_kmap) */
uint32_t pmm_alloc_contig(uint32_t frames); /* below 1 GiB */
uint32_t pmm_alloc_reserved(uint32_t reserve); /* cache pages; never reclaim, retain low frames */
void pmm_set_reclaimer(uint32_t (*reclaim)(uint32_t pages)); /* boot-time registration */
int pmm_ref(uint32_t phys);                 /* add a COW/shared reference; 0 if full/invalid */
int pmm_claim_sole(uint32_t phys);          /* 1 if allocated with no extra references */
void pmm_free(uint32_t phys);
uint32_t pmm_total_kb(void);
uint32_t pmm_free_kb(void);
uint32_t pmm_low_free_kb(void);             /* of the first GiB, the kernel's own */

/* ---- paging ---- */
#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_USER    0x004
#define PTE_PWT     0x008       /* write-through */
#define PTE_PCD     0x010       /* cache disabled (device registers) */
#define PTE_LARGE   0x080
#define PTE_GLOBAL  0x100
#define PTE_MMIO    0x200       /* (available bit) device memory, not ours to free */
#define PTE_COW     0x400       /* (available bit) copy-on-write user page */
#define PTE_SHARED  0x800       /* (available bit) shared across fork without COW */

/* PTE bits for write-combining device memory (the framebuffer), or 0 if
 * the processor has no page attribute table (cpu.c). */
extern uint32_t pte_wc;

extern uint32_t *kernel_pd;

void paging_init(void);
void vmm_switch(uint32_t *pd);
uint32_t *vmm_create(void);
void vmm_destroy(uint32_t *pd);
uint32_t *vmm_clone(uint32_t *pd);
int vmm_alloc(uint32_t *pd, uint32_t va);   /* map a zeroed user page */
int vmm_alloc_prot(uint32_t *pd, uint32_t va, uint32_t prot, uint32_t flags);
int vmm_protect_page(uint32_t *pd, uint32_t va, uint32_t prot);
void vmm_free_page(uint32_t *pd, uint32_t va);
int vmm_pte_present(uint32_t *pd, uint32_t va);
uint32_t vmm_translate(uint32_t *pd, uint32_t va);
int vmm_write_ok(uint32_t *pd, uint32_t va);
int vmm_resolve_cow(uint32_t *pd, uint32_t va);
int vmm_copy_to(uint32_t *pd, uint32_t va, const void *src, uint32_t len);
void vmm_read_phys(void *dst, uint32_t phys, uint32_t n);   /* any frame, high memory too */
uint32_t vmm_user_pages(uint32_t *pd);
/* flags: extra page bits, such as PTE_PCD for device registers */
void *vmm_map_mmio(uint32_t phys, uint32_t size, uint32_t flags);  /* boot time only */
void *vmm_map_late(uint32_t phys, uint32_t size, uint32_t flags);  /* any time; kept for good */
int vmm_map_phys(uint32_t *pd, uint32_t va, uint32_t phys, uint32_t size, uint32_t flags);

/*
 * ---- SSE block moves (sse.S) ----
 * Only once fpu_init() has enabled SSE, which sets kernel_sse. Each runs
 * with interrupts off; the page routines take 16-byte aligned pages.
 */
extern int kernel_sse;
void sse_zero_page(void *page);
void sse_copy_page(void *dst, const void *src);     /* streams: dst is not cached */
void sse_copy(void *dst, const void *src, size_t n);    /* forwards: dst < src may overlap */
void sse_fill(void *dst, uint32_t pattern, size_t n);   /* dst and n multiples of 4 */

/* ---- kernel heap ---- */
void heap_init(void);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void *krealloc(void *p, size_t n);
void kfree(void *p);
void heap_stats(uint32_t *total, uint32_t *used);

#endif
