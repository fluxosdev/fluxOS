/*
 * View transitions. document.startViewTransition() asks for pictures of the page as it is
 * (vt_capture): the page without the elements that have a view-transition-name, and each of
 * those apart, over a transparent background (painted over black and over white, which gives
 * their alpha). While the page's scripts change the DOM the old pictures stay on the screen.
 * Then the new pictures are taken (vt_begin) and each group, the root and every name, moves
 * from its old box to its new one while its old picture fades out and the new one fades in,
 * added together (mix-blend-mode: plus-lighter).
 *
 * The pseudo-elements (::view-transition-group(name), -image-pair, -old, -new) are styled by
 * css.c from the page's style sheets each frame, and their animations run in anim.c on nodes
 * kept here; so a page's own @keyframes and timing for them apply. Opacity and transforms of
 * the old and new pictures are drawn; the group's own box follows its animation's timing.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

#define VT_MAX_GROUPS 64
#define VT_MAX_PIXELS (4u << 20)        /* a picture bigger than this is cut down to the window */

struct vt_image {
    int w, h;
    uint32_t *px;                       /* premultiplied: alpha << 24 | red, green, blue */
};

struct vt_group {
    char name[96];
    char *classes;                      /* view-transition-class (malloc'd), or NULL */
    struct vt_image old, now;
    struct rect old_r, new_r;           /* border boxes in the window */
    int has_old, has_new;
    int order;                          /* where it paints, among the groups */
    struct node holder[4];              /* the group, image pair, old and new pseudo-elements' animations */
};

struct vtrans {
    struct vt_group *g;                 /* [0] is the root */
    int n;
    int begun;
    unsigned t0;                        /* anim_now when it began */
    uint32_t canvas;                    /* the new page's background */
    struct arena arena;                 /* the pseudo-elements' styles, for its whole length */
};

/* ---- pictures ---- */

struct keep_ctx {
    int vtg;                            /* items of this group (0: the page without the named ones) */
};

static int keep_group(void *ctx, const struct ditem *d)
{
    return d->vtg == ((struct keep_ctx *)ctx)->vtg;
}

static void vt_image_free(struct vt_image *im)
{
    free(im->px);
    memset(im, 0, sizeof(*im));
}

/* The items of one group, for the window rectangle r, over black and over white: alpha from the difference. */
static int snapshot(const struct vt_page *pg, int vtg, struct rect r, int opaque, struct vt_image *out)
{
    memset(out, 0, sizeof(*out));
    if (r.w <= 0 || r.h <= 0 || (size_t)r.w * (size_t)r.h > VT_MAX_PIXELS)
        return 0;
    size_t count = (size_t)r.w * (size_t)r.h;
    uint32_t *black = malloc(count * sizeof(uint32_t)), *white = opaque ? NULL : malloc(count * sizeof(uint32_t));
    if (!black || (!opaque && !white)) {
        free(black);
        free(white);
        return 0;
    }
    struct keep_ctx k = { vtg };
    struct surface s = { .px = black, .w = r.w, .h = r.h, .stride = r.w, .clip = { 0, 0, r.w, r.h } };
    layout_paint_filtered(pg->lo, &s, -r.x, -r.y, pg->sx, pg->sy, opaque ? pg->lo->canvas & 0xFFFFFF : 0,
                          keep_group, &k);
    if (!opaque) {
        s.px = white;
        layout_paint_filtered(pg->lo, &s, -r.x, -r.y, pg->sx, pg->sy, 0xFFFFFF, keep_group, &k);
    }
    for (size_t i = 0; i < count; i++) {
        uint32_t b = black[i] & 0xFFFFFF;
        unsigned a = 255;
        if (!opaque) {
            int clear = (int)(white[i] >> 8 & 255) - (int)(b >> 8 & 255);
            a = clear < 0 ? 255 : clear > 255 ? 0 : 255u - (unsigned)clear;
        }
        black[i] = a << 24 | b;
    }
    free(white);
    out->w = r.w, out->h = r.h, out->px = black;
    return 1;
}

