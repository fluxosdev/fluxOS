/*
 * Dialogs: System Properties, Run and Shut Down. Each is a window whose
 * client area holds a few controls; the dialog code handles focus,
 * Tab/Enter/Escape and mouse presses the way Windows 95 dialogs do.
 */
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/soundcard.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "desktop.h"

enum ctl_type { CT_LABEL, CT_BOLD, CT_ICON, CT_BUTTON, CT_RADIO, CT_EDIT, CT_SEPARATOR };

struct control {
    enum ctl_type type;
    struct rect r;
    char text[96];
    int id;                     /* buttons: command; radios: value */
};

#define MAX_CONTROLS 24

struct dialog {
    struct window *win;
    struct control ctl[MAX_CONTROLS];
    int n;
    int focus;                  /* focused control, or -1 */
    int pressed;                /* button held down with the mouse, or -1 */
    int pressed_in;             /* ...and the pointer is still over it */
    int radio;                  /* value of the checked radio button */
    int default_id, cancel_id;
    char edit[128];
    int edit_len, edit_cur;
    int masked;                 /* the edit field shows asterisks (a password) */
    void (*command)(struct dialog *d, int id);
    struct dialog **self;       /* cleared when the dialog closes */
    /* message boxes and prompts: what to call when the user agrees */
    void (*on_yes)(void *ctx);
    void (*on_no)(void *ctx);
    void (*on_text)(void *ctx, const char *text);
    void (*on_cancel)(void *ctx);       /* a prompt closed without an answer */
    void *ctx;
};

enum { ID_OK = 1, ID_CANCEL, ID_YES, ID_NO, ID_SLEEP, ID_DIMMER, ID_BRIGHTER };
enum { OPT_SHUTDOWN, OPT_RESTART, OPT_SLEEP, OPT_CONSOLE };

static struct dialog *about_dlg, *run_dlg, *shutdown_dlg, *ask_dlg, *power_dlg;

static struct control *add(struct dialog *d, enum ctl_type type, int x, int y, int w, int h,
                           const char *text, int id)
{
    struct control *c = &d->ctl[d->n++];
    c->type = type;
    c->r = mkrect(x, y, w, h);
    snprintf(c->text, sizeof(c->text), "%s", text ? text : "");
    c->id = id;
    return c;
}

static int focusable(struct control *c)
{
    return c->type == CT_BUTTON || c->type == CT_RADIO || c->type == CT_EDIT;
}

static void paint(struct dialog *d)
{
    struct surface *s = &d->win->content;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);
    for (int i = 0; i < d->n; i++) {
        struct control *c = &d->ctl[i];
        int focused = i == d->focus;
        switch (c->type) {
        case CT_LABEL:
            gfx_text(s, &font_sans, c->r.x, c->r.y, c->text, C_BLACK);
            break;
        case CT_BOLD:
            gfx_text(s, &font_sans_bold, c->r.x, c->r.y, c->text, C_BLACK);
            break;
        case CT_ICON:
            draw_icon(s, c->r.x, c->r.y, (enum icon_id)c->id, c->r.w);
            break;
        case CT_SEPARATOR:
            draw_separator(s, c->r.x, c->r.y, c->r.w);
            break;
        case CT_BUTTON: {
            /* The default button moves to whichever button has the focus. */
            int focus_is_button = d->focus >= 0 && d->ctl[d->focus].type == CT_BUTTON;
            int is_default = focus_is_button ? focused : c->id == d->default_id;
            int pressed = i == d->pressed && d->pressed_in;
            draw_button(s, c->r, c->text, pressed, is_default, focused);
            break;
        }
        case CT_RADIO:
            draw_radio(s, c->r.x, c->r.y, c->text, c->id == d->radio, focused);
            break;
        case CT_EDIT:
            if (d->masked) {
                char stars[sizeof(d->edit)];
                memset(stars, '*', (size_t)d->edit_len);
                stars[d->edit_len] = 0;
                draw_text_field(s, c->r, stars, d->edit_cur, focused);
            } else {
                draw_text_field(s, c->r, d->edit, d->edit_cur, focused);
            }
            break;
        }
    }
    wm_damage_client(d->win, mkrect(0, 0, s->w, s->h));
}

