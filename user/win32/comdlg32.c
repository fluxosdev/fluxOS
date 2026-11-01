/* Standard dialogs composed from the same themed windows and controls as other Win32
 * dialogs. The desktop currently has no separate file-picker service to call. */
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include "ui.h"
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
extern struct api_dll comdlg32_dll;
static pthread_key_t error_key;
static void error_set(uint32_t error) {
    pthread_setspecific(error_key,(void *)(uintptr_t)error);
}
static const uint16_t *wide_empty=(const uint16_t *)u"";
struct entry {
    char *name;
    int directory;
};
struct chooser {
    int mode,save,directory,wide;
    HWND hwnd,owner;
    gp_t hook,argument,browse_callback,browse_data;
    char path[PATH_MAX],initial[PATH_MAX],pattern[512],extension[128];
    struct entry *entries;
    int count;
    char **chosen;
    int nchosen;
    uint32_t flags;
    COLORREF rgb;
    uint8_t logfont[92];
    int points;
};
static uint16_t *utf16(const char *s) {
    size_t n=strlen(s);
    uint16_t *w=calloc(n+1,2);
    if(!w) return NULL;
    utf8_to_wide(s,-1,w,n+1);
    return w;
}
static char *utf8(const uint16_t *w) {
    int n=wstrlen(w);
    char *s=malloc((size_t)n*4+1);
    if(!s) return NULL;
    int len=wide_to_utf8(w,n,s,n*4);
    s[len]=0;
    return s;
}
static char *argument_string(gp_t p,int wide) {
    uint16_t *w=ui_string(p,wide);
    char *s=utf8(w);
    free(w);
    return s;
}
static void set_control(HWND h,int id,const char *s) {
    uint16_t *w=utf16(s);
    gp_t p=ui_guest(w ? w:wide_empty,1);
    U(SetDlgItemTextW,h,id,p);
    gfree(p);
    free(w);
}
static char *get_control(HWND h,int id)
{
    gp_t p=galloc(PATH_MAX*2);
    U(GetDlgItemTextW,h,id,p,PATH_MAX);
    char *s=utf8(GP(p));
    gfree(p);
    return s;
}
static int matches(const char *pattern,const char *s)
{
    if(!strcmp(pattern,"*.*")) pattern="*";
    const char *star=NULL,*retry=NULL;
    while(*s) {
        if(*pattern=='?' || (*pattern && ((*pattern|32)==(*s|32)))) {
            pattern++;
            s++;
        }
        else if(*pattern=='*') {
            star=pattern++;
            retry=s;
        }
        else if(star) {
            pattern=star+1;
            s=++retry;
        }
        else return 0;
    }
    while(*pattern=='*') pattern++;
    return !*pattern;
}
static int accepted(const char *patterns,const char *name)
{
    if(!patterns[0]) return 1;
    char copy[512];
    snprintf(copy,sizeof(copy),"%s",patterns);
    char *p=copy;
    while(p) {
        char *end=strchr(p,';');
        if(end) *end=0;
        if(matches(p,name)) return 1;
        p=end ? end+1:NULL;
    }
    return 0;
}
static int compare_entry(const void *a,const void *b)
{
    const struct entry *x=a,*y=b;
    if(x->directory!=y->directory) return y->directory-x->directory;
    return strcasecmp(x->name,y->name);
}
static void clear_entries(struct chooser *c) {
    for(int i=0;i<c->count;i++) free(c->entries[i].name);
    free(c->entries);
    c->entries=NULL;
    c->count=0;
}
static int fill_directory(struct chooser *c)
{
    char canonical[PATH_MAX];
    if(realpath(c->path,canonical)) snprintf(c->path,sizeof(c->path),"%s",canonical);
    DIR *dir=opendir(c->path);
    if(!dir) return 0;
    clear_entries(c);
    struct dirent *e;
    while((e=readdir(dir))) {
        if(!strcmp(e->d_name,".") || (!strcmp(e->d_name,"..") && !strcmp(c->path,"/"))) continue;
        char path[PATH_MAX];
        int length=snprintf(path,sizeof(path),"%s/%s",c->path,e->d_name);
        if(length>=(int)sizeof(path)) continue;
        struct stat st;
        if(stat(path,&st)<0) continue;
        int directory=S_ISDIR(st.st_mode);
        if(!directory && (c->directory || !accepted(c->pattern,e->d_name))) continue;
        struct entry *entries=realloc(c->entries,(c->count+1)*sizeof(*entries));
        if(!entries) {
            closedir(dir);
            return 0;
        }
        c->entries=entries;
        entries[c->count].name=strdup(e->d_name);
        if(!entries[c->count].name) {
            closedir(dir);
            return 0;
        }
        entries[c->count++].directory=directory;
    }
    closedir(dir);
    qsort(c->entries,c->count,sizeof(*c->entries),compare_entry);
    HWND list=U(GetDlgItem,c->hwnd,101);
    ui_send(list,0x184,0,0,1);
    for(int i=0;i<c->count;i++) {
        char display[512];
        snprintf(display,sizeof(display),c->entries[i].directory ? "[%s]":"%s",c->entries[i].name);
        uint16_t *w=utf16(display);
        gp_t p=ui_guest(w ? w:wide_empty,1);
        ui_send(list,0x180,0,p,1);
        gfree(p);
        free(w);
    }
    set_control(c->hwnd,103,c->path);
    ui_invalidate(list);
    return 1;
}
static void notice(HWND h,const char *message)
{
    gp_t text=ui_memory(message,strlen(message)+1),title=ui_memory("File selection",15);
    U(MessageBoxA,h,text,title,0x30);
    gfree(text);
    gfree(title);
}
static int question(HWND h,const char *message)
{
    gp_t text=ui_memory(message,strlen(message)+1),title=ui_memory("Confirm",8);
    int yes=U(MessageBoxA,h,text,title,4)==6;
    gfree(text);
    gfree(title);
    return yes;
}
static int resolve_path(struct chooser *c,const char *name,char *out)
{
    if(name[0]=='/') return snprintf(out,PATH_MAX,"%s",name)<PATH_MAX;
    if(strchr(name,'\\') || (name[0] && name[1]==':')) return path_to_host(name,out)==0;
    return snprintf(out,PATH_MAX,"%s%s%s",c->path,!strcmp(c->path,"/") ? "":"/",name)<PATH_MAX;
}
static void accept_path(struct chooser *c)
{
    char *name=get_control(c->hwnd,100);
    if(!name) return;
    char path[PATH_MAX];
    if(!name[0] && !c->directory) {
        free(name);
        return;
    }
    if(!name[0]) snprintf(path,sizeof(path),"%s",c->path);
    else if(!resolve_path(c,name,path)) {
        free(name);
        notice(c->hwnd,"The path is too long.");
        return;
    }
    free(name);
    path_fix_case(path);
    char canonical[PATH_MAX];
    if(realpath(path,canonical)) snprintf(path,sizeof(path),"%s",canonical);
    struct stat st;
    if(!stat(path,&st) && S_ISDIR(st.st_mode) && !c->directory) {
        snprintf(c->path,sizeof(c->path),"%s",path);
        set_control(c->hwnd,100,"");
        fill_directory(c);
        return;
    }
    if(!c->directory && c->extension[0]) {
        char *base=strrchr(path,'/');
        base=base ? base+1:path;
        if(!strchr(base,'.')) {
            size_t n=strlen(path);
            snprintf(path+n,sizeof(path)-n,".%s",c->extension);
        }
    }
    int exists=stat(path,&st)==0;
    if(c->directory && (!exists || !S_ISDIR(st.st_mode))) {
        notice(c->hwnd,"Choose an existing folder.");
        return;
    }
    char parent[PATH_MAX];
    snprintf(parent,sizeof(parent),"%s",path);
    char *slash=strrchr(parent,'/');
    if(slash && slash!=parent) *slash=0;
    else strcpy(parent,"/");
    if((c->flags&0x800) && (stat(parent,&st)<0 || !S_ISDIR(st.st_mode))) {
        notice(c->hwnd,"The containing folder does not exist.");
        return;
    }
    if(!c->save && !exists && (c->flags&0x1000)) {
        notice(c->hwnd,"The file does not exist.");
        return;
    }
    if(c->save && exists && (c->flags&2) && !question(c->hwnd,"Replace the existing file?")) return;
    if(!c->save && !exists && (c->flags&0x2000) && !question(c->hwnd,"Use this new file name?")) return;
    c->chosen=calloc(1,sizeof(*c->chosen));
    if(!c->chosen) return;
    c->chosen[0]=strdup(path);
    if(!c->chosen[0]) {
        free(c->chosen);
        c->chosen=NULL;
        return;
    }
    c->nchosen=1;
    U(EndDialog,c->hwnd,1);
}
static void color_fields(struct chooser *c) {
    U(SetDlgItemInt,c->hwnd,100,c->rgb&255,0);
    U(SetDlgItemInt,c->hwnd,101,(c->rgb>>8)&255,0);
    U(SetDlgItemInt,c->hwnd,102,(c->rgb>>16)&255,0);
    ui_invalidate(c->hwnd);
}
static LRESULT chooser_proc(HWND h,UINT message,WPARAM wp,LPARAM lp)
{
    struct win *w=ui_win(h);
    struct chooser *c=w ? w->aux:NULL;
    if(!c) return 0;
    if(c->hook) {
        LRESULT handled=ui_proc(c->hook,c->wide,h,message,wp,message==WM_INITDIALOG ? c->argument:lp,1);
        if(!ui_win(h) || handled) return handled;
    }
    if(c->browse_callback) {
        if(message==WM_INITDIALOG) {
            call_guest4(c->browse_callback,h,1,0,c->browse_data);
            return 1;
        }
        if(message==0x465) {
            U(EnableWindow,U(GetDlgItem,h,1),lp!=0);
            return 1;
        }
        if(message==0x464 || message==0x468) {
            char *s=argument_string(lp,message==0x468);
            set_control(h,103,s ? s:"");
            free(s);
            return 1;
        }
        if((message==0x466 || message==0x467) && wp) {
            char *s=argument_string(lp,message==0x467);
            char host[PATH_MAX];
            if(s && path_to_host(s,host)==0) {
                snprintf(c->path,sizeof(c->path),"%s",host);
                set_control(h,100,"");
                fill_directory(c);
            }
            free(s);
            return 1;
        }
    }
    if(message==WM_INITDIALOG) return 1;
    if(message==WM_CLOSE) {
        U(EndDialog,h,0);
        return 1;
    }
    if(message==WM_PAINT && c->mode==1) {
        struct paintstruct ps;
        HDC dc=ui_begin(h,&ps);
        struct win *win=ui_win(h);
        if(win) {
            ui_fill(dc,0,0,win->cw,win->ch,sys_color(15));
            ui_fill(dc,250,20,100,100,c->rgb);
            ui_frame(dc,250,20,100,100,sys_color(16));
        }
        ui_end(h,&ps);
        return 1;
    }
    if(message==WM_COMMAND) {
        int id=wp&0xFFFF,code=wp>>16;
        if(id==2) {
            U(EndDialog,h,0);
            return 1;
        }
        if(c->mode==0) {
            if(id==1) {
                accept_path(c);
                return 1;
            }
            if(id==102) {
                char *slash=strrchr(c->path,'/');
                if(slash && slash!=c->path) *slash=0;
                else strcpy(c->path,"/");
                fill_directory(c);
                return 1;
            }
            if(id==101 && (code==1 || code==2)) {
                int i=ui_send(U(GetDlgItem,h,101),0x188,0,0,1);
                if(i>=0 && i<c->count) {
                    set_control(h,100,c->entries[i].name);
                    if(code==2) {
                        if(c->entries[i].directory) {
                            char next[PATH_MAX];
                            if(resolve_path(c,c->entries[i].name,next)) {
                                snprintf(c->path,sizeof(c->path),"%s",next);
                                fill_directory(c);
                                set_control(h,100,"");
                            }
                        }
                        else accept_path(c);
                    }
                }
                return 1;
            }
        }
        else if(c->mode==1) {
            if(id>=200 && id<216) {
                static const COLORREF colors[]={
                    0,0xFFFFFF,0x0000FF,0x00FF00,0xFF0000,0x00FFFF,0xFF00FF,0xFFFF00,0x808080,0x000080,0x008000,0x800000,0x008080,0x800080,0x808000,0xC0C0C0
                };
                c->rgb=colors[id-200];
                color_fields(c);
                return 1;
            }
            if(id==1 || (id>=100 && id<=102 && code==0x300)) {
                int rgb[3];
                for(int i=0;i<3;i++) {
                    BOOL translated=0;
                    gp_t valid=ui_memory(&translated,sizeof(translated));
                    rgb[i]=U(GetDlgItemInt,h,100+i,valid,0);
                    translated=*(BOOL *)GP(valid);
                    gfree(valid);
                    if(!translated || rgb[i]<0 || rgb[i]>255) return 1;
                }
                c->rgb=rgb[0]|rgb[1]<<8|rgb[2]<<16;
                ui_invalidate(h);
                if(id==1) U(EndDialog,h,1);
                return 1;
            }
        }
        else if(c->mode==2 && id==1) {
            int points=U(GetDlgItemInt,h,101,0,0);
            if(points<1 || points>512) return 1;
            c->points=points;
            HWND combo=U(GetDlgItem,h,100);
            gp_t face=galloc(64);
            U(GetWindowTextW,combo,face,32);
            memcpy(c->logfont+28,GP(face),64);
            gfree(face);
            int32_t height=-(points*96+36)/72,weight=ui_send(U(GetDlgItem,h,102),0xF0,0,0,1) ? 700:400;
            memcpy(c->logfont,&height,4);
            memcpy(c->logfont+16,&weight,4);
            c->logfont[20]=ui_send(U(GetDlgItem,h,103),0xF0,0,0,1)!=0;
            c->logfont[21]=ui_send(U(GetDlgItem,h,104),0xF0,0,0,1)!=0;
            c->logfont[22]=ui_send(U(GetDlgItem,h,105),0xF0,0,0,1)!=0;
            U(EndDialog,h,1);
            return 1;
        }
    }
    return 0;
}
DEF(CommonDialogProcW) {
    return chooser_proc(A(0),A(1),A(2),A(3));
}
static void button(HWND h,const uint16_t *text,int x,int y,int id) {
    ui_control(h,(const uint16_t *)u"BUTTON",text,0x10000|(id==1 ? 1:0),x,y,88,26,id);
}
static int run_chooser(struct chooser *c,const char *title)
{
    HWND previous=U(GetFocus);
    int enabled=c->owner && U(IsWindowEnabled,c->owner);
    if(enabled) U(EnableWindow,c->owner,0);
    uint16_t *caption=utf16(title);
    gp_t proc=api_thunk(&comdlg32_dll,api_find(&comdlg32_dll,"CommonDialogProcW"));
    c->hwnd=ui_dialog_new(caption ? caption:wide_empty,c->owner,c->mode==0 ? 520:380,c->mode==0 ? 360:220,proc,0);
    free(caption);
    if(!c->hwnd) {
        if(enabled) U(EnableWindow,c->owner,1);
        error_set(0xFFFF);
        return 0;
    }
    ui_win(c->hwnd)->aux=c;
    HWND h=c->hwnd;
    if(c->mode==0) {
        ui_control(h,(const uint16_t *)u"STATIC",wide_empty,0,10,8,400,22,103);
        button(h,(const uint16_t *)u"Up",420,8,102);
        ui_control(h,(const uint16_t *)u"LISTBOX",wide_empty,0x10001|0x800000|0x200000,10,38,498,240,101);
        ui_control(h,(const uint16_t *)u"EDIT",wide_empty,0x10000|0x800000,10,290,498,24,100);
        set_control(h,100,c->initial);
        fill_directory(c);
        button(h,c->directory ? (const uint16_t *)u"Select":c->save ? (const uint16_t *)u"Save":(const uint16_t *)u"Open",320,326,1);
        button(h,(const uint16_t *)u"Cancel",420,326,2);
    }
    else if(c->mode==1) {
        const uint16_t *labels[]={
            (const uint16_t *)u"Red",(const uint16_t *)u"Green",(const uint16_t *)u"Blue"
        };
        for(int i=0;i<3;i++) {
            ui_control(h,(const uint16_t *)u"STATIC",labels[i],0,10,20+i*32,60,24,300+i);
            ui_control(h,(const uint16_t *)u"EDIT",wide_empty,0x10000|0x800000|0x2000,80,20+i*32,72,24,100+i);
        }
        for(int i=0;i<16;i++) {
            char label[8];
            snprintf(label,sizeof(label),"%d",i+1);
            uint16_t *text=utf16(label);
            ui_control(h,(const uint16_t *)u"BUTTON",text,0x10000,10+(i%8)*44,128+(i/8)*26,40,24,200+i);
            free(text);
        }
        color_fields(c);
        button(h,(const uint16_t *)u"OK",180,188,1);
        button(h,(const uint16_t *)u"Cancel",280,188,2);
    }
    else {
        ui_control(h,(const uint16_t *)u"STATIC",(const uint16_t *)u"Font",0,10,12,70,24,300);
        HWND combo=ui_control(h,(const uint16_t *)u"COMBOBOX",wide_empty,0x10003,80,10,270,150,100);
        const uint16_t *faces[]={
            (const uint16_t *)u"Segoe UI",(const uint16_t *)u"Tahoma",(const uint16_t *)u"MS Sans Serif",(const uint16_t *)u"Courier New",(const uint16_t *)u"Consolas"
        };
        for(int i=0;i<5;i++) {
            gp_t face=ui_guest(faces[i],1);
            ui_send(combo,0x143,0,face,1);
            gfree(face);
        }
        ui_send(combo,0x14E,0,0,1);
        for(int i=0;i<5;i++) if(!wstrcasecmp((uint16_t *)(c->logfont+28),faces[i])) ui_send(combo,0x14E,i,0,1);
        ui_control(h,(const uint16_t *)u"STATIC",(const uint16_t *)u"Points",0,10,50,70,24,301);
        ui_control(h,(const uint16_t *)u"EDIT",wide_empty,0x10000|0x800000|0x2000,80,48,70,24,101);
        U(SetDlgItemInt,h,101,c->points,0);
        const uint16_t *labels[]={
            (const uint16_t *)u"Bold",(const uint16_t *)u"Italic",(const uint16_t *)u"Underline",(const uint16_t *)u"Strikeout"
        };
        int32_t weight;
        memcpy(&weight,c->logfont+16,4);
        for(int i=0;i<4;i++) {
            HWND box=ui_control(h,(const uint16_t *)u"BUTTON",labels[i],0x10003,10+(i%2)*180,86+(i/2)*32,170,26,102+i);
            ui_send(box,0xF1,i ? c->logfont[19+i]!=0:weight>=600,0,1);
        }
        button(h,(const uint16_t *)u"OK",180,180,1);
        button(h,(const uint16_t *)u"Cancel",280,180,2);
    }
    ui_send(h,WM_INITDIALOG,U(GetDlgItem,h,100),c->argument,1);
    if(ui_win(h)) U(SetFocus,U(GetDlgItem,h,100));
    int result=ui_win(h) ? ui_modal(h):0;
    if(enabled && ui_win(c->owner)) {
        U(EnableWindow,c->owner,1);
        U(SetForegroundWindow,c->owner);
        if(ui_win(previous)) U(SetFocus,previous);
    }
    clear_entries(c);
    return result==1;
}
char *ui_choose_path(HWND owner,const char *title,const char *initial,int save,int directory,const char *filter,const char *extension,uint32_t flags)
{
    struct chooser c={0};
    c.owner=owner;
    c.save=save;
    c.directory=directory;
    c.flags=flags;
    c.wide=1;
    snprintf(c.path,sizeof(c.path),"%s",initial && initial[0] ? initial:"/winapps");
    snprintf(c.pattern,sizeof(c.pattern),"%s",filter ? filter:"*");
    snprintf(c.extension,sizeof(c.extension),"%s",extension ? extension:"");
    if(!run_chooser(&c,title)) return NULL;
    char *result=c.chosen[0];
    free(c.chosen);
    return result;
}
char *ui_browse_path(HWND owner,const char *title,const char *initial,gp_t callback,LPARAM data)
{
    struct chooser c={0};
    c.owner=owner;
    c.directory=1;
    c.wide=1;
    c.browse_callback=callback;
    c.browse_data=data;
    snprintf(c.path,sizeof(c.path),"%s",initial);
    strcpy(c.pattern,"*");
    if(!run_chooser(&c,title)) return NULL;
    char *result=c.chosen[0];
    free(c.chosen);
    return result;
}
struct ofn {
    uint32_t size,owner,inst,filter,custom_filter,max_custom,filter_index,file,max_file,title_file,max_title_file,initial,title,flags;
    uint16_t offset,extension_offset;
    uint32_t extension,data,hook,template_name;
};
static uint32_t file_dialog(uint32_t *a,int wide,int save)
{
    struct ofn *f=P(0);
    error_set(0);
    if(!f || f->size<76 || !f->file || !f->max_file) {
        error_set(1);
        return 0;
    }
    if(f->flags&0xC0) {
        error_set(3);
        return 0;
    }
    struct chooser c={0};
    c.owner=f->owner;
    c.wide=wide;
    c.save=save;
    c.flags=f->flags;
    c.argument=A(0);
    c.hook=f->flags&0x20 ? f->hook:0;
    char *initial=argument_string(f->initial,wide),*filename=argument_string(f->file,wide),*title=argument_string(f->title,wide),*extension=argument_string(f->extension,wide);
    if(initial && initial[0] && path_to_host(initial,c.path)<0) c.path[0]=0;
    if(!c.path[0] && !getcwd(c.path,sizeof(c.path))) strcpy(c.path,"/winapps");
    if(filename) {
        char *slash=strrchr(filename,'\\');
        if(!slash) slash=strrchr(filename,'/');
        if(slash) {
            char native[PATH_MAX];
            if(path_to_host(filename,native)==0) {
                char *last=strrchr(native,'/');
                if(last) {
                    *last=0;
                    snprintf(c.path,sizeof(c.path),"%s",native[0] ? native:"/");
                }
            }
            snprintf(c.initial,sizeof(c.initial),"%s",slash+1);
        }
        else snprintf(c.initial,sizeof(c.initial),"%s",filename);
    }
    snprintf(c.extension,sizeof(c.extension),"%s",extension ? extension:"");
    strcpy(c.pattern,"*");
    if(f->filter) {
        gp_t p=f->filter;
        unsigned index=f->filter_index ? f->filter_index:1;
        for(unsigned i=1;i<=index && i<256;i++) {
            uint16_t *description=ui_string(p,wide);
            int len=wstrlen(description);
            free(description);
            if(!len) break;
            p+=(len+1)*(wide ? 2:1);
            char *pattern=argument_string(p,wide);
            if(!pattern) break;
            if(i==index) snprintf(c.pattern,sizeof(c.pattern),"%s",pattern);
            uint16_t *pw=ui_string(p,wide);
            p+=(wstrlen(pw)+1)*(wide ? 2:1);
            free(pw);
            free(pattern);
        }
    }
    int success=run_chooser(&c,title && title[0] ? title:save ? "Save As":"Open");
    free(initial);
    free(filename);
    free(title);
    free(extension);
    if(!success) return 0;
    char win[PATH_MAX];
    path_to_win(c.chosen[0],win,sizeof(win));
    free(c.chosen[0]);
    free(c.chosen);
    uint16_t *text=utf16(win);
    int n=text ? wstrlen(text):0;
    uint32_t needed=n+1+((f->flags&0x200) ? 1:0);
    if(needed>f->max_file) {
        if(f->max_file>=2) *(uint16_t *)GP(f->file)=needed;
        free(text);
        error_set(0x3003);
        return 0;
    }
    if(wide) {
        memcpy(GP(f->file),text,(n+1)*2);
        if(f->flags&0x200) ((uint16_t *)GP(f->file))[n+1]=0;
    }
    else {
        wide_to_ansi(text,n+1,GP(f->file),n+1);
        if(f->flags&0x200) ((char *)GP(f->file))[n+1]=0;
    }
    char *base=strrchr(win,'\\');
    base=base ? base+1:win;
        f->offset=0;f->extension_offset=0;
    for(int i=0;i<n;i++) if(text[i]=='\\') f->offset=i+1;
    for(int i=f->offset;i<n;i++) if(text[i]=='.') f->extension_offset=i+1;
    if(!f->filter_index) f->filter_index=1;
    if(f->title_file && f->max_title_file) {
        uint16_t *name=utf16(base);
        int count=wstrlen(name);
        if(count>=(int)f->max_title_file) count=f->max_title_file-1;
        if(wide) {
            memcpy(GP(f->title_file),name,count*2);
            ((uint16_t *)GP(f->title_file))[count]=0;
        }
        else {
            wide_to_ansi(name,count,GP(f->title_file),count);
            ((char *)GP(f->title_file))[count]=0;
        }
        free(name);
    }
    if(!(f->flags&8)) {
        char parent[PATH_MAX];
        path_to_host(win,parent);
        char *slash=strrchr(parent,'/');
        if(slash) {
            *slash=0;
            chdir(parent[0] ? parent:"/");
        }
    }
    free(text);
    return 1;
}
DEF(GetOpenFileNameA) {
    return file_dialog(a,0,0);
}
DEF(GetOpenFileNameW) {
    return file_dialog(a,1,0);
}
DEF(GetSaveFileNameA) {
    return file_dialog(a,0,1);
}
DEF(GetSaveFileNameW) {
    return file_dialog(a,1,1);
}
static uint32_t choose_color(uint32_t *a,int wide)
{
    uint32_t *p=P(0);
    error_set(0);
    if(!p || p[0]<36) {
        error_set(1);
        return 0;
    }
    if(p[5]&0x60) {
        error_set(3);
        return 0;
    }
    struct chooser c={0};
    c.mode=1;
    c.owner=p[1];
    c.rgb=p[5]&1 ? p[3]:0;
    c.wide=wide;
    c.hook=p[5]&0x10 ? p[7]:0;
    c.argument=A(0);
    if(!run_chooser(&c,"Color")) return 0;
    p[3]=c.rgb;
    return 1;
}
DEF(ChooseColorA) {
    return choose_color(a,0);
}
DEF(ChooseColorW) {
    return choose_color(a,1);
}
static uint32_t choose_font(uint32_t *a,int wide)
{
    uint32_t *p=P(0);
    error_set(0);
    if(!p || p[0]<60 || !p[3]) {
        error_set(1);
        return 0;
    }
    if(p[5]&0x30) {
        error_set(3);
        return 0;
    }
    struct chooser c={0};
    c.mode=2;
    c.owner=p[1];
    c.wide=wide;
    c.hook=p[5]&8 ? p[8]:0;
    c.argument=A(0);
    c.points=p[4]/10;
    if(c.points<1) c.points=10;
    if(p[5]&0x40) {
        memcpy(c.logfont,GP(p[3]),28);
        if(wide) memcpy(c.logfont+28,(uint8_t *)GP(p[3])+28,64);
        else ansi_to_wide((char *)GP(p[3])+28,-1,(uint16_t *)(c.logfont+28),32);
        int32_t height;
        memcpy(&height,c.logfont,4);
        if(height) c.points=abs(height)*72/96;
    }
    else {
        int32_t weight=400;
        memcpy(c.logfont+16,&weight,4);
        memcpy(c.logfont+28,u"Segoe UI",18);
    }
    c.logfont[90]=c.logfont[91]=0;
    if(!run_chooser(&c,"Font")) return 0;
    memcpy(GP(p[3]),c.logfont,28);
    if(wide) memcpy((uint8_t *)GP(p[3])+28,c.logfont+28,64);
    else wide_to_ansi((uint16_t *)(c.logfont+28),-1,(char *)GP(p[3])+28,32);
    p[4]=c.points*10;
    p[12]=(p[12]&0xFFFF0000)|0x2000;
    return 1;
}
DEF(ChooseFontA) {
    return choose_font(a,0);
}
DEF(ChooseFontW) {
    return choose_font(a,1);
}
DEF(CommDlgExtendedError) {
    (void)a;
    return (uint32_t)(uintptr_t)pthread_getspecific(error_key);
}
static void init(void) {
    pthread_key_create(&error_key,NULL);
}
static const struct api_entry entries[]={
    API(CommonDialogProcW,4),API(GetOpenFileNameA,1),API(GetOpenFileNameW,1),API(GetSaveFileNameA,1),API(GetSaveFileNameW,1),API(ChooseColorA,1),API(ChooseColorW,1),API(ChooseFontA,1),API(ChooseFontW,1),API(CommDlgExtendedError,0)
};
struct api_dll comdlg32_dll={
    .name="COMDLG32.DLL",.entries=entries,.count=sizeof(entries)/sizeof(entries[0]),.init=init
};
