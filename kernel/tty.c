/*
 * The console tty: keyboard and serial input are collected here and
 * handed to readers either line by line (canonical mode, with echo and
 * erase/kill editing) or byte by byte (raw mode, used by the shell's
 * line editor). ^C and ^\ signal the foreground process group.
 */
#include "kernel.h"
#include "proc.h"
#include "tty.h"

#define IBUF_SIZE 4096
#define LINE_MAX  1024
#define CH_EOF    0x04        /* end-of-file marker inside ibuf (canonical) */

static char ibuf[IBUF_SIZE];
static uint32_t ihead, itail;  /* free-running indices */
static char line[LINE_MAX];
static uint32_t line_len;
static int lines_ready;
static int mode = TTY_COOKED;
static int fg_pgid;

static uint32_t ibuf_count(void)
{
    return ihead - itail;
}

static void ibuf_push(char c)
{
    if (ibuf_count() < IBUF_SIZE)
        ibuf[ihead++ % IBUF_SIZE] = c;
}

static char ibuf_pop(void)
{
    return ibuf[itail++ % IBUF_SIZE];
}

static void echo_raw(const char *s, uint32_t n)
{
    if (mode & TTY_ECHO)
        console_write(s, n);
}

static int is_ctl(char c)
{
    return (unsigned char)c < 32 && c != '\n' && c != '\t';
}

/* Control characters echo as ^X, like Linux's ECHOCTL. */
static void echo_char(char c)
{
    if (is_ctl(c)) {
        char b[2] = { '^', (char)(c + 64) };
        echo_raw(b, 2);
    } else {
        echo_raw(&c, 1);
    }
}

static void erase_one(void)
{
    if (!line_len)
        return;
    char c = line[--line_len];
    echo_raw("\b \b", 3);
    if (is_ctl(c))
        echo_raw("\b \b", 3);
}

static void commit_line(int eof)
{
    if (IBUF_SIZE - ibuf_count() < line_len + 1) {
        line_len = 0;               /* overflow: drop the line */
        return;
    }
    for (uint32_t i = 0; i < line_len; i++)
        ibuf_push(line[i]);
    if (eof)
        ibuf_push(CH_EOF);
    line_len = 0;
    lines_ready++;
    wakeup(ibuf);
}

void tty_input(char c)
{
    if (c == '\r')
        c = '\n';

    if ((mode & TTY_ISIG) && (c == 0x03 || c == 0x1C)) {
        echo_raw(c == 0x03 ? "^C\n" : "^\\\n", 3);
        line_len = 0;
        if (mode & TTY_ICANON) {
            ihead = itail = 0;
            lines_ready = 0;
        }
        if (fg_pgid > 0)
            proc_signal_group(fg_pgid, c == 0x03 ? SIGINT : SIGQUIT);
        return;
    }

    if (!(mode & TTY_ICANON)) {
        ibuf_push(c);
        echo_raw(&c, 1);
        wakeup(ibuf);
        return;
    }

    switch (c) {
    case 0x7F:
    case 0x08:
        erase_one();
        break;
    case 0x15:                          /* ^U: kill line */
        while (line_len)
            erase_one();
        break;
    case 0x17:                          /* ^W: erase word */
        while (line_len && line[line_len - 1] == ' ')
            erase_one();
        while (line_len && line[line_len - 1] != ' ')
            erase_one();
        break;
    case CH_EOF:                        /* ^D */
        commit_line(1);
        break;
    case '\n':
        line[line_len++] = '\n';
        echo_raw("\n", 1);
        commit_line(0);
        break;
    default:
        if (line_len < LINE_MAX - 1) {
            line[line_len++] = c;
            echo_char(c);
        }
        break;
    }
}

int tty_read(char *buf, uint32_t n)
{
    if (!n)
        return 0;
    for (;;) {
        if ((mode & TTY_ICANON) ? lines_ready > 0 : ibuf_count() > 0)
            break;
        if (signal_pending(current))
            return -EINTR;
        sleep_on(ibuf);
    }

    uint32_t i = 0;
    if (mode & TTY_ICANON) {
        while (i < n && ibuf_count()) {
            char c = ibuf_pop();
            if (c == CH_EOF) {
                lines_ready--;
                break;
            }
            buf[i++] = c;
            if (c == '\n') {
                lines_ready--;
                break;
            }
        }
    } else {
        while (i < n && ibuf_count())
            buf[i++] = ibuf_pop();
    }
    return (int)i;
}

int tty_write(const char *buf, uint32_t n)
{
    console_write(buf, n);
    return (int)n;
}

int tty_set_mode(int newmode)
{
    int old = mode;
    if (newmode < 0)
        return old;

    if ((old & TTY_ICANON) && !(newmode & TTY_ICANON)) {
        /* Hand type-ahead to the raw reader; drop EOF markers. */
        uint32_t n = ibuf_count();
        static char tmp[IBUF_SIZE];
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            char c = ibuf_pop();
            if (c != CH_EOF)
                tmp[k++] = c;
        }
        ihead = itail = 0;
        for (uint32_t i = 0; i < k; i++)
            ibuf_push(tmp[i]);
        for (uint32_t i = 0; i < line_len; i++)
            ibuf_push(line[i]);
        line_len = 0;
    }

    mode = newmode & (TTY_ECHO | TTY_ICANON | TTY_ISIG);

    if (mode & TTY_ICANON) {
        lines_ready = 0;
        for (uint32_t i = itail; i != ihead; i++) {
            char c = ibuf[i % IBUF_SIZE];
            if (c == '\n' || c == CH_EOF)
                lines_ready++;
        }
    }
    wakeup(ibuf);
    return old;
}

int tty_get_fg(void)
{
    return fg_pgid;
}

void tty_set_fg(int pgid)
{
    fg_pgid = pgid;
}

void tty_init(void)
{
    ihead = itail = 0;
    line_len = 0;
    lines_ready = 0;
    mode = TTY_COOKED;
}
