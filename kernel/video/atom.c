/*
 * AtomBIOS interpreter, ported from Linux's radeon driver (drivers/gpu/
 * drm/radeon/atom.c); see atom.h for its MIT license.
 *
 * Each opcode works on a destination and a source operand: a register,
 * the parameter space (PS, the caller's arguments), the work space (WS,
 * the table's locals and a few special values), the scratch "frame
 * buffer", a data table entry (ID) or an immediate. An operand may be a
 * byte, word or dword lane of its 32-bit location.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "atom.h"

#define ATOM_BIOS_MAGIC     0xAA55
#define ATOM_ATI_MAGIC_PTR  0x30
#define ATOM_ATI_MAGIC      " 761295520"
#define ATOM_ROM_TABLE_PTR  0x48
#define ATOM_ROM_MAGIC      "ATOM"
#define ATOM_ROM_MAGIC_PTR  4
#define ATOM_ROM_MSG_PTR    0x10
#define ATOM_ROM_CMD_PTR    0x1E
#define ATOM_ROM_DATA_PTR   0x20
#define ATOM_DATA_IIO_PTR   0x32

#define ATOM_CT_SIZE_PTR    0
#define ATOM_CT_WS_PTR      4
#define ATOM_CT_PS_PTR      5
#define ATOM_CT_PS_MASK     0x7F
#define ATOM_CT_CODE_PTR    6

#define ATOM_OP_CNT         123
#define ATOM_OP_EOT         91
#define ATOM_CASE_MAGIC     0x63
#define ATOM_CASE_END       0x5A5A

#define ATOM_ARG_REG 0
#define ATOM_ARG_PS  1
#define ATOM_ARG_WS  2
#define ATOM_ARG_FB  3
#define ATOM_ARG_ID  4
#define ATOM_ARG_IMM 5
#define ATOM_ARG_PLL 6
#define ATOM_ARG_MC  7

#define ATOM_SRC_DWORD  0
#define ATOM_SRC_WORD0  1
#define ATOM_SRC_WORD8  2
#define ATOM_SRC_WORD16 3
#define ATOM_SRC_BYTE0  4
#define ATOM_SRC_BYTE8  5
#define ATOM_SRC_BYTE16 6
#define ATOM_SRC_BYTE24 7

#define ATOM_WS_QUOTIENT   0x40
#define ATOM_WS_REMAINDER  0x41
#define ATOM_WS_DATAPTR    0x42
#define ATOM_WS_SHIFT      0x43
#define ATOM_WS_OR_MASK    0x44
#define ATOM_WS_AND_MASK   0x45
#define ATOM_WS_FB_WINDOW  0x46
#define ATOM_WS_ATTRIBUTES 0x47
#define ATOM_WS_REGPTR     0x48

#define ATOM_IIO_NOP        0
#define ATOM_IIO_START      1
#define ATOM_IIO_READ       2
#define ATOM_IIO_WRITE      3
#define ATOM_IIO_CLEAR      4
#define ATOM_IIO_SET        5
#define ATOM_IIO_MOVE_INDEX 6
#define ATOM_IIO_MOVE_ATTR  7
#define ATOM_IIO_MOVE_DATA  8
#define ATOM_IIO_END        9

#define ATOM_IO_MM    0
#define ATOM_IO_PCI   1
#define ATOM_IO_SYSIO 2
#define ATOM_IO_IIO   0x80

#define ATOM_COND_ABOVE        0
#define ATOM_COND_ABOVEOREQUAL 1
#define ATOM_COND_ALWAYS       2
#define ATOM_COND_BELOW        3
#define ATOM_COND_BELOWOREQUAL 4
#define ATOM_COND_EQUAL        5
#define ATOM_COND_NOTEQUAL     6

#define ATOM_PORT_ATI   0
#define ATOM_PORT_PCI   1
#define ATOM_PORT_SYSIO 2

#define ATOM_UNIT_MICROSEC 0
#define ATOM_UNIT_MILLISEC 1

/* The parameter space a table sees: the caller's arguments, then room for
 * what tables called from it address past them. */
#define PS_DWORDS 1024

#define ATOM_MAX_DEPTH 16               /* tables calling tables */

struct exec {
    struct atom_context *ctx;
    uint32_t *ps, *ws;
    uint32_t ps_dwords;                 /* how many of ps there are */
    int depth;
    int ps_shift;
    uint16_t start;
    uint32_t last_jump, last_jump_ticks;
    int abort;
};

static const uint32_t arg_mask[8] = {
    0xFFFFFFFF, 0x0000FFFF, 0x00FFFF00, 0xFFFF0000,
    0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000
};
static const int arg_shift[8] = { 0, 0, 8, 16, 0, 8, 16, 24 };

