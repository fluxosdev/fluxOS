/*
 * The GL library on the host: its state, vertex pipeline and software
 * backend, drawing into a 64 x 64 picture that is read back pixel by
 * pixel: make test-gl. A pretend Voodoo (the tdfx backend, below, which
 * fills each polygon's bounding box) stands in for the 3D hardware, so
 * that moving a context to software can be checked too.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/glu.h>
#include "../user/gl/glint.h"

static int failures, checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("FAIL %s:%d: ", __func__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

void memset32(void *dst, unsigned v, size_t n)
{
    unsigned *p = dst;
    while (n--)
        *p++ = v;
}

/* ---- the pretend hardware ---- */

static int fake_hw;                     /* gl_tdfx_attach takes contexts */

static int fake_resize(struct fgl_context *c)
{
    free(c->bpriv);
    c->bpriv = calloc((size_t)c->width * (size_t)c->height, 4);
    return c->bpriv ? 0 : -1;
}

static void fake_destroy(struct fgl_context *c)
{
    free(c->bpriv);
    c->bpriv = NULL;
}

static uint32_t rgb(float r, float g, float b)
{
    return (uint32_t)gl_iround(r * 255) << 16 | (uint32_t)gl_iround(g * 255) << 8 | (uint32_t)gl_iround(b * 255);
}

static void fill(struct fgl_context *c, int x0, int y0, int x1, int y1, uint32_t px)
{
    uint32_t *p = c->bpriv;
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < c->height; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < c->width; x++)
            p[(size_t)y * c->width + x] = px;
}

static void fake_clear(struct fgl_context *c, GLbitfield mask, int x0, int y0, int x1, int y1)
{
    if (mask & GL_COLOR_BUFFER_BIT)
        fill(c, x0, y0, x1, y1, rgb(c->clear_color[0], c->clear_color[1], c->clear_color[2]));
}

static void fake_polygon(struct fgl_context *c, const struct wvert *v, int n)
{
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int i = 1; i < n; i++) {
        x0 = fminf(x0, v[i].x);
        x1 = fmaxf(x1, v[i].x);
        y0 = fminf(y0, v[i].y);
        y1 = fmaxf(y1, v[i].y);
    }
    fill(c, (int)ceilf(x0 - 0.5f), (int)ceilf(y0 - 0.5f), (int)ceilf(x1 - 0.5f), (int)ceilf(y1 - 0.5f),
         rgb(v[0].r, v[0].g, v[0].b));
}

static void fake_finish(struct fgl_context *c)
{
    (void)c;
}

static void fake_read(struct fgl_context *c, uint32_t *dst, int stride)
{
    for (int y = 0; y < c->height; y++)
        memcpy(dst + (size_t)y * stride, (uint32_t *)c->bpriv + (size_t)y * c->width, (size_t)c->width * 4);
}

/* Every depth reads as 0.25. */
static void fake_read_depth(struct fgl_context *c, int x, int y, int w, int h, float *dst)
{
    for (int i = 0; i < w * h; i++)
        dst[i] = 0.25f;
}

const struct backend gl_tdfx_backend = {
    .name = "pretend Voodoo",
    .resize = fake_resize,
    .destroy = fake_destroy,
    .clear = fake_clear,
    .polygon = fake_polygon,
    .finish = fake_finish,
    .read = fake_read,
    .read_depth = fake_read_depth,
};
const struct backend gl_radeon_backend = { .name = "none" };
const struct backend gl_intel_backend = { .name = "none" };

int gl_tdfx_attach(struct fgl_context *c)
{
    if (!fake_hw)
        return -1;
    c->be = &gl_tdfx_backend;
    strcpy(c->renderer, gl_tdfx_backend.name);
    return fake_resize(c);
}

int gl_tdfx_check(struct fgl_context *c) { return 0; }
int gl_tdfx_can_draw(struct fgl_context *c) { return !gl_active_texture(c); }
int gl_radeon_attach(struct fgl_context *c) { return -1; }
int gl_radeon_check(struct fgl_context *c) { return 0; }
int gl_intel_attach(struct fgl_context *c) { return -1; }
int gl_intel_check(struct fgl_context *c) { return 0; }

