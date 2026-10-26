/*
 * The JavaScript engine's built-in objects: Object, Function, Array,
 * String, Number, Boolean, Math, JSON, Date, RegExp, Map, Set, the Error
 * types, console, and the global functions.
 *
 * Dates have no time zone: local time is UTC.
 */
#include <flux.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "jsint.h"

#define ARG(i) ((i) < argc ? args[(i)] : js_undefined)

static jsval strv(struct jsstr *s)
{
    jsval v;
    v.t = JS_STR;
    v.u.s = s;
    return v;
}

static struct jsstr *S(struct js *J, const char *s)
{
    return js_mkstr(J, s, (int)strlen(s));
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

static double num_arg(struct js *J, jsval *args, int argc, int i)
{
    return js_to_number(J, ARG(i));
}

/* An integer argument, clamped into [lo, hi]; def if missing. */
static int int_arg(struct js *J, jsval *args, int argc, int i, int def, int lo, int hi)
{
    if (i >= argc || args[i].t == JS_UNDEF)
        return def;
    double d = js_to_number(J, args[i]);
    if (isnan(d))
        d = 0;
    d = trunc(d);
    if (d < lo)
        return lo;
    if (d > hi)
        return hi;
    return (int)d;
}

/* A relative index (negative counts from the end) clamped into [0, len]. */
static int rel_index(struct js *J, jsval *args, int argc, int i, int def, int len)
{
    if (i >= argc || args[i].t == JS_UNDEF)
        return def;
    double d = js_to_number(J, args[i]);
    if (isnan(d))
        d = 0;
    d = trunc(d);
    if (d < 0)
        d += len;
    if (d < 0)
        d = 0;
    if (d > len)
        d = len;
    return (int)d;
}

static struct jsobj *this_array(struct js *J, jsval self)
{
    if (self.t == JS_OBJ && (self.u.o->cls == C_ARRAY || self.u.o->cls == C_ARGUMENTS))
        return self.u.o;
    js_throw_error(J, "TypeError: not an array");
    return NULL;
}

static struct jsstr *this_string(struct js *J, jsval self)
{
    if (self.t == JS_OBJ && self.u.o->cls == C_STRING)
        return self.u.o->str;
    if (self.t == JS_UNDEF || self.t == JS_NULL) {
        js_throw_error(J, "TypeError: String method called on null or undefined");
        return js_mkstr(J, "", 0);
    }
    return js_tostr(J, self);
}

/* ---- Object ---- */

static jsval object_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    jsval v = ARG(0);
    if (v.t == JS_OBJ)
        return v;
    if (v.t == JS_STR || v.t == JS_NUM || v.t == JS_BOOL) {
        struct jsobj *o = js_mkobj(J, v.t == JS_STR ? C_STRING : v.t == JS_NUM ? C_NUMBER : C_BOOLEAN,
                                   v.t == JS_STR ? J->string_proto : v.t == JS_NUM ? J->number_proto : J->boolean_proto);
        if (v.t == JS_STR)
            o->str = v.u.s;
        else
            o->num = v.t == JS_NUM ? v.u.n : v.u.b;
        return js_obj(o);
    }
    return js_obj(js_new_object(J));
}

static jsval object_keys_kind(struct js *J, jsval obj, int kind)
{
    struct jsobj *a = js_new_array(J);
    struct jsstr **keys;
    if (obj.t == JS_UNDEF || obj.t == JS_NULL)
        return js_throw_error(J, "TypeError: cannot convert undefined or null to object");
    int n = js_keys(J, obj, 0, &keys);
    for (int i = 0; i < n; i++) {
        if (kind == 0) {
            js_array_push(J, a, strv(keys[i]));
        } else {
            jsval v = js_get_str(J, obj, keys[i]);
            if (kind == 1) {
                js_array_push(J, a, v);
            } else {
                struct jsobj *pair = js_new_array(J);
                js_array_push(J, pair, strv(keys[i]));
                js_array_push(J, pair, v);
                js_array_push(J, a, js_obj(pair));
            }
        }
    }
    return js_obj(a);
}

static jsval object_keys(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return object_keys_kind(J, ARG(0), 0);
}

static jsval object_values(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return object_keys_kind(J, ARG(0), 1);
}

static jsval object_entries(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return object_keys_kind(J, ARG(0), 2);
}

static jsval object_assign(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    jsval target = ARG(0);
    if (target.t != JS_OBJ)
        return js_throw_error(J, "TypeError: Object.assign target is not an object");
    for (int i = 1; i < argc; i++) {
        struct jsstr **keys;
        int n = js_keys(J, args[i], 0, &keys);
        for (int k = 0; k < n; k++) {
            js_setv(J, target, strv(keys[k]), js_get_str(J, args[i], keys[k]));
            if (J->throwing)
                return js_undefined;
        }
    }
    return target;
}

static jsval object_create(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    jsval p = ARG(0);
    if (p.t != JS_OBJ && p.t != JS_NULL)
        return js_throw_error(J, "TypeError: Object prototype may only be an Object or null");
    struct jsobj *o = js_mkobj(J, C_OBJECT, p.t == JS_OBJ ? p.u.o : NULL);
    if (ARG(1).t == JS_OBJ) {
        struct jsstr **keys;
        int n = js_keys(J, ARG(1), 0, &keys);
        for (int i = 0; i < n; i++) {
            jsval d = js_get_str(J, ARG(1), keys[i]);
            js_put(J, o, keys[i], js_get(J, d, "value"), 0);
        }
    }
    return js_obj(o);
}

static jsval object_identity(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    return ARG(0);
}

static jsval object_false(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_bool(0);
}

static jsval object_get_proto(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    jsval v = ARG(0);
    if (v.t == JS_OBJ)
        return js_obj(v.u.o->proto);
    if (v.t == JS_STR)
        return js_obj(J->string_proto);
    if (v.t == JS_NUM)
        return js_obj(J->number_proto);
    return js_null;
}

static jsval object_set_proto(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    if (ARG(0).t == JS_OBJ && (ARG(1).t == JS_OBJ || ARG(1).t == JS_NULL))
        args[0].u.o->proto = ARG(1).t == JS_OBJ ? args[1].u.o : NULL;
    return ARG(0);
}

static void define_property(struct js *J, jsval obj, struct jsstr *key, jsval desc)
{
    if (obj.t != JS_OBJ || desc.t != JS_OBJ)
        return;
    jsval get = js_get(J, desc, "get"), set = js_get(J, desc, "set");
    jsval enumerable = js_get(J, desc, "enumerable");
    struct jsobj *o = obj.u.o;
    int indexed = js_typed_has(obj, key->s);
    if (indexed >= 0) {
        if (!indexed || js_is_callable(get) || js_is_callable(set) ||
            (js_has(J, desc, S(J, "writable")) && !js_truthy(js_get(J, desc, "writable"))) ||
            (js_has(J, desc, S(J, "enumerable")) && !js_truthy(enumerable)) ||
            (js_has(J, desc, S(J, "configurable")) && !js_truthy(js_get(J, desc, "configurable")))) {
            js_throw_error(J, "TypeError: invalid typed array descriptor");
            return;
        }
        if (js_has(J, desc, S(J, "value")))
            js_setv(J, obj, strv(key), js_get(J, desc, "value"));
        return;
    }
    if (js_is_callable(get) || js_is_callable(set)) {
        struct prop *p = js_own(o, key);
        if (!p) {
            js_put(J, o, key, js_undefined, !js_truthy(enumerable));
            p = js_own(o, key);
        }
        if (js_is_callable(get))
            p->getter = get.u.o;
        if (js_is_callable(set))
            p->setter = set.u.o;
        return;
    }
    jsval v = js_get(J, desc, "value");
    if (o->cls == C_ARRAY || o->cls == C_HOST) {
        js_setv(J, obj, strv(key), v);
        return;
    }
    struct prop *p = js_own(o, key);
    if (p)
        p->getter = p->setter = NULL;           /* a data property now */
    js_put(J, o, key, v, p ? p->hidden : !js_truthy(enumerable));
}

static jsval object_define_property(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (ARG(0).t != JS_OBJ)
        return js_throw_error(J, "TypeError: Object.defineProperty called on non-object");
    define_property(J, args[0], js_tostr(J, ARG(1)), ARG(2));
    return args[0];
}

static jsval object_define_properties(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (ARG(0).t != JS_OBJ)
        return js_throw_error(J, "TypeError: Object.defineProperties called on non-object");
    struct jsstr **keys;
    int n = js_keys(J, ARG(1), 0, &keys);
    for (int i = 0; i < n; i++)
        define_property(J, args[0], keys[i], js_get_str(J, args[1], keys[i]));
    return args[0];
}

static jsval object_own_names(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsobj *a = js_new_array(J);
    jsval o = ARG(0);
    if (o.t != JS_OBJ)
        return js_obj(a);
    if (o.u.o->cls == C_ARRAY)
        for (int i = 0; i < o.u.o->len; i++) {
            char b[16];
            snprintf(b, sizeof(b), "%d", i);
            js_array_push(J, a, js_str(J, b));
        }
    for (struct prop *p = o.u.o->props; p; p = p->next)
        js_array_push(J, a, strv(p->key));
    return js_obj(a);
}

static jsval object_true(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_bool(1);
}

/* A property's descriptor: { value, writable, enumerable, configurable } or { get, set, ... }. */
static jsval descriptor_of(struct js *J, jsval obj, struct jsstr *key)
{
    if (js_typed_has(obj, key->s) == 0)
        return js_undefined;
    if (obj.t != JS_OBJ)
        return js_undefined;
    struct jsobj *o = obj.u.o;
    struct prop *p = js_own(o, key);
    uint32_t i;
    int own = p != NULL;
    if (!own && (o->cls == C_ARRAY || o->cls == C_ARGUMENTS))
        own = (js_index(strv(key), &i) && i < (uint32_t)o->len) || !strcmp(key->s, "length");
    if (!own && o->cls == C_HOST) {
        jsval tmp;
        own = o->hcls->get && o->hcls->get(J, o, key->s, &tmp);
    }
    if (!own)
        return js_undefined;
    struct jsobj *d = js_new_object(J);
    jsval dv = js_obj(d);
    if (p && (p->getter || p->setter)) {
        js_set(J, dv, "get", p->getter ? js_obj(p->getter) : js_undefined);
        js_set(J, dv, "set", p->setter ? js_obj(p->setter) : js_undefined);
    } else {
        js_set(J, dv, "value", js_get_str(J, obj, key));
        js_set(J, dv, "writable", js_bool(1));
    }
    js_set(J, dv, "enumerable", js_bool(!p || !p->hidden));
    js_set(J, dv, "configurable", js_bool(1));
    return dv;
}

static jsval object_get_own_descriptor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return descriptor_of(J, ARG(0), js_tostr(J, ARG(1)));
}

static jsval object_get_own_descriptors(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsobj *r = js_new_object(J);
    jsval o = ARG(0);
    if (o.t != JS_OBJ)
        return js_obj(r);
    for (struct prop *p = o.u.o->props; p; p = p->next)
        js_put(J, r, p->key, descriptor_of(J, o, p->key), 0);
    return js_obj(r);
}

static jsval object_no_symbols(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    return js_obj(js_new_array(J));
}

static jsval object_is(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    jsval a = ARG(0), b = ARG(1);
    if (a.t == JS_NUM && b.t == JS_NUM) {
        if (isnan(a.u.n) && isnan(b.u.n))
            return js_bool(1);
        if (a.u.n == 0 && b.u.n == 0)
            return js_bool(__builtin_signbit(a.u.n) == __builtin_signbit(b.u.n));
    }
    return js_bool(js_strict_equal(a, b));
}

static jsval object_from_entries(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsobj *o = js_new_object(J);
    struct iter it;
    if (js_iter_open(J, ARG(0), &it) < 0)
        return js_undefined;
    jsval pair;
    int r;
    while ((r = js_iter_next(J, &it, &pair)) > 0) {
        js_setv(J, js_obj(o), js_getv(J, pair, js_num(0)), js_getv(J, pair, js_num(1)));
        if (J->throwing)
            return js_undefined;
    }
    return r < 0 ? js_undefined : js_obj(o);
}

static jsval proto_has_own(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *key = js_tostr(J, ARG(0));
    if (self.t == JS_STR) {
        uint32_t i;
        return js_bool(js_index(strv(key), &i) && i < (uint32_t)js_u16len(self.u.s));
    }
    if (self.t != JS_OBJ)
        return js_bool(0);
    struct jsobj *o = self.u.o;
    int indexed = js_typed_has(self, key->s);
    if (indexed >= 0)
        return js_bool(indexed);
    uint32_t i;
    if (o->phandler)
        return js_bool(js_has(J, self, key));
    if ((o->cls == C_ARRAY || o->cls == C_ARGUMENTS) && js_index(strv(key), &i))
        return js_bool(i < (uint32_t)o->len);
    if (o->cls == C_HOST) {
        jsval tmp;
        if (o->hcls->get && o->hcls->get(J, o, key->s, &tmp))
            return js_bool(1);
    }
    return js_bool(js_own(o, key) != NULL);
}

static jsval proto_is_prototype_of(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    jsval v = ARG(0);
    if (self.t != JS_OBJ || v.t != JS_OBJ)
        return js_bool(0);
    for (struct jsobj *o = v.u.o->proto; o; o = o->proto)
        if (o == self.u.o)
            return js_bool(1);
    return js_bool(0);
}

static const char *class_name(jsval v)
{
    switch (v.t) {
    case JS_UNDEF: return "Undefined";
    case JS_NULL: return "Null";
    case JS_BOOL: return "Boolean";
    case JS_NUM: return "Number";
    case JS_STR: return "String";
    default:
        switch (v.u.o->cls) {
        case C_ARRAY: return "Array";
        case C_FUNCTION: return "Function";
        case C_ERROR: return "Error";
        case C_DATE: return "Date";
        case C_REGEXP: return "RegExp";
        case C_ARGUMENTS: return "Arguments";
        case C_HOST: return v.u.o->hcls->name;
        default: return "Object";
        }
    }
}

static jsval proto_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    char b[64];
    snprintf(b, sizeof(b), "[object %s]", class_name(self));
    return js_str(J, b);
}

static jsval proto_value_of(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)args;
    (void)argc;
    return self;
}

/* ---- Function ---- */

static jsval function_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    /* new Function("a", "b", "return a + b"): compiled as an expression in the global scope */
    struct jsstr *src = S(J, "__fluxfn=(function anonymous(");
    for (int i = 0; i < argc - 1; i++) {
        if (i)
            src = js_concat(J, src, S(J, ","));
        src = js_concat(J, src, js_tostr(J, args[i]));
    }
    src = js_concat(J, src, S(J, "\n) {\n"));
    if (argc)
        src = js_concat(J, src, js_tostr(J, args[argc - 1]));
    src = js_concat(J, src, S(J, "\n});"));
    if (J->throwing)
        return js_undefined;
    if (js_eval(J, src->s, src->len, "new Function") < 0)
        return js_throw_error(J, "SyntaxError: %s", J->err);
    jsval f = js_get(J, js_obj(J->global), "__fluxfn");
    js_delete(J, js_obj(J->global), S(J, "__fluxfn"));
    return f;
}

