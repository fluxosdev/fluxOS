/*
 * More of the JavaScript engine's built-in objects: Symbol, BigInt, Promise
 * and its job queue, Reflect,
 * WeakMap and WeakSet, generators and async functions' support, and the
 * newer methods of the older objects.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jsint.h"
#include "jsprelude.h"
#include "jsbinarylib.h"

#define ARG(i) ((i) < argc ? args[(i)] : js_undefined)

static struct jsstr *S(struct js *J, const char *s)
{
    return js_mkstr(J, s, (int)strlen(s));
}

static jsval strv(struct jsstr *s)
{
    jsval v = { JS_STR, { .s = s } };
    return v;
}

static struct jsobj *fn_obj(struct js *J, struct jsobj *on, const char *name, js_native f, int n)
{
    jsval v = js_new_function(J, f, name, n);
    js_put(J, on, S(J, name), v, 1);
    return v.u.o;
}

static struct jsobj *ctor_obj(struct js *J, const char *name, js_native f, int n, struct jsobj *proto)
{
    struct jsobj *c = fn_obj(J, J->global, name, f, n);
    c->ctor = 1;
    if (proto) {
        js_put(J, c, S(J, "prototype"), js_obj(proto), 1);
        js_put(J, proto, S(J, "constructor"), js_obj(c), 1);
    }
    return c;
}

static void put_value(struct js *J, struct jsobj *o, const char *name, jsval v)
{
    js_put(J, o, S(J, name), v, 1);
}

/* ---- Symbol ---- */

static unsigned symbol_count;

static jsval sym_value(struct js *J, const char *tag, struct jsstr *desc)
{
    int tl = (int)strlen(tag), n = 1 + tl + (desc ? 1 + desc->len : 0);
    char *b = js_alloc_atomic(J, (size_t)n + 1);
    b[0] = (char)0xFF;
    memcpy(b + 1, tag, (size_t)tl);
    if (desc) {
        b[1 + tl] = (char)0xFF;
        memcpy(b + 2 + tl, desc->s, (size_t)desc->len);
    }
    jsval v = { JS_SYM, { .s = js_mkstr(J, b, n) } };
    return v;
}

static jsval symbol_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (J->constructing)
        return js_throw_error(J, "TypeError: Symbol is not a constructor");
    struct jsstr *desc = argc && args[0].t != JS_UNDEF ? js_tostr(J, args[0]) : NULL;
    if (J->throwing)
        return js_undefined;
    char tag[16];
    snprintf(tag, sizeof(tag), "u%u", ++symbol_count);
    return sym_value(J, tag, desc);
}

static jsval symbol_for(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *key = ARG(0).t == JS_SYM ? js_sym_text(J, args[0].u.s) : js_tostr(J, ARG(0));
    return sym_value(J, "r", key);
}

static jsval symbol_key_for(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (ARG(0).t != JS_SYM)
        return js_throw_error(J, "TypeError: Symbol.keyFor needs a symbol");
    struct jsstr *k = args[0].u.s;
    if (k->len < 2 || k->s[1] != 'r')
        return js_undefined;
    struct jsstr *d = js_sym_desc(J, k);
    return d ? strv(d) : js_undefined;
}

static struct jsstr *this_symbol(struct js *J, jsval self)
{
    if (self.t == JS_SYM)
        return self.u.s;
    js_throw_error(J, "TypeError: not a Symbol");
    return NULL;
}

static jsval symbol_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *k = this_symbol(J, self);
    return k ? strv(js_sym_text(J, k)) : js_undefined;
}

static jsval symbol_value_of(struct js *J, jsval self, jsval *args, int argc)
{
    return this_symbol(J, self) ? self : js_undefined;
}

static jsval symbol_description(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *k = this_symbol(J, self), *d = k ? js_sym_desc(J, k) : NULL;
    return d ? strv(d) : js_undefined;
}

static void getter(struct js *J, struct jsobj *on, const char *name, js_native f)
{
    struct jsstr *key = S(J, name);
    js_put(J, on, key, js_undefined, 1);
    js_own(on, key)->getter = js_new_function(J, f, name, 0).u.o;
}

