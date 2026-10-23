/*
 * Dialogs: System Properties, Run and Shut Down. Each is a window whose
 * client area holds a few controls; the dialog code handles focus,
 * Tab/Enter/Escape and mouse presses the way Windows 95 dialogs do.
 */
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    void (*command)(struct dialog *d, int id);
    struct dialog **self;       /* cleared when the dialog closes */
    /* message boxes and prompts: what to call when the user agrees */
    void (*on_yes)(void *ctx);
    void (*on_no)(void *ctx);
    void (*on_text)(void *ctx, const char *text);
    void *ctx;
};

enum { ID_OK = 1, ID_CANCEL, ID_YES, ID_NO };
enum { OPT_SHUTDOWN, OPT_RESTART, OPT_CONSOLE };

static struct dialog *about_dlg, *run_dlg, *shutdown_dlg, *ask_dlg;

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
            draw_text_field(s, c->r, d->edit, d->edit_cur, focused);
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
    void *ctx = d->ctx;
    char text[sizeof(d->edit)];
    snprintf(text, sizeof(text), "%s", d->edit);
    close_dialog(d);                    /* frees d */
    if (id == ID_YES && on_yes)
        on_yes(ctx);
    else if (id == ID_NO && on_no)
        on_no(ctx);
    else if (id == ID_OK && on_text && text[0])
        on_text(ctx, text);
}

/* A box with an icon, a line of text and the given buttons; NULL if one is open. */
static struct dialog *message_dialog(const char *title, enum icon_id icon, const char *text,
                                     const char *const *labels, const int *ids, int nbuttons)
{
    if (ask_dlg) {
        wm_focus(ask_dlg->win);
        return NULL;
    }
    int w = 62 + gfx_text_width(&font_sans, text) + 24;
    int bw = nbuttons * 75 + (nbuttons - 1) * 6;
    if (w < bw + 40)
        w = bw + 40;
    if (w < 240)
        w = 240;
    int h = 100;
    struct dialog *d = dialog_new(title, icon, w, h, WF_NOTASK, &ask_dlg);
    if (!d)
        return NULL;
    d->command = ask_command;
    d->default_id = ids[0];
    d->cancel_id = ids[nbuttons - 1];
    add(d, CT_ICON, 14, 14, 32, 32, NULL, icon);
    add(d, CT_LABEL, 62, 23, 0, 0, text, 0);
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

void ask_text(const char *title, const char *label, const char *initial,
              void (*done)(void *ctx, const char *text), void *ctx)
{
    if (ask_dlg) {
        wm_focus(ask_dlg->win);
        return;
    }
    struct dialog *d = dialog_new(title, ICON_NONE, 320, 104, WF_NOTASK, &ask_dlg);
    if (!d)
        return;
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
enum { AB_RELEASE, AB_VERSION, AB_CPU, AB_MEMORY, AB_DISPLAY, AB_UPTIME, AB_PROCS, AB_LINES };
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
    snprintf(t[AB_CPU], n, "%s processor", u.machine);
    snprintf(t[AB_MEMORY], n, "%u.%uMB RAM (%u.%uMB free)",
             mi.total_kb / 1024, mi.total_kb % 1024 * 10 / 1024,
             mi.free_kb / 1024, mi.free_kb % 1024 * 10 / 1024);
    snprintf(t[AB_DISPLAY], n, "Display: %d x %d", screen_w, screen_h);
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
    struct dialog *d = dialog_new("System Properties", ICON_COMPUTER, 380, 262, 0, &about_dlg);
    if (!d)
        return;
    d->command = about_command;
    d->default_id = ID_OK;
    d->cancel_id = ID_OK;

    static const struct { int line, y; } lines[] = {
        { AB_RELEASE, 32 }, { AB_VERSION, 50 }, { AB_CPU, 94 }, { AB_MEMORY, 112 },
        { AB_DISPLAY, 130 }, { AB_UPTIME, 174 }, { AB_PROCS, 192 },
    };
    add(d, CT_ICON, 32, 30, 48, 48, NULL, ICON_COMPUTER);
    add(d, CT_BOLD, 136, 14, 0, 0, "System:", 0);
    add(d, CT_BOLD, 136, 76, 0, 0, "Computer:", 0);
    add(d, CT_BOLD, 136, 156, 0, 0, "Status:", 0);
    for (int i = 0; i < AB_LINES; i++) {
        about_line[lines[i].line] = d->n;
        add(d, CT_LABEL, 150, lines[i].y, 0, 0, NULL, 0);
    }
    add(d, CT_SEPARATOR, 10, 216, 360, 2, NULL, 0);
    add(d, CT_BUTTON, 380 - 10 - 75, 262 - 10 - 23, 75, 23, "OK", ID_OK);
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
    wm_splash("Please wait while your computer restarts.", RGB(0xFF, 0xFF, 0xFF));
    sync();
    reboot(REBOOT_RESTART);
}

static void shutdown_command(struct dialog *d, int id)
{
    int choice = d->radio;
    close_dialog(d);
    if (id != ID_YES)
        return;
    switch (choice) {
    case OPT_SHUTDOWN:
        wm_splash("Please wait while your computer shuts down.", RGB(0xFF, 0xFF, 0xFF));
        sync();
        wm_splash("It's now safe to turn off your computer.", RGB(0xFF, 0x80, 0x00));
        reboot(REBOOT_POWEROFF);
        reboot(REBOOT_HALT);
        break;
    case OPT_RESTART:
        system_restart();
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
    struct dialog *d = dialog_new("Shut Down fluxOS", ICON_SHUTDOWN, 344, 150,
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
    add(d, CT_RADIO, 66, 40, 0, 0, "Shut down the computer?", OPT_SHUTDOWN);
    add(d, CT_RADIO, 66, 60, 0, 0, "Restart the computer?", OPT_RESTART);
    add(d, CT_RADIO, 66, 80, 0, 0, "Close all programs and exit to the console?", OPT_CONSOLE);
    add(d, CT_BUTTON, 344 / 2 - 6 - 75, 150 - 12 - 23, 75, 23, "Yes", ID_YES);
    add(d, CT_BUTTON, 344 / 2 + 6, 150 - 12 - 23, 75, 23, "No", ID_NO);
    dialog_show(d, first);
}
