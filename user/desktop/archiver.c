/*
 * Archive Manager: browse ZIP and 7z archives like folders and extract
 * what they hold.
 *
 * The archive's directory is read here (libc's archive.c), and shown one
 * folder at a time. Extracting runs /bin/unzip with -P, which reports its
 * progress line by line through a pipe, so the desktop goes on while a big
 * archive unpacks: a progress bar and a Cancel button take the status
 * bar's place. Opening a file in the archive extracts it to /tmp first.
 */
#include <archive.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "desktop.h"

#define AW_W        600
#define AW_H        400
#define PAD         6
#define TB_H        34
#define BTN_H       24
#define LOC_H       22
#define HDR_H       18
#define ROW_H       18
#define STATUS_H    22
#define DBLCLICK_MS 500
#define UNZIP       "/bin/unzip"
#define VIEW_DIR    "/tmp/archive-manager"
#define PATH_LEN    1024

enum { B_OPEN, B_EXTRACT_ALL, B_EXTRACT, B_UP, B_TEST, NBTNS };
static const char *const btn_labels[NBTNS] = { "Open...", "Extract All...", "Extract...", "Up", "Test" };
static const int btn_w[NBTNS] = { 70, 98, 80, 50, 56 };
#define B_CANCEL 100                    /* the status bar's button while a job runs */

enum { COL_NAME, COL_SIZE, COL_PACKED, COL_DATE, COL_METHOD, NCOLS };
static const char *const col_names[NCOLS] = { "Name", "Size", "Packed", "Modified", "Method" };
static const int col_w[NCOLS] = { 0, 76, 76, 112, 86 };     /* the name takes the rest */

enum { JOB_NONE, JOB_EXTRACT, JOB_TEST, JOB_VIEW };
enum { HOLD_NONE, HOLD_BTN, HOLD_HDR, HOLD_ARROW, HOLD_THUMB };

struct row {
    char *name;
    int dir;
    int entry;                          /* the archive entry, -1 for a folder known only from paths */
    uint32_t size, csize, mtime;
    const char *method;
    enum icon_id icon;
    uint8_t sel;
};

struct arcwin {
    struct window *win;
    struct arcwin *next;
    char path[PATH_LEN];
    struct archive *a;
    char cwd[PATH_LEN];                 /* the folder shown, inside the archive ("" at the top) */
    struct row *rows;
    int nrows, cap;
    int scroll, cursor, anchor;
    int sort_col, sort_desc;
    int last_click;
    unsigned last_click_ms;
    int hold, hold_id, hold_in, grab;
    int mods;                           /* Ctrl and Shift, as the last key event said */
    enum sbar_part sbar_pressed;
    /* the running job */
    int job, pid, fd, exited, eof, exit_ok, cancelled;
    uint32_t done, total;
    int nbegun, nfailed, nskipped;
    char current[PATH_LEN];
    char errors[600];
    char dest[PATH_LEN];
    char view_name[PATH_LEN];           /* JOB_VIEW: the file to open afterwards */
    char listfile[64];                  /* the names unzip is to extract, while it runs */
    char line[PATH_LEN + 64];
    int linelen;
    int overwrite;                      /* for the job about to start */
    uint8_t *pending;                   /* names chosen before the destination was asked */
};

static struct arcwin *wins;

static struct arcwin *alive(void *ctx)
{
    for (struct arcwin *w = wins; w; w = w->next)
        if (w == ctx)
            return w;
    return NULL;
}

/* ---- the rows of the folder shown ---- */

static int has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name), k = strlen(suffix);
    return n >= k && !strcasecmp(name + n - k, suffix);
}

static enum icon_id icon_for(const char *name, int dir)
{
    if (dir)
        return ICON_FOLDER;
    static const char *const images[] = { ".png", ".jpg", ".jpeg", ".gif", ".svg", ".ico", ".bmp" };
    for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); i++)
        if (has_suffix(name, images[i]))
            return ICON_IMAGE;
    if (has_suffix(name, ".html") || has_suffix(name, ".htm"))
        return ICON_HTML;
    if (has_suffix(name, ".wav") || has_suffix(name, ".voc") || has_suffix(name, ".mp3"))
        return ICON_SOUND;
    if (has_suffix(name, ".zip") || has_suffix(name, ".7z"))
        return ICON_ARCHIVE;
    if (has_suffix(name, ".exe") || has_suffix(name, ".com"))
        return ICON_RUN;
    return ICON_README;
}

static void free_rows(struct arcwin *w)
{
    for (int i = 0; i < w->nrows; i++)
        free(w->rows[i].name);
    w->nrows = 0;
}

static struct row *add_row(struct arcwin *w, const char *name, int len, int dir)
{
    if (w->nrows == w->cap) {
        int cap = w->cap ? w->cap * 2 : 64;
        struct row *r = realloc(w->rows, sizeof(*r) * (size_t)cap);
        if (!r)
            return NULL;
        w->rows = r;
        w->cap = cap;
    }
    struct row *r = &w->rows[w->nrows];
    memset(r, 0, sizeof(*r));
    r->name = malloc((size_t)len + 1);
    if (!r->name)
        return NULL;
    memcpy(r->name, name, (size_t)len);
    r->name[len] = 0;
    r->dir = dir;
    r->entry = -1;
    r->method = "";
    r->icon = icon_for(r->name, dir);
    w->nrows++;
    return r;
}

static struct row *find_dir(struct arcwin *w, const char *name, int len)
{
    for (int i = w->nrows - 1; i >= 0; i--)
        if (w->rows[i].dir && (int)strlen(w->rows[i].name) == len && !memcmp(w->rows[i].name, name, (size_t)len))
            return &w->rows[i];
    return NULL;
}

static int sort_col_g, sort_desc_g;

