/*
 * GL contexts and state: switches, matrices and their stacks, lights and
 * materials, the viewport, clearing and the glGet queries.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "glint.h"

struct fgl_context *gl_ctx;

#define GET_CTX(c)                                  \
    struct fgl_context *c = gl_ctx;                 \
    if (!c)                                         \
        return
#define OUTSIDE_BEGIN(c)                            \
    do {                                            \
        if ((c)->in_begin) {                        \
            gl_error((c), GL_INVALID_OPERATION);    \
            return;                                 \
        }                                           \
    } while (0)

void gl_error(struct fgl_context *c, GLenum e)
{
    if (c->error == GL_NO_ERROR)
        c->error = e;
}

/* ---- matrices (column major, as GL stores them) ---- */

void gl_mat_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
}

/* out = a b, any of them the same matrix. Column j of the product is a's
 * columns weighted by column j of b: four columns of four lanes each. The
 * matrices may come from the program, so they need not be aligned. */
void gl_mat_mul(float *out, const float *a, const float *b)
{
    __m128 a0 = _mm_loadu_ps(a), a1 = _mm_loadu_ps(a + 4);
    __m128 a2 = _mm_loadu_ps(a + 8), a3 = _mm_loadu_ps(a + 12);
    __m128 r[4];
    for (int col = 0; col < 4; col++) {
        const float *bc = b + col * 4;
        r[col] = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(a0, _mm_set1_ps(bc[0])),
                                                  _mm_mul_ps(a1, _mm_set1_ps(bc[1]))),
                                       _mm_mul_ps(a2, _mm_set1_ps(bc[2]))),
                            _mm_mul_ps(a3, _mm_set1_ps(bc[3])));
    }
    for (int col = 0; col < 4; col++)
        _mm_storeu_ps(out + col * 4, r[col]);
}

float *gl_top_matrix(struct fgl_context *c)
{
    switch (c->matrix_mode) {
    case GL_PROJECTION: return c->projection[c->proj_depth];
    case GL_TEXTURE: return c->texture[c->tex_depth];
    default: return c->modelview[c->mv_depth];
    }
}

void gl_matrix_changed(struct fgl_context *c)
{
    if (c->matrix_mode == GL_MODELVIEW) {
        c->normal_dirty = 1;
        c->eye_normal_valid = 0;
    }
}

static void mult_top(struct fgl_context *c, const float *m)
{
    float *top = gl_top_matrix(c);
    gl_mat_mul(top, top, m);
    gl_matrix_changed(c);
}

void glMatrixMode(GLenum mode)
{
    GET_CTX(c);
    RECORD(c, OP_MATRIX_MODE, 1, mode);
    OUTSIDE_BEGIN(c);
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->matrix_mode = mode;
}

void glLoadIdentity(void)
{
    GET_CTX(c);
    RECORD0(c, OP_LOAD_IDENTITY);
    OUTSIDE_BEGIN(c);
    gl_mat_identity(gl_top_matrix(c));
    gl_matrix_changed(c);
}

void glLoadMatrixf(const GLfloat *m)
{
    GET_CTX(c);
    RECORD(c, OP_LOAD_MATRIX, 16, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9],
           m[10], m[11], m[12], m[13], m[14], m[15]);
    OUTSIDE_BEGIN(c);
    memcpy(gl_top_matrix(c), m, 16 * sizeof(float));
    gl_matrix_changed(c);
}

void glLoadMatrixd(const GLdouble *m)
{
    float f[16];
    for (int i = 0; i < 16; i++)
        f[i] = (float)m[i];
    glLoadMatrixf(f);
}

void glMultMatrixf(const GLfloat *m)
{
    GET_CTX(c);
    RECORD(c, OP_MULT_MATRIX, 16, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9],
           m[10], m[11], m[12], m[13], m[14], m[15]);
    OUTSIDE_BEGIN(c);
    mult_top(c, m);
}

void glMultMatrixd(const GLdouble *m)
{
    float f[16];
    for (int i = 0; i < 16; i++)
        f[i] = (float)m[i];
    glMultMatrixf(f);
}

