/*
 * Sound on the desktop: the startup sound, the terminals' bell, the
 * speaker in the taskbar's tray, and the Volume Control window it opens.
 *
 * Volume Control has a column for each of the Sound Blaster 16 mixer's
 * outputs (master, wave, FM synth, CD, line in and the PC speaker), each
 * with a vertical slider that sets both sides and a Mute box. The card
 * has no mute switches of its own, so muting turns the volume down to 0
 * and unmuting puts it back. The window follows changes made elsewhere
 * (the mixer command, say) once a second.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <unistd.h>
#include "desktop.h"

#define STARTUP_SOUND "/usr/share/sounds/startup.wav"
#define BELL_GAP_MS   150               /* at most one bell this often */

#define COL_W    86
#define VC_H     252
#define TRACK_Y  60
#define TRACK_H  132
#define THUMB_W  22
#define THUMB_H  11
#define MUTE_Y   216
#define STEP     5

static const struct {
    int channel;
    const char *title;
} columns[] = {
    { MIXER_MASTER, "Volume Control" }, { MIXER_PCM, "Wave" }, { MIXER_SYNTH, "FM Synth" },
    { MIXER_CD, "CD Audio" }, { MIXER_LINE, "Line In" }, { MIXER_SPEAKER, "PC Speaker" },
};
#define NCOLS ((int)(sizeof(columns) / sizeof(columns[0])))

static int have_mixer, have_dsp;
static int level[NCOLS];                /* 0-100, both sides */
static int muted[NCOLS], saved[NCOLS];  /* muted: the level to go back to */
static unsigned int last_bell;

struct vc {
    struct window *win;
    int focus;                          /* column */
    int dragging;                       /* column whose thumb is held, or -1 */
    int grab;                           /* where in the thumb it was taken */
};

static struct vc *vc_win;

/* ---- the mixer ---- */

static int mixer_get(int channel, struct mixer_level *ml)
{
    int fd = open("/dev/mixer", O_RDONLY);
    if (fd < 0)
        return -1;
    ml->channel = (uint32_t)channel;
    int r = ioctl(fd, MIXER_READ, ml);
    close(fd);
    return r;
}

static void mixer_set(int channel, int value)
{
    int fd = open("/dev/mixer", O_RDONLY);
    if (fd < 0)
        return;
    struct mixer_level ml = { (uint32_t)channel, (uint32_t)value, (uint32_t)value };
    ioctl(fd, MIXER_WRITE, &ml);
    close(fd);
}

/* Read every level; returns 1 if any changed. */
static int read_levels(void)
{
    int changed = 0;
    for (int i = 0; i < NCOLS; i++) {
        struct mixer_level ml;
        if (mixer_get(columns[i].channel, &ml) < 0)
            continue;
        int v = (int)(ml.left > ml.right ? ml.left : ml.right);
        if (v != level[i]) {
            level[i] = v;
            changed = 1;
        }
        if (muted[i] && v != 0)
            muted[i] = 0;               /* someone turned it up meanwhile */
    }
    return changed;
}

/* ---- the tray, the bell and the startup sound ---- */

enum icon_id sound_tray_icon(void)
{
    if (!have_mixer)
        return ICON_NONE;
    return muted[0] || level[0] == 0 ? ICON_MUTED : ICON_VOLUME;
}

void sound_beep(void)
{
    unsigned int now = now_ms();
    if (now - last_bell < BELL_GAP_MS)
        return;
    last_bell = now;
    int fd = open("/dev/speaker", O_WRONLY);
    if (fd < 0)
        return;
    struct spkr_tone t = { 750, 100 };
    ioctl(fd, SPKR_TONE, &t);
    close(fd);
}

/* Play a sound file in the background with /bin/play. */
static void play_in_background(const char *path)
{
    int pid = fork();
    if (pid != 0)
        return;
    int null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(null, 1);
    dup2(null, 2);
    for (int fd = 3; fd < 32; fd++)
        close(fd);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    char *argv[] = { "play", "-q", (char *)path, NULL };
    execve("/bin/play", argv, NULL);
    _exit(127);
}

void sound_init(void)
{
    struct stat st;
    have_mixer = stat("/dev/mixer", &st) == 0;
    have_dsp = stat("/dev/dsp", &st) == 0;
    if (have_mixer)
        read_levels();
    if (have_dsp && stat(STARTUP_SOUND, &st) == 0)
        play_in_background(STARTUP_SOUND);
}

