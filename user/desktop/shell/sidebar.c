/*
 * The sidebar: a column of widgets down the right edge of the screen, from
 * the top to the taskbar, as Longhorn had. Like the taskbar it is always in
 * view: it is drawn over windows, and maximized ones stop at its edge.
 *
 * It follows the Kinetik sidebar studies: a satin body in the theme's color
 * under a heading, cards with a title strip of the captions' chrome, and
 * "Add widgets..." at the foot. Under Classic it takes the gray faces and
 * navy title strips of Windows 95 instead.
 *
 * The widgets are an analog clock (clicking it opens Date and Time), the
 * month's calendar (paged with its arrows; clicking the month comes back
 * to today's) and a graph of the last minute of processor and memory use
 * (clicking it opens Task Manager). Each can be added or taken away from
 * the menu its heading, its foot and the right button open; a card that
 * does not fit on the screen is left out.
 *
 * Everything is drawn into a surface of its own, over a copy of the bare
 * body, and copied to the screen when composing: each second only the
 * clock's and the graph's cards are drawn again.
 *
 * The choice is kept in GRUB's environment block when the system runs from
 * a hard disk (fluxos_sidebar: "on" or "off", then the widgets shown).
 */
#include <flux.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "desktop.h"

#define HEAD_H  40              /* the heading */
#define FOOT_H  30              /* "Add widgets..." */
#define MARGIN  10              /* from the sidebar's sides to its cards */
#define GAP     10              /* between cards */
#define CAP_H   20              /* a card's title strip */
#define FACE_R  54              /* the clock's face */
#define CAL_ROW 20              /* a week of the calendar */
#define GRAPH_H 56
#define HIST    60              /* seconds the graph shows */

enum { W_CLOCK, W_CALENDAR, W_SYSTEM, NWIDGETS };
static const char *const widget_keys[NWIDGETS] = { "clock", "calendar", "system" };

static int shown;
static int enabled[NWIDGETS] = { 1, 1, 1 };
static struct surface bg, cache;        /* the bare body; everything on it */
static int cache_ok;
static struct rect card[NWIDGETS];      /* sidebar coordinates; empty when not shown */
static struct rect r_menu, r_add, r_prev, r_next, r_month;
static int view_year, view_month, last_mday = -1;

/* What the graph shows. */
static uint8_t cpu_hist[HIST], mem_hist[HIST];
static int cpu_now;
static uint32_t mem_used_kb, mem_total_kb;
static uint32_t prev_idle, prev_ms;
static int sampled;

static const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
    "November", "December",
};
static const char *const day_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

/* ---- the calendar's arithmetic, and the clock's ---- */

/* Day of the week of a date (0 is Sunday); month from 0. */
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

/* ---- geometry ---- */

static int full_width(void)
{
    return screen_w >= 1280 ? 248 : 224;
}

int sidebar_width(void)
{
    return shown && screen_w >= 640 ? full_width() : 0;
}

int sidebar_shown(void)
{
    return shown;
}

/* Hidden by choice, on a screen it would fit: the taskbar offers a button to bring it back. */
int sidebar_collapsed(void)
{
    return !shown && screen_w >= 640;
}

struct rect sidebar_rect(void)
{
    int w = sidebar_width();
    return mkrect(screen_w - w, 0, w, w ? screen_h - TASKBAR_H : 0);
}

static int card_height(int i)
{
    switch (i) {
    case W_CLOCK: return CAP_H + 8 + 2 * FACE_R + 10 + 18 + 16 + 8;
    case W_CALENDAR: return CAP_H + 6 + 20 + 4 + 16 + 6 * CAL_ROW + 6;
    default: return CAP_H + 8 + GRAPH_H + 8 + 2 * (16 + 4 + 6) + 4;
    }
}

