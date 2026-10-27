/*
 * The 2D drawing behind <canvas>. Paths are flattened into line segments as
 * they are built (in device space, as the specification has it); filling
 * walks each pixel row in eight sub-rows, adding up exact horizontal
 * coverage, so edges are anti-aliased and the non-zero and even-odd rules
 * both work. A stroke is turned into polygons (a quad for every segment, a
 * wedge or circle for every join and cap) that are filled non-zero together.
 * Colours are 0xAARRGGBB with straight alpha, as in struct image.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "canvas.h"
#include "web.h"

void *(*cv_gc_alloc)(size_t n, int atomic);

static void *gcz(size_t n, int atomic)
{
    return cv_gc_alloc ? cv_gc_alloc(n, atomic) : calloc(1, n);
}

#define SS 8                            /* sub-rows per pixel row */
#define FLATNESS 0.03f                  /* how far (pixels) a flattened curve may stray */
#define MAX_COORD 1.0e7f
#define MAX_POINTS (4u << 20)

static inline float hyp_f(float x, float y) { return sqrtf(x * x + y * y); }
static inline double hyp_d(double x, double y) { return sqrt(x * x + y * y); }
static inline float fminf2(float a, float b) { return a < b ? a : b; }
static inline float fmaxf2(float a, float b) { return a > b ? a : b; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- matrices ---- */

static const cv_mat IDENT = {1, 0, 0, 1, 0, 0};

static cv_mat mmul(cv_mat m, cv_mat n)  /* apply n first, then m */
{
    cv_mat r;
    r.a = m.a * n.a + m.c * n.b;
    r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;
    r.d = m.b * n.c + m.d * n.d;
    r.e = m.a * n.e + m.c * n.f + m.e;
    r.f = m.b * n.e + m.d * n.f + m.f;
    return r;
}

static int minv(const cv_mat *m, cv_mat *o)
{
    double det = m->a * m->d - m->b * m->c;
    if (!(fabs(det) > 1e-18) || isnan(det))
        return 0;
    double id = 1.0 / det;
    o->a = m->d * id;
    o->b = -m->b * id;
    o->c = -m->c * id;
    o->d = m->a * id;
    o->e = (m->c * m->f - m->d * m->e) * id;
    o->f = (m->b * m->e - m->a * m->f) * id;
    return 1;
}

static inline void mpt(const cv_mat *m, double x, double y, float *ox, float *oy)
{
    double X = m->a * x + m->c * y + m->e, Y = m->b * x + m->d * y + m->f;
    *ox = (float)(X > MAX_COORD ? MAX_COORD : X < -MAX_COORD ? -MAX_COORD : X);
    *oy = (float)(Y > MAX_COORD ? MAX_COORD : Y < -MAX_COORD ? -MAX_COORD : Y);
}

static double mscale(const cv_mat *m)
{
    double s1 = sqrt(m->a * m->a + m->b * m->b), s2 = sqrt(m->c * m->c + m->d * m->d);
    return s1 > s2 ? s1 : s2;
}

static int finite3(double a, double b, double c)
{
    return !isnan(a) && !isnan(b) && !isnan(c) && !isinf(a) && !isinf(b) && !isinf(c);
}

/* ---- paths ---- */

static int grow_pts(struct cv_path *p)
{
    if (p->npts < p->cap)
        return 1;
    if ((unsigned)p->cap >= MAX_POINTS)
        return 0;
    int cap = p->cap ? p->cap * 2 : 64;
    struct cv_pt *n = gcz((size_t)cap * sizeof(*n), 1);
    if (!n)
        return 0;
    if (p->pts)
        memcpy(n, p->pts, (size_t)p->npts * sizeof(*n));
    p->pts = n;
    p->cap = cap;
    return 1;
}

static int grow_subs(struct cv_path *p)
{
    if (p->nsubs < p->subcap)
        return 1;
    int cap = p->subcap ? p->subcap * 2 : 8;
    struct cv_sub *n = gcz((size_t)cap * sizeof(*n), 1);
    if (!n)
        return 0;
    if (p->subs)
        memcpy(n, p->subs, (size_t)p->nsubs * sizeof(*n));
    p->subs = n;
    p->subcap = cap;
    return 1;
}

void cv_path_clear(struct cv_path *p)
{
    p->npts = p->nsubs = 0;
    p->has_cur = p->open = 0;
}

static void p_move(struct cv_path *p, float x, float y)
{
    if (p->open && p->nsubs && p->subs[p->nsubs - 1].n == 1) {
        p->pts[p->npts - 1] = (struct cv_pt){x, y};
    } else {
        if (!grow_subs(p) || !grow_pts(p))
            return;
        p->subs[p->nsubs++] = (struct cv_sub){p->npts, 1, 0};
        p->pts[p->npts++] = (struct cv_pt){x, y};
    }
    p->sx = p->cx = x;
    p->sy = p->cy = y;
    p->has_cur = p->open = 1;
}

static void p_line(struct cv_path *p, float x, float y)
{
    if (!p->has_cur) {
        p_move(p, x, y);
        return;
    }
    if (!p->open)
        p_move(p, p->sx, p->sy);
    if (!grow_pts(p))
        return;
    p->pts[p->npts++] = (struct cv_pt){x, y};
    p->subs[p->nsubs - 1].n++;
    p->cx = x;
    p->cy = y;
}

void cv_path_close(struct cv_path *p)
{
    if (!p->has_cur || !p->nsubs)
        return;
    p->subs[p->nsubs - 1].closed = 1;
    p->open = 0;
    p->cx = p->sx;
    p->cy = p->sy;
}

void cv_path_move(struct cv_path *p, const cv_mat *m, double x, double y)
{
    if (!finite3(x, y, 0))
        return;
    float X, Y;
    mpt(m, x, y, &X, &Y);
    p_move(p, X, Y);
}

void cv_path_line(struct cv_path *p, const cv_mat *m, double x, double y)
{
    if (!finite3(x, y, 0))
        return;
    float X, Y;
    mpt(m, x, y, &X, &Y);
    p_line(p, X, Y);
}

void cv_path_quad(struct cv_path *p, const cv_mat *m, double x1, double y1, double x, double y)
{
    if (!finite3(x1, y1, 0) || !finite3(x, y, 0))
        return;
    float X1, Y1, X, Y;
    mpt(m, x1, y1, &X1, &Y1);
    mpt(m, x, y, &X, &Y);
    if (!p->has_cur)
        p_move(p, X1, Y1);
    float x0 = p->open ? p->cx : p->sx, y0 = p->open ? p->cy : p->sy;
    float dd = hyp_f(x0 - 2 * X1 + X, y0 - 2 * Y1 + Y);
    int n = (int)ceilf(sqrtf(0.25f * dd / FLATNESS));
    n = clampi(n, 1, 200);
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        p_line(p, u * u * x0 + 2 * u * t * X1 + t * t * X, u * u * y0 + 2 * u * t * Y1 + t * t * Y);
    }
}

void cv_path_bezier(struct cv_path *p, const cv_mat *m, double x1, double y1, double x2, double y2, double x, double y)
{
    if (!finite3(x1, y1, 0) || !finite3(x2, y2, 0) || !finite3(x, y, 0))
        return;
    float X1, Y1, X2, Y2, X, Y;
    mpt(m, x1, y1, &X1, &Y1);
    mpt(m, x2, y2, &X2, &Y2);
    mpt(m, x, y, &X, &Y);
    if (!p->has_cur)
        p_move(p, X1, Y1);
    float x0 = p->open ? p->cx : p->sx, y0 = p->open ? p->cy : p->sy;
    float d1 = hyp_f(x0 - 2 * X1 + X2, y0 - 2 * Y1 + Y2), d2 = hyp_f(X1 - 2 * X2 + X, Y1 - 2 * Y2 + Y);
    int n = (int)ceilf(sqrtf(0.75f * fmaxf2(d1, d2) / FLATNESS));
    n = clampi(n, 1, 200);
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        p_line(p, a * x0 + b * X1 + c * X2 + d * X, a * y0 + b * Y1 + c * Y2 + d * Y);
    }
}

/* Points of an arc of an ellipse, in user space, joined to the path. */
static void arc_points(struct cv_path *p, const cv_mat *m, double cx, double cy, double rx, double ry, double rot,
                       double a0, double a1, int ccw)
{
    const double two_pi = 2 * M_PI;
    double sweep;
    if (!ccw) {
        sweep = a1 - a0;
        if (sweep >= two_pi)
            sweep = two_pi;
        else {
            sweep = fmod(sweep, two_pi);
            if (sweep < 0)
                sweep += two_pi;
        }
    } else {
        sweep = a0 - a1;
        if (sweep >= two_pi)
            sweep = two_pi;
        else {
            sweep = fmod(sweep, two_pi);
            if (sweep < 0)
                sweep += two_pi;
        }
        sweep = -sweep;
    }
    double rdev = (rx > ry ? rx : ry) * mscale(m);
    double step = rdev > 2 * FLATNESS ? 2 * acos(1 - FLATNESS / rdev) : M_PI_2;
    if (!(step > 0.01))
        step = 0.01;
    int n = (int)ceil(fabs(sweep) / step);
    n = clampi(n, 1, 720);
    double cr = cos(rot), sr = sin(rot);
    for (int i = 0; i <= n; i++) {
        double ang = a0 + sweep * i / n;
        double ex = rx * cos(ang), ey = ry * sin(ang);
        double ux = cx + ex * cr - ey * sr, uy = cy + ex * sr + ey * cr;
        float X, Y;
        mpt(m, ux, uy, &X, &Y);
        if (i == 0 && !p->has_cur)
            p_move(p, X, Y);
        else
            p_line(p, X, Y);
    }
}

void cv_path_arc(struct cv_path *p, const cv_mat *m, double x, double y, double r, double a0, double a1, int ccw)
{
    if (!finite3(x, y, r) || !finite3(a0, a1, 0) || r < 0)
        return;
    arc_points(p, m, x, y, r, r, 0, a0, a1, ccw);
}

void cv_path_ellipse(struct cv_path *p, const cv_mat *m, double x, double y, double rx, double ry, double rot, double a0,
                     double a1, int ccw)
{
    if (!finite3(x, y, rx) || !finite3(ry, rot, 0) || !finite3(a0, a1, 0) || rx < 0 || ry < 0)
        return;
    arc_points(p, m, x, y, rx, ry, rot, a0, a1, ccw);
}