static void init_symbol(struct js *J)
{
    J->symbol_proto = js_new_object(J);
    struct jsobj *sym = ctor_obj(J, "Symbol", symbol_ctor, 0, J->symbol_proto);
    fn_obj(J, sym, "for", symbol_for, 1);
    fn_obj(J, sym, "keyFor", symbol_key_for, 1);
    static const char *const well_known[] = {
        "iterator", "asyncIterator", "hasInstance", "toPrimitive", "toStringTag", "species",
        "isConcatSpreadable", "match", "matchAll", "replace", "search", "split", "unscopables",
        "dispose", "asyncDispose",
    };
    for (unsigned i = 0; i < sizeof(well_known) / sizeof(well_known[0]); i++) {
        char d[48];
        snprintf(d, sizeof(d), "Symbol.%s", well_known[i]);
        put_value(J, sym, well_known[i], sym_value(J, "w", S(J, d)));
    }
    fn_obj(J, J->symbol_proto, "toString", symbol_to_string, 0);
    fn_obj(J, J->symbol_proto, "valueOf", symbol_value_of, 0);
    fn_obj(J, J->symbol_proto, SYM_TO_PRIMITIVE, symbol_value_of, 1);
    getter(J, J->symbol_proto, "description", symbol_description);
    put_value(J, J->symbol_proto, SYM_TO_STRING_TAG, js_str(J, "Symbol"));
}

/* ---- BigInt ---- */

/* ToBigInt: booleans, strings and BigInts; anything else is a TypeError (a SyntaxError for bad text). */
static struct jsbig *to_bigint(struct js *J, jsval v)
{
    v = js_to_primitive(J, v, 0);
    if (J->throwing)
        return NULL;
    switch (v.t) {
    case JS_BIG: return v.u.g;
    case JS_BOOL: return jb_from_u64(J, (uint64_t)v.u.b, 0);
    case JS_STR: {
        struct jsbig *g = jb_parse(J, v.u.s->s, v.u.s->len, 0, 1);
        if (!g && !J->throwing)
            js_throw_error(J, "SyntaxError: Cannot convert %s to a BigInt", v.u.s->s);
        return g;
    }
    default:
        js_throw_error(J, "TypeError: Cannot convert %s to a BigInt", js_text(J, v));
        return NULL;
    }
}

static jsval bigint_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (J->constructing)
        return js_throw_error(J, "TypeError: BigInt is not a constructor");
    jsval v = js_to_primitive(J, ARG(0), 0);
    if (J->throwing)
        return js_undefined;
    if (v.t == JS_NUM) {
        if (isnan(v.u.n) || isinf(v.u.n) || v.u.n != trunc(v.u.n))
            return js_throw_error(J, "RangeError: The number %s cannot be converted to a BigInt because it is not an integer",
                                  js_text(J, v));
        return js_bigv(jb_from_double(J, v.u.n));
    }
    struct jsbig *g = to_bigint(J, v);
    return g ? js_bigv(g) : js_undefined;
}

static jsval bigint_as_n(struct js *J, jsval *args, int argc, int is_signed)
{
    double bits = js_to_number(J, ARG(0));
    if (isnan(bits))
        bits = 0;
    bits = trunc(bits);
    if (bits < 0 || bits > 9007199254740991.0)
        return js_throw_error(J, "RangeError: Invalid value: not (convertible to) a safe integer");
    struct jsbig *g = to_bigint(J, ARG(1));
    if (!g)
        return js_undefined;
    struct jsbig *r = jb_as_n(J, bits > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)bits, g, is_signed);
    return r ? js_bigv(r) : js_undefined;
}

static jsval bigint_as_int_n(struct js *J, jsval self, jsval *args, int argc)
{
    return bigint_as_n(J, args, argc, 1);
}

static jsval bigint_as_uint_n(struct js *J, jsval self, jsval *args, int argc)
{
    return bigint_as_n(J, args, argc, 0);
}

static struct jsbig *this_bigint(struct js *J, jsval self)
{
    if (self.t == JS_BIG)
        return self.u.g;
    js_throw_error(J, "TypeError: not a BigInt");
    return NULL;
}

static jsval bigint_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsbig *g = this_bigint(J, self);
    if (!g)
        return js_undefined;
    int radix = 10;
    if (ARG(0).t != JS_UNDEF) {
        double r = js_to_number(J, args[0]);
        if (!(r >= 2 && r <= 36))
            return js_throw_error(J, "RangeError: toString() radix must be between 2 and 36");
        radix = (int)r;
    }
    return strv(jb_tostr(J, g, radix));
}

