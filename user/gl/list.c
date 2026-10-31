/*
 * Display lists. While a list is being compiled, each GL call that can go
 * in one records an opcode, its argument count and its arguments (all as
 * floats; enums and list and texture names fit exactly, up to 2^24).
 * glTexImage2D, glTexSubImage2D, glCopyTexImage2D, glDrawPixels and
 * glCopyPixels are not recorded: they take effect when called, even under
 * GL_COMPILE. Nor are the vertex array calls, but glDrawArrays and the
 * like record the vertices they draw, as GL has it. glBitmap records its
 * bits as they were unpacked then. glCallList replays them
 * through the same entry points, with recording held off so that a list
 * called during GL_COMPILE_AND_EXECUTE is not copied into the new one.
 * glCallLists records its list names as given; glListBase is added to
 * them when the list runs.
 */
#include <stdlib.h>
#include <string.h>
#include "glint.h"

#define MAX_CALL_DEPTH 64               /* lists calling lists */

#define GET_CTX(c)                      \
    struct fgl_context *c = gl_ctx;     \
    if (!c)                             \
        return

static void list_free(struct dlist *l)
{
    if (l) {
        free(l->ops);
        free(l);
    }
}

int gl_record(struct fgl_context *c, int op, int nargs, const float *args)
{
    struct dlist *l = c->compiling;
    if (l->n + 2 + nargs > l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        while (cap < l->n + 2 + nargs)
            cap *= 2;
        float *ops = realloc(l->ops, (size_t)cap * sizeof(float));
        if (!ops) {
            gl_error(c, GL_OUT_OF_MEMORY);
            return c->list_mode == GL_COMPILE;
        }
        l->ops = ops;
        l->cap = cap;
    }
    l->ops[l->n++] = (float)op;
    l->ops[l->n++] = (float)nargs;
    for (int i = 0; i < nargs; i++)
        l->ops[l->n++] = args[i];
    return c->list_mode == GL_COMPILE;
}

void gl_free_lists(struct fgl_context *c)
{
    for (GLuint i = 0; i < c->nlists; i++)
        list_free(c->lists[i]);
    free(c->lists);
    c->lists = NULL;
    c->nlists = 0;
    list_free(c->compiling);
    c->compiling = NULL;
}

/* Make sure slot id exists. */
static int ensure_slot(struct fgl_context *c, GLuint id)
{
    if (id < c->nlists)
        return 0;
    if (id >= 0x10000000u)              /* so n * 2 and n * sizeof cannot wrap */
        return -1;
    GLuint n = c->nlists ? c->nlists : 16;
    while (n <= id)
        n *= 2;
    struct dlist **nl = realloc(c->lists, n * sizeof(*nl));
    if (!nl)
        return -1;
    memset(nl + c->nlists, 0, (n - c->nlists) * sizeof(*nl));
    c->lists = nl;
    c->nlists = n;
    return 0;
}

GLuint glGenLists(GLsizei range)
{
    struct fgl_context *c = gl_ctx;
    if (!c || range <= 0 || c->compiling)
        return 0;
    /* range unused names in a row, after the highest one in use */
    GLuint first = 1;
    for (GLuint i = 1; i < c->nlists; i++)
        if (c->lists[i])
            first = i + 1;
    if (ensure_slot(c, first + (GLuint)range - 1) < 0) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return 0;
    }
    /* Reserve the names with empty lists so the next glGenLists skips them. */
    for (GLuint i = first; i < first + (GLuint)range; i++) {
        c->lists[i] = calloc(1, sizeof(struct dlist));
        if (!c->lists[i]) {
            gl_error(c, GL_OUT_OF_MEMORY);
            return 0;
        }
    }
    return first;
}

