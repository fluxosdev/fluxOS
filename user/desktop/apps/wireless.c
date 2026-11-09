/*
 * Wireless Networks under Settings (and the wireless icon in the tray,
 * which the desktop shows: wifitray.c): the networks in range, strongest first, joining one (asking for its
 * password when it has one), leaving it, and how the connection is
 * doing. The kernel does the work (SYS_wifi): this only asks it and
 * shows what it says, once a second. A network joined here is
 * remembered in /etc/wifi.conf, as "wifi connect" does, and joined again
 * at startup; Disconnect forgets it.
 *
 * Several access points of one network show as one entry, the strongest:
 * the kernel picks the best of them when it connects anyway. Networks
 * that hide their name are listed but cannot be joined: the kernel's scans
 * do not ask for a network by name, which is what makes those answer.
 */
#include <arpa/inet.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop.h"

#define WL_W 420
#define WL_H 390
#define PAD 12
#define ROW_H 20
#define LIST_Y 84
#define BTN_W 90
#define BTN_H 23
#define MAX_NETS 64

enum { B_REFRESH, B_CONNECT, B_DISCONNECT, B_CLOSE, B_COUNT };
enum { F_LIST = B_COUNT };              /* focus: a button, or the list */
enum { HOLD_NONE, HOLD_BTN, HOLD_ARROW, HOLD_THUMB };

static struct window *wl_win;
static struct wifi_status st;
static int have_card;                   /* the kernel has a wireless card */
static struct wifi_bss_info nets[MAX_NETS];
static int nnets;
/* The selection, kept across refreshes: by name, or a hidden network by address. */
static char sel_ssid[33];
static uint8_t sel_bssid[6];
static int sel_valid;
static int scroll;
static uint32_t seen_scans;             /* scans finished when the list was read */
static int scanning;                    /* we asked for one and it is not over */
static char note[112];                  /* what our last action did */
static int focus = F_LIST;
static int hold, hold_btn, hold_in, grab;
static enum sbar_part sbar_pressed;
static int last_click = -1;
static unsigned int last_click_ms;

/* A connection asked for here: remembered once it works. */
static struct wifi_connect pending;
static int pending_active, pending_started;
static char pending_error[64];          /* the last failure already reported */

/* ---- the kernel's view ---- */

static void read_status(void)
{
    if (wifi(WIFI_STATUS, &st) < 0)
        memset(&st, 0, sizeof(st));
    have_card = st.state != WIFI_ST_NONE;
}

static int is_connected(const struct wifi_bss_info *b)
{
    return st.state == WIFI_ST_CONNECTED && b->ssid_len && !strcmp(b->ssid, st.ssid);
}

static int stronger_first(const void *a, const void *b)
{
    const struct wifi_bss_info *x = a, *y = b;
    int xc = is_connected(x), yc = is_connected(y);
    if (xc != yc)
        return yc - xc;                 /* the network we are on comes first */
    return y->rssi - x->rssi;
}

/* The scan results, one entry a network (its strongest access point). */
static void read_networks(void)
{
    nnets = 0;
    for (uint32_t i = 0; nnets < MAX_NETS; i++) {
        struct wifi_bss_info b;
        b.index = i;
        if (wifi(WIFI_BSS, &b) < 0)
            break;
        int dup = -1;
        for (int k = 0; k < nnets && b.ssid_len; k++)
            if (nets[k].ssid_len == b.ssid_len && !strcmp(nets[k].ssid, b.ssid))
                dup = k;
        if (dup < 0)
            nets[nnets++] = b;
        else if (b.rssi > nets[dup].rssi)
            nets[dup] = b;
    }
    qsort(nets, (size_t)nnets, sizeof(nets[0]), stronger_first);
}

static int selected(void)
{
    for (int i = 0; sel_valid && i < nnets; i++) {
        if (sel_ssid[0] ? nets[i].ssid_len && !strcmp(nets[i].ssid, sel_ssid)
                        : !nets[i].ssid_len && !memcmp(nets[i].bssid, sel_bssid, 6))
            return i;
    }
    return -1;
}

