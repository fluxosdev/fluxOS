/*
 * The POSIX calls fluxOS has no system call of its own for, built from
 * those it has: the exec family, sessions, sysconf, links, select over
 * poll, resource limits, wait3/wait4, and the stubs a single-user system
 * answers simply (ownership, groups, locks).
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <flux/syscall.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sched.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

static inline int sc0(int n)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}

static inline int sc1(int n, int a)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}

static inline int sc2(int n, int a, int b)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static inline int sc3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int ret(int r)
{
    if (r < 0 && r > -4096) {
        errno = -r;
        return -1;
    }
    return r;
}

/* ---- the exec family ---- */

#define MAX_LIST_ARGS 1024

static int collect(const char *arg0, va_list ap, char **argv, char ***envp)
{
    int n = 0;
    argv[n++] = (char *)arg0;
    while (arg0) {
        char *a = va_arg(ap, char *);
        if (n == MAX_LIST_ARGS - 1) {
            errno = E2BIG;
            return -1;
        }
        argv[n++] = a;
        if (!a)
            break;
    }
    if (envp)
        *envp = va_arg(ap, char **);
    return 0;
}

int execl(const char *path, const char *arg, ...)
{
    char *argv[MAX_LIST_ARGS];
    va_list ap;
    va_start(ap, arg);
    int r = collect(arg, ap, argv, NULL);
    va_end(ap);
    return r < 0 ? -1 : execv(path, argv);
}

int execlp(const char *file, const char *arg, ...)
{
    char *argv[MAX_LIST_ARGS];
    va_list ap;
    va_start(ap, arg);
    int r = collect(arg, ap, argv, NULL);
    va_end(ap);
    return r < 0 ? -1 : execvp(file, argv);
}

int execle(const char *path, const char *arg, ...)
{
    char *argv[MAX_LIST_ARGS];
    char **envp;
    va_list ap;
    va_start(ap, arg);
    int r = collect(arg, ap, argv, &envp);
    va_end(ap);
    return r < 0 ? -1 : execve(path, argv, envp);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    char **saved = environ;
    environ = (char **)envp;
    execvp(file, argv);
    environ = saved;
    return -1;
}

pid_t vfork(void)
{
    return fork();
}

/* ---- sessions, ids, users ---- */

pid_t setsid(void)
{
    return ret(sc0(SYS_setsid));
}

pid_t getsid(pid_t pid)
{
    return ret(sc1(SYS_getsid, pid));
}

int seteuid(uid_t uid) { (void)uid; return 0; }
int setegid(gid_t gid) { (void)gid; return 0; }
int setreuid(uid_t r, uid_t e) { (void)r; (void)e; return 0; }
int setregid(gid_t r, gid_t e) { (void)r; (void)e; return 0; }

int getgroups(int size, gid_t list[])
{
    if (size > 0)
        list[0] = 0;
    return 1;
}

int fchown(int fd, uid_t owner, gid_t group)
{
    struct stat st;
    (void)owner;
    (void)group;
    return fstat(fd, &st);
}

int lchown(const char *path, uid_t owner, gid_t group)
{
    return chown(path, owner, group);
}

int nice(int inc)
{
    (void)inc;
    return 0;
}

int getpriority(int which, id_t who) { (void)which; (void)who; errno = 0; return 0; }
int setpriority(int which, id_t who, int prio) { (void)which; (void)who; (void)prio; return 0; }
int sched_get_priority_min(int policy) { (void)policy; return 0; }
int sched_get_priority_max(int policy) { (void)policy; return 0; }

/* ---- system information ---- */

