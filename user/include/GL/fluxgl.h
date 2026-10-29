/*
 * fluxOS GL contexts, the counterpart of GLX or WGL. A context draws into
 * a picture of its own: on an ATI Radeon HD 4000 or a 3dfx Voodoo card in
 * video memory, with the card's 3D engine, on Haswell's Intel HD Graphics
 * with its 3D engine in memory the kernel lends it, and anywhere else in
 * ordinary memory, with the software renderer. fglSwapBuffers() finishes the frame
 * and copies the picture to a 0xRRGGBB pixel buffer, such as a desktop
 * window's.
 *
 * A new context takes the first of the Radeon, the Intel, the Voodoo and
 * the software renderer that works. FGL_NO_RADEON, FGL_NO_INTEL and
 * FGL_NO_TDFX skip one; FGL_SOFTWARE, or LIBGL_ALWAYS_SOFTWARE in the
 * environment, keeps the context off the hardware; FLUXGL_DRIVER=radeon,
 * intel, tdfx or soft in the
 * environment names the only renderer to try (software stays the last
 * resort).
 */
#ifndef _GL_FLUXGL_H
#define _GL_FLUXGL_H

#include <stdint.h>
#include <GL/gl.h>

typedef struct fgl_context FGLcontext;

#define FGL_SOFTWARE  0x1           /* never use the 3D hardware */
#define FGL_NO_RADEON 0x2           /* not an ATI Radeon's 3D engine */
#define FGL_NO_TDFX   0x4           /* not a 3dfx Voodoo's */
#define FGL_NO_INTEL  0x8           /* not Intel HD Graphics' */

/* fglGetDriver */
#define FGL_DRIVER_SOFTWARE 0
#define FGL_DRIVER_TDFX     1
#define FGL_DRIVER_RADEON   2
#define FGL_DRIVER_INTEL    3

/* A context with a width x height picture; NULL if out of memory. */
FGLcontext *fglCreateContext(int width, int height, unsigned int flags);
void fglDestroyContext(FGLcontext *ctx);
/* GL calls go to the current context (NULL: none; they are ignored). */
void fglMakeCurrent(FGLcontext *ctx);
FGLcontext *fglGetCurrentContext(void);
/* Give the picture a new size; its contents are undefined afterwards. */
int fglResize(FGLcontext *ctx, int width, int height);
/* Finish drawing and copy the picture to dst (stride in pixels). */
void fglSwapBuffers(FGLcontext *ctx, uint32_t *dst, int stride);
/* 1 if the context draws with 3D hardware. */
int fglIsAccelerated(FGLcontext *ctx);
/* Which renderer draws the context: FGL_DRIVER_*. */
int fglGetDriver(FGLcontext *ctx);

#endif
