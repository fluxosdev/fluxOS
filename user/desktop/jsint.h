/*
 * The JavaScript engine's insides, shared by js.c (values, the parser and
 * the interpreter), jslib.c (the built-in objects) and jsre.c (regular
 * expressions). Nothing outside the engine includes this.
 */
#ifndef JSINT_H
#define JSINT_H

#include "js.h"
#include "web.h"

#define JS_HEAP_LIMIT (48u << 20)       /* bytes a page's scripts may allocate */
#define JS_STEP_LIMIT 40000000u         /* steps one entry into the engine may take (a second or so) */
#define JS_MAX_DEPTH  180               /* nested calls */

struct jsstr {
    int len;                            /* bytes of UTF-8 */
    char s[];                           /* NUL-terminated as well */
};

struct prop {
    struct jsstr *key;
    jsval v;
    struct jsobj *getter, *setter;      /* an accessor property */
    struct prop *next;
    uint8_t hidden;                     /* not enumerable */
};

enum {
    C_OBJECT, C_ARRAY, C_FUNCTION, C_ERROR, C_DATE, C_REGEXP, C_HOST, C_BOOLEAN, C_NUMBER,
    C_STRING, C_ARGUMENTS,
};

struct ast;
struct coro;
struct env;
struct regex;

/* A microtask: run(J, job) later, after the script that queued it returns. */
struct job {
    struct job *next;
    void (*run)(struct js *J, struct job *job);
    jsval a, b, c, d;
};

struct jsobj {
    uint8_t cls;
    uint8_t arrow;                      /* an arrow function: this from its scope */
    uint8_t ctor;                       /* a native that may be called with new */
    struct jsobj *proto;
    struct prop *props;
    /* C_ARRAY, C_ARGUMENTS */
    jsval *items;
    int len, cap;
    /* C_FUNCTION */
    struct ast *fn;                     /* NULL for natives */
    struct env *scope;
    js_native native;
    const char *name;
    int nargs;
    struct jsobj *home;                 /* a method's home object, for super */
    uint8_t is_class;                   /* a class constructor: not callable without new */
    struct jsobj *bound;                /* Function.prototype.bind */
    jsval bound_this;
    jsval *bound_args;
    int nbound;
    /* C_HOST */
    const struct jsclass *hcls;
    void *hdata;
    /* C_DATE, C_NUMBER, C_BOOLEAN: the value; C_STRING; C_REGEXP */
    double num;
    struct jsstr *str;
    struct regex *re;
};

struct js {
    struct arena arena;
    void *host;
    struct jsobj *global;
    struct env *genv;
    /* prototypes */
    struct jsobj *object_proto, *function_proto, *array_proto, *string_proto, *number_proto,
                 *boolean_proto, *error_proto, *date_proto, *regexp_proto;
    /* a thrown value in flight */
    int throwing;
    jsval exc;
    /* control flow */
    jsval retval;
    jsval completion;                   /* the last expression statement's value (eval) */
    struct jsstr *label;                /* break/continue label */
    int constructing;                   /* a native called with new */
    unsigned steps, step_limit;
    int depth;
    int dead;                           /* out of memory or time: no more scripts */
    struct jsstr *s_home;               /* the hidden variable that holds a method's home object */
    struct job *jobs, *jobs_tail;       /* the microtask queue (promises) */
    int in_jobs;
    struct jsobj *promise_proto, *generator_proto;
    struct coro *cur_co, *co_live;      /* the running coroutine, all those with a stack */
    int nco;
    unsigned lazy_done;                 /* which lazily loaded library chunks have run (jslib2.c) */
    struct jsobj *return_marker;        /* thrown to unwind a generator that is being closed */
    char err[512];
    char textbuf[1024];
};

