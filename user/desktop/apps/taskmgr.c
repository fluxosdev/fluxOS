/*
 * Task Manager: a Windows NT style window with three pages.
 *
 *  - Applications: the desktop's windows, with End Task (as the close
 *    button does), Switch To and New Task (the Run box).
 *  - Processes: every process with its state, CPU share, CPU time and memory,
 *    sortable by clicking a column heading, and End Process (SIGKILL).
 *  - Performance: CPU and memory gauges with a two minute history graph, and
 *    the memory figures the kernel reports.
 *
 * Once a second the desktop calls taskmgr_tick(), which reads the process
 * table and moves the graphs on. CPU shares come from how much each
 * process's tick count grew since the last look, like top(1).
 *
 * Keys: Up/Down move the selection, Delete ends the selected task or process,
 * Tab and Shift+Tab change page, Enter switches to the selected application.
 */
#include <flux.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

#define TM_W       424
#define TM_H       420
#define MAXP       128
#define MAXAPPS    64
#define HIST       120          /* samples kept: two minutes */
#define MAXCORES   8            /* processors with a graph of their own */
#define ROW_H      17
#define HDR_H      18
#define TAB_H      21
#define STATUS_H   20
#define BTN_W      96
#define BTN_H      24
#define PAD        8

enum { TAB_APPS, TAB_PROCS, TAB_PERF, NTABS };
static const char *const tab_names[NTABS] = { "Applications", "Processes", "Performance" };

enum { COL_PID, COL_NAME, COL_STATE, COL_CPU, COL_TIME, COL_MEM, NCOLS };
static const char *const col_names[NCOLS] = { "PID", "Name", "Status", "CPU", "CPU Time", "Memory" };
static const int col_w[NCOLS] = { 48, 0, 62, 44, 70, 72 };      /* the name takes the rest */

struct prow {
    struct procinfo pi;
    unsigned cpu10;             /* tenths of a percent */
};

/* What the mouse is holding down. */
enum hold { HOLD_NONE, HOLD_BTN, HOLD_THUMB, HOLD_ARROW };

enum { BTN_END, BTN_SWITCH, BTN_NEW, NBTNS };

struct tmgr {
    struct window *win;
    int tab;
    int sort_col, sort_desc;
    int sel_pid;                /* selected process, or -1 */
    struct window *sel_win;     /* selected application */
    int scroll;                 /* in rows, for the current page's list */
    enum hold hold;
    int hold_btn, hold_in;
    enum sbar_part sbar_pressed;
    int grab;
    int last_click;
    unsigned int last_click_ms;
    int pending_pid;            /* what the End Process prompt is about */
};

static struct tmgr *tmgr;

/* ---- what was sampled ---- */

static struct prow rows[MAXP];
static int nrows;
static struct {
    int pid;
    unsigned ticks;
} prev[MAXP];
static int nprev;
static unsigned last_ms;
static int sampled;

static uint8_t cpu_hist[HIST], mem_hist[HIST];
static uint8_t core_hist[MAXCORES][HIST];       /* each processor's, when there are several */
static unsigned core_idle_prev[MAXCORES];
static int ncores;
static int cpu_now, mem_now;
static struct meminfo mi;
static unsigned up_secs;
static int ntasks, nrunning;

static struct window *apps[MAXAPPS];
static int napps;

static unsigned prev_ticks(int pid, int *found)
{
    for (int i = 0; i < nprev; i++) {
        if (prev[i].pid == pid) {
            *found = 1;
            return prev[i].ticks;
        }
    }
    *found = 0;
    return 0;
}

static int sort_col_g, sort_desc_g;

static int row_cmp(const void *a, const void *b)
{
    const struct prow *x = a, *y = b;
    long d = 0;
    switch (sort_col_g) {
    case COL_PID: d = x->pi.pid - y->pi.pid; break;
    case COL_NAME: d = strcasecmp(x->pi.name, y->pi.name); break;
    case COL_STATE: d = x->pi.state - y->pi.state; break;
    case COL_CPU: d = (long)x->cpu10 - (long)y->cpu10; break;
    case COL_TIME: d = (long)x->pi.cpu_ticks - (long)y->pi.cpu_ticks; break;
    case COL_MEM: d = (long)x->pi.mem_kb - (long)y->pi.mem_kb; break;
    }
    if (!d)
        d = x->pi.pid - y->pi.pid;
    if (sort_desc_g)
        d = -d;
    return d < 0 ? -1 : d > 0;
}

static void sort_rows(struct tmgr *t)
{
    sort_col_g = t->sort_col;
    sort_desc_g = t->sort_desc;
    qsort(rows, (size_t)nrows, sizeof(rows[0]), row_cmp);
}

static const char *state_name(int s, int pid)
{
    if (!pid)
        return "Idle";
    switch (s) {
    case PSTATE_RUNNING: return "Running";
    case PSTATE_READY: return "Ready";
    case PSTATE_BLOCKED: return "Waiting";
    case PSTATE_SLEEPING: return "Sleeping";
    case PSTATE_ZOMBIE: return "Zombie";
    case PSTATE_STOPPED: return "Stopped";
    }
    return "?";
}