static void select_index(int i)
{
    sel_valid = i >= 0 && i < nnets;
    if (!sel_valid)
        return;
    snprintf(sel_ssid, sizeof(sel_ssid), "%s", nets[i].ssid_len ? nets[i].ssid : "");
    memcpy(sel_bssid, nets[i].bssid, 6);
}

/* ---- layout ---- */

static struct rect list_rect(void)
{
    return mkrect(PAD, LIST_Y, WL_W - 2 * PAD - SBAR_W, WL_H - LIST_Y - PAD - BTN_H - 36);
}

static int visible_rows(void)
{
    int n = (list_rect().h - 4) / ROW_H;
    return n < 1 ? 1 : n;
}

static struct sbar list_bar(void)
{
    struct rect l = list_rect();
    return (struct sbar){ mkrect(l.x + l.w, l.y, SBAR_W, l.h), 1, scroll, visible_rows(), nnets };
}

static void clamp_scroll(void)
{
    struct sbar b = list_bar();
    int max = sbar_max(&b);
    if (scroll > max)
        scroll = max;
    if (scroll < 0)
        scroll = 0;
}

static void scroll_to_selection(void)
{
    int i = selected(), vis = visible_rows();
    if (i < 0)
        return;
    if (i < scroll)
        scroll = i;
    else if (i >= scroll + vis)
        scroll = i - vis + 1;
    clamp_scroll();
}

static struct rect button_rect(int b)
{
    int y = WL_H - PAD - BTN_H;
    if (b == B_CLOSE)
        return mkrect(WL_W - PAD - 75, y, 75, BTN_H);
    return mkrect(PAD + b * (BTN_W + 6), y, BTN_W, BTN_H);
}

static int button_at(int x, int y)
{
    for (int b = 0; b < B_COUNT; b++)
        if (rect_contains(button_rect(b), x, y))
            return b;
    return -1;
}

/* ---- drawing ---- */

static int bars_for(int rssi)
{
    return rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
}

static void draw_bars(struct surface *s, int x, int y, int rssi, color_t on, color_t off)
{
    int n = bars_for(rssi);
    for (int i = 0; i < 4; i++) {
        int h = 4 + 3 * i;
        gfx_fill(s, x + i * 4, y + 13 - h, 3, h, i < n ? on : off);
    }
}

static const char *security_name(int sec)
{
    return sec == WIFI_SEC_OPEN ? "Open" : sec == WIFI_SEC_WPA2_PSK ? "WPA2" : "Not supported";
}

static void text_in(struct surface *s, const struct pfont *f, int x, int y, int w, const char *str,
                    color_t c)
{
    char buf[128];
    gfx_ellipsize(f, str, w, buf, sizeof(buf));
    gfx_text(s, f, x, y, buf, c);
}

static void draw_header(struct surface *s)
{
    char line1[96], line2[128];
    line2[0] = 0;
    switch (st.state) {
    case WIFI_ST_NONE:
        snprintf(line1, sizeof(line1), "No wireless card was found.");
        break;
    case WIFI_ST_STARTING:
        snprintf(line1, sizeof(line1), "The wireless card is starting...");
        break;
    case WIFI_ST_FAILED:
        snprintf(line1, sizeof(line1), "The wireless card could not be started.");
        snprintf(line2, sizeof(line2), "%s", st.error);
        break;
    case WIFI_ST_CONNECTED: {
        snprintf(line1, sizeof(line1), "Connected to %s", st.ssid);
        struct netinfo ni;
        char addr[32] = "waiting for an address";
        if (netinfo(&ni) == 0 && ni.addr) {
            struct in_addr a = { .s_addr = htonl(ni.addr) };
            snprintf(addr, sizeof(addr), "address %s", inet_ntoa(a));
        }
        snprintf(line2, sizeof(line2), "%d dBm, channel %u (%u MHz), %s; %s", st.rssi, st.channel,
                 st.freq, security_name(st.security), addr);
        break;
    }
    case WIFI_ST_CONNECTING:
        snprintf(line1, sizeof(line1), "Connecting to %s...", st.ssid);
        if (st.error[0])
            snprintf(line2, sizeof(line2), "Last try: %s", st.error);
        break;
    default:
        snprintf(line1, sizeof(line1), "Not connected");
        if (st.error[0])
            snprintf(line2, sizeof(line2), "%s", st.error);
        break;
    }
    draw_icon(s, PAD, PAD, ICON_WIFI, 32);
    int tx = PAD + 32 + 10, tw = WL_W - tx - PAD;
    text_in(s, &font_sans_bold, tx, PAD + 1, tw, line1, C_BLACK);
    if (line2[0])
        text_in(s, &font_sans, tx, PAD + 19, tw, line2, C_BLACK);
    draw_separator(s, PAD, 56, WL_W - 2 * PAD);
    if (have_card)
        gfx_text(s, &font_sans, PAD, 64, "Networks in range:", C_BLACK);
}