/* ---- the groups: what has a name, where it is ---- */

static void group_name(const struct node *n, char *out, int size)
{
    const char *name = n->style->vt_name;
    if (!strcmp(name, VT_NAME_ELEMENT))
        snprintf(out, (size_t)size, "\001%p", (const void *)n);       /* one of its own, kept by the element */
    else
        snprintf(out, (size_t)size, "%s", name);
}

static struct vt_group *group_find(struct vtrans *t, const char *name)
{
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->g[i].name, name))
            return &t->g[i];
    return NULL;
}

/* Where each named element is in the window, and the order the groups paint in. */
static int collect(struct vtrans *t, const struct vt_page *pg, int new_state, int *duplicate)
{
    const struct layout *lo = pg->lo;
    struct rect window = mkrect(0, 0, pg->vw, pg->vh);
    for (int i = 0; i < lo->nvt; i++) {
        const struct lbox *e = &lo->vt[i];
        if (!e->node->style || !e->node->style->vt_name || e->node->style->display == D_NONE)
            continue;
        char name[96];
        group_name(e->node, name, sizeof(name));
        struct vt_group *g = group_find(t, name);
        if (g && (new_state ? g->has_new : g->has_old)) {
            *duplicate = 1;             /* two elements with the same name */
            return 0;
        }
        if (!g) {
            if (t->n >= VT_MAX_GROUPS)
                continue;
            g = &t->g[t->n++];
            snprintf(g->name, sizeof(g->name), "%s", name);
            g->classes = e->node->style->vt_class ? strdup(e->node->style->vt_class) : NULL;
        }
        struct rect r = layout_node_visual_rect(lo, e->node, mkrect(e->x, e->y, e->w, e->h), pg->sx, pg->sy);
        r.x -= pg->sx, r.y -= pg->sy;
        if ((size_t)(r.w > 0 ? r.w : 0) * (size_t)(r.h > 0 ? r.h : 0) > VT_MAX_PIXELS)
            r = rect_intersect(r, window);              /* only what shows of something enormous */
        int first = 0x7fffffff;                         /* where its first item paints */
        for (int k = 0; k < lo->nitems; k++)
            if (lo->items[k].vtg == i + 1) {
                first = k;
                break;
            }
        struct vt_image *im = new_state ? &g->now : &g->old;
        if (!snapshot(pg, i + 1, r, 0, im))
            memset(im, 0, sizeof(*im));
        if (new_state)
            g->has_new = 1, g->new_r = r, g->order = first;
        else
            g->has_old = 1, g->old_r = r, g->order = g->has_new ? g->order : first;
    }
    return 1;
}

static int by_order(const void *a, const void *b)
{
    const struct vt_group *x = a, *y = b;
    return x->order < y->order ? -1 : x->order > y->order;
}

static int group_styles(struct vtrans *t, const struct vt_page *pg, struct vt_group *g, const struct style **gst,
                        const struct style **pst, const struct style **ost, const struct style **nst);

struct vtrans *vt_capture(const struct vt_page *pg, int *duplicate)
{
    *duplicate = 0;
    if (!pg->lo || !pg->root || pg->vw <= 0 || pg->vh <= 0 || (size_t)pg->vw * (size_t)pg->vh > VT_MAX_PIXELS)
        return NULL;
    struct vtrans *t = calloc(1, sizeof(*t));
    if (!t || !(t->g = calloc(VT_MAX_GROUPS + 1, sizeof(*t->g)))) {
        free(t);
        return NULL;
    }
    t->n = 1;
    snprintf(t->g[0].name, sizeof(t->g[0].name), "root");
    t->g[0].has_old = 1;
    t->g[0].old_r = mkrect(0, 0, pg->vw, pg->vh);
    t->g[0].order = -1;
    if (!collect(t, pg, 0, duplicate) || !snapshot(pg, 0, t->g[0].old_r, 1, &t->g[0].old)) {
        vt_free(t);
        return NULL;
    }
    return t;
}

