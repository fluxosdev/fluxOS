/*
 * The vertex pipeline. Each glVertex is transformed to eye coordinates,
 * lit there, and projected to clip coordinates; glEnd assembles the
 * vertices into polygons, lines and points. Polygons are clipped against
 * the view volume (Sutherland-Hodgman, in homogeneous coordinates),
 * divided by w, mapped through the viewport, culled by their winding and
 * handed to the backend. Lines and points become small polygons too, so
 * a backend only ever draws convex polygons.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "glint.h"

#define MAX_POLY 64                     /* vertices of a polygon, clipping included */
#define NPLANES  7

#define GET_CTX(c)                      \
    struct fgl_context *c = gl_ctx;     \
    if (!c)                             \
        return

void gl_prim_init(struct fgl_context *c)
{
    c->prim = GL_POINTS;
    c->in_begin = 0;
    c->nverts = 0;
}

/* ---- lighting ---- */

void gl_update_normal_matrix(struct fgl_context *c)
{
    const float *m = c->modelview[c->mv_depth];
    /* The inverse transpose of the upper 3x3 is its cofactor matrix over its
     * determinant. Rows of that 3x3: (a b d), (e f g), (h i k). */
    float a = m[0], b = m[4], d = m[8];
    float e = m[1], f = m[5], g = m[9];
    float h = m[2], i = m[6], k = m[10];
    float c00 = f * k - g * i, c01 = -(e * k - g * h), c02 = e * i - f * h;
    float c10 = -(b * k - d * i), c11 = a * k - d * h, c12 = -(a * i - b * h);
    float c20 = b * g - d * f, c21 = -(a * g - d * e), c22 = a * f - b * e;
    float det = a * c00 + b * c01 + d * c02;
    float inv = det != 0 ? 1 / det : 0;
    float *n = c->normal_matrix;        /* row major: eye normal = n * normal */
    n[0] = c00 * inv; n[1] = c01 * inv; n[2] = c02 * inv;
    n[3] = c10 * inv; n[4] = c11 * inv; n[5] = c12 * inv;
    n[6] = c20 * inv; n[7] = c21 * inv; n[8] = c22 * inv;
    c->normal_dirty = 0;
}

static void eye_normal(struct fgl_context *c, float *out)
{
    if (!c->eye_normal_valid) {
        if (c->normal_dirty)
            gl_update_normal_matrix(c);
        const float *n = c->normal_matrix, *v = c->normal;
        float *en = c->eye_normal;
        en[0] = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
        en[1] = n[3] * v[0] + n[4] * v[1] + n[5] * v[2];
        en[2] = n[6] * v[0] + n[7] * v[1] + n[8] * v[2];
        if (c->normalize) {
            float len = sqrtf(en[0] * en[0] + en[1] * en[1] + en[2] * en[2]);
            if (len > 0) {
                en[0] /= len;
                en[1] /= len;
                en[2] /= len;
            }
        }
        c->eye_normal_valid = 1;
    }
    memcpy(out, c->eye_normal, 3 * sizeof(float));
}

/* Colors are worked out as SSE vectors, red, green and blue in the first
 * three lanes; the geometry stays scalar. */
