/*
 * Paging. Every address space shares the kernel half: the first GiB of
 * physical memory identity-mapped with 4 MiB global pages (supervisor
 * only), and device memory (the framebuffer) mapped at boot above 3 GiB.
 * Above that is a window of 4 KiB pages for what is mapped later, such
 * as ACPI's firmware memory: its page tables exist from the start, so
 * every address space has them, and filling one in reaches them all.
 * Just below it, each processor has two pages through which it reaches
 * user pages in high memory (past the first GiB) for a moment: kmap.
 * The user half (1 GiB - 3 GiB) uses ordinary 4 KiB pages, and page
 * tables are reached through the identity map. User pages marked
 * PTE_MMIO map device memory, lent to one process: never freed, and
 * not copied into a forked child, which has no claim on the device.
 *
 * An address space can be loaded on several processors at once (the
 * threads of one process share it), so a change to its mappings is
 * flushed here and on every other processor that has it loaded
 * (smp_flush_tlb).
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "smp.h"
#include "x86.h"

#define PDE_INDEX(va) ((va) >> 22)
#define PTE_INDEX(va) (((va) >> 12) & 0x3FF)
#define FRAME(e)      ((e) & ~0xFFFu)

#define KERNEL_PDES (KERNEL_SPACE_END >> 22)
#define USER_PDE_FIRST PDE_INDEX(USER_BASE)
#define USER_PDE_END   PDE_INDEX(USER_TOP)

uint32_t *kernel_pd;
static uint32_t mmio_next = KERNEL_MMIO_BASE;

#define LATE_PDES 4                     /* 16 MiB of late mappings */
static uint32_t late_base, late_next;   /* the window, and its first unused page */
static uint32_t kmap_base;              /* the temporary mappings' 4 MiB, below it */
static uint32_t *kmap_pt;
static uint32_t *late_pt[LATE_PDES];
static spinlock_t late_lock = SPINLOCK_INIT("vmm-late");

/*
 * A frame through this processor's temporary slot (0 or 1), or the
 * identity map below 1 GiB. Interrupts stay off from here to the last
 * use: the slot is this processor's, and nothing else touches the
 * address, so only this processor's TLB needs telling.
 */
static void *kmap(uint32_t phys, int slot)
{
    if (phys < KERNEL_SPACE_END) {
        void *p = (void *)phys;
        __asm__("" : "+r"(p));
        return p;
    }
    uint32_t va = kmap_base + (uint32_t)(this_cpu()->index * 2 + slot) * PAGE_SIZE;
    kmap_pt[PTE_INDEX(va)] = ALIGN_DOWN(phys, PAGE_SIZE) | PTE_PRESENT | PTE_WRITE;
    invlpg(va);
    return (void *)(va + (phys & (PAGE_SIZE - 1)));     /* the offset in the page kept */
}

/* The first page tables are made before fpu_init() enables SSE. */
static void zero_page(uint32_t phys)
{
    uint32_t flags = irq_save();
    void *p = kmap(phys, 0);
    if (kernel_sse)
        sse_zero_page(p);
    else
        memset(p, 0, PAGE_SIZE);
    irq_restore(flags);
}

static uint32_t *alloc_table(void)
{
    uint32_t phys = pmm_alloc();
    if (!phys)
        return NULL;
    zero_page(phys);
    return (uint32_t *)phys;
}

void paging_init(void)
{
    kernel_pd = alloc_table();
    if (!kernel_pd)
        panic("paging: out of memory");
    for (uint32_t i = 0; i < KERNEL_PDES; i++)
        kernel_pd[i] = (i << 22) | PTE_PRESENT | PTE_WRITE | PTE_LARGE | PTE_GLOBAL;

    /* The late window, at the very top, and the temporary mappings below it. */
    late_base = late_next = 0u - LATE_PDES * 0x400000u;
    for (int i = 0; i < LATE_PDES; i++) {
        late_pt[i] = alloc_table();
        if (!late_pt[i])
            panic("paging: out of memory");
        kernel_pd[PDE_INDEX(late_base) + (uint32_t)i] = (uint32_t)late_pt[i] | PTE_PRESENT | PTE_WRITE;
    }
    kmap_base = late_base - 0x400000u;
    kmap_pt = alloc_table();
    if (!kmap_pt)
        panic("paging: out of memory");
    kernel_pd[PDE_INDEX(kmap_base)] = (uint32_t)kmap_pt | PTE_PRESENT | PTE_WRITE;

    write_cr4(read_cr4() | (1u << 4) | (1u << 7));      /* PSE, PGE */
    write_cr3((uint32_t)kernel_pd);
    /* PG, WP (honour read-only pages in ring 0); fpu_init() sets the FPU bits */
    write_cr0(read_cr0() | (1u << 31) | (1u << 16));
}