/* ---- helpers ---- */

#define W 64
#define H 64

static FGLcontext *ctx;

/* A fresh context, with window coordinates as vertex coordinates (z from 0 to -20 in the eye). */
static void begin_test(void)
{
    ctx = fglCreateContext(W, H, fake_hw ? 0 : FGL_SOFTWARE);
    fglMakeCurrent(ctx);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, 0, 20);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static void end_test(void)
{
    GLenum e = glGetError();
    CHECK(e == GL_NO_ERROR, "GL error 0x%x left over", e);
    fglDestroyContext(ctx);
    ctx = NULL;
}

static uint32_t pixel(int x, int y)
{
    uint8_t p[3];
    glReadPixels(x, y, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static int near(uint32_t a, uint32_t b, int tol)
{
    for (int sh = 0; sh < 24; sh += 8) {
        int d = (int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF);
        if (d < -tol || d > tol)
            return 0;
    }
    return 1;
}

#define CHECK_PIXEL(x, y, want, tol) \
    CHECK(near(pixel(x, y), (want), (tol)), "pixel (%d, %d) is %06x, not %06x", (x), (y), pixel(x, y), (want))

#define RED   0xFF0000u
#define GREEN 0x00FF00u
#define BLUE  0x0000FFu
#define WHITE 0xFFFFFFu
#define BLACK 0x000000u

static void quad(float x0, float y0, float x1, float y1, float z)
{
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex3f(x0, y0, z);
    glTexCoord2f(1, 0);
    glVertex3f(x1, y0, z);
    glTexCoord2f(1, 1);
    glVertex3f(x1, y1, z);
    glTexCoord2f(0, 1);
    glVertex3f(x0, y1, z);
    glEnd();
}

/* A 1 x 1 texture of one color, bound. */
static GLuint solid_texture(uint8_t r, uint8_t g, uint8_t b)
{
    GLuint t;
    uint8_t px[3] = { r, g, b };
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

/* ---- the tests ---- */

static void test_stencil(void)
{
    begin_test();
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(0, 0, 0, 0);
    glRectf(16, 16, 48, 48);
    glColorMask(1, 1, 1, 1);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColor3f(1, 0, 0);
    glRectf(0, 0, W, H);
    CHECK_PIXEL(32, 32, RED, 0);
    CHECK_PIXEL(4, 4, BLUE, 0);
    CHECK_PIXEL(50, 30, BLUE, 0);

    /* Counting: two increments over the left half make 2 there (3 where the square was). */
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    glColorMask(0, 0, 0, 0);
    glRectf(0, 0, 32, H);
    glRectf(0, 0, 32, H);
    glColorMask(1, 1, 1, 1);
    glStencilFunc(GL_EQUAL, 2, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColor3f(0, 1, 0);
    glRectf(0, 0, W, H);
    CHECK_PIXEL(4, 4, GREEN, 0);
    CHECK_PIXEL(20, 32, RED, 0);
    CHECK_PIXEL(40, 32, RED, 0);

    /* The depth test failing runs the zfail op; a masked clear leaves the other bits. */
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glColor3f(1, 1, 1);
    glDisable(GL_STENCIL_TEST);
    quad(0, 0, W, H, -1);                       /* depth near 0.05 everywhere */
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 5, 0xFF);
    glStencilOp(GL_KEEP, GL_REPLACE, GL_ZERO);
    glColor3f(1, 0, 0);
    quad(0, 0, 32, H, -10);                     /* behind: fails, stencil 5 */
    glStencilFunc(GL_EQUAL, 5, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDisable(GL_DEPTH_TEST);
    glColor3f(0, 0, 0);
    glRectf(0, 0, W, H);
    CHECK_PIXEL(10, 10, BLACK, 0);
    CHECK_PIXEL(50, 10, WHITE, 0);
    GLint v;
    glGetIntegerv(GL_STENCIL_BITS, &v);
    CHECK(v == 8, "stencil bits %d", v);
    glGetIntegerv(GL_STENCIL_REF, &v);
    CHECK(v == 5, "stencil ref %d", v);
    end_test();
}

static void test_fog(void)
{
    begin_test();
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 0);
    glFogf(GL_FOG_END, 10);
    GLfloat white[4] = { 1, 1, 1, 1 };
    glFogfv(GL_FOG_COLOR, white);
    glColor3f(0, 0, 0);
    quad(0, 0, 32, H, -5);                      /* halfway into the fog */
    CHECK_PIXEL(16, 32, 0x808080u, 2);
    quad(32, 0, W, H, -15);                     /* past its end */
    CHECK_PIXEL(48, 32, WHITE, 1);

    /* After a texture in GL_REPLACE fog is mixed in at each fragment. */
    solid_texture(0, 0, 0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor3f(1, 0, 0);
    quad(0, 0, 32, H, -2.5f);
    CHECK_PIXEL(16, 32, 0x404040u, 2);
    glDisable(GL_TEXTURE_2D);

    glFogi(GL_FOG_MODE, GL_EXP);
    glFogf(GL_FOG_DENSITY, 0.1f);
    glColor3f(0, 0, 0);
    quad(0, 0, W, H, -5);
    uint32_t want = (uint32_t)lround((1 - exp(-0.5)) * 255) * 0x010101u;
    CHECK_PIXEL(32, 32, want, 2);
    GLfloat d;
    glGetFloatv(GL_FOG_DENSITY, &d);
    CHECK(d == 0.1f, "density %g", d);
    end_test();
}

static void test_mipmaps(void)
{
    static const uint32_t colors[7] = { RED, GREEN, BLUE, WHITE, 0xFFFF00, 0x00FFFF, 0xFF00FF };
    begin_test();
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);      /* the 2 x 2 level's rows are 6 bytes */
    uint8_t *img = malloc(64 * 64 * 3);
    for (int level = 0, size = 64; size >= 1; level++, size /= 2) {
        for (int i = 0; i < size * size; i++) {
            img[i * 3] = (uint8_t)(colors[level] >> 16);
            img[i * 3 + 1] = (uint8_t)(colors[level] >> 8);
            img[i * 3 + 2] = (uint8_t)colors[level];
        }
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGB, size, size, 0, GL_RGB, GL_UNSIGNED_BYTE, img);
    }
    free(img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad(0, 0, 64, 64, -1);                     /* a texel a pixel: level 0 */
    CHECK_PIXEL(32, 32, RED, 0);
    quad(0, 0, 32, 32, -1);                     /* two: level 1 */
    CHECK_PIXEL(16, 16, GREEN, 0);
    quad(40, 40, 56, 56, -1);                   /* four: level 2 */
    CHECK_PIXEL(48, 48, BLUE, 0);
    quad(0, 48, 2, 50, -1);                     /* 32: level 5 */
    CHECK_PIXEL(1, 49, 0x00FFFF, 0);

    /* Trilinear halfway between levels 1 and 2 (three texels a pixel: log2 3 = 1.58). */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glClear(GL_COLOR_BUFFER_BIT);
    quad(0, 0, 64.0f / 3, 64.0f / 3, -1);
    uint32_t p = pixel(10, 10);
    CHECK((p & 0xFF) > 0x60 && (p & 0xFF) < 0xA0 && ((p >> 8) & 0xFF) > 0x60 && ((p >> 8) & 0xFF) < 0xA0 && !(p >> 16),
          "trilinear gave %06x", p);

    GLint v;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 3, GL_TEXTURE_WIDTH, &v);
    CHECK(v == 8, "level 3 width %d", v);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE, &v);
    CHECK(v == 8, "red size %d", v);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE, &v);
    CHECK(v == 0, "alpha size %d", v);

    /* gluBuild2DMipmaps: a 4 x 4 checkerboard comes down to grey. */
    uint8_t checker[16];
    for (int i = 0; i < 16; i++)
        checker[i] = ((i & 1) ^ ((i >> 2) & 1)) ? 255 : 0;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    CHECK(gluBuild2DMipmaps(GL_TEXTURE_2D, GL_LUMINANCE, 4, 4, GL_LUMINANCE, GL_UNSIGNED_BYTE, checker) == 0,
          "gluBuild2DMipmaps failed");
    uint8_t got[4] = { 0 };
    glGetTexImage(GL_TEXTURE_2D, 2, GL_LUMINANCE, GL_UNSIGNED_BYTE, got);
    CHECK(got[0] >= 126 && got[0] <= 129, "1 x 1 level is %d", got[0]);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &v);
    CHECK(v == 2, "level 1 width %d", v);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &v);
    CHECK(v == 1, "unpack alignment %d after gluBuild2DMipmaps", v);
    end_test();
}