/* Where each card goes: down from the heading, as many as fit above the foot. */
static void place_cards(int w, int h)
{
    int y = HEAD_H + GAP, limit = h - FOOT_H - GAP / 2;
    for (int i = 0; i < NWIDGETS; i++) {
        card[i] = mkrect(0, 0, 0, 0);
        if (!enabled[i] || y + card_height(i) > limit)
            continue;
        card[i] = mkrect(MARGIN, y, w - 2 * MARGIN, card_height(i));
        y += card_height(i) + GAP;
    }
    r_menu = mkrect(w - 34, (HEAD_H - 24) / 2, 24, 24);
    r_add = mkrect(MARGIN - 4, h - FOOT_H + 4, gfx_text_width(&font_sans, "Add widgets...") + 12, FOOT_H - 8);
    struct rect c = card[W_CALENDAR];
    int top = c.y + CAP_H + 6;
    r_prev = mkrect(c.x + 4, top, 22, 20);
    r_next = mkrect(c.x + c.w - 26, top, 22, 20);
    r_month = mkrect(c.x + 30, top, c.w - 60, 20);
}

/* ---- colors ---- */

/* Kinetik's body runs from the captions' middle color down to the darkest of the
 * wallpaper (White's is pale throughout, with dark text). */
static int light_body(void)
{
    return theme.accent == ACCENT_WHITE;
}

static color_t ink(void)
{
    return KINETIK ? theme.on_chrome : C_BLACK;
}

static color_t muted(void)
{
    if (!KINETIK)
        return C_SHADOW;
    return light_body() ? RGB(0x53, 0x68, 0x7E) : gfx_mix(theme.pale, 0xFFFFFF, 110);
}

/* The graph, the bars and the second hand: the theme's own color where it shows up. */
static color_t hot(void)
{
    if (!KINETIK)
        return RGB(0xC0, 0x00, 0x00);
    if (theme.accent == ACCENT_WHITE)
        return RGB(0x2E, 0x6F, 0xB7);
    if (theme.accent == ACCENT_BLACK)
        return theme.pale;
    return theme.glow;
}

static void chrome_fill(struct surface *s, struct rect r, int rt, int rb)
{
    static const int pos[3] = { 0, 115, 256 };
    color_t c[3] = { ARGB(255, theme.chrome0), ARGB(255, theme.chrome1), ARGB(255, theme.chrome2) };
    gfx_gradient(s, r, rt, rb, 3, c, pos);
}

/* A blended line or box (gfx_hline does not blend). */
static void tint(struct surface *s, struct rect r, color_t argb)
{
    gfx_round_fill(s, r, 0, 0, argb);
}

/* ---- the body ---- */

