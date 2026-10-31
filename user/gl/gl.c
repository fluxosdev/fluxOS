/*
 * GL contexts and state: switches, matrices and their stacks, lights and
 * materials, the viewport, blending, the stencil test, fog, user clip
 * planes and the other fragment operations, polygon modes, clearing, the
 * attribute stacks and the glGet queries.
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

/* The adjugate over the determinant, in doubles: the cofactors of the
 * first column give the determinant. */
int gl_mat_invert(float *out, const float *m)
{
    double inv[16], det;
    inv[0] = (double)m[5] * m[10] * m[15] - (double)m[5] * m[11] * m[14] - (double)m[9] * m[6] * m[15] +
             (double)m[9] * m[7] * m[14] + (double)m[13] * m[6] * m[11] - (double)m[13] * m[7] * m[10];
    inv[4] = -(double)m[4] * m[10] * m[15] + (double)m[4] * m[11] * m[14] + (double)m[8] * m[6] * m[15] -
             (double)m[8] * m[7] * m[14] - (double)m[12] * m[6] * m[11] + (double)m[12] * m[7] * m[10];
    inv[8] = (double)m[4] * m[9] * m[15] - (double)m[4] * m[11] * m[13] - (double)m[8] * m[5] * m[15] +
             (double)m[8] * m[7] * m[13] + (double)m[12] * m[5] * m[11] - (double)m[12] * m[7] * m[9];
    inv[12] = -(double)m[4] * m[9] * m[14] + (double)m[4] * m[10] * m[13] + (double)m[8] * m[5] * m[14] -
              (double)m[8] * m[6] * m[13] - (double)m[12] * m[5] * m[10] + (double)m[12] * m[6] * m[9];
    inv[1] = -(double)m[1] * m[10] * m[15] + (double)m[1] * m[11] * m[14] + (double)m[9] * m[2] * m[15] -
             (double)m[9] * m[3] * m[14] - (double)m[13] * m[2] * m[11] + (double)m[13] * m[3] * m[10];
    inv[5] = (double)m[0] * m[10] * m[15] - (double)m[0] * m[11] * m[14] - (double)m[8] * m[2] * m[15] +
             (double)m[8] * m[3] * m[14] + (double)m[12] * m[2] * m[11] - (double)m[12] * m[3] * m[10];
    inv[9] = -(double)m[0] * m[9] * m[15] + (double)m[0] * m[11] * m[13] + (double)m[8] * m[1] * m[15] -
             (double)m[8] * m[3] * m[13] - (double)m[12] * m[1] * m[11] + (double)m[12] * m[3] * m[9];
    inv[13] = (double)m[0] * m[9] * m[14] - (double)m[0] * m[10] * m[13] - (double)m[8] * m[1] * m[14] +
              (double)m[8] * m[2] * m[13] + (double)m[12] * m[1] * m[10] - (double)m[12] * m[2] * m[9];
    inv[2] = (double)m[1] * m[6] * m[15] - (double)m[1] * m[7] * m[14] - (double)m[5] * m[2] * m[15] +
             (double)m[5] * m[3] * m[14] + (double)m[13] * m[2] * m[7] - (double)m[13] * m[3] * m[6];
    inv[6] = -(double)m[0] * m[6] * m[15] + (double)m[0] * m[7] * m[14] + (double)m[4] * m[2] * m[15] -
             (double)m[4] * m[3] * m[14] - (double)m[12] * m[2] * m[7] + (double)m[12] * m[3] * m[6];
    inv[10] = (double)m[0] * m[5] * m[15] - (double)m[0] * m[7] * m[13] - (double)m[4] * m[1] * m[15] +
              (double)m[4] * m[3] * m[13] + (double)m[12] * m[1] * m[7] - (double)m[12] * m[3] * m[5];
    inv[14] = -(double)m[0] * m[5] * m[14] + (double)m[0] * m[6] * m[13] + (double)m[4] * m[1] * m[14] -
              (double)m[4] * m[2] * m[13] - (double)m[12] * m[1] * m[6] + (double)m[12] * m[2] * m[5];
    inv[3] = -(double)m[1] * m[6] * m[11] + (double)m[1] * m[7] * m[10] + (double)m[5] * m[2] * m[11] -
             (double)m[5] * m[3] * m[10] - (double)m[9] * m[2] * m[7] + (double)m[9] * m[3] * m[6];
    inv[7] = (double)m[0] * m[6] * m[11] - (double)m[0] * m[7] * m[10] - (double)m[4] * m[2] * m[11] +
             (double)m[4] * m[3] * m[10] + (double)m[8] * m[2] * m[7] - (double)m[8] * m[3] * m[6];
    inv[11] = -(double)m[0] * m[5] * m[11] + (double)m[0] * m[7] * m[9] + (double)m[4] * m[1] * m[11] -
              (double)m[4] * m[3] * m[9] - (double)m[8] * m[1] * m[7] + (double)m[8] * m[3] * m[5];
    inv[15] = (double)m[0] * m[5] * m[10] - (double)m[0] * m[6] * m[9] - (double)m[4] * m[1] * m[10] +
              (double)m[4] * m[2] * m[9] + (double)m[8] * m[1] * m[6] - (double)m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0) {
        gl_mat_identity(out);
        return -1;
    }
    for (int i = 0; i < 16; i++)
        out[i] = (float)(inv[i] / det);
    return 0;
}

void gl_plane_to_eye(struct fgl_context *c, const float *p, float *out)
{
    float inv[16];
    gl_mat_invert(inv, c->modelview[c->mv_depth]);
    /* The row vector p times the inverse: out[j] is p dotted with its column j. */
    for (int j = 0; j < 4; j++)
        out[j] = p[0] * inv[j * 4] + p[1] * inv[j * 4 + 1] + p[2] * inv[j * 4 + 2] + p[3] * inv[j * 4 + 3];
}