void cv_path_arcto(struct cv_path *p, const cv_mat *m, double x1, double y1, double x2, double y2, double r)
{
    if (!finite3(x1, y1, x2) || !finite3(y2, r, 0) || r < 0)
        return;
    if (!p->has_cur) {
        cv_path_move(p, m, x1, y1);
        return;
    }
    cv_mat inv;
    double x0, y0;
    float cx = p->open ? p->cx : p->sx, cy = p->open ? p->cy : p->sy;
    if (!minv(m, &inv)) {
        cv_path_line(p, m, x1, y1);
        return;
    }
    x0 = inv.a * cx + inv.c * cy + inv.e;
    y0 = inv.b * cx + inv.d * cy + inv.f;
    double v1x = x0 - x1, v1y = y0 - y1, v2x = x2 - x1, v2y = y2 - y1;
    double l1 = hyp_d(v1x, v1y), l2 = hyp_d(v2x, v2y);
    if (l1 < 1e-12 || l2 < 1e-12 || r == 0) {
        cv_path_line(p, m, x1, y1);
        return;
    }
    v1x /= l1, v1y /= l1, v2x /= l2, v2y /= l2;
    double cross = v1x * v2y - v1y * v2x, dot = v1x * v2x + v1y * v2y;
    if (fabs(cross) < 1e-9) {
        cv_path_line(p, m, x1, y1);
        return;
    }
    double theta = acos(dot < -1 ? -1 : dot > 1 ? 1 : dot);
    double dist = r / tan(theta / 2);
    double t1x = x1 + v1x * dist, t1y = y1 + v1y * dist, t2x = x1 + v2x * dist, t2y = y1 + v2y * dist;
    double bx = v1x + v2x, by = v1y + v2y, bl = hyp_d(bx, by);
    bx /= bl, by /= bl;
    double cd = r / sin(theta / 2);
    double ccx = x1 + bx * cd, ccy = y1 + by * cd;
    double a0 = atan2(t1y - ccy, t1x - ccx), a1 = atan2(t2y - ccy, t2x - ccx);
    double delta = a1 - a0;
    while (delta > M_PI)
        delta -= 2 * M_PI;
    while (delta <= -M_PI)
        delta += 2 * M_PI;
    cv_path_line(p, m, t1x, t1y);
    arc_points(p, m, ccx, ccy, r, r, 0, a0, a0 + delta, delta < 0);
}

void cv_path_rect(struct cv_path *p, const cv_mat *m, double x, double y, double w, double h)
{
    if (!finite3(x, y, w) || !finite3(h, 0, 0))
        return;
    cv_path_move(p, m, x, y);
    cv_path_line(p, m, x + w, y);
    cv_path_line(p, m, x + w, y + h);
    cv_path_line(p, m, x, y + h);
    cv_path_close(p);
}

void cv_path_round_rect(struct cv_path *p, const cv_mat *m, double x, double y, double w, double h, const double *r)
{
    if (!finite3(x, y, w) || !finite3(h, 0, 0))
        return;
    if (w < 0)
        x += w, w = -w;
    if (h < 0)
        y += h, h = -h;
    double rad[4];
    for (int i = 0; i < 4; i++)
        rad[i] = r[i] < 0 ? 0 : r[i];
    double scale = 1;
    double sides[4] = {rad[0] + rad[1], rad[1] + rad[2], rad[2] + rad[3], rad[3] + rad[0]};
    double lens[4] = {w, h, w, h};
    for (int i = 0; i < 4; i++)
        if (sides[i] > lens[i] && sides[i] > 0 && lens[i] / sides[i] < scale)
            scale = lens[i] / sides[i];
    for (int i = 0; i < 4; i++)
        rad[i] *= scale;
    cv_path_move(p, m, x + rad[0], y);
    cv_path_line(p, m, x + w - rad[1], y);
    if (rad[1] > 0)
        arc_points(p, m, x + w - rad[1], y + rad[1], rad[1], rad[1], 0, -M_PI_2, 0, 0);
    cv_path_line(p, m, x + w, y + h - rad[2]);
    if (rad[2] > 0)
        arc_points(p, m, x + w - rad[2], y + h - rad[2], rad[2], rad[2], 0, 0, M_PI_2, 0);
    cv_path_line(p, m, x + rad[3], y + h);
    if (rad[3] > 0)
        arc_points(p, m, x + rad[3], y + h - rad[3], rad[3], rad[3], 0, M_PI_2, M_PI, 0);
    cv_path_line(p, m, x, y + rad[0]);
    if (rad[0] > 0)
        arc_points(p, m, x + rad[0], y + rad[0], rad[0], rad[0], 0, M_PI, 3 * M_PI_2, 0);
    cv_path_close(p);
}

/* ---- the rasteriser ---- */

struct edge {
    float x0, y0, x1, y1, dxdy;
    int dir;
};

struct irect {
    int x0, y0, x1, y1;
};

static int edge_cmp(const void *a, const void *b)
{
    float ya = ((const struct edge *)a)->y0, yb = ((const struct edge *)b)->y0;
    return ya < yb ? -1 : ya > yb;
}

/* The edges of a path's subpaths (each closed), and their bounding box. */
static struct edge *path_edges(const struct cv_path *p, int *ne, struct irect *bb)
{
    int total = 0;
    for (int s = 0; s < p->nsubs; s++)
        if (p->subs[s].n >= 2)
            total += p->subs[s].n;
    struct edge *e = malloc((size_t)(total ? total : 1) * sizeof(*e));
    *ne = 0;
    bb->x0 = bb->y0 = 1 << 30;
    bb->x1 = bb->y1 = -(1 << 30);
    if (!e)
        return NULL;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int s = 0; s < p->nsubs; s++) {
        const struct cv_sub *sb = &p->subs[s];
        if (sb->n < 2)
            continue;
        const struct cv_pt *pt = p->pts + sb->start;
        for (int i = 0; i < sb->n; i++) {
            struct cv_pt a = pt[i], b = pt[i + 1 < sb->n ? i + 1 : 0];
            if (a.x < minx) minx = a.x;
            if (a.x > maxx) maxx = a.x;
            if (a.y < miny) miny = a.y;
            if (a.y > maxy) maxy = a.y;
            if (a.y == b.y)
                continue;
            struct edge *ed = &e[(*ne)++];
            if (a.y < b.y) {
                ed->x0 = a.x, ed->y0 = a.y, ed->x1 = b.x, ed->y1 = b.y, ed->dir = 1;
            } else {
                ed->x0 = b.x, ed->y0 = b.y, ed->x1 = a.x, ed->y1 = a.y, ed->dir = -1;
            }
            ed->dxdy = (ed->x1 - ed->x0) / (ed->y1 - ed->y0);
        }
    }
    if (minx <= maxx) {
        bb->x0 = (int)floorf(minx);
        bb->y0 = (int)floorf(miny);
        bb->x1 = (int)ceilf(maxx) + 1;
        bb->y1 = (int)ceilf(maxy) + 1;
    }
    return e;
}

typedef void (*row_fn)(void *u, int y, int x0, int x1, const uint8_t *cov);

static inline void add_span(float *acc, float *dif, int *minx, int *maxx, float xa, float xb, float w)
{
    if (xa < 0)
        xa = 0;
    if (xb > w)
        xb = w;
    if (xb <= xa)
        return;
    int ia = (int)xa, ib = (int)xb;
    if (ia < *minx)
        *minx = ia;
    if (ib > *maxx)
        *maxx = ib;
    if (ia == ib) {
        acc[ia] += xb - xa;
        return;
    }
    acc[ia] += (float)(ia + 1) - xa;
    dif[ia + 1] += 1;
    dif[ib] -= 1;
    acc[ib] += xb - (float)ib;
}

/* Scan convert. fn is called for each row with some coverage (every row of b if all_rows). */
static void raster(struct edge *e, int ne, int evenodd, struct irect b, row_fn fn, void *u, int all_rows)
{
    int W = b.x1 - b.x0, H = b.y1 - b.y0;
    if (W <= 0 || H <= 0)
        return;
    float *acc = calloc((size_t)W + 4, sizeof(float)), *dif = calloc((size_t)W + 4, sizeof(float));
    uint8_t *cov = calloc((size_t)W + 4, 1);
    int *act = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    float *cx = malloc((size_t)(ne ? ne : 1) * sizeof(float));
    int *cd = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    if (!acc || !dif || !cov || !act || !cx || !cd)
        goto done;
    if (ne > 1)
        qsort(e, (size_t)ne, sizeof(*e), edge_cmp);
    int ymin = b.y0, ymax = b.y1;
    if (!all_rows) {
        if (!ne)
            goto done;
        float lo = 1e30f, hi = -1e30f;
        for (int i = 0; i < ne; i++) {
            if (e[i].y0 < lo)
                lo = e[i].y0;
            if (e[i].y1 > hi)
                hi = e[i].y1;
        }
        int a = (int)floorf(lo), z = (int)ceilf(hi);
        if (a > ymin)
            ymin = a;
        if (z < ymax)
            ymax = z;
    }
    int nact = 0, next = 0;
    for (int y = ymin; y < ymax; y++) {
        while (next < ne && e[next].y0 < (float)(y + 1))
            act[nact++] = next++;
        int k = 0;
        for (int i = 0; i < nact; i++)
            if (e[act[i]].y1 > (float)y)
                act[k++] = act[i];
        nact = k;
        if (!nact) {
            if (all_rows) {
                memset(cov, 0, (size_t)W);
                fn(u, y, b.x0, b.x1, cov);
            }
            continue;
        }
        int minx = W + 1, maxx = -1;
        for (int sub = 0; sub < SS; sub++) {
            float sy = (float)y + ((float)sub + 0.5f) / SS;
            int nc = 0;
            for (int i = 0; i < nact; i++) {
                const struct edge *ed = &e[act[i]];
                if (ed->y0 > sy || sy >= ed->y1)
                    continue;
                float x = ed->x0 + (sy - ed->y0) * ed->dxdy - (float)b.x0;
                int j = nc++;
                while (j > 0 && cx[j - 1] > x) {
                    cx[j] = cx[j - 1];
                    cd[j] = cd[j - 1];
                    j--;
                }
                cx[j] = x;
                cd[j] = ed->dir;
            }
            int wind = 0;
            float start = 0;
            for (int i = 0; i < nc; i++) {
                int before = evenodd ? (wind & 1) : (wind != 0);
                wind += cd[i];
                int after = evenodd ? (wind & 1) : (wind != 0);
                if (!before && after)
                    start = cx[i];
                else if (before && !after)
                    add_span(acc, dif, &minx, &maxx, start, cx[i], (float)W);
            }
        }
        if (maxx < 0) {
            if (all_rows) {
                memset(cov, 0, (size_t)W);
                fn(u, y, b.x0, b.x1, cov);
            }
            continue;
        }
        int last = maxx < W - 1 ? maxx : W - 1;
        float run = 0;
        for (int x = minx; x <= last; x++) {
            run += dif[x];
            float v = (acc[x] + run) * (255.0f / SS) + 0.5f;
            cov[x - minx] = v >= 255 ? 255 : v <= 0 ? 0 : (uint8_t)v;
            acc[x] = dif[x] = 0;
        }
        for (int x = last + 1; x <= maxx + 1 && x < W + 4; x++)
            acc[x] = dif[x] = 0;
        if (all_rows) {
            /* the whole row, zero outside the shape */
            uint8_t *full = calloc((size_t)W, 1);
            if (full) {
                memcpy(full + minx, cov, (size_t)(last - minx + 1));
                fn(u, y, b.x0, b.x1, full);
                free(full);
            }
        } else if (last >= minx) {
            fn(u, y, b.x0 + minx, b.x0 + last + 1, cov);
        }
    }
done:
    free(acc);
    free(dif);
    free(cov);
    free(act);
    free(cx);
    free(cd);
}

