/*
 * Mouse Properties under Settings: button configuration (right-handed or
 * left-handed), double-click speed with a test box, pointer speed for
 * mouse motion, and scroll wheel speed and direction. The settings
 * themselves are mousecfg.c's; the desktop hears of each change, as it
 * moves the pointer (mouse_config_changed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop.h"
#include "mousecfg.h"

#define MP_W 368
#define MP_H 398

enum ctl {
    C_RIGHT,
    C_LEFT,
    C_DBLCLICK,
    C_SPEED,
    C_WHEEL_LINES,
    C_WHEEL_REV,
    C_OK,
    C_CANCEL,
    C_APPLY,
    C_COUNT,
};

#define IS_BUTTON(k) ((k) >= C_OK)
#define IS_SLIDER(k) ((k) == C_DBLCLICK || (k) == C_SPEED || (k) == C_WHEEL_LINES)

struct mprops {
    struct window *win;
    struct mouse_config cfg;
    enum ctl focus;
    int pressed, pressed_in;
    int dragging;           /* slider control being dragged, or -1 */
    int test_open;          /* toggled by double-clicking the test box */
    unsigned int last_test_ms;
};

static struct mprops *dlg;

/* Preview slider and wheel settings live while the dialog is open; button swap waits for Apply/OK. */
static void sync_preview(const struct mouse_config *c)
{
    mouse_active_cfg.speed = c->speed;
    mouse_active_cfg.dblclick = c->dblclick;
    mouse_active_cfg.wheel_lines = c->wheel_lines;
    mouse_active_cfg.wheel_reverse = c->wheel_reverse;
    mouse_config_changed();             /* the desktop moves the pointer: it previews too */
}

static void commit_config(const struct mouse_config *c)
{
    int changed = memcmp(c, &mouse_saved_cfg, sizeof(*c)) != 0;
    mouse_saved_cfg = *c;
    mouse_active_cfg = *c;
    if (changed)
        mouse_save(c);
    mouse_config_changed();
}

/* ---- geometry and drawing ---- */

static struct rect test_box_rect(void)
{
    return mkrect(244, 122, 98, 60);
}

static struct rect ctl_rect(enum ctl k)
{
    switch (k) {
    case C_RIGHT:       return mkrect(26, 30, 120, 14);
    case C_LEFT:        return mkrect(26, 50, 120, 14);
    case C_DBLCLICK:    return mkrect(26, 144, 200, 20);
    case C_SPEED:       return mkrect(76, 238, 260, 20);
    case C_WHEEL_LINES: return mkrect(132, 298, 136, 20);
    case C_WHEEL_REV:   return mkrect(26, 326, 300, 14);
    case C_OK:          return mkrect(MP_W - 14 - 3 * 75 - 2 * 8, MP_H - 12 - 23, 75, 23);
    case C_CANCEL:      return mkrect(MP_W - 14 - 2 * 75 - 8, MP_H - 12 - 23, 75, 23);
    default:            return mkrect(MP_W - 14 - 75, MP_H - 12 - 23, 75, 23);
    }
}

static void draw_group(struct surface *s, struct rect r, const char *label)
{
    if (KINETIK) {
        gfx_round_frame(s, r, 4, 4, ARGB(255, sys.field_edge));
    } else {
        gfx_frame(s, r.x, r.y, r.w - 1, r.h - 1, C_SHADOW);
        gfx_frame(s, r.x + 1, r.y + 1, r.w - 1, r.h - 1, C_WHITE);
    }
    int tw = gfx_text_width(&font_sans, label);
    gfx_fill(s, r.x + 6, r.y - 6, tw + 4, font_sans.height, C_FACE);
    gfx_text(s, &font_sans, r.x + 8, r.y - 7, label, C_BLACK);
}