void glPushMatrix(void)
{
    GET_CTX(c);
    RECORD0(c, OP_PUSH_MATRIX);
    OUTSIDE_BEGIN(c);
    int *depth, max;
    float (*stack)[16];
    switch (c->matrix_mode) {
    case GL_PROJECTION: depth = &c->proj_depth; max = PROJECTION_DEPTH; stack = c->projection; break;
    case GL_TEXTURE: depth = &c->tex_depth; max = TEXTURE_DEPTH; stack = c->texture; break;
    default: depth = &c->mv_depth; max = MODELVIEW_DEPTH; stack = c->modelview; break;
    }
    if (*depth + 1 >= max) {
        gl_error(c, GL_STACK_OVERFLOW);
        return;
    }
    memcpy(stack[*depth + 1], stack[*depth], 16 * sizeof(float));
    (*depth)++;
}

void glPopMatrix(void)
{
    GET_CTX(c);
    RECORD0(c, OP_POP_MATRIX);
    OUTSIDE_BEGIN(c);
    int *depth;
    switch (c->matrix_mode) {
    case GL_PROJECTION: depth = &c->proj_depth; break;
    case GL_TEXTURE: depth = &c->tex_depth; break;
    default: depth = &c->mv_depth; break;
    }
    if (*depth == 0) {
        gl_error(c, GL_STACK_UNDERFLOW);
        return;
    }
    (*depth)--;
    gl_matrix_changed(c);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    GET_CTX(c);
    RECORD(c, OP_TRANSLATE, 3, x, y, z);
    OUTSIDE_BEGIN(c);
    float m[16];
    gl_mat_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
    mult_top(c, m);
}

void glTranslated(GLdouble x, GLdouble y, GLdouble z)
{
    glTranslatef((float)x, (float)y, (float)z);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    GET_CTX(c);
    RECORD(c, OP_SCALE, 3, x, y, z);
    OUTSIDE_BEGIN(c);
    float m[16];
    gl_mat_identity(m);
    m[0] = x;
    m[5] = y;
    m[10] = z;
    mult_top(c, m);
}

void glScaled(GLdouble x, GLdouble y, GLdouble z)
{
    glScalef((float)x, (float)y, (float)z);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    GET_CTX(c);
    RECORD(c, OP_ROTATE, 4, angle, x, y, z);
    OUTSIDE_BEGIN(c);
    float len = sqrtf(x * x + y * y + z * z);
    if (len == 0)
        return;
    x /= len;
    y /= len;
    z /= len;
    float a = angle * (float)(M_PI / 180.0), s = sinf(a), co = cosf(a), t = 1 - co;
    float m[16];
    m[0] = x * x * t + co;     m[4] = x * y * t - z * s;  m[8] = x * z * t + y * s;   m[12] = 0;
    m[1] = y * x * t + z * s;  m[5] = y * y * t + co;     m[9] = y * z * t - x * s;   m[13] = 0;
    m[2] = x * z * t - y * s;  m[6] = y * z * t + x * s;  m[10] = z * z * t + co;     m[14] = 0;
    m[3] = 0;                  m[7] = 0;                  m[11] = 0;                  m[15] = 1;
    mult_top(c, m);
}

void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z)
{
    glRotatef((float)angle, (float)x, (float)y, (float)z);
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    GET_CTX(c);
    RECORD(c, OP_FRUSTUM, 6, l, r, b, t, n, f);
    OUTSIDE_BEGIN(c);
    if (n <= 0 || f <= 0 || l == r || b == t || n == f) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    float m[16] = { 0 };
    m[0] = (float)(2 * n / (r - l));
    m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l));
    m[9] = (float)((t + b) / (t - b));
    m[10] = (float)(-(f + n) / (f - n));
    m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    mult_top(c, m);
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    GET_CTX(c);
    RECORD(c, OP_ORTHO, 6, l, r, b, t, n, f);
    OUTSIDE_BEGIN(c);
    if (l == r || b == t || n == f) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    float m[16];
    gl_mat_identity(m);
    m[0] = (float)(2 / (r - l));
    m[5] = (float)(2 / (t - b));
    m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l));
    m[13] = (float)(-(t + b) / (t - b));
    m[14] = (float)(-(f + n) / (f - n));
    mult_top(c, m);
}

