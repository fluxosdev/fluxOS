/*
 * Pixels: glReadPixels and glGetTexImage, with the glPixelStorei pack
 * rules; glReadBuffer and glDrawBuffer, which have only the one buffer to
 * name; and the drawing done at the raster position: glBitmap, glDrawPixels
 * and glCopyPixels, with glPixelZoom.
 *
 * Colors read back come from the backend's whole picture; depths from its
 * read_depth, which the Radeon and Intel (whose depth buffers are tiled)
 * lack. They read as GL defines them for a buffer without alpha: alpha is
 * 1, and luminance is red plus green plus blue, clamped.
 *
 * What is drawn at the raster position goes to the backend as polygons,
 * so it takes the fragment operations as everything else does: a bitmap as
 * a rectangle for each run of set bits in a row, in the raster color and
 * at the raster position's texture coordinates; an image as a texture of
 * its pixels (in pieces, past the largest texture), drawn over its
 * rectangle with GL_REPLACE and nearest texels. Images are not textured
 * by the program's textures.
 */
#include <math.h>
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

static int buffer_ok(GLenum mode)
{
    switch (mode) {
    case GL_NONE: case GL_FRONT_LEFT: case GL_FRONT_RIGHT: case GL_BACK_LEFT: case GL_BACK_RIGHT:
    case GL_FRONT: case GL_BACK: case GL_LEFT: case GL_RIGHT: case GL_FRONT_AND_BACK:
        return 1;
    }
    return 0;
}

/* Every buffer name means the one picture; the right buffers and none are
 * accepted and remembered, nothing more. */
void glDrawBuffer(GLenum mode)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    if (!buffer_ok(mode)) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->draw_buffer = mode;
}

void glReadBuffer(GLenum mode)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    if (!buffer_ok(mode) || mode == GL_NONE || mode == GL_FRONT_AND_BACK) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->read_buffer = mode;
}

uint32_t *gl_read_picture(struct fgl_context *c)
{
    uint32_t *picture = malloc((size_t)c->width * (size_t)c->height * 4);
    if (picture) {
        c->be->finish(c);
        c->be->read(c, picture, c->width);
    }
    return picture;
}

/* ---- reading pixels back ---- */

/* Components a pixel of this format has; 0 if glReadPixels does not take it. */
static int read_format_size(GLenum format)
{
    switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT:
        return 1;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_RGB: case GL_BGR: return 3;
    case GL_RGBA: case GL_BGRA: return 4;
    default: return 0;
    }
}

static int read_type_size(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE: return 1;
    case GL_UNSIGNED_SHORT: return 2;
    case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
    default: return 0;
    }
}

/* One component, 0 to 1, stored as type at p. */
static void put(void *p, GLenum type, float v)
{
    v = gl_clampf(v);
    switch (type) {
    case GL_UNSIGNED_BYTE: *(uint8_t *)p = (uint8_t)gl_iround(v * 255); break;
    case GL_UNSIGNED_SHORT: *(uint16_t *)p = (uint16_t)gl_iround(v * 65535); break;
    case GL_UNSIGNED_INT: *(uint32_t *)p = (uint32_t)((double)v * 4294967295.0 + 0.5); break;
    default: *(float *)p = v; break;
    }
}

/* A pixel's components in format: rgba for a color, rgba[0] for a depth. */
static int components(GLenum format, const float *rgba, float *out)
{
    float r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];
    switch (format) {
    case GL_RED: out[0] = r; return 1;
    case GL_GREEN: out[0] = g; return 1;
    case GL_BLUE: out[0] = b; return 1;
    case GL_ALPHA: out[0] = a; return 1;
    case GL_LUMINANCE: out[0] = r + g + b; return 1;
    case GL_LUMINANCE_ALPHA: out[0] = r + g + b; out[1] = a; return 2;
    case GL_RGB: out[0] = r; out[1] = g; out[2] = b; return 3;
    case GL_BGR: out[0] = b; out[1] = g; out[2] = r; return 3;
    case GL_RGBA: out[0] = r; out[1] = g; out[2] = b; out[3] = a; return 4;
    case GL_BGRA: out[0] = b; out[1] = g; out[2] = r; out[3] = a; return 4;
    default: out[0] = r; return 1;     /* GL_DEPTH_COMPONENT */
    }
}