/* js.c */
void *js_alloc(struct js *J, size_t n);
struct jsstr *js_mkstr(struct js *J, const char *s, int n);
struct jsstr *js_concat(struct js *J, struct jsstr *a, struct jsstr *b);
struct jsobj *js_mkobj(struct js *J, int cls, struct jsobj *proto);
jsval js_getv(struct js *J, jsval obj, jsval key);
void js_setv(struct js *J, jsval obj, jsval key, jsval v);
jsval js_get_str(struct js *J, jsval obj, struct jsstr *key);
void js_put(struct js *J, struct jsobj *o, struct jsstr *key, jsval v, int hidden);
struct prop *js_own(struct jsobj *o, struct jsstr *key);
int js_has(struct js *J, jsval obj, struct jsstr *key);
int js_delete(struct js *J, jsval obj, struct jsstr *key);
jsval js_call_value(struct js *J, jsval f, jsval self, jsval *args, int argc);
jsval js_construct(struct js *J, jsval f, jsval *args, int argc);
jsval js_throw_value(struct js *J, jsval v);
jsval js_make_error(struct js *J, const char *type, const char *msg);
int js_strict_equal(jsval a, jsval b);
int js_loose_equal(struct js *J, jsval a, jsval b);
jsval js_to_primitive(struct js *J, jsval v, int hint_string);
struct jsstr *js_tostr(struct js *J, jsval v);
int js_index(jsval key, uint32_t *out);           /* key is an array index */
void js_numtostr(double d, char *out);
double js_strtonum(const char *s, int n);           /* NaN if it is not a number */
double js_parse_float_prefix(const char *s, int n); /* parseFloat */
int js_array_set_len(struct js *J, struct jsobj *a, int len);
void js_define_native(struct js *J, struct jsobj *o, const char *name, js_native fn, int nargs);
int js_is_array(jsval v);
/* The keys for for-in and Object.keys, own and enumerable (inherited too if asked). */
int js_keys(struct js *J, jsval obj, int inherited, struct jsstr ***out);

/* Iteration (js.c): arrays, strings, array-likes and objects with a [Symbol.iterator]. */
enum { IT_ARRAY, IT_STR, IT_LIKE, IT_OBJ };

struct iter {
    int kind;
    struct jsobj *arr;
    int i;
    const char *sp, *send;
    jsval obj, next;
};

int js_iter_open(struct js *J, jsval v, struct iter *it);
int js_iter_next(struct js *J, struct iter *it, jsval *out);
void js_iter_close(struct js *J, struct iter *it);

void js_enqueue(struct js *J, void (*run)(struct js *, struct job *), jsval a, jsval b, jsval c, jsval d);
void js_run_jobs(struct js *J);

/* jslib.c */
void js_init_lib(struct js *J);
void js_init_lib2(struct js *J);          /* jslib2.c: Symbol, Promise, Reflect, ... */

/* Promises (jslib2.c). Settling and resolving are safe to call twice. */
jsval js_promise_new(struct js *J);
void js_promise_resolve(struct js *J, jsval po, jsval v);       /* follows thenables */
void js_promise_settle(struct js *J, jsval po, int state, jsval v);     /* state 1 fulfilled, 2 rejected */
jsval js_promise_then(struct js *J, jsval po, jsval ok, jsval err);
jsval js_promise_from(struct js *J, jsval v);                   /* Promise.resolve */
int js_is_promise(jsval v);
int js_promise_state(jsval po, jsval *value);                   /* -1 if not a promise */
/* Call on_ok(self, value) or on_err(self, reason) when it settles. */
void js_promise_watch(struct js *J, jsval po, js_native on_ok, js_native on_err, jsval self);
struct jsstr *js_json_stringify(struct js *J, jsval v, const char *indent);
jsval js_array_join(struct js *J, struct jsobj *a, const char *sep);

/* jsre.c: backtracking regular expressions over UTF-8 bytes */
#define RE_MAX_GROUPS 32
struct regex *re_compile(struct js *J, const char *pattern, int len, const char *flags,
                         const char **error);
/* Match at or after start; caps[2*i], caps[2*i+1] are group i's offsets (-1: unmatched). */
int re_exec(struct regex *re, const char *s, int len, int start, int caps[2 * RE_MAX_GROUPS]);
int re_ngroups(struct regex *re);
const char *re_group_name(struct regex *re, int g);     /* NULL if group g is not named */
int re_has_names(struct regex *re);
int re_global(struct regex *re);
int re_icase(struct regex *re);
const char *re_source(struct regex *re);
const char *re_flags(struct regex *re);

#endif
