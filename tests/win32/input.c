/*
 * Stage 1 of the Windows GUI roadmap, as a small game would use it: a
 * borderless window the size of the screen, a frame timer and a timer
 * procedure, a scene drawn into a back buffer (bitmaps, DIBs, masks,
 * shapes, text) and blitted to the window, and the keys, mouse and focus
 * win32-gui-guest.c sends. After the last of its input (the middle button
 * let go) the program titles its window "ok", or "FAIL" and the lines of
 * the checks that did not hold.
 */
#include "winapi.h"

static char title[64] = "FAIL";
static int nfail;
static int width, height;
static int ticks, proc_ticks, got_focus, ldown, dblclk, rdown, rup, mdown, wheel, shift_ok = -1, cursor_ok;
static unsigned keys[8], nkeys;
static char chars[8];
static unsigned nchars;
static DWORD started;

void *memset(void *p, int c, unsigned int n)
{
    unsigned char *b = p;
    while (n--)
        *b++ = (unsigned char)c;
    return p;
}

static void fail(int line)
{
    int n = 4;
    while (title[n])
        n++;
    if (n > 56)
        return;
    title[n++] = ' ';
    char digits[8];
    int k = 0;
    do {
        digits[k++] = (char)('0' + line % 10);
        line /= 10;
    } while (line);
    while (k)
        title[n++] = digits[--k];
    title[n] = 0;
    nfail++;
}

#define CHECK(c) do { if (!(c)) fail(__LINE__); } while (0)