static int row_cmp(const void *pa, const void *pb)
{
    const struct row *a = pa, *b = pb;
    if (a->dir != b->dir)
        return b->dir - a->dir;         /* folders first */
    int c = 0;
    if (sort_col_g == COL_SIZE)
        c = a->size < b->size ? -1 : a->size > b->size;
    else if (sort_col_g == COL_PACKED)
        c = a->csize < b->csize ? -1 : a->csize > b->csize;
    else if (sort_col_g == COL_DATE)
        c = a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
    else if (sort_col_g == COL_METHOD)
        c = strcmp(a->method, b->method);
    if (!c)
        c = strcasecmp(a->name, b->name);
    return sort_desc_g ? -c : c;
}

static void sort_rows(struct arcwin *w)
{
    sort_col_g = w->sort_col;
    sort_desc_g = w->sort_desc;
    qsort(w->rows, (size_t)w->nrows, sizeof(*w->rows), row_cmp);
}

/* The entries directly in cwd: files, and folders (with the sizes of all they hold). */
static void build_rows(struct arcwin *w)
{
    free_rows(w);
    if (!w->a)
        return;
    size_t plen = strlen(w->cwd);
    for (int i = 0; i < arc_count(w->a); i++) {
        const struct arc_entry *e = arc_entry(w->a, i);
        const char *rest = e->name;
        if (plen) {
            if (strncmp(rest, w->cwd, plen) || rest[plen] != '/')
                continue;
            rest += plen + 1;
        }
        if (!*rest)
            continue;
        const char *slash = strchr(rest, '/');
        if (slash || e->is_dir) {
            int len = slash ? (int)(slash - rest) : (int)strlen(rest);
            struct row *r = find_dir(w, rest, len);
            if (!r && !(r = add_row(w, rest, len, 1)))
                break;
            if (!slash) {
                r->entry = i;
                r->mtime = e->mtime;
            }
            r->size += e->size;
            r->csize += e->csize;
        } else {
            struct row *r = add_row(w, rest, (int)strlen(rest), 0);
            if (!r)
                break;
            r->entry = i;
            r->size = e->size;
            r->csize = e->csize;
            r->mtime = e->mtime;
            r->method = e->encrypted ? "Encrypted" : e->method;
        }
    }
    sort_rows(w);
    w->scroll = 0;
    w->cursor = w->nrows ? 0 : -1;
    w->anchor = w->cursor;
}

/* ---- layout ---- */

static struct rect button_rect(const struct arcwin *w, int i)
{
    (void)w;
    int x = PAD;
    for (int k = 0; k < i; k++)
        x += btn_w[k] + 4;
    return mkrect(x, (TB_H - BTN_H) / 2, btn_w[i], BTN_H);
}

static struct rect loc_rect(const struct arcwin *w)
{
    return mkrect(PAD, TB_H, w->win->content.w - 2 * PAD, LOC_H);
}

static struct rect list_rect(const struct arcwin *w)
{
    const struct surface *s = &w->win->content;
    int top = TB_H + LOC_H + 4 + HDR_H;
    return mkrect(PAD, top, s->w - 2 * PAD - SBAR_W, s->h - top - STATUS_H - 6);
}

static struct rect status_rect(const struct arcwin *w)
{
    const struct surface *s = &w->win->content;
    return mkrect(PAD, s->h - STATUS_H - 2, s->w - 2 * PAD, STATUS_H);
}

static struct rect cancel_rect(const struct arcwin *w)
{
    struct rect st = status_rect(w);
    return mkrect(st.x + st.w - 76, st.y + 1, 76, st.h - 2);
}

static int visible_rows(const struct arcwin *w)
{
    int n = (list_rect(w).h - 4) / ROW_H;
    return n < 1 ? 1 : n;
}

static struct sbar list_bar(const struct arcwin *w)
{
    struct rect l = list_rect(w);
    return (struct sbar){ mkrect(l.x + l.w, l.y, SBAR_W, l.h), 1, w->scroll, visible_rows(w), w->nrows };
}

static void clamp_scroll(struct arcwin *w)
{
    struct sbar b = list_bar(w);
    int max = sbar_max(&b);
    if (w->scroll > max)
        w->scroll = max;
    if (w->scroll < 0)
        w->scroll = 0;
}

static void scroll_to_cursor(struct arcwin *w)
{
    int vis = visible_rows(w);
    if (w->cursor < 0)
        return;
    if (w->cursor < w->scroll)
        w->scroll = w->cursor;
    else if (w->cursor >= w->scroll + vis)
        w->scroll = w->cursor - vis + 1;
    clamp_scroll(w);
}

static void col_layout(const struct arcwin *w, int *x, int *cw)
{
    int total = list_rect(w).w - 4, fixed = 0;
    for (int c = 0; c < NCOLS; c++)
        fixed += col_w[c];
    int pos = 0;
    for (int c = 0; c < NCOLS; c++) {
        cw[c] = col_w[c] ? col_w[c] : (total > fixed + 80 ? total - fixed : 80);
        x[c] = pos;
        pos += cw[c];
    }
}

static int nselected(const struct arcwin *w)
{
    int n = 0;
    for (int i = 0; i < w->nrows; i++)
        n += w->rows[i].sel;
    return n;
}

static int button_enabled(const struct arcwin *w, int b)
{
    if (w->job)
        return 0;
    switch (b) {
    case B_OPEN: return 1;
    case B_EXTRACT_ALL: case B_TEST: return w->a != NULL;
    case B_EXTRACT: return w->a && nselected(w) > 0;
    case B_UP: return w->a && w->cwd[0];
    }
    return 0;
}

/* ---- drawing ---- */

