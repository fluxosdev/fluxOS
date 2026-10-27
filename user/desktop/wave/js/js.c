/*
 * The JavaScript engine: values, objects, the lexer, the parser and a
 * tree-walking interpreter. The built-in objects are in jslib.c and
 * regular expressions in jsre.c.
 *
 * The language is most of ES2020 and some later: var, let and const;
 * functions, closures and arrow functions with default and rest
 * parameters; classes (fields, #private members, extends, super, getters,
 * statics); destructuring in declarations, assignments, parameters, loops
 * and catch; objects with getters and setters, arrays, strings, numbers,
 * booleans; every operator; if, for, for-in, for-of, for await, while,
 * do-while, switch, labels, break and continue; try, catch, finally and
 * throw; template literals (tagged too), regular expression literals,
 * optional chaining, spread; generators, async functions and async
 * generators, which run as coroutines on stacks of their own (see the
 * coroutine section); promises and their microtask queue; ES modules (see
 * the modules section: the host fetches, the engine links and runs). Not
 * there: BigInt (read as a number).
 *
 * Strings are stored as UTF-8/WTF-8; indexed access and core string
 * operations expose UTF-16 code units. The regex engine still uses bytes.
 *
 * Memory comes from an arena. Call environments that no closure captured
 * are recycled; everything else lasts until the page goes. Running out of
 * memory, or of steps, ends the page's scripts: the engine jumps back to
 * the outermost entry (with GCC's builtin setjmp).
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <flux.h>
#include "jsint.h"

const jsval js_undefined = { JS_UNDEF, { 0 } };
const jsval js_null = { JS_NULL, { 0 } };

/* ---- memory ---- */

static void *jmp_target[5];
static int entered;
static struct js *active;

static void bail(struct js *J, const char *why) __attribute__((noreturn));
static void bail(struct js *J, const char *why)
{
    snprintf(J->err, sizeof(J->err), "%s", why);
    J->dead = 1;
    __builtin_longjmp(jmp_target, 1);
}

static void *alloc_kind(struct js *J, size_t n, int atomic)
{
    if (gc_heap_bytes(J) + n > J->heap_limit && entered) {
        gc_collect(J);
        if (gc_heap_bytes(J) + n > J->heap_limit)
            bail(J, "the page's scripts ran out of memory");
    }
    void *p = gc_alloc(J, n, atomic);
    if (!p) {
        gc_collect(J);
        p = gc_alloc(J, n, atomic);
    }
    if (!p) {
        if (entered)
            bail(J, "the page's scripts ran out of memory");
        abort();
    }
    return p;
}

void *js_alloc(struct js *J, size_t n)
{
    return alloc_kind(J, n, 0);
}

void *js_alloc_atomic(struct js *J, size_t n)
{
    return alloc_kind(J, n, 1);
}

struct jsstr *js_mkstr(struct js *J, const char *s, int n)
{
    struct jsstr *r = js_alloc_atomic(J, sizeof(struct jsstr) + (size_t)n + 1);
    r->len = n;
    if (n)
        memcpy(r->s, s, (size_t)n);
    r->s[n] = 0;
    return r;
}

/* Keep UTF-8 storage, expose UTF-16 code units to JavaScript. Lone surrogates use WTF-8. */
static int next_u16(const char **p, const char *end, int *pending)
{
    if (*pending) {
        int unit = *pending;
        *pending = 0;
        return unit;
    }
    if (*p >= end)
        return -1;
    int cp = utf8_next(p, end);
    if (cp > 0xFFFF) {
        *pending = 0xDC00 + ((cp - 0x10000) & 1023);
        return 0xD800 + ((cp - 0x10000) >> 10);
    }
    return cp;
}

int js_u16len(const struct jsstr *s)
{
    if (s->utf16_length || !s->len)
        return s->utf16_length;
    int n = 0;
    const char *p = s->s, *end = p + s->len;
    while (p < end)
        n += utf8_next(&p, end) > 0xFFFF ? 2 : 1;
    ((struct jsstr *)s)->utf16_length = n;
    return n;
}
int js_u16unit(const struct jsstr *s, int index)
{
    if (index < 0)
        return -1;
    const char *p = s->s, *end = p + s->len;
    while (p < end) {
        int cp = utf8_next(&p, end);
        if (cp > 0xFFFF) {
            if (index-- == 0)
                return 0xD800 + ((cp - 0x10000) >> 10);
            cp = 0xDC00 + ((cp - 0x10000) & 1023);
        }
        if (index-- == 0)
            return cp;
    }
    return -1;
}
struct jsstr *js_u16slice(struct js *J, const struct jsstr *s, int start, int end)
{
    if (end <= start)
        return js_mkstr(J, "", 0);
    char *buf = js_alloc_atomic(J, (size_t)(end - start) * 3 + 1);
    const char *p = s->s, *limit = p + s->len;
    int i = 0, n = 0;
    while (p < limit && i < end) {
        int cp = utf8_next(&p, limit), units = cp > 0xFFFF ? 2 : 1;
        if (units == 2 && (i < start || i + 1 >= end)) {
            if (i >= start && i < end)
                n += utf8_put(buf + n, 0xD800 + ((cp - 0x10000) >> 10));
            if (i + 1 >= start && i + 1 < end)
                n += utf8_put(buf + n, 0xDC00 + ((cp - 0x10000) & 1023));
        } else if (i >= start)
            n += utf8_put(buf + n, cp);
        i += units;
    }
    return js_mkstr(J, buf, n);
}

struct jsstr *js_concat(struct js *J, struct jsstr *a, struct jsstr *b)
{
    struct jsstr *r = js_alloc_atomic(J, sizeof(struct jsstr) + (size_t)a->len + (size_t)b->len + 1);
    r->len = a->len + b->len;
    memcpy(r->s, a->s, (size_t)a->len);
    memcpy(r->s + a->len, b->s, (size_t)b->len);
    r->s[r->len] = 0;
    return r;
}

static int str_eq(const struct jsstr *a, const struct jsstr *b)
{
    if (a == b || (a->len == b->len && !memcmp(a->s, b->s, (size_t)a->len)))
        return 1;
    /* A scalar and its separately produced surrogate pair are the same JS string. */
    if (js_u16len(a) != js_u16len(b))
        return 0;
    const char *ap = a->s, *bp = b->s, *ae = ap + a->len, *be = bp + b->len;
    int ah = 0, bh = 0;
    for (;;) {
        int x = next_u16(&ap, ae, &ah), y = next_u16(&bp, be, &bh);
        if (x != y)
            return 0;
        if (x < 0)
            return 1;
    }
}

static int str_is(const struct jsstr *a, const char *c)
{
    return !strcmp(a->s, c);
}

/* ---- values ---- */

jsval js_num(double n)
{
    jsval v;
    v.t = JS_NUM;
    v.u.n = n;
    return v;
}

jsval js_bool(int b)
{
    jsval v;
    v.t = JS_BOOL;
    v.u.b = b != 0;
    return v;
}

jsval js_strn(struct js *J, const char *s, int n)
{
    jsval v;
    v.t = JS_STR;
    v.u.s = js_mkstr(J, s, n);
    return v;
}

jsval js_str(struct js *J, const char *s)
{
    return js_strn(J, s, (int)strlen(s));
}

static jsval strv(struct jsstr *s)
{
    jsval v;
    v.t = JS_STR;
    v.u.s = s;
    return v;
}

jsval js_obj(struct jsobj *o)
{
    jsval v;
    if (!o)
        return js_null;
    v.t = JS_OBJ;
    v.u.o = o;
    return v;
}

const char *js_cstr(jsval v)
{
    return v.t == JS_STR ? v.u.s->s : "";
}

int js_strlen(jsval v)
{
    return v.t == JS_STR ? v.u.s->len : 0;
}

int js_truthy(jsval v)
{
    switch (v.t) {
    case JS_BOOL: return v.u.b;
    case JS_NUM: return v.u.n != 0 && !isnan(v.u.n);
    case JS_STR: return v.u.s->len > 0;
    case JS_OBJ: return 1;
    default: return 0;
    }
}

int js_is_callable(jsval v)
{
    return v.t == JS_OBJ && v.u.o->cls == C_FUNCTION;
}

int js_is_array(jsval v)
{
    return v.t == JS_OBJ && v.u.o->cls == C_ARRAY;
}

int js_throwing(struct js *J)
{
    return J->throwing;
}

struct jsobj *js_global(struct js *J)
{
    return J->global;
}

void *js_host(struct js *J)
{
    return J->host;
}

/* ---- numbers ---- */

static long double pow10l_(int e)
{
    long double r = 1, b = 10;
    int neg = e < 0;
    if (neg)
        e = -e;
    while (e) {
        if (e & 1)
            r *= b;
        b *= b;
        e >>= 1;
    }
    return neg ? 1 / r : r;
}

/* Up to 17 decimal digits of an integer-valued long double below 1e18. */
static int int_digits(long double r, char *out)
{
    uint32_t hi = (uint32_t)(long long)(r / 1e9L);
    uint32_t lo = (uint32_t)(long long)(r - (long double)hi * 1e9L);
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + lo % 10);
        lo /= 10;
    } while (lo && n < 9);
    if (hi) {
        while (n < 9)
            tmp[n++] = '0';
        do {
            tmp[n++] = (char)('0' + hi % 10);
            hi /= 10;
        } while (hi);
    }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

/* Number.prototype.toString(): the shortest digits that read back the same. */
void js_numtostr(double d, char *out)
{
    if (isnan(d)) {
        strcpy(out, "NaN");
        return;
    }
    if (isinf(d)) {
        strcpy(out, d < 0 ? "-Infinity" : "Infinity");
        return;
    }
    if (d == 0) {
        strcpy(out, "0");
        return;
    }
    char *o = out;
    if (d < 0) {
        *o++ = '-';
        d = -d;
    }
    char digits[32];
    int k = 0, n = 0;
    int e = (int)floor(log10(d));
    for (int p = 1; p <= 17; p++) {
        int ee = e;
        long double m = (long double)d / pow10l_(ee - p + 1);
        long double r = (long double)(long long)(m + 0.5L);
        if (r >= pow10l_(p)) {
            ee++;
            r = (long double)(long long)((long double)d / pow10l_(ee - p + 1) + 0.5L);
        }
        if (r < pow10l_(p - 1)) {           /* log10 overshot */
            ee--;
            r = (long double)(long long)((long double)d / pow10l_(ee - p + 1) + 0.5L);
        }
        double back = (double)(r * pow10l_(ee - p + 1));
        if (back == d || p == 17) {
            k = int_digits(r, digits);
            n = ee + 1;
            break;
        }
    }
    while (k > 1 && digits[k - 1] == '0') {
        k--;
    }
    digits[k] = 0;
    if (k <= n && n <= 21) {
        memcpy(o, digits, (size_t)k);
        o += k;
        for (int i = k; i < n; i++)
            *o++ = '0';
        *o = 0;
    } else if (0 < n && n <= 21) {
        memcpy(o, digits, (size_t)n);
        o += n;
        *o++ = '.';
        memcpy(o, digits + n, (size_t)(k - n));
        o += k - n;
        *o = 0;
    } else if (-6 < n && n <= 0) {
        *o++ = '0';
        *o++ = '.';
        for (int i = 0; i < -n; i++)
            *o++ = '0';
        memcpy(o, digits, (size_t)k);
        o[k] = 0;
    } else {
        *o++ = digits[0];
        if (k > 1) {
            *o++ = '.';
            memcpy(o, digits + 1, (size_t)(k - 1));
            o += k - 1;
        }
        sprintf(o, "e%c%d", n - 1 < 0 ? '-' : '+', n - 1 < 0 ? 1 - n : n - 1);
    }
}

/* A number at the start of s: its value, and in *used how much it took (0: none). */
static double parse_number(const char *s, int n, int *used)
{
    int i = 0, neg = 0;
    *used = 0;
    if (i < n && (s[i] == '+' || s[i] == '-'))
        neg = s[i++] == '-';
    if (n - i >= 8 && !memcmp(s + i, "Infinity", 8)) {
        *used = i + 8;
        return neg ? -INFINITY : INFINITY;
    }
    if (n - i > 2 && s[i] == '0' && (s[i + 1] | 32) != 'e' && strchr("xXoObB", s[i + 1])) {
        int base = (s[i + 1] | 32) == 'x' ? 16 : (s[i + 1] | 32) == 'o' ? 8 : 2;
        int j = i + 2;
        double v = 0;
        for (; j < n; j++) {
            int c = s[j], dv = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 99;
            if (dv >= base)
                break;
            v = v * base + dv;
        }
        if (j == i + 2)
            return NAN;
        *used = j;
        return neg ? -v : v;
    }
    long double m = 0;
    int exp10 = 0, digits = 0, any = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        if (digits < 19) {
            m = m * 10 + (s[i] - '0');
            if (m)
                digits++;
        } else {
            exp10++;
        }
        i++;
        any = 1;
    }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (digits < 19) {
                m = m * 10 + (s[i] - '0');
                if (m)
                    digits++;
                exp10--;
            }
            i++;
            any = 1;
        }
    }
    if (!any)
        return NAN;
    if (i < n && (s[i] | 32) == 'e') {
        int j = i + 1, eneg = 0, ev = 0, edig = 0;
        if (j < n && (s[j] == '+' || s[j] == '-'))
            eneg = s[j++] == '-';
        while (j < n && s[j] >= '0' && s[j] <= '9') {
            if (ev < 10000)
                ev = ev * 10 + (s[j] - '0');
            j++;
            edig = 1;
        }
        if (edig) {
            exp10 += eneg ? -ev : ev;
            i = j;
        }
    }
    *used = i;
    double v;
    if (exp10 > 400)
        v = m ? INFINITY : 0;
    else if (exp10 < -400)
        v = 0;
    else
        v = (double)(m * pow10l_(exp10));
    return neg ? -v : v;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

double js_strtonum(const char *s, int n)
{
    while (n && is_space((unsigned char)*s))
        s++, n--;
    while (n && is_space((unsigned char)s[n - 1]))
        n--;
    if (!n)
        return 0;
    int used;
    double v = parse_number(s, n, &used);
    return used == n ? v : NAN;
}

double js_parse_float_prefix(const char *s, int n)
{
    while (n && is_space((unsigned char)*s))
        s++, n--;
    int used;
    if (n >= 2 && s[0] == '0' && (s[1] | 32) == 'x')
        n = 1;                          /* parseFloat("0x10") is 0 */
    double v = parse_number(s, n, &used);
    return used ? v : NAN;
}

/* ---- conversions ---- */

struct jsstr *js_tostr(struct js *J, jsval v)
{
    char buf[64];
    switch (v.t) {
    case JS_UNDEF: return js_mkstr(J, "undefined", 9);
    case JS_NULL: return js_mkstr(J, "null", 4);
    case JS_BOOL: return v.u.b ? js_mkstr(J, "true", 4) : js_mkstr(J, "false", 5);
    case JS_NUM:
        if (v.u.n >= 0 && v.u.n < 256 && v.u.n == (int)v.u.n) {
            int k = (int)v.u.n;
            if (!J->small_ints[k]) {
                snprintf(buf, sizeof(buf), "%d", k);
                J->small_ints[k] = js_mkstr(J, buf, (int)strlen(buf));
            }
            return J->small_ints[k];
        }
        js_numtostr(v.u.n, buf);
        return js_mkstr(J, buf, (int)strlen(buf));
    case JS_STR: return v.u.s;
    default: {
        jsval p = js_to_primitive(J, v, 1);
        if (J->throwing)
            return js_mkstr(J, "", 0);
        return p.t == JS_OBJ ? js_mkstr(J, "[object Object]", 15) : js_tostr(J, p);
    }
    }
}

jsval js_to_string(struct js *J, jsval v)
{
    return strv(js_tostr(J, v));
}

double js_to_number(struct js *J, jsval v)
{
    switch (v.t) {
    case JS_UNDEF: return NAN;
    case JS_NULL: return 0;
    case JS_BOOL: return v.u.b;
    case JS_NUM: return v.u.n;
    case JS_STR: return js_strtonum(v.u.s->s, v.u.s->len);
    default: {
        jsval p = js_to_primitive(J, v, 0);
        if (J->throwing || p.t == JS_OBJ)
            return NAN;
        return js_to_number(J, p);
    }
    }
}

static int32_t to_int32(struct js *J, jsval v)
{
    double d = js_to_number(J, v);
    if (isnan(d) || isinf(d))
        return 0;
    d = trunc(d);
    d = fmod(d, 4294967296.0);
    if (d < 0)
        d += 4294967296.0;
    return (int32_t)(uint32_t)(long long)d;
}

static uint32_t to_uint32(struct js *J, jsval v)
{
    return (uint32_t)to_int32(J, v);
}

const char *js_text(struct js *J, jsval v)
{
    int saved = J->throwing;
    jsval exc = J->exc;
    struct jsstr *s = js_tostr(J, v);
    J->throwing = saved;
    J->exc = exc;
    snprintf(J->textbuf, sizeof(J->textbuf), "%s", s->s);
    return J->textbuf;
}

int js_index(jsval key, uint32_t *out)
{
    if (key.t == JS_NUM) {
        double d = key.u.n;
        if (d >= 0 && d < 4294967295.0 && d == floor(d)) {
            *out = (uint32_t)(long long)d;
            return 1;
        }
        return 0;
    }
    if (key.t == JS_STR) {
        const struct jsstr *s = key.u.s;
        if (!s->len || s->len > 10 || (s->len > 1 && s->s[0] == '0'))
            return 0;
        uint32_t v = 0;
        for (int i = 0; i < s->len; i++) {
            if (s->s[i] < '0' || s->s[i] > '9')
                return 0;
            v = v * 10 + (uint32_t)(s->s[i] - '0');
        }
        *out = v;
        return 1;
    }
    return 0;
}

/* ---- objects ---- */

struct jsobj *js_mkobj(struct js *J, int cls, struct jsobj *proto)
{
    struct jsobj *o = js_alloc(J, sizeof(*o));
    o->cls = (uint8_t)cls;
    o->proto = proto;
    return o;
}

struct jsobj *js_new_object(struct js *J)
{
    return js_mkobj(J, C_OBJECT, J->object_proto);
}

struct jsobj *js_new_array(struct js *J)
{
    return js_mkobj(J, C_ARRAY, J->array_proto);
}

struct jsobj *js_new_host(struct js *J, const struct jsclass *cls, void *data)
{
    struct jsobj *o = js_mkobj(J, C_HOST, J->object_proto);
    o->hcls = cls;
    o->hdata = data;
    return o;
}

void *js_host_data(struct jsobj *o, const struct jsclass *cls)
{
    return o && o->cls == C_HOST && o->hcls == cls ? o->hdata : NULL;
}

struct prop *js_own(struct jsobj *o, struct jsstr *key)
{
    for (struct prop *p = o->props; p; p = p->next)
        if (str_eq(p->key, key))
            return p;
    return NULL;
}

void js_put(struct js *J, struct jsobj *o, struct jsstr *key, jsval v, int hidden)
{
    struct prop *p = js_own(o, key);
    if (!p) {
        p = js_alloc(J, sizeof(*p));
        p->key = key;
        p->hidden = (uint8_t)hidden;
        /* Append, so enumeration follows insertion order. */
        struct prop **pp = &o->props;
        while (*pp)
            pp = &(*pp)->next;
        *pp = p;
    }
    p->v = v;
}

static void array_reserve(struct js *J, struct jsobj *a, int n)
{
    if (n <= a->cap)
        return;
    int cap = a->cap ? a->cap * 2 : 8;
    while (cap < n)
        cap *= 2;
    jsval *items = js_alloc(J, sizeof(jsval) * (size_t)cap);
    if (a->len)
        memcpy(items, a->items, sizeof(jsval) * (size_t)a->len);
    a->items = items;
    a->cap = cap;
}

int js_array_set_len(struct js *J, struct jsobj *a, int len)
{
    if (len < 0 || len > (1 << 24))
        return -1;
    array_reserve(J, a, len);
    for (int i = a->len; i < len; i++)
        a->items[i] = js_undefined;
    a->len = len;
    return 0;
}

void js_array_push(struct js *J, struct jsobj *a, jsval v)
{
    array_reserve(J, a, a->len + 1);
    a->items[a->len++] = v;
}

static jsval fn_prototype(struct js *J, struct jsobj *f);

/* ---- Proxy: the traps get, set, has, deleteProperty and ownKeys ---- */

static int proxy_trap(struct js *J, struct jsobj *p, const char *name, jsval *fn)
{
    jsval t = js_get(J, js_obj(p->phandler), name);
    if (J->throwing || !js_is_callable(t))
        return 0;
    *fn = t;
    return 1;
}

static int obj_get(struct js *J, struct jsobj *o, struct jsstr *key, jsval self, jsval *out);

static int proxy_get(struct js *J, struct jsobj *p, struct jsstr *key, jsval self, jsval *out)
{
    jsval fn;
    if (proxy_trap(J, p, "get", &fn)) {
        jsval a[3] = { js_obj(p->ptarget), strv(key), self };
        *out = js_call_value(J, fn, js_obj(p->phandler), a, 3);
        return 1;
    }
    if (J->throwing) {
        *out = js_undefined;
        return 1;
    }
    return obj_get(J, p->ptarget, key, js_obj(p->ptarget), out);
}

/* A property of an object, through its prototype chain: 1 if found. */
static int obj_get(struct js *J, struct jsobj *o, struct jsstr *key, jsval self, jsval *out)
{
    for (struct jsobj *x = o; x; x = x->proto) {
        if (x->phandler)
            return proxy_get(J, x, key, self, out);
        if ((x->cls == C_ARRAY || x->cls == C_ARGUMENTS) && str_is(key, "length")) {
            *out = js_num(x->len);
            return 1;
        }
        if (x->cls == C_HOST && x->hcls->get && x->hcls->get(J, x, key->s, out))
            return 1;
        struct prop *p = js_own(x, key);
        if (p) {
            if (p->getter)
                *out = js_call_value(J, js_obj(p->getter), self, NULL, 0);
            else if (p->setter)
                *out = js_undefined;
            else
                *out = p->v;
            return 1;
        }
        if (x->cls == C_FUNCTION && x == o) {
            if (str_is(key, "prototype") && x->fn && !x->arrow) {
                *out = fn_prototype(J, x);
                return 1;
            }
            if (str_is(key, "name")) {
                *out = js_str(J, x->name ? x->name : "");
                return 1;
            }
            if (str_is(key, "length")) {
                *out = js_num(x->nargs);
                return 1;
            }
        }
        if (x->cls == C_STRING && x == o) {
            if (str_is(key, "length")) {
                *out = js_num(js_u16len(x->str));
                return 1;
            }
        }
    }
    return 0;
}

static jsval fn_prototype(struct js *J, struct jsobj *f)
{
    struct jsstr *name = js_mkstr(J, "prototype", 9);
    struct prop *p = js_own(f, name);
    if (p)
        return p->v;
    struct jsobj *proto = js_new_object(J);
    js_put(J, proto, js_mkstr(J, "constructor", 11), js_obj(f), 1);
    js_put(J, f, name, js_obj(proto), 1);
    return js_obj(proto);
}

static struct jsobj *proto_of_primitive(struct js *J, jsval v)
{
    switch (v.t) {
    case JS_STR: return J->string_proto;
    case JS_NUM: return J->number_proto;
    case JS_BOOL: return J->boolean_proto;
    default: return NULL;
    }
}

jsval js_get_str(struct js *J, jsval obj, struct jsstr *key)
{
    jsval out;
    if (obj.t == JS_OBJ) {
        if (obj_get(J, obj.u.o, key, obj, &out))
            return out;
        return js_undefined;
    }
    if (obj.t == JS_UNDEF || obj.t == JS_NULL)
        return js_throw_error(J, "TypeError: cannot read properties of %s (reading '%s')",
                              obj.t == JS_NULL ? "null" : "undefined", key->s);
    if (obj.t == JS_STR) {
        if (str_is(key, "length"))
            return js_num(js_u16len(obj.u.s));
        uint32_t i;
        if (js_index(strv(key), &i))
            return i < (uint32_t)js_u16len(obj.u.s)
                       ? (jsval){JS_STR, {.s = js_u16slice(J, obj.u.s, (int)i, (int)i + 1)}}
                       : js_undefined;
    }
    struct jsobj *proto = proto_of_primitive(J, obj);
    if (proto && obj_get(J, proto, key, obj, &out))
        return out;
    return js_undefined;
}

jsval js_getv(struct js *J, jsval obj, jsval key)
{
    uint32_t i;
    if (obj.t == JS_OBJ && (obj.u.o->cls == C_ARRAY || obj.u.o->cls == C_ARGUMENTS) &&
        js_index(key, &i)) {
        struct jsobj *a = obj.u.o;
        return i < (uint32_t)a->len ? a->items[i] : js_undefined;
    }
    if (obj.t == JS_STR && key.t == JS_NUM && js_index(key, &i))
        return i < (uint32_t)js_u16len(obj.u.s)
                   ? (jsval){JS_STR, {.s = js_u16slice(J, obj.u.s, (int)i, (int)i + 1)}}
                   : js_undefined;
    struct jsstr *k = js_tostr(J, key);
    if (J->throwing)
        return js_undefined;
    return js_get_str(J, obj, k);
}

jsval js_get(struct js *J, jsval obj, const char *key)
{
    return js_get_str(J, obj, js_mkstr(J, key, (int)strlen(key)));
}

static void obj_set(struct js *J, struct jsobj *o, struct jsstr *key, jsval v, jsval self)
{
    if (o->phandler) {
        jsval fn;
        if (proxy_trap(J, o, "set", &fn)) {
            jsval a[4] = { js_obj(o->ptarget), strv(key), v, self };
            js_call_value(J, fn, js_obj(o->phandler), a, 4);
        } else if (!J->throwing) {
            obj_set(J, o->ptarget, key, v, js_obj(o->ptarget));
        }
        return;
    }
    if (o->cls == C_ARRAY || o->cls == C_ARGUMENTS) {
        uint32_t i;
        if (js_index(strv(key), &i)) {
            if (i >= (uint32_t)o->len && js_array_set_len(J, o, (int)i + 1) < 0) {
                js_throw_error(J, "RangeError: array too large");
                return;
            }
            o->items[i] = v;
            return;
        }
        if (str_is(key, "length")) {
            double d = js_to_number(J, v);
            if (d < 0 || d != floor(d) || js_array_set_len(J, o, (int)d) < 0)
                js_throw_error(J, "RangeError: invalid array length");
            return;
        }
    }
    if (o->cls == C_HOST && o->hcls->set && o->hcls->set(J, o, key->s, v))
        return;
    /* A setter anywhere up the chain takes the write. */
    for (struct jsobj *x = o; x; x = x->proto) {
        struct prop *p = js_own(x, key);
        if (p && p->setter) {
            jsval arg = v;
            js_call_value(J, js_obj(p->setter), self, &arg, 1);
            return;
        }
        if (p && x == o) {
            if (!p->getter)
                p->v = v;
            return;
        }
        if (p)
            break;
    }
    js_put(J, o, key, v, 0);
}

void js_setv(struct js *J, jsval obj, jsval key, jsval v)
{
    if (obj.t != JS_OBJ) {
        if (obj.t == JS_UNDEF || obj.t == JS_NULL) {
            struct jsstr *k = js_tostr(J, key);
            js_throw_error(J, "TypeError: cannot set properties of %s (setting '%s')",
                           obj.t == JS_NULL ? "null" : "undefined", k->s);
        }
        return;                         /* primitives: silently nothing */
    }
    uint32_t i;
    struct jsobj *o = obj.u.o;
    if ((o->cls == C_ARRAY || o->cls == C_ARGUMENTS) && key.t == JS_NUM && js_index(key, &i)) {
        if (i >= (uint32_t)o->len && js_array_set_len(J, o, (int)i + 1) < 0) {
            js_throw_error(J, "RangeError: array too large");
            return;
        }
        o->items[i] = v;
        return;
    }
    struct jsstr *k = js_tostr(J, key);
    if (J->throwing)
        return;
    obj_set(J, o, k, v, obj);
}