/* Where an image width pixels wide goes in client memory under the pack
 * rules: the first pixel, and the bytes from a row and a pixel to the next. */
static uint8_t *pack_layout(struct fgl_context *c, void *pixels, int width, int comps, int tsize, size_t *row,
                            size_t *group)
{
    *group = (size_t)comps * (size_t)tsize;
    int row_pixels = c->pack_row_length > 0 ? c->pack_row_length : width;
    size_t align = (size_t)c->pack_align;
    *row = tsize >= (int)align ? (size_t)row_pixels * *group
                               : ((size_t)row_pixels * *group + align - 1) / align * align;
    return (uint8_t *)pixels + (size_t)c->pack_skip_rows * *row + (size_t)c->pack_skip_pixels * *group;
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                  GLvoid *pixels)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    int comps = read_format_size(format), tsize = read_type_size(type);
    if (!comps || !tsize) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    int depth = format == GL_DEPTH_COMPONENT;
    if (depth && !c->be->read_depth) {
        gl_error(c, GL_INVALID_OPERATION);
        return;
    }
    /* Only the part inside the picture is read; GL leaves the rest undefined. */
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + width > c->width ? c->width : x + width;
    int y1 = y + height > c->height ? c->height : y + height;
    if (x1 <= x0 || y1 <= y0)
        return;
    int w = x1 - x0, h = y1 - y0;

    /* The picture's rows are top first, GL's bottom first: GL's row y0 is
     * the picture's row height - 1 - y0. */
    uint32_t *picture = NULL;
    float *depths = NULL;
    if (depth) {
        c->be->finish(c);
        if ((depths = malloc((size_t)w * (size_t)h * sizeof(float))))
            c->be->read_depth(c, x0, c->height - y1, w, h, depths);
    } else {
        picture = gl_read_picture(c);
    }
    if (!picture && !depths) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }

    size_t row, group;
    uint8_t *base = pack_layout(c, pixels, width, comps, tsize, &row, &group);
    for (int j = 0; j < h; j++) {
        int gy = y0 + j;                        /* GL's row */
        uint8_t *out = base + (size_t)(gy - y) * row + (size_t)(x0 - x) * group;
        for (int i = 0; i < w; i++) {
            float rgba[4] = { 0, 0, 0, 1 }, v[4];
            if (depth) {
                rgba[0] = depths[(size_t)(h - 1 - j) * (size_t)w + (size_t)i];
            } else {
                uint32_t px = picture[(size_t)(c->height - 1 - gy) * (size_t)c->width + (size_t)(x0 + i)];
                rgba[0] = (float)((px >> 16) & 0xFF) / 255.0f;
                rgba[1] = (float)((px >> 8) & 0xFF) / 255.0f;
                rgba[2] = (float)(px & 0xFF) / 255.0f;
                rgba[3] = 1;
            }
            int n = components(format, rgba, v);
            for (int k = 0; k < n; k++, out += tsize)
                put(out, type, v[k]);
        }
    }
    free(picture);
    free(depths);
}

