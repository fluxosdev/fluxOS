/*
 * Date and Time under Settings: the current time, the time zone (the clock
 * itself runs on UTC), setting the clock by hand, and synchronizing it with
 * a network time server over SNTP. The zone is kept in GRUB's environment
 * block when fluxOS runs from a disk.
 */
#include <arpa/inet.h>
#include <flux.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "desktop.h"

#define DT_W 330
#define DT_H 232
#define NTP_PORT 123
#define NTP_UNIX_DELTA 2208988800u      /* seconds from 1900 to 1970 */
#define NTP_TIMEOUT 2000

static const char *const ntp_servers[] = { "pool.ntp.org", "time.cloudflare.com", "time.google.com" };
#define NSERVERS ((int)(sizeof(ntp_servers) / sizeof(ntp_servers[0])))

static const struct { int minutes; const char *name; } zones[] = {
    { -720, "UTC-12:00" }, { -660, "UTC-11:00 Samoa" }, { -600, "UTC-10:00 Hawaii" },
    { -540, "UTC-09:00 Alaska" }, { -480, "UTC-08:00 Pacific" }, { -420, "UTC-07:00 Mountain" },
    { -360, "UTC-06:00 Central" }, { -300, "UTC-05:00 Eastern" }, { -240, "UTC-04:00 Atlantic" },
    { -210, "UTC-03:30 Newfoundland" }, { -180, "UTC-03:00 Brasilia" }, { -120, "UTC-02:00" },
    { -60, "UTC-01:00 Azores" }, { 0, "UTC+00:00 London, UTC" }, { 60, "UTC+01:00 Berlin, Paris" },
    { 120, "UTC+02:00 Athens, Cairo" }, { 180, "UTC+03:00 Moscow" }, { 210, "UTC+03:30 Tehran" },
    { 240, "UTC+04:00 Dubai" }, { 270, "UTC+04:30 Kabul" }, { 300, "UTC+05:00 Karachi" },
    { 330, "UTC+05:30 India" }, { 345, "UTC+05:45 Kathmandu" }, { 360, "UTC+06:00 Dhaka" },
    { 420, "UTC+07:00 Bangkok" }, { 480, "UTC+08:00 Beijing, Perth" }, { 540, "UTC+09:00 Tokyo" },
    { 570, "UTC+09:30 Adelaide" }, { 600, "UTC+10:00 Sydney" }, { 660, "UTC+11:00 Solomon Is." },
    { 720, "UTC+12:00 Auckland" }, { 780, "UTC+13:00 Tonga" },
};
#define NZONES ((int)(sizeof(zones) / sizeof(zones[0])))

enum { B_ZPREV, B_ZNEXT, B_SYNC, B_SET, B_CLOSE, B_COUNT };

static struct window *dt_win;
static int zone;                        /* index into zones[] */
static int focus = B_SYNC, pressed = -1, pressed_in;
static char status[80] = "";

static const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
    "November", "December",
};
static const char *const day_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static struct rect button_rect(int b)
{
    switch (b) {
    case B_ZPREV: return mkrect(14, 98, 24, 21);
    case B_ZNEXT: return mkrect(DT_W - 14 - 24, 98, 24, 21);
    case B_SYNC: return mkrect(14, 146, 140, 23);
    case B_SET: return mkrect(DT_W - 14 - 140, 146, 140, 23);
    default: return mkrect(DT_W - 14 - 75, DT_H - 12 - 23, 75, 23);
    }
}

static void paint(void)
{
    if (!dt_win)
        return;
    struct surface *s = &dt_win->content;
    char buf[96];
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    gfx_text(s, &font_sans_bold, 14, 12, "Current date and time", C_BLACK);
    snprintf(buf, sizeof(buf), "%s, %d %s %d", day_names[tm.tm_wday], tm.tm_mday, month_names[tm.tm_mon],
             tm.tm_year + 1900);
    gfx_text(s, &font_sans, 14, 34, buf, C_BLACK);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    gfx_text(s, &font_sans_bold, 14, 54, buf, C_BLACK);

    gfx_text(s, &font_sans, 14, 80, "Time zone:", C_BLACK);
    struct rect z = mkrect(14 + 24 + 4, 98, DT_W - 2 * (14 + 24 + 4), 21);
    gfx_fill(s, z.x, z.y, z.w, z.h, C_WHITE);
    gfx_bevel(s, z, BEVEL_SUNKEN);
    gfx_text(s, &font_sans, z.x + 6, z.y + (z.h - font_sans.height) / 2, zones[zone].name, C_BLACK);

    gfx_text(s, &font_sans, 14, 128, "Time server:", C_BLACK);
    gfx_text(s, &font_sans, 100, 128, ntp_servers[0], C_BLACK);
    gfx_text_wrap(s, &font_sans, 14, 180, DT_W - 28, status, C_BLACK);

    static const char *const labels[B_COUNT] = { "<", ">", "Synchronize Now", "Set Manually...", "Close" };
    for (int b = 0; b < B_COUNT; b++)
        draw_button(s, button_rect(b), labels[b], pressed == b && pressed_in, focus == b && b == B_CLOSE,
                    focus == b);
    wm_damage_client(dt_win, mkrect(0, 0, s->w, s->h));
}

