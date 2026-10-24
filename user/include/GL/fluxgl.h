/*
 * fluxOS GL contexts, the counterpart of GLX or WGL. A context draws into
 * a picture of its own: on a 3dfx Voodoo card in video memory, with the
 * card's 3D engine, and anywhere else in ordinary memory, with the
 * software renderer. fglSwapBuffers() finishes the frame and copies the
 * picture to a 0xRRGGBB pixel buffer, such as a desktop window's.
 *
 * Setting LIBGL_ALWAYS_SOFTWARE in the environment, or passing
 * FGL_SOFTWARE, keeps a context off the hardware.
 */
#ifndef _GL_FLUXGL_H
#define _GL_FLUXGL_H

#include <stdint.h>
#include <GL/gl.h>

typedef struct fgl_context FGLcontext;

#define FGL_SOFTWARE 0x1            /* never use the 3D hardware */

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

#endif
