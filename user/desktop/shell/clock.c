/*
 * The clock that opens from the taskbar: an analog clock face with a
 * digital readout beside a month calendar that can be paged back and forth.
 * The desktop has no floating point, so the hands come from a table of
 * the sine at every six degrees.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "desktop.h"

#define CLK_W   352
#define CLK_H   214
#define FACE_CX 84
#define FACE_CY 88
#define FACE_R  70
#define CAL_X   178
#define CAL_Y   30
#define COL_W   24
#define ROW_H   20

static struct window *clock_win;
static int view_year, view_month;       /* the month on show; month from 0 */

static const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
    "November", "December",
};
static const char *const day_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

/* sine of 6 * i degrees times 1000, for i in 0..15 */
static const int sine_q[16] = { 0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000 };

static int sin60(int i)
{
    i = ((i % 60) + 60) % 60;
    int q = i / 15, r = i % 15;
    switch (q) {
    case 0: return sine_q[r];
    case 1: return sine_q[15 - r];
    case 2: return -sine_q[r];
    default: return -sine_q[15 - r];
    }
}

static int cos60(int i)
{
    return sin60(i + 15);
}

/* Day of the week of a date (0 is Sunday). */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 2)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m] + d) % 7;
}

static int days_in(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
        return 29;
    return d[m];
}

static void line(struct surface *s, int x0, int y0, int x1, int y1, color_t c)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* A hand of the given length (in pixels) at position i of 60, two pixels wide if thick. */
static void hand(struct surface *s, int i, int len, int thick, color_t c)
{
    int x = FACE_CX + len * sin60(i) / 1000, y = FACE_CY - len * cos60(i) / 1000;
    line(s, FACE_CX, FACE_CY, x, y, c);
    if (thick) {
        line(s, FACE_CX + 1, FACE_CY, x + 1, y, c);
        line(s, FACE_CX, FACE_CY + 1, x, y + 1, c);
    }
}

static void disc(struct surface *s, int cx, int cy, int r, color_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                gfx_pixel(s, cx + x, cy + y, c);
}

static struct rect prev_rect(void)
{
    return mkrect(CAL_X, 4, 22, 20);
}

static struct rect next_rect(void)
{
    return mkrect(CAL_X + 7 * COL_W - 22, 4, 22, 20);
}

static void arrow(struct surface *s, struct rect r, int right)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int i = 0; i < 4; i++)
        gfx_vline(s, right ? cx - 2 + i : cx + 1 - i, cy - 3 + i, 7 - 2 * i, C_BLACK);
}