/* Destination alignment field to the source alignment encoding */
static const int dst_to_src[8][4] = {
    { 0, 0, 0, 0 },
    { 1, 2, 3, 0 },
    { 1, 2, 3, 0 },
    { 1, 2, 3, 0 },
    { 4, 5, 6, 7 },
    { 4, 5, 6, 7 },
    { 4, 5, 6, 7 },
    { 4, 5, 6, 7 },
};
static const int def_dst[8] = { 0, 0, 1, 2, 0, 1, 2, 3 };

uint8_t atom_u8(const struct atom_context *ctx, uint32_t off)
{
    return off < ctx->bios_size ? ctx->bios[off] : 0;
}

uint16_t atom_u16(const struct atom_context *ctx, uint32_t off)
{
    return (uint16_t)(atom_u8(ctx, off) | atom_u8(ctx, off + 1) << 8);
}

uint32_t atom_u32(const struct atom_context *ctx, uint32_t off)
{
    return atom_u16(ctx, off) | (uint32_t)atom_u16(ctx, off + 2) << 16;
}

#define U8(p)  atom_u8(e->ctx, (uint32_t)(p))
#define U16(p) atom_u16(e->ctx, (uint32_t)(p))
#define U32(p) atom_u32(e->ctx, (uint32_t)(p))

static int execute_locked(struct atom_context *ctx, int index, uint32_t *params, uint32_t ps_dwords,
                          int depth);

static uint32_t iio_execute(struct atom_context *ctx, int base, uint32_t index, uint32_t data)
{
    uint32_t temp = 0xCDCDCDCD;
    for (int guard = 0; guard < 4096; guard++) {
        uint32_t op = atom_u8(ctx, (uint32_t)base);
        uint32_t a1 = atom_u8(ctx, (uint32_t)base + 1), a2 = atom_u8(ctx, (uint32_t)base + 2);
        uint32_t a3 = atom_u8(ctx, (uint32_t)base + 3);
        uint32_t m1 = a1 ? 0xFFFFFFFFu >> (32 - a1) : 0;
        switch (op) {
        case ATOM_IIO_NOP:
            base++;
            break;
        case ATOM_IIO_READ:
            temp = ctx->card->ioreg_read(atom_u16(ctx, (uint32_t)base + 1));
            base += 3;
            break;
        case ATOM_IIO_WRITE:
            ctx->card->ioreg_write(atom_u16(ctx, (uint32_t)base + 1), temp);
            base += 3;
            break;
        case ATOM_IIO_CLEAR:
            temp &= ~(m1 << a2);
            base += 3;
            break;
        case ATOM_IIO_SET:
            temp |= m1 << a2;
            base += 3;
            break;
        case ATOM_IIO_MOVE_INDEX:
            temp &= ~(m1 << a3);
            temp |= ((index >> a2) & m1) << a3;
            base += 4;
            break;
        case ATOM_IIO_MOVE_DATA:
            temp &= ~(m1 << a3);
            temp |= ((data >> a2) & m1) << a3;
            base += 4;
            break;
        case ATOM_IIO_MOVE_ATTR:
            temp &= ~(m1 << a3);
            temp |= (((uint32_t)ctx->io_attr >> a2) & m1) << a3;
            base += 4;
            break;
        case ATOM_IIO_END:
            return temp;
        default:
            klog("atom: unknown IIO opcode %u\n", op);
            return 0;
        }
    }
    return temp;
}

static uint32_t reg_read(struct exec *e, uint32_t idx)
{
    struct atom_context *g = e->ctx;
    idx += g->reg_block;
    switch (g->io_mode) {
    case ATOM_IO_MM:
        return g->card->reg_read(idx);
    case ATOM_IO_PCI:
    case ATOM_IO_SYSIO:
        klog("atom: PCI and system I/O registers are not supported\n");
        return 0;
    default:
        if (!(g->io_mode & 0x80) || !g->iio[g->io_mode & 0x7F]) {
            klog("atom: bad I/O mode %x\n", g->io_mode);
            return 0;
        }
        return iio_execute(g, g->iio[g->io_mode & 0x7F], idx, 0);
    }
}

static void reg_write(struct exec *e, uint32_t idx, uint32_t val)
{
    struct atom_context *g = e->ctx;
    idx += g->reg_block;
    switch (g->io_mode) {
    case ATOM_IO_MM:
        g->card->reg_write(idx, idx == 0 ? val << 2 : val);
        break;
    case ATOM_IO_PCI:
    case ATOM_IO_SYSIO:
        klog("atom: PCI and system I/O registers are not supported\n");
        break;
    default:
        if (!(g->io_mode & 0x80) || !g->iio[g->io_mode & 0x7F]) {
            klog("atom: bad I/O mode %x\n", g->io_mode);
            return;
        }
        iio_execute(g, g->iio[g->io_mode & 0x7F], idx, val);
    }
}

