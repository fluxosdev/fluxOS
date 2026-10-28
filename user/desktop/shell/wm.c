/*
 * Window manager and compositor: windows, the taskbar with its Start
 * menu and clock, desktop icons, the mouse pointer, and input routing.
 * The sidebar (sidebar.c) is composed over the windows as the taskbar is,
 * and windows are placed and maximized beside it.
 *
 * Changes only record damaged rectangles; wm_flush() repaints each one
 * from the bottom of the scene to the top into the back buffer and
 * copies it to the framebuffer. A window moves with its contents as its
 * caption is dragged; resizing drags an outline, as in Windows 95, so a
 * resize only repaints the outline until the button is let go.
 */
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <flux/logo.h>
#include "desktop.h"
#include "simd.h"

#define MAX_DAMAGE   32
#define DBLCLICK_MS  500
#define OUTLINE      3          /* resize outline thickness */
#define ICON_CELL_W  75
#define ICON_CELL_H  70
#define MENU_ITEM_H  36
#define MENU_SEP_H   8
#define SUB_ITEM_H   24         /* items of a cascading menu: 16 pixel icons */
#define BANNER_W     21
#define KHEAD        46         /* Kinetik: the Start menu's heading */
#define SHADOW_BLUR  12         /* Kinetik: the shadow under frames and menus */
#define SHADOW_DY    5
#define SHADOW_COLOR ARGB(0x48, 0x020B19)

struct surface screen;
int screen_w, screen_h;

static uint8_t *fb;
static struct fbinfo fbi;
/* How present() writes pixels to the screen. */
enum {
    FB_GENERIC,
    FB_RGB32,                           /* 32 bpp with the 0xRRGGBB layout */
    FB_RGB24,                           /* the same in 24 bpp */
    FB_16,                              /* 15 or 16 bpp, red highest */
};
static int fb_kind;
static int fb16_shift[3];               /* FB_16: right shifts of red, green, blue */
static uint32_t fb16_mask[3];           /* and their bits in the pixel */

/* With two pages of the screen, frames are drawn on the one not shown and
 * flipped to; that page still lacks what changed in the frame before.
 * Both this and the hardware pointer are used only when the adapter's own
 * driver is chosen in Display Properties (fluxos_driver in grubenv). */
static int accel_wanted;
static int flip_ok;
static uint32_t page_offset;
static int shown_page;                  /* the page on the screen */
static struct rect shown[MAX_DAMAGE];
static int nshown;
static int hw_cursor;                   /* the adapter draws the pointer */
static int gpu_copy;                    /* and copies between the pages (DISPF_ACCEL) */
static int gpu_present;                 /* and copies the back buffer to the screen (DISPF_GART) */
static uint32_t screen_handle;          /* the back buffer, lent to the GPU */
static void *screen_mem;                /* its allocation */

static struct window *bottom;           /* z-order list, bottom to top */
static struct window *focused;

static struct rect damage[MAX_DAMAGE];
static int ndamage;

static int mx, my, mbuttons;

enum drag {
    DRAG_NONE,
    DRAG_MOVE,                          /* caption: moving the window */
    DRAG_RESIZE,                        /* a window edge: resizing the outline */
    DRAG_TITLE_BUTTON,                  /* holding a caption button */
    DRAG_CLIENT,                        /* the client area has the mouse */
    DRAG_TASK,                          /* holding a taskbar button */
    DRAG_MENU,                          /* the Start menu is tracking */
    DRAG_ICON,                          /* moving a desktop icon */
    DRAG_CTX,                           /* the context menu is tracking */
};
static enum drag drag;
static struct window *drag_win;
static int drag_button;                 /* TB_* while DRAG_TITLE_BUTTON */
static int drag_pressed;                /* the held button is drawn pushed */
static int drag_dx, drag_dy;
static struct rect outline;
static int drag_edges;                  /* E_* while DRAG_RESIZE */
static int drag_mx, drag_my;            /* where a resize started */
static struct rect drag_start;          /* the frame when it did */
static enum cursor_kind cursor_kind;
static struct window *last_caption_win;  /* for double-clicking a caption */
static unsigned int last_caption_ms;

enum { E_L = 1, E_R = 2, E_T = 4, E_B = 8 };
#define GRIP 16                         /* corner grab length along an edge */

static int menu_open;
static int menu_hover = -1;
static int menu_sub = -1;               /* the item whose submenu is open, or -1 */
static int sub_hover = -1;              /* the highlighted item in that submenu */
static int task_pressed_idx = -1;
static char clock_text[16];

/* ---- desktop icons and the Start menu ---- */

static void open_terminal(void)
{
    terminal_open("Terminal", NULL, 0, 0);
}

static void open_computer(void)
{
    files_open("/");
}

static void open_editor(void)
{
    editor_open(NULL);
}

static void open_calculator(void)
{
    calculator_open();
}

static void open_player(void)
{
    player_open(NULL);
}

static void open_browser(void)
{
    browser_open(NULL);
}

static void open_gears(void)
{
    gears_open();
}

static void open_taskmgr(void)
{
    taskmgr_open();
}

static void open_diskmgr(void)
{
    diskmgr_open();
}

static void open_home(void)
{
    const char *home = getenv("HOME");
    files_open(home ? home : "/");
}

static void open_minesweeper(void)
{
    minesweeper_open();
}

static void open_paint(void)
{
    paint_open(NULL);
}

static void open_archiver(void)
{
    archiver_open(NULL);
}

static void open_readme(void)
{
    editor_open("/home/flux/README.txt");
}

struct desk_icon {
    enum icon_id icon;
    const char *label;
    void (*open)(void);
    int x, y;                           /* the cell's top left corner */
};

static struct desk_icon desk_icons[] = {
    { ICON_COMPUTER, "My Computer", open_computer, 0, 0 },
    { ICON_TERMINAL, "Terminal", open_terminal, 0, 0 },
    { ICON_BROWSER, "Wave", open_browser, 0, 0 },
    { ICON_CALC, "Calculator", open_calculator, 0, 0 },
    { ICON_README, "Read Me", open_readme, 0, 0 },
};
static int ndesk = (int)(sizeof(desk_icons) / sizeof(desk_icons[0]));
#define NDESK ndesk
static int desk_selected = -1;
static int last_click_icon = -1;
static int drag_icon;                   /* the icon DRAG_ICON is moving */
static int icon_moved;                  /* it left its cell */
static int icon_ox, icon_oy;            /* where it started */
static int icon_px, icon_py;            /* where the pointer went down */
static unsigned int last_click_ms;