static void paint(void)
{
    struct surface *s = &clock_win->content;
    int w = s->w, h = s->h;
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    gfx_noclip(s);
    gfx_fill(s, 0, 0, w, h, C_FACE);

    /* The clock face. */
    disc(s, FACE_CX, FACE_CY, FACE_R + 3, C_SHADOW);
    disc(s, FACE_CX, FACE_CY, FACE_R, C_WHITE);
    for (int i = 0; i < 60; i++) {
        int r0 = i % 5 ? FACE_R - 4 : FACE_R - 9;
        int x0 = FACE_CX + r0 * sin60(i) / 1000, y0 = FACE_CY - r0 * cos60(i) / 1000;
        int x1 = FACE_CX + (FACE_R - 2) * sin60(i) / 1000, y1 = FACE_CY - (FACE_R - 2) * cos60(i) / 1000;
        line(s, x0, y0, x1, y1, i % 5 ? C_SHADOW : C_BLACK);
    }
    hand(s, (tm.tm_hour % 12) * 5 + tm.tm_min / 12, FACE_R - 24, 1, C_BLACK);
    hand(s, tm.tm_min, FACE_R - 10, 1, C_NAVY);
    hand(s, tm.tm_sec, FACE_R - 6, 0, RGB(0xC0, 0x00, 0x00));
    disc(s, FACE_CX, FACE_CY, 3, C_BLACK);

    /* Digital time and date under it. */
    char buf[64];
    int h12 = tm.tm_hour % 12;
    snprintf(buf, sizeof(buf), "%d:%02d:%02d %s", h12 ? h12 : 12, tm.tm_min, tm.tm_sec, tm.tm_hour < 12 ? "AM" : "PM");
    gfx_text(s, &font_sans, FACE_CX - gfx_text_width(&font_sans, buf) / 2, 168, buf, C_BLACK);
    snprintf(buf, sizeof(buf), "%s, %s %d", day_names[tm.tm_wday % 7], month_names[tm.tm_mon % 12], tm.tm_mday);
    gfx_text(s, &font_sans, FACE_CX - gfx_text_width(&font_sans, buf) / 2, 186, buf, C_BLACK);

    /* The calendar. */
    gfx_bevel(s, mkrect(CAL_X - 4, 2, 7 * COL_W + 8, h - 6), BEVEL_SUNKEN);
    gfx_fill(s, CAL_X - 2, 4, 7 * COL_W + 4, h - 10, C_WHITE);
    draw_button(s, prev_rect(), "", 0, 0, 0);
    draw_button(s, next_rect(), "", 0, 0, 0);
    arrow(s, prev_rect(), 0);
    arrow(s, next_rect(), 1);
    snprintf(buf, sizeof(buf), "%s %d", month_names[view_month], view_year);
    gfx_text(s, &font_sans, CAL_X + 7 * COL_W / 2 - gfx_text_width(&font_sans, buf) / 2, 8, buf, C_NAVY);
    static const char *const dow[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    for (int c = 0; c < 7; c++)
        gfx_text(s, &font_sans, CAL_X + c * COL_W + (COL_W - gfx_text_width(&font_sans, dow[c])) / 2, CAL_Y, dow[c],
                 C_SHADOW);
    gfx_hline(s, CAL_X, CAL_Y + 16, 7 * COL_W, C_SHADOW);
    int first = weekday(view_year, view_month, 1), n = days_in(view_year, view_month);
    for (int d = 1; d <= n; d++) {
        int slot = first + d - 1, col = slot % 7, row = slot / 7;
        int x = CAL_X + col * COL_W, y = CAL_Y + 20 + row * ROW_H;
        int today = d == tm.tm_mday && view_month == tm.tm_mon && view_year == tm.tm_year + 1900;
        char num[4];
        snprintf(num, sizeof(num), "%d", d);
        if (today)
            gfx_fill(s, x + 1, y, COL_W - 2, ROW_H - 2, C_NAVY);
        gfx_text(s, &font_sans, x + (COL_W - gfx_text_width(&font_sans, num)) / 2, y + 2, num,
                 today ? C_WHITE : col == 0 ? RGB(0xC0, 0x00, 0x00) : C_BLACK);
    }
    wm_damage_client(clock_win, mkrect(0, 0, w, h));
}

static void show_month(int delta)
{
    view_month += delta;
    if (view_month < 0) {
        view_month = 11;
        view_year--;
    } else if (view_month > 11) {
        view_month = 0;
        view_year++;
    }
    paint();
}

static void clock_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    (void)w;
    if (!((changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT)))
        return;
    if (rect_contains(prev_rect(), x, y))
        show_month(-1);
    else if (rect_contains(next_rect(), x, y))
        show_month(1);
}

static void clock_key(struct window *w, const struct input_event *ev)
{
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (ev->code == KEY_ESC)
        wm_close(w);
    else if (ev->code == KEY_LEFT || ev->code == KEY_PGUP)
        show_month(-1);
    else if (ev->code == KEY_RIGHT || ev->code == KEY_PGDN)
        show_month(1);
}

static void clock_destroy(struct window *w)
{
    (void)w;
    clock_win = NULL;
}

static void clock_restyle(struct window *w)
{
    (void)w;
    paint();
}

static const struct window_ops clock_ops = {
    .restyle = clock_restyle,
    .key = clock_key,
    .mouse = clock_mouse,
    .destroy = clock_destroy,
};

void clock_tick(void)
{
    if (!clock_win || clock_win->minimized)
        return;
    paint();
}

void clock_open(void)
{
    if (clock_win) {                    /* clicking the clock again puts it away */
        wm_close(clock_win);
        return;
    }
    clock_win = wm_create("Date and Time", ICON_CLOCK, CLK_W, CLK_H, WF_NOTASK, &clock_ops, NULL);
    if (!clock_win)
        return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    view_year = tm->tm_year + 1900;
    view_month = tm->tm_mon;
    wm_move(clock_win, wm_work_right() - clock_win->r.w - 2, screen_h - TASKBAR_H - clock_win->r.h - 2);
    paint();
}

/* At startup: the saved time zone (fluxos_tz, minutes east of UTC), which Date and Time sets. */
void datetime_init(void)
{
    char dir[128], value[16];
    if (grubenv_boot_dir(dir, sizeof(dir)) && grubenv_get(dir, "fluxos_tz", value, sizeof(value)) > 0)
        tz_set(atoi(value));
}