int vt_begin(struct vtrans *t, const struct vt_page *pg, int *duplicate)
{
    *duplicate = 0;
    if (!t || t->begun)
        return 0;
    t->g[0].has_new = 1;
    t->g[0].new_r = mkrect(0, 0, pg->vw, pg->vh);
    if (!collect(t, pg, 1, duplicate) || !snapshot(pg, 0, t->g[0].new_r, 1, &t->g[0].now))
        return 0;
    qsort(t->g + 1, (size_t)(t->n - 1), sizeof(*t->g), by_order);
    t->canvas = pg->lo->canvas & 0xFFFFFF;
    t->begun = 1;
    t->t0 = anim_now;
    for (int i = 0; i < t->n; i++) {    /* their animations start now */
        const struct style *gst, *pst, *ost, *nst;
        group_styles(t, pg, &t->g[i], &gst, &pst, &ost, &nst);
    }
    return 1;
}

int vt_begun(const struct vtrans *t)
{
    return t && t->begun;
}

void vt_free(struct vtrans *t)
{
    if (!t)
        return;
    for (int i = 0; i < t->n; i++) {
        for (int k = 0; k < 4; k++)
            anim_forget(&t->g[i].holder[k]);
        vt_image_free(&t->g[i].old);
        vt_image_free(&t->g[i].now);
        free(t->g[i].classes);
    }
    arena_free(&t->arena);
    free(t->g);
    free(t);
}

/* ---- drawing ---- */

struct placed {                         /* a picture in the window: window = m * picture */
    const struct vt_image *im;
    double m[6];                        /* a, b, c, d, e, f: x' = a x + c y + e, y' = b x + d y + f */
    double inv[6];
    unsigned alpha;                     /* 0 to 255 */
};

static int invert(const double m[6], double out[6])
{
    double det = m[0] * m[3] - m[1] * m[2];
    if (fabs(det) < 1e-12)
        return 0;
    out[0] = m[3] / det, out[1] = -m[1] / det, out[2] = -m[2] / det, out[3] = m[0] / det;
    out[4] = -(out[0] * m[4] + out[2] * m[5]);
    out[5] = -(out[1] * m[4] + out[3] * m[5]);
    return 1;
}

/* m = a * b (b first) */
static void multiply(const double a[6], const double b[6], double out[6])
{
    double r[6] = { a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1], a[0] * b[2] + a[2] * b[3],
                    a[1] * b[2] + a[3] * b[3], a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5] };
    memcpy(out, r, sizeof(r));
}

/*
 * A picture inside its group's box r: as wide as the box, its height in proportion, at the top
 * (::view-transition-old/new are inline-size: 100%, block-size: auto); then the pseudo-element's
 * transform, about its transform-origin.
 */
static int place(struct placed *p, const struct vt_image *im, struct rect r, const struct style *st, unsigned alpha)
{
    if (!im->px || im->w <= 0 || im->h <= 0 || r.w <= 0 || alpha == 0)
        return 0;
    double k = (double)r.w / im->w, bw = r.w, bh = im->h * k;
    double m[6] = { k, 0, 0, k, r.x, r.y };
    if (st && (st->xf || st->xf_ind)) {
        double xf[6], ox = css_transform_length(st->tox, (int)bw), oy = css_transform_length(st->toy, (int)bh);
        css_transform_matrix(st, (int)bw, (int)bh, xf);
        double to[6] = { 1, 0, 0, 1, r.x + ox, r.y + oy }, from[6] = { 1, 0, 0, 1, -ox, -oy };
        double scale[6] = { k, 0, 0, k, 0, 0 }, t1[6], t2[6];
        multiply(xf, from, t1);
        multiply(to, t1, t2);
        multiply(t2, scale, m);
    }
    if (!invert(m, p->inv))
        return 0;
    memcpy(p->m, m, sizeof(m));
    p->im = im;
    p->alpha = alpha;
    return 1;
}