static void paint_body(struct surface *s, int w, int h)
{
    gfx_noclip(s);
    if (!KINETIK) {
        gfx_fill(s, 0, 0, w, h, C_FACE);
        gfx_vline(s, 0, 0, h, C_LIGHT);
        gfx_vline(s, 1, 0, h, C_WHITE);
        gfx_fill(s, 2, 2, w - 4, HEAD_H - 4, C_NAVY);
        gfx_text(s, &font_sans_bold, 10, (HEAD_H - font_sans_bold.height) / 2, "Sidebar", C_WHITE);
        draw_button(s, r_menu, "", 0, 0, 0);
        int cx = r_menu.x + r_menu.w / 2, cy = r_menu.y + r_menu.h / 2 - 1;
        for (int i = 0; i < 4; i++)
            gfx_hline(s, cx - 3 + i, cy + i, 7 - 2 * i, C_BLACK);
        draw_separator(s, 6, h - FOOT_H, w - 12);
    } else {
        static const int pos[2] = { 0, 256 };
        color_t body[2] = {
            ARGB(255, light_body() ? RGB(0xCB, 0xD6, 0xE3) : theme.chrome1),
            ARGB(255, light_body() ? RGB(0xA6, 0xB5, 0xC7) : theme.wall0),
        };
        gfx_gradient(s, mkrect(0, 0, w, h), 0, 0, 2, body, pos);
        for (int i = 0; i < 6; i++)     /* light along the edge it shows the desktop */
            tint(s, mkrect(1 + i, 0, 1, h), ARGB(46 * (6 - i) / 6, 0xFFFFFF));
        chrome_fill(s, mkrect(0, 0, w, HEAD_H), 0, 0);
        tint(s, mkrect(0, 1, w, 1), ARGB(64, 0xFFFFFF));
        tint(s, mkrect(0, HEAD_H - 1, w, 1), ARGB(255, theme.stroke));
        gfx_text(s, &font_archivo_title, 16, (HEAD_H - font_archivo_title.height) / 2, "Sidebar", ink());
        /* The menu button, in the chrome's satin, with a chevron. */
        chrome_fill(s, r_menu, 4, 4);
        gfx_round_frame(s, r_menu, 4, 4, ARGB(110, theme.stroke));
        int cx = r_menu.x + r_menu.w / 2, cy = r_menu.y + r_menu.h / 2;
        gfx_line(s, cx - 5, cy - 2, cx, cy + 3, 2, ARGB(255, ink()));
        gfx_line(s, cx, cy + 3, cx + 5, cy - 2, 2, ARGB(255, ink()));
        tint(s, mkrect(MARGIN, h - FOOT_H, w - 2 * MARGIN, 1), ARGB(72, theme.pale));
        tint(s, mkrect(0, 0, 1, h), ARGB(255, theme.stroke));      /* where it meets the desktop */
    }
    int ty = h - FOOT_H + (FOOT_H - font_sans.height) / 2;
    gfx_text(s, &font_sans, MARGIN + 2, ty, "Add widgets...", ink());
    gfx_text(s, &font_sans, w - MARGIN - gfx_text_width(&font_sans, "fluxOS"), ty, "fluxOS", muted());
}

/* A card's frame and title strip; what is in it is drawn after. */
static void paint_card(struct surface *s, struct rect r, const char *title, const char *right)
{
    if (!KINETIK) {
        gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
        gfx_bevel(s, r, BEVEL_THIN_RAISED);
        gfx_fill(s, r.x + 2, r.y + 2, r.w - 4, CAP_H - 3, C_NAVY);
        gfx_text(s, &font_sans, r.x + 8, r.y + (CAP_H - font_sans.height) / 2, title, C_WHITE);
        if (right[0])
            gfx_text(s, &font_sans, r.x + r.w - 8 - gfx_text_width(&font_sans, right),
                     r.y + (CAP_H - font_sans.height) / 2, right, C_WHITE);
        return;
    }
    static const int pos[2] = { 0, 256 };
    color_t fill[2] = {
        ARGB(light_body() ? 66 : 18, 0xFFFFFF),
        ARGB(light_body() ? 38 : 26, light_body() ? 0xFFFFFF : theme.wall0),
    };
    gfx_shadow(s, r, 5, 2, 3, ARGB(26, 0x000714));
    gfx_gradient(s, r, 5, 5, 2, fill, pos);
    gfx_round_frame(s, r, 5, 5, ARGB(46, theme.pale));
    struct rect strip = mkrect(r.x + 1, r.y + 1, r.w - 2, CAP_H);
    chrome_fill(s, strip, 4, 0);
    tint(s, mkrect(strip.x + 6, strip.y + 1, strip.w - 12, 1), ARGB(64, 0xFFFFFF));
    gfx_text(s, &font_sans, r.x + 10, r.y + (CAP_H - font_sans.height) / 2 + 1, title, ink());
    if (right[0])
        gfx_text(s, &font_sans, r.x + r.w - 10 - gfx_text_width(&font_sans, right),
                 r.y + (CAP_H - font_sans.height) / 2 + 1, right, muted());
}

/* ---- the widgets ---- */

static void disc(struct surface *s, int cx, int cy, int r, color_t argb)
{
    gfx_round_fill(s, mkrect(cx - r, cy - r, 2 * r + 1, 2 * r + 1), r, r, argb);
}

/* A hand at position i of 60, len pixels long. */
static void hand(struct surface *s, int cx, int cy, int i, int len, int width, color_t argb)
{
    gfx_line(s, cx, cy, cx + len * sin60(i) / 1000, cy - len * cos60(i) / 1000, width, argb);
}

