/*
 * The DOS emulator's CPU (user/dos/cpu.c) against the host's own: random
 * 32-bit protected-mode instructions run on the host processor and in the
 * emulator from the same registers, flags and memory; the registers, the
 * memory and the flags the instruction defines must agree (make test-dos).
 *
 *   build/tests/dos-cpu [COUNT] [SEED]
 *
 * The host is x86-64, where 32-bit operations behave as on a 386, so the
 * instructions avoid what 64-bit mode lacks or reads differently: 40-4F
 * (REX prefixes there), DAA/DAS/AAA/AAS/AAM/AAD/SALC, and ModR/M disp32
 * alone (RIP-relative there; a SIB byte with no base gives the absolute
 * address instead). No instruction touches ESP.
 */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "dos.h"

/* ---- what cpu.c needs from the rest of the emulator ---- */
uint8_t *mem;
uint32_t mem_mask;
int video_written, vga_graphics;
uint8_t pic_irr;
uint8_t vga_read(uint32_t a) { (void)a; return 0; }
void vga_write(uint32_t a, uint8_t v) { (void)a; (void)v; }
uint32_t io_in(uint16_t p, int s) { (void)p; (void)s; return 0; }
void io_out(uint16_t p, uint32_t v, int s) { (void)p; (void)v; (void)s; }
int pic_pending(void) { return -1; }
void pic_ack(int irq) { (void)irq; }
static int emu_fault;
void dpmi_exception(int n) { emu_fault = n + 1; }
void dpmi_interrupt(int n, int hw) { (void)n; (void)hw; }
int dpmi_irq_in_real_mode(int v) { (void)v; return 0; }
void dpmi_callback(int id) { (void)id; }
void bios_callback(int id) { (void)id; }
int dpmi_descriptor(uint16_t sel, uint32_t *base, uint32_t *limit, uint8_t *access, int *big)
{
    (void)sel; *base = 0; *limit = 0xFFFFFFFF; *access = 0xF2; *big = 1;
    return 1;
}
void dlog(const char *fmt, ...) { (void)fmt; }
void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}

/* ---- the host side ---- */

#define SCRATCH 0x00100000u             /* data, at the same address in both */
#define CODE    0x00200000u             /* the instruction, the same too */
#define TRAMP   0x00300000u
#define SCRATCH_SIZE 256

struct regs { uint32_t r[8]; uint64_t flags; };
static struct regs *in, *out;           /* at fixed addresses, for absolute moves */
static uint64_t *save_esp;
static uint8_t *code, *tramp;
static sigjmp_buf fpe_jmp;

static void on_fpe(int sig) { (void)sig; siglongjmp(fpe_jmp, 1); }

/* op [disp32] through a SIB byte with no base or index (not RIP-relative). */
static uint8_t *emit_abs(uint8_t *p, uint8_t op, uint8_t modrm_reg, uint64_t addr)
{
    uint32_t a = (uint32_t)addr;
    *p++ = op;
    *p++ = (uint8_t)(modrm_reg << 3 | 4);
    *p++ = 0x25;
    memcpy(p, &a, 4);
    return p + 4;
}

/* Save the callee-saved registers and flags; load flags and registers; jump to
 * the instruction; store registers and flags; restore. */
static void build_trampoline(void)
{
    uint8_t *p = tramp;
    static const uint8_t pro[] = { 0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x9C };
    memcpy(p, pro, sizeof(pro));
    p += sizeof(pro);
    *p++ = 0x48;
    p = emit_abs(p, 0x89, R_SP, (uintptr_t)save_esp);   /* mov [save], rsp */
    p = emit_abs(p, 0xFF, 6, (uintptr_t)&in->flags);    /* push qword [in.flags] */
    *p++ = 0x9D;                                        /* popfq */
    for (int r = 0; r < 8; r++)
        if (r != R_SP)
            p = emit_abs(p, 0x8B, (uint8_t)r, (uintptr_t)&in->r[r]);
    *p++ = 0xE9;                                        /* jmp CODE */
    int32_t d = (int32_t)(CODE - ((uint32_t)(uintptr_t)p + 4));
    memcpy(p, &d, 4);
    p += 4;
    /* the instruction jumps back here */
    uint32_t back = (uint32_t)(uintptr_t)p;
    for (int r = 0; r < 8; r++)
        if (r != R_SP)
            p = emit_abs(p, 0x89, (uint8_t)r, (uintptr_t)&out->r[r]);
    *p++ = 0x9C;                                        /* pushfq */
    p = emit_abs(p, 0x8F, 0, (uintptr_t)&out->flags);   /* pop qword [out.flags] */
    *p++ = 0x48;
    p = emit_abs(p, 0x8B, R_SP, (uintptr_t)save_esp);   /* mov rsp, [save] */
    static const uint8_t epi[] = { 0x9D, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B, 0xC3 };
    memcpy(p, epi, sizeof(epi));                        /* popfq; pop r15..rbx; ret */
    memcpy(tramp + 0xF00, &back, 4);
}