static void push(uint8_t *hist, int v)
{
    memmove(hist, hist + 1, HIST - 1);
    hist[HIST - 1] = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
}

static void refresh_apps(struct tmgr *t)
{
    napps = 0;
    for (int i = 0; napps < MAXAPPS; i++) {
        struct window *w = wm_nth(i);
        if (!w)
            break;
        if (w == t->win || (w->flags & (WF_NOTASK | WF_MODAL)))
            continue;
        apps[napps++] = w;
    }
    int found = 0;
    for (int i = 0; i < napps; i++)
        found |= apps[i] == t->sel_win;
    if (!found)
        t->sel_win = NULL;
}

/* Read the process table and the memory figures; move the graphs on. */
static void sample(struct tmgr *t)
{
    unsigned now = now_ms();
    meminfo(&mi);
    /* Ticks of processor time there were: shares are of all the processors. */
    unsigned wall_ticks = (now - last_ms) * HZ / 1000;
    unsigned elapsed = wall_ticks * (mi.ncpu > 1 ? mi.ncpu : 1);
    ncores = mi.ncpu > MAXCORES ? MAXCORES : (int)mi.ncpu;
    if (ncores < 2)
        ncores = 0;
    int first = !sampled;
    last_ms = now;
    sampled = 1;

    struct {
        int pid;
        unsigned ticks;
    } cur[MAXP];
    int n = 0, ncur = 0, tasks = 0, running = 0;
    unsigned idle10 = 0;
    for (int slot = 0; n < MAXP; slot++) {
        struct procinfo pi;
        int r = procinfo(slot, &pi);
        if (r < 0)
            break;
        if (r == 0)
            continue;
        int found;
        unsigned before = prev_ticks(pi.pid, &found);
        unsigned delta = first || !found ? (first ? 0 : pi.cpu_ticks) : pi.cpu_ticks - before;
        unsigned cpu10 = elapsed ? delta * 1000 / elapsed : 0;
        if (cpu10 > 1000)
            cpu10 = 1000;
        rows[n].pi = pi;
        rows[n].cpu10 = cpu10;
        cur[ncur].pid = pi.pid;
        cur[ncur].ticks = pi.cpu_ticks;
        ncur++;
        if (pi.pid) {
            tasks++;
            if (pi.state == PSTATE_RUNNING || pi.state == PSTATE_READY)
                running++;
        } else {
            idle10 = cpu10;
        }
        n++;
    }
    memcpy(prev, cur, sizeof(cur[0]) * (size_t)ncur);
    nprev = ncur;
    nrows = n;
    ntasks = tasks;
    nrunning = running;
    sort_rows(t);

    up_secs = now / 1000;
    if (!first && elapsed) {
        cpu_now = (int)((1000 - (idle10 > 1000 ? 1000 : idle10) + 5) / 10);
        push(cpu_hist, cpu_now);
        for (int i = 0; i < ncores && wall_ticks; i++) {
            unsigned idle = (mi.cpu_idle[i] - core_idle_prev[i]) * 100 / wall_ticks;
            push(core_hist[i], (int)(100 - (idle > 100 ? 100 : idle)));
        }
    }
    for (int i = 0; i < ncores; i++)
        core_idle_prev[i] = mi.cpu_idle[i];
    mem_now = mi.total_kb ? (int)((mi.total_kb - mi.free_kb) / (mi.total_kb / 100 ? mi.total_kb / 100 : 1)) : 0;
    if (!first)
        push(mem_hist, mem_now);
    else
        memset(mem_hist, 0, sizeof(mem_hist));

    int keep = 0;
    for (int i = 0; i < nrows; i++)
        keep |= rows[i].pi.pid == t->sel_pid;
    if (!keep)
        t->sel_pid = -1;
    refresh_apps(t);
}

/* ---- geometry ---- */

static struct rect page_rect(const struct tmgr *t)
{
    const struct surface *s = &t->win->content;
    return mkrect(6, 6 + TAB_H - 1, s->w - 12, s->h - 6 - (TAB_H - 1) - STATUS_H - 10);
}

static struct rect tab_rect(const struct tmgr *t, int i)
{
    (void)t;
    int x = 6;
    for (int k = 0; k < i; k++)
        x += 96;
    int sel = t->tab == i;
    return mkrect(x - (sel ? 2 : 0), 6 + (sel ? 0 : 2), 96 + (sel ? 4 : 0), TAB_H - (sel ? 0 : 2));
}

static struct rect button_rect(const struct tmgr *t, int i, int count)
{
    struct rect p = page_rect(t);
    int x = p.x + p.w - PAD - BTN_W - (count - 1 - i) * (BTN_W + 6);
    return mkrect(x, p.y + p.h - PAD - BTN_H, BTN_W, BTN_H);
}

