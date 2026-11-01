/* The common controls use ordinary HWNDs and guest-callable procedures. Their own
 * state never occupies application extra bytes, and notifications run through USER32. */
#include "ui.h"
#define WM_NOTIFY 0x004E
#define WM_COMMAND 0x0111
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_GETDLGCODE 0x0087
#define TEXT_CALLBACK 0xFFFFFFFFu
#define MAX_SUBITEMS 32
#define IL_API(name,n) extern uint32_t api_##name(uint32_t *a);
#include "imagelist-api.h"
#undef IL_API
extern struct api_dll comctl32_dll;
enum {
    STATUS,TOOLBAR,PROGRESS,TRACKBAR,TAB,LISTVIEW,TREEVIEW,UPDOWN,TOOLTIP
};
static const char *const names[]={
    "msctls_statusbar32","ToolbarWindow32","msctls_progress32","msctls_trackbar32","SysTabControl32","SysListView32","SysTreeView32","msctls_updown32","tooltips_class32"
};
struct item {
    uint16_t *text,*sub[MAX_SUBITEMS];
    uint32_t id,parent,param,state,style;
    int image,selected_image,children,expanded,width,fmt;
    RECT rect;
};
struct common {
    struct common *next;
    HWND hwnd;
    int kind,n,cap,selected,first,min,max,pos,step,page,line,drag,hot,unicode,marquee;
    int buddy,base,width,height,tick_freq,tip_width;
    uint32_t extended;
    HFONT font;
    COLORREF background,foreground,bar;
    HANDLE images[3];
    int edges[64],nparts,simple;
    struct item *items;
    uint16_t **strings;
    int nstrings;
    HWND tool_owner;
    uint32_t tool_id;
    RECT tool_rect;
    uint16_t *tip;
};
static struct common *all;
static uint32_t next_tree=0x64000001;
static struct common *get(HWND h) {
    struct win *w=ui_win(h);
    return w ? w->common:NULL;
}
static void free_item(struct item *i) {
    free(i->text);
    for(int j=0;j<MAX_SUBITEMS;j++) free(i->sub[j]);
    memset(i,0,sizeof(*i));
}
void common_destroy(struct win *w)
{
    struct common *c=w->common;
    if(!c) return;
    struct common **p=&all;
    while(*p && *p!=c) p=&(*p)->next;
    if(*p) *p=c->next;
    for(int i=0;i<c->n;i++) free_item(&c->items[i]);
    for(int i=0;i<c->nstrings;i++) free(c->strings[i]);
    free(c->strings);
    free(c->items);
    free(c->tip);
    free(c);
    w->common=NULL;
}
static uint16_t *string(gp_t p,int wide) {
    return p && p!=TEXT_CALLBACK ? ui_string(p,wide):ui_wdup((const uint16_t *)u"",0);
}
static int put_text(gp_t out,int cap,const uint16_t *text,int wide)
{
    int n=text ? wstrlen(text):0;
    if(!out || cap<=0) return n;
    if(n>=cap) n=cap-1;
    if(wide) {
        memcpy(GP(out),text,n*2);
        ((uint16_t *)GP(out))[n]=0;
    }
    else {
        wide_to_ansi(text,n,GP(out),n);
        ((char *)GP(out))[n]=0;
    }
    return n;
}
static int insert_item(struct common *c,int at)
{
    if(c->n>=65536) return -1;
    if(c->n==c->cap) {
        int cap=c->cap ? c->cap*2:8;
        struct item *p=realloc(c->items,cap*sizeof(*p));
        if(!p) return -1;
        c->items=p;
        c->cap=cap;
    }
    if(at<0 || at>c->n) at=c->n;
    memmove(c->items+at+1,c->items+at,(c->n-at)*sizeof(*c->items));
    memset(c->items+at,0,sizeof(*c->items));
    c->items[at].image=-1;
    c->items[at].width=100;
    c->items[at].id=next_tree++;
    c->n++;
    if(c->selected>=at) c->selected++;
    return at;
}
static int index_of(struct common *c,uint32_t h) {
    for(int i=0;i<c->n;i++) if(c->items[i].id==h) return i;
    return -1;
}
static LRESULT notify(HWND h,int code,const void *tail,size_t size)
{
    struct win *w=ui_win(h);
    if(!w || !w->parent) return 0;
    HWND parent=w->parent;
    uint32_t id=w->id;
    gp_t memory=galloc(12+size);
    uint32_t *hdr=GP(memory);
    hdr[0]=h;
    hdr[1]=id;
    hdr[2]=code;
    if(size) memcpy(hdr+3,tail,size);
    LRESULT result=ui_send(parent,WM_NOTIFY,id,memory,1);
    gfree(memory);
    return result;
}
static int clamp(struct common *c,int pos) {
    if(pos<c->min) pos=c->min;
    if(pos>c->max) pos=c->max;
    return pos;
}
static int row_height(HDC dc) {
    int h=ui_measure(dc,(const uint16_t *)u"Mg",2).cy+4;
    return h>8 ? h:20;
}
static int tree_visible(struct common *c,int i)
{
    uint32_t parent=c->items[i].parent;
    for(int depth=0;parent && depth<64;depth++) {
        int p=index_of(c,parent);
        if(p<0 || !c->items[p].expanded) return 0;
        parent=c->items[p].parent;
    }
    return 1;
}
static int tree_depth(struct common *c,int i) {
    int depth=0;
    uint32_t parent=c->items[i].parent;
    while(parent && depth<64) {
        int p=index_of(c,parent);
        if(p<0) break;
        depth++;
        parent=c->items[p].parent;
    }
    return depth;
}
static int visible_index(struct common *c,int row) {
    for(int i=0,n=0;i<c->n;i++) if(tree_visible(c,i) && n++==row) return i;
    return -1;
}
static void tree_item(uint32_t *out,const struct item *item)
{
    memset(out,0,40);
    if(!item) return;
    out[0]=0x7F;
    out[1]=item->id;
    out[2]=item->state;
    out[3]=-1;
    out[6]=item->image;
    out[7]=item->selected_image;
    out[8]=item->children;
    /* lParam follows the first 9 DWORDs in TVITEM (40 bytes). */
    out[9]=item->param;
}
static int select_item(HWND h,int index)
{
    struct common *c=get(h);
    if(!c || index<0 || index>=c->n || index==c->selected) return 0;
    int old=c->selected,kind=c->kind;
    uint32_t new_id=c->items[index].id;
    if(kind==TAB) {
        if(notify(h,-552,NULL,0)) return 0;
    }
    else if(kind==TREEVIEW) {
        uint32_t change[23]={
            1
        };
        if(old>=0) tree_item(change+1,&c->items[old]);
        tree_item(change+11,&c->items[index]);
        if(notify(h,c->unicode ? -450:-401,change,sizeof(change))) return 0;
    }
    c=get(h);
    if(!c) return 0;
    index=index_of(c,new_id);
    if(index<0) return 0;
    old=c->selected;
    if(old>=0 && old<c->n) c->items[old].state&=~3u;
    c->selected=index;
    c->items[index].state|=3;
    ui_invalidate(h);
    if(kind==TAB) notify(h,-551,NULL,0);
    else if(kind==TREEVIEW) {
        uint32_t change[23]={
            1
        };
        if(old>=0 && old<c->n) tree_item(change+1,&c->items[old]);
        tree_item(change+11,&c->items[index]);
        notify(h,c->unicode ? -451:-402,change,sizeof(change));
    }
    else if(kind==LISTVIEW) {
        uint32_t change[]={
            index,0,3,0,8,0,0,c->items[index].param
        };
        notify(h,-101,change,sizeof(change));
    }
    return 1;
}
static int toolbar_width(struct common *c,int i) {
    return c->items[i].style&1 ? 8:c->width;
}
static RECT item_rect(struct common *c,struct win *w,HDC dc,int i)
{
    int lh=row_height(dc);
    if(c->kind==TAB || c->kind==TOOLBAR) {
        int x=0;
        for(int j=0;j<i;j++) x+=c->kind==TAB ? c->items[j].width:toolbar_width(c,j);
        return (RECT){
            x,0,x+(c->kind==TAB ? c->items[i].width:toolbar_width(c,i)),c->height
        };
    }
    if(c->kind==LISTVIEW && (w->style&3)!=1) {
        int columns=w->cw/90;
        if(columns<1) columns=1;
        return (RECT){
            i%columns*90,i/columns*64, i%columns*90+90,i/columns*64+64
        };
    }
    return (RECT){
        0,(i-c->first)*lh+(c->kind==LISTVIEW && c->nparts ? lh:0),w->cw,(i-c->first+1)*lh+(c->kind==LISTVIEW && c->nparts ? lh:0)
    };
}
static void paint(HWND h,HDC dc)
{
    struct common *c=get(h);
    struct win *w=ui_win(h);
    if(!c || !w) return;
    if(c->font) G(SelectObject,dc,c->font);
    int width=w->cw,height=w->ch,lh=row_height(dc);
    COLORREF face=c->background!=0xFFFFFFFF ? c->background:sys_color(c->kind==LISTVIEW || c->kind==TREEVIEW ? 5:15);
    ui_fill(dc,0,0,width,height,face);
    if(c->kind==PROGRESS) {
        ui_frame(dc,0,0,width,height,sys_color(16));
        int range=c->max-c->min,filled=range>0 ? (int)((int64_t)(c->pos-c->min)*(width-4)/range):0;
        if(w->style&4) {
            filled=range>0 ? (int)((int64_t)(c->pos-c->min)*(height-4)/range):0;
            ui_fill(dc,2,height-2-filled,width-4,filled,c->bar);
        }
        else ui_fill(dc,2,2,filled,height-4,c->bar);
        return;
    }
    if(c->kind==TRACKBAR) {
        int vertical=w->style&2,len=vertical ? height:width,thickness=vertical ? width:height;
        int pos=10+(c->max>c->min ? (int)((int64_t)(c->pos-c->min)*(len-20)/(c->max-c->min)):0);
        if(vertical) {
            ui_frame(dc,thickness/2-2,8,4,len-16,sys_color(16));
            ui_fill(dc,2,pos-4,thickness-4,9,sys_color(15));
            ui_frame(dc,2,pos-4,thickness-4,9,sys_color(16));
        }
        else {
            ui_frame(dc,8,thickness/2-2,len-16,4,sys_color(16));
            ui_fill(dc,pos-4,2,9,thickness-4,sys_color(15));
            ui_frame(dc,pos-4,2,9,thickness-4,sys_color(16));
        }
        return;
    }
    if(c->kind==UPDOWN) {
        int horizontal=w->style&0x40;
        ui_frame(dc,0,0,width,height,sys_color(16));
        if(horizontal) {
            ui_frame(dc,0,0,width/2,height,sys_color(16));
            ui_label(dc,(const uint16_t *)u"<",(RECT){
                0,0,width/2,height
            }
            ,0x25,sys_color(18));
            ui_label(dc,(const uint16_t *)u">",(RECT){
                width/2,0,width,height
            }
            ,0x25,sys_color(18));
        }
        else {
            ui_frame(dc,0,0,width,height/2,sys_color(16));
            ui_label(dc,(const uint16_t *)u"^",(RECT){
                0,0,width,height/2
            }
            ,0x25,sys_color(18));
            ui_label(dc,(const uint16_t *)u"v",(RECT){
                0,height/2,width,height
            }
            ,0x25,sys_color(18));
        }
        return;
    }
    if(c->kind==STATUS) {
        int count=c->simple ? 1:c->nparts;
        if(!count) count=1;
        for(int i=0,left=0;i<count;i++) {
            int right=c->simple || !c->nparts || c->edges[i]<0 ? width:c->edges[i];
            ui_frame(dc,left,1,right-left,height-2,sys_color(16));
            if(i<c->n) ui_label(dc,c->items[i].text,(RECT){
                left+4,2,right-3,height-2
            }
            ,0x24,c->foreground);
            left=right;
        }
        return;
    }
    if(c->kind==TOOLTIP) {
        ui_fill(dc,0,0,width,height,0x00E1FFFF);
        ui_frame(dc,0,0,width,height,sys_color(6));
        ui_label(dc,c->tip,(RECT){
            4,3,width-4,height-3
        }
        ,0x10,c->foreground);
        return;
    }
    if(c->kind==TAB) ui_frame(dc,0,c->height-1,width,height-c->height+1,sys_color(16));
    if(c->kind==LISTVIEW && (w->style&3)==1 && c->nparts) {
        for(int j=0,x=0;j<c->nparts;j++) {
            int col=c->edges[j];
            ui_fill(dc,x,0,col,lh,sys_color(15));
            ui_frame(dc,x,0,col,lh,sys_color(16));
            if(j<c->nstrings) ui_label(dc,c->strings[j],(RECT){
                x+4,1,x+col-2,lh
            }
            ,0x24,c->foreground);
            x+=col;
        }
    }
    for(int row=0;row<c->n;row++) {
        int i=c->kind==TREEVIEW ? visible_index(c,row+c->first):row;
        if(i<0) break;
        struct item *item=&c->items[i];
        RECT r=item_rect(c,w,dc,c->kind==TREEVIEW ? row+c->first:i);
        if(c->kind==TREEVIEW) r=(RECT){
            0,row*lh,width,(row+1)*lh
        };
        if(r.bottom<=0 || r.top>=height || r.left>=width) continue;
        if(c->kind==TOOLBAR && (item->state&8)) continue;
        int selected=i==c->selected || (item->state&2 && c->kind==LISTVIEW);
        COLORREF fg=selected ? sys_color(14):c->foreground;
        if(c->kind==TOOLBAR || c->kind==TAB) {
            ui_fill(dc,r.left+1,r.top+1,r.right-r.left-2,r.bottom-r.top-2,sys_color(15));
            if(!(item->style&1) || c->kind==TAB) ui_frame(dc,r.left,r.top,r.right-r.left,r.bottom-r.top,sys_color(selected ? 6:16));
            if(c->kind==TOOLBAR && !(item->state&4)) fg=sys_color(17);
            else fg=c->foreground;
        }
        else if(selected) ui_fill(dc,r.left,r.top,r.right-r.left,r.bottom-r.top,sys_color(13));
        int x=r.left+4,y=r.top+2;
        if(c->kind==TREEVIEW) {
            x+=tree_depth(c,i)*16;
            int has_children=item->children;
            for(int j=0;j<c->n;j++) if(c->items[j].parent==item->id) has_children=1;
            if(has_children) {
                ui_frame(dc,x,y+2,11,11,sys_color(16));
                ui_label(dc,(const uint16_t *)(item->expanded ? u"-":u"+"),(RECT){
                    x,y,x+11,y+15
                }
                ,0x21,c->foreground);
            }
            x+=16;
        }
        HANDLE images=c->images[c->kind==LISTVIEW && (w->style&3)==0 ? 0:1];
        if(!images) images=c->images[0];
        if(item->image>=0 && images) {
            int iw,ih;
            if(imagelist_size(images,&iw,&ih)) {
                imagelist_draw(images,item->image,dc,x,y,0,0,1);
                x+=iw+4;
            }
        }
        if(c->kind==LISTVIEW && (w->style&3)!=1) {
            ui_label(dc,item->text,(RECT){
                r.left+2,r.top+38,r.right-2,r.bottom
            }
            ,0x21,fg);
        }
        else if(c->kind==LISTVIEW && c->nparts) {
            int left=0;
            for(int j=0;j<c->nparts;j++) {
                ui_label(dc,j ? item->sub[j]:item->text,(RECT){
                    left+(j ? 4:x),y,left+c->edges[j]-2,r.bottom
                }
                ,0x24,fg);
                left+=c->edges[j];
            }
        }
        else if(!(c->kind==TOOLBAR && (item->style&1))) ui_label(dc,item->text,(RECT){
            x,y,r.right-3,r.bottom-1
        }
        ,0x24|(c->kind==TAB ? 1:0),fg);
    }
}
static LRESULT list_message(HWND h,UINT m,WPARAM wp,LPARAM lp,int *handled)
{
    struct common *c=get(h);
    int wide=m==0x104B || m==0x104C || m==0x104D || m==0x1073 || m==0x1074 || m==0x105F || m==0x1060 || m==0x1061;
    *handled=1;
    if(m==0x1004) return c->n;
    if(m==0x1002) return wp<3 ? c->images[wp]:0;
    if(m==0x1003) {
        if(wp>=3) return 0;
        HANDLE old=c->images[wp];
        c->images[wp]=lp;
        ui_invalidate(h);
        return old;
    }
    if(m==0x1036) {
        uint32_t old=c->extended;
        c->extended=wp ? (old&~wp)|(lp&wp):lp;
        ui_invalidate(h);
        return old;
    }
    if(m==0x1037) return c->extended;
    if(m==0x1000 || m==0x1026) {
        COLORREF old=m==0x1000 ? c->background:c->foreground;
        if(m==0x1000) c->background=lp;
        else c->foreground=lp;
        ui_invalidate(h);
        return old;
    }
    if(m==0x1001) return c->background;
    if(m==0x1023) return c->foreground;
    if(m==0x101B || m==0x1061) {
        uint32_t *column=GP(lp);
        if(!column || c->nparts==64) return -1;
        int at=wp>(uint32_t)c->nparts ? c->nparts:(int)wp;
        uint16_t **strings=realloc(c->strings,(c->nparts+1)*sizeof(*strings));
        if(!strings) return -1;
        c->strings=strings;
        memmove(c->strings+at+1,c->strings+at,(c->nparts-at)*sizeof(*c->strings));
        memmove(c->edges+at+1,c->edges+at,(c->nparts-at)*sizeof(*c->edges));
        c->strings[at]=string(column[3],wide);
        c->edges[at]=(column[0]&2) ? (int)column[2]:100;
        c->nparts++;
        c->nstrings=c->nparts;
        ui_invalidate(h);
        return at;
    }
    if(m==0x101C) {
        if(wp>=(uint32_t)c->nparts) return 0;
        free(c->strings[wp]);
        memmove(c->strings+wp,c->strings+wp+1,(c->nparts-wp-1)*sizeof(*c->strings));
        memmove(c->edges+wp,c->edges+wp+1,(c->nparts-wp-1)*sizeof(*c->edges));
        c->nparts--;
        c->nstrings--;
        ui_invalidate(h);
        return 1;
    }
    if(m==0x101E) {
        if(wp>=(uint32_t)c->nparts) return 0;
        c->edges[wp]=(int16_t)lp;
        if(c->edges[wp]<0) c->edges[wp]=100;
        ui_invalidate(h);
        return 1;
    }
    if(m==0x101D) return wp<(uint32_t)c->nparts ? c->edges[wp]:0;
    if(m==0x1019 || m==0x105F || m==0x101A || m==0x1060) {
        if(wp>=(uint32_t)c->nparts || !lp) return 0;
        uint32_t *column=GP(lp);
        int set=m==0x101A || m==0x1060;
        if(column[0]&2) {
            if(set) c->edges[wp]=column[2];
            else column[2]=c->edges[wp];
        }
        if(column[0]&4) {
            if(set) {
                free(c->strings[wp]);
                c->strings[wp]=string(column[3],wide);
            }
            else put_text(column[3],column[4],c->strings[wp],wide);
        }
        ui_invalidate(h);
        return 1;
    }
    if(m==0x1007 || m==0x104D || m==0x1006 || m==0x104C || m==0x1005 || m==0x104B) {
        uint32_t *item=GP(lp);
        if(!item) return m==0x1007 || m==0x104D ? -1:0;
        int add=m==0x1007 || m==0x104D,set=add || m==0x1006 || m==0x104C,index=item[1],sub=item[2];
        if(add) {
            index=insert_item(c,index);
            if(index<0) return -1;
        }
        if(index<0 || index>=c->n || sub<0 || sub>=MAX_SUBITEMS) return add ? -1:0;
        struct item *i=&c->items[index];
        uint32_t mask=item[0];
        if(mask&1) {
            uint16_t **text=sub ? &i->sub[sub]:&i->text;
            if(set) {
                uint16_t *s=string(item[5],wide);
                free(*text);
                *text=s;
            }
            else put_text(item[5],item[6],*text,wide);
        }
        if(mask&2) {
            if(set) i->image=item[7];
            else item[7]=i->image;
        }
        if(mask&4) {
            if(set) i->param=item[8];
            else item[8]=i->param;
        }
        if(mask&8) {
            if(set) {
                i->state=(i->state&~item[4])|(item[3]&item[4]);
                if(i->state&2) c->selected=index;
            }
            else item[3]=i->state&item[4];
        }
        ui_invalidate(h);
        return add ? index:1;
    }
    if(m==0x102D || m==0x1073 || m==0x102E || m==0x1074) {
        uint32_t *item=GP(lp);
        if(!item || wp>=(uint32_t)c->n || item[2]>=MAX_SUBITEMS) return 0;
        uint16_t **text=item[2] ? &c->items[wp].sub[item[2]]:&c->items[wp].text;
        if(m==0x102E || m==0x1074) {
            uint16_t *s=string(item[5],wide);
            free(*text);
            *text=s;
            ui_invalidate(h);
            return 1;
        }
        return put_text(item[5],item[6],*text,wide);
    }
    if(m==0x1008) {
        if(wp>=(uint32_t)c->n) return 0;
        uint32_t data=c->items[wp].param;
        free_item(&c->items[wp]);
        memmove(c->items+wp,c->items+wp+1,(c->n-wp-1)*sizeof(*c->items));
        c->n--;
        if(c->selected==(int)wp) c->selected=-1;
        else if(c->selected>(int)wp) c->selected--;
        ui_invalidate(h);
        uint32_t tail[]={
            wp,0,0,0,0,0,0,data
        };
        notify(h,-103,tail,sizeof(tail));
        return 1;
    }
    if(m==0x1009) {
        for(int i=0;i<c->n;i++) free_item(&c->items[i]);
        c->n=0;
        c->selected=-1;
        ui_invalidate(h);
        return 1;
    }
    if(m==0x102B) {
        uint32_t *item=GP(lp);
        if(!item) return 0;
        int first=(int32_t)wp==-1 ? 0:(int)wp,last=(int32_t)wp==-1 ? c->n-1:(int)wp;
        if(first<0 || last>=c->n) return 0;
        for(int i=first;i<=last;i++) {
            c->items[i].state=(c->items[i].state&~item[4])|(item[3]&item[4]);
            if(c->items[i].state&2) c->selected=i;
        }
        ui_invalidate(h);
        return 1;
    }
    if(m==0x102C) return wp<(uint32_t)c->n ? c->items[wp].state&lp:0;
    if(m==0x1032) {
        int n=0;
        for(int i=0;i<c->n;i++) n+=(c->items[i].state&2)!=0;
        return n;
    }
    if(m==0x100C) {
        for(int i=(int)wp+1;i<c->n;i++) if(!(lp&3) || (c->items[i].state&(lp&3))) return i;
        return -1;
    }
    if(m==0x1027) return c->first;
    if(m==0x1013) {
        if(wp>=(uint32_t)c->n) return 0;
        c->first=wp;
        ui_invalidate(h);
        return 1;
    }
    if(m==0x100E || m==0x1038) {
        if(wp>=(uint32_t)c->n || !lp) return 0;
        HDC dc=gdi_window_dc(h);
        *(RECT *)GP(lp)=item_rect(c,ui_win(h),dc,wp);
        gdi_release_dc(dc);
        return 1;
    }
    if(m==0x100F || m==0x1031) return wp<(uint32_t)c->n;
    if(m==0x1014) {
        c->first+=(int32_t)lp/20;
        if(c->first<0) c->first=0;
        ui_invalidate(h);
        return 1;
    }
    *handled=0;
    return 0;
}
static LRESULT tree_message(HWND h,UINT m,WPARAM wp,LPARAM lp,int *handled)
{
    struct common *c=get(h);
    int wide=m==0x1132 || m==0x113E || m==0x113F;
    *handled=1;
    if(m==0x1105) return c->n;
    if(m==0x1108) return wp<3 ? c->images[wp]:0;
    if(m==0x1109) {
        if(wp>=3) return 0;
        HANDLE old=c->images[wp];
        c->images[wp]=lp;
        ui_invalidate(h);
        return old;
    }
    if(m==0x1100 || m==0x1132 || m==0x110C || m==0x113E || m==0x110D || m==0x113F) {
        uint32_t *data=GP(lp);
        if(!data) return 0;
        int add=m==0x1100 || m==0x1132,set=add || m==0x110D || m==0x113F;
        uint32_t *item=data+(add ? 2:0),parent=add ? data[0]:0;
        if(parent==0xFFFF0000u) parent=0;
        if(add && parent && index_of(c,parent)<0) return 0;
        int at=c->n;
        if(add) {
            uint32_t after=data[1];
            if(after==0xFFFF0001u) {
                at=parent ? index_of(c,parent)+1:0;
            }
            else if(after && after!=0xFFFF0002u && after!=0xFFFF0003u) {
                int sibling=index_of(c,after);
                if(sibling<0 || c->items[sibling].parent!=parent) return 0;
                at=sibling+1;
                while(at<c->n && tree_depth(c,at)>tree_depth(c,sibling)) at++;
            }
            else if(parent) {
                int p=index_of(c,parent);
                at=p+1;
                while(at<c->n && tree_depth(c,at)>tree_depth(c,p)) at++;
            }
        }
        int index=add ? insert_item(c,at):index_of(c,item[1]);
        if(index<0) return 0;
        struct item *i=&c->items[index];
        if(add) i->parent=parent;
        uint32_t mask=item[0];
        if(mask&1) {
            if(set) {
                uint16_t *s=string(item[4],wide);
                free(i->text);
                i->text=s;
            }
            else put_text(item[4],item[5],i->text,wide);
        }
        if(mask&2) {
            if(set) i->image=item[6];
            else item[6]=i->image;
        }
        if(mask&4) {
            if(set) i->param=item[9];
            else item[9]=i->param;
        }
        if(mask&8) {
            if(set) {
                i->state=(i->state&~item[3])|(item[2]&item[3]);
                i->expanded=(i->state&0x20)!=0;
            }
            else item[2]=i->state&item[3];
        }
        if(mask&0x20) {
            if(set) i->selected_image=item[7];
            else item[7]=i->selected_image;
        }
        if(mask&0x40) {
            if(set) i->children=item[8];
            else item[8]=i->children;
        }
        ui_invalidate(h);
        return add ? i->id:1;
    }
    if(m==0x1101) {
        if((uint32_t)lp==0xFFFF0000u) {
            for(int i=0;i<c->n;i++) free_item(&c->items[i]);
            c->n=0;
            c->selected=-1;
            ui_invalidate(h);
            return 1;
        }
        int index=index_of(c,lp);
        if(index<0) return 0;
        /* Remove descendants first; their stable handles are never raw host pointers. */
        for(int i=c->n-1;i>=0;i--) {
            uint32_t parent=c->items[i].id;
            int remove=0;
            for(int depth=0;parent && depth<64;depth++) {
                if(parent==(uint32_t)lp) {
                    remove=1;
                    break;
                }
                int p=index_of(c,parent);
                parent=p<0 ? 0:c->items[p].parent;
            }
            if(remove) {
                free_item(&c->items[i]);
                memmove(c->items+i,c->items+i+1,(c->n-i-1)*sizeof(*c->items));
                c->n--;
                if(c->selected==i) c->selected=-1;
                else if(c->selected>i) c->selected--;
            }
        }
        ui_invalidate(h);
        return 1;
    }
    if(m==0x1102) {
        int i=index_of(c,lp);
        if(i<0) return 0;
        uint32_t change[23]={
            wp
        };
        tree_item(change+11,&c->items[i]);
        if(notify(h,c->unicode ? -454:-405,change,sizeof(change))) return 0;
        c=get(h);
        if(!c || (i=index_of(c,lp))<0) return 0;
        c->items[i].expanded=wp==3 ? !c->items[i].expanded:(wp&2)!=0;
        c->items[i].state=(c->items[i].state&~0x20u)|(c->items[i].expanded ? 0x20:0);
        ui_invalidate(h);
        notify(h,c->unicode ? -455:-406,change,sizeof(change));
        return 1;
    }
    if(m==0x110B) {
        if(wp==9) return select_item(h,index_of(c,lp));
        return 0;
    }
    if(m==0x110A) {
        int i=index_of(c,lp);
        if(wp==9) return c->selected>=0 ? c->items[c->selected].id:0;
        if(wp==0) {
            for(int k=0;k<c->n;k++) if(!c->items[k].parent) return c->items[k].id;
            return 0;
        }
        if(wp==3) return i>=0 ? c->items[i].parent:0;
        if(wp==4) {
            uint32_t parent=i>=0 ? c->items[i].id:0;
            for(int k=0;k<c->n;k++) if(c->items[k].parent==parent) return c->items[k].id;
            return 0;
        }
        if((wp==1 || wp==2) && i>=0) {
            int direction=wp==1 ? 1:-1;
            for(int k=i+direction;k>=0 && k<c->n;k+=direction) if(c->items[k].parent==c->items[i].parent) return c->items[k].id;
            return 0;
        }
        if(wp==5) {
            int k=visible_index(c,c->first);
            return k>=0 ? c->items[k].id:0;
        }
        if(wp==6 || wp==7) {
            int direction=wp==6 ? 1:-1;
            for(int k=i+direction;k>=0 && k<c->n;k+=direction) if(tree_visible(c,k)) return c->items[k].id;
            return 0;
        }
        return 0;
    }
    if(m==0x1114) {
        int i=index_of(c,lp);
        if(i<0) return 0;
        uint32_t parent=c->items[i].parent;
        while(parent) {
            int p=index_of(c,parent);
            if(p<0) break;
            c->items[p].expanded=1;
            parent=c->items[p].parent;
        }
        ui_invalidate(h);
        return 1;
    }
    if(m==0x1106 && lp) {
        uint32_t id=*(uint32_t *)GP(lp);
        int i=index_of(c,id);
        if(i<0) return 0;
        int row=0;
        for(int k=0;k<i;k++) row+=tree_visible(c,k);
        HDC dc=gdi_window_dc(h);
        int lh=row_height(dc);
        gdi_release_dc(dc);
        *(RECT *)GP(lp)=(RECT){
            tree_depth(c,i)*16+16,(row-c->first)*lh,ui_win(h)->cw,(row-c->first+1)*lh
        };
        return 1;
    }
    *handled=0;
    return 0;
}
static LRESULT tab_message(HWND h,UINT m,WPARAM wp,LPARAM lp,int *handled)
{
    struct common *c=get(h);
    *handled=1;
    int wide=m==0x133C || m==0x133D || m==0x133E;
    if(m==0x1304) return c->n;
    if(m==0x130B || m==0x132F) return c->selected;
    if(m==0x130C || m==0x1330) {
        int old=c->selected;
        c->selected=(int)wp<c->n ? (int)wp:-1;
        ui_invalidate(h);
        return old;
    }
    if(m==0x1302) return c->images[0];
    if(m==0x1303) {
        HANDLE old=c->images[0];
        c->images[0]=lp;
        return old;
    }
    if(m==0x1328 && lp) {
        RECT *r=GP(lp);
        int d=wp ? 1:-1;
        r->left-=d*3;
        r->right+=d*3;
        r->top-=d*(c->height+3);
        r->bottom+=d*3;
        return 0;
    }
    if(m==0x1329) {
        uint32_t old=(uint16_t)c->width|(uint32_t)(uint16_t)c->height<<16;
        c->width=(uint16_t)lp;
        c->height=(uint16_t)(lp>>16);
        for(int i=0;i<c->n;i++) c->items[i].width=c->width;
        ui_invalidate(h);
        return old;
    }
    if(m==0x132C) return c->n ? 1:0;
    if(m==0x130A) {
        if(wp>=(uint32_t)c->n || !lp) return 0;
        HDC dc=gdi_window_dc(h);
        *(RECT *)GP(lp)=item_rect(c,ui_win(h),dc,wp);
        gdi_release_dc(dc);
        return 1;
    }
    if(m==0x1307 || m==0x133E || m==0x1306 || m==0x133D || m==0x1305 || m==0x133C) {
        uint32_t *item=GP(lp);
        if(!item) return 0;
        int add=m==0x1307 || m==0x133E,set=add || m==0x1306 || m==0x133D;
        int i=add ? insert_item(c,wp):(int)wp;
        if(i<0 || i>=c->n) return add ? -1:0;
        if(item[0]&1) {
            if(set) {
                free(c->items[i].text);
                c->items[i].text=string(item[3],wide);
            }
            else put_text(item[3],item[4],c->items[i].text,wide);
        }
        if(item[0]&2) {
            if(set) c->items[i].image=item[5];
            else item[5]=c->items[i].image;
        }
        if(item[0]&8) {
            if(set) c->items[i].param=item[6];
            else item[6]=c->items[i].param;
        }
        if(add) {
            c->items[i].width=c->width;
            if(c->selected<0) c->selected=0;
        }
        ui_invalidate(h);
        return add ? i:1;
    }
    if(m==0x1308) {
        if(wp>=(uint32_t)c->n) return 0;
        free_item(&c->items[wp]);
        memmove(c->items+wp,c->items+wp+1,(c->n-wp-1)*sizeof(*c->items));
        c->n--;
        if(c->selected>=c->n) c->selected=c->n-1;
        ui_invalidate(h);
        return 1;
    }
    if(m==0x1309) {
        for(int i=0;i<c->n;i++) free_item(&c->items[i]);
        c->n=0;
        c->selected=-1;
        ui_invalidate(h);
        return 1;
    }
    *handled=0;
    return 0;
}
static LRESULT toolbar_message(HWND h,UINT m,WPARAM wp,LPARAM lp,int *handled)
{
    struct common *c=get(h);
    *handled=1;
    if(m==0x041E) return wp==20;
    if(m==0x0418) return c->n;
    if(m==0x0430) {
        HANDLE old=c->images[0];
        c->images[0]=lp;
        ui_invalidate(h);
        return old;
    }
    if(m==0x0431) return c->images[0];
    if(m==0x041F) {
        c->width=(uint16_t)lp;
        c->height=(uint16_t)(lp>>16);
        ui_invalidate(h);
        return 1;
    }
    if(m==0x0420 || m==0x0421 || m==0x043C || m==0x0459) {
        ui_invalidate(h);
        return 1;
    }
    if(m==0x041C || m==0x044D) {
        if(wp || !lp) return -1;
        int wide=m==0x044D,first=c->nstrings;
        gp_t p=lp;
        while(wide ? *(uint16_t *)GP(p):*(uint8_t *)GP(p)) {
            uint16_t *s=string(p,wide);
            uint16_t **strings=realloc(c->strings,(c->nstrings+1)*sizeof(*strings));
            if(!strings) {
                free(s);
                return -1;
            }
            c->strings=strings;
            c->strings[c->nstrings++]=s;
            p+=(wstrlen(s)+1)*(wide ? 2:1);
        }
        return first;
    }
    if(m==0x0414 || m==0x0444 || m==0x0415 || m==0x0443) {
        int add=m==0x0414 || m==0x0444,wide=m==0x0444 || m==0x0443,count=add ? (int)wp:1,at=add ? c->n:(int)wp;
        if(count<0 || count>4096 || !lp) return 0;
        const uint8_t *data=GP(lp);
        for(int k=0;k<count;k++) {
            int i=insert_item(c,at+k);
            if(i<0) return 0;
            const uint32_t *b=(const uint32_t *)(data+k*20);
            struct item *item=&c->items[i];
            item->image=b[0];
            item->id=b[1];
            item->state=data[k*20+8];
            item->style=data[k*20+9];
            item->param=b[3];
            item->text=b[4]<(uint32_t)c->nstrings ? ui_wdup(c->strings[b[4]],-1):b[4]>=0x10000 && b[4]!=TEXT_CALLBACK ? string(b[4],wide):ui_wdup((const uint16_t *)u"",0);
        }
        ui_invalidate(h);
        return 1;
    }
    if(m==0x0416) {
        if(wp>=(uint32_t)c->n) return 0;
        free_item(&c->items[wp]);
        memmove(c->items+wp,c->items+wp+1,(c->n-wp-1)*sizeof(*c->items));
        c->n--;
        ui_invalidate(h);
        return 1;
    }
    if(m==0x0417) {
        if(wp>=(uint32_t)c->n || !lp) return 0;
        uint32_t *b=GP(lp);
        memset(b,0,20);
        b[0]=c->items[wp].image;
        b[1]=c->items[wp].id;
        ((uint8_t *)b)[8]=c->items[wp].state;
        ((uint8_t *)b)[9]=c->items[wp].style;
        b[3]=c->items[wp].param;
        b[4]=TEXT_CALLBACK;
        return 1;
    }
    if(m==0x0419) return index_of(c,wp);
    if(m==0x041D) {
        if(wp>=(uint32_t)c->n || !lp) return 0;
        HDC dc=gdi_window_dc(h);
        *(RECT *)GP(lp)=item_rect(c,ui_win(h),dc,wp);
        gdi_release_dc(dc);
        return 1;
    }
    if(m==0x042D || m==0x044B) {
        int i=index_of(c,wp);
        if(i<0) return -1;
        return put_text(lp,32767,c->items[i].text,m==0x044B);
    }
    if(m>=0x0401 && m<=0x0412) {
        int i=index_of(c,wp);
        if(i<0) return -1;
        uint32_t bit=0;
        if(m==0x0401 || m==0x0409) bit=4;
        else if(m==0x0402 || m==0x040A) bit=1;
        else if(m==0x0403 || m==0x040B) bit=2;
        else if(m==0x0404 || m==0x040C) bit=8;
        if(m==0x0412) return c->items[i].state;
        if(m==0x0411) c->items[i].state=lp;
        else if(m>=0x0409) return (c->items[i].state&bit)!=0;
        else c->items[i].state=(c->items[i].state&~bit)|(lp ? bit:0);
        ui_invalidate(h);
        return 1;
    }
    *handled=0;
    return 0;
}
static void tooltip_show(HWND h,int show)
{
    struct common *c=get(h);
    if(!c) return;
    if(!show || !c->tip || !c->tip[0]) {
        U(ShowWindow,h,0);
        return;
    }
    HDC dc=gdi_window_dc(h);
    SIZE size=ui_measure(dc,c->tip,-1);
    gdi_release_dc(dc);
    int width=size.cx+8;
    if(c->tip_width>0 && width>c->tip_width) width=c->tip_width;
    U(SetWindowPos,h,0,c->min,c->max,width,size.cy+8,0x14);
    U(ShowWindow,h,4);
    ui_invalidate(h);
}
void common_mouse(HWND target,UINT message,POINT screen)
{
    HWND tips[256];
    int count=0;
    for(struct common *c=all;c && count<256;c=c->next) if(c->kind==TOOLTIP) tips[count++]=c->hwnd;
    for(int n=0;n<count;n++) {
        struct common *c=get(tips[n]);
        if(!c) continue;
        int hit=-1;
        for(int i=0;i<c->n;i++) {
            struct item *item=&c->items[i];
            if(!(item->style&0x10)) continue;
            if(item->style&1) {
                if(item->parent==target) hit=i;
            }
            else if(item->param==target) {
                POINT local=screen;
                gp_t p=ui_memory(&local,sizeof(local));
                U(ScreenToClient,target,p);
                local=*(POINT *)GP(p);
                gfree(p);
                RECT r=item->rect;
                if(local.x>=r.left && local.x<r.right && local.y>=r.top && local.y<r.bottom) hit=i;
            }
        }
        if(message!=WM_MOUSEMOVE || hit<0) {
            c->hot=-1;
            U(KillTimer,tips[n],1);
            tooltip_show(tips[n],0);
        }
        else if(c->hot!=hit || !c->tip) {
            c->hot=hit;
            free(c->tip);
            c->tip=ui_wdup(c->items[hit].text,-1);
            c->min=screen.x+16;
            c->max=screen.y+20;
            U(SetTimer,tips[n],1,500,0);
        }
    }
}
static LRESULT common_proc(HWND h,UINT m,WPARAM wp,LPARAM lp)
{
    struct win *w=ui_win(h);
    if(!w) return 0;
    struct common *c=w->common;
    if(m==WM_NCCREATE) {
        c=calloc(1,sizeof(*c));
        if(!c) return 0;
        for(int i=0;i<9;i++) {
            uint16_t text[64];
            ansi_to_wide(names[i],-1,text,64);
            if(!wstrcasecmp(text,w->cls->name)) c->kind=i;
        }
        c->hwnd=h;
        c->next=all;
        all=c;
        w->common=c;
        c->selected=-1;
        c->hot=-1;
        c->min=0;
        c->max=100;
        c->step=10;
        c->line=1;
        c->page=10;
        c->base=10;
        c->width=90;
        c->height=24;
        c->tip_width=320;
        c->background=0xFFFFFFFF;
        c->foreground=sys_color(18);
        c->bar=sys_color(13);
        c->unicode=1;
        if(c->kind==TOOLTIP) {
            w->exstyle|=0x08000080;
            w->style|=WS_POPUP;
        }
        return ui_default(h,m,wp,lp,1);
    }
    if(!c) return ui_default(h,m,wp,lp,1);
    if(m==WM_CREATE) {
        if(w->parent) {
            int unicode=ui_send(w->parent,0x55,h,3,1)==2;
            c=get(h);
            if(!c) return 0;
            c->unicode=unicode;
        }
        if(c->kind==UPDOWN && (w->style&0x10)) c->buddy=w->next_sibling;
        return 0;
    }
    if(m==WM_SETFONT) {
        c->font=wp;
        if(lp) ui_invalidate(h);
        return 0;
    }
    if(m==WM_GETFONT) return c->font;
    if(m==WM_GETDLGCODE) return 1;
    if(m==0x2005) {
        int old=c->unicode;
        c->unicode=wp!=0;
        return old;
    }
    if(m==0x2006) return c->unicode;
    if(m==WM_ERASEBKGND) return 1;
    if(m==WM_PAINT) {
        struct paintstruct ps;
        HDC dc=ui_begin(h,&ps);
        if(get(h)) paint(h,dc);
        ui_end(h,&ps);
        return 0;
    }
    if(m==WM_SIZE || m==WM_SETFOCUS || m==WM_KILLFOCUS || m==0x000A) {
        ui_invalidate(h);
        return 0;
    }
    int handled=0;
    LRESULT result=0;
    if(c->kind==LISTVIEW) result=list_message(h,m,wp,lp,&handled);
    if(c->kind==TREEVIEW) result=tree_message(h,m,wp,lp,&handled);
    if(c->kind==TAB) result=tab_message(h,m,wp,lp,&handled);
    if(c->kind==TOOLBAR) result=toolbar_message(h,m,wp,lp,&handled);
    if(handled) return result;
    c=get(h);
    w=ui_win(h);
    if(!c || !w) return result;
    if(c->kind==STATUS) {
        if(m==0x404) {
            if(wp>64 || (wp && !lp)) return 0;
            c->nparts=wp;
            memcpy(c->edges,GP(lp),wp*4);
            while(c->n<(int)wp) if(insert_item(c,c->n)<0) return 0;
            ui_invalidate(h);
            return 1;
        }
        if(m==0x406) {
            int n=wp<(uint32_t)c->nparts ? (int)wp:c->nparts;
            if(lp) memcpy(GP(lp),c->edges,n*4);
            return c->nparts;
        }
        if(m==0x409) {
            c->simple=wp!=0;
            ui_invalidate(h);
            return 0;
        }
        if(m==0x40E) return c->simple;
        if(m==0x401 || m==0x40B) {
            int i=wp&255;
            if(i==255) i=0;
            if(i>=64) return 0;
            while(c->n<=i) if(insert_item(c,c->n)<0) return 0;
            uint16_t *s=string(lp,m==0x40B);
            free(c->items[i].text);
            c->items[i].text=s;
            c->items[i].style=wp&~255u;
            ui_invalidate(h);
            return 1;
        }
        if(m==0x402 || m==0x40D || m==0x403 || m==0x40C) {
            int i=wp&255;
            if(i>=c->n) return 0;
            int n=wstrlen(c->items[i].text);
            if(m==0x402 || m==0x40D) put_text(lp,n+1,c->items[i].text,m==0x40D);
            return (uint16_t)n|(c->items[i].style<<16);
        }
        if(m==0x40A && lp) {
            if(wp>=(uint32_t)c->nparts) return 0;
            *(RECT *)GP(lp)=(RECT){
                wp ? c->edges[wp-1]:0,0,c->edges[wp]<0 ? w->cw:c->edges[wp],w->ch
            };
            return 1;
        }
        if(m==0x408) {
            c->height=wp;
            return 0;
        }
    }
    if(c->kind==PROGRESS) {
        if(m==0x401 || m==0x406) {
            uint32_t old=(uint16_t)c->min|(uint32_t)(uint16_t)c->max<<16;
            c->min=m==0x401 ? (int16_t)lp:(int)wp;
            c->max=m==0x401 ? (int16_t)(lp>>16):(int)lp;
            if(c->max<c->min) c->max=c->min;
            c->pos=clamp(c,c->pos);
            ui_invalidate(h);
            return old;
        }
        if(m==0x402 || m==0x403 || m==0x405) {
            int old=c->pos;
            c->pos=clamp(c,m==0x402 ? (int)wp:c->pos+(m==0x405 ? c->step:(int)wp));
            ui_invalidate(h);
            return old;
        }
        if(m==0x404) {
            int old=c->step;
            c->step=wp;
            return old;
        }
        if(m==0x408) return c->pos;
        if(m==0x407) {
            if(lp) {
                ((int *)GP(lp))[0]=c->min;
                ((int *)GP(lp))[1]=c->max;
            }
            return wp ? c->min:c->max;
        }
        if(m==0x409 || m==0x2001) {
            COLORREF old=m==0x409 ? c->bar:c->background;
            if(m==0x409) c->bar=lp;
            else c->background=lp;
            ui_invalidate(h);
            return old;
        }
        if(m==0x40A) {
            c->marquee=wp!=0;
            if(wp) U(SetTimer,h,1,lp ? lp:60,0);
            else U(KillTimer,h,1);
            return 1;
        }
        if(m==WM_TIMER && c->marquee) {
            c->pos=c->pos>=c->max ? c->min:c->pos+1;
            ui_invalidate(h);
            return 0;
        }
    }
    if(c->kind==TRACKBAR) {
        if(m==0x400) return c->pos;
        if(m==0x401) return c->min;
        if(m==0x402) return c->max;
        if(m==0x405) {
            c->pos=clamp(c,lp);
            if(wp) ui_invalidate(h);
            return 0;
        }
        if(m==0x406) {
            c->min=(int16_t)lp;
            c->max=(int16_t)(lp>>16);
            c->pos=clamp(c,c->pos);
            if(wp) ui_invalidate(h);
            return 0;
        }
        if(m==0x407 || m==0x408) {
            if(m==0x407) c->min=lp;
            else c->max=lp;
            c->pos=clamp(c,c->pos);
            if(wp) ui_invalidate(h);
            return 0;
        }
        if(m==0x415 || m==0x417) {
            int old=m==0x415 ? c->page:c->line;
            if(m==0x415) c->page=lp;
            else c->line=lp;
            return old;
        }
        if(m==0x416) return c->page;
        if(m==0x418) return c->line;
        if(m==0x414) {
            c->tick_freq=wp ? wp:1;
            return 0;
        }
        if(m==0x410) return (c->max-c->min)/(c->tick_freq ? c->tick_freq:1)+1;
        if(m==0x419 || m==0x41A) {
            if(!lp) return 0;
            *(RECT *)GP(lp)=m==0x41A ? (RECT){
                8,w->ch/2-2,w->cw-8,w->ch/2+2
            }
            :(RECT){
                0,0,9,w->ch
            };
            return 0;
        }
    }
    if(c->kind==UPDOWN) {
        if(m==0x469) {
            HWND old=c->buddy;
            c->buddy=wp;
            return old;
        }
        if(m==0x46A) return c->buddy;
        if(m==0x465 || m==0x46F) {
            c->min=m==0x465 ? (int16_t)(lp>>16):(int)wp;
            c->max=m==0x465 ? (int16_t)lp:(int)lp;
            c->pos=clamp(c,c->pos);
            return 0;
        }
        if(m==0x466) return (uint16_t)c->max|(uint32_t)(uint16_t)c->min<<16;
        if(m==0x470) {
            if(wp) *(int *)GP(wp)=c->min;
            if(lp) *(int *)GP(lp)=c->max;
            return 0;
        }
        if(m==0x467 || m==0x471) {
            int old=c->pos;
            c->pos=clamp(c,m==0x467 ? (int16_t)lp:(int)lp);
            ui_invalidate(h);
            if(c->buddy && (w->style&2)) U(SetDlgItemInt,w->parent,ui_win(c->buddy) ? ui_win(c->buddy)->id:0,c->pos,1);
            return old;
        }
        if(m==0x468 || m==0x472) {
            if(m==0x472 && lp) *(BOOL *)GP(lp)=0;
            return c->pos;
        }
        if(m==0x46D) {
            int old=c->base;
            if(wp==10 || wp==16) c->base=wp;
            return old;
        }
        if(m==0x46E) return c->base;
    }
    if(c->kind==TOOLTIP) {
        if(m==0x404 || m==0x432 || m==0x405 || m==0x433 || m==0x409 || m==0x436 || m==0x40C || m==0x439 || m==0x408 || m==0x435 || m==0x40B || m==0x438 || m==0x411) {
            uint32_t *info=GP(lp);
            if(!info || info[0]<40) return 0;
            int wide=m>=0x432,i=-1;
            for(int k=0;k<c->n;k++) if(c->items[k].param==info[2] && c->items[k].parent==info[3]) {
                i=k;
                break;
            }
            if(m==0x404 || m==0x432) {
                if(i>=0) return 0;
                i=insert_item(c,c->n);
                if(i<0) return 0;
                c->items[i].param=info[2];
                c->items[i].parent=info[3];
            }
            if(i<0) return 0;
            if(m==0x405 || m==0x433) {
                free_item(&c->items[i]);
                memmove(c->items+i,c->items+i+1,(c->n-i-1)*sizeof(*c->items));
                c->n--;
                return 1;
            }
            if(m==0x408 || m==0x435 || m==0x40B || m==0x438) {
                put_text(info[9],wp ? wp:80,c->items[i].text,wide);
                return 1;
            }
            if(m==0x411) {
                free(c->tip);
                c->tip=ui_wdup(c->items[i].text,-1);
                tooltip_show(h,wp!=0);
                return 0;
            }
            c->items[i].style=info[1];
            memcpy(&c->items[i].rect,info+4,sizeof(RECT));
            uint16_t *s=string(info[9],wide);
            free(c->items[i].text);
            c->items[i].text=s;
            return 1;
        }
        if(m==0x40D) return c->n;
        if(m==0x412) {
            c->min=(int16_t)lp;
            c->max=(int16_t)(lp>>16);
            return 0;
        }
        if(m==0x418) {
            int old=c->tip_width;
            c->tip_width=lp;
            return old;
        }
        if(m==0x419) return c->tip_width;
        if(m==0x41C || (m==0x401 && !wp)) {
            tooltip_show(h,0);
            return 0;
        }
        if(m==0x407 && lp && c->n) {
            struct msg *message=GP(lp);
            if(message->message==WM_MOUSEMOVE) {
                free(c->tip);
                c->tip=ui_wdup(c->items[0].text,-1);
                c->min=message->pt.x+16;
                c->max=message->pt.y+20;
                U(SetTimer,h,1,500,0);
            }
            else if(message->message==WM_LBUTTONDOWN) tooltip_show(h,0);
            return 0;
        }
        if(m==WM_TIMER) {
            U(KillTimer,h,1);
            tooltip_show(h,1);
            return 0;
        }
    }
    if(m==WM_LBUTTONDOWN || m==WM_LBUTTONDBLCLK || m==WM_MOUSEMOVE || m==WM_LBUTTONUP || m==WM_KEYDOWN) {
        int kind=c->kind,x=(int16_t)lp,y=(int16_t)(lp>>16),chosen=-1;
        if(m==WM_LBUTTONDOWN) {
            U(SetFocus,h);
            c=get(h);
            w=ui_win(h);
            if(!c || !w) return 0;
        }
        if(kind==TRACKBAR) {
            int vertical=w->style&2,len=vertical ? w->ch:w->cw,at=vertical ? y:x;
            if(m==WM_LBUTTONDOWN) {
                c->drag=1;
                U(SetCapture,h);
            }
            c=get(h);
            w=ui_win(h);
            if(!c || !w) return 0;
            if(m==WM_KEYDOWN) {
                if(wp==0x25 || wp==0x26) c->pos=clamp(c,c->pos-c->line);
                else if(wp==0x27 || wp==0x28) c->pos=clamp(c,c->pos+c->line);
                else if(wp==0x21) c->pos=clamp(c,c->pos-c->page);
                else if(wp==0x22) c->pos=clamp(c,c->pos+c->page);
                else if(wp==0x24) c->pos=c->min;
                else if(wp==0x23) c->pos=c->max;
                else return 0;
            }
            else if(c->drag) c->pos=clamp(c,c->min+(len>20 ? (int)((int64_t)(at-10)*(c->max-c->min)/(len-20)):0));
            else return 0;
            int pos=c->pos,code=m==WM_LBUTTONUP ? 4:5;
            HWND parent=w->parent;
            if(m==WM_LBUTTONUP) {
                c->drag=0;
                U(ReleaseCapture);
            }
            ui_invalidate(h);
            ui_send(parent,vertical ? 0x115:0x114,((uint32_t)(uint16_t)pos<<16)|code,h,1);
            return 0;
        }
        if(kind==UPDOWN && (m==WM_LBUTTONDOWN || m==WM_KEYDOWN)) {
            int delta=m==WM_KEYDOWN ? (wp==0x26 || wp==0x27 ? 1:wp==0x28 || wp==0x25 ? -1:0):(w->style&0x40 ? (x<w->cw/2 ? -1:1):(y<w->ch/2 ? 1:-1));
            int tail[]={
                c->pos,delta
            };
            if(!delta || notify(h,-722,tail,sizeof(tail))) return 0;
            c=get(h);
            w=ui_win(h);
            if(!c || !w) return 0;
            c->pos=clamp(c,c->pos+delta);
            int pos=c->pos;
            HWND buddy=c->buddy,parent=w->parent;
            if(buddy && (w->style&2)) {
                char text[32];
                snprintf(text,sizeof(text),"%d",pos);
                gp_t g=ui_memory(text,strlen(text)+1);
                U(SetWindowTextA,buddy,g);
                gfree(g);
            }
            ui_invalidate(h);
            ui_send(parent,w->style&0x40 ? 0x114:0x115,((uint32_t)(uint16_t)pos<<16)|4,h,1);
            return 0;
        }
        if(m==WM_KEYDOWN && (kind==LISTVIEW || kind==TREEVIEW || kind==TAB)) {
            chosen=c->selected;
            if(wp==0x25 || wp==0x26) chosen--;
            else if(wp==0x27 || wp==0x28) chosen++;
            else if(wp==0x24) chosen=0;
            else if(wp==0x23) chosen=c->n-1;
            else if(wp==VK_RETURN) {
                notify(h,-4,NULL,0);
                return 0;
            }
            else return 0;
        }
        else if(m==WM_LBUTTONDOWN || m==WM_LBUTTONDBLCLK || (kind==TOOLBAR && m==WM_LBUTTONUP)) {
            HDC dc=gdi_window_dc(h);
            if(c->font) G(SelectObject,dc,c->font);
            int lh=row_height(dc);
            if(kind==TREEVIEW) chosen=visible_index(c,c->first+y/lh);
            else for(int i=0;i<c->n;i++) {
                RECT r=item_rect(c,w,dc,i);
                if(x>=r.left && x<r.right && y>=r.top && y<r.bottom) {
                    chosen=i;
                    break;
                }
            }
            gdi_release_dc(dc);
        }
        if(chosen>=0 && chosen<c->n) {
            if(kind==TOOLBAR && m==WM_LBUTTONUP) {
                struct item item=c->items[chosen];
                HWND parent=w->parent;
                if(!(item.state&4) || (item.style&1)) return 0;
                if(item.style&2) c->items[chosen].state^=1;
                ui_invalidate(h);
                ui_send(parent,WM_COMMAND,item.id,h,1);
                return 0;
            }
            if(kind==TREEVIEW && m==WM_LBUTTONDOWN && x<tree_depth(c,chosen)*16+16) {
                tree_message(h,0x1102,3,c->items[chosen].id,&handled);
                return 0;
            }
            if(kind==LISTVIEW || kind==TREEVIEW || kind==TAB) {
                select_item(h,chosen);
                if(get(h) && m==WM_LBUTTONDBLCLK) notify(h,-3,NULL,0);
                else if(get(h) && m==WM_LBUTTONDOWN) notify(h,-2,NULL,0);
                return 0;
            }
        }
    }
    return ui_default(h,m,wp,lp,1);
}
DEF(CommonControlProcW) {
    return common_proc(A(0),A(1),A(2),A(3));
}
DEF(InitCommonControls) {
    (void)a;
    return 0;
}
DEF(InitCommonControlsEx) {
    return A(0) && ((uint32_t *)P(0))[0]>=8;
}
DEF(DllGetVersion) {
    if(!A(0) || *(uint32_t *)P(0)<20) return 0x80070057;
    uint32_t *v=P(0);
    v[1]=6;
    v[2]=0;
    v[3]=2600;
    v[4]=1;
    return 0;
}
static const struct api_entry common_entries[]={
    API(CommonControlProcW,4),API(InitCommonControls,0),API(InitCommonControlsEx,1),API(DllGetVersion,1),
#define IL_API(name,n) {#name,api_##name,n,0,NULL,0},
#include "imagelist-api.h"
#undef IL_API
};
static void common_init(void)
{
    U(GetDesktopWindow);
    for(int i=0;i<9;i++) ui_class_dll(names[i],&comctl32_dll,&common_entries[0],0);
}
struct api_dll comctl32_dll={
    .name="COMCTL32.DLL",.entries=common_entries,.count=sizeof(common_entries)/sizeof(common_entries[0]),.init=common_init
};