void js_set(struct js *J, jsval obj, const char *key, jsval v)
{
    js_setv(J, obj, js_str(J, key), v);
}

int js_has(struct js *J, jsval obj, struct jsstr *key)
{
    if (obj.t != JS_OBJ)
        return 0;
    int indexed = js_typed_has(obj, key->s);
    if (indexed >= 0)
        return indexed;
    uint32_t i;
    for (struct jsobj *x = obj.u.o; x; x = x->proto) {
        if (x->phandler) {
            jsval fn;
            if (proxy_trap(J, x, "has", &fn)) {
                jsval a[2] = { js_obj(x->ptarget), strv(key) };
                return js_truthy(js_call_value(J, fn, js_obj(x->phandler), a, 2));
            }
            return js_has(J, js_obj(x->ptarget), key);
        }
        if ((x->cls == C_ARRAY || x->cls == C_ARGUMENTS) &&
            ((js_index(strv(key), &i) && i < (uint32_t)x->len) || str_is(key, "length")))
            return 1;
        if (js_own(x, key))
            return 1;
        jsval tmp;
        if (x->cls == C_HOST && x->hcls->get && x->hcls->get(J, x, key->s, &tmp))
            return 1;
    }
    return 0;
}

int js_delete(struct js *J, jsval obj, struct jsstr *key)
{
    (void)J;
    if (obj.t != JS_OBJ)
        return 1;
    int indexed = js_typed_has(obj, key->s);
    if (indexed >= 0)
        return !indexed;
    struct jsobj *o = obj.u.o;
    uint32_t i;
    if (o->phandler) {
        jsval fn;
        if (proxy_trap(J, o, "deleteProperty", &fn)) {
            jsval a[2] = { js_obj(o->ptarget), strv(key) };
            return js_truthy(js_call_value(J, fn, js_obj(o->phandler), a, 2));
        }
        return js_delete(J, js_obj(o->ptarget), key);
    }
    if ((o->cls == C_ARRAY || o->cls == C_ARGUMENTS) && js_index(strv(key), &i)) {
        if (i < (uint32_t)o->len)
            o->items[i] = js_undefined;
        return 1;
    }
    for (struct prop **pp = &o->props; *pp; pp = &(*pp)->next)
        if (str_eq((*pp)->key, key)) {
            *pp = (*pp)->next;
            return 1;
        }
    return 1;
}

int js_keys(struct js *J, jsval obj, int inherited, struct jsstr ***out)
{
    int n = 0, cap = 16;
    struct jsstr **keys = js_alloc(J, sizeof(*keys) * (size_t)cap);
    if (obj.t == JS_STR) {
        for (int i = 0, len = js_u16len(obj.u.s); i < len; i++) {
            char b[16];
            snprintf(b, sizeof(b), "%d", i);
            if (n == cap) {
                struct jsstr **nk = js_alloc(J, sizeof(*nk) * (size_t)cap * 2);
                memcpy(nk, keys, sizeof(*nk) * (size_t)n);
                keys = nk;
                cap *= 2;
            }
            keys[n++] = js_mkstr(J, b, (int)strlen(b));
        }
        *out = keys;
        return n;
    }
    if (obj.t != JS_OBJ) {
        *out = keys;
        return 0;
    }
    if (obj.u.o->phandler) {
        struct jsobj *p = obj.u.o;
        jsval fn, tgt = js_obj(p->ptarget);
        if (proxy_trap(J, p, "ownKeys", &fn)) {
            jsval r = js_call_value(J, fn, js_obj(p->phandler), &tgt, 1);
            if (J->throwing || r.t != JS_OBJ || r.u.o->cls != C_ARRAY) {
                *out = keys;
                return 0;
            }
            int len = r.u.o->len;
            keys = js_alloc(J, sizeof(*keys) * (size_t)(len ? len : 1));
            for (int i = 0; i < len; i++)
                keys[i] = js_tostr(J, r.u.o->items[i]);
            *out = keys;
            return len;
        }
        return js_keys(J, tgt, inherited, out);
    }
    for (struct jsobj *x = obj.u.o; x; x = inherited ? x->proto : NULL) {
        int typedlen = js_typed_length(js_obj(x));
        int count = (x->cls == C_ARRAY || x->cls == C_ARGUMENTS) ? x->len : typedlen > 0 ? typedlen : 0;
        for (struct prop *p = x->props; p; p = p->next)
            count++;
        if (n + count > cap) {
            while (n + count > cap)
                cap *= 2;
            struct jsstr **nk = js_alloc(J, sizeof(*nk) * (size_t)cap);
            memcpy(nk, keys, sizeof(*nk) * (size_t)n);
            keys = nk;
        }
        if (x->cls == C_ARRAY || x->cls == C_ARGUMENTS || typedlen >= 0)
            for (int i = 0, len = typedlen >= 0 ? typedlen : x->len; i < len; i++) {
                char b[16];
                snprintf(b, sizeof(b), "%d", i);
                keys[n++] = js_mkstr(J, b, (int)strlen(b));
            }
        for (struct prop *p = x->props; p; p = p->next) {
            if (p->hidden || (p->key->len > 2 && p->key->s[0] == '@' && p->key->s[1] == '@') ||
                (p->key->len > 1 && p->key->s[0] == '#'))
                continue;                       /* not enumerable; "@@..." keys are symbols, "#..." private */
            int dup = 0;
            for (int i = 0; i < n && !dup; i++)
                dup = str_eq(keys[i], p->key);
            if (!dup)
                keys[n++] = p->key;
        }
    }
    /* Integer keys come first, in numeric order, then the others as they were added. */
    int nint = 0, ordered = 1;
    uint32_t last = 0;
    for (int i = 0; i < n; i++) {
        uint32_t v;
        if (js_index(strv(keys[i]), &v)) {
            if (i != nint || (nint && v < last))
                ordered = 0;
            last = v;
            nint++;
        }
    }
    if (nint && !ordered) {
        struct jsstr **sorted = js_alloc(J, sizeof(*sorted) * (size_t)(n ? n : 1));
        int k = 0;
        for (int i = 0; i < n; i++) {           /* integer keys, by insertion sort */
            uint32_t v;
            if (!js_index(strv(keys[i]), &v))
                continue;
            int j = k++;
            while (j > 0) {
                uint32_t pv;
                js_index(strv(sorted[j - 1]), &pv);
                if (pv <= v)
                    break;
                sorted[j] = sorted[j - 1];
                j--;
            }
            sorted[j] = keys[i];
        }
        for (int i = 0; i < n; i++) {
            uint32_t v;
            if (!js_index(strv(keys[i]), &v))
                sorted[k++] = keys[i];
        }
        keys = sorted;
    }
    *out = keys;
    return n;
}

jsval js_new_function(struct js *J, js_native fn, const char *name, int nargs)
{
    struct jsobj *f = js_mkobj(J, C_FUNCTION, J->function_proto);
    f->native = fn;
    f->name = name;
    f->nargs = nargs;
    return js_obj(f);
}

void js_define_native(struct js *J, struct jsobj *o, const char *name, js_native fn, int nargs)
{
    js_put(J, o, js_mkstr(J, name, (int)strlen(name)), js_new_function(J, fn, name, nargs), 1);
}

/* ---- errors ---- */

jsval js_make_error(struct js *J, const char *type, const char *msg)
{
    struct jsobj *e = js_mkobj(J, C_ERROR, J->error_proto);
    jsval ctor = js_get(J, js_obj(J->global), type);
    if (ctor.t == JS_OBJ) {
        jsval proto = js_get(J, ctor, "prototype");
        if (proto.t == JS_OBJ)
            e->proto = proto.u.o;
    }
    js_put(J, e, js_mkstr(J, "message", 7), js_str(J, msg), 1);
    return js_obj(e);
}

jsval js_throw_value(struct js *J, jsval v)
{
    J->throwing = 1;
    J->exc = v;
    return js_undefined;
}

jsval js_throw_error(struct js *J, const char *fmt, ...)
{
    char msg[400], type[32] = "Error";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    const char *text = msg;
    const char *colon = strstr(msg, ": ");
    const char *err_word = strstr(msg, "Error");
    if (colon && colon - msg < (int)sizeof(type) && err_word && err_word < colon) {
        snprintf(type, sizeof(type), "%.*s", (int)(colon - msg), msg);
        text = colon + 2;
    }
    return js_throw_value(J, js_make_error(J, type, text));
}

/* ---- comparison and primitives ---- */

int js_strict_equal(jsval a, jsval b)
{
    if (a.t != b.t)
        return 0;
    switch (a.t) {
    case JS_UNDEF:
    case JS_NULL: return 1;
    case JS_BOOL: return a.u.b == b.u.b;
    case JS_NUM: return a.u.n == b.u.n;
    case JS_STR: return str_eq(a.u.s, b.u.s);
    default: return a.u.o == b.u.o;
    }
}

jsval js_to_primitive(struct js *J, jsval v, int hint_string)
{
    if (v.t != JS_OBJ)
        return v;
    struct jsobj *o = v.u.o;
    if (o->cls == C_DATE && !hint_string)
        hint_string = 1;
    const char *order[2] = { hint_string ? "toString" : "valueOf", hint_string ? "valueOf" : "toString" };
    for (int i = 0; i < 2; i++) {
        jsval f = js_get(J, v, order[i]);
        if (J->throwing)
            return js_undefined;
        if (js_is_callable(f)) {
            jsval r = js_call_value(J, f, v, NULL, 0);
            if (J->throwing || r.t != JS_OBJ)
                return r;
        }
    }
    js_throw_error(J, "TypeError: cannot convert object to primitive value");
    return js_undefined;
}

int js_loose_equal(struct js *J, jsval a, jsval b)
{
    if (a.t == b.t)
        return js_strict_equal(a, b);
    if ((a.t == JS_NULL || a.t == JS_UNDEF) && (b.t == JS_NULL || b.t == JS_UNDEF))
        return 1;
    if (a.t == JS_NULL || a.t == JS_UNDEF || b.t == JS_NULL || b.t == JS_UNDEF)
        return 0;
    if (a.t == JS_OBJ)
        a = js_to_primitive(J, a, 0);
    if (b.t == JS_OBJ)
        b = js_to_primitive(J, b, 0);
    if (J->throwing)
        return 0;
    if (a.t == JS_STR && b.t == JS_STR)
        return str_eq(a.u.s, b.u.s);
    return js_to_number(J, a) == js_to_number(J, b);
}

/* ---- the lexer ---- */

enum { K_EOF, K_NUM, K_STR, K_TEMPLATE, K_IDENT, K_PUNCT, K_REGEX };

struct tok {
    uint8_t kind;
    uint8_t nl;                         /* a line break came before it */
    int line;
    const char *p;                      /* source text */
    int len;
    double num;
    struct jsstr *str;                  /* strings: the value; regex: the pattern */
    struct jsstr *flags;                /* regex */
};

struct lexer {
    struct js *J;
    const char *s, *end;
    int line;
    struct tok *toks;
    int n, cap;
    const char *error;
    int error_line;
};

static const char *const puncts[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
    "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=",
    "%=", "&=", "|=", "^=", "**", "<<", ">>",
    "{", "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/", "%", "&", "|",
    "^", "!", "~", "?", ":", "=", ".", NULL,
};

static int tok_is(const struct tok *t, const char *s)
{
    return (t->kind == K_PUNCT || t->kind == K_IDENT) && t->len == (int)strlen(s) &&
           !memcmp(t->p, s, (size_t)t->len);
}

static const char *const keywords[] = {
    "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete",
    "do", "else", "export", "extends", "finally", "for", "function", "if", "import", "in",
    "instanceof", "let", "new", "return", "super", "switch", "this", "throw", "try", "typeof",
    "var", "void", "while", "with", "yield", "null", "true", "false", NULL,
};

static int is_keyword(const struct tok *t)
{
    if (t->kind != K_IDENT)
        return 0;
    for (int i = 0; keywords[i]; i++)
        if (tok_is(t, keywords[i]))
            return 1;
    return 0;
}

static int ident_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}

static int ident_char(int c)
{
    return ident_start(c) || (c >= '0' && c <= '9');
}

