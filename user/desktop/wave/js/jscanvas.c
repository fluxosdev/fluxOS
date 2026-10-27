/*
 * <canvas> for scripts: getContext("2d") and the CanvasRenderingContext2D
 * that draws with canvas.c, gradients, patterns, ImageData and
 * toDataURL. A canvas's pixels are its node's img, so layout and painting
 * treat it like a picture; this file allocates them (and keeps them until
 * the page goes). The context and everything it holds live in the script
 * engine's memory.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "canvas.h"
#include "jsdom.h"
#include "jsint.h"
#include "jscanvas.h"

#define ARG(i) ((i) < argc ? args[(i)] : js_undefined)
#define NUM(i) js_to_number(J, ARG(i))
#define S(J, s) js_mkstr(J, s, (int)strlen(s))
#define MAX_CANVAS_BYTES (96u << 20)
#define MAX_SIDE 16384

struct jcx {
    struct cv *cv;
    struct node *node;
    jsval canvas;
    char fill_str[48], stroke_str[48];
    char filter[40];
};

static const struct jsclass ctx_class, grad_class, pat_class;

/* ---- the engine's memory for canvas.c ---- */

static struct js *cur_J;

static void *gc_hook(size_t n, int atomic)
{
    return atomic ? js_alloc_atomic(cur_J, n) : js_alloc(cur_J, n);
}

/* ---- what a page's canvases occupy ---- */

struct reg {
    struct js *J;
    struct image **imgs;
    int n, cap;
    struct image **retired;
    int nret, retcap;
    size_t bytes;
    struct reg *next;
};
static struct reg *regs;

static struct reg *reg_of(struct js *J)
{
    for (struct reg *r = regs; r; r = r->next)
        if (r->J == J)
            return r;
    struct reg *r = calloc(1, sizeof(*r));
    if (r) {
        r->J = J;
        r->next = regs;
        regs = r;
    }
    return r;
}

static void reg_track(struct reg *r, struct image *img)
{
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 8;
        struct image **n = realloc(r->imgs, (size_t)cap * sizeof(*n));
        if (!n)
            return;
        r->imgs = n;
        r->cap = cap;
    }
    r->imgs[r->n++] = img;
}

static void img_release(struct image *img)
{
    if (img) {
        free(img->px);
        free(img);
    }
}

/* An image replaced while the layout may still point at it goes here until the next layout. */
static void reg_retire(struct reg *r, struct image *img)
{
    for (int i = 0; i < r->n; i++)
        if (r->imgs[i] == img) {
            r->imgs[i] = r->imgs[--r->n];
            break;
        }
    r->bytes -= (size_t)img->w * img->h * 4;
    if (r->nret == r->retcap) {
        int cap = r->retcap ? r->retcap * 2 : 4;
        struct image **n = realloc(r->retired, (size_t)cap * sizeof(*n));
        if (!n) {
            img_release(img);
            return;
        }
        r->retired = n;
        r->retcap = cap;
    }
    r->retired[r->nret++] = img;
}

void jscanvas_laid_out(struct js *J)
{
    struct reg *r = reg_of(J);
    if (!r)
        return;
    for (int i = 0; i < r->nret; i++)
        img_release(r->retired[i]);
    r->nret = 0;
}

void jscanvas_free(struct js *J)
{
    for (struct reg **pp = &regs; *pp; pp = &(*pp)->next)
        if ((*pp)->J == J) {
            struct reg *r = *pp;
            *pp = r->next;
            for (int i = 0; i < r->n; i++)
                img_release(r->imgs[i]);
            for (int i = 0; i < r->nret; i++)
                img_release(r->retired[i]);
            free(r->imgs);
            free(r->retired);
            free(r);
            return;
        }
}

static int attr_dim(struct node *n, const char *name, int def)
{
    const char *v = node_attr(n, name);
    if (!v)
        return def;
    while (*v == ' ')
        v++;
    if (*v < '0' || *v > '9')
        return def;
    long x = 0;
    while (*v >= '0' && *v <= '9' && x < 1000000)
        x = x * 10 + (*v++ - '0');
    return (int)x;
}

void jscanvas_size(struct node *n, int *w, int *h)
{
    *w = attr_dim(n, "width", 300);
    *h = attr_dim(n, "height", 150);
}

/* The canvas's bitmap, made if it is not there (and fits). NULL for a canvas with no area. */
static struct image *canvas_image(struct js *J, struct node *n, int create)
{
    int w, h;
    jscanvas_size(n, &w, &h);
    if (w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE)
        return NULL;
    if (n->img && n->img->w == w && n->img->h == h)
        return n->img;
    if (!create)
        return n->img;
    struct reg *r = reg_of(J);
    size_t bytes = (size_t)w * h * 4;
    if (!r || r->bytes + bytes > MAX_CANVAS_BYTES)
        return NULL;
    struct image *img = malloc(sizeof(*img));
    if (!img)
        return NULL;
    img->w = w;
    img->h = h;
    img->px = calloc((size_t)w * h, 4);
    if (!img->px) {
        free(img);
        return NULL;
    }
    if (n->img)
        reg_retire(r, n->img);
    reg_track(r, img);
    r->bytes += bytes;
    n->img = img;
    return img;
}

/* The width or height attribute was set: the bitmap starts again (and so does the context). */
void jscanvas_resized(struct js *J, struct node *n, jsval wrapper)
{
    if (!n->img)
        return;
    struct image *old = n->img;
    n->img = NULL;
    struct reg *r = reg_of(J);
    if (r) {
        for (int i = 0; i < r->n; i++)
            if (r->imgs[i] == old) {
                r->bytes -= (size_t)old->w * old->h * 4;
                break;
            }
        reg_retire(r, old);
        r->bytes += (size_t)old->w * old->h * 4;     /* undone by the retire above; it counts until released */
        r->bytes -= (size_t)old->w * old->h * 4;
    }
    canvas_image(J, n, 1);
    jsval ctx = wrapper.t == JS_OBJ ? js_get(J, wrapper, "%ctx") : js_undefined;
    struct jcx *cx = ctx.t == JS_OBJ ? js_host_data(ctx.u.o, &ctx_class) : NULL;
    if (cx) {
        cur_J = J;
        cv_reset(cx->cv);
    }
}

/* ---- errors ---- */

static jsval throw_dom(struct js *J, const char *name, const char *msg)
{
    jsval ctor = js_get(J, js_obj(js_global(J)), "DOMException");
    if (ctor.t == JS_OBJ) {
        jsval a[2] = {js_str(J, msg), js_str(J, name)};
        jsval e = js_construct(J, ctor, a, 2);
        return js_throw_value(J, e);
    }
    return js_throw_error(J, "Error: %s", msg);
}

#define NEED(n, what)                                                                                          \
    do {                                                                                                       \
        if (argc < (n))                                                                                        \
            return js_throw_error(J, "TypeError: Failed to execute '%s' on 'CanvasRenderingContext2D': %d "    \
                                     "arguments required, but only %d present.",                               \
                                  what, n, argc);                                                              \
    } while (0)

/* ---- the context ---- */

static struct jcx *cx_of(struct js *J, jsval self)
{
    struct jcx *cx = self.t == JS_OBJ ? js_host_data(self.u.o, &ctx_class) : NULL;
    if (!cx) {
        js_throw_error(J, "TypeError: Illegal invocation");
        return NULL;
    }
    cur_J = J;
    cx->cv->img = canvas_image(J, cx->node, 1);
    return cx;
}

#define CX()                                                                                                   \
    struct jcx *cx = cx_of(J, self);                                                                           \
    if (!cx)                                                                                                   \
        return js_undefined;                                                                                   \
    struct cv *c = cx->cv;                                                                                     \
    (void)c