/* The sunken white list, headings (on the Processes page) above it. */
static struct rect list_rect(const struct tmgr *t)
{
    struct rect p = page_rect(t);
    int top = p.y + PAD + (t->tab == TAB_PROCS ? HDR_H : 0);
    int bottom = p.y + p.h - PAD - BTN_H - 8;
    return mkrect(p.x + PAD, top, p.w - 2 * PAD - SBAR_W, bottom - top);
}

static int visible_rows(const struct tmgr *t)
{
    int n = (list_rect(t).h - 4) / ROW_H;
    return n < 1 ? 1 : n;
}

static int list_count(const struct tmgr *t)
{
    return t->tab == TAB_APPS ? napps : nrows;
}

static struct sbar list_bar(const struct tmgr *t)
{
    struct rect l = list_rect(t);
    return (struct sbar){ mkrect(l.x + l.w, l.y, SBAR_W, l.h), 1, t->scroll, visible_rows(t),
                          list_count(t) };
}

static void clamp_scroll(struct tmgr *t)
{
    struct sbar b = list_bar(t);
    int max = sbar_max(&b);
    if (t->scroll > max)
        t->scroll = max;
    if (t->scroll < 0)
        t->scroll = 0;
}

static int selected_index(const struct tmgr *t)
{
    if (t->tab == TAB_APPS) {
        for (int i = 0; i < napps; i++)
            if (apps[i] == t->sel_win)
                return i;
    } else {
        for (int i = 0; i < nrows; i++)
            if (rows[i].pi.pid == t->sel_pid)
                return i;
    }
    return -1;
}

static void scroll_to_selection(struct tmgr *t)
{
    int i = selected_index(t), vis = visible_rows(t);
    if (i < 0)
        return;
    if (i < t->scroll)
        t->scroll = i;
    else if (i >= t->scroll + vis)
        t->scroll = i - vis + 1;
    clamp_scroll(t);
}

/* x of each column on the Processes page, relative to the list's left edge. */
static void col_layout(const struct tmgr *t, int *x, int *w)
{
    int total = list_rect(t).w - 4, fixed = 0;
    for (int c = 0; c < NCOLS; c++)
        fixed += col_w[c];
    int pos = 0;
    for (int c = 0; c < NCOLS; c++) {
        w[c] = col_w[c] ? col_w[c] : (total > fixed + 40 ? total - fixed : 40);
        x[c] = pos;
        pos += w[c];
    }
}

/* ---- drawing ---- */

static void text_in(struct surface *s, int x, int y, int w, const char *str, color_t c)
{
    char buf[96];
    gfx_ellipsize(&font_sans, str, w, buf, sizeof(buf));
    gfx_text(s, &font_sans, x, y, buf, c);
}

static void text_right(struct surface *s, int right, int y, const char *str, color_t c)
{
    gfx_text(s, &font_sans, right - gfx_text_width(&font_sans, str), y, str, c);
}

static void draw_group(struct surface *s, struct rect r, const char *title)
{
    gfx_frame(s, r.x + 1, r.y + 7, r.w - 1, r.h - 7, C_WHITE);
    gfx_frame(s, r.x, r.y + 6, r.w - 1, r.h - 7, C_SHADOW);
    int tw = gfx_text_width(&font_sans, title);
    gfx_fill(s, r.x + 6, r.y, tw + 4, font_sans.height, C_FACE);
    gfx_text(s, &font_sans, r.x + 8, r.y, title, C_BLACK);
}

static void fmt_kb(char *out, size_t n, unsigned kb)
{
    if (kb >= 10 * 1024)
        snprintf(out, n, "%u MB", kb / 1024);
    else
        snprintf(out, n, "%u K", kb);
}

static void fmt_time(char *out, size_t n, unsigned ticks)
{
    unsigned secs = ticks / HZ;
    snprintf(out, n, "%u:%02u:%02u", secs / 3600, secs / 60 % 60, secs % 60);
}

static void draw_tabs(struct tmgr *t, struct surface *s)
{
    struct rect p = page_rect(t);
    gfx_fill(s, p.x, p.y, p.w, p.h, C_FACE);
    gfx_bevel(s, p, BEVEL_RAISED);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < NTABS; i++) {
            if ((i == t->tab) != pass)
                continue;
            struct rect r = tab_rect(t, i);
            gfx_fill(s, r.x, r.y, r.w, r.h + (i == t->tab ? 1 : 0), C_FACE);
            draw_tab_edges(s, r, i == t->tab);
            int tw = gfx_text_width(&font_sans, tab_names[i]);
            gfx_text(s, &font_sans, r.x + (r.w - tw) / 2, r.y + (TAB_H - font_sans.height) / 2 + 1,
                     tab_names[i], C_BLACK);
            if (i == t->tab && t->hold == HOLD_NONE)
                gfx_focus_rect(s, mkrect(r.x + 4, r.y + 3, r.w - 8, TAB_H - 6));
        }
    }
}

