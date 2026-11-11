/*
 * Paging: four levels of tables, 64-bit entries. Every address space
 * shares the kernel half (the top-level table's upper 256 entries, which
 * all point at the same lower tables): all of physical memory mapped at
 * hhdm_base with 2 MiB global pages (the direct map, supervisor only),
 * device memory mapped from KERNEL_MMIO_BASE with 2 MiB pages, a window
 * of 4 KiB pages for what is mapped a page at a time, such as ACPI's
 * firmware memory, and the kernel image in the top 2 GiB. The tables of
 * all of these hang off top-level entries made at boot, so a mapping
 * added to them later is in every address space at once.
 *
 * The user half is the 2 GiB from USER_BASE, of 4 KiB pages: processes
 * are 32-bit programs. Page tables, and any frame, are reached through
 * the direct map. User pages marked PTE_MMIO map device memory, lent to
 * one process: never freed, and not copied into a forked child, which
 * has no claim on the device.
 *
 * An address space can be loaded on several processors at once (the
 * threads of one process share it), so a change to its mappings is
 * flushed here and on every other processor that has it loaded
 * (smp_flush_tlb).
 */
#include "kernel.h"
#include "boot.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "smp.h"
#include "x86.h"

#define ENTRIES    512
#define LARGE_PAGE 0x200000ull
#define FRAME(e)   ((e) & 0x000FFFFFFFFFF000ull)
/* The index into the table of a level: 3 the top (PML4), 0 the page tables. */
#define INDEX(va, level) (((uint64_t)(va) >> (12 + 9 * (level))) & (ENTRIES - 1))

#define HHDM_BASE     0xFFFF800000000000ull     /* the direct map, in the kernel's own tables */
#define KERNEL_PML4   256                       /* the first top-level entry of the kernel half */
/* The page tables of the user half, 2 MiB each. */
#define USER_TABLES   ((USER_TOP - USER_BASE) >> 21)
#define USER_TABLE_VA(n) (USER_BASE + ((uint32_t)(n) << 21))

#define CR0_WP  (1u << 16)
#define CR4_PGE (1u << 7)

uint64_t hhdm_base;                     /* the boot loader's until paging_init() */
pte_t *kernel_pd;
static uint64_t kernel_phys;            /* where the image is in physical memory */
static pte_t *boot_pd;                  /* the kernel half, and the first 2 MiB one to one */
static uint64_t mmio_next = KERNEL_MMIO_BASE;
#define MMIO_WINDOWS 32                 /* vmm_map_mmio()'s windows, for vmm_unmap_mmio() */
static struct { uint64_t start, end; } mmio_windows[MMIO_WINDOWS];
static uint64_t late_next = KERNEL_LATE_BASE;
static spinlock_t kmap_lock = SPINLOCK_INIT("vmm-late");    /* the kernel half's device mappings */

uint32_t virt_to_phys(const volatile void *virt)
{
    uint64_t a = (uint64_t)(uintptr_t)virt, pa;
    if (!virt)
        return 0;
    if (a >= KERNEL_VIRT_BASE)
        pa = a - KERNEL_VIRT_BASE + kernel_phys;
    else if (a >= hhdm_base && a < KERNEL_MMIO_BASE)
        pa = a - hhdm_base;
    else
        panic("virt_to_phys: %p is not kernel memory", virt);
    if (pa >> 32)
        panic("virt_to_phys: %p is above 4 GiB (%lx)", virt, pa);
    return (uint32_t)pa;
}

int virt_dma_ok(const void *virt, uint32_t len)
{
    uint64_t a = (uint64_t)(uintptr_t)virt;
    return a >= hhdm_base + 0x100000 && a + len <= hhdm_base + 0x100000000ull;
}

/* Of a page table (always in the direct map, the loader's at first). */
static uint64_t direct_phys(const pte_t *t)
{
    return (uint64_t)(uintptr_t)t - hhdm_base;
}

