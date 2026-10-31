/*
 * The JavaScript engine's insides, shared by js.c (values, the parser and
 * compiler and VM), jslib.c (the built-in objects) and jsre.c (regular
 * expressions). Nothing outside the engine includes this.
 */
#ifndef JSINT_H
#define JSINT_H

#include "js.h"
#include "web.h"

/* Bytes a page's scripts may allocate: half the memory free when the page
 * opens, within these bounds. */
#define JS_HEAP_MIN   (48u << 20)
#define JS_HEAP_MAX   (512u << 20)
#define JS_STEP_LIMIT 40000000u         /* steps one entry into the engine may take (a second or so) */
#define JS_MAX_DEPTH  180               /* nested calls */
#define JS_STR_MAX    (1 << 28)         /* bytes in one string */
#define JS_STACK_SPARE (64u << 10)      /* stack left below the last check, for natives and the host */

struct jsstr {
    int len;                            /* bytes of UTF-8 */
    int utf16_length;                   /* cached code-unit length, zero until measured */
    uint32_t hash;                      /* of the UTF-16 code units, zero until needed */
    char s[];                           /* NUL-terminated as well */
};

/*
 * Symbols are property keys like strings, kept apart by their first byte,
 * 0xFF, which UTF-8 never has: "\xff" tag ["\xff" description]. The tag is
 * "w" for the well-known symbols, "r" for Symbol.for's and "u<n>" for the
 * rest. A JS_SYM value's u.s is that key; js_tostr gives the key back.
 */
#define SYMK(name) "\xff" "w" "\xff" "Symbol." name
#define SYM_ITERATOR       SYMK("iterator")
#define SYM_ASYNC_ITERATOR SYMK("asyncIterator")
#define SYM_TO_PRIMITIVE   SYMK("toPrimitive")
#define SYM_TO_STRING_TAG  SYMK("toStringTag")
#define SYM_HAS_INSTANCE   SYMK("hasInstance")

static inline int js_is_symkey(const struct jsstr *k)
{
    return k->len > 0 && (unsigned char)k->s[0] == 0xFF;
}

/* A property key as a value: a symbol or a string. */
static inline jsval js_keyval(struct jsstr *k)
{
    jsval v = { js_is_symkey(k) ? JS_SYM : JS_STR, { .s = k } };
    return v;
}

struct jsbig {
    int neg;
    int n;                              /* limbs in use; 0 for zero */
    uint32_t d[];                       /* 32 bits each, least significant first */
};

struct prop {
    struct jsstr *key;
    jsval v;
    struct jsobj *getter, *setter;      /* an accessor property */
    uint8_t hidden;                     /* not enumerable */
};

/*
 * A shape is the layout of an object's properties: their keys, in order. Objects built alike
 * (the same keys added in the same order to the same kind of object) share one, so the VM's
 * inline caches can tell where a key is from the shape alone. Shapes make a tree from a root
 * per cls (per host class for C_HOST): adding a key moves an object to a child. An object with
 * many properties, or that had one deleted, gets a shape of its own ("owned"), which grows in
 * place. While an object keeps its shape, none of its keys move.
 */
struct shape {
    struct shape *parent;
    struct jsstr *key;                  /* the last property's (NULL in a root) */
    uint32_t n;                         /* properties */
    uint8_t owned;
    struct shape **kids;                /* transitions: an open addressing table by key */
    uint32_t nkids, kidcap;
    uint32_t *tab;                      /* key -> index + 1 over the first tabn keys, past SHAPE_SCAN */
    uint32_t tabcap, tabn;
};

enum {
    C_OBJECT, C_ARRAY, C_FUNCTION, C_ERROR, C_DATE, C_REGEXP, C_HOST, C_BOOLEAN, C_NUMBER,
    C_STRING, C_ARGUMENTS,
};

struct ast;
struct coro;
struct vcompile;
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
    uint8_t is_proto;                   /* an inline cache relies on its keys and prototype */
    uint8_t size_hint;                  /* C_FUNCTION: properties `new` gave its objects */
    struct jsobj *proto;
    struct shape *shape;                /* the layout of props */
    struct prop *props;                 /* nprops of them, in the order they were added */
    uint32_t nprops, pcap;
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
    /* a Proxy: reads, writes and the rest go to the handler's traps */
    struct jsobj *ptarget, *phandler;
};

struct gcheap;