static void test_multitexture(void)
{
    begin_test();
    CHECK(strstr((const char *)glGetString(GL_EXTENSIONS), "GL_ARB_multitexture") != NULL, "no GL_ARB_multitexture");
    GLint units;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &units);
    CHECK(units == 2, "%d units", units);
    solid_texture(255, 255, 0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glActiveTextureARB(GL_TEXTURE1_ARB);
    solid_texture(0, 255, 255);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glBegin(GL_QUADS);
    glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0.5f, 0.5f);
    glVertex2f(0, 0);
    glVertex2f(32, 0);
    glVertex2f(32, 64);
    glVertex2f(0, 64);
    glEnd();
    CHECK_PIXEL(16, 32, GREEN, 0);              /* yellow times cyan */

    /* From arrays, unit 1's through glClientActiveTexture. */
    static const float verts[] = { 32, 0, 64, 0, 64, 64, 32, 64 };
    static const float tcs[] = { 0, 0, 1, 0, 1, 1, 0, 1 };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glClientActiveTextureARB(GL_TEXTURE1_ARB);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, tcs);
    glActiveTextureARB(GL_TEXTURE0_ARB);
    glDisable(GL_TEXTURE_2D);
    glColor3f(1, 0, 1);
    glLockArraysEXT(0, 4);
    glDrawArrays(GL_QUADS, 0, 4);
    glUnlockArraysEXT();
    CHECK_PIXEL(48, 32, BLUE, 0);               /* magenta times cyan */

    /* GL 1.3's names, and by name. */
    glMultiTexCoord2f(GL_TEXTURE1, 0.25f, 0.75f);
    glActiveTexture(GL_TEXTURE1);
    GLfloat tc[4];
    glGetFloatv(GL_CURRENT_TEXTURE_COORDS, tc);
    CHECK(tc[0] == 0.25f && tc[1] == 0.75f, "unit 1 coordinates %g %g", tc[0], tc[1]);
    CHECK(fglGetProcAddress("glActiveTextureARB") == (void *)glActiveTextureARB, "glActiveTextureARB by name");
    CHECK(fglGetProcAddress("glMultiTexCoord2fv") == (void *)glMultiTexCoord2fv, "glMultiTexCoord2fv by name");
    CHECK(fglGetProcAddress("glNoSuchCall") == NULL, "a call that is not there");
    glActiveTexture(GL_TEXTURE0);
    end_test();
}