static void paint_clock(struct surface *s, struct rect r, const struct tm *tm)
{
    paint_card(s, r, "local time", "");
    int cx = r.x + r.w / 2, cy = r.y + CAP_H + 8 + FACE_R;
    color_t face_ink = KINETIK ? ink() : C_BLACK, sec = hot();
    if (KINETIK) {
        disc(s, cx, cy, FACE_R, ARGB(light_body() ? 90 : 30, 0xFFFFFF));
        gfx_round_frame(s, mkrect(cx - FACE_R, cy - FACE_R, 2 * FACE_R + 1, 2 * FACE_R + 1), FACE_R, FACE_R,
                        ARGB(120, theme.pale));
    } else {
        disc(s, cx, cy, FACE_R + 2, ARGB(255, C_SHADOW));
        disc(s, cx, cy, FACE_R, ARGB(255, C_WHITE));
    }
    for (int i = 0; i < 60; i++) {
        int hour = i % 5 == 0, r0 = FACE_R - (hour ? 9 : 4), r1 = FACE_R - 3;
        gfx_line(s, cx + r0 * sin60(i) / 1000, cy - r0 * cos60(i) / 1000, cx + r1 * sin60(i) / 1000,
                 cy - r1 * cos60(i) / 1000, hour ? 2 : 1, ARGB(hour ? 230 : 110, face_ink));
    }
    hand(s, cx, cy, (tm->tm_hour % 12) * 5 + tm->tm_min / 12, FACE_R - 24, 3, ARGB(255, face_ink));
    hand(s, cx, cy, tm->tm_min, FACE_R - 11, 2, ARGB(255, KINETIK ? face_ink : C_NAVY));
    hand(s, cx, cy, tm->tm_sec, FACE_R - 6, 1, ARGB(255, sec));
    disc(s, cx, cy, 4, ARGB(255, face_ink));
    disc(s, cx, cy, 2, ARGB(255, sec));

    char buf[48];
    int h12 = tm->tm_hour % 12, y = cy + FACE_R + 10;
    const struct pfont *big = KINETIK ? &font_archivo_title : &font_sans_bold;
    snprintf(buf, sizeof(buf), "%d:%02d %s", h12 ? h12 : 12, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
    gfx_text(s, big, cx - gfx_text_width(big, buf) / 2, y, buf, ink());
    snprintf(buf, sizeof(buf), "%s, %s %d", day_names[tm->tm_wday % 7], month_names[tm->tm_mon % 12],
             tm->tm_mday);
    gfx_text(s, &font_sans, cx - gfx_text_width(&font_sans, buf) / 2, y + 18, buf, muted());
}

static void arrow(struct surface *s, struct rect r, int right, color_t c)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (KINETIK) {
        gfx_line(s, right ? cx - 2 : cx + 2, cy - 4, right ? cx + 2 : cx - 2, cy, 2, ARGB(255, c));
        gfx_line(s, right ? cx + 2 : cx - 2, cy, right ? cx - 2 : cx + 2, cy + 4, 2, ARGB(255, c));
        return;
    }
    for (int i = 0; i < 4; i++)
        gfx_vline(s, right ? cx - 2 + i : cx + 1 - i, cy - 3 + i, 7 - 2 * i, c);
}

