/*
 * Vertex arrays. glArrayElement reads vertex i of each enabled array and
 * hands it to glNormal, glColor, glTexCoord and glVertex, in that order;
 * glDrawArrays and glDrawElements are glBegin, glArrayElement for each
 * vertex, and glEnd. So vertices from arrays take the same path as
 * immediate ones, and a display list being compiled records them.
 *
 * Normals, and colors of integer types, are mapped to -1 to 1 or 0 to 1
 * as GL has it; vertex and texture coordinates are taken as they are.
 * Each texture unit has its texture coordinate array, which
 * glClientActiveTexture picks. glLockArraysEXT and glUnlockArraysEXT
 * (GL_EXT_compiled_vertex_array) are kept track of and change nothing:
 * every vertex is transformed when it is drawn.
 */
#include <stddef.h>
#include "glint.h"

#define GET_CTX(c)                      \
    struct fgl_context *c = gl_ctx;     \
    if (!c)                             \
        return

static int type_size(GLenum type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
    case GL_DOUBLE: return 8;
    default: return 0;
    }
}

struct varray *gl_client_array(struct fgl_context *c, GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY: return &c->arrays[ARRAY_VERTEX];
    case GL_NORMAL_ARRAY: return &c->arrays[ARRAY_NORMAL];
    case GL_COLOR_ARRAY: return &c->arrays[ARRAY_COLOR];
    case GL_TEXTURE_COORD_ARRAY: return &c->arrays[ARRAY_TEXCOORD + c->client_unit];
    default: return NULL;
    }
}

static void set_client_state(GLenum cap, int on)
{
    GET_CTX(c);
    struct varray *a = gl_client_array(c, cap);
    if (a)
        a->enabled = on;
    else if (cap != GL_INDEX_ARRAY && cap != GL_EDGE_FLAG_ARRAY)     /* accepted and never read */
        gl_error(c, GL_INVALID_ENUM);
}

void glEnableClientState(GLenum cap) { set_client_state(cap, 1); }
void glDisableClientState(GLenum cap) { set_client_state(cap, 0); }

/* Point array k at ptr, if size and type are among those allowed (types
 * is a list ending in 0). */
static void set_pointer(int k, GLint size, int min_size, int max_size, GLenum type, const GLenum *types,
                        GLsizei stride, const GLvoid *ptr)
{
    GET_CTX(c);
    int ok = 0;
    for (; *types; types++)
        ok |= *types == type;
    if (!ok) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (size < min_size || size > max_size || stride < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    struct varray *a = &c->arrays[k];
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->ptr = ptr;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    static const GLenum types[] = { GL_SHORT, GL_INT, GL_FLOAT, GL_DOUBLE, 0 };
    set_pointer(ARRAY_VERTEX, size, 2, 4, type, types, stride, ptr);
}

void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr)
{
    static const GLenum types[] = { GL_BYTE, GL_SHORT, GL_INT, GL_FLOAT, GL_DOUBLE, 0 };
    set_pointer(ARRAY_NORMAL, 3, 3, 3, type, types, stride, ptr);
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    static const GLenum types[] = {
        GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_UNSIGNED_SHORT, GL_INT, GL_UNSIGNED_INT, GL_FLOAT, GL_DOUBLE, 0,
    };
    set_pointer(ARRAY_COLOR, size, 3, 4, type, types, stride, ptr);
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    static const GLenum types[] = { GL_SHORT, GL_INT, GL_FLOAT, GL_DOUBLE, 0 };
    struct fgl_context *c = gl_ctx;
    if (c)
        set_pointer(ARRAY_TEXCOORD + c->client_unit, size, 1, 4, type, types, stride, ptr);
}

/* glInterleavedArrays' formats: which arrays, the texture coordinates' and
 * color's sizes, the vertex's, and the color's type. */
static const struct {
    GLenum format;
    int tex, color, normal, vertex;
    GLenum color_type;
} interleaved[] = {
    { GL_V2F, 0, 0, 0, 2, 0 },
    { GL_V3F, 0, 0, 0, 3, 0 },
    { GL_C4UB_V2F, 0, 4, 0, 2, GL_UNSIGNED_BYTE },
    { GL_C4UB_V3F, 0, 4, 0, 3, GL_UNSIGNED_BYTE },
    { GL_C3F_V3F, 0, 3, 0, 3, GL_FLOAT },
    { GL_N3F_V3F, 0, 0, 1, 3, 0 },
    { GL_C4F_N3F_V3F, 0, 4, 1, 3, GL_FLOAT },
    { GL_T2F_V3F, 2, 0, 0, 3, 0 },
    { GL_T4F_V4F, 4, 0, 0, 4, 0 },
    { GL_T2F_C4UB_V3F, 2, 4, 0, 3, GL_UNSIGNED_BYTE },
    { GL_T2F_C3F_V3F, 2, 3, 0, 3, GL_FLOAT },
    { GL_T2F_N3F_V3F, 2, 0, 1, 3, 0 },
    { GL_T2F_C4F_N3F_V3F, 2, 4, 1, 3, GL_FLOAT },
    { GL_T4F_C4F_N3F_V4F, 4, 4, 1, 4, GL_FLOAT },
};

