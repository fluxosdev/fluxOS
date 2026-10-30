/*
 * A DPMI 0.9 host: protected mode and extended memory for DOS extenders
 * (DOS/4GW and the like), which then run their 32-bit programs.
 *
 * A program finds the host with INT 2Fh AX=1687h and far-calls the entry
 * point it returns; the CPU is then in protected mode, with selectors for
 * the program's code, data, stack, PSP and environment. From there it
 * uses INT 31h for descriptors, memory and real-mode calls.
 *
 *  - Descriptors live here, not in emulated memory: the CPU asks
 *    dpmi_descriptor() when a segment register is loaded. Selector n*8+7
 *    is descriptor n (an LDT selector at ring 3), whatever its TI bit.
 *  - Memory: once a client starts, mem grows to EXT_MEM, the first MiB
 *    and extended memory after it, linear address = physical address.
 *    INT 31h 0501h hands out blocks above the HMA.
 *  - Interrupts in protected mode go to the vector the client set with
 *    0205h, or to the host's stub for the vector: a ROM stub in a host
 *    code segment, "0F FF id; IRET". The stub reflects the interrupt to
 *    real mode: the CPU switches to real mode on the host's real-mode
 *    stack, runs the real-mode vector, and an IRET to a ROM callback
 *    comes back to protected mode, copying the returned flags into the
 *    interrupt frame. INT 31h itself is handled by its stub.
 *  - INT 31h 0300h-0302h run real-mode code the same way, with the
 *    registers from a structure the client passes; real-mode callbacks
 *    (0303h) and hardware interrupts that happen in real mode while the
 *    client has a protected-mode handler for them go the other way, on
 *    the host's protected-mode stack.
 *
 * Each switch that has to come back pushes a context (struct ctx); they
 * nest, as when the timer interrupt arrives during a reflected DOS call.
 * There is one client at a time, and no virtual memory: locking and
 * paging calls succeed and do nothing. Faults in protected mode end the
 * emulation with a report.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dos.h"

#define EXT_MEM      0x1000000          /* 16 MiB in all */
#define EXT_START    0x110000           /* above the HMA */
#define MAX_DESC     2048
#define MAX_BLOCKS   256
#define MAX_RMCB     16
#define MAX_CTX      32
#define RM_STACK_SEG 0xE000             /* the host's real-mode stack: upper memory no one uses */
#define RM_STACK     0x2000
#define PM_STACK     0x20000            /* the host's protected-mode stack */

/* Real-mode callback ids (stubs in the ROM's F000 segment). */
enum {
    CB_ENTRY = CB_DPMI,                 /* the mode switch entry point */
    CB_RETURN,                          /* back from reflected interrupts and calls */
    CB_RAW_TO_PM,                       /* raw mode switch */
    CB_RETF,                            /* state save/restore: nothing to do */
    CB_RMCB,                            /* CB_RMCB + n: real-mode callback n */
};

/* Protected-mode stub ids: DPMI_CB_PM + vector, then these. */
enum {
    PMS_RMCB_RET = DPMI_CB_PM + 0x100,  /* a callback procedure's IRET lands here */
    PMS_PASSUP_RET,                     /* a passed-up interrupt's IRET */
    PMS_RAW_TO_RM,
    PMS_RETF,
};
#define PMS_OFF(id) (DPMI_STUBS + ((id) - DPMI_CB_PM) * 8)

struct desc {
    uint32_t base, limit;               /* limit in bytes, granularity applied */
    uint8_t access;                     /* P, DPL, S, type */
    uint8_t big;                        /* D/B */
    uint8_t used;
};

struct block {
    uint32_t addr, size;                /* size 0: free slot */
};

struct rmcb {
    int used;
    uint16_t proc_sel;
    uint32_t proc_off;
    uint16_t regs_sel;
    uint32_t regs_off;
};

/* The CPU state besides the general registers. */
struct state {
    uint16_t seg[6];
    uint32_t base[6];
    int pm, code32, stack32;
    uint32_t ip_mask, ip, sp, flags;
};

enum { K_REFLECT, K_SIM, K_RMCB, K_PASSUP };

struct ctx {
    int kind;
    struct state saved;                 /* where to go back to */
    uint32_t regs[8];                   /* K_SIM, K_PASSUP: the general registers */
    uint32_t regs_lin;                  /* K_SIM: the real-mode call structure */
    uint32_t frame;                     /* K_REFLECT, K_SIM: the INT frame to update */
    int hardware;                       /* K_REFLECT: an IRQ, which must leave no trace */
    uint16_t rm_sp;
    uint32_t pm_sp;
};

static int active;
static uint16_t client_psp;
static int client32;
static uint16_t env_seg;                /* the PSP's environment segment, given back at the end */
static struct desc desc[MAX_DESC];
static struct block blocks[MAX_BLOCKS];
static struct rmcb rmcbs[MAX_RMCB];
static struct { uint16_t sel; uint32_t off; } pm_vec[256];
static struct { uint16_t sel; uint32_t off; } exc_vec[32];
static struct ctx ctxs[MAX_CTX];
static int nctx;
static uint16_t code32_sel, code16_sel; /* the host's stub segments */
static uint16_t stack_sel;              /* the host's protected-mode stack */
static uint16_t rmcb_ss_sel;            /* a real-mode callback's view of the real-mode stack */
static uint16_t rm_stack_seg;
static uint16_t rm_sp;                  /* the free top of the real-mode stack */
static uint32_t pm_sp;                  /* the free top of the protected-mode stack */
static uint32_t pm_stack_lin;
static uint32_t ext_end;

/* ---- descriptors ---- */

static uint16_t sel_of(int i)
{
    return (uint16_t)(i << 3 | 7);
}

static struct desc *desc_of(uint16_t sel)
{
    int i = sel >> 3;
    return i > 0 && i < MAX_DESC && desc[i].used ? &desc[i] : NULL;
}

