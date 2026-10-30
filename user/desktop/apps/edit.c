/*
 * The text editor: a Windows 95 Notepad style window with a File and an
 * Edit menu, a monospaced text area with both scroll bars, and a status
 * bar. It opens and saves plain text files and keeps CRLF files as CRLF.
 *
 * The document is one byte array. Lines are found by scanning for
 * newlines (the index is rebuilt after each change); a tab advances to
 * the next multiple of 8 columns.
 *
 * Keys: arrows, Home/End, PgUp/PgDn (with Ctrl: document start/end and
 * whole words are not special-cased), Shift extends the selection,
 * Ctrl+N/O/S new/open/save, Ctrl+A/C/X/V select all/copy/cut/paste,
 * Alt+F and Alt+E open the menus.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <flux/font.h>
#include "desktop.h"

#define ED_W       620
#define ED_H       420
#define MENU_H     20
#define STATUS_H   20
#define MAX_TEXT   (1 << 20)
#define TABSTOP    8
#define PAD        2
#define EFONT      font_archivo
#define LINE_H     (EFONT.height + 3)
#define HSTEP      16                   /* pixels an arrow of the horizontal scroll bar moves */

/* PS/2 scancodes of the letters used for shortcuts */
#define SC_A 0x1E
#define SC_C 0x2E
#define SC_E 0x12
#define SC_F 0x21
#define SC_N 0x31
#define SC_O 0x18
#define SC_S 0x1F
#define SC_V 0x2F
#define SC_X 0x2D

enum action { A_NONE, A_NEW, A_OPEN, A_CLOSE };
enum cmd {
    C_NEW, C_OPEN, C_SAVE, C_SAVEAS, C_EXIT,
    C_CUT, C_COPY, C_PASTE, C_DELETE, C_SELALL,
};

struct item {
    const char *label;                  /* NULL: separator */
    const char *shortcut;
    enum cmd cmd;
};

static const struct item file_items[] = {
    { "New", "Ctrl+N", C_NEW },
    { "Open...", "Ctrl+O", C_OPEN },
    { "Save", "Ctrl+S", C_SAVE },
    { "Save As...", NULL, C_SAVEAS },
    { NULL, NULL, 0 },
    { "Exit", NULL, C_EXIT },
};

static const struct item edit_items[] = {
    { "Cut", "Ctrl+X", C_CUT },
    { "Copy", "Ctrl+C", C_COPY },
    { "Paste", "Ctrl+V", C_PASTE },
    { "Delete", "Del", C_DELETE },
    { NULL, NULL, 0 },
    { "Select All", "Ctrl+A", C_SELALL },
};

static const struct menu {
    const char *title;
    const struct item *items;
    int n;
    char mnemonic;                      /* the underlined letter */
} menus[] = {
    { "File", file_items, 6, 'F' },
    { "Edit", edit_items, 6, 'E' },
};
#define NMENUS 2

enum hold { H_NONE, H_TEXT, H_VBAR, H_HBAR };

struct ed {
    struct window *win;
    char *text;
    int len, cap;
    int cur;                            /* caret: an index in 0..len */
    int sel;                            /* the other end of the selection, or -1 */
    int want_col;                       /* column to return to on up/down */
    int *lines;                         /* start index of every line */
    int nlines;
    int maxcol;                         /* the longest line, in pixels */
    int lines_ok;
    int anchor;                         /* where a mouse selection started */
    int vscroll, hscroll;               /* first visible line, and pixels scrolled left */
    char path[PATH_MAX];                /* "" for an untitled document */
    int modified, crlf;
    enum action after;                  /* what to do once a pending save is done */
    int focused;

    int menu;                           /* open menu, or -1 */
    int menu_hover;                     /* highlighted item, or -1 */
    int tracking;                       /* the mouse button is down in the menus */
    enum hold hold;
    int grab;                           /* where a scroll thumb was grabbed */
    enum sbar_part sbar_pressed;
    struct ed *next;
};

static struct ed *eds;
static int blink_on = 1;

/* The clipboard, shared by every editor. */
static char *clip;
static int cliplen;

/* ---- text buffer ---- */

static int reserve(struct ed *e, int need)
{
    if (need > MAX_TEXT)
        return -1;
    if (need <= e->cap)
        return 0;
    int cap = e->cap ? e->cap : 1024;
    while (cap < need)
        cap *= 2;
    char *t = realloc(e->text, (size_t)cap);
    if (!t)
        return -1;
    e->text = t;
    e->cap = cap;
    return 0;
}

static void changed(struct ed *e)
{
    e->lines_ok = 0;
    e->modified = 1;
}

/* Replace [from, to) with n bytes of s; the caret goes after the new text. */
static int replace(struct ed *e, int from, int to, const char *s, int n)
{
    if (reserve(e, e->len - (to - from) + n) < 0)
        return -1;
    memmove(e->text + from + n, e->text + to, (size_t)(e->len - to));
    memcpy(e->text + from, s, (size_t)n);
    e->len += n - (to - from);
    e->cur = from + n;
    e->sel = -1;
    changed(e);
    return 0;
}