static uint32_t get_src_int(struct exec *e, uint8_t attr, int *ptr, uint32_t *saved)
{
    struct atom_context *g = e->ctx;
    uint32_t idx, val = 0xCDCDCDCD, align = (attr >> 3) & 7, arg = attr & 7;
    switch (arg) {
    case ATOM_ARG_REG:
        idx = U16(*ptr);
        *ptr += 2;
        val = reg_read(e, idx);
        break;
    case ATOM_ARG_PS:
        idx = U8(*ptr);
        (*ptr)++;
        val = e->ps[idx];
        break;
    case ATOM_ARG_WS:
        idx = U8(*ptr);
        (*ptr)++;
        switch (idx) {
        case ATOM_WS_QUOTIENT: val = g->divmul[0]; break;
        case ATOM_WS_REMAINDER: val = g->divmul[1]; break;
        case ATOM_WS_DATAPTR: val = g->data_block; break;
        case ATOM_WS_SHIFT: val = g->shift; break;
        case ATOM_WS_OR_MASK: val = 1u << g->shift; break;
        case ATOM_WS_AND_MASK: val = ~(1u << g->shift); break;
        case ATOM_WS_FB_WINDOW: val = g->fb_base; break;
        case ATOM_WS_ATTRIBUTES: val = g->io_attr; break;
        case ATOM_WS_REGPTR: val = g->reg_block; break;
        default: val = e->ws ? e->ws[idx] : 0; break;
        }
        break;
    case ATOM_ARG_ID:
        idx = U16(*ptr);
        *ptr += 2;
        val = U32(idx + g->data_block);
        break;
    case ATOM_ARG_FB:
        idx = U8(*ptr);
        (*ptr)++;
        if (g->fb_base + idx * 4 >= g->scratch_size_bytes) {
            klog("atom: scratch read past its end\n");
            val = 0;
        } else {
            val = g->scratch[g->fb_base / 4 + idx];
        }
        break;
    case ATOM_ARG_IMM:
        switch (align) {
        case ATOM_SRC_DWORD:
            val = U32(*ptr);
            *ptr += 4;
            return val;
        case ATOM_SRC_WORD0:
        case ATOM_SRC_WORD8:
        case ATOM_SRC_WORD16:
            val = U16(*ptr);
            *ptr += 2;
            return val;
        default:
            val = U8(*ptr);
            (*ptr)++;
            return val;
        }
    case ATOM_ARG_PLL:
    case ATOM_ARG_MC:
        /* Indirect PLL and MC registers belong to older chips. */
        (*ptr)++;
        klog("atom: PLL/MC registers are not supported\n");
        val = 0;
        break;
    }
    if (saved)
        *saved = val;
    return (val & arg_mask[align]) >> arg_shift[align];
}

static void skip_src_int(struct exec *e, uint8_t attr, int *ptr)
{
    uint32_t align = (attr >> 3) & 7, arg = attr & 7;
    (void)e;
    switch (arg) {
    case ATOM_ARG_REG:
    case ATOM_ARG_ID:
        *ptr += 2;
        break;
    case ATOM_ARG_PLL:
    case ATOM_ARG_MC:
    case ATOM_ARG_PS:
    case ATOM_ARG_WS:
    case ATOM_ARG_FB:
        (*ptr)++;
        break;
    case ATOM_ARG_IMM:
        if (align == ATOM_SRC_DWORD)
            *ptr += 4;
        else if (align <= ATOM_SRC_WORD16)
            *ptr += 2;
        else
            (*ptr)++;
        break;
    }
}

static uint32_t get_src(struct exec *e, uint8_t attr, int *ptr)
{
    return get_src_int(e, attr, ptr, NULL);
}

static uint32_t get_src_direct(struct exec *e, uint8_t align, int *ptr)
{
    uint32_t val;
    if (align == ATOM_SRC_DWORD) {
        val = U32(*ptr);
        *ptr += 4;
    } else if (align <= ATOM_SRC_WORD16) {
        val = U16(*ptr);
        *ptr += 2;
    } else {
        val = U8(*ptr);
        (*ptr)++;
    }
    return val;
}

static uint32_t get_dst(struct exec *e, int arg, uint8_t attr, int *ptr, uint32_t *saved)
{
    return get_src_int(e, (uint8_t)(arg | dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3] << 3), ptr,
                       saved);
}

static void skip_dst(struct exec *e, int arg, uint8_t attr, int *ptr)
{
    skip_src_int(e, (uint8_t)(arg | dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3] << 3), ptr);
}