static jsval bigint_to_locale_string(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsbig *g = this_bigint(J, self);
    if (!g)
        return js_undefined;
    struct jsstr *d = jb_tostr(J, g, 10);       /* grouped by threes, as en-US has it */
    int digits = d->len - g->neg, n = d->len + (digits - 1) / 3;
    char *b = js_alloc_atomic(J, (size_t)n + 1);
    int o = 0;
    for (int i = 0; i < d->len; i++) {
        b[o++] = d->s[i];
        int left = d->len - 1 - i;
        if (left && left % 3 == 0 && d->s[i] != '-')
            b[o++] = ',';
    }
    return strv(js_mkstr(J, b, o));
}

static jsval bigint_value_of(struct js *J, jsval self, jsval *args, int argc)
{
    return this_bigint(J, self) ? self : js_undefined;
}

static void init_bigint(struct js *J)
{
    J->bigint_proto = js_new_object(J);
    struct jsobj *big = ctor_obj(J, "BigInt", bigint_ctor, 1, J->bigint_proto);
    fn_obj(J, big, "asIntN", bigint_as_int_n, 2);
    fn_obj(J, big, "asUintN", bigint_as_uint_n, 2);
    fn_obj(J, J->bigint_proto, "toString", bigint_to_string, 0);
    fn_obj(J, J->bigint_proto, "toLocaleString", bigint_to_locale_string, 0);
    fn_obj(J, J->bigint_proto, "valueOf", bigint_value_of, 0);
    put_value(J, J->bigint_proto, SYM_TO_STRING_TAG, js_str(J, "BigInt"));
}

/* ---- Promise ---- */

struct reaction {
    struct reaction *next;
    jsval ok, err, child;
};

struct promise {
    int state;                          /* 0 pending, 1 fulfilled, 2 rejected */
    jsval value;
    struct reaction *head, *tail;
    int locked;                         /* resolved, perhaps to another promise still pending */
    int handled;                        /* something has asked for its result (then, catch, await...) */
};

static const struct jsclass promise_class = { "Promise", NULL, NULL, NULL };

static struct promise *pdata(jsval v)
{
    return v.t == JS_OBJ ? js_host_data(v.u.o, &promise_class) : NULL;
}

static struct jsobj *new_promise(struct js *J)
{
    struct promise *p = js_alloc(J, sizeof(*p));
    struct jsobj *o = js_new_host(J, &promise_class, p);
    o->proto = J->promise_proto;
    return o;
}

/* A function that calls fn with `self` as this and the extra arguments first. */
static jsval bound_fn(struct js *J, js_native fn, jsval self, const jsval *bargs, int nb)
{
    jsval t = js_new_function(J, fn, "", 1);
    struct jsobj *w = js_mkobj(J, C_FUNCTION, J->function_proto);
    w->bound = t.u.o;
    w->bound_this = self;
    w->name = "";
    if (nb) {
        w->bound_args = js_alloc(J, sizeof(jsval) * (size_t)nb);
        memcpy(w->bound_args, bargs, sizeof(jsval) * (size_t)nb);
        w->nbound = nb;
    }
    return js_obj(w);
}

static void reaction_job(struct js *J, struct job *j)
{
    jsval handler = j->a, child = j->b, v = j->c;
    int state = (int)j->d.u.n;
    if (js_is_callable(handler)) {
        jsval r = js_call_value(J, handler, js_undefined, &v, 1);
        if (J->throwing) {
            jsval exc = J->exc;
            J->throwing = 0;
            if (child.t == JS_OBJ)
                js_promise_settle(J, child, 2, exc);
        } else if (child.t == JS_OBJ) {
            js_promise_resolve(J, child, r);
        }
    } else if (child.t == JS_OBJ) {
        if (state == 1)
            js_promise_resolve(J, child, v);
        else
            js_promise_settle(J, child, 2, v);
    }
}