static void sel_range(struct ed *e, int *from, int *to)
{
    if (e->sel < 0) {
        *from = *to = e->cur;
    } else {
        *from = e->sel < e->cur ? e->sel : e->cur;
        *to = e->sel < e->cur ? e->cur : e->sel;
    }
}

static int has_selection(struct ed *e)
{
    return e->sel >= 0 && e->sel != e->cur;
}

static int column_of(struct ed *e, int line, int idx);
static int line_end(struct ed *e, int line);

static void build_lines(struct ed *e)
{
    if (e->lines_ok)
        return;
    int n = 1;
    for (int i = 0; i < e->len; i++)
        if (e->text[i] == '\n')
            n++;
    int *l = realloc(e->lines, (size_t)n * sizeof(int));
    if (!l)
        return;                         /* keep the stale index; it is still safe to use */
    e->lines = l;
    e->nlines = n;
    int k = 0;
    l[k++] = 0;
    for (int i = 0; i < e->len; i++)
        if (e->text[i] == '\n')
            l[k++] = i + 1;
    e->lines_ok = 1;
    e->maxcol = 0;
    for (int l = 0; l < n; l++) {
        int c = column_of(e, l, line_end(e, l));
        if (c > e->maxcol)
            e->maxcol = c;
    }
}

static int line_of(struct ed *e, int idx)
{
    build_lines(e);
    int lo = 0, hi = e->nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (e->lines[mid] <= idx)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static int line_end(struct ed *e, int line)      /* index of the line's newline, or len */
{
    build_lines(e);
    return line + 1 < e->nlines ? e->lines[line + 1] - 1 : e->len;
}

/* Archivo is proportional, so a position in a line is a distance in pixels from its left edge. */
static int adv(unsigned char ch)
{
    static int width[128];
    if (ch < 32 || ch > 126)
        ch = '?';
    if (!width[ch]) {
        char one[2] = { (char)ch, 0 };
        width[ch] = gfx_text_width(&EFONT, one);
    }
    return width[ch];
}

/* Where the pen is after the character ch that began at x (tabs go to the next multiple of 8 spaces). */
static int pen_after(int x, unsigned char ch)
{
    if (ch == '\t') {
        int tab = TABSTOP * adv(' ');
        return (x / tab + 1) * tab;
    }
    return x + adv(ch);
}

/* The distance in pixels to index idx, which is on `line`. */
static int column_of(struct ed *e, int line, int idx)
{
    int x = 0;
    for (int i = e->lines[line]; i < idx; i++)
        x = pen_after(x, (unsigned char)e->text[i]);
    return x;
}

/* The character column of idx (what the status bar counts, tabs to multiples of TABSTOP). */
static int char_column_of(struct ed *e, int line, int idx)
{
    int col = 0;
    for (int i = e->lines[line]; i < idx; i++)
        col = e->text[i] == '\t' ? (col / TABSTOP + 1) * TABSTOP : col + 1;
    return col;
}

/* The index on `line` nearest to pixel x. */
static int index_at(struct ed *e, int line, int x)
{
    int c = 0, end = line_end(e, line);
    for (int i = e->lines[line]; i < end; i++) {
        int next = pen_after(c, (unsigned char)e->text[i]);
        if (x < c + (next - c + 1) / 2)
            return i;
        c = next;
    }
    return end;
}

/* ---- geometry ---- */

static struct rect frame_rect(struct ed *e)
{
    return mkrect(2, MENU_H + 2, e->win->content.w - 4, e->win->content.h - MENU_H - STATUS_H - 4);
}

static struct rect text_rect(struct ed *e)
{
    struct rect f = frame_rect(e);
    return mkrect(f.x + 2, f.y + 2, f.w - 4 - SBAR_W, f.h - 4 - SBAR_W);
}

static int rows(struct ed *e)
{
    return text_rect(e).h / LINE_H;
}

static int cols(struct ed *e)
{
    return text_rect(e).w - 2 * PAD;          /* pixels */
}

static struct sbar vbar(struct ed *e)
{
    struct rect t = text_rect(e);
    build_lines(e);
    return (struct sbar){ mkrect(t.x + t.w, t.y, SBAR_W, t.h), 1, e->vscroll, rows(e), e->nlines };
}

static struct sbar hbar(struct ed *e)
{
    struct rect t = text_rect(e);
    build_lines(e);
    /* A little more than the longest line, so the caret at its end shows. */
    return (struct sbar){ mkrect(t.x, t.y + t.h, t.w, SBAR_W), 0, e->hscroll, cols(e),
                          e->maxcol + 2 * PAD };
}

static struct rect menu_title_rect(int i)
{
    int x = 4;
    for (int k = 0; k < i; k++)
        x += gfx_text_width(&font_sans, menus[k].title) + 16;
    return mkrect(x, 1, gfx_text_width(&font_sans, menus[i].title) + 16, MENU_H - 2);
}

#define ITEM_H 20
#define SEP_H  8

static int item_h(const struct item *it)
{
    return it->label ? ITEM_H : SEP_H;
}

static struct rect dropdown_rect(int m)
{
    int w = 0, h = 4;
    for (int i = 0; i < menus[m].n; i++) {
        const struct item *it = &menus[m].items[i];
        h += item_h(it);
        if (it->label) {
            int iw = gfx_text_width(&font_sans, it->label) + 36 +
                     (it->shortcut ? gfx_text_width(&font_sans, it->shortcut) + 24 : 0);
            if (iw > w)
                w = iw;
        }
    }
    struct rect t = menu_title_rect(m);
    return mkrect(t.x, MENU_H, w + 8, h);
}

static struct rect item_rect(int m, int idx)
{
    struct rect d = dropdown_rect(m);
    int y = d.y + 2;
    for (int i = 0; i < idx; i++)
        y += item_h(&menus[m].items[i]);
    return mkrect(d.x + 2, y, d.w - 4, item_h(&menus[m].items[idx]));
}

static int item_enabled(struct ed *e, const struct item *it)
{
    switch (it->cmd) {
    case C_CUT:
    case C_COPY:
    case C_DELETE:
        return has_selection(e);
    case C_PASTE:
        return cliplen > 0;
    default:
        return 1;
    }
}

/* ---- drawing ---- */

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void update_title(struct ed *e)
{
    char t[96];
    snprintf(t, sizeof(t), "%s - Text Editor", e->path[0] ? base_name(e->path) : "Untitled");
    wm_set_title(e->win, t);
}

/* Draw `text` with the mnemonic letter underlined. */
static void draw_label(struct surface *s, int x, int y, const char *text, char mnemonic, color_t c)
{
    gfx_text(s, &font_sans, x, y, text, c);
    const char *p = strchr(text, mnemonic);
    if (!p)
        return;
    char one[2] = { *p, 0 };
    char head[32];
    size_t n = (size_t)(p - text) < sizeof(head) - 1 ? (size_t)(p - text) : sizeof(head) - 1;
    memcpy(head, text, n);
    head[n] = 0;
    gfx_hline(s, x + gfx_text_width(&font_sans, head), y + font_sans.height,
              gfx_text_width(&font_sans, one), c);
}

static void draw_menubar(struct ed *e, struct surface *s)
{
    gfx_fill(s, 0, 0, s->w, MENU_H, C_FACE);
    for (int i = 0; i < NMENUS; i++) {
        struct rect r = menu_title_rect(i);
        int open = e->menu == i;
        if (open)
            gfx_bevel(s, r, BEVEL_THIN_SUNKEN);
        draw_label(s, r.x + 8 + open, r.y + 3 + open, menus[i].title, menus[i].mnemonic, C_BLACK);
    }
}

static void draw_dropdown(struct ed *e, struct surface *s)
{
    if (e->menu < 0)
        return;
    int m = e->menu;
    struct rect d = dropdown_rect(m);
    gfx_fill(s, d.x, d.y, d.w, d.h, C_FACE);
    gfx_bevel(s, d, BEVEL_WINDOW);
    for (int i = 0; i < menus[m].n; i++) {
        const struct item *it = &menus[m].items[i];
        struct rect r = item_rect(m, i);
        if (!it->label) {
            draw_separator(s, r.x + 2, r.y + 3, r.w - 4);
            continue;
        }
        int enabled = item_enabled(e, it), hot = i == e->menu_hover && enabled;
        if (hot)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_NAVY);
        color_t fg = hot ? C_WHITE : enabled ? C_BLACK : C_SHADOW;
        int ty = r.y + (r.h - font_sans.height) / 2;
        if (!enabled) {
            gfx_text(s, &font_sans, r.x + 19, ty + 1, it->label, C_WHITE);
            if (it->shortcut)
                gfx_text(s, &font_sans, r.x + r.w - 9 - gfx_text_width(&font_sans, it->shortcut) + 1,
                         ty + 1, it->shortcut, C_WHITE);
        }
        gfx_text(s, &font_sans, r.x + 18, ty, it->label, fg);
        if (it->shortcut)
            gfx_text(s, &font_sans, r.x + r.w - 10 - gfx_text_width(&font_sans, it->shortcut), ty,
                     it->shortcut, fg);
    }
}