float *gl_top_matrix(struct fgl_context *c)
{
    struct texunit *u = &c->unit[c->active_unit];
    switch (c->matrix_mode) {
    case GL_PROJECTION: return c->projection[c->proj_depth];
    case GL_TEXTURE: return u->matrix[u->depth];
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

/* The stack the matrix mode names: its depth, its size and its matrices. */
static float (*matrix_stack(struct fgl_context *c, int **depth, int *max))[16]
{
    struct texunit *u = &c->unit[c->active_unit];
    switch (c->matrix_mode) {
    case GL_PROJECTION: *depth = &c->proj_depth; *max = PROJECTION_DEPTH; return c->projection;
    case GL_TEXTURE: *depth = &u->depth; *max = TEXTURE_DEPTH; return u->matrix;
    default: *depth = &c->mv_depth; *max = MODELVIEW_DEPTH; return c->modelview;
    }
}

void glPushMatrix(void)
{
    GET_CTX(c);
    RECORD0(c, OP_PUSH_MATRIX);
    OUTSIDE_BEGIN(c);
    int *depth, max;
    float (*stack)[16] = matrix_stack(c, &depth, &max);
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
    int *depth, max;
    matrix_stack(c, &depth, &max);
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

/* ---- user clip planes ---- */

void glClipPlane(GLenum plane, const GLdouble *eq)
{
    GET_CTX(c);
    RECORD(c, OP_CLIP_PLANE, 5, plane, eq[0], eq[1], eq[2], eq[3]);
    OUTSIDE_BEGIN(c);
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + MAX_CLIP_PLANES) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    float p[4] = { (float)eq[0], (float)eq[1], (float)eq[2], (float)eq[3] };
    gl_plane_to_eye(c, p, c->clip_plane[plane - GL_CLIP_PLANE0]);
}

void glGetClipPlane(GLenum plane, GLdouble *eq)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + MAX_CLIP_PLANES) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    for (int i = 0; i < 4; i++)
        eq[i] = c->clip_plane[plane - GL_CLIP_PLANE0][i];
}

/* ---- switches ---- */

static int *cap_flag(struct fgl_context *c, GLenum cap)
{
    static int ignored;
    struct texunit *u = &c->unit[c->active_unit];
    switch (cap) {
    case GL_CULL_FACE: return &c->cull;
    case GL_DEPTH_TEST: return &c->depth_test;
    case GL_LIGHTING: return &c->lighting;
    case GL_NORMALIZE: case GL_RESCALE_NORMAL: return &c->normalize;
    case GL_COLOR_MATERIAL: return &c->color_material;
    case GL_SCISSOR_TEST: return &c->scissor_test;
    case GL_DITHER: return &c->dither;
    case GL_TEXTURE_2D: return &u->enabled;
    case GL_BLEND: return &c->blend;
    case GL_ALPHA_TEST: return &c->alpha_test;
    case GL_POLYGON_OFFSET_FILL: return &c->offset_fill;
    case GL_POLYGON_OFFSET_LINE: return &c->offset_line;
    case GL_POLYGON_OFFSET_POINT: return &c->offset_point;
    case GL_FOG: return &c->fog;
    case GL_STENCIL_TEST: return &c->stencil_test;
    case GL_TEXTURE_GEN_S: case GL_TEXTURE_GEN_T: case GL_TEXTURE_GEN_R: case GL_TEXTURE_GEN_Q:
        return &u->gen_on[cap - GL_TEXTURE_GEN_S];
    case GL_POINT_SMOOTH: case GL_LINE_SMOOTH: case GL_POLYGON_SMOOTH:
        ignored = 0;
        return &ignored;                /* accepted; nothing is anti-aliased */
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_LIGHTS)
        return &c->light_on[cap - GL_LIGHT0];
    if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + MAX_CLIP_PLANES)
        return &c->clip_on[cap - GL_CLIP_PLANE0];
    return NULL;
}

/* Features this GL lacks: switching them off is fine, and they read as off. */
static int lacking(GLenum cap)
{
    return cap == GL_TEXTURE_1D || cap == GL_LINE_STIPPLE || cap == GL_POLYGON_STIPPLE || cap == GL_COLOR_LOGIC_OP;
}

static void set_cap(GLenum cap, int on)
{
    GET_CTX(c);
    RECORD(c, on ? OP_ENABLE : OP_DISABLE, 1, cap);
    OUTSIDE_BEGIN(c);
    int *flag = cap_flag(c, cap);
    if (!flag) {
        if (!on && lacking(cap))
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
    struct varray *a = gl_client_array(c, cap);
    if (a)
        return a->enabled ? GL_TRUE : GL_FALSE;
    int *flag = cap_flag(c, cap);
    if (!flag) {
        if (!lacking(cap))
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

/* Hints are kept for glGet; nothing draws differently for them. */
void glHint(GLenum target, GLenum mode)
{
    GET_CTX(c);
    RECORD(c, OP_HINT, 2, target, mode);
    OUTSIDE_BEGIN(c);
    if (target < GL_PERSPECTIVE_CORRECTION_HINT || target > GL_FOG_HINT ||
        (mode != GL_DONT_CARE && mode != GL_FASTEST && mode != GL_NICEST)) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->hints[target - GL_PERSPECTIVE_CORRECTION_HINT] = mode;
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

void glPolygonMode(GLenum face, GLenum mode)
{
    GET_CTX(c);
    RECORD(c, OP_POLYGON_MODE, 2, face, mode);
    OUTSIDE_BEGIN(c);
    if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) ||
        (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL)) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (face != GL_BACK)
        c->polygon_mode[0] = mode;
    if (face != GL_FRONT)
        c->polygon_mode[1] = mode;
}

void glPolygonOffset(GLfloat factor, GLfloat units)
{
    GET_CTX(c);
    RECORD(c, OP_POLYGON_OFFSET, 2, factor, units);
    OUTSIDE_BEGIN(c);
    c->offset_factor = factor;
    c->offset_units = units;
}

/* ---- fragment operations ---- */

static int blend_factor_ok(GLenum f)
{
    return f == GL_ZERO || f == GL_ONE || (f >= GL_SRC_COLOR && f <= GL_SRC_ALPHA_SATURATE);
}

/* Any factor either way round, as GL 1.4 allows, except that only the
 * source may saturate. */
void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    GET_CTX(c);
    RECORD(c, OP_BLEND_FUNC, 2, sfactor, dfactor);
    OUTSIDE_BEGIN(c);
    if (!blend_factor_ok(sfactor) || !blend_factor_ok(dfactor) || dfactor == GL_SRC_ALPHA_SATURATE) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->blend_src = sfactor;
    c->blend_dst = dfactor;
    c->hw_dirty = 1;
}

void glAlphaFunc(GLenum func, GLclampf ref)
{
    GET_CTX(c);
    RECORD(c, OP_ALPHA_FUNC, 2, func, ref);
    OUTSIDE_BEGIN(c);
    if (func < GL_NEVER || func > GL_ALWAYS) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->alpha_func = func;
    c->alpha_ref = gl_clampf(ref);
    c->hw_dirty = 1;
}

void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
    GET_CTX(c);
    RECORD(c, OP_COLOR_MASK, 4, red, green, blue, alpha);
    OUTSIDE_BEGIN(c);
    c->color_mask[0] = red != 0;
    c->color_mask[1] = green != 0;
    c->color_mask[2] = blue != 0;
    c->color_mask[3] = alpha != 0;
    c->hw_dirty = 1;
}

