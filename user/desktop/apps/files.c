/*
 * The file manager: a folder window in the style of Windows 95's large
 * icon view, with a toolbar (Up, New Folder, Delete, Rename), an address
 * field, a scroll bar and a status bar. Double-clicking a folder opens
 * it in the same window; a program runs in a terminal window, a PNG or
 * JPEG picture opens in the image viewer, a text file in the text editor, a WAV sound opens
 * in the sound player, a VOC one plays with play(1), and anything else is shown with more(1).
 *
 * Keys: arrows move the selection, Enter opens, Backspace goes up,
 * Delete deletes, F2 renames and F5 reloads the folder. Ctrl+X, Ctrl+C
 * and Ctrl+V cut, copy and paste (also between disks and the CD), and
 * Ctrl+A selects everything; dragging a box with the mouse selects several
 * items. Clicking the
 * address field or Ctrl+L lets you type a path to go to.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include "desktop.h"

#define FM_W       560
#define FM_H       380
#define TOOLBAR_H  30
#define ADDR_Y     32
#define ADDR_H     22
#define VIEW_Y     (ADDR_Y + ADDR_H + 4)
#define STATUS_H   20
#define CELL_W     80
#define CELL_H     68
#define SB_W       16
#define LINE_STEP  CELL_H
#define DBLCLICK_MS 500

struct entry {
    char name[NAME_MAX + 1];
    int is_dir;
    int selected;
    enum icon_id icon;
};

enum { TB_UP, TB_NEWDIR, TB_CUT, TB_COPY, TB_PASTE, TB_DELETE, TB_RENAME, NTOOLS };

static const struct {
    const char *label;
    enum icon_id icon;
} tools[NTOOLS] = {
    { "Up", ICON_UP },
    { "New Folder", ICON_FOLDER_NEW },
    { "Cut", ICON_NONE },
    { "Copy", ICON_NONE },
    { "Paste", ICON_NONE },
    { "Delete", ICON_DELETE },
    { "Rename", ICON_NONE },
};

/* The clipboard is shared by all folder windows. */
static char (*clip)[PATH_MAX];
static int clip_n;
static int clip_cut;

/* What the mouse is holding down. */
enum hold { HOLD_NONE, HOLD_TOOL, HOLD_THUMB, HOLD_ARROW_UP, HOLD_ARROW_DOWN, HOLD_BAND, HOLD_ITEM };

#define DRAG_SLOP 4

struct fm {
    struct window *win;
    char path[PATH_MAX];
    struct entry *ents;
    int n, cap;
    int sel;                            /* the primary selected entry, or -1 */
    int band_x0, band_y0;               /* rubber band start (y includes the scroll) */
    int band_x1, band_y1;               /* rubber band end, in window coordinates */
    int scroll;                         /* view offset in pixels */
    enum hold hold;
    int hold_tool, hold_in;
    int thumb_grab;                     /* where the thumb was grabbed */
    int item_x, item_y;                 /* where an item was pressed */
    int item_hit;                       /* the entry pressed, for HOLD_ITEM */
    int dragging;                       /* HOLD_ITEM moved far enough to be a drag */
    int last_click;
    unsigned int last_click_ms;
    char pending[NAME_MAX + 1];         /* entry a delete/rename prompt is about */
    int editing;                        /* typing in the address field */
    char addr[PATH_MAX];
    int addr_len, addr_cur;
    int ctx_open;                       /* the right-click menu */
    int ctx_x, ctx_y, ctx_hover;
    int ctx_n;
    int ctx_codes[10];
    struct fm *next;
};

static struct fm *fms;

/* The folder a drag is hovering over: the entry drop_i of window drop_f, or -1 for the window. */
static struct fm *drop_f;
static int drop_i = -1;

/* ---- paths and directory loading ---- */

static void join(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

static int is_program(const char *path)
{
    char head[4];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    int n = (int)read(fd, head, sizeof(head));
    close(fd);
    return (n == 4 && !memcmp(head, "\x7f" "ELF", 4)) || (n >= 2 && head[0] == '#' && head[1] == '!');
}

static int has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name), k = strlen(suffix);
    return n > k && !strcasecmp(name + n - k, suffix);
}

/* A DOS program, which runs under /bin/dos: .COM, or an MZ executable. */
static int is_dos_program(const char *path)
{
    if (has_suffix(path, ".com"))
        return 1;
    if (!has_suffix(path, ".exe"))
        return 0;
    char head[2];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    int n = (int)read(fd, head, sizeof(head));
    close(fd);
    return n == 2 && ((head[0] == 'M' && head[1] == 'Z') || (head[0] == 'Z' && head[1] == 'M'));
}

/* Files the double-click opens in the text editor. */
static int is_text_name(const char *name)
{
    static const char *const suffixes[] = { ".txt", ".md", ".log", ".cfg", ".conf", ".ini", ".c", ".h" };
    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
        if (has_suffix(name, suffixes[i]))
            return 1;
    return 0;
}

static enum icon_id classify(struct fm *f, const struct entry *e, const char *parent_fs)
{
    char path[PATH_MAX];
    join(path, sizeof(path), f->path, e->name);
    if (e->is_dir) {
        const char *home = getenv("HOME");
        if (home && !strcmp(path, home))
            return ICON_HOME;
        /* A mount point has a different filesystem from its parent. */
        struct statfs st;
        if (statfs(path, &st) == 0 && strcmp(st.f_fstype, parent_fs))
            return !strcmp(st.f_fstype, "iso9660") ? ICON_CDROM : ICON_DRIVE;
        return ICON_FOLDER;
    }
    if (has_suffix(e->name, ".png") || has_suffix(e->name, ".jpg") ||
        has_suffix(e->name, ".jpeg") || has_suffix(e->name, ".jpe") || has_suffix(e->name, ".jfif") ||
        has_suffix(e->name, ".gif") || has_suffix(e->name, ".svg") || has_suffix(e->name, ".ico"))
        return ICON_IMAGE;
    if (has_suffix(e->name, ".html") || has_suffix(e->name, ".htm"))
        return ICON_HTML;
    if (has_suffix(e->name, ".wav") || has_suffix(e->name, ".voc"))
        return ICON_SOUND;
    if (has_suffix(e->name, ".zip") || has_suffix(e->name, ".7z"))
        return ICON_ARCHIVE;
    struct stat st;
    if (is_dos_program(path) || (stat(path, &st) == 0 && (st.st_mode & 0111) && is_program(path)))
        return ICON_RUN;
    return ICON_README;
}

static int compare(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    if (x->is_dir != y->is_dir)
        return y->is_dir - x->is_dir;   /* folders first */
    return strcasecmp(x->name, y->name);
}

static void set_title(struct fm *f)
{
    wm_set_title(f->win, f->path);
}