static void draw_text(struct ed *e, struct surface *s)
{
    struct rect t = text_rect(e);
    gfx_fill(s, t.x, t.y, t.w, t.h, C_WHITE);
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, t);
    build_lines(e);

    int from, to;
    sel_range(e, &from, &to);
    int vis = cols(e) + 2 * adv('W');
    int x0 = t.x + PAD - e->hscroll;

    for (int r = 0; r < rows(e) + 1; r++) {
        int line = e->vscroll + r;
        if (line >= e->nlines)
            break;
        int y = t.y + r * LINE_H, end = line_end(e, line), col = 0;
        for (int i = e->lines[line]; i <= end && col < e->hscroll + vis; i++) {
            int selected = i >= from && i < to;
            if (i == end) {             /* the newline: shown as a selected blank */
                if (selected && end < e->len)
                    gfx_fill(s, x0 + col, y, adv(' '), LINE_H, C_NAVY);
                break;
            }
            unsigned char ch = (unsigned char)e->text[i];
            int next = pen_after(col, ch);
            if (next > e->hscroll) {
                if (selected)
                    gfx_fill(s, x0 + col, y, next - col, LINE_H, C_NAVY);
                if (ch != '\t') {
                    char one[2] = { (char)ch, 0 };
                    gfx_text(s, &EFONT, x0 + col, y + 1, one, selected ? C_WHITE : C_BLACK);
                }
            }
            col = next;
        }
    }

    if (e->focused && blink_on && !has_selection(e)) {
        int line = line_of(e, e->cur), col = column_of(e, line, e->cur);
        if (line >= e->vscroll && line <= e->vscroll + rows(e) && col >= e->hscroll)
            gfx_fill(s, x0 + col, t.y + (line - e->vscroll) * LINE_H, 2, LINE_H, C_BLACK);
    }
    s->clip = saved;
}

