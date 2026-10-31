#include "ui.h"
uint32_t ui_call(struct api_dll *d,const char *name,uint32_t *args)
{
    const struct api_entry *e=api_find(d,name);
    return e ? e->fn(args) : 0;
}
gp_t ui_memory(const void *p,size_t n)
{
    gp_t g=galloc(n ? n : 1);
    if(n) memcpy(GP(g),p,n);
    return g;
}
void ui_fill(HDC dc,int x,int y,int w,int h,COLORREF color)
{
    if(w<=0 || h<=0) return;
    RECT r={x,y,x+w,y+h};
    HBRUSH b=G(CreateSolidBrush,color);
    gdi_fill_rect(dc,&r,b);
    G(DeleteObject,b);
}
void ui_frame(HDC dc,int x,int y,int w,int h,COLORREF c)
{
    ui_fill(dc,x,y,w,1,c); ui_fill(dc,x,y+h-1,w,1,c);
    ui_fill(dc,x,y,1,h,c); ui_fill(dc,x+w-1,y,1,h,c);
}
void ui_label(HDC dc,const uint16_t *s,RECT r,uint32_t format,COLORREF color)
{
    gp_t gs=ui_guest(s ? s : (const uint16_t *)u"",1),gr=ui_memory(&r,sizeof(r));
    G(SetTextColor,dc,color); G(SetBkMode,dc,1);
    U(DrawTextW,dc,gs,-1,gr,format);
    gfree(gs); gfree(gr);
}
SIZE ui_measure(HDC dc,const uint16_t *s,int n)
{
    if(n<0) n=wstrlen(s);
    gp_t gs=ui_memory(s,(size_t)n*2),sz=galloc(sizeof(SIZE));
    G(GetTextExtentPoint32W,dc,gs,n,sz);
    SIZE out=*(SIZE *)GP(sz);
    gfree(gs); gfree(sz); return out;
}
gp_t ui_resource(HINSTANCE inst,uint32_t type,gp_t name,int wide,uint32_t *size)
{
    struct module *m=inst ? module_by_base(inst) : main_module;
    if(!m) return 0;
    uint16_t *text=name>=0x10000 ? ui_string(name,wide) : NULL;
    gp_t r=pe_find_resource(m,type,NULL,text ? 0 : name,text,size);
    free(text);
    if(r && (r<m->base || *size>m->size || r-m->base>m->size-*size)) return 0;
    return r;
}