long sysconf(int name)
{
    struct meminfo mi;
    switch (name) {
    case _SC_ARG_MAX: return ARG_MAX;
    case _SC_CHILD_MAX: return CHILD_MAX;
    case _SC_CLK_TCK: return 100;
    case _SC_NGROUPS_MAX: return NGROUPS_MAX;
    case _SC_OPEN_MAX: return OPEN_MAX;
    case _SC_STREAM_MAX: return FOPEN_MAX;
    case _SC_TZNAME_MAX: return TZNAME_MAX;
    case _SC_JOB_CONTROL: return 1;
    case _SC_SAVED_IDS: return 1;
    case _SC_VERSION: return _POSIX_VERSION;
    case _SC_PAGESIZE: return 4096;
    case _SC_LINE_MAX: return LINE_MAX;
    case _SC_HOST_NAME_MAX: return HOST_NAME_MAX;
    case _SC_LOGIN_NAME_MAX: return LOGIN_NAME_MAX;
    case _SC_TTY_NAME_MAX: return TTY_NAME_MAX;
    case _SC_SYMLOOP_MAX: return SYMLOOP_MAX;
    case _SC_GETPW_R_SIZE_MAX: return 1024;
    case _SC_GETGR_R_SIZE_MAX: return 1024;
    case _SC_MONOTONIC_CLOCK: return _POSIX_MONOTONIC_CLOCK;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
        return meminfo(&mi) == 0 && mi.ncpu ? (long)mi.ncpu : 1;
    case _SC_PHYS_PAGES:
        return meminfo(&mi) == 0 ? (long)(mi.total_kb / 4) : 0;
    case _SC_AVPHYS_PAGES:
        return meminfo(&mi) == 0 ? (long)(mi.free_kb / 4) : 0;
    }
    errno = EINVAL;
    return -1;
}

long fpathconf(int fd, int name)
{
    (void)fd;
    switch (name) {
    case _PC_LINK_MAX: return LINK_MAX;
    case _PC_MAX_CANON: return 1024;
    case _PC_MAX_INPUT: return 4096;
    case _PC_NAME_MAX: return NAME_MAX;
    case _PC_PATH_MAX: return PATH_MAX;
    case _PC_PIPE_BUF: return PIPE_BUF;
    case _PC_CHOWN_RESTRICTED: return 1;
    case _PC_NO_TRUNC: return 1;
    case _PC_VDISABLE: return 0;
    }
    errno = EINVAL;
    return -1;
}

long pathconf(const char *path, int name)
{
    (void)path;
    return fpathconf(-1, name);
}

int getpagesize(void)
{
    return 4096;
}

int gethostname(char *name, size_t size)
{
    struct utsname u;
    if (uname(&u) < 0)
        return -1;
    size_t n = strlen(u.nodename);
    if (n + 1 > size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, u.nodename, n + 1);
    return 0;
}

int sethostname(const char *name, size_t size)
{
    (void)name;
    (void)size;
    errno = EPERM;
    return -1;
}

int getloadavg(double loadavg[], int n)
{
    for (int i = 0; i < n && i < 3; i++)
        loadavg[i] = 0;
    return n < 3 ? n : 3;
}

/* ---- files ---- */

/* FAT and the RAM filesystem have no links of either kind: as Linux
 * answers for vfat. A path that is not a link makes readlink EINVAL. */
int link(const char *from, const char *to)
{
    struct stat st;
    if (stat(from, &st) < 0)
        return -1;
    (void)to;
    errno = EPERM;
    return -1;
}

int symlink(const char *target, const char *path)
{
    (void)target;
    (void)path;
    errno = EPERM;
    return -1;
}

ssize_t readlink(const char *path, char *buf, size_t size)
{
    struct stat st;
    (void)buf;
    (void)size;
    if (stat(path, &st) < 0)
        return -1;
    errno = EINVAL;
    return -1;
}

int mkfifo(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int mknod(const char *path, mode_t mode, dev_t dev)
{
    (void)path;
    (void)mode;
    (void)dev;
    errno = EPERM;
    return -1;
}

int truncate(const char *path, off_t size)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    int r = ftruncate(fd, size);
    int saved = errno;
    close(fd);
    errno = saved;
    return r;
}

int fdatasync(int fd)
{
    return fsync(fd);
}

int fchdir(int fd)
{
    return ret(sc1(SYS_fchdir, fd));
}

int fchmod(int fd, mode_t mode)
{
    return ret(sc2(SYS_fchmod, fd, (int)mode));
}

