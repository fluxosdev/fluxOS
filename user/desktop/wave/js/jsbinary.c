/* ArrayBuffer, typed array views and DataView. Buffers own bytes; views share them. */
#include <math.h>
#include <flux/digest.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inflate.h>
#include "jsint.h"
#define ARG(i) ((i) < argc ? args[(i)] : js_undefined)
#define BUFFER_MAX (64u << 20)
struct buffer {
    unsigned char *data;
    int len;
};
struct view {
    struct jsobj *buffer;
    int offset, len, bytes, kind;
};
static const struct jsclass buffer_class, typed_class, view_class;
static const int sizes[] = {1, 1, 1, 2, 2, 4, 4, 4, 8};
static const char *names[] = {"Int8Array",  "Uint8Array",  "Uint8ClampedArray", "Int16Array",  "Uint16Array",
                              "Int32Array", "Uint32Array", "Float32Array",      "Float64Array"};
static jsval prop(struct js *J, struct jsobj *o, const char *s, jsval v)
{
    js_put(J, o, js_mkstr(J, s, (int)strlen(s)), v, 1);
    return v;
}
static struct jsobj *ctor(struct js *J, const char *s, js_native f, struct jsobj *p)
{
    jsval c = js_new_function(J, f, s, 1);
    c.u.o->ctor = 1;
    prop(J, c.u.o, "prototype", js_obj(p));
    prop(J, p, "constructor", c);
    prop(J, J->global, s, c);
    return c.u.o;
}
static struct buffer *buffer_of(jsval v)
{
    return v.t == JS_OBJ ? js_host_data(v.u.o, &buffer_class) : NULL;
}
static struct view *typed_of(jsval v)
{
    return v.t == JS_OBJ ? js_host_data(v.u.o, &typed_class) : NULL;
}
int js_typed_length(jsval v)
{
    struct view *a = typed_of(v);
    return a ? a->len : -1;
}
unsigned char *js_binary_bytes(jsval v, int *len)
{
    struct buffer *b = buffer_of(v);
    if (b) {
        *len = b->len;
        return b->data;
    }
    struct view *a = typed_of(v);
    if (!a && v.t == JS_OBJ)
        a = js_host_data(v.u.o, &view_class);
    if (!a)
        return NULL;
    b = buffer_of(js_obj(a->buffer));
    *len = a->len * a->bytes;
    return b->data + a->offset;
}
static int index_arg(struct js *J, jsval v, int def, int max)
{
    if (v.t == JS_UNDEF)
        return def;
    double n = js_to_number(J, v);
    if (isnan(n))
        n = 0;
    n = trunc(n);
    if (n < 0 || n > max || isinf(n)) {
        js_throw_error(J, "RangeError: invalid buffer index or length");
        return -1;
    }
    return (int)n;
}
static jsval new_buffer(struct js *J, int n)
{
    struct buffer *b = js_alloc(J, sizeof(*b));
    b->data = js_alloc_atomic(J, (size_t)(n ? n : 1));
    b->len = n;
    struct jsobj *o = js_new_host(J, &buffer_class, b);
    o->proto = J->buffer_proto;
    return js_obj(o);
}
static jsval buffer_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    if (!J->constructing)
        return js_throw_error(J, "TypeError: ArrayBuffer requires new");
    int n = index_arg(J, ARG(0), 0, BUFFER_MAX);
    return n < 0 ? js_undefined : new_buffer(J, n);
}
static int buffer_get(struct js *J, struct jsobj *o, const char *k, jsval *out)
{
    struct buffer *b = o->hdata;
    if (strcmp(k, "byteLength"))
        return 0;
    *out = js_num(b->len);
    return 1;
}
static int buffer_set(struct js *J, struct jsobj *o, const char *k, jsval v)
{
    return !strcmp(k, "byteLength");
}
static const struct jsclass buffer_class = {"ArrayBuffer", buffer_get, buffer_set, NULL};
static int relative(struct js *J, jsval v, int def, int len)
{
    if (v.t == JS_UNDEF)
        return def;
    double d = js_to_number(J, v);
    if (isnan(d))
        d = 0;
    d = trunc(d);
    if (d < 0)
        d += len;
    return d < 0 ? 0 : d > len ? len : (int)d;
}
static jsval buffer_slice(struct js *J, jsval self, jsval *args, int argc)
{
    struct buffer *b = buffer_of(self);
    if (!b)
        return js_throw_error(J, "TypeError: not an ArrayBuffer");
    int a = relative(J, ARG(0), 0, b->len), z = relative(J, ARG(1), b->len, b->len);
    if (z < a)
        z = a;
    jsval r = new_buffer(J, z - a);
    memcpy(buffer_of(r)->data, b->data + a, (size_t)(z - a));
    return r;
}
static jsval is_view(struct js *J, jsval self, jsval *args, int argc)
{
    return js_bool(typed_of(ARG(0)) || (ARG(0).t == JS_OBJ && js_host_data(ARG(0).u.o, &view_class)));
}
static double read_number(const unsigned char *p, int kind, int little)
{
    int bytes = sizes[kind];
    uint64_t bits = 0;
    for (int i = 0; i < bytes; i++)
        bits |= (uint64_t)p[i] << (8 * (little ? i : bytes - 1 - i));
    switch (kind) {
    case 0:
        return (int8_t)bits;
    case 1:
    case 2:
        return (uint8_t)bits;
    case 3:
        return (int16_t)bits;
    case 4:
        return (uint16_t)bits;
    case 5:
        return (int32_t)bits;
    case 6:
        return (uint32_t)bits;
    case 7: {
        uint32_t u = (uint32_t)bits;
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    default: {
        double d;
        memcpy(&d, &bits, 8);
        return d;
    }
    }
}
static void write_number(unsigned char *p, int kind, int little, double d)
{
    int bytes = sizes[kind];
    uint64_t bits;
    if (kind == 7) {
        float f = (float)d;
        uint32_t u;
        memcpy(&u, &f, 4);
        bits = u;
    } else if (kind == 8)
        memcpy(&bits, &d, 8);
    else if (kind == 2) {
        if (isnan(d) || d <= 0)
            d = 0;
        else if (d >= 255)
            d = 255;
        else {
            double lo = floor(d), frac = d - lo;
            d = frac > 0.5 ? lo + 1 : frac < 0.5 ? lo : ((int)lo & 1) ? lo + 1 : lo;
        }
        bits = (uint32_t)d;
    } else {
        if (isnan(d) || isinf(d))
            d = 0;
        double modulus = bytes == 4 ? 4294967296.0 : bytes == 2 ? 65536.0 : 256.0;
        d = fmod(trunc(d), modulus);
        if (d < 0)
            d += modulus;
        bits = (uint32_t)d;
    }
    for (int i = 0; i < bytes; i++)
        p[i] = (unsigned char)(bits >> (8 * (little ? i : bytes - 1 - i)));
}
/* Canonical numeric keys outside the view are still handled, so -1/NaN cannot become expandos. */
static int numeric_key(struct js *J, const char *k, double *n)
{
    if (!strcmp(k, "-0")) {
        *n = -1;
        return 1;
    }
    *n = js_strtonum(k, (int)strlen(k));
    char b[40];
    js_numtostr(*n, b);
    return !strcmp(b, k);
}
int js_typed_has(jsval value, const char *key)
{
    struct view *a = typed_of(value);
    double n;
    if (!a || !numeric_key(NULL, key, &n))
        return -1;
    return n >= 0 && n < a->len && n == floor(n);
}

static int typed_get(struct js *J, struct jsobj *o, const char *k, jsval *out)
{
    struct view *a = o->hdata;
    if (!strcmp(k, "length")) {
        *out = js_num(a->len);
        return 1;
    }
    if (!strcmp(k, "byteLength")) {
        *out = js_num(a->len * a->bytes);
        return 1;
    }
    if (!strcmp(k, "byteOffset")) {
        *out = js_num(a->offset);
        return 1;
    }
    if (!strcmp(k, "buffer")) {
        *out = js_obj(a->buffer);
        return 1;
    }
    double n;
    if (!numeric_key(J, k, &n))
        return 0;
    *out = js_undefined;
    if (n >= 0 && n < a->len && n == floor(n)) {
        struct buffer *b = buffer_of(js_obj(a->buffer));
        *out = js_num(read_number(b->data + a->offset + (int)n * a->bytes, a->kind, 1));
    }
    return 1;
}
static int typed_set_prop(struct js *J, struct jsobj *o, const char *k, jsval val)
{
    struct view *a = o->hdata;
    if (!strcmp(k, "length") || !strcmp(k, "byteLength") || !strcmp(k, "byteOffset") || !strcmp(k, "buffer"))
        return 1;
    double n;
    if (!numeric_key(J, k, &n))
        return 0;
    double d = js_to_number(J, val);
    if (n >= 0 && n < a->len && n == floor(n) && !J->throwing) {
        struct buffer *b = buffer_of(js_obj(a->buffer));
        write_number(b->data + a->offset + (int)n * a->bytes, a->kind, 1, d);
    }
    return 1;
}
static const struct jsclass typed_class = {"TypedArray", typed_get, typed_set_prop, NULL};
static jsval new_typed(struct js *J, jsval buf, int offset, int len, int kind)
{
    struct view *a = js_alloc(J, sizeof(*a));
    *a = (struct view){buf.u.o, offset, len, sizes[kind], kind};
    struct jsobj *o = js_new_host(J, &typed_class, a);
    o->proto = J->typed_protos[kind];
    return js_obj(o);
}
static jsval typed_make(struct js *J, jsval *args, int argc, int kind)
{
    if (!J->constructing)
        return js_throw_error(J, "TypeError: typed array requires new");
    jsval src = ARG(0);
    struct buffer *b = buffer_of(src);
    int bytes = sizes[kind], n;
    if (b) {
        int off = index_arg(J, ARG(1), 0, b->len);
        if (off < 0)
            return js_undefined;
        if (off % bytes)
            return js_throw_error(J, "RangeError: unaligned typed array offset");
        if (ARG(2).t == JS_UNDEF && (b->len - off) % bytes)
            return js_throw_error(J, "RangeError: unaligned buffer length");
        n = index_arg(J, ARG(2), (b->len - off) / bytes, (b->len - off) / bytes);
        return n < 0 ? js_undefined : new_typed(J, src, off, n, kind);
    }
    jsval values = js_undefined;
    if (src.t == JS_OBJ) {
        /* Snapshot iterable input before converting values, including aliased views. */
        struct jsobj *list = js_new_array(J);
        struct iter it;
        jsval v;
        int rc;
        if (js_iter_open(J, src, &it) < 0)
            return js_undefined;
        while ((rc = js_iter_next(J, &it, &v)) > 0) {
            if (list->len >= (int)BUFFER_MAX / bytes)
                return js_throw_error(J, "RangeError: typed array too large");
            js_array_push(J, list, v);
        }
        if (rc < 0)
            return js_undefined;
        values = js_obj(list);
        n = list->len;
    } else {
        n = index_arg(J, src, 0, BUFFER_MAX / bytes);
        if (n < 0)
            return js_undefined;
    }
    jsval r = new_typed(J, new_buffer(J, n * bytes), 0, n, kind);
    if (values.t == JS_OBJ)
        for (int i = 0; i < n; i++)
            js_setv(J, r, js_num(i), values.u.o->items[i]);
    return r;
}
#define CTOR(i)                                                                                              \
    static jsval ctor_##i(struct js *J, jsval self, jsval *args, int argc)                                   \
    {                                                                                                        \
        return typed_make(J, args, argc, i);                                                                 \
    }
CTOR(0)
CTOR(1)
CTOR(2) CTOR(3) CTOR(4) CTOR(5) CTOR(6) CTOR(7) CTOR(8) static const js_native ctors[] = {
    ctor_0, ctor_1, ctor_2, ctor_3, ctor_4, ctor_5, ctor_6, ctor_7, ctor_8};
static jsval typed_set(struct js *J, jsval self, jsval *args, int argc)
{
    struct view *a = typed_of(self);
    if (!a)
        return js_throw_error(J, "TypeError: not a typed array");
    int off = index_arg(J, ARG(1), 0, a->len);
    if (off < 0)
        return js_undefined;
    if (ARG(0).t == JS_NULL || ARG(0).t == JS_UNDEF)
        return js_throw_error(J, "TypeError: missing array source");
    int len = index_arg(J, js_get(J, ARG(0), "length"), 0, a->len - off);
    if (len < 0)
        return js_undefined;
    jsval *snapshot = js_alloc(J, (size_t)(len ? len : 1) * sizeof(jsval));
    for (int i = 0; i < len; i++)
        snapshot[i] = js_getv(J, ARG(0), js_num(i));
    for (int i = 0; i < len && !J->throwing; i++)
        js_setv(J, self, js_num(off + i), snapshot[i]);
    return js_undefined;
}
static jsval typed_subarray(struct js *J, jsval self, jsval *args, int argc)
{
    struct view *a = typed_of(self);
    if (!a)
        return js_throw_error(J, "TypeError: not a typed array");
    int lo = relative(J, ARG(0), 0, a->len), hi = relative(J, ARG(1), a->len, a->len);
    if (hi < lo)
        hi = lo;
    return new_typed(J, js_obj(a->buffer), a->offset + lo * a->bytes, hi - lo, a->kind);
}
static int view_get(struct js *J, struct jsobj *o, const char *k, jsval *out)
{
    struct view *v = o->hdata;
    if (!strcmp(k, "buffer"))
        *out = js_obj(v->buffer);
    else if (!strcmp(k, "byteOffset"))
        *out = js_num(v->offset);
    else if (!strcmp(k, "byteLength"))
        *out = js_num(v->len);
    else
        return 0;
    return 1;
}
static int view_set(struct js *J, struct jsobj *o, const char *k, jsval v)
{
    return !strcmp(k, "buffer") || !strcmp(k, "byteOffset") || !strcmp(k, "byteLength");
}
static const struct jsclass view_class = {"DataView", view_get, view_set, NULL};
static jsval view_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    struct buffer *b = buffer_of(ARG(0));
    if (!J->constructing || !b)
        return js_throw_error(J, "TypeError: DataView requires an ArrayBuffer");
    int off = index_arg(J, ARG(1), 0, b->len);
    if (off < 0)
        return js_undefined;
    int len = index_arg(J, ARG(2), b->len - off, b->len - off);
    if (len < 0)
        return js_undefined;
    struct view *v = js_alloc(J, sizeof(*v));
    *v = (struct view){ARG(0).u.o, off, len, 1, 1};
    struct jsobj *o = js_new_host(J, &view_class, v);
    o->proto = J->dataview_proto;
    return js_obj(o);
}
static jsval view_access(struct js *J, jsval self, jsval *args, int argc, int kind, int set)
{
    struct view *v = self.t == JS_OBJ ? js_host_data(self.u.o, &view_class) : NULL;
    if (!v)
        return js_throw_error(J, "TypeError: not a DataView");
    int off = index_arg(J, ARG(0), 0, v->len);
    if (off < 0)
        return js_undefined;
    double value = set ? js_to_number(J, ARG(1)) : 0;
    if (sizes[kind] > v->len - off)
        return js_throw_error(J, "RangeError: DataView access outside buffer");
    unsigned char *p = buffer_of(js_obj(v->buffer))->data + v->offset + off;
    int little = js_truthy(ARG(set ? 2 : 1));
    if (set) {
        write_number(p, kind, little, value);
        return js_undefined;
    }
    return js_num(read_number(p, kind, little));
}
#define ACCESS(name, i)                                                                                      \
    static jsval get_##name(struct js *J, jsval s, jsval *a, int n)                                          \
    {                                                                                                        \
        return view_access(J, s, a, n, i, 0);                                                                \
    }                                                                                                        \
    static jsval set_##name(struct js *J, jsval s, jsval *a, int n)                                          \
    {                                                                                                        \
        return view_access(J, s, a, n, i, 1);                                                                \
    }