static void load(struct fm *f)
{
    f->n = 0;
    DIR *d = opendir(f->path);
    if (!d)
        return;
    struct statfs pst;
    if (statfs(f->path, &pst) < 0)
        pst.f_fstype[0] = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (f->n == f->cap) {
            int cap = f->cap ? f->cap * 2 : 32;
            struct entry *ne = realloc(f->ents, (size_t)cap * sizeof(*ne));
            if (!ne)
                break;
            f->ents = ne;
            f->cap = cap;
        }
        struct entry *e = &f->ents[f->n++];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->is_dir = de->d_type == DT_DIR;
        e->selected = 0;
        e->icon = classify(f, e, pst.f_fstype);
    }
    closedir(d);
    qsort(f->ents, (size_t)f->n, sizeof(f->ents[0]), compare);
}

/* ---- layout ---- */

static struct rect view_rect(struct fm *f)
{
    int w = f->win->content.w, h = f->win->content.h;
    return mkrect(4, VIEW_Y, w - 8, h - VIEW_Y - STATUS_H - 4);
}

/* The icon area inside the view's border, left of the scroll bar. */
static struct rect icon_area(struct fm *f)
{
    struct rect v = view_rect(f);
    return mkrect(v.x + 2, v.y + 2, v.w - 4 - SB_W, v.h - 4);
}

static struct rect sb_rect(struct fm *f)
{
    struct rect a = icon_area(f);
    return mkrect(a.x + a.w, a.y, SB_W, a.h);
}

static int columns(struct fm *f)
{
    int c = icon_area(f).w / CELL_W;
    return c > 0 ? c : 1;
}

static int content_height(struct fm *f)
{
    int rows = (f->n + columns(f) - 1) / columns(f);
    return rows * CELL_H + 8;
}

static int max_scroll(struct fm *f)
{
    int m = content_height(f) - icon_area(f).h;
    return m > 0 ? m : 0;
}

static struct rect cell_rect(struct fm *f, int i)
{
    struct rect a = icon_area(f);
    int cols = columns(f);
    int pad = (a.w - cols * CELL_W) / 2;
    return mkrect(a.x + pad + (i % cols) * CELL_W, a.y + 4 + (i / cols) * CELL_H - f->scroll,
                  CELL_W, CELL_H);
}

static struct rect addr_rect(struct fm *f)
{
    return mkrect(60, ADDR_Y + 1, f->win->content.w - 66, ADDR_H - 1);
}

static struct rect tool_rect(int i)
{
    int x = 6;
    for (int k = 0; k <= i; k++) {
        int w = gfx_text_width(&font_sans, tools[k].label) + (tools[k].icon ? 36 : 20);
        if (k == i)
            return mkrect(x, 4, w, 23);
        x += w + 4;
    }
    return mkrect(0, 0, 0, 0);
}

/* Scroll bar parts: arrows at the ends, the thumb somewhere between. */
static void sb_parts(struct fm *f, struct rect *up, struct rect *down, struct rect *thumb)
{
    struct rect sb = sb_rect(f);
    *up = mkrect(sb.x, sb.y, SB_W, SB_W);
    *down = mkrect(sb.x, sb.y + sb.h - SB_W, SB_W, SB_W);
    int track = sb.h - 2 * SB_W, ms = max_scroll(f);
    if (ms == 0 || track < 8) {
        *thumb = mkrect(0, 0, 0, 0);
        return;
    }
    int len = track * icon_area(f).h / content_height(f);
    if (len < 8)
        len = 8;
    int pos = (track - len) * f->scroll / ms;
    *thumb = mkrect(sb.x, sb.y + SB_W + pos, SB_W, len);
}

/* ---- drawing ---- */

static void draw_arrow(struct surface *s, struct rect r, int down, int enabled, int pressed)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
    gfx_bevel(s, r, pressed ? BEVEL_PRESSED : BEVEL_RAISED);
    int cx = r.x + r.w / 2 - 1 + pressed, cy = r.y + r.h / 2 - 2 + pressed;
    for (int i = 0; i < 4; i++) {
        int y = down ? cy + i : cy + 3 - i;
        if (!enabled)
            gfx_hline(s, cx - 3 + i + 1, y + 1, 7 - 2 * i, C_WHITE);
        gfx_hline(s, cx - 3 + i, y, 7 - 2 * i, enabled ? C_BLACK : C_SHADOW);
    }
}

static void draw_scrollbar(struct fm *f, struct surface *s)
{
    struct rect sb = sb_rect(f), up, down, thumb;
    sb_parts(f, &up, &down, &thumb);
    gfx_fill(s, sb.x, sb.y, sb.w, sb.h, C_FACE);
    gfx_dither(s, sb.x, sb.y, sb.w, sb.h, C_WHITE);
    int enabled = max_scroll(f) > 0;
    draw_arrow(s, up, 0, enabled, f->hold == HOLD_ARROW_UP);
    draw_arrow(s, down, 1, enabled, f->hold == HOLD_ARROW_DOWN);
    if (!rect_empty(thumb)) {
        gfx_fill(s, thumb.x, thumb.y, thumb.w, thumb.h, C_FACE);
        gfx_bevel(s, thumb, BEVEL_RAISED);
    }
}

static int count_selected(struct fm *f)
{
    int c = 0;
    for (int i = 0; i < f->n; i++)
        c += f->ents[i].selected;
    return c;
}

static void clear_selection(struct fm *f)
{
    for (int i = 0; i < f->n; i++)
        f->ents[i].selected = 0;
    f->sel = -1;
}

static void select_only(struct fm *f, int i)
{
    clear_selection(f);
    if (i >= 0 && i < f->n) {
        f->ents[i].selected = 1;
        f->sel = i;
    }
}

static struct rect band_rect(struct fm *f)
{
    int x0 = f->band_x0, x1 = f->band_x1;
    int y0 = f->band_y0 - f->scroll, y1 = f->band_y1;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    return mkrect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

/* Selects the entries whose icon or label the rubber band touches. */
static void band_select(struct fm *f)
{
    struct rect b = band_rect(f);
    f->sel = -1;
    for (int i = 0; i < f->n; i++) {
        struct rect c = cell_rect(f, i);
        struct rect icon = mkrect(c.x + (CELL_W - 32) / 2, c.y + 2, 32, 32);
        struct rect label = mkrect(c.x + 2, c.y + 37, CELL_W - 4, font_sans.height + 1);
        f->ents[i].selected = rect_overlaps(b, icon) || rect_overlaps(b, label);
        if (f->ents[i].selected && f->sel < 0)
            f->sel = i;
    }
}

static void draw_entries(struct fm *f, struct surface *s)
{
    struct rect a = icon_area(f);
    gfx_fill(s, a.x, a.y, a.w, a.h, C_WHITE);
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, a);
    for (int i = 0; i < f->n; i++) {
        struct rect c = cell_rect(f, i);
        if (!rect_overlaps(c, a))
            continue;
        int sel = f->ents[i].selected || (f == drop_f && i == drop_i);
        int ix = c.x + (CELL_W - 32) / 2, iy = c.y + 2;
        if (sel)
            draw_icon_selected(s, ix, iy, f->ents[i].icon, 32);
        else
            draw_icon(s, ix, iy, f->ents[i].icon, 32);
        char label[64];
        gfx_ellipsize(&font_sans, f->ents[i].name, CELL_W - 6, label, sizeof(label));
        int tw = gfx_text_width(&font_sans, label);
        struct rect lr = mkrect(c.x + (CELL_W - tw) / 2 - 2, iy + 35, tw + 4, font_sans.height + 1);
        if (sel)
            gfx_fill(s, lr.x, lr.y, lr.w, lr.h, C_NAVY);
        gfx_text(s, &font_sans, lr.x + 2, lr.y, label, sel ? C_WHITE : C_BLACK);
        if (i == f->sel && wm_focused() == f->win)
            gfx_focus_rect(s, lr);
    }
    if (f->hold == HOLD_BAND)
        gfx_focus_rect(s, band_rect(f));
    s->clip = saved;
}