static void draw_buttons(struct tmgr *t, struct surface *s)
{
    static const char *const apps_labels[] = { "End Task", "Switch To", "New Task..." };
    if (t->tab == TAB_APPS) {
        for (int i = 0; i < NBTNS; i++) {
            struct rect r = button_rect(t, i, NBTNS);
            int pressed = t->hold == HOLD_BTN && t->hold_btn == i && t->hold_in;
            draw_button(s, r, apps_labels[i], pressed, 0, 0);
        }
    } else if (t->tab == TAB_PROCS) {
        struct rect r = button_rect(t, 0, 1);
        int pressed = t->hold == HOLD_BTN && t->hold_in;
        draw_button(s, r, "End Process", pressed, 0, 0);
    }
}

static void draw_list_frame(struct tmgr *t, struct surface *s)
{
    struct rect l = list_rect(t);
    struct rect box = mkrect(l.x, l.y, l.w + SBAR_W, l.h);
    gfx_fill(s, l.x, l.y, l.w, l.h, C_WHITE);
    gfx_bevel(s, box, BEVEL_SUNKEN);
    struct sbar b = list_bar(t);
    sbar_draw(s, &b, t->hold == HOLD_ARROW || t->hold == HOLD_THUMB ? t->sbar_pressed : SBAR_NONE);
}

static void draw_apps(struct tmgr *t, struct surface *s)
{
    struct rect l = list_rect(t);
    draw_list_frame(t, s);
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, mkrect(l.x + 2, l.y + 2, l.w - 4, l.h - 4));
    int vis = visible_rows(t);
    for (int k = 0; k < vis && t->scroll + k < napps; k++) {
        struct window *w = apps[t->scroll + k];
        int y = l.y + 2 + k * ROW_H;
        int sel = w == t->sel_win;
        if (sel)
            gfx_fill(s, l.x + 2, y, l.w - 4, ROW_H, C_NAVY);
        color_t fg = sel ? C_WHITE : C_BLACK;
        draw_icon(s, l.x + 4, y + 1, w->icon, 16);
        int status_w = 74;
        text_in(s, l.x + 24, y + (ROW_H - font_sans.height) / 2, l.w - 24 - status_w - 8, w->title, fg);
        gfx_text(s, &font_sans, l.x + l.w - status_w - 4, y + (ROW_H - font_sans.height) / 2,
                 w->minimized ? "Minimized" : "Running", fg);
        if (sel && t->hold == HOLD_NONE)
            gfx_focus_rect(s, mkrect(l.x + 2, y, l.w - 4, ROW_H));
    }
    s->clip = saved;
    if (!napps)
        gfx_text(s, &font_sans, l.x + 8, l.y + 6, "No other windows are open.", C_SHADOW);
}

static void draw_procs(struct tmgr *t, struct surface *s)
{
    struct rect l = list_rect(t), p = page_rect(t);
    int cx[NCOLS], cw[NCOLS];
    col_layout(t, cx, cw);

    /* Column headings: buttons that sort. */
    for (int c = 0; c < NCOLS; c++) {
        struct rect h = mkrect(l.x + 2 + cx[c], p.y + PAD, cw[c], HDR_H);
        if (c == NCOLS - 1)
            h.w = l.x + l.w - h.x + SBAR_W;
        int down = t->hold == HOLD_BTN && t->hold_btn == 100 + c && t->hold_in;
        gfx_fill(s, h.x, h.y, h.w, h.h, C_FACE);
        gfx_bevel(s, h, down ? BEVEL_PRESSED : BEVEL_RAISED);
        int right = c == COL_PID || c == COL_CPU || c == COL_TIME || c == COL_MEM;
        int ty = h.y + (h.h - font_sans.height) / 2 + down;
        int arrow = c == t->sort_col ? 10 : 0;
        if (right)
            text_right(s, h.x + h.w - 6 - arrow + down, ty, col_names[c], C_BLACK);
        else
            gfx_text(s, &font_sans, h.x + 6 + down, ty, col_names[c], C_BLACK);
        if (c == t->sort_col) {                 /* a little triangle: up is ascending */
            int ax = h.x + h.w - 12, ay = h.y + h.h / 2 - 1;
            for (int i = 0; i < 4; i++)
                gfx_hline(s, ax + (t->sort_desc ? i : 3 - i), ay + (t->sort_desc ? i : 3 - i) - 1,
                          t->sort_desc ? 7 - 2 * i : 1 + 2 * i, C_BLACK);
        }
    }

    draw_list_frame(t, s);
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, mkrect(l.x + 2, l.y + 2, l.w - 4, l.h - 4));
    int vis = visible_rows(t);
    for (int k = 0; k < vis && t->scroll + k < nrows; k++) {
        const struct prow *r = &rows[t->scroll + k];
        int y = l.y + 2 + k * ROW_H, ty = y + (ROW_H - font_sans.height) / 2;
        int sel = r->pi.pid == t->sel_pid;
        if (sel)
            gfx_fill(s, l.x + 2, y, l.w - 4, ROW_H, C_NAVY);
        color_t fg = sel ? C_WHITE : C_BLACK;
        char buf[32];
        int x0 = l.x + 2;
        snprintf(buf, sizeof(buf), "%d", r->pi.pid);
        text_right(s, x0 + cx[COL_PID] + cw[COL_PID] - 6, ty, buf, fg);
        text_in(s, x0 + cx[COL_NAME] + 6, ty, cw[COL_NAME] - 10,
                r->pi.pid ? r->pi.name : "System Idle", fg);
        gfx_text(s, &font_sans, x0 + cx[COL_STATE] + 6, ty, state_name(r->pi.state, r->pi.pid), fg);
        snprintf(buf, sizeof(buf), "%02u", (r->cpu10 + 5) / 10);
        text_right(s, x0 + cx[COL_CPU] + cw[COL_CPU] - 6, ty, buf, fg);
        fmt_time(buf, sizeof(buf), r->pi.cpu_ticks);
        text_right(s, x0 + cx[COL_TIME] + cw[COL_TIME] - 6, ty, buf, fg);
        snprintf(buf, sizeof(buf), "%u K", r->pi.mem_kb);
        text_right(s, x0 + cx[COL_MEM] + cw[COL_MEM] - 6, ty, buf, fg);
        if (sel && t->hold == HOLD_NONE)
            gfx_focus_rect(s, mkrect(l.x + 2, y, l.w - 4, ROW_H));
    }
    s->clip = saved;
}

