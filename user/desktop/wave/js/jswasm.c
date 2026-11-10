/*
 * WebAssembly: a decoder, validator and interpreter for WebAssembly modules,
 * and the primitives the WebAssembly namespace (jswasmlib.h) is built on.
 *
 * Compiling a module decodes every section and validates every function
 * body. A function is translated the first time it is called, in the same
 * pass that validated it, into an array of fixed-size instructions whose
 * branches name the instruction they go to, the stack height they unwind
 * to and how many values they carry. The interpreter runs those without
 * recursing in C for calls between WebAssembly functions, on a stack of
 * 64-bit slots shared by every entry into WebAssembly from the engine.
 *
 * Supported: the 1.0 instruction set, sign extension, non-trapping
 * float-to-int conversions, bulk memory, reference types (funcref and
 * externref, several tables), multiple values and mutable globals.
 * Not supported: SIMD, threads and shared memory, exception handling, tail
 * calls, GC types, memory64 and more than one memory.
 *
 * Values on the stack: i32 and f32 in the low 32 bits, i64 and f64 in all
 * 64; a funcref is a struct wfn pointer, an externref a pointer to a jsval
 * the collector keeps (both 0 for null). The stack and tables are scanned
 * by the collector like any other block, so references there stay alive.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jsint.h"

#define ARG(i) ((i) < argc ? args[(i)] : js_undefined)

#define W_PAGE        65536u
#define W_SPEC_PAGES  65536u            /* what a memory type may declare */
#define W_IMPL_PAGES  16384u            /* what this engine will give: 1 GiB */
#define W_IMPL_TABLE  (1u << 22)        /* table elements */
#define W_MAX_LOCALS  50000u
#define W_MAX_PARAMS  1000u             /* params or results of one type */
#define W_MAX_BODY    (8u << 20)        /* bytes of one function body */
#define W_STACK_SLOTS (1u << 16)        /* 512 KiB of 64-bit values */
#define W_MAX_FRAMES  10000

enum { I32 = 0x7F, I64 = 0x7E, F32 = 0x7D, F64 = 0x7C, FUNCREF = 0x70, EXTERNREF = 0x6F, ANY = 0 };
enum { K_FUNC, K_TABLE, K_MEM, K_GLOBAL };
#define FC(n) (0xE0 + (n))              /* 0xFC-prefixed instructions, as translated */

/* ---- modules ---- */

struct wtype {
    uint32_t np, nr;
    uint8_t *t;                         /* params, then results */
};
#define TR(ty, i) ((ty)->t[(ty)->np + (i)])

struct wi {                             /* a translated instruction */
    uint16_t op, x;                     /* x: what a branch carries */
    uint32_t a, b;
};
struct wbr {                            /* a br_table target */
    uint32_t pc, height, arity;
};

struct wcode {                          /* a function defined in the module */
    uint32_t type, nlocals;             /* nlocals counts the params */
    uint8_t *ltype;
    const uint8_t *body, *end;          /* its instructions, in the module's bytes */
    struct wi *code;                    /* translated: NULL until first called */
    struct wbr *brs;
    uint32_t maxstack;
};

struct wlimits {
    uint32_t min, max;
    int has_max;
};
struct wtabdef {
    uint8_t type;
    struct wlimits lim;
};
struct wglobdef {
    uint8_t type, mut;
    const uint8_t *init;
};
struct wimport {
    struct jsstr *mod, *name;
    uint8_t kind, vtype, mut;           /* vtype: a table's elements or a global's */
    uint32_t type;                      /* a function's */
    struct wlimits lim;
};
struct wexport {
    struct jsstr *name;
    uint8_t kind;
    uint32_t idx;
};
struct welem {
    uint8_t mode;                       /* 0 active, 1 passive, 2 declarative */
    uint8_t type;
    uint32_t table, n;
    const uint8_t *offset;
    uint32_t *funcs;                    /* function indices, or NULL for */
    const uint8_t **exprs;              /* constant expressions */
};
struct wdata {
    uint8_t mode;                       /* 0 active, 1 passive */
    const uint8_t *offset, *bytes;
    uint32_t len;
};
struct wcustom {
    struct jsstr *name;
    const uint8_t *p;
    uint32_t len;
};

struct wmod {
    uint8_t *bytes;
    uint32_t len;
    struct wtype *types;
    uint32_t ntypes;
    struct wimport *imports;
    uint32_t nimports;
    uint32_t *ftype, nfuncs, nfimports;
    struct wcode *codes;                /* nfuncs - nfimports */
    struct wtabdef *tables;
    uint32_t ntables, ntimports;
    struct wlimits *mems;
    uint32_t nmems, nmimports;
    struct wglobdef *globals;
    uint32_t nglobals, ngimports;
    struct wexport *exports;
    uint32_t nexports;
    int64_t start;
    struct welem *elems;
    uint32_t nelems;
    struct wdata *datas;
    uint32_t ndatas;
    int64_t datacount;
    struct wcustom *customs;
    uint32_t ncustoms;
    uint8_t *declared;                  /* functions ref.func may name */
};

/* ---- instances ---- */

struct winst;
struct wfn {
    struct wtype *type;
    struct winst *inst;                 /* NULL for a host function */
    struct wcode *code;
    jsval host;                         /* a host function's callable */
    struct jsobj *wrapper;              /* its JavaScript function, once made */
    uint32_t idx;
};
struct wmem {
    uint8_t *data;
    uint32_t pages, max, cap;           /* max: what it may grow to here; cap: pages data has room for */
    uint32_t decl_max;                  /* the maximum it was given, if has_max */
    int has_max;
    struct jsobj *buffer, *handle;
};
struct wtable {
    uint8_t type;
    uint32_t len, max, cap;
    uint32_t decl_max;
    int has_max;
    uint64_t *el;
    struct jsobj *handle;
};
struct wglobal {
    uint8_t type, mut;
    uint64_t v;
    struct jsobj *handle;
};
struct wseg {
    uint32_t n;                         /* 0 once dropped */
    uint64_t *v;
};
struct winst {
    struct wmod *m;
    struct wfn **funcs;
    struct wtable **tables;
    struct wmem *mem;
    struct wglobal **globals;
    struct wseg *elems;
    uint8_t *ddrop;
};

struct wframe {
    struct wfn *f;
    struct wi *pc;
    uint64_t *fp;
};

/* Per engine: the error constructors and the value stack (J->wasm). */
struct wrt {
    jsval err[3];                       /* CompileError, LinkError, RuntimeError */
    uint64_t *stack, *end, *top;
    struct wframe *frames;
    int nframes;
};

static const struct jsclass mod_class = {"WebAssembly.Module", NULL, NULL, NULL};
static const struct jsclass mem_class = {"WebAssembly.Memory", NULL, NULL, NULL};
static const struct jsclass table_class = {"WebAssembly.Table", NULL, NULL, NULL};
static const struct jsclass global_class = {"WebAssembly.Global", NULL, NULL, NULL};
static const struct jsclass fn_class = {"WebAssembly function", NULL, NULL, NULL};

static struct wrt *rt_of(struct js *J)
{
    if (!J->wasm)
        J->wasm = js_alloc(J, sizeof(struct wrt));
    return J->wasm;
}

enum { E_COMPILE, E_LINK, E_RUNTIME };
static jsval wthrow(struct js *J, int kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static jsval wthrow(struct js *J, int kind, const char *fmt, ...)
{
    static const char *names[] = {"CompileError", "LinkError", "RuntimeError"};
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    struct wrt *rt = rt_of(J);
    if (js_is_callable(rt->err[kind])) {
        jsval m = js_str(J, msg);
        jsval e = js_construct(J, rt->err[kind], &m, 1);
        if (!J->throwing)
            js_throw_value(J, e);
        return js_undefined;
    }
    return js_throw_error(J, "%s: %s", names[kind], msg);
}

/* ---- values ---- */

static inline float f32of(uint64_t v)
{
    uint32_t u = (uint32_t)v;
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static inline uint64_t f32bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static inline double f64of(uint64_t v)
{
    double d;
    memcpy(&d, &v, 8);
    return d;
}
static inline uint64_t f64bits(double d)
{
    uint64_t v;
    memcpy(&v, &d, 8);
    return v;
}

static jsval call_export(struct js *J, jsval self, jsval *args, int argc);

/* The JavaScript function for a WebAssembly function: the same one each time. */
static jsval fn_value(struct js *J, struct wfn *f)
{
    if (f->wrapper)
        return js_obj(f->wrapper);
    jsval t = js_new_function(J, call_export, "", (int)f->type->np);
    struct jsobj *w = js_mkobj(J, C_FUNCTION, J->function_proto);
    w->bound = t.u.o;
    w->bound_this = js_obj(js_new_host(J, &fn_class, f));
    char name[16];
    int n = snprintf(name, sizeof(name), "%u", f->idx);
    char *s = js_alloc_atomic(J, (size_t)n + 1);
    memcpy(s, name, (size_t)n + 1);
    w->name = s;
    w->nargs = (int)f->type->np;
    f->wrapper = w;
    return js_obj(w);
}

/* The WebAssembly function behind a JavaScript value, if it is one. */
static struct wfn *fn_of(jsval v)
{
    if (v.t != JS_OBJ || v.u.o->cls != C_FUNCTION || !v.u.o->bound || v.u.o->nbound)
        return NULL;
    if (v.u.o->bound->native != call_export || v.u.o->bound_this.t != JS_OBJ)
        return NULL;
    return js_host_data(v.u.o->bound_this.u.o, &fn_class);
}

static jsval to_js(struct js *J, uint8_t t, uint64_t v)
{
    switch (t) {
    case I32: return js_num((int32_t)(uint32_t)v);
    case I64: return js_bigv(jb_from_i64(J, (int64_t)v));
    case F32: return js_num(f32of(v));
    case F64: return js_num(f64of(v));
    case FUNCREF: return v ? fn_value(J, (struct wfn *)(uintptr_t)v) : js_null;
    default: return v ? *(jsval *)(uintptr_t)v : js_null;
    }
}

static uint32_t to_u32(double d)
{
    if (!isfinite(d))
        return 0;
    d = fmod(trunc(d), 4294967296.0);
    if (d < 0)
        d += 4294967296.0;
    return (uint32_t)d;
}

/* ToWebAssemblyValue: 0 or -1 with an error thrown. */
static int to_wasm(struct js *J, uint8_t t, jsval v, uint64_t *out)
{
    switch (t) {
    case I32: {
        double d = js_to_number(J, v);
        if (J->throwing)
            return -1;
        *out = to_u32(d);
        return 0;
    }
    case I64: {
        if (v.t == JS_OBJ)
            v = js_to_primitive(J, v, HINT_NUMBER);
        if (J->throwing)
            return -1;
        if (v.t != JS_BIG) {
            if (v.t == JS_NUM || v.t == JS_UNDEF || v.t == JS_NULL || v.t == JS_SYM) {
                js_throw_error(J, "TypeError: cannot convert %s to a BigInt", js_text(J, v));
                return -1;
            }
            jsval ctor = js_get(J, js_obj(J->global), "BigInt");
            v = js_call_value(J, ctor, js_undefined, &v, 1);
            if (J->throwing)
                return -1;
        }
        *out = jb_to_u64(v.u.g);
        return 0;
    }
    case F32:
    case F64: {
        double d = js_to_number(J, v);
        if (J->throwing)
            return -1;
        *out = t == F32 ? f32bits((float)d) : f64bits(d);
        return 0;
    }
    case FUNCREF: {
        if (v.t == JS_NULL) {
            *out = 0;
            return 0;
        }
        struct wfn *f = fn_of(v);
        if (!f) {
            js_throw_error(J, "TypeError: a WebAssembly function or null is required");
            return -1;
        }
        *out = (uintptr_t)f;
        return 0;
    }
    default: {
        if (v.t == JS_NULL) {
            *out = 0;
            return 0;
        }
        jsval *box = js_alloc(J, sizeof(jsval));
        *box = v;
        *out = (uintptr_t)box;
        return 0;
    }
    }
}

static int type_eq(const struct wtype *a, const struct wtype *b)
{
    return a == b || (a->np == b->np && a->nr == b->nr && !memcmp(a->t, b->t, a->np + a->nr));
}

/* ---- decoding ---- */

struct ctl {
    uint8_t kind;                       /* 0 block, 1 loop, 2 if, 3 else, 4 the function */
    uint8_t unreachable;
    uint32_t height;
    const struct wtype *bt;
    uint32_t start, ifinsn, fix;
};
struct fix {
    uint32_t where, next;               /* where: an instruction, or a br_table entry | 1 << 31 */
};

struct wcomp {
    struct js *J;
    struct wmod *m;
    void *jb[5];
    char msg[200];
    /* function translation */
    uint8_t *vals;
    uint32_t nvals, capvals, maxh, nlocals;
    struct ctl *ctls;
    uint32_t nctl, capctl;
    struct wi *code;
    uint32_t ncode, capcode;
    struct wbr *brs;
    uint32_t nbrs, capbrs;
    struct fix *fixes;
    uint32_t nfix, capfix;
    uint32_t *depths;
    uint32_t capdepths;
};

struct rd {
    const uint8_t *p, *end;
};

static void wfail(struct wcomp *C, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3), noinline));
static void wfail(struct wcomp *C, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(C->msg, sizeof(C->msg), fmt, ap);
    va_end(ap);
    __builtin_longjmp(C->jb, 1);
}

/* Room for one more of n, doubling. */
static void *grow(struct wcomp *C, void *p, uint32_t n, uint32_t *cap, size_t size, int atomic)
{
    if (n < *cap)
        return p;
    uint32_t c = *cap ? *cap * 2 : 16;
    if (c > (1u << 26))
        wfail(C, "module too large");
    void *q = atomic ? js_alloc_atomic(C->J, c * size) : js_alloc(C->J, c * size);
    if (n)
        memcpy(q, p, n * size);
    *cap = c;
    return q;
}

static uint8_t rd_u8(struct wcomp *C, struct rd *r)
{
    if (r->p >= r->end)
        wfail(C, "unexpected end of section or function");
    return *r->p++;
}