static void draw_check(struct surface *s, int x, int y, const char *label, int checked, int focused)
{
    struct rect box = mkrect(x, y, 13, 13);
    if (KINETIK) {
        gfx_round_fill(s, box, 3, 3, ARGB(255, 0xFFFFFF));
        gfx_round_frame(s, box, 3, 3, ARGB(255, checked ? theme.wall2 : sys.field_edge));
    } else {
        gfx_fill(s, box.x, box.y, box.w, box.h, C_WHITE);
        gfx_bevel(s, box, BEVEL_SUNKEN);
    }
    if (checked) {
        color_t tick = KINETIK ? theme.wall2 : C_BLACK;
        for (int i = 0; i < 7; i++) {
            int yy = y + (i < 3 ? 5 + i : 9 - (i - 2));
            gfx_vline(s, x + 3 + i, yy, 3, tick);
        }
    }
    int tx = x + 18, ty = y + (13 - font_sans.height) / 2;
    gfx_text(s, &font_sans, tx, ty, label, C_BLACK);
    if (focused)
        gfx_focus_rect(s, mkrect(tx - 2, ty, gfx_text_width(&font_sans, label) + 4, font_sans.height + 1));
}

static int slider_steps(enum ctl k)
{
    if (k == C_DBLCLICK)
        return DBLCLICK_STEPS;
    if (k == C_SPEED)
        return SPEED_STEPS;
    return WHEEL_MAX;
}

static int slider_val(const struct mprops *m, enum ctl k)
{
    if (k == C_DBLCLICK)
        return m->cfg.dblclick;
    if (k == C_SPEED)
        return m->cfg.speed;
    return m->cfg.wheel_lines - 1;
}

static void set_slider_val(struct mprops *m, enum ctl k, int v)
{
    int n = slider_steps(k);
    v = mouse_clamp(v, 0, n - 1);
    if (k == C_DBLCLICK)
        m->cfg.dblclick = v;
    else if (k == C_SPEED)
        m->cfg.speed = v;
    else if (k == C_WHEEL_LINES)
        m->cfg.wheel_lines = v + 1;
    sync_preview(&m->cfg);
}

static int slider_thumb_x(const struct mprops *m, enum ctl k)
{
    struct rect r = ctl_rect(k);
    int n = slider_steps(k), v = slider_val(m, k), track = r.w - 11;
    return r.x + (n > 1 ? track * v / (n - 1) : track / 2);
}

static void draw_slider_ctl(const struct mprops *m, struct surface *s, enum ctl k, int focused)
{
    struct rect r = ctl_rect(k);
    struct rect ch = mkrect(r.x, r.y + 8, r.w, 4);
    gfx_bevel(s, ch, BEVEL_SUNKEN);
    gfx_fill(s, ch.x + 2, ch.y + 2, ch.w - 4, ch.h - 4, C_LIGHT);
    int n = slider_steps(k);
    for (int i = 0; i < n; i++) {
        int x = r.x + 5 + (n > 1 ? (r.w - 11) * i / (n - 1) : (r.w - 11) / 2);
        gfx_vline(s, x, r.y + 21, 3, C_BLACK);
    }
    int tx = slider_thumb_x(m, k);
    struct rect th = mkrect(tx, r.y, 11, 20);
    gfx_fill(s, th.x, th.y, th.w, th.h, C_FACE);
    gfx_bevel(s, th, BEVEL_RAISED);
    if (focused)
        gfx_focus_rect(s, mkrect(r.x - 4, r.y - 2, r.w + 8, r.h + 2));
}

