/*
 * Textures: texture objects and their mipmap levels, uploading texels
 * from the formats GL 1.1 has (unsigned bytes, with the glPixelStorei
 * unpack rules) or copying them from the picture, texture parameters, the
 * texture environment and coordinate generation, and the texture units of
 * GL_ARB_multitexture. glPixelStorei is here, its pack rules for
 * pixels.c's glReadPixels and glGetTexImage.
 *
 * Every texture is kept as 0xAABBGGRR texels whatever it came as, so the
 * backends only ever see one format. Levels 0, 1, ... as far as their
 * sizes halve make the mipmap the software backend samples; the hardware
 * backends draw with level 0, the mipmap filters as their nearest or
 * linear counterparts.
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
    t->internal = 1;
    t->nlevels = 1;
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
    t->priority = 1;
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
    for (int i = 1; i < MAX_LEVELS; i++)
        free(t->mip[i].texels);
    free(t);
}

/* Make sure slot name exists. */
static int ensure_slot(struct fgl_context *c, GLuint name)
{
    if (name < c->ntextures)
        return 0;
    if (name >= 0x10000000u)            /* so n * 2 and n * sizeof cannot wrap */
        return -1;
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
    for (int k = 0; k < MAX_UNITS; k++) {
        struct texunit *u = &c->unit[k];
        u->env_mode = GL_MODULATE;
        gl_mat_identity(u->matrix[0]);
        u->coord[3] = 1;
        for (int i = 0; i < 4; i++)
            u->gen_mode[i] = GL_EYE_LINEAR;
        u->gen_object[0][0] = u->gen_eye[0][0] = 1;     /* s = x, t = y */
        u->gen_object[1][1] = u->gen_eye[1][1] = 1;
    }
    c->unpack_align = 4;
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

static struct gltex *unit_bound(struct fgl_context *c, int u)
{
    GLuint name = c->unit[u].bound;
    return name < c->ntextures ? c->textures[name] : NULL;
}

struct gltex *gl_bound_texture(struct fgl_context *c)
{
    return unit_bound(c, c->active_unit);
}

struct gltex *gl_unit_texture(struct fgl_context *c, int u)
{
    if (u == 0 && c->tex_override)
        return c->tex_override;
    if (!c->unit[u].enabled)
        return NULL;
    struct gltex *t = unit_bound(c, u);
    return t && t->texels && t->w > 0 && t->h > 0 ? t : NULL;
}

struct texlevel gl_tex_level(const struct gltex *t, int i)
{
    if (i == 0) {
        struct texlevel l = { t->w, t->h, t->texels };
        return l;
    }
    return t->mip[i];
}

struct gltex *gl_tex_temp(int w, int h)
{
    struct gltex *t = tex_new(0);
    if (!t)
        return NULL;
    t->texels = malloc((size_t)w * (size_t)h * 4);
    if (!t->texels) {
        free(t);
        return NULL;
    }
    t->w = w;
    t->h = h;
    t->min_filter = t->mag_filter = GL_NEAREST;
    t->wrap_s = t->wrap_t = GL_CLAMP_TO_EDGE;
    return t;
}

void gl_tex_destroy(struct fgl_context *c, struct gltex *t)
{
    tex_free(c, t);
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
        for (int u = 0; u < MAX_UNITS; u++)
            if (c->unit[u].bound == name)
                c->unit[u].bound = 0;
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
    struct texunit *u = &c->unit[c->active_unit];
    if (u->bound != texture)
        c->hw_dirty = 1;
    u->bound = texture;
}

GLboolean glIsTexture(GLuint texture)
{
    struct fgl_context *c = gl_ctx;
    return c && texture && texture < c->ntextures && c->textures[texture] ? GL_TRUE : GL_FALSE;
}

/* ---- texels ---- */

int gl_format_size(GLenum format)
{
    switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: return 1;
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

/* One pixel from client memory as R, G, B, A (what a format lacks is 0, and
 * alpha 1), then as the base format keeps it. An alpha texture's color is
 * white, so that it modulates the fragment's color as GL has it pass
 * through. */
static uint32_t convert(const uint8_t *p, GLenum format, GLenum base)
{
    uint32_t r = 0, g = 0, b = 0, a = 255;
    switch (format) {
    case GL_RED: r = p[0]; break;
    case GL_GREEN: g = p[0]; break;
    case GL_BLUE: b = p[0]; break;
    case GL_ALPHA: a = p[0]; break;
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

void gl_unpack_image(struct fgl_context *c, uint32_t *dst, int dst_stride, int w, int h, GLenum format,
                     GLenum base, const void *pixels)
{
    int comps = gl_format_size(format);
    int row_pixels = c->unpack_row_length > 0 ? c->unpack_row_length : w;
    size_t row = ((size_t)row_pixels * (size_t)comps + (size_t)c->unpack_align - 1) /
                 (size_t)c->unpack_align * (size_t)c->unpack_align;
    const uint8_t *src = (const uint8_t *)pixels + (size_t)c->unpack_skip_rows * row +
                         (size_t)c->unpack_skip_pixels * (size_t)comps;
    for (int j = 0; j < h; j++) {
        const uint8_t *p = src + (size_t)j * row;
        uint32_t *out = dst + (size_t)j * (size_t)dst_stride;
        for (int i = 0; i < w; i++, p += comps)
            out[i] = convert(p, format, base);
    }
}

/* Count the levels from 0 that make a mipmap: each half the last (at least 1). */
static void update_levels(struct gltex *t)
{
    int n = 1, w = t->w, h = t->h;
    while (n < MAX_LEVELS && (w > 1 || h > 1)) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        if (!t->mip[n].texels || t->mip[n].w != w || t->mip[n].h != h)
            break;
        n++;
    }
    t->nlevels = n;
}

/* Give the texture new texels for a level (they are its now). */
static void set_level(struct fgl_context *c, struct gltex *t, int level, int w, int h, uint32_t *texels)
{
    if (level == 0) {
        free(t->texels);
        t->texels = texels;
        t->w = w;
        t->h = h;
        t->hw_stale = 1;
        c->hw_dirty = 1;
    } else {
        free(t->mip[level].texels);
        t->mip[level].texels = texels;
        t->mip[level].w = w;
        t->mip[level].h = h;
    }
    update_levels(t);
}

/* The checks glTexImage2D and glCopyTexImage2D share; the base format, or 0 after an error. */
static GLenum check_image(struct fgl_context *c, GLenum target, GLint level, GLint internal_format,
                          GLsizei width, GLsizei height, GLint border)
{
    GLenum base = base_format(internal_format);
    if (target != GL_TEXTURE_2D) {
        gl_error(c, GL_INVALID_ENUM);
        return 0;
    }
    if (level < 0 || level >= MAX_LEVELS || !base || border != 0 || width < 0 || height < 0 ||
        width > MAX_TEXTURE || height > MAX_TEXTURE) {
        gl_error(c, GL_INVALID_VALUE);
        return 0;
    }
    return base;
}

void glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    if (!gl_format_size(format) || type != GL_UNSIGNED_BYTE) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    GLenum base = check_image(c, target, level, internal_format, width, height, border);
    struct gltex *t = gl_bound_texture(c);
    if (!base || !t)
        return;
    uint32_t *texels = NULL;
    if (width && height && !(texels = calloc((size_t)width * (size_t)height, 4))) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    if (level == 0) {
        t->format = base;
        t->internal = internal_format;
    }
    if (texels && pixels)
        gl_unpack_image(c, texels, width, width, height, format, t->format, pixels);
    set_level(c, t, level, width, height, texels);
}

/* The level's texels a sub-image goes into, if (x, y, width, height) is inside it. */
static struct texlevel *sub_level(struct fgl_context *c, GLenum target, GLint level, GLint x, GLint y,
                                  GLsizei width, GLsizei height, struct texlevel *out)
{
    struct gltex *t = gl_bound_texture(c);
    if (target != GL_TEXTURE_2D) {
        gl_error(c, GL_INVALID_ENUM);
        return NULL;
    }
    if (!t || level < 0 || level >= MAX_LEVELS) {
        gl_error(c, GL_INVALID_VALUE);
        return NULL;
    }
    *out = gl_tex_level(t, level);
    if (!out->texels || x < 0 || y < 0 || width < 0 || height < 0 || x > out->w - width || y > out->h - height) {
        gl_error(c, GL_INVALID_VALUE);
        return NULL;
    }
    if (level == 0) {
        t->hw_stale = 1;
        c->hw_dirty = 1;
    }
    return out;
}

void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const GLvoid *pixels)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    if (!gl_format_size(format) || type != GL_UNSIGNED_BYTE) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    struct texlevel l;
    if (!sub_level(c, target, level, x, y, width, height, &l) || !width || !height || !pixels)
        return;
    gl_unpack_image(c, l.texels + (size_t)y * (size_t)l.w + x, l.w, width, height, format,
                    gl_bound_texture(c)->format, pixels);
}