static void close_dialog(struct dialog *d)
{
    wm_destroy(d->win);
}

static void dlg_destroy(struct window *w)
{
    struct dialog *d = w->priv;
    if (d->self)
        *d->self = NULL;
    free(d);
}

static void dlg_close(struct window *w)
{
    struct dialog *d = w->priv;
    if (d->command && d->cancel_id)
        d->command(d, d->cancel_id);
    else
        close_dialog(d);
}

static void dlg_focus(struct window *w, int focused)
{
    struct dialog *d = w->priv;
    (void)focused;
    if (d->win)                         /* not while wm_create() runs */
        paint(d);
}

static void activate(struct dialog *d, int id)
{
    if (d->command)
        d->command(d, id);
}

static void move_focus(struct dialog *d, int dir)
{
    if (d->n == 0)
        return;
    int i = d->focus < 0 ? (dir > 0 ? -1 : 0) : d->focus;
    for (int k = 0; k < d->n; k++) {
        i = (i + dir + d->n) % d->n;
        struct control *c = &d->ctl[i];
        /* A radio group is one tab stop: its checked button. */
        if (c->type == CT_RADIO && c->id != d->radio)
            continue;
        if (focusable(c)) {
            d->focus = i;
            return;
        }
    }
}

static void edit_key(struct dialog *d, const struct input_event *ev)
{
    if (ev->code == KEY_LEFT && d->edit_cur > 0)
        d->edit_cur--;
    else if (ev->code == KEY_RIGHT && d->edit_cur < d->edit_len)
        d->edit_cur++;
    else if (ev->code == 0xC7)                          /* home */
        d->edit_cur = 0;
    else if (ev->code == 0xCF)                          /* end */
        d->edit_cur = d->edit_len;
    else if (ev->code == 0xD3 && d->edit_cur < d->edit_len) {  /* delete */
        memmove(d->edit + d->edit_cur, d->edit + d->edit_cur + 1, (size_t)(d->edit_len - d->edit_cur));
        d->edit_len--;
    } else if (ev->len == 1 && ev->text[0] == 0x7F && d->edit_cur > 0) {
        memmove(d->edit + d->edit_cur - 1, d->edit + d->edit_cur, (size_t)(d->edit_len - d->edit_cur + 1));
        d->edit_cur--;
        d->edit_len--;
    } else if (ev->len == 1 && ev->text[0] == 0x15) {   /* ^U */
        d->edit_len = d->edit_cur = 0;
        d->edit[0] = 0;
    } else if (ev->len == 1 && ev->text[0] >= 32 && ev->text[0] < 127 &&
               d->edit_len < (int)sizeof(d->edit) - 1) {
        memmove(d->edit + d->edit_cur + 1, d->edit + d->edit_cur, (size_t)(d->edit_len - d->edit_cur + 1));
        d->edit[d->edit_cur++] = ev->text[0];
        d->edit_len++;
    }
}

static void dlg_key(struct window *w, const struct input_event *ev)
{
    struct dialog *d = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    struct control *fc = d->focus >= 0 ? &d->ctl[d->focus] : NULL;

    if (ev->code == KEY_TAB) {
        move_focus(d, (ev->mods & KMOD_SHIFT) ? -1 : 1);
    } else if (ev->code == KEY_ENTER) {
        activate(d, fc && fc->type == CT_BUTTON ? fc->id : d->default_id);
        return;
    } else if (ev->code == KEY_ESC) {
        dlg_close(w);
        return;
    } else if (fc && fc->type == CT_BUTTON && ev->code == KEY_SPACE) {
        activate(d, fc->id);
        return;
    } else if (fc && fc->type == CT_RADIO &&
               (ev->code == KEY_UP || ev->code == KEY_DOWN || ev->code == KEY_LEFT || ev->code == KEY_RIGHT)) {
        int dir = ev->code == KEY_UP || ev->code == KEY_LEFT ? -1 : 1;
        int i = d->focus;
        do {
            i = (i + dir + d->n) % d->n;
        } while (d->ctl[i].type != CT_RADIO);
        d->radio = d->ctl[i].id;
        d->focus = i;
    } else if (fc && fc->type == CT_EDIT) {
        edit_key(d, ev);
    } else {
        return;
    }
    paint(d);
}

