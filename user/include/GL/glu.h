/* A few GLU helpers for setting up views. */
#ifndef _GL_GLU_H
#define _GL_GLU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <GL/gl.h>

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble z_near, GLdouble z_far);
void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top);
void gluLookAt(GLdouble eye_x, GLdouble eye_y, GLdouble eye_z,
               GLdouble center_x, GLdouble center_y, GLdouble center_z,
               GLdouble up_x, GLdouble up_y, GLdouble up_z);
/* A texture and its mipmap levels, each half the last, down to 1 x 1. */
GLint gluBuild2DMipmaps(GLenum target, GLint internal_format, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, const void *data);

#ifdef __cplusplus
}
#endif

#endif
