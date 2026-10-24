/*
 * The CPU: an x86 in real or protected mode, interpreted one instruction
 * at a time.
 *
 * It implements the 8086 instruction set, the 186/286 additions (PUSHA,
 * ENTER, immediate shifts and IMUL, BOUND, INS/OUTS) and what a 386
 * offers in real mode: 32-bit operands and addresses through the 66h
 * and 67h prefixes, FS and GS, MOVZX/MOVSX, SETcc, bit tests, SHLD/SHRD
 * and near conditional jumps, plus the 486's BSWAP, XADD and CMPXCHG.
 * Flag behaviour follows a 386, so CPU detection code finds one. There
 * is no FPU: escape opcodes are decoded and ignored, which is how a PC
 * without a coprocessor behaves, and how programs detect that.
 *
 * Faults (divide error, invalid opcode, BOUND) raise the interrupt with
 * the faulting instruction's address pushed, as on a 286 or later.
 * Opcode 0F FF followed by a 16-bit id calls into the emulator; the ROM
 * interrupt stubs use it.
 *
 * Protected mode is there for DPMI clients (see dpmi.c), which run at
 * ring 3 and never touch the descriptor tables themselves: segment
 * registers take their base and size from the DPMI host's descriptors,
 * a 32-bit code segment makes 32-bit operands and addresses the default,
 * a 32-bit stack segment uses ESP, and interrupts and faults go to the
 * host. Limits and privileges are not checked.
 */
#include <string.h>
#include "dos.h"

struct cpu cpu;

static uint8_t parity[256];

/* Decoding state of the current instruction. */
static int opsize32, addrsize32;
static int seg_ovr;                     /* segment override, or -1 */
static int rep_prefix;                  /* 0, 0xF2 (REPNE) or 0xF3 (REP/REPE) */

#define MASK(s) ((s) == 8 ? 0xFFu : (s) == 16 ? 0xFFFFu : 0xFFFFFFFFu)
#define SIGN(s) (1u << ((s) - 1))

#define segbase seg_base

static inline uint8_t fetch8(void)
{
    uint8_t b = rb(segbase(S_CS) + cpu.ip);
    cpu.ip = (cpu.ip + 1) & cpu.ip_mask;
    return b;
}

static inline uint16_t fetch16(void)
{
    uint16_t lo = fetch8();
    return (uint16_t)(lo | fetch8() << 8);
}

static inline uint32_t fetch32(void)
{
    uint32_t lo = fetch16();
    return lo | (uint32_t)fetch16() << 16;
}

static inline uint32_t fetch_v(void)
{
    return opsize32 ? fetch32() : fetch16();
}

static inline int vsize(void)
{
    return opsize32 ? 32 : 16;
}

/* Instruction pointers are cut to the operand size. */
static inline uint32_t vmask(void)
{
    return opsize32 ? 0xFFFFFFFFu : 0xFFFFu;
}

static inline uint32_t get_reg(int size, int r)
{
    if (size == 8)
        return r < 4 ? REG8L(r) : REG8H(r - 4);
    if (size == 16)
        return REG16(r);
    return cpu.r[r];
}

static inline void set_reg(int size, int r, uint32_t v)
{
    if (size == 8) {
        if (r < 4)
            REG8L(r) = (uint8_t)v;
        else
            REG8H(r - 4) = (uint8_t)v;
    } else if (size == 16) {
        REG16(r) = (uint16_t)v;
    } else {
        cpu.r[r] = v;
    }
}

static inline uint32_t mread(int size, uint32_t a)
{
    return size == 8 ? rb(a) : size == 16 ? rw(a) : rd(a);
}

static inline void mwrite(int size, uint32_t a, uint32_t v)
{
    if (size == 8)
        wb(a, (uint8_t)v);
    else if (size == 16)
        ww(a, (uint16_t)v);
    else
        wd(a, v);
}

static inline void set_flag(uint32_t f, int on)
{
    if (on)
        cpu.flags |= f;
    else
        cpu.flags &= ~f;
}

/* ---- ModR/M ---- */

struct modrm {
    int reg, rm;
    int is_reg;
    uint32_t off;                       /* effective offset */
    uint32_t addr;                      /* linear address */
};

static void decode(struct modrm *m)
{
    uint8_t b = fetch8();
    int mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    m->is_reg = mod == 3;
    if (m->is_reg)
        return;

    int seg = S_DS;
    uint32_t off;
    if (!addrsize32) {
        switch (m->rm) {
        case 0: off = (uint32_t)BX + SI; break;
        case 1: off = (uint32_t)BX + DI; break;
        case 2: off = (uint32_t)BP + SI; seg = S_SS; break;
        case 3: off = (uint32_t)BP + DI; seg = S_SS; break;
        case 4: off = SI; break;
        case 5: off = DI; break;
        case 6:
            if (mod == 0) {
                off = fetch16();
            } else {
                off = BP;
                seg = S_SS;
            }
            break;
        default: off = BX; break;
        }
        if (mod == 1)
            off += (uint32_t)(int8_t)fetch8();
        else if (mod == 2)
            off += fetch16();
        off &= 0xFFFF;
    } else {
        if (m->rm == 4) {
            uint8_t sib = fetch8();
            int scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
            if (base == 5 && mod == 0) {
                off = fetch32();
            } else {
                off = cpu.r[base];
                if (base == R_SP || base == R_BP)
                    seg = S_SS;
            }
            if (index != 4)
                off += cpu.r[index] << scale;
        } else if (m->rm == 5 && mod == 0) {
            off = fetch32();
        } else {
            off = cpu.r[m->rm];
            if (m->rm == R_BP)
                seg = S_SS;
        }
        if (mod == 1)
            off += (uint32_t)(int8_t)fetch8();
        else if (mod == 2)
            off += fetch32();
    }
    if (seg_ovr >= 0)
        seg = seg_ovr;
    m->off = off;
    m->addr = segbase(seg) + off;
}

static inline uint32_t rm_get(const struct modrm *m, int size)
{
    return m->is_reg ? get_reg(size, m->rm) : mread(size, m->addr);
}

static inline void rm_set(const struct modrm *m, int size, uint32_t v)
{
    if (m->is_reg)
        set_reg(size, m->rm, v);
    else
        mwrite(size, m->addr, v);
}

/* ---- stack ---- */

uint32_t cpu_sp(void)
{
    return cpu.stack32 ? cpu.r[R_SP] : SP;
}

void cpu_set_sp(uint32_t v)
{
    if (cpu.stack32)
        cpu.r[R_SP] = v;
    else
        SP = (uint16_t)v;
}

static inline void sp_add(int32_t d)
{
    if (cpu.stack32)
        cpu.r[R_SP] += (uint32_t)d;
    else
        SP = (uint16_t)(SP + d);
}

void cpu_push16(uint16_t v)
{
    sp_add(-2);
    ww(segbase(S_SS) + cpu_sp(), v);
}