static void format_status(struct fm *f, char *buf, size_t size)
{
    int nsel = count_selected(f);
    if (nsel == 0) {
        snprintf(buf, size, "%d object%s", f->n, f->n == 1 ? "" : "s");
        return;
    }
    if (nsel > 1) {
        snprintf(buf, size, "%d objects selected", nsel);
        return;
    }
    struct entry *e = &f->ents[f->sel];
    char path[PATH_MAX];
    struct stat st;
    join(path, sizeof(path), f->path, e->name);
    if (e->is_dir || stat(path, &st) < 0)
        snprintf(buf, size, "1 object selected");
    else if (st.st_size < 1024)
        snprintf(buf, size, "%s: %u bytes", e->name, st.st_size);
    else
        snprintf(buf, size, "%s: %u.%uKB", e->name, st.st_size / 1024, st.st_size % 1024 * 10 / 1024);
}

/* Free space on the folder's drive: a usage bar and a caption at the right of the status bar. */
static void draw_disk_space(struct fm *f, struct surface *s, int w, int h)
{
    struct statfs st;
    if (statfs(f->path, &st) < 0 || !st.f_blocks)
        return;
    /* No 64-bit division here, so work in KB with 32-bit arithmetic. */
    unsigned int total, free_kb;
    if (st.f_bsize >= 1024) {
        total = st.f_blocks * (st.f_bsize / 1024);
        free_kb = st.f_bfree * (st.f_bsize / 1024);
    } else {
        total = st.f_blocks / (1024 / st.f_bsize);
        free_kb = st.f_bfree / (1024 / st.f_bsize);
    }
    if (!total)
        return;
    /* "/" is a ramfs in kernel memory, not the hard disk: say so. */
    const char *what = strcmp(st.f_fstype, "ramfs") ? "free" : "RAM free";
    char text[48];
    if (free_kb >= 1024 * 1024)
        snprintf(text, sizeof(text), "%u.%uGB %s", free_kb >> 20,
                 (free_kb & 0xFFFFF) * 10 >> 20, what);
    else if (free_kb >= 1024)
        snprintf(text, sizeof(text), "%u.%uMB %s", free_kb >> 10,
                 (free_kb & 0x3FF) * 10 >> 10, what);
    else
        snprintf(text, sizeof(text), "%uKB %s", free_kb, what);

    int tw = gfx_text_width(&font_sans, text);
    int bar_w = 60, pw = bar_w + tw + 20;
    struct rect pr = mkrect(w - 2 - pw, h - STATUS_H, pw, STATUS_H - 2);
    gfx_bevel(s, pr, BEVEL_THIN_SUNKEN);
    struct rect bar = mkrect(pr.x + 5, pr.y + 4, bar_w, pr.h - 8);
    gfx_fill(s, bar.x, bar.y, bar.w, bar.h, C_WHITE);
    unsigned int used_kb = total - free_kb;
    while (total > 0x3FFFFFF) {         /* keep used * width within 32 bits */
        total >>= 4;
        used_kb >>= 4;
    }
    int used = (int)(used_kb * (bar_w - 2) / (total ? total : 1));
    if (used > 0)
        gfx_fill(s, bar.x + 1, bar.y + 1, used, bar.h - 2, C_NAVY);
    gfx_text(s, &font_sans, bar.x + bar_w + 6, h - STATUS_H + 2, text, C_BLACK);
}

/* ---- the right-click menu ---- */

enum { CX_SEP, CX_OPEN, CX_CUT, CX_COPY, CX_PASTE, CX_DELETE, CX_RENAME, CX_NEWDIR, CX_REFRESH, CX_TERMINAL,
       CX_EXTRACT, CX_PAINT };

static const char *const ctx_labels[] = {
    [CX_OPEN] = "Open", [CX_CUT] = "Cut", [CX_COPY] = "Copy", [CX_PASTE] = "Paste", [CX_DELETE] = "Delete",
    [CX_RENAME] = "Rename", [CX_NEWDIR] = "New Folder", [CX_REFRESH] = "Refresh",
    [CX_TERMINAL] = "Open Terminal Here", [CX_EXTRACT] = "Extract All...", [CX_PAINT] = "Edit in Paint",
};

#define CTX_W 150
#define CTX_ITEM_H 20
#define CTX_SEP_H 8

static int ctx_height(const struct fm *f, int i)
{
    return f->ctx_codes[i] == CX_SEP ? CTX_SEP_H : CTX_ITEM_H;
}

static struct rect ctx_rect(const struct fm *f)
{
    int h = 4;
    for (int i = 0; i < f->ctx_n; i++)
        h += ctx_height(f, i);
    int x = f->ctx_x, y = f->ctx_y, w = f->win->content.w, ch = f->win->content.h;
    if (x + CTX_W > w)
        x = w - CTX_W;
    if (y + h > ch)
        y = ch - h;
    return mkrect(x < 0 ? 0 : x, y < 0 ? 0 : y, CTX_W, h);
}

static struct rect ctx_item_rect(const struct fm *f, int idx)
{
    struct rect m = ctx_rect(f);
    int y = m.y + 2;
    for (int i = 0; i < idx; i++)
        y += ctx_height(f, i);
    return mkrect(m.x + 2, y, m.w - 4, ctx_height(f, idx));
}

static int ctx_hit(const struct fm *f, int x, int y)
{
    for (int i = 0; i < f->ctx_n; i++)
        if (f->ctx_codes[i] != CX_SEP && rect_contains(ctx_item_rect(f, i), x, y))
            return i;
    return -1;
}

static int ctx_enabled(const struct fm *f, int code)
{
    if (code == CX_PASTE)
        return clip_n > 0;
    if (code == CX_RENAME)
        return count_selected((struct fm *)f) == 1;
    return 1;
}