static void draw_list(struct surface *s)
{
    struct rect l = list_rect();
    gfx_fill(s, l.x, l.y, l.w, l.h, C_WHITE);
    gfx_bevel(s, mkrect(l.x, l.y, l.w + SBAR_W, l.h), BEVEL_SUNKEN);
    struct sbar b = list_bar();
    sbar_draw(s, &b, hold == HOLD_ARROW || hold == HOLD_THUMB ? sbar_pressed : SBAR_NONE);

    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, mkrect(l.x + 2, l.y + 2, l.w - 4, l.h - 4));
    int sel = selected(), vis = visible_rows();
    for (int k = 0; k < vis && scroll + k < nnets; k++) {
        const struct wifi_bss_info *n = &nets[scroll + k];
        int y = l.y + 2 + k * ROW_H, ty = y + (ROW_H - font_sans.height) / 2;
        int is_sel = scroll + k == sel, conn = is_connected(n);
        if (is_sel)
            gfx_fill(s, l.x + 2, y, l.w - 4, ROW_H, C_NAVY);
        color_t fg = is_sel ? C_WHITE : n->security == WIFI_SEC_OTHER ? C_SHADOW : C_BLACK;
        draw_bars(s, l.x + 6, y + 3, n->rssi, is_sel ? C_WHITE : C_BLACK,
                  is_sel ? RGB(0x60, 0x60, 0xA0) : RGB(0xC8, 0xC8, 0xC8));
        const char *right = conn ? "Connected" : security_name(n->security);
        const struct pfont *f = conn ? &font_sans_bold : &font_sans;
        int rw = gfx_text_width(f, right);
        gfx_text(s, f, l.x + l.w - 6 - rw, ty, right, fg);
        text_in(s, f, l.x + 28, ty, l.w - 28 - rw - 16, n->ssid_len ? n->ssid : "(hidden network)", fg);
        if (is_sel && focus == F_LIST && hold == HOLD_NONE)
            gfx_focus_rect(s, mkrect(l.x + 2, y, l.w - 4, ROW_H));
    }
    s->clip = saved;
    const char *empty = !have_card || st.state == WIFI_ST_STARTING || st.state == WIFI_ST_FAILED ? ""
                      : scanning ? "Searching for networks..."
                      : "No networks found. Click Refresh to look again.";
    if (!nnets && empty[0])
        gfx_text(s, &font_sans, l.x + 8, l.y + 6, empty, C_SHADOW);
}

static void paint(void)
{
    if (!wl_win)
        return;
    struct surface *s = &wl_win->content;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);
    draw_header(s);
    draw_list(s);
    const char *msg = note[0] ? note : scanning && nnets ? "Searching for networks..." : "";
    text_in(s, &font_sans, PAD, WL_H - PAD - BTN_H - 24, WL_W - 2 * PAD, msg, C_BLACK);

    static const char *const labels[B_COUNT] = { "Refresh", "Connect...", "Disconnect", "Close" };
    for (int b = 0; b < B_COUNT; b++)
        draw_button(s, button_rect(b), labels[b], hold == HOLD_BTN && hold_btn == b && hold_in,
                    b == (focus == F_LIST ? B_CONNECT : focus), focus == b);
    wm_damage_client(wl_win, mkrect(0, 0, s->w, s->h));
}