/* A two-button mouse with its primary button highlighted in the accent color. */
static void draw_mouse_diagram(struct surface *s, int x, int y, int left_handed)
{
    color_t hi = KINETIK ? theme.wall2 : C_NAVY;
    color_t body = KINETIK ? RGB(0xE8, 0xEF, 0xF6) : C_WHITE;

    /* cord */
    gfx_vline(s, x + 20, y, 5, C_SHADOW);
    gfx_vline(s, x + 21, y, 5, C_BLACK);

    /* body */
    struct rect b = mkrect(x, y + 5, 42, 54);
    gfx_round_fill(s, b, 10, 14, ARGB(255, body));

    /* left and right buttons in the top half */
    struct rect lb = mkrect(x + 2, y + 7, 18, 20);
    struct rect rb = mkrect(x + 22, y + 7, 18, 20);
    gfx_round_fill(s, lb, 8, 2, ARGB(255, left_handed ? body : hi));
    gfx_round_fill(s, rb, 8, 2, ARGB(255, left_handed ? hi : body));

    /* button seam and scroll wheel */
    gfx_hline(s, x + 1, y + 27, 40, C_SHADOW);
    gfx_vline(s, x + 20, y + 6, 21, C_SHADOW);
    gfx_vline(s, x + 21, y + 6, 21, C_SHADOW);
    struct rect wh = mkrect(x + 18, y + 11, 6, 11);
    gfx_round_fill(s, wh, 2, 2, ARGB(255, C_FACE));
    gfx_round_frame(s, wh, 2, 2, ARGB(255, C_BLACK));

    gfx_round_frame(s, b, 10, 14, ARGB(255, C_BLACK));
}

static void mp_paint(struct mprops *m)
{
    if (!m->win)
        return;
    struct surface *s = &m->win->content;
    int win_focused = wm_focused() == m->win;
    char buf[64];

    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);

    /* 1. Button configuration */
    draw_group(s, mkrect(14, 14, MP_W - 28, 82), "Button configuration");
    draw_radio(s, 26, 30, "Right-handed", !m->cfg.left_handed, win_focused && m->focus == C_RIGHT);
    draw_radio(s, 26, 50, "Left-handed", m->cfg.left_handed, win_focused && m->focus == C_LEFT);
    gfx_text(s, &font_sans, 26, 72,
             m->cfg.left_handed ? "Right button clicks; left button opens menus."
                                : "Left button clicks; right button opens menus.",
             C_BLACK);
    draw_mouse_diagram(s, 290, 24, m->cfg.left_handed);

    /* 2. Double-click speed */
    draw_group(s, mkrect(14, 108, MP_W - 28, 84), "Double-click speed");
    gfx_text(s, &font_sans, 26, 126, "Slow", C_BLACK);
    gfx_text(s, &font_sans, 200, 126, "Fast", C_BLACK);
    draw_slider_ctl(m, s, C_DBLCLICK, win_focused && m->focus == C_DBLCLICK);
    snprintf(buf, sizeof(buf), "%u ms", dblclick_times[m->cfg.dblclick]);
    int tw = gfx_text_width(&font_sans, buf);
    gfx_text(s, &font_sans, 26 + (200 - tw) / 2, 171, buf, C_SHADOW);

    struct rect tb = test_box_rect();
    gfx_fill(s, tb.x, tb.y, tb.w, tb.h, C_WHITE);
    gfx_bevel(s, tb, BEVEL_SUNKEN);
    draw_icon(s, tb.x + (tb.w - 32) / 2, tb.y + 6,
              m->test_open ? ICON_FOLDER_OPEN : ICON_FOLDER, 32);
    const char *tlabel = m->test_open ? "Test: Opened!" : "Double-click";
    tw = gfx_text_width(&font_sans, tlabel);
    gfx_text(s, &font_sans, tb.x + (tb.w - tw) / 2, tb.y + 42, tlabel,
             m->test_open ? (KINETIK ? theme.wall2 : C_NAVY) : C_BLACK);

    /* 3. Pointer speed */
    draw_group(s, mkrect(14, 204, MP_W - 28, 68), "Pointer speed");
    draw_icon(s, 28, 224, ICON_MOUSE, 32);
    gfx_text(s, &font_sans, 76, 220, "Slow", C_BLACK);
    gfx_text(s, &font_sans, 310, 220, "Fast", C_BLACK);
    draw_slider_ctl(m, s, C_SPEED, win_focused && m->focus == C_SPEED);

    /* 4. Wheel */
    draw_group(s, mkrect(14, 284, MP_W - 28, 66), "Wheel");
    gfx_text(s, &font_sans, 26, 302, "Lines per notch:", C_BLACK);
    draw_slider_ctl(m, s, C_WHEEL_LINES, win_focused && m->focus == C_WHEEL_LINES);
    snprintf(buf, sizeof(buf), "%d line%s", m->cfg.wheel_lines, m->cfg.wheel_lines == 1 ? "" : "s");
    gfx_text(s, &font_sans, 278, 302, buf, C_BLACK);
    draw_check(s, 26, 326, "Reverse scroll direction (natural scrolling)",
               m->cfg.wheel_reverse, win_focused && m->focus == C_WHEEL_REV);

    /* OK / Cancel / Apply */
    static const char *const blabels[3] = { "OK", "Cancel", "Apply" };
    for (int i = 0; i < 3; i++) {
        enum ctl k = C_OK + i;
        draw_button(s, ctl_rect(k), blabels[i], m->pressed == (int)k && m->pressed_in,
                    k == C_OK && (!IS_BUTTON(m->focus) || m->focus == C_OK),
                    win_focused && m->focus == k);
    }

    wm_damage_client(m->win, mkrect(0, 0, s->w, s->h));
}

