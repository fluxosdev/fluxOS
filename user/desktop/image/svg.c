/*
 * SVG: a renderer for the browser. It draws a parsed <svg> element tree
 * (the HTML parser builds it, so the same code serves an inline <svg> and
 * an .svg file) into an image: rect, circle, ellipse, line, polyline,
 * polygon, path (all commands, arcs included), g, use, text, solid colors,
 * linear and radial gradients, fill and stroke with opacity, dashes, caps
 * and joins, transform, viewBox, and <style> rules made of a tag, a class
 * or an id. Shapes are flattened to polygons and filled by a scanline
 * rasteriser with four sub-scanlines a pixel and exact horizontal
 * coverage. Clip paths, masks, filters and patterns are ignored.
 *
 * This file uses floating point, so it is linked with the FPU objects.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"
#include "simd.h"

#define SS        4             /* sub-scanlines per pixel */
#define MAX_DASH  16
#define MAX_DASH_STEPS 20000            /* dash pieces per sub-path; past that, a solid line */
#define MAX_RULES 128
#define MAX_DEPTH 24
#define MAX_PX    2048
#define PI        3.14159265358979323846

struct pt { double x, y; };
struct mat { double a, b, c, d, e, f; };

static const struct mat identity = { 1, 0, 0, 1, 0, 0 };

static int svg_parse_transform(const char *s, struct mat *out);

static struct mat mat_mul(struct mat m, struct mat n)       /* m applied after n */
{
    struct mat r;
    r.a = m.a * n.a + m.c * n.b;
    r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;
    r.d = m.b * n.c + m.d * n.d;
    r.e = m.a * n.e + m.c * n.f + m.e;
    r.f = m.b * n.e + m.d * n.f + m.f;
    return r;
}

static struct pt mat_pt(const struct mat *m, double x, double y)
{
    struct pt p = { m->a * x + m->c * y + m->e, m->b * x + m->d * y + m->f };
    return p;
}

static int mat_inv(const struct mat *m, struct mat *out)
{
    double det = m->a * m->d - m->b * m->c;
    if (fabs(det) < 1e-12)
        return -1;
    out->a = m->d / det;
    out->b = -m->b / det;
    out->c = -m->c / det;
    out->d = m->a / det;
    out->e = (m->c * m->f - m->d * m->e) / det;
    out->f = (m->b * m->e - m->a * m->f) / det;
    return 0;
}

static double mat_scale(const struct mat *m)
{
    double s = sqrt(fabs(m->a * m->d - m->b * m->c));
    return s > 1e-6 ? s : 1;
}

/* ---- paths: flattened sub-paths in user space ---- */

struct sub { int start, n, closed; };

struct path {
    struct pt *pts;
    int np, pcap;
    struct sub *subs;
    int ns, scap;
};

static void path_free(struct path *p)
{
    free(p->pts);
    free(p->subs);
    memset(p, 0, sizeof(*p));
}

static void path_move(struct path *p, double x, double y)
{
    if (p->ns && p->subs[p->ns - 1].n == 1 && !p->subs[p->ns - 1].closed) {
        p->pts[p->np - 1].x = x;        /* a move after a move replaces it */
        p->pts[p->np - 1].y = y;
        return;
    }
    if (p->ns == p->scap) {
        int cap = p->scap ? p->scap * 2 : 8;
        struct sub *t = realloc(p->subs, sizeof(struct sub) * (size_t)cap);
        if (!t) {
            path_free(p);               /* out of memory: the path is lost, capacities with it */
            return;
        }
        p->subs = t;
        p->scap = cap;
    }
    if (p->np == p->pcap) {
        int cap = p->pcap ? p->pcap * 2 : 64;
        struct pt *t = realloc(p->pts, sizeof(struct pt) * (size_t)cap);
        if (!t) {
            path_free(p);
            return;
        }
        p->pts = t;
        p->pcap = cap;
    }
    p->subs[p->ns].start = p->np;
    p->subs[p->ns].n = 1;
    p->subs[p->ns].closed = 0;
    p->ns++;
    p->pts[p->np].x = x;
    p->pts[p->np].y = y;
    p->np++;
}

static void path_line(struct path *p, double x, double y)
{
    if (!p->ns)
        return;
    if (p->subs[p->ns - 1].closed)
        path_move(p, p->pts[p->subs[p->ns - 1].start].x, p->pts[p->subs[p->ns - 1].start].y);
    if (!p->ns)
        return;                         /* that ran out of memory */
    if (p->np == p->pcap) {
        struct pt *t = realloc(p->pts, sizeof(struct pt) * (size_t)p->pcap * 2);
        if (!t) {
            path_free(p);
            return;
        }
        p->pts = t;
        p->pcap *= 2;
    }
    p->pts[p->np].x = x;
    p->pts[p->np].y = y;
    p->np++;
    p->subs[p->ns - 1].n++;
}

static void path_close(struct path *p)
{
    if (p->ns)
        p->subs[p->ns - 1].closed = 1;
}

static struct pt path_cur(const struct path *p)
{
    struct pt z = { 0, 0 };
    if (!p->ns)
        return z;
    const struct sub *s = &p->subs[p->ns - 1];
    return s->closed ? p->pts[s->start] : p->pts[p->np - 1];
}

static int steps_for(double len, double scale)
{
    int n = (int)(len * scale / 3) + 4;
    return n > 80 ? 80 : n;
}

static void path_cubic(struct path *p, double x1, double y1, double x2, double y2, double x3, double y3,
                       double scale)
{
    struct pt a = path_cur(p);
    double len = fabs(x1 - a.x) + fabs(y1 - a.y) + fabs(x2 - x1) + fabs(y2 - y1) + fabs(x3 - x2) + fabs(y3 - y2);
    int n = steps_for(len, scale);
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        path_line(p, u * u * u * a.x + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                  u * u * u * a.y + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
    }
}

static void path_quad(struct path *p, double x1, double y1, double x2, double y2, double scale)
{
    struct pt a = path_cur(p);
    double len = fabs(x1 - a.x) + fabs(y1 - a.y) + fabs(x2 - x1) + fabs(y2 - y1);
    int n = steps_for(len, scale);
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        path_line(p, u * u * a.x + 2 * u * t * x1 + t * t * x2, u * u * a.y + 2 * u * t * y1 + t * t * y2);
    }
}

static double vang(double ux, double uy, double vx, double vy)
{
    double a = atan2(vy, vx) - atan2(uy, ux);
    return a;
}

static void path_arc(struct path *p, double rx, double ry, double rot, int large, int sweep,
                     double x, double y, double scale)
{
    struct pt a = path_cur(p);
    if (rx < 0)
        rx = -rx;
    if (ry < 0)
        ry = -ry;
    if ((a.x == x && a.y == y) || rx == 0 || ry == 0) {
        path_line(p, x, y);
        return;
    }
    double phi = rot * PI / 180, cp = cos(phi), sp = sin(phi);
    double dx = (a.x - x) / 2, dy = (a.y - y) / 2;
    double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    double lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lam > 1) {
        double s = sqrt(lam);
        rx *= s;
        ry *= s;
    }
    double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double co = den > 0 && num > 0 ? sqrt(num / den) : 0;
    if (large == sweep)
        co = -co;
    double cx1 = co * rx * y1 / ry, cy1 = -co * ry * x1 / rx;
    double cx = cp * cx1 - sp * cy1 + (a.x + x) / 2, cy = sp * cx1 + cp * cy1 + (a.y + y) / 2;
    double th = vang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    double dth = vang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    while (dth > PI * 2)
        dth -= PI * 2;
    while (dth < -PI * 2)
        dth += PI * 2;
    if (!sweep && dth > 0)
        dth -= PI * 2;
    else if (sweep && dth < 0)
        dth += PI * 2;
    int n = steps_for((rx + ry) * fabs(dth), scale);
    for (int i = 1; i <= n; i++) {
        double t = th + dth * i / n;
        double ex = rx * cos(t), ey = ry * sin(t);
        path_line(p, cp * ex - sp * ey + cx, sp * ex + cp * ey + cy);
    }
    path_line(p, x, y);
}

/* ---- parsing helpers ---- */

/* strtod for plain decimals: sign, digits, fraction, exponent. */
static double svg_strtod(const char *s, char **end)
{
    const char *p = s;
    int neg = 0;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';
    double v = 0;
    int digits = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (*p++ - '0');
        digits++;
    }
    if (*p == '.') {
        double scale = 0.1;
        p++;
        while (isdigit((unsigned char)*p)) {
            v += (*p++ - '0') * scale;
            scale /= 10;
            digits++;
        }
    }
    if (!digits) {
        *end = (char *)s;
        return 0;
    }
    if ((*p == 'e' || *p == 'E') && (isdigit((unsigned char)p[1]) ||
                                     ((p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2])))) {
        int eneg = 0, e = 0;
        p++;
        if (*p == '+' || *p == '-')
            eneg = *p++ == '-';
        while (isdigit((unsigned char)*p)) {
            e = e * 10 + (*p++ - '0');
            if (e > 400)
                e = 400;                /* saturated: 1e999 is huge, not 1 */
        }
        double m = 1;
        for (; e > 0; e--)
            m *= 10;
        v = eneg ? v / m : v ? v * m : 0;
    }
    *end = (char *)p;
    /* Kept finite (inf * 0 is NaN, which no comparison clips) and in a range
     * a few transforms cannot overflow. */
    if (!(v <= 1e15))
        v = 1e15;
    return neg ? -v : v;
}