static jsval function_call(struct js *J, jsval self, jsval *args, int argc)
{
    return js_call_value(J, self, ARG(0), argc > 1 ? args + 1 : NULL, argc > 1 ? argc - 1 : 0);
}

static jsval function_apply(struct js *J, jsval self, jsval *args, int argc)
{
    jsval list = ARG(1);
    if (list.t == JS_UNDEF || list.t == JS_NULL)
        return js_call_value(J, self, ARG(0), NULL, 0);
    if (list.t == JS_OBJ && (list.u.o->cls == C_ARRAY || list.u.o->cls == C_ARGUMENTS))
        return js_call_value(J, self, ARG(0), list.u.o->items, list.u.o->len);
    jsval len = js_get(J, list, "length");
    int n = len.t == JS_NUM ? (int)len.u.n : 0;
    if (n < 0 || n > 65536)
        return js_throw_error(J, "RangeError: too many arguments");
    jsval *a = js_alloc(J, sizeof(jsval) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++)
        a[i] = js_getv(J, list, js_num(i));
    return js_call_value(J, self, ARG(0), a, n);
}

static jsval function_bind(struct js *J, jsval self, jsval *args, int argc)
{
    if (!js_is_callable(self))
        return js_throw_error(J, "TypeError: bind called on a non-function");
    struct jsobj *f = js_mkobj(J, C_FUNCTION, J->function_proto);
    f->bound = self.u.o;
    f->bound_this = ARG(0);
    f->nbound = argc > 1 ? argc - 1 : 0;
    if (f->nbound) {
        f->bound_args = js_alloc(J, sizeof(jsval) * (size_t)f->nbound);
        memcpy(f->bound_args, args + 1, sizeof(jsval) * (size_t)f->nbound);
    }
    f->name = self.u.o->name;
    f->nargs = self.u.o->nargs > f->nbound ? self.u.o->nargs - f->nbound : 0;
    f->ctor = self.u.o->ctor || self.u.o->fn;
    return js_obj(f);
}

static jsval function_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    char b[160];
    const char *name = js_is_callable(self) && self.u.o->name ? self.u.o->name : "";
    snprintf(b, sizeof(b), "function %s() { [native code] }", name);
    return js_str(J, b);
}

/* ---- Array ---- */

static jsval array_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsobj *a = js_new_array(J);
    if (argc == 1 && args[0].t == JS_NUM) {
        double d = args[0].u.n;
        if (d < 0 || d != floor(d) || js_array_set_len(J, a, (int)d) < 0)
            return js_throw_error(J, "RangeError: invalid array length");
        return js_obj(a);
    }
    for (int i = 0; i < argc; i++)
        js_array_push(J, a, args[i]);
    return js_obj(a);
}

static jsval array_is_array(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    return js_bool(js_is_array(ARG(0)));
}

static jsval array_from(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsobj *a = js_new_array(J);
    jsval src = ARG(0), map = ARG(1);
    if (src.t == JS_STR || src.t == JS_OBJ) {       /* anything iterable, or with a length */
        struct iter it;
        jsval len = src.t == JS_OBJ ? js_get(J, src, "length") : js_undefined;
        jsval iterf = src.t == JS_OBJ ? js_get(J, src, "@@iterator") : js_undefined;
        if (src.t == JS_OBJ && len.t != JS_NUM && iterf.t == JS_UNDEF)
            return js_obj(a);                       /* a plain object: no elements */
        if (js_iter_open(J, src, &it) < 0)
            return js_undefined;
        jsval x;
        int r;
        while ((r = js_iter_next(J, &it, &x)) > 0)
            js_array_push(J, a, x);
        if (r < 0)
            return js_undefined;
    }
    if (js_is_callable(map))
        for (int i = 0; i < a->len; i++) {
            jsval cb[2] = { a->items[i], js_num(i) };
            a->items[i] = js_call_value(J, map, js_undefined, cb, 2);
            if (J->throwing)
                return js_undefined;
        }
    return js_obj(a);
}

static jsval array_of(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsobj *a = js_new_array(J);
    for (int i = 0; i < argc; i++)
        js_array_push(J, a, args[i]);
    return js_obj(a);
}

static jsval array_push(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    for (int i = 0; i < argc; i++)
        js_array_push(J, a, args[i]);
    return js_num(a->len);
}

static jsval array_pop(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct jsobj *a = this_array(J, self);
    if (!a || !a->len)
        return js_undefined;
    return a->items[--a->len];
}

static jsval array_shift(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct jsobj *a = this_array(J, self);
    if (!a || !a->len)
        return js_undefined;
    jsval v = a->items[0];
    memmove(a->items, a->items + 1, sizeof(jsval) * (size_t)(a->len - 1));
    a->len--;
    return v;
}

static jsval array_unshift(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    int old = a->len;
    js_array_set_len(J, a, old + argc);
    memmove(a->items + argc, a->items, sizeof(jsval) * (size_t)old);
    for (int i = 0; i < argc; i++)
        a->items[i] = args[i];
    return js_num(a->len);
}

static jsval array_slice(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    int from = rel_index(J, args, argc, 0, 0, a->len), to = rel_index(J, args, argc, 1, a->len, a->len);
    struct jsobj *r = js_new_array(J);
    for (int i = from; i < to; i++)
        js_array_push(J, r, a->items[i]);
    return js_obj(r);
}

static jsval array_splice(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    int start = rel_index(J, args, argc, 0, 0, a->len);
    int del = argc < 2 ? a->len - start : int_arg(J, args, argc, 1, 0, 0, a->len - start);
    int add = argc > 2 ? argc - 2 : 0;
    struct jsobj *removed = js_new_array(J);
    for (int i = 0; i < del; i++)
        js_array_push(J, removed, a->items[start + i]);
    int old = a->len, nl = old - del + add;
    if (nl > old)
        js_array_set_len(J, a, nl);
    memmove(a->items + start + add, a->items + start + del, sizeof(jsval) * (size_t)(old - start - del));
    for (int i = 0; i < add; i++)
        a->items[start + i] = args[2 + i];
    a->len = nl;
    return js_obj(removed);
}

static jsval array_concat(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *r = js_new_array(J);
    for (int i = -1; i < argc; i++) {
        jsval v = i < 0 ? self : args[i];
        if (js_is_array(v))
            for (int k = 0; k < v.u.o->len; k++)
                js_array_push(J, r, v.u.o->items[k]);
        else
            js_array_push(J, r, v);
    }
    return js_obj(r);
}

jsval js_array_join(struct js *J, struct jsobj *a, const char *sep)
{
    static int depth;
    if (depth > 32)
        return js_str(J, "");
    depth++;
    int seplen = (int)strlen(sep), total = 0;
    struct jsstr **parts = js_alloc(J, sizeof(*parts) * (size_t)(a->len ? a->len : 1));
    for (int i = 0; i < a->len; i++) {
        jsval v = a->items[i];
        parts[i] = v.t == JS_UNDEF || v.t == JS_NULL ? js_mkstr(J, "", 0) : js_tostr(J, v);
        if (J->throwing) {
            depth--;
            return js_undefined;
        }
        total += parts[i]->len + (i ? seplen : 0);
    }
    struct jsstr *r = js_alloc_atomic(J, sizeof(struct jsstr) + (size_t)total + 1);
    int o = 0;
    for (int i = 0; i < a->len; i++) {
        if (i) {
            memcpy(r->s + o, sep, (size_t)seplen);
            o += seplen;
        }
        memcpy(r->s + o, parts[i]->s, (size_t)parts[i]->len);
        o += parts[i]->len;
    }
    r->len = o;
    r->s[o] = 0;
    depth--;
    return strv(r);
}

static jsval array_join(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    struct jsstr *sep = argc && args[0].t != JS_UNDEF ? js_tostr(J, args[0]) : S(J, ",");
    return js_array_join(J, a, sep->s);
}

static jsval array_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    if (!js_is_array(self))
        return proto_to_string(J, self, NULL, 0);
    return js_array_join(J, self.u.o, ",");
}

static jsval array_reverse(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    for (int i = 0, k = a->len - 1; i < k; i++, k--) {
        jsval t = a->items[i];
        a->items[i] = a->items[k];
        a->items[k] = t;
    }
    return self;
}

static int same_value_zero(jsval a, jsval b)
{
    if (a.t == JS_NUM && b.t == JS_NUM && isnan(a.u.n) && isnan(b.u.n))
        return 1;
    return js_strict_equal(a, b);
}

static jsval array_index_of(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    int from = rel_index(J, args, argc, 1, 0, a->len);
    for (int i = from; i < a->len; i++)
        if (js_strict_equal(a->items[i], ARG(0)))
            return js_num(i);
    return js_num(-1);
}

static jsval array_last_index_of(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    for (int i = a->len - 1; i >= 0; i--)
        if (js_strict_equal(a->items[i], ARG(0)))
            return js_num(i);
    return js_num(-1);
}

static jsval array_includes(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    for (int i = 0; i < a->len; i++)
        if (same_value_zero(a->items[i], ARG(0)))
            return js_bool(1);
    return js_bool(0);
}

/* forEach, map, filter, some, every, find, findIndex: kind picks which. */
enum { IT_EACH, IT_MAP, IT_FILTER, IT_SOME, IT_EVERY, IT_FIND, IT_FINDINDEX };

static jsval iterate(struct js *J, jsval self, jsval *args, int argc, int kind)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    jsval fn = ARG(0), that = ARG(1);
    if (!js_is_callable(fn))
        return js_throw_error(J, "TypeError: callback is not a function");
    struct jsobj *out = kind == IT_MAP || kind == IT_FILTER ? js_new_array(J) : NULL;
    for (int i = 0; i < a->len; i++) {
        jsval v = a->items[i];
        jsval cb[3] = { v, js_num(i), self };
        jsval r = js_call_value(J, fn, that, cb, 3);
        if (J->throwing)
            return js_undefined;
        switch (kind) {
        case IT_MAP: js_array_push(J, out, r); break;
        case IT_FILTER: if (js_truthy(r)) js_array_push(J, out, v); break;
        case IT_SOME: if (js_truthy(r)) return js_bool(1); break;
        case IT_EVERY: if (!js_truthy(r)) return js_bool(0); break;
        case IT_FIND: if (js_truthy(r)) return v; break;
        case IT_FINDINDEX: if (js_truthy(r)) return js_num(i); break;
        }
    }
    switch (kind) {
    case IT_MAP:
    case IT_FILTER: return js_obj(out);
    case IT_SOME: return js_bool(0);
    case IT_EVERY: return js_bool(1);
    case IT_FINDINDEX: return js_num(-1);
    default: return js_undefined;
    }
}

static jsval array_for_each(struct js *J, jsval s, jsval *a, int n) { return iterate(J, s, a, n, IT_EACH); }
static jsval array_map(struct js *J, jsval s, jsval *a, int n) { return iterate(J, s, a, n, IT_MAP); }
static jsval array_filter(struct js *J, jsval s, jsval *a, int n) { return iterate(J, s, a, n, IT_FILTER); }
static jsval array_some(struct js *J, jsval s, jsval *a, int n) { return iterate(J, s, a, n, IT_SOME); }
static jsval array_every(struct js *J, jsval s, jsval *a, int n) { return iterate(J, s, a, n, IT_EVERY); }
static jsval array_find(struct js *J, jsval s, jsval *a, int n) { return iterate(J, s, a, n, IT_FIND); }
static jsval array_find_index(struct js *J, jsval s, jsval *a, int n) { return iterate(J, s, a, n, IT_FINDINDEX); }

static jsval reduce(struct js *J, jsval self, jsval *args, int argc, int right)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    jsval fn = ARG(0);
    if (!js_is_callable(fn))
        return js_throw_error(J, "TypeError: callback is not a function");
    int i = right ? a->len - 1 : 0, step = right ? -1 : 1;
    jsval acc;
    if (argc > 1) {
        acc = args[1];
    } else {
        if (!a->len)
            return js_throw_error(J, "TypeError: reduce of empty array with no initial value");
        acc = a->items[i];
        i += step;
    }
    for (; i >= 0 && i < a->len; i += step) {
        jsval cb[4] = { acc, a->items[i], js_num(i), self };
        acc = js_call_value(J, fn, js_undefined, cb, 4);
        if (J->throwing)
            return js_undefined;
    }
    return acc;
}

static jsval array_reduce(struct js *J, jsval s, jsval *a, int n) { return reduce(J, s, a, n, 0); }
static jsval array_reduce_right(struct js *J, jsval s, jsval *a, int n) { return reduce(J, s, a, n, 1); }

static jsval array_fill(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    int from = rel_index(J, args, argc, 1, 0, a->len), to = rel_index(J, args, argc, 2, a->len, a->len);
    for (int i = from; i < to; i++)
        a->items[i] = ARG(0);
    return self;
}

static int sort_compare(struct js *J, jsval cmp, jsval a, jsval b)
{
    if (a.t == JS_UNDEF)
        return b.t == JS_UNDEF ? 0 : 1;
    if (b.t == JS_UNDEF)
        return -1;
    if (js_is_callable(cmp)) {
        jsval args[2] = { a, b };
        double r = js_to_number(J, js_call_value(J, cmp, js_undefined, args, 2));
        return r < 0 ? -1 : r > 0;
    }
    struct jsstr *x = js_tostr(J, a), *y = js_tostr(J, b);
    int c = memcmp(x->s, y->s, (size_t)(x->len < y->len ? x->len : y->len));
    return c ? c : x->len - y->len;
}

static jsval array_sort(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    jsval cmp = ARG(0);
    int n = a->len;
    if (n < 2)
        return self;
    /* a stable merge sort */
    jsval *tmp = js_alloc(J, sizeof(jsval) * (size_t)n);
    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            int i = lo, k = mid, o = lo;
            while (i < mid && k < hi) {
                int c = sort_compare(J, cmp, a->items[i], a->items[k]);
                if (J->throwing)
                    return js_undefined;
                tmp[o++] = c <= 0 ? a->items[i++] : a->items[k++];
            }
            while (i < mid)
                tmp[o++] = a->items[i++];
            while (k < hi)
                tmp[o++] = a->items[k++];
        }
        memcpy(a->items, tmp, sizeof(jsval) * (size_t)n);
    }
    return self;
}