/* A month from Monday to Sunday, with the days around it in the weeks it takes. */
static void paint_calendar(struct surface *s, struct rect r, const struct tm *tm)
{
    paint_card(s, r, "calendar", "");
    char buf[32];
    arrow(s, r_prev, 0, muted());
    arrow(s, r_next, 1, muted());
    snprintf(buf, sizeof(buf), "%s %d", month_names[view_month], view_year);
    gfx_text(s, &font_sans_bold, r_month.x + (r_month.w - gfx_text_width(&font_sans_bold, buf)) / 2,
             r_month.y + (r_month.h - font_sans_bold.height) / 2, buf, KINETIK ? ink() : C_NAVY);

    int cw = (r.w - 12) / 7, x0 = r.x + 6 + (r.w - 12 - 7 * cw) / 2, y = r_month.y + 24;
    static const char *const dow[7] = { "M", "T", "W", "T", "F", "S", "S" };
    for (int c = 0; c < 7; c++)
        gfx_text(s, &font_sans_bold, x0 + c * cw + (cw - gfx_text_width(&font_sans_bold, dow[c])) / 2, y, dow[c],
                 muted());
    y += 16;
    int first = (weekday(view_year, view_month, 1) + 6) % 7, n = days_in(view_year, view_month);
    int py = view_month ? view_year : view_year - 1, pm = view_month ? view_month - 1 : 11;
    int before = days_in(py, pm);
    for (int k = 0; k < 42; k++) {
        int d = k - first + 1, here = d >= 1 && d <= n;
        int shown_day = d < 1 ? before + d : d > n ? d - n : d;
        int today = here && d == tm->tm_mday && view_month == tm->tm_mon && view_year == tm->tm_year + 1900;
        struct rect cell = mkrect(x0 + (k % 7) * cw + 1, y + (k / 7) * CAL_ROW, cw - 2, CAL_ROW - 2);
        if (today && KINETIK) {
            chrome_fill(s, cell, 5, 5);
            gfx_round_frame(s, cell, 5, 5, ARGB(115, theme.pale));
        } else if (today) {
            gfx_fill(s, cell.x, cell.y, cell.w, cell.h, C_NAVY);
        }
        snprintf(buf, sizeof(buf), "%d", shown_day);
        const struct pfont *f = today ? &font_sans_bold : &font_sans;
        color_t c = today && !KINETIK ? C_WHITE : here ? ink() : KINETIK ? gfx_mix(muted(), theme.wall0, 90)
                                                                         : C_SHADOW;
        gfx_text(s, f, cell.x + (cell.w - gfx_text_width(f, buf)) / 2, cell.y + (cell.h - f->height) / 2, buf, c);
    }
}

/* The value at column x of w of a history running left (oldest) to right. */
static int hist_at(const uint8_t *h, int x, int w)
{
    return h[w > 1 ? x * (HIST - 1) / (w - 1) : HIST - 1];
}

/* A bar of pct percent along r. */
static void bar(struct surface *s, struct rect r, int pct)
{
    int fw = r.w * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100;
    if (!KINETIK) {
        gfx_bevel(s, mkrect(r.x - 1, r.y - 1, r.w + 2, r.h + 2), BEVEL_THIN_SUNKEN);
        gfx_fill(s, r.x, r.y, r.w, r.h, C_WHITE);
        gfx_fill(s, r.x, r.y, fw, r.h, C_NAVY);
        return;
    }
    gfx_round_fill(s, r, 2, 2, ARGB(140, light_body() ? 0x7B8CA0 : theme.wall0));
    if (fw > 0)
        gfx_round_fill(s, mkrect(r.x, r.y, fw, r.h), 2, 2, ARGB(255, hot()));
}

