/*
 * fluxOS Setup: the installer the CD starts, as Windows Setup is, on a
 * screen of its own (the desktop's setup mode). A backdrop lists the
 * steps and shows which one is under way; a wizard in front of it finds
 * the install files and the disks, lets the user choose a disk, name the
 * computer and confirm that the disk will be erased, then runs
 * fluxinstall(1) to do the work (partition table, GRUB, FAT32, copying)
 * and follows its progress through a pipe. When it is done the computer
 * restarts into the installed system.
 *
 * Pages: Welcome, Warning (fluxOS is experimental; "I understand" must
 * be ticked), Choose a Disk, Name This Computer, Ready to Install,
 * Installing, and Complete (or Failed); Quit Setup, from any of them
 * before the copying starts, restarts or shuts the computer down. The
 * name goes to fluxinstall -n, which writes it to the installed system's
 * /etc/hostname. Disk Manager opens from Choose a Disk (and after a
 * failure), to see what is on the disks, unmount or explore their volumes
 * and copy files off them first; Shift+F10 opens a command prompt (wm.c).
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
#include <flux/logo.h>
#include "desktop.h"

#define SW          500
#define SH          340
#define PANEL_W     148
#define TEXT_X      (PANEL_W + 22)
#define TEXT_W      (SW - TEXT_X - 18)
#define BUTTONS_Y   (SH - 12 - 23)
#define MAX_DISKS   8
#define LOG_LINES   5
#define INSTALLER   "/bin/fluxinstall"
#define MIN_SECTORS (2048 + 67584)      /* fluxinstall's minimum: 34 MiB */
#define HOST_MAX    63                  /* a host name's longest */
#define RESTART_SECONDS 15

/* The backdrop: the logo and title at the top left, the steps under them. */
#define BD_MARGIN   40
#define BD_LOGO     (FLUXOS_LOGO_SIZE / 2)
#define BD_STEPS_Y  (BD_MARGIN + BD_LOGO + 48)
#define BD_STEP_H   34
#define BD_COLUMN_W 260

enum page { P_WELCOME, P_WARNING, P_DISK, P_NAME, P_CONFIRM, P_INSTALL, P_DONE, P_QUIT };
enum { B_BACK, B_NEXT, B_CANCEL, B_DISKS, NBUTTONS };

static const char *const steps[] = {
    "Welcome", "Warning", "Choose a disk", "Name this computer", "Ready to install", "Installing fluxOS",
    "Finishing",
};
#define NSTEPS ((int)(sizeof(steps) / sizeof(steps[0])))

struct disk {
    char name[16];
    char model[44];
    char size[24];
    const char *why_not;                /* NULL if the disk can be used */
};

struct setup {
    struct window *win;
    enum page page;
    enum page quit_from;                /* where Quit Setup's Back goes */
    char source[128];                   /* the install medium, "" if none */
    struct disk disks[MAX_DISKS];
    int ndisks;
    int choice;                         /* selected disk, or -1 */
    int understood;                     /* the warning's "I understand" box */
    int agreed;                         /* the "erase this disk" box */
    char host[HOST_MAX + 1];            /* the computer's name, as typed */
    int host_len, host_cur;
    int pressed, pressed_in;            /* button held with the mouse */

    /* the running installer */
    int pid, fd;
    int exited, eof, ok;
    int percent;                        /* 0..100, from its "progress" lines */
    unsigned int started;               /* now_ms() when it started */
    char status[96];
    char error[160];
    char log[LOG_LINES][96];
    int nlog;
    char line[256];
    int linelen;
    int countdown;                      /* seconds to the restart, once installed */
};

static struct setup *setup;

/* ---- the backdrop ---- */

static struct surface backdrop;
static int backdrop_step = -1;          /* the step it shows; -1: not drawn yet */

static int current_step(void)
{
    struct setup *s = setup;
    if (!s)
        return 0;
    enum page p = s->page == P_QUIT ? s->quit_from : s->page;
    if (p == P_DONE)
        return s->ok ? NSTEPS - 1 : P_INSTALL;
    return (int)p;
}

/* The logo at half its size, blended over what is there. */
static void draw_logo(struct surface *sf, int x0, int y0)
{
    for (int j = 0; j < BD_LOGO; j++) {
        for (int i = 0; i < BD_LOGO; i++) {
            uint32_t a = 0, r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t p = fluxos_logo[(2 * j + k / 2) * FLUXOS_LOGO_SIZE + 2 * i + k % 2];
                uint32_t pa = p >> 24;
                a += pa;
                r += (p >> 16 & 0xFF) * pa;
                g += (p >> 8 & 0xFF) * pa;
                b += (p & 0xFF) * pa;
            }
            int x = x0 + i, y = y0 + j;
            if (!a || x < 0 || y < 0 || x >= sf->w || y >= sf->h)
                continue;
            uint32_t *px = sf->px + y * sf->stride + x;
            *px = gfx_mix(*px, RGB(r / a, g / a, b / a), (int)(a / 4) * 256 / 255);
        }
    }
}