/* Run after the reactions queued before it: is the rejected promise still without a handler? */
static void unhandled_job(struct js *J, struct job *j)
{
    struct promise *p = pdata(j->a);
    if (p && p->state == 2 && !p->handled && J->on_unhandled) {
        p->handled = 1;
        J->on_unhandled(J, j->a, p->value);
    }
}

void js_promise_settle(struct js *J, jsval po, int state, jsval v)
{
    struct promise *p = pdata(po);
    if (!p || p->state)
        return;
    p->state = state;
    p->value = v;
    if (state == 2 && !p->handled && !p->head && J->on_unhandled)
        js_enqueue(J, unhandled_job, po, js_undefined, js_undefined, js_undefined);
    for (struct reaction *r = p->head; r; r = r->next)
        js_enqueue(J, reaction_job, state == 1 ? r->ok : r->err, r->child, v, js_num(state));
    p->head = p->tail = NULL;
}

static void thenable_job(struct js *J, struct job *j);

/* Resolve: a plain value fulfils; a thenable is followed. */
void js_promise_resolve(struct js *J, jsval po, jsval v)
{
    struct promise *p = pdata(po);
    if (!p || p->state)
        return;
    if (v.t == JS_OBJ) {
        if (v.u.o == po.u.o) {
            js_promise_settle(J, po, 2, js_make_error(J, "TypeError", "a promise cannot resolve to itself"));
            return;
        }
        jsval then = js_get(J, v, "then");
        if (J->throwing) {
            jsval exc = J->exc;
            J->throwing = 0;
            js_promise_settle(J, po, 2, exc);
            return;
        }
        if (js_is_callable(then)) {
            p->locked = 1;
            js_enqueue(J, thenable_job, then, v, po, js_undefined);
            return;
        }
    }
    js_promise_settle(J, po, 1, v);
}

/* The resolving functions of a promise that is following a thenable. */
static jsval adopt_resolve(struct js *J, jsval self, jsval *args, int argc)
{
    js_promise_resolve(J, self, ARG(0));
    return js_undefined;
}

static jsval adopt_reject(struct js *J, jsval self, jsval *args, int argc)
{
    js_promise_settle(J, self, 2, ARG(0));
    return js_undefined;
}

static void thenable_job(struct js *J, struct job *j)
{
    jsval fns[2] = { bound_fn(J, adopt_resolve, j->c, NULL, 0), bound_fn(J, adopt_reject, j->c, NULL, 0) };
    js_call_value(J, j->a, j->b, fns, 2);
    if (J->throwing) {
        jsval exc = J->exc;
        J->throwing = 0;
        js_promise_settle(J, j->c, 2, exc);
    }
}

/* then, from C: ok and err are functions or anything else (passed through). */
static void add_reaction(struct js *J, jsval po, jsval ok, jsval err, jsval child)
{
    struct promise *p = pdata(po);
    p->handled = 1;
    if (p->state) {
        js_enqueue(J, reaction_job, p->state == 1 ? ok : err, child, p->value, js_num(p->state));
        return;
    }
    struct reaction *r = js_alloc(J, sizeof(*r));
    r->ok = ok;
    r->err = err;
    r->child = child;
    if (p->tail)
        p->tail->next = r;
    else
        p->head = r;
    p->tail = r;
}

jsval js_promise_new(struct js *J)
{
    return js_obj(new_promise(J));
}

jsval js_promise_then(struct js *J, jsval po, jsval ok, jsval err)
{
    jsval child = js_obj(new_promise(J));
    add_reaction(J, po, ok, err, child);
    return child;
}

int js_is_promise(jsval v)
{
    return pdata(v) != NULL;
}

/* Promise.resolve(v): v itself if it is one of ours. */
jsval js_promise_from(struct js *J, jsval v)
{
    if (pdata(v))
        return v;
    jsval po = js_obj(new_promise(J));
    js_promise_resolve(J, po, v);
    return po;
}

/* Observe a promise from C: call on_ok or on_err (natives, with `self`) when it settles. */
void js_promise_watch(struct js *J, jsval po, js_native on_ok, js_native on_err, jsval self)
{
    add_reaction(J, po, bound_fn(J, on_ok, self, NULL, 0), bound_fn(J, on_err, self, NULL, 0), js_undefined);
}

int js_promise_state(jsval po, jsval *value)
{
    struct promise *p = pdata(po);
    if (!p)
        return -1;
    if (value)
        *value = p->value;
    return p->state;
}