int dup3(int oldfd, int newfd, int flags)
{
    if (oldfd == newfd || (flags & ~O_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    int r = dup2(oldfd, newfd);
    if (r >= 0 && (flags & O_CLOEXEC))
        fcntl(r, F_SETFD, FD_CLOEXEC);
    return r;
}

int pipe2(int fds[2], int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
        errno = EINVAL;
        return -1;
    }
    if (pipe(fds) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        if (flags & O_CLOEXEC)
            fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        if (flags & O_NONBLOCK)
            fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
    }
    return 0;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (dirfd != AT_FDCWD && path[0] != '/') {
        errno = ENOSYS;                 /* only paths from the working directory */
        return -1;
    }
    return open(path, flags, mode);
}

int posix_fadvise(int fd, off_t off, off_t len, int advice)
{
    (void)fd; (void)off; (void)len; (void)advice;
    return 0;
}

int flock(int fd, int op)
{
    (void)op;
    struct stat st;
    return fstat(fd, &st);              /* one user: every lock is granted */
}

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    off_t here = lseek(fd, 0, SEEK_CUR);
    if (here < 0 || lseek(fd, off, SEEK_SET) < 0)
        return -1;
    ssize_t r = read(fd, buf, n);
    int saved = errno;
    lseek(fd, here, SEEK_SET);
    errno = saved;
    return r;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    off_t here = lseek(fd, 0, SEEK_CUR);
    if (here < 0 || lseek(fd, off, SEEK_SET) < 0)
        return -1;
    ssize_t r = write(fd, buf, n);
    int saved = errno;
    lseek(fd, here, SEEK_SET);
    errno = saved;
    return r;
}

ssize_t readv(int fd, const struct iovec *iov, int n)
{
    ssize_t total = 0;
    for (int i = 0; i < n; i++) {
        ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0)
            return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len)
            break;
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *iov, int n)
{
    size_t len = 0;
    for (int i = 0; i < n; i++)
        len += iov[i].iov_len;
    /* one write, so that a pipe's reader sees it in one piece */
    char small[1024];
    char *buf = len <= sizeof(small) ? small : malloc(len);
    if (!buf)
        return -1;
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        memcpy(buf + o, iov[i].iov_base, iov[i].iov_len);
        o += iov[i].iov_len;
    }
    ssize_t r = write(fd, buf, len);
    if (buf != small)
        free(buf);
    return r;
}

/* ---- terminals ---- */

int ttyname_r(int fd, char *buf, size_t size)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return errno;
    if (!isatty(fd))
        return ENOTTY;
    char name[32];
    if (MAJOR(st.st_rdev) == PTY_MAJOR)
        snprintf(name, sizeof(name), "/dev/pts/%u", MINOR(st.st_rdev) / 2);
    else
        snprintf(name, sizeof(name), "/dev/tty");
    if (strlen(name) + 1 > size)
        return ERANGE;
    strcpy(buf, name);
    return 0;
}

char *ttyname(int fd)
{
    static char name[32];
    int r = ttyname_r(fd, name, sizeof(name));
    if (r) {
        errno = r;
        return NULL;
    }
    return name;
}

char *getlogin(void)
{
    char *u = getenv("USER");
    return u ? u : "flux";
}

int getlogin_r(char *buf, size_t size)
{
    const char *u = getlogin();
    if (strlen(u) + 1 > size)
        return ERANGE;
    strcpy(buf, u);
    return 0;
}

/* ---- time ---- */

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000) {
        errno = EINVAL;
        return -1;
    }
    unsigned int start = uptime_ms();
    unsigned long long ms = (unsigned long long)req->tv_sec * 1000 + (unsigned long long)(req->tv_nsec + 999999) / 1000000;
    while (ms) {
        unsigned int chunk = ms > 0x7FFFFFFF ? 0x7FFFFFFF : (unsigned int)ms;
        if (msleep(chunk) < 0) {
            if (rem) {
                unsigned int done = uptime_ms() - start;
                unsigned long long left = ms > done ? ms - done : 0;
                rem->tv_sec = (time_t)(left / 1000);
                rem->tv_nsec = (long)(left % 1000) * 1000000;
            }
            return -1;                  /* EINTR, from msleep */
        }
        ms -= chunk;
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    switch (clk) {
    case CLOCK_REALTIME: {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) < 0)
            return -1;
        ts->tv_sec = tv.tv_sec;
        ts->tv_nsec = tv.tv_usec * 1000;
        return 0;
    }
    case CLOCK_MONOTONIC: {
        unsigned int ms = uptime_ms();
        ts->tv_sec = (time_t)(ms / 1000);
        ts->tv_nsec = (long)(ms % 1000) * 1000000;
        return 0;
    }
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID: {
        clock_t c = clock();
        ts->tv_sec = c / CLOCKS_PER_SEC;
        ts->tv_nsec = (long)(c % CLOCKS_PER_SEC) * (1000000000 / CLOCKS_PER_SEC);
        return 0;
    }
    }
    errno = EINVAL;
    return -1;
}