/* ---- actions ---- */

static void refresh(void)
{
    if (!have_card || st.state == WIFI_ST_FAILED) {
        snprintf(note, sizeof(note), "There is no wireless card to search with.");
        return;
    }
    if (wifi(WIFI_SCAN, NULL) < 0) {
        snprintf(note, sizeof(note), "The search could not be started.");
        return;
    }
    scanning = 1;
    note[0] = 0;
}

static void forget_pending(void)
{
    memset(&pending, 0, sizeof(pending));
    pending_active = pending_started = 0;
}

static void start_connect(void)
{
    if (wifi(WIFI_CONNECT, &pending) < 0) {
        forget_pending();
        message_box("Wireless Networks", ICON_WARNING, "The wireless card would not start connecting.");
        return;
    }
    pending_active = 1;
    pending_started = 0;
    pending_error[0] = 0;
    snprintf(note, sizeof(note), "Connecting to %.*s...", pending.ssid_len, pending.ssid);
    paint();
}

static void password_done(void *ctx, const char *text)
{
    (void)ctx;
    if (wifi_make_psk(pending.ssid, pending.ssid_len, text, pending.psk) < 0) {
        forget_pending();
        message_box("Wireless Networks", ICON_WARNING,
                    "A WPA2 password has 8 to 63 characters (or is the key itself, as 64 hex digits).");
        return;
    }
    pending.has_psk = 1;
    start_connect();
}

static void cancelled(void *ctx)
{
    (void)ctx;
    forget_pending();
}

static void ask_password_for(int security)
{
    if (security == WIFI_SEC_OPEN) {
        start_connect();
        return;
    }
    char title[64], label[80];
    snprintf(title, sizeof(title), "Connect to %.*s", pending.ssid_len, pending.ssid);
    snprintf(label, sizeof(label), "Password for %.*s:", pending.ssid_len, pending.ssid);
    if (!ask_password(title, label, password_done, cancelled, NULL))
        forget_pending();               /* another prompt is open */
}

static void connect_selected(void)
{
    if (!have_card || st.state == WIFI_ST_FAILED) {
        snprintf(note, sizeof(note), "There is no wireless card to connect with.");
        return;
    }
    int i = selected();
    if (i < 0) {
        snprintf(note, sizeof(note), "Choose a network to connect to.");
        return;
    }
    const struct wifi_bss_info *n = &nets[i];
    if (n->security == WIFI_SEC_OTHER) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "%s uses security fluxOS does not support (WPA3, WPA2-Enterprise, WEP or WPA).",
                 n->ssid_len ? n->ssid : "This network");
        message_box("Wireless Networks", ICON_WARNING, msg);
        return;
    }
    if (is_connected(n)) {
        snprintf(note, sizeof(note), "You are already connected to %s.", n->ssid);
        return;
    }
    if (!n->ssid_len) {
        message_box("Wireless Networks", ICON_WARNING,
                    "This network hides its name, and fluxOS cannot join hidden networks yet.");
        return;
    }
    forget_pending();
    memcpy(pending.ssid, n->ssid, n->ssid_len);
    pending.ssid_len = n->ssid_len;
    ask_password_for(n->security);
}

static void disconnect(void)
{
    if (st.state != WIFI_ST_CONNECTED && st.state != WIFI_ST_CONNECTING) {
        snprintf(note, sizeof(note), "You are not connected to a wireless network.");
        return;
    }
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s", st.ssid);
    forget_pending();
    if (wifi(WIFI_DISCONNECT, NULL) < 0) {
        snprintf(note, sizeof(note), "The wireless card would not disconnect.");
        return;
    }
    struct wifi_connect saved;
    if (wifi_conf_load(&saved) == 0 && saved.ssid_len == strlen(ssid) &&
        !memcmp(saved.ssid, ssid, saved.ssid_len)) {
        wifi_conf_forget();
        snprintf(note, sizeof(note), "Disconnected from %s. It will not be joined at startup.", ssid);
    } else {
        snprintf(note, sizeof(note), "Disconnected from %s.", ssid);
    }
    memset(&saved, 0, sizeof(saved));
}

