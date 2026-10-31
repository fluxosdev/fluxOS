/*
 * DLLs winrun names but does not fill in yet: their tables are empty, so
 * every import from them reaches api_missing_thunk (logged, returns 0).
 */
#include "win32.h"

#define EMPTY_DLL(var, nm) struct api_dll var = { nm, NULL, 0, NULL, { NULL }, { 0 }, NULL, NULL, 0 }

EMPTY_DLL(advapi32_dll, "ADVAPI32.DLL");
EMPTY_DLL(shell32_dll, "SHELL32.DLL");
EMPTY_DLL(ole32_dll, "OLE32.DLL");
EMPTY_DLL(oleaut32_dll, "OLEAUT32.DLL");
EMPTY_DLL(comctl32_dll, "COMCTL32.DLL");
EMPTY_DLL(comdlg32_dll, "COMDLG32.DLL");
EMPTY_DLL(version_dll, "VERSION.DLL");
EMPTY_DLL(winspool_dll, "WINSPOOL.DRV");
EMPTY_DLL(ws2_32_dll, "WS2_32.DLL");
EMPTY_DLL(imm32_dll, "IMM32.DLL");
EMPTY_DLL(shlwapi_dll, "SHLWAPI.DLL");
EMPTY_DLL(rpcrt4_dll, "RPCRT4.DLL");
EMPTY_DLL(uxtheme_dll, "UXTHEME.DLL");
EMPTY_DLL(dwmapi_dll, "DWMAPI.DLL");
EMPTY_DLL(psapi_dll, "PSAPI.DLL");
EMPTY_DLL(crypt32_dll, "CRYPT32.DLL");
EMPTY_DLL(secur32_dll, "SECUR32.DLL");
EMPTY_DLL(wininet_dll, "WININET.DLL");
EMPTY_DLL(mpr_dll, "MPR.DLL");
EMPTY_DLL(setupapi_dll, "SETUPAPI.DLL");
