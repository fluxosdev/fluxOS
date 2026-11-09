#include "winapi.h"
static int failures,notifications,drops;static HWND root;static HANDLE ready,finish;static HWND worker_window;static DWORD worker_id;
static int equal(const char *a,const char *b) {while(*a && *a==*b) {a++;b++;}return *a==*b;}
#define CHECK(x) do {if(!(x)) {DWORD written;const char message[]="phase3 FAIL: " #x "\n";W(WriteFile)(W(GetStdHandle)(-11),message,sizeof(message)-1,&written,0);if(!failures++) W(SetWindowTextA)(root,"phase3-failed:" #x);}}while(0)
static LRESULT send(HWND h,UINT m,WPARAM w,void *p) {return W(SendMessageA)(h,m,w,(LPARAM)p);}
static HWND control(const char *cls,UINT style,int y) {HWND h=W(CreateWindowExA)(0,cls,"",WS_CHILD|WS_VISIBLE|style,0,y,300,28,root,(HMENU)(y+1),0,0);CHECK(h);return h;}
static DWORD WINAPI worker(void *unused) {
    (void)unused;worker_window=W(CreateWindowExA)(0,"Phase3","",0,0,0,20,20,0,0,0,0);MSG m;W(PeekMessageA)(&m,0,0,0,0);
    CHECK(!W(GetFocus)());unsigned char keys[256]={0};keys['A']=0x80;W(SetKeyboardState)(keys);W(SetEvent)(ready);
    while(W(WaitForSingleObject)(finish,0)!=0) {while(W(PeekMessageA)(&m,0,0,0,1)) W(DispatchMessageA)(&m);W(Sleep)(1);}W(DestroyWindow)(worker_window);return 27;
}
static UINT WINAPI hook(HWND h,UINT msg,WPARAM wp,LPARAM lp) {(void)wp;(void)lp;if(msg==0x110) CHECK(!W(IsWindowEnabled)(root) && h);return 0;}
static UINT WINAPI cancel_hook(HWND h,UINT m,WPARAM w,LPARAM l) {(void)w;(void)l;if(m==0x110) W(EndDialog)(h,0);return 0;}
static UINT WINAPI short_hook(HWND h,UINT m,WPARAM w,LPARAM l) {(void)w;(void)l;if(m==0x110) {W(SetDlgItemTextA)(h,100,"phase3.txt");send(h,0x111,1,0);}return 0;}
static int WINAPI browse_hook(HWND h,UINT msg,LPARAM lp,LPARAM data) {(void)lp;(void)data;if(msg==1) send(h,0x466,1,"C:\\users\\flux\\Documents");return 0;}
struct ofn {DWORD size;HWND owner;HANDLE instance;const void *filter;void *custom;DWORD maxcustom,index;void *file;DWORD maxfile;void *filetitle;DWORD maxtitle;const void *initial,*title;DWORD flags;unsigned short offset,extoffset;const void *extension;LPARAM data;void *hook;const void *template;};
static void dialogs(void) {
    char name[260]="phase3.txt";struct ofn f={0};f.size=sizeof(f);f.owner=root;f.file=name;f.maxfile=260;f.initial="C:\\users\\flux\\Documents";f.flags=0x1000|8|0x20;f.hook=hook;
    CHECK(W(GetOpenFileNameA)(&f));CHECK(equal(name,"C:\\users\\flux\\Documents\\phase3.txt"));CHECK(f.offset==24 && f.extoffset==31);CHECK(!W(CommDlgExtendedError)());
    char cancelled[32]="unchanged";f.file=cancelled;f.maxfile=32;f.hook=cancel_hook;
    CHECK(!W(GetOpenFileNameA)(&f) && equal(cancelled,"unchanged") && !W(CommDlgExtendedError)());
    char tiny[2]={0};f.file=tiny;f.maxfile=2;f.hook=short_hook;
    CHECK(!W(GetOpenFileNameA)(&f) && W(CommDlgExtendedError)()==0x3003 && *(unsigned short *)tiny==35);
    f.maxfile=260;
    unsigned short out[260]={'s','a','v','e',0};f.file=out;f.initial=u"C:\\users\\flux\\Documents";f.extension=u"txt";f.flags=8;f.hook=0;
    CHECK(W(GetSaveFileNameW)(&f));CHECK(out[24]=='s' && out[28]=='.' && out[29]=='t');
    DWORD colors[16]={0},cc[9]={36,(DWORD)root,0,0x332211,(DWORD)colors,1|0x10,0,(DWORD)hook,0};CHECK(W(ChooseColorA)(cc));CHECK(cc[3]==0x332211);
    unsigned char font[92]={0};DWORD cf[15]={60,(DWORD)root,0,(DWORD)font,120,8,0,0,(DWORD)hook,0,0,0,0,0,0};CHECK(W(ChooseFontW)(cf));CHECK(cf[4]==120 && *(int *)font==-16);
    char display[260],path[260];DWORD bi[8]={(DWORD)root,0,(DWORD)display,(DWORD)"Select Folder",1,(DWORD)browse_hook,0,0};void *pidl=W(SHBrowseForFolderA)(bi);CHECK(pidl);CHECK(W(SHGetPathFromIDListA)(pidl,path));CHECK(equal(path,"C:\\users\\flux\\Documents"));W(CoTaskMemFree)(pidl);
    W(SetWindowTextA)(root,failures ? "phase3-failed":"phase3-ok");
}
static LRESULT WINAPI proc(HWND h,UINT m,WPARAM wp,LPARAM lp) {
    if(m==0x111 && (wp&65535)==901) {W(SetWindowTextA)(root,"phase3-click");W(SetFocus)(root);return 0;}
    if(m==0x4E) {notifications++;return 0;}
    if(m==WM_CHAR && wp=='d') {dialogs();return 0;}
    if(m==0x233) {char a[260],b[260],shortname[3];POINT p;CHECK(W(DragQueryFileA)((HANDLE)wp,-1,0,0)==2);CHECK(W(DragQueryFileA)((HANDLE)wp,0,shortname,3)==2);W(DragQueryFileA)((HANDLE)wp,0,a,260);W(DragQueryFileA)((HANDLE)wp,1,b,260);CHECK(equal(a,"C:\\users\\flux\\Documents\\phase3.txt"));CHECK(equal(b,"Z:\\test\\console.exe"));CHECK(W(DragQueryPoint)((HANDLE)wp,&p));W(DragFinish)((HANDLE)wp);drops++;return 0;}
    if(m==WM_DESTROY) {if(h==root) W(PostQuitMessage)(failures ? 1:19);return 0;}return W(DefWindowProcA)(h,m,wp,lp);
}
void start(void) {
    WNDCLASSA cls={0};cls.lpfnWndProc=proc;cls.lpszClassName="Phase3";cls.hbrBackground=W(GetSysColorBrush)(15);W(RegisterClassA)(&cls);
    root=W(CreateWindowExA)(0,"Phase3","Phase3",WS_OVERLAPPEDWINDOW,40,40,340,350,0,0,0,0);CHECK(root);
    DWORD init[2]={8,0xFFFF};CHECK(W(InitCommonControlsEx)(init));
    HWND list=control("SysListView32",1,0);DWORD column[8]={6,0,150,(DWORD)"Name",0,0,0,0};CHECK(send(list,0x101B,0,column)==0);
    DWORD item[10]={0xF,0,0,0,0,(DWORD)"Alpha",0,-1,123,0};CHECK(send(list,0x1007,0,item)==0);CHECK(send(list,0x1004,0,0)==1);
    char text[32];item[5]=(DWORD)text;item[6]=32;CHECK(send(list,0x102D,0,item)==5 && equal(text,"Alpha"));
    HWND tree=control("SysTreeView32",0,35);DWORD ti[12]={0xFFFF0000,0xFFFF0002,1,0,0,0,(DWORD)"root",0,0,0,0,0};DWORD parent=send(tree,0x1100,0,ti);CHECK(parent);ti[0]=parent;ti[6]=(DWORD)"child";DWORD child=send(tree,0x1100,0,ti);CHECK(child);CHECK(send(tree,0x110A,4,(void *)parent)==(int)child);CHECK(send(tree,0x110B,9,(void *)child));
    HWND tab=control("SysTabControl32",0,70);DWORD tabitem[7]={1,0,0,(DWORD)"Tab",0,0,0};CHECK(send(tab,0x1307,0,tabitem)==0);CHECK(send(tab,0x1304,0,0)==1);
    HWND progress=control("msctls_progress32",0,105);send(progress,0x406,10,(void *)200);send(progress,0x402,123,0);CHECK(send(progress,0x408,0,0)==123);
    HWND track=control("msctls_trackbar32",0,140);send(track,0x405,1,(void *)42);CHECK(send(track,0x400,0,0)==42);
    HWND status=control("msctls_statusbar32",0,175);int edges[2]={100,-1};CHECK(send(status,0x404,2,edges));CHECK(send(status,0x401,1,"Ready"));CHECK((send(status,0x402,1,text)&65535)==5 && equal(text,"Ready"));
    HWND toolbar=control("ToolbarWindow32",0,210);DWORD tb[5]={0,901,4,0,(DWORD)"Run"};CHECK(send(toolbar,0x414,1,tb));CHECK(send(toolbar,0x418,0,0)==1);
    HWND up=control("msctls_updown32",0,245);send(up,0x46F,0,(void *)500);send(up,0x471,0,(void *)200);CHECK(send(up,0x472,0,0)==200);
    HWND tip=control("tooltips_class32",0,280);DWORD tool[10]={40,0,(DWORD)root,1,0,0,100,100,0,(DWORD)"Tip"};CHECK(send(tip,0x404,0,tool));CHECK(send(tip,0x40D,0,0)==1);W(DestroyWindow)(tip);
    HANDLE images=W(ImageList_Create)(2,2,0x20,1,1);CHECK(images);HDC dc=W(CreateCompatibleDC)(0);HDC screen=W(GetDC)(root);HBITMAP bitmap=W(CreateCompatibleBitmap)(screen,4,2);W(ReleaseDC)(root,screen);HGDIOBJ old=W(SelectObject)(dc,bitmap);W(SetPixel)(dc,0,0,RGB(1,2,3));W(SelectObject)(dc,old);CHECK(W(ImageList_Add)(images,bitmap,0)==0);CHECK(W(ImageList_GetImageCount)(images)==2);W(SelectObject)(dc,bitmap);W(SetPixel)(dc,0,0,0);CHECK(W(ImageList_Draw)(images,0,dc,0,0,0));CHECK(W(GetPixel)(dc,0,0)==RGB(1,2,3));CHECK(W(ImageList_Remove)(images,0));CHECK(W(ImageList_Destroy)(images));CHECK(!W(ImageList_Destroy)(images));W(SelectObject)(dc,old);W(DeleteObject)(bitmap);W(DeleteDC)(dc);
    W(SetFocus)(list);ready=W(CreateEventA)(0,1,0,0);finish=W(CreateEventA)(0,1,0,0);HANDLE thread=W(CreateThread)(0,0,worker,0,0,&worker_id);CHECK(thread);CHECK(W(WaitForSingleObject)(ready,5000)==0);CHECK(!(W(GetKeyState)('A')&0x8000));CHECK(W(AttachThreadInput)(W(GetCurrentThreadId)(),worker_id,1));CHECK(W(GetFocus)()==list);W(SetFocus)(worker_window);CHECK(W(GetFocus)()==worker_window);CHECK(W(AttachThreadInput)(W(GetCurrentThreadId)(),worker_id,0));CHECK(!W(GetFocus)());W(SetEvent)(finish);CHECK(W(WaitForSingleObject)(thread,5000)==0);W(CloseHandle)(thread);W(CloseHandle)(ready);W(CloseHandle)(finish);
    HANDLE file=W(CreateFileA)("C:\\users\\flux\\Documents\\phase3.txt",0x40000000,3,0,2,0,0);CHECK(file!=(HANDLE)-1);W(CloseHandle)(file);
    DWORD ex[15]={60,0x40,(DWORD)root,0,(DWORD)"Z:\\test\\console.exe",0,0,1,0,0,0,0,0,0,0};CHECK(W(ShellExecuteExA)(ex));CHECK(ex[14]);CHECK(W(WaitForSingleObject)((HANDLE)ex[14],5000)==0);DWORD code;CHECK(W(GetExitCodeProcess)((HANDLE)ex[14],&code) && code==5);W(CloseHandle)((HANDLE)ex[14]);
    W(DragAcceptFiles)(root,1);W(ShowWindow)(root,1);W(UpdateWindow)(root);W(SetWindowTextA)(root,failures ? "phase3-failed":"phase3-ready");
    MSG m;while(W(GetMessageA)(&m,0,0,0)>0) {W(TranslateMessage)(&m);W(DispatchMessageA)(&m);}W(ExitProcess)(failures || !drops ? 1:19);
}