static void text_in(struct surface *s, int x, int y, int w, const char *str, color_t c)
{
    char buf[PATH_LEN];
    gfx_ellipsize(&font_sans, str, w, buf, sizeof(buf));
    gfx_text(s, &font_sans, x, y, buf, c);
}

static void text_right(struct surface *s, int right, int y, const char *str, color_t c)
{
    gfx_text(s, &font_sans, right - gfx_text_width(&font_sans, str), y, str, c);
}

static void fmt_size(char *out, size_t n, uint32_t v)
{
    if (v < 1024)
        snprintf(out, n, "%u B", v);
    else if (v < 10u * 1024 * 1024)
        snprintf(out, n, "%u KB", (v + 1023) / 1024);
    else
        snprintf(out, n, "%u MB", (v + 512 * 1024) / (1024 * 1024));
}

static void fmt_date(char *out, size_t n, uint32_t t)
{
    if (!t) {
        out[0] = 0;
        return;
    }
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void draw_toolbar(struct arcwin *w, struct surface *s)
{
    for (int i = 0; i < NBTNS; i++) {
        struct rect r = button_rect(w, i);
        int down = w->hold == HOLD_BTN && w->hold_id == i && w->hold_in;
        draw_button(s, r, btn_labels[i], down, 0, 0);
        if (!button_enabled(w, i))
            gfx_dither(s, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_FACE);
    }
    gfx_hline(s, 0, TB_H - 2, s->w, C_SHADOW);
    gfx_hline(s, 0, TB_H - 1, s->w, C_WHITE);
}

static void draw_location(struct arcwin *w, struct surface *s)
{
    struct rect r = loc_rect(w);
    char text[PATH_LEN * 2];
    if (w->a)
        snprintf(text, sizeof(text), "%s%s%s", base_name(w->path), w->cwd[0] ? "/" : "", w->cwd);
    else
        snprintf(text, sizeof(text), "No archive open");
    gfx_text(s, &font_sans, r.x, r.y + (r.h - font_sans.height) / 2, "Location:", C_BLACK);
    int lx = r.x + gfx_text_width(&font_sans, "Location:") + 8;
    struct rect f = mkrect(lx, r.y + 1, r.x + r.w - lx, r.h - 2);
    gfx_fill(s, f.x, f.y, f.w, f.h, C_WHITE);
    gfx_bevel(s, f, BEVEL_SUNKEN);
    draw_icon(s, f.x + 3, f.y + (f.h - 16) / 2, w->cwd[0] ? ICON_FOLDER : ICON_ARCHIVE, 16);
    text_in(s, f.x + 22, f.y + (f.h - font_sans.height) / 2, f.w - 28, text, C_BLACK);
}

static void draw_list(struct arcwin *w, struct surface *s)
{
    struct rect l = list_rect(w);
    int cx[NCOLS], cw[NCOLS];
    col_layout(w, cx, cw);
    for (int c = 0; c < NCOLS; c++) {           /* headings: buttons that sort */
        struct rect h = mkrect(l.x + 2 + cx[c], l.y - HDR_H, cw[c], HDR_H);
        if (c == 0)
            h.x = l.x, h.w += 2;
        if (c == NCOLS - 1)
            h.w = l.x + l.w + SBAR_W - h.x;
        int down = w->hold == HOLD_HDR && w->hold_id == c && w->hold_in;
        gfx_fill(s, h.x, h.y, h.w, h.h, C_FACE);
        gfx_bevel(s, h, down ? BEVEL_PRESSED : BEVEL_RAISED);
        int ty = h.y + (h.h - font_sans.height) / 2 + down;
        if (c == COL_SIZE || c == COL_PACKED)
            text_right(s, h.x + h.w - 16 + down, ty, col_names[c], C_BLACK);
        else
            gfx_text(s, &font_sans, h.x + 6 + down, ty, col_names[c], C_BLACK);
        if (c == w->sort_col) {
            int ax = h.x + h.w - 12, ay = h.y + h.h / 2 - 1;
            for (int i = 0; i < 4; i++)
                gfx_hline(s, ax + (w->sort_desc ? i : 3 - i), ay + (w->sort_desc ? i : 3 - i) - 1,
                          w->sort_desc ? 7 - 2 * i : 1 + 2 * i, C_BLACK);
        }
    }
    struct rect box = mkrect(l.x, l.y, l.w + SBAR_W, l.h);
    gfx_fill(s, l.x, l.y, l.w, l.h, C_WHITE);
    gfx_bevel(s, box, BEVEL_SUNKEN);
    struct sbar bar = list_bar(w);
    sbar_draw(s, &bar, w->hold == HOLD_ARROW || w->hold == HOLD_THUMB ? w->sbar_pressed : SBAR_NONE);

    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, mkrect(l.x + 2, l.y + 2, l.w - 4, l.h - 4));
    if (!w->a) {
        const char *msg = "Open a ZIP or 7z archive with Open..., or double-click one in the File Manager.";
        gfx_text_wrap(s, &font_sans, l.x + 12, l.y + 12, l.w - 24, msg, C_SHADOW);
    } else if (!w->nrows) {
        gfx_text(s, &font_sans, l.x + 12, l.y + 12, "This folder is empty.", C_SHADOW);
    }
    int vis = visible_rows(w);
    for (int k = 0; k <= vis && w->scroll + k < w->nrows; k++) {
        int i = w->scroll + k;
        const struct row *r = &w->rows[i];
        int y = l.y + 2 + k * ROW_H, ty = y + (ROW_H - font_sans.height) / 2;
        if (r->sel)
            gfx_fill(s, l.x + 2, y, l.w - 4, ROW_H, C_NAVY);
        color_t fg = r->sel ? C_WHITE : C_BLACK;
        int x0 = l.x + 2;
        draw_icon(s, x0 + 3, y + (ROW_H - 16) / 2, r->icon, 16);
        text_in(s, x0 + 22, ty, cw[COL_NAME] - 26, r->name, fg);
        char buf[48];
        fmt_size(buf, sizeof(buf), r->size);
        text_right(s, x0 + cx[COL_SIZE] + cw[COL_SIZE] - 6, ty, buf, fg);
        if (r->csize) {
            fmt_size(buf, sizeof(buf), r->csize);
            text_right(s, x0 + cx[COL_PACKED] + cw[COL_PACKED] - 6, ty, buf, fg);
        }
        fmt_date(buf, sizeof(buf), r->mtime);
        gfx_text(s, &font_sans, x0 + cx[COL_DATE] + 6, ty, buf, fg);
        text_in(s, x0 + cx[COL_METHOD] + 6, ty, cw[COL_METHOD] - 8, r->method, fg);
        if (i == w->cursor && wm_focused() == w->win)
            gfx_focus_rect(s, mkrect(l.x + 2, y, l.w - 4, ROW_H));
    }
    s->clip = saved;
}