uint16_t cpu_pop16(void)
{
    uint16_t v = rw(segbase(S_SS) + cpu_sp());
    sp_add(2);
    return v;
}

void cpu_push32(uint32_t v)
{
    sp_add(-4);
    wd(segbase(S_SS) + cpu_sp(), v);
}

uint32_t cpu_pop32(void)
{
    uint32_t v = rd(segbase(S_SS) + cpu_sp());
    sp_add(4);
    return v;
}

#define push32 cpu_push32
#define pop32 cpu_pop32

static void push_v(uint32_t v)
{
    if (opsize32)
        push32(v);
    else
        cpu_push16((uint16_t)v);
}

static uint32_t pop_v(void)
{
    return opsize32 ? pop32() : cpu_pop16();
}

/* ---- segments ---- */

void cpu_load_seg(int s, uint16_t sel)
{
    cpu.seg[s] = sel;
    if (!cpu.pm)
        return;
    uint32_t base, limit;
    uint8_t access;
    int big;
    if (!(sel & 0xFFFC)) {
        if (s == S_CS || s == S_SS) {
            dlog("null selector loaded into %s at %04x:%08x\n", s == S_CS ? "CS" : "SS",
                 cpu.seg[S_CS], cpu.ip_start);
            dpmi_exception(13);
        }
        cpu.base[s] = 0;
        return;
    }
    if (!dpmi_descriptor(sel, &base, &limit, &access, &big)) {
        dlog("bad selector %04x loaded at %04x:%08x\n", sel, cpu.seg[S_CS], cpu.ip_start);
        dpmi_exception(13);
        return;
    }
    cpu.base[s] = base;
    if (s == S_CS) {
        cpu.code32 = big;
        cpu.ip_mask = big ? 0xFFFFFFFFu : 0xFFFFu;
    } else if (s == S_SS) {
        cpu.stack32 = big;
    }
}

void cpu_real_mode(void)
{
    cpu.pm = 0;
    cpu.code32 = cpu.stack32 = 0;
    cpu.ip_mask = 0xFFFF;
    cpu.r[R_SP] &= 0xFFFF;
}

/* ---- interrupts and faults ---- */

void cpu_interrupt(int n)
{
    if (cpu.pm) {
        dpmi_interrupt(n, 0);
        return;
    }
    cpu_push16((uint16_t)cpu.flags);
    cpu_push16(CS);
    cpu_push16((uint16_t)cpu.ip);
    cpu.flags &= ~(F_IF | F_TF);
    cpu.ip = rw((uint32_t)n * 4);
    CS = rw((uint32_t)n * 4 + 2);
}

static void fault(int n)
{
    cpu.ip = cpu.ip_start;
    if (cpu.pm)
        dpmi_exception(n);
    else
        cpu_interrupt(n);
}

static void invalid_opcode(void)
{
    uint32_t at = segbase(S_CS) + cpu.ip_start;
    dlog("invalid opcode %02x %02x %02x at %04x:%04x\n", rb(at), rb(at + 1), rb(at + 2),
         CS, cpu.ip_start);
    fault(6);
}

static void set_flags16(uint32_t v)
{
    /* Bits 12-14 (IOPL, NT) can be changed in real mode on a 386; bit 15
     * stays clear, and bit 1 is always set. */
    cpu.flags = (v & 0x7FD5) | 0x0002;
}

/* ---- arithmetic ---- */

static void szp(uint32_t r, int size)
{
    r &= MASK(size);
    cpu.flags &= ~(F_SF | F_ZF | F_PF);
    if (!r)
        cpu.flags |= F_ZF;
    if (r & SIGN(size))
        cpu.flags |= F_SF;
    if (parity[r & 0xFF])
        cpu.flags |= F_PF;
}

enum { OP_ADD, OP_OR, OP_ADC, OP_SBB, OP_AND, OP_SUB, OP_XOR, OP_CMP };

/* ALU operation op on a and b, setting the flags from the operands and
 * the result. */
static uint32_t arith(int op, uint32_t a, uint32_t b, int size)
{
    uint32_t mask = MASK(size), sign = SIGN(size), r, c = cpu.flags & F_CF;
    a &= mask;
    b &= mask;
    cpu.flags &= ~(F_CF | F_OF | F_AF);
    switch (op) {
    case OP_ADD:
    case OP_ADC:
        if (op == OP_ADD)
            c = 0;
        r = (a + b + c) & mask;
        if (c ? r <= a : r < a)
            cpu.flags |= F_CF;
        if ((a ^ r) & (b ^ r) & sign)
            cpu.flags |= F_OF;
        break;
    case OP_SUB:
    case OP_SBB:
    case OP_CMP:
        if (op != OP_SBB)
            c = 0;
        r = (a - b - c) & mask;
        if (c ? a <= b : a < b)
            cpu.flags |= F_CF;
        if ((a ^ b) & (a ^ r) & sign)
            cpu.flags |= F_OF;
        break;
    default:
        r = op == OP_OR ? a | b : op == OP_AND ? a & b : a ^ b;
        szp(r, size);
        return r;
    }
    if ((a ^ b ^ r) & 0x10)
        cpu.flags |= F_AF;
    szp(r, size);
    return r;
}

static uint32_t inc_dec(uint32_t a, int size, int dec)
{
    uint32_t cf = cpu.flags & F_CF;
    uint32_t r = arith(dec ? OP_SUB : OP_ADD, a, 1, size);
    cpu.flags = (cpu.flags & ~F_CF) | cf;
    return r;
}

static uint32_t sign_extend(uint32_t v, int size)
{
    return size == 8 ? (uint32_t)(int8_t)v : size == 16 ? (uint32_t)(int16_t)v : v;
}

