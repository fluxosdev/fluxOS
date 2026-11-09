/*
 * termios over the kernel's line discipline (SYS_ttymode): ECHO, ICANON
 * and ISIG are what it does; the rest of a struct termios is remembered
 * per process and handed back unchanged.
 */
#include <errno.h>
#include <flux.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved;
static int have_saved;

static void defaults(struct termios *t)
{
    memset(t, 0, sizeof(*t));
    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD | B38400;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN | ECHOCTL | ECHOKE;
    t->c_cc[VINTR] = 3;
    t->c_cc[VQUIT] = 28;
    t->c_cc[VERASE] = 127;
    t->c_cc[VKILL] = 21;
    t->c_cc[VEOF] = 4;
    t->c_cc[VMIN] = 1;
    t->c_cc[VSTART] = 17;
    t->c_cc[VSTOP] = 19;
    t->c_cc[VSUSP] = 26;
    t->c_cc[VWERASE] = 23;
    t->c_ispeed = t->c_ospeed = B38400;
}

int tcgetattr(int fd, struct termios *t)
{
    if (!isatty(fd)) {
        errno = ENOTTY;
        return -1;
    }
    if (have_saved)
        *t = saved;
    else
        defaults(t);
    int mode = ttymode(-1);
    if (mode >= 0) {
        t->c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG);
        if (mode & TTY_ECHO)
            t->c_lflag |= ECHO;
        if (mode & TTY_ICANON)
            t->c_lflag |= ICANON;
        if (mode & TTY_ISIG)
            t->c_lflag |= ISIG;
    }
    return 0;
}

int tcsetattr(int fd, int when, const struct termios *t)
{
    (void)when;
    if (!isatty(fd)) {
        errno = ENOTTY;
        return -1;
    }
    int mode = 0;
    if (t->c_lflag & ECHO)
        mode |= TTY_ECHO;
    if (t->c_lflag & ICANON)
        mode |= TTY_ICANON;
    if (t->c_lflag & ISIG)
        mode |= TTY_ISIG;
    ttymode(mode);
    saved = *t;
    have_saved = 1;
    return 0;
}

void cfmakeraw(struct termios *t)
{
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }
int cfsetispeed(struct termios *t, speed_t speed) { t->c_ispeed = speed; return 0; }
int cfsetospeed(struct termios *t, speed_t speed) { t->c_ospeed = speed; return 0; }

int tcflush(int fd, int queue)
{
    (void)queue;
    return isatty(fd) ? 0 : (errno = ENOTTY, -1);
}

int tcdrain(int fd) { return tcflush(fd, 0); }
int tcsendbreak(int fd, int duration) { (void)duration; return tcflush(fd, 0); }
int tcflow(int fd, int action) { (void)action; return tcflush(fd, 0); }