static void bounds(const struct placed *p, int *x0, int *y0, int *x1, int *y1)
{
    for (int c = 0; c < 4; c++) {
        double x = c & 1 ? p->im->w : 0, y = c & 2 ? p->im->h : 0;
        double wx = p->m[0] * x + p->m[2] * y + p->m[4], wy = p->m[1] * x + p->m[3] * y + p->m[5];
        int fx0 = (int)floor(wx), fy0 = (int)floor(wy), fx1 = (int)ceil(wx), fy1 = (int)ceil(wy);
        if (fx0 < *x0) *x0 = fx0;
        if (fy0 < *y0) *y0 = fy0;
        if (fx1 > *x1) *x1 = fx1;
        if (fy1 > *y1) *y1 = fy1;
    }
}

/* Some pictures added together (plus-lighter), then put over what s has, within the window. */
static void composite(struct surface *s, int ox, int oy, int vw, int vh, const struct placed *pics, int n)
{
    if (!n)
        return;
    int x0 = 0x7fffffff, y0 = 0x7fffffff, x1 = -0x7fffffff, y1 = -0x7fffffff;
    for (int i = 0; i < n; i++)
        bounds(&pics[i], &x0, &y0, &x1, &y1);
    struct rect area = rect_intersect(mkrect(x0, y0, x1 - x0, y1 - y0), mkrect(0, 0, vw, vh));
    area = rect_intersect(mkrect(area.x + ox, area.y + oy, area.w, area.h), s->clip);
    if (rect_empty(area))
        return;
    for (int y = area.y; y < area.y + area.h; y++) {
        uint32_t *row = &s->px[(size_t)y * (size_t)s->stride];
        double wy = y - oy + 0.5;
        for (int x = area.x; x < area.x + area.w; x++) {
            double wx = x - ox + 0.5;
            unsigned acc[4] = { 0, 0, 0, 0 };   /* alpha, red, green, blue */
            for (int i = 0; i < n; i++) {
                const struct placed *p = &pics[i];
                double u = p->inv[0] * wx + p->inv[2] * wy + p->inv[4];
                double v = p->inv[1] * wx + p->inv[3] * wy + p->inv[5];
                if (u < 0 || v < 0 || u >= p->im->w || v >= p->im->h)
                    continue;
                uint32_t px = p->im->px[(size_t)(int)v * (size_t)p->im->w + (size_t)(int)u];
                acc[0] += (px >> 24) * p->alpha / 255;
                acc[1] += (px >> 16 & 255) * p->alpha / 255;
                acc[2] += (px >> 8 & 255) * p->alpha / 255;
                acc[3] += (px & 255) * p->alpha / 255;
            }
            if (!acc[0])
                continue;
            unsigned a = acc[0] > 255 ? 255 : acc[0], under = row[x], out = 0;
            for (int ch = 0; ch < 3; ch++) {
                unsigned c = acc[ch + 1] > 255 ? 255 : acc[ch + 1];
                unsigned u = under >> (16 - 8 * ch) & 255;
                unsigned v = c + u * (255 - a) / 255;
                out |= (v > 255 ? 255 : v) << (16 - 8 * ch);
            }
            row[x] = out;
        }
    }
}

/* How far through its move the group is (0 to 1), and whether that is still going on. */
static double group_progress(const struct vtrans *t, const struct style *gst, int *running)
{
    if (!gst)
        return 1;
    for (int i = 0; i < gst->nanims; i++) {
        const struct css_animation *an = &gst->anims[i];
        if (strcmp(an->name, "-ua-view-transition-group-anim"))
            continue;
        int el = (int)(anim_now - t->t0) - an->delay;
        if (an->duration <= 0)
            return 1;
        if (el < an->duration)
            *running = 1;
        double x = el <= 0 ? 0 : el >= an->duration ? 1 : (double)el / an->duration;
        return anim_ease(&an->ease, (float)x);
    }
    return 1;
}

