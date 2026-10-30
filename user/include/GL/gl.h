/*
 * fluxOS OpenGL: a subset of OpenGL 1.1's fixed-function pipeline.
 *
 * Supported: immediate mode (glBegin/glEnd with every primitive type) and
 * vertex arrays (glDrawArrays, glDrawElements, glArrayElement,
 * glInterleavedArrays), the modelview, projection and texture matrix
 * stacks, per-vertex lighting with eight lights and materials
 * (GL_COLOR_MATERIAL too), flat and smooth shading, back-face culling,
 * polygon modes and polygon offset, the depth buffer, scissoring, the
 * alpha test, blending, color masks, display lists (glCallLists too),
 * glReadPixels, and 2D textures: texture objects, perspective-correct,
 * nearest or bilinear, repeating or clamped, in the modulate, replace,
 * decal and blend modes. Textures need not be a power of two in size, and
 * only level 0 is kept: the mipmap filters use it as their nearest or
 * linear counterparts. The color buffer has no alpha: GL_DST_ALPHA blends
 * as 1. Not supported: fog, stencil, other pixel operations (glDrawPixels,
 * glBitmap), edge flags, and selection.
 *
 * Create a context and see the picture with <GL/fluxgl.h>. Programs that
 * use GL must be built with the FPU allowed (the Makefile's
 * USER_FP_CFLAGS).
 */
#ifndef _GL_GL_H
#define _GL_GL_H

#ifdef __cplusplus
extern "C" {
#endif

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
#define GL_TEXTURE_1D      0x0DE0
#define GL_POLYGON_OFFSET_POINT 0x2A01
#define GL_POLYGON_OFFSET_LINE  0x2A02
#define GL_POLYGON_OFFSET_FILL  0x8037
#define GL_RESCALE_NORMAL  0x803A
#define GL_LIGHT0          0x4000
#define GL_LIGHT1          0x4001
#define GL_LIGHT2          0x4002
#define GL_LIGHT3          0x4003
#define GL_LIGHT4          0x4004
#define GL_LIGHT5          0x4005
#define GL_LIGHT6          0x4006
#define GL_LIGHT7          0x4007

/* blending */
#define GL_ZERO                0
#define GL_ONE                 1
#define GL_SRC_COLOR           0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA           0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA           0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR           0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE  0x0308

/* polygon modes */
#define GL_POINT 0x1B00
#define GL_LINE  0x1B01
#define GL_FILL  0x1B02

/* buffers */
#define GL_NONE        0
#define GL_FRONT_LEFT  0x0400
#define GL_FRONT_RIGHT 0x0401
#define GL_BACK_LEFT   0x0402
#define GL_BACK_RIGHT  0x0403
#define GL_LEFT        0x0406
#define GL_RIGHT       0x0407

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
#define GL_TEXTURE_MATRIX    0x0BA8

/* textures */
#define GL_TEXTURE_WIDTH        0x1000
#define GL_TEXTURE_HEIGHT       0x1001
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_NEAREST              0x2600
#define GL_LINEAR               0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST  0x2701
#define GL_NEAREST_MIPMAP_LINEAR  0x2702
#define GL_LINEAR_MIPMAP_LINEAR   0x2703
#define GL_CLAMP                0x2900
#define GL_REPEAT               0x2901
#define GL_CLAMP_TO_EDGE        0x812F
#define GL_TEXTURE_ENV          0x2300
#define GL_TEXTURE_ENV_MODE     0x2200
#define GL_TEXTURE_ENV_COLOR    0x2201
#define GL_MODULATE             0x2100
#define GL_DECAL                0x2101
#define GL_REPLACE              0x1E01
#define GL_TEXTURE_BINDING_2D   0x8069
#define GL_MAX_TEXTURE_SIZE     0x0D33
#define GL_MAX_TEXTURE_STACK_DEPTH 0x0D39
#define GL_CURRENT_TEXTURE_COORDS 0x0B03

/* pixel formats and types */
#define GL_BYTE                 0x1400
#define GL_UNSIGNED_BYTE        0x1401
#define GL_SHORT                0x1402
#define GL_UNSIGNED_SHORT       0x1403
#define GL_INT                  0x1404
#define GL_UNSIGNED_INT         0x1405
#define GL_FLOAT                0x1406
#define GL_2_BYTES              0x1407
#define GL_3_BYTES              0x1408
#define GL_4_BYTES              0x1409
#define GL_DOUBLE               0x140A
#define GL_DEPTH_COMPONENT      0x1902
#define GL_RED                  0x1903
#define GL_GREEN                0x1904
#define GL_BLUE                 0x1905
#define GL_ALPHA                0x1906
#define GL_RGB                  0x1907
#define GL_RGBA                 0x1908
#define GL_LUMINANCE            0x1909
#define GL_LUMINANCE_ALPHA      0x190A
#define GL_INTENSITY            0x8049
#define GL_RGB8                 0x8051
#define GL_RGBA8                0x8058
#define GL_BGR                  0x80E0
#define GL_BGRA                 0x80E1
#define GL_UNPACK_ROW_LENGTH    0x0CF2
#define GL_UNPACK_SKIP_ROWS     0x0CF3
#define GL_UNPACK_SKIP_PIXELS   0x0CF4
#define GL_UNPACK_ALIGNMENT     0x0CF5
#define GL_PACK_ROW_LENGTH      0x0D02
#define GL_PACK_SKIP_ROWS       0x0D03
#define GL_PACK_SKIP_PIXELS     0x0D04
#define GL_PACK_ALIGNMENT       0x0D05

/* vertex arrays */
#define GL_VERTEX_ARRAY                0x8074
#define GL_NORMAL_ARRAY                0x8075
#define GL_COLOR_ARRAY                 0x8076
#define GL_INDEX_ARRAY                 0x8077
#define GL_TEXTURE_COORD_ARRAY         0x8078
#define GL_EDGE_FLAG_ARRAY             0x8079
#define GL_VERTEX_ARRAY_SIZE           0x807A
#define GL_VERTEX_ARRAY_TYPE           0x807B
#define GL_VERTEX_ARRAY_STRIDE         0x807C
#define GL_NORMAL_ARRAY_TYPE           0x807E
#define GL_NORMAL_ARRAY_STRIDE         0x807F
#define GL_COLOR_ARRAY_SIZE            0x8081
#define GL_COLOR_ARRAY_TYPE            0x8082
#define GL_COLOR_ARRAY_STRIDE          0x8083
#define GL_TEXTURE_COORD_ARRAY_SIZE    0x8088
#define GL_TEXTURE_COORD_ARRAY_TYPE    0x8089
#define GL_TEXTURE_COORD_ARRAY_STRIDE  0x808A
#define GL_VERTEX_ARRAY_POINTER        0x808E
#define GL_NORMAL_ARRAY_POINTER        0x808F
#define GL_COLOR_ARRAY_POINTER         0x8090
#define GL_TEXTURE_COORD_ARRAY_POINTER 0x8092
#define GL_V2F             0x2A20
#define GL_V3F             0x2A21
#define GL_C4UB_V2F        0x2A22
#define GL_C4UB_V3F        0x2A23
#define GL_C3F_V3F         0x2A24
#define GL_N3F_V3F         0x2A25
#define GL_C4F_N3F_V3F     0x2A26
#define GL_T2F_V3F         0x2A27
#define GL_T4F_V4F         0x2A28
#define GL_T2F_C4UB_V3F    0x2A29
#define GL_T2F_C3F_V3F     0x2A2A
#define GL_T2F_N3F_V3F     0x2A2B
#define GL_T2F_C4F_N3F_V3F 0x2A2C
#define GL_T4F_C4F_N3F_V4F 0x2A2D

/* display lists */
#define GL_COMPILE             0x1300
#define GL_COMPILE_AND_EXECUTE 0x1301
#define GL_LIST_MODE           0x0B30
#define GL_MAX_LIST_NESTING    0x0B31
#define GL_LIST_BASE           0x0B32
#define GL_LIST_INDEX          0x0B33

/* glGet */
#define GL_CURRENT_COLOR    0x0B00
#define GL_CURRENT_NORMAL   0x0B02
#define GL_POINT_SIZE       0x0B11
#define GL_LINE_WIDTH       0x0B21
#define GL_POLYGON_MODE     0x0B40
#define GL_SHADE_MODEL      0x0B54
#define GL_DEPTH_RANGE      0x0B70
#define GL_DEPTH_WRITEMASK  0x0B72
#define GL_DEPTH_CLEAR_VALUE 0x0B73
#define GL_DEPTH_FUNC       0x0B74
#define GL_VIEWPORT         0x0BA2
#define GL_ALPHA_TEST_FUNC  0x0BC1
#define GL_ALPHA_TEST_REF   0x0BC2
#define GL_BLEND_DST        0x0BE0
#define GL_BLEND_SRC        0x0BE1
#define GL_DRAW_BUFFER      0x0C01
#define GL_READ_BUFFER      0x0C02
#define GL_SCISSOR_BOX      0x0C10
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK  0x0C23
#define GL_DOUBLEBUFFER     0x0C32
#define GL_POLYGON_OFFSET_UNITS  0x2A00
#define GL_POLYGON_OFFSET_FACTOR 0x8038
#define GL_MAX_LIGHTS       0x0D31
#define GL_MAX_MODELVIEW_STACK_DEPTH  0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH 0x0D38
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_RED_BITS         0x0D52
#define GL_GREEN_BITS       0x0D53
#define GL_BLUE_BITS        0x0D54
#define GL_ALPHA_BITS       0x0D55
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
void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void glDrawBuffer(GLenum mode);
void glReadBuffer(GLenum mode);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                  GLvoid *pixels);