static uint64_t rd_leb(struct wcomp *C, struct rd *r, int bits, int sign)
{
    uint64_t v = 0;
    int shift = 0, max = (bits + 6) / 7;
    uint8_t b;
    for (int i = 0;; i++) {
        if (i >= max)
            wfail(C, "integer representation too long");
        b = rd_u8(C, r);
        v |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    if (shift > bits) {
        int used = bits - (shift - 7), pay = b & 0x7F;
        if (!sign ? (pay >> used) != 0
                  : ((pay >> (used - 1)) != 0 && (pay >> (used - 1)) != (0x7F >> (used - 1))))
            wfail(C, "integer too large");
    }
    if (sign && shift < 64 && (b & 0x40))
        v |= ~(uint64_t)0 << shift;
    return v;
}
#define U32(C, r) ((uint32_t)rd_leb(C, r, 32, 0))

static const uint8_t *rd_bytes(struct wcomp *C, struct rd *r, uint32_t n)
{
    if ((size_t)(r->end - r->p) < n)
        wfail(C, "unexpected end of section or function");
    const uint8_t *p = r->p;
    r->p += n;
    return p;
}

static int utf8_ok(const uint8_t *s, uint32_t n)
{
    for (uint32_t i = 0; i < n;) {
        uint8_t c = s[i];
        uint32_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + len > n)
            return 0;
        uint32_t cp = len == 1 ? c : c & (0x7F >> len);
        for (uint32_t k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += len;
    }
    return 1;
}

static struct jsstr *rd_name(struct wcomp *C, struct rd *r)
{
    uint32_t n = U32(C, r);
    const uint8_t *p = rd_bytes(C, r, n);
    if (!utf8_ok(p, n))
        wfail(C, "malformed UTF-8 encoding");
    return js_mkstr(C->J, (const char *)p, (int)n);
}

static int is_num(uint8_t t)
{
    return t == I32 || t == I64 || t == F32 || t == F64;
}
static int is_ref(uint8_t t)
{
    return t == FUNCREF || t == EXTERNREF;
}

static uint8_t rd_valtype(struct wcomp *C, struct rd *r)
{
    uint8_t t = rd_u8(C, r);
    if (!is_num(t) && !is_ref(t))
        wfail(C, t == 0x7B ? "SIMD (v128) is not supported" : "invalid value type 0x%02x", t);
    return t;
}

static uint8_t rd_reftype(struct wcomp *C, struct rd *r)
{
    uint8_t t = rd_u8(C, r);
    if (!is_ref(t))
        wfail(C, "invalid reference type 0x%02x", t);
    return t;
}

static void rd_limits(struct wcomp *C, struct rd *r, struct wlimits *l, uint32_t bound, const char *what)
{
    uint8_t f = rd_u8(C, r);
    if (f > 1)
        wfail(C, f == 3 ? "shared memory is not supported" : f >= 4 ? "memory64 is not supported"
                                                                   : "invalid limits flags");
    l->min = U32(C, r);
    l->has_max = f;
    l->max = f ? U32(C, r) : 0;
    if (l->min > bound || (f && l->max > bound))
        wfail(C, "%s size must be at most %u", what, bound);
    if (f && l->max < l->min)
        wfail(C, "%s size minimum must not be greater than maximum", what);
}

static void declare(struct wcomp *C, uint32_t f)
{
    struct wmod *m = C->m;
    if (f >= m->nfuncs)
        wfail(C, "unknown function %u", f);
    if (!m->declared)
        m->declared = js_alloc_atomic(C->J, m->nfuncs + 1);
    m->declared[f] = 1;
}

/* A constant expression of type t; nglobals: the globals it may read. */
static const uint8_t *rd_const(struct wcomp *C, struct rd *r, uint8_t t, uint32_t nglobals)
{
    const uint8_t *start = r->p;
    uint8_t st[64];
    int n = 0;
    for (;;) {
        uint8_t op = rd_u8(C, r), push = 0;
        switch (op) {
        case 0x41: rd_leb(C, r, 32, 1); push = I32; break;
        case 0x42: rd_leb(C, r, 64, 1); push = I64; break;
        case 0x43: rd_bytes(C, r, 4); push = F32; break;
        case 0x44: rd_bytes(C, r, 8); push = F64; break;
        case 0x23: {
            uint32_t g = U32(C, r);
            if (g >= nglobals)
                wfail(C, "unknown global %u", g);
            if (C->m->globals[g].mut)
                wfail(C, "constant expression required");
            push = C->m->globals[g].type;
            break;
        }
        case 0xD0: push = rd_reftype(C, r); break;
        case 0xD2: declare(C, U32(C, r)); push = FUNCREF; break;
        case 0x6A: case 0x6B: case 0x6C:
        case 0x7C: case 0x7D: case 0x7E: {
            uint8_t want = op < 0x70 ? I32 : I64;
            if (n < 2 || st[n - 1] != want || st[n - 2] != want)
                wfail(C, "type mismatch in constant expression");
            n--;
            break;
        }
        case 0x0B:
            if (n != 1 || st[0] != t)
                wfail(C, "type mismatch in constant expression");
            return start;
        default:
            wfail(C, "constant expression required");
        }
        if (push) {
            if (n == (int)sizeof(st))
                wfail(C, "constant expression too deep");
            st[n++] = push;
        }
    }
}

/* ---- function bodies: validation and translation ---- */

static const struct wtype empty_type = {0, 0, NULL};
static uint8_t single_t[6] = {I32, I64, F32, F64, FUNCREF, EXTERNREF};
static const struct wtype single_types[6] = {
    {0, 1, &single_t[0]}, {0, 1, &single_t[1]}, {0, 1, &single_t[2]},
    {0, 1, &single_t[3]}, {0, 1, &single_t[4]}, {0, 1, &single_t[5]},
};

static void vpush(struct wcomp *C, uint8_t t)
{
    C->vals = grow(C, C->vals, C->nvals, &C->capvals, 1, 1);
    C->vals[C->nvals++] = t;
    if (C->nvals > C->maxh)
        C->maxh = C->nvals;
}

static uint8_t vpop(struct wcomp *C)
{
    struct ctl *c = &C->ctls[C->nctl - 1];
    if (C->nvals == c->height) {
        if (c->unreachable)
            return ANY;
        wfail(C, "type mismatch: not enough operands");
    }
    return C->vals[--C->nvals];
}

static uint8_t vpopt(struct wcomp *C, uint8_t want)
{
    uint8_t t = vpop(C);
    if (t != want && t != ANY && want != ANY)
        wfail(C, "type mismatch: expected 0x%02x, got 0x%02x", want, t);
    return t == ANY ? want : t;
}

/* The operand k below the top, ANY in unreachable code. */
static uint8_t vpeek(struct wcomp *C, uint32_t k)
{
    struct ctl *c = &C->ctls[C->nctl - 1];
    if (C->nvals - c->height <= k) {
        if (c->unreachable)
            return ANY;
        wfail(C, "type mismatch: not enough operands");
    }
    return C->vals[C->nvals - 1 - k];
}

static void unreachable(struct wcomp *C)
{
    struct ctl *c = &C->ctls[C->nctl - 1];
    c->unreachable = 1;
    C->nvals = c->height;
}

static uint32_t emit(struct wcomp *C, uint16_t op, uint32_t a, uint32_t b)
{
    C->code = grow(C, C->code, C->ncode, &C->capcode, sizeof(struct wi), 1);
    C->code[C->ncode] = (struct wi){op, 0, a, b};
    return C->ncode++;
}

static void add_fix(struct wcomp *C, struct ctl *c, uint32_t where)
{
    C->fixes = grow(C, C->fixes, C->nfix, &C->capfix, sizeof(struct fix), 1);
    C->fixes[C->nfix] = (struct fix){where, c->fix};
    c->fix = ++C->nfix;
}

static void cpush(struct wcomp *C, uint8_t kind, const struct wtype *bt)
{
    for (uint32_t i = bt->np; i-- > 0;)
        vpopt(C, bt->t[i]);
    C->ctls = grow(C, C->ctls, C->nctl, &C->capctl, sizeof(struct ctl), 0);
    C->ctls[C->nctl++] = (struct ctl){kind, 0, C->nvals, bt, 0, 0, 0};
    for (uint32_t i = 0; i < bt->np; i++)
        vpush(C, bt->t[i]);
}

static void pop_results(struct wcomp *C, struct ctl *c)
{
    for (uint32_t i = c->bt->nr; i-- > 0;)
        vpopt(C, TR(c->bt, i));
    if (C->nvals != c->height)
        wfail(C, "type mismatch: values remaining on the stack at the end of a block");
}

static struct ctl *label(struct wcomp *C, uint32_t depth)
{
    if (depth >= C->nctl)
        wfail(C, "unknown label %u", depth);
    return &C->ctls[C->nctl - 1 - depth];
}
static uint32_t arity(const struct ctl *L)
{
    return L->kind == 1 ? L->bt->np : L->bt->nr;
}
static uint8_t label_type(const struct ctl *L, uint32_t i)
{
    return L->kind == 1 ? L->bt->t[i] : TR(L->bt, i);
}

/* A branch to label depth: check and pop what it carries, then emit it. */
static void branch(struct wcomp *C, uint16_t op, uint32_t depth)
{
    struct ctl *L = label(C, depth);
    uint32_t n = arity(L);
    for (uint32_t i = n; i-- > 0;)
        vpopt(C, label_type(L, i));
    uint32_t at = emit(C, op, L->kind == 1 ? L->start : 0, C->nlocals + L->height);
    C->code[at].x = (uint16_t)n;
    if (L->kind != 1)
        add_fix(C, L, at);
}

static struct wtype *func_type(struct wcomp *C, uint32_t f)
{
    if (f >= C->m->nfuncs)
        wfail(C, "unknown function %u", f);
    return &C->m->types[C->m->ftype[f]];
}

static void call_types(struct wcomp *C, const struct wtype *ty)
{
    for (uint32_t i = ty->np; i-- > 0;)
        vpopt(C, ty->t[i]);
    for (uint32_t i = 0; i < ty->nr; i++)
        vpush(C, TR(ty, i));
}

static void need_table(struct wcomp *C, uint32_t t)
{
    if (t >= C->m->ntables)
        wfail(C, "unknown table %u", t);
}
static void need_memory(struct wcomp *C, struct rd *r, int index)
{
    if (index && rd_u8(C, r) != 0)
        wfail(C, "multiple memories are not supported");
    if (!C->m->nmems)
        wfail(C, "unknown memory 0");
}

/* Operand and result types of numeric instructions (0xFC 0-7 as FC(n)). */
static int num_sig(unsigned op, uint8_t *in, uint8_t *out)
{
    static const struct { uint16_t lo, hi; uint8_t n, a, r; } sig[] = {
        {0x45, 0x45, 1, I32, I32}, {0x46, 0x4F, 2, I32, I32}, {0x50, 0x50, 1, I64, I32}, {0x51, 0x5A, 2, I64, I32},
        {0x5B, 0x60, 2, F32, I32}, {0x61, 0x66, 2, F64, I32}, {0x67, 0x69, 1, I32, I32}, {0x6A, 0x78, 2, I32, I32},
        {0x79, 0x7B, 1, I64, I64}, {0x7C, 0x8A, 2, I64, I64}, {0x8B, 0x91, 1, F32, F32}, {0x92, 0x98, 2, F32, F32},
        {0x99, 0x9F, 1, F64, F64}, {0xA0, 0xA6, 2, F64, F64}, {0xA7, 0xA7, 1, I64, I32}, {0xA8, 0xA9, 1, F32, I32},
        {0xAA, 0xAB, 1, F64, I32}, {0xAC, 0xAD, 1, I32, I64}, {0xAE, 0xAF, 1, F32, I64}, {0xB0, 0xB1, 1, F64, I64},
        {0xB2, 0xB3, 1, I32, F32}, {0xB4, 0xB5, 1, I64, F32}, {0xB6, 0xB6, 1, F64, F32}, {0xB7, 0xB8, 1, I32, F64},
        {0xB9, 0xBA, 1, I64, F64}, {0xBB, 0xBB, 1, F32, F64}, {0xBC, 0xBC, 1, F32, I32}, {0xBD, 0xBD, 1, F64, I64},
        {0xBE, 0xBE, 1, I32, F32}, {0xBF, 0xBF, 1, I64, F64}, {0xC0, 0xC1, 1, I32, I32}, {0xC2, 0xC4, 1, I64, I64},
        {FC(0), FC(1), 1, F32, I32}, {FC(2), FC(3), 1, F64, I32}, {FC(4), FC(5), 1, F32, I64}, {FC(6), FC(7), 1, F64, I64},
    };
    for (unsigned i = 0; i < sizeof(sig) / sizeof(sig[0]); i++)
        if (op >= sig[i].lo && op <= sig[i].hi) {
            *in = sig[i].a;
            *out = sig[i].r;
            return sig[i].n;
        }
    return 0;
}

/* Loads 0x28-0x35 and stores 0x36-0x3E: value type and bytes. */
static const uint8_t mem_type[] = {I32, I64, F32, F64, I32, I32, I32, I32, I64, I64, I64, I64, I64, I64,
                                   I32, I64, F32, F64, I32, I32, I64, I64, I64};
static const uint8_t mem_size[] = {4, 8, 4, 8, 1, 1, 2, 2, 1, 1, 2, 2, 4, 4, 4, 8, 4, 8, 1, 2, 1, 2, 4};

static uint32_t memarg(struct wcomp *C, struct rd *r, unsigned size)
{
    uint32_t align = U32(C, r);
    if (align & 0x40)
        wfail(C, "multiple memories are not supported");
    if (align >= 32 || (1u << align) > size)
        wfail(C, "alignment must not be larger than natural");
    return U32(C, r);
}

static const struct wtype *blocktype(struct wcomp *C, struct rd *r)
{
    if (r->p < r->end) {
        uint8_t b = *r->p;
        if (b == 0x40) {
            r->p++;
            return &empty_type;
        }
        for (int i = 0; i < 6; i++)
            if (b == single_t[i]) {
                r->p++;
                return &single_types[i];
            }
    }
    int64_t idx = (int64_t)rd_leb(C, r, 33, 1);
    if (idx < 0 || idx >= C->m->ntypes)
        wfail(C, "unknown type %lld", (long long)idx);
    return &C->m->types[idx];
}

static void compile_body(struct wcomp *C, struct wcode *cd, int keep)
{
    struct wmod *m = C->m;
    struct rd rr = {cd->body, cd->end}, *r = &rr;
    struct wtype *ft = &m->types[cd->type];
    struct wtype fres = {0, ft->nr, ft->t + ft->np};
    C->nvals = C->nctl = C->ncode = C->nbrs = C->nfix = C->maxh = 0;
    C->nlocals = cd->nlocals;
    C->ctls = grow(C, C->ctls, 0, &C->capctl, sizeof(struct ctl), 0);
    C->ctls[C->nctl++] = (struct ctl){4, 0, 0, &fres, 0, 0, 0};
    while (C->nctl) {
        uint32_t op = rd_u8(C, r);
        if (op == 0xFC) {
            uint32_t sub = U32(C, r);
            if (sub > 17)
                wfail(C, "invalid opcode 0xfc %u", sub);
            op = FC(sub);
        }
        uint8_t in, out;
        int n = num_sig(op, &in, &out);
        if (n) {
            vpopt(C, in);
            if (n == 2)
                vpopt(C, in);
            vpush(C, out);
            emit(C, (uint16_t)op, 0, 0);
            continue;
        }
        if (op >= 0x28 && op <= 0x3E) {
            need_memory(C, r, 0);
            uint32_t off = memarg(C, r, mem_size[op - 0x28]);
            if (op <= 0x35) {
                vpopt(C, I32);
                vpush(C, mem_type[op - 0x28]);
            } else {
                vpopt(C, mem_type[op - 0x28]);
                vpopt(C, I32);
            }
            emit(C, (uint16_t)op, off, 0);
            continue;
        }
        switch (op) {
        case 0x00: emit(C, 0x00, 0, 0); unreachable(C); break;
        case 0x01: break;
        case 0x02: cpush(C, 0, blocktype(C, r)); break;
        case 0x03: {
            const struct wtype *bt = blocktype(C, r);
            cpush(C, 1, bt);
            C->ctls[C->nctl - 1].start = emit(C, 0x03, 0, 0);
            break;
        }
        case 0x04: {
            const struct wtype *bt = blocktype(C, r);
            vpopt(C, I32);
            cpush(C, 2, bt);
            C->ctls[C->nctl - 1].ifinsn = emit(C, 0x04, 0, 0);
            break;
        }
        case 0x05: {
            struct ctl *c = &C->ctls[C->nctl - 1];
            if (c->kind != 2)
                wfail(C, "else without if");
            pop_results(C, c);
            add_fix(C, c, emit(C, 0x05, 0, 0));
            C->code[c->ifinsn].a = C->ncode;
            c->kind = 3;
            c->unreachable = 0;
            for (uint32_t i = 0; i < c->bt->np; i++)
                vpush(C, c->bt->t[i]);
            break;
        }
        case 0x0B: {
            struct ctl *c = &C->ctls[C->nctl - 1];
            if (c->kind == 2 && (c->bt->np != c->bt->nr || (c->bt->np && memcmp(c->bt->t, c->bt->t + c->bt->np, c->bt->np))))
                wfail(C, "type mismatch: if without else must leave its parameters as its results");
            pop_results(C, c);
            uint32_t target = c->kind == 4 ? emit(C, 0x0F, 0, 0) : C->ncode;
            if (c->kind == 2)
                C->code[c->ifinsn].a = target;
            for (uint32_t k = c->fix; k; k = C->fixes[k - 1].next) {
                uint32_t w = C->fixes[k - 1].where;
                if (w & 0x80000000u)
                    C->brs[w & 0x7FFFFFFFu].pc = target;
                else
                    C->code[w].a = target;
            }
            const struct wtype *bt = c->bt;
            C->nctl--;
            if (C->nctl)
                for (uint32_t i = 0; i < bt->nr; i++)
                    vpush(C, TR(bt, i));
            break;
        }
        case 0x0C: branch(C, 0x0C, U32(C, r)); unreachable(C); break;
        case 0x0D: {
            uint32_t depth = U32(C, r);
            vpopt(C, I32);
            branch(C, 0x0D, depth);
            struct ctl *L = label(C, depth);
            for (uint32_t i = 0; i < arity(L); i++)
                vpush(C, label_type(L, i));
            break;
        }
        case 0x0E: {
            uint32_t cnt = U32(C, r);
            if (cnt > (uint32_t)(r->end - r->p))
                wfail(C, "unexpected end of function");
            if (cnt + 1 > C->capdepths) {
                C->capdepths = cnt + 16;
                C->depths = js_alloc_atomic(C->J, C->capdepths * sizeof(uint32_t));
            }
            for (uint32_t i = 0; i <= cnt; i++)
                C->depths[i] = U32(C, r);
            vpopt(C, I32);
            struct ctl *D = label(C, C->depths[cnt]);
            uint32_t ar = arity(D);
            for (uint32_t i = 0; i <= cnt; i++) {
                struct ctl *L = label(C, C->depths[i]);
                if (arity(L) != ar)
                    wfail(C, "type mismatch: br_table targets carry different numbers of values");
                for (uint32_t k = 0; k < ar; k++) {
                    uint8_t t = vpeek(C, ar - 1 - k), want = label_type(L, k);
                    if (t != ANY && t != want)
                        wfail(C, "type mismatch in br_table");
                }
            }
            uint32_t base = C->nbrs;
            for (uint32_t i = 0; i <= cnt; i++) {
                struct ctl *L = label(C, C->depths[i]);
                C->brs = grow(C, C->brs, C->nbrs, &C->capbrs, sizeof(struct wbr), 1);
                C->brs[C->nbrs] = (struct wbr){L->kind == 1 ? L->start : 0, C->nlocals + L->height, ar};
                if (L->kind != 1)
                    add_fix(C, L, C->nbrs | 0x80000000u);
                C->nbrs++;
            }
            emit(C, 0x0E, base, cnt);
            unreachable(C);
            break;
        }
        case 0x0F:
            for (uint32_t i = ft->nr; i-- > 0;)
                vpopt(C, TR(ft, i));
            emit(C, 0x0F, 0, 0);
            unreachable(C);
            break;
        case 0x10: {
            uint32_t f = U32(C, r);
            call_types(C, func_type(C, f));
            emit(C, 0x10, f, 0);
            break;
        }
        case 0x11: {
            uint32_t ti = U32(C, r), tab = U32(C, r);
            if (ti >= m->ntypes)
                wfail(C, "unknown type %u", ti);
            need_table(C, tab);
            if (m->tables[tab].type != FUNCREF)
                wfail(C, "call_indirect requires a funcref table");
            vpopt(C, I32);
            call_types(C, &m->types[ti]);
            emit(C, 0x11, ti, tab);
            break;
        }
        case 0x1A: vpop(C); emit(C, 0x1A, 0, 0); break;
        case 0x1B: case 0x1C: {
            uint8_t t = ANY;
            if (op == 0x1C) {
                if (U32(C, r) != 1)
                    wfail(C, "invalid result arity for select");
                t = rd_valtype(C, r);
            }
            vpopt(C, I32);
            uint8_t a = vpopt(C, t), b = vpopt(C, t);
            if (op == 0x1B) {
                if ((a != ANY && !is_num(a)) || (b != ANY && !is_num(b)))
                    wfail(C, "type mismatch: select needs numeric operands or a type");
                if (a != b && a != ANY && b != ANY)
                    wfail(C, "type mismatch in select");
                t = a == ANY ? b : a;
            }
            vpush(C, t);
            emit(C, 0x1B, 0, 0);
            break;
        }
        case 0x20: case 0x21: case 0x22: {
            uint32_t i = U32(C, r);
            if (i >= cd->nlocals)
                wfail(C, "unknown local %u", i);
            uint8_t t = cd->ltype[i];
            if (op != 0x20)
                vpopt(C, t);
            if (op != 0x21)
                vpush(C, t);
            emit(C, (uint16_t)op, i, 0);
            break;
        }
        case 0x23: case 0x24: {
            uint32_t g = U32(C, r);
            if (g >= m->nglobals)
                wfail(C, "unknown global %u", g);
            if (op == 0x24) {
                if (!m->globals[g].mut)
                    wfail(C, "global is immutable");
                vpopt(C, m->globals[g].type);
            } else
                vpush(C, m->globals[g].type);
            emit(C, (uint16_t)op, g, 0);
            break;
        }
        case 0x25: case 0x26: {
            uint32_t t = U32(C, r);
            need_table(C, t);
            if (op == 0x26)
                vpopt(C, m->tables[t].type);
            vpopt(C, I32);
            if (op == 0x25)
                vpush(C, m->tables[t].type);
            emit(C, (uint16_t)op, t, 0);
            break;
        }
        case 0x3F: need_memory(C, r, 1); vpush(C, I32); emit(C, 0x3F, 0, 0); break;
        case 0x40: need_memory(C, r, 1); vpopt(C, I32); vpush(C, I32); emit(C, 0x40, 0, 0); break;
        case 0x41: emit(C, 0x41, (uint32_t)rd_leb(C, r, 32, 1), 0); vpush(C, I32); break;
        case 0x42: {
            uint64_t v = rd_leb(C, r, 64, 1);
            emit(C, 0x42, (uint32_t)v, (uint32_t)(v >> 32));
            vpush(C, I64);
            break;
        }
        case 0x43: {
            const uint8_t *p = rd_bytes(C, r, 4);
            uint32_t v;
            memcpy(&v, p, 4);
            emit(C, 0x43, v, 0);
            vpush(C, F32);
            break;
        }
        case 0x44: {
            const uint8_t *p = rd_bytes(C, r, 8);
            uint32_t lo, hi;
            memcpy(&lo, p, 4);
            memcpy(&hi, p + 4, 4);
            emit(C, 0x44, lo, hi);
            vpush(C, F64);
            break;
        }
        case 0xD0: {
            uint8_t t = rd_reftype(C, r);
            vpush(C, t);
            emit(C, 0xD0, 0, 0);
            break;
        }
        case 0xD1: {
            uint8_t t = vpop(C);
            if (t != ANY && !is_ref(t))
                wfail(C, "type mismatch: ref.is_null needs a reference");
            vpush(C, I32);
            emit(C, 0xD1, 0, 0);
            break;
        }
        case 0xD2: {
            uint32_t f = U32(C, r);
            func_type(C, f);
            if (!m->declared || !m->declared[f])
                wfail(C, "undeclared function reference %u", f);
            vpush(C, FUNCREF);
            emit(C, 0xD2, f, 0);
            break;
        }
        case FC(8): case FC(9): {
            uint32_t d = U32(C, r);
            if (m->datacount < 0)
                wfail(C, "data count section required");
            if (d >= (uint64_t)m->datacount)
                wfail(C, "unknown data segment %u", d);
            if (op == FC(8)) {
                need_memory(C, r, 1);
                vpopt(C, I32);
                vpopt(C, I32);
                vpopt(C, I32);
            }
            emit(C, (uint16_t)op, d, 0);
            break;
        }
        case FC(10): case FC(11):
            need_memory(C, r, 1);
            if (op == FC(10))
                need_memory(C, r, 1);
            vpopt(C, I32);
            vpopt(C, I32);
            vpopt(C, I32);
            emit(C, (uint16_t)op, 0, 0);
            break;
        case FC(12): {
            uint32_t e = U32(C, r), t = U32(C, r);
            if (e >= m->nelems)
                wfail(C, "unknown element segment %u", e);
            need_table(C, t);
            if (m->elems[e].type != m->tables[t].type)
                wfail(C, "type mismatch in table.init");
            vpopt(C, I32);
            vpopt(C, I32);
            vpopt(C, I32);
            emit(C, (uint16_t)op, e, t);
            break;
        }
        case FC(13): {
            uint32_t e = U32(C, r);
            if (e >= m->nelems)
                wfail(C, "unknown element segment %u", e);
            emit(C, (uint16_t)op, e, 0);
            break;
        }
        case FC(14): {
            uint32_t d = U32(C, r), s = U32(C, r);
            need_table(C, d);
            need_table(C, s);
            if (m->tables[d].type != m->tables[s].type)
                wfail(C, "type mismatch in table.copy");
            vpopt(C, I32);
            vpopt(C, I32);
            vpopt(C, I32);
            emit(C, (uint16_t)op, d, s);
            break;
        }
        case FC(15): case FC(16): case FC(17): {
            uint32_t t = U32(C, r);
            need_table(C, t);
            if (op == FC(15)) {
                vpopt(C, I32);
                vpopt(C, m->tables[t].type);
                vpush(C, I32);
            } else if (op == FC(16))
                vpush(C, I32);
            else {
                vpopt(C, I32);
                vpopt(C, m->tables[t].type);
                vpopt(C, I32);
            }
            emit(C, (uint16_t)op, t, 0);
            break;
        }
        case 0x06: case 0x07: case 0x08: case 0x09: case 0x18: case 0x19: case 0x1F:
            wfail(C, "exception handling is not supported");
        case 0x12: case 0x13:
            wfail(C, "tail calls are not supported");
        case 0xFD:
            wfail(C, "SIMD is not supported");
        case 0xFE:
            wfail(C, "threads are not supported");
        default:
            wfail(C, "invalid opcode 0x%02x", op);
        }
    }
    if (r->p != r->end)
        wfail(C, "operators remaining after the end of the function");
    if (keep) {
        cd->maxstack = C->maxh + 1;
        struct wi *code = js_alloc_atomic(C->J, C->ncode * sizeof(struct wi));
        memcpy(code, C->code, C->ncode * sizeof(struct wi));
        if (C->nbrs) {
            cd->brs = js_alloc_atomic(C->J, C->nbrs * sizeof(struct wbr));
            memcpy(cd->brs, C->brs, C->nbrs * sizeof(struct wbr));
        }
        cd->code = code;
    }
}

/* ---- modules ---- */

static int name_cmp(const void *a, const void *b)
{
    const struct jsstr *x = ((const struct wexport *)a)->name, *y = ((const struct wexport *)b)->name;
    int n = x->len < y->len ? x->len : y->len, c = memcmp(x->s, y->s, (size_t)n);
    return c ? c : x->len - y->len;
}

static void decode(struct wcomp *C, struct wmod *m)
{
    struct js *J = C->J;
    struct rd top = {m->bytes, m->bytes + m->len}, *r = &top;
    static const uint8_t magic[8] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    if (m->len < 4 || memcmp(m->bytes, magic, 4))
        wfail(C, "expected magic word 00 61 73 6d");
    if (m->len < 8 || memcmp(m->bytes + 4, magic + 4, 4))
        wfail(C, "expected version 01 00 00 00");
    r->p += 8;
    /* where each section may come, in order */
    static const uint8_t rank[14] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 11, 6};
    int last = 0;
    uint32_t nbodies = 0;
    int have_funcs = 0;
    m->start = -1;
    m->datacount = -1;
    while (r->p < r->end) {
        uint8_t id = rd_u8(C, r);
        uint32_t size = U32(C, r);
        struct rd s = {rd_bytes(C, r, size), NULL};
        s.end = s.p + size;
        if (id > 13)
            wfail(C, "malformed section id %u", id);
        if (id == 13)
            wfail(C, "exception handling is not supported");
        if (id) {
            if (rank[id] <= last)
                wfail(C, "unexpected section %u: sections out of order or repeated", id);
            last = rank[id];
        }
        switch (id) {
        case 0: {
            struct jsstr *name = rd_name(C, &s);
            uint32_t cap = m->ncustoms;
            struct wcustom *c = js_alloc(J, (m->ncustoms + 1) * sizeof(*c));
            if (cap)
                memcpy(c, m->customs, cap * sizeof(*c));
            c[m->ncustoms++] = (struct wcustom){name, s.p, (uint32_t)(s.end - s.p)};
            m->customs = c;
            s.p = s.end;
            break;
        }
        case 1: {
            uint32_t n = U32(C, &s);
            if (n > size)
                wfail(C, "too many types");
            m->types = js_alloc(J, (n + 1) * sizeof(struct wtype));
            for (uint32_t i = 0; i < n; i++) {
                uint8_t form = rd_u8(C, &s);
                if (form != 0x60)
                    wfail(C, form == 0x4E || form == 0x50 || form == 0x4F ? "GC types are not supported"
                                                                         : "invalid function type form 0x%02x", form);
                uint32_t np = U32(C, &s);
                if (np > W_MAX_PARAMS || np > (uint32_t)(s.end - s.p))
                    wfail(C, "too many parameters");
                struct rd save = s;
                rd_bytes(C, &s, np);
                uint32_t nr = U32(C, &s);
                if (nr > W_MAX_PARAMS || nr > (uint32_t)(s.end - s.p))
                    wfail(C, "too many results");
                struct wtype *ty = &m->types[i];
                ty->np = np;
                ty->nr = nr;
                ty->t = js_alloc_atomic(J, np + nr + 1);
                s = save;
                for (uint32_t k = 0; k < np; k++)
                    ty->t[k] = rd_valtype(C, &s);
                (void)U32(C, &s);
                for (uint32_t k = 0; k < nr; k++)
                    ty->t[np + k] = rd_valtype(C, &s);
            }
            m->ntypes = n;
            break;
        }
        case 2: {
            uint32_t n = U32(C, &s);
            if (n > size)
                wfail(C, "too many imports");
            m->imports = js_alloc(J, (n + 1) * sizeof(struct wimport));
            m->ftype = js_alloc_atomic(J, (n + 1) * sizeof(uint32_t));
            m->tables = js_alloc(J, (n + 1) * sizeof(struct wtabdef));
            m->mems = js_alloc(J, (n + 1) * sizeof(struct wlimits));
            m->globals = js_alloc(J, (n + 1) * sizeof(struct wglobdef));
            for (uint32_t i = 0; i < n; i++) {
                struct wimport *im = &m->imports[i];
                im->mod = rd_name(C, &s);
                im->name = rd_name(C, &s);
                im->kind = rd_u8(C, &s);
                switch (im->kind) {
                case K_FUNC:
                    im->type = U32(C, &s);
                    if (im->type >= m->ntypes)
                        wfail(C, "unknown type %u", im->type);
                    m->ftype[m->nfuncs++] = im->type;
                    break;
                case K_TABLE:
                    im->vtype = rd_reftype(C, &s);
                    rd_limits(C, &s, &im->lim, 0xFFFFFFFFu, "table");
                    m->tables[m->ntables++] = (struct wtabdef){im->vtype, im->lim};
                    break;
                case K_MEM:
                    rd_limits(C, &s, &im->lim, W_SPEC_PAGES, "memory");
                    if (m->nmems)
                        wfail(C, "multiple memories are not supported");
                    m->mems[m->nmems++] = im->lim;
                    break;
                case K_GLOBAL:
                    im->vtype = rd_valtype(C, &s);
                    im->mut = rd_u8(C, &s);
                    if (im->mut > 1)
                        wfail(C, "invalid mutability");
                    m->globals[m->nglobals++] = (struct wglobdef){im->vtype, im->mut, NULL};
                    break;
                default:
                    wfail(C, im->kind == 4 ? "exception handling is not supported" : "invalid import kind %u", im->kind);
                }
            }
            m->nimports = n;
            m->nfimports = m->nfuncs;
            m->ntimports = m->ntables;
            m->nmimports = m->nmems;
            m->ngimports = m->nglobals;
            break;
        }
        case 3: {
            uint32_t n = U32(C, &s);
            if (n > size)
                wfail(C, "too many functions");
            uint32_t *ft = js_alloc_atomic(J, (m->nfuncs + n + 1) * sizeof(uint32_t));
            if (m->nfuncs)
                memcpy(ft, m->ftype, m->nfuncs * sizeof(uint32_t));
            m->ftype = ft;
            m->codes = js_alloc(J, (n + 1) * sizeof(struct wcode));
            for (uint32_t i = 0; i < n; i++) {
                uint32_t t = U32(C, &s);
                if (t >= m->ntypes)
                    wfail(C, "unknown type %u", t);
                m->ftype[m->nfuncs++] = t;
                m->codes[i].type = t;
            }
            have_funcs = 1;
            break;
        }
        case 4: {
            uint32_t n = U32(C, &s);
            if (n > size)
                wfail(C, "too many tables");
            struct wtabdef *t = js_alloc(J, (m->ntables + n + 1) * sizeof(*t));
            if (m->ntables)
                memcpy(t, m->tables, m->ntables * sizeof(*t));
            m->tables = t;
            for (uint32_t i = 0; i < n; i++) {
                if (s.p < s.end && *s.p == 0x40)
                    wfail(C, "table initializers are not supported");
                t[m->ntables].type = rd_reftype(C, &s);
                rd_limits(C, &s, &t[m->ntables].lim, 0xFFFFFFFFu, "table");
                m->ntables++;
            }
            break;
        }
        case 5: {
            uint32_t n = U32(C, &s);
            if (n + m->nmems > 1)
                wfail(C, "multiple memories are not supported");
            struct wlimits *l = js_alloc(J, 2 * sizeof(*l));
            if (m->nmems)
                l[0] = m->mems[0];
            m->mems = l;
            for (uint32_t i = 0; i < n; i++)
                rd_limits(C, &s, &m->mems[m->nmems++], W_SPEC_PAGES, "memory");
            break;
        }
        case 6: {
            uint32_t n = U32(C, &s);
            if (n > size)
                wfail(C, "too many globals");
            struct wglobdef *g = js_alloc(J, (m->nglobals + n + 1) * sizeof(*g));
            if (m->nglobals)
                memcpy(g, m->globals, m->nglobals * sizeof(*g));
            m->globals = g;
            for (uint32_t i = 0; i < n; i++) {
                struct wglobdef *d = &g[m->nglobals];
                d->type = rd_valtype(C, &s);
                d->mut = rd_u8(C, &s);
                if (d->mut > 1)
                    wfail(C, "invalid mutability");
                d->init = rd_const(C, &s, d->type, m->nglobals);
                m->nglobals++;
            }
            break;
        }
        case 7: {
            uint32_t n = U32(C, &s);
            if (n > size)
                wfail(C, "too many exports");
            m->exports = js_alloc(J, (n + 1) * sizeof(struct wexport));
            for (uint32_t i = 0; i < n; i++) {
                struct wexport *e = &m->exports[i];
                e->name = rd_name(C, &s);
                e->kind = rd_u8(C, &s);
                e->idx = U32(C, &s);
                uint32_t lim = e->kind == K_FUNC ? m->nfuncs : e->kind == K_TABLE ? m->ntables
                             : e->kind == K_MEM ? m->nmems : e->kind == K_GLOBAL ? m->nglobals : 0;
                if (e->kind > K_GLOBAL)
                    wfail(C, "invalid export kind %u", e->kind);
                if (e->idx >= lim)
                    wfail(C, "unknown export index %u", e->idx);
                if (e->kind == K_FUNC)
                    declare(C, e->idx);
            }
            m->nexports = n;
            if (n > 1) {
                struct wexport *sorted = js_alloc(J, n * sizeof(struct wexport));
                memcpy(sorted, m->exports, n * sizeof(struct wexport));
                qsort(sorted, n, sizeof(struct wexport), name_cmp);
                for (uint32_t i = 1; i < n; i++)
                    if (!name_cmp(&sorted[i - 1], &sorted[i]))
                        wfail(C, "duplicate export name \"%s\"", sorted[i].name->s);
            }
            break;
        }
        case 8: {
            uint32_t f = U32(C, &s);
            struct wtype *ty = func_type(C, f);
            if (ty->np || ty->nr)
                wfail(C, "the start function must take and return nothing");
            m->start = f;
            break;
        }
        case 9: {
            uint32_t n = U32(C, &s);
            if (n > size)
                wfail(C, "too many element segments");
            m->elems = js_alloc(J, (n + 1) * sizeof(struct welem));
            for (uint32_t i = 0; i < n; i++) {
                struct welem *e = &m->elems[i];
                uint32_t flags = U32(C, &s);
                if (flags > 7)
                    wfail(C, "invalid element segment flags %u", flags);
                e->mode = flags & 1 ? (flags & 2 ? 2 : 1) : 0;
                e->type = FUNCREF;
                if (!(flags & 1)) {
                    e->table = flags & 2 ? U32(C, &s) : 0;
                    need_table(C, e->table);
                    e->offset = rd_const(C, &s, I32, m->nglobals);
                }
                if (flags & 3) {
                    if (flags & 4)
                        e->type = rd_reftype(C, &s);
                    else if (rd_u8(C, &s) != 0)
                        wfail(C, "invalid element kind");
                }
                e->n = U32(C, &s);
                if (e->n > (uint32_t)(s.end - s.p))
                    wfail(C, "unexpected end of element segment");
                if (flags & 4) {
                    e->exprs = js_alloc(J, (e->n + 1) * sizeof(*e->exprs));
                    for (uint32_t k = 0; k < e->n; k++)
                        e->exprs[k] = rd_const(C, &s, e->type, m->nglobals);
                } else {
                    e->funcs = js_alloc_atomic(J, (e->n + 1) * sizeof(uint32_t));
                    for (uint32_t k = 0; k < e->n; k++) {
                        e->funcs[k] = U32(C, &s);
                        declare(C, e->funcs[k]);
                    }
                }
                if (e->mode == 0 && m->tables[e->table].type != e->type)
                    wfail(C, "type mismatch: element segment and table");
            }
            m->nelems = n;
            break;
        }
        case 12:
            m->datacount = U32(C, &s);
            break;
        case 10: {
            uint32_t n = U32(C, &s);
            if (n != m->nfuncs - m->nfimports)
                wfail(C, "function and code section have inconsistent lengths");
            for (uint32_t i = 0; i < n; i++) {
                struct wcode *cd = &m->codes[i];
                uint32_t len = U32(C, &s);
                if (len > W_MAX_BODY)
                    wfail(C, "function body too large");
                struct rd b = {rd_bytes(C, &s, len), NULL};
                b.end = b.p + len;
                struct wtype *ty = &m->types[cd->type];
                uint32_t groups = U32(C, &b), total = ty->np;
                struct rd save = b;
                for (uint32_t k = 0; k < groups; k++) {
                    uint32_t cnt = U32(C, &b);
                    rd_valtype(C, &b);
                    if (cnt > W_MAX_LOCALS || (total += cnt) > W_MAX_LOCALS)
                        wfail(C, "too many locals");
                }
                cd->nlocals = total;
                cd->ltype = js_alloc_atomic(J, total + 1);
                memcpy(cd->ltype, ty->t, ty->np);
                b = save;
                total = ty->np;
                for (uint32_t k = 0; k < groups; k++) {
                    uint32_t cnt = U32(C, &b);
                    uint8_t t = rd_valtype(C, &b);
                    memset(cd->ltype + total, t, cnt);
                    total += cnt;
                }
                cd->body = b.p;
                cd->end = b.end;
            }
            nbodies = n;
            break;
        }
        case 11: {
            uint32_t n = U32(C, &s);
            if (n > size)
                wfail(C, "too many data segments");
            if (m->datacount >= 0 && n != m->datacount)
                wfail(C, "data count and data section have inconsistent lengths");
            m->datas = js_alloc(J, (n + 1) * sizeof(struct wdata));
            for (uint32_t i = 0; i < n; i++) {
                struct wdata *d = &m->datas[i];
                uint32_t flags = U32(C, &s);
                if (flags > 2)
                    wfail(C, "invalid data segment flags %u", flags);
                d->mode = flags & 1;
                if (!(flags & 1)) {
                    if (flags == 2 && U32(C, &s) != 0)
                        wfail(C, "multiple memories are not supported");
                    if (!m->nmems)
                        wfail(C, "unknown memory 0");
                    d->offset = rd_const(C, &s, I32, m->nglobals);
                }
                d->len = U32(C, &s);
                d->bytes = rd_bytes(C, &s, d->len);
            }
            m->ndatas = n;
            break;
        }
        }
        if (s.p != s.end)
            wfail(C, "section size mismatch");
    }
    if (have_funcs && m->nfuncs > m->nfimports && nbodies != m->nfuncs - m->nfimports)
        wfail(C, "function and code section have inconsistent lengths");
    if (m->datacount >= 0 && !m->datas && m->datacount)
        wfail(C, "data count and data section have inconsistent lengths");
    for (uint32_t i = 0; i < m->nfuncs - m->nfimports; i++)
        compile_body(C, &m->codes[i], 0);
}

