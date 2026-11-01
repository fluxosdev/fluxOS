/* Phase-4 Windows ABI, rendering and actual overlapping PCM regressions. */
#include "winapi.h"
static int failures;
#define CHECK(c) do {if(!(c)){const char text[]="phase4 FAIL: " #c "\n";DWORD n;W(WriteFile)(W(GetStdHandle)(-11),text,sizeof(text)-1,&n,0);failures++;}}while(0)
#define F(name,ret,args) static ret (WINAPI *name) args
#define LOAD(m,name) do {name=(void *)W(GetProcAddress)(m,#name);CHECK(name);}while(0)
#define M(p,n,ret,args) ((ret (WINAPI *)args)(*(void ***)(p))[n])
F(ChoosePixelFormat,int,(HDC,const void *));F(SetPixelFormat,BOOL,(HDC,int,const void *));F(SwapBuffers,BOOL,(HDC));
F(wglCreateContext,HANDLE,(HDC));F(wglMakeCurrent,BOOL,(HDC,HANDLE));F(wglDeleteContext,BOOL,(HANDLE));
F(wglGetCurrentContext,HANDLE,(void));F(wglGetProcAddress,void *,(const char *));
F(glClearColor,void,(float,float,float,float));F(glClear,void,(UINT));F(glBegin,void,(UINT));F(glEnd,void,(void));
F(glColor3f,void,(float,float,float));F(glVertex2f,void,(float,float));F(glGetString,const char *,(UINT));
F(glReadPixels,void,(int,int,int,int,UINT,UINT,void *));F(glOrtho,void,(double,double,double,double,double,double));
F(glGetError,UINT,(void));F(glVertexPointer,void,(int,UINT,int,const void *));F(glGetPointerv,void,(UINT,void **));
static HANDLE main_context;static HDC main_dc;
static DWORD WINAPI gl_thread(void *unused)
{(void)unused;CHECK(!wglGetCurrentContext());CHECK(!wglMakeCurrent(main_dc,main_context));return 0;}
static void opengl(void)
{
    HMODULE gl=W(GetModuleHandleA)("opengl32.dll"),gdi=W(GetModuleHandleA)("gdi32.dll");CHECK(gl);
    LOAD(gdi,ChoosePixelFormat);LOAD(gdi,SetPixelFormat);LOAD(gdi,SwapBuffers);
    LOAD(gl,wglCreateContext);LOAD(gl,wglMakeCurrent);LOAD(gl,wglDeleteContext);LOAD(gl,wglGetCurrentContext);LOAD(gl,wglGetProcAddress);
    LOAD(gl,glClearColor);LOAD(gl,glClear);LOAD(gl,glBegin);LOAD(gl,glEnd);LOAD(gl,glColor3f);LOAD(gl,glVertex2f);
    LOAD(gl,glGetString);LOAD(gl,glReadPixels);LOAD(gl,glOrtho);LOAD(gl,glGetError);LOAD(gl,glVertexPointer);LOAD(gl,glGetPointerv);
    if(failures)return;
    HDC dc=W(CreateCompatibleDC)(0);HBITMAP bitmap=W(CreateCompatibleBitmap)(0,32,32);HGDIOBJ old=W(SelectObject)(dc,bitmap);
    unsigned char pfd[40]={40,0,1,0};CHECK(ChoosePixelFormat(dc,pfd)==1);CHECK(SetPixelFormat(dc,1,pfd));
    HANDLE context=wglCreateContext(dc);CHECK(context);CHECK(wglMakeCurrent(dc,context));
    CHECK(wglGetCurrentContext()==context);CHECK(glGetString(0x1f02));CHECK(wglGetProcAddress("glClear")==W(GetProcAddress)(gl,"glClear"));
    CHECK(!wglGetProcAddress("not_a_gl_function"));
    glClearColor(0,0,1,1);glClear(0x4000|0x100);glOrtho(-1,1,-1,1,-1,1);
    glBegin(4);glColor3f(1,0,0);glVertex2f(-0.8f,-0.8f);glVertex2f(0.8f,-0.8f);glVertex2f(0,0.8f);glEnd();
    unsigned char rgba[4]={0};glReadPixels(16,16,1,1,0x1908,0x1401,rgba);CHECK(rgba[0]>200 && rgba[2]<20);CHECK(!glGetError());
    CHECK(SwapBuffers(dc));CHECK(W(GetPixel)(dc,16,16)==RGB(255,0,0));CHECK(W(GetPixel)(dc,0,0)==RGB(0,0,255));
    float vertices[]={0,0,1,1};void *pointer=0;glVertexPointer(2,0x1406,0,vertices);glGetPointerv(0x808e,&pointer);CHECK(pointer==vertices);
    main_context=context;main_dc=dc;HANDLE thread=W(CreateThread)(0,0,gl_thread,0,0,0);CHECK(thread);CHECK(W(WaitForSingleObject)(thread,5000)==0);W(CloseHandle)(thread);
    CHECK(wglGetCurrentContext()==context);CHECK(wglMakeCurrent(0,0));CHECK(wglDeleteContext(context));CHECK(!wglDeleteContext(context));
    W(SelectObject)(dc,old);W(DeleteObject)(bitmap);W(DeleteDC)(dc);
    /* Bulk GL allocations exceed the native-loader gap below the guest EXE. */
    dc=W(CreateCompatibleDC)(0);bitmap=W(CreateCompatibleBitmap)(0,1024,1024);old=W(SelectObject)(dc,bitmap);
    CHECK(SetPixelFormat(dc,1,pfd));context=wglCreateContext(dc);CHECK(context);CHECK(wglMakeCurrent(dc,context));
    glClearColor(0,1,0,1);glClear(0x4000);CHECK(SwapBuffers(dc));CHECK(W(GetPixel)(dc,1023,1023)==RGB(0,255,0));
    wglMakeCurrent(0,0);wglDeleteContext(context);W(SelectObject)(dc,old);W(DeleteObject)(bitmap);W(DeleteDC)(dc);
}
F(DirectDrawCreate,UINT,(void *,void **,void *));
F(DirectDrawCreateEx,UINT,(void *,void **,const void *,void *));
F(DirectSoundCreate,UINT,(void *,void **,void *));
F(CoInitializeEx,UINT,(void *,DWORD));F(CoUninitialize,void,(void));F(CoCreateInstance,UINT,(const void *,void *,DWORD,const void *,void **));
F(waveOutGetNumDevs,UINT,(void));F(waveOutOpen,UINT,(HANDLE *,UINT,const void *,DWORD,DWORD,DWORD));
F(waveOutClose,UINT,(HANDLE));F(waveOutPrepareHeader,UINT,(HANDLE,void *,UINT));F(waveOutUnprepareHeader,UINT,(HANDLE,void *,UINT));
F(waveOutWrite,UINT,(HANDLE,void *,UINT));F(waveOutReset,UINT,(HANDLE));F(waveOutPause,UINT,(HANDLE));F(waveOutRestart,UINT,(HANDLE));
F(PlaySoundA,BOOL,(const void *,HMODULE,UINT));
F(midiOutPrepareHeader,UINT,(HANDLE,void *,UINT));F(midiOutUnprepareHeader,UINT,(HANDLE,void *,UINT));F(midiOutLongMsg,UINT,(HANDLE,void *,UINT));
F(midiOutGetNumDevs,UINT,(void));F(midiOutOpen,UINT,(HANDLE *,UINT,DWORD,DWORD,DWORD));F(midiOutShortMsg,UINT,(HANDLE,DWORD));F(midiOutClose,UINT,(HANDLE));
static LRESULT WINAPI window_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {return W(DefWindowProcA)(hwnd,message,wp,lp);}
static HWND create_window(void)
{
    WNDCLASSA cls={0};cls.lpfnWndProc=window_proc;cls.lpszClassName="Phase4";CHECK(W(RegisterClassA)(&cls));
    return W(CreateWindowExA)(0,"Phase4","Phase4",WS_POPUP,0,0,32,32,0,0,0,0);
}
static void directdraw(HWND window)
{
    HMODULE module=W(GetModuleHandleA)("ddraw.dll");LOAD(module,DirectDrawCreate);LOAD(module,DirectDrawCreateEx);if(failures)return;
    HMODULE ole=W(GetModuleHandleA)("ole32.dll");LOAD(ole,CoInitializeEx);LOAD(ole,CoUninitialize);LOAD(ole,CoCreateInstance);
    const DWORD iid7[]={0x15e65ec0,0x11d23b9c,0x60002fb9,0x5bea9797};void *dd=0;
    CHECK(!CoInitializeEx(0,0));CHECK(CoInitializeEx(0,0)==1);CHECK(CoInitializeEx(0,2)==0x80010106);CoUninitialize();
    const DWORD clsid[]={0x3c305196,0x11d350db,0xc000fe9c,0xc530d94f};
    CHECK(!CoCreateInstance(clsid,0,1,iid7,&dd) && dd);if(dd)M(dd,2,UINT,(void *))(dd);CoUninitialize();
    CHECK(!DirectDrawCreateEx(0,&dd,iid7,0) && dd);if(!dd)return;
    CHECK(!M(dd,20,UINT,(void *,HWND,DWORD))(dd,window,8));
    CHECK(!M(dd,21,UINT,(void *,DWORD,DWORD,DWORD,DWORD,DWORD))(dd,32,32,8,0,0));
    DWORD desc[31]={124,1|0x20};desc[5]=1;desc[26]=0x200|8|0x10;
    void *front=0,*back=0;CHECK(!M(dd,6,UINT,(void *,void *,void **,void *))(dd,desc,&front,0));if(!front)return;
    DWORD caps[4]={4};CHECK(!M(front,12,UINT,(void *,void *,void **))(front,caps,&back) && back);
    unsigned char colors[1024]={0};colors[4]=255;colors[9]=255;colors[14]=255;void *palette=0;
    CHECK(!M(dd,5,UINT,(void *,DWORD,void *,void **,void *))(dd,4|0x40,colors,&palette,0));
    CHECK(!M(front,31,UINT,(void *,void *))(front,palette));M(palette,2,UINT,(void *))(palette);
    DWORD locked[31]={124};CHECK(!M(back,25,UINT,(void *,RECT *,void *,DWORD,HANDLE))(back,0,locked,0,0));
    CHECK(locked[9] && locked[4]>=32 && locked[21]==8);
    unsigned char *pixels=(void *)locked[9];for(int y=0;y<32;y++)for(int x=0;x<32;x++)pixels[y*locked[4]+x]=1;
    DWORD fx[25]={100};fx[20]=2;CHECK(M(back,5,UINT,(void *,RECT *,void *,RECT *,DWORD,void *))(back,0,0,0,0x400,fx)!=0);
    CHECK(!M(back,32,UINT,(void *,void *))(back,0));CHECK(M(back,32,UINT,(void *,void *))(back,0)!=0);
    CHECK(!M(front,11,UINT,(void *,void *,DWORD))(front,0,0));
    HDC dc=W(GetDC)(window);CHECK(W(GetPixel)(dc,3,3)==RGB(255,0,0));W(ReleaseDC)(window,dc);
    /* Fill, color-keyed blit, copied palette, and surface GDI DC lifetime. */
    CHECK(!M(back,5,UINT,(void *,RECT *,void *,RECT *,DWORD,void *))(back,0,0,0,0x400,fx));
    CHECK(!M(front,5,UINT,(void *,RECT *,void *,RECT *,DWORD,void *))(front,0,back,0,0,0));
    dc=W(GetDC)(window);CHECK(W(GetPixel)(dc,3,3)==RGB(0,255,0));W(ReleaseDC)(window,dc);
    DWORD key[2]={2,2};CHECK(!M(back,29,UINT,(void *,DWORD,void *))(back,8,key));
    CHECK(!M(front,5,UINT,(void *,RECT *,void *,RECT *,DWORD,void *))(front,0,back,0,0x8000,0));
    HDC sdc=0;CHECK(!M(front,17,UINT,(void *,HDC *))(front,&sdc) && sdc);
    W(SetPixel)(sdc,0,0,RGB(0,0,255));CHECK(!M(front,26,UINT,(void *,HDC))(front,sdc));
    dc=W(GetDC)(window);CHECK(W(GetPixel)(dc,0,0)==RGB(0,0,255));W(ReleaseDC)(window,dc);
    CHECK(M(dd,2,UINT,(void *))(dd)>0); /* Surfaces retain the device. */
    M(back,2,UINT,(void *))(back);M(front,2,UINT,(void *))(front);
    /* Windowed primary coordinates are screen coordinates, not scaled to the client. */
    CHECK(W(SetWindowPos)(window,0,10,10,32,32,0));
    dd=0;front=0;CHECK(!DirectDrawCreate(0,&dd,0));if(!dd)return;
    CHECK(!M(dd,20,UINT,(void *,HWND,DWORD))(dd,window,8));
    DWORD normal[27]={108,1};normal[26]=0x200;CHECK(!M(dd,6,UINT,(void *,void *,void **,void *))(dd,normal,&front,0));
    RECT destination={10,10,42,42};fx[20]=0xff00;CHECK(!M(front,5,UINT,(void *,RECT *,void *,RECT *,DWORD,void *))(front,&destination,0,0,0x400,fx));
    dc=W(GetDC)(window);CHECK(W(GetPixel)(dc,0,0)==RGB(0,255,0));CHECK(W(GetPixel)(dc,31,31)==RGB(0,255,0));W(ReleaseDC)(window,dc);
    M(front,2,UINT,(void *))(front);M(dd,2,UINT,(void *))(dd);
}
struct format {unsigned short tag,channels;DWORD rate,average;unsigned short block,bits,extra;} __attribute__((packed));
struct header {void *data;DWORD bytes,recorded,user,flags,loops;void *next;DWORD reserved;};
static short tone440[52920];static volatile int audio_done,return_count;static DWORD audio_thread,return_order[4];
static void WINAPI wave_callback(HANDLE wave,UINT message,DWORD instance,DWORD param,DWORD unused)
{(void)wave;(void)unused;if(message==0x3bd){if(instance==1 && return_count<4)return_order[return_count++]=((struct header *)param)->user;else audio_done++;audio_thread=W(GetCurrentThreadId)();}}
static void audio(HWND window)
{
    HMODULE mm=W(GetModuleHandleA)("winmm.dll"),ds=W(GetModuleHandleA)("dsound.dll");
    LOAD(mm,waveOutGetNumDevs);LOAD(mm,waveOutOpen);LOAD(mm,waveOutClose);LOAD(mm,waveOutPrepareHeader);LOAD(mm,waveOutUnprepareHeader);LOAD(mm,waveOutWrite);
    LOAD(mm,waveOutReset);LOAD(mm,waveOutPause);LOAD(mm,waveOutRestart);LOAD(mm,PlaySoundA);LOAD(ds,DirectSoundCreate);
    LOAD(mm,midiOutPrepareHeader);LOAD(mm,midiOutUnprepareHeader);LOAD(mm,midiOutLongMsg);LOAD(mm,midiOutGetNumDevs);LOAD(mm,midiOutOpen);LOAD(mm,midiOutShortMsg);LOAD(mm,midiOutClose);if(failures)return;
    if(!waveOutGetNumDevs()) {
        const char msg[]="phase4: no PCM device; audio skipped\n";DWORD n;W(WriteFile)(W(GetStdHandle)(-11),msg,sizeof(msg)-1,&n,0);return;
    }
    struct format f={1,1,44100,88200,2,16,0};HANDLE wave=0;
    CHECK(!waveOutOpen(&wave,-1,&f,(DWORD)wave_callback,0,0x30000));CHECK(wave);
    for(DWORD i=0;i<52920;i++)tone440[i]=((i*880/44100)&1)?9000:-9000;
    struct header header={tone440,sizeof(tone440),0,0,0,0,0,0};
    CHECK(!waveOutPrepareHeader(wave,&header,32));CHECK((header.flags&2)!=0);
    CHECK(!waveOutPause(wave));CHECK(!waveOutWrite(wave,&header,32));CHECK(header.flags&0x10);
    CHECK(waveOutUnprepareHeader(wave,&header,32)==33);CHECK(waveOutClose(wave)==33);
    void *sound=0,*buffer=0;CHECK(!DirectSoundCreate(0,&sound,0));if(!sound)return;
    CHECK(!M(sound,6,UINT,(void *,HWND,DWORD))(sound,window,2));
    DWORD desc[9]={36,0x20|0x40|0x80|0x10000,sizeof(tone440),0,(DWORD)&f,0,0,0,0};
    CHECK(!M(sound,3,UINT,(void *,void *,void **,void *))(sound,desc,&buffer,0));if(!buffer)return;
    void *p1=0,*p2=0;DWORD n1=0,n2=0;
    CHECK(!M(buffer,11,UINT,(void *,DWORD,DWORD,void **,DWORD *,void **,DWORD *,DWORD))(buffer,0,sizeof(tone440),&p1,&n1,&p2,&n2,0));
    CHECK(n1==sizeof(tone440) && !n2 && p1);for(DWORD i=0;i<n1/2;i++)((short *)p1)[i]=((i*1320/44100)&1)?9000:-9000;
    CHECK(!M(buffer,19,UINT,(void *,void *,DWORD,void *,DWORD))(buffer,p1,n1,p2,n2));
    CHECK(!M(buffer,12,UINT,(void *,DWORD,DWORD,DWORD))(buffer,0,0,0));CHECK(!waveOutRestart(wave));
    for(int i=0;i<500 && !audio_done;i++)W(Sleep)(10);
    CHECK(audio_done==1 && (header.flags&1) && !(header.flags&0x10));CHECK(audio_thread!=W(GetCurrentThreadId)());
    CHECK(!waveOutUnprepareHeader(wave,&header,32));CHECK(!waveOutClose(wave));
    /* Reset must return headers in submission order, not preparation order. */
    CHECK(!waveOutOpen(&wave,-1,&f,(DWORD)wave_callback,1,0x30000));CHECK(!waveOutPause(wave));
    short tiny[8]={0};struct header first={tiny,sizeof(tiny),0,1,0,0,0,0},second={tiny,sizeof(tiny),0,2,0,0,0,0};
    CHECK(!waveOutPrepareHeader(wave,&first,32));CHECK(!waveOutPrepareHeader(wave,&second,32));
    CHECK(!waveOutWrite(wave,&second,32));CHECK(!waveOutWrite(wave,&first,32));CHECK(!waveOutReset(wave));
    CHECK(return_count==2 && return_order[0]==2 && return_order[1]==1);
    CHECK(!waveOutUnprepareHeader(wave,&first,32));CHECK(!waveOutUnprepareHeader(wave,&second,32));CHECK(!waveOutClose(wave));
    f.rate=192000;f.average=384000;CHECK(!waveOutOpen(&wave,-1,&f,(DWORD)wave_callback,0,0x30000));
    struct header loop={tiny,sizeof(tiny),0,0,12,3,0,0};CHECK(!waveOutPrepareHeader(wave,&loop,32));CHECK(!waveOutWrite(wave,&loop,32));
    for(int i=0;i<100 && audio_done<2;i++)W(Sleep)(10);
    CHECK(audio_done==2);
    CHECK(!waveOutUnprepareHeader(wave,&loop,32));CHECK(!waveOutClose(wave));f.rate=44100;f.average=88200;
    DWORD status=0;for(int i=0;i<100;i++){CHECK(!M(buffer,9,UINT,(void *,DWORD *))(buffer,&status));if(!status)break;W(Sleep)(10);}CHECK(!status);
    CHECK(!M(buffer,11,UINT,(void *,DWORD,DWORD,void **,DWORD *,void **,DWORD *,DWORD))(buffer,sizeof(tone440)-4,8,&p1,&n1,&p2,&n2,0));CHECK(n1==4 && n2==4);
    CHECK(!M(buffer,19,UINT,(void *,void *,DWORD,void *,DWORD))(buffer,p1,n1,p2,n2));
    CHECK(!M(buffer,15,UINT,(void *,int))(buffer,-1000));CHECK(!M(buffer,16,UINT,(void *,int))(buffer,500));CHECK(!M(buffer,17,UINT,(void *,DWORD))(buffer,22050));
    CHECK(!M(buffer,13,UINT,(void *,DWORD))(buffer,0));CHECK(!M(buffer,12,UINT,(void *,DWORD,DWORD,DWORD))(buffer,0,0,1));W(Sleep)(20);CHECK(!M(buffer,18,UINT,(void *))(buffer));
    void *duplicate=0;CHECK(!M(sound,5,UINT,(void *,void *,void **))(sound,buffer,&duplicate));
    CHECK(!M(duplicate,11,UINT,(void *,DWORD,DWORD,void **,DWORD *,void **,DWORD *,DWORD))(duplicate,0,4,&p1,&n1,&p2,&n2,0));
    ((short *)p1)[0]=1234;CHECK(!M(duplicate,19,UINT,(void *,void *,DWORD,void *,DWORD))(duplicate,p1,n1,p2,n2));
    CHECK(!M(buffer,11,UINT,(void *,DWORD,DWORD,void **,DWORD *,void **,DWORD *,DWORD))(buffer,0,4,&p1,&n1,&p2,&n2,0));
    CHECK(((short *)p1)[0]==1234);CHECK(!M(buffer,19,UINT,(void *,void *,DWORD,void *,DWORD))(buffer,p1,n1,p2,n2));
    M(duplicate,2,UINT,(void *))(duplicate);M(buffer,2,UINT,(void *))(buffer);M(sound,2,UINT,(void *))(sound);
    /* A tiny in-memory WAV, both sync and async/stop. */
    unsigned char wav[60]={'R','I','F','F',52,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0x44,0xac,0,0,0x88,0x58,1,0,2,0,16,0,'d','a','t','a',16,0,0,0};
    CHECK(PlaySoundA(wav,0,4));CHECK(PlaySoundA((const void *)1,W(GetModuleHandleA)(0),0x40004));
    HANDLE file=W(CreateFileA)("C:\\tmp\\phase4.wav",0x40000000,0,0,2,0,0);CHECK(file!=(HANDLE)-1);
    DWORD written;CHECK(W(WriteFile)(file,wav,sizeof(wav),&written,0) && written==sizeof(wav));W(CloseHandle)(file);
    CHECK(PlaySoundA("C:\\tmp\\phase4.wav",0,0x20000));CHECK(PlaySoundA(wav,0,4|1|8));CHECK(PlaySoundA(0,0,0));
    if(midiOutGetNumDevs()){HANDLE midi=0;CHECK(!midiOutOpen(&midi,-1,0,0,0));CHECK(!midiOutShortMsg(midi,0x00643c90));W(Sleep)(10);CHECK(!midiOutShortMsg(midi,0x00003c80));unsigned char gm[]={0xf0,0x7e,0x7f,9,1,0xf7};DWORD mh[16]={(DWORD)gm,6};CHECK(!midiOutPrepareHeader(midi,mh,64));CHECK(!midiOutLongMsg(midi,mh,64));CHECK((mh[4]&1)!=0);CHECK(!midiOutUnprepareHeader(midi,mh,64));CHECK(!midiOutClose(midi));const char text[]="phase4: MIDI checks passed\n";DWORD n;W(WriteFile)(W(GetStdHandle)(-11),text,sizeof(text)-1,&n,0);}
}
void start(void) {opengl();HWND window=create_window();CHECK(window);if(window){directdraw(window);audio(window);W(DestroyWindow)(window);}W(ExitProcess)(failures?1:25);}