static void put_utf8(char *out, int *n, int cp)
{
    *n += utf8_put(out + *n, cp);
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* Decode the escapes of a string body s..end (no quotes) into a new string. */
static struct jsstr *cook(struct js *J, const char *s, const char *end)
{
    char *out = malloc((size_t)(end - s) + 4);
    int n = 0;
    if (!out)
        return js_mkstr(J, "", 0);
    while (s < end) {
        char c = *s++;
        if (c != '\\' || s >= end) {
            out[n++] = c;
            continue;
        }
        c = *s++;
        switch (c) {
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'r': out[n++] = '\r'; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case 'v': out[n++] = '\v'; break;
        case '0':
            if (s < end && *s >= '0' && *s <= '7') {    /* legacy octal */
                int v = 0;
                s--;
                for (int k = 0; k < 3 && s < end && *s >= '0' && *s <= '7'; k++)
                    v = v * 8 + (*s++ - '0');
                put_utf8(out, &n, v);
            } else {
                out[n++] = 0;
            }
            break;
        case 'x':
            if (end - s >= 2 && hexval(s[0]) >= 0 && hexval(s[1]) >= 0) {
                put_utf8(out, &n, hexval(s[0]) * 16 + hexval(s[1]));
                s += 2;
            } else {
                out[n++] = 'x';
            }
            break;
        case 'u': {
            int cp = -1;
            if (s < end && *s == '{') {
                const char *q = s + 1;
                int v = 0;
                while (q < end && hexval(*q) >= 0)
                    v = v * 16 + hexval(*q++);
                if (q < end && *q == '}') {
                    cp = v;
                    s = q + 1;
                }
            } else if (end - s >= 4 && hexval(s[0]) >= 0 && hexval(s[1]) >= 0 &&
                       hexval(s[2]) >= 0 && hexval(s[3]) >= 0) {
                cp = hexval(s[0]) << 12 | hexval(s[1]) << 8 | hexval(s[2]) << 4 | hexval(s[3]);
                s += 4;
                /* a surrogate pair: one code point */
                if (cp >= 0xD800 && cp < 0xDC00 && end - s >= 6 && s[0] == '\\' && s[1] == 'u') {
                    int lo = 0, ok = 1;
                    for (int k = 2; k < 6; k++)
                        ok &= hexval(s[k]) >= 0, lo = lo * 16 + (hexval(s[k]) >= 0 ? hexval(s[k]) : 0);
                    if (ok && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        s += 6;
                    }
                }
            }
            if (cp >= 0)
                put_utf8(out, &n, cp);
            else
                out[n++] = 'u';
            break;
        }
        case '\r':
            if (s < end && *s == '\n')
                s++;
            break;
        case '\n':
            break;                      /* line continuation */
        default:
            out[n++] = c;
            break;
        }
    }
    struct jsstr *r = js_mkstr(J, out, n);
    free(out);
    return r;
}

static struct tok *new_tok(struct lexer *L)
{
    if (L->n == L->cap) {
        int cap = L->cap ? L->cap * 2 : 256;
        struct tok *nt = realloc(L->toks, sizeof(struct tok) * (size_t)cap);
        if (!nt)
            return NULL;
        L->toks = nt;
        L->cap = cap;
    }
    struct tok *t = &L->toks[L->n++];
    memset(t, 0, sizeof(*t));
    return t;
}

/* After this token, a '/' starts a regular expression rather than dividing. */
static int regex_allowed(const struct tok *prev)
{
    if (!prev)
        return 1;
    if (prev->kind == K_NUM || prev->kind == K_STR || prev->kind == K_TEMPLATE || prev->kind == K_REGEX)
        return 0;
    if (prev->kind == K_IDENT)
        return is_keyword(prev) && !tok_is(prev, "this") && !tok_is(prev, "null") &&
               !tok_is(prev, "true") && !tok_is(prev, "false") && !tok_is(prev, "super");
    return !tok_is(prev, ")") && !tok_is(prev, "]") && !tok_is(prev, "}");
}

static int lex(struct lexer *L)
{
    int nl = 0;
    while (L->s < L->end) {
        const char *s = L->s;
        char c = *s;
        if (c == '\n') {
            L->line++;
            nl = 1;
            L->s++;
            continue;
        }
        if (is_space((unsigned char)c) || (unsigned char)c == 0xA0) {
            L->s++;
            continue;
        }
        if ((unsigned char)c == 0xEF && L->end - s >= 3 && (unsigned char)s[1] == 0xBB &&
            (unsigned char)s[2] == 0xBF) {      /* byte order mark */
            L->s += 3;
            continue;
        }
        if (c == '/' && s + 1 < L->end && s[1] == '/') {
            while (L->s < L->end && *L->s != '\n')
                L->s++;
            continue;
        }
        if (c == '<' && L->end - s >= 4 && !memcmp(s, "<!--", 4)) {    /* HTML comment in a script */
            while (L->s < L->end && *L->s != '\n')
                L->s++;
            continue;
        }
        if (c == '-' && L->end - s >= 3 && !memcmp(s, "-->", 3) && (nl || L->n == 0)) {
            while (L->s < L->end && *L->s != '\n')
                L->s++;
            continue;
        }
        if (c == '/' && s + 1 < L->end && s[1] == '*') {
            const char *q = s + 2;
            while (q + 1 < L->end && !(q[0] == '*' && q[1] == '/')) {
                if (*q == '\n') {
                    L->line++;
                    nl = 1;
                }
                q++;
            }
            L->s = q + 2 <= L->end ? q + 2 : L->end;
            continue;
        }
        struct tok *t = new_tok(L);
        if (!t) {
            L->error = "out of memory";
            return -1;
        }
        t->nl = (uint8_t)nl;
        nl = 0;
        t->line = L->line;
        t->p = s;
        struct tok *prev = L->n > 1 ? &L->toks[L->n - 2] : NULL;
        if ((c >= '0' && c <= '9') || (c == '.' && s + 1 < L->end && s[1] >= '0' && s[1] <= '9')) {
            int used;
            t->kind = K_NUM;
            /* 1_000_000: read the digits without the underscores */
            const char *run = s;
            while (run < L->end && (ident_char((unsigned char)*run) || *run == '.'))
                run++;
            char clean[80];
            int nclean = 0, has_sep = 0;
            for (const char *q = s; q < run && nclean < (int)sizeof(clean) - 1; q++) {
                if (*q == '_')
                    has_sep = 1;
                else
                    clean[nclean++] = *q;
            }
            clean[nclean] = 0;
            int used_clean = 0;
            if (has_sep && run - s < (int)sizeof(clean)) {
                double v = parse_number(clean, nclean, &used_clean);
                if (used_clean == nclean) {
                    t->num = v;
                    t->len = (int)(run - s);
                    L->s = run;
                    continue;
                }
            }
            t->num = parse_number(s, (int)(L->end - s), &used);
            if (!used) {
                L->error = "bad number";
                L->error_line = L->line;
                return -1;
            }
            /* legacy octal like 0755 */
            if (s[0] == '0' && used > 1 && s[1] >= '0' && s[1] <= '7') {
                int k = 1;
                double v = 0;
                while (s + k < L->end && s[k] >= '0' && s[k] <= '7')
                    v = v * 8 + (s[k++] - '0');
                if (s + k >= L->end || !(s[k] >= '8' && s[k] <= '9') ) {
                    t->num = v;
                    used = k;
                }
            }
            if (s + used < L->end && s[used] == 'n')
                used++;                 /* BigInt: read as a number */
            t->len = used;
            L->s = s + used;
            continue;
        }
        if (ident_start((unsigned char)c) || c == '\\' ||
            (c == '#' && s + 1 < L->end && ident_start((unsigned char)s[1]))) {
            const char *q = s + (c == '#');     /* #name: a private member, an ordinary name here */
            while (q < L->end && (ident_char((unsigned char)*q) || *q == '\\'))
                q++;
            t->kind = K_IDENT;
            t->len = (int)(q - s);
            L->s = q;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char *q = s + 1;
            while (q < L->end && *q != c && *q != '\n') {
                if (*q == '\\' && q + 1 < L->end) {
                    if (q[1] == '\n')
                        L->line++;
                    q++;
                }
                q++;
            }
            if (q >= L->end || *q != c) {
                L->error = "unterminated string";
                L->error_line = L->line;
                return -1;
            }
            t->kind = K_STR;
            t->str = cook(L->J, s + 1, q);
            t->len = (int)(q + 1 - s);
            L->s = q + 1;
            continue;
        }
        if (c == '`') {
            const char *q = s + 1;
            int depth = 0;
            while (q < L->end) {
                if (*q == '\\') {
                    q += 2;
                    continue;
                }
                if (*q == '\n')
                    L->line++;
                if (!depth && *q == '`')
                    break;
                if (*q == '$' && q + 1 < L->end && q[1] == '{') {
                    depth++;
                    q += 2;
                    continue;
                }
                if (depth && *q == '{')
                    depth++;
                else if (depth && *q == '}')
                    depth--;
                else if (depth && (*q == '"' || *q == '\'' || *q == '`')) {
                    char quote = *q++;
                    while (q < L->end && *q != quote) {
                        if (*q == '\\')
                            q++;
                        q++;
                    }
                }
                q++;
            }
            if (q >= L->end) {
                L->error = "unterminated template literal";
                L->error_line = L->line;
                return -1;
            }
            t->kind = K_TEMPLATE;
            t->len = (int)(q + 1 - s);
            L->s = q + 1;
            continue;
        }
        if (c == '/' && regex_allowed(prev)) {
            const char *q = s + 1;
            int in_class = 0;
            while (q < L->end && *q != '\n' && (in_class || *q != '/')) {
                if (*q == '\\')
                    q++;
                else if (*q == '[')
                    in_class = 1;
                else if (*q == ']')
                    in_class = 0;
                q++;
            }
            if (q >= L->end || *q != '/') {
                L->error = "unterminated regular expression";
                L->error_line = L->line;
                return -1;
            }
            const char *f = q + 1;
            while (f < L->end && ident_char((unsigned char)*f))
                f++;
            t->kind = K_REGEX;
            t->str = js_mkstr(L->J, s + 1, (int)(q - s - 1));
            t->flags = js_mkstr(L->J, q + 1, (int)(f - q - 1));
            t->len = (int)(f - s);
            L->s = f;
            continue;
        }
        int matched = 0;
        for (int i = 0; puncts[i]; i++) {
            int len = (int)strlen(puncts[i]);
            if (L->end - s >= len && !memcmp(s, puncts[i], (size_t)len)) {
                if (len == 2 && s[0] == '?' && s[1] == '.' && L->end - s > 2 && s[2] >= '0' && s[2] <= '9')
                    continue;           /* a ? .5 : b */
                t->kind = K_PUNCT;
                t->len = len;
                L->s = s + len;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            L->error = "unexpected character";
            L->error_line = L->line;
            return -1;
        }
    }
    struct tok *t = new_tok(L);
    if (!t)
        return -1;
    t->kind = K_EOF;
    t->nl = 1;
    t->line = L->line;
    t->p = L->end;
    return 0;
}

/* ---- the syntax tree ---- */

enum {
    /* expressions */
    N_NUM, N_STR, N_TEMPLATE, N_REGEX, N_IDENT, N_THIS, N_TRUE, N_FALSE, N_NULL, N_ARRAY, N_OBJECT,
    N_PROP, N_GETTER, N_SETTER, N_SPREAD, N_HOLE, N_FUNC, N_CALL, N_NEW, N_MEMBER, N_INDEX,
    N_UNARY, N_PREINC, N_POSTINC, N_BINARY, N_AND, N_OR, N_NULLISH, N_ASSIGN, N_COND, N_SEQ,
    N_DESTRUCT, N_CLASS, N_FIELD, N_SUPER, N_AWAIT, N_YIELD, N_IMPORT, N_IMPORT_META,
    /* statements */
    S_VAR, S_EXPR, S_BLOCK, S_IF, S_FOR, S_FORIN, S_FOROF, S_WHILE, S_DOWHILE, S_RETURN,
    S_BREAK, S_CONTINUE, S_THROW, S_TRY, S_SWITCH, S_CASE, S_FUNCDECL, S_EMPTY, S_LABEL,
    S_IMPORT, S_EXPORT,
};

/* S_EXPORT's op: export var/function/class, export default <value>, export {...}, export * */
enum { EX_DECL, EX_DEFAULT, EX_LIST, EX_ALL };

enum { V_VAR, V_LET, V_CONST };

/* Binary and assignment operators */
enum {
    OP_NONE, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_EXP, OP_SHL, OP_SHR, OP_USHR, OP_BAND,
    OP_BOR, OP_BXOR, OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_LT, OP_GT, OP_LE, OP_GE, OP_INSTANCEOF,
    OP_IN, OP_AND, OP_OR, OP_NULLISH,
    /* unary */
    OP_NOT, OP_BNOT, OP_NEG, OP_PLUS, OP_TYPEOF, OP_VOID, OP_DELETE, OP_INC, OP_DEC,
};

struct fninfo {
    int prepared;
    struct jsstr **vars;                /* var-declared names, hoisted */
    int nvars;
    struct ast **funcs;                 /* function declarations at the top level */
    int nfuncs;
    int uses_args;
};

struct ast {
    uint8_t kind;
    uint8_t op;
    uint8_t flags;                      /* var kind; optional chaining; arrow; scoped block */
    int line;
    struct ast *a, *b, *c, *d;
    struct ast **list;
    int n;
    double num;
    struct jsstr *str;
    struct regex *re;
    struct jsstr *re_flags;
    struct fninfo *info;
};

#define F_OPTIONAL 1                    /* ?. */
#define F_ARROW    1                    /* functions */
#define F_REST     2                    /* the last parameter is ...rest */
#define F_SCOPED   1                    /* blocks with let, const or functions */
#define F_EXPRBODY 4                    /* an arrow function's body is an expression */
#define F_ASYNC    8                    /* async function: returns a promise, may await */
#define F_NOPARAMS 32                   /* a class static block: no parameter list */
#define F_GEN      16                   /* function*: returns a generator, may yield */
#define F_MODULE   64                   /* a module's body: runs in the module's scope */

struct parser {
    struct js *J;
    struct tok *t;
    int pos, n;
    const char *error;
    int error_line;
    int noin;                           /* no 'in' operator: a for statement's head */
    int fn_uses_args;
    const char *where;
    int in_async, in_gen;               /* parsing the body of an async function, a generator */
    struct jsstr **names;               /* the names seen so far, hashed (tok_name) */
    int names_cap, nnames;
    int module;                         /* parsing a module: import and export, top-level await */
    int fn_depth;                       /* functions the parser is inside */
    int tla;                            /* the module awaits at its top level */
};

static struct ast *mk(struct parser *P, int kind)
{
    struct ast *n = js_alloc(P->J, sizeof(*n));
    n->kind = (uint8_t)kind;
    n->line = P->t[P->pos].line;
    return n;
}

static struct tok *peek(struct parser *P)
{
    return &P->t[P->pos];
}

static struct tok *peek2(struct parser *P)
{
    return &P->t[P->pos + 1 < P->n ? P->pos + 1 : P->pos];
}

static int at(struct parser *P, const char *s)
{
    return tok_is(peek(P), s);
}

static int fail_parse(struct parser *P, const char *msg)
{
    if (!P->error) {
        P->error = msg;
        P->error_line = peek(P)->line;
    }
    return 0;
}

static int accept(struct parser *P, const char *s)
{
    if (!at(P, s))
        return 0;
    P->pos++;
    return 1;
}

static int expect(struct parser *P, const char *s)
{
    if (accept(P, s))
        return 1;
    static char msg[64];
    snprintf(msg, sizeof(msg), "expected '%s'", s);
    return fail_parse(P, msg);
}

/* A name's string: one copy for every use of the same name in a script. */
static struct jsstr *tok_name(struct parser *P, const struct tok *t)
{
    if (P->names_cap == 0 || P->nnames * 2 >= P->names_cap) {
        int cap = P->names_cap ? P->names_cap * 2 : 1024;
        struct jsstr **nt = calloc((size_t)cap, sizeof(*nt));
        if (!nt)
            return js_mkstr(P->J, t->p, t->len);
        for (int i = 0; i < P->names_cap; i++) {
            struct jsstr *x = P->names[i];
            if (!x)
                continue;
            unsigned h = 2166136261u;
            for (int k = 0; k < x->len; k++)
                h = (h ^ (unsigned char)x->s[k]) * 16777619u;
            unsigned j = h & (unsigned)(cap - 1);
            while (nt[j])
                j = (j + 1) & (unsigned)(cap - 1);
            nt[j] = x;
        }
        free(P->names);
        P->names = nt;
        P->names_cap = cap;
    }
    unsigned h = 2166136261u;
    for (int k = 0; k < t->len; k++)
        h = (h ^ (unsigned char)t->p[k]) * 16777619u;
    unsigned j = h & (unsigned)(P->names_cap - 1);
    for (struct jsstr *x; (x = P->names[j]); j = (j + 1) & (unsigned)(P->names_cap - 1))
        if (x->len == t->len && !memcmp(x->s, t->p, (size_t)t->len))
            return x;
    P->nnames++;
    return P->names[j] = js_mkstr(P->J, t->p, t->len);
}

static void list_add(struct parser *P, struct ast ***list, int *n, int *cap, struct ast *x)
{
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 4;
        struct ast **nl = js_alloc(P->J, sizeof(*nl) * (size_t)nc);
        if (*n)
            memcpy(nl, *list, sizeof(*nl) * (size_t)*n);
        *list = nl;
        *cap = nc;
    }
    (*list)[(*n)++] = x;
}

/* Automatic semicolon insertion. */
static int semicolon(struct parser *P)
{
    if (accept(P, ";"))
        return 1;
    struct tok *t = peek(P);
    if (t->kind == K_EOF || tok_is(t, "}") || t->nl)
        return 1;
    return fail_parse(P, "expected ';'");
}

static struct ast *expression(struct parser *P);
static struct ast *primary(struct parser *P);
static struct ast *call_member(struct parser *P);
static struct ast *assignment(struct parser *P);
static struct ast *statement(struct parser *P);
static struct ast *function_rest(struct parser *P, struct ast *fn, int arrow);
static struct ast *function_body(struct parser *P, struct ast *fn, int arrow);
static int arguments_list(struct parser *P, struct ast *call, int *capp);

static struct ast *parse_source_expr(struct parser *P, const char *s, int len);

static struct ast *template_literal(struct parser *P, struct tok *t)
{
    struct ast *n = mk(P, N_TEMPLATE);
    int cap = 0;
    const char *s = t->p + 1, *end = t->p + t->len - 1;
    const char *chunk = s;
    while (s <= end) {
        if (s == end || (*s == '$' && s + 1 < end && s[1] == '{')) {
            struct ast *str = mk(P, N_STR);
            str->str = cook(P->J, chunk, s);
            str->a = mk(P, N_STR);              /* the raw text, for tagged templates */
            str->a->str = js_mkstr(P->J, chunk, (int)(s - chunk));
            list_add(P, &n->list, &n->n, &cap, str);
            if (s == end)
                break;
            const char *e = s + 2;
            int depth = 1;
            while (e < end && depth) {
                if (*e == '{')
                    depth++;
                else if (*e == '}' && !--depth)
                    break;
                else if (*e == '"' || *e == '\'' || *e == '`') {
                    char q = *e++;
                    while (e < end && *e != q) {
                        if (*e == '\\')
                            e++;
                        e++;
                    }
                }
                e++;
            }
            struct ast *x = parse_source_expr(P, s + 2, (int)(e - s - 2));
            if (!x)
                return NULL;
            list_add(P, &n->list, &n->n, &cap, x);
            s = e + 1;
            chunk = s;
            continue;
        }
        if (*s == '\\')
            s++;
        s++;
    }
    return n;
}

static struct ast *array_literal(struct parser *P)
{
    struct ast *n = mk(P, N_ARRAY);
    int cap = 0;
    while (!at(P, "]")) {
        if (at(P, ",")) {
            P->pos++;
            list_add(P, &n->list, &n->n, &cap, mk(P, N_HOLE));
            continue;
        }
        struct ast *e;
        if (accept(P, "...")) {
            e = mk(P, N_SPREAD);
            e->a = assignment(P);
        } else {
            e = assignment(P);
        }
        if (!e)
            return NULL;
        list_add(P, &n->list, &n->n, &cap, e);
        if (!at(P, "]") && !expect(P, ","))
            return NULL;
    }
    P->pos++;
    return n;
}

/* An object literal's property name: a struct jsstr, or a computed expression. */
static int prop_key(struct parser *P, struct ast *prop)
{
    struct tok *t = peek(P);
    if (accept(P, "[")) {
        prop->b = assignment(P);
        return prop->b && expect(P, "]");
    }
    if (t->kind == K_IDENT) {
        prop->str = tok_name(P, t);
    } else if (t->kind == K_STR) {
        prop->str = t->str;
    } else if (t->kind == K_NUM) {
        char b[64];
        js_numtostr(t->num, b);
        prop->str = js_mkstr(P->J, b, (int)strlen(b));
    } else {
        return fail_parse(P, "bad property name");
    }
    P->pos++;
    return 1;
}

static struct ast *object_literal(struct parser *P)
{
    struct ast *n = mk(P, N_OBJECT);
    int cap = 0;
    while (!at(P, "}")) {
        struct ast *prop = mk(P, N_PROP);
        if (accept(P, "...")) {
            prop->kind = N_SPREAD;
            prop->a = assignment(P);
            if (!prop->a)
                return NULL;
        } else {
            struct tok *t = peek(P);
            int accessor = (tok_is(t, "get") || tok_is(t, "set")) &&
                           !tok_is(peek2(P), ",") && !tok_is(peek2(P), ":") &&
                           !tok_is(peek2(P), "(") && !tok_is(peek2(P), "}");
            if (accessor) {
                prop->kind = tok_is(t, "get") ? N_GETTER : N_SETTER;
                P->pos++;
            }
            int mflags = 0;
            if (tok_is(peek(P), "async") && !tok_is(peek2(P), ",") && !tok_is(peek2(P), ":") &&
                !tok_is(peek2(P), "(") && !tok_is(peek2(P), "}") && !peek2(P)->nl) {
                P->pos++;
                mflags |= F_ASYNC;
            }
            if (accept(P, "*")) {
                mflags |= F_GEN;
            }
            struct tok *key = peek(P);
            if (!prop_key(P, prop))
                return NULL;
            if (mflags && !at(P, "("))
                return fail_parse(P, "bad object literal"), NULL;
            if (at(P, "(")) {                   /* a method */
                struct ast *fn = mk(P, N_FUNC);
                fn->flags |= mflags;
                fn->str = prop->str;
                prop->a = function_rest(P, fn, 0);
            } else if (prop->kind == N_PROP && accept(P, ":")) {
                prop->a = assignment(P);
            } else if (prop->kind == N_PROP && key->kind == K_IDENT && !prop->b) {
                prop->a = mk(P, N_IDENT);       /* shorthand */
                prop->a->str = prop->str;
                if (at(P, "=")) {               /* {a = 1}: a default, in a pattern */
                    struct ast *as = mk(P, N_ASSIGN);
                    P->pos++;
                    as->a = prop->a;
                    as->b = assignment(P);
                    if (!as->b)
                        return NULL;
                    prop->a = as;
                }
            } else {
                return fail_parse(P, "bad object literal"), NULL;
            }
            if (!prop->a)
                return NULL;
        }
        list_add(P, &n->list, &n->n, &cap, prop);
        if (!at(P, "}") && !expect(P, ","))
            return NULL;
    }
    P->pos++;
    return n;
}

/* Parameters and body, after "function name" (or a method's name). */
static struct ast *function_rest(struct parser *P, struct ast *fn, int arrow)
{
    int saved_noin = P->noin;
    P->noin = 0;
    struct ast *r = function_body(P, fn, arrow);
    P->noin = saved_noin;
    return r;
}

static struct ast *function_body_inner(struct parser *P, struct ast *fn, int arrow);

static struct ast *function_body(struct parser *P, struct ast *fn, int arrow)
{
    int saved_async = P->in_async, saved_gen = P->in_gen;
    P->in_async = (fn->flags & F_ASYNC) != 0;
    P->in_gen = (fn->flags & F_GEN) != 0;
    P->fn_depth++;
    struct ast *r = function_body_inner(P, fn, arrow);
    P->fn_depth--;
    P->in_async = saved_async;
    P->in_gen = saved_gen;
    return r;
}

static struct ast *function_body_inner(struct parser *P, struct ast *fn, int arrow)
{
    int cap = 0;
    int saved_args = P->fn_uses_args;
    P->fn_uses_args = 0;
    if (fn->flags & F_NOPARAMS)
        goto body;
    if (!expect(P, "("))
        return NULL;
    while (!at(P, ")")) {
        int rest = accept(P, "...");
        struct tok *t = peek(P);
        struct ast *param;
        if (tok_is(t, "[") || tok_is(t, "{")) {
            param = mk(P, N_DESTRUCT);          /* ({a, b}, [c, d]) */
            param->b = primary(P);
            if (!param->b)
                return NULL;
        } else if (t->kind == K_IDENT) {
            param = mk(P, N_IDENT);
            param->str = tok_name(P, t);
            P->pos++;
        } else {
            return fail_parse(P, "bad parameter"), NULL;
        }
        if (accept(P, "="))
            param->a = assignment(P);
        list_add(P, &fn->list, &fn->n, &cap, param);
        if (rest) {
            fn->flags |= F_REST;
            break;
        }
        if (!at(P, ")") && !expect(P, ","))
            return NULL;
    }
    if (!expect(P, ")"))
        return NULL;
    if (arrow) {
        fn->flags |= F_ARROW;
        if (!expect(P, "=>"))
            return NULL;
        if (!at(P, "{")) {
            fn->flags |= F_EXPRBODY;
            fn->a = assignment(P);
            P->fn_uses_args = saved_args | P->fn_uses_args;
            return fn->a ? fn : NULL;
        }
    }
body:
    if (!expect(P, "{"))
        return NULL;
    struct ast *body = mk(P, S_BLOCK);
    int bcap = 0;
    while (!at(P, "}")) {
        if (peek(P)->kind == K_EOF)
            return fail_parse(P, "unexpected end of script"), NULL;
        struct ast *s = statement(P);
        if (!s)
            return NULL;
        list_add(P, &body->list, &body->n, &bcap, s);
    }
    P->pos++;
    fn->a = body;
    fn->info = js_alloc(P->J, sizeof(struct fninfo));
    fn->info->uses_args = P->fn_uses_args;
    /* Arrow functions see their surroundings' arguments. */
    P->fn_uses_args = arrow ? saved_args | P->fn_uses_args : saved_args;
    return fn;
}

/* Is the '(' at pos the start of an arrow function's parameters? */
static int arrow_ahead(struct parser *P)
{
    int depth = 0;
    for (int i = P->pos; i < P->n; i++) {
        struct tok *t = &P->t[i];
        if (tok_is(t, "(") || tok_is(t, "[") || tok_is(t, "{"))
            depth++;
        else if (tok_is(t, ")") || tok_is(t, "]") || tok_is(t, "}")) {
            if (--depth == 0)
                return i + 1 < P->n && tok_is(&P->t[i + 1], "=>");
        } else if (t->kind == K_EOF) {
            return 0;
        }
    }
    return 0;
}

/* Is the next token the end of a member's name (so "get" or "static" is the name)? */
static int name_ends(struct parser *P)
{
    struct tok *t = peek2(P);
    return tok_is(t, "(") || tok_is(t, "=") || tok_is(t, ";") || tok_is(t, "}");
}

static struct ast *this_member(struct parser *P, struct ast *field)
{
    struct ast *target;
    if (field->b) {                     /* [computed] */
        target = mk(P, N_INDEX);
        target->a = mk(P, N_THIS);
        target->b = field->b;
    } else {
        target = mk(P, N_MEMBER);
        target->a = mk(P, N_THIS);
        target->str = field->str;
    }
    return target;
}

/*
 * A class: its members are collected here, and the instance fields are
 * written into the constructor as this.x = ... statements (after the
 * super() call in a derived class), so the interpreter has no special case
 * for them. A class without a constructor gets the one the language
 * implies.
 */
static struct ast *class_expr(struct parser *P)
{
    P->pos++;                           /* class */
    struct ast *n = mk(P, N_CLASS);
    if (peek(P)->kind == K_IDENT && !at(P, "extends") && !at(P, "{")) {
        n->str = tok_name(P, peek(P));
        P->pos++;
    }
    if (accept(P, "extends")) {
        n->a = call_member(P);
        if (!n->a)
            return NULL;
    }
    if (!expect(P, "{"))
        return NULL;
    int cap = 0;
    while (!at(P, "}")) {
        if (peek(P)->kind == K_EOF)
            return fail_parse(P, "unexpected end of script"), NULL;
        if (accept(P, ";"))
            continue;
        struct ast *m = mk(P, N_PROP);
        if (tok_is(peek(P), "static") && !name_ends(P)) {
            P->pos++;
            m->op = 1;
            if (at(P, "{")) {
                struct ast *fn = mk(P, N_FUNC);
                fn->flags |= F_NOPARAMS;
                m->kind = N_FIELD;
                m->op = 2;              /* a static block, run as a method of the class */
                m->a = function_rest(P, fn, 0);
                if (!m->a)
                    return NULL;
                list_add(P, &n->list, &n->n, &cap, m);
                continue;
            }
        }
        struct tok *t = peek(P);
        if ((tok_is(t, "get") || tok_is(t, "set")) && !name_ends(P)) {
            m->kind = tok_is(t, "get") ? N_GETTER : N_SETTER;
            P->pos++;
        }
        int mflags = 0;
        if (tok_is(peek(P), "async") && !name_ends(P) && !peek2(P)->nl) {
            P->pos++;
            mflags |= F_ASYNC;
        }
        if (accept(P, "*")) {
            mflags |= F_GEN;
        }
        if (!prop_key(P, m))
            return NULL;
        if (mflags && !at(P, "("))
            return fail_parse(P, "bad class member"), NULL;
        if (at(P, "(")) {
            struct ast *fn = mk(P, N_FUNC);
            fn->flags |= mflags;
            fn->str = m->str;
            m->a = function_rest(P, fn, 0);
            if (!m->a)
                return NULL;
            if (!m->op && m->kind == N_PROP && !m->b && m->str && !strcmp(m->str->s, "constructor")) {
                n->b = m->a;
                continue;
            }
        } else if (m->kind == N_PROP) {
            m->kind = N_FIELD;
            if (accept(P, "=") && !(m->a = assignment(P)))
                return NULL;
            if (!semicolon(P))
                return NULL;
        } else {
            return fail_parse(P, "bad class member"), NULL;
        }
        list_add(P, &n->list, &n->n, &cap, m);
    }
    P->pos++;

    /* The instance fields, as statements for the constructor. */
    struct ast **inits = NULL;
    int ninit = 0, icap = 0;
    for (int i = 0; i < n->n; i++) {
        struct ast *m = n->list[i];
        if (m->kind != N_FIELD || m->op)
            continue;
        struct ast *as = mk(P, N_ASSIGN);
        as->a = this_member(P, m);
        as->b = m->a;
        if (!as->b) {
            as->b = mk(P, N_UNARY);
            as->b->op = OP_VOID;
            as->b->a = mk(P, N_NUM);
        }
        struct ast *st = mk(P, S_EXPR);
        st->a = as;
        list_add(P, &inits, &ninit, &icap, st);
    }
    if (!n->b) {
        struct ast *fn = mk(P, N_FUNC);
        fn->str = n->str;
        fn->info = js_alloc(P->J, sizeof(struct fninfo));
        struct ast *body = mk(P, S_BLOCK);
        if (n->a) {                     /* constructor(...args) { super(...args); } */
            struct ast *param = mk(P, N_IDENT), *use = mk(P, N_IDENT);
            param->str = use->str = js_mkstr(P->J, "args", 4);
            fn->list = js_alloc(P->J, sizeof(struct ast *));
            fn->list[0] = param;
            fn->n = 1;
            fn->flags |= F_REST;
            struct ast *call = mk(P, N_CALL), *spread = mk(P, N_SPREAD), *st = mk(P, S_EXPR);
            call->a = mk(P, N_SUPER);
            spread->a = use;
            call->list = js_alloc(P->J, sizeof(struct ast *));
            call->list[0] = spread;
            call->n = 1;
            st->a = call;
            int bcap = 0;
            list_add(P, &body->list, &body->n, &bcap, st);
        }
        fn->a = body;
        n->b = fn;
    }
    struct ast *body = n->b->a;
    if (ninit) {
        int at_i = 0;                   /* after super(...) in a derived class */
        if (n->a) {
            at_i = -1;
            for (int i = 0; i < body->n && at_i < 0; i++)
                if (body->list[i]->kind == S_EXPR && body->list[i]->a->kind == N_CALL &&
                    body->list[i]->a->a->kind == N_SUPER)
                    at_i = i + 1;
        }
        if (at_i >= 0) {
            struct ast **nl = js_alloc(P->J, sizeof(*nl) * (size_t)(body->n + ninit + 1));
            if (body->n)
                memcpy(nl, body->list, sizeof(*nl) * (size_t)at_i);
            memcpy(nl + at_i, inits, sizeof(*nl) * (size_t)ninit);
            if (body->n > at_i)
                memcpy(nl + at_i + ninit, body->list + at_i, sizeof(*nl) * (size_t)(body->n - at_i));
            body->list = nl;
            body->n += ninit;
        }
    }
    return n;
}

static struct ast *primary(struct parser *P)
{
    struct tok *t = peek(P);
    struct ast *n;
    switch (t->kind) {
    case K_NUM:
        n = mk(P, N_NUM);
        n->num = t->num;
        P->pos++;
        return n;
    case K_STR:
        n = mk(P, N_STR);
        n->str = t->str;
        P->pos++;
        return n;
    case K_TEMPLATE:
        P->pos++;
        return template_literal(P, t);
    case K_REGEX: {
        const char *err = NULL;
        n = mk(P, N_REGEX);
        n->str = t->str;
        n->re_flags = t->flags;
        n->re = re_compile(P->J, t->str->s, t->str->len, t->flags->s, &err);
        if (!n->re) {                   /* the script runs; evaluating this literal throws */
            n->a = mk(P, N_STR);
            n->a->str = js_mkstr(P->J, err ? err : "bad regular expression", err ? (int)strlen(err) : 22);
        }
        P->pos++;
        return n;
    }
    case K_IDENT:
        if (tok_is(t, "function") || (tok_is(t, "async") && tok_is(peek2(P), "function") && !peek2(P)->nl)) {
            int async = tok_is(t, "async");
            P->pos += async ? 2 : 1;
            n = mk(P, N_FUNC);
            if (async)
                n->flags |= F_ASYNC;
            if (accept(P, "*")) {
                n->flags |= F_GEN;
            }
            if (peek(P)->kind == K_IDENT && !at(P, "(")) {
                n->str = tok_name(P, peek(P));
                n->flags |= 0;
                P->pos++;
                n->b = n;               /* named expression: binds its own name */
            }
            return function_rest(P, n, 0);
        }
        if (tok_is(t, "this")) {
            P->pos++;
            return mk(P, N_THIS);
        }
        if (tok_is(t, "true") || tok_is(t, "false") || tok_is(t, "null")) {
            P->pos++;
            return mk(P, tok_is(t, "true") ? N_TRUE : tok_is(t, "false") ? N_FALSE : N_NULL);
        }
        if (tok_is(t, "class"))
            return class_expr(P);
        if (tok_is(t, "import") && tok_is(peek2(P), "(")) {      /* import(specifier) */
            P->pos += 2;
            n = mk(P, N_IMPORT);
            n->str = js_mkstr(P->J, P->where, (int)strlen(P->where));
            int saved = P->noin;
            P->noin = 0;
            n->a = assignment(P);
            if (n->a && accept(P, ",") && !at(P, ")") && !assignment(P))    /* options */
                n->a = NULL;
            accept(P, ",");
            P->noin = saved;
            return n->a && expect(P, ")") ? n : NULL;
        }
        if (tok_is(t, "import") && tok_is(peek2(P), ".")) {      /* import.meta */
            P->pos += 2;
            if (!accept(P, "meta"))
                return fail_parse(P, "expected import.meta"), NULL;
            if (!P->module)
                return fail_parse(P, "import.meta is only valid in a module"), NULL;
            return mk(P, N_IMPORT_META);
        }
        if (tok_is(t, "super") && (tok_is(peek2(P), "(") || tok_is(peek2(P), ".") || tok_is(peek2(P), "["))) {
            P->pos++;
            return mk(P, N_SUPER);
        }
        if (is_keyword(t) && !tok_is(t, "let") && !tok_is(t, "yield"))
            return fail_parse(P, "unexpected keyword"), NULL;
        n = mk(P, N_IDENT);
        n->str = tok_name(P, t);
        if (tok_is(t, "arguments"))
            P->fn_uses_args = 1;
        P->pos++;
        return n;
    case K_PUNCT: {
        /* Inside brackets, 'in' is an operator again, even in a for statement's head. */
        int saved = P->noin;
        P->noin = 0;
        n = NULL;
        if (accept(P, "(")) {
            n = expression(P);
            if (n && !expect(P, ")"))
                n = NULL;
        } else if (accept(P, "[")) {
            n = array_literal(P);
        } else if (accept(P, "{")) {
            n = object_literal(P);
        } else {
            P->noin = saved;
            break;
        }
        P->noin = saved;
        return n;
    }
    default:
        break;
    }
    return fail_parse(P, t->kind == K_EOF ? "unexpected end of script" : "unexpected token"), NULL;
}

static int arguments(struct parser *P, struct ast *call)
{
    int cap = 0, saved = P->noin, ok = 0;
    P->noin = 0;
    ok = arguments_list(P, call, &cap);
    P->noin = saved;
    return ok;
}

static int arguments_list(struct parser *P, struct ast *call, int *capp)
{
    int cap = *capp;
    while (!at(P, ")")) {
        struct ast *e;
        if (accept(P, "...")) {
            e = mk(P, N_SPREAD);
            e->a = assignment(P);
            if (!e->a)
                return 0;
        } else {
            e = assignment(P);
        }
        if (!e)
            return 0;
        list_add(P, &call->list, &call->n, &cap, e);
        if (!at(P, ")") && !expect(P, ","))
            return 0;
    }
    P->pos++;
    *capp = cap;
    return 1;
}

static struct ast *member_name(struct parser *P, struct ast *obj, int optional)
{
    struct tok *t = peek(P);
    if (t->kind != K_IDENT)
        return fail_parse(P, "expected a property name"), NULL;
    struct ast *m = mk(P, N_MEMBER);
    m->a = obj;
    m->str = tok_name(P, t);
    m->flags = (uint8_t)(optional ? F_OPTIONAL : 0);
    P->pos++;
    return m;
}

static struct ast *call_member(struct parser *P)
{
    struct ast *e;
    if (accept(P, "new")) {
        if (accept(P, ".")) {               /* new.target */
            P->pos++;
            return mk(P, N_NULL);
        }
        struct ast *callee = NULL;
        if (at(P, "new"))
            callee = call_member(P);
        else
            callee = primary(P);
        while (callee) {                    /* member accesses, not calls */
            if (accept(P, "."))
                callee = member_name(P, callee, 0);
            else if (accept(P, "[")) {
                struct ast *ix = mk(P, N_INDEX);
                ix->a = callee;
                ix->b = expression(P);
                if (!ix->b || !expect(P, "]"))
                    return NULL;
                callee = ix;
            } else
                break;
        }
        if (!callee)
            return NULL;
        e = mk(P, N_NEW);
        e->a = callee;
        if (accept(P, "(") && !arguments(P, e))
            return NULL;
    } else {
        e = primary(P);
    }
    while (e) {
        if (accept(P, ".")) {
            e = member_name(P, e, 0);
        } else if (accept(P, "?.")) {
            if (accept(P, "(")) {
                struct ast *c = mk(P, N_CALL);
                c->a = e;
                c->flags = F_OPTIONAL;
                if (!arguments(P, c))
                    return NULL;
                e = c;
            } else if (accept(P, "[")) {
                struct ast *ix = mk(P, N_INDEX);
                ix->a = e;
                ix->flags = F_OPTIONAL;
                ix->b = expression(P);
                if (!ix->b || !expect(P, "]"))
                    return NULL;
                e = ix;
            } else {
                e = member_name(P, e, 1);
            }
        } else if (accept(P, "[")) {
            struct ast *ix = mk(P, N_INDEX);
            int saved = P->noin;
            P->noin = 0;
            ix->a = e;
            ix->b = expression(P);
            P->noin = saved;
            if (!ix->b || !expect(P, "]"))
                return NULL;
            e = ix;
        } else if (accept(P, "(")) {
            struct ast *c = mk(P, N_CALL);
            c->a = e;
            if (!arguments(P, c))
                return NULL;
            e = c;
        } else if (peek(P)->kind == K_TEMPLATE) {      /* a tagged template: called with the text */
            struct ast *c = mk(P, N_CALL);
            c->a = e;
            struct ast *tl = template_literal(P, peek(P));
            P->pos++;
            if (!tl)
                return NULL;
            c->list = js_alloc(P->J, sizeof(struct ast *));
            c->list[0] = tl;
            c->n = 1;
            c->op = 1;                          /* tagged: called with the strings, then the values */
            e = c;
        } else {
            break;
        }
    }
    return e;
}

static struct ast *postfix(struct parser *P)
{
    struct ast *e = call_member(P);
    if (!e)
        return NULL;
    struct tok *t = peek(P);
    if ((tok_is(t, "++") || tok_is(t, "--")) && !t->nl) {
        if (e->kind != N_IDENT && e->kind != N_MEMBER && e->kind != N_INDEX)
            return fail_parse(P, "invalid increment target"), NULL;
        struct ast *u = mk(P, N_POSTINC);
        u->op = tok_is(t, "++") ? OP_INC : OP_DEC;
        u->a = e;
        P->pos++;
        return u;
    }
    return e;
}

static struct ast *unary(struct parser *P)
{
    struct tok *t = peek(P);
    static const struct { const char *s; int op; } ops[] = {
        { "!", OP_NOT }, { "~", OP_BNOT }, { "-", OP_NEG }, { "+", OP_PLUS },
        { "typeof", OP_TYPEOF }, { "void", OP_VOID }, { "delete", OP_DELETE },
    };
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (tok_is(t, ops[i].s)) {
            P->pos++;
            struct ast *u = mk(P, N_UNARY);
            u->op = (uint8_t)ops[i].op;
            u->a = unary(P);
            return u->a ? u : NULL;
        }
    if (tok_is(t, "++") || tok_is(t, "--")) {
        P->pos++;
        struct ast *u = mk(P, N_PREINC);
        u->op = tok_is(t, "++") ? OP_INC : OP_DEC;
        u->a = unary(P);
        if (!u->a)
            return NULL;
        if (u->a->kind != N_IDENT && u->a->kind != N_MEMBER && u->a->kind != N_INDEX)
            return fail_parse(P, "invalid increment target"), NULL;
        return u;
    }
    if (P->in_async && tok_is(t, "await")) {
        P->pos++;
        if (!P->fn_depth)
            P->tla = 1;
        struct ast *u = mk(P, N_AWAIT);
        u->a = unary(P);
        return u->a ? u : NULL;
    }
    return postfix(P);
}

static const struct binop {
    const char *s;
    int prec, op, kind;
} binops[] = {
    { "??", 1, OP_NULLISH, N_NULLISH }, { "||", 2, OP_OR, N_OR }, { "&&", 3, OP_AND, N_AND },
    { "|", 4, OP_BOR, N_BINARY }, { "^", 5, OP_BXOR, N_BINARY }, { "&", 6, OP_BAND, N_BINARY },
    { "===", 7, OP_SEQ, N_BINARY }, { "!==", 7, OP_SNE, N_BINARY }, { "==", 7, OP_EQ, N_BINARY },
    { "!=", 7, OP_NE, N_BINARY }, { "<=", 8, OP_LE, N_BINARY }, { ">=", 8, OP_GE, N_BINARY },
    { "<", 8, OP_LT, N_BINARY }, { ">", 8, OP_GT, N_BINARY },
    { "instanceof", 8, OP_INSTANCEOF, N_BINARY }, { "in", 8, OP_IN, N_BINARY },
    { ">>>", 9, OP_USHR, N_BINARY }, { "<<", 9, OP_SHL, N_BINARY }, { ">>", 9, OP_SHR, N_BINARY },
    { "+", 10, OP_ADD, N_BINARY }, { "-", 10, OP_SUB, N_BINARY }, { "*", 11, OP_MUL, N_BINARY },
    { "/", 11, OP_DIV, N_BINARY }, { "%", 11, OP_MOD, N_BINARY }, { "**", 12, OP_EXP, N_BINARY },
    { NULL, 0, 0, 0 },
};

static const struct binop *binop_at(struct parser *P)
{
    struct tok *t = peek(P);
    if (t->kind != K_PUNCT && t->kind != K_IDENT)
        return NULL;
    for (int i = 0; binops[i].s; i++)
        if (tok_is(t, binops[i].s)) {
            if (binops[i].op == OP_IN && P->noin)
                return NULL;
            return &binops[i];
        }
    return NULL;
}

static struct ast *binary(struct parser *P, int minprec)
{
    struct ast *left = unary(P);
    while (left) {
        const struct binop *b = binop_at(P);
        if (!b || b->prec < minprec)
            break;
        P->pos++;
        /* ** is right-associative */
        struct ast *right = binary(P, b->op == OP_EXP ? b->prec : b->prec + 1);
        if (!right)
            return NULL;
        struct ast *n = mk(P, b->kind);
        n->op = (uint8_t)b->op;
        n->a = left;
        n->b = right;
        left = n;
    }
    return left;
}

static struct ast *conditional(struct parser *P)
{
    struct ast *c = binary(P, 1);
    if (!c || !accept(P, "?"))
        return c;
    struct ast *n = mk(P, N_COND);
    int saved = P->noin;
    P->noin = 0;
    n->a = c;
    n->b = assignment(P);
    P->noin = saved;
    if (!n->b || !expect(P, ":"))
        return NULL;
    n->c = assignment(P);
    return n->c ? n : NULL;
}

static const struct { const char *s; int op; } assignops[] = {
    { "=", OP_NONE }, { "+=", OP_ADD }, { "-=", OP_SUB }, { "*=", OP_MUL }, { "/=", OP_DIV },
    { "%=", OP_MOD }, { "**=", OP_EXP }, { "<<=", OP_SHL }, { ">>=", OP_SHR }, { ">>>=", OP_USHR },
    { "&=", OP_BAND }, { "|=", OP_BOR }, { "^=", OP_BXOR }, { "&&=", OP_AND }, { "||=", OP_OR },
    { "?\?=", OP_NULLISH }, { NULL, 0 },
};

static struct ast *assignment(struct parser *P)
{
    struct tok *t = peek(P);
    int async_arrow = 0;
    if (tok_is(t, "async") && !peek2(P)->nl) {
        struct tok *t2 = peek2(P);
        if (t2->kind == K_IDENT && !is_keyword(t2) &&
            tok_is(&P->t[P->pos + 2 < P->n ? P->pos + 2 : P->pos], "=>")) {
            P->pos++;                   /* async x => ... */
            t = peek(P);
            async_arrow = 1;
        } else if (tok_is(t2, "(")) {
            P->pos++;
            if (arrow_ahead(P)) {       /* async (a, b) => ... */
                struct ast *fn = mk(P, N_FUNC);
                fn->flags |= F_ASYNC;
                return function_rest(P, fn, 1);
            }
            P->pos--;                   /* a call to something named async */
        }
    }
    if (P->in_gen && tok_is(t, "yield")) {
        P->pos++;
        struct ast *y = mk(P, N_YIELD);
        if (accept(P, "*")) {
            y->op = 1;                  /* yield*: hand over to another iterable */
            y->a = assignment(P);
            return y->a ? y : NULL;
        }
        struct tok *nt = peek(P);
        if (!(nt->nl || nt->kind == K_EOF || tok_is(nt, ")") || tok_is(nt, "]") || tok_is(nt, "}") ||
              tok_is(nt, ",") || tok_is(nt, ";") || tok_is(nt, ":"))) {
            y->a = assignment(P);
            if (!y->a)
                return NULL;
        }
        return y;
    }
    if (t->kind == K_IDENT && !is_keyword(t) && tok_is(peek2(P), "=>")) {
        struct ast *fn = mk(P, N_FUNC);
        if (async_arrow)
            fn->flags |= F_ASYNC;
        int saved_async = P->in_async, saved_gen = P->in_gen;
        P->in_async = async_arrow;
        P->in_gen = 0;
        struct ast *param = mk(P, N_IDENT);
        param->str = tok_name(P, t);
        fn->list = js_alloc(P->J, sizeof(struct ast *));
        fn->list[0] = param;
        fn->n = 1;
        P->pos += 2;
        fn->flags |= F_ARROW;
        if (at(P, "{")) {
            /* reuse the body parser: pretend the parameters were already read */
            int saved_args = P->fn_uses_args, saved_noin = P->noin;
            P->fn_uses_args = 0;
            P->noin = 0;
            P->pos++;
            struct ast *body = mk(P, S_BLOCK);
            int bcap = 0;
            while (!at(P, "}")) {
                if (peek(P)->kind == K_EOF)
                    return fail_parse(P, "unexpected end of script"), NULL;
                struct ast *s = statement(P);
                if (!s)
                    return NULL;
                list_add(P, &body->list, &body->n, &bcap, s);
            }
            P->pos++;
            fn->a = body;
            fn->info = js_alloc(P->J, sizeof(struct fninfo));
            P->fn_uses_args = saved_args | P->fn_uses_args;
            P->noin = saved_noin;
            P->in_async = saved_async;
            P->in_gen = saved_gen;
            return fn;
        }
        fn->flags |= F_EXPRBODY;
        fn->a = assignment(P);
        P->in_async = saved_async;
        P->in_gen = saved_gen;
        return fn->a ? fn : NULL;
    }
    if (tok_is(t, "(") && arrow_ahead(P)) {
        struct ast *fn = mk(P, N_FUNC);
        return function_rest(P, fn, 1);
    }
    struct ast *left = conditional(P);
    if (!left)
        return NULL;
    for (int i = 0; assignops[i].s; i++) {
        if (!at(P, assignops[i].s))
            continue;
        if ((left->kind == N_ARRAY || left->kind == N_OBJECT) && assignops[i].op != OP_NONE)
            return fail_parse(P, "invalid assignment target"), NULL;
        if (left->kind != N_ARRAY && left->kind != N_OBJECT && left->kind != N_IDENT &&
            left->kind != N_MEMBER && left->kind != N_INDEX)
            return fail_parse(P, "invalid assignment target"), NULL;
        P->pos++;
        struct ast *n = mk(P, N_ASSIGN);
        n->op = (uint8_t)assignops[i].op;
        n->a = left;
        n->b = assignment(P);
        return n->b ? n : NULL;
    }
    return left;
}

static struct ast *expression(struct parser *P)
{
    struct ast *e = assignment(P);
    if (!e || !at(P, ","))
        return e;
    struct ast *seq = mk(P, N_SEQ);
    int cap = 0;
    list_add(P, &seq->list, &seq->n, &cap, e);
    while (accept(P, ",")) {
        e = assignment(P);
        if (!e)
            return NULL;
        list_add(P, &seq->list, &seq->n, &cap, e);
    }
    return seq;
}

static struct ast *var_declaration(struct parser *P, int kind)
{
    struct ast *n = mk(P, S_VAR);
    n->flags = (uint8_t)kind;
    int cap = 0;
    do {
        struct tok *t = peek(P);
        struct ast *d;
        if (tok_is(t, "[") || tok_is(t, "{")) {
            d = mk(P, N_DESTRUCT);              /* const {a, b} = x; let [c, d] = y */
            d->b = primary(P);
            if (!d->b)
                return NULL;
            if (!at(P, "=") && !(P->noin == 1 && (at(P, "in") || at(P, "of"))))
                return fail_parse(P, "a destructuring declaration needs a value"), NULL;
        } else {
            if (t->kind != K_IDENT || (is_keyword(t) && !tok_is(t, "let")))
                return fail_parse(P, "expected a variable name"), NULL;
            d = mk(P, N_IDENT);
            d->str = tok_name(P, t);
            P->pos++;
        }
        if (accept(P, "=")) {
            d->a = assignment(P);
            if (!d->a)
                return NULL;
        }
        list_add(P, &n->list, &n->n, &cap, d);
    } while (accept(P, ","));
    return n;
}

static int var_keyword(struct parser *P)
{
    struct tok *t = peek(P);
    if (tok_is(t, "var"))
        return V_VAR + 1;
    if (tok_is(t, "const"))
        return V_CONST + 1;
    /* "let" is a keyword only before a name */
    if (tok_is(t, "let") && (peek2(P)->kind == K_IDENT || tok_is(peek2(P), "[") || tok_is(peek2(P), "{")))
        return V_LET + 1;
    return 0;
}

static struct ast *block(struct parser *P)
{
    struct ast *n = mk(P, S_BLOCK);
    int cap = 0;
    while (!at(P, "}")) {
        if (peek(P)->kind == K_EOF)
            return fail_parse(P, "unexpected end of script"), NULL;
        struct ast *s = statement(P);
        if (!s)
            return NULL;
        if ((s->kind == S_VAR && s->flags != V_VAR) || s->kind == S_FUNCDECL)
            n->flags |= F_SCOPED;
        list_add(P, &n->list, &n->n, &cap, s);
    }
    P->pos++;
    return n;
}

static struct ast *for_statement(struct parser *P)
{
    int await_loop = 0;
    if (tok_is(peek(P), "await")) {
        if (!P->in_async)
            return fail_parse(P, "for await is only valid in an async function"), NULL;
        P->pos++;
        await_loop = 1;
        if (!P->fn_depth)
            P->tla = 1;
    }
    if (!expect(P, "("))
        return NULL;
    struct ast *init = NULL;
    int vk = var_keyword(P);
    P->noin = 1;
    if (vk) {
        P->pos++;
        init = var_declaration(P, vk - 1);
    } else if (!at(P, ";")) {
        init = expression(P);
    }
    P->noin = 0;
    if (P->error)
        return NULL;
    if (init && (at(P, "in") || at(P, "of"))) {
        int of = at(P, "of");
        P->pos++;
        if (init->kind == S_VAR && (init->n != 1 || init->list[0]->a))
            return fail_parse(P, "bad for-in/of declaration"), NULL;
        if (init->kind != S_VAR && init->kind != N_IDENT && init->kind != N_MEMBER &&
            init->kind != N_INDEX && init->kind != N_ARRAY && init->kind != N_OBJECT)
            return fail_parse(P, "bad for-in/of target"), NULL;
        if (await_loop && !of)
            return fail_parse(P, "for await needs 'of'"), NULL;
        struct ast *n = mk(P, of ? S_FOROF : S_FORIN);
        n->op = (uint8_t)await_loop;
        n->a = init;
        n->b = of ? assignment(P) : expression(P);
        if (!n->b || !expect(P, ")"))
            return NULL;
        n->c = statement(P);
        return n->c ? n : NULL;
    }
    struct ast *n = mk(P, S_FOR);
    n->a = init;
    if (!expect(P, ";"))
        return NULL;
    if (!at(P, ";") && !(n->b = expression(P)))
        return NULL;
    if (!expect(P, ";"))
        return NULL;
    if (!at(P, ")") && !(n->c = expression(P)))
        return NULL;
    if (!expect(P, ")"))
        return NULL;
    n->d = statement(P);
    return n->d ? n : NULL;
}

static struct ast *statement(struct parser *P)
{
    struct tok *t = peek(P);
    struct ast *n;
    if (accept(P, "{"))
        return block(P);
    if (accept(P, ";"))
        return mk(P, S_EMPTY);
    int vk = var_keyword(P);
    if (vk) {
        P->pos++;
        n = var_declaration(P, vk - 1);
        return n && semicolon(P) ? n : NULL;
    }
    if (t->kind == K_IDENT && !is_keyword(t) && tok_is(peek2(P), ":")) {
        n = mk(P, S_LABEL);
        n->str = tok_name(P, t);
        P->pos += 2;
        n->a = statement(P);
        return n->a ? n : NULL;
    }
    if (tok_is(t, "function") || (tok_is(t, "async") && tok_is(peek2(P), "function") && !peek2(P)->nl)) {
        int async = tok_is(t, "async"), flags = async ? F_ASYNC : 0;
        P->pos += async ? 2 : 1;
        if (accept(P, "*")) {
            flags |= F_GEN;
        }
        struct tok *name = peek(P);
        if (name->kind != K_IDENT)
            return fail_parse(P, "expected a function name"), NULL;
        n = mk(P, S_FUNCDECL);
        struct ast *fn = mk(P, N_FUNC);
        fn->flags |= flags;
        fn->str = tok_name(P, name);
        P->pos++;
        n->a = function_rest(P, fn, 0);
        return n->a ? n : NULL;
    }
    if (tok_is(t, "class")) {
        struct ast *c = class_expr(P);
        if (!c)
            return NULL;
        if (!c->str)
            return fail_parse(P, "a class declaration needs a name"), NULL;
        n = mk(P, S_VAR);               /* class A {} is let A = class A {} */
        n->flags = V_LET;
        struct ast *d = mk(P, N_IDENT);
        d->str = c->str;
        d->a = c;
        int ncap = 0;
        list_add(P, &n->list, &n->n, &ncap, d);
        return n;
    }
    if ((tok_is(t, "import") && !tok_is(peek2(P), "(") && !tok_is(peek2(P), ".")) || tok_is(t, "export"))
        return fail_parse(P, P->module ? "import and export may only appear at the top level"
                                       : "import and export may only appear in a module"), NULL;
    if (accept(P, "if")) {
        n = mk(P, S_IF);
        if (!expect(P, "(") || !(n->a = expression(P)) || !expect(P, ")") || !(n->b = statement(P)))
            return NULL;
        if (accept(P, "else") && !(n->c = statement(P)))
            return NULL;
        return n;
    }
    if (accept(P, "for"))
        return for_statement(P);
    if (accept(P, "while")) {
        n = mk(P, S_WHILE);
        if (!expect(P, "(") || !(n->a = expression(P)) || !expect(P, ")") || !(n->b = statement(P)))
            return NULL;
        return n;
    }
    if (accept(P, "do")) {
        n = mk(P, S_DOWHILE);
        if (!(n->b = statement(P)) || !expect(P, "while") || !expect(P, "(") ||
            !(n->a = expression(P)) || !expect(P, ")"))
            return NULL;
        accept(P, ";");
        return n;
    }
    if (accept(P, "return")) {
        n = mk(P, S_RETURN);
        struct tok *nt = peek(P);
        if (!nt->nl && !tok_is(nt, ";") && !tok_is(nt, "}") && nt->kind != K_EOF &&
            !(n->a = expression(P)))
            return NULL;
        return semicolon(P) ? n : NULL;
    }
    if (tok_is(t, "break") || tok_is(t, "continue")) {
        P->pos++;
        n = mk(P, tok_is(t, "break") ? S_BREAK : S_CONTINUE);
        struct tok *nt = peek(P);
        if (nt->kind == K_IDENT && !nt->nl && !is_keyword(nt)) {
            n->str = tok_name(P, nt);
            P->pos++;
        }
        return semicolon(P) ? n : NULL;
    }
    if (accept(P, "throw")) {
        n = mk(P, S_THROW);
        if (!(n->a = expression(P)))
            return NULL;
        return semicolon(P) ? n : NULL;
    }
    if (accept(P, "try")) {
        n = mk(P, S_TRY);
        if (!expect(P, "{") || !(n->a = block(P)))
            return NULL;
        if (accept(P, "catch")) {
            if (accept(P, "(")) {
                struct tok *nt = peek(P);
                if (tok_is(nt, "[") || tok_is(nt, "{")) {
                    n->d = primary(P);          /* catch ({message}) */
                    if (!n->d)
                        return NULL;
                } else if (nt->kind == K_IDENT) {
                    n->str = tok_name(P, nt);
                    P->pos++;
                } else {
                    return fail_parse(P, "bad catch parameter"), NULL;
                }
                if (!expect(P, ")"))
                    return NULL;
            }
            if (!expect(P, "{") || !(n->b = block(P)))
                return NULL;
        }
        if (accept(P, "finally") && (!expect(P, "{") || !(n->c = block(P))))
            return NULL;
        if (!n->b && !n->c)
            return fail_parse(P, "try without catch or finally"), NULL;
        return n;
    }
    if (accept(P, "switch")) {
        n = mk(P, S_SWITCH);
        int cap = 0;
        if (!expect(P, "(") || !(n->a = expression(P)) || !expect(P, ")") || !expect(P, "{"))
            return NULL;
        while (!accept(P, "}")) {
            struct ast *c = mk(P, S_CASE);
            int ccap = 0;
            if (accept(P, "case")) {
                if (!(c->a = expression(P)))
                    return NULL;
            } else if (!accept(P, "default")) {
                return fail_parse(P, "expected 'case'"), NULL;
            }
            if (!expect(P, ":"))
                return NULL;
            while (!at(P, "case") && !at(P, "default") && !at(P, "}")) {
                if (peek(P)->kind == K_EOF)
                    return fail_parse(P, "unexpected end of script"), NULL;
                struct ast *s = statement(P);
                if (!s)
                    return NULL;
                list_add(P, &c->list, &c->n, &ccap, s);
            }
            list_add(P, &n->list, &n->n, &cap, c);
        }
        return n;
    }
    if (tok_is(t, "debugger")) {
        P->pos++;
        semicolon(P);
        return mk(P, S_EMPTY);
    }
    if (tok_is(t, "with"))
        return fail_parse(P, "with is not supported"), NULL;
    n = mk(P, S_EXPR);
    n->a = expression(P);
    if (!n->a)
        return NULL;
    return semicolon(P) ? n : NULL;
}

/* Hoisting: the var names and top-level functions of a function body. */
static void add_var(struct js *J, struct fninfo *fi, int *cap, struct jsstr *name)
{
    if (fi->nvars == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        struct jsstr **nv = js_alloc(J, sizeof(*nv) * (size_t)nc);
        if (fi->nvars)
            memcpy(nv, fi->vars, sizeof(*nv) * (size_t)fi->nvars);
        fi->vars = nv;
        *cap = nc;
    }
    fi->vars[fi->nvars++] = name;
}

/* The names a destructuring pattern declares. */
static void pattern_vars(struct js *J, struct ast *p, struct fninfo *fi, int *cap)
{
    if (!p)
        return;
    switch (p->kind) {
    case N_IDENT:
        add_var(J, fi, cap, p->str);
        break;
    case N_ASSIGN:                      /* a default */
        pattern_vars(J, p->a, fi, cap);
        break;
    case N_SPREAD:
        pattern_vars(J, p->a, fi, cap);
        break;
    case N_ARRAY:
        for (int i = 0; i < p->n; i++)
            pattern_vars(J, p->list[i], fi, cap);
        break;
    case N_OBJECT:
        for (int i = 0; i < p->n; i++)
            pattern_vars(J, p->list[i]->a, fi, cap);
        break;
    default:
        break;
    }
}

static void collect_vars(struct js *J, struct ast *s, struct fninfo *fi, int *cap)
{
    if (!s)
        return;
    switch (s->kind) {
    case S_VAR:
        if (s->flags != V_VAR)
            return;
        for (int i = 0; i < s->n; i++) {
            if (s->list[i]->kind == N_DESTRUCT)
                pattern_vars(J, s->list[i]->b, fi, cap);
            else
                add_var(J, fi, cap, s->list[i]->str);
        }
        return;
    case S_BLOCK:
        for (int i = 0; i < s->n; i++)
            collect_vars(J, s->list[i], fi, cap);
        return;
    case S_IF:
        collect_vars(J, s->b, fi, cap);
        collect_vars(J, s->c, fi, cap);
        return;
    case S_FOR:
        collect_vars(J, s->a, fi, cap);
        collect_vars(J, s->d, fi, cap);
        return;
    case S_FORIN:
    case S_FOROF:
        collect_vars(J, s->a, fi, cap);
        collect_vars(J, s->c, fi, cap);
        return;
    case S_WHILE:
    case S_DOWHILE:
        collect_vars(J, s->b, fi, cap);
        return;
    case S_TRY:
        collect_vars(J, s->a, fi, cap);
        collect_vars(J, s->b, fi, cap);
        collect_vars(J, s->c, fi, cap);
        return;
    case S_SWITCH:
        for (int i = 0; i < s->n; i++)
            for (int k = 0; k < s->list[i]->n; k++)
                collect_vars(J, s->list[i]->list[k], fi, cap);
        return;
    case S_LABEL:
        collect_vars(J, s->a, fi, cap);
        return;
    default:
        return;
    }
}

static void prepare_body(struct js *J, struct ast *body, struct fninfo *fi)
{
    int cap = 0, fcap = 0;
    fi->prepared = 1;
    for (int i = 0; i < body->n; i++) {
        struct ast *s = body->list[i];
        if (s->kind == S_EXPORT && s->op == EX_DECL)
            s = s->a;
        if (s->kind == S_FUNCDECL) {
            if (fi->nfuncs == fcap) {
                int nc = fcap ? fcap * 2 : 4;
                struct ast **nf = js_alloc(J, sizeof(*nf) * (size_t)nc);
                if (fi->nfuncs)
                    memcpy(nf, fi->funcs, sizeof(*nf) * (size_t)fi->nfuncs);
                fi->funcs = nf;
                fcap = nc;
            }
            fi->funcs[fi->nfuncs++] = s->a;
        } else {
            collect_vars(J, s, fi, &cap);
        }
    }
}

/* ---- modules: import and export declarations ---- */

/* A pair of names, a in one module (or "*", or "default") and b in the other. */
static struct ast *name_pair(struct parser *P, struct jsstr *a, struct jsstr *b)
{
    struct ast *p = mk(P, N_PROP);
    p->a = mk(P, N_STR);
    p->a->str = a;
    p->b = mk(P, N_STR);
    p->b->str = b;
    return p;
}

/* A name in an import or export list: an identifier (keywords too) or a string. */
static struct jsstr *module_name(struct parser *P)
{
    struct tok *t = peek(P);
    if (t->kind != K_IDENT && t->kind != K_STR)
        return fail_parse(P, "expected a name"), NULL;
    P->pos++;
    return t->kind == K_STR ? t->str : tok_name(P, t);
}

static struct jsstr *local_name(struct parser *P)
{
    struct tok *t = peek(P);
    if (t->kind != K_IDENT || is_keyword(t))
        return fail_parse(P, "expected a name"), NULL;
    P->pos++;
    return tok_name(P, t);
}

/* from "specifier" [with { type: "json" }]; */
static int module_from(struct parser *P, struct ast *n)
{
    if (!accept(P, "from"))
        return fail_parse(P, "expected 'from'");
    struct tok *t = peek(P);
    if (t->kind != K_STR)
        return fail_parse(P, "expected a module specifier");
    n->str = t->str;
    P->pos++;
    if ((at(P, "with") || at(P, "assert")) && !peek(P)->nl && tok_is(peek2(P), "{")) {
        P->pos += 2;
        for (int depth = 1; depth && peek(P)->kind != K_EOF; P->pos++)
            depth += at(P, "{") - at(P, "}");
    }
    return semicolon(P);
}

static struct ast *import_declaration(struct parser *P)
{
    struct ast *n = mk(P, S_IMPORT);
    int cap = 0;
    struct jsstr *star = js_mkstr(P->J, "*", 1), *dflt = js_mkstr(P->J, "default", 7);
    if (peek(P)->kind == K_STR) {                /* import "x"; */
        n->str = peek(P)->str;
        P->pos++;
        return semicolon(P) ? n : NULL;
    }
    if (peek(P)->kind == K_IDENT) {              /* import name from "x" */
        struct jsstr *local = local_name(P);
        if (!local)
            return NULL;
        list_add(P, &n->list, &n->n, &cap, name_pair(P, dflt, local));
        if (!accept(P, ","))
            return module_from(P, n) ? n : NULL;
    }
    if (accept(P, "*")) {                        /* * as ns */
        struct jsstr *local;
        if (!accept(P, "as"))
            return fail_parse(P, "expected 'as'"), NULL;
        if (!(local = local_name(P)))
            return NULL;
        list_add(P, &n->list, &n->n, &cap, name_pair(P, star, local));
    } else if (accept(P, "{")) {                 /* { a, b as c, "d" as e } */
        while (!accept(P, "}")) {
            struct jsstr *name = module_name(P), *local = name;
            if (!name)
                return NULL;
            if (accept(P, "as") && !(local = local_name(P)))
                return NULL;
            list_add(P, &n->list, &n->n, &cap, name_pair(P, name, local));
            if (!at(P, "}") && !expect(P, ","))
                return NULL;
        }
    } else {
        return fail_parse(P, "bad import"), NULL;
    }
    return module_from(P, n) ? n : NULL;
}

/* The names a declaration exports as themselves. */
static void export_pattern(struct parser *P, struct ast *p, struct ast *n, int *cap)
{
    if (!p)
        return;
    switch (p->kind) {
    case N_IDENT:
        list_add(P, &n->list, &n->n, cap, name_pair(P, p->str, p->str));
        break;
    case N_ASSIGN:
    case N_SPREAD:
        export_pattern(P, p->a, n, cap);
        break;
    case N_ARRAY:
        for (int i = 0; i < p->n; i++)
            export_pattern(P, p->list[i], n, cap);
        break;
    case N_OBJECT:
        for (int i = 0; i < p->n; i++)
            export_pattern(P, p->list[i]->a, n, cap);
        break;
    default:
        break;
    }
}

static struct ast *export_declaration(struct parser *P)
{
    struct ast *n = mk(P, S_EXPORT);
    int cap = 0;
    struct jsstr *star = js_mkstr(P->J, "*", 1), *dflt = js_mkstr(P->J, "default", 7);
    if (accept(P, "default")) {
        struct tok *t = peek(P);
        int fn = tok_is(t, "function") || (tok_is(t, "async") && tok_is(peek2(P), "function") && !peek2(P)->nl);
        if (fn || tok_is(t, "class")) {
            /* export default function f() {}: a declaration, exported as default */
            int k = P->pos + (tok_is(t, "async") ? 2 : 1);
            if (k < P->n && tok_is(&P->t[k], "*"))
                k++;
            if (k < P->n && P->t[k].kind == K_IDENT && !is_keyword(&P->t[k])) {
                n->op = EX_DECL;
                if (!(n->a = statement(P)))
                    return NULL;
                struct jsstr *name = n->a->kind == S_FUNCDECL ? n->a->a->str : n->a->list[0]->str;
                list_add(P, &n->list, &n->n, &cap, name_pair(P, name, dflt));
                return n;
            }
        }
        n->op = EX_DEFAULT;
        if (!(n->a = assignment(P)))
            return NULL;
        if (fn || tok_is(t, "class"))
            accept(P, ";");
        else if (!semicolon(P))
            return NULL;
        return n;
    }
    if (accept(P, "*")) {                        /* export * [as ns] from "x" */
        n->op = EX_ALL;
        if (accept(P, "as")) {
            struct jsstr *name = module_name(P);
            if (!name)
                return NULL;
            list_add(P, &n->list, &n->n, &cap, name_pair(P, star, name));
        }
        return module_from(P, n) ? n : NULL;
    }
    if (accept(P, "{")) {                        /* export { a, b as c } [from "x"] */
        n->op = EX_LIST;
        while (!accept(P, "}")) {
            struct jsstr *local = module_name(P), *name = local;
            if (!local)
                return NULL;
            if (accept(P, "as") && !(name = module_name(P)))
                return NULL;
            list_add(P, &n->list, &n->n, &cap, name_pair(P, local, name));
            if (!at(P, "}") && !expect(P, ","))
                return NULL;
        }
        if (at(P, "from"))
            return module_from(P, n) ? n : NULL;
        return semicolon(P) ? n : NULL;
    }
    struct tok *t = peek(P);
    if (!var_keyword(P) && !tok_is(t, "function") && !tok_is(t, "async") && !tok_is(t, "class"))
        return fail_parse(P, "bad export"), NULL;
    n->op = EX_DECL;
    if (!(n->a = statement(P)))
        return NULL;
    if (n->a->kind == S_FUNCDECL) {
        list_add(P, &n->list, &n->n, &cap, name_pair(P, n->a->a->str, n->a->a->str));
    } else if (n->a->kind == S_VAR) {
        for (int i = 0; i < n->a->n; i++) {
            struct ast *d = n->a->list[i];
            export_pattern(P, d->kind == N_DESTRUCT ? d->b : d, n, &cap);
        }
    } else {
        return fail_parse(P, "bad export"), NULL;
    }
    return n;
}

/* A statement at a module's top level, where import and export may be. */
static struct ast *module_item(struct parser *P)
{
    struct tok *t = peek(P);
    if (tok_is(t, "import") && !tok_is(peek2(P), "(") && !tok_is(peek2(P), ".")) {
        P->pos++;
        return import_declaration(P);
    }
    if (tok_is(t, "export")) {
        P->pos++;
        return export_declaration(P);
    }
    return statement(P);
}

/* No collections while parsing: the tokens' strings are in memory the collector does not scan.
 * expr_only: 0 a script, 1 an expression, 2 a module (prog->op is 1 if it awaits at its top level). */
static struct ast *parse(struct js *J, const char *src, int len, const char *where, int expr_only)
{
    struct lexer L = { J, src, src + len, 1, NULL, 0, 0, NULL, 0 };
    J->gc_inhibit++;
    if (lex(&L) < 0) {
        snprintf(J->err, sizeof(J->err), "SyntaxError: %s (%s, line %d)", L.error ? L.error : "?",
                 where, L.error_line);
        free(L.toks);
        J->gc_inhibit--;
        return NULL;
    }
    struct parser P = { J, L.toks, 0, L.n, NULL, 0, 0, 0, where, 0, 0, NULL, 0, 0, 0, 0, 0 };
    struct ast *prog;
    if (expr_only == 2) {
        P.module = 1;
        P.in_async = 1;
        prog = mk(&P, S_BLOCK);
        int cap = 0;
        while (peek(&P)->kind != K_EOF) {
            struct ast *s = module_item(&P);
            if (!s)
                break;
            list_add(&P, &prog->list, &prog->n, &cap, s);
        }
        prog->op = (uint8_t)P.tla;
        prog->info = js_alloc(J, sizeof(struct fninfo));
    } else if (expr_only) {
        prog = expression(&P);
        if (prog && peek(&P)->kind != K_EOF)
            fail_parse(&P, "unexpected token");
    } else {
        prog = mk(&P, S_BLOCK);
        int cap = 0;
        while (peek(&P)->kind != K_EOF) {
            struct ast *s = statement(&P);
            if (!s)
                break;
            list_add(&P, &prog->list, &prog->n, &cap, s);
        }
        prog->info = js_alloc(J, sizeof(struct fninfo));
    }
    if (P.error) {
        snprintf(J->err, sizeof(J->err), "SyntaxError: %s (%s, line %d)", P.error, where, P.error_line);
        prog = NULL;
    }
    free(P.names);
    free(L.toks);
    J->gc_inhibit--;
    return prog;
}

/* A ${...} in a template literal, parsed on its own. */
static struct ast *parse_source_expr(struct parser *P, const char *s, int len)
{
    struct ast *e = parse(P->J, s, len, P->where, 1);
    if (!e)
        fail_parse(P, "bad expression in template literal");
    return e;
}

/* ---- environments ---- */

struct binding {
    struct jsstr *name;
    jsval v;
    struct binding *next;
    uint8_t konst;
    struct binding *alias;              /* an import: the exporting module's binding */
};

struct env {
    struct env *parent;
    struct binding *vars;
    struct jsobj *obj;                  /* the global object, for the global scope */
    jsval self;
    uint8_t has_self;
    uint8_t captured;                   /* a closure holds it: never recycled */
};

static struct env *free_envs;
static struct binding *free_bindings;
static struct js *free_owner;

static struct env *new_env(struct js *J, struct env *parent)
{
    struct env *e;
    if (free_owner == J && free_envs) {
        e = free_envs;
        free_envs = e->parent;
        memset(e, 0, sizeof(*e));
    } else {
        e = js_alloc(J, sizeof(*e));
    }
    e->parent = parent;
    return e;
}

static void recycle_env(struct js *J, struct env *e)
{
    if (e->captured || free_owner != J)
        return;
    struct binding *b = e->vars;
    while (b) {
        struct binding *next = b->next;
        b->next = free_bindings;
        free_bindings = b;
        b = next;
    }
    e->parent = free_envs;
    free_envs = e;
}

static void capture(struct env *e)
{
    for (; e && !e->captured; e = e->parent)
        e->captured = 1;
}

static struct binding *declare(struct js *J, struct env *e, struct jsstr *name, jsval v, int konst)
{
    for (struct binding *b = e->vars; b; b = b->next)
        if (str_eq(b->name, name)) {
            b->v = v;
            b->konst = (uint8_t)konst;
            return b;
        }
    struct binding *b;
    if (free_owner == J && free_bindings) {
        b = free_bindings;
        free_bindings = b->next;
    } else {
        b = js_alloc(J, sizeof(*b));
    }
    b->name = name;
    b->v = v;
    b->konst = (uint8_t)konst;
    b->alias = NULL;
    b->next = e->vars;
    e->vars = b;
    return b;
}

static void declare_global(struct js *J, struct env *e, struct jsstr *name, jsval v, int init)
{
    if (e->obj) {
        if (init || !js_own(e->obj, name))
            js_put(J, e->obj, name, v, 0);
    } else if (init) {
        declare(J, e, name, v, 0);
    } else {
        for (struct binding *b = e->vars; b; b = b->next)
            if (str_eq(b->name, name))
                return;
        declare(J, e, name, v, 0);
    }
}

/* Find a variable: the binding, or the global object that has it. */
static struct binding *lookup(struct env *e, struct jsstr *name, struct jsobj **in_obj)
{
    *in_obj = NULL;
    for (; e; e = e->parent) {
        for (struct binding *b = e->vars; b; b = b->next)
            if (str_eq(b->name, name)) {
                while (b->alias)
                    b = b->alias;
                return b;
            }
        if (e->obj) {
            for (struct jsobj *o = e->obj; o; o = o->proto)
                if (js_own(o, name)) {
                    *in_obj = e->obj;
                    return NULL;
                }
        }
    }
    return NULL;
}

/* ---- the interpreter ---- */

enum { CN, CR, CB, CC, CT };            /* normal, return, break, continue, throw */

enum { B_ASSIGN, B_VAR, B_LET, B_CONST };      /* how bind_pattern declares what it binds */

static jsval eval(struct js *J, struct ast *n, struct env *e);
static jsval mod_import(struct js *J, const char *spec, const char *base);
static int in_async_gen(struct js *J);
static void bind_pattern(struct js *J, struct ast *p, jsval v, struct env *e, int mode);
static jsval super_call(struct js *J, struct ast *n, struct env *e);
static jsval super_get(struct js *J, struct ast *n, struct env *e, jsval key);
static jsval await_value(struct js *J, jsval v);
static jsval yield_value(struct js *J, jsval v);
static int exec(struct js *J, struct ast *n, struct env *e);
static int exec_list(struct js *J, struct ast **list, int n, struct env *e);

#define TRY(v) do { if (J->throwing) return js_undefined; } while (0)

static void step(struct js *J)
{
    if (++J->steps > J->step_limit)
        bail(J, "a script ran too long and was stopped");
}

static jsval read_var(struct js *J, struct env *e, struct jsstr *name, int for_typeof)
{
    struct jsobj *obj;
    struct binding *b = lookup(e, name, &obj);
    if (b)
        return b->v;
    if (obj)
        return js_get_str(J, js_obj(obj), name);
    if (for_typeof)
        return js_undefined;
    return js_throw_error(J, "ReferenceError: %s is not defined", name->s);
}

static void write_var(struct js *J, struct env *e, struct jsstr *name, jsval v)
{
    struct jsobj *obj;
    struct binding *b = lookup(e, name, &obj);
    if (b) {
        if (b->konst) {
            js_throw_error(J, "TypeError: assignment to constant variable '%s'", name->s);
            return;
        }
        b->v = v;
        return;
    }
    obj_set(J, J->global, name, v, js_obj(J->global));
}

static jsval this_value(struct env *e)
{
    for (; e; e = e->parent)
        if (e->has_self)
            return e->self;
    return js_undefined;
}

static jsval make_function(struct js *J, struct ast *fn, struct env *e)
{
    struct jsobj *f = js_mkobj(J, C_FUNCTION, J->function_proto);
    f->fn = fn;
    f->name = fn->str ? fn->str->s : "";
    f->nargs = fn->n - ((fn->flags & F_REST) ? 1 : 0);
    f->arrow = (fn->flags & F_ARROW) != 0;
    if (fn->b == fn) {                  /* a named function expression sees its name */
        struct env *own = new_env(J, e);
        declare(J, own, fn->str, js_obj(f), 0);
        e = own;
    }
    capture(e);
    f->scope = e;
    if (fn->a && fn->a->kind == S_BLOCK && fn->info && !fn->info->prepared)
        prepare_body(J, fn->a, fn->info);
    return js_obj(f);
}

static jsval start_coroutine(struct js *J, struct jsobj *f, jsval self, jsval *args, int argc);

static jsval run_user(struct js *J, struct jsobj *f, jsval self, jsval *args, int argc)
{
    struct ast *fn = f->fn;
    if (++J->depth > JS_MAX_DEPTH) {
        J->depth--;
        return js_throw_error(J, "RangeError: maximum call stack size exceeded");
    }
    if (fn->flags & F_MODULE) {         /* its bindings were made when it was linked */
        exec_list(J, fn->a->list, fn->a->n, f->scope);
        J->depth--;
        return js_undefined;
    }
    struct env *e = new_env(J, f->scope);
    if (f->home)
        declare(J, e, J->s_home, js_obj(f->home), 0);
    if (!f->arrow) {
        e->has_self = 1;
        if (self.t == JS_UNDEF || self.t == JS_NULL)
            self = js_obj(J->global);           /* sloppy mode */
        e->self = self;
        if (fn->info && fn->info->uses_args) {
            struct jsobj *a = js_mkobj(J, C_ARGUMENTS, J->object_proto);
            for (int i = 0; i < argc; i++)
                js_array_push(J, a, args[i]);
            js_put(J, a, js_mkstr(J, "callee", 6), js_obj(f), 1);
            declare(J, e, js_mkstr(J, "arguments", 9), js_obj(a), 0);
            capture(e);                         /* arguments may outlive the call */
        }
    }
    for (int i = 0; i < fn->n; i++) {
        struct ast *p = fn->list[i];
        jsval v;
        if ((fn->flags & F_REST) && i == fn->n - 1) {
            struct jsobj *rest = js_new_array(J);
            for (int k = i; k < argc; k++)
                js_array_push(J, rest, args[k]);
            v = js_obj(rest);
        } else {
            v = i < argc ? args[i] : js_undefined;
            if (v.t == JS_UNDEF && p->a) {
                v = eval(J, p->a, e);
                if (J->throwing) {
                    J->depth--;
                    return js_undefined;
                }
            }
        }
        if (p->kind == N_DESTRUCT) {
            bind_pattern(J, p->b, v, e, B_LET);
            if (J->throwing) {
                J->depth--;
                return js_undefined;
            }
        } else {
            declare(J, e, p->str, v, 0);
        }
    }
    jsval result = js_undefined;
    if (fn->flags & F_EXPRBODY) {
        result = eval(J, fn->a, e);
    } else {
        struct fninfo *fi = fn->info;
        for (int i = 0; fi && i < fi->nvars; i++) {
            struct binding *b;
            for (b = e->vars; b; b = b->next)
                if (str_eq(b->name, fi->vars[i]))
                    break;
            if (!b)
                declare(J, e, fi->vars[i], js_undefined, 0);
        }
        for (int i = 0; fi && i < fi->nfuncs; i++)
            declare(J, e, fi->funcs[i]->str, make_function(J, fi->funcs[i], e), 0);
        int c = exec_list(J, fn->a->list, fn->a->n, e);
        if (c == CR)
            result = J->retval;
    }
    J->depth--;
    recycle_env(J, e);
    return J->throwing ? js_undefined : result;
}

/* A bound function: the stored arguments, then the call's. Kept out of line: its
 * buffer would otherwise make every call's stack frame big. */
static __attribute__((noinline)) jsval call_bound(struct js *J, struct jsobj *f, jsval *args, int argc)
{
    jsval all[64];
    int n = 0;
    for (int i = 0; i < f->nbound && n < 64; i++)
        all[n++] = f->bound_args[i];
    for (int i = 0; i < argc && n < 64; i++)
        all[n++] = args[i];
    return js_call_value(J, js_obj(f->bound), f->bound_this, all, n);
}

/* Calling a user function: async functions and generators run as coroutines. */
static jsval call_user(struct js *J, struct jsobj *f, jsval self, jsval *args, int argc)
{
    if (f->fn->flags & (F_ASYNC | F_GEN))
        return start_coroutine(J, f, self, args, argc);
    return run_user(J, f, self, args, argc);
}

jsval js_call_value(struct js *J, jsval fv, jsval self, jsval *args, int argc)
{
    if (!js_is_callable(fv))
        return js_throw_error(J, "TypeError: %s is not a function",
                              fv.t == JS_UNDEF ? "undefined" : fv.t == JS_NULL ? "null" : "value");
    struct jsobj *f = fv.u.o;
    if (f->bound)
        return call_bound(J, f, args, argc);
    step(J);
    if (f->native) {
        int saved = J->constructing;
        J->constructing = 0;
        jsval r = f->native(J, self, args, argc);
        J->constructing = saved;
        return r;
    }
    if (f->is_class)
        return js_throw_error(J, "TypeError: class constructor %s cannot be invoked without 'new'",
                              f->name ? f->name : "");
    return call_user(J, f, self, args, argc);
}

jsval js_construct(struct js *J, jsval fv, jsval *args, int argc)
{
    if (!js_is_callable(fv) || fv.u.o->arrow)
        return js_throw_error(J, "TypeError: not a constructor");
    struct jsobj *f = fv.u.o;
    if (f->bound) {                             /* new (F.bind(null, a))(b): a, b */
        jsval all[64];
        int n = 0;
        for (int i = 0; i < f->nbound && n < 64; i++)
            all[n++] = f->bound_args[i];
        for (int i = 0; i < argc && n < 64; i++)
            all[n++] = args[i];
        return js_construct(J, js_obj(f->bound), all, n);     /* a bound bound function unwraps in turn */
    }
    if (f->native) {
        if (!f->ctor)
            return js_throw_error(J, "TypeError: %s is not a constructor", f->name ? f->name : "value");
        int saved = J->constructing;
        J->constructing = 1;
        jsval r = f->native(J, js_undefined, args, argc);
        J->constructing = saved;
        return r;
    }
    jsval proto = js_get(J, js_obj(f), "prototype");
    struct jsobj *o = js_mkobj(J, C_OBJECT, proto.t == JS_OBJ ? proto.u.o : J->object_proto);
    step(J);
    jsval r = call_user(J, f, js_obj(o), args, argc);
    if (J->throwing)
        return js_undefined;
    return r.t == JS_OBJ ? r : js_obj(o);
}

/* Evaluate a call's arguments, spreading arrays; returns the count. */
static int eval_args(struct js *J, struct ast *call, struct env *e, jsval **out, jsval *local, int nlocal)
{
    int n = 0, cap = nlocal;
    jsval *args = local;
    for (int i = 0; i < call->n; i++) {
        struct ast *a = call->list[i];
        if (a->kind == N_SPREAD) {
            jsval v = eval(J, a->a, e);
            if (J->throwing)
                return -1;
            struct iter it;
            if (js_iter_open(J, v, &it) < 0)
                return -1;
            jsval item;
            int r;
            while ((r = js_iter_next(J, &it, &item)) > 0) {
                if (n == cap) {
                    jsval *na = js_alloc(J, sizeof(jsval) * (size_t)cap * 2);
                    memcpy(na, args, sizeof(jsval) * (size_t)n);
                    args = na;
                    cap *= 2;
                }
                args[n++] = item;
            }
            if (r < 0)
                return -1;
            continue;
        }
        if (n == cap) {
            jsval *na = js_alloc(J, sizeof(jsval) * (size_t)cap * 2);
            memcpy(na, args, sizeof(jsval) * (size_t)n);
            args = na;
            cap *= 2;
        }
        args[n++] = eval(J, a, e);
        if (J->throwing)
            return -1;
    }
    *out = args;
    return n;
}

static const char *type_of(jsval v)
{
    switch (v.t) {
    case JS_UNDEF: return "undefined";
    case JS_NULL: return "object";
    case JS_BOOL: return "boolean";
    case JS_NUM: return "number";
    case JS_STR: return "string";
    default: return v.u.o->cls == C_FUNCTION ? "function" : "object";
    }
}

static jsval add(struct js *J, jsval a, jsval b)
{
    if (a.t == JS_NUM && b.t == JS_NUM)
        return js_num(a.u.n + b.u.n);
    if (a.t == JS_STR && b.t == JS_STR)
        return strv(js_concat(J, a.u.s, b.u.s));
    a = js_to_primitive(J, a, 0);
    TRY();
    b = js_to_primitive(J, b, 0);
    TRY();
    if (a.t == JS_STR || b.t == JS_STR) {
        struct jsstr *x = js_tostr(J, a), *y = js_tostr(J, b);
        return strv(js_concat(J, x, y));
    }
    return js_num(js_to_number(J, a) + js_to_number(J, b));
}

static int compare(struct js *J, jsval a, jsval b, int *undef)
{
    *undef = 0;
    a = js_to_primitive(J, a, 0);
    b = js_to_primitive(J, b, 0);
    if (a.t == JS_STR && b.t == JS_STR) {
        const char *ap = a.u.s->s, *bp = b.u.s->s, *ae = ap + a.u.s->len, *be = bp + b.u.s->len;
        int ah = 0, bh = 0;
        for (;;) {
            int x = next_u16(&ap, ae, &ah), y = next_u16(&bp, be, &bh);
            if (x != y)
                return x - y;
            if (x < 0)
                return 0;
        }
    }
    double x = js_to_number(J, a), y = js_to_number(J, b);
    if (isnan(x) || isnan(y)) {
        *undef = 1;
        return 0;
    }
    return x < y ? -1 : x > y;
}

static int instance_of(struct js *J, jsval v, jsval ctor)
{
    if (!js_is_callable(ctor)) {
        js_throw_error(J, "TypeError: right side of instanceof is not callable");
        return 0;
    }
    if (v.t != JS_OBJ)
        return 0;
    struct jsobj *f = ctor.u.o;
    while (f->bound)
        f = f->bound;
    jsval proto = js_get(J, js_obj(f), "prototype");
    if (proto.t != JS_OBJ)
        return 0;
    for (struct jsobj *o = v.u.o->proto; o; o = o->proto)
        if (o == proto.u.o)
            return 1;
    return 0;
}

static jsval binary_op(struct js *J, int op, jsval a, jsval b)
{
    int undef;
    switch (op) {
    case OP_ADD: return add(J, a, b);
    case OP_SUB: return js_num(js_to_number(J, a) - js_to_number(J, b));
    case OP_MUL: return js_num(js_to_number(J, a) * js_to_number(J, b));
    case OP_DIV: return js_num(js_to_number(J, a) / js_to_number(J, b));
    case OP_MOD: {
        double x = js_to_number(J, a), y = js_to_number(J, b);
        return js_num(isinf(y) && !isinf(x) ? x : fmod(x, y));
    }
    case OP_EXP: return js_num(pow(js_to_number(J, a), js_to_number(J, b)));
    case OP_SHL: return js_num((double)(int32_t)((uint32_t)to_int32(J, a) << (to_uint32(J, b) & 31)));
    case OP_SHR: return js_num((double)(to_int32(J, a) >> (to_uint32(J, b) & 31)));
    case OP_USHR: return js_num((double)(to_uint32(J, a) >> (to_uint32(J, b) & 31)));
    case OP_BAND: return js_num((double)(to_int32(J, a) & to_int32(J, b)));
    case OP_BOR: return js_num((double)(to_int32(J, a) | to_int32(J, b)));
    case OP_BXOR: return js_num((double)(to_int32(J, a) ^ to_int32(J, b)));
    case OP_EQ: return js_bool(js_loose_equal(J, a, b));
    case OP_NE: return js_bool(!js_loose_equal(J, a, b));
    case OP_SEQ: return js_bool(js_strict_equal(a, b));
    case OP_SNE: return js_bool(!js_strict_equal(a, b));
    case OP_LT: { int c = compare(J, a, b, &undef); return js_bool(!undef && c < 0); }
    case OP_GT: { int c = compare(J, a, b, &undef); return js_bool(!undef && c > 0); }
    case OP_LE: { int c = compare(J, a, b, &undef); return js_bool(!undef && c <= 0); }
    case OP_GE: { int c = compare(J, a, b, &undef); return js_bool(!undef && c >= 0); }
    case OP_INSTANCEOF: return js_bool(instance_of(J, a, b));
    case OP_IN:
        if (b.t != JS_OBJ)
            return js_throw_error(J, "TypeError: 'in' needs an object");
        return js_bool(js_has(J, b, js_tostr(J, a)));
    }
    return js_undefined;
}

/* A reference: where an assignment or ++ goes. */
struct ref {
    int kind;                           /* N_IDENT, N_MEMBER, N_INDEX */
    jsval obj, key;
    struct jsstr *name;
};

static int make_ref(struct js *J, struct ast *n, struct env *e, struct ref *r)
{
    r->kind = n->kind;
    if (n->kind == N_IDENT) {
        r->name = n->str;
        return 0;
    }
    r->obj = eval(J, n->a, e);
    if (J->throwing)
        return -1;
    if (n->kind == N_MEMBER)
        r->key = strv(n->str);
    else {
        r->key = eval(J, n->b, e);
        if (J->throwing)
            return -1;
    }
    return 0;
}

static jsval ref_get(struct js *J, struct ref *r, struct env *e)
{
    if (r->kind == N_IDENT)
        return read_var(J, e, r->name, 0);
    return js_getv(J, r->obj, r->key);
}

static void ref_put(struct js *J, struct ref *r, struct env *e, jsval v)
{
    if (r->kind == N_IDENT)
        write_var(J, e, r->name, v);
    else
        js_setv(J, r->obj, r->key, v);
}

/* ---- iteration ---- */

/*
 * One way to walk anything iterable, for for-of, spread, destructuring and
 * the built-ins: arrays, strings, array-likes (a NodeList), and objects with
 * a [Symbol.iterator] method (Symbol.iterator is the key "@@iterator"),
 * whose iterator is an object with next(). A method that returns an array
 * stands for that array's elements (the Map and Set built-ins do).
 */
int js_iter_open(struct js *J, jsval v, struct iter *it)
{
    memset(it, 0, sizeof(*it));
    if (v.t == JS_STR) {
        it->kind = IT_STR;
        it->sp = v.u.s->s;
        it->send = v.u.s->s + v.u.s->len;
        return 0;
    }
    if (v.t != JS_OBJ) {
        js_throw_error(J, "TypeError: %s is not iterable", v.t == JS_UNDEF ? "undefined" :
                       v.t == JS_NULL ? "null" : "value");
        return -1;
    }
    if (v.u.o->cls == C_ARRAY || v.u.o->cls == C_ARGUMENTS) {
        it->kind = IT_ARRAY;
        it->arr = v.u.o;
        return 0;
    }
    jsval f = js_get(J, v, "@@iterator");
    if (J->throwing)
        return -1;
    if (js_is_callable(f)) {
        jsval r = js_call_value(J, f, v, NULL, 0);
        if (J->throwing)
            return -1;
        if (r.t == JS_OBJ && (r.u.o->cls == C_ARRAY || r.u.o->cls == C_ARGUMENTS)) {
            it->kind = IT_ARRAY;
            it->arr = r.u.o;
            return 0;
        }
        jsval next = r.t == JS_OBJ ? js_get(J, r, "next") : js_undefined;
        if (J->throwing)
            return -1;
        if (!js_is_callable(next)) {
            js_throw_error(J, "TypeError: the iterator has no next() method");
            return -1;
        }
        it->kind = IT_OBJ;
        it->obj = r;
        it->next = next;
        return 0;
    }
    jsval len = js_get(J, v, "length");
    if (J->throwing)
        return -1;
    if (len.t != JS_NUM) {
        js_throw_error(J, "TypeError: value is not iterable");
        return -1;
    }
    it->kind = IT_LIKE;
    it->obj = v;
    it->arr = NULL;
    it->i = 0;
    return 0;
}

/* 1 and the next value, 0 when there is none, -1 if something threw. */
int js_iter_next(struct js *J, struct iter *it, jsval *out)
{
    switch (it->kind) {
    case IT_ARRAY:
        if (it->i >= it->arr->len)
            return 0;
        *out = it->arr->items[it->i++];
        return 1;
    case IT_STR: {
        if (it->sp >= it->send)
            return 0;
        const char *start = it->sp;
        utf8_next(&it->sp, it->send);
        *out = js_strn(J, start, (int)(it->sp - start));
        return 1;
    }
    case IT_LIKE: {
        jsval len = js_get(J, it->obj, "length");
        if (J->throwing)
            return -1;
        if (it->i >= (int)js_to_number(J, len))
            return 0;
        *out = js_getv(J, it->obj, js_num(it->i++));
        return J->throwing ? -1 : 1;
    }
    default: {
        jsval r = js_call_value(J, it->next, it->obj, NULL, 0);
        if (J->throwing)
            return -1;
        if (r.t != JS_OBJ) {
            js_throw_error(J, "TypeError: iterator result is not an object");
            return -1;
        }
        jsval done = js_get(J, r, "done");
        if (J->throwing)
            return -1;
        if (js_truthy(done))
            return 0;
        *out = js_get(J, r, "value");
        return J->throwing ? -1 : 1;
    }
    }
}

/* Stopped early (break, a destructuring that needs no more): let it clean up. */
void js_iter_close(struct js *J, struct iter *it)
{
    if (it->kind != IT_OBJ)
        return;
    int saved = J->throwing;
    jsval exc = J->exc;
    J->throwing = 0;
    jsval f = js_get(J, it->obj, "return");
    if (!J->throwing && js_is_callable(f))
        js_call_value(J, f, it->obj, NULL, 0);
    if (saved) {
        J->throwing = saved;
        J->exc = exc;
    } else {
        J->throwing = 0;
    }
}

/* ---- destructuring ---- */

static void bind_name(struct js *J, struct env *e, struct jsstr *name, jsval v, int mode)
{
    if (mode == B_ASSIGN || mode == B_VAR)
        write_var(J, e, name, v);
    else if (e->obj)
        js_put(J, e->obj, name, v, 0);          /* top-level let/const: global */
    else
        declare(J, e, name, v, mode == B_CONST);
}

static void bind_pattern(struct js *J, struct ast *p, jsval v, struct env *e, int mode)
{
    switch (p->kind) {
    case N_IDENT:
        bind_name(J, e, p->str, v, mode);
        return;
    case N_MEMBER:
    case N_INDEX: {
        struct ref r;
        if (make_ref(J, p, e, &r) == 0)
            ref_put(J, &r, e, v);
        return;
    }
    case N_ASSIGN:                      /* a target with a default */
        if (v.t == JS_UNDEF) {
            v = eval(J, p->b, e);
            if (J->throwing)
                return;
            if (p->a->kind == N_IDENT && js_is_callable(v) && v.u.o->fn &&
                (!v.u.o->name || !*v.u.o->name))
                v.u.o->name = p->a->str->s;
        }
        bind_pattern(J, p->a, v, e, mode);
        return;
    case N_ARRAY: {
        struct iter it;
        if (js_iter_open(J, v, &it) < 0)
            return;
        int done = 0;
        for (int i = 0; i < p->n && !J->throwing; i++) {
            struct ast *x = p->list[i];
            jsval item = js_undefined;
            if (x->kind == N_SPREAD) {
                struct jsobj *rest = js_new_array(J);
                while (!done) {
                    int r = js_iter_next(J, &it, &item);
                    if (r < 0)
                        return;
                    if (!r)
                        done = 1;
                    else
                        js_array_push(J, rest, item);
                }
                bind_pattern(J, x->a, js_obj(rest), e, mode);
                return;
            }
            if (!done) {
                int r = js_iter_next(J, &it, &item);
                if (r < 0)
                    return;
                if (!r) {
                    done = 1;
                    item = js_undefined;
                }
            }
            if (x->kind != N_HOLE)
                bind_pattern(J, x, item, e, mode);
        }
        if (!done)
            js_iter_close(J, &it);
        return;
    }
    case N_OBJECT: {
        if (v.t == JS_UNDEF || v.t == JS_NULL) {
            js_throw_error(J, "TypeError: cannot destructure '%s'", v.t == JS_NULL ? "null" : "undefined");
            return;
        }
        struct jsstr *used[32];
        int nused = 0;
        for (int i = 0; i < p->n; i++) {
            struct ast *pr = p->list[i];
            if (pr->kind == N_SPREAD) {         /* {a, ...rest} */
                struct jsobj *rest = js_new_object(J);
                struct jsstr **keys;
                int nk = js_keys(J, v, 0, &keys);
                for (int k = 0; k < nk; k++) {
                    int skip = 0;
                    for (int u = 0; u < nused; u++)
                        skip |= str_eq(used[u], keys[k]);
                    if (!skip)
                        js_put(J, rest, keys[k], js_get_str(J, v, keys[k]), 0);
                }
                bind_pattern(J, pr->a, js_obj(rest), e, mode);
                return;
            }
            struct jsstr *key = pr->str;
            if (pr->b) {
                jsval kv = eval(J, pr->b, e);
                if (J->throwing)
                    return;
                key = js_tostr(J, kv);
            }
            if (nused < 32)
                used[nused++] = key;
            jsval item = js_get_str(J, v, key);
            if (J->throwing)
                return;
            bind_pattern(J, pr->a, item, e, mode);
            if (J->throwing)
                return;
        }
        return;
    }
    default:
        js_throw_error(J, "SyntaxError: invalid destructuring target");
    }
}

static jsval eval_unary(struct js *J, struct ast *n, struct env *e)
{
    if (n->op == OP_TYPEOF && n->a->kind == N_IDENT) {
        jsval v = read_var(J, e, n->a->str, 1);
        TRY();
        return js_str(J, type_of(v));
    }
    if (n->op == OP_DELETE) {
        if (n->a->kind == N_MEMBER || n->a->kind == N_INDEX) {
            struct ref r;
            if (make_ref(J, n->a, e, &r) < 0)
                return js_undefined;
            struct jsstr *k = js_tostr(J, r.key);
            TRY();
            return js_bool(js_delete(J, r.obj, k));
        }
        return js_bool(1);
    }
    jsval v = eval(J, n->a, e);
    TRY();
    switch (n->op) {
    case OP_NOT: return js_bool(!js_truthy(v));
    case OP_BNOT: return js_num((double)~to_int32(J, v));
    case OP_NEG: return js_num(-js_to_number(J, v));
    case OP_PLUS: return js_num(js_to_number(J, v));
    case OP_TYPEOF: return js_str(J, type_of(v));
    case OP_VOID: return js_undefined;
    }
    return js_undefined;
}

/* The arguments of a tagged template: the strings (with .raw), then each ${value}. */
static int eval_tagged(struct js *J, struct ast *tl, struct env *e, jsval **out)
{
    struct jsobj *strings = js_new_array(J), *raw = js_new_array(J);
    jsval *args = js_alloc(J, sizeof(jsval) * (size_t)(tl->n / 2 + 1));
    int n = 1;
    for (int i = 0; i < tl->n; i++) {
        if (i % 2 == 0) {
            js_array_push(J, strings, strv(tl->list[i]->str));
            js_array_push(J, raw, strv(tl->list[i]->a ? tl->list[i]->a->str : tl->list[i]->str));
        } else {
            args[n++] = eval(J, tl->list[i], e);
            if (J->throwing)
                return -1;
        }
    }
    js_put(J, strings, js_mkstr(J, "raw", 3), js_obj(raw), 1);
    args[0] = js_obj(strings);
    *out = args;
    return n;
}

static jsval eval_call(struct js *J, struct ast *n, struct env *e)
{
    jsval fn, self = js_undefined;
    struct ast *callee = n->a;
    if (callee->kind == N_SUPER)
        return super_call(J, n, e);
    if ((callee->kind == N_MEMBER || callee->kind == N_INDEX) && callee->a->kind == N_SUPER) {
        jsval key = callee->kind == N_MEMBER ? strv(callee->str) : eval(J, callee->b, e);
        TRY();
        fn = super_get(J, callee, e, key);
        TRY();
        if (!js_is_callable(fn))
            return js_throw_error(J, "TypeError: super.%s is not a function", js_tostr(J, key)->s);
        self = this_value(e);
        jsval local[16], *args;
        int argc = eval_args(J, n, e, &args, local, 16);
        if (argc < 0)
            return js_undefined;
        return js_call_value(J, fn, self, args, argc);
    }
    if (callee->kind == N_MEMBER || callee->kind == N_INDEX) {
        self = eval(J, callee->a, e);
        TRY();
        if ((callee->flags & F_OPTIONAL) && (self.t == JS_UNDEF || self.t == JS_NULL))
            return js_undefined;
        jsval key = callee->kind == N_MEMBER ? strv(callee->str) : eval(J, callee->b, e);
        TRY();
        fn = js_getv(J, self, key);
        TRY();
        if (!js_is_callable(fn) && !(n->flags & F_OPTIONAL)) {
            struct jsstr *k = js_tostr(J, key);
            return js_throw_error(J, "TypeError: %s is not a function", k->s);
        }
    } else {
        fn = eval(J, callee, e);
        TRY();
        if (!js_is_callable(fn) && !(n->flags & F_OPTIONAL) && callee->kind == N_IDENT)
            return js_throw_error(J, "TypeError: %s is not a function", callee->str->s);
    }
    if ((n->flags & F_OPTIONAL) && (fn.t == JS_UNDEF || fn.t == JS_NULL))
        return js_undefined;
    jsval local[16], *args;
    int argc = n->op == 1 ? eval_tagged(J, n->list[0], e, &args) : eval_args(J, n, e, &args, local, 16);
    if (argc < 0)
        return js_undefined;
    return js_call_value(J, fn, self, args, argc);
}

/* ---- classes ---- */

/* A native base class built an object for us: make `self` that object. */
static void adopt(struct js *J, struct jsobj *self, struct jsobj *from)
{
    self->cls = from->cls;
    self->items = from->items;
    self->len = from->len;
    self->cap = from->cap;
    self->num = from->num;
    self->str = from->str;
    self->re = from->re;
    self->hcls = from->hcls;
    self->hdata = from->hdata;
    for (struct prop *p = from->props; p; p = p->next) {
        js_put(J, self, p->key, p->v, p->hidden);
        struct prop *q = js_own(self, p->key);
        if (q) {
            q->getter = p->getter;
            q->setter = p->setter;
        }
    }
    if (from->cls == C_HOST && from->hcls && from->hcls->adopt)
        from->hcls->adopt(J, self, from);
}

/* super(...) in a constructor: the parent runs on the same this. */
static jsval super_call(struct js *J, struct ast *n, struct env *e)
{
    struct jsobj *obj;
    struct binding *b = lookup(e, J->s_home, &obj);
    if (!b || b->v.t != JS_OBJ)
        return js_throw_error(J, "SyntaxError: 'super' is only valid in a class constructor");
    jsval ctor = js_get(J, b->v, "constructor");
    TRY();
    struct jsobj *parent = ctor.t == JS_OBJ ? ctor.u.o->proto : NULL;
    if (!parent || parent == J->function_proto || !js_is_callable(js_obj(parent)))
        return js_throw_error(J, "TypeError: super constructor is not a constructor");
    jsval local[16], *args;
    int argc = eval_args(J, n, e, &args, local, 16);
    if (argc < 0)
        return js_undefined;
    jsval self = this_value(e);
    if (self.t != JS_OBJ)
        return js_throw_error(J, "ReferenceError: 'this' is not available here");
    while (parent->bound)
        parent = parent->bound;
    if (parent->native) {
        struct jsobj *saved = J->super_this;
        J->super_this = self.u.o;
        jsval made = js_construct(J, js_obj(parent), args, argc);
        J->super_this = saved;
        TRY();
        if (made.t == JS_OBJ && made.u.o != self.u.o)
            adopt(J, self.u.o, made.u.o);
    } else {
        step(J);
        call_user(J, parent, self, args, argc);
        TRY();
    }
    return self;
}

/* super.name: looked up from the home object's prototype, with this as the receiver. */
static jsval super_get(struct js *J, struct ast *n, struct env *e, jsval key)
{
    struct jsobj *obj;
    struct binding *b = lookup(e, J->s_home, &obj);
    if (!b || b->v.t != JS_OBJ)
        return js_throw_error(J, "SyntaxError: 'super' is only valid inside a method");
    struct jsobj *proto = b->v.u.o->proto;
    if (!proto)
        return js_undefined;
    struct jsstr *k = js_tostr(J, key);
    jsval out;
    if (!obj_get(J, proto, k, this_value(e), &out))
        return js_undefined;
    (void)n;
    return out;
}

static jsval eval_class(struct js *J, struct ast *n, struct env *e)
{
    struct jsobj *pproto = J->object_proto;
    jsval parent = js_undefined;
    if (n->a) {
        parent = eval(J, n->a, e);
        TRY();
        if (parent.t == JS_NULL) {
            pproto = NULL;
        } else if (!js_is_callable(parent) || parent.u.o->arrow) {
            return js_throw_error(J, "TypeError: class extends value is not a constructor or null");
        } else {
            jsval pp = js_get(J, parent, "prototype");
            TRY();
            pproto = pp.t == JS_OBJ ? pp.u.o : NULL;
        }
    }
    struct env *ce = new_env(J, e);             /* the scope that holds the class's own name */
    struct jsobj *proto = js_mkobj(J, C_OBJECT, pproto);
    jsval fv = make_function(J, n->b, ce);
    struct jsobj *F = fv.u.o;
    F->is_class = 1;
    F->home = proto;
    F->name = n->str ? n->str->s : "";
    if (parent.t == JS_OBJ)
        F->proto = parent.u.o;                  /* static members are inherited */
    js_put(J, F, js_mkstr(J, "prototype", 9), js_obj(proto), 1);
    js_put(J, proto, js_mkstr(J, "constructor", 11), fv, 1);
    if (n->str)
        declare(J, ce, n->str, fv, 1);
    capture(ce);

    for (int i = 0; i < n->n; i++) {
        struct ast *m = n->list[i];
        struct jsobj *target = m->op ? F : proto;
        struct jsstr *key = m->str;
        if (m->b) {
            jsval kv = eval(J, m->b, ce);
            TRY();
            key = js_tostr(J, kv);
        }
        if (m->kind == N_FIELD && m->op == 2) {
            jsval bf = make_function(J, m->a, ce);
            bf.u.o->home = F;
            js_call_value(J, bf, fv, NULL, 0);
            TRY();
            continue;
        }
        if (m->kind == N_FIELD) {
            if (!m->op)
                continue;                       /* instance fields run in the constructor */
            struct env *se = new_env(J, ce);
            se->has_self = 1;
            se->self = fv;
            jsval v = js_undefined;
            if (m->a) {
                v = eval(J, m->a, se);
                TRY();
                if (js_is_callable(v) && v.u.o->fn && (!v.u.o->name || !*v.u.o->name))
                    v.u.o->name = key->s;
            }
            capture(se);
            js_put(J, F, key, v, 0);
            continue;
        }
        jsval f = make_function(J, m->a, ce);
        f.u.o->home = target;
        f.u.o->name = key->s;
        if (m->kind == N_GETTER || m->kind == N_SETTER) {
            struct prop *pr = js_own(target, key);
            if (!pr) {
                js_put(J, target, key, js_undefined, 1);
                pr = js_own(target, key);
            }
            if (m->kind == N_GETTER)
                pr->getter = f.u.o;
            else
                pr->setter = f.u.o;
        } else {
            js_put(J, target, key, f, 1);
        }
    }
    return fv;
}

static jsval eval_object(struct js *J, struct ast *n, struct env *e)
{
    struct jsobj *o = js_new_object(J);
    for (int i = 0; i < n->n; i++) {
        struct ast *p = n->list[i];
        if (p->kind == N_SPREAD) {
            jsval src = eval(J, p->a, e);
            TRY();
            struct jsstr **keys;
            int nk = js_keys(J, src, 0, &keys);
            for (int k = 0; k < nk; k++)
                js_put(J, o, keys[k], js_get_str(J, src, keys[k]), 0);
            continue;
        }
        struct jsstr *key = p->str;
        if (p->b) {
            jsval kv = eval(J, p->b, e);
            TRY();
            key = js_tostr(J, kv);
        }
        jsval v = eval(J, p->a, e);
        TRY();
        if (p->kind == N_GETTER || p->kind == N_SETTER) {
            struct prop *pr = js_own(o, key);
            if (!pr) {
                js_put(J, o, key, js_undefined, 0);
                pr = js_own(o, key);
            }
            if (p->kind == N_GETTER)
                pr->getter = v.u.o;
            else
                pr->setter = v.u.o;
            continue;
        }
        if (js_is_callable(v) && v.u.o->fn && (!v.u.o->name || !*v.u.o->name))
            v.u.o->name = key->s;
        js_put(J, o, key, v, 0);
    }
    return js_obj(o);
}

/* new F(...): out of line, so that eval's own stack frame stays small. */
static __attribute__((noinline)) jsval eval_new(struct js *J, struct ast *n, struct env *e)
{
    jsval fn = eval(J, n->a, e);
    TRY();
    jsval local[16], *args;
    int argc = eval_args(J, n, e, &args, local, 16);
    if (argc < 0)
        return js_undefined;
    if (!js_is_callable(fn))
        return js_throw_error(J, "TypeError: %s is not a constructor",
                              n->a->kind == N_IDENT ? n->a->str->s : "value");
    return js_construct(J, fn, args, argc);
}

static jsval eval(struct js *J, struct ast *n, struct env *e)
{
    step(J);
    switch (n->kind) {
    case N_NUM: return js_num(n->num);
    case N_STR: return strv(n->str);
    case N_TRUE: return js_bool(1);
    case N_FALSE: return js_bool(0);
    case N_NULL: return js_null;
    case N_THIS: return this_value(e);
    case N_IDENT: return read_var(J, e, n->str, 0);
    case N_TEMPLATE: {
        struct jsstr *s = js_mkstr(J, "", 0);
        for (int i = 0; i < n->n; i++) {
            jsval v = eval(J, n->list[i], e);
            TRY();
            s = js_concat(J, s, js_tostr(J, v));
            TRY();
        }
        return strv(s);
    }
    case N_REGEX: {
        if (!n->re)
            return js_throw_error(J, "SyntaxError: invalid regular expression /%s/: %s", n->str->s,
                                  n->a ? n->a->str->s : "?");
        struct jsobj *o = js_mkobj(J, C_REGEXP, J->regexp_proto);
        o->re = n->re;
        js_put(J, o, js_mkstr(J, "lastIndex", 9), js_num(0), 1);
        return js_obj(o);
    }
    case N_ARRAY: {
        struct jsobj *a = js_new_array(J);
        for (int i = 0; i < n->n; i++) {
            struct ast *x = n->list[i];
            if (x->kind == N_HOLE) {
                js_array_push(J, a, js_undefined);
                continue;
            }
            jsval v = eval(J, x->kind == N_SPREAD ? x->a : x, e);
            TRY();
            if (x->kind == N_SPREAD) {
                struct iter it;
                if (js_iter_open(J, v, &it) < 0)
                    return js_undefined;
                jsval item;
                int r;
                while ((r = js_iter_next(J, &it, &item)) > 0)
                    js_array_push(J, a, item);
                if (r < 0)
                    return js_undefined;
            } else {
                js_array_push(J, a, v);
            }
        }
        return js_obj(a);
    }
    case N_OBJECT: return eval_object(J, n, e);
    case N_FUNC: return make_function(J, n, e);
    case N_CLASS: return eval_class(J, n, e);
    case N_AWAIT: {
        jsval v = eval(J, n->a, e);
        TRY();
        return await_value(J, v);
    }
    case N_IMPORT: {
        jsval spec = eval(J, n->a, e);
        TRY();
        struct jsstr *s = js_tostr(J, spec);
        TRY();
        return mod_import(J, s->s, n->str->s);
    }
    case N_IMPORT_META:
        return read_var(J, e, J->s_meta, 1);
    case N_YIELD: {
        if (!n->op) {
            jsval v = n->a ? eval(J, n->a, e) : js_undefined;
            TRY();
            if (in_async_gen(J) && js_is_promise(v)) {     /* async: yield await v */
                v = await_value(J, v);
                TRY();
            }
            return yield_value(J, v);
        }
        jsval src = eval(J, n->a, e);       /* yield*: pass on everything the other iterable gives */
        TRY();
        struct iter it;
        if (js_iter_open(J, src, &it) < 0)
            return js_undefined;
        for (;;) {
            jsval item;
            int r = js_iter_next(J, &it, &item);
            if (r < 0)
                return js_undefined;
            if (!r)
                break;
            yield_value(J, item);
            if (J->throwing) {              /* thrown in, or the generator is being closed */
                js_iter_close(J, &it);
                return js_undefined;
            }
        }
        return js_undefined;
    }
    case N_MEMBER:
    case N_INDEX: {
        if (n->a->kind == N_SUPER) {
            jsval key = n->kind == N_MEMBER ? strv(n->str) : eval(J, n->b, e);
            TRY();
            return super_get(J, n, e, key);
        }
        jsval obj = eval(J, n->a, e);
        TRY();
        if ((n->flags & F_OPTIONAL) && (obj.t == JS_UNDEF || obj.t == JS_NULL))
            return js_undefined;
        if (n->kind == N_MEMBER)
            return js_get_str(J, obj, n->str);
        jsval key = eval(J, n->b, e);
        TRY();
        return js_getv(J, obj, key);
    }
    case N_CALL: return eval_call(J, n, e);
    case N_NEW: return eval_new(J, n, e);
    case N_UNARY: return eval_unary(J, n, e);
    case N_PREINC:
    case N_POSTINC: {
        struct ref r;
        if (make_ref(J, n->a, e, &r) < 0)
            return js_undefined;
        double old = js_to_number(J, ref_get(J, &r, e));
        TRY();
        double nv = n->op == OP_INC ? old + 1 : old - 1;
        ref_put(J, &r, e, js_num(nv));
        return js_num(n->kind == N_PREINC ? nv : old);
    }
    case N_BINARY: {
        jsval a = eval(J, n->a, e);
        TRY();
        jsval b = eval(J, n->b, e);
        TRY();
        return binary_op(J, n->op, a, b);
    }
    case N_AND: {
        jsval a = eval(J, n->a, e);
        TRY();
        return js_truthy(a) ? eval(J, n->b, e) : a;
    }
    case N_OR: {
        jsval a = eval(J, n->a, e);
        TRY();
        return js_truthy(a) ? a : eval(J, n->b, e);
    }
    case N_NULLISH: {
        jsval a = eval(J, n->a, e);
        TRY();
        return a.t == JS_UNDEF || a.t == JS_NULL ? eval(J, n->b, e) : a;
    }
    case N_COND: {
        jsval c = eval(J, n->a, e);
        TRY();
        return eval(J, js_truthy(c) ? n->b : n->c, e);
    }
    case N_SEQ: {
        jsval v = js_undefined;
        for (int i = 0; i < n->n; i++) {
            v = eval(J, n->list[i], e);
            TRY();
        }
        return v;
    }
    case N_ASSIGN: {
        struct ref r;
        if (n->a->kind == N_ARRAY || n->a->kind == N_OBJECT) {
            jsval v = eval(J, n->b, e);         /* [a, b] = [b, a] */
            TRY();
            bind_pattern(J, n->a, v, e, B_ASSIGN);
            return J->throwing ? js_undefined : v;
        }
        if (make_ref(J, n->a, e, &r) < 0)
            return js_undefined;
        jsval v;
        if (n->op == OP_NONE) {
            v = eval(J, n->b, e);
            TRY();
            if (js_is_callable(v) && v.u.o->fn && (!v.u.o->name || !*v.u.o->name) && r.kind == N_IDENT)
                v.u.o->name = r.name->s;
        } else {
            jsval old = ref_get(J, &r, e);
            TRY();
            if (n->op == OP_AND || n->op == OP_OR || n->op == OP_NULLISH) {
                int keep = n->op == OP_AND ? !js_truthy(old) : n->op == OP_OR ? js_truthy(old) :
                           !(old.t == JS_UNDEF || old.t == JS_NULL);
                if (keep)
                    return old;
                v = eval(J, n->b, e);
            } else {
                jsval rhs = eval(J, n->b, e);
                TRY();
                v = binary_op(J, n->op, old, rhs);
            }
            TRY();
        }
        ref_put(J, &r, e, v);
        TRY();
        return v;
    }
    default:
        return js_throw_error(J, "SyntaxError: unexpected expression");
    }
}

/* ---- statements ---- */

static int throwing(struct js *J)
{
    return J->throwing ? CT : CN;
}

static int exec_var(struct js *J, struct ast *n, struct env *e)
{
    for (int i = 0; i < n->n; i++) {
        struct ast *d = n->list[i];
        jsval v = js_undefined;
        if (d->kind == N_DESTRUCT) {
            if (!d->a) {
                js_throw_error(J, "SyntaxError: a destructuring declaration needs a value");
                return CT;
            }
            v = eval(J, d->a, e);
            if (J->throwing)
                return CT;
            bind_pattern(J, d->b, v, e, n->flags == V_VAR ? B_VAR : n->flags == V_LET ? B_LET : B_CONST);
            if (J->throwing)
                return CT;
            continue;
        }
        if (d->a) {
            v = eval(J, d->a, e);
            if (J->throwing)
                return CT;
            if (js_is_callable(v) && v.u.o->fn && (!v.u.o->name || !*v.u.o->name))
                v.u.o->name = d->str->s;
        }
        if (n->flags == V_VAR) {
            if (!d->a)
                continue;
            /* the hoisted binding, wherever it is */
            struct env *s = e;
            struct binding *b = NULL;
            for (; s; s = s->parent) {
                for (b = s->vars; b; b = b->next)
                    if (str_eq(b->name, d->str))
                        break;
                if (b || s->obj || s->has_self)
                    break;
            }
            if (b)
                b->v = v;
            else if (s && s->obj)
                js_put(J, s->obj, d->str, v, 0);
            else if (s)
                declare(J, s, d->str, v, 0);
            else
                write_var(J, e, d->str, v);
        } else {
            if (e->obj)
                js_put(J, e->obj, d->str, v, 0);    /* top-level let/const: global */
            else
                declare(J, e, d->str, v, n->flags == V_CONST);
        }
    }
    return CN;
}

/* Bind a for-in/of loop variable. */
static void bind_loop_var(struct js *J, struct ast *target, struct env *e, jsval v)
{
    if (target->kind == S_VAR && target->list[0]->kind == N_DESTRUCT) {
        bind_pattern(J, target->list[0]->b, v, e,
                     target->flags == V_VAR ? B_VAR : target->flags == V_LET ? B_LET : B_CONST);
        return;
    }
    if (target->kind == N_ARRAY || target->kind == N_OBJECT) {
        bind_pattern(J, target, v, e, B_ASSIGN);
        return;
    }
    if (target->kind == S_VAR) {
        struct jsstr *name = target->list[0]->str;
        if (target->flags == V_VAR)
            write_var(J, e, name, v);
        else
            declare(J, e, name, v, target->flags == V_CONST);
        return;
    }
    struct ref r;
    if (make_ref(J, target, e, &r) == 0)
        ref_put(J, &r, e, v);
}

/* A loop body's completion: 1 to leave the loop with *result. */
static int loop_body_done(struct js *J, int c, struct jsstr *label, int *result)
{
    if (c == CB) {
        if (!J->label || (label && str_eq(J->label, label))) {
            J->label = NULL;
            *result = CN;
        } else {
            *result = CB;
        }
        return 1;
    }
    if (c == CC) {
        if (!J->label || (label && str_eq(J->label, label))) {
            J->label = NULL;
            return 0;
        }
        *result = CC;
        return 1;
    }
    if (c == CR || c == CT) {
        *result = c;
        return 1;
    }
    return 0;
}

static int exec_loop(struct js *J, struct ast *n, struct env *e, struct jsstr *label)
{
    int result = CN, c;
    switch (n->kind) {
    case S_WHILE:
        for (;;) {
            jsval t = eval(J, n->a, e);
            if (J->throwing)
                return CT;
            if (!js_truthy(t))
                return CN;
            c = exec(J, n->b, e);
            if (loop_body_done(J, c, label, &result))
                return result;
        }
    case S_DOWHILE:
        for (;;) {
            c = exec(J, n->b, e);
            if (loop_body_done(J, c, label, &result))
                return result;
            jsval t = eval(J, n->a, e);
            if (J->throwing)
                return CT;
            if (!js_truthy(t))
                return CN;
        }
    case S_FOR: {
        struct env *le = e;
        int lexical = n->a && n->a->kind == S_VAR && n->a->flags != V_VAR;
        if (lexical)
            le = new_env(J, e);
        if (n->a) {
            if (n->a->kind == S_VAR)
                c = exec_var(J, n->a, le);
            else {
                eval(J, n->a, le);
                c = throwing(J);
            }
            if (c == CT)
                return CT;
        }
        for (;;) {
            if (n->b) {
                jsval t = eval(J, n->b, le);
                if (J->throwing)
                    return CT;
                if (!js_truthy(t))
                    break;
            }
            c = exec(J, n->d, le);
            if (loop_body_done(J, c, label, &result))
                return result;
            if (lexical && le->captured) {
                /* closures keep this iteration's variables: copy into a fresh scope */
                struct env *next = new_env(J, e);
                for (struct binding *b = le->vars; b; b = b->next)
                    declare(J, next, b->name, b->v, b->konst);
                le = next;
            }
            if (n->c) {
                eval(J, n->c, le);
                if (J->throwing)
                    return CT;
            }
        }
        return CN;
    }
    case S_FORIN:
    case S_FOROF: {
        jsval obj = eval(J, n->b, e);
        if (J->throwing)
            return CT;
        int lexical = n->a->kind == S_VAR && n->a->flags != V_VAR;
        if (n->kind == S_FORIN) {
            if (obj.t == JS_UNDEF || obj.t == JS_NULL)
                return CN;
            struct jsstr **keys;
            int nk = js_keys(J, obj, 1, &keys);
            for (int i = 0; i < nk; i++) {
                struct env *le = lexical ? new_env(J, e) : e;
                bind_loop_var(J, n->a, le, strv(keys[i]));
                if (J->throwing)
                    return CT;
                c = exec(J, n->c, le);
                if (le != e)
                    recycle_env(J, le);
                if (loop_body_done(J, c, label, &result))
                    return result;
            }
            return CN;
        }
        struct iter it;
        memset(&it, 0, sizeof(it));
        jsval async_it = js_undefined, async_next = js_undefined;
        if (n->op) {                            /* for await: an async iterator, or a sync one whose values are awaited */
            jsval m = obj.t == JS_OBJ ? js_get(J, obj, "@@asyncIterator") : js_undefined;
            if (J->throwing)
                return CT;
            if (js_is_callable(m)) {
                async_it = js_call_value(J, m, obj, NULL, 0);
                if (J->throwing)
                    return CT;
                async_next = js_get(J, async_it, "next");
                if (J->throwing)
                    return CT;
                if (!js_is_callable(async_next)) {
                    js_throw_error(J, "TypeError: the async iterator has no next() method");
                    return CT;
                }
            }
        }
        if (async_next.t == JS_UNDEF && js_iter_open(J, obj, &it) < 0)
            return CT;
        for (;;) {
            jsval item;
            int r;
            if (async_next.t != JS_UNDEF) {
                jsval res = js_call_value(J, async_next, async_it, NULL, 0);
                if (J->throwing)
                    return CT;
                res = await_value(J, res);
                if (J->throwing)
                    return CT;
                if (res.t != JS_OBJ) {
                    js_throw_error(J, "TypeError: iterator result is not an object");
                    return CT;
                }
                jsval done = js_get(J, res, "done");
                if (J->throwing)
                    return CT;
                if (js_truthy(done))
                    break;
                item = js_get(J, res, "value");
                r = J->throwing ? -1 : 1;
            } else {
                r = js_iter_next(J, &it, &item);
                if (r > 0 && n->op) {
                    item = await_value(J, item);
                    if (J->throwing)
                        return CT;
                }
            }
            if (r < 0)
                return CT;
            if (!r)
                break;
            struct env *le = lexical ? new_env(J, e) : e;
            bind_loop_var(J, n->a, le, item);
            if (J->throwing) {
                js_iter_close(J, &it);
                return CT;
            }
            c = exec(J, n->c, le);
            if (le != e)
                recycle_env(J, le);
            if (loop_body_done(J, c, label, &result)) {
                js_iter_close(J, &it);
                return result;
            }
        }
        return CN;
    }
    }
    return CN;
}

static int exec_list(struct js *J, struct ast **list, int n, struct env *e)
{
    for (int i = 0; i < n; i++) {
        int c = exec(J, list[i], e);
        if (c != CN)
            return c;
    }
    return CN;
}

static int exec(struct js *J, struct ast *n, struct env *e)
{
    step(J);
    switch (n->kind) {
    case S_EMPTY:
    case S_FUNCDECL:                    /* hoisted */
    case S_IMPORT:                      /* bound when the module was linked */
        return CN;
    case S_EXPORT:
        if (n->op == EX_DECL)
            return exec(J, n->a, e);
        if (n->op == EX_DEFAULT) {
            jsval v = eval(J, n->a, e);
            if (J->throwing)
                return CT;
            if (js_is_callable(v) && v.u.o->fn && (!v.u.o->name || !*v.u.o->name))
                v.u.o->name = "default";
            declare(J, e, J->s_default, v, 0);
        }
        return CN;
    case S_EXPR:
        J->completion = eval(J, n->a, e);
        return throwing(J);
    case S_VAR:
        return exec_var(J, n, e);
    case S_BLOCK: {
        struct env *be = e;
        if (n->flags & F_SCOPED) {
            be = new_env(J, e);
            for (int i = 0; i < n->n; i++)
                if (n->list[i]->kind == S_FUNCDECL) {
                    struct ast *fn = n->list[i]->a;
                    jsval f = make_function(J, fn, be);
                    declare(J, be, fn->str, f, 0);
                    /* sloppy mode: visible in the function too */
                    struct env *fe = e;
                    while (fe && !fe->has_self && !fe->obj)
                        fe = fe->parent;
                    if (fe && fe->obj)
                        js_put(J, fe->obj, fn->str, f, 0);
                    else if (fe)
                        declare(J, fe, fn->str, f, 0);
                }
        }
        int c = exec_list(J, n->list, n->n, be);
        if (be != e)
            recycle_env(J, be);
        return c;
    }
    case S_IF: {
        jsval t = eval(J, n->a, e);
        if (J->throwing)
            return CT;
        if (js_truthy(t))
            return exec(J, n->b, e);
        return n->c ? exec(J, n->c, e) : CN;
    }
    case S_FOR:
    case S_FORIN:
    case S_FOROF:
    case S_WHILE:
    case S_DOWHILE:
        return exec_loop(J, n, e, NULL);
    case S_LABEL: {
        struct ast *body = n->a;
        int c;
        if (body->kind == S_FOR || body->kind == S_FORIN || body->kind == S_FOROF ||
            body->kind == S_WHILE || body->kind == S_DOWHILE)
            c = exec_loop(J, body, e, n->str);
        else
            c = exec(J, body, e);
        if (c == CB && J->label && str_eq(J->label, n->str)) {
            J->label = NULL;
            return CN;
        }
        return c;
    }
    case S_RETURN:
        J->retval = n->a ? eval(J, n->a, e) : js_undefined;
        return J->throwing ? CT : CR;
    case S_BREAK:
        J->label = n->str;
        return CB;
    case S_CONTINUE:
        J->label = n->str;
        return CC;
    case S_THROW: {
        jsval v = eval(J, n->a, e);
        if (J->throwing)
            return CT;
        js_throw_value(J, v);
        return CT;
    }
    case S_TRY: {
        int c = exec(J, n->a, e);
        if (c == CT && n->b && !(J->exc.t == JS_OBJ && J->exc.u.o == J->return_marker)) {
            jsval exc = J->exc;
            J->throwing = 0;
            J->exc = js_undefined;
            struct env *ce = new_env(J, e);
            if (n->str)
                declare(J, ce, n->str, exc, 0);
            else if (n->d)
                bind_pattern(J, n->d, exc, ce, B_LET);
            c = J->throwing ? CT : exec(J, n->b, ce);
            recycle_env(J, ce);
        }
        if (n->c) {
            int saved_throw = J->throwing;
            jsval saved_exc = J->exc, saved_ret = J->retval;
            struct jsstr *saved_label = J->label;
            J->throwing = 0;
            int f = exec(J, n->c, e);
            if (f != CN)
                return f;
            J->throwing = saved_throw;
            J->exc = saved_exc;
            J->retval = saved_ret;
            J->label = saved_label;
        }
        return c;
    }
    case S_SWITCH: {
        jsval d = eval(J, n->a, e);
        if (J->throwing)
            return CT;
        int start = -1;
        for (int i = 0; i < n->n && start < 0; i++) {
            struct ast *cs = n->list[i];
            if (!cs->a)
                continue;
            jsval v = eval(J, cs->a, e);
            if (J->throwing)
                return CT;
            if (js_strict_equal(d, v))
                start = i;
        }
        if (start < 0)
            for (int i = 0; i < n->n; i++)
                if (!n->list[i]->a)
                    start = i;
        if (start < 0)
            return CN;
        struct env *se = new_env(J, e);
        int c = CN;
        for (int i = start; i < n->n && c == CN; i++)
            c = exec_list(J, n->list[i]->list, n->list[i]->n, se);
        recycle_env(J, se);
        if (c == CB && !J->label)
            return CN;
        return c;
    }
    default:
        js_throw_error(J, "SyntaxError: unexpected statement");
        return CT;
    }
}

/* eval(): the code runs in the global scope (an indirect eval), and gives
 * back the value of its last expression statement. */
static jsval global_eval(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (argc < 1 || args[0].t != JS_STR)
        return argc ? args[0] : js_undefined;
    struct ast *prog = parse(J, args[0].u.s->s, args[0].u.s->len, "eval", 0);
    if (!prog)
        return js_throw_error(J, "%s", J->err);
    prepare_body(J, prog, prog->info);
    struct env *g = J->genv;
    for (int i = 0; i < prog->info->nvars; i++)
        declare_global(J, g, prog->info->vars[i], js_undefined, 0);
    for (int i = 0; i < prog->info->nfuncs; i++)
        declare_global(J, g, prog->info->funcs[i]->str, make_function(J, prog->info->funcs[i], g), 1);
    jsval saved = J->completion;
    J->completion = js_undefined;
    exec_list(J, prog->list, prog->n, g);
    jsval r = J->completion;
    J->completion = saved;
    return J->throwing ? js_undefined : r;
}

/* ---- coroutines: async functions and generators ---- */

/*
 * A function that awaits or yields runs on a stack of its own, so that it
 * can stop in the middle of an expression and carry on later. Switching
 * stacks only saves the registers the C calling convention says a callee
 * keeps; everything else lives in the engine's own structures. A coroutine
 * starts when its function is called (async) or on the first next()
 * (generators); await and yield switch back to whoever started or resumed
 * it. The stack is given back as soon as the function finishes.
 */
#ifndef CORO_STACK
#define CORO_STACK  (96 * 1024)
#endif
#define CORO_DEPTH  32                  /* nested calls allowed on one stack */
#define MAX_CORO    192
#define STACK_POOL  8

enum { CO_NEW, CO_SUSPENDED, CO_RUNNING, CO_DONE };
enum { CO_NEXT, CO_THROW, CO_RETURN };
enum { OUT_AWAIT, OUT_YIELD };

struct coro {
    struct coro *next_live, *prev_live;
    struct js *J;
    void *stack, *sp, *caller_sp;
    int state, depth, is_gen, live;
    struct jsobj *f;
    jsval self;
    jsval *args;
    int argc;
    jsval in_val;
    int in_mode;
    jsval out_val;
    int out_kind;
    jsval result, exc;
    int threw;
    jsval promise;                      /* async: what the call returned */
    struct coro *resumer;
};

void js_coro_switch(void **save_sp, void *new_sp);

#if defined(__i386__)
__asm__(".text\n"
        ".globl js_coro_switch\n"
        "js_coro_switch:\n"
        "    movl 4(%esp), %eax\n"
        "    movl 8(%esp), %edx\n"
        "    pushl %ebp\n"
        "    pushl %ebx\n"
        "    pushl %esi\n"
        "    pushl %edi\n"
        "    movl %esp, (%eax)\n"
        "    movl %edx, %esp\n"
        "    popl %edi\n"
        "    popl %esi\n"
        "    popl %ebx\n"
        "    popl %ebp\n"
        "    ret\n");
#elif defined(__x86_64__)
__asm__(".text\n"
        ".globl js_coro_switch\n"
        "js_coro_switch:\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        ".globl js_coro_boot\n"
        "js_coro_boot:\n"
        "    movq %r12, %rdi\n"
        "    callq *%r13\n"
        "    ud2\n");
void js_coro_boot(void);
#else
#error "coroutines need a context switch for this CPU"
#endif

static void *stack_pool[STACK_POOL];
static int nstack_pool;

static void *stack_get(void)
{
    if (nstack_pool)
        return stack_pool[--nstack_pool];
    return malloc(CORO_STACK);
}

static void stack_put(void *stack)
{
    if (nstack_pool < STACK_POOL)
        stack_pool[nstack_pool++] = stack;
    else
        free(stack);
}

static void co_main(void *arg) __attribute__((noreturn, used));

static void co_main(void *arg)
{
    struct coro *co = arg;
    struct js *J = co->J;
    jsval r = run_user(J, co->f, co->self, co->args, co->argc);
    if (J->throwing) {
        co->threw = 1;
        co->exc = J->exc;
        J->throwing = 0;
        J->exc = js_undefined;
        if (co->exc.t == JS_OBJ && co->exc.u.o == J->return_marker) {   /* generator.return(v) */
            co->threw = 0;
            r = js_get(J, co->exc, "value");
            J->throwing = 0;
        }
    }
    co->result = r;
    co->state = CO_DONE;
    js_coro_switch(&co->sp, co->caller_sp);
    __builtin_unreachable();
}

/* Lay out a new stack so that the first switch into it calls co_main(co). */
static void co_prepare(struct coro *co)
{
    uintptr_t top = ((uintptr_t)co->stack + CORO_STACK) & ~(uintptr_t)15;
#if defined(__i386__)
    uint32_t *sp = (uint32_t *)(top - 16);
    sp[0] = (uint32_t)(uintptr_t)co;            /* the argument, 16-byte aligned */
    *--sp = 0;                                  /* co_main never returns */
    *--sp = (uint32_t)(uintptr_t)co_main;       /* where the first switch's ret goes */
    *--sp = 0;                                  /* ebp, ebx, esi, edi as the switch pops them */
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    co->sp = sp;
#else
    uint64_t *sp = (uint64_t *)(top - 24);
    sp[0] = (uint64_t)(uintptr_t)js_coro_boot;
    *--sp = 0;                                  /* rbp */
    *--sp = 0;                                  /* rbx */
    *--sp = (uint64_t)(uintptr_t)co;            /* r12 */
    *--sp = (uint64_t)(uintptr_t)co_main;       /* r13 */
    *--sp = 0;                                  /* r14 */
    *--sp = 0;                                  /* r15 */
    co->sp = sp;
#endif
}

static void co_unlink(struct coro *co)
{
    if (!co->live)
        return;
    co->live = 0;
    if (co->prev_live)
        co->prev_live->next_live = co->next_live;
    else
        co->J->co_live = co->next_live;
    if (co->next_live)
        co->next_live->prev_live = co->prev_live;
    co->J->nco--;
    if (co->stack) {
        stack_put(co->stack);
        co->stack = NULL;
    }
}

static struct coro *co_new(struct js *J, struct jsobj *f, jsval self, jsval *args, int argc, int is_gen)
{
    if (J->nco >= MAX_CORO) {
        js_throw_error(J, "RangeError: too many async calls and generators are waiting");
        return NULL;
    }
    struct coro *co = js_alloc(J, sizeof(*co));
    co->J = J;
    co->f = f;
    co->self = self;
    co->argc = argc;
    co->is_gen = is_gen;
    co->args = js_alloc(J, sizeof(jsval) * (size_t)(argc ? argc : 1));
    if (argc)
        memcpy(co->args, args, sizeof(jsval) * (size_t)argc);
    co->depth = JS_MAX_DEPTH - CORO_DEPTH;
    co->state = CO_NEW;
    co->live = 1;
    co->next_live = J->co_live;
    if (J->co_live)
        J->co_live->prev_live = co;
    J->co_live = co;
    J->nco++;
    return co;
}

/* Run the coroutine until it awaits, yields or finishes. */
static void co_resume(struct js *J, struct coro *co, jsval v, int mode)
{
    if (co->state == CO_NEW) {
        co->stack = stack_get();
        if (!co->stack) {
            co->state = CO_DONE;
            co->threw = 1;
            co->exc = js_make_error(J, "RangeError", "out of memory for a coroutine");
            co_unlink(co);
            return;
        }
        co_prepare(co);
    }
    co->in_val = v;
    co->in_mode = mode;
    co->resumer = J->cur_co;
    J->cur_co = co;
    co->state = CO_RUNNING;
    int saved_depth = J->depth;
    struct jsstr *saved_label = J->label;
    jsval saved_ret = J->retval;
    J->depth = co->depth;
    js_coro_switch(&co->caller_sp, co->sp);
    co->depth = J->depth;
    J->depth = saved_depth;
    J->label = saved_label;
    J->retval = saved_ret;
    J->cur_co = co->resumer;
    if (co->state == CO_DONE)
        co_unlink(co);
}

/* Inside the coroutine: hand `out` to whoever resumed it; returns what it is resumed with. */
static jsval co_suspend(struct js *J, struct coro *co, int kind, jsval out)
{
    co->out_val = out;
    co->out_kind = kind;
    co->state = CO_SUSPENDED;
    js_coro_switch(&co->sp, co->caller_sp);
    if (co->in_mode == CO_THROW)
        return js_throw_value(J, co->in_val);
    if (co->in_mode == CO_RETURN) {             /* unwind through finally blocks to the top */
        js_put(J, J->return_marker, js_mkstr(J, "value", 5), co->in_val, 1);
        return js_throw_value(J, js_obj(J->return_marker));
    }
    return co->in_val;
}

static jsval await_value(struct js *J, jsval v)
{
    struct coro *co = J->cur_co;
    if (!co || co->is_gen == 1)
        return js_throw_error(J, "SyntaxError: await is only valid in an async function");
    return co_suspend(J, co, OUT_AWAIT, v);
}

static jsval yield_value(struct js *J, jsval v)
{
    struct coro *co = J->cur_co;
    if (!co || !co->is_gen)
        return js_throw_error(J, "SyntaxError: yield is only valid in a generator");
    return co_suspend(J, co, OUT_YIELD, v);
}

/* ---- async functions ---- */

static const struct jsclass coro_class = { "Coroutine", NULL, NULL, NULL };

static void co_step(struct js *J, struct coro *co, jsval v, int mode);

static jsval co_on_ok(struct js *J, jsval self, jsval *args, int argc)
{
    struct coro *co = self.t == JS_OBJ ? js_host_data(self.u.o, &coro_class) : NULL;
    if (co)
        co_step(J, co, argc ? args[0] : js_undefined, CO_NEXT);
    return js_undefined;
}

static jsval co_on_err(struct js *J, jsval self, jsval *args, int argc)
{
    struct coro *co = self.t == JS_OBJ ? js_host_data(self.u.o, &coro_class) : NULL;
    if (co)
        co_step(J, co, argc ? args[0] : js_undefined, CO_THROW);
    return js_undefined;
}

/* Run an async function's coroutine to its next await; settle or wait on a promise. */
static void co_step(struct js *J, struct coro *co, jsval v, int mode)
{
    co_resume(J, co, v, mode);
    if (co->state == CO_DONE) {
        if (co->threw)
            js_promise_settle(J, co->promise, 2, co->exc);
        else
            js_promise_resolve(J, co->promise, co->result);
        return;
    }
    jsval p = js_promise_from(J, co->out_val);
    js_promise_watch(J, p, co_on_ok, co_on_err, js_obj(js_new_host(J, &coro_class, co)));
}

/* ---- generators ---- */

struct gen {
    struct coro *co;
    int running;
};

static const struct jsclass gen_class = { "Generator", NULL, NULL, NULL };

static jsval iter_result(struct js *J, jsval value, int done)
{
    struct jsobj *o = js_new_object(J);
    js_put(J, o, js_mkstr(J, "value", 5), value, 0);
    js_put(J, o, js_mkstr(J, "done", 4), js_bool(done), 0);
    return js_obj(o);
}

static jsval gen_resume(struct js *J, jsval self, jsval v, int mode)
{
    struct gen *g = self.t == JS_OBJ ? js_host_data(self.u.o, &gen_class) : NULL;
    if (!g)
        return js_throw_error(J, "TypeError: not a generator");
    struct coro *co = g->co;
    if (g->running)
        return js_throw_error(J, "TypeError: the generator is already running");
    if (co->state == CO_DONE || (co->state == CO_NEW && mode != CO_NEXT)) {
        if (co->state == CO_NEW) {              /* closed before it started */
            co->state = CO_DONE;
            co_unlink(co);
        }
        if (mode == CO_THROW)
            return js_throw_value(J, v);
        return iter_result(J, mode == CO_RETURN ? v : js_undefined, 1);
    }
    g->running = 1;
    co_resume(J, co, v, mode);
    g->running = 0;
    if (co->state == CO_DONE) {
        if (co->threw) {
            co->threw = 0;
            return js_throw_value(J, co->exc);
        }
        return iter_result(J, co->result, 1);
    }
    return iter_result(J, co->out_val, 0);
}

static jsval gen_next(struct js *J, jsval self, jsval *args, int argc)
{
    return gen_resume(J, self, argc ? args[0] : js_undefined, CO_NEXT);
}

static jsval gen_throw(struct js *J, jsval self, jsval *args, int argc)
{
    return gen_resume(J, self, argc ? args[0] : js_undefined, CO_THROW);
}

static jsval gen_return(struct js *J, jsval self, jsval *args, int argc)
{
    return gen_resume(J, self, argc ? args[0] : js_undefined, CO_RETURN);
}

static jsval gen_self(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)args;
    (void)argc;
    return self;
}