static jsval promise_resolve_fn(struct js *J, jsval self, jsval *args, int argc)
{
    struct promise *p = pdata(self);
    if (!p || p->locked)
        return js_undefined;
    p->locked = 1;
    js_promise_resolve(J, self, ARG(0));
    return js_undefined;
}

static jsval promise_reject_fn(struct js *J, jsval self, jsval *args, int argc)
{
    struct promise *p = pdata(self);
    if (!p || p->locked)
        return js_undefined;
    p->locked = 1;
    js_promise_settle(J, self, 2, ARG(0));
    return js_undefined;
}

static jsval promise_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (!J->constructing)
        return js_throw_error(J, "TypeError: Promise constructor cannot be invoked without 'new'");
    if (!js_is_callable(ARG(0)))
        return js_throw_error(J, "TypeError: Promise resolver is not a function");
    jsval po = js_obj(new_promise(J));
    jsval fns[2] = { bound_fn(J, promise_resolve_fn, po, NULL, 0), bound_fn(J, promise_reject_fn, po, NULL, 0) };
    js_call_value(J, args[0], js_undefined, fns, 2);
    if (J->throwing) {
        jsval exc = J->exc;
        J->throwing = 0;
        struct promise *p = pdata(po);
        if (!p->locked) {
            p->locked = 1;
            js_promise_settle(J, po, 2, exc);
        }
    }
    return po;
}

static jsval promise_then_m(struct js *J, jsval self, jsval *args, int argc)
{
    if (!pdata(self))
        return js_throw_error(J, "TypeError: then called on a value that is not a promise");
    return js_promise_then(J, self, ARG(0), ARG(1));
}

static jsval promise_catch_m(struct js *J, jsval self, jsval *args, int argc)
{
    if (!pdata(self))
        return js_throw_error(J, "TypeError: catch called on a value that is not a promise");
    return js_promise_then(J, self, js_undefined, ARG(0));
}

static jsval throw_self(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return js_throw_value(J, self);
}

static jsval return_self(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)args;
    (void)argc;
    return self;
}

/* finally's reactions: self is {f}; the original value or reason carries on. */
static jsval finally_step(struct js *J, jsval self, jsval *args, int argc, int rejected)
{
    jsval f = js_get(J, self, "f");
    jsval r = js_call_value(J, f, js_undefined, NULL, 0);
    if (J->throwing)
        return js_undefined;
    jsval keep = bound_fn(J, rejected ? throw_self : return_self, ARG(0), NULL, 0);
    if (pdata(r) || (r.t == JS_OBJ && js_is_callable(js_get(J, r, "then"))))
        return js_promise_then(J, js_promise_from(J, r), keep, js_undefined);
    return rejected ? js_throw_value(J, ARG(0)) : ARG(0);
}

static jsval finally_ok(struct js *J, jsval self, jsval *args, int argc)
{
    return finally_step(J, self, args, argc, 0);
}

static jsval finally_err(struct js *J, jsval self, jsval *args, int argc)
{
    return finally_step(J, self, args, argc, 1);
}

static jsval promise_finally_m(struct js *J, jsval self, jsval *args, int argc)
{
    if (!pdata(self))
        return js_throw_error(J, "TypeError: finally called on a value that is not a promise");
    if (!js_is_callable(ARG(0)))
        return js_promise_then(J, self, ARG(0), ARG(0));
    struct jsobj *st = js_new_object(J);
    js_put(J, st, S(J, "f"), args[0], 1);
    return js_promise_then(J, self, bound_fn(J, finally_ok, js_obj(st), NULL, 0),
                           bound_fn(J, finally_err, js_obj(st), NULL, 0));
}

static jsval promise_resolve_s(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return js_promise_from(J, ARG(0));
}

static jsval promise_reject_s(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    jsval po = js_obj(new_promise(J));
    js_promise_settle(J, po, 2, ARG(0));
    return po;
}

static jsval promise_with_resolvers(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    jsval po = js_obj(new_promise(J));
    struct jsobj *o = js_new_object(J);
    js_put(J, o, S(J, "promise"), po, 0);
    js_put(J, o, S(J, "resolve"), bound_fn(J, promise_resolve_fn, po, NULL, 0), 0);
    js_put(J, o, S(J, "reject"), bound_fn(J, promise_reject_fn, po, NULL, 0), 0);
    return js_obj(o);
}

