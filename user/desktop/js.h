/*
 * The browser's JavaScript engine (js.c): a tree-walking interpreter for
 * a practical subset of ES5 and ES6, and its built-in objects. jsdom.c
 * binds it to the document.
 *
 * What a page's scripts allocate is garbage collected (jsgc.c), within a
 * limit. A script that runs too long, or keeps too much, is stopped with
 * an error.
 *
 * Numbers are doubles, so js.c and jsdom.c are compiled with the FPU on.
 */
#ifndef JS_H
#define JS_H

#include <stddef.h>
#include <stdint.h>

struct arena;
struct js;
struct jsobj;
struct jsstr;

enum { JS_UNDEF, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ };

typedef struct jsval {
    uint8_t t;
    union {
        double n;
        int b;
        struct jsstr *s;
        struct jsobj *o;
    } u;
} jsval;

typedef jsval (*js_native)(struct js *J, jsval self, jsval *args, int argc);

/* Objects that stand for something outside the engine (DOM nodes): property
 * reads and writes go to these first. get returns 1 if it knew the name. */
struct jsclass {
    const char *name;
    int (*get)(struct js *J, struct jsobj *o, const char *key, jsval *out);
    int (*set)(struct js *J, struct jsobj *o, const char *key, jsval v);
    /* A subclass's constructor called super() and self became what from was (from is dropped). */
    void (*adopt)(struct js *J, struct jsobj *self, struct jsobj *from);
};

/*
 * The garbage collector finds what is alive from the engine's own memory,
 * the stacks and the program's globals. Hosts register any other memory
 * that holds engine values; a host whose memory points into the heap
 * without keeping things alive (DOM nodes and their wrappers) hooks into
 * each collection: mark marks what must stay (js_gc_mark), more marks what
 * the marked things keep alive and says whether it marked anything, sweep
 * forgets what is not marked. Hooks must not allocate.
 */
struct js_gc_hooks {
    void *ctx;
    void (*mark)(struct js *J, void *ctx);
    int (*more)(struct js *J, void *ctx);
    void (*sweep)(struct js *J, void *ctx);
};
void js_gc_root(struct js *J, const void *p, size_t n);
void js_gc_unroot(struct js *J, const void *p);
void js_gc_hooks(struct js *J, const struct js_gc_hooks *hooks);
void js_gc_mark(struct js *J, const void *p);
int js_gc_marked(struct js *J, const void *p);
void js_gc_pause(struct js *J, int pause);          /* 1: no collections until a 0 */

struct js *js_new(void *host);
void js_free(struct js *J);
void *js_host(struct js *J);
/* Run source; returns 0, or -1 with the error in js_error(). */
int js_eval(struct js *J, const char *src, int len, const char *where);
const char *js_error(struct js *J);
/* Call a function value; returns 0, or -1 if it threw (the error in js_error()). */
int js_call(struct js *J, jsval fn, jsval self, jsval *args, int argc, jsval *result);
/* Compile a handler attribute ("onclick") into a function of (event). */
jsval js_compile_handler(struct js *J, const char *body, const char *where);

/*
 * ES modules. The host fetches each module's source and resolves
 * specifiers; the engine parses every module of a graph, links their
 * imports to the exports (live bindings) and runs them, dependencies
 * first, once the whole graph is here. import() works the same way.
 */
struct js_modhost {
    void *ctx;
    /* Fetch module id from url; answer with js_module_source(). */
    void (*fetch)(void *ctx, int id, const char *url);
    /* spec as imported by the module or script at base: an absolute URL, or -1. */
    int (*resolve)(void *ctx, const char *spec, const char *base, char *out, int size);
    /* A module script given to js_run_module has run, or failed (the error in js_error()). */
    void (*done)(void *ctx, void *tag, int failed);
};
void js_module_host(struct js *J, const struct js_modhost *h);
/* Run a module script: an inline one (url is the document's, for resolving)
 * or the module at url with the source fetched for it. tag goes to done(). */
void js_run_module(struct js *J, const char *url, int inline_src, const char *src, int len, void *tag);
/* What fetch() got for module id: its source, or NULL if it could not be fetched. */
void js_module_source(struct js *J, int id, const char *src, int len);

/* ---- values ---- */
extern const jsval js_undefined, js_null;
jsval js_num(double n);
jsval js_bool(int b);
jsval js_str(struct js *J, const char *s);
jsval js_strn(struct js *J, const char *s, int n);
jsval js_obj(struct jsobj *o);
const char *js_cstr(jsval v);                       /* a string value's bytes */
int js_strlen(jsval v);
/* Conversions: to_string copies into the engine's memory. */
jsval js_to_string(struct js *J, jsval v);
double js_to_number(struct js *J, jsval v);
int js_truthy(jsval v);
int js_is_callable(jsval v);
/* A value as C text, in a buffer of the engine's: for printing. */
const char *js_text(struct js *J, jsval v);

/* ---- objects ---- */
struct jsobj *js_new_object(struct js *J);
struct jsobj *js_new_array(struct js *J);
struct jsobj *js_new_host(struct js *J, const struct jsclass *cls, void *data);
void *js_host_data(struct jsobj *o, const struct jsclass *cls);    /* NULL if not of cls */
jsval js_new_function(struct js *J, js_native fn, const char *name, int nargs);
jsval js_get(struct js *J, jsval obj, const char *key);
void js_set(struct js *J, jsval obj, const char *key, jsval v);
void js_array_push(struct js *J, struct jsobj *arr, jsval v);
struct jsobj *js_global(struct js *J);
/* Throw from a native function: returns undefined, the error is pending. */
jsval js_throw_error(struct js *J, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int js_throwing(struct js *J);

#endif