/* The stencil buffer has 8 bits: references are clamped to them, and
 * masks and clear values are taken modulo 256 where it is read. */
void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
    GET_CTX(c);
    RECORD(c, OP_STENCIL_FUNC, 3, func, ref, mask & 0xFF);
    OUTSIDE_BEGIN(c);
    if (func < GL_NEVER || func > GL_ALWAYS) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->stencil_func = func;
    c->stencil_ref = ref < 0 ? 0 : ref > 255 ? 255 : ref;
    c->stencil_value_mask = mask;
}

static int stencil_op_ok(GLenum op)
{
    return op == GL_KEEP || op == GL_ZERO || op == GL_REPLACE || op == GL_INCR || op == GL_DECR ||
           op == GL_INVERT || op == GL_INCR_WRAP_EXT || op == GL_DECR_WRAP_EXT;
}

void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
    GET_CTX(c);
    RECORD(c, OP_STENCIL_OP, 3, fail, zfail, zpass);
    OUTSIDE_BEGIN(c);
    if (!stencil_op_ok(fail) || !stencil_op_ok(zfail) || !stencil_op_ok(zpass)) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->stencil_fail = fail;
    c->stencil_zfail = zfail;
    c->stencil_zpass = zpass;
}

void glStencilMask(GLuint mask)
{
    GET_CTX(c);
    RECORD(c, OP_STENCIL_MASK, 1, mask & 0xFF);
    OUTSIDE_BEGIN(c);
    c->stencil_writemask = mask;
}

void glClearStencil(GLint s)
{
    GET_CTX(c);
    RECORD(c, OP_CLEAR_STENCIL, 1, s & 0xFF);
    OUTSIDE_BEGIN(c);
    c->clear_stencil = s;
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

/* ---- fog ---- */

float gl_fog_factor(struct fgl_context *c, float d)
{
    float f;
    switch (c->fog_mode) {
    case GL_LINEAR:
        f = c->fog_end != c->fog_start ? (c->fog_end - d) / (c->fog_end - c->fog_start) : 1;
        break;
    case GL_EXP:
        f = expf(-c->fog_density * d);
        break;
    default: {                          /* GL_EXP2 */
        float e = c->fog_density * d;
        f = expf(-e * e);
        break;
    }
    }
    return gl_clampf(f);
}

void glFogfv(GLenum pname, const GLfloat *p)
{
    GET_CTX(c);
    float a[4] = { p[0], 0, 0, 0 };
    if (pname == GL_FOG_COLOR)
        memcpy(a, p, sizeof(a));
    RECORD(c, OP_FOG, 5, pname, a[0], a[1], a[2], a[3]);
    OUTSIDE_BEGIN(c);
    switch (pname) {
    case GL_FOG_MODE: {
        GLenum m = (GLenum)a[0];
        if (m != GL_LINEAR && m != GL_EXP && m != GL_EXP2) {
            gl_error(c, GL_INVALID_ENUM);
            return;
        }
        c->fog_mode = m;
        return;
    }
    case GL_FOG_DENSITY:
        if (a[0] < 0) {
            gl_error(c, GL_INVALID_VALUE);
            return;
        }
        c->fog_density = a[0];
        return;
    case GL_FOG_START: c->fog_start = a[0]; return;
    case GL_FOG_END: c->fog_end = a[0]; return;
    case GL_FOG_INDEX: c->fog_index = a[0]; return;
    case GL_FOG_COLOR:
        for (int i = 0; i < 4; i++)
            c->fog_color[i] = gl_clampf(a[i]);
        return;
    }
    gl_error(c, GL_INVALID_ENUM);
}

void glFogf(GLenum pname, GLfloat param)
{
    glFogfv(pname, &param);
}

void glFogi(GLenum pname, GLint param)
{
    GLfloat f = (GLfloat)param;
    glFogfv(pname, &f);
}

void glFogiv(GLenum pname, const GLint *params)
{
    GLfloat v[4] = { (GLfloat)params[0], 0, 0, 0 };
    if (pname == GL_FOG_COLOR)
        for (int i = 0; i < 4; i++)
            v[i] = gl_int_color(params[i]);
    glFogfv(pname, v);
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
    if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT)) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    if (!c->depth_mask)
        mask &= ~(GLbitfield)GL_DEPTH_BUFFER_BIT;
    if (!gl_rgb_write_mask(c))
        mask &= ~(GLbitfield)GL_COLOR_BUFFER_BIT;
    /* Only software has a stencil buffer (a context that goes there later
     * starts with it cleared); nothing has an accumulation buffer. */
    if (!(c->stencil_writemask & 0xFF) || c->be != &gl_soft_backend)
        mask &= ~(GLbitfield)GL_STENCIL_BUFFER_BIT;
    mask &= GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    if (mask & GL_COLOR_BUFFER_BIT)
        gl_check_backend(c);
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

/* Parameters that are colors, which integers give as fractions of their range. */
static int is_color(GLenum pname)
{
    return pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR || pname == GL_EMISSION ||
           pname == GL_AMBIENT_AND_DIFFUSE || pname == GL_LIGHT_MODEL_AMBIENT;
}