/* A tall gauge of lit segments, filled to percent. */
static void draw_gauge(struct surface *s, struct rect r, int percent)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, C_BLACK);
    gfx_bevel(s, r, BEVEL_SUNKEN);
    int inner_h = r.h - 4, lit = inner_h * percent / 100 / 3;
    for (int i = 0; i < inner_h / 3; i++) {
        int y = r.y + r.h - 2 - (i + 1) * 3;
        gfx_fill(s, r.x + 3, y, r.w - 6, 2, i < lit ? RGB(0x00, 0xE0, 0x00) : RGB(0x00, 0x40, 0x00));
    }
}

/* The last two minutes as a line, newest at the right edge. */
static void draw_graph(struct surface *s, struct rect r, const uint8_t *hist)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, C_BLACK);
    gfx_bevel(s, r, BEVEL_SUNKEN);
    struct rect in = mkrect(r.x + 2, r.y + 2, r.w - 4, r.h - 4);
    color_t grid = RGB(0x00, 0x60, 0x00), line = RGB(0x00, 0xFF, 0x00);
    for (int y = in.y + in.h / 4; y < in.y + in.h; y += in.h / 4)
        gfx_hline(s, in.x, y, in.w, grid);
    for (int x = in.x + in.w - 1; x >= in.x; x -= 12)
        gfx_vline(s, x, in.y, in.h, grid);
    int step = 2, prev_y = -1;
    for (int i = HIST - 1, x = in.x + in.w - 2; i >= 0 && x >= in.x; i--, x -= step) {
        int y = in.y + in.h - 1 - (in.h - 1) * hist[i] / 100;
        if (prev_y < 0) {
            gfx_pixel(s, x, y, line);
        } else {
            int y0 = y < prev_y ? y : prev_y, y1 = y < prev_y ? prev_y : y;
            gfx_vline(s, x + step, y0, y1 - y0 + 1, line);
            gfx_hline(s, x, y, step, line);
        }
        prev_y = y;
    }
}

static void stat_line(struct surface *s, struct rect g, int row, const char *name, const char *value)
{
    int y = g.y + 16 + row * 15;
    gfx_text(s, &font_sans, g.x + 8, y, name, C_BLACK);
    text_right(s, g.x + g.w - 8, y, value, C_BLACK);
}