static jsval array_flat(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsobj *a = this_array(J, self);
    if (!a)
        return js_undefined;
    int depth = int_arg(J, args, argc, 0, 1, 0, 64);
    struct jsobj *r = js_new_array(J);
    for (int i = 0; i < a->len; i++) {
        jsval v = a->items[i];
        if (js_is_array(v) && depth > 0) {
            jsval d = js_num(depth - 1);
            jsval inner = array_flat(J, v, &d, 1);
            for (int k = 0; k < inner.u.o->len; k++)
                js_array_push(J, r, inner.u.o->items[k]);
        } else {
            js_array_push(J, r, v);
        }
    }
    return js_obj(r);
}

static jsval make_seqiter(struct js *J, jsval keys, jsval vals, int kind, int index_keys);

enum { ITK_VALUES, ITK_KEYS, ITK_ENTRIES };

static jsval array_iter(struct js *J, jsval self, int kind)
{
    if (self.t != JS_OBJ)
        return js_throw_error(J, "TypeError: not an array");
    return make_seqiter(J, self, js_undefined, kind, 1);
}

static jsval array_keys(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return array_iter(J, self, ITK_KEYS);
}

static jsval array_values(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return array_iter(J, self, ITK_VALUES);
}

static jsval array_entries(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return array_iter(J, self, ITK_ENTRIES);
}

/* ---- String ---- */

static unsigned to_uint16(double d)
{
    if (isnan(d) || isinf(d))
        return 0;
    d = fmod(trunc(d), 65536);
    if (d < 0)
        d += 65536;
    return (unsigned)d;
}

static jsval string_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *s = argc ? js_tostr(J, args[0]) : js_mkstr(J, "", 0);
    if (J->constructing) {
        struct jsobj *o = js_mkobj(J, C_STRING, J->string_proto);
        o->str = s;
        return js_obj(o);
    }
    return strv(s);
}

static jsval string_from_char_code(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    char *buf = js_alloc(J, (size_t)argc * 4 + 1);
    int n = 0;
    for (int i = 0; i < argc; i++) {
        int cp = (int)(to_uint16(js_to_number(J, args[i])));
        /* a surrogate pair */
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < argc) {
            int lo = (int)to_uint16(js_to_number(J, args[i + 1]));
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        n += utf8_put(buf + n, cp);
    }
    return js_strn(J, buf, n);
}

static jsval string_char_at(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    int len = js_u16len(s);
    int i = int_arg(J, args, argc, 0, 0, -1, len);
    return strv(js_u16slice(J, s, i < 0 ? len : i, i < 0 ? len : i + 1));
}
static jsval string_char_code_at(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    int i = int_arg(J, args, argc, 0, 0, -1, js_u16len(s));
    int cp = js_u16unit(s, i);
    return js_num(cp < 0 ? NAN : cp);
}
static jsval string_code_point_at(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    int i = int_arg(J, args, argc, 0, 0, -1, js_u16len(s));
    int hi = js_u16unit(s, i);
    if (hi < 0)
        return js_undefined;
    int lo = js_u16unit(s, i + 1);
    return js_num(hi >= 0xD800 && hi <= 0xDBFF && lo >= 0xDC00 && lo <= 0xDFFF
                      ? 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00)
                      : hi);
}
static int u16match(const struct jsstr *s, const struct jsstr *sub, int at)
{
    int n = js_u16len(sub);
    if (at < 0 || at + n > js_u16len(s))
        return 0;
    for (int i = 0; i < n; i++)
        if (js_u16unit(s, at + i) != js_u16unit(sub, i))
            return 0;
    return 1;
}
/* Byte search for the byte-oriented regexp/replacement paths. */
static int str_find(const struct jsstr *s, const struct jsstr *sub, int from)
{
    if (from < 0)
        from = 0;
    for (int i = from; i + sub->len <= s->len; i++)
        if (!memcmp(s->s + i, sub->s, (size_t)sub->len))
            return i;
    return -1;
}
static jsval string_index_of(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self), *sub = js_tostr(J, ARG(0));
    int len = js_u16len(s);
    for (int i = int_arg(J, args, argc, 1, 0, 0, len); i <= len; i++)
        if (u16match(s, sub, i))
            return js_num(i);
    return js_num(-1);
}
static jsval string_last_index_of(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self), *sub = js_tostr(J, ARG(0));
    int len = js_u16len(s);
    for (int i = int_arg(J, args, argc, 1, len, 0, len); i >= 0; i--)
        if (u16match(s, sub, i))
            return js_num(i);
    return js_num(-1);
}
static jsval string_includes(struct js *J, jsval self, jsval *args, int argc)
{
    jsval r = string_index_of(J, self, args, argc);
    return js_bool(r.u.n >= 0);
}
static jsval string_starts_with(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self), *sub = js_tostr(J, ARG(0));
    return js_bool(u16match(s, sub, int_arg(J, args, argc, 1, 0, 0, js_u16len(s))));
}
static jsval string_ends_with(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self), *sub = js_tostr(J, ARG(0));
    int len = js_u16len(s);
    return js_bool(u16match(s, sub, int_arg(J, args, argc, 1, len, 0, len) - js_u16len(sub)));
}
static jsval string_slice(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    int len = js_u16len(s);
    return strv(
        js_u16slice(J, s, rel_index(J, args, argc, 0, 0, len), rel_index(J, args, argc, 1, len, len)));
}
static jsval string_substring(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    int len = js_u16len(s);
    int a = int_arg(J, args, argc, 0, 0, 0, len), b = int_arg(J, args, argc, 1, len, 0, len);
    return strv(js_u16slice(J, s, a < b ? a : b, a < b ? b : a));
}
static jsval string_substr(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    int len = js_u16len(s), a = rel_index(J, args, argc, 0, 0, len);
    return strv(js_u16slice(J, s, a, a + int_arg(J, args, argc, 1, len - a, 0, len - a)));
}

static jsval change_case(struct js *J, jsval self, int upper)
{
    struct jsstr *s = this_string(J, self);
    struct jsstr *r = js_mkstr(J, s->s, s->len);
    for (int i = 0; i < r->len; i++) {
        char c = r->s[i];
        if (upper && c >= 'a' && c <= 'z')
            r->s[i] = (char)(c - 32);
        else if (!upper && c >= 'A' && c <= 'Z')
            r->s[i] = (char)(c + 32);
    }
    return strv(r);
}

static jsval string_upper(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return change_case(J, self, 1);
}

static jsval string_lower(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return change_case(J, self, 0);
}

static int js_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static jsval trim(struct js *J, jsval self, int start, int end)
{
    struct jsstr *s = this_string(J, self);
    int a = 0, b = s->len;
    while (start && a < b && js_space((unsigned char)s->s[a]))
        a++;
    while (end && b > a && js_space((unsigned char)s->s[b - 1]))
        b--;
    return js_strn(J, s->s + a, b - a);
}

static jsval string_trim(struct js *J, jsval s, jsval *a, int n) { (void)a; (void)n; return trim(J, s, 1, 1); }
static jsval string_trim_start(struct js *J, jsval s, jsval *a, int n) { (void)a; (void)n; return trim(J, s, 1, 0); }
static jsval string_trim_end(struct js *J, jsval s, jsval *a, int n) { (void)a; (void)n; return trim(J, s, 0, 1); }

static jsval string_pad(struct js *J, jsval self, jsval *args, int argc, int start)
{
    struct jsstr *s = this_string(J, self);
    int len = js_u16len(s);
    int target = int_arg(J, args, argc, 0, 0, 0, 8 << 20);
    struct jsstr *fill = argc > 1 && args[1].t != JS_UNDEF ? js_tostr(J, args[1]) : S(J, " ");
    int flen = js_u16len(fill);
    if (target <= len || !flen)
        return strv(s);
    int padlen = target - len;
    if ((size_t)((padlen + flen - 1) / flen) * fill->len > (8u << 20))
        return js_throw_error(J, "RangeError: invalid string length");
    char *buf = js_alloc_atomic(J, (size_t)((padlen + flen - 1) / flen) * fill->len + 1);
    int n = 0;
    for (int units = 0; units < padlen; units += flen) {
        memcpy(buf + n, fill->s, (size_t)fill->len);
        n += fill->len;
    }
    struct jsstr *padding = js_u16slice(J, js_mkstr(J, buf, n), 0, padlen);
    return strv(start ? js_concat(J, padding, s) : js_concat(J, s, padding));
}

static jsval string_pad_start(struct js *J, jsval s, jsval *a, int n) { return string_pad(J, s, a, n, 1); }
static jsval string_pad_end(struct js *J, jsval s, jsval *a, int n) { return string_pad(J, s, a, n, 0); }

static jsval string_repeat(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    int count = int_arg(J, args, argc, 0, 0, 0, 1 << 24);
    if ((long long)count * s->len > (8 << 20))
        return js_throw_error(J, "RangeError: invalid string length");
    char *buf = js_alloc(J, (size_t)(count * s->len) + 1);
    for (int i = 0; i < count; i++)
        memcpy(buf + i * s->len, s->s, (size_t)s->len);
    return js_strn(J, buf, count * s->len);
}

static jsval string_concat(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    for (int i = 0; i < argc; i++)
        s = js_concat(J, s, js_tostr(J, args[i]));
    return strv(s);
}

static jsval string_locale_compare(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *a = this_string(J, self), *b = js_tostr(J, ARG(0));
    int c = strcasecmp(a->s, b->s);
    if (!c)
        c = strcmp(a->s, b->s);
    return js_num(c < 0 ? -1 : c > 0);
}

static jsval string_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return strv(this_string(J, self));
}

static struct regex *regex_of(jsval v)
{
    return v.t == JS_OBJ && v.u.o->cls == C_REGEXP ? v.u.o->re : NULL;
}

static jsval string_split(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    struct jsobj *r = js_new_array(J);
    int limit = int_arg(J, args, argc, 1, 1 << 30, 0, 1 << 30);
    jsval sep = ARG(0);
    if (sep.t == JS_UNDEF) {
        js_array_push(J, r, strv(s));
        return js_obj(r);
    }
    struct regex *re = regex_of(sep);
    if (re) {
        int caps[2 * RE_MAX_GROUPS], at = 0, last = 0;
        if (!s->len) {
            if (re_exec(re, s->s, 0, 0, caps) < 0)
                js_array_push(J, r, strv(s));
            return js_obj(r);
        }
        while (at < s->len && r->len < limit) {
            if (re_exec(re, s->s, s->len, at, caps) < 0)
                break;
            int ms = caps[0], me = caps[1];
            if (me == ms && ms >= s->len)
                break;
            if (me == last && ms == me) {       /* an empty match where the last ended */
                at = ms + 1;
                continue;
            }
            if (me == ms && ms == 0) {
                at = 1;
                continue;
            }
            js_array_push(J, r, js_strn(J, s->s + last, ms - last));
            for (int g = 1; g < re_ngroups(re) && r->len < limit; g++)
                js_array_push(J, r, caps[2 * g] < 0 ? js_undefined :
                              js_strn(J, s->s + caps[2 * g], caps[2 * g + 1] - caps[2 * g]));
            last = me;
            at = me > ms ? me : me + 1;
        }
        if (r->len < limit)
            js_array_push(J, r, js_strn(J, s->s + last, s->len - last));
        return js_obj(r);
    }
    struct jsstr *sp = js_tostr(J, sep);
    if (!sp->len) {
        for (int i = 0, len = js_u16len(s); i < len && r->len < limit; i++)
            js_array_push(J, r, strv(js_u16slice(J, s, i, i + 1)));
        return js_obj(r);
    }
    int from = 0;
    while (r->len < limit) {
        int at = str_find(s, sp, from);
        if (at < 0) {
            js_array_push(J, r, js_strn(J, s->s + from, s->len - from));
            break;
        }
        js_array_push(J, r, js_strn(J, s->s + from, at - from));
        from = at + sp->len;
    }
    return js_obj(r);
}

/* A match as an array: the whole match, the groups, index and input. */
static jsval match_array(struct js *J, struct jsstr *s, struct regex *re, int *caps)
{
    struct jsobj *m = js_new_array(J);
    for (int g = 0; g < re_ngroups(re); g++)
        js_array_push(J, m, caps[2 * g] < 0 ? js_undefined :
                      js_strn(J, s->s + caps[2 * g], caps[2 * g + 1] - caps[2 * g]));
    js_put(J, m, S(J, "index"), js_num(caps[0]), 0);
    js_put(J, m, S(J, "input"), strv(s), 0);
    jsval groups = js_undefined;
    if (re_has_names(re)) {
        struct jsobj *go = js_new_object(J);
        for (int g = 1; g < re_ngroups(re); g++)
            if (re_group_name(re, g))
                js_put(J, go, S(J, re_group_name(re, g)), m->items[g], 0);
        groups = js_obj(go);
    }
    js_put(J, m, S(J, "groups"), groups, 0);
    return js_obj(m);
}

static jsval regexp_exec(struct js *J, jsval self, jsval *args, int argc)
{
    struct regex *re = regex_of(self);
    if (!re)
        return js_throw_error(J, "TypeError: exec called on a non-RegExp");
    struct jsstr *s = js_tostr(J, ARG(0));
    int start = 0, global = re_global(re) || strchr(re_flags(re), 'y');
    if (global) {
        double li = js_to_number(J, js_get(J, self, "lastIndex"));
        start = isnan(li) || li < 0 ? 0 : (int)li;
        if (start > s->len) {
            js_set(J, self, "lastIndex", js_num(0));
            return js_null;
        }
    }
    int caps[2 * RE_MAX_GROUPS];
    if (re_exec(re, s->s, s->len, start, caps) < 0) {
        if (global)
            js_set(J, self, "lastIndex", js_num(0));
        return js_null;
    }
    if (global)
        js_set(J, self, "lastIndex", js_num(caps[1] > caps[0] ? caps[1] : caps[1] + 1));
    return match_array(J, s, re, caps);
}

static jsval regexp_test(struct js *J, jsval self, jsval *args, int argc)
{
    jsval m = regexp_exec(J, self, args, argc);
    return js_bool(m.t == JS_OBJ);
}

static jsval regexp_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct regex *re = regex_of(self);
    if (!re)
        return js_str(J, "/(?:)/");
    struct jsstr *r = js_concat(J, S(J, "/"), S(J, re_source(re)));
    r = js_concat(J, r, S(J, "/"));
    return strv(js_concat(J, r, S(J, re_flags(re))));
}

static jsval regexp_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *pattern, *flags;
    struct regex *src = regex_of(ARG(0));
    if (src) {
        pattern = S(J, re_source(src));
        flags = argc > 1 && args[1].t != JS_UNDEF ? js_tostr(J, args[1]) : S(J, re_flags(src));
    } else {
        pattern = ARG(0).t == JS_UNDEF ? S(J, "(?:)") : js_tostr(J, ARG(0));
        flags = argc > 1 && args[1].t != JS_UNDEF ? js_tostr(J, args[1]) : S(J, "");
    }
    const char *err = NULL;
    struct regex *re = re_compile(J, pattern->s, pattern->len, flags->s, &err);
    if (!re)
        return js_throw_error(J, "SyntaxError: invalid regular expression: %s", err ? err : "?");
    struct jsobj *o = js_mkobj(J, C_REGEXP, J->regexp_proto);
    o->re = re;
    js_put(J, o, S(J, "lastIndex"), js_num(0), 1);
    return js_obj(o);
}