static void touch(struct js *J)
{
    jsdom_canvas_dirty(J);
}

static int str_is(jsval v, const char *s)
{
    return v.t == JS_STR && !strcmp(js_cstr(v), s);
}

static jsval ctx_get_canvas(struct jcx *cx) { return cx->canvas; }

static const char *const cap_names[] = {"butt", "round", "square"};
static const char *const join_names[] = {"miter", "round", "bevel"};
static const char *const align_names[] = {"start", "end", "left", "right", "center"};
static const char *const base_names[] = {"alphabetic", "top", "hanging", "middle", "ideographic", "bottom"};

static int index_of(const char *const *names, int n, const char *s)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], s))
            return i;
    return -1;
}

static jsval style_value(struct js *J, struct jcx *cx, const struct cv_style *st, const char *cache)
{
    (void)cache;
    if (st->kind == 1 && st->grad && st->grad->host)
        return js_obj(st->grad->host);
    if (st->kind == 2 && st->pat && st->pat->host)
        return js_obj(st->pat->host);
    char buf[48];
    cv_color_string(st->color, buf, sizeof(buf));
    (void)cx;
    return js_str(J, buf);
}

static int ctx_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    struct jcx *cx = o->hdata;
    const struct cv_state *s = &cx->cv->st;
#define IS(x) (!strcmp(key, x))
    if (IS("canvas"))
        *out = ctx_get_canvas(cx);
    else if (IS("fillStyle"))
        *out = style_value(J, cx, &s->fill, cx->fill_str);
    else if (IS("strokeStyle"))
        *out = style_value(J, cx, &s->stroke, cx->stroke_str);
    else if (IS("lineWidth"))
        *out = js_num(s->line_width);
    else if (IS("lineCap"))
        *out = js_str(J, cap_names[s->cap]);
    else if (IS("lineJoin"))
        *out = js_str(J, join_names[s->join]);
    else if (IS("miterLimit"))
        *out = js_num(s->miter);
    else if (IS("globalAlpha"))
        *out = js_num(s->alpha);
    else if (IS("globalCompositeOperation"))
        *out = js_str(J, cv_op_name(s->op));
    else if (IS("shadowBlur"))
        *out = js_num(s->shadow_blur);
    else if (IS("shadowOffsetX"))
        *out = js_num(s->shadow_dx);
    else if (IS("shadowOffsetY"))
        *out = js_num(s->shadow_dy);
    else if (IS("shadowColor")) {
        char buf[48];
        cv_color_string(s->shadow_color, buf, sizeof(buf));
        *out = js_str(J, buf);
    } else if (IS("font"))
        *out = js_str(J, s->font);
    else if (IS("textAlign"))
        *out = js_str(J, align_names[s->align]);
    else if (IS("textBaseline"))
        *out = js_str(J, base_names[s->baseline]);
    else if (IS("direction"))
        *out = js_str(J, s->rtl ? "rtl" : "ltr");
    else if (IS("lineDashOffset"))
        *out = js_num(s->dash_off);
    else if (IS("imageSmoothingEnabled"))
        *out = js_bool(s->smooth);
    else if (IS("imageSmoothingQuality"))
        *out = js_str(J, "low");
    else if (IS("filter"))
        *out = js_str(J, cx->filter[0] ? cx->filter : "none");
    else if (IS("letterSpacing"))
        *out = js_str(J, "0px");
    else if (IS("wordSpacing"))
        *out = js_str(J, "0px");
    else if (IS("fontKerning"))
        *out = js_str(J, "auto");
    else if (IS("textRendering"))
        *out = js_str(J, "auto");
    else if (IS("fontStretch"))
        *out = js_str(J, "normal");
    else if (IS("fontVariantCaps"))
        *out = js_str(J, "normal");
    else
        return 0;
#undef IS
    return 1;
}

static void set_style(struct js *J, struct cv_style *st, jsval v)
{
    if (v.t == JS_OBJ) {
        struct cv_grad *g = js_host_data(v.u.o, &grad_class);
        struct cv_pat *p = g ? NULL : js_host_data(v.u.o, &pat_class);
        if (g) {
            st->kind = 1;
            st->grad = g;
        } else if (p) {
            st->kind = 2;
            st->pat = p;
        }
        if (g || p)
            return;
    }
    jsval sv = js_to_string(J, v);
    uint32_t argb;
    if (cv_parse_color(js_cstr(sv), &argb) == 0) {
        st->kind = 0;
        st->color = argb;
    }
}

static int ctx_set(struct js *J, struct jsobj *o, const char *key, jsval v)
{
    struct jcx *cx = o->hdata;
    struct cv_state *s = &cx->cv->st;
    cur_J = J;
#define IS(x) (!strcmp(key, x))
    if (IS("fillStyle"))
        set_style(J, &s->fill, v);
    else if (IS("strokeStyle"))
        set_style(J, &s->stroke, v);
    else if (IS("lineWidth")) {
        double d = js_to_number(J, v);
        if (d > 0 && !isinf(d) && d == d)
            s->line_width = (float)d;
    } else if (IS("lineCap")) {
        int i = index_of(cap_names, 3, js_cstr(js_to_string(J, v)));
        if (i >= 0)
            s->cap = i;
    } else if (IS("lineJoin")) {
        int i = index_of(join_names, 3, js_cstr(js_to_string(J, v)));
        if (i >= 0)
            s->join = i;
    } else if (IS("miterLimit")) {
        double d = js_to_number(J, v);
        if (d > 0 && !isinf(d) && d == d)
            s->miter = (float)d;
    } else if (IS("globalAlpha")) {
        double d = js_to_number(J, v);
        if (d >= 0 && d <= 1)
            s->alpha = (float)d;
    } else if (IS("globalCompositeOperation")) {
        int op = cv_parse_op(js_cstr(js_to_string(J, v)));
        if (op >= 0)
            s->op = op;
    } else if (IS("shadowBlur")) {
        double d = js_to_number(J, v);
        if (d >= 0 && !isinf(d) && d == d)
            s->shadow_blur = (float)d;
    } else if (IS("shadowOffsetX")) {
        double d = js_to_number(J, v);
        if (!isinf(d) && d == d)
            s->shadow_dx = (float)d;
    } else if (IS("shadowOffsetY")) {
        double d = js_to_number(J, v);
        if (!isinf(d) && d == d)
            s->shadow_dy = (float)d;
    } else if (IS("shadowColor")) {
        uint32_t argb;
        if (cv_parse_color(js_cstr(js_to_string(J, v)), &argb) == 0)
            s->shadow_color = argb;
    } else if (IS("font")) {
        cv_set_font(cx->cv, js_cstr(js_to_string(J, v)));
    } else if (IS("textAlign")) {
        int i = index_of(align_names, 5, js_cstr(js_to_string(J, v)));
        if (i >= 0)
            s->align = i;
    } else if (IS("textBaseline")) {
        int i = index_of(base_names, 6, js_cstr(js_to_string(J, v)));
        if (i >= 0)
            s->baseline = i;
    } else if (IS("direction")) {
        const char *d = js_cstr(js_to_string(J, v));
        if (!strcmp(d, "rtl"))
            s->rtl = 1;
        else if (!strcmp(d, "ltr") || !strcmp(d, "inherit"))
            s->rtl = 0;
    } else if (IS("lineDashOffset")) {
        double d = js_to_number(J, v);
        if (!isinf(d) && d == d)
            s->dash_off = (float)d;
    } else if (IS("imageSmoothingEnabled")) {
        s->smooth = js_truthy(v);
    } else if (IS("filter")) {
        snprintf(cx->filter, sizeof(cx->filter), "%s", js_cstr(js_to_string(J, v)));
    } else if (IS("canvas")) {
        /* read-only */
    } else if (IS("imageSmoothingQuality") || IS("letterSpacing") || IS("wordSpacing") || IS("fontKerning") ||
               IS("textRendering") || IS("fontStretch") || IS("fontVariantCaps")) {
        /* accepted, and ignored */
    } else {
        return 0;
    }
#undef IS
    return 1;
}