void glNewList(GLuint list, GLenum mode)
{
    GET_CTX(c);
    if (list == 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (c->compiling || c->in_begin) {
        gl_error(c, GL_INVALID_OPERATION);
        return;
    }
    c->compiling = calloc(1, sizeof(struct dlist));
    if (!c->compiling) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    c->compiling_id = list;
    c->list_mode = mode;
}

void glEndList(void)
{
    GET_CTX(c);
    if (!c->compiling) {
        gl_error(c, GL_INVALID_OPERATION);
        return;
    }
    struct dlist *l = c->compiling;
    c->compiling = NULL;
    if (ensure_slot(c, c->compiling_id) < 0) {
        list_free(l);
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    list_free(c->lists[c->compiling_id]);
    c->lists[c->compiling_id] = l;
}

void glDeleteLists(GLuint list, GLsizei range)
{
    GET_CTX(c);
    if (range < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    for (GLuint i = list; i < list + (GLuint)range && i < c->nlists; i++) {
        list_free(c->lists[i]);
        c->lists[i] = NULL;
    }
}

GLboolean glIsList(GLuint list)
{
    struct fgl_context *c = gl_ctx;
    return c && list < c->nlists && c->lists[list] ? GL_TRUE : GL_FALSE;
}

static void call_list(struct fgl_context *c, GLuint list);

/* OP_BITMAP: width, height, origin and move, then the bits sixteen to a float. */
static void replay_bitmap(struct fgl_context *c, const float *a)
{
    int w = (int)a[0], h = (int)a[1];
    size_t bits = (size_t)w * (size_t)h;
    uint8_t *on = bits ? malloc(bits) : NULL;
    if (bits && !on) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    for (size_t i = 0; i < bits; i++)
        on[i] = ((unsigned)a[6 + i / 16] >> (i % 16)) & 1;
    if (c->in_begin)
        gl_error(c, GL_INVALID_OPERATION);
    else
        gl_draw_bitmap(c, w, h, a[2], a[3], a[4], a[5], on);
    free(on);
}

static void replay(struct fgl_context *c, const struct dlist *l)
{
    const float *p = l->ops, *end = l->ops + l->n;
    while (p < end) {
        int op = (int)p[0], n = (int)p[1];
        const float *a = p + 2;
        p += 2 + n;
        switch (op) {
        case OP_BEGIN: glBegin((GLenum)a[0]); break;
        case OP_END: glEnd(); break;
        case OP_VERTEX: glVertex4f(a[0], a[1], a[2], a[3]); break;
        case OP_NORMAL: glNormal3f(a[0], a[1], a[2]); break;
        case OP_COLOR: glColor4f(a[0], a[1], a[2], a[3]); break;
        case OP_MATERIAL: glMaterialfv((GLenum)a[0], (GLenum)a[1], a + 2); break;
        case OP_LIGHT: glLightfv((GLenum)a[0], (GLenum)a[1], a + 2); break;
        case OP_LIGHT_MODEL: glLightModelfv((GLenum)a[0], a + 1); break;
        case OP_COLOR_MATERIAL: glColorMaterial((GLenum)a[0], (GLenum)a[1]); break;
        case OP_SHADE_MODEL: glShadeModel((GLenum)a[0]); break;
        case OP_ENABLE: glEnable((GLenum)a[0]); break;
        case OP_DISABLE: glDisable((GLenum)a[0]); break;
        case OP_MATRIX_MODE: glMatrixMode((GLenum)a[0]); break;
        case OP_LOAD_IDENTITY: glLoadIdentity(); break;
        case OP_LOAD_MATRIX: glLoadMatrixf(a); break;
        case OP_MULT_MATRIX: glMultMatrixf(a); break;
        case OP_PUSH_MATRIX: glPushMatrix(); break;
        case OP_POP_MATRIX: glPopMatrix(); break;
        case OP_TRANSLATE: glTranslatef(a[0], a[1], a[2]); break;
        case OP_ROTATE: glRotatef(a[0], a[1], a[2], a[3]); break;
        case OP_SCALE: glScalef(a[0], a[1], a[2]); break;
        case OP_FRUSTUM: glFrustum(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case OP_ORTHO: glOrtho(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case OP_CALL_LIST: glCallList((GLuint)a[0]); break;
        case OP_CULL_FACE: glCullFace((GLenum)a[0]); break;
        case OP_FRONT_FACE: glFrontFace((GLenum)a[0]); break;
        case OP_DEPTH_FUNC: glDepthFunc((GLenum)a[0]); break;
        case OP_DEPTH_MASK: glDepthMask((GLboolean)a[0]); break;
        case OP_DEPTH_RANGE: glDepthRange(a[0], a[1]); break;
        case OP_CLEAR: glClear((GLbitfield)a[0]); break;
        case OP_CLEAR_COLOR: glClearColor(a[0], a[1], a[2], a[3]); break;
        case OP_CLEAR_DEPTH: glClearDepth(a[0]); break;
        case OP_VIEWPORT: glViewport((GLint)a[0], (GLint)a[1], (GLsizei)a[2], (GLsizei)a[3]); break;
        case OP_SCISSOR: glScissor((GLint)a[0], (GLint)a[1], (GLsizei)a[2], (GLsizei)a[3]); break;
        case OP_POINT_SIZE: glPointSize(a[0]); break;
        case OP_LINE_WIDTH: glLineWidth(a[0]); break;
        case OP_TEXCOORD: glTexCoord4f(a[0], a[1], a[2], a[3]); break;
        case OP_BIND_TEXTURE: glBindTexture((GLenum)a[0], (GLuint)a[1]); break;
        case OP_TEX_ENV: glTexEnvfv((GLenum)a[0], (GLenum)a[1], a + 2); break;
        case OP_TEX_PARAMETER: glTexParameterfv((GLenum)a[0], (GLenum)a[1], a + 2); break;
        case OP_BLEND_FUNC: glBlendFunc((GLenum)a[0], (GLenum)a[1]); break;
        case OP_ALPHA_FUNC: glAlphaFunc((GLenum)a[0], a[1]); break;
        case OP_COLOR_MASK:
            glColorMask((GLboolean)a[0], (GLboolean)a[1], (GLboolean)a[2], (GLboolean)a[3]);
            break;
        case OP_POLYGON_MODE: glPolygonMode((GLenum)a[0], (GLenum)a[1]); break;
        case OP_POLYGON_OFFSET: glPolygonOffset(a[0], a[1]); break;
        case OP_LIST_BASE: glListBase((GLuint)a[0]); break;
        case OP_CALL_LISTS:
            for (int i = 0; i < n; i++)
                call_list(c, c->list_base + (GLuint)(GLint)a[i]);
            break;
        case OP_FOG: glFogfv((GLenum)a[0], a + 1); break;
        case OP_STENCIL_FUNC: glStencilFunc((GLenum)a[0], (GLint)a[1], (GLuint)a[2]); break;
        case OP_STENCIL_OP: glStencilOp((GLenum)a[0], (GLenum)a[1], (GLenum)a[2]); break;
        case OP_STENCIL_MASK: glStencilMask((GLuint)a[0]); break;
        case OP_CLEAR_STENCIL: glClearStencil((GLint)a[0]); break;
        case OP_CLIP_PLANE: {
            GLdouble eq[4] = { a[1], a[2], a[3], a[4] };
            glClipPlane((GLenum)a[0], eq);
            break;
        }
        case OP_TEX_GEN: glTexGenfv((GLenum)a[0], (GLenum)a[1], a + 2); break;
        case OP_RASTER_POS: glRasterPos4f(a[0], a[1], a[2], a[3]); break;
        case OP_BITMAP: replay_bitmap(c, a); break;
        case OP_PIXEL_ZOOM: glPixelZoom(a[0], a[1]); break;
        case OP_ACTIVE_TEXTURE: glActiveTextureARB((GLenum)a[0]); break;
        case OP_MULTI_TEXCOORD: glMultiTexCoord4fARB((GLenum)a[0], a[1], a[2], a[3], a[4]); break;
        case OP_PUSH_ATTRIB: glPushAttrib((GLbitfield)a[0]); break;
        case OP_POP_ATTRIB: glPopAttrib(); break;
        case OP_HINT: glHint((GLenum)a[0], (GLenum)a[1]); break;
        }
    }
}

static void call_list(struct fgl_context *c, GLuint list)
{
    if (list >= c->nlists || !c->lists[list] || c->executing >= MAX_CALL_DEPTH)
        return;
    c->executing++;
    replay(c, c->lists[list]);
    c->executing--;
}

void glCallList(GLuint list)
{
    GET_CTX(c);
    RECORD(c, OP_CALL_LIST, 1, list);
    call_list(c, list);
}

void glListBase(GLuint base)
{
    GET_CTX(c);
    RECORD(c, OP_LIST_BASE, 1, base);
    c->list_base = base;
}

/* The i-th list name of an array of type type (one glCallLists takes). */
static GLint list_name(GLenum type, const void *lists, GLsizei i)
{
    const uint8_t *b = lists;
    switch (type) {
    case GL_BYTE: return ((const int8_t *)lists)[i];
    case GL_UNSIGNED_BYTE: return b[i];
    case GL_SHORT: return ((const int16_t *)lists)[i];
    case GL_UNSIGNED_SHORT: return ((const uint16_t *)lists)[i];
    case GL_INT: return ((const int32_t *)lists)[i];
    case GL_UNSIGNED_INT: return (GLint)((const uint32_t *)lists)[i];
    case GL_FLOAT: return (GLint)((const float *)lists)[i];
    /* names in bytes, the most significant first */
    case GL_2_BYTES: return b[2 * i] << 8 | b[2 * i + 1];
    case GL_3_BYTES: return b[3 * i] << 16 | b[3 * i + 1] << 8 | b[3 * i + 2];
    case GL_4_BYTES: return (GLint)((uint32_t)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3]);
    }
    return 0;
}

void glCallLists(GLsizei n, GLenum type, const GLvoid *lists)
{
    GET_CTX(c);
    if (type < GL_BYTE || type > GL_4_BYTES) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (n < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    if (c->compiling && !c->executing) {
        /* The names go in as floats, which hold them exactly up to 2^24. */
        float *names = malloc(((size_t)n + 1) * sizeof(float));
        if (!names) {
            gl_error(c, GL_OUT_OF_MEMORY);
            return;
        }
        for (GLsizei i = 0; i < n; i++)
            names[i] = (float)list_name(type, lists, i);
        int skip = gl_record(c, OP_CALL_LISTS, n, names);
        free(names);
        if (skip)
            return;
    }
    for (GLsizei i = 0; i < n; i++)
        call_list(c, c->list_base + (GLuint)list_name(type, lists, i));
}
