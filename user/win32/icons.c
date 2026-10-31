#include "ui.h"
#define ICON_BASE 0x70000000u
#define ICON_MAX 256
static struct image *icons[ICON_MAX];
static uint16_t rd16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
static struct image *icon_get(HICON h) { return h>=ICON_BASE && h<ICON_BASE+ICON_MAX ? icons[h-ICON_BASE]:NULL; }
HICON icon_load(HINSTANCE inst,gp_t name,int wide)
{
    if(!inst && IS_INTRES(name)) return ICON_HANDLE(name);
    uint32_t size=0; gp_t group=ui_resource(inst,14,name,wide,&size);
    if(!group || size<6) return 0;
    const uint8_t *g=GP(group); int count=rd16(g+4),best=-1,score=0x7FFFFFFF;
    if(count>(int)((size-6)/14)) return 0;
    for(int i=0;i<count;i++) {
        const uint8_t *e=g+6+i*14; int w=e[0] ? e[0]:256,h=e[1] ? e[1]:256;
        int s=abs(w-32)+abs(h-32)+32-rd16(e+6);
        if(s<score) { best=i; score=s; }
    }
    if(best<0) return 0;
    const uint8_t *entry=g+6+best*14;
    gp_t bits=ui_resource(inst,3,rd16(entry+12),1,&size);
    if(!bits || size>16*1024*1024) return 0;
    uint8_t *ico=calloc(1,(size_t)size+22); if(!ico) return 0;
    ico[2]=1; ico[4]=1; memcpy(ico+6,entry,8);
    memcpy(ico+14,&size,4); ico[18]=22; memcpy(ico+22,GP(bits),size);
    struct image *image=image_decode_ico(ico,size+22); free(ico);
    if(!image) return 0;
    for(int i=0;i<ICON_MAX;i++) if(!icons[i]) { icons[i]=image; return ICON_BASE+i; }
    image_free(image); return 0;
}
static HICON icon_find_default(HINSTANCE inst)
{
    struct module *m=inst ? module_by_base(inst):main_module;
    if(!m || !m->dir[DIR_RESOURCE][0]) return 0;
    uint32_t root=m->dir[DIR_RESOURCE][0];
    if(root>m->size || m->size-root<16) return 0;
    const uint8_t *base=GP(m->base),*p=base+root;
    uint32_t n=(uint32_t)rd16(p+12)+rd16(p+14);
    if(n>(m->size-root-16)/8) return 0;
    for(uint32_t i=0;i<n;i++) {
        const uint8_t *e=p+16+i*8;uint32_t id;memcpy(&id,e,4);if(id!=14) continue;
        uint32_t offset;memcpy(&offset,e+4,4);offset&=0x7FFFFFFF;
        if(offset>m->size-root || m->size-root-offset<24) return 0;
        const uint8_t *dir=base+root+offset;
        if(!rd16(dir+12) && !rd16(dir+14)) return 0;
        memcpy(&id,dir+16,4);
        if(!(id&0x80000000)) return icon_load(inst,id,1);
        uint32_t name=id&0x7FFFFFFF;
        if(name>m->size-root || m->size-root-name<2) return 0;
        int len=rd16(base+root+name);
        if((uint32_t)len>(m->size-root-name-2)/2) return 0;
        uint16_t *s=ui_wdup((const uint16_t *)(base+root+name+2),len);
        gp_t g=ui_guest(s,1);free(s);HICON h=icon_load(inst,g,1);gfree(g);return h;
    }
    return 0;
}
HICON icon_default(HINSTANCE inst)
{
    static struct {HINSTANCE inst;HICON icon;} cache[ICON_MAX];
    int slot=-1;
    for(int i=0;i<ICON_MAX;i++) {
        if(icon_get(cache[i].icon)) {if(cache[i].inst==inst) return cache[i].icon;}
        else if(slot<0) slot=i;
    }
    HICON h=icon_find_default(inst);
    if(h && slot>=0) {cache[slot].inst=inst;cache[slot].icon=h;}
    return h;
}
void icon_send(uint32_t id,HICON h)
{
    struct image *im=icon_get(h); if(!im) return;
    uint32_t pixels[32*32];
    for(int y=0;y<32;y++) for(int x=0;x<32;x++) pixels[y*32+x]=im->px[(y*im->h/32)*im->w+x*im->w/32];
    gui_icon(id,pixels);
}
DEF(DrawIconEx)
{
    struct image *im=icon_get(A(3));
    int width=A(4) ? (int)A(4):32,height=A(5) ? (int)A(5):32;
    if(width<=0 || height<=0 || width>4096 || height>4096) return 0;
    HBRUSH bg=A(7); if(bg) { RECT r={(int)A(1),(int)A(2),(int)A(1)+width,(int)A(2)+height}; gdi_fill_rect(A(0),&r,bg); }
    if(!im) {
        ui_fill(A(0),A(1),A(2),width,height,sys_color(15));
        ui_frame(A(0),A(1),A(2),width,height,sys_color(16)); return 1;
    }
    for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
        uint32_t c=im->px[(y*im->h/height)*im->w+x*im->w/width]; int alpha=c>>24;
        if(!alpha) continue;
        if(alpha<255) { COLORREF old=G(GetPixel,A(0),A(1)+x,A(2)+y); uint32_t dst=colorref_rgb(old),out=0;
            for(int shift=0;shift<24;shift+=8) out|=((((c>>shift)&255)*alpha+((dst>>shift)&255)*(255-alpha))/255)<<shift;
            c=out;
        }
        G(SetPixel,A(0),A(1)+x,A(2)+y,colorref_rgb(c));
    }
    return 1;
}
DEF(DrawIcon) { uint32_t args[]={A(0),A(1),A(2),A(3),32,32,0,0,3}; return api_DrawIconEx(args); }
DEF(CopyIcon)
{
    struct image *im=icon_get(A(0)); if(!im) return A(0);
    for(int i=0;i<ICON_MAX;i++) if(!icons[i]) {
        struct image *copy=malloc(sizeof(*copy)); if(!copy) return 0;
        *copy=*im; copy->px=malloc((size_t)im->w*im->h*4); if(!copy->px) { free(copy); return 0; }
        memcpy(copy->px,im->px,(size_t)im->w*im->h*4); icons[i]=copy; return ICON_BASE+i;
    }
    return 0;
}
uint32_t ui_icon_destroy(HICON h) { struct image *im=icon_get(h); if(im) { image_free(im); icons[h-ICON_BASE]=NULL; } return 1; }
const struct api_entry icon_entries[]={API(DrawIconEx,9),API(DrawIcon,4),API(CopyIcon,1)};
const int icon_count=sizeof(icon_entries)/sizeof(icon_entries[0]);