static void test_texgen(void)
{
    begin_test();
    GLuint t;
    uint8_t px[6] = { 0, 0, 0, 255, 255, 255 };
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    GLfloat splane[4] = { 1.0f / 64, 0, 0, 0 }, tplane[4] = { 0, 0, 0, 0.5f };
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    glTexGenfv(GL_S, GL_OBJECT_PLANE, splane);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    glTexGenfv(GL_T, GL_OBJECT_PLANE, tplane);
    glEnable(GL_TEXTURE_GEN_S);
    glEnable(GL_TEXTURE_GEN_T);
    glRectf(0, 0, W, H);
    CHECK_PIXEL(10, 32, BLACK, 0);
    CHECK_PIXEL(50, 32, WHITE, 0);

    /* Eye planes go through the inverse of the modelview when given: given
     * as s = x / 32 under a translation of 32, s = (x - 32) / 32 in the eye. */
    glPushMatrix();
    glTranslatef(32, 0, 0);
    GLfloat eplane[4] = { 1.0f / 32, 0, 0, 0 };
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
    glTexGenfv(GL_S, GL_EYE_PLANE, eplane);
    glPopMatrix();
    GLfloat got[4];
    glGetTexGenfv(GL_S, GL_EYE_PLANE, got);
    CHECK(fabsf(got[0] - 1.0f / 32) < 1e-6f && fabsf(got[3] + 1) < 1e-6f, "eye plane %g %g %g %g", got[0], got[1],
          got[2], got[3]);
    glRectf(0, 0, W, H);
    CHECK_PIXEL(20, 32, BLACK, 0);              /* s < 0: clamped to texel 0 (x / 32 would be white) */
    CHECK_PIXEL(40, 32, BLACK, 0);              /* s = 0.27 */
    CHECK_PIXEL(56, 32, WHITE, 0);              /* s = 0.77 */
    GLint mode;
    glGetTexGeniv(GL_S, GL_TEXTURE_GEN_MODE, &mode);
    CHECK(mode == GL_EYE_LINEAR, "mode %x", mode);
    end_test();
}