static jsval string_match(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    jsval rv = ARG(0);
    if (!regex_of(rv)) {
        jsval pa[1] = { rv.t == JS_UNDEF ? js_str(J, "") : rv };
        rv = regexp_ctor(J, js_undefined, pa, 1);
        if (J->throwing)
            return js_undefined;
    }
    struct regex *re = regex_of(rv);
    if (!re_global(re)) {
        jsval a[1] = { strv(s) };
        js_set(J, rv, "lastIndex", js_num(0));
        return regexp_exec(J, rv, a, 1);
    }
    struct jsobj *all = js_new_array(J);
    int caps[2 * RE_MAX_GROUPS], at = 0;
    while (at <= s->len && re_exec(re, s->s, s->len, at, caps) >= 0) {
        js_array_push(J, all, js_strn(J, s->s + caps[0], caps[1] - caps[0]));
        at = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
    }
    js_set(J, rv, "lastIndex", js_num(0));
    return all->len ? js_obj(all) : js_null;
}

static jsval string_match_all(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    struct regex *re = regex_of(ARG(0));
    if (!re)
        return js_throw_error(J, "TypeError: matchAll needs a RegExp");
    struct jsobj *all = js_new_array(J);
    int caps[2 * RE_MAX_GROUPS], at = 0;
    while (at <= s->len && re_exec(re, s->s, s->len, at, caps) >= 0) {
        js_array_push(J, all, match_array(J, s, re, caps));
        at = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
    }
    return js_obj(all);
}

static jsval string_search(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    jsval rv = ARG(0);
    if (!regex_of(rv)) {
        rv = regexp_ctor(J, js_undefined, args, argc ? 1 : 0);
        if (J->throwing)
            return js_undefined;
    }
    int caps[2 * RE_MAX_GROUPS];
    return js_num(re_exec(regex_of(rv), s->s, s->len, 0, caps) < 0 ? -1 : caps[0]);
}

/* The replacement for one match: a function's result, or the template with $1, $& and so on. */
static struct jsstr *replacement(struct js *J, struct jsstr *s, jsval with, int *caps, int ngroups,
                                 struct regex *re)
{
    if (js_is_callable(with)) {
        jsval a[RE_MAX_GROUPS + 3];
        int n = 0;
        for (int g = 0; g < ngroups; g++)
            a[n++] = caps[2 * g] < 0 ? js_undefined :
                     js_strn(J, s->s + caps[2 * g], caps[2 * g + 1] - caps[2 * g]);
        a[n++] = js_num(caps[0]);
        a[n++] = strv(s);
        if (re && re_has_names(re)) {            /* the named groups, as the last argument */
            struct jsobj *go = js_new_object(J);
            for (int g = 1; g < ngroups; g++)
                if (re_group_name(re, g))
                    js_put(J, go, S(J, re_group_name(re, g)), a[g], 0);
            a[n++] = js_obj(go);
        }
        return js_tostr(J, js_call_value(J, with, js_undefined, a, n));
    }
    struct jsstr *t = js_tostr(J, with);
    char *out = js_alloc(J, (size_t)t->len * 2 + (size_t)s->len + 64);
    int cap = t->len * 2 + s->len + 64, n = 0;
    for (int i = 0; i < t->len; i++) {
        const char *piece = NULL;
        int plen = 0;
        if (t->s[i] == '$' && i + 1 < t->len) {
            char c = t->s[i + 1];
            if (c == '$') {
                piece = "$";
                plen = 1;
                i++;
            } else if (c == '&') {
                piece = s->s + caps[0];
                plen = caps[1] - caps[0];
                i++;
            } else if (c == '`') {
                piece = s->s;
                plen = caps[0];
                i++;
            } else if (c == '\'') {
                piece = s->s + caps[1];
                plen = s->len - caps[1];
                i++;
            } else if (c == '<' && re && re_has_names(re)) {      /* $<name> */
                const char *close = memchr(t->s + i + 2, '>', (size_t)(t->len - i - 2));
                if (close) {
                    int nl = (int)(close - (t->s + i + 2));
                    piece = "";
                    for (int g = 1; g < ngroups; g++) {
                        const char *gn = re_group_name(re, g);
                        if (gn && (int)strlen(gn) == nl && !memcmp(gn, t->s + i + 2, (size_t)nl) && caps[2 * g] >= 0) {
                            piece = s->s + caps[2 * g];
                            plen = caps[2 * g + 1] - caps[2 * g];
                        }
                    }
                    i += nl + 2;
                }
            } else if (c >= '1' && c <= '9' && c - '0' < ngroups) {
                int g = c - '0';
                if (i + 2 < t->len && t->s[i + 2] >= '0' && t->s[i + 2] <= '9' &&
                    g * 10 + t->s[i + 2] - '0' < ngroups) {
                    g = g * 10 + t->s[i + 2] - '0';
                    i++;
                }
                if (caps[2 * g] >= 0) {
                    piece = s->s + caps[2 * g];
                    plen = caps[2 * g + 1] - caps[2 * g];
                } else {
                    piece = "";
                }
                i++;
            }
        }
        if (!piece) {
            piece = t->s + i;
            plen = 1;
        }
        if (n + plen >= cap) {
            int nc = (n + plen) * 2 + 16;
            char *no = js_alloc(J, (size_t)nc);
            memcpy(no, out, (size_t)n);
            out = no;
            cap = nc;
        }
        memcpy(out + n, piece, (size_t)plen);
        n += plen;
    }
    return js_mkstr(J, out, n);
}

static jsval string_replace_impl(struct js *J, jsval self, jsval *args, int argc, int all)
{
    struct jsstr *s = this_string(J, self);
    jsval pat = ARG(0), with = ARG(1);
    struct regex *re = regex_of(pat);
    struct jsstr *out = js_mkstr(J, "", 0);
    int last = 0;
    if (re) {
        int caps[2 * RE_MAX_GROUPS], at = 0, global = re_global(re) || all;
        while (at <= s->len && re_exec(re, s->s, s->len, at, caps) >= 0) {
            out = js_concat(J, out, js_mkstr(J, s->s + last, caps[0] - last));
            out = js_concat(J, out, replacement(J, s, with, caps, re_ngroups(re), re));
            if (J->throwing)
                return js_undefined;
            last = caps[1];
            if (!global)
                break;
            at = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
            if (caps[1] == caps[0] && caps[0] < s->len) {
                out = js_concat(J, out, js_mkstr(J, s->s + caps[0], 1));
                last = caps[0] + 1;
            }
        }
        if (last <= s->len)
            out = js_concat(J, out, js_mkstr(J, s->s + last, s->len - last));
        return strv(out);
    }
    struct jsstr *sub = js_tostr(J, pat);
    int from = 0;
    for (;;) {
        int at = str_find(s, sub, from);
        if (at < 0)
            break;
        int caps[2] = { at, at + sub->len };
        out = js_concat(J, out, js_mkstr(J, s->s + last, at - last));
        out = js_concat(J, out, replacement(J, s, with, caps, 1, NULL));
        if (J->throwing)
            return js_undefined;
        last = at + sub->len;
        if (!all)
            break;
        from = sub->len ? last : last + 1;
        if (from > s->len)
            break;
    }
    out = js_concat(J, out, js_mkstr(J, s->s + last, s->len - last));
    return strv(out);
}

static jsval string_replace(struct js *J, jsval s, jsval *a, int n) { return string_replace_impl(J, s, a, n, 0); }
static jsval string_replace_all(struct js *J, jsval s, jsval *a, int n) { return string_replace_impl(J, s, a, n, 1); }

static jsval string_at(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *s = this_string(J, self);
    int len = js_u16len(s);
    int i = int_arg(J, args, argc, 0, 0, -len - 1, len);
    if (i < 0)
        i += len;
    return i >= 0 && i < len ? strv(js_u16slice(J, s, i, i + 1)) : js_undefined;
}

/* ---- Number and Boolean ---- */

static jsval number_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    double d = argc ? js_to_number(J, args[0]) : 0;
    if (J->constructing) {
        struct jsobj *o = js_mkobj(J, C_NUMBER, J->number_proto);
        o->num = d;
        return js_obj(o);
    }
    return js_num(d);
}

static double this_number(struct js *J, jsval self)
{
    if (self.t == JS_NUM)
        return self.u.n;
    if (self.t == JS_OBJ && self.u.o->cls == C_NUMBER)
        return self.u.o->num;
    js_throw_error(J, "TypeError: not a number");
    return NAN;
}

/* An integer below 1e21 as decimal digits (long double, so no 64-bit division). */
static int big_digits(long double v, char *out)
{
    char tmp[64];
    int n = 0;
    if (v < 1) {
        out[0] = '0';
        out[1] = 0;
        return 1;
    }
    while (v >= 1 && n < 60) {
        long double q = (long double)(long long)(v / 10);
        if (v >= 9e18L)
            q = v / 10 - (long double)fmod((double)(v / 10), 1.0);
        int d = (int)(v - q * 10 + 0.5L);
        if (d > 9)
            d = 9;
        if (d < 0)
            d = 0;
        tmp[n++] = (char)('0' + d);
        v = q;
    }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static jsval number_to_fixed(struct js *J, jsval self, jsval *args, int argc)
{
    double x = this_number(J, self);
    int d = int_arg(J, args, argc, 0, 0, 0, 100);
    if (J->throwing)
        return js_undefined;
    char buf[160];
    if (isnan(x) || isinf(x) || fabs(x) >= 1e21) {
        js_numtostr(x, buf);
        return js_str(J, buf);
    }
    int neg = x < 0;
    long double scaled = (long double)fabs(x);
    for (int i = 0; i < d; i++)
        scaled *= 10;
    scaled = (long double)floor((double)(scaled + 0.5L));
    char digits[128];
    int n = big_digits(scaled, digits);
    if (n <= d) {                       /* leading zeros: 0.0x */
        memmove(digits + (d - n + 1), digits, (size_t)n + 1);
        memset(digits, '0', (size_t)(d - n + 1));
        n = d + 1;
    }
    int o = 0;
    if (neg && strspn(digits, "0") != (size_t)n)
        buf[o++] = '-';
    memcpy(buf + o, digits, (size_t)(n - d));
    o += n - d;
    if (d) {
        buf[o++] = '.';
        memcpy(buf + o, digits + n - d, (size_t)d);
        o += d;
    }
    buf[o] = 0;
    return js_str(J, buf);
}

static jsval number_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    double x = this_number(J, self);
    int radix = int_arg(J, args, argc, 0, 10, 2, 36);
    if (J->throwing)
        return js_undefined;
    char buf[128];
    if (radix == 10 || isnan(x) || isinf(x)) {
        js_numtostr(x, buf);
        return js_str(J, buf);
    }
    int neg = x < 0;
    double ip = floor(fabs(x)), fp = fabs(x) - ip;
    char tmp[128];
    int n = 0;
    do {
        double q = floor(ip / radix);
        int dg = (int)(ip - q * radix);
        tmp[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[dg];
        ip = q;
    } while (ip > 0 && n < 100);
    int o = 0;
    if (neg)
        buf[o++] = '-';
    while (n)
        buf[o++] = tmp[--n];
    if (fp > 0) {
        buf[o++] = '.';
        for (int k = 0; k < 20 && fp > 0; k++) {
            fp *= radix;
            int dg = (int)fp;
            buf[o++] = "0123456789abcdefghijklmnopqrstuvwxyz"[dg];
            fp -= dg;
        }
    }
    buf[o] = 0;
    return js_str(J, buf);
}

static jsval number_to_precision(struct js *J, jsval self, jsval *args, int argc)
{
    double x = this_number(J, self);
    if (argc == 0 || args[0].t == JS_UNDEF || isnan(x) || isinf(x) || x == 0)
        return number_to_string(J, self, NULL, 0);
    int p = int_arg(J, args, argc, 0, 1, 1, 21);
    int e = (int)floor(log10(fabs(x)));
    int decimals = p - 1 - e;
    if (decimals >= 0 && decimals <= 20) {
        jsval d = js_num(decimals);
        return number_to_fixed(J, self, &d, 1);
    }
    return number_to_string(J, self, NULL, 0);
}

static jsval number_value_of(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return js_num(this_number(J, self));
}

static jsval number_to_locale(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    double x = this_number(J, self);
    char raw[64], out[96];
    if (x != floor(x) || fabs(x) >= 1e21) {
        jsval d = js_num(fabs(x) < 1000 ? 3 : 2);
        jsval s = number_to_fixed(J, self, &d, 1);
        snprintf(raw, sizeof(raw), "%s", js_cstr(s));
        int n = (int)strlen(raw);
        if (strchr(raw, '.')) {
            while (n && raw[n - 1] == '0')
                raw[--n] = 0;
            if (n && raw[n - 1] == '.')
                raw[--n] = 0;
        }
    } else {
        js_numtostr(x, raw);
    }
    /* group the integer part in thousands */
    const char *p = raw;
    int o = 0;
    if (*p == '-')
        out[o++] = *p++;
    int ilen = (int)strcspn(p, ".");
    for (int i = 0; i < ilen; i++) {
        if (i && (ilen - i) % 3 == 0)
            out[o++] = ',';
        out[o++] = p[i];
    }
    snprintf(out + o, sizeof(out) - (size_t)o, "%s", p + ilen);
    return js_str(J, out);
}

static jsval boolean_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    int b = js_truthy(ARG(0));
    if (J->constructing) {
        struct jsobj *o = js_mkobj(J, C_BOOLEAN, J->boolean_proto);
        o->num = b;
        return js_obj(o);
    }
    return js_bool(b);
}

static jsval boolean_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    int b = self.t == JS_BOOL ? self.u.b : self.t == JS_OBJ && self.u.o->num != 0;
    return js_str(J, b ? "true" : "false");
}

static jsval boolean_value_of(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)args;
    (void)argc;
    return js_bool(self.t == JS_BOOL ? self.u.b : self.t == JS_OBJ && self.u.o->num != 0);
}

static jsval number_is_integer(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    jsval v = ARG(0);
    return js_bool(v.t == JS_NUM && !isinf(v.u.n) && v.u.n == floor(v.u.n));
}

static jsval number_is_nan(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    return js_bool(ARG(0).t == JS_NUM && isnan(args[0].u.n));
}

static jsval number_is_finite(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    return js_bool(ARG(0).t == JS_NUM && !isnan(args[0].u.n) && !isinf(args[0].u.n));
}

/* ---- globals ---- */

