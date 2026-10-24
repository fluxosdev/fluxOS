/* GLU's view helpers, built on glFrustum, glOrtho and glMultMatrixf. */
#include <math.h>
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