/* A tick, for the steps that are done. */
static void draw_tick(struct surface *sf, int x, int y, color_t c)
{
    gfx_line(sf, x, y + 6, x + 4, y + 10, 2, ARGB(255, c));
    gfx_line(sf, x + 4, y + 10, x + 12, y + 1, 2, ARGB(255, c));
}

static void draw_backdrop(int step)
{
    struct surface *sf = &backdrop;
    theme_kinetik_blue(sf, sf->w, sf->h);
    draw_logo(sf, BD_MARGIN, BD_MARGIN);
    const struct pfont *big = &font_archivo_large;
    gfx_text(sf, big, BD_MARGIN + BD_LOGO + 18, BD_MARGIN + (BD_LOGO - big->height) / 2, "fluxOS Setup",
             C_WHITE);

    color_t done = RGB(0xC4, 0xD4, 0xEC), later = RGB(0x8C, 0xA2, 0xC4);
    for (int i = 0; i < NSTEPS; i++) {
        int y = BD_STEPS_Y + i * BD_STEP_H;
        if (i < step) {
            draw_tick(sf, BD_MARGIN, y + 3, done);
            gfx_text(sf, &font_archivo, BD_MARGIN + 24, y, steps[i], done);
        } else if (i == step) {
            gfx_round_fill(sf, mkrect(BD_MARGIN - 10, y - 7, BD_COLUMN_W - BD_MARGIN + 10,
                                      font_archivo_bold.height + 14), 6, 6, ARGB(0x40, 0xFFFFFF));
            gfx_fill(sf, BD_MARGIN + 2, y + font_archivo_bold.height / 2 - 4, 8, 8, C_WHITE);
            gfx_text(sf, &font_archivo_bold, BD_MARGIN + 24, y, steps[i], C_WHITE);
        } else {
            gfx_text(sf, &font_archivo, BD_MARGIN + 24, y, steps[i], later);
        }
    }
    gfx_text(sf, &font_archivo, BD_MARGIN, sf->h - BD_MARGIN, "Shift+F10: command prompt", later);
}

void setup_draw_backdrop(struct surface *dst, struct rect clip)
{
    int step = current_step();
    if (!backdrop.px || backdrop.w != screen_w || backdrop.h != screen_h) {
        surface_free(&backdrop);
        if (surface_init(&backdrop, screen_w, screen_h) < 0) {
            backdrop.px = NULL;
            gfx_fill(dst, clip.x, clip.y, clip.w, clip.h, RGB(0x10, 0x30, 0x60));
            return;
        }
        backdrop_step = -1;
    }
    if (backdrop_step != step) {
        draw_backdrop(step);
        backdrop_step = step;
    }
    gfx_blit(dst, clip.x, clip.y, &backdrop, clip.x, clip.y, clip.w, clip.h);
}

/* The page changed: the steps on the backdrop may have too. */
static void steps_changed(void)
{
    if (backdrop_step != current_step())
        wm_damage(mkrect(0, BD_STEPS_Y - 10, BD_COLUMN_W, NSTEPS * BD_STEP_H + 10));
}

/* ---- disks and the install medium ---- */

static int has_install_files(const char *dir)
{
    char path[160];
    struct stat st;
    snprintf(path, sizeof(path), "%s/boot/system.tar", dir);
    if (stat(path, &st) < 0)
        return 0;
    snprintf(path, sizeof(path), "%s/firmware/grub/core.img", dir);
    return stat(path, &st) == 0;
}

/* Is the block device `dev` the disk `disk` or one of its partitions? */
static int on_disk(const char *dev, const char *disk)
{
    size_t n = strlen(disk);
    if (strncmp(dev, disk, n))
        return 0;
    if (n && disk[n - 1] >= '0' && disk[n - 1] <= '9' && dev[n] == 'p')
        n++;                            /* nvme0n1p1 */
    return dev[n] == 0 || (dev[n] >= '0' && dev[n] <= '9');
}

static void size_text(uint32_t sectors, char *out, size_t n)
{
    uint32_t mb = sectors / 2048;
    if (mb < 1024)
        snprintf(out, n, "%u MiB", mb);
    else
        snprintf(out, n, "%u.%u GiB", mb / 1024, mb % 1024 * 10 / 1024);
}