static const struct jsclass ctx_class = {"CanvasRenderingContext2D", ctx_get, ctx_set, NULL};

/* ---- gradients and patterns ---- */

static jsval grad_add_stop(struct js *J, jsval self, jsval *args, int argc)
{
    struct cv_grad *g = self.t == JS_OBJ ? js_host_data(self.u.o, &grad_class) : NULL;
    if (!g)
        return js_throw_error(J, "TypeError: Illegal invocation");
    if (argc < 2)
        return js_throw_error(J, "TypeError: Failed to execute 'addColorStop' on 'CanvasGradient': 2 arguments "
                                 "required, but only %d present.", argc);
    double off = NUM(0);
    if (!(off >= 0 && off <= 1))
        return throw_dom(J, "IndexSizeError", "The provided value is outside the range (0.0 - 1.0).");
    uint32_t argb;
    if (cv_parse_color(js_cstr(js_to_string(J, args[1])), &argb))
        return throw_dom(J, "SyntaxError", "The value provided could not be parsed as a color.");
    cur_J = J;
    cv_grad_stop(g, off, argb);
    return js_undefined;
}

static const struct jsclass grad_class = {"CanvasGradient", NULL, NULL, NULL};
static const struct jsclass pat_class = {"CanvasPattern", NULL, NULL, NULL};

static jsval proto_of(struct js *J, const char *name)
{
    return js_get(J, js_obj(js_global(J)), name);
}

static jsval new_gradient(struct js *J, int kind, double x0, double y0, double r0, double x1, double y1, double r1)
{
    cur_J = J;
    struct cv_grad *g = js_alloc(J, sizeof(*g));
    if (!g)
        return js_null;
    g->kind = kind;
    g->x0 = x0, g->y0 = y0, g->r0 = r0, g->x1 = x1, g->y1 = y1, g->r1 = r1;
    struct jsobj *o = js_new_host(J, &grad_class, g);
    jsval proto = js_get(J, proto_of(J, "CanvasGradient"), "prototype");
    if (proto.t == JS_OBJ)
        o->proto = proto.u.o;
    g->host = o;
    return js_obj(o);
}

static int finite_args(struct js *J, jsval *args, int argc, int n)
{
    for (int i = 0; i < n && i < argc; i++) {
        double d = js_to_number(J, args[i]);
        if (isnan(d) || isinf(d))
            return 0;
    }
    return 1;
}

static jsval ctx_create_linear(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(4, "createLinearGradient");
    if (!finite_args(J, args, argc, 4))
        return js_throw_error(J, "TypeError: The provided double value is non-finite.");
    return new_gradient(J, 0, NUM(0), NUM(1), 0, NUM(2), NUM(3), 0);
}

static jsval ctx_create_radial(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(6, "createRadialGradient");
    if (!finite_args(J, args, argc, 6))
        return js_throw_error(J, "TypeError: The provided double value is non-finite.");
    if (NUM(2) < 0 || NUM(5) < 0)
        return throw_dom(J, "IndexSizeError", "The radius provided is negative.");
    return new_gradient(J, 1, NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), NUM(5));
}

static jsval ctx_create_conic(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(3, "createConicGradient");
    if (!finite_args(J, args, argc, 3))
        return js_throw_error(J, "TypeError: The provided double value is non-finite.");
    return new_gradient(J, 2, NUM(1), NUM(2), NUM(0), 0, 0, 0);
}

/* ---- images: sources of drawImage and createPattern ---- */

struct node *jsdom_node_of(jsval v);

static const struct image *image_source(struct js *J, jsval v)
{
    struct node *n = jsdom_node_of(v);
    if (!n && v.t == JS_OBJ) {
        jsval inner = js_get(J, v, "_c");
        n = jsdom_node_of(inner);
    }
    if (!n || n->type != N_ELEMENT)
        return NULL;
    if (n->tag == T_CANVAS)
        canvas_image(J, n, 1);
    if (n->tag != T_IMG && n->tag != T_CANVAS && n->tag != T_SVG)
        return NULL;
    return n->img;
}

static int is_image_object(struct js *J, jsval v)
{
    if (v.t != JS_OBJ)
        return 0;
    struct node *n = jsdom_node_of(v);
    if (!n)
        n = jsdom_node_of(js_get(J, v, "_c"));
    return n && n->type == N_ELEMENT && (n->tag == T_IMG || n->tag == T_CANVAS || n->tag == T_SVG);
}

static jsval ctx_create_pattern(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(2, "createPattern");
    if (!is_image_object(J, args[0]))
        return js_throw_error(J, "TypeError: Failed to execute 'createPattern' on 'CanvasRenderingContext2D': The "
                                 "provided value is not of type '(CSSImageValue or HTMLImageElement or SVGImageElement "
                                 "or HTMLVideoElement or HTMLCanvasElement or ImageBitmap or OffscreenCanvas or "
                                 "VideoFrame)'.");
    const struct image *src = image_source(J, args[0]);
    if (!src || src->w <= 0 || src->h <= 0)
        return js_null;
    int repeat = CV_REPEAT;
    if (args[1].t != JS_NULL && args[1].t != JS_UNDEF) {
        const char *r = js_cstr(js_to_string(J, args[1]));
        if (!strcmp(r, "repeat") || !*r)
            repeat = CV_REPEAT;
        else if (!strcmp(r, "repeat-x"))
            repeat = CV_REPEAT_X;
        else if (!strcmp(r, "repeat-y"))
            repeat = CV_REPEAT_Y;
        else if (!strcmp(r, "no-repeat"))
            repeat = CV_NO_REPEAT;
        else
            return throw_dom(J, "SyntaxError", "The provided type ('') is not one of 'repeat', 'repeat-x', 'repeat-y', "
                                               "or 'no-repeat'.");
    }
    size_t px = (size_t)src->w * src->h * 4;
    struct cv_pat *p = js_alloc(J, sizeof(*p) + px);
    if (!p)
        return js_null;
    p->img.w = src->w;
    p->img.h = src->h;
    p->img.px = (uint32_t *)(p + 1);
    memcpy(p->img.px, src->px, px);
    p->repeat = repeat;
    struct jsobj *o = js_new_host(J, &pat_class, p);
    jsval proto = js_get(J, proto_of(J, "CanvasPattern"), "prototype");
    if (proto.t == JS_OBJ)
        o->proto = proto.u.o;
    p->host = o;
    return js_obj(o);
}

/* ---- state and transforms ---- */

static jsval ctx_save(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    cv_save(c);
    return js_undefined;
}

static jsval ctx_restore(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    cv_restore(c);
    return js_undefined;
}

static jsval ctx_reset(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    cv_reset(c);
    if (c->img) {
        memset(c->img->px, 0, (size_t)c->img->w * c->img->h * 4);
        touch(J);
    }
    return js_undefined;
}

static jsval ctx_scale(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(2, "scale");
    cv_scale(c, NUM(0), NUM(1));
    return js_undefined;
}

static jsval ctx_rotate(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(1, "rotate");
    cv_rotate(c, NUM(0));
    return js_undefined;
}

static jsval ctx_translate(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(2, "translate");
    cv_translate(c, NUM(0), NUM(1));
    return js_undefined;
}

static jsval ctx_transform(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(6, "transform");
    cv_transform(c, NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), NUM(5));
    return js_undefined;
}