static jsval agen_next(struct js *J, jsval self, jsval *args, int argc);
static jsval agen_return(struct js *J, jsval self, jsval *args, int argc);
static jsval agen_throw(struct js *J, jsval self, jsval *args, int argc);

static int in_async_gen(struct js *J)
{
    return J->cur_co && J->cur_co->is_gen == 2;
}

static void init_generators(struct js *J)
{
    struct jsobj *gp = J->generator_proto = js_mkobj(J, C_OBJECT, J->object_proto);
    js_define_native(J, gp, "next", gen_next, 1);
    js_define_native(J, gp, "return", gen_return, 1);
    js_define_native(J, gp, "throw", gen_throw, 1);
    js_define_native(J, gp, "@@iterator", gen_self, 0);
    J->return_marker = js_new_object(J);
    struct jsobj *ap = J->agen_proto = js_mkobj(J, C_OBJECT, J->object_proto);
    js_define_native(J, ap, "next", agen_next, 1);
    js_define_native(J, ap, "return", agen_return, 1);
    js_define_native(J, ap, "throw", agen_throw, 1);
    js_define_native(J, ap, "@@asyncIterator", gen_self, 0);
}

/* ---- async generators ----
 * next(), throw() and return() queue a request and return its promise; the
 * coroutine serves them in order, waiting through its awaits. */