/* ---- gradients and patterns ---- */

void cv_grad_stop(struct cv_grad *g, double off, uint32_t argb)
{
    if (g->nstops >= CV_MAX_STOPS)
        return;
    int i = g->nstops;
    while (i > 0 && g->st[i - 1].off > (float)off) {
        g->st[i] = g->st[i - 1];
        i--;
    }
    g->st[i].off = (float)off;
    g->st[i].argb = argb;
    g->nstops++;
    g->lut_ok = 0;
}

static void build_lut(struct cv_grad *g)
{
    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f;
        uint32_t c = 0;
        if (g->nstops) {
            if (t <= g->st[0].off)
                c = g->st[0].argb;
            else if (t >= g->st[g->nstops - 1].off)
                c = g->st[g->nstops - 1].argb;
            else {
                int j = 1;
                while (j < g->nstops && g->st[j].off < t)
                    j++;
                float o0 = g->st[j - 1].off, o1 = g->st[j].off;
                float f = o1 > o0 ? (t - o0) / (o1 - o0) : 1;
                uint32_t a = g->st[j - 1].argb, b = g->st[j].argb;
                int ch[4];
                for (int k = 0; k < 4; k++) {
                    int sh = 24 - 8 * k;
                    int va = (int)(a >> sh & 255), vb = (int)(b >> sh & 255);
                    ch[k] = (int)((float)va + (float)(vb - va) * f + 0.5f);
                }
                c = (uint32_t)ch[0] << 24 | (uint32_t)ch[1] << 16 | (uint32_t)ch[2] << 8 | (uint32_t)ch[3];
            }
        }
        g->lut[i] = c;
    }
    g->lut_ok = 1;
}

/* ---- paint: what a shape is filled with ---- */

enum { P_SOLID, P_LINEAR, P_RADIAL, P_CONIC, P_IMAGE };

struct paint {
    int kind;
    uint32_t color;
    const uint32_t *lut;
    float inv[6];                       /* device point -> the paint's own space */
    float x0, y0, dx, dy, inv_len2;     /* linear */
    float r0, dr, a_q;                  /* radial: a = dc.dc - dr^2 */
    float cdx, cdy;
    float start;                        /* conic: start angle */
    const struct image *img;
    int rx, ry;                         /* image: repeat in x, y */
    int clamp;                          /* image: clamp to the box below instead of repeating or fading */
    float bx0, by0, bx1, by1;
    int smooth;
};

static inline void paint_map(const struct paint *p, float x, float y, float *u, float *v)
{
    *u = p->inv[0] * x + p->inv[2] * y + p->inv[4];
    *v = p->inv[1] * x + p->inv[3] * y + p->inv[5];
}

static inline uint32_t texel(const struct paint *p, int x, int y)
{
    const struct image *im = p->img;
    if (p->clamp) {
        int x0 = (int)p->bx0, y0 = (int)p->by0, x1 = (int)p->bx1 - 1, y1 = (int)p->by1 - 1;
        x = clampi(x, x0, x1);
        y = clampi(y, y0, y1);
    } else {
        if (x < 0 || x >= im->w) {
            if (!p->rx)
                return 0;
            x %= im->w;
            if (x < 0)
                x += im->w;
        }
        if (y < 0 || y >= im->h) {
            if (!p->ry)
                return 0;
            y %= im->h;
            if (y < 0)
                y += im->h;
        }
    }
    return im->px[(size_t)y * im->w + x];
}

static uint32_t sample_image(const struct paint *p, float u, float v)
{
    if (!p->smooth) {
        return texel(p, (int)floorf(u), (int)floorf(v));
    }
    float fu = u - 0.5f, fv = v - 0.5f;
    int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
    int wx = (int)((fu - (float)x0) * 256), wy = (int)((fv - (float)y0) * 256);
    uint32_t c00 = texel(p, x0, y0), c10 = texel(p, x0 + 1, y0), c01 = texel(p, x0, y0 + 1),
             c11 = texel(p, x0 + 1, y0 + 1);
    int w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy), w01 = (256 - wx) * wy, w11 = wx * wy;
    unsigned a00 = c00 >> 24, a10 = c10 >> 24, a01 = c01 >> 24, a11 = c11 >> 24;
    /* weights sum to 65536; alpha-weighted so transparent texels do not bleed their colour */
    uint32_t wa00 = (uint32_t)w00 * a00, wa10 = (uint32_t)w10 * a10, wa01 = (uint32_t)w01 * a01,
             wa11 = (uint32_t)w11 * a11;
    uint32_t asum = wa00 + wa10 + wa01 + wa11;
    if (!asum)
        return 0;
    uint32_t out = ((asum + 32768) >> 16) << 24;
    for (int sh = 16; sh >= 0; sh -= 8) {
        uint32_t t = wa00 * (c00 >> sh & 255) + wa10 * (c10 >> sh & 255) + wa01 * (c01 >> sh & 255) +
                     wa11 * (c11 >> sh & 255);
        out |= ((t + asum / 2) / asum) << sh;
    }
    return out;
}

static uint32_t paint_at(const struct paint *p, int x, int y)
{
    float u, v;
    paint_map(p, (float)x + 0.5f, (float)y + 0.5f, &u, &v);
    switch (p->kind) {
    case P_LINEAR: {
        float t = ((u - p->x0) * p->dx + (v - p->y0) * p->dy) * p->inv_len2;
        int i = t <= 0 ? 0 : t >= 1 ? 255 : (int)(t * 255 + 0.5f);
        return p->lut[i];
    }
    case P_RADIAL: {
        float pdx = u - p->x0, pdy = v - p->y0;
        float a = p->a_q, b = pdx * p->cdx + pdy * p->cdy + p->r0 * p->dr, c = pdx * pdx + pdy * pdy - p->r0 * p->r0;
        float t;
        if (fabsf(a) < 1e-9f) {
            if (fabsf(b) < 1e-9f)
                return 0;
            t = c / (2 * b);
            if (p->r0 + t * p->dr < 0)
                return 0;
        } else {
            float disc = b * b - a * c;
            if (disc < 0)
                return 0;
            float sq = sqrtf(disc);
            float t1 = (b + sq) / a, t2 = (b - sq) / a;
            if (t1 < t2) {
                float tmp = t1;
                t1 = t2;
                t2 = tmp;
            }
            if (p->r0 + t1 * p->dr >= 0)
                t = t1;
            else if (p->r0 + t2 * p->dr >= 0)
                t = t2;
            else
                return 0;
        }
        int i = t <= 0 ? 0 : t >= 1 ? 255 : (int)(t * 255 + 0.5f);
        return p->lut[i];
    }
    case P_CONIC: {
        float ang = atan2f(v - p->y0, u - p->x0) - p->start;
        float t = ang / (2 * (float)M_PI);
        t -= floorf(t);
        return p->lut[(int)(t * 255 + 0.5f)];
    }
    case P_IMAGE:
        return sample_image(p, u, v);
    }
    return p->color;
}

static int setup_paint(struct paint *p, const struct cv_style *s, const cv_mat *ctm)
{
    memset(p, 0, sizeof(*p));
    cv_mat inv;
    if (s->kind == 0) {
        p->kind = P_SOLID;
        p->color = s->color;
        return 1;
    }
    if (!minv(ctm, &inv))
        return 0;
    p->inv[0] = (float)inv.a, p->inv[1] = (float)inv.b, p->inv[2] = (float)inv.c;
    p->inv[3] = (float)inv.d, p->inv[4] = (float)inv.e, p->inv[5] = (float)inv.f;
    if (s->kind == 1) {
        struct cv_grad *g = s->grad;
        if (!g->lut_ok)
            build_lut(g);
        p->lut = g->lut;
        if (g->kind == 0) {
            p->kind = P_LINEAR;
            p->x0 = (float)g->x0, p->y0 = (float)g->y0;
            p->dx = (float)(g->x1 - g->x0), p->dy = (float)(g->y1 - g->y0);
            float l2 = p->dx * p->dx + p->dy * p->dy;
            if (l2 < 1e-12f) {
                p->kind = P_SOLID;       /* a gradient with no extent paints its last stop */
                p->color = g->nstops ? g->st[g->nstops - 1].argb : 0;
                return 1;
            }
            p->inv_len2 = 1 / l2;
        } else if (g->kind == 1) {
            p->kind = P_RADIAL;
            p->x0 = (float)g->x0, p->y0 = (float)g->y0, p->r0 = (float)g->r0;
            p->cdx = (float)(g->x1 - g->x0), p->cdy = (float)(g->y1 - g->y0);
            p->dr = (float)(g->r1 - g->r0);
            p->a_q = p->cdx * p->cdx + p->cdy * p->cdy - p->dr * p->dr;
        } else {
            p->kind = P_CONIC;
            p->x0 = (float)g->x0, p->y0 = (float)g->y0;
            p->start = (float)g->r0;
        }
        return 1;
    }
    p->kind = P_IMAGE;
    p->img = &s->pat->img;
    p->rx = s->pat->repeat & 1;
    p->ry = s->pat->repeat & 2;
    p->smooth = 1;
    return p->img->w > 0 && p->img->h > 0;
}