void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer)
{
    GET_CTX(c);
    int k = 0, n = (int)(sizeof(interleaved) / sizeof(interleaved[0]));
    while (k < n && interleaved[k].format != format)
        k++;
    if (k == n) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (stride < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    /* Each vertex is texture coordinates, color, normal, position, packed
     * (a color of bytes takes one float's room). */
    int tex = interleaved[k].tex, color = interleaved[k].color, normal = interleaved[k].normal;
    int vertex = interleaved[k].vertex;
    GLenum ctype = interleaved[k].color_type;
    int csize = ctype == GL_UNSIGNED_BYTE ? 4 : color * 4;
    int toff = 0, coff = tex * 4, noff = coff + csize, voff = noff + normal * 12;
    if (stride == 0)
        stride = voff + vertex * 4;
    const uint8_t *p = pointer;
    c->arrays[ARRAY_TEXCOORD + c->client_unit].enabled = tex != 0;
    c->arrays[ARRAY_COLOR].enabled = color != 0;
    c->arrays[ARRAY_NORMAL].enabled = normal;
    c->arrays[ARRAY_VERTEX].enabled = 1;
    if (tex)
        glTexCoordPointer(tex, GL_FLOAT, stride, p + toff);
    if (color)
        glColorPointer(color, ctype, stride, p + coff);
    if (normal)
        glNormalPointer(GL_FLOAT, stride, p + noff);
    glVertexPointer(vertex, GL_FLOAT, stride, p + voff);
}

void glGetPointerv(GLenum pname, GLvoid **params)
{
    GET_CTX(c);
    int k;
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: k = ARRAY_VERTEX; break;
    case GL_NORMAL_ARRAY_POINTER: k = ARRAY_NORMAL; break;
    case GL_COLOR_ARRAY_POINTER: k = ARRAY_COLOR; break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: k = ARRAY_TEXCOORD + c->client_unit; break;
    default:
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    *params = (GLvoid *)c->arrays[k].ptr;
}

int gl_array_get(struct fgl_context *c, GLenum pname, double *v)
{
    const struct varray *a;
    switch (pname) {
    case GL_VERTEX_ARRAY: case GL_NORMAL_ARRAY: case GL_COLOR_ARRAY: case GL_TEXTURE_COORD_ARRAY:
        v[0] = gl_client_array(c, pname)->enabled;
        return 1;
    case GL_VERTEX_ARRAY_SIZE: v[0] = c->arrays[ARRAY_VERTEX].size; return 1;
    case GL_COLOR_ARRAY_SIZE: v[0] = c->arrays[ARRAY_COLOR].size; return 1;
    case GL_TEXTURE_COORD_ARRAY_SIZE: v[0] = c->arrays[ARRAY_TEXCOORD + c->client_unit].size; return 1;
    case GL_VERTEX_ARRAY_TYPE: case GL_VERTEX_ARRAY_STRIDE: a = &c->arrays[ARRAY_VERTEX]; break;
    case GL_NORMAL_ARRAY_TYPE: case GL_NORMAL_ARRAY_STRIDE: a = &c->arrays[ARRAY_NORMAL]; break;
    case GL_COLOR_ARRAY_TYPE: case GL_COLOR_ARRAY_STRIDE: a = &c->arrays[ARRAY_COLOR]; break;
    case GL_TEXTURE_COORD_ARRAY_TYPE: case GL_TEXTURE_COORD_ARRAY_STRIDE:
        a = &c->arrays[ARRAY_TEXCOORD + c->client_unit];
        break;
    default: return 0;
    }
    int is_type = pname == GL_VERTEX_ARRAY_TYPE || pname == GL_NORMAL_ARRAY_TYPE ||
                  pname == GL_COLOR_ARRAY_TYPE || pname == GL_TEXTURE_COORD_ARRAY_TYPE;
    v[0] = is_type ? (double)a->type : (double)a->stride;
    return 1;
}

/* ---- reading vertices ---- */

/* Element i of array a, its components as floats in out (a->size of them).
 * norm: map integers to -1..1 (signed) or 0..1 (unsigned). */
static void fetch(const struct varray *a, GLint i, float *out, int norm)
{
    int stride = a->stride ? a->stride : a->size * type_size(a->type);
    const uint8_t *p = (const uint8_t *)a->ptr + (ptrdiff_t)i * stride;
    for (int k = 0; k < a->size; k++) {
        float f;
        switch (a->type) {
        case GL_BYTE: {
            int v = ((const int8_t *)p)[k];
            f = norm ? (2 * v + 1) / 255.0f : (float)v;
            break;
        }
        case GL_UNSIGNED_BYTE: {
            int v = p[k];
            f = norm ? v / 255.0f : (float)v;
            break;
        }
        case GL_SHORT: {
            int v = ((const int16_t *)p)[k];
            f = norm ? (2 * v + 1) / 65535.0f : (float)v;
            break;
        }
        case GL_UNSIGNED_SHORT: {
            int v = ((const uint16_t *)p)[k];
            f = norm ? v / 65535.0f : (float)v;
            break;
        }
        case GL_INT: {
            int32_t v = ((const int32_t *)p)[k];
            f = norm ? (float)((2.0 * v + 1) / 4294967295.0) : (float)v;
            break;
        }
        case GL_UNSIGNED_INT: {
            uint32_t v = ((const uint32_t *)p)[k];
            f = norm ? (float)(v / 4294967295.0) : (float)v;
            break;
        }
        case GL_DOUBLE: f = (float)((const double *)p)[k]; break;
        default: f = ((const float *)p)[k]; break;
        }
        out[k] = f;
    }
}

void glArrayElement(GLint i)
{
    GET_CTX(c);
    float v[4];
    const struct varray *a = &c->arrays[ARRAY_NORMAL];
    if (a->enabled && a->ptr) {
        fetch(a, i, v, 1);
        glNormal3f(v[0], v[1], v[2]);
    }
    a = &c->arrays[ARRAY_COLOR];
    if (a->enabled && a->ptr) {
        v[3] = 1;
        fetch(a, i, v, 1);
        glColor4f(v[0], v[1], v[2], v[3]);
    }
    for (int u = 0; u < MAX_UNITS; u++) {
        a = &c->arrays[ARRAY_TEXCOORD + u];
        if (a->enabled && a->ptr) {
            v[1] = v[2] = 0;
            v[3] = 1;
            fetch(a, i, v, 0);
            if (u == 0)
                glTexCoord4f(v[0], v[1], v[2], v[3]);
            else
                glMultiTexCoord4fARB(GL_TEXTURE0_ARB + (GLenum)u, v[0], v[1], v[2], v[3]);
        }
    }
    a = &c->arrays[ARRAY_VERTEX];
    if (a->enabled && a->ptr) {
        v[2] = 0;
        v[3] = 1;
        fetch(a, i, v, 0);
        glVertex4f(v[0], v[1], v[2], v[3]);
    }
}

/* What glDrawArrays and glDrawElements share; returns 0 if they may draw. */
static int check_draw(struct fgl_context *c, GLenum mode, GLsizei count)
{
    if (mode > GL_POLYGON) {
        gl_error(c, GL_INVALID_ENUM);
        return -1;
    }
    if (count < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return -1;
    }
    if (c->in_begin) {
        gl_error(c, GL_INVALID_OPERATION);
        return -1;
    }
    return 0;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    GET_CTX(c);
    if (check_draw(c, mode, count) < 0)
        return;
    if (first < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    glBegin(mode);
    for (GLsizei i = 0; i < count; i++)
        glArrayElement(first + i);
    glEnd();
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    GET_CTX(c);
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (check_draw(c, mode, count) < 0)
        return;
    glBegin(mode);
    for (GLsizei i = 0; i < count; i++) {
        GLint k = type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i]
                  : type == GL_UNSIGNED_SHORT ? ((const uint16_t *)indices)[i]
                                              : (GLint)((const uint32_t *)indices)[i];
        glArrayElement(k);
    }
    glEnd();
}

void glLockArraysEXT(GLint first, GLsizei count)
{
    GET_CTX(c);
    if (first < 0 || count <= 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    if (c->lock_count || c->in_begin) {
        gl_error(c, GL_INVALID_OPERATION);
        return;
    }
    c->lock_first = first;
    c->lock_count = count;
}

void glUnlockArraysEXT(void)
{
    GET_CTX(c);
    if (!c->lock_count || c->in_begin) {
        gl_error(c, GL_INVALID_OPERATION);
        return;
    }
    c->lock_first = 0;
    c->lock_count = 0;
}
