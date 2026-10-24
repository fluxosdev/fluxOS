/*
 * fluxOS OpenGL: a subset of OpenGL 1.1's fixed-function pipeline.
 *
 * Supported: immediate mode (glBegin/glEnd with every primitive type),
 * the modelview and projection matrix stacks, per-vertex lighting with
 * eight lights and materials (GL_COLOR_MATERIAL too), flat and smooth
 * shading, back-face culling, the depth buffer, scissoring and display
 * lists. Not supported: textures, blending, fog, stencil, vertex arrays,
 * pixel operations and selection.
 *
 * Create a context and see the picture with <GL/fluxgl.h>. Programs that
 * use GL must be built with the FPU allowed (the Makefile's
 * USER_FP_CFLAGS).
 */
#ifndef _GL_GL_H
#define _GL_GL_H

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;

#define GL_VERSION_1_1 1

#define GL_FALSE 0
#define GL_TRUE  1

/* primitives */
#define GL_POINTS         0x0000
#define GL_LINES          0x0001
#define GL_LINE_LOOP      0x0002
#define GL_LINE_STRIP     0x0003
#define GL_TRIANGLES      0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN   0x0006
#define GL_QUADS          0x0007
#define GL_QUAD_STRIP     0x0008
#define GL_POLYGON        0x0009

/* glClear */
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000

/* errors */
#define GL_NO_ERROR          0
#define GL_INVALID_ENUM      0x0500
#define GL_INVALID_VALUE     0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_STACK_OVERFLOW    0x0503
#define GL_STACK_UNDERFLOW   0x0504
#define GL_OUT_OF_MEMORY     0x0505

/* faces, winding, shading */
#define GL_FRONT          0x0404
#define GL_BACK           0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW             0x0900
#define GL_CCW            0x0901
#define GL_FLAT           0x1D00
#define GL_SMOOTH         0x1D01

/* depth functions */
#define GL_NEVER    0x0200
#define GL_LESS     0x0201
#define GL_EQUAL    0x0202
#define GL_LEQUAL   0x0203
#define GL_GREATER  0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL   0x0206
#define GL_ALWAYS   0x0207

/* glEnable capabilities */
#define GL_POINT_SMOOTH    0x0B10
#define GL_LINE_SMOOTH     0x0B20
#define GL_POLYGON_SMOOTH  0x0B41
#define GL_CULL_FACE       0x0B44
#define GL_LIGHTING        0x0B50
#define GL_COLOR_MATERIAL  0x0B57
#define GL_FOG             0x0B60
#define GL_DEPTH_TEST      0x0B71
#define GL_NORMALIZE       0x0BA1
#define GL_ALPHA_TEST      0x0BC0
#define GL_DITHER          0x0BD0
#define GL_BLEND           0x0BE2
#define GL_SCISSOR_TEST    0x0C11
#define GL_TEXTURE_2D      0x0DE1
#define GL_RESCALE_NORMAL  0x803A
#define GL_LIGHT0          0x4000
#define GL_LIGHT1          0x4001
#define GL_LIGHT2          0x4002
#define GL_LIGHT3          0x4003
#define GL_LIGHT4          0x4004
#define GL_LIGHT5          0x4005
#define GL_LIGHT6          0x4006
#define GL_LIGHT7          0x4007

/* lights and materials */
#define GL_AMBIENT               0x1200
#define GL_DIFFUSE               0x1201
#define GL_SPECULAR              0x1202
#define GL_POSITION              0x1203
#define GL_SPOT_DIRECTION        0x1204
#define GL_SPOT_EXPONENT         0x1205
#define GL_SPOT_CUTOFF           0x1206
#define GL_CONSTANT_ATTENUATION  0x1207
#define GL_LINEAR_ATTENUATION    0x1208
#define GL_QUADRATIC_ATTENUATION 0x1209
#define GL_EMISSION              0x1600
#define GL_SHININESS             0x1601
#define GL_AMBIENT_AND_DIFFUSE   0x1602
#define GL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE  0x0B52
#define GL_LIGHT_MODEL_AMBIENT   0x0B53

/* matrices */
#define GL_MATRIX_MODE       0x0BA0
#define GL_MODELVIEW         0x1700
#define GL_PROJECTION        0x1701
#define GL_TEXTURE           0x1702
#define GL_MODELVIEW_MATRIX  0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7

/* display lists */
#define GL_COMPILE             0x1300
#define GL_COMPILE_AND_EXECUTE 0x1301