static void dlg_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct dialog *d = w->priv;
    int hit = -1;
    for (int i = 0; i < d->n; i++) {
        struct rect r = d->ctl[i].r;
        if (d->ctl[i].type == CT_RADIO)
            r = mkrect(r.x, r.y - 2, 18 + gfx_text_width(&font_sans, d->ctl[i].text) + 4, 16);
        if (focusable(&d->ctl[i]) && rect_contains(r, x, y))
            hit = i;
    }

    if ((changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT)) {     /* press */
        if (hit < 0)
            return;
        struct control *c = &d->ctl[hit];
        d->focus = hit;
        if (c->type == CT_BUTTON) {
            d->pressed = hit;
            d->pressed_in = 1;
        } else if (c->type == CT_RADIO) {
            d->radio = c->id;
        } else if (c->type == CT_EDIT) {
            d->edit_cur = d->edit_len;
        }
        paint(d);
    } else if (changed & MOUSE_LEFT) {                          /* release */
        int p = d->pressed;
        int in = d->pressed_in;
        d->pressed = -1;
        d->pressed_in = 0;
        if (p >= 0) {
            paint(d);
            if (in && hit == p)
                activate(d, d->ctl[p].id);
        }
    } else if (d->pressed >= 0) {                               /* drag */
        int in = hit == d->pressed;
        if (in != d->pressed_in) {
            d->pressed_in = in;
            paint(d);
        }
    }
}

static const struct window_ops dialog_ops = {
    .key = dlg_key,
    .mouse = dlg_mouse,
    .focus = dlg_focus,
    .close = dlg_close,
    .destroy = dlg_destroy,
};

static struct dialog *dialog_new(const char *title, enum icon_id icon, int w, int h, int flags,
                                 struct dialog **self)
{
    struct dialog *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->focus = -1;
    d->pressed = -1;
    d->self = self;
    d->win = wm_create(title, icon, w, h, flags, &dialog_ops, d);
    if (!d->win) {
        free(d);
        return NULL;
    }
    *self = d;
    return d;
}

static void dialog_show(struct dialog *d, int focus_index)
{
    d->focus = focus_index;
    wm_center(d->win);
    paint(d);
}

/* ---- message boxes and prompts ---- */

static void ask_command(struct dialog *d, int id)
{
    void (*on_yes)(void *) = d->on_yes;
    void (*on_no)(void *) = d->on_no;
    void (*on_text)(void *, const char *) = d->on_text;
    void (*on_cancel)(void *) = d->on_cancel;
    void *ctx = d->ctx;
    char text[sizeof(d->edit)];
    snprintf(text, sizeof(text), "%s", d->edit);
    memset(d->edit, 0, sizeof(d->edit));    /* it may be a password */
    close_dialog(d);                    /* frees d */
    if (id == ID_YES && on_yes)
        on_yes(ctx);
    else if (id == ID_NO && on_no)
        on_no(ctx);
    else if (id == ID_OK && on_text && text[0])
        on_text(ctx, text);
    else if (on_cancel && (id == ID_CANCEL || id == ID_OK))
        on_cancel(ctx);
    memset(text, 0, sizeof(text));
}

#define MSG_TEXT_W    420               /* message text wraps at this width */
#define MSG_MAX_LINES (MAX_CONTROLS - 4) /* room left beside the icon and three buttons */