int clock_getres(clockid_t clk, struct timespec *ts)
{
    if (clk < CLOCK_REALTIME || clk > CLOCK_THREAD_CPUTIME_ID) {
        errno = EINVAL;
        return -1;
    }
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = clk == CLOCK_REALTIME ? 1000 : 1000000;
    }
    return 0;
}

int clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem)
{
    struct timespec rel = *req;
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        if (clock_gettime(clk, &now) < 0)
            return errno;
        rel.tv_sec = req->tv_sec - now.tv_sec;
        rel.tv_nsec = req->tv_nsec - now.tv_nsec;
        if (rel.tv_nsec < 0) {
            rel.tv_sec--;
            rel.tv_nsec += 1000000000;
        }
        if (rel.tv_sec < 0)
            return 0;
        rem = NULL;
    }
    return nanosleep(&rel, rem) < 0 ? errno : 0;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tz;
    if (!tv)
        return 0;
    time_t t = tv->tv_sec;
    return stime(&t);
}

/* ---- interval timers (the kernel's, in milliseconds) ---- */

static uint32_t tv_ms(const struct timeval *tv)
{
    if (tv->tv_sec < 0 || tv->tv_sec > 0x7FFFFFFF / 1000)
        return 0x7FFFFFFF;
    uint32_t ms = (uint32_t)tv->tv_sec * 1000 + ((uint32_t)tv->tv_usec + 999) / 1000;
    return ms || (!tv->tv_sec && !tv->tv_usec) ? ms : 1;
}

static void ms_tv(uint32_t ms, struct timeval *tv)
{
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
}

int getitimer(int which, struct itimerval *value)
{
    struct flux_itimer t;
    if (ret(sc2(SYS_getitimer, which, (int)&t)) < 0)
        return -1;
    ms_tv(t.value_ms, &value->it_value);
    ms_tv(t.interval_ms, &value->it_interval);
    return 0;
}

int setitimer(int which, const struct itimerval *value, struct itimerval *old)
{
    struct flux_itimer t = { 0, 0 }, o;
    if (value) {
        if (value->it_value.tv_usec < 0 || value->it_value.tv_usec >= 1000000 ||
            value->it_interval.tv_usec < 0 || value->it_interval.tv_usec >= 1000000) {
            errno = EINVAL;
            return -1;
        }
        t.value_ms = tv_ms(&value->it_value);
        t.interval_ms = tv_ms(&value->it_interval);
    }
    if (ret(sc3(SYS_setitimer, which, value ? (int)&t : 0, (int)&o)) < 0)
        return -1;
    if (old) {
        ms_tv(o.value_ms, &old->it_value);
        ms_tv(o.interval_ms, &old->it_interval);
    }
    return 0;
}

unsigned int alarm(unsigned int seconds)
{
    struct flux_itimer t = { seconds > 0x7FFFFFFF / 1000 ? 0x7FFFFFFF : seconds * 1000, 0 }, o;
    if (sc3(SYS_setitimer, ITIMER_REAL, (int)&t, (int)&o) < 0)
        return 0;
    return (o.value_ms + 999) / 1000;
}

int pause(void)
{
    for (;;)
        if (msleep(0x7FFFFFFF) < 0)
            return -1;
}