static pte_t *table(pte_t e)
{
    return phys_to_virt(FRAME(e));
}

/* The first page tables are made before fpu_init() enables SSE. */
static void zero_page(uint32_t phys)
{
    void *p = phys_to_virt(phys);
    if (kernel_sse)
        sse_zero_page(p);
    else
        memset(p, 0, PAGE_SIZE);
}

static void copy_page(uint32_t to, uint32_t from)
{
    if (kernel_sse)
        sse_copy_page(phys_to_virt(to), phys_to_virt(from));
    else
        memcpy(phys_to_virt(to), phys_to_virt(from), PAGE_SIZE);
}

static pte_t *alloc_table(void)
{
    uint32_t phys = pmm_alloc();
    if (!phys)
        return NULL;
    zero_page(phys);
    return phys_to_virt(phys);
}

/*
 * The entry for va in pd's table of a level (0: the page table's, 1: the
 * one that maps 2 MiB), or NULL if a table on the way is missing; with
 * create, missing tables are made, with these flags.
 */
static pte_t *walk(pte_t *pd, uint64_t va, int level, int create, uint32_t flags)
{
    pte_t *t = pd;
    for (int l = 3; l > level; l--) {
        pte_t *e = &t[INDEX(va, l)];
        if (!(*e & PTE_PRESENT)) {
            if (!create)
                return NULL;
            pte_t *n = alloc_table();
            if (!n)
                return NULL;
            *e = direct_phys(n) | flags;
        } else if (*e & PTE_LARGE) {
            return NULL;                /* a large page maps it: there is no table below */
        }
        t = table(*e);
    }
    return &t[INDEX(va, level)];
}

#define KTABLE (PTE_PRESENT | PTE_WRITE)
#define UTABLE (PTE_PRESENT | PTE_WRITE | PTE_USER)

/* Physical [start, end) into the direct map, in 2 MiB pages. */
static void map_direct(pte_t *pd, uint64_t start, uint64_t end)
{
    for (uint64_t pa = start & ~(LARGE_PAGE - 1); pa < end; pa += LARGE_PAGE) {
        pte_t *e = walk(pd, HHDM_BASE + pa, 1, 1, KTABLE);
        if (!e)
            panic("paging: out of memory");
        *e = pa | PTE_PRESENT | PTE_WRITE | PTE_LARGE | PTE_GLOBAL;
    }
}

/*
 * The kernel's own page tables, in place of the boot loader's. They are
 * built through the loader's direct map (the frames they are made of are
 * memory it reported usable, so it mapped them); the kernel's direct map
 * is at HHDM_BASE whatever the loader's was, and takes in the whole first
 * 4 GiB (where the firmware's tables and the legacy BIOS areas are, which
 * the loader's need not map) and all memory above it.
 */