ACCESS(Int8, 0)
ACCESS(Uint8, 1)
ACCESS(Int16, 3) ACCESS(Uint16, 4) ACCESS(Int32, 5) ACCESS(Uint32, 6) ACCESS(Float32, 7)
    ACCESS(Float64, 8) static jsval random_fill(struct js *, jsval, jsval *, int);
static jsval encode_utf8(struct js *, jsval, jsval *, int);
static jsval decode_utf8(struct js *, jsval, jsval *, int);
static jsval digest_bytes(struct js *J, jsval self, jsval *args, int argc)
{
    int len = 0;
    unsigned char *p = js_binary_bytes(ARG(1), &len), hash[64];
    if (!p) return js_throw_error(J, "TypeError: digest requires a BufferSource");
    struct jsstr *name = js_tostr(J, ARG(0));
    int n = flux_digest(name->s, p, (size_t)len, hash);
    if (!n) return js_throw_error(J, "NotSupportedError: unsupported digest algorithm");
    jsval b = new_buffer(J, n);
    if (b.t != JS_OBJ) return js_undefined;
    memcpy(buffer_of(b)->data, hash, (size_t)n);
    return b;
}

static jsval hmac_bytes(struct js *J, jsval self, jsval *args, int argc)
{
    int kl = 0, len = 0;
    unsigned char *key = js_binary_bytes(ARG(1), &kl), *data = js_binary_bytes(ARG(2), &len), hash[64];
    if (!key || !data) return js_throw_error(J, "TypeError: HMAC requires BufferSource inputs");
    int size = flux_hmac(js_tostr(J, ARG(0))->s, key, (size_t)kl, data, (size_t)len, hash);
    if (!size) return js_throw_error(J, "OperationError: HMAC failed");
    jsval out = new_buffer(J, size);
    if (out.t == JS_OBJ) memcpy(buffer_of(out)->data, hash, (size_t)size);
    memset(hash, 0, sizeof(hash));
    return out;
}

