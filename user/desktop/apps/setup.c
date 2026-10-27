/*
 * fluxOS Setup: a Windows 95 style wizard that installs fluxOS on a hard
 * disk. It finds the install files and the disks, lets the user choose a
 * disk and confirm that it will be erased, then runs fluxinstall(1) to
 * do the work (partition table, GRUB, FAT32, copying) and follows its
 * progress through a pipe.
 *
 * Pages: Welcome, Choose a Disk, Name This Computer, Ready to Install,
 * Installing, and Complete (or Failed). The name goes to fluxinstall -n,
 * which writes it to the installed system's /etc/hostname.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include "desktop.h"

#define SW          500
#define SH          340
#define PANEL_W     148
#define TEXT_X      (PANEL_W + 22)
#define TEXT_W      (SW - TEXT_X - 18)
#define BUTTONS_Y   (SH - 12 - 23)
#define MAX_DISKS   8
#define LOG_LINES   7
#define INSTALLER   "/bin/fluxinstall"
#define MIN_SECTORS (2048 + 67584)      /* fluxinstall's minimum: 34 MiB */
#define STEPS       4                   /* fluxinstall prints [1/4] .. [4/4] */
#define HOST_MAX    63                  /* a host name's longest */

enum page { P_WELCOME, P_DISK, P_NAME, P_CONFIRM, P_INSTALL, P_DONE };
enum { B_BACK, B_NEXT, B_CANCEL, NBUTTONS };

struct disk {
    char name[16];
    char model[44];
    char size[24];
    const char *why_not;                /* NULL if the disk can be used */
};

struct setup {
    struct window *win;
    enum page page;
    char source[128];                   /* the install medium, "" if none */
    struct disk disks[MAX_DISKS];
    int ndisks;
    int choice;                         /* selected disk, or -1 */
    int agreed;                         /* the "erase this disk" box */
    char host[HOST_MAX + 1];            /* the computer's name, as typed */
    int host_len, host_cur;
    int pressed, pressed_in;            /* button held with the mouse */

    /* the running installer */
    int pid, fd;
    int exited, eof, ok;
    int step;                           /* 0..STEPS */
    char status[96];
    char error[160];
    char log[LOG_LINES][96];
    int nlog;
    char line[256];
    int linelen;
};

static struct setup *setup;

/* ---- disks and the install medium ---- */

static int has_install_files(const char *dir)
{
    char path[160];
    struct stat st;
    snprintf(path, sizeof(path), "%s/boot/install/core.img", dir);
    return stat(path, &st) == 0;
}

/* Is the block device `dev` the disk `disk` or one of its partitions? */
static int on_disk(const char *dev, const char *disk)
{
    size_t n = strlen(disk);
    return !strncmp(dev, disk, n) && (dev[n] == 0 || (dev[n] >= '0' && dev[n] <= '9'));
}

static void size_text(uint32_t sectors, char *out, size_t n)
{
    uint32_t mb = sectors / 2048;
    if (mb < 1024)
        snprintf(out, n, "%u MiB", mb);
    else
        snprintf(out, n, "%u.%u GiB", mb / 1024, mb % 1024 * 10 / 1024);
}

static void find_source(struct setup *s)
{
    s->source[0] = 0;
    if (has_install_files("/mnt/cdrom")) {
        strcpy(s->source, "/mnt/cdrom");
        return;
    }
    /* Booted from an installed disk: it carries the install files too. */
    struct blkinfo bi;
    for (int k = 0; blkinfo(k, &bi) == 0; k++) {
        if (bi.mountpoint[0] && has_install_files(bi.mountpoint)) {
            snprintf(s->source, sizeof(s->source), "%s", bi.mountpoint);
            return;
        }
    }
}

static void find_disks(struct setup *s)
{
    char source_dev[32] = "";
    struct statfs sfs;
    if (s->source[0] && statfs(s->source, &sfs) == 0 && !strncmp(sfs.f_source, "/dev/", 5))
        snprintf(source_dev, sizeof(source_dev), "%s", sfs.f_source + 5);

    s->ndisks = 0;
    s->choice = -1;
    struct blkinfo bi;
    for (int k = 0; blkinfo(k, &bi) == 0 && s->ndisks < MAX_DISKS; k++) {
        if (bi.type != BLK_DISK)
            continue;
        struct disk *d = &s->disks[s->ndisks++];
        snprintf(d->name, sizeof(d->name), "%s", bi.name);
        snprintf(d->model, sizeof(d->model), "%s", bi.model[0] ? bi.model : "Hard disk");
        size_text(bi.sectors, d->size, sizeof(d->size));
        d->why_not = NULL;
        if (bi.sector_size != 512)
            d->why_not = "unsupported sector size";
        else if (bi.sectors < MIN_SECTORS)
            d->why_not = "too small (needs 34 MiB)";
        else if (source_dev[0] && on_disk(source_dev, bi.name))
            d->why_not = "holds the install files";
        if (!d->why_not && s->choice < 0)
            s->choice = s->ndisks - 1;
    }
}