void paging_init(void)
{
    hhdm_base = boot.hhdm;
    kernel_phys = boot.kernel_phys;
    if (boot.kernel_virt != KERNEL_VIRT_BASE)
        panic("the kernel was loaded at %lx, not where it was linked", boot.kernel_virt);

    pte_t *pd = alloc_table();
    if (!pd)
        panic("paging: out of memory");
    map_direct(pd, 0, 0x100000000ull);
    for (int i = 0; i < boot.nmem; i++) {
        const struct boot_mem *m = &boot.mem[i];
        if (m->type == BOOT_MEM_RESERVED || m->type == BOOT_MEM_FRAMEBUFFER)
            continue;
        if (m->base + m->len > 0x100000000ull)
            map_direct(pd, MAX(m->base, 0x100000000ull), m->base + m->len);
    }
    /* The image, where the loader put it. */
    for (uint64_t va = (uint64_t)(uintptr_t)kernel_start; va < (uint64_t)(uintptr_t)kernel_end; va += PAGE_SIZE) {
        pte_t *e = walk(pd, va, 0, 1, KTABLE);
        if (!e)
            panic("paging: out of memory");
        *e = (va - KERNEL_VIRT_BASE + kernel_phys) | PTE_PRESENT | PTE_WRITE | PTE_GLOBAL;
    }
    /* The top-level entries of the device windows, so that every address
     * space made from now on shares what is mapped in them later. */
    if (!walk(pd, KERNEL_MMIO_BASE, 2, 1, KTABLE) || !walk(pd, KERNEL_LATE_BASE, 2, 1, KTABLE))
        panic("paging: out of memory");

    /* For a processor starting in real mode (trampoline.S, wakeup.S): the
     * same, with the first 2 MiB where their addresses say as well. */
    pte_t *bpd = alloc_table();
    pte_t *low = bpd ? walk(bpd, 0, 1, 1, KTABLE) : NULL;
    if (!low)
        panic("paging: out of memory");
    *low = 0 | PTE_PRESENT | PTE_WRITE | PTE_LARGE;
    memcpy(bpd + KERNEL_PML4, pd + KERNEL_PML4, (ENTRIES - KERNEL_PML4) * sizeof(pte_t));

    uint64_t pd_phys = direct_phys(pd), bpd_phys = direct_phys(bpd);
    write_cr3(pd_phys);
    hhdm_base = HHDM_BASE;
    kernel_pd = phys_to_virt(pd_phys);
    boot_pd = phys_to_virt(bpd_phys);
    /* Global pages on (which also empties the TLB of the loader's
     * mappings), and read-only pages honoured in ring 0. */
    write_cr4(read_cr4() & ~(uint64_t)CR4_PGE);
    write_cr4(read_cr4() | CR4_PGE);
    write_cr0(read_cr0() | CR0_WP);
}

uint64_t vmm_boot_cr3(void)
{
    return virt_to_phys(boot_pd);
}

/*
 * Map physical memory into the late window, at any time, a page at a
 * time. Nothing was mapped there before, so no processor can have a stale
 * translation (not-present entries are never cached) and none needs
 * flushing. The mapping stays for good; callers keep what they map.
 */
void *vmm_map_late(uint64_t phys, uint64_t size, uint32_t flags)
{
    uint64_t base = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = (phys + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!size || end <= base)
        return NULL;
    spin_lock(&kmap_lock);
    uint64_t va = late_next;
    for (uint64_t pa = base; pa != end; pa += PAGE_SIZE, late_next += PAGE_SIZE) {
        pte_t *e = walk(kernel_pd, late_next, 0, 1, KTABLE);
        if (!e) {
            spin_unlock(&kmap_lock);
            return NULL;
        }
        *e = pa | PTE_PRESENT | PTE_WRITE | PTE_GLOBAL | flags;
    }
    spin_unlock(&kmap_lock);
    return (void *)(uintptr_t)(va + (phys - base));
}

void vmm_switch(pte_t *pd)
{
    /* Announce pd before loading it, so that a flush racing with the
     * switch cannot miss this processor (write_cr3 serializes). */
    this_cpu()->pd = pd;
    uint64_t phys = virt_to_phys(pd);
    if (read_cr3() != phys)
        write_cr3(phys);
}

pte_t *vmm_create(void)
{
    pte_t *pd = alloc_table();
    if (!pd)
        return NULL;
    memcpy(pd + KERNEL_PML4, kernel_pd + KERNEL_PML4, (ENTRIES - KERNEL_PML4) * sizeof(pte_t));
    return pd;
}

/*
 * Map device memory into the kernel with 2 MiB pages. Each call gets a
 * window of its own, kept so that vmm_unmap_mmio() can take it back.
 */