static void run_native(const uint8_t *insn, int len)
{
    uint32_t back;
    memcpy(&back, tramp + 0xF00, 4);
    memcpy(code, insn, (size_t)len);
    code[len] = 0xE9;                                   /* jmp back */
    int32_t d = (int32_t)(back - (CODE + (uint32_t)len + 5));
    memcpy(code + len + 1, &d, 4);
    ((void (*)(void))(uintptr_t)TRAMP)();
}

/* ---- random instructions ---- */

static uint32_t rnd(void)
{
    static uint64_t s = 88172645463325252ull;
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return (uint32_t)s;
}
static void seed(uint64_t v) { while (v--) rnd(); }

static uint32_t value(void)
{
    static const uint32_t special[] = { 0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF,
        0x10000, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 16, 31, 32, 33, 9, 17 };
    switch (rnd() % 6) {
    case 0: return special[rnd() % (sizeof(special) / sizeof(special[0]))];
    case 1: return rnd() & 0xFF;
    case 2: return rnd() & 0xFFFF;
    case 3: return (uint32_t)-(int32_t)(rnd() & 0xFFFF);
    default: return rnd();
    }
}

enum { OSZAPC = 0x8D5, F_ALL = OSZAPC };

struct insn {
    uint8_t b[16];
    int len;
    uint32_t fmask;                     /* flags to compare */
    int skip;                           /* undefined result */
    const char *what;
};

/* ModR/M for a register operand or the scratch memory; writes it and returns the length. */
static int modrm(uint8_t *p, int reg, int size, int allow_mem, int *is_mem)
{
    int m = allow_mem && rnd() % 3 == 0;
    *is_mem = m;
    if (m) {
        p[0] = (uint8_t)(reg << 3 | 4);
        p[1] = 0x25;                    /* SIB: no base, no index, disp32 */
        uint32_t a = SCRATCH + (rnd() % (SCRATCH_SIZE - 8));
        memcpy(p + 2, &a, 4);
        return 6;
    }
    int rm;
    do
        rm = (int)(rnd() % 8);
    while (size != 8 && rm == R_SP);
    p[0] = (uint8_t)(0xC0 | reg << 3 | rm);
    return 1;
}

static int gpr(int size)
{
    int r;
    do
        r = (int)(rnd() % 8);
    while (size != 8 && r == R_SP);
    return r;
}