/*
 * Map physical memory into the late window, at any time. Nothing was
 * mapped there before, so no processor can have a stale translation
 * (not-present entries are never cached) and none needs flushing. The
 * mapping stays for good; callers keep what they map.
 */
void *vmm_map_late(uint32_t phys, uint32_t size, uint32_t flags)
{
    uint32_t base = ALIGN_DOWN(phys, PAGE_SIZE);
    uint32_t end = ALIGN_UP(phys + size, PAGE_SIZE);
    if (!size || end <= base)
        return NULL;
    spin_lock(&late_lock);
    uint32_t va = late_next;
    if (end - base > late_base + LATE_PDES * 0x400000u - va) {
        spin_unlock(&late_lock);
        return NULL;
    }
    late_next += end - base;
    for (uint32_t pa = base, v = va; pa != end; pa += PAGE_SIZE, v += PAGE_SIZE)
        late_pt[(v - late_base) >> 22][PTE_INDEX(v)] = pa | PTE_PRESENT | PTE_WRITE | PTE_GLOBAL | flags;
    spin_unlock(&late_lock);
    return (void *)(va + (phys - base));
}

void vmm_switch(uint32_t *pd)
{
    /* Announce pd before loading it, so that a flush racing with the
     * switch cannot miss this processor (write_cr3 serializes). */
    this_cpu()->pd = pd;
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
    if (end - base > kmap_base - va)
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

static void flush_page(uint32_t *pd, uint32_t va)
{
    if (read_cr3() == (uint32_t)pd)
        invlpg(va);
    smp_flush_tlb(pd);              /* other threads of this process */
}

int vmm_alloc(uint32_t *pd, uint32_t va)
{
    return vmm_alloc_prot(pd, va, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
}

int vmm_alloc_prot(uint32_t *pd, uint32_t va, uint32_t prot, uint32_t flags)
{
    va = ALIGN_DOWN(va, PAGE_SIZE);
    if (va < USER_BASE || va >= USER_TOP)
        return -EFAULT;
    uint32_t *pte = get_pte(pd, va, 1);
    if (!pte)
        return -ENOMEM;
    if (*pte & PTE_PRESENT)
        return 0;
    uint32_t frame = pmm_alloc_user();
    if (!frame)
        return -ENOMEM;
    zero_page(frame);
    uint32_t bits = PTE_PRESENT;
    if (prot != PROT_NONE)
        bits |= PTE_USER;
    if (prot & PROT_WRITE)
        bits |= PTE_WRITE;
    if (flags & MAP_SHARED)
        bits |= PTE_SHARED;
    *pte = frame | bits;
    flush_page(pd, va);
    return 0;
}

int vmm_protect_page(uint32_t *pd, uint32_t va, uint32_t prot)
{
    va = ALIGN_DOWN(va, PAGE_SIZE);
    if (va < USER_BASE || va >= USER_TOP)
        return -EFAULT;
    uint32_t *pte = get_pte(pd, va, 0);
    if (!pte || !(*pte & PTE_PRESENT) || (*pte & PTE_MMIO))
        return -ENOMEM;
    uint32_t e = *pte;
    if (prot == PROT_NONE) {
        e &= ~(PTE_USER | PTE_WRITE);
    } else if (prot & PROT_WRITE) {
        e |= PTE_USER;
        if ((e & PTE_SHARED) || pmm_claim_sole(FRAME(e))) {
            e = (e & ~PTE_COW) | PTE_WRITE;
        } else {
            e = (e & ~PTE_WRITE) | PTE_COW;
        }
    } else {
        e = (e | PTE_USER) & ~(PTE_WRITE | PTE_COW);
    }
    *pte = e;
    flush_page(pd, va);
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
        flush_page(pd, va + off);
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
        flush_page(pd, va);
    }
}

int vmm_pte_present(uint32_t *pd, uint32_t va)
{
    if (va < USER_BASE || va >= USER_TOP)
        return 0;
    uint32_t *pte = get_pte(pd, va, 0);
    return pte && (*pte & (PTE_PRESENT | PTE_MMIO)) == PTE_PRESENT;
}

uint32_t vmm_translate(uint32_t *pd, uint32_t va)
{
    if (va < USER_BASE || va >= USER_TOP)
        return 0;
    uint32_t *pte = get_pte(pd, va, 0);
    if (!pte || (*pte & (PTE_PRESENT | PTE_USER | PTE_MMIO)) != (PTE_PRESENT | PTE_USER))
        return 0;                               /* device memory or PROT_NONE is not RAM */
    return FRAME(*pte) | (va & 0xFFF);
}

int vmm_write_ok(uint32_t *pd, uint32_t va)
{
    if (va < USER_BASE || va >= USER_TOP)
        return 0;
    uint32_t *pte = get_pte(pd, va, 0);
    if (!pte)
        return 0;
    uint32_t req = PTE_PRESENT | PTE_USER | PTE_WRITE;
    return (*pte & (req | PTE_MMIO)) == req;
}

int vmm_resolve_cow(uint32_t *pd, uint32_t va)
{
    va = ALIGN_DOWN(va, PAGE_SIZE);
    if (va < USER_BASE || va >= USER_TOP)
        return 0;
    uint32_t *pte = get_pte(pd, va, 0);
    if (!pte || (*pte & (PTE_PRESENT | PTE_USER | PTE_MMIO)) != (PTE_PRESENT | PTE_USER))
        return 0;
    if (*pte & PTE_WRITE)
        return 1;
    if (!(*pte & PTE_COW))
        return 0;
    uint32_t old_frame = FRAME(*pte);
    if (pmm_claim_sole(old_frame)) {
        *pte = (*pte & ~PTE_COW) | PTE_WRITE;
        flush_page(pd, va);
        return 1;
    }
    uint32_t new_frame = pmm_alloc_user();
    if (!new_frame)
        return 0;
    uint32_t flags = irq_save();
    if (kernel_sse)
        sse_copy_page(kmap(new_frame, 0), kmap(old_frame, 1));
    else
        memcpy(kmap(new_frame, 0), kmap(old_frame, 1), PAGE_SIZE);
    irq_restore(flags);
    *pte = new_frame | ((*pte & 0xFFFu & ~PTE_COW) | PTE_WRITE);
    flush_page(pd, va);
    pmm_free(old_frame);
    return 1;
}

/* Copy n bytes of physical memory, any frame (high memory included, which
 * the identity map does not reach), into kernel memory. */
void vmm_read_phys(void *dst, uint32_t phys, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t chunk = MIN(n, PAGE_SIZE - (phys & 0xFFF));
        uint32_t flags = irq_save();
        memcpy(d, kmap(phys, 0), chunk);
        irq_restore(flags);
        d += chunk;
        phys += chunk;
        n -= chunk;
    }
}