static void light_vertex(struct fgl_context *c, const float *eye, float *out)
{
    const struct material *m = &c->mat;
    float n[3];
    eye_normal(c, n);
    __m128 m_ambient = _mm_load_ps(m->ambient), m_diffuse = _mm_load_ps(m->diffuse);
    __m128 m_specular = _mm_load_ps(m->specular);
    __m128 color = _mm_add_ps(_mm_load_ps(m->emission),
                              _mm_mul_ps(m_ambient, _mm_load_ps(c->model_ambient)));
    int specular = m->specular[0] != 0 || m->specular[1] != 0 || m->specular[2] != 0;

    for (int i = 0; i < MAX_LIGHTS; i++) {
        if (!c->light_on[i])
            continue;
        const struct light *l = &c->lights[i];
        float L[3], H[3], att = 1;
        if (l->position[3] == 0) {
            memcpy(L, l->dir, sizeof(L));
            memcpy(H, l->half, sizeof(H));
        } else {
            float w = l->position[3];
            L[0] = l->position[0] / w - eye[0] / eye[3];
            L[1] = l->position[1] / w - eye[1] / eye[3];
            L[2] = l->position[2] / w - eye[2] / eye[3];
            float d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            if (d > 0) {
                float inv = 1 / d;
                L[0] *= inv;
                L[1] *= inv;
                L[2] *= inv;
            }
            float k = l->att[0] + l->att[1] * d + l->att[2] * d * d;
            att = k > 0 ? 1 / k : 1;
            if (l->spot_cutoff != 180) {
                float cs = -(L[0] * l->spot_dir[0] + L[1] * l->spot_dir[1] + L[2] * l->spot_dir[2]);
                if (cs < l->spot_cos)
                    continue;           /* outside the cone: not even ambient */
                if (l->spot_exp != 0)
                    att *= powf(cs, l->spot_exp);
            }
            H[0] = L[0];
            H[1] = L[1];
            H[2] = L[2] + 1;
            float hl = sqrtf(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]);
            if (hl > 0) {
                float inv = 1 / hl;
                H[0] *= inv;
                H[1] *= inv;
                H[2] *= inv;
            }
        }
        __m128 lit = _mm_mul_ps(m_ambient, _mm_load_ps(l->ambient));
        float nl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        if (nl > 0) {
            lit = _mm_add_ps(lit, _mm_mul_ps(_mm_set1_ps(nl), _mm_mul_ps(m_diffuse, _mm_load_ps(l->diffuse))));
            float nh = n[0] * H[0] + n[1] * H[1] + n[2] * H[2];
            if (specular && nh > 0) {
                float s = m->shininess == 0 ? 1 : powf(nh, m->shininess);
                lit = _mm_add_ps(lit, _mm_mul_ps(_mm_set1_ps(s),
                                                 _mm_mul_ps(m_specular, _mm_load_ps(l->specular))));
            }
        }
        color = _mm_add_ps(color, _mm_mul_ps(_mm_set1_ps(att), lit));
    }
    color = _mm_min_ps(_mm_max_ps(color, _mm_setzero_ps()), _mm_set1_ps(1));
    _mm_store_ps(out, color);
    out[3] = gl_clampf(m->diffuse[3]);
}

/* ---- vertex attributes ---- */

void glNormal3f(GLfloat x, GLfloat y, GLfloat z)
{
    GET_CTX(c);
    RECORD(c, OP_NORMAL, 3, x, y, z);
    c->normal[0] = x;
    c->normal[1] = y;
    c->normal[2] = z;
    c->eye_normal_valid = 0;
}

void glNormal3d(GLdouble x, GLdouble y, GLdouble z) { glNormal3f((float)x, (float)y, (float)z); }
void glNormal3fv(const GLfloat *v) { glNormal3f(v[0], v[1], v[2]); }

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    GET_CTX(c);
    RECORD(c, OP_COLOR, 4, r, g, b, a);
    c->color[0] = gl_clampf(r);
    c->color[1] = gl_clampf(g);
    c->color[2] = gl_clampf(b);
    c->color[3] = gl_clampf(a);
    if (c->color_material && c->cm_face != GL_BACK) {
        struct material *m = &c->mat;
        switch (c->cm_mode) {
        case GL_AMBIENT: memcpy(m->ambient, c->color, sizeof(c->color)); break;
        case GL_DIFFUSE: memcpy(m->diffuse, c->color, sizeof(c->color)); break;
        case GL_SPECULAR: memcpy(m->specular, c->color, sizeof(c->color)); break;
        case GL_EMISSION: memcpy(m->emission, c->color, sizeof(c->color)); break;
        default:
            memcpy(m->ambient, c->color, sizeof(c->color));
            memcpy(m->diffuse, c->color, sizeof(c->color));
            break;
        }
    }
}

void glColor4fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], v[3]); }
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(r, g, b, 1); }
void glColor3d(GLdouble r, GLdouble g, GLdouble b) { glColor4f((float)r, (float)g, (float)b, 1); }
void glColor3fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], 1); }

void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
    glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

void glColor3ub(GLubyte r, GLubyte g, GLubyte b) { glColor4ub(r, g, b, 255); }
void glColor3ubv(const GLubyte *v) { glColor4ub(v[0], v[1], v[2], 255); }

/* ---- vertices ---- */