static void draw_ctx(struct fm *f, struct surface *s)
{
    struct rect m = ctx_rect(f);
    gfx_fill(s, m.x, m.y, m.w, m.h, C_FACE);
    gfx_bevel(s, m, BEVEL_WINDOW);
    for (int i = 0; i < f->ctx_n; i++) {
        struct rect r = ctx_item_rect(f, i);
        int code = f->ctx_codes[i];
        if (code == CX_SEP) {
            draw_separator(s, r.x + 1, r.y + 3, r.w - 2);
            continue;
        }
        int en = ctx_enabled(f, code), hot = i == f->ctx_hover && en;
        if (hot)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_NAVY);
        int ty = r.y + (r.h - font_sans.height) / 2;
        if (!en)
            gfx_text(s, &font_sans, r.x + 13, ty + 1, ctx_labels[code], C_WHITE);
        gfx_text(s, &font_sans, r.x + 12, ty, ctx_labels[code], hot ? C_WHITE : en ? C_BLACK : C_SHADOW);
    }
}

static void paint(struct fm *f)
{
    struct surface *s = &f->win->content;
    int w = s->w, h = s->h;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, w, h, C_FACE);

    /* Toolbar */
    for (int i = 0; i < NTOOLS; i++) {
        struct rect r = tool_rect(i);
        int down = f->hold == HOLD_TOOL && f->hold_tool == i && f->hold_in;
        int nsel = count_selected(f);
        int disabled = ((i == TB_DELETE || i == TB_CUT || i == TB_COPY) && nsel == 0) ||
                       (i == TB_RENAME && nsel != 1) || (i == TB_PASTE && !clip_n);
        draw_button(s, r, "", down, 0, 0);
        int x = r.x + 8 + down, y = r.y + 4 + down;
        if (tools[i].icon) {
            draw_icon(s, x, y - 1, tools[i].icon, 16);
            x += 20;
        }
        if (disabled)
            gfx_text(s, &font_sans, x + 1, y + 1, tools[i].label, C_WHITE);
        gfx_text(s, &font_sans, x, y, tools[i].label, disabled ? C_SHADOW : C_BLACK);
    }
    draw_separator(s, 2, TOOLBAR_H, w - 4);

    /* Address field */
    gfx_text(s, &font_sans, 8, ADDR_Y + 5, "Address", C_BLACK);
    if (f->editing)
        draw_text_field(s, addr_rect(f), f->addr, f->addr_cur, wm_focused() == f->win);
    else
        draw_text_field(s, addr_rect(f), f->path, 0, 0);

    /* The folder view */
    gfx_bevel(s, view_rect(f), BEVEL_SUNKEN);
    draw_entries(f, s);
    draw_scrollbar(f, s);

    /* Status bar */
    char status[96];
    format_status(f, status, sizeof(status));
    gfx_bevel(s, mkrect(2, h - STATUS_H, w - 4, STATUS_H - 2), BEVEL_THIN_SUNKEN);
    gfx_text(s, &font_sans, 7, h - STATUS_H + 2, status, C_BLACK);
    draw_disk_space(f, s, w, h);
    if (f->ctx_open)
        draw_ctx(f, s);

    wm_damage_client(f->win, mkrect(0, 0, w, h));
}

/* ---- navigation and actions ---- */

static void set_scroll(struct fm *f, int y)
{
    int ms = max_scroll(f);
    f->scroll = y < 0 ? 0 : y > ms ? ms : y;
}

static void show_selection(struct fm *f)
{
    if (f->sel < 0)
        return;
    struct rect a = icon_area(f), c = cell_rect(f, f->sel);
    if (c.y < a.y)
        set_scroll(f, f->scroll - (a.y - c.y) - 4);
    else if (c.y + c.h > a.y + a.h)
        set_scroll(f, f->scroll + (c.y + c.h - a.y - a.h) + 4);
}

static void select_name(struct fm *f, const char *name)
{
    int hit = -1;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->ents[i].name, name))
            hit = i;
    select_only(f, hit);
    show_selection(f);
}

static void navigate(struct fm *f, const char *path)
{
    f->editing = 0;
    snprintf(f->path, sizeof(f->path), "%s", path);
    f->sel = -1;
    f->scroll = 0;
    load(f);
    set_title(f);
    paint(f);
}

static void go_up(struct fm *f)
{
    if (!strcmp(f->path, "/"))
        return;
    char parent[PATH_MAX], child[NAME_MAX + 1];
    snprintf(parent, sizeof(parent), "%s", f->path);
    char *slash = strrchr(parent, '/');
    snprintf(child, sizeof(child), "%s", slash + 1);
    if (slash == parent)
        slash[1] = 0;
    else
        *slash = 0;
    navigate(f, parent);
    select_name(f, child);              /* land on the folder we came from */
    paint(f);
}

static void reload(struct fm *f, const char *select)
{
    char keep[NAME_MAX + 1] = "";
    char (*kept)[NAME_MAX + 1] = NULL;
    int nkept = 0;
    if (select) {
        snprintf(keep, sizeof(keep), "%s", select);
    } else if (f->sel >= 0) {
        snprintf(keep, sizeof(keep), "%s", f->ents[f->sel].name);
        kept = malloc((size_t)f->n * sizeof(*kept) + 1);
        for (int i = 0; kept && i < f->n; i++)
            if (f->ents[i].selected)
                snprintf(kept[nkept++], sizeof(*kept), "%s", f->ents[i].name);
    }
    load(f);
    set_scroll(f, f->scroll);
    select_name(f, keep);
    for (int k = 0; k < nkept; k++)
        for (int i = 0; i < f->n; i++)
            if (!strcmp(f->ents[i].name, kept[k]))
                f->ents[i].selected = 1;
    free(kept);
    paint(f);
}

/*
 * Once a second: discs and floppies are mounted and unmounted by the kernel
 * as they are put in and taken out (the automounter), so when the list of
 * mounts changes, every folder window reads its folder again and the drive
 * icons follow.
 */
void files_tick(void)
{
    static char last[2048];
    static int known;
    char now[sizeof(last)];
    int n = getmounts(now, sizeof(now) - 1);
    if (n < 0)
        return;
    now[n] = 0;
    if (known && !strcmp(now, last))
        return;
    int changed = known;
    memcpy(last, now, (size_t)n + 1);
    known = 1;
    if (!changed)
        return;                         /* the first look only takes note */
    for (struct fm *f = fms; f; f = f->next)
        reload(f, NULL);
}