static struct wmod *compile(struct js *J, jsval src, int quiet)
{
    int len = -1;
    unsigned char *p = js_binary_bytes(src, &len);
    if (!p) {                           /* not bytes, or a detached buffer's */
        if (!quiet)
            js_throw_error(J, "TypeError: WebAssembly needs an ArrayBuffer or a typed array of bytes");
        return NULL;
    }
    if (!quiet && J->wasm_allowed && !J->wasm_allowed(J)) {
        wthrow(J, E_COMPILE, "WebAssembly compilation is blocked by the Content Security Policy");
        return NULL;
    }
    struct wmod *m = js_alloc(J, sizeof(*m));
    m->len = (uint32_t)len;
    m->bytes = js_alloc_atomic(J, (size_t)len + 1);
    memcpy(m->bytes, p, (size_t)len);
    struct wcomp *C = js_alloc(J, sizeof(*C));
    C->J = J;
    C->m = m;
    if (__builtin_setjmp(C->jb)) {
        if (!quiet)
            wthrow(J, E_COMPILE, "%s", C->msg);
        return NULL;
    }
    decode(C, m);
    return m;
}

/* Translate a function the first time it runs. It was validated already. */
static int translate(struct js *J, struct wmod *m, struct wcode *cd)
{
    struct wcomp *C = js_alloc(J, sizeof(*C));
    C->J = J;
    C->m = m;
    if (__builtin_setjmp(C->jb))
        return -1;
    compile_body(C, cd, 1);
    return 0;
}