static void paint_system(struct surface *s, struct rect r)
{
    char right[24], buf[48];
    snprintf(right, sizeof(right), "cpu %d%%", cpu_now);
    paint_card(s, r, "system", right);

    struct rect g = mkrect(r.x + 8, r.y + CAP_H + 8, r.w - 16, GRAPH_H);
    color_t cpu_c = hot(), mem_c = KINETIK ? (light_body() ? theme.ink : theme.pale) : RGB(0xFF, 0xFF, 0x00);
    if (KINETIK) {
        gfx_round_fill(s, g, 3, 3, ARGB(light_body() ? 60 : 90, light_body() ? 0x7B8CA0 : theme.wall0));
        for (int i = 1; i < 4; i++)
            tint(s, mkrect(g.x + 1, g.y + i * g.h / 4, g.w - 2, 1), ARGB(26, theme.pale));
    } else {
        cpu_c = RGB(0x00, 0xFF, 0x00);
        gfx_bevel(s, mkrect(g.x - 1, g.y - 1, g.w + 2, g.h + 2), BEVEL_THIN_SUNKEN);
        gfx_fill(s, g.x, g.y, g.w, g.h, C_BLACK);
        for (int i = 1; i < 4; i++)
            gfx_hline(s, g.x, g.y + i * g.h / 4, g.w, RGB(0x00, 0x60, 0x30));
        for (int x = g.w - 1; x > 0; x -= 12)
            gfx_vline(s, g.x + x, g.y, g.h, RGB(0x00, 0x60, 0x30));
    }
    /* The processor filled under its line, memory as a line over it. */
    int bottom = g.y + g.h - 2, span = g.h - 4;
    for (int x = 0; x < g.w - 2; x++) {
        int v = hist_at(cpu_hist, x, g.w - 2) * span / 100;
        if (v > 0)
            tint(s, mkrect(g.x + 1 + x, bottom - v, 1, v), ARGB(KINETIK ? 70 : 90, cpu_c));
    }
    int step = (g.w - 2) / 30 > 1 ? (g.w - 2) / 30 : 1;
    for (int x = 0; x + step < g.w - 2; x += step) {
        int x1 = x + step > g.w - 3 ? g.w - 3 : x + step;
        gfx_line(s, g.x + 1 + x, bottom - hist_at(mem_hist, x, g.w - 2) * span / 100, g.x + 1 + x1,
                 bottom - hist_at(mem_hist, x1, g.w - 2) * span / 100, 1, ARGB(200, mem_c));
        gfx_line(s, g.x + 1 + x, bottom - hist_at(cpu_hist, x, g.w - 2) * span / 100, g.x + 1 + x1,
                 bottom - hist_at(cpu_hist, x1, g.w - 2) * span / 100, 2, ARGB(255, cpu_c));
    }

    int y = g.y + g.h + 8;
    snprintf(buf, sizeof(buf), "%d%%", cpu_now);
    gfx_text(s, &font_sans, r.x + 10, y, "Processor", muted());
    gfx_text(s, &font_sans, r.x + r.w - 10 - gfx_text_width(&font_sans, buf), y, buf, ink());
    bar(s, mkrect(r.x + 10, y + 18, r.w - 20, 4), cpu_now);
    y += 26;
    snprintf(buf, sizeof(buf), "%u / %u MB", (unsigned)(mem_used_kb / 1024), (unsigned)(mem_total_kb / 1024));
    gfx_text(s, &font_sans, r.x + 10, y, "Memory", muted());
    gfx_text(s, &font_sans, r.x + r.w - 10 - gfx_text_width(&font_sans, buf), y, buf, ink());
    bar(s, mkrect(r.x + 10, y + 18, r.w - 20, 4),
        mem_total_kb ? (int)(mem_used_kb * 100 / mem_total_kb) : 0);
}

/* ---- drawing into the cache ---- */

static void paint_widget(int i, const struct tm *tm)
{
    struct rect r = card[i];
    if (rect_empty(r))
        return;
    /* The bare body behind it first, shadow and all. */
    struct rect under = rect_intersect(mkrect(r.x - 5, r.y - 5, r.w + 10, r.h + 10), mkrect(0, 0, cache.w, cache.h));
    gfx_noclip(&cache);
    gfx_blit(&cache, under.x, under.y, &bg, under.x, under.y, under.w, under.h);
    if (i == W_CLOCK)
        paint_clock(&cache, r, tm);
    else if (i == W_CALENDAR)
        paint_calendar(&cache, r, tm);
    else
        paint_system(&cache, r);
}

static void now_tm(struct tm *tm)
{
    time_t t = time(NULL);
    *tm = *localtime(&t);
}