/* Copy the picture's w x h pixels from GL's window (x, y) up into texels
 * of base format base, stride texels apart. Pixels outside the picture
 * are black. */
static int copy_picture(struct fgl_context *c, int x, int y, int w, int h, GLenum base, uint32_t *dst,
                        int stride)
{
    uint32_t *picture = gl_read_picture(c);
    if (!picture) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return -1;
    }
    for (int j = 0; j < h; j++) {
        int gy = y + j;
        for (int i = 0; i < w; i++) {
            int gx = x + i;
            uint32_t px = gx >= 0 && gx < c->width && gy >= 0 && gy < c->height
                              ? picture[(size_t)(c->height - 1 - gy) * (size_t)c->width + (size_t)gx]
                              : 0;
            uint8_t p[4] = { (uint8_t)(px >> 16), (uint8_t)(px >> 8), (uint8_t)px, 255 };
            dst[(size_t)j * (size_t)stride + (size_t)i] = convert(p, GL_RGBA, base);
        }
    }
    free(picture);
    return 0;
}

void glCopyTexImage2D(GLenum target, GLint level, GLenum internal_format, GLint x, GLint y, GLsizei width,
                      GLsizei height, GLint border)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    GLenum base = check_image(c, target, level, (GLint)internal_format, width, height, border);
    struct gltex *t = gl_bound_texture(c);
    if (!base || !t)
        return;
    uint32_t *texels = NULL;
    if (width && height) {
        if (!(texels = malloc((size_t)width * (size_t)height * 4))) {
            gl_error(c, GL_OUT_OF_MEMORY);
            return;
        }
        if (copy_picture(c, x, y, width, height, level == 0 ? base : t->format, texels, width) < 0) {
            free(texels);
            return;
        }
    }
    if (level == 0) {
        t->format = base;
        t->internal = (GLint)internal_format;
    }
    set_level(c, t, level, width, height, texels);
}