static void paint(struct ed *e)
{
    struct surface *s = &e->win->content;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);

    draw_menubar(e, s);
    gfx_bevel(s, frame_rect(e), BEVEL_SUNKEN);
    draw_text(e, s);

    struct sbar v = vbar(e), h = hbar(e);
    sbar_draw(s, &v, e->hold == H_VBAR ? e->sbar_pressed : SBAR_NONE);
    sbar_draw(s, &h, e->hold == H_HBAR ? e->sbar_pressed : SBAR_NONE);
    struct rect corner = mkrect(v.r.x, h.r.y, SBAR_W, SBAR_W);
    gfx_fill(s, corner.x, corner.y, corner.w, corner.h, C_FACE);

    /* Status bar */
    int line = line_of(e, e->cur);
    char buf[64];
    snprintf(buf, sizeof(buf), "Ln %d, Col %d", line + 1, char_column_of(e, line, e->cur) + 1);
    gfx_bevel(s, mkrect(2, s->h - STATUS_H, s->w - 4, STATUS_H - 2), BEVEL_THIN_SUNKEN);
    gfx_text(s, &font_sans, 8, s->h - STATUS_H + 2, buf, C_BLACK);
    snprintf(buf, sizeof(buf), "%d byte%s%s", e->len, e->len == 1 ? "" : "s",
             e->modified ? "  (modified)" : "");
    gfx_text(s, &font_sans, s->w - 10 - gfx_text_width(&font_sans, buf), s->h - STATUS_H + 2, buf,
             C_BLACK);

    draw_dropdown(e, s);
    wm_damage_client(e->win, mkrect(0, 0, s->w, s->h));
}

/* ---- caret and scrolling ---- */

static void clamp_scroll(struct ed *e)
{
    struct sbar v = vbar(e), h = hbar(e);
    if (e->vscroll > sbar_max(&v))
        e->vscroll = sbar_max(&v);
    if (e->hscroll > sbar_max(&h))
        e->hscroll = sbar_max(&h);
    if (e->vscroll < 0)
        e->vscroll = 0;
    if (e->hscroll < 0)
        e->hscroll = 0;
}

static void show_caret(struct ed *e)
{
    int line = line_of(e, e->cur), col = column_of(e, line, e->cur);
    if (line < e->vscroll)
        e->vscroll = line;
    else if (line >= e->vscroll + rows(e))
        e->vscroll = line - rows(e) + 1;
    if (col < e->hscroll)
        e->hscroll = col;
    else if (col >= e->hscroll + cols(e))
        e->hscroll = col - cols(e) + 2;
    clamp_scroll(e);
    blink_on = 1;
}

static void move_caret(struct ed *e, int pos, int extend, int keep_col)
{
    if (extend && e->sel < 0)
        e->sel = e->cur;
    else if (!extend)
        e->sel = -1;
    e->cur = pos < 0 ? 0 : pos > e->len ? e->len : pos;
    if (e->sel == e->cur)
        e->sel = -1;
    if (!keep_col)
        e->want_col = column_of(e, line_of(e, e->cur), e->cur);
    show_caret(e);
}

static void move_lines(struct ed *e, int delta, int extend)
{
    int line = line_of(e, e->cur) + delta;
    if (line < 0) {
        move_caret(e, 0, extend, 0);
        return;
    }
    if (line >= e->nlines) {
        move_caret(e, e->len, extend, 0);
        return;
    }
    move_caret(e, index_at(e, line, e->want_col), extend, 1);
}

/* ---- loading and saving ---- */

static void message(const char *what, const char *name, int err)
{
    char msg[PATH_MAX + 96];
    snprintf(msg, sizeof(msg), "Cannot %s %s: %s.", what, name, strerror(err));
    message_box("Text Editor", ICON_WARNING, msg);
}

