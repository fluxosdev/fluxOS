/*
 * DLLs winrun names but does not fill in yet: their tables are empty, so
 * the DLL itself is found (LoadLibrary and GetModuleHandle succeed) but
 * every function imported from it is unresolved: a program that imports
 * one does not start, and GetProcAddress gives NULL.
 */
#include "win32.h"

#define EMPTY_DLL(var, nm) struct api_dll var = { nm, NULL, 0, NULL, { NULL }, { 0 }, NULL, NULL, 0 }


EMPTY_DLL(oleaut32_dll, "OLEAUT32.DLL");
EMPTY_DLL(version_dll, "VERSION.DLL");
EMPTY_DLL(winspool_dll, "WINSPOOL.DRV");
EMPTY_DLL(ws2_32_dll, "WS2_32.DLL");
EMPTY_DLL(imm32_dll, "IMM32.DLL");
EMPTY_DLL(rpcrt4_dll, "RPCRT4.DLL");
EMPTY_DLL(uxtheme_dll, "UXTHEME.DLL");
EMPTY_DLL(dwmapi_dll, "DWMAPI.DLL");
EMPTY_DLL(psapi_dll, "PSAPI.DLL");
EMPTY_DLL(crypt32_dll, "CRYPT32.DLL");
EMPTY_DLL(secur32_dll, "SECUR32.DLL");
EMPTY_DLL(wininet_dll, "WININET.DLL");
EMPTY_DLL(mpr_dll, "MPR.DLL");
EMPTY_DLL(setupapi_dll, "SETUPAPI.DLL");
