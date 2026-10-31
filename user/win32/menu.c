/* Menus and accelerators have separate generation-tagged handle spaces. */
#include "ui.h"
#define MENU_MAX 512
#define MENU_BASE 0x50000000u
#define ACCEL_BASE 0x60000000u
#define MF_POPUP 0x0010
#define MF_BYPOSITION 0x0400
#define MF_SEPARATOR 0x0800
#define WM_COMMAND 0x0111
struct menu_item { uint32_t flags,id,data; HMENU sub; uint16_t *text; };
struct menu { HMENU handle; struct menu_item *items; int n,cap,popup; };
static struct menu *menus[MENU_MAX];static uint16_t generations[MENU_MAX];
struct accel { uint16_t flags,key,cmd; };
struct accelerators { HANDLE handle; struct accel *items; int n; };
static struct accelerators *accels[MENU_MAX];static uint16_t accel_gen[MENU_MAX];
struct tracking { HWND hwnd,owner; HMENU menu; int selected,done,result; struct tracking *previous; };
static struct tracking *tracking;
static struct menu *menu_get(HMENU h)
{
    unsigned i=(h&0xFFFF);if(i>=MENU_MAX) return NULL;
    struct menu *m=menus[i];return m && m->handle==h ? m:NULL;
}
static HMENU menu_new(int popup)
{
    for(int i=0;i<MENU_MAX;i++) if(!menus[i]) {
        struct menu *m=calloc(1,sizeof(*m));if(!m) return 0;
        generations[i]=(generations[i]%0xFFF)+1;
        m->handle=MENU_BASE|((uint32_t)generations[i]<<16)|i;m->popup=popup;menus[i]=m;return m->handle;
    }
    return 0;
}
static struct menu_item *item_get(struct menu *m,uint32_t item,int position,int depth)
{
    if(!m || depth>32) return NULL;
    if(position) return item<(uint32_t)m->n ? &m->items[item]:NULL;
    for(int i=0;i<m->n;i++) {
        if(m->items[i].id==item) return &m->items[i];
        struct menu_item *found=item_get(menu_get(m->items[i].sub),item,0,depth+1);if(found) return found;
    }
    return NULL;
}
static int menu_contains(HMENU root,HMENU find,int depth)
{
    if(root==find) return 1;
    if(depth>32) return 1;
    struct menu *m=menu_get(root);if(!m) return 0;
    for(int i=0;i<m->n;i++) if(m->items[i].sub && menu_contains(m->items[i].sub,find,depth+1)) return 1;
    return 0;
}
static int insert(struct menu *m,int pos,uint32_t flags,uint32_t id,gp_t text,int wide)
{
    if(!m) return 0;
    if((flags&MF_POPUP) && (!menu_get(id) || menu_contains(id,m->handle,0))) return 0;
    uint16_t *s=!(flags&(MF_SEPARATOR|4|0x100)) && text ? ui_string(text,wide):ui_wdup((const uint16_t *)u"",0);
    if(m->n==m->cap) { int cap=m->cap ? m->cap*2:8;struct menu_item *items=realloc(m->items,cap*sizeof(*items));if(!items) { free(s);return 0; }m->items=items;m->cap=cap; }
    if(pos<0 || pos>m->n) pos=m->n;
    memmove(m->items+pos+1,m->items+pos,(m->n-pos)*sizeof(*m->items));
    m->items[pos]=(struct menu_item){flags,id,0,(flags&MF_POPUP) ? id:0,s};m->n++;return 1;
}
static void menu_free(HMENU h,int depth)
{
    struct menu *m=menu_get(h);if(!m || depth>32) return;
    menus[h&0xFFFF]=NULL;
    for(int i=0;i<m->n;i++) { if(m->items[i].sub) menu_free(m->items[i].sub,depth+1);free(m->items[i].text); }
    free(m->items);free(m);
}
DEF(CreateMenu) { (void)a;return menu_new(0); }
DEF(CreatePopupMenu) { (void)a;return menu_new(1); }
DEF(IsMenu) { return menu_get(A(0))!=NULL; }
DEF(DestroyMenu) { if(!menu_get(A(0))) return 0;menu_free(A(0),0);return 1; }
static uint32_t append_menu(uint32_t *a,int wide) { struct menu *m=menu_get(A(0));return insert(m,-1,A(1),A(2),A(3),wide); }
DEF(AppendMenuA) { return append_menu(a,0); } DEF(AppendMenuW) { return append_menu(a,1); }
static uint32_t insert_menu(uint32_t *a,int wide)
{
    struct menu *m=menu_get(A(0));int pos=A(1);
    if(!(A(2)&MF_BYPOSITION) && m) { pos=m->n;for(int i=0;i<m->n;i++) if(m->items[i].id==A(1)) {pos=i;break;} }
    return insert(m,pos,A(2),A(3),A(4),wide);
}
DEF(InsertMenuA) { return insert_menu(a,0); } DEF(InsertMenuW) { return insert_menu(a,1); }
static uint32_t remove_menu(uint32_t *a,int destroy)
{
    struct menu *m=menu_get(A(0));if(!m) return 0;
    struct menu_item *it=item_get(m,A(1),A(2)&MF_BYPOSITION,0);if(!it) return 0;
    /* Find the containing menu when addressing a submenu item by command. */
    for(int k=0;k<MENU_MAX;k++) if(menus[k]) for(int i=0;i<menus[k]->n;i++) if(&menus[k]->items[i]==it) {
        struct menu *parent=menus[k];HMENU sub=it->sub;free(it->text);
        memmove(it,it+1,(parent->n-i-1)*sizeof(*it));parent->n--;
        if(destroy && sub) menu_free(sub,0);
        return 1;
    }
    return 0;
}
DEF(RemoveMenu) { return remove_menu(a,0); } DEF(DeleteMenu) { return remove_menu(a,1); }
static uint32_t modify_menu(uint32_t *a,int wide)
{
    struct menu_item *it=item_get(menu_get(A(0)),A(1),A(2)&MF_BYPOSITION,0);if(!it) return 0;
    if((A(2)&MF_POPUP) && (!menu_get(A(3)) || menu_contains(A(3),A(0),0))) return 0;
    uint16_t *s=A(4) && !(A(2)&MF_SEPARATOR) ? ui_string(A(4),wide):ui_wdup((const uint16_t *)u"",0);
    free(it->text);it->text=s;it->flags=A(2);it->id=A(3);it->sub=A(2)&MF_POPUP ? A(3):0;return 1;
}
DEF(ModifyMenuA) { return modify_menu(a,0); } DEF(ModifyMenuW) { return modify_menu(a,1); }
DEF(GetMenuItemCount) { struct menu *m=menu_get(A(0));return m ? m->n:-1; }
DEF(GetMenuItemID) { struct menu_item *it=item_get(menu_get(A(0)),A(1),1,0);return it && !it->sub ? it->id:(uint32_t)-1; }
DEF(GetSubMenu) { struct menu_item *it=item_get(menu_get(A(0)),A(1),1,0);return it ? it->sub:0; }
DEF(GetMenuState) { struct menu_item *it=item_get(menu_get(A(0)),A(1),A(2)&MF_BYPOSITION,0);return it ? it->flags:(uint32_t)-1; }
DEF(EnableMenuItem) { struct menu_item *it=item_get(menu_get(A(0)),A(1),A(2)&MF_BYPOSITION,0);if(!it) return -1;uint32_t old=it->flags&3;it->flags=(it->flags&~3u)|(A(2)&3);return old; }
DEF(CheckMenuItem) { struct menu_item *it=item_get(menu_get(A(0)),A(1),A(2)&MF_BYPOSITION,0);if(!it) return -1;uint32_t old=it->flags&8;it->flags=(it->flags&~8u)|(A(2)&8);return old; }
DEF(CheckMenuRadioItem)
{
    struct menu *m=menu_get(A(0));if(!m) return 0;
    for(uint32_t i=A(1);i<=A(2) && i-A(1)<65536;i++) {struct menu_item *it=item_get(m,i,A(4)&MF_BYPOSITION,0);if(it) it->flags=(it->flags&~8u)|(i==A(3) ? 8:0);}
    return 1;
}
static uint32_t get_menu_string(uint32_t *a,int wide)
{
    struct menu_item *it=item_get(menu_get(A(0)),A(1),A(4)&MF_BYPOSITION,0);if(!it) return 0;
    int n=wstrlen(it->text);if(!A(2) || !A(3)) return n;if(n>=(int)A(3)) n=A(3)-1;
    if(wide) {memcpy(P(2),it->text,n*2);((uint16_t *)P(2))[n]=0;}else {wide_to_ansi(it->text,n,P(2),n);((char *)P(2))[n]=0;}return n;
}
DEF(GetMenuStringW) { return get_menu_string(a,1); } DEF(GetMenuStringA) { return get_menu_string(a,0); }
/* MENUITEMINFO, including the Win95 structure without hbmpItem (44 bytes). */
struct menuiteminfo {uint32_t size,mask,type,state,id,sub,checked,unchecked,data,text,count,bitmap;};
static uint32_t menu_info(uint32_t *a,int wide,int set,int add)
{
    struct menuiteminfo *info=P(3);
    if(!info || info->size<44) return 0;
    struct menu *m=menu_get(A(0));if(!m) return 0;
    if((set || add) && (info->mask&4) && info->sub && (!menu_get(info->sub) || menu_contains(info->sub,A(0),0))) return 0;
    struct menu_item *it=item_get(m,A(1),A(2)!=0,0);
    if(add) {
        int pos=A(2) ? (int)A(1):m->n;
        if(!A(2) && it) for(int i=0;i<m->n;i++) if(&m->items[i]==it) pos=i;
        if(!insert(m,pos,0,0,0,wide)) return 0;
        if(pos<0 || pos>=m->n) pos=m->n-1;
        it=&m->items[pos];
    }
    if(!it) return 0;
    uint32_t mask=info->mask;
    if(set || add) {
        if((mask&4) && info->sub && (!menu_get(info->sub) || menu_contains(info->sub,A(0),0))) return 0;
        if(mask&(0x10|0x40)) {
            uint16_t *s=info->text && !(info->type&(MF_SEPARATOR|4|0x100)) ? ui_string(info->text,wide):ui_wdup((const uint16_t *)u"",0);
            free(it->text);it->text=s;
        }
        if(mask&(0x10|0x100)) it->flags=(it->flags&0x0B)|(info->type&~0x0Bu);
        if(mask&1) it->flags=(it->flags&~0x0Bu)|(info->state&0x0B);
        if(mask&2) it->id=info->id;
        if(mask&4) {it->sub=info->sub;it->flags=(it->flags&~MF_POPUP)|(it->sub ? MF_POPUP:0);}
        if(mask&0x20) it->data=info->data;
        return 1;
    }
    if(mask&1) info->state=it->flags&0x0B;
    if(mask&(0x10|0x100)) info->type=it->flags&~0x0Bu;
    if(mask&2) info->id=it->id;
    if(mask&4) info->sub=it->sub;
    if(mask&0x20) info->data=it->data;
    if(mask&(0x10|0x40)) {
        int n=wstrlen(it->text),cap=info->count;
        if(info->text && cap>0) {
            if(n>=cap) n=cap-1;
            if(wide) {memcpy(GP(info->text),it->text,n*2);((uint16_t *)GP(info->text))[n]=0;}
            else {wide_to_ansi(it->text,n,GP(info->text),n);((char *)GP(info->text))[n]=0;}
        }
        info->count=n;
    }
    return 1;
}
DEF(GetMenuItemInfoA) {return menu_info(a,0,0,0);} DEF(GetMenuItemInfoW) {return menu_info(a,1,0,0);}
DEF(SetMenuItemInfoA) {return menu_info(a,0,1,0);} DEF(SetMenuItemInfoW) {return menu_info(a,1,1,0);}
DEF(InsertMenuItemA) {return menu_info(a,0,1,1);} DEF(InsertMenuItemW) {return menu_info(a,1,1,1);}
int menu_height(HMENU h) { return menu_get(h) ? 20:0; }
DEF(SetMenu)
{
    struct win *w=ui_win(A(0));if(!w || (w->style&WS_CHILD) || (A(1) && !menu_get(A(1)))) return 0;
    w->menu=A(1);ui_menu_changed(A(0),menu_height(A(1)));return 1;
}
DEF(GetMenu) { struct win *w=ui_win(A(0));return w ? w->menu:0; }
DEF(DrawMenuBar) { if(!ui_win(A(0))) return 0;menu_draw(A(0));return 1; }
void menu_destroy_window(HWND h) {struct win *w=ui_win(h);if(w && w->menu_height && w->menu) menu_free(w->menu,0);}
static void menu_text(const uint16_t *text,char *out,int cap)
{
    uint16_t clean[512];int n=0;
    for(int i=0;text && text[i] && n<511;i++) {if(text[i]=='&' && text[i+1] && text[i+1]!='&') continue;clean[n++]=text[i];if(text[i]=='&' && text[i+1]=='&') i++;}
    clean[n]=0;int len=wide_to_utf8(clean,n,out,cap-1);out[len]=0;
}
void menu_draw(HWND h)
{
    struct win *w=ui_win(h);if(!w || !w->menu_height || !w->surf.px) return;
    struct menu *m=menu_get(w->menu);if(!m) return;
    struct surface *s=&w->surf;gfx_clip(s,mkrect(0,0,s->w,20));gfx_fill(s,0,0,s->w,20,C_FACE);
    int x=4;
    for(int i=0;i<m->n;i++) {char text[2048];menu_text(m->items[i].text,text,sizeof(text));int width=gfx_text_width(&font_sans,text)+16;gfx_text(s,&font_sans,x+8,2,text,m->items[i].flags&3 ? C_SHADOW:C_BLACK);x+=width;}
    gfx_hline(s,0,19,s->w,C_SHADOW);gfx_noclip(s);win_dirty(h,mkrect(0,0,s->w,20));
}
static int popup_item(HWND h,int y)
{
    struct win *w=ui_win(h);struct menu *m=w ? menu_get(w->menu):NULL;if(!m) return -1;
    for(int i=0,at=2;i<m->n;i++) {int height=m->items[i].flags&MF_SEPARATOR ? 8:22;if(y>=at && y<at+height) return i;at+=height;}return -1;
}
static int popup_track(HMENU menu,HWND owner,int x,int y,uint32_t flags);
static void choose(struct tracking *t)
{
    struct menu *m=menu_get(t->menu);if(!m || t->selected<0 || t->selected>=m->n) return;
    struct menu_item item=m->items[t->selected];if(item.flags&(3|MF_SEPARATOR)) return;
    if(item.sub) {
        struct win *w=ui_win(t->hwnd);if(!w) return;
        int y=w->wr.top+2;for(int i=0;i<t->selected;i++) y+=(m->items[i].flags&MF_SEPARATOR) ? 8:22;
        int result=popup_track(item.sub,t->owner,w->wr.right,y,0x100|0x80);
        if(result) {t->result=result;t->done=1;}else if(ui_win(t->hwnd)) U(SetCapture,t->hwnd);
    } else {t->result=item.id;t->done=1;}
}
DEF(MenuPopupProc)
{
    HWND h=A(0);struct win *w=ui_win(h);if(!w) return 0;
    struct tracking *t=tracking;while(t && t->hwnd!=h) t=t->previous;
    if(A(1)==WM_NCCREATE) return ui_default(h,A(1),A(2),A(3),1);
    if(!t) return ui_default(h,A(1),A(2),A(3),1);
    struct menu *m=menu_get(t->menu);if(!m) {t->done=1;return 0;}
    if(A(1)==WM_PAINT) {
        struct paintstruct ps;HDC dc=ui_begin(h,&ps);ui_fill(dc,0,0,w->cw,w->ch,sys_color(4));ui_frame(dc,0,0,w->cw,w->ch,sys_color(16));
        for(int i=0,y=2;i<m->n;i++) {
            struct menu_item *it=&m->items[i];int height=it->flags&MF_SEPARATOR ? 8:22;
            if(it->flags&MF_SEPARATOR) ui_fill(dc,4,y+3,w->cw-8,1,sys_color(16));
            else {
                if(i==t->selected) ui_fill(dc,2,y,w->cw-4,height,sys_color(13));
                COLORREF color=sys_color(it->flags&3 ? 17:i==t->selected ? 14:7);
                ui_label(dc,it->text,(RECT){24,y+2,w->cw-20,y+height},0x20,color);
                if(it->flags&8) ui_label(dc,(const uint16_t *)u"*",(RECT){5,y+2,22,y+height},0x20,color);
                if(it->sub) ui_label(dc,(const uint16_t *)u">",(RECT){w->cw-16,y+2,w->cw-2,y+height},0x20,color);
            }y+=height;
        }
        ui_end(h,&ps);return 0;
    }
    if(A(1)==WM_MOUSEMOVE || A(1)==WM_LBUTTONDOWN || A(1)==0x0202) {
        int x=(int16_t)A(3),y=(int16_t)(A(3)>>16);
        if(x<0 || y<0 || x>=w->cw || y>=w->ch) {if(A(1)==WM_LBUTTONDOWN) t->done=1;return 0;}
        int sel=popup_item(h,y);if(t->selected!=sel) {t->selected=sel;ui_invalidate(h);if(sel>=0) ui_send(t->owner,0x011F,(uint16_t)m->items[sel].id|(m->items[sel].flags<<16),t->menu,1);}
        if(A(1)==0x0202) choose(t);
        return 0;
    }
    if(A(1)==WM_KEYDOWN || A(1)==WM_SYSKEYDOWN) {
        if(A(2)==VK_ESCAPE || A(2)==0x25) t->done=1;
        else if(A(2)==VK_RETURN || A(2)==0x27) choose(t);
        else if(A(2)==0x26 || A(2)==0x28) {int dir=A(2)==0x26 ? -1:1;for(int n=0;n<m->n;n++) {t->selected=(t->selected+dir+m->n)%m->n;if(!(m->items[t->selected].flags&(3|MF_SEPARATOR))) break;}ui_invalidate(h);}
        return 0;
    }
    if(A(1)==WM_CHAR || A(1)==WM_SYSCHAR) {
        for(int i=0;i<m->n;i++) for(int j=0;m->items[i].text[j];j++) if(m->items[i].text[j]=='&' && (m->items[i].text[j+1]|32)==(A(2)|32)) {t->selected=i;choose(t);return 0;}
        return 0;
    }
    if(A(1)==WM_KILLFOCUS) {
        struct win *next=ui_win(A(2));
        if(!next || !next->cls || wstrcasecmp(next->cls->name,(const uint16_t *)u"#MenuPopup")) t->done=1;
        return 0;
    }
    if(A(1)==WM_CLOSE || A(1)==WM_DESTROY) {t->done=1;return 0;}
    return ui_default(h,A(1),A(2),A(3),1);
}
static int popup_track(HMENU handle,HWND owner,int x,int y,uint32_t flags)
{
    struct menu *m=menu_get(handle);if(!m || !ui_win(owner)) return 0;
    ui_send(owner,0x0117,handle,0,1);m=menu_get(handle);if(!m || !ui_win(owner)) return 0;
    HDC dc=gdi_window_dc(0);int width=60,height=4;
    for(int i=0;i<m->n;i++) {int w=ui_measure(dc,m->items[i].text,-1).cx+48;if(w>width) width=w;height+=(m->items[i].flags&MF_SEPARATOR) ? 8:22;}
    gdi_release_dc(dc);if(flags&8) x-=width;else if(flags&4) x-=width/2;
    if(x+width>gui_screen_w()) x=gui_screen_w()-width;
    if(y+height>gui_screen_h()) y=gui_screen_h()-height;
    if(x<0) x=0;
    if(y<0) y=0;
    gp_t cls=ui_guest((const uint16_t *)u"#MenuPopup",1);uint32_t ca[]={0x80,cls,0,WS_POPUP,x,y,width,height,owner,0,0,0};
    HWND h=ui_create(ca,1);gfree(cls);if(!h) return 0;
    struct tracking t={h,owner,handle,-1,0,0,tracking};tracking=&t;ui_win(h)->menu=handle;
    HWND oldfocus=U(GetFocus);U(ShowWindow,h,5);U(SetFocus,h);U(SetCapture,h);ui_invalidate(h);
    while(ui_win(h) && ui_win(owner) && !t.done) {
        struct msg msg;ui_next(&msg,1);
        if(msg.message==WM_QUIT) {U(PostQuitMessage,msg.wparam);break;}
        gp_t gm=ui_memory(&msg,sizeof(msg));U(TranslateMessage,gm);gfree(gm);ui_dispatch(&msg);
    }
    if(U(GetCapture)==h) U(ReleaseCapture);
    if(ui_win(h)) U(DestroyWindow,h);
    tracking=t.previous;
    if(ui_win(owner)) {U(SetForegroundWindow,owner);if(ui_win(oldfocus)) U(SetFocus,oldfocus);}
    if(t.result && !(flags&(0x100|0x80)) && ui_win(owner)) ui_send(owner,WM_COMMAND,t.result,0,1);
    return flags&0x100 ? t.result:t.result!=0;
}
DEF(TrackPopupMenu) { return popup_track(A(0),A(5),A(2),A(3),A(1)); }
DEF(TrackPopupMenuEx) { return popup_track(A(0),A(4),A(2),A(3),A(1)); }
DEF(EndMenu) { (void)a;if(tracking) tracking->done=1;return tracking!=NULL; }
int menu_mouse(HWND h,UINT msg,int x,int y)
{
    struct win *w=ui_win(h);struct menu *m=w ? menu_get(w->menu):NULL;
    if(!w || !m || y<0 || y>=w->menu_height) return 0;
    if(msg!=WM_LBUTTONDOWN) return 1;
    for(int i=0,left=4;i<m->n;i++) {
        char text[2048];menu_text(m->items[i].text,text,sizeof(text));int width=gfx_text_width(&font_sans,text)+16;
        if(x>=left && x<left+width) {
            struct menu_item item=m->items[i];if(item.flags&3) return 1;
            HMENU bar=w->menu;
            ui_send(h,0x0211,0,0,1);
            if(!ui_win(h)) return 1;
            ui_send(h,0x0116,bar,0,1);
            if(!ui_win(h)) return 1;
            if(item.sub) {POINT p={left,0};gp_t gp=ui_memory(&p,sizeof(p));U(ClientToScreen,h,gp);p=*(POINT *)GP(gp);gfree(gp);int id=popup_track(item.sub,h,p.x,p.y,0x100);if(id && ui_win(h)) ui_send(h,WM_COMMAND,id,0,1);}
            else ui_send(h,WM_COMMAND,item.id,0,1);
            if(ui_win(h)) ui_send(h,0x0212,0,0,1);
            return 1;
        }left+=width;
    }return 1;
}
int menu_key(HWND h,UINT msg,WPARAM key)
{
    struct win *w=ui_win(h);struct menu *m=w ? menu_get(w->menu):NULL;if(!m || !w->menu_height) return 0;
    if(msg==WM_SYSCHAR) for(int i=0,left=4;i<m->n;i++) {
        const uint16_t *s=m->items[i].text;
        for(int j=0;s[j];j++) if(s[j]=='&' && (s[j+1]|32)==(key|32)) return menu_mouse(h,WM_LBUTTONDOWN,left+1,1);
        char text[2048];menu_text(s,text,sizeof(text));left+=gfx_text_width(&font_sans,text)+16;
    }
    if((msg==WM_KEYDOWN || msg==WM_SYSKEYDOWN) && key==VK_F10) return menu_mouse(h,WM_LBUTTONDOWN,5,1);
    return 0;
}
/* Bounded menu resource reader. */
struct mr { const uint8_t *p,*end;int bad; };
static unsigned m16(struct mr *r) {if(r->end-r->p<2) {r->bad=1;return 0;}unsigned v=r->p[0]|r->p[1]<<8;r->p+=2;return v;}
static uint32_t m32(struct mr *r) {uint32_t a=m16(r);return a|(uint32_t)m16(r)<<16;}
static void ma(struct mr *r) {size_t pad=(-(uintptr_t)r->p)&3;if((size_t)(r->end-r->p)<pad) r->bad=1;else r->p+=pad;}
static HMENU parse_menu(struct mr *r,int extended,int popup,int depth)
{
    if(depth>32) {r->bad=1;return 0;}HMENU h=menu_new(popup);if(!h) {r->bad=1;return 0;}
    int ended=0;
    for(int n=0;n<4096 && !r->bad && !ended;n++) {
        uint32_t flags,id=0,resinfo;
        if(extended) {ma(r);flags=m32(r);flags|=m32(r);id=m32(r);resinfo=m16(r);if(resinfo&1) flags|=MF_POPUP;}
        else {flags=m16(r);resinfo=flags;if(!(flags&MF_POPUP)) id=m16(r);}
        const uint8_t *start=r->p;while(!r->bad && m16(r)) {}
        if(r->bad) break;
        gp_t text=ui_memory(start,r->p-start);
        ended=(resinfo&0x80)!=0;
        if(flags&MF_POPUP) {if(extended) {ma(r);m32(r);}id=parse_menu(r,extended,1,depth+1);}
        if(!r->bad && !insert(menu_get(h),-1,flags&~0x80u,id,text,1)) r->bad=1;
        gfree(text);
    }
    if(!ended) r->bad=1;
    if(r->bad) {menu_free(h,0);return 0;}return h;
}
static HMENU load_menu(gp_t data,uint32_t size)
{
    if(!data || size<4) return 0;
    struct mr r={GP(data),(uint8_t *)GP(data)+size,0};unsigned version=m16(&r),offset=m16(&r);
    if(version>1 || offset>(unsigned)(r.end-r.p)) return 0;
    r.p+=offset;return parse_menu(&r,version==1,0,0);
}
DEF(LoadMenuW) {uint32_t n=0;gp_t r=ui_resource(A(0),4,A(1),1,&n);return load_menu(r,n);}
DEF(LoadMenuA) {uint32_t n=0;gp_t r=ui_resource(A(0),4,A(1),0,&n);return load_menu(r,n);}
DEF(LoadMenuIndirectW) {uint32_t n=0;while(n<65536 && vm_readable(A(0)+n,1)) n++;return load_menu(A(0),n);}
DEF(LoadMenuIndirectA) {return api_LoadMenuIndirectW(a);}
static struct accelerators *accel_get(HANDLE h) {unsigned i=h&0xFFFF;struct accelerators *t=i<MENU_MAX ? accels[i]:NULL;return t && t->handle==h ? t:NULL;}
static HANDLE accel_create(const uint8_t *data,int n,int resource)
{
    if(n<=0 || n>65536) return 0;
    for(int i=0;i<MENU_MAX;i++) if(!accels[i]) {
        struct accelerators *t=calloc(1,sizeof(*t));if(!t) return 0;t->items=calloc(n,sizeof(*t->items));if(!t->items) {free(t);return 0;}
        accel_gen[i]=accel_gen[i]%0xFFF+1;t->handle=ACCEL_BASE|(uint32_t)accel_gen[i]<<16|i;t->n=n;
        for(int j=0;j<n;j++) {const uint8_t *p=data+j*(resource ? 8:6);t->items[j]=(struct accel){p[0]&0x7F,p[2]|p[3]<<8,p[4]|p[5]<<8};}
        accels[i]=t;return t->handle;
    }return 0;
}
DEF(CreateAcceleratorTableW) {return vm_readable(A(0),A(1)*6) ? accel_create(P(0),A(1),0):0;}
DEF(CreateAcceleratorTableA) {return api_CreateAcceleratorTableW(a);}
DEF(DestroyAcceleratorTable) {struct accelerators *t=accel_get(A(0));if(!t) return 0;accels[A(0)&0xFFFF]=NULL;free(t->items);free(t);return 1;}
DEF(CopyAcceleratorTableW)
{
    struct accelerators *t=accel_get(A(0));if(!t) return 0;if(!A(1)) return t->n;int n=A(2)<(uint32_t)t->n ? (int)A(2):t->n;
    for(int i=0;i<n;i++) {uint8_t *p=(uint8_t *)P(1)+i*6;memset(p,0,6);p[0]=t->items[i].flags;memcpy(p+2,&t->items[i].key,2);memcpy(p+4,&t->items[i].cmd,2);}return n;
}
DEF(CopyAcceleratorTableA) {return api_CopyAcceleratorTableW(a);}
static uint32_t load_accel(uint32_t *a,int wide)
{
    uint32_t size=0;gp_t data=ui_resource(A(0),9,A(1),wide,&size);if(!data) return 0;return accel_create(GP(data),size/8,1);
}
DEF(LoadAcceleratorsA) {return load_accel(a,0);} DEF(LoadAcceleratorsW) {return load_accel(a,1);}
DEF(TranslateAcceleratorW)
{
    struct accelerators *t=accel_get(A(1));struct msg *m=P(2);if(!t || !ui_win(A(0)) || !U(IsWindowEnabled,A(0))) return 0;
    int mods=((int16_t)U(GetKeyState,VK_SHIFT)<0 ? 4:0)|((int16_t)U(GetKeyState,VK_CONTROL)<0 ? 8:0)|((int16_t)U(GetKeyState,VK_MENU)<0 ? 16:0);
    for(int i=0;i<t->n;i++) {struct accel *e=&t->items[i];if(e->key!=m->wparam) continue;
        if(e->flags&1) {if(m->message!=WM_KEYDOWN && m->message!=WM_SYSKEYDOWN) continue;if((e->flags&28)!=mods) continue;}
        else if(m->message!=WM_CHAR && m->message!=WM_SYSCHAR) continue;
        ui_send(A(0),WM_COMMAND,e->cmd|0x10000,0,1);return 1;
    }return 0;
}
DEF(TranslateAcceleratorA) {return api_TranslateAcceleratorW(a);}
const struct api_entry menu_entries[]={
    API(MenuPopupProc,4),API(CreateMenu,0),API(CreatePopupMenu,0),API(IsMenu,1),API(DestroyMenu,1),
    API(AppendMenuA,4),API(AppendMenuW,4),API(InsertMenuA,5),API(InsertMenuW,5),API(RemoveMenu,3),API(DeleteMenu,3),API(ModifyMenuA,5),API(ModifyMenuW,5),
    API(GetMenuItemCount,1),API(GetMenuItemID,2),API(GetSubMenu,2),API(GetMenuState,3),API(EnableMenuItem,3),API(CheckMenuItem,3),API(CheckMenuRadioItem,5),
    API(GetMenuItemInfoA,4),API(GetMenuItemInfoW,4),API(SetMenuItemInfoA,4),API(SetMenuItemInfoW,4),API(InsertMenuItemA,4),API(InsertMenuItemW,4),
    API(GetMenuStringA,5),API(GetMenuStringW,5),API(SetMenu,2),API(GetMenu,1),API(DrawMenuBar,1),API(TrackPopupMenu,7),API(TrackPopupMenuEx,6),API(EndMenu,0),
    API(LoadMenuA,2),API(LoadMenuW,2),API(LoadMenuIndirectA,1),API(LoadMenuIndirectW,1),
    API(CreateAcceleratorTableA,2),API(CreateAcceleratorTableW,2),API(DestroyAcceleratorTable,1),API(CopyAcceleratorTableA,3),API(CopyAcceleratorTableW,3),
    API(LoadAcceleratorsA,2),API(LoadAcceleratorsW,2),API(TranslateAcceleratorA,3),API(TranslateAcceleratorW,3),
};
const int menu_count=sizeof(menu_entries)/sizeof(menu_entries[0]);
void menu_init(void) {ui_class("#MenuPopup",&menu_entries[0],5);}