/* Promise.all and the like: self is {values, remaining, result}; args are [index, value]. */
static void all_done(struct js *J, jsval st, int rejected_all)
{
    jsval remaining = js_get(J, st, "remaining");
    int left = (int)remaining.u.n - 1;
    js_put(J, st.u.o, S(J, "remaining"), js_num(left), 1);
    if (left)
        return;
    jsval values = js_get(J, st, "values"), result = js_get(J, st, "result");
    if (rejected_all) {
        jsval err = js_make_error(J, "Error", "All promises were rejected");
        js_set(J, err, "errors", values);
        js_promise_settle(J, result, 2, err);
    } else {
        js_promise_resolve(J, result, values);
    }
}

static void store(struct js *J, jsval st, jsval idx, jsval v)
{
    jsval values = js_get(J, st, "values");
    values.u.o->items[(int)idx.u.n] = v;
}

static jsval all_ok(struct js *J, jsval self, jsval *args, int argc)
{
    store(J, self, ARG(0), ARG(1));
    all_done(J, self, 0);
    return js_undefined;
}

static jsval settled_ok(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *o = js_new_object(J);
    js_put(J, o, S(J, "status"), js_str(J, "fulfilled"), 0);
    js_put(J, o, S(J, "value"), ARG(1), 0);
    store(J, self, ARG(0), js_obj(o));
    all_done(J, self, 0);
    return js_undefined;
}

static jsval settled_err(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *o = js_new_object(J);
    js_put(J, o, S(J, "status"), js_str(J, "rejected"), 0);
    js_put(J, o, S(J, "reason"), ARG(1), 0);
    store(J, self, ARG(0), js_obj(o));
    all_done(J, self, 0);
    return js_undefined;
}

static jsval any_err(struct js *J, jsval self, jsval *args, int argc)
{
    store(J, self, ARG(0), ARG(1));
    all_done(J, self, 1);
    return js_undefined;
}

static jsval reject_result(struct js *J, jsval self, jsval *args, int argc)
{
    js_promise_settle(J, js_get(J, self, "result"), 2, ARG(0));
    return js_undefined;
}

static jsval resolve_result(struct js *J, jsval self, jsval *args, int argc)
{
    js_promise_resolve(J, js_get(J, self, "result"), ARG(0));
    return js_undefined;
}

enum { COMB_ALL, COMB_SETTLED, COMB_ANY, COMB_RACE };

static jsval combinator(struct js *J, jsval *args, int argc, int kind)
{
    jsval result = js_obj(new_promise(J));
    struct iter it;
    if (js_iter_open(J, ARG(0), &it) < 0) {
        jsval exc = J->exc;
        J->throwing = 0;
        js_promise_settle(J, result, 2, exc);
        return result;
    }
    struct jsobj *items = js_new_array(J), *values = js_new_array(J);
    jsval x;
    int r;
    while ((r = js_iter_next(J, &it, &x)) > 0)
        js_array_push(J, items, x);
    if (r < 0) {
        jsval exc = J->exc;
        J->throwing = 0;
        js_promise_settle(J, result, 2, exc);
        return result;
    }
    struct jsobj *st = js_new_object(J);
    js_put(J, st, S(J, "values"), js_obj(values), 1);
    js_put(J, st, S(J, "remaining"), js_num(items->len), 1);
    js_put(J, st, S(J, "result"), result, 1);
    if (!items->len && kind != COMB_RACE) {
        if (kind == COMB_ANY) {
            jsval err = js_make_error(J, "Error", "All promises were rejected");
            js_set(J, err, "errors", js_obj(values));
            js_promise_settle(J, result, 2, err);
        } else {
            js_promise_resolve(J, result, js_obj(values));
        }
        return result;
    }
    for (int i = 0; i < items->len; i++) {
        js_array_push(J, values, js_undefined);
        jsval p = js_promise_from(J, items->items[i]);
        jsval idx[1] = { js_num(i) };
        jsval ok, err;
        switch (kind) {
        case COMB_ALL:
            ok = bound_fn(J, all_ok, js_obj(st), idx, 1);
            err = bound_fn(J, reject_result, js_obj(st), NULL, 0);
            break;
        case COMB_SETTLED:
            ok = bound_fn(J, settled_ok, js_obj(st), idx, 1);
            err = bound_fn(J, settled_err, js_obj(st), idx, 1);
            break;
        case COMB_ANY:
            ok = bound_fn(J, resolve_result, js_obj(st), NULL, 0);
            err = bound_fn(J, any_err, js_obj(st), idx, 1);
            break;
        default:
            ok = bound_fn(J, resolve_result, js_obj(st), NULL, 0);
            err = bound_fn(J, reject_result, js_obj(st), NULL, 0);
            break;
        }
        add_reaction(J, p, ok, err, js_undefined);
    }
    return result;
}