static void test_clip_planes(void)
{
    begin_test();
    glPushMatrix();
    glTranslatef(10, 0, 0);
    GLdouble eq[4] = { 1, 0, 0, -32 };          /* x >= 32 where it was given: 42 in the eye */
    glClipPlane(GL_CLIP_PLANE0, eq);
    glPopMatrix();
    glEnable(GL_CLIP_PLANE0);
    glColor3f(1, 0, 0);
    glRectf(0, 0, W, H);
    CHECK_PIXEL(40, 32, BLUE, 0);
    CHECK_PIXEL(44, 32, RED, 0);
    GLdouble got[4];
    glGetClipPlane(GL_CLIP_PLANE0, got);
    CHECK(got[0] == 1 && got[3] == -42, "plane %g %g %g %g", got[0], got[1], got[2], got[3]);

    /* Lines and points are clipped too. */
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(0, 1, 0);
    glBegin(GL_LINES);
    glVertex2f(0, 10.5f);
    glVertex2f(64, 10.5f);
    glEnd();
    glBegin(GL_POINTS);
    glVertex2f(20.5f, 30.5f);
    glVertex2f(50.5f, 30.5f);
    glEnd();
    CHECK_PIXEL(30, 10, BLUE, 0);
    CHECK_PIXEL(50, 10, GREEN, 0);
    CHECK_PIXEL(20, 30, BLUE, 0);
    CHECK_PIXEL(50, 30, GREEN, 0);
    CHECK(glIsEnabled(GL_CLIP_PLANE0), "plane 0 off");
    end_test();
}

