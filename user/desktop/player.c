/*
 * The sound player: opens a PCM WAV file, shows its format and length, and
 * plays it with play(1) as a child process, so the desktop keeps running
 * while it sounds. Pause stops the child and remembers the second; Play
 * starts it again from there (play -s). Clicking or dragging the bar seeks.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "desktop.h"

#define PL_W      320
#define PL_H      150
#define PATH_MAX_CHARS 255
#define PLAY      "/bin/play"

extern char **environ;

enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };
enum { BT_PLAY, BT_PAUSE, BT_STOP, NBT };

struct player {
    struct window *win;
    char path[PATH_MAX_CHARS + 1];      /* the field */
    int path_len, path_cur;
    char loaded[PATH_MAX_CHARS + 1];    /* the file that is open */
    int rate, channels, bits;
    uint32_t dur_ms;                    /* 0: nothing open */
    int state;
    int pid;                            /* the play process, 0 if none */
    uint32_t pos_ms, started;           /* where it began, and when (now_ms) */
    char msg[96];
    int pressed;                        /* a button held down, or -1 */
    int seeking, seek_resume;           /* the bar is held; it was playing  */
};

static struct player *cur;

static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

/* ---- the file ---- */

static int read_full(int fd, void *buf, uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        int r = (int)read(fd, (uint8_t *)buf + got, n - got);
        if (r <= 0)
            break;
        got += (uint32_t)r;
    }
    return (int)got;
}

/* Read a PCM WAV header; 0 on success, else a message in err. */
static int wav_info(const char *path, struct player *p, const char **err)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        *err = strerror(errno);
        return -1;
    }
    uint8_t h[12], ck[8], fmt[40];
    int have_fmt = 0, ok = -1;
    *err = "Not a WAV file.";
    if (read_full(fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        goto out;
    for (;;) {
        if (read_full(fd, ck, 8) != 8) {
            *err = "The file has no sound data.";
            goto out;
        }
        uint32_t len = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            uint32_t n = len < sizeof(fmt) ? len : sizeof(fmt);
            if (n < 16 || read_full(fd, fmt, n) != (int)n ||
                lseek(fd, (long)((len - n) + (len & 1)), SEEK_CUR) < 0)
                goto out;
            uint32_t tag = le16(fmt);
            if (tag == 0xFFFE && n >= 26)
                tag = le16(fmt + 24);
            p->channels = (int)le16(fmt + 2);
            p->rate = (int)le32(fmt + 4);
            p->bits = (int)le16(fmt + 14);
            if (tag != 1 || p->channels < 1 || p->channels > 2 || p->rate < 1000 ||
                (p->bits != 8 && p->bits != 16 && p->bits != 24 && p->bits != 32)) {
                *err = "Only PCM WAV files, mono or stereo, can be played.";
                goto out;
            }
            have_fmt = 1;
        } else if (!memcmp(ck, "data", 4)) {
            if (!have_fmt)
                goto out;
            uint32_t frames = len / (uint32_t)(p->channels * p->bits / 8);
            p->dur_ms = frames / (uint32_t)p->rate * 1000 + frames % (uint32_t)p->rate * 1000 / (uint32_t)p->rate;
            ok = 0;
            goto out;
        } else if (lseek(fd, (long)(len + (len & 1)), SEEK_CUR) < 0) {
            goto out;
        }
    }
out:
    close(fd);
    return ok;
}

/* ---- the process ---- */

static uint32_t position(const struct player *p)
{
    uint32_t t = p->pos_ms;
    if (p->state == ST_PLAYING)
        t += now_ms() - p->started;
    return t > p->dur_ms ? p->dur_ms : t;
}

static void stop_child(struct player *p)
{
    if (p->pid > 0) {
        int pid = p->pid;
        p->pid = 0;                     /* its exit is expected */
        kill(pid, SIGKILL);
    }
}

static void paint(struct player *p);

static void start_child(struct player *p, uint32_t from_ms)
{
    char sec[16];
    snprintf(sec, sizeof(sec), "%u", from_ms / 1000);
    int pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
        for (int fd = 3; fd < 32; fd++)
            close(fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        char *argv[] = { "play", "-q", "-s", sec, p->loaded, NULL };
        execve(PLAY, argv, environ);
        _exit(127);
    }
    if (pid < 0) {
        snprintf(p->msg, sizeof(p->msg), "Cannot start playback: %s.", strerror(errno));
        p->state = ST_STOPPED;
        return;
    }
    p->pid = pid;
    p->pos_ms = from_ms / 1000 * 1000;
    p->started = now_ms();
    p->state = ST_PLAYING;
    p->msg[0] = 0;
}

static void do_play(struct player *p)
{
    if (!p->dur_ms || p->state == ST_PLAYING)
        return;
    uint32_t from = p->state == ST_PAUSED ? p->pos_ms : 0;
    if (from + 1000 > p->dur_ms)
        from = 0;
    start_child(p, from);
}

