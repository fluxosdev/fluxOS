/*
 * Textures: texture objects, uploading texels from the formats GL 1.1
 * has (unsigned bytes, with the glPixelStorei unpack rules), texture
 * parameters and the texture environment.
 *
 * Every texture is kept as 0xAABBGGRR texels whatever it came as, so the
 * backends only ever see one format. Only level 0 is kept: the mipmap
 * filters fall back to it, and other levels are accepted and dropped.
 */
#include <stdlib.h>
#include <string.h>
#include "glint.h"

#define GET_CTX(c)                      \
    struct fgl_context *c = gl_ctx;     \
    if (!c)                             \
        return
#define OUTSIDE_BEGIN(c)                            \
    do {                                            \
        if ((c)->in_begin) {                        \
            gl_error((c), GL_INVALID_OPERATION);    \
            return;                                 \
        }                                           \
    } while (0)

static struct gltex *tex_new(GLuint name)
{
    struct gltex *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->name = name;
    t->format = GL_RGBA;
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
    t->hw_stale = 1;
    return t;
}

static void tex_free(struct fgl_context *c, struct gltex *t)
{
    if (!t)
        return;
    if (c->be && c->be->tex_free)
        c->be->tex_free(c, t);
    free(t->texels);
    free(t);
}

/* Make sure slot name exists. */
static int ensure_slot(struct fgl_context *c, GLuint name)
{
    if (name < c->ntextures)
        return 0;
    GLuint n = c->ntextures ? c->ntextures : 16;
    while (n <= name)
        n *= 2;
    struct gltex **nt = realloc(c->textures, n * sizeof(*nt));
    if (!nt)
        return -1;
    memset(nt + c->ntextures, 0, (n - c->ntextures) * sizeof(*nt));
    c->textures = nt;
    c->ntextures = n;
    return 0;
}

void gl_tex_init(struct fgl_context *c)
{
    c->tex_env_mode = GL_MODULATE;
    c->unpack_align = 4;
    c->texcoord[3] = 1;
    if (ensure_slot(c, 0) == 0)
        c->textures[0] = tex_new(0);    /* the default texture, which name 0 binds */
}

void gl_tex_free_all(struct fgl_context *c)
{
    for (GLuint i = 0; i < c->ntextures; i++)
        tex_free(c, c->textures[i]);
    free(c->textures);
    c->textures = NULL;
    c->ntextures = 0;
}

static struct gltex *bound_texture(struct fgl_context *c)
{
    return c->bound < c->ntextures ? c->textures[c->bound] : NULL;
}

struct gltex *gl_active_texture(struct fgl_context *c)
{
    if (!c->texture_2d)
        return NULL;
    struct gltex *t = bound_texture(c);
    return t && t->texels && t->w > 0 && t->h > 0 ? t : NULL;
}

/* The Voodoo backend draws without textures: a context that needs them
 * goes on in software, keeping what it has drawn. */
void gl_tex_check_backend(struct fgl_context *c)
{
    if (c->be == &gl_tdfx_backend && gl_active_texture(c))
        gl_use_software(c, 1);
}

/* ---- names and binding ---- */