int dpmi_descriptor(uint16_t sel, uint32_t *base, uint32_t *limit, uint8_t *access, int *big)
{
    struct desc *d = desc_of(sel);
    if (!d || !(d->access & 0x80))
        return 0;
    *base = d->base;
    *limit = d->limit;
    *access = d->access;
    *big = d->big;
    return 1;
}

/* Allocate n consecutive descriptors: present ring 3 data segments, base and limit 0. */
static int desc_alloc(int n)
{
    for (int i = 1; i + n <= MAX_DESC; i++) {
        int k = 0;
        while (k < n && !desc[i + k].used)
            k++;
        if (k == n) {
            for (k = 0; k < n; k++)
                desc[i + k] = (struct desc){ 0, 0, 0xF2, 0, 1 };
            return i;
        }
        i += k;
    }
    return -1;
}

static uint16_t make_desc(uint32_t base, uint32_t limit, uint8_t access, int big)
{
    int i = desc_alloc(1);
    if (i < 0)
        fatal("DPMI: out of descriptors");
    desc[i] = (struct desc){ base, limit, access, (uint8_t)big, 1 };
    return sel_of(i);
}

static void desc_to_bytes(const struct desc *d, uint8_t b[8])
{
    uint32_t limit = d->limit, g = 0;
    if (limit > 0xFFFFF) {
        limit >>= 12;
        g = 0x80;
    }
    b[0] = (uint8_t)limit;
    b[1] = (uint8_t)(limit >> 8);
    b[2] = (uint8_t)d->base;
    b[3] = (uint8_t)(d->base >> 8);
    b[4] = (uint8_t)(d->base >> 16);
    b[5] = d->access;
    b[6] = (uint8_t)(g | (d->big ? 0x40 : 0) | ((limit >> 16) & 0x0F));
    b[7] = (uint8_t)(d->base >> 24);
}

static void desc_from_bytes(struct desc *d, const uint8_t b[8])
{
    uint32_t limit = b[0] | (uint32_t)b[1] << 8 | (uint32_t)(b[6] & 0x0F) << 16;
    if (b[6] & 0x80)
        limit = limit << 12 | 0xFFF;
    d->limit = limit;
    d->base = b[2] | (uint32_t)b[3] << 8 | (uint32_t)b[4] << 16 | (uint32_t)b[7] << 24;
    d->access = b[5] | 0x10 | 0x60;     /* a code or data segment at ring 3 */
    d->big = (b[6] & 0x40) != 0;
}

/* A selector for real-mode segment seg (INT 31h 0002h): one per segment. */
static uint16_t seg_selector(uint16_t seg)
{
    for (int i = 1; i < MAX_DESC; i++)
        if (desc[i].used == 2 && desc[i].base == (uint32_t)seg << 4)
            return sel_of(i);
    uint16_t sel = make_desc((uint32_t)seg << 4, 0xFFFF, 0xF2, 0);
    desc[sel >> 3].used = 2;            /* kept: such selectors cannot be freed */
    return sel;
}

/* ---- extended memory ---- */

static uint32_t mem_largest(void)
{
    /* The gaps between blocks, which are kept sorted by address. */
    uint32_t largest = 0, at = EXT_START;
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (!blocks[i].size)
            continue;
        if (blocks[i].addr > at && blocks[i].addr - at > largest)
            largest = blocks[i].addr - at;
        at = blocks[i].addr + blocks[i].size;
    }
    if (ext_end > at && ext_end - at > largest)
        largest = ext_end - at;
    return largest;
}

static uint32_t mem_free_total(void)
{
    uint32_t used = 0;
    for (int i = 0; i < MAX_BLOCKS; i++)
        used += blocks[i].size;
    return ext_end - EXT_START - used;
}

/* Find room for size bytes; returns the address or 0. */
static uint32_t mem_find(uint32_t size, int ignore)
{
    uint32_t at = EXT_START;
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (!blocks[i].size || i == ignore)
            continue;
        if (blocks[i].addr >= at && blocks[i].addr - at >= size)
            return at;
        if (blocks[i].addr + blocks[i].size > at)
            at = blocks[i].addr + blocks[i].size;
    }
    return ext_end - at >= size && at < ext_end ? at : 0;
}