/* ---- compositing ---- */

static inline int div255(int v) { return (v + 127) / 255; }

/* source-over with the colour of s at alpha sa (0-255) */
static inline void over_px(uint32_t *d, uint32_t s, int sa)
{
    if (sa <= 0)
        return;
    if (sa >= 255) {
        *d = 0xFF000000u | (s & 0xFFFFFF);
        return;
    }
    uint32_t dv = *d;
    int da = (int)(dv >> 24);
    if (da == 0) {
        *d = (uint32_t)sa << 24 | (s & 0xFFFFFF);
        return;
    }
    int sr = (int)(s >> 16 & 255), sg = (int)(s >> 8 & 255), sb = (int)(s & 255);
    int dr = (int)(dv >> 16 & 255), dg = (int)(dv >> 8 & 255), db = (int)(dv & 255);
    if (da == 255) {
        dr += (sr - dr) * sa / 255 + ((sr - dr) * sa % 255 > 127 ? 1 : (sr - dr) * sa % 255 < -127 ? -1 : 0);
        dg += (sg - dg) * sa / 255 + ((sg - dg) * sa % 255 > 127 ? 1 : (sg - dg) * sa % 255 < -127 ? -1 : 0);
        db += (sb - db) * sa / 255 + ((sb - db) * sa % 255 > 127 ? 1 : (sb - db) * sa % 255 < -127 ? -1 : 0);
        *d = 0xFF000000u | (uint32_t)dr << 16 | (uint32_t)dg << 8 | (uint32_t)db;
        return;
    }
    int wd = div255(da * (255 - sa));
    int oa = sa + wd;
    if (oa <= 0)
        return;
    int r = (sr * sa + dr * wd + oa / 2) / oa, g = (sg * sa + dg * wd + oa / 2) / oa,
        b = (sb * sa + db * wd + oa / 2) / oa;
    *d = (uint32_t)oa << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static float blend_fn(int op, float cb, float cs)
{
    switch (op) {
    case CV_MULTIPLY: return cb * cs;
    case CV_SCREEN: return cb + cs - cb * cs;
    case CV_OVERLAY: return blend_fn(CV_HARDLIGHT, cs, cb);
    case CV_DARKEN: return cb < cs ? cb : cs;
    case CV_LIGHTEN: return cb > cs ? cb : cs;
    case CV_DODGE:
        if (cb <= 0)
            return 0;
        if (cs >= 1)
            return 1;
        return fminf2(1, cb / (1 - cs));
    case CV_BURN:
        if (cb >= 1)
            return 1;
        if (cs <= 0)
            return 0;
        return 1 - fminf2(1, (1 - cb) / cs);
    case CV_HARDLIGHT: return cs <= 0.5f ? cb * 2 * cs : blend_fn(CV_SCREEN, cb, 2 * cs - 1);
    case CV_SOFTLIGHT: {
        if (cs <= 0.5f)
            return cb - (1 - 2 * cs) * cb * (1 - cb);
        float dd = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : sqrtf(cb);
        return cb + (2 * cs - 1) * (dd - cb);
    }
    case CV_DIFFERENCE: return fabsf(cb - cs);
    case CV_EXCLUSION: return cb + cs - 2 * cb * cs;
    }
    return cs;
}

/* Any operator: d under s (s's alpha is what is being drawn). */
static uint32_t composite(uint32_t d, uint32_t s, int op)
{
    float sa = (float)(s >> 24) / 255, da = (float)(d >> 24) / 255;
    float sc[3] = {(float)(s >> 16 & 255) / 255, (float)(s >> 8 & 255) / 255, (float)(s & 255) / 255};
    float dc[3] = {(float)(d >> 16 & 255) / 255, (float)(d >> 8 & 255) / 255, (float)(d & 255) / 255};
    float ao, co[3];
    if (op >= CV_MULTIPLY) {
        ao = sa + da - sa * da;
        for (int i = 0; i < 3; i++)
            co[i] = sa * (1 - da) * sc[i] + da * (1 - sa) * dc[i] + sa * da * blend_fn(op, dc[i], sc[i]);
    } else {
        float fs = 1, fd = 0;
        switch (op) {
        case CV_OVER: fs = 1, fd = 1 - sa; break;
        case CV_IN: fs = da, fd = 0; break;
        case CV_OUT: fs = 1 - da, fd = 0; break;
        case CV_ATOP: fs = da, fd = 1 - sa; break;
        case CV_DOVER: fs = 1 - da, fd = 1; break;
        case CV_DIN: fs = 0, fd = sa; break;
        case CV_DOUT: fs = 0, fd = 1 - sa; break;
        case CV_DATOP: fs = 1 - da, fd = sa; break;
        case CV_LIGHTER: fs = 1, fd = 1; break;
        case CV_COPY: fs = 1, fd = 0; break;
        case CV_XOR: fs = 1 - da, fd = 1 - sa; break;
        }
        ao = sa * fs + da * fd;
        for (int i = 0; i < 3; i++)
            co[i] = sa * sc[i] * fs + da * dc[i] * fd;
        if (ao > 1)
            ao = 1;
    }
    if (ao <= 0.0001f)
        return 0;
    uint32_t out = (uint32_t)(ao * 255 + 0.5f) << 24;
    for (int i = 0; i < 3; i++) {
        float v = co[i] / ao;
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        out |= (uint32_t)(v * 255 + 0.5f) << (16 - 8 * i);
    }
    return out;
}

static inline int op_is_global(int op)
{
    return op == CV_IN || op == CV_OUT || op == CV_DIN || op == CV_DATOP || op == CV_COPY;
}

struct bsink {
    struct image *im;
    struct paint paint;
    const uint8_t *clip;
    int alpha;                          /* 0-256 */
    int op;
};

static void blend_row(void *u, int y, int x0, int x1, const uint8_t *cov)
{
    struct bsink *b = u;
    struct image *im = b->im;
    uint32_t *row = im->px + (size_t)y * im->w;
    const uint8_t *clip = b->clip ? b->clip + (size_t)y * im->w : NULL;
    int solid = b->paint.kind == P_SOLID;
    uint32_t color = b->paint.color;
    if (b->op == CV_OVER) {
        for (int x = x0; x < x1; x++) {
            int c = cov[x - x0];
            if (!c)
                continue;
            if (clip) {
                c = c * clip[x] / 255;
                if (!c)
                    continue;
            }
            uint32_t src = solid ? color : paint_at(&b->paint, x, y);
            int sa = (int)(src >> 24);
            if (!sa)
                continue;
            sa = (sa * b->alpha + 128) >> 8;
            if (c != 255)
                sa = div255(sa * c);
            over_px(&row[x], src, sa);
        }
        return;
    }
    for (int x = x0; x < x1; x++) {
        int c = cov[x - x0];
        if (!c && !op_is_global(b->op))
            continue;
        int cl = clip ? clip[x] : 255;
        if (!cl)
            continue;
        uint32_t src = c ? (solid ? color : paint_at(&b->paint, x, y)) : 0;
        int sa = ((int)(src >> 24) * b->alpha + 128) >> 8;
        sa = div255(sa * c);
        uint32_t s2 = (src & 0xFFFFFF) | (uint32_t)sa << 24;
        uint32_t res = composite(row[x], s2, b->op);
        if (cl != 255) {
            uint32_t old = row[x], mix = 0;
            for (int sh = 24; sh >= 0; sh -= 8) {
                int a = (int)(old >> sh & 255), z = (int)(res >> sh & 255);
                mix |= (uint32_t)(a + (z - a) * cl / 255) << sh;
            }
            res = mix;
        }
        row[x] = res;
    }
}

/* ---- the context ---- */

static void state_default(struct cv_state *s)
{
    memset(s, 0, sizeof(*s));
    s->m = IDENT;
    s->fill.color = 0xFF000000u;
    s->stroke.color = 0xFF000000u;
    s->alpha = 1;
    s->op = CV_OVER;
    s->line_width = 1;
    s->miter = 10;
    s->smooth = 1;
    s->family = FAM_SANS;
    s->px = 10;
    snprintf(s->font, sizeof(s->font), "10px sans-serif");
    s->align = CV_ALIGN_START;
    s->baseline = CV_ALPHABETIC;
}

struct cv *cv_new(void)
{
    struct cv *c = gcz(sizeof(*c), 0);
    if (c)
        state_default(&c->st);
    return c;
}

void cv_reset(struct cv *c)
{
    state_default(&c->st);
    c->depth = 0;
    cv_path_clear(&c->path);
}

void cv_save(struct cv *c)
{
    if (c->depth >= CV_MAX_STACK)
        return;
    struct cv_state *s = gcz(sizeof(*s), 0);
    if (!s)
        return;
    *s = c->st;
    c->stack[c->depth++] = s;
}

void cv_restore(struct cv *c)
{
    if (c->depth <= 0)
        return;
    c->st = *c->stack[--c->depth];
    c->stack[c->depth] = NULL;
}

void cv_scale(struct cv *c, double x, double y)
{
    if (!finite3(x, y, 0))
        return;
    cv_mat m = {x, 0, 0, y, 0, 0};
    c->st.m = mmul(c->st.m, m);
}

void cv_rotate(struct cv *c, double a)
{
    if (!finite3(a, 0, 0))
        return;
    cv_mat m = {cos(a), sin(a), -sin(a), cos(a), 0, 0};
    c->st.m = mmul(c->st.m, m);
}

void cv_translate(struct cv *c, double x, double y)
{
    if (!finite3(x, y, 0))
        return;
    cv_mat m = {1, 0, 0, 1, x, y};
    c->st.m = mmul(c->st.m, m);
}

void cv_transform(struct cv *c, double a, double b, double cc, double d, double e, double f)
{
    if (!finite3(a, b, cc) || !finite3(d, e, f))
        return;
    cv_mat m = {a, b, cc, d, e, f};
    c->st.m = mmul(c->st.m, m);
}

void cv_set_transform(struct cv *c, double a, double b, double cc, double d, double e, double f)
{
    if (!finite3(a, b, cc) || !finite3(d, e, f))
        return;
    cv_mat m = {a, b, cc, d, e, f};
    c->st.m = m;
}

static struct irect canvas_rect(const struct cv *c)
{
    return (struct irect){0, 0, c->img->w, c->img->h};
}

/* ---- shadows ---- */

static int shadow_on(const struct cv_state *s)
{
    return (s->shadow_color >> 24) && (s->shadow_blur > 0 || s->shadow_dx != 0 || s->shadow_dy != 0);
}

struct msink {
    uint8_t *mask;
    int ox, oy, w, h;
};

static void mask_row(void *u, int y, int x0, int x1, const uint8_t *cov)
{
    struct msink *m = u;
    if (y < m->oy || y >= m->oy + m->h)
        return;
    uint8_t *row = m->mask + (size_t)(y - m->oy) * m->w;
    for (int x = x0; x < x1; x++) {
        int v = row[x - m->ox] + cov[x - x0];
        row[x - m->ox] = (uint8_t)(v > 255 ? 255 : v);
    }
}

static void box_blur(uint8_t *mask, int w, int h, int r)
{
    if (r < 1)
        return;
    uint8_t *tmp = malloc((size_t)w * h);
    if (!tmp)
        return;
    int span = 2 * r + 1;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) {      /* horizontal */
            const uint8_t *s = mask + (size_t)y * w;
            uint8_t *d = tmp + (size_t)y * w;
            int sum = 0;
            for (int x = -r; x <= r; x++)
                sum += x >= 0 && x < w ? s[x] : 0;
            for (int x = 0; x < w; x++) {
                d[x] = (uint8_t)(sum / span);
                int out = x - r, in = x + r + 1;
                sum += (in < w ? s[in] : 0) - (out >= 0 ? s[out] : 0);
            }
        }
        for (int x = 0; x < w; x++) {      /* vertical */
            int sum = 0;
            for (int y = -r; y <= r; y++)
                sum += y >= 0 && y < h ? tmp[(size_t)y * w + x] : 0;
            for (int y = 0; y < h; y++) {
                mask[(size_t)y * w + x] = (uint8_t)(sum / span);
                int out = y - r, in = y + r + 1;
                sum += (in < h ? tmp[(size_t)in * w + x] : 0) - (out >= 0 ? tmp[(size_t)out * w + x] : 0);
            }
        }
    }
    free(tmp);
}

