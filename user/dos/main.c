/*
 * dos - run a DOS program in the terminal.
 *
 *   dos [-l LOGFILE] PROGRAM [ARGUMENTS...]
 *
 * PROGRAM is a .COM or .EXE file; the extension may be left out, and
 * the name may be in any case. The kernel runs DOS programs through
 * this, so "./GAME.EXE" works too. The exit status is the program's
 * return code. Ctrl+\ stops the emulator at any time. With -l (or
 * DOSLOG=file in the environment), calls the emulator does not support
 * are logged to LOGFILE.
 */
#include <dirent.h>
#include <errno.h>
#include <flux.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dos.h"

#define BATCH      20000                /* instructions between looks at the clock */
#define REFRESH_MS 40                   /* screen redraws, at most */

static FILE *logfile;
static int idle_ms = -1;

uint32_t now_ms(void)
{
    return uptime_ms();
}

void request_idle(int ms)
{
    if (idle_ms < 0 || ms < idle_ms)
        idle_ms = ms;
    cpu.stop = 1;
}

void dlog(const char *fmt, ...)
{
    if (!logfile)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logfile, fmt, ap);
    va_end(ap);
    fflush(logfile);
}

static void restore_terminal(void)
{
    video_leave();
    kbd_restore();
}

void fatal(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    restore_terminal();
    dprintf(2, "dos: %s\n", msg);
    dlog("fatal: %s\n", msg);
    exit(1);
}

void mem_read(uint32_t addr, void *buf, uint32_t n)
{
    uint8_t *p = buf;
    for (uint32_t i = 0; i < n; i++)
        p[i] = rb(addr + i);
}

void mem_write(uint32_t addr, const void *buf, uint32_t n)
{
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < n; i++)
        wb(addr + i, p[i]);
}

int mem_strz(uint32_t addr, char *buf, int size)
{
    int i = 0;
    for (; i < size - 1; i++) {
        buf[i] = (char)rb(addr + (uint32_t)i);
        if (!buf[i])
            break;
    }
    buf[i] = 0;
    return i;
}

/* The program file: as given, or in any case, or with .COM or .EXE added. */
static int find_program(const char *arg, char *out, int size)
{
    struct stat st;
    if (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(out, (size_t)size, "%s", arg);
        return 0;
    }
    char dir[PATH_MAX];
    const char *slash = strrchr(arg, '/'), *base = slash ? slash + 1 : arg;
    if (slash)
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - arg) + (slash == arg), arg);
    else
        strcpy(dir, ".");
    static const char *const exts[] = { "", ".COM", ".EXE" };
    for (int e = 0; e < 3; e++) {
        char want[NAME_MAX + 1];
        snprintf(want, sizeof(want), "%s%s", base, exts[e]);
        DIR *dh = opendir(dir);
        if (!dh)
            return -1;
        struct dirent *de;
        while ((de = readdir(dh)))
            if (!strcasecmp(de->d_name, want)) {
                snprintf(out, (size_t)size, "%s/%s", dir, de->d_name);
                closedir(dh);
                if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
                    return 0;
                dh = NULL;
                break;
            }
        if (dh)
            closedir(dh);
    }
    return -1;
}

static void usage(void)
{
    fprintf(stderr, "usage: dos [-l LOGFILE] PROGRAM [ARGUMENTS...]\n");
    exit(2);
}

static void run(void)
{
    uint32_t last_refresh = now_ms();
    while (!dos_exited) {
        cpu_run(BATCH);
        if (dos_exited)
            break;
        timer_poll();
        kbd_poll(0);
        if (cpu.halted || idle_ms >= 0) {
            /* Waiting for a key or an interrupt: sleep until input comes
             * or the next timer tick is due. */
            int ms = idle_ms >= 0 ? idle_ms : 55;
            idle_ms = -1;
            video_refresh(0);
            last_refresh = now_ms();
            if (pic_pending() < 0 || !(cpu.flags & F_IF))
                kbd_poll(ms);
            timer_poll();
        }
        uint32_t t = now_ms();
        if (t - last_refresh >= REFRESH_MS) {
            video_refresh(0);
            last_refresh = t;
        }
    }
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-l") && i + 1 < argc) {
            logfile = fopen(argv[++i], "a");
            if (!logfile) {
                fprintf(stderr, "dos: %s: %s\n", argv[i], strerror(errno));
                return 2;
            }
        } else if (!strcmp(argv[i], "--")) {
            i++;
            break;
        } else {
            usage();
        }
    }
    if (i >= argc)
        usage();
    if (!logfile && getenv("DOSLOG"))
        logfile = fopen(getenv("DOSLOG"), "a");

    char prog[PATH_MAX];
    if (find_program(argv[i], prog, sizeof(prog)) < 0) {
        fprintf(stderr, "dos: %s: no such program\n", argv[i]);
        return 127;
    }
    size_t n = strlen(prog);
    if (n > 4 && !strcasecmp(prog + n - 4, ".bat")) {
        fprintf(stderr, "dos: %s: batch files are not supported\n", prog);
        return 1;
    }

    cpu_reset();
    bios_init();
    video_init();
    files_init();
    dos_init(environ);
    int err = dos_start(prog, argc - i - 1, argv + i + 1);
    if (err) {
        fprintf(stderr, "dos: %s: %s\n", prog,
                err == 8 ? "not enough memory" : err == 11 ? "not a valid DOS program" :
                "cannot load the program");
        return 1;
    }
    dlog("running %s\n", prog);
    kbd_init();
    run();
    restore_terminal();
    return dos_exit_code;
}