static void WINAPI timer_proc(HWND h, UINT msg, UINT id, DWORD time)
{
    (void)h; (void)id; (void)time;
    if (msg == WM_TIMER)
        proc_ticks++;
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = W(BeginPaint)(h, &ps);
    HDC back = W(CreateCompatibleDC)(wdc);
    HBITMAP bb = W(CreateCompatibleBitmap)(wdc, width, height);
    HGDIOBJ old_bb = W(SelectObject)(back, bb);
    HGDIOBJ old_pen = W(SelectObject)(back, W(GetStockObject)(NULL_PEN));
    HGDIOBJ old_brush = W(SelectObject)(back, W(GetStockObject)(NULL_BRUSH));
    RECT r = { 0, 0, width, height };
    W(FillRect)(back, &r, W(GetStockObject)(BLACK_BRUSH));

    /* an 8-bit bottom-up DIB section, red above blue, stretched four times */
    struct { BITMAPINFOHEADER h; RGBQUAD pal[2]; } bi = {
        { sizeof(BITMAPINFOHEADER), 16, 16, 1, 8, BI_RGB, 0, 0, 0, 2, 0 },
        { { 0, 0, 255, 0 }, { 255, 0, 0, 0 } },
    };
    unsigned char *bits = 0;
    HBITMAP dib = W(CreateDIBSection)(wdc, &bi, DIB_RGB_COLORS, (void **)&bits, 0, 0);
    CHECK(dib && bits);
    for (int y = 0; y < 16; y++)        /* the first rows in memory are the bottom ones */
        for (int x = 0; x < 16; x++)
            bits[y * 16 + x] = y < 8 ? 1 : 0;
    BITMAP bm;
    CHECK(W(GetObjectA)(dib, sizeof(bm), &bm) == sizeof(bm) && bm.bmBitsPixel == 8 && bm.bmBits == bits && bm.bmWidth == 16);
    HDC sdc = W(CreateCompatibleDC)(wdc);
    HGDIOBJ old_s = W(SelectObject)(sdc, dib);
    W(StretchBlt)(back, 0, 0, 64, 64, sdc, 0, 0, 16, 16, SRCCOPY);

    /* a 2 x 2 bottom-up 32-bit DIB from the program's own memory */
    static const unsigned quad[4] = { 0x00FF00, 0x0000FF, 0xFFFFFF, 0xFFFF00 };
    BITMAPINFOHEADER b2 = { sizeof(BITMAPINFOHEADER), 2, 2, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
    W(StretchDIBits)(back, 100, 0, 40, 40, 0, 0, 2, 2, quad, &b2, DIB_RGB_COLORS, SRCCOPY);

    /* shapes */
    HPEN white_pen = W(CreatePen)(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN red_pen = W(CreatePen)(PS_SOLID, 1, RGB(255, 0, 0));
    HBRUSH gray = W(CreateSolidBrush)(RGB(128, 128, 128));
    W(SelectObject)(back, white_pen);
    W(SelectObject)(back, gray);
    W(Rectangle)(back, 200, 0, 260, 40);
    W(SelectObject)(back, W(GetStockObject)(NULL_PEN));
    W(SelectObject)(back, W(GetStockObject)(WHITE_BRUSH));
    W(Ellipse)(back, 300, 0, 360, 60);
    POINT tri[3] = { { 700, 0 }, { 760, 0 }, { 700, 60 } };
    W(Polygon)(back, tri, 3);
    W(SelectObject)(back, red_pen);
    W(MoveToEx)(back, 400, 10, 0);
    W(LineTo)(back, 460, 10);

    /* a sprite through a monochrome mask: AND the mask, OR the sprite */
    HBRUSH blue = W(CreateSolidBrush)(RGB(0, 0, 255)), yellow = W(CreateSolidBrush)(RGB(255, 255, 0));
    RECT mr = { 500, 0, 516, 16 };
    W(FillRect)(back, &mr, blue);
    static const unsigned char mask_bits[16] = { 0x0F, 0, 0x0F, 0, 0x0F, 0, 0x0F, 0, 0x0F, 0, 0x0F, 0, 0x0F, 0, 0x0F, 0 };
    HBITMAP mask = W(CreateBitmap)(8, 8, 1, 1, mask_bits);
    HDC mdc = W(CreateCompatibleDC)(wdc);
    HGDIOBJ old_m = W(SelectObject)(mdc, mask);
    HBITMAP sprite = W(CreateCompatibleBitmap)(wdc, 8, 8);
    HDC pdc = W(CreateCompatibleDC)(wdc);
    HGDIOBJ old_p = W(SelectObject)(pdc, sprite);
    RECT left = { 0, 0, 4, 8 }, right = { 4, 0, 8, 8 };
    W(FillRect)(pdc, &left, yellow);
    W(FillRect)(pdc, &right, W(GetStockObject)(BLACK_BRUSH));
    W(BitBlt)(back, 500, 0, 8, 8, mdc, 0, 0, SRCAND);
    W(BitBlt)(back, 500, 0, 8, 8, pdc, 0, 0, SRCPAINT);

    W(SetPixel)(back, 600, 5, RGB(255, 0, 255));
    CHECK(W(GetPixel)(back, 600, 5) == RGB(255, 0, 255));
    W(SetTextColor)(back, RGB(255, 255, 255));
    W(SetBkMode)(back, TRANSPARENT);
    W(TextOutA)(back, 800, 0, "Hi", 2);
    W(PatBlt)(back, 900, 0, 20, 20, WHITENESS);
    W(SelectObject)(back, gray);
    W(PatBlt)(back, 930, 0, 20, 20, PATCOPY);

    W(BitBlt)(wdc, 0, 0, width, height, back, 0, 0, SRCCOPY);

    W(SelectObject)(sdc, old_s);
    W(SelectObject)(mdc, old_m);
    W(SelectObject)(pdc, old_p);
    W(DeleteDC)(sdc);
    W(DeleteDC)(mdc);
    W(DeleteDC)(pdc);
    W(DeleteObject)(dib);
    W(DeleteObject)(mask);
    W(DeleteObject)(sprite);
    W(SelectObject)(back, old_pen);
    W(SelectObject)(back, old_brush);
    W(SelectObject)(back, old_bb);
    W(DeleteObject)(white_pen);
    W(DeleteObject)(red_pen);
    W(DeleteObject)(gray);
    W(DeleteObject)(blue);
    W(DeleteObject)(yellow);
    W(DeleteObject)(bb);
    W(DeleteDC)(back);
    W(EndPaint)(h, &ps);
}

/* After the last input: everything the program heard, checked. */
static void verdict(HWND h)
{
    CHECK(ticks >= 3);
    CHECK(proc_ticks >= 2);
    CHECK(W(timeGetTime)() - started >= 200);
    CHECK(got_focus);
    CHECK(W(GetFocus)() == h);
    CHECK(W(GetActiveWindow)() == h);
    CHECK(nkeys == 4 && keys[0] == 'A' && keys[1] == VK_SHIFT && keys[2] == 'B' && keys[3] == VK_LEFT);
    CHECK(nchars == 2 && chars[0] == 'a' && chars[1] == 'B');
    CHECK(shift_ok == 1);
    CHECK(ldown == 1 && dblclk == 1);
    CHECK(rdown == 1 && rup == 1);
    CHECK(wheel == 120);
    CHECK(mdown == 1);
    CHECK(cursor_ok);
    W(SetWindowTextA)(h, nfail ? title : "ok");
}

static LRESULT WINAPI wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        started = W(timeGetTime)();
        CHECK(W(SetTimer)(h, 1, 20, 0) != 0);
        CHECK(W(SetTimer)(0, 0, 25, timer_proc) != 0);
        return 0;
    case WM_TIMER:
        if (wp == 1)
            ticks++;
        return 0;
    case WM_SETFOCUS:
        got_focus = 1;
        return 0;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_KEYDOWN:
        if (nkeys < 8)
            keys[nkeys++] = wp;
        if (wp == 'B')
            shift_ok = W(GetKeyState)(VK_SHIFT) < 0;
        return 0;
    case WM_CHAR:
        if (nchars < 8)
            chars[nchars++] = (char)wp;
        return 0;
    case WM_LBUTTONDOWN:
        ldown++;
        CHECK(lp == (100 | 100 << 16));
        return 0;
    case WM_LBUTTONDBLCLK:
        dblclk++;
        return 0;
    case WM_RBUTTONDOWN:
        rdown++;
        return 0;
    case WM_RBUTTONUP: {
        POINT p;
        rup++;
        W(GetCursorPos)(&p);
        CHECK(p.x == 200 && p.y == 150);
        W(ScreenToClient)(h, &p);
        CHECK(p.x == 200 && p.y == 150 && lp == (200 | 150 << 16));
        return 0;
    }
    case WM_MOUSEWHEEL:
        wheel = (short)(wp >> 16);
        return 0;
    case WM_MBUTTONDOWN:
        mdown++;
        return 0;
    case WM_MBUTTONUP:
        verdict(h);
        return 0;
    case WM_DESTROY:
        W(PostQuitMessage)(9);
        return 0;
    }
    if (msg == 0x0020 && (lp >> 16) == 0x0200)  /* WM_SETCURSOR for a move: the class cursor follows */
        cursor_ok = 1;
    return W(DefWindowProcA)(h, msg, wp, lp);
}

static int win_main(HINSTANCE inst)
{
    WNDCLASSA wc = { 0 };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = W(LoadCursorA)(0, IDC_CROSS);
    wc.lpszClassName = "Game";
    if (!W(RegisterClassA)(&wc))
        return 2;
    width = W(GetSystemMetrics)(SM_CXSCREEN);
    height = W(GetSystemMetrics)(SM_CYSCREEN);
    HWND h = W(CreateWindowExA)(0, "Game", "Game", WS_POPUP | WS_VISIBLE, 0, 0, width, height, 0, 0, inst, 0);
    if (!h)
        return 3;
    RECT r;
    W(GetClientRect)(h, &r);
    CHECK(r.right == width && r.bottom == height);
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