static void do_pause(struct player *p)
{
    if (p->state != ST_PLAYING)
        return;
    p->pos_ms = position(p);
    stop_child(p);
    p->state = ST_PAUSED;
}

static void do_stop(struct player *p)
{
    stop_child(p);
    p->state = ST_STOPPED;
    p->pos_ms = 0;
}

static void load(struct player *p, const char *path)
{
    do_stop(p);
    p->dur_ms = 0;
    p->loaded[0] = 0;
    p->msg[0] = 0;
    const char *err;
    if (wav_info(path, p, &err) < 0) {
        p->dur_ms = 0;
        snprintf(p->msg, sizeof(p->msg), "%s", err);
        wm_set_title(p->win, "Sound Player");
        return;
    }
    snprintf(p->loaded, sizeof(p->loaded), "%s", path);
    const char *base = strrchr(path, '/');
    char title[64];
    snprintf(title, sizeof(title), "%s - Sound Player", base ? base + 1 : path);
    wm_set_title(p->win, title);
}

/* ---- the window ---- */

static struct rect path_rect(void) { return mkrect(40, 8, PL_W - 48, 22); }
static struct rect bar_rect(void) { return mkrect(8, 62, PL_W - 16, 8); }
static struct rect button_rect(int i) { return mkrect(8 + i * 72, 112, 66, 26); }

/* a * n / total without 64-bit arithmetic (n is at most a few hundred). */
static uint32_t scale(uint32_t a, uint32_t n, uint32_t total)
{
    uint32_t q = total / 4096 + 1;      /* coarsen a and total until a * n fits */
    return total ? (a / q) * n / (total / q ? total / q : 1) : 0;
}

static uint32_t bar_pos(const struct player *p, int x)
{
    struct rect r = bar_rect();
    int off = x - r.x;
    if (off < 0)
        off = 0;
    if (off > r.w)
        off = r.w;
    return p->dur_ms / (uint32_t)r.w * (uint32_t)off + p->dur_ms % (uint32_t)r.w * (uint32_t)off / (uint32_t)r.w;
}

static void fmt_time(char *out, int size, uint32_t ms)
{
    snprintf(out, (size_t)size, "%u:%02u", ms / 60000, ms / 1000 % 60);
}

static void paint(struct player *p)
{
    struct surface *s = &p->win->content;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);

    gfx_text(s, &font_sans, 8, 13, "File:", C_BLACK);
    draw_text_field(s, path_rect(), p->path, p->path_cur, wm_focused() == p->win);

    char line[128], fit[128];
    if (p->msg[0])
        snprintf(line, sizeof(line), "%s", p->msg);
    else if (p->dur_ms)
        snprintf(line, sizeof(line), "%d Hz, %d bit, %s", p->rate, p->bits, p->channels == 2 ? "stereo" : "mono");
    else
        snprintf(line, sizeof(line), "Type the name of a WAV file and press Enter.");
    gfx_ellipsize(&font_sans, line, s->w - 16, fit, sizeof(fit));
    gfx_text(s, &font_sans, 8, 38, fit, p->msg[0] ? C_BLACK : C_SHADOW);

    struct rect bar = bar_rect();
    gfx_fill(s, bar.x, bar.y, bar.w, bar.h, C_WHITE);
    gfx_bevel(s, bar, BEVEL_SUNKEN);
    uint32_t pos = position(p);
    int px = (int)scale(pos, (uint32_t)(bar.w - 2), p->dur_ms);
    if (px > 0)
        gfx_fill(s, bar.x + 1, bar.y + 1, px, bar.h - 2, C_NAVY);
    struct rect th = mkrect(bar.x + px - 3, bar.y - 5, 8, 18);
    gfx_fill(s, th.x, th.y, th.w, th.h, C_FACE);
    gfx_bevel(s, th, BEVEL_RAISED);

    char a[16], b[16], t[40];
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), p->dur_ms);
    snprintf(t, sizeof(t), "%s / %s", a, b);
    gfx_text(s, &font_sans, 8, 84, t, C_BLACK);
    const char *st = p->state == ST_PLAYING ? "Playing" : p->state == ST_PAUSED ? "Paused" : "Stopped";
    gfx_text(s, &font_sans, s->w - 8 - gfx_text_width(&font_sans, st), 84, st, C_SHADOW);

    static const char *const labels[NBT] = { "Play", "Pause", "Stop" };
    for (int i = 0; i < NBT; i++)
        draw_button(s, button_rect(i), labels[i], p->pressed == i, i == BT_PLAY && p->state != ST_PLAYING, 0);
    wm_damage_client(p->win, mkrect(0, 0, s->w, s->h));
}

