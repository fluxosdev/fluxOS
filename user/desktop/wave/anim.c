/*
 * CSS transitions and animations. css.c's compute() hands each element's
 * style here once its declarations apply (anim_style): a property whose
 * value changed since the last cascade starts a transition if the element
 * has one for it, @keyframes animations are run by name, and the values
 * they have at anim_now are put into the style before anything else is
 * worked out from it, so children inherit them. While anything moves the
 * browser restyles those elements every frame (css_restyle_animated).
 *
 * What is remembered of an element (its last base values, its running
 * transitions and animations) is an anim_state, kept here per document and
 * dropped when a whole cascade no longer styles the element.
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

#define FRAME_MS 16

unsigned anim_now;

/* ---- timing functions ---- */

static float bezier_at(float a, float b, float t)       /* one coordinate of a cubic from (0,0) to (1,1) */
{
    float u = 1 - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

static float timing(const struct css_timing *e, float x, int before)
{
    if (e->steps_kind) {
        int n = e->steps, k = (int)floorf(x * (float)n);
        int jumps = e->jump == JUMP_NONE ? n - 1 : e->jump == JUMP_BOTH ? n + 1 : n;
        if (e->jump == JUMP_START || e->jump == JUMP_BOTH)
            k++;
        if (before && x * n == floorf(x * n) && k > 0)
            k--;
        if (x >= 0 && k < 0)
            k = 0;
        if (x <= 1 && k > jumps)
            k = jumps;
        return jumps > 0 ? (float)k / (float)jumps : 1;
    }
    if (x <= 0 || x >= 1)
        return x <= 0 ? 0 : 1;
    if (e->x1 == e->y1 && e->x2 == e->y2)
        return x;                       /* linear */
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 8; i++) {       /* Newton's method, falling back on bisection */
        float fx = bezier_at(e->x1, e->x2, t) - x;
        if (fabsf(fx) < 1e-4f)
            return bezier_at(e->y1, e->y2, t);
        float u = 1 - t;
        float d = 3 * u * u * e->x1 + 6 * u * t * (e->x2 - e->x1) + 3 * t * t * (1 - e->x2);
        if (fabsf(d) < 1e-6f)
            break;
        t -= fx / d;
        if (t < 0 || t > 1)
            break;
    }
    for (int i = 0; i < 30; i++) {
        t = (lo + hi) / 2;
        if (bezier_at(e->x1, e->x2, t) < x)
            lo = t;
        else
            hi = t;
    }
    return bezier_at(e->y1, e->y2, (lo + hi) / 2);
}

/* ---- property values ---- */

struct apval {
    uint32_t c;                 /* colors (also a shadow's) */
    float f;                    /* opacity, flex factors, line height */
    struct len l, l2;           /* lengths (transform-origin: both) */
    int i[4];                   /* pixels and integers; shadows: x, y, blur, spread */
    uint8_t k;                  /* line height: its kind */
    uint8_t has_xf;
    struct css_xform xf;
};

static const struct { const char *name; int ap; } prop_table[] = {
    { "background-color", AP_BACKGROUND_COLOR }, { "border-bottom-color", AP_BORDER_BOTTOM_COLOR },
    { "border-bottom-left-radius", AP_BORDER_BL_RADIUS }, { "border-bottom-right-radius", AP_BORDER_BR_RADIUS },
    { "border-bottom-width", AP_BORDER_BOTTOM_WIDTH }, { "border-left-color", AP_BORDER_LEFT_COLOR },
    { "border-left-width", AP_BORDER_LEFT_WIDTH }, { "border-right-color", AP_BORDER_RIGHT_COLOR },
    { "border-right-width", AP_BORDER_RIGHT_WIDTH }, { "border-top-color", AP_BORDER_TOP_COLOR },
    { "border-top-left-radius", AP_BORDER_TL_RADIUS }, { "border-top-right-radius", AP_BORDER_TR_RADIUS },
    { "border-top-width", AP_BORDER_TOP_WIDTH }, { "bottom", AP_BOTTOM }, { "box-shadow", AP_BOX_SHADOW },
    { "color", AP_COLOR }, { "column-gap", AP_COLUMN_GAP }, { "flex-basis", AP_FLEX_BASIS },
    { "flex-grow", AP_FLEX_GROW }, { "flex-shrink", AP_FLEX_SHRINK }, { "font-size", AP_FONT_SIZE },
    { "height", AP_HEIGHT }, { "left", AP_LEFT }, { "letter-spacing", AP_LETTER_SPACING },
    { "line-height", AP_LINE_HEIGHT }, { "margin-bottom", AP_MARGIN_BOTTOM }, { "margin-left", AP_MARGIN_LEFT },
    { "margin-right", AP_MARGIN_RIGHT }, { "margin-top", AP_MARGIN_TOP }, { "max-height", AP_MAX_HEIGHT },
    { "max-width", AP_MAX_WIDTH }, { "min-height", AP_MIN_HEIGHT }, { "min-width", AP_MIN_WIDTH },
    { "opacity", AP_OPACITY }, { "outline-color", AP_OUTLINE_COLOR }, { "outline-offset", AP_OUTLINE_OFFSET },
    { "outline-width", AP_OUTLINE_WIDTH }, { "padding-bottom", AP_PADDING_BOTTOM },
    { "padding-left", AP_PADDING_LEFT }, { "padding-right", AP_PADDING_RIGHT }, { "padding-top", AP_PADDING_TOP },
    { "right", AP_RIGHT }, { "row-gap", AP_ROW_GAP }, { "text-indent", AP_TEXT_INDENT },
    { "text-shadow", AP_TEXT_SHADOW }, { "top", AP_TOP }, { "transform", AP_TRANSFORM },
    { "transform-origin", AP_TRANSFORM_ORIGIN }, { "visibility", AP_VISIBILITY }, { "width", AP_WIDTH },
    { "word-spacing", AP_WORD_SPACING }, { "z-index", AP_Z_INDEX },
};