/* glGet */
#define GL_CURRENT_COLOR    0x0B00
#define GL_CURRENT_NORMAL   0x0B02
#define GL_POINT_SIZE       0x0B11
#define GL_LINE_WIDTH       0x0B21
#define GL_SHADE_MODEL      0x0B54
#define GL_DEPTH_RANGE      0x0B70
#define GL_DEPTH_WRITEMASK  0x0B72
#define GL_DEPTH_CLEAR_VALUE 0x0B73
#define GL_DEPTH_FUNC       0x0B74
#define GL_VIEWPORT         0x0BA2
#define GL_SCISSOR_BOX      0x0C10
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_MAX_LIGHTS       0x0D31
#define GL_MAX_MODELVIEW_STACK_DEPTH  0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH 0x0D38
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_RED_BITS         0x0D52
#define GL_GREEN_BITS       0x0D53
#define GL_BLUE_BITS        0x0D54
#define GL_DEPTH_BITS       0x0D56

/* glGetString */
#define GL_VENDOR     0x1F00
#define GL_RENDERER   0x1F01
#define GL_VERSION    0x1F02
#define GL_EXTENSIONS 0x1F03

/* glHint */
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_POINT_SMOOTH_HINT           0x0C51
#define GL_LINE_SMOOTH_HINT            0x0C52
#define GL_POLYGON_SMOOTH_HINT         0x0C53
#define GL_FOG_HINT                    0x0C54
#define GL_DONT_CARE 0x1100
#define GL_FASTEST   0x1101
#define GL_NICEST    0x1102

/* state */
void glEnable(GLenum cap);
void glDisable(GLenum cap);
GLboolean glIsEnabled(GLenum cap);
GLenum glGetError(void);
const GLubyte *glGetString(GLenum name);
void glGetFloatv(GLenum pname, GLfloat *params);
void glGetDoublev(GLenum pname, GLdouble *params);
void glGetIntegerv(GLenum pname, GLint *params);
void glGetBooleanv(GLenum pname, GLboolean *params);
void glHint(GLenum target, GLenum mode);
void glFlush(void);
void glFinish(void);

/* buffers */
void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glClearDepth(GLclampd depth);
void glClear(GLbitfield mask);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glDepthRange(GLclampd near_val, GLclampd far_val);

/* rasterisation */
void glShadeModel(GLenum mode);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glPointSize(GLfloat size);
void glLineWidth(GLfloat width);

/* matrices */
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glLoadMatrixd(const GLdouble *m);
void glMultMatrixf(const GLfloat *m);
void glMultMatrixd(const GLdouble *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glTranslated(GLdouble x, GLdouble y, GLdouble z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glScaled(GLdouble x, GLdouble y, GLdouble z);
void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble near_val, GLdouble far_val);
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble near_val, GLdouble far_val);

/* vertices */
void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex2i(GLint x, GLint y);
void glVertex2d(GLdouble x, GLdouble y);
void glVertex2fv(const GLfloat *v);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3i(GLint x, GLint y, GLint z);
void glVertex3d(GLdouble x, GLdouble y, GLdouble z);
void glVertex3fv(const GLfloat *v);
void glVertex3dv(const GLdouble *v);
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glVertex4fv(const GLfloat *v);
void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);
void glNormal3d(GLdouble nx, GLdouble ny, GLdouble nz);
void glNormal3fv(const GLfloat *v);
void glColor3f(GLfloat red, GLfloat green, GLfloat blue);
void glColor3d(GLdouble red, GLdouble green, GLdouble blue);
void glColor3fv(const GLfloat *v);
void glColor3ub(GLubyte red, GLubyte green, GLubyte blue);
void glColor3ubv(const GLubyte *v);
void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glColor4fv(const GLfloat *v);
void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2);
void glRecti(GLint x1, GLint y1, GLint x2, GLint y2);

/* lighting */
void glLightf(GLenum light, GLenum pname, GLfloat param);
void glLighti(GLenum light, GLenum pname, GLint param);
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glLightModelf(GLenum pname, GLfloat param);
void glLightModeli(GLenum pname, GLint param);
void glLightModelfv(GLenum pname, const GLfloat *params);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glMateriali(GLenum face, GLenum pname, GLint param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glColorMaterial(GLenum face, GLenum mode);

/* display lists */
GLuint glGenLists(GLsizei range);
void glNewList(GLuint list, GLenum mode);
void glEndList(void);
void glCallList(GLuint list);
void glDeleteLists(GLuint list, GLsizei range);
GLboolean glIsList(GLuint list);

#endif