/* ---- switches ---- */

static int *cap_flag(struct fgl_context *c, GLenum cap)
{
    static int ignored;
    switch (cap) {
    case GL_CULL_FACE: return &c->cull;
    case GL_DEPTH_TEST: return &c->depth_test;
    case GL_LIGHTING: return &c->lighting;
    case GL_NORMALIZE: case GL_RESCALE_NORMAL: return &c->normalize;
    case GL_COLOR_MATERIAL: return &c->color_material;
    case GL_SCISSOR_TEST: return &c->scissor_test;
    case GL_DITHER: return &c->dither;
    case GL_POINT_SMOOTH: case GL_LINE_SMOOTH: case GL_POLYGON_SMOOTH:
        ignored = 0;
        return &ignored;                /* accepted; nothing is anti-aliased */
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_LIGHTS)
        return &c->light_on[cap - GL_LIGHT0];
    return NULL;
}

static void set_cap(GLenum cap, int on)
{
    GET_CTX(c);
    RECORD(c, on ? OP_ENABLE : OP_DISABLE, 1, cap);
    OUTSIDE_BEGIN(c);
    int *flag = cap_flag(c, cap);
    if (!flag) {
        /* Features this GL lacks: switching them off is fine. */
        if (!on && (cap == GL_BLEND || cap == GL_TEXTURE_2D || cap == GL_FOG || cap == GL_ALPHA_TEST))
            return;
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    *flag = on;
    c->hw_dirty = 1;
}

void glEnable(GLenum cap) { set_cap(cap, 1); }
void glDisable(GLenum cap) { set_cap(cap, 0); }

GLboolean glIsEnabled(GLenum cap)
{
    struct fgl_context *c = gl_ctx;
    if (!c)
        return GL_FALSE;
    int *flag = cap_flag(c, cap);
    if (!flag) {
        if (cap != GL_BLEND && cap != GL_TEXTURE_2D && cap != GL_FOG && cap != GL_ALPHA_TEST)
            gl_error(c, GL_INVALID_ENUM);
        return GL_FALSE;
    }
    return *flag ? GL_TRUE : GL_FALSE;
}

GLenum glGetError(void)
{
    struct fgl_context *c = gl_ctx;
    if (!c)
        return GL_NO_ERROR;
    GLenum e = c->error;
    c->error = GL_NO_ERROR;
    return e;
}

void glHint(GLenum target, GLenum mode)
{
    (void)target;
    (void)mode;
}

/* ---- render state ---- */

void glShadeModel(GLenum mode)
{
    GET_CTX(c);
    RECORD(c, OP_SHADE_MODEL, 1, mode);
    if (mode != GL_FLAT && mode != GL_SMOOTH) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->shade_model = mode;
}

void glCullFace(GLenum mode)
{
    GET_CTX(c);
    RECORD(c, OP_CULL_FACE, 1, mode);
    if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->cull_face = mode;
}

void glFrontFace(GLenum mode)
{
    GET_CTX(c);
    RECORD(c, OP_FRONT_FACE, 1, mode);
    if (mode != GL_CW && mode != GL_CCW) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->front_face = mode;
}

void glDepthFunc(GLenum func)
{
    GET_CTX(c);
    RECORD(c, OP_DEPTH_FUNC, 1, func);
    if (func < GL_NEVER || func > GL_ALWAYS) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->depth_func = func;
    c->hw_dirty = 1;
}

void glDepthMask(GLboolean flag)
{
    GET_CTX(c);
    RECORD(c, OP_DEPTH_MASK, 1, flag);
    c->depth_mask = flag != 0;
    c->hw_dirty = 1;
}

void glDepthRange(GLclampd n, GLclampd f)
{
    GET_CTX(c);
    RECORD(c, OP_DEPTH_RANGE, 2, n, f);
    c->depth_near = gl_clampf((float)n);
    c->depth_far = gl_clampf((float)f);
}

void glPointSize(GLfloat size)
{
    GET_CTX(c);
    RECORD(c, OP_POINT_SIZE, 1, size);
    if (size <= 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    c->point_size = size;
}

void glLineWidth(GLfloat width)
{
    GET_CTX(c);
    RECORD(c, OP_LINE_WIDTH, 1, width);
    if (width <= 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    c->line_width = width;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    GET_CTX(c);
    RECORD(c, OP_VIEWPORT, 4, x, y, w, h);
    OUTSIDE_BEGIN(c);
    if (w < 0 || h < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    c->vp_x = x;
    c->vp_y = y;
    c->vp_w = w > MAX_VIEWPORT ? MAX_VIEWPORT : w;
    c->vp_h = h > MAX_VIEWPORT ? MAX_VIEWPORT : h;
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    GET_CTX(c);
    RECORD(c, OP_SCISSOR, 4, x, y, w, h);
    OUTSIDE_BEGIN(c);
    if (w < 0 || h < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    c->scissor_box[0] = x;
    c->scissor_box[1] = y;
    c->scissor_box[2] = w;
    c->scissor_box[3] = h;
    c->hw_dirty = 1;
}

void gl_clip_rect(struct fgl_context *c, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 0;
    *y0 = 0;
    *x1 = c->width;
    *y1 = c->height;
    if (!c->scissor_test)
        return;
    /* The scissor box has GL's bottom-left origin. */
    int sx0 = c->scissor_box[0], sx1 = sx0 + c->scissor_box[2];
    int sy0 = c->height - (c->scissor_box[1] + c->scissor_box[3]), sy1 = c->height - c->scissor_box[1];
    if (sx0 > *x0) *x0 = sx0;
    if (sy0 > *y0) *y0 = sy0;
    if (sx1 < *x1) *x1 = sx1;
    if (sy1 < *y1) *y1 = sy1;
    if (*x1 < *x0) *x1 = *x0;
    if (*y1 < *y0) *y1 = *y0;
}

/* ---- clearing ---- */

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    GET_CTX(c);
    RECORD(c, OP_CLEAR_COLOR, 4, r, g, b, a);
    c->clear_color[0] = gl_clampf(r);
    c->clear_color[1] = gl_clampf(g);
    c->clear_color[2] = gl_clampf(b);
    c->clear_color[3] = gl_clampf(a);
}

void glClearDepth(GLclampd depth)
{
    GET_CTX(c);
    RECORD(c, OP_CLEAR_DEPTH, 1, depth);
    c->clear_depth = gl_clampf((float)depth);
}

void glClear(GLbitfield mask)
{
    GET_CTX(c);
    RECORD(c, OP_CLEAR, 1, mask);
    OUTSIDE_BEGIN(c);
    if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | 0x400 | 0x200)) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    if (!c->depth_mask)
        mask &= ~(GLbitfield)GL_DEPTH_BUFFER_BIT;
    mask &= GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;     /* there is no stencil or accum */
    int x0, y0, x1, y1;
    gl_clip_rect(c, &x0, &y0, &x1, &y1);
    if (mask && x1 > x0 && y1 > y0)
        c->be->clear(c, mask, x0, y0, x1, y1);
}

void glFlush(void)
{
}

void glFinish(void)
{
    GET_CTX(c);
    c->be->finish(c);
}

/* ---- lights and materials ---- */

static void normalize3(float *v)
{
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

static int nparams(GLenum pname)
{
    switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: case GL_EMISSION:
    case GL_AMBIENT_AND_DIFFUSE: case GL_LIGHT_MODEL_AMBIENT:
        return 4;
    case GL_SPOT_DIRECTION:
        return 3;
    default:
        return 1;
    }
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *p)
{
    GET_CTX(c);
    float a[4] = { p[0], 0, 0, 0 };
    for (int i = 1; i < nparams(pname); i++)
        a[i] = p[i];
    RECORD(c, OP_LIGHT, 6, light, pname, a[0], a[1], a[2], a[3]);
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + MAX_LIGHTS) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    struct light *l = &c->lights[light - GL_LIGHT0];
    const float *mv = c->modelview[c->mv_depth];
    switch (pname) {
    case GL_AMBIENT: memcpy(l->ambient, a, sizeof(a)); break;
    case GL_DIFFUSE: memcpy(l->diffuse, a, sizeof(a)); break;
    case GL_SPECULAR: memcpy(l->specular, a, sizeof(a)); break;
    case GL_POSITION:
        /* Positions are kept in eye coordinates, through today's modelview. */
        for (int i = 0; i < 4; i++)
            l->position[i] = mv[i] * a[0] + mv[4 + i] * a[1] + mv[8 + i] * a[2] + mv[12 + i] * a[3];
        memcpy(l->dir, l->position, sizeof(l->dir));
        normalize3(l->dir);
        l->half[0] = l->dir[0];
        l->half[1] = l->dir[1];
        l->half[2] = l->dir[2] + 1;     /* the viewer is along +z */
        normalize3(l->half);
        break;
    case GL_SPOT_DIRECTION:
        for (int i = 0; i < 3; i++)
            l->spot_dir[i] = mv[i] * a[0] + mv[4 + i] * a[1] + mv[8 + i] * a[2];
        normalize3(l->spot_dir);
        break;
    case GL_SPOT_EXPONENT:
        if (a[0] < 0 || a[0] > 128) {
            gl_error(c, GL_INVALID_VALUE);
            return;
        }
        l->spot_exp = a[0];
        break;
    case GL_SPOT_CUTOFF:
        if ((a[0] < 0 || a[0] > 90) && a[0] != 180) {
            gl_error(c, GL_INVALID_VALUE);
            return;
        }
        l->spot_cutoff = a[0];
        l->spot_cos = cosf(a[0] * (float)(M_PI / 180.0));
        break;
    case GL_CONSTANT_ATTENUATION: l->att[0] = a[0]; break;
    case GL_LINEAR_ATTENUATION: l->att[1] = a[0]; break;
    case GL_QUADRATIC_ATTENUATION: l->att[2] = a[0]; break;
    default:
        gl_error(c, GL_INVALID_ENUM);
    }
}

void glLightf(GLenum light, GLenum pname, GLfloat param)
{
    glLightfv(light, pname, &param);
}

void glLighti(GLenum light, GLenum pname, GLint param)
{
    GLfloat f = (GLfloat)param;
    glLightfv(light, pname, &f);
}

void glLightModelfv(GLenum pname, const GLfloat *p)
{
    GET_CTX(c);
    float a[4] = { p[0], 0, 0, 0 };
    for (int i = 1; i < nparams(pname); i++)
        a[i] = p[i];
    RECORD(c, OP_LIGHT_MODEL, 5, pname, a[0], a[1], a[2], a[3]);
    switch (pname) {
    case GL_LIGHT_MODEL_AMBIENT:
        memcpy(c->model_ambient, a, sizeof(a));
        break;
    case GL_LIGHT_MODEL_LOCAL_VIEWER:
    case GL_LIGHT_MODEL_TWO_SIDE:
        break;                          /* accepted: an infinite viewer, front faces only */
    default:
        gl_error(c, GL_INVALID_ENUM);
    }
}

void glLightModelf(GLenum pname, GLfloat param)
{
    glLightModelfv(pname, &param);
}

void glLightModeli(GLenum pname, GLint param)
{
    GLfloat f = (GLfloat)param;
    glLightModelfv(pname, &f);
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *p)
{
    GET_CTX(c);
    float a[4] = { p[0], 0, 0, 0 };
    for (int i = 1; i < nparams(pname); i++)
        a[i] = p[i];
    RECORD(c, OP_MATERIAL, 6, face, pname, a[0], a[1], a[2], a[3]);
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (face == GL_BACK)
        return;                         /* back faces are never lit */
    struct material *m = &c->mat;
    switch (pname) {
    case GL_AMBIENT: memcpy(m->ambient, a, sizeof(a)); break;
    case GL_DIFFUSE: memcpy(m->diffuse, a, sizeof(a)); break;
    case GL_SPECULAR: memcpy(m->specular, a, sizeof(a)); break;
    case GL_EMISSION: memcpy(m->emission, a, sizeof(a)); break;
    case GL_AMBIENT_AND_DIFFUSE:
        memcpy(m->ambient, a, sizeof(a));
        memcpy(m->diffuse, a, sizeof(a));
        break;
    case GL_SHININESS:
        if (a[0] < 0 || a[0] > 128) {
            gl_error(c, GL_INVALID_VALUE);
            return;
        }
        m->shininess = a[0];
        break;
    default:
        gl_error(c, GL_INVALID_ENUM);
    }
}

void glMaterialf(GLenum face, GLenum pname, GLfloat param)
{
    glMaterialfv(face, pname, &param);
}

void glMateriali(GLenum face, GLenum pname, GLint param)
{
    GLfloat f = (GLfloat)param;
    glMaterialfv(face, pname, &f);
}

void glColorMaterial(GLenum face, GLenum mode)
{
    GET_CTX(c);
    RECORD(c, OP_COLOR_MATERIAL, 2, face, mode);
    if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) ||
        (mode != GL_AMBIENT && mode != GL_DIFFUSE && mode != GL_SPECULAR &&
         mode != GL_EMISSION && mode != GL_AMBIENT_AND_DIFFUSE)) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->cm_face = face;
    c->cm_mode = mode;
}