/* ---- Volume Control ---- */

static struct rect track_rect(int col)
{
    return mkrect(col * COL_W + COL_W / 2 - 2, TRACK_Y, 4, TRACK_H);
}

static struct rect thumb_rect(int col)
{
    int y = TRACK_Y + (100 - level[col]) * (TRACK_H - THUMB_H) / 100;
    return mkrect(col * COL_W + (COL_W - THUMB_W) / 2, y, THUMB_W, THUMB_H);
}

static struct rect mute_rect(int col)
{
    return mkrect(col * COL_W + 14, MUTE_Y, COL_W - 20, 14);
}

static void draw_check(struct surface *s, int x, int y, const char *label, int checked, int focused)
{
    struct rect box = mkrect(x, y, 13, 13);
    gfx_fill(s, box.x, box.y, box.w, box.h, C_WHITE);
    gfx_bevel(s, box, BEVEL_SUNKEN);
    if (checked)
        for (int i = 0; i < 7; i++) {   /* a tick: down three, up four */
            int yy = y + (i < 3 ? 5 + i : 9 - (i - 2));
            gfx_vline(s, x + 3 + i, yy, 3, C_BLACK);
        }
    int tx = x + 18, ty = y + (13 - font_sans.height) / 2;
    gfx_text(s, &font_sans, tx, ty, label, C_BLACK);
    if (focused)
        gfx_focus_rect(s, mkrect(tx - 2, ty, gfx_text_width(&font_sans, label) + 4, font_sans.height + 1));
}

static void vc_paint(struct vc *v)
{
    struct surface *s = &v->win->content;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);
    for (int c = 0; c < NCOLS; c++) {
        int x0 = c * COL_W;
        if (c > 0) {
            gfx_vline(s, x0 - 1, 6, VC_H - 12, C_SHADOW);
            gfx_vline(s, x0, 6, VC_H - 12, C_WHITE);
        }
        const struct pfont *f = c == 0 ? &font_sans_bold : &font_sans;
        char title[32];
        gfx_ellipsize(f, columns[c].title, COL_W - 8, title, sizeof(title));
        int tw = gfx_text_width(f, title);
        gfx_text(s, f, x0 + (COL_W - tw) / 2, 10, title, C_BLACK);
        gfx_text(s, &font_sans, x0 + 10, 36, "Volume:", C_BLACK);

        /* the track, tick marks down its left, and the thumb */
        struct rect tr = track_rect(c);
        gfx_bevel(s, tr, BEVEL_SUNKEN);
        for (int i = 0; i <= 10; i++)
            gfx_hline(s, tr.x - 16, TRACK_Y + THUMB_H / 2 + i * (TRACK_H - THUMB_H) / 10, 5, C_BLACK);
        struct rect th = thumb_rect(c);
        gfx_fill(s, th.x, th.y, th.w, th.h, C_FACE);
        gfx_bevel(s, th, BEVEL_RAISED);
        gfx_hline(s, th.x + 3, th.y + th.h / 2, th.w - 6, C_SHADOW);
        if (c == v->focus && wm_focused() == v->win)
            gfx_focus_rect(s, mkrect(x0 + 8, TRACK_Y - 4, COL_W - 16, TRACK_H + 8));

        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", level[c]);
        tw = gfx_text_width(&font_sans, pct);
        gfx_text(s, &font_sans, x0 + (COL_W - tw) / 2, TRACK_Y + TRACK_H + 4, pct, C_SHADOW);

        struct rect mr = mute_rect(c);
        draw_check(s, mr.x, mr.y, c == 0 ? "Mute all" : "Mute", muted[c], 0);
    }
    wm_damage_client(v->win, mkrect(0, 0, s->w, s->h));
}

static void set_level(struct vc *v, int col, int value)
{
    value = value < 0 ? 0 : value > 100 ? 100 : value;
    if (muted[col] && value > 0)
        muted[col] = 0;
    level[col] = value;
    mixer_set(columns[col].channel, value);
    read_levels();                      /* what the card made of it */
    if (col == 0)
        wm_tray_changed();
    if (v)
        vc_paint(v);
}