void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                         GLsizei width, GLsizei height)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    struct texlevel l;
    if (!sub_level(c, target, level, xoffset, yoffset, width, height, &l) || !width || !height)
        return;
    copy_picture(c, x, y, width, height, gl_bound_texture(c)->format,
                 l.texels + (size_t)yoffset * (size_t)l.w + xoffset, l.w);
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
        else
            c->pack_align = param;
        return;
    case GL_UNPACK_ROW_LENGTH:
    case GL_UNPACK_SKIP_ROWS:
    case GL_UNPACK_SKIP_PIXELS:
    case GL_PACK_ROW_LENGTH:
    case GL_PACK_SKIP_ROWS:
    case GL_PACK_SKIP_PIXELS:
        if (param < 0) {
            gl_error(c, GL_INVALID_VALUE);
            return;
        }
        switch (pname) {
        case GL_UNPACK_ROW_LENGTH: c->unpack_row_length = param; break;
        case GL_UNPACK_SKIP_ROWS: c->unpack_skip_rows = param; break;
        case GL_UNPACK_SKIP_PIXELS: c->unpack_skip_pixels = param; break;
        case GL_PACK_ROW_LENGTH: c->pack_row_length = param; break;
        case GL_PACK_SKIP_ROWS: c->pack_skip_rows = param; break;
        default: c->pack_skip_pixels = param; break;
        }
        return;
    case GL_UNPACK_LSB_FIRST:
        c->unpack_lsb_first = param != 0;       /* glBitmap's bit order */
        return;
    case GL_UNPACK_SWAP_BYTES:
    case GL_PACK_SWAP_BYTES:
    case GL_PACK_LSB_FIRST:
        return;                                 /* every pixel is bytes: nothing to swap */
    }
    gl_error(c, GL_INVALID_ENUM);
}