static const char *skip_sep(const char *s)
{
    while (*s && (isspace((unsigned char)*s) || *s == ','))
        s++;
    return s;
}

static int is_numstart(const char *s)
{
    return isdigit((unsigned char)*s) || *s == '.' || *s == '-' || *s == '+';
}

/* A number; advances *sp. 0 on failure. */
static int get_num(const char **sp, double *out)
{
    const char *s = skip_sep(*sp);
    if (!is_numstart(s))
        return 0;
    char *e;
    double v = svg_strtod(s, &e);
    if (e == s)
        return 0;
    *sp = e;
    *out = v;
    return 1;
}

static int get_flag(const char **sp, int *out)
{
    const char *s = skip_sep(*sp);
    if (*s != '0' && *s != '1')
        return 0;
    *out = *s - '0';
    *sp = s + 1;
    return 1;
}

static void parse_path_data(struct path *p, const char *d, double scale)
{
    double x = 0, y = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;
    char cmd = 0, prev = 0;
    for (;;) {
        d = skip_sep(d);
        if (!*d)
            break;
        if (isalpha((unsigned char)*d)) {
            cmd = *d++;
        } else if (!cmd) {
            break;
        } else if (cmd == 'M') {
            cmd = 'L';                  /* extra pairs after a move are lines */
        } else if (cmd == 'm') {
            cmd = 'l';
        }
        int rel = islower((unsigned char)cmd);
        char C = (char)toupper((unsigned char)cmd);
        double v[7];
        int ok = 1;
        int need = C == 'M' || C == 'L' || C == 'T' ? 2 : C == 'H' || C == 'V' ? 1 : C == 'C' ? 6 :
                   C == 'S' || C == 'Q' ? 4 : C == 'A' ? 7 : 0;
        if (C == 'Z') {
            path_close(p);
            struct pt s = { sx, sy };
            x = s.x, y = s.y;
            prev = 'Z';
            continue;
        }
        if (!need)
            break;
        for (int i = 0; i < need && ok; i++) {
            if (C == 'A' && (i == 3 || i == 4)) {
                int fl = 0;
                ok = get_flag(&d, &fl);
                v[i] = fl;
            } else
                ok = get_num(&d, &v[i]);
        }
        if (!ok)
            break;
        double ox = rel ? x : 0, oy = rel ? y : 0;
        switch (C) {
        case 'M':
            x = v[0] + ox, y = v[1] + oy;
            sx = x, sy = y;
            path_move(p, x, y);
            break;
        case 'L':
            x = v[0] + ox, y = v[1] + oy;
            path_line(p, x, y);
            break;
        case 'H':
            x = v[0] + ox;
            path_line(p, x, y);
            break;
        case 'V':
            y = v[0] + oy;
            path_line(p, x, y);
            break;
        case 'C':
            path_cubic(p, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy, v[4] + ox, v[5] + oy, scale);
            lcx = v[2] + ox, lcy = v[3] + oy;
            x = v[4] + ox, y = v[5] + oy;
            break;
        case 'S': {
            double c1x = prev == 'C' || prev == 'S' ? 2 * x - lcx : x;
            double c1y = prev == 'C' || prev == 'S' ? 2 * y - lcy : y;
            path_cubic(p, c1x, c1y, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy, scale);
            lcx = v[0] + ox, lcy = v[1] + oy;
            x = v[2] + ox, y = v[3] + oy;
            break;
        }
        case 'Q':
            path_quad(p, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy, scale);
            lcx = v[0] + ox, lcy = v[1] + oy;
            x = v[2] + ox, y = v[3] + oy;
            break;
        case 'T': {
            double cx = prev == 'Q' || prev == 'T' ? 2 * x - lcx : x;
            double cy = prev == 'Q' || prev == 'T' ? 2 * y - lcy : y;
            path_quad(p, cx, cy, v[0] + ox, v[1] + oy, scale);
            lcx = cx, lcy = cy;
            x = v[0] + ox, y = v[1] + oy;
            break;
        }
        case 'A':
            path_arc(p, v[0], v[1], v[2], (int)v[3], (int)v[4], v[5] + ox, v[6] + oy, scale);
            x = v[5] + ox, y = v[6] + oy;
            break;
        }
        prev = C;
    }
}

/* ---- the element tree ---- */

static int name_is(const struct node *n, const char *name)
{
    return n->type == N_ELEMENT && n->name && !strcmp(n->name, name);
}

static const char *attr(const struct node *n, const char *name)
{
    return node_attr(n, name);
}

static const struct node *find_id(const struct node *root, const char *id, int len)
{
    for (struct node *n = (struct node *)root; n; n = node_next(n, root)) {
        const char *v = n->type == N_ELEMENT ? attr(n, "id") : NULL;
        if (v && (int)strlen(v) == len && !strncmp(v, id, (size_t)len))
            return n;
    }
    return NULL;
}

/* "#id" or "url(#id)": the id and its length, or NULL. */
static const char *ref_id(const char *s, int *len)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!strncmp(s, "url(", 4)) {
        s += 4;
        while (*s == ' ' || *s == '\'' || *s == '"')
            s++;
    }
    if (*s != '#')
        return NULL;
    s++;
    int n = 0;
    while (s[n] && s[n] != ')' && s[n] != '\'' && s[n] != '"' && !isspace((unsigned char)s[n]))
        n++;
    *len = n;
    return s;
}

/* ---- paint and graphics state ---- */

enum { PT_NONE, PT_COLOR, PT_URL };

struct paint {
    int type;
    uint32_t rgb;               /* PT_COLOR: 0xAARRGGBB */
    const char *url;            /* PT_URL */
    int urllen;
};

struct gs {
    struct paint fill, stroke;
    double fill_op, stroke_op, opacity;
    double stroke_w;
    int cap, join;              /* 0 butt/miter, 1 round, 2 square/bevel */
    int evenodd;
    double dash[MAX_DASH];
    int ndash;
    double dash_off;
    uint32_t color;             /* currentColor */
    double font_size;
    int bold, italic;
    int anchor;                 /* 0 start, 1 middle, 2 end */
    int hidden;
};

struct rule {
    char kind;                  /* 't' tag, '.' class, '#' id, '*' any */
    char name[40];
    char tag[24];               /* tag.class */
    const char *decls;
    int dlen;
};

struct canvas {
    int w, h;
    uint32_t *px;
    uint8_t *mask;
    float *acc;
    int ry0, ry1;               /* the rows the mask touches */
    const struct node *root;
    struct rule rules[MAX_RULES];
    int nrules;
    double vw, vh;              /* the viewport, for percentages */
    int failed;
};

static int parse_color_value(const char *s, int n, const struct gs *g, struct paint *out)
{
    while (n > 0 && isspace((unsigned char)*s))
        s++, n--;
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    if (n == 4 && !strncasecmp(s, "none", 4)) {
        out->type = PT_NONE;
        return 1;
    }
    if (n == 12 && !strncasecmp(s, "currentcolor", 12)) {
        out->type = PT_COLOR;
        out->rgb = g->color;
        return 1;
    }
    if (n > 4 && !strncmp(s, "url(", 4)) {
        int len;
        const char *id = ref_id(s, &len);
        if (!id)
            return 0;
        out->type = PT_URL;
        out->url = id;
        out->urllen = len;
        /* "url(#g) red": a fallback we do not need */
        return 1;
    }
    uint32_t c;
    if (css_parse_color(s, n, &c) == 0) {
        out->type = PT_COLOR;
        out->rgb = c;
        return 1;
    }
    return 0;
}

static double parse_len(const char *s, double percent_of, const struct gs *g)
{
    char *e;
    double v = svg_strtod(s, &e);
    if (e == s)
        return 0;
    while (isspace((unsigned char)*e))
        e++;
    if (!strncmp(e, "%", 1))
        return v * percent_of / 100;
    if (!strncmp(e, "pt", 2))
        return v * 4 / 3;
    if (!strncmp(e, "pc", 2))
        return v * 16;
    if (!strncmp(e, "mm", 2))
        return v * 96 / 25.4;
    if (!strncmp(e, "cm", 2))
        return v * 96 / 2.54;
    if (!strncmp(e, "in", 2))
        return v * 96;
    if (!strncmp(e, "em", 2))
        return v * g->font_size;
    if (!strncmp(e, "ex", 2))
        return v * g->font_size / 2;
    return v;
}

static double parse_alpha(const char *v)
{
    char *e;
    double a = svg_strtod(v, &e);
    if (*e == '%')
        a /= 100;
    return a < 0 ? 0 : a > 1 ? 1 : a;
}