static void draw_perf(struct tmgr *t, struct surface *s)
{
    struct rect p = page_rect(t);
    int text_h = 76, gap = 4;
    int top = p.y + 4, avail = p.h - 8 - text_h - gap;
    int row_h = avail / 2, left_w = 80;
    int full = p.w - 2 * PAD;
    char buf[48];

    for (int k = 0; k < 2; k++) {
        int y = top + k * (row_h + gap / 2);
        int pct = k ? mem_now : cpu_now;
        struct rect gl = mkrect(p.x + PAD, y, left_w, row_h);
        struct rect gr = mkrect(p.x + PAD + left_w + 6, y, full - left_w - 6, row_h);
        draw_group(s, gl, k ? "MEM Usage" : "CPU Usage");
        draw_group(s, gr, k ? "Memory Usage History" : ncores ? "CPU Usage History (per processor)"
                                                              : "CPU Usage History");
        struct rect gauge = mkrect(gl.x + 14, gl.y + 16, left_w - 28, gl.h - 16 - 22);
        draw_gauge(s, gauge, pct);
        if (k) {
            fmt_kb(buf, sizeof(buf), mi.total_kb - mi.free_kb);
            int tw = gfx_text_width(&font_sans, buf);
            gfx_text(s, &font_sans, gl.x + (gl.w - tw) / 2, gl.y + gl.h - 18, buf, C_BLACK);
        } else {
            snprintf(buf, sizeof(buf), "%d %%", pct);
            int tw = gfx_text_width(&font_sans, buf);
            gfx_text(s, &font_sans, gl.x + (gl.w - tw) / 2, gl.y + gl.h - 18, buf, C_BLACK);
        }
        struct rect gg = mkrect(gr.x + 8, gr.y + 16, gr.w - 16, gr.h - 16 - 8);
        if (k || !ncores) {
            draw_graph(s, gg, k ? mem_hist : cpu_hist);
            continue;
        }
        /* one graph for each processor, side by side, each named under it */
        int cw = (gg.w - (ncores - 1) * 4) / ncores;
        for (int c = 0; c < ncores; c++) {
            struct rect cr = mkrect(gg.x + c * (cw + 4), gg.y, cw, gg.h - 14);
            draw_graph(s, cr, core_hist[c]);
            snprintf(buf, sizeof(buf), "CPU %d", c);
            int tw = gfx_text_width(&font_sans, buf);
            gfx_text(s, &font_sans, cr.x + (cr.w - tw) / 2, cr.y + cr.h + 1, buf, C_BLACK);
        }
    }

    int ty = p.y + p.h - 4 - text_h, gw = (full - 2 * 6) / 3;
    struct rect g1 = mkrect(p.x + PAD, ty, gw, text_h);
    struct rect g2 = mkrect(g1.x + gw + 6, ty, gw, text_h);
    struct rect g3 = mkrect(g2.x + gw + 6, ty, gw, text_h);
    draw_group(s, g1, "Totals");
    snprintf(buf, sizeof(buf), "%d", ntasks);
    stat_line(s, g1, 0, "Processes", buf);
    snprintf(buf, sizeof(buf), "%d", nrunning);
    stat_line(s, g1, 1, "Runnable", buf);
    fmt_time(buf, sizeof(buf), up_secs * HZ);
    stat_line(s, g1, 2, "Up Time", buf);

    draw_group(s, g2, "Physical Memory");
    fmt_kb(buf, sizeof(buf), mi.total_kb);
    stat_line(s, g2, 0, "Total", buf);
    fmt_kb(buf, sizeof(buf), mi.free_kb);
    stat_line(s, g2, 1, "Available", buf);
    fmt_kb(buf, sizeof(buf), mi.total_kb - mi.free_kb);
    stat_line(s, g2, 2, "In Use", buf);

    draw_group(s, g3, "Kernel Memory");
    fmt_kb(buf, sizeof(buf), mi.kernel_kb);
    stat_line(s, g3, 0, "Image", buf);
    fmt_kb(buf, sizeof(buf), mi.heap_total_kb);
    stat_line(s, g3, 1, "Heap", buf);
    fmt_kb(buf, sizeof(buf), mi.heap_used_kb);
    stat_line(s, g3, 2, "Heap in Use", buf);
}

static void draw_status(struct tmgr *t, struct surface *s)
{
    int y = s->h - STATUS_H - 2, w = s->w - 12;
    int w1 = w * 30 / 100, w2 = w * 28 / 100, w3 = w - w1 - w2 - 4;
    struct rect cells[3] = { mkrect(6, y, w1, STATUS_H), mkrect(6 + w1 + 2, y, w2, STATUS_H),
                             mkrect(6 + w1 + w2 + 4, y, w3, STATUS_H) };
    char text[3][48], k[16], k2[16];
    snprintf(text[0], sizeof(text[0]), "Processes: %d", ntasks);
    snprintf(text[1], sizeof(text[1]), "CPU Usage: %d%%", cpu_now);
    fmt_kb(k, sizeof(k), mi.total_kb - mi.free_kb);
    fmt_kb(k2, sizeof(k2), mi.total_kb);
    snprintf(text[2], sizeof(text[2]), "Mem Usage: %s / %s", k, k2);
    (void)t;
    for (int i = 0; i < 3; i++) {
        gfx_bevel(s, cells[i], BEVEL_THIN_SUNKEN);
        text_in(s, cells[i].x + 6, cells[i].y + (STATUS_H - font_sans.height) / 2, cells[i].w - 10,
                text[i], C_BLACK);
    }
}

static void paint(struct tmgr *t)
{
    struct surface *s = &t->win->content;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);
    clamp_scroll(t);
    draw_tabs(t, s);
    switch (t->tab) {
    case TAB_APPS: draw_apps(t, s); break;
    case TAB_PROCS: draw_procs(t, s); break;
    case TAB_PERF: draw_perf(t, s); break;
    }
    draw_buttons(t, s);
    draw_status(t, s);
    wm_damage_client(t->win, mkrect(0, 0, s->w, s->h));
}

/* ---- actions ---- */

static int window_alive(const struct window *w)
{
    for (int i = 0; wm_nth(i); i++)
        if (wm_nth(i) == w)
            return 1;
    return 0;
}

static void switch_to(struct tmgr *t)
{
    if (t->sel_win && window_alive(t->sel_win))
        wm_focus(t->sel_win);
}

static void end_task(struct tmgr *t)
{
    struct window *w = t->sel_win;
    if (!w || !window_alive(w))
        return;
    t->sel_win = NULL;
    wm_close(w);
    refresh_apps(t);
}

