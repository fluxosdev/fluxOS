/* Physical memory, paging and the kernel heap. */
#ifndef MM_H
#define MM_H

#include <stddef.h>
#include <stdint.h>

typedef uint64_t pte_t;                 /* an entry of a page table, at any of the four levels */

/* ---- physical frame allocator ---- */
void pmm_init(void);                        /* from the boot loader's memory map (boot.h) */
void pmm_remap(void);                       /* the direct map moved (paging_init) */
void pmm_reclaim_loader(void);              /* the loader's own memory, once nothing reads it */
void pmm_reserve(uint64_t start, uint64_t end);
void pmm_release(uint64_t start, uint64_t end);
uint32_t pmm_alloc(void);                   /* below 1 GiB; 0 when out of memory */
uint64_t pmm_alloc_user(void);              /* anywhere, above 4 GiB first: for user pages */
uint32_t pmm_alloc_user_low(void);          /* for a user page a 32-bit device address must reach */
uint32_t pmm_alloc_contig(uint32_t frames); /* below 1 GiB, the lowest run there is */
uint32_t pmm_alloc_isa(void);               /* 32 KiB below 16 MiB, for ISA DMA; kept for good */
uint32_t pmm_alloc_reserved(uint32_t reserve); /* cache pages; never reclaim, retain low frames */
void pmm_set_reclaimer(uint32_t (*reclaim)(uint32_t pages)); /* boot-time registration */
int pmm_ref(uint64_t phys);                 /* add a COW/shared reference; 0 if full/invalid */
int pmm_claim_sole(uint64_t phys);          /* 1 if allocated with no extra references */
void pmm_free(uint64_t phys);
uint32_t pmm_total_kb(void);
uint32_t pmm_free_kb(void);
uint32_t pmm_low_free_kb(void);             /* of the first GiB, the kernel's own */
/* The same as kernel memory, through the direct map: NULL when there is none.
 * (virt_to_phys() gives the frame back, to free it or to tell a device.) */
static inline void *pmm_alloc_page(void)
{
    uint32_t phys = pmm_alloc();
    return phys ? phys_to_virt(phys) : NULL;
}
static inline void *pmm_alloc_pages(uint32_t frames)
{
    uint32_t phys = pmm_alloc_contig(frames);
    return phys ? phys_to_virt(phys) : NULL;
}

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
 * the page attribute table is not to be used (cpu.c). */
extern uint32_t pte_wc;

/* An address space is its top-level table (the PML4), by its address in
 * the direct map. The kernel's has no user half. */
extern pte_t *kernel_pd;

void paging_init(void);
uint64_t vmm_boot_cr3(void);                /* tables that also map low memory one to one (trampoline.S, wakeup.S) */
void vmm_switch(pte_t *pd);
pte_t *vmm_create(void);
void vmm_destroy(pte_t *pd);
pte_t *vmm_clone(pte_t *pd);
int vmm_alloc(pte_t *pd, uint64_t va);      /* map a zeroed user page */
int vmm_alloc_prot(pte_t *pd, uint64_t va, uint32_t prot, uint32_t flags);
int vmm_protect_page(pte_t *pd, uint64_t va, uint32_t prot);
void vmm_free_page(pte_t *pd, uint64_t va);
void vmm_free_range(pte_t *pd, uint64_t start, uint64_t end);    /* its pages: page-aligned */
int vmm_pte_present(pte_t *pd, uint64_t va);
int vmm_mmio_mapped(pte_t *pd, uint64_t va);    /* device memory there (the screen) */
uint64_t vmm_translate(pte_t *pd, uint64_t va);     /* a user page's frame (and offset), or 0 */
uint32_t vmm_translate_low(pte_t *pd, uint64_t va); /* for a device: moved below 4 GiB if need be */
int vmm_write_ok(pte_t *pd, uint64_t va);
int vmm_resolve_cow(pte_t *pd, uint64_t va);
int vmm_copy_to(pte_t *pd, uint64_t va, const void *src, uint32_t len);
void vmm_read_phys(void *dst, uint64_t phys, uint32_t n);   /* any frame */
uint32_t vmm_user_pages(pte_t *pd);
/* flags: extra page bits, such as PTE_PCD for device registers. Device
 * memory may be anywhere in the 64-bit physical address space. */
void *vmm_map_mmio(uint64_t phys, uint64_t size, uint32_t flags);  /* 2 MiB pages */
void vmm_unmap_mmio(void *virt);            /* undo it (or pci_map_bar), boot time only */
void *vmm_map_late(uint64_t phys, uint64_t size, uint32_t flags);  /* 4 KiB pages; kept for good */
int vmm_map_phys(pte_t *pd, uint64_t va, uint64_t phys, uint32_t size, uint32_t flags);
void vmm_zero_frame(uint64_t phys);         /* any frame */
int vmm_map_frame(pte_t *pd, uint64_t va, uint32_t frame, uint32_t prot);

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