static jsval crypto_compare(struct js *J, jsval self, jsval *args, int argc)
{
    int al = 0, bl = 0;
    unsigned char *a = js_binary_bytes(ARG(0), &al), *b = js_binary_bytes(ARG(1), &bl);
    if (!a || !b) return js_throw_error(J, "TypeError: signature requires a BufferSource");
    if (al != bl) return js_bool(0);
    volatile unsigned difference = 0;
    for (int i = 0; i < al; i++) difference |= a[i] ^ b[i];
    return js_bool(difference == 0);
}

/* Identify intrinsic binary classes without trusting a writable constructor. */
static jsval binary_type(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    jsval v = ARG(0);
    if (buffer_of(v)) return js_str(J, "ArrayBuffer");
    struct view *a = typed_of(v);
    if (a) return js_str(J, names[a->kind]);
    if (v.t == JS_OBJ && js_host_data(v.u.o, &view_class)) return js_str(J, "DataView");
    return js_null;
}

static jsval binary_clone(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct buffer *b = buffer_of(ARG(0));
    if (b) {
        jsval r = new_buffer(J, b->len);
        memcpy(buffer_of(r)->data, b->data, (size_t)b->len);
        return r;
    }
    struct view *a = typed_of(ARG(0));
    int typed = a != NULL;
    if (!a && ARG(0).t == JS_OBJ) a = js_host_data(ARG(0).u.o, &view_class);
    b = buffer_of(ARG(1));
    if (!a || !b || a->offset > b->len || a->len > (b->len-a->offset)/a->bytes)
        return js_throw_error(J, "DataCloneError: invalid binary clone");
    if (typed) return new_typed(J, ARG(1), a->offset, a->len, a->kind);
    struct view *v = js_alloc(J, sizeof(*v));
    *v = *a; v->buffer = ARG(1).u.o;
    struct jsobj *o = js_new_host(J, &view_class, v);
    o->proto = J->dataview_proto;
    return js_obj(o);
}