/* ---- queries ---- */

static const char *gl_extensions = "";

const GLubyte *glGetString(GLenum name)
{
    struct fgl_context *c = gl_ctx;
    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"fluxOS";
    case GL_RENDERER: return (const GLubyte *)(c ? c->renderer : "none");
    case GL_VERSION: return (const GLubyte *)"1.1 fluxOS";
    case GL_EXTENSIONS: return (const GLubyte *)gl_extensions;
    }
    if (c)
        gl_error(c, GL_INVALID_ENUM);
    return NULL;
}

/* Every glGet goes through doubles; returns the number of values. */
static int get_values(struct fgl_context *c, GLenum pname, double *v)
{
    switch (pname) {
    case GL_MODELVIEW_MATRIX:
    case GL_PROJECTION_MATRIX: {
        const float *m = pname == GL_MODELVIEW_MATRIX ? c->modelview[c->mv_depth]
                                                      : c->projection[c->proj_depth];
        for (int i = 0; i < 16; i++)
            v[i] = m[i];
        return 16;
    }
    case GL_VIEWPORT:
        v[0] = c->vp_x; v[1] = c->vp_y; v[2] = c->vp_w; v[3] = c->vp_h;
        return 4;
    case GL_SCISSOR_BOX:
        for (int i = 0; i < 4; i++)
            v[i] = c->scissor_box[i];
        return 4;
    case GL_CURRENT_COLOR:
        for (int i = 0; i < 4; i++)
            v[i] = c->color[i];
        return 4;
    case GL_COLOR_CLEAR_VALUE:
        for (int i = 0; i < 4; i++)
            v[i] = c->clear_color[i];
        return 4;
    case GL_CURRENT_NORMAL:
        v[0] = c->normal[0]; v[1] = c->normal[1]; v[2] = c->normal[2];
        return 3;
    case GL_DEPTH_RANGE:
        v[0] = c->depth_near; v[1] = c->depth_far;
        return 2;
    case GL_MAX_VIEWPORT_DIMS:
        v[0] = v[1] = MAX_VIEWPORT;
        return 2;
    case GL_MATRIX_MODE: v[0] = c->matrix_mode; return 1;
    case GL_SHADE_MODEL: v[0] = c->shade_model; return 1;
    case GL_DEPTH_FUNC: v[0] = c->depth_func; return 1;
    case GL_DEPTH_WRITEMASK: v[0] = c->depth_mask; return 1;
    case GL_DEPTH_CLEAR_VALUE: v[0] = c->clear_depth; return 1;
    case GL_POINT_SIZE: v[0] = c->point_size; return 1;
    case GL_LINE_WIDTH: v[0] = c->line_width; return 1;
    case GL_MAX_LIGHTS: v[0] = MAX_LIGHTS; return 1;
    case GL_MAX_MODELVIEW_STACK_DEPTH: v[0] = MODELVIEW_DEPTH; return 1;
    case GL_MAX_PROJECTION_STACK_DEPTH: v[0] = PROJECTION_DEPTH; return 1;
    case GL_RED_BITS: case GL_BLUE_BITS:
        v[0] = c->be == &gl_tdfx_backend ? 5 : 8;
        return 1;
    case GL_GREEN_BITS:
        v[0] = c->be == &gl_tdfx_backend ? 6 : 8;
        return 1;
    case GL_DEPTH_BITS:
        v[0] = c->be == &gl_tdfx_backend ? 16 : c->be == &gl_radeon_backend ? 24 : 30;
        return 1;
    }
    {
        int *flag = cap_flag(c, pname);
        if (flag) {
            v[0] = *flag;
            return 1;
        }
    }
    gl_error(c, GL_INVALID_ENUM);
    return 0;
}