static void new_document(struct ed *e)
{
    e->len = 0;
    e->cur = 0;
    e->sel = -1;
    e->vscroll = e->hscroll = 0;
    e->want_col = 0;
    e->path[0] = 0;
    e->crlf = 0;
    changed(e);
    e->modified = 0;
    update_title(e);
}

static int load_file(struct ed *e, const char *path)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0) {
        int err = errno;
        if (fd >= 0)
            close(fd);
        message("open", base_name(path), err);
        return -1;
    }
    if (S_ISDIR(st.st_mode) || st.st_size > MAX_TEXT) {
        close(fd);
        message_box("Text Editor", ICON_WARNING,
                    S_ISDIR(st.st_mode) ? "That is a folder, not a text file."
                                        : "The file is too large to edit (1 MB limit).");
        return -1;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    int got = 0, r = 1;
    while (buf && got < (int)st.st_size && (r = (int)read(fd, buf + got, (size_t)st.st_size - (size_t)got)) > 0)
        got += r;
    int err = errno;
    close(fd);
    if (!buf || r < 0) {
        free(buf);
        message("read", base_name(path), buf ? err : ENOMEM);
        return -1;
    }

    /* A file whose every line ends in CRLF is shown without the CRs. */
    int crlf = 0, bare = 0;
    for (int i = 0; i < got; i++) {
        if (buf[i] == '\n') {
            if (i > 0 && buf[i - 1] == '\r')
                crlf = 1;
            else
                bare = 1;
        }
    }
    e->crlf = crlf && !bare;
    int n = 0;
    for (int i = 0; i < got; i++)
        if (!(e->crlf && buf[i] == '\r' && i + 1 < got && buf[i + 1] == '\n'))
            buf[n++] = buf[i];
    if (reserve(e, n) < 0) {
        free(buf);
        message("open", base_name(path), ENOMEM);
        return -1;
    }
    memcpy(e->text, buf, (size_t)n);
    free(buf);
    e->len = n;
    e->cur = 0;
    e->sel = -1;
    e->vscroll = e->hscroll = 0;
    e->want_col = 0;
    snprintf(e->path, sizeof(e->path), "%s", path);
    changed(e);
    e->modified = 0;
    update_title(e);
    return 0;
}

static int write_all(int fd, const char *p, int n)
{
    while (n > 0) {
        int w = (int)write(fd, p, (size_t)n);
        if (w <= 0)
            return -1;
        p += w;
        n -= w;
    }
    return 0;
}

static int save_file(struct ed *e, const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    int ok = fd >= 0;
    if (ok && e->crlf) {                /* put the CRs back, line by line */
        int start = 0;
        for (int i = 0; ok && i <= e->len; i++) {
            if (i == e->len || e->text[i] == '\n') {
                ok = write_all(fd, e->text + start, i - start) == 0 &&
                     (i == e->len || write_all(fd, "\r\n", 2) == 0);
                start = i + 1;
            }
        }
    } else if (ok) {
        ok = write_all(fd, e->text, e->len) == 0;
    }
    if (ok)
        ok = fsync(fd) == 0;
    int err = errno;
    if (fd >= 0)
        close(fd);
    if (!ok) {
        message("save", base_name(path), err);
        return -1;
    }
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->modified = 0;
    update_title(e);
    return 0;
}

/* A typed name: absolute as is, otherwise inside the home folder. */
static void resolve(const char *name, char *out, size_t size)
{
    const char *home = getenv("HOME");
    if (name[0] == '/' || !home)
        snprintf(out, size, "%s", name);
    else
        snprintf(out, size, "%s%s%s", home, strcmp(home, "/") ? "/" : "", name);
}

/* ---- actions and prompts ---- */

static struct ed *alive(void *ctx)
{
    for (struct ed *e = eds; e; e = e->next)
        if (e == ctx)
            return e;
    return NULL;
}

static void perform(struct ed *e, enum action a);
static void begin_save(struct ed *e, enum action then);

static void open_done(void *ctx, const char *name)
{
    struct ed *e = alive(ctx);
    char path[PATH_MAX];
    if (!e)
        return;
    resolve(name, path, sizeof(path));
    if (load_file(e, path) == 0)
        show_caret(e);
    paint(e);
}

static void save_as_done(void *ctx, const char *name)
{
    struct ed *e = alive(ctx);
    char path[PATH_MAX];
    if (!e)
        return;
    resolve(name, path, sizeof(path));
    if (!strchr(base_name(path), '.') && strlen(path) + 4 < sizeof(path))
        strcat(path, ".txt");           /* like Notepad: .txt unless you give an extension */
    enum action then = e->after;
    e->after = A_NONE;
    if (save_file(e, path) == 0 && then != A_NONE) {
        perform(e, then);
        return;
    }
    paint(e);
}

