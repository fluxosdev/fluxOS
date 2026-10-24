/*
 * The browser's JavaScript engine (js.c): a tree-walking interpreter for
 * a practical subset of ES5 and ES6, and its built-in objects. jsdom.c
 * binds it to the document.
 *
 * Everything a page's scripts allocate comes from one arena and is freed
 * with the page; there is no garbage collector, only a limit. A script
 * that runs too long, or allocates too much, is stopped with an error.
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
};

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
