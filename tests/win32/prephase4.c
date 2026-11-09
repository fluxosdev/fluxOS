#include "winapi.h"
static int failures;
#define CHECK(c) do {if(!(c)) {const char msg[]="prephase4 failed: " #c "\n";DWORD n;W(WriteFile)(W(GetStdHandle)(-11),msg,sizeof(msg)-1,&n,0);failures++;}}while(0)
#define F(name,ret,args) static ret (WINAPI *name) args
F(SetBkColor,COLORREF,(HDC,COLORREF));
F(SaveDC,int,(HDC));F(RestoreDC,BOOL,(HDC,int));
F(CreateRectRgn,HANDLE,(int,int,int,int));F(CombineRgn,int,(HANDLE,HANDLE,HANDLE,int));
F(SelectClipRgn,int,(HDC,HANDLE));F(GetClipRgn,int,(HDC,HANDLE));F(PtInRegion,BOOL,(HANDLE,int,int));
F(ExcludeClipRect,int,(HDC,int,int,int,int));F(IntersectClipRect,int,(HDC,int,int,int,int));
F(CreatePatternBrush,HBRUSH,(HBITMAP));F(CreateHatchBrush,HBRUSH,(int,COLORREF));
F(SetBrushOrgEx,BOOL,(HDC,int,int,POINT *));F(GetBrushOrgEx,BOOL,(HDC,POINT *));
F(timeSetEvent,UINT,(UINT,UINT,void *,DWORD,UINT));F(timeKillEvent,UINT,(UINT));
static HANDLE (*begin)(void (*)(void *),UINT,void *);
static HANDLE (*beginex)(void *,UINT,DWORD (WINAPI *)(void *),void *,UINT,DWORD *);
static void (*end)(void);static void (*endex)(UINT);
#define LOAD(module,name) do {name=(void *)W(GetProcAddress)(module,#name);CHECK(name);}while(0)
static void gdi(void)
{
    HMODULE m=W(GetModuleHandleA)("gdi32.dll");
    LOAD(m,SetBkColor);LOAD(m,SaveDC);LOAD(m,RestoreDC);LOAD(m,CreateRectRgn);LOAD(m,CombineRgn);
    LOAD(m,SelectClipRgn);LOAD(m,GetClipRgn);LOAD(m,PtInRegion);LOAD(m,ExcludeClipRect);LOAD(m,IntersectClipRect);
    LOAD(m,CreatePatternBrush);LOAD(m,CreateHatchBrush);LOAD(m,SetBrushOrgEx);LOAD(m,GetBrushOrgEx);
    if(failures) return;
    HDC dc=W(CreateCompatibleDC)(0);HBITMAP bm=W(CreateCompatibleBitmap)(0,16,16);
    HGDIOBJ original=W(SelectObject)(dc,bm);HBRUSH red=W(CreateSolidBrush)(RGB(255,0,0));
    W(SelectObject)(dc,red);W(PatBlt)(dc,0,0,16,16,WHITENESS);
    HANDLE outer=CreateRectRgn(0,0,12,12),hole=CreateRectRgn(4,4,8,8),region=CreateRectRgn(0,0,0,0);
    CHECK(CombineRgn(region,outer,hole,4)==3);
    CHECK(PtInRegion(region,2,2) && !PtInRegion(region,5,5));
    CHECK(SelectClipRgn(dc,region)==3);
    CHECK(GetClipRgn(dc,hole)==1);
    W(DeleteObject)(region); /* DC owns a copy. */
    CHECK(SaveDC(dc)==1);
    W(PatBlt)(dc,0,0,16,16,PATCOPY);
    CHECK(SelectClipRgn(dc,0)==2);
    CHECK(W(GetPixel)(dc,2,2)==RGB(255,0,0));
    CHECK(W(GetPixel)(dc,5,5)==RGB(255,255,255));
    CHECK(W(GetPixel)(dc,13,13)==RGB(255,255,255));
    CHECK(SaveDC(dc)==2);W(SetTextColor)(dc,RGB(1,2,3));SetBrushOrgEx(dc,3,4,0);
    CHECK(RestoreDC(dc,1));POINT point;CHECK(GetBrushOrgEx(dc,&point) && !point.x && !point.y);
    CHECK(GetClipRgn(dc,hole)==1 && !PtInRegion(hole,5,5));
    CHECK(!RestoreDC(dc,-1));CHECK(SelectClipRgn(dc,0)==2);
    CHECK(IntersectClipRect(dc,1,1,10,10)==2);CHECK(ExcludeClipRect(dc,3,3,6,6)==3);
    CHECK(SelectClipRgn(dc,0)==2);
    /* Pattern brush copies its source. Source deletion and negative origins work. */
    HDC source=W(CreateCompatibleDC)(0);HBITMAP pattern=W(CreateCompatibleBitmap)(0,2,2);
    HGDIOBJ source_old=W(SelectObject)(source,pattern);
    W(SetPixel)(source,0,0,RGB(0,0,255));W(SetPixel)(source,1,0,RGB(0,255,0));
    W(SetPixel)(source,0,1,RGB(255,0,0));W(SetPixel)(source,1,1,RGB(0,0,0));
    HBRUSH brush=CreatePatternBrush(pattern);CHECK(brush);
    W(SelectObject)(source,source_old);W(DeleteObject)(pattern);W(DeleteDC)(source);
    W(SelectObject)(dc,brush);SetBrushOrgEx(dc,-1,-1,0);W(PatBlt)(dc,0,0,4,4,PATCOPY);
    CHECK(W(GetPixel)(dc,0,0)==RGB(0,0,0));CHECK(W(GetPixel)(dc,1,1)==RGB(0,0,255));
    RECT rect={0,0,4,4};W(FillRect)(dc,&rect,brush);CHECK(W(GetPixel)(dc,1,1)==RGB(0,0,255));
    HBRUSH hatch=CreateHatchBrush(0,RGB(255,0,0));CHECK(hatch);
    W(SelectObject)(dc,hatch);SetBrushOrgEx(dc,0,0,0);SetBkColor(dc,RGB(0,255,0));
    W(SetBkMode)(dc,2);W(PatBlt)(dc,0,0,8,8,PATCOPY);
    CHECK(W(GetPixel)(dc,2,0)==RGB(255,0,0));CHECK(W(GetPixel)(dc,2,1)==RGB(0,255,0));
    W(SetBkMode)(dc,1);W(SetPixel)(dc,2,1,RGB(0,0,255));W(PatBlt)(dc,0,0,8,8,PATCOPY);
    CHECK(W(GetPixel)(dc,2,1)==RGB(0,0,255));
    CHECK(!CreateHatchBrush(6,0));
    W(SelectObject)(dc,original);W(DeleteDC)(dc);W(DeleteObject)(bm);W(DeleteObject)(red);
    W(DeleteObject)(brush);W(DeleteObject)(hatch);W(DeleteObject)(outer);W(DeleteObject)(hole);
}
static HANDLE done;
static void cdecl_worker(void *arg) {CHECK(arg==(void *)123);W(SetEvent)(done);}
static void cdecl_end(void *arg) {(void)arg;W(SetEvent)(done);end();CHECK(0);}
static DWORD WINAPI ex_worker(void *arg) {CHECK(arg==(void *)456);return 37;}
static DWORD WINAPI ex_end(void *arg) {(void)arg;endex(41);return 0;}
static volatile DWORD callback_id,callback_count,self_kill_result,slow_finished;
static void WINAPI slow_callback(UINT id,UINT msg,DWORD user,DWORD a,DWORD b)
{(void)id;(void)msg;(void)user;(void)a;(void)b;W(SetEvent)(done);W(Sleep)(50);slow_finished=1;}
static void WINAPI timer_callback(UINT id,UINT msg,DWORD user,DWORD a,DWORD b)
{
    CHECK(id && !msg && !a && !b);callback_id=W(GetCurrentThreadId)();callback_count++;
    if(user==99) self_kill_result=timeKillEvent(id);
    W(SetEvent)(done);
}
static void threading(void)
{
    HMODULE crt=W(GetModuleHandleA)("msvcrt.dll"),mm=W(GetModuleHandleA)("winmm.dll");
    begin=W(GetProcAddress)(crt,"_beginthread");beginex=W(GetProcAddress)(crt,"_beginthreadex");
    end=W(GetProcAddress)(crt,"_endthread");endex=W(GetProcAddress)(crt,"_endthreadex");
    CHECK(begin && beginex && end && endex);LOAD(mm,timeSetEvent);LOAD(mm,timeKillEvent);if(failures) return;
    DWORD id,code=0;HANDLE h=beginex(0,0,ex_worker,(void *)456,4,&id);CHECK(h && id);
    CHECK(W(WaitForSingleObject)(h,0)==258);CHECK(W(ResumeThread)(h)==1);
    CHECK(W(WaitForSingleObject)(h,5000)==0);CHECK(W(GetExitCodeThread)(h,&code) && code==37);W(CloseHandle)(h);
    h=beginex(0,0,ex_end,0,0,0);CHECK(h);CHECK(W(WaitForSingleObject)(h,5000)==0);
    CHECK(W(GetExitCodeThread)(h,&code) && code==41);W(CloseHandle)(h);
    done=W(CreateEventA)(0,0,0,0);CHECK(done);
    h=begin(cdecl_worker,0,(void *)123);CHECK(h!=(HANDLE)-1);
    CHECK(W(WaitForSingleObject)(done,5000)==0);W(Sleep)(20);CHECK(!W(GetExitCodeThread)(h,&code));
    h=begin(cdecl_end,0,0);CHECK(h!=(HANDLE)-1);
    CHECK(W(WaitForSingleObject)(done,5000)==0);W(Sleep)(20);CHECK(!W(GetExitCodeThread)(h,&code));
    CHECK(begin(0,0,0)==(HANDLE)-1 && !beginex(0,0,0,0,0,0));
    UINT timer=timeSetEvent(10,0,(void *)done,0,0x10);CHECK(timer);
    CHECK(W(WaitForSingleObject)(done,5000)==0);
    timer=timeSetEvent(5,0,timer_callback,0,1|0x100);CHECK(timer);
    CHECK(W(WaitForSingleObject)(done,5000)==0);CHECK(callback_id!=W(GetCurrentThreadId)());
    CHECK(timeKillEvent(timer)==0);DWORD count=callback_count;W(Sleep)(30);CHECK(callback_count==count);
    while(W(WaitForSingleObject)(done,0)==0) {} /* Drain the last periodic notification. */
    timer=timeSetEvent(5,0,timer_callback,99,1|0x100);CHECK(timer);
    CHECK(W(WaitForSingleObject)(done,5000)==0);CHECK(!self_kill_result);
    count=callback_count;W(Sleep)(30);CHECK(callback_count==count);
    while(W(WaitForSingleObject)(done,0)==0) {}
    timer=timeSetEvent(5,0,slow_callback,0,1|0x100);CHECK(timer);
    CHECK(W(WaitForSingleObject)(done,5000)==0);
    CHECK(timeKillEvent(timer)==0 && slow_finished);
    timer=timeSetEvent(100,0,timer_callback,0,1|0x100);CHECK(timer);
    CHECK(timeKillEvent(timer)==0);count=callback_count;W(Sleep)(120);CHECK(callback_count==count);
    CHECK(!timeSetEvent(0,0,timer_callback,0,0));CHECK(timeKillEvent(0)!=0);
    W(CloseHandle)(done);
}
void start(void) {gdi();threading();W(ExitProcess)(failures?1:23);}
