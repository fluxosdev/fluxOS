/*
 * Layout: turns the styled document tree into a list of things to paint
 * (rectangles, text, images, form controls) in page coordinates, plus
 * the rectangles of links and form controls for the mouse.
 *
 * Block boxes stack vertically, with adjacent margins collapsed. Inline
 * content is gathered into a run of items (words, spaces, line breaks,
 * the edges of inline elements, and atomic boxes such as images and
 * inline-blocks) and broken into lines at spaces. Tables size their
 * columns from the minimum and maximum widths of their cells. Flex
 * containers grow and shrink their items along lines and align them
 * across; grid containers size column tracks (fixed, content and fr) and
 * place items in rows. Floats sit against the sides of their block
 * formatting context and lines are shortened beside them; relatively
 * positioned boxes are moved after layout, and absolute and fixed ones are
 * laid out once the box they are placed against is known.
 *
 * Atomic boxes are laid out at (0, 0) first, to learn their size, and
 * moved into place when their line is placed; table cells are moved
 * down to their vertical alignment the same way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"
#include "simd.h"

#define MAX_COLS   128
#define MAX_ROWS   4096
#define BIG        (1 << 24)

enum { I_WORD, I_SPACE, I_BREAK, I_ATOMIC, I_OPEN, I_CLOSE };

struct mark {
    int items, links, controls, anchors;
};

struct item {
    uint8_t kind;
    uint8_t wrap;               /* space: a line may break here */
    uint8_t invisible;          /* a space that is only a break opportunity (<wbr>) */
    const struct style *st;
    struct node *node;          /* atomic, open, close: the element */
    struct node *link;          /* the enclosing <a href> */
    const char *text;
    int len;
    int w, wmin;                /* wmin: an atomic's narrowest width (measuring) */
    int h, base;                /* atomic: margin-box height, baseline from its top */
    int x;
    struct mark m0, m1;         /* atomic: what it painted, to be moved */
};

struct open_el {                /* an inline element with a background or border, on this line */
    const struct style *st;
    int x;
};

struct flow {
    int x, w;                   /* the container's content box */
    int y0, cy, pm;             /* top, where the next box goes, pending margin */
    const struct style *st;
    struct item *items;
    int n, cap;
    int space_before;           /* the run ends in a collapsible space (or is empty) */
    struct node *link;
    int lines;                  /* lines placed so far */
    int last_line;              /* the line being placed is the last of its block (or before a break) */
    int min, max;               /* measuring */
    struct open_el open[16];
    int nopen;
};

struct lctx {
    struct arena *a;
    struct layout *lo;
    int vw, vh;
    int measuring;
    unsigned gen;
    /* the marker of a list item, waiting for the item's first line */
    const struct style *marker_st;
    char marker_text[16];
    int marker_kind;            /* LS_* */
    struct image *marker_img;   /* list-style-image */
    int marker_x;               /* the list item's content box left */
    int marker_pending;
    char inside_text[24];       /* list-style-position: inside: the marker, to start the item's text */
    const struct style *inside_st;
    int last_baseline;          /* of the most recent line */
    struct node *skip_bg;       /* the element whose background became the canvas */
    struct node *contain;       /* a flex or grid item: its children's margins stay inside */
    /* floats of the current block formatting context are floats[float_base..nfloats) */
    struct fbox { int x, y, w, h, side; } floats[64];    /* margin boxes; side 1 left, 2 right */
    int nfloats, float_base;
    /* absolute and fixed boxes waiting for the box they are placed against */
    struct pend { struct node *n; int sx, sy; } pend[128];
    int npend;
    int force_container;        /* the next block box places its absolute descendants itself */
    int abs_bw1;                /* an absolute box's stretched border-box width, plus 1; 0: none */
    struct zrange { int from, to, z; } zr[64];   /* items of positioned boxes with a z-index */
    int nzr;
};

static unsigned layout_gen;
static int len_px(struct len l, int base);
static int bg_picture_wanted(const struct style *st);
static void bg_picture_finish(struct lctx *c, struct node *n, int slot);

static int layout_block_box(struct lctx *c, struct node *n, int x, int y, int cw, int force_bw,
                            int *mb, int *bg_slot);
static int layout_table(struct lctx *c, struct node *n, int x, int y, int cw, int force_bw, int *mb);
static void measure(struct lctx *c, struct node *n, int *min, int *max);
static void flow_children(struct lctx *c, struct flow *f, struct node *n);
static void flush_run(struct lctx *c, struct flow *f);
static int layout_flex(struct lctx *c, struct node *n, int x, int y, int w, int defh, int minh);
static int layout_grid(struct lctx *c, struct node *n, int x, int y, int w, int defh, int minh);
static int muldiv(int a, int b, int c);
static void container_measure(struct lctx *c, struct node *n, int *min, int *max);

/* ---- output ---- */

static void *grow(struct arena *a, void *arr, int n, int *cap, size_t size)
{
    if (n < *cap)
        return arr;
    int nc = *cap ? *cap * 2 : 256;
    void *na = arena_alloc(a, size * (size_t)nc);
    if (!na)
        return NULL;
    if (n)
        memcpy(na, arr, size * (size_t)n);
    *cap = nc;
    return na;
}

static struct ditem *emit(struct lctx *c, int kind)
{
    struct layout *lo = c->lo;
    struct ditem *items = grow(c->a, lo->items, lo->nitems, &lo->cap, sizeof(struct ditem));
    if (!items)
        return NULL;
    lo->items = items;
    struct ditem *d = &items[lo->nitems++];
    memset(d, 0, sizeof(*d));
    d->kind = (uint8_t)kind;
    return d;
}

static void add_hotspot(struct lctx *c, struct hotspot **arr, int *n, int *cap, struct node *node,
                        int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    /* Extend the last rectangle when a link continues along the same line. */
    if (*n && (*arr)[*n - 1].node == node && (*arr)[*n - 1].y == y && (*arr)[*n - 1].h == h &&
        (*arr)[*n - 1].x + (*arr)[*n - 1].w >= x - 1 && (*arr)[*n - 1].x <= x) {
        struct hotspot *last = &(*arr)[*n - 1];
        if (x + w > last->x + last->w)
            last->w = x + w - last->x;
        return;
    }
    struct hotspot *na = grow(c->a, *arr, *n, cap, sizeof(struct hotspot));
    if (!na)
        return;
    *arr = na;
    (*arr)[(*n)++] = (struct hotspot){ x, y, w, h, node, 0, 0 };
}

static void add_anchor(struct lctx *c, const char *name, int y)
{
    struct layout *lo = c->lo;
    struct anchor *na = grow(c->a, lo->anchors, lo->nanchors, &lo->anchorcap, sizeof(struct anchor));
    if (!na)
        return;
    lo->anchors = na;
    lo->anchors[lo->nanchors++] = (struct anchor){ name, y };
}

static void anchors_of(struct lctx *c, struct node *n, int y)
{
    if (!n)
        return;
    const char *id = node_attr(n, "id");
    if (id && *id)
        add_anchor(c, id, y);
    const char *name = n->tag == T_A ? node_attr(n, "name") : NULL;
    if (name && *name)
        add_anchor(c, name, y);
}

static struct mark mark_now(struct lctx *c)
{
    struct layout *lo = c->lo;
    return (struct mark){ lo->nitems, lo->nlinks, lo->ncontrols, lo->nanchors };
}

/* Does dyn (1 + an index into lo->dyn) belong to a fixed box, or to one inside a fixed box? */
static int under_fixed(const struct layout *lo, int dyn)
{
    for (int k = dyn, depth = 0; k && depth < 16; k = lo->dyn[k - 1].parent, depth++)
        if (lo->dyn[k - 1].fixed)
            return 1;
    return 0;
}

static void shift(struct lctx *c, struct mark from, struct mark to, int dx, int dy)
{
    struct layout *lo = c->lo;
    for (int i = from.items; i < to.items; i++)
        if (!lo->items[i].dyn || !under_fixed(lo, lo->items[i].dyn))     /* fixed ones stay on the screen */
            lo->items[i].x += dx, lo->items[i].y += dy;
    for (int i = from.links; i < to.links; i++)
        if (!lo->links[i].dyn || !under_fixed(lo, lo->links[i].dyn))
            lo->links[i].x += dx, lo->links[i].y += dy;
    for (int i = from.controls; i < to.controls; i++)
        if (!lo->controls[i].dyn || !under_fixed(lo, lo->controls[i].dyn))
            lo->controls[i].x += dx, lo->controls[i].y += dy;
    for (int i = from.anchors; i < to.anchors; i++)
        lo->anchors[i].y += dy;
    for (int k = 0; k < lo->ndyn; k++) {        /* sticky boxes and their containers move with their items */
        struct dynbox *d = &lo->dyn[k];
        if (d->fixed || d->i0 < from.items || d->i1 > to.items)
            continue;
        d->x += dx, d->y += dy;
        if (d->cy1 != DYN_NONE)
            d->cx0 += dx, d->cx1 += dx, d->cy0 += dy, d->cy1 += dy;
    }
}

/* A box that moves as the page scrolls: it owns the items, links and controls laid out since m0. */
static void dyn_register(struct lctx *c, struct mark m0, int fixed, int x, int y, int w, int h,
                         const struct style *st, struct node *container)
{
    struct layout *lo = c->lo;
    if (lo->ndyn >= MAX_DYNBOX || !lo->dyn)
        return;
    int idx = lo->ndyn++;
    struct dynbox *d = &lo->dyn[idx];
    memset(d, 0, sizeof(*d));
    d->fixed = (uint8_t)fixed;
    d->x = x, d->y = y, d->w = w, d->h = h;
    d->top = st->top.unit == U_AUTO ? DYN_NONE : len_px(st->top, c->vh);
    d->bottom = st->bottom.unit == U_AUTO ? DYN_NONE : len_px(st->bottom, c->vh);
    d->left = st->left.unit == U_AUTO ? DYN_NONE : len_px(st->left, c->vw);
    d->right = st->right.unit == U_AUTO ? DYN_NONE : len_px(st->right, c->vw);
    d->cy0 = d->cy1 = d->cx0 = d->cx1 = DYN_NONE;
    d->container = fixed ? NULL : container;
    d->i0 = m0.items;
    d->i1 = lo->nitems;
    /* Whatever is not in an inner moving box yet moves with this one; inner ones are now inside it. */
    for (int i = m0.items; i < lo->nitems; i++) {
        struct ditem *it = &lo->items[i];
        if (!it->dyn) {
            it->dyn = (uint8_t)(idx + 1);
            continue;
        }
        int k = it->dyn - 1, depth = 0;
        while (lo->dyn[k].parent && depth++ < 16)
            k = lo->dyn[k].parent - 1;
        if (k != idx)
            lo->dyn[k].parent = (uint8_t)(idx + 1);
    }
    struct hotspot *arrs[2] = { lo->links, lo->controls };
    int from[2] = { m0.links, m0.controls }, to[2] = { lo->nlinks, lo->ncontrols };
    for (int a = 0; a < 2; a++)
        for (int i = from[a]; i < to[a]; i++) {
            struct hotspot *h2 = &arrs[a][i];
            if (!h2->dyn) {
                h2->dyn = (uint8_t)(idx + 1);
                continue;
            }
            int k = h2->dyn - 1, depth = 0;
            while (lo->dyn[k].parent && depth++ < 16)
                k = lo->dyn[k].parent - 1;
            if (k != idx)
                lo->dyn[k].parent = (uint8_t)(idx + 1);
        }
}

/* A block box is done: the sticky boxes inside it (first_dyn on) stay within its content area. */
static void dyn_set_container(struct lctx *c, int first_dyn, struct node *n, int x0, int y0, int x1, int y1)
{
    struct layout *lo = c->lo;
    for (int k = first_dyn; k < lo->ndyn; k++)
        if (!lo->dyn[k].fixed && lo->dyn[k].container == n && lo->dyn[k].cy1 == DYN_NONE) {
            lo->dyn[k].cx0 = x0, lo->dyn[k].cx1 = x1, lo->dyn[k].cy0 = y0, lo->dyn[k].cy1 = y1;
        }
}

void layout_dyn_offset(const struct layout *lo, int dyn, int sx, int sy, int *dx, int *dy)
{
    int chain[16], n = 0;
    for (int k = dyn; k && n < 16; k = lo->dyn[k - 1].parent)
        chain[n++] = k - 1;
    int ox = 0, oy = 0;
    for (int i = n - 1; i >= 0; i--) {              /* outermost first: what moved it is added in */
        const struct dynbox *d = &lo->dyn[chain[i]];
        if (d->fixed) {
            ox += sx;
            oy += sy;
            continue;
        }
        /* Sticky: kept inside the window's edges (inset by the offsets), then inside its container. */
        int vy = d->y + oy - sy, vx = d->x + ox - sx, ny = vy, nx = vx;
        if (d->bottom != DYN_NONE && lo->vh > 0 && ny + d->h > lo->vh - d->bottom)
            ny = lo->vh - d->bottom - d->h;
        if (d->top != DYN_NONE && ny < d->top)
            ny = d->top;
        if (d->right != DYN_NONE && lo->vw > 0 && nx + d->w > lo->vw - d->right)
            nx = lo->vw - d->right - d->w;
        if (d->left != DYN_NONE && nx < d->left)
            nx = d->left;
        int py = ny + sy, px = nx + sx;                 /* in page coordinates */
        if (d->cy1 != DYN_NONE) {
            if (py + d->h > d->cy1 + oy)
                py = d->cy1 + oy - d->h;
            if (py < d->cy0 + oy)
                py = d->cy0 + oy;
            if (px + d->w > d->cx1 + ox)
                px = d->cx1 + ox - d->w;
            if (px < d->cx0 + ox)
                px = d->cx0 + ox;
        }
        if (py < d->y + oy && d->top != DYN_NONE && d->bottom == DYN_NONE)
            py = d->y + oy;                             /* top-sticky boxes never rise above their place */
        if (px < d->x + ox && d->left != DYN_NONE && d->right == DYN_NONE)
            px = d->x + ox;
        oy += py - (d->y + oy);
        ox += px - (d->x + ox);
    }
    *dx = ox;
    *dy = oy;
}

/* ---- boxes that clip and scroll ---- */

struct sbrange {                /* what a box holds, as ranges of the output (a box's own borders are outside) */
    int i0, i1, l0, l1, k0, k1;
};

/* An item (or hotspot) tag joins the box idx: if it is in an inner box already, that one is inside idx now. */
static void sb_adopt(struct layout *lo, uint16_t *tag, int idx)
{
    if (!*tag) {
        *tag = (uint16_t)(idx + 1);
        return;
    }
    int k = *tag - 1, depth = 0;
    while (lo->sb[k].parent && depth++ < 64)
        k = lo->sb[k].parent - 1;
    if (k != idx)
        lo->sb[k].parent = (uint16_t)(idx + 1);
}

/*
 * A box with overflow other than visible: its contents (the ranges r) are clipped to its padding box
 * and, with overflow auto or scroll, scrolled. (x, y, w, h) is its border box; the box itself, its
 * borders and its shadow stay put. How far they overflow is learned once everything is in place.
 */
static void sbox_register(struct lctx *c, struct node *n, int x, int y, int w, int h,
                          const struct sbrange *r, int nr)
{
    struct layout *lo = c->lo;
    const struct style *st = n->style;
    if (!st->clips || c->measuring || !lo->sb || lo->nsb >= MAX_SBOX || !n->parent || n->tag == T_BODY ||
        n->tag == T_HTML)
        return;
    int px = x + st->border_w[3], py = y + st->border_w[0];
    int pw = w - st->border_w[3] - st->border_w[1], ph = h - st->border_w[0] - st->border_w[2];
    if (pw <= 0 || ph <= 0)
        return;
    int idx = lo->nsb++;
    struct sbox *b = &lo->sb[idx];
    memset(b, 0, sizeof(*b));
    b->x = px, b->y = py, b->w = pw, b->h = ph;
    b->ov_x = st->ov_x, b->ov_y = st->ov_y;
    b->node = n;
    for (int k = 0; k < nr; k++) {
        for (int i = r[k].i0; i < r[k].i1 && i < lo->nitems; i++) {
            struct ditem *it = &lo->items[i];
            if (it->dyn && under_fixed(lo, it->dyn))        /* a fixed box is not clipped by what it is in */
                continue;
            sb_adopt(lo, &it->sb, idx);
        }
        for (int i = r[k].l0; i < r[k].l1 && i < lo->nlinks; i++)
            if (!lo->links[i].dyn || !under_fixed(lo, lo->links[i].dyn))
                sb_adopt(lo, &lo->links[i].sb, idx);
        for (int i = r[k].k0; i < r[k].k1 && i < lo->ncontrols; i++)
            if (!lo->controls[i].dyn || !under_fixed(lo, lo->controls[i].dyn))
                sb_adopt(lo, &lo->controls[i].sb, idx);
    }
    struct ditem *d = emit(c, DK_SBOX);     /* where the box is, and its scroll bars: moves with the page */
    if (d) {
        d->x = px, d->y = py, d->w = pw, d->h = ph;
        d->sbi = (uint16_t)idx;
    }
}

