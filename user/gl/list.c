/*
 * Display lists. While a list is being compiled, each GL call that can go
 * in one records an opcode, its argument count and its arguments (all as
 * floats; enums and list and texture names fit exactly, up to 2^24).
 * glTexImage2D and glTexSubImage2D are not recorded: they take effect
 * when called, even under GL_COMPILE. glCallList replays them
 * through the same entry points, with recording held off so that a list
 * called during GL_COMPILE_AND_EXECUTE is not copied into the new one.
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

static void replay(const struct dlist *l)
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
        }
    }
}

void glCallList(GLuint list)
{
    GET_CTX(c);
    RECORD(c, OP_CALL_LIST, 1, list);
    if (list >= c->nlists || !c->lists[list] || c->executing >= MAX_CALL_DEPTH)
        return;
    c->executing++;
    replay(c->lists[list]);
    c->executing--;
}