static jsval global_parse_int(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *s = js_tostr(J, ARG(0));
    int radix = int_arg(J, args, argc, 1, 0, 0, 36);
    const char *p = s->s, *end = p + s->len;
    while (p < end && js_space((unsigned char)*p))
        p++;
    int neg = 0;
    if (p < end && (*p == '+' || *p == '-'))
        neg = *p++ == '-';
    if ((radix == 0 || radix == 16) && end - p >= 2 && p[0] == '0' && (p[1] | 32) == 'x') {
        p += 2;
        radix = 16;
    }
    if (radix == 0)
        radix = 10;
    if (radix < 2)
        return js_num(NAN);
    double v = 0;
    int any = 0;
    for (; p < end; p++) {
        int c = *p, d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'z' ? (c | 32) - 'a' + 10 : 99;
        if (d >= radix)
            break;
        v = v * radix + d;
        any = 1;
    }
    return js_num(!any ? NAN : neg ? -v : v);
}

static jsval global_parse_float(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *s = js_tostr(J, ARG(0));
    return js_num(js_parse_float_prefix(s->s, s->len));
}

static jsval global_is_nan(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return js_bool(isnan(num_arg(J, args, argc, 0)));
}

static jsval global_is_finite(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    double d = num_arg(J, args, argc, 0);
    return js_bool(!isnan(d) && !isinf(d));
}

static jsval uri_encode(struct js *J, jsval *args, int argc, const char *keep)
{
    struct jsstr *s = js_tostr(J, ARG(0));
    char *out = js_alloc(J, (size_t)s->len * 3 + 1);
    int n = 0;
    for (int i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            (c && strchr(keep, c)))
            out[n++] = (char)c;
        else
            n += sprintf(out + n, "%%%02X", c);
    }
    return js_strn(J, out, n);
}

static jsval global_encode_uri_component(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return uri_encode(J, args, argc, "-_.!~*'()");
}

static jsval global_encode_uri(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return uri_encode(J, args, argc, "-_.!~*'();/?:@&=+$,#");
}

static jsval global_escape(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return uri_encode(J, args, argc, "@*_+-./");
}

static int hexv(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

static jsval global_decode_uri(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *s = js_tostr(J, ARG(0));
    char *out = js_alloc(J, (size_t)s->len + 1);
    int n = 0;
    for (int i = 0; i < s->len; i++) {
        if (s->s[i] == '%' && i + 2 < s->len && hexv(s->s[i + 1]) >= 0 && hexv(s->s[i + 2]) >= 0) {
            out[n++] = (char)(hexv(s->s[i + 1]) * 16 + hexv(s->s[i + 2]));
            i += 2;
        } else {
            out[n++] = s->s[i];
        }
    }
    return js_strn(J, out, n);
}

/* ---- Math ---- */

static uint32_t rng_state;

static double random_double(void)
{
    if (!rng_state)
        rng_state = uptime_ms() * 2654435761u + (uint32_t)time(NULL);
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    uint32_t y = x * 1664525u + 1013904223u;
    return ((double)(x >> 5) * 67108864.0 + (double)(y >> 6)) / 9007199254740992.0;
}

#define MATH1(name, expr) \
    static jsval math_##name(struct js *J, jsval self, jsval *args, int argc) \
    { (void)self; double x = num_arg(J, args, argc, 0); return js_num(expr); }

MATH1(abs, fabs(x))
MATH1(floor, floor(x))
MATH1(ceil, ceil(x))
MATH1(sqrt, sqrt(x))
MATH1(sin, sin(x))
MATH1(cos, cos(x))
MATH1(tan, tan(x))
MATH1(atan, atan(x))
MATH1(asin, asin(x))
MATH1(acos, acos(x))
MATH1(exp, exp(x))
MATH1(log, log(x))
MATH1(log2, log2(x))
MATH1(log10, log10(x))
MATH1(trunc, trunc(x))
MATH1(sign, isnan(x) ? x : x > 0 ? 1 : x < 0 ? -1 : x)
MATH1(cbrt, x < 0 ? -pow(-x, 1.0 / 3) : pow(x, 1.0 / 3))
MATH1(round, floor(x + 0.5))

static jsval math_atan2(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return js_num(atan2(num_arg(J, args, argc, 0), num_arg(J, args, argc, 1)));
}

static jsval math_pow(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return js_num(pow(num_arg(J, args, argc, 0), num_arg(J, args, argc, 1)));
}

static jsval math_hypot(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    double s = 0;
    for (int i = 0; i < argc; i++) {
        double x = js_to_number(J, args[i]);
        s += x * x;
    }
    return js_num(sqrt(s));
}

static jsval math_minmax(struct js *J, jsval *args, int argc, int max)
{
    double r = max ? -INFINITY : INFINITY;
    for (int i = 0; i < argc; i++) {
        double x = js_to_number(J, args[i]);
        if (isnan(x))
            return js_num(NAN);
        if (max ? x > r : x < r)
            r = x;
    }
    return js_num(r);
}

static jsval math_max(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return math_minmax(J, args, argc, 1);
}

static jsval math_min(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return math_minmax(J, args, argc, 0);
}

static jsval math_random(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_num(random_double());
}

/* ---- JSON ---- */

struct sbuf {
    struct js *J;
    char *p;
    int n, cap;
};

static void sb_add(struct sbuf *b, const char *s, int n)
{
    if (b->n + n + 1 > b->cap) {
        int nc = (b->n + n + 1) * 2;
        char *np = js_alloc(b->J, (size_t)nc);
        if (b->n)
            memcpy(np, b->p, (size_t)b->n);
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += n;
}

static void sb_str(struct sbuf *b, const char *s)
{
    sb_add(b, s, (int)strlen(s));
}

static void json_quote(struct sbuf *b, const struct jsstr *s)
{
    sb_add(b, "\"", 1);
    for (int i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        char esc[8];
        switch (c) {
        case '"': sb_add(b, "\\\"", 2); break;
        case '\\': sb_add(b, "\\\\", 2); break;
        case '\n': sb_add(b, "\\n", 2); break;
        case '\r': sb_add(b, "\\r", 2); break;
        case '\t': sb_add(b, "\\t", 2); break;
        case '\b': sb_add(b, "\\b", 2); break;
        case '\f': sb_add(b, "\\f", 2); break;
        default:
            if (c < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                sb_add(b, esc, 6);
            } else {
                sb_add(b, (const char *)&s->s[i], 1);
            }
        }
    }
    sb_add(b, "\"", 1);
}

static void newline_indent(struct sbuf *b, const char *indent, int depth)
{
    if (!indent || !*indent)
        return;
    sb_add(b, "\n", 1);
    for (int i = 0; i < depth; i++)
        sb_str(b, indent);
}

static int json_value(struct sbuf *b, jsval v, const char *indent, int depth)
{
    struct js *J = b->J;
    if (depth > 64) {
        js_throw_error(J, "TypeError: converting circular structure to JSON");
        return -1;
    }
    if (v.t == JS_OBJ) {
        jsval tojson = js_get(J, v, "toJSON");
        if (js_is_callable(tojson))
            v = js_call_value(J, tojson, v, NULL, 0);
        if (J->throwing)
            return -1;
    }
    switch (v.t) {
    case JS_NULL:
        sb_str(b, "null");
        return 0;
    case JS_BOOL:
        sb_str(b, v.u.b ? "true" : "false");
        return 0;
    case JS_NUM: {
        char n[64];
        if (isnan(v.u.n) || isinf(v.u.n))
            strcpy(n, "null");
        else
            js_numtostr(v.u.n, n);
        sb_str(b, n);
        return 0;
    }
    case JS_STR:
        json_quote(b, v.u.s);
        return 0;
    case JS_OBJ:
        break;
    default:
        return 1;                       /* undefined: left out */
    }
    struct jsobj *o = v.u.o;
    if (o->cls == C_FUNCTION)
        return 1;
    if (o->cls == C_STRING || o->cls == C_NUMBER || o->cls == C_BOOLEAN)
        return json_value(b, js_to_primitive(J, v, 0), indent, depth);
    if (o->cls == C_ARRAY) {
        sb_add(b, "[", 1);
        for (int i = 0; i < o->len; i++) {
            if (i)
                sb_add(b, ",", 1);
            newline_indent(b, indent, depth + 1);
            int r = json_value(b, o->items[i], indent, depth + 1);
            if (r < 0)
                return -1;
            if (r > 0)
                sb_str(b, "null");
        }
        if (o->len)
            newline_indent(b, indent, depth);
        sb_add(b, "]", 1);
        return 0;
    }
    sb_add(b, "{", 1);
    struct jsstr **keys;
    int nk = js_keys(J, v, 0, &keys), first = 1;
    for (int i = 0; i < nk; i++) {
        jsval pv = js_get_str(J, v, keys[i]);
        if (J->throwing)
            return -1;
        if (pv.t == JS_UNDEF || js_is_callable(pv))
            continue;
        if (!first)
            sb_add(b, ",", 1);
        first = 0;
        newline_indent(b, indent, depth + 1);
        json_quote(b, keys[i]);
        sb_add(b, indent && *indent ? ": " : ":", indent && *indent ? 2 : 1);
        if (json_value(b, pv, indent, depth + 1) < 0)
            return -1;
    }
    if (!first)
        newline_indent(b, indent, depth);
    sb_add(b, "}", 1);
    return 0;
}

struct jsstr *js_json_stringify(struct js *J, jsval v, const char *indent)
{
    struct sbuf b = { J, NULL, 0, 0 };
    int r = json_value(&b, v, indent, 0);
    if (r != 0)
        return NULL;
    return js_mkstr(J, b.p ? b.p : "", b.n);
}

static jsval json_stringify(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    char indent[16] = "";
    jsval sp = ARG(2);
    if (sp.t == JS_NUM) {
        int n = (int)sp.u.n;
        for (int i = 0; i < n && i < 10; i++)
            indent[i] = ' ';
    } else if (sp.t == JS_STR) {
        snprintf(indent, sizeof(indent), "%.10s", sp.u.s->s);
    }
    struct jsstr *s = js_json_stringify(J, ARG(0), indent);
    return s ? strv(s) : js_undefined;
}

struct jparse {
    struct js *J;
    const char *s, *end;
    const char *error;
};

static void jskip(struct jparse *p)
{
    while (p->s < p->end && js_space((unsigned char)*p->s))
        p->s++;
}

static jsval jvalue(struct jparse *p, int depth);

static jsval jstring(struct jparse *p)
{
    const char *start = ++p->s;
    char *out = NULL;
    int n = 0, cap = 0;
    while (p->s < p->end && *p->s != '"') {
        if (n + 8 > cap) {
            int nc = cap ? cap * 2 : 64;
            char *no = js_alloc(p->J, (size_t)nc);
            if (n)
                memcpy(no, out, (size_t)n);
            out = no;
            cap = nc;
        }
        char c = *p->s++;
        if (c != '\\') {
            out[n++] = c;
            continue;
        }
        if (p->s >= p->end)
            break;
        c = *p->s++;
        switch (c) {
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'r': out[n++] = '\r'; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case 'u': {
            int cp = 0;
            for (int k = 0; k < 4 && p->s < p->end; k++)
                cp = cp * 16 + (hexv(*p->s) >= 0 ? hexv(*p->s) : 0), p->s++;
            if (cp >= 0xD800 && cp < 0xDC00 && p->end - p->s >= 6 && p->s[0] == '\\' && p->s[1] == 'u') {
                int lo = 0;
                for (int k = 2; k < 6; k++)
                    lo = lo * 16 + (hexv(p->s[k]) >= 0 ? hexv(p->s[k]) : 0);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p->s += 6;
                }
            }
            n += utf8_put(out + n, cp);
            break;
        }
        default: out[n++] = c; break;
        }
    }
    (void)start;
    if (p->s >= p->end) {
        p->error = "unterminated string";
        return js_undefined;
    }
    p->s++;
    return js_strn(p->J, out ? out : "", n);
}

static jsval jvalue(struct jparse *p, int depth)
{
    jskip(p);
    if (p->s >= p->end || depth > 200) {
        p->error = "unexpected end of JSON";
        return js_undefined;
    }
    char c = *p->s;
    if (c == '{') {
        struct jsobj *o = js_new_object(p->J);
        p->s++;
        jskip(p);
        if (p->s < p->end && *p->s == '}') {
            p->s++;
            return js_obj(o);
        }
        for (;;) {
            jskip(p);
            if (p->s >= p->end || *p->s != '"') {
                p->error = "expected a property name";
                return js_undefined;
            }
            jsval k = jstring(p);
            if (p->error)
                return js_undefined;
            jskip(p);
            if (p->s >= p->end || *p->s++ != ':') {
                p->error = "expected ':'";
                return js_undefined;
            }
            jsval v = jvalue(p, depth + 1);
            if (p->error)
                return js_undefined;
            js_put(p->J, o, k.u.s, v, 0);
            jskip(p);
            if (p->s < p->end && *p->s == ',') {
                p->s++;
                continue;
            }
            if (p->s < p->end && *p->s == '}') {
                p->s++;
                return js_obj(o);
            }
            p->error = "expected ',' or '}'";
            return js_undefined;
        }
    }
    if (c == '[') {
        struct jsobj *a = js_new_array(p->J);
        p->s++;
        jskip(p);
        if (p->s < p->end && *p->s == ']') {
            p->s++;
            return js_obj(a);
        }
        for (;;) {
            jsval v = jvalue(p, depth + 1);
            if (p->error)
                return js_undefined;
            js_array_push(p->J, a, v);
            jskip(p);
            if (p->s < p->end && *p->s == ',') {
                p->s++;
                continue;
            }
            if (p->s < p->end && *p->s == ']') {
                p->s++;
                return js_obj(a);
            }
            p->error = "expected ',' or ']'";
            return js_undefined;
        }
    }
    if (c == '"')
        return jstring(p);
    if (p->end - p->s >= 4 && !memcmp(p->s, "true", 4)) {
        p->s += 4;
        return js_bool(1);
    }
    if (p->end - p->s >= 5 && !memcmp(p->s, "false", 5)) {
        p->s += 5;
        return js_bool(0);
    }
    if (p->end - p->s >= 4 && !memcmp(p->s, "null", 4)) {
        p->s += 4;
        return js_null;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *st = p->s;
        if (*p->s == '-')
            p->s++;
        while (p->s < p->end && ((*p->s >= '0' && *p->s <= '9') || *p->s == '.' || *p->s == 'e' ||
                                 *p->s == 'E' || *p->s == '+' || *p->s == '-'))
            p->s++;
        return js_num(js_strtonum(st, (int)(p->s - st)));
    }
    p->error = "unexpected character";
    return js_undefined;
}

static jsval json_parse(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *s = js_tostr(J, ARG(0));
    struct jparse p = { J, s->s, s->s + s->len, NULL };
    jsval v = jvalue(&p, 0);
    jskip(&p);
    if (!p.error && p.s != p.end)
        p.error = "unexpected text after JSON";
    if (p.error)
        return js_throw_error(J, "SyntaxError: JSON.parse: %s", p.error);
    return v;
}

/* ---- Date ---- */

static double date_now_ms(void)
{
    static double base;
    static unsigned int base_up;
    if (!base) {
        base = (double)time(NULL) * 1000;
        base_up = uptime_ms();
    }
    return base + (double)(uptime_ms() - base_up);
}

/* Days since 1970-01-01 from a civil date, and back (Howard Hinnant's algorithms). */
static double days_from_civil(double y, double m, double d)
{
    y -= m <= 2;
    double era = floor(y / 400);
    double yoe = y - era * 400;
    double mp = fmod(m + 9, 12);
    double doy = floor((153 * mp + 2) / 5) + d - 1;
    double doe = yoe * 365 + floor(yoe / 4) - floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}

struct civil {
    int year, month, day, hour, min, sec, ms, wday;
};

static void civil_from_ms(double t, struct civil *c)
{
    double days = floor(t / 86400000);
    double rem = t - days * 86400000;
    c->hour = (int)(rem / 3600000);
    c->min = (int)(fmod(rem, 3600000) / 60000);
    c->sec = (int)(fmod(rem, 60000) / 1000);
    c->ms = (int)fmod(rem, 1000);
    c->wday = (int)fmod(fmod(days + 4, 7) + 7, 7);
    double z = days + 719468;
    double era = floor(z / 146097);
    double doe = z - era * 146097;
    double yoe = floor((doe - floor(doe / 1460) + floor(doe / 36524) - floor(doe / 146096)) / 365);
    double y = yoe + era * 400;
    double doy = doe - (365 * yoe + floor(yoe / 4) - floor(yoe / 100));
    double mp = floor((5 * doy + 2) / 153);
    c->day = (int)(doy - floor((153 * mp + 2) / 5) + 1);
    c->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    c->year = (int)(y + (c->month <= 2));
}

static double make_time(double y, double mon, double d, double h, double mi, double s, double ms)
{
    if (isnan(y) || isnan(mon) || isnan(d) || isnan(h) || isnan(mi) || isnan(s) || isnan(ms))
        return NAN;
    y += floor(mon / 12);
    mon = fmod(mon, 12);
    if (mon < 0)
        mon += 12;
    return days_from_civil(y, mon + 1, 1) * 86400000 + (d - 1) * 86400000 + h * 3600000 + mi * 60000 +
           s * 1000 + ms;
}

/* Read up to max digits at *p as a number; -1 if there are none. */
static int digits(const char **p, int max)
{
    int v = 0, n = 0;
    while (n < max && **p >= '0' && **p <= '9') {
        v = v * 10 + (*(*p)++ - '0');
        n++;
    }
    return n ? v : -1;
}

/* h:m[:s] at p, if it is there. */
static void read_clock(const char *p, int *h, int *mi, int *sec)
{
    int hh = digits(&p, 2);
    if (hh < 0 || *p != ':')
        return;
    p++;
    int mm = digits(&p, 2);
    if (mm < 0)
        return;
    *h = hh;
    *mi = mm;
    if (*p == ':') {
        p++;
        int ss = digits(&p, 2);
        if (ss >= 0)
            *sec = ss;
    }
}

/* A date string: ISO 8601 ("2026-10-24T13:54:46Z"), or "Oct 24 2026"-like. */
static double parse_date(const char *s)
{
    int h = 0, mi = 0, sec = 0;
    const char *p = s;
    while (*p == ' ')
        p++;
    const char *q = p;
    int y = digits(&q, 6);
    if (y >= 0 && *q == '-') {
        q++;
        int mo = digits(&q, 2), d = 1;
        if (mo < 0)
            return NAN;
        if (*q == '-') {
            q++;
            d = digits(&q, 2);
            if (d < 0)
                return NAN;
        }
        if (*q == 'T' || *q == ' ')
            read_clock(q + 1, &h, &mi, &sec);
        return make_time(y, mo - 1, d, h, mi, sec, 0);
    }
    static const char *months[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    for (int m = 0; m < 12; m++) {
        const char *at = strcasestr(s, months[m]);
        if (!at)
            continue;
        const char *r = at;
        while (*r && !(*r >= '0' && *r <= '9'))
            r++;
        int d = digits(&r, 2);
        while (*r == ',' || *r == ' ')
            r++;
        int yy = digits(&r, 4);
        if (d < 0 || yy < 0)
            return NAN;
        const char *colon = strchr(r, ':');
        if (colon && colon - r >= 2)
            read_clock(colon - 2, &h, &mi, &sec);
        return make_time(yy, m, d, h, mi, sec, 0);
    }
    return NAN;
}

static jsval date_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    double t;
    if (argc == 0)
        t = date_now_ms();
    else if (argc == 1 && args[0].t == JS_STR)
        t = parse_date(args[0].u.s->s);
    else if (argc == 1 && args[0].t == JS_OBJ && args[0].u.o->cls == C_DATE)
        t = args[0].u.o->num;
    else if (argc == 1)
        t = js_to_number(J, args[0]);
    else
        t = make_time(num_arg(J, args, argc, 0), num_arg(J, args, argc, 1),
                      argc > 2 ? num_arg(J, args, argc, 2) : 1, argc > 3 ? num_arg(J, args, argc, 3) : 0,
                      argc > 4 ? num_arg(J, args, argc, 4) : 0, argc > 5 ? num_arg(J, args, argc, 5) : 0,
                      argc > 6 ? num_arg(J, args, argc, 6) : 0);
    if (!J->constructing) {             /* Date() is a string */
        struct civil c;
        civil_from_ms(date_now_ms(), &c);
        char b[64];
        static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        static const char *mn[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        snprintf(b, sizeof(b), "%s %s %02d %d %02d:%02d:%02d GMT+0000", wd[c.wday], mn[c.month - 1],
                 c.day, c.year, c.hour, c.min, c.sec);
        return js_str(J, b);
    }
    struct jsobj *o = js_mkobj(J, C_DATE, J->date_proto);
    o->num = isnan(t) ? NAN : trunc(t);
    return js_obj(o);
}

static jsval date_now(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_num(floor(date_now_ms()));
}

static jsval date_parse_fn(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return js_num(parse_date(js_tostr(J, ARG(0))->s));
}

static jsval date_utc(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return js_num(make_time(num_arg(J, args, argc, 0), argc > 1 ? num_arg(J, args, argc, 1) : 0,
                            argc > 2 ? num_arg(J, args, argc, 2) : 1, argc > 3 ? num_arg(J, args, argc, 3) : 0,
                            argc > 4 ? num_arg(J, args, argc, 4) : 0, argc > 5 ? num_arg(J, args, argc, 5) : 0,
                            argc > 6 ? num_arg(J, args, argc, 6) : 0));
}

static struct jsobj *this_date(struct js *J, jsval self)
{
    if (self.t == JS_OBJ && self.u.o->cls == C_DATE)
        return self.u.o;
    js_throw_error(J, "TypeError: this is not a Date object");
    return NULL;
}

enum { DG_TIME, DG_YEAR, DG_MONTH, DG_DATE, DG_DAY, DG_HOURS, DG_MINUTES, DG_SECONDS, DG_MS };

static jsval date_get(struct js *J, jsval self, int what)
{
    struct jsobj *d = this_date(J, self);
    if (!d)
        return js_undefined;
    if (what == DG_TIME || isnan(d->num))
        return js_num(d->num);
    struct civil c;
    civil_from_ms(d->num, &c);
    int v = what == DG_YEAR ? c.year : what == DG_MONTH ? c.month - 1 : what == DG_DATE ? c.day :
            what == DG_DAY ? c.wday : what == DG_HOURS ? c.hour : what == DG_MINUTES ? c.min :
            what == DG_SECONDS ? c.sec : c.ms;
    return js_num(v);
}

#define DATE_GETTER(name, what) \
    static jsval date_##name(struct js *J, jsval self, jsval *args, int argc) \
    { (void)args; (void)argc; return date_get(J, self, what); }

DATE_GETTER(get_time, DG_TIME)
DATE_GETTER(get_full_year, DG_YEAR)
DATE_GETTER(get_month, DG_MONTH)
DATE_GETTER(get_date, DG_DATE)
DATE_GETTER(get_day, DG_DAY)
DATE_GETTER(get_hours, DG_HOURS)
DATE_GETTER(get_minutes, DG_MINUTES)
DATE_GETTER(get_seconds, DG_SECONDS)
DATE_GETTER(get_ms, DG_MS)

static jsval date_get_year(struct js *J, jsval self, jsval *args, int argc)
{
    jsval y = date_get_full_year(J, self, args, argc);
    return y.t == JS_NUM ? js_num(y.u.n - 1900) : y;
}

static jsval date_zero(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_num(0);                   /* getTimezoneOffset: UTC */
}

static jsval date_set(struct js *J, jsval self, jsval *args, int argc, int what)
{
    struct jsobj *d = this_date(J, self);
    if (!d)
        return js_undefined;
    if (what == DG_TIME) {
        d->num = trunc(num_arg(J, args, argc, 0));
        return js_num(d->num);
    }
    struct civil c;
    civil_from_ms(isnan(d->num) ? 0 : d->num, &c);
    double f[7] = { c.year, c.month - 1, c.day, c.hour, c.min, c.sec, c.ms };
    int first = what == DG_YEAR ? 0 : what == DG_MONTH ? 1 : what == DG_DATE ? 2 :
                what == DG_HOURS ? 3 : what == DG_MINUTES ? 4 : what == DG_SECONDS ? 5 : 6;
    for (int i = 0; i < argc && first + i < 7; i++)
        f[first + i] = js_to_number(J, args[i]);
    d->num = make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    return js_num(d->num);
}

#define DATE_SETTER(name, what) \
    static jsval date_##name(struct js *J, jsval self, jsval *args, int argc) \
    { return date_set(J, self, args, argc, what); }

DATE_SETTER(set_time, DG_TIME)
DATE_SETTER(set_full_year, DG_YEAR)
DATE_SETTER(set_month, DG_MONTH)
DATE_SETTER(set_date, DG_DATE)
DATE_SETTER(set_hours, DG_HOURS)
DATE_SETTER(set_minutes, DG_MINUTES)
DATE_SETTER(set_seconds, DG_SECONDS)
DATE_SETTER(set_ms, DG_MS)

static const char *const wday_names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const month_names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
                                           "Sep", "Oct", "Nov", "Dec" };

enum { DF_FULL, DF_ISO, DF_DATE, DF_TIME, DF_LOCALE, DF_LOCALE_DATE, DF_LOCALE_TIME, DF_UTC };

static jsval date_format(struct js *J, jsval self, int fmt)
{
    struct jsobj *d = this_date(J, self);
    if (!d)
        return js_undefined;
    if (isnan(d->num)) {
        if (fmt == DF_ISO)
            return js_throw_error(J, "RangeError: invalid time value");
        return js_str(J, "Invalid Date");
    }
    struct civil c;
    civil_from_ms(d->num, &c);
    char b[96];
    int h12 = c.hour % 12 ? c.hour % 12 : 12;
    const char *ampm = c.hour < 12 ? "AM" : "PM";
    switch (fmt) {
    case DF_ISO:
        snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", c.year, c.month, c.day, c.hour,
                 c.min, c.sec, c.ms);
        break;
    case DF_DATE:
        snprintf(b, sizeof(b), "%s %s %02d %d", wday_names[c.wday], month_names[c.month - 1], c.day, c.year);
        break;
    case DF_TIME:
        snprintf(b, sizeof(b), "%02d:%02d:%02d GMT+0000 (Coordinated Universal Time)", c.hour, c.min, c.sec);
        break;
    case DF_LOCALE:
        snprintf(b, sizeof(b), "%d/%d/%d, %d:%02d:%02d %s", c.month, c.day, c.year, h12, c.min, c.sec, ampm);
        break;
    case DF_LOCALE_DATE:
        snprintf(b, sizeof(b), "%d/%d/%d", c.month, c.day, c.year);
        break;
    case DF_LOCALE_TIME:
        snprintf(b, sizeof(b), "%d:%02d:%02d %s", h12, c.min, c.sec, ampm);
        break;
    case DF_UTC:
        snprintf(b, sizeof(b), "%s, %02d %s %d %02d:%02d:%02d GMT", wday_names[c.wday], c.day,
                 month_names[c.month - 1], c.year, c.hour, c.min, c.sec);
        break;
    default:
        snprintf(b, sizeof(b), "%s %s %02d %d %02d:%02d:%02d GMT+0000 (Coordinated Universal Time)",
                 wday_names[c.wday], month_names[c.month - 1], c.day, c.year, c.hour, c.min, c.sec);
        break;
    }
    return js_str(J, b);
}