static void do_end_process(void *ctx)
{
    (void)ctx;
    if (!tmgr)
        return;
    int pid = tmgr->pending_pid;
    if (kill(pid, SIGKILL) < 0)
        message_box("Task Manager", ICON_WARNING, "The process could not be ended.");
    sample(tmgr);
    paint(tmgr);
}

static void end_process(struct tmgr *t)
{
    int i = selected_index(t);
    if (i < 0)
        return;
    const struct procinfo *pi = &rows[i].pi;
    if (pi->pid <= 1 || pi->pid == getpid()) {
        message_box("Task Manager", ICON_WARNING,
                    pi->pid == getpid() ? "That is the desktop itself: end it with Shut Down."
                                        : "This process is part of the system and cannot be ended.");
        return;
    }
    char msg[128];
    t->pending_pid = pi->pid;
    snprintf(msg, sizeof(msg), "Do you want to end \"%s\" (process %d)?\nUnsaved data in it will be lost.",
             pi->name, pi->pid);
    ask_yes_no("Task Manager", ICON_WARNING, msg, do_end_process, t);
}

static void button_action(struct tmgr *t, int btn)
{
    if (t->tab == TAB_APPS) {
        if (btn == BTN_END)
            end_task(t);
        else if (btn == BTN_SWITCH)
            switch_to(t);
        else
            run_open();
    } else if (t->tab == TAB_PROCS) {
        end_process(t);
    }
}

static void set_tab(struct tmgr *t, int tab)
{
    t->tab = (tab + NTABS) % NTABS;
    t->scroll = 0;
    scroll_to_selection(t);
}

/* ---- input ---- */

static int button_at(const struct tmgr *t, int x, int y)
{
    int count = t->tab == TAB_APPS ? NBTNS : t->tab == TAB_PROCS ? 1 : 0;
    for (int i = 0; i < count; i++)
        if (rect_contains(button_rect(t, i, count), x, y))
            return i;
    return -1;
}

static int header_at(const struct tmgr *t, int x, int y)
{
    if (t->tab != TAB_PROCS)
        return -1;
    struct rect l = list_rect(t), p = page_rect(t);
    if (y < p.y + PAD || y >= p.y + PAD + HDR_H || x < l.x + 2 || x >= l.x + l.w + SBAR_W)
        return -1;
    int cx[NCOLS], cw[NCOLS];
    col_layout(t, cx, cw);
    for (int c = 0; c < NCOLS; c++)
        if (x < l.x + 2 + cx[c] + cw[c] || c == NCOLS - 1)
            return c;
    return -1;
}

static void select_index(struct tmgr *t, int i)
{
    if (t->tab == TAB_APPS)
        t->sel_win = i >= 0 && i < napps ? apps[i] : NULL;
    else
        t->sel_pid = i >= 0 && i < nrows ? rows[i].pi.pid : -1;
}

static void scroll_step(struct tmgr *t, const struct sbar *b, enum sbar_part part)
{
    switch (part) {
    case SBAR_ARROW1: t->scroll -= 1; break;
    case SBAR_ARROW2: t->scroll += 1; break;
    case SBAR_PAGE1: t->scroll -= b->page > 1 ? b->page - 1 : 1; break;
    case SBAR_PAGE2: t->scroll += b->page > 1 ? b->page - 1 : 1; break;
    default: break;
    }
    clamp_scroll(t);
}

static void tm_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct tmgr *t = w->priv;
    int left = buttons & MOUSE_LEFT;
    int pressed = (changed & MOUSE_LEFT) && left, released = (changed & MOUSE_LEFT) && !left;
    struct sbar bar = list_bar(t);

    if (pressed) {
        for (int i = 0; i < NTABS; i++) {
            if (rect_contains(tab_rect(t, i), x, y)) {
                if (i != t->tab)
                    set_tab(t, i);
                paint(t);
                return;
            }
        }
        int b = button_at(t, x, y);
        if (b >= 0) {
            t->hold = HOLD_BTN;
            t->hold_btn = b;
            t->hold_in = 1;
        } else if (header_at(t, x, y) >= 0) {
            t->hold = HOLD_BTN;
            t->hold_btn = 100 + header_at(t, x, y);
            t->hold_in = 1;
        } else if (t->tab != TAB_PERF) {
            enum sbar_part part = sbar_hit(&bar, x, y);
            struct rect l = list_rect(t);
            if (part != SBAR_NONE) {
                t->sbar_pressed = part;
                if (part == SBAR_THUMB) {
                    t->hold = HOLD_THUMB;
                    t->grab = y - sbar_thumb(&bar).y;
                } else {
                    t->hold = HOLD_ARROW;
                    scroll_step(t, &bar, part);
                }
            } else if (rect_contains(l, x, y)) {
                int i = t->scroll + (y - l.y - 2) / ROW_H;
                unsigned int now = now_ms();
                int dbl = i == t->last_click && now - t->last_click_ms < DBLCLICK_MS;
                select_index(t, i < list_count(t) ? i : -1);
                t->last_click = i;
                t->last_click_ms = now;
                if (dbl && t->tab == TAB_APPS)
                    switch_to(t);
            } else {
                return;
            }
        } else {
            return;
        }
    } else if (released) {
        int h = t->hold, b = t->hold_btn, in = t->hold_in;
        t->hold = HOLD_NONE;
        if (h == HOLD_BTN && in) {
            if (b >= 100) {
                int c = b - 100;
                if (t->sort_col == c) {
                    t->sort_desc = !t->sort_desc;
                } else {
                    t->sort_col = c;
                    t->sort_desc = c == COL_CPU || c == COL_TIME || c == COL_MEM;
                }
                sort_rows(t);
                scroll_to_selection(t);
            } else {
                button_action(t, b);
            }
        }
    } else if (t->hold == HOLD_BTN && left) {
        int in = t->hold_btn >= 100 ? header_at(t, x, y) == t->hold_btn - 100
                                    : button_at(t, x, y) == t->hold_btn;
        if (in == t->hold_in)
            return;
        t->hold_in = in;
    } else if (t->hold == HOLD_THUMB && left) {
        t->scroll = sbar_drag(&bar, y, t->grab);
    } else {
        return;
    }
    paint(t);
}