static int rect_has(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static struct rect rect_and(struct rect a, struct rect b)
{
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return mkrect(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

/* A box's size is known (the layout is done): how far it overflows, which scroll bars it has, where it is scrolled. */
static void sbox_finalize(struct layout *lo)
{
    if (!lo->nsb)
        return;
    for (int i = 0; i < lo->nitems; i++) {
        const struct ditem *d = &lo->items[i];
        if (d->kind == DK_SBOX && d->sbi < lo->nsb) {
            struct sbox *b = &lo->sb[d->sbi];
            b->x = d->x, b->y = d->y, b->w = d->w, b->h = d->h;
            b->dyn = d->dyn;
        }
    }
    for (int k = 0; k < lo->nsb; k++)
        lo->sb[k].cw = lo->sb[k].x, lo->sb[k].ch = lo->sb[k].y;      /* the right and bottom edges, for now */
    for (int i = 0; i < lo->nitems; i++) {
        const struct ditem *d = &lo->items[i];
        if (!d->sb || d->w <= 0 || d->h <= 0 || d->x >= BIG || d->y >= BIG ||
            (d->kind == DK_FILL && !d->node) || (d->kind == DK_TEXT && d->len == 1 && d->text[0] == ' '))
            continue;
        struct sbox *b = &lo->sb[d->sb - 1];
        if (d->dyn != b->dyn)           /* in a sticky box of its own, or the like: not where it was laid out */
            continue;
        if (d->x + d->w > b->cw)
            b->cw = d->x + d->w;
        if (d->y + d->h > b->ch)
            b->ch = d->y + d->h;
    }
    for (int k = 0; k < lo->nsb; k++) {
        struct sbox *b = &lo->sb[k];
        b->cw -= b->x, b->ch -= b->y;
        if (b->cw <= b->w + 1)          /* a pixel of rounding is not overflow */
            b->cw = b->w;
        if (b->ch <= b->h + 1)
            b->ch = b->h;
        int mx = b->cw - b->w, my = b->ch - b->h;
        b->bar_x = b->ov_x == OV_SCROLL || (b->ov_x == OV_AUTO && mx > 0);
        b->bar_y = b->ov_y == OV_SCROLL || (b->ov_y == OV_AUTO && my > 0);
        /* the bars lie over the box, not beside it: once one axis scrolls, the other bar covers a little more */
        b->max_x = mx > 0 ? b->cw - (b->w - (b->bar_y ? SB_BAR : 0)) : 0;
        b->max_y = my > 0 ? b->ch - (b->h - (b->bar_x ? SB_BAR : 0)) : 0;
        b->active = mx > 0 || my > 0 || b->bar_x || b->bar_y;
        struct node *n = b->node;
        b->sx = n->scroll_x < 0 ? 0 : n->scroll_x > b->max_x ? b->max_x : n->scroll_x;
        b->sy = n->scroll_y < 0 ? 0 : n->scroll_y > b->max_y ? b->max_y : n->scroll_y;
        if (b->sx != n->scroll_x || b->sy != n->scroll_y) {
            n->scroll_x = b->sx;
            n->scroll_y = b->sy;
        }
    }
}

/*
 * How the scrolling boxes round an item (the sb of an item or hotspot) move it: (dx, dy) is what to add to its
 * position, clip the page rectangle that shows it (the page scrolled to psx, psy).
 */
void layout_sb_view(const struct layout *lo, int sb, int psx, int psy, int *dx, int *dy, struct rect *clip)
{
    int chain[32], n = 0;
    for (int k = sb; k > 0 && k <= lo->nsb && n < 32; k = lo->sb[k - 1].parent)
        chain[n++] = k - 1;
    int ox = 0, oy = 0;
    struct rect cl = mkrect(-BIG, -BIG, 2 * BIG, 2 * BIG);
    for (int i = n - 1; i >= 0; i--) {              /* the outermost first: it moves what is in it */
        const struct sbox *b = &lo->sb[chain[i]];
        if (!b->active)
            continue;
        int ddx = 0, ddy = 0;
        if (b->dyn)
            layout_dyn_offset(lo, b->dyn, psx, psy, &ddx, &ddy);
        cl = rect_and(cl, mkrect(b->x + ddx + ox, b->y + ddy + oy, b->w, b->h));
        ox -= b->sx, oy -= b->sy;
    }
    *dx = ox, *dy = oy;
    if (clip)
        *clip = cl;
}

/* The page rectangle a box's padding box is at (its own scrolling does not count), and what clips it. */
static struct rect sbox_rect(const struct layout *lo, int idx, int psx, int psy, struct rect *clip)
{
    const struct sbox *b = &lo->sb[idx];
    int ox = 0, oy = 0, ddx = 0, ddy = 0;
    struct rect cl;
    layout_sb_view(lo, b->parent, psx, psy, &ox, &oy, &cl);
    if (b->dyn)
        layout_dyn_offset(lo, b->dyn, psx, psy, &ddx, &ddy);
    if (clip)
        *clip = cl;
    return mkrect(b->x + ddx + ox, b->y + ddy + oy, b->w, b->h);
}

/* ---- colors and borders ---- */

static uint32_t shade(uint32_t c, int num, int den, int toward_white)
{
    uint32_t out = c & 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        int v = (int)(c >> sh & 0xFF);
        v = toward_white ? v + (255 - v) * num / den : v * num / den;
        out |= (uint32_t)v << sh;
    }
    return out;
}

static struct ditem *fill(struct lctx *c, int x, int y, int w, int h, uint32_t color, int dash)
{
    if (w <= 0 || h <= 0 || !ALPHA(color))
        return NULL;
    struct ditem *d = emit(c, DK_FILL);
    if (!d)
        return NULL;
    d->x = x, d->y = y, d->w = w, d->h = h;
    d->color = color;
    d->deco = (uint8_t)dash;
    return d;
}

static int len_px(struct len l, int base);

/* A box's rounded corners, w x h, into its item. */
static void round_corners(struct ditem *d, const struct style *st, int w, int h)
{
    int lim = (w < h ? w : h) / 2;
    if (lim > 2000)
        lim = 2000;
    for (int k = 0; k < 4; k++) {
        int r = len_px(st->radius[k], w < h ? w : h);
        d->rad[k] = r < 0 ? 0 : r > lim ? lim : r;
    }
}

/* Rounded corners and a gradient for a box's fill. */
static void decorate(struct ditem *d, const struct style *st, int w, int h)
{
    round_corners(d, st, w, h);
    if (st->grad) {
        d->grad = st->grad;
        d->g0 = st->g0;
        d->g1 = st->g1;
    }
}

/* The four sides of a border box. */
static void borders(struct lctx *c, const struct style *st, int x, int y, int w, int h)
{
    if (st->hidden)
        return;
    const int *bw = st->border_w;
    for (int side = 0; side < 4; side++) {
        int t = bw[side];
        if (!t)
            continue;
        uint32_t col = st->border_color[side];
        int bs = st->border_style[side];
        int dark_side = side == 0 || side == 3;         /* top and left */
        if (bs == BS_INSET || bs == BS_GROOVE)
            col = dark_side ? shade(col, 2, 3, 0) : shade(col, 2, 3, 1);
        else if (bs == BS_OUTSET || bs == BS_RIDGE)
            col = dark_side ? shade(col, 2, 3, 1) : shade(col, 2, 3, 0);
        int dash = bs == BS_DOTTED ? 1 : bs == BS_DASHED ? 2 : 0;
        switch (side) {
        case 0: fill(c, x, y, w, t, col, dash); break;
        case 2: fill(c, x, y + h - t, w, t, col, dash); break;
        case 3: fill(c, x, y + bw[0], t, h - bw[0] - bw[2], col, dash); break;
        case 1: fill(c, x + w - t, y + bw[0], t, h - bw[0] - bw[2], col, dash); break;
        }
    }
}

/*
 * Size a box's background (the item reserved at slot) and draw its
 * borders: as a ring round the background when the corners are rounded
 * and the border is the same all round.
 */
static void finish_box_inner(struct lctx *c, struct node *n, int slot, int x, int y, int w, int h);

/* outline: four strips outside the border box (further out by outline-offset); it takes no room. */
static void outline_box(struct lctx *c, const struct style *st, int x, int y, int w, int h)
{
    int t = st->outline_w;
    if (!t || st->outline_style == BS_NONE || st->hidden || w <= 0 || h <= 0)
        return;
    int o = st->outline_offset;
    x -= o + t, y -= o + t, w += 2 * (o + t), h += 2 * (o + t);
    if (w <= 2 * t || h <= 2 * t)
        return;
    uint32_t col = st->outline_color;
    int bs = st->outline_style;
    int dash = bs == BS_DOTTED ? 1 : bs == BS_DASHED ? 2 : 0;
    if (bs == BS_DOUBLE && t >= 3) {
        int th = t / 3 > 0 ? t / 3 : 1;
        fill(c, x, y, w, th, col, 0), fill(c, x, y + h - th, w, th, col, 0);
        fill(c, x, y + th, th, h - 2 * th, col, 0), fill(c, x + w - th, y + th, th, h - 2 * th, col, 0);
        x += t - th, y += t - th, w -= 2 * (t - th), h -= 2 * (t - th);
        t = th;
    }
    fill(c, x, y, w, t, col, dash);
    fill(c, x, y + h - t, w, t, col, dash);
    fill(c, x, y + t, t, h - 2 * t, col, dash);
    fill(c, x + w - t, y + t, t, h - 2 * t, col, dash);
}

static void finish_box(struct lctx *c, struct node *n, int slot, int x, int y, int w, int h)
{
    finish_box_inner(c, n, slot, x, y, w, h);
    bg_picture_finish(c, n, slot);
    if (n->style->outline_w)
        outline_box(c, n->style, x, y, w, h);
}

static void finish_box_inner(struct lctx *c, struct node *n, int slot, int x, int y, int w, int h)
{
    const struct style *st = n->style;
    if (slot < 0) {
        borders(c, st, x, y, w, h);
        return;
    }
    struct ditem *d = &c->lo->items[slot];
    int paint = !st->hidden && n != c->skip_bg;
    /* An unpainted background keeps the box (transparent) so clicks find the element. */
    d->x = x, d->y = y, d->w = w, d->h = h;
    d->color = paint && ALPHA(st->bg) ? st->bg : TRANSPARENT;
    d->node = n;
    if (!paint) {
        borders(c, st, x, y, w, h);
        return;
    }
    decorate(d, st, w, h);
    const int *bw = st->border_w;
    int bs = st->border_style[0];
    if ((d->rad[0] | d->rad[1] | d->rad[2] | d->rad[3]) && bw[0] && bw[0] == bw[1] &&
        bw[0] == bw[2] && bw[0] == bw[3] && st->border_color[0] == st->border_color[1] &&
        st->border_color[0] == st->border_color[2] && st->border_color[0] == st->border_color[3] &&
        bs != BS_DOTTED && bs != BS_DASHED && bs != BS_DOUBLE) {
        d->ring = bw[0];
        d->ring_color = st->border_color[0];
        return;
    }
    borders(c, st, x, y, w, h);
}

/* ---- background pictures ---- */

/* Does this box get a place reserved for a background picture (the item after its background)? */
static int bg_picture_wanted(const struct style *st)
{
    return st->bg_img && st->bg_img->w > 0 && st->bg_img->h > 0 && !st->hidden;
}

/*
 * The item after a box's background slot is its background picture (reserved when the box began): now that the
 * slot has its size, work out the size and place of the tile, and where it is painted. A gradient goes over the
 * picture, so it moves to this item.
 */
static void bg_picture_finish(struct lctx *c, struct node *n, int slot)
{
    struct layout *lo = c->lo;
    const struct style *st = n->style;
    if (slot < 0 || slot + 1 >= lo->nitems || lo->items[slot + 1].kind != DK_BGIMG)
        return;
    struct ditem *s0 = &lo->items[slot], *d = &lo->items[slot + 1];
    d->bgp = NULL;
    d->w = d->h = 0;
    d->grad = 0;
    d->color = TRANSPARENT;
    if (!bg_picture_wanted(st) || s0->w <= 0 || s0->h <= 0)
        return;
    int w = s0->w, h = s0->h;
    int ox = 0, oy = 0, ow = w, oh = h;                 /* the area the picture is placed in */
    if (st->bg_origin != 1) {
        ox = st->border_w[3], oy = st->border_w[0];
        ow = w - ox - st->border_w[1], oh = h - oy - st->border_w[2];
        if (st->bg_origin == 2) {
            int pl = len_px(st->padding[3], w), pt = len_px(st->padding[0], w);
            ox += pl, oy += pt;
            ow -= pl + len_px(st->padding[1], w), oh -= pt + len_px(st->padding[2], w);
        }
    }
    if (ow <= 0 || oh <= 0)
        return;
    const struct image *img = st->bg_img;
    int iw = img->w, ih = img->h, tw = ZOOM(iw), th = ZOOM(ih);
    switch (st->bg_size) {
    case BGS_COVER:
    case BGS_CONTAIN: {
        int wide = (long long)ow * ih > (long long)oh * iw;     /* the area is wider than the picture */
        if (wide == (st->bg_size == BGS_COVER))
            tw = ow, th = muldiv(ih, ow, iw);
        else
            th = oh, tw = muldiv(iw, oh, ih);
        break;
    }
    case BGS_LEN: {
        int lw = st->bg_w.unit == U_AUTO ? -1 : len_px(st->bg_w, ow);
        int lh = st->bg_h.unit == U_AUTO ? -1 : len_px(st->bg_h, oh);
        if (lw >= 0 && lh >= 0)
            tw = lw, th = lh;
        else if (lw >= 0)
            tw = lw, th = muldiv(ih, lw, iw);
        else if (lh >= 0)
            th = lh, tw = muldiv(iw, lh, ih);
        break;
    }
    default:
        break;
    }
    if (tw <= 0 || th <= 0)
        return;
    if (tw > 16384)
        tw = 16384;
    if (th > 16384)
        th = 16384;
    struct bgpaint *bp = arena_alloc(c->a, sizeof(*bp));
    if (!bp)
        return;
    bp->img = st->bg_img;
    bp->tw = tw, bp->th = th;
    bp->tx = ox + len_px(st->bg_px, ow - tw);
    bp->ty = oy + len_px(st->bg_py, oh - th);
    bp->norep_x = st->bg_norep_x, bp->norep_y = st->bg_norep_y;
    d->bgp = bp;
    d->x = s0->x, d->y = s0->y, d->w = w, d->h = h;
    round_corners(d, st, w, h);
    if (st->grad) {
        d->grad = st->grad;
        d->g0 = st->g0;
        d->g1 = st->g1;
        s0->grad = 0;
    }
}

/* ---- boxes ---- */

static int len_px(struct len l, int base)
{
    if (l.unit == U_PX)
        return l.v;
    if (l.unit == U_PCT) {
        int v = l.v > 100000 ? 100000 : l.v;
        return base > 20000 ? base / 10000 * v : base * v / 10000;
    }
    if (l.unit == U_CALC || l.unit == U_MIN || l.unit == U_MAX) {
        int p = len_px((struct len){ .v = l.pct, .unit = U_PCT }, base);
        if (l.unit == U_CALC)
            return l.v + p;
        return (l.unit == U_MIN) == (p < l.v) ? p : l.v;
    }
    return 0;
}

/* A length that depends on what it is in. */
static int relative(int unit)
{
    return unit == U_PCT || unit == U_CALC || unit == U_MIN || unit == U_MAX;
}

struct boxm {
    int mt, mr, mb, ml, bt, br, bb, bl, pt, pr, pb, pl, w;
};

static int hz(const struct boxm *b)
{
    return b->bl + b->br + b->pl + b->pr;
}

/* Margins, borders, padding and content width of a block in a containing block cw wide. */
static void resolve_box(const struct style *st, int cw, int force_bw, struct boxm *b)
{
    b->mt = len_px(st->margin[0], cw);
    b->mr = len_px(st->margin[1], cw);
    b->mb = len_px(st->margin[2], cw);
    b->ml = len_px(st->margin[3], cw);
    b->bt = st->border_w[0], b->br = st->border_w[1];
    b->bb = st->border_w[2], b->bl = st->border_w[3];
    b->pt = len_px(st->padding[0], cw);
    b->pr = len_px(st->padding[1], cw);
    b->pb = len_px(st->padding[2], cw);
    b->pl = len_px(st->padding[3], cw);
    int h = hz(b);
    if (force_bw >= 0) {
        b->ml = b->mr = 0;
        b->w = force_bw - h;
        if (b->w < 0)
            b->w = 0;
        return;
    }
    int auto_l = st->margin[3].unit == U_AUTO, auto_r = st->margin[1].unit == U_AUTO;
    int specified = st->width.unit != U_AUTO;
    int w;
    if (specified) {
        w = len_px(st->width, cw);
        if (st->box_sizing_border)
            w -= h;
    } else {
        w = cw - b->ml - b->mr - h;
    }
    if (st->max_width.unit != U_AUTO) {
        int mx = len_px(st->max_width, cw) - (st->box_sizing_border ? h : 0);
        if (w > mx) {
            w = mx;
            specified = 1;
        }
    }
    if (st->min_width.unit != U_AUTO) {
        int mn = len_px(st->min_width, cw) - (st->box_sizing_border ? h : 0);
        if (w < mn)
            w = mn;
    }
    if (w < 0)
        w = 0;
    if (specified) {
        int rest = cw - w - h - (auto_l ? 0 : b->ml) - (auto_r ? 0 : b->mr);
        if (auto_l && auto_r)
            b->ml = b->mr = rest > 0 ? rest / 2 : 0;
        else if (auto_l)
            b->ml = rest > 0 ? rest : 0;
        else if (auto_r)
            b->mr = rest;
    }
    b->w = w;
}

static int line_height(const struct style *st)
{
    switch (st->lh_kind) {
    case LH_FACTOR: return st->font_size * st->lh / 100;
    case LH_PX: return st->lh;
    default: return wfont_height(st->font);
    }
}

static int collapse(int a, int b)
{
    if (a >= 0 && b >= 0)
        return a > b ? a : b;
    if (a < 0 && b < 0)
        return a < b ? a : b;
    return a + b;
}

static int is_block_level(int d)
{
    return d == D_BLOCK || d == D_LIST_ITEM || d == D_TABLE || d == D_TABLE_ROW ||
           d == D_TABLE_ROW_GROUP || d == D_TABLE_CAPTION;
}

static int is_replaced(const struct node *n)
{
    return n->tag == T_IMG || n->tag == T_INPUT || n->tag == T_SELECT || n->tag == T_TEXTAREA ||
           n->tag == T_IFRAME || n->tag == T_SVG || n->tag == T_CANVAS || n->tag == T_PROGRESS || n->tag == T_METER;
}

/* ---- list markers ---- */

static void roman(int v, int upper, char *out)
{
    static const struct { int v; const char *s; } r[] = {
        { 1000, "m" }, { 900, "cm" }, { 500, "d" }, { 400, "cd" }, { 100, "c" }, { 90, "xc" },
        { 50, "l" }, { 40, "xl" }, { 10, "x" }, { 9, "ix" }, { 5, "v" }, { 4, "iv" }, { 1, "i" },
    };
    int n = 0;
    for (int i = 0; i < 13 && v > 0; i++)
        while (v >= r[i].v && n < 12) {
            for (const char *p = r[i].s; *p; p++)
                out[n++] = upper ? (char)(*p - 32) : *p;
            v -= r[i].v;
        }
    out[n] = 0;
}

static int list_number(const struct node *li)
{
    int k = 0;
    const struct node *list = li->parent;
    const char *start = list && list->tag == T_OL ? node_attr(list, "start") : NULL;
    int base = start ? atoi(start) : 1;
    for (const struct node *p = list ? list->first : NULL; p; p = p->next) {
        if (p->type != N_ELEMENT || !p->style || p->style->display != D_LIST_ITEM)
            continue;
        const char *val = node_attr(p, "value");
        if (val)
            base = atoi(val), k = 0;
        if (p == li)
            return base + k;
        k++;
    }
    return base + k;
}

static void start_marker(struct lctx *c, struct node *li, int content_x)
{
    const struct style *st = li->style;
    c->marker_pending = 0;
    c->inside_text[0] = 0;
    c->marker_img = st->ls_img && st->ls_img->w > 0 && st->ls_img->h > 0 ? st->ls_img : NULL;
    if ((st->list_style == LS_NONE && !c->marker_img) || st->hidden)
        return;
    c->marker_st = li->pseudo_st[PE_MARKER - 1] ? li->pseudo_st[PE_MARKER - 1] : st;
    c->marker_kind = st->list_style;
    c->marker_x = content_x;
    c->marker_text[0] = 0;
    int num = list_number(li);
    switch (st->list_style) {
    case LS_DECIMAL:
        snprintf(c->marker_text, sizeof(c->marker_text), "%d.", num);
        break;
    case LS_LOWER_ALPHA:
    case LS_UPPER_ALPHA:
        snprintf(c->marker_text, sizeof(c->marker_text), "%c.",
                 (st->list_style == LS_LOWER_ALPHA ? 'a' : 'A') + (num - 1 + 2600) % 26);
        break;
    case LS_LOWER_ROMAN:
    case LS_UPPER_ROMAN:
        roman(num > 0 && num < 4000 ? num : 1, st->list_style == LS_UPPER_ROMAN, c->marker_text);
        strcat(c->marker_text, ".");
        break;
    default:
        break;
    }
    if (c->marker_img) {                /* a picture instead: outside the box, whatever list-style-position says */
        c->marker_pending = 1;
        return;
    }
    if (st->list_inside) {              /* the marker is the first thing on the first line */
        const char *glyph = c->marker_text[0] ? c->marker_text
                          : st->list_style == LS_CIRCLE ? "o" : st->list_style == LS_SQUARE ? "-"
                          : "\xE2\x80\xA2";
        snprintf(c->inside_text, sizeof(c->inside_text), "%s ", glyph);
        c->inside_st = c->marker_st;
        return;
    }
    c->marker_pending = 1;
}

static void place_marker(struct lctx *c, int baseline)
{
    if (!c->marker_pending)
        return;
    c->marker_pending = 0;
    const struct style *st = c->marker_st;
    int size = st->font_size;
    if (c->marker_img) {
        int iw = ZOOM(c->marker_img->w), ih = ZOOM(c->marker_img->h), maxh = size * 3 / 2;
        if (ih > maxh) {
            iw = iw * maxh / ih;
            ih = maxh;
        }
        struct ditem *d = emit(c, DK_IMAGE);
        if (d) {
            d->x = c->marker_x - (iw < 1 ? 1 : iw) - size / 2;
            d->y = baseline - ih + size / 6;
            d->w = iw < 1 ? 1 : iw, d->h = ih < 1 ? 1 : ih;
            d->img = c->marker_img;
        }
        return;
    }
    if (c->marker_text[0]) {
        int w = wfont_width(st->font, c->marker_text, (int)strlen(c->marker_text));
        struct ditem *d = emit(c, DK_TEXT);
        if (!d)
            return;
        d->x = c->marker_x - w - size / 2;
        d->y = baseline - wfont_ascent(st->font);
        d->w = w;
        d->h = wfont_height(st->font);
        d->color = st->color;
        d->font = st->font;
        d->text = arena_strdup(c->a, c->marker_text);
        d->len = d->text ? (int)strlen(d->text) : 0;
        return;
    }
    int s = size * 35 / 100 < 4 ? 4 : size * 35 / 100;
    int cx = c->marker_x - size / 2 - s, cy = baseline - size * 3 / 10 - s / 2;
    if (c->marker_kind == LS_SQUARE) {
        fill(c, cx, cy, s, s, st->color, 0);
        return;
    }
    struct ditem *d = emit(c, c->marker_kind == LS_CIRCLE ? DK_CIRCLE : DK_DISC);
    if (d) {
        d->x = cx, d->y = cy, d->w = s, d->h = s;
        d->color = st->color;
    }
}

/* ---- inline runs ---- */

static struct item *push_item(struct flow *f, int kind, const struct style *st)
{
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 64;
        struct item *ni = realloc(f->items, sizeof(struct item) * (size_t)cap);
        if (!ni)
            return NULL;
        f->items = ni;
        f->cap = cap;
    }
    struct item *it = &f->items[f->n++];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->st = st;
    it->link = f->link;
    return it;
}

/* UTF-8 characters in s[0..n). */
static int utf8_count(const char *s, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        k += ((unsigned char)s[i] & 0xC0) != 0x80;
    return k;
}

/* Text width with letter-spacing, which goes after every character. */
static int text_width(const struct style *st, const char *s, int n)
{
    int w = wfont_width(st->font, s, n);
    if (st->letter_spacing)
        w += utf8_count(s, n) * st->letter_spacing;
    return w;
}

static int space_width(const struct style *st)
{
    return wfont_width(st->font, " ", 1) + st->letter_spacing + st->word_spacing;
}

static int wraps(const struct style *st)
{
    return st->white_space != WS_PRE && st->white_space != WS_NOWRAP;
}

static const char *transform(struct lctx *c, const struct style *st, const char *s, int n,
                             int word_start)
{
    if (st->text_transform == TT_NONE)
        return s;
    char *t = arena_strndup(c->a, s, (size_t)n);
    if (!t)
        return s;
    for (int i = 0; i < n; i++) {
        char ch = t[i];
        if (st->text_transform == TT_UPPER || (st->text_transform == TT_CAPITALIZE && i == 0 && word_start)) {
            if (ch >= 'a' && ch <= 'z')
                t[i] = (char)(ch - 32);
        } else if (st->text_transform == TT_LOWER) {
            if (ch >= 'A' && ch <= 'Z')
                t[i] = (char)(ch + 32);
        }
    }
    return t;
}

static void push_word(struct lctx *c, struct flow *f, const struct style *st, struct node *parent,
                      const char *text, int len, int word_start)
{
    struct item *it = push_item(f, I_WORD, st);
    if (!it)
        return;
    it->text = transform(c, st, text, len, word_start);
    it->len = len;
    it->w = text_width(st, it->text, len);
    it->node = parent;
}

/* A break opportunity that takes no room (like <wbr>). */
static void push_break_opportunity(struct flow *f, const struct style *st)
{
    struct item *it = push_item(f, I_SPACE, st);
    if (it) {
        it->wrap = 1;
        it->invisible = 1;
    }
}

/* Characters of scripts written without spaces: a line may break after each. */
static int cjk_char(int cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) ||
           (cp >= 0x20000 && cp <= 0x3FFFF);
}

/* Punctuation and small kana that may not start a line. */
static int no_break_before(int cp)
{
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B: case 0xFF01: case 0xFF1F:
    case 0xFF09: case 0xFF3D: case 0xFF5D: case 0x300D: case 0x300F: case 0x3011: case 0x3015: case 0x3009:
    case 0x300B: case 0x30FC: case 0x2026: case 0x30FB: case 0x3005: case 0x309D: case 0x309E: case 0x30FD:
    case 0x30FE: case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063: case 0x3083:
    case 0x3085: case 0x3087: case 0x308E: case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9:
    case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE: case 0x30F5: case 0x30F6: case 0xFF5E:
    case 0x3030: case 0xFF05: case 0x2019: case 0x201D:
        return 1;
    }
    return 0;
}

/* Opening brackets, which may not end one. */
static int no_break_after(int cp)
{
    switch (cp) {
    case 0xFF08: case 0xFF3B: case 0xFF5B: case 0x300C: case 0x300E: case 0x3010: case 0x3014: case 0x3008:
    case 0x300A: case 0x2018: case 0x201C:
        return 1;
    }
    return 0;
}

static int has_cjk(const char *s, int n)
{
    const char *end = s + n;
    while (s < end) {
        if ((unsigned char)*s < 0xE2) {
            s++;
            continue;
        }
        if (cjk_char(utf8_next(&s, end)))
            return 1;
    }
    return 0;
}

/* The end of the piece that starts at p: one character of CJK text with whatever clings to it, or a run of
 * anything else. */
static const char *cjk_piece_end(const char *p, const char *end)
{
    const char *q = p;
    int cp = utf8_next(&q, end), last = cp;
    if (!cjk_char(cp)) {
        while (q < end) {
            const char *r = q;
            cp = utf8_next(&r, end);
            if (cjk_char(cp))
                break;
            q = r;
        }
        return q;
    }
    while (q < end) {
        const char *r = q;
        cp = utf8_next(&r, end);
        if (no_break_before(cp) || no_break_after(last)) {
            q = r;
            last = cp;
            if (!cjk_char(cp))          /* a bracket before a Latin word: keep the word with it */
                while (q < end) {
                    r = q;
                    cp = utf8_next(&r, end);
                    if (cjk_char(cp))
                        break;
                    q = r;
                }
            continue;
        }
        break;
    }
    return q;
}

/* A word, as one item; or in pieces for small capitals (lower case letters become smaller upper
 * case ones), word-break: break-all (a chance to break after every character) and CJK text (the same,
 * but not where the rules of the script forbid it). */
static void emit_word(struct lctx *c, struct flow *f, const struct style *st, struct node *parent,
                      const char *text, int len, int word_start)
{
    int cjk = st->word_break != WB_BREAK_ALL && has_cjk(text, len);
    if (!st->small_caps && st->word_break != WB_BREAK_ALL && !cjk) {
        push_word(c, f, st, parent, text, len, word_start);
        return;
    }
    struct style *small = NULL;
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *q = p;
        int lower = st->small_caps && *p >= 'a' && *p <= 'z';
        if (st->word_break == WB_BREAK_ALL) {
            q++;
            while (q < end && ((unsigned char)*q & 0xC0) == 0x80)
                q++;
        } else if (cjk) {
            q = cjk_piece_end(p, end);
            lower = 0;
        } else {
            while (q < end && (st->small_caps && *q >= 'a' && *q <= 'z') == lower)
                q++;
        }
        if (lower) {
            if (!small && (small = arena_alloc(c->a, sizeof(*small)))) {
                *small = *st;
                small->font_size = st->font_size * 8 / 10 > 1 ? st->font_size * 8 / 10 : 1;
                small->font = wfont_get(st->family, st->bold, st->italic, small->font_size);
                small->text_transform = TT_NONE;
                small->small_caps = 0;
                if (!small->font)
                    small = NULL;
            }
            char *up = small ? arena_strndup(c->a, p, (size_t)(q - p)) : NULL;
            if (up) {
                for (char *u = up; *u; u++)
                    *u = (char)(*u - 32);
                push_word(c, f, small, parent, up, (int)(q - p), 0);
            } else {
                push_word(c, f, st, parent, p, (int)(q - p), 0);
            }
        } else {
            push_word(c, f, st, parent, p, (int)(q - p), word_start && p == text);
        }
        if (q < end && (st->word_break == WB_BREAK_ALL || cjk))
            push_break_opportunity(f, st);
        p = q;
    }
}

/* Text in st, from a text node (or generated content) inside `parent`. */
static void add_text(struct lctx *c, struct flow *f, const char *text, int tlen,
                     const struct style *st, struct node *parent)
{
    if (!st || !st->font)
        return;
    const char *s = text, *end = s + tlen;
    int ws = st->white_space;
    int collapse_spaces = ws == WS_NORMAL || ws == WS_NOWRAP || ws == WS_PRE_LINE;
    int keep_newlines = ws == WS_PRE || ws == WS_PRE_WRAP || ws == WS_PRE_LINE;
    int sw = space_width(st), col = 0;
    while (s < end) {
        char ch = *s;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
            if (collapse_spaces) {
                int newlines = 0;
                while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f'))
                    newlines += *s++ == '\n';
                if (keep_newlines && newlines) {
                    for (int k = 0; k < newlines; k++)
                        push_item(f, I_BREAK, st);
                    f->space_before = 1;
                } else if (!f->space_before) {
                    struct item *it = push_item(f, I_SPACE, st);
                    if (it) {
                        it->w = sw;
                        it->wrap = (uint8_t)wraps(st);
                    }
                    f->space_before = 1;
                }
                continue;
            }
            /* Preserved white space: newlines break, spaces and tabs keep their width. */
            if (ch == '\n') {
                push_item(f, I_BREAK, st);
                col = 0;
                s++;
                continue;
            }
            if (ch == '\r') {
                s++;
                continue;
            }
            int w = 0;
            while (s < end && (*s == ' ' || *s == '\t')) {
                int ts = st->tab_size ? st->tab_size : 8;
                int adv = *s == '\t' ? ts - col % ts : 1;
                w += adv * sw;
                col += adv;
                s++;
            }
            struct item *it = push_item(f, I_SPACE, st);
            if (it) {
                it->w = w;
                it->wrap = ws == WS_PRE_WRAP;
            }
            f->space_before = 0;
            continue;
        }
        const char *w0 = s;
        while (s < end && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != '\f')
            s++;
        int len = (int)(s - w0);
        emit_word(c, f, st, parent, w0, len, w0 == text || f->space_before);
        col += len;
        f->space_before = 0;
    }
}

/* ---- replaced elements and form controls ---- */

static const char *input_type(const struct node *n)
{
    const char *t = node_attr(n, "type");
    return t ? t : "text";
}

static int is_button_input(const char *t)
{
    return !strcasecmp(t, "submit") || !strcasecmp(t, "reset") || !strcasecmp(t, "button") ||
           !strcasecmp(t, "image") || !strcasecmp(t, "file");
}

const char *control_label(const struct node *n)
{
    if (n->tag == T_INPUT) {
        const char *t = input_type(n), *v = node_attr(n, "value");
        if (!strcasecmp(t, "file"))
            return "Browse...";
        if (!strcasecmp(t, "image"))
            return node_attr(n, "alt") ? node_attr(n, "alt") : "Submit";
        if (v)
            return v;
        return !strcasecmp(t, "reset") ? "Reset" : !strcasecmp(t, "submit") ? "Submit" : "";
    }
    if (n->tag == T_SELECT) {
        int k = 0;
        for (const struct node *o = n; o; o = node_next((struct node *)o, n)) {
            if (o->type != N_ELEMENT || o->tag != T_OPTION)
                continue;
            if (k++ == n->checked)
                return o->value ? o->value : "";
        }
    }
    return "";
}

/* A canvas's width or height attribute as a number of pixels (def when it is missing or not one). */
static int canvas_attr(const struct node *n, const char *name, int def)
{
    const char *v = node_attr(n, name);
    if (!v || *v < '0' || *v > '9')
        return def;
    int x = 0;
    while (*v >= '0' && *v <= '9' && x < 100000)
        x = x * 10 + (*v++ - '0');
    return x;
}

/*
 * Lay out an image or a form control with its margin box at (0, 0).
 * Sets its margin-box size and baseline.
 */