/* A new Uint8Array holding a copy of n bytes. */
jsval js_new_bytes(struct js *J, const unsigned char *p, int n)
{
    jsval buf = new_buffer(J, n);
    if (n)
        memcpy(buffer_of(buf)->data, p, (size_t)n);
    return new_typed(J, buf, 0, n, 1);
}

/* ---- CompressionStream's deflate and gzip: a small LZ77 encoder with the fixed Huffman codes ---- */

struct bitw {
    unsigned char *out;
    size_t n, cap;
    uint32_t bits;
    int nbits;
    int failed;
};

static void bw_byte(struct bitw *w, unsigned char b)
{
    if (w->n == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 4096;
        unsigned char *o = realloc(w->out, cap);
        if (!o) {
            w->failed = 1;
            return;
        }
        w->out = o, w->cap = cap;
    }
    w->out[w->n++] = b;
}

static void bw_put(struct bitw *w, uint32_t v, int n)          /* least significant bit first */
{
    w->bits |= v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        bw_byte(w, (unsigned char)w->bits);
        w->bits >>= 8;
        w->nbits -= 8;
    }
}

static void bw_code(struct bitw *w, uint32_t code, int len)    /* Huffman codes go most significant bit first */
{
    uint32_t r = 0;
    for (int i = 0; i < len; i++)
        r = r << 1 | (code >> i & 1);
    bw_put(w, r, len);
}