void glPixelStoref(GLenum pname, GLfloat param)
{
    glPixelStorei(pname, gl_iround(param));
}

/* ---- parameters ---- */

void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
    GET_CTX(c);
    float v[4] = { params[0], 0, 0, 0 };
    if (pname == GL_TEXTURE_BORDER_COLOR)
        memcpy(v, params, sizeof(v));
    RECORD(c, OP_TEX_PARAMETER, 6, target, pname, v[0], v[1], v[2], v[3]);
    OUTSIDE_BEGIN(c);
    struct gltex *t = gl_bound_texture(c);
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
    case GL_TEXTURE_PRIORITY:
        t->priority = gl_clampf(v[0]);          /* kept; everything stays resident */
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
    else if (pname == GL_TEXTURE_PRIORITY)
        v[0] = gl_int_color(params[0]);
    glTexParameterfv(target, pname, v);
}

/* A texture parameter; the number of values, 0 after an error. */
static int tex_parameter(struct fgl_context *c, GLenum target, GLenum pname, float *v)
{
    struct gltex *t = gl_bound_texture(c);
    if (target != GL_TEXTURE_2D || !t) {
        gl_error(c, GL_INVALID_ENUM);
        return 0;
    }
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: v[0] = (float)t->min_filter; return 1;
    case GL_TEXTURE_MAG_FILTER: v[0] = (float)t->mag_filter; return 1;
    case GL_TEXTURE_WRAP_S: v[0] = (float)t->wrap_s; return 1;
    case GL_TEXTURE_WRAP_T: v[0] = (float)t->wrap_t; return 1;
    case GL_TEXTURE_BORDER_COLOR: memcpy(v, t->border, sizeof(t->border)); return 4;
    case GL_TEXTURE_PRIORITY: v[0] = t->priority; return 1;
    case GL_TEXTURE_RESIDENT: v[0] = 1; return 1;
    }
    gl_error(c, GL_INVALID_ENUM);
    return 0;
}

void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    float v[4];
    int n = tex_parameter(c, target, pname, v);
    memcpy(params, v, (size_t)n * sizeof(float));
}

void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    float v[4];
    int n = tex_parameter(c, target, pname, v);
    int color = pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_PRIORITY;
    for (int i = 0; i < n; i++)
        params[i] = color ? (GLint)((double)v[i] * 2147483647.0) : (GLint)v[i];
}

/* A level's parameter; 1 if pname is one. */
static int level_parameter(struct fgl_context *c, GLenum target, GLint level, GLenum pname, GLint *v)
{
    struct gltex *t = gl_bound_texture(c);
    if (target != GL_TEXTURE_2D || !t) {
        gl_error(c, GL_INVALID_ENUM);
        return 0;
    }
    if (level < 0 || level >= MAX_LEVELS) {
        gl_error(c, GL_INVALID_VALUE);
        return 0;
    }
    struct texlevel l = gl_tex_level(t, level);
    int have = l.texels != NULL;
    GLenum b = t->format;
    /* The bits each component has: 8, if the base format has it at all. */
    int rgb = b == GL_RGB || b == GL_RGBA, alpha = b == GL_RGBA || b == GL_ALPHA || b == GL_LUMINANCE_ALPHA;
    int lum = b == GL_LUMINANCE || b == GL_LUMINANCE_ALPHA;
    switch (pname) {
    case GL_TEXTURE_WIDTH: *v = have ? l.w : 0; return 1;
    case GL_TEXTURE_HEIGHT: *v = have ? l.h : 0; return 1;
    case GL_TEXTURE_INTERNAL_FORMAT: *v = t->internal; return 1;
    case GL_TEXTURE_BORDER: *v = 0; return 1;
    case GL_TEXTURE_RED_SIZE: case GL_TEXTURE_GREEN_SIZE: case GL_TEXTURE_BLUE_SIZE:
        *v = have && rgb ? 8 : 0;
        return 1;
    case GL_TEXTURE_ALPHA_SIZE: *v = have && alpha ? 8 : 0; return 1;
    case GL_TEXTURE_LUMINANCE_SIZE: *v = have && lum ? 8 : 0; return 1;
    case GL_TEXTURE_INTENSITY_SIZE: *v = have && b == GL_INTENSITY ? 8 : 0; return 1;
    }
    gl_error(c, GL_INVALID_ENUM);
    return 0;
}