/* RFC 1123: letters, digits and hyphens, no hyphen at either end. NULL if fine, else what is wrong. */
static const char *hostname_problem(const char *h)
{
    size_t n = strlen(h);
    if (n == 0)
        return "Type a name for this computer.";
    if (h[0] == '-' || h[n - 1] == '-')
        return "The name cannot start or end with a hyphen.";
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)h[i]) && h[i] != '-')
            return "Use only letters, digits and hyphens (no spaces).";
    return NULL;
}

/* ---- layout ---- */

static struct rect button_rect(int b)
{
    int cancel_x = SW - 12 - 75;
    switch (b) {
    case B_CANCEL: return mkrect(cancel_x, BUTTONS_Y, 75, 23);
    case B_NEXT: return mkrect(cancel_x - 10 - 75, BUTTONS_Y, 75, 23);
    default: return mkrect(cancel_x - 10 - 150, BUTTONS_Y, 75, 23);
    }
}

static struct rect disk_row(int i)
{
    return mkrect(TEXT_X, 74 + i * 24, TEXT_W, 20);
}

static struct rect agree_rect(void)
{
    return mkrect(TEXT_X, 214, TEXT_W, 18);
}

static struct rect host_field(void)
{
    return mkrect(TEXT_X, 118, 200, 21);
}

static const char *button_label(struct setup *s, int b)
{
    if (b == B_BACK)
        return "< Back";
    if (b == B_CANCEL)
        return s->page == P_DONE ? "Close" : "Cancel";
    switch (s->page) {
    case P_CONFIRM: return "Install";
    case P_DONE: return s->ok ? "Restart" : "Finish";
    default: return "Next >";
    }
}

static int button_enabled(struct setup *s, int b)
{
    switch (s->page) {
    case P_WELCOME:
        return b == B_CANCEL || (b == B_NEXT && s->source[0]);
    case P_DISK:
        return b != B_NEXT || (s->choice >= 0 && !s->disks[s->choice].why_not);
    case P_NAME:
        return b != B_NEXT || !hostname_problem(s->host);
    case P_CONFIRM:
        return b != B_NEXT || s->agreed;
    case P_INSTALL:
        return 0;                       /* it cannot be stopped halfway */
    case P_DONE:
        return b != B_BACK || !s->ok;
    }
    return 0;
}

/* ---- drawing ---- */

static void draw_checkbox(struct surface *sf, int x, int y, const char *label, int checked)
{
    struct rect box = mkrect(x, y + 1, 13, 13);
    gfx_fill(sf, box.x, box.y, box.w, box.h, C_WHITE);
    gfx_bevel(sf, box, BEVEL_SUNKEN);
    if (checked)
        for (int i = 0; i < 7; i++) {   /* a tick, three pixels thick */
            int cy = i < 3 ? y + 6 + i : y + 10 - (i - 2);
            gfx_vline(sf, x + 3 + i, cy, 3, C_BLACK);
        }
    gfx_text(sf, &font_sans, x + 20, y, label, C_BLACK);
}

static void draw_progress(struct surface *sf, struct rect r, int done, int total)
{
    gfx_fill(sf, r.x, r.y, r.w, r.h, C_WHITE);
    gfx_bevel(sf, r, BEVEL_THIN_SUNKEN);
    int inner = r.w - 4, filled = total ? inner * done / total : 0;
    for (int x = 0; x + 8 <= filled; x += 10)   /* Win95's block segments */
        gfx_fill(sf, r.x + 2 + x, r.y + 2, 8, r.h - 4, C_NAVY);
}

static void draw_log(struct setup *s, struct surface *sf, struct rect r)
{
    gfx_fill(sf, r.x, r.y, r.w, r.h, C_WHITE);
    gfx_bevel(sf, r, BEVEL_SUNKEN);
    struct rect saved = sf->clip;
    sf->clip = rect_intersect(saved, mkrect(r.x + 2, r.y + 2, r.w - 4, r.h - 4));
    for (int i = 0; i < s->nlog; i++)
        gfx_text(sf, &font_sans, r.x + 6, r.y + 4 + i * 15, s->log[i], C_BLACK);
    sf->clip = saved;
}

