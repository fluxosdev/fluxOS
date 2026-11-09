/* Dialog resources are read with a bounded cursor, including DIALOGEX and creation data. */
#include "ui.h"
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_GETDLGCODE 0x0087
#define DM_GETDEFID 0x0400
#define DM_SETDEFID 0x0401
struct dialog { gp_t proc; int wide,ended,modal,result,defid,messagebox,cancelid; uint32_t buttonmask; HWND owner, previous_focus; HFONT font; };
struct reader { const uint8_t *p,*end; int bad; };
static uint16_t get16(struct reader *r)
{
    if(r->end-r->p<2) { r->bad=1; return 0; }
    uint16_t v=r->p[0]|(uint16_t)r->p[1]<<8; r->p+=2; return v;
}
static uint32_t get32(struct reader *r) { uint32_t lo=get16(r); return lo|(uint32_t)get16(r)<<16; }
static void align4(struct reader *r)
{
    uintptr_t p=((uintptr_t)r->p+3)&~3u;
    if(p>(uintptr_t)r->end) r->bad=1; else r->p=(void *)p;
}
static gp_t field(struct reader *r)
{
    const uint8_t *start=r->p; uint16_t first=get16(r);
    if(first==0xFFFF) return get16(r);
    if(!first) return 0;
    while(!r->bad && get16(r)) {}
    if(r->bad) return 0;
    return ui_memory(start,r->p-start);
}
static void free_field(gp_t f) { if(f>=0x10000) gfree(f); }
static LRESULT dlg_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp)
{
    struct win *w=ui_win(h); if(!w) return 0;
    struct dialog *d=w->dialog;
    if(d && d->proc) {
        gp_t proc=d->proc; /* dialog result lives in DWL_MSGRESULT */
        if(w->nextra>=4) memset(w->extra,0,4);
        LRESULT handled=ui_proc(proc,d->wide,h,msg,wp,lp,1);
        if(!ui_win(h)) return handled;
        w=ui_win(h); d=w->dialog;
        if(msg==WM_INITDIALOG || msg==WM_GETDLGCODE || msg==0x0138 || msg==0x0133 || msg==0x0135 || msg==0x0136) return handled;
        if(handled) { uint32_t result=0; if(w->nextra>=4) memcpy(&result,w->extra,4); return result; }
    }
    if(msg==DM_GETDEFID) return d ? ((uint32_t)0x534B<<16)|(uint16_t)d->defid:0;
    if(msg==DM_SETDEFID) { if(d) d->defid=wp; return 1; }
    if(msg==WM_CLOSE) { U(EndDialog,h,2); return 0; }
    if(msg==WM_COMMAND && (wp&0xFFFF)==2 && !(wp>>16)) { U(EndDialog,h,2); return 0; }
    return ui_default(h,msg,wp,lp,1);
}
DEF(DialogProcW) { return dlg_proc(A(0),A(1),A(2),A(3)); }
void dialog_destroy(struct win *w)
{
    struct dialog *d=w->dialog; if(!d) return;
    if(d->font) G(DeleteObject,d->font);
    if(!d->modal) free(d);
    w->dialog=NULL;
}
int dialog_window_long(struct win *w,int index,int set,uint32_t value,uint32_t *result)
{
    struct dialog *d=w->dialog;
    if(!d || index!=4) return 0;
    *result=d->proc;if(set) d->proc=value;return 1;
}
static int dialog_visible(HWND root,struct win *w)
{
    for(int depth=0;w && w->hwnd!=root && depth<64;depth++,w=ui_win(w->parent))
        if(!(w->style&WS_VISIBLE) || (w->style&WS_DISABLED)) return 0;
    return 1;
}
static HWND next_tab(HWND dialog,HWND current,int backwards)
{
    struct win *root=ui_win(dialog); if(!root) return 0;
    HWND list[MAX_WINDOWS]; int n=0,current_index=-1;
    /* Depth-first order is the creation order (reverse of the front-to-back child list). */
    struct win *w=ui_win(root->last_child);
    for(int count=0;w && count<MAX_WINDOWS;count++) {
        if(w->hwnd==current) current_index=n;
        list[n++]=w->hwnd;
        if((w->exstyle&0x10000) && w->last_child) { w=ui_win(w->last_child); continue; }
        while(w && w!=root && !w->prev_sibling) w=ui_win(w->parent);
        if(!w || w==root) break;
        w=ui_win(w->prev_sibling);
    }
    if(!n) return 0;
    if(current_index<0) current_index=backwards ? 0:n-1;
    for(int i=1;i<=n;i++) {
        int index=(current_index+(backwards ? n-i:i))%n;
        w=ui_win(list[index]);
        if(w && (w->style&0x10000) && dialog_visible(dialog,w)) return w->hwnd;
    }
    return 0;
}
static HWND next_group(HWND dialog,HWND current,int backwards)
{
    struct win *w=ui_win(current),*root=ui_win(dialog);
    if(!root) return 0;
    if(!w || !U(IsChild,dialog,current)) w=ui_win(root->last_child);
    if(!w) return 0;
    struct win *first=w;
    while(!(first->style&0x20000) && first->next_sibling) first=ui_win(first->next_sibling);
    HWND list[MAX_WINDOWS];int n=0,index=-1;
    for(struct win *c=first;c && n<MAX_WINDOWS;c=ui_win(c->prev_sibling)) {
        if(c!=first && (c->style&0x20000)) break;
        if(c->hwnd==current) index=n;
        list[n++]=c->hwnd;
    }
    if(index<0) index=0;
    for(int i=1;i<=n;i++) {
        int at=(index+(backwards ? n-i:i))%n;
        struct win *c=ui_win(list[at]);if(c && dialog_visible(dialog,c)) return c->hwnd;
    }
    return current;
}
static int is_dialog_message(HWND h,struct msg *m)
{
    if(m->hwnd!=h && !U(IsChild,h,m->hwnd)) return 0;
    if(m->message==WM_KEYDOWN || m->message==WM_SYSKEYDOWN) {
        uint32_t code=ui_send(m->hwnd,WM_GETDLGCODE,m->wparam,0,1);
        if(code&4) return 0;
        if(m->wparam==VK_TAB && !(code&2)) {
            HWND next=next_tab(h,U(GetFocus),(int16_t)U(GetKeyState,VK_SHIFT)<0);
            if(next) { U(SetFocus,next); if(ui_win(next) && !wstrcasecmp(ui_win(next)->cls->name,(const uint16_t *)u"EDIT")) ui_send(next,0xB1,0,-1,1); }
            return 1;
        }
        if(m->wparam>=0x25 && m->wparam<=0x28 && !(code&1)) {
            HWND next=next_group(h,U(GetFocus),m->wparam==0x25 || m->wparam==0x26);
            if(next) {U(SetFocus,next);uint32_t flags=ui_send(next,WM_GETDLGCODE,0,0,1);if(flags&0x40) ui_send(next,0xF5,0,0,1);}
            return 1;
        }
        if(m->wparam==VK_RETURN || m->wparam==VK_ESCAPE) {
            int id=m->wparam==VK_ESCAPE ? 2:1;
            struct win *w=ui_win(h); struct dialog *d=w ? w->dialog:NULL;
            HWND focus=U(GetFocus);
            struct win *f=ui_win(focus);
            if(m->wparam==VK_RETURN && f && !wstrcasecmp(f->cls->name,(const uint16_t *)u"BUTTON")) id=f->id;
            else if(id==1 && d) id=d->defid;
            HWND button=U(GetDlgItem,h,id);
            if(button && U(IsWindowEnabled,button)) ui_send(button,0xF5,0,0,1);
            else ui_send(h,WM_COMMAND,id,button,1);
            return 1;
        }
    }
    if(m->message==WM_SYSCHAR || m->message==WM_CHAR) {
        if(m->message==WM_CHAR && m->wparam>=32) return 0;
        struct win *root=ui_win(h); if(!root) return 0;
        for(struct win *w=ui_win(root->last_child);w;w=ui_win(w->prev_sibling)) {
            const uint16_t *s=w->text;
            for(int i=0;s && s[i];i++) if(s[i]=='&' && s[i+1]) {
                if(s[i+1]=='&') { i++; continue; }
                if((s[i+1]|32)==(m->wparam|32) && !(w->style&WS_DISABLED)) {
                    HWND target=w->hwnd;
                    if(!wstrcasecmp(w->cls->name,(const uint16_t *)u"STATIC")) target=next_tab(h,target,0);
                    U(SetFocus,target);
                    if(ui_win(target) && !wstrcasecmp(ui_win(target)->cls->name,(const uint16_t *)u"BUTTON")) ui_send(target,0xF5,0,0,1);
                    return 1;
                }
            }
        }
    }
    return 0;
}
static HWND create_dialog(HINSTANCE inst,gp_t data,uint32_t size,HWND owner,gp_t proc,LPARAM param,int wide)
{
    if(!data || size<18 || !vm_readable(data,size)) return 0;
    struct reader r={GP(data),(uint8_t *)GP(data)+size,0};
    int extended=r.end-r.p>=4 && r.p[2]==0xFF && r.p[3]==0xFF;
    uint32_t style,exstyle; int n,x,y,cx,cy;
    if(extended) { get16(&r); get16(&r); get32(&r); exstyle=get32(&r); style=get32(&r); }
    else { style=get32(&r); exstyle=get32(&r); }
    n=get16(&r); x=(int16_t)get16(&r); y=(int16_t)get16(&r); cx=(int16_t)get16(&r); cy=(int16_t)get16(&r);
    gp_t menu=field(&r),cls=field(&r),title=field(&r),face=0;
    int points=8,weight=400,italic=0;
    if(style&0x40) { points=get16(&r); if(extended) { weight=get16(&r); uint16_t attr=get16(&r); italic=attr&255; } face=field(&r); }
    if(r.bad || n>MAX_WINDOWS-2 || cx<0 || cy<0) { free_field(menu);free_field(cls);free_field(title);free_field(face);return 0; }
    HFONT font=0; int bx=8,by=16;
    if(face) {
        font=G(CreateFontW,-(points*96/72),0,0,0,weight,italic,0,0,1,0,0,0,0,face);
        HDC dc=gdi_window_dc(0); if(font) G(SelectObject,dc,font);
        SIZE sz=ui_measure(dc,(const uint16_t *)u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",52);
        bx=(sz.cx/26+1)/2; by=sz.cy; if(bx<1) bx=8; if(by<1) by=16; gdi_release_dc(dc);
    }
    gp_t class_name=ui_guest((const uint16_t *)u"#32770",1);
    RECT rect={0,0,cx*bx/4,cy*by/8}; gp_t gr=ui_memory(&rect,sizeof(rect));
    U(AdjustWindowRectEx,gr,style,menu!=0,exstyle); rect=*(RECT *)GP(gr); gfree(gr);
    uint32_t args[]={exstyle,class_name,title,style&~WS_VISIBLE,x*bx/4,y*by/8,rect.right-rect.left,rect.bottom-rect.top,owner,0,inst,0};
    HWND h=ui_create(args,1); gfree(class_name);free_field(cls);free_field(title);free_field(face);
    if(!h) { free_field(menu); if(font) G(DeleteObject,font); return 0; }
    struct dialog *d=calloc(1,sizeof(*d)); if(!d) { U(DestroyWindow,h);free_field(menu);return 0; }
    d->proc=proc;d->wide=wide;d->owner=owner;d->font=font;d->defid=1;
    ui_win(h)->dialog=d;
    if(menu) { HMENU hm=wide ? U(LoadMenuW,inst,menu):U(LoadMenuA,inst,menu); if(hm) U(SetMenu,h,hm); } free_field(menu);
    U(SendMessageW,h,0x30,font,0);
    for(int i=0;i<n && !r.bad;i++) {
        align4(&r); uint32_t child_style,child_ex,id;
        if(extended) { get32(&r);child_ex=get32(&r);child_style=get32(&r); }
        else { child_style=get32(&r);child_ex=get32(&r); }
        int ix=(int16_t)get16(&r),iy=(int16_t)get16(&r),iw=(int16_t)get16(&r),ih=(int16_t)get16(&r);
        id=extended ? get32(&r):get16(&r);
        gp_t child_cls=field(&r),child_title=field(&r); int extra=get16(&r); gp_t creation=0;
        if(extra) { int bytes=extra-2; if(bytes<0 || r.end-r.p<bytes) r.bad=1; else { creation=ui_memory(r.p,bytes); r.p+=bytes; } }
        gp_t builtin=0;
        if(child_cls>=0x80 && child_cls<=0x85) {
            const uint16_t *names[]={ (const uint16_t *)u"BUTTON",(const uint16_t *)u"EDIT",(const uint16_t *)u"STATIC",(const uint16_t *)u"LISTBOX",(const uint16_t *)u"SCROLLBAR",(const uint16_t *)u"COMBOBOX" };
            builtin=ui_guest(names[child_cls-0x80],1);
        }
        if(!r.bad) {
            uint32_t ca[]={child_ex,builtin ? builtin:child_cls,child_title,child_style|WS_CHILD,ix*bx/4,iy*by/8,iw*bx/4,ih*by/8,h,id,inst,creation};
            HWND child=ui_create(ca,1);
            if(!child) r.bad=1; else { ui_send(child,0x30,font,0,1); if((child_style&15)==1 && child_cls==0x80) d->defid=id; }
        }
        if(builtin) gfree(builtin);
        if(creation) gfree(creation);
        free_field(child_cls); free_field(child_title);
        if(!ui_win(h)) return 0;
    }
    if(r.bad) { U(DestroyWindow,h); return 0; }
    HWND focus=next_tab(h,0,0);
    if(ui_send(h,WM_INITDIALOG,focus,param,1) && ui_win(h)) U(SetFocus,focus);
    if(ui_win(h) && (style&WS_VISIBLE)) U(ShowWindow,h,5);
    return ui_win(h) ? h:0;
}
static uint32_t template_size(gp_t data)
{
    /* Indirect templates have no length parameter. Bound them to readable guest pages. */
    uint32_t size=0;
    while(size<1024*1024 && vm_readable(data+size,1)) { uint32_t step=4096-((data+size)&4095); if(!vm_readable(data+size,step)) break; size+=step; }
    return size;
}
static int modal(HWND h)
{
    struct win *w=ui_win(h); if(!w || !w->dialog) return -1;
    struct dialog *d=w->dialog; d->modal=1; HWND owner=d->owner;
    int disabled=owner && U(IsWindowEnabled,owner); if(disabled) U(EnableWindow,owner,0);
    if(!d->ended) { U(ShowWindow,h,5); U(SetForegroundWindow,h); }
    while(ui_win(h) && !d->ended) {
        struct msg m; ui_next(&m,1);
        if(m.message==WM_QUIT) { U(PostQuitMessage,m.wparam); break; }
        if(!is_dialog_message(h,&m)) { gp_t gm=ui_memory(&m,sizeof(m)); U(TranslateMessage,gm);gfree(gm);ui_dispatch(&m); }
    }
    int result=d->ended ? d->result:-1;
    if(ui_win(h)) U(DestroyWindow,h);
    if(disabled && ui_win(owner)) { U(EnableWindow,owner,1);U(SetForegroundWindow,owner);if(ui_win(d->previous_focus)) U(SetFocus,d->previous_focus); }
    free(d);return result;
}
static uint32_t run_template(HINSTANCE inst,gp_t data,uint32_t size,HWND owner,gp_t proc,LPARAM param,int wide)
{
    int enabled=owner && U(IsWindowEnabled,owner);
    HWND focus=U(GetFocus);
    if(enabled) U(EnableWindow,owner,0);
    HWND h=create_dialog(inst,data,size,owner,proc,param,wide);
    uint32_t result=h ? (uint32_t)modal(h):(uint32_t)-1;
    if(enabled && ui_win(owner)) {
        U(EnableWindow,owner,1); U(SetForegroundWindow,owner);
        if(ui_win(focus)) U(SetFocus,focus);
    }
    return result;
}
DEF(CreateDialogIndirectParamW) { return create_dialog(A(0),A(1),template_size(A(1)),A(2),A(3),A(4),1); }
DEF(CreateDialogIndirectParamA) { return create_dialog(A(0),A(1),template_size(A(1)),A(2),A(3),A(4),0); }
static uint32_t dialog_resource(uint32_t *a,int wide,int run)
{
    uint32_t size=0; gp_t data=ui_resource(A(0),5,A(1),wide,&size);
    if(run) return run_template(A(0),data,size,A(2),A(3),A(4),wide);
    return create_dialog(A(0),data,size,A(2),A(3),A(4),wide);
}
DEF(CreateDialogParamW) { return dialog_resource(a,1,0); }
DEF(CreateDialogParamA) { return dialog_resource(a,0,0); }
DEF(DialogBoxParamW) { return dialog_resource(a,1,1); }
DEF(DialogBoxParamA) { return dialog_resource(a,0,1); }
DEF(DialogBoxIndirectParamW) { return run_template(A(0),A(1),template_size(A(1)),A(2),A(3),A(4),1); }
DEF(DialogBoxIndirectParamA) { return run_template(A(0),A(1),template_size(A(1)),A(2),A(3),A(4),0); }
DEF(EndDialog)
{
    struct win *w=ui_win(A(0));struct dialog *d=w ? w->dialog:NULL;
    if(!d) return 0;
    d->ended=1;d->result=A(1);
    if(!d->modal) U(ShowWindow,A(0),0);
    return 1;
}
DEF(IsDialogMessageW) { return is_dialog_message(A(0),P(1)); }
DEF(IsDialogMessageA) { return is_dialog_message(A(0),P(1)); }
DEF(GetNextDlgTabItem) { return next_tab(A(0),A(1),A(2)!=0); }
DEF(GetNextDlgGroupItem) { return next_group(A(0),A(1),A(2)!=0); }
DEF(SendDlgItemMessageW) { return ui_send(U(GetDlgItem,A(0),A(1)),A(2),A(3),A(4),1); }
DEF(SendDlgItemMessageA) { return ui_send(U(GetDlgItem,A(0),A(1)),A(2),A(3),A(4),0); }
DEF(SetDlgItemTextW) { return U(SetWindowTextW,U(GetDlgItem,A(0),A(1)),A(2)); }
DEF(SetDlgItemTextA) { return U(SetWindowTextA,U(GetDlgItem,A(0),A(1)),A(2)); }
DEF(GetDlgItemTextW) { return U(GetWindowTextW,U(GetDlgItem,A(0),A(1)),A(2),A(3)); }
DEF(GetDlgItemTextA) { return U(GetWindowTextA,U(GetDlgItem,A(0),A(1)),A(2),A(3)); }
DEF(SetDlgItemInt)
{
    char text[32]; if(A(3)) snprintf(text,sizeof(text),"%d",(int32_t)A(2));else snprintf(text,sizeof(text),"%u",A(2));
    gp_t g=ui_memory(text,strlen(text)+1);uint32_t r=U(SetWindowTextA,U(GetDlgItem,A(0),A(1)),g);gfree(g);return r;
}
DEF(GetDlgItemInt)
{
    gp_t g=galloc(64); uint32_t n=U(GetWindowTextA,U(GetDlgItem,A(0),A(1)),g,64);
    char *s=GP(g),*end;unsigned long value=0; int good=n>0;
    if(A(3)) { long v=strtol(s,&end,10); good=good && end!=s && !*end;value=(unsigned long)v; }
    else { value=strtoul(s,&end,10); good=good && end!=s && !*end && !strchr(s,'-'); }
    if(A(2)) *(BOOL *)P(2)=good;
    gfree(g);return good ? value:0;
}
DEF(CheckDlgButton) { HWND h=U(GetDlgItem,A(0),A(1));if(!h) return 0;ui_send(h,0xF1,A(2),0,1);return 1; }
DEF(IsDlgButtonChecked) { return ui_send(U(GetDlgItem,A(0),A(1)),0xF0,0,0,1); }
DEF(CheckRadioButton)
{
    for(uint32_t id=A(1);id<=A(2) && id-A(1)<65536;id++) { HWND h=U(GetDlgItem,A(0),id);if(h) ui_send(h,0xF1,id==A(3),0,1); }
    return 1;
}
DEF(MapDialogRect)
{
    struct win *w=ui_win(A(0));if(!w) return 0;struct dialog *d=w->dialog;
    HDC dc=gdi_window_dc(0);if(d && d->font) G(SelectObject,dc,d->font);
    SIZE sz=ui_measure(dc,(const uint16_t *)u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",52);gdi_release_dc(dc);
    int bx=(sz.cx/26+1)/2;RECT *r=P(1);r->left=r->left*bx/4;r->right=r->right*bx/4;r->top=r->top*sz.cy/8;r->bottom=r->bottom*sz.cy/8;return 1;
}
DEF(GetDialogBaseUnits) { (void)a;HDC dc=gdi_window_dc(0);SIZE z=ui_measure(dc,(const uint16_t *)u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",52);gdi_release_dc(dc);return (uint16_t)((z.cx/26+1)/2)|(uint32_t)(uint16_t)z.cy<<16; }
DEF(DefDlgProcW) { return ui_default(A(0),A(1),A(2),A(3),1); }
DEF(DefDlgProcA) { return ui_default(A(0),A(1),A(2),A(3),0); }
static HWND make_control(HWND owner,const uint16_t *cls,const uint16_t *text,uint32_t style,int x,int y,int width,int height,int id)
{
    gp_t gc=ui_guest(cls,1),gt=ui_guest(text,1);uint32_t ca[]={0,gc,gt,WS_CHILD|WS_VISIBLE|style,x,y,width,height,owner,id,0,0};
    HWND h=ui_create(ca,1);gfree(gc);gfree(gt);return h;
}
HWND ui_control(HWND owner,const uint16_t *cls,const uint16_t *text,uint32_t style,int x,int y,int width,int height,int id)
{
    return make_control(owner,cls,text,style,x,y,width,height,id);
}
HWND ui_dialog_new(const uint16_t *title,HWND owner,int width,int height,gp_t proc,LPARAM param)
{
    (void)param;
    gp_t cls=ui_guest((const uint16_t *)u"#32770",1),caption=ui_guest(title,1);
    RECT bounds={0,0,width,height};gp_t rectangle=ui_memory(&bounds,sizeof(bounds));U(AdjustWindowRect,rectangle,WS_POPUP|WS_CAPTION|0x80000,0);bounds=*(RECT *)GP(rectangle);gfree(rectangle);
    uint32_t args[]={0,cls,caption,WS_POPUP|WS_CAPTION|0x00080000|0x02000000,80,60,bounds.right-bounds.left,bounds.bottom-bounds.top,owner,0,main_module ? main_module->base:0,0};
    HWND h=ui_create(args,1);gfree(cls);gfree(caption);if(!h) return 0;
    struct dialog *d=calloc(1,sizeof(*d));if(!d) {U(DestroyWindow,h);return 0;}
    d->owner=owner;d->proc=proc;d->wide=1;d->defid=1;ui_win(h)->dialog=d;return h;
}
int ui_modal(HWND h) {return modal(h);}

DEF(MessageBoxProc)
{
    if(A(1)==WM_INITDIALOG) return 1;
    if(A(1)==WM_COMMAND || A(1)==WM_CLOSE) {
        struct win *w=ui_win(A(0));struct dialog *d=w ? w->dialog:NULL;
        int id=A(1)==WM_CLOSE ? 2:A(2)&0xFFFF;
        if(d && id==2) id=d->cancelid;
        if(d && id>0 && id<32 && (d->buttonmask&(1u<<id))) U(EndDialog,A(0),id);
        return 1;
    }
    return 0;
}
static uint32_t message_box(uint32_t *a,int wide)
{
    HWND previous_focus=U(GetFocus);
    uint16_t *text=ui_string(A(1),wide),*caption=ui_string(A(2),wide);
    HDC measure=gdi_window_dc(0);RECT layout={0,0,380,0};
    gp_t message=ui_guest(text,1),gr=ui_memory(&layout,sizeof(layout));
    U(DrawTextW,measure,message,-1,gr,0x410);layout=*(RECT *)GP(gr);gfree(message);gfree(gr);gdi_release_dc(measure);
    int text_height=layout.bottom>32 ? layout.bottom:32;
    if(text_height>gui_screen_h()-160) text_height=gui_screen_h()-160;
    gp_t gc=ui_guest((const uint16_t *)u"#32770",1),gt=ui_guest(caption,1);
    RECT bounds={0,0,420,text_height+90};gr=ui_memory(&bounds,sizeof(bounds));U(AdjustWindowRect,gr,WS_CAPTION|WS_POPUP|0x80000,0);bounds=*(RECT *)GP(gr);gfree(gr);
    uint32_t ca[]={0,gc,gt,WS_CAPTION|WS_POPUP|0x00080000u,80,80,bounds.right-bounds.left,bounds.bottom-bounds.top,A(0),0,0,0};
    HWND h=ui_create(ca,1);gfree(gc);gfree(gt);free(caption);
    if(!h) {free(text);return 0;}
    struct dialog *d=calloc(1,sizeof(*d));if(!d) {free(text);U(DestroyWindow,h);return 0;}
    d->owner=A(0);d->previous_focus=previous_focus;d->wide=1;d->defid=1;
    d->proc=api_thunk(&user32_dll,api_find(&user32_dll,"MessageBoxProc"));ui_win(h)->dialog=d;
    make_control(h,(const uint16_t *)u"STATIC",text,0,20,20,380,text_height,0);free(text);
    static const uint16_t *labels[]={ (const uint16_t *)u"OK",(const uint16_t *)u"Cancel",(const uint16_t *)u"Abort",(const uint16_t *)u"Retry",(const uint16_t *)u"Ignore",(const uint16_t *)u"Yes",(const uint16_t *)u"No",(const uint16_t *)u"Try Again",(const uint16_t *)u"Continue" };
    static const int groups[][4]={{1,0,0,0},{1,2,0,0},{3,4,5,0},{6,7,2,0},{6,7,0,0},{4,2,0,0},{2,10,11,0}};
    int type=A(3)&15;if(type>6) type=0;d->messagebox=1;d->cancelid=type==0 ? 1:(type==2 || type==4) ? 0:2;int count=0;while(count<3 && groups[type][count]) count++;
    int def=(A(3)>>8)&3;if(def>=count) def=0;
    HWND focus=0;
    for(int i=0;i<count;i++) {int id=groups[type][i];d->buttonmask|=1u<<id;int label=id==10 ? 7:id==11 ? 8:id-1; HWND b=make_control(h,(const uint16_t *)u"BUTTON",labels[label],0x10000|(i==def ? 1:0),410-count*90+i*90,text_height+45,80,26,id);if(i==def) { d->defid=id;focus=b; }}
    U(SetFocus,focus);return modal(h);
}
DEF(MessageBoxW) { return message_box(a,1); }
DEF(MessageBoxA) { return message_box(a,0); }
DEF(MessageBoxExW) { return message_box(a,1); }
DEF(MessageBoxExA) { return message_box(a,0); }
const struct api_entry dialog_entries[]={
    API(DialogProcW,4),API(MessageBoxProc,4),API(CreateDialogParamA,5),API(CreateDialogParamW,5),
    API(CreateDialogIndirectParamA,5),API(CreateDialogIndirectParamW,5),API(DialogBoxParamA,5),API(DialogBoxParamW,5),
    API(DialogBoxIndirectParamA,5),API(DialogBoxIndirectParamW,5),API(EndDialog,2),API(IsDialogMessageA,2),API(IsDialogMessageW,2),
    API(GetNextDlgTabItem,3),API(GetNextDlgGroupItem,3),API(SendDlgItemMessageA,5),API(SendDlgItemMessageW,5),
    API(SetDlgItemTextA,3),API(SetDlgItemTextW,3),API(GetDlgItemTextA,4),API(GetDlgItemTextW,4),API(SetDlgItemInt,4),API(GetDlgItemInt,4),
    API(CheckDlgButton,3),API(IsDlgButtonChecked,2),API(CheckRadioButton,4),API(MapDialogRect,2),API(GetDialogBaseUnits,0),
    API(DefDlgProcA,4),API(DefDlgProcW,4),API(MessageBoxA,4),API(MessageBoxW,4),API(MessageBoxExA,5),API(MessageBoxExW,5),
};
const int dialog_count=sizeof(dialog_entries)/sizeof(dialog_entries[0]);
void dialog_init(void) { ui_class("#32770",&dialog_entries[0],16); }