/* The group's pseudo-elements, styled for anim_now (their animations start the first time); 1 if one moves. */
static int group_styles(struct vtrans *t, const struct vt_page *pg, struct vt_group *g, const struct style **gst,
                        const struct style **pst, const struct style **ost, const struct style **nst)
{
    struct node *h = g->holder;
    *gst = css_vt_style(&t->arena, pg->root, pg->sheets, pg->nsheets, pg->vw, pg->vh, VT_GROUP, g->name,
                        g->classes, NULL, &h[0]);
    *pst = css_vt_style(&t->arena, pg->root, pg->sheets, pg->nsheets, pg->vw, pg->vh, VT_PAIR, g->name,
                        g->classes, *gst, &h[1]);
    *ost = g->has_old ? css_vt_style(&t->arena, pg->root, pg->sheets, pg->nsheets, pg->vw, pg->vh, VT_OLD,
                                     g->name, g->classes, *pst, &h[2]) : NULL;
    *nst = g->has_new ? css_vt_style(&t->arena, pg->root, pg->sheets, pg->nsheets, pg->vw, pg->vh, VT_NEW,
                                     g->name, g->classes, *pst, &h[3]) : NULL;
    int running = 0;
    for (int k = 0; k < 4; k++)
        running |= anim_running(&h[k]);
    return running;
}

static struct rect lerp_rect(struct rect a, struct rect b, double p)
{
    return mkrect((int)lround(a.x + (b.x - a.x) * p), (int)lround(a.y + (b.y - a.y) * p),
                  (int)lround(a.w + (b.w - a.w) * p), (int)lround(a.h + (b.h - a.h) * p));
}

int vt_draw(struct vtrans *t, const struct vt_page *pg, struct surface *s, int ox, int oy)
{
    if (!t)
        return 0;
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, mkrect(ox, oy, pg->vw, pg->vh));
    if (!t->begun) {                    /* the update is running: the old pictures, as they were */
        gfx_fill(s, ox, oy, pg->vw, pg->vh, 0xFFFFFF);
        for (int i = 0; i < t->n; i++) {
            struct placed p;
            if (t->g[i].has_old && place(&p, &t->g[i].old, t->g[i].old_r, NULL, 255))
                composite(s, ox, oy, pg->vw, pg->vh, &p, 1);
        }
        s->clip = saved;
        return 1;
    }
    int running = 0;
    gfx_fill(s, ox, oy, pg->vw, pg->vh, t->canvas);
    for (int i = 0; i < t->n; i++) {
        struct vt_group *g = &t->g[i];
        const struct style *gst, *pst, *ost, *nst;
        running |= group_styles(t, pg, g, &gst, &pst, &ost, &nst);
        double p = g->has_old && g->has_new ? group_progress(t, gst, &running) : 1;
        struct rect r = g->has_old && g->has_new ? lerp_rect(g->old_r, g->new_r, p) : g->has_new ? g->new_r : g->old_r;
        struct placed pics[2];
        int n = 0;
        /* opacity: what its animation (or a style) gives, times the group's and the pair's */
        unsigned shared = (gst ? gst->opacity : 255) * (pst ? pst->opacity : 255) / 255;
        if (g->has_old && place(&pics[n], &g->old, r, ost, (ost ? ost->opacity : 0) * shared / 255))
            n++;
        if (g->has_new && place(&pics[n], &g->now, r, nst, (nst ? nst->opacity : 255) * shared / 255))
            n++;
        composite(s, ox, oy, pg->vw, pg->vh, pics, n);
    }
    s->clip = saved;
    return running;
}