static int title(struct surface *sf, const char *text)
{
    gfx_text(sf, &font_banner, TEXT_X, 16, text, C_BLACK);
    return 16 + font_banner.height + 12;
}

static void paint(struct setup *s)
{
    struct surface *sf = &s->win->content;
    gfx_noclip(sf);
    gfx_fill(sf, 0, 0, sf->w, sf->h, C_FACE);

    /* The picture panel on the left. */
    struct rect panel = mkrect(12, 12, PANEL_W - 12, BUTTONS_Y - 24);
    gfx_fill(sf, panel.x, panel.y, panel.w, panel.h, C_DESKTOP);
    gfx_bevel(sf, panel, BEVEL_SUNKEN);
    int cx = panel.x + panel.w / 2;
    draw_icon(sf, cx - 24, panel.y + 40, ICON_FLUX, 48);
    draw_icon(sf, cx - 24, panel.y + 120, ICON_INSTALL, 48);
    int tw = gfx_text_width(&font_sans_bold, "fluxOS Setup");
    gfx_text(sf, &font_sans_bold, cx - tw / 2, panel.y + panel.h - 30, "fluxOS Setup", C_WHITE);

    char buf[320];
    int y;
    struct disk *d = s->choice >= 0 ? &s->disks[s->choice] : NULL;
    switch (s->page) {
    case P_WELCOME:
        y = title(sf, "Welcome to fluxOS Setup");
        y = gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W,
                          "This wizard installs fluxOS on a hard disk in this computer, so "
                          "that it can start without the CD.\n\n"
                          "Setup creates a new partition table on the disk you choose, "
                          "formats it as FAT32 and copies fluxOS and the GRUB boot loader "
                          "onto it.", C_BLACK);
        y += 14;
        if (s->source[0])
            gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W, "Click Next to continue.", C_BLACK);
        else
            gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W,
                          "Setup cannot find the fluxOS installation files. Start the "
                          "computer from the fluxOS CD and try again.", C_BLACK);
        break;

    case P_DISK:
        y = title(sf, "Choose a Disk");
        gfx_text(sf, &font_sans, TEXT_X, y, "Select the hard disk to install fluxOS on:", C_BLACK);
        if (s->ndisks == 0)
            gfx_text(sf, &font_sans, TEXT_X + 10, disk_row(0).y, "No hard disks were found.", C_BLACK);
        for (int i = 0; i < s->ndisks; i++) {
            struct rect r = disk_row(i);
            struct disk *di = &s->disks[i];
            if (di->why_not)
                snprintf(buf, sizeof(buf), "%s - %s, %s (%s)", di->name, di->model, di->size, di->why_not);
            else
                snprintf(buf, sizeof(buf), "%s - %s, %s", di->name, di->model, di->size);
            draw_icon(sf, r.x + 18, r.y + 1, ICON_DRIVE, 16);
            if (di->why_not) {
                gfx_text(sf, &font_sans, r.x + 41, r.y + 4, buf, C_WHITE);
                gfx_text(sf, &font_sans, r.x + 40, r.y + 3, buf, C_SHADOW);
            } else {
                draw_radio(sf, r.x, r.y + 3, "", i == s->choice, 0);
                gfx_text(sf, &font_sans, r.x + 40, r.y + 3, buf, C_BLACK);
            }
        }
        snprintf(buf, sizeof(buf), "Installation files: %s", s->source);
        gfx_text(sf, &font_sans, TEXT_X, BUTTONS_Y - 42, buf, C_SHADOW);
        break;

    case P_NAME: {
        y = title(sf, "Name This Computer");
        y = gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W,
                          "Choose a name for this computer. It appears in the shell's prompt and "
                          "can be changed later in /etc/hostname.", C_BLACK);
        gfx_text(sf, &font_sans, TEXT_X, host_field().y - 18, "Computer name:", C_BLACK);
        draw_text_field(sf, host_field(), s->host, s->host_cur, 1);
        const char *why = hostname_problem(s->host);
        gfx_text_wrap(sf, &font_sans, TEXT_X, host_field().y + 30, TEXT_W,
                      why ? why : "Up to 63 letters, digits and hyphens.", why ? C_BLACK : C_SHADOW);
        break;
    }

    case P_CONFIRM:
        y = title(sf, "Ready to Install");
        draw_icon(sf, TEXT_X, y, ICON_WARNING, 32);
        snprintf(buf, sizeof(buf), "All data on /dev/%s (%s, %s) will be erased.", d->name, d->model, d->size);
        gfx_text_wrap(sf, &font_sans_bold, TEXT_X + 44, y + 1, TEXT_W - 44, buf, C_BLACK);
        snprintf(buf, sizeof(buf),
                 "Setup will create one FAT32 partition, /dev/%s1, install GRUB and make "
                 "the disk bootable, and name the computer %s. Any volumes on this disk are "
                 "unmounted first.\n\n"
                 "Click Install to begin. This takes a moment and cannot be interrupted.",
                 d->name, s->host);
        gfx_text_wrap(sf, &font_sans, TEXT_X, y + 48, TEXT_W, buf, C_BLACK);
        snprintf(buf, sizeof(buf), "Erase /dev/%s and install fluxOS", d->name);
        draw_checkbox(sf, agree_rect().x, agree_rect().y, buf, s->agreed);
        break;

    case P_INSTALL:
    case P_DONE:
        if (s->page == P_INSTALL) {
            y = title(sf, "Installing fluxOS");
            gfx_text(sf, &font_sans, TEXT_X, y, s->status, C_BLACK);
            draw_progress(sf, mkrect(TEXT_X, y + 22, TEXT_W, 18), s->step, STEPS);
            y += 50;
        } else if (s->ok) {
            y = title(sf, "Setup Complete");
            snprintf(buf, sizeof(buf),
                     "fluxOS is installed on /dev/%s.\n\nTo start it from the hard disk, "
                     "remove the CD (or choose the disk in your computer's boot menu) and "
                     "click Restart.", d->name);
            y = gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W, buf, C_BLACK) + 8;
        } else {
            y = title(sf, "Setup Failed");
            y = gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W,
                              s->error[0] ? s->error : "The installer stopped unexpectedly.",
                              C_BLACK) + 8;
        }
        draw_log(s, sf, mkrect(TEXT_X, y, TEXT_W, LOG_LINES * 15 + 8));
        break;
    }

    draw_separator(sf, 12, BUTTONS_Y - 14, sf->w - 24);
    for (int b = 0; b < NBUTTONS; b++) {
        if (b == B_BACK && s->page == P_DONE && s->ok)
            continue;
        struct rect r = button_rect(b);
        const char *label = button_label(s, b);
        int enabled = button_enabled(s, b);
        int pressed = s->pressed == b && s->pressed_in;
        draw_button(sf, r, enabled ? label : "", pressed, b == B_NEXT && enabled, 0);
        if (!enabled) {
            int lw = gfx_text_width(&font_sans, label);
            int lx = r.x + (r.w - lw) / 2, ly = r.y + (r.h - font_sans.height) / 2;
            gfx_text(sf, &font_sans, lx + 1, ly + 1, label, C_WHITE);
            gfx_text(sf, &font_sans, lx, ly, label, C_SHADOW);
        }
    }
    wm_damage_client(s->win, mkrect(0, 0, sf->w, sf->h));
}

