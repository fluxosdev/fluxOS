/* DirectDraw's software surfaces: native guest pixels, copied palettes and
 * nearest-neighbor blits. Presentation shares GDI's visibility/clipping path. */
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "compat-com.h"
#include "graphics-memory.h"
#define DD_INVALID 0x80070057u
#define DD_UNSUPPORTED 0x80004001u
#define DD_BUSY (0x88760000u+430)
#define DD_NOTFOUND (0x88760000u+255)
#define DD_NOOBJECT (0x88760000u+130)
struct dd_object {
    struct com_object com;
    struct dd_object *device,*back,*palette,*clipper,*primary;
    HWND hwnd;
    int version,w,h,bpp,pitch,locked,owns_bits,mode_set;
    gp_t bits;
    HDC gdic;HBITMAP gdib;HGDIOBJ old_bitmap;
    uint32_t caps,coop,colors[256],palette_flags,key_low,key_high,key_flags,unique;
};
extern struct api_dll ddraw_dll;
static uint32_t dd_dispatch(int kind,int op,uint32_t *a);
#include "ddraw-vtables.inc"
#define NSPECS ((int)(sizeof(dd_specs)/sizeof(dd_specs[0])))
static gp_t iface(struct dd_object *o,int kind)
{return com_interface(&o->com,kind,dd_specs,NSPECS,dd_methods,&ddraw_dll);}
static struct dd_object *find(gp_t p,int category)
{for(struct com_object *o=com_objects;o;o=o->next)if(!category || o->category==category)for(int i=0;i<24;i++)if(o->interfaces[i]==p)return (struct dd_object *)o;return NULL;}
static void dd_destroy(struct com_object *base)
{
    struct dd_object *o=(struct dd_object *)base;
    if(o->back)com_release(&o->back->com);
    if(o->palette)com_release(&o->palette->com);
    if(o->clipper)com_release(&o->clipper->com);
    if(o->gdic){G(SelectObject,o->gdic,o->old_bitmap);G(DeleteObject,o->gdib);G(DeleteDC,o->gdic);}
    if(o->owns_bits)gfree(o->bits);
    if(o->device) {if(o->device->primary==o)o->device->primary=NULL;com_release(&o->device->com);}
    free(o);
}
static struct dd_object *new_object(int category)
{
    struct dd_object *o=calloc(1,sizeof(*o));if(!o)return NULL;
    com_init(&o->com,category,dd_destroy);o->unique=1;return o;
}
static int format_fill(gp_t ptr,int bpp)
{
    if(!vm_writable(ptr,32))return 0;
    uint32_t *p=GP(ptr);
    memset(p,0,32);
    p[0]=32;p[1]=0x40|(bpp==8?0x20:0);p[3]=bpp;
    if(bpp==16){p[4]=0xf800;p[5]=0x7e0;p[6]=0x1f;}
    if(bpp==24 || bpp==32){p[4]=0xff0000;p[5]=0xff00;p[6]=0xff;}
    return 1;
}
static int describe(struct dd_object *o,gp_t ptr)
{
    if(!vm_readable(ptr,4))return 0;
    uint32_t size=*(uint32_t *)GP(ptr);
    if((size!=108 && size!=124) || !vm_writable(ptr,size))return 0;
    uint32_t *p=GP(ptr);memset(p,0,size);p[0]=size;p[1]=1|2|4|8|0x1000|0x800;
    p[2]=o->h;p[3]=o->w;p[4]=o->pitch;p[9]=o->bits;p[26]=o->caps;
    if(o->back){p[1]|=0x20;p[5]=1;}
    format_fill(ptr+72,o->bpp);return 1;
}
static uint32_t pixel(struct dd_object *s,int x,int y)
{
    const uint8_t *p=(uint8_t *)GP(s->bits)+y*s->pitch+x*(s->bpp/8);uint32_t v=0;memcpy(&v,p,s->bpp/8);return v;
}
static uint32_t color(struct dd_object *s,uint32_t v)
{
    if(s->bpp==8){struct dd_object *p=s->palette?s->palette:s->device->primary?s->device->primary->palette:NULL;return p?p->colors[v&255]:((v&255)*0x10101);}
    if(s->bpp==16)return ((v>>11)*255/31<<16)|(((v>>5)&63)*255/63<<8)|((v&31)*255/31);
    return v&0xffffff;
}
static void put_pixel(struct dd_object *s,int x,int y,uint32_t v)
{memcpy((uint8_t *)GP(s->bits)+y*s->pitch+x*(s->bpp/8),&v,s->bpp/8);}
static uint32_t convert(struct dd_object *dst,struct dd_object *src,uint32_t v)
{
    if(dst->bpp==src->bpp)return v;
    v=color(src,v);
    if(dst->bpp==16)return ((v>>19)<<11)|(((v>>10)&63)<<5)|((v>>3)&31);
    if(dst->bpp==8)return 0;
    return v;
}
static void present(struct dd_object *s)
{
    if(!(s->caps&0x200) || !s->device)return;
    HWND hwnd=s->clipper && s->clipper->hwnd?s->clipper->hwnd:s->device->hwnd;
    if(!hwnd || !ui_win(hwnd))return;
    HDC dc=U(GetDC,hwnd);
    int w,h;
    if(!dc || gdi_dc_size(dc,&w,&h)<0 || w<1 || h<1) {if(dc)U(ReleaseDC,hwnd,dc);return;}
    uint32_t *pixels=win_graphics_malloc((size_t)w*h*4);if(!pixels){U(ReleaseDC,hwnd,dc);return;}
    POINT origin={0};
    if(!s->device->mode_set) {
        gp_t point=galloc(8);if(point){memset(GP(point),0,8);U(ClientToScreen,hwnd,point);origin=*(POINT *)GP(point);gfree(point);}
    }
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        int sx=s->device->mode_set?(int)((int64_t)x*s->w/w):origin.x+x;
        int sy=s->device->mode_set?(int)((int64_t)y*s->h/h):origin.y+y;
        pixels[y*w+x]=sx>=0 && sx<s->w && sy>=0 && sy<s->h?color(s,pixel(s,sx,sy)):0;
    }
    gdi_argb_blit(dc,0,0,w,h,pixels,w,1);win_graphics_free(pixels);U(ReleaseDC,hwnd,dc);gui_flush();
}
static int valid_rect(const RECT *r,struct dd_object *s)
{return r->left>=0 && r->top>=0 && r->right<=s->w && r->bottom<=s->h && r->left<r->right && r->top<r->bottom;}
static uint32_t blt(struct dd_object *dst,gp_t dr,gp_t source,gp_t sr,uint32_t flags,gp_t fx)
{
    struct dd_object *src=source?find(source,2):NULL;RECT d={0,0,dst->w,dst->h},s={0};
    if(dst->locked || (src && src->locked))return DD_BUSY;
    if(dr){if(!vm_readable(dr,16))return DD_INVALID;d=*(RECT *)GP(dr);}
    if(d.left>=d.right || d.top>=d.bottom || d.left<-(1<<24) || d.top<-(1<<24) || d.right>(1<<24) || d.bottom>(1<<24))return DD_INVALID;
    if(flags&~(0x400u|0x8000u|0x2000u|0x1000000u|0x8000000u))return DD_UNSUPPORTED;
    uint32_t fill=0;if(flags&0x400) {if(!vm_readable(fx,100))return DD_INVALID;fill=((uint32_t *)GP(fx))[20];}
    else {
        if(!src)return DD_INVALID;
        s=(RECT){0,0,src->w,src->h};
        if(sr){if(!vm_readable(sr,16))return DD_INVALID;s=*(RECT *)GP(sr);}if(!valid_rect(&s,src))return DD_INVALID;
        if(dst->bpp==8 && src->bpp!=8)return DD_UNSUPPORTED;
    }
    /* Snapshot the source for overlapping/scaled self-blits. */
    uint32_t *copy=NULL;int sw=s.right-s.left,sh=s.bottom-s.top;
    if(src==dst && !(flags&0x400)) {
        copy=win_graphics_malloc((size_t)sw*sh*4);if(!copy)return 0x8007000e;
        for(int y=0;y<sh;y++)for(int x=0;x<sw;x++)copy[y*sw+x]=pixel(src,s.left+x,s.top+y);
    }
    int top=d.top<0?0:d.top,left=d.left<0?0:d.left,bottom=d.bottom>dst->h?dst->h:d.bottom,right=d.right>dst->w?dst->w:d.right;
    for(int y=top;y<bottom;y++)for(int x=left;x<right;x++) {
        uint32_t v=fill;
        if(!(flags&0x400)) {
            int sx=(int)((int64_t)(x-d.left)*sw/(d.right-d.left)),sy=(int)((int64_t)(y-d.top)*sh/(d.bottom-d.top));
            v=copy?copy[sy*sw+sx]:pixel(src,s.left+sx,s.top+sy);
            if((flags&0x8000) && (src->key_flags&8) && v>=src->key_low && v<=src->key_high)continue;
            if(flags&0x2000){uint32_t old=pixel(dst,x,y);if(!(dst->key_flags&2) || old<dst->key_low || old>dst->key_high)continue;}
            v=convert(dst,src,v);
        }
        put_pixel(dst,x,y,v);
    }
    win_graphics_free(copy);dst->unique++;present(dst);return 0;
}
static struct dd_object *surface_new(struct dd_object *device,int w,int h,int bpp,uint32_t caps)
{
    if(w<1 || h<1 || w>8192 || h>8192 || (bpp!=8 && bpp!=16 && bpp!=24 && bpp!=32) || (uint64_t)w*h*4>64*1024*1024)return NULL;
    struct dd_object *s=new_object(2);if(!s)return NULL;s->w=w;s->h=h;s->bpp=bpp;s->pitch=((w*(bpp/8)+3)&~3);s->caps=caps|0x800;
    s->bits=galloc(s->pitch*h);if(!s->bits){com_release(&s->com);return NULL;}
    s->owns_bits=1;s->device=device;device->com.refs++;return s;
}
static uint32_t query(struct dd_object *o,gp_t iid,gp_t out)
{
    if(!vm_writable(out,4))return DD_INVALID;
    *(gp_t *)GP(out)=0;
    if(!vm_readable(iid,16))return DD_INVALID;
    int kind=0;
    if(!memcmp(GP(iid),unknown_iid,16))kind=o->com.category==1?1:o->com.category==2?11:o->com.category==3?20:21;
    else for(int i=0;i<NSPECS;i++)if(!memcmp(GP(iid),dd_specs[i].iid,16)) {int k=dd_specs[i].kind;if((o->com.category==1 && k<10)||(o->com.category==2 && k>=11 && k<=17)||(o->com.category==3 && k==20)||(o->com.category==4 && k==21))kind=k;}
    if(!kind)return 0x80004002;
    gp_t p=iface(o,kind);
    if(!p)return 0x8007000e;
    *(gp_t *)GP(out)=p;
    o->com.refs++;
    return 0;
}
static uint32_t dd_dispatch(int kind,int op,uint32_t *a)
{
    struct dd_object *o=(struct dd_object *)com_find(A(0),kind);if(!o)return DD_NOOBJECT;
    if(op==DDOP_QueryInterface)return query(o,A(1),A(2));
    if(op==DDOP_AddRef)return ++o->com.refs;
    if(op==DDOP_Release)return com_release(&o->com);
    if(op==DDOP_Initialize)return 0x88760005u;
    if(o->com.category==1)switch(op) {
    case DDOP_SetCooperativeLevel:if(!A(1) || !ui_win(A(1)))return DD_INVALID;o->hwnd=A(1);o->coop=A(2);return 0;
    case DDOP_SetDisplayMode:
        if(A(1)<1 || A(2)<1 || A(1)>8192 || A(2)>8192 || (A(3)!=8 && A(3)!=16 && A(3)!=24 && A(3)!=32))return DD_INVALID;
        if(o->primary)return DD_BUSY;
        o->mode_set=1;
        o->w=A(1);
        o->h=A(2);
        o->bpp=A(3);
        o->pitch=(o->w*(o->bpp/8)+3)&~3;
        return 0;
    case DDOP_RestoreDisplayMode:o->mode_set=0;o->w=gui_screen_w();o->h=gui_screen_h();o->bpp=32;return 0;
    case DDOP_Compact:case DDOP_TestCooperativeLevel:case DDOP_RestoreAllSurfaces:case DDOP_FlipToGDISurface:return 0;
    case DDOP_GetDisplayMode:return describe(o,A(1))?0:DD_INVALID;
    case DDOP_GetMonitorFrequency:case DDOP_GetScanLine:case DDOP_GetVerticalBlankStatus:
        if(!vm_writable(A(1),4))return DD_INVALID;
        *(uint32_t *)P(1)=op==DDOP_GetMonitorFrequency?60:0;
        return 0;
    case DDOP_WaitForVerticalBlank:gui_wait_ms(16);return 0;
    case DDOP_GetFourCCCodes:if(!vm_writable(A(1),4))return DD_INVALID;*(uint32_t *)P(1)=0;return 0;
    case DDOP_GetAvailableVidMem:if(!vm_writable(A(2),4)||!vm_writable(A(3),4))return DD_INVALID;*(uint32_t *)P(2)=64*1024*1024;*(uint32_t *)P(3)=32*1024*1024;return 0;
    case DDOP_GetCaps:
        for(int i=1;i<=2;i++)if(A(i)){if(!vm_readable(A(i),4))return DD_INVALID;uint32_t n=*(uint32_t *)P(i);if(n<4 || n>512 || !vm_writable(A(i),n))return DD_INVALID;memset((uint8_t *)P(i)+4,0,n-4);if(n>=68){uint32_t *p=P(i);p[1]=0x40|0x200|0x400|0x8000|0x400000|0x4000000|0x20000000;p[3]=1|0x200;p[4]=0x4000|0x10000;p[6]=4|0x40;p[15]=64*1024*1024;p[16]=32*1024*1024;}}
        return 0;
    case DDOP_CreateClipper: {
        if(!vm_writable(A(2),4) || A(3))return DD_INVALID;
        struct dd_object *c=new_object(4);
        if(!c)return 0x8007000e;
        *(gp_t *)P(2)=iface(c,21);return *(gp_t *)P(2)?0:0x8007000e;
    }
    case DDOP_CreatePalette: {
        if(!vm_writable(A(3),4)||A(4)|| !(A(1)&4) || !vm_readable(A(2),1024))return DD_INVALID;
        struct dd_object *p=new_object(3);if(!p)return 0x8007000e;p->palette_flags=A(1);
        for(int i=0;i<256;i++){uint8_t *c=(uint8_t *)P(2)+i*4;p->colors[i]=(c[0]<<16)|(c[1]<<8)|c[2];}
        *(gp_t *)P(3)=iface(p,20);return *(gp_t *)P(3)?0:0x8007000e;
    }
    case DDOP_CreateSurface: {
        if(!vm_readable(A(1),108)||!vm_writable(A(2),4)||A(3))return DD_INVALID;
        *(gp_t *)P(2)=0;
        uint32_t *d=P(1);
        if(d[0]!=108 && d[0]!=124)return DD_INVALID;
        uint32_t caps=d[26];int primary=(caps&0x200)!=0,w=primary?o->w:(int)d[3],h=primary?o->h:(int)d[2],bpp=(d[1]&0x1000)?(int)d[21]:o->bpp;
        if(primary && o->primary)return DD_BUSY;
        if((d[1]&0x20) && d[5]>1)return DD_UNSUPPORTED;
        if((d[1]&0x800) || ((caps&(0x10|8)) && !(caps&0x200)))return DD_UNSUPPORTED;
        struct dd_object *s=surface_new(o,w,h,bpp,caps);if(!s)return 0x8007000e;
        if(primary)o->primary=s;
        if((d[1]&0x20) && d[5]){s->back=surface_new(o,w,h,bpp,4);if(!s->back){com_release(&s->com);return 0x8007000e;}}
        *(gp_t *)P(2)=iface(s,kind==7?17:kind==4?14:11);if(!*(gp_t *)P(2)){com_release(&s->com);return 0x8007000e;}return 0;
    }
    case DDOP_GetGDISurface:if(!o->primary)return DD_NOTFOUND;if(!vm_writable(A(1),4))return DD_INVALID;*(gp_t *)P(1)=iface(o->primary,kind==7?17:kind==4?14:11);o->primary->com.refs++;return 0;
    case DDOP_EnumDisplayModes: {
        if(!vm_readable(A(4),1))return DD_INVALID;
        gp_t desc=galloc(124);if(!desc)return 0x8007000e;struct dd_object mode=*o;
        const int sizes[][2]={{640,480},{800,600},{1024,768}};
        for(int b=8;b<=32;b*=2)for(int i=0;i<3;i++) {mode.w=sizes[i][0];mode.h=sizes[i][1];mode.bpp=b;mode.pitch=mode.w*(b/8);*(uint32_t *)GP(desc)=kind>=4?124:108;describe(&mode,desc);if(!call_guest2(A(4),desc,A(3)))goto modes_done;}
    modes_done:gfree(desc);return 0;
    }
    default:return DD_UNSUPPORTED;
    }
    if(o->com.category==2)switch(op) {
    case DDOP_GetDC: {
        if(o->locked)return DD_BUSY;
        if(!vm_writable(A(1),4))return DD_INVALID;
        HDC dc=G(CreateCompatibleDC,0);HBITMAP bm=G(CreateCompatibleBitmap,0,o->w,o->h);
        if(!dc || !bm){if(dc)G(DeleteDC,dc);if(bm)G(DeleteObject,bm);return 0x8007000e;}
        o->old_bitmap=G(SelectObject,dc,bm);o->gdic=dc;o->gdib=bm;
        uint32_t *pixels=win_graphics_malloc((size_t)o->w*o->h*4);if(!pixels){G(SelectObject,o->gdic,o->old_bitmap);G(DeleteObject,o->gdib);G(DeleteDC,o->gdic);o->gdic=0;o->gdib=0;return 0x8007000e;}
        for(int y=0;y<o->h;y++)for(int x=0;x<o->w;x++)pixels[y*o->w+x]=color(o,pixel(o,x,y));
        gdi_argb_blit(dc,0,0,o->w,o->h,pixels,o->w,1);win_graphics_free(pixels);o->locked=2;*(HDC *)P(1)=dc;return 0;
    }
    case DDOP_ReleaseDC: {
        if(o->locked!=2 || A(1)!=o->gdic)return DD_INVALID;
        uint32_t *pixels=win_graphics_malloc((size_t)o->w*o->h*4);if(!pixels)return 0x8007000e;
        if(gdi_dc_copy(o->gdic,pixels,o->w)<0){win_graphics_free(pixels);return DD_INVALID;}
        for(int y=0;y<o->h;y++)for(int x=0;x<o->w;x++) {
            uint32_t c=pixels[y*o->w+x]&0xffffff,v=c;
            if(o->bpp==16)v=((c>>19)<<11)|(((c>>10)&63)<<5)|((c>>3)&31);
            if(o->bpp==8) {
                struct dd_object *pal=o->palette?o->palette:o->device->primary?o->device->primary->palette:NULL;int best=0x7fffffff;v=0;
                for(int i=0;i<256;i++){uint32_t p=pal?pal->colors[i]:(uint32_t)i*0x10101;int r=(int)(c>>16)-(int)(p>>16),g=(int)((c>>8)&255)-(int)((p>>8)&255),b=(int)(c&255)-(int)(p&255);int score=r*r+g*g+b*b;if(score<best){best=score;v=i;}if(!score)break;}
            }
            put_pixel(o,x,y,v);
        }
        win_graphics_free(pixels);G(SelectObject,o->gdic,o->old_bitmap);G(DeleteObject,o->gdib);G(DeleteDC,o->gdic);o->gdic=0;o->gdib=0;o->locked=0;o->unique++;present(o);return 0;
    }
    case DDOP_GetSurfaceDesc:return describe(o,A(1))?0:DD_INVALID;
    case DDOP_GetPixelFormat:return format_fill(A(1),o->bpp)?0:DD_INVALID;
    case DDOP_GetCaps:if(!vm_writable(A(1),kind>=14?16:4))return DD_INVALID;memset(P(1),0,kind>=14?16:4);*(uint32_t *)P(1)=o->caps;return 0;
    case DDOP_IsLost:case DDOP_Restore:case DDOP_PageLock:case DDOP_PageUnlock:return 0;
    case DDOP_GetBltStatus:case DDOP_GetFlipStatus:return o->locked?DD_BUSY:0;
    case DDOP_Lock: {
        if(o->locked)return DD_BUSY;
        RECT r={0,0,o->w,o->h};
        if(A(1)){if(!vm_readable(A(1),16))return DD_INVALID;
        r=*(RECT *)P(1);
        }
        if(!valid_rect(&r,o)||!describe(o,A(2)))return DD_INVALID;
        ((uint32_t *)P(2))[9]=o->bits+r.top*o->pitch+r.left*(o->bpp/8);o->locked=1;return 0;
    }
    case DDOP_Unlock:if(o->locked!=1)return DD_INVALID;o->locked=0;o->unique++;present(o);return 0;
    case DDOP_Blt:return blt(o,A(1),A(2),A(3),A(4),A(5));
    case DDOP_BltFast: {
        struct dd_object *src=find(A(3),2);if(!src)return DD_INVALID;RECT r={0,0,src->w,src->h};if(A(4)){if(!vm_readable(A(4),16))return DD_INVALID;r=*(RECT *)P(4);}
        if(A(5)&~0x33u)return DD_UNSUPPORTED;
        gp_t rect=galloc(16);if(!rect)return 0x8007000e;*(RECT *)GP(rect)=(RECT){A(1),A(2),A(1)+r.right-r.left,A(2)+r.bottom-r.top};
        uint32_t result=blt(o,rect,A(3),A(4),(A(5)&1?0x8000:0)|(A(5)&2?0x2000:0),0);gfree(rect);return result;
    }
    case DDOP_GetAttachedSurface:if(!o->back)return DD_NOTFOUND;if(!vm_writable(A(2),4))return DD_INVALID;*(gp_t *)P(2)=iface(o->back,kind);o->back->com.refs++;return 0;
    case DDOP_Flip: {
        struct dd_object *back=A(1)?find(A(1),2):o->back;if(!back || back!=o->back)return DD_NOTFOUND;
        if(o->locked||back->locked)return DD_BUSY;
        gp_t bits=o->bits;
        o->bits=back->bits;
        back->bits=bits;
        present(o);
        return 0;
    }
    case DDOP_SetPalette:case DDOP_SetClipper: {
        struct dd_object *n=A(1)?find(A(1),op==DDOP_SetPalette?3:4):NULL;if(A(1)&&!n)return DD_INVALID;
        struct dd_object **slot=op==DDOP_SetPalette?&o->palette:&o->clipper;if(n)n->com.refs++;if(*slot)com_release(&(*slot)->com);*slot=n;present(o);return 0;
    }
    case DDOP_GetPalette:case DDOP_GetClipper: {
        struct dd_object *n=op==DDOP_GetPalette?o->palette:o->clipper;if(!n)return DD_NOTFOUND;if(!vm_writable(A(1),4))return DD_INVALID;
        *(gp_t *)P(1)=iface(n,op==DDOP_GetPalette?20:21);n->com.refs++;return 0;
    }
    case DDOP_SetColorKey:if((A(1)&~1u)!=2 && (A(1)&~1u)!=8)return DD_UNSUPPORTED;if(!A(2)){o->key_flags&=~A(1);return 0;}if(!vm_readable(A(2),8))return DD_INVALID;o->key_low=((uint32_t *)P(2))[0];o->key_high=((uint32_t *)P(2))[1];o->key_flags|=A(1);return 0;
    case DDOP_GetColorKey:if(!(o->key_flags&A(1)))return DD_NOTFOUND;if(!vm_writable(A(2),8))return DD_INVALID;((uint32_t *)P(2))[0]=o->key_low;((uint32_t *)P(2))[1]=o->key_high;return 0;
    case DDOP_GetDDInterface:if(!vm_writable(A(1),4))return DD_INVALID;*(gp_t *)P(1)=iface(o->device,kind>=14?7:1);o->device->com.refs++;return 0;
    case DDOP_GetUniquenessValue:if(!vm_writable(A(1),4))return DD_INVALID;*(uint32_t *)P(1)=o->unique;return 0;
    case DDOP_ChangeUniquenessValue:o->unique++;return 0;
    default:return DD_UNSUPPORTED;
    }
    if(o->com.category==3)switch(op) {
    case DDOP_GetCaps:if(!vm_writable(A(1),4))return DD_INVALID;*(uint32_t *)P(1)=o->palette_flags;return 0;
    case DDOP_GetEntries:case DDOP_SetEntries:
        if(A(2)>256 || A(3)>256-A(2) || !(op==DDOP_GetEntries?vm_writable(A(4),A(3)*4):vm_readable(A(4),A(3)*4)))return DD_INVALID;
        for(uint32_t i=0;i<A(3);i++){uint8_t *p=(uint8_t *)P(4)+i*4;uint32_t *c=o->colors+A(2)+i;if(op==DDOP_SetEntries)*c=(p[0]<<16)|(p[1]<<8)|p[2];else{p[0]=*c>>16;p[1]=*c>>8;p[2]=*c;p[3]=0;}}
        if(op==DDOP_SetEntries)for(struct com_object *s=com_objects;s;s=s->next)if(s->category==2 && ((struct dd_object *)s)->palette==o)present((struct dd_object *)s);
        return 0;
    default:return DD_UNSUPPORTED;
    }
    if(o->com.category==4)switch(op) {
    case DDOP_SetHWnd:if(A(1) || (A(2)&&!ui_win(A(2))))return DD_INVALID;o->hwnd=A(2);return 0;
    case DDOP_GetHWnd:if(!vm_writable(A(1),4))return DD_INVALID;*(HWND *)P(1)=o->hwnd;return 0;
    case DDOP_IsClipListChanged:if(!vm_writable(A(1),4))return DD_INVALID;*(uint32_t *)P(1)=0;return 0;
    default:return DD_UNSUPPORTED;
    }
    return DD_UNSUPPORTED;
}
XDEF(DirectDrawCreate)
{
    if(!vm_writable(A(1),4))return DD_INVALID;
    *(gp_t *)P(1)=0;
    if(A(2))return 0x80040110;
    struct dd_object *o=new_object(1);if(!o)return 0x8007000e;o->w=gui_screen_w();o->h=gui_screen_h();o->bpp=32;o->pitch=o->w*4;
    *(gp_t *)P(1)=iface(o,1);if(!*(gp_t *)P(1)){com_release(&o->com);return 0x8007000e;}return 0;
}
XDEF(DirectDrawCreateEx)
{
    if(A(3))return 0x80040110;
    uint32_t args[]={A(0),A(1),0};
    uint32_t result=api_DirectDrawCreate(args);
    if(result)return result;
    struct dd_object *o=find(*(gp_t *)P(1),1);result=query(o,A(2),A(1));com_release(&o->com);return result;
}
XDEF(DirectDrawCreateClipper)
{
    if(A(2)||!vm_writable(A(1),4))return DD_INVALID;
    struct dd_object *c=new_object(4);
    if(!c)return 0x8007000e;
    *(gp_t *)P(1)=iface(c,21);
    return *(gp_t *)P(1)?0:0x8007000e;
}
static uint32_t enumerate(uint32_t *a,int wide,int ex)
{
    if(!vm_readable(A(0),1))return DD_INVALID;
    gp_t name=galloc(128);
    if(!name)return 0x8007000e;
    if(wide){ansi_to_wide("fluxOS software DirectDraw",-1,GP(name),32);ansi_to_wide("display",-1,GP(name+64),32);}else{strcpy(GP(name),"fluxOS software DirectDraw");strcpy(GP(name+64),"display");}
    uint32_t args[]={0,name,name+64,A(1),0};call_guest(A(0),ex?5:4,args);gfree(name);return 0;
}
DEF(DirectDrawEnumerateA) {return enumerate(a,0,0);}DEF(DirectDrawEnumerateW) {return enumerate(a,1,0);}
DEF(DirectDrawEnumerateExA) {return enumerate(a,0,1);}DEF(DirectDrawEnumerateExW) {return enumerate(a,1,1);}
static const struct api_entry exports[]={API(DirectDrawCreate,3),API(DirectDrawCreateEx,4),API(DirectDrawCreateClipper,3),API(DirectDrawEnumerateA,2),API(DirectDrawEnumerateW,2),API(DirectDrawEnumerateExA,3),API(DirectDrawEnumerateExW,3)};
struct api_dll ddraw_dll={.name="DDRAW.DLL",.entries=exports,.count=sizeof(exports)/sizeof(exports[0]),.tables={dd_methods},.counts={sizeof(dd_methods)/sizeof(dd_methods[0])}};