void *vmm_map_mmio(uint64_t phys, uint64_t size, uint32_t flags)
{
    uint64_t base = phys & ~(LARGE_PAGE - 1);
    uint64_t end = (phys + size + LARGE_PAGE - 1) & ~(LARGE_PAGE - 1);
    if (!size || end <= base)
        return NULL;
    spin_lock(&kmap_lock);
    uint64_t va = mmio_next;
    if (end - base > KERNEL_LATE_BASE - va) {
        spin_unlock(&kmap_lock);
        return NULL;
    }
    for (uint64_t pa = base; pa != end; pa += LARGE_PAGE, mmio_next += LARGE_PAGE) {
        pte_t *e = walk(kernel_pd, mmio_next, 1, 1, KTABLE);
        if (!e) {
            spin_unlock(&kmap_lock);
            return NULL;
        }
        *e = pa | PTE_PRESENT | PTE_WRITE | PTE_LARGE | PTE_GLOBAL | flags;
    }
    for (int i = 0; i < MMIO_WINDOWS; i++) {
        if (mmio_windows[i].end <= mmio_windows[i].start) {
            mmio_windows[i].start = va;
            mmio_windows[i].end = mmio_next;
            break;
        }
    }
    spin_unlock(&kmap_lock);
    return (void *)(uintptr_t)(va + (phys - base));
}

/*
 * Undo vmm_map_mmio() (or pci_map_bar()): virt is what it returned. Boot time
 * only: once another processor runs, the mappings stay (logged), as its
 * global TLB entries cannot be recalled here. The virtual window is reused
 * if it was the latest.
 */
void vmm_unmap_mmio(void *virt)
{
    uint64_t v = (uint64_t)(uintptr_t)virt;
    for (int i = 0; v && i < MMIO_WINDOWS; i++) {
        uint64_t start = mmio_windows[i].start, end = mmio_windows[i].end;
        if (end <= start || v < start || v >= end)
            continue;
        if (ncpus > 1) {
            klog("vmm: device memory at %lx stays mapped (unmapped after boot)\n", v);
            return;
        }
        spin_lock(&kmap_lock);
        for (uint64_t va = start; va != end; va += LARGE_PAGE) {
            pte_t *e = walk(kernel_pd, va, 1, 0, 0);
            if (e)
                *e = 0;
            invlpg(va);                 /* (a global page: a CR3 reload would keep it) */
        }
        if (end == mmio_next)
            mmio_next = start;
        mmio_windows[i].start = mmio_windows[i].end = 0;
        spin_unlock(&kmap_lock);
        return;
    }
}

static pte_t *get_pte(pte_t *pd, uint32_t va, int create)
{
    return walk(pd, va, 0, create, UTABLE);
}

/* The page table of the n-th 2 MiB of the user half, or NULL. */
static pte_t *user_table(pte_t *pd, uint32_t n)
{
    pte_t *e = walk(pd, USER_TABLE_VA(n), 1, 0, 0);
    return e && (*e & PTE_PRESENT) ? table(*e) : NULL;
}

static void flush_page(pte_t *pd, uint32_t va)
{
    if (read_cr3() == virt_to_phys(pd))
        invlpg(va);
    smp_flush_tlb(pd);              /* other threads of this process */
}

