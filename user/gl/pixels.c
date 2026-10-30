/*
 * Reading the picture back: glReadPixels, with the glPixelStorei pack
 * rules, and glReadBuffer and glDrawBuffer, which have only the one
 * buffer to name. Colors come from the backend's whole picture; depths
 * from its read_depth, which the Radeon (whose depth buffer is tiled)
 * lacks.
 *
 * Colors read as GL defines them for a buffer without alpha: alpha is 1,
 * and luminance is red plus green plus blue, clamped.
 */
#include <stdlib.h>
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
    c->be->finish(c);
    uint32_t *picture = NULL;
    float *depths = NULL;
    if (depth)
        depths = malloc((size_t)w * (size_t)h * sizeof(float));
    else
        picture = malloc((size_t)c->width * (size_t)c->height * 4);
    if (!picture && !depths) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    if (depth)
        c->be->read_depth(c, x0, c->height - y1, w, h, depths);
    else
        c->be->read(c, picture, c->width);

    size_t group = (size_t)comps * (size_t)tsize;
    int row_pixels = c->pack_row_length > 0 ? c->pack_row_length : width;
    size_t align = (size_t)c->pack_align;
    size_t row = tsize >= (int)align ? (size_t)row_pixels * group
                                     : ((size_t)row_pixels * group + align - 1) / align * align;
    uint8_t *base = (uint8_t *)pixels + (size_t)c->pack_skip_rows * row + (size_t)c->pack_skip_pixels * group;
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