struct js {
    struct gcheap *heap;                /* jsgc.c */
    int gc_inhibit;                     /* no collections while this is above 0 (parsing) */
    int (*codegen_allowed)(struct js *J);
    int (*resource_allowed)(struct js *J,const char *directive,const char *url);
    void *host;
    void *network;                      /* jsnet: WebSocket worker state */
    void *workers;                      /* jsworker: the page's dedicated workers */
    /* A promise was rejected and nothing had asked for the result by the time the microtasks ran. */
    void (*on_unhandled)(struct js *J, jsval promise, jsval reason);
    struct jsobj *global;
    struct env *genv;
    /* prototypes */
    struct jsobj *object_proto, *function_proto, *array_proto, *string_proto, *number_proto,
                 *boolean_proto, *error_proto, *date_proto, *regexp_proto;
    /* a thrown value in flight */
    int throwing;
    jsval exc;
    jsval completion;                   /* the last expression statement's value (eval) */
    struct jsobj *new_target;            /* constructor currently entering a user frame */
    int constructing;                   /* a native called with new */
    unsigned steps, step_limit;
    size_t heap_limit;
    struct jsstr *small_ints[256];      /* "0" to "255", made once: common property keys */
    int depth;
    const char *stack_lo, *stack_hi;    /* the stack it runs on now: main, a coroutine's or the compiler's */
    int dead;                           /* out of memory or time: no more scripts */
    struct jsstr *s_home;               /* the hidden variable that holds a method's home object */
    struct jsstr *s_prototype;          /* "prototype", made once: every new and instanceof asks */
    struct job *jobs, *jobs_tail;       /* the microtask queue (promises) */
    int in_jobs;
    struct jsobj *promise_proto, *generator_proto, *agen_proto, *symbol_proto, *bigint_proto;
    struct jsobj *map_proto, *set_proto, *seqiter_proto, *buffer_proto;
    struct jsobj *typed_protos[11], *dataview_proto, *shared_proto;
    struct js_shared_mapping *shared_mappings;
    struct env *eval_env;
    struct jsstr *s_value, *s_done;     /* iterator results' keys */
    struct coro *cur_co, *co_live;      /* the running coroutine, all those with a stack */
    int nco;
    struct vcompile *compiler;          /* reusable compilation stack, separate from coroutine stacks */
    unsigned lazy_done;                 /* which lazily loaded library chunks have run (jslib2.c) */
    struct jsobj *return_marker;        /* thrown to unwind a generator that is being closed */
    /* ES modules (js.c) */
    struct js_modhost modhost;
    struct jsmod *mods;                 /* every module the page has asked for */
    struct jsentry *entries;            /* module scripts and import()s waiting for their graphs */
    int mod_ids, mod_busy, mod_again;
    unsigned mod_mark;
    struct jsstr *s_default, *s_meta;   /* the hidden bindings of export default and import.meta */
    struct jsobj *super_this;           /* while super() runs a native constructor: the new object */
    /* Shapes (js.c): the roots, and epochs that the VM's inline caches check. */
    struct shape *roots[C_ARGUMENTS + 1], *proxy_root, *prim_roots[JS_BIG + 1];
    struct js_host_root *host_roots;
    int nhost_roots;
    unsigned nshapes;
    uint32_t shape_epoch;               /* a prototype an inline cache relies on changed */
    uint32_t var_epoch;                 /* a name was declared in an environment's list */
    struct vmega *ic_mega;              /* jsvm.h: the inline caches' shared second level */
    char err[512];
    char textbuf[1024];
};

/* Is the stack the engine is on nearly used up? Recursion checks this and
 * gives up (a RangeError or SyntaxError) rather than run off its end. */
static inline int js_stack_low(struct js *J)
{
    const char *sp = __builtin_frame_address(0);
    return sp >= J->stack_lo && sp < J->stack_hi && (size_t)(sp - J->stack_lo) < JS_STACK_SPARE;
}

/* jsgc.c: the garbage-collected heap */
struct gcheap *gc_new(void);
void *gc_alloc(struct js *J, size_t n, int atomic);    /* zeroed; NULL if out of memory */
int js_running(void);                   /* inside a script now */
void gc_collect(struct js *J);
void gc_destroy(struct js *J);
size_t gc_heap_bytes(struct js *J);                     /* in use, garbage included */
/* js.c, for the collector: scan the live part of each stack the engine may be
 * running on (sp is where this one is now); -1 if sp is on none it knows. */
int js_gc_scan_stacks(struct js *J, const char *sp,
                      void (*scan)(struct js *J, const void *lo, const void *hi));

