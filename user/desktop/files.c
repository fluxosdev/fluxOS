/*
 * The file manager: a folder window in the style of Windows 95's large
 * icon view, with a toolbar (Up, New Folder, Delete, Rename), an address
 * field, a scroll bar and a status bar. Double-clicking a folder opens
 * it in the same window; a program runs in a terminal window, a PNG
 * opens in the image viewer, a text file in the text editor, and anything
 * else is shown with more(1).
 *
 * Keys: arrows move the selection, Enter opens, Backspace goes up,
 * Delete deletes, F2 renames and F5 reloads the folder. Clicking the
 * address field or Ctrl+L lets you type a path to go to.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
    enum icon_id icon;
};

enum { TB_UP, TB_NEWDIR, TB_DELETE, TB_RENAME, NTOOLS };

static const struct {
    const char *label;
    enum icon_id icon;
} tools[NTOOLS] = {
    { "Up", ICON_UP },
    { "New Folder", ICON_FOLDER_NEW },
    { "Delete", ICON_DELETE },
    { "Rename", ICON_NONE },
};

/* What the mouse is holding down. */
enum hold { HOLD_NONE, HOLD_TOOL, HOLD_THUMB, HOLD_ARROW_UP, HOLD_ARROW_DOWN };

struct fm {
    struct window *win;
    char path[PATH_MAX];
    struct entry *ents;
    int n, cap;
    int sel;                            /* selected entry, or -1 */
    int scroll;                         /* view offset in pixels */
    enum hold hold;
    int hold_tool, hold_in;
    int thumb_grab;                     /* where the thumb was grabbed */
    int last_click;
    unsigned int last_click_ms;
    char pending[NAME_MAX + 1];         /* entry a delete/rename prompt is about */
    int editing;                        /* typing in the address field */
    char addr[PATH_MAX];
    int addr_len, addr_cur;
    struct fm *next;
};

static struct fm *fms;

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
    if (has_suffix(e->name, ".png"))
        return ICON_IMAGE;
    struct stat st;
    if (stat(path, &st) == 0 && (st.st_mode & 0111) && is_program(path))
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
        int sel = i == f->sel;
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
        if (sel && wm_focused() == f->win)
            gfx_focus_rect(s, lr);
    }
    s->clip = saved;
}

static void format_status(struct fm *f, char *buf, size_t size)
{
    if (f->sel < 0) {
        snprintf(buf, size, "%d object%s", f->n, f->n == 1 ? "" : "s");
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
        int disabled = (i == TB_DELETE || i == TB_RENAME) && f->sel < 0;
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
    f->sel = -1;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->ents[i].name, name))
            f->sel = i;
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
    if (select)
        snprintf(keep, sizeof(keep), "%s", select);
    else if (f->sel >= 0)
        snprintf(keep, sizeof(keep), "%s", f->ents[f->sel].name);
    load(f);
    set_scroll(f, f->scroll);
    select_name(f, keep);
    paint(f);
}

static void open_entry(struct fm *f, int i)
{
    struct entry *e = &f->ents[i];
    char path[PATH_MAX];
    join(path, sizeof(path), f->path, e->name);
    if (e->is_dir) {
        navigate(f, path);
    } else if (e->icon == ICON_IMAGE) {
        if (!image_view_open(path)) {
            char msg[NAME_MAX + 64];
            snprintf(msg, sizeof(msg), "%s is not a PNG image fluxOS can show.", e->name);
            message_box("Image Viewer", ICON_WARNING, msg);
        }
    } else if (e->icon == ICON_RUN) {
        const char *argv[] = { path, NULL };
        terminal_open(e->name, argv, 1, 0);
    } else if (is_text_name(e->name)) {
        editor_open(path);
    } else {
        const char *argv[] = { "/bin/more", path, NULL };
        terminal_open(e->name, argv, 0, 0);
    }
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

static void do_delete(void *ctx)
{
    struct fm *f = alive(ctx);
    if (!f)
        return;
    char path[PATH_MAX];
    struct stat st;
    join(path, sizeof(path), f->path, f->pending);
    int r = stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    if (r < 0)
        report("Error Deleting File", "delete", f->pending);
    reload(f, NULL);
}

static void ask_delete(struct fm *f)
{
    if (f->sel < 0)
        return;
    struct entry *e = &f->ents[f->sel];
    snprintf(f->pending, sizeof(f->pending), "%s", e->name);
    char msg[NAME_MAX + 64];
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
    if (f->sel < 0)
        return;
    snprintf(f->pending, sizeof(f->pending), "%s", f->ents[f->sel].name);
    char label[NAME_MAX + 32];
    snprintf(label, sizeof(label), "New name for '%s':", f->pending);
    ask_text("Rename", label, f->pending, do_rename, f);
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
    case TB_DELETE: ask_delete(f); break;
    case TB_RENAME: ask_rename(f); break;
    }
}

/* ---- input ---- */

static void fm_key(struct window *w, const struct input_event *ev)
{
    struct fm *f = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (f->editing) {
        address_key(f, ev);
        return;
    }
    if (ev->len == 1 && ev->text[0] == 0x0C) {         /* Ctrl+L */
        start_editing(f);
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
    f->sel = sel;
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
    f->sel = hit;
    unsigned int t = now_ms();
    if (hit >= 0 && hit == f->last_click && t - f->last_click_ms < DBLCLICK_MS) {
        f->last_click = -1;
        open_entry(f, hit);
        return;
    }
    f->last_click = hit;
    f->last_click_ms = t;
}

static void fm_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct fm *f = w->priv;
    if ((changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT)) {
        press(f, x, y);
    } else if (changed & MOUSE_LEFT) {                  /* release */
        enum hold h = f->hold;
        f->hold = HOLD_NONE;
        if (h == HOLD_TOOL && f->hold_in && rect_contains(tool_rect(f->hold_tool), x, y))
            tool_action(f, f->hold_tool);
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
    if (focused)
        reload(f, NULL);                /* other windows may have changed files */
    else
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
    free(f->ents);
    free(f);
}

static const struct window_ops fm_ops = {
    .key = fm_key,
    .mouse = fm_mouse,
    .focus = fm_focus,
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
    struct window *w = wm_create(path, ICON_FOLDER_OPEN, FM_W, FM_H, WF_MINIMIZE | WF_MAXIMIZE,
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

static const struct window_ops iv_ops = {
    .destroy = iv_destroy,
};

struct window *image_view_open(const char *path)
{
    struct image *img = image_load_png(path);
    if (!img)
        return NULL;
    int cw = img->w + 16, ch = img->h + 16;
    int maxw = screen_w - 2 * BORDER - 40, maxh = screen_h - TASKBAR_H - TITLE_H - 60;
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
    struct surface *s = &w->content;
    gfx_fill(s, 0, 0, cw, ch, C_SHADOW);
    gfx_image(s, (cw - img->w) / 2, (ch - img->h) / 2, img);    /* clipped if too large */
    wm_damage_client(w, mkrect(0, 0, cw, ch));
    return w;
}
