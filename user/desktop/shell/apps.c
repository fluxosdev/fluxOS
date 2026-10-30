/*
 * Dialogs, and the message boxes and prompts every program asks with.
 * Each is a window whose client area holds a few controls; the dialog code
 * handles focus, Tab/Enter/Escape and mouse presses the way Windows 95
 * dialogs do. The desktop's own (System Properties, Run, Shut Down and
 * Power) are in sysdlg.c.
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
#include "dialog.h"

static struct dialog *ask_dlg;

struct control *dialog_add(struct dialog *d, enum ctl_type type, int x, int y, int w, int h,
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

void dialog_paint(struct dialog *d)
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

void dialog_close(struct dialog *d)
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
        dialog_close(d);
}

static void dlg_focus(struct window *w, int focused)
{
    struct dialog *d = w->priv;
    (void)focused;
    if (d->win)                         /* not while wm_create() runs */
        dialog_paint(d);
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
    dialog_paint(d);
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
        dialog_paint(d);
    } else if (changed & MOUSE_LEFT) {                          /* release */
        int p = d->pressed;
        int in = d->pressed_in;
        d->pressed = -1;
        d->pressed_in = 0;
        if (p >= 0) {
            dialog_paint(d);
            if (in && hit == p)
                activate(d, d->ctl[p].id);
        }
    } else if (d->pressed >= 0) {                               /* drag */
        int in = hit == d->pressed;
        if (in != d->pressed_in) {
            d->pressed_in = in;
            dialog_paint(d);
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

struct dialog *dialog_new(const char *title, enum icon_id icon, int w, int h, int flags,
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

void dialog_show(struct dialog *d, int focus_index)
{
    d->focus = focus_index;
    wm_center(d->win);
    dialog_paint(d);
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
    dialog_close(d);                    /* frees d */
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
    dialog_add(d, CT_ICON, 14, 14, 32, 32, NULL, icon);
    for (int i = 0; i < nlines; i++)
        dialog_add(d, CT_LABEL, 62, text_y + i * line_h, 0, 0, lines[i], 0);
    int first = d->n;
    for (int i = 0; i < nbuttons; i++)
        dialog_add(d, CT_BUTTON, (w - bw) / 2 + i * 81, h - 12 - 23, 75, 23, labels[i], ids[i]);
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
    dialog_add(d, CT_LABEL, 12, 12, 0, 0, label, 0);
    int edit = d->n;
    dialog_add(d, CT_EDIT, 12, 32, 296, 21, NULL, 0);
    dialog_add(d, CT_BUTTON, 320 - 12 - 75 - 6 - 75, 104 - 12 - 23, 75, 23, "OK", ID_OK);
    dialog_add(d, CT_BUTTON, 320 - 12 - 75, 104 - 12 - 23, 75, 23, "Cancel", ID_CANCEL);
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
    dialog_paint(ask_dlg);
    return 1;
}
