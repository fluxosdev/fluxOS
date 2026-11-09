/* Shell folder, launch, PIDL and desktop file-drop integration. */
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include "wmconn.h"
#include "os.h"
#include "ui.h"
static const char *folder(int id)
{
    switch(id&0xFF) {
        case 0: case 0x10:return "C:\\users\\flux\\Desktop";
        case 2:return "C:\\users\\flux\\Start Menu\\Programs";
        case 5:return "C:\\users\\flux\\Documents";
        case 6:return "C:\\users\\flux\\Favorites";
        case 7:return "C:\\users\\flux\\Start Menu\\Programs\\Startup";
        case 8:return "C:\\users\\flux\\Recent";
        case 9:return "C:\\users\\flux\\SendTo";
        case 0xB:return "C:\\users\\flux\\Start Menu";
        case 0xD:return "C:\\users\\flux\\Music";
        case 0xE:return "C:\\users\\flux\\Videos";
        case 0x14:return "C:\\WINDOWS\\Fonts";
        case 0x1A:return "C:\\users\\flux\\AppData\\Roaming";
        case 0x1C:return "C:\\users\\flux\\AppData\\Local";
        case 0x23:return "C:\\ProgramData";
        case 0x24:return "C:\\WINDOWS";
        case 0x25:return "C:\\WINDOWS\\SYSTEM32";
        case 0x26:return "C:\\Program Files";
        case 0x27:return "C:\\users\\flux\\Pictures";
        case 0x28:return "C:\\users\\flux";
        case 0x2B:return "C:\\Program Files\\Common Files";
        default:return NULL;
    }
}
static uint32_t folder_path(uint32_t *a,int wide)
{
    const char *win=folder(A(1));
    if(!win || !A(4)) return 0x80070057u;
    if(A(1)&0x8000) {
        char path[PATH_MAX];
        if(path_to_host(win,path)<0) return 0x80070003u;
        for(char *p=path+1;;p++) if(*p=='/' || !*p) {
            int end=!*p;
            char c=*p;
            *p=0;
            if(mkdir(path,0755)<0 && errno!=EEXIST) return 0x80070005u;
            *p=c;
            if(end) break;
        }
    }
    if(wide) ansi_to_wide(win,-1,P(4),MAX_PATH);
    else strcpy(P(4),win);
    return 0;
}
DEF(SHGetFolderPathA) {
    return folder_path(a,0);
}
DEF(SHGetFolderPathW) {
    return folder_path(a,1);
}
DEF(SHGetSpecialFolderPathA) {
    uint32_t b[]={
        A(0),A(2)|(A(3) ? 0x8000:0),0,0,A(1)
    };
    return folder_path(b,0)==0;
}
DEF(SHGetSpecialFolderPathW) {
    uint32_t b[]={
        A(0),A(2)|(A(3) ? 0x8000:0),0,0,A(1)
    };
    return folder_path(b,1)==0;
}
static char *argument(gp_t p,int wide) {
    uint16_t *w=ui_string(p,wide);
    int n=wstrlen(w);
    char *s=malloc((size_t)n*4+1);
    if(s) {
        int len=wide_to_utf8(w,n,s,n*4);
        s[len]=0;
    }
    free(w);
    return s;
}
static int suffix(const char *path,const char *extension) {
    size_t n=strlen(path),e=strlen(extension);
    return n>=e && !strcasecmp(path+n-e,extension);
}
static uint32_t shell_execute(gp_t verb,gp_t file,gp_t params,gp_t directory,int wide,HANDLE *process)
{
    char *operation=argument(verb,wide),*name=argument(file,wide),*parameters=argument(params,wide),*dir=argument(directory,wide);
    uint32_t result=2;
    if(!operation || !name || !parameters || !dir) {
        result=8;
        goto done;
    }
    if(operation[0] && strcasecmp(operation,"open") && strcasecmp(operation,"explore")) {
        result=31;
        goto done;
    }
    if(!name[0]) goto done;
    char host[PATH_MAX],cwd[PATH_MAX],storage[4096];
    char *words[64];
    const char *argv[70];
    int n=0;
    int url=!strncasecmp(name,"http://",7) || !strncasecmp(name,"https://",8) || !strncasecmp(name,"mailto:",7);
    if(!url && path_to_host(name,host)<0) goto done;
    if(!url) path_fix_case(host);
    if(dir[0]) {
        if(path_to_host(dir,cwd)<0) goto done;
    }
    else if(!getcwd(cwd,sizeof(cwd))) strcpy(cwd,"/winapps");
    struct stat st;
    if(!url && stat(host,&st)<0) goto done;
    const char *program;
    uint16_t subsystem;
    int dll=0;
    if(url) {
        program="wave";
        argv[n++]=name;
    }
    else if(S_ISDIR(st.st_mode)) {
        program="files";
        argv[n++]=host;
    }
    else if(!pe_file_info(host,&subsystem,&dll) && !dll) {
        program="winrun";
        argv[n++]=host;
    }
    else if(suffix(host,".png") || suffix(host,".jpg") || suffix(host,".jpeg") || suffix(host,".bmp") || suffix(host,".gif") || suffix(host,".ico")) {
        program="imageview";
        argv[n++]=host;
    }
    else if(suffix(host,".wav") || suffix(host,".mp3") || suffix(host,".ogg") || suffix(host,".mp4") || suffix(host,".avi") || suffix(host,".flac")) {
        program="mediaplayer";
        argv[n++]=host;
    }
    else if(suffix(host,".htm") || suffix(host,".html")) {
        program="wave";
        argv[n++]=host;
    }
    else if(suffix(host,".zip") || suffix(host,".7z")) {
        program="archiver";
        argv[n++]=host;
    }
    else if(access(host,X_OK)==0 && !suffix(host,".exe")) {
        program=host;
    }
    else {
        program="editor";
        argv[n++]=host;
    }
    int count=win_split_command_line(parameters,words,64,storage,sizeof(storage));
    for(int i=0;i<count && n<68;i++) argv[n++]=words[i];
    argv[n]=NULL;
    char executable[PATH_MAX];
    if(program[0]=='/') snprintf(executable,sizeof(executable),"%s",program);
    else snprintf(executable,sizeof(executable),"/bin/%s",program);
    if(access(executable,X_OK)<0) {
        result=2;
        goto done;
    }
    char *args[72];
    args[0]=executable;
    for(int i=0;i<=n;i++) args[i+1]=(char *)argv[i];
    int fds[3]={
        -1,-1,-1
    };
    if(process) {
        HANDLE h=handle_new(H_PROCESS);
        struct hobj *o=handle_get(h);
        if(!o) {
            result=8;
            goto done;
        }
        int pid=os_spawn(executable,args,NULL,cwd,fds);
        if(pid<0) {
            handle_close(h);
            result=8;
            goto done;
        }
        o->pid=pid;
        *process=h;
    }
    else {
        /* The intermediate child is reaped here; init reaps the detached program. */
        int pid=fork();
        if(pid<0) {
            result=8;
            goto done;
        }
        if(!pid) {
            int child=fork();
            if(child<0) _exit(1);
            if(child) _exit(0);
            if(chdir(cwd)<0) _exit(1);
            execv(executable,args);
            _exit(1);
        }
        int status=0,reaped;
        do {reaped=waitpid(pid,&status,0);} while(reaped<0 && errno==EINTR);
        if(reaped!=pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
            result=8;
            goto done;
        }
    }
    result=33;
    done:
    free(operation);
    free(name);
    free(parameters);
    free(dir);
    return result;
}
DEF(ShellExecuteA) {
    return shell_execute(A(1),A(2),A(3),A(4),0,NULL);
}
DEF(ShellExecuteW) {
    return shell_execute(A(1),A(2),A(3),A(4),1,NULL);
}
static uint32_t execute_ex(uint32_t *a,int wide)
{
    uint32_t *info=P(0);
    if(!info || info[0]<60) return 0;
    HANDLE process=0;
    uint32_t result=shell_execute(info[3],info[4],info[5],info[6],wide,info[1]&0x40 ? &process:NULL);
    info[8]=result;
    if(info[0]>=60) info[14]=process;
    if(result<=32) {
        set_last_error(result==2 ? ERROR_FILE_NOT_FOUND:ERROR_ACCESS_DENIED);
        return 0;
    }
    return 1;
}
DEF(ShellExecuteExA) {
    return execute_ex(a,0);
}
DEF(ShellExecuteExW) {
    return execute_ex(a,1);
}
gp_t shell_pidl(const char *host)
{
    int n=strlen(host);
    uint16_t *w=calloc((size_t)n+1,2);
    if(!w) return 0;
    utf8_to_wide(host,-1,w,n+1);
    n=wstrlen(w);
    if(n>32000) {
        free(w);
        return 0;
    }
    uint16_t size=6+(n+1)*2;
    gp_t p=galloc(size+2);
    uint8_t *data=GP(p);
    memcpy(data,&size,2);
    uint32_t magic=0x584C5546;
    memcpy(data+2,&magic,4);
    memcpy(data+6,w,(n+1)*2);
    free(w);
    return p;
}
static char *pidl_path(gp_t p)
{
    if(!p || !vm_readable(p,8)) return NULL;
    const uint8_t *data=GP(p);
    uint16_t size;
    uint32_t magic;
    memcpy(&size,data,2);
    memcpy(&magic,data+2,4);
    if(size<8 || magic!=0x584C5546 || !vm_readable(p,size+2) || ((size-6)&1)) return NULL;
    int count=(size-6)/2;
    const uint16_t *w=(const uint16_t *)(data+6);
    if(w[count-1]) return NULL;
    char *s=malloc((size_t)count*4+1);
    if(!s) return NULL;
    int n=wide_to_utf8(w,count,s,count*4);
    s[n]=0;
    return s;
}
static uint32_t path_from_pidl(uint32_t *a,int wide)
{
    char *native=pidl_path(A(0));
    if(!native || !A(1)) {
        free(native);
        return 0;
    }
    char win[PATH_MAX];
    path_to_win(native,win,sizeof(win));
    free(native);
    if(strlen(win)>=MAX_PATH) return 0;
    if(wide) utf8_to_wide(win,-1,P(1),MAX_PATH);
    else snprintf(P(1),MAX_PATH,"%s",win);
    return 1;
}
DEF(SHGetPathFromIDListA) {
    return path_from_pidl(a,0);
}
DEF(SHGetPathFromIDListW) {
    return path_from_pidl(a,1);
}
DEF(SHGetFolderLocation)
{
    const char *win=folder(A(1));
    if(!win || !A(4)) return 0x80070057;
    char host[PATH_MAX];
    if(path_to_host(win,host)<0) return 0x80070003;
    *(gp_t *)P(4)=shell_pidl(host);
    return *(gp_t *)P(4) ? 0:0x8007000E;
}
DEF(SHGetSpecialFolderLocation) {
    uint32_t args[]={
        A(0),A(1),0,0,A(2)
    };
    return api_SHGetFolderLocation(args);
}
static uint32_t browse(uint32_t *a,int wide)
{
    uint32_t *info=P(0);
    if(!info || !info[2]) return 0;
    char *root=info[1] ? pidl_path(info[1]):NULL,*title=argument(info[3],wide);
    char *selected=ui_browse_path(info[0],title && title[0] ? title:"Select Folder",root ? root:"/winapps",info[5],info[6]);
    free(root);
    free(title);
    if(!selected) return 0;
    gp_t pidl=shell_pidl(selected);
    char win[PATH_MAX];
    path_to_win(selected,win,sizeof(win));
    free(selected);
    const char *base=strrchr(win,'\\');
    base=base ? base+1:win;
    if(wide) utf8_to_wide(base,-1,GP(info[2]),MAX_PATH);
    else snprintf(GP(info[2]),MAX_PATH,"%s",base);
    info[7]=0;
    return pidl;
}
DEF(SHBrowseForFolderA) {
    return browse(a,0);
}
DEF(SHBrowseForFolderW) {
    return browse(a,1);
}
DEF(ILFree) {
    if(A(0)) gfree(A(0));
    return 0;
}
DEF(ILGetSize) {
    if(!A(0) || !vm_readable(A(0),2)) return 0;
    uint16_t size=*(uint16_t *)P(0);
    return vm_readable(A(0),size+2) ? size+2:0;
}
DEF(ILClone) {
    uint32_t n=api_ILGetSize(a);
    return n ? ui_memory(P(0),n):0;
}
DEF(DragAcceptFiles) {
    struct win *w=ui_win(A(0));
    if(w) {
        w->accept_files=A(1)!=0;
        w->exstyle=(w->exstyle&~0x10u)|(A(1) ? 0x10:0);
    }
    return 0;
}
static struct drag_data drop_data;
static uint32_t drop_serial;
void shell_drop(HWND h,const struct wm_msg *message)
{
    struct win *w=ui_win(h);
    if(!w || wm_payload_len(message)<sizeof(struct wm_drop)) return;
    const struct wm_drop *drop=wm_payload(message);
    if(drop->drag!=drop_serial) {
        drag_data_free(&drop_data);
        drop_serial=drop->drag;
    }
    if(drop->has_data) {
        drag_data_free(&drop_data);
        if(wm_drag_unpack(drop+1,wm_payload_len(message)-sizeof(*drop),&drop_data)<0) memset(&drop_data,0,sizeof(drop_data));
    }
    int effect=(w->accept_files || (w->exstyle&0x10)) && drop_data.npaths>0 ? DROP_COPY:DROP_NONE;
    gui_drop_reply(w->desk_id,message->serial,effect);
    if(drop->phase!=DROP_DO || effect==DROP_NONE) return;
    size_t chars=1;
    for(int i=0;i<drop_data.npaths;i++) chars+=strlen(drop_data.paths[i])+4;
    if(chars>1024*1024) return;
    HANDLE memory=K(GlobalAlloc,0x42,20+chars*2);
    gp_t bits=K(GlobalLock,memory);
    if(!bits) {
        K(GlobalFree,memory);
        return;
    }
    uint32_t *header=GP(bits);
    header[0]=20;
    header[1]=drop->x;
    header[2]=drop->y-w->menu_height;
    header[3]=0;
    header[4]=1;
    uint16_t *out=(uint16_t *)(header+5);
    for(int i=0;i<drop_data.npaths;i++) {
        char win[PATH_MAX];
        path_to_win(drop_data.paths[i],win,sizeof(win));
        int n=utf8_to_wide(win,-1,out,chars);
        out+=n;
        chars-=n;
    }
    *out=0;
    K(GlobalUnlock,memory);
    U(PostMessageW,h,0x233,memory,0);
}
static uint32_t query_drop(uint32_t *a,int wide)
{
    uint32_t size=K(GlobalSize,A(0));
    gp_t data=K(GlobalLock,A(0));
    if(!data || size<20) {
        if(data) K(GlobalUnlock,A(0));
        return 0;
    }
    const uint32_t *header=GP(data);
    uint32_t offset=header[0],count=0;
    int unit=header[4] ? 2:1,result=0;
    if(offset>=size) goto done;
    while(offset+(uint32_t)unit<=size) {
        const uint8_t *start=(const uint8_t *)GP(data)+offset;
        int n=0;
        while(offset+(uint32_t)(n+1)*unit<=size && (unit==2 ? ((const uint16_t *)start)[n]:start[n])) n++;
        if(!n || offset+(uint32_t)(n+1)*unit>size) break;
        uint32_t next=offset+(n+1)*unit;
        if(A(1)==count) {
            uint16_t *text=unit==2 ? ui_wdup((const uint16_t *)start,n):calloc(n+1,2);
            if(unit==1 && text) ansi_to_wide((const char *)start,n,text,n);
            if(text) {
                result=n;
                if(A(2) && A(3)) {
                    if(n>=(int)A(3)) n=A(3)-1;
                    if(wide) {
                        memcpy(P(2),text,n*2);
                        ((uint16_t *)P(2))[n]=0;
                    }
                    else {
                        wide_to_ansi(text,n,P(2),n);
                        ((char *)P(2))[n]=0;
                    }
                    result=n;
                }
                free(text);
            }
        }
        count++;
        offset=next;
    }
    if(A(1)==0xFFFFFFFFu) result=count;
    done:
    K(GlobalUnlock,A(0));
    return result;
}
DEF(DragQueryFileA) {
    return query_drop(a,0);
}
DEF(DragQueryFileW) {
    return query_drop(a,1);
}
DEF(DragQueryPoint) {
    gp_t p=K(GlobalLock,A(0));
    if(!p || K(GlobalSize,A(0))<20) {
        if(p) K(GlobalUnlock,A(0));
        return 0;
    }
    memcpy(P(1),(uint32_t *)GP(p)+1,8);
    int client=!((uint32_t *)GP(p))[3];
    K(GlobalUnlock,A(0));
    return client;
}
DEF(DragFinish) {
    K(GlobalFree,A(0));
    return 0;
}
static const struct api_entry shell_entries[]={
    API(SHGetFolderPathA,5),API(SHGetFolderPathW,5),API(SHGetSpecialFolderPathA,4),API(SHGetSpecialFolderPathW,4),
    API(ShellExecuteA,6),API(ShellExecuteW,6),API(ShellExecuteExA,1),API(ShellExecuteExW,1),API(SHBrowseForFolderA,1),API(SHBrowseForFolderW,1),
    API(SHGetPathFromIDListA,2),API(SHGetPathFromIDListW,2),API(SHGetFolderLocation,5),API(SHGetSpecialFolderLocation,3),API(ILFree,1),API(ILGetSize,1),API(ILClone,1),
    API(DragAcceptFiles,2),API(DragQueryFileA,4),API(DragQueryFileW,4),API(DragQueryPoint,2),API(DragFinish,1)
};
struct api_dll shell32_dll={
    .name="SHELL32.DLL",.entries=shell_entries,.count=sizeof(shell_entries)/sizeof(shell_entries[0])
};
