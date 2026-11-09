/*
 * Chess's 3D view: Staunton pieces turned on a lathe (the knight's head is
 * its outline drawn out sideways, with rounded edges), a wooden board with
 * glossy squares that the pieces are reflected in, soft shadows under
 * them, and the marks on the squares, all in front of a picture.
 *
 * The board is one unit a square, its middle at the origin, white's side
 * toward +z, up +y. The reflection needs no stencil buffer (which would
 * keep the 3D hardware out of it): the squares are drawn first at the far
 * depth over a depth buffer cleared to the near one, so that the pieces
 * mirrored under the board come through only where it is, and then again,
 * see-through, over the reflection. The picture behind is not drawn with GL
 * at all: what the scene leaves at the clear color is filled in from it
 * once the frame is read back, which costs the software renderer nothing.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include "chess.h"

#define SLICES   18                     /* around a turned piece */
#define BOARD_IN 4.0f                   /* the squares' edge, from the middle */
#define FRAME_W  0.55f                  /* the frame around them */
#define BOARD_D  0.42f                  /* the board's thickness */
#define SQUARE_ALPHA 0.68f              /* how much of a square covers its reflection */
#define DEG      ((float)M_PI / 180.0f)

static GLuint piece_list[7], reflected_list[7], frame_list, wood_tex;
static int lod;                         /* building the plainer models the reflection shows */

static const float heights[7] = { 0, 0.72f, 0.96f, 0.97f, 0.84f, 1.21f, 1.40f };
static const float radii[7] = { 0, 0.30f, 0.33f, 0.31f, 0.33f, 0.34f, 0.35f };

float chessview_height(int type)
{
    return heights[type & 7];
}

float chessview_radius(int type)
{
    return radii[type & 7];
}

/* Each piece's outline, roughly, as (height, radius) up from its foot: what picking it
 * out with the pointer measures against (the knight's head reaches forward). */
static const float girths[7][10][2] = {
    [PAWN] = { { 0, .30f }, { .14f, .22f }, { .38f, .12f }, { .42f, .17f }, { .47f, .12f }, { .56f, .14f },
               { .66f, .09f }, { .72f, 0 } },
    [KNIGHT] = { { 0, .33f }, { .14f, .25f }, { .24f, .22f }, { .60f, .25f }, { .85f, .16f }, { .96f, .08f } },
    [BISHOP] = { { 0, .31f }, { .14f, .23f }, { .55f, .11f }, { .57f, .17f }, { .62f, .11f }, { .72f, .14f },
                 { .90f, .06f }, { .97f, .04f } },
    [ROOK] = { { 0, .33f }, { .14f, .25f }, { .56f, .18f }, { .60f, .22f }, { .63f, .19f }, { .70f, .235f },
               { .84f, .235f } },
    [QUEEN] = { { 0, .34f }, { .14f, .26f }, { .74f, .12f }, { .77f, .19f }, { .82f, .11f }, { 1.02f, .21f },
                { 1.09f, .20f }, { 1.14f, .09f }, { 1.21f, .05f } },
    [KING] = { { 0, .35f }, { .14f, .27f }, { .82f, .12f }, { .85f, .20f }, { .90f, .12f }, { 1.08f, .20f },
               { 1.17f, .08f }, { 1.40f, .07f } },
};

float chessview_girth(int type, float y)
{
    const float (*g)[2] = girths[type & 7];
    if (y < 0 || y > heights[type & 7])
        return 0;
    for (int i = 1; i < 10 && (g[i][0] > 0 || g[i][1] > 0); i++)
        if (y <= g[i][0])
            return g[i - 1][1] + (g[i][1] - g[i - 1][1]) * (y - g[i - 1][0]) / (g[i][0] - g[i - 1][0]);
    return 0.07f;                       /* the top of the last stretch */
}

void chessview_square(int sq, float *x, float *z)
{
    *x = (float)FILE_OF(sq) - 3.5f;
    *z = 3.5f - (float)RANK_OF(sq);
}

int chessview_square_at(float x, float z)
{
    int f = (int)floorf(x + 4.0f), r = (int)floorf(4.0f - z);
    return f >= 0 && f < 8 && r >= 0 && r < 8 ? SQ(f, r) : -1;
}

/* ---- triangles, wound to face along their normals ---- */

static void sub3(float *o, const float *a, const float *b)
{
    o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2];
}

