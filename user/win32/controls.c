/* Standard USER32 classes. The procedures are guest-callable thunks, so subclassing
 * and CallWindowProc use the same path as application window procedures. */
#include "ui.h"
#define WM_COMMAND 0x0111
#define WM_GETDLGCODE 0x0087
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_LBUTTONUP 0x0202
#define WM_COPY 0x0301
#define WM_CUT 0x0300
#define WM_PASTE 0x0302
#define WM_CLEAR 0x0303
#define WM_UNDO 0x0304
#define EM_GETSEL 0x00B0
#define EM_SETSEL 0x00B1
#define EM_REPLACESEL 0x00C2
#define LB_ADDSTRING 0x0180
#define CB_ADDSTRING 0x0143
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115

enum { BUTTON=1, STATIC, EDIT, LISTBOX, COMBOBOX, SCROLLBAR };
struct control {
    int kind, check, pressed, anchor, caret, first, left, limit, modified, readonly;
    int selecting, sel, nitems, cap, dropped, drop_height, changing;
    HWND dropdown, entry;
    uint32_t *item_data;
    uint8_t *selected_items;
    uint16_t **items, *undo;
    int undo_anchor, undo_caret;
    HFONT font;
    HICON image;
    int min,max,pos,page,track,drag_offset,arrows;
};
static const uint16_t *empty=(const uint16_t *)u"";
static struct control *state(HWND h) { struct win *w=ui_win(h); return w ? w->control : NULL; }
void controls_destroy(struct win *w)
{
    struct control *c=w->control;
    if(!c) return;
    for(int i=0;i<c->nitems;i++) free(c->items[i]);
    free(c->items); free(c->item_data); free(c->selected_items); free(c->undo); free(c); w->control=NULL;
}
static uint32_t word(int lo,int hi) { return (uint16_t)lo | (uint32_t)(uint16_t)hi<<16; }
static void notify(HWND h,int code)
{
    struct win *w=ui_win(h);
    if(w && (w->parent || w->owner)) ui_send(w->parent ? w->parent:w->owner,WM_COMMAND,word(w->id,code),h,1);
}
static HDC control_dc(HWND h)
{
    HDC dc=gdi_window_dc(h);
    struct control *c=state(h);
    if(c && c->font) G(SelectObject,dc,c->font);
    return dc;
}
static int line_height(HDC dc) { SIZE z=ui_measure(dc,(const uint16_t *)u"Mg",2); return z.cy>0 ? z.cy : 16; }
static int text_len(HWND h) { struct win *w=ui_win(h); return w && w->text ? wstrlen(w->text) : 0; }
static const uint16_t *text_of(HWND h) { struct win *w=ui_win(h); return w && w->text ? w->text : empty; }
static int line_start(const uint16_t *s,int pos)
{
    while(pos>0 && s[pos-1]!='\n') pos--;
    return pos;
}
static int line_end(const uint16_t *s,int pos)
{
    while(s[pos] && s[pos]!='\r' && s[pos]!='\n') pos++;
    return pos;
}
static int line_number(const uint16_t *s,int pos) { int n=0; for(int i=0;i<pos && s[i];i++) n+=s[i]=='\n'; return n; }
static int line_index(const uint16_t *s,int line)
{
    int i=0; while(s[i] && line>0) if(s[i++]=='\n') line--;
    return line ? -1 : i;
}
static void edit_scroll_info(HWND h,HDC dc)
{
    struct win *w=ui_win(h);struct control *c=state(h);if(!w || !c) return;
    const uint16_t *s=text_of(h);int n=text_len(h),lh=line_height(dc),maxwidth=0;
    for(int pos=0;pos<=n;) {int end=line_end(s,pos),width=ui_measure(dc,s+pos,end-pos).cx;if(width>maxwidth) maxwidth=width;if(!s[end]) break;pos=end;if(s[pos]=='\r') pos++;if(s[pos]=='\n') pos++;}
    int vertical[]={28,7,0,line_number(s,n),w->ch/lh,c->first,0};
    int horizontal[]={28,7,0,maxwidth,w->cw,c->left,0};
    if(w->vscroll) {gp_t p=ui_memory(vertical,sizeof(vertical));ui_send(w->vscroll,0xE9,1,p,1);gfree(p);}
    if(w->hscroll) {gp_t p=ui_memory(horizontal,sizeof(horizontal));ui_send(w->hscroll,0xE9,1,p,1);gfree(p);}
}
static void edit_visible(HWND h)
{
    struct control *c=state(h); struct win *w=ui_win(h);
    if(!c || !w) return;
    HDC dc=control_dc(h); int lh=line_height(dc),ln=line_number(text_of(h),c->caret);
    int rows=(w->ch-4)/lh; if(rows<1) rows=1;
    if(ln<c->first) c->first=ln;
    if(ln>=c->first+rows) c->first=ln-rows+1;
    const uint16_t *s=text_of(h); int start=line_start(s,c->caret);
    int x=ui_measure(dc,s+start,c->caret-start).cx;
    if(x<c->left) c->left=x;
    if(x>c->left+w->cw-8) c->left=x-w->cw+8;
    if(c->left<0) c->left=0;
    edit_scroll_info(h,dc);
    gdi_release_dc(dc); ui_invalidate(h);
}
static void edit_changed(HWND h)
{
    edit_visible(h); notify(h,0x400); /* EN_UPDATE */
    if(ui_win(h)) notify(h,0x300); /* EN_CHANGE */
}
static void edit_replace(HWND h,const uint16_t *insert,int undoable)
{
    struct control *c=state(h); struct win *w=ui_win(h);
    if(!c || !w || c->readonly || (w->style & WS_DISABLED)) return;
    int n=text_len(h),lo=c->anchor<c->caret ? c->anchor:c->caret,hi=c->anchor>c->caret ? c->anchor:c->caret;
    int add=wstrlen(insert);
    if(add>c->limit-(n-(hi-lo))) add=c->limit-(n-(hi-lo));
    if(add<0) add=0;
    if(undoable) { free(c->undo); c->undo=ui_wdup(text_of(h),-1); c->undo_anchor=c->anchor; c->undo_caret=c->caret; }
    else { free(c->undo); c->undo=NULL; }
    uint16_t *s=malloc((size_t)(n-hi+lo+add+1)*2);
    if(!s) return;
    memcpy(s,text_of(h),lo*2); memcpy(s+lo,insert,add*2);
    memcpy(s+lo+add,text_of(h)+hi,(n-hi+1)*2);
    ui_text(h,s); free(s); c->caret=c->anchor=lo+add; c->modified=1;
    edit_changed(h);
}
static void edit_undo(HWND h)
{
    struct control *c=state(h); if(!c || !c->undo || c->readonly) return;
    uint16_t *s=c->undo; int a=c->undo_anchor,p=c->undo_caret;
    c->undo=ui_wdup(text_of(h),-1); c->undo_anchor=c->anchor; c->undo_caret=c->caret;
    ui_text(h,s); free(s); c->anchor=a; c->caret=p; c->modified=1; edit_changed(h);
}
static void edit_copy(HWND h)
{
    struct control *c=state(h); struct win *w=ui_win(h);
    if(!c || (w->style & 0x20)) return; /* ES_PASSWORD */
    int lo=c->anchor<c->caret ? c->anchor:c->caret,hi=c->anchor>c->caret ? c->anchor:c->caret;
    if(lo==hi) return;
    uint16_t *s=ui_wdup(text_of(h)+lo,hi-lo); ui_clip_set(s); free(s);
}
static int edit_hit(HWND h,int x,int y)
{
    struct control *c=state(h); HDC dc=control_dc(h); int lh=line_height(dc);
    const uint16_t *s=text_of(h); int line=c->first+(y>2 ? (y-2)/lh : 0);
    int start=line_index(s,line); if(start<0) start=text_len(h);
    int end=line_end(s,start),pos=start,px=2-c->left;
    while(pos<end) { int advance=ui_measure(dc,s+pos,1).cx; if(x<px+advance/2) break; px+=advance; pos++; }
    gdi_release_dc(dc); return pos;
}
static void edit_draw(HWND h,HDC dc)
{
    struct win *w=ui_win(h); struct control *c=state(h);
    int lh=line_height(dc),lo=c->anchor<c->caret ? c->anchor:c->caret,hi=c->anchor>c->caret ? c->anchor:c->caret;
    ui_fill(dc,0,0,w->cw,w->ch,sys_color(c->readonly ? 15 : 5));
    if((w->style & 0x00800000) || (w->exstyle & 0x200)) ui_frame(dc,0,0,w->cw,w->ch,sys_color(16));
    const uint16_t *s=text_of(h); int p=line_index(s,c->first),y=2;
    if(p<0) return;
    while(y<w->ch && p<=text_len(h)) {
        int end=line_end(s,p),x=2-c->left;
        for(int i=p;i<end;i++) {
            uint16_t ch[2]={ (w->style & 0x20) ? '*' : s[i],0 };
            int width=ui_measure(dc,ch,1).cx;
            int selected=i>=lo && i<hi;
            if(selected) ui_fill(dc,x,y,width,lh,sys_color(13));
            ui_label(dc,ch,(RECT){x,y,x+width,y+lh},0x100|0x20,sys_color(selected ? 14 : (w->style & WS_DISABLED) ? 17 : 8));
            if(i==c->caret && U(GetFocus)==h) ui_fill(dc,x,y,1,lh,sys_color(8));
            x+=width;
        }
        if(c->caret==end && U(GetFocus)==h) ui_fill(dc,x,y,1,lh,sys_color(8));
        if(!s[end]) break;
        p=end; if(s[p]=='\r') p++; if(s[p]=='\n') p++;
        y+=lh;
        if(!(w->style & 4)) break;
    }
}
static void button_click(HWND h)
{
    struct win *w=ui_win(h); struct control *c=state(h);
    if(!w || !c || (w->style & WS_DISABLED)) return;
    int kind=w->style & 15;
    if(kind==3 || kind==6) c->check=(c->check+1)%(kind==6 ? 3:2);
    if(kind==9) { /* BS_AUTORADIOBUTTON: only the WS_GROUP-delimited group. */
        struct win *first=w;
        while(!(first->style & 0x20000) && first->next_sibling) {
            struct win *prev=ui_win(first->next_sibling); if(!prev) break; first=prev;
        }
        for(struct win *s=first;s;s=ui_win(s->prev_sibling)) {
            if(s!=first && (s->style & 0x20000)) break;
            struct control *r=s->control;
            if(r && r->kind==BUTTON && ((s->style & 15)==4 || (s->style & 15)==9)) {
                r->check=s==w; ui_invalidate(s->hwnd);
            }
        }
        c->check=1;
    }
    ui_invalidate(h); notify(h,0);
}
static void button_draw(HWND h,HDC dc)
{
    struct win *w=ui_win(h); struct control *c=state(h); int kind=w->style&15;
    COLORREF face=sys_color(15),edge=sys_color(c->pressed ? 16:20),fg=sys_color(w->style & WS_DISABLED ? 17:18);
    ui_fill(dc,0,0,w->cw,w->ch,face);
    int tx=4;
    if(kind==7) { /* group box */
        ui_frame(dc,0,7,w->cw,w->ch-7,sys_color(16));
        int tw=ui_measure(dc,text_of(h),-1).cx+8; ui_fill(dc,6,0,tw,16,face); tx=10;
    } else if(kind==2 || kind==3 || kind==4 || kind==5 || kind==6 || kind==9) {
        int y=(w->ch-13)/2;
        ui_fill(dc,2,y,13,13,sys_color(5)); ui_frame(dc,2,y,13,13,sys_color(16));
        if(c->check) {
            if(kind==4 || kind==9) ui_fill(dc,6,y+4,5,5,sys_color(8));
            else { ui_fill(dc,5,y+5,3,5,sys_color(8)); ui_fill(dc,8,y+3,4,3,sys_color(8)); }
        }
        tx=20;
    } else {
        ui_frame(dc,0,0,w->cw,w->ch,sys_color(16));
        ui_fill(dc,1,1,w->cw-2,1,edge); ui_fill(dc,1,1,1,w->ch-2,edge);
        if(kind==1) ui_frame(dc,1,1,w->cw-2,w->ch-2,sys_color(8));
    }
    RECT r={tx+(c->pressed ? 1:0),kind==7 ? 0:0,w->cw-4,w->ch};
    ui_label(dc,text_of(h),r,0x20|0x4|(tx==4 ? 1:0),fg);
    if(U(GetFocus)==h && kind!=7) ui_frame(dc,tx-1,3,w->cw-tx-3,w->ch-6,sys_color(16));
}
static void list_draw(HWND h,HDC dc)
{
    struct win *w=ui_win(h); struct control *c=state(h); int lh=line_height(dc)+2;
    ui_fill(dc,0,0,w->cw,w->ch,sys_color(5)); ui_frame(dc,0,0,w->cw,w->ch,sys_color(16));
    if(c->kind==COMBOBOX && !c->dropped && (w->style&3)!=1) {
        if(c->sel>=0 && c->sel<c->nitems) ui_label(dc,c->items[c->sel],(RECT){3,1,w->cw-20,lh+1},0x24|0x100,sys_color(8));
        ui_fill(dc,w->cw-18,1,17,lh,sys_color(15));
        ui_label(dc,(const uint16_t *)u"v",(RECT){w->cw-18,1,w->cw-1,lh+1},0x25,sys_color(8)); return;
    }
    for(int i=c->first,y=(c->kind==COMBOBOX && (w->style&3)==1) ? lh+6:1;i<c->nitems && y<w->ch;i++,y+=lh) {
        int selected=(w->style&(8|0x800)) ? c->selected_items[i]:i==c->sel;
        if(selected) ui_fill(dc,1,y,w->cw-2,lh,sys_color(13));
        ui_label(dc,c->items[i],(RECT){3,y,w->cw-2,y+lh},0x20|0x100,sys_color(selected ? 14:8));
    }
}
static int prefix_equal(const uint16_t *s,const uint16_t *key)
{
    for(int i=0;key[i];i++) if(!s[i] || ((s[i]|32)!=(key[i]|32))) return 0;
    return 1;
}
static int list_add(HWND h,int index,gp_t value)
{
    struct control *c=state(h); struct win *w=ui_win(h); if(!c) return -1;
    uint16_t *s=ui_string(value,1);
    if(c->nitems==c->cap) {
        int cap=c->cap ? c->cap*2:16;
        uint16_t **items=malloc((size_t)cap*sizeof(*items));
        uint32_t *data=calloc(cap,sizeof(*data));uint8_t *selected=calloc(cap,1);
        if(!items || !data || !selected) {free(items);free(data);free(selected);free(s);return -2;}
        if(c->nitems) {memcpy(items,c->items,c->nitems*sizeof(*items));memcpy(data,c->item_data,c->nitems*sizeof(*data));memcpy(selected,c->selected_items,c->nitems);}
        free(c->items);free(c->item_data);free(c->selected_items);
        c->items=items;c->item_data=data;c->selected_items=selected;c->cap=cap;
    }
    if(index<0 || index>c->nitems) index=c->nitems;
    if(w->style & (c->kind==LISTBOX ? 2:0x100)) {
        index=0; while(index<c->nitems && wstrcasecmp(c->items[index],s)<=0) index++;
    }
    memmove(c->items+index+1,c->items+index,(size_t)(c->nitems-index)*sizeof(*c->items));
    memmove(c->item_data+index+1,c->item_data+index,(c->nitems-index)*sizeof(*c->item_data));
    memmove(c->selected_items+index+1,c->selected_items+index,c->nitems-index);
    c->item_data[index]=0;c->selected_items[index]=0;
    c->items[index]=s; c->nitems++; if(c->sel>=index) c->sel++;
    ui_invalidate(h); return index;
}
static void combo_select(HWND h,int selection,int close,int send)
{
    struct control *c=state(h);if(!c) return;
    c->sel=selection;
    const uint16_t *text=selection>=0 && selection<c->nitems ? c->items[selection]:empty;
    ui_text(h,text);
    if(c->entry) {gp_t g=ui_guest(text,1);c->changing=1;ui_send(c->entry,WM_SETTEXT,0,g,1);gfree(g);c=state(h);if(c) c->changing=0;}
    c=state(h);if(!c) return;
    HWND list=c->dropdown;
    if(close) {
        c->dropped=0;c->dropdown=0;
        if(U(GetCapture)==list) U(ReleaseCapture);
        if(ui_win(list)) U(DestroyWindow,list);
        if(ui_win(h)) U(SetFocus,c->entry ? c->entry:h);
    }
    ui_invalidate(h);
    if(send && ui_win(h)) notify(h,1);
    if(close && ui_win(h)) notify(h,8);
}
static void combo_drop(HWND h,int show)
{
    struct win *w=ui_win(h);struct control *c=state(h);if(!w || !c || c->dropped==show || (w->style&3)==1) return;
    if(!show) {combo_select(h,c->sel,1,0);return;}
    notify(h,7);w=ui_win(h);c=state(h);if(!w || !c) return;
    POINT p={0,w->ch};gp_t point=ui_memory(&p,sizeof(p));U(ClientToScreen,h,point);p=*(POINT *)GP(point);gfree(point);
    gp_t cls=ui_guest((const uint16_t *)u"LISTBOX",1);
    int height=c->drop_height>24 ? c->drop_height:120;
    if(p.y+height>gui_screen_h()) p.y-=height+w->ch;
    uint32_t ca[]={0x80,cls,0,WS_POPUP|0x00800000,p.x,p.y,w->cw,height,h,0,w->inst,0};
    HWND list=ui_create(ca,1);gfree(cls);if(!list) return;
    c=state(h);if(!c) {U(DestroyWindow,list);return;}
    c->dropdown=list;c->dropped=1;
    for(int i=0;i<c->nitems;i++) {gp_t g=ui_guest(c->items[i],1);ui_send(list,LB_ADDSTRING,0,g,1);gfree(g);}
    ui_send(list,0x0186,c->sel,0,1);ui_send(list,WM_SETFONT,c->font,0,1);
    U(ShowWindow,list,5);U(SetFocus,list);U(SetCapture,list);ui_invalidate(h);
}
static int scroll_limit(struct control *c)
{
    int max=c->max-(c->page>0 ? c->page-1:0); return max<c->min ? c->min:max;
}
static void scroll_clamp(struct control *c)
{
    if(c->max<c->min) c->max=c->min;
    int64_t range=(int64_t)c->max-c->min+1;
    if((uint32_t)c->page>range) c->page=range>0x7FFFFFFF ? 0x7FFFFFFF:range;
    if(c->pos<c->min) c->pos=c->min;
    if(c->pos>scroll_limit(c)) c->pos=scroll_limit(c);
    if(!c->selecting) c->track=c->pos;
}
struct scroll_geometry {int vertical,len,thick,start,size,travel;};
static struct scroll_geometry scroll_geometry(struct win *w,struct control *c)
{
    struct scroll_geometry g={0};g.vertical=(w->style&1)!=0;
    g.len=g.vertical ? w->ch:w->cw;g.thick=g.vertical ? w->cw:w->ch;
    int track=g.len-2*g.thick;if(track<0) track=0;
    int64_t range=(int64_t)c->max-c->min+1;
    g.size=range>0 && c->page>0 ? (int)((int64_t)track*c->page/range):g.thick;
    if(g.size<8) g.size=8;
    if(g.size>track) g.size=track;
    g.travel=track-g.size;
    int limit=scroll_limit(c);
    g.start=g.thick+(limit>c->min ? (int)((int64_t)(c->pos-c->min)*g.travel/(limit-c->min)):0);
    return g;
}
static void scroll_draw(HWND h,HDC dc)
{
    struct win *w=ui_win(h);struct control *c=state(h);struct scroll_geometry g=scroll_geometry(w,c);
    ui_fill(dc,0,0,w->cw,w->ch,sys_color(15));ui_frame(dc,0,0,w->cw,w->ch,sys_color(16));
    RECT first={0,0,g.vertical ? g.thick:g.thick,g.thick};
    RECT last=g.vertical ? (RECT){0,g.len-g.thick,g.thick,g.len}:(RECT){g.len-g.thick,0,g.len,g.thick};
    if(g.vertical) {
        ui_frame(dc,0,0,g.thick,g.thick,sys_color(16));ui_frame(dc,0,g.len-g.thick,g.thick,g.thick,sys_color(16));
        ui_frame(dc,0,g.start,g.thick,g.size,sys_color(16));
    } else {
        ui_frame(dc,0,0,g.thick,g.thick,sys_color(16));ui_frame(dc,g.len-g.thick,0,g.thick,g.thick,sys_color(16));
        ui_frame(dc,g.start,0,g.size,g.thick,sys_color(16));
    }
    ui_label(dc,(const uint16_t *)(g.vertical ? u"^":u"<"),first,0x25,sys_color(c->arrows&1 ? 17:18));
    ui_label(dc,(const uint16_t *)(g.vertical ? u"v":u">"),last,0x25,sys_color(c->arrows&2 ? 17:18));
}
static LRESULT control_proc(HWND h,UINT m,WPARAM wp,LPARAM lp)
{
    struct win *w=ui_win(h); if(!w) return 0;
    struct control *c=w->control;
    if(m==WM_NCCREATE) {
        c=calloc(1,sizeof(*c)); if(!c) return 0;
        const char *names[]={"BUTTON","STATIC","EDIT","LISTBOX","COMBOBOX","SCROLLBAR"};
        for(int i=0;i<6;i++) { uint16_t wide[20]; ansi_to_wide(names[i],-1,wide,20); if(!wstrcasecmp(w->cls->name,wide)) c->kind=i+1; }
        c->limit=0x7FFFFFFE; c->sel=-1; c->max=100; c->readonly=(w->style & 0x800)!=0;
        w->control=c; return ui_default(h,m,wp,lp,1);
    }
    if(!c) return ui_default(h,m,wp,lp,1);
    if(m==WM_CREATE && c->kind==COMBOBOX) {
        HDC dc=control_dc(h);int height=line_height(dc)+6;gdi_release_dc(dc);
        c->drop_height=w->ch-height;
        if((w->style&3)!=1) U(MoveWindow,h,w->wr.left,w->wr.top,w->cw,height,0);
        if((w->style&3)!=3) {
            gp_t name=ui_guest((const uint16_t *)u"EDIT",1),initial=ui_guest(text_of(h),1);
            uint32_t ca[]={0,name,initial,WS_CHILD|WS_VISIBLE,2,2,w->cw-22,height-4,h,0,w->inst,0};
            c->entry=ui_create(ca,1);gfree(name);gfree(initial);
        }
        return 0;
    }
    if(m==WM_CREATE && c->kind==STATIC && (w->style&31)==3) {
        const struct createstruct *cs=GP(lp);if(cs && cs->name) c->image=icon_load(cs->inst,cs->name,1);return 0;
    }
    if(c->kind==COMBOBOX && m==WM_COMMAND && c->entry && (HWND)lp==c->entry) {
        if(c->changing) return 0;
        if((wp>>16)==0x300) {ui_text(h,text_of(c->entry));notify(h,5);}
        else if((wp>>16)==0x400) notify(h,6);
        return 0;
    }

    if(m==WM_SETFONT) { c->font=wp; if(c->entry) ui_send(c->entry,m,wp,lp,1); if(lp) ui_invalidate(h); return 0; }
    if(m==WM_GETFONT) return c->font;
    if(c->kind==COMBOBOX && m==WM_SIZE && c->entry) {U(MoveWindow,c->entry,2,2,w->cw-22,20,0);ui_invalidate(h);return 0;}
    if(m==WM_GETDLGCODE) return c->kind==EDIT ? 0x0089 : c->kind==BUTTON ? ((w->style&15)==1 ? 0x2010:((w->style&15)==4 || (w->style&15)==9) ? 0x2040:0x2000) : c->kind==STATIC ? 0x100 : 1;
    if(m==WM_SETTEXT) {
        LRESULT r=ui_default(h,m,wp,lp,1);
        if(c->kind==COMBOBOX && c->entry) {ui_send(c->entry,m,wp,lp,1);return r;}
        if(c->kind==EDIT) { c->anchor=c->caret=c->first=c->left=0; free(c->undo); c->undo=NULL; c->modified=0; edit_changed(h);return r; }
        ui_invalidate(h); return r;
    }
    if(m==WM_PAINT) {
        struct paintstruct ps; HDC dc=ui_begin(h,&ps);
        if(!(w=ui_win(h)) || !(c=state(h))) {ui_end(h,&ps);return 0;}
        if(c->font) G(SelectObject,dc,c->font);
        if(c->kind==BUTTON) button_draw(h,dc);
        else if(c->kind==EDIT) {edit_scroll_info(h,dc);edit_draw(h,dc);}
        else if(c->kind==LISTBOX || c->kind==COMBOBOX) list_draw(h,dc);
        else if(c->kind==SCROLLBAR) scroll_draw(h,dc);
        else {
            ui_fill(dc,0,0,w->cw,w->ch,sys_color(15));
            int kind=w->style&31;
            if(kind>=4 && kind<=9) {
                COLORREF col=sys_color(kind==4 || kind==7 ? 6 : kind==5 || kind==8 ? 16:5);
                if(kind>=7) ui_frame(dc,0,0,w->cw,w->ch,col); else ui_fill(dc,0,0,w->cw,w->ch,col);
            } else if(kind==3) U(DrawIconEx,dc,0,0,c->image,w->cw,w->ch,0,0,3);
            else ui_label(dc,text_of(h),(RECT){0,0,w->cw,w->ch},(kind==1 ? 1:kind==2 ? 2:0)|(kind==12 ? 0x20:0x10),sys_color(w->style & WS_DISABLED ? 17:18));
        }
        ui_end(h,&ps); return 0;
    }
    if(m==WM_ERASEBKGND) return 1;
    if(m==WM_KILLFOCUS && c->kind==LISTBOX && w->owner) {
        struct control *combo=state(w->owner);
        if(combo && combo->kind==COMBOBOX && combo->dropdown==h) {
            HWND owner=w->owner;combo->dropdown=0;combo->dropped=0;
            if(U(GetCapture)==h) U(ReleaseCapture);
            if(ui_win(h)) U(DestroyWindow,h);
            ui_invalidate(owner);if(ui_win(owner)) notify(owner,8);return 0;
        }
    }
    if(m==WM_SETFOCUS || m==WM_KILLFOCUS || m==0x000A || m==WM_SIZE) {
        ui_invalidate(h);
        if(c->kind==EDIT && (m==WM_SETFOCUS || m==WM_KILLFOCUS)) notify(h,m==WM_SETFOCUS ? 0x100:0x200);
        return 0;
    }
    if(c->kind==BUTTON) {
        switch(m) {
        case 0x00F0: return c->check;
        case 0x00F1: c->check=wp; ui_invalidate(h); return 0;
        case 0x00F2: return c->check | (c->pressed ? 4:0) | (U(GetFocus)==h ? 8:0);
        case 0x00F3: c->pressed=wp!=0; ui_invalidate(h); return 0;
        case 0x00F4: w->style=(w->style&~15u)|(wp&15); if(lp) ui_invalidate(h); return 0;
        case 0x00F5: button_click(h); return 0;
        case 0x00F6: return c->image;
        case 0x00F7: { HICON old=c->image; c->image=lp; ui_invalidate(h); return old; }
        case WM_LBUTTONDOWN: if((w->style&15)==7) return 0; U(SetFocus,h); if(!state(h)) return 0; U(SetCapture,h); if(!(c=state(h))) return 0; c->pressed=1; ui_invalidate(h); return 0;
        case WM_LBUTTONUP: {
            int fire=c->pressed && (int16_t)lp>=0 && (int16_t)(lp>>16)>=0 && (int16_t)lp<w->cw && (int16_t)(lp>>16)<w->ch;
            c->pressed=0; U(ReleaseCapture); ui_invalidate(h); if(fire) button_click(h); return 0;
        }
        case WM_CAPTURECHANGED: c->pressed=0; ui_invalidate(h); return 0;
        case WM_KEYDOWN: if(wp==VK_SPACE) { c->pressed=1; ui_invalidate(h); } return 0;
        case WM_KEYUP: if(wp==VK_SPACE && c->pressed) { c->pressed=0; button_click(h); } return 0;
        }
    }
    if(c->kind==STATIC) {
        if(m==0x0170 || m==0x0172) { HICON old=c->image; c->image=m==0x170 ? wp:lp; ui_invalidate(h); return old; }
        if(m==0x0171 || m==0x0173) return c->image;
        if(m==WM_LBUTTONDOWN && (w->style&0x100)) { notify(h,0); return 0; }
    }
    if(c->kind==EDIT) {
        int n=text_len(h); const uint16_t *s=text_of(h);
        switch(m) {
        case EM_GETSEL: if(wp) *(uint32_t *)GP(wp)=c->anchor<c->caret ? c->anchor:c->caret; if(lp) *(uint32_t *)GP(lp)=c->anchor>c->caret ? c->anchor:c->caret; return word(c->anchor<c->caret ? c->anchor:c->caret,c->anchor>c->caret ? c->anchor:c->caret);
        case EM_SETSEL: if((int32_t)wp==-1) c->anchor=c->caret; else { c->anchor=wp>(uint32_t)n ? n:(int)wp; c->caret=(int32_t)lp<0 || lp>(uint32_t)n ? n:(int)lp; } edit_visible(h); return 0;
        case EM_REPLACESEL: edit_replace(h,lp ? GP(lp):empty,wp!=0); return 1;
        case 0x00C5: c->limit=wp ? (wp>0x7FFFFFFEu ? 0x7FFFFFFE:wp):0x7FFFFFFE; return 0;
        case 0x00D5: return c->limit;
        case 0x00B8: return c->modified;
        case 0x00B9: c->modified=wp!=0; return 0;
        case 0x00BA: return line_number(s,n)+1;
        case 0x00BB: return line_index(s,(int32_t)wp<0 ? line_number(s,c->caret):(int)wp);
        case 0x00C9: return line_number(s,(int32_t)wp<0 ? c->caret:(int)wp);
        case 0x00C1: { int pos=(int32_t)wp<0 ? c->caret : wp>(uint32_t)n ? n:(int)wp; int start=line_start(s,pos); return line_end(s,start)-start; }
        case 0x00C4: { int start=line_index(s,wp); if(start<0 || !lp) return 0; int len=line_end(s,start)-start,cap=*(uint16_t *)GP(lp); if(len>cap) len=cap; memcpy(GP(lp),s+start,len*2); return len; }
        case 0x00CE: return c->first;
        case 0x00B6: c->first+=(int32_t)lp; if(c->first<0) c->first=0; c->left+=(int32_t)wp*8; if(c->left<0) c->left=0; ui_invalidate(h); return 1;
        case 0x00B7: edit_visible(h); return 0;
        case 0x00CF: c->readonly=wp!=0; ui_invalidate(h); return 1;
        case 0x00C6: return c->undo!=NULL;
        case 0x00CD: free(c->undo); c->undo=NULL; return 0;
        case 0x00C7: case WM_UNDO: edit_undo(h); return 1;
        case WM_COPY: edit_copy(h); return 0;
        case WM_CUT: if(!c->readonly) { edit_copy(h); edit_replace(h,empty,1); } return 0;
        case WM_CLEAR: edit_replace(h,empty,1); return 0;
        case WM_PASTE: { uint16_t *p=ui_clip_get(); edit_replace(h,p,1); free(p); return 0; }
        case WM_LBUTTONDOWN: { int pos=edit_hit(h,(int16_t)lp,(int16_t)(lp>>16)); c->caret=pos; if(!(wp&MK_SHIFT)) c->anchor=pos; c->selecting=1; U(SetCapture,h); U(SetFocus,h); if(ui_win(h)) edit_visible(h); return 0; }
        case WM_MOUSEMOVE: if(c->selecting) { c->caret=edit_hit(h,(int16_t)lp,(int16_t)(lp>>16)); edit_visible(h); } return 0;
        case WM_LBUTTONUP: c->selecting=0; U(ReleaseCapture); return 0;
        case WM_CAPTURECHANGED: c->selecting=0; return 0;
        case WM_KEYDOWN: {
            int ctrl=(int16_t)U(GetKeyState,VK_CONTROL)<0,shift=(int16_t)U(GetKeyState,VK_SHIFT)<0,pos=c->caret;
            if(ctrl) {
                if(wp=='A') { c->anchor=0; c->caret=n; edit_visible(h); return 0; }
                if(wp=='C') { edit_copy(h); return 0; }
                if(wp=='X') { ui_send(h,WM_CUT,0,0,1); return 0; }
                if(wp=='V') { ui_send(h,WM_PASTE,0,0,1); return 0; }
                if(wp=='Z') { edit_undo(h); return 0; }
            }
            if(wp==0x25) { if(pos>0) pos--; if(pos>0 && s[pos]=='\n' && s[pos-1]=='\r') pos--; }
            else if(wp==0x27) { if(pos<n) pos++; if(pos<n && s[pos-1]=='\r' && s[pos]=='\n') pos++; }
            else if(wp==0x24) pos=ctrl ? 0:line_start(s,pos);
            else if(wp==0x23) pos=ctrl ? n:line_end(s,pos);
            else if(wp==0x26 || wp==0x28) { int col=pos-line_start(s,pos),line=line_number(s,pos)+(wp==0x26 ? -1:1); int start=line<0 ? 0:line_index(s,line); if(start>=0) { int end=line_end(s,start); pos=start+col<end ? start+col:end; } }
            else if(wp==0x2E) { if(c->anchor==c->caret && c->caret<n) { c->caret++; if(c->caret<n && s[c->caret-1]=='\r' && s[c->caret]=='\n') c->caret++; } edit_replace(h,empty,1); return 0; }
            else return 0;
            c->caret=pos; if(!shift) c->anchor=pos; edit_visible(h); return 0;
        }
        case WM_CHAR:
            if((int16_t)U(GetKeyState,VK_CONTROL)<0) return 0;
            if(wp==VK_BACK) { if(c->anchor==c->caret && c->caret>0) { c->anchor--; if(c->anchor>0 && s[c->anchor]=='\n' && s[c->anchor-1]=='\r') c->anchor--; } edit_replace(h,empty,1); }
            else if(wp==VK_RETURN && (w->style&4)) edit_replace(h,(const uint16_t *)u"\r\n",1);
            else if(wp>=32 || (wp==VK_TAB && (w->style&4))) { uint16_t ch[2]={wp,0}; if((w->style&0x2000) && (wp<'0' || wp>'9')) return 0; edit_replace(h,ch,1); }
            return 0;
        case WM_VSCROLL: {
            int code=wp&0xFFFF,rows=8;
            if(code==0) c->first--;else if(code==1) c->first++;
            else if(code==2) c->first-=rows;else if(code==3) c->first+=rows;
            else if(code==4 || code==5) c->first=wp>>16;
            else if(code==6) c->first=0;else if(code==7) c->first=line_number(s,n);
            if(c->first<0) c->first=0;
            if(c->first>line_number(s,n)) c->first=line_number(s,n);
            ui_invalidate(h);return 0;
        }
        case WM_HSCROLL: {
            int code=wp&0xFFFF;
            if(code==0) c->left-=8;else if(code==1) c->left+=8;
            else if(code==2) c->left-=w->cw;else if(code==3) c->left+=w->cw;
            else if(code==4 || code==5) c->left=wp>>16;
            if(c->left<0) c->left=0;
            ui_invalidate(h);return 0;
        }
        case WM_MOUSEWHEEL: c->first-=(int16_t)(wp>>16)/120*3; if(c->first<0) c->first=0; ui_invalidate(h); return 0;
        }
    }
    if(c->kind==LISTBOX || c->kind==COMBOBOX) {
        int cb=c->kind==COMBOBOX;
        if(m==(cb ? CB_ADDSTRING:LB_ADDSTRING)) return list_add(h,-1,lp);
        if(m==(cb ? 0x014A:0x0181)) return list_add(h,(int)wp,lp);
        if(m==(cb ? 0x0144:0x0182)) {
            if(wp>=(uint32_t)c->nitems) return -1;
            free(c->items[wp]); memmove(c->items+wp,c->items+wp+1,(c->nitems-wp-1)*sizeof(*c->items)); memmove(c->item_data+wp,c->item_data+wp+1,(c->nitems-wp-1)*sizeof(*c->item_data));
            memmove(c->selected_items+wp,c->selected_items+wp+1,c->nitems-wp-1);c->nitems--;
            if(c->sel==(int)wp) c->sel=-1; else if(c->sel>(int)wp) c->sel--; ui_invalidate(h); return c->nitems;
        }
        if(m==(cb ? 0x014B:0x0184)) { for(int i=0;i<c->nitems;i++) free(c->items[i]); c->nitems=0; c->sel=-1; c->first=0; ui_invalidate(h); return 0; }
        if(m==(cb ? 0x0146:0x018B)) return c->nitems;
        if(m==(cb ? 0x0147:0x0188)) return c->sel;
        if(m==(cb ? 0x014E:0x0186)) { int sel=(int)wp; if(sel<0 || sel>=c->nitems) sel=-1; if(cb) combo_select(h,sel,0,0);else {c->sel=sel;ui_invalidate(h);} return sel; }
        if(m==(cb ? 0x0150:0x0199)) return wp<(uint32_t)c->nitems ? c->item_data[wp]:(uint32_t)-1;
        if(m==(cb ? 0x0151:0x019A)) {if(wp>=(uint32_t)c->nitems) return -1;c->item_data[wp]=lp;return 0;}
        if(!cb && m==0x0185) {if((int32_t)lp==-1) memset(c->selected_items,wp!=0,c->nitems);else if(lp<(uint32_t)c->nitems) c->selected_items[lp]=wp!=0;else return -1;ui_invalidate(h);return 0;}
        if(!cb && m==0x0187) return wp<(uint32_t)c->nitems ? c->selected_items[wp]:(uint32_t)-1;
        if(!cb && (m==0x0190 || m==0x0191)) {int count=0;for(int i=0;i<c->nitems;i++) if(c->selected_items[i]) {if(m==0x191 && count<(int)wp) ((int *)GP(lp))[count]=i;count++;}return m==0x191 && count>(int)wp ? wp:(uint32_t)count;}
        if(m==(cb ? 0x0149:0x018A)) return wp<(uint32_t)c->nitems ? (uint32_t)wstrlen(c->items[wp]):(uint32_t)-1;
        if(m==(cb ? 0x0148:0x0189)) { if(wp>=(uint32_t)c->nitems || !lp) return -1; int len=wstrlen(c->items[wp]); memcpy(GP(lp),c->items[wp],(len+1)*2); return len; }
        if(m==(cb ? 0x0154:0x01A1)) { HDC dc=control_dc(h);int height=line_height(dc)+2;gdi_release_dc(dc);return height; }
        if(m==(cb ? 0x015C:0x0197)) {if(wp>=(uint32_t)c->nitems) return -1;c->first=wp;ui_invalidate(h);return 0;}
        if(m==(cb ? 0x015B:0x018E)) return c->first;
        if(m==(cb ? 0x014D:0x018C) || m==(cb ? 0x014C:0x018F) || m==(cb ? 0x0158:0x01A2)) {
            const uint16_t *key=lp ? GP(lp):empty; int exact=m==(cb ? 0x0158:0x01A2);
            for(int i=0;i<c->nitems;i++) { int index=((int)wp+1+i)%c->nitems; if(index<0) index=0; int match=exact ? !wstrcasecmp(c->items[index],key) : prefix_equal(c->items[index],key); if(match) {if(m==(cb ? 0x014D:0x018C)) {if(cb) combo_select(h,index,0,0);else {c->sel=index;ui_invalidate(h);}}return index;} }
            return -1;
        }
        if(cb && m==0x0140) return c->entry ? ui_send(c->entry,EM_GETSEL,wp,lp,1):0;
        if(cb && m==0x0141) return c->entry ? ui_send(c->entry,0xC5,wp,0,1):0;
        if(cb && m==0x0142) return c->entry ? ui_send(c->entry,EM_SETSEL,(int16_t)lp,(int16_t)(lp>>16),1):(uint32_t)-1;
        if(cb && m==0x0152) {if(!lp) return 0;U(GetWindowRect,h,lp);((RECT *)GP(lp))->bottom+=c->drop_height>24 ? c->drop_height:120;return 1;}
        if(cb && m==0x014F) { combo_drop(h,wp!=0);return 1; }
        if(cb && m==0x0157) return c->dropped;
        if(m==WM_LBUTTONDOWN || m==WM_KEYDOWN || m==WM_CHAR) {
            int sel=c->sel;
            if(m==WM_LBUTTONDOWN) {
                if(cb && (w->style&3)!=1) { combo_drop(h,!c->dropped);return 0; }
                if(w->owner && state(w->owner) && state(w->owner)->kind==COMBOBOX && ((int16_t)lp<0 || (int16_t)(lp>>16)<0 || (int16_t)lp>=w->cw || (int16_t)(lp>>16)>=w->ch)) {combo_drop(w->owner,0);return 0;}
                HDC dc=control_dc(h); sel=c->first+((int16_t)(lp>>16)-((cb && (w->style&3)==1) ? line_height(dc)+8:1))/(line_height(dc)+2); gdi_release_dc(dc); U(SetFocus,h);
                c=state(h);w=ui_win(h);if(!c || !w) return 0;
            } else if(m==WM_KEYDOWN) {
                if(w->owner && state(w->owner) && state(w->owner)->kind==COMBOBOX) {
                    if(wp==VK_ESCAPE) {combo_drop(w->owner,0);return 0;}
                    if(wp==VK_RETURN) {combo_select(w->owner,c->sel,1,1);return 0;}
                }
                if(cb && (wp==0x73 || (wp==0x28 && (int16_t)U(GetKeyState,VK_MENU)<0))) {combo_drop(h,!c->dropped);return 0;}
                if(wp==0x26) sel--; else if(wp==0x28) sel++; else if(wp==0x24) sel=0; else if(wp==0x23) sel=c->nitems-1; else return 0; }
            else { for(int i=0;i<c->nitems;i++) if((c->items[i][0]|32)==(wp|32)) { sel=i; break; } }
            if(sel<0) sel=0;
            if(sel>=c->nitems) sel=c->nitems-1;
            c->sel=sel;if(sel<c->first) c->first=sel;
            HDC dc=control_dc(h);int rows=w->ch/(line_height(dc)+2);gdi_release_dc(dc);if(rows<1) rows=1;
            if(sel>=c->first+rows) c->first=sel-rows+1;
            if(w->owner && state(w->owner) && state(w->owner)->kind==COMBOBOX) {combo_select(w->owner,sel,m==WM_LBUTTONDOWN,1);return 0;}
            if(cb) {combo_select(h,sel,0,1);return 0;}
            if(sel>=0 && (w->style&(8|0x800))) c->selected_items[sel]=!c->selected_items[sel];
            ui_invalidate(h);notify(h,1);return 0;
        }
        if(m==WM_MOUSEWHEEL) { c->first-=(int16_t)(wp>>16)/120*3; if(c->first<0) c->first=0; ui_invalidate(h); return 0; }
        if(m==0x0203) { notify(h,cb ? 2:2); return 0; }
    }
    if(c->kind==SCROLLBAR) {
        switch(m) {
        case 0x00E0: { int old=c->pos; c->pos=(int)wp; scroll_clamp(c); if(lp) ui_invalidate(h); return old; }
        case 0x00E1: return c->pos;
        case 0x00E4: {int old=c->arrows;c->arrows=wp&3;ui_invalidate(h);return old!=c->arrows;}
        case 0x00E2: case 0x00E6: c->min=(int)wp; c->max=(int)lp; scroll_clamp(c); if(m==0xE6) ui_invalidate(h); return 0;
        case 0x00E3: if(wp) *(int *)GP(wp)=c->min; if(lp) *(int *)GP(lp)=c->max; return 0;
        case 0x00E9: case 0x00EA: {
            uint32_t *si=GP(lp); if(!si || si[0]<28) return 0;
            uint32_t mask=si[1];
            if(m==0xE9) { if(mask&1) { c->min=si[2]; c->max=si[3]; } if(mask&2) c->page=si[4]; if(mask&4) c->pos=si[5]; scroll_clamp(c); if(wp) ui_invalidate(h); return c->pos; }
            if(mask&1) { si[2]=c->min; si[3]=c->max; } if(mask&2) si[4]=c->page; if(mask&4) si[5]=c->pos; if(mask&16) si[6]=c->track;
            return 1;
        }
        case WM_LBUTTONDOWN: case WM_MOUSEMOVE: case WM_LBUTTONUP: {
            struct scroll_geometry g=scroll_geometry(w,c);
            int vertical=g.vertical,at=vertical ? (int16_t)(lp>>16):(int16_t)lp;
            int code;
            if(m==WM_LBUTTONUP) {int dragging=c->selecting;c->selecting=0;U(ReleaseCapture);code=dragging ? 4:8;}
            else if(m==WM_MOUSEMOVE) { if(!c->selecting) return 0; code=5; }
            else if(at<g.thick) {if(c->arrows&1) return 0;code=0;}
            else if(at>=g.len-g.thick) {if(c->arrows&2) return 0;code=1;}
            else if(at<g.start) code=2;
            else if(at>=g.start+g.size) code=3;
            else {c->selecting=1;c->drag_offset=at-g.start;U(SetCapture,h);code=5;}
            c=state(h);w=ui_win(h);if(!c || !w) return 0;
            if(code==5) {c->track=c->min+(g.travel>0 ? (int)((int64_t)(at-g.thick-c->drag_offset)*(scroll_limit(c)-c->min)/g.travel):0); if(c->track<c->min) c->track=c->min; if(c->track>scroll_limit(c)) c->track=scroll_limit(c); }
            ui_send(w->parent,vertical ? WM_VSCROLL:WM_HSCROLL,word(code,c->track),w->nonclient ? 0:h,1); return 0;
        }
        }
    }
    return ui_default(h,m,wp,lp,1);
}
DEF(ControlProcW) { return control_proc(A(0),A(1),A(2),A(3)); }
/* Window scroll bars use the same control implementation, hidden until requested. */
static HWND scroll_window(HWND h,int bar,int create)
{
    struct win *w=ui_win(h); if(!w) return 0;
    if(bar==2) return h;
    HWND *slot=bar==1 ? &w->vscroll:&w->hscroll;
    if(*slot && !ui_win(*slot)) *slot=0;
    if(!*slot && create) {
        gp_t name=ui_guest((const uint16_t *)u"SCROLLBAR",1);
        uint32_t args[]={0,name,0,WS_CHILD|WS_VISIBLE|(bar==1 ? 1:0),bar==1 ? w->cw:0,bar==1 ? 0:w->ch,bar==1 ? 16:w->cw,bar==1 ? w->ch:16,h,0,w->inst,0};
        HWND child=ui_create(args,1);gfree(name);
        w=ui_win(h);if(!w) return 0;
        slot=bar==1 ? &w->vscroll:&w->hscroll;*slot=child;
        struct win *s=ui_win(child);if(s) s->nonclient=1;
    }
    return *slot;
}
void controls_layout(HWND h)
{
    struct win *w=ui_win(h);if(!w) return;
    for(int bar=0;bar<2;bar++) {
        w=ui_win(h);if(!w) return;
        int show=bar ? w->sb_w:w->sb_h;
        HWND child=scroll_window(h,bar,show!=0);
        if(child && (w=ui_win(h))) {
            U(MoveWindow,child,bar ? w->cw:0,bar ? 0:w->ch,bar ? 16:w->cw,bar ? w->ch:16,0);
            U(ShowWindow,child,show ? 5:0);
        }
    }
}
DEF(SetScrollInfo) { HWND h=scroll_window(A(0),A(1),1); return h ? ui_send(h,0xE9,A(3),A(2),1):0; }
DEF(GetScrollInfo) { HWND h=scroll_window(A(0),A(1),0); return h ? ui_send(h,0xEA,0,A(2),1):0; }
DEF(SetScrollPos) { HWND h=scroll_window(A(0),A(1),1); return h ? ui_send(h,0xE0,A(2),A(3),1):0; }
DEF(GetScrollPos) { HWND h=scroll_window(A(0),A(1),0); return h ? ui_send(h,0xE1,0,0,1):0; }
DEF(SetScrollRange) { HWND h=scroll_window(A(0),A(1),1); if(!h) return 0; ui_send(h,A(4) ? 0xE6:0xE2,A(2),A(3),1); return 1; }
DEF(GetScrollRange) { HWND h=scroll_window(A(0),A(1),0); if(!h) return 0; ui_send(h,0xE3,A(2),A(3),1); return 1; }
DEF(ShowScrollBar)
{
    int first=A(1)==3 ? 0:A(1),last=A(1)==3 ? 1:A(1);
    for(int i=first;i<=last;i++) { if(i!=2) ui_scroll_changed(A(0),i==1,A(2)!=0); HWND h=scroll_window(A(0),i,A(2)!=0); if(h) U(ShowWindow,h,A(2) ? 5:0); }
    return 1;
}
DEF(EnableScrollBar)
{
    int first=A(1)==3 ? 0:A(1),last=A(1)==3 ? 1:A(1),changed=0;
    for(int i=first;i<=last;i++) {HWND h=scroll_window(A(0),i,0);if(h) changed|=ui_send(h,0xE4,A(2),0,1);}
    return changed;
}
const struct api_entry controls_entries[]={
    API(ControlProcW,4), API(SetScrollInfo,4), API(GetScrollInfo,3), API(SetScrollPos,4), API(GetScrollPos,2),
    API(SetScrollRange,5), API(GetScrollRange,4), API(ShowScrollBar,3), API(EnableScrollBar,3),
};
const int controls_count=sizeof(controls_entries)/sizeof(controls_entries[0]);
void controls_init(void)
{
    const char *names[]={"BUTTON","STATIC","EDIT","LISTBOX","COMBOBOX","SCROLLBAR"};
    for(int i=0;i<6;i++) ui_class(names[i],&controls_entries[0],0);
}