static void draw_status(struct arcwin *w, struct surface *s)
{
    struct rect st = status_rect(w);
    char text[PATH_LEN + 64];
    if (w->job) {
        struct rect c = cancel_rect(w);
        draw_button(s, c, "Cancel", w->hold == HOLD_BTN && w->hold_id == B_CANCEL && w->hold_in, 0, 0);
        struct rect bar = mkrect(st.x, st.y, 160, st.h);
        gfx_bevel(s, bar, BEVEL_THIN_SUNKEN);
        uint32_t total = w->total, done = w->done;
        while (total > 0xFFFFF) {           /* keep done * width within 32 bits */
            total >>= 4;
            done >>= 4;
        }
        int fill = total ? (int)(done * (uint32_t)(bar.w - 4) / total) : 0;
        if (fill > bar.w - 4)
            fill = bar.w - 4;
        if (fill > 0)
            gfx_fill(s, bar.x + 2, bar.y + 2, fill, bar.h - 4, C_NAVY);
        snprintf(text, sizeof(text), "%s %s", w->job == JOB_TEST ? "Testing" : "Extracting",
                 w->current[0] ? base_name(w->current) : "...");
        struct rect t = mkrect(bar.x + bar.w + 4, st.y, c.x - bar.x - bar.w - 8, st.h);
        gfx_bevel(s, t, BEVEL_THIN_SUNKEN);
        text_in(s, t.x + 6, t.y + (t.h - font_sans.height) / 2, t.w - 10, text, C_BLACK);
        return;
    }
    gfx_bevel(s, st, BEVEL_THIN_SUNKEN);
    if (!w->a) {
        snprintf(text, sizeof(text), "Ready");
    } else {
        uint32_t total = 0;
        for (int i = 0; i < w->nrows; i++)
            total += w->rows[i].size;
        char size[32];
        fmt_size(size, sizeof(size), total);
        int sel = nselected(w);
        snprintf(text, sizeof(text), "%d item%s, %s   |   %s archive, %d entries%s", w->nrows,
                 w->nrows == 1 ? "" : "s", size, arc_type(w->a) == ARC_7Z ? "7z" : "ZIP",
                 arc_count(w->a), arc_solid(w->a) ? " (solid)" : "");
        if (sel) {
            char extra[48];
            snprintf(extra, sizeof(extra), "   |   %d selected", sel);
            strncat(text, extra, sizeof(text) - strlen(text) - 1);
        }
    }
    text_in(s, st.x + 6, st.y + (st.h - font_sans.height) / 2, st.w - 12, text, C_BLACK);
}

static void paint(struct arcwin *w)
{
    struct surface *s = &w->win->content;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);
    clamp_scroll(w);
    draw_toolbar(w, s);
    draw_location(w, s);
    draw_list(w, s);
    draw_status(w, s);
    wm_damage_client(w->win, mkrect(0, 0, s->w, s->h));
}

/* ---- opening ---- */

static void set_title(struct arcwin *w)
{
    char t[64];
    if (w->a)
        snprintf(t, sizeof(t), "%.40s - Archive Manager", base_name(w->path));
    else
        snprintf(t, sizeof(t), "Archive Manager");
    wm_set_title(w->win, t);
}

static int load(struct arcwin *w, const char *path)
{
    char err[256];
    struct archive *a = arc_open(path, err, sizeof(err));
    if (!a) {
        char msg[PATH_LEN + 300];
        snprintf(msg, sizeof(msg), "Cannot open %s: %s.", base_name(path), err);
        message_box("Archive Manager", ICON_WARNING, msg);
        return -1;
    }
    arc_close(w->a);
    w->a = a;
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->cwd[0] = 0;
    build_rows(w);
    set_title(w);
    return 0;
}

static void go_to(struct arcwin *w, const char *dir)
{
    char old[PATH_LEN];
    snprintf(old, sizeof(old), "%s", w->cwd);
    snprintf(w->cwd, sizeof(w->cwd), "%s", dir);
    build_rows(w);
    /* going up: the cursor on the folder we came from */
    size_t n = strlen(w->cwd);
    if (n < strlen(old)) {
        const char *child = old + (n ? n + 1 : 0);
        for (int i = 0; i < w->nrows; i++)
            if (w->rows[i].dir && !strncmp(w->rows[i].name, child, strlen(w->rows[i].name)) &&
                (child[strlen(w->rows[i].name)] == '/' || !child[strlen(w->rows[i].name)])) {
                w->cursor = w->anchor = i;
                w->rows[i].sel = 1;
                scroll_to_cursor(w);
                break;
            }
    }
}