static void layout_replaced(struct lctx *c, struct node *n, int avail, int *ow, int *oh, int *obase)
{
    const struct style *st = n->style;
    struct boxm b;
    resolve_box(st, avail, -1, &b);
    b.ml = len_px(st->margin[3], avail);       /* auto margins are the caller's business */
    b.mr = len_px(st->margin[1], avail);
    int w = st->width.unit == U_PX ? st->width.v
          : relative(st->width.unit) && !c->measuring ? len_px(st->width, avail) : -1;
    int h = st->height.unit == U_PX ? st->height.v : -1;
    if (st->box_sizing_border) {
        if (w >= 0)
            w -= hz(&b);
        if (h >= 0)
            h -= b.bt + b.bb + b.pt + b.pb;
    }
    const struct wfont *f = st->font;
    int fh = wfont_height(f);
    int kind = DK_CONTROL, ctrl = 1;
    const char *alt = NULL;

    if (n->tag == T_IMG) {
        ctrl = 0;
        struct image *img = n->img;
        alt = node_attr(n, "alt");
        if (img && img->w > 0 && img->h > 0) {
            if (w < 0 && h < 0)
                w = ZOOM(img->w), h = ZOOM(img->h);
            else if (w < 0)
                w = h * img->w / img->h;
            else if (h < 0)
                h = w * img->h / img->w;
            if (st->max_width.unit == U_PX || (relative(st->max_width.unit) && !c->measuring)) {
                int mx = len_px(st->max_width, avail);
                if (w > mx && mx > 0) {
                    h = h * mx / w;
                    w = mx;
                }
            }
            kind = DK_IMAGE;
        } else {
            kind = DK_FRAME;
            if (w < 0 && h < 0 && alt && *alt) {
                w = wfont_width(f, alt, (int)strlen(alt)) + 6;
                h = fh + 4;
            }
            if (w < 0)
                w = h > 0 ? h : 0;
            if (h < 0)
                h = w > 0 && alt && *alt ? fh + 4 : w;
        }
    } else if (n->tag == T_SVG) {       /* drawn by svg.c at the size it ends up */
        ctrl = 0;
        int iw, ih;
        svg_size(n, &iw, &ih);
        iw = ZOOM(iw), ih = ZOOM(ih);
        if (w < 0 && h < 0)
            w = iw, h = ih;
        else if (w < 0)
            w = h * iw / ih;
        else if (h < 0)
            h = w * ih / iw;
        if (st->max_width.unit == U_PX || (relative(st->max_width.unit) && !c->measuring)) {
            int mx = len_px(st->max_width, avail);
            if (w > mx && mx > 0) {
                h = h * mx / w;
                w = mx;
            }
        }
        kind = DK_IMAGE;
    } else if (n->tag == T_CANVAS) {    /* its pixels are n->img (jscanvas.c), 300 x 150 unless it says */
        ctrl = 0;
        int iw = ZOOM(canvas_attr(n, "width", 300)), ih = ZOOM(canvas_attr(n, "height", 150));
        if (w < 0 && h < 0)
            w = iw, h = ih;
        else if (w < 0)
            w = ih ? h * iw / ih : 0;
        else if (h < 0)
            h = iw ? w * ih / iw : 0;
        if (st->max_width.unit == U_PX || (relative(st->max_width.unit) && !c->measuring)) {
            int mx = len_px(st->max_width, avail);
            if (w > mx && mx > 0) {
                h = h * mx / w;
                w = mx;
            }
        }
        kind = DK_IMAGE;
    } else if (n->tag == T_INPUT) {
        const char *t = input_type(n);
        if (!strcasecmp(t, "checkbox") || !strcasecmp(t, "radio")) {
            w = w < 0 ? ZOOM(13) : w;
            h = h < 0 ? ZOOM(13) : h;
            if (st->margin[1].unit == U_PX && !st->margin[1].v && !st->margin[3].v)
                b.ml = b.mr = 3;
        } else if (!strcasecmp(t, "range")) {
            if (w < 0)
                w = ZOOM(129);
            if (h < 0)
                h = ZOOM(18);
        } else if (!strcasecmp(t, "color")) {
            if (w < 0)
                w = ZOOM(48);
            if (h < 0)
                h = fh + 6;
        } else if (is_button_input(t)) {
            const char *label = control_label(n);
            if (w < 0)
                w = wfont_width(f, label, (int)strlen(label)) + ZOOM(16) +
                    (!strcasecmp(t, "file") ? ZOOM(6 + 150) : 0);       /* a file's name goes beside its button */
            if (h < 0)
                h = fh + 6;
        } else {
            if (w < 0)
                w = ZOOM(150);
            if (h < 0)
                h = fh + 6;
        }
    } else if (n->tag == T_SELECT) {
        if (w < 0) {
            w = ZOOM(40);
            for (const struct node *o = n; o; o = node_next((struct node *)o, n))
                if (o->type == N_ELEMENT && o->tag == T_OPTION && o->value) {
                    int ow2 = wfont_width(f, o->value, (int)strlen(o->value)) + 28;
                    if (ow2 > w)
                        w = ow2;
                }
            if (w > avail && avail > 60)
                w = avail;
        }
        if (h < 0)
            h = fh + 6;
    } else if (n->tag == T_PROGRESS || n->tag == T_METER) {     /* a gauge, drawn by draw_control */
        ctrl = 0;
        if (w < 0)
            w = ZOOM(n->tag == T_PROGRESS ? 160 : 80);
        if (h < 0)
            h = ZOOM(16);
    } else if (n->tag == T_IFRAME) {    /* its page is drawn by the browser */
        ctrl = 0;
        kind = DK_IFRAME;
        if (w < 0)
            w = ZOOM(300);
        if (h < 0)
            h = ZOOM(150);
    } else {                            /* textarea */
        if (w < 0)
            w = ZOOM(168);
        if (h < 0)
            h = 2 * fh + 6;
    }
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;

    int bx = b.ml, by = b.mt;
    int bw = b.bl + b.pl + w + b.pr + b.br, bh = b.bt + b.pt + h + b.pb + b.bb;
    *ow = b.ml + bw + b.mr;
    *oh = b.mt + bh + b.mb;
    *obase = *oh;
    if (n->tag != T_IMG && n->tag != T_SVG && n->tag != T_CANVAS && !(n->tag == T_INPUT && h <= 13))
        *obase = b.mt + b.bt + b.pt + (h - fh) / 2 + wfont_ascent(f) + 1;
    if (c->measuring || st->hidden)
        return;

    if (n->tag == T_IMG || n->tag == T_SVG || n->tag == T_CANVAS)
        fill(c, bx + b.bl, by + b.bt, b.pl + w + b.pr, b.pt + h + b.pb, st->bg, 0);
    borders(c, st, bx, by, bw, bh);
    outline_box(c, st, bx, by, bw, bh);
    if (w <= 0 || h <= 0)
        return;
    if (n->tag == T_CANVAS && !n->img)
        return;                         /* nothing drawn on it yet */
    if (n->tag == T_SVG) {
        if (!n->img || n->img->w != w || n->img->h != h) {
            image_free(n->img);
            n->img = svg_render(n, w, h, st->color | 0xFF000000u);
        }
        if (!n->img)
            kind = DK_FRAME;
    }
    struct ditem *d = emit(c, kind);
    if (!d)
        return;
    d->x = bx + b.bl + b.pl;
    d->y = by + b.bt + b.pt;
    d->w = w;
    d->h = h;
    d->img = n->img;
    d->node = n;
    d->font = f;
    d->color = st->color;
    if (kind == DK_IMAGE) {
        round_corners(d, st, w, h);
        if (st->obj_fit != OF_FILL && n->tag == T_IMG && n->img && n->img->w > 0 && n->img->h > 0) {
            int iw = n->img->w, ih = n->img->h, fw = ZOOM(iw), fh = ZOOM(ih);
            int fit = st->obj_fit;
            if (fit == OF_SCALE_DOWN)
                fit = iw <= w && ih <= h ? OF_NONE : OF_CONTAIN;
            if (fit == OF_CONTAIN || fit == OF_COVER) {
                int wide = (long long)iw * h > (long long)ih * w;      /* the picture is wider than the box */
                if (wide == (fit == OF_CONTAIN))
                    fw = w, fh = muldiv(ih, w, iw);
                else
                    fh = h, fw = muldiv(iw, h, ih);
            }
            if (fw < 1)
                fw = 1;
            if (fh < 1)
                fh = 1;
            d->fit_w = fw, d->fit_h = fh;
            d->fit_x = len_px(st->obj_px, w - fw);
            d->fit_y = len_px(st->obj_py, h - fh);
        }
    }
    if (kind == DK_FRAME && alt && *alt) {
        struct ditem *t = emit(c, DK_TEXT);
        if (t) {
            int len = (int)strlen(alt);
            while (len > 0 && wfont_width(f, alt, len) > w - 4)
                len--;
            t->x = d->x + 3, t->y = d->y + (h - fh) / 2;
            t->w = wfont_width(f, alt, len), t->h = fh;
            t->font = f;
            t->text = alt;
            t->len = len;
            t->color = st->color;
        }
    }
    if (ctrl)
        add_hotspot(c, &c->lo->controls, &c->lo->ncontrols, &c->lo->controlcap, n, d->x, d->y, w, h);
}

/* An inline-block (or float, or flex item) with its margin box at (0, 0). */
static void layout_inline_block(struct lctx *c, struct node *n, int avail, int *ow, int *oh,
                                int *obase)
{
    const struct style *st = n->style;
    struct boxm b;
    resolve_box(st, avail, -1, &b);
    /* Auto margins are zero on an inline-block. */
    int ml = len_px(st->margin[3], avail), mr = len_px(st->margin[1], avail);
    int mt = len_px(st->margin[0], avail);
    int bw;
    if (c->abs_bw1) {                   /* an absolute box stretched between its sides */
        bw = c->abs_bw1 - 1;
        c->abs_bw1 = 0;
    } else if (st->width.unit == U_AUTO) {     /* shrink to fit */
        int mn, mx;
        measure(c, n, &mn, &mx);
        int stf = mx < avail ? mx : avail;
        if (stf < mn)
            stf = mn;
        bw = stf - ml - mr;
    } else {
        bw = b.w + hz(&b);
    }
    if (bw < 0)
        bw = 0;
    int saved_pending = c->marker_pending;
    c->marker_pending = 0;
    c->last_baseline = -1;
    int mb, h;
    if (st->display == D_INLINE_TABLE || st->display == D_TABLE) {
        h = layout_table(c, n, ml, mt, avail, bw, &mb);
    } else {
        c->force_container = 1;         /* what is absolute inside is placed against this box */
        h = layout_block_box(c, n, ml, mt, avail, bw, &mb, NULL);
    }
    c->marker_pending = saved_pending;
    if (n->tag == T_BUTTON && !st->hidden)
        add_hotspot(c, &c->lo->controls, &c->lo->ncontrols, &c->lo->controlcap, n, ml, mt, bw, h);
    *ow = ml + bw + mr;
    *oh = mt + h + mb;
    *obase = c->last_baseline >= 0 ? c->last_baseline : *oh;
}

static void add_atomic(struct lctx *c, struct flow *f, struct node *n)
{
    struct item *it = push_item(f, I_ATOMIC, n->style);
    if (!it)
        return;
    it->node = n;
    f->space_before = 0;
    if (c->measuring) {
        int mn, mx;
        measure(c, n, &mn, &mx);
        it->w = mx;
        it->wmin = mn;
        return;
    }
    int idx = f->n - 1;
    struct mark m0 = mark_now(c);
    int w, h, base;
    if (is_replaced(n))
        layout_replaced(c, n, f->w, &w, &h, &base);
    else
        layout_inline_block(c, n, f->w, &w, &h, &base);
    it = &f->items[idx];                /* the run may have moved while laying it out */
    it->m0 = m0;
    it->m1 = mark_now(c);
    it->w = w;
    it->h = h;
    it->base = base;
}

/* ---- floats ---- */

/* Narrow [*left, *right) by the floats that meet y .. y + h; 1 if there are any. */
static int float_edges(struct lctx *c, int y, int h, int *left, int *right)
{
    int hit = 0;
    for (int i = c->float_base; i < c->nfloats; i++) {
        const struct fbox *b = &c->floats[i];
        if (b->y + b->h <= y || b->y >= y + (h > 0 ? h : 1))
            continue;
        hit = 1;
        if (b->side == 1) {
            if (b->x + b->w > *left)
                *left = b->x + b->w;
        } else if (b->x < *right) {
            *right = b->x;
        }
    }
    return hit;
}

/* The nearest bottom edge below y among the floats that meet y .. y + h; 0 if none. */
static int float_next_bottom(struct lctx *c, int y, int h)
{
    int best = 0;
    for (int i = c->float_base; i < c->nfloats; i++) {
        const struct fbox *b = &c->floats[i];
        int bottom = b->y + b->h;
        if (bottom <= y || b->y >= y + (h > 0 ? h : 1))
            continue;
        if (!best || bottom < best)
            best = bottom;
    }
    return best;
}

/* The lowest bottom edge of the floats on the sides in mask (1 left, 2 right); -BIG if none. */
static int float_clear_y(struct lctx *c, int mask)
{
    int low = -BIG;
    for (int i = c->float_base; i < c->nfloats; i++)
        if ((c->floats[i].side & mask) && c->floats[i].y + c->floats[i].h > low)
            low = c->floats[i].y + c->floats[i].h;
    return low;
}

/* Lay out a float and put it against the side of the container, below what is already there. */
static void place_float(struct lctx *c, struct flow *f, struct node *n)
{
    const struct style *st = n->style;
    struct mark m0 = mark_now(c);
    int w, h, base;
    if (is_replaced(n))
        layout_replaced(c, n, f->w, &w, &h, &base);
    else
        layout_inline_block(c, n, f->w, &w, &h, &base);
    struct mark m1 = mark_now(c);
    int y = f->cy + f->pm;
    if (st->clear && float_clear_y(c, st->clear) > y)
        y = float_clear_y(c, st->clear);
    for (int i = c->float_base; i < c->nfloats; i++)        /* never higher than an earlier float */
        if (c->floats[i].y > y)
            y = c->floats[i].y;
    int left, right;
    for (int tries = 0; tries < 64; tries++) {
        left = f->x;
        right = f->x + f->w;
        int hit = float_edges(c, y, h, &left, &right);
        int nb = hit && right - left < w ? float_next_bottom(c, y, h) : 0;
        if (nb <= y)
            break;
        y = nb;
    }
    int x = st->floated == 2 ? right - w : left;
    shift(c, m0, m1, x, y);
    if (c->nfloats < (int)(sizeof(c->floats) / sizeof(c->floats[0]))) {
        c->floats[c->nfloats++] = (struct fbox){ x, y, w, h, st->floated };
    }
}

/* Width of the run from item i up to its first place to break. */
static int first_chunk(const struct item *items, int i, int n)
{
    int w = 0;
    for (; i < n; i++) {
        if (items[i].kind == I_BREAK || (items[i].kind == I_SPACE && items[i].wrap))
            break;
        w += items[i].w;
    }
    return w;
}

/* ---- lines ---- */

/* How far below the baseline sub and super move text (negative: up). */
static int va_shift(const struct style *st)
{
    if (st->vertical_align == VA_SUB)
        return st->font_size / 4;
    if (st->vertical_align == VA_SUPER)
        return -st->font_size / 3;
    return 0;
}

static void open_box_rect(struct lctx *c, const struct style *st, int x0, int x1, int baseline)
{
    if (st->hidden || x1 <= x0)
        return;
    const struct wfont *f = st->font;
    int asc = wfont_ascent(f), desc = wfont_height(f) - asc;
    int pt = len_px(st->padding[0], 0), pb = len_px(st->padding[2], 0);
    int top = baseline - asc - pt - st->border_w[0];
    int bottom = baseline + desc + pb + st->border_w[2];
    struct ditem *d = fill(c, x0, top, x1 - x0, bottom - top, st->bg, 0);
    if (d)
        decorate(d, st, x1 - x0, bottom - top);
    if (st->border_w[0] || st->border_w[1] || st->border_w[2] || st->border_w[3])
        borders(c, st, x0, top, x1 - x0, bottom - top);
}

/*
 * text-overflow: ellipsis. A line of a clipping box that is wider than the box ends in "..." where
 * the room runs out: the word that crosses the limit is cut, and what follows is dropped. Lines
 * with a picture or an inline-block at or after that place are left alone (their boxes are
 * already laid out). Returns the new end of the line.
 */
static int ellipsize_line(struct lctx *c, struct flow *f, int from, int to, int avail)
{
    struct item *items = f->items;
    const struct style *cs = f->st;
    int dots = text_width(cs, "...", 3), limit = avail - dots, x = 0, k = from;
    if (limit < 0)
        limit = 0;
    while (k < to && x + items[k].w <= limit)
        x += items[k++].w;
    for (int i = k; i < to; i++)
        if (items[i].kind == I_ATOMIC)
            return to;
    if (k >= to)
        return to;
    struct item *it = &items[k];
    if (it->kind == I_WORD) {
        int keep = 0, w = 0;
        while (keep < it->len) {
            int e = keep + 1;
            while (e < it->len && ((unsigned char)it->text[e] & 0xC0) == 0x80)
                e++;
            int cw = text_width(it->st, it->text + keep, e - keep);
            if (x + w + cw > limit)
                break;
            w += cw;
            keep = e;
        }
        char *cut = arena_alloc(c->a, (size_t)keep + 4);
        if (!cut)
            return to;
        memcpy(cut, it->text, (size_t)keep);
        memcpy(cut + keep, "...", 4);
        it->text = cut;
        it->len = keep + 3;
        it->w = text_width(it->st, cut, it->len);
    } else {
        it->kind = I_WORD;
        it->text = "...";
        it->len = 3;
        it->w = text_width(it->st, "...", 3);
        it->invisible = 0;
        it->wrap = 0;
    }
    return k + 1;
}

/*
 * word-break: break-word and overflow-wrap: a word wider than a whole line goes in pieces of at most
 * that width, with a chance to break between them.
 */
static void split_long_words(struct flow *f, int avail)
{
    int extra = 0;
    for (int i = 0; i < f->n; i++)
        if (f->items[i].kind == I_WORD && f->items[i].st->word_break == WB_BREAK_WORD &&
            f->items[i].w > avail && avail > 0)
            extra += f->items[i].len * 2;
    if (!extra)
        return;
    struct item *out = malloc(sizeof(struct item) * (size_t)(f->n + extra));
    if (!out)
        return;
    int n = 0;
    for (int i = 0; i < f->n; i++) {
        const struct item *it = &f->items[i];
        if (!(it->kind == I_WORD && it->st->word_break == WB_BREAK_WORD && it->w > avail && avail > 0)) {
            out[n++] = *it;
            continue;
        }
        int start = 0, w = 0, pos = 0;
        while (pos < it->len) {
            int e = pos + 1;
            while (e < it->len && ((unsigned char)it->text[e] & 0xC0) == 0x80)
                e++;
            int cw = text_width(it->st, it->text + pos, e - pos);
            if (w + cw > avail && pos > start) {
                out[n] = *it;
                out[n].text = it->text + start;
                out[n].len = pos - start;
                out[n].w = w;
                n++;
                out[n] = *it;
                out[n].kind = I_SPACE;
                out[n].wrap = 1;
                out[n].invisible = 1;
                out[n].w = 0;
                out[n].text = NULL;
                out[n].len = 0;
                n++;
                start = pos;
                w = 0;
            }
            w += cw;
            pos = e;
        }
        out[n] = *it;
        out[n].text = it->text + start;
        out[n].len = it->len - start;
        out[n].w = w;
        n++;
    }
    free(f->items);
    f->items = out;
    f->n = n;
    f->cap = n + extra;
}

static void place_line(struct lctx *c, struct flow *f, int from, int to)
{
    struct item *items = f->items;
    int indent = f->lines == 0 ? f->st->text_indent : 0;
    int avail = f->w - indent;
    int lw = 0;
    for (int i = from; i < to; i++)
        lw += items[i].w;
    if (f->st->text_ellipsis && f->st->clips && lw > avail && avail > 0) {
        to = ellipsize_line(c, f, from, to, avail);
        lw = 0;
        for (int i = from; i < to; i++)
            lw += items[i].w;
    }

    /* Vertical extent: the container's strut, then every item. */
    const struct style *cs = f->st;
    int lh = line_height(cs), fa = wfont_ascent(cs->font), fh = wfont_height(cs->font);
    int above = fa + (lh - fh) / 2, below = lh - above;
    int xh = cs->font_size * 45 / 100;
    for (int i = from; i < to; i++) {
        struct item *it = &items[i];
        if (it->kind == I_WORD || (it->kind == I_SPACE && !it->invisible)) {
            int ilh = line_height(it->st), ia = wfont_ascent(it->st->font);
            int a = ia + (ilh - wfont_height(it->st->font)) / 2, d = ilh - a;
            int sh = va_shift(it->st);
            if (a - sh > above)
                above = a - sh;
            if (d + sh > below)
                below = d + sh;
        } else if (it->kind == I_ATOMIC) {
            int top = it->base, bot = it->h - it->base;
            if (it->st->vertical_align == VA_MIDDLE) {
                top = it->h / 2 + xh / 2;
                bot = it->h - top;
            }
            if (top > above)
                above = top;
            if (bot > below)
                below = bot;
        }
    }
    int line_top = f->cy, baseline = line_top + above, line_h = above + below;

    int x = f->x + indent;
    int align = f->last_line && cs->text_align_last ? cs->text_align_last - 1 : cs->text_align;
    if (align == TA_CENTER)
        x += (avail - lw) / 2;
    else if (align == TA_RIGHT)
        x += avail - lw;
    if (x < f->x && align != TA_LEFT)
        x = f->x;
    int line_left = x;

    /* Pass 1: positions, and the backgrounds of inline elements behind the text. */
    for (int k = 0; k < f->nopen; k++)
        f->open[k].x = line_left;
    for (int i = from; i < to; i++) {
        struct item *it = &items[i];
        it->x = x;
        if (it->kind == I_OPEN) {
            const struct style *st = it->st;
            int ml = len_px(st->margin[3], f->w);
            anchors_of(c, it->node, line_top);
            if ((ALPHA(st->bg) || st->border_w[0] || st->border_w[2]) && f->nopen < 16)
                f->open[f->nopen++] = (struct open_el){ st, x + ml };
        } else if (it->kind == I_CLOSE) {
            for (int k = f->nopen - 1; k >= 0; k--) {
                if (f->open[k].st != it->st)
                    continue;
                const struct style *st = it->st;
                int edge = x + len_px(st->padding[1], f->w) + st->border_w[1];
                open_box_rect(c, st, f->open[k].x, edge, baseline);
                memmove(&f->open[k], &f->open[k + 1], sizeof(f->open[0]) * (size_t)(f->nopen - k - 1));
                f->nopen--;
                break;
            }
        }
        x += it->w;
    }
    for (int k = 0; k < f->nopen; k++)          /* still open: up to the end of the line */
        open_box_rect(c, f->open[k].st, f->open[k].x, x, baseline);

    /* Pass 2: text, atomic boxes and links. */
    for (int i = from; i < to; i++) {
        struct item *it = &items[i];
        const struct style *st = it->st;
        if (it->kind == I_WORD || (it->kind == I_SPACE && !it->invisible && (st->decoration || it->link))) {
            int sh = va_shift(st);
            if (!st->hidden && (it->kind == I_WORD || st->decoration)) {
                struct ditem *d = emit(c, DK_TEXT);
                if (d) {
                    d->x = it->x;
                    d->y = baseline + sh - wfont_ascent(st->font);
                    d->w = it->w;
                    d->h = wfont_height(st->font);
                    d->font = st->font;
                    d->text = it->kind == I_WORD ? it->text : " ";
                    d->len = it->kind == I_WORD ? it->len : 1;
                    d->color = st->color;
                    d->deco = st->decoration;
                    d->ls = (int8_t)st->letter_spacing;
                    if (st->ts_color) {
                        d->sh_color = st->ts_color;
                        d->sh_x = (int8_t)st->ts_x, d->sh_y = (int8_t)st->ts_y;
                        d->sh_blur = (int8_t)st->ts_blur;
                    }
                    d->deco_color = st->deco_color;
                    d->deco_style = st->deco_style;
                    int fs = wfont_size(st->font);
                    struct len t = st->deco_thickness, u = st->underline_offset;
                    d->deco_thickness = t.unit == U_AUTO ? 0 : t.unit == U_PCT ? fs * t.v / 10000 : t.v;
                    if (t.unit != U_AUTO && d->deco_thickness < 1) d->deco_thickness = 1;
                    d->underline_offset = u.unit == U_AUTO ? (-2147483647-1) : u.unit == U_PCT ? fs * u.v / 10000 : u.v;
                    d->node = it->node;         /* the element the text is in: for clicks */
                    if (it->kind == I_SPACE && it->w > d->w)
                        d->w = it->w;
                }
            }
            if (it->link)
                add_hotspot(c, &c->lo->links, &c->lo->nlinks, &c->lo->linkcap, it->link, it->x,
                            line_top, it->w, line_h);
        } else if (it->kind == I_ATOMIC) {
            int top = baseline - it->base;
            if (st->vertical_align == VA_MIDDLE)
                top = baseline - xh / 2 - it->h / 2;
            shift(c, it->m0, it->m1, it->x, top);
            if (it->link)
                add_hotspot(c, &c->lo->links, &c->lo->nlinks, &c->lo->linkcap, it->link, it->x, top,
                            it->w, it->h);
        }
    }
    place_marker(c, baseline);
    c->last_baseline = baseline;
    f->cy += line_h;
    f->lines++;
}

static int is_content(const struct item *it)
{
    return it->kind == I_WORD || it->kind == I_ATOMIC || (it->kind == I_SPACE && !it->wrap && it->w);
}

static void measure_run(struct flow *f)
{
    int line = f->st->text_indent, group = line;
    for (int i = 0; i < f->n; i++) {
        struct item *it = &f->items[i];
        switch (it->kind) {
        case I_BREAK:
            if (line > f->max)
                f->max = line;
            if (group > f->min)
                f->min = group;
            line = group = 0;
            break;
        case I_SPACE:
            if (it->wrap) {
                if (group > f->min)
                    f->min = group;
                group = 0;
            } else {
                group += it->w;
            }
            line += it->w;
            break;
        case I_ATOMIC:
            group += it->wmin;
            line += it->w;
            break;
        default:
            group += it->w;
            line += it->w;
            break;
        }
    }
    if (line > f->max)
        f->max = line;
    if (group > f->min)
        f->min = group;
}

static void flush_run(struct lctx *c, struct flow *f)
{
    if (!f->n)
        return;
    if (c->measuring) {
        measure_run(f);
        f->n = 0;
        f->space_before = 1;
        return;
    }
    int any = 0;
    for (int i = 0; i < f->n && !any; i++)
        any = is_content(&f->items[i]) || f->items[i].kind == I_BREAK;
    if (any) {
        f->cy += f->pm;
        f->pm = 0;
    }
    split_long_words(f, f->w - (f->lines == 0 ? f->st->text_indent : 0));
    struct item *items = f->items;
    int n = f->n, i = 0;
    int ox = f->x, ow = f->w;           /* the container; a line beside floats is narrower */
    while (any && i < n) {
        /* Collapsible spaces at the start of a line go. */
        while (i < n && items[i].kind == I_SPACE && items[i].wrap)
            i++;
        if (i >= n)
            break;
        f->x = ox;
        f->w = ow;
        if (c->nfloats > c->float_base) {
            int probe = line_height(f->st), need = first_chunk(items, i, n);
            for (int tries = 0; tries < 64; tries++) {
                int l = ox, r = ox + ow;
                int hit = float_edges(c, f->cy, probe, &l, &r);
                int nb = hit && r - l < need ? float_next_bottom(c, f->cy, probe) : 0;
                if (nb > f->cy) {
                    f->cy = nb;         /* too narrow: go down past the float */
                    continue;
                }
                f->x = l;
                f->w = r - l;
                break;
            }
        }
        int avail = f->w - (f->lines == 0 ? f->st->text_indent : 0);
        int start = i, x = 0, brk = -1, end = n, next = n, forced = 0;
        for (; i < n; i++) {
            struct item *it = &items[i];
            if (it->kind == I_BREAK) {
                end = i;
                next = i + 1;
                forced = 1;
                break;
            }
            if (it->kind == I_SPACE && it->wrap) {
                if (x > avail && x > 0) {       /* a long word overflowed: break after it */
                    end = i;
                    next = i + 1;
                    break;
                }
                brk = i;
            } else if (x + it->w > avail && brk > start && it->kind != I_CLOSE) {
                end = brk;
                next = brk + 1;
                break;
            }
            x += it->w;
        }
        if (i >= n)
            end = next = n;
        /* Trailing spaces do not count. */
        int e = end;
        while (e > start && items[e - 1].kind == I_SPACE && items[e - 1].wrap)
            e--;
        int has = forced;
        for (int k = start; k < e && !has; k++)
            has = is_content(&items[k]);
        f->last_line = forced || next >= n;
        if (has)
            place_line(c, f, start, e);
        else if (forced)
            place_line(c, f, start, start);
        i = next;
    }
    f->x = ox;
    f->w = ow;
    f->n = 0;
    f->nopen = 0;
    f->space_before = 1;
}

/* ---- blocks ---- */