static jsval ctx_set_transform(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    if (argc >= 1 && args[0].t == JS_OBJ) {      /* a DOMMatrix-like object */
        jsval m = args[0];
        cv_set_transform(c, js_to_number(J, js_get(J, m, "a")), js_to_number(J, js_get(J, m, "b")),
                         js_to_number(J, js_get(J, m, "c")), js_to_number(J, js_get(J, m, "d")),
                         js_to_number(J, js_get(J, m, "e")), js_to_number(J, js_get(J, m, "f")));
        return js_undefined;
    }
    NEED(6, "setTransform");
    cv_set_transform(c, NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), NUM(5));
    return js_undefined;
}

static jsval ctx_reset_transform(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    cv_set_transform(c, 1, 0, 0, 1, 0, 0);
    return js_undefined;
}

static jsval ctx_get_transform(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    const cv_mat *m = &c->st.m;
    jsval ctor = js_get(J, js_obj(js_global(J)), "DOMMatrix");
    jsval o;
    if (js_is_callable(ctor)) {
        jsval a[1];
        struct jsobj *arr = js_new_array(J);
        double v[6] = {m->a, m->b, m->c, m->d, m->e, m->f};
        for (int i = 0; i < 6; i++)
            js_array_push(J, arr, js_num(v[i]));
        a[0] = js_obj(arr);
        o = js_construct(J, ctor, a, 1);
    } else {
        o = js_obj(js_new_object(J));
        js_set(J, o, "a", js_num(m->a));
        js_set(J, o, "b", js_num(m->b));
        js_set(J, o, "c", js_num(m->c));
        js_set(J, o, "d", js_num(m->d));
        js_set(J, o, "e", js_num(m->e));
        js_set(J, o, "f", js_num(m->f));
        js_set(J, o, "m11", js_num(m->a));
        js_set(J, o, "m12", js_num(m->b));
        js_set(J, o, "m21", js_num(m->c));
        js_set(J, o, "m22", js_num(m->d));
        js_set(J, o, "m41", js_num(m->e));
        js_set(J, o, "m42", js_num(m->f));
        js_set(J, o, "is2D", js_bool(1));
    }
    return o;
}

/* ---- path building ---- */

static jsval ctx_begin_path(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    cv_path_clear(&c->path);
    return js_undefined;
}

static jsval ctx_close_path(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    cv_path_close(&c->path);
    return js_undefined;
}

static jsval ctx_move_to(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(2, "moveTo");
    cv_path_move(&c->path, &c->st.m, NUM(0), NUM(1));
    return js_undefined;
}

static jsval ctx_line_to(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(2, "lineTo");
    cv_path_line(&c->path, &c->st.m, NUM(0), NUM(1));
    return js_undefined;
}

static jsval ctx_quad_to(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(4, "quadraticCurveTo");
    cv_path_quad(&c->path, &c->st.m, NUM(0), NUM(1), NUM(2), NUM(3));
    return js_undefined;
}

static jsval ctx_bezier_to(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(6, "bezierCurveTo");
    cv_path_bezier(&c->path, &c->st.m, NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), NUM(5));
    return js_undefined;
}

static jsval ctx_arc_to(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(5, "arcTo");
    if (NUM(4) < 0)
        return throw_dom(J, "IndexSizeError", "The radius provided is negative.");
    cv_path_arcto(&c->path, &c->st.m, NUM(0), NUM(1), NUM(2), NUM(3), NUM(4));
    return js_undefined;
}

static jsval ctx_arc(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(5, "arc");
    if (NUM(2) < 0)
        return throw_dom(J, "IndexSizeError", "The radius provided is negative.");
    cv_path_arc(&c->path, &c->st.m, NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), argc > 5 && js_truthy(args[5]));
    return js_undefined;
}

static jsval ctx_ellipse(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(7, "ellipse");
    if (NUM(2) < 0 || NUM(3) < 0)
        return throw_dom(J, "IndexSizeError", "The radius provided is negative.");
    cv_path_ellipse(&c->path, &c->st.m, NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), NUM(5), NUM(6),
                    argc > 7 && js_truthy(args[7]));
    return js_undefined;
}

static jsval ctx_rect(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(4, "rect");
    cv_path_rect(&c->path, &c->st.m, NUM(0), NUM(1), NUM(2), NUM(3));
    return js_undefined;
}

/* Corner radii: a number, a DOMPoint-like {x, y} or a list of up to four of them. */
static void parse_radii(struct js *J, jsval v, double *r)
{
    r[0] = r[1] = r[2] = r[3] = 0;
    if (v.t == JS_UNDEF)
        return;
    if (v.t == JS_OBJ && js_is_array(v)) {
        struct jsobj *a = v.u.o;
        double q[4];
        int n = a->len < 4 ? a->len : 4;
        for (int i = 0; i < n; i++) {
            jsval e = a->items[i];
            q[i] = e.t == JS_OBJ ? js_to_number(J, js_get(J, e, "x")) : js_to_number(J, e);
        }
        if (n == 1)
            r[0] = r[1] = r[2] = r[3] = q[0];
        else if (n == 2) {
            r[0] = r[2] = q[0];
            r[1] = r[3] = q[1];
        } else if (n == 3) {
            r[0] = q[0];
            r[1] = r[3] = q[1];
            r[2] = q[2];
        } else if (n == 4) {
            memcpy(r, q, sizeof(q));
        }
        return;
    }
    double d = v.t == JS_OBJ ? js_to_number(J, js_get(J, v, "x")) : js_to_number(J, v);
    r[0] = r[1] = r[2] = r[3] = d;
}

static jsval ctx_round_rect(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(4, "roundRect");
    double r[4];
    parse_radii(J, ARG(4), r);
    for (int i = 0; i < 4; i++)
        if (r[i] < 0 || isnan(r[i]))
            return js_throw_error(J, "RangeError: Failed to execute 'roundRect' on 'CanvasRenderingContext2D': Radii "
                                     "cannot be negative.");
    cv_path_round_rect(&c->path, &c->st.m, NUM(0), NUM(1), NUM(2), NUM(3), r);
    return js_undefined;
}

/* ---- Path2D: its ops, and SVG path data, replayed onto a path ---- */

static const char *svg_num(const char *p, double *out)
{
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    const char *s = p;
    if (*p == '+' || *p == '-')
        p++;
    int digits = 0;
    while (*p >= '0' && *p <= '9')
        p++, digits++;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9')
            p++, digits++;
    }
    if (!digits)
        return NULL;
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        if (*q == '+' || *q == '-')
            q++;
        if (*q >= '0' && *q <= '9') {
            while (*q >= '0' && *q <= '9')
                q++;
            p = q;
        }
    }
    *out = js_strtonum(s, (int)(p - s));
    return p;
}

static const char *svg_flag(const char *p, int *out)
{
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n')
        p++;
    if (*p != '0' && *p != '1')
        return NULL;
    *out = *p - '0';
    return p + 1;
}

