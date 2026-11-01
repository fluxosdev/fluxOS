/* WGL over fluxOS GL. API calls are serialized by the Win32 runtime; every
 * wrapper restores this Windows thread's native GL context before drawing. */
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/fluxgl.h>
#include "ui.h"
#include "threads.h"
#include "graphics-memory.h"
struct wgl_context {
    struct wgl_context *next;
    uint32_t handle,owner;
    HDC dc;
    int w,h;
    FGLcontext *native;
    gp_t strings[4];
};
static struct wgl_context *contexts;
static uint32_t next_context=1;
extern struct api_dll opengl32_dll;
int gdi_pixel_format(HDC h,int set);
static struct wgl_context *context_find(uint32_t handle)
{for(struct wgl_context *c=contexts;c;c=c->next) if(c->handle==handle) return c;return NULL;}
static struct wgl_context *gl_current(void)
{
    for(struct wgl_context *c=contexts;c;c=c->next) if(c->owner==teb->thread_id) {
        int w,h;
        if(gdi_dc_size(c->dc,&w,&h)<0 || w<1 || h<1) break;
        if(c->w!=w || c->h!=h) {if(fglResize(c->native,w,h)<0) break;c->w=w;c->h=h;}
        fglMakeCurrent(c->native);return c;
    }
    fglMakeCurrent(NULL);return NULL;
}
void opengl_thread_exit(uint32_t tid)
{for(struct wgl_context *c=contexts;c;c=c->next) if(c->owner==tid) c->owner=0;}
XDEF(ChoosePixelFormat)
{
    if(!gdi_dc_valid(A(0)) || !A(1) || !vm_readable(A(1),40)) return 0;
    const uint8_t *p=P(1);uint16_t size,version;uint32_t flags;
    memcpy(&size,p,2);memcpy(&version,p+2,2);memcpy(&flags,p+4,4);
    return size>=40 && version==1 && p[8]==0 && !(flags&(0x10|0x80|0x100))?1:0;
}
XDEF(DescribePixelFormat)
{
    if(!gdi_dc_valid(A(0)) || A(1)!=1) return 0;
    if(A(3)) {
        if(A(2)<40 || !vm_writable(A(3),40)) return 0;
        uint8_t p[40]={0};uint16_t size=40,version=1;uint32_t flags=1|4|8|0x20|0x40;
        memcpy(p,&size,2);memcpy(p+2,&version,2);memcpy(p+4,&flags,4);
        p[9]=32;p[10]=8;p[11]=16;p[12]=8;p[13]=8;p[14]=8;p[23]=24;p[24]=8;
        memcpy(P(3),p,40);
    }
    return 1;
}
XDEF(SetPixelFormat)
{
    if(A(1)!=1 || !gdi_dc_valid(A(0))) return 0;
    return gdi_pixel_format(A(0),1)==1;
}
XDEF(GetPixelFormat) {return gdi_pixel_format(A(0),0);}
XDEF(SwapBuffers)
{
    struct wgl_context *c=gl_current();if(!c || c->dc!=A(0)) return 0;
    uint32_t *pixels=win_graphics_malloc((size_t)c->w*c->h*4);if(!pixels) return 0;
    fglSwapBuffers(c->native,pixels,c->w);
    int ok=gdi_argb_blit(c->dc,0,0,c->w,c->h,pixels,c->w,1)==0;
    win_graphics_free(pixels);gui_flush();return ok;
}
DEF(wglCreateContext)
{
    int w,h;if(!gdi_pixel_format(A(0),0) || gdi_dc_size(A(0),&w,&h)<0 || w<1 || h<1) return 0;
    struct wgl_context *c=calloc(1,sizeof(*c));if(!c) return 0;
    c->native=fglCreateContext(w,h,FGL_SOFTWARE);if(!c->native) {free(c);return 0;}
    c->w=w;c->h=h;c->dc=A(0);c->handle=next_context++;if(!c->handle)c->handle=next_context++;
    c->next=contexts;contexts=c;return c->handle;
}
DEF(wglCreateLayerContext) {return A(1)?0:api_wglCreateContext(a);}
DEF(wglMakeCurrent)
{
    struct wgl_context *c=A(1)?context_find(A(1)):NULL;
    if(A(1) && (!c || !gdi_pixel_format(A(0),0) || (c->owner && c->owner!=teb->thread_id))) return 0;
    for(struct wgl_context *old=contexts;old;old=old->next) if(old->owner==teb->thread_id) old->owner=0;
    if(c) {c->owner=teb->thread_id;c->dc=A(0);}
    return c?gl_current()!=NULL:(fglMakeCurrent(NULL),1);
}
DEF(wglGetCurrentContext) {(void)a;struct wgl_context *c=gl_current();return c?c->handle:0;}
DEF(wglGetCurrentDC) {(void)a;struct wgl_context *c=gl_current();return c?c->dc:0;}
DEF(wglDeleteContext)
{
    struct wgl_context *c=context_find(A(0));if(!c || (c->owner && c->owner!=teb->thread_id)) return 0;
    for(struct wgl_context **p=&contexts;*p;p=&(*p)->next) if(*p==c) {*p=c->next;break;}
    for(int i=0;i<4;i++) if(c->strings[i]) gfree(c->strings[i]);
    fglDestroyContext(c->native);free(c);return 1;
}
DEF(wglGetProcAddress)
{
    if(!A(0) || !gl_current()) return 0;
    const struct api_entry *e=api_find(&opengl32_dll,gstr(A(0)));
    return e?api_thunk(&opengl32_dll,e):0;
}
DEF(wglShareLists) {(void)a;set_last_error(ERROR_NOT_SUPPORTED);return 0;}
DEF(wglCopyContext) {(void)a;set_last_error(ERROR_NOT_SUPPORTED);return 0;}
static float arg_float(uint32_t v) {float f;memcpy(&f,&v,4);return f;}
static double arg_double(const uint32_t *p) {double d;memcpy(&d,p,8);return d;}
DEF(glGetString)
{
    struct wgl_context *c=gl_current();if(!c) return 0;
    const GLubyte *s=glGetString(A(0));if(!s) return 0;
    int i=A(0)==GL_VENDOR?0:A(0)==GL_RENDERER?1:A(0)==GL_VERSION?2:3;
    if(!c->strings[i]) c->strings[i]=gstrdup((const char *)s);
    return c->strings[i];
}
DEF(glGetPointerv)
{
    if(!gl_current() || !A(1)) return 0;
    void *p=NULL;glGetPointerv(A(0),&p);*(gp_t *)P(1)=p?GA(p):0;return 0;
}
#include "win32-gl.inc"
static const struct api_entry entries[]={
    API(wglCreateContext,1),API(wglCreateLayerContext,2),API(wglMakeCurrent,2),API(wglDeleteContext,1),
    API(wglGetCurrentContext,0),API(wglGetCurrentDC,0),API(wglGetProcAddress,1),API(wglShareLists,2),API(wglCopyContext,3),
    API(glGetString,1),API(glGetPointerv,2),
    WIN_GL_ENTRIES
};
struct api_dll opengl32_dll={.name="OPENGL32.DLL",.entries=entries,.count=sizeof(entries)/sizeof(entries[0])};