/* A level's texels in format and type. The texture's components come out
 * as GL has them: luminance and intensity as red (intensity as alpha too),
 * and what the base format lacks as 0, alpha as 1. */
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    int comps = read_format_size(format), tsize = read_type_size(type);
    if (target != GL_TEXTURE_2D || !comps || format == GL_DEPTH_COMPONENT || !tsize) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (level < 0 || level >= MAX_LEVELS) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    struct gltex *t = gl_bound_texture(c);
    struct texlevel l = t ? gl_tex_level(t, level) : (struct texlevel){ 0, 0, NULL };
    if (!l.texels)
        return;
    size_t row, group;
    uint8_t *base = pack_layout(c, pixels, l.w, comps, tsize, &row, &group);
    for (int j = 0; j < l.h; j++) {
        uint8_t *out = base + (size_t)j * row;
        for (int i = 0; i < l.w; i++) {
            uint32_t tx = l.texels[(size_t)j * (size_t)l.w + (size_t)i];
            float r = (float)(tx & 0xFF) / 255.0f, g = (float)((tx >> 8) & 0xFF) / 255.0f;
            float b = (float)((tx >> 16) & 0xFF) / 255.0f, a = (float)(tx >> 24) / 255.0f;
            float rgba[4], v[4];
            switch (t->format) {
            case GL_LUMINANCE: rgba[0] = r; rgba[1] = rgba[2] = 0; rgba[3] = 1; break;
            case GL_LUMINANCE_ALPHA: rgba[0] = r; rgba[1] = rgba[2] = 0; rgba[3] = a; break;
            case GL_INTENSITY: rgba[0] = rgba[3] = r; rgba[1] = rgba[2] = 0; break;
            case GL_ALPHA: rgba[0] = rgba[1] = rgba[2] = 0; rgba[3] = a; break;
            case GL_RGB: rgba[0] = r; rgba[1] = g; rgba[2] = b; rgba[3] = 1; break;
            default: rgba[0] = r; rgba[1] = g; rgba[2] = b; rgba[3] = a; break;
            }
            int n = components(format, rgba, v);
            for (int k = 0; k < n; k++, out += tsize)
                put(out, type, v[k]);
        }
    }
}

/* ---- drawing at the raster position ---- */

void glPixelZoom(GLfloat xfactor, GLfloat yfactor)
{
    GET_CTX(c);
    RECORD(c, OP_PIXEL_ZOOM, 2, xfactor, yfactor);
    OUTSIDE_BEGIN(c);
    c->zoom_x = xfactor;
    c->zoom_y = yfactor;
}

/* A vertex at GL's window (x, y) with what the raster position gives a
 * fragment: its depth, color, texture coordinates and fog. The draw must
 * have been through gl_check_backend. */
static void raster_vertex(struct fgl_context *c, struct wvert *v, float x, float y)
{
    v->x = x;
    v->y = (float)c->height - y;
    v->z = c->raster_pos[2];
    v->r = c->raster_color[0];
    v->g = c->raster_color[1];
    v->b = c->raster_color[2];
    v->a = c->raster_color[3];
    v->f = 1;
    if (c->fog_draw != FOG_NONE) {
        float f = gl_fog_factor(c, c->raster_distance);
        if (c->fog_draw == FOG_PIXEL) {
            v->f = f;
        } else {
            v->r = f * v->r + (1 - f) * c->fog_color[0];
            v->g = f * v->g + (1 - f) * c->fog_color[1];
            v->b = f * v->b + (1 - f) * c->fog_color[2];
        }
    }
    const float *t0 = c->raster_tex[0], *t1 = c->raster_tex[1];
    float q0 = t0[3] != 0 ? 1 / t0[3] : 1, q1 = t1[3] != 0 ? 1 / t1[3] : 1;
    v->s = t0[0] * q0;
    v->t = t0[1] * q0;
    v->s1 = t1[0] * q1;
    v->t1 = t1[1] * q1;
    v->w = 1;
}