/* Open a file (not a folder) by its kind; dir is its folder, for programs. */
static void open_file(const char *dir, const char *name, const char *path, enum icon_id icon)
{
    if (icon == ICON_IMAGE) {
        if (!image_view_open(path)) {
            char msg[NAME_MAX + 64];
            snprintf(msg, sizeof(msg), "%s is not a picture fluxOS can show (PNG, JPEG, GIF, SVG or ICO).", name);
            message_box("Image Viewer", ICON_WARNING, msg);
        }
    } else if (icon == ICON_HTML) {
        char url[PATH_MAX + 8];
        snprintf(url, sizeof(url), "file://%s", path);
        browser_open(url);
    } else if (icon == ICON_SOUND && has_suffix(name, ".wav")) {
        player_open(path);
    } else if (icon == ICON_SOUND) {
        const char *argv[] = { "/bin/play", path, NULL };
        terminal_open(name, argv, 0, 0);
    } else if (icon == ICON_ARCHIVE) {
        archiver_open(path);
    } else if (icon == ICON_RUN) {
        /* DOS programs go through the emulator even without execute permission.
         * Programs start in their own folder, as on Windows: a DOS game's
         * setup program runs the game by a name relative to it. */
        const char *dos_argv[] = { "/bin/dos", path, NULL }, *argv[] = { path, NULL };
        terminal_open_in(dir, name, is_dos_program(path) ? dos_argv : argv, 1);
    } else if (is_text_name(name)) {
        editor_open(path);
    } else {
        const char *argv[] = { "/bin/more", path, NULL };
        terminal_open(name, argv, 0, 0);
    }
}

static void open_entry(struct fm *f, int i)
{
    struct entry *e = &f->ents[i];
    char path[PATH_MAX];
    join(path, sizeof(path), f->path, e->name);
    if (e->is_dir)
        navigate(f, path);
    else
        open_file(f->path, e->name, path, e->icon);
}

void files_open_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        files_open(path);
        return;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    const char *name = slash ? slash + 1 : path;
    if (slash == dir)
        dir[1] = 0;                     /* a file in / */
    else if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof(dir), ".");
    /* the same kinds classify() tells apart, from the name alone */
    struct fm tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.path, sizeof(tmp.path), "%s", dir);
    struct entry e;
    memset(&e, 0, sizeof(e));
    snprintf(e.name, sizeof(e.name), "%s", name);
    open_file(dir, name, path, classify(&tmp, &e, ""));
}

/* Prompts call back with the fm; it may have been closed meanwhile. */
static struct fm *alive(void *ctx)
{
    for (struct fm *f = fms; f; f = f->next)
        if (f == ctx)
            return f;
    return NULL;
}

static void report(const char *title, const char *what, const char *name)
{
    char msg[NAME_MAX + 96];
    snprintf(msg, sizeof(msg), "Cannot %s %s: %s.", what, name, strerror(errno));
    message_box(title, ICON_WARNING, msg);
}

static int remove_tree(const char *path);

static void do_delete(void *ctx)
{
    struct fm *f = alive(ctx);
    if (!f)
        return;
    for (int i = 0; i < f->n; i++) {
        if (!f->ents[i].selected)
            continue;
        char path[PATH_MAX];
        join(path, sizeof(path), f->path, f->ents[i].name);
        struct stat st;
        int r = stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
        if (r < 0) {
            report("Error Deleting File", "delete", f->ents[i].name);
            break;
        }
    }
    f->sel = -1;
    reload(f, NULL);
}

static void ask_delete(struct fm *f)
{
    int nsel = count_selected(f);
    if (nsel == 0)
        return;
    char msg[NAME_MAX + 64];
    if (nsel > 1) {
        snprintf(msg, sizeof(msg), "Are you sure you want to delete these %d items?", nsel);
        ask_yes_no("Confirm Multiple File Delete", ICON_WARNING, msg, do_delete, f);
        return;
    }
    struct entry *e = &f->ents[f->sel];
    snprintf(msg, sizeof(msg), "Are you sure you want to delete '%s'?", e->name);
    ask_yes_no(e->is_dir ? "Confirm Folder Delete" : "Confirm File Delete", ICON_WARNING, msg,
               do_delete, f);
}

static void do_mkdir(void *ctx, const char *name)
{
    struct fm *f = alive(ctx);
    if (!f)
        return;
    char path[PATH_MAX];
    join(path, sizeof(path), f->path, name);
    if (mkdir(path, 0755) < 0) {
        report("Error Creating Folder", "create", name);
        return;
    }
    reload(f, name);
}

static void do_rename(void *ctx, const char *name)
{
    struct fm *f = alive(ctx);
    if (!f || !strcmp(name, f->pending))
        return;
    char from[PATH_MAX], to[PATH_MAX];
    join(from, sizeof(from), f->path, f->pending);
    join(to, sizeof(to), f->path, name);
    if (strchr(name, '/') || rename(from, to) < 0) {
        if (strchr(name, '/'))
            errno = EINVAL;
        report("Error Renaming File", "rename", f->pending);
        return;
    }
    reload(f, name);
}

static void ask_rename(struct fm *f)
{
    if (f->sel < 0 || count_selected(f) != 1)
        return;
    snprintf(f->pending, sizeof(f->pending), "%s", f->ents[f->sel].name);
    char label[NAME_MAX + 32];
    snprintf(label, sizeof(label), "New name for '%s':", f->pending);
    ask_text("Rename", label, f->pending, do_rename, f);
}

/* ---- cut, copy and paste ---- */

static int copy_file(const char *src, const char *dst)
{
    static char buf[4096];
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        int e = errno;
        close(in);
        errno = e;
        return -1;
    }
    int n, r = 0;
    while ((n = (int)read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            r = -1;
            break;
        }
    }
    if (n < 0)
        r = -1;
    int e = errno;
    close(in);
    close(out);
    errno = e;
    return r;
}

/* Copies a file or a whole folder tree; this works between filesystems. */
static int copy_tree(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) < 0)
        return -1;
    if (!S_ISDIR(st.st_mode))
        return copy_file(src, dst);
    if (mkdir(dst, 0755) < 0 && errno != EEXIST)
        return -1;
    DIR *d = opendir(src);
    if (!d)
        return -1;
    int r = 0;
    struct dirent *de;
    while (r == 0 && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char s[PATH_MAX], t[PATH_MAX];
        join(s, sizeof(s), src, de->d_name);
        join(t, sizeof(t), dst, de->d_name);
        r = copy_tree(s, t);
    }
    closedir(d);
    return r;
}

static int remove_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int r = 0;
    struct dirent *de;
    while (r == 0 && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char c[PATH_MAX];
        join(c, sizeof(c), path, de->d_name);
        r = remove_tree(c);
    }
    closedir(d);
    return r ? r : rmdir(path);
}