/* ---- running the installer ---- */

static void add_log(struct setup *s, const char *text)
{
    if (s->nlog == LOG_LINES) {
        memmove(s->log[0], s->log[1], sizeof(s->log[0]) * (LOG_LINES - 1));
        s->nlog--;
    }
    snprintf(s->log[s->nlog++], sizeof(s->log[0]), "%s", text);
}

/* One line of fluxinstall's output, colour escapes already removed. */
static void handle_line(struct setup *s, const char *line)
{
    const char *p = line;
    while (*p == ' ')
        p++;
    if (!*p)
        return;
    add_log(s, p);
    if (p[0] == '[' && p[1] >= '1' && p[1] <= '0' + STEPS && p[2] == '/') {
        s->step = p[1] - '1';
        const char *text = strchr(p, ']');
        snprintf(s->status, sizeof(s->status), "%s...", text ? text + 2 : p);
    } else if (!strncmp(p, "fluxinstall: ", 13)) {
        snprintf(s->error, sizeof(s->error), "%s", p + 13);
        if (s->error[0] >= 'a' && s->error[0] <= 'z')
            s->error[0] = (char)(s->error[0] - 'a' + 'A');
    } else if (strstr(p, "fluxOS is installed")) {
        s->step = STEPS;
    }
}

static void finish_if_done(struct setup *s)
{
    if (!s->exited || !s->eof)
        return;
    if (s->ok)
        s->step = STEPS;
    s->page = P_DONE;
    paint(s);
}