void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    GLint v;
    if (level_parameter(c, target, level, pname, &v))
        *params = v;
}

void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    GLint v;
    if (level_parameter(c, target, level, pname, &v))
        *params = (GLfloat)v;
}

/* ---- the environment ---- */

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
    struct texunit *u = &c->unit[c->active_unit];
    if (pname == GL_TEXTURE_ENV_MODE) {
        GLenum m = (GLenum)v[0];
        if (m != GL_MODULATE && m != GL_DECAL && m != GL_REPLACE && m != GL_BLEND) {
            gl_error(c, GL_INVALID_ENUM);
            return;
        }
        u->env_mode = m;
        c->hw_dirty = 1;
    } else if (pname == GL_TEXTURE_ENV_COLOR) {
        for (int i = 0; i < 4; i++)
            u->env_color[i] = gl_clampf(v[i]);
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

void glTexEnviv(GLenum target, GLenum pname, const GLint *params)
{
    GLfloat v[4] = { (GLfloat)params[0], 0, 0, 0 };
    if (pname == GL_TEXTURE_ENV_COLOR)
        for (int i = 0; i < 4; i++)
            v[i] = gl_int_color(params[i]);
    glTexEnvfv(target, pname, v);
}

void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    const struct texunit *u = &c->unit[c->active_unit];
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE)
        params[0] = (GLfloat)u->env_mode;
    else if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_COLOR)
        memcpy(params, u->env_color, sizeof(u->env_color));
    else
        gl_error(c, GL_INVALID_ENUM);
}

void glGetTexEnviv(GLenum target, GLenum pname, GLint *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    const struct texunit *u = &c->unit[c->active_unit];
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE)
        params[0] = (GLint)u->env_mode;
    else if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_COLOR)
        for (int i = 0; i < 4; i++)
            params[i] = (GLint)((double)u->env_color[i] * 2147483647.0);
    else
        gl_error(c, GL_INVALID_ENUM);
}

/* ---- coordinate generation ---- */

void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params)
{
    GET_CTX(c);
    float v[4] = { params[0], 0, 0, 0 };
    int plane = pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE;
    if (plane)
        memcpy(v, params, sizeof(v));
    RECORD(c, OP_TEX_GEN, 6, coord, pname, v[0], v[1], v[2], v[3]);
    OUTSIDE_BEGIN(c);
    if (coord < GL_S || coord > GL_Q) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    int k = (int)(coord - GL_S);
    struct texunit *u = &c->unit[c->active_unit];
    switch (pname) {
    case GL_TEXTURE_GEN_MODE: {
        GLenum m = (GLenum)v[0];
        if ((m != GL_OBJECT_LINEAR && m != GL_EYE_LINEAR && m != GL_SPHERE_MAP) || (m == GL_SPHERE_MAP && k >= 2)) {
            gl_error(c, GL_INVALID_ENUM);
            return;
        }
        u->gen_mode[k] = m;
        return;
    }
    case GL_OBJECT_PLANE:
        memcpy(u->gen_object[k], v, sizeof(v));
        return;
    case GL_EYE_PLANE:
        gl_plane_to_eye(c, v, u->gen_eye[k]);
        return;
    }
    gl_error(c, GL_INVALID_ENUM);
}

void glTexGendv(GLenum coord, GLenum pname, const GLdouble *params)
{
    int plane = pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE;
    GLfloat v[4] = { (GLfloat)params[0], 0, 0, 0 };
    for (int i = 1; plane && i < 4; i++)
        v[i] = (GLfloat)params[i];
    glTexGenfv(coord, pname, v);
}