static void apply_prop(struct canvas *cv, struct gs *g, const char *name, int nl, const char *val, int vl)
{
    char v[256];
    if (vl <= 0 || vl >= (int)sizeof(v))
        return;
    while (vl > 0 && isspace((unsigned char)*val))
        val++, vl--;
    while (vl > 0 && isspace((unsigned char)val[vl - 1]))
        vl--;
    memcpy(v, val, (size_t)vl);
    v[vl] = 0;
    char *imp = strstr(v, "!important");
    if (imp)
        *imp = 0;
#define IS(s) ((int)sizeof(s) - 1 == nl && !strncmp(name, s, (size_t)nl))
    struct paint p;
    if (IS("fill")) {
        if (parse_color_value(v, (int)strlen(v), g, &p)) {
            if (p.type == PT_URL)
                p.url = val + (p.url - v);      /* (v is only a copy) */
            g->fill = p;
        }
    } else if (IS("stroke")) {
        if (parse_color_value(v, (int)strlen(v), g, &p)) {
            if (p.type == PT_URL)
                p.url = val + (p.url - v);
            g->stroke = p;
        }
    } else if (IS("color")) {
        uint32_t c;
        if (css_parse_color(v, (int)strlen(v), &c) == 0)
            g->color = c;
    } else if (IS("fill-opacity")) {
        g->fill_op = parse_alpha(v);
    } else if (IS("stroke-opacity")) {
        g->stroke_op = parse_alpha(v);
    } else if (IS("opacity")) {
        g->opacity *= parse_alpha(v);
    } else if (IS("stroke-width")) {
        g->stroke_w = parse_len(v, (cv->vw + cv->vh) / 2, g);
    } else if (IS("stroke-linecap")) {
        g->cap = !strcmp(v, "round") ? 1 : !strcmp(v, "square") ? 2 : 0;
    } else if (IS("stroke-linejoin")) {
        g->join = !strcmp(v, "round") ? 1 : !strcmp(v, "bevel") ? 2 : 0;
    } else if (IS("fill-rule")) {
        g->evenodd = !strcmp(v, "evenodd");
    } else if (IS("stroke-dasharray")) {
        g->ndash = 0;
        if (strcmp(v, "none")) {
            const char *s = v;
            while (g->ndash < MAX_DASH) {
                s = skip_sep(s);
                if (!*s)
                    break;
                g->dash[g->ndash++] = parse_len(s, cv->vw, g);
                while (*s && *s != ',' && !isspace((unsigned char)*s))
                    s++;
            }
            double sum = 0;
            for (int i = 0; i < g->ndash; i++) {
                if (!(g->dash[i] >= 0) || !isfinite(g->dash[i]))
                    sum = -1;           /* a negative one makes it all invalid: no dashes */
                if (sum >= 0)
                    sum += g->dash[i];
            }
            if (!(sum > 0) || !isfinite(sum))
                g->ndash = 0;
            if (g->ndash & 1) {
                int n = g->ndash;
                for (int i = 0; i < n && g->ndash < MAX_DASH; i++)
                    g->dash[g->ndash++] = g->dash[i];
            }
        }
    } else if (IS("stroke-dashoffset")) {
        g->dash_off = parse_len(v, cv->vw, g);
    } else if (IS("font-size")) {
        g->font_size = parse_len(v, g->font_size, g);
    } else if (IS("font-weight")) {
        g->bold = !strcmp(v, "bold") || !strcmp(v, "bolder") || atoi(v) >= 600;
    } else if (IS("font-style")) {
        g->italic = !strcmp(v, "italic") || !strcmp(v, "oblique");
    } else if (IS("text-anchor")) {
        g->anchor = !strcmp(v, "middle") ? 1 : !strcmp(v, "end") ? 2 : 0;
    } else if (IS("display")) {
        if (!strcmp(v, "none"))
            g->hidden = 2;
    } else if (IS("visibility")) {
        g->hidden = !strcmp(v, "hidden") || !strcmp(v, "collapse") ? 1 : 0;
    }
#undef IS
}

static void apply_decls(struct canvas *cv, struct gs *g, const char *s, int len)
{
    const char *end = s + len;
    while (s < end) {
        const char *semi = memchr(s, ';', (size_t)(end - s));
        if (!semi)
            semi = end;
        const char *colon = memchr(s, ':', (size_t)(semi - s));
        if (colon) {
            const char *ne = colon;
            while (s < ne && isspace((unsigned char)*s))
                s++;
            while (ne > s && isspace((unsigned char)ne[-1]))
                ne--;
            apply_prop(cv, g, s, (int)(ne - s), colon + 1, (int)(semi - colon - 1));
        }
        s = semi + 1;
    }
}

static int has_class(const struct node *n, const char *cls)
{
    const char *c = attr(n, "class");
    size_t l = strlen(cls);
    while (c && *c) {
        while (isspace((unsigned char)*c))
            c++;
        const char *e = c;
        while (*e && !isspace((unsigned char)*e))
            e++;
        if ((size_t)(e - c) == l && !strncmp(c, cls, l))
            return 1;
        c = e;
    }
    return 0;
}

static const char *const presentation[] = {
    "fill", "stroke", "color", "fill-opacity", "stroke-opacity", "opacity", "stroke-width",
    "stroke-linecap", "stroke-linejoin", "fill-rule", "stroke-dasharray", "stroke-dashoffset",
    "font-size", "font-weight", "font-style", "text-anchor", "display", "visibility",
};

/* The element's style on top of its parent's. */
static void style_of(struct canvas *cv, const struct node *n, struct gs *g)
{
    g->opacity = 1;                     /* (group opacity multiplies down below) */
    for (size_t i = 0; i < sizeof(presentation) / sizeof(presentation[0]); i++) {
        const char *v = attr(n, presentation[i]);
        if (v)
            apply_prop(cv, g, presentation[i], (int)strlen(presentation[i]), v, (int)strlen(v));
    }
    for (int i = 0; i < cv->nrules; i++) {
        const struct rule *r = &cv->rules[i];
        int match = 0;
        if (r->kind == '*')
            match = 1;
        else if (r->kind == 't')
            match = n->name && !strcmp(n->name, r->name);
        else if (r->kind == '#')
            match = attr(n, "id") && !strcmp(attr(n, "id"), r->name) &&
                    (!r->tag[0] || (n->name && !strcmp(n->name, r->tag)));
        else
            match = has_class(n, r->name) && (!r->tag[0] || (n->name && !strcmp(n->name, r->tag)));
        if (match)
            apply_decls(cv, g, r->decls, r->dlen);
    }
    const char *st = attr(n, "style");
    if (st)
        apply_decls(cv, g, st, (int)strlen(st));
}

/* Collect the rules of every <style> in the tree (simple selectors only). */
static void collect_rules(struct canvas *cv)
{
    for (struct node *n = (struct node *)cv->root; n; n = node_next(n, cv->root)) {
        if (!name_is(n, "style"))
            continue;
        for (struct node *t = n->first; t; t = t->next) {
            if (t->type != N_TEXT || !t->text)
                continue;
            const char *s = t->text, *end = t->text + t->len;
            while (s < end && cv->nrules < MAX_RULES) {
                const char *open = memchr(s, '{', (size_t)(end - s));
                if (!open)
                    break;
                const char *close = memchr(open, '}', (size_t)(end - open));
                if (!close)
                    break;
                const char *sel = s;
                while (sel < open) {
                    const char *comma = memchr(sel, ',', (size_t)(open - sel));
                    const char *se = comma ? comma : open;
                    const char *a = sel, *b = se;
                    while (a < b && isspace((unsigned char)*a))
                        a++;
                    while (b > a && isspace((unsigned char)b[-1]))
                        b--;
                    sel = comma ? comma + 1 : open;
                    if (a >= b || *a == '@' || cv->nrules >= MAX_RULES)
                        continue;
                    int simple = 1;
                    for (const char *q = a; q < b; q++)
                        if (isspace((unsigned char)*q) || *q == '>' || *q == '+' || *q == '~' ||
                            *q == ':' || *q == '[')
                            simple = 0;
                    if (!simple)
                        continue;
                    struct rule *r = &cv->rules[cv->nrules];
                    memset(r, 0, sizeof(*r));
                    const char *dot = memchr(a, '.', (size_t)(b - a));
                    if (*a == '*' && b - a == 1) {
                        r->kind = '*';
                    } else if (*a == '.' || *a == '#') {
                        r->kind = *a;
                        a++;
                        snprintf(r->name, sizeof(r->name), "%.*s", (int)(b - a), a);
                    } else if (dot) {
                        r->kind = '.';
                        snprintf(r->tag, sizeof(r->tag), "%.*s", (int)(dot - a), a);
                        snprintf(r->name, sizeof(r->name), "%.*s", (int)(b - dot - 1), dot + 1);
                    } else {
                        r->kind = 't';
                        snprintf(r->name, sizeof(r->name), "%.*s", (int)(b - a), a);
                    }
                    r->decls = open + 1;
                    r->dlen = (int)(close - open - 1);
                    cv->nrules++;
                }
                s = close + 1;
            }
        }
    }
}