static void ask_path(struct ed *e, const char *title, const char *initial,
                     void (*done)(void *, const char *))
{
    ask_text(title, "File name:", initial, done, e);
}

static void begin_save(struct ed *e, enum action then)
{
    e->after = then;
    if (!e->path[0]) {
        char initial[PATH_MAX];
        resolve("untitled.txt", initial, sizeof(initial));
        ask_path(e, "Save As", initial, save_as_done);
        return;
    }
    e->after = A_NONE;
    if (save_file(e, e->path) == 0 && then != A_NONE)
        perform(e, then);
    else
        paint(e);
}

static void perform(struct ed *e, enum action a)
{
    switch (a) {
    case A_NEW:
        new_document(e);
        paint(e);
        break;
    case A_OPEN: {
        char initial[PATH_MAX];
        resolve("", initial, sizeof(initial));
        ask_path(e, "Open", initial, open_done);
        break;
    }
    case A_CLOSE:
        wm_destroy(e->win);
        break;
    case A_NONE:
        break;
    }
}

static void save_yes(void *ctx)
{
    struct ed *e = alive(ctx);
    if (e)
        begin_save(e, e->after);
}

static void save_no(void *ctx)
{
    struct ed *e = alive(ctx);
    if (!e)
        return;
    enum action a = e->after;
    e->after = A_NONE;
    perform(e, a);
}

/* Do `a`, after asking whether to keep unsaved changes. */
static void request(struct ed *e, enum action a)
{
    if (!e->modified) {
        perform(e, a);
        return;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "Save changes to %s?", e->path[0] ? base_name(e->path) : "Untitled");
    e->after = a;
    ask_yes_no_cancel("Text Editor", ICON_WARNING, msg, save_yes, save_no, e);
}

/* ---- editing commands ---- */

static void copy_selection(struct ed *e)
{
    int from, to;
    sel_range(e, &from, &to);
    if (from == to)
        return;
    char *c = malloc((size_t)(to - from));
    if (!c)
        return;
    memcpy(c, e->text + from, (size_t)(to - from));
    free(clip);
    clip = c;
    cliplen = to - from;
}

/* The shared clipboard, for the other programs. */
void clipboard_set(const char *text)
{
    size_t n = strlen(text);
    char *c = malloc(n ? n : 1);
    if (!c)
        return;
    memcpy(c, text, n);
    free(clip);
    clip = c;
    cliplen = (int)n;
}

const char *clipboard_get(int *len)
{
    *len = cliplen;
    return clip;
}

static void delete_selection(struct ed *e)
{
    int from, to;
    sel_range(e, &from, &to);
    if (from != to)
        replace(e, from, to, "", 0);
}

static void insert_text(struct ed *e, const char *s, int n)
{
    int from, to;
    sel_range(e, &from, &to);
    if (replace(e, from, to, s, n) < 0)
        message_box("Text Editor", ICON_WARNING, "The document is too large (1 MB limit).");
    e->want_col = column_of(e, line_of(e, e->cur), e->cur);
    show_caret(e);
}

static void run_command(struct ed *e, enum cmd c)
{
    switch (c) {
    case C_NEW: request(e, A_NEW); return;
    case C_OPEN: request(e, A_OPEN); return;
    case C_SAVE: begin_save(e, A_NONE); return;
    case C_SAVEAS: {
        e->after = A_NONE;
        char initial[PATH_MAX];
        if (e->path[0])
            snprintf(initial, sizeof(initial), "%s", e->path);
        else
            resolve("untitled.txt", initial, sizeof(initial));
        ask_path(e, "Save As", initial, save_as_done);
        return;
    }
    case C_EXIT: request(e, A_CLOSE); return;
    case C_CUT:
        copy_selection(e);
        delete_selection(e);
        break;
    case C_COPY: copy_selection(e); break;
    case C_PASTE:
        if (cliplen)
            insert_text(e, clip, cliplen);
        break;
    case C_DELETE: delete_selection(e); break;
    case C_SELALL:
        e->sel = 0;
        e->cur = e->len;
        if (e->sel == e->cur)
            e->sel = -1;
        show_caret(e);
        break;
    }
    show_caret(e);
    paint(e);
}

/* ---- menus ---- */

static void set_menu(struct ed *e, int m)
{
    e->menu = m;
    e->menu_hover = -1;
    e->tracking = 0;
}

static int menu_item_at(struct ed *e, int x, int y)
{
    if (e->menu < 0)
        return -1;
    for (int i = 0; i < menus[e->menu].n; i++)
        if (menus[e->menu].items[i].label && rect_contains(item_rect(e->menu, i), x, y))
            return i;
    return -1;
}

static int menu_title_at(int x, int y)
{
    for (int i = 0; i < NMENUS; i++)
        if (rect_contains(menu_title_rect(i), x, y))
            return i;
    return -1;
}

static void menu_step(struct ed *e, int dir)
{
    int n = menus[e->menu].n, i = e->menu_hover;
    for (int k = 0; k < n; k++) {
        i = (i + dir + n) % n;
        if (menus[e->menu].items[i].label && item_enabled(e, &menus[e->menu].items[i])) {
            e->menu_hover = i;
            return;
        }
    }
}

