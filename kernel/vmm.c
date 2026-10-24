/*
 * Paging. Every address space shares the kernel half: the first GiB of
 * physical memory identity-mapped with 4 MiB global pages (supervisor
 * only), and device memory (the framebuffer) mapped at boot above 3 GiB.
 * The user half (1 GiB - 3 GiB) uses ordinary 4 KiB pages, and page
 * tables are reached through the identity map. User pages marked
 * PTE_MMIO map device memory: they are shared on fork, never freed.
 */
#include "kernel.h"
#include "mm.h"
#include "x86.h"

#define PDE_INDEX(va) ((va) >> 22)
#define PTE_INDEX(va) (((va) >> 12) & 0x3FF)
#define FRAME(e)      ((e) & ~0xFFFu)

#define KERNEL_PDES (KERNEL_SPACE_END >> 22)
#define USER_PDE_FIRST PDE_INDEX(USER_BASE)
#define USER_PDE_END   PDE_INDEX(USER_TOP)

uint32_t *kernel_pd;
static uint32_t mmio_next = KERNEL_MMIO_BASE;

static uint32_t *alloc_table(void)
{
    uint32_t phys = pmm_alloc();
    if (!phys)
        return NULL;
    memset((void *)phys, 0, PAGE_SIZE);
    return (uint32_t *)phys;
}

void paging_init(void)
{
    kernel_pd = alloc_table();
    if (!kernel_pd)
        panic("paging: out of memory");
    for (uint32_t i = 0; i < KERNEL_PDES; i++)
        kernel_pd[i] = (i << 22) | PTE_PRESENT | PTE_WRITE | PTE_LARGE | PTE_GLOBAL;

    write_cr4(read_cr4() | (1u << 4) | (1u << 7));      /* PSE, PGE */
    write_cr3((uint32_t)kernel_pd);
    /* PG, WP (honour read-only pages in ring 0); fpu_init() sets the FPU bits */
    write_cr0(read_cr0() | (1u << 31) | (1u << 16));
}

void vmm_switch(uint32_t *pd)
{
    if (read_cr3() != (uint32_t)pd)
        write_cr3((uint32_t)pd);
}

uint32_t *vmm_create(void)
{
    uint32_t *pd = alloc_table();
    if (!pd)
        return NULL;
    memcpy(pd, kernel_pd, KERNEL_PDES * sizeof(uint32_t));
    memcpy(pd + USER_PDE_END, kernel_pd + USER_PDE_END, (1024 - USER_PDE_END) * sizeof(uint32_t));
    return pd;
}

/*
 * Map device memory into the kernel above 3 GiB with 4 MiB pages. Only
 * for use before the first process exists: address spaces copy these
 * entries when they are created.
 */
void *vmm_map_mmio(uint32_t phys, uint32_t size, uint32_t flags)
{
    uint32_t base = ALIGN_DOWN(phys, 0x400000u);
    uint32_t end = ALIGN_UP(phys + size, 0x400000u);
    uint32_t va = mmio_next;
    if (end - base > 0u - va)
        return NULL;
    for (uint32_t pa = base; pa != end; pa += 0x400000u, mmio_next += 0x400000u)
        kernel_pd[PDE_INDEX(mmio_next)] = pa | PTE_PRESENT | PTE_WRITE | PTE_LARGE | PTE_GLOBAL | flags;
    write_cr3(read_cr3());
    return (void *)(va + (phys - base));
}

static uint32_t *get_pte(uint32_t *pd, uint32_t va, int create)
{
    uint32_t *pde = &pd[PDE_INDEX(va)];
    if (!(*pde & PTE_PRESENT)) {
        if (!create)
            return NULL;
        uint32_t *pt = alloc_table();
        if (!pt)
            return NULL;
        *pde = (uint32_t)pt | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    uint32_t *pt = (uint32_t *)FRAME(*pde);
    return &pt[PTE_INDEX(va)];
}

static void flush_if_current(uint32_t *pd, uint32_t va)
{
    if (read_cr3() == (uint32_t)pd)
        invlpg(va);
}

int vmm_alloc(uint32_t *pd, uint32_t va)
{
    va = ALIGN_DOWN(va, PAGE_SIZE);
    if (va < USER_BASE || va >= USER_TOP)
        return -EFAULT;
    uint32_t *pte = get_pte(pd, va, 1);
    if (!pte)
        return -ENOMEM;
    if (*pte & PTE_PRESENT)
        return 0;
    uint32_t frame = pmm_alloc();
    if (!frame)
        return -ENOMEM;
    memset((void *)frame, 0, PAGE_SIZE);
    *pte = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
    flush_if_current(pd, va);
    return 0;
}

/* Map [phys, phys + size) into a user address space as device memory. */
int vmm_map_phys(uint32_t *pd, uint32_t va, uint32_t phys, uint32_t size, uint32_t flags)
{
    if (va < USER_BASE || va + size > USER_STACK_LIMIT || va + size < va)
        return -EINVAL;
    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        uint32_t *pte = get_pte(pd, va + off, 1);
        if (!pte)
            return -ENOMEM;
        if ((*pte & (PTE_PRESENT | PTE_MMIO)) == PTE_PRESENT)
            return -EBUSY;                      /* ordinary memory is there */
        *pte = (phys + off) | PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_MMIO | flags;
        flush_if_current(pd, va + off);
    }
    return 0;
}