/* ---- rasteriser ---- */

struct edge { double x0, y0, x1, y1; int dir; };

struct poly { struct pt *p; int n; };

struct polys {
    struct poly *list;
    int n, cap;
    struct pt *store;           /* all the points */
    int sn, scap;
};

static void polys_free(struct polys *ps)
{
    for (int i = 0; i < ps->n; i++)
        free(ps->list[i].p);
    free(ps->list);
    memset(ps, 0, sizeof(*ps));
}

/* Add a polygon already in device space; its orientation is made positive. */
static void polys_add(struct polys *ps, const struct pt *pts, int n, int norm)
{
    if (n < 3)
        return;
    if (ps->n == ps->cap) {
        ps->cap = ps->cap ? ps->cap * 2 : 16;
        ps->list = realloc(ps->list, sizeof(struct poly) * (size_t)ps->cap);
        if (!ps->list) {
            ps->n = ps->cap = 0;
            return;
        }
    }
    struct pt *c = malloc(sizeof(struct pt) * (size_t)n);
    if (!c)
        return;
    double area = 0;
    for (int i = 0; i < n; i++) {
        const struct pt *a = &pts[i], *b = &pts[(i + 1) % n];
        area += a->x * b->y - b->x * a->y;
    }
    for (int i = 0; i < n; i++)
        c[i] = norm && area < 0 ? pts[n - 1 - i] : pts[i];
    ps->list[ps->n].p = c;
    ps->list[ps->n].n = n;
    ps->n++;
}

static void add_user_poly(struct polys *ps, const struct mat *m, const struct pt *u, int n, int norm)
{
    struct pt tmp[32];
    struct pt *d = n <= 32 ? tmp : malloc(sizeof(struct pt) * (size_t)n);
    if (!d)
        return;
    for (int i = 0; i < n; i++)
        d[i] = mat_pt(m, u[i].x, u[i].y);
    polys_add(ps, d, n, norm);
    if (d != tmp)
        free(d);
}

/* Fill the polygons into cv->mask (0..255 coverage). */
static void rasterize(struct canvas *cv, const struct polys *ps, int evenodd)
{
    memset(cv->mask, 0, (size_t)cv->w * (size_t)cv->h);
    cv->ry0 = cv->ry1 = 0;
    int ne = 0;
    for (int i = 0; i < ps->n; i++)
        ne += ps->list[i].n;
    if (!ne)
        return;
    struct edge *edges = malloc(sizeof(struct edge) * (size_t)ne);
    double *xs = malloc(sizeof(double) * (size_t)(ne + 1));
    int *dirs = malloc(sizeof(int) * (size_t)(ne + 1));
    if (!edges || !xs || !dirs) {
        free(edges);
        free(xs);
        free(dirs);
        return;
    }
    int k = 0;
    double ymin = 1e30, ymax = -1e30;
    for (int i = 0; i < ps->n; i++) {
        const struct poly *pl = &ps->list[i];
        for (int j = 0; j < pl->n; j++) {
            const struct pt *a = &pl->p[j], *b = &pl->p[(j + 1) % pl->n];
            if (a->y == b->y || !isfinite(a->x) || !isfinite(a->y) || !isfinite(b->x) || !isfinite(b->y))
                continue;                   /* flat, or overflowed to inf or NaN */
            struct edge *e = &edges[k++];
            if (a->y < b->y) {
                e->x0 = a->x, e->y0 = a->y, e->x1 = b->x, e->y1 = b->y, e->dir = 1;
            } else {
                e->x0 = b->x, e->y0 = b->y, e->x1 = a->x, e->y1 = a->y, e->dir = -1;
            }
            if (e->y0 < ymin)
                ymin = e->y0;
            if (e->y1 > ymax)
                ymax = e->y1;
        }
    }
    /* Clamped as doubles: out of int range, a cast is undefined. */
    ymin = ymin < 0 ? 0 : ymin > cv->h ? cv->h : ymin;
    ymax = ymax < 0 ? 0 : ymax > cv->h ? cv->h : ymax;
    int y0 = (int)floor(ymin), y1 = (int)ceil(ymax);
    cv->ry0 = y0;
    cv->ry1 = y1;
    for (int y = y0; y < y1; y++) {
        memset(cv->acc, 0, sizeof(float) * (size_t)(cv->w + 2));
        for (int s = 0; s < SS; s++) {
            double sy = y + (s + 0.5) / SS;
            int nx = 0;
            for (int i = 0; i < k; i++) {
                const struct edge *e = &edges[i];
                if (sy < e->y0 || sy >= e->y1)
                    continue;
                double x = e->x0 + (e->x1 - e->x0) * (sy - e->y0) / (e->y1 - e->y0);
                if (isnan(x))
                    continue;           /* inf * 0 of a huge edge */
                int j = nx++;
                while (j > 0 && xs[j - 1] > x) {
                    xs[j] = xs[j - 1];
                    dirs[j] = dirs[j - 1];
                    j--;
                }
                xs[j] = x;
                dirs[j] = e->dir;
            }
            int wind = 0;
            for (int i = 0; i + 1 < nx; i++) {
                wind += dirs[i];
                int inside = evenodd ? ((i + 1) & 1) : wind != 0;
                if (!inside)
                    continue;
                double xa = xs[i], xb = xs[i + 1];
                if (xb <= 0 || xa >= cv->w)
                    continue;
                if (xa < 0)
                    xa = 0;
                if (xb > cv->w)
                    xb = cv->w;
                int ia = (int)xa, ib = (int)xb;
                if (ia == ib) {
                    cv->acc[ia] += (float)(xb - xa);
                } else {
                    cv->acc[ia] += (float)(ia + 1 - xa);
                    int x = ia + 1;
                    for (; x + 3 < ib; x += 4)
                        _mm_storeu_ps(cv->acc + x, _mm_add_ps(_mm_loadu_ps(cv->acc + x), _mm_set1_ps(1)));
                    for (; x < ib; x++)
                        cv->acc[x] += 1;
                    cv->acc[ib] += (float)(xb - ib);
                }
            }
        }
        /* Coverage to bytes, four at a time: clamped, then rounded by
         * truncating with half added (CVTTPS2DQ). */
        uint8_t *row = cv->mask + (size_t)y * (size_t)cv->w;
        const __m128 k = _mm_set1_ps(255.0f / SS), top = _mm_set1_ps(255), half = _mm_set1_ps(0.5f);
        int x = 0;
        for (; x + 3 < cv->w; x += 4) {
            __m128 c = _mm_min_ps(_mm_max_ps(_mm_mul_ps(_mm_loadu_ps(cv->acc + x), k), _mm_setzero_ps()), top);
            c = _mm_add_ps(c, half);
            __m128i w = _mm_cvttps_epi32(c);
            w = _mm_packs_epi32(w, w);
            *(u32_any *)(row + x) = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(w, w));
        }
        for (; x < cv->w; x++) {
            float c = cv->acc[x] * (255.0f / SS);
            row[x] = c >= 255 ? 255 : c <= 0 ? 0 : (uint8_t)(c + 0.5f);
        }
    }
    free(edges);
    free(xs);
    free(dirs);
}

/* ---- shading ---- */

struct stop { double off; uint32_t rgb; };

struct shader {
    int kind;                   /* 0 solid, 1 linear, 2 radial */
    uint32_t rgb;
    struct stop stops[16];
    int nstops;
    struct mat inv;             /* device -> gradient space */
    double x1, y1, x2, y2, r;
};

static uint32_t mix(uint32_t a, uint32_t b, double t)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        double ca = (a >> sh) & 255, cb = (b >> sh) & 255;
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5) << sh;
    }
    return out;
}

static uint32_t shade(const struct shader *sh, double px, double py)
{
    if (!sh->kind)
        return sh->rgb;
    struct pt p = mat_pt(&sh->inv, px, py);
    double t;
    if (sh->kind == 1) {
        double dx = sh->x2 - sh->x1, dy = sh->y2 - sh->y1, l = dx * dx + dy * dy;
        t = l > 0 ? ((p.x - sh->x1) * dx + (p.y - sh->y1) * dy) / l : 0;
    } else {
        double dx = p.x - sh->x1, dy = p.y - sh->y1;
        t = sh->r > 0 ? sqrt(dx * dx + dy * dy) / sh->r : 0;
    }
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    if (sh->nstops == 0)
        return 0;
    if (t <= sh->stops[0].off)
        return sh->stops[0].rgb;
    for (int i = 1; i < sh->nstops; i++)
        if (t <= sh->stops[i].off) {
            double span = sh->stops[i].off - sh->stops[i - 1].off;
            return mix(sh->stops[i - 1].rgb, sh->stops[i].rgb,
                       span > 0 ? (t - sh->stops[i - 1].off) / span : 1);
        }
    return sh->stops[sh->nstops - 1].rgb;
}

