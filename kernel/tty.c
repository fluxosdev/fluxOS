/*
 * Terminals. Input is collected here and handed to readers either line
 * by line (canonical mode, with echo and erase/kill editing) or byte by
 * byte (raw mode, used by the shell's line editor). ^C and ^\ signal the
 * terminal's foreground process group.
 *
 * The console's input comes from the keyboard and serial port and its
 * output goes to the screen. A pseudo-terminal (pty) connects a master
 * side, held by a terminal emulator such as the desktop's terminal
 * windows, to a slave side that a shell uses like the console: bytes
 * written to the master are the slave's input, and the slave's output
 * is buffered for the master to read.
 */
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "tty.h"
#include "vfs.h"

#define IBUF_SIZE 4096
#define LINE_MAX  1024
#define OBUF_SIZE 4096
#define MAX_PTYS  32
#define CH_EOF    0x04        /* end-of-file marker inside ibuf (canonical) */

struct pty;

struct tty {
    char ibuf[IBUF_SIZE];
    uint32_t ihead, itail;      /* free-running indices */
    char line[LINE_MAX];
    uint32_t line_len;
    int lines_ready;
    int mode;
    int fg_pgid;
    struct pty *pty;            /* NULL for the console */
};

struct pty {
    struct tty tty;
    char obuf[OBUF_SIZE];       /* slave output, read by the master */
    uint32_t ohead, otail;
    int index;
    int refs;                   /* master, slave and controlling-tty users */
    int hungup;                 /* the master was closed */
    int slave_closed;
};

struct tty console_tty;
static struct pty *ptys[MAX_PTYS];

static uint32_t ibuf_count(struct tty *t)
{
    return t->ihead - t->itail;
}

static void ibuf_push(struct tty *t, char c)
{
    if (ibuf_count(t) < IBUF_SIZE)
        t->ibuf[t->ihead++ % IBUF_SIZE] = c;
}

static char ibuf_pop(struct tty *t)
{
    return t->ibuf[t->itail++ % IBUF_SIZE];
}

static int readable(struct tty *t)
{
    return (t->mode & TTY_ICANON) ? t->lines_ready > 0 : ibuf_count(t) > 0;
}

/*
 * Terminal output. A pty buffers it for the master; a writer that fills
 * the buffer waits for the master to catch up, but echo (which happens
 * while the master itself is writing) never waits and may be dropped.
 */
static int output(struct tty *t, const char *s, uint32_t n, int block)
{
    struct pty *p = t->pty;
    if (!p) {
        console_write(s, n);
        return (int)n;
    }
    uint32_t i = 0;
    while (i < n) {
        if (p->hungup)
            return i ? (int)i : -EIO;
        if (p->ohead - p->otail == OBUF_SIZE) {
            if (!block)
                break;
            wakeup(p->obuf);
            poll_wakeup();
            if (signal_pending(current))
                return i ? (int)i : -EINTR;
            sleep_on(p->obuf);
            continue;
        }
        p->obuf[p->ohead++ % OBUF_SIZE] = s[i++];
    }
    wakeup(p->obuf);
    poll_wakeup();
    return (int)i;
}

static void echo_raw(struct tty *t, const char *s, uint32_t n)
{
    if (t->mode & TTY_ECHO)
        output(t, s, n, 0);
}

static int is_ctl(char c)
{
    return (unsigned char)c < 32 && c != '\n' && c != '\t';
}

/* Control characters echo as ^X, like Linux's ECHOCTL. */
static void echo_char(struct tty *t, char c)
{
    if (is_ctl(c)) {
        char b[2] = { '^', (char)(c + 64) };
        echo_raw(t, b, 2);
    } else {
        echo_raw(t, &c, 1);
    }
}

static void erase_one(struct tty *t)
{
    if (!t->line_len)
        return;
    char c = t->line[--t->line_len];
    echo_raw(t, "\b \b", 3);
    if (is_ctl(c))
        echo_raw(t, "\b \b", 3);
}

static void input_ready(struct tty *t)
{
    wakeup(t->ibuf);
    poll_wakeup();
}

static void commit_line(struct tty *t, int eof)
{
    if (IBUF_SIZE - ibuf_count(t) < t->line_len + 1) {
        t->line_len = 0;                /* overflow: drop the line */
        return;
    }
    for (uint32_t i = 0; i < t->line_len; i++)
        ibuf_push(t, t->line[i]);
    if (eof)
        ibuf_push(t, CH_EOF);
    t->line_len = 0;
    t->lines_ready++;
    input_ready(t);
}

