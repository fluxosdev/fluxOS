/* Just enough of <windows.h> for the test programs, which are built without a Windows SDK. */
#ifndef TEST_WINAPI_H
#define TEST_WINAPI_H

#define WINAPI __attribute__((stdcall))
typedef unsigned int UINT, DWORD, WPARAM, COLORREF, BOOL;
typedef int LONG, LRESULT, LPARAM;
typedef unsigned short ATOM;
typedef void *HANDLE, *HWND, *HDC, *HBRUSH, *HINSTANCE, *HMODULE, *HMENU, *HICON, *HCURSOR, *HGDIOBJ, *HBITMAP, *HPEN;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG cx, cy; } SIZE;
typedef struct { LONG x, y; } POINT;
typedef LRESULT (WINAPI *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void (WINAPI *TIMERPROC)(HWND, UINT, UINT, DWORD);
typedef struct {
    DWORD biSize; LONG biWidth, biHeight; unsigned short biPlanes, biBitCount;
    DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
typedef struct { unsigned char rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { LONG bmType, bmWidth, bmHeight, bmWidthBytes; unsigned short bmPlanes, bmBitsPixel; void *bmBits; } BITMAP;
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
#define WM_SETFOCUS 0x0007
#define WM_KEYDOWN 0x0100
#define WM_CHAR 0x0102
#define WM_TIMER 0x0113
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020A
#define WS_POPUP 0x80000000
#define CS_DBLCLKS 0x0008
#define VK_SHIFT 0x10
#define VK_LEFT 0x25
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define IDC_CROSS ((const char *)32515)
#define WHITE_BRUSH 0
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define NULL_PEN 8
#define PS_SOLID 0
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define SRCCOPY 0x00CC0020
#define SRCPAINT 0x00EE0086
#define SRCAND 0x008800C6
#define PATCOPY 0x00F00021
#define WHITENESS 0x00FF0062
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