struct agen_req {
    struct agen_req *next;
    int mode;
    jsval v, promise;
};

struct agen {
    struct coro *co;
    struct agen_req *head, *tail;
    int running;
};

static const struct jsclass agen_class = { "AsyncGenerator", NULL, NULL, NULL };

static void agen_drain(struct js *J, struct agen *g);

/* The request at the head is answered: settle it and serve the next. */
static void agen_answer(struct js *J, struct agen *g, int reject, jsval v, int done)
{
    struct agen_req *r = g->head;
    g->head = r->next;
    if (!g->head)
        g->tail = NULL;
    g->running = 0;
    if (reject)
        js_promise_settle(J, r->promise, 2, v);
    else
        js_promise_resolve(J, r->promise, iter_result(J, v, done));
}

static void agen_after(struct js *J, struct agen *g);

static jsval agen_on_ok(struct js *J, jsval self, jsval *args, int argc)
{
    struct agen *g = self.t == JS_OBJ ? js_host_data(self.u.o, &agen_class) : NULL;
    if (g) {
        co_resume(J, g->co, argc ? args[0] : js_undefined, CO_NEXT);
        agen_after(J, g);
    }
    return js_undefined;
}

static jsval agen_on_err(struct js *J, jsval self, jsval *args, int argc)
{
    struct agen *g = self.t == JS_OBJ ? js_host_data(self.u.o, &agen_class) : NULL;
    if (g) {
        co_resume(J, g->co, argc ? args[0] : js_undefined, CO_THROW);
        agen_after(J, g);
    }
    return js_undefined;
}