/* A gradient's number: a plain value or a percentage of 1 (or of the viewport in user space). */
static double grad_len(const struct node *g, const char *name, double dflt, int obb, double vp)
{
    const char *v = attr(g, name);
    if (!v)
        return dflt;
    char *e;
    double x = svg_strtod(v, &e);
    if (e == v)
        return dflt;
    if (*e == '%')
        return obb ? x / 100 : x * vp / 100;
    return x;
}

static const struct node *grad_href(struct canvas *cv, const struct node *g)
{
    const char *h = attr(g, "href");
    if (!h)
        h = attr(g, "xlink:href");
    int len;
    const char *id = h ? ref_id(h, &len) : NULL;
    return id ? find_id(cv->root, id, len) : NULL;
}

/* Set up the shader for a paint over a shape with this device matrix and
 * user-space bounding box. Returns 0 if the paint draws nothing. */
static int make_shader(struct canvas *cv, const struct paint *p, const struct gs *g, const struct mat *ctm,
                       const double bbox[4], struct shader *sh)
{
    memset(sh, 0, sizeof(*sh));
    if (p->type == PT_NONE)
        return 0;
    if (p->type == PT_COLOR) {
        sh->rgb = p->rgb;
        return 1;
    }
    const struct node *gn = find_id(cv->root, p->url, p->urllen);
    if (!gn || !(name_is(gn, "lineargradient") || name_is(gn, "radialgradient")))
        return 0;
    int radial = name_is(gn, "radialgradient");
    /* stops come from the gradient or the one it links to */
    const struct node *sn = gn;
    for (int depth = 0; sn && depth < 4; depth++) {
        int found = 0;
        for (struct node *c = sn->first; c; c = c->next)
            if (name_is(c, "stop"))
                found = 1;
        if (found)
            break;
        sn = grad_href(cv, sn);
    }
    if (!sn)
        return 0;
    for (struct node *c = sn->first; c && sh->nstops < 16; c = c->next) {
        if (!name_is(c, "stop"))
            continue;
        struct gs tmp = *g;
        struct paint col = { PT_COLOR, 0xFF000000, NULL, 0 };
        double off = 0, op = 1;
        const char *o = attr(c, "offset");
        if (o) {
            char *e;
            off = svg_strtod(o, &e);
            if (*e == '%')
                off /= 100;
        }
        const char *sc = attr(c, "stop-color"), *so = attr(c, "stop-opacity"), *st = attr(c, "style");
        if (st) {
            const char *q = strstr(st, "stop-color:");
            if (q) {
                q += 11;
                const char *e = strchr(q, ';');
                parse_color_value(q, (int)(e ? e - q : (int)strlen(q)), &tmp, &col);
            }
            q = strstr(st, "stop-opacity:");
            if (q)
                op = parse_alpha(q + 13);
        }
        if (sc)
            parse_color_value(sc, (int)strlen(sc), &tmp, &col);
        if (so)
            op = parse_alpha(so);
        if (col.type != PT_COLOR)
            continue;
        uint32_t a = (uint32_t)(((col.rgb >> 24) & 255) * op + 0.5);
        if (off < 0)
            off = 0;
        if (off > 1)
            off = 1;
        if (sh->nstops && off < sh->stops[sh->nstops - 1].off)
            off = sh->stops[sh->nstops - 1].off;
        sh->stops[sh->nstops].off = off;
        sh->stops[sh->nstops].rgb = a << 24 | (col.rgb & 0xFFFFFF);
        sh->nstops++;
    }
    if (!sh->nstops)
        return 0;
    if (sh->nstops == 1) {
        sh->rgb = sh->stops[0].rgb;
        sh->kind = 0;
        return 1;
    }
    const char *units = attr(gn, "gradientunits");
    int obb = !units || strcmp(units, "userSpaceOnUse");
    if (obb && (bbox[2] <= 0 || bbox[3] <= 0))
        return 0;
    sh->kind = radial ? 2 : 1;
    if (radial) {
        sh->x1 = grad_len(gn, "cx", obb ? 0.5 : cv->vw / 2, obb, cv->vw);
        sh->y1 = grad_len(gn, "cy", obb ? 0.5 : cv->vh / 2, obb, cv->vh);
        sh->r = grad_len(gn, "r", obb ? 0.5 : (cv->vw + cv->vh) / 4, obb, (cv->vw + cv->vh) / 2);
    } else {
        sh->x1 = grad_len(gn, "x1", 0, obb, cv->vw);
        sh->y1 = grad_len(gn, "y1", 0, obb, cv->vh);
        sh->x2 = grad_len(gn, "x2", obb ? 1 : cv->vw, obb, cv->vw);
        sh->y2 = grad_len(gn, "y2", 0, obb, cv->vh);
    }
    struct mat m = *ctm;
    if (obb) {
        struct mat bb = { bbox[2], 0, 0, bbox[3], bbox[0], bbox[1] };
        m = mat_mul(m, bb);
    }
    const char *gt = attr(gn, "gradienttransform");
    if (gt) {
        struct mat t = identity;
        if (svg_parse_transform(gt, &t) == 0)
            m = mat_mul(m, t);
    }
    return mat_inv(&m, &sh->inv) == 0;
}

/* Blend the mask, shaded, over the canvas. */
static void composite(struct canvas *cv, const struct shader *sh, double alpha)
{
    for (int y = cv->ry0; y < cv->ry1; y++) {
        const uint8_t *m = cv->mask + (size_t)y * (size_t)cv->w;
        uint32_t *d = cv->px + (size_t)y * (size_t)cv->w;
        for (int x = 0; x < cv->w; x++) {
            if (!m[x])
                continue;
            uint32_t c = shade(sh, x + 0.5, y + 0.5);
            double sa = (c >> 24) / 255.0 * m[x] / 255.0 * alpha;
            if (sa <= 0)
                continue;
            uint32_t dc = d[x];
            double da = (dc >> 24) / 255.0;
            double oa = sa + da * (1 - sa);
            uint32_t out = (uint32_t)(oa * 255 + 0.5) << 24;
            for (int sh2 = 0; sh2 < 24; sh2 += 8) {
                double s = (c >> sh2) & 255, dd = (dc >> sh2) & 255;
                double v = (s * sa + dd * da * (1 - sa)) / oa;
                out |= (uint32_t)(v + 0.5) << sh2;
            }
            d[x] = out;
        }
    }
}

/* ---- strokes ---- */

static void disc(struct polys *ps, const struct mat *m, struct pt c, double r)
{
    struct pt p[16];
    for (int i = 0; i < 16; i++) {
        double a = 2 * PI * i / 16;
        p[i].x = c.x + r * cos(a);
        p[i].y = c.y + r * sin(a);
    }
    add_user_poly(ps, m, p, 16, 1);
}

static void stroke_line(struct polys *ps, const struct mat *m, const struct pt *pts, int n, int closed,
                        const struct gs *g)
{
    double hw = g->stroke_w / 2;
    if (hw <= 0)
        return;
    /* drop repeated points */
    struct pt *q = malloc(sizeof(struct pt) * (size_t)n);
    if (!q)
        return;
    int qn = 0;
    for (int i = 0; i < n; i++)
        if (!qn || fabs(pts[i].x - q[qn - 1].x) > 1e-9 || fabs(pts[i].y - q[qn - 1].y) > 1e-9)
            q[qn++] = pts[i];
    if (closed && qn > 1 && fabs(q[0].x - q[qn - 1].x) < 1e-9 && fabs(q[0].y - q[qn - 1].y) < 1e-9)
        qn--;
    if (qn == 1) {
        if (g->cap == 1)
            disc(ps, m, q[0], hw);
        else if (g->cap == 2) {
            struct pt s[4] = { { q[0].x - hw, q[0].y - hw }, { q[0].x + hw, q[0].y - hw },
                               { q[0].x + hw, q[0].y + hw }, { q[0].x - hw, q[0].y + hw } };
            add_user_poly(ps, m, s, 4, 1);
        }
        free(q);
        return;
    }
    int nseg = closed ? qn : qn - 1;
    for (int i = 0; i < nseg; i++) {
        struct pt a = q[i], b = q[(i + 1) % qn];
        double dx = b.x - a.x, dy = b.y - a.y, l = sqrt(dx * dx + dy * dy);
        if (l < 1e-12)
            continue;
        dx /= l, dy /= l;
        if (!closed && g->cap == 2) {
            if (i == 0)
                a.x -= dx * hw, a.y -= dy * hw;
            if (i == nseg - 1)
                b.x += dx * hw, b.y += dy * hw;
        }
        double nx = -dy * hw, ny = dx * hw;
        struct pt s[4] = { { a.x + nx, a.y + ny }, { b.x + nx, b.y + ny }, { b.x - nx, b.y - ny },
                           { a.x - nx, a.y - ny } };
        add_user_poly(ps, m, s, 4, 1);
    }
    /* joins */
    int j0 = closed ? 0 : 1, j1 = closed ? qn : qn - 1;
    for (int i = j0; i < j1; i++) {
        struct pt p = q[i], a = q[(i + qn - 1) % qn], b = q[(i + 1) % qn];
        double d1x = p.x - a.x, d1y = p.y - a.y, d2x = b.x - p.x, d2y = b.y - p.y;
        double l1 = sqrt(d1x * d1x + d1y * d1y), l2 = sqrt(d2x * d2x + d2y * d2y);
        if (l1 < 1e-12 || l2 < 1e-12)
            continue;
        d1x /= l1, d1y /= l1, d2x /= l2, d2y /= l2;
        if (g->join == 1) {
            disc(ps, m, p, hw);
            continue;
        }
        double cross = d1x * d2y - d1y * d2x, dot = d1x * d2x + d1y * d2y;
        if (fabs(cross) < 1e-9)
            continue;
        double sgn = cross > 0 ? -1 : 1;
        struct pt A = { p.x + sgn * -d1y * hw, p.y + sgn * d1x * hw };
        struct pt B = { p.x + sgn * -d2y * hw, p.y + sgn * d2x * hw };
        double n1x = -d1y, n1y = d1x, n2x = -d2y, n2y = d2x;
        if (g->join == 0 && 1 + dot >= 2.0 / 16) {
            double k = sgn * hw / (1 + dot);
            struct pt M = { p.x + k * (n1x + n2x), p.y + k * (n1y + n2y) };
            struct pt s[4] = { p, A, M, B };
            add_user_poly(ps, m, s, 4, 1);
        } else {
            struct pt s[3] = { p, A, B };
            add_user_poly(ps, m, s, 3, 1);
        }
    }
    if (!closed && g->cap == 1) {
        disc(ps, m, q[0], hw);
        disc(ps, m, q[qn - 1], hw);
    }
    free(q);
}