/* fragments */
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glAlphaFunc(GLenum func, GLclampf ref);

/* rasterisation */
void glShadeModel(GLenum mode);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glPointSize(GLfloat size);
void glLineWidth(GLfloat width);
void glPolygonMode(GLenum face, GLenum mode);
void glPolygonOffset(GLfloat factor, GLfloat units);

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

/* vertex arrays */
void glEnableClientState(GLenum cap);
void glDisableClientState(GLenum cap);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer);
void glGetPointerv(GLenum pname, GLvoid **params);
void glArrayElement(GLint i);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

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

/* textures */
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glBindTexture(GLenum target, GLuint texture);
GLboolean glIsTexture(GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params);
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);
void glPixelStorei(GLenum pname, GLint param);
void glTexCoord1f(GLfloat s);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2d(GLdouble s, GLdouble t);
void glTexCoord2i(GLint s, GLint t);
void glTexCoord2fv(const GLfloat *v);
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r);
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q);
void glTexCoord4fv(const GLfloat *v);

/* display lists */
GLuint glGenLists(GLsizei range);
void glNewList(GLuint list, GLenum mode);
void glEndList(void);
void glCallList(GLuint list);
void glCallLists(GLsizei n, GLenum type, const GLvoid *lists);
void glListBase(GLuint base);
void glDeleteLists(GLuint list, GLsizei range);
GLboolean glIsList(GLuint list);

#ifdef __cplusplus
}
#endif

#endif