/* The coroutine stopped: it finished, yielded, or awaits something. */
static void agen_after(struct js *J, struct agen *g)
{
    struct coro *co = g->co;
    if (co->state == CO_DONE) {
        int threw = co->threw;
        co->threw = 0;
        agen_answer(J, g, threw, threw ? co->exc : co->result, 1);
        agen_drain(J, g);
        return;
    }
    if (co->out_kind == OUT_AWAIT) {
        jsval p = js_promise_from(J, co->out_val);
        js_promise_watch(J, p, agen_on_ok, agen_on_err, js_obj(js_new_host(J, &agen_class, g)));
        return;
    }
    agen_answer(J, g, 0, co->out_val, 0);
    agen_drain(J, g);
}

static void agen_drain(struct js *J, struct agen *g)
{
    while (g->head && !g->running) {
        struct agen_req *r = g->head;
        struct coro *co = g->co;
        if (co->state == CO_NEW && r->mode != CO_NEXT) {        /* closed before it started */
            co->state = CO_DONE;
            co_unlink(co);
        }
        if (co->state == CO_DONE) {
            agen_answer(J, g, r->mode == CO_THROW, r->mode == CO_NEXT ? js_undefined : r->v, 1);
            continue;
        }
        g->running = 1;
        co_resume(J, co, r->v, r->mode);
        agen_after(J, g);
    }
}