static jsval promise_all(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return combinator(J, args, argc, COMB_ALL);
}

static jsval promise_all_settled(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return combinator(J, args, argc, COMB_SETTLED);
}

static jsval promise_any(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return combinator(J, args, argc, COMB_ANY);
}

static jsval promise_race(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return combinator(J, args, argc, COMB_RACE);
}

static void call_job(struct js *J, struct job *j)
{
    js_call_value(J, j->a, js_undefined, NULL, 0);
}

static jsval queue_microtask(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (!js_is_callable(ARG(0)))
        return js_throw_error(J, "TypeError: queueMicrotask needs a function");
    js_enqueue(J, call_job, args[0], js_undefined, js_undefined, js_undefined);
    return js_undefined;
}

static jsval proxy_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (!J->constructing)
        return js_throw_error(J, "TypeError: Constructor Proxy requires 'new'");
    if (ARG(0).t != JS_OBJ || ARG(1).t != JS_OBJ)
        return js_throw_error(J, "TypeError: Cannot create proxy with a non-object as target or handler");
    if (js_is_callable(ARG(0)))
        return js_throw_error(J, "TypeError: Proxy of a function is not supported");
    struct jsobj *p = js_mkobj(J, C_OBJECT, ARG(0).u.o->proto);
    p->ptarget = ARG(0).u.o;
    p->phandler = ARG(1).u.o;
    return js_obj(p);
}

static void init_promise(struct js *J)
{
    ctor_obj(J, "Proxy", proxy_ctor, 2, NULL);
    struct jsobj *pp = J->promise_proto = js_mkobj(J, C_OBJECT, J->object_proto);
    struct jsobj *promise = ctor_obj(J, "Promise", promise_ctor, 1, pp);
    fn_obj(J, pp, "then", promise_then_m, 2);
    fn_obj(J, pp, "catch", promise_catch_m, 1);
    fn_obj(J, pp, "finally", promise_finally_m, 1);
    fn_obj(J, promise, "resolve", promise_resolve_s, 1);
    fn_obj(J, promise, "reject", promise_reject_s, 1);
    fn_obj(J, promise, "all", promise_all, 1);
    fn_obj(J, promise, "allSettled", promise_all_settled, 1);
    fn_obj(J, promise, "any", promise_any, 1);
    fn_obj(J, promise, "race", promise_race, 1);
    fn_obj(J, promise, "withResolvers", promise_with_resolvers, 0);
    fn_obj(J, J->global, "queueMicrotask", queue_microtask, 1);
}

/* ---- the library written in JavaScript ---- */

/*
 * jsprelude.h has a core that runs when the engine starts and chunks that
 * run the first time a page uses a name in them (URL, fetch, ...): the
 * name is an accessor on the global object until then. Most pages use none
 * of them, and parsing them all would cost every page time and memory.
 */