static void toggle_mute(struct vc *v, int col)
{
    if (muted[col]) {
        muted[col] = 0;
        set_level(v, col, saved[col] ? saved[col] : 75);
    } else {
        saved[col] = level[col];
        set_level(v, col, 0);
        muted[col] = 1;
        if (col == 0)
            wm_tray_changed();
        vc_paint(v);
    }
}

static int level_at(int y, int grab)
{
    return 100 - (y - grab - TRACK_Y) * 100 / (TRACK_H - THUMB_H);
}

static void vc_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct vc *v = w->priv;
    int col = x / COL_W;
    if (col >= NCOLS)
        col = NCOLS - 1;
    if ((changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT)) {
        v->focus = col;
        if (rect_contains(mute_rect(col), x, y)) {
            toggle_mute(v, col);
            return;
        }
        struct rect th = thumb_rect(col);
        struct rect area = mkrect(col * COL_W + 8, TRACK_Y - 4, COL_W - 16, TRACK_H + 8);
        if (rect_contains(th, x, y)) {
            v->dragging = col;
            v->grab = y - th.y;
        } else if (rect_contains(area, x, y)) {
            set_level(v, col, level[col] + (y < th.y ? 20 : -20));     /* page up or down */
            return;
        }
        vc_paint(v);
    } else if (changed & MOUSE_LEFT) {
        v->dragging = -1;
    } else if (v->dragging >= 0) {
        int nv = level_at(y, v->grab);
        nv = nv < 0 ? 0 : nv > 100 ? 100 : nv;
        if (nv != level[v->dragging])
            set_level(v, v->dragging, nv);
    }
}

static void vc_key(struct window *w, const struct input_event *ev)
{
    struct vc *v = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    switch (ev->code) {
    case KEY_UP: set_level(v, v->focus, level[v->focus] + STEP); break;
    case KEY_DOWN: set_level(v, v->focus, level[v->focus] - STEP); break;
    case KEY_PGUP: set_level(v, v->focus, level[v->focus] + 20); break;
    case KEY_PGDN: set_level(v, v->focus, level[v->focus] - 20); break;
    case KEY_HOME: set_level(v, v->focus, 100); break;
    case KEY_END: set_level(v, v->focus, 0); break;
    case KEY_SPACE: toggle_mute(v, v->focus); break;
    case KEY_LEFT:
        v->focus = (v->focus + NCOLS - 1) % NCOLS;
        vc_paint(v);
        break;
    case KEY_RIGHT:
    case KEY_TAB:
        v->focus = (v->focus + ((ev->mods & KMOD_SHIFT) ? NCOLS - 1 : 1)) % NCOLS;
        vc_paint(v);
        break;
    case KEY_ESC:
        wm_destroy(w);
        break;
    }
}

static void vc_focus(struct window *w, int focused)
{
    struct vc *v = w->priv;
    if (focused)
        read_levels();
    if (v->win)
        vc_paint(v);
}

static void vc_destroy(struct window *w)
{
    struct vc *v = w->priv;
    if (vc_win == v)
        vc_win = NULL;
    free(v);
}

static const struct window_ops vc_ops = {
    .key = vc_key,
    .mouse = vc_mouse,
    .focus = vc_focus,
    .destroy = vc_destroy,
};

void volume_open(void)
{
    if (!have_mixer) {
        message_box("Volume Control", ICON_WARNING, "There is no Sound Blaster 16 to control.");
        return;
    }
    if (vc_win) {
        wm_focus(vc_win->win);
        return;
    }
    struct vc *v = calloc(1, sizeof(*v));
    if (!v)
        return;
    v->dragging = -1;
    read_levels();
    v->win = wm_create("Volume Control", ICON_VOLUME, NCOLS * COL_W, VC_H, WF_MINIMIZE, &vc_ops, v);
    if (!v->win) {
        free(v);
        return;
    }
    vc_win = v;
    vc_paint(v);
}

/* Once a second: follow changes made by other programs. */
void sound_tick(void)
{
    if (!have_mixer)
        return;
    int was_muted = sound_tray_icon() == ICON_MUTED;
    int changed = read_levels();
    if ((sound_tray_icon() == ICON_MUTED) != was_muted)
        wm_tray_changed();
    if (changed && vc_win && vc_win->dragging < 0)
        vc_paint(vc_win);
}
