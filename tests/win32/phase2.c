#include "winapi.h"
#define WM_COMMAND 0x0111
#define WM_INITDIALOG 0x0110
static HWND root, edit, combo;
static HINSTANCE instance;
static HANDLE accelerators;
static int failed,changes,stage;
static WNDPROC original;
#define CHECK(c) do { if(!(c) && !failed) failed=__LINE__; } while(0)
void *memset(void *p,int c,unsigned int n) {unsigned char *b=p;while(n--) *b++=c;return p;}
static int equal(const char *a,const char *b) {while(*a && *a==*b) {a++;b++;}return *a==*b;}
static void status(const char *text)
{
    if(failed) {
        char out[40]="phase2-failed-";int n=14,v=failed,d=1000;
        while(d>1 && v<d) d/=10;
        while(d) {out[n++]=(char)('0'+v/d);v%=d;d/=10;}out[n]=0;
        DWORD wrote;W(WriteFile)(W(GetStdHandle)((DWORD)-11),out,n,&wrote,0);
        W(SetWindowTextA)(root,out);
    } else W(SetWindowTextA)(root,text);
}
static LRESULT WINAPI subclass(HWND h,UINT msg,WPARAM wp,LPARAM lp)
{
    return W(CallWindowProcA)(original,h,msg,wp,lp);
}
static LRESULT WINAPI dialog_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp)
{
    if(msg==WM_INITDIALOG) {
        char text[24];BOOL translated=0;
        CHECK(W(GetDlgItem)(h,100)!=0);
        CHECK(W(GetDlgItemTextA)(h,100,text,24)==2 && equal(text,"42"));
        CHECK(W(GetDlgItemInt)(h,100,&translated,0)==42 && translated);
        CHECK(W(SetDlgItemInt)(h,100,55,0));
        RECT units={0,0,4,8};CHECK(W(MapDialogRect)(h,&units) && units.right>0 && units.bottom>0);
        if(lp==1 || lp==2) CHECK(!W(IsWindowEnabled)(root));
        if(lp==1) W(EndDialog)(h,17);
        return 1;
    }
    if(msg==WM_COMMAND && (wp&0xFFFF)==1) {
        CHECK(W(GetFocus)()==W(GetDlgItem)(h,1));
        CHECK(!W(IsWindowEnabled)(root));
        CHECK(W(GetDlgItemInt)(h,100,0,0)==55);
        W(EndDialog)(h,23);return 1;
    }
    return 0;
}
static LRESULT WINAPI proc(HWND h,UINT msg,WPARAM wp,LPARAM lp)
{
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps;HDC dc=W(BeginPaint)(h,&ps);RECT r;W(GetClientRect)(h,&r);
        W(FillRect)(dc,&r,(HBRUSH)6);W(EndPaint)(h,&ps);return 0;
    }
    if(msg==WM_COMMAND) {
        int id=wp&0xFFFF,code=wp>>16;
        if(id==100 && code==0x300) changes++;
        if(id==100 && code==0x100 && stage==1) status("phase2-edit");
        if(id==300 && code==0x300) {W(DestroyWindow)((HWND)lp);return 0;}
        if(id==205 && code==1) {CHECK(W(SendMessageA)(combo,0x147,0,0)==1);status("phase2-combo");return 0;}
        if(id==501) { if(stage==0) {CHECK(code==1);stage=1;status("phase2-accel");}else {CHECK(stage==2 && !code);stage=3;status("phase2-ok");}return 0; }
        if(id==200 && !code) {
            char text[32];CHECK(W(GetWindowTextA)(edit,text,32)==1 && text[0]=='Z');CHECK(changes>0);
            CHECK(W(DialogBoxParamA)(instance,(const char *)2,root,dialog_proc,2)==23);
            CHECK(W(IsWindowEnabled)(root));
            CHECK(W(MessageBoxA)(root,"Choose the default answer.","Question",0x104)==7);
            HMENU sub=W(GetSubMenu)(W(GetMenu)(root),0);
            CHECK(W(TrackPopupMenu)(sub,0x100,30,50,0,root,0)==501);
            stage=2;status("phase2-workflow");return 0;
        }
    }
    if(msg==WM_DESTROY) {W(PostQuitMessage)(failed || stage!=3 ? 1:13);return 0;}
    return W(DefWindowProcA)(h,msg,wp,lp);
}
static HWND child(const char *cls,const char *text,DWORD style,int x,int y,int width,int height,int id)
{
    HWND h=W(CreateWindowExA)(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,root,(HMENU)id,instance,0);
    CHECK(h!=0);return h;
}
void start(void)
{
    instance=W(GetModuleHandleA)(0);
    WNDCLASSA wc={0};wc.lpfnWndProc=proc;wc.lpszClassName="Phase2Class";wc.hInstance=instance;
    HICON icon=W(LoadIconA)(instance,(const char *)1);CHECK(icon!=0);W(RegisterClassA)(&wc);
    HMENU menu=W(LoadMenuA)(instance,(const char *)1);CHECK(menu!=0);
    CHECK(W(GetMenuItemCount)(menu)==1 && W(GetMenuItemID)(W(GetSubMenu)(menu,0),0)==501);
    char menu_text[32];UINT mi[]={48,0x42,0,0,0,0,0,0,0,(UINT)menu_text,32,0};
    CHECK(W(GetMenuItemInfoA)(W(GetSubMenu)(menu,0),0,1,mi) && mi[4]==501 && menu_text[0]=='&');
    CHECK(W(CheckMenuItem)(menu,501,8)==0);CHECK(W(GetMenuState)(menu,501,0)&8);
    CHECK(W(EnableMenuItem)(menu,501,1)==0);CHECK(W(EnableMenuItem)(menu,501,0)==1);
    HMENU ex=W(LoadMenuA)(instance,(const char *)2);CHECK(ex && W(GetMenuItemID)(W(GetSubMenu)(ex,0),0)==502);W(DestroyMenu)(ex);
    root=W(CreateWindowExA)(0,"Phase2Class","Phase2",WS_POPUP|WS_CLIPCHILDREN,0,0,320,240,0,menu,instance,0);
    CHECK(root!=0 && W(GetMenu)(root)==menu);
    RECT r;W(GetClientRect)(root,&r);CHECK(r.right==320 && r.bottom==220);
    edit=child("EDIT","hello",0x10000|0x800000|4,10,10,150,70,100);
    CHECK(W(SendMessageA)(edit,0xB1,1,4)==0);
    W(SendMessageA)(edit,0xC2,1,(LPARAM)"i");
    char buf[64];CHECK(W(GetWindowTextA)(edit,buf,64)==3 && equal(buf,"hio"));
    CHECK(W(SendMessageA)(edit,0xC6,0,0));W(SendMessageA)(edit,0xC7,0,0);
    CHECK(W(GetWindowTextA)(edit,buf,64)==5 && equal(buf,"hello"));
    W(SendMessageA)(edit,0xCF,1,0);W(SendMessageA)(edit,0xC2,1,(LPARAM)"blocked");
    CHECK(W(GetWindowTextA)(edit,buf,64)==5);W(SendMessageA)(edit,0xCF,0,0);
    unsigned short wide_text[32];W(SetWindowTextW)(edit,(const unsigned short *)u"caf\u00e9");
    CHECK(W(GetWindowTextW)(edit,wide_text,32)==4 && wide_text[3]==0xE9);
    W(SetWindowTextA)(edit,"one\r\ntwo");CHECK(W(SendMessageA)(edit,0xBA,0,0)==2);
    CHECK(W(SendMessageA)(edit,0xBB,1,0)==5 && W(SendMessageA)(edit,0xC1,5,0)==3);
    original=(WNDPROC)W(GetWindowLongA)(edit,-4);CHECK(original!=0);
    W(SetWindowLongA)(edit,-4,(LONG)subclass);W(SendMessageW)(edit,0xB1,0,3);
    W(SendMessageW)(edit,0xC2,1,(LPARAM)u"ONE");CHECK(W(GetWindowTextA)(edit,buf,64)==8 && equal(buf,"ONE\r\ntwo"));
    W(SetWindowLongA)(edit,-4,(LONG)original);
    HWND button=child("BUTTON","&Run",0x10001,180,10,90,28,200);
    HWND check=child("BUTTON","Check",3,180,45,90,22,201);W(SendMessageA)(check,0xF5,0,0);CHECK(W(SendMessageA)(check,0xF0,0,0)==1);
    HWND radio1=child("BUTTON","One",9|0x20000,0,0,1,1,210);
    HWND radio2=child("BUTTON","Two",9,0,0,1,1,211);
    W(SendMessageA)(radio1,0xF5,0,0);W(SendMessageA)(radio2,0xF5,0,0);
    CHECK(!W(SendMessageA)(radio1,0xF0,0,0) && W(SendMessageA)(radio2,0xF0,0,0));
    W(DestroyWindow)(radio1);W(DestroyWindow)(radio2);
    child("BUTTON","Group",7,175,80,130,70,202);
    child("STATIC","Label",0,10,85,140,18,203);
    HWND list=child("LISTBOX","",0,10,110,130,65,204);
    CHECK(W(SendMessageA)(list,0x180,0,(LPARAM)"alpha")==0);CHECK(W(SendMessageA)(list,0x180,0,(LPARAM)"beta")==1);
    CHECK(W(SendMessageA)(list,0x18B,0,0)==2);CHECK(W(SendMessageA)(list,0x186,1,0)==1);
    CHECK(W(SendMessageA)(list,0x189,1,(LPARAM)buf)==4 && equal(buf,"beta"));
    CHECK(W(SendMessageA)(list,0x18F,-1,(LPARAM)"be")==1);
    CHECK(W(SendMessageA)(list,0x18E,0,0)==0 && W(SendMessageA)(list,0x197,1,0)==0);
    CHECK(W(SendMessageA)(list,0x18E,0,0)==1);W(SendMessageA)(list,0x197,0,0);
    W(SendMessageA)(list,0x19A,1,0x1234);CHECK(W(SendMessageA)(list,0x199,1,0)==0x1234);
    combo=child("COMBOBOX","",3,180,160,110,30,205);
    CHECK(W(SendMessageA)(combo,0x143,0,(LPARAM)"first")==0);CHECK(W(SendMessageA)(combo,0x143,0,(LPARAM)"second")==1);CHECK(W(SendMessageA)(combo,0x14E,0,0)==0);
    HWND scroll=child("SCROLLBAR","",0,10,185,140,16,206);
    int si[]={28,7,0,100,10,95,0};CHECK(W(SetScrollInfo)(scroll,2,si,1)==91);si[1]=7;CHECK(W(GetScrollInfo)(scroll,2,si) && si[5]==91);
    HWND scrolling=child("EDIT","scrolling",4|0x00300000,280,20,36,100,207);
    RECT sc;W(GetClientRect)(scrolling,&sc);CHECK(sc.right==20 && sc.bottom==84);
    W(ShowScrollBar)(scrolling,1,0);W(GetClientRect)(scrolling,&sc);CHECK(sc.right==36 && sc.bottom==84);
    W(ShowScrollBar)(scrolling,1,1);W(GetClientRect)(scrolling,&sc);CHECK(sc.right==20);
    W(DestroyWindow)(scrolling);
    HDC dc=W(GetDC)(root);CHECK(W(DrawIconEx)(dc,280,180,icon,32,32,0,0,3));CHECK(W(GetPixel)(dc,290,190)==RGB(255,0,0));W(ReleaseDC)(root,dc);
    CHECK(W(OpenClipboard)(root));CHECK(!W(OpenClipboard)(root));
    HANDLE desktop_text=W(GetClipboardData)(1);char *dt=W(GlobalLock)(desktop_text);CHECK(dt && equal(dt,"from desktop"));W(GlobalUnlock)(desktop_text);
    CHECK(W(EmptyClipboard)());
    HANDLE data=W(GlobalAlloc)(0x42,6);char *p=W(GlobalLock)(data);p[0]='c';p[1]='l';p[2]='i';p[3]='p';p[4]='!';p[5]=0;W(GlobalUnlock)(data);
    CHECK(W(SetClipboardData)(1,data)==data);CHECK(W(CloseClipboard)());
    CHECK(W(OpenClipboard)(root));HANDLE wide=W(GetClipboardData)(13);const unsigned short *ws=W(GlobalLock)(wide);CHECK(ws && ws[0]=='c' && ws[4]=='!' && !ws[5]);W(GlobalUnlock)(wide);CHECK(W(CloseClipboard)());
    W(SetWindowTextA)(edit,"");W(SendMessageA)(edit,0x302,0,0);CHECK(W(GetWindowTextA)(edit,buf,64)==5 && equal(buf,"clip!"));
    CHECK(W(DialogBoxParamA)(instance,(const char *)1,root,dialog_proc,1)==17);
    CHECK(W(DialogBoxParamA)(instance,(const char *)3,root,dialog_proc,1)==-1); /* truncated */
    HWND modeless=W(CreateDialogParamA)(instance,(const char *)2,root,dialog_proc,0);CHECK(modeless!=0);
    HWND de=W(GetDlgItem)(modeless,100),db=W(GetDlgItem)(modeless,1);
    CHECK(W(GetNextDlgTabItem)(modeless,de,1)==db);
    MSG dm={de,0x106,'v',0,0,{0,0}};CHECK(W(IsDialogMessageA)(modeless,&dm) && W(GetFocus)()==de);
    dm.message=WM_KEYDOWN;dm.wParam=9;CHECK(W(IsDialogMessageA)(modeless,&dm) && W(GetFocus)()==db);
    W(DestroyWindow)(modeless);
    accelerators=W(LoadAcceleratorsA)(instance,(const char *)1);CHECK(accelerators!=0);
    HWND doomed=child("EDIT","",0,0,0,1,1,300);W(SetWindowTextA)(doomed,"destroy during notification");CHECK(!W(IsWindow)(doomed));
    W(SetWindowTextA)(edit,"");W(SendMessageA)(edit,0xB1,0,0);
    W(ShowWindow)(root,5);W(SetFocus)(edit);W(UpdateWindow)(root);W(UpdateWindow)(edit);W(UpdateWindow)(button);
    W(UpdateWindow)(check);W(UpdateWindow)(list);W(UpdateWindow)(combo);
    status("phase2-ready");
    MSG m;
    while(W(GetMessageA)(&m,0,0,0)>0) {
        if(!W(TranslateAcceleratorA)(root,accelerators,&m)) {W(TranslateMessage)(&m);W(DispatchMessageA)(&m);}
    }
    W(DestroyMenu)(menu);W(ExitProcess)(m.wParam);
}