/* The CD, or a USB stick written from it, wherever it was mounted. */
static void find_source(struct setup *s)
{
    s->source[0] = 0;
    if (has_install_files("/mnt/cdrom")) {
        strcpy(s->source, "/mnt/cdrom");
        return;
    }
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

    char chosen[16] = "";
    if (s->choice >= 0 && s->choice < s->ndisks)
        snprintf(chosen, sizeof(chosen), "%s", s->disks[s->choice].name);
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
        if (!d->why_not && (s->choice < 0 || !strcmp(d->name, chosen)))
            s->choice = s->ndisks - 1;      /* the first usable, or the one chosen before */
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
    case B_DISKS: return mkrect(12, BUTTONS_Y, 110, 23);   /* at the left, apart */
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

static struct rect understood_rect(void)
{
    return mkrect(TEXT_X, BUTTONS_Y - 38, TEXT_W, 18);
}

static struct rect host_field(void)
{
    return mkrect(TEXT_X, 118, 200, 21);
}

static int button_shown(struct setup *s, int b)
{
    if (b == B_DISKS)
        return s->page == P_DISK || (s->page == P_DONE && !s->ok);
    switch (s->page) {
    case P_WELCOME:
        return b != B_BACK;
    case P_DONE:
        return s->ok ? b == B_NEXT : b != B_NEXT;
    default:
        return 1;
    }
}

static const char *button_label(struct setup *s, int b)
{
    if (b == B_DISKS)
        return "Disk Manager...";
    if (s->page == P_QUIT)
        return b == B_BACK ? "< Back" : b == B_NEXT ? "Restart" : "Shut Down";
    if (b == B_BACK)
        return "< Back";
    if (b == B_CANCEL)
        return "Quit";
    switch (s->page) {
    case P_CONFIRM: return "Install";
    case P_DONE: return "Restart Now";
    default: return "Next >";
    }
}

static int button_enabled(struct setup *s, int b)
{
    if (!button_shown(s, b))
        return 0;
    if (b == B_DISKS)
        return 1;
    switch (s->page) {
    case P_WELCOME:
        return b == B_CANCEL || (b == B_NEXT && s->source[0]);
    case P_WARNING:
        return b != B_NEXT || s->understood;
    case P_DISK:
        return b != B_NEXT || (s->choice >= 0 && !s->disks[s->choice].why_not);
    case P_NAME:
        return b != B_NEXT || !hostname_problem(s->host);
    case P_CONFIRM:
        return b != B_NEXT || s->agreed;
    case P_INSTALL:
        return 0;                       /* it cannot be stopped halfway */
    case P_DONE:
    case P_QUIT:
        return 1;
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

/* "About 3 minutes left", once there is enough to go on. */
static void time_left(struct setup *s, char *out, size_t n)
{
    unsigned int elapsed = now_ms() - s->started;
    out[0] = 0;
    if (s->percent < 5 || s->percent >= 100 || elapsed < 3000)
        return;
    unsigned int left = elapsed / 1000 * (unsigned)(100 - s->percent) / (unsigned)s->percent;
    if (left < 60)
        snprintf(out, n, "Less than a minute left");
    else
        snprintf(out, n, "About %u minute%s left", (left + 30) / 60, (left + 30) / 60 == 1 ? "" : "s");
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
                          "Setup installs fluxOS on a hard disk in this computer, so that it "
                          "starts from the disk without the CD.\n\n"
                          "Setup creates a new partition table on the disk you choose, "
                          "formats it as FAT32 and copies fluxOS and the GRUB boot loader "
                          "onto it.", C_BLACK);
        y += 14;
        if (s->source[0])
            gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W,
                          "To install fluxOS, click Next. To leave without installing it, click Quit.",
                          C_BLACK);
        else
            gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W,
                          "Setup is looking for the fluxOS installation files on the CD...", C_BLACK);
        break;

    case P_WARNING:
        gfx_text(sf, &font_banner, TEXT_X, 16, "Warning", RGB(0xC0, 0x00, 0x00));
        gfx_text_wrap(sf, &font_sans, TEXT_X, 16 + font_banner.height + 12, TEXT_W,
                      "fluxOS is an experimental project, created entirely by generative AI "
                      "models, with a very limited amount of human testers. It is highly "
                      "discouraged to use fluxOS in any form of production environment.\n\n"
                      "Technical problems and data loss may occur, and by installing fluxOS, "
                      "you are entirely responsible for whatever happens to your machine.\n\n"
                      "If you find a bug, file an issue on the GitHub page: "
                      "https://github.com/fluxosdev/fluxOS", C_BLACK);
        draw_checkbox(sf, understood_rect().x, understood_rect().y, "I understand", s->understood);
        break;

    case P_DISK:
        y = title(sf, "Choose a Disk");
        gfx_text(sf, &font_sans, TEXT_X, y, "Select the hard disk to install fluxOS on:", C_BLACK);
        if (s->ndisks == 0)
            gfx_text_wrap(sf, &font_sans, TEXT_X + 10, disk_row(0).y, TEXT_W - 10,
                          "No hard disks were found. Check that the disk is connected, then "
                          "restart Setup.", C_BLACK);
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
        if (s->ndisks <= 4)
            gfx_text_wrap(sf, &font_sans, TEXT_X, disk_row(s->ndisks ? s->ndisks : 2).y + 8, TEXT_W,
                          "To see what is on a disk before it is erased, or to copy files off "
                          "it, click Disk Manager.", C_SHADOW);
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
                 "Setup will create one FAT32 partition, /dev/%s%s1, install GRUB and make "
                 "the disk bootable, and name the computer %s. Any volumes on this disk are "
                 "unmounted first.\n\n"
                 "Click Install to begin. This takes a few minutes and cannot be interrupted.",
                 d->name, d->name[0] && isdigit((unsigned char)d->name[strlen(d->name) - 1]) ? "p" : "",
                 s->host);
        gfx_text_wrap(sf, &font_sans, TEXT_X, y + 48, TEXT_W, buf, C_BLACK);
        snprintf(buf, sizeof(buf), "Erase /dev/%s and install fluxOS", d->name);
        draw_checkbox(sf, agree_rect().x, agree_rect().y, buf, s->agreed);
        break;

    case P_INSTALL: {
        y = title(sf, "Installing fluxOS");
        gfx_text(sf, &font_sans, TEXT_X, y, s->status, C_BLACK);
        struct rect bar = mkrect(TEXT_X, y + 22, TEXT_W - 42, 18);
        draw_progress(sf, bar, s->percent, 100);
        snprintf(buf, sizeof(buf), "%d%%", s->percent);
        gfx_text(sf, &font_sans, bar.x + bar.w + 8, bar.y + 2, buf, C_BLACK);
        time_left(s, buf, sizeof(buf));
        gfx_text(sf, &font_sans, TEXT_X, bar.y + 26, buf, C_SHADOW);
        draw_log(s, sf, mkrect(TEXT_X, bar.y + 50, TEXT_W, LOG_LINES * 15 + 8));
        break;
    }

    case P_DONE:
        if (s->ok) {
            y = title(sf, "Setup Complete");
            snprintf(buf, sizeof(buf),
                     "fluxOS is installed on /dev/%s.\n\n"
                     "Remove the CD from the drive. The computer will restart from the hard "
                     "disk in %d second%s, or click Restart Now.", d->name, s->countdown,
                     s->countdown == 1 ? "" : "s");
            gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W, buf, C_BLACK);
        } else {
            y = title(sf, "Setup Failed");
            y = gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W,
                              s->error[0] ? s->error : "The installer stopped unexpectedly.",
                              C_BLACK) + 8;
            y = gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W,
                              "Click Back to try again, perhaps with another disk.", C_BLACK) + 8;
            draw_log(s, sf, mkrect(TEXT_X, y, TEXT_W, LOG_LINES * 15 + 8));
        }
        break;

    case P_QUIT:
        y = title(sf, "Quit Setup?");
        gfx_text_wrap(sf, &font_sans, TEXT_X, y, TEXT_W,
                      "fluxOS is not installed. To install it later, start the computer from "
                      "the fluxOS CD again.\n\n"
                      "To go on with Setup, click Back. To leave it, restart or shut down the "
                      "computer; nothing on its disks has been changed.", C_BLACK);
        break;
    }

    draw_separator(sf, 12, BUTTONS_Y - 14, sf->w - 24);
    for (int b = 0; b < NBUTTONS; b++) {
        if (!button_shown(s, b))
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
    steps_changed();
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
    if (!strncmp(p, "progress ", 9)) {          /* fluxinstall -p */
        int v = atoi(p + 9);
        s->percent = v < 0 ? 0 : v > 100 ? 100 : v;
        return;
    }
    add_log(s, p);
    if (p[0] == '[' && isdigit((unsigned char)p[1]) && p[2] == '/') {
        const char *text = strchr(p, ']');
        snprintf(s->status, sizeof(s->status), "%s...", text ? text + 2 : p);
    } else if (!strncmp(p, "fluxinstall: ", 13)) {
        snprintf(s->error, sizeof(s->error), "%s", p + 13);
        if (s->error[0] >= 'a' && s->error[0] <= 'z')
            s->error[0] = (char)(s->error[0] - 'a' + 'A');
    }
}