/* An elliptical arc from (x0, y0) to (x, y) in the SVG way. */
static void svg_arc(struct cv_path *p, const cv_mat *m, double x0, double y0, double rx, double ry, double rot, int large,
                    int sweep, double x, double y)
{
    if (rx == 0 || ry == 0 || (x0 == x && y0 == y)) {
        cv_path_line(p, m, x, y);
        return;
    }
    rx = fabs(rx), ry = fabs(ry);
    double phi = rot * M_PI / 180, cp = cos(phi), sp = sin(phi);
    double dx = (x0 - x) / 2, dy = (y0 - y) / 2;
    double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    double lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lam > 1) {
        double s = sqrt(lam);
        rx *= s, ry *= s;
    }
    double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double co = den == 0 ? 0 : sqrt(num / den < 0 ? 0 : num / den);
    if (large == sweep)
        co = -co;
    double cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
    double cx = cp * cxp - sp * cyp + (x0 + x) / 2, cy = sp * cxp + cp * cyp + (y0 + y) / 2;
    double a0 = atan2((y1 - cyp) / ry, (x1 - cxp) / rx);
    double a1 = atan2((-y1 - cyp) / ry, (-x1 - cxp) / rx);
    double delta = a1 - a0;
    if (!sweep && delta > 0)
        delta -= 2 * M_PI;
    else if (sweep && delta < 0)
        delta += 2 * M_PI;
    cv_path_ellipse(p, m, cx, cy, rx, ry, phi, a0, a0 + delta, delta < 0);
}

static void svg_path(struct cv_path *p, const cv_mat *m, const char *d)
{
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;
    char cmd = 0, prev = 0;
    while (*d) {
        while (*d == ' ' || *d == ',' || *d == '\t' || *d == '\n' || *d == '\r')
            d++;
        if (!*d)
            break;
        if ((*d >= 'a' && *d <= 'z') || (*d >= 'A' && *d <= 'Z')) {
            cmd = *d++;
        } else if (!cmd) {
            return;
        } else if (cmd == 'M') {
            cmd = 'L';
        } else if (cmd == 'm') {
            cmd = 'l';
        }
        int rel = cmd >= 'a';
        char up = (char)(rel ? cmd - 32 : cmd);
        double v[7];
        int need = up == 'M' || up == 'L' || up == 'T' ? 2 : up == 'H' || up == 'V' ? 1 : up == 'C' ? 6 :
                   up == 'S' || up == 'Q' ? 4 : up == 'A' ? 7 : 0;
        if (up == 'Z') {
            cv_path_close(p);
            cx = sx, cy = sy;
            prev = 'Z';
            cmd = 0;
            continue;
        }
        if (!need)
            return;
        for (int i = 0; i < need; i++) {
            if (up == 'A' && (i == 3 || i == 4)) {
                int f;
                d = svg_flag(d, &f);
                if (!d)
                    return;
                v[i] = f;
            } else {
                d = svg_num(d, &v[i]);
                if (!d)
                    return;
            }
        }
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (up) {
        case 'M':
            cx = v[0] + ox, cy = v[1] + oy;
            sx = cx, sy = cy;
            cv_path_move(p, m, cx, cy);
            break;
        case 'L':
            cx = v[0] + ox, cy = v[1] + oy;
            cv_path_line(p, m, cx, cy);
            break;
        case 'H':
            cx = v[0] + ox;
            cv_path_line(p, m, cx, cy);
            break;
        case 'V':
            cy = v[0] + oy;
            cv_path_line(p, m, cx, cy);
            break;
        case 'C':
            cv_path_bezier(p, m, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy, v[4] + ox, v[5] + oy);
            lcx = v[2] + ox, lcy = v[3] + oy;
            cx = v[4] + ox, cy = v[5] + oy;
            break;
        case 'S': {
            double x1 = prev == 'C' || prev == 'S' ? 2 * cx - lcx : cx, y1 = prev == 'C' || prev == 'S' ? 2 * cy - lcy : cy;
            cv_path_bezier(p, m, x1, y1, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy);
            lcx = v[0] + ox, lcy = v[1] + oy;
            cx = v[2] + ox, cy = v[3] + oy;
            break;
        }
        case 'Q':
            cv_path_quad(p, m, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy);
            lcx = v[0] + ox, lcy = v[1] + oy;
            cx = v[2] + ox, cy = v[3] + oy;
            break;
        case 'T': {
            double x1 = prev == 'Q' || prev == 'T' ? 2 * cx - lcx : cx, y1 = prev == 'Q' || prev == 'T' ? 2 * cy - lcy : cy;
            cv_path_quad(p, m, x1, y1, v[0] + ox, v[1] + oy);
            lcx = x1, lcy = y1;
            cx = v[0] + ox, cy = v[1] + oy;
            break;
        }
        case 'A':
            svg_arc(p, m, cx, cy, v[0], v[1], v[2], (int)v[3], (int)v[4], v[5] + ox, v[6] + oy);
            cx = v[5] + ox, cy = v[6] + oy;
            break;
        }
        prev = up;
    }
}

static double op_num(struct js *J, struct jsobj *op, int i)
{
    return i < op->len ? js_to_number(J, op->items[i]) : 0;
}

/* Replay a Path2D's ops through m onto p. */
static void replay_path2d(struct js *J, jsval path, struct cv_path *p, const cv_mat *m)
{
    jsval ops = js_get(J, path, "ops");
    if (ops.t != JS_OBJ || !js_is_array(ops))
        return;
    for (int k = 0; k < ops.u.o->len; k++) {
        jsval ov = ops.u.o->items[k];
        if (ov.t != JS_OBJ || !js_is_array(ov) || ov.u.o->len < 1 || ov.u.o->items[0].t != JS_STR)
            continue;
        struct jsobj *op = ov.u.o;
        const char *name = js_cstr(op->items[0]);
        double a = op_num(J, op, 1), b = op_num(J, op, 2), cc = op_num(J, op, 3), d = op_num(J, op, 4),
               e = op_num(J, op, 5), f = op_num(J, op, 6);
        if (!strcmp(name, "move"))
            cv_path_move(p, m, a, b);
        else if (!strcmp(name, "line"))
            cv_path_line(p, m, a, b);
        else if (!strcmp(name, "close"))
            cv_path_close(p);
        else if (!strcmp(name, "bezier"))
            cv_path_bezier(p, m, a, b, cc, d, e, f);
        else if (!strcmp(name, "quad"))
            cv_path_quad(p, m, a, b, cc, d);
        else if (!strcmp(name, "arc"))
            cv_path_arc(p, m, a, b, cc, d, e, op->len > 6 && js_truthy(op->items[6]));
        else if (!strcmp(name, "arcTo"))
            cv_path_arcto(p, m, a, b, cc, d, e);
        else if (!strcmp(name, "ellipse"))
            cv_path_ellipse(p, m, a, b, cc, d, e, f, op_num(J, op, 7), op->len > 8 && js_truthy(op->items[8]));
        else if (!strcmp(name, "rect"))
            cv_path_rect(p, m, a, b, cc, d);
        else if (!strcmp(name, "roundRect")) {
            double r[4];
            parse_radii(J, op->len > 5 ? op->items[5] : js_undefined, r);
            cv_path_round_rect(p, m, a, b, cc, d, r);
        } else if (!strcmp(name, "svg") && op->len > 1 && op->items[1].t == JS_STR)
            svg_path(p, m, js_cstr(op->items[1]));
    }
}

static int is_path2d(struct js *J, jsval v)
{
    if (v.t != JS_OBJ)
        return 0;
    jsval ctor = js_get(J, js_obj(js_global(J)), "Path2D");
    if (ctor.t != JS_OBJ)
        return 0;
    jsval proto = js_get(J, ctor, "prototype");
    for (struct jsobj *o = v.u.o->proto; o; o = o->proto)
        if (proto.t == JS_OBJ && o == proto.u.o)
            return 1;
    return 0;
}

/* The path to draw: the current one, or a Path2D replayed under the current transform. */
static struct cv_path *path_arg(struct js *J, struct cv *c, jsval v, struct cv_path *tmp)
{
    if (is_path2d(J, v)) {
        memset(tmp, 0, sizeof(*tmp));
        replay_path2d(J, v, tmp, &c->st.m);
        return tmp;
    }
    return &c->path;
}

static int fill_rule(jsval v) { return str_is(v, "evenodd"); }