/* ---- select, over poll ---- */

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout)
{
    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    struct pollfd fds[FD_SETSIZE];
    int n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (rd && FD_ISSET(fd, rd))
            ev |= POLLIN;
        if (wr && FD_ISSET(fd, wr))
            ev |= POLLOUT;
        if (ex && FD_ISSET(fd, ex))
            ev |= POLLPRI;
        if (ev) {
            fds[n].fd = fd;
            fds[n].events = ev;
            fds[n].revents = 0;
            n++;
        }
    }
    int ms = -1;
    if (timeout) {
        long long t = (long long)timeout->tv_sec * 1000 + (timeout->tv_usec + 999) / 1000;
        ms = t > 0x7FFFFFFF ? 0x7FFFFFFF : (int)t;
    }
    int r = poll(fds, (unsigned int)n, ms);
    if (r < 0)
        return -1;
    if (rd)
        FD_ZERO(rd);
    if (wr)
        FD_ZERO(wr);
    if (ex)
        FD_ZERO(ex);
    int count = 0;
    for (int i = 0; i < n; i++) {
        short re = fds[i].revents;
        if (re & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        if (rd && (fds[i].events & POLLIN) && (re & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(fds[i].fd, rd);
            count++;
        }
        if (wr && (fds[i].events & POLLOUT) && (re & (POLLOUT | POLLERR))) {
            FD_SET(fds[i].fd, wr);
            count++;
        }
        if (ex && (fds[i].events & POLLPRI) && (re & POLLPRI)) {
            FD_SET(fds[i].fd, ex);
            count++;
        }
    }
    if (timeout) {
        timeout->tv_sec = 0;
        timeout->tv_usec = 0;
    }
    return count;
}

int pselect(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, const struct timespec *timeout, const sigset_t *mask)
{
    sigset_t old;
    struct timeval tv, *tvp = NULL;
    if (timeout) {
        tv.tv_sec = timeout->tv_sec;
        tv.tv_usec = (suseconds_t)(timeout->tv_nsec / 1000);
        tvp = &tv;
    }
    if (mask)
        sigprocmask(SIG_SETMASK, mask, &old);
    int r = select(nfds, rd, wr, ex, tvp);
    if (mask) {
        int saved = errno;
        sigprocmask(SIG_SETMASK, &old, NULL);
        errno = saved;
    }
    return r;
}

/* ---- children ---- */

pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage)
{
    pid_t r = waitpid(pid, status, options);
    if (r > 0 && usage)
        memset(usage, 0, sizeof(*usage));
    return r;
}

pid_t wait3(int *status, int options, struct rusage *usage)
{
    return wait4(-1, status, options, usage);
}

/* ---- resource limits: what the kernel enforces, reported ---- */