/* The integer parameters of glLightiv and the like as floats. */
static void int_params(GLenum pname, const GLint *p, float *out)
{
    int n = nparams(pname), color = is_color(pname);
    for (int i = 0; i < n; i++)
        out[i] = color ? gl_int_color(p[i]) : (float)p[i];
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

void glLightiv(GLenum light, GLenum pname, const GLint *params)
{
    float v[4];
    int_params(pname, params, v);
    glLightfv(light, pname, v);
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

void glLightModeliv(GLenum pname, const GLint *params)
{
    float v[4];
    int_params(pname, params, v);
    glLightModelfv(pname, v);
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

void glMaterialiv(GLenum face, GLenum pname, const GLint *params)
{
    float v[4];
    int_params(pname, params, v);
    glMaterialfv(face, pname, v);
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

/* A light's parameter; the number of values, 0 if there is no such light or parameter. */
static int light_values(struct fgl_context *c, GLenum light, GLenum pname, float *v)
{
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + MAX_LIGHTS)
        return 0;
    const struct light *l = &c->lights[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT: memcpy(v, l->ambient, 4 * sizeof(float)); return 4;
    case GL_DIFFUSE: memcpy(v, l->diffuse, 4 * sizeof(float)); return 4;
    case GL_SPECULAR: memcpy(v, l->specular, 4 * sizeof(float)); return 4;
    case GL_POSITION: memcpy(v, l->position, 4 * sizeof(float)); return 4;
    case GL_SPOT_DIRECTION: memcpy(v, l->spot_dir, 3 * sizeof(float)); return 3;
    case GL_SPOT_EXPONENT: v[0] = l->spot_exp; return 1;
    case GL_SPOT_CUTOFF: v[0] = l->spot_cutoff; return 1;
    case GL_CONSTANT_ATTENUATION: v[0] = l->att[0]; return 1;
    case GL_LINEAR_ATTENUATION: v[0] = l->att[1]; return 1;
    case GL_QUADRATIC_ATTENUATION: v[0] = l->att[2]; return 1;
    }
    return 0;
}

/* The material's parameter (back faces share the front's); the number of values. */
static int material_values(struct fgl_context *c, GLenum face, GLenum pname, float *v)
{
    if (face != GL_FRONT && face != GL_BACK)
        return 0;
    const struct material *m = &c->mat;
    switch (pname) {
    case GL_AMBIENT: memcpy(v, m->ambient, 4 * sizeof(float)); return 4;
    case GL_DIFFUSE: memcpy(v, m->diffuse, 4 * sizeof(float)); return 4;
    case GL_SPECULAR: memcpy(v, m->specular, 4 * sizeof(float)); return 4;
    case GL_EMISSION: memcpy(v, m->emission, 4 * sizeof(float)); return 4;
    case GL_SHININESS: v[0] = m->shininess; return 1;
    }
    return 0;
}

/* Floats as glGet*iv gives them: colors over the whole int range, the rest rounded. */
static void to_ints(const float *v, int n, int color, GLint *out)
{
    for (int i = 0; i < n; i++)
        out[i] = color ? (GLint)((double)v[i] * 2147483647.0) : (GLint)(v[i] < 0 ? v[i] - 0.5f : v[i] + 0.5f);
}

void glGetLightfv(GLenum light, GLenum pname, GLfloat *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    float v[4];
    int n = light_values(c, light, pname, v);
    if (!n)
        gl_error(c, GL_INVALID_ENUM);
    memcpy(params, v, (size_t)n * sizeof(float));
}

void glGetLightiv(GLenum light, GLenum pname, GLint *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    float v[4];
    int n = light_values(c, light, pname, v);
    if (!n)
        gl_error(c, GL_INVALID_ENUM);
    to_ints(v, n, is_color(pname), params);
}

void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    float v[4];
    int n = material_values(c, face, pname, v);
    if (!n)
        gl_error(c, GL_INVALID_ENUM);
    memcpy(params, v, (size_t)n * sizeof(float));
}

void glGetMaterialiv(GLenum face, GLenum pname, GLint *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    float v[4];
    int n = material_values(c, face, pname, v);
    if (!n)
        gl_error(c, GL_INVALID_ENUM);
    to_ints(v, n, is_color(pname), params);
}

/* ---- the attribute stacks ---- */

/* A glPushAttrib: the whole state as it was, of which the groups in mask
 * come back, and the parameters of the textures bound then. */
struct attrib_frame {
    GLbitfield mask;
    struct fgl_context s;
    struct {
        GLenum min_filter, mag_filter, wrap_s, wrap_t;
        float border[4], priority;
    } tex[MAX_UNITS];
};

struct client_frame {
    GLbitfield mask;
    int pack_align, pack_row_length, pack_skip_rows, pack_skip_pixels;
    int unpack_align, unpack_row_length, unpack_skip_rows, unpack_skip_pixels, unpack_lsb_first;
    struct varray arrays[NARRAYS];
    int client_unit;
};

void glPushAttrib(GLbitfield mask)
{
    GET_CTX(c);
    RECORD(c, OP_PUSH_ATTRIB, 1, (float)(mask & 0xFFFFF));     /* the groups there are fit a float */
    OUTSIDE_BEGIN(c);
    if (c->attrib_depth == ATTRIB_DEPTH) {
        gl_error(c, GL_STACK_OVERFLOW);
        return;
    }
    struct attrib_frame *f = malloc(sizeof(*f));
    if (!f) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    f->mask = mask;
    memcpy(&f->s, c, sizeof(*c));
    for (int u = 0; u < MAX_UNITS; u++) {
        GLuint name = c->unit[u].bound;
        const struct gltex *t = name < c->ntextures ? c->textures[name] : NULL;
        if (t) {
            f->tex[u].min_filter = t->min_filter;
            f->tex[u].mag_filter = t->mag_filter;
            f->tex[u].wrap_s = t->wrap_s;
            f->tex[u].wrap_t = t->wrap_t;
            memcpy(f->tex[u].border, t->border, sizeof(t->border));
            f->tex[u].priority = t->priority;
        }
    }
    c->attrib[c->attrib_depth++] = f;
}

#define R(field) memcpy(&c->field, &s->field, sizeof(c->field))

static void restore_enables(struct fgl_context *c, const struct fgl_context *s)
{
    R(alpha_test); R(blend); R(clip_on); R(color_material); R(cull); R(depth_test); R(dither); R(fog);
    R(light_on); R(lighting); R(normalize); R(offset_fill); R(offset_line); R(offset_point);
    R(scissor_test); R(stencil_test);
    for (int u = 0; u < MAX_UNITS; u++) {
        R(unit[u].enabled);
        R(unit[u].gen_on);
    }
}

void glPopAttrib(void)
{
    GET_CTX(c);
    RECORD0(c, OP_POP_ATTRIB);
    OUTSIDE_BEGIN(c);
    if (c->attrib_depth == 0) {
        gl_error(c, GL_STACK_UNDERFLOW);
        return;
    }
    struct attrib_frame *f = c->attrib[--c->attrib_depth];
    const struct fgl_context *s = &f->s;
    GLbitfield m = f->mask;
    if (m & GL_CURRENT_BIT) {
        R(color); R(normal); R(raster_pos); R(raster_color); R(raster_tex); R(raster_distance);
        R(raster_valid);
        for (int u = 0; u < MAX_UNITS; u++)
            R(unit[u].coord);
        c->eye_normal_valid = 0;
    }
    if (m & GL_POINT_BIT)
        R(point_size);
    if (m & GL_LINE_BIT)
        R(line_width);
    if (m & GL_POLYGON_BIT) {
        R(cull); R(cull_face); R(front_face); R(polygon_mode); R(offset_fill); R(offset_line);
        R(offset_point); R(offset_factor); R(offset_units);
    }
    if (m & GL_PIXEL_MODE_BIT) {
        R(read_buffer); R(zoom_x); R(zoom_y);
    }
    if (m & GL_LIGHTING_BIT) {
        R(lighting); R(color_material); R(cm_face); R(cm_mode); R(mat); R(model_ambient); R(lights);
        R(light_on); R(shade_model);
    }
    if (m & GL_FOG_BIT) {
        R(fog); R(fog_mode); R(fog_density); R(fog_start); R(fog_end); R(fog_index); R(fog_color);
    }
    if (m & GL_DEPTH_BUFFER_BIT) {
        R(depth_test); R(depth_func); R(clear_depth); R(depth_mask);
    }
    if (m & GL_STENCIL_BUFFER_BIT) {
        R(stencil_test); R(stencil_func); R(stencil_ref); R(stencil_value_mask); R(stencil_fail);
        R(stencil_zfail); R(stencil_zpass); R(stencil_writemask); R(clear_stencil);
    }
    if (m & GL_VIEWPORT_BIT) {
        R(vp_x); R(vp_y); R(vp_w); R(vp_h); R(depth_near); R(depth_far);
    }
    if (m & GL_TRANSFORM_BIT) {
        R(matrix_mode); R(normalize); R(clip_plane); R(clip_on);
        c->eye_normal_valid = 0;
    }
    if (m & GL_ENABLE_BIT)
        restore_enables(c, s);
    if (m & GL_COLOR_BUFFER_BIT) {
        R(alpha_test); R(alpha_func); R(alpha_ref); R(blend); R(blend_src); R(blend_dst); R(dither);
        R(draw_buffer); R(color_mask); R(clear_color);
    }
    if (m & GL_HINT_BIT)
        R(hints);
    if (m & GL_LIST_BIT)
        R(list_base);
    if (m & GL_TEXTURE_BIT) {
        R(active_unit);
        for (int u = 0; u < MAX_UNITS; u++) {
            R(unit[u].enabled); R(unit[u].env_mode); R(unit[u].env_color); R(unit[u].gen_on);
            R(unit[u].gen_mode); R(unit[u].gen_object); R(unit[u].gen_eye);
            /* The binding, if the texture is still there, and its parameters. */
            GLuint name = s->unit[u].bound;
            struct gltex *t = name < c->ntextures ? c->textures[name] : NULL;
            c->unit[u].bound = t ? name : 0;
            if (t) {
                t->min_filter = f->tex[u].min_filter;
                t->mag_filter = f->tex[u].mag_filter;
                t->wrap_s = f->tex[u].wrap_s;
                t->wrap_t = f->tex[u].wrap_t;
                memcpy(t->border, f->tex[u].border, sizeof(t->border));
                t->priority = f->tex[u].priority;
            }
        }
    }
    if (m & GL_SCISSOR_BIT) {
        R(scissor_test); R(scissor_box);
    }
    free(f);
    c->hw_dirty = 1;
}

void glPushClientAttrib(GLbitfield mask)
{
    GET_CTX(c);
    if (c->client_depth == ATTRIB_DEPTH) {
        gl_error(c, GL_STACK_OVERFLOW);
        return;
    }
    struct client_frame *f = malloc(sizeof(*f));
    if (!f) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    f->mask = mask;
    f->pack_align = c->pack_align;
    f->pack_row_length = c->pack_row_length;
    f->pack_skip_rows = c->pack_skip_rows;
    f->pack_skip_pixels = c->pack_skip_pixels;
    f->unpack_align = c->unpack_align;
    f->unpack_row_length = c->unpack_row_length;
    f->unpack_skip_rows = c->unpack_skip_rows;
    f->unpack_skip_pixels = c->unpack_skip_pixels;
    f->unpack_lsb_first = c->unpack_lsb_first;
    memcpy(f->arrays, c->arrays, sizeof(c->arrays));
    f->client_unit = c->client_unit;
    c->client_attrib[c->client_depth++] = f;
}

void glPopClientAttrib(void)
{
    GET_CTX(c);
    if (c->client_depth == 0) {
        gl_error(c, GL_STACK_UNDERFLOW);
        return;
    }
    struct client_frame *s = c->client_attrib[--c->client_depth];
    if (s->mask & GL_CLIENT_PIXEL_STORE_BIT) {
        R(pack_align); R(pack_row_length); R(pack_skip_rows); R(pack_skip_pixels);
        R(unpack_align); R(unpack_row_length); R(unpack_skip_rows); R(unpack_skip_pixels);
        R(unpack_lsb_first);
    }
    if (s->mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
        R(arrays); R(client_unit);
    }
    free(s);
}

#undef R

/* ---- queries ---- */

#define EXTENSIONS_HW "GL_EXT_bgra GL_EXT_compiled_vertex_array GL_EXT_rescale_normal GL_EXT_stencil_wrap " \
                      "GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp"

const GLubyte *glGetString(GLenum name)
{
    struct fgl_context *c = gl_ctx;
    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"fluxOS";
    case GL_RENDERER: return (const GLubyte *)(c ? c->renderer : "none");
    case GL_VERSION: return (const GLubyte *)"1.1 fluxOS";
    case GL_EXTENSIONS:
        /* The hardware cannot draw two textures at once, so a program with
         * another way had better take it there. */
        if (c && c->be == &gl_soft_backend)
            return (const GLubyte *)("GL_ARB_multitexture " EXTENSIONS_HW);
        return (const GLubyte *)EXTENSIONS_HW;
    }
    if (c)
        gl_error(c, GL_INVALID_ENUM);
    return NULL;
}

/* Every glGet goes through doubles; returns the number of values. */
static int get_values(struct fgl_context *c, GLenum pname, double *v)
{
    const struct texunit *u = &c->unit[c->active_unit];
    switch (pname) {
    case GL_MODELVIEW_MATRIX:
    case GL_PROJECTION_MATRIX:
    case GL_TEXTURE_MATRIX: {
        const float *m = pname == GL_MODELVIEW_MATRIX ? c->modelview[c->mv_depth]
                         : pname == GL_TEXTURE_MATRIX ? u->matrix[u->depth]
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
    case GL_CURRENT_TEXTURE_COORDS:
        for (int i = 0; i < 4; i++)
            v[i] = u->coord[i];
        return 4;
    case GL_TEXTURE_ENV_COLOR:
        for (int i = 0; i < 4; i++)
            v[i] = u->env_color[i];
        return 4;
    case GL_TEXTURE_ENV_MODE: v[0] = u->env_mode; return 1;
    case GL_TEXTURE_BINDING_2D: v[0] = u->bound; return 1;
    case GL_MAX_TEXTURE_SIZE: v[0] = MAX_TEXTURE; return 1;
    case GL_MAX_TEXTURE_STACK_DEPTH: v[0] = TEXTURE_DEPTH; return 1;
    case GL_UNPACK_ALIGNMENT: v[0] = c->unpack_align; return 1;
    case GL_UNPACK_ROW_LENGTH: v[0] = c->unpack_row_length; return 1;
    case GL_UNPACK_SKIP_ROWS: v[0] = c->unpack_skip_rows; return 1;
    case GL_UNPACK_SKIP_PIXELS: v[0] = c->unpack_skip_pixels; return 1;
    case GL_UNPACK_LSB_FIRST: v[0] = c->unpack_lsb_first; return 1;
    case GL_UNPACK_SWAP_BYTES: case GL_PACK_SWAP_BYTES: case GL_PACK_LSB_FIRST: v[0] = 0; return 1;
    case GL_PACK_ALIGNMENT: v[0] = c->pack_align; return 1;
    case GL_DEPTH_RANGE:
        v[0] = c->depth_near; v[1] = c->depth_far;
        return 2;
    case GL_MAX_VIEWPORT_DIMS:
        v[0] = v[1] = MAX_VIEWPORT;
        return 2;
    case GL_MATRIX_MODE: v[0] = c->matrix_mode; return 1;
    case GL_MODELVIEW_STACK_DEPTH: v[0] = c->mv_depth + 1; return 1;
    case GL_PROJECTION_STACK_DEPTH: v[0] = c->proj_depth + 1; return 1;
    case GL_TEXTURE_STACK_DEPTH: v[0] = u->depth + 1; return 1;
    case GL_SHADE_MODEL: v[0] = c->shade_model; return 1;
    case GL_CULL_FACE_MODE: v[0] = c->cull_face; return 1;
    case GL_FRONT_FACE: v[0] = c->front_face; return 1;
    case GL_DEPTH_FUNC: v[0] = c->depth_func; return 1;
    case GL_DEPTH_WRITEMASK: v[0] = c->depth_mask; return 1;
    case GL_DEPTH_CLEAR_VALUE: v[0] = c->clear_depth; return 1;
    case GL_POINT_SIZE: v[0] = c->point_size; return 1;
    case GL_LINE_WIDTH: v[0] = c->line_width; return 1;
    case GL_POINT_SIZE_RANGE: case GL_LINE_WIDTH_RANGE:
        v[0] = 1; v[1] = 256;
        return 2;
    case GL_POINT_SIZE_GRANULARITY: case GL_LINE_WIDTH_GRANULARITY: v[0] = 0.125; return 1;
    case GL_MAX_LIGHTS: v[0] = MAX_LIGHTS; return 1;
    case GL_MAX_CLIP_PLANES: v[0] = MAX_CLIP_PLANES; return 1;
    case GL_MAX_MODELVIEW_STACK_DEPTH: v[0] = MODELVIEW_DEPTH; return 1;
    case GL_MAX_PROJECTION_STACK_DEPTH: v[0] = PROJECTION_DEPTH; return 1;
    case GL_MAX_ATTRIB_STACK_DEPTH: case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH: v[0] = ATTRIB_DEPTH; return 1;
    case GL_ATTRIB_STACK_DEPTH: v[0] = c->attrib_depth; return 1;
    case GL_CLIENT_ATTRIB_STACK_DEPTH: v[0] = c->client_depth; return 1;
    case GL_MAX_TEXTURE_UNITS_ARB: v[0] = MAX_UNITS; return 1;
    case GL_ACTIVE_TEXTURE_ARB: v[0] = GL_TEXTURE0_ARB + c->active_unit; return 1;
    case GL_CLIENT_ACTIVE_TEXTURE_ARB: v[0] = GL_TEXTURE0_ARB + c->client_unit; return 1;
    case GL_RED_BITS: case GL_BLUE_BITS:
        v[0] = c->be == &gl_tdfx_backend ? 5 : 8;
        return 1;
    case GL_GREEN_BITS:
        v[0] = c->be == &gl_tdfx_backend ? 6 : 8;
        return 1;
    case GL_ALPHA_BITS: case GL_INDEX_BITS: case GL_AUX_BUFFERS: case GL_INDEX_MODE: case GL_STEREO:
    case GL_ACCUM_RED_BITS: case GL_ACCUM_GREEN_BITS: case GL_ACCUM_BLUE_BITS: case GL_ACCUM_ALPHA_BITS:
        v[0] = 0;
        return 1;
    case GL_RGBA_MODE: v[0] = 1; return 1;
    case GL_SUBPIXEL_BITS: v[0] = 4; return 1;
    case GL_DEPTH_BITS: v[0] = gl_depth_bits(c); return 1;
    case GL_STENCIL_BITS: v[0] = 8; return 1;          /* in software, where the stencil test moves a context */
    case GL_BLEND_SRC: v[0] = c->blend_src; return 1;
    case GL_BLEND_DST: v[0] = c->blend_dst; return 1;
    case GL_ALPHA_TEST_FUNC: v[0] = c->alpha_func; return 1;
    case GL_ALPHA_TEST_REF: v[0] = c->alpha_ref; return 1;
    case GL_STENCIL_FUNC: v[0] = c->stencil_func; return 1;
    case GL_STENCIL_REF: v[0] = c->stencil_ref; return 1;
    case GL_STENCIL_VALUE_MASK: v[0] = c->stencil_value_mask; return 1;
    case GL_STENCIL_WRITEMASK: v[0] = c->stencil_writemask; return 1;
    case GL_STENCIL_FAIL: v[0] = c->stencil_fail; return 1;
    case GL_STENCIL_PASS_DEPTH_FAIL: v[0] = c->stencil_zfail; return 1;
    case GL_STENCIL_PASS_DEPTH_PASS: v[0] = c->stencil_zpass; return 1;
    case GL_STENCIL_CLEAR_VALUE: v[0] = c->clear_stencil; return 1;
    case GL_FOG_MODE: v[0] = c->fog_mode; return 1;
    case GL_FOG_DENSITY: v[0] = c->fog_density; return 1;
    case GL_FOG_START: v[0] = c->fog_start; return 1;
    case GL_FOG_END: v[0] = c->fog_end; return 1;
    case GL_FOG_INDEX: v[0] = c->fog_index; return 1;
    case GL_FOG_COLOR:
        for (int i = 0; i < 4; i++)
            v[i] = c->fog_color[i];
        return 4;
    case GL_LIGHT_MODEL_AMBIENT:
        for (int i = 0; i < 4; i++)
            v[i] = c->model_ambient[i];
        return 4;
    case GL_LIGHT_MODEL_TWO_SIDE: case GL_LIGHT_MODEL_LOCAL_VIEWER: v[0] = 0; return 1;
    case GL_COLOR_MATERIAL_FACE: v[0] = c->cm_face; return 1;
    case GL_COLOR_MATERIAL_PARAMETER: v[0] = c->cm_mode; return 1;
    case GL_COLOR_WRITEMASK:
        for (int i = 0; i < 4; i++)
            v[i] = c->color_mask[i];
        return 4;
    case GL_POLYGON_MODE:
        v[0] = c->polygon_mode[0]; v[1] = c->polygon_mode[1];
        return 2;
    case GL_POLYGON_OFFSET_FACTOR: v[0] = c->offset_factor; return 1;
    case GL_POLYGON_OFFSET_UNITS: v[0] = c->offset_units; return 1;
    case GL_DRAW_BUFFER: v[0] = c->draw_buffer; return 1;
    case GL_READ_BUFFER: v[0] = c->read_buffer; return 1;
    case GL_DOUBLEBUFFER: v[0] = 1; return 1;
    case GL_PACK_ROW_LENGTH: v[0] = c->pack_row_length; return 1;
    case GL_PACK_SKIP_ROWS: v[0] = c->pack_skip_rows; return 1;
    case GL_PACK_SKIP_PIXELS: v[0] = c->pack_skip_pixels; return 1;
    case GL_LIST_BASE: v[0] = c->list_base; return 1;
    case GL_LIST_INDEX: v[0] = c->compiling ? c->compiling_id : 0; return 1;
    case GL_LIST_MODE: v[0] = c->compiling ? c->list_mode : 0; return 1;
    case GL_MAX_LIST_NESTING: v[0] = 64; return 1;
    case GL_CURRENT_RASTER_POSITION:
        for (int i = 0; i < 4; i++)
            v[i] = c->raster_pos[i];
        return 4;
    case GL_CURRENT_RASTER_POSITION_VALID: v[0] = c->raster_valid; return 1;
    case GL_CURRENT_RASTER_COLOR:
        for (int i = 0; i < 4; i++)
            v[i] = c->raster_color[i];
        return 4;
    case GL_CURRENT_RASTER_TEXTURE_COORDS:
        for (int i = 0; i < 4; i++)
            v[i] = c->raster_tex[c->active_unit][i];
        return 4;
    case GL_CURRENT_RASTER_DISTANCE: v[0] = c->raster_distance; return 1;
    case GL_CURRENT_RASTER_INDEX: v[0] = 1; return 1;
    case GL_ZOOM_X: v[0] = c->zoom_x; return 1;
    case GL_ZOOM_Y: v[0] = c->zoom_y; return 1;
    case GL_ARRAY_ELEMENT_LOCK_FIRST_EXT: v[0] = c->lock_first; return 1;
    case GL_ARRAY_ELEMENT_LOCK_COUNT_EXT: v[0] = c->lock_count; return 1;
    }
    if (pname >= GL_PERSPECTIVE_CORRECTION_HINT && pname <= GL_FOG_HINT) {
        v[0] = c->hints[pname - GL_PERSPECTIVE_CORRECTION_HINT];
        return 1;
    }
    {
        int n = gl_array_get(c, pname, v);
        if (n)
            return n;
    }
    {
        int *flag = cap_flag(c, pname);
        if (flag) {
            v[0] = *flag;
            return 1;
        }
        if (lacking(pname)) {
            v[0] = 0;
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
    int colors = pname == GL_CURRENT_COLOR || pname == GL_COLOR_CLEAR_VALUE || pname == GL_TEXTURE_ENV_COLOR ||
                 pname == GL_ALPHA_TEST_REF || pname == GL_FOG_COLOR || pname == GL_CURRENT_RASTER_COLOR ||
                 pname == GL_LIGHT_MODEL_AMBIENT;
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
    c->polygon_mode[0] = c->polygon_mode[1] = GL_FILL;
    for (int i = 0; i < 5; i++)
        c->hints[i] = GL_DONT_CARE;
    c->blend_src = GL_ONE;
    c->blend_dst = GL_ZERO;
    c->alpha_func = GL_ALWAYS;
    for (int i = 0; i < 4; i++)
        c->color_mask[i] = 1;
    c->draw_buffer = c->read_buffer = GL_BACK;
    c->pack_align = 4;
    c->stencil_func = GL_ALWAYS;
    c->stencil_fail = c->stencil_zfail = c->stencil_zpass = GL_KEEP;
    c->stencil_value_mask = c->stencil_writemask = ~0u;
    c->fog_mode = GL_EXP;
    c->fog_density = 1;
    c->fog_end = 1;
    c->arrays[ARRAY_VERTEX].size = 4;
    c->arrays[ARRAY_COLOR].size = 4;
    for (int u = 0; u < MAX_UNITS; u++)
        c->arrays[ARRAY_TEXCOORD + u].size = 4;
    for (int i = 0; i < NARRAYS; i++)
        c->arrays[i].type = GL_FLOAT;
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
        c->color[k] = c->raster_color[k] = 1;
    c->normal[2] = 1;
    c->raster_pos[3] = 1;
    c->raster_valid = 1;
    for (int u = 0; u < MAX_UNITS; u++)
        c->raster_tex[u][3] = 1;
    c->zoom_x = c->zoom_y = 1;
    c->hw_dirty = 1;
    gl_tex_init(c);
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
    if (!(flags & FGL_NO_INTEL) && (!only || !strcmp(only, "intel")) && gl_intel_attach(c) == 0)
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
            gl_tex_free_all(c);
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
    gl_tex_free_all(c);
    c->be->destroy(c);
    gl_free_lists(c);
    while (c->attrib_depth)
        free(c->attrib[--c->attrib_depth]);
    while (c->client_depth)
        free(c->client_attrib[--c->client_depth]);
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

int gl_depth_bits(struct fgl_context *c)
{
    return c->be == &gl_tdfx_backend ? 16 : c->be == &gl_radeon_backend || c->be == &gl_intel_backend ? 24 : 30;
}

/* Fog can be worked out at the vertices and folded into their colors when
 * the colors are not textured after, or textured in GL_MODULATE into black
 * fog (f C T + (1 - f) 0 is (f C) T); otherwise it is mixed in at each
 * fragment, after texturing. */
static int fog_folds(struct fgl_context *c)
{
    if (gl_unit_texture(c, 1))
        return 0;
    if (!gl_unit_texture(c, 0))
        return 1;
    return c->unit[0].env_mode == GL_MODULATE && c->fog_color[0] == 0 && c->fog_color[1] == 0 &&
           c->fog_color[2] == 0;
}

void gl_check_backend(struct fgl_context *c)
{
    c->fog_draw = !c->fog ? FOG_NONE : fog_folds(c) ? FOG_FOLD : FOG_PIXEL;
    if (c->be == &gl_soft_backend)
        return;
    /* What only software does: the stencil test, a second texture, fog after texturing. */
    if (c->stencil_test || gl_unit_texture(c, 1) || c->fog_draw == FOG_PIXEL ||
        (c->be == &gl_tdfx_backend && !gl_tdfx_can_draw(c)))
        gl_use_software(c, 1);
}

/* Fall back to drawing in memory if the hardware cannot keep the context
 * (or cannot draw what it is asked to), perhaps with the picture so far,
 * and its depths if they can be read. */
int gl_use_software(struct fgl_context *c, int keep_picture)
{
    size_t n = (size_t)c->width * (size_t)c->height;
    uint32_t *picture = keep_picture ? malloc(n * 4) : NULL;
    float *depths = keep_picture && c->be->read_depth ? malloc(n * sizeof(float)) : NULL;
    if (picture || depths)
        c->be->finish(c);
    if (picture)
        c->be->read(c, picture, c->width);
    if (depths)
        c->be->read_depth(c, 0, 0, c->width, c->height, depths);
    c->be->destroy(c);
    c->be = &gl_soft_backend;
    strcpy(c->renderer, gl_soft_backend.name);
    int r = c->be->resize(c);
    if (r == 0)
        gl_soft_load(c, picture, depths);
    free(picture);
    free(depths);
    c->hw_dirty = 1;
    return r;
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
    if (c->be != &gl_soft_backend && gl_use_software(c, 0) == 0)
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
        (c->be == &gl_radeon_backend && gl_radeon_check(c) < 0) ||
        (c->be == &gl_intel_backend && gl_intel_check(c) < 0))
        gl_use_software(c, 0);
}

int fglIsAccelerated(FGLcontext *c)
{
    return c && c->be != &gl_soft_backend;
}

int fglGetDriver(FGLcontext *c)
{
    if (c && c->be == &gl_radeon_backend)
        return FGL_DRIVER_RADEON;
    if (c && c->be == &gl_intel_backend)
        return FGL_DRIVER_INTEL;
    if (c && c->be == &gl_tdfx_backend)
        return FGL_DRIVER_TDFX;
    return FGL_DRIVER_SOFTWARE;
}

/* The calls programs look up by name: the extensions', and GL 1.3's names
 * for GL_ARB_multitexture. */
#define MULTITEXTURE_PROCS(sfx)                                                                       \
    P(glActiveTexture##sfx) P(glClientActiveTexture##sfx)                                             \
    P(glMultiTexCoord1d##sfx) P(glMultiTexCoord1f##sfx) P(glMultiTexCoord1i##sfx) P(glMultiTexCoord1s##sfx) \
    P(glMultiTexCoord2d##sfx) P(glMultiTexCoord2f##sfx) P(glMultiTexCoord2i##sfx) P(glMultiTexCoord2s##sfx) \
    P(glMultiTexCoord3d##sfx) P(glMultiTexCoord3f##sfx) P(glMultiTexCoord3i##sfx) P(glMultiTexCoord3s##sfx) \
    P(glMultiTexCoord4d##sfx) P(glMultiTexCoord4f##sfx) P(glMultiTexCoord4i##sfx) P(glMultiTexCoord4s##sfx) \
    P(glMultiTexCoord1dv##sfx) P(glMultiTexCoord1fv##sfx) P(glMultiTexCoord1iv##sfx) P(glMultiTexCoord1sv##sfx) \
    P(glMultiTexCoord2dv##sfx) P(glMultiTexCoord2fv##sfx) P(glMultiTexCoord2iv##sfx) P(glMultiTexCoord2sv##sfx) \
    P(glMultiTexCoord3dv##sfx) P(glMultiTexCoord3fv##sfx) P(glMultiTexCoord3iv##sfx) P(glMultiTexCoord3sv##sfx) \
    P(glMultiTexCoord4dv##sfx) P(glMultiTexCoord4fv##sfx) P(glMultiTexCoord4iv##sfx) P(glMultiTexCoord4sv##sfx)

void *fglGetProcAddress(const char *name)
{
#define P(f) { #f, (void *)f },
    static const struct {
        const char *name;
        void *fn;
    } procs[] = {
        MULTITEXTURE_PROCS(ARB)
        MULTITEXTURE_PROCS()
        P(glLockArraysEXT) P(glUnlockArraysEXT)
    };
#undef P
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(procs) / sizeof(procs[0]); i++)
        if (!strcmp(procs[i].name, name))
            return procs[i].fn;
    return NULL;
}