static void put_dst(struct exec *e, int arg, uint8_t attr, int *ptr, uint32_t val, uint32_t saved)
{
    struct atom_context *g = e->ctx;
    uint32_t align = (uint32_t)dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3], idx;
    val <<= arg_shift[align];
    val &= arg_mask[align];
    saved &= ~arg_mask[align];
    val |= saved;
    switch (arg) {
    case ATOM_ARG_REG:
        idx = U16(*ptr);
        *ptr += 2;
        reg_write(e, idx, val);
        break;
    case ATOM_ARG_PS:
        idx = U8(*ptr);
        (*ptr)++;
        e->ps[idx] = val;
        break;
    case ATOM_ARG_WS:
        idx = U8(*ptr);
        (*ptr)++;
        switch (idx) {
        case ATOM_WS_QUOTIENT: g->divmul[0] = val; break;
        case ATOM_WS_REMAINDER: g->divmul[1] = val; break;
        case ATOM_WS_DATAPTR: g->data_block = (uint16_t)val; break;
        case ATOM_WS_SHIFT: g->shift = (uint8_t)val; break;
        case ATOM_WS_OR_MASK:
        case ATOM_WS_AND_MASK: break;
        case ATOM_WS_FB_WINDOW: g->fb_base = val; break;
        case ATOM_WS_ATTRIBUTES: g->io_attr = (uint16_t)val; break;
        case ATOM_WS_REGPTR: g->reg_block = (uint16_t)val; break;
        default:
            if (e->ws)
                e->ws[idx] = val;
            break;
        }
        break;
    case ATOM_ARG_FB:
        idx = U8(*ptr);
        (*ptr)++;
        if (g->fb_base + idx * 4 >= g->scratch_size_bytes)
            klog("atom: scratch write past its end\n");
        else
            g->scratch[g->fb_base / 4 + idx] = val;
        break;
    case ATOM_ARG_PLL:
    case ATOM_ARG_MC:
        (*ptr)++;
        klog("atom: PLL/MC registers are not supported\n");
        break;
    }
}

/* ---- opcodes ---- */

typedef uint32_t (*alu_fn)(uint32_t dst, uint32_t src);

static void op_alu(struct exec *e, int *ptr, int arg, alu_fn fn)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t saved;
    int dptr = *ptr;
    uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
    uint32_t src = get_src(e, attr, ptr);
    put_dst(e, arg, attr, &dptr, fn(dst, src), saved);
}

static uint32_t alu_add(uint32_t d, uint32_t s) { return d + s; }
static uint32_t alu_and(uint32_t d, uint32_t s) { return d & s; }
static uint32_t alu_or(uint32_t d, uint32_t s) { return d | s; }
static uint32_t alu_sub(uint32_t d, uint32_t s) { return d - s; }
static uint32_t alu_xor(uint32_t d, uint32_t s) { return d ^ s; }

static void op_add(struct exec *e, int *ptr, int arg) { op_alu(e, ptr, arg, alu_add); }
static void op_and(struct exec *e, int *ptr, int arg) { op_alu(e, ptr, arg, alu_and); }
static void op_or(struct exec *e, int *ptr, int arg) { op_alu(e, ptr, arg, alu_or); }
static void op_sub(struct exec *e, int *ptr, int arg) { op_alu(e, ptr, arg, alu_sub); }
static void op_xor(struct exec *e, int *ptr, int arg) { op_alu(e, ptr, arg, alu_xor); }

static void op_beep(struct exec *e, int *ptr, int arg)
{
    (void)e; (void)ptr; (void)arg;
    klog("atom: the BIOS beeped\n");
}

static void op_calltable(struct exec *e, int *ptr, int arg)
{
    int idx = U8((*ptr)++);
    (void)arg;
    if (U16(e->ctx->cmd_table + 4 + 2 * idx) &&
        execute_locked(e->ctx, idx, e->ps + e->ps_shift, e->ps_dwords - (uint32_t)e->ps_shift,
                       e->depth + 1) < 0)
        e->abort = 1;
}

static void op_clear(struct exec *e, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t saved;
    int dptr = *ptr;
    attr &= 0x38;
    attr |= (uint8_t)(def_dst[attr >> 3] << 6);
    get_dst(e, arg, attr, ptr, &saved);
    put_dst(e, arg, attr, &dptr, 0, saved);
}

static void op_compare(struct exec *e, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst = get_dst(e, arg, attr, ptr, NULL);
    uint32_t src = get_src(e, attr, ptr);
    e->ctx->cs_equal = dst == src;
    e->ctx->cs_above = dst > src;
}

static void op_delay(struct exec *e, int *ptr, int arg)
{
    unsigned count = U8((*ptr)++);
    if (arg == ATOM_UNIT_MICROSEC)
        udelay(count);
    else
        mdelay(count);
}

static void op_div(struct exec *e, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst = get_dst(e, arg, attr, ptr, NULL);
    uint32_t src = get_src(e, attr, ptr);
    e->ctx->divmul[0] = src ? dst / src : 0;
    e->ctx->divmul[1] = src ? dst % src : 0;
}

static void op_eot(struct exec *e, int *ptr, int arg)
{
    (void)e; (void)ptr; (void)arg;
}