static void block_child(struct lctx *c, struct flow *f, struct node *n)
{
    flush_run(c, f);
    if (c->measuring) {
        int mn, mx;
        measure(c, n, &mn, &mx);
        if (mn > f->min)
            f->min = mn;
        if (mx > f->max)
            f->max = mx;
        return;
    }
    const struct style *st = n->style;
    if (is_replaced(n)) {               /* img { display: block } */
        struct mark m0 = mark_now(c);
        int w, h, base;
        layout_replaced(c, n, f->w, &w, &h, &base);
        int auto_l = st->margin[3].unit == U_AUTO, auto_r = st->margin[1].unit == U_AUTO;
        int off = auto_l && auto_r ? (f->w - w) / 2 : auto_l ? f->w - w : 0;
        f->cy += f->pm;
        shift(c, m0, mark_now(c), f->x + (off > 0 ? off : 0), f->cy);
        f->cy += h;
        f->pm = 0;
        return;
    }
    int mt = len_px(st->margin[0], f->w);
    int gap = collapse(f->pm, mt);
    if (st->clear && float_clear_y(c, st->clear) > f->cy + gap)
        gap = float_clear_y(c, st->clear) - f->cy;      /* clearance */
    int mb, bx = f->x, bwid = f->w;
    if (c->nfloats > c->float_base && (st->clips || st->display == D_TABLE)) {
        int l = bx, r = bx + bwid;              /* a new formatting context sits beside floats */
        if (float_edges(c, f->cy + gap, 1, &l, &r) && r > l) {
            bx = l;
            bwid = r - l;
        }
    }
    int h = st->display == D_TABLE ? layout_table(c, n, bx, f->cy + gap, bwid, -1, &mb)
                                   : layout_block_box(c, n, bx, f->cy + gap, bwid, -1, &mb, NULL);
    f->cy += gap + h;
    f->pm = mb;
}

/* ::before (0) or ::after (1): its text in an inline box of its own. */
static void add_pseudo(struct lctx *c, struct flow *f, struct node *n, int which)
{
    const struct style *ps = n->pseudo_st[which];
    if (!ps || !n->pseudo_text[which])
        return;
    int base = c->measuring ? 0 : f->w;
    struct item *it = push_item(f, I_OPEN, ps);
    if (it)
        it->w = len_px(ps->margin[3], base) + ps->border_w[3] + len_px(ps->padding[3], base);
    add_text(c, f, n->pseudo_text[which], (int)strlen(n->pseudo_text[which]), ps, n);
    it = push_item(f, I_CLOSE, ps);
    if (it)
        it->w = len_px(ps->padding[1], base) + ps->border_w[1] + len_px(ps->margin[1], base);
}

static void flow_node(struct lctx *c, struct flow *f, struct node *n)
{
    if (n->type == N_TEXT) {
        add_text(c, f, n->text, n->len, n->parent ? n->parent->style : NULL, n->parent);
        return;
    }
    const struct style *st = n->style;
    if (!st || st->display == D_NONE || !st->font)
        return;
    if (n->tag == T_BR) {
        push_item(f, I_BREAK, st);
        f->space_before = 1;
        return;
    }
    if (n->tag == T_WBR) {
        struct item *it = push_item(f, I_SPACE, st);
        if (it) {
            it->wrap = 1;
            it->invisible = 1;
        }
        return;
    }
    if (n->tag == T_INPUT && !strcasecmp(input_type(n), "hidden"))
        return;
    if (st->position == POS_ABSOLUTE || st->position == POS_FIXED) {
        /* out of the flow: laid out once the box it is placed against is known */
        if (!c->measuring && c->npend < (int)(sizeof(c->pend) / sizeof(c->pend[0])))
            c->pend[c->npend++] = (struct pend){ n, f->x, f->cy + f->pm };
        return;
    }
    if (st->floated && !c->measuring) {
        place_float(c, f, n);
        return;
    }
    if (is_block_level(st->display)) {
        block_child(c, f, n);
        return;
    }
    if (is_replaced(n) || st->display == D_INLINE_BLOCK || st->display == D_INLINE_TABLE ||
        st->display == D_TABLE_CELL) {
        add_atomic(c, f, n);
        return;
    }
    /* An inline element: its edges, then its content. Percentages are of the
     * container's width, which measuring does not know: they count as 0. */
    int base = c->measuring ? 0 : f->w;
    struct item *it = push_item(f, I_OPEN, st);
    if (it) {
        it->node = n;
        it->w = len_px(st->margin[3], base) + st->border_w[3] + len_px(st->padding[3], base);
    }
    struct node *saved = f->link;
    if (n->tag == T_A && node_attr(n, "href"))
        f->link = n;
    add_pseudo(c, f, n, 0);
    flow_children(c, f, n);
    add_pseudo(c, f, n, 1);
    f->link = saved;
    it = push_item(f, I_CLOSE, st);
    if (it) {
        it->node = n;
        it->w = len_px(st->padding[1], base) + st->border_w[1] + len_px(st->margin[1], base);
    }
}

static void flow_children(struct lctx *c, struct flow *f, struct node *n)
{
    for (struct node *k = n->first; k; k = k->next)
        flow_node(c, f, k);
}

/* The <a href> that n is, or is inside of. */
static struct node *enclosing_link(struct node *n)
{
    for (; n; n = n->parent)
        if (n->type == N_ELEMENT && n->tag == T_A && node_attr(n, "href"))
            return n;
    return NULL;
}

/* Lay out the content of a block container; returns its height (the last
 * child's bottom margin is left in *pm). defh is the content height when
 * the box has one set (else -1), minh its least. */
static int block_content(struct lctx *c, struct node *n, int x, int y, int w, int defh, int minh,
                         int *pm)
{
    const struct style *st = n->style;
    if (st->inner == IN_FLEX || st->inner == IN_GRID) {
        *pm = 0;
        int h = st->inner == IN_FLEX ? layout_flex(c, n, x, y, w, defh, minh)
                                     : layout_grid(c, n, x, y, w, defh, minh);
        if (!c->measuring)
            for (struct node *k = n->first; k; k = k->next)     /* absolute children are not items */
                if (k->type == N_ELEMENT && k->style && k->style->display != D_NONE &&
                    (k->style->position == POS_ABSOLUTE || k->style->position == POS_FIXED) &&
                    c->npend < (int)(sizeof(c->pend) / sizeof(c->pend[0])))
                    c->pend[c->npend++] = (struct pend){ k, x, y };
        return h;
    }
    struct flow f = { 0 };
    f.x = x, f.w = w, f.y0 = f.cy = y;
    f.st = st;
    f.space_before = 1;
    f.link = enclosing_link(n);
    if (c->inside_text[0] && n->style->display == D_LIST_ITEM && c->inside_st) {
        add_text(c, &f, c->inside_text, (int)strlen(c->inside_text), c->inside_st, n);
        c->inside_text[0] = 0;
    }
    add_pseudo(c, &f, n, 0);
    flow_children(c, &f, n);
    add_pseudo(c, &f, n, 1);
    flush_run(c, &f);
    free(f.items);
    *pm = f.pm;
    return f.cy - y;
}

/* An absolute or fixed box, placed against the padding box (cbx, cby, cbw x cbh) of its containing block. */
static void layout_abs(struct lctx *c, const struct pend *p, int cbx, int cby, int cbw, int cbh)
{
    struct node *n = p->n;
    const struct style *st = n->style;
    int la = st->left.unit == U_AUTO, ra = st->right.unit == U_AUTO;
    int ta = st->top.unit == U_AUTO, ba = st->bottom.unit == U_AUTO;
    int l = len_px(st->left, cbw), r = len_px(st->right, cbw);
    int t = len_px(st->top, cbh), bo = len_px(st->bottom, cbh);
    int ml = len_px(st->margin[3], cbw), mr = len_px(st->margin[1], cbw);
    int avail = cbw - (la ? 0 : l) - (ra ? 0 : r);
    if (avail < 0)
        avail = 0;
    struct mark m0 = mark_now(c);
    int ow, oh, base;
    if (is_replaced(n)) {
        layout_replaced(c, n, avail, &ow, &oh, &base);
    } else {
        if (st->width.unit == U_AUTO && !la && !ra)
            c->abs_bw1 = (avail - ml - mr > 0 ? avail - ml - mr : 0) + 1;      /* stretched between the sides */
        layout_inline_block(c, n, avail, &ow, &oh, &base);
        c->abs_bw1 = 0;
    }
    int x, y;
    if (!la)
        x = cbx + l;
    else if (!ra)
        x = cbx + cbw - r - ow;
    else
        x = p->sx;
    if (!ta)
        y = cby + t;
    else if (!ba)
        y = cby + cbh - bo - oh;
    else
        y = p->sy;
    if (!la && !ra && st->width.unit != U_AUTO && st->margin[3].unit == U_AUTO && st->margin[1].unit == U_AUTO)
        x = cbx + l + (avail - ow) / 2;                 /* auto margins centre it between the sides */
    if (!ta && !ba && st->height.unit != U_AUTO && st->margin[0].unit == U_AUTO && st->margin[2].unit == U_AUTO)
        y = cby + t + (cbh - t - bo - oh) / 2;
    shift(c, m0, mark_now(c), x, y);
    if (st->z_set && st->z_index != 0 && c->nzr < (int)(sizeof(c->zr) / sizeof(c->zr[0])))
        c->zr[c->nzr++] = (struct zrange){ m0.items, c->lo->nitems, st->z_index };
    if (st->position == POS_FIXED)      /* it stays where it is in the window as the page scrolls */
        dyn_register(c, m0, 1, x, y, ow, oh, st, NULL);
}

/*
 * Lay out a block box with its border box's top at y, in a containing
 * block starting at x and cw wide (force_bw >= 0 fixes the border-box
 * width). Returns the border-box height; *mb gets the bottom margin.
 * With bg_slot, the background is left for the caller to size and the
 * borders are not drawn (table cells grow to their row's height).
 */
static int layout_block_box(struct lctx *c, struct node *n, int x, int y, int cw, int force_bw,
                            int *mb, int *bg_slot)
{
    const struct style *st = n->style;
    struct boxm b;
    resolve_box(st, cw, force_bw, &b);
    int bw = b.bl + b.pl + b.w + b.pr + b.br;
    if (force_bw < 0 && st->width.unit != U_AUTO && n->parent && n->parent->style &&
        n->parent->style->center_blocks)
        b.ml = (cw - bw) / 2 > 0 ? (cw - bw) / 2 : 0;
    int bx = x + b.ml;
    struct mark m_start = mark_now(c);
    int first_dyn = c->lo->ndyn;
    /* Positioned boxes (and boxes that will be moved as a whole) place absolute descendants;
     * boxes that start a formatting context keep their floats to themselves. */
    int is_cb = st->position != POS_STATIC || c->force_container || bg_slot || n == c->contain || !n->parent;
    int pfirst = c->npend;
    int bfc = !n->parent || st->clips || st->floated || st->display == D_TABLE_CELL ||
              st->display == D_INLINE_BLOCK || st->position == POS_ABSOLUTE || st->position == POS_FIXED ||
              n == c->contain;
    int saved_base = c->float_base;
    c->force_container = 0;
    if (bfc)
        c->float_base = c->nfloats;
    anchors_of(c, n, y);

    /* A box-shadow is a few translucent rectangles behind the background, sized below. */
    int sh_first = c->lo->nitems, sh_steps = 0;
    if (ALPHA(st->sh_color) && !st->hidden && !bg_slot) {
        sh_steps = st->sh_blur <= 0 ? 1 : st->sh_blur < 10 ? 3 : 5;
        for (int k = 0; k < sh_steps; k++)
            fill(c, 0, 0, 1, 1, OPAQUE(0), 0);
        if (c->lo->nitems - sh_first != sh_steps)
            sh_steps = 0;
    }
    int slot = c->lo->nitems;
    fill(c, 0, 0, 1, 1, OPAQUE(0), 0);          /* the background, sized below */
    if (c->lo->nitems == slot)
        slot = -1;
    else if (bg_picture_wanted(st))
        emit(c, DK_BGIMG);                      /* its picture goes just after it */
    struct mark mc0 = mark_now(c);              /* the contents start here */

    int cx = bx + b.bl + b.pl, cy = y + b.bt + b.pt;
    if (st->display == D_LIST_ITEM)
        start_marker(c, n, cx);
    int pm = 0, vt = st->box_sizing_border ? b.bt + b.bb + b.pt + b.pb : 0, defh = -1, minh = 0;
    if (st->height.unit == U_PX)
        defh = st->height.v - vt > 0 ? st->height.v - vt : 0;
    if (st->min_height.unit == U_PX)
        minh = st->min_height.v - vt > 0 ? st->min_height.v - vt : 0;
    int ch = block_content(c, n, cx, cy, b.w, defh, minh, &pm);
    c->inside_text[0] = 0;
    if (c->marker_pending && st->display == D_LIST_ITEM)
        place_marker(c, cy + wfont_ascent(st->font));
    struct mark mc1 = mark_now(c);              /* ...and end here */
    if (bfc) {                          /* a formatting context contains its floats */
        int low = float_clear_y(c, 3);
        if (low > cy + ch)
            ch = low - cy;
    } else if (n->pseudo_st[1] && n->pseudo_st[1]->clear) {      /* a clearfix */
        int low = float_clear_y(c, n->pseudo_st[1]->clear);
        if (low > cy + ch)
            ch = low - cy;
    }
    if (bfc) {
        c->nfloats = c->float_base;
        c->float_base = saved_base;
    }

    *mb = b.mb;
    if (st->height.unit == U_PX) {
        ch = st->height.v - (st->box_sizing_border ? b.bt + b.bb + b.pt + b.pb : 0);
        if (ch < 0)
            ch = 0;
    } else if (!b.pb && !b.bb && st->display != D_TABLE_CELL && n->parent && n != c->contain) {
        *mb = collapse(b.mb, pm);               /* the last child's margin passes through */
    } else {
        ch += pm;
    }
    if (st->aspect_w && st->height.unit == U_AUTO) {
        int ah = muldiv(b.w, st->aspect_h, st->aspect_w);
        if (ch < ah)
            ch = ah;
    }
    if (st->max_height.unit == U_PX) {
        int mh = st->max_height.v - (st->box_sizing_border ? b.bt + b.bb + b.pt + b.pb : 0);
        if (mh >= 0 && ch > mh)
            ch = mh;
    }
    if (st->min_height.unit == U_PX && ch < st->min_height.v)
        ch = st->min_height.v;
    int bh = b.bt + b.pt + ch + b.pb + b.bb;

    for (int k = 0; k < sh_steps; k++) {        /* the outermost layer first, fading outwards */
        struct ditem *d = &c->lo->items[sh_first + k];
        int e = st->sh_spread + (sh_steps > 1 ? st->sh_blur - st->sh_blur * k / sh_steps : 0);
        int lim = ((bw + 2 * e) < (bh + 2 * e) ? (bw + 2 * e) : (bh + 2 * e)) / 2;
        d->x = bx + st->sh_x - e, d->y = y + st->sh_y - e, d->w = bw + 2 * e, d->h = bh + 2 * e;
        if (d->w <= 0 || d->h <= 0) {
            d->w = d->h = 0;
            continue;
        }
        uint32_t a = ALPHA(st->sh_color) / (uint32_t)sh_steps;
        d->color = (a ? a : 1) << 24 | (st->sh_color & 0xFFFFFF);
        d->node = NULL;
        for (int q = 0; q < 4; q++) {
            int r = len_px(st->radius[q], bw < bh ? bw : bh) + e;
            d->rad[q] = r < 0 ? 0 : r > lim ? lim : r;
        }
    }

    struct mark mf = mc1;
    if (bg_slot) {
        *bg_slot = slot;
        if (slot >= 0) {
            struct ditem *d = &c->lo->items[slot];
            int paint = ALPHA(st->bg) && !st->hidden && n != c->skip_bg;
            d->x = bx, d->y = y, d->w = bw, d->h = bh;
            d->color = paint ? st->bg : TRANSPARENT;
            d->node = n;
            bg_picture_finish(c, n, slot);
        }
    } else {
        finish_box(c, n, slot, bx, y, bw, bh);
        mf = mark_now(c);                       /* what follows is not the box's own */
    }
    if (is_cb && c->npend > pfirst) {
        int end = c->npend;
        for (int i = pfirst; i < end; i++) {
            struct pend p = c->pend[i];
            if (p.n->style->position == POS_FIXED || !n->parent)
                layout_abs(c, &p, 0, 0, c->vw, c->vh);          /* against the viewport */
            else
                layout_abs(c, &p, bx + b.bl, y + b.bt, bw - b.bl - b.br, bh - b.bt - b.bb);
        }
        c->npend = pfirst;
    }
    if (st->clips && !bg_slot) {                        /* overflow: its contents are clipped, perhaps scrolled */
        struct mark mp = mark_now(c);
        struct sbrange rg[2] = {
            { mc0.items, mc1.items, mc0.links, mc1.links, mc0.controls, mc1.controls },
            { mf.items, mp.items, mf.links, mp.links, mf.controls, mp.controls },   /* absolute descendants */
        };
        sbox_register(c, n, bx, y, bw, bh, rg, 2);
    }
    if (!bg_slot && (st->tx.v || st->ty.v)) {           /* transform: translate() */
        int dx = len_px(st->tx, bw), dy = len_px(st->ty, bh);
        if (dx || dy)
            shift(c, m_start, mark_now(c), dx, dy);
    }
    if (st->position == POS_RELATIVE && !bg_slot) {
        int dx = st->left.unit != U_AUTO ? len_px(st->left, cw) : st->right.unit != U_AUTO ? -len_px(st->right, cw) : 0;
        int dy = st->top.unit != U_AUTO ? len_px(st->top, 0) : st->bottom.unit != U_AUTO ? -len_px(st->bottom, 0) : 0;
        if (dx || dy)
            shift(c, m_start, mark_now(c), dx, dy);
    }
    /* Sticky boxes inside this one stay within its content area; this one may be sticky too. */
    if (c->lo->ndyn > first_dyn)
        dyn_set_container(c, first_dyn, n, bx + b.bl + b.pl, y + b.bt + b.pt,
                          bx + bw - b.br - b.pr, y + bh - b.bb - b.pb);
    if (st->position == POS_STICKY && !bg_slot && n->parent &&
        (st->top.unit != U_AUTO || st->bottom.unit != U_AUTO || st->left.unit != U_AUTO ||
         st->right.unit != U_AUTO))
        dyn_register(c, m_start, 0, bx, y, bw, bh, st, n->parent);
    return bh;
}

/* ---- flex and grid containers ---- */

/* a * b / c without overflowing 32 bits on the way (c > 0). */
static int muldiv(int a, int b, int c)
{
    int neg = (a < 0) != (b < 0);
    unsigned ua = a < 0 ? 0u - (unsigned)a : (unsigned)a, ub = b < 0 ? 0u - (unsigned)b : (unsigned)b;
    unsigned uc = c > 0 ? (unsigned)c : 1u;
    unsigned long long p = (unsigned long long)ua * ub;
    unsigned hi = (unsigned)(p >> 32), lo = (unsigned)p;
    if (hi >= uc)
        return neg ? -0x7FFFFFFF : 0x7FFFFFFF;
    unsigned r = hi, q = 0;
    for (int i = 31; i >= 0; i--) {             /* long division, a bit at a time */
        r = (r << 1) | (lo >> i & 1);
        if (r >= uc) {
            r -= uc;
            q |= 1u << i;
        }
    }
    int res = q > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)q;
    return neg ? -res : res;
}

enum { FI_BLOCK, FI_REPLACED, FI_TABLE, FI_TEXT, FI_PSEUDO };

struct fitem {
    struct node *n;             /* the element; a text node; the container, for ::before/::after */
    const struct style *st;     /* the element's style; the container's for text */
    uint8_t kind, which;        /* FI_*; FI_PSEUDO: 0 ::before, 1 ::after */
    int order;
    int ml, mr, mt, mb;         /* margins; auto ones are 0 here and flagged */
    uint8_t aml, amr, amt, amb;
    int hz, vt;                 /* borders and padding across and down */
    int grow, shrink;           /* times 100 */
    /* sizes along the main axis: content boxes for rows, border boxes for columns */
    int base, hyp, minm, maxm, main;
    int me, mm;                 /* the edges and margins along it */
    int viol;
    uint8_t frozen;
    int bw, bh;                 /* the border box as laid out */
    int slot;                   /* its background, sized when it is placed */
    struct mark m0, m1;
    int col, row, cspan, rspan; /* grid */
};

static int anonymous(const struct fitem *it)
{
    return it->kind == FI_TEXT || it->kind == FI_PSEUDO;
}

/* Text with something other than collapsible spaces in it. */
static int has_text(const char *t, int len, const struct style *st)
{
    if (st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP)
        return len > 0;
    for (int i = 0; i < len; i++)
        if (t[i] != ' ' && t[i] != '\t' && t[i] != '\n' && t[i] != '\r' && t[i] != '\f')
            return 1;
    return 0;
}

static void push_fitem(struct fitem *items, int *n, struct node *node, const struct style *st,
                       int kind, int which)
{
    struct fitem *it = &items[(*n)++];
    memset(it, 0, sizeof(*it));
    it->n = node;
    it->st = st;
    it->kind = (uint8_t)kind;
    it->which = (uint8_t)which;
    it->order = kind == FI_PSEUDO ? (which ? 1 << 30 : -(1 << 30)) : anonymous(it) ? 0 : st->order;
    it->slot = -1;
}

/*
 * The items of a flex or grid container: its element children, and runs
 * of text and ::before/::after content as anonymous items, in order.
 * Returns how many (the array is malloc'd).
 */
static int container_items(struct node *n, struct fitem **out)
{
    int cap = 2;
    for (struct node *k = n->first; k; k = k->next)
        cap++;
    struct fitem *items = malloc(sizeof(struct fitem) * (size_t)cap);
    *out = items;
    if (!items)
        return 0;
    int ni = 0;
    for (int pe = 0; pe < 2; pe++) {
        const struct style *ps = n->pseudo_st[pe];
        const char *t = n->pseudo_text[pe];
        if (ps && t && ps->display != D_NONE && has_text(t, (int)strlen(t), ps))
            push_fitem(items, &ni, n, ps, FI_PSEUDO, pe);
    }
    for (struct node *k = n->first; k; k = k->next) {
        if (k->type == N_TEXT) {
            if (has_text(k->text, k->len, n->style))
                push_fitem(items, &ni, k, n->style, FI_TEXT, 0);
            continue;
        }
        const struct style *st = k->style;
        if (!st || st->display == D_NONE || !st->font || k->tag == T_BR || k->tag == T_WBR ||
            (k->tag == T_INPUT && !strcasecmp(input_type(k), "hidden")) ||
            st->position == POS_ABSOLUTE || st->position == POS_FIXED)
            continue;
        int kind = is_replaced(k) ? FI_REPLACED
                 : st->display == D_TABLE || st->display == D_INLINE_TABLE ? FI_TABLE : FI_BLOCK;
        push_fitem(items, &ni, k, st, kind, 0);
    }
    /* order, keeping document order among equals */
    for (int i = 1; i < ni; i++) {
        struct fitem t = items[i];
        int j = i;
        while (j > 0 && items[j - 1].order > t.order) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = t;
    }
    return ni;
}

/* An anonymous item's content, into a flow. */
static void anon_content(struct lctx *c, struct flow *f, struct fitem *it)
{
    if (it->kind == FI_TEXT)
        flow_node(c, f, it->n);
    else
        add_pseudo(c, f, it->n, it->which);
}

/* An item's narrowest and widest margin boxes. */
static void item_measure(struct lctx *c, struct fitem *it, int *mn, int *mx)
{
    if (!anonymous(it)) {
        measure(c, it->n, mn, mx);
        return;
    }
    int saved = c->measuring;
    c->measuring = 1;
    struct flow f = { 0 };
    f.st = it->st;
    f.space_before = 1;
    f.w = BIG;
    anon_content(c, &f, it);
    flush_run(c, &f);
    free(f.items);
    c->measuring = saved;
    *mn = f.min;
    *mx = f.max;
}

/* Margins, borders and padding, in a container cw wide. Replaced elements
 * keep theirs inside the box layout_replaced makes. */
static void item_edges(struct fitem *it, int cw)
{
    const struct style *st = it->st;
    it->ml = it->mr = it->mt = it->mb = it->hz = it->vt = 0;
    it->aml = it->amr = it->amt = it->amb = 0;
    it->grow = 0;
    it->shrink = 100;
    if (anonymous(it))
        return;
    it->aml = st->margin[3].unit == U_AUTO, it->amr = st->margin[1].unit == U_AUTO;
    it->amt = st->margin[0].unit == U_AUTO, it->amb = st->margin[2].unit == U_AUTO;
    it->grow = st->flex_grow;
    it->shrink = st->flex_shrink;
    if (it->kind == FI_REPLACED)
        return;
    it->ml = len_px(st->margin[3], cw), it->mr = len_px(st->margin[1], cw);
    it->mt = len_px(st->margin[0], cw), it->mb = len_px(st->margin[2], cw);
    it->hz = st->border_w[1] + st->border_w[3] + len_px(st->padding[1], cw) + len_px(st->padding[3], cw);
    it->vt = st->border_w[0] + st->border_w[2] + len_px(st->padding[0], cw) + len_px(st->padding[2], cw);
}

/* How it sits across the line: align-self, or the container's align-items. */
static int item_align(const struct style *cst, const struct fitem *it)
{
    int al = anonymous(it) || it->st->align_self == AL_AUTO ? cst->align_items : it->st->align_self;
    return al == AL_NORMAL ? AL_STRETCH : al;
}

static int item_justify(const struct style *cst, const struct fitem *it)
{
    int al = anonymous(it) || it->st->justify_self == AL_AUTO ? cst->justify_items : it->st->justify_self;
    return al == AL_NORMAL ? AL_STRETCH : al;
}

/* The border-box width of an item that is not stretched: its own width,
 * or shrink-to-fit in avail (its margin box). */
static int fit_width(struct lctx *c, struct fitem *it, int avail)
{
    const struct style *st = it->st;
    if (!anonymous(it) && st->width.unit != U_AUTO) {
        struct boxm b;
        resolve_box(st, avail, -1, &b);
        return b.w + hz(&b);
    }
    int mn, mx;
    item_measure(c, it, &mn, &mx);
    int stf = mx < avail ? mx : avail;
    if (stf < mn)
        stf = mn;
    stf -= it->ml + it->mr;
    return stf > 0 ? stf : 0;
}

/* A stretched item's border-box width: the space less margins, within max-width. */
static int stretch_width(struct fitem *it, int avail)
{
    int bw = avail - it->ml - it->mr;
    const struct style *st = it->st;
    if (!anonymous(it) && st->max_width.unit != U_AUTO) {
        int mx = len_px(st->max_width, avail) + (st->box_sizing_border ? 0 : it->hz);
        if (bw > mx)
            bw = mx;
    }
    if (!anonymous(it) && st->min_width.unit != U_AUTO) {
        int mn = len_px(st->min_width, avail) + (st->box_sizing_border ? 0 : it->hz);
        if (bw < mn)
            bw = mn;
    }
    return bw > 0 ? bw : 0;
}

/*
 * Lay out an item with its border box at (0, 0), bw wide (replaced
 * elements take their own size, margins and all). Returns its height;
 * a block's background and borders wait for item_place.
 */