static void input_char(struct tty *t, char c)
{
    if (c == '\r')
        c = '\n';

    if ((t->mode & TTY_ISIG) && (c == 0x03 || c == 0x1C)) {
        echo_raw(t, c == 0x03 ? "^C\n" : "^\\\n", 3);
        t->line_len = 0;
        if (t->mode & TTY_ICANON) {
            t->ihead = t->itail = 0;
            t->lines_ready = 0;
        }
        if (t->fg_pgid > 0)
            proc_signal_group(t->fg_pgid, c == 0x03 ? SIGINT : SIGQUIT);
        return;
    }

    if (!(t->mode & TTY_ICANON)) {
        ibuf_push(t, c);
        echo_raw(t, &c, 1);
        input_ready(t);
        return;
    }

    switch (c) {
    case 0x7F:
    case 0x08:
        erase_one(t);
        break;
    case 0x15:                          /* ^U: kill line */
        while (t->line_len)
            erase_one(t);
        break;
    case 0x17:                          /* ^W: erase word */
        while (t->line_len && t->line[t->line_len - 1] == ' ')
            erase_one(t);
        while (t->line_len && t->line[t->line_len - 1] != ' ')
            erase_one(t);
        break;
    case CH_EOF:                        /* ^D */
        commit_line(t, 1);
        break;
    case '\n':
        t->line[t->line_len++] = '\n';
        echo_raw(t, "\n", 1);
        commit_line(t, 0);
        break;
    default:
        if (t->line_len < LINE_MAX - 1) {
            t->line[t->line_len++] = c;
            echo_char(t, c);
        }
        break;
    }
}

void tty_input(char c)
{
    input_char(&console_tty, c);
}

int tty_read(struct tty *t, char *buf, uint32_t n)
{
    if (!n)
        return 0;
    for (;;) {
        if (readable(t))
            break;
        if (t->pty && t->pty->hungup)
            return 0;
        if (signal_pending(current))
            return -EINTR;
        sleep_on(t->ibuf);
    }

    uint32_t i = 0;
    if (t->mode & TTY_ICANON) {
        while (i < n && ibuf_count(t)) {
            char c = ibuf_pop(t);
            if (c == CH_EOF) {
                t->lines_ready--;
                break;
            }
            buf[i++] = c;
            if (c == '\n') {
                t->lines_ready--;
                break;
            }
        }
    } else {
        while (i < n && ibuf_count(t))
            buf[i++] = ibuf_pop(t);
    }
    return (int)i;
}

int tty_write(struct tty *t, const char *buf, uint32_t n)
{
    return output(t, buf, n, 1);
}

int tty_set_mode(struct tty *t, int newmode)
{
    int old = t->mode;
    if (newmode < 0)
        return old;

    if ((old & TTY_ICANON) && !(newmode & TTY_ICANON)) {
        /* Hand type-ahead to the raw reader; drop EOF markers. */
        uint32_t n = ibuf_count(t);
        static char tmp[IBUF_SIZE];
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            char c = ibuf_pop(t);
            if (c != CH_EOF)
                tmp[k++] = c;
        }
        t->ihead = t->itail = 0;
        for (uint32_t i = 0; i < k; i++)
            ibuf_push(t, tmp[i]);
        for (uint32_t i = 0; i < t->line_len; i++)
            ibuf_push(t, t->line[i]);
        t->line_len = 0;
    }

    t->mode = newmode & (TTY_ECHO | TTY_ICANON | TTY_ISIG);

    if (t->mode & TTY_ICANON) {
        t->lines_ready = 0;
        for (uint32_t i = t->itail; i != t->ihead; i++) {
            char c = t->ibuf[i % IBUF_SIZE];
            if (c == '\n' || c == CH_EOF)
                t->lines_ready++;
        }
    }
    input_ready(t);
    return old;
}

int tty_get_fg(struct tty *t)
{
    return t->fg_pgid;
}

void tty_set_fg(struct tty *t, int pgid)
{
    t->fg_pgid = pgid;
}

static void tty_reset(struct tty *t)
{
    t->ihead = t->itail = 0;
    t->line_len = 0;
    t->lines_ready = 0;
    t->mode = TTY_COOKED;
}

void tty_init(void)
{
    tty_reset(&console_tty);
}

/* ---- controlling terminals ---- */

static void pty_put(struct pty *p)
{
    if (--p->refs > 0)
        return;
    ptys[p->index] = NULL;
    kfree(p);
}

struct tty *tty_hold(struct tty *t)
{
    if (t && t->pty)
        t->pty->refs++;
    return t;
}

void tty_drop(struct tty *t)
{
    if (t && t->pty)
        pty_put(t->pty);
}