static jsval agen_request(struct js *J, jsval self, jsval v, int mode)
{
    struct agen *g = self.t == JS_OBJ ? js_host_data(self.u.o, &agen_class) : NULL;
    jsval p = js_promise_new(J);
    if (!g) {
        js_promise_settle(J, p, 2, js_make_error(J, "TypeError", "not an async generator"));
        return p;
    }
    struct agen_req *r = js_alloc(J, sizeof(*r));
    r->mode = mode;
    r->v = v;
    r->promise = p;
    if (g->tail)
        g->tail->next = r;
    else
        g->head = r;
    g->tail = r;
    agen_drain(J, g);
    return p;
}

static jsval agen_next(struct js *J, jsval self, jsval *args, int argc)
{
    return agen_request(J, self, argc ? args[0] : js_undefined, CO_NEXT);
}

static jsval agen_throw(struct js *J, jsval self, jsval *args, int argc)
{
    return agen_request(J, self, argc ? args[0] : js_undefined, CO_THROW);
}

static jsval agen_return(struct js *J, jsval self, jsval *args, int argc)
{
    return agen_request(J, self, argc ? args[0] : js_undefined, CO_RETURN);
}

static jsval start_coroutine(struct js *J, struct jsobj *f, jsval self, jsval *args, int argc)
{
    if ((f->fn->flags & F_GEN) && (f->fn->flags & F_ASYNC)) {
        struct coro *co = co_new(J, f, self, args, argc, 2);
        if (!co)
            return js_undefined;
        struct agen *g = js_alloc(J, sizeof(*g));
        g->co = co;
        struct jsobj *o = js_new_host(J, &agen_class, g);
        o->proto = J->agen_proto;
        return js_obj(o);
    }
    if (f->fn->flags & F_GEN) {
        struct coro *co = co_new(J, f, self, args, argc, 1);
        if (!co)
            return js_undefined;
        struct gen *g = js_alloc(J, sizeof(*g));
        g->co = co;
        struct jsobj *o = js_new_host(J, &gen_class, g);
        jsval proto = js_get(J, js_obj(f), "prototype");    /* generator.prototype inherits next() */
        o->proto = proto.t == JS_OBJ && proto.u.o->proto == J->object_proto ? proto.u.o : J->generator_proto;
        if (o->proto != J->generator_proto && o->proto->proto == J->object_proto)
            o->proto->proto = J->generator_proto;
        return js_obj(o);
    }
    struct coro *co = co_new(J, f, self, args, argc, 0);
    if (!co)
        return js_undefined;
    co->promise = js_promise_new(J);
    co_step(J, co, js_undefined, CO_NEXT);
    return co->promise;
}

static void co_free_all(struct js *J)
{
    while (J->co_live) {
        struct coro *co = J->co_live;
        J->co_live = co->next_live;
        if (co->stack) {
            free(co->stack);
            co->stack = NULL;
        }
        co->live = 0;
    }
    J->nco = 0;
}

/* ---- for the garbage collector ---- */

#define MAIN_STACK_TOP 0xC0000000u      /* the kernel puts a program's stack below here */
#define MAIN_STACK_LOW (MAIN_STACK_TOP - (8u << 20))

static int on_stack(const char *p, const char *lo, const char *hi)
{
    return p >= lo && p < hi;
}

/*
 * The main stack and every coroutine's: each is scanned from the lowest
 * place its frames may reach: where the engine is running now, where a
 * coroutine switched away from it, or where it was left suspended.
 */
int js_gc_scan_stacks(struct js *J, const char *sp,
                      void (*scan)(struct js *J, const void *lo, const void *hi))
{
    const char *mlo = (const char *)MAIN_STACK_LOW, *mhi = (const char *)MAIN_STACK_TOP;
    int known = on_stack(sp, mlo, mhi);
    const char *main_from = known ? sp : NULL;
    for (struct coro *co = J->co_live; co; co = co->next_live) {
        if (!co->stack)
            continue;
        const char *lo = co->stack, *hi = (const char *)co->stack + CORO_STACK;
        const char *from = on_stack(co->sp, lo, hi) ? co->sp : lo;
        if (on_stack(sp, lo, hi)) {
            known = 1;
            if (sp < from)
                from = sp;
        }
        for (struct coro *x = J->co_live; x; x = x->next_live)
            if (on_stack(x->caller_sp, lo, hi) && (const char *)x->caller_sp < from)
                from = x->caller_sp;
        scan(J, from, hi);
    }
    for (struct coro *x = J->co_live; x; x = x->next_live)
        if (on_stack(x->caller_sp, mlo, mhi) && (!main_from || (const char *)x->caller_sp < main_from))
            main_from = x->caller_sp;
    if (!known)
        return -1;
    if (main_from)
        scan(J, main_from, mhi);
    return 0;
}