int getrlimit(int resource, struct rlimit *rl)
{
    switch (resource) {
    case RLIMIT_STACK:
        rl->rlim_cur = rl->rlim_max = 32ul * 1024 * 1024;
        return 0;
    case RLIMIT_NOFILE:
        rl->rlim_cur = rl->rlim_max = OPEN_MAX;
        return 0;
    case RLIMIT_NPROC:
        rl->rlim_cur = rl->rlim_max = CHILD_MAX;
        return 0;
    case RLIMIT_CORE:
        rl->rlim_cur = rl->rlim_max = 0;
        return 0;
    case RLIMIT_CPU:
    case RLIMIT_FSIZE:
    case RLIMIT_DATA:
    case RLIMIT_RSS:
    case RLIMIT_MEMLOCK:
    case RLIMIT_AS:
        rl->rlim_cur = rl->rlim_max = RLIM_INFINITY;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int setrlimit(int resource, const struct rlimit *rl)
{
    struct rlimit cur;
    if (getrlimit(resource, &cur) < 0)
        return -1;
    if (rl->rlim_cur > cur.rlim_max || rl->rlim_max > cur.rlim_max) {
        errno = EPERM;
        return -1;
    }
    return 0;                           /* lower limits are accepted and not enforced */
}

/* ---- signals ---- */

int killpg(pid_t pgrp, int sig)
{
    if (pgrp <= 1) {
        errno = EINVAL;
        return -1;
    }
    return kill(-pgrp, sig);
}

int sigpending(sigset_t *set)
{
    *set = (sigset_t)sc0(SYS_sigpending);
    return 0;
}

int sigsuspend(const sigset_t *mask)
{
    sigset_t old;
    sigprocmask(SIG_SETMASK, mask, &old);
    while (msleep(0x7FFFFFFF) == 0)
        ;
    int saved = errno;
    sigprocmask(SIG_SETMASK, &old, NULL);
    errno = saved;
    return -1;
}

int sigaltstack(const stack_t *ss, stack_t *old)
{
    struct flux_stack ks, ko;
    if (ss) {
        ks.sp = (uint32_t)ss->ss_sp;
        ks.flags = (uint32_t)ss->ss_flags;
        ks.size = ss->ss_size;
    }
    if (ret(sc2(SYS_sigaltstack, ss ? (int)&ks : 0, old ? (int)&ko : 0)) < 0)
        return -1;
    if (old) {
        old->ss_sp = (void *)ko.sp;
        old->ss_flags = (int)ko.flags;
        old->ss_size = ko.size;
    }
    return 0;
}

int siginterrupt(int sig, int flag)
{
    struct sigaction sa;
    if (sigaction(sig, NULL, &sa) < 0)
        return -1;
    if (flag)
        sa.sa_flags &= ~SA_RESTART;
    else
        sa.sa_flags |= SA_RESTART;
    return sigaction(sig, &sa, NULL);
}

sighandler_t bsd_signal(int sig, sighandler_t handler)
{
    return signal(sig, handler);
}

void psignal(int sig, const char *msg)
{
    if (msg && *msg)
        fprintf(stderr, "%s: %s\n", msg, strsignal(sig));
    else
        fprintf(stderr, "%s\n", strsignal(sig));
}

const char *const sys_siglist[NSIG] = {
    [SIGHUP] = "Hangup", [SIGINT] = "Interrupt", [SIGQUIT] = "Quit", [SIGILL] = "Illegal instruction",
    [SIGTRAP] = "Trace/breakpoint trap", [SIGABRT] = "Aborted", [SIGBUS] = "Bus error",
    [SIGFPE] = "Floating point exception", [SIGKILL] = "Killed", [SIGUSR1] = "User defined signal 1",
    [SIGSEGV] = "Segmentation fault", [SIGUSR2] = "User defined signal 2", [SIGPIPE] = "Broken pipe",
    [SIGALRM] = "Alarm clock", [SIGTERM] = "Terminated", [SIGCHLD] = "Child exited",
    [SIGCONT] = "Continued", [SIGSTOP] = "Stopped (signal)", [SIGTSTP] = "Stopped",
    [SIGTTIN] = "Stopped (tty input)", [SIGTTOU] = "Stopped (tty output)", [SIGURG] = "Urgent I/O condition",
    [SIGXCPU] = "CPU time limit exceeded", [SIGXFSZ] = "File size limit exceeded",
    [SIGVTALRM] = "Virtual timer expired", [SIGPROF] = "Profiling timer expired",
    [SIGWINCH] = "Window changed", [SIGIO] = "I/O possible", [SIGSYS] = "Bad system call",
};

/* ---- memory ---- */

int msync(void *addr, size_t len, int flags)
{
    (void)addr; (void)len; (void)flags;
    return 0;                           /* mappings are anonymous: nothing to write back */
}

int madvise(void *addr, size_t len, int advice)
{
    (void)addr; (void)len; (void)advice;
    return 0;
}

int posix_madvise(void *addr, size_t len, int advice)
{
    (void)addr; (void)len; (void)advice;
    return 0;
}

int mlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
int munlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }

int brk(void *addr)
{
    void *cur = sbrk(0);
    if (sbrk((int)((char *)addr - (char *)cur)) == (void *)-1)
        return -1;
    return 0;
}

/* ---- no dynamic linking ---- */

static const char *dl_error = "dynamic loading is not supported on fluxOS";
static int dl_failed;

void *dlopen(const char *path, int flags)
{
    (void)path;
    (void)flags;
    dl_failed = 1;
    return NULL;
}

void *dlsym(void *handle, const char *name)
{
    (void)handle;
    (void)name;
    dl_failed = 1;
    return NULL;
}

int dlclose(void *handle)
{
    (void)handle;
    return 0;
}

char *dlerror(void)
{
    if (!dl_failed)
        return NULL;
    dl_failed = 0;
    return (char *)dl_error;
}