static void gen(struct insn *x)
{
    uint8_t *p = x->b;
    int o16 = rnd() % 4 == 0, size, mem;
    memset(x, 0, sizeof(*x));
    if (o16)
        *p++ = 0x66;
    int vs = o16 ? 16 : 32;
    x->fmask = F_ALL;
    switch (rnd() % 24) {
    case 0: case 1: case 2: {           /* ALU r/m, r and r, r/m */
        int op = (int)(rnd() % 8), form = (int)(rnd() % 4);
        size = form & 1 ? vs : 8;
        *p++ = (uint8_t)(op << 3 | form);
        p += modrm(p, gpr(size), size, 1, &mem);
        if (op == 1 || op == 4 || op == 6)
            x->fmask &= ~F_AF;
        x->what = "alu";
        break;
    }
    case 3: {                           /* ALU AL/eAX, imm */
        int op = (int)(rnd() % 8), wide = rnd() & 1;
        *p++ = (uint8_t)(op << 3 | 4 | wide);
        uint32_t v = value();
        memcpy(p, &v, 4);
        p += wide ? vs / 8 : 1;
        if (op == 1 || op == 4 || op == 6)
            x->fmask &= ~F_AF;
        x->what = "alu imm";
        break;
    }
    case 4: case 5: {                   /* group 1 */
        static const uint8_t ops[] = { 0x80, 0x81, 0x83 };
        uint8_t op = ops[rnd() % 3];
        int sub = (int)(rnd() % 8);
        size = op == 0x80 ? 8 : vs;
        *p++ = op;
        p += modrm(p, sub, size, 1, &mem);
        uint32_t v = value();
        memcpy(p, &v, 4);
        p += op == 0x81 ? vs / 8 : 1;
        if (sub == 1 || sub == 4 || sub == 6)
            x->fmask &= ~F_AF;
        x->what = "group 1";
        break;
    }
    case 6: {                           /* TEST, XCHG, MOV */
        static const uint8_t ops[] = { 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B };
        uint8_t op = ops[rnd() % 8];
        size = op & 1 ? vs : 8;
        *p++ = op;
        p += modrm(p, gpr(size), size, 1, &mem);
        x->fmask &= ~F_AF;
        x->what = "test/xchg/mov";
        break;
    }
    case 7: {                           /* INC/DEC reg, and FE/FF */
        {                               /* (40-4F are REX prefixes on a 64-bit host) */
            int wide = rnd() & 1;
            size = wide ? vs : 8;
            *p++ = wide ? 0xFF : 0xFE;
            p += modrm(p, (int)(rnd() & 1), size, 1, &mem);
        }
        x->what = "inc/dec";
        break;
    }
    case 8: case 9: {                   /* group 3 */
        int wide = rnd() & 1, sub = (int)(rnd() % 8);
        size = wide ? vs : 8;
        *p++ = wide ? 0xF7 : 0xF6;
        p += modrm(p, sub, size, 1, &mem);
        if (sub < 2) {
            uint32_t v = value();
            memcpy(p, &v, 4);
            p += size / 8;
            x->fmask &= ~F_AF;
        } else if (sub == 2) {
            x->fmask = F_ALL;           /* NOT: flags unchanged */
        } else if (sub == 4 || sub == 5) {
            x->fmask = F_OF | F_CF;
        } else if (sub >= 6) {
            x->fmask = 0;
        }
        x->what = sub >= 4 ? "mul/div" : "test/not/neg";
        break;
    }
    case 10: {                          /* IMUL 3-operand and 2-operand */
        int k = (int)(rnd() % 3);
        if (k == 2) {
            *p++ = 0x0F;
            *p++ = 0xAF;
        } else {
            *p++ = k ? 0x6B : 0x69;
        }
        p += modrm(p, gpr(vs), vs, 1, &mem);
        if (k == 0) {
            uint32_t v = value();
            memcpy(p, &v, 4);
            p += vs / 8;
        } else if (k == 1) {
            *p++ = (uint8_t)value();
        }
        x->fmask = F_OF | F_CF;
        x->what = "imul";
        break;
    }
    case 11: case 12: {                 /* shifts and rotates */
        static const uint8_t ops[] = { 0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3 };
        uint8_t op = ops[rnd() % 6];
        int sub = (int)(rnd() % 8);
        size = op & 1 ? vs : 8;
        *p++ = op;
        p += modrm(p, sub, size, 1, &mem);
        uint32_t count = 1;
        if (op <= 0xC1) {
            count = rnd() % 3 ? rnd() % 34 : rnd() & 0xFF;
            *p++ = (uint8_t)count;
        } else if (op >= 0xD2) {
            count = 0xFFFFFFFF;         /* from CL: decided at run time */
        }
        x->fmask = (uint32_t)sub << 8 | (count & 0xFF);
        x->skip = 1000 + size;          /* (flags decided once CL is known) */
        x->what = "shift";
        break;
    }
    case 13: {                          /* SHLD/SHRD */
        static const uint8_t ops[] = { 0xA4, 0xA5, 0xAC, 0xAD };
        uint8_t op = ops[rnd() % 4];
        *p++ = 0x0F;
        *p++ = op;
        p += modrm(p, gpr(vs), vs, 1, &mem);
        uint32_t count = 0xFFFFFFFF;
        if (!(op & 1)) {
            count = rnd() % 34;
            *p++ = (uint8_t)count;
        }
        x->fmask = count & 0xFF;
        x->skip = 2000 + vs;
        x->what = "shld/shrd";
        break;
    }
    case 14: {                          /* BT, BTS, BTR, BTC */
        if (rnd() & 1) {
            static const uint8_t ops[] = { 0xA3, 0xAB, 0xB3, 0xBB };
            *p++ = 0x0F;
            *p++ = ops[rnd() % 4];
            p += modrm(p, gpr(vs), vs, 0, &mem);
        } else {
            *p++ = 0x0F;
            *p++ = 0xBA;
            p += modrm(p, 4 + (int)(rnd() % 4), vs, 1, &mem);
            *p++ = (uint8_t)rnd();
        }
        x->fmask = F_CF;
        x->what = "bt";
        break;
    }
    case 15: {                          /* BSF/BSR, MOVZX/MOVSX */
        static const uint8_t ops[] = { 0xBC, 0xBD, 0xB6, 0xB7, 0xBE, 0xBF };
        uint8_t op = ops[rnd() % 6];
        *p++ = 0x0F;
        *p++ = op;
        p += modrm(p, gpr(vs), op == 0xB6 || op == 0xBE ? 8 : vs, 1, &mem);
        x->fmask = op >= 0xBC ? F_ZF : F_ALL;
        x->skip = op >= 0xBC ? 3000 : 0;   /* destination undefined for a zero source */
        x->what = op >= 0xBC ? "bsf/bsr" : "movzx/movsx";
        break;
    }
    case 16: {                          /* SETcc */
        *p++ = 0x0F;
        *p++ = (uint8_t)(0x90 | (rnd() & 15));
        p += modrm(p, 0, 8, 1, &mem);
        x->what = "setcc";
        break;
    }
    case 17: {                          /* XADD, CMPXCHG */
        static const uint8_t ops[] = { 0xC0, 0xC1, 0xB0, 0xB1 };
        uint8_t op = ops[rnd() % 4];
        size = op & 1 ? vs : 8;
        *p++ = 0x0F;
        *p++ = op;
        p += modrm(p, gpr(size), size, 1, &mem);
        x->what = "xadd/cmpxchg";
        break;
    }
    case 18: {                          /* one-byte odds and ends */
        /* (DAA, DAS, AAA, AAS, AAM, AAD and SALC do not exist on a 64-bit host) */
        static const uint8_t ops[] = { 0x98, 0x99, 0x9E, 0x9F, 0xF5, 0xF8, 0xF9, 0x91, 0x92, 0x93, 0x95,
                                       0x96, 0x97 };
        uint8_t op = ops[rnd() % 13];
        *p++ = op;
        if (op == 0x27 || op == 0x2F)
            x->fmask = F_ALL & ~F_OF;
        else if (op == 0x37 || op == 0x3F)
            x->fmask = F_AF | F_CF;
        x->what = "misc";
        break;
    }
    case 19: {                          /* NEG/NOT/MUL again, in memory */
        *p++ = 0xF7;
        int sub = 2 + (int)(rnd() % 4);
        p += modrm(p, sub, vs, 1, &mem);
        x->fmask = sub < 4 ? F_ALL : F_OF | F_CF;
        x->what = "group 3";
        break;
    }
    case 20: {                          /* BSWAP */
        *p++ = 0x0F;
        *p++ = (uint8_t)(0xC8 | gpr(32));
        x->what = "bswap";
        if (o16)
            x->skip = 1;                /* undefined with 16-bit operands */
        break;
    }
    case 21: case 22: {                 /* LEA: every addressing form but ESP-based */
        int reg = gpr(vs);
        *p++ = 0x8D;
        int md = (int)(rnd() % 3), rm;
        do
            rm = (int)(rnd() % 8);
        while (rm == 5 && md == 0);         /* (RIP-relative on a 64-bit host) */
        *p++ = (uint8_t)(md << 6 | reg << 3 | rm);
        if (rm == 4) {
            int sc = (int)(rnd() % 4), idx = (int)(rnd() % 8), base;
            do
                base = (int)(rnd() % 8);
            while (base == R_SP);
            *p++ = (uint8_t)(sc << 6 | idx << 3 | base);
            if (base == 5 && md == 0) {
                uint32_t v = rnd();
                memcpy(p, &v, 4);
                p += 4;
            }
        } else if (rm == 5 && md == 0) {
            uint32_t v = rnd();
            memcpy(p, &v, 4);
            p += 4;
        }
        if (md == 1)
            *p++ = (uint8_t)rnd();
        else if (md == 2) {
            uint32_t v = rnd();
            memcpy(p, &v, 4);
            p += 4;
        }
        x->what = "lea";
        break;
    }
    default: {                          /* SETcc to a register */
        *p++ = 0x0F;
        *p++ = (uint8_t)(0x90 | (rnd() & 15));
        p += modrm(p, 0, 8, 0, &mem);
        x->what = "setcc";
        break;
    }
    }
    x->len = (int)(p - x->b);
}