/* ---- memories, tables and globals ---- */

static int room_for(struct js *J, size_t n)
{
    if (gc_heap_bytes(J) + n <= J->heap_limit)
        return 1;
    gc_collect(J);
    return gc_heap_bytes(J) + n <= J->heap_limit;
}

static struct wmem *mem_new(struct js *J, uint32_t min, uint32_t max, int has_max)
{
    struct wmem *m = js_alloc(J, sizeof(*m));
    m->max = has_max && max < W_IMPL_PAGES ? max : W_IMPL_PAGES;
    if (min > m->max || !room_for(J, (size_t)min * W_PAGE + 16)) {
        js_throw_error(J, "RangeError: could not allocate %u pages of WebAssembly memory", min);
        return NULL;
    }
    m->data = js_alloc_atomic(J, (size_t)min * W_PAGE + 16);
    m->pages = m->cap = min;
    m->decl_max = max;
    m->has_max = has_max;
    return m;
}

/* The old size in pages, or -1. */
static int64_t mem_grow(struct js *J, struct wmem *m, uint32_t delta)
{
    if (delta > m->max - m->pages)
        return -1;
    uint32_t want = m->pages + delta;
    if (want > m->cap) {
        uint32_t cap = want + want / 2 + 1;
        if (cap > m->max)
            cap = m->max;
        if (!room_for(J, (size_t)cap * W_PAGE + 16)) {
            cap = want;
            if (!room_for(J, (size_t)cap * W_PAGE + 16))
                return -1;
        }
        uint8_t *d = js_alloc_atomic(J, (size_t)cap * W_PAGE + 16);
        memcpy(d, m->data, (size_t)m->pages * W_PAGE);
        m->data = d;
        m->cap = cap;
    }
    uint32_t old = m->pages;
    m->pages = want;
    if (m->buffer) {                    /* the old ArrayBuffer is detached; a new one comes next */
        js_buffer_detach(js_obj(m->buffer));
        m->buffer = NULL;
    }
    return old;
}