static void clip_set(struct fm *f, int cut)
{
    int nsel = count_selected(f);
    if (nsel == 0)
        return;
    char (*list)[PATH_MAX] = malloc((size_t)nsel * sizeof(*list));
    if (!list)
        return;
    free(clip);
    clip = list;
    clip_n = 0;
    for (int i = 0; i < f->n; i++)
        if (f->ents[i].selected)
            join(clip[clip_n++], PATH_MAX, f->path, f->ents[i].name);
    clip_cut = cut;
    paint(f);
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Pastes one clipboard item into the folder; 0 on success. */
static int paste_one(const char *dir, const char *src, char *name, int cut)
{
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char dst[PATH_MAX];
    snprintf(name, NAME_MAX + 1, "%s", base);
    join(dst, sizeof(dst), dir, name);

    /* A folder cannot go inside itself. */
    size_t n = strlen(src);
    if (!strncmp(dst, src, n) && dst[n] == '/') {
        struct stat st;
        if (stat(src, &st) == 0 && S_ISDIR(st.st_mode)) {
            message_box("File Manager", ICON_WARNING, "Cannot copy a folder into itself.");
            return 1;
        }
    }
    if (cut && !strcmp(dst, src))
        return 0;                       /* moved onto itself */
    for (int k = 1; exists(dst) && k < 100; k++) {
        if (k == 1)
            snprintf(name, NAME_MAX + 1, "Copy of %s", base);
        else
            snprintf(name, NAME_MAX + 1, "Copy (%d) of %s", k, base);
        join(dst, sizeof(dst), dir, name);
    }

    int r;
    if (cut && rename(src, dst) == 0) {
        r = 0;
    } else {
        /* Different filesystem (say, CD to hard disk): copy, then remove the original. */
        r = copy_tree(src, dst);
        if (r < 0) {
            int e = errno;
            remove_tree(dst);
            errno = e;
        } else if (cut) {
            r = remove_tree(src);
        }
    }
    if (r < 0) {
        report(cut ? "Error Moving File" : "Error Copying File", cut ? "move" : "copy", base);
        return 1;
    }
    return 0;
}

static void do_paste(struct fm *f)
{
    char name[NAME_MAX + 1] = "";
    int failed = 0;
    for (int i = 0; i < clip_n && !failed; i++)
        failed = paste_one(f->path, clip[i], name, clip_cut);
    if (clip_cut && !failed) {
        free(clip);
        clip = NULL;
        clip_n = 0;
    }
    reload(f, name[0] ? name : NULL);
}

/* ---- the address field ---- */

static void start_editing(struct fm *f)
{
    f->editing = 1;
    snprintf(f->addr, sizeof(f->addr), "%s", f->path);
    f->addr_len = f->addr_cur = (int)strlen(f->addr);
}

static void go_to_address(struct fm *f)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", f->addr);
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        path[--n] = 0;
    struct stat st;
    f->editing = 0;
    if (path[0] != '/' || stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "Cannot find the folder '%s'.", path);
        paint(f);
        message_box("File Manager", ICON_WARNING, msg);
        return;
    }
    navigate(f, path);
}

static void address_key(struct fm *f, const struct input_event *ev)
{
    char *a = f->addr;
    int c = ev->len == 1 ? (unsigned char)ev->text[0] : 0;
    if (ev->code == KEY_ENTER) {
        go_to_address(f);
        return;
    } else if (ev->code == KEY_ESC) {
        f->editing = 0;
    } else if (ev->code == KEY_LEFT && f->addr_cur > 0) {
        f->addr_cur--;
    } else if (ev->code == KEY_RIGHT && f->addr_cur < f->addr_len) {
        f->addr_cur++;
    } else if (ev->code == KEY_HOME) {
        f->addr_cur = 0;
    } else if (ev->code == KEY_END) {
        f->addr_cur = f->addr_len;
    } else if (ev->code == KEY_DELETE && f->addr_cur < f->addr_len) {
        memmove(a + f->addr_cur, a + f->addr_cur + 1, (size_t)(f->addr_len-- - f->addr_cur));
    } else if (c == 0x7F && f->addr_cur > 0) {
        memmove(a + f->addr_cur - 1, a + f->addr_cur, (size_t)(f->addr_len-- - f->addr_cur + 1));
        f->addr_cur--;
    } else if (c == 0x15) {                             /* ^U */
        f->addr_len = f->addr_cur = 0;
        a[0] = 0;
    } else if (c >= 32 && c < 127 && f->addr_len < (int)sizeof(f->addr) - 1) {
        memmove(a + f->addr_cur + 1, a + f->addr_cur, (size_t)(f->addr_len++ - f->addr_cur + 1));
        a[f->addr_cur++] = (char)c;
    } else {
        return;
    }
    paint(f);
}

static void tool_action(struct fm *f, int i)
{
    switch (i) {
    case TB_UP: go_up(f); break;
    case TB_NEWDIR: ask_text("New Folder", "Name of the new folder:", "New Folder", do_mkdir, f); break;
    case TB_CUT: clip_set(f, 1); break;
    case TB_COPY: clip_set(f, 0); break;
    case TB_PASTE: do_paste(f); break;
    case TB_DELETE: ask_delete(f); break;
    case TB_RENAME: ask_rename(f); break;
    }
}

static void open_ctx(struct fm *f, int x, int y, int hit)
{
    int n = 0;
    if (hit >= 0) {
        if (!f->ents[hit].selected)
            select_only(f, hit);
        else
            f->sel = hit;
        int c[] = { CX_OPEN, CX_SEP, CX_CUT, CX_COPY, CX_PASTE, CX_SEP, CX_DELETE, CX_RENAME };
        for (int i = 0; i < (int)(sizeof(c) / sizeof(c[0])); i++) {
            f->ctx_codes[n++] = c[i];
            if (c[i] == CX_OPEN && f->ents[hit].icon == ICON_ARCHIVE)
                f->ctx_codes[n++] = CX_EXTRACT;
            if (c[i] == CX_OPEN && f->ents[hit].icon == ICON_IMAGE)
                f->ctx_codes[n++] = CX_PAINT;
        }
    } else {
        clear_selection(f);
        int c[] = { CX_NEWDIR, CX_SEP, CX_PASTE, CX_SEP, CX_REFRESH, CX_TERMINAL };
        for (int i = 0; i < (int)(sizeof(c) / sizeof(c[0])); i++)
            f->ctx_codes[n++] = c[i];
    }
    f->ctx_n = n;
    f->ctx_x = x;
    f->ctx_y = y;
    f->ctx_hover = -1;
    f->ctx_open = 1;
}

static void ctx_action(struct fm *f, int code)
{
    switch (code) {
    case CX_OPEN:
        if (f->sel >= 0)
            open_entry(f, f->sel);
        break;
    case CX_CUT: clip_set(f, 1); break;
    case CX_COPY: clip_set(f, 0); break;
    case CX_PASTE: do_paste(f); break;
    case CX_DELETE: ask_delete(f); break;
    case CX_RENAME: ask_rename(f); break;
    case CX_NEWDIR: ask_text("New Folder", "Name of the new folder:", "New Folder", do_mkdir, f); break;
    case CX_REFRESH: reload(f, NULL); break;
    case CX_TERMINAL: terminal_open_in(f->path, "Terminal", NULL, 0); break;
    case CX_PAINT:
        if (f->sel >= 0) {
            char path[PATH_MAX];
            join(path, sizeof(path), f->path, f->ents[f->sel].name);
            paint_open(path);
        }
        break;
    case CX_EXTRACT:
        if (f->sel >= 0) {
            char path[PATH_MAX];
            join(path, sizeof(path), f->path, f->ents[f->sel].name);
            archiver_extract(path);
        }
        break;
    }
}