void glGenTextures(GLsizei n, GLuint *textures)
{
    GET_CTX(c);
    if (n < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    GLuint name = 1;
    for (GLsizei i = 0; i < n; i++) {
        while (name < c->ntextures && c->textures[name])
            name++;
        if (ensure_slot(c, name) < 0 || !(c->textures[name] = tex_new(name))) {
            gl_error(c, GL_OUT_OF_MEMORY);
            return;
        }
        textures[i] = name++;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    if (n < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        GLuint name = textures[i];
        if (name == 0 || name >= c->ntextures || !c->textures[name])
            continue;
        tex_free(c, c->textures[name]);
        c->textures[name] = NULL;
        if (c->bound == name)
            c->bound = 0;
        c->hw_dirty = 1;
    }
}

void glBindTexture(GLenum target, GLuint texture)
{
    GET_CTX(c);
    RECORD(c, OP_BIND_TEXTURE, 2, target, texture);
    OUTSIDE_BEGIN(c);
    if (target != GL_TEXTURE_2D) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (ensure_slot(c, texture) < 0 ||
        (!c->textures[texture] && !(c->textures[texture] = tex_new(texture)))) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    if (c->bound != texture)
        c->hw_dirty = 1;
    c->bound = texture;
}

GLboolean glIsTexture(GLuint texture)
{
    struct fgl_context *c = gl_ctx;
    return c && texture && texture < c->ntextures && c->textures[texture] ? GL_TRUE : GL_FALSE;
}

/* ---- texels ---- */

/* Components a pixel of this format has in client memory; 0 if it is not one. */
static int format_size(GLenum format)
{
    switch (format) {
    case GL_ALPHA: case GL_LUMINANCE: return 1;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_RGB: case GL_BGR: return 3;
    case GL_RGBA: case GL_BGRA: return 4;
    default: return 0;
    }
}

/* The base format an internal format stands for; 0 if it is not one. */
static GLenum base_format(GLint internal)
{
    switch (internal) {
    case 1: case GL_LUMINANCE: return GL_LUMINANCE;
    case 2: case GL_LUMINANCE_ALPHA: return GL_LUMINANCE_ALPHA;
    case 3: case GL_RGB: case GL_RGB8: return GL_RGB;
    case 4: case GL_RGBA: case GL_RGBA8: return GL_RGBA;
    case GL_ALPHA: return GL_ALPHA;
    case GL_INTENSITY: return GL_INTENSITY;
    default: return 0;
    }
}

/* One pixel from client memory as R, G, B, A, then as the texture's base
 * format keeps it. An alpha texture's color is white, so that it modulates
 * the fragment's color as GL has it pass through. */
static uint32_t convert(const uint8_t *p, GLenum format, GLenum base)
{
    uint32_t r, g, b, a = 255;
    switch (format) {
    case GL_ALPHA: r = g = b = 255; a = p[0]; break;
    case GL_LUMINANCE: r = g = b = p[0]; break;
    case GL_LUMINANCE_ALPHA: r = g = b = p[0]; a = p[1]; break;
    case GL_RGB: r = p[0]; g = p[1]; b = p[2]; break;
    case GL_BGR: r = p[2]; g = p[1]; b = p[0]; break;
    case GL_BGRA: r = p[2]; g = p[1]; b = p[0]; a = p[3]; break;
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
    }
    switch (base) {
    case GL_RGB: a = 255; break;
    case GL_LUMINANCE: g = b = r; a = 255; break;
    case GL_LUMINANCE_ALPHA: g = b = r; break;
    case GL_INTENSITY: g = b = a = r; break;
    case GL_ALPHA: r = g = b = 255; break;
    }
    return a << 24 | b << 16 | g << 8 | r;
}

/* Copy a w x h block from client memory into the texture at (x, y). */
static void unpack(struct fgl_context *c, struct gltex *t, int x, int y, int w, int h, GLenum format,
                   const void *pixels)
{
    int comps = format_size(format);
    int row_pixels = c->unpack_row_length > 0 ? c->unpack_row_length : w;
    size_t row = ((size_t)row_pixels * (size_t)comps + (size_t)c->unpack_align - 1) /
                 (size_t)c->unpack_align * (size_t)c->unpack_align;
    const uint8_t *src = (const uint8_t *)pixels + (size_t)c->unpack_skip_rows * row +
                         (size_t)c->unpack_skip_pixels * (size_t)comps;
    for (int j = 0; j < h; j++) {
        const uint8_t *p = src + (size_t)j * row;
        uint32_t *dst = t->texels + (size_t)(y + j) * (size_t)t->w + x;
        for (int i = 0; i < w; i++, p += comps)
            dst[i] = convert(p, format, t->format);
    }
}

void glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    GLenum base = base_format(internal_format);
    if (target != GL_TEXTURE_2D || !format_size(format) || type != GL_UNSIGNED_BYTE) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (level < 0 || !base || border != 0 || width < 0 || height < 0 ||
        width > MAX_TEXTURE || height > MAX_TEXTURE) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    if (level > 0)
        return;                         /* mipmaps: level 0 serves for them all */
    struct gltex *t = bound_texture(c);
    if (!t)
        return;
    uint32_t *texels = NULL;
    if (width && height && !(texels = calloc((size_t)width * (size_t)height, 4))) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    free(t->texels);
    t->texels = texels;
    t->w = width;
    t->h = height;
    t->format = base;
    if (texels && pixels)
        unpack(c, t, 0, 0, width, height, format, pixels);
    t->hw_stale = 1;
    c->hw_dirty = 1;
}

void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const GLvoid *pixels)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    if (target != GL_TEXTURE_2D || !format_size(format) || type != GL_UNSIGNED_BYTE) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (level > 0)
        return;
    struct gltex *t = bound_texture(c);
    if (!t || level < 0 || x < 0 || y < 0 || width < 0 || height < 0 ||
        x + width > t->w || y + height > t->h) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    if (!width || !height || !pixels)
        return;
    unpack(c, t, x, y, width, height, format, pixels);
    t->hw_stale = 1;
    c->hw_dirty = 1;
}

void glPixelStorei(GLenum pname, GLint param)
{
    GET_CTX(c);
    switch (pname) {
    case GL_UNPACK_ALIGNMENT:
    case GL_PACK_ALIGNMENT:
        if (param != 1 && param != 2 && param != 4 && param != 8) {
            gl_error(c, GL_INVALID_VALUE);
            return;
        }
        if (pname == GL_UNPACK_ALIGNMENT)
            c->unpack_align = param;
        return;                         /* nothing is read back with glReadPixels */
    case GL_UNPACK_ROW_LENGTH:
    case GL_UNPACK_SKIP_ROWS:
    case GL_UNPACK_SKIP_PIXELS:
        if (param < 0) {
            gl_error(c, GL_INVALID_VALUE);
            return;
        }
        if (pname == GL_UNPACK_ROW_LENGTH)
            c->unpack_row_length = param;
        else if (pname == GL_UNPACK_SKIP_ROWS)
            c->unpack_skip_rows = param;
        else
            c->unpack_skip_pixels = param;
        return;
    }
    gl_error(c, GL_INVALID_ENUM);
}