/* Lay it out and draw it all, at the size it has now. */
static void render(void)
{
    struct rect sr = sidebar_rect();
    cache_ok = 0;
    if (rect_empty(sr))
        return;
    if (cache.w != sr.w || cache.h != sr.h) {
        surface_free(&cache);
        surface_free(&bg);
        memset(&cache, 0, sizeof(cache));
        memset(&bg, 0, sizeof(bg));
        if (surface_init(&bg, sr.w, sr.h) < 0 || surface_init(&cache, sr.w, sr.h) < 0)
            return;
    }
    place_cards(sr.w, sr.h);
    paint_body(&bg, sr.w, sr.h);
    gfx_noclip(&cache);
    gfx_blit(&cache, 0, 0, &bg, 0, 0, sr.w, sr.h);
    struct tm tm;
    now_tm(&tm);
    last_mday = tm.tm_mday;
    for (int i = 0; i < NWIDGETS; i++)
        paint_widget(i, &tm);
    cache_ok = 1;
}

static void damage_card(int i)
{
    struct rect sr = sidebar_rect(), r = card[i];
    if (!rect_empty(r))
        wm_damage(mkrect(sr.x + r.x - 5, r.y - 5, r.w + 10, r.h + 10));
}

void sidebar_draw(struct surface *s)
{
    struct rect sr = sidebar_rect();
    if (rect_empty(sr) || !rect_overlaps(sr, s->clip))
        return;
    if (!cache_ok || cache.w != sr.w || cache.h != sr.h)
        render();
    if (cache_ok)
        gfx_blit(s, sr.x, sr.y, &cache, 0, 0, sr.w, sr.h);
    else
        gfx_fill(s, sr.x, sr.y, sr.w, sr.h, C_FACE);
}

void sidebar_layout(void)
{
    cache_ok = 0;
    struct rect sr = sidebar_rect();
    if (!rect_empty(sr))
        wm_damage(sr);
}

/* ---- the processor and memory, each second ---- */

static void push(uint8_t *h, int v)
{
    memmove(h, h + 1, HIST - 1);
    h[HIST - 1] = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
}

/* The share of the last second the processors were not idle: their idle ticks
 * against the ticks there were. */
static void sample(void)
{
    struct meminfo mi;
    if (meminfo(&mi) < 0)
        return;
    unsigned n = mi.ncpu ? mi.ncpu : 1, now = now_ms();
    uint32_t idle = 0;
    for (unsigned i = 0; i < n && i < 16; i++)
        idle += mi.cpu_idle[i];
    if (sampled) {
        uint32_t ticks = (now - prev_ms) * HZ / 1000 * n, spent = idle - prev_idle;
        cpu_now = ticks ? 100 - (int)(spent >= ticks ? 100 : spent * 100 / ticks) : cpu_now;
        push(cpu_hist, cpu_now);
    }
    mem_total_kb = mi.total_kb;
    mem_used_kb = mi.total_kb - mi.free_kb;
    push(mem_hist, mi.total_kb ? (int)(mem_used_kb * 100 / mi.total_kb) : 0);
    prev_idle = idle;
    prev_ms = now;
    sampled = 1;
}

void sidebar_tick(void)
{
    sample();
    if (!sidebar_width() || !cache_ok)
        return;
    struct tm tm;
    now_tm(&tm);
    if (tm.tm_mday != last_mday) {      /* a new day: today moves on the calendar */
        if (view_month == (tm.tm_mon + 11) % 12)
            view_year = tm.tm_year + 1900, view_month = tm.tm_mon;
        render();
        wm_damage(sidebar_rect());
        return;
    }
    paint_widget(W_CLOCK, &tm);
    paint_widget(W_SYSTEM, &tm);
    damage_card(W_CLOCK);
    damage_card(W_SYSTEM);
}

/* ---- the menu and the choice ---- */

static void save(void)
{
    char dir[128], value[64];
    if (!grubenv_boot_dir(dir, sizeof(dir)))
        return;
    int n = snprintf(value, sizeof(value), "%s", shown ? "on" : "off");
    for (int i = 0; i < NWIDGETS; i++)
        if (enabled[i])
            n += snprintf(value + n, sizeof(value) - (size_t)n, ",%s", widget_keys[i]);
    const char *keys[1] = { "fluxos_sidebar" }, *vals[1] = { value };
    grubenv_update(dir, keys, vals, 1);
}

