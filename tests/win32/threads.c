/* Native Win32 threads, FS/TLS, synchronization, message wakeups and the /winapps drive. */
#include "winapi.h"
static DWORD main_id,tls,cs[6];
static HANDLE event,mutex,semaphore,pipe_read,pipe_write;
static volatile int failures,total;
static volatile HWND worker_windows[2];
#define CHECK(c) do {if(!(c)) {failures++;report(__LINE__);}} while(0)
void *memset(void *p,int c,unsigned int n) {unsigned char *b=p;while(n--) *b++=c;return p;}
static int equal(const char *a,const char *b) {while(*a && *a==*b) {a++;b++;}return *a==*b;}
static void report(int line)
{
    char text[40]="thread test failed: ";int n=20,d=10000;while(d>1 && line<d) d/=10;
    while(d) {text[n++]='0'+line/d;line%=d;d/=10;}text[n++]='\n';DWORD wrote;W(WriteFile)(W(GetStdHandle)((DWORD)-11),text,n,&wrote,0);
}
static LRESULT WINAPI thread_window(HWND h,UINT message,WPARAM wp,LPARAM lp)
{
    if(message==0x8002) {DWORD id=W(GetCurrentThreadId)();CHECK(id==(DWORD)wp);W(PostQuitMessage)(0);return id;}
    return W(DefWindowProcA)(h,message,wp,lp);
}
static DWORD WINAPI worker(void *arg)
{
    DWORD own=W(GetCurrentThreadId)(),fs_id,self;
    __asm__ volatile("movl %%fs:0x24,%0;movl %%fs:0x18,%1":"=r"(fs_id),"=r"(self));
    CHECK(own!=main_id && own==fs_id && self!=0);
    CHECK(W(TlsGetValue)(tls)==0);CHECK(W(TlsSetValue)(tls,arg));
    W(SetLastError)(0x12345678+(DWORD)arg);
    CHECK(W(WaitForSingleObject)(event,5000)==0);
    CHECK(W(GetLastError)()==0x12345678+(DWORD)arg);
    CHECK(W(TlsGetValue)(tls)==arg);
    CHECK(W(WaitForSingleObject)(mutex,5000)==0);CHECK(W(WaitForSingleObject)(mutex,0)==0);
    CHECK(W(ReleaseMutex)(mutex));CHECK(W(ReleaseMutex)(mutex));CHECK(!W(ReleaseMutex)(mutex));
    CHECK(W(WaitForSingleObject)(semaphore,5000)==0);
    for(int i=0;i<100;i++) {
        W(EnterCriticalSection)(cs);CHECK(W(TryEnterCriticalSection)(cs));
        int old=total;W(Sleep)(0);total=old+1;
        W(LeaveCriticalSection)(cs);W(LeaveCriticalSection)(cs);
    }
    DWORD written;CHECK(W(WriteFile)(pipe_write,"p",1,&written,0) && written==1);
    HWND window=W(CreateWindowExA)(0,"ThreadWindow","",WS_POPUP,0,0,16,16,0,0,0,0);
    CHECK(window!=0);worker_windows[(DWORD)arg-1]=window;
    CHECK(W(PostThreadMessageA)(main_id,0x8001,(WPARAM)arg,own));
    MSG message;while(W(GetMessageA)(&message,0,0,0)>0) W(DispatchMessageA)(&message);
    if((DWORD)arg==1) W(ExitThread)(31);
    return (DWORD)arg==2 ? 259:32;
}
static void files(void)
{
    char path[260];CHECK(W(GetCurrentDirectoryA)(260,path)==3 && equal(path,"C:\\"));
    CHECK(W(GetEnvironmentVariableA)("USERPROFILE",path,260)==13 && equal(path,"C:\\users\\flux"));
    CHECK(W(GetLogicalDrives)()&(1u<<25));
    CHECK(W(GetFullPathNameA)("C:\\..\\..\\prefix-test.txt",260,path,0)>0 && equal(path,"C:\\prefix-test.txt"));
    HANDLE f=W(CreateFileA)("C:\\prefix-test.txt",0x40000000,0,0,2,0,0);CHECK(f!=(HANDLE)-1);
    DWORD n=0;CHECK(W(WriteFile)(f,"prefix",6,&n,0) && n==6);CHECK(W(CloseHandle)(f));
    f=W(CreateFileA)("Z:\\winapps\\prefix-test.txt",0x80000000,1,0,3,0,0);CHECK(f!=(HANDLE)-1);
    char text[8]={0};CHECK(W(ReadFile)(f,text,6,&n,0) && n==6 && equal(text,"prefix"));W(CloseHandle)(f);
    CHECK(W(CreateDirectoryA)("C:\\thread-test",0));CHECK(W(SetCurrentDirectoryA)("C:\\thread-test"));
    CHECK(W(GetFullPathNameA)("C:..\\prefix-test.txt",260,path,0)>0 && equal(path,"C:\\prefix-test.txt"));
    CHECK(W(SetCurrentDirectoryA)("C:\\"));
}
void start(void)
{
    files();
    WNDCLASSA wc={0};wc.lpszClassName="ThreadWindow";wc.lpfnWndProc=thread_window;CHECK(W(RegisterClassA)(&wc)!=0);
    HANDLE reg;DWORD disposition,type,size;char value[32];
    CHECK(W(RegCreateKeyExA)((HANDLE)0x80000001,"Software\\fluxOS\\ThreadTest",0,0,0,0,0,&reg,&disposition)==0 && disposition==1);
    CHECK(W(RegSetValueExA)(reg,"Greeting",0,1,"persisted",10)==0);
    size=2;CHECK(W(RegQueryValueExA)(reg,"greeting",0,&type,value,&size)==234 && size==10 && type==1);
    size=32;CHECK(W(RegQueryValueExA)(reg,"GREETING",0,&type,value,&size)==0 && equal(value,"persisted"));
    unsigned short unicode[16];size=sizeof(unicode);CHECK(W(RegQueryValueExW)(reg,(const unsigned short *)u"Greeting",0,&type,unicode,&size)==0 && size==20 && unicode[8]=='d');
    CHECK(W(RegCloseKey)(reg)==0);
    CHECK(W(SHGetFolderPathA)(0,0x801A,0,0,value)==0 && equal(value,"C:\\users\\flux\\AppData\\Roaming"));
    main_id=W(GetCurrentThreadId)();tls=W(TlsAlloc)();CHECK(tls<64);
    CHECK(W(TlsSetValue)(tls,(void *)0xABC));W(InitializeCriticalSection)(cs);
    event=W(CreateEventA)(0,1,0,0);mutex=W(CreateMutexA)(0,0,0);semaphore=W(CreateSemaphoreA)(0,0,2,0);
    CHECK(event && mutex && semaphore);
    CHECK(W(CreatePipe)(&pipe_read,&pipe_write,0,0));MSG probe;W(PeekMessageA)(&probe,0,0,0,0);
    HANDLE threads[2];DWORD ids[2],code;
    threads[0]=W(CreateThread)(0,0,worker,(void *)1,4,&ids[0]);CHECK(threads[0]!=0);
    threads[1]=W(CreateThread)(0,0,worker,(void *)2,0,&ids[1]);CHECK(threads[1]!=0 && ids[0]!=ids[1]);
    CHECK(W(GetExitCodeThread)(threads[0],&code) && code==259);
    CHECK(W(WaitForSingleObject)(threads[0],0)==258);
    CHECK(W(ResumeThread)(threads[0])==1);
    CHECK(W(SetEvent)(event));CHECK(W(ReleaseSemaphore)(semaphore,2,0));
    char pipe_data[2];DWORD received=0,part;
    while(received<2) {CHECK(W(ReadFile)(pipe_read,pipe_data+received,2-received,&part,0));received+=part;}
    CHECK(pipe_data[0]=='p' && pipe_data[1]=='p');W(CloseHandle)(pipe_read);W(CloseHandle)(pipe_write);
    /* The worker posts wake a blocked GetMessage even without a desktop socket. */
    MSG msg;int messages=0;
    while(messages<2) {CHECK(W(GetMessageA)(&msg,0,0,0));CHECK(msg.message==0x8001 && msg.wParam>=1 && msg.wParam<=2);CHECK(W(SendMessageA)(worker_windows[msg.wParam-1],0x8002,msg.lParam,0)==msg.lParam);messages++;}
    CHECK(W(WaitForMultipleObjects)(2,threads,1,5000)==0);
    CHECK(!W(IsWindow)(worker_windows[0]) && !W(IsWindow)(worker_windows[1]));
    CHECK(total==200 && W(TlsGetValue)(tls)==(void *)0xABC);
    CHECK(W(GetExitCodeThread)(threads[0],&code) && code==31);
    CHECK(W(GetExitCodeThread)(threads[1],&code) && code==259); /* 259 is also a valid exit status */
    CHECK(W(WaitForSingleObject)(threads[1],0)==0);
    W(CloseHandle)(threads[0]);W(CloseHandle)(threads[1]);
    CHECK(W(ResetEvent)(event));CHECK(W(WaitForSingleObject)(event,10)==258);
    HANDLE both[2]={event,semaphore};CHECK(W(ReleaseSemaphore)(semaphore,1,0));
    CHECK(W(WaitForMultipleObjects)(2,both,1,0)==258); /* unsuccessful wait-all must not consume semaphore */
    CHECK(W(WaitForSingleObject)(semaphore,0)==0);
    W(DeleteCriticalSection)(cs);W(CloseHandle)(event);W(CloseHandle)(mutex);W(CloseHandle)(semaphore);
    W(ExitProcess)(failures ? 1:15);
}
