/* Stage 2's window tree: API checks, clipped pixels and desktop input to descendants. */
#include "winapi.h"

static HWND root, a, b, nested;
static HBRUSH red, blue, green, black;
static int ok = 1, clicks, enumerated;
#define CHECK(c) do { if (!(c)) ok = 0; } while (0)

void *memset(void *p, int c, unsigned int n)
{
    unsigned char *b = p;
    while (n--) *b++ = (unsigned char)c;
    return p;
}

static void paint(HWND h)
{
    HDC dc = W(GetDC)(h);
    RECT r;
    W(GetClientRect)(h, &r);
    W(FillRect)(dc, &r, h == a ? red : h == b ? blue : h == nested ? green : black);
    W(ReleaseDC)(h, dc);
}

static BOOL WINAPI enumerate(HWND h, LPARAM lp)
{
    CHECK(lp == 0x1234);
    CHECK(W(IsChild)(root, h));
    enumerated++;
    return 1;
}

static HWND victim;
static BOOL WINAPI mutate(HWND h, LPARAM lp)
{
    (void)h; (void)lp;
    /* A destroyed handle in the snapshot must not be called, even after reuse. */
    if (victim) {
        HWND old = victim;
        victim = 0;
        W(DestroyWindow)(old);
        W(CreateWindowExA)(0, "Tree", "", WS_CHILD, 0, 0, 1, 1, root, (HMENU)99, 0, 0);
    }
    enumerated++;
    return 1;
}

static LRESULT WINAPI proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        W(BeginPaint)(h, &ps);
        RECT r;
        W(GetClientRect)(h, &r);
        W(FillRect)(ps.hdc, &r, h == a ? red : h == b ? blue : h == nested ? green : black);
        W(EndPaint)(h, &ps);
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        int x = (short)lp, y = (short)(lp >> 16);
        if (clicks == 0) {
            CHECK(h == b && x == 10 && y == 10);
            W(SetCapture)(nested);
        } else if (clicks == 1) {
            CHECK(h == nested && x == 200 && y == 90);
            W(ReleaseCapture)();
            W(EnableWindow)(b, 0);
        } else if (clicks == 2) {
            CHECK(h == a && x == 50 && y == 20);
            W(EnableWindow)(b, 1);
            W(SetWindowPos)(b, (HWND)1, 0, 0, 0, 0, 3); /* HWND_BOTTOM */
        } else if (clicks == 3) {
            CHECK(h == a && x == 50 && y == 20);
            W(SetWindowTextA)(root, ok ? "children-ok" : "children-failed");
        } else CHECK(0);
        if (clicks < 3) W(SetWindowTextA)(root, ok ? (clicks == 0 ? "step-1" : clicks == 1 ? "step-2" : "step-3") : "children-failed");
        clicks++;
        return 0;
    }
    if (m == WM_DESTROY && h == root) {
        W(PostQuitMessage)(ok && clicks == 4 ? 11 : 1);
        return 0;
    }
    return W(DefWindowProcA)(h, m, wp, lp);
}