static void cross3(float *o, const float *a, const float *b)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot3(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void norm3(float *v)
{
    float l = sqrtf(dot3(v, v));
    if (l > 1e-9f)
        v[0] /= l, v[1] /= l, v[2] /= l;
}

/* Inside glBegin(GL_TRIANGLES): counterclockwise as seen from the side the normals face. */
static void tri(const float *p0, const float *n0, const float *p1, const float *n1, const float *p2,
                const float *n2)
{
    float e1[3], e2[3], f[3], n[3] = { n0[0] + n1[0] + n2[0], n0[1] + n1[1] + n2[1], n0[2] + n1[2] + n2[2] };
    sub3(e1, p1, p0);
    sub3(e2, p2, p0);
    cross3(f, e1, e2);
    if (dot3(f, f) < 1e-14f)
        return;                         /* no area */
    if (dot3(f, n) < 0) {
        const float *t = p1, *tn = n1;
        p1 = p2, n1 = n2, p2 = t, n2 = tn;
    }
    glNormal3fv(n0), glVertex3fv(p0);
    glNormal3fv(n1), glVertex3fv(p1);
    glNormal3fv(n2), glVertex3fv(p2);
}

static void quad(const float *p0, const float *n0, const float *p1, const float *n1, const float *p2,
                 const float *n2, const float *p3, const float *n3)
{
    tri(p0, n0, p1, n1, p2, n2);
    tri(p0, n0, p2, n2, p3, n3);
}

static void flat_quad(const float *p0, const float *p1, const float *p2, const float *p3, const float *n)
{
    quad(p0, n, p1, n, p2, n, p3, n);
}

/* The quads between two rows of n points as one strip (each vertex lit once, not in six
 * triangles' worth), wound to face along the normals as most of its quads say; a closed
 * band goes on round from the last points to the first. */
static void band(int n, float (*pa)[3], float (*na)[3], float (*pb)[3], float (*nb)[3], int closed)
{
    int m = closed ? n : n - 1;
    float score = 0;
    for (int j = 0; j < m; j++) {
        int k = (j + 1) % n;
        const float *q[4] = { pa[j], pb[j], pb[k], pa[k] };
        float f[3] = { 0, 0, 0 };
        for (int e = 0; e < 4; e++) {   /* Newell's normal, whatever edges have no length */
            const float *u = q[e], *v = q[(e + 1) & 3];
            f[0] += (u[1] - v[1]) * (u[2] + v[2]);
            f[1] += (u[2] - v[2]) * (u[0] + v[0]);
            f[2] += (u[0] - v[0]) * (u[1] + v[1]);
        }
        float a[3] = { na[j][0] + nb[j][0] + nb[k][0] + na[k][0], na[j][1] + nb[j][1] + nb[k][1] + na[k][1],
                       na[j][2] + nb[j][2] + nb[k][2] + na[k][2] };
        score += dot3(f, a);
    }
    float (*p0)[3] = score < 0 ? pb : pa, (*n0)[3] = score < 0 ? nb : na;
    float (*p1)[3] = score < 0 ? pa : pb, (*n1)[3] = score < 0 ? na : nb;
    glBegin(GL_QUAD_STRIP);
    for (int j = 0; j <= m; j++) {
        int k = j % n;
        glNormal3fv(n0[k]), glVertex3fv(p0[k]);
        glNormal3fv(n1[k]), glVertex3fv(p1[k]);
    }
    glEnd();
}

/* ---- turned pieces ---- */

#define MAX_PROFILE 96

/* A piece's outline from the middle of its foot round to the top of its head, as
 * (radius, height); a crease is a sharp edge there rather than a smooth curve. */
struct profile {
    int n;
    float r[MAX_PROFILE], y[MAX_PROFILE];
    uint8_t crease[MAX_PROFILE];
    uint8_t carved[MAX_PROFILE];        /* the bishop's slit is cut into these */
};

static int carving;

static void pt(struct profile *p, float r, float y, int crease)
{
    if (p->n && fabsf(p->r[p->n - 1] - r) < 1e-5f && fabsf(p->y[p->n - 1] - y) < 1e-5f) {
        p->crease[p->n - 1] |= (uint8_t)crease;
        return;
    }
    if (p->n >= MAX_PROFILE)
        return;
    p->r[p->n] = r < 0 ? 0 : r;
    p->y[p->n] = y;
    p->crease[p->n] = (uint8_t)crease;
    p->carved[p->n] = (uint8_t)carving;
    p->n++;
}

/* Part of a circle about (cr, cy), from angle a0 to a1 (degrees, 0 pointing out). */
static void arc(struct profile *p, float cr, float cy, float rad, float a0, float a1, int steps)
{
    for (int i = 0; i <= steps; i++) {
        float a = (a0 + (a1 - a0) * (float)i / (float)steps) * DEG;
        pt(p, cr + rad * cosf(a), cy + rad * sinf(a), 0);
    }
}

/* A quadratic curve from the last point, toward (cx, cy), to (x, y). */
static void curve(struct profile *p, float cx, float cy, float x, float y, int steps)
{
    float x0 = p->r[p->n - 1], y0 = p->y[p->n - 1];
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps, u = 1 - t;
        pt(p, u * u * x0 + 2 * u * t * cx + t * t * x, u * u * y0 + 2 * u * t * cy + t * t * y, 0);
    }
}

/* The foot every piece stands on: a rounded plinth and a bead above it, ending at
 * (R - 0.085, 0.135). */
static void foot(struct profile *p, float R)
{
    pt(p, 0, 0, 1);
    pt(p, R, 0, 1);
    pt(p, R, 0.03f, 0);
    arc(p, R - 0.03f, 0.03f, 0.03f, 0, 90, 2);
    pt(p, R - 0.085f, 0.06f, 1);
    arc(p, R - 0.085f, 0.0975f, 0.0375f, -90, 90, 4);
    pt(p, R - 0.085f, 0.135f, 1);
}

/* A ring that bulges out, as on the collars: from (r, y) out to r + rad and back in. */
static void bead(struct profile *p, float r, float y, float rad)
{
    pt(p, r, y, 1);
    arc(p, r, y + rad, rad, -90, 90, 4);
    pt(p, r, y + 2 * rad, 1);
}

/* The bishop's slit: a groove where a slanted plane meets the front of the mitre. */
static float slit_depth(float x, float y)
{
    float d = 0.57f * x + 0.82f * (y - 0.84f), w = 0.016f;
    if (fabsf(d) >= w || x < -0.04f)
        return 0;
    float across = 1 - (d / w) * (d / w), side = x > 0.08f ? 1 : (x + 0.04f) / 0.12f;
    return 0.045f * across * side;
}

static void lathe_point(const struct profile *p, int i, int j, int slices, float *pos)
{
    float a = 2 * (float)M_PI * (float)j / (float)slices, c = cosf(a), s = sinf(a), r = p->r[i];
    if (p->carved[i] && !lod)
        r -= slit_depth(r * c, p->y[i]);
    if (r < 0.004f && p->r[i] > 0)
        r = 0.004f;
    pos[0] = r * c, pos[1] = p->y[i], pos[2] = r * s;
}

/* Turn the outline round the y axis, one smooth stretch between creases at a time. */
static void lathe(const struct profile *p)
{
    static float pos[MAX_PROFILE][49][3], nrm[MAX_PROFILE][49][3];
    int start = 0;
    while (start < p->n - 1) {
        int end = start + 1;
        while (end < p->n - 1 && !p->crease[end])
            end++;
        int carved = 0;
        for (int i = start; i <= end; i++)
            carved |= p->carved[i];
        int slices = lod ? SLICES / 2 : carved ? 36 : SLICES;
        for (int i = start; i <= end; i++) {
            /* the outline's normal: across its direction here, averaged over the two sides */
            float tr, ty;
            if (i == start) {
                tr = p->r[i + 1] - p->r[i], ty = p->y[i + 1] - p->y[i];
            } else if (i == end) {
                tr = p->r[i] - p->r[i - 1], ty = p->y[i] - p->y[i - 1];
            } else {
                float ar = p->r[i] - p->r[i - 1], ay = p->y[i] - p->y[i - 1];
                float br = p->r[i + 1] - p->r[i], by = p->y[i + 1] - p->y[i];
                float la = sqrtf(ar * ar + ay * ay), lb = sqrtf(br * br + by * by);
                tr = ar / la + br / lb, ty = ay / la + by / lb;
            }
            float l = sqrtf(tr * tr + ty * ty), nr = ty / l, ny = -tr / l;
            for (int j = 0; j <= slices; j++) {
                float a = 2 * (float)M_PI * (float)j / (float)slices;
                lathe_point(p, i, j, slices, pos[i][j]);
                nrm[i][j][0] = nr * cosf(a), nrm[i][j][1] = ny, nrm[i][j][2] = nr * sinf(a);
            }
        }
        if (carved && !lod) {
            /* the slit bends the surface: normals from the points around each */
            for (int i = start + 1; i < end; i++) {
                for (int j = 0; j <= slices; j++) {
                    float du[3], dv[3], n[3];
                    sub3(du, pos[i][(j + 1) % slices], pos[i][(j + slices - 1) % slices]);
                    sub3(dv, pos[i + 1][j], pos[i - 1][j]);
                    cross3(n, dv, du);
                    norm3(n);
                    if (dot3(n, n) > 0.5f) {
                        if (dot3(n, nrm[i][j]) < 0)
                            n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
                        memcpy(nrm[i][j], n, sizeof(n));
                    }
                }
            }
        }
        for (int i = start; i < end; i++)
            band(slices + 1, pos[i], nrm[i], pos[i + 1], nrm[i + 1], 0);
        start = end;
    }
}