/* ---- input ---- */

static void slider_to_mouse(struct mprops *m, enum ctl k, int x)
{
    struct rect r = ctl_rect(k);
    int n = slider_steps(k), track = r.w - 11;
    if (n < 2 || track <= 0)
        return;
    int i = ((x - r.x - 5) * (n - 1) + track / 2) / track;
    set_slider_val(m, k, i);
}

static void activate(struct mprops *m, enum ctl k)
{
    switch (k) {
    case C_RIGHT:
        m->cfg.left_handed = 0;
        break;
    case C_LEFT:
        m->cfg.left_handed = 1;
        break;
    case C_WHEEL_REV:
        m->cfg.wheel_reverse = !m->cfg.wheel_reverse;
        sync_preview(&m->cfg);
        break;
    case C_OK:
        commit_config(&m->cfg);
        wm_destroy(m->win);
        return;
    case C_CANCEL:
        mouse_active_cfg = mouse_saved_cfg;
        mouse_config_changed();
        wm_destroy(m->win);
        return;
    case C_APPLY:
        commit_config(&m->cfg);
        break;
    default:
        break;
    }
    mp_paint(m);
}

static void mp_key(struct window *w, const struct input_event *ev)
{
    struct mprops *m = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    int code = ev->code;
    if (code == KEY_ESC) {
        activate(m, C_CANCEL);
        return;
    }
    if (code == KEY_TAB) {
        int dir = (ev->mods & KMOD_SHIFT) ? C_COUNT - 1 : 1;
        m->focus = (enum ctl)((m->focus + dir) % C_COUNT);
        mp_paint(m);
        return;
    }
    if (code == KEY_ENTER) {
        activate(m, IS_BUTTON(m->focus) ? m->focus : C_OK);
        return;
    }
    if (code == KEY_SPACE) {
        if (m->focus == C_RIGHT || m->focus == C_LEFT || m->focus == C_WHEEL_REV || IS_BUTTON(m->focus))
            activate(m, m->focus);
        return;
    }
    if (code == KEY_LEFT || code == KEY_UP || code == KEY_RIGHT || code == KEY_DOWN) {
        int dir = (code == KEY_LEFT || code == KEY_UP) ? -1 : 1;
        if (m->focus == C_RIGHT || m->focus == C_LEFT) {
            m->cfg.left_handed = dir > 0 ? 1 : 0;
            m->focus = m->cfg.left_handed ? C_LEFT : C_RIGHT;
        } else if (IS_SLIDER(m->focus)) {
            set_slider_val(m, m->focus, slider_val(m, m->focus) + dir);
        } else {
            return;
        }
        mp_paint(m);
    }
}