static void stroke_path(struct polys *ps, const struct mat *m, const struct path *p, const struct gs *g)
{
    for (int i = 0; i < p->ns; i++) {
        const struct sub *s = &p->subs[i];
        const struct pt *pts = &p->pts[s->start];
        if (!g->ndash) {
            stroke_line(ps, m, pts, s->n, s->closed, g);
            continue;
        }
        /* dashes: walk the polyline cutting it into pieces */
        int n = s->n + (s->closed ? 1 : 0);
        double total = 0, len = 0;
        for (int j = 0; j < g->ndash; j++)
            total += g->dash[j];
        for (int j = 0; j + 1 < n; j++) {
            struct pt a = pts[j % s->n], b = pts[(j + 1) % s->n];
            len += sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        }
        /* Dashes too small for the length would take forever (or never end,
         * once a step is below the precision of the position): solid. */
        if (!(total > 0) || !isfinite(len) || !isfinite(g->dash_off) ||
            len / total * g->ndash > MAX_DASH_STEPS) {
            stroke_line(ps, m, pts, s->n, s->closed, g);
            continue;
        }
        double pos = fmod(g->dash_off, total);
        if (pos < 0)
            pos += total;
        int di = 0;
        for (int k = 0; k < 2 * g->ndash && pos >= g->dash[di]; k++) {
            pos -= g->dash[di];
            di = (di + 1) % g->ndash;
        }
        int steps = 0;
        double left = g->dash[di] - pos > 0 ? g->dash[di] - pos : 0;
        int on = !(di & 1);
        struct pt cur[512];
        int nc = 0;
        for (int j = 0; j + 1 < n; j++) {
            struct pt a = pts[j % s->n], b = pts[(j + 1) % s->n];
            double dx = b.x - a.x, dy = b.y - a.y, l = sqrt(dx * dx + dy * dy), at = 0;
            if (l < 1e-12)
                continue;
            if (on && nc == 0)
                cur[nc++] = a;
            while (l - at > left && steps++ < 2 * MAX_DASH_STEPS) {
                at += left;
                struct pt c = { a.x + dx * at / l, a.y + dy * at / l };
                if (on) {
                    cur[nc++] = c;
                    stroke_line(ps, m, cur, nc, 0, g);
                    nc = 0;
                } else {
                    cur[0] = c;
                    nc = 1;
                }
                on = !on;
                di = (di + 1) % g->ndash;
                left = g->dash[di];
                if (left <= 0)
                    left = 1e-6;
                if (nc >= 500)
                    nc = 1;
            }
            left -= l - at;
            if (on && nc < 500)
                cur[nc++] = b;
        }
        if (on && nc > 1)
            stroke_line(ps, m, cur, nc, 0, g);
    }
}

/* ---- drawing a shape ---- */

static void bounds_of(const struct path *p, double bb[4])
{
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (int i = 0; i < p->np; i++) {
        if (p->pts[i].x < x0) x0 = p->pts[i].x;
        if (p->pts[i].x > x1) x1 = p->pts[i].x;
        if (p->pts[i].y < y0) y0 = p->pts[i].y;
        if (p->pts[i].y > y1) y1 = p->pts[i].y;
    }
    if (x0 > x1)
        x0 = y0 = x1 = y1 = 0;
    bb[0] = x0, bb[1] = y0, bb[2] = x1 - x0, bb[3] = y1 - y0;
}

static void draw_path(struct canvas *cv, const struct path *p, const struct mat *ctm, const struct gs *g)
{
    if (!p->np || g->hidden)
        return;
    double bb[4];
    bounds_of(p, bb);
    struct shader sh;
    if (g->fill.type != PT_NONE && make_shader(cv, &g->fill, g, ctm, bb, &sh)) {
        struct polys ps = { 0 };
        for (int i = 0; i < p->ns; i++)
            if (p->subs[i].n >= 3)
                add_user_poly(&ps, ctm, &p->pts[p->subs[i].start], p->subs[i].n, 0);
        rasterize(cv, &ps, g->evenodd);
        composite(cv, &sh, g->fill_op * g->opacity);
        polys_free(&ps);
    }
    if (g->stroke.type != PT_NONE && g->stroke_w > 0 && make_shader(cv, &g->stroke, g, ctm, bb, &sh)) {
        struct polys ps = { 0 };
        stroke_path(&ps, ctm, p, g);
        rasterize(cv, &ps, 0);
        composite(cv, &sh, g->stroke_op * g->opacity);
        polys_free(&ps);
    }
}

static double len_attr(struct canvas *cv, const struct node *n, const char *name, double pct, const struct gs *g)
{
    const char *v = attr(n, name);
    return v ? parse_len(v, pct, g) : 0;
}

static int svg_parse_transform(const char *s, struct mat *out)
{
    struct mat m = identity;
    while (*s) {
        s = skip_sep(s);
        if (!*s)
            break;
        const char *name = s;
        while (isalpha((unsigned char)*s))
            s++;
        int nl = (int)(s - name);
        while (isspace((unsigned char)*s))
            s++;
        if (*s != '(')
            return -1;
        s++;
        double v[6] = { 0 };
        int n = 0;
        while (n < 6 && get_num(&s, &v[n]))
            n++;
        s = skip_sep(s);
        if (*s != ')')
            return -1;
        s++;
        struct mat t = identity;
        if (nl == 6 && !strncmp(name, "matrix", 6) && n == 6) {
            t.a = v[0], t.b = v[1], t.c = v[2], t.d = v[3], t.e = v[4], t.f = v[5];
        } else if (nl == 9 && !strncmp(name, "translate", 9) && n >= 1) {
            t.e = v[0], t.f = n > 1 ? v[1] : 0;
        } else if (nl == 5 && !strncmp(name, "scale", 5) && n >= 1) {
            t.a = v[0], t.d = n > 1 ? v[1] : v[0];
        } else if (nl == 6 && !strncmp(name, "rotate", 6) && n >= 1) {
            double a = v[0] * PI / 180, c = cos(a), sn = sin(a);
            t.a = c, t.b = sn, t.c = -sn, t.d = c;
            if (n >= 3) {
                struct mat to = { 1, 0, 0, 1, v[1], v[2] }, back = { 1, 0, 0, 1, -v[1], -v[2] };
                t = mat_mul(to, mat_mul(t, back));
            }
        } else if (nl == 5 && !strncmp(name, "skewX", 5) && n >= 1) {
            t.c = tan(v[0] * PI / 180);
        } else if (nl == 5 && !strncmp(name, "skewY", 5) && n >= 1) {
            t.b = tan(v[0] * PI / 180);
        } else {
            return -1;
        }
        m = mat_mul(m, t);
    }
    *out = m;
    return 0;
}