void js_gc_before(struct js *J)
{
    if (free_owner == J) {              /* recycled scopes: garbage now, unless still in use */
        free_envs = NULL;
        free_bindings = NULL;
    }
}

/* ---- microtasks ---- */

void js_enqueue(struct js *J, void (*run)(struct js *, struct job *), jsval a, jsval b, jsval c, jsval d)
{
    struct job *j = js_alloc(J, sizeof(*j));
    j->run = run;
    j->a = a;
    j->b = b;
    j->c = c;
    j->d = d;
    if (J->jobs_tail)
        J->jobs_tail->next = j;
    else
        J->jobs = j;
    J->jobs_tail = j;
}

/* Run the queued microtasks, and those they queue, until none are left. */
void js_run_jobs(struct js *J)
{
    if (J->in_jobs)
        return;
    J->in_jobs = 1;
    while (J->jobs && !J->dead) {
        struct job *j = J->jobs;
        J->jobs = j->next;
        if (!J->jobs)
            J->jobs_tail = NULL;
        j->run(J, j);
        J->throwing = 0;                /* a job reports its own errors */
        J->exc = js_undefined;
    }
    J->in_jobs = 0;
}

/* ---- the interface ---- */

static int enter(struct js *J)
{
    if (J->dead)
        return -1;
    if (!entered) {
        J->steps = 0;
        J->depth = 0;
    }
    active = J;
    if (free_owner != J) {
        free_owner = J;
        free_envs = NULL;
        free_bindings = NULL;
    }
    return 0;
}

static void error_text(struct js *J, jsval exc)
{
    J->throwing = 0;
    if (exc.t == JS_OBJ && exc.u.o->cls == C_ERROR) {
        jsval name = js_get(J, exc, "name"), msg = js_get(J, exc, "message");
        J->throwing = 0;
        snprintf(J->err, sizeof(J->err), "%s: %s", name.t == JS_STR ? name.u.s->s : "Error",
                 msg.t == JS_STR ? msg.u.s->s : "");
    } else {
        snprintf(J->err, sizeof(J->err), "uncaught %s", js_text(J, exc));
    }
}

int js_eval(struct js *J, const char *src, int len, const char *where)
{
    if (enter(J) < 0)
        return -1;
    int outer = !entered;
    if (outer) {
        entered = 1;
        if (__builtin_setjmp(jmp_target)) {
            entered = 0;
            free_owner = NULL;
            return -1;
        }
    }
    int r = 0;
    struct ast *prog = parse(J, src, len, where, 0);
    if (!prog) {
        r = -1;
    } else {
        prepare_body(J, prog, prog->info);
        struct env *g = J->genv;
        for (int i = 0; i < prog->info->nvars; i++)
            declare_global(J, g, prog->info->vars[i], js_undefined, 0);
        for (int i = 0; i < prog->info->nfuncs; i++)
            declare_global(J, g, prog->info->funcs[i]->str, make_function(J, prog->info->funcs[i], g), 1);
        int c = exec_list(J, prog->list, prog->n, g);
        if (c == CT) {
            error_text(J, J->exc);
            r = -1;
        }
    }
    J->throwing = 0;
    if (outer)
        js_run_jobs(J);
    if (outer)
        entered = 0;
    return r;
}

int js_call(struct js *J, jsval fn, jsval self, jsval *args, int argc, jsval *result)
{
    if (enter(J) < 0)
        return -1;
    int outer = !entered;
    if (outer) {
        entered = 1;
        if (__builtin_setjmp(jmp_target)) {
            entered = 0;
            free_owner = NULL;
            return -1;
        }
    }
    jsval r = js_call_value(J, fn, self, args, argc);
    int rc = 0;
    if (J->throwing) {
        error_text(J, J->exc);
        rc = -1;
        r = js_undefined;
    }
    if (result)
        *result = r;
    if (outer) {
        char err[sizeof(J->err)];
        memcpy(err, J->err, sizeof(err));
        js_run_jobs(J);
        if (rc < 0)
            memcpy(J->err, err, sizeof(err));    /* the call's error, not a job's */
        entered = 0;
    }
    return rc;
}

jsval js_compile_handler(struct js *J, const char *body, const char *where)
{
    if (enter(J) < 0)
        return js_undefined;
    int outer = !entered;
    if (outer) {
        entered = 1;
        if (__builtin_setjmp(jmp_target)) {
            entered = 0;
            free_owner = NULL;
            return js_undefined;
        }
    }
    size_t len = strlen(body);
    char *src = malloc(len + 32);
    jsval f = js_undefined;
    if (src) {
        int n = snprintf(src, len + 32, "(function(event){%s\n})", body);
        struct ast *x = parse(J, src, n, where, 1);
        free(src);
        if (x)
            f = eval(J, x, J->genv);
        J->throwing = 0;
    }
    if (outer)
        entered = 0;
    return f;
}

/* ---- modules ----
 *
 * Every module the page imports is fetched once (by URL) and kept here.
 * A module script or import() waits as an entry until every module in
 * its graph is parsed (or one failed); then the graph is linked, each
 * module getting a scope whose imports alias the exporters' bindings,
 * and run dependencies first. A module that awaits at its top level runs
 * as an async function, and what comes after it waits for it.
 */

enum { MS_FETCHING, MS_LOADED, MS_LINKED, MS_EVALUATING, MS_DONE, MS_FAILED };

struct jsmod {
    struct jsmod *next;
    int id;
    struct jsstr *url;
    uint8_t state, inline_src, async;
    struct ast *prog;
    struct env *env;
    struct jsobj *ns;                   /* the namespace object, once asked for */
    jsval error;                        /* MS_FAILED */
    jsval promise;                      /* an async module's evaluation */
    struct jsmod **deps;                /* one per import or export ... from, in order */
    int ndeps;
    unsigned mark;
};

struct jsentry {
    struct jsentry *next;
    struct jsmod *m;
    void *tag;                          /* a module script's, for the host */
    jsval promise;                      /* import(): settles with the namespace */
    int dynamic;
};

struct modjob {                         /* running a graph: its modules in order */
    struct jsentry *en;
    struct jsmod **order;
    int n, i;
};

struct resolved {
    struct jsmod *m;                    /* the module and binding that hold the export */
    struct jsstr *local;
    struct jsmod *ns;                   /* or: the export is this module's namespace */
};

void js_module_host(struct js *J, const struct js_modhost *h)
{
    J->modhost = *h;
}

static struct jsmod *mod_new(struct js *J, const char *url, int inline_src)
{
    struct jsmod *m = js_alloc(J, sizeof(*m));
    m->id = ++J->mod_ids;
    m->url = js_mkstr(J, url, (int)strlen(url));
    m->inline_src = (uint8_t)inline_src;
    m->error = js_undefined;
    m->promise = js_undefined;
    m->next = J->mods;
    J->mods = m;
    return m;
}

static void mod_fail(struct js *J, struct jsmod *m, jsval error)
{
    m->state = MS_FAILED;
    m->error = error;
    (void)J;
}

/* The module at url, asked for from the host if it is new. */
static struct jsmod *mod_request(struct js *J, const char *url)
{
    for (struct jsmod *m = J->mods; m; m = m->next)
        if (!m->inline_src && !strcmp(m->url->s, url))
            return m;
    struct jsmod *m = mod_new(J, url, 0);
    if (J->modhost.fetch)
        J->modhost.fetch(J->modhost.ctx, m->id, url);
    else
        mod_fail(J, m, js_make_error(J, "TypeError", "modules cannot be fetched here"));
    return m;
}

static int resolve_spec(struct js *J, const char *spec, const char *base, char *out, int size)
{
    if (J->modhost.resolve)
        return J->modhost.resolve(J->modhost.ctx, spec, base, out, size);
    snprintf(out, (size_t)size, "%s", spec);
    return 0;
}

/* A module's source is here: parse it and ask for what it imports. */
static void mod_set_source(struct js *J, struct jsmod *m, const char *src, int len)
{
    struct ast *prog = parse(J, src, len, m->url->s, 2);
    if (!prog) {
        mod_fail(J, m, js_make_error(J, "SyntaxError", J->err));
        return;
    }
    prepare_body(J, prog, prog->info);
    int n = 0;
    for (int i = 0; i < prog->n; i++)
        if ((prog->list[i]->kind == S_IMPORT || prog->list[i]->kind == S_EXPORT) && prog->list[i]->str)
            prog->list[i]->num = n++;
    struct jsmod **deps = js_alloc(J, sizeof(*deps) * (size_t)(n ? n : 1));
    char url[URL_MAX];
    for (int i = 0; i < n; i++)
        deps[i] = NULL;
    m->prog = prog;
    m->async = prog->op;
    m->deps = deps;
    m->ndeps = n;
    m->state = MS_LOADED;
    for (int i = 0, k = 0; i < prog->n; i++) {
        struct ast *s = prog->list[i];
        if ((s->kind != S_IMPORT && s->kind != S_EXPORT) || !s->str)
            continue;
        if (resolve_spec(J, s->str->s, m->url->s, url, sizeof(url)) < 0) {
            char msg[300];
            snprintf(msg, sizeof(msg), "Failed to resolve module specifier \"%.200s\"", s->str->s);
            mod_fail(J, m, js_make_error(J, "TypeError", msg));
            return;
        }
        deps[k++] = mod_request(J, url);
    }
}

/* Is every module m leads to here? 1 yes, 0 not yet, -1 one failed (its error in *err). */
static int graph_state(struct js *J, struct jsmod *m, jsval *err)
{
    if (m->mark == J->mod_mark)
        return 1;
    m->mark = J->mod_mark;
    if (m->state == MS_FAILED) {
        *err = m->error;
        return -1;
    }
    if (m->state == MS_FETCHING)
        return 0;
    int r = 1;
    for (int i = 0; i < m->ndeps; i++) {
        int s = m->deps[i] ? graph_state(J, m->deps[i], err) : 0;
        if (s < 0)
            return -1;
        if (!s)
            r = 0;
    }
    return r;
}

/* The modules m leads to, dependencies before what imports them. */
static void post_order(struct js *J, struct jsmod *m, struct jsmod ***list, int *n, int *cap)
{
    if (!m || m->mark == J->mod_mark)
        return;
    m->mark = J->mod_mark;
    for (int i = 0; i < m->ndeps; i++)
        post_order(J, m->deps[i], list, n, cap);
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        struct jsmod **nl = js_alloc(J, sizeof(*nl) * (size_t)nc);
        if (*n)
            memcpy(nl, *list, sizeof(*nl) * (size_t)*n);
        *list = nl;
        *cap = nc;
    }
    (*list)[(*n)++] = m;
}

static struct binding *find_binding(struct env *e, struct jsstr *name)
{
    for (struct binding *b = e->vars; b; b = b->next)
        if (str_eq(b->name, name))
            return b;
    return NULL;
}

/* Where module m's export `name` lives, following re-exports. */
static int resolve_export(struct js *J, struct jsmod *m, const char *name, struct resolved *r, int depth)
{
    if (depth > 64 || !m || !m->prog)
        return 0;
    struct ast *prog = m->prog;
    for (int i = 0; i < prog->n; i++) {
        struct ast *s = prog->list[i];
        if (s->kind != S_EXPORT)
            continue;
        if (s->op == EX_DEFAULT) {
            if (!strcmp(name, "default")) {
                r->m = m, r->local = J->s_default, r->ns = NULL;
                return 1;
            }
            continue;
        }
        for (int k = 0; k < s->n; k++) {
            struct ast *p = s->list[k];
            if (strcmp(p->b->str->s, name))
                continue;
            if (!s->str) {
                r->m = m, r->local = p->a->str, r->ns = NULL;
                return 1;
            }
            struct jsmod *dep = m->deps[(int)s->num];
            if (str_is(p->a->str, "*")) {
                r->m = NULL, r->local = NULL, r->ns = dep;
                return 1;
            }
            return resolve_export(J, dep, p->a->str->s, r, depth + 1);
        }
    }
    if (!strcmp(name, "default"))
        return 0;
    for (int i = 0; i < prog->n; i++) {         /* export * from "x" */
        struct ast *s = prog->list[i];
        if (s->kind == S_EXPORT && s->op == EX_ALL && !s->n &&
            resolve_export(J, m->deps[(int)s->num], name, r, depth + 1))
            return 1;
    }
    return 0;
}

static struct jsobj *mod_namespace(struct js *J, struct jsmod *m);

/* The value an import sees: live, from the exporter's binding. */
static int ns_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    struct jsmod *m = o->hdata;
    struct resolved r;
    if (!resolve_export(J, m, key, &r, 0))
        return 0;
    if (r.ns) {
        *out = js_obj(mod_namespace(J, r.ns));
        return 1;
    }
    struct binding *b = r.m->env ? find_binding(r.m->env, r.local) : NULL;
    while (b && b->alias)
        b = b->alias;
    *out = b ? b->v : js_undefined;
    return 1;
}

static int ns_set(struct js *J, struct jsobj *o, const char *key, jsval v)
{
    (void)J, (void)o, (void)key, (void)v;
    return 1;                           /* read-only */
}

static const struct jsclass ns_class = { "Module", ns_get, ns_set, NULL };

/* Every name module m exports: own properties of the namespace, for Object.keys. */
static void ns_names(struct js *J, struct jsmod *m, struct jsobj *ns, int star, int depth)
{
    if (!m || !m->prog || depth > 16)
        return;
    for (int i = 0; i < m->prog->n; i++) {
        struct ast *s = m->prog->list[i];
        if (s->kind != S_EXPORT)
            continue;
        if (s->op == EX_ALL && !s->n) {
            ns_names(J, m->deps[(int)s->num], ns, 1, depth + 1);
            continue;
        }
        if (s->op == EX_DEFAULT) {
            if (!star && !js_own(ns, J->s_default))
                js_put(J, ns, js_mkstr(J, "default", 7), js_undefined, 0);
            continue;
        }
        for (int k = 0; k < s->n; k++) {
            struct jsstr *name = s->list[k]->b->str;
            if ((!star || !str_is(name, "default")) && !js_own(ns, name))
                js_put(J, ns, name, js_undefined, 0);
        }
    }
}

static struct jsobj *mod_namespace(struct js *J, struct jsmod *m)
{
    if (!m->ns) {
        struct jsobj *ns = js_new_host(J, &ns_class, m);
        ns->proto = NULL;
        ns_names(J, m, ns, 0, 0);
        m->ns = ns;
    }
    return m->ns;
}

/* Give a loaded module its scope: hoisted functions, and every top-level name. */
static void mod_instantiate(struct js *J, struct jsmod *m)
{
    struct env *e = js_alloc(J, sizeof(*e));
    e->parent = J->genv;
    e->has_self = 1;                    /* this is undefined at a module's top level */
    e->self = js_undefined;
    e->captured = 1;
    m->env = e;
    struct fninfo *fi = m->prog->info, lex = { 0 };
    int cap = 0;
    for (int i = 0; i < m->prog->n; i++) {
        struct ast *s = m->prog->list[i];
        if (s->kind == S_EXPORT && s->op == EX_DECL)
            s = s->a;
        if (s->kind != S_VAR || s->flags == V_VAR)
            continue;
        for (int k = 0; k < s->n; k++) {
            if (s->list[k]->kind == N_DESTRUCT)
                pattern_vars(J, s->list[k]->b, &lex, &cap);
            else
                add_var(J, &lex, &cap, s->list[k]->str);
        }
    }
    for (int i = 0; i < lex.nvars; i++)
        declare(J, e, lex.vars[i], js_undefined, 0);
    for (int i = 0; i < fi->nvars; i++)
        if (!find_binding(e, fi->vars[i]))
            declare(J, e, fi->vars[i], js_undefined, 0);
    declare(J, e, J->s_default, js_undefined, 0);
    struct jsobj *meta = js_new_object(J);
    js_put(J, meta, js_mkstr(J, "url", 3), strv(m->url), 0);
    declare(J, e, J->s_meta, js_obj(meta), 1);
    for (int i = 0; i < fi->nfuncs; i++)
        declare(J, e, fi->funcs[i]->str, make_function(J, fi->funcs[i], e), 0);
    m->state = MS_LINKED;
}

/* Point a linked module's imports at what they import; 0, or -1 with it failed. */
static int mod_bind_imports(struct js *J, struct jsmod *m)
{
    for (int i = 0; i < m->prog->n; i++) {
        struct ast *s = m->prog->list[i];
        if (s->kind != S_IMPORT || !s->str)
            continue;
        struct jsmod *dep = m->deps[(int)s->num];
        for (int k = 0; k < s->n; k++) {
            struct jsstr *name = s->list[k]->a->str, *local = s->list[k]->b->str;
            struct resolved r;
            if (str_is(name, "*")) {
                declare(J, m->env, local, js_obj(mod_namespace(J, dep)), 1);
                continue;
            }
            if (!resolve_export(J, dep, name->s, &r, 0)) {
                char msg[400];
                snprintf(msg, sizeof(msg), "The requested module '%.150s' does not provide an export named '%.100s'",
                         s->str->s, name->s);
                mod_fail(J, m, js_make_error(J, "SyntaxError", msg));
                return -1;
            }
            if (r.ns) {
                declare(J, m->env, local, js_obj(mod_namespace(J, r.ns)), 1);
                continue;
            }
            struct binding *target = r.m->env ? find_binding(r.m->env, r.local) : NULL;
            struct binding *b = declare(J, m->env, local, js_undefined, 1);
            if (target != b)
                b->alias = target;
        }
    }
    return 0;
}

/* Link every module in m's graph that is not linked yet. */
static int mod_link(struct js *J, struct jsmod *m, jsval *err)
{
    struct jsmod **list = NULL;
    int n = 0, cap = 0;
    J->mod_mark++;
    post_order(J, m, &list, &n, &cap);
    int fresh = 0;
    for (int i = 0; i < n; i++)
        if (list[i]->state == MS_LOADED) {
            mod_instantiate(J, list[i]);
            list[fresh++] = list[i];
        }
    for (int i = 0; i < fresh; i++)
        if (mod_bind_imports(J, list[i]) < 0) {
            *err = list[i]->error;
            return -1;
        }
    return 0;
}

static void mod_entry_done(struct js *J, struct jsentry *en, int failed, jsval err)
{
    if (en->dynamic) {
        if (failed)
            js_promise_settle(J, en->promise, 2, err);
        else
            js_promise_resolve(J, en->promise, js_obj(mod_namespace(J, en->m)));
        return;
    }
    if (failed)
        error_text(J, err);
    if (J->modhost.done)
        J->modhost.done(J->modhost.ctx, en->tag, failed);
}

static const struct jsclass modjob_class = { "ModuleJob", NULL, NULL, NULL };
static void mod_job_run(struct js *J, struct modjob *job);

static jsval mod_job_resume(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args, (void)argc;
    struct modjob *job = self.t == JS_OBJ ? js_host_data(self.u.o, &modjob_class) : NULL;
    if (job)
        mod_job_run(J, job);
    return js_undefined;
}

/* An async module that has finished: done, or failed. */
static void mod_settle(struct js *J, struct jsmod *m)
{
    jsval v;
    int st;
    if (m->state != MS_EVALUATING || (st = js_promise_state(m->promise, &v)) <= 0)
        return;
    if (st == 1)
        m->state = MS_DONE;
    else
        mod_fail(J, m, v);
}

/* Wait for promise p, then go on with the job. */
static void mod_job_wait(struct js *J, struct modjob *job, jsval p)
{
    jsval self = js_obj(js_new_host(J, &modjob_class, job));
    js_promise_watch(J, p, mod_job_resume, mod_job_resume, self);
}

static void mod_job_run(struct js *J, struct modjob *job)
{
    while (job->i < job->n) {
        struct jsmod *m = job->order[job->i];
        mod_settle(J, m);
        if (m->state == MS_FAILED) {
            mod_entry_done(J, job->en, 1, m->error);
            return;
        }
        if (m->state == MS_DONE) {
            job->i++;
            continue;
        }
        if (m->state == MS_EVALUATING) {        /* an async module, still running */
            mod_job_wait(J, job, m->promise);
            return;
        }
        for (int i = 0; i < m->ndeps; i++) {    /* run elsewhere and not finished yet */
            struct jsmod *d = m->deps[i];
            mod_settle(J, d);
            if (d->state == MS_EVALUATING && d->async) {
                mod_job_wait(J, job, d->promise);
                return;
            }
            if (d->state == MS_FAILED) {
                mod_fail(J, m, d->error);
                break;
            }
        }
        if (m->state == MS_FAILED)
            continue;
        struct ast *fn = js_alloc(J, sizeof(*fn));
        fn->kind = N_FUNC;
        fn->flags = F_MODULE | (m->async ? F_ASYNC : 0);
        fn->a = m->prog;
        fn->info = m->prog->info;
        struct jsobj *f = js_mkobj(J, C_FUNCTION, J->function_proto);
        f->fn = fn;
        f->scope = m->env;
        f->name = "";
        m->state = MS_EVALUATING;
        jsval r = js_call_value(J, js_obj(f), js_undefined, NULL, 0);
        if (J->throwing) {
            jsval exc = J->exc;
            J->throwing = 0;
            J->exc = js_undefined;
            mod_fail(J, m, exc);
            continue;
        }
        if (m->async)
            m->promise = r;
        else
            m->state = MS_DONE;
    }
    mod_entry_done(J, job->en, 0, js_undefined);
}

/* Link and run the entries whose graphs are all here; fail those that cannot be. */
static void mod_progress(struct js *J)
{
    if (J->mod_busy) {
        J->mod_again = 1;
        return;
    }
    J->mod_busy = 1;
    do {
        J->mod_again = 0;
        struct jsentry **pp = &J->entries;
        while (*pp) {
            struct jsentry *en = *pp;
            jsval err = js_undefined;
            J->mod_mark++;
            int st = graph_state(J, en->m, &err);
            if (!st) {
                pp = &en->next;
                continue;
            }
            *pp = en->next;
            if (st > 0 && mod_link(J, en->m, &err) < 0)
                st = -1;
            if (st < 0) {
                mod_entry_done(J, en, 1, err);
                continue;
            }
            struct modjob *job = js_alloc(J, sizeof(*job));
            job->en = en;
            int cap = 0;
            J->mod_mark++;
            post_order(J, en->m, &job->order, &job->n, &cap);
            mod_job_run(J, job);        /* their import()s wait for a microtask */
        }
    } while (J->mod_again);
    J->mod_busy = 0;
}

static void mod_add_entry(struct js *J, struct jsentry *en)
{
    struct jsentry **pp = &J->entries;
    while (*pp)
        pp = &(*pp)->next;
    *pp = en;
}

static void mod_progress_job(struct js *J, struct job *job)
{
    (void)job;
    mod_progress(J);
}

/* import(spec): a promise of the module's namespace. */
static jsval mod_import(struct js *J, const char *spec, const char *base)
{
    jsval p = js_promise_new(J);
    char url[URL_MAX];
    if (resolve_spec(J, spec, base, url, sizeof(url)) < 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Failed to resolve module specifier \"%.200s\"", spec);
        js_promise_settle(J, p, 2, js_make_error(J, "TypeError", msg));
        return p;
    }
    struct jsentry *en = js_alloc(J, sizeof(*en));
    en->promise = p;
    en->dynamic = 1;
    en->m = mod_request(J, url);
    mod_add_entry(J, en);
    /* never synchronously: the importing code goes on first */
    js_enqueue(J, mod_progress_job, js_undefined, js_undefined, js_undefined, js_undefined);
    return p;
}

void js_run_module(struct js *J, const char *url, int inline_src, const char *src, int len, void *tag)
{
    if (enter(J) < 0) {
        if (J->modhost.done)
            J->modhost.done(J->modhost.ctx, tag, 1);
        return;
    }
    int outer = !entered;
    if (outer) {
        entered = 1;
        if (__builtin_setjmp(jmp_target)) {
            entered = 0;
            free_owner = NULL;
            J->mod_busy = 0;
            if (J->modhost.done)
                J->modhost.done(J->modhost.ctx, tag, 1);
            return;
        }
    }
    struct jsmod *m = NULL;
    if (!inline_src)
        for (m = J->mods; m && (m->inline_src || strcmp(m->url->s, url)); m = m->next)
            ;
    if (!m)
        m = mod_new(J, url, inline_src);
    if (m->state == MS_FETCHING) {      /* new, or imported and on its way: the source is here */
        J->mod_busy++;
        mod_set_source(J, m, src, len);
        J->mod_busy--;
    }
    struct jsentry *en = js_alloc(J, sizeof(*en));
    en->m = m;
    en->tag = tag;
    mod_add_entry(J, en);
    mod_progress(J);
    J->throwing = 0;
    if (outer) {
        js_run_jobs(J);
        entered = 0;
    }
}

void js_module_source(struct js *J, int id, const char *src, int len)
{
    if (enter(J) < 0)
        return;
    int outer = !entered;
    if (outer) {
        entered = 1;
        if (__builtin_setjmp(jmp_target)) {
            entered = 0;
            free_owner = NULL;
            J->mod_busy = 0;
            return;
        }
    }
    struct jsmod *m = J->mods;
    while (m && m->id != id)
        m = m->next;
    if (m && m->state == MS_FETCHING) {
        J->mod_busy++;
        if (src) {
            mod_set_source(J, m, src, len);
        } else {
            char msg[URL_MAX + 64];
            snprintf(msg, sizeof(msg), "Failed to fetch module %s", m->url->s);
            mod_fail(J, m, js_make_error(J, "TypeError", msg));
        }
        J->mod_busy--;
        mod_progress(J);
    }
    J->throwing = 0;
    if (outer) {
        js_run_jobs(J);
        entered = 0;
    }
}

const char *js_error(struct js *J)
{
    return J->err;
}

struct js *js_new(void *host)
{
    struct js *J = calloc(1, sizeof(*J));
    if (!J)
        return NULL;
    J->heap = gc_new();
    if (!J->heap) {
        free(J);
        return NULL;
    }
    J->gc_inhibit = 1;                  /* no collections until the library is in place */
    J->host = host;
    J->step_limit = JS_STEP_LIMIT;
    struct meminfo mi;
    J->heap_limit = JS_HEAP_MIN;
    if (meminfo(&mi) == 0 && mi.free_kb / 2 > (JS_HEAP_MIN >> 10))
        J->heap_limit = mi.free_kb / 2 < (JS_HEAP_MAX >> 10) ? (size_t)(mi.free_kb / 2) << 10 : JS_HEAP_MAX;
    J->s_home = js_mkstr(J, "%home", 5);
    J->s_default = js_mkstr(J, "*default*", 9);
    J->s_meta = js_mkstr(J, "%meta", 5);
    J->object_proto = js_mkobj(J, C_OBJECT, NULL);
    J->function_proto = js_mkobj(J, C_FUNCTION, J->object_proto);
    J->global = js_mkobj(J, C_OBJECT, J->object_proto);
    J->genv = js_alloc(J, sizeof(struct env));
    J->genv->obj = J->global;
    J->genv->has_self = 1;
    J->genv->self = js_obj(J->global);
    J->genv->captured = 1;
    js_init_lib(J);
    js_define_native(J, J->global, "eval", global_eval, 1);
    init_generators(J);
    J->gc_inhibit = 0;
    return J;
}

void js_free(struct js *J)
{
    if (!J)
        return;
    if (free_owner == J) {
        free_owner = NULL;
        free_envs = NULL;
        free_bindings = NULL;
    }
    if (active == J)
        active = NULL;
    co_free_all(J);
    gc_destroy(J);
    free(J);
}