/* Copy kernel memory into another address space, a page at a time. */
int vmm_copy_to(uint32_t *pd, uint32_t va, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    while (len) {
        if (!vmm_write_ok(pd, va) && !vmm_resolve_cow(pd, va))
            return -EFAULT;
        uint32_t phys = vmm_translate(pd, va);
        if (!phys)
            return -EFAULT;
        uint32_t chunk = MIN(len, PAGE_SIZE - (va & 0xFFF));
        uint32_t flags = irq_save();
        memcpy(kmap(phys, 0), s, chunk);
        irq_restore(flags);
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

static void flush_pd(uint32_t *pd)
{
    if (read_cr3() == (uint32_t)pd)
        write_cr3((uint32_t)pd);
    smp_flush_tlb(pd);              /* other threads of this process */
}

/* Copy-on-write clone of the user half (fork), without the device memory.
 * Pages lent to the GPU stay writable in the parent and are copied for the child. */
uint32_t *vmm_clone(uint32_t *src)
{
    uint32_t *pd = vmm_create();
    if (!pd)
        return NULL;
    int pid = current ? current->leader->pid : 0;
    for (uint32_t i = USER_PDE_FIRST; i < USER_PDE_END; i++) {
        if (!(src[i] & PTE_PRESENT))
            continue;
        uint32_t *spt = (uint32_t *)FRAME(src[i]);
        uint32_t *dpt = alloc_table();
        if (!dpt)
            goto fail;
        pd[i] = (uint32_t)dpt | PTE_PRESENT | PTE_WRITE | PTE_USER;
        int pde_pinned = pid > 0 && fb_pinned(pid, i << 22, (i + 1) << 22);
        for (int j = 0; j < 1024; j++) {
            if ((spt[j] & (PTE_PRESENT | PTE_MMIO)) != PTE_PRESENT)
                continue;
            uint32_t va = (i << 22) | ((uint32_t)j << 12);
            int pinned = pde_pinned && fb_pinned(pid, va, va + PAGE_SIZE);
            if (!pinned && pmm_ref(FRAME(spt[j]))) {
                if ((spt[j] & (PTE_WRITE | PTE_COW)) && !(spt[j] & PTE_SHARED))
                    spt[j] = (spt[j] & ~PTE_WRITE) | PTE_COW;
                dpt[j] = spt[j];
                continue;
            }
            uint32_t frame = pmm_alloc_user();
            if (!frame)
                goto fail;
            uint32_t flags = irq_save();
            if (kernel_sse)
                sse_copy_page(kmap(frame, 0), kmap(FRAME(spt[j]), 1));
            else
                memcpy(kmap(frame, 0), kmap(FRAME(spt[j]), 1), PAGE_SIZE);
            irq_restore(flags);
            uint32_t eflags = spt[j] & 0xFFFu & ~(PTE_COW | PTE_SHARED);
            if (spt[j] & PTE_COW)
                eflags |= PTE_WRITE;
            dpt[j] = frame | eflags;
        }
    }
    flush_pd(src);
    return pd;
fail:
    flush_pd(src);
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