static void menu_key(struct ed *e, const struct input_event *ev)
{
    if (ev->code == KEY_ESC) {
        set_menu(e, -1);
    } else if (ev->code == KEY_UP) {
        menu_step(e, -1);
    } else if (ev->code == KEY_DOWN) {
        menu_step(e, 1);
    } else if (ev->code == KEY_LEFT || ev->code == KEY_RIGHT) {
        set_menu(e, (e->menu + (ev->code == KEY_LEFT ? NMENUS - 1 : 1)) % NMENUS);
    } else if (ev->code == KEY_ENTER && e->menu_hover >= 0) {
        enum cmd c = menus[e->menu].items[e->menu_hover].cmd;
        set_menu(e, -1);
        paint(e);
        run_command(e, c);
        return;
    }
    paint(e);
}

/* ---- keyboard ---- */

static void ed_key(struct window *w, const struct input_event *ev)
{
    struct ed *e = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    int ctrl = ev->mods & KMOD_CTRL, alt = ev->mods & KMOD_ALT, shift = ev->mods & KMOD_SHIFT;

    if (e->menu >= 0) {
        menu_key(e, ev);
        return;
    }
    if (alt && (ev->code == SC_F || ev->code == SC_E)) {
        set_menu(e, ev->code == SC_F ? 0 : 1);
        menu_step(e, 1);
        paint(e);
        return;
    }
    if (ctrl) {
        switch (ev->code) {
        case SC_N: run_command(e, C_NEW); return;
        case SC_O: run_command(e, C_OPEN); return;
        case SC_S: run_command(e, C_SAVE); return;
        case SC_A: run_command(e, C_SELALL); return;
        case SC_C: run_command(e, C_COPY); return;
        case SC_X: run_command(e, C_CUT); return;
        case SC_V: run_command(e, C_PASTE); return;
        case KEY_HOME: move_caret(e, 0, shift, 0); paint(e); return;
        case KEY_END: move_caret(e, e->len, shift, 0); paint(e); return;
        default: return;
        }
    }

    int line = line_of(e, e->cur);
    switch (ev->code) {
    case KEY_LEFT: move_caret(e, e->cur - 1, shift, 0); break;
    case KEY_RIGHT: move_caret(e, e->cur + 1, shift, 0); break;
    case KEY_UP: move_lines(e, -1, shift); break;
    case KEY_DOWN: move_lines(e, 1, shift); break;
    case KEY_PGUP: move_lines(e, -rows(e), shift); break;
    case KEY_PGDN: move_lines(e, rows(e), shift); break;
    case KEY_HOME: move_caret(e, e->lines[line], shift, 0); break;
    case KEY_END: move_caret(e, line_end(e, line), shift, 0); break;
    case KEY_DELETE:
        if (has_selection(e))
            delete_selection(e);
        else if (e->cur < e->len)
            replace(e, e->cur, e->cur + 1, "", 0);
        show_caret(e);
        break;
    default:
        if (ev->len != 1)
            return;
        char c = ev->text[0];
        if (c == 0x7F) {                /* backspace */
            if (has_selection(e))
                delete_selection(e);
            else if (e->cur > 0)
                replace(e, e->cur - 1, e->cur, "", 0);
            e->want_col = column_of(e, line_of(e, e->cur), e->cur);
            show_caret(e);
        } else if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) {
            insert_text(e, &c, 1);
        } else {
            return;
        }
        break;
    }
    paint(e);
}

/* ---- mouse ---- */

static int index_at_point(struct ed *e, int x, int y)
{
    struct rect t = text_rect(e);
    build_lines(e);
    int line = e->vscroll + (y - t.y) / LINE_H - (y < t.y ? 1 : 0);
    if (line < 0)
        line = 0;
    if (line >= e->nlines)
        line = e->nlines - 1;
    int px = x - t.x - PAD + e->hscroll;
    return index_at(e, line, px < 0 ? 0 : px);
}

static void scroll_step(struct ed *e, struct sbar *b, enum sbar_part part)
{
    int *pos = b->vertical ? &e->vscroll : &e->hscroll;
    int unit = b->vertical ? 1 : HSTEP;
    int page = b->page > unit ? b->page - unit : unit;
    switch (part) {
    case SBAR_ARROW1: *pos -= unit; break;
    case SBAR_ARROW2: *pos += unit; break;
    case SBAR_PAGE1: *pos -= page; break;
    case SBAR_PAGE2: *pos += page; break;
    default: break;
    }
    clamp_scroll(e);
}