static void test_bitmap(void)
{
    begin_test();
    glColor3f(0, 1, 0);
    glRasterPos2i(10, 20);
    glColor3f(1, 0, 0);                         /* too late: the raster color is green */
    static const GLubyte bits[2] = { 0xF0, 0x0F };
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBitmap(8, 2, 0, 0, 8, 0, bits);
    CHECK_PIXEL(10, 20, GREEN, 0);
    CHECK_PIXEL(13, 20, GREEN, 0);
    CHECK_PIXEL(14, 20, BLUE, 0);
    CHECK_PIXEL(14, 21, GREEN, 0);
    CHECK_PIXEL(13, 21, BLUE, 0);
    GLfloat pos[4];
    glGetFloatv(GL_CURRENT_RASTER_POSITION, pos);
    CHECK(pos[0] == 18 && pos[1] == 20, "raster position %g %g", pos[0], pos[1]);

    /* In a display list, drawn where the raster position is when it is called. */
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBitmap(8, 2, 0, 0, 0, 4, bits);
    glEndList();
    glRasterPos2i(30, 40);
    glCallList(list);
    CHECK_PIXEL(30, 40, RED, 0);
    CHECK_PIXEL(37, 41, RED, 0);
    glGetFloatv(GL_CURRENT_RASTER_POSITION, pos);
    CHECK(pos[0] == 30 && pos[1] == 44, "raster position %g %g after the list", pos[0], pos[1]);

    /* Outside the view volume the raster position is invalid, and nothing is drawn. */
    glRasterPos2i(-10, -10);
    GLboolean valid;
    glGetBooleanv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
    CHECK(!valid, "raster position valid outside");
    glBitmap(8, 2, 0, 0, 0, 0, bits);
    end_test();
}

static void test_pixels(void)
{
    begin_test();
    static const GLubyte img[12] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255 };
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glRasterPos2i(5, 5);
    glDrawPixels(2, 2, GL_RGB, GL_UNSIGNED_BYTE, img);
    CHECK_PIXEL(5, 5, RED, 0);
    CHECK_PIXEL(6, 5, GREEN, 0);
    CHECK_PIXEL(5, 6, BLUE, 0);
    CHECK_PIXEL(6, 6, WHITE, 0);
    CHECK_PIXEL(7, 5, BLUE, 0);

    glPixelZoom(2, 2);
    glRasterPos2i(20, 20);
    glDrawPixels(2, 2, GL_RGB, GL_UNSIGNED_BYTE, img);
    CHECK_PIXEL(21, 21, RED, 0);
    CHECK_PIXEL(22, 20, GREEN, 0);
    CHECK_PIXEL(20, 23, BLUE, 0);
    CHECK_PIXEL(23, 23, WHITE, 0);

    glPixelZoom(1, 1);
    glRasterPos2i(40, 40);
    glCopyPixels(5, 5, 2, 2, GL_COLOR);
    CHECK_PIXEL(40, 40, RED, 0);
    CHECK_PIXEL(41, 41, WHITE, 0);

    /* The fragment operations apply: the alpha test keeps alpha above a half. */
    static const GLubyte rgba[8] = { 255, 255, 0, 255, 255, 255, 0, 0 };
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glRasterPos2i(50, 5);
    glDrawPixels(2, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glDisable(GL_ALPHA_TEST);
    CHECK_PIXEL(50, 5, 0xFFFF00, 0);
    CHECK_PIXEL(51, 5, BLUE, 0);
    end_test();
}

static void test_copy_texture(void)
{
    begin_test();
    glColor3f(1, 0, 0);
    glRectf(0, 0, 8, 8);
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, 16, 16, 0);
    uint8_t texels[16 * 16 * 3];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, texels);
    CHECK(texels[(2 * 16 + 2) * 3] == 255 && texels[(2 * 16 + 2) * 3 + 2] == 0, "texel (2, 2) not red");
    CHECK(texels[(12 * 16 + 12) * 3] == 0 && texels[(12 * 16 + 12) * 3 + 2] == 255, "texel (12, 12) not blue");
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 8, 8, 0, 0, 4, 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, texels);
    CHECK(texels[(9 * 16 + 9) * 3] == 255, "texel (9, 9) not red after the sub-image");

    /* The texture drawn back over the whole picture. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad(0, 0, W, H, -1);
    CHECK_PIXEL(10, 10, RED, 0);                /* texel (2, 2) */
    CHECK_PIXEL(40, 10, BLUE, 0);               /* texel (10, 2) */
    CHECK_PIXEL(40, 40, RED, 0);                /* texel (10, 10), from the sub-image */
    end_test();
}