static jsval ctx_fill(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    struct cv_path tmp;
    int p = argc > 0 && is_path2d(J, args[0]);
    cv_fill(c, path_arg(J, c, p ? args[0] : js_undefined, &tmp), fill_rule(ARG(p ? 1 : 0)));
    touch(J);
    return js_undefined;
}

static jsval ctx_stroke(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    struct cv_path tmp;
    cv_stroke(c, path_arg(J, c, ARG(0), &tmp));
    touch(J);
    return js_undefined;
}

static jsval ctx_clip(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    struct cv_path tmp;
    int p = argc > 0 && is_path2d(J, args[0]);
    cv_clip(c, path_arg(J, c, p ? args[0] : js_undefined, &tmp), fill_rule(ARG(p ? 1 : 0)));
    return js_undefined;
}

static jsval ctx_in_path(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    struct cv_path tmp;
    int p = argc > 0 && is_path2d(J, args[0]);
    if (argc < (p ? 3 : 2))
        return js_throw_error(J, "TypeError: Failed to execute 'isPointInPath' on 'CanvasRenderingContext2D': 2 "
                                 "arguments required, but only %d present.", argc);
    return js_bool(cv_point_in_path(c, path_arg(J, c, p ? args[0] : js_undefined, &tmp), NUM(p ? 1 : 0), NUM(p ? 2 : 1),
                                    fill_rule(ARG(p ? 3 : 2))));
}

static jsval ctx_in_stroke(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    struct cv_path tmp;
    int p = argc > 0 && is_path2d(J, args[0]);
    if (argc < (p ? 3 : 2))
        return js_throw_error(J, "TypeError: Failed to execute 'isPointInStroke' on 'CanvasRenderingContext2D': 2 "
                                 "arguments required, but only %d present.", argc);
    return js_bool(cv_point_in_stroke(c, path_arg(J, c, p ? args[0] : js_undefined, &tmp), NUM(p ? 1 : 0),
                                      NUM(p ? 2 : 1)));
}

/* ---- rectangles ---- */

static jsval ctx_fill_rect(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(4, "fillRect");
    cv_fill_rect(c, NUM(0), NUM(1), NUM(2), NUM(3));
    touch(J);
    return js_undefined;
}

static jsval ctx_stroke_rect(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(4, "strokeRect");
    cv_stroke_rect(c, NUM(0), NUM(1), NUM(2), NUM(3));
    touch(J);
    return js_undefined;
}

static jsval ctx_clear_rect(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(4, "clearRect");
    cv_clear_rect(c, NUM(0), NUM(1), NUM(2), NUM(3));
    touch(J);
    return js_undefined;
}

/* ---- line dashes ---- */

static jsval ctx_set_line_dash(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(1, "setLineDash");
    if (args[0].t != JS_OBJ || !js_is_array(args[0]))
        return js_undefined;
    struct jsobj *a = args[0].u.o;
    int n = a->len;
    if (n > 256)
        return js_undefined;
    float *d = js_alloc_atomic(J, sizeof(float) * (size_t)(n ? 2 * n : 1));
    if (!d)
        return js_undefined;
    for (int i = 0; i < n; i++) {
        double v = js_to_number(J, a->items[i]);
        if (!(v >= 0) || isinf(v))
            return js_undefined;
        d[i] = (float)v;
    }
    if (n % 2) {
        for (int i = 0; i < n; i++)
            d[n + i] = d[i];
        n *= 2;
    }
    c->st.dash = d;
    c->st.ndash = n;
    return js_undefined;
}

static jsval ctx_get_line_dash(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    struct jsobj *a = js_new_array(J);
    for (int i = 0; i < c->st.ndash; i++)
        js_array_push(J, a, js_num(c->st.dash[i]));
    return js_obj(a);
}

/* ---- text ---- */

static jsval draw_text(struct js *J, jsval self, jsval *args, int argc, int stroke, const char *what)
{
    CX();
    NEED(3, what);
    jsval sv = js_to_string(J, args[0]);
    double mw = -1;
    if (argc > 3 && ARG(3).t != JS_UNDEF) {
        mw = NUM(3);
        if (mw != mw)
            return js_undefined;
    }
    cv_text(c, js_cstr(sv), js_strlen(sv), NUM(1), NUM(2), mw, stroke);
    touch(J);
    return js_undefined;
}

static jsval ctx_fill_text(struct js *J, jsval self, jsval *args, int argc)
{
    return draw_text(J, self, args, argc, 0, "fillText");
}

static jsval ctx_stroke_text(struct js *J, jsval self, jsval *args, int argc)
{
    return draw_text(J, self, args, argc, 1, "strokeText");
}

static jsval ctx_measure_text(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(1, "measureText");
    jsval sv = js_to_string(J, args[0]);
    double asc, desc, w = cv_measure(c, js_cstr(sv), js_strlen(sv), &asc, &desc);
    jsval o = js_obj(js_new_object(J));
    double left = 0;
    if (c->st.align == CV_ALIGN_RIGHT || c->st.align == CV_ALIGN_END)
        left = w;
    else if (c->st.align == CV_ALIGN_CENTER)
        left = w / 2;
    js_set(J, o, "width", js_num(w));
    js_set(J, o, "actualBoundingBoxLeft", js_num(left));
    js_set(J, o, "actualBoundingBoxRight", js_num(w - left));
    js_set(J, o, "fontBoundingBoxAscent", js_num(asc));
    js_set(J, o, "fontBoundingBoxDescent", js_num(desc));
    js_set(J, o, "actualBoundingBoxAscent", js_num(asc * 0.9));
    js_set(J, o, "actualBoundingBoxDescent", js_num(desc * 0.6));
    js_set(J, o, "emHeightAscent", js_num(asc));
    js_set(J, o, "emHeightDescent", js_num(desc));
    js_set(J, o, "hangingBaseline", js_num(asc * 0.8));
    js_set(J, o, "alphabeticBaseline", js_num(0));
    js_set(J, o, "ideographicBaseline", js_num(-desc));
    return o;
}

/* ---- drawImage ---- */

static jsval ctx_draw_image(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(3, "drawImage");
    if (!is_image_object(J, args[0]))
        return js_throw_error(J, "TypeError: Failed to execute 'drawImage' on 'CanvasRenderingContext2D': The provided "
                                 "value is not of type '(CSSImageValue or HTMLImageElement or SVGImageElement or "
                                 "HTMLVideoElement or HTMLCanvasElement or ImageBitmap or OffscreenCanvas or "
                                 "VideoFrame)'.");
    const struct image *src = image_source(J, args[0]);
    if (!src || src->w <= 0 || src->h <= 0) {
        struct node *n = jsdom_node_of(args[0]);
        if (n && n->tag == T_CANVAS)
            return throw_dom(J, "InvalidStateError", "The image argument is a canvas element with a width or height of 0.");
        return js_undefined;
    }
    if (src == c->img) {                 /* drawing a canvas on itself: from a copy */
        struct image *copy = malloc(sizeof(*copy));
        if (!copy)
            return js_undefined;
        copy->w = src->w;
        copy->h = src->h;
        copy->px = malloc((size_t)src->w * src->h * 4);
        if (!copy->px) {
            free(copy);
            return js_undefined;
        }
        memcpy(copy->px, src->px, (size_t)src->w * src->h * 4);
        if (argc >= 9)
            cv_draw_image(c, copy, NUM(1), NUM(2), NUM(3), NUM(4), NUM(5), NUM(6), NUM(7), NUM(8));
        else if (argc >= 5)
            cv_draw_image(c, copy, 0, 0, copy->w, copy->h, NUM(1), NUM(2), NUM(3), NUM(4));
        else
            cv_draw_image(c, copy, 0, 0, copy->w, copy->h, NUM(1), NUM(2), copy->w, copy->h);
        img_release(copy);
        touch(J);
        return js_undefined;
    }
    if (argc >= 9)
        cv_draw_image(c, src, NUM(1), NUM(2), NUM(3), NUM(4), NUM(5), NUM(6), NUM(7), NUM(8));
    else if (argc >= 5)
        cv_draw_image(c, src, 0, 0, src->w, src->h, NUM(1), NUM(2), NUM(3), NUM(4));
    else
        cv_draw_image(c, src, 0, 0, src->w, src->h, NUM(1), NUM(2), src->w, src->h);
    touch(J);
    return js_undefined;
}