static void mp_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct mprops *m = w->priv;
    int press = (changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT);
    int release = (changed & MOUSE_LEFT) && !(buttons & MOUSE_LEFT);

    if (m->dragging >= 0) {
        slider_to_mouse(m, (enum ctl)m->dragging, x);
        if (release)
            m->dragging = -1;
        mp_paint(m);
        return;
    }

    if (press) {
        if (rect_contains(test_box_rect(), x, y)) {
            unsigned int now = now_ms();
            if (m->last_test_ms && now - m->last_test_ms < dblclick_times[m->cfg.dblclick]) {
                m->test_open = !m->test_open;
                m->last_test_ms = 0;
            } else {
                m->last_test_ms = now;
            }
            mp_paint(m);
            return;
        }
        for (int k = 0; k < C_COUNT; k++) {
            struct rect r = ctl_rect((enum ctl)k);
            if (IS_SLIDER(k))
                r = mkrect(r.x - 4, r.y - 2, r.w + 8, r.h + 6);
            if (!rect_contains(r, x, y))
                continue;
            m->focus = (enum ctl)k;
            if (IS_BUTTON(k)) {
                m->pressed = k;
                m->pressed_in = 1;
                mp_paint(m);
            } else if (IS_SLIDER(k)) {
                int tx = slider_thumb_x(m, (enum ctl)k);
                if (x >= tx && x < tx + 11) {
                    m->dragging = k;
                } else {
                    set_slider_val(m, (enum ctl)k, slider_val(m, (enum ctl)k) + (x < tx ? -1 : 1));
                    m->dragging = k;
                }
                mp_paint(m);
            } else {
                activate(m, (enum ctl)k);
            }
            return;
        }
    } else if (release && m->pressed >= 0) {
        int k = m->pressed, in = m->pressed_in;
        m->pressed = -1;
        m->pressed_in = 0;
        if (in && rect_contains(ctl_rect((enum ctl)k), x, y))
            activate(m, (enum ctl)k);
        else
            mp_paint(m);
    } else if (m->pressed >= 0) {
        int in = rect_contains(ctl_rect((enum ctl)m->pressed), x, y);
        if (in != m->pressed_in) {
            m->pressed_in = in;
            mp_paint(m);
        }
    }
}

static void mp_focus(struct window *w, int focused)
{
    (void)focused;
    mp_paint(w->priv);
}

static void mp_restyle(struct window *w)
{
    mp_paint(w->priv);
}

static void mp_close(struct window *w)
{
    mouse_active_cfg = mouse_saved_cfg;
    mouse_config_changed();
    wm_destroy(w);
}

static void mp_destroy(struct window *w)
{
    struct mprops *m = w->priv;
    if (dlg == m)
        dlg = NULL;
    free(m);
}

static const struct window_ops mp_ops = {
    .key = mp_key,
    .mouse = mp_mouse,
    .focus = mp_focus,
    .restyle = mp_restyle,
    .close = mp_close,
    .destroy = mp_destroy,
};

void mouse_open(void)
{
    if (dlg) {
        wm_focus(dlg->win);
        return;
    }
    struct mprops *m = calloc(1, sizeof(*m));
    if (!m)
        return;
    m->cfg = mouse_saved_cfg;
    m->focus = mouse_saved_cfg.left_handed ? C_LEFT : C_RIGHT;
    m->pressed = -1;
    m->dragging = -1;
    m->win = wm_create("Mouse Properties", ICON_MOUSE, MP_W, MP_H, 0, &mp_ops, m);
    if (!m->win) {
        free(m);
        return;
    }
    dlg = m;
    wm_center(m->win);
    mp_paint(m);
}

