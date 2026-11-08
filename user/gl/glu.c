/* GLU's view helpers, built on glFrustum, glOrtho and glMultMatrixf, and
 * gluBuild2DMipmaps. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <GL/glu.h>
#include "glint.h"

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble z_near, GLdouble z_far)
{
    double top = z_near * tan(fovy * M_PI / 360.0), right = top * aspect;
    glFrustum(-right, right, -top, top, z_near, z_far);
}

void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top)
{
    glOrtho(left, right, bottom, top, -1, 1);
}

static void normalize(double *v)
{
    double len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

static void cross(const double *a, const double *b, double *out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void gluLookAt(GLdouble eye_x, GLdouble eye_y, GLdouble eye_z,
               GLdouble center_x, GLdouble center_y, GLdouble center_z,
               GLdouble up_x, GLdouble up_y, GLdouble up_z)
{
    double f[3] = { center_x - eye_x, center_y - eye_y, center_z - eye_z };
    double up[3] = { up_x, up_y, up_z }, s[3], u[3];
    normalize(f);
    cross(f, up, s);
    normalize(s);
    cross(s, f, u);
    GLfloat m[16] = {
        (float)s[0], (float)u[0], (float)-f[0], 0,
        (float)s[1], (float)u[1], (float)-f[1], 0,
        (float)s[2], (float)u[2], (float)-f[2], 0,
        0, 0, 0, 1,
    };
    glMultMatrixf(m);
    glTranslated(-eye_x, -eye_y, -eye_z);
}

/* An RGBA image w x h halved in place (a box filter over two by two
 * texels, or two in a line once a side is 1): each texel written comes
 * after every one it reads. */
static void halve(unsigned char *img, int *pw, int *ph)
{
    int w = *pw, h = *ph;
    int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    int sx = w > 1 ? 2 : 1, sy = h > 1 ? 2 : 1;
    for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
            for (int k = 0; k < 4; k++) {
                unsigned sum = 0;
                for (int j = 0; j < sy; j++)
                    for (int i = 0; i < sx; i++)
                        sum += img[((size_t)(y * sy + j) * (size_t)w + (size_t)(x * sx + i)) * 4 + (size_t)k];
                img[((size_t)y * (size_t)nw + (size_t)x) * 4 + (size_t)k] =
                    (unsigned char)((sum + (unsigned)(sx * sy) / 2) / (unsigned)(sx * sy));
            }
        }
    }
    *pw = nw;
    *ph = nh;
}

/* The image from the program (with its unpack rules) as RGBA, halved
 * until it fits the largest texture, is level 0; then each level the last
 * one halved, down to 1 x 1. The GL errors the uploads raise are left for
 * glGetError, behind any the program had not read yet; the result is a GLU
 * error code, or 0. */
GLint gluBuild2DMipmaps(GLenum target, GLint internal_format, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, const void *data)
{
    struct fgl_context *c = gl_ctx;
    if (width < 1 || height < 1)
        return GLU_INVALID_VALUE;
    if (!gl_format_size(format) || type != GL_UNSIGNED_BYTE)
        return GLU_INVALID_ENUM;
    if (!c || c->in_begin)
        return GLU_INVALID_OPERATION;
    unsigned char *img = gl_size_ok((size_t)width, (size_t)height, 4) ? malloc((size_t)width * (size_t)height * 4)
                                                                     : NULL;
    if (!img)
        return GLU_OUT_OF_MEMORY;
    if (data)
        gl_unpack_image(c, (uint32_t *)(void *)img, width, width, height, format, GL_RGBA, data);
    else
        memset(img, 0, (size_t)width * (size_t)height * 4);
    int w = width, h = height;
    while (w > MAX_TEXTURE || h > MAX_TEXTURE)
        halve(img, &w, &h);

    GLenum pending = c->error;
    c->error = GL_NO_ERROR;
    /* The levels are tightly packed: the program's unpack rules are put
     * aside meanwhile (not on its glPushClientAttrib stack, which may be full). */
    int align = c->unpack_align, row_length = c->unpack_row_length;
    int skip_rows = c->unpack_skip_rows, skip_pixels = c->unpack_skip_pixels;
    c->unpack_align = 1;
    c->unpack_row_length = c->unpack_skip_rows = c->unpack_skip_pixels = 0;
    glTexImage2D(target, 0, internal_format, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    for (GLint level = 1; c->error == GL_NO_ERROR && (w > 1 || h > 1); level++) {
        halve(img, &w, &h);
        glTexImage2D(target, level, internal_format, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    }
    c->unpack_align = align;
    c->unpack_row_length = row_length;
    c->unpack_skip_rows = skip_rows;
    c->unpack_skip_pixels = skip_pixels;
    free(img);
    GLenum e = c->error;
    c->error = pending != GL_NO_ERROR ? pending : e;
    switch (e) {
    case GL_NO_ERROR: return 0;
    case GL_INVALID_ENUM: return GLU_INVALID_ENUM;
    case GL_OUT_OF_MEMORY: return GLU_OUT_OF_MEMORY;
    case GL_INVALID_OPERATION: return GLU_INVALID_OPERATION;
    default: return GLU_INVALID_VALUE;
    }
}