static int item_layout(struct lctx *c, struct fitem *it, int bw, int cw)
{
    it->m0 = mark_now(c);
    it->slot = -1;
    int h = 0, mb;
    if (anonymous(it)) {
        struct flow f = { 0 };
        f.w = bw;
        f.st = it->st;
        f.space_before = 1;
        f.link = enclosing_link(it->kind == FI_TEXT ? it->n->parent : it->n);
        anon_content(c, &f, it);
        flush_run(c, &f);
        free(f.items);
        h = f.cy;
    } else if (it->kind == FI_REPLACED) {
        int base;
        layout_replaced(c, it->n, cw, &bw, &h, &base);
    } else if (it->kind == FI_TABLE) {
        h = layout_table(c, it->n, 0, 0, cw, bw, &mb);
    } else {
        int saved = c->marker_pending;
        struct node *outer = c->contain;
        c->marker_pending = 0;
        c->contain = it->n;
        h = layout_block_box(c, it->n, 0, 0, cw, bw, &mb, &it->slot);
        c->contain = outer;
        c->marker_pending = saved;
    }
    it->m1 = mark_now(c);
    it->bw = bw;
    it->bh = h;
    return h;
}

/* Move a laid-out item to (x, y), h tall, and finish its box. */
static void item_place(struct lctx *c, struct fitem *it, int x, int y, int h)
{
    shift(c, it->m0, it->m1, x, y);
    if (it->kind != FI_BLOCK)
        return;
    finish_box(c, it->n, it->slot, x, y, it->bw, h);
    if (it->n->style->clips) {                          /* overflow: clip (and scroll) what the item holds */
        struct mark from = it->m0;
        if (it->slot >= 0 && from.items == it->slot) {  /* not its background, nor its picture */
            from.items++;
            if (from.items < it->m1.items && c->lo->items[from.items].kind == DK_BGIMG)
                from.items++;
        }
        struct sbrange rg = { from.items, it->m1.items, from.links, it->m1.links, from.controls,
                              it->m1.controls };
        sbox_register(c, it->n, x, y, it->bw, h, &rg, 1);
    }
    if (it->n->tag == T_BUTTON && !it->st->hidden)
        add_hotspot(c, &c->lo->controls, &c->lo->ncontrols, &c->lo->controlcap, it->n, x, y, it->bw, h);
    struct node *link = enclosing_link(it->n);
    if (link)
        add_hotspot(c, &c->lo->links, &c->lo->nlinks, &c->lo->linkcap, link, x, y, it->bw, h);
}

/* A row item's flex base size and limits (content box widths). */
static void row_sizes(struct lctx *c, struct fitem *it, int cw)
{
    const struct style *st = it->st;
    int anon = anonymous(it), mn = 0, mx = 0;
    it->me = it->hz;
    it->mm = it->ml + it->mr;
    if (it->kind == FI_REPLACED) {
        item_measure(c, it, &mn, &mx);
        it->base = it->hyp = it->minm = it->maxm = mx;
        it->grow = it->shrink = 0;
        return;
    }
    int bs = !anon && st->box_sizing_border ? it->hz : 0, edges = it->hz + it->mm;
    if (anon || (st->flex_basis.unit == U_AUTO && st->width.unit == U_AUTO) ||
        (st->min_width.unit == U_AUTO && !st->clips))
        item_measure(c, it, &mn, &mx);
    if (!anon && st->flex_basis.unit != U_AUTO)
        it->base = len_px(st->flex_basis, cw) - bs;
    else if (!anon && st->width.unit != U_AUTO)
        it->base = len_px(st->width, cw) - bs;
    else
        it->base = mx - edges;
    if (!anon && st->min_width.unit != U_AUTO) {
        it->minm = len_px(st->min_width, cw) - bs;
    } else if (!anon && st->clips) {
        it->minm = 0;
    } else {                                    /* automatic: no narrower than its content */
        it->minm = mn - edges;
        if (!anon && st->width.unit != U_AUTO && len_px(st->width, cw) - bs < it->minm)
            it->minm = len_px(st->width, cw) - bs;
    }
    it->maxm = !anon && st->max_width.unit != U_AUTO ? len_px(st->max_width, cw) - bs : BIG;
    if (it->base < 0)
        it->base = 0;
    if (it->minm < 0)
        it->minm = 0;
    if (it->maxm < it->minm)
        it->maxm = it->minm;
    it->hyp = it->base < it->minm ? it->minm : it->base > it->maxm ? it->maxm : it->base;
}

/* Grow or shrink a line's items to fill avail (their margin boxes, less gaps). */
static void resolve_flexible(struct fitem *it, int n, int avail)
{
    int used = 0;
    for (int k = 0; k < n; k++)
        used += it[k].hyp + it[k].me + it[k].mm;
    int growing = used < avail;
    for (int k = 0; k < n; k++) {
        int factor = growing ? it[k].grow : it[k].shrink;
        it[k].main = it[k].hyp;
        it[k].frozen = used == avail || !factor ||
                       (growing ? it[k].base > it[k].hyp : it[k].base < it[k].hyp);
    }
    for (int iter = 0; iter <= n; iter++) {
        int spare = avail, unfrozen = 0, sumf = 0;
        for (int k = 0; k < n; k++) {
            spare -= (it[k].frozen ? it[k].main : it[k].base) + it[k].me + it[k].mm;
            if (it[k].frozen)
                continue;
            unfrozen++;
            sumf += growing ? it[k].grow : muldiv(it[k].shrink, it[k].base, 100);
        }
        if (!unfrozen)
            break;
        if (growing && sumf < 100)              /* flex-grow: .5 takes half the room */
            spare = muldiv(spare, sumf, 100);
        int total = 0;
        for (int k = 0; k < n; k++) {
            if (it[k].frozen)
                continue;
            int share = growing ? it[k].grow : muldiv(it[k].shrink, it[k].base, 100);
            int target = it[k].base + (sumf > 0 ? muldiv(spare, share, sumf) : 0);
            int clamped = target < it[k].minm ? it[k].minm : target > it[k].maxm ? it[k].maxm : target;
            it[k].viol = clamped - target;
            it[k].main = clamped;
            total += it[k].viol;
        }
        for (int k = 0; k < n; k++)
            if (!it[k].frozen && (!total || (total > 0 ? it[k].viol > 0 : it[k].viol < 0)))
                it[k].frozen = 1;
        if (!total)
            break;
    }
}

/* Where free space goes along a line: before the first item, and between them. */
static void justify_space(int jst, int spare, int n, int gap, int *off, int *between)
{
    *off = 0;
    *between = gap;
    if (spare <= 0) {
        if (jst == AL_CENTER)
            *off = spare / 2 > 0 ? spare / 2 : 0;
        return;
    }
    switch (jst) {
    case AL_END: *off = spare; break;
    case AL_CENTER: *off = spare / 2; break;
    case AL_BETWEEN: if (n > 1) *between += spare / (n - 1); break;
    case AL_AROUND: *between += spare / n; *off = spare / n / 2; break;
    case AL_EVENLY: *between += spare / (n + 1); *off = spare / (n + 1); break;
    default: break;
    }
}

static int flex_row(struct lctx *c, const struct style *st, struct fitem *it, int ni, int x, int y,
                    int w, int defh, int minh)
{
    int gap = len_px(st->col_gap, w), rgap = len_px(st->row_gap, w);
    for (int i = 0; i < ni; i++) {
        item_edges(&it[i], w);
        row_sizes(c, &it[i], w);
    }
    /* Break into lines, flex each, and lay the items out to learn their heights. */
    int *starts = malloc(sizeof(int) * (size_t)(ni + 1) * 2);
    if (!starts)
        return 0;
    int *cross = starts + ni + 1;
    int nlines = 0;
    for (int i = 0; i < ni;) {
        int start = i, sum = it[i].hyp + it[i].me + it[i].mm;
        for (i++; i < ni && st->flex_wrap; i++) {
            int o = it[i].hyp + it[i].me + it[i].mm;
            if (sum + gap + o > w)
                break;
            sum += gap + o;
        }
        if (!st->flex_wrap)
            i = ni;
        starts[nlines] = start;
        int cnt = i - start;
        resolve_flexible(it + start, cnt, w - gap * (cnt - 1));
        cross[nlines] = 0;
        for (int k = start; k < i; k++) {
            int h = item_layout(c, &it[k], it[k].main + it[k].me, w);
            if (it[k].mt + h + it[k].mb > cross[nlines])
                cross[nlines] = it[k].mt + h + it[k].mb;
        }
        nlines++;
    }
    starts[nlines] = ni;

    /* A single line fills the container's height; several share what is spare. */
    int total = rgap * (nlines - 1);
    for (int l = 0; l < nlines; l++)
        total += cross[l];
    int target = defh >= 0 ? defh : minh;
    if (!st->flex_wrap && nlines == 1 && defh >= 0 && defh < cross[0]) {
        total += defh - cross[0];       /* one line in a container of a set height: the line is that tall */
        cross[0] = defh;
    } else if (!st->flex_wrap && nlines == 1 && target > cross[0]) {
        total += target - cross[0];
        cross[0] = target;
    } else if (target > total && nlines) {
        int extra = target - total;
        for (int l = 0; l < nlines; l++)
            cross[l] += extra / nlines;
        total = target;
    }

    int jst = st->justify;
    if (st->flex_reverse)
        jst = jst == AL_END ? AL_START : jst == AL_START || jst == AL_NORMAL ? AL_END : jst;
    int ly = y;
    for (int l = 0; l < nlines; l++) {
        int a = starts[l], b = starts[l + 1], cnt = b - a;
        int spare = w - gap * (cnt - 1), nauto = 0;
        for (int k = a; k < b; k++) {
            spare -= it[k].bw + it[k].ml + it[k].mr;
            nauto += it[k].aml + it[k].amr;
        }
        int off, between, per_auto = 0;
        if (spare > 0 && nauto) {
            per_auto = spare / nauto;
            off = 0, between = gap;
        } else {
            justify_space(jst, spare, cnt, gap, &off, &between);
        }
        int cx = x + off;
        for (int k = a; k < b; k++) {
            struct fitem *t = &it[k];
            if (t->aml)
                cx += per_auto;
            cx += t->ml;
            int al = item_align(st, t), h = t->bh, top;
            int outer = t->mt + h + t->mb;
            if (al == AL_STRETCH && t->kind == FI_BLOCK && t->st->height.unit == U_AUTO &&
                !t->amt && !t->amb && (cross[l] - t->mt - t->mb > h ||
                                       (t->st->clips && cross[l] - t->mt - t->mb >= 0)))
                h = cross[l] - t->mt - t->mb;           /* (one that clips or scrolls may be made shorter) */
            if (t->amt && t->amb)
                top = (cross[l] - outer) / 2 + t->mt;
            else if (t->amt)
                top = cross[l] - t->mb - h;
            else if (al == AL_END)
                top = cross[l] - t->mb - h;
            else if (al == AL_CENTER)
                top = (cross[l] - outer) / 2 + t->mt;
            else
                top = t->mt;
            item_place(c, t, cx, ly + top, h);
            cx += t->bw + t->mr + between;
            if (t->amr)
                cx += per_auto;
        }
        ly += cross[l] + rgap;
    }
    free(starts);
    return total;
}

static int flex_column(struct lctx *c, const struct style *st, struct fitem *it, int ni, int x, int y,
                       int w, int defh, int minh)
{
    int gap = len_px(st->row_gap, w);
    int used = gap * (ni - 1);
    for (int i = 0; i < ni; i++) {
        struct fitem *t = &it[i];
        item_edges(t, w);
        int al = item_align(st, t), bw;
        if (t->kind == FI_REPLACED)
            bw = -1;
        else if (anonymous(t))
            bw = w;
        else if (al == AL_STRETCH && t->st->width.unit == U_AUTO && !t->aml && !t->amr)
            bw = stretch_width(t, w);
        else
            bw = fit_width(c, t, w);
        int h = item_layout(c, t, bw, w);
        /* Heights are border boxes here; an item is never shorter than its content. */
        const struct style *ist = t->st;
        int bs = !anonymous(t) && !ist->box_sizing_border ? t->vt : 0;
        t->me = 0;
        t->mm = t->mt + t->mb;
        t->base = h;
        if (!anonymous(t) && ist->flex_basis.unit == U_PX)
            t->base = ist->flex_basis.v + bs;
        else if (!anonymous(t) && ist->flex_basis.unit != U_AUTO && defh >= 0)
            t->base = len_px(ist->flex_basis, defh) + bs;
        t->minm = !anonymous(t) && ist->clips ? t->vt : h;
        if (!anonymous(t) && ist->min_height.unit == U_PX && ist->min_height.v + bs > t->minm)
            t->minm = ist->min_height.v + bs;
        t->maxm = BIG;
        if (t->kind != FI_BLOCK)
            t->grow = t->shrink = 0, t->minm = t->maxm = h;
        t->hyp = t->base < t->minm ? t->minm : t->base;
        used += t->hyp + t->mm;
    }
    int avail = defh >= 0 ? defh : minh > used ? minh : used;
    resolve_flexible(it, ni, avail - gap * (ni - 1));

    int spare = avail - gap * (ni - 1), nauto = 0;
    for (int i = 0; i < ni; i++) {
        if (it[i].main < it[i].bh && !(it[i].kind == FI_BLOCK && !anonymous(&it[i]) && it[i].st->clips))
            it[i].main = it[i].bh;      /* never shorter than its content, unless it clips or scrolls it */
        spare -= it[i].main + it[i].mm;
        nauto += it[i].amt + it[i].amb;
    }
    int off, between, per_auto = 0;
    int jst = st->justify;
    if (st->flex_reverse)
        jst = jst == AL_END ? AL_START : jst == AL_START || jst == AL_NORMAL ? AL_END : jst;
    if (spare > 0 && nauto) {
        per_auto = spare / nauto;
        off = 0, between = gap;
    } else {
        justify_space(jst, spare, ni, gap, &off, &between);
    }
    int cy = y + off;
    for (int i = 0; i < ni; i++) {
        struct fitem *t = &it[i];
        if (t->amt)
            cy += per_auto;
        cy += t->mt;
        int al = item_align(st, t), left;
        int outer = t->ml + t->bw + t->mr;
        if (t->aml && t->amr)
            left = (w - outer) / 2 + t->ml;
        else if (t->aml || al == AL_END)
            left = w - t->mr - t->bw;
        else if (al == AL_CENTER)
            left = (w - outer) / 2 + t->ml;
        else
            left = t->ml;
        item_place(c, t, x + left, cy, t->main);
        cy += t->main + t->mb + between;
        if (t->amb)
            cy += per_auto;
    }
    int h = cy - between - y;
    if (ni && it[ni - 1].amb)
        h -= per_auto;
    return h > avail ? h : avail;
}

static int layout_flex(struct lctx *c, struct node *n, int x, int y, int w, int defh, int minh)
{
    const struct style *st = n->style;
    struct fitem *items;
    int ni = container_items(n, &items), h = 0;
    if (st->flex_reverse)
        for (int i = 0; i < ni / 2; i++) {
            struct fitem t = items[i];
            items[i] = items[ni - 1 - i];
            items[ni - 1 - i] = t;
        }
    if (ni > 0)
        h = st->flex_column ? flex_column(c, st, items, ni, x, y, w, defh, minh)
                            : flex_row(c, st, items, ni, x, y, w, defh, minh);
    free(items);
    return h;
}

/* Track sizes as pixels, or 0 for content-sized ones. */
static int track_px(int kind, int v, int w)
{
    return kind == TK_PX ? v : kind == TK_PCT ? muldiv(w, v, 10000) : 0;
}

/*
 * The column tracks for a container w wide: the template's, with
 * auto-fill and auto-fit repeated as often as they fit. No template
 * makes one column.
 */
static int grid_tracks(const struct gridspec *g, int w, int gap, int nitems, struct track *out)
{
    if (!g || !g->n) {
        out[0] = (struct track){ TK_AUTO, TK_AUTO, 0, 0 };
        return 1;
    }
    if (!g->auto_repeat) {
        memcpy(out, g->t, sizeof(struct track) * (size_t)g->n);
        return g->n;
    }
    int fixed = 0, rep = 0;
    for (int i = 0; i < g->n; i++) {
        const struct track *t = &g->t[i];
        int size = track_px(t->max, t->vmax, w);
        if (!size)
            size = track_px(t->min, t->vmin, w);
        if (i >= g->rep && i < g->rep + g->nrep)
            rep += size + gap;
        else
            fixed += size + gap;
    }
    int others = g->n - g->nrep, count = 1;
    if (rep > 0)
        count = (w - fixed + gap) / rep;
    if (count > (MAX_TRACKS - others) / g->nrep)
        count = (MAX_TRACKS - others) / g->nrep;
    if (g->auto_repeat == 2 && count * g->nrep > nitems)     /* auto-fit: no empty columns */
        count = (nitems + g->nrep - 1) / g->nrep;
    if (count < 1)
        count = 1;
    int n = 0;
    for (int i = 0; i < g->rep; i++)
        out[n++] = g->t[i];
    for (int k = 0; k < count; k++)
        for (int i = 0; i < g->nrep; i++)
            out[n++] = g->t[g->rep + i];
    for (int i = g->rep + g->nrep; i < g->n; i++)
        out[n++] = g->t[i];
    return n;
}

/* Column widths: fixed tracks, then content-sized ones up to what their
 * items want, then fr tracks share the rest; with no fr tracks, auto
 * ones stretch to fill. */
static void size_columns(struct lctx *c, const struct track *tr, int nt, struct fitem *it, int ni,
                         int w, int avail, int *colw)
{
    int cmin[MAX_TRACKS], cmax[MAX_TRACKS], limit[MAX_TRACKS];
    memset(cmin, 0, sizeof(cmin));
    memset(cmax, 0, sizeof(cmax));
    for (int i = 0; i < ni; i++) {
        if (it[i].cspan != 1)
            continue;
        int mn, mx;
        item_measure(c, &it[i], &mn, &mx);
        if (mn > cmin[it[i].col])
            cmin[it[i].col] = mn;
        if (mx > cmax[it[i].col])
            cmax[it[i].col] = mx;
    }
    int spare = avail, sumfr = 0, nauto = 0;
    for (int k = 0; k < nt; k++) {
        const struct track *t = &tr[k];
        int base = t->min == TK_PX || t->min == TK_PCT ? track_px(t->min, t->vmin, w)
                 : t->min == TK_MAX ? cmax[k] : cmin[k];
        int lim = t->max == TK_PX || t->max == TK_PCT ? track_px(t->max, t->vmax, w)
                : t->max == TK_MIN ? base : t->max == TK_FR ? base : cmax[k];
        colw[k] = base;
        limit[k] = lim > base ? lim : base;
        spare -= base;
        if (t->max == TK_FR)
            sumfr += t->vmax;
        else if (t->max == TK_AUTO)
            nauto++;
    }
    /* content-sized tracks grow towards their limits */
    int room = 0;
    for (int k = 0; k < nt; k++)
        if (tr[k].max != TK_FR)
            room += limit[k] - colw[k];
    if (spare > 0 && room > 0) {
        int give = room < spare ? room : spare;
        for (int k = 0; k < nt; k++)
            if (tr[k].max != TK_FR)
                colw[k] += muldiv(limit[k] - colw[k], give, room);
        spare -= give;
    }
    if (sumfr) {
        /* fr tracks: a share of what is left, unless their content needs more */
        uint8_t fixed[MAX_TRACKS];
        memset(fixed, 0, sizeof(fixed));
        for (int iter = 0; iter < nt; iter++) {
            int left = avail, frs = 0;
            for (int k = 0; k < nt; k++) {
                if (tr[k].max != TK_FR || fixed[k])
                    left -= colw[k];
                else
                    frs += tr[k].vmax;
            }
            if (!frs)
                break;
            int unit = muldiv(left > 0 ? left : 0, 100, frs < 100 ? 100 : frs);   /* per 1fr */
            int again = 0;
            for (int k = 0; k < nt; k++) {
                if (tr[k].max != TK_FR || fixed[k])
                    continue;
                int share = muldiv(unit, tr[k].vmax, 100);
                if (share < colw[k]) {
                    fixed[k] = 1;
                    again = 1;
                }
            }
            if (again)
                continue;
            for (int k = 0; k < nt; k++)
                if (tr[k].max == TK_FR && !fixed[k])
                    colw[k] = muldiv(unit, tr[k].vmax, 100);
            break;
        }
    } else if (spare > 0 && nauto) {
        for (int k = 0; k < nt; k++)
            if (tr[k].max == TK_AUTO)
                colw[k] += spare / nauto;
    }
}

/* Put items into cells: explicit columns and rows, then the rest in reading order. */
static int grid_place(struct fitem *it, int ni, int nt, const struct style *cst)
{
    int rows_cap = 16, nrows = 0;
    uint8_t *occ = calloc((size_t)(rows_cap * nt), 1);
    if (!occ)
        return 0;
    int cur_r = 0, cur_c = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < ni; i++) {
            struct fitem *t = &it[i];
            const struct style *st = t->st;
            int anon = anonymous(t);
            int col = -1, cs = !anon && st->gc_span > 0 ? st->gc_span : 1;
            int row = !anon && st->gr_start > 0 ? st->gr_start - 1 : -1;
            int rs = !anon && st->gr_span > 0 ? st->gr_span : 1;
            if (!anon && st->grid_area && cst->grid_areas) {      /* grid-area: name */
                const struct gridareas *ga = cst->grid_areas;
                for (int k = 0; k < ga->n; k++)
                    if (!strcmp(ga->a[k].name, st->grid_area)) {
                        col = ga->a[k].c0;
                        cs = ga->a[k].c1 - ga->a[k].c0;
                        row = ga->a[k].r0;
                        rs = ga->a[k].r1 - ga->a[k].r0;
                        break;
                    }
            }
            if (!anon && st->gc_start && col < 0)
                col = (st->gc_start > 0 ? st->gc_start : nt + 2 + st->gc_start) - 1;
            if (!anon && st->gr_end > 0 && row >= 0 && st->gr_end - 1 > row && !st->grid_area)
                rs = st->gr_end - 1 - row;
            if (!anon && st->gc_end) {
                int end = (st->gc_end > 0 ? st->gc_end : nt + 2 + st->gc_end) - 1;
                if (col >= 0 && end > col)
                    cs = end - col;
                else if (col < 0 && end - cs >= 0)
                    col = end - cs;
            }
            if (cs > nt)
                cs = nt;
            if (col >= 0 && col + cs > nt)
                col = nt - cs;
            if (rs > 64)
                rs = 64;
            if (row > 1024)
                row = 1024;
            if ((pass == 0) != (col >= 0 && row >= 0))
                continue;
            if (cst->grid_dense && row < 0 && col < 0)
                cur_r = cur_c = 0;              /* dense: fill the earliest hole */
            /* find a free spot */
            int r = row >= 0 ? row : cur_r, cc = col >= 0 ? col : row >= 0 ? 0 : cur_c;
            if (col < 0 && cc + cs > nt) {
                cc = 0;
                if (row < 0)
                    r++;
            }
            for (;;) {
                if (r + rs > rows_cap) {
                    int ncap = (r + rs) * 2;
                    uint8_t *no = realloc(occ, (size_t)(ncap * nt));
                    if (!no) {
                        free(occ);
                        return nrows;
                    }
                    memset(no + rows_cap * nt, 0, (size_t)((ncap - rows_cap) * nt));
                    occ = no;
                    rows_cap = ncap;
                }
                int fits = pass == 0;
                if (!fits) {
                    fits = 1;
                    for (int a = r; a < r + rs && fits; a++)
                        for (int b = cc; b < cc + cs && fits; b++)
                            fits = !occ[a * nt + b];
                }
                if (fits)
                    break;
                if (col >= 0) {
                    r++;
                } else if (++cc + cs > nt) {
                    cc = 0;
                    if (row >= 0)
                        break;                  /* the row is full: overlap */
                    r++;
                }
            }
            for (int a = r; a < r + rs; a++)
                for (int b = cc; b < cc + cs; b++)
                    occ[a * nt + b] = 1;
            t->row = r, t->col = cc, t->rspan = rs, t->cspan = cs;
            if (r + rs > nrows)
                nrows = r + rs;
            if (pass == 1 && row < 0) {
                cur_r = r;
                cur_c = cc + cs;
            }
        }
    }
    free(occ);
    return nrows;
}