void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    GET_CTX(c);
    RECORD(c, OP_VERTEX, 4, x, y, z, w);
    if (!c->in_begin)
        return;                         /* undefined outside glBegin; ignore it */
    if (c->nverts == c->vcap) {
        int cap = c->vcap ? c->vcap * 2 : 256;
        struct cvert *nv = realloc(c->verts, (size_t)cap * sizeof(*nv));
        if (!nv) {
            gl_error(c, GL_OUT_OF_MEMORY);
            return;
        }
        c->verts = nv;
        c->vcap = cap;
    }
    struct cvert *v = &c->verts[c->nverts++];
    const float *mv = c->modelview[c->mv_depth], *p = c->projection[c->proj_depth];
    /* The modelview's columns weighted by x, y, z and w, then the projection's
     * by the eye coordinates. */
    float eye[4] VEC4;
    __m128 e = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_load_ps(mv), _mm_set1_ps(x)),
                                                _mm_mul_ps(_mm_load_ps(mv + 4), _mm_set1_ps(y))),
                                     _mm_mul_ps(_mm_load_ps(mv + 8), _mm_set1_ps(z))),
                          _mm_mul_ps(_mm_load_ps(mv + 12), _mm_set1_ps(w)));
    _mm_store_ps(eye, e);
    __m128 clip = _mm_add_ps(_mm_add_ps(_mm_add_ps(
                      _mm_mul_ps(_mm_load_ps(p), _mm_shuffle_ps(e, e, 0x00)),
                      _mm_mul_ps(_mm_load_ps(p + 4), _mm_shuffle_ps(e, e, 0x55))),
                      _mm_mul_ps(_mm_load_ps(p + 8), _mm_shuffle_ps(e, e, 0xAA))),
                      _mm_mul_ps(_mm_load_ps(p + 12), _mm_shuffle_ps(e, e, 0xFF)));
    _mm_store_ps(v->clip, clip);
    if (c->lighting)
        light_vertex(c, eye, v->color);
    else
        _mm_store_ps(v->color, _mm_load_ps(c->color));
}

void glVertex2f(GLfloat x, GLfloat y) { glVertex4f(x, y, 0, 1); }
void glVertex2i(GLint x, GLint y) { glVertex4f((float)x, (float)y, 0, 1); }
void glVertex2d(GLdouble x, GLdouble y) { glVertex4f((float)x, (float)y, 0, 1); }
void glVertex2fv(const GLfloat *v) { glVertex4f(v[0], v[1], 0, 1); }
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { glVertex4f(x, y, z, 1); }
void glVertex3i(GLint x, GLint y, GLint z) { glVertex4f((float)x, (float)y, (float)z, 1); }
void glVertex3d(GLdouble x, GLdouble y, GLdouble z) { glVertex4f((float)x, (float)y, (float)z, 1); }
void glVertex3fv(const GLfloat *v) { glVertex4f(v[0], v[1], v[2], 1); }
void glVertex3dv(const GLdouble *v) { glVertex4f((float)v[0], (float)v[1], (float)v[2], 1); }
void glVertex4fv(const GLfloat *v) { glVertex4f(v[0], v[1], v[2], v[3]); }

/* ---- clipping and drawing ---- */

/* Signed distance to clip plane k; inside when >= 0. The last plane keeps w positive. */
static inline float plane_dist(const float *v, int k)
{
    switch (k) {
    case 0: return v[3] + v[0];
    case 1: return v[3] - v[0];
    case 2: return v[3] + v[1];
    case 3: return v[3] - v[1];
    case 4: return v[3] + v[2];
    case 5: return v[3] - v[2];
    default: return v[3] - 1e-5f;
    }
}

/* Bit k set when the vertex is outside plane k. w + (x, y, z) and
 * w - (x, y, z) are compared with 0 in one go each. */
static int outcode(const float *v)
{
    __m128 p = _mm_load_ps(v), w = _mm_shuffle_ps(p, p, 0xFF), zero = _mm_setzero_ps();
    int plus = _mm_movemask_ps(_mm_cmplt_ps(_mm_add_ps(w, p), zero));     /* planes 0, 2, 4 */
    int minus = _mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(w, p), zero));    /* planes 1, 3, 5 */
    int code = (plus & 1) | (minus & 1) << 1 | (plus & 2) << 1 | (minus & 2) << 2 |
               (plus & 4) << 2 | (minus & 4) << 3;
    if (plane_dist(v, 6) < 0)
        code |= 1 << 6;
    return code;
}

static void lerp(struct cvert *out, const struct cvert *a, const struct cvert *b, float t)
{
    __m128 tt = _mm_set1_ps(t);
    __m128 ac = _mm_load_ps(a->clip), bc = _mm_load_ps(b->clip);
    __m128 acol = _mm_load_ps(a->color), bcol = _mm_load_ps(b->color);
    _mm_store_ps(out->clip, _mm_add_ps(ac, _mm_mul_ps(_mm_sub_ps(bc, ac), tt)));
    _mm_store_ps(out->color, _mm_add_ps(acol, _mm_mul_ps(_mm_sub_ps(bcol, acol), tt)));
}