/* ---- ImageData ---- */

static jsval new_image_data(struct js *J, int w, int h)
{
    jsval ctor = js_get(J, js_obj(js_global(J)), "ImageData");
    jsval a[2] = {js_num(w), js_num(h)};
    return js_construct(J, ctor, a, 2);
}

static jsval ctx_get_image_data(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(4, "getImageData");
    double dx = NUM(0), dy = NUM(1), dw = NUM(2), dh = NUM(3);
    if (isnan(dx) || isnan(dy) || isnan(dw) || isnan(dh) || isinf(dx) || isinf(dy) || isinf(dw) || isinf(dh))
        return js_throw_error(J, "TypeError: The provided double value is non-finite.");
    if (dw == 0 || dh == 0)
        return throw_dom(J, "IndexSizeError", "The source width is 0.");
    if (dw < 0)
        dx += dw, dw = -dw;
    if (dh < 0)
        dy += dh, dh = -dh;
    if (dw > 16384 || dh > 16384 || dw * dh > 32.0 * 1024 * 1024)
        return js_throw_error(J, "RangeError: Out of memory");
    int w = (int)dw, h = (int)dh;
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (!c->img)
        return throw_dom(J, "InvalidStateError", "The canvas has no area.");
    jsval id = new_image_data(J, w, h);
    if (js_throwing(J))
        return js_undefined;
    int len;
    unsigned char *bytes = js_binary_bytes(js_get(J, id, "data"), &len);
    if (bytes && len >= w * h * 4)
        cv_get_image_data(c, (int)floor(dx), (int)floor(dy), w, h, bytes);
    return id;
}

static jsval ctx_put_image_data(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(3, "putImageData");
    if (args[0].t != JS_OBJ)
        return js_throw_error(J, "TypeError: Failed to execute 'putImageData' on 'CanvasRenderingContext2D': parameter "
                                 "1 is not of type 'ImageData'.");
    int w = (int)js_to_number(J, js_get(J, args[0], "width")), h = (int)js_to_number(J, js_get(J, args[0], "height"));
    int len = 0;
    unsigned char *bytes = js_binary_bytes(js_get(J, args[0], "data"), &len);
    if (!bytes || w <= 0 || h <= 0 || len < w * h * 4)
        return js_throw_error(J, "TypeError: Failed to execute 'putImageData' on 'CanvasRenderingContext2D': parameter "
                                 "1 is not of type 'ImageData'.");
    double x = NUM(1), y = NUM(2);
    if (isnan(x) || isnan(y) || isinf(x) || isinf(y))
        return js_throw_error(J, "TypeError: The provided double value is non-finite.");
    int dx = 0, dy = 0, dw = w, dh = h;
    if (argc >= 7) {
        double rx = NUM(3), ry = NUM(4), rw = NUM(5), rh = NUM(6);
        if (rw < 0)
            rx += rw, rw = -rw;
        if (rh < 0)
            ry += rh, rh = -rh;
        int x0 = (int)floor(rx), y0 = (int)floor(ry), x1 = (int)floor(rx + rw), y1 = (int)floor(ry + rh);
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 > w)
            x1 = w;
        if (y1 > h)
            y1 = h;
        dx = x0, dy = y0, dw = x1 - x0, dh = y1 - y0;
    }
    if (dw > 0 && dh > 0)
        cv_put_image_data(c, (int)floor(x), (int)floor(y), w, h, bytes, dx, dy, dw, dh);
    touch(J);
    return js_undefined;
}

static jsval ctx_create_image_data(struct js *J, jsval self, jsval *args, int argc)
{
    CX();
    NEED(1, "createImageData");
    int w, h;
    if (args[0].t == JS_OBJ) {
        w = (int)js_to_number(J, js_get(J, args[0], "width"));
        h = (int)js_to_number(J, js_get(J, args[0], "height"));
    } else {
        NEED(2, "createImageData");
        double dw = NUM(0), dh = NUM(1);
        if (dw == 0 || dh == 0)
            return throw_dom(J, "IndexSizeError", "The source width is 0.");
        w = (int)fabs(dw);
        h = (int)fabs(dh);
    }
    if (w < 1 || h < 1 || (double)w * h > 32.0 * 1024 * 1024)
        return throw_dom(J, "IndexSizeError", "The source width is 0.");
    return new_image_data(J, w, h);
}

static jsval ctx_get_attributes(struct js *J, jsval self, jsval *args, int argc)
{
    jsval o = js_obj(js_new_object(J));
    js_set(J, o, "alpha", js_bool(1));
    js_set(J, o, "colorSpace", js_str(J, "srgb"));
    js_set(J, o, "desynchronized", js_bool(0));
    js_set(J, o, "willReadFrequently", js_bool(0));
    return o;
}

static jsval ctx_false(struct js *J, jsval self, jsval *args, int argc) { return js_bool(0); }
static jsval ctx_nothing(struct js *J, jsval self, jsval *args, int argc) { return js_undefined; }

/* ---- the canvas element ---- */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static jsval canvas_get_context(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = jsdom_node_of(self);
    if (!n)
        return js_throw_error(J, "TypeError: Illegal invocation");
    const char *type = argc > 0 ? js_cstr(js_to_string(J, args[0])) : "";
    if (strcmp(type, "2d"))
        return js_null;
    jsval had = js_get(J, self, "%ctx");
    if (had.t == JS_OBJ)
        return had;
    cur_J = J;
    canvas_image(J, n, 1);
    struct jcx *cx = js_alloc(J, sizeof(*cx));
    if (!cx)
        return js_null;
    cx->cv = cv_new();
    cx->node = n;
    cx->canvas = self;
    if (!cx->cv)
        return js_null;
    struct jsobj *o = js_new_host(J, &ctx_class, cx);
    jsval proto = js_get(J, proto_of(J, "CanvasRenderingContext2D"), "prototype");
    if (proto.t == JS_OBJ)
        o->proto = proto.u.o;
    if (self.t == JS_OBJ)
        js_put(J, self.u.o, js_mkstr(J, "%ctx", 4), js_obj(o), 1);
    return js_obj(o);
}

static jsval canvas_to_data_url(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = jsdom_node_of(self);
    if (!n)
        return js_throw_error(J, "TypeError: Illegal invocation");
    struct image *img = canvas_image(J, n, 1);
    if (!img)
        return js_str(J, "data:,");
    size_t len;
    uint8_t *png = cv_png(img, &len);
    if (!png)
        return js_str(J, "data:,");
    size_t out = 22 + (len + 2) / 3 * 4;
    char *s = malloc(out + 1);
    if (!s) {
        free(png);
        return js_str(J, "data:,");
    }
    size_t o = (size_t)sprintf(s, "data:image/png;base64,");
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)png[i] << 16 | (i + 1 < len ? (uint32_t)png[i + 1] << 8 : 0) | (i + 2 < len ? png[i + 2] : 0);
        s[o++] = b64[v >> 18 & 63];
        s[o++] = b64[v >> 12 & 63];
        s[o++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
        s[o++] = i + 2 < len ? b64[v & 63] : '=';
    }
    jsval r = js_strn(J, s, (int)o);
    free(s);
    free(png);
    return r;
}