void start(void)
{
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = proc;
    wc.lpszClassName = "Tree";
    W(RegisterClassA)(&wc);
    red = W(CreateSolidBrush)(RGB(255,0,0));
    blue = W(CreateSolidBrush)(RGB(0,0,255));
    green = W(CreateSolidBrush)(RGB(0,255,0));
    black = W(CreateSolidBrush)(RGB(0,0,0));
    root = W(CreateWindowExA)(0, "Tree", "Children", WS_POPUP | WS_CLIPCHILDREN, 0,0,240,160,0,0,0,0);
    a = W(CreateWindowExA)(0,"Tree","",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
                           10,10,90,80,root,(HMENU)101,0,0);
    b = W(CreateWindowExA)(0,"Tree","",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
                           50,20,90,80,root,(HMENU)102,0,0);
    nested = W(CreateWindowExA)(0,"Tree","",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
                                -10,40,40,60,a,(HMENU)103,0,0);
    CHECK(root && a && b && nested);
    CHECK(W(GetParent)(nested) == a);
    CHECK(W(GetDlgItem)(root,101) == a && !W(GetDlgItem)(root,103));
    CHECK(W(GetDlgCtrlID)(nested) == 103 && W(IsChild)(root,nested) && !W(IsChild)(root,root));
    CHECK(W(GetWindow)(root,5) == b && W(GetWindow)(b,2) == a);
    CHECK(W(ChildWindowFromPointEx)(root, (POINT){60,30}, 3) == b);
    W(EnumChildWindows)(root,enumerate,0x1234);
    CHECK(enumerated == 3);
    victim = W(CreateWindowExA)(0,"Tree","",WS_CHILD,0,0,1,1,root,(HMENU)104,0,0);
    HWND trigger = W(CreateWindowExA)(0,"Tree","",WS_CHILD,0,0,1,1,root,(HMENU)105,0,0);
    enumerated = 0;
    W(EnumChildWindows)(root,mutate,0);
    CHECK(enumerated == 4); /* trigger, b, a, nested; the victim and replacement are skipped */
    W(DestroyWindow)(trigger);
    W(DestroyWindow)(W(GetDlgItem)(root,99));

    /* Parent changes must update both sibling lists and reject cycles. */
    CHECK((HWND)W(SetWindowLongA)(nested,-8,(LONG)b) == a);
    CHECK(W(GetParent)(nested) == b && !W(GetDlgItem)(a,103) && W(GetDlgItem)(b,103) == nested);
    CHECK(!W(SetWindowLongA)(b,-8,(LONG)nested));
    CHECK(W(GetParent)(b) == root);
    CHECK((HWND)W(SetWindowLongA)(nested,-8,(LONG)a) == b);
    CHECK(W(MoveWindow)(b,150,20,80,70,0));
    CHECK(W(ChildWindowFromPointEx)(root,(POINT){60,30},3) == a);
    CHECK(W(MoveWindow)(b,50,20,90,80,0));
    W(ShowWindow)(b,0);
    CHECK(W(ChildWindowFromPointEx)(root,(POINT){60,30},3) == a);
    W(ShowWindow)(b,SW_SHOWDEFAULT);

    paint(b); paint(a); paint(nested); paint(root);
    HDC dc = W(GetDC)(root);
    CHECK(W(GetPixel)(dc,60,30) == RGB(0,0,255));
    CHECK(W(GetPixel)(dc,15,55) == RGB(0,255,0));
    CHECK(W(GetPixel)(dc,5,55) == RGB(0,0,0));
    CHECK(W(GetPixel)(dc,15,95) == RGB(0,0,0));
    /* Non-fill primitives use exactly the same exclusions. */
    W(SetPixel)(dc,60,30,RGB(255,255,255));
    W(PatBlt)(dc,0,0,240,160,WHITENESS);
    CHECK(W(GetPixel)(dc,60,30) == RGB(0,0,255));
    CHECK(W(GetPixel)(dc,15,55) == RGB(0,255,0));
    W(ReleaseDC)(root,dc);
    W(SetWindowPos)(b,(HWND)1,0,0,0,0,3);
    paint(a); paint(nested); paint(b);
    dc = W(GetDC)(root);
    CHECK(W(GetPixel)(dc,60,30) == RGB(255,0,0));
    W(ReleaseDC)(root,dc);
    W(SetWindowPos)(b,0,0,0,0,0,3);
    CHECK(W(GetWindow)(a,3) == b);
    CHECK(!W(SetWindowPos)(b,nested,0,0,0,0,3)); /* another parent's child */
    W(SetFocus)(nested);
    W(SetCapture)(nested);
    CHECK(!W(EnableWindow)(a,0));
    CHECK(!W(GetFocus)() && !W(GetCapture)() && !W(IsWindowEnabled)(a));
    CHECK(W(EnableWindow)(a,1));
    W(ShowWindow)(root,SW_SHOWDEFAULT);
    W(InvalidateRect)(root,0,1);
    W(UpdateWindow)(root); W(UpdateWindow)(a); W(UpdateWindow)(b); W(UpdateWindow)(nested);
    W(SetWindowTextA)(root,ok ? "children-ready" : "children-failed");
    MSG msg;
    while (W(GetMessageA)(&msg,0,0,0) > 0) {
        W(TranslateMessage)(&msg);
        W(DispatchMessageA)(&msg);
    }
    CHECK(!W(IsWindow)(a) && !W(IsWindow)(nested));
    W(ExitProcess)(ok ? msg.wParam : 1);
}