static const struct lazy_chunk {
    const char *src;
    int len;
    const char *names[24];
} lazy_chunks[] = {
    {js_chunk_weak,
     sizeof(js_chunk_weak) - 1,
     {"Reflect", "WeakMap", "WeakSet", "WeakRef", "FinalizationRegistry", "AggregateError", "DOMException",
      NULL}},
    {js_chunk_misc, sizeof(js_chunk_misc) - 1, {"structuredClone", "btoa", "atob", "Intl", NULL}},
    {js_chunk_idb,
     sizeof(js_chunk_idb) - 1,
     {"indexedDB", "IDBFactory", "IDBKeyRange", "IDBDatabase", "IDBTransaction", "IDBObjectStore", "IDBIndex",
      "IDBCursor", "IDBCursorWithValue", "IDBRequest", "IDBOpenDBRequest", "IDBVersionChangeEvent", NULL}},
    {js_chunk_events, sizeof(js_chunk_events) - 1, {"EventTarget", "AbortSignal", "AbortController", NULL}},
    {js_chunk_url, sizeof(js_chunk_url) - 1, {"URLSearchParams", "URL", "URLPattern", NULL}},
    {js_chunk_observers,
     sizeof(js_chunk_observers) - 1,
     {"MutationObserver", "IntersectionObserver", "ResizeObserver", NULL}},
    {js_chunk_web,
     sizeof(js_chunk_web) - 1,
     {"TextEncoder", "TextDecoder", "crypto", "CryptoKey", "Blob", "FormData", "DOMParser", "customElements", "CSS",
      "Notification", "DOMRect", "File", "FileList", "FileReader", NULL}},
    {js_chunk_fetch,
     sizeof(js_chunk_fetch) - 1,
     {"Headers", "Response", "Request", "ReadableStream", "WritableStream", "TransformStream",
      "CountQueuingStrategy", "ByteLengthQueuingStrategy", "fetch", "EventSource", "TextDecoderStream",
      "TextEncoderStream", "CompressionStream", "DecompressionStream", NULL}},
};

/* A chunk gives a name its value, unless the page has set it already. */
static jsval lazy_define(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *name = js_tostr(J, ARG(0));
    struct prop *p = js_own(J->global, name);
    if (!p)
        js_put(J, J->global, name, ARG(1), 1);
    else if (p->getter) {
        p->getter = p->setter = NULL;
        p->v = ARG(1);
    }
    return js_undefined;
}

static jsval lazy_get(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    int idx = (int)js_get(J, self, "chunk").u.n;
    struct jsstr *name = js_tostr(J, js_get(J, self, "name"));
    if (!(J->lazy_done & (1u << idx))) {
        J->lazy_done |= 1u << idx;
        if (js_eval(J, lazy_chunks[idx].src, lazy_chunks[idx].len, "library") < 0)
            fprintf(stderr, "javascript library: %s\n", js_error(J));
    }
    struct prop *p = js_own(J->global, name);
    if (!p)
        return js_undefined;
    if (p->getter) {                            /* the chunk did not define it */
        p->getter = p->setter = NULL;
        p->v = js_undefined;
    }
    return p->v;
}

static jsval lazy_set(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *name = js_tostr(J, js_get(J, self, "name"));
    struct prop *p = js_own(J->global, name);
    if (p) {
        p->getter = p->setter = NULL;
        p->v = ARG(0);
    }
    return js_undefined;
}

static void init_prelude(struct js *J)
{
    fn_obj(J, J->global, "%lazy", lazy_define, 2);
    for (unsigned i = 0; i < sizeof(lazy_chunks) / sizeof(lazy_chunks[0]); i++) {
        for (int k = 0; lazy_chunks[i].names[k]; k++) {
            struct jsobj *st = js_new_object(J);
            js_put(J, st, S(J, "chunk"), js_num((double)i), 1);
            js_put(J, st, S(J, "name"), js_str(J, lazy_chunks[i].names[k]), 1);
            struct jsstr *name = S(J, lazy_chunks[i].names[k]);
            js_put(J, J->global, name, js_undefined, 1);
            struct prop *p = js_own(J->global, name);
            p->getter = bound_fn(J, lazy_get, js_obj(st), NULL, 0).u.o;
            p->setter = bound_fn(J, lazy_set, js_obj(st), NULL, 0).u.o;
        }
    }
    /* The core. A failure here is a bug in the prelude, not the page's. */
    if (js_eval(J, js_core, (int)sizeof(js_core) - 1, "prelude") < 0)
        fprintf(stderr, "javascript prelude: %s\n", js_error(J));
}

void js_init_lib2(struct js *J)
{
    init_symbol(J);
    init_bigint(J);
    init_promise(J);
    init_prelude(J);
    if (js_eval(J, js_binary_lib, sizeof(js_binary_lib) - 1, "binary library") < 0)
        fprintf(stderr, "binary library: %s\n", js_error(J));
}