int vmm_alloc(pte_t *pd, uint32_t va)
{
    return vmm_alloc_prot(pd, va, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
}

int vmm_alloc_prot(pte_t *pd, uint32_t va, uint32_t prot, uint32_t flags)
{
    va = ALIGN_DOWN(va, PAGE_SIZE);
    if (va < USER_BASE || va >= USER_TOP)
        return -EFAULT;
    pte_t *pte = get_pte(pd, va, 1);
    if (!pte)
        return -ENOMEM;
    if (*pte & PTE_PRESENT)
        return 0;
    uint32_t frame = pmm_alloc_user();
    if (!frame)
        return -ENOMEM;
    zero_page(frame);
    pte_t bits = PTE_PRESENT;
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

int vmm_protect_page(pte_t *pd, uint32_t va, uint32_t prot)
{
    va = ALIGN_DOWN(va, PAGE_SIZE);
    if (va < USER_BASE || va >= USER_TOP)
        return -EFAULT;
    pte_t *pte = get_pte(pd, va, 0);
    if (!pte || !(*pte & PTE_PRESENT) || (*pte & PTE_MMIO))
        return -ENOMEM;
    pte_t e = *pte;
    if (prot == PROT_NONE) {
        e &= ~(pte_t)(PTE_USER | PTE_WRITE);
    } else if (prot & PROT_WRITE) {
        e |= PTE_USER;
        if ((e & PTE_SHARED) || pmm_claim_sole((uint32_t)FRAME(e))) {
            e = (e & ~(pte_t)PTE_COW) | PTE_WRITE;
        } else {
            e = (e & ~(pte_t)PTE_WRITE) | PTE_COW;
        }
    } else {
        e = (e | PTE_USER) & ~(pte_t)(PTE_WRITE | PTE_COW);
    }
    *pte = e;
    flush_page(pd, va);
    return 0;
}

void vmm_zero_frame(uint32_t phys)
{
    zero_page(phys);
}

/* Map an ordinary frame at va, shared across fork (a shared memory segment's). The caller has
 * taken the reference the mapping holds, which unmapping gives back. */
int vmm_map_frame(pte_t *pd, uint32_t va, uint32_t frame, uint32_t prot)
{
    va = ALIGN_DOWN(va, PAGE_SIZE);
    if (va < USER_BASE || va >= USER_TOP)
        return -EFAULT;
    pte_t *pte = get_pte(pd, va, 1);
    if (!pte)
        return -ENOMEM;
    if (*pte & PTE_PRESENT)
        return -EBUSY;
    pte_t bits = PTE_PRESENT | PTE_SHARED;
    if (prot != PROT_NONE)
        bits |= PTE_USER;
    if (prot & PROT_WRITE)
        bits |= PTE_WRITE;
    *pte = frame | bits;
    flush_page(pd, va);
    return 0;
}

/* Map [phys, phys + size) into a user address space as device memory. */
int vmm_map_phys(pte_t *pd, uint32_t va, uint64_t phys, uint32_t size, uint32_t flags)
{
    if (va < USER_BASE || va + size > USER_STACK_LIMIT || va + size < va)
        return -EINVAL;
    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        pte_t *pte = get_pte(pd, va + off, 1);
        if (!pte)
            return -ENOMEM;
        if ((*pte & (PTE_PRESENT | PTE_MMIO)) == PTE_PRESENT)
            return -EBUSY;                      /* ordinary memory is there */
        *pte = (phys + off) | PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_MMIO | flags;
        flush_page(pd, va + off);
    }
    return 0;
}

void vmm_free_page(pte_t *pd, uint32_t va)
{
    pte_t *pte = get_pte(pd, ALIGN_DOWN(va, PAGE_SIZE), 0);
    if (pte && (*pte & PTE_PRESENT)) {
        pte_t e = *pte;
        *pte = 0;
        flush_page(pd, va);             /* no processor may still write to it once it is reused */
        if (!(e & PTE_MMIO))
            pmm_free((uint32_t)FRAME(e));
    }
}

int vmm_pte_present(pte_t *pd, uint32_t va)
{
    if (va < USER_BASE || va >= USER_TOP)
        return 0;
    pte_t *pte = get_pte(pd, va, 0);
    return pte && (*pte & (PTE_PRESENT | PTE_MMIO)) == PTE_PRESENT;
}

/* Is device memory mapped at va (the screen at USER_MMIO_BASE, by fbmap)? */
int vmm_mmio_mapped(pte_t *pd, uint32_t va)
{
    if (va < USER_BASE || va >= USER_TOP)
        return 0;
    pte_t *pte = get_pte(pd, va, 0);
    return pte && (*pte & (PTE_PRESENT | PTE_MMIO)) == (PTE_PRESENT | PTE_MMIO);
}

uint32_t vmm_translate(pte_t *pd, uint32_t va)
{
    if (va < USER_BASE || va >= USER_TOP)
        return 0;
    pte_t *pte = get_pte(pd, va, 0);
    if (!pte || (*pte & (PTE_PRESENT | PTE_USER | PTE_MMIO)) != (PTE_PRESENT | PTE_USER))
        return 0;                               /* device memory or PROT_NONE is not RAM */
    return (uint32_t)FRAME(*pte) | (va & 0xFFF);
}

int vmm_write_ok(pte_t *pd, uint32_t va)
{
    if (va < USER_BASE || va >= USER_TOP)
        return 0;
    pte_t *pte = get_pte(pd, va, 0);
    if (!pte)
        return 0;
    pte_t req = PTE_PRESENT | PTE_USER | PTE_WRITE;
    return (*pte & (req | PTE_MMIO)) == req;
}

int vmm_resolve_cow(pte_t *pd, uint32_t va)
{
    va = ALIGN_DOWN(va, PAGE_SIZE);
    if (va < USER_BASE || va >= USER_TOP)
        return 0;
    pte_t *pte = get_pte(pd, va, 0);
    if (!pte || (*pte & (PTE_PRESENT | PTE_USER | PTE_MMIO)) != (PTE_PRESENT | PTE_USER))
        return 0;
    if (*pte & PTE_WRITE)
        return 1;
    if (!(*pte & PTE_COW))
        return 0;
    uint32_t old_frame = (uint32_t)FRAME(*pte);
    if (pmm_claim_sole(old_frame)) {
        *pte = (*pte & ~(pte_t)PTE_COW) | PTE_WRITE;
        flush_page(pd, va);
        return 1;
    }
    uint32_t new_frame = pmm_alloc_user();
    if (!new_frame)
        return 0;
    copy_page(new_frame, old_frame);
    *pte = new_frame | ((*pte & 0xFFFu & ~(pte_t)PTE_COW) | PTE_WRITE);
    flush_page(pd, va);
    pmm_free(old_frame);
    return 1;
}

/* Copy n bytes of physical memory, any frame, into kernel memory. */
void vmm_read_phys(void *dst, uint32_t phys, uint32_t n)
{
    memcpy(dst, phys_to_virt(phys), n);
}

/* Copy kernel memory into another address space, a page at a time. */
int vmm_copy_to(pte_t *pd, uint32_t va, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    while (len) {
        if (!vmm_write_ok(pd, va) && !vmm_resolve_cow(pd, va))
            return -EFAULT;
        uint32_t phys = vmm_translate(pd, va);
        if (!phys)
            return -EFAULT;
        uint32_t chunk = MIN(len, PAGE_SIZE - (va & 0xFFF));
        memcpy(phys_to_virt(phys), s, chunk);
        s += chunk;
        va += chunk;
        len -= chunk;
    }
    return 0;
}

/* The user half's pages and every table above them, and the top one. */
void vmm_destroy(pte_t *pd)
{
    if (!pd || pd == kernel_pd)
        return;
    if (pd[0] & PTE_PRESENT) {
        pte_t *pdpt = table(pd[0]);
        for (int i = 0; i < ENTRIES; i++) {
            if (!(pdpt[i] & PTE_PRESENT))
                continue;
            pte_t *dir = table(pdpt[i]);
            for (int j = 0; j < ENTRIES; j++) {
                if (!(dir[j] & PTE_PRESENT))
                    continue;
                pte_t *pt = table(dir[j]);
                for (int k = 0; k < ENTRIES; k++)
                    if ((pt[k] & (PTE_PRESENT | PTE_MMIO)) == PTE_PRESENT)
                        pmm_free((uint32_t)FRAME(pt[k]));
                pmm_free((uint32_t)FRAME(dir[j]));
            }
            pmm_free((uint32_t)FRAME(pdpt[i]));
        }
        pmm_free((uint32_t)FRAME(pd[0]));
    }
    pmm_free(virt_to_phys(pd));
}

static void flush_pd(pte_t *pd)
{
    if (read_cr3() == virt_to_phys(pd))
        write_cr3(read_cr3());
    smp_flush_tlb(pd);              /* other threads of this process */
}

/* Copy-on-write clone of the user half (fork), without the device memory.
 * Pages lent to the GPU stay writable in the parent and are copied for the child.
 * The parent's pages are all made read-only, and flushed, before any is shared:
 * a thread still writing through a stale translation meanwhile writes what the
 * child gets, so the child's copy is of one moment. */
pte_t *vmm_clone(pte_t *src)
{
    pte_t *pd = vmm_create();
    if (!pd)
        return NULL;
    int pid = current ? current->leader->pid : 0;
    for (uint32_t i = 0; i < USER_TABLES; i++) {
        pte_t *spt = user_table(src, i);
        if (!spt)
            continue;
        uint32_t base = USER_TABLE_VA(i);
        int pde_pinned = pid > 0 && fb_pinned(pid, base, base + (uint32_t)LARGE_PAGE);
        for (int j = 0; j < ENTRIES; j++) {
            uint32_t va = base | ((uint32_t)j << 12);
            if ((spt[j] & (PTE_PRESENT | PTE_MMIO)) == PTE_PRESENT &&
                (spt[j] & (PTE_WRITE | PTE_COW)) && !(spt[j] & PTE_SHARED) &&
                !(pde_pinned && fb_pinned(pid, va, va + PAGE_SIZE)))
                spt[j] = (spt[j] & ~(pte_t)PTE_WRITE) | PTE_COW;
        }
    }
    flush_pd(src);
    for (uint32_t i = 0; i < USER_TABLES; i++) {
        pte_t *spt = user_table(src, i);
        if (!spt)
            continue;
        uint32_t base = USER_TABLE_VA(i);
        pte_t *de = walk(pd, base, 1, 1, UTABLE);
        pte_t *dpt = de ? alloc_table() : NULL;
        if (!dpt)
            goto fail;
        *de = direct_phys(dpt) | UTABLE;
        int pde_pinned = pid > 0 && fb_pinned(pid, base, base + (uint32_t)LARGE_PAGE);
        for (int j = 0; j < ENTRIES; j++) {
            if ((spt[j] & (PTE_PRESENT | PTE_MMIO)) != PTE_PRESENT)
                continue;
            uint32_t va = base | ((uint32_t)j << 12);
            int pinned = pde_pinned && fb_pinned(pid, va, va + PAGE_SIZE);
            if (!pinned && pmm_ref((uint32_t)FRAME(spt[j]))) {
                dpt[j] = spt[j];        /* (made copy-on-write above) */
                continue;
            }
            uint32_t frame = pmm_alloc_user();
            if (!frame)
                goto fail;
            copy_page(frame, (uint32_t)FRAME(spt[j]));
            pte_t eflags = spt[j] & 0xFFFu & ~(pte_t)(PTE_COW | PTE_SHARED);
            if (spt[j] & PTE_COW)
                eflags |= PTE_WRITE;
            dpt[j] = frame | eflags;
        }
    }
    return pd;
fail:
    vmm_destroy(pd);
    return NULL;
}

uint32_t vmm_user_pages(pte_t *pd)
{
    uint32_t n = 0;
    if (!pd || pd == kernel_pd)
        return 0;
    for (uint32_t i = 0; i < USER_TABLES; i++) {
        pte_t *pt = user_table(pd, i);
        if (!pt)
            continue;
        for (int j = 0; j < ENTRIES; j++)
            if ((pt[j] & (PTE_PRESENT | PTE_MMIO)) == PTE_PRESENT)
                n++;
    }
    return n;
}