static void finish_if_done(struct setup *s)
{
    if (!s->exited || !s->eof)
        return;
    if (s->ok) {
        s->percent = 100;
        s->countdown = RESTART_SECONDS;
    }
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
        char *argv[] = { "fluxinstall", "-y", "-p", "-s", s->source, "-n", s->host, dev, NULL };
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
    s->percent = 0;
    s->started = now_ms();
    s->nlog = 0;
    s->linelen = 0;
    s->error[0] = 0;
    snprintf(s->status, sizeof(s->status), "Starting...");
    loop_watch(s->fd, output_ready, s);
    s->page = P_INSTALL;
}

/* ---- input ---- */

static void quit(struct setup *s)
{
    s->quit_from = s->page;
    s->page = P_QUIT;
}

static void press_button(struct setup *s, int b)
{
    if (!button_enabled(s, b))
        return;
    if (b == B_DISKS) {
        app_launch("diskmgr");        /* setup_focus looks at the disks again after */
        paint(s);
        return;
    }
    if (s->page == P_QUIT) {
        if (b == B_NEXT) {
            system_restart();
            return;
        }
        if (b == B_CANCEL) {
            system_shutdown();
            return;
        }
        s->page = s->quit_from;
        paint(s);
        return;
    }
    if (b == B_CANCEL) {
        quit(s);
    } else if (b == B_BACK) {
        if (s->page == P_DONE) {         /* after a failure: try another disk */
            find_disks(s);
            s->page = P_DISK;
        } else if (s->page > P_WELCOME) {
            s->page--;
        }
    } else {
        switch (s->page) {
        case P_WELCOME:
            s->page = P_WARNING;
            break;
        case P_WARNING:
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
            system_restart();
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
        if (s->page == P_QUIT)
            press_button(s, B_BACK);
        else if (button_shown(s, B_CANCEL))
            press_button(s, B_CANCEL);
    } else if (s->page == P_WARNING && ev->code == KEY_SPACE) {
        s->understood = !s->understood;
        paint(s);
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
        if (button_shown(s, b) && rect_contains(button_rect(b), x, y))
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
        } else if (s->page == P_WARNING && rect_contains(understood_rect(), x, y)) {
            s->understood = !s->understood;
        } else if (s->page == P_CONFIRM && rect_contains(agree_rect(), x, y)) {
            s->agreed = !s->agreed;
        }
        paint(s);
    } else if (changed & MOUSE_LEFT) {                          /* release */
        int b = s->pressed, in = s->pressed_in;
        s->pressed = -1;
        s->pressed_in = 0;
        if (b >= 0 && in && hit == b)
            press_button(s, b);
        else
            paint(s);
    } else if (s->pressed >= 0 && (hit == s->pressed) != s->pressed_in) {
        s->pressed_in = hit == s->pressed;
        paint(s);
    }
}