static void activate(int b)
{
    switch (b) {
    case B_REFRESH: refresh(); break;
    case B_CONNECT: connect_selected(); break;
    case B_DISCONNECT: disconnect(); break;
    case B_CLOSE:
        wm_close(wl_win);
        return;
    }
    paint();
}

/* A connection asked for here: remember it once it works, say why if not. */
static void follow_pending(void)
{
    if (!pending_active)
        return;
    int ours = strlen(st.ssid) == pending.ssid_len && !memcmp(st.ssid, pending.ssid, pending.ssid_len);
    if (st.state == WIFI_ST_CONNECTING && ours)
        pending_started = 1;
    if (st.state == WIFI_ST_CONNECTED && ours) {
        if (wifi_conf_save(&pending) < 0)
            snprintf(note, sizeof(note), "Connected, but it could not be remembered for startup.");
        else
            snprintf(note, sizeof(note), "Connected to %.*s.", pending.ssid_len, pending.ssid);
        forget_pending();
        return;
    }
    if (!pending_started)
        return;                         /* the kernel has not taken the request yet */
    if (st.error[0] && strcmp(st.error, pending_error)) {
        snprintf(pending_error, sizeof(pending_error), "%s", st.error);
        snprintf(note, sizeof(note), "Trying again...");    /* the reason is in the header */
    }
    if (st.state == WIFI_ST_IDLE) {     /* it gave up */
        snprintf(note, sizeof(note), "Could not connect to %.*s.%s", pending.ssid_len, pending.ssid,
                 strstr(st.error, "password") ? " Check the password and try again." : "");
        forget_pending();
    }
}

/* ---- input ---- */

static void move_selection(int delta)
{
    if (!nnets)
        return;
    int i = selected();
    i = i < 0 ? (delta > 0 ? 0 : nnets - 1) : i + delta;
    if (i < 0)
        i = 0;
    if (i >= nnets)
        i = nnets - 1;
    select_index(i);
    scroll_to_selection();
}

static void wl_key(struct window *w, const struct input_event *ev)
{
    (void)w;
    if (!(ev->flags & IEV_PRESSED))
        return;
    switch (ev->code) {
    case KEY_ESC:
        wm_close(wl_win);
        return;
    case KEY_F5:
        refresh();
        break;
    case KEY_TAB:
        focus = (focus + ((ev->mods & KMOD_SHIFT) ? F_LIST : 1)) % (F_LIST + 1);
        break;
    case KEY_UP: focus = F_LIST; move_selection(-1); break;
    case KEY_DOWN: focus = F_LIST; move_selection(1); break;
    case KEY_PGUP: focus = F_LIST; move_selection(-(visible_rows() - 1)); break;
    case KEY_PGDN: focus = F_LIST; move_selection(visible_rows() - 1); break;
    case KEY_HOME: focus = F_LIST; move_selection(-MAX_NETS); break;
    case KEY_END: focus = F_LIST; move_selection(MAX_NETS); break;
    case KEY_ENTER:
    case KEY_SPACE:
        activate(focus == F_LIST ? B_CONNECT : focus);
        return;
    default:
        return;
    }
    paint();
}

static void scroll_step(const struct sbar *b, enum sbar_part part)
{
    switch (part) {
    case SBAR_ARROW1: scroll -= 1; break;
    case SBAR_ARROW2: scroll += 1; break;
    case SBAR_PAGE1: scroll -= b->page > 1 ? b->page - 1 : 1; break;
    case SBAR_PAGE2: scroll += b->page > 1 ? b->page - 1 : 1; break;
    default: break;
    }
    clamp_scroll();
}

