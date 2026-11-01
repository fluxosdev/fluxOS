#include "winapi.h"
void start(void)
{
    HANDLE h;DWORD type,size=32;char data[32];int ok=1;
    if(W(RegOpenKeyExA)((HANDLE)0x80000001,"Software\\fluxOS\\ThreadTest",0,0,&h)) ok=0;
    else {if(W(RegQueryValueExA)(h,"Greeting",0,&type,data,&size) || type!=1 || size!=10 || data[0]!='p' || data[8]!='d' || data[9]) ok=0;char name[32];DWORD length=32;size=32;
        if(W(RegEnumValueA)(h,0,name,&length,0,&type,data,&size) || length!=8 || name[0]!='g' || type!=1 || size!=10 || data[0]!='p') ok=0;
        length=32;if(W(RegEnumValueA)(h,1,name,&length,0,&type,data,&size)!=259) ok=0;
        W(RegCloseKey)(h);}
    W(ExitProcess)(ok ? 17:1);
}
