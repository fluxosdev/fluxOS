/* GDI+ 1.x image core, implemented over fluxOS surfaces and codecs.
 * ABI references: Microsoft GDI+ Flat API and Wine's gdiplus.spec/tests/image.c.
 * No Wine or ReactOS implementation code is incorporated. */
#include <math.h>
#include <errno.h>
#include "gdiplus.h"
#define OBJECTS 512
#define SESSION 1
#define BITMAP 2
#define GRAPHICS 3
#define BRUSH 4
struct graphics {
    uint32_t image;
    HDC dc;
    HWND owner;
    int own_dc,busy,composite,interpolation,unit;
    float scale,sx,sy,tx,ty;
};
struct object {
    uint32_t handle;
    int type;
    union {
        struct gp_bitmap bitmap;
        struct graphics graphics;
        uint32_t color;
    }
    u;
};
static struct object objects[OBJECTS];
static uint16_t generation[OBJECTS];
static int sessions;
static struct object *object(uint32_t h,int type)
{
    unsigned slot=h&511;
    struct object *o=&objects[slot];
    return h && o->handle==h && o->type==type ? o:NULL;
}
static struct object *new_object(int type)
{
    for(int i=0;i<OBJECTS;i++) if(!objects[i].handle) {
        struct object *o=&objects[i];
        memset(o,0,sizeof(*o));
        generation[i]=generation[i]%0x7FFF+1;
        o->handle=0x6D000000u|((uint32_t)generation[i]<<9)|i;
        o->type=type;
        return o;
    }
    return NULL;
}
static int output(gp_t p,uint32_t n) {
    return p && vm_writable(p,n);
}
static float real(uint32_t bits) {
    float f;
    memcpy(&f,&bits,4);
    return f;
}
static int finite_real(float f) {
    return isfinite(f) && fabsf(f)<100000000.f;
}
#define READY() do {if(!sessions) return GP_NOTINITIALIZED;} while(0)
struct gp_bitmap *gp_bitmap(uint32_t h) {
    struct object *o=object(h,BITMAP);
    return o ? &o->u.bitmap:NULL;
}
int gp_pixel_bytes(uint32_t format) {
    return format==GP_RGB24 ? 3:format==GP_RGB32 || format==GP_ARGB32 || format==GP_PARGB32 ? 4:0;
}
int gp_rows_valid(gp_t scan,int stride,int width,int height,int bytes,int writable)
{
    if(!scan || stride==INT32_MIN || width<=0 || height<=0 || !bytes || (int64_t)abs(stride)<(int64_t)width*bytes) return 0;
    int64_t low=scan,high=scan;
    if(stride<0) low+=(int64_t)(height-1)*stride;
    else high+=(int64_t)(height-1)*stride;
    high+=(int64_t)width*bytes;
    if(low<0 || high>UINT32_MAX || high-low>64*1024*1024) return 0;
    return writable ? vm_writable(low,high-low):vm_readable(low,high-low);
}
void gp_read_rows(uint32_t *out,int out_stride,const uint8_t *scan,int stride,int width,int height,uint32_t format)
{
    int bytes=gp_pixel_bytes(format);
    for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
        const uint8_t *p=scan+(int64_t)y*stride+x*bytes;
        uint32_t alpha=(format==GP_ARGB32 || format==GP_PARGB32) ? p[3]:255;
        uint32_t r=p[2],g=p[1],b=p[0];
        if(format==GP_PARGB32) {
            r=alpha ? r*255/alpha:0;
            g=alpha ? g*255/alpha:0;
            b=alpha ? b*255/alpha:0;
            if(r>255) r=255;
            if(g>255) g=255;
            if(b>255) b=255;
        }
        out[y*out_stride+x]=alpha<<24|r<<16|g<<8|b;
    }
}
void gp_write_rows(uint8_t *scan,int stride,const uint32_t *in,int in_stride,int width,int height,uint32_t format)
{
    int bytes=gp_pixel_bytes(format);
    for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
        uint32_t c=in[y*in_stride+x],alpha=c>>24;
        uint8_t *p=scan+(int64_t)y*stride+x*bytes;
        for(int i=0;i<3;i++) p[i]=format==GP_PARGB32 ? ((c>>(i*8))&255)*alpha/255:c>>(i*8);
        if(bytes==4) p[3]=format==GP_RGB32 ? 255:alpha;
    }
}
int gp_bitmap_sync(struct gp_bitmap *b,int write)
{
    if(!b->scan0) return GP_OK;
    if(!gp_rows_valid(b->scan0,b->stride,b->pixels.w,b->pixels.h,gp_pixel_bytes(b->format),write)) return GP_INVALID;
    if(write) gp_write_rows(GP(b->scan0),b->stride,b->pixels.px,b->pixels.stride,b->pixels.w,b->pixels.h,b->format);
    else gp_read_rows(b->pixels.px,b->pixels.stride,GP(b->scan0),b->stride,b->pixels.w,b->pixels.h,b->format);
    return GP_OK;
}
uint32_t gp_publish_bitmap(int width,int height,uint32_t format,gp_t out,struct gp_bitmap **bitmap)
{
    if(!sessions) return GP_NOTINITIALIZED;
    if(!output(out,4) || width<=0 || height<=0 || width>16384 || height>16384 || !gp_pixel_bytes(format)) return GP_INVALID;
    if((uint64_t)width*height>16*1024*1024) return GP_MEMORY;
    struct object *o=new_object(BITMAP);
    if(!o) return GP_MEMORY;
    struct gp_bitmap *b=&o->u.bitmap;
    if(win_surface_init(&b->pixels,width,height)<0) {
        memset(o,0,sizeof(*o));
        return GP_MEMORY;
    }
    b->format=format;
    b->dpi_x=b->dpi_y=96;
    if(format==GP_RGB24 || format==GP_RGB32) for(int i=0;i<width*height;i++) b->pixels.px[i]=0xFF000000;
    *(uint32_t *)GP(out)=o->handle;
    *bitmap=b;
    return GP_OK;
}
DEF(GdiplusNotificationHook) {
    if(!output(A(0),4)) return GP_INVALID;
    *(uint32_t *)P(0)=1;
    return GP_OK;
}
DEF(GdiplusNotificationUnhook) {
    (void)a;
    return 0;
}
DEF(GdiplusStartup)
{
    if(!output(A(0),4) || !A(1) || !vm_readable(A(1),16) || (A(2) && !output(A(2),8))) return GP_INVALID;
    uint32_t *in=P(1);
    if(in[0]!=1) return GP_VERSION;
    if(in[2] && !A(2)) return GP_INVALID;
    struct object *o=new_object(SESSION);
    if(!o) return GP_MEMORY;
    sessions++;
    *(uint32_t *)P(0)=o->handle;
    if(A(2)) {
        uint32_t *out=P(2);
        out[0]=api_thunk(&gdiplus_dll,api_find(&gdiplus_dll,"GdiplusNotificationHook"));
        out[1]=api_thunk(&gdiplus_dll,api_find(&gdiplus_dll,"GdiplusNotificationUnhook"));
    }
    return GP_OK;
}
DEF(GdiplusShutdown) {
    struct object *o=object(A(0),SESSION);
    if(o) {
        memset(o,0,sizeof(*o));
        sessions--;
    }
    return 0;
}
DEF(GdipAlloc) {
    return galloc(A(0) ? A(0):1);
}
DEF(GdipFree) {
    if(A(0)) gfree(A(0));
    return 0;
}
DEF(GdipCreateBitmapFromScan0)
{
    READY();
    int width=A(0),height=A(1),stride=A(2),bytes=gp_pixel_bytes(A(3));
    if(A(4) && ((stride&3) || !gp_rows_valid(A(4),stride,width,height,bytes,0))) return GP_INVALID;
    struct gp_bitmap *b;
    uint32_t status=gp_publish_bitmap(width,height,A(3),A(5),&b);
    if(status) return status;
    b->scan0=A(4);
    b->stride=stride;
    return gp_bitmap_sync(b,0);
}
static uint32_t load_file(uint32_t *a)
{
    if(!sessions) return GP_NOTINITIALIZED;
    if(!A(0) || !output(A(1),4)) return GP_INVALID;
    *(uint32_t *)P(1)=0;
    uint16_t *wide=ui_string(A(0),1);
    int n=wstrlen(wide);
    char *name=malloc((size_t)n*4+1);
    if(!name) {
        free(wide);
        return GP_MEMORY;
    }
    int len=wide_to_utf8(wide,n,name,n*4);
    name[len]=0;
    free(wide);
    char path[PATH_MAX];
    int bad=path_to_host(name,path);
    free(name);
    if(bad<0) return GP_INVALID;
    path_fix_case(path);
    uint32_t size;
    uint8_t *data=file_read_all(path,&size);
    if(!data) return errno==ENOENT ? GP_NOTFOUND:GP_ERROR;
    int status=gp_decode(data,size,A(1));
    free(data);
    return status;
}
DEF(GdipLoadImageFromFile) {
    return load_file(a);
}
DEF(GdipCreateBitmapFromFile) {
    return load_file(a);
}
/* ICM variants must not silently pretend that profiles were applied. */
DEF(GdipLoadImageFromFileICM) {
    (void)a;
    return GP_NOTIMPL;
}
DEF(GdipCreateBitmapFromFileICM) {
    (void)a;
    return GP_NOTIMPL;
}
DEF(GdipDisposeImage)
{
    READY();
    struct object *o=object(A(0),BITMAP);
    if(!o) return GP_INVALID;
    struct gp_bitmap *b=&o->u.bitmap;
    if(b->lock_data || b->graphics) return GP_BUSY;
    win_surface_free(&b->pixels);
    memset(o,0,sizeof(*o));
    return GP_OK;
}
static uint32_t dimension(uint32_t *a,int what)
{
    if(!sessions) return GP_NOTINITIALIZED;
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || !output(A(1),4)) return GP_INVALID;
    uint32_t *out=P(1);
    switch(what) {
        case 0:*out=b->pixels.w;
        break;
        case 1:*out=b->pixels.h;
        break;
        case 2:*out=b->format;
        break;
        case 3:*out=1;
        break;
        case 4:*(float *)out=b->dpi_x;
        break;
        case 5:*(float *)out=b->dpi_y;
        break;
        case 6:*out=b->format&0x40000 ? 2:0;
        break;
    }
    return GP_OK;
}
DEF(GdipGetImageWidth) {
    return dimension(a,0);
}
DEF(GdipGetImageHeight) {
    return dimension(a,1);
}
DEF(GdipGetImagePixelFormat) {
    return dimension(a,2);
}
DEF(GdipGetImageType) {
    return dimension(a,3);
}
DEF(GdipGetImageHorizontalResolution) {
    return dimension(a,4);
}
DEF(GdipGetImageVerticalResolution) {
    return dimension(a,5);
}
DEF(GdipGetImageFlags) {
    return dimension(a,6);
}
DEF(GdipGetImageDimension)
{
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || !output(A(1),4) || !output(A(2),4)) return GP_INVALID;
    *(float *)P(1)=b->pixels.w;
    *(float *)P(2)=b->pixels.h;
    return GP_OK;
}
DEF(GdipGetImageBounds)
{
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || !output(A(1),16) || !output(A(2),4)) return GP_INVALID;
    float *r=P(1);
    r[0]=r[1]=0;
    r[2]=b->pixels.w;
    r[3]=b->pixels.h;
    *(int *)P(2)=2;
    return GP_OK;
}
DEF(GdipGetImageRawFormat) {
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || !output(A(1),16)) return GP_INVALID;
    gp_format_guid(b->raw,P(1));
    return GP_OK;
}
DEF(GdipBitmapSetResolution)
{
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    float x=real(A(1)),y=real(A(2));
    if(!b || !finite_real(x) || !finite_real(y) || x<=0 || y<=0) return GP_INVALID;
    b->dpi_x=x;
    b->dpi_y=y;
    return GP_OK;
}
DEF(GdipBitmapGetPixel)
{
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || A(1)>=(uint32_t)b->pixels.w || A(2)>=(uint32_t)b->pixels.h || !output(A(3),4)) return GP_INVALID;
    if(b->lock_data) return GP_BUSY;
    int status=gp_bitmap_sync(b,0);
    if(status) return status;
    *(uint32_t *)P(3)=b->pixels.px[A(2)*b->pixels.stride+A(1)];
    return GP_OK;
}
DEF(GdipBitmapSetPixel)
{
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || A(1)>=(uint32_t)b->pixels.w || A(2)>=(uint32_t)b->pixels.h) return GP_INVALID;
    if(b->lock_data) return GP_BUSY;
    int status=gp_bitmap_sync(b,0);
    if(status) return status;
    b->pixels.px[A(2)*b->pixels.stride+A(1)]=A(3)|((b->format==GP_RGB24 || b->format==GP_RGB32) ? 0xFF000000:0);
    return gp_bitmap_sync(b,1);
}
struct bitmap_data {
    uint32_t width,height;
    int32_t stride;
    uint32_t format;
    gp_t scan;
    uint32_t reserved;
};
DEF(GdipBitmapLockBits)
{
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    int bytes=gp_pixel_bytes(A(3));
    if(!b || !bytes || !output(A(4),sizeof(struct bitmap_data)) || !(A(2)&3) || (A(2)&~7u)) return GP_INVALID;
    if(b->lock_data || b->graphics) return GP_BUSY;
    RECT r={
        0,0,b->pixels.w,b->pixels.h
    };
    if(A(1)) {
        if(!vm_readable(A(1),16)) return GP_INVALID;
        int *p=P(1);
        r=(RECT){
            p[0],p[1],p[2],p[3]
        };
    }
    int x=r.left,y=r.top,w=r.right,h=r.bottom;
    // GDI+ Rect is x,y,width,height.
    if(x<0 || y<0 || w<=0 || h<=0 || (int64_t)x+w>b->pixels.w || (int64_t)y+h>b->pixels.h) return GP_INVALID;
    int status=gp_bitmap_sync(b,0);
    if(status) return status;
    struct bitmap_data *data=P(4);
    int stride=(w*bytes+3)&~3;
    gp_t bits;
    if(A(2)&4) {
        bits=data->scan;
        stride=data->stride;
        if(!gp_rows_valid(bits,stride,w,h,bytes,1)) return GP_INVALID;
    }
    else {
        bits=galloc((uint32_t)stride*h);
        if(!bits) return GP_MEMORY;
    }
    if(A(2)&1) gp_write_rows(GP(bits),stride,b->pixels.px+y*b->pixels.stride+x,b->pixels.stride,w,h,A(3));
    b->lock_rect=r;
    b->lock_bits=bits;
    b->lock_stride=stride;
    b->lock_flags=A(2);
    b->lock_format=A(3);
    b->lock_owned=!(A(2)&4);
    b->lock_data=A(4);
    *data=(struct bitmap_data){
        w,h,stride,A(3),bits,0
    };
    return GP_OK;
}
DEF(GdipBitmapUnlockBits)
{
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || !A(1) || b->lock_data!=A(1) || !vm_readable(A(1),24)) return GP_INVALID;
    struct bitmap_data *data=P(1);
    if(data->scan!=b->lock_bits || data->stride!=b->lock_stride || data->format!=b->lock_format) return GP_INVALID;
    RECT r=b->lock_rect;
    int status=GP_OK;
    if(b->lock_flags&2) {
        if(!gp_rows_valid(b->lock_bits,b->lock_stride,r.right,r.bottom,gp_pixel_bytes(b->lock_format),0)) return GP_INVALID;
        gp_read_rows(b->pixels.px+r.top*b->pixels.stride+r.left,b->pixels.stride,GP(b->lock_bits),b->lock_stride,r.right,r.bottom,b->lock_format);
        status=gp_bitmap_sync(b,1);
    }
    if(b->lock_owned) gfree(b->lock_bits);
    b->lock_data=b->lock_bits=0;
    return status;
}
static uint32_t clone_area(uint32_t *a,int floats)
{
    if(!sessions) return GP_NOTINITIALIZED;
    float coords[4];
    for(int i=0;i<4;i++) {
        coords[i]=floats ? real(A(i)):(float)(int32_t)A(i);
        if(!finite_real(coords[i])) return GP_INVALID;
    }
    int x=coords[0],y=coords[1],w=coords[2],h=coords[3];
    struct gp_bitmap *src=gp_bitmap(A(5));
    if(!src || x<0 || y<0 || w<=0 || h<=0 || (int64_t)x+w>src->pixels.w || (int64_t)y+h>src->pixels.h) return GP_INVALID;
    if(src->lock_data) return GP_BUSY;
    int status=gp_bitmap_sync(src,0);
    if(status) return status;
    struct gp_bitmap *dst;
    status=gp_publish_bitmap(w,h,A(4),A(6),&dst);
    if(status) return status;
    for(int j=0;j<h;j++) memcpy(dst->pixels.px+j*w,src->pixels.px+(y+j)*src->pixels.stride+x,w*4);
    if(A(4)==GP_RGB24 || A(4)==GP_RGB32) for(int i=0;i<w*h;i++) dst->pixels.px[i]|=0xFF000000;
    dst->dpi_x=src->dpi_x;
    dst->dpi_y=src->dpi_y;
    return GP_OK;
}
DEF(GdipCloneBitmapAreaI) {
    return clone_area(a,0);
}
DEF(GdipCloneBitmapArea) {
    return clone_area(a,1);
}
DEF(GdipCloneImage)
{
    READY();
    struct gp_bitmap *src=gp_bitmap(A(0));
    if(!src) return GP_INVALID;
    uint32_t args[]={
        0,0,src->pixels.w,src->pixels.h,src->format,A(0),A(1)
    };
    int status=clone_area(args,0);
    if(!status) gp_bitmap(*(uint32_t *)P(1))->raw=src->raw;
    return status;
}
DEF(GdipImageRotateFlip)
{
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || A(1)>7) return GP_INVALID;
    if(b->lock_data || b->graphics) return GP_BUSY;
    if(b->scan0) return GP_NOTIMPL;
    int rotate=A(1)&3,flip=A(1)&4,w=b->pixels.w,h=b->pixels.h;
    struct surface next;
    if(win_surface_init(&next,(rotate&1) ? h:w,(rotate&1) ? w:h)<0) return GP_MEMORY;
    for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
        int nx=x,ny=y;
        if(rotate==1) {
            nx=h-1-y;
            ny=x;
        }
        else if(rotate==2) {
            nx=w-1-x;
            ny=h-1-y;
        }
        else if(rotate==3) {
            nx=y;
            ny=w-1-x;
        }
        if(flip) nx=next.w-1-nx;
        next.px[ny*next.stride+nx]=b->pixels.px[y*b->pixels.stride+x];
    }
    win_surface_free(&b->pixels);
    b->pixels=next;
    return GP_OK;
}
DEF(GdipGetPropertyCount) {
    READY();
    if(!gp_bitmap(A(0)) || !output(A(1),4)) return GP_INVALID;
    *(uint32_t *)P(1)=0;
    return GP_OK;
}
DEF(GdipGetPropertySize) {
    READY();
    if(!gp_bitmap(A(0)) || !output(A(1),4) || !output(A(2),4)) return GP_INVALID;
    *(uint32_t *)P(1)=*(uint32_t *)P(2)=0;
    return GP_OK;
}
DEF(GdipGetPropertyItemSize) {
    READY();
    if(!gp_bitmap(A(0)) || !output(A(2),4)) return GP_INVALID;
    *(uint32_t *)P(2)=0;
    return GP_PROPERTYNOTFOUND;
}
DEF(GdipGetPropertyItem) {
    READY();
    return gp_bitmap(A(0)) ? GP_PROPERTYNOTFOUND:GP_INVALID;
}
DEF(GdipGetImagePaletteSize) {
    READY();
    if(!gp_bitmap(A(0)) || !output(A(1),4)) return GP_INVALID;
    *(uint32_t *)P(1)=8;
    return GP_OK;
}
DEF(GdipGetImagePalette) {
    READY();
    if(!gp_bitmap(A(0)) || A(2)<8 || !output(A(1),8)) return GP_INVALID;
    memset(P(1),0,8);
    return GP_OK;
}
/* GDI bitmap interop retains alpha only in GDI+; HBITMAPs are opaque RGB. */
DEF(GdipCreateBitmapFromHBITMAP)
{
    READY();
    if(!output(A(2),4)) return GP_INVALID;
    struct image im={0};
    if(gdi_bitmap_copy(A(0),&im)<0) return GP_INVALID;
    struct gp_bitmap *b;
    int status=gp_publish_bitmap(im.w,im.h,GP_RGB32,A(2),&b);
    if(!status) memcpy(b->pixels.px,im.px,(size_t)im.w*im.h*4);
    free(im.px);
    return status;
}
DEF(GdipCreateHBITMAPFromBitmap)
{
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || !output(A(1),4)) return GP_INVALID;
    if(b->lock_data) return GP_BUSY;
    int status=gp_bitmap_sync(b,0);
    if(status) return status;
    struct dibheader {
        uint32_t size;
        int32_t w,h;
        uint16_t planes,bpp;
        uint32_t compression,sizeimage;
        int32_t xppm,yppm;
        uint32_t used,important;
    }
    header={
        40,b->pixels.w,-b->pixels.h,1,32,0,0,0,0,0,0
    };
    gp_t info=ui_memory(&header,40),out=galloc(4);
    HBITMAP bitmap=G(CreateDIBSection,0,info,0,out,0,0);
    gp_t bits=*(gp_t *)GP(out);
    gfree(info);
    gfree(out);
    if(!bitmap) return GP_MEMORY;
    uint32_t *pixels=GP(bits),bg=A(2);
    for(int i=0;i<b->pixels.w*b->pixels.h;i++) {
        uint32_t c=b->pixels.px[i],alpha=c>>24,rgb=0;
        for(int shift=0;shift<24;shift+=8) rgb|=((((c>>shift)&255)*alpha+((bg>>shift)&255)*(255-alpha))/255)<<shift;
        pixels[i]=rgb;
    }
    *(HBITMAP *)P(1)=bitmap;
    return GP_OK;
}
DEF(GdipCreateBitmapFromHICON)
{
    READY();
    const struct image *im=icon_image(A(0));
    if(!im) return GP_INVALID;
    struct gp_bitmap *b;
    int status=gp_publish_bitmap(im->w,im->h,GP_ARGB32,A(1),&b);
    if(!status) memcpy(b->pixels.px,im->px,(size_t)im->w*im->h*4);
    return status;
}
DEF(GdipCreateHICONFromBitmap)
{
    READY();
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || !output(A(1),4)) return GP_INVALID;
    if(b->lock_data) return GP_BUSY;
    int status=gp_bitmap_sync(b,0);
    if(status) return status;
    struct image im={
        b->pixels.w,b->pixels.h,b->pixels.px
    };
    HICON icon=icon_create(&im);
    if(!icon) return GP_MEMORY;
    *(HICON *)P(1)=icon;
    return GP_OK;
}
static uint32_t create_graphics(uint32_t image,HDC dc,HWND owner,int own,gp_t out)
{
    if(!sessions) return GP_NOTINITIALIZED;
    if(!output(out,4)) return GP_INVALID;
    struct gp_bitmap *b=image ? gp_bitmap(image):NULL;
    if(image && !b) return GP_INVALID;
    if(b && (b->lock_data || b->graphics)) return GP_BUSY;
    if(!image && !gdi_dc_valid(dc)) return GP_INVALID;
    struct object *o=new_object(GRAPHICS);
    if(!o) return GP_MEMORY;
    o->u.graphics=(struct graphics){
        .image=image,.dc=dc,.owner=owner,.own_dc=own,.unit=1,.scale=1,.sx=1,.sy=1,.interpolation=5
    };
    if(b) b->graphics++;
    *(uint32_t *)GP(out)=o->handle;
    return GP_OK;
}
DEF(GdipGetImageGraphicsContext) {
    return create_graphics(A(0),0,0,0,A(1));
}
DEF(GdipCreateFromHDC) {
    return create_graphics(0,A(0),0,0,A(1));
}
DEF(GdipCreateFromHDC2) {
    return create_graphics(0,A(0),0,0,A(2));
}
DEF(GdipCreateFromHWND)
{
    READY();
    if(!output(A(1),4) || (A(0) && !ui_win(A(0)))) return GP_INVALID;
    HDC dc=U(GetDC,A(0));
    if(!dc) return GP_INVALID;
    int status=create_graphics(0,dc,A(0),1,A(1));
    if(status) U(ReleaseDC,A(0),dc);
    return status;
}
DEF(GdipCreateFromHWNDICM) {
    (void)a;
    return GP_NOTIMPL;
}
DEF(GdipDeleteGraphics)
{
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o) return GP_INVALID;
    struct graphics *g=&o->u.graphics;
    if(g->busy) return GP_BUSY;
    struct gp_bitmap *b=gp_bitmap(g->image);
    if(b) b->graphics--;
    if(g->own_dc) U(ReleaseDC,g->owner,g->dc);
    memset(o,0,sizeof(*o));
    return GP_OK;
}
static uint32_t blend(uint32_t src,uint32_t dst,int copy)
{
    if(copy) return src;
    uint32_t sa=src>>24,da=dst>>24,alpha=sa+da*(255-sa)/255,result=alpha<<24;
    for(int sh=0;sh<24;sh+=8) {
        uint32_t value=((src>>sh)&255)*sa+((dst>>sh)&255)*da*(255-sa)/255;
        result|=(alpha ? value/alpha:0)<<sh;
    }
    return result;
}
static uint32_t paint_pixels(struct graphics *g,int dx,int dy,int w,int h,const uint32_t *pixels,int stride)
{
    if(g->busy) return GP_BUSY;
    struct gp_bitmap *b=gp_bitmap(g->image);
    if(b) {
        if(b->lock_data) return GP_BUSY;
        int status=gp_bitmap_sync(b,0);
        if(status) return status;
        for(int y=0;y<h;y++) if((int64_t)dy+y>=0 && (int64_t)dy+y<b->pixels.h) for(int x=0;x<w;x++) if((int64_t)dx+x>=0 && (int64_t)dx+x<b->pixels.w) {
            uint32_t *dest=&b->pixels.px[(dy+y)*b->pixels.stride+dx+x];
            *dest=blend(pixels[y*stride+x],*dest,g->composite);
            if(b->format==GP_RGB24 || b->format==GP_RGB32) *dest|=0xFF000000;
        }
        return gp_bitmap_sync(b,1);
    }
    return gdi_argb_blit(g->dc,dx,dy,w,h,pixels,stride,g->composite)<0 ? GP_INVALID:GP_OK;
}
static uint32_t draw_rect(struct graphics *g,struct gp_bitmap *b,float dx,float dy,float dw,float dh,float sx,float sy,float sw,float sh,int unit)
{
    if(unit!=2) return GP_NOTIMPL;
    if(b->lock_data || g->busy) return GP_BUSY;
    float values[]={
        dx,dy,dw,dh,sx,sy,sw,sh
    };
    for(unsigned i=0;i<8;i++) if(!finite_real(values[i])) return GP_INVALID;
    if(dw<=0 || dh<=0 || sw<=0 || sh<=0 || sx<0 || sy<0 || sx+sw>b->pixels.w || sy+sh>b->pixels.h) return GP_INVALID;
    float transformed[]={
        dw*g->scale*g->sx,dh*g->scale*g->sy,dx*g->scale*g->sx+g->tx,dy*g->scale*g->sy+g->ty
    };
    for(unsigned i=0;i<4;i++) if(!finite_real(transformed[i])) return GP_INVALID;
    int width=transformed[0],height=transformed[1],x=transformed[2],y=transformed[3];
    if(width<=0 || height<=0) return GP_NOTIMPL;
    if(width>16384 || height>16384 || (uint64_t)width*height>16*1024*1024) return GP_MEMORY;
    int status=gp_bitmap_sync(b,0);
    if(status) return status;
    struct surface scaled;
    if(win_surface_init(&scaled,width,height)<0) return GP_MEMORY;
    for(int j=0;j<height;j++) for(int i=0;i<width;i++) {
        int source_x=sx+(i+0.5f)*sw/width,source_y=sy+(j+0.5f)*sh/height;
        if(source_x>=b->pixels.w) source_x=b->pixels.w-1;
        if(source_y>=b->pixels.h) source_y=b->pixels.h-1;
        scaled.px[j*width+i]=b->pixels.px[source_y*b->pixels.stride+source_x];
    }
    status=paint_pixels(g,x,y,width,height,scaled.px,scaled.stride);
    win_surface_free(&scaled);
    return status;
}
static uint32_t draw_image(uint32_t *a,int type,int floats)
{
    if(!sessions) return GP_NOTINITIALIZED;
    struct object *o=object(A(0),GRAPHICS);
    struct gp_bitmap *b=gp_bitmap(A(1));
    if(!o || !b) return GP_INVALID;
    float v[8]={
        0,0,b->pixels.w,b->pixels.h,0,0,b->pixels.w,b->pixels.h
    };
    int count=type==0 ? 2:type==1 ? 4:8;
    for(int i=0;i<count;i++) v[i]=floats ? real(A(i+2)):(float)(int32_t)A(i+2);
    if(type==2 && A(11)) return GP_NOTIMPL;
    // Image attributes need a separate color pipeline.
    int unit=type==2 ? A(10):2;
    if(type==2 && A(12) && call_guest1(A(12),A(13))) return GP_ABORTED;
    /* A guest abort callback can delete either object. */
    o=object(A(0),GRAPHICS);
    b=gp_bitmap(A(1));
    if(!o || !b) return GP_INVALID;
    return draw_rect(&o->u.graphics,b,v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],unit);
}
DEF(GdipDrawImageI) {
    return draw_image(a,0,0);
}
DEF(GdipDrawImage) {
    return draw_image(a,0,1);
}
DEF(GdipDrawImageRectI) {
    return draw_image(a,1,0);
}
DEF(GdipDrawImageRect) {
    return draw_image(a,1,1);
}
DEF(GdipDrawImageRectRectI) {
    return draw_image(a,2,0);
}
DEF(GdipDrawImageRectRect) {
    return draw_image(a,2,1);
}
DEF(GdipCreateSolidFill)
{
    READY();
    if(!output(A(1),4)) return GP_INVALID;
    struct object *o=new_object(BRUSH);
    if(!o) return GP_MEMORY;
    o->u.color=A(0);
    *(uint32_t *)P(1)=o->handle;
    return GP_OK;
}
DEF(GdipDeleteBrush) {
    READY();
    struct object *o=object(A(0),BRUSH);
    if(!o) return GP_INVALID;
    memset(o,0,sizeof(*o));
    return GP_OK;
}
DEF(GdipSetSolidFillColor) {
    READY();
    struct object *o=object(A(0),BRUSH);
    if(!o) return GP_INVALID;
    o->u.color=A(1);
    return GP_OK;
}
DEF(GdipGetSolidFillColor) {
    READY();
    struct object *o=object(A(0),BRUSH);
    if(!o || !output(A(1),4)) return GP_INVALID;
    *(uint32_t *)P(1)=o->u.color;
    return GP_OK;
}
static uint32_t fill(struct graphics *g,uint32_t color,int x,int y,int width,int height)
{
    if(width<=0 || height<=0) return GP_INVALID;
    if(width>16384 || height>16384 || (uint64_t)width*height>16*1024*1024) return GP_MEMORY;
    struct surface pixels;
    if(win_surface_init(&pixels,width,height)<0) return GP_MEMORY;
    for(int i=0;i<width*height;i++) pixels.px[i]=color;
    int status=paint_pixels(g,x,y,width,height,pixels.px,width);
    win_surface_free(&pixels);
    return status;
}
static uint32_t fill_rect(uint32_t *a,int floats)
{
    if(!sessions) return GP_NOTINITIALIZED;
    struct object *o=object(A(0),GRAPHICS),*brush=object(A(1),BRUSH);
    if(!o || !brush) return GP_INVALID;
    float v[4];
    for(int i=0;i<4;i++) {
        v[i]=floats ? real(A(i+2)):(float)(int32_t)A(i+2);
        if(!finite_real(v[i])) return GP_INVALID;
    }
    struct graphics *g=&o->u.graphics;
    float transformed[]={
        v[0]*g->scale*g->sx+g->tx,v[1]*g->scale*g->sy+g->ty,v[2]*g->scale*g->sx,v[3]*g->scale*g->sy
    };
    for(unsigned i=0;i<4;i++) if(!finite_real(transformed[i])) return GP_INVALID;
    return fill(g,brush->u.color,transformed[0],transformed[1],transformed[2],transformed[3]);
}
DEF(GdipFillRectangleI) {
    return fill_rect(a,0);
}
DEF(GdipFillRectangle) {
    return fill_rect(a,1);
}
DEF(GdipGraphicsClear)
{
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o) return GP_INVALID;
    struct graphics *g=&o->u.graphics;
    struct gp_bitmap *b=gp_bitmap(g->image);
    int width,height;
    if(b) {
        width=b->pixels.w;
        height=b->pixels.h;
    }
    else if(gdi_dc_size(g->dc,&width,&height)<0) return GP_INVALID;
    int old=g->composite;
    g->composite=1;
    int status=fill(g,A(1),0,0,width,height);
    g->composite=old;
    return status;
}
DEF(GdipSetCompositingMode) {
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o || A(1)>1) return GP_INVALID;
    o->u.graphics.composite=A(1);
    return GP_OK;
}
DEF(GdipGetCompositingMode) {
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o || !output(A(1),4)) return GP_INVALID;
    *(int *)P(1)=o->u.graphics.composite;
    return GP_OK;
}
DEF(GdipSetInterpolationMode) {
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o || A(1)>7) return GP_INVALID;
    if(A(1)!=5) return GP_NOTIMPL;
    o->u.graphics.interpolation=5;
    return GP_OK;
}
DEF(GdipGetInterpolationMode) {
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o || !output(A(1),4)) return GP_INVALID;
    *(int *)P(1)=o->u.graphics.interpolation;
    return GP_OK;
}
DEF(GdipSetPageUnit) {
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o || A(1)>6 || !A(1)) return GP_INVALID;
    if(A(1)!=1 && A(1)!=2) return GP_NOTIMPL;
    o->u.graphics.unit=A(1);
    return GP_OK;
}
DEF(GdipSetPageScale) {
    READY();
    struct object *o=object(A(0),GRAPHICS);
    float f=real(A(1));
    if(!o || !finite_real(f) || f<=0 || f>10000) return GP_INVALID;
    o->u.graphics.scale=f;
    return GP_OK;
}
DEF(GdipResetWorldTransform) {
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o) return GP_INVALID;
    o->u.graphics.sx=o->u.graphics.sy=1;
    o->u.graphics.tx=o->u.graphics.ty=0;
    return GP_OK;
}
DEF(GdipTranslateWorldTransform)
{
    READY();
    struct object *o=object(A(0),GRAPHICS);
    float x=real(A(1)),y=real(A(2));
    if(!o || !finite_real(x) || !finite_real(y) || A(3)>1) return GP_INVALID;
    struct graphics *g=&o->u.graphics;
    float tx=g->tx+x*(A(3) ? 1:g->sx),ty=g->ty+y*(A(3) ? 1:g->sy);
    if(!finite_real(tx) || !finite_real(ty)) return GP_INVALID;
    g->tx=tx;
    g->ty=ty;
    return GP_OK;
}
DEF(GdipScaleWorldTransform)
{
    READY();
    struct object *o=object(A(0),GRAPHICS);
    float x=real(A(1)),y=real(A(2));
    if(!o || !finite_real(x) || !finite_real(y) || x<=0 || y<=0 || A(3)>1) return GP_INVALID;
    struct graphics *g=&o->u.graphics;
    if(!finite_real(g->sx*x) || !finite_real(g->sy*y)) return GP_INVALID;
    g->sx*=x;
    g->sy*=y;
    if(A(3)) {
        g->tx*=x;
        g->ty*=y;
    }
    return GP_OK;
}
DEF(GdipFlush) {
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o || A(1)>1) return GP_INVALID;
    if(o->u.graphics.busy) return GP_BUSY;
    gui_flush();
    return GP_OK;
}
DEF(GdipGetDC) {
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o || !output(A(1),4)) return GP_INVALID;
    if(o->u.graphics.image) return GP_NOTIMPL;
    if(o->u.graphics.busy) return GP_BUSY;
    o->u.graphics.busy=1;
    *(HDC *)P(1)=o->u.graphics.dc;
    return GP_OK;
}
DEF(GdipReleaseDC) {
    READY();
    struct object *o=object(A(0),GRAPHICS);
    if(!o || !o->u.graphics.busy || o->u.graphics.dc!=A(1)) return GP_INVALID;
    o->u.graphics.busy=0;
    return GP_OK;
}
int gp_started(void) {
    return sessions!=0;
}
DEF(GdipCreateBitmapFromGdiDib)
{
    READY();if(!vm_readable(A(0),40) || !A(1) || !output(A(2),4)) return GP_INVALID;
    HBITMAP bitmap=G(CreateDIBitmap,0,A(0),4,A(1),A(0),0);if(!bitmap) return GP_INVALID;
    uint32_t args[]={bitmap,0,A(2)};uint32_t status=api_GdipCreateBitmapFromHBITMAP(args);G(DeleteObject,bitmap);return status;
}
DEF(GdipCreateBitmapFromGraphics)
{
    READY();if(!object(A(2),GRAPHICS)) return GP_INVALID;struct gp_bitmap *b;
    return gp_publish_bitmap(A(0),A(1),GP_ARGB32,A(3),&b);
}
DEF(GdipImageForceValidation) {READY();return gp_bitmap(A(0)) ? GP_OK:GP_INVALID;}
DEF(GdipGetImageThumbnail)
{
    READY();struct gp_bitmap *src=gp_bitmap(A(0));if(!src || !output(A(3),4) || !A(1) || !A(2) || A(1)>16384 || A(2)>16384) return GP_INVALID;
    if(src->lock_data) return GP_BUSY;
    if(A(4) && call_guest1(A(4),A(5))) return GP_ABORTED;
    src=gp_bitmap(A(0));if(!src) return GP_INVALID;int status=gp_bitmap_sync(src,0);if(status) return status;
    struct gp_bitmap *dst;status=gp_publish_bitmap(A(1),A(2),GP_ARGB32,A(3),&dst);if(status) return status;
    for(uint32_t y=0;y<A(2);y++) for(uint32_t x=0;x<A(1);x++) dst->pixels.px[y*A(1)+x]=src->pixels.px[(y*src->pixels.h/A(2))*src->pixels.stride+x*src->pixels.w/A(1)];
    return GP_OK;
}
static const struct api_entry entries[]={
    {
        "GdiplusNotificationHook",api_GdiplusNotificationHook,1,0,NULL,606
    }
    ,
    {
        "GdiplusNotificationUnhook",api_GdiplusNotificationUnhook,1,0,NULL,607
    }
    ,
    {
        "GdiplusStartup",api_GdiplusStartup,3,0,NULL,609
    }
    ,
    {
        "GdiplusShutdown",api_GdiplusShutdown,1,0,NULL,608
    }
    ,
    {
        "GdipAlloc",api_GdipAlloc,1,0,NULL,34
    }
    ,
    API(GdipFree,1),
    {
        "GdipCreateBitmapFromScan0",api_GdipCreateBitmapFromScan0,6,0,NULL,73
    }
    ,
    API(GdipLoadImageFromFile,2),
    {
        "GdipCreateBitmapFromFile",api_GdipCreateBitmapFromFile,2,0,NULL,66
    }
    ,
    API(GdipLoadImageFromFileICM,2),
    {
        "GdipCreateBitmapFromFileICM",api_GdipCreateBitmapFromFileICM,2,0,NULL,67
    }
    ,
    API(GdipDisposeImage,1),
    API(GdipGetImageWidth,2),
    API(GdipGetImageHeight,2),
    API(GdipGetImagePixelFormat,2),
    API(GdipGetImageType,2),
    API(GdipGetImageHorizontalResolution,2),
    API(GdipGetImageVerticalResolution,2),
    API(GdipGetImageFlags,2),
    API(GdipGetImageDimension,3),
    API(GdipGetImageBounds,3),
    API(GdipGetImageRawFormat,2),
    {
        "GdipBitmapSetResolution",api_GdipBitmapSetResolution,3,0,NULL,41
    }
    ,
    {
        "GdipBitmapGetPixel",api_GdipBitmapGetPixel,4,0,NULL,38
    }
    ,
    {
        "GdipBitmapSetPixel",api_GdipBitmapSetPixel,4,0,NULL,40
    }
    ,
    {
        "GdipBitmapLockBits",api_GdipBitmapLockBits,5,0,NULL,39
    }
    ,
    {
        "GdipBitmapUnlockBits",api_GdipBitmapUnlockBits,2,0,NULL,42
    }
    ,
    {
        "GdipCloneBitmapAreaI",api_GdipCloneBitmapAreaI,7,0,NULL,45
    }
    ,
    {
        "GdipCloneBitmapArea",api_GdipCloneBitmapArea,7,0,NULL,44
    }
    ,
    {
        "GdipCloneImage",api_GdipCloneImage,2,0,NULL,50
    }
    ,
    API(GdipImageRotateFlip,2),
    API(GdipGetPropertyCount,2),
    API(GdipGetPropertySize,3),
    API(GdipGetPropertyItemSize,3),
    API(GdipGetPropertyItem,4),
    API(GdipGetImagePaletteSize,2),
    API(GdipGetImagePalette,3),
    {
        "GdipCreateBitmapFromHBITMAP",api_GdipCreateBitmapFromHBITMAP,3,0,NULL,70
    }
    ,
    {
        "GdipCreateHBITMAPFromBitmap",api_GdipCreateHBITMAPFromBitmap,3,0,NULL,87
    }
    ,
    {
        "GdipCreateBitmapFromHICON",api_GdipCreateBitmapFromHICON,2,0,NULL,71
    }
    ,
    {
        "GdipCreateHICONFromBitmap",api_GdipCreateHICONFromBitmap,2,0,NULL,88
    }
    ,
    API(GdipGetImageGraphicsContext,2),
    {
        "GdipCreateFromHDC",api_GdipCreateFromHDC,2,0,NULL,84
    }
    ,
    {
        "GdipCreateFromHDC2",api_GdipCreateFromHDC2,3,0,NULL,83
    }
    ,
    {
        "GdipCreateFromHWND",api_GdipCreateFromHWND,2,0,NULL,85
    }
    ,
    {
        "GdipCreateFromHWNDICM",api_GdipCreateFromHWNDICM,2,0,NULL,86
    }
    ,
    API(GdipDeleteGraphics,1),
    API(GdipDrawImageI,4),
    API(GdipDrawImage,4),
    API(GdipDrawImageRectI,6),
    API(GdipDrawImageRect,6),
    API(GdipDrawImageRectRectI,14),
    API(GdipDrawImageRectRect,14),
    API(GdipCreateSolidFill,2),
    API(GdipDeleteBrush,1),
    API(GdipSetSolidFillColor,2),
    API(GdipGetSolidFillColor,2),
    API(GdipFillRectangleI,6),
    API(GdipFillRectangle,6),
    API(GdipGraphicsClear,2),
    API(GdipSetCompositingMode,2),
    API(GdipGetCompositingMode,2),
    API(GdipSetInterpolationMode,2),
    API(GdipGetInterpolationMode,2),
    API(GdipSetPageUnit,2),
    API(GdipSetPageScale,2),
    API(GdipResetWorldTransform,1),
    API(GdipTranslateWorldTransform,4),
    API(GdipScaleWorldTransform,4),
    API(GdipFlush,2),
    API(GdipCreateBitmapFromGdiDib,3),
    API(GdipCreateBitmapFromGraphics,4),
    API(GdipImageForceValidation,1),
    API(GdipGetImageThumbnail,6),
    API(GdipGetDC,2),
    API(GdipReleaseDC,2),
};
void gdiplus_link(void) {
    gdiplus_dll.tables[0]=gdip_codec_entries;
    gdiplus_dll.counts[0]=gdip_codec_count;
}
struct api_dll gdiplus_dll={
    .name="GDIPLUS.DLL",.entries=entries,.count=sizeof(entries)/sizeof(entries[0])
};