static int layout_grid(struct lctx *c, struct node *n, int x, int y, int w, int defh, int minh)
{
    const struct style *st = n->style;
    struct fitem *it;
    int ni = container_items(n, &it);
    if (ni <= 0) {
        free(it);
        return 0;
    }
    int cgap = len_px(st->col_gap, w), rgap = len_px(st->row_gap, w);
    struct track tr[MAX_TRACKS];
    int nt = grid_tracks(st->grid_cols, w, cgap, ni, tr);
    for (int i = 0; i < ni; i++)
        item_edges(&it[i], w);
    int nrows = grid_place(it, ni, nt, st);
    const struct gridspec *rspec = st->grid_rows, *aspec = st->grid_auto_rows;
    int erows = rspec ? rspec->n : 0;
    if (nrows < erows)
        nrows = erows;
    if (nrows <= 0) {
        free(it);
        return 0;
    }
    int colw[MAX_TRACKS], colx[MAX_TRACKS + 1];
    size_columns(c, tr, nt, it, ni, w, w - cgap * (nt - 1), colw);
    colx[0] = 0;
    for (int k = 0; k < nt; k++)
        colx[k + 1] = colx[k] + colw[k] + cgap;

    int *rowh = calloc((size_t)(nrows + 1) * 2, sizeof(int));
    if (!rowh) {
        free(it);
        return 0;
    }
    int *rowy = rowh + nrows + 1;
    for (int i = 0; i < ni; i++) {
        struct fitem *t = &it[i];
        int cw = colx[t->col + t->cspan] - colx[t->col] - cgap;
        int js = item_justify(st, t), bw;
        if (t->kind == FI_REPLACED)
            bw = -1;
        else if (js == AL_STRETCH && (anonymous(t) || t->st->width.unit == U_AUTO) && !t->aml && !t->amr)
            bw = stretch_width(t, cw);
        else
            bw = fit_width(c, t, cw);
        int h = item_layout(c, t, bw, cw);
        if (t->rspan == 1 && t->mt + h + t->mb > rowh[t->row])
            rowh[t->row] = t->mt + h + t->mb;
    }
    for (int i = 0; i < ni; i++) {              /* rows spanned by taller items grow */
        struct fitem *t = &it[i];
        if (t->rspan == 1)
            continue;
        int have = rgap * (t->rspan - 1);
        for (int r = t->row; r < t->row + t->rspan; r++)
            have += rowh[r];
        if (t->mt + t->bh + t->mb > have)
            rowh[t->row + t->rspan - 1] += t->mt + t->bh + t->mb - have;
    }
    /* row sizes from grid-template-rows and grid-auto-rows */
    int sumfr = 0, nauto = 0;
    uint8_t rkind[1100];                        /* 0 auto, 1 fixed, 2 fr */
    int rweight[1100];
    for (int r = 0; r < nrows && r < 1100; r++) {
        const struct track *t = r < erows ? &rspec->t[r]
                              : aspec && aspec->n ? &aspec->t[(r - erows) % aspec->n] : NULL;
        rkind[r] = 0;
        rweight[r] = 0;
        if (t) {
            int lo = t->min == TK_PX ? t->vmin : t->min == TK_PCT && defh >= 0 ? muldiv(defh, t->vmin, 10000) : -1;
            int hi = t->max == TK_PX ? t->vmax : t->max == TK_PCT && defh >= 0 ? muldiv(defh, t->vmax, 10000) : -1;
            if (lo > rowh[r])
                rowh[r] = lo;
            if (hi >= 0 && rowh[r] > hi)
                rowh[r] = hi;
            if (hi >= 0)
                rkind[r] = 1;
            else if (t->max == TK_FR)
                rkind[r] = 2, rweight[r] = t->vmax, sumfr += t->vmax;
        }
        if (!rkind[r])
            nauto++;
    }
    int total = rgap * (nrows - 1);
    for (int r = 0; r < nrows; r++)
        total += rowh[r];
    int target = defh >= 0 ? defh : minh;
    if (target > total && nrows && nrows <= 1100) {
        if (sumfr) {                            /* fr rows share what is left */
            int left = target - total, given = 0, last = -1;
            for (int r = 0; r < nrows; r++)
                if (rkind[r] == 2) {
                    int add = muldiv(left, rweight[r], sumfr);
                    rowh[r] += add;
                    given += add;
                    last = r;
                }
            if (last >= 0)
                rowh[last] += left - given;
            total = target;
        } else if (nauto) {                     /* auto rows stretch to fill */
            int left = target - total, given = 0, last = -1;
            for (int r = 0; r < nrows; r++)
                if (!rkind[r]) {
                    rowh[r] += left / nauto;
                    given += left / nauto;
                    last = r;
                }
            if (last >= 0)
                rowh[last] += left - given;
            total = target;
        }
    }
    rowy[0] = 0;
    for (int r = 0; r < nrows; r++)
        rowy[r + 1] = rowy[r] + rowh[r] + rgap;

    for (int i = 0; i < ni; i++) {
        struct fitem *t = &it[i];
        int cx = x + colx[t->col], cw = colx[t->col + t->cspan] - colx[t->col] - cgap;
        int cy = y + rowy[t->row], ch = rowy[t->row + t->rspan] - rowy[t->row] - rgap;
        int al = item_align(st, t), js = item_justify(st, t), h = t->bh, top, left;
        int outer = t->mt + h + t->mb, oute = t->ml + t->bw + t->mr;
        if (al == AL_STRETCH && t->kind == FI_BLOCK && t->st->height.unit == U_AUTO &&
            !t->amt && !t->amb && ch - t->mt - t->mb > h)
            h = ch - t->mt - t->mb;
        if (t->amt && t->amb)
            top = (ch - outer) / 2 + t->mt;
        else if (t->amt || al == AL_END)
            top = ch - t->mb - h;
        else if (al == AL_CENTER)
            top = (ch - outer) / 2 + t->mt;
        else
            top = t->mt;
        if (t->aml && t->amr)
            left = (cw - oute) / 2 + t->ml;
        else if (t->aml || js == AL_END)
            left = cw - t->mr - t->bw;
        else if (js == AL_CENTER)
            left = (cw - oute) / 2 + t->ml;
        else
            left = t->ml;
        item_place(c, t, cx + left, cy + top, h);
    }
    free(rowh);
    free(it);
    return total;
}

/* The intrinsic widths of a flex or grid container's content. */
static void container_measure(struct lctx *c, struct node *n, int *min, int *max)
{
    const struct style *st = n->style;
    struct fitem *it;
    int ni = container_items(n, &it), mn = 0, mx = 0;
    int row = st->inner == IN_FLEX && !st->flex_column;
    int gap = len_px(st->col_gap, 0);
    int nt = 1, tfixed = 0;
    if (st->inner == IN_GRID && st->grid_cols && st->grid_cols->n) {
        const struct gridspec *g = st->grid_cols;
        nt = g->auto_repeat ? (ni < MAX_TRACKS ? ni : MAX_TRACKS) : g->n;
        for (int k = 0; k < g->n; k++)
            tfixed += track_px(g->t[k].min, g->t[k].vmin, 0);
    }
    int bigmin = 0, bigmax = 0;
    for (int i = 0; i < ni; i++) {
        int a, z;
        item_edges(&it[i], 0);
        item_measure(c, &it[i], &a, &z);
        if (row && !anonymous(&it[i]) && it[i].st->min_width.unit == U_PX && it[i].kind != FI_REPLACED)
            a = it[i].st->min_width.v + it[i].ml + it[i].mr +
                (it[i].st->box_sizing_border ? 0 : it[i].hz);
        else if (row && !anonymous(&it[i]) && it[i].st->clips)
            a = it[i].ml + it[i].mr + it[i].hz;
        if (row) {
            mn = st->flex_wrap ? (a > mn ? a : mn) : mn + a + (i ? gap : 0);
            mx += z + (i ? gap : 0);
        }
        bigmin = a > bigmin ? a : bigmin;
        bigmax = z > bigmax ? z : bigmax;
    }
    if (!row) {
        mn = bigmin;
        mx = bigmax;
        if (st->inner == IN_GRID && nt > 1) {
            int k = nt < ni ? nt : ni;
            mx = bigmax * k + gap * (k - 1);
            if (tfixed + gap * (nt - 1) > mn)
                mn = tfixed + gap * (nt - 1);
            if (mn > mx)
                mx = mn;
        }
    }
    free(it);
    *min = mn;
    *max = mx;
}

/* ---- intrinsic widths ---- */

static void measure(struct lctx *c, struct node *n, int *min, int *max)
{
    if (n->mgen == c->gen) {
        *min = n->mmin;
        *max = n->mmax;
        return;
    }
    const struct style *st = n->style;
    struct boxm b;
    resolve_box(st, 0, -1, &b);
    int edges = b.ml + b.mr + (st->box_sizing_border ? 0 : hz(&b));
    int mn = 0, mx = 0;
    if (is_replaced(n)) {
        int saved = c->measuring, w, h, base;
        c->measuring = 1;
        layout_replaced(c, n, BIG, &w, &h, &base);
        c->measuring = saved;
        mn = mx = w;
        if (relative(st->width.unit) || relative(st->max_width.unit))
            mn = 0;
        edges = 0;
    } else if (st->display == D_TABLE || st->display == D_INLINE_TABLE) {
        int saved = c->measuring;
        c->measuring = 1;
        int mb;
        mn = layout_table(c, n, 0, 0, 0, -2, &mb);     /* -2: report the minimum width */
        mx = layout_table(c, n, 0, 0, 0, -3, &mb);     /* -3: and the maximum */
        c->measuring = saved;
        edges = b.ml + b.mr;
    } else if (st->inner == IN_FLEX || st->inner == IN_GRID) {
        container_measure(c, n, &mn, &mx);
    } else {
        int saved = c->measuring;
        c->measuring = 1;
        struct flow f = { 0 };
        f.st = st;
        f.space_before = 1;
        f.w = BIG;
        add_pseudo(c, &f, n, 0);
        flow_children(c, &f, n);
        add_pseudo(c, &f, n, 1);
        flush_run(c, &f);
        free(f.items);
        c->measuring = saved;
        mn = f.min;
        mx = f.max;
    }
    if (st->width.unit == U_PX && !is_replaced(n) && st->display != D_TABLE &&
        st->display != D_INLINE_TABLE) {
        mn = mx = st->width.v + (st->box_sizing_border ? 0 : hz(&b));
        edges = b.ml + b.mr;
    }
    if (st->max_width.unit == U_PX) {
        int cap = st->max_width.v + (st->box_sizing_border ? 0 : hz(&b));
        if (mx > cap)
            mx = cap > mn ? cap : mn;
    }
    if (st->min_width.unit == U_PX && st->min_width.v > mn)
        mn = st->min_width.v, mx = mx > mn ? mx : mn;
    *min = mn + edges;
    *max = (mx > mn ? mx : mn) + edges;
    n->mmin = *min;
    n->mmax = *max;
    n->mgen = c->gen;
}

/* ---- tables ---- */

struct tcell {
    struct node *n;
    int row, col, colspan, rowspan;
    int min, max, fixed, pct;
    int x, w, y, h;
    int slot;
    struct mark m0, m1;
};

static int attr_int(const struct node *n, const char *name, int def, int lo, int hi)
{
    const char *v = node_attr(n, name);
    int k = v ? atoi(v) : def;
    return k < lo ? lo : k > hi ? hi : k;
}

static int is_cell(const struct node *n)
{
    return n->type == N_ELEMENT && n->style && n->style->display == D_TABLE_CELL;
}

/* A cell with nothing in it: no elements and no text but white space (empty-cells: hide). */
static int cell_is_empty(const struct node *cell)
{
    for (const struct node *k = cell->first; k; k = k->next) {
        if (k->type == N_ELEMENT)
            return 0;
        for (int i = 0; i < k->len; i++)
            if (k->text[i] != ' ' && k->text[i] != '\t' && k->text[i] != '\n' && k->text[i] != '\r' &&
                k->text[i] != '\f')
                return 0;
    }
    return 1;
}

/*
 * table-layout: fixed (with a width): the columns take the widths of the first row's cells, the
 * rest share what is left, whatever the content; with room to spare every column grows alike.
 */
static void fixed_columns(const struct tcell *cells, int ncells, int ncols, int total, int *colw)
{
    int known = 0, unknown = 0;
    for (int k = 0; k < ncols; k++)
        colw[k] = -1;
    for (int i = 0; i < ncells; i++) {
        const struct tcell *t = &cells[i];
        if (t->row != 0 || t->colspan != 1 || colw[t->col] >= 0)
            continue;
        const struct style *cst = t->n->style;
        int edges = cst->box_sizing_border ? 0
                  : cst->border_w[1] + cst->border_w[3] + len_px(cst->padding[1], 0) + len_px(cst->padding[3], 0);
        if (cst->width.unit == U_PX)
            colw[t->col] = cst->width.v + edges;
        else if (cst->width.unit == U_PCT)
            colw[t->col] = muldiv(total, cst->width.v, 10000);
    }
    for (int k = 0; k < ncols; k++) {
        if (colw[k] >= 0)
            known += colw[k];
        else
            unknown++;
    }
    int rest = total - known;
    for (int k = 0; k < ncols; k++)
        if (colw[k] < 0)
            colw[k] = unknown && rest > 0 ? rest / unknown : 0;
    int sum = 0;
    for (int k = 0; k < ncols; k++)
        sum += colw[k];
    if (sum < total && ncols)
        for (int k = 0, extra = total - sum; k < ncols; k++)
            colw[k] += extra / ncols + (k < extra % ncols);
}

/* Gather the rows of a table: rows of row groups, rows, and cells loose in
 * the table (which form rows of their own). Returns the number of cells. */
static int table_cells(struct node *table, struct tcell **out, int *nrows_out, struct node ***rows_out,
                       struct node ***caps_out, int *ncaps_out)
{
    int cap = 64, n = 0, nrows = 0, rcap = 16, ncaps = 0;
    struct tcell *cells = malloc(sizeof(*cells) * (size_t)cap);
    struct node **rows = malloc(sizeof(*rows) * (size_t)rcap);
    struct node **caps = malloc(sizeof(*caps) * 4);
    if (!cells || !rows || !caps) {
        free(cells), free(rows), free(caps);
        return -1;
    }
    int pending[MAX_COLS] = { 0 };

    /* Walk the table's rows in order, wherever they are. Cells loose in the
     * table (display: table-cell on a div) form rows of their own. */
    struct node *stack[4];
    int depth = 0, loose = 0, loose_col = 0;
    struct node *k = table->first;
    while (k || depth) {
        if (!k) {
            k = stack[--depth]->next;
            loose = 0;
            continue;
        }
        if (k->type != N_ELEMENT || !k->style) {
            k = k->next;
            continue;
        }
        int d = k->style->display;
        if (d == D_TABLE_ROW_GROUP && depth < 4) {
            stack[depth++] = k;
            k = k->first;
            loose = 0;
            continue;
        }
        if (d == D_TABLE_CAPTION) {
            if (ncaps < 4)
                caps[ncaps++] = k;
            k = k->next;
            continue;
        }
        if (d != D_TABLE_ROW && d != D_TABLE_CELL) {
            k = k->next;
            continue;
        }
        if (d == D_TABLE_ROW || !loose) {
            if (nrows >= MAX_ROWS)
                break;
            if (nrows == rcap) {
                rcap *= 2;
                struct node **nr = realloc(rows, sizeof(*rows) * (size_t)rcap);
                if (!nr)
                    break;
                rows = nr;
            }
            rows[nrows++] = k;
            for (int col = 0; col < MAX_COLS; col++)
                if (pending[col] > 0)
                    pending[col]--;
            loose = d == D_TABLE_CELL;
            loose_col = 0;
        }
        int r = nrows - 1, col = d == D_TABLE_CELL ? loose_col : 0;
        for (struct node *cell = d == D_TABLE_CELL ? k : k->first; cell;
             cell = d == D_TABLE_CELL ? NULL : cell->next) {
            if (!is_cell(cell))
                continue;
            while (col < MAX_COLS && pending[col] > 0)
                col++;
            if (col >= MAX_COLS)
                break;
            if (n == cap) {
                cap *= 2;
                struct tcell *nc = realloc(cells, sizeof(*cells) * (size_t)cap);
                if (!nc)
                    break;
                cells = nc;
            }
            struct tcell *t = &cells[n++];
            memset(t, 0, sizeof(*t));
            t->n = cell;
            t->row = r;
            t->col = col;
            t->colspan = attr_int(cell, "colspan", 1, 1, MAX_COLS - col);
            t->rowspan = attr_int(cell, "rowspan", 1, 0, 1000);
            if (t->rowspan == 0)
                t->rowspan = 1000;      /* "to the end of the group": clipped below */
            for (int sp = 0; sp < t->colspan; sp++)
                pending[col + sp] = t->rowspan;
            col += t->colspan;
        }
        if (d == D_TABLE_CELL)
            loose_col = col;
        k = k->next;
    }
    for (int i = 0; i < n; i++)
        if (cells[i].row + cells[i].rowspan > nrows)
            cells[i].rowspan = nrows - cells[i].row;
    *out = cells;
    *nrows_out = nrows;
    *rows_out = rows;
    *caps_out = caps;
    *ncaps_out = ncaps;
    return n;
}

static void spread(int *cols, int from, int span, int want, int spacing)
{
    int have = spacing * (span - 1);
    for (int k = 0; k < span; k++)
        have += cols[from + k];
    if (have >= want)
        return;
    int extra = want - have;
    for (int k = 0; k < span; k++)
        cols[from + k] += extra / span + (k < extra % span);
}

/* Share `target` pixels among the columns, starting from their minimums. */
static void distribute(int *colw, const int *mn, const int *mx, const int *pct, int ncols,
                       int target, int stretch)
{
    int sum = 0;
    for (int k = 0; k < ncols; k++)
        sum += colw[k] = mn[k];
    int extra = target - sum;
    if (extra <= 0)
        return;
    /* Percentage columns first. */
    for (int k = 0; k < ncols && extra > 0; k++) {
        if (!pct[k])
            continue;
        int want = target * pct[k] / 10000 - colw[k];
        if (want > extra)
            want = extra;
        if (want > 0)
            colw[k] += want, extra -= want;
    }
    /* Then every other column toward its maximum. */
    int room = 0;
    for (int k = 0; k < ncols; k++)
        if (!pct[k] && mx[k] > colw[k])
            room += mx[k] - colw[k];
    if (room > 0) {
        int give = extra < room ? extra : room, given = 0;
        for (int k = 0; k < ncols; k++) {
            if (pct[k] || mx[k] <= colw[k])
                continue;
            int d = (int)((long)(mx[k] - colw[k]) * give / room);
            colw[k] += d;
            given += d;
        }
        extra -= given;
        for (int k = ncols - 1; k >= 0 && extra > 0 && given < give; k--)
            if (!pct[k] && mx[k] > colw[k]) {
                colw[k]++;
                extra--;
                given++;
            }
    }
    if (extra <= 0 || !stretch)
        return;
    /* A table wider than its content: in proportion to the columns' maximums. */
    int total = 0, autos = 0;
    for (int k = 0; k < ncols; k++)
        if (!pct[k])
            total += mx[k], autos++;
    if (!autos) {
        autos = ncols;
        for (int k = 0; k < ncols; k++)
            total += mx[k];
    }
    int given = 0;
    for (int k = 0; k < ncols; k++) {
        if (pct[k] && autos != ncols)
            continue;
        int d = total ? (int)((long)mx[k] * extra / total) : extra / autos;
        colw[k] += d;
        given += d;
    }
    for (int k = ncols - 1; k >= 0 && given < extra; k--)
        if (!pct[k] || autos == ncols)
            colw[k]++, given++;
}

/*
 * Lay out a table with its border box's top at y. force_bw >= 0 sets the
 * border-box width; -2 and -3 only report the minimum or maximum width.
 */
static int layout_table(struct lctx *c, struct node *n, int x, int y, int cw, int force_bw, int *mb)
{
    const struct style *st = n->style;
    struct boxm b;
    resolve_box(st, cw, -1, &b);
    *mb = b.mb;
    struct tcell *cells;
    struct node **rows, **caps;
    int nrows, ncaps;
    int ncells = table_cells(n, &cells, &nrows, &rows, &caps, &ncaps);
    if (ncells < 0)
        return 0;
    int ncols = 0;
    for (int i = 0; i < ncells; i++)
        if (cells[i].col + cells[i].colspan > ncols)
            ncols = cells[i].col + cells[i].colspan;
    int s = st->border_spacing, edges = hz(&b);
    int *colmin = calloc((size_t)(ncols + 1) * 4, sizeof(int));
    int *colmax = colmin + ncols + 1, *colpct = colmax + ncols + 1, *colw = colpct + ncols + 1;
    if (!colmin) {
        free(cells), free(rows), free(caps);
        return 0;
    }

    /* Column widths from the cells: single-column cells first, then spans. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < ncells; i++) {
            struct tcell *t = &cells[i];
            if ((t->colspan > 1) != pass)
                continue;
            if (!pass) {
                measure(c, t->n, &t->min, &t->max);
                const struct style *cst = t->n->style;
                if (cst->width.unit == U_PX) {
                    int fw = cst->width.v + (cst->box_sizing_border ? 0 : cst->border_w[1] + cst->border_w[3] +
                                             len_px(cst->padding[1], 0) + len_px(cst->padding[3], 0));
                    t->max = fw > t->min ? fw : t->min;
                } else if (cst->width.unit == U_PCT && cst->width.v > colpct[t->col]) {
                    colpct[t->col] = cst->width.v;
                }
                if (t->min > colmin[t->col])
                    colmin[t->col] = t->min;
                if (t->max > colmax[t->col])
                    colmax[t->col] = t->max;
            } else {
                measure(c, t->n, &t->min, &t->max);
                spread(colmin, t->col, t->colspan, t->min, s);
                spread(colmax, t->col, t->colspan, t->max, s);
            }
        }
    }
    int sum_min = s * (ncols + 1), sum_max = s * (ncols + 1);
    for (int k = 0; k < ncols; k++) {
        if (colmax[k] < colmin[k])
            colmax[k] = colmin[k];
        sum_min += colmin[k];
        sum_max += colmax[k];
    }
    for (int i = 0; i < ncaps; i++) {
        int a, z;
        measure(c, caps[i], &a, &z);
        if (a > sum_min)
            sum_min = a;
    }
    if (force_bw == -2 || force_bw == -3) {
        int w = (force_bw == -2 ? sum_min : sum_max) + edges;
        if (st->width.unit == U_PX && st->width.v > w)
            w = st->width.v;
        free(colmin), free(cells), free(rows), free(caps);
        return w;
    }

    /* The table's width. */
    int avail = cw - (st->margin[3].unit == U_AUTO ? 0 : b.ml) - (st->margin[1].unit == U_AUTO ? 0 : b.mr);
    int content, stretch = 0;
    if (force_bw >= 0) {
        content = force_bw - edges;
        stretch = 1;
    } else if (st->width.unit != U_AUTO) {
        content = len_px(st->width, cw) - edges;
        stretch = 1;
    } else {
        content = sum_max < avail - edges ? sum_max : avail - edges;
    }
    int fixed_layout = st->table_fixed && st->width.unit != U_AUTO && ncols;
    if (content < sum_min && !fixed_layout)      /* a fixed table ignores what its content needs */
        content = sum_min;
    if (fixed_layout)
        fixed_columns(cells, ncells, ncols, content - s * (ncols + 1), colw);
    else
        distribute(colw, colmin, colmax, colpct, ncols, content - s * (ncols + 1), stretch);
    int used = s * (ncols + 1);
    for (int k = 0; k < ncols; k++)
        used += colw[k];
    if (used > content)
        content = used;
    int bw = content + edges;
    int ml = force_bw >= 0 || st->margin[3].unit == U_AUTO ? 0 : b.ml;
    int centered = n->parent && n->parent->style && n->parent->style->center_blocks;
    if (force_bw < 0 && ((st->margin[3].unit == U_AUTO && st->margin[1].unit == U_AUTO) || centered))
        ml = (cw - bw) / 2 > 0 ? (cw - bw) / 2 : 0;
    else if (force_bw < 0 && st->margin[3].unit == U_AUTO)
        ml = cw - bw - b.mr > 0 ? cw - bw - b.mr : 0;
    int bx = x + ml, cx = bx + b.bl + b.pl;
    anchors_of(c, n, y);

    int table_slot = c->lo->nitems;
    fill(c, 0, 0, 1, 1, OPAQUE(0), 0);
    if (c->lo->nitems == table_slot)
        table_slot = -1;

    int cy = y + b.bt + b.pt;
    for (int i = 0; i < ncaps; i++) {
        int cmb;
        if (caps[i]->style->caption_bottom)
            continue;                   /* after the rows */
        cy += layout_block_box(c, caps[i], cx, cy, content, -1, &cmb, NULL) + cmb;
    }
    int rows_top = cy;
    cy += s;

    int *row_slot = malloc(sizeof(int) * (size_t)(nrows + 1));
    for (int r = 0; r < nrows && row_slot; r++) {
        const struct style *rst = rows[r]->style;
        row_slot[r] = -1;
        if (rows[r]->style->display == D_TABLE_ROW && ALPHA(rst->bg) && !rst->hidden) {
            row_slot[r] = c->lo->nitems;
            fill(c, 0, 0, 1, 1, rst->bg, 0);
        }
        int rowh = rst->display == D_TABLE_ROW && rst->height.unit == U_PX ? rst->height.v : 0;
        for (int i = 0; i < ncells; i++) {
            struct tcell *t = &cells[i];
            if (t->row != r)
                continue;
            t->x = cx + s;
            for (int k = 0; k < t->col; k++)
                t->x += colw[k] + s;
            t->w = s * (t->colspan - 1);
            for (int k = 0; k < t->colspan; k++)
                t->w += colw[t->col + k];
            t->y = cy;
            int cmb;
            t->m0 = mark_now(c);
            t->h = layout_block_box(c, t->n, t->x, cy, cw, t->w, &cmb, &t->slot);
            t->m1 = mark_now(c);
            const struct style *cst = t->n->style;
            if (cst->height.unit == U_PX && cst->height.v > t->h)
                t->h = cst->height.v;
            if (t->rowspan == 1 && t->h > rowh)
                rowh = t->h;
        }
        /* Cells spanning down to this row may need it taller. */
        for (int i = 0; i < ncells; i++) {
            struct tcell *t = &cells[i];
            if (t->rowspan > 1 && t->row + t->rowspan - 1 == r && t->y + t->h > cy + rowh)
                rowh = t->y + t->h - cy;
        }
        /* Finish the cells that end in this row. */
        for (int i = 0; i < ncells; i++) {
            struct tcell *t = &cells[i];
            if (t->row + t->rowspan - 1 != r)
                continue;
            int full = cy + rowh - t->y;
            int va = t->n->style->vertical_align, dy = 0;
            if (va == VA_MIDDLE)
                dy = (full - t->h) / 2;
            else if (va == VA_BOTTOM)
                dy = full - t->h;
            if (dy > 0) {
                struct mark from = t->m0;
                if (t->slot >= 0 && from.items == t->slot) {
                    from.items++;               /* the background stays where the cell is */
                    if (from.items < t->m1.items && c->lo->items[from.items].kind == DK_BGIMG)
                        from.items++;
                }
                shift(c, from, t->m1, 0, dy);
            }
            if (t->n->style->empty_hide && s > 0 && cell_is_empty(t->n)) {
                if (t->slot >= 0)
                    c->lo->items[t->slot].h = 0, c->lo->items[t->slot].color = TRANSPARENT;
                continue;               /* empty-cells: hide: no background and no border */
            }
            if (t->slot >= 0) {
                c->lo->items[t->slot].h = c->lo->items[t->slot].h ? full : 0;
                bg_picture_finish(c, t->n, t->slot);
            }
            borders(c, t->n->style, t->x, t->y, t->w, full);
        }
        if (row_slot[r] >= 0) {
            struct ditem *d = &c->lo->items[row_slot[r]];
            d->x = cx + s, d->y = cy, d->w = content - 2 * s, d->h = rowh;
        }
        cy += rowh + s;
    }
    if (!nrows)
        cy = rows_top;
    for (int i = 0; i < ncaps; i++) {
        int cmb;
        if (caps[i]->style->caption_bottom)
            cy += layout_block_box(c, caps[i], cx, cy, content, -1, &cmb, NULL) + cmb;
    }
    int bh = cy - y + b.pb + b.bb;
    if (st->height.unit == U_PX && st->height.v > bh)
        bh = st->height.v;
    if (table_slot >= 0) {
        struct ditem *d = &c->lo->items[table_slot];
        int paint = ALPHA(st->bg) && !st->hidden;
        d->x = bx, d->y = y, d->w = paint ? bw : 0, d->h = paint ? bh : 0;
        d->color = st->bg;
    }
    borders(c, st, bx, y, bw, bh);
    outline_box(c, st, bx, y, bw, bh);
    free(row_slot);
    free(colmin), free(cells), free(rows), free(caps);
    return bh;
}

/* ---- the document ---- */

/* Paint order: boxes with a negative z-index first, then the rest in document order, then positive ones. */
static void apply_z_order(struct lctx *c)
{
    struct layout *lo = c->lo;
    if (!c->nzr)
        return;
    struct ditem *out = arena_alloc(c->a, sizeof(struct ditem) * (size_t)(lo->nitems ? lo->nitems : 1));
    char *taken = arena_alloc(c->a, (size_t)lo->nitems + 1);
    int *order = arena_alloc(c->a, sizeof(int) * (size_t)c->nzr);
    if (!out || !taken || !order)
        return;
    int n = 0, no = 0;
    for (int i = 0; i < c->nzr; i++) {          /* outermost ranges only */
        int nested = 0;
        for (int j = 0; j < c->nzr && !nested; j++)
            nested = j != i && c->zr[j].from <= c->zr[i].from && c->zr[i].to <= c->zr[j].to &&
                     (c->zr[j].from < c->zr[i].from || c->zr[i].to < c->zr[j].to || j < i);
        if (!nested && c->zr[i].to > c->zr[i].from)
            order[no++] = i;
    }
    for (int i = 1; i < no; i++) {              /* by z, keeping document order among equals */
        int t = order[i], j = i;
        while (j > 0 && c->zr[order[j - 1]].z > c->zr[t].z) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = t;
    }
    for (int k = 0; k < no; k++)
        for (int i = c->zr[order[k]].from; i < c->zr[order[k]].to; i++)
            taken[i] = 1;
    for (int k = 0; k < no; k++)
        if (c->zr[order[k]].z < 0)
            for (int i = c->zr[order[k]].from; i < c->zr[order[k]].to && n < lo->nitems; i++)
                out[n++] = lo->items[i];
    for (int i = 0; i < lo->nitems; i++)
        if (!taken[i] && n < lo->nitems)
            out[n++] = lo->items[i];
    for (int k = 0; k < no; k++)
        if (c->zr[order[k]].z > 0)
            for (int i = c->zr[order[k]].from; i < c->zr[order[k]].to && n < lo->nitems; i++)
                out[n++] = lo->items[i];
    if (n == lo->nitems)
        memcpy(lo->items, out, sizeof(struct ditem) * (size_t)n);
}