/* One of the rook's battlements: a piece of a ring from angle a0 to a1. */
static void merlon(float a0, float a1, float r0, float r1, float y0, float y1)
{
    enum { STEPS = 4 };
    static const float up[3] = { 0, 1, 0 };
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < STEPS; i++) {
        float b0 = (a0 + (a1 - a0) * (float)i / STEPS) * DEG, b1 = (a0 + (a1 - a0) * (float)(i + 1) / STEPS) * DEG;
        float c0 = cosf(b0), s0 = sinf(b0), c1 = cosf(b1), s1 = sinf(b1);
        float o0[3] = { c0, 0, s0 }, o1[3] = { c1, 0, s1 }, i0[3] = { -c0, 0, -s0 }, i1[3] = { -c1, 0, -s1 };
        float A[3] = { r1 * c0, y0, r1 * s0 }, B[3] = { r1 * c1, y0, r1 * s1 };
        float C[3] = { r1 * c1, y1, r1 * s1 }, D[3] = { r1 * c0, y1, r1 * s0 };
        quad(A, o0, B, o1, C, o1, D, o0);
        float E[3] = { r0 * c0, y0, r0 * s0 }, F[3] = { r0 * c1, y0, r0 * s1 };
        float G[3] = { r0 * c1, y1, r0 * s1 }, H[3] = { r0 * c0, y1, r0 * s0 };
        quad(E, i0, F, i1, G, i1, H, i0);
        flat_quad(D, C, G, H, up);
    }
    for (int k = 0; k < 2; k++) {
        float a = (k ? a1 : a0) * DEG, c = cosf(a), s = sinf(a);
        float n[3] = { k ? -s : s, 0, k ? c : -c };
        float A[3] = { r0 * c, y0, r0 * s }, B[3] = { r1 * c, y0, r1 * s };
        float C[3] = { r1 * c, y1, r1 * s }, D[3] = { r0 * c, y1, r0 * s };
        flat_quad(A, B, C, D, n);
    }
    glEnd();
}