static void go_up(struct arcwin *w)
{
    if (!w->cwd[0])
        return;
    char up[PATH_LEN];
    snprintf(up, sizeof(up), "%s", w->cwd);
    char *slash = strrchr(up, '/');
    if (slash)
        *slash = 0;
    else
        up[0] = 0;
    go_to(w, up);
}

static void do_open(void *ctx, const char *text)
{
    struct arcwin *w = alive(ctx);
    if (!w)
        return;
    load(w, text);
    paint(w);
}

/* ---- jobs: unzip in a child process ---- */

static void full_name(const struct arcwin *w, const struct row *r, char *out, int size)
{
    snprintf(out, (size_t)size, "%s%s%s", w->cwd, w->cwd[0] ? "/" : "", r->name);
}

static void finish_job(struct arcwin *w);

static void handle_line(struct arcwin *w, char *line)
{
    if (line[0] == 'F' && line[1] == ' ') {
        char *p = strchr(line + 2, ' ');
        snprintf(w->current, sizeof(w->current), "%s", p ? p + 1 : line + 2);
        w->nbegun++;
    } else if (line[0] == 'P' && line[1] == ' ') {
        unsigned long done = strtoul(line + 2, &line, 10), total = strtoul(line, NULL, 10);
        w->done = (uint32_t)done;
        w->total = (uint32_t)total;
    } else if (line[0] == 'S' && line[1] == ' ') {
        w->nskipped++;
    } else if (line[0] == 'D' && line[1] == ' ') {
        /* the end: the count of failures is known from the E lines */
    } else if (line[0]) {
        const char *what = line;
        if (line[0] == 'E' && line[1] == ' ') {
            w->nfailed++;
            const char *p = strchr(line + 2, ' ');
            what = p ? p + 1 : line + 2;
        } else if (!strncmp(line, "unzip: ", 7)) {
            what = line + 7;
            w->nfailed++;
        }
        size_t l = strlen(w->errors);
        if (l < sizeof(w->errors) - 80)
            snprintf(w->errors + l, sizeof(w->errors) - l, "%s\n", what);
    }
}

static void output_ready(void *ctx, int revents)
{
    struct arcwin *w = alive(ctx);
    if (!w)
        return;
    char buf[1024];
    int n = (revents & (POLLIN | POLLHUP)) ? (int)read(w->fd, buf, sizeof(buf)) : 0;
    if (n <= 0) {
        loop_unwatch(w->fd);
        close(w->fd);
        w->fd = -1;
        w->eof = 1;
        if (w->exited)
            finish_job(w);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            w->line[w->linelen] = 0;
            handle_line(w, w->line);
            w->linelen = 0;
        } else if (w->linelen < (int)sizeof(w->line) - 1) {
            w->line[w->linelen++] = buf[i];
        }
    }
    paint(w);
}

void archiver_child_exited(int pid, int status)
{
    for (struct arcwin *w = wins; w; w = w->next) {
        if (!w->job || w->pid != pid)
            continue;
        w->pid = 0;
        w->exited = 1;
        w->exit_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (w->eof)
            finish_job(w);
        return;
    }
}

static void open_dest(void *ctx)
{
    struct arcwin *w = alive(ctx);
    if (w)
        files_open(w->dest);
}

static void finish_job(struct arcwin *w)
{
    int job = w->job;
    w->job = JOB_NONE;
    if (w->listfile[0]) {
        unlink(w->listfile);
        w->listfile[0] = 0;
    }
    w->hold = HOLD_NONE;
    paint(w);
    char msg[1200];
    if (w->cancelled) {
        snprintf(msg, sizeof(msg), "%s was cancelled.", job == JOB_TEST ? "The test" : "Extracting");
        message_box("Archive Manager", ICON_INFO, msg);
        return;
    }
    if (job == JOB_VIEW) {
        char path[PATH_LEN * 2];
        snprintf(path, sizeof(path), "%s/%s", w->dest, w->view_name);
        struct stat st;
        if (w->nfailed || stat(path, &st) < 0) {
            snprintf(msg, sizeof(msg), "Cannot open %s from the archive.\n%s", w->view_name, w->errors);
            message_box("Archive Manager", ICON_WARNING, msg);
        } else {
            files_open_path(path);
        }
        return;
    }
    if (w->nfailed || (!w->exit_ok && !w->nbegun)) {
        snprintf(msg, sizeof(msg), "%s %d file%s.\n\n%s", job == JOB_TEST ? "The test found errors in" : "Could not extract",
                 w->nfailed, w->nfailed == 1 ? "" : "s", w->errors);
        message_box("Archive Manager", ICON_WARNING, msg);
        return;
    }
    if (job == JOB_TEST) {
        snprintf(msg, sizeof(msg), "No errors were found in %s.", base_name(w->path));
        message_box("Archive Manager", ICON_INFO, msg);
        return;
    }
    char skipped[96] = "";
    if (w->nskipped)
        snprintf(skipped, sizeof(skipped), " %d file%s that were there already were left as they were.",
                 w->nskipped, w->nskipped == 1 ? "" : "s");
    snprintf(msg, sizeof(msg), "Extracted to %s.%s\n\nOpen the folder?", w->dest, skipped);
    ask_yes_no("Archive Manager", ICON_INFO, msg, open_dest, w);
}