void layout_document(struct arena *a, struct layout *lo, struct node *root, int vw, int vh)
{
    memset(lo, 0, sizeof(*lo));
    lo->canvas = OPAQUE(0xFFFFFF);
    if (!root || !root->style)
        return;
    struct shadow_swap *composed = shadow_compose(a, root);
    struct lctx c = { .a = a, .lo = lo, .vw = vw, .vh = vh, .gen = ++layout_gen };
    lo->vw = vw;
    lo->vh = vh;
    lo->dyn = arena_alloc(a, sizeof(struct dynbox) * MAX_DYNBOX);
    lo->sb = arena_alloc(a, sizeof(struct sbox) * MAX_SBOX);
    /* The root's background, or else the body's, covers the whole canvas. */
    struct node *body = NULL;
    for (struct node *k = root->first; k; k = k->next)
        if (k->type == N_ELEMENT && k->tag == T_BODY)
            body = k;
    if (ALPHA(root->style->bg)) {
        lo->canvas = root->style->bg;
        c.skip_bg = root;
    } else if (body && body->style && ALPHA(body->style->bg)) {
        lo->canvas = body->style->bg;
        c.skip_bg = body;
    }
    if (ALPHA(lo->canvas) < 255)
        lo->canvas = OPAQUE(0xFFFFFF);
    int mb;
    int h = layout_block_box(&c, root, 0, 0, vw, -1, &mb, NULL);
    lo->height = h + (mb > 0 ? mb : 0);
    lo->width = vw;
    apply_z_order(&c);
    sbox_finalize(lo);
    for (int i = 0; i < lo->nitems; i++) {
        const struct ditem *d = &lo->items[i];
        if (d->dyn && under_fixed(lo, d->dyn))      /* a fixed box is in the window, not in the page */
            continue;
        if (d->sb)                                  /* what a box clips does not stretch the page */
            continue;
        if (d->w > 0 && d->x + d->w > lo->width && d->x < BIG)
            lo->width = d->x + d->w;
        if (d->h > 0 && d->y + d->h > lo->height && d->y < BIG)
            lo->height = d->y + d->h;
    }
    shadow_restore(composed);
}

const struct hotspot *layout_hit(const struct layout *lo, const struct hotspot *h, int n, int x, int y,
                                 int sx, int sy)
{
    for (int i = n - 1; i >= 0; i--) {
        int dx = 0, dy = 0;
        if (h[i].dyn)
            layout_dyn_offset(lo, h[i].dyn, sx, sy, &dx, &dy);
        if (h[i].node && h[i].node->style && h[i].node->style->pointer_none)
            continue;                   /* pointer-events: none */
        if (h[i].sb) {                  /* inside scrolling boxes: moved by them, and only where they show it */
            int bx, by;
            struct rect clip;
            layout_sb_view(lo, h[i].sb, sx, sy, &bx, &by, &clip);
            if (!rect_has(clip, x, y))
                continue;
            dx += bx, dy += by;
        }
        if (x >= h[i].x + dx && x < h[i].x + dx + h[i].w && y >= h[i].y + dy && y < h[i].y + dy + h[i].h)
            return &h[i];
    }
    return NULL;
}

/* ---- painting ---- */

/* over (0xAARRGGBB) on under: (o * a + u * (255 - a)) / 255 in each
 * channel, in MMX registers, so loops of these end with simd_done(). */
static inline uint32_t blend_px(uint32_t under, uint32_t over)
{
    uint32_t a = ALPHA(over);
    if (a == 255)
        return over & 0xFFFFFF;
    return px_blend255(under, over, a, 0) & 0xFFFFFF;
}

static uint32_t blend(uint32_t under, uint32_t over)
{
    uint32_t c = blend_px(under, over);
    simd_done();
    return c;
}

static void fill_blend(struct surface *s, int x, int y, int w, int h, uint32_t color, int dash)
{
    struct rect r = rect_intersect(mkrect(x, y, w, h), s->clip);
    if (rect_empty(r))
        return;
    int a = (int)ALPHA(color);
    if (a == 255 && !dash) {
        gfx_fill(s, r.x, r.y, r.w, r.h, color & 0xFFFFFF);
        return;
    }
    int horizontal = w >= h;
    for (int j = r.y; j < r.y + r.h; j++) {
        uint32_t *row = s->px + j * s->stride;
        for (int i = r.x; i < r.x + r.w; i++) {
            if (dash) {
                int along = horizontal ? i - x : j - y;
                int period = dash == 1 ? 2 * (horizontal ? h : w) : 6 * (horizontal ? h : w);
                if (period < 2)
                    period = 2;
                if (along % period >= period / 2)
                    continue;
            }
            row[i] = blend_px(row[i], color);
        }
    }
    simd_done();
}

static unsigned isqrt(unsigned v)
{
    unsigned r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* How much of pixel (i, j) a w x h box with these corner radii covers, out of 256. */
static int coverage(int i, int j, int w, int h, const int *rad)
{
    if (i < 0 || j < 0 || i >= w || j >= h)
        return 0;
    int r, cx, cy;
    if (i < rad[0] && j < rad[0])
        r = rad[0], cx = r, cy = r;
    else if (i >= w - rad[1] && j < rad[1])
        r = rad[1], cx = w - r, cy = r;
    else if (i >= w - rad[2] && j >= h - rad[2])
        r = rad[2], cx = w - r, cy = h - r;
    else if (i < rad[3] && j >= h - rad[3])
        r = rad[3], cx = r, cy = h - r;
    else
        return 256;
    int dx = (2 * i + 1 - 2 * cx) * 8, dy = (2 * j + 1 - 2 * cy) * 8;   /* sixteenths */
    int cov = r * 16 + 8 - (int)isqrt((unsigned)(dx * dx + dy * dy));
    return cov <= 0 ? 0 : cov >= 16 ? 256 : cov * 16;
}

static uint32_t with_alpha(uint32_t c, int cov)
{
    return (c & 0xFFFFFF) | (uint32_t)((int)ALPHA(c) * cov / 256) << 24;
}

/* k of the way from a to b, in n steps. */
static uint32_t lerp(uint32_t a, uint32_t b, int k, int n)
{
    int t = n > 1 ? k * 256 / (n - 1) : 0;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int u = (int)(a >> sh & 0xFF), v = (int)(b >> sh & 0xFF);
        out |= (uint32_t)(u + (v - u) * t / 256) << sh;
    }
    return out;
}

/* A fill with rounded corners, a gradient or a border ring. */
static void fill_rich(struct surface *s, const struct ditem *d, int x, int y)
{
    struct rect r = rect_intersect(mkrect(x, y, d->w, d->h), s->clip);
    if (rect_empty(r))
        return;
    int ring = d->ring, inner[4], maxr = 0;
    for (int k = 0; k < 4; k++) {
        inner[k] = d->rad[k] > ring ? d->rad[k] - ring : 0;
        if (d->rad[k] > maxr)
            maxr = d->rad[k];
    }
    if (maxr < ring)
        maxr = ring;
    for (int j = r.y; j < r.y + r.h; j++) {
        int bj = j - y;
        uint32_t *row = s->px + j * s->stride;
        uint32_t rowc = d->grad == 1 ? lerp(d->g0, d->g1, bj, d->h) : 0;
        int edge_row = bj < maxr || bj >= d->h - maxr;
        if (!edge_row && !ring && d->grad != 2) {
            /* away from the corners a row is one color */
            uint32_t col = d->grad && ALPHA(rowc) == 255 ? rowc : d->color;
            if (d->grad && ALPHA(rowc) < 255)
                col = 0;
            if (ALPHA(col) == 255) {
                gfx_fill(s, r.x, j, r.w, 1, col & 0xFFFFFF);
                continue;
            }
        }
        for (int i = r.x; i < r.x + r.w; i++) {
            int bi = i - x, out, in;
            if (!edge_row && bi >= maxr && bi < d->w - maxr) {
                if (!ALPHA(d->color) && !d->grad) {     /* a ring's empty middle */
                    i = x + d->w - maxr - 1;
                    continue;
                }
                out = in = 256;
            } else {
                out = coverage(bi, bj, d->w, d->h, d->rad);
                in = ring ? coverage(bi - ring, bj - ring, d->w - 2 * ring, d->h - 2 * ring, inner) : out;
                if (in > out)
                    in = out;
            }
            if (!out)
                continue;
            uint32_t px = row[i];
            if (in) {
                if (ALPHA(d->color))
                    px = blend_px(px, with_alpha(d->color, in));
                if (d->grad)
                    px = blend_px(px, with_alpha(d->grad == 1 ? rowc : lerp(d->g0, d->g1, bi, d->w), in));
            }
            if (ring && out > in)
                px = blend_px(px, with_alpha(d->ring_color, out - in));
            row[i] = px & 0xFFFFFF;
        }
    }
    simd_done();
}

/* An image with its corners rounded off, or put (object-fit) somewhere other than filling its box. */
static void draw_image_round(struct surface *s, const struct ditem *d, int x, int y)
{
    const struct image *img = d->img;
    struct rect r = rect_intersect(mkrect(x, y, d->w, d->h), s->clip);
    if (rect_empty(r) || !img || img->w <= 0 || img->h <= 0)
        return;
    int fx = x, fy = y, fw = d->w, fh = d->h;
    if (d->fit_w > 0 && d->fit_h > 0)
        fx = x + d->fit_x, fy = y + d->fit_y, fw = d->fit_w, fh = d->fit_h;
    int rounded = d->rad[0] || d->rad[1] || d->rad[2] || d->rad[3];
    for (int j = r.y; j < r.y + r.h; j++) {
        if (j < fy || j >= fy + fh)
            continue;
        const uint32_t *src = img->px + ((j - fy) * img->h / fh) * img->w;
        uint32_t *dst = s->px + j * s->stride;
        for (int i = r.x; i < r.x + r.w; i++) {
            if (i < fx || i >= fx + fw)
                continue;
            int cov = rounded ? coverage(i - x, j - y, d->w, d->h, d->rad) : 256;
            if (!cov)
                continue;
            uint32_t p = src[((i - fx) * img->w / fw)];
            uint32_t a = (p >> 24) * (uint32_t)cov / 256;
            if (a == 255)
                dst[i] = p & 0xFFFFFF;
            else if (a)
                dst[i] = blend_px(dst[i], (p & 0xFFFFFF) | a << 24);
        }
    }
    simd_done();
}

/* A background picture: tiles of bgp from its corner, over the item's box (rounded if it is), then a gradient. */
static void draw_bgimg(struct surface *s, const struct ditem *d, int x, int y)
{
    const struct bgpaint *bp = d->bgp;
    struct rect r = rect_intersect(mkrect(x, y, d->w, d->h), s->clip);
    if (rect_empty(r))
        return;
    if (bp && bp->img && bp->img->w > 0 && bp->img->h > 0 && bp->tw > 0 && bp->th > 0) {
        const struct image *img = bp->img;
        int *xs = malloc(sizeof(int) * (size_t)r.w);
        if (xs) {
            for (int i = 0; i < r.w; i++) {             /* the picture's column under each pixel, or -1 */
                int rel = r.x + i - (x + bp->tx);
                if (bp->norep_x && (rel < 0 || rel >= bp->tw)) {
                    xs[i] = -1;
                    continue;
                }
                rel %= bp->tw;
                if (rel < 0)
                    rel += bp->tw;
                xs[i] = (rel * img->w / bp->tw);
            }
            int rounded = d->rad[0] || d->rad[1] || d->rad[2] || d->rad[3];
            for (int j = r.y; j < r.y + r.h; j++) {
                int rel = j - (y + bp->ty);
                if (bp->norep_y && (rel < 0 || rel >= bp->th))
                    continue;
                rel %= bp->th;
                if (rel < 0)
                    rel += bp->th;
                const uint32_t *src = img->px + (rel * img->h / bp->th) * img->w;
                uint32_t *dst = s->px + j * s->stride;
                for (int i = 0; i < r.w; i++) {
                    if (xs[i] < 0)
                        continue;
                    uint32_t p = src[xs[i]];
                    uint32_t a = p >> 24;
                    if (rounded) {
                        int cov = coverage(r.x + i - x, j - y, d->w, d->h, d->rad);
                        a = a * (uint32_t)cov / 256;
                    }
                    if (a == 255)
                        dst[r.x + i] = p & 0xFFFFFF;
                    else if (a)
                        dst[r.x + i] = blend_px(dst[r.x + i], (p & 0xFFFFFF) | a << 24);
                }
            }
            free(xs);
            simd_done();
        }
    }
    if (d->grad)
        fill_rich(s, d, x, y);
}

void draw_image_scaled(struct surface *s, const struct image *img, int x, int y, int w, int h)
{
    struct rect r = rect_intersect(mkrect(x, y, w, h), s->clip);
    if (rect_empty(r) || !img || img->w <= 0 || img->h <= 0)
        return;
    for (int j = r.y; j < r.y + r.h; j++) {
        int sy = (j - y) * img->h / h;
        const uint32_t *src = img->px + sy * img->w;
        uint32_t *dst = s->px + j * s->stride;
        for (int i = r.x; i < r.x + r.w; i++) {
            uint32_t p = src[(i - x) * img->w / w];
            uint32_t a = p >> 24;
            if (a == 255)
                dst[i] = p & 0xFFFFFF;
            else if (a)
                dst[i] = blend_px(dst[i], p);
        }
    }
    simd_done();
}

static void disc(struct surface *s, int x, int y, int d, uint32_t color, int hollow)
{
    int r2 = d * d;
    for (int j = 0; j < d; j++)
        for (int i = 0; i < d; i++) {
            int dx = 2 * i + 1 - d, dy = 2 * j + 1 - d, q = dx * dx + dy * dy;
            if (q <= r2 && (!hollow || q >= (d - 2) * (d - 2)))
                gfx_pixel(s, x + i, y + j, color & 0xFFFFFF);
        }
}

static void draw_text_clipped(struct surface *s, const struct wfont *f, struct rect box, int x, int y,
                              const char *t, int n, uint32_t color)
{
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, box);
    if (!rect_empty(s->clip))
        wfont_draw(s, f, x, y, t, n, color & 0xFFFFFF);
    s->clip = saved;
}

/* An attribute's number times 1000 ("0.75" is 750), or def. */
static int milli_str(const char *v, int def)
{
    if (!v)
        return def;
    while (*v == ' ')
        v++;
    int neg = *v == '-', whole = 0, frac = 0, scale = 100;
    if (*v == '-' || *v == '+')
        v++;
    if (!(*v >= '0' && *v <= '9') && *v != '.')
        return def;
    while (*v >= '0' && *v <= '9')
        whole = whole * 10 + (*v++ - '0');
    if (*v == '.')
        for (v++; *v >= '0' && *v <= '9'; v++, scale /= 10)
            frac += (*v - '0') * scale;
    int r = whole * 1000 + frac;
    return neg ? -r : r;
}

static int milli_attr(const struct node *n, const char *name, int def)
{
    return milli_str(node_attr(n, name), def);
}

/* A value in thousandths as text: "5", "2.5", "0.125". */
static void milli_text(int v, char *out, int size)
{
    int neg = v < 0, whole, frac;
    if (neg)
        v = -v;
    whole = v / 1000, frac = v % 1000;
    int o = snprintf(out, (size_t)size, "%s%d", neg ? "-" : "", whole);
    if (frac && o < size - 4) {
        o += snprintf(out + o, (size_t)(size - o), ".%03d", frac);
        while (out[o - 1] == '0')
            out[--o] = 0;
    }
}

int range_fraction(const struct node *n)
{
    int lo = milli_attr(n, "min", 0), hi = milli_attr(n, "max", 100000);
    int v = milli_str(n->value, lo + (hi - lo) / 2);       /* the middle, if nothing is set */
    if (hi <= lo)
        return 0;
    v = v < lo ? lo : v > hi ? hi : v;
    return muldiv(v - lo, 1000, hi - lo);
}

void range_value_at(const struct node *n, int fraction, char *out, int size)
{
    int lo = milli_attr(n, "min", 0), hi = milli_attr(n, "max", 100000);
    const char *sa = node_attr(n, "step");
    int step = sa && !strcasecmp(sa, "any") ? 1 : milli_attr(n, "step", 1000);
    if (step <= 0)
        step = 1000;
    if (hi < lo)
        hi = lo;
    fraction = fraction < 0 ? 0 : fraction > 1000 ? 1000 : fraction;
    int v = lo + muldiv(hi - lo, fraction, 1000);
    v = lo + ((v - lo + step / 2) / step) * step;       /* to the nearest step from the minimum */
    if (v > hi)
        v = lo + ((hi - lo) / step) * step;
    milli_text(v < lo ? lo : v, out, size);
}

void range_nudge(const struct node *n, int dir, char *out, int size)
{
    int lo = milli_attr(n, "min", 0), hi = milli_attr(n, "max", 100000);
    const char *sa = node_attr(n, "step");
    int step = sa && !strcasecmp(sa, "any") ? 1000 : milli_attr(n, "step", 1000);
    if (step <= 0)
        step = 1000;
    int v = milli_str(n->value, lo + (hi - lo) / 2);
    if (dir == -2)
        v = lo;
    else if (dir == 2)
        v = hi;
    else
        v += dir * step;
    v = v < lo ? lo : v > hi ? hi : v;
    milli_text(v, out, size);
}

const char *input_pattern(const char *type)
{
    if (!strcasecmp(type, "date"))
        return "yyyy-mm-dd";
    if (!strcasecmp(type, "time"))
        return "hh:mm";
    if (!strcasecmp(type, "datetime-local"))
        return "yyyy-mm-ddThh:mm";
    if (!strcasecmp(type, "month"))
        return "yyyy-mm";
    if (!strcasecmp(type, "week"))
        return "yyyy-Www";
    return NULL;
}

/* <progress> and <meter>: a sunken trough with a bar. */
static void draw_gauge(struct surface *s, const struct ditem *d, int x, int y)
{
    const struct node *n = d->node;
    struct rect r = mkrect(x, y, d->w, d->h);
    gfx_fill(s, x, y, d->w, d->h, C_WHITE);
    gfx_bevel(s, r, BEVEL_SUNKEN);
    struct rect in = mkrect(x + 2, y + 2, d->w - 4, d->h - 4);
    if (in.w <= 0 || in.h <= 0)
        return;
    if (n->tag == T_PROGRESS) {
        int max = milli_attr(n, "max", 1000);
        if (max <= 0)
            max = 1000;
        if (!node_attr(n, "value")) {           /* no value: working on it; a few blocks */
            for (int bx = in.x + 4; bx + 8 <= in.x + in.w && bx < in.x + in.w / 2 + 16; bx += 10)
                if (bx > in.x + in.w / 2 - 20)
                    gfx_fill(s, bx, in.y, 8, in.h, C_NAVY);
            return;
        }
        int v = milli_attr(n, "value", 0);
        v = v < 0 ? 0 : v > max ? max : v;
        int full = muldiv(in.w, v, max);
        for (int bx = in.x; bx < in.x + full; bx += 10)     /* blocks, as in Windows 95 */
            gfx_fill(s, bx, in.y, in.x + full - bx < 8 ? in.x + full - bx : 8, in.h, C_NAVY);
        return;
    }
    int min = milli_attr(n, "min", 0), max = milli_attr(n, "max", 1000);
    if (max <= min)
        max = min + 1000;
    int v = milli_attr(n, "value", 0);
    v = v < min ? min : v > max ? max : v;
    int low = milli_attr(n, "low", min), high = milli_attr(n, "high", max);
    int opt = milli_attr(n, "optimum", (min + max) / 2);
    low = low < min ? min : low > max ? max : low;
    high = high < low ? low : high > max ? max : high;
    #define REGION(x) ((x) < low ? 0 : (x) > high ? 2 : 1)
    int dist = REGION(v) - REGION(opt);
    #undef REGION
    uint32_t col = dist == 0 ? 0x00A000 : dist == 1 || dist == -1 ? 0xE0C000 : 0xD00000;
    int full = muldiv(in.w, v - min, max - min);
    gfx_fill(s, in.x, in.y, full, in.h, col);
}

static void draw_control(struct surface *s, const struct ditem *d, int x, int y, int focused, int caret_on)
{
    struct node *n = d->node;
    const struct wfont *f = d->font;
    int fh = wfont_height(f);
    struct rect r = mkrect(x, y, d->w, d->h);
    const char *type = n->tag == T_INPUT ? input_type(n) : "";
    const char *value = n->value ? n->value : "";

    if (n->tag == T_PROGRESS || n->tag == T_METER) {
        draw_gauge(s, d, x, y);
        return;
    }
    uint32_t ph_color = n->pseudo_st[PE_PLACEHOLDER - 1] ? n->pseudo_st[PE_PLACEHOLDER - 1]->color
                                                         : OPAQUE(0x808080);

    if (n->tag == T_INPUT && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) {
        if (!strcasecmp(type, "radio")) {
            disc(s, x, y, d->w, OPAQUE(0x808080), 0);
            disc(s, x + 1, y + 1, d->w - 2, OPAQUE(0xFFFFFF), 0);
            if (n->checked)
                disc(s, x + d->w / 2 - 2, y + d->h / 2 - 2, 4, OPAQUE(0), 0);
        } else {
            gfx_fill(s, x, y, d->w, d->h, C_WHITE);
            gfx_bevel(s, r, BEVEL_SUNKEN);
            if (n->checked)
                for (int k = 0; k < 7; k++) {   /* a tick */
                    int px = x + 3 + k, py = y + (k < 3 ? 5 + k : 9 - (k - 2));
                    gfx_fill(s, px, py, 1, 3, C_BLACK);
                }
        }
        if (focused)
            gfx_focus_rect(s, mkrect(x - 2, y - 2, d->w + 4, d->h + 4));
        return;
    }
    if (n->tag == T_INPUT && !strcasecmp(type, "range")) {
        int mid = y + d->h / 2, left = x + 6, track = d->w - 12;
        gfx_fill(s, left, mid - 2, track, 4, C_LIGHT);
        gfx_bevel(s, mkrect(left, mid - 2, track, 4), BEVEL_SUNKEN);
        int tx = left + muldiv(track - 10, range_fraction(n), 1000);
        struct rect th = mkrect(tx, y + 1, 10, d->h - 2);
        gfx_fill(s, th.x, th.y, th.w, th.h, C_FACE);
        gfx_bevel(s, th, BEVEL_RAISED);
        if (focused)
            gfx_focus_rect(s, mkrect(x, y, d->w, d->h));
        return;
    }
    if (n->tag == T_INPUT && !strcasecmp(type, "color")) {
        uint32_t rgb = 0;
        const char *cv = n->value && n->value[0] ? n->value : "#000000";
        if (*cv == '#')
            cv++;
        for (int k = 0; k < 6 && cv[k]; k++)
            rgb = rgb << 4 | (uint32_t)((cv[k] >= '0' && cv[k] <= '9') ? cv[k] - '0'
                              : (cv[k] | 32) >= 'a' && (cv[k] | 32) <= 'f' ? (cv[k] | 32) - 'a' + 10 : 0);
        gfx_fill(s, x, y, d->w, d->h, C_FACE);
        gfx_bevel(s, r, BEVEL_RAISED);
        gfx_fill(s, x + 4, y + 4, d->w - 8, d->h - 8, rgb);
        gfx_frame(s, x + 4, y + 4, d->w - 8, d->h - 8, C_SHADOW);
        if (focused)
            gfx_focus_rect(s, mkrect(x + 2, y + 2, d->w - 4, d->h - 4));
        return;
    }
    if (n->tag == T_INPUT && !strcasecmp(type, "file")) {
        const char *label = control_label(n);
        int tw = wfont_width(f, label, (int)strlen(label)), bw = tw + ZOOM(16);
        struct rect b = mkrect(x, y, bw < d->w ? bw : d->w, d->h);
        gfx_fill(s, b.x, b.y, b.w, b.h, C_FACE);
        gfx_bevel(s, b, BEVEL_RAISED);
        draw_text_clipped(s, f, b, x + (bw - tw) / 2, y + (d->h - fh) / 2, label, (int)strlen(label), 0);
        const char *path = n->value && n->value[0] ? n->value : "No file chosen";
        const char *base = strrchr(path, '/');
        base = base && n->value && n->value[0] ? base + 1 : path;
        struct rect nr = mkrect(x + bw + ZOOM(6), y, d->w - bw - ZOOM(6), d->h);
        if (nr.w > 0)
            draw_text_clipped(s, f, nr, nr.x, y + (d->h - fh) / 2, base, (int)strlen(base),
                              n->value && n->value[0] ? 0 : OPAQUE(0x808080));
        if (focused)
            gfx_focus_rect(s, mkrect(x + 3, y + 3, b.w - 6, b.h - 6));
        return;
    }
    if (n->tag == T_INPUT && is_button_input(type)) {
        gfx_fill(s, x, y, d->w, d->h, C_FACE);
        gfx_bevel(s, r, BEVEL_RAISED);
        const char *label = control_label(n);
        int tw = wfont_width(f, label, (int)strlen(label));
        draw_text_clipped(s, f, r, x + (d->w - tw) / 2, y + (d->h - fh) / 2, label, (int)strlen(label), 0);
        if (focused)
            gfx_focus_rect(s, mkrect(x + 3, y + 3, d->w - 6, d->h - 6));
        return;
    }
    gfx_fill(s, x, y, d->w, d->h, C_WHITE);
    gfx_bevel(s, r, BEVEL_SUNKEN);
    struct rect inner = mkrect(x + 3, y + 2, d->w - 6, d->h - 4);
    if (n->tag == T_SELECT) {
        struct rect btn = mkrect(x + d->w - 18, y + 2, 16, d->h - 4);
        const char *label = control_label(n);
        inner.w -= 18;
        draw_text_clipped(s, f, inner, inner.x, y + (d->h - fh) / 2, label, (int)strlen(label), 0);
        gfx_fill(s, btn.x, btn.y, btn.w, btn.h, C_FACE);
        gfx_bevel(s, btn, BEVEL_RAISED);
        for (int k = 0; k < 4; k++)
            gfx_hline(s, btn.x + 4 + k, btn.y + btn.h / 2 - 1 + k, 7 - 2 * k, C_BLACK);
        if (focused)
            gfx_focus_rect(s, inner);
        return;
    }
    if (n->tag == T_TEXTAREA) {
        int ly = y + 2, start = 0, len = (int)strlen(value);
        if (!len && !focused && node_attr(n, "placeholder")) {
            const char *ph = node_attr(n, "placeholder");
            draw_text_clipped(s, f, inner, inner.x, ly, ph, (int)strlen(ph), ph_color);
            return;
        }
        for (int i = 0; i <= len; i++) {
            if (i == len || value[i] == '\n') {
                draw_text_clipped(s, f, inner, inner.x, ly, value + start, i - start, 0);
                if (focused && caret_on && n->cursor >= start && n->cursor <= i)
                    gfx_vline(s, inner.x + wfont_width(f, value + start, n->cursor - start), ly, fh, C_BLACK);
                ly += fh;
                start = i + 1;
            }
        }
        return;
    }
    /* A text field: password fields show bullets, empty ones their placeholder. */
    char masked[256];
    const char *shown = value;
    int len = (int)strlen(value);
    if (!strcasecmp(type, "password")) {
        len = len < 255 ? len : 255;
        memset(masked, '*', (size_t)len);
        masked[len] = 0;
        shown = masked;
    }
    int ty = y + (d->h - fh) / 2;
    int cur = n->cursor > len ? len : n->cursor;
    int caret_x = wfont_width(f, shown, cur), scroll = caret_x > inner.w - 2 ? caret_x - inner.w + 2 : 0;
    const char *pattern = input_pattern(type);
    if (!len && !focused && (node_attr(n, "placeholder") || pattern)) {
        const char *ph = node_attr(n, "placeholder") ? node_attr(n, "placeholder") : pattern;
        draw_text_clipped(s, f, inner, inner.x, ty, ph, (int)strlen(ph), ph_color);
    } else {
        draw_text_clipped(s, f, inner, inner.x - scroll, ty, shown, len, 0);
    }
    if (focused && caret_on)
        gfx_vline(s, inner.x + caret_x - scroll, ty, fh, C_BLACK);
}