static void box(float x0, float x1, float y0, float y1, float z0, float z1)
{
    float p[8][3];
    for (int i = 0; i < 8; i++)
        p[i][0] = i & 1 ? x1 : x0, p[i][1] = i & 2 ? y1 : y0, p[i][2] = i & 4 ? z1 : z0;
    static const int faces[6][4] = { { 0, 2, 6, 4 }, { 1, 5, 7, 3 }, { 0, 4, 5, 1 },
                                     { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 6, 7, 5 } };
    static const float normals[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 },
                                         { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
    glBegin(GL_TRIANGLES);
    for (int f = 0; f < 6; f++)
        flat_quad(p[faces[f][0]], p[faces[f][1]], p[faces[f][2]], p[faces[f][3]], normals[f]);
    glEnd();
}

static void ball(float cx, float cy, float cz, float rad)
{
    enum { LON = 10 };
    int lat = lod ? 3 : 5, lon = lod ? LON / 2 : LON;
    float p[2][LON + 1][3], n[2][LON + 1][3];
    for (int i = 0; i <= lat; i++) {
        float th = ((float)i / (float)lat - 0.5f) * (float)M_PI;
        for (int j = 0; j <= lon; j++) {
            float ph = (float)j / (float)lon * 2 * (float)M_PI, *v = n[i & 1][j], *q = p[i & 1][j];
            v[0] = cosf(th) * cosf(ph), v[1] = sinf(th), v[2] = cosf(th) * sinf(ph);
            q[0] = cx + rad * v[0], q[1] = cy + rad * v[1], q[2] = cz + rad * v[2];
        }
        if (i)
            band(lon + 1, p[(i - 1) & 1], n[(i - 1) & 1], p[i & 1], n[i & 1], 0);
    }
}

/* ---- the knight's head ---- */

/* Its outline from the side, facing +z: (forward, up), from the back of the neck over the
 * ears and down the face, under the jaw and down the chest. */
static const float horse[][2] = {
    { -0.19f, 0.22f }, { -0.205f, 0.30f }, { -0.20f, 0.40f }, { -0.18f, 0.50f }, { -0.145f, 0.60f },
    { -0.10f, 0.69f }, { -0.06f, 0.77f }, { -0.035f, 0.84f }, { -0.025f, 0.90f }, { 0.0f, 0.965f },
    { 0.03f, 0.905f }, { 0.06f, 0.865f }, { 0.11f, 0.82f }, { 0.165f, 0.75f }, { 0.215f, 0.675f },
    { 0.25f, 0.615f }, { 0.268f, 0.575f }, { 0.262f, 0.535f }, { 0.24f, 0.515f }, { 0.205f, 0.527f },
    { 0.17f, 0.507f }, { 0.135f, 0.47f }, { 0.095f, 0.455f }, { 0.06f, 0.43f }, { 0.05f, 0.39f },
    { 0.075f, 0.33f }, { 0.10f, 0.27f }, { 0.105f, 0.22f },
};
#define HORSE_N ((int)(sizeof(horse) / sizeof(horse[0])))
#define HORSE_ROUND_X  0.075f          /* how far across the head its rounding reaches */
#define HORSE_ROUND_IN 0.085f           /* ... and in from the outline, where it is wide enough */
#define HORSE_RINGS    5

/* Half its thickness at a point of the outline: narrower toward the muzzle and at the ears. */
static float horse_width(float u, float v)
{
    float w = 0.13f - 0.2f * (u > 0.04f ? u - 0.04f : 0);
    if (v > 0.86f)
        w *= 1 - (v - 0.86f) * 3.0f;
    return w < 0.045f ? 0.045f : w;
}

/* Is c inside triangle abc's corner at b? (2D, counterclockwise) */
static float cross2(const float *a, const float *b, const float *c)
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

/* Cut a simple counterclockwise polygon into triangles (indices into tris), by clipping ears. */
static int ear_clip(float (*pt)[2], int n, int (*tris)[3])
{
    int idx[64], m = n, count = 0;
    for (int i = 0; i < n; i++)
        idx[i] = i;
    while (m > 3) {
        int found = 0;
        for (int i = 0; i < m && !found; i++) {
            int a = idx[(i + m - 1) % m], b = idx[i], c = idx[(i + 1) % m];
            if (cross2(pt[a], pt[b], pt[c]) <= 0)
                continue;                   /* a reflex corner */
            int inside = 0;
            for (int k = 0; k < m && !inside; k++) {
                int q = idx[k];
                if (q == a || q == b || q == c)
                    continue;
                inside = cross2(pt[a], pt[b], pt[q]) >= 0 && cross2(pt[b], pt[c], pt[q]) >= 0 &&
                         cross2(pt[c], pt[a], pt[q]) >= 0;
            }
            if (inside)
                continue;
            tris[count][0] = a, tris[count][1] = b, tris[count][2] = c;
            count++;
            memmove(idx + i, idx + i + 1, (size_t)(m - i - 1) * sizeof(int));
            m--;
            found = 1;
        }
        if (!found)
            break;                          /* not simple after all: fan out the rest */
    }
    for (int i = 1; i + 1 < m; i++) {
        tris[count][0] = idx[0], tris[count][1] = idx[i], tris[count][2] = idx[i + 1];
        count++;
    }
    return count;
}

/* How far it is from outline point i straight in (along -n) to the other side. */
static float across(float (*out)[2], float (*n)[2], int i)
{
    float best = 1, dx = -n[i][0], dy = -n[i][1];
    for (int k = 0; k < HORSE_N; k++) {
        int j = (k + 1) % HORSE_N;
        if (k == i || j == i)
            continue;
        float ex = out[j][0] - out[k][0], ey = out[j][1] - out[k][1];
        float den = dx * ey - dy * ex;
        if (fabsf(den) < 1e-9f)
            continue;
        float wx = out[k][0] - out[i][0], wy = out[k][1] - out[i][1];
        float t = (wx * ey - wy * ex) / den, s = (wx * dy - wy * dx) / den;
        if (t > 1e-4f && s >= 0 && s <= 1 && t < best)
            best = t;
    }
    return best;
}

/* The head: the outline drawn out to each side and rounded off like a pillow, in
 * HORSE_RINGS steps each side from the outline to the flat of the cheek. The rounding
 * goes in from the outline by as much as the head is wide there allows. */
static void knight_head(void)
{
    float out[HORSE_N][2], n2[HORSE_N][2], miter[HORSE_N], round_in[HORSE_N], round_x[HORSE_N], w[HORSE_N];
    float area = 0;
    for (int i = 0; i < HORSE_N; i++) {
        const float *a = horse[i], *b = horse[(i + 1) % HORSE_N];
        area += a[0] * b[1] - b[0] * a[1];
    }
    for (int i = 0; i < HORSE_N; i++) {     /* counterclockwise */
        int k = area > 0 ? i : HORSE_N - 1 - i;
        out[i][0] = horse[k][0], out[i][1] = horse[k][1];
    }
    for (int i = 0; i < HORSE_N; i++) {
        const float *p = out[(i + HORSE_N - 1) % HORSE_N], *c = out[i], *q = out[(i + 1) % HORSE_N];
        float e0[2] = { c[1] - p[1], -(c[0] - p[0]) }, e1[2] = { q[1] - c[1], -(q[0] - c[0]) };
        float l0 = sqrtf(e0[0] * e0[0] + e0[1] * e0[1]), l1 = sqrtf(e1[0] * e1[0] + e1[1] * e1[1]);
        e0[0] /= l0, e0[1] /= l0, e1[0] /= l1, e1[1] /= l1;
        float nx = e0[0] + e1[0], ny = e0[1] + e1[1], l = sqrtf(nx * nx + ny * ny);
        n2[i][0] = nx / l, n2[i][1] = ny / l;
        float cosine = n2[i][0] * e0[0] + n2[i][1] * e0[1];
        miter[i] = 1 / (cosine < 0.6f ? 0.6f : cosine);
        w[i] = horse_width(c[0], c[1]);
        round_x[i] = w[i] - 0.006f < HORSE_ROUND_X ? w[i] - 0.006f : HORSE_ROUND_X;
    }
    float raw[HORSE_N];
    for (int i = 0; i < HORSE_N; i++) {
        float room = 0.42f * across(out, n2, i) / miter[i];
        raw[i] = room < HORSE_ROUND_IN ? room : HORSE_ROUND_IN;
    }
    for (int i = 0; i < HORSE_N; i++) {     /* no sudden steps from one point to the next */
        float a = raw[(i + HORSE_N - 1) % HORSE_N], b = raw[(i + 1) % HORSE_N], m = (a + 2 * raw[i] + b) / 4;
        round_in[i] = m < raw[i] ? m : raw[i];
    }
    /* ring k at angle th: in from the outline by round_in (1 - cos th), out to the side by
     * width - round_x + round_x sin th; an ellipse's quarter, and its normal */
    float pos[2 * HORSE_RINGS + 2][HORSE_N][3], nrm[2 * HORSE_RINGS + 2][HORSE_N][3];
    int steps = lod ? 2 : HORSE_RINGS, rings = 2 * steps + 2;
    for (int side = -1; side <= 1; side += 2) {
        for (int k = 0; k <= steps; k++) {
            int ring = side < 0 ? steps - k : steps + 1 + k;
            float th = (float)k / (float)steps * 0.5f * (float)M_PI, c = cosf(th), sn = sinf(th);
            for (int i = 0; i < HORSE_N; i++) {
                float in = round_in[i] * (1 - c) * miter[i];
                pos[ring][i][0] = (float)side * (w[i] - round_x[i] + round_x[i] * sn);
                pos[ring][i][1] = out[i][1] - n2[i][1] * in;
                pos[ring][i][2] = out[i][0] - n2[i][0] * in;
                float a = round_x[i] * c, b = round_in[i] * sn, l = sqrtf(a * a + b * b);
                nrm[ring][i][0] = (float)side * b / l;
                nrm[ring][i][1] = n2[i][1] * a / l;
                nrm[ring][i][2] = n2[i][0] * a / l;
            }
        }
    }
    for (int r = 0; r + 1 < rings; r++)
        band(HORSE_N, pos[r], nrm[r], pos[r + 1], nrm[r + 1], 1);
    /* the flat of each cheek */
    glBegin(GL_TRIANGLES);
    float flat[HORSE_N][2];
    int tris[HORSE_N][3];
    for (int i = 0; i < HORSE_N; i++)
        flat[i][0] = pos[rings - 1][i][2], flat[i][1] = pos[rings - 1][i][1];
    int nt = ear_clip(flat, HORSE_N, tris);
    for (int side = 0; side < 2; side++) {
        int ring = side ? rings - 1 : 0;
        for (int t = 0; t < nt; t++) {
            const int *v = tris[t];
            tri(pos[ring][v[0]], nrm[ring][v[0]], pos[ring][v[1]], nrm[ring][v[1]], pos[ring][v[2]],
                nrm[ring][v[2]]);
        }
    }
    glEnd();
    if (lod)
        return;
    /* the eyes: a dark bead each side, on the cheek's surface */
    static const float eye[2] = { 0.125f, 0.725f };
    int near = 0;
    float dist = 1;
    for (int i = 0; i < HORSE_N; i++) {
        float d = hypotf(out[i][0] - eye[0], out[i][1] - eye[1]);
        if (d < dist)
            dist = d, near = i;
    }
    float x = w[near];
    if (dist < round_in[near])
        x = w[near] - round_x[near] + round_x[near] * sinf(acosf(1 - dist / round_in[near]));
    GLfloat dark[4] = { 0.05f, 0.05f, 0.06f, 1 };
    glPushAttrib(GL_LIGHTING_BIT);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, dark);
    for (int side = -1; side <= 1; side += 2)
        ball((float)side * (x - 0.008f), eye[1], eye[0], 0.02f);
    glPopAttrib();
}