static void save_zone(void)
{
    char dir[128], value[16];
    if (!grubenv_boot_dir(dir, sizeof(dir)))
        return;
    snprintf(value, sizeof(value), "%d", zones[zone].minutes);
    const char *keys[1] = { "fluxos_tz" }, *vals[1] = { value };
    grubenv_update(dir, keys, vals, 1);
}

static void set_zone(int i)
{
    if (i < 0 || i >= NZONES)
        return;
    zone = i;
    tz_set(zones[zone].minutes);
    save_zone();
    datetime_zone_changed(zones[zone].minutes);     /* the desktop's clock, and every program's */
}

/* The zone in use, which the desktop said (UTC if it is not in the list). */
static void find_zone(void)
{
    int minutes = tz_get(), utc = 0;
    zone = -1;
    for (int i = 0; i < NZONES; i++) {
        if (zones[i].minutes == minutes && zone < 0)
            zone = i;
        if (!zones[i].minutes)
            utc = i;
    }
    if (zone < 0)
        zone = utc;
}

/* ---- SNTP ---- */

/* Ask one server; returns the Unix time, or 0 if it did not answer. */
static uint32_t ntp_query(const char *host)
{
    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list[0])
        return 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(NTP_PORT) };
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    uint32_t result = 0;
    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x23;                      /* version 4, client */
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        for (int tries = 0; tries < 2 && !result; tries++) {
            if (send(fd, pkt, sizeof(pkt), 0) < 0)
                break;
            unsigned int deadline = uptime_ms() + NTP_TIMEOUT;
            for (;;) {
                int left = (int)(deadline - uptime_ms());
                struct pollfd pfd = { fd, POLLIN, 0 };
                if (left <= 0 || poll(&pfd, 1, left) <= 0)
                    break;
                uint8_t reply[128];
                int r = (int)recv(fd, reply, sizeof(reply), 0);
                if (r < 0)
                    break;
                if (r >= 48 && (reply[0] & 7) == 4 && reply[1] != 0 && (reply[0] >> 6) != 3) {
                    uint32_t secs = (uint32_t)reply[40] << 24 | (uint32_t)reply[41] << 16 |
                                    (uint32_t)reply[42] << 8 | reply[43];
                    if (secs > NTP_UNIX_DELTA) {
                        result = secs - NTP_UNIX_DELTA;
                        break;
                    }
                }
            }
        }
    }
    close(fd);
    return result;
}

static void sync_now(void)
{
    struct netinfo ni;
    if (netinfo(&ni) < 0 || !ni.addr) {
        snprintf(status, sizeof(status), "There is no network connection.");
        return;
    }
    snprintf(status, sizeof(status), "Synchronizing...");
    paint();
    wm_flush();                         /* the lookup blocks for a moment */
    for (int i = 0; i < NSERVERS; i++) {
        uint32_t now = ntp_query(ntp_servers[i]);
        if (now) {
            time_t t = (time_t)now;
            stime(&t);
            snprintf(status, sizeof(status), "Synchronized with %s.", ntp_servers[i]);
            return;
        }
    }
    snprintf(status, sizeof(status), "No time server answered. Check the network connection.");
}

/* ---- setting by hand ---- */

static int days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* "YYYY-MM-DD HH:MM[:SS]" */
static int parse_datetime(const char *p, int v[6])
{
    static const char seps[6] = { '-', '-', ' ', ':', ':', 0 };
    v[5] = 0;
    for (int i = 0; i < 6; i++) {
        if (i == 5 && !*p)
            return 0;
        if (*p < '0' || *p > '9')
            return -1;
        int n = 0;
        while (*p >= '0' && *p <= '9' && n < 10000)
            n = n * 10 + (*p++ - '0');
        v[i] = n;
        if (i < 5 && *p++ != seps[i])
            return -1;
    }
    return *p ? -1 : 0;
}