/* ---- parameters and the environment ---- */

void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
    GET_CTX(c);
    float v[4] = { params[0], 0, 0, 0 };
    if (pname == GL_TEXTURE_BORDER_COLOR)
        memcpy(v, params, sizeof(v));
    RECORD(c, OP_TEX_PARAMETER, 6, target, pname, v[0], v[1], v[2], v[3]);
    OUTSIDE_BEGIN(c);
    struct gltex *t = bound_texture(c);
    if (target != GL_TEXTURE_2D || !t) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    GLenum e = (GLenum)v[0];
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR && e != GL_NEAREST_MIPMAP_NEAREST &&
            e != GL_LINEAR_MIPMAP_NEAREST && e != GL_NEAREST_MIPMAP_LINEAR && e != GL_LINEAR_MIPMAP_LINEAR)
            break;
        t->min_filter = e;
        c->hw_dirty = 1;
        return;
    case GL_TEXTURE_MAG_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR)
            break;
        t->mag_filter = e;
        c->hw_dirty = 1;
        return;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        if (e != GL_REPEAT && e != GL_CLAMP && e != GL_CLAMP_TO_EDGE)
            break;
        if (pname == GL_TEXTURE_WRAP_S)
            t->wrap_s = e;
        else
            t->wrap_t = e;
        c->hw_dirty = 1;
        return;
    case GL_TEXTURE_BORDER_COLOR:
        for (int i = 0; i < 4; i++)
            t->border[i] = gl_clampf(v[i]);
        return;
    }
    gl_error(c, GL_INVALID_ENUM);
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    glTexParameterfv(target, pname, &param);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    GLfloat f = (GLfloat)param;
    glTexParameterfv(target, pname, &f);
}

void glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
    GLfloat v[4] = { (GLfloat)params[0], 0, 0, 0 };
    if (pname == GL_TEXTURE_BORDER_COLOR)       /* colors as integers span the int range */
        for (int i = 0; i < 4; i++)
            v[i] = (GLfloat)((double)params[i] / 2147483647.0);
    glTexParameterfv(target, pname, v);
}

void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
    GET_CTX(c);
    struct gltex *t = bound_texture(c);
    if (target != GL_TEXTURE_2D || !t) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    int have = level == 0;
    switch (pname) {
    case GL_TEXTURE_WIDTH: *params = have ? t->w : 0; return;
    case GL_TEXTURE_HEIGHT: *params = have ? t->h : 0; return;
    case GL_TEXTURE_INTERNAL_FORMAT: *params = (GLint)t->format; return;
    }
    gl_error(c, GL_INVALID_ENUM);
}

void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params)
{
    GET_CTX(c);
    float v[4] = { params[0], 0, 0, 0 };
    if (pname == GL_TEXTURE_ENV_COLOR)
        memcpy(v, params, sizeof(v));
    RECORD(c, OP_TEX_ENV, 6, target, pname, v[0], v[1], v[2], v[3]);
    OUTSIDE_BEGIN(c);
    if (target != GL_TEXTURE_ENV) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (pname == GL_TEXTURE_ENV_MODE) {
        GLenum m = (GLenum)v[0];
        if (m != GL_MODULATE && m != GL_DECAL && m != GL_REPLACE && m != GL_BLEND) {
            gl_error(c, GL_INVALID_ENUM);
            return;
        }
        c->tex_env_mode = m;
        c->hw_dirty = 1;
    } else if (pname == GL_TEXTURE_ENV_COLOR) {
        for (int i = 0; i < 4; i++)
            c->tex_env_color[i] = gl_clampf(v[i]);
    } else {
        gl_error(c, GL_INVALID_ENUM);
    }
}

void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    glTexEnvfv(target, pname, &param);
}

void glTexEnvi(GLenum target, GLenum pname, GLint param)
{
    GLfloat f = (GLfloat)param;
    glTexEnvfv(target, pname, &f);
}

/* ---- texture coordinates ---- */

void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q)
{
    GET_CTX(c);
    RECORD(c, OP_TEXCOORD, 4, s, t, r, q);
    c->texcoord[0] = s;
    c->texcoord[1] = t;
    c->texcoord[2] = r;
    c->texcoord[3] = q;
}

void glTexCoord1f(GLfloat s) { glTexCoord4f(s, 0, 0, 1); }
void glTexCoord2f(GLfloat s, GLfloat t) { glTexCoord4f(s, t, 0, 1); }
void glTexCoord2d(GLdouble s, GLdouble t) { glTexCoord4f((float)s, (float)t, 0, 1); }
void glTexCoord2i(GLint s, GLint t) { glTexCoord4f((float)s, (float)t, 0, 1); }
void glTexCoord2fv(const GLfloat *v) { glTexCoord4f(v[0], v[1], 0, 1); }
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) { glTexCoord4f(s, t, r, 1); }
void glTexCoord4fv(const GLfloat *v) { glTexCoord4f(v[0], v[1], v[2], v[3]); }