/* ---- input ---- */

static void fm_key(struct window *w, const struct input_event *ev)
{
    struct fm *f = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (f->ctx_open) {
        f->ctx_open = 0;
        paint(f);
        if (ev->code == KEY_ESC)
            return;
    }
    if (f->editing) {
        address_key(f, ev);
        return;
    }
    if (ev->len == 1 && ev->text[0] == 0x0C) {         /* Ctrl+L */
        start_editing(f);
        paint(f);
        return;
    }
    if (ev->len == 1 && (ev->text[0] == 0x18 || ev->text[0] == 0x03 || ev->text[0] == 0x16)) {
        if (ev->text[0] == 0x16)
            do_paste(f);                /* Ctrl+V */
        else
            clip_set(f, ev->text[0] == 0x18);   /* Ctrl+X, Ctrl+C */
        return;
    }
    if (ev->len == 1 && ev->text[0] == 0x01) {         /* Ctrl+A */
        for (int i = 0; i < f->n; i++)
            f->ents[i].selected = 1;
        f->sel = f->n ? 0 : -1;
        paint(f);
        return;
    }
    int cols = columns(f), sel = f->sel;
    switch (ev->code) {
    case KEY_LEFT: sel = sel < 0 ? 0 : sel - 1; break;
    case KEY_RIGHT: sel = sel < 0 ? 0 : sel + 1; break;
    case KEY_UP: sel = sel < 0 ? 0 : sel - cols; break;
    case KEY_DOWN: sel = sel < 0 ? 0 : sel + cols; break;
    case KEY_HOME: sel = 0; break;
    case KEY_END: sel = f->n - 1; break;
    case KEY_ENTER:
        if (f->sel >= 0)
            open_entry(f, f->sel);
        return;
    case KEY_DELETE: ask_delete(f); return;
    case KEY_F2: ask_rename(f); return;
    case KEY_F5: reload(f, NULL); return;
    default:
        if (ev->len == 1 && ev->text[0] == 0x7F)
            go_up(f);
        return;
    }
    if (f->n == 0 || sel < 0 || sel >= f->n)
        return;
    select_only(f, sel);
    show_selection(f);
    paint(f);
}

static void scroll_to_mouse(struct fm *f, int y)
{
    struct rect sb = sb_rect(f), up, down, thumb;
    sb_parts(f, &up, &down, &thumb);
    int span = sb.h - 2 * SB_W - thumb.h;
    if (span > 0)
        set_scroll(f, (y - f->thumb_grab - (sb.y + SB_W)) * max_scroll(f) / span);
}

/* The folder window under a screen point, and which of its entries (or -1). */
static struct fm *fm_at(int sx, int sy, int *entry)
{
    for (struct fm *o = fms; o; o = o->next) {
        struct rect cl = wm_client_rect(o->win);
        int x = sx - cl.x, y = sy - cl.y;
        if (o->win->minimized || !rect_contains(mkrect(0, 0, cl.w, cl.h), x, y) ||
            !rect_contains(icon_area(o), x, y))
            continue;
        *entry = -1;
        for (int i = 0; i < o->n; i++)
            if (o->ents[i].is_dir && rect_contains(cell_rect(o, i), x, y))
                *entry = i;
        return o;
    }
    return NULL;
}

/* Finds what a drag from f is over; entries being dragged are not drop targets. */
static void drop_target(struct fm *f, int x, int y, struct fm **tf, int *ti)
{
    struct rect cl = wm_client_rect(f->win);
    *tf = fm_at(cl.x + x, cl.y + y, ti);
    if (*tf == f && *ti >= 0 && f->ents[*ti].selected)
        *ti = -1;
}

static void set_drop(struct fm *tf, int ti)
{
    struct fm *old = drop_f;
    if (tf == drop_f && ti == drop_i)
        return;
    drop_f = tf;
    drop_i = ti;
    if (old)
        paint(old);
    if (tf && tf != old)
        paint(tf);
}

/* Moves the dragged selection into the folder it was dropped on. */
static void drop_items(struct fm *f, struct fm *tf, int ti)
{
    char dir[PATH_MAX];
    if (ti >= 0)
        join(dir, sizeof(dir), tf->path, tf->ents[ti].name);
    else
        snprintf(dir, sizeof(dir), "%s", tf->path);
    if (!strcmp(dir, f->path))
        return;                         /* dropped where it came from */
    char name[NAME_MAX + 1] = "";
    for (int i = 0; i < f->n; i++) {
        if (!f->ents[i].selected)
            continue;
        char src[PATH_MAX];
        join(src, sizeof(src), f->path, f->ents[i].name);
        if (paste_one(dir, src, name, 1))
            break;
    }
    reload(f, NULL);
    if (tf != f)
        reload(tf, NULL);
}

static void press(struct fm *f, int x, int y)
{
    if (rect_contains(addr_rect(f), x, y)) {
        if (!f->editing)
            start_editing(f);
        return;
    }
    f->editing = 0;
    for (int i = 0; i < NTOOLS; i++) {
        if (rect_contains(tool_rect(i), x, y)) {
            f->hold = HOLD_TOOL;
            f->hold_tool = i;
            f->hold_in = 1;
            return;
        }
    }
    struct rect sb = sb_rect(f), up, down, thumb;
    sb_parts(f, &up, &down, &thumb);
    if (rect_contains(sb, x, y)) {
        int page = icon_area(f).h - 16;
        if (rect_contains(up, x, y)) {
            f->hold = HOLD_ARROW_UP;
            set_scroll(f, f->scroll - LINE_STEP);
        } else if (rect_contains(down, x, y)) {
            f->hold = HOLD_ARROW_DOWN;
            set_scroll(f, f->scroll + LINE_STEP);
        } else if (rect_contains(thumb, x, y)) {
            f->hold = HOLD_THUMB;
            f->thumb_grab = y - thumb.y;
        } else if (!rect_empty(thumb)) {
            set_scroll(f, f->scroll + (y < thumb.y ? -page : page));
        }
        return;
    }
    if (!rect_contains(icon_area(f), x, y))
        return;
    int hit = -1;
    for (int i = 0; i < f->n; i++) {
        struct rect c = cell_rect(f, i);
        struct rect icon = mkrect(c.x + (CELL_W - 32) / 2, c.y + 2, 32, 32);
        struct rect label = mkrect(c.x + 2, c.y + 37, CELL_W - 4, font_sans.height + 1);
        if (rect_contains(icon, x, y) || rect_contains(label, x, y))
            hit = i;
    }
    if (hit < 0) {
        clear_selection(f);
        f->last_click = -1;
        f->hold = HOLD_BAND;
        f->band_x0 = f->band_x1 = x;
        f->band_y0 = y + f->scroll;
        f->band_y1 = y;
        return;
    }
    if (!f->ents[hit].selected)
        select_only(f, hit);
    else
        f->sel = hit;
    unsigned int t = now_ms();
    if (hit >= 0 && hit == f->last_click && t - f->last_click_ms < DBLCLICK_MS) {
        f->last_click = -1;
        open_entry(f, hit);
        return;
    }
    f->last_click = hit;
    f->last_click_ms = t;
    f->hold = HOLD_ITEM;
    f->item_hit = hit;
    f->item_x = x;
    f->item_y = y;
    f->dragging = 0;
}