static void draw_text(struct canvas *cv, const struct node *n, const struct mat *ctm, const struct gs *g,
                      double x, double y)
{
    if (g->hidden || g->fill.type == PT_NONE)
        return;
    char buf[512];
    int len = 0, space = 1;
    for (struct node *k = n->first; k && len < (int)sizeof(buf) - 4; k = k->next) {
        const struct node *t = k;
        if (k->type == N_ELEMENT && k->first && k->first->type == N_TEXT)
            t = k->first;               /* <tspan> */
        if (t->type != N_TEXT || !t->text)
            continue;
        for (int i = 0; i < t->len && len < (int)sizeof(buf) - 4; i++) {
            unsigned char ch = (unsigned char)t->text[i];
            if (isspace(ch)) {
                if (!space)
                    buf[len++] = ' ';
                space = 1;
            } else {
                buf[len++] = (char)ch;
                space = 0;
            }
        }
    }
    while (len > 0 && buf[len - 1] == ' ')
        len--;
    if (!len)
        return;
    if (wfont_init() < 0)
        return;
    double sc = mat_scale(ctm);
    int px = (int)(g->font_size * sc + 0.5);
    if (px < 4)
        return;
    if (px > 100)
        px = 100;
    const struct wfont *f = wfont_get(FAM_SANS, g->bold, g->italic, px);
    if (!f)
        return;
    struct pt o = mat_pt(ctm, x, y);
    int w = wfont_width(f, buf, len);
    int left = (int)(o.x + 0.5) - (g->anchor == 1 ? w / 2 : g->anchor == 2 ? w : 0);
    int top = (int)(o.y + 0.5) - wfont_ascent(f);
    /* draw white on black to get the coverage */
    uint32_t *tmp = malloc(sizeof(uint32_t) * (size_t)cv->w * (size_t)cv->h);
    if (!tmp)
        return;
    for (int i = 0; i < cv->w * cv->h; i++)
        tmp[i] = 0;
    struct surface s;
    s.px = tmp;
    s.w = cv->w;
    s.h = cv->h;
    s.stride = cv->w;
    s.clip = mkrect(0, 0, cv->w, cv->h);
    wfont_draw(&s, f, left, top, buf, len, 0xFFFFFF);
    for (int i = 0; i < cv->w * cv->h; i++)
        cv->mask[i] = (uint8_t)(tmp[i] & 255);
    free(tmp);
    cv->ry0 = 0;
    cv->ry1 = cv->h;
    struct shader sh;
    double bb[4] = { x, y - g->font_size, 1, g->font_size };
    if (make_shader(cv, &g->fill, g, ctm, bb, &sh))
        composite(cv, &sh, g->fill_op * g->opacity);
}

static void draw_node(struct canvas *cv, const struct node *n, struct mat ctm, struct gs g, int depth);

static void draw_children(struct canvas *cv, const struct node *n, const struct mat *ctm, const struct gs *g,
                          int depth)
{
    for (struct node *k = n->first; k; k = k->next)
        if (k->type == N_ELEMENT)
            draw_node(cv, k, *ctm, *g, depth + 1);
}

static void draw_node_impl(struct canvas *cv,const struct node *n,struct mat ctm,struct gs g,int depth);
/* Resolve local clip paths and masks as nested groups, retaining antialiased coverage. */
static const char *effect_reference(const struct node *n,const char *name,char out[256])
{
    const char *value=attr(n,name);if(value)return value;
    const char *style=attr(n,"style");size_t len=strlen(name);
    for(const char *p=style?style:"";*p;) {
        while(isspace((unsigned char)*p) || *p==';')p++;
        const char *end=strchr(p,';');if(!end)end=p+strlen(p);
        const char *colon=memchr(p,':',(size_t)(end-p));
        const char *ne=colon;while(ne && ne>p && isspace((unsigned char)ne[-1]))ne--;
        if(colon && (size_t)(ne-p)==len && !strncasecmp(p,name,len)) {
            const char *v=colon+1;while(v<end && isspace((unsigned char)*v))v++;
            snprintf(out,256,"%.*s",(int)(end-v),v);return out;
        }
        p=*end?end+1:end;
    }
    return NULL;
}
static uint32_t svg_over(uint32_t dst,uint32_t src)
{
    unsigned sa=src>>24,da=dst>>24,a=sa*255+da*(255-sa);if(!a)return 0;
    uint32_t out=((a+127)/255)<<24;
    for(int shift=0;shift<24;shift+=8)out|=(((src>>shift&255)*sa*255+(dst>>shift&255)*da*(255-sa)+a/2)/a)<<shift;
    return out;
}
static void draw_node(struct canvas *cv,const struct node *n,struct mat ctm,struct gs g,int depth)
{
    if(depth>MAX_DEPTH || cv->failed || !n->name)return;
    char value[256];const char *spec=effect_reference(n,"clip-path",value);int len=0,is_mask=0;
    const char *id=spec?ref_id(spec,&len):NULL;
    const struct node *clip=id?find_id(cv->root,id,len):NULL;
    if(!clip) {
        spec=effect_reference(n,"mask",value);id=spec?ref_id(spec,&len):NULL;
        clip=id?find_id(cv->root,id,len):NULL;is_mask=clip!=NULL;
    }
    if(!clip || clip==n){draw_node_impl(cv,n,ctm,g,depth);return;}
    size_t pixels=(size_t)cv->w*cv->h;
    uint32_t *source=calloc(pixels,sizeof(uint32_t)),*coverage=calloc(pixels,sizeof(uint32_t));
    if(!source || !coverage){free(source);free(coverage);cv->failed=1;return;}
    struct canvas layer=*cv;layer.px=source;draw_node_impl(&layer,n,ctm,g,depth);
    struct mat clipmat=ctm,t;
    const char *transform=attr(n,"transform");if(transform && svg_parse_transform(transform,&t)==0)clipmat=mat_mul(clipmat,t);
    const char *units=attr(clip,is_mask?"maskcontentunits":"clippathunits");
    if(units && !strcasecmp(units,"objectBoundingBox")) {
        int x0=cv->w,y0=cv->h,x1=0,y1=0;
        for(int y=0;y<cv->h;y++)for(int x=0;x<cv->w;x++)if(source[(size_t)y*cv->w+x]>>24) {
            if(x<x0)x0=x;
            if(x+1>x1)x1=x+1;
            if(y<y0)y0=y;
            if(y+1>y1)y1=y+1;
        }
        clipmat=(struct mat){x1-x0,0,0,y1-y0,x0,y0};
    }
    transform=attr(clip,"transform");if(transform && svg_parse_transform(transform,&t)==0)clipmat=mat_mul(clipmat,t);
    struct gs cg=g;cg.fill.type=PT_COLOR;cg.fill.rgb=0xffffffffu;cg.stroke.type=PT_NONE;cg.fill_op=cg.opacity=1;
    layer.px=coverage;draw_children(&layer,clip,&clipmat,&cg,depth+1);
    int alpha=is_mask && attr(clip,"mask-type") && !strcasecmp(attr(clip,"mask-type"),"alpha");
    for(size_t i=0;i<pixels;i++) {
        uint32_t p=source[i],m=coverage[i];unsigned mask=m>>24;
        if(is_mask && !alpha)mask=mask*(54*(m>>16&255)+183*(m>>8&255)+19*(m&255))/(256*255);
        p=(p&0xffffffu)|(((p>>24)*mask+127)/255)<<24;
        cv->px[i]=svg_over(cv->px[i],p);
    }
    cv->failed|=layer.failed;free(source);free(coverage);
}

