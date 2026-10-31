/*
 * Stage 0 of the Windows GUI roadmap: register a class, create a window,
 * pump messages, paint text on a brush, quit on close. win32-gui-guest.c
 * plays the desktop to it and looks at the pixels. A few checks of what
 * the program can ask about its window decide its exit code: 7 when all
 * of them hold.
 */
#include "winapi.h"

static HBRUSH blue;
static int ok = 1, painted;

void *memset(void *p, int c, unsigned int n)
{
    unsigned char *b = p;
    while (n--)
        *b++ = (unsigned char)c;
    return p;
}

static LRESULT WINAPI wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    PAINTSTRUCT ps;
    RECT r;
    switch (msg) {
    case WM_PAINT: {
        HDC dc = W(BeginPaint)(h, &ps);
        W(GetClientRect)(h, &r);
        W(FillRect)(dc, &r, blue);
        W(SetTextColor)(dc, RGB(255, 255, 255));
        W(SetBkMode)(dc, TRANSPARENT);
        W(DrawTextA)(dc, "Hello, fluxOS", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SIZE sz;
        if (!W(GetTextExtentPoint32A)(dc, "Hello", 5, &sz) || sz.cx <= 0 || sz.cy <= 0)
            ok = 0;
        W(EndPaint)(h, &ps);
        if (!painted++) {
            char text[32];
            W(SetWindowTextA)(h, "Painted");
            if (W(GetWindowTextA)(h, text, sizeof(text)) != 7 || text[0] != 'P' || text[6] != 'd' || text[7])
                ok = 0;
        }
        return 0;
    }
    case WM_DESTROY:
        W(PostQuitMessage)(ok && W(GetWindowLongA)(h, GWL_USERDATA) == 0x1234 ? 7 : 1);
        return 0;
    }
    return W(DefWindowProcA)(h, msg, wp, lp);
}

static int win_main(HINSTANCE inst)
{
    WNDCLASSA wc = { 0 };
    blue = W(CreateSolidBrush)(RGB(0x20, 0x40, 0x80));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = W(LoadCursorA)(0, IDC_ARROW);
    wc.lpszClassName = "HelloClass";
    if (!W(RegisterClassA)(&wc))
        return 2;
    RECT r = { 0, 0, 320, 200 };
    W(AdjustWindowRect)(&r, WS_OVERLAPPEDWINDOW, 0);
    HWND h = W(CreateWindowExA)(0, "HelloClass", "Hello", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0,
                                r.right - r.left, r.bottom - r.top, 0, 0, inst, 0);
    if (!h)
        return 3;
    W(GetClientRect)(h, &r);
    if (r.right != 320 || r.bottom != 200)
        ok = 0;
    W(SetWindowLongA)(h, GWL_USERDATA, 0x1234);
    W(ShowWindow)(h, SW_SHOWDEFAULT);
    W(UpdateWindow)(h);
    MSG m;
    while (W(GetMessageA)(&m, 0, 0, 0) > 0) {
        W(TranslateMessage)(&m);
        W(DispatchMessageA)(&m);
    }
    return (int)m.wParam;
}

void start(void)
{
    W(ExitProcess)((UINT)win_main(W(GetModuleHandleA)(0)));
}