/* Break a message into lines no wider than MSG_TEXT_W: at each '\n', and
 * between words. Tabs become spaces and other control characters go, as
 * the font has no glyphs for them. Returns the number of lines; a message
 * too long for MSG_MAX_LINES ends in "...". */
static int message_lines(const char *text, char lines[][sizeof(((struct control *)0)->text)])
{
    const int cap = (int)sizeof(lines[0]) - 1;
    int n = 0;
    const char *p = text;
    while (*p && n < MSG_MAX_LINES) {
        char *out = lines[n++];
        int len = 0, width = 0, last_space = -1;
        const char *after_space = NULL;
        for (; *p && *p != '\n'; p++) {
            char ch = *p == '\t' ? ' ' : *p;
            if ((unsigned char)ch < 32)
                continue;
            int a = gfx_text_width(&font_sans, (char[]){ ch, 0 });
            if (len && (width + a > MSG_TEXT_W || len == cap)) {
                if (ch != ' ' && last_space > 0) {
                    len = last_space;   /* the word goes to the next line */
                    p = after_space;
                }
                break;
            }
            if (ch == ' ') {
                last_space = len;
                after_space = p + 1;
            }
            out[len++] = ch;
            width += a;
        }
        while (len && out[len - 1] == ' ')
            len--;
        out[len] = 0;
        if (*p == '\n')
            p++;
        else
            while (*p == ' ')
                p++;
    }
    if (!*p)
        while (n > 1 && !lines[n - 1][0])
            n--;
    if (*p && n) {
        char *last = lines[n - 1];
        size_t l = strlen(last);
        if (l > (size_t)cap - 3)
            l = (size_t)cap - 3;
        memcpy(last + l, "...", 4);
    }
    return n ? n : 1;
}

/* A box with an icon, its text and the given buttons; NULL if one is open. */
static struct dialog *message_dialog(const char *title, enum icon_id icon, const char *text,
                                     const char *const *labels, const int *ids, int nbuttons)
{
    if (ask_dlg) {
        wm_focus(ask_dlg->win);
        return NULL;
    }
    if (icon == ICON_STOP || icon == ICON_WARNING)
        sound_error();
    char lines[MSG_MAX_LINES][sizeof(((struct control *)0)->text)];
    lines[0][0] = 0;
    int nlines = message_lines(text, lines);
    int text_w = 0;
    for (int i = 0; i < nlines; i++) {
        int lw = gfx_text_width(&font_sans, lines[i]);
        if (lw > text_w)
            text_w = lw;
    }
    int w = 62 + text_w + 24;
    int bw = nbuttons * 75 + (nbuttons - 1) * 6;
    if (w < bw + 40)
        w = bw + 40;
    if (w < 240)
        w = 240;
    /* One line sits level with the middle of the icon; more start at its top. */
    int line_h = font_sans.height + 2;
    int text_y = nlines == 1 ? 23 : 14;
    int h = text_y + nlines * line_h + 62;
    if (h < 100)
        h = 100;
    struct dialog *d = dialog_new(title, icon, w, h, WF_NOTASK, &ask_dlg);
    if (!d)
        return NULL;
    d->command = ask_command;
    d->default_id = ids[0];
    d->cancel_id = ids[nbuttons - 1];
    add(d, CT_ICON, 14, 14, 32, 32, NULL, icon);
    for (int i = 0; i < nlines; i++)
        add(d, CT_LABEL, 62, text_y + i * line_h, 0, 0, lines[i], 0);
    int first = d->n;
    for (int i = 0; i < nbuttons; i++)
        add(d, CT_BUTTON, (w - bw) / 2 + i * 81, h - 12 - 23, 75, 23, labels[i], ids[i]);
    dialog_show(d, first);
    return d;
}

void message_box(const char *title, enum icon_id icon, const char *text)
{
    static const char *const labels[] = { "OK" };
    static const int ids[] = { ID_OK };
    message_dialog(title, icon, text, labels, ids, 1);
}