/* The shadow of a shape: its coverage, blurred, offset, in the shadow's colour. */
static void draw_shadow(struct cv *c, struct edge *e, int ne, int evenodd, struct irect bb)
{
    const struct cv_state *s = &c->st;
    int margin = (int)(s->shadow_blur) + 2;
    struct irect cr = canvas_rect(c);
    /* only the part that lands on the canvas matters */
    int ox = bb.x0 - margin, oy = bb.y0 - margin, ex = bb.x1 + margin, ey = bb.y1 + margin;
    int lx = cr.x0 - (int)floorf(s->shadow_dx) - margin, ly = cr.y0 - (int)floorf(s->shadow_dy) - margin;
    int hx = cr.x1 - (int)floorf(s->shadow_dx) + margin, hy = cr.y1 - (int)floorf(s->shadow_dy) + margin;
    if (ox < lx) ox = lx;
    if (oy < ly) oy = ly;
    if (ex > hx) ex = hx;
    if (ey > hy) ey = hy;
    int w = ex - ox, h = ey - oy;
    if (w <= 0 || h <= 0 || (long)w * h > 16L * 1024 * 1024)
        return;
    uint8_t *mask = calloc((size_t)w, (size_t)h);
    if (!mask)
        return;
    struct msink ms = {mask, ox, oy, w, h};
    raster(e, ne, evenodd, (struct irect){ox, oy, ex, ey}, mask_row, &ms, 0);
    float sigma = s->shadow_blur / 2;
    if (sigma > 0.3f)
        box_blur(mask, w, h, (int)(sigma * 1.5f + 0.5f) > 0 ? (int)(sigma * 1.5f + 0.5f) : 1);
    int dx = (int)floorf(s->shadow_dx + 0.5f), dy = (int)floorf(s->shadow_dy + 0.5f);
    uint32_t col = s->shadow_color;
    int calpha = ((int)(col >> 24) * (int)(s->alpha * 256 + 0.5f) + 128) >> 8;
    for (int y = 0; y < h; y++) {
        int Y = oy + y + dy;
        if (Y < 0 || Y >= c->img->h)
            continue;
        uint32_t *row = c->img->px + (size_t)Y * c->img->w;
        for (int x = 0; x < w; x++) {
            int X = ox + x + dx;
            int m = mask[(size_t)y * w + x];
            if (!m || X < 0 || X >= c->img->w)
                continue;
            if (s->clip)
                m = m * s->clip[(size_t)Y * c->img->w + X] / 255;
            over_px(&row[X], col, div255(calpha * m));
        }
    }
    free(mask);
}

/* ---- filling ---- */

static void fill_edges(struct cv *c, struct edge *e, int ne, int evenodd, const struct cv_style *style,
                       struct irect bb)
{
    if (!c->img || !ne)
        return;
    const struct cv_state *s = &c->st;
    struct bsink bs;
    memset(&bs, 0, sizeof(bs));
    if (!setup_paint(&bs.paint, style, &s->m))
        return;
    bs.im = c->img;
    bs.clip = s->clip;
    bs.alpha = (int)(s->alpha * 256 + 0.5f);
    bs.op = s->op;
    if (bs.alpha <= 0 && !op_is_global(bs.op))
        return;
    if (shadow_on(s))
        draw_shadow(c, e, ne, evenodd, bb);
    raster(e, ne, evenodd, canvas_rect(c), blend_row, &bs, op_is_global(bs.op));
}

void cv_fill(struct cv *c, struct cv_path *p, int evenodd)
{
    if (!c->img)
        return;
    if (!p)
        p = &c->path;
    int ne;
    struct irect bb;
    struct edge *e = path_edges(p, &ne, &bb);
    if (e) {
        fill_edges(c, e, ne, evenodd, &c->st.fill, bb);
        free(e);
    }
}

static void rect_path(struct cv *c, struct cv_path *tmp, double x, double y, double w, double h)
{
    memset(tmp, 0, sizeof(*tmp));
    cv_path_rect(tmp, &c->st.m, x, y, w, h);
}

void cv_fill_rect(struct cv *c, double x, double y, double w, double h)
{
    if (!c->img || !finite3(x, y, w) || !finite3(h, 0, 0) || w == 0 || h == 0)
        return;
    struct cv_path tmp;
    rect_path(c, &tmp, x, y, w, h);
    cv_fill(c, &tmp, 0);
}

struct csink {
    struct image *im;
    const uint8_t *clip;
};

static void clear_row(void *u, int y, int x0, int x1, const uint8_t *cov)
{
    struct csink *k = u;
    uint32_t *row = k->im->px + (size_t)y * k->im->w;
    for (int x = x0; x < x1; x++) {
        int a = cov[x - x0];
        if (k->clip)
            a = a * k->clip[(size_t)y * k->im->w + x] / 255;
        if (!a)
            continue;
        uint32_t d = row[x];
        int da = (int)(d >> 24) * (255 - a) / 255;
        row[x] = da ? (uint32_t)da << 24 | (d & 0xFFFFFF) : 0;
    }
}

void cv_clear_rect(struct cv *c, double x, double y, double w, double h)
{
    if (!c->img || !finite3(x, y, w) || !finite3(h, 0, 0) || w == 0 || h == 0)
        return;
    struct cv_path tmp;
    rect_path(c, &tmp, x, y, w, h);
    int ne;
    struct irect bb;
    struct edge *e = path_edges(&tmp, &ne, &bb);
    if (!e)
        return;
    struct csink k = {c->img, c->st.clip};
    raster(e, ne, 0, canvas_rect(c), clear_row, &k, 0);
    free(e);
}

/* ---- clipping ---- */

struct clipsink {
    uint8_t *mask;
    int w;
};

static void clip_store(void *u, int y, int x0, int x1, const uint8_t *cov)
{
    struct clipsink *k = u;
    memcpy(k->mask + (size_t)y * k->w + x0, cov, (size_t)(x1 - x0));
}

void cv_clip(struct cv *c, struct cv_path *p, int evenodd)
{
    if (!c->img)
        return;
    if (!p)
        p = &c->path;
    int w = c->img->w, h = c->img->h;
    uint8_t *mask = gcz((size_t)w * h, 1);
    if (!mask)
        return;
    int ne;
    struct irect bb;
    struct edge *e = path_edges(p, &ne, &bb);
    if (!e)
        return;
    struct clipsink k = {mask, w};
    raster(e, ne, evenodd, canvas_rect(c), clip_store, &k, 0);
    free(e);
    if (c->st.clip)
        for (size_t i = 0, n = (size_t)w * h; i < n; i++)
            mask[i] = (uint8_t)(mask[i] * c->st.clip[i] / 255);
    c->st.clip = mask;
}

/* ---- point tests ---- */

static int winding_at(const struct cv_path *p, float x, float y, int evenodd)
{
    int wind = 0;
    for (int s = 0; s < p->nsubs; s++) {
        const struct cv_sub *sb = &p->subs[s];
        if (sb->n < 2)
            continue;
        const struct cv_pt *pt = p->pts + sb->start;
        for (int i = 0; i < sb->n; i++) {
            struct cv_pt a = pt[i], b = pt[i + 1 < sb->n ? i + 1 : 0];
            if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
                float t = (y - a.y) / (b.y - a.y);
                float xi = a.x + t * (b.x - a.x);
                if (xi > x)
                    wind += b.y > a.y ? 1 : -1;
            }
        }
    }
    return evenodd ? (wind & 1) : wind != 0;
}

int cv_point_in_path(struct cv *c, struct cv_path *p, double x, double y, int evenodd)
{
    if (!p)
        p = &c->path;
    if (!finite3(x, y, 0))
        return 0;
    return winding_at(p, (float)x, (float)y, evenodd);
}

/* ---- stroking ---- */

struct dpt {
    double x, y;
};

struct stroker {
    const cv_mat *m;
    struct cv_path *out;
    double hw;
    int cap, join;
    double miter;
    int circle_n;
};

static void poly_begin(struct stroker *k)
{
    struct cv_path *o = k->out;
    if (!grow_subs(o))
        return;
    o->subs[o->nsubs++] = (struct cv_sub){o->npts, 0, 1};
}