/* Clip the polygon in place against every plane its vertices cross; returns its size. */
static int clip_polygon(struct cvert *poly, int n, int planes)
{
    struct cvert tmp[MAX_POLY];
    for (int k = 0; k < NPLANES && n >= 3; k++) {
        if (!(planes & (1 << k)))
            continue;
        int m = 0;
        for (int i = 0; i < n; i++) {
            const struct cvert *a = &poly[i], *b = &poly[(i + 1) % n];
            float da = plane_dist(a->clip, k), db = plane_dist(b->clip, k);
            if (da >= 0 && m < MAX_POLY)
                tmp[m++] = *a;
            if ((da >= 0) != (db >= 0) && m < MAX_POLY)
                lerp(&tmp[m++], a, b, da / (da - db));
        }
        memcpy(poly, tmp, (size_t)m * sizeof(*poly));
        n = m;
    }
    return n;
}

/* Clip coordinates to the backend's window coordinates (top-left origin). */
static void to_window(struct fgl_context *c, const struct cvert *v, struct wvert *w)
{
    float iw = 1 / v->clip[3];
    float x = v->clip[0] * iw, y = v->clip[1] * iw, z = v->clip[2] * iw;
    w->x = c->vp_x + (x + 1) * c->vp_w * 0.5f;
    w->y = c->height - (c->vp_y + (y + 1) * c->vp_h * 0.5f);
    w->z = gl_clampf(c->depth_near + (z + 1) * 0.5f * (c->depth_far - c->depth_near));
    w->r = v->color[0];
    w->g = v->color[1];
    w->b = v->color[2];
    w->a = v->color[3];
}

static void draw_polygon(struct fgl_context *c, const int *idx, int n, int provoking)
{
    if (n < 3 || n > MAX_POLY / 2)
        return;
    struct cvert poly[MAX_POLY];
    int all = 0x7F, any = 0;
    for (int i = 0; i < n; i++) {
        poly[i] = c->verts[idx[i]];
        int code = outcode(poly[i].clip);
        all &= code;
        any |= code;
    }
    if (all)
        return;                         /* entirely outside one plane */
    if (c->shade_model == GL_FLAT)
        for (int i = 0; i < n; i++)
            memcpy(poly[i].color, c->verts[provoking].color, sizeof(poly[i].color));
    if (any)
        n = clip_polygon(poly, n, any);
    if (n < 3)
        return;

    struct wvert w[MAX_POLY];
    for (int i = 0; i < n; i++)
        to_window(c, &poly[i], &w[i]);
    /* Twice the signed area with y up: positive means counterclockwise. */
    float area = 0;
    for (int i = 0; i < n; i++) {
        const struct wvert *a = &w[i], *b = &w[(i + 1) % n];
        area += a->x * -b->y - b->x * -a->y;
    }
    if (area == 0)
        return;
    if (c->cull) {
        int front = (area > 0) == (c->front_face == GL_CCW);
        if (c->cull_face == GL_FRONT_AND_BACK || (front ? c->cull_face == GL_FRONT : c->cull_face == GL_BACK))
            return;
    }
    c->be->polygon(c, w, n);
}

static void draw_line(struct fgl_context *c, int i0, int i1)
{
    struct cvert a = c->verts[i0], b = c->verts[i1];
    if (c->shade_model == GL_FLAT)
        memcpy(a.color, b.color, sizeof(a.color));
    /* Liang-Barsky against each plane. */
    float t0 = 0, t1 = 1;
    for (int k = 0; k < NPLANES; k++) {
        float da = plane_dist(a.clip, k), db = plane_dist(b.clip, k);
        if (da < 0 && db < 0)
            return;
        if (da < 0) {
            float t = da / (da - db);
            if (t > t0)
                t0 = t;
        } else if (db < 0) {
            float t = da / (da - db);
            if (t < t1)
                t1 = t;
        }
    }
    if (t0 >= t1)
        return;
    struct cvert ca, cb;
    lerp(&ca, &a, &b, t0);
    lerp(&cb, &a, &b, t1);
    struct wvert wa, wb, q[4];
    to_window(c, &ca, &wa);
    to_window(c, &cb, &wb);
    /* A line is a thin quad: x-major lines widen vertically, y-major ones horizontally. */
    float half = c->line_width * 0.5f, dx = wb.x - wa.x, dy = wb.y - wa.y;
    float ox = 0, oy = 0;
    if (fabsf(dx) >= fabsf(dy))
        oy = half;
    else
        ox = half;
    q[0] = wa; q[0].x -= ox; q[0].y -= oy;
    q[1] = wb; q[1].x -= ox; q[1].y -= oy;
    q[2] = wb; q[2].x += ox; q[2].y += oy;
    q[3] = wa; q[3].x += ox; q[3].y += oy;
    c->be->polygon(c, q, 4);
}