static void fixed_literal(struct bitw *w, int sym)
{
    if (sym < 144)
        bw_code(w, 0x30 + (uint32_t)sym, 8);
    else if (sym < 256)
        bw_code(w, 0x190 + (uint32_t)(sym - 144), 9);
    else if (sym < 280)
        bw_code(w, (uint32_t)(sym - 256), 7);
    else
        bw_code(w, 0xC0 + (uint32_t)(sym - 280), 8);
}

static void deflate_fixed(struct bitw *w, const unsigned char *in, size_t n)
{
    static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                        67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const uint8_t lextra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                        5, 5, 5, 5, 0 };
    static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                        769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const uint8_t dextra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                        11, 11, 12, 12, 13, 13 };
    enum { WSIZE = 32768, HSIZE = 1 << 15, CHAIN = 48 };
    int32_t *head = malloc(sizeof(int32_t) * HSIZE), *prev = malloc(sizeof(int32_t) * WSIZE);
    bw_put(w, 1, 1);                    /* one block, the last */
    bw_put(w, 1, 2);                    /* fixed codes */
    if (!head || !prev) {
        for (size_t i = 0; i < n; i++)
            fixed_literal(w, in[i]);
    } else {
        memset(head, 0xFF, sizeof(int32_t) * HSIZE);
        size_t i = 0;
        while (i < n) {
            int best = 0, dist = 0;
            if (i + 3 <= n) {
                uint32_t h = ((uint32_t)in[i] << 10 ^ (uint32_t)in[i + 1] << 5 ^ in[i + 2]) & (HSIZE - 1);
                int32_t cand = head[h];
                for (int k = 0; cand >= 0 && k < CHAIN && i - (size_t)cand <= WSIZE - 1; k++) {
                    int len = 0, max = n - i < 258 ? (int)(n - i) : 258;
                    while (len < max && in[cand + len] == in[i + len])
                        len++;
                    if (len > best)
                        best = len, dist = (int)(i - (size_t)cand);
                    if (len == max)
                        break;
                    cand = prev[cand & (WSIZE - 1)];
                }
                prev[i & (WSIZE - 1)] = head[h];
                head[h] = (int32_t)i;
            }
            if (best >= 3) {
                int li = 28;
                while (lbase[li] > best)
                    li--;
                fixed_literal(w, 257 + li);
                bw_put(w, (uint32_t)(best - lbase[li]), lextra[li]);
                int di = 29;
                while (dbase[di] > dist)
                    di--;
                bw_code(w, (uint32_t)di, 5);
                bw_put(w, (uint32_t)(dist - dbase[di]), dextra[di]);
                for (size_t k = i + 1; k < i + (size_t)best && k + 3 <= n; k++) {   /* the hash sees the match */
                    uint32_t h = ((uint32_t)in[k] << 10 ^ (uint32_t)in[k + 1] << 5 ^ in[k + 2]) & (HSIZE - 1);
                    prev[k & (WSIZE - 1)] = head[h];
                    head[h] = (int32_t)k;
                }
                i += (size_t)best;
            } else {
                fixed_literal(w, in[i++]);
            }
        }
    }
    fixed_literal(w, 256);              /* end of block */
    if (w->nbits)
        bw_put(w, 0, 8 - w->nbits);
    free(head);
    free(prev);
}