static void output_ready(void *ctx, int revents)
{
    struct setup *s = ctx;
    char buf[512];
    int n = (revents & POLLIN) ? (int)read(s->fd, buf, sizeof(buf)) : 0;
    if (n <= 0) {
        loop_unwatch(s->fd);
        close(s->fd);
        s->fd = -1;
        s->eof = 1;
        finish_if_done(s);
        return;
    }
    int esc = 0;
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (esc) {                      /* skip ESC [ ... final-byte */
            if (c >= 0x40 && c <= 0x7E && c != '[')
                esc = 0;
            continue;
        }
        if (c == 0x1B) {
            esc = 1;
        } else if (c == '\n') {
            s->line[s->linelen] = 0;
            handle_line(s, s->line);
            s->linelen = 0;
        } else if (s->linelen < (int)sizeof(s->line) - 1) {
            s->line[s->linelen++] = c;
        }
    }
    paint(s);
}

void setup_child_exited(int pid, int status)
{
    struct setup *s = setup;
    if (!s || !s->pid || pid != s->pid)
        return;
    s->pid = 0;
    s->exited = 1;
    s->ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    finish_if_done(s);
}

static void start_install(struct setup *s)
{
    int fds[2];
    if (pipe(fds) < 0) {
        snprintf(s->error, sizeof(s->error), "Cannot start the installer: %s.", strerror(errno));
        s->ok = 0;
        s->page = P_DONE;
        return;
    }
    char dev[32];
    snprintf(dev, sizeof(dev), "/dev/%s", s->disks[s->choice].name);
    int pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDONLY);
        dup2(null, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        for (int fd = 3; fd < 32; fd++)
            close(fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        char *argv[] = { "fluxinstall", "-y", "-s", s->source, "-n", s->host, dev, NULL };
        execve(INSTALLER, argv, environ);
        fprintf(stderr, "fluxinstall: cannot run %s: %s\n", INSTALLER, strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        snprintf(s->error, sizeof(s->error), "Cannot start the installer: %s.", strerror(errno));
        s->ok = 0;
        s->page = P_DONE;
        return;
    }
    s->pid = pid;
    s->fd = fds[0];
    s->exited = s->eof = s->ok = 0;
    s->step = 0;
    s->nlog = 0;
    s->linelen = 0;
    s->error[0] = 0;
    snprintf(s->status, sizeof(s->status), "Starting...");
    loop_watch(s->fd, output_ready, s);
    s->page = P_INSTALL;
}

/* ---- input ---- */

static void press_button(struct setup *s, int b)
{
    if (!button_enabled(s, b))
        return;
    if (b == B_CANCEL) {
        wm_destroy(s->win);
        return;
    }
    if (b == B_BACK) {
        if (s->page == P_DONE) {         /* after a failure: try another disk */
            find_disks(s);
            s->page = P_DISK;
        } else if (s->page > P_WELCOME) {
            s->page--;
        }
    } else {
        switch (s->page) {
        case P_WELCOME:
            find_disks(s);
            s->page = P_DISK;
            break;
        case P_DISK:
            s->page = P_NAME;
            break;
        case P_NAME:
            s->agreed = 0;
            s->page = P_CONFIRM;
            break;
        case P_CONFIRM:
            start_install(s);
            break;
        case P_DONE:
            if (s->ok) {
                system_restart();
                return;
            }
            wm_destroy(s->win);
            return;
        default:
            break;
        }
    }
    paint(s);
}

/* Editing the computer's name; returns 1 if the key was the field's. */
static int host_key(struct setup *s, const struct input_event *ev)
{
    if (ev->code == KEY_LEFT && s->host_cur > 0)
        s->host_cur--;
    else if (ev->code == KEY_RIGHT && s->host_cur < s->host_len)
        s->host_cur++;
    else if (ev->code == KEY_HOME)
        s->host_cur = 0;
    else if (ev->code == KEY_END)
        s->host_cur = s->host_len;
    else if (ev->code == KEY_DELETE && s->host_cur < s->host_len) {
        memmove(s->host + s->host_cur, s->host + s->host_cur + 1, (size_t)(s->host_len - s->host_cur));
        s->host_len--;
    } else if (ev->len == 1 && ev->text[0] == 0x7F && s->host_cur > 0) {
        memmove(s->host + s->host_cur - 1, s->host + s->host_cur, (size_t)(s->host_len - s->host_cur + 1));
        s->host_cur--;
        s->host_len--;
    } else if (ev->len == 1 && ev->text[0] > 32 && ev->text[0] < 127 && s->host_len < HOST_MAX) {
        memmove(s->host + s->host_cur + 1, s->host + s->host_cur, (size_t)(s->host_len - s->host_cur + 1));
        s->host[s->host_cur++] = ev->text[0];
        s->host_len++;
    } else {
        return 0;
    }
    return 1;
}

static void setup_key(struct window *w, const struct input_event *ev)
{
    struct setup *s = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (s->page == P_NAME && host_key(s, ev)) {
        paint(s);
        return;
    }
    if (ev->code == KEY_ENTER) {
        press_button(s, B_NEXT);
    } else if (ev->code == KEY_ESC) {
        press_button(s, B_CANCEL);
    } else if (s->page == P_CONFIRM && ev->code == KEY_SPACE) {
        s->agreed = !s->agreed;
        paint(s);
    } else if (s->page == P_DISK && (ev->code == KEY_UP || ev->code == KEY_DOWN)) {
        int dir = ev->code == KEY_UP ? -1 : 1;
        for (int i = s->choice + dir; i >= 0 && i < s->ndisks; i += dir) {
            if (!s->disks[i].why_not) {
                s->choice = i;
                break;
            }
        }
        paint(s);
    }
}

static void setup_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct setup *s = w->priv;
    int hit = -1;
    for (int b = 0; b < NBUTTONS; b++)
        if (rect_contains(button_rect(b), x, y))
            hit = b;

    if ((changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT)) {     /* press */
        if (hit >= 0 && button_enabled(s, hit)) {
            s->pressed = hit;
            s->pressed_in = 1;
        } else if (s->page == P_DISK) {
            for (int i = 0; i < s->ndisks; i++)
                if (rect_contains(disk_row(i), x, y) && !s->disks[i].why_not)
                    s->choice = i;
        } else if (s->page == P_NAME && rect_contains(host_field(), x, y)) {
            /* the caret to the character clicked on */
            int cx = host_field().x + 4;
            s->host_cur = 0;
            while (s->host_cur < s->host_len) {
                char one[2] = { s->host[s->host_cur], 0 };
                int w = gfx_text_width(&font_sans, one);
                if (x < cx + w / 2)
                    break;
                cx += w;
                s->host_cur++;
            }
        } else if (s->page == P_CONFIRM && rect_contains(agree_rect(), x, y)) {
            s->agreed = !s->agreed;
        }
        paint(s);
    } else if (changed & MOUSE_LEFT) {                          /* release */
        int b = s->pressed, in = s->pressed_in;
        s->pressed = -1;
        s->pressed_in = 0;
        if (b >= 0 && in && hit == b)
            press_button(s, b);         /* may close the window */
        else
            paint(s);
    } else if (s->pressed >= 0 && (hit == s->pressed) != s->pressed_in) {
        s->pressed_in = hit == s->pressed;
        paint(s);
    }
}