static void fm_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct fm *f = w->priv;
    if (changed & MOUSE_RIGHT) {
        if (!(buttons & MOUSE_RIGHT))
            return;
        int hit = -1;
        if (rect_contains(icon_area(f), x, y))
            for (int i = 0; i < f->n; i++) {
                struct rect c = cell_rect(f, i);
                if (rect_contains(mkrect(c.x + (CELL_W - 32) / 2, c.y + 2, 32, 32), x, y) ||
                    rect_contains(mkrect(c.x + 2, c.y + 37, CELL_W - 4, font_sans.height + 1), x, y))
                    hit = i;
            }
        if (!rect_contains(icon_area(f), x, y))
            f->ctx_open = 0;
        else
            open_ctx(f, x, y, hit);
        f->editing = 0;
        paint(f);
        return;
    }
    if (f->ctx_open && !(changed & MOUSE_LEFT)) {
        int hv = ctx_hit(f, x, y);
        if (hv != f->ctx_hover) {
            f->ctx_hover = hv;
            paint(f);
        }
        return;
    }
    if ((changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT) && f->ctx_open) {
        int i = ctx_hit(f, x, y);
        int code = i >= 0 && ctx_enabled(f, f->ctx_codes[i]) ? f->ctx_codes[i] : CX_SEP;
        int inside = rect_contains(ctx_rect(f), x, y);
        f->ctx_open = 0;
        paint(f);
        if (code != CX_SEP)
            ctx_action(f, code);
        if (inside)
            return;                     /* a click on the menu is not a click on the folder */
    }
    if ((changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT)) {
        press(f, x, y);
    } else if (changed & MOUSE_LEFT) {                  /* release */
        enum hold h = f->hold;
        f->hold = HOLD_NONE;
        if (h == HOLD_ITEM) {
            struct fm *tf;
            int ti;
            if (f->dragging) {
                drop_target(f, x, y, &tf, &ti);
                set_drop(NULL, -1);
                f->dragging = 0;
                if (tf)
                    drop_items(f, tf, ti);
            } else if (count_selected(f) > 1) {
                select_only(f, f->item_hit);    /* a plain click on one of several */
            }
        }
        if (h == HOLD_TOOL && f->hold_in && rect_contains(tool_rect(f->hold_tool), x, y))
            tool_action(f, f->hold_tool);
    } else if (f->hold == HOLD_BAND) {
        f->band_x1 = x;
        f->band_y1 = y;
        band_select(f);
    } else if (f->hold == HOLD_ITEM) {
        if (!f->dragging && abs(x - f->item_x) < DRAG_SLOP && abs(y - f->item_y) < DRAG_SLOP)
            return;
        f->dragging = 1;
        struct fm *tf;
        int ti;
        drop_target(f, x, y, &tf, &ti);
        set_drop(tf, ti);
        return;
    } else if (f->hold == HOLD_THUMB) {
        scroll_to_mouse(f, y);
    } else if (f->hold == HOLD_TOOL) {
        int in = rect_contains(tool_rect(f->hold_tool), x, y);
        if (in == f->hold_in)
            return;
        f->hold_in = in;
    } else {
        return;
    }
    paint(f);
}

static void fm_focus(struct window *w, int focused)
{
    struct fm *f = w->priv;
    if (!f->win)
        return;                         /* still inside wm_create() */
    if (!focused)
        f->ctx_open = 0;
    if (focused)
        reload(f, NULL);                /* other windows may have changed files */
    else
        paint(f);
}

static void fm_resize(struct window *w)
{
    struct fm *f = w->priv;
    set_scroll(f, f->scroll);
    paint(f);
}

static void fm_destroy(struct window *w)
{
    struct fm *f = w->priv;
    for (struct fm **pp = &fms; *pp; pp = &(*pp)->next) {
        if (*pp == f) {
            *pp = f->next;
            break;
        }
    }
    if (drop_f == f)
        drop_f = NULL;
    free(f->ents);
    free(f);
}

static const struct window_ops fm_ops = {
    .key = fm_key,
    .mouse = fm_mouse,
    .focus = fm_focus,
    .resize = fm_resize,
    .destroy = fm_destroy,
};

struct window *files_open(const char *path)
{
    struct fm *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->sel = -1;
    f->last_click = -1;
    snprintf(f->path, sizeof(f->path), "%s", path);
    struct window *w = wm_create(path, ICON_FOLDER_OPEN, FM_W, FM_H, WF_MINIMIZE | WF_MAXIMIZE | WF_RIGHTCLICK,
                                 &fm_ops, f);
    if (!w) {
        free(f);
        return NULL;
    }
    f->win = w;
    f->next = fms;
    fms = f;
    navigate(f, path);
    return w;
}

/* ---- image viewer ---- */

static void iv_destroy(struct window *w)
{
    image_free(w->priv);
}

/* The picture on a shaded background, in the middle (clipped if too large). */
static void iv_paint(struct window *w)
{
    const struct image *img = w->priv;
    struct surface *s = &w->content;
    gfx_fill(s, 0, 0, s->w, s->h, C_SHADOW);
    gfx_image(s, (s->w - img->w) / 2, (s->h - img->h) / 2, img);
    wm_damage_client(w, mkrect(0, 0, s->w, s->h));
}

static const struct window_ops iv_ops = {
    .restyle = iv_paint,
    .destroy = iv_destroy,
};

struct window *image_view_open(const char *path)
{
    struct image *img = image_load(path);
    if (!img)
        return NULL;
    int cw = img->w + 16, ch = img->h + 16;
    int maxw = wm_work_right() - 2 * BORDER - 40, maxh = screen_h - TASKBAR_H - TITLE_H - 60;
    if (cw < 160)
        cw = 160;
    if (ch < 100)
        ch = 100;
    if (cw > maxw)
        cw = maxw;
    if (ch > maxh)
        ch = maxh;
    const char *name = strrchr(path, '/');
    struct window *w = wm_create(name ? name + 1 : path, ICON_IMAGE, cw, ch, WF_MINIMIZE,
                                 &iv_ops, img);
    if (!w) {
        image_free(img);
        return NULL;
    }
    iv_paint(w);
    return w;
}