static void poly_pt(struct stroker *k, double x, double y)
{
    struct cv_path *o = k->out;
    if (!grow_pts(o) || !o->nsubs)
        return;
    float X, Y;
    mpt(k->m, x, y, &X, &Y);
    o->pts[o->npts++] = (struct cv_pt){X, Y};
    o->subs[o->nsubs - 1].n++;
}

static void poly_end(struct stroker *k)
{
    struct cv_path *o = k->out;
    if (!o->nsubs)
        return;
    struct cv_sub *sb = &o->subs[o->nsubs - 1];
    if (sb->n < 3) {
        o->npts = sb->start;
        o->nsubs--;
        return;
    }
    struct cv_pt *p = o->pts + sb->start;
    double area = 0;
    for (int i = 0; i < sb->n; i++) {
        struct cv_pt a = p[i], b = p[(i + 1) % sb->n];
        area += (double)a.x * b.y - (double)b.x * a.y;
    }
    if (area < 0)
        for (int i = 0, j = sb->n - 1; i < j; i++, j--) {
            struct cv_pt t = p[i];
            p[i] = p[j];
            p[j] = t;
        }
}

static void add_circle(struct stroker *k, double x, double y)
{
    poly_begin(k);
    for (int i = 0; i < k->circle_n; i++) {
        double a = 2 * M_PI * i / k->circle_n;
        poly_pt(k, x + k->hw * cos(a), y + k->hw * sin(a));
    }
    poly_end(k);
}

static void add_quad(struct stroker *k, double ax, double ay, double bx, double by, double cx, double cy, double dx,
                     double dy)
{
    poly_begin(k);
    poly_pt(k, ax, ay);
    poly_pt(k, bx, by);
    poly_pt(k, cx, cy);
    poly_pt(k, dx, dy);
    poly_end(k);
}

/* The join at v where a segment with direction (d0x, d0y) is followed by one with (d1x, d1y). */
static void add_join(struct stroker *k, double vx, double vy, double d0x, double d0y, double d1x, double d1y)
{
    double cross = d0x * d1y - d0y * d1x, dot = d0x * d1x + d0y * d1y;
    if (fabs(cross) < 1e-9 && dot > 0)
        return;
    if (k->join == CV_ROUND_JOIN) {
        add_circle(k, vx, vy);
        return;
    }
    double o = cross > 0 ? -1 : 1;
    double n0x = -d0y * k->hw * o, n0y = d0x * k->hw * o, n1x = -d1y * k->hw * o, n1y = d1x * k->hw * o;
    if (k->join == CV_MITER) {
        double c2 = 1 + dot;               /* 2 cos^2(half the turning angle) */
        if (c2 > 1e-9) {
            double ratio = 1 / sqrt(c2 / 2);
            if (ratio <= k->miter) {
                double tx = vx + (n0x + n1x) / c2, ty = vy + (n0y + n1y) / c2;
                poly_begin(k);
                poly_pt(k, vx, vy);
                poly_pt(k, vx + n0x, vy + n0y);
                poly_pt(k, tx, ty);
                poly_pt(k, vx + n1x, vy + n1y);
                poly_end(k);
                return;
            }
        }
    }
    poly_begin(k);
    poly_pt(k, vx, vy);
    poly_pt(k, vx + n0x, vy + n0y);
    poly_pt(k, vx + n1x, vy + n1y);
    poly_end(k);
}

static void add_cap(struct stroker *k, double px, double py, double dx, double dy)
{
    /* (dx, dy): the direction pointing away from the line, out of this end */
    if (k->cap == CV_ROUND_CAP) {
        add_circle(k, px, py);
    } else if (k->cap == CV_SQUARE) {
        double nx = -dy * k->hw, ny = dx * k->hw, ex = dx * k->hw, ey = dy * k->hw;
        add_quad(k, px + nx, py + ny, px + nx + ex, py + ny + ey, px - nx + ex, py - ny + ey, px - nx, py - ny);
    }
}

static void stroke_polyline(struct stroker *k, struct dpt *p, int n, int closed)
{
    /* drop repeated points */
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || fabs(p[i].x - p[m - 1].x) > 1e-9 || fabs(p[i].y - p[m - 1].y) > 1e-9)
            p[m++] = p[i];
    n = m;
    if (closed && n > 1 && fabs(p[0].x - p[n - 1].x) < 1e-9 && fabs(p[0].y - p[n - 1].y) < 1e-9)
        n--;
    if (n == 0)
        return;
    if (n == 1) {
        if (k->cap == CV_ROUND_CAP)
            add_circle(k, p[0].x, p[0].y);
        else if (k->cap == CV_SQUARE)
            add_quad(k, p[0].x - k->hw, p[0].y - k->hw, p[0].x + k->hw, p[0].y - k->hw, p[0].x + k->hw,
                     p[0].y + k->hw, p[0].x - k->hw, p[0].y + k->hw);
        return;
    }
    int ns = closed ? n : n - 1;
    double *dx = malloc(sizeof(double) * 2 * (size_t)ns);
    if (!dx)
        return;
    double *dy = dx + ns;
    for (int i = 0; i < ns; i++) {
        struct dpt a = p[i], b = p[(i + 1) % n];
        double l = hyp_d(b.x - a.x, b.y - a.y);
        dx[i] = (b.x - a.x) / l;
        dy[i] = (b.y - a.y) / l;
        double nx = -dy[i] * k->hw, ny = dx[i] * k->hw;
        add_quad(k, a.x + nx, a.y + ny, b.x + nx, b.y + ny, b.x - nx, b.y - ny, a.x - nx, a.y - ny);
    }
    for (int i = closed ? 0 : 1; i < (closed ? ns : ns); i++) {
        int prev = (i + ns - 1) % ns;
        if (!closed && i == 0)
            continue;
        add_join(k, p[i].x, p[i].y, dx[prev], dy[prev], dx[i], dy[i]);
    }
    if (!closed) {
        add_cap(k, p[0].x, p[0].y, -dx[0], -dy[0]);
        add_cap(k, p[n - 1].x, p[n - 1].y, dx[ns - 1], dy[ns - 1]);
    }
    free(dx);
}

/* The stroke of a path (device space in, device space out) as polygons to fill non-zero. */
static void stroke_path(const struct cv_path *dev, const struct cv_state *st, struct cv_path *out)
{
    cv_mat inv;
    if (!minv(&st->m, &inv) || st->line_width <= 0)
        return;
    struct stroker k;
    k.m = &st->m;
    k.out = out;
    k.hw = st->line_width / 2;
    k.cap = st->cap;
    k.join = st->join;
    k.miter = st->miter;
    double rdev = k.hw * mscale(&st->m);
    k.circle_n = rdev > 0.15 ? (int)ceil(M_PI / acos(1 - 0.1 / (rdev > 0.2 ? rdev : 0.2))) : 8;
    k.circle_n = clampi(k.circle_n, 8, 90);
    cv_path_clear(out);
    int maxn = 0;
    for (int s = 0; s < dev->nsubs; s++)
        if (dev->subs[s].n > maxn)
            maxn = dev->subs[s].n;
    struct dpt *u = malloc(sizeof(*u) * (size_t)(maxn + 1));
    if (!u)
        return;
    int dashing = st->ndash > 0;
    double total = 0;
    for (int i = 0; i < st->ndash; i++)
        total += st->dash[i];
    if (!(total > 0))
        dashing = 0;
    struct dpt *piece = dashing ? malloc(sizeof(*piece) * (size_t)(maxn + 2) * 4) : NULL;
    for (int s = 0; s < dev->nsubs; s++) {
        const struct cv_sub *sb = &dev->subs[s];
        if (sb->n < 1)
            continue;
        for (int i = 0; i < sb->n; i++) {
            const struct cv_pt *pt = &dev->pts[sb->start + i];
            u[i].x = inv.a * pt->x + inv.c * pt->y + inv.e;
            u[i].y = inv.b * pt->x + inv.d * pt->y + inv.f;
        }
        int n = sb->n, closed = sb->closed;
        if (closed && n > 1) {
            u[n] = u[0];
            n++;
            closed = 1;
        }
        if (!dashing || !piece) {
            if (closed) {
                n--;
                stroke_polyline(&k, u, n, 1);
            } else {
                stroke_polyline(&k, u, n, 0);
            }
            continue;
        }
        /* dashes along the polyline */
        int cnt = st->ndash, idx = 0;
        double phase = fmod(st->dash_off, total);
        if (phase < 0)
            phase += total;
        while (phase >= st->dash[idx]) {
            phase -= st->dash[idx];
            idx = (idx + 1) % cnt;
        }
        double left = st->dash[idx] - phase;
        int on = idx % 2 == 0, np = 0;
        if (on)
            piece[np++] = u[0];
        for (int i = 0; i + 1 < n; i++) {
            double ax = u[i].x, ay = u[i].y, bx = u[i + 1].x, by = u[i + 1].y;
            double len = hyp_d(bx - ax, by - ay), pos = 0;
            if (len <= 0)
                continue;
            while (len - pos > left) {
                pos += left;
                struct dpt q = {ax + (bx - ax) * pos / len, ay + (by - ay) * pos / len};
                if (on) {
                    piece[np++] = q;
                    stroke_polyline(&k, piece, np, 0);
                    np = 0;
                } else {
                    piece[0] = q;
                    np = 1;
                }
                on = !on;
                idx = (idx + 1) % cnt;
                left = st->dash[idx];
            }
            left -= len - pos;
            if (on) {
                if (np >= (maxn + 2) * 4 - 1) {
                    stroke_polyline(&k, piece, np, 0);
                    piece[0] = piece[np - 1];
                    np = 1;
                }
                piece[np++] = u[i + 1];
            }
        }
        if (on && np > 1)
            stroke_polyline(&k, piece, np, 0);
    }
    free(piece);
    free(u);
}

void cv_stroke(struct cv *c, struct cv_path *p)
{
    if (!c->img)
        return;
    if (!p)
        p = &c->path;
    struct cv_path out;
    memset(&out, 0, sizeof(out));
    stroke_path(p, &c->st, &out);
    int ne;
    struct irect bb;
    struct edge *e = path_edges(&out, &ne, &bb);
    if (e) {
        fill_edges(c, e, ne, 0, &c->st.stroke, bb);
        free(e);
    }
}