static void draw_point(struct fgl_context *c, int i)
{
    const struct cvert *v = &c->verts[i];
    if (outcode(v->clip))
        return;
    struct wvert w, q[4];
    to_window(c, v, &w);
    float half = c->point_size * 0.5f;
    for (int k = 0; k < 4; k++) {
        q[k] = w;
        q[k].x += (k == 1 || k == 2) ? half : -half;
        q[k].y += (k >= 2) ? half : -half;
    }
    c->be->polygon(c, q, 4);
}

void glBegin(GLenum mode)
{
    GET_CTX(c);
    RECORD(c, OP_BEGIN, 1, mode);
    if (c->in_begin) {
        gl_error(c, GL_INVALID_OPERATION);
        return;
    }
    if (mode > GL_POLYGON) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->prim = mode;
    c->in_begin = 1;
    c->nverts = 0;
}

void glEnd(void)
{
    GET_CTX(c);
    RECORD0(c, OP_END);
    if (!c->in_begin) {
        gl_error(c, GL_INVALID_OPERATION);
        return;
    }
    c->in_begin = 0;
    int n = c->nverts;
    int idx[MAX_POLY / 2];
    switch (c->prim) {
    case GL_POINTS:
        for (int i = 0; i < n; i++)
            draw_point(c, i);
        break;
    case GL_LINES:
        for (int i = 0; i + 1 < n; i += 2)
            draw_line(c, i, i + 1);
        break;
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        for (int i = 0; i + 1 < n; i++)
            draw_line(c, i, i + 1);
        if (c->prim == GL_LINE_LOOP && n > 2)
            draw_line(c, n - 1, 0);
        break;
    case GL_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3) {
            idx[0] = i; idx[1] = i + 1; idx[2] = i + 2;
            draw_polygon(c, idx, 3, i + 2);
        }
        break;
    case GL_TRIANGLE_STRIP:
        for (int i = 2; i < n; i++) {
            /* Every other triangle is flipped so they all keep the first one's winding. */
            idx[0] = (i & 1) ? i - 1 : i - 2;
            idx[1] = (i & 1) ? i - 2 : i - 1;
            idx[2] = i;
            draw_polygon(c, idx, 3, i);
        }
        break;
    case GL_TRIANGLE_FAN:
        for (int i = 2; i < n; i++) {
            idx[0] = 0; idx[1] = i - 1; idx[2] = i;
            draw_polygon(c, idx, 3, i);
        }
        break;
    case GL_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            idx[0] = i; idx[1] = i + 1; idx[2] = i + 2; idx[3] = i + 3;
            draw_polygon(c, idx, 4, i + 3);
        }
        break;
    case GL_QUAD_STRIP:
        for (int i = 0; i + 3 < n; i += 2) {
            idx[0] = i; idx[1] = i + 1; idx[2] = i + 3; idx[3] = i + 2;
            draw_polygon(c, idx, 4, i + 3);
        }
        break;
    case GL_POLYGON:
        if (n > MAX_POLY / 2) {
            /* Too big to clip in one piece: draw it as a fan (it is convex). */
            for (int i = 2; i < n; i++) {
                idx[0] = 0; idx[1] = i - 1; idx[2] = i;
                draw_polygon(c, idx, 3, 0);
            }
        } else {
            for (int i = 0; i < n; i++)
                idx[i] = i;
            draw_polygon(c, idx, n, 0);
        }
        break;
    }
    c->nverts = 0;
}

void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2)
{
    glBegin(GL_POLYGON);
    glVertex2f(x1, y1);
    glVertex2f(x2, y1);
    glVertex2f(x2, y2);
    glVertex2f(x1, y2);
    glEnd();
}

void glRecti(GLint x1, GLint y1, GLint x2, GLint y2)
{
    glRectf((float)x1, (float)y1, (float)x2, (float)y2);
}