/* Group 2: rotates and shifts. A count of 0 changes nothing. */
static uint32_t shift(int op, uint32_t a, uint32_t count, int size)
{
    uint32_t mask = MASK(size), sign = SIGN(size), r = a & mask, cf, n;
    a &= mask;
    count &= 0x1F;
    if (!count)
        return a;
    switch (op) {
    case 0:                             /* ROL */
        n = count % (uint32_t)size;
        if (n)
            r = ((a << n) | (a >> (size - (int)n))) & mask;
        set_flag(F_CF, r & 1);
        set_flag(F_OF, ((r >> (size - 1)) ^ r) & 1);
        return r;
    case 1:                             /* ROR */
        n = count % (uint32_t)size;
        if (n)
            r = ((a >> n) | (a << (size - (int)n))) & mask;
        set_flag(F_CF, r & sign);
        set_flag(F_OF, ((r >> (size - 1)) ^ (r >> (size - 2))) & 1);
        return r;
    case 2:                             /* RCL */
        n = count % (uint32_t)(size + 1);
        cf = (cpu.flags & F_CF) != 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t out = (r & sign) != 0;
            r = ((r << 1) | cf) & mask;
            cf = out;
        }
        set_flag(F_CF, cf);
        set_flag(F_OF, ((r & sign) != 0) ^ cf);
        return r;
    case 3:                             /* RCR */
        n = count % (uint32_t)(size + 1);
        cf = (cpu.flags & F_CF) != 0;
        set_flag(F_OF, ((r & sign) != 0) ^ cf);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t out = r & 1;
            r = (r >> 1) | (cf ? sign : 0);
            cf = out;
        }
        set_flag(F_CF, cf);
        return r;
    case 4:                             /* SHL */
    case 6:                             /* SAL */
        if (count < (uint32_t)size) {
            cf = (a >> (size - (int)count)) & 1;
            r = (a << count) & mask;
        } else {
            cf = count == (uint32_t)size ? a & 1 : 0;
            r = 0;
        }
        set_flag(F_CF, cf);
        set_flag(F_OF, ((r & sign) != 0) ^ cf);
        break;
    case 5:                             /* SHR */
        if (count < (uint32_t)size) {
            cf = (a >> (count - 1)) & 1;
            r = a >> count;
        } else {
            cf = count == (uint32_t)size ? (a >> (size - 1)) & 1 : 0;
            r = 0;
        }
        set_flag(F_CF, cf);
        set_flag(F_OF, a & sign);
        break;
    default: {                          /* SAR */
        int32_t sa = (int32_t)sign_extend(a, size);
        if (count >= (uint32_t)size) {
            cf = (a & sign) != 0;
            r = cf ? mask : 0;
        } else {
            cf = (uint32_t)(sa >> (count - 1)) & 1;
            r = (uint32_t)(sa >> count) & mask;
        }
        set_flag(F_CF, cf);
        cpu.flags &= ~F_OF;
        break;
    }
    }
    szp(r, size);
    return r;
}

/* SHLD/SHRD: shift a, filling with bits from b. */
static uint32_t shift_double(uint32_t a, uint32_t b, uint32_t count, int size, int right)
{
    uint32_t r, cf, sign = SIGN(size);
    count &= 0x1F;
    if (!count)
        return a;
    if (size == 16) {
        if (count > 16)
            return a;                   /* undefined on a 386: leave it */
        uint32_t t = right ? (b << 16) | (a & 0xFFFF) : ((a & 0xFFFF) << 16) | (b & 0xFFFF);
        if (right) {
            cf = (t >> (count - 1)) & 1;
            r = (t >> count) & 0xFFFF;
        } else {
            cf = (t >> (32 - count)) & 1;
            r = (t << count) >> 16;
        }
    } else if (right) {
        cf = (a >> (count - 1)) & 1;
        r = (a >> count) | (b << (32 - count));
    } else {
        cf = (a >> (32 - count)) & 1;
        r = (a << count) | (b >> (32 - count));
    }
    set_flag(F_CF, cf);
    set_flag(F_OF, (r ^ a) & sign);
    szp(r, size);
    return r;
}

static int div32(uint32_t hi, uint32_t lo, uint32_t d, uint32_t *q, uint32_t *r)
{
    if (d == 0 || hi >= d)
        return -1;                      /* the quotient would not fit */
    __asm__("divl %4" : "=a"(*q), "=d"(*r) : "a"(lo), "d"(hi), "rm"(d));
    return 0;
}

/* Group 3 multiply and divide; returns -1 for a divide error. */
static int muldiv(int op, uint32_t v, int size)
{
    switch (op) {
    case 4:                             /* MUL */
        if (size == 8) {
            AX = (uint16_t)(AL * v);
            set_flag(F_CF | F_OF, AH != 0);
        } else if (size == 16) {
            uint32_t r = (uint32_t)AX * v;
            AX = (uint16_t)r;
            DX = (uint16_t)(r >> 16);
            set_flag(F_CF | F_OF, DX != 0);
        } else {
            uint64_t r = (uint64_t)cpu.r[R_AX] * v;
            cpu.r[R_AX] = (uint32_t)r;
            cpu.r[R_DX] = (uint32_t)(r >> 32);
            set_flag(F_CF | F_OF, cpu.r[R_DX] != 0);
        }
        return 0;
    case 5:                             /* IMUL */
        if (size == 8) {
            int r = (int8_t)AL * (int8_t)v;
            AX = (uint16_t)r;
            set_flag(F_CF | F_OF, r != (int8_t)r);
        } else if (size == 16) {
            int32_t r = (int16_t)AX * (int16_t)v;
            AX = (uint16_t)r;
            DX = (uint16_t)((uint32_t)r >> 16);
            set_flag(F_CF | F_OF, r != (int16_t)r);
        } else {
            int64_t r = (int64_t)(int32_t)cpu.r[R_AX] * (int32_t)v;
            cpu.r[R_AX] = (uint32_t)r;
            cpu.r[R_DX] = (uint32_t)((uint64_t)r >> 32);
            set_flag(F_CF | F_OF, r != (int32_t)r);
        }
        return 0;
    case 6:                             /* DIV */
        if (size == 8) {
            if (!v || AX / v > 0xFF)
                return -1;
            uint16_t n = AX;
            AL = (uint8_t)(n / v);
            AH = (uint8_t)(n % v);
        } else if (size == 16) {
            uint32_t n = (uint32_t)DX << 16 | AX;
            if (!v || n / v > 0xFFFF)
                return -1;
            AX = (uint16_t)(n / v);
            DX = (uint16_t)(n % v);
        } else {
            uint32_t q, r;
            if (div32(cpu.r[R_DX], cpu.r[R_AX], v, &q, &r) < 0)
                return -1;
            cpu.r[R_AX] = q;
            cpu.r[R_DX] = r;
        }
        return 0;
    default:                            /* IDIV */
        if (size == 8) {
            int n = (int16_t)AX, d = (int8_t)v;
            if (!d)
                return -1;
            int q = n / d;
            if (q > 127 || q < -128)
                return -1;
            AL = (uint8_t)q;
            AH = (uint8_t)(n % d);
        } else if (size == 16) {
            int32_t n = (int32_t)((uint32_t)DX << 16 | AX), d = (int16_t)v;
            if (!d || (n == (int32_t)0x80000000 && d == -1))
                return -1;
            int32_t q = n / d;
            if (q > 32767 || q < -32768)
                return -1;
            AX = (uint16_t)q;
            DX = (uint16_t)(n % d);
        } else {
            int neg_n = (int32_t)cpu.r[R_DX] < 0, neg_d = (int32_t)v < 0;
            uint64_t n = (uint64_t)cpu.r[R_DX] << 32 | cpu.r[R_AX];
            if (neg_n)
                n = 0 - n;
            uint32_t ad = neg_d ? 0 - v : v, q, r;
            if (div32((uint32_t)(n >> 32), (uint32_t)n, ad, &q, &r) < 0)
                return -1;
            if (neg_n != neg_d) {
                if (q > 0x80000000u)
                    return -1;
                q = 0 - q;
            } else if (q > 0x7FFFFFFFu) {
                return -1;
            }
            cpu.r[R_AX] = q;
            cpu.r[R_DX] = neg_n ? 0 - r : r;
        }
        return 0;
    }
}

