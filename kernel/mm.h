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
uint32_t pmm_alloc(void);                   /* 0 when out of memory */
uint32_t pmm_alloc_contig(uint32_t frames);
void pmm_free(uint32_t phys);
uint32_t pmm_total_kb(void);
uint32_t pmm_free_kb(void);

/* ---- paging ---- */
#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_USER    0x004
#define PTE_LARGE   0x080
#define PTE_GLOBAL  0x100
#define PTE_MMIO    0x200       /* (available bit) device memory, not ours to free */

extern uint32_t *kernel_pd;

void paging_init(void);
void vmm_switch(uint32_t *pd);
uint32_t *vmm_create(void);
void vmm_destroy(uint32_t *pd);
uint32_t *vmm_clone(uint32_t *pd);
int vmm_alloc(uint32_t *pd, uint32_t va);   /* map a zeroed user page */
void vmm_free_page(uint32_t *pd, uint32_t va);
uint32_t vmm_translate(uint32_t *pd, uint32_t va);
int vmm_copy_to(uint32_t *pd, uint32_t va, const void *src, uint32_t len);
uint32_t vmm_user_pages(uint32_t *pd);
void *vmm_map_mmio(uint32_t phys, uint32_t size);       /* boot time only */
int vmm_map_phys(uint32_t *pd, uint32_t va, uint32_t phys, uint32_t size);

/* ---- kernel heap ---- */
void heap_init(void);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void *krealloc(void *p, size_t n);
void kfree(void *p);
void heap_stats(uint32_t *total, uint32_t *used);

#endif