void glGetDoublev(GLenum pname, GLdouble *params)
{
    GET_CTX(c);
    double v[16];
    int n = get_values(c, pname, v);
    for (int i = 0; i < n; i++)
        params[i] = v[i];
}

void glGetFloatv(GLenum pname, GLfloat *params)
{
    GET_CTX(c);
    double v[16];
    int n = get_values(c, pname, v);
    for (int i = 0; i < n; i++)
        params[i] = (float)v[i];
}

void glGetIntegerv(GLenum pname, GLint *params)
{
    GET_CTX(c);
    double v[16];
    int n = get_values(c, pname, v);
    int colors = pname == GL_CURRENT_COLOR || pname == GL_COLOR_CLEAR_VALUE;
    for (int i = 0; i < n; i++)
        params[i] = colors ? (GLint)(v[i] * 2147483647.0) : (GLint)(v[i] < 0 ? v[i] - 0.5 : v[i] + 0.5);
}

void glGetBooleanv(GLenum pname, GLboolean *params)
{
    GET_CTX(c);
    double v[16];
    int n = get_values(c, pname, v);
    for (int i = 0; i < n; i++)
        params[i] = v[i] != 0;
}

/* ---- contexts ---- */

static void init_state(struct fgl_context *c)
{
    c->matrix_mode = GL_MODELVIEW;
    gl_mat_identity(c->modelview[0]);
    gl_mat_identity(c->projection[0]);
    gl_mat_identity(c->texture[0]);
    c->normal_dirty = 1;
    c->vp_w = c->width;
    c->vp_h = c->height;
    c->scissor_box[2] = c->width;
    c->scissor_box[3] = c->height;
    c->depth_near = 0;
    c->depth_far = 1;
    c->dither = 1;
    c->cull_face = GL_BACK;
    c->front_face = GL_CCW;
    c->shade_model = GL_SMOOTH;
    c->depth_func = GL_LESS;
    c->depth_mask = 1;
    c->clear_depth = 1;
    c->point_size = 1;
    c->line_width = 1;
    for (int i = 0; i < MAX_LIGHTS; i++) {
        struct light *l = &c->lights[i];
        l->ambient[3] = l->diffuse[3] = l->specular[3] = 1;
        if (i == 0) {
            for (int k = 0; k < 3; k++)
                l->diffuse[k] = l->specular[k] = 1;
        }
        l->position[2] = 1;
        l->dir[2] = 1;
        l->half[2] = 1;
        l->spot_dir[2] = -1;
        l->spot_cutoff = 180;
        l->spot_cos = -1;
        l->att[0] = 1;
    }
    struct material *m = &c->mat;
    for (int k = 0; k < 3; k++) {
        m->ambient[k] = 0.2f;
        m->diffuse[k] = 0.8f;
    }
    m->ambient[3] = m->diffuse[3] = m->specular[3] = m->emission[3] = 1;
    for (int k = 0; k < 3; k++)
        c->model_ambient[k] = 0.2f;
    c->model_ambient[3] = 1;
    c->cm_face = GL_FRONT_AND_BACK;
    c->cm_mode = GL_AMBIENT_AND_DIFFUSE;
    for (int k = 0; k < 4; k++)
        c->color[k] = 1;
    c->normal[2] = 1;
    c->hw_dirty = 1;
    gl_prim_init(c);
}