static struct wtable *table_new(struct js *J, uint8_t type, uint32_t min, uint32_t max, int has_max, uint64_t init)
{
    struct wtable *t = js_alloc(J, sizeof(*t));
    t->type = type;
    t->max = has_max && max < W_IMPL_TABLE ? max : W_IMPL_TABLE;
    if (min > t->max || !room_for(J, (size_t)min * 8 + 8)) {
        js_throw_error(J, "RangeError: could not allocate a table of %u elements", min);
        return NULL;
    }
    t->el = js_alloc(J, (size_t)min * 8 + 8);
    t->len = t->cap = min;
    t->decl_max = max;
    t->has_max = has_max;
    for (uint32_t i = 0; i < min; i++)
        t->el[i] = init;
    return t;
}

static int64_t table_grow(struct js *J, struct wtable *t, uint32_t delta, uint64_t init)
{
    if (delta > t->max - t->len)
        return -1;
    uint32_t want = t->len + delta;
    if (want > t->cap) {
        uint32_t cap = want + want / 2 + 1;
        if (cap > t->max)
            cap = t->max;
        if (!room_for(J, (size_t)cap * 8 + 8))
            return -1;
        uint64_t *el = js_alloc(J, (size_t)cap * 8 + 8);
        memcpy(el, t->el, (size_t)t->len * 8);
        t->el = el;
        t->cap = cap;
    }
    uint32_t old = t->len;
    for (uint32_t i = old; i < want; i++)
        t->el[i] = init;
    t->len = want;
    return old;
}

static jsval handle(struct js *J, struct jsobj **slot, const struct jsclass *cls, void *data)
{
    if (!*slot)
        *slot = js_new_host(J, cls, data);
    return js_obj(*slot);
}

/* ---- the interpreter ---- */

static int host_call(struct js *J, struct wrt *rt, struct wfn *f, uint64_t *fp);

static uint64_t const_value(struct winst *I, const uint8_t *p);

static int trunc_check(double x, double lo, double hi)
{
    if (x != x)
        return 1;
    return !(x > lo && x < hi) ? 2 : 0;
}

static inline float wminf(float a, float b)
{
    if (a != a || b != b)
        return a + b;
    if (a == 0 && b == 0)
        return signbit(a) ? a : b;
    return a < b ? a : b;
}
static inline float wmaxf(float a, float b)
{
    if (a != a || b != b)
        return a + b;
    if (a == 0 && b == 0)
        return signbit(a) ? b : a;
    return a > b ? a : b;
}
static inline double wmin(double a, double b)
{
    if (a != a || b != b)
        return a + b;
    if (a == 0 && b == 0)
        return signbit(a) ? a : b;
    return a < b ? a : b;
}
static inline double wmax(double a, double b)
{
    if (a != a || b != b)
        return a + b;
    if (a == 0 && b == 0)
        return signbit(a) ? b : a;
    return a > b ? a : b;
}

static inline uint32_t rotl32(uint32_t a, uint32_t b) { b &= 31; return b ? a << b | a >> (32 - b) : a; }
static inline uint32_t rotr32(uint32_t a, uint32_t b) { b &= 31; return b ? a >> b | a << (32 - b) : a; }
static inline uint64_t rotl64(uint64_t a, uint64_t b) { b &= 63; return b ? a << b | a >> (64 - b) : a; }
static inline uint64_t rotr64(uint64_t a, uint64_t b) { b &= 63; return b ? a >> b | a << (64 - b) : a; }

static int64_t sat_s(double x, double lo, double hi, int64_t min, int64_t max)
{
    return x != x ? 0 : x <= lo ? min : x >= hi ? max : (int64_t)x;
}
static uint64_t sat_u(double x, double hi, uint64_t max)
{
    return x != x || x <= -1.0 ? 0 : x >= hi ? max : (uint64_t)x;
}

#define S32(v) ((int32_t)(uint32_t)(v))
#define R32(e) ((uint64_t)(uint32_t)(e))
#define TRAP(m) do { msg = (m); goto trap; } while (0)
#define BIN_I32(e) { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2]; (void)a; (void)b; sp--; sp[-1] = R32(e); break; }
#define BIN_I64(e) { uint64_t b = sp[-1], a = sp[-2]; (void)a; (void)b; sp--; sp[-1] = (uint64_t)(e); break; }
#define BIN_F32(e) { float b = f32of(sp[-1]), a = f32of(sp[-2]); sp--; sp[-1] = f32bits(e); break; }
#define BIN_F64(e) { double b = f64of(sp[-1]), a = f64of(sp[-2]); sp--; sp[-1] = f64bits(e); break; }
#define CMP_F32(e) { float b = f32of(sp[-1]), a = f32of(sp[-2]); sp--; sp[-1] = (e) ? 1 : 0; break; }
#define CMP_F64(e) { double b = f64of(sp[-1]), a = f64of(sp[-2]); sp--; sp[-1] = (e) ? 1 : 0; break; }
#define UN_I32(e) { uint32_t a = (uint32_t)sp[-1]; sp[-1] = R32(e); break; }
#define UN_I64(e) { uint64_t a = sp[-1]; sp[-1] = (uint64_t)(e); break; }
#define UN_F32(e) { float a = f32of(sp[-1]); sp[-1] = f32bits(e); break; }
#define UN_F64(e) { double a = f64of(sp[-1]); sp[-1] = f64bits(e); break; }
#define EA(n) uint64_t ea = (uint64_t)(uint32_t)sp[-1] + in->a; if (ea + (n) > msize) TRAP("out of bounds memory access")
#define LOAD(T, n, conv) { EA(n); T v; memcpy(&v, mem + ea, n); sp[-1] = (uint64_t)(conv); break; }
#define STORE(T, n) { T v = (T)sp[-1]; sp--; EA(n); memcpy(mem + ea, &v, n); sp--; break; }
#define RELOAD() do { mem = I->mem ? I->mem->data : NULL; msize = I->mem ? (uint64_t)I->mem->pages * W_PAGE : 0; } while (0)