void gl_draw_bitmap(struct fgl_context *c, int w, int h, float xorig, float yorig, float xmove, float ymove,
                    const uint8_t *on)
{
    if (!c->raster_valid)
        return;
    if (on && w > 0 && h > 0) {
        gl_check_backend(c);
        /* Bit (i, j) is the fragment at (floor(xr - xorig) + i, floor(yr - yorig) + j). */
        int bx = (int)floorf(c->raster_pos[0] - xorig), by = (int)floorf(c->raster_pos[1] - yorig);
        struct wvert q[4];
        raster_vertex(c, &q[0], 0, 0);
        q[1] = q[2] = q[3] = q[0];
        for (int j = 0; j < h; j++) {
            int gy = by + j;
            if (gy < 0 || gy >= c->height)
                continue;
            const uint8_t *row = on + (size_t)j * (size_t)w;
            for (int i = 0; i < w;) {
                if (!row[i]) {
                    i++;
                    continue;
                }
                int i0 = i;
                while (i < w && row[i])
                    i++;
                float x0 = (float)(bx + i0), x1 = (float)(bx + i);
                if (x1 <= 0 || x0 >= (float)c->width)
                    continue;
                float top = (float)(c->height - gy - 1), bottom = top + 1;
                q[0].x = x0; q[0].y = top;
                q[1].x = x1; q[1].y = top;
                q[2].x = x1; q[2].y = bottom;
                q[3].x = x0; q[3].y = bottom;
                c->be->polygon(c, q, 4);
            }
        }
    }
    c->raster_pos[0] += xmove;
    c->raster_pos[1] += ymove;
}

/* The bits of a bitmap in client memory (rows from the bottom, the first
 * bit the most significant unless GL_UNPACK_LSB_FIRST, with the unpack
 * rules) as a byte for each pixel. */
static void unpack_bitmap(struct fgl_context *c, int w, int h, const GLubyte *bitmap, uint8_t *on)
{
    int row_bits = c->unpack_row_length > 0 ? c->unpack_row_length : w;
    size_t align = (size_t)c->unpack_align;
    size_t row = (((size_t)row_bits + 7) / 8 + align - 1) / align * align;
    const uint8_t *src = bitmap + (size_t)c->unpack_skip_rows * row;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int bit = c->unpack_skip_pixels + i;
            uint8_t byte = src[(size_t)j * row + (size_t)(bit >> 3)];
            uint8_t mask = c->unpack_lsb_first ? (uint8_t)(1u << (bit & 7)) : (uint8_t)(0x80u >> (bit & 7));
            on[(size_t)j * (size_t)w + (size_t)i] = (byte & mask) != 0;
        }
    }
}

/* A display list keeps a bitmap as its arguments, then its bits sixteen
 * to a float (which holds them exactly), as they were when it was compiled. */
static int record_bitmap(struct fgl_context *c, int w, int h, float xorig, float yorig, float xmove, float ymove,
                         const uint8_t *on)
{
    size_t bits = on ? (size_t)w * (size_t)h : 0, n = 6 + (bits + 15) / 16;
    float *args = calloc(n, sizeof(float));
    if (!args) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return c->list_mode == GL_COMPILE;
    }
    args[0] = (float)w;
    args[1] = (float)(on ? h : 0);
    args[2] = xorig;
    args[3] = yorig;
    args[4] = xmove;
    args[5] = ymove;
    for (size_t i = 0; i < bits; i += 16) {
        unsigned v = 0;
        for (size_t k = 0; k < 16 && i + k < bits; k++)
            v |= (unsigned)on[i + k] << k;
        args[6 + i / 16] = (float)v;
    }
    int skip = gl_record(c, OP_BITMAP, (int)n, args);
    free(args);
    return skip;
}

void glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove,
              const GLubyte *bitmap)
{
    GET_CTX(c);
    if (width < 0 || height < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    uint8_t *on = NULL;
    if (width && height && bitmap) {
        if (!(on = malloc((size_t)width * (size_t)height))) {
            gl_error(c, GL_OUT_OF_MEMORY);
            return;
        }
        unpack_bitmap(c, width, height, bitmap, on);
    }
    if (c->compiling && !c->executing && record_bitmap(c, width, height, xorig, yorig, xmove, ymove, on)) {
        free(on);
        return;
    }
    if (c->in_begin)
        gl_error(c, GL_INVALID_OPERATION);
    else
        gl_draw_bitmap(c, width, height, xorig, yorig, xmove, ymove, on);
    free(on);
}

/* Texture t over GL's window rectangle from (x0, y0) to (x1, y1), its
 * texels replacing the raster color. */
static void draw_image(struct fgl_context *c, struct gltex *t, float x0, float y0, float x1, float y1)
{
    struct texunit *u0 = &c->unit[0], *u1 = &c->unit[1];
    GLenum mode = u0->env_mode;
    int on1 = u1->enabled;
    c->tex_override = t;
    u0->env_mode = GL_REPLACE;
    u1->enabled = 0;
    c->hw_dirty = 1;
    gl_check_backend(c);
    struct wvert q[4];
    raster_vertex(c, &q[0], x0, y0);
    q[1] = q[2] = q[3] = q[0];
    q[1].x = x1;
    q[2].x = x1;
    q[2].y = q[3].y = (float)c->height - y1;
    q[0].s = q[3].s = 0;
    q[1].s = q[2].s = 1;
    q[0].t = q[1].t = 0;
    q[2].t = q[3].t = 1;
    c->be->polygon(c, q, 4);
    c->tex_override = NULL;
    u0->env_mode = mode;
    u1->enabled = on1;
    c->hw_dirty = 1;
}

/* A w x h image of 0xAABBGGRR texels, rows from the bottom, drawn at the
 * raster position with the pixel zoom. */
static void draw_texels(struct fgl_context *c, const uint32_t *img, int w, int h)
{
    float rx = c->raster_pos[0], ry = c->raster_pos[1], zx = c->zoom_x, zy = c->zoom_y;
    for (int ty = 0; ty < h; ty += MAX_TEXTURE) {
        for (int tx = 0; tx < w; tx += MAX_TEXTURE) {
            int tw = w - tx < MAX_TEXTURE ? w - tx : MAX_TEXTURE;
            int th = h - ty < MAX_TEXTURE ? h - ty : MAX_TEXTURE;
            struct gltex *t = gl_tex_temp(tw, th);
            if (!t) {
                gl_error(c, GL_OUT_OF_MEMORY);
                return;
            }
            for (int j = 0; j < th; j++)
                memcpy(t->texels + (size_t)j * (size_t)tw, img + (size_t)(ty + j) * (size_t)w + (size_t)tx,
                       (size_t)tw * 4);
            draw_image(c, t, rx + (float)tx * zx, ry + (float)ty * zy, rx + (float)(tx + tw) * zx,
                       ry + (float)(ty + th) * zy);
            gl_tex_destroy(c, t);
        }
    }
}

void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    if (format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX) {
        gl_error(c, GL_INVALID_OPERATION);      /* only colors are drawn */
        return;
    }
    if (!gl_format_size(format) || type != GL_UNSIGNED_BYTE) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    if (!c->raster_valid || !width || !height || !pixels)
        return;
    uint32_t *img = malloc((size_t)width * (size_t)height * 4);
    if (!img) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    gl_unpack_image(c, img, width, width, height, format, GL_RGBA, pixels);
    draw_texels(c, img, width, height);
    free(img);
}

void glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type)
{
    GET_CTX(c);
    OUTSIDE_BEGIN(c);
    if (type != GL_COLOR && type != GL_DEPTH && type != GL_STENCIL) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    if (type != GL_COLOR) {
        gl_error(c, GL_INVALID_OPERATION);      /* only colors are copied */
        return;
    }
    if (!c->raster_valid || !width || !height)
        return;
    uint32_t *picture = gl_read_picture(c);
    uint32_t *img = malloc((size_t)width * (size_t)height * 4);
    if (!picture || !img) {
        free(picture);
        free(img);
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    /* Outside the picture is undefined: black. */
    for (int j = 0; j < height; j++) {
        int gy = y + j;
        for (int i = 0; i < width; i++) {
            int gx = x + i;
            uint32_t px = gx >= 0 && gx < c->width && gy >= 0 && gy < c->height
                              ? picture[(size_t)(c->height - 1 - gy) * (size_t)c->width + (size_t)gx]
                              : 0;
            img[(size_t)j * (size_t)width + (size_t)i] =
                0xFF000000u | (px & 0xFF) << 16 | (px & 0xFF00) | ((px >> 16) & 0xFF);
        }
    }
    free(picture);
    draw_texels(c, img, width, height);
    free(img);
}