uint64_t anim_prop_bits(const char *name, int len)
{
    char n[48];
    if (len <= 0 || len >= (int)sizeof(n))
        return 0;
    for (int i = 0; i < len; i++)
        n[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    n[len] = 0;
    const char *p = !strncmp(n, "-webkit-", 8) ? n + 8 : n;
    if (!strcmp(p, "all"))
        return AP_EVERY;
    for (unsigned i = 0; i < sizeof(prop_table) / sizeof(prop_table[0]); i++)
        if (!strcmp(p, prop_table[i].name))
            return AP_BIT(prop_table[i].ap);
    static const struct { const char *name; int first, count; } groups[] = {
        { "background", AP_BACKGROUND_COLOR, 1 }, { "border-color", AP_BORDER_TOP_COLOR, 4 },
        { "border-width", AP_BORDER_TOP_WIDTH, 4 }, { "border-radius", AP_BORDER_TL_RADIUS, 4 },
        { "margin", AP_MARGIN_TOP, 4 }, { "padding", AP_PADDING_TOP, 4 }, { "inset", AP_TOP, 4 },
        { "outline", AP_OUTLINE_COLOR, 1 }, { "font", AP_FONT_SIZE, 1 }, { "gap", AP_ROW_GAP, 2 },
        { "flex", AP_FLEX_GROW, 3 },
    };
    for (unsigned i = 0; i < sizeof(groups) / sizeof(groups[0]); i++)
        if (!strcmp(p, groups[i].name)) {
            uint64_t b = 0;
            for (int k = 0; k < groups[i].count; k++)
                b |= AP_BIT(groups[i].first + k);
            if (!strcmp(p, "outline"))
                b |= AP_BIT(AP_OUTLINE_WIDTH);
            return b;
        }
    if (!strcmp(p, "border"))
        return anim_prop_bits("border-color", 12) | anim_prop_bits("border-width", 12);
    if (!strncmp(p, "border-", 7)) {                 /* border-top and the other sides */
        static const char *const sides[] = { "top", "right", "bottom", "left" };
        for (int s = 0; s < 4; s++)
            if (!strcmp(p + 7, sides[s]))
                return AP_BIT(AP_BORDER_TOP_WIDTH + s) | AP_BIT(AP_BORDER_TOP_COLOR + s);
    }
    return 0;
}

static struct len *len_field(struct style *st, int ap)
{
    switch (ap) {
    case AP_WIDTH: return &st->width;
    case AP_HEIGHT: return &st->height;
    case AP_MIN_WIDTH: return &st->min_width;
    case AP_MAX_WIDTH: return &st->max_width;
    case AP_MIN_HEIGHT: return &st->min_height;
    case AP_MAX_HEIGHT: return &st->max_height;
    case AP_MARGIN_TOP: case AP_MARGIN_RIGHT: case AP_MARGIN_BOTTOM: case AP_MARGIN_LEFT:
        return &st->margin[ap - AP_MARGIN_TOP];
    case AP_PADDING_TOP: case AP_PADDING_RIGHT: case AP_PADDING_BOTTOM: case AP_PADDING_LEFT:
        return &st->padding[ap - AP_PADDING_TOP];
    case AP_TOP: return &st->top;
    case AP_RIGHT: return &st->right;
    case AP_BOTTOM: return &st->bottom;
    case AP_LEFT: return &st->left;
    case AP_BORDER_TL_RADIUS: case AP_BORDER_TR_RADIUS: case AP_BORDER_BR_RADIUS: case AP_BORDER_BL_RADIUS:
        return &st->radius[ap - AP_BORDER_TL_RADIUS];
    case AP_FLEX_BASIS: return &st->flex_basis;
    case AP_ROW_GAP: return &st->row_gap;
    case AP_COLUMN_GAP: return &st->col_gap;
    default: return NULL;
    }
}

static int *int_field(struct style *st, int ap)
{
    switch (ap) {
    case AP_BORDER_TOP_WIDTH: case AP_BORDER_RIGHT_WIDTH: case AP_BORDER_BOTTOM_WIDTH: case AP_BORDER_LEFT_WIDTH:
        return &st->border_w[ap - AP_BORDER_TOP_WIDTH];
    case AP_FONT_SIZE: return &st->font_size;
    case AP_LETTER_SPACING: return &st->letter_spacing;
    case AP_WORD_SPACING: return &st->word_spacing;
    case AP_FLEX_GROW: return &st->flex_grow;
    case AP_FLEX_SHRINK: return &st->flex_shrink;
    case AP_TEXT_INDENT: return &st->text_indent;
    case AP_OUTLINE_WIDTH: return &st->outline_w;
    case AP_OUTLINE_OFFSET: return &st->outline_offset;
    case AP_Z_INDEX: return &st->z_index;
    default: return NULL;
    }
}

static uint32_t *color_field(struct style *st, int ap)
{
    switch (ap) {
    case AP_COLOR: return &st->color;
    case AP_BACKGROUND_COLOR: return &st->bg;
    case AP_BORDER_TOP_COLOR: case AP_BORDER_RIGHT_COLOR: case AP_BORDER_BOTTOM_COLOR: case AP_BORDER_LEFT_COLOR:
        return &st->border_color[ap - AP_BORDER_TOP_COLOR];
    case AP_OUTLINE_COLOR: return &st->outline_color;
    default: return NULL;
    }
}

static void get(const struct style *cst, int ap, struct apval *v)
{
    struct style *st = (struct style *)cst;
    memset(v, 0, offsetof(struct apval, xf));
    uint32_t *c = color_field(st, ap);
    struct len *l = len_field(st, ap);
    int *i = int_field(st, ap);
    if (c)
        v->c = *c == 1 ? st->color : *c;        /* 1 is currentColor until the cascade ends */
    else if (l)
        v->l = *l;
    else if (i)
        v->i[0] = *i;
    switch (ap) {
    case AP_OPACITY: v->f = st->opacity; break;
    case AP_LINE_HEIGHT: v->k = st->lh_kind, v->i[0] = st->lh; break;
    case AP_VISIBILITY: v->i[0] = st->hidden; break;
    case AP_TRANSFORM_ORIGIN: v->l = st->tox, v->l2 = st->toy; break;
    case AP_BOX_SHADOW:
        v->c = st->sh_color, v->i[0] = st->sh_x, v->i[1] = st->sh_y, v->i[2] = st->sh_blur, v->i[3] = st->sh_spread;
        break;
    case AP_TEXT_SHADOW:
        v->c = st->ts_color, v->i[0] = st->ts_x, v->i[1] = st->ts_y, v->i[2] = st->ts_blur;
        break;
    case AP_TRANSFORM:
        v->has_xf = st->xf != NULL;
        if (st->xf)
            v->xf = *st->xf;
        break;
    }
}

static void put(struct style *st, int ap, const struct apval *v, struct arena *a)
{
    uint32_t *c = color_field(st, ap);
    struct len *l = len_field(st, ap);
    int *i = int_field(st, ap);
    if (c)
        *c = v->c;
    else if (l)
        *l = v->l;
    else if (i)
        *i = v->i[0];
    switch (ap) {
    case AP_OPACITY:
        st->opacity = (uint8_t)(v->f < 0 ? 0 : v->f > 255 ? 255 : v->f + .5f);
        st->invisible = (uint8_t)((st->invisible & ~1) | (st->opacity == 0));
        break;
    case AP_LINE_HEIGHT: st->lh_kind = v->k, st->lh = v->i[0]; break;
    case AP_VISIBILITY: st->hidden = (uint8_t)v->i[0]; break;
    case AP_TRANSFORM_ORIGIN: st->tox = v->l, st->toy = v->l2; break;
    case AP_BOX_SHADOW:
        st->sh_color = v->c, st->sh_x = v->i[0], st->sh_y = v->i[1], st->sh_blur = v->i[2], st->sh_spread = v->i[3];
        break;
    case AP_TEXT_SHADOW:
        st->ts_color = v->c, st->ts_x = v->i[0], st->ts_y = v->i[1], st->ts_blur = v->i[2];
        break;
    case AP_TRANSFORM:
        st->xf = NULL;
        if (v->has_xf && v->xf.n) {
            struct css_xform *x = arena_alloc(a, sizeof(*x));
            if (x) {
                *x = v->xf;
                st->xf = x;
            }
        }
        break;
    case AP_Z_INDEX:
        st->z_set = 1;
        break;
    }
}

static int same_len(struct len a, struct len b)
{
    return a.v == b.v && a.pct == b.pct && a.unit == b.unit;
}

static int same(int ap, const struct apval *a, const struct apval *b)
{
    if (ap == AP_TRANSFORM) {
        if (a->has_xf != b->has_xf || (a->has_xf && a->xf.n != b->xf.n))
            return 0;
        for (int k = 0; a->has_xf && k < a->xf.n; k++) {
            const struct tfop *p = &a->xf.op[k], *q = &b->xf.op[k];
            if (p->kind != q->kind || !same_len(p->x, q->x) || !same_len(p->y, q->y) || p->a != q->a ||
                p->b != q->b || p->c != q->c || p->d != q->d || p->e != q->e || p->f != q->f)
                return 0;
        }
        return 1;
    }
    return a->c == b->c && a->f == b->f && same_len(a->l, b->l) && same_len(a->l2, b->l2) &&
           !memcmp(a->i, b->i, sizeof(a->i)) && a->k == b->k;
}

static int lerp_i(int a, int b, float t)
{
    float v = (float)a + (float)(b - a) * t;
    return (int)(v < 0 ? v - .5f : v + .5f);
}

/* Premultiplied, so a fade from transparent does not pass through black. */
static uint32_t lerp_color(uint32_t a, uint32_t b, float t)
{
    float aa = (float)ALPHA(a), ba = (float)ALPHA(b), oa = aa + (ba - aa) * t;
    if (oa <= 0)
        return TRANSPARENT;
    uint32_t out = (uint32_t)(oa + .5f) << 24;
    for (int s = 0; s <= 16; s += 8) {
        float x = (float)((a >> s) & 255) * aa, y = (float)((b >> s) & 255) * ba;
        float ch = (x + (y - x) * t) / oa;
        out |= (uint32_t)(ch < 0 ? 0 : ch > 255 ? 255 : ch + .5f) << s;
    }
    return out;
}

/* Lengths of the same unit blend; pixels and percentages blend into calc(); auto does not. */
static struct len lerp_len(struct len a, struct len b, float t)
{
    if (a.unit == b.unit && (a.unit == U_PX || a.unit == U_PCT || a.unit == U_CALC))
        return (struct len){ lerp_i(a.v, b.v, t), lerp_i(a.pct, b.pct, t), a.unit };
    int mixable = (a.unit == U_PX || a.unit == U_PCT || a.unit == U_CALC) &&
                  (b.unit == U_PX || b.unit == U_PCT || b.unit == U_CALC);
    if (!mixable)
        return t < .5f ? a : b;
    int apx = a.unit == U_PCT ? 0 : a.v, apc = a.unit == U_PCT ? a.v : a.unit == U_CALC ? a.pct : 0;
    int bpx = b.unit == U_PCT ? 0 : b.v, bpc = b.unit == U_PCT ? b.v : b.unit == U_CALC ? b.pct : 0;
    return (struct len){ lerp_i(apx, bpx, t), lerp_i(apc, bpc, t), U_CALC };
}

/* An operation that does nothing, of op's kind (for blending with transform: none). */
static struct tfop identity_op(const struct tfop *op)
{
    struct tfop id = { 0 };
    id.kind = op->kind;
    id.x.unit = id.y.unit = U_PX;
    if (op->kind == TF_SCALE)
        id.a = id.b = id.m[10] = 1;
    if (op->kind == TF_MATRIX)
        id.a = id.d = 1;
    if (op->kind == TF_MATRIX3D)
        id.m[0] = id.m[5] = id.m[10] = id.m[15] = 1;
    if (op->kind == TF_PERSPECTIVE)
        id.z.unit = U_AUTO;
    return id;
}

static int blend_xf(const struct apval *a, const struct apval *b, float t, struct apval *out)
{
    struct css_xform ia, ib;
    const struct css_xform *x = a->has_xf ? &a->xf : NULL, *y = b->has_xf ? &b->xf : NULL;
    if (!x && !y)
        return 1;
    if (!x || !y) {                     /* none against a list: the list's identity */
        const struct css_xform *list = x ? x : y;
        struct css_xform *id = x ? &ib : &ia;
        id->n = list->n;
        for (int k = 0; k < list->n; k++)
            id->op[k] = identity_op(&list->op[k]);
        if (!x)
            x = &ia;
        else
            y = &ib;
    }
    if (x->n != y->n)
        return 0;
    for (int k = 0; k < x->n; k++)
        if (x->op[k].kind != y->op[k].kind)
            return 0;
    out->has_xf = 1;
    out->xf.n = x->n;
    for (int k = 0; k < x->n; k++) {
        const struct tfop *p = &x->op[k], *q = &y->op[k];
        struct tfop *r = &out->xf.op[k];
        r->kind = p->kind;
        r->z = lerp_len(p->z, q->z, t);
        for(int j=0;j<16;j++)r->m[j]=p->m[j]+(q->m[j]-p->m[j])*t;
        if(r->kind==TF_PERSPECTIVE) {
            double a=p->z.unit==U_AUTO?0:1.0/(p->z.v<1?1:p->z.v);
            double b=q->z.unit==U_AUTO?0:1.0/(q->z.v<1?1:q->z.v), inv=a+(b-a)*t;
            r->z=(struct len){.unit=inv>0?U_PX:U_AUTO,.v=inv>0?(int)(1/inv):0};
        }
        r->x = lerp_len(p->x, q->x, t);
        r->y = lerp_len(p->y, q->y, t);
        r->a = p->a + (q->a - p->a) * t, r->b = p->b + (q->b - p->b) * t;
        r->c = p->c + (q->c - p->c) * t, r->d = p->d + (q->d - p->d) * t;
        r->e = p->e + (q->e - p->e) * t, r->f = p->f + (q->f - p->f) * t;
    }
    return 1;
}

static void blend(int ap, const struct apval *a, const struct apval *b, float t, struct apval *out)
{
    memset(out, 0, offsetof(struct apval, xf));
    switch (ap) {
    case AP_OPACITY:
        out->f = a->f + (b->f - a->f) * t;
        return;
    case AP_VISIBILITY:                 /* visible while either end is */
        out->i[0] = t <= 0 ? a->i[0] : t >= 1 ? b->i[0] : a->i[0] && b->i[0];
        return;
    case AP_LINE_HEIGHT:
        if (a->k != b->k) {
            *out = t < .5f ? *a : *b;
            return;
        }
        out->k = a->k;
        out->i[0] = lerp_i(a->i[0], b->i[0], t);
        return;
    case AP_TRANSFORM_ORIGIN:
        out->l = lerp_len(a->l, b->l, t);
        out->l2 = lerp_len(a->l2, b->l2, t);
        return;
    case AP_BOX_SHADOW: case AP_TEXT_SHADOW: {
        struct apval x = *a, y = *b;    /* none is a transparent shadow of no size */
        if (!x.c)
            memset(x.i, 0, sizeof(x.i)), x.c = y.c & 0xFFFFFF;
        if (!y.c)
            memset(y.i, 0, sizeof(y.i)), y.c = x.c & 0xFFFFFF;
        out->c = lerp_color(x.c, y.c, t);
        for (int k = 0; k < 4; k++)
            out->i[k] = lerp_i(x.i[k], y.i[k], t);
        if (!ALPHA(out->c))
            out->c = 0;
        return;
    }
    case AP_TRANSFORM:
        if (!blend_xf(a, b, t, out))
            *out = t < .5f ? *a : *b;
        return;
    }
    struct style probe;
    if (color_field(&probe, ap))
        out->c = lerp_color(a->c, b->c, t);
    else if (len_field(&probe, ap))
        out->l = lerp_len(a->l, b->l, t);
    else
        out->i[0] = lerp_i(a->i[0], b->i[0], t);
}

/* ---- what is remembered of an element ---- */

struct transition {
    int ap;
    unsigned start;             /* when it starts moving (after its delay) */
    int duration;
    struct css_timing ease;
    struct apval from, to;
    struct apval reverse_from;  /* where the one it replaced started, for shortening a reversal */
    float scale;                /* reversing: the fraction of a full transition this one is */
    uint8_t started;            /* transitionstart has been sent */
    const char *name;           /* the transition-property name it came from */
};

struct animation {
    char name[64];
    unsigned start;             /* when it began (its delay counts from here) */
    unsigned paused_at;         /* while paused: when it stopped; else 0 */
    int iteration;              /* the last one animationiteration was sent for */
    uint8_t started, ended;
    struct css_animation spec;
};

struct anim_state {
    struct node *node;
    const struct node *doc;     /* the root of its document */
    unsigned seen;              /* the whole cascade that last styled it */
    int has_prev;
    struct style prev;          /* its base values in the last cascade (no transitions or animations) */
    struct css_xform prev_xf;
    struct transition *t;
    int nt, tcap;
    struct animation *a;
    int na;
};

static struct anim_state **states;
static int nstates, scap;
static unsigned cascade_gen;

static struct anim_event events[256];
static const struct node *event_doc[256];
static int nevents;

static void event(struct anim_state *s, const char *type, const char *name, float elapsed)
{
    if (nevents == (int)(sizeof(events) / sizeof(events[0])))
        return;
    struct anim_event *e = &events[nevents];
    e->node = s->node;
    snprintf(e->type, sizeof(e->type), "%s", type);
    snprintf(e->name, sizeof(e->name), "%s", name ? name : "");
    e->elapsed = elapsed;
    event_doc[nevents++] = s->doc;
}

int anim_take_event(const struct node *root, struct anim_event *out)
{
    for (int i = 0; i < nevents; i++) {
        if (event_doc[i] != root)
            continue;
        *out = events[i];
        memmove(&events[i], &events[i + 1], sizeof(events[0]) * (size_t)(nevents - i - 1));
        memmove(&event_doc[i], &event_doc[i + 1], sizeof(event_doc[0]) * (size_t)(nevents - i - 1));
        nevents--;
        return 1;
    }
    return 0;
}

static const struct node *document_of(const struct node *n)
{
    while (n->parent || n->shadow_host)
        n = n->parent ? n->parent : n->shadow_host;
    return n;
}

static struct anim_state *state_of(struct node *n)
{
    if (n->anim)
        return n->anim;
    if (nstates == scap) {
        int nc = scap ? scap * 2 : 64;
        struct anim_state **ns = realloc(states, sizeof(*ns) * (size_t)nc);
        if (!ns)
            return NULL;
        states = ns;
        scap = nc;
    }
    struct anim_state *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->node = n;
    s->doc = document_of(n);
    states[nstates++] = s;
    n->anim = s;
    return s;
}

static void free_state(int i, int touch_node)
{
    struct anim_state *s = states[i];
    if (touch_node && s->node->anim == s)
        s->node->anim = NULL;
    free(s->t);
    free(s->a);
    free(s);
    states[i] = states[--nstates];
}

void anim_cascade_begin(struct node *root)
{
    (void)root;
    if (!++cascade_gen)
        cascade_gen = 1;
}

void anim_cascade_end(struct node *root)
{
    for (int i = nstates - 1; i >= 0; i--) {
        struct anim_state *s = states[i];
        if (s->doc != root || s->seen == cascade_gen)
            continue;
        for (int k = 0; k < s->nt; k++)
            event(s, "transitioncancel", s->t[k].name, 0);
        for (int k = 0; k < s->na; k++)
            if (!s->a[k].ended)
                event(s, "animationcancel", s->a[k].name, 0);
        free_state(i, 1);
    }
}

void anim_forget(const struct node *root)
{
    for (int i = nstates - 1; i >= 0; i--)
        if (states[i]->doc == root)
            free_state(i, 0);
    for (int i = nevents - 1; i >= 0; i--)
        if (event_doc[i] == root) {
            memmove(&events[i], &events[i + 1], sizeof(events[0]) * (size_t)(nevents - i - 1));
            memmove(&event_doc[i], &event_doc[i + 1], sizeof(event_doc[0]) * (size_t)(nevents - i - 1));
            nevents--;
        }
}

/* ---- animations ---- */

/*
 * Where an animation is at now: its progress through the current iteration
 * (directed), and the phase. Returns 0 if it has no effect now.
 */
static int animation_progress(struct animation *an, unsigned now, float *p, int *iter, int *phase)
{
    const struct css_animation *sp = &an->spec;
    if (an->paused_at)
        now = an->paused_at;
    float local = (float)(int)(now - an->start) - (float)sp->delay;
    float dur = (float)sp->duration, iters = sp->iterations < 0 ? INFINITY : sp->iterations;
    float active = dur * iters;
    float overall;
    if (local < 0) {
        *phase = -1;
        if (sp->fill != AF_BACKWARDS && sp->fill != AF_BOTH)
            return 0;
        overall = 0;
    } else if (local >= active || dur <= 0) {
        *phase = 1;
        if (sp->fill != AF_FORWARDS && sp->fill != AF_BOTH)
            return 0;
        overall = iters;
    } else {
        *phase = 0;
        overall = local / dur;
    }
    if (isinf(overall))
        overall = 0;
    int it = (int)floorf(overall);
    float frac = overall - (float)it;
    if (*phase == 1 && frac == 0 && it > 0) {   /* the very end of the last iteration */
        it--;
        frac = 1;
    }
    int reversed = sp->direction == AD_REVERSE || (sp->direction == AD_ALTERNATE && (it & 1)) ||
                   (sp->direction == AD_ALTERNATE_REVERSE && !(it & 1));
    *p = reversed ? 1 - frac : frac;
    *iter = it;
    return 1;
}

/* The value of ap at progress p through keyframes k, from base (implicit 0% and 100%). */
static void keyframe_value(const struct keyframes *k, struct style *frames, const struct style *base,
                           const struct css_timing *ease, int ap, float p, struct apval *out)
{
    int a = -1, b = -1;
    for (int i = 0; i < k->n; i++) {
        if (!(k->f[i].props & AP_BIT(ap)))
            continue;
        if (k->f[i].offset <= p)
            a = i;
        if (k->f[i].offset >= p && b < 0)
            b = i;
    }
    float oa = a >= 0 ? k->f[a].offset : 0, ob = b >= 0 ? k->f[b].offset : 1;
    struct apval va, vb;
    get(a >= 0 ? &frames[a] : base, ap, &va);
    get(b >= 0 ? &frames[b] : base, ap, &vb);
    if (a >= 0 && b >= 0 && a == b) {
        *out = va;
        return;
    }
    const struct css_timing *e = a >= 0 && k->f[a].ease ? k->f[a].ease : ease;
    float t = ob > oa ? (p - oa) / (ob - oa) : 1;
    blend(ap, &va, &vb, timing(e, t, 0), out);
}

static void run_animations(struct anim_state *s, struct style *st, const struct anim_env *env)
{
    struct style *frames = NULL;
    int fcap = 0;
    struct style base = *st;
    for (int i = 0; i < s->na; i++) {
        struct animation *an = &s->a[i];
        const struct keyframes *k = env->find(env->ctx, an->name);
        if (!k)
            continue;                   /* no such @keyframes: no animation, and no events */
        float p = 0;
        int iter = 0, phase = 0;
        int effect = animation_progress(an, anim_now, &p, &iter, &phase);
        float elapsed = (float)an->spec.duration / 1000 * (float)iter;
        /* Events: start, each new iteration, end. */
        if (!an->paused_at) {
            if (!an->started && phase >= 0) {
                an->started = 1;
                event(s, "animationstart", an->name, 0);
            }
            if (phase == 0 && iter > an->iteration) {
                an->iteration = iter;
                event(s, "animationiteration", an->name, elapsed);
            }
            if (phase == 1 && !an->ended) {
                an->ended = 1;
                if (!an->started)
                    event(s, "animationstart", an->name, 0);
                an->started = 1;
                float total = an->spec.iterations < 0 ? 0 : (float)an->spec.duration * an->spec.iterations / 1000;
                event(s, "animationend", an->name, total);
            }
        }
        if (!effect || !k->n)
            continue;
        if (fcap < k->n) {
            free(frames);
            frames = malloc(sizeof(*frames) * (size_t)k->n);
            fcap = frames ? k->n : 0;
            if (!frames)
                return;
        }
        uint64_t props = 0;
        for (int f = 0; f < k->n; f++) {
            frames[f] = base;
            env->apply(env->ctx, &k->f[f], &frames[f]);
            props |= k->f[f].props;
        }
        for (int ap = 0; ap < AP_COUNT; ap++) {
            if (!(props & AP_BIT(ap)))
                continue;
            struct apval v;
            keyframe_value(k, frames, &base, &an->spec.ease, ap, p, &v);
            put(st, ap, &v, env->a);
        }
    }
    free(frames);
}

/* The animations the style names: keep the ones already running, start new ones, drop the rest. */
static void sync_animations(struct anim_state *s, const struct style *st)
{
    struct animation *list = st->nanims ? calloc((size_t)st->nanims, sizeof(*list)) : NULL;
    int n = 0;
    for (int i = 0; i < st->nanims && (list || !st->nanims); i++) {
        const struct css_animation *sp = &st->anims[i];
        struct animation *old = NULL;
        for (int k = 0; k < s->na && !old; k++)
            if (!strcmp(s->a[k].name, sp->name) && s->a[k].name[0])
                old = &s->a[k];
        struct animation *an = &list[n++];
        if (old) {
            *an = *old;
            old->name[0] = 0;           /* taken */
        } else {
            snprintf(an->name, sizeof(an->name), "%s", sp->name);
            an->start = anim_now;
            an->iteration = 0;
        }
        an->spec = *sp;
        an->spec.name = NULL;
        if (sp->paused && !an->paused_at)
            an->paused_at = anim_now ? anim_now : 1;
        else if (!sp->paused && an->paused_at) {
            an->start += anim_now - an->paused_at;
            an->paused_at = 0;
        }
    }
    for (int k = 0; k < s->na; k++)
        if (s->a[k].name[0] && !s->a[k].ended)
            event(s, "animationcancel", s->a[k].name, 0);
    free(s->a);
    s->a = list;
    s->na = n;
}

/* ---- transitions ---- */

static const struct css_transition *transition_for(const struct style *st, int ap)
{
    const struct css_transition *found = NULL;
    for (int i = 0; i < st->ntrans; i++)
        if (st->trans[i].props & AP_BIT(ap))
            found = &st->trans[i];      /* the last one that names it */
    return found;
}

static float transition_at(const struct transition *t, unsigned now)
{
    if ((int)(now - t->start) <= 0)
        return 0;
    float x = (float)(int)(now - t->start) / (float)(t->duration > 0 ? t->duration : 1);
    return x >= 1 ? 1 : timing(&t->ease, x, 0);
}

static void transition_value(const struct transition *t, unsigned now, struct apval *out)
{
    blend(t->ap, &t->from, &t->to, transition_at(t, now), out);
}

static void remove_transition(struct anim_state *s, int k)
{
    s->t[k] = s->t[--s->nt];
}

static void sync_transitions(struct anim_state *s, const struct style *st)
{
    /* The ones whose property no longer has a transition stop where they are. */
    for (int k = s->nt - 1; k >= 0; k--)
        if (!transition_for(st, s->t[k].ap)) {
            event(s, "transitioncancel", s->t[k].name, 0);
            remove_transition(s, k);
        }
    if (!s->has_prev)
        return;
    uint64_t all = 0;
    for (int i = 0; i < st->ntrans; i++)
        all |= st->trans[i].props;
    for (int ap = 0; ap < AP_COUNT; ap++) {
        if (!(all & AP_BIT(ap)))
            continue;
        struct apval before, after;
        get(&s->prev, ap, &before);
        get(st, ap, &after);
        if (same(ap, &before, &after))
            continue;
        const struct css_transition *spec = transition_for(st, ap);
        int k;
        for (k = 0; k < s->nt && s->t[k].ap != ap; k++)
            ;
        struct transition *old = k < s->nt ? &s->t[k] : NULL;
        if (old && same(ap, &old->to, &after))
            continue;                   /* already on its way there */
        struct transition t = { 0 };
        t.ap = ap;
        t.duration = spec->duration;
        t.ease = spec->ease;
        t.name = spec->name;
        t.scale = 1;
        if (old)
            transition_value(old, anim_now, &t.from);
        else
            t.from = before;
        t.to = after;
        t.reverse_from = t.from;
        if (old && same(ap, &old->reverse_from, &after)) {
            /* Going back where the last one started: only as long as it took to come this far. */
            float done = transition_at(old, anim_now) * old->scale;
            t.scale = done > 0 && done < 1 ? done : 1;
            t.duration = (int)((float)spec->duration * t.scale);
            t.reverse_from = old->to;
        }
        int delay = spec->delay < 0 ? (int)((float)spec->delay * t.scale) : spec->delay;
        t.start = anim_now + (unsigned)delay;
        if (old)
            event(s, "transitioncancel", old->name, 0);
        if (t.duration <= 0) {
            if (old)
                remove_transition(s, k);
            continue;
        }
        if (!old) {
            if (s->nt == s->tcap) {
                int nc = s->tcap ? s->tcap * 2 : 4;
                struct transition *nt = realloc(s->t, sizeof(*nt) * (size_t)nc);
                if (!nt)
                    continue;
                s->t = nt;
                s->tcap = nc;
            }
            k = s->nt++;
        }
        s->t[k] = t;
        event(s, "transitionrun", t.name, 0);
    }
}

static void run_transitions(struct anim_state *s, struct style *st, const struct anim_env *env)
{
    for (int k = s->nt - 1; k >= 0; k--) {
        struct transition *t = &s->t[k];
        if (!t->started && (int)(anim_now - t->start) >= 0) {
            t->started = 1;
            event(s, "transitionstart", t->name, 0);
        }
        if ((int)(anim_now - t->start) >= t->duration) {
            event(s, "transitionend", t->name, (float)t->duration / 1000);
            remove_transition(s, k);    /* done: the style's own value shows */
            continue;
        }
        struct apval v;
        transition_value(t, anim_now, &v);
        put(st, t->ap, &v, env->a);
    }
}

/* ---- the hook ---- */

static int state_moving(const struct anim_state *s);

void anim_style(struct node *n, struct style *st, const struct anim_env *env)
{
    if (!st->ntrans && !st->nanims && !n->anim)
        return;
    if (!st->ntrans && !st->nanims && !state_moving(n->anim)) {
        for (int i = 0; i < nstates; i++)       /* nothing left to do or remember */
            if (states[i] == n->anim) {
                free_state(i, 1);
                break;
            }
        return;
    }
    struct anim_state *s = state_of(n);
    if (!s)
        return;
    s->seen = cascade_gen;
    sync_transitions(s, st);
    sync_animations(s, st);
    /* Remember the base values for the next change, before anything moves them. */
    if (st->ntrans) {
        s->prev = *st;
        if (st->xf) {
            s->prev_xf = *st->xf;
            s->prev.xf = &s->prev_xf;
        }
        s->has_prev = 1;
    } else {
        s->has_prev = 0;
    }
    run_animations(s, st, env);
    run_transitions(s, st, env);
}

static int state_moving(const struct anim_state *s)
{
    if (s->nt)
        return 1;
    for (int k = 0; k < s->na; k++)
        if (!s->a[k].ended && !s->a[k].paused_at)
            return 1;
    return 0;
}

int anim_running(const struct node *n)
{
    return n->anim && state_moving(n->anim);
}

int anim_mark(struct node *root, unsigned gen)
{
    int marked = 0;
    for (int i = 0; i < nstates; i++)
        if (states[i]->doc == root && state_moving(states[i])) {
            states[i]->node->dgen = gen;
            marked++;
        }
    return marked;
}

int anim_timeout(const struct node *root)
{
    int best = -1;
    for (int i = 0; i < nstates; i++) {
        const struct anim_state *s = states[i];
        if (s->doc != root || !state_moving(s))
            continue;
        int t = FRAME_MS;
        int wait = -1;                  /* only delays still to run: wake when the first ends */
        for (int k = 0; k < s->nt; k++) {
            int left = (int)(s->t[k].start - anim_now);
            if (left <= 0) {
                wait = -1;
                break;
            }
            if (wait < 0 || left < wait)
                wait = left;
        }
        if (wait > 0 && !s->na)
            t = wait < FRAME_MS ? FRAME_MS : wait;
        if (best < 0 || t < best)
            best = t;
    }
    return best;
}