/* ---- the pieces ---- */

static void build_piece(int type)
{
    struct profile p = { 0 };
    float R = radii[type];
    foot(&p, R);
    float neck = R - 0.085f;
    switch (type) {
    case PAWN:
        curve(&p, 0.11f, 0.20f, 0.095f, 0.39f, 6);
        bead(&p, 0.15f, 0.39f, 0.021f);
        pt(&p, 0.075f, 0.445f, 1);
        arc(&p, 0, 0.56f, 0.13f, -55, 90, 7);
        lathe(&p);
        break;
    case ROOK:
        curve(&p, 0.165f, 0.24f, 0.175f, 0.56f, 6);
        bead(&p, 0.20f, 0.56f, 0.02f);
        pt(&p, 0.19f, 0.605f, 1);
        curve(&p, 0.19f, 0.65f, 0.235f, 0.685f, 4);
        pt(&p, 0.235f, 0.72f, 1);
        pt(&p, 0.15f, 0.72f, 1);
        pt(&p, 0.15f, 0.70f, 1);
        pt(&p, 0, 0.70f, 0);
        lathe(&p);
        for (int i = 0; i < 6; i++)
            merlon((float)i * 60 + 12, (float)i * 60 + 48, 0.15f, 0.235f, 0.72f, 0.84f);
        break;
    case KNIGHT:
        curve(&p, neck - 0.04f, 0.17f, 0.20f, 0.205f, 4);
        pt(&p, 0.20f, 0.235f, 1);
        pt(&p, 0, 0.235f, 1);
        lathe(&p);
        knight_head();
        break;
    case BISHOP:
        curve(&p, 0.09f, 0.25f, 0.09f, 0.55f, 6);
        bead(&p, 0.15f, 0.55f, 0.019f);
        pt(&p, 0.115f, 0.59f, 1);
        bead(&p, 0.125f, 0.59f, 0.013f);
        pt(&p, 0.085f, 0.62f, 1);
        carving = 1;
        curve(&p, 0.205f, 0.715f, 0.03f, 0.905f, 14);
        carving = 0;
        pt(&p, 0.03f, 0.905f, 1);
        arc(&p, 0, 0.935f, 0.035f, -50, 90, 6);
        lathe(&p);
        break;
    case QUEEN:
        curve(&p, 0.10f, 0.35f, 0.105f, 0.74f, 6);
        bead(&p, 0.17f, 0.74f, 0.022f);
        pt(&p, 0.13f, 0.787f, 1);
        bead(&p, 0.14f, 0.787f, 0.015f);
        pt(&p, 0.10f, 0.82f, 1);
        curve(&p, 0.12f, 0.93f, 0.205f, 1.02f, 6);
        pt(&p, 0.205f, 1.045f, 1);
        pt(&p, 0.165f, 1.045f, 1);
        curve(&p, 0.16f, 1.12f, 0.03f, 1.13f, 6);
        pt(&p, 0.03f, 1.13f, 1);
        arc(&p, 0, 1.165f, 0.04f, -40, 90, 6);
        lathe(&p);
        for (int i = 0; i < 9; i++) {
            float a = (float)i * 40 * DEG;
            ball(0.19f * cosf(a), 1.06f, 0.19f * sinf(a), 0.028f);
        }
        break;
    case KING:
        curve(&p, 0.11f, 0.40f, 0.115f, 0.82f, 6);
        bead(&p, 0.18f, 0.82f, 0.022f);
        pt(&p, 0.14f, 0.867f, 1);
        bead(&p, 0.15f, 0.867f, 0.015f);
        pt(&p, 0.11f, 0.90f, 1);
        curve(&p, 0.13f, 1.00f, 0.20f, 1.08f, 6);
        pt(&p, 0.20f, 1.10f, 1);
        pt(&p, 0.17f, 1.10f, 1);
        curve(&p, 0.17f, 1.17f, 0.04f, 1.18f, 6);
        pt(&p, 0.04f, 1.18f, 1);
        pt(&p, 0, 1.18f, 0);
        lathe(&p);
        box(-0.03f, 0.03f, 1.16f, 1.40f, -0.026f, 0.026f);
        box(-0.095f, 0.095f, 1.26f, 1.315f, -0.022f, 0.022f);
        break;
    }
}