/* Flags a shift defines, once the count is known. */
static uint32_t shift_mask(int sub, uint32_t count, int size)
{
    count &= 0x1F;
    if (!count)
        return F_ALL;                   /* unchanged */
    uint32_t m = F_ALL & ~F_AF;
    if (sub <= 3) {                     /* rotates: only CF and OF change */
        m = F_ALL & ~F_AF;
        if (count != 1)
            m &= ~F_OF;
        return m;
    }
    if (count != 1)
        m &= ~F_OF;
    if (count >= (uint32_t)size && size < 32)
        m &= ~(F_CF | F_OF);
    return m;
}

static void dump(const char *tag, const struct regs *r)
{
    printf("  %-6s eax=%08x ecx=%08x edx=%08x ebx=%08x ebp=%08x esi=%08x edi=%08x fl=%04x\n", tag,
           r->r[0], r->r[1], r->r[2], r->r[3], r->r[5], r->r[6], r->r[7], (unsigned)(r->flags & 0xFFF));
}

int main(int argc, char **argv)
{
    long count = argc > 1 ? atol(argv[1]) : 1000000;
    if (argc > 2)
        seed((uint64_t)atol(argv[2]));
    if (mmap((void *)SCRATCH, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
            MAP_FAILED ||
        mmap((void *)CODE, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
             -1, 0) == MAP_FAILED ||
        mmap((void *)TRAMP, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
             -1, 0) == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    code = (uint8_t *)CODE;
    tramp = (uint8_t *)TRAMP;
    in = (struct regs *)(uintptr_t)(TRAMP + 0x1000);
    out = in + 1;
    save_esp = (uint64_t *)(out + 1);
    build_trampoline();
    signal(SIGFPE, on_fpe);

    mem = calloc(1, 0x1000000 + 4);
    mem_mask = 0xFFFFFF;
    cpu_reset();

    static uint8_t scratch0[SCRATCH_SIZE];
    long failures = 0, skipped = 0;
    for (long n = 0; n < count && failures < 20; n++) {
        struct insn x;
        gen(&x);
        for (int r = 0; r < 8; r++)
            in->r[r] = value();
        in->flags = (rnd() & OSZAPC) | 0x202;
        for (int i = 0; i < SCRATCH_SIZE; i++)
            scratch0[i] = (uint8_t)rnd();

        /* the emulator */
        memcpy(mem + SCRATCH, scratch0, SCRATCH_SIZE);
        memcpy(mem + CODE, x.b, (size_t)x.len);
        memset(&cpu.base, 0, sizeof(cpu.base));
        cpu.pm = cpu.code32 = cpu.stack32 = 1;
        cpu.ip_mask = 0xFFFFFFFF;
        cpu.ip = CODE;
        memcpy(cpu.r, in->r, sizeof(cpu.r));
        cpu.r[R_SP] = 0x00800000;
        cpu.flags = (uint32_t)in->flags;
        cpu.inhibit = cpu.halted = 0;
        emu_fault = 0;
        cpu_run(1);

        /* the host */
        memcpy((void *)SCRATCH, scratch0, SCRATCH_SIZE);
        int native_fault = 0;
        if (sigsetjmp(fpe_jmp, 1))
            native_fault = 1;
        else
            run_native(x.b, x.len);

        if (native_fault || emu_fault) {
            if (native_fault != (emu_fault == 1)) {
                printf("FAULT MISMATCH %s: native %d, emulator %d\n", x.what, native_fault, emu_fault);
                goto fail;
            }
            continue;
        }
        if (cpu.ip != CODE + (uint32_t)x.len) {
            printf("LENGTH %s: emulator went to +%u, instruction is %d bytes\n", x.what, cpu.ip - CODE, x.len);
            goto fail;
        }

        uint32_t fmask = x.fmask;
        if (x.skip == 1) {
            skipped++;
            continue;
        }
        if (x.skip >= 1000 && x.skip < 2000) {          /* shift */
            int sub = (int)(fmask >> 8), size = x.skip - 1000;
            uint32_t c = fmask & 0xFF;
            if (x.b[x.len > 1 && x.b[0] == 0x66 ? 1 : 0] >= 0xD2)
                c = in->r[R_CX] & 0xFF;
            if (x.b[x.b[0] == 0x66 ? 1 : 0] == 0xD0 || x.b[x.b[0] == 0x66 ? 1 : 0] == 0xD1)
                c = 1;
            if (x.b[x.b[0] == 0x66 ? 1 : 0] == 0xD2 || x.b[x.b[0] == 0x66 ? 1 : 0] == 0xD3)
                c = in->r[R_CX] & 0xFF;
            int opsz = (x.b[x.b[0] == 0x66 ? 1 : 0] & 1) ? size : 8;
            fmask = shift_mask(sub, c, opsz);
        } else if (x.skip >= 2000 && x.skip < 3000) {   /* shld/shrd */
            int size = x.skip - 2000;
            uint32_t c = fmask & 0xFF;
            if (x.b[(x.b[0] == 0x66) + 1] & 1)
                c = in->r[R_CX] & 0xFF;
            c &= 0x1F;
            if (size == 16 && c > 16) {
                skipped++;
                continue;
            }
            fmask = !c ? F_ALL : (F_ALL & ~F_AF & ~(c == 1 ? 0 : F_OF));
        } else if (x.skip == 3000) {                    /* bsf/bsr of zero */
            /* the source is the operand after the 0F BC/BD and ModR/M: check the result's ZF */
            if (out->flags & F_ZF) {
                fmask = F_ZF;
                /* destination undefined: copy the host's into the emulator's */
                int reg = (x.b[(x.b[0] == 0x66) + 2] >> 3) & 7;
                cpu.r[reg] = out->r[reg];
            }
        }

        int bad = 0;
        for (int r = 0; r < 8; r++)
            if (r != R_SP && cpu.r[r] != out->r[r])
                bad = 1;
        if ((cpu.flags ^ out->flags) & fmask)
            bad = 1;
        if (memcmp(mem + SCRATCH, (void *)SCRATCH, SCRATCH_SIZE))
            bad = 1;
        if (!bad)
            continue;
        printf("MISMATCH %s (flags compared %03x):", x.what, fmask);
fail:
        for (int i = 0; i < x.len; i++)
            printf(" %02x", x.b[i]);
        printf("\n");
        dump("in", in);
        dump("host", out);
        struct regs e;
        memcpy(e.r, cpu.r, sizeof(e.r));
        e.flags = cpu.flags;
        dump("emu", &e);
        if (memcmp(mem + SCRATCH, (void *)SCRATCH, SCRATCH_SIZE))
            printf("  memory differs\n");
        failures++;
    }

    /* Real mode: a word at offset FFFFh wraps to offset 0 of its segment. */
    static const uint8_t wrap_code[][4] = {
        { 0x8B, 0x06, 0xFF, 0xFF },     /* mov ax, [FFFF] */
        { 0xA1, 0xFF, 0xFF, 0x90 },     /* mov ax, moffs [FFFF] */
        { 0x89, 0x1E, 0xFF, 0xFF },     /* mov [FFFF], bx */
    };
    for (int t = 0; t < 3; t++) {
        cpu.pm = cpu.code32 = cpu.stack32 = 0;
        cpu.ip_mask = 0xFFFF;
        cpu.seg[S_CS] = 0x8000;
        cpu.seg[S_DS] = 0x1000;
        cpu.ip = 0;
        memcpy(mem + 0x80000, wrap_code[t], 4);
        mem[0x10000] = 0x12;
        mem[0x1FFFF] = 0x34;
        mem[0x20000] = 0x56;
        cpu.r[R_AX] = 0;
        cpu.r[R_BX] = 0xBBAA;
        cpu_run(1);
        int ok = t < 2 ? AX == 0x1234 : mem[0x1FFFF] == 0xAA && mem[0x10000] == 0xBB && mem[0x20000] == 0x56;
        if (!ok) {
            printf("MISMATCH real-mode wrap at FFFFh, test %d\n", t);
            failures++;
        }
    }
    printf("%ld instructions, %ld failures, %ld skipped\n", count, failures, skipped);
    return failures != 0;
}