static void op_jump(struct exec *e, int *ptr, int arg)
{
    int execute = 0, target = U16(*ptr);
    *ptr += 2;
    struct atom_context *g = e->ctx;
    switch (arg) {
    case ATOM_COND_ABOVE: execute = g->cs_above; break;
    case ATOM_COND_ABOVEOREQUAL: execute = g->cs_above || g->cs_equal; break;
    case ATOM_COND_ALWAYS: execute = 1; break;
    case ATOM_COND_BELOW: execute = !(g->cs_above || g->cs_equal); break;
    case ATOM_COND_BELOWOREQUAL: execute = !g->cs_above; break;
    case ATOM_COND_EQUAL: execute = g->cs_equal; break;
    case ATOM_COND_NOTEQUAL: execute = !g->cs_equal; break;
    }
    if (!execute)
        return;
    /* A table that keeps jumping to the same place for 5 s is stuck. */
    uint32_t dest = (uint32_t)(e->start + target);
    if (e->last_jump == dest) {
        if (ticks - e->last_jump_ticks > 5 * HZ) {
            klog("atom: BIOS table stuck in a loop for 5 s, giving up\n");
            e->abort = 1;
        }
    } else {
        e->last_jump = dest;
        e->last_jump_ticks = ticks;
    }
    *ptr = (int)dest;
}

static void op_mask(struct exec *e, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t saved;
    int dptr = *ptr;
    uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
    uint32_t mask = get_src_direct(e, (attr >> 3) & 7, ptr);
    uint32_t src = get_src(e, attr, ptr);
    put_dst(e, arg, attr, &dptr, (dst & mask) | src, saved);
}

static void op_move(struct exec *e, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t saved;
    int dptr = *ptr;
    if (((attr >> 3) & 7) != ATOM_SRC_DWORD) {
        get_dst(e, arg, attr, ptr, &saved);
    } else {
        skip_dst(e, arg, attr, ptr);
        saved = 0xCDCDCDCD;
    }
    uint32_t src = get_src(e, attr, ptr);
    put_dst(e, arg, attr, &dptr, src, saved);
}

static void op_mul(struct exec *e, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst = get_dst(e, arg, attr, ptr, NULL);
    uint32_t src = get_src(e, attr, ptr);
    e->ctx->divmul[0] = dst * src;
}

static void op_nop(struct exec *e, int *ptr, int arg)
{
    (void)e; (void)ptr; (void)arg;
}

static void op_postcard(struct exec *e, int *ptr, int arg)
{
    (void)arg;
    (*ptr)++;
    (void)e;
}

static void op_unimplemented(struct exec *e, int *ptr, int arg)
{
    (void)ptr; (void)arg;
    klog("atom: unimplemented opcode\n");
    e->abort = 1;
}

static void op_setdatablock(struct exec *e, int *ptr, int arg)
{
    int idx = U8(*ptr);
    (void)arg;
    (*ptr)++;
    if (!idx)
        e->ctx->data_block = 0;
    else if (idx == 255)
        e->ctx->data_block = e->start;
    else
        e->ctx->data_block = U16(e->ctx->data_table + 4 + 2 * idx);
}

static void op_setfbbase(struct exec *e, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    (void)arg;
    e->ctx->fb_base = get_src(e, attr, ptr);
}

static void op_setport(struct exec *e, int *ptr, int arg)
{
    int port;
    switch (arg) {
    case ATOM_PORT_ATI:
        port = U16(*ptr);
        e->ctx->io_mode = port ? ATOM_IO_IIO | port : ATOM_IO_MM;
        *ptr += 2;
        break;
    case ATOM_PORT_PCI:
        e->ctx->io_mode = ATOM_IO_PCI;
        (*ptr)++;
        break;
    case ATOM_PORT_SYSIO:
        e->ctx->io_mode = ATOM_IO_SYSIO;
        (*ptr)++;
        break;
    }
}

static void op_setregblock(struct exec *e, int *ptr, int arg)
{
    (void)arg;
    e->ctx->reg_block = U16(*ptr);
    *ptr += 2;
}

static void op_shift(struct exec *e, int *ptr, int arg, int left)
{
    uint8_t attr = U8((*ptr)++), shift;
    uint32_t saved;
    int dptr = *ptr;
    attr &= 0x38;
    attr |= (uint8_t)(def_dst[attr >> 3] << 6);
    uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
    shift = (uint8_t)get_src_direct(e, ATOM_SRC_BYTE0, ptr);
    dst = left ? dst << shift : dst >> shift;
    put_dst(e, arg, attr, &dptr, dst, saved);
}

static void op_shift_left(struct exec *e, int *ptr, int arg) { op_shift(e, ptr, arg, 1); }
static void op_shift_right(struct exec *e, int *ptr, int arg) { op_shift(e, ptr, arg, 0); }