/* Run f with its arguments at fp; its results end up there. 0, or -1 with an error thrown. */
static int exec(struct js *J, struct wrt *rt, struct wfn *f, uint64_t *fp)
{
    if (!f->inst)
        return host_call(J, rt, f, fp);
    int base = rt->nframes;
    const char *msg = NULL;
    struct winst *I;
    struct wi *code, *pc;
    uint64_t *sp, msize;
    uint8_t *mem;
    struct wfn *callee;

enter:
    {
        struct wcode *cd = f->code;
        if (!cd->code && translate(J, f->inst->m, cd) < 0)
            TRAP("internal error translating a function");
        if (fp + cd->nlocals + cd->maxstack > rt->end)
            TRAP("call stack exhausted");
        memset(fp + f->type->np, 0, (cd->nlocals - f->type->np) * sizeof(uint64_t));
        sp = fp + cd->nlocals;
        code = pc = cd->code;
        I = f->inst;
        RELOAD();
    }
    for (;;) {
        struct wi *in = pc++;
        switch (in->op) {
        case 0x00: TRAP("unreachable executed");
        case 0x03:
            if (++J->steps > J->step_limit)
                TRAP("the WebAssembly code ran too long and was stopped");
            break;
        case 0x04:
            if (!(uint32_t)*--sp)
                pc = code + in->a;
            break;
        case 0x05: pc = code + in->a; break;
        case 0x0D:
            if (!(uint32_t)*--sp)
                break;
            /* fall through */
        case 0x0C: {
            uint64_t *dst = fp + in->b;
            uint32_t n = in->x;
            if (n && dst != sp - n)
                memmove(dst, sp - n, n * sizeof(uint64_t));
            sp = dst + n;
            pc = code + in->a;
            break;
        }
        case 0x0E: {
            uint32_t i = (uint32_t)*--sp;
            if (i > in->b)
                i = in->b;
            struct wbr *t = &f->code->brs[in->a + i];
            uint64_t *dst = fp + t->height;
            if (t->arity && dst != sp - t->arity)
                memmove(dst, sp - t->arity, t->arity * sizeof(uint64_t));
            sp = dst + t->arity;
            pc = code + t->pc;
            break;
        }
        case 0x0F: {
            uint32_t n = f->type->nr;
            if (n && fp != sp - n)
                memmove(fp, sp - n, n * sizeof(uint64_t));
            sp = fp + n;
            if (rt->nframes == base)
                return 0;
            struct wframe *fr = &rt->frames[--rt->nframes];
            f = fr->f;
            pc = fr->pc;
            fp = fr->fp;
            code = f->code->code;
            I = f->inst;
            RELOAD();
            break;
        }
        case 0x10:
            callee = I->funcs[in->a];
            goto call;
        case 0x11: {
            struct wtable *t = I->tables[in->b];
            uint32_t i = (uint32_t)*--sp;
            if (i >= t->len)
                TRAP("undefined element: table index out of bounds");
            callee = (struct wfn *)(uintptr_t)t->el[i];
            if (!callee)
                TRAP("uninitialized element");
            if (!type_eq(callee->type, &I->m->types[in->a]))
                TRAP("indirect call signature mismatch");
            goto call;
        }
        case 0x1A: sp--; break;
        case 0x1B: {
            uint32_t c = (uint32_t)sp[-1];
            sp -= 2;
            if (!c)
                sp[-1] = sp[0];
            break;
        }
        case 0x20: *sp++ = fp[in->a]; break;
        case 0x21: fp[in->a] = *--sp; break;
        case 0x22: fp[in->a] = sp[-1]; break;
        case 0x23: *sp++ = I->globals[in->a]->v; break;
        case 0x24: I->globals[in->a]->v = *--sp; break;
        case 0x25: {
            struct wtable *t = I->tables[in->a];
            uint32_t i = (uint32_t)sp[-1];
            if (i >= t->len)
                TRAP("out of bounds table access");
            sp[-1] = t->el[i];
            break;
        }
        case 0x26: {
            struct wtable *t = I->tables[in->a];
            uint32_t i = (uint32_t)sp[-2];
            if (i >= t->len)
                TRAP("out of bounds table access");
            t->el[i] = sp[-1];
            sp -= 2;
            break;
        }
        case 0x28: LOAD(uint32_t, 4, v)
        case 0x29: LOAD(uint64_t, 8, v)
        case 0x2A: LOAD(uint32_t, 4, v)
        case 0x2B: LOAD(uint64_t, 8, v)
        case 0x2C: LOAD(int8_t, 1, (uint32_t)(int32_t)v)
        case 0x2D: LOAD(uint8_t, 1, v)
        case 0x2E: LOAD(int16_t, 2, (uint32_t)(int32_t)v)
        case 0x2F: LOAD(uint16_t, 2, v)
        case 0x30: LOAD(int8_t, 1, (int64_t)v)
        case 0x31: LOAD(uint8_t, 1, v)
        case 0x32: LOAD(int16_t, 2, (int64_t)v)
        case 0x33: LOAD(uint16_t, 2, v)
        case 0x34: LOAD(int32_t, 4, (int64_t)v)
        case 0x35: LOAD(uint32_t, 4, v)
        case 0x36: STORE(uint32_t, 4)
        case 0x37: STORE(uint64_t, 8)
        case 0x38: STORE(uint32_t, 4)
        case 0x39: STORE(uint64_t, 8)
        case 0x3A: STORE(uint8_t, 1)
        case 0x3B: STORE(uint16_t, 2)
        case 0x3C: STORE(uint8_t, 1)
        case 0x3D: STORE(uint16_t, 2)
        case 0x3E: STORE(uint32_t, 4)
        case 0x3F: *sp++ = I->mem->pages; break;
        case 0x40: {
            int64_t old = mem_grow(J, I->mem, (uint32_t)sp[-1]);
            sp[-1] = R32(old);
            RELOAD();
            break;
        }
        case 0x41: *sp++ = in->a; break;
        case 0x42: case 0x44: *sp++ = (uint64_t)in->b << 32 | in->a; break;
        case 0x43: *sp++ = in->a; break;

        case 0x45: UN_I32(a == 0)
        case 0x46: BIN_I32(a == b)
        case 0x47: BIN_I32(a != b)
        case 0x48: BIN_I32((int32_t)a < (int32_t)b)
        case 0x49: BIN_I32(a < b)
        case 0x4A: BIN_I32((int32_t)a > (int32_t)b)
        case 0x4B: BIN_I32(a > b)
        case 0x4C: BIN_I32((int32_t)a <= (int32_t)b)
        case 0x4D: BIN_I32(a <= b)
        case 0x4E: BIN_I32((int32_t)a >= (int32_t)b)
        case 0x4F: BIN_I32(a >= b)
        case 0x50: UN_I64(a == 0)
        case 0x51: BIN_I64(a == b)
        case 0x52: BIN_I64(a != b)
        case 0x53: BIN_I64((int64_t)a < (int64_t)b)
        case 0x54: BIN_I64(a < b)
        case 0x55: BIN_I64((int64_t)a > (int64_t)b)
        case 0x56: BIN_I64(a > b)
        case 0x57: BIN_I64((int64_t)a <= (int64_t)b)
        case 0x58: BIN_I64(a <= b)
        case 0x59: BIN_I64((int64_t)a >= (int64_t)b)
        case 0x5A: BIN_I64(a >= b)
        case 0x5B: CMP_F32(a == b)
        case 0x5C: CMP_F32(a != b)
        case 0x5D: CMP_F32(a < b)
        case 0x5E: CMP_F32(a > b)
        case 0x5F: CMP_F32(a <= b)
        case 0x60: CMP_F32(a >= b)
        case 0x61: CMP_F64(a == b)
        case 0x62: CMP_F64(a != b)
        case 0x63: CMP_F64(a < b)
        case 0x64: CMP_F64(a > b)
        case 0x65: CMP_F64(a <= b)
        case 0x66: CMP_F64(a >= b)

        case 0x67: UN_I32(a ? __builtin_clz(a) : 32)
        case 0x68: UN_I32(a ? __builtin_ctz(a) : 32)
        case 0x69: UN_I32(__builtin_popcount(a))
        case 0x6A: BIN_I32(a + b)
        case 0x6B: BIN_I32(a - b)
        case 0x6C: BIN_I32(a * b)
        case 0x6D: {
            int32_t b = S32(sp[-1]), a = S32(sp[-2]);
            if (!b)
                TRAP("integer divide by zero");
            if (a == INT32_MIN && b == -1)
                TRAP("integer overflow");
            sp--;
            sp[-1] = R32(a / b);
            break;
        }
        case 0x6E: {
            uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];
            if (!b)
                TRAP("integer divide by zero");
            sp--;
            sp[-1] = R32(a / b);
            break;
        }
        case 0x6F: {
            int32_t b = S32(sp[-1]), a = S32(sp[-2]);
            if (!b)
                TRAP("integer divide by zero");
            sp--;
            sp[-1] = R32(b == -1 ? 0 : a % b);
            break;
        }
        case 0x70: {
            uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];
            if (!b)
                TRAP("integer divide by zero");
            sp--;
            sp[-1] = R32(a % b);
            break;
        }
        case 0x71: BIN_I32(a & b)
        case 0x72: BIN_I32(a | b)
        case 0x73: BIN_I32(a ^ b)
        case 0x74: BIN_I32(a << (b & 31))
        case 0x75: BIN_I32((int32_t)a >> (b & 31))
        case 0x76: BIN_I32(a >> (b & 31))
        case 0x77: BIN_I32(rotl32(a, b))
        case 0x78: BIN_I32(rotr32(a, b))

        case 0x79: UN_I64(a ? __builtin_clzll(a) : 64)
        case 0x7A: UN_I64(a ? __builtin_ctzll(a) : 64)
        case 0x7B: UN_I64(__builtin_popcountll(a))
        case 0x7C: BIN_I64(a + b)
        case 0x7D: BIN_I64(a - b)
        case 0x7E: BIN_I64(a * b)
        case 0x7F: {
            int64_t b = (int64_t)sp[-1], a = (int64_t)sp[-2];
            if (!b)
                TRAP("integer divide by zero");
            if (a == INT64_MIN && b == -1)
                TRAP("integer overflow");
            sp--;
            sp[-1] = (uint64_t)(a / b);
            break;
        }
        case 0x80: {
            uint64_t b = sp[-1], a = sp[-2];
            if (!b)
                TRAP("integer divide by zero");
            sp--;
            sp[-1] = a / b;
            break;
        }
        case 0x81: {
            int64_t b = (int64_t)sp[-1], a = (int64_t)sp[-2];
            if (!b)
                TRAP("integer divide by zero");
            sp--;
            sp[-1] = (uint64_t)(b == -1 ? 0 : a % b);
            break;
        }
        case 0x82: {
            uint64_t b = sp[-1], a = sp[-2];
            if (!b)
                TRAP("integer divide by zero");
            sp--;
            sp[-1] = a % b;
            break;
        }
        case 0x83: BIN_I64(a & b)
        case 0x84: BIN_I64(a | b)
        case 0x85: BIN_I64(a ^ b)
        case 0x86: BIN_I64(a << (b & 63))
        case 0x87: BIN_I64((int64_t)a >> (b & 63))
        case 0x88: BIN_I64(a >> (b & 63))
        case 0x89: BIN_I64(rotl64(a, b))
        case 0x8A: BIN_I64(rotr64(a, b))

        case 0x8B: sp[-1] = (uint32_t)sp[-1] & 0x7FFFFFFFu; break;
        case 0x8C: sp[-1] = (uint32_t)sp[-1] ^ 0x80000000u; break;
        case 0x8D: UN_F32(ceilf(a))
        case 0x8E: UN_F32(floorf(a))
        case 0x8F: UN_F32(truncf(a))
        case 0x90: UN_F32(nearbyintf(a))
        case 0x91: UN_F32(sqrtf(a))
        case 0x92: BIN_F32(a + b)
        case 0x93: BIN_F32(a - b)
        case 0x94: BIN_F32(a * b)
        case 0x95: BIN_F32(a / b)
        case 0x96: BIN_F32(wminf(a, b))
        case 0x97: BIN_F32(wmaxf(a, b))
        case 0x98: {
            uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];
            sp--;
            sp[-1] = (a & 0x7FFFFFFFu) | (b & 0x80000000u);
            break;
        }
        case 0x99: sp[-1] &= 0x7FFFFFFFFFFFFFFFull; break;
        case 0x9A: sp[-1] ^= 0x8000000000000000ull; break;
        case 0x9B: UN_F64(ceil(a))
        case 0x9C: UN_F64(floor(a))
        case 0x9D: UN_F64(trunc(a))
        case 0x9E: UN_F64(nearbyint(a))
        case 0x9F: UN_F64(sqrt(a))
        case 0xA0: BIN_F64(a + b)
        case 0xA1: BIN_F64(a - b)
        case 0xA2: BIN_F64(a * b)
        case 0xA3: BIN_F64(a / b)
        case 0xA4: BIN_F64(wmin(a, b))
        case 0xA5: BIN_F64(wmax(a, b))
        case 0xA6: {
            uint64_t b = sp[-1], a = sp[-2];
            sp--;
            sp[-1] = (a & 0x7FFFFFFFFFFFFFFFull) | (b & 0x8000000000000000ull);
            break;
        }

        case 0xA7: sp[-1] = (uint32_t)sp[-1]; break;
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAE: case 0xAF: case 0xB0: case 0xB1: {
            int from32 = in->op == 0xA8 || in->op == 0xA9 || in->op == 0xAE || in->op == 0xAF;
            double x = from32 ? (double)f32of(sp[-1]) : f64of(sp[-1]);
            int k;
            switch (in->op) {
            case 0xA8: case 0xAA:
                if ((k = trunc_check(x, -2147483649.0, 2147483648.0)))
                    goto bad_trunc;
                sp[-1] = R32((int32_t)x);
                break;
            case 0xA9: case 0xAB:
                if ((k = trunc_check(x, -1.0, 4294967296.0)))
                    goto bad_trunc;
                sp[-1] = R32((uint32_t)x);
                break;
            case 0xAE: case 0xB0:
                if (x != x)
                    TRAP("invalid conversion to integer");
                if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0))
                    TRAP("integer overflow");
                sp[-1] = (uint64_t)(int64_t)x;
                break;
            default:
                if ((k = trunc_check(x, -1.0, 18446744073709551616.0)))
                    goto bad_trunc;
                sp[-1] = (uint64_t)x;
                break;
            }
            break;
        bad_trunc:
            TRAP(k == 1 ? "invalid conversion to integer" : "integer overflow");
        }
        case 0xAC: sp[-1] = (uint64_t)(int64_t)S32(sp[-1]); break;
        case 0xAD: sp[-1] = (uint32_t)sp[-1]; break;
        case 0xB2: sp[-1] = f32bits((float)S32(sp[-1])); break;
        case 0xB3: sp[-1] = f32bits((float)(uint32_t)sp[-1]); break;
        case 0xB4: sp[-1] = f32bits((float)(int64_t)sp[-1]); break;
        case 0xB5: sp[-1] = f32bits((float)sp[-1]); break;
        case 0xB6: sp[-1] = f32bits((float)f64of(sp[-1])); break;
        case 0xB7: sp[-1] = f64bits((double)S32(sp[-1])); break;
        case 0xB8: sp[-1] = f64bits((double)(uint32_t)sp[-1]); break;
        case 0xB9: sp[-1] = f64bits((double)(int64_t)sp[-1]); break;
        case 0xBA: sp[-1] = f64bits((double)sp[-1]); break;
        case 0xBB: sp[-1] = f64bits((double)f32of(sp[-1])); break;
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: break;       /* the bits stay */
        case 0xC0: sp[-1] = R32((int32_t)(int8_t)sp[-1]); break;
        case 0xC1: sp[-1] = R32((int32_t)(int16_t)sp[-1]); break;
        case 0xC2: sp[-1] = (uint64_t)(int64_t)(int8_t)sp[-1]; break;
        case 0xC3: sp[-1] = (uint64_t)(int64_t)(int16_t)sp[-1]; break;
        case 0xC4: sp[-1] = (uint64_t)(int64_t)(int32_t)sp[-1]; break;

        case 0xD0: *sp++ = 0; break;
        case 0xD1: sp[-1] = sp[-1] == 0; break;
        case 0xD2: *sp++ = (uintptr_t)I->funcs[in->a]; break;

        case FC(0): sp[-1] = R32(sat_s(f32of(sp[-1]), -2147483649.0, 2147483648.0, INT32_MIN, INT32_MAX)); break;
        case FC(1): sp[-1] = R32(sat_u(f32of(sp[-1]), 4294967296.0, UINT32_MAX)); break;
        case FC(2): sp[-1] = R32(sat_s(f64of(sp[-1]), -2147483649.0, 2147483648.0, INT32_MIN, INT32_MAX)); break;
        case FC(3): sp[-1] = R32(sat_u(f64of(sp[-1]), 4294967296.0, UINT32_MAX)); break;
        case FC(4): {
            double x = f32of(sp[-1]);
            sp[-1] = x != x ? 0 : x < -9223372036854775808.0 ? (uint64_t)INT64_MIN
                   : x >= 9223372036854775808.0 ? (uint64_t)INT64_MAX : (uint64_t)(int64_t)x;
            break;
        }
        case FC(5): sp[-1] = sat_u(f32of(sp[-1]), 18446744073709551616.0, UINT64_MAX); break;
        case FC(6): {
            double x = f64of(sp[-1]);
            sp[-1] = x != x ? 0 : x < -9223372036854775808.0 ? (uint64_t)INT64_MIN
                   : x >= 9223372036854775808.0 ? (uint64_t)INT64_MAX : (uint64_t)(int64_t)x;
            break;
        }
        case FC(7): sp[-1] = sat_u(f64of(sp[-1]), 18446744073709551616.0, UINT64_MAX); break;
        case FC(8): {
            uint64_t n = (uint32_t)sp[-1], s = (uint32_t)sp[-2], d = (uint32_t)sp[-3];
            sp -= 3;
            struct wdata *seg = &I->m->datas[in->a];
            uint64_t len = I->ddrop[in->a] ? 0 : seg->len;
            if (s + n > len || d + n > msize)
                TRAP("out of bounds memory access");
            memcpy(mem + d, seg->bytes + s, n);
            break;
        }
        case FC(9): I->ddrop[in->a] = 1; break;
        case FC(10): {
            uint64_t n = (uint32_t)sp[-1], s = (uint32_t)sp[-2], d = (uint32_t)sp[-3];
            sp -= 3;
            if (s + n > msize || d + n > msize)
                TRAP("out of bounds memory access");
            memmove(mem + d, mem + s, n);
            break;
        }
        case FC(11): {
            uint64_t n = (uint32_t)sp[-1], d = (uint32_t)sp[-3];
            uint8_t v = (uint8_t)sp[-2];
            sp -= 3;
            if (d + n > msize)
                TRAP("out of bounds memory access");
            memset(mem + d, v, n);
            break;
        }
        case FC(12): {
            uint64_t n = (uint32_t)sp[-1], s = (uint32_t)sp[-2], d = (uint32_t)sp[-3];
            sp -= 3;
            struct wseg *seg = &I->elems[in->a];
            struct wtable *t = I->tables[in->b];
            if (s + n > seg->n || d + n > t->len)
                TRAP("out of bounds table access");
            memcpy(t->el + d, seg->v + s, n * sizeof(uint64_t));
            break;
        }
        case FC(13): I->elems[in->a].n = 0; break;
        case FC(14): {
            uint64_t n = (uint32_t)sp[-1], s = (uint32_t)sp[-2], d = (uint32_t)sp[-3];
            sp -= 3;
            struct wtable *td = I->tables[in->a], *ts = I->tables[in->b];
            if (s + n > ts->len || d + n > td->len)
                TRAP("out of bounds table access");
            memmove(td->el + d, ts->el + s, n * sizeof(uint64_t));
            break;
        }
        case FC(15): {
            uint32_t n = (uint32_t)sp[-1];
            uint64_t v = sp[-2];
            sp--;
            sp[-1] = R32(table_grow(J, I->tables[in->a], n, v));
            break;
        }
        case FC(16): *sp++ = I->tables[in->a]->len; break;
        case FC(17): {
            uint64_t n = (uint32_t)sp[-1], v = sp[-2], i = (uint32_t)sp[-3];
            sp -= 3;
            struct wtable *t = I->tables[in->a];
            if (i + n > t->len)
                TRAP("out of bounds table access");
            for (uint64_t k = 0; k < n; k++)
                t->el[i + k] = v;
            break;
        }
        default:
            TRAP("internal error: bad instruction");
        }
        continue;

    call: {
        uint32_t np = callee->type->np, nr = callee->type->nr;
        uint64_t *nfp = sp - np;
        if (!callee->inst) {
            rt->top = nfp + (np > nr ? np : nr);
            if (rt->top < sp)
                rt->top = sp;
            if (host_call(J, rt, callee, nfp) < 0)
                goto fail;
            sp = nfp + nr;
            RELOAD();
            continue;
        }
        if (rt->nframes >= W_MAX_FRAMES)
            TRAP("call stack exhausted");
        rt->frames[rt->nframes++] = (struct wframe){f, pc, fp};
        f = callee;
        fp = nfp;
        goto enter;
    }
    }