static int cond(int c)
{
    uint32_t f = cpu.flags;
    int r;
    switch (c >> 1) {
    case 0: r = (f & F_OF) != 0; break;
    case 1: r = (f & F_CF) != 0; break;
    case 2: r = (f & F_ZF) != 0; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;
    case 4: r = (f & F_SF) != 0; break;
    case 5: r = (f & F_PF) != 0; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = (f & F_ZF) || ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    }
    return r ^ (c & 1);
}

/* ---- string instructions ---- */

static inline uint32_t str_count(void)
{
    return addrsize32 ? cpu.r[R_CX] : CX;
}

static inline void str_set_count(uint32_t v)
{
    if (addrsize32)
        cpu.r[R_CX] = v;
    else
        CX = (uint16_t)v;
}

static inline uint32_t str_src(void)
{
    return segbase(seg_ovr >= 0 ? seg_ovr : S_DS) + (addrsize32 ? cpu.r[R_SI] : SI);
}

static inline uint32_t str_dst(void)
{
    return segbase(S_ES) + (addrsize32 ? cpu.r[R_DI] : DI);
}

static inline void str_step(int reg, int bytes)
{
    int d = (cpu.flags & F_DF) ? -bytes : bytes;
    if (addrsize32)
        cpu.r[reg] += (uint32_t)d;
    else
        REG16(reg) = (uint16_t)(REG16(reg) + d);
}

/* The string instructions; the last two compare, so REPE/REPNE apply. */
enum { S_MOVS, S_STOS, S_LODS, S_INS, S_OUTS, S_CMPS, S_SCAS };

static void string_op(int kind, int size)
{
    int bytes = size / 8;
    int repeat = rep_prefix != 0;
    if (repeat && !str_count())
        return;
    for (;;) {
        switch (kind) {
        case S_MOVS:
            mwrite(size, str_dst(), mread(size, str_src()));
            str_step(R_SI, bytes);
            str_step(R_DI, bytes);
            break;
        case S_STOS:
            mwrite(size, str_dst(), get_reg(size, R_AX));
            str_step(R_DI, bytes);
            break;
        case S_LODS:
            set_reg(size, R_AX, mread(size, str_src()));
            str_step(R_SI, bytes);
            break;
        case S_INS:
            mwrite(size, str_dst(), io_in(DX, size));
            str_step(R_DI, bytes);
            break;
        case S_OUTS:
            io_out(DX, mread(size, str_src()), size);
            str_step(R_SI, bytes);
            break;
        case S_CMPS:
            arith(OP_CMP, mread(size, str_src()), mread(size, str_dst()), size);
            str_step(R_SI, bytes);
            str_step(R_DI, bytes);
            break;
        default:                        /* SCAS */
            arith(OP_CMP, get_reg(size, R_AX), mread(size, str_dst()), size);
            str_step(R_DI, bytes);
            break;
        }
        if (!repeat)
            return;
        uint32_t n = str_count() - 1;
        str_set_count(n);
        if (!n)
            return;
        if (kind >= S_CMPS && ((rep_prefix == 0xF3) != ((cpu.flags & F_ZF) != 0)))
            return;                     /* REPE stops on a difference, REPNE on a match */
    }
}

/* ---- BCD ---- */

static void daa_das(int sub)
{
    uint8_t old = AL;
    int cf = (cpu.flags & F_CF) != 0, af = (cpu.flags & F_AF) != 0;
    cpu.flags &= ~(F_CF | F_AF);
    if ((old & 0x0F) > 9 || af) {
        AL = (uint8_t)(sub ? AL - 6 : AL + 6);
        cpu.flags |= F_AF;
    }
    if (old > 0x99 || cf) {
        AL = (uint8_t)(sub ? AL - 0x60 : AL + 0x60);
        cpu.flags |= F_CF;
    }
    szp(AL, 8);
}

static void aaa_aas(int sub)
{
    if ((AL & 0x0F) > 9 || (cpu.flags & F_AF)) {
        if (sub) {
            AL = (uint8_t)(AL - 6);
            AH = (uint8_t)(AH - 1);
        } else {
            AX = (uint16_t)(AX + 0x106);
        }
        cpu.flags |= F_AF | F_CF;
    } else {
        cpu.flags &= ~(F_AF | F_CF);
    }
    AL &= 0x0F;
}

/* ---- bit tests ---- */

/* BT/BTS/BTR/BTC: op 0-3. A register bit offset may reach past a
 * memory operand. */
static void bit_test(int op, const struct modrm *m, uint32_t bit, int from_reg)
{
    int size = vsize();
    uint32_t v, addr = m->addr;
    if (!m->is_reg && from_reg) {
        int32_t sb = (int32_t)sign_extend(bit, size);
        addr += (uint32_t)((sb >> (size == 16 ? 4 : 5)) * (size / 8));
    }
    bit &= (uint32_t)size - 1;
    v = m->is_reg ? get_reg(size, m->rm) : mread(size, addr);
    set_flag(F_CF, (v >> bit) & 1);
    if (op == 0)
        return;
    v = op == 1 ? v | (1u << bit) : op == 2 ? v & ~(1u << bit) : v ^ (1u << bit);
    if (m->is_reg)
        set_reg(size, m->rm, v);
    else
        mwrite(size, addr, v);
}

/* ---- control transfer helpers ---- */

static void jump_rel(int32_t d)
{
    cpu.ip = (cpu.ip + (uint32_t)d) & vmask();
}

static void far_jump(uint16_t seg, uint32_t off)
{
    cpu_load_seg(S_CS, seg);
    cpu.ip = off & vmask();
}

static void load_far_pointer(int seg)
{
    struct modrm m;
    decode(&m);
    if (m.is_reg) {
        invalid_opcode();
        return;
    }
    int size = vsize();
    uint32_t off = mread(size, m.addr);
    cpu_load_seg(seg, rw(m.addr + (uint32_t)size / 8));
    set_reg(size, m.reg, off);
    if (seg == S_SS)
        cpu.inhibit = 1;
}

/* ---- the 0F opcodes ---- */