void ask_yes_no(const char *title, enum icon_id icon, const char *text,
                void (*yes)(void *ctx), void *ctx)
{
    static const char *const labels[] = { "Yes", "No" };
    static const int ids[] = { ID_YES, ID_NO };
    struct dialog *d = message_dialog(title, icon, text, labels, ids, 2);
    if (d) {
        d->on_yes = yes;
        d->ctx = ctx;
    }
}

void ask_yes_no_cancel(const char *title, enum icon_id icon, const char *text,
                       void (*yes)(void *ctx), void (*no)(void *ctx), void *ctx)
{
    static const char *const labels[] = { "Yes", "No", "Cancel" };
    static const int ids[] = { ID_YES, ID_NO, ID_CANCEL };
    struct dialog *d = message_dialog(title, icon, text, labels, ids, 3);
    if (d) {
        d->on_yes = yes;
        d->on_no = no;
        d->ctx = ctx;
    }
}

int ask_text_cancel(const char *title, const char *label, const char *initial,
                    void (*done)(void *ctx, const char *text), void (*cancel)(void *ctx), void *ctx)
{
    if (ask_dlg) {
        wm_focus(ask_dlg->win);
        return 0;
    }
    struct dialog *d = dialog_new(title, ICON_NONE, 320, 104, WF_NOTASK, &ask_dlg);
    if (!d)
        return 0;
    d->on_cancel = cancel;
    d->command = ask_command;
    d->default_id = ID_OK;
    d->cancel_id = ID_CANCEL;
    d->on_text = done;
    d->ctx = ctx;
    add(d, CT_LABEL, 12, 12, 0, 0, label, 0);
    int edit = d->n;
    add(d, CT_EDIT, 12, 32, 296, 21, NULL, 0);
    add(d, CT_BUTTON, 320 - 12 - 75 - 6 - 75, 104 - 12 - 23, 75, 23, "OK", ID_OK);
    add(d, CT_BUTTON, 320 - 12 - 75, 104 - 12 - 23, 75, 23, "Cancel", ID_CANCEL);
    snprintf(d->edit, sizeof(d->edit), "%s", initial);
    d->edit_len = d->edit_cur = (int)strlen(d->edit);
    dialog_show(d, edit);
    return 1;
}

void ask_text(const char *title, const char *label, const char *initial,
              void (*done)(void *ctx, const char *text), void *ctx)
{
    ask_text_cancel(title, label, initial, done, NULL, ctx);
}

int ask_password(const char *title, const char *label,
                 void (*done)(void *ctx, const char *text), void (*cancel)(void *ctx), void *ctx)
{
    if (!ask_text_cancel(title, label, "", done, cancel, ctx))
        return 0;
    ask_dlg->masked = 1;
    paint(ask_dlg);
    return 1;
}

/* ---- System Properties ---- */