/* ---- the board ---- */

static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* Smooth noise from 0 to 1 on a lattice that repeats every period cells. */
static float noise(float x, float y, int period)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - (float)ix, fy = y - (float)iy;
    fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
    float v[4];
    for (int k = 0; k < 4; k++)
        v[k] = (float)(hash2(((ix + (k & 1)) % period + period) % period,
                             ((iy + (k >> 1)) % period + period) % period) & 0xFFFF) / 65535.0f;
    return (v[0] * (1 - fx) + v[1] * fx) * (1 - fy) + (v[2] * (1 - fx) + v[3] * fx) * fy;
}

/* A gray wood grain, for the lit colors to tint: lines along s that wander. It has no fine
 * detail, so that it needs no mipmaps (which cost the software renderer dearly) to be
 * drawn small without shimmering. */
static void make_wood(void)
{
    enum { N = 128 };
    static uint8_t texels[N * N * 3];
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            float u = (float)x / N, v = (float)y / N;
            float wander = noise(u * 2, v * 4, 2) * 0.7f + noise(u * 4, v * 8, 4) * 0.3f;
            float g = v * 14 + wander * 1.1f;
            g -= floorf(g);
            float val = 0.84f + 0.06f * sinf(g * 2 * (float)M_PI) + 0.09f * powf(1 - g, 8) +
                        0.08f * (noise(u * 8, v * 32, 8) - 0.5f);
            int c = (int)(val * 255);
            c = c < 0 ? 0 : c > 255 ? 255 : c;
            uint8_t *p = texels + (y * N + x) * 3;
            p[0] = p[1] = p[2] = (uint8_t)c;
        }
    }
    glGenTextures(1, &wood_tex);
    glBindTexture(GL_TEXTURE_2D, wood_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, N, N, 0, GL_RGB, GL_UNSIGNED_BYTE, texels);
}

static const GLfloat light_wood[4] = { 0.93f, 0.78f, 0.56f, 1 };
static const GLfloat dark_wood[4] = { 0.40f, 0.22f, 0.11f, 1 };
static const GLfloat frame_wood[4] = { 0.30f, 0.15f, 0.07f, 1 };
static const GLfloat inlay_wood[4] = { 0.85f, 0.66f, 0.36f, 1 };

static void wood_vertex(float x, float y, float z, float s, float t)
{
    glTexCoord2f(s, t);
    glVertex3f(x, y, z);
}

/* A strip of the frame around one side, from the inner edge a to the outer b, at the
 * heights ya and yb, its corners mitred; turn quarter turns from the front. */
static void frame_band(int turn, float a, float ya, float b, float yb, const float *n_front)
{
    float c[4][3] = { { -a, ya, a }, { a, ya, a }, { b, yb, b }, { -b, yb, b } };
    float n[3] = { n_front[0], n_front[1], n_front[2] };
    for (int t = 0; t < turn; t++) {
        for (int k = 0; k < 4; k++) {
            float x = c[k][0];
            c[k][0] = c[k][2], c[k][2] = -x;
        }
        float x = n[0];
        n[0] = n[2], n[2] = -x;
    }
    float e1[3], e2[3], f[3];
    sub3(e1, c[1], c[0]);
    sub3(e2, c[2], c[0]);
    cross3(f, e1, e2);
    int flip = dot3(f, n) < 0;
    glNormal3fv(n);
    for (int k = 0; k < 4; k++) {
        int i = flip ? 3 - k : k;
        /* the grain runs along the strip */
        float along = turn & 1 ? c[i][2] : c[i][0], across = turn & 1 ? c[i][0] : c[i][2];
        wood_vertex(c[i][0], c[i][1], c[i][2], along * 0.35f + (float)turn * 0.3f, across * 0.6f);
    }
}

static void build_frame(void)
{
    static const float up[3] = { 0, 1, 0 }, front[3] = { 0, 0, 1 }, down[3] = { 0, -1, 0 };
    float bevel[3] = { 0, 0.8f, 0.6f };
    float in = BOARD_IN, inlay = BOARD_IN + 0.07f, top = BOARD_IN + FRAME_W, out = top + 0.12f;
    frame_list = glGenLists(1);
    glNewList(frame_list, GL_COMPILE);
    glBegin(GL_QUADS);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, inlay_wood);
    for (int t = 0; t < 4; t++)
        frame_band(t, in, 0, inlay, 0, up);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, frame_wood);
    for (int t = 0; t < 4; t++) {
        frame_band(t, inlay, 0, top, 0, up);
        frame_band(t, top, 0, out, -0.09f, bevel);
    }
    glEnd();
    /* the sides and the underneath */
    glBegin(GL_QUADS);
    for (int t = 0; t < 4; t++) {
        float n[3] = { front[0], front[1], front[2] };
        float p[4][3] = { { -out, -0.09f, out }, { out, -0.09f, out }, { out, -BOARD_D, out }, { -out, -BOARD_D, out } };
        for (int k = 0; k < t; k++) {
            for (int i = 0; i < 4; i++) {
                float x = p[i][0];
                p[i][0] = p[i][2], p[i][2] = -x;
            }
            float x = n[0];
            n[0] = n[2], n[2] = -x;
        }
        glNormal3fv(n);
        float e1[3], e2[3], f[3];
        sub3(e1, p[1], p[0]);
        sub3(e2, p[2], p[0]);
        cross3(f, e1, e2);
        int flip = dot3(f, n) < 0;
        for (int k = 0; k < 4; k++) {
            int i = flip ? 3 - k : k;
            wood_vertex(p[i][0], p[i][1], p[i][2], (p[i][0] + p[i][2]) * 0.35f, p[i][1] * 0.6f);
        }
    }
    glNormal3fv(down);
    wood_vertex(-out, -BOARD_D, -out, 0, 0);
    wood_vertex(out, -BOARD_D, -out, 1, 0);
    wood_vertex(out, -BOARD_D, out, 1, 1);
    wood_vertex(-out, -BOARD_D, out, 0, 1);
    glEnd();
    glEndList();
}

