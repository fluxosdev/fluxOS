/* Image lists own their pixels; bitmap and icon handles supplied by callers stay theirs. */
#include "ui.h"
#define MAX_LISTS 256
struct ilist {
    HANDLE handle;
    int w,h,n,cap,grow;
    COLORREF background;
    struct surface *tiles;
    int overlays[16];
};
static struct ilist lists[MAX_LISTS];
static uint16_t generations[MAX_LISTS];
static struct ilist *get(HANDLE h) {
    unsigned i=h&255;
    return lists[i].handle==h && h ? &lists[i]:NULL;
}
static int reserve(struct ilist *l,int count)
{
    if(count<0 || count>65536) return 0;
    if(count<=l->cap) return 1;
    int cap=count+l->grow;
    if(cap<count || cap>65536) cap=count;
    struct surface *p=realloc(l->tiles,(size_t)cap*sizeof(*p));
    if(!p) return 0;
    memset(p+l->cap,0,(cap-l->cap)*sizeof(*p));
    l->tiles=p;
    l->cap=cap;
    return 1;
}
static int tile(struct ilist *l,int i)
{
    if(!reserve(l,i+1)) return 0;
    if(!l->tiles[i].px && win_surface_init(&l->tiles[i],l->w,l->h)<0) return 0;
    if(i>=l->n) l->n=i+1;
    return 1;
}
uint32_t image_list_create(int w,int h,int initial,int grow)
{
    if(w<=0 || h<=0 || w>1024 || h>1024 || initial<0 || grow<0) return 0;
    for(int i=0;i<MAX_LISTS;i++) if(!lists[i].handle) {
        struct ilist *l=&lists[i];
        l->w=w;
        l->h=h;
        l->grow=grow ? grow:4;
        l->background=0xFFFFFFFF;
        generations[i]=generations[i]%0xFFFF+1;
        l->handle=0x63000000u|(uint32_t)generations[i]<<8|i;
        for(int j=0;j<16;j++) l->overlays[j]=-1;
        if(!reserve(l,initial)) {
            memset(l,0,sizeof(*l));
            return 0;
        }
        return l->handle;
    }
    return 0;
}
static uint32_t destroy(HANDLE h)
{
    struct ilist *l=get(h);
    if(!l) return 0;
    for(int i=0;i<l->cap;i++) win_surface_free(&l->tiles[i]);
    free(l->tiles);
    memset(l,0,sizeof(*l));
    return 1;
}
int imagelist_size(HANDLE h,int *w,int *height) {
    struct ilist *l=get(h);
    if(!l) return 0;
    *w=l->w;
    *height=l->h;
    return 1;
}
static void draw_tile(const struct surface *s,HDC dc,int x,int y,int width,int height)
{
    if(width<=0 || height<=0) return;
    for(int j=0;j<height;j++) for(int i=0;i<width;i++) {
        uint32_t p=s->px[(j*s->h/height)*s->stride+i*s->w/width],alpha=p>>24;
        if(!alpha) continue;
        if(alpha<255) {
            uint32_t old=colorref_rgb(G(GetPixel,dc,x+i,y+j)),mixed=0;
            for(int sh=0;sh<24;sh+=8) mixed|=((((p>>sh)&255)*alpha+((old>>sh)&255)*(255-alpha))/255)<<sh;
            p=mixed;
        }
        G(SetPixel,dc,x+i,y+j,colorref_rgb(p));
    }
}
int imagelist_draw(HANDLE h,int index,HDC dc,int x,int y,int width,int height,uint32_t style)
{
    struct ilist *l=get(h);
    if(!l || index<0 || index>=l->n || !l->tiles[index].px) return 0;
    if(!width) width=l->w;
    if(!height) height=l->h;
    if(l->background!=0xFFFFFFFF && !(style&1)) ui_fill(dc,x,y,width,height,l->background);
    draw_tile(&l->tiles[index],dc,x,y,width,height);
    int overlay=(style>>8)&15;
    if(overlay && l->overlays[overlay]>=0 && l->overlays[overlay]<l->n) draw_tile(&l->tiles[l->overlays[overlay]],dc,x,y,width,height);
    return 1;
}
uint32_t image_list_replace_icon(HANDLE h,int index,HICON icon)
{
    struct ilist *l=get(h);
    const struct image *im=icon_image(icon);
    if(!l) return -1;
    if(index==-1) index=l->n;
    if(index<0 || index>l->n || !tile(l,index)) return -1;
    if(im) for(int y=0;y<l->h;y++) for(int x=0;x<l->w;x++) l->tiles[index].px[y*l->w+x]=im->px[(y*im->h/l->h)*im->w+x*im->w/l->w];
    else {
        HDC dc=G(CreateCompatibleDC,0);
        HBITMAP bitmap=G(CreateCompatibleBitmap,dc,l->w,l->h);
        HGDIOBJ old=G(SelectObject,dc,bitmap);
        U(DrawIconEx,dc,0,0,icon,l->w,l->h,0,0,3);
        for(int y=0;y<l->h;y++) for(int x=0;x<l->w;x++) l->tiles[index].px[y*l->w+x]=0xFF000000|colorref_rgb(G(GetPixel,dc,x,y));
        G(SelectObject,dc,old);
        G(DeleteObject,bitmap);
        G(DeleteDC,dc);
    }
    return index;
}
static int bitmap_tiles(HANDLE handle,int index,HBITMAP bitmap,HBITMAP mask,COLORREF transparent,int append)
{
    struct ilist *l=get(handle);
    if(!l) return -1;
    gp_t info=galloc(24);
    int good=G(GetObjectW,bitmap,24,info);
    int width=good ? ((int *)GP(info))[1]:0;
    gfree(info);
    if(width<l->w) return -1;
    int count=append ? width/l->w:1,first=append ? l->n:index;
    if(first<0 || first>l->n || !reserve(l,first+count)) return -1;
    HDC src=G(CreateCompatibleDC,0),mdc=mask ? G(CreateCompatibleDC,0):0;
    HGDIOBJ old=G(SelectObject,src,bitmap),mold=mask ? G(SelectObject,mdc,mask):0;
    if(!old || (mask && !mold)) {
        if(old) G(SelectObject,src,old);
        if(mold) G(SelectObject,mdc,mold);
        G(DeleteDC,src);if(mdc) G(DeleteDC,mdc);return -1;
    }
    for(int i=0;i<count;i++) {
        if(!tile(l,first+i)) {
            G(SelectObject,src,old);
            G(DeleteDC,src);
            if(mask) {
                G(SelectObject,mdc,mold);
                G(DeleteDC,mdc);
            }
            return -1;
        }
        for(int y=0;y<l->h;y++) for(int x=0;x<l->w;x++) {
            COLORREF c=G(GetPixel,src,i*l->w+x,y);
            int clear=(mask && G(GetPixel,mdc,i*l->w+x,y)!=0) || (transparent!=0xFFFFFFFF && c==transparent);
            l->tiles[first+i].px[y*l->w+x]=(clear ? 0:0xFF000000)|colorref_rgb(c);
        }
    }
    G(SelectObject,src,old);
    G(DeleteDC,src);
    if(mask) {
        G(SelectObject,mdc,mold);
        G(DeleteDC,mdc);
    }
    return first;
}
#define IDEF(name) uint32_t api_##name(uint32_t *a)
IDEF(ImageList_Create) {
    return image_list_create(A(0),A(1),A(3),A(4));
}
IDEF(ImageList_Destroy) {
    return destroy(A(0));
}
IDEF(ImageList_GetImageCount) {
    struct ilist *l=get(A(0));
    return l ? l->n:0;
}
IDEF(ImageList_SetImageCount) {
    struct ilist *l=get(A(0));
    if(!l || !reserve(l,A(1))) return 0;
    while(l->n>(int)A(1)) win_surface_free(&l->tiles[--l->n]);
    while(l->n<(int)A(1)) if(!tile(l,l->n)) return 0;
    return 1;
}
IDEF(ImageList_ReplaceIcon) {
    return image_list_replace_icon(A(0),A(1),A(2));
}
IDEF(ImageList_Add) {
    return bitmap_tiles(A(0),0,A(1),A(2),0xFFFFFFFF,1);
}
IDEF(ImageList_AddMasked) {
    return bitmap_tiles(A(0),0,A(1),0,A(2),1);
}
IDEF(ImageList_Replace) {
    return bitmap_tiles(A(0),A(1),A(2),A(3),0xFFFFFFFF,0)>=0;
}
IDEF(ImageList_Remove)
{
    struct ilist *l=get(A(0));
    if(!l) return 0;
    int i=A(1);
    if(i==-1) {
        for(i=0;i<l->n;i++) win_surface_free(&l->tiles[i]);
        l->n=0;
        return 1;
    }
    if(i<0 || i>=l->n) return 0;
    win_surface_free(&l->tiles[i]);
    memmove(l->tiles+i,l->tiles+i+1,(l->n-i-1)*sizeof(*l->tiles));
    memset(&l->tiles[--l->n],0,sizeof(*l->tiles));
    for(int k=1;k<16;k++) if(l->overlays[k]==i) l->overlays[k]=-1;
    else if(l->overlays[k]>i) l->overlays[k]--;
    return 1;
}
IDEF(ImageList_Draw) {
    return imagelist_draw(A(0),A(1),A(2),A(3),A(4),0,0,A(5));
}
IDEF(ImageList_DrawEx) {
    return imagelist_draw(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(9));
}
IDEF(ImageList_GetIconSize) {
    int w,h;
    if(!imagelist_size(A(0),&w,&h)) return 0;
    if(A(1)) *(int *)P(1)=w;
    if(A(2)) *(int *)P(2)=h;
    return 1;
}
IDEF(ImageList_SetIconSize) {
    struct ilist *l=get(A(0));
    if(!l || !A(1) || !A(2) || A(1)>1024 || A(2)>1024) return 0;
    uint32_t args[]={
        A(0),(uint32_t)-1
    };
    api_ImageList_Remove(args);
    l->w=A(1);
    l->h=A(2);
    return 1;
}
IDEF(ImageList_SetBkColor) {
    struct ilist *l=get(A(0));
    if(!l) return 0xFFFFFFFF;
    COLORREF old=l->background;
    l->background=A(1);
    return old;
}
IDEF(ImageList_GetBkColor) {
    struct ilist *l=get(A(0));
    return l ? l->background:0xFFFFFFFF;
}
IDEF(ImageList_SetOverlayImage) {
    struct ilist *l=get(A(0));
    if(!l || A(1)>=(uint32_t)l->n || !A(2) || A(2)>15) return 0;
    l->overlays[A(2)]=A(1);
    return 1;
}
IDEF(ImageList_GetIcon) {
    struct ilist *l=get(A(0));
    if(!l || A(1)>=(uint32_t)l->n) return 0;
    struct image image={
        l->w,l->h,l->tiles[A(1)].px
    };
    return icon_create(&image);
}
IDEF(ImageList_Duplicate)
{
    struct ilist *l=get(A(0));
    if(!l) return 0;
    HANDLE h=image_list_create(l->w,l->h,l->n,l->grow);
    struct ilist *copy=get(h);
    if(!copy) return 0;
    copy->background=l->background;
    memcpy(copy->overlays,l->overlays,sizeof(l->overlays));
    for(int i=0;i<l->n;i++) {
        if(!tile(copy,i)) {
            destroy(h);
            return 0;
        }
        memcpy(copy->tiles[i].px,l->tiles[i].px,(size_t)l->w*l->h*4);
    }
    return h;
}