static int block_cmp(const void *a, const void *b)
{
    const struct block *x = a, *y = b;
    if (!x->size || !y->size)
        return !x->size - !y->size;     /* free slots last */
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

/* Handles are block addresses: blocks move around in the sorted table. */
static int block_by_handle(uint32_t h)
{
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (blocks[i].size && blocks[i].addr == h)
            return i;
    return -1;
}

static uint32_t block_alloc(uint32_t size)
{
    if (size > ~0xFFFu)
        return 0;
    size = (size + 0xFFF) & ~0xFFFu;
    if (!size)
        size = 0x1000;
    uint32_t at = mem_find(size, -1);
    int slot;
    for (slot = 0; slot < MAX_BLOCKS && blocks[slot].size; slot++)
        ;
    if (!at || slot == MAX_BLOCKS)
        return 0;
    blocks[slot] = (struct block){ at, size };
    qsort(blocks, MAX_BLOCKS, sizeof(blocks[0]), block_cmp);
    memset(mem + at, 0, size);
    return at;
}

/* ---- switching between the modes ---- */

static void save_state(struct state *s)
{
    memcpy(s->seg, cpu.seg, sizeof(s->seg));
    memcpy(s->base, cpu.base, sizeof(s->base));
    s->pm = cpu.pm;
    s->code32 = cpu.code32;
    s->stack32 = cpu.stack32;
    s->ip_mask = cpu.ip_mask;
    s->ip = cpu.ip;
    s->sp = cpu.r[R_SP];
    s->flags = cpu.flags;
}

static void restore_state(const struct state *s)
{
    memcpy(cpu.seg, s->seg, sizeof(s->seg));
    memcpy(cpu.base, s->base, sizeof(s->base));
    cpu.pm = s->pm;
    cpu.code32 = s->code32;
    cpu.stack32 = s->stack32;
    cpu.ip_mask = s->ip_mask;
    cpu.ip = s->ip;
    cpu.r[R_SP] = s->sp;
}

static struct ctx *push_ctx(int kind)
{
    if (nctx == MAX_CTX)
        fatal("DPMI: mode switches nested too deeply");
    struct ctx *c = &ctxs[nctx++];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    save_state(&c->saved);
    c->rm_sp = rm_sp;
    c->pm_sp = pm_sp;
    return c;
}

/* Into real mode on the host's real-mode stack (protected mode calls this). */
static void to_real_mode(void)
{
    /* A nested switch keeps below a protected-mode handler running on the host's stack. */
    if (cpu.seg[S_SS] == stack_sel && cpu.r[R_SP] - 0x40 < pm_sp)
        pm_sp = (cpu.r[R_SP] - 0x40) & ~3u;
    cpu_real_mode();
    for (int s = 0; s < 6; s++)
        cpu.seg[s] = 0;
    SS = rm_stack_seg;
    cpu.r[R_SP] = rm_sp;
}

/* Into protected mode on the host's stack (real mode calls this). */
static void to_protected_mode(void)
{
    if (SS == rm_stack_seg && SP - 0x40 < rm_sp)
        rm_sp = (uint16_t)((SP - 0x40) & ~1u);
    cpu.pm = 1;
    cpu_load_seg(S_DS, 0);
    cpu_load_seg(S_ES, 0);
    cpu_load_seg(S_FS, 0);
    cpu_load_seg(S_GS, 0);
    cpu_load_seg(S_SS, stack_sel);
    cpu.r[R_SP] = pm_sp;
}

/* Push an interrupt frame in the client's size. */
static void push_frame(uint32_t flags, uint16_t cs, uint32_t ip)
{
    if (client32) {
        cpu_push32(flags);
        cpu_push32(cs);
        cpu_push32(ip);
    } else {
        cpu_push16((uint16_t)flags);
        cpu_push16(cs);
        cpu_push16((uint16_t)ip);
    }
}

static uint32_t frame_flags_addr(uint32_t frame)
{
    return frame + (client32 ? 8 : 4);
}

/* The status flags of a call's result, into the frame its stub's IRET pops. */
static void set_frame_flags(uint32_t frame, uint32_t flags)
{
    uint32_t a = frame_flags_addr(frame);
    uint16_t f = rw(a);
    ww(a, (uint16_t)((f & ~0x08D5u) | (flags & 0x08D5u)));
}

static uint32_t stub_frame(void)
{
    return seg_base(S_SS) + cpu_sp();
}

/* ---- the client's start and end ---- */

static int grow_memory(void)
{
    if (mem_mask == EXT_MEM - 1)
        return 0;
    uint8_t *n = malloc(EXT_MEM + 4);
    if (!n)
        return -1;
    memcpy(n, mem, MEM_SIZE);
    memset(n + MEM_SIZE, 0, EXT_MEM + 4 - MEM_SIZE);
    mem = n;                            /* the first MiB's static array is left unused */
    mem_mask = EXT_MEM - 1;
    return 0;
}

static void reset(void)
{
    memset(desc, 0, sizeof(desc));
    memset(blocks, 0, sizeof(blocks));
    memset(rmcbs, 0, sizeof(rmcbs));
    memset(exc_vec, 0, sizeof(exc_vec));
    nctx = 0;
}

/* The far call to the entry point: AX bit 0 asks for a 32-bit client. */
static void enter(void)
{
    uint16_t ret_ip = cpu_pop16(), ret_cs = cpu_pop16();
    if (active || grow_memory() < 0) {
        CS = ret_cs;
        cpu.ip = ret_ip;
        set_cf(1);
        AX = 0x8011;
        return;
    }
    reset();
    rm_stack_seg = RM_STACK_SEG;
    active = 1;
    client_psp = dos_psp;
    client32 = AX & 1;
    ext_end = EXT_MEM;
    rm_sp = RM_STACK - 2;

    code32_sel = make_desc(0xF0000, 0xFFFF, 0xFA, 1);
    code16_sel = make_desc(0xF0000, 0xFFFF, 0xFA, 0);
    pm_stack_lin = block_alloc(PM_STACK);
    stack_sel = make_desc(pm_stack_lin, PM_STACK - 1, 0xF2, 1);
    pm_sp = PM_STACK - 4;
    rmcb_ss_sel = make_desc(0, 0xFFFF, 0xF2, 0);
    uint16_t stubs = client32 ? code32_sel : code16_sel;
    for (int n = 0; n < 256; n++) {
        pm_vec[n].sel = stubs;
        pm_vec[n].off = DPMI_STUBS + (uint32_t)n * 8;
    }

    /* The program's segments as selectors, and its environment. */
    uint16_t psp = dos_psp;
    uint16_t cs_sel = make_desc((uint32_t)ret_cs << 4, 0xFFFF, 0xFA, 0);
    uint16_t ds_sel = make_desc((uint32_t)DS << 4, 0xFFFF, 0xF2, 0);
    uint16_t ss_sel = SS == DS ? ds_sel : make_desc((uint32_t)SS << 4, 0xFFFF, 0xF2, 0);
    uint16_t psp_sel = make_desc((uint32_t)psp << 4, 0xFF, 0xF2, 0);
    env_seg = rw(ADDR(psp, 0x2C));
    if (env_seg) {
        uint32_t env_len = (uint32_t)rw(ADDR(env_seg - 1, 3)) * 16;
        ww(ADDR(psp, 0x2C), make_desc((uint32_t)env_seg << 4, env_len ? env_len - 1 : 0xFFFF,
                                      0xF2, 0));
    }

    cpu.pm = 1;
    cpu_load_seg(S_CS, cs_sel);
    cpu_load_seg(S_DS, ds_sel);
    cpu_load_seg(S_SS, ss_sel);
    cpu_load_seg(S_ES, psp_sel);
    cpu_load_seg(S_FS, 0);
    cpu_load_seg(S_GS, 0);
    cpu.ip = ret_ip;
    set_cf(0);
    dlog("DPMI: %d-bit client started\n", client32 ? 32 : 16);
}

void dpmi_terminate(uint16_t psp)
{
    if (!active || psp != client_psp)
        return;
    dlog("DPMI: client ended\n");
    if (env_seg)
        ww(ADDR(psp, 0x2C), env_seg);
    active = 0;
    reset();
    cpu_real_mode();
    /* Back in real mode, the protected-mode vectors' stubs are not reached any more. */
}

int dpmi_active(void)
{
    return active;
}

void dpmi_install_check(void)
{
    AX = 0;
    BX = 1;                             /* 32-bit programs are supported */
    CL = 4;                             /* a 486 */
    DX = 0x005A;                        /* version 0.90 */
    SI = 0;                             /* no private data */
    ES = BIOS_SEG;
    DI = CALLBACK_OFF(CB_ENTRY);
}

/* ---- interrupts ---- */

static void reflect(int n, int hardware);

static int is_stub(int n)
{
    return pm_vec[n].sel == (client32 ? code32_sel : code16_sel) &&
           pm_vec[n].off == DPMI_STUBS + (uint32_t)n * 8;
}

void dpmi_interrupt(int n, int hardware)
{
    uint32_t flags = cpu.flags;
    push_frame(flags, cpu.seg[S_CS], cpu.ip);
    cpu.flags &= ~(F_IF | F_TF);
    cpu_load_seg(S_CS, pm_vec[n].sel);
    cpu.ip = pm_vec[n].off & cpu.ip_mask;
    if (hardware && is_stub(n)) {
        /* An IRQ the program left to real mode: reflected from here, so it
         * comes back to the stub's IRET with the interrupted code's flags
         * and registers as they were (a software INT returns results in them). */
        cpu.ip = (cpu.ip + 4) & cpu.ip_mask;
        reflect(n, 1);
    }
}

int dpmi_hooked(int vector)
{
    return active && !is_stub(vector);
}

/* A hardware interrupt in real mode: the client's protected-mode handler gets it. */
int dpmi_irq_in_real_mode(int vector)
{
    if (!active || is_stub(vector))
        return 0;
    struct ctx *c = push_ctx(K_PASSUP);
    memcpy(c->regs, cpu.r, sizeof(c->regs));
    uint32_t flags = cpu.flags;
    to_protected_mode();
    push_frame(flags, client32 ? code32_sel : code16_sel, PMS_OFF(PMS_PASSUP_RET));
    cpu.flags &= ~(F_IF | F_TF);
    cpu_load_seg(S_CS, pm_vec[vector].sel);
    cpu.ip = pm_vec[vector].off & cpu.ip_mask;
    return 1;
}

void dpmi_exception(int n)
{
    static const char *const names[] = {
        "divide error", "debug", "NMI", "breakpoint", "overflow", "bounds", "invalid opcode",
        "no coprocessor", "double fault", "", "invalid TSS", "segment not present",
        "stack fault", "general protection fault", "page fault",
    };
    uint32_t at = seg_base(S_CS) + cpu.ip_start;
    dlog("exception %d at %04x:%08x (%02x %02x %02x %02x)\n"
         "  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n"
         "  ds=%04x es=%04x fs=%04x gs=%04x ss=%04x\n",
         n, cpu.seg[S_CS], cpu.ip_start, rb(at), rb(at + 1), rb(at + 2), rb(at + 3),
         cpu.r[R_AX], cpu.r[R_BX], cpu.r[R_CX], cpu.r[R_DX], cpu.r[R_SI], cpu.r[R_DI],
         cpu.r[R_BP], cpu.r[R_SP], cpu.seg[S_DS], cpu.seg[S_ES], cpu.seg[S_FS],
         cpu.seg[S_GS], cpu.seg[S_SS]);
    fatal("protected-mode %s at %04X:%08X", n < 15 ? names[n] : "exception",
          cpu.seg[S_CS], cpu.ip_start);
}

/* A vector's stub: reflect the interrupt to real mode. */
static void reflect(int n, int hardware)
{
    struct ctx *c = push_ctx(K_REFLECT);
    c->frame = stub_frame();
    c->hardware = hardware;
    if (hardware)
        memcpy(c->regs, cpu.r, sizeof(c->regs));
    uint32_t caller_flags = client32 ? rd(frame_flags_addr(c->frame)) : rw(frame_flags_addr(c->frame));
    to_real_mode();
    cpu_push16((uint16_t)caller_flags);
    cpu_push16(BIOS_SEG);
    cpu_push16(CALLBACK_OFF(CB_RETURN));
    cpu.flags = (caller_flags & ~(F_IF | F_TF)) | 0x0002;
    CS = rw((uint32_t)n * 4 + 2);
    cpu.ip = rw((uint32_t)n * 4);
}

/* ---- INT 31h ---- */

#define RM_EDI 0
#define RM_ESI 4
#define RM_EBP 8
#define RM_EBX 16
#define RM_EDX 20
#define RM_ECX 24
#define RM_EAX 28
#define RM_FLAGS 32
#define RM_ES 34
#define RM_DS 36
#define RM_FS 38
#define RM_GS 40
#define RM_IP 42
#define RM_CS 44
#define RM_SP 46
#define RM_SS 48

static const int rm_reg_off[8] = { RM_EAX, RM_ECX, RM_EDX, RM_EBX, -1, RM_EBP, RM_ESI, RM_EDI };

static uint32_t reg_v(int r)
{
    return client32 ? cpu.r[r] : REG16(r);
}

static uint32_t es_edi(void)
{
    return seg_base(S_ES) + reg_v(R_DI);
}

static void fail(uint16_t code)
{
    AX = code;
    set_cf(1);
}

/* INT 31h 0300h-0302h: run real-mode code with the registers in ES:(E)DI. */
static void simulate(int fn)
{
    uint32_t regs = es_edi();
    uint32_t words = CX;
    struct ctx *c = push_ctx(K_SIM);
    memcpy(c->regs, cpu.r, sizeof(c->regs));
    c->regs_lin = regs;
    c->frame = stub_frame();
    uint32_t params = c->frame + (client32 ? 12 : 6);
    uint8_t vector = BL;

    to_real_mode();
    uint16_t ss = rw(regs + RM_SS), sp = rw(regs + RM_SP);
    if (ss || sp) {
        SS = ss;
        SP = sp;
    }
    for (uint32_t i = words; i-- > 0; )
        cpu_push16(rw(params + i * 2));
    for (int r = 0; r < 8; r++)
        if (rm_reg_off[r] >= 0)
            cpu.r[r] = rd(regs + (uint32_t)rm_reg_off[r]);
    ES = rw(regs + RM_ES);
    DS = rw(regs + RM_DS);
    cpu.seg[S_FS] = rw(regs + RM_FS);
    cpu.seg[S_GS] = rw(regs + RM_GS);
    uint16_t flags = rw(regs + RM_FLAGS);
    if (fn != 0x0301)
        cpu_push16(flags);
    cpu_push16(BIOS_SEG);
    cpu_push16(CALLBACK_OFF(CB_RETURN));
    cpu.flags = ((fn == 0x0301 ? flags : flags & ~(F_IF | F_TF)) & 0x7FD5) | 0x0002;
    if (fn == 0x0300) {
        CS = rw((uint32_t)vector * 4 + 2);
        cpu.ip = rw((uint32_t)vector * 4);
    } else {
        CS = rw(regs + RM_CS);
        cpu.ip = rw(regs + RM_IP);
    }
}

static void get_mem_info(void)
{
    uint32_t a = es_edi(), largest = mem_largest(), free_pages = mem_free_total() / 4096;
    for (int i = 0; i < 12; i++)
        wd(a + (uint32_t)i * 4, 0xFFFFFFFF);
    wd(a, largest);
    wd(a + 4, largest / 4096);          /* unlocked pages available */
    wd(a + 8, largest / 4096);          /* lockable pages */
    wd(a + 12, (ext_end - EXT_START) / 4096);   /* linear address space */
    wd(a + 16, free_pages);
    wd(a + 20, free_pages);
    wd(a + 24, (ext_end - EXT_START) / 4096);   /* physical pages */
    wd(a + 28, free_pages);
    wd(a + 32, 0);                      /* no paging file */
}

static void int31(void)
{
    static uint8_t seen[0x10000 / 8];
    uint16_t fn = AX;
    if (!(seen[fn >> 3] & (1 << (fn & 7)))) {
        seen[fn >> 3] |= (uint8_t)(1 << (fn & 7));
        dlog("DPMI: INT 31h AX=%04X\n", fn);
    }
    set_cf(0);
    struct desc *d;
    switch (fn) {
    case 0x0000: {                      /* allocate descriptors */
        int i = desc_alloc(CX ? CX : 1);
        if (i < 0) {
            fail(0x8011);
            return;
        }
        AX = sel_of(i);
        return;
    }
    case 0x0001:                        /* free a descriptor */
        if (!(d = desc_of(BX)) || d->used != 1) {
            fail(0x8022);
            return;
        }
        d->used = 0;
        for (int s = 0; s < 6; s++)     /* segment registers holding it become null */
            if (cpu.seg[s] == BX && s != S_CS && s != S_SS)
                cpu_load_seg(s, 0);
        return;
    case 0x0002:                        /* real-mode segment to descriptor */
        AX = seg_selector(BX);
        return;
    case 0x0003:                        /* selector increment */
        AX = 8;
        return;
    case 0x0006:                        /* get segment base */
        if (!(d = desc_of(BX))) {
            fail(0x8022);
            return;
        }
        CX = (uint16_t)(d->base >> 16);
        DX = (uint16_t)d->base;
        return;
    case 0x0007:                        /* set segment base */
        if (!(d = desc_of(BX))) {
            fail(0x8022);
            return;
        }
        d->base = (uint32_t)CX << 16 | DX;
        break;
    case 0x0008:                        /* set segment limit */
        if (!(d = desc_of(BX))) {
            fail(0x8022);
            return;
        }
        d->limit = (uint32_t)CX << 16 | DX;
        break;
    case 0x0009:                        /* set access rights */
        if (!(d = desc_of(BX))) {
            fail(0x8022);
            return;
        }
        d->access = CL | 0x60;          /* always ring 3; limits are kept in bytes */
        d->big = (CH & 0x40) != 0;
        break;
    case 0x000A: {                      /* create an alias (data) descriptor */
        if (!(d = desc_of(BX))) {
            fail(0x8022);
            return;
        }
        struct desc copy = *d;
        int i = desc_alloc(1);
        if (i < 0) {
            fail(0x8011);
            return;
        }
        desc[i] = copy;
        desc[i].access = (uint8_t)((copy.access & 0xF0) | 0x02);
        desc[i].used = 1;
        AX = sel_of(i);
        return;
    }
    case 0x000B: {                      /* get descriptor */
        uint8_t b[8];
        if (!(d = desc_of(BX))) {
            fail(0x8022);
            return;
        }
        desc_to_bytes(d, b);
        mem_write(es_edi(), b, 8);
        return;
    }
    case 0x000C: {                      /* set descriptor */
        uint8_t b[8];
        if (!(d = desc_of(BX))) {
            fail(0x8022);
            return;
        }
        mem_read(es_edi(), b, 8);
        desc_from_bytes(d, b);
        break;
    }
    case 0x000D: {                      /* allocate a specific descriptor */
        int i = BX >> 3;
        if (i <= 0 || i >= 0x10 || desc[i].used) {
            fail(0x8011);
            return;
        }
        desc[i] = (struct desc){ 0, 0, 0xF2, 0, 1 };
        return;
    }
    case 0x0100: {                      /* allocate DOS memory */
        uint16_t seg, largest;
        uint32_t paras = BX;
        int n = (int)((paras + 0xFFF) / 0x1000);
        int err = dos_mem_alloc((uint16_t)paras, &seg, &largest);
        if (err) {
            BX = largest;
            fail((uint16_t)err);
            return;
        }
        int i = desc_alloc(n ? n : 1);
        if (i < 0) {
            dos_mem_free(seg);
            fail(0x8011);
            return;
        }
        for (int k = 0; k < (n ? n : 1); k++) {
            uint32_t left = paras * 16 - (uint32_t)k * 0x10000;
            desc[i + k].base = ((uint32_t)seg << 4) + (uint32_t)k * 0x10000;
            desc[i + k].limit = (k == 0 ? paras * 16 : (left < 0x10000 ? left : 0x10000)) - 1;
        }
        AX = seg;
        DX = sel_of(i);
        return;
    }
    case 0x0101:                        /* free DOS memory */
        if (!(d = desc_of(DX))) {
            fail(0x8022);
            return;
        }
        if (dos_mem_free((uint16_t)(d->base >> 4))) {
            fail(9);
            return;
        }
        d->used = 0;
        return;
    case 0x0102: {                      /* resize DOS memory */
        uint16_t max;
        if (!(d = desc_of(DX))) {
            fail(0x8022);
            return;
        }
        int err = dos_mem_resize((uint16_t)(d->base >> 4), BX, &max);
        if (err) {
            BX = max;
            fail((uint16_t)err);
            return;
        }
        d->limit = (uint32_t)BX * 16 - 1;
        return;
    }
    case 0x0200:                        /* get real-mode interrupt vector */
        DX = rw((uint32_t)BL * 4);
        CX = rw((uint32_t)BL * 4 + 2);
        return;
    case 0x0201:                        /* set real-mode interrupt vector */
        ww((uint32_t)BL * 4, DX);
        ww((uint32_t)BL * 4 + 2, CX);
        return;
    case 0x0202:                        /* get exception handler */
        if (BL >= 32) {
            fail(0x8021);
            return;
        }
        CX = exc_vec[BL].sel ? exc_vec[BL].sel : (client32 ? code32_sel : code16_sel);
        cpu.r[R_DX] = exc_vec[BL].sel ? exc_vec[BL].off : PMS_OFF(PMS_RETF);
        return;
    case 0x0203:                        /* set exception handler: kept, not called */
        if (BL >= 32) {
            fail(0x8021);
            return;
        }
        exc_vec[BL].sel = CX;
        exc_vec[BL].off = reg_v(R_DX);
        return;
    case 0x0204:                        /* get protected-mode interrupt vector */
        CX = pm_vec[BL].sel;
        if (client32)
            cpu.r[R_DX] = pm_vec[BL].off;
        else
            DX = (uint16_t)pm_vec[BL].off;
        return;
    case 0x0205:                        /* set protected-mode interrupt vector */
        if (!desc_of(CX)) {
            fail(0x8022);
            return;
        }
        pm_vec[BL].sel = CX;
        pm_vec[BL].off = reg_v(R_DX);
        return;
    case 0x0300:                        /* simulate a real-mode interrupt */
    case 0x0301:                        /* call a real-mode far procedure */
    case 0x0302:                        /* call a real-mode interrupt procedure */
        simulate(fn);
        return;
    case 0x0303: {                      /* allocate a real-mode callback */
        int k = 0;
        while (k < MAX_RMCB && rmcbs[k].used)
            k++;
        if (k == MAX_RMCB) {
            fail(0x8015);
            return;
        }
        rmcbs[k] = (struct rmcb){ 1, DS, reg_v(R_SI), ES, reg_v(R_DI) };
        CX = BIOS_SEG;
        DX = CALLBACK_OFF(CB_RMCB + k);
        return;
    }
    case 0x0304:                        /* free a real-mode callback */
        for (int k = 0; k < MAX_RMCB; k++)
            if (CX == BIOS_SEG && DX == CALLBACK_OFF(CB_RMCB + k) && rmcbs[k].used) {
                rmcbs[k].used = 0;
                return;
            }
        fail(0x8024);
        return;
    case 0x0305:                        /* state save/restore: none needed */
        AX = 0;
        BX = BIOS_SEG;
        CX = CALLBACK_OFF(CB_RETF);
        SI = client32 ? code32_sel : code16_sel;
        cpu.r[R_DI] = PMS_OFF(PMS_RETF);
        return;
    case 0x0306:                        /* raw mode switch addresses */
        BX = BIOS_SEG;
        CX = CALLBACK_OFF(CB_RAW_TO_PM);
        SI = client32 ? code32_sel : code16_sel;
        cpu.r[R_DI] = PMS_OFF(PMS_RAW_TO_RM);
        return;
    case 0x0400:                        /* version */
        AX = 0x005A;
        BX = 0x0005;                    /* 32-bit, interrupts reflected to real mode */
        CL = 4;
        DH = 0x08;
        DL = 0x70;
        return;
    case 0x0500:
        get_mem_info();
        return;
    case 0x0501: {                      /* allocate a memory block */
        uint32_t a = block_alloc((uint32_t)BX << 16 | CX);
        if (!a) {
            fail(0x8013);
            return;
        }
        BX = SI = (uint16_t)(a >> 16);
        CX = DI = (uint16_t)a;
        return;
    }
    case 0x0502: {                      /* free a memory block */
        int i = block_by_handle((uint32_t)SI << 16 | DI);
        if (i < 0) {
            fail(0x8023);
            return;
        }
        blocks[i].size = 0;
        qsort(blocks, MAX_BLOCKS, sizeof(blocks[0]), block_cmp);
        return;
    }
    case 0x0503: {                      /* resize a memory block */
        int i = block_by_handle((uint32_t)SI << 16 | DI);
        uint32_t size = (uint32_t)BX << 16 | CX;
        if (i < 0) {
            fail(0x8023);
            return;
        }
        if (size > ~0xFFFu) {
            fail(0x8013);
            return;
        }
        size = (size + 0xFFF) & ~0xFFFu;
        uint32_t old = blocks[i].addr, old_size = blocks[i].size;
        uint32_t next = ext_end;
        for (int k = 0; k < MAX_BLOCKS; k++)
            if (blocks[k].size && blocks[k].addr > old && blocks[k].addr < next)
                next = blocks[k].addr;
        if (size <= next - old) {       /* in place */
            if (size > old_size)
                memset(mem + old + old_size, 0, size - old_size);
            blocks[i].size = size;
        } else {
            uint32_t a = mem_find(size, i);
            if (!a) {
                fail(0x8013);
                return;
            }
            memmove(mem + a, mem + old, old_size);
            memset(mem + a + old_size, 0, size - old_size);
            blocks[i] = (struct block){ a, size };
            qsort(blocks, MAX_BLOCKS, sizeof(blocks[0]), block_cmp);
            old = a;
        }
        BX = SI = (uint16_t)(old >> 16);
        CX = DI = (uint16_t)old;
        return;
    }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603:    /* lock and unlock */
    case 0x0702: case 0x0703:                               /* paging hints */
    case 0x0801:                                            /* free a physical mapping */
        return;
    case 0x0604:                        /* page size */
        BX = 0;
        CX = 0x1000;
        return;
    case 0x0800: {                      /* map physical memory: the same addresses */
        uint32_t phys = (uint32_t)BX << 16 | CX;
        if (phys >= ext_end) {
            fail(0x8021);
            return;
        }
        return;
    }
    case 0x0900:                        /* disable interrupts, return the old state */
    case 0x0901:
    case 0x0902: {
        uint32_t a = frame_flags_addr(stub_frame());
        uint16_t f = rw(a);
        AL = (f & F_IF) != 0;
        if (fn == 0x0900)
            ww(a, (uint16_t)(f & ~F_IF));
        else if (fn == 0x0901)
            ww(a, (uint16_t)(f | F_IF));
        return;
    }
    case 0x0A00:                        /* no vendor extensions */
    default:
        dlog("DPMI: unsupported INT 31h AX=%04X\n", fn);
        fail(0x8001);
        return;
    }
    /* Descriptor changes reach segment registers that hold the selector. */
    for (int s = 0; s < 6; s++)
        if ((cpu.seg[s] >> 3) == (BX >> 3) && (cpu.seg[s] & 0xFFFC))
            cpu_load_seg(s, cpu.seg[s]);
}

/* ---- the stubs' callbacks ---- */

static void return_from_real_mode(void)
{
    if (!nctx)
        fatal("DPMI: return to protected mode with nothing to return to");
    struct ctx *c = &ctxs[--nctx];
    uint32_t flags = cpu.flags;
    if (c->kind == K_SIM) {
        uint32_t regs = c->regs_lin;
        for (int r = 0; r < 8; r++)
            if (rm_reg_off[r] >= 0)
                wd(regs + (uint32_t)rm_reg_off[r], cpu.r[r]);
        ww(regs + RM_FLAGS, (uint16_t)flags);
        ww(regs + RM_ES, ES);
        ww(regs + RM_DS, DS);
        ww(regs + RM_FS, cpu.seg[S_FS]);
        ww(regs + RM_GS, cpu.seg[S_GS]);
        memcpy(cpu.r, c->regs, sizeof(c->regs));
    } else if (c->hardware) {
        memcpy(cpu.r, c->regs, sizeof(c->regs));
    }
    restore_state(&c->saved);
    cpu.flags = c->saved.flags;         /* the stub's, until its IRET */
    rm_sp = c->rm_sp;
    pm_sp = c->pm_sp;
    if (c->kind == K_SIM) {
        uint32_t a = frame_flags_addr(c->frame);
        ww(a, (uint16_t)(rw(a) & ~F_CF));
    } else if (!c->hardware) {
        set_frame_flags(c->frame, flags);
    }
}

static void rmcb_enter(int k)
{
    if (k >= MAX_RMCB || !rmcbs[k].used)
        fatal("DPMI: call to a freed real-mode callback");
    struct rmcb *cb = &rmcbs[k];
    push_ctx(K_RMCB);
    struct desc *rd_ = desc_of(cb->regs_sel);
    uint32_t regs = (rd_ ? rd_->base : 0) + cb->regs_off;
    for (int r = 0; r < 8; r++)
        if (rm_reg_off[r] >= 0)
            wd(regs + (uint32_t)rm_reg_off[r], cpu.r[r]);
    ww(regs + RM_FLAGS, (uint16_t)cpu.flags);
    ww(regs + RM_ES, ES);
    ww(regs + RM_DS, DS);
    ww(regs + RM_FS, cpu.seg[S_FS]);
    ww(regs + RM_GS, cpu.seg[S_GS]);
    ww(regs + RM_IP, (uint16_t)(cpu.ip - 4));
    ww(regs + RM_CS, CS);
    ww(regs + RM_SP, SP);
    ww(regs + RM_SS, SS);
    uint32_t flags = cpu.flags;
    uint16_t rss = SS, rsp = SP;

    to_protected_mode();
    desc[rmcb_ss_sel >> 3].base = (uint32_t)rss << 4;
    cpu_load_seg(S_DS, rmcb_ss_sel);
    cpu.r[R_SI] = rsp;
    cpu_load_seg(S_ES, cb->regs_sel);
    cpu.r[R_DI] = cb->regs_off;
    push_frame(flags, client32 ? code32_sel : code16_sel, PMS_OFF(PMS_RMCB_RET));
    cpu.flags &= ~(F_IF | F_TF);
    cpu_load_seg(S_CS, cb->proc_sel);
    cpu.ip = cb->proc_off & cpu.ip_mask;
}

/* The callback procedure returned: real mode resumes with the registers it left in ES:(E)DI. */
static void rmcb_return(void)
{
    uint32_t regs = es_edi();
    if (!nctx)
        fatal("DPMI: callback return with nothing to return to");
    struct ctx *c = &ctxs[--nctx];
    for (int r = 0; r < 8; r++)
        if (rm_reg_off[r] >= 0)
            cpu.r[r] = rd(regs + (uint32_t)rm_reg_off[r]);
    uint16_t flags = rw(regs + RM_FLAGS);
    uint16_t es = rw(regs + RM_ES), ds = rw(regs + RM_DS), fs = rw(regs + RM_FS), gs = rw(regs + RM_GS);
    uint16_t ip = rw(regs + RM_IP), cs = rw(regs + RM_CS), sp = rw(regs + RM_SP), ss = rw(regs + RM_SS);
    cpu_real_mode();
    ES = es;
    DS = ds;
    cpu.seg[S_FS] = fs;
    cpu.seg[S_GS] = gs;
    CS = cs;
    SS = ss;
    cpu.r[R_SP] = sp;
    cpu.ip = ip;
    cpu.flags = (flags & 0x7FD5) | 0x0002;
    rm_sp = c->rm_sp;
    pm_sp = c->pm_sp;
}

static void passup_return(void)
{
    if (!nctx)
        fatal("DPMI: interrupt return with nothing to return to");
    struct ctx *c = &ctxs[--nctx];
    memcpy(cpu.r, c->regs, sizeof(c->regs));
    restore_state(&c->saved);
    cpu.flags = c->saved.flags;
    rm_sp = c->rm_sp;
    pm_sp = c->pm_sp;
}

/* Raw switches: AX DS, CX ES, DX SS, (E)BX (E)SP, SI CS, (E)DI (E)IP. */
static void raw_to_pm(void)
{
    uint16_t ds = AX, es = CX, ss = DX, cs = SI;
    uint32_t sp = cpu.r[R_BX], ip = cpu.r[R_DI];
    cpu.pm = 1;
    cpu_load_seg(S_DS, ds);
    cpu_load_seg(S_ES, es);
    cpu_load_seg(S_SS, ss);
    cpu_load_seg(S_FS, 0);
    cpu_load_seg(S_GS, 0);
    cpu_load_seg(S_CS, cs);
    cpu.r[R_SP] = cpu.stack32 ? sp : sp & 0xFFFF;
    cpu.ip = ip & cpu.ip_mask;
}

static void raw_to_rm(void)
{
    uint16_t ds = AX, es = CX, ss = DX, cs = SI, sp = BX, ip = DI;
    cpu_real_mode();
    DS = ds;
    ES = es;
    SS = ss;
    cpu.seg[S_FS] = cpu.seg[S_GS] = 0;
    CS = cs;
    cpu.r[R_SP] = sp;
    cpu.ip = ip;
}

void dpmi_callback(int id)
{
    if (id == CB_ENTRY) {
        enter();
        return;
    }
    if (!active)
        fatal("DPMI call %#x without a client", id);
    if (id >= DPMI_CB_PM && id < DPMI_CB_PM + 0x100) {
        int n = id - DPMI_CB_PM;
        if (n == 0x31) {
            uint32_t frame = stub_frame();
            int before = nctx;
            int31();
            if (nctx == before)         /* finished here (0300h-0302h finish later) */
                set_frame_flags(frame, cpu.flags);
        } else if (n == 0x2F && AX == 0x1686) {
            AX = 0;                     /* in protected mode */
        } else {
            reflect(n, 0);
        }
        return;
    }
    switch (id) {
    case CB_RETURN:
        return_from_real_mode();
        return;
    case CB_RAW_TO_PM:
        raw_to_pm();
        return;
    case PMS_RMCB_RET:
        rmcb_return();
        return;
    case PMS_PASSUP_RET:
        passup_return();
        return;
    case PMS_RAW_TO_RM:
        raw_to_rm();
        return;
    }
    if (id >= CB_RMCB && id < CB_RMCB + MAX_RMCB) {
        rmcb_enter(id - CB_RMCB);
        return;
    }
    fatal("unknown DPMI callback %#x", id);
}

/* The ROM stubs: called from bios_init. */
void dpmi_rom_init(void)
{
    uint8_t *rom = mem + 0xF0000;
    for (int n = 0; n < 0x104; n++) {
        int id = DPMI_CB_PM + n;
        uint8_t *p = rom + DPMI_STUBS + n * 8;
        p[0] = 0x0F;
        p[1] = 0xFF;
        p[2] = (uint8_t)id;
        p[3] = (uint8_t)(id >> 8);
        p[4] = 0xCF;                    /* IRET, or IRETD in the 32-bit stub segment */
    }
    rom[PMS_OFF(PMS_RETF)] = 0xCB;      /* RETF */
    rom[CALLBACK_OFF(CB_RETF)] = 0xCB;
    for (int id = CB_ENTRY; id < CB_RMCB + MAX_RMCB; id++) {
        if (id == CB_RETF)
            continue;
        uint8_t *p = rom + CALLBACK_OFF(id);
        p[0] = 0x0F;
        p[1] = 0xFF;
        p[2] = (uint8_t)id;
        p[3] = (uint8_t)(id >> 8);
    }
}