static void draw_node_impl(struct canvas *cv, const struct node *n, struct mat ctm, struct gs g, int depth)
{
    if (depth > MAX_DEPTH || cv->failed || !n->name)
        return;
    const char *nm = n->name;
    if (!strcmp(nm, "defs") || !strcmp(nm, "style") || !strcmp(nm, "title") || !strcmp(nm, "desc") ||
        !strcmp(nm, "lineargradient") || !strcmp(nm, "radialgradient") || !strcmp(nm, "clippath") ||
        !strcmp(nm, "mask") || !strcmp(nm, "pattern") || !strcmp(nm, "filter") || !strcmp(nm, "symbol") ||
        !strcmp(nm, "marker") || !strcmp(nm, "metadata") || !strcmp(nm, "script"))
        return;
    double op = g.opacity;
    style_of(cv, n, &g);
    g.opacity *= op;
    if (g.hidden == 2)
        return;
    const char *tr = attr(n, "transform");
    if (tr) {
        struct mat t;
        if (svg_parse_transform(tr, &t) == 0)
            ctm = mat_mul(ctm, t);
    }
    double scale = mat_scale(&ctm);
    struct path p = { 0 };
    if (!strcmp(nm, "g") || !strcmp(nm, "a") || !strcmp(nm, "svg") || !strcmp(nm, "switch")) {
        if (!strcmp(nm, "svg") && depth > 0) {
            struct mat t = { 1, 0, 0, 1, len_attr(cv, n, "x", cv->vw, &g), len_attr(cv, n, "y", cv->vh, &g) };
            ctm = mat_mul(ctm, t);
        }
        draw_children(cv, n, &ctm, &g, depth);
        return;
    }
    if (!strcmp(nm, "use")) {
        const char *h = attr(n, "href");
        if (!h)
            h = attr(n, "xlink:href");
        int len;
        const char *id = h ? ref_id(h, &len) : NULL;
        const struct node *t = id ? find_id(cv->root, id, len) : NULL;
        if (!t || t == n)
            return;
        struct mat tr2 = { 1, 0, 0, 1, len_attr(cv, n, "x", cv->vw, &g), len_attr(cv, n, "y", cv->vh, &g) };
        ctm = mat_mul(ctm, tr2);
        if (name_is(t, "symbol")) {
            draw_children(cv, t, &ctm, &g, depth);
        } else {
            /* draw the target even if it sits in <defs> */
            draw_node(cv, t, ctm, g, depth + 1);
        }
        return;
    }
    if (!strcmp(nm, "rect")) {
        double x = len_attr(cv, n, "x", cv->vw, &g), y = len_attr(cv, n, "y", cv->vh, &g);
        double w = len_attr(cv, n, "width", cv->vw, &g), h = len_attr(cv, n, "height", cv->vh, &g);
        double rx = len_attr(cv, n, "rx", cv->vw, &g), ry = len_attr(cv, n, "ry", cv->vh, &g);
        if (w <= 0 || h <= 0)
            return;
        if (!attr(n, "rx"))
            rx = ry;
        if (!attr(n, "ry"))
            ry = rx;
        if (rx > w / 2)
            rx = w / 2;
        if (ry > h / 2)
            ry = h / 2;
        if (rx <= 0 || ry <= 0) {
            path_move(&p, x, y);
            path_line(&p, x + w, y);
            path_line(&p, x + w, y + h);
            path_line(&p, x, y + h);
            path_close(&p);
        } else {
            path_move(&p, x + rx, y);
            path_line(&p, x + w - rx, y);
            path_arc(&p, rx, ry, 0, 0, 1, x + w, y + ry, scale);
            path_line(&p, x + w, y + h - ry);
            path_arc(&p, rx, ry, 0, 0, 1, x + w - rx, y + h, scale);
            path_line(&p, x + rx, y + h);
            path_arc(&p, rx, ry, 0, 0, 1, x, y + h - ry, scale);
            path_line(&p, x, y + ry);
            path_arc(&p, rx, ry, 0, 0, 1, x + rx, y, scale);
            path_close(&p);
        }
    } else if (!strcmp(nm, "circle") || !strcmp(nm, "ellipse")) {
        double cx = len_attr(cv, n, "cx", cv->vw, &g), cy = len_attr(cv, n, "cy", cv->vh, &g);
        double rx, ry;
        if (nm[0] == 'c')
            rx = ry = len_attr(cv, n, "r", (cv->vw + cv->vh) / 2, &g);
        else
            rx = len_attr(cv, n, "rx", cv->vw, &g), ry = len_attr(cv, n, "ry", cv->vh, &g);
        if (rx <= 0 || ry <= 0)
            return;
        path_move(&p, cx + rx, cy);
        path_arc(&p, rx, ry, 0, 0, 1, cx, cy + ry, scale);
        path_arc(&p, rx, ry, 0, 0, 1, cx - rx, cy, scale);
        path_arc(&p, rx, ry, 0, 0, 1, cx, cy - ry, scale);
        path_arc(&p, rx, ry, 0, 0, 1, cx + rx, cy, scale);
        path_close(&p);
    } else if (!strcmp(nm, "line")) {
        path_move(&p, len_attr(cv, n, "x1", cv->vw, &g), len_attr(cv, n, "y1", cv->vh, &g));
        path_line(&p, len_attr(cv, n, "x2", cv->vw, &g), len_attr(cv, n, "y2", cv->vh, &g));
        g.fill.type = PT_NONE;
    } else if (!strcmp(nm, "polyline") || !strcmp(nm, "polygon")) {
        const char *s = attr(n, "points");
        double x, y;
        int first = 1;
        while (s && get_num(&s, &x) && get_num(&s, &y)) {
            if (first)
                path_move(&p, x, y);
            else
                path_line(&p, x, y);
            first = 0;
        }
        if (nm[4] == 'g')
            path_close(&p);
    } else if (!strcmp(nm, "path")) {
        const char *d = attr(n, "d");
        if (d)
            parse_path_data(&p, d, scale);
    } else if (!strcmp(nm, "text")) {
        const char *xs = attr(n, "x"), *ys = attr(n, "y");
        double x = xs ? parse_len(xs, cv->vw, &g) : 0, y = ys ? parse_len(ys, cv->vh, &g) : 0;
        const char *dx = attr(n, "dx"), *dy = attr(n, "dy");
        if (dx)
            x += parse_len(dx, cv->vw, &g);
        if (dy)
            y += parse_len(dy, cv->vh, &g);
        draw_text(cv, n, &ctm, &g, x, y);
        return;
    } else {
        return;
    }
    draw_path(cv, &p, &ctm, &g);
    path_free(&p);
}

/* ---- entry points ---- */

static double svg_dim(const struct node *svg, const char *name)
{
    const char *v = attr(svg, name);
    if (!v)
        return 0;
    struct gs g = { 0 };
    g.font_size = 16;
    char *e;
    svg_strtod(v, &e);
    if (*e == '%')
        return 0;
    return parse_len(v, 0, &g);
}

static int view_box(const struct node *svg, double vb[4])
{
    const char *v = attr(svg, "viewbox");
    if (!v)
        return 0;
    for (int i = 0; i < 4; i++)
        if (!get_num(&v, &vb[i]))
            return 0;
    return vb[2] > 0 && vb[3] > 0;
}

void svg_size(const struct node *svg, int *w, int *h)
{
    double sw = svg_dim(svg, "width"), sh = svg_dim(svg, "height"), vb[4];
    int has = view_box(svg, vb);
    if (sw <= 0 && sh <= 0) {
        if (has)
            sw = vb[2], sh = vb[3];
        else
            sw = 300, sh = 150;
    } else if (sw <= 0) {
        sw = has ? sh * vb[2] / vb[3] : sh * 2;
    } else if (sh <= 0) {
        sh = has ? sw * vb[3] / vb[2] : sw / 2;
    }
    *w = sw < 1e6 ? (int)(sw + 0.5) : 1000000;     /* out of int range, a cast is undefined */
    *h = sh < 1e6 ? (int)(sh + 0.5) : 1000000;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

struct image *svg_render(const struct node *svg, int w, int h, uint32_t color)
{
    if (w < 1 || h < 1 || w > MAX_PX || h > MAX_PX)
        return NULL;
    struct canvas *cv = calloc(1, sizeof(*cv));
    struct image *img = malloc(sizeof(*img));
    if (!cv || !img) {
        free(cv);
        free(img);
        return NULL;
    }
    img->w = w;
    img->h = h;
    img->px = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    cv->w = w;
    cv->h = h;
    cv->px = img->px;
    cv->mask = malloc((size_t)w * (size_t)h);
    cv->acc = malloc(sizeof(float) * (size_t)(w + 2));
    cv->root = svg;
    if (!img->px || !cv->mask || !cv->acc) {
        image_free(img);
        free(cv->mask);
        free(cv->acc);
        free(cv);
        return NULL;
    }
    collect_rules(cv);

    struct mat ctm = identity;
    double vb[4];
    cv->vw = w, cv->vh = h;
    if (view_box(svg, vb)) {
        const char *par = attr(svg, "preserveaspectratio");
        double sx = w / vb[2], sy = h / vb[3];
        int none = par && strstr(par, "none"), slice = par && strstr(par, "slice");
        double s = slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
        double ox, oy;
        if (none) {
            ctm.a = sx, ctm.d = sy;
            ox = oy = 0;
        } else {
            ctm.a = ctm.d = s;
            double fx = par && strstr(par, "xMin") ? 0 : par && strstr(par, "xMax") ? 1 : 0.5;
            double fy = par && strstr(par, "YMin") ? 0 : par && strstr(par, "YMax") ? 1 : 0.5;
            ox = (w - vb[2] * s) * fx;
            oy = (h - vb[3] * s) * fy;
        }
        ctm.e = ox - vb[0] * ctm.a;
        ctm.f = oy - vb[1] * ctm.d;
        cv->vw = vb[2], cv->vh = vb[3];
    }
    struct gs g;
    memset(&g, 0, sizeof(g));
    g.fill.type = PT_COLOR;
    g.fill.rgb = 0xFF000000;
    g.fill_op = g.stroke_op = g.opacity = 1;
    g.stroke_w = 1;
    g.color = color ? color : 0xFF000000;
    g.font_size = 16;
    draw_node(cv, svg, ctm, g, 0);
    free(cv->mask);
    free(cv->acc);
    free(cv);
    return img;
}

/* ---- SVG as a picture file ---- */

int image_is_svg(const uint8_t *data, uint32_t len)
{
    uint32_t n = len < 1024 ? len : 1024;
    for (uint32_t i = 0; i + 4 <= n; i++)
        if (data[i] == '<' && !strncasecmp((const char *)data + i, "<svg", 4))
            return 1;
    return 0;
}

struct image *image_decode_svg(const uint8_t *data, uint32_t len)
{
    struct arena a = { 0 };
    struct node *root = html_parse(&a, (const char *)data, (int)len);
    struct image *img = NULL;
    if (root) {
        for (struct node *n = root; n; n = node_next(n, root))
            if (n->type == N_ELEMENT && n->tag == T_SVG) {
                int w, h;
                svg_size(n, &w, &h);
                while (w > MAX_PX || h > MAX_PX) {
                    w /= 2;
                    h /= 2;
                }
                img = svg_render(n, w < 1 ? 1 : w, h < 1 ? 1 : h, 0);
                break;
            }
    }
    arena_free(&a);
    return img;
}