static uint32_t crc32_of(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static uint32_t adler32_of(const unsigned char *p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}

/* %compress(bytes, "gzip" | "deflate" | "deflate-raw"): a Uint8Array. */
static jsval compress_bytes(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    int len = 0;
    unsigned char *in = js_binary_bytes(ARG(0), &len);
    const char *fmt = js_tostr(J, ARG(1))->s;
    if (!in && len)
        return js_throw_error(J, "TypeError: compress needs bytes");
    struct bitw w = { 0 };
    int gzip = !strcmp(fmt, "gzip"), zlib = !strcmp(fmt, "deflate");
    if (gzip) {
        static const unsigned char hdr[10] = { 0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF };
        for (int i = 0; i < 10; i++)
            bw_byte(&w, hdr[i]);
    } else if (zlib) {
        bw_byte(&w, 0x78);
        bw_byte(&w, 0x01);
    }
    deflate_fixed(&w, in ? in : (const unsigned char *)"", (size_t)len);
    if (gzip) {
        uint32_t crc = crc32_of(in, (size_t)len), sz = (uint32_t)len;
        for (int i = 0; i < 4; i++)
            bw_byte(&w, (unsigned char)(crc >> (8 * i)));
        for (int i = 0; i < 4; i++)
            bw_byte(&w, (unsigned char)(sz >> (8 * i)));
    } else if (zlib) {
        uint32_t ad = adler32_of(in, (size_t)len);
        for (int i = 3; i >= 0; i--)
            bw_byte(&w, (unsigned char)(ad >> (8 * i)));
    }
    if (w.failed || w.n > BUFFER_MAX) {
        free(w.out);
        return js_throw_error(J, "RangeError: out of memory compressing");
    }
    jsval r = js_new_bytes(J, w.out, (int)w.n);
    free(w.out);
    return r;
}

/* %decompress(bytes, format): a Uint8Array, or a TypeError for damaged data. */
static jsval decompress_bytes(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    int len = 0;
    unsigned char *in = js_binary_bytes(ARG(0), &len);
    const char *fmt = js_tostr(J, ARG(1))->s;
    int f = !strcmp(fmt, "gzip") ? INFLATE_GZIP : !strcmp(fmt, "deflate") ? INFLATE_ZLIB : INFLATE_RAW;
    uint8_t *out = NULL;
    size_t outlen = 0;
    if (!in || inflate_buf(in, (size_t)len, f, &out, &outlen, BUFFER_MAX) < 0) {
        free(out);
        return js_throw_error(J, "TypeError: the compressed data is damaged");
    }
    jsval r = js_new_bytes(J, out, (int)outlen);
    free(out);
    return r;
}

void js_init_binary(struct js *J)
{
    js_define_native(J, J->global, "%compress", compress_bytes, 2);
    js_define_native(J, J->global, "%decompress", decompress_bytes, 2);
    js_define_native(J, J->global, "%binaryType", binary_type, 1);
    js_define_native(J, J->global, "%binaryClone", binary_clone, 2);
    js_define_native(J, J->global, "%digest", digest_bytes, 2);
    js_define_native(J, J->global, "%hmac", hmac_bytes, 3);
    js_define_native(J, J->global, "%cryptoCompare", crypto_compare, 2);
    js_define_native(J, J->global, "%randomFill", random_fill, 1);
    js_define_native(J, J->global, "%encodeUTF8", encode_utf8, 1);
    js_define_native(J, J->global, "%decodeUTF8", decode_utf8, 1);
    J->buffer_proto = js_new_object(J);
    struct jsobj *c = ctor(J, "ArrayBuffer", buffer_ctor, J->buffer_proto);
    js_define_native(J, c, "isView", is_view, 1);
    js_define_native(J, J->buffer_proto, "slice", buffer_slice, 2);
    for (int i = 0; i < 9; i++) {
        struct jsobj *p = js_new_object(J);
        J->typed_protos[i] = p;
        c = ctor(J, names[i], ctors[i], p);
        prop(J, c, "BYTES_PER_ELEMENT", js_num(sizes[i]));
        prop(J, p, "BYTES_PER_ELEMENT", js_num(sizes[i]));
        js_define_native(J, p, "set", typed_set, 1);
        js_define_native(J, p, "subarray", typed_subarray, 2);
    }
    J->dataview_proto = js_new_object(J);
    ctor(J, "DataView", view_ctor, J->dataview_proto);
#define BIND(name)                                                                                           \
    js_define_native(J, J->dataview_proto, "get" #name, get_##name, 1);                                      \
    js_define_native(J, J->dataview_proto, "set" #name, set_##name, 2);
    BIND(Int8) BIND(Uint8) BIND(Int16) BIND(Uint16) BIND(Int32) BIND(Uint32) BIND(Float32) BIND(Float64)
#undef BIND
}

/* Encoding helpers keep binary bytes separate from JavaScript's UTF-16 indexing. */
static jsval encode_utf8(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = js_tostr(J, ARG(0));
    const char *p = s->s, *end = p + s->len;
    char *out = js_alloc_atomic(J, (size_t)s->len * 3 + 1);
    int n = 0;
    while (p < end) {
        int cp = utf8_next(&p, end);
        if (cp >= 0xD800 && cp <= 0xDBFF && p < end) {
            const char *next = p;
            int lo = utf8_next(&next, end);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                p = next;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF)
            cp = 0xFFFD;
        n += utf8_put(out + n, cp);
    }
    jsval v = new_typed(J, new_buffer(J, n), 0, n, 1);
    int len;
    memcpy(js_binary_bytes(v, &len), out, (size_t)n);
    return v;
}
static jsval decode_utf8(struct js *J, jsval self, jsval *args, int argc)
{
    int len = 0;
    unsigned char *bytes = js_binary_bytes(ARG(0), &len);
    if (!bytes && ARG(0).t != JS_UNDEF)
        return js_throw_error(J, "TypeError: TextDecoder requires a buffer or view");
    char *out = js_alloc_atomic(J, (size_t)len * 3 + 1);
    int n = 0, i = 0;
    while (i < len) {
        unsigned c = bytes[i++];
        int cp = (int)c, count = 0, valid = 1;
        if (c >= 0xC2 && c <= 0xDF)
            cp = c & 31, count = 1;
        else if (c >= 0xE0 && c <= 0xEF)
            cp = c & 15, count = 2;
        else if (c >= 0xF0 && c <= 0xF4)
            cp = c & 7, count = 3;
        else if (c >= 0x80)
            valid = 0;
        for (int k = 0; k < count && valid; k++) {
            if (i >= len || (bytes[i] & 0xC0) != 0x80 ||
                (k == 0 && ((c == 0xE0 && bytes[i] < 0xA0) || (c == 0xED && bytes[i] >= 0xA0) ||
                            (c == 0xF0 && bytes[i] < 0x90) || (c == 0xF4 && bytes[i] >= 0x90)))) {
                valid = 0;
                break;
            }
            cp = (cp << 6) | (bytes[i++] & 63);
        }
        if (!valid) {
            if (js_truthy(ARG(1)))
                return js_throw_error(J, "TypeError: invalid UTF-8");
            cp = 0xFFFD;
        }
        if (n == 0 && cp == 0xFEFF && !js_truthy(ARG(2)))
            continue;
        n += utf8_put(out + n, cp);
    }
    return js_strn(J, out, n);
}

static jsval random_fill(struct js *J, jsval self, jsval *args, int argc)
{
    struct view *a = typed_of(ARG(0));
    if (!a || a->kind >= 7)
        return js_throw_error(J, "TypeMismatchError: an integer typed array is required");
    int len;
    unsigned char *p = js_binary_bytes(ARG(0), &len);
    if (len > 65536)
        return js_throw_error(J, "QuotaExceededError: getRandomValues is limited to 65536 bytes");
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return js_throw_error(J, "OperationError: random source unavailable");
    int got = 0;
    while (got < len) {
        int n = (int)read(fd, p + got, (size_t)(len - got));
        if (n <= 0)
            break;
        got += n;
    }
    close(fd);
    if (got != len)
        return js_throw_error(J, "OperationError: random source failed");
    return ARG(0);
}