/* The first 3D card that takes the context; -1 for software. */
static int attach_hardware(struct fgl_context *c, unsigned int flags)
{
    const char *only = getenv("FLUXGL_DRIVER");
    if ((flags & FGL_SOFTWARE) || getenv("LIBGL_ALWAYS_SOFTWARE") || (only && !strcmp(only, "soft")))
        return -1;
    if (!(flags & FGL_NO_RADEON) && (!only || !strcmp(only, "radeon")) && gl_radeon_attach(c) == 0)
        return 0;
    if (!(flags & FGL_NO_TDFX) && (!only || !strcmp(only, "tdfx")) && gl_tdfx_attach(c) == 0)
        return 0;
    return -1;
}

FGLcontext *fglCreateContext(int width, int height, unsigned int flags)
{
    if (width < 1 || height < 1 || width > MAX_VIEWPORT || height > MAX_VIEWPORT)
        return NULL;
    struct fgl_context *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->width = width;
    c->height = height;
    init_state(c);
    if (attach_hardware(c, flags) < 0) {
        c->be = &gl_soft_backend;
        strcpy(c->renderer, gl_soft_backend.name);
        if (c->be->resize(c) < 0) {
            free(c->verts);
            free(c);
            return NULL;
        }
    }
    return c;
}

void fglDestroyContext(FGLcontext *c)
{
    if (!c)
        return;
    if (gl_ctx == c)
        gl_ctx = NULL;
    c->be->destroy(c);
    gl_free_lists(c);
    free(c->verts);
    free(c);
}