/* js.c */
void *js_alloc(struct js *J, size_t n);
void *js_alloc_atomic(struct js *J, size_t n);      /* for bytes with no pointers: strings */
struct jsstr *js_mkstr(struct js *J, const char *s, int n);
int js_u16len(const struct jsstr *s);
int js_u16unit(const struct jsstr *s, int index);
struct jsstr *js_u16slice(struct js *J, const struct jsstr *s, int start, int end);
struct jsstr *js_concat(struct js *J, struct jsstr *a, struct jsstr *b);
struct jsobj *js_mkobj(struct js *J, int cls, struct jsobj *proto);
/* The same, with room for n properties in the object's own block. */
struct jsobj *js_mkobj_room(struct js *J, int cls, struct jsobj *proto, uint32_t n);
jsval js_getv(struct js *J, jsval obj, jsval key);
void js_setv(struct js *J, jsval obj, jsval key, jsval v);
jsval js_get_str(struct js *J, jsval obj, struct jsstr *key);
void js_put(struct js *J, struct jsobj *o, struct jsstr *key, jsval v, int hidden);
struct prop *js_own(struct jsobj *o, struct jsstr *key);
int js_own_index(struct jsobj *o, struct jsstr *key);         /* -1 if o has no such property */
/* Add a property o does not have (with no value yet); next, if not NULL, is the shape it moves to. */
struct prop *js_prop_append(struct js *J, struct jsobj *o, struct jsstr *key, struct shape *next);
void js_set_proto(struct js *J, struct jsobj *o, struct jsobj *proto);
/* o's prototype or a property's accessors changed in a way inline caches must see. */
static inline void js_shape_changed(struct js *J, struct jsobj *o)
{
    if (o->is_proto)
        J->shape_epoch++;
}
uint32_t js_str_hash(struct jsstr *s);
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
struct jsstr *js_sym_desc(struct js *J, const struct jsstr *k);    /* a symbol key's description, or NULL */
struct jsstr *js_sym_text(struct js *J, const struct jsstr *k);    /* "Symbol(description)" */
void js_numtostr(double d, char *out);
double js_strtonum(const char *s, int n);           /* NaN if it is not a number */
double js_parse_float_prefix(const char *s, int n); /* parseFloat */
int js_array_set_len(struct js *J, struct jsobj *a, int len);
void js_define_native(struct js *J, struct jsobj *o, const char *name, js_native fn, int nargs);
int js_is_array(jsval v);
/* The keys for for-in and Object.keys, own and enumerable (inherited too if asked). */
int js_keys(struct js *J, jsval obj, int inherited, struct jsstr ***out);
int js_own_symbols(struct js *J, jsval obj, struct jsstr ***out);

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
void js_init_binary(struct js *J);
int js_typed_length(jsval v);
int js_typed_has(jsval v, const char *key);
int js_typed_get_index(struct js *J, struct jsobj *o, uint32_t i, jsval *out);
int js_typed_set_index(struct js *J, struct jsobj *o, uint32_t i, jsval v);
unsigned char *js_binary_bytes(jsval v, int *len);
jsval js_new_bytes(struct js *J, const unsigned char *p, int n);    /* a Uint8Array with a copy */
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

/* jsbig.c: BigInt arithmetic. Results are new values; NULL means an error was thrown. */
jsval js_bigv(struct jsbig *b);
struct jsbig *jb_from_u64(struct js *J, uint64_t v, int neg);
struct jsbig *jb_from_i64(struct js *J, int64_t v);
struct jsbig *jb_from_double(struct js *J, double d);      /* d an integer */
uint64_t jb_to_u64(const struct jsbig *b);                  /* modulo 2^64 */
double jb_to_double(const struct jsbig *b);
struct jsbig *jb_neg(struct js *J, const struct jsbig *a);
struct jsbig *jb_add(struct js *J, const struct jsbig *a, const struct jsbig *b);
struct jsbig *jb_sub(struct js *J, const struct jsbig *a, const struct jsbig *b);
struct jsbig *jb_mul(struct js *J, const struct jsbig *a, const struct jsbig *b);
struct jsbig *jb_div(struct js *J, const struct jsbig *a, const struct jsbig *b);
struct jsbig *jb_mod(struct js *J, const struct jsbig *a, const struct jsbig *b);
struct jsbig *jb_pow(struct js *J, const struct jsbig *a, const struct jsbig *e);
struct jsbig *jb_bitop(struct js *J, int op, const struct jsbig *a, const struct jsbig *b);   /* & | ^ */
struct jsbig *jb_not(struct js *J, const struct jsbig *a);
struct jsbig *jb_shl(struct js *J, const struct jsbig *a, int64_t k);   /* k < 0 shifts right */
struct jsbig *jb_as_n(struct js *J, uint32_t bits, const struct jsbig *a, int is_signed);
int jb_cmp(const struct jsbig *a, const struct jsbig *b);
int jb_cmp_double(struct js *J, const struct jsbig *a, double d);      /* 2: d is NaN */
struct jsbig *jb_parse(struct js *J, const char *s, int n, int radix, int sign);
struct jsstr *jb_tostr(struct js *J, const struct jsbig *a, int radix);

/* jsuni.c: Unicode. A uset is a list of code point ranges; uset_norm sorts and merges them. */
struct uset {
    uint32_t (*r)[2];
    int n, cap;
};
void uset_add(struct js *J, struct uset *u, uint32_t lo, uint32_t hi);
void uset_norm(struct uset *u);
void uset_invert(struct js *J, struct uset *u);
void uset_combine(struct js *J, struct uset *u, struct uset *v, int minus);  /* and, or minus */
int uset_has(const struct uset *u, uint32_t c);
int uc_add_property(struct js *J, struct uset *u, const char *spec, int len);  /* -1: unknown */
uint32_t uc_upper(uint32_t c);
uint32_t uc_lower(uint32_t c);
uint32_t uc_fold(uint32_t c);
uint32_t uc_orbit(uint32_t c);
int uc_is_space(uint32_t c);
struct jsstr *uc_change_case(struct js *J, struct jsstr *s, int upper);
struct jsstr *uc_normalize(struct js *J, struct jsstr *s, int compat, int compose);

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

void js_binary_free(struct js *J);

#endif