void glTexGeniv(GLenum coord, GLenum pname, const GLint *params)
{
    int plane = pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE;
    GLfloat v[4] = { (GLfloat)params[0], 0, 0, 0 };
    for (int i = 1; plane && i < 4; i++)
        v[i] = (GLfloat)params[i];
    glTexGenfv(coord, pname, v);
}

void glTexGenf(GLenum coord, GLenum pname, GLfloat param) { glTexGenfv(coord, pname, &param); }
void glTexGend(GLenum coord, GLenum pname, GLdouble param) { GLfloat f = (GLfloat)param; glTexGenfv(coord, pname, &f); }
void glTexGeni(GLenum coord, GLenum pname, GLint param) { GLfloat f = (GLfloat)param; glTexGenfv(coord, pname, &f); }

/* A coordinate's generation parameter; the number of values, 0 after an error. */
static int tex_gen(struct fgl_context *c, GLenum coord, GLenum pname, float *v)
{
    if (coord < GL_S || coord > GL_Q) {
        gl_error(c, GL_INVALID_ENUM);
        return 0;
    }
    const struct texunit *u = &c->unit[c->active_unit];
    int k = (int)(coord - GL_S);
    switch (pname) {
    case GL_TEXTURE_GEN_MODE: v[0] = (float)u->gen_mode[k]; return 1;
    case GL_OBJECT_PLANE: memcpy(v, u->gen_object[k], 4 * sizeof(float)); return 4;
    case GL_EYE_PLANE: memcpy(v, u->gen_eye[k], 4 * sizeof(float)); return 4;
    }
    gl_error(c, GL_INVALID_ENUM);
    return 0;
}

void glGetTexGenfv(GLenum coord, GLenum pname, GLfloat *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    float v[4];
    int n = tex_gen(c, coord, pname, v);
    memcpy(params, v, (size_t)n * sizeof(float));
}

void glGetTexGendv(GLenum coord, GLenum pname, GLdouble *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    float v[4];
    int n = tex_gen(c, coord, pname, v);
    for (int i = 0; i < n; i++)
        params[i] = v[i];
}

void glGetTexGeniv(GLenum coord, GLenum pname, GLint *params)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    float v[4];
    int n = tex_gen(c, coord, pname, v);
    for (int i = 0; i < n; i++)
        params[i] = (GLint)v[i];
}

/* ---- texture units and coordinates ---- */

void glActiveTextureARB(GLenum texture)
{
    GET_CTX(c);
    RECORD(c, OP_ACTIVE_TEXTURE, 1, texture);
    if (texture < GL_TEXTURE0_ARB || texture >= GL_TEXTURE0_ARB + MAX_UNITS) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->active_unit = (int)(texture - GL_TEXTURE0_ARB);
}

void glClientActiveTextureARB(GLenum texture)
{
    GET_CTX(c);
    if (texture < GL_TEXTURE0_ARB || texture >= GL_TEXTURE0_ARB + MAX_UNITS) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->client_unit = (int)(texture - GL_TEXTURE0_ARB);
}

void glMultiTexCoord4fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q)
{
    GET_CTX(c);
    RECORD(c, OP_MULTI_TEXCOORD, 5, target, s, t, r, q);
    if (target < GL_TEXTURE0_ARB || target >= GL_TEXTURE0_ARB + MAX_UNITS) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    float *tc = c->unit[target - GL_TEXTURE0_ARB].coord;
    tc[0] = s;
    tc[1] = t;
    tc[2] = r;
    tc[3] = q;
}

/* glTexCoord sets unit 0's. */
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q)
{
    GET_CTX(c);
    RECORD(c, OP_TEXCOORD, 4, s, t, r, q);
    float *tc = c->unit[0].coord;
    tc[0] = s;
    tc[1] = t;
    tc[2] = r;
    tc[3] = q;
}