/* ---- scroll bars of boxes ---- */

#define SB_STEP 40              /* an arrow's step, in pixels */

/* One of a box's scroll bars at its padding box's corner (ox, oy): where it lies, and what it shows. */
static struct rect sbar_geometry(const struct sbox *b, int axis, int ox, int oy, int *page, int *total, int *pos)
{
    if (axis) {
        *page = b->h - (b->bar_x ? SB_BAR : 0), *total = *page + b->max_y, *pos = b->sy;
        return mkrect(ox + b->w - SB_BAR, oy, SB_BAR, *page);
    }
    *page = b->w - (b->bar_y ? SB_BAR : 0), *total = *page + b->max_x, *pos = b->sx;
    return mkrect(ox, oy + b->h - SB_BAR, *page, SB_BAR);
}

/* The thumb's start and length along a bar of this length (arrows excluded from the track). */
static void sbar_thumb_span(int len, int page, int total, int pos, int max, int *start, int *tlen)
{
    int track = len - 2 * SB_BAR;
    *start = SB_BAR, *tlen = 0;
    if (max <= 0 || track < 8 || total <= 0)
        return;
    int t = muldiv(track, page, total);
    if (t < 8)
        t = 8;
    *tlen = t;
    *start = SB_BAR + muldiv(track - t, pos, max);
}

static void sbar_arrow_draw(struct surface *s, struct rect r, int vertical, int second, int enabled)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
    gfx_bevel(s, r, BEVEL_RAISED);
    int mx = r.x + r.w / 2 - 1, my = r.y + r.h / 2 - 1;
    for (int i = 0; i < 4; i++) {
        int k = second ? i : 3 - i;
        for (int shadow = !enabled; shadow >= 0; shadow--) {
            color_t col = shadow ? C_WHITE : enabled ? C_BLACK : C_SHADOW;
            if (vertical)
                gfx_hline(s, mx - 3 + k + shadow, my - 2 + i + shadow, 7 - 2 * k, col);
            else
                gfx_vline(s, mx - 2 + i + shadow, my - 3 + k + shadow, 7 - 2 * k, col);
        }
    }
}

static void draw_sbox_bars(struct surface *s, const struct sbox *b, int ox, int oy)
{
    if (!b->active)
        return;
    for (int axis = 0; axis < 2; axis++) {
        if (!(axis ? b->bar_y : b->bar_x))
            continue;
        int page, total, pos;
        struct rect r = sbar_geometry(b, axis, ox, oy, &page, &total, &pos);
        int max = axis ? b->max_y : b->max_x;
        gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
        gfx_dither(s, r.x, r.y, r.w, r.h, C_WHITE);
        int len = axis ? r.h : r.w;
        if (len < 2 * SB_BAR)
            continue;
        struct rect a1 = mkrect(r.x, r.y, SB_BAR, SB_BAR);
        struct rect a2 = axis ? mkrect(r.x, r.y + r.h - SB_BAR, SB_BAR, SB_BAR)
                              : mkrect(r.x + r.w - SB_BAR, r.y, SB_BAR, SB_BAR);
        sbar_arrow_draw(s, a1, axis, 0, max > 0);
        sbar_arrow_draw(s, a2, axis, 1, max > 0);
        int ts, tl;
        sbar_thumb_span(len, page, total, pos, max, &ts, &tl);
        if (tl) {
            struct rect t = axis ? mkrect(r.x, r.y + ts, SB_BAR, tl) : mkrect(r.x + ts, r.y, tl, SB_BAR);
            gfx_fill(s, t.x, t.y, t.w, t.h, C_FACE);
            gfx_bevel(s, t, BEVEL_RAISED);
        }
    }
    if (b->bar_x && b->bar_y)
        gfx_fill(s, ox + b->w - SB_BAR, oy + b->h - SB_BAR, SB_BAR, SB_BAR, C_FACE);
}

/* ---- selecting text ---- */

/* The character of a text item that x falls at, counted in bytes. */
/* The width of the first n bytes of a text item, with its letter-spacing. */
static int item_width(const struct ditem *d, int n)
{
    int w = wfont_width(d->font, d->text, n);
    return d->ls ? w + utf8_count(d->text, n) * d->ls : w;
}

/* Text that user-select: none keeps out of selections. */
static int selectable(const struct ditem *d)
{
    return !(d->node && d->node->type == N_ELEMENT && d->node->style && d->node->style->select_none);
}

static int char_at(const struct ditem *d, int px)
{
    int prev = 0, pw = 0;
    for (int n = 1; n <= d->len; n++) {
        if (n < d->len && ((unsigned char)d->text[n] & 0xC0) == 0x80)
            continue;                   /* inside a UTF-8 character */
        int w = item_width(d, n);
        if (d->x + w >= px)
            return px - (d->x + pw) < d->x + w - px ? prev : n;
        prev = n;
        pw = w;
    }
    return d->len;
}

static void sel_pos(const struct layout *lo, int px, int py, int *item, int *ch)
{
    int last = -1;
    for (int i = 0; i < lo->nitems; i++) {
        const struct ditem *d = &lo->items[i];
        if (d->kind != DK_TEXT || !d->len || !selectable(d))
            continue;
        last = i;
        int qx = px, qy = py;           /* the point as the item's box has it (it may be scrolled) */
        if (d->sb) {
            int sdx, sdy;
            layout_sb_view(lo, d->sb, 0, 0, &sdx, &sdy, NULL);
            qx -= sdx, qy -= sdy;
        }
        if (d->y + d->h > qy && (d->y > qy || d->x + d->w > qx)) {
            *item = i;
            *ch = d->y <= qy && d->x < qx ? char_at(d, qx) : 0;
            return;
        }
    }
    *item = last;                       /* below everything: the end of the last text */
    *ch = last >= 0 ? lo->items[last].len : 0;
}

void layout_select(struct layout *lo, int ax, int ay, int fx, int fy)
{
    int i1, c1, i2, c2;
    sel_pos(lo, ax, ay, &i1, &c1);
    sel_pos(lo, fx, fy, &i2, &c2);
    lo->sel_on = 0;
    if (i1 < 0 || i2 < 0)
        return;
    if (i1 > i2 || (i1 == i2 && c1 > c2)) {
        int t = i1;
        i1 = i2, i2 = t;
        t = c1, c1 = c2, c2 = t;
    }
    if (i1 == i2 && c1 == c2)
        return;
    lo->sel_on = 1;
    lo->sel_from = i1, lo->sel_to = i2, lo->sel_c0 = c1, lo->sel_c1 = c2;
}

void layout_select_all(struct layout *lo)
{
    int first = -1, last = -1;
    for (int i = 0; i < lo->nitems; i++)
        if (lo->items[i].kind == DK_TEXT && lo->items[i].len && selectable(&lo->items[i])) {
            if (first < 0)
                first = i;
            last = i;
        }
    lo->sel_on = first >= 0;
    lo->sel_from = first, lo->sel_to = last, lo->sel_c0 = 0;
    lo->sel_c1 = last >= 0 ? lo->items[last].len : 0;
}

int layout_selection_text(const struct layout *lo, char *out, int size)
{
    if (!lo->sel_on || size < 2)
        return 0;
    int o = 0;
    const struct ditem *prev = NULL;
    for (int i = lo->sel_from; i <= lo->sel_to && i < lo->nitems; i++) {
        const struct ditem *d = &lo->items[i];
        if (d->kind != DK_TEXT || !d->len || !selectable(d))
            continue;
        int a = i == lo->sel_from ? lo->sel_c0 : 0, b = i == lo->sel_to ? lo->sel_c1 : d->len;
        if (b > d->len)
            b = d->len;
        if (a >= b)
            continue;
        if (prev && o < size - 1) {
            int dy = d->y - prev->y;
            if (dy < 0)
                dy = -dy;
            if (dy > prev->h / 2)
                out[o++] = '\n';
            else if (d->x > prev->x + prev->w && out[o - 1] != ' ')
                out[o++] = ' ';
        }
        for (int k = a; k < b && o < size - 1; k++)
            out[o++] = d->text[k];
        prev = d;
    }
    out[o] = 0;
    return o;
}

/* Found text (the find bar): a yellow box behind each piece, orange for the current match. */
static void paint_found(const struct layout *lo, struct surface *s, const struct ditem *d, int item, int x, int y)
{
    int a = 0, b = lo->nhl;
    while (a < b) {                     /* the first piece in this item (they are in item order) */
        int m = (a + b) / 2;
        if (lo->hl[m].item < item)
            a = m + 1;
        else
            b = m;
    }
    for (; a < lo->nhl && lo->hl[a].item == item; a++) {
        int c0 = lo->hl[a].c0, c1 = lo->hl[a].c1 > d->len ? d->len : lo->hl[a].c1;
        if (c0 >= c1)
            continue;
        int xa = item_width(d, c0), xb = item_width(d, c1);
        gfx_fill(s, x + xa, y, xb - xa, d->h, lo->hl[a].match == lo->hl_cur ? 0xFF9632 : 0xFFE866);
    }
}

/* Text, a character at a time when it has letter-spacing. */
static void draw_text(struct surface *s, const struct ditem *d, int x, int y, const char *text, int len,
                      uint32_t col)
{
    if (!d->ls) {
        wfont_draw(s, d->font, x, y, text, len, col);
        return;
    }
    for (int i = 0; i < len;) {
        int e = i + 1;
        while (e < len && ((unsigned char)text[e] & 0xC0) == 0x80)
            e++;
        x += wfont_draw(s, d->font, x, y, text + i, e - i, col) + d->ls;
        i = e;
    }
}

/* text-shadow: the text again, offset and in the shadow's color; a blur is a few fainter copies round it. */
static void draw_shadow(struct surface *s, const struct ditem *d, int x, int y)
{
    uint32_t col = ALPHA(d->sh_color) == 255 ? d->sh_color : blend(0xFFFFFF, d->sh_color);
    if (d->sh_blur > 0) {
        uint32_t soft = shade(col, 1, 2, 1) & 0xFFFFFF;
        static const int step[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
        for (int k = 0; k < 4; k++)
            draw_text(s, d, x + d->sh_x + step[k][0], y + d->sh_y + step[k][1], d->text, d->len, soft);
    }
    draw_text(s, d, x + d->sh_x, y + d->sh_y, d->text, d->len, col & 0xFFFFFF);
}

/* One decoration line, w wide: solid, double, dotted, dashed or wavy. */
static void deco_line(struct surface *s, int x, int y, int w, int thick, uint32_t col, int style)
{
    switch (style) {
    case DS_DOUBLE:
        gfx_fill(s, x, y, w, thick, col);
        gfx_fill(s, x, y + thick + 1, w, thick, col);
        break;
    case DS_DOTTED:
    case DS_DASHED:
        fill_blend(s, x, y, w, thick, OPAQUE(col), style == DS_DOTTED ? 1 : 2);
        break;
    case DS_WAVY:
        for (int i = 0; i < w; i++)
            gfx_fill(s, x + i, y + (i / 2) % 2, 1, thick, col);
        break;
    default:
        gfx_fill(s, x, y, w, thick, col);
        break;
    }
}

static void draw_decoration(struct surface *s, const struct ditem *d, int x, int y, uint32_t col)
{
    int size = wfont_size(d->font), thick = size >= 24 ? 2 : 1;
    if (d->deco_thickness) thick = d->deco_thickness;
    int asc = wfont_ascent(d->font);
    int offset = d->underline_offset == (-2147483647-1) ? 1 + (size >= 20) : d->underline_offset;
    if (d->deco & DECO_UNDERLINE)
        deco_line(s, x, y + asc + offset, d->w, thick, col, d->deco_style);
    if (d->deco & DECO_STRIKE)
        deco_line(s, x, y + asc - size * 3 / 10, d->w, thick, col, d->deco_style);
    if (d->deco & DECO_OVERLINE)
        deco_line(s, x, y + (wfont_height(d->font) - size) / 2, d->w, thick, col, d->deco_style);
}

/* The ::selection style that applies to text in n: its own or the nearest ancestor's. */
static const struct style *selection_style(const struct node *n)
{
    for (; n; n = n->parent)
        if (n->type == N_ELEMENT && n->pseudo_st[PE_SELECTION - 1])
            return n->pseudo_st[PE_SELECTION - 1];
    return NULL;
}

void paint_document(const struct layout *lo, struct surface *s, int ox, int oy, int sx, int sy,
                    const struct node *focus, int caret_on)
{
    struct rect view = s->clip;
    int vx = view.x - ox + sx, vy = view.y - oy + sy;   /* the clip in page coordinates */
    gfx_fill(s, view.x, view.y, view.w, view.h, lo->canvas & 0xFFFFFF);
    int last_sb = 0, sbdx = 0, sbdy = 0;
    struct rect sbclip = view;
    for (int i = 0; i < lo->nitems; i++) {
        const struct ditem *d = &lo->items[i];
        int ddx = 0, ddy = 0;
        if (d->dyn)
            layout_dyn_offset(lo, d->dyn, sx, sy, &ddx, &ddy);
        if (d->sb != last_sb) {                 /* clipped (and moved) by the scrolling boxes round it */
            last_sb = d->sb;
            sbdx = sbdy = 0;
            if (d->sb) {
                struct rect pc;
                layout_sb_view(lo, d->sb, sx, sy, &sbdx, &sbdy, &pc);
                sbclip = rect_intersect(view, mkrect(pc.x - sx + ox, pc.y - sy + oy, pc.w, pc.h));
            } else {
                sbclip = view;
            }
        }
        int idx = d->sb ? sbdx : 0, idy = d->sb ? sbdy : 0;
        if (d->y + ddy + idy >= vy + view.h || d->y + ddy + idy + d->h + 4 <= vy ||
            d->x + ddx + idx >= vx + view.w || d->x + ddx + idx + d->w <= vx)
            continue;
        if (d->sb && rect_empty(sbclip))
            continue;
        int x = d->x + ddx + idx - sx + ox, y = d->y + ddy + idy - sy + oy;
        if (d->sb)
            s->clip = sbclip;
        switch (d->kind) {
        case DK_FILL:
            if (!ALPHA(d->color) && !d->grad && !d->ring)
                break;
            if (d->grad || d->ring || d->rad[0] || d->rad[1] || d->rad[2] || d->rad[3])
                fill_rich(s, d, x, y);
            else if (ALPHA(d->color))
                fill_blend(s, x, y, d->w, d->h, d->color, d->deco);
            break;
        case DK_TEXT: {
            uint32_t col = ALPHA(d->color) == 255 ? d->color : blend(0xFFFFFF, d->color);
            int shown = d->text[0] != ' ' || d->len != 1;
            if (lo->nhl)
                paint_found(lo, s, d, i, x, y);
            if (shown && d->sh_color)
                draw_shadow(s, d, x, y);
            if (shown)
                draw_text(s, d, x, y, d->text, d->len, col);
            if (lo->sel_on && i >= lo->sel_from && i <= lo->sel_to && d->len && selectable(d)) {
                int a = i == lo->sel_from ? lo->sel_c0 : 0, e = i == lo->sel_to ? lo->sel_c1 : d->len;
                if (e > d->len)
                    e = d->len;
                if (a < e) {
                    const struct style *ss = selection_style(d->node);
                    uint32_t sbg = ss && ALPHA(ss->bg) ? ss->bg & 0xFFFFFF : 0x000080;
                    uint32_t sfg = ss && ss->color != col ? ss->color & 0xFFFFFF : 0xFFFFFF;
                    int xa = item_width(d, a), xb = item_width(d, e);
                    gfx_fill(s, x + xa, y, xb - xa, d->h, sbg);
                    struct ditem part = *d;
                    part.sh_color = 0;
                    draw_text(s, &part, x + xa, y, d->text + a, e - a, sfg);
                    if (i > lo->sel_from) {         /* the space between words on this line */
                        for (int j = i - 1; j >= lo->sel_from; j--) {
                            const struct ditem *p = &lo->items[j];
                            if (p->kind != DK_TEXT || !p->len)
                                continue;
                            int dy = p->y - d->y;
                            if ((dy < 0 ? -dy : dy) <= d->h / 2 && p->x + p->w < d->x)
                                gfx_fill(s, p->x + p->w - sx + ox, y, d->x - p->x - p->w, d->h, sbg);
                            break;
                        }
                    }
                }
            }
            if (d->deco)
                draw_decoration(s, d, x, y, d->deco_color ? d->deco_color & 0xFFFFFF : col & 0xFFFFFF);
            break;
        }
        case DK_IMAGE:
            if (d->rad[0] || d->rad[1] || d->rad[2] || d->rad[3] || d->fit_w)
                draw_image_round(s, d, x, y);
            else
                draw_image_scaled(s, d->img, x, y, d->w, d->h);
            break;
        case DK_BGIMG:
            draw_bgimg(s, d, x, y);
            break;
        case DK_SBOX:
            draw_sbox_bars(s, &lo->sb[d->sbi], x, y);
            break;
        case DK_FRAME:
            gfx_frame(s, x, y, d->w, d->h, 0xC0C0C0);
            break;
        case DK_IFRAME:
            fill_blend(s, x, y, d->w, d->h, OPAQUE(0xFFFFFF), 0);   /* until its page is drawn over it */
            break;
        case DK_DISC:
        case DK_CIRCLE:
            disc(s, x, y, d->w, d->color, d->kind == DK_CIRCLE);
            break;
        case DK_CONTROL:
            draw_control(s, d, x, y, d->node == focus, caret_on);
            break;
        }
        if (d->sb)
            s->clip = view;
    }
}

/* ---- scrolling boxes: what the browser asks of them ---- */

static int sbox_depth(const struct layout *lo, int k)
{
    int d = 0;
    for (int p = lo->sb[k].parent; p && d < 64; p = lo->sb[p - 1].parent)
        d++;
    return d;
}

/* Is the page point inside the box, where what clips it shows it? r gets the box's padding box on the page. */
static int sbox_contains(const struct layout *lo, int k, int px, int py, int psx, int psy, struct rect *r)
{
    struct rect clip;
    *r = sbox_rect(lo, k, psx, psy, &clip);
    return rect_has(clip, px, py) && rect_has(*r, px, py);
}

/* Scroll a box to (sx, sy), clamped; the node keeps it for the next layout. 1 if it moved. */
static int sbox_set(struct layout *lo, int k, int sx, int sy)
{
    struct sbox *b = &lo->sb[k];
    sx = sx < 0 ? 0 : sx > b->max_x ? b->max_x : sx;
    sy = sy < 0 ? 0 : sy > b->max_y ? b->max_y : sy;
    int moved = sx != b->sx || sy != b->sy;
    b->sx = b->node->scroll_x = sx;
    b->sy = b->node->scroll_y = sy;
    return moved;
}

int layout_sbar_hit(const struct layout *lo, int px, int py, int psx, int psy, struct sbhit *out)
{
    for (int k = lo->nsb - 1; k >= 0; k--) {
        const struct sbox *b = &lo->sb[k];
        struct rect r;
        if (!b->active || (!b->bar_x && !b->bar_y) || !sbox_contains(lo, k, px, py, psx, psy, &r))
            continue;
        for (int axis = 0; axis < 2; axis++) {
            if (!(axis ? b->bar_y : b->bar_x))
                continue;
            int page, total, pos;
            struct rect bar = sbar_geometry(b, axis, r.x, r.y, &page, &total, &pos);
            if (!rect_has(bar, px, py))
                continue;
            int len = axis ? bar.h : bar.w, along = axis ? py - bar.y : px - bar.x;
            int ts, tl;
            sbar_thumb_span(len, page, total, pos, axis ? b->max_y : b->max_x, &ts, &tl);
            out->box = k, out->axis = axis;
            out->thumb = (axis ? bar.y : bar.x) + ts;
            out->part = along < SB_BAR ? SBP_ARROW1 : along >= len - SB_BAR ? SBP_ARROW2 : !tl ? SBP_NONE
                      : along < ts ? SBP_PAGE1 : along >= ts + tl ? SBP_PAGE2 : SBP_THUMB;
            return 1;
        }
        if (b->bar_x && b->bar_y && px >= r.x + r.w - SB_BAR && py >= r.y + r.h - SB_BAR) {
            out->box = k, out->axis = 0, out->part = SBP_NONE, out->thumb = 0;      /* the corner */
            return 1;
        }
    }
    return 0;
}

int layout_sbar_press(struct layout *lo, const struct sbhit *h)
{
    const struct sbox *b = &lo->sb[h->box];
    int page = h->axis ? b->h - (b->bar_x ? SB_BAR : 0) : b->w - (b->bar_y ? SB_BAR : 0);
    int d = h->part == SBP_ARROW1 ? -SB_STEP : h->part == SBP_ARROW2 ? SB_STEP
          : h->part == SBP_PAGE1 ? -(page - SB_STEP / 2) : h->part == SBP_PAGE2 ? page - SB_STEP / 2 : 0;
    if (!d)
        return 0;
    return h->axis ? sbox_set(lo, h->box, b->sx, b->sy + d) : sbox_set(lo, h->box, b->sx + d, b->sy);
}

void layout_sbar_drag(struct layout *lo, const struct sbhit *h, int mouse, int grab, int psx, int psy)
{
    const struct sbox *b = &lo->sb[h->box];
    struct rect clip;
    struct rect r = sbox_rect(lo, h->box, psx, psy, &clip);
    int page, total, pos;
    struct rect bar = sbar_geometry(b, h->axis, r.x, r.y, &page, &total, &pos);
    int len = h->axis ? bar.h : bar.w, max = h->axis ? b->max_y : b->max_x;
    int ts, tl;
    sbar_thumb_span(len, page, total, pos, max, &ts, &tl);
    int span = len - 2 * SB_BAR - tl;
    if (span <= 0 || max <= 0)
        return;
    int start = (h->axis ? bar.y : bar.x) + SB_BAR;
    int np = muldiv(mouse - grab - start, max, span);
    if (h->axis)
        sbox_set(lo, h->box, b->sx, np);
    else
        sbox_set(lo, h->box, np, b->sy);
}

int layout_wheel(struct layout *lo, int px, int py, int psx, int psy, int dx, int dy)
{
    int best = -1, best_depth = -1;
    for (int k = 0; k < lo->nsb; k++) {
        struct rect r;
        if (!lo->sb[k].active || !(lo->sb[k].max_x || lo->sb[k].max_y) ||
            !sbox_contains(lo, k, px, py, psx, psy, &r))
            continue;
        int depth = sbox_depth(lo, k);
        if (depth > best_depth)
            best = k, best_depth = depth;
    }
    for (int k = best, n = 0; k >= 0 && n < 64; k = lo->sb[k].parent - 1, n++) {
        const struct sbox *b = &lo->sb[k];
        int user_x = b->ov_x == OV_SCROLL || b->ov_x == OV_AUTO, user_y = b->ov_y == OV_SCROLL || b->ov_y == OV_AUTO;
        int can_x = user_x && ((dx < 0 && b->sx > 0) || (dx > 0 && b->sx < b->max_x));
        int can_y = user_y && ((dy < 0 && b->sy > 0) || (dy > 0 && b->sy < b->max_y));
        if (can_x || can_y) {
            sbox_set(lo, k, b->sx + (can_x ? dx : 0), b->sy + (can_y ? dy : 0));
            return 1;
        }
        if (!b->parent)
            break;
    }
    return 0;
}

static int sbox_of_node(const struct layout *lo, const struct node *n)
{
    for (int k = 0; k < lo->nsb; k++)
        if (lo->sb[k].node == n)
            return k;
    return -1;
}

int layout_node_scroll(const struct layout *lo, const struct node *n, int *sx, int *sy, int *cw, int *ch,
                       int *vw, int *vh)
{
    int k = sbox_of_node(lo, n);
    if (k < 0)
        return 0;
    const struct sbox *b = &lo->sb[k];
    *sx = b->sx, *sy = b->sy, *cw = b->cw, *ch = b->ch, *vw = b->w, *vh = b->h;
    return 1;
}

int layout_set_node_scroll(struct layout *lo, struct node *n, int sx, int sy)
{
    int k = sbox_of_node(lo, n);
    if (k >= 0)
        sbox_set(lo, k, sx, sy);
    /* Kept as asked, even by a box that is not laid out yet (or is stale): the next layout clamps it. */
    n->scroll_x = sx < 0 ? 0 : sx;
    n->scroll_y = sy < 0 ? 0 : sy;
    return k >= 0;
}

void layout_reveal(struct layout *lo, const struct node *n, int x, int y, int w, int h)
{
    int accx = 0, accy = 0;
    for (const struct node *a = n->parent; a; a = a->parent) {
        int k = sbox_of_node(lo, a);
        if (k < 0)
            continue;
        const struct sbox *b = &lo->sb[k];
        int rx = x - accx - b->x, ry = y - accy - b->y;
        int vw = b->w - (b->bar_y ? SB_BAR : 0), vh = b->h - (b->bar_x ? SB_BAR : 0);
        int nx = b->sx, ny = b->sy;
        if (ry + h > ny + vh)
            ny = ry + h - vh;
        if (ry < ny)
            ny = ry;
        if (rx + w > nx + vw)
            nx = rx + w - vw;
        if (rx < nx)
            nx = rx;
        sbox_set(lo, k, nx, ny);
        accx += b->sx, accy += b->sy;
    }
}