/* SHL and SHR shift the whole destination, then take its lane. */
static void op_sh(struct exec *e, int *ptr, int arg, int left)
{
    uint8_t attr = U8((*ptr)++), shift;
    uint32_t saved;
    int dptr = *ptr;
    uint32_t dst_align = (uint32_t)dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
    get_dst(e, arg, attr, ptr, &saved);
    uint32_t dst = saved;
    shift = (uint8_t)get_src(e, attr, ptr);
    dst = left ? dst << shift : dst >> shift;
    dst &= arg_mask[dst_align];
    dst >>= arg_shift[dst_align];
    put_dst(e, arg, attr, &dptr, dst, saved);
}

static void op_shl(struct exec *e, int *ptr, int arg) { op_sh(e, ptr, arg, 1); }
static void op_shr(struct exec *e, int *ptr, int arg) { op_sh(e, ptr, arg, 0); }

static void op_switch(struct exec *e, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    (void)arg;
    uint32_t src = get_src(e, attr, ptr);
    while (U16(*ptr) != ATOM_CASE_END) {
        if (U8(*ptr) != ATOM_CASE_MAGIC) {
            klog("atom: bad case in a switch\n");
            e->abort = 1;
            return;
        }
        (*ptr)++;
        uint32_t val = get_src(e, (uint8_t)((attr & 0x38) | ATOM_ARG_IMM), ptr);
        uint32_t target = U16(*ptr);
        if (val == src) {
            *ptr = e->start + (int)target;
            return;
        }
        *ptr += 2;
    }
    *ptr += 2;
}

static void op_test(struct exec *e, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst = get_dst(e, arg, attr, ptr, NULL);
    uint32_t src = get_src(e, attr, ptr);
    e->ctx->cs_equal = (dst & src) == 0;
}