static void format_uptime(char *buf, size_t size)
{
    unsigned int s = uptime_ms() / 1000;
    snprintf(buf, size, "Up %u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}

static int count_processes(void)
{
    struct procinfo pi;
    int n = 0;
    for (int slot = 1; slot < 64; slot++)
        if (procinfo(slot, &pi) > 0 && pi.state != PSTATE_ZOMBIE)
            n++;
    return n;
}

/* System Properties' text lines, which about_fill() keeps current. */
enum { AB_RELEASE, AB_VERSION, AB_CPU, AB_MEMORY, AB_DISPLAY, AB_SOUND, AB_UPTIME, AB_PROCS, AB_LINES };
static int about_line[AB_LINES];

static void about_fill(struct dialog *d)
{
    struct utsname u;
    struct meminfo mi;
    uname(&u);
    meminfo(&mi);
    char *t[AB_LINES];
    size_t n = sizeof(d->ctl[0].text);
    for (int i = 0; i < AB_LINES; i++)
        t[i] = d->ctl[about_line[i]].text;

    snprintf(t[AB_RELEASE], n, "%s %s", u.sysname, u.release);
    snprintf(t[AB_VERSION], n, "%s", u.version);
    if (mi.ncpu > 1)
        snprintf(t[AB_CPU], n, "%u x %s processors", mi.ncpu, u.machine);
    else
        snprintf(t[AB_CPU], n, "%s processor", u.machine);
    snprintf(t[AB_MEMORY], n, "%u.%uMB RAM (%u.%uMB free)",
             mi.total_kb / 1024, mi.total_kb % 1024 * 10 / 1024,
             mi.free_kb / 1024, mi.free_kb % 1024 * 10 / 1024);
    struct dispinfo di;
    if (display(DISP_INFO, &di) == 0 && di.adapter[0])
        snprintf(t[AB_DISPLAY], n, "Display: %d x %d, %s", screen_w, screen_h, di.adapter);
    else
        snprintf(t[AB_DISPLAY], n, "Display: %d x %d", screen_w, screen_h);
    struct sound_info si;
    memset(&si, 0, sizeof(si));
    int fd = open("/dev/speaker", O_WRONLY);
    if (fd >= 0 && ioctl(fd, SOUND_INFO, &si) == 0 && (si.flags & SOUNDF_DSP))
        snprintf(t[AB_SOUND], n, "Sound: %s%s", si.card,
                 (si.flags & SOUNDF_OPL3) ? ", OPL3 FM" : (si.flags & SOUNDF_OPL2) ? ", OPL2 FM" : "");
    else if (fd >= 0 && (si.flags & SOUNDF_OPL2))
        snprintf(t[AB_SOUND], n, "Sound: PC speaker, %s FM", (si.flags & SOUNDF_OPL3) ? "OPL3" : "OPL2");
    else
        snprintf(t[AB_SOUND], n, "Sound: PC speaker");
    if (fd >= 0)
        close(fd);
    format_uptime(t[AB_UPTIME], n);
    snprintf(t[AB_PROCS], n, "%d processes running", count_processes());
}

static void about_command(struct dialog *d, int id)
{
    (void)id;
    close_dialog(d);
}

void about_open(void)
{
    if (about_dlg) {
        wm_focus(about_dlg->win);
        return;
    }
    struct dialog *d = dialog_new("System Properties", ICON_COMPUTER, 380, 280, 0, &about_dlg);
    if (!d)
        return;
    d->command = about_command;
    d->default_id = ID_OK;
    d->cancel_id = ID_OK;

    static const struct { int line, y; } lines[] = {
        { AB_RELEASE, 32 }, { AB_VERSION, 50 }, { AB_CPU, 94 }, { AB_MEMORY, 112 },
        { AB_DISPLAY, 130 }, { AB_SOUND, 148 }, { AB_UPTIME, 192 }, { AB_PROCS, 210 },
    };
    add(d, CT_ICON, 32, 30, 48, 48, NULL, ICON_COMPUTER);
    add(d, CT_BOLD, 136, 14, 0, 0, "System:", 0);
    add(d, CT_BOLD, 136, 76, 0, 0, "Computer:", 0);
    add(d, CT_BOLD, 136, 174, 0, 0, "Status:", 0);
    for (int i = 0; i < AB_LINES; i++) {
        about_line[lines[i].line] = d->n;
        add(d, CT_LABEL, 150, lines[i].y, 0, 0, NULL, 0);
    }
    add(d, CT_SEPARATOR, 10, 234, 360, 2, NULL, 0);
    add(d, CT_BUTTON, 380 - 10 - 75, 280 - 10 - 23, 75, 23, "OK", ID_OK);
    about_fill(d);
    dialog_show(d, d->n - 1);
}

void about_tick(void)
{
    if (!about_dlg)
        return;
    about_fill(about_dlg);
    paint(about_dlg);
}

/* ---- Run ---- */

static void run_command(struct dialog *d, int id)
{
    if (id == ID_OK) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "%s", d->edit);
        if (!cmd[0])
            return;
        close_dialog(d);
        const char *argv[] = { "/bin/fsh", "-c", cmd, NULL };
        terminal_open(cmd, argv, 1, 0);
        return;
    }
    close_dialog(d);
}