static void ed_mouse(struct window *w, int x, int y, int buttons, int changed_buttons)
{
    struct ed *e = w->priv;
    int left = buttons & MOUSE_LEFT;
    int pressed = (changed_buttons & MOUSE_LEFT) && left;
    int released = (changed_buttons & MOUSE_LEFT) && !left;

    /* ---- menus ---- */
    int title = menu_title_at(x, y), item = menu_item_at(e, x, y);
    if (e->menu >= 0 && !pressed && !released) {
        int hover = item >= 0 && item_enabled(e, &menus[e->menu].items[item]) ? item : -1;
        int switched = title >= 0 && title != e->menu;
        if (switched)
            set_menu(e, title);
        if (hover != e->menu_hover || switched) {
            e->menu_hover = hover;
            paint(e);
        }
        return;
    }
    if (pressed && (title >= 0 || e->menu >= 0)) {
        if (title >= 0) {
            set_menu(e, e->menu == title ? -1 : title);
            e->tracking = e->menu >= 0;
        } else if (item >= 0) {
            e->tracking = 1;
            e->menu_hover = item_enabled(e, &menus[e->menu].items[item]) ? item : -1;
        } else {
            set_menu(e, -1);            /* a click elsewhere only closes the menu */
        }
        paint(e);
        return;
    }
    if (released && e->tracking) {
        e->tracking = 0;
        if (e->menu >= 0 && item >= 0 && item_enabled(e, &menus[e->menu].items[item])) {
            enum cmd c = menus[e->menu].items[item].cmd;
            set_menu(e, -1);
            paint(e);
            run_command(e, c);
            return;
        }
        if (title < 0 && item < 0)
            set_menu(e, -1);
        paint(e);
        return;
    }

    /* ---- scroll bars and text ---- */
    struct sbar v = vbar(e), h = hbar(e);
    if (pressed) {
        enum sbar_part pv = sbar_hit(&v, x, y), ph = sbar_hit(&h, x, y);
        if (pv != SBAR_NONE || ph != SBAR_NONE) {
            struct sbar *b = pv != SBAR_NONE ? &v : &h;
            enum sbar_part part = pv != SBAR_NONE ? pv : ph;
            e->hold = b->vertical ? H_VBAR : H_HBAR;
            e->sbar_pressed = part;
            if (part == SBAR_THUMB) {
                struct rect t = sbar_thumb(b);
                e->grab = b->vertical ? y - t.y : x - t.x;
            } else {
                scroll_step(e, b, part);
            }
        } else if (rect_contains(text_rect(e), x, y)) {
            e->hold = H_TEXT;
            move_caret(e, index_at_point(e, x, y), 0, 0);
            e->anchor = e->cur;
        } else {
            return;
        }
    } else if (released) {
        e->hold = H_NONE;
    } else if (e->hold == H_TEXT && left) {
        e->cur = index_at_point(e, x, y);
        e->sel = e->cur == e->anchor ? -1 : e->anchor;
        e->want_col = column_of(e, line_of(e, e->cur), e->cur);
        show_caret(e);
    } else if (e->hold == H_VBAR && e->sbar_pressed == SBAR_THUMB) {
        e->vscroll = sbar_drag(&v, y, e->grab);
    } else if (e->hold == H_HBAR && e->sbar_pressed == SBAR_THUMB) {
        e->hscroll = sbar_drag(&h, x, e->grab);
    } else {
        return;
    }
    paint(e);
}

/* ---- window callbacks ---- */

static void ed_focus(struct window *w, int focused)
{
    struct ed *e = w->priv;
    e->focused = focused;
    if (!focused)
        set_menu(e, -1);
    if (e->win)                         /* not while wm_create() runs */
        paint(e);
}

static void ed_resize(struct window *w)
{
    struct ed *e = w->priv;
    show_caret(e);
    paint(e);
}

static void ed_close(struct window *w)
{
    request(w->priv, A_CLOSE);
}

static void ed_destroy(struct window *w)
{
    struct ed *e = w->priv;
    for (struct ed **pp = &eds; *pp; pp = &(*pp)->next) {
        if (*pp == e) {
            *pp = e->next;
            break;
        }
    }
    free(e->text);
    free(e->lines);
    free(e);
}

static const struct window_ops ed_ops = {
    .key = ed_key,
    .mouse = ed_mouse,
    .focus = ed_focus,
    .resize = ed_resize,
    .close = ed_close,
    .destroy = ed_destroy,
};

void editor_blink(void)
{
    blink_on = !blink_on;
    for (struct ed *e = eds; e; e = e->next)
        if (e->focused && e->menu < 0)
            paint(e);
}

struct window *editor_open(const char *path)
{
    struct ed *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->sel = -1;
    e->menu = -1;
    e->menu_hover = -1;
    e->lines_ok = 0;
    struct window *w = wm_create("Untitled - Text Editor", ICON_EDITOR, ED_W, ED_H,
                                 WF_MINIMIZE | WF_MAXIMIZE, &ed_ops, e);
    if (!w) {
        free(e);
        return NULL;
    }
    e->win = w;
    e->next = eds;
    eds = e;
    if (path)
        load_file(e, path);             /* an unreadable file leaves an empty document */
    update_title(e);
    paint(e);
    return w;
}