#define DATE_FORMAT(name, fmt) \
    static jsval date_##name(struct js *J, jsval self, jsval *args, int argc) \
    { (void)args; (void)argc; return date_format(J, self, fmt); }

DATE_FORMAT(to_string, DF_FULL)
DATE_FORMAT(to_iso, DF_ISO)
DATE_FORMAT(to_date_string, DF_DATE)
DATE_FORMAT(to_time_string, DF_TIME)
DATE_FORMAT(to_locale, DF_LOCALE)
DATE_FORMAT(to_locale_date, DF_LOCALE_DATE)
DATE_FORMAT(to_locale_time, DF_LOCALE_TIME)
DATE_FORMAT(to_utc, DF_UTC)

/* ---- Error ---- */

static jsval error_ctor_named(struct js *J, jsval *args, int argc, const char *type)
{
    struct jsobj *e = js_make_error(J, type, "").u.o;
    if (argc && args[0].t != JS_UNDEF)
        js_put(J, e, S(J, "message"), js_to_string(J, args[0]), 1);
    else
        js_put(J, e, S(J, "message"), js_str(J, ""), 1);
    if (argc > 1 && args[1].t == JS_OBJ && js_has(J, args[1], S(J, "cause")))     /* new Error(m, { cause }) */
        js_put(J, e, S(J, "cause"), js_get(J, args[1], "cause"), 1);
    return js_obj(e);
}