static void step_0f(void)
{
    uint8_t op = fetch8();
    struct modrm m;
    int size = vsize();

    if (op >= 0x80 && op <= 0x8F) {     /* Jcc near */
        int32_t d = opsize32 ? (int32_t)fetch32() : (int16_t)fetch16();
        if (cond(op & 0x0F))
            jump_rel(d);
        return;
    }
    if (op >= 0x90 && op <= 0x9F) {     /* SETcc */
        decode(&m);
        rm_set(&m, 8, (uint32_t)cond(op & 0x0F));
        return;
    }
    if (op >= 0xC8 && op <= 0xCF) {     /* BSWAP */
        uint32_t v = cpu.r[op & 7];
        cpu.r[op & 7] = v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24;
        return;
    }

    switch (op) {
    case 0x00:                          /* SLDT, STR, LLDT, LTR, VERR, VERW */
        if (!cpu.pm) {
            invalid_opcode();
            return;
        }
        decode(&m);
        if (m.reg == 4 || m.reg == 5) {
            uint32_t base, limit;
            uint8_t access;
            int big;
            set_flag(F_ZF, dpmi_descriptor((uint16_t)rm_get(&m, 16), &base, &limit, &access, &big));
        } else if (m.reg < 2) {
            rm_set(&m, 16, 0);
        }
        return;
    case 0x02:                          /* LAR */
    case 0x03: {                        /* LSL */
        uint32_t base, limit;
        uint8_t access;
        int big;
        if (!cpu.pm) {
            invalid_opcode();
            return;
        }
        decode(&m);
        if (!dpmi_descriptor((uint16_t)rm_get(&m, 16), &base, &limit, &access, &big)) {
            cpu.flags &= ~F_ZF;
            return;
        }
        cpu.flags |= F_ZF;
        if (op == 0x02)
            set_reg(size, m.reg, (uint32_t)access << 8 | (big ? 0x400000u : 0) |
                                 (limit > 0xFFFFF ? 0x800000u : 0));
        else
            set_reg(size, m.reg, limit);
        return;
    }
    case 0x01:
        decode(&m);
        switch (m.reg) {
        case 0:                         /* SGDT, SIDT: nothing loaded */
        case 1:
            if (m.is_reg) {
                invalid_opcode();
                return;
            }
            ww(m.addr, 0x03FF * (m.reg == 1));
            wd(m.addr + 2, 0);
            return;
        case 2:                         /* LGDT, LIDT: accepted, unused */
        case 3:
            return;
        case 4:                         /* SMSW: no FPU */
            rm_set(&m, 16, 0x0010 | (uint32_t)cpu.pm);
            return;
        case 6:                         /* LMSW */
            if ((rm_get(&m, 16) & 1) && !cpu.pm)
                fatal("the program switched to protected mode itself, which is only "
                      "supported through DPMI");
            return;
        default:
            invalid_opcode();
            return;
        }
    case 0x06:                          /* CLTS */
    case 0x08:                          /* INVD */
    case 0x09:                          /* WBINVD */
        return;
    case 0x20:                          /* MOV r32, CRn */
    case 0x21:                          /* MOV r32, DRn */
        decode(&m);
        cpu.r[m.rm] = op == 0x20 && m.reg == 0 ? 0x00000010u | (uint32_t)cpu.pm : 0;
        return;
    case 0x22:                          /* MOV CRn, r32 */
    case 0x23:
        decode(&m);
        if (op == 0x22 && m.reg == 0 && (cpu.r[m.rm] & 1) && !cpu.pm)
            fatal("the program switched to protected mode itself, which is only "
                  "supported through DPMI");
        return;
    case 0xA0: push_v(cpu.seg[S_FS]); return;
    case 0xA1: cpu_load_seg(S_FS, (uint16_t)pop_v()); return;
    case 0xA8: push_v(cpu.seg[S_GS]); return;
    case 0xA9: cpu_load_seg(S_GS, (uint16_t)pop_v()); return;
    case 0xA3:                          /* BT */
    case 0xAB:                          /* BTS */
    case 0xB3:                          /* BTR */
    case 0xBB:                          /* BTC */
        decode(&m);
        bit_test(op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3, &m,
                 get_reg(size, m.reg), 1);
        return;
    case 0xBA:
        decode(&m);
        if (m.reg < 4) {
            invalid_opcode();
            return;
        }
        bit_test(m.reg - 4, &m, fetch8(), 0);
        return;
    case 0xA4:                          /* SHLD Ib */
    case 0xA5:                          /* SHLD CL */
    case 0xAC:                          /* SHRD Ib */
    case 0xAD: {                        /* SHRD CL */
        decode(&m);
        uint32_t count = (op & 1) ? CL : fetch8();
        rm_set(&m, size, shift_double(rm_get(&m, size), get_reg(size, m.reg), count, size,
                                      op >= 0xAC));
        return;
    }
    case 0xAF: {                        /* IMUL Gv, Ev */
        decode(&m);
        int64_t r = (int64_t)(int32_t)sign_extend(get_reg(size, m.reg), size) *
                    (int32_t)sign_extend(rm_get(&m, size), size);
        set_reg(size, m.reg, (uint32_t)r);
        set_flag(F_CF | F_OF, r != (int64_t)(int32_t)sign_extend((uint32_t)r, size));
        return;
    }
    case 0xB0:                          /* CMPXCHG */
    case 0xB1: {
        int sz = op == 0xB0 ? 8 : size;
        decode(&m);
        uint32_t dst = rm_get(&m, sz);
        arith(OP_CMP, get_reg(sz, R_AX), dst, sz);
        if (cpu.flags & F_ZF)
            rm_set(&m, sz, get_reg(sz, m.reg));
        else
            set_reg(sz, R_AX, dst);
        return;
    }
    case 0xB2: load_far_pointer(S_SS); return;
    case 0xB4: load_far_pointer(S_FS); return;
    case 0xB5: load_far_pointer(S_GS); return;
    case 0xB6:                          /* MOVZX */
    case 0xB7:
    case 0xBE:                          /* MOVSX */
    case 0xBF: {
        int from = (op & 1) ? 16 : 8;
        decode(&m);
        uint32_t v = rm_get(&m, from);
        set_reg(size, m.reg, op >= 0xBE ? sign_extend(v, from) : v);
        return;
    }
    case 0xBC:                          /* BSF */
    case 0xBD: {                        /* BSR */
        decode(&m);
        uint32_t v = rm_get(&m, size);
        if (!v) {
            cpu.flags |= F_ZF;
            return;
        }
        cpu.flags &= ~F_ZF;
        int i;
        if (op == 0xBC)
            for (i = 0; !(v & (1u << i)); i++)
                ;
        else
            for (i = size - 1; !(v & (1u << i)); i--)
                ;
        set_reg(size, m.reg, (uint32_t)i);
        return;
    }
    case 0xC0:                          /* XADD */
    case 0xC1: {
        int sz = op == 0xC0 ? 8 : size;
        decode(&m);
        uint32_t a = rm_get(&m, sz), b = get_reg(sz, m.reg);
        uint32_t r = arith(OP_ADD, a, b, sz);
        set_reg(sz, m.reg, a);
        rm_set(&m, sz, r);
        return;
    }
    case 0xFF: {                        /* emulator callback */
        int id = fetch16();
        if (id >= DPMI_CB_PM || (id >= CB_DPMI && id < CB_DPMI_END))
            dpmi_callback(id);
        else
            bios_callback(id);
        return;
    }
    default:
        invalid_opcode();
        return;
    }
}

/* ---- one instruction ---- */