/* Every other form of glTexCoord and glMultiTexCoord: sfx and type T. */
#define TEXCOORDS(sfx, T)                                                                               \
    void glTexCoord1##sfx(T s) { glTexCoord4f((float)s, 0, 0, 1); }                                   \
    void glTexCoord2##sfx(T s, T t) { glTexCoord4f((float)s, (float)t, 0, 1); }                       \
    void glTexCoord3##sfx(T s, T t, T r) { glTexCoord4f((float)s, (float)t, (float)r, 1); }           \
    void glTexCoord1##sfx##v(const T *v) { glTexCoord4f((float)v[0], 0, 0, 1); }                      \
    void glTexCoord2##sfx##v(const T *v) { glTexCoord4f((float)v[0], (float)v[1], 0, 1); }            \
    void glTexCoord3##sfx##v(const T *v) { glTexCoord4f((float)v[0], (float)v[1], (float)v[2], 1); }  \
    void glTexCoord4##sfx##v(const T *v) { glTexCoord4f((float)v[0], (float)v[1], (float)v[2], (float)v[3]); } \
    void glMultiTexCoord1##sfx##ARB(GLenum u, T s) { glMultiTexCoord4fARB(u, (float)s, 0, 0, 1); }    \
    void glMultiTexCoord2##sfx##ARB(GLenum u, T s, T t) { glMultiTexCoord4fARB(u, (float)s, (float)t, 0, 1); } \
    void glMultiTexCoord3##sfx##ARB(GLenum u, T s, T t, T r)                                          \
    {                                                                                                 \
        glMultiTexCoord4fARB(u, (float)s, (float)t, (float)r, 1);                                     \
    }                                                                                                 \
    void glMultiTexCoord1##sfx##vARB(GLenum u, const T *v) { glMultiTexCoord4fARB(u, (float)v[0], 0, 0, 1); } \
    void glMultiTexCoord2##sfx##vARB(GLenum u, const T *v)                                            \
    {                                                                                                 \
        glMultiTexCoord4fARB(u, (float)v[0], (float)v[1], 0, 1);                                      \
    }                                                                                                 \
    void glMultiTexCoord3##sfx##vARB(GLenum u, const T *v)                                            \
    {                                                                                                 \
        glMultiTexCoord4fARB(u, (float)v[0], (float)v[1], (float)v[2], 1);                            \
    }                                                                                                 \
    void glMultiTexCoord4##sfx##vARB(GLenum u, const T *v)                                            \
    {                                                                                                 \
        glMultiTexCoord4fARB(u, (float)v[0], (float)v[1], (float)v[2], (float)v[3]);                  \
    }
/* ... and the four-coordinate ones not taking floats. */
#define TEXCOORDS4(sfx, T)                                                                              \
    void glTexCoord4##sfx(T s, T t, T r, T q) { glTexCoord4f((float)s, (float)t, (float)r, (float)q); } \
    void glMultiTexCoord4##sfx##ARB(GLenum u, T s, T t, T r, T q)                                     \
    {                                                                                                 \
        glMultiTexCoord4fARB(u, (float)s, (float)t, (float)r, (float)q);                              \
    }

TEXCOORDS(d, GLdouble)
TEXCOORDS(f, GLfloat)
TEXCOORDS(i, GLint)
TEXCOORDS(s, GLshort)
TEXCOORDS4(d, GLdouble)
TEXCOORDS4(i, GLint)
TEXCOORDS4(s, GLshort)

/* GL 1.3's names for GL_ARB_multitexture's calls. */
#define ALIAS(name) __typeof__(name##ARB) name __attribute__((alias(#name "ARB")));
#define ALIASES(sfx)                                                                                   \
    ALIAS(glMultiTexCoord1##sfx) ALIAS(glMultiTexCoord2##sfx) ALIAS(glMultiTexCoord3##sfx)             \
    ALIAS(glMultiTexCoord4##sfx) ALIAS(glMultiTexCoord1##sfx##v) ALIAS(glMultiTexCoord2##sfx##v)       \
    ALIAS(glMultiTexCoord3##sfx##v) ALIAS(glMultiTexCoord4##sfx##v)
ALIASES(d)
ALIASES(f)
ALIASES(i)
ALIASES(s)
ALIAS(glActiveTexture)
ALIAS(glClientActiveTexture)