void fglMakeCurrent(FGLcontext *c)
{
    gl_ctx = c;
}

FGLcontext *fglGetCurrentContext(void)
{
    return gl_ctx;
}

/* Fall back to drawing in memory if the hardware cannot keep the context. */
static int use_software(struct fgl_context *c)
{
    c->be->destroy(c);
    c->be = &gl_soft_backend;
    strcpy(c->renderer, gl_soft_backend.name);
    return c->be->resize(c);
}

int fglResize(FGLcontext *c, int width, int height)
{
    if (!c || width < 1 || height < 1 || width > MAX_VIEWPORT || height > MAX_VIEWPORT)
        return -1;
    int ow = c->width, oh = c->height;
    c->width = width;
    c->height = height;
    if (c->be->resize(c) == 0)
        return 0;
    if (c->be != &gl_soft_backend && use_software(c) == 0)
        return 0;
    c->width = ow;
    c->height = oh;
    c->be->resize(c);
    return -1;
}

void fglSwapBuffers(FGLcontext *c, uint32_t *dst, int stride)
{
    if (!c)
        return;
    c->be->finish(c);
    c->be->read(c, dst, stride);
    /* A video mode change may have grown the screen over our video memory,
     * or the engine may have hung. */
    if ((c->be == &gl_tdfx_backend && gl_tdfx_check(c) < 0) ||
        (c->be == &gl_radeon_backend && gl_radeon_check(c) < 0))
        use_software(c);
}

int fglIsAccelerated(FGLcontext *c)
{
    return c && c->be != &gl_soft_backend;
}

int fglGetDriver(FGLcontext *c)
{
    if (c && c->be == &gl_radeon_backend)
        return FGL_DRIVER_RADEON;
    if (c && c->be == &gl_tdfx_backend)
        return FGL_DRIVER_TDFX;
    return FGL_DRIVER_SOFTWARE;
}