static void test_attrib_stacks(void)
{
    begin_test();
    glEnable(GL_FOG);
    glFogf(GL_FOG_DENSITY, 0.5f);
    glColor3f(0.25f, 0.5f, 0.75f);
    glPushAttrib(GL_FOG_BIT | GL_ENABLE_BIT | GL_CURRENT_BIT);
    glDisable(GL_FOG);
    glEnable(GL_LIGHTING);
    glFogf(GL_FOG_DENSITY, 2);
    glColor3f(1, 1, 1);
    glPopAttrib();
    GLfloat d, col[4];
    glGetFloatv(GL_FOG_DENSITY, &d);
    glGetFloatv(GL_CURRENT_COLOR, col);
    CHECK(glIsEnabled(GL_FOG) && !glIsEnabled(GL_LIGHTING) && d == 0.5f, "fog and enables not back");
    CHECK(col[0] == 0.25f && col[2] == 0.75f, "color not back: %g %g %g", col[0], col[1], col[2]);

    /* Groups not pushed stay as they are. */
    glPushAttrib(GL_LINE_BIT);
    glDepthFunc(GL_GREATER);
    glLineWidth(3);
    glPopAttrib();
    GLint v;
    GLfloat lw;
    glGetIntegerv(GL_DEPTH_FUNC, &v);
    glGetFloatv(GL_LINE_WIDTH, &lw);
    CHECK(v == GL_GREATER && lw == 1, "depth func %x, line width %g", v, lw);

    /* The texture group: the active unit, its binding, enables and the texture's parameters. */
    GLuint t = solid_texture(1, 2, 3);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glPushAttrib(GL_TEXTURE_BIT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glActiveTextureARB(GL_TEXTURE1_ARB);
    glBindTexture(GL_TEXTURE_2D, 0);
    glPopAttrib();
    glGetIntegerv(GL_ACTIVE_TEXTURE_ARB, &v);
    CHECK(v == GL_TEXTURE0_ARB, "active unit %x", v);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &v);
    CHECK(v == (GLint)t, "binding %d", v);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &v);
    CHECK(v == GL_CLAMP, "wrap %x", v);

    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &v);
    CHECK(v == 0, "stack depth %d", v);
    for (int i = 0; i < 16; i++)
        glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    CHECK(glGetError() == GL_STACK_OVERFLOW, "no overflow");
    for (int i = 0; i < 16; i++)
        glPopAttrib();
    glPopAttrib();
    CHECK(glGetError() == GL_STACK_UNDERFLOW, "no underflow");

    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glEnableClientState(GL_COLOR_ARRAY);
    glPopClientAttrib();
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &v);
    CHECK(v == 1, "unpack alignment %d", v);    /* as solid_texture left it */
    CHECK(glIsEnabled(GL_COLOR_ARRAY), "the color array is not in the pixel store group");
    glDisableClientState(GL_COLOR_ARRAY);
    end_test();
}