static const struct {
    void (*func)(struct exec *, int *, int);
    int arg;
} opcodes[ATOM_OP_CNT] = {
    { NULL, 0 },
    { op_move, ATOM_ARG_REG }, { op_move, ATOM_ARG_PS }, { op_move, ATOM_ARG_WS },
    { op_move, ATOM_ARG_FB }, { op_move, ATOM_ARG_PLL }, { op_move, ATOM_ARG_MC },
    { op_and, ATOM_ARG_REG }, { op_and, ATOM_ARG_PS }, { op_and, ATOM_ARG_WS },
    { op_and, ATOM_ARG_FB }, { op_and, ATOM_ARG_PLL }, { op_and, ATOM_ARG_MC },
    { op_or, ATOM_ARG_REG }, { op_or, ATOM_ARG_PS }, { op_or, ATOM_ARG_WS },
    { op_or, ATOM_ARG_FB }, { op_or, ATOM_ARG_PLL }, { op_or, ATOM_ARG_MC },
    { op_shift_left, ATOM_ARG_REG }, { op_shift_left, ATOM_ARG_PS }, { op_shift_left, ATOM_ARG_WS },
    { op_shift_left, ATOM_ARG_FB }, { op_shift_left, ATOM_ARG_PLL }, { op_shift_left, ATOM_ARG_MC },
    { op_shift_right, ATOM_ARG_REG }, { op_shift_right, ATOM_ARG_PS }, { op_shift_right, ATOM_ARG_WS },
    { op_shift_right, ATOM_ARG_FB }, { op_shift_right, ATOM_ARG_PLL }, { op_shift_right, ATOM_ARG_MC },
    { op_mul, ATOM_ARG_REG }, { op_mul, ATOM_ARG_PS }, { op_mul, ATOM_ARG_WS },
    { op_mul, ATOM_ARG_FB }, { op_mul, ATOM_ARG_PLL }, { op_mul, ATOM_ARG_MC },
    { op_div, ATOM_ARG_REG }, { op_div, ATOM_ARG_PS }, { op_div, ATOM_ARG_WS },
    { op_div, ATOM_ARG_FB }, { op_div, ATOM_ARG_PLL }, { op_div, ATOM_ARG_MC },
    { op_add, ATOM_ARG_REG }, { op_add, ATOM_ARG_PS }, { op_add, ATOM_ARG_WS },
    { op_add, ATOM_ARG_FB }, { op_add, ATOM_ARG_PLL }, { op_add, ATOM_ARG_MC },
    { op_sub, ATOM_ARG_REG }, { op_sub, ATOM_ARG_PS }, { op_sub, ATOM_ARG_WS },
    { op_sub, ATOM_ARG_FB }, { op_sub, ATOM_ARG_PLL }, { op_sub, ATOM_ARG_MC },
    { op_setport, ATOM_PORT_ATI }, { op_setport, ATOM_PORT_PCI }, { op_setport, ATOM_PORT_SYSIO },
    { op_setregblock, 0 }, { op_setfbbase, 0 },
    { op_compare, ATOM_ARG_REG }, { op_compare, ATOM_ARG_PS }, { op_compare, ATOM_ARG_WS },
    { op_compare, ATOM_ARG_FB }, { op_compare, ATOM_ARG_PLL }, { op_compare, ATOM_ARG_MC },
    { op_switch, 0 },
    { op_jump, ATOM_COND_ALWAYS }, { op_jump, ATOM_COND_EQUAL }, { op_jump, ATOM_COND_BELOW },
    { op_jump, ATOM_COND_ABOVE }, { op_jump, ATOM_COND_BELOWOREQUAL },
    { op_jump, ATOM_COND_ABOVEOREQUAL }, { op_jump, ATOM_COND_NOTEQUAL },
    { op_test, ATOM_ARG_REG }, { op_test, ATOM_ARG_PS }, { op_test, ATOM_ARG_WS },
    { op_test, ATOM_ARG_FB }, { op_test, ATOM_ARG_PLL }, { op_test, ATOM_ARG_MC },
    { op_delay, ATOM_UNIT_MILLISEC }, { op_delay, ATOM_UNIT_MICROSEC },
    { op_calltable, 0 }, { op_unimplemented, 0 },              /* repeat */
    { op_clear, ATOM_ARG_REG }, { op_clear, ATOM_ARG_PS }, { op_clear, ATOM_ARG_WS },
    { op_clear, ATOM_ARG_FB }, { op_clear, ATOM_ARG_PLL }, { op_clear, ATOM_ARG_MC },
    { op_nop, 0 }, { op_eot, 0 },
    { op_mask, ATOM_ARG_REG }, { op_mask, ATOM_ARG_PS }, { op_mask, ATOM_ARG_WS },
    { op_mask, ATOM_ARG_FB }, { op_mask, ATOM_ARG_PLL }, { op_mask, ATOM_ARG_MC },
    { op_postcard, 0 }, { op_beep, 0 },
    { op_unimplemented, 0 }, { op_unimplemented, 0 },          /* savereg, restorereg */
    { op_setdatablock, 0 },
    { op_xor, ATOM_ARG_REG }, { op_xor, ATOM_ARG_PS }, { op_xor, ATOM_ARG_WS },
    { op_xor, ATOM_ARG_FB }, { op_xor, ATOM_ARG_PLL }, { op_xor, ATOM_ARG_MC },
    { op_shl, ATOM_ARG_REG }, { op_shl, ATOM_ARG_PS }, { op_shl, ATOM_ARG_WS },
    { op_shl, ATOM_ARG_FB }, { op_shl, ATOM_ARG_PLL }, { op_shl, ATOM_ARG_MC },
    { op_shr, ATOM_ARG_REG }, { op_shr, ATOM_ARG_PS }, { op_shr, ATOM_ARG_WS },
    { op_shr, ATOM_ARG_FB }, { op_shr, ATOM_ARG_PLL }, { op_shr, ATOM_ARG_MC },
    { op_unimplemented, 0 },                                    /* debug */
};

static int execute_locked(struct atom_context *ctx, int index, uint32_t *params, uint32_t ps_dwords,
                          int depth)
{
    int base = atom_u16(ctx, ctx->cmd_table + 4u + 2u * (uint32_t)index);
    if (!base)
        return -1;
    int ws = atom_u8(ctx, (uint32_t)base + ATOM_CT_WS_PTR);
    int ps = atom_u8(ctx, (uint32_t)base + ATOM_CT_PS_PTR) & ATOM_CT_PS_MASK;
    int ptr = base + ATOM_CT_CODE_PTR;

    /* PS indexes are a byte, and a table it calls starts ps / 4 further on:
     * both must stay inside the parameter space. A BIOS that nests deeper
     * or reaches past it is not run. */
    if (depth > ATOM_MAX_DEPTH || ps_dwords < 256 || (uint32_t)(ps / 4) > ps_dwords - 256) {
        klog("atom: table %d: parameter space or nesting exceeded\n", index);
        return -1;
    }

    struct exec e;
    memset(&e, 0, sizeof(e));
    e.ctx = ctx;
    e.ps_shift = ps / 4;
    e.start = (uint16_t)base;
    e.ps = params;
    e.ps_dwords = ps_dwords;
    e.depth = depth;
    /* WS indexes are a byte; leave room for all of them. */
    e.ws = ws ? kzalloc(256 * sizeof(uint32_t)) : NULL;
    if (ws && !e.ws)
        return -1;

    int ret = 0;
    for (;;) {
        uint8_t op = atom_u8(ctx, (uint32_t)ptr++);
        if (e.abort) {
            klog("atom: giving up on table %d at %04x\n", index, ptr - 1);
            ret = -1;
            break;
        }
        if (op == 0 || op >= ATOM_OP_CNT)
            break;
        opcodes[op].func(&e, &ptr, opcodes[op].arg);
        if (op == ATOM_OP_EOT)
            break;
    }
    kfree(e.ws);
    return ret;
}