/* ---- OffscreenCanvas, ImageBitmap and toBlob, in script ---- */

static const char canvas_lib[] =
    "(function(g){\n"
    "  var def = function (o, n, v) { Object.defineProperty(o, n, { value: v, writable: true, configurable: true, enumerable: false }); };\n"
    "  class OffscreenCanvas {\n"
    "    constructor(w, h) { var c = document.createElement('canvas'); c.width = w | 0; c.height = h | 0; def(this, '_c', c); }\n"
    "    get width() { return this._c.width; } set width(v) { this._c.width = v; }\n"
    "    get height() { return this._c.height; } set height(v) { this._c.height = v; }\n"
    "    getContext(t, o) { return this._c.getContext(t, o); }\n"
    "    convertToBlob(o) { var c = this._c; return new Promise(function (res) { c.toBlob(res, o && o.type, o && o.quality); }); }\n"
    "    transferToImageBitmap() { return makeBitmap(this._c, 0, 0, this._c.width, this._c.height); }\n"
    "  }\n"
    "  class ImageBitmap { constructor() { throw new TypeError('Illegal constructor'); } close() { this.width = 0; this.height = 0; } }\n"
    "  function makeBitmap(src, sx, sy, sw, sh) {\n"
    "    var c = document.createElement('canvas'); c.width = sw; c.height = sh;\n"
    "    var x = c.getContext('2d');\n"
    "    if (src && src.data && src.width) { x.putImageData(src, -sx, -sy); }\n"
    "    else x.drawImage(src, sx, sy, sw, sh, 0, 0, sw, sh);\n"
    "    var b = Object.create(ImageBitmap.prototype); def(b, '_c', c);\n"
    "    Object.defineProperty(b, 'width', { value: sw, writable: true, configurable: true });\n"
    "    Object.defineProperty(b, 'height', { value: sh, writable: true, configurable: true });\n"
    "    return b;\n"
    "  }\n"
    "  g.createImageBitmap = function (src, sx, sy, sw, sh) {\n"
    "    return new Promise(function (res, rej) {\n"
    "      var w = src && (src.naturalWidth || src.videoWidth || src.width), h = src && (src.naturalHeight || src.videoHeight || src.height);\n"
    "      if (!src || !w || !h) { rej(new DOMException('The source image could not be decoded.', 'InvalidStateError')); return; }\n"
    "      if (typeof sx !== 'number') { sx = 0; sy = 0; sw = w; sh = h; }\n"
    "      else if (typeof sw !== 'number') { sw = w; sh = h; }\n"
    "      res(makeBitmap(src, sx, sy, sw, sh));\n"
    "    });\n"
    "  };\n"
    "  def(g, 'OffscreenCanvas', OffscreenCanvas); def(g, 'ImageBitmap', ImageBitmap);\n"
    "  g.HTMLCanvasElement.prototype.toBlob = function (cb, type, q) {\n"
    "    var url = this.toDataURL(type, q), i = url.indexOf(',');\n"
    "    if (i < 0) { setTimeout(function () { cb(null); }, 0); return; }\n"
    "    var bin = atob(url.slice(i + 1)), arr = new Uint8Array(bin.length);\n"
    "    for (var k = 0; k < bin.length; k++) arr[k] = bin.charCodeAt(k);\n"
    "    var mime = url.slice(5, url.indexOf(';'));\n"
    "    setTimeout(function () { cb(new Blob([arr], { type: mime })); }, 0);\n"
    "  };\n"
    "  g.HTMLCanvasElement.prototype.captureStream = function () { throw new DOMException('Not supported', 'NotSupportedError'); };\n"
    "})(globalThis);\n";

static void define_methods(struct js *J, struct jsobj *proto)
{
    static const struct { const char *name; js_native fn; int n; } methods[] = {
        {"save", ctx_save, 0}, {"restore", ctx_restore, 0}, {"reset", ctx_reset, 0},
        {"scale", ctx_scale, 2}, {"rotate", ctx_rotate, 1}, {"translate", ctx_translate, 2},
        {"transform", ctx_transform, 6}, {"setTransform", ctx_set_transform, 6},
        {"resetTransform", ctx_reset_transform, 0}, {"getTransform", ctx_get_transform, 0},
        {"beginPath", ctx_begin_path, 0}, {"closePath", ctx_close_path, 0}, {"moveTo", ctx_move_to, 2},
        {"lineTo", ctx_line_to, 2}, {"quadraticCurveTo", ctx_quad_to, 4}, {"bezierCurveTo", ctx_bezier_to, 6},
        {"arcTo", ctx_arc_to, 5}, {"arc", ctx_arc, 5}, {"ellipse", ctx_ellipse, 7}, {"rect", ctx_rect, 4},
        {"roundRect", ctx_round_rect, 4}, {"fill", ctx_fill, 0}, {"stroke", ctx_stroke, 0}, {"clip", ctx_clip, 0},
        {"isPointInPath", ctx_in_path, 2}, {"isPointInStroke", ctx_in_stroke, 2},
        {"fillRect", ctx_fill_rect, 4}, {"strokeRect", ctx_stroke_rect, 4}, {"clearRect", ctx_clear_rect, 4},
        {"setLineDash", ctx_set_line_dash, 1}, {"getLineDash", ctx_get_line_dash, 0},
        {"fillText", ctx_fill_text, 3}, {"strokeText", ctx_stroke_text, 3}, {"measureText", ctx_measure_text, 1},
        {"drawImage", ctx_draw_image, 3}, {"getImageData", ctx_get_image_data, 4},
        {"putImageData", ctx_put_image_data, 3}, {"createImageData", ctx_create_image_data, 1},
        {"createLinearGradient", ctx_create_linear, 4}, {"createRadialGradient", ctx_create_radial, 6},
        {"createConicGradient", ctx_create_conic, 3}, {"createPattern", ctx_create_pattern, 2},
        {"getContextAttributes", ctx_get_attributes, 0}, {"isContextLost", ctx_false, 0},
        {"drawFocusIfNeeded", ctx_nothing, 1}, {"scrollPathIntoView", ctx_nothing, 0},
    };
    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
        js_define_native(J, proto, methods[i].name, methods[i].fn, methods[i].n);
}

static struct jsobj *make_class(struct js *J, const char *name, jsval *ctor_out)
{
    struct jsobj *proto = js_new_object(J);
    jsval ctor = js_new_function(J, ctx_nothing, name, 0);
    js_set(J, ctor, "prototype", js_obj(proto));
    js_put(J, proto, js_mkstr(J, "constructor", 11), ctor, 1);
    js_set(J, js_obj(js_global(J)), name, ctor);
    if (ctor_out)
        *ctor_out = ctor;
    return proto;
}

void jscanvas_install(struct js *J, struct jsobj *canvas_proto)
{
    cur_J = J;
    cv_gc_alloc = gc_hook;
    define_methods(J, make_class(J, "CanvasRenderingContext2D", NULL));
    js_define_native(J, make_class(J, "CanvasGradient", NULL), "addColorStop", grad_add_stop, 2);
    make_class(J, "CanvasPattern", NULL);
    js_define_native(J, canvas_proto, "getContext", canvas_get_context, 2);
    js_define_native(J, canvas_proto, "toDataURL", canvas_to_data_url, 2);
    if (js_eval(J, canvas_lib, (int)sizeof(canvas_lib) - 1, "canvas library") < 0)
        fprintf(stderr, "canvas library: %s\n", js_error(J));
}