#define ERROR_CTOR(name) \
    static jsval name##_ctor(struct js *J, jsval self, jsval *args, int argc) \
    { (void)self; return error_ctor_named(J, args, argc, #name); }

ERROR_CTOR(Error)
ERROR_CTOR(TypeError)
ERROR_CTOR(RangeError)
ERROR_CTOR(SyntaxError)
ERROR_CTOR(ReferenceError)
ERROR_CTOR(EvalError)
ERROR_CTOR(URIError)

static jsval error_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    jsval name = js_get(J, self, "name"), msg = js_get(J, self, "message");
    struct jsstr *n = name.t == JS_UNDEF ? S(J, "Error") : js_tostr(J, name);
    struct jsstr *m = msg.t == JS_UNDEF ? S(J, "") : js_tostr(J, msg);
    if (!m->len)
        return strv(n);
    if (!n->len)
        return strv(m);
    return strv(js_concat(J, js_concat(J, n, S(J, ": ")), m));
}

/* ---- Map and Set: arrays of keys and values, searched in order ---- */

static jsval collection_items(struct js *J, jsval self, const char *which)
{
    jsval v = self.t == JS_OBJ ? js_get(J, self, which) : js_undefined;
    if (!js_is_array(v))
        return js_throw_error(J, "TypeError: not a Map or Set");
    return v;
}

static int collection_find(jsval keys, jsval k)
{
    for (int i = 0; i < keys.u.o->len; i++)
        if (same_value_zero(keys.u.o->items[i], k))
            return i;
    return -1;
}

static void collection_init(struct js *J, struct jsobj *o, int map, jsval src)
{
    js_put(J, o, S(J, "__items"), js_obj(js_new_array(J)), 1);
    if (map)
        js_put(J, o, S(J, "__values"), js_obj(js_new_array(J)), 1);
    if (src.t == JS_UNDEF || src.t == JS_NULL)
        return;
    struct iter iter;
    if (js_iter_open(J, src, &iter) < 0)
        return;
    jsval it;
    int r;
    while ((r = js_iter_next(J, &iter, &it)) > 0) {
        jsval keys = js_get(J, js_obj(o), "__items");
        if (map) {
            jsval k = js_getv(J, it, js_num(0)), v = js_getv(J, it, js_num(1));
            jsval vals = js_get(J, js_obj(o), "__values");
            int at = collection_find(keys, k);
            if (at >= 0) {
                vals.u.o->items[at] = v;
            } else {
                js_array_push(J, keys.u.o, k);
                js_array_push(J, vals.u.o, v);
            }
        } else if (collection_find(keys, it) < 0) {
            js_array_push(J, keys.u.o, it);
        }
    }
}


static jsval map_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (!J->constructing)
        return js_throw_error(J, "TypeError: Map needs new");
    struct jsobj *o = js_mkobj(J, C_OBJECT, J->map_proto);
    collection_init(J, o, 1, ARG(0));
    return js_obj(o);
}

static jsval set_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    if (!J->constructing)
        return js_throw_error(J, "TypeError: Set needs new");
    struct jsobj *o = js_mkobj(J, C_OBJECT, J->set_proto);
    collection_init(J, o, 0, ARG(0));
    return js_obj(o);
}

static jsval map_get(struct js *J, jsval self, jsval *args, int argc)
{
    jsval keys = collection_items(J, self, "__items");
    if (J->throwing)
        return js_undefined;
    int at = collection_find(keys, ARG(0));
    return at < 0 ? js_undefined : js_get(J, self, "__values").u.o->items[at];
}

static jsval map_set(struct js *J, jsval self, jsval *args, int argc)
{
    jsval keys = collection_items(J, self, "__items");
    if (J->throwing)
        return js_undefined;
    jsval vals = js_get(J, self, "__values");
    int at = collection_find(keys, ARG(0));
    if (at >= 0) {
        vals.u.o->items[at] = ARG(1);
    } else {
        js_array_push(J, keys.u.o, ARG(0));
        js_array_push(J, vals.u.o, ARG(1));
    }
    return self;
}

static jsval collection_has(struct js *J, jsval self, jsval *args, int argc)
{
    jsval keys = collection_items(J, self, "__items");
    if (J->throwing)
        return js_undefined;
    return js_bool(collection_find(keys, ARG(0)) >= 0);
}

static jsval collection_delete(struct js *J, jsval self, jsval *args, int argc)
{
    jsval keys = collection_items(J, self, "__items");
    if (J->throwing)
        return js_undefined;
    int at = collection_find(keys, ARG(0));
    if (at < 0)
        return js_bool(0);
    jsval idx = js_num(at), one = js_num(1);
    jsval sa[2] = { idx, one };
    array_splice(J, keys, sa, 2);
    jsval vals = js_get(J, self, "__values");
    if (js_is_array(vals))
        array_splice(J, vals, sa, 2);
    return js_bool(1);
}

static jsval collection_clear(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    jsval keys = collection_items(J, self, "__items");
    if (J->throwing)
        return js_undefined;
    keys.u.o->len = 0;
    jsval vals = js_get(J, self, "__values");
    if (js_is_array(vals))
        vals.u.o->len = 0;
    return js_undefined;
}

static jsval set_add(struct js *J, jsval self, jsval *args, int argc)
{
    jsval keys = collection_items(J, self, "__items");
    if (J->throwing)
        return js_undefined;
    if (collection_find(keys, ARG(0)) < 0)
        js_array_push(J, keys.u.o, ARG(0));
    return self;
}

static jsval collection_for_each(struct js *J, jsval self, jsval *args, int argc)
{
    jsval keys = collection_items(J, self, "__items");
    if (J->throwing)
        return js_undefined;
    jsval vals = js_get(J, self, "__values");
    for (int i = 0; i < keys.u.o->len; i++) {
        jsval k = keys.u.o->items[i];
        jsval cb[3] = { js_is_array(vals) ? vals.u.o->items[i] : k, k, self };
        js_call_value(J, ARG(0), ARG(1), cb, 3);
        if (J->throwing)
            return js_undefined;
    }
    return js_undefined;
}

/* Map and Set iterators walk the key and value arrays as they are then. */
static jsval collection_iter(struct js *J, jsval self, int kind)
{
    jsval keys = collection_items(J, self, "__items");
    if (J->throwing)
        return js_undefined;
    jsval vals = js_get(J, self, "__values");
    return make_seqiter(J, keys, js_is_array(vals) ? vals : js_undefined, kind, 0);
}

static jsval collection_keys(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return collection_iter(J, self, ITK_KEYS);
}

static jsval collection_values(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return collection_iter(J, self, ITK_VALUES);
}

static jsval collection_entries(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    return collection_iter(J, self, ITK_ENTRIES);
}

/* ---- iterators: what keys(), values(), entries() and [Symbol.iterator] return ---- */

struct seqiter {
    jsval keys, vals;           /* an array or array-like to walk, and a Map's values */
    struct jsstr *str;          /* or a string's characters */
    int i, kind, index_keys;    /* index_keys: the keys are indexes (arrays) */
    int done;
};

static const struct jsclass seqiter_class = { "Iterator", NULL, NULL, NULL };

static jsval make_seqiter(struct js *J, jsval keys, jsval vals, int kind, int index_keys)
{
    struct seqiter *it = js_alloc(J, sizeof(*it));
    memset(it, 0, sizeof(*it));
    it->keys = keys;
    it->vals = vals;
    it->kind = kind;
    it->index_keys = index_keys;
    struct jsobj *o = js_new_host(J, &seqiter_class, it);
    o->proto = J->seqiter_proto;
    return js_obj(o);
}

static jsval iter_result(struct js *J, jsval value, int done)
{
    struct jsobj *r = js_new_object(J);
    js_put(J, r, J->s_value, value, 0);
    js_put(J, r, J->s_done, js_bool(done), 0);
    return js_obj(r);
}

static int seq_len(struct js *J, jsval a)
{
    if (a.t == JS_OBJ && (a.u.o->cls == C_ARRAY || a.u.o->cls == C_ARGUMENTS))
        return a.u.o->len;
    double d = js_to_number(J, js_get(J, a, "length"));
    return d > 0 && d < (1 << 30) ? (int)d : 0;
}

static jsval seq_at(struct js *J, jsval a, int i)
{
    if (a.t == JS_OBJ && (a.u.o->cls == C_ARRAY || a.u.o->cls == C_ARGUMENTS))
        return i < a.u.o->len ? a.u.o->items[i] : js_undefined;
    return js_getv(J, a, js_num(i));
}

static jsval seqiter_next(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct seqiter *it = self.t == JS_OBJ ? js_host_data(self.u.o, &seqiter_class) : NULL;
    if (!it)
        return js_throw_error(J, "TypeError: next() called on something that is not an iterator");
    if (it->done)
        return iter_result(J, js_undefined, 1);
    if (it->str) {
        if (it->i >= it->str->len) {
            it->done = 1;
            return iter_result(J, js_undefined, 1);
        }
        unsigned char c = (unsigned char)it->str->s[it->i];
        int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (n > it->str->len - it->i)
            n = it->str->len - it->i;
        jsval ch = js_strn(J, it->str->s + it->i, n);
        it->i += n;
        return iter_result(J, ch, 0);
    }
    if (it->i >= seq_len(J, it->keys)) {
        it->done = 1;
        return iter_result(J, js_undefined, 1);
    }
    int i = it->i++;
    jsval k = it->index_keys ? js_num(i) : seq_at(J, it->keys, i);
    if (it->kind == ITK_KEYS)
        return iter_result(J, k, 0);
    jsval v = it->vals.t == JS_OBJ ? seq_at(J, it->vals, i) : it->index_keys ? seq_at(J, it->keys, i) : k;
    if (it->kind == ITK_VALUES)
        return iter_result(J, v, 0);
    struct jsobj *pair = js_new_array(J);
    js_array_push(J, pair, k);
    js_array_push(J, pair, v);
    return iter_result(J, js_obj(pair), 0);
}

static jsval seqiter_self(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)args;
    (void)argc;
    return self;
}

static jsval string_iterator(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct jsstr *str = this_string(J, self);
    if (!str)
        return js_undefined;
    jsval r = make_seqiter(J, js_undefined, js_undefined, ITK_VALUES, 0);
    ((struct seqiter *)r.u.o->hdata)->str = str;
    return r;
}

static void put_value(struct js *J, struct jsobj *o, const char *name, jsval v);

/* ---- console ---- */

static jsval console_log(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    char line[1024];
    int n = 0;
    for (int i = 0; i < argc && n < (int)sizeof(line) - 2; i++) {
        const char *t;
        if (args[i].t == JS_OBJ && args[i].u.o->cls != C_FUNCTION && args[i].u.o->cls != C_ERROR &&
            args[i].u.o->cls != C_HOST) {
            struct jsstr *s = js_json_stringify(J, args[i], NULL);
            J->throwing = 0;
            t = s ? s->s : "[object]";
        } else {
            t = js_text(J, args[i]);
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%s", i ? " " : "", t);
    }
    fprintf(stderr, "console: %s\n", line);
    return js_undefined;
}

/* ---- setup ---- */

static void put_value(struct js *J, struct jsobj *o, const char *name, jsval v)
{
    js_put(J, o, S(J, name), v, 1);
}

static jsval collection_size_getter(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    jsval keys = collection_items(J, self, "__items");
    if (J->throwing)
        return js_undefined;
    return js_num(keys.u.o->len);
}

static void define_getter(struct js *J, struct jsobj *o, const char *name, js_native fn)
{
    js_put(J, o, S(J, name), js_undefined, 1);
    struct prop *p = js_own(o, S(J, name));
    p->getter = js_new_function(J, fn, name, 0).u.o;
}

static jsval regexp_source(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct regex *re = regex_of(self);
    return js_str(J, re ? re_source(re) : "(?:)");
}

static jsval regexp_flags_get(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct regex *re = regex_of(self);
    return js_str(J, re ? re_flags(re) : "");
}

static jsval regexp_global_get(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)args;
    (void)argc;
    struct regex *re = regex_of(self);
    return js_bool(re && re_global(re));
}

static jsval regexp_icase_get(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)args;
    (void)argc;
    struct regex *re = regex_of(self);
    return js_bool(re && re_icase(re));
}