static void manual_done(void *ctx, const char *text)
{
    (void)ctx;
    int v[6];
    if (parse_datetime(text, v) < 0 || v[0] < 2000 || v[0] > 2099 || v[1] < 1 || v[1] > 12 || v[2] < 1 ||
        v[2] > 31 || v[3] > 23 || v[4] > 59 || v[5] > 59) {
        message_box("Date and Time", ICON_WARNING, "Type the date and time as YYYY-MM-DD HH:MM:SS.");
        return;
    }
    long local = (long)days_from_civil(v[0], v[1], v[2]) * 86400L + v[3] * 3600L + v[4] * 60L + v[5];
    time_t t = (time_t)(local - zones[zone].minutes * 60L);
    stime(&t);
    snprintf(status, sizeof(status), "The clock was set by hand.");
    paint();
}

static void activate(int b)
{
    switch (b) {
    case B_ZPREV: set_zone(zone - 1); break;
    case B_ZNEXT: set_zone(zone + 1); break;
    case B_SYNC: sync_now(); break;
    case B_SET: {
        time_t t = time(NULL);
        struct tm tm = *localtime(&t);
        char initial[32];
        snprintf(initial, sizeof(initial), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        ask_text("Set Date and Time", "Local time (YYYY-MM-DD HH:MM:SS):", initial, manual_done, NULL);
        break;
    }
    case B_CLOSE:
        wm_close(dt_win);
        return;
    }
    paint();
}

static void dt_key(struct window *w, const struct input_event *ev)
{
    (void)w;
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (ev->code == KEY_ESC) {
        wm_close(dt_win);
        return;
    }
    if (ev->code == KEY_TAB) {
        focus = (focus + ((ev->mods & KMOD_SHIFT) ? B_COUNT - 1 : 1)) % B_COUNT;
    } else if (ev->code == KEY_LEFT || ev->code == KEY_UP) {
        set_zone(zone - 1);
    } else if (ev->code == KEY_RIGHT || ev->code == KEY_DOWN) {
        set_zone(zone + 1);
    } else if (ev->code == KEY_ENTER || ev->code == KEY_SPACE) {
        activate(focus);
        return;
    } else {
        return;
    }
    paint();
}

static void dt_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    (void)w;
    int press = (changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT);
    int release = (changed & MOUSE_LEFT) && !(buttons & MOUSE_LEFT);
    if (press) {
        for (int b = 0; b < B_COUNT; b++) {
            if (rect_contains(button_rect(b), x, y)) {
                pressed = focus = b;
                pressed_in = 1;
                paint();
                return;
            }
        }
    } else if (release && pressed >= 0) {
        int b = pressed, in = pressed_in;
        pressed = -1;
        pressed_in = 0;
        if (in && rect_contains(button_rect(b), x, y))
            activate(b);
        else
            paint();
    } else if (pressed >= 0) {
        int in = rect_contains(button_rect(pressed), x, y);
        if (in != pressed_in) {
            pressed_in = in;
            paint();
        }
    }
}

static void dt_focus(struct window *w, int focused)
{
    (void)w;
    (void)focused;
    paint();
}

static void dt_restyle(struct window *w)
{
    (void)w;
    paint();
}

static void dt_close(struct window *w)
{
    wm_destroy(w);
}

static void dt_destroy(struct window *w)
{
    (void)w;
    dt_win = NULL;
}

static const struct window_ops dt_ops = {
    .key = dt_key,
    .mouse = dt_mouse,
    .focus = dt_focus,
    .restyle = dt_restyle,
    .close = dt_close,
    .destroy = dt_destroy,
};

void datetime_tick(void)
{
    if (dt_win && !dt_win->minimized)
        paint();
}

void datetime_open(void)
{
    if (dt_win) {
        wm_focus(dt_win);
        return;
    }
    pressed = -1;
    focus = B_SYNC;
    status[0] = 0;
    find_zone();
    dt_win = wm_create("Date and Time Properties", ICON_DATETIME, DT_W, DT_H, 0, &dt_ops, NULL);
    if (!dt_win)
        return;
    wm_center(dt_win);
    paint();
}
