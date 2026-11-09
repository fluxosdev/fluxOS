/* An OpenGL frame through the real desktop protocol, including resize. */
#include "winapi.h"
#define F(name,ret,args) static ret (WINAPI *name) args
F(ChoosePixelFormat,int,(HDC,const void *));F(SetPixelFormat,BOOL,(HDC,int,const void *));F(SwapBuffers,BOOL,(HDC));
F(wglCreateContext,HANDLE,(HDC));F(wglMakeCurrent,BOOL,(HDC,HANDLE));F(wglDeleteContext,BOOL,(HANDLE));
F(glClearColor,void,(float,float,float,float));F(glClear,void,(UINT));
static HANDLE context;static HDC dc;
static void fail(void) {W(ExitProcess)(1);}
#define LOAD(m,name) do {name=(void *)W(GetProcAddress)(m,#name);if(!name)fail();}while(0)
static LRESULT WINAPI proc(HWND window,UINT message,WPARAM wp,LPARAM lp)
{
    if(message==WM_PAINT && context){PAINTSTRUCT p;W(BeginPaint)(window,&p);glClearColor(0,1,0,1);glClear(0x4000);if(!SwapBuffers(dc))fail();W(EndPaint)(window,&p);return 0;}
    if(message==WM_DESTROY){W(PostQuitMessage)(0);return 0;}
    return W(DefWindowProcA)(window,message,wp,lp);
}
void start(void)
{
    HMODULE g=W(GetModuleHandleA)("gdi32.dll"),gl=W(GetModuleHandleA)("opengl32.dll");
    LOAD(g,ChoosePixelFormat);LOAD(g,SetPixelFormat);LOAD(g,SwapBuffers);LOAD(gl,wglCreateContext);LOAD(gl,wglMakeCurrent);LOAD(gl,wglDeleteContext);LOAD(gl,glClearColor);LOAD(gl,glClear);
    WNDCLASSA cls={0};cls.lpfnWndProc=proc;cls.lpszClassName="Phase4GL";cls.style=0x20;
    if(!W(RegisterClassA)(&cls))fail();HWND window=W(CreateWindowExA)(0,"Phase4GL","Phase4GL",WS_POPUP,0,0,32,32,0,0,0,0);if(!window)fail();
    dc=W(GetDC)(window);unsigned char pfd[40]={40,0,1,0};if(!ChoosePixelFormat(dc,pfd)||!SetPixelFormat(dc,1,pfd))fail();
    context=wglCreateContext(dc);if(!context||!wglMakeCurrent(dc,context))fail();W(ShowWindow)(window,1);
    MSG msg;while(W(GetMessageA)(&msg,0,0,0)>0){W(TranslateMessage)(&msg);W(DispatchMessageA)(&msg);}
    wglMakeCurrent(0,0);wglDeleteContext(context);W(ExitProcess)(27);
}