trap:
    if (!J->throwing) {
        if (!strcmp(msg, "call stack exhausted"))
            js_throw_error(J, "RangeError: Maximum call stack size exceeded (WebAssembly)");
        else
            wthrow(J, E_RUNTIME, "%s", msg);
    }
fail:
    rt->nframes = base;
    return -1;
}

static int set_results(struct js *J, struct wtype *ty, jsval r, uint64_t *out)
{
    if (!ty->nr)
        return 0;
    if (ty->nr == 1)
        return to_wasm(J, TR(ty, 0), r, out);
    struct jsobj *list = js_new_array(J);
    struct iter it;
    jsval v;
    int rc;
    if (r.t != JS_OBJ || js_iter_open(J, r, &it) < 0) {
        if (!J->throwing)
            js_throw_error(J, "TypeError: a function returning several values must return an iterable");
        return -1;
    }
    while ((rc = js_iter_next(J, &it, &v)) > 0)
        js_array_push(J, list, v);
    if (rc < 0)
        return -1;
    if ((uint32_t)list->len != ty->nr) {
        js_throw_error(J, "TypeError: expected %u results, got %d", ty->nr, list->len);
        return -1;
    }
    for (uint32_t i = 0; i < ty->nr; i++)
        if (to_wasm(J, TR(ty, i), list->items[i], &out[i]) < 0)
            return -1;
    return 0;
}

static int host_call(struct js *J, struct wrt *rt, struct wfn *f, uint64_t *fp)
{
    struct wtype *ty = f->type;
    jsval buf[8], *argv = ty->np <= 8 ? buf : js_alloc(J, ty->np * sizeof(jsval));
    for (uint32_t i = 0; i < ty->np; i++)
        argv[i] = to_js(J, ty->t[i], fp[i]);
    if (js_stack_low(J)) {
        js_throw_error(J, "RangeError: Maximum call stack size exceeded");
        return -1;
    }
    jsval r = js_call_value(J, f->host, js_undefined, argv, (int)ty->np);
    if (J->throwing)
        return -1;
    return set_results(J, ty, r, fp);
}

static struct wrt *rt_ready(struct js *J)
{
    struct wrt *rt = rt_of(J);
    if (!rt->stack) {
        rt->stack = js_alloc(J, W_STACK_SLOTS * sizeof(uint64_t));
        rt->end = rt->stack + W_STACK_SLOTS;
        rt->top = rt->stack;
        rt->frames = js_alloc(J, W_MAX_FRAMES * sizeof(struct wframe));
    }
    return rt;
}

/* Call f with JavaScript arguments; its results as JavaScript (an array if several). */
static jsval invoke(struct js *J, struct wfn *f, jsval *args, int argc)
{
    struct wrt *rt = rt_ready(J);
    struct wtype *ty = f->type;
    uint32_t n = ty->np > ty->nr ? ty->np : ty->nr;
    if (js_stack_low(J) || rt->top + n + 1 > rt->end)
        return js_throw_error(J, "RangeError: Maximum call stack size exceeded");
    uint64_t *base = rt->top;
    rt->top = base + n;
    jsval r = js_undefined;
    int ok = 1;
    for (uint32_t i = 0; i < ty->np && ok; i++)
        ok = to_wasm(J, ty->t[i], ARG((int)i), &base[i]) == 0;
    if (ok && exec(J, rt, f, base) == 0) {
        if (ty->nr == 1)
            r = to_js(J, TR(ty, 0), base[0]);
        else if (ty->nr > 1) {
            struct jsobj *a = js_new_array(J);
            for (uint32_t i = 0; i < ty->nr; i++)
                js_array_push(J, a, to_js(J, TR(ty, i), base[i]));
            r = js_obj(a);
        }
    }
    rt->top = base;
    return J->throwing ? js_undefined : r;
}

static jsval call_export(struct js *J, jsval self, jsval *args, int argc)
{
    struct wfn *f = self.t == JS_OBJ ? js_host_data(self.u.o, &fn_class) : NULL;
    if (!f)
        return js_throw_error(J, "TypeError: not a WebAssembly function");
    if (J->constructing)
        return js_throw_error(J, "TypeError: WebAssembly functions are not constructors");
    return invoke(J, f, args, argc);
}

/* ---- instantiation ---- */

static uint64_t rd_uleb(const uint8_t **p)
{
    uint64_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        if (shift < 64)
            v |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    return v;
}
static int64_t rd_sleb(const uint8_t **p)
{
    uint64_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        if (shift < 64)
            v |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        v |= ~(uint64_t)0 << shift;
    return (int64_t)v;
}

/* A validated constant expression's value. */
static uint64_t const_value(struct winst *I, const uint8_t *p)
{
    uint64_t st[64];
    int n = 0;
    for (;;) {
        uint8_t op = *p++;
        switch (op) {
        case 0x41: st[n++] = R32((int32_t)rd_sleb(&p)); break;
        case 0x42: st[n++] = (uint64_t)rd_sleb(&p); break;
        case 0x43: { uint32_t v; memcpy(&v, p, 4); p += 4; st[n++] = v; break; }
        case 0x44: { uint64_t v; memcpy(&v, p, 8); p += 8; st[n++] = v; break; }
        case 0x23: st[n++] = I->globals[rd_uleb(&p)]->v; break;
        case 0xD0: p++; st[n++] = 0; break;
        case 0xD2: st[n++] = (uintptr_t)I->funcs[rd_uleb(&p)]; break;
        case 0x6A: n--; st[n - 1] = R32(st[n - 1] + st[n]); break;
        case 0x6B: n--; st[n - 1] = R32(st[n - 1] - st[n]); break;
        case 0x6C: n--; st[n - 1] = R32((uint32_t)st[n - 1] * (uint32_t)st[n]); break;
        case 0x7C: n--; st[n - 1] += st[n]; break;
        case 0x7D: n--; st[n - 1] -= st[n]; break;
        case 0x7E: n--; st[n - 1] *= st[n]; break;
        default: return st[0];
        }
    }
}

static const char *kind_name(int k)
{
    return k == K_FUNC ? "function" : k == K_TABLE ? "table" : k == K_MEM ? "memory" : "global";
}
static const char *type_name(uint8_t t)
{
    switch (t) {
    case I32: return "i32";
    case I64: return "i64";
    case F32: return "f32";
    case F64: return "f64";
    case FUNCREF: return "funcref";
    default: return "externref";
    }
}
static int type_named(const char *s)
{
    if (!strcmp(s, "i32")) return I32;
    if (!strcmp(s, "i64")) return I64;
    if (!strcmp(s, "f32")) return F32;
    if (!strcmp(s, "f64")) return F64;
    if (!strcmp(s, "funcref") || !strcmp(s, "anyfunc")) return FUNCREF;
    if (!strcmp(s, "externref")) return EXTERNREF;
    return 0;
}

static int limits_fit(uint32_t len, uint32_t max, int has_max, const struct wlimits *want)
{
    if (len < want->min)
        return 0;
    if (want->has_max && (!has_max || max > want->max))
        return 0;
    return 1;
}

static struct wmod *mod_of(struct js *J, jsval v)
{
    struct wmod *m = v.t == JS_OBJ ? js_host_data(v.u.o, &mod_class) : NULL;
    if (!m)
        js_throw_error(J, "TypeError: a WebAssembly.Module is required");
    return m;
}

/* %wasmInstantiate(module, values): values are the imports, resolved by the caller,
 * in order: functions as they are, memories, tables and globals as handles (or a
 * plain value for a global). Returns [[name, kind, value], ...]. */
static jsval w_instantiate(struct js *J, jsval self, jsval *args, int argc)
{
    struct wmod *m = mod_of(J, ARG(0));
    if (!m)
        return js_undefined;
    jsval vals = ARG(1);
    struct winst *I = js_alloc(J, sizeof(*I));
    I->m = m;
    I->funcs = js_alloc(J, (m->nfuncs + 1) * sizeof(struct wfn *));
    I->tables = js_alloc(J, (m->ntables + 1) * sizeof(struct wtable *));
    I->globals = js_alloc(J, (m->nglobals + 1) * sizeof(struct wglobal *));
    I->elems = js_alloc(J, (m->nelems + 1) * sizeof(struct wseg));
    I->ddrop = js_alloc_atomic(J, m->ndatas + 1);
    uint32_t nf = 0, nt = 0, ng = 0;
    for (uint32_t i = 0; i < m->nimports; i++) {
        struct wimport *im = &m->imports[i];
        jsval v = js_getv(J, vals, js_num(i));
        if (J->throwing)
            return js_undefined;
        const char *mod = im->mod->s, *name = im->name->s;
        switch (im->kind) {
        case K_FUNC: {
            struct wtype *ty = &m->types[im->type];
            if (!js_is_callable(v))
                return wthrow(J, E_LINK, "import %s.%s: a function is required", mod, name);
            struct wfn *f = fn_of(v);
            if (f) {
                if (!type_eq(f->type, ty))
                    return wthrow(J, E_LINK, "import %s.%s: the function's signature does not match", mod, name);
            } else {
                f = js_alloc(J, sizeof(*f));
                f->type = ty;
                f->host = v;
                f->idx = nf;
            }
            I->funcs[nf++] = f;
            break;
        }
        case K_TABLE: {
            struct wtable *t = v.t == JS_OBJ ? js_host_data(v.u.o, &table_class) : NULL;
            if (!t)
                return wthrow(J, E_LINK, "import %s.%s: a WebAssembly.Table is required", mod, name);
            if (t->type != im->vtype || !limits_fit(t->len, t->decl_max, t->has_max, &im->lim))
                return wthrow(J, E_LINK, "import %s.%s: the table does not match", mod, name);
            I->tables[nt++] = t;
            break;
        }
        case K_MEM: {
            struct wmem *mm = v.t == JS_OBJ ? js_host_data(v.u.o, &mem_class) : NULL;
            if (!mm)
                return wthrow(J, E_LINK, "import %s.%s: a WebAssembly.Memory is required", mod, name);
            if (!limits_fit(mm->pages, mm->decl_max, mm->has_max, &im->lim))
                return wthrow(J, E_LINK, "import %s.%s: the memory does not match", mod, name);
            I->mem = mm;
            break;
        }
        default: {
            struct wglobal *g = v.t == JS_OBJ ? js_host_data(v.u.o, &global_class) : NULL;
            if (g) {
                if (g->type != im->vtype || g->mut != im->mut)
                    return wthrow(J, E_LINK, "import %s.%s: the global's type does not match", mod, name);
            } else {
                if (im->mut)
                    return wthrow(J, E_LINK, "import %s.%s: a mutable global needs a WebAssembly.Global", mod, name);
                if ((im->vtype == I64 && v.t != JS_BIG) || (is_num(im->vtype) && im->vtype != I64 && v.t != JS_NUM))
                    return wthrow(J, E_LINK, "import %s.%s: a %s value is required", mod, name, type_name(im->vtype));
                g = js_alloc(J, sizeof(*g));
                g->type = im->vtype;
                if (to_wasm(J, g->type, v, &g->v) < 0)
                    return js_undefined;
            }
            I->globals[ng++] = g;
            break;
        }
        }
    }
    for (uint32_t i = m->nfimports; i < m->nfuncs; i++) {
        struct wfn *f = js_alloc(J, sizeof(*f));
        f->type = &m->types[m->ftype[i]];
        f->inst = I;
        f->code = &m->codes[i - m->nfimports];
        f->idx = i;
        I->funcs[i] = f;
    }
    for (uint32_t i = m->ntimports; i < m->ntables; i++) {
        struct wtabdef *d = &m->tables[i];
        if (!(I->tables[i] = table_new(J, d->type, d->lim.min, d->lim.max, d->lim.has_max, 0)))
            return js_undefined;
    }
    if (m->nmems > m->nmimports && !(I->mem = mem_new(J, m->mems[0].min, m->mems[0].max, m->mems[0].has_max)))
        return js_undefined;
    for (uint32_t i = m->ngimports; i < m->nglobals; i++) {
        struct wglobal *g = js_alloc(J, sizeof(*g));
        g->type = m->globals[i].type;
        g->mut = m->globals[i].mut;
        g->v = const_value(I, m->globals[i].init);
        I->globals[i] = g;
    }
    for (uint32_t i = 0; i < m->nelems; i++) {
        struct welem *e = &m->elems[i];
        struct wseg *s = &I->elems[i];
        s->n = e->n;
        s->v = js_alloc(J, (e->n + 1) * sizeof(uint64_t));
        for (uint32_t k = 0; k < e->n; k++)
            s->v[k] = e->funcs ? (uintptr_t)I->funcs[e->funcs[k]] : const_value(I, e->exprs[k]);
    }
    struct jsobj *out = js_new_array(J);
    for (uint32_t i = 0; i < m->nexports; i++) {
        struct wexport *e = &m->exports[i];
        jsval v;
        switch (e->kind) {
        case K_FUNC: v = fn_value(J, I->funcs[e->idx]); break;
        case K_TABLE: v = handle(J, &I->tables[e->idx]->handle, &table_class, I->tables[e->idx]); break;
        case K_MEM: v = handle(J, &I->mem->handle, &mem_class, I->mem); break;
        default: v = handle(J, &I->globals[e->idx]->handle, &global_class, I->globals[e->idx]); break;
        }
        struct jsobj *entry = js_new_array(J);
        js_array_push(J, entry, js_keyval(e->name));
        js_array_push(J, entry, js_str(J, kind_name(e->kind)));
        js_array_push(J, entry, v);
        js_array_push(J, out, js_obj(entry));
    }
    /* Segments, then the start function: a trap leaves what was written before it. */
    for (uint32_t i = 0; i < m->nelems; i++) {
        struct welem *e = &m->elems[i];
        if (e->mode == 0) {
            uint64_t d = (uint32_t)const_value(I, e->offset);
            struct wtable *t = I->tables[e->table];
            if (d + e->n > t->len)
                return wthrow(J, E_RUNTIME, "out of bounds table access");
            memcpy(t->el + d, I->elems[i].v, e->n * sizeof(uint64_t));
        }
        if (e->mode != 1)
            I->elems[i].n = 0;
    }
    for (uint32_t i = 0; i < m->ndatas; i++) {
        struct wdata *d = &m->datas[i];
        if (d->mode == 0) {
            uint64_t at = (uint32_t)const_value(I, d->offset);
            if (at + d->len > (uint64_t)I->mem->pages * W_PAGE)
                return wthrow(J, E_RUNTIME, "out of bounds memory access");
            memcpy(I->mem->data + at, d->bytes, d->len);
            I->ddrop[i] = 1;
        }
    }
    if (m->start >= 0) {
        invoke(J, I->funcs[m->start], NULL, 0);
        if (J->throwing)
            return js_undefined;
    }
    return js_obj(out);
}