static void step(void)
{
    struct modrm m;
    uint32_t v;
    int size;

    cpu.ip_start = cpu.ip;
    opsize32 = addrsize32 = cpu.code32;
    seg_ovr = -1;
    rep_prefix = 0;

    uint8_t op;
    for (int prefixes = 0;; prefixes++) {
        op = fetch8();
        if (prefixes > 14)
            break;
        switch (op) {
        case 0x26: seg_ovr = S_ES; continue;
        case 0x2E: seg_ovr = S_CS; continue;
        case 0x36: seg_ovr = S_SS; continue;
        case 0x3E: seg_ovr = S_DS; continue;
        case 0x64: seg_ovr = S_FS; continue;
        case 0x65: seg_ovr = S_GS; continue;
        case 0x66: opsize32 = !cpu.code32; continue;
        case 0x67: addrsize32 = !cpu.code32; continue;
        case 0xF0: continue;            /* LOCK */
        case 0xF2:
        case 0xF3: rep_prefix = op; continue;
        }
        break;
    }
    cpu.steps++;

    /* The eight ALU operations, in six forms each. */
    if (op < 0x40 && (op & 7) < 6) {
        int alu_op = op >> 3;
        switch (op & 7) {
        case 0:                         /* Eb, Gb */
        case 1:                         /* Ev, Gv */
            size = (op & 1) ? vsize() : 8;
            decode(&m);
            v = arith(alu_op, rm_get(&m, size), get_reg(size, m.reg), size);
            if (alu_op != OP_CMP)
                rm_set(&m, size, v);
            return;
        case 2:                         /* Gb, Eb */
        case 3:                         /* Gv, Ev */
            size = (op & 1) ? vsize() : 8;
            decode(&m);
            v = arith(alu_op, get_reg(size, m.reg), rm_get(&m, size), size);
            if (alu_op != OP_CMP)
                set_reg(size, m.reg, v);
            return;
        case 4:                         /* AL, Ib */
            v = arith(alu_op, AL, fetch8(), 8);
            if (alu_op != OP_CMP)
                AL = (uint8_t)v;
            return;
        default:                        /* eAX, Iv */
            size = vsize();
            v = arith(alu_op, get_reg(size, R_AX), fetch_v(), size);
            if (alu_op != OP_CMP)
                set_reg(size, R_AX, v);
            return;
        }
    }

    switch (op) {
    case 0x06: push_v(ES); return;
    case 0x07: cpu_load_seg(S_ES, (uint16_t)pop_v()); return;
    case 0x0E: push_v(CS); return;
    case 0x0F: step_0f(); return;
    case 0x16: push_v(SS); return;
    case 0x17: cpu_load_seg(S_SS, (uint16_t)pop_v()); cpu.inhibit = 1; return;
    case 0x1E: push_v(DS); return;
    case 0x1F: cpu_load_seg(S_DS, (uint16_t)pop_v()); return;
    case 0x27: daa_das(0); return;
    case 0x2F: daa_das(1); return;
    case 0x37: aaa_aas(0); return;
    case 0x3F: aaa_aas(1); return;

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        size = vsize();
        set_reg(size, op & 7, inc_dec(get_reg(size, op & 7), size, 0));
        return;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        size = vsize();
        set_reg(size, op & 7, inc_dec(get_reg(size, op & 7), size, 1));
        return;
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        push_v(get_reg(vsize(), op & 7));       /* PUSH SP pushes the old value, as on a 286 */
        return;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        v = pop_v();
        set_reg(vsize(), op & 7, v);
        return;

    case 0x60: {                        /* PUSHA */
        size = vsize();
        uint32_t sp = get_reg(size, R_SP);
        for (int r = R_AX; r <= R_DI; r++)
            push_v(r == R_SP ? sp : get_reg(size, r));
        return;
    }
    case 0x61:                          /* POPA */
        size = vsize();
        for (int r = R_DI; r >= R_AX; r--) {
            v = pop_v();
            if (r != R_SP)
                set_reg(size, r, v);
        }
        return;
    case 0x62: {                        /* BOUND */
        size = vsize();
        decode(&m);
        if (m.is_reg) {
            invalid_opcode();
            return;
        }
        int32_t idx = (int32_t)sign_extend(get_reg(size, m.reg), size);
        int32_t lo = (int32_t)sign_extend(mread(size, m.addr), size);
        int32_t hi = (int32_t)sign_extend(mread(size, m.addr + (uint32_t)size / 8), size);
        if (idx < lo || idx > hi)
            fault(5);
        return;
    }
    case 0x63:                          /* ARPL (protected mode only) */
        if (!cpu.pm) {
            invalid_opcode();
            return;
        }
        decode(&m);
        v = rm_get(&m, 16);
        if ((v & 3) < (REG16(m.reg) & 3u)) {
            rm_set(&m, 16, (v & ~3u) | (REG16(m.reg) & 3u));
            cpu.flags |= F_ZF;
        } else {
            cpu.flags &= ~F_ZF;
        }
        return;
    case 0x68: push_v(fetch_v()); return;
    case 0x6A: push_v(sign_extend(fetch8(), 8)); return;
    case 0x69:                          /* IMUL Gv, Ev, Iv */
    case 0x6B: {                        /* IMUL Gv, Ev, Ib */
        size = vsize();
        decode(&m);
        int32_t a = (int32_t)sign_extend(rm_get(&m, size), size);
        int32_t b = op == 0x69 ? (int32_t)sign_extend(fetch_v(), size) : (int8_t)fetch8();
        int64_t r = (int64_t)a * b;
        set_reg(size, m.reg, (uint32_t)r);
        set_flag(F_CF | F_OF, r != (int64_t)(int32_t)sign_extend((uint32_t)r, size));
        return;
    }
    case 0x6C: string_op(S_INS, 8); return;
    case 0x6D: string_op(S_INS, vsize()); return;
    case 0x6E: string_op(S_OUTS, 8); return;
    case 0x6F: string_op(S_OUTS, vsize()); return;

    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = (int8_t)fetch8();
        if (cond(op & 0x0F))
            jump_rel(d);
        return;
    }

    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83: {                        /* group 1: ALU op Ev, imm */
        size = op == 0x81 || op == 0x83 ? vsize() : 8;
        decode(&m);
        uint32_t imm = op == 0x81 ? fetch_v() : op == 0x83 ? sign_extend(fetch8(), 8) : fetch8();
        v = arith(m.reg, rm_get(&m, size), imm, size);
        if (m.reg != OP_CMP)
            rm_set(&m, size, v);
        return;
    }
    case 0x84:                          /* TEST */
    case 0x85:
        size = op == 0x85 ? vsize() : 8;
        decode(&m);
        arith(OP_AND, rm_get(&m, size), get_reg(size, m.reg), size);
        return;
    case 0x86:                          /* XCHG */
    case 0x87:
        size = op == 0x87 ? vsize() : 8;
        decode(&m);
        v = rm_get(&m, size);
        rm_set(&m, size, get_reg(size, m.reg));
        set_reg(size, m.reg, v);
        return;
    case 0x88:                          /* MOV Eb, Gb */
    case 0x89:
        size = op == 0x89 ? vsize() : 8;
        decode(&m);
        rm_set(&m, size, get_reg(size, m.reg));
        return;
    case 0x8A:                          /* MOV Gb, Eb */
    case 0x8B:
        size = op == 0x8B ? vsize() : 8;
        decode(&m);
        set_reg(size, m.reg, rm_get(&m, size));
        return;
    case 0x8C:                          /* MOV Ew, Sw */
        decode(&m);
        if (m.reg > S_GS) {
            invalid_opcode();
            return;
        }
        rm_set(&m, m.is_reg ? vsize() : 16, cpu.seg[m.reg]);
        return;
    case 0x8D:                          /* LEA */
        decode(&m);
        if (m.is_reg) {
            invalid_opcode();
            return;
        }
        set_reg(vsize(), m.reg, m.off);
        return;
    case 0x8E:                          /* MOV Sw, Ew */
        decode(&m);
        if (m.reg > S_GS || m.reg == S_CS) {
            invalid_opcode();
            return;
        }
        cpu_load_seg(m.reg, (uint16_t)rm_get(&m, 16));
        if (m.reg == S_SS)
            cpu.inhibit = 1;
        return;
    case 0x8F:                          /* POP Ev */
        decode(&m);
        rm_set(&m, vsize(), pop_v());
        return;

    case 0x90:
        return;                         /* NOP (also PAUSE) */
    case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
        size = vsize();
        v = get_reg(size, op & 7);
        set_reg(size, op & 7, get_reg(size, R_AX));
        set_reg(size, R_AX, v);
        return;
    case 0x98:                          /* CBW, CWDE */
        if (opsize32)
            cpu.r[R_AX] = (uint32_t)(int16_t)AX;
        else
            AX = (uint16_t)(int8_t)AL;
        return;
    case 0x99:                          /* CWD, CDQ */
        if (opsize32)
            cpu.r[R_DX] = (int32_t)cpu.r[R_AX] < 0 ? 0xFFFFFFFF : 0;
        else
            DX = (int16_t)AX < 0 ? 0xFFFF : 0;
        return;
    case 0x9A: {                        /* CALL far */
        uint32_t off = fetch_v();
        uint16_t seg = fetch16();
        push_v(CS);
        push_v(cpu.ip);
        far_jump(seg, off);
        return;
    }
    case 0x9B:                          /* WAIT */
        return;
    case 0x9C:                          /* PUSHF */
        push_v(cpu.flags & (opsize32 ? 0x00007FD7 : 0x7FD7));
        return;
    case 0x9D:                          /* POPF */
        set_flags16(pop_v());
        return;
    case 0x9E:                          /* SAHF */
        cpu.flags = (cpu.flags & ~0xD5u) | (AH & 0xD5u);
        return;
    case 0x9F:                          /* LAHF */
        AH = (uint8_t)((cpu.flags & 0xD5) | 0x02);
        return;

    case 0xA0:                          /* MOV AL, moffs */
    case 0xA1:
    case 0xA2:
    case 0xA3: {
        uint32_t off = addrsize32 ? fetch32() : fetch16();
        uint32_t a = segbase(seg_ovr >= 0 ? seg_ovr : S_DS) + off;
        size = (op & 1) ? vsize() : 8;
        if (op < 0xA2)
            set_reg(size, R_AX, mread(size, a));
        else
            mwrite(size, a, get_reg(size, R_AX));
        return;
    }
    case 0xA4: string_op(S_MOVS, 8); return;
    case 0xA5: string_op(S_MOVS, vsize()); return;
    case 0xA6: string_op(S_CMPS, 8); return;
    case 0xA7: string_op(S_CMPS, vsize()); return;
    case 0xA8: arith(OP_AND, AL, fetch8(), 8); return;
    case 0xA9: size = vsize(); arith(OP_AND, get_reg(size, R_AX), fetch_v(), size); return;
    case 0xAA: string_op(S_STOS, 8); return;
    case 0xAB: string_op(S_STOS, vsize()); return;
    case 0xAC: string_op(S_LODS, 8); return;
    case 0xAD: string_op(S_LODS, vsize()); return;
    case 0xAE: string_op(S_SCAS, 8); return;
    case 0xAF: string_op(S_SCAS, vsize()); return;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        set_reg(8, op & 7, fetch8());
        return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        set_reg(vsize(), op & 7, fetch_v());
        return;

    case 0xC0:                          /* group 2 Eb, Ib */
    case 0xC1:
    case 0xD0:                          /* group 2 Eb, 1 */
    case 0xD1:
    case 0xD2:                          /* group 2 Eb, CL */
    case 0xD3: {
        size = (op & 1) ? vsize() : 8;
        decode(&m);
        uint32_t count = op <= 0xC1 ? fetch8() : op <= 0xD1 ? 1 : CL;
        rm_set(&m, size, shift(m.reg, rm_get(&m, size), count, size));
        return;
    }
    case 0xC2: {                        /* RET imm16 */
        uint16_t n = fetch16();
        cpu.ip = pop_v() & vmask();
        sp_add(n);
        return;
    }
    case 0xC3:
        cpu.ip = pop_v() & vmask();
        return;
    case 0xC4: load_far_pointer(S_ES); return;
    case 0xC5: load_far_pointer(S_DS); return;
    case 0xC6:                          /* MOV Ev, imm */
    case 0xC7:
        size = op == 0xC7 ? vsize() : 8;
        decode(&m);
        rm_set(&m, size, size == 8 ? fetch8() : fetch_v());
        return;
    case 0xC8: {                        /* ENTER */
        uint16_t alloc = fetch16();
        int level = fetch8() & 31;
        size = vsize();
        push_v(get_reg(size, R_BP));
        uint32_t frame = cpu_sp();
        for (int i = 1; i < level; i++) {
            if (cpu.stack32)
                cpu.r[R_BP] -= (uint32_t)size / 8;
            else
                BP = (uint16_t)(BP - size / 8);
            push_v(mread(size, segbase(S_SS) + (cpu.stack32 ? cpu.r[R_BP] : BP)));
        }
        if (level)
            push_v(frame);
        set_reg(size, R_BP, frame);
        sp_add(-(int32_t)alloc);
        return;
    }
    case 0xC9:                          /* LEAVE */
        cpu_set_sp(cpu.stack32 ? cpu.r[R_BP] : BP);
        set_reg(vsize(), R_BP, pop_v());
        return;
    case 0xCA: {                        /* RETF imm16 */
        uint16_t n = fetch16();
        uint32_t ip = pop_v();
        cpu_load_seg(S_CS, (uint16_t)pop_v());
        cpu.ip = ip & vmask();
        sp_add(n);
        return;
    }
    case 0xCB: {
        uint32_t ip = pop_v();
        cpu_load_seg(S_CS, (uint16_t)pop_v());
        cpu.ip = ip & vmask();
        return;
    }
    case 0xCC: cpu_interrupt(3); return;
    case 0xCD: cpu_interrupt(fetch8()); return;
    case 0xCE:
        if (cpu.flags & F_OF)
            cpu_interrupt(4);
        return;
    case 0xCF: {                        /* IRET */
        uint32_t ip = pop_v();
        cpu_load_seg(S_CS, (uint16_t)pop_v());
        cpu.ip = ip & vmask();
        set_flags16(pop_v());
        return;
    }

    case 0xD4: {                        /* AAM */
        uint8_t base = fetch8();
        if (!base) {
            fault(0);
            return;
        }
        uint8_t al = AL;
        AH = al / base;
        AL = al % base;
        szp(AL, 8);
        return;
    }
    case 0xD5: {                        /* AAD */
        uint8_t base = fetch8();
        AL = (uint8_t)(AL + AH * base);
        AH = 0;
        szp(AL, 8);
        return;
    }
    case 0xD6:                          /* SALC */
        AL = (cpu.flags & F_CF) ? 0xFF : 0;
        return;
    case 0xD7: {                        /* XLAT */
        uint32_t off = addrsize32 ? cpu.r[R_BX] + AL : (uint16_t)(BX + AL);
        AL = rb(segbase(seg_ovr >= 0 ? seg_ovr : S_DS) + off);
        return;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        decode(&m);                     /* no coprocessor */
        return;

    case 0xE0:                          /* LOOPNE */
    case 0xE1:                          /* LOOPE */
    case 0xE2: {                        /* LOOP */
        int8_t d = (int8_t)fetch8();
        uint32_t n = str_count() - 1;
        str_set_count(n);
        if (n && (op == 0xE2 || ((cpu.flags & F_ZF) != 0) == (op == 0xE1)))
            jump_rel(d);
        return;
    }
    case 0xE3: {                        /* JCXZ */
        int8_t d = (int8_t)fetch8();
        if (!str_count())
            jump_rel(d);
        return;
    }
    case 0xE4: AL = (uint8_t)io_in(fetch8(), 8); return;
    case 0xE5: size = vsize(); set_reg(size, R_AX, io_in(fetch8(), size)); return;
    case 0xE6: io_out(fetch8(), AL, 8); return;
    case 0xE7: size = vsize(); io_out(fetch8(), get_reg(size, R_AX), size); return;
    case 0xE8: {                        /* CALL near */
        int32_t d = opsize32 ? (int32_t)fetch32() : (int16_t)fetch16();
        push_v(cpu.ip);
        jump_rel(d);
        return;
    }
    case 0xE9: {
        int32_t d = opsize32 ? (int32_t)fetch32() : (int16_t)fetch16();
        jump_rel(d);
        return;
    }
    case 0xEA: {
        uint32_t off = fetch_v();
        far_jump(fetch16(), off);
        return;
    }
    case 0xEB:
        jump_rel((int8_t)fetch8());
        return;
    case 0xEC: AL = (uint8_t)io_in(DX, 8); return;
    case 0xED: size = vsize(); set_reg(size, R_AX, io_in(DX, size)); return;
    case 0xEE: io_out(DX, AL, 8); return;
    case 0xEF: size = vsize(); io_out(DX, get_reg(size, R_AX), size); return;

    case 0xF1: cpu_interrupt(1); return;
    case 0xF4:                          /* HLT: wait for an interrupt */
        cpu.halted = 1;
        cpu.stop = 1;
        return;
    case 0xF5: cpu.flags ^= F_CF; return;
    case 0xF6:                          /* group 3 */
    case 0xF7:
        size = op == 0xF7 ? vsize() : 8;
        decode(&m);
        switch (m.reg) {
        case 0:
        case 1:                         /* TEST */
            arith(OP_AND, rm_get(&m, size), size == 8 ? fetch8() : fetch_v(), size);
            return;
        case 2:                         /* NOT */
            rm_set(&m, size, ~rm_get(&m, size));
            return;
        case 3:                         /* NEG */
            v = rm_get(&m, size);
            rm_set(&m, size, arith(OP_SUB, 0, v, size));
            return;
        default:
            if (muldiv(m.reg, rm_get(&m, size) & MASK(size), size) < 0)
                fault(0);
            return;
        }
    case 0xF8: cpu.flags &= ~F_CF; return;
    case 0xF9: cpu.flags |= F_CF; return;
    case 0xFA: cpu.flags &= ~F_IF; return;
    case 0xFB: cpu.flags |= F_IF; cpu.inhibit = 1; return;
    case 0xFC: cpu.flags &= ~F_DF; return;
    case 0xFD: cpu.flags |= F_DF; return;
    case 0xFE:                          /* group 4 */
        decode(&m);
        if (m.reg > 1) {
            invalid_opcode();
            return;
        }
        rm_set(&m, 8, inc_dec(rm_get(&m, 8), 8, m.reg));
        return;
    case 0xFF:                          /* group 5 */
        size = vsize();
        decode(&m);
        switch (m.reg) {
        case 0:
        case 1:
            rm_set(&m, size, inc_dec(rm_get(&m, size), size, m.reg));
            return;
        case 2:                         /* CALL near indirect */
            v = rm_get(&m, size);
            push_v(cpu.ip);
            cpu.ip = v & vmask();
            return;
        case 3:                         /* CALL far indirect */
        case 5: {                       /* JMP far indirect */
            if (m.is_reg) {
                invalid_opcode();
                return;
            }
            uint32_t off = mread(size, m.addr);
            uint16_t seg = rw(m.addr + (uint32_t)size / 8);
            if (m.reg == 3) {
                push_v(CS);
                push_v(cpu.ip);
            }
            far_jump(seg, off);
            return;
        }
        case 4:                         /* JMP near indirect */
            cpu.ip = rm_get(&m, size) & vmask();
            return;
        case 6:
            push_v(rm_get(&m, size));
            return;
        default:
            invalid_opcode();
            return;
        }
    default:
        invalid_opcode();
        return;
    }
}

/* Run up to count instructions, delivering hardware interrupts between
 * them, until something sets cpu.stop. */
void cpu_run(uint32_t count)
{
    cpu.stop = 0;
    while (count-- && !cpu.stop) {
        if (pic_irr && !cpu.inhibit && (cpu.flags & F_IF)) {
            int irq = pic_pending();
            if (irq >= 0) {
                int vector = irq < 8 ? 8 + irq : 0x70 + irq - 8;
                cpu.halted = 0;
                pic_ack(irq);
                if (cpu.pm)
                    dpmi_interrupt(vector, 1);
                else if (!dpmi_irq_in_real_mode(vector))
                    cpu_interrupt(vector);
            }
        }
        cpu.inhibit = 0;
        if (cpu.halted) {
            cpu.stop = 1;
            break;
        }
        step();
    }
}

void cpu_reset(void)
{
    for (int i = 0; i < 256; i++) {
        int bits = 0;
        for (int b = 0; b < 8; b++)
            bits += (i >> b) & 1;
        parity[i] = !(bits & 1);
    }
    memset(&cpu, 0, sizeof(cpu));
    cpu.flags = 0x0202;                 /* IF set; bit 1 always reads 1 */
    cpu.ip_mask = 0xFFFF;
}
