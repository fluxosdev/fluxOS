/* Just enough of <windows.h> for the test programs, which are built without a Windows SDK. */
#ifndef TEST_WINAPI_H
#define TEST_WINAPI_H

#define WINAPI __attribute__((stdcall))
typedef unsigned int UINT, DWORD, WPARAM, COLORREF, BOOL;
typedef int LONG, LRESULT, LPARAM;
typedef unsigned short ATOM;
typedef void *HANDLE, *HWND, *HDC, *HBRUSH, *HINSTANCE, *HMODULE, *HMENU, *HICON, *HCURSOR;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG cx, cy; } SIZE;
typedef struct { LONG x, y; } POINT;
typedef LRESULT (WINAPI *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct {
    UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance;
    HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; const char *lpszMenuName, *lpszClassName;
} WNDCLASSA;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef struct { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; unsigned char reserved[32]; } PAINTSTRUCT;

#define RGB(r, g, b) ((COLORREF)((r) | ((g) << 8) | ((b) << 16)))
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_SIZE 0x0005
#define WM_PAINT 0x000F
#define WS_OVERLAPPEDWINDOW 0x00CF0000
#define WS_VISIBLE 0x10000000
#define CW_USEDEFAULT ((int)0x80000000)
#define SW_SHOWDEFAULT 10
#define TRANSPARENT 1
#define DT_CENTER 0x1
#define DT_VCENTER 0x4
#define DT_SINGLELINE 0x20
#define GWL_USERDATA (-21)
#define IDC_ARROW ((const char *)32512)

#define IMP(dll, ret, name, args) extern ret (WINAPI *const __imp_##name) args;
#include "imports.h"
#undef IMP
/* W(Fn)(...) calls Fn through the import table */
#define W(name) (*__imp_##name)

#endif