static void tm_wheel(struct window *w, int steps)
{
    struct tmgr *t = w->priv;
    if (t->tab == TAB_PERF)
        return;
    t->scroll -= steps * mouse_wheel_lines();
    clamp_scroll(t);
    paint(t);
}

static void tm_key(struct window *w, const struct input_event *ev)
{
    struct tmgr *t = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    int i = selected_index(t);
    if (ev->code == KEY_TAB) {
        set_tab(t, t->tab + ((ev->mods & KMOD_SHIFT) ? -1 : 1));
    } else if (t->tab != TAB_PERF && ev->code == KEY_UP) {
        select_index(t, i > 0 ? i - 1 : 0);
        scroll_to_selection(t);
    } else if (t->tab != TAB_PERF && ev->code == KEY_DOWN) {
        select_index(t, i < 0 ? 0 : i + 1 < list_count(t) ? i + 1 : i);
        scroll_to_selection(t);
    } else if (t->tab != TAB_PERF && ev->code == KEY_HOME) {
        select_index(t, 0);
        scroll_to_selection(t);
    } else if (t->tab != TAB_PERF && ev->code == KEY_END) {
        select_index(t, list_count(t) - 1);
        scroll_to_selection(t);
    } else if (t->tab != TAB_PERF && ev->code == KEY_PGUP) {
        select_index(t, i - visible_rows(t) < 0 ? 0 : i - visible_rows(t));
        scroll_to_selection(t);
    } else if (t->tab != TAB_PERF && ev->code == KEY_PGDN) {
        int n = (i < 0 ? 0 : i) + visible_rows(t);
        select_index(t, n < list_count(t) ? n : list_count(t) - 1);
        scroll_to_selection(t);
    } else if (ev->code == KEY_DELETE) {
        button_action(t, BTN_END);
        return;
    } else if (ev->code == KEY_ENTER && t->tab == TAB_APPS) {
        switch_to(t);
        return;
    } else {
        return;
    }
    paint(t);
}

static void tm_resize(struct window *w)
{
    struct tmgr *t = w->priv;
    paint(t);
}

static void tm_focus(struct window *w, int focused)
{
    struct tmgr *t = w->priv;
    (void)focused;
    if (t->win)                         /* not while wm_create() runs */
        paint(t);
}

static void tm_destroy(struct window *w)
{
    free(w->priv);
    tmgr = NULL;
    sampled = 0;
    nprev = 0;
}

static const struct window_ops tm_ops = {
    .key = tm_key,
    .mouse = tm_mouse,
    .wheel = tm_wheel,
    .resize = tm_resize,
    .focus = tm_focus,
    .destroy = tm_destroy,
};

void taskmgr_tick(void)
{
    if (!tmgr)
        return;
    sample(tmgr);
    paint(tmgr);
}

struct window *taskmgr_open(void)
{
    if (tmgr) {                         /* one is enough */
        wm_focus(tmgr->win);
        return tmgr->win;
    }
    struct tmgr *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->tab = TAB_PROCS;
    t->sort_col = COL_CPU;
    t->sort_desc = 1;
    t->sel_pid = -1;
    t->last_click = -1;
    memset(cpu_hist, 0, sizeof(cpu_hist));
    memset(core_hist, 0, sizeof(core_hist));
    memset(mem_hist, 0, sizeof(mem_hist));
    cpu_now = 0;
    struct window *w = wm_create("Task Manager", ICON_TASKMGR, TM_W, TM_H, WF_MINIMIZE | WF_MAXIMIZE,
                                 &tm_ops, t);
    if (!w) {
        free(t);
        return NULL;
    }
    t->win = w;
    tmgr = t;
    wm_set_min_size(w, 380, 300);
    sample(t);
    paint(t);
    return w;
}