struct menu_item {
    enum icon_id icon;
    const char *label;                  /* NULL: separator */
    void (*run)(void);
    const struct menu_item *sub;        /* a cascading menu instead of run */
    int nsub;
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const struct menu_item programs_items[] = {
    { ICON_BROWSER, "Wave", open_browser, NULL, 0 },
    { ICON_TERMINAL, "Terminal", open_terminal, NULL, 0 },
    { ICON_GEARS, "3D Gears", open_gears, NULL, 0 },
    { ICON_MINES, "Minesweeper", open_minesweeper, NULL, 0 },
};

static const struct menu_item accessories_items[] = {
    { ICON_FOLDER, "File Manager", open_home, NULL, 0 },
    { ICON_EDITOR, "Text Editor", open_editor, NULL, 0 },
    { ICON_CALC, "Calculator", open_calculator, NULL, 0 },
    { ICON_SOUND, "Sound Player", open_player, NULL, 0 },
    { ICON_ARCHIVE, "Archive Manager", open_archiver, NULL, 0 },
    { ICON_PAINT, "Paint", open_paint, NULL, 0 },
};

static const struct menu_item system_items[] = {
    { ICON_TASKMGR, "Task Manager", open_taskmgr, NULL, 0 },
    { ICON_DRIVE, "Disk Manager", open_diskmgr, NULL, 0 },
};

static const struct menu_item settings_items[] = {
    { ICON_DISPLAY, "Display Properties", display_open, NULL, 0 },
    { ICON_DATETIME, "Date and Time", datetime_open, NULL, 0 },
    { ICON_COMPUTER, "System Properties", about_open, NULL, 0 },
};

static struct menu_item menu_items[] = {
    { ICON_FOLDER_OPEN, "Programs", NULL, programs_items, COUNT(programs_items) },
    { ICON_FOLDER, "Accessories", NULL, accessories_items, COUNT(accessories_items) },
    { ICON_GEARS, "System Tools", NULL, system_items, COUNT(system_items) },
    { ICON_DISPLAY, "Settings", NULL, settings_items, COUNT(settings_items) },
    { ICON_NONE, NULL, NULL, NULL, 0 },
    { ICON_README, "Read Me", open_readme, NULL, 0 },
    { ICON_RUN, "Run...", run_open, NULL, 0 },
    { ICON_NONE, NULL, NULL, NULL, 0 },
    { ICON_SHUTDOWN, "Shut Down...", shutdown_open, NULL, 0 },
};
static int nmenu = (int)(sizeof(menu_items) / sizeof(menu_items[0]));
#define NMENU nmenu

/*
 * Setup mode: the desktop as the install CD starts it, for fluxOS Setup
 * alone. Setup paints the whole background; there is no taskbar, Start
 * menu or desktop icons, and Shift+F10 opens a command prompt for when
 * something needs looking into, as in Windows Setup.
 */
static int setup_mode;

/* Where windows end: above the taskbar, or at the bottom in setup mode. */
static int work_bottom(void)
{
    return setup_mode ? screen_h : screen_h - TASKBAR_H;
}

/* And on the right: beside the sidebar. */
static int work_right(void)
{
    return setup_mode ? screen_w : screen_w - sidebar_width();
}

int wm_work_right(void)
{
    return work_right();
}

/* ---- the desktop context menu ---- */

#define CTX_ITEM_H 22
#define CTX_SEP_H 8
#define ICON_MARGIN 4

static void arrange_icons(void);
static void open_ctx_icon(void);
static void minimize_all(void);
static void tile_horizontally(void);
static void tile_vertically(void);

static void toggle_sidebar(void)
{
    sidebar_show(!sidebar_shown());
}

/* What right-clicking the taskbar offers. */
static const struct ctx_item taskbar_ctx[] = {
    { "Task Manager", ICON_TASKMGR, open_taskmgr, NULL },
    { NULL, ICON_NONE, NULL, NULL },
    { "Show Sidebar", ICON_NONE, toggle_sidebar, sidebar_shown },
    { NULL, ICON_NONE, NULL, NULL },
    { "Minimize All Windows", ICON_NONE, minimize_all, NULL },
    { "Tile Windows Horizontally", ICON_NONE, tile_horizontally, NULL },
    { "Tile Windows Vertically", ICON_NONE, tile_vertically, NULL },
};

static const struct ctx_item desktop_ctx[] = {
    { "Arrange Icons", ICON_NONE, arrange_icons, NULL },
    { NULL, ICON_NONE, NULL, NULL },
    { "Terminal", ICON_TERMINAL, open_terminal, NULL },
    { "File Manager", ICON_FOLDER, open_home, NULL },
    { "Task Manager", ICON_TASKMGR, open_taskmgr, NULL },
    { NULL, ICON_NONE, NULL, NULL },
    { "Display Properties", ICON_DISPLAY, display_open, NULL },
};

static const struct ctx_item icon_ctx[] = {
    { "Open", ICON_NONE, open_ctx_icon, NULL },
};

static const struct ctx_item *ctx_items;
static int ctx_n;
static int ctx_open;
static int ctx_hover = -1;
static int ctx_x, ctx_y;
static int ctx_icon = -1;               /* the icon the menu was opened on */

/* What a frame or menu covers, its shadow included. */
static struct rect shadowed(struct rect r)
{
    if (!KINETIK)
        return r;
    return mkrect(r.x - SHADOW_BLUR, r.y - SHADOW_BLUR + SHADOW_DY, r.w + 2 * SHADOW_BLUR, r.h + 2 * SHADOW_BLUR);
}

/* The Start menu's labels: larger in Kinetik. */
static const struct pfont *menu_font(void)
{
    return KINETIK ? &font_archivo_title : &font_sans;
}

static int ctx_item_height(int i)
{
    return ctx_items[i].label ? CTX_ITEM_H : CTX_SEP_H;
}

static struct rect ctx_rect(void)
{
    int h = 4, tw = 0;
    for (int i = 0; i < ctx_n; i++) {
        h += ctx_item_height(i);
        if (ctx_items[i].label) {
            int w = gfx_text_width(&font_sans, ctx_items[i].label);
            if (w > tw)
                tw = w;
        }
    }
    int w = 4 + 4 + 16 + 8 + tw + 16;
    int x = ctx_x, y = ctx_y;
    if (x + w > screen_w)
        x = screen_w - w;
    if (y + h > screen_h - TASKBAR_H)
        y = screen_h - TASKBAR_H - h;
    return mkrect(x < 0 ? 0 : x, y < 0 ? 0 : y, w, h);
}

static struct rect ctx_item_rect(int idx)
{
    struct rect m = ctx_rect();
    int y = m.y + 2;
    for (int i = 0; i < idx; i++)
        y += ctx_item_height(i);
    return mkrect(m.x + 2, y, m.w - 4, ctx_item_height(idx));
}

static int ctx_hit(int x, int y)
{
    if (!ctx_open)
        return -1;
    for (int i = 0; i < ctx_n; i++)
        if (ctx_items[i].label && rect_contains(ctx_item_rect(i), x, y))
            return i;
    return -1;
}

static void set_ctx(const struct ctx_item *items, int n, int x, int y, int icon)
{
    if (ctx_open)
        wm_damage(shadowed(ctx_rect()));
    ctx_open = items != NULL;
    ctx_hover = -1;
    if (items) {
        ctx_items = items;
        ctx_n = n;
        ctx_x = x;
        ctx_y = y;
        ctx_icon = icon;
        wm_damage(shadowed(ctx_rect()));
    }
}

void wm_popup(const struct ctx_item *items, int n, int x, int y)
{
    set_ctx(items, n, x, y, -1);
}

static void set_ctx_hover(int i)
{
    if (i == ctx_hover)
        return;
    if (ctx_hover >= 0)
        wm_damage(ctx_item_rect(ctx_hover));
    ctx_hover = i;
    if (i >= 0)
        wm_damage(ctx_item_rect(i));
}

/* ---- time ---- */

unsigned int now_ms(void)
{
    return uptime_ms();
}

static int update_clock(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[16];
    int h = tm->tm_hour % 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s", h ? h : 12, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
    if (!strcmp(buf, clock_text))
        return 0;
    strcpy(clock_text, buf);
    return 1;
}

/* ---- geometry ---- */

static struct rect taskbar_rect(void)
{
    if (setup_mode)
        return mkrect(0, screen_h, 0, 0);
    return mkrect(0, screen_h - TASKBAR_H, screen_w, TASKBAR_H);
}

static struct rect start_button_rect(void)
{
    if (KINETIK)
        return mkrect(4, screen_h - TASKBAR_H + 3, 7 + 24 + 6 + gfx_text_width(&font_archivo_start, "menu") + 12,
                      TASKBAR_H - 6);
    return mkrect(2, screen_h - TASKBAR_H + 4, 54, 22);
}

static struct rect tray_rect(void)
{
    int w = gfx_text_width(&font_sans, "88:88 PM") + 18;
    if (sound_tray_icon() != ICON_NONE)
        w += 20;                        /* the speaker */
    if (power_tray_present())
        w += 20;                        /* the battery */
    return mkrect(screen_w - 2 - w, screen_h - TASKBAR_H + 4, w, TASKBAR_H - 8);
}

static struct rect tray_icon_rect(void)
{
    struct rect tr = tray_rect();
    return mkrect(tr.x + 3, tr.y + (tr.h - 16) / 2, 16, 16);
}

/* The battery: after the speaker, if there is one. */
static struct rect battery_icon_rect(void)
{
    struct rect r = tray_icon_rect();
    if (sound_tray_icon() != ICON_NONE)
        r.x += 20;
    return power_tray_present() ? r : mkrect(r.x, r.y, 0, 0);
}

#define SB_BUTTON_W 22

/* While the sidebar is collapsed: a button before the tray to open it again. */
static struct rect sidebar_button_rect(void)
{
    struct rect tr = tray_rect();
    if (setup_mode || !sidebar_collapsed())
        return mkrect(tr.x, tr.y, 0, 0);
    return mkrect(tr.x - 3 - SB_BUTTON_W, tr.y, SB_BUTTON_W, tr.h);
}

/* The tray may have grown or shrunk too: the whole taskbar. */
void wm_tray_changed(void)
{
    wm_damage(taskbar_rect());
}

/* Taskbar buttons are in creation order, which is kept in task_order. */
static struct window *task_order[64];
static int ntasks;

static struct rect task_button_rect(int i)
{
    struct rect sb = start_button_rect();
    int x0 = sb.x + sb.w + (KINETIK ? 10 : 4), x1 = tray_rect().x - 4, gap = KINETIK ? 5 : 3;
    if (!rect_empty(sidebar_button_rect()))
        x1 -= SB_BUTTON_W + 3;
    int n = ntasks ? ntasks : 1;
    int w = (x1 - x0 - (n - 1) * gap) / n;
    if (w > (KINETIK ? 180 : 160))
        w = KINETIK ? 180 : 160;
    return mkrect(x0 + i * (w + gap), screen_h - TASKBAR_H + 4, w, TASKBAR_H - 8);
}

static int menu_item_height(int i)
{
    return menu_items[i].label ? MENU_ITEM_H : MENU_SEP_H;
}

/* Classic: a banner down the left, items beside it. Kinetik: a heading across the top, items below. */
static struct rect menu_rect(void)
{
    int h = KINETIK ? KHEAD + 12 : 6;
    int tw = 0;
    for (int i = 0; i < NMENU; i++) {
        h += menu_item_height(i);
        if (menu_items[i].label) {
            int w = gfx_text_width(menu_font(), menu_items[i].label);
            if (w > tw)
                tw = w;
        }
    }
    if (KINETIK)
        return mkrect(4, screen_h - TASKBAR_H - h - 2, 12 + 32 + 12 + tw + 40, h);
    int w = 6 + BANNER_W + 4 + 32 + 10 + tw + 24;
    return mkrect(2, screen_h - TASKBAR_H - h + 2, w, h);
}

static struct rect menu_item_rect(int idx)
{
    struct rect m = menu_rect();
    int y = KINETIK ? m.y + KHEAD + 6 : m.y + 3;
    for (int i = 0; i < idx; i++)
        y += menu_item_height(i);
    if (KINETIK)
        return mkrect(m.x + 6, y, m.w - 12, menu_item_height(idx));
    return mkrect(m.x + 3 + BANNER_W, y, m.w - 6 - BANNER_W, menu_item_height(idx));
}

/* The cascading menu of main item p: beside it, level with it, kept on the screen. */
static struct rect sub_rect(int p)
{
    const struct menu_item *it = &menu_items[p];
    int tw = 0;
    for (int i = 0; i < it->nsub; i++) {
        int w = gfx_text_width(&font_sans, it->sub[i].label);
        if (w > tw)
            tw = w;
    }
    int pad = KINETIK ? 4 : 3;
    int w = 2 * pad + 4 + 16 + 8 + tw + 20, h = 2 * pad + it->nsub * SUB_ITEM_H;
    struct rect m = menu_rect(), r = menu_item_rect(p);
    int y = r.y - pad;
    if (y + h > screen_h - TASKBAR_H + 2)
        y = screen_h - TASKBAR_H + 2 - h;
    return mkrect(m.x + m.w - (KINETIK ? 2 : 3), y, w, h);
}

static struct rect sub_item_rect(int p, int j)
{
    struct rect m = sub_rect(p);
    int pad = KINETIK ? 4 : 3;
    return mkrect(m.x + pad, m.y + pad + j * SUB_ITEM_H, m.w - 2 * pad, SUB_ITEM_H);
}

static struct rect desk_icon_rect(int i)
{
    return mkrect(desk_icons[i].x, desk_icons[i].y, ICON_CELL_W, ICON_CELL_H - 4);
}

/* Lay the icons out in columns down the left edge, as they start. */
static void arrange_icons(void)
{
    int per = (screen_h - TASKBAR_H - 6) / ICON_CELL_H;
    if (per < 1)
        per = 1;
    for (int i = 0; i < NDESK; i++) {
        wm_damage(desk_icon_rect(i));
        desk_icons[i].x = ICON_MARGIN + (i / per) * ICON_CELL_W;
        desk_icons[i].y = 6 + (i % per) * ICON_CELL_H;
        wm_damage(desk_icon_rect(i));
    }
}

static void open_ctx_icon(void)
{
    if (ctx_icon >= 0 && ctx_icon < NDESK)
        desk_icons[ctx_icon].open();
}

/* Drop the dragged icon onto the nearest grid cell, or back if that one is taken. */
static void place_icon(int i)
{
    int cx = (desk_icons[i].x - ICON_MARGIN + ICON_CELL_W / 2) / ICON_CELL_W;
    int cy = (desk_icons[i].y - 6 + ICON_CELL_H / 2) / ICON_CELL_H;
    int maxx = (work_right() - ICON_CELL_W - ICON_MARGIN) / ICON_CELL_W;
    int maxy = (screen_h - TASKBAR_H - 6 - ICON_CELL_H) / ICON_CELL_H;
    if (cx < 0)
        cx = 0;
    if (cy < 0)
        cy = 0;
    if (cx > maxx)
        cx = maxx;
    if (cy > maxy)
        cy = maxy;
    int nx = ICON_MARGIN + cx * ICON_CELL_W, ny = 6 + cy * ICON_CELL_H;
    for (int j = 0; j < NDESK; j++)
        if (j != i && desk_icons[j].x == nx && desk_icons[j].y == ny) {
            nx = icon_ox;
            ny = icon_oy;
            break;
        }
    wm_damage(desk_icon_rect(i));
    desk_icons[i].x = nx;
    desk_icons[i].y = ny;
    wm_damage(desk_icon_rect(i));
}

static struct rect caption_rect(struct window *w)
{
    int in = theme.cap_inset;
    return mkrect(w->r.x + in, w->r.y + in, w->r.w - 2 * in, TITLE_H);
}

struct rect wm_client_rect(struct window *w)
{
    return mkrect(w->r.x + BORDER, w->r.y + FRAME_TOP, w->content.w, w->content.h);
}

static int resizable(const struct window *w)
{
    return (w->flags & WF_MAXIMIZE) && w->ops && w->ops->resize;
}

/* A maximize button that does nothing is drawn disabled. */
static int button_disabled(const struct window *w, int kind)
{
    return (kind == TB_MAXIMIZE || kind == TB_RESTORE) && !resizable(w);
}

/* Caption buttons from right to left: close, then maximize and minimize. */
static int title_buttons(struct window *w, struct rect *rects, int *kinds)
{
    struct rect c = caption_rect(w);
    int bw = KINETIK ? 22 : 16, bh = KINETIK ? 20 : 14, gap = KINETIK ? 1 : 2;
    int x = c.x + c.w - (KINETIK ? 6 : 2) - bw, y = c.y + (TITLE_H - bh) / 2, n = 0;
    rects[n] = mkrect(x, y, bw, bh);
    kinds[n++] = TB_CLOSE;
    x -= gap;
    if (w->flags & WF_MAXIMIZE) {
        x -= bw;
        rects[n] = mkrect(x, y, bw, bh);
        kinds[n++] = w->maximized ? TB_RESTORE : TB_MAXIMIZE;
    }
    if ((w->flags & WF_MINIMIZE) && !setup_mode) {    /* no taskbar to bring it back */
        x -= bw;
        rects[n] = mkrect(x, y, bw, bh);
        kinds[n++] = TB_MINIMIZE;
    }
    return n;
}

/* ---- damage and presentation ---- */

/* Whether a lies wholly within b (an empty a does). */
static int inside(struct rect a, struct rect b)
{
    return rect_empty(a) || (a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h);
}

static int area(struct rect r)
{
    return r.w * r.h;
}

/* Add r to a list of at most MAX_DAMAGE rectangles, merging where cheap. */
static void add_rect(struct rect *list, int *n, struct rect r)
{
    for (int i = 0; i < *n; i++) {
        struct rect u = rect_union(list[i], r);
        if (area(u) <= area(list[i]) + area(r) + 4096) {
            list[i] = u;
            return;
        }
    }
    if (*n == MAX_DAMAGE) {
        for (int i = 1; i < *n; i++)
            list[0] = rect_union(list[0], list[i]);
        list[0] = rect_union(list[0], r);
        *n = 1;
        return;
    }
    list[(*n)++] = r;
}

void wm_damage(struct rect r)
{
    r = rect_intersect(r, mkrect(0, 0, screen_w, screen_h));
    if (!rect_empty(r))
        add_rect(damage, &ndamage, r);
}

static void classify_fb(void)
{
    int rgb = fbi.red_pos == 16 && fbi.green_pos == 8 && fbi.blue_pos == 0;
    fb_kind = FB_GENERIC;
    if (fbi.bpp == 32 && rgb) {
        fb_kind = FB_RGB32;
    } else if (fbi.bpp == 24 && rgb) {
        fb_kind = FB_RGB24;
    } else if (fbi.bpp == 15 || fbi.bpp == 16) {
        /* Each channel's top bits move right into place, masked. */
        const int from[3] = { 16, 8, 0 };
        const int pos[3] = { fbi.red_pos, fbi.green_pos, fbi.blue_pos };
        const int size[3] = { fbi.red_size, fbi.green_size, fbi.blue_size };
        fb_kind = FB_16;
        for (int c = 0; c < 3; c++) {
            fb16_shift[c] = from[c] + 8 - size[c] - pos[c];
            fb16_mask[c] = ((1u << size[c]) - 1) << pos[c];
            if (size[c] < 1 || size[c] > 8 || fb16_shift[c] < 0)
                fb_kind = FB_GENERIC;
        }
    }
}

/*
 * Rows of pixels to the screen. The framebuffer is write-combining (when
 * the processor has a PAT), and streaming stores (MOVNTPS, MOVNTQ) fill
 * whole write-combining buffers without reading anything back.
 */
static void put_row32(uint8_t *dst, const uint32_t *src, int n)
{
    uint32_t *d = (uint32_t *)dst;
    int i = 0;
    for (; i < n && ((uintptr_t)(d + i) & 15); i++)
        d[i] = src[i];
    if (((uintptr_t)(src + i) & 15) == 0) {
        for (; i + 4 <= n; i += 4)
            _mm_stream_ps((float *)(d + i), _mm_load_ps((const float *)(src + i)));
    } else {
        for (; i + 4 <= n; i += 4)
            _mm_stream_ps((float *)(d + i), _mm_loadu_ps((const float *)(src + i)));
    }
    for (; i < n; i++)
        d[i] = src[i];
}

/* Four pixels to three words. */
static void put_row24(uint8_t *dst, const uint32_t *src, int n)
{
    int i = 0;
    for (; i < n && ((uintptr_t)dst & 3); i++, dst += 3) {
        dst[0] = (uint8_t)src[i];
        dst[1] = (uint8_t)(src[i] >> 8);
        dst[2] = (uint8_t)(src[i] >> 16);
    }
    uint32_t *d = (uint32_t *)dst;
    for (; i + 4 <= n; i += 4, d += 3) {
        uint32_t p0 = src[i], p1 = src[i + 1], p2 = src[i + 2], p3 = src[i + 3];
        d[0] = (p0 & 0xFFFFFF) | p1 << 24;
        d[1] = (p1 >> 8 & 0xFFFF) | p2 << 16;
        d[2] = (p2 >> 16 & 0xFF) | p3 << 8;
    }
    for (dst = (uint8_t *)d; i < n; i++, dst += 3) {
        dst[0] = (uint8_t)src[i];
        dst[1] = (uint8_t)(src[i] >> 8);
        dst[2] = (uint8_t)(src[i] >> 16);
    }
}

static inline uint16_t pack16(uint32_t p)
{
    return (uint16_t)((p >> fb16_shift[0] & fb16_mask[0]) | (p >> fb16_shift[1] & fb16_mask[1]) |
                      (p >> fb16_shift[2] & fb16_mask[2]));
}

/* Two pixels' 15/16-bit values, in the low halves of their lanes. */
static inline __m64 pack16x2(__m64 p, const __m64 *sh, const __m64 *mask)
{
    __m64 v = _mm_or_si64(_mm_or_si64(_mm_and_si64(_mm_srl_pi32(p, sh[0]), mask[0]),
                                      _mm_and_si64(_mm_srl_pi32(p, sh[1]), mask[1])),
                          _mm_and_si64(_mm_srl_pi32(p, sh[2]), mask[2]));
    return _mm_srai_pi32(_mm_slli_pi32(v, 16), 16);     /* sign extended: PACKSSDW keeps it */
}

/* Four pixels at a time in MMX registers; the caller runs simd_done(). */
static void put_row16(uint8_t *dst, const uint32_t *src, int n)
{
    uint16_t *d = (uint16_t *)dst;
    __m64 sh[3], mask[3];
    for (int c = 0; c < 3; c++) {
        sh[c] = _mm_cvtsi32_si64(fb16_shift[c]);
        mask[c] = _mm_set1_pi32((int)fb16_mask[c]);
    }
    int i = 0;
    for (; i < n && ((uintptr_t)(d + i) & 7); i++)
        d[i] = pack16(src[i]);
    for (; i + 4 <= n; i += 4) {
        __m64 a = pack16x2(*(const __m64_u *)(src + i), sh, mask);
        __m64 b = pack16x2(*(const __m64_u *)(src + i + 2), sh, mask);
        _mm_stream_pi((__m64 *)(d + i), _mm_packs_pi32(a, b));
    }
    for (; i < n; i++)
        d[i] = pack16(src[i]);
}

/* Copy r of the back buffer to a page of the screen. */
static void present(uint8_t *page, struct rect r)
{
    uint32_t bytespp = (fbi.bpp + 7) / 8;
    for (int j = r.y; j < r.y + r.h; j++) {
        const uint32_t *src = screen.px + j * screen.stride + r.x;
        uint8_t *dst = page + (uint32_t)j * fbi.pitch + (uint32_t)r.x * bytespp;
        switch (fb_kind) {
        case FB_RGB32:
            put_row32(dst, src, r.w);
            continue;
        case FB_RGB24:
            put_row24(dst, src, r.w);
            continue;
        case FB_16:
            put_row16(dst, src, r.w);
            continue;
        }
        for (int i = 0; i < r.w; i++, dst += bytespp) {
            uint32_t c = src[i];
            uint32_t v = (((c >> 16) & 0xFF) >> (8 - fbi.red_size)) << fbi.red_pos |
                         (((c >> 8) & 0xFF) >> (8 - fbi.green_size)) << fbi.green_pos |
                         ((c & 0xFF) >> (8 - fbi.blue_size)) << fbi.blue_pos;
            if (bytespp == 4) {
                *(uint32_t *)dst = v;
            } else if (bytespp == 3) {
                dst[0] = (uint8_t)v;
                dst[1] = (uint8_t)(v >> 8);
                dst[2] = (uint8_t)(v >> 16);
            } else {
                *(uint16_t *)dst = (uint16_t)v;
            }
        }
    }
    _mm_sfence();                       /* the streamed pixels are on their way */
    simd_done();
}

/* The back buffer, laid out for the GPU too: page aligned, lines a
 * multiple of 256 bytes. */
static int screen_alloc(struct surface *s, int w, int h)
{
    int stride = (w + 63) & ~63;
    size_t bytes = (size_t)stride * (size_t)h * 4;
    void *mem = malloc(bytes + 4095);
    if (!mem)
        return -1;
    s->px = (uint32_t *)(((uintptr_t)mem + 4095) & ~(uintptr_t)4095);
    memset(s->px, 0, bytes);            /* every page present, for DISP_MAPBUF */
    s->w = w;
    s->h = h;
    s->stride = stride;
    gfx_noclip(s);
    screen_mem = mem;
    return 0;
}

static void unlend_screen(void)
{
    if (screen_handle) {
        struct dispbuf b = { 0, 0, screen_handle, 0, 0 };
        display(DISP_UNMAPBUF, &b);
        screen_handle = 0;
    }
    gpu_present = 0;
}

/* Lend the back buffer to the GPU so it can copy from it. */
static void lend_screen(void)
{
    struct dispbuf b = { (uint32_t)(uintptr_t)screen.px, (uint32_t)(screen.stride * screen.h * 4), 0, 0, 0 };
    if (display(DISP_MAPBUF, &b) == 0) {
        screen_handle = b.handle;
        gpu_present = 1;
    }
}

/* Have the GPU copy rectangles of the back buffer to a page. */
static int gpu_show(const struct rect *r, int n, int page)
{
    struct dispblit b;
    memset(&b, 0, sizeof(b));
    b.src.kind = DISP_SURF_BUFFER;
    b.src.handle = screen_handle;
    b.src.pitch = (uint32_t)screen.stride * 4;
    b.src.width = (uint32_t)screen.w;
    b.src.height = (uint32_t)screen.h;
    b.dst.kind = DISP_SURF_PAGE;
    b.dst.handle = (uint32_t)page;
    b.count = (uint32_t)n;
    for (int i = 0; i < n; i++) {
        b.rects[i].sx = b.rects[i].dx = (int16_t)r[i].x;
        b.rects[i].sy = b.rects[i].dy = (int16_t)r[i].y;
        b.rects[i].w = (int16_t)r[i].w;
        b.rects[i].h = (int16_t)r[i].h;
    }
    return display(DISP_BLIT, &b);
}

/* Put the changed rectangles of the back buffer on the screen. */
static void show_frame(const struct rect *r, int n)
{
    if (!flip_ok) {
        for (int i = 0; i < n; i++)
            present(fb + shown_page * page_offset, r[i]);
        return;
    }
    /* The page to draw on still lacks last frame's changes: the 3D engine
     * copies them over from the page on the screen, or the CPU draws them. */
    struct rect todo[MAX_DAMAGE];
    int ntodo = 0;
    if (gpu_present) {
        /* The GPU copies this frame's and last frame's changes from memory. */
        for (int i = 0; i < nshown; i++)
            add_rect(todo, &ntodo, shown[i]);
        for (int i = 0; i < n; i++)
            add_rect(todo, &ntodo, r[i]);
        int back = shown_page ^ 1;
        if (gpu_show(todo, ntodo, back) == 0) {
            struct dispflip f = { (uint32_t)back };
            shown_page = back;
            if (display(DISP_FLIP, &f) < 0) {
                flip_ok = 0;
                unlend_screen();
                return;
            }
            memcpy(shown, r, (size_t)n * sizeof(*r));
            nshown = n;
            return;
        }
        unlend_screen();                /* the CPU draws from now on */
        ntodo = 0;
    }
    if (gpu_copy && nshown) {
        struct dispcopy c;
        c.from_page = (uint32_t)shown_page;
        c.count = (uint32_t)nshown;
        for (int i = 0; i < nshown; i++) {
            c.rects[i].x = (int16_t)shown[i].x;
            c.rects[i].y = (int16_t)shown[i].y;
            c.rects[i].w = (int16_t)shown[i].w;
            c.rects[i].h = (int16_t)shown[i].h;
        }
        if (display(DISP_COPYRECTS, &c) < 0)
            gpu_copy = 0;
    }
    if (!gpu_copy)
        for (int i = 0; i < nshown; i++)
            add_rect(todo, &ntodo, shown[i]);
    for (int i = 0; i < n; i++)
        add_rect(todo, &ntodo, r[i]);
    int back = shown_page ^ 1;
    for (int i = 0; i < ntodo; i++)
        present(fb + back * page_offset, todo[i]);
    struct dispflip f = { (uint32_t)back };
    shown_page = back;
    if (display(DISP_FLIP, &f) < 0) {
        flip_ok = 0;                    /* draw on the page asked for from now on */
        return;
    }
    memcpy(shown, r, (size_t)n * sizeof(*r));
    nshown = n;
}

/* ---- the hardware pointer ---- */

static struct dispcursor cursor_dc;

/* Hand the adapter the pointer's picture. It is drawn twice, on black and
 * on white: the pixels that come out different are transparent. */
static int load_cursor(void)
{
    static uint32_t px[2][DISP_CURSOR_SIZE * DISP_CURSOR_SIZE];
    struct dispcursor *dc = &cursor_dc;
    struct rect b = cursor_bounds(0, 0, cursor_kind);
    if (b.x > 0 || b.y > 0 || -b.x >= DISP_CURSOR_SIZE || -b.y >= DISP_CURSOR_SIZE ||
        b.w > DISP_CURSOR_SIZE || b.h > DISP_CURSOR_SIZE)
        return -1;
    for (int k = 0; k < 2; k++) {
        struct surface s;
        s.px = px[k];
        s.w = s.h = s.stride = DISP_CURSOR_SIZE;
        gfx_noclip(&s);
        gfx_fill(&s, 0, 0, s.w, s.h, k ? C_WHITE : C_BLACK);
        draw_cursor(&s, -b.x, -b.y, cursor_kind);
    }
    for (int i = 0; i < DISP_CURSOR_SIZE * DISP_CURSOR_SIZE; i++)
        dc->image[i] = px[0][i] == px[1][i] ? 0xFF000000u | px[0][i] : 0;
    dc->show = 1;
    dc->hot_x = (uint16_t)-b.x;
    dc->hot_y = (uint16_t)-b.y;
    if (display(DISP_CURSOR, dc) < 0)
        return -1;
    struct disppos pos = { mx, my };
    return display(DISP_CURSORPOS, &pos);
}

static void hide_cursor(void)
{
    cursor_dc.show = 0;
    display(DISP_CURSOR, &cursor_dc);
}

/* Draw the pointer in the back buffer from now on. */
static void software_cursor(void)
{
    hide_cursor();
    hw_cursor = 0;
    wm_damage(cursor_bounds(mx, my, cursor_kind));
}

/* Flip pages and let the adapter draw the pointer, if it can and is chosen. */
static void use_adapter(void)
{
    struct dispinfo di;
    unlend_screen();
    flip_ok = hw_cursor = gpu_copy = 0;
    nshown = 0;
    if (!accel_wanted || display(DISP_INFO, &di) < 0)
        return;
    if ((di.flags & DISPF_FLIP) && di.page_offset) {
        flip_ok = 1;
        page_offset = di.page_offset;
        gpu_copy = (di.flags & DISPF_ACCEL) != 0;
        if (gpu_copy && (di.flags & DISPF_GART))
            lend_screen();
    }
    if ((di.flags & DISPF_CURSOR) && load_cursor() == 0)
        hw_cursor = 1;
}

int wm_accel(void)
{
    return flip_ok || hw_cursor;
}

void wm_set_accel(int on)
{
    accel_wanted = on;
    if (on) {
        use_adapter();
        wm_damage(mkrect(0, 0, screen_w, screen_h));    /* fills both pages */
        return;
    }
    if (hw_cursor)
        software_cursor();
    gpu_copy = 0;
    unlend_screen();
    if (flip_ok && shown_page) {
        /* Back to page 0, where the console and a plain screen draw. */
        struct dispflip f = { 0 };
        present(fb, mkrect(0, 0, screen_w, screen_h));
        if (display(DISP_FLIP, &f) == 0)
            shown_page = 0;
    }
    flip_ok = 0;
}

/* ---- drawing the scene ---- */

static void draw_desktop(void)
{
    struct rect c = screen.clip;
    if (setup_mode) {
        setup_draw_backdrop(&screen, c);
        return;
    }
    wallpaper_draw(&screen, c);

    for (int i = 0; i < NDESK; i++) {
        struct rect cell = desk_icon_rect(i);
        if (!rect_overlaps(cell, c))
            continue;
        int sel = i == desk_selected;
        int ix = cell.x + (cell.w - 32) / 2, iy = cell.y + 2;
        if (sel)
            draw_icon_selected(&screen, ix, iy, desk_icons[i].icon, 32);
        else
            draw_icon(&screen, ix, iy, desk_icons[i].icon, 32);

        int tw = gfx_text_width(&font_sans, desk_icons[i].label);
        struct rect lr = mkrect(cell.x + (cell.w - tw) / 2 - 2, iy + 32 + 3, tw + 4, font_sans.height + 1);
        if (sel)
            gfx_fill(&screen, lr.x, lr.y, lr.w, lr.h, C_NAVY);
        /* Over a photograph, white text needs a shadow to stay readable. */
        if (!sel && wallpaper_active())
            gfx_text(&screen, &font_sans, lr.x + 3, lr.y + 1, desk_icons[i].label, C_BLACK);
        gfx_text(&screen, &font_sans, lr.x + 2, lr.y, desk_icons[i].label, C_WHITE);
        if (sel && focused == NULL)
            gfx_focus_rect(&screen, lr);
    }
}

/* ---- the Kinetik theme's chrome ---- */

static void draw_menu_arrow(int x, int y, color_t c);

/* A caption or taskbar's three colors, dimmed for a window that is not active. */
static void chrome_colors(int active, color_t *c)
{
    c[0] = theme.chrome0, c[1] = theme.chrome1, c[2] = theme.chrome2;
    if (!active)
        for (int i = 0; i < 3; i++)
            c[i] = gfx_mix(c[i], RGB(0xD3, 0xDC, 0xE6), 176);
}

/* The chrome's gradient down r: top corners rounded by rt. */
static void chrome_fill(struct rect r, int rt, int rb, const color_t *c)
{
    static const int pos[3] = { 0, 115, 256 };
    color_t argb[3] = { ARGB(255, c[0]), ARGB(255, c[1]), ARGB(255, c[2]) };
    gfx_gradient(&screen, r, rt, rb, 3, argb, pos);
}

/* A glassy button on the chrome, or (pressed) a hollow one. */
static void glass(struct rect r, int rad, int pressed)
{
    static const int pos[3] = { 0, 154, 256 };
    if (pressed) {
        static const int p2[2] = { 0, 256 };
        color_t c[2] = { ARGB(255, theme.stroke), ARGB(255, theme.chrome1) };
        gfx_gradient(&screen, r, rad, rad, 2, c, p2);
    } else {
        color_t c[3] = { ARGB(48, 0xFFFFFF), ARGB(13, 0xFFFFFF), ARGB(20, 0x132C4B) };
        gfx_gradient(&screen, r, rad, rad, 3, c, pos);
    }
    gfx_round_frame(&screen, r, rad, rad, ARGB(255, theme.stroke));
}

/* The pale surface of menus, under a shadow, outlined in ink. */
static void menu_surface(struct rect m)
{
    static const int pos[2] = { 0, 256 };
    static const color_t c[2] = { ARGB(255, 0xFFFFFF), ARGB(255, 0xE4EDF6) };
    gfx_shadow(&screen, m, 7, SHADOW_DY, SHADOW_BLUR, SHADOW_COLOR);
    gfx_gradient(&screen, m, 7, 7, 2, c, pos);
    gfx_round_frame(&screen, m, 7, 7, ARGB(255, theme.ink));
}

/* A highlighted menu item. */
static void menu_hot(struct rect r)
{
    gfx_round_fill(&screen, r, 3, 3, ARGB(255, theme.pale));
    gfx_round_frame(&screen, r, 3, 3, ARGB(255, theme.wall2));
}

static void draw_window_kinetik(struct window *w)
{
    struct rect r = w->r, cl = wm_client_rect(w), cap = caption_rect(w);
    int active = w == focused, rt = w->maximized ? 0 : theme.radius, rb = w->maximized ? 0 : 3;
    color_t c[3];
    chrome_colors(active, c);
    if (!w->maximized)
        gfx_shadow(&screen, r, rt, SHADOW_DY, SHADOW_BLUR, SHADOW_COLOR);
    /* The frame round the client area is the caption's last color; the caption runs down to it.
     * Only the frame: the client area is about to be copied over whatever is there. */
    if (inside(cl, r)) {
        struct rect saved = screen.clip;
        struct rect ring[4] = {
            mkrect(r.x, r.y, r.w, cl.y - r.y),
            mkrect(r.x, cl.y + cl.h, r.w, r.y + r.h - cl.y - cl.h),
            mkrect(r.x, cl.y, cl.x - r.x, cl.h),
            mkrect(cl.x + cl.w, cl.y, r.x + r.w - cl.x - cl.w, cl.h),
        };
        for (int i = 0; i < 4; i++) {
            screen.clip = rect_intersect(saved, ring[i]);
            if (!rect_empty(screen.clip))
                gfx_round_fill(&screen, r, rt, rb, ARGB(255, c[2]));
        }
        screen.clip = saved;
    } else {
        gfx_round_fill(&screen, r, rt, rb, ARGB(255, c[2]));
    }
    chrome_fill(mkrect(r.x, r.y, r.w, theme.cap_inset + TITLE_H), rt, 0, c);
    gfx_round_fill(&screen, mkrect(r.x + rt, r.y + 1, r.w - 2 * rt, 1), 0, 0, ARGB(64, 0xFFFFFF));
    gfx_round_frame(&screen, r, rt, rb, ARGB(255, active ? theme.stroke : gfx_mix(theme.stroke, 0x9AA9B9, 176)));

    color_t text = active ? theme.on_chrome : RGB(0x5A, 0x6C, 0x80);
    if (w->icon_img)
        gfx_image(&screen, cap.x + 7, cap.y + (TITLE_H - 16) / 2, w->icon_img);
    else
        draw_icon(&screen, cap.x + 7, cap.y + (TITLE_H - 16) / 2, w->icon, 16);
    struct rect br[3];
    int kinds[3];
    int nb = title_buttons(w, br, kinds);
    /* The title in the middle of the caption, as far as the icon and buttons leave room. */
    const struct pfont *f = theme.title_font;
    int left = cap.x + 28, right = br[nb - 1].x - 6;
    char title[64];
    gfx_ellipsize(f, w->title, right - left, title, sizeof(title));
    int tw = gfx_text_width(f, title), tx = cap.x + (cap.w - tw) / 2;
    if (tx + tw > right)
        tx = right - tw;
    if (tx < left)
        tx = left;
    gfx_text(&screen, f, tx, cap.y + (TITLE_H - f->height) / 2, title, text);
    for (int i = 0; i < nb; i++) {
        int pushed = drag == DRAG_TITLE_BUTTON && drag_win == w && drag_button == kinds[i] && drag_pressed;
        draw_title_button(&screen, br[i], kinds[i], pushed, button_disabled(w, kinds[i]));
    }
    gfx_blit(&screen, cl.x, cl.y, &w->content, 0, 0, cl.w, cl.h);
}

/* The sidebar button: an arrow pointing left, to where the sidebar comes from. */
static void draw_sidebar_button(void)
{
    struct rect b = sidebar_button_rect();
    if (rect_empty(b))
        return;
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    if (KINETIK) {
        glass(b, 3, 0);
        gfx_line(&screen, cx + 2, cy - 5, cx - 2, cy, 2, ARGB(255, theme.on_chrome));
        gfx_line(&screen, cx - 2, cy, cx + 2, cy + 5, 2, ARGB(255, theme.on_chrome));
        return;
    }
    gfx_bevel(&screen, b, BEVEL_RAISED);
    for (int i = 0; i < 4; i++)
        gfx_vline(&screen, cx + 1 - i, cy - 3 + i, 7 - 2 * i, C_BLACK);
}

static void draw_taskbar_kinetik(void)
{
    struct rect tb = taskbar_rect();
    color_t c[3];
    chrome_colors(1, c);
    chrome_fill(tb, 0, 0, c);
    gfx_hline(&screen, tb.x, tb.y, tb.w, theme.stroke);
    gfx_hline(&screen, tb.x, tb.y + 1, tb.w, gfx_mix(theme.chrome0, 0xFFFFFF, 72));

    struct rect sb = start_button_rect();
    glass(sb, 6, menu_open);
    struct rect orb = mkrect(sb.x + 7, sb.y + (sb.h - 24) / 2, 24, 24);
    gfx_shadow(&screen, orb, 12, 0, 4, ARGB(0x48, 0x020B19));      /* the orb sits above the button */
    draw_icon(&screen, orb.x, orb.y, ICON_FLUX, 24);
    gfx_text(&screen, &font_archivo_start, sb.x + 37, sb.y + (sb.h - font_archivo_start.height) / 2, "menu",
             theme.on_chrome);

    for (int i = 0; i < ntasks; i++) {
        struct window *w = task_order[i];
        struct rect b = task_button_rect(i);
        int active = (w == focused && !w->minimized) || task_pressed_idx == i;
        glass(b, 3, active);
        if (w->icon_img)
            gfx_image(&screen, b.x + 7, b.y + (b.h - 16) / 2, w->icon_img);
        else
            draw_icon(&screen, b.x + 7, b.y + (b.h - 16) / 2, w->icon, 16);
        char label[64];
        gfx_ellipsize(&font_sans, w->title, b.w - 34, label, sizeof(label));
        gfx_text(&screen, &font_sans, b.x + 29, b.y + (b.h - font_sans.height) / 2, label, theme.on_chrome);
    }

    draw_sidebar_button();
    struct rect tr = tray_rect();
    enum icon_id speaker = sound_tray_icon();
    if (speaker != ICON_NONE) {
        struct rect ir = tray_icon_rect();
        draw_icon(&screen, ir.x, ir.y, speaker, 16);
    }
    if (power_tray_present()) {
        struct rect br = battery_icon_rect();
        draw_icon(&screen, br.x, br.y, power_tray_icon(), 16);
    }
    int cw = gfx_text_width(&font_sans, clock_text);
    gfx_text(&screen, &font_sans, tr.x + tr.w - 9 - cw, tr.y + (tr.h - font_sans.height) / 2, clock_text,
             theme.on_chrome);
}

static void draw_start_menu_kinetik(void)
{
    struct rect m = menu_rect();
    if (rect_overlaps(shadowed(m), screen.clip)) {
        menu_surface(m);
        color_t c[3];
        chrome_colors(1, c);
        chrome_fill(mkrect(m.x + 1, m.y + 1, m.w - 2, KHEAD), 6, 0, c);
        gfx_text(&screen, &font_archivo_large, m.x + 16, m.y + 1 + (KHEAD - font_archivo_large.height) / 2, "fluxOS",
                 theme.on_chrome);
        const struct pfont *f = menu_font();
        for (int i = 0; i < NMENU; i++) {
            struct rect r = menu_item_rect(i);
            if (!menu_items[i].label) {
                gfx_hline(&screen, r.x + 8, r.y + r.h / 2, r.w - 16, RGB(0xBE, 0xCD, 0xDD));
                continue;
            }
            if (i == menu_hover || i == menu_sub)
                menu_hot(r);
            draw_icon(&screen, r.x + 6, r.y + 2, menu_items[i].icon, 32);
            gfx_text(&screen, f, r.x + 6 + 32 + 12, r.y + (r.h - f->height) / 2, menu_items[i].label, theme.ink);
            if (menu_items[i].sub)
                draw_menu_arrow(r.x + r.w - 14, r.y + r.h / 2, theme.ink);
        }
    }
    if (menu_sub >= 0) {
        struct rect sm = sub_rect(menu_sub);
        menu_surface(sm);
        const struct menu_item *it = &menu_items[menu_sub];
        for (int j = 0; j < it->nsub; j++) {
            struct rect r = sub_item_rect(menu_sub, j);
            if (j == sub_hover)
                menu_hot(r);
            draw_icon(&screen, r.x + 4, r.y + (r.h - 16) / 2, it->sub[j].icon, 16);
            gfx_text(&screen, &font_sans, r.x + 4 + 16 + 8, r.y + (r.h - font_sans.height) / 2,
                     it->sub[j].label, theme.ink);
        }
    }
}

/* An item's tick, in its icon's place. */
static void draw_check(struct rect r, color_t c)
{
    int x = r.x + 4, y = r.y + (r.h - 16) / 2;
    gfx_line(&screen, x + 3, y + 8, x + 6, y + 11, 2, ARGB(255, c));
    gfx_line(&screen, x + 6, y + 11, x + 12, y + 4, 2, ARGB(255, c));
}

static void draw_ctx_menu_kinetik(void)
{
    struct rect m = ctx_rect();
    menu_surface(m);
    for (int i = 0; i < ctx_n; i++) {
        struct rect r = ctx_item_rect(i);
        if (!ctx_items[i].label) {
            gfx_hline(&screen, r.x + 6, r.y + r.h / 2, r.w - 12, RGB(0xBE, 0xCD, 0xDD));
            continue;
        }
        if (i == ctx_hover)
            menu_hot(r);
        if (ctx_items[i].checked && ctx_items[i].checked())
            draw_check(r, theme.ink);
        else if (ctx_items[i].icon != ICON_NONE)
            draw_icon(&screen, r.x + 4, r.y + (r.h - 16) / 2, ctx_items[i].icon, 16);
        gfx_text(&screen, &font_sans, r.x + 4 + 16 + 8, r.y + (r.h - font_sans.height) / 2,
                 ctx_items[i].label, theme.ink);
    }
}

static void draw_window(struct window *w)
{
    struct rect r = w->r;
    if (!rect_overlaps(KINETIK ? shadowed(r) : r, screen.clip))
        return;
    struct rect client = wm_client_rect(w);
    if (inside(screen.clip, client)) {   /* only the contents show here: the frame is all outside */
        gfx_blit(&screen, client.x, client.y, &w->content, 0, 0, client.w, client.h);
        return;
    }
    if (KINETIK) {
        draw_window_kinetik(w);
        return;
    }
    struct rect cl = wm_client_rect(w);

    /* Frame face around the client, then the bevel and caption. */
    gfx_fill(&screen, r.x, r.y, r.w, cl.y - r.y, C_FACE);
    gfx_fill(&screen, r.x, cl.y, cl.x - r.x, cl.h, C_FACE);
    gfx_fill(&screen, cl.x + cl.w, cl.y, r.x + r.w - cl.x - cl.w, cl.h, C_FACE);
    gfx_fill(&screen, r.x, cl.y + cl.h, r.w, r.y + r.h - cl.y - cl.h, C_FACE);
    gfx_bevel(&screen, r, BEVEL_WINDOW);

    struct rect cap = caption_rect(w);
    int active = w == focused;
    gfx_fill(&screen, cap.x, cap.y, cap.w, cap.h, active ? C_NAVY : C_SHADOW);
    if (w->icon_img)
        gfx_image(&screen, cap.x + 2, cap.y + 1, w->icon_img);
    else
        draw_icon(&screen, cap.x + 2, cap.y + 1, w->icon, 16);

    struct rect br[3];
    int kinds[3];
    int nb = title_buttons(w, br, kinds);
    char title[64];
    int text_x = cap.x + 21;
    gfx_ellipsize(&font_sans_bold, w->title, br[nb - 1].x - 4 - text_x, title, sizeof(title));
    gfx_text(&screen, &font_sans_bold, text_x, cap.y + 2, title, active ? C_WHITE : C_FACE);

    for (int i = 0; i < nb; i++) {
        int pushed = drag == DRAG_TITLE_BUTTON && drag_win == w && drag_button == kinds[i] && drag_pressed;
        draw_title_button(&screen, br[i], kinds[i], pushed, button_disabled(w, kinds[i]));
    }

    gfx_blit(&screen, cl.x, cl.y, &w->content, 0, 0, cl.w, cl.h);
}

static void draw_taskbar(void)
{
    struct rect tb = taskbar_rect();
    if (!rect_overlaps(tb, screen.clip))
        return;
    if (KINETIK) {
        draw_taskbar_kinetik();
        return;
    }
    gfx_fill(&screen, tb.x, tb.y, tb.w, tb.h, C_FACE);
    gfx_hline(&screen, tb.x, tb.y, tb.w, C_LIGHT);
    gfx_hline(&screen, tb.x, tb.y + 1, tb.w, C_WHITE);

    struct rect sb = start_button_rect();
    int down = menu_open;
    gfx_bevel(&screen, sb, down ? BEVEL_PRESSED : BEVEL_RAISED);
    draw_icon(&screen, sb.x + 4 + down, sb.y + 3 + down, ICON_FLUX, 16);
    gfx_text(&screen, &font_sans_bold, sb.x + 22 + down, sb.y + 4 + down, "menu", C_BLACK);
    if (down)
        gfx_focus_rect(&screen, mkrect(sb.x + 3, sb.y + 3, sb.w - 6, sb.h - 6));

    for (int i = 0; i < ntasks; i++) {
        struct window *w = task_order[i];
        struct rect b = task_button_rect(i);
        int active = (w == focused && !w->minimized) || task_pressed_idx == i;
        if (active) {
            gfx_bevel(&screen, b, BEVEL_PRESSED);
            if (task_pressed_idx != i)
                gfx_dither(&screen, b.x + 2, b.y + 2, b.w - 4, b.h - 4, C_WHITE);
        } else {
            gfx_bevel(&screen, b, BEVEL_RAISED);
        }
        if (w->icon_img)
            gfx_image(&screen, b.x + 4 + active, b.y + 3 + active, w->icon_img);
        else
            draw_icon(&screen, b.x + 4 + active, b.y + 3 + active, w->icon, 16);
        char label[64];
        const struct pfont *f = active ? &font_sans_bold : &font_sans;
        gfx_ellipsize(f, w->title, b.w - 28, label, sizeof(label));
        gfx_text(&screen, f, b.x + 23 + active, b.y + 4 + active, label, C_BLACK);
    }

    draw_sidebar_button();
    struct rect tr = tray_rect();
    gfx_bevel(&screen, tr, BEVEL_THIN_SUNKEN);
    enum icon_id speaker = sound_tray_icon();
    if (speaker != ICON_NONE) {
        struct rect ir = tray_icon_rect();
        draw_icon(&screen, ir.x, ir.y, speaker, 16);
    }
    if (power_tray_present()) {
        struct rect br = battery_icon_rect();
        draw_icon(&screen, br.x, br.y, power_tray_icon(), 16);
    }
    int cw = gfx_text_width(&font_sans, clock_text);
    gfx_text(&screen, &font_sans, tr.x + tr.w - 9 - cw, tr.y + 4, clock_text, C_BLACK);
}

/* A small triangle pointing right, centred vertically on y. */
static void draw_menu_arrow(int x, int y, color_t c)
{
    for (int i = 0; i < 4; i++)
        gfx_vline(&screen, x + i, y - 3 + i, 7 - 2 * i, c);
}

static void draw_start_menu(void)
{
    struct rect m = menu_rect();
    if (!rect_overlaps(shadowed(m), screen.clip) &&
        (menu_sub < 0 || !rect_overlaps(shadowed(sub_rect(menu_sub)), screen.clip)))
        return;
    if (KINETIK) {
        draw_start_menu_kinetik();
        return;
    }
    gfx_fill(&screen, m.x, m.y, m.w, m.h, C_FACE);
    gfx_bevel(&screen, m, BEVEL_WINDOW);

    /* The banner down the left side, reading upwards. */
    int bx = m.x + 3, by = m.y + 3, bh = m.h - 6;
    gfx_fill(&screen, bx, by, BANNER_W, bh, C_SHADOW);
    int y = by + bh - 5;
    gfx_text_up(&screen, &font_banner, bx + 1, y, "flux", C_FACE);
    gfx_text_up(&screen, &font_banner, bx + 1, y - gfx_text_width(&font_banner, "flux"),
                "OS", C_WHITE);

    for (int i = 0; i < NMENU; i++) {
        struct rect r = menu_item_rect(i);
        if (!menu_items[i].label) {
            draw_separator(&screen, r.x + 1, r.y + 3, r.w - 2);
            continue;
        }
        int hot = i == menu_hover || i == menu_sub;
        if (hot)
            gfx_fill(&screen, r.x, r.y, r.w, r.h, C_NAVY);
        draw_icon(&screen, r.x + 4, r.y + 2, menu_items[i].icon, 32);
        gfx_text(&screen, &font_sans, r.x + 4 + 32 + 10, r.y + (r.h - font_sans.height) / 2,
                 menu_items[i].label, hot ? C_WHITE : C_BLACK);
        if (menu_items[i].sub)
            draw_menu_arrow(r.x + r.w - 12, r.y + r.h / 2, hot ? C_WHITE : C_BLACK);
    }
    if (menu_sub >= 0) {
        struct rect sm = sub_rect(menu_sub);
        gfx_fill(&screen, sm.x, sm.y, sm.w, sm.h, C_FACE);
        gfx_bevel(&screen, sm, BEVEL_WINDOW);
        const struct menu_item *it = &menu_items[menu_sub];
        for (int j = 0; j < it->nsub; j++) {
            struct rect r = sub_item_rect(menu_sub, j);
            int hot = j == sub_hover;
            if (hot)
                gfx_fill(&screen, r.x, r.y, r.w, r.h, C_NAVY);
            draw_icon(&screen, r.x + 4, r.y + (r.h - 16) / 2, it->sub[j].icon, 16);
            gfx_text(&screen, &font_sans, r.x + 4 + 16 + 8, r.y + (r.h - font_sans.height) / 2,
                     it->sub[j].label, hot ? C_WHITE : C_BLACK);
        }
    }
}

static void draw_ctx_menu(void)
{
    struct rect m = ctx_rect();
    if (!rect_overlaps(shadowed(m), screen.clip))
        return;
    if (KINETIK) {
        draw_ctx_menu_kinetik();
        return;
    }
    gfx_fill(&screen, m.x, m.y, m.w, m.h, C_FACE);
    gfx_bevel(&screen, m, BEVEL_WINDOW);
    for (int i = 0; i < ctx_n; i++) {
        struct rect r = ctx_item_rect(i);
        if (!ctx_items[i].label) {
            draw_separator(&screen, r.x + 1, r.y + 3, r.w - 2);
            continue;
        }
        int hot = i == ctx_hover;
        if (hot)
            gfx_fill(&screen, r.x, r.y, r.w, r.h, C_NAVY);
        if (ctx_items[i].checked && ctx_items[i].checked())
            draw_check(r, hot ? C_WHITE : C_BLACK);
        else if (ctx_items[i].icon != ICON_NONE)
            draw_icon(&screen, r.x + 4, r.y + (r.h - 16) / 2, ctx_items[i].icon, 16);
        gfx_text(&screen, &font_sans, r.x + 4 + 16 + 8, r.y + (r.h - font_sans.height) / 2,
                 ctx_items[i].label, hot ? C_WHITE : C_BLACK);
    }
}

static void draw_outline(void)
{
    struct rect o = outline;
    gfx_invert_dither(&screen, o.x, o.y, o.w, OUTLINE);
    gfx_invert_dither(&screen, o.x, o.y + o.h - OUTLINE, o.w, OUTLINE);
    gfx_invert_dither(&screen, o.x, o.y + OUTLINE, OUTLINE, o.h - 2 * OUTLINE);
    gfx_invert_dither(&screen, o.x + o.w - OUTLINE, o.y + OUTLINE, OUTLINE, o.h - 2 * OUTLINE);
}

static struct window *modal_window(void)
{
    for (struct window *w = bottom; w; w = w->above)
        if ((w->flags & WF_MODAL) && !w->minimized)
            return w;
    return NULL;
}

/* The topmost window whose contents cover all of r: nothing under it shows there. */
static struct window *covering(struct rect r, const struct window *modal)
{
    struct window *found = NULL;
    for (struct window *w = bottom; w; w = w->above)
        if (!w->minimized && w != modal && inside(r, wm_client_rect(w)))
            found = w;
    return found;
}

static void compose(struct rect r)
{
    gfx_clip(&screen, r);
    if (saver_active()) {
        saver_draw(&screen, r);
        gfx_noclip(&screen);
        return;
    }
    struct window *modal = modal_window();
    /* Draw from the window that hides everything under it here, if one does; the desktop
     * and the windows below it would only be painted over. */
    struct window *from = covering(screen.clip, modal);
    if (!from)
        draw_desktop();
    for (struct window *w = from ? from : bottom; w; w = w->above)
        if (!w->minimized && w != modal)
            draw_window(w);
    if (!setup_mode) {
        sidebar_draw(&screen);
        draw_taskbar();
    }
    if (modal) {
        gfx_dither(&screen, r.x, r.y, r.w, r.h, C_BLACK);
        draw_window(modal);
    }
    if (menu_open)
        draw_start_menu();
    if (ctx_open)
        draw_ctx_menu();
    if (drag == DRAG_RESIZE)
        draw_outline();
    if (!hw_cursor)
        draw_cursor(&screen, mx, my, cursor_kind);
    gfx_noclip(&screen);
}

void wm_flush(void)
{
    if (!ndamage)
        return;
    for (int i = 0; i < ndamage; i++)
        compose(damage[i]);
    show_frame(damage, ndamage);
    ndamage = 0;
}

static void damage_outline(void)
{
    struct rect o = outline;
    wm_damage(mkrect(o.x, o.y, o.w, OUTLINE));
    wm_damage(mkrect(o.x, o.y + o.h - OUTLINE, o.w, OUTLINE));
    wm_damage(mkrect(o.x, o.y, OUTLINE, o.h));
    wm_damage(mkrect(o.x + o.w - OUTLINE, o.y, OUTLINE, o.h));
}

static void damage_taskbar(void)
{
    wm_damage(taskbar_rect());
}

/* ---- windows ---- */

struct window *wm_create(const char *title, enum icon_id icon, int client_w, int client_h,
                         int flags, const struct window_ops *ops, void *priv)
{
    struct window *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    if (surface_init(&w->content, client_w, client_h) < 0) {
        free(w);
        return NULL;
    }
    gfx_fill(&w->content, 0, 0, client_w, client_h, C_FACE);
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->icon = icon;
    w->flags = flags;
    w->ops = ops;
    w->priv = priv;
    w->min_w = 200;
    w->min_h = 120;
    w->r = mkrect(0, 0, client_w + 2 * BORDER, client_h + FRAME_TOP + BORDER);

    struct window **pp = &bottom;
    while (*pp)
        pp = &(*pp)->above;
    *pp = w;
    if (!(flags & WF_NOTASK) && ntasks < (int)(sizeof(task_order) / sizeof(task_order[0])))
        task_order[ntasks++] = w;
    wm_cascade(w);
    wm_focus(w);
    damage_taskbar();
    if (flags & WF_MODAL)
        wm_damage(mkrect(0, 0, screen_w, screen_h));    /* dim everything */
    return w;
}

static void unlink_window(struct window *w)
{
    for (struct window **pp = &bottom; *pp; pp = &(*pp)->above) {
        if (*pp == w) {
            *pp = w->above;
            break;
        }
    }
    w->above = NULL;
}

static struct window *topmost_visible(void)
{
    struct window *top = NULL;
    for (struct window *w = bottom; w; w = w->above)
        if (!w->minimized)
            top = w;
    return top;
}

void wm_destroy(struct window *w)
{
    wm_damage(w->flags & WF_MODAL ? mkrect(0, 0, screen_w, screen_h) : shadowed(w->r));
    unlink_window(w);
    for (int i = 0; i < ntasks; i++) {
        if (task_order[i] == w) {
            memmove(&task_order[i], &task_order[i + 1], (size_t)(ntasks - i - 1) * sizeof(w));
            ntasks--;
            break;
        }
    }
    if (drag_win == w) {
        if (drag == DRAG_RESIZE)
            damage_outline();
        drag = DRAG_NONE;
        drag_win = NULL;
    }
    task_pressed_idx = -1;
    if (focused == w) {
        focused = NULL;
        struct window *next = topmost_visible();
        if (next)
            wm_focus(next);
    }
    damage_taskbar();
    if (w->ops && w->ops->destroy)
        w->ops->destroy(w);
    surface_free(&w->content);
    image_free(w->icon_img);
    free(w);
}

void wm_focus(struct window *w)
{
    struct window *old = focused;
    if (w) {
        w->minimized = 0;
        if (w->above) {                 /* raise */
            unlink_window(w);
            struct window **pp = &bottom;
            while (*pp)
                pp = &(*pp)->above;
            *pp = w;
            wm_damage(shadowed(w->r));
        }
    }
    if (old == w)
        return;
    focused = w;
    if (old) {
        wm_damage(old->r);            /* the border changes colour too */
        if (old->ops && old->ops->focus)
            old->ops->focus(old, 0);
    }
    if (w) {
        wm_damage(shadowed(w->r));
        if (w->ops && w->ops->focus)
            w->ops->focus(w, 1);
    }
    damage_taskbar();
    if (desk_selected >= 0)
        wm_damage(desk_icon_rect(desk_selected));
}

struct window *wm_focused(void)
{
    return focused;
}

static void minimize(struct window *w)
{
    w->minimized = 1;
    wm_damage(shadowed(w->r));
    if (focused == w) {
        focused = NULL;
        if (w->ops && w->ops->focus)
            w->ops->focus(w, 0);
        struct window *next = topmost_visible();
        if (next)
            wm_focus(next);
    }
    damage_taskbar();
}

static void move_window(struct window *w, int x, int y)
{
    wm_damage(shadowed(w->r));
    w->r.x = x;
    w->r.y = y;
    wm_damage(shadowed(w->r));
}

static int set_frame(struct window *w, struct rect r);

void wm_move(struct window *w, int x, int y)
{
    move_window(w, x, y);
}

int wm_resize_client(struct window *w, int client_w, int client_h)
{
    struct rect r = w->r;
    r.w = client_w + 2 * BORDER;
    r.h = client_h + FRAME_TOP + BORDER;
    if (r.x + r.w > work_right())
        r.x = work_right() > r.w ? work_right() - r.w : 0;
    if (r.y + r.h > work_bottom())
        r.y = work_bottom() > r.h ? work_bottom() - r.h : 0;
    return set_frame(w, r);
}

void wm_center(struct window *w)
{
    int x = (work_right() - w->r.w) / 2;
    int y = (work_bottom() - w->r.h) / 2;
    move_window(w, x > 0 ? x : 0, y > 0 ? y : 0);
}

void wm_cascade(struct window *w)
{
    static int slot;
    int x = 96 + (slot % 8) * 26, y = 24 + (slot % 8) * 26;
    slot++;
    if (x + w->r.w > work_right())
        x = work_right() - w->r.w > 0 ? work_right() - w->r.w : 0;
    if (y + w->r.h > work_bottom())
        y = work_bottom() - w->r.h > 0 ? work_bottom() - w->r.h : 0;
    move_window(w, x, y);
}

void wm_set_min_size(struct window *w, int client_w, int client_h)
{
    w->min_w = client_w;
    w->min_h = client_h;
}

/*
 * Give the window the frame r: allocate a client surface of the new size,
 * let the app redraw into it. If memory runs out the window stays as it was.
 */
static int set_frame(struct window *w, struct rect r)
{
    int cw = r.w - 2 * BORDER, ch = r.h - (FRAME_TOP + BORDER);
    if (cw < 1 || ch < 1)
        return -1;
    struct rect old = w->r;
    if (cw != w->content.w || ch != w->content.h) {
        struct surface ns;
        if (surface_init(&ns, cw, ch) < 0)
            return -1;
        gfx_fill(&ns, 0, 0, cw, ch, C_FACE);
        surface_free(&w->content);
        w->content = ns;
        w->r = r;
        if (w->ops && w->ops->resize)
            w->ops->resize(w);
    } else {
        w->r = r;
    }
    wm_damage(shadowed(old));
    wm_damage(shadowed(w->r));
    return 0;
}

static void toggle_maximize(struct window *w)
{
    if (!resizable(w))
        return;
    struct rect to = w->restore;
    if (!w->maximized)
        to = mkrect(0, 0, work_right(), work_bottom());
    struct rect from = w->r;
    if (set_frame(w, to) < 0)
        return;
    if (!w->maximized)
        w->restore = from;
    w->maximized = !w->maximized;
    wm_damage(caption_rect(w));
}

/* ---- the taskbar's menu: minimizing and tiling ---- */

static void minimize_all(void)
{
    struct window *list[sizeof(task_order) / sizeof(task_order[0])];
    int n = ntasks;
    memcpy(list, task_order, (size_t)n * sizeof(list[0]));
    for (int i = 0; i < n; i++)
        if (!list[i]->minimized)
            minimize(list[i]);
}

/* Place the visible windows in a grid of rows x cols cells over the screen
 * above the taskbar, filled a row at a time (by_rows) or a column at a time.
 * A window that cannot change size (no resize handler) is only moved. */
static void tile(int by_rows)
{
    struct window *list[sizeof(task_order) / sizeof(task_order[0])];
    int n = 0;
    for (int i = 0; i < ntasks; i++)
        if (!task_order[i]->minimized && !(task_order[i]->flags & WF_MODAL))
            list[n++] = task_order[i];
    if (!n)
        return;
    /* at most four windows deep, so that none gets too thin */
    int lines = n < 4 ? n : 4, across = (n + lines - 1) / lines;
    int rows = by_rows ? lines : across, cols = by_rows ? across : lines;
    int w = work_right(), h = work_bottom();
    for (int i = 0; i < n; i++) {
        int r = by_rows ? i / cols : i % rows, c = by_rows ? i % cols : i / rows;
        struct rect cell = mkrect(c * w / cols, r * h / rows, (c + 1) * w / cols - c * w / cols,
                                  (r + 1) * h / rows - r * h / rows);
        struct window *win = list[i];
        if (resizable(win)) {
            int min_w = win->min_w + 2 * BORDER, min_h = win->min_h + FRAME_TOP + BORDER;
            if (cell.w < min_w)
                cell.w = min_w;
            if (cell.h < min_h)
                cell.h = min_h;
            if (win->maximized)
                win->maximized = 0;
            if (set_frame(win, cell) == 0)
                continue;
        }
        move_window(win, cell.x, cell.y);
    }
    damage_taskbar();
}

static void tile_horizontally(void)
{
    tile(1);
}

static void tile_vertically(void)
{
    tile(0);
}

/* ---- a sample of the theme, for Display Properties ---- */

static void sample_noop(struct window *w)
{
    (void)w;
}

static const struct window_ops sample_ops = { .resize = sample_noop };

/* A window's client area in the sample: a line of text and a button. */
static void sample_client(struct window *w, const char *text, int button)
{
    struct surface *c = &w->content;
    gfx_fill(c, 0, 0, c->w, c->h, C_FACE);
    gfx_text(c, &font_sans, 8, 8, text, C_BLACK);
    if (button)
        draw_button(c, mkrect(8, c->h - 31, 75, 23), "OK", 0, 1, 0);
}

void wm_draw_sample(struct surface *s, struct rect r)
{
    struct rect cl = rect_intersect(s->clip, r);
    if (rect_empty(cl))
        return;
    /* Draw as if r were the screen: the scene's own code, on a view of s. */
    struct surface saved = screen;
    int saved_w = screen_w, saved_h = screen_h, saved_tasks = ntasks, saved_menu = menu_open;
    int saved_pressed = task_pressed_idx;
    struct window *saved_focus = focused;
    screen.px = s->px + r.y * s->stride + r.x;
    screen.w = screen_w = r.w;
    screen.h = screen_h = r.h;
    screen.stride = s->stride;
    screen.clip = mkrect(cl.x - r.x, cl.y - r.y, cl.w, cl.h);
    ntasks = 0;
    menu_open = 0;
    task_pressed_idx = -1;

    struct window back = { 0 }, front = { 0 };
    int bw = r.w * 11 / 20, fw = r.w * 3 / 5, top = r.h - TASKBAR_H;
    snprintf(back.title, sizeof(back.title), "Inactive window");
    back.icon = ICON_FOLDER;
    back.flags = WF_MINIMIZE | WF_MAXIMIZE;
    back.ops = &sample_ops;
    snprintf(front.title, sizeof(front.title), "Active window");
    front.icon = ICON_DISPLAY;
    front.flags = WF_MINIMIZE | WF_MAXIMIZE;
    front.ops = &sample_ops;
    int bh = top * 11 / 20, fh = top * 3 / 4;
    if (surface_init(&back.content, bw - 2 * BORDER, bh - FRAME_TOP - BORDER) == 0 &&
        surface_init(&front.content, fw - 2 * BORDER, fh - FRAME_TOP - BORDER) == 0) {
        back.r = mkrect(r.w - bw - 10, 10, bw, bh);
        front.r = mkrect(12, top - fh - 8, fw, fh);
        sample_client(&back, "Window text", 0);
        sample_client(&front, "Window text", 1);
        focused = &front;
        draw_window(&back);
        draw_window(&front);
    }
    surface_free(&back.content);
    surface_free(&front.content);
    draw_taskbar();

    screen = saved;
    screen_w = saved_w;
    screen_h = saved_h;
    ntasks = saved_tasks;
    menu_open = saved_menu;
    task_pressed_idx = saved_pressed;
    focused = saved_focus;
}

/* What the frame adds to the client area, across and down, in the theme the frames were made for. */
static int frame_dw, frame_dh;

void wm_restyle(void)
{
    int dw = 2 * BORDER, dh = FRAME_TOP + BORDER;
    sidebar_layout();
    for (struct window *w = bottom; w; w = w->above) {
        int cw = w->content.w, ch = w->content.h;
        if (w->maximized) {             /* still filling the screen above the taskbar */
            w->restore.w += dw - frame_dw;
            w->restore.h += dh - frame_dh;
            set_frame(w, mkrect(0, 0, work_right(), work_bottom()));
        } else {
            w->r.w = cw + dw;
            w->r.h = ch + dh;
        }
        if (w->ops && w->ops->restyle) {
            w->ops->restyle(w);
        } else if (w->ops && w->ops->resize) {
            gfx_fill(&w->content, 0, 0, w->content.w, w->content.h, C_FACE);
            w->ops->resize(w);
        } else if (w->ops && w->ops->focus) {
            w->ops->focus(w, w == focused);
        }
    }
    frame_dw = dw;
    frame_dh = dh;
    /* A taller taskbar may cover icons in the bottom row: lay them out again. */
    for (int i = 0; i < NDESK; i++)
        if (desk_icons[i].y + ICON_CELL_H > screen_h - TASKBAR_H) {
            arrange_icons();
            break;
        }
    wm_damage(mkrect(0, 0, screen_w, screen_h));
}

void wm_set_title(struct window *w, const char *title)
{
    strncpy(w->title, title, sizeof(w->title) - 1);
    wm_damage(caption_rect(w));
    damage_taskbar();
}

void wm_damage_client(struct window *w, struct rect r)
{
    if (w->minimized)
        return;
    struct rect cl = wm_client_rect(w);
    r.x += cl.x;
    r.y += cl.y;
    wm_damage(rect_intersect(r, cl));
}

int wm_window_count(void)
{
    int n = 0;
    for (struct window *w = bottom; w; w = w->above)
        n++;
    return n;
}

static void close_window(struct window *w)
{
    if (w->ops && w->ops->close)
        w->ops->close(w);
    else
        wm_destroy(w);
}

struct window *wm_nth(int i)
{
    struct window *w = bottom;
    while (w && i-- > 0)
        w = w->above;
    return w;
}

void wm_close(struct window *w)
{
    close_window(w);
}

/* ---- the Start menu ---- */

static void set_menu(int open)
{
    if (open == menu_open)
        return;
    struct rect m = menu_rect();
    if (menu_sub >= 0)
        wm_damage(shadowed(sub_rect(menu_sub)));
    menu_open = open;
    menu_hover = menu_sub = sub_hover = -1;
    wm_damage(shadowed(m));
    wm_damage(start_button_rect());
}

static void menu_activate(int i)
{
    set_menu(0);
    if (i >= 0 && i < NMENU && menu_items[i].run)
        menu_items[i].run();
}

static void sub_activate(int p, int j)
{
    void (*run)(void) = menu_items[p].sub[j].run;
    set_menu(0);
    if (run)
        run();
}

static int menu_hit(int x, int y)
{
    for (int i = 0; i < NMENU; i++)
        if (menu_items[i].label && rect_contains(menu_item_rect(i), x, y))
            return i;
    return -1;
}

static int sub_hit(int x, int y)
{
    if (menu_sub < 0)
        return -1;
    for (int j = 0; j < menu_items[menu_sub].nsub; j++)
        if (rect_contains(sub_item_rect(menu_sub, j), x, y))
            return j;
    return -1;
}

/* Is the point on the menu or its open submenu? */
static int menu_contains(int x, int y)
{
    return rect_contains(menu_rect(), x, y) || (menu_sub >= 0 && rect_contains(sub_rect(menu_sub), x, y));
}

static void set_sub_hover(int j)
{
    if (j == sub_hover)
        return;
    if (menu_sub >= 0 && sub_hover >= 0)
        wm_damage(sub_item_rect(menu_sub, sub_hover));
    sub_hover = j;
    if (menu_sub >= 0 && j >= 0)
        wm_damage(sub_item_rect(menu_sub, j));
}

static void set_menu_hover(int i)
{
    if (i == menu_hover)
        return;
    if (menu_hover >= 0)
        wm_damage(menu_item_rect(menu_hover));
    menu_hover = i;
    if (i >= 0)
        wm_damage(menu_item_rect(i));
}

/* Open the submenu of main item p (or close it, for -1). */
static void set_menu_sub(int p)
{
    if (p == menu_sub)
        return;
    if (menu_sub >= 0) {
        wm_damage(shadowed(sub_rect(menu_sub)));
        wm_damage(menu_item_rect(menu_sub));
    }
    menu_sub = p;
    sub_hover = -1;
    if (p >= 0) {
        wm_damage(shadowed(sub_rect(p)));
        wm_damage(menu_item_rect(p));
    }
}

static int tri_side(int ax, int ay, int bx, int by, int px, int py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Is (x, y) inside the triangle from (px, py) to the near edge of the open submenu? Moving
 * diagonally toward the submenu then does not trip over the items in between. */
static int heading_for_sub(int px, int py, int x, int y)
{
    struct rect sm = sub_rect(menu_sub);
    int ax = sm.x, ay = sm.y, bx = sm.x, by = sm.y + sm.h;
    if (x < px || px >= sm.x)
        return 0;
    int d1 = tri_side(px, py, ax, ay, x, y), d2 = tri_side(ax, ay, bx, by, x, y),
        d3 = tri_side(bx, by, px, py, x, y);
    int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

/* The pointer moved over the menu: highlight, and open or close submenus. */
static void menu_pointer(int x, int y)
{
    static int last_x, last_y;
    int px = last_x, py = last_y;
    last_x = x;
    last_y = y;
    int j = sub_hit(x, y);
    if (j >= 0) {
        set_menu_hover(-1);
        set_sub_hover(j);
        return;
    }
    if (menu_sub >= 0 && rect_contains(sub_rect(menu_sub), x, y))
        return;                         /* its border */
    int i = menu_hit(x, y);
    if (menu_sub >= 0 && i != menu_sub && heading_for_sub(px, py, x, y))
        return;                         /* on the way to the submenu */
    if (i >= 0) {
        set_menu_hover(i);
        set_menu_sub(menu_items[i].sub ? i : -1);
    } else if (menu_sub >= 0 && y >= menu_item_rect(menu_sub).y &&
               y < menu_item_rect(menu_sub).y + menu_item_rect(menu_sub).h) {
        return;                         /* the gap between the item and its submenu */
    } else if (rect_contains(menu_rect(), x, y)) {
        set_menu_hover(-1);             /* a separator or the banner */
        set_menu_sub(-1);
    }
}

/* ---- mouse ---- */

static struct window *window_at(int x, int y)
{
    struct window *hit = NULL;
    struct window *modal = modal_window();
    if (modal)
        return rect_contains(modal->r, x, y) ? modal : NULL;
    if (!setup_mode && rect_contains(sidebar_rect(), x, y))
        return NULL;                    /* the sidebar is over them */
    for (struct window *w = bottom; w; w = w->above)
        if (!w->minimized && rect_contains(w->r, x, y))
            hit = w;
    return hit;
}

static void select_icon(int i)
{
    if (desk_selected >= 0)
        wm_damage(desk_icon_rect(desk_selected));
    desk_selected = i;
    if (i >= 0)
        wm_damage(desk_icon_rect(i));
}

static void client_mouse(struct window *w, int buttons, int changed)
{
    if (!w->ops || !w->ops->mouse)
        return;
    struct rect cl = wm_client_rect(w);
    w->ops->mouse(w, mx - cl.x, my - cl.y, buttons, changed);
}

/* Which edges of the frame (E_*) a point grabs, for a window that can be resized. */
static int edge_hit(struct window *w, int x, int y)
{
    struct rect r = w->r;
    if (!resizable(w) || w->maximized || !rect_contains(r, x, y) ||
        rect_contains(caption_rect(w), x, y) || rect_contains(wm_client_rect(w), x, y))
        return 0;
    int e = 0;
    if (x < r.x + BORDER)
        e |= E_L;
    if (x >= r.x + r.w - BORDER)
        e |= E_R;
    if (y < r.y + BORDER)
        e |= E_T;
    if (y >= r.y + r.h - BORDER)
        e |= E_B;
    if (e & (E_T | E_B)) {              /* near a corner: grab both edges */
        if (x < r.x + GRIP)
            e |= E_L;
        if (x >= r.x + r.w - GRIP)
            e |= E_R;
    }
    if (e & (E_L | E_R)) {
        if (y < r.y + GRIP)
            e |= E_T;
        if (y >= r.y + r.h - GRIP)
            e |= E_B;
    }
    return e;
}

/* The frame the pointer's drag asks for, kept on screen and above the minimum size. */
static struct rect resized_frame(void)
{
    struct window *w = drag_win;
    struct rect s = drag_start;
    int dx = mx - drag_mx, dy = my - drag_my;
    int min_w = w->min_w + 2 * BORDER, min_h = w->min_h + FRAME_TOP + BORDER;
    int l = s.x, r = s.x + s.w, t = s.y, b = s.y + s.h;
    if (drag_edges & E_L)
        l = s.x + dx < 0 ? 0 : s.x + dx;
    if (drag_edges & E_R)
        r = s.x + s.w + dx > screen_w ? screen_w : s.x + s.w + dx;
    if (drag_edges & E_T)
        t = s.y + dy < 0 ? 0 : s.y + dy;
    if (drag_edges & E_B)
        b = s.y + s.h + dy > work_bottom() ? work_bottom() : s.y + s.h + dy;
    if (r - l < min_w) {
        if (drag_edges & E_L)
            l = r - min_w;
        else
            r = l + min_w;
    }
    if (b - t < min_h) {
        if (drag_edges & E_T)
            t = b - min_h;
        else
            b = t + min_h;
    }
    return mkrect(l, t, r - l, b - t);
}

static enum cursor_kind kind_for_edges(int e)
{
    if ((e & (E_L | E_T)) == (E_L | E_T) || (e & (E_R | E_B)) == (E_R | E_B))
        return CUR_NWSE;
    if (e & (E_L | E_R)) {
        if (e & (E_T | E_B))
            return CUR_NESW;
        return CUR_EW;
    }
    return e & (E_T | E_B) ? CUR_NS : CUR_ARROW;
}

/* Show the pointer the place under it calls for. */
static void update_cursor(void)
{
    enum cursor_kind k = CUR_ARROW;
    if (drag == DRAG_RESIZE) {
        k = kind_for_edges(drag_edges);
    } else if (drag == DRAG_NONE && !menu_open) {
        struct window *w = window_at(mx, my);
        if (w) {
            k = kind_for_edges(edge_hit(w, mx, my));
            if (k == CUR_ARROW && rect_contains(wm_client_rect(w), mx, my))
                k = w->cursor;          /* what the program in the window asked for */
        }
    }
    if (k == cursor_kind)
        return;
    if (hw_cursor) {
        cursor_kind = k;
        if (load_cursor() < 0)
            software_cursor();
        return;
    }
    wm_damage(cursor_bounds(mx, my, cursor_kind));
    cursor_kind = k;
    wm_damage(cursor_bounds(mx, my, cursor_kind));
}

void wm_set_cursor(struct window *w, enum cursor_kind kind)
{
    if (!w || w->cursor == kind)
        return;
    w->cursor = kind;
    update_cursor();
}

static void press_left(void)
{
    if (ctx_open) {
        if (rect_contains(ctx_rect(), mx, my)) {
            drag = DRAG_CTX;
            set_ctx_hover(ctx_hit(mx, my));
            return;
        }
        set_ctx(NULL, 0, 0, 0, -1);
    }
    if (menu_open) {
        if (menu_contains(mx, my)) {
            drag = DRAG_MENU;
            menu_pointer(mx, my);
            return;
        }
        set_menu(0);
        if (rect_contains(start_button_rect(), mx, my))
            return;                     /* the click closed the menu */
    }

    struct window *modal = modal_window();
    if (rect_contains(taskbar_rect(), mx, my) && !modal) {
        if (rect_contains(start_button_rect(), mx, my)) {
            set_menu(1);
            drag = DRAG_MENU;
            return;
        }
        if (rect_contains(sidebar_button_rect(), mx, my)) {
            sidebar_show(1);
            return;
        }
        if (sound_tray_icon() != ICON_NONE && rect_contains(tray_icon_rect(), mx, my)) {
            volume_open();
            return;
        }
        if (rect_contains(battery_icon_rect(), mx, my)) {
            power_open();
            return;
        }
        if (rect_contains(tray_rect(), mx, my)) {
            clock_open();
            return;
        }
        for (int i = 0; i < ntasks; i++) {
            if (rect_contains(task_button_rect(i), mx, my)) {
                drag = DRAG_TASK;
                task_pressed_idx = i;
                damage_taskbar();
                return;
            }
        }
        return;
    }
    if (!setup_mode && rect_contains(sidebar_rect(), mx, my)) {
        if (!modal)
            sidebar_press(mx, my);
        return;
    }

    struct window *w = window_at(mx, my);
    if (!w) {
        if (modal)
            return;
        int hit = -1;
        for (int i = 0; i < NDESK; i++) {
            struct rect cell = desk_icon_rect(i);
            struct rect icon = mkrect(cell.x + (cell.w - 32) / 2, cell.y + 2, 32, 32);
            int tw = gfx_text_width(&font_sans, desk_icons[i].label);
            struct rect label = mkrect(cell.x + (cell.w - tw) / 2 - 2, cell.y + 37, tw + 4, 15);
            if (rect_contains(icon, mx, my) || rect_contains(label, mx, my))
                hit = i;
        }
        if (focused)
            wm_focus(NULL);
        select_icon(hit);
        if (hit >= 0 && !(hit == last_click_icon && now_ms() - last_click_ms < DBLCLICK_MS)) {
            drag = DRAG_ICON;
            drag_icon = hit;
            icon_moved = 0;
            icon_ox = desk_icons[hit].x;
            icon_oy = desk_icons[hit].y;
            icon_px = mx;
            icon_py = my;
        }
        unsigned int t = now_ms();
        if (hit >= 0 && hit == last_click_icon && t - last_click_ms < DBLCLICK_MS) {
            last_click_icon = -1;
            desk_icons[hit].open();
            return;
        }
        last_click_icon = hit;
        last_click_ms = t;
        return;
    }

    wm_focus(w);
    struct rect br[3];
    int kinds[3];
    int nb = title_buttons(w, br, kinds);
    for (int i = 0; i < nb; i++) {
        if (rect_contains(br[i], mx, my)) {
            drag = DRAG_TITLE_BUTTON;
            drag_win = w;
            drag_button = kinds[i];
            drag_pressed = !button_disabled(w, kinds[i]);
            wm_damage(br[i]);
            return;
        }
    }
    if (rect_contains(caption_rect(w), mx, my)) {
        unsigned int t = now_ms();
        if (resizable(w) && w == last_caption_win && t - last_caption_ms < DBLCLICK_MS) {
            last_caption_win = NULL;
            toggle_maximize(w);         /* a double click on the caption */
            return;
        }
        last_caption_win = w;
        last_caption_ms = t;
        if (w->maximized)
            return;                     /* maximized windows stay put */
        drag = DRAG_MOVE;
        drag_win = w;
        drag_dx = mx - w->r.x;
        drag_dy = my - w->r.y;
        return;
    }
    int edges = edge_hit(w, mx, my);
    if (edges) {
        drag = DRAG_RESIZE;
        drag_win = w;
        drag_edges = edges;
        drag_mx = mx;
        drag_my = my;
        drag_start = w->r;
        outline = w->r;
        damage_outline();
        return;
    }
    if (rect_contains(wm_client_rect(w), mx, my)) {
        drag = DRAG_CLIENT;
        drag_win = w;
        client_mouse(w, mbuttons, MOUSE_LEFT);
    }
}

static void drag_motion(void)
{
    switch (drag) {
    case DRAG_MOVE: {
        int x = mx - drag_dx, y = my - drag_dy;
        /* Keep part of the caption on screen. */
        if (y < 0)
            y = 0;
        if (y > work_bottom() - 10)
            y = work_bottom() - 10;
        if (x + drag_win->r.w < 40)
            x = 40 - drag_win->r.w;
        if (x > screen_w - 40)
            x = screen_w - 40;
        if (x != drag_win->r.x || y != drag_win->r.y)
            move_window(drag_win, x, y);
        break;
    }
    case DRAG_RESIZE: {
        struct rect n = resized_frame();
        if (n.x != outline.x || n.y != outline.y || n.w != outline.w || n.h != outline.h) {
            damage_outline();
            outline = n;
            damage_outline();
        }
        break;
    }
    case DRAG_TITLE_BUTTON: {
        struct rect br[3];
        int kinds[3];
        int nb = title_buttons(drag_win, br, kinds);
        for (int i = 0; i < nb; i++) {
            if (kinds[i] != drag_button)
                continue;
            int over = rect_contains(br[i], mx, my) && !button_disabled(drag_win, drag_button);
            if (over != drag_pressed) {
                drag_pressed = over;
                wm_damage(br[i]);
            }
        }
        break;
    }
    case DRAG_CLIENT:
        client_mouse(drag_win, mbuttons, 0);
        break;
    case DRAG_TASK: {
        int idx = -1;
        for (int i = 0; i < ntasks; i++)
            if (rect_contains(task_button_rect(i), mx, my))
                idx = i;
        if (idx != task_pressed_idx) {
            task_pressed_idx = idx;
            damage_taskbar();
        }
        break;
    }
    case DRAG_MENU:
        menu_pointer(mx, my);
        break;
    case DRAG_CTX:
        set_ctx_hover(ctx_hit(mx, my));
        break;
    case DRAG_ICON: {
        if (!icon_moved && abs(mx - icon_px) < 4 && abs(my - icon_py) < 4)
            break;
        icon_moved = 1;
        int x = icon_ox + mx - icon_px, y = icon_oy + my - icon_py;
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (x > screen_w - ICON_CELL_W)
            x = screen_w - ICON_CELL_W;
        if (y > screen_h - TASKBAR_H - ICON_CELL_H)
            y = screen_h - TASKBAR_H - ICON_CELL_H;
        if (x != desk_icons[drag_icon].x || y != desk_icons[drag_icon].y) {
            wm_damage(desk_icon_rect(drag_icon));
            desk_icons[drag_icon].x = x;
            desk_icons[drag_icon].y = y;
            wm_damage(desk_icon_rect(drag_icon));
        }
        break;
    }
    case DRAG_NONE:
        break;
    }
}

static void release_left(void)
{
    enum drag d = drag;
    struct window *w = drag_win;
    drag = DRAG_NONE;
    drag_win = NULL;

    switch (d) {
    case DRAG_MOVE:
        break;                          /* already where it was dragged */
    case DRAG_RESIZE:
        damage_outline();
        set_frame(w, outline);
        break;
    case DRAG_TITLE_BUTTON:
        wm_damage(caption_rect(w));
        if (!drag_pressed)
            break;
        drag_pressed = 0;
        if (drag_button == TB_CLOSE)
            close_window(w);
        else if (drag_button == TB_MINIMIZE)
            minimize(w);
        else
            toggle_maximize(w);
        break;
    case DRAG_CLIENT:
        client_mouse(w, mbuttons, MOUSE_LEFT);
        break;
    case DRAG_TASK: {
        int i = task_pressed_idx;
        task_pressed_idx = -1;
        damage_taskbar();
        if (i < 0 || i >= ntasks)
            break;
        struct window *t = task_order[i];
        if (t == focused && !t->minimized)
            minimize(t);
        else
            wm_focus(t);
        break;
    }
    case DRAG_MENU: {
        int j = sub_hit(mx, my), i = menu_hit(mx, my);
        if (j >= 0)
            sub_activate(menu_sub, j);
        else if (i >= 0 && menu_items[i].sub)
            set_menu_sub(i);            /* stays open: pick from the submenu */
        else if (i >= 0)
            menu_activate(i);
        else if (!rect_contains(start_button_rect(), mx, my) && !menu_contains(mx, my))
            set_menu(0);
        break;
    }
    case DRAG_CTX: {
        int i = ctx_hit(mx, my);
        void (*run)(void) = i >= 0 ? ctx_items[i].run : NULL;
        if (i >= 0 || !rect_contains(ctx_rect(), mx, my)) {
            int icon = ctx_icon;
            set_ctx(NULL, 0, 0, 0, -1);
            ctx_icon = icon;            /* for open_ctx_icon */
            if (run)
                run();
        }
        break;
    }
    case DRAG_ICON:
        if (icon_moved)
            place_icon(drag_icon);
        break;
    case DRAG_NONE:
        break;
    }
}

static void press_right(void)
{
    if (menu_open)
        set_menu(0);
    struct window *rw = window_at(mx, my);
    if (rw && (rw->flags & WF_RIGHTCLICK) && drag == DRAG_NONE && rect_contains(wm_client_rect(rw), mx, my)) {
        set_ctx(NULL, 0, 0, 0, -1);
        wm_focus(rw);
        client_mouse(rw, mbuttons, MOUSE_RIGHT);
        return;
    }
    if (!modal_window() && drag == DRAG_NONE && rect_contains(taskbar_rect(), mx, my)) {
        set_ctx(taskbar_ctx, COUNT(taskbar_ctx), mx, my, -1);
        return;
    }
    if (!setup_mode && !modal_window() && drag == DRAG_NONE && rect_contains(sidebar_rect(), mx, my)) {
        sidebar_menu(mx, my);
        return;
    }
    if (setup_mode || modal_window() || drag != DRAG_NONE || rect_contains(taskbar_rect(), mx, my) ||
        window_at(mx, my)) {
        set_ctx(NULL, 0, 0, 0, -1);
        return;
    }
    int hit = -1;
    for (int i = 0; i < NDESK; i++) {
        struct rect cell = desk_icon_rect(i);
        if (rect_contains(mkrect(cell.x, cell.y, cell.w, 52), mx, my))
            hit = i;
    }
    if (focused)
        wm_focus(NULL);
    select_icon(hit);
    if (hit >= 0)
        set_ctx(icon_ctx, COUNT(icon_ctx), mx, my, hit);
    else
        set_ctx(desktop_ctx, COUNT(desktop_ctx), mx, my, -1);
}

static void mouse_event(const struct input_event *ev)
{
    int ox = mx, oy = my;
    if (ev->flags & IEV_ABS) {
        mx = ev->x * screen_w / 32768;
        my = ev->y * screen_h / 32768;
    } else {
        mx += ev->x;
        my += ev->y;
    }
    if (mx < 0)
        mx = 0;
    if (my < 0)
        my = 0;
    if (mx >= screen_w)
        mx = screen_w - 1;
    if (my >= screen_h)
        my = screen_h - 1;
    if ((mx != ox || my != oy) && hw_cursor) {
        struct disppos pos = { mx, my };
        if (display(DISP_CURSORPOS, &pos) < 0)
            software_cursor();
    } else if (mx != ox || my != oy) {
        wm_damage(cursor_bounds(ox, oy, cursor_kind));
        wm_damage(cursor_bounds(mx, my, cursor_kind));
    }

    int old = mbuttons;
    mbuttons = ev->mods;
    int pressed = mbuttons & ~old, released = old & ~mbuttons;

    /* The wheel scrolls the window under the pointer. */
    int16_t wheel = (int16_t)ev->code;
    if (wheel && !menu_open && drag == DRAG_NONE && (setup_mode || !rect_contains(sidebar_rect(), mx, my))) {
        struct window *w = window_at(mx, my);
        if (!w)
            w = focused;
        if (w && !w->minimized && w->ops && w->ops->wheel)
            w->ops->wheel(w, wheel);
    }

    if (mx != ox || my != oy) {
        if (drag != DRAG_NONE)
            drag_motion();
        else if (menu_open)
            menu_pointer(mx, my);
        else if (ctx_open)
            set_ctx_hover(ctx_hit(mx, my));
        else if (focused && !focused->minimized && focused->ops && focused->ops->mouse &&
                 rect_contains(wm_client_rect(focused), mx, my))
            client_mouse(focused, mbuttons, 0);     /* hovering: menus highlight */
    }
    if (pressed & MOUSE_RIGHT)
        press_right();
    if (pressed & MOUSE_LEFT)
        press_left();
    if (released & MOUSE_LEFT)
        release_left();
    update_cursor();
}

/* ---- keyboard ---- */

static void focus_next(void)
{
    /* Alt+Tab: bring up the window that has been waiting longest. */
    for (struct window *w = bottom; w; w = w->above) {
        if (w != focused && !(w->flags & WF_NOTASK)) {
            wm_focus(w);
            return;
        }
    }
}

static int menu_step(int from, int dir)
{
    int i = from;
    for (int n = 0; n < NMENU; n++) {
        i = (i + dir + NMENU) % NMENU;
        if (menu_items[i].label)
            return i;
    }
    return from;
}

static void key_event(const struct input_event *ev)
{
    if (ev->code == KEY_POWER) {
        if (ev->flags & IEV_PRESSED)
            system_shutdown();          /* the power button: shut down cleanly */
        return;
    }
    if (ev->code == KEY_SLEEP) {
        if ((ev->flags & IEV_PRESSED) && !setup_mode)
            system_sleep();             /* the sleep button or key */
        return;
    }
    if (ev->code == KEY_VOLUMEUP || ev->code == KEY_VOLUMEDOWN || ev->code == KEY_MUTE) {
        if (ev->flags & IEV_PRESSED)
            sound_key(ev->code);
        return;
    }
    if (!(ev->flags & IEV_PRESSED)) {
        /* Releases only matter to a window that follows keys held down. */
        if (!menu_open && focused && focused->ops && focused->ops->key)
            focused->ops->key(focused, ev);
        return;
    }
    int code = ev->code;
    int ctrl = ev->mods & KMOD_CTRL, alt = ev->mods & KMOD_ALT;

    if (setup_mode && code == KEY_F10 && (ev->mods & KMOD_SHIFT)) {
        terminal_open("Command Prompt", NULL, 0, 0);
        return;
    }
    if (code == KEY_LWIN || code == KEY_RWIN || (ctrl && code == KEY_ESC)) {
        if (!modal_window() && !setup_mode)
            set_menu(!menu_open);
        return;
    }
    if (ctx_open && code == KEY_ESC) {
        set_ctx(NULL, 0, 0, 0, -1);
        return;
    }
    if (menu_open) {
        int n = menu_sub >= 0 ? menu_items[menu_sub].nsub : 0;
        if (code == KEY_ESC) {
            if (sub_hover >= 0 || menu_sub >= 0)
                set_menu_sub(-1);       /* one level back */
            else
                set_menu(0);
        } else if (code == KEY_LEFT) {
            if (menu_sub >= 0) {
                set_menu_hover(menu_sub);
                set_menu_sub(-1);
            }
        } else if (code == KEY_RIGHT) {
            int p = menu_sub >= 0 ? menu_sub : menu_hover;
            if (p >= 0 && menu_items[p].sub) {
                set_menu_sub(p);
                set_menu_hover(-1);
                set_sub_hover(0);
            }
        } else if (code == KEY_UP || code == KEY_DOWN) {
            int dir = code == KEY_UP ? -1 : 1;
            if (sub_hover >= 0)
                set_sub_hover((sub_hover + dir + n) % n);
            else {
                set_menu_sub(-1);
                set_menu_hover(menu_step(menu_hover < 0 ? (dir > 0 ? NMENU - 1 : 0) : menu_hover, dir));
            }
        } else if (code == KEY_ENTER) {
            if (sub_hover >= 0) {
                sub_activate(menu_sub, sub_hover);
            } else if (menu_hover >= 0 && menu_items[menu_hover].sub) {
                set_menu_sub(menu_hover);
                set_menu_hover(-1);
                set_sub_hover(0);
            } else if (menu_hover >= 0) {
                menu_activate(menu_hover);
            }
        }
        return;
    }
    if (alt && code == KEY_F4) {
        if (focused)
            close_window(focused);
        else if (!setup_mode)
            shutdown_open();
        return;
    }
    if (alt && code == KEY_TAB) {
        if (!modal_window())
            focus_next();
        return;
    }
    if (focused && focused->ops && focused->ops->key)
        focused->ops->key(focused, ev);
    else if (!focused && desk_selected >= 0 && code == KEY_ENTER)
        desk_icons[desk_selected].open();
}

void wm_saver(int on)
{
    if (hw_cursor) {
        if (on)
            hide_cursor();
        else if (load_cursor() < 0)
            software_cursor();
    }
    wm_damage(mkrect(0, 0, screen_w, screen_h));
}

void wm_input(const struct input_event *ev)
{
    if (saver_input(ev))
        return;
    if (ev->type == IEV_MOUSE)
        mouse_event(ev);
    else if (ev->type == IEV_KEY)
        key_event(ev);
    else if (ev->type == IEV_POWER && !setup_mode)
        power_event(ev);
}

/* Back from sleep: the screen's memory may have lost what was on it, and
 * the adapter its pointer. */
void wm_resumed(void)
{
    if (hw_cursor && load_cursor() < 0)
        software_cursor();
    wm_damage(mkrect(0, 0, screen_w, screen_h));
}

/* The shutdown screen: Kinetik Blue's wallpaper, whatever the theme, with
 * the logo and the message under it in Archivo, as the startup screen has
 * them (kernel/video/splash.c). */
void wm_splash(const char *message)
{
    gfx_noclip(&screen);
    theme_kinetik_blue(&screen, screen_w, screen_h);
    int text_h = font_archivo_large.height;
    int y = (screen_h - (FLUXOS_LOGO_SIZE + 24 + text_h)) / 2;
    int lx = (screen_w - FLUXOS_LOGO_SIZE) / 2;
    for (int j = 0; j < FLUXOS_LOGO_SIZE; j++) {
        if (y + j < 0 || y + j >= screen_h)
            continue;
        uint32_t *row = screen.px + (y + j) * screen.stride;
        for (int i = 0; i < FLUXOS_LOGO_SIZE; i++) {
            uint32_t p = fluxos_logo[j * FLUXOS_LOGO_SIZE + i];
            int x = lx + i;
            if ((p >> 24) && x >= 0 && x < screen_w)
                row[x] = gfx_mix(row[x], p & 0xFFFFFF, (int)(p >> 24) * 256 / 255);
        }
    }
    y += FLUXOS_LOGO_SIZE + 24;
    gfx_text(&screen, &font_archivo_large, (screen_w - gfx_text_width(&font_archivo_large, message)) / 2, y,
             message, RGB(0xFF, 0xFF, 0xFF));
    if (hw_cursor)
        hide_cursor();
    struct rect all = mkrect(0, 0, screen_w, screen_h);
    show_frame(&all, 1);
}

void wm_tick(void)
{
    if (update_clock() && !setup_mode)
        wm_damage(tray_rect());
    if (!setup_mode)
        sidebar_tick();
}

/* Maximized windows fill the space the sidebar now leaves, and icons it would cover move. */
void wm_work_changed(void)
{
    for (struct window *w = bottom; w; w = w->above)
        if (w->maximized)
            set_frame(w, mkrect(0, 0, work_right(), work_bottom()));
    for (int i = 0; i < NDESK; i++)
        if (desk_icons[i].x + ICON_CELL_W > work_right()) {
            arrange_icons();
            break;
        }
    wm_damage(mkrect(0, 0, screen_w, screen_h));
}

/*
 * After a mode change: map the new screen, give the back buffer its size,
 * and keep every window reachable (maximized ones fill the new screen).
 * On failure (no memory for the back buffer) nothing changes.
 */
int wm_reconfigure(void)
{
    struct fbinfo nfi;
    if (fbinfo(&nfi) < 0)
        return -1;
    uint8_t *nfb = fbmap();
    if (!nfb)
        return -1;
    void *old_mem = screen_mem;
    struct surface ns;
    if (screen_alloc(&ns, (int)nfi.width, (int)nfi.height) < 0)
        return -1;
    unlend_screen();
    free(old_mem);
    screen = ns;
    fb = nfb;
    fbi = nfi;
    screen_w = (int)fbi.width;
    screen_h = (int)fbi.height;
    classify_fb();
    shown_page = 0;                     /* a new mode starts on page 0 */
    use_adapter();
    wallpaper_resize();

    if (drag != DRAG_CLIENT) {
        drag = DRAG_NONE;
        drag_win = NULL;
        task_pressed_idx = -1;
    }
    menu_open = 0;
    ctx_open = 0;
    menu_hover = menu_sub = sub_hover = -1;
    if (mx >= screen_w)
        mx = screen_w - 1;
    if (my >= screen_h)
        my = screen_h - 1;
    sidebar_layout();
    for (struct window *w = bottom; w; w = w->above) {
        if (w->maximized) {
            set_frame(w, mkrect(0, 0, work_right(), work_bottom()));
            continue;
        }
        int x = w->r.x, y = w->r.y;
        if (x + w->r.w > work_right())
            x = work_right() - w->r.w;
        if (y + w->r.h > work_bottom())
            y = work_bottom() - w->r.h;
        w->r.x = x < 0 ? 0 : x;
        w->r.y = y < 0 ? 0 : y;
    }
    for (int i = 0; i < NDESK; i++) {
        if (desk_icons[i].x > work_right() - ICON_CELL_W || desk_icons[i].y > screen_h - TASKBAR_H - ICON_CELL_H) {
            arrange_icons();
            break;
        }
    }
    ndamage = 0;
    wm_damage(mkrect(0, 0, screen_w, screen_h));
    return 0;
}

void wm_init(int setup)
{
    setup_mode = setup;
    theme_init();
    frame_dw = 2 * BORDER;
    frame_dh = FRAME_TOP + BORDER;
    if (fbinfo(&fbi) < 0) {
        perror("desktop: no framebuffer");
        exit(1);
    }
    fb = fbmap();
    if (!fb) {
        perror("desktop: cannot map the framebuffer");
        exit(1);
    }
    screen_w = (int)fbi.width;
    screen_h = (int)fbi.height;
    classify_fb();
    if (screen_alloc(&screen, screen_w, screen_h) < 0) {
        fprintf(stderr, "desktop: out of memory\n");
        exit(1);
    }
    if (setup_mode)
        ndesk = 0;
    else
        sidebar_init();
    arrange_icons();
    mx = screen_w / 2;
    my = screen_h / 2;
    char dir[128], drv[16];
    accel_wanted = grubenv_boot_dir(dir, sizeof(dir)) &&
                   grubenv_get(dir, "fluxos_driver", drv, sizeof(drv)) &&
                   (!strcmp(drv, "radeon") || !strcmp(drv, "intel"));
    use_adapter();
    update_clock();
    wm_damage(mkrect(0, 0, screen_w, screen_h));
}