void run_open(void)
{
    if (run_dlg) {
        wm_focus(run_dlg->win);
        return;
    }
    struct dialog *d = dialog_new("Run", ICON_RUN, 347, 140, 0, &run_dlg);
    if (!d)
        return;
    d->command = run_command;
    d->default_id = ID_OK;
    d->cancel_id = ID_CANCEL;
    add(d, CT_ICON, 12, 14, 32, 32, NULL, ICON_RUN);
    add(d, CT_LABEL, 56, 14, 0, 0, "Type the name of a program, and fluxOS", 0);
    add(d, CT_LABEL, 56, 30, 0, 0, "will run it for you in a new window.", 0);
    add(d, CT_LABEL, 12, 63, 0, 0, "Open:", 0);
    int edit = d->n;
    add(d, CT_EDIT, 56, 59, 279, 21, NULL, 0);
    add(d, CT_BUTTON, 347 - 12 - 75 - 6 - 75, 140 - 12 - 23, 75, 23, "OK", ID_OK);
    add(d, CT_BUTTON, 347 - 12 - 75, 140 - 12 - 23, 75, 23, "Cancel", ID_CANCEL);
    dialog_show(d, edit);
}

/* ---- Shut Down ---- */

void system_restart(void)
{
    wm_splash("restarting...");
    sync();
    sound_shutdown();
    reboot(REBOOT_RESTART);
}

void system_shutdown(void)
{
    wm_splash("shutting down...");
    sync();
    sound_shutdown();
    reboot(REBOOT_POWEROFF);
    /* still here: the machine cannot switch itself off */
    wm_splash("it's now safe to turn off your computer");
    reboot(REBOOT_HALT);
}

static void shutdown_command(struct dialog *d, int id)
{
    int choice = d->radio;
    close_dialog(d);
    if (id != ID_YES)
        return;
    switch (choice) {
    case OPT_SHUTDOWN:
        system_shutdown();
        break;
    case OPT_RESTART:
        system_restart();
        break;
    case OPT_SLEEP:
        system_sleep();
        break;
    case OPT_CONSOLE:
        exit(0);
    }
}

void shutdown_open(void)
{
    if (shutdown_dlg) {
        wm_focus(shutdown_dlg->win);
        return;
    }
    const struct powerinfo *pi = power_info();
    int sleep = pi && (pi->flags & PWR_CAN_SLEEP);
    int h = sleep ? 170 : 150, y = 40;
    struct dialog *d = dialog_new("Shut Down fluxOS", ICON_SHUTDOWN, 344, h,
                                  WF_MODAL | WF_NOTASK, &shutdown_dlg);
    if (!d)
        return;
    d->command = shutdown_command;
    d->default_id = ID_YES;
    d->cancel_id = ID_NO;
    d->radio = OPT_SHUTDOWN;
    add(d, CT_ICON, 14, 14, 32, 32, NULL, ICON_SHUTDOWN);
    add(d, CT_LABEL, 62, 14, 0, 0, "Are you sure you want to:", 0);
    int first = d->n;
    add(d, CT_RADIO, 66, y, 0, 0, "Shut down the computer?", OPT_SHUTDOWN);
    add(d, CT_RADIO, 66, y += 20, 0, 0, "Restart the computer?", OPT_RESTART);
    if (sleep)
        add(d, CT_RADIO, 66, y += 20, 0, 0, "Put the computer to sleep?", OPT_SLEEP);
    add(d, CT_RADIO, 66, y += 20, 0, 0, "Close all programs and exit to the console?", OPT_CONSOLE);
    add(d, CT_BUTTON, 344 / 2 - 6 - 75, h - 12 - 23, 75, 23, "Yes", ID_YES);
    add(d, CT_BUTTON, 344 / 2 + 6, h - 12 - 23, 75, 23, "No", ID_NO);
    dialog_show(d, first);
}

/* ---- Power ---- */