/* Run unzip on the archive: names (NULL: everything) under dest. */
static void start_job(struct arcwin *w, int job, const char *dest, char **names, int nnames, int flat)
{
    static int serial;
    char listfile[64] = "";
    if (nnames > 0) {                   /* the names go in a file: there may be many */
        mkdir("/tmp", 01777);
        snprintf(listfile, sizeof(listfile), "/tmp/archive-names-%d-%d", getpid(), ++serial);
        FILE *f = fopen(listfile, "w");
        if (!f) {
            message_box("Archive Manager", ICON_WARNING, "Cannot write the list of files to extract in /tmp.");
            return;
        }
        for (int i = 0; i < nnames; i++)
            fprintf(f, "%s\n", names[i]);
        fclose(f);
    }
    int fds[2];
    if (pipe(fds) < 0) {
        message_box("Archive Manager", ICON_WARNING, "Cannot start unzip.");
        return;
    }
    int pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDONLY);
        dup2(null, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        for (int fd = 3; fd < 64; fd++)
            close(fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        char *argv[16];
        int n = 0;
        argv[n++] = "unzip";
        argv[n++] = "-P";
        argv[n++] = job == JOB_TEST ? "-t" : w->overwrite ? "-o" : "-n";
        if (flat)
            argv[n++] = "-j";
        if (job != JOB_TEST) {
            argv[n++] = "-d";
            argv[n++] = (char *)dest;
        }
        if (listfile[0]) {
            argv[n++] = "-@";
            argv[n++] = listfile;
        }
        argv[n++] = w->path;
        argv[n] = NULL;
        execve(UNZIP, argv, environ);
        printf("unzip: cannot run %s: %s\n", UNZIP, strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        if (listfile[0])
            unlink(listfile);
        message_box("Archive Manager", ICON_WARNING, "Cannot start unzip.");
        return;
    }
    snprintf(w->listfile, sizeof(w->listfile), "%s", listfile);
    w->job = job;
    w->pid = pid;
    w->fd = fds[0];
    w->exited = w->eof = w->exit_ok = w->cancelled = 0;
    w->done = w->total = 0;
    w->nbegun = w->nfailed = w->nskipped = 0;
    w->current[0] = w->errors[0] = 0;
    w->linelen = 0;
    snprintf(w->dest, sizeof(w->dest), "%s", dest ? dest : "");
    loop_watch(w->fd, output_ready, w);
    paint(w);
}

/* The names of the selected rows (folders stand for all they hold); NULL: none chosen. */
static char **chosen_names(struct arcwin *w, const uint8_t *chosen, int *n)
{
    *n = 0;
    if (!chosen)
        return NULL;
    char **v = calloc((size_t)w->nrows + 1, sizeof(char *));
    for (int i = 0; v && i < w->nrows; i++) {
        if (!chosen[i])
            continue;
        char name[PATH_LEN * 2];
        full_name(w, &w->rows[i], name, sizeof(name));
        v[(*n)++] = strdup(name);
    }
    return v;
}

static void free_names(char **v, int n)
{
    for (int i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

static void run_extract(struct arcwin *w)
{
    int n;
    char **names = chosen_names(w, w->pending, &n);
    start_job(w, JOB_EXTRACT, w->dest, names, n, 0);
    free_names(names, n);
    free(w->pending);
    w->pending = NULL;
}

static void replace_yes(void *ctx)
{
    struct arcwin *w = alive(ctx);
    if (w && !w->job) {
        w->overwrite = 1;
        run_extract(w);
    }
}

static void replace_no(void *ctx)
{
    struct arcwin *w = alive(ctx);
    if (w && !w->job) {
        w->overwrite = 0;
        run_extract(w);
    }
}

static void dest_chosen(void *ctx, const char *text)
{
    struct arcwin *w = alive(ctx);
    if (!w || w->job)
        return;
    snprintf(w->dest, sizeof(w->dest), "%s", text);
    size_t l = strlen(w->dest);
    while (l > 1 && w->dest[l - 1] == '/')
        w->dest[--l] = 0;
    struct stat st;
    if (stat(w->dest, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            message_box("Archive Manager", ICON_WARNING, "That is a file, not a folder.");
            return;
        }
        ask_yes_no_cancel("Archive Manager", ICON_WARNING,
                          "The folder is there already. Replace the files in it that have the same names?",
                          replace_yes, replace_no, w);
        return;
    }
    char inside[PATH_LEN + 4];
    snprintf(inside, sizeof(inside), "%s/.", w->dest);
    arc_make_parents(inside);           /* the folder, and any above it that are missing */
    if (stat(w->dest, &st) < 0) {
        char msg[PATH_LEN + 80];
        snprintf(msg, sizeof(msg), "Cannot make the folder %s: %s.", w->dest, strerror(errno));
        message_box("Archive Manager", ICON_WARNING, msg);
        return;
    }
    w->overwrite = 0;
    run_extract(w);
}

/* Ask where to extract; selected: only the selected rows. */
static void ask_extract(struct arcwin *w, int selected)
{
    free(w->pending);
    w->pending = NULL;
    if (selected) {
        w->pending = calloc((size_t)w->nrows + 1, 1);
        if (!w->pending)
            return;
        for (int i = 0; i < w->nrows; i++)
            w->pending[i] = w->rows[i].sel;
    }
    /* next to the archive, in a folder named after it; at home if that cannot be written */
    char dir[PATH_LEN], name[PATH_LEN], def[PATH_LEN * 2];
    snprintf(dir, sizeof(dir), "%s", w->path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof(dir), ".");
    if (!dir[0])
        snprintf(dir, sizeof(dir), "/");
    if (access(dir, W_OK) < 0) {
        const char *home = getenv("HOME");
        snprintf(dir, sizeof(dir), "%s", home ? home : "/");
    }
    snprintf(name, sizeof(name), "%s", base_name(w->path));
    char *dot = strrchr(name, '.');
    if (dot && dot != name)
        *dot = 0;
    snprintf(def, sizeof(def), "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", name);
    ask_text(selected ? "Extract" : "Extract All", "Extract the files to this folder:", def, dest_chosen, w);
}

/* Double-clicking a file: extract it to /tmp and open it there. */
static void view_row(struct arcwin *w, int i)
{
    const struct row *r = &w->rows[i];
    const struct arc_entry *e = arc_entry(w->a, r->entry);
    if (!e || !e->supported) {
        message_box("Archive Manager", ICON_WARNING, e && e->encrypted ? "This file is encrypted." :
                    "This file uses a compression method the Archive Manager cannot unpack.");
        return;
    }
    char dest[PATH_LEN];
    mkdir("/tmp", 01777);
    mkdir(VIEW_DIR, 0755);
    snprintf(dest, sizeof(dest), VIEW_DIR "/%d", (int)(now_ms() % 100000));
    mkdir(dest, 0755);
    char name[PATH_LEN * 2], *names[1] = { name };
    full_name(w, r, name, sizeof(name));
    snprintf(w->view_name, sizeof(w->view_name), "%s", r->name);
    w->overwrite = 1;
    start_job(w, JOB_VIEW, dest, names, 1, 1);
}

static void open_row(struct arcwin *w, int i)
{
    if (i < 0 || i >= w->nrows)
        return;
    if (w->rows[i].dir) {
        char dir[PATH_LEN * 2];
        full_name(w, &w->rows[i], dir, sizeof(dir));
        go_to(w, dir);
    } else if (!w->job) {
        view_row(w, i);
    }
}

static void cancel_job(struct arcwin *w)
{
    if (!w->job || w->cancelled)
        return;
    w->cancelled = 1;
    if (w->pid > 0)
        kill(w->pid, SIGTERM);
}

static void button_action(struct arcwin *w, int b)
{
    if (b == B_CANCEL) {
        cancel_job(w);
        return;
    }
    if (!button_enabled(w, b))
        return;
    switch (b) {
    case B_OPEN: {
        const char *home = getenv("HOME");
        char def[PATH_LEN];
        snprintf(def, sizeof(def), "%s", w->a ? w->path : home ? home : "/");
        ask_text("Open Archive", "The ZIP or 7z file to open:", def, do_open, w);
        break;
    }
    case B_EXTRACT_ALL: ask_extract(w, 0); break;
    case B_EXTRACT: ask_extract(w, 1); break;
    case B_UP: go_up(w); break;
    case B_TEST: start_job(w, JOB_TEST, NULL, NULL, 0, 0); break;
    }
}

/* ---- input ---- */

static void select_only(struct arcwin *w, int i)
{
    for (int k = 0; k < w->nrows; k++)
        w->rows[k].sel = k == i;
    w->cursor = w->anchor = i;
}

static void select_range(struct arcwin *w, int i)
{
    int a = w->anchor < 0 ? i : w->anchor, lo = a < i ? a : i, hi = a < i ? i : a;
    for (int k = 0; k < w->nrows; k++)
        w->rows[k].sel = k >= lo && k <= hi;
    w->cursor = i;
}

static int button_at(const struct arcwin *w, int x, int y)
{
    if (w->job && rect_contains(cancel_rect(w), x, y))
        return B_CANCEL;
    for (int i = 0; i < NBTNS; i++)
        if (rect_contains(button_rect(w, i), x, y))
            return i;
    return -1;
}

static int header_at(const struct arcwin *w, int x, int y)
{
    struct rect l = list_rect(w);
    if (y < l.y - HDR_H || y >= l.y || x < l.x || x >= l.x + l.w + SBAR_W)
        return -1;
    int cx[NCOLS], cw[NCOLS];
    col_layout(w, cx, cw);
    for (int c = 0; c < NCOLS; c++)
        if (x < l.x + 2 + cx[c] + cw[c] || c == NCOLS - 1)
            return c;
    return -1;
}

static void scroll_step(struct arcwin *w, const struct sbar *b, enum sbar_part part)
{
    switch (part) {
    case SBAR_ARROW1: w->scroll -= 1; break;
    case SBAR_ARROW2: w->scroll += 1; break;
    case SBAR_PAGE1: w->scroll -= b->page > 1 ? b->page - 1 : 1; break;
    case SBAR_PAGE2: w->scroll += b->page > 1 ? b->page - 1 : 1; break;
    default: break;
    }
    clamp_scroll(w);
}

static void aw_mouse(struct window *win, int x, int y, int buttons, int changed)
{
    struct arcwin *w = win->priv;
    int left = buttons & MOUSE_LEFT;
    int pressed = (changed & MOUSE_LEFT) && left, released = (changed & MOUSE_LEFT) && !left;
    struct sbar bar = list_bar(w);
    if (pressed) {
        int b = button_at(w, x, y), h;
        enum sbar_part part;
        struct rect l = list_rect(w);
        if (b >= 0) {
            if (b != B_CANCEL && !button_enabled(w, b))
                return;
            w->hold = HOLD_BTN;
            w->hold_id = b;
            w->hold_in = 1;
        } else if ((h = header_at(w, x, y)) >= 0) {
            w->hold = HOLD_HDR;
            w->hold_id = h;
            w->hold_in = 1;
        } else if ((part = sbar_hit(&bar, x, y)) != SBAR_NONE) {
            w->sbar_pressed = part;
            if (part == SBAR_THUMB) {
                w->hold = HOLD_THUMB;
                w->grab = y - sbar_thumb(&bar).y;
            } else {
                w->hold = HOLD_ARROW;
                scroll_step(w, &bar, part);
            }
        } else if (rect_contains(l, x, y) && w->a) {
            int i = w->scroll + (y - l.y - 2) / ROW_H;
            if (i >= w->nrows) {
                for (int k = 0; k < w->nrows; k++)
                    w->rows[k].sel = 0;
                paint(w);
                return;
            }
            unsigned now = now_ms();
            int dbl = i == w->last_click && now - w->last_click_ms < DBLCLICK_MS;
            w->last_click = i;
            w->last_click_ms = now;
            int mods = w->mods;
            if (mods & KMOD_CTRL) {
                w->rows[i].sel = !w->rows[i].sel;
                w->cursor = w->anchor = i;
            } else if (mods & KMOD_SHIFT) {
                select_range(w, i);
            } else {
                select_only(w, i);
            }
            paint(w);
            if (dbl && !(mods & (KMOD_CTRL | KMOD_SHIFT))) {
                w->last_click = -1;
                open_row(w, i);
                paint(w);
            }
            return;
        } else {
            return;
        }
    } else if (released) {
        int hold = w->hold, id = w->hold_id, in = w->hold_in;
        w->hold = HOLD_NONE;
        if (hold == HOLD_BTN && in) {
            paint(w);
            button_action(w, id);
        } else if (hold == HOLD_HDR && in) {
            if (w->sort_col == id)
                w->sort_desc = !w->sort_desc;
            else
                w->sort_col = id, w->sort_desc = id == COL_SIZE || id == COL_PACKED || id == COL_DATE;
            sort_rows(w);
        }
    } else if ((w->hold == HOLD_BTN || w->hold == HOLD_HDR) && left) {
        int in = w->hold == HOLD_BTN ? button_at(w, x, y) == w->hold_id : header_at(w, x, y) == w->hold_id;
        if (in == w->hold_in)
            return;
        w->hold_in = in;
    } else if (w->hold == HOLD_THUMB && left) {
        w->scroll = sbar_drag(&bar, y, w->grab);
    } else {
        return;
    }
    paint(w);
}

static void aw_wheel(struct window *win, int steps)
{
    struct arcwin *w = win->priv;
    w->scroll -= steps * 3;
    clamp_scroll(w);
    paint(w);
}

static void move_cursor(struct arcwin *w, int to, int shift)
{
    if (!w->nrows)
        return;
    if (to < 0)
        to = 0;
    if (to >= w->nrows)
        to = w->nrows - 1;
    if (shift)
        select_range(w, to);
    else
        select_only(w, to);
    scroll_to_cursor(w);
}

static void aw_key(struct window *win, const struct input_event *ev)
{
    struct arcwin *w = win->priv;
    w->mods = ev->mods;
    if (!(ev->flags & IEV_PRESSED))
        return;
    int shift = ev->mods & KMOD_SHIFT, c = w->cursor, vis = visible_rows(w);
    if (ev->len == 1 && ev->text[0] == 0x01) {         /* Ctrl+A */
        for (int i = 0; i < w->nrows; i++)
            w->rows[i].sel = 1;
    } else if (ev->len == 1 && ev->text[0] == 0x0F) {  /* Ctrl+O */
        button_action(w, B_OPEN);
        return;
    } else if (ev->code == KEY_UP) {
        move_cursor(w, c - 1, shift);
    } else if (ev->code == KEY_DOWN) {
        move_cursor(w, c + 1, shift);
    } else if (ev->code == KEY_HOME) {
        move_cursor(w, 0, shift);
    } else if (ev->code == KEY_END) {
        move_cursor(w, w->nrows - 1, shift);
    } else if (ev->code == KEY_PGUP) {
        move_cursor(w, c - vis, shift);
    } else if (ev->code == KEY_PGDN) {
        move_cursor(w, c + vis, shift);
    } else if (ev->code == KEY_ENTER) {
        open_row(w, c);
    } else if (ev->len == 1 && ev->text[0] == 0x08) {  /* Backspace */
        go_up(w);
    } else if (ev->code == KEY_ESC) {
        cancel_job(w);
    } else if (ev->code == KEY_F5 && w->a && !w->job) {
        char path[PATH_LEN];
        snprintf(path, sizeof(path), "%s", w->path);
        char cwd[PATH_LEN];
        snprintf(cwd, sizeof(cwd), "%s", w->cwd);
        if (load(w, path) == 0)
            go_to(w, cwd);
    } else {
        return;
    }
    paint(w);
}

static void aw_resize(struct window *win)
{
    paint(win->priv);
}

static void aw_focus(struct window *win, int focused)
{
    (void)focused;
    struct arcwin *w = win->priv;
    w->mods = 0;                        /* keys held elsewhere are not ours */
    if (w->win)
        paint(w);
}

static void aw_destroy(struct window *win)
{
    struct arcwin *w = win->priv;
    if (w->pid > 0)
        kill(w->pid, SIGTERM);
    if (w->fd >= 0) {
        loop_unwatch(w->fd);
        close(w->fd);
    }
    for (struct arcwin **pp = &wins; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    free_rows(w);
    free(w->rows);
    free(w->pending);
    arc_close(w->a);
    free(w);
}

static const struct window_ops aw_ops = {
    .key = aw_key,
    .mouse = aw_mouse,
    .wheel = aw_wheel,
    .resize = aw_resize,
    .focus = aw_focus,
    .destroy = aw_destroy,
};

struct window *archiver_open(const char *path)
{
    struct arcwin *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->fd = -1;
    w->cursor = w->anchor = w->last_click = -1;
    w->sort_col = COL_NAME;
    struct window *win = wm_create("Archive Manager", ICON_ARCHIVE, AW_W, AW_H, WF_MINIMIZE | WF_MAXIMIZE,
                                   &aw_ops, w);
    if (!win) {
        free(w);
        return NULL;
    }
    w->win = win;
    w->next = wins;
    wins = w;
    wm_set_min_size(win, 460, 240);
    if (path)
        load(w, path);
    paint(w);
    return win;
}

void archiver_extract(const char *path)
{
    struct window *win = archiver_open(path);
    struct arcwin *w = win ? win->priv : NULL;
    if (w && w->a)
        ask_extract(w, 0);
}