/* The 64 squares at y = 0, lit and grained, with alpha for blending. */
static void draw_squares(float alpha)
{
    GLfloat lc[4] = { light_wood[0], light_wood[1], light_wood[2], alpha };
    GLfloat dc[4] = { dark_wood[0], dark_wood[1], dark_wood[2], alpha };
    for (int pass = 0; pass < 2; pass++) {
        glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, pass ? dc : lc);
        glBegin(GL_QUADS);
        glNormal3f(0, 1, 0);
        for (int sq = 0; sq < 64; sq++) {
            int dark = ((FILE_OF(sq) + RANK_OF(sq)) & 1) == 0;
            if (dark != pass)
                continue;
            float x, z;
            chessview_square(sq, &x, &z);
            /* each square from its own part of the grain, alternate ones turned */
            uint32_t h = hash2(sq, 7);
            float s0 = (float)(h & 255) / 64.0f, t0 = (float)((h >> 8) & 255) / 64.0f;
            int turned = sq & 1;
            for (int k = 0; k < 4; k++) {
                float dx = (k == 1 || k == 2) ? 0.5f : -0.5f, dz = k >= 2 ? -0.5f : 0.5f;
                float s = turned ? dz : dx, t = turned ? dx : dz;
                wood_vertex(x + dx, 0, z + dz, s0 + s * 0.45f, t0 + t * 0.8f);
            }
        }
        glEnd();
    }
}

/* ---- marks and shadows ---- */

static void draw_marks(const uint8_t *mark)
{
    static const struct {
        int bit;
        float r, g, b, a, fill;
    } kinds[] = {
        { MARK_LAST, 1.0f, 0.84f, 0.30f, 0.50f, 0.30f },
        { MARK_HOVER, 1.0f, 1.0f, 1.0f, 0.35f, 0.12f },
        { MARK_MOVE, 0.35f, 0.90f, 0.40f, 0.75f, 0.30f },
        { MARK_CAPTURE, 1.0f, 0.42f, 0.18f, 0.85f, 0.35f },
        { MARK_SELECTED, 0.30f, 0.72f, 1.0f, 0.90f, 0.40f },
        { MARK_CHECK, 1.0f, 0.12f, 0.08f, 0.95f, 0.50f },
    };
    const float e = 0.5f, in = 0.30f, y = 0.002f;
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        for (int sq = 0; sq < 64; sq++) {
            if (!(mark[sq] & kinds[k].bit))
                continue;
            float x, z;
            chessview_square(sq, &x, &z);
            float r = kinds[k].r, g = kinds[k].g, b = kinds[k].b, a = kinds[k].a, f = a * kinds[k].fill;
            /* a glow bright at the edges, fading in toward a faint middle */
            static const float ox[4] = { -1, 1, 1, -1 }, oz[4] = { 1, 1, -1, -1 };
            glBegin(GL_QUADS);
            for (int s = 0; s < 4; s++) {
                int t = (s + 1) & 3;
                glColor4f(r, g, b, a);
                glVertex3f(x + ox[s] * e, y, z + oz[s] * e);
                glVertex3f(x + ox[t] * e, y, z + oz[t] * e);
                glColor4f(r, g, b, f);
                glVertex3f(x + ox[t] * in, y, z + oz[t] * in);
                glVertex3f(x + ox[s] * in, y, z + oz[s] * in);
            }
            glColor4f(r, g, b, f);
            for (int s = 0; s < 4; s++)
                glVertex3f(x + ox[s] * in, y, z + oz[s] * in);
            glEnd();
        }
    }
}

static void draw_shadow(const struct chess_figure *f)
{
    enum { SEGS = 18 };
    float lift = f->y > 0 ? f->y : 0;
    float rad = radii[PTYPE(f->piece)] * (1.25f + lift * 0.6f), a = 0.42f / (1 + lift * 2.5f);
    float cx = f->x + 0.05f + lift * 0.25f, cz = f->z - 0.06f - lift * 0.3f;
    glBegin(GL_TRIANGLE_FAN);
    glColor4f(0, 0, 0, a);
    glVertex3f(cx, 0.003f, cz);
    glColor4f(0, 0, 0, 0);
    for (int i = 0; i <= SEGS; i++) {
        float t = (float)i / SEGS * 2 * (float)M_PI;
        glVertex3f(cx + rad * cosf(t), 0.003f, cz - rad * sinf(t));
    }
    glEnd();
}

/* ---- the camera ---- */

static void mat_mul(float *o, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, sizeof(r));
}

static int mat_invert(float *o, const float *m)
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-12f)
        return -1;
    for (int i = 0; i < 16; i++)
        o[i] = inv[i] / det;
    return 0;
}

/* The projection and view matrices for the scene's camera (column major, as GL keeps them). */
static void camera(const struct chess_scene *sc, float *proj, float *view)
{
    float aspect = (float)sc->w / (float)(sc->h > 0 ? sc->h : 1);
    /* keep the board's width in view in a narrow window */
    float t = tanf(16.0f * DEG) * (aspect < 1.25f ? 1.25f / aspect : 1), n = 2, f = 80;
    memset(proj, 0, 16 * sizeof(float));
    proj[0] = 1 / (t * aspect);
    proj[5] = 1 / t;
    proj[10] = (f + n) / (n - f);
    proj[11] = -1;
    proj[14] = 2 * f * n / (n - f);

    float yaw = sc->yaw * DEG, pitch = sc->pitch * DEG;
    float target[3] = { 0, 0.15f, 0 };
    float eye[3] = { target[0] + sc->dist * sinf(yaw) * cosf(pitch), target[1] + sc->dist * sinf(pitch),
                     target[2] + sc->dist * cosf(yaw) * cosf(pitch) };
    float fw[3], up[3] = { 0, 1, 0 }, side[3], u[3];
    sub3(fw, target, eye);
    norm3(fw);
    cross3(side, fw, up);
    norm3(side);
    cross3(u, side, fw);
    float m[16] = { side[0], u[0], -fw[0], 0, side[1], u[1], -fw[1], 0,
                    side[2], u[2], -fw[2], 0, 0, 0, 0, 1 };
    m[12] = -dot3(side, eye);
    m[13] = -dot3(u, eye);
    m[14] = dot3(fw, eye);
    memcpy(view, m, sizeof(m));
}

