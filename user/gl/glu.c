/* GLU's view helpers, built on glFrustum, glOrtho and glMultMatrixf, and
 * gluBuild2DMipmaps. */
#include <math.h>
#include <stdlib.h>
#include <GL/glu.h>

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

/* Level 0 from the program, then each level the last one halved (a box
 * filter over two by two texels, or two in a line once a side is 1), down
 * to 1 x 1. The levels come from level 0 as glGetTexImage reads it back
 * in RGBA, which glTexImage2D takes again into the same internal format. */
GLint gluBuild2DMipmaps(GLenum target, GLint internal_format, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, const void *data)
{
    glTexImage2D(target, 0, internal_format, width, height, 0, format, type, data);
    if (glGetError() != GL_NO_ERROR)
        return GL_INVALID_VALUE;
    if (width < 1 || height < 1)
        return 0;
    unsigned char *img = malloc((size_t)width * (size_t)height * 4);
    if (!img)
        return GL_OUT_OF_MEMORY;
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glGetTexImage(target, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    int w = width, h = height;
    for (GLint level = 1; w > 1 || h > 1; level++) {
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        int sx = w > 1 ? 2 : 1, sy = h > 1 ? 2 : 1;
        /* In place: each texel written comes after every one it reads. */
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
        w = nw;
        h = nh;
        glTexImage2D(target, level, internal_format, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    }
    glPopClientAttrib();
    free(img);
    return glGetError() == GL_NO_ERROR ? 0 : GL_INVALID_VALUE;
}