void cv_stroke_rect(struct cv *c, double x, double y, double w, double h)
{
    if (!c->img || !finite3(x, y, w) || !finite3(h, 0, 0) || (w == 0 && h == 0))
        return;
    struct cv_path tmp;
    rect_path(c, &tmp, x, y, w, h);
    cv_stroke(c, &tmp);
}

int cv_point_in_stroke(struct cv *c, struct cv_path *p, double x, double y)
{
    if (!p)
        p = &c->path;
    if (!finite3(x, y, 0))
        return 0;
    struct cv_path out;
    memset(&out, 0, sizeof(out));
    stroke_path(p, &c->st, &out);
    return winding_at(&out, (float)x, (float)y, 0);
}

/* ---- images ---- */

void cv_draw_image(struct cv *c, const struct image *src, double sx, double sy, double sw, double sh, double dx,
                   double dy, double dw, double dh)
{
    if (!c->img || !src || src->w <= 0 || src->h <= 0)
        return;
    if (!finite3(sx, sy, sw) || !finite3(sh, dx, dy) || !finite3(dw, dh, 0) || dw == 0 || dh == 0 || sw == 0 || sh == 0)
        return;
    if (sw < 0)
        sx += sw, sw = -sw;
    if (sh < 0)
        sy += sh, sh = -sh;
    if (dw < 0)
        dx += dw, dw = -dw;
    if (dh < 0)
        dy += dh, dh = -dh;
    /* the part of the source that exists */
    double cx0 = sx < 0 ? 0 : sx, cy0 = sy < 0 ? 0 : sy;
    double cx1 = sx + sw > src->w ? src->w : sx + sw, cy1 = sy + sh > src->h ? src->h : sy + sh;
    if (cx1 <= cx0 || cy1 <= cy0)
        return;
    double kx = dw / sw, ky = dh / sh;
    /* shrink the destination to match the clipped source */
    double ddx = dx + (cx0 - sx) * kx, ddy = dy + (cy0 - sy) * ky;
    double ddw = (cx1 - cx0) * kx, ddh = (cy1 - cy0) * ky;
    struct bsink bs;
    memset(&bs, 0, sizeof(bs));
    cv_mat inv;
    if (!minv(&c->st.m, &inv))
        return;
    bs.paint.kind = P_IMAGE;
    bs.paint.img = src;
    bs.paint.clamp = 1;
    bs.paint.bx0 = (float)cx0, bs.paint.by0 = (float)cy0, bs.paint.bx1 = (float)ceil(cx1), bs.paint.by1 = (float)ceil(cy1);
    bs.paint.smooth = c->st.smooth;
    /* device -> user -> source */
    double a = 1 / kx, d = 1 / ky;
    bs.paint.inv[0] = (float)(inv.a * a);
    bs.paint.inv[1] = (float)(inv.b * d);
    bs.paint.inv[2] = (float)(inv.c * a);
    bs.paint.inv[3] = (float)(inv.d * d);
    bs.paint.inv[4] = (float)((inv.e - ddx) * a + cx0);
    bs.paint.inv[5] = (float)((inv.f - ddy) * d + cy0);
    bs.im = c->img;
    bs.clip = c->st.clip;
    bs.alpha = (int)(c->st.alpha * 256 + 0.5f);
    bs.op = c->st.op;
    struct cv_path tmp;
    rect_path(c, &tmp, ddx, ddy, ddw, ddh);
    int ne;
    struct irect bb;
    struct edge *e = path_edges(&tmp, &ne, &bb);
    if (!e)
        return;
    if (shadow_on(&c->st))
        draw_shadow(c, e, ne, 0, bb);
    raster(e, ne, 0, canvas_rect(c), blend_row, &bs, op_is_global(bs.op));
    free(e);
}

void cv_get_image_data(struct cv *c, int x, int y, int w, int h, uint8_t *rgba)
{
    memset(rgba, 0, (size_t)w * h * 4);
    if (!c->img)
        return;
    for (int j = 0; j < h; j++) {
        int Y = y + j;
        if (Y < 0 || Y >= c->img->h)
            continue;
        for (int i = 0; i < w; i++) {
            int X = x + i;
            if (X < 0 || X >= c->img->w)
                continue;
            uint32_t p = c->img->px[(size_t)Y * c->img->w + X];
            uint8_t *o = rgba + ((size_t)j * w + i) * 4;
            o[0] = (uint8_t)(p >> 16), o[1] = (uint8_t)(p >> 8), o[2] = (uint8_t)p, o[3] = (uint8_t)(p >> 24);
        }
    }
}

void cv_put_image_data(struct cv *c, int x, int y, int w, int h, const uint8_t *rgba, int dx, int dy, int dw, int dh)
{
    if (!c->img)
        return;
    for (int j = dy; j < dy + dh; j++) {
        if (j < 0 || j >= h)
            continue;
        int Y = y + j;
        if (Y < 0 || Y >= c->img->h)
            continue;
        for (int i = dx; i < dx + dw; i++) {
            if (i < 0 || i >= w)
                continue;
            int X = x + i;
            if (X < 0 || X >= c->img->w)
                continue;
            const uint8_t *s = rgba + ((size_t)j * w + i) * 4;
            c->img->px[(size_t)Y * c->img->w + X] =
                s[3] ? (uint32_t)s[3] << 24 | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | (uint32_t)s[2] : 0;
        }
    }
}

/* ---- text ---- */