static void path_key(struct player *p, const struct input_event *ev)
{
    if (ev->code == KEY_ENTER) {
        if (p->path[0]) {
            load(p, p->path);
            if (p->dur_ms)
                do_play(p);
        }
    } else if (ev->code == KEY_LEFT) {
        if (p->path_cur > 0)
            p->path_cur--;
    } else if (ev->code == KEY_RIGHT) {
        if (p->path_cur < p->path_len)
            p->path_cur++;
    } else if (ev->code == KEY_HOME) {
        p->path_cur = 0;
    } else if (ev->code == KEY_END) {
        p->path_cur = p->path_len;
    } else if (ev->code == KEY_DELETE) {
        if (p->path_cur < p->path_len) {
            memmove(p->path + p->path_cur, p->path + p->path_cur + 1, (size_t)(p->path_len - p->path_cur));
            p->path_len--;
        }
    } else if (ev->len == 1 && ev->text[0] == 0x7F) {
        if (p->path_cur > 0) {
            memmove(p->path + p->path_cur - 1, p->path + p->path_cur, (size_t)(p->path_len - p->path_cur + 1));
            p->path_cur--;
            p->path_len--;
        }
    } else if (ev->len == 1 && ev->text[0] >= 32 && ev->text[0] < 127 && p->path_len < PATH_MAX_CHARS) {
        memmove(p->path + p->path_cur + 1, p->path + p->path_cur, (size_t)(p->path_len - p->path_cur + 1));
        p->path[p->path_cur++] = ev->text[0];
        p->path_len++;
    }
}

static void pl_key(struct window *w, const struct input_event *ev)
{
    struct player *p = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    path_key(p, ev);
    paint(p);
}

static void press_button(struct player *p, int i)
{
    switch (i) {
    case BT_PLAY: do_play(p); break;
    case BT_PAUSE: do_pause(p); break;
    case BT_STOP: do_stop(p); break;
    }
}

static void pl_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct player *p = w->priv;
    int left = buttons & MOUSE_LEFT;
    if ((changed & MOUSE_LEFT) && left) {
        struct rect bar = bar_rect();
        bar.y -= 6;
        bar.h += 12;
        p->pressed = -1;
        for (int i = 0; i < NBT; i++)
            if (rect_contains(button_rect(i), x, y))
                p->pressed = i;
        if (p->pressed < 0 && p->dur_ms && rect_contains(bar, x, y)) {
            p->seeking = 1;
            p->seek_resume = p->state == ST_PLAYING;
            do_pause(p);
            p->state = ST_PAUSED;
            p->pos_ms = bar_pos(p, x);
        }
    } else if ((changed & MOUSE_LEFT) && !left) {
        int i = p->pressed;
        p->pressed = -1;
        if (i >= 0 && rect_contains(button_rect(i), x, y))
            press_button(p, i);
        if (p->seeking) {
            p->seeking = 0;
            if (p->seek_resume)
                start_child(p, p->pos_ms);
        }
    } else if (p->seeking) {
        p->pos_ms = bar_pos(p, x);
    } else {
        return;
    }
    paint(p);
}

static void pl_focus(struct window *w, int focused)
{
    paint(w->priv);
}

static void pl_destroy(struct window *w)
{
    struct player *p = w->priv;
    stop_child(p);
    if (cur == p)
        cur = NULL;
    free(p);
}

static const struct window_ops pl_ops = {
    .key = pl_key,
    .mouse = pl_mouse,
    .focus = pl_focus,
    .destroy = pl_destroy,
};

void player_child_exited(int pid, int status)
{
    struct player *p = cur;
    if (!p || p->pid != pid)
        return;
    p->pid = 0;
    uint32_t played = position(p);
    if (status != 0 && played < p->pos_ms + 1500) {
        snprintf(p->msg, sizeof(p->msg), "Cannot play: no sound card, or another program is using it.");
        p->pos_ms = 0;
    } else {
        p->pos_ms = 0;
    }
    p->state = ST_STOPPED;
    paint(p);
}

void player_tick(void)
{
    if (cur && cur->state == ST_PLAYING)
        paint(cur);
}

struct window *player_open(const char *path)
{
    if (cur) {
        wm_focus(cur->win);
    } else {
        struct player *p = calloc(1, sizeof(*p));
        if (!p)
            return NULL;
        p->pressed = -1;
        struct window *w = wm_create("Sound Player", ICON_SOUND, PL_W, PL_H, WF_MINIMIZE, &pl_ops, p);
        if (!w) {
            free(p);
            return NULL;
        }
        p->win = w;
        cur = p;
    }
    if (path && *path) {
        snprintf(cur->path, sizeof(cur->path), "%s", path);
        cur->path_len = cur->path_cur = (int)strlen(cur->path);
        load(cur, cur->path);
        if (cur->dur_ms)
            do_play(cur);
    }
    paint(cur);
    return cur->win;
}