/* ---- the primitives ---- */

static jsval w_setup(struct js *J, jsval self, jsval *args, int argc)
{
    struct wrt *rt = rt_of(J);
    for (int i = 0; i < 3; i++)
        rt->err[i] = ARG(i);
    return js_undefined;
}

static jsval w_compile(struct js *J, jsval self, jsval *args, int argc)
{
    struct wmod *m = compile(J, ARG(0), 0);
    return m ? js_obj(js_new_host(J, &mod_class, m)) : js_undefined;
}

static jsval w_validate(struct js *J, jsval self, jsval *args, int argc)
{
    int len = -1;
    if (!js_binary_bytes(ARG(0), &len) && len != 0)
        return js_throw_error(J, "TypeError: WebAssembly.validate needs an ArrayBuffer or a typed array of bytes");
    return js_bool(compile(J, ARG(0), 1) != NULL);
}

static jsval descriptor(struct js *J, struct jsstr *mod, struct jsstr *name, int kind)
{
    struct jsobj *o = js_new_object(J);
    if (mod)
        js_set(J, js_obj(o), "module", js_keyval(mod));
    js_set(J, js_obj(o), "name", js_keyval(name));
    js_set(J, js_obj(o), "kind", js_str(J, kind_name(kind)));
    return js_obj(o);
}

static jsval w_imports(struct js *J, jsval self, jsval *args, int argc)
{
    struct wmod *m = mod_of(J, ARG(0));
    if (!m)
        return js_undefined;
    struct jsobj *a = js_new_array(J);
    for (uint32_t i = 0; i < m->nimports; i++)
        js_array_push(J, a, descriptor(J, m->imports[i].mod, m->imports[i].name, m->imports[i].kind));
    return js_obj(a);
}

static jsval w_exports(struct js *J, jsval self, jsval *args, int argc)
{
    struct wmod *m = mod_of(J, ARG(0));
    if (!m)
        return js_undefined;
    struct jsobj *a = js_new_array(J);
    for (uint32_t i = 0; i < m->nexports; i++)
        js_array_push(J, a, descriptor(J, NULL, m->exports[i].name, m->exports[i].kind));
    return js_obj(a);
}

static jsval w_custom(struct js *J, jsval self, jsval *args, int argc)
{
    struct wmod *m = mod_of(J, ARG(0));
    if (!m)
        return js_undefined;
    struct jsstr *name = js_tostr(J, ARG(1));
    struct jsobj *a = js_new_array(J);
    for (uint32_t i = 0; i < m->ncustoms; i++) {
        struct wcustom *c = &m->customs[i];
        if (c->name->len == name->len && !memcmp(c->name->s, name->s, (size_t)name->len))
            js_array_push(J, a, js_get(J, js_new_bytes(J, c->p, (int)c->len), "buffer"));
    }
    return js_obj(a);
}

static int u32_arg(struct js *J, jsval v, uint32_t max, const char *what, uint32_t *out)
{
    double d = js_to_number(J, v);
    if (J->throwing)
        return -1;
    if (!isfinite(d) || trunc(d) < 0 || trunc(d) > 4294967295.0) {
        js_throw_error(J, "TypeError: %s must be a number from 0 to 4294967295", what);
        return -1;
    }
    if (trunc(d) > max) {
        js_throw_error(J, "RangeError: %s must be at most %u", what, max);
        return -1;
    }
    *out = (uint32_t)trunc(d);
    return 0;
}

static jsval w_memory(struct js *J, jsval self, jsval *args, int argc)
{
    uint32_t min, max = 0;
    int has_max = ARG(1).t != JS_UNDEF;
    if (u32_arg(J, ARG(0), W_SPEC_PAGES, "initial", &min) < 0 ||
        (has_max && u32_arg(J, ARG(1), W_SPEC_PAGES, "maximum", &max) < 0))
        return js_undefined;
    if (has_max && max < min)
        return js_throw_error(J, "RangeError: the maximum must not be smaller than the initial size");
    struct wmem *m = mem_new(J, min, max, has_max);
    return m ? handle(J, &m->handle, &mem_class, m) : js_undefined;
}

static struct wmem *mem_arg(struct js *J, jsval v)
{
    struct wmem *m = v.t == JS_OBJ ? js_host_data(v.u.o, &mem_class) : NULL;
    if (!m)
        js_throw_error(J, "TypeError: a WebAssembly.Memory is required");
    return m;
}

static jsval w_mem_buffer(struct js *J, jsval self, jsval *args, int argc)
{
    struct wmem *m = mem_arg(J, ARG(0));
    if (!m)
        return js_undefined;
    if (!m->buffer)
        m->buffer = js_buffer_over(J, m->data, (int)((size_t)m->pages * W_PAGE)).u.o;
    return js_obj(m->buffer);
}

static jsval w_mem_grow(struct js *J, jsval self, jsval *args, int argc)
{
    struct wmem *m = mem_arg(J, ARG(0));
    uint32_t delta;
    if (!m || u32_arg(J, ARG(1), W_SPEC_PAGES, "delta", &delta) < 0)
        return js_undefined;
    int64_t old = mem_grow(J, m, delta);
    if (old < 0)
        return js_throw_error(J, "RangeError: WebAssembly.Memory.grow(): could not grow the memory");
    return js_num((double)old);
}

static uint64_t default_ref(struct js *J, uint8_t type)
{
    uint64_t v = 0;
    if (type == EXTERNREF)
        to_wasm(J, type, js_undefined, &v);
    return v;
}

/* %wasmTable(type, initial, maximum, value, hasValue) */
static jsval w_table(struct js *J, jsval self, jsval *args, int argc)
{
    int type = type_named(js_tostr(J, ARG(0))->s);
    if (!is_ref((uint8_t)type))
        return js_throw_error(J, "TypeError: a table's elements must be \"funcref\" or \"externref\"");
    uint32_t min, max = 0;
    int has_max = ARG(2).t != JS_UNDEF;
    if (u32_arg(J, ARG(1), W_IMPL_TABLE, "initial", &min) < 0 ||
        (has_max && u32_arg(J, ARG(2), 0xFFFFFFFFu, "maximum", &max) < 0))
        return js_undefined;
    if (has_max && max < min)
        return js_throw_error(J, "RangeError: the maximum must not be smaller than the initial size");
    uint64_t init;
    if (!js_truthy(ARG(4)) || ARG(3).t == JS_UNDEF)
        init = default_ref(J, (uint8_t)type);
    else if (to_wasm(J, (uint8_t)type, ARG(3), &init) < 0)
        return js_undefined;
    struct wtable *t = table_new(J, (uint8_t)type, min, max, has_max, init);
    return t ? handle(J, &t->handle, &table_class, t) : js_undefined;
}

/* %wasmTableOp(table, op, index, value, hasValue): 0 length, 1 get, 2 set, 3 grow */
static jsval w_table_op(struct js *J, jsval self, jsval *args, int argc)
{
    struct wtable *t = ARG(0).t == JS_OBJ ? js_host_data(ARG(0).u.o, &table_class) : NULL;
    if (!t)
        return js_throw_error(J, "TypeError: a WebAssembly.Table is required");
    int op = (int)js_to_number(J, ARG(1));
    if (op == 0)
        return js_num(t->len);
    uint32_t i;
    if (u32_arg(J, ARG(2), 0xFFFFFFFFu, op == 3 ? "delta" : "index", &i) < 0)
        return js_undefined;
    if (op == 1) {
        if (i >= t->len)
            return js_throw_error(J, "RangeError: table index %u out of bounds", i);
        return to_js(J, t->type, t->el[i]);
    }
    uint64_t v;
    if (!js_truthy(ARG(4)) || ARG(3).t == JS_UNDEF)
        v = default_ref(J, t->type);
    else if (to_wasm(J, t->type, ARG(3), &v) < 0)
        return js_undefined;
    if (op == 2) {
        if (i >= t->len)
            return js_throw_error(J, "RangeError: table index %u out of bounds", i);
        t->el[i] = v;
        return js_undefined;
    }
    int64_t old = table_grow(J, t, i, v);
    if (old < 0)
        return js_throw_error(J, "RangeError: WebAssembly.Table.grow(): could not grow the table");
    return js_num((double)old);
}

/* %wasmGlobal(type, mutable, value) */
static jsval w_global(struct js *J, jsval self, jsval *args, int argc)
{
    int type = type_named(js_tostr(J, ARG(0))->s);
    if (!type)
        return js_throw_error(J, "TypeError: unknown WebAssembly value type");
    struct wglobal *g = js_alloc(J, sizeof(*g));
    g->type = (uint8_t)type;
    g->mut = (uint8_t)js_truthy(ARG(1));
    if (ARG(2).t == JS_UNDEF)
        g->v = default_ref(J, g->type);
    else if (to_wasm(J, g->type, ARG(2), &g->v) < 0)
        return js_undefined;
    return handle(J, &g->handle, &global_class, g);
}

/* %wasmGlobalOp(global, op, value): 0 get, 1 set, 2 its type */
static jsval w_global_op(struct js *J, jsval self, jsval *args, int argc)
{
    struct wglobal *g = ARG(0).t == JS_OBJ ? js_host_data(ARG(0).u.o, &global_class) : NULL;
    if (!g)
        return js_throw_error(J, "TypeError: a WebAssembly.Global is required");
    int op = (int)js_to_number(J, ARG(1));
    if (op == 0)
        return to_js(J, g->type, g->v);
    if (op == 2)
        return js_str(J, type_name(g->type));
    if (!g->mut)
        return js_throw_error(J, "TypeError: the global is immutable");
    uint64_t v;
    if (to_wasm(J, g->type, ARG(2), &v) < 0)
        return js_undefined;
    g->v = v;
    return js_undefined;
}

/* What a handle is: "module", "memory", "table", "global", or null. */
static jsval w_kind(struct js *J, jsval self, jsval *args, int argc)
{
    jsval v = ARG(0);
    if (v.t != JS_OBJ)
        return js_null;
    if (js_host_data(v.u.o, &mod_class)) return js_str(J, "module");
    if (js_host_data(v.u.o, &mem_class)) return js_str(J, "memory");
    if (js_host_data(v.u.o, &table_class)) return js_str(J, "table");
    if (js_host_data(v.u.o, &global_class)) return js_str(J, "global");
    return js_null;
}

void js_init_wasm(struct js *J)
{
    static const struct { const char *name; js_native fn; int n; } fns[] = {
        {"%wasmSetup", w_setup, 3},        {"%wasmCompile", w_compile, 1},     {"%wasmValidate", w_validate, 1},
        {"%wasmImports", w_imports, 1},    {"%wasmExports", w_exports, 1},     {"%wasmCustom", w_custom, 2},
        {"%wasmInstantiate", w_instantiate, 2}, {"%wasmMemory", w_memory, 2}, {"%wasmMemBuffer", w_mem_buffer, 1},
        {"%wasmMemGrow", w_mem_grow, 2},   {"%wasmTable", w_table, 5},         {"%wasmTableOp", w_table_op, 5},
        {"%wasmGlobal", w_global, 3},      {"%wasmGlobalOp", w_global_op, 3},  {"%wasmKind", w_kind, 1},
    };
    for (unsigned i = 0; i < sizeof(fns) / sizeof(fns[0]); i++)
        js_define_native(J, J->global, fns[i].name, fns[i].fn, fns[i].n);
}