static void wl_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    (void)w;
    int left = buttons & MOUSE_LEFT;
    int pressed = (changed & MOUSE_LEFT) && left, released = (changed & MOUSE_LEFT) && !left;
    struct sbar bar = list_bar();
    struct rect l = list_rect();

    if (pressed) {
        int b = button_at(x, y);
        enum sbar_part part = sbar_hit(&bar, x, y);
        if (b >= 0) {
            hold = HOLD_BTN;
            hold_btn = focus = b;
            hold_in = 1;
        } else if (part != SBAR_NONE) {
            sbar_pressed = part;
            if (part == SBAR_THUMB) {
                hold = HOLD_THUMB;
                grab = y - sbar_thumb(&bar).y;
            } else {
                hold = HOLD_ARROW;
                scroll_step(&bar, part);
            }
        } else if (rect_contains(l, x, y)) {
            int i = scroll + (y - l.y - 2) / ROW_H;
            unsigned int now = now_ms();
            int dbl = i == last_click && now - last_click_ms < DBLCLICK_MS;
            focus = F_LIST;
            select_index(i < nnets ? i : -1);
            last_click = i;
            last_click_ms = now;
            if (dbl && i < nnets) {
                connect_selected();
                last_click = -1;
            }
        } else {
            return;
        }
    } else if (released) {
        int h = hold, b = hold_btn, in = hold_in;
        hold = HOLD_NONE;
        if (h == HOLD_BTN && in) {
            activate(b);
            return;
        }
    } else if (hold == HOLD_BTN && left) {
        int in = button_at(x, y) == hold_btn;
        if (in == hold_in)
            return;
        hold_in = in;
    } else if (hold == HOLD_THUMB && left) {
        scroll = sbar_drag(&bar, y, grab);
        clamp_scroll();
    } else {
        return;
    }
    paint();
}

static void wl_wheel(struct window *w, int steps)
{
    (void)w;
    scroll -= steps * mouse_wheel_lines();
    clamp_scroll();
    paint();
}

static void wl_focus(struct window *w, int focused)
{
    (void)w;
    (void)focused;
    paint();
}

static void wl_restyle(struct window *w)
{
    (void)w;
    paint();
}

static void wl_close(struct window *w)
{
    wm_destroy(w);
}

static void wl_destroy(struct window *w)
{
    (void)w;
    wl_win = NULL;
}

static const struct window_ops wl_ops = {
    .key = wl_key,
    .mouse = wl_mouse,
    .wheel = wl_wheel,
    .focus = wl_focus,
    .restyle = wl_restyle,
    .close = wl_close,
    .destroy = wl_destroy,
};

/* ---- the once-a-second look ---- */

/* A network being joined is remembered once it is, even after the window closes. */
int wireless_busy(void)
{
    return pending_active;
}

void wireless_tick(void)
{
    read_status();
    follow_pending();                   /* even with the window closed: remember what was joined */
    if (!wl_win)
        return;
    if (st.scans != seen_scans) {
        seen_scans = st.scans;
        scanning = 0;
    }
    int had = nnets;
    read_networks();
    if (!had && nnets && selected() < 0)
        select_index(0);                /* the first results: something to press Connect on */
    clamp_scroll();
    if (!wl_win->minimized)
        paint();
}

void wireless_open(void)
{
    if (wl_win) {
        wm_focus(wl_win);
        return;
    }
    hold = HOLD_NONE;
    focus = F_LIST;
    note[0] = 0;
    scroll = 0;
    read_status();
    seen_scans = st.scans;
    read_networks();
    if (selected() < 0 && nnets)
        select_index(0);
    wl_win = wm_create("Wireless Networks", ICON_WIFI, WL_W, WL_H, WF_MINIMIZE, &wl_ops, NULL);
    if (!wl_win)
        return;
    wm_center(wl_win);
    if (have_card && (st.state == WIFI_ST_IDLE || st.state == WIFI_ST_CONNECTED))
        refresh();                      /* fresh results to show */
    paint();
}