int atom_execute(struct atom_context *ctx, int index, void *params, uint32_t size)
{
    static uint32_t ps[PS_DWORDS];
    memset(ps, 0, sizeof(ps));
    if (size > ATOM_PARAMS_BYTES)
        size = ATOM_PARAMS_BYTES;
    if (params)
        memcpy(ps, params, size);
    ctx->data_block = 0;
    ctx->reg_block = 0;
    ctx->fb_base = 0;
    ctx->io_mode = ATOM_IO_MM;
    ctx->divmul[0] = ctx->divmul[1] = 0;
    int r = execute_locked(ctx, index, ps, PS_DWORDS, 0);
    if (params)
        memcpy(params, ps, size);
    return r;
}

static const int iio_len[] = { 1, 2, 3, 3, 3, 3, 4, 4, 4, 3 };

static void index_iio(struct atom_context *ctx, uint32_t base)
{
    memset(ctx->iio, 0, sizeof(ctx->iio));
    while (atom_u8(ctx, base) == ATOM_IIO_START) {
        ctx->iio[atom_u8(ctx, base + 1)] = (uint16_t)(base + 2);
        base += 2;
        while (base < ctx->bios_size && atom_u8(ctx, base) != ATOM_IIO_END) {
            uint8_t op = atom_u8(ctx, base);
            base += op < ARRAY_SIZE(iio_len) ? (uint32_t)iio_len[op] : 1;
        }
        base += 3;
    }
}

uint16_t atom_data_table(struct atom_context *ctx, int index, uint16_t *size, uint8_t *frev,
                         uint8_t *crev)
{
    uint16_t off = atom_u16(ctx, ctx->data_table + 4u + 2u * (uint32_t)index);
    if (!off)
        return 0;
    if (size)
        *size = atom_u16(ctx, off);
    if (frev)
        *frev = atom_u8(ctx, off + 2u);
    if (crev)
        *crev = atom_u8(ctx, off + 3u);
    return off;
}

int atom_cmd_revision(struct atom_context *ctx, int index, uint8_t *frev, uint8_t *crev)
{
    uint16_t off = atom_u16(ctx, ctx->cmd_table + 4u + 2u * (uint32_t)index);
    if (!off)
        return 0;
    if (frev)
        *frev = atom_u8(ctx, off + 2u);
    if (crev)
        *crev = atom_u8(ctx, off + 3u);
    return 1;
}

int atom_parse(struct atom_context *ctx, const struct atom_card *card, const uint8_t *bios,
               uint32_t size)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->card = card;
    ctx->bios = bios;
    ctx->bios_size = size;
    if (atom_u16(ctx, 0) != ATOM_BIOS_MAGIC)
        return -1;
    if (memcmp(bios + ATOM_ATI_MAGIC_PTR, ATOM_ATI_MAGIC, strlen(ATOM_ATI_MAGIC)) != 0)
        return -1;
    uint32_t base = atom_u16(ctx, ATOM_ROM_TABLE_PTR);
    if (base + 8 >= size || memcmp(bios + base + ATOM_ROM_MAGIC_PTR, ATOM_ROM_MAGIC, 4) != 0)
        return -1;
    ctx->cmd_table = atom_u16(ctx, base + ATOM_ROM_CMD_PTR);
    ctx->data_table = atom_u16(ctx, base + ATOM_ROM_DATA_PTR);
    index_iio(ctx, atom_u16(ctx, ctx->data_table + ATOM_DATA_IIO_PTR) + 4u);

    uint32_t msg = atom_u16(ctx, base + ATOM_ROM_MSG_PTR);
    while (atom_u8(ctx, msg) == '\n' || atom_u8(ctx, msg) == '\r')
        msg++;
    size_t n = 0;
    for (uint8_t c; n + 1 < sizeof(ctx->name) && (c = atom_u8(ctx, (uint32_t)(msg + n))) >= '.' &&
                    c <= 'z'; n++)
        ctx->name[n] = (char)c;
    ctx->name[n] = 0;

    /* The scratch area the BIOS asks for in VRAM_UsageByFirmware, else 20 KiB. */
    uint32_t usage = 0;
    uint16_t vu = atom_data_table(ctx, ATOM_DATA_VRAM_USAGE, NULL, NULL, NULL);
    if (vu)
        usage = (uint32_t)atom_u16(ctx, vu + 4u + 4u) * 1024;
    if (!usage || usage > 256 * 1024)
        usage = 20 * 1024;
    ctx->scratch = kzalloc(usage);
    if (!ctx->scratch)
        return -1;
    ctx->scratch_size_bytes = usage;
    return 0;
}