enum { PW_SOURCE, PW_BATTERY, PW_LID, PW_TEMP, PW_BRIGHT, PW_LINES };
static int power_line[PW_LINES], power_icon_ctl;

static void power_fill(struct dialog *d)
{
    const struct powerinfo *pi = power_info();
    char *t[PW_LINES];
    size_t n = sizeof(d->ctl[0].text);
    for (int i = 0; i < PW_LINES; i++) {
        t[i] = d->ctl[power_line[i]].text;
        t[i][0] = 0;
    }
    if (!pi)
        return;
    if (power_tray_present())
        d->ctl[power_icon_ctl].id = power_tray_icon();      /* the charge, as in the tray */
    if (pi->flags & PWR_AC_KNOWN)
        snprintf(t[PW_SOURCE], n, "Power source: %s", (pi->flags & PWR_AC_ONLINE) ? "AC adapter" : "battery");
    char b[64];
    power_battery_text(b, sizeof(b));
    snprintf(t[PW_BATTERY], n, "Battery: %s", b);
    if (pi->flags & PWR_LID)
        snprintf(t[PW_LID], n, "Lid: %s (closing it puts the computer to sleep)",
                 (pi->flags & PWR_LID_CLOSED) ? "closed" : "open");
    if (pi->flags & PWR_THERMAL)
        snprintf(t[PW_TEMP], n, "Temperature: %d C", pi->temp / 10);
    if (pi->flags & PWR_BACKLIGHT)
        snprintf(t[PW_BRIGHT], n, "Screen brightness: %u%%", pi->brightness);
    else
        snprintf(t[PW_BRIGHT], n, "Screen brightness: not adjustable here");
}

static void power_command(struct dialog *d, int id)
{
    switch (id) {
    case ID_SLEEP:
        close_dialog(d);
        system_sleep();
        return;
    case ID_DIMMER:
    case ID_BRIGHTER:
        power(POWER_BRIGHTNESS, id == ID_BRIGHTER ? BRIGHTNESS_UP : BRIGHTNESS_DOWN);
        power_tick();
        power_dialog_tick();
        return;
    }
    close_dialog(d);
}

void power_open(void)
{
    if (power_dlg) {
        wm_focus(power_dlg->win);
        return;
    }
    const struct powerinfo *pi = power_info();
    enum icon_id icon = power_tray_present() ? power_tray_icon() : ICON_INFO;
    struct dialog *d = dialog_new("Power", icon, 380, 214, 0, &power_dlg);
    if (!d)
        return;
    d->command = power_command;
    d->default_id = ID_OK;
    d->cancel_id = ID_OK;
    power_icon_ctl = d->n;
    add(d, CT_ICON, 14, 14, 32, 32, NULL, icon);
    add(d, CT_BOLD, 62, 14, 0, 0, "Power", 0);
    for (int i = 0; i < PW_LINES; i++) {
        power_line[i] = d->n;
        add(d, CT_LABEL, 62, 38 + 20 * i, 0, 0, NULL, 0);
    }
    add(d, CT_SEPARATOR, 10, 214 - 50, 360, 2, NULL, 0);
    int x = 10;
    if (pi && (pi->flags & PWR_CAN_SLEEP)) {
        add(d, CT_BUTTON, x, 214 - 10 - 23, 75, 23, "Sleep", ID_SLEEP);
        x += 81;
    }
    if (pi && (pi->flags & PWR_BACKLIGHT)) {
        add(d, CT_BUTTON, x, 214 - 10 - 23, 75, 23, "Dimmer", ID_DIMMER);
        add(d, CT_BUTTON, x + 81, 214 - 10 - 23, 75, 23, "Brighter", ID_BRIGHTER);
    }
    add(d, CT_BUTTON, 380 - 10 - 75, 214 - 10 - 23, 75, 23, "Close", ID_OK);
    power_fill(d);
    dialog_show(d, d->n - 1);
}

void power_dialog_tick(void)
{
    if (!power_dlg)
        return;
    power_fill(power_dlg);
    paint(power_dlg);
}