void chessview_ray(const struct chess_scene *sc, int x, int y, float org[3], float dir[3])
{
    float proj[16], view[16], pv[16], inv[16];
    camera(sc, proj, view);
    mat_mul(pv, proj, view);
    if (mat_invert(inv, pv) < 0) {
        org[0] = org[1] = org[2] = 0;
        dir[0] = dir[2] = 0, dir[1] = -1;
        return;
    }
    float nx = 2 * ((float)x + 0.5f) / (float)sc->w - 1, ny = 1 - 2 * ((float)y + 0.5f) / (float)sc->h;
    float pts[2][3];
    for (int k = 0; k < 2; k++) {
        float nz = k ? 1.0f : -1.0f, v[4];
        for (int i = 0; i < 4; i++)
            v[i] = inv[i] * nx + inv[4 + i] * ny + inv[8 + i] * nz + inv[12 + i];
        pts[k][0] = v[0] / v[3], pts[k][1] = v[1] / v[3], pts[k][2] = v[2] / v[3];
    }
    memcpy(org, pts[0], sizeof(pts[0]));
    sub3(dir, pts[1], pts[0]);
    norm3(dir);
}

/* ---- drawing ---- */

static void set_lights(void)
{
    static const GLfloat key[4] = { -0.45f, 1.0f, 0.55f, 0 }, fill[4] = { 0.6f, 0.45f, -0.7f, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, key);
    glLightfv(GL_LIGHT1, GL_POSITION, fill);
}

int chessview_init(void)
{
    static const GLfloat white[4] = { 1, 1, 1, 1 }, key_c[4] = { 0.88f, 0.86f, 0.82f, 1 };
    static const GLfloat fill_c[4] = { 0.32f, 0.34f, 0.40f, 1 }, none[4] = { 0, 0, 0, 1 };
    static const GLfloat ambient[4] = { 0.30f, 0.30f, 0.32f, 1 };
    glLightfv(GL_LIGHT0, GL_DIFFUSE, key_c);
    glLightfv(GL_LIGHT0, GL_SPECULAR, white);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, fill_c);
    glLightfv(GL_LIGHT1, GL_SPECULAR, none);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    glShadeModel(GL_SMOOTH);
    glCullFace(GL_BACK);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    make_wood();
    for (lod = 0; lod < 2; lod++) {
        for (int t = PAWN; t <= KING; t++) {
            GLuint list = glGenLists(1);
            glNewList(list, GL_COMPILE);
            build_piece(t);
            glEndList();
            *(lod ? &reflected_list[t] : &piece_list[t]) = list;
        }
    }
    lod = 0;
    build_frame();
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

void chessview_backdrop(uint32_t *px, int stride, const uint32_t *back, int back_stride, int w, int h)
{
    for (int y = 0; y < h; y++) {
        uint32_t *p = px + (size_t)y * (size_t)stride;
        const uint32_t *q = back + (size_t)y * (size_t)back_stride;
        for (int x = 0; x < w; x++)
            if ((p[x] & 0xFFFFFF) == CHESSVIEW_KEY)
                p[x] = q[x];
    }
}

static void draw_figures(const struct chess_scene *sc, const GLuint *lists)
{
    static const GLfloat ivory[4] = { 0.93f, 0.91f, 0.86f, 1 }, ebony[4] = { 0.13f, 0.12f, 0.12f, 1 };
    static const GLfloat shine_w[4] = { 0.75f, 0.75f, 0.75f, 1 }, shine_b[4] = { 0.85f, 0.85f, 0.88f, 1 };
    glEnable(GL_LIGHTING);
    glEnable(GL_CULL_FACE);
    for (int i = 0; i < sc->nfig; i++) {
        const struct chess_figure *f = &sc->fig[i];
        int type = PTYPE(f->piece), black = PCOLOR(f->piece) == BLACK;
        glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, black ? ebony : ivory);
        glMaterialfv(GL_FRONT, GL_SPECULAR, black ? shine_b : shine_w);
        glMaterialf(GL_FRONT, GL_SHININESS, black ? 70 : 45);
        glPushMatrix();
        glTranslatef(f->x, f->y, f->z);
        if (type == KNIGHT)
            glRotatef(black ? 90 : -90, 0, 1, 0);   /* in profile to its own side */
        else if (type == BISHOP)
            glRotatef(black ? 135 : -45, 0, 1, 0);  /* the slit toward the front */
        glCallList(lists[type]);
        glPopMatrix();
    }
    static const GLfloat none[4] = { 0, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_SPECULAR, none);
    glDisable(GL_CULL_FACE);
}

void chessview_draw(const struct chess_scene *sc)
{
    float proj[16], view[16];
    camera(sc, proj, view);
    glViewport(0, 0, sc->w, sc->h);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glClearColor((CHESSVIEW_KEY >> 16) / 255.0f, (CHESSVIEW_KEY >> 8 & 0xFF) / 255.0f, (CHESSVIEW_KEY & 0xFF) / 255.0f, 1);
    glClearDepth(sc->reflections ? 0 : 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view);
    set_lights();
    glEnable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glBindTexture(GL_TEXTURE_2D, wood_tex);

    if (sc->reflections) {
        /* the squares, solid, at the far depth: all that lets the reflection through (and
         * left without their grain, which little of shows through the grained ones on top) */
        glDepthFunc(GL_ALWAYS);
        glDepthRange(1, 1);
        draw_squares(1);
        glDepthRange(0, 1);
        glDepthFunc(GL_LESS);

        static const GLdouble below[4] = { 0, -1, 0, 0.001 };
        glClipPlane(GL_CLIP_PLANE0, below);
        glEnable(GL_CLIP_PLANE0);
        glPushMatrix();
        glScalef(1, -1, 1);
        set_lights();
        glFrontFace(GL_CW);
        draw_figures(sc, reflected_list);
        glFrontFace(GL_CCW);
        glPopMatrix();
        glDisable(GL_CLIP_PLANE0);
        set_lights();

        glClearDepth(1);
        glClear(GL_DEPTH_BUFFER_BIT);
    }

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_CULL_FACE);
    glCallList(frame_list);
    glDisable(GL_CULL_FACE);
    if (sc->reflections)
        glEnable(GL_BLEND);
    draw_squares(sc->reflections ? SQUARE_ALPHA : 1);
    glDisable(GL_TEXTURE_2D);

    /* marks and shadows lie on the squares: nothing but the pieces comes in front of them */
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    draw_marks(sc->mark);
    for (int i = 0; i < sc->nfig; i++)
        if (sc->fig[i].y > -0.05f)
            draw_shadow(&sc->fig[i]);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    draw_figures(sc, piece_list);
    glDisable(GL_LIGHTING);
}