static int has_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = s; (p = strcasestr(p, w)); p += n) {
        int before = p == s || !((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z') || p[-1] == '-');
        int after = !((p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z') || p[n] == '-');
        if (before && after)
            return 1;
    }
    return 0;
}

/* A decimal number at the start of s (the libc has no strtod). */
static double parse_dbl(const char *s, char **end)
{
    double v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    if (*p == '.') {
        double f = 0.1;
        p++;
        while (*p >= '0' && *p <= '9') {
            v += (*p++ - '0') * f;
            f /= 10;
        }
    }
    *end = (char *)p;
    return v;
}

/* "italic bold 20px/1.2 'Helvetica', sans-serif": the style, weight, size and family that matter. */
int cv_set_font(struct cv *c, const char *css)
{
    char buf[200];
    snprintf(buf, sizeof(buf), "%s", css);
    int bold = 0, italic = 0;
    double px = -1;
    char *p = buf;
    char *family = NULL;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char *w = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        char save = *p;
        *p = 0;
        if (!strcasecmp(w, "italic") || !strcasecmp(w, "oblique"))
            italic = 1;
        else if (!strcasecmp(w, "bold") || !strcasecmp(w, "bolder"))
            bold = 1;
        else if (!strcasecmp(w, "normal") || !strcasecmp(w, "small-caps") || !strcasecmp(w, "lighter"))
            ;
        else if (w[0] >= '1' && w[0] <= '9' && strlen(w) == 3 && !strcmp(w + 1, "00"))
            bold = w[0] >= '6';
        else if ((w[0] >= '0' && w[0] <= '9') || w[0] == '.') {
            char *end;
            double v = parse_dbl(w, &end);
            if (!strncasecmp(end, "px", 2))
                px = v;
            else if (!strncasecmp(end, "pt", 2))
                px = v * 4 / 3;
            else if (!strncasecmp(end, "em", 2) || !strncasecmp(end, "rem", 3))
                px = v * 16;
            else if (!strncmp(end, "%", 1))
                px = v * 16 / 100;
            else if (!*end || *end == '/')
                px = v;
            else
                return -1;
            if (save) {
                family = p + 1;
                break;
            }
            return -1;
        } else if (!strcasecmp(w, "xx-small")) px = 9;
        else if (!strcasecmp(w, "x-small")) px = 10;
        else if (!strcasecmp(w, "small")) px = 13;
        else if (!strcasecmp(w, "medium")) px = 16;
        else if (!strcasecmp(w, "large")) px = 18;
        else if (!strcasecmp(w, "x-large")) px = 24;
        else if (!strcasecmp(w, "xx-large")) px = 32;
        else {
            return -1;
        }
        if (px >= 0 && save) {
            family = p + 1;
            break;
        }
        if (save)
            p++;
    }
    if (px < 0 || !family)
        return -1;
    int fam = FAM_SANS;
    if (has_word(family, "monospace") || has_word(family, "courier") || has_word(family, "consolas") ||
        has_word(family, "menlo") || has_word(family, "monaco") || has_word(family, "mono"))
        fam = FAM_MONO;
    else if (has_word(family, "serif") && !has_word(family, "sans-serif")) {
        fam = FAM_SERIF;
    } else if (has_word(family, "times") || has_word(family, "georgia") || has_word(family, "garamond") ||
               has_word(family, "palatino"))
        fam = FAM_SERIF;
    c->st.family = fam;
    c->st.bold = bold;
    c->st.italic = italic;
    c->st.px = (int)(px + 0.5) < 1 ? 1 : (int)(px + 0.5) > 255 ? 255 : (int)(px + 0.5);
    snprintf(c->st.font, sizeof(c->st.font), "%s", css);
    return 0;
}

double cv_measure(struct cv *c, const char *s, int n, double *ascent, double *descent)
{
    int w16 = wfont_measure(c->st.family, c->st.bold, c->st.italic, c->st.px, s, n);
    int a, d;
    wfont_vmetrics(c->st.family, c->st.bold, c->st.italic, c->st.px, &a, &d);
    if (ascent)
        *ascent = a;
    if (descent)
        *descent = d;
    return (double)w16 / 16;
}

/* Fill edges through a stencil image's alpha: a pixel's coverage is the geometry's times the stencil's. */
struct sten {
    struct bsink *b;
    const struct paint *sp;
    uint8_t *buf;
};

static void stencil_row(void *u, int y, int x0, int x1, const uint8_t *cov)
{
    struct sten *s = u;
    for (int x = x0; x < x1; x++) {
        int c = cov[x - x0];
        if (c) {
            float uu, vv;
            paint_map(s->sp, (float)x + 0.5f, (float)y + 0.5f, &uu, &vv);
            uint32_t t = sample_image(s->sp, uu, vv);
            c = c * (int)(t >> 24) / 255;
        }
        s->buf[x - x0] = (uint8_t)c;
    }
    blend_row(s->b, y, x0, x1, s->buf);
}

/* Text: drawn into a coverage mask (webfont.c) that is then painted through the transform. max_width < 0: none. */
void cv_text(struct cv *c, const char *s, int n, double x, double y, double max_width, int stroke)
{
    if (!c->img || n <= 0 || !finite3(x, y, 0))
        return;
    if (max_width >= 0 && !(max_width > 0))
        return;
    const struct cv_state *st = &c->st;
    double asc, desc, width = cv_measure(c, s, n, &asc, &desc);
    double scale_x = max_width > 0 && width > max_width ? max_width / width : 1;
    double ox = x;
    int align = st->align;
    if (align == CV_ALIGN_START)
        align = st->rtl ? CV_ALIGN_RIGHT : CV_ALIGN_LEFT;
    else if (align == CV_ALIGN_END)
        align = st->rtl ? CV_ALIGN_LEFT : CV_ALIGN_RIGHT;
    if (align == CV_ALIGN_RIGHT)
        ox -= width * scale_x;
    else if (align == CV_ALIGN_CENTER)
        ox -= width * scale_x / 2;
    double oy = y;                       /* the baseline */
    switch (st->baseline) {
    case CV_TOP: oy = y + asc * 0.8; break;
    case CV_HANGING: oy = y + asc * 0.72; break;
    case CV_MIDDLE: oy = y + (asc - desc) / 2; break;
    case CV_IDEOGRAPHIC: oy = y - desc * 0.5; break;
    case CV_BOTTOM: oy = y - desc; break;
    }
    struct wmask mk;
    if (!wfont_mask(st->family, st->bold, st->italic, st->px, s, n, &mk))
        return;
    struct image mi;
    mi.w = mk.w;
    mi.h = mk.h;
    mi.px = malloc((size_t)mk.w * mk.h * sizeof(uint32_t));
    if (!mi.px) {
        free(mk.cov);
        return;
    }
    for (int i = 0; i < mk.w * mk.h; i++)
        mi.px[i] = (uint32_t)mk.cov[i] << 24 | 0xFFFFFF;
    free(mk.cov);

    cv_mat ctm = c->st.m, t = {scale_x, 0, 0, 1, ox - mk.pad * scale_x, oy - mk.ascent};
    if (fabs(ctm.b) < 1e-9 && fabs(ctm.c) < 1e-9 && fabs(ctm.a - 1) < 1e-9 && fabs(ctm.d - 1) < 1e-9 && scale_x == 1) {
        t.e = floor(ctm.e + t.e + 0.5) - ctm.e;     /* a plain move: land on whole pixels so the text stays sharp */
        t.f = floor(ctm.f + t.f + 0.5) - ctm.f;
    }
    cv_mat full = mmul(ctm, t), inv;
    struct bsink bs;
    memset(&bs, 0, sizeof(bs));
    if (minv(&full, &inv) && setup_paint(&bs.paint, stroke ? &st->stroke : &st->fill, &ctm)) {
        struct paint stencil;
        memset(&stencil, 0, sizeof(stencil));
        stencil.kind = P_IMAGE;
        stencil.img = &mi;
        stencil.clamp = 1;
        stencil.bx1 = (float)mi.w, stencil.by1 = (float)mi.h;
        stencil.smooth = 1;
        stencil.inv[0] = (float)inv.a, stencil.inv[1] = (float)inv.b, stencil.inv[2] = (float)inv.c;
        stencil.inv[3] = (float)inv.d, stencil.inv[4] = (float)inv.e, stencil.inv[5] = (float)inv.f;
        bs.im = c->img;
        bs.clip = st->clip;
        bs.alpha = (int)(st->alpha * 256 + 0.5f);
        bs.op = st->op;
        struct cv_path quad;
        memset(&quad, 0, sizeof(quad));
        cv_path_rect(&quad, &full, 0, 0, mi.w, mi.h);
        int ne;
        struct irect bb;
        struct edge *e = path_edges(&quad, &ne, &bb);
        uint8_t *buf = malloc((size_t)c->img->w + 4);
        if (e && buf) {
            struct sten sn = {&bs, &stencil, buf};
            raster(e, ne, 0, canvas_rect(c), stencil_row, &sn, op_is_global(bs.op));
        }
        free(e);
        free(buf);
    }
    free(mi.px);
}

/* ---- colours and names ---- */

/* The alpha of rgba(), hsla() and "/ alpha" colours to 8-bit precision (css_parse_color keeps percents). */
static int exact_alpha(const char *s, int *alpha)
{
    const char *p = s;
    while (*p == ' ')
        p++;
    if (strncasecmp(p, "rgb", 3) && strncasecmp(p, "hsl", 3))
        return 0;
    const char *open = strchr(p, '('), *close = strrchr(p, ')');
    if (!open || !close || close < open)
        return 0;
    int commas = 0;
    const char *last = NULL;
    for (const char *q = open; q < close; q++) {
        if (*q == ',') {
            commas++;
            last = q;
        } else if (*q == '/') {
            last = q;
            commas = 3;
            break;
        }
    }
    if (commas != 3 || !last)
        return 0;
    last++;
    while (*last == ' ')
        last++;
    if (!((*last >= '0' && *last <= '9') || *last == '.'))
        return 0;
    char *end;
    double a = parse_dbl(last, &end);
    if (*end == '%')
        a /= 100;
    if (a > 1)
        a = 1;
    *alpha = (int)(a * 255 + 0.5);
    return 1;
}

int cv_parse_color(const char *s, uint32_t *argb)
{
    uint32_t c;
    if (css_parse_color(s, (int)strlen(s), &c) != 0)
        return -1;
    int a;
    if (exact_alpha(s, &a))
        c = (c & 0xFFFFFF) | (uint32_t)a << 24;
    *argb = c;
    return 0;
}

static double strtod_lite(const char *s)
{
    char *end;
    return parse_dbl(s, &end);
}

void cv_color_string(uint32_t argb, char *out, int size)
{
    int a = (int)(argb >> 24), r = (int)(argb >> 16 & 255), g = (int)(argb >> 8 & 255), b = (int)(argb & 255);
    if (a == 255) {
        snprintf(out, (size_t)size, "#%02x%02x%02x", r, g, b);
        return;
    }
    char al[16];
    snprintf(al, sizeof(al), "%.2f", a / 255.0);        /* two decimals when they read back as the same alpha */
    if ((int)(strtod_lite(al) * 255 + 0.5) != a)
        snprintf(al, sizeof(al), "%.3f", a / 255.0);
    char *e = al + strlen(al);
    while (e > al + 1 && e[-1] == '0')
        *--e = 0;
    if (e > al && e[-1] == '.')
        *--e = 0;
    snprintf(out, (size_t)size, "rgba(%d, %d, %d, %s)", r, g, b, al);
}

static const char *const op_names[CV_NOPS] = {
    "source-over", "source-in", "source-out", "source-atop", "destination-over", "destination-in",
    "destination-out", "destination-atop", "lighter", "copy", "xor", "multiply", "screen", "overlay", "darken",
    "lighten", "color-dodge", "color-burn", "hard-light", "soft-light", "difference", "exclusion"};

int cv_parse_op(const char *s)
{
    for (int i = 0; i < CV_NOPS; i++)
        if (!strcmp(s, op_names[i]))
            return i;
    return -1;
}

const char *cv_op_name(int op)
{
    return op >= 0 && op < CV_NOPS ? op_names[op] : "source-over";
}

/* ---- PNG ---- */

static uint32_t crc_table[256];

static uint32_t crc32_of(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static size_t chunk(uint8_t *out, const char *type, const uint8_t *data, size_t n)
{
    be32(out, (uint32_t)n);
    memcpy(out + 4, type, 4);
    if (n)
        memcpy(out + 8, data, n);
    uint32_t crc = crc32_of(0, out + 4, n + 4);
    be32(out + 8 + n, crc);
    return n + 12;
}

uint8_t *cv_png(const struct image *img, size_t *len)
{
    if (!img || img->w <= 0 || img->h <= 0)
        return NULL;
    size_t row = (size_t)img->w * 4 + 1, raw = row * (size_t)img->h;
    size_t blocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + blocks * 5 + 4;
    uint8_t *z = malloc(zlen), *out = malloc(zlen + 100);
    if (!z || !out) {
        free(z);
        free(out);
        return NULL;
    }
    uint8_t *zp = z;
    *zp++ = 0x78;
    *zp++ = 0x01;
    uint32_t a = 1, b = 0;
    size_t pos = 0;
    uint8_t *line = malloc(row);
    if (!line) {
        free(z);
        free(out);
        return NULL;
    }
    /* feed the raw scanlines through stored deflate blocks */
    size_t blk_left = 0;
    uint8_t *hdr = NULL;
    for (int y = 0; y < img->h; y++) {
        line[0] = 0;
        for (int x = 0; x < img->w; x++) {
            uint32_t p = img->px[(size_t)y * img->w + x];
            line[1 + x * 4] = (uint8_t)(p >> 16), line[2 + x * 4] = (uint8_t)(p >> 8);
            line[3 + x * 4] = (uint8_t)p, line[4 + x * 4] = (uint8_t)(p >> 24);
        }
        for (size_t i = 0; i < row; i++) {
            if (!blk_left) {
                size_t rest = raw - pos, n = rest < 65535 ? rest : 65535;
                hdr = zp;
                *zp++ = rest <= 65535;
                *zp++ = (uint8_t)n;
                *zp++ = (uint8_t)(n >> 8);
                *zp++ = (uint8_t)~n;
                *zp++ = (uint8_t)(~n >> 8);
                blk_left = n;
            }
            *zp++ = line[i];
            a = (a + line[i]) % 65521;
            b = (b + a) % 65521;
            blk_left--;
            pos++;
        }
    }
    (void)hdr;
    uint32_t ad = b << 16 | a;
    be32(zp, ad);
    zp += 4;
    size_t o = 0;
    memcpy(out, "\x89PNG\r\n\x1a\n", 8);
    o = 8;
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)img->w);
    be32(ihdr + 4, (uint32_t)img->h);
    ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    o += chunk(out + o, "IHDR", ihdr, 13);
    uint8_t *big = realloc(out, o + (size_t)(zp - z) + 12 + 12);
    if (!big) {
        free(z);
        free(out);
        free(line);
        return NULL;
    }
    out = big;
    o += chunk(out + o, "IDAT", z, (size_t)(zp - z));
    o += chunk(out + o, "IEND", NULL, 0);
    free(z);
    free(line);
    *len = o;
    return out;
}