void js_init_lib(struct js *J)
{
    struct jsobj *g = J->global;

    /* Object */
    struct jsobj *op = J->object_proto;
    struct jsobj *object = ctor_obj(J, "Object", object_ctor, 1, op);
    fn_obj(J, object, "keys", object_keys, 1);
    fn_obj(J, object, "values", object_values, 1);
    fn_obj(J, object, "entries", object_entries, 1);
    fn_obj(J, object, "assign", object_assign, 2);
    fn_obj(J, object, "create", object_create, 2);
    fn_obj(J, object, "freeze", object_identity, 1);
    fn_obj(J, object, "seal", object_identity, 1);
    fn_obj(J, object, "preventExtensions", object_identity, 1);
    fn_obj(J, object, "isFrozen", object_false, 1);
    fn_obj(J, object, "isExtensible", object_true, 1);
    fn_obj(J, object, "getOwnPropertyDescriptor", object_get_own_descriptor, 2);
    fn_obj(J, object, "getOwnPropertyDescriptors", object_get_own_descriptors, 1);
    fn_obj(J, object, "getOwnPropertySymbols", object_no_symbols, 1);
    fn_obj(J, object, "isSealed", object_false, 1);
    fn_obj(J, object, "getPrototypeOf", object_get_proto, 1);
    fn_obj(J, object, "setPrototypeOf", object_set_proto, 2);
    fn_obj(J, object, "defineProperty", object_define_property, 3);
    fn_obj(J, object, "defineProperties", object_define_properties, 2);
    fn_obj(J, object, "getOwnPropertyNames", object_own_names, 1);
    fn_obj(J, object, "is", object_is, 2);
    fn_obj(J, object, "fromEntries", object_from_entries, 1);
    fn_obj(J, op, "hasOwnProperty", proto_has_own, 1);
    fn_obj(J, op, "isPrototypeOf", proto_is_prototype_of, 1);
    fn_obj(J, op, "propertyIsEnumerable", proto_has_own, 1);
    fn_obj(J, op, "toString", proto_to_string, 0);
    fn_obj(J, op, "toLocaleString", proto_to_string, 0);
    fn_obj(J, op, "valueOf", proto_value_of, 0);

    /* Function */
    struct jsobj *fp = J->function_proto;
    fp->native = object_identity;       /* Function.prototype is callable */
    ctor_obj(J, "Function", function_ctor, 1, fp);
    fn_obj(J, fp, "call", function_call, 1);
    fn_obj(J, fp, "apply", function_apply, 2);
    fn_obj(J, fp, "bind", function_bind, 1);
    fn_obj(J, fp, "toString", function_to_string, 0);

    /* Array */
    struct jsobj *ap = J->array_proto = js_mkobj(J, C_ARRAY, op);
    struct jsobj *array = ctor_obj(J, "Array", array_ctor, 1, ap);
    fn_obj(J, array, "isArray", array_is_array, 1);
    fn_obj(J, array, "from", array_from, 1);
    fn_obj(J, array, "of", array_of, 0);
    static const struct { const char *name; js_native fn; int n; } array_methods[] = {
        { "push", array_push, 1 }, { "pop", array_pop, 0 }, { "shift", array_shift, 0 },
        { "unshift", array_unshift, 1 }, { "slice", array_slice, 2 }, { "splice", array_splice, 2 },
        { "concat", array_concat, 1 }, { "join", array_join, 1 }, { "reverse", array_reverse, 0 },
        { "indexOf", array_index_of, 1 }, { "lastIndexOf", array_last_index_of, 1 },
        { "includes", array_includes, 1 }, { "forEach", array_for_each, 1 }, { "map", array_map, 1 },
        { "filter", array_filter, 1 }, { "some", array_some, 1 }, { "every", array_every, 1 },
        { "find", array_find, 1 }, { "findIndex", array_find_index, 1 }, { "reduce", array_reduce, 1 },
        { "reduceRight", array_reduce_right, 1 }, { "fill", array_fill, 1 }, { "sort", array_sort, 1 },
        { "flat", array_flat, 0 }, { "keys", array_keys, 0 }, { "entries", array_entries, 0 },
        { "values", array_values, 0 }, { "@@iterator", array_values, 0 },
        { "toString", array_to_string, 0 },
        { "toLocaleString", array_to_string, 0 },
    };
    for (unsigned i = 0; i < sizeof(array_methods) / sizeof(array_methods[0]); i++)
        fn_obj(J, ap, array_methods[i].name, array_methods[i].fn, array_methods[i].n);

    /* String */
    struct jsobj *sp = J->string_proto = js_mkobj(J, C_STRING, op);
    sp->str = js_mkstr(J, "", 0);
    struct jsobj *string = ctor_obj(J, "String", string_ctor, 1, sp);
    fn_obj(J, string, "fromCharCode", string_from_char_code, 1);
    fn_obj(J, string, "fromCodePoint", string_from_char_code, 1);
    static const struct {
        const char *name;
        js_native fn;
        int n;
    } string_methods[] = {
        {"charAt", string_char_at, 1},
        {"charCodeAt", string_char_code_at, 1},
        {"codePointAt", string_code_point_at, 1},
        {"indexOf", string_index_of, 1},
        {"lastIndexOf", string_last_index_of, 1},
        {"includes", string_includes, 1},
        {"startsWith", string_starts_with, 1},
        {"endsWith", string_ends_with, 1},
        {"slice", string_slice, 2},
        {"substring", string_substring, 2},
        {"substr", string_substr, 2},
        {"toUpperCase", string_upper, 0},
        {"toLowerCase", string_lower, 0},
        {"toLocaleUpperCase", string_upper, 0},
        {"toLocaleLowerCase", string_lower, 0},
        {"trim", string_trim, 0},
        {"trimStart", string_trim_start, 0},
        {"trimEnd", string_trim_end, 0},
        {"trimLeft", string_trim_start, 0},
        {"trimRight", string_trim_end, 0},
        {"padStart", string_pad_start, 2},
        {"padEnd", string_pad_end, 2},
        {"repeat", string_repeat, 1},
        {"concat", string_concat, 1},
        {"split", string_split, 2},
        {"replace", string_replace, 2},
        {"replaceAll", string_replace_all, 2},
        {"match", string_match, 1},
        {"matchAll", string_match_all, 1},
        {"search", string_search, 1},
        {"localeCompare", string_locale_compare, 1},
        {"toString", string_to_string, 0},
        {"valueOf", string_to_string, 0},
        {"at", string_at, 1},
    };
    for (unsigned i = 0; i < sizeof(string_methods) / sizeof(string_methods[0]); i++)
        fn_obj(J, sp, string_methods[i].name, string_methods[i].fn, string_methods[i].n);

    /* Number and Boolean */
    struct jsobj *np = J->number_proto = js_mkobj(J, C_NUMBER, op);
    struct jsobj *number = ctor_obj(J, "Number", number_ctor, 1, np);
    fn_obj(J, np, "toFixed", number_to_fixed, 1);
    fn_obj(J, np, "toString", number_to_string, 1);
    fn_obj(J, np, "toPrecision", number_to_precision, 1);
    fn_obj(J, np, "valueOf", number_value_of, 0);
    fn_obj(J, np, "toLocaleString", number_to_locale, 0);
    fn_obj(J, number, "isInteger", number_is_integer, 1);
    fn_obj(J, number, "isSafeInteger", number_is_integer, 1);
    fn_obj(J, number, "isNaN", number_is_nan, 1);
    fn_obj(J, number, "isFinite", number_is_finite, 1);
    fn_obj(J, number, "parseFloat", global_parse_float, 1);
    fn_obj(J, number, "parseInt", global_parse_int, 2);
    put_value(J, number, "MAX_SAFE_INTEGER", js_num(9007199254740991.0));
    put_value(J, number, "MIN_SAFE_INTEGER", js_num(-9007199254740991.0));
    put_value(J, number, "EPSILON", js_num(2.220446049250313e-16));
    put_value(J, number, "MAX_VALUE", js_num(1.7976931348623157e308));
    put_value(J, number, "MIN_VALUE", js_num(5e-324));
    put_value(J, number, "POSITIVE_INFINITY", js_num(INFINITY));
    put_value(J, number, "NEGATIVE_INFINITY", js_num(-INFINITY));
    put_value(J, number, "NaN", js_num(NAN));
    struct jsobj *bp = J->boolean_proto = js_mkobj(J, C_BOOLEAN, op);
    ctor_obj(J, "Boolean", boolean_ctor, 1, bp);
    fn_obj(J, bp, "toString", boolean_to_string, 0);
    fn_obj(J, bp, "valueOf", boolean_value_of, 0);

    /* globals */
    fn_obj(J, g, "parseInt", global_parse_int, 2);
    fn_obj(J, g, "parseFloat", global_parse_float, 1);
    fn_obj(J, g, "isNaN", global_is_nan, 1);
    fn_obj(J, g, "isFinite", global_is_finite, 1);
    fn_obj(J, g, "encodeURIComponent", global_encode_uri_component, 1);
    fn_obj(J, g, "encodeURI", global_encode_uri, 1);
    fn_obj(J, g, "decodeURIComponent", global_decode_uri, 1);
    fn_obj(J, g, "decodeURI", global_decode_uri, 1);
    fn_obj(J, g, "escape", global_escape, 1);
    fn_obj(J, g, "unescape", global_decode_uri, 1);
    put_value(J, g, "NaN", js_num(NAN));
    put_value(J, g, "Infinity", js_num(INFINITY));
    put_value(J, g, "undefined", js_undefined);
    put_value(J, g, "globalThis", js_obj(g));

    /* Math */
    struct jsobj *math = js_new_object(J);
    put_value(J, g, "Math", js_obj(math));
    static const struct { const char *name; js_native fn; int n; } math_fns[] = {
        { "abs", math_abs, 1 }, { "floor", math_floor, 1 }, { "ceil", math_ceil, 1 },
        { "round", math_round, 1 }, { "sqrt", math_sqrt, 1 }, { "sin", math_sin, 1 },
        { "cos", math_cos, 1 }, { "tan", math_tan, 1 }, { "atan", math_atan, 1 },
        { "asin", math_asin, 1 }, { "acos", math_acos, 1 }, { "atan2", math_atan2, 2 },
        { "exp", math_exp, 1 }, { "log", math_log, 1 }, { "log2", math_log2, 1 },
        { "log10", math_log10, 1 }, { "pow", math_pow, 2 }, { "max", math_max, 2 },
        { "min", math_min, 2 }, { "random", math_random, 0 }, { "trunc", math_trunc, 1 },
        { "sign", math_sign, 1 }, { "cbrt", math_cbrt, 1 }, { "hypot", math_hypot, 2 },
    };
    for (unsigned i = 0; i < sizeof(math_fns) / sizeof(math_fns[0]); i++)
        fn_obj(J, math, math_fns[i].name, math_fns[i].fn, math_fns[i].n);
    put_value(J, math, "PI", js_num(M_PI));
    put_value(J, math, "E", js_num(M_E));
    put_value(J, math, "LN2", js_num(M_LN2));
    put_value(J, math, "LN10", js_num(2.302585092994046));
    put_value(J, math, "LOG2E", js_num(M_LOG2E));
    put_value(J, math, "LOG10E", js_num(0.4342944819032518));
    put_value(J, math, "SQRT2", js_num(M_SQRT2));
    put_value(J, math, "SQRT1_2", js_num(0.7071067811865476));

    /* JSON */
    struct jsobj *json = js_new_object(J);
    put_value(J, g, "JSON", js_obj(json));
    fn_obj(J, json, "stringify", json_stringify, 3);
    fn_obj(J, json, "parse", json_parse, 2);

    /* Date */
    struct jsobj *dp = J->date_proto = js_mkobj(J, C_OBJECT, op);
    struct jsobj *date = ctor_obj(J, "Date", date_ctor, 7, dp);
    fn_obj(J, date, "now", date_now, 0);
    fn_obj(J, date, "parse", date_parse_fn, 1);
    fn_obj(J, date, "UTC", date_utc, 7);
    static const struct { const char *name; js_native fn; } date_methods[] = {
        { "getTime", date_get_time }, { "valueOf", date_get_time }, { "getFullYear", date_get_full_year },
        { "getYear", date_get_year }, { "getMonth", date_get_month }, { "getDate", date_get_date },
        { "getDay", date_get_day }, { "getHours", date_get_hours }, { "getMinutes", date_get_minutes },
        { "getSeconds", date_get_seconds }, { "getMilliseconds", date_get_ms },
        { "getUTCFullYear", date_get_full_year }, { "getUTCMonth", date_get_month },
        { "getUTCDate", date_get_date }, { "getUTCDay", date_get_day }, { "getUTCHours", date_get_hours },
        { "getUTCMinutes", date_get_minutes }, { "getUTCSeconds", date_get_seconds },
        { "getUTCMilliseconds", date_get_ms }, { "getTimezoneOffset", date_zero },
        { "setTime", date_set_time }, { "setFullYear", date_set_full_year }, { "setMonth", date_set_month },
        { "setDate", date_set_date }, { "setHours", date_set_hours }, { "setMinutes", date_set_minutes },
        { "setSeconds", date_set_seconds }, { "setMilliseconds", date_set_ms },
        { "setUTCFullYear", date_set_full_year }, { "setUTCMonth", date_set_month },
        { "setUTCDate", date_set_date }, { "setUTCHours", date_set_hours },
        { "setUTCMinutes", date_set_minutes }, { "setUTCSeconds", date_set_seconds },
        { "toString", date_to_string }, { "toISOString", date_to_iso }, { "toJSON", date_to_iso },
        { "toDateString", date_to_date_string }, { "toTimeString", date_to_time_string },
        { "toLocaleString", date_to_locale }, { "toLocaleDateString", date_to_locale_date },
        { "toLocaleTimeString", date_to_locale_time }, { "toUTCString", date_to_utc },
        { "toGMTString", date_to_utc },
    };
    for (unsigned i = 0; i < sizeof(date_methods) / sizeof(date_methods[0]); i++)
        fn_obj(J, dp, date_methods[i].name, date_methods[i].fn, 0);

    /* RegExp */
    struct jsobj *rp = J->regexp_proto = js_mkobj(J, C_OBJECT, op);
    ctor_obj(J, "RegExp", regexp_ctor, 2, rp);
    fn_obj(J, rp, "exec", regexp_exec, 1);
    fn_obj(J, rp, "test", regexp_test, 1);
    fn_obj(J, rp, "toString", regexp_to_string, 0);
    define_getter(J, rp, "source", regexp_source);
    define_getter(J, rp, "flags", regexp_flags_get);
    define_getter(J, rp, "global", regexp_global_get);
    define_getter(J, rp, "ignoreCase", regexp_icase_get);

    /* Errors */
    struct jsobj *ep = J->error_proto = js_mkobj(J, C_OBJECT, op);
    ctor_obj(J, "Error", Error_ctor, 1, ep);
    put_value(J, ep, "name", js_str(J, "Error"));
    put_value(J, ep, "message", js_str(J, ""));
    fn_obj(J, ep, "toString", error_to_string, 0);
    static const struct { const char *name; js_native fn; } errors[] = {
        { "TypeError", TypeError_ctor }, { "RangeError", RangeError_ctor },
        { "SyntaxError", SyntaxError_ctor }, { "ReferenceError", ReferenceError_ctor },
        { "EvalError", EvalError_ctor }, { "URIError", URIError_ctor },
    };
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        struct jsobj *p = js_mkobj(J, C_OBJECT, ep);
        put_value(J, p, "name", js_str(J, errors[i].name));
        ctor_obj(J, errors[i].name, errors[i].fn, 1, p);
    }

    /* Map and Set */
    J->map_proto = js_mkobj(J, C_OBJECT, op);
    ctor_obj(J, "Map", map_ctor, 0, J->map_proto);
    fn_obj(J, J->map_proto, "get", map_get, 1);
    fn_obj(J, J->map_proto, "set", map_set, 2);
    fn_obj(J, J->map_proto, "has", collection_has, 1);
    fn_obj(J, J->map_proto, "delete", collection_delete, 1);
    fn_obj(J, J->map_proto, "clear", collection_clear, 0);
    fn_obj(J, J->map_proto, "forEach", collection_for_each, 1);
    fn_obj(J, J->map_proto, "keys", collection_keys, 0);
    fn_obj(J, J->map_proto, "values", collection_values, 0);
    fn_obj(J, J->map_proto, "entries", collection_entries, 0);
    fn_obj(J, J->map_proto, "@@iterator", collection_entries, 0);
    define_getter(J, J->map_proto, "size", collection_size_getter);
    J->set_proto = js_mkobj(J, C_OBJECT, op);
    ctor_obj(J, "Set", set_ctor, 0, J->set_proto);
    fn_obj(J, J->set_proto, "add", set_add, 1);
    fn_obj(J, J->set_proto, "has", collection_has, 1);
    fn_obj(J, J->set_proto, "delete", collection_delete, 1);
    fn_obj(J, J->set_proto, "clear", collection_clear, 0);
    fn_obj(J, J->set_proto, "forEach", collection_for_each, 1);
    fn_obj(J, J->set_proto, "keys", collection_keys, 0);
    fn_obj(J, J->set_proto, "values", collection_values, 0);
    fn_obj(J, J->set_proto, "entries", collection_entries, 0);
    fn_obj(J, J->set_proto, "@@iterator", collection_values, 0);
    define_getter(J, J->set_proto, "size", collection_size_getter);

    J->seqiter_proto = js_mkobj(J, C_OBJECT, op);
    fn_obj(J, J->seqiter_proto, "next", seqiter_next, 0);
    fn_obj(J, J->seqiter_proto, "@@iterator", seqiter_self, 0);
    J->s_value = S(J, "value");
    J->s_done = S(J, "done");
    fn_obj(J, sp, "@@iterator", string_iterator, 0);

    js_init_binary(J);

    /* console */
    struct jsobj *console = js_new_object(J);
    put_value(J, g, "console", js_obj(console));
    fn_obj(J, console, "log", console_log, 0);
    fn_obj(J, console, "info", console_log, 0);
    fn_obj(J, console, "warn", console_log, 0);
    fn_obj(J, console, "error", console_log, 0);
    fn_obj(J, console, "debug", console_log, 0);
    fn_obj(J, console, "trace", console_log, 0);

    js_init_lib2(J);
}