static void setup_focus(struct window *w, int focused)
{
    struct setup *s = w->priv;
    if (!s->win)
        return;
    if (focused && s->page == P_DISK)
        find_disks(s);                  /* Disk Manager may have changed them */
    paint(s);
}

/* The close button and Alt+F4: Setup only ends by restarting or shutting down. */
static void setup_close(struct window *w)
{
    struct setup *s = w->priv;
    if (s->page == P_INSTALL || s->page == P_QUIT)
        return;                         /* never abandon a half-written disk */
    if (s->page == P_DONE && s->ok)
        return;
    quit(s);
    paint(s);
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

void setup_tick(void)
{
    struct setup *s = setup;
    if (!s)
        return;
    if (s->page == P_WELCOME && !s->source[0]) {
        find_source(s);                 /* the CD may be mounted a moment after the desktop starts */
        if (s->source[0])
            paint(s);
    } else if (s->page == P_INSTALL) {
        paint(s);                       /* the time left */
    } else if (s->page == P_DONE && s->ok) {
        if (--s->countdown <= 0) {
            system_restart();
            return;
        }
        paint(s);
    }
}

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
    s->win = wm_create("fluxOS Setup", ICON_INSTALL, SW, SH, 0, &setup_ops, s);
    if (!s->win) {
        free(s);
        setup = NULL;
        return;
    }
    wm_center(s->win);
    /* Right of the steps on the backdrop, where there is room for both. */
    if (screen_w >= BD_COLUMN_W + s->win->r.w + 20)
        wm_move(s->win, BD_COLUMN_W + (screen_w - BD_COLUMN_W - s->win->r.w) / 2, s->win->r.y);
    paint(s);
}