static void toggle(int i)
{
    enabled[i] = !enabled[i];
    save();
    render();
    wm_damage(sidebar_rect());
}

static void toggle_clock(void) { toggle(W_CLOCK); }
static void toggle_calendar(void) { toggle(W_CALENDAR); }
static void toggle_system(void) { toggle(W_SYSTEM); }
static int clock_on(void) { return enabled[W_CLOCK]; }
static int calendar_on(void) { return enabled[W_CALENDAR]; }
static int system_on(void) { return enabled[W_SYSTEM]; }
static void close_sidebar(void) { sidebar_show(0); }

static const struct ctx_item menu[] = {
    { "Analog Clock", ICON_NONE, toggle_clock, clock_on },
    { "Calendar", ICON_NONE, toggle_calendar, calendar_on },
    { "System Resources", ICON_NONE, toggle_system, system_on },
    { NULL, ICON_NONE, NULL, NULL },
    { "Collapse Sidebar", ICON_NONE, close_sidebar, NULL },
};

void sidebar_menu(int x, int y)
{
    wm_popup(menu, (int)(sizeof(menu) / sizeof(menu[0])), x, y);
}

void sidebar_show(int on)
{
    if (!on == !shown)
        return;
    struct rect old = sidebar_rect();
    shown = on;
    save();
    cache_ok = 0;
    wm_damage(old);
    wm_work_changed();
}

static void show_month(int delta)
{
    struct tm tm;
    now_tm(&tm);
    if (!delta) {
        view_year = tm.tm_year + 1900;
        view_month = tm.tm_mon;
    } else {
        view_month += delta;
        if (view_month < 0)
            view_month = 11, view_year--;
        if (view_month > 11)
            view_month = 0, view_year++;
    }
    paint_widget(W_CALENDAR, &tm);
    damage_card(W_CALENDAR);
}

void sidebar_press(int x, int y)
{
    struct rect sr = sidebar_rect();
    int lx = x - sr.x, ly = y - sr.y;
    if (rect_contains(r_menu, lx, ly)) {
        sidebar_menu(sr.x + r_menu.x, sr.y + r_menu.y + r_menu.h + 2);
    } else if (rect_contains(r_add, lx, ly)) {
        sidebar_menu(x, y);
    } else if (rect_contains(card[W_CALENDAR], lx, ly)) {
        if (rect_contains(r_prev, lx, ly))
            show_month(-1);
        else if (rect_contains(r_next, lx, ly))
            show_month(1);
        else if (rect_contains(r_month, lx, ly))
            show_month(0);
    } else if (rect_contains(card[W_CLOCK], lx, ly)) {
        datetime_open();
    } else if (rect_contains(card[W_SYSTEM], lx, ly)) {
        taskmgr_open();
    }
}

/* "on,clock,calendar,system" and the like; without a saved one, it is shown on
 * screens wide enough for it to leave windows room, with every widget. */
void sidebar_init(void)
{
    char dir[128], value[64];
    struct tm tm;
    now_tm(&tm);
    view_year = tm.tm_year + 1900;
    view_month = tm.tm_mon;
    shown = screen_w >= 1024;
    if (!grubenv_boot_dir(dir, sizeof(dir)) || !grubenv_get(dir, "fluxos_sidebar", value, sizeof(value)))
        return;
    shown = !strncmp(value, "on", 2);
    for (int i = 0; i < NWIDGETS; i++)
        enabled[i] = 0;
    for (char *p = strchr(value, ','); p; p = strchr(p, ',')) {
        p++;
        for (int i = 0; i < NWIDGETS; i++) {
            size_t n = strlen(widget_keys[i]);
            if (!strncmp(p, widget_keys[i], n) && (p[n] == ',' || !p[n]))
                enabled[i] = 1;
        }
    }
}