static void test_queries(void)
{
    begin_test();
    glLightf(GL_LIGHT1, GL_SPOT_CUTOFF, 45);
    GLfloat pos[4] = { 1, 2, 3, 1 }, f[4];
    glLightfv(GL_LIGHT1, GL_POSITION, pos);
    glGetLightfv(GL_LIGHT1, GL_SPOT_CUTOFF, f);
    CHECK(f[0] == 45, "cutoff %g", f[0]);
    glGetLightfv(GL_LIGHT1, GL_POSITION, f);
    CHECK(f[0] == 1 && f[1] == 2 && f[2] == 3 && f[3] == 1, "position %g %g %g %g", f[0], f[1], f[2], f[3]);
    GLint iv[4];
    glGetLightiv(GL_LIGHT0, GL_DIFFUSE, iv);
    CHECK(iv[0] == 2147483647, "diffuse as an int %d", iv[0]);
    glMaterialf(GL_FRONT, GL_SHININESS, 10);
    glGetMaterialfv(GL_FRONT, GL_SHININESS, f);
    CHECK(f[0] == 10, "shininess %g", f[0]);
    GLint half[4] = { 1073741823, 0, 0, 2147483647 };
    glMaterialiv(GL_FRONT, GL_AMBIENT, half);
    glGetMaterialfv(GL_BACK, GL_AMBIENT, f);
    CHECK(fabsf(f[0] - 0.5f) < 1e-3f && f[3] == 1, "ambient from ints %g %g", f[0], f[3]);
    solid_texture(0, 0, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, iv);
    CHECK(iv[0] == GL_LINEAR, "min filter %x", iv[0]);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, iv);
    CHECK(iv[0] == GL_DECAL, "env mode %x", iv[0]);
    glGetIntegerv(GL_FRONT_FACE, iv);
    CHECK(iv[0] == GL_CCW, "front face %x", iv[0]);
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, iv);
    CHECK(iv[0] == 1, "modelview depth %d", iv[0]);
    glHint(GL_FOG_HINT, GL_NICEST);
    glGetIntegerv(GL_FOG_HINT, iv);
    CHECK(iv[0] == GL_NICEST, "fog hint %x", iv[0]);
    glDisable(GL_LINE_STIPPLE);                 /* lacking, but off is fine */
    CHECK(!glIsEnabled(GL_LINE_STIPPLE), "line stipple on");

    /* The other forms of the vertex calls. */
    glColor3s(32767, 0, -32768);
    glGetFloatv(GL_CURRENT_COLOR, f);
    CHECK(f[0] == 1 && f[2] == 0, "color from shorts %g %g", f[0], f[2]);
    glNormal3b(127, 0, -128);
    glGetFloatv(GL_CURRENT_NORMAL, f);
    CHECK(f[0] == 1 && f[2] == -1, "normal from bytes %g %g", f[0], f[2]);
    static const GLubyte c4[4] = { 0, 255, 0, 255 };
    static const GLint a[2] = { 0, 0 }, b[2] = { 8, 8 };
    glColor4ubv(c4);
    glRectiv(a, b);
    CHECK_PIXEL(4, 4, GREEN, 0);
    end_test();
}

/* A context on the pretend Voodoo stays there for what it can draw (fog
 * folded into untextured colors among it), and moves to software with
 * its picture and depths for the stencil test. */
static void test_fallback(void)
{
    fake_hw = 1;
    begin_test();
    CHECK(fglGetDriver(ctx) == FGL_DRIVER_TDFX, "not on the pretend Voodoo");
    CHECK(strstr((const char *)glGetString(GL_EXTENSIONS), "GL_ARB_multitexture") == NULL,
          "GL_ARB_multitexture on the hardware");
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_END, 10);
    GLfloat white[4] = { 1, 1, 1, 1 };
    glFogfv(GL_FOG_COLOR, white);
    glColor3f(0, 0, 0);
    quad(0, 0, W, H, -5);
    CHECK(fglGetDriver(ctx) == FGL_DRIVER_TDFX, "fog moved it to software");
    CHECK_PIXEL(32, 32, 0x808080u, 2);
    glDisable(GL_FOG);

    glEnable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glColor3f(1, 0, 0);
    quad(0, 0, 32, H, -10);                     /* depth 0.5, behind the 0.25 carried over */
    CHECK(fglGetDriver(ctx) == FGL_DRIVER_SOFTWARE, "the stencil test stayed on the hardware");
    CHECK_PIXEL(16, 32, 0x808080u, 2);
    quad(32, 0, W, H, -1);                      /* depth 0.05: in front */
    CHECK_PIXEL(48, 32, RED, 0);
    CHECK(strstr((const char *)glGetString(GL_EXTENSIONS), "GL_ARB_multitexture") != NULL,
          "no GL_ARB_multitexture in software");
    end_test();
    fake_hw = 0;
}

int main(void)
{
    test_stencil();
    test_fog();
    test_mipmaps();
    test_multitexture();
    test_texgen();
    test_clip_planes();
    test_bitmap();
    test_pixels();
    test_copy_texture();
    test_attrib_stacks();
    test_queries();
    test_fallback();
    printf("gl: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