static void setup_focus(struct window *w, int focused)
{
    (void)focused;
    if (((struct setup *)w->priv)->win)
        paint(w->priv);
}

static void setup_close(struct window *w)
{
    struct setup *s = w->priv;
    if (s->page != P_INSTALL)           /* never abandon a half-written disk */
        wm_destroy(w);
}

static void setup_destroy(struct window *w)
{
    free(w->priv);
    setup = NULL;
}

static const struct window_ops setup_ops = {
    .key = setup_key,
    .mouse = setup_mouse,
    .focus = setup_focus,
    .close = setup_close,
    .destroy = setup_destroy,
};

void setup_open(void)
{
    if (setup) {
        wm_focus(setup->win);
        return;
    }
    struct setup *s = calloc(1, sizeof(*s));
    if (!s)
        return;
    s->pressed = -1;
    s->fd = -1;
    const char *host = getenv("HOST");
    snprintf(s->host, sizeof(s->host), "%s", host && !hostname_problem(host) ? host : "fluxOS");
    s->host_len = s->host_cur = (int)strlen(s->host);
    find_source(s);
    setup = s;
    s->win = wm_create("fluxOS Setup", ICON_INSTALL, SW, SH, WF_MINIMIZE, &setup_ops, s);
    if (!s->win) {
        free(s);
        setup = NULL;
        return;
    }
    wm_center(s->win);
    paint(s);
}