struct tty *tty_current(void)
{
    return current && current->ctty ? current->ctty : &console_tty;
}

static struct tty *vnode_tty(struct vnode *vn)
{
    return vn->dev_priv ? vn->dev_priv : tty_current();
}

void tty_exec_attach(struct proc *p)
{
    struct file *f = p->fds[0];
    if (!f || f->type != FT_VNODE || f->vn->type != VN_CHR)
        return;
    uint32_t major = MAJOR(f->vn->rdev);
    if (major != TTY_MAJOR && !(major == PTY_MAJOR && !(MINOR(f->vn->rdev) & 1)))
        return;
    struct tty *t = vnode_tty(f->vn);
    if (t == p->ctty)
        return;
    tty_drop(p->ctty);
    p->ctty = tty_hold(t);
}

/* ---- device nodes ---- */

int tty_dev_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    (void)off;
    return tty_read(vnode_tty(vn), buf, n);
}

int tty_dev_write(struct vnode *vn, const void *buf, uint32_t off, uint32_t n)
{
    (void)off;
    return tty_write(vnode_tty(vn), buf, n);
}

int tty_dev_poll(struct vnode *vn)
{
    struct tty *t = vnode_tty(vn);
    int ev = POLLOUT;
    if (readable(t))
        ev |= POLLIN;
    if (t->pty && t->pty->hungup)
        ev |= POLLHUP;
    return ev;
}

/* ---- pseudo-terminals ---- */

static int master_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    (void)off;
    struct pty *p = vn->dev_priv;
    while (p->ohead == p->otail) {
        if (p->slave_closed)
            return 0;
        if (signal_pending(current))
            return -EINTR;
        sleep_on(p->obuf);
    }
    uint32_t i = 0;
    char *d = buf;
    while (i < n && p->otail != p->ohead)
        d[i++] = p->obuf[p->otail++ % OBUF_SIZE];
    wakeup(p->obuf);
    return (int)i;
}

static int master_write(struct vnode *vn, const void *buf, uint32_t off, uint32_t n)
{
    (void)off;
    struct pty *p = vn->dev_priv;
    const char *s = buf;
    for (uint32_t i = 0; i < n; i++)
        input_char(&p->tty, s[i]);
    return (int)n;
}

static int master_poll(struct vnode *vn)
{
    struct pty *p = vn->dev_priv;
    int ev = POLLOUT;
    if (p->ohead != p->otail)
        ev |= POLLIN;
    if (p->slave_closed)
        ev |= POLLHUP;
    return ev;
}

/* Closing the master hangs up the terminal: its processes get SIGHUP. */
static void master_release(struct vnode *vn)
{
    struct pty *p = vn->dev_priv;
    p->hungup = 1;
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state != P_UNUSED && procs[i].ctty == &p->tty)
            proc_signal(&procs[i], SIGHUP);
    wakeup(p->tty.ibuf);
    wakeup(p->obuf);
    pty_put(p);
}

static void slave_release(struct vnode *vn)
{
    struct pty *p = ((struct tty *)vn->dev_priv)->pty;
    p->slave_closed = 1;
    wakeup(p->obuf);
    poll_wakeup();
    pty_put(p);
}

int pty_create(struct file **master, struct file **slave)
{
    int idx = 0;
    while (idx < MAX_PTYS && ptys[idx])
        idx++;
    if (idx == MAX_PTYS)
        return -EAGAIN;

    struct pty *p = kzalloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    tty_reset(&p->tty);
    p->tty.pty = p;
    p->index = idx;

    struct vnode *mv = vfs_mknod_anon(MKDEV(PTY_MAJOR, 2 * idx + 1), master_read, master_write);
    struct vnode *sv = vfs_mknod_anon(MKDEV(PTY_MAJOR, 2 * idx), tty_dev_read, tty_dev_write);
    struct file *mf = mv ? file_open(mv, O_RDWR) : NULL;
    struct file *sf = sv ? file_open(sv, O_RDWR) : NULL;
    if (!mf || !sf) {
        /* Nothing refers to p yet, so the vnodes have no release hooks. */
        if (mf)
            file_close(mf);
        else
            vfs_unref(mv);
        if (sf)
            file_close(sf);
        else
            vfs_unref(sv);
        kfree(p);
        return -ENOMEM;
    }
    mv->dev_priv = p;
    mv->dev_poll = master_poll;
    mv->dev_release = master_release;
    sv->dev_priv = &p->tty;
    sv->dev_poll = tty_dev_poll;
    sv->dev_release = slave_release;
    p->refs = 2;
    ptys[idx] = p;
    *master = mf;
    *slave = sf;
    return 0;
}