void vmm_free_page(uint32_t *pd, uint32_t va)
{
    uint32_t *pte = get_pte(pd, ALIGN_DOWN(va, PAGE_SIZE), 0);
    if (pte && (*pte & PTE_PRESENT)) {
        if (!(*pte & PTE_MMIO))
            pmm_free(FRAME(*pte));
        *pte = 0;
        flush_if_current(pd, va);
    }
}

uint32_t vmm_translate(uint32_t *pd, uint32_t va)
{
    if (va < USER_BASE || va >= USER_TOP)
        return 0;
    uint32_t *pte = get_pte(pd, va, 0);
    if (!pte || (*pte & (PTE_PRESENT | PTE_MMIO)) != PTE_PRESENT)
        return 0;                               /* device memory is not RAM */
    return FRAME(*pte) | (va & 0xFFF);
}

/* Copy into another address space through the identity map. */
int vmm_copy_to(uint32_t *pd, uint32_t va, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint32_t phys = vmm_translate(pd, va);
        if (!phys)
            return -EFAULT;
        uint32_t chunk = MIN(len, PAGE_SIZE - (va & 0xFFF));
        memcpy((void *)phys, s, chunk);
        s += chunk;
        va += chunk;
        len -= chunk;
    }
    return 0;
}

void vmm_destroy(uint32_t *pd)
{
    if (!pd || pd == kernel_pd)
        return;
    for (uint32_t i = USER_PDE_FIRST; i < USER_PDE_END; i++) {
        if (!(pd[i] & PTE_PRESENT))
            continue;
        uint32_t *pt = (uint32_t *)FRAME(pd[i]);
        for (int j = 0; j < 1024; j++)
            if ((pt[j] & (PTE_PRESENT | PTE_MMIO)) == PTE_PRESENT)
                pmm_free(FRAME(pt[j]));
        pmm_free((uint32_t)pt);
    }
    pmm_free((uint32_t)pd);
}

/* Deep copy of the user half (fork). */
uint32_t *vmm_clone(uint32_t *src)
{
    uint32_t *pd = vmm_create();
    if (!pd)
        return NULL;
    for (uint32_t i = USER_PDE_FIRST; i < USER_PDE_END; i++) {
        if (!(src[i] & PTE_PRESENT))
            continue;
        uint32_t *spt = (uint32_t *)FRAME(src[i]);
        uint32_t *dpt = alloc_table();
        if (!dpt)
            goto fail;
        pd[i] = (uint32_t)dpt | PTE_PRESENT | PTE_WRITE | PTE_USER;
        for (int j = 0; j < 1024; j++) {
            if (!(spt[j] & PTE_PRESENT))
                continue;
            if (spt[j] & PTE_MMIO) {
                dpt[j] = spt[j];
                continue;
            }
            uint32_t frame = pmm_alloc();
            if (!frame)
                goto fail;
            memcpy((void *)frame, (void *)FRAME(spt[j]), PAGE_SIZE);
            dpt[j] = frame | (spt[j] & 0xFFF);
        }
    }
    return pd;
fail:
    vmm_destroy(pd);
    return NULL;
}

uint32_t vmm_user_pages(uint32_t *pd)
{
    uint32_t n = 0;
    if (!pd || pd == kernel_pd)
        return 0;
    for (uint32_t i = USER_PDE_FIRST; i < USER_PDE_END; i++) {
        if (!(pd[i] & PTE_PRESENT))
            continue;
        uint32_t *pt = (uint32_t *)FRAME(pd[i]);
        for (int j = 0; j < 1024; j++)
            if ((pt[j] & (PTE_PRESENT | PTE_MMIO)) == PTE_PRESENT)
                n++;
    }
    return n;
}
