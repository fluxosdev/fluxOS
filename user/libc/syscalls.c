/*
 * System call wrappers. The kernel returns -errno on failure; these
 * convert that into the usual -1 + errno convention.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <flux/syscall.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "pthread_impl.h"


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

static inline int sc4(int n, int a, int b, int c, int d)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

static inline int sc5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                     : "memory");
    return r;
}

static int ret(int r)
{
    if ((unsigned int)r >= (unsigned int)-4095) {
        errno = -r;
        return -1;
    }
    return r;
}

void _exit(int status)
{
    sc1(SYS_exit, status);
    for (;;)
        ;
}

/* The calling thread alone goes on in the child, so libc's locks must be free there (pthread.c). */
pid_t fork(void)
{
    __fork_prepare();
    int r = sc0(SYS_fork);
    if (r == 0)
        __fork_child();
    else
        __fork_parent();
    return ret(r);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return ret(sc3(SYS_execve, (int)path, (int)argv, (int)envp));
}

ssize_t read(int fd, void *buf, size_t n) { return ret(sc3(SYS_read, fd, (int)buf, (int)n)); }
ssize_t write(int fd, const void *buf, size_t n) { return ret(sc3(SYS_write, fd, (int)buf, (int)n)); }

int open(const char *path, int flags, ...)
{
    mode_t mode = 0666;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return ret(sc3(SYS_open, (int)path, flags, (int)mode));
}

int creat(const char *path, mode_t mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int fcntl(int fd, int cmd, ...)
{
    int arg = 0;
    va_list ap;
    va_start(ap, cmd);
    arg = va_arg(ap, int);
    va_end(ap);
    switch (cmd) {
    case F_GETLK:                       /* one user: no lock is ever in the way */
        if (ret(sc3(SYS_fcntl, fd, F_GETFD, 0)) < 0)
            return -1;
        ((struct flock *)arg)->l_type = F_UNLCK;
        return 0;
    case F_SETLK:
    case F_SETLKW:
    case F_SETOWN:
        return ret(sc3(SYS_fcntl, fd, F_GETFD, 0)) < 0 ? -1 : 0;
    case F_GETOWN:
        return ret(sc3(SYS_fcntl, fd, F_GETFD, 0)) < 0 ? -1 : getpid();
    }
    return ret(sc3(SYS_fcntl, fd, cmd, arg));
}

int close(int fd) { return ret(sc1(SYS_close, fd)); }
int dup(int fd) { return ret(sc1(SYS_dup, fd)); }
int dup2(int oldfd, int newfd) { return ret(sc2(SYS_dup2, oldfd, newfd)); }
int pipe(int fds[2]) { return ret(sc1(SYS_pipe, (int)fds)); }
off_t lseek(int fd, off_t off, int whence) { return ret(sc3(SYS_lseek, fd, off, whence)); }

off64_t lseek64(int fd, off64_t off, int whence)
{
    uint64_t pos;
    if (ret(sc5(SYS_llseek, fd, (int)((uint64_t)off >> 32), (int)off, (int)&pos, whence)) < 0)
        return -1;
    return (off64_t)pos;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    return ret(sc3(SYS_waitpid, pid, (int)status, options));
}

pid_t wait(int *status) { return waitpid(-1, status, 0); }

int chdir(const char *path) { return ret(sc1(SYS_chdir, (int)path)); }

char *getcwd(char *buf, size_t size)
{
    return ret(sc2(SYS_getcwd, (int)buf, (int)size)) < 0 ? NULL : buf;
}

int mkdir(const char *path, mode_t mode)
{
    return ret(sc2(SYS_mkdir, (int)path, (int)mode));
}

int rmdir(const char *path) { return ret(sc1(SYS_rmdir, (int)path)); }
int chmod(const char *path, mode_t mode) { return ret(sc2(SYS_chmod, (int)path, (int)mode)); }
mode_t umask(mode_t mask) { return (mode_t)sc1(SYS_umask, (int)mask); }
int utime(const char *path, const struct utimbuf *times)
{
    struct flux_utimbuf ft;
    if (times) {
        ft.actime = (uint32_t)times->actime;
        ft.modtime = (uint32_t)times->modtime;
    }
    return ret(sc2(SYS_utime, (int)path, times ? (int)&ft : 0));
}

int utimes(const char *path, const struct timeval times[2])
{
    struct flux_utimbuf ft;
    if (times) {
        ft.actime = (uint32_t)times[0].tv_sec;
        ft.modtime = (uint32_t)times[1].tv_sec;
    }
    return ret(sc2(SYS_utime, (int)path, times ? (int)&ft : 0));
}
int unlink(const char *path) { return ret(sc1(SYS_unlink, (int)path)); }
int rename(const char *from, const char *to) { return ret(sc2(SYS_rename, (int)from, (int)to)); }
int stat(const char *path, struct stat *st) { return ret(sc2(SYS_stat, (int)path, (int)st)); }
int lstat(const char *path, struct stat *st) { return ret(sc2(SYS_lstat, (int)path, (int)st)); }
int fstat(int fd, struct stat *st) { return ret(sc2(SYS_fstat, fd, (int)st)); }

int getdents(int fd, struct dirent *buf, unsigned int count)
{
    return ret(sc3(SYS_getdents, fd, (int)buf, (int)count));
}

pid_t getpid(void) { return sc0(SYS_getpid); }
pid_t getppid(void) { return sc0(SYS_getppid); }
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
int setuid(uid_t uid) { (void)uid; return 0; }
int setgid(gid_t gid) { (void)gid; return 0; }
/* Owners stay where the filesystem keeps them (ext2); -1 keeps one. */
int chown(const char *path, uid_t owner, gid_t group)
{
    return ret(sc4(SYS_chown, (int)path, (int)owner, (int)group, 1));
}

int lchown(const char *path, uid_t owner, gid_t group)
{
    return ret(sc4(SYS_chown, (int)path, (int)owner, (int)group, 0));
}

int fchown(int fd, uid_t owner, gid_t group)
{
    return ret(sc3(SYS_fchown, fd, (int)owner, (int)group));
}

/* Links are ext2's (and a symbolic one ramfs's too); FAT has neither:
 * EPERM, as Linux answers for vfat. */
int link(const char *from, const char *to) { return ret(sc2(SYS_link, (int)from, (int)to)); }
int symlink(const char *target, const char *path) { return ret(sc2(SYS_symlink, (int)target, (int)path)); }

ssize_t readlink(const char *path, char *buf, size_t size)
{
    return ret(sc3(SYS_readlink, (int)path, (int)buf, (int)size));
}
int kill(pid_t pid, int sig) { return ret(sc2(SYS_kill, pid, sig)); }
int raise(int sig) { return kill(getpid(), sig); }
int setpgid(pid_t pid, pid_t pgid) { return ret(sc2(SYS_setpgid, pid, pgid)); }
pid_t getpgid(pid_t pid) { return ret(sc1(SYS_getpgid, pid)); }
pid_t getpgrp(void) { return getpgid(0); }

pid_t tcgetpgrp(int fd)
{
    (void)fd;
    return ret(sc0(SYS_tcgetpgrp));
}

int tcsetpgrp(int fd, pid_t pgid)
{
    (void)fd;
    return ret(sc1(SYS_tcsetpgrp, pgid));
}

extern void __sigrestorer(void);

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    struct flux_sigaction kact, koact;
    if (act) {
        kact.fsa_handler = (uint32_t)act->sa_handler;
        kact.fsa_flags = act->sa_flags;
        kact.fsa_mask = act->sa_mask;
        kact.fsa_restorer = (uint32_t)(act->sa_restorer ? act->sa_restorer : __sigrestorer);
    }
    int r = ret(sc3(SYS_sigaction, sig, act ? (int)&kact : 0, oldact ? (int)&koact : 0));
    if (r == 0 && oldact) {
        oldact->sa_handler = (sighandler_t)koact.fsa_handler;
        oldact->sa_flags = koact.fsa_flags;
        oldact->sa_mask = koact.fsa_mask;
        oldact->sa_restorer = (void (*)(void))koact.fsa_restorer;
    }
    return r;
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction act, oact;
    act.sa_handler = handler;
    act.sa_flags = SA_RESTART;
    act.sa_mask = 0;
    act.sa_restorer = __sigrestorer;
    if (sigaction(sig, &act, &oact) < 0)
        return SIG_ERR;
    return oact.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return ret(sc3(SYS_sigprocmask, how, (int)set, (int)oldset));
}

int sigemptyset(sigset_t *set)
{
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set)
{
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = ~1u;
    return 0;
}

int sigaddset(sigset_t *set, int sig)
{
    if (!set || sig <= 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set |= 1u << sig;
    return 0;
}

int sigdelset(sigset_t *set, int sig)
{
    if (!set || sig <= 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1u << sig);
    return 0;
}

int sigismember(const sigset_t *set, int sig)
{
    if (!set || sig <= 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    return (*set & (1u << sig)) != 0;
}

void __sigjmp_save_mask(uint32_t *env)
{
    sigset_t mask = 0;
    sigprocmask(SIG_SETMASK, NULL, &mask);
    env[7] = mask;
}

void __sigjmp_restore_mask(const uint32_t *env)
{
    sigset_t mask = env[7];
    sigprocmask(SIG_SETMASK, &mask, NULL);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    struct flux_mmap_args a = {
        .addr = (uint32_t)addr,
        .len = (uint32_t)len,
        .prot = (uint32_t)prot,
        .flags = (uint32_t)flags,
        .fd = (int32_t)fd,
        .offset = (uint32_t)offset,
    };
    int r = sc1(SYS_mmap, (int)&a);
    if ((unsigned int)r >= (unsigned int)-4095) {
        errno = -r;
        return MAP_FAILED;
    }
    return (void *)r;
}

int munmap(void *addr, size_t len)
{
    return ret(sc2(SYS_munmap, (int)addr, (int)len));
}

int mprotect(void *addr, size_t len, int prot)
{
    return ret(sc3(SYS_mprotect, (int)addr, (int)len, prot));
}

void *sbrk(int incr)
{
    int r = ret(sc1(SYS_sbrk, incr));
    return r == -1 ? (void *)-1 : (void *)r;
}

int msleep(unsigned int ms) { return ret(sc1(SYS_sleep, (int)ms)); }

unsigned int sleep(unsigned int seconds)
{
    unsigned int start = uptime_ms();
    /* An hour at a time: the kernel takes milliseconds, which seconds
     * beyond 49 days would not fit in. */
    for (unsigned int left = seconds; left; ) {
        unsigned int chunk = left > 3600 ? 3600 : left;
        if (msleep(chunk * 1000) < 0) {
            unsigned int elapsed = (uptime_ms() - start) / 1000;
            return elapsed < seconds ? seconds - elapsed : 0;
        }
        left -= chunk;
    }
    return 0;
}

int usleep(unsigned int usec) { return msleep((usec + 999) / 1000); }

time_t time(time_t *t)
{
    time_t now = sc0(SYS_time);
    if (t)
        *t = now;
    return now;
}

int stime(const time_t *t) { return ret(sc1(SYS_stime, (int)*t)); }

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    if (tv) {
        /* Both count the same ticks: read the milliseconds within a second
         * that did not roll over meanwhile, or the time could go back. */
        int sec;
        unsigned int ms;
        do {
            sec = sc0(SYS_time);
            ms = uptime_ms();
        } while (sc0(SYS_time) != sec);
        tv->tv_sec = sec;
        tv->tv_usec = (suseconds_t)((ms % 1000) * 1000);
    }
    if (tz) {
        tz->tz_minuteswest = -tz_get();
        tz->tz_dsttime = 0;
    }
    return 0;
}

clock_t times(struct tms *buf)
{
    struct flux_tms ft;
    int r = ret(sc1(SYS_times, buf ? (int)&ft : 0));
    if (r >= 0 && buf) {
        buf->tms_utime = (clock_t)ft.tms_utime;
        buf->tms_stime = (clock_t)ft.tms_stime;
        buf->tms_cutime = (clock_t)ft.tms_cutime;
        buf->tms_cstime = (clock_t)ft.tms_cstime;
    }
    return (clock_t)r;
}

clock_t clock(void)
{
    struct tms t;
    if (times(&t) == (clock_t)-1)
        return (clock_t)-1;
    return t.tms_utime + t.tms_stime;
}

int getrusage(int who, struct rusage *usage)
{
    if (!usage || (who != RUSAGE_SELF && who != RUSAGE_CHILDREN)) {
        errno = EINVAL;
        return -1;
    }
    struct tms t;
    if (times(&t) == (clock_t)-1)
        return -1;
    memset(usage, 0, sizeof(*usage));
    clock_t ut = (who == RUSAGE_CHILDREN) ? t.tms_cutime : t.tms_utime;
    clock_t st = (who == RUSAGE_CHILDREN) ? t.tms_cstime : t.tms_stime;
    usage->ru_utime.tv_sec = ut / CLOCKS_PER_SEC;
    usage->ru_utime.tv_usec = (ut % CLOCKS_PER_SEC) * (1000000 / CLOCKS_PER_SEC);
    usage->ru_stime.tv_sec = st / CLOCKS_PER_SEC;
    usage->ru_stime.tv_usec = (st % CLOCKS_PER_SEC) * (1000000 / CLOCKS_PER_SEC);
    return 0;
}

int uname(struct utsname *buf) { return ret(sc1(SYS_uname, (int)buf)); }
int sched_yield(void) { return sc0(SYS_yield); }
int ttymode(int mode) { return sc1(SYS_ttymode, mode); }
int procinfo(int slot, struct procinfo *pi) { return ret(sc2(SYS_procinfo, slot, (int)pi)); }
int meminfo(struct meminfo *mi) { return ret(sc1(SYS_meminfo, (int)mi)); }
unsigned int uptime_ms(void) { return (unsigned int)sc0(SYS_uptime); }
int reboot(int cmd) { return ret(sc1(SYS_reboot, cmd)); }
int dmesg(char *buf, unsigned int size) { return ret(sc2(SYS_dmesg, (int)buf, (int)size)); }
int mount(const char *source, const char *target, const char *type)
{
    return ret(sc3(SYS_mount, (int)source, (int)target, (int)type));
}
int umount(const char *target) { return ret(sc1(SYS_umount, (int)target)); }
void sync(void) { sc0(SYS_sync); }
int blkrescan(const char *disk) { return ret(sc1(SYS_blkrescan, (int)disk)); }
int fstrim(const char *path, unsigned long long *trimmed)
{
    return ret(sc2(SYS_fstrim, (int)path, (int)trimmed));
}
int fsync(int fd) { return ret(sc1(SYS_fsync, fd)); }
int ftruncate(int fd, off_t size) { return ret(sc2(SYS_ftruncate, fd, (int)size)); }
int statfs(const char *path, struct statfs *st) { return ret(sc2(SYS_statfs, (int)path, (int)st)); }
int getmounts(char *buf, unsigned int size) { return ret(sc2(SYS_getmounts, (int)buf, (int)size)); }
int blkinfo(int index, struct blkinfo *bi) { return ret(sc2(SYS_blkinfo, index, (int)bi)); }
int usbinfo(int index, struct usbinfo *ui) { return ret(sc2(SYS_usbinfo, index, (int)ui)); }
int power(int op, unsigned int arg) { return ret(sc2(SYS_power, op, (int)arg)); }
int wifi(int op, void *arg) { return ret(sc2(SYS_wifi, op, (int)arg)); }

int poll(struct pollfd *fds, unsigned int nfds, int timeout_ms)
{
    return ret(sc3(SYS_poll, (int)fds, (int)nfds, timeout_ms));
}

int openpty(int fds[2]) { return ret(sc1(SYS_openpty, (int)fds)); }
int fbinfo(struct fbinfo *fi) { return ret(sc1(SYS_fbinfo, (int)fi)); }
int display(int op, void *arg) { return ret(sc2(SYS_display, op, (int)arg)); }

int ioctl(int fd, unsigned int request, ...)
{
    va_list ap;
    va_start(ap, request);
    int arg = va_arg(ap, int);
    va_end(ap);
    return ret(sc3(SYS_ioctl, fd, (int)request, arg));
}

void *fbmap(void)
{
    int r = sc0(SYS_fbmap);
    if ((unsigned int)r >= (unsigned int)-4095) {
        errno = -r;
        return NULL;
    }
    return (void *)r;
}

int isatty(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return 0;
    int pty_slave = MAJOR(st.st_rdev) == PTY_MAJOR && !(MINOR(st.st_rdev) & 1);
    if (!S_ISCHR(st.st_mode) || (MAJOR(st.st_rdev) != TTY_MAJOR && !pty_slave)) {
        errno = ENOTTY;
        return 0;
    }
    return 1;
}

int access(const char *path, int mode)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    if ((mode & X_OK) && !S_ISDIR(st.st_mode) && !(st.st_mode & 0111)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int remove(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    return S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
}

/* ---- sockets ---- */

int socket(int domain, int type, int protocol)
{
    return ret(sc3(SYS_socket, domain, type, protocol));
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    return ret(sc3(SYS_bind, fd, (int)addr, (int)len));
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    return ret(sc3(SYS_connect, fd, (int)addr, (int)len));
}

int listen(int fd, int backlog) { return ret(sc2(SYS_listen, fd, backlog)); }

/* The kernel always fills a whole sockaddr_in; give the caller what fits. */
static void give_addr(const struct sockaddr_in *sin, struct sockaddr *addr, socklen_t *len)
{
    if (!addr || !len)
        return;
    memcpy(addr, sin, *len < sizeof(*sin) ? *len : sizeof(*sin));
    *len = sizeof(*sin);
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    struct sockaddr_in sin = { 0 };     /* an AF_UNIX peer has no address for the kernel to fill */
    int r = ret(sc2(SYS_accept, fd, (int)&sin));
    if (r >= 0)
        give_addr(&sin, addr, len);
    return r;
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to,
               socklen_t len)
{
    if (to && len < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    return ret(sc5(SYS_sendto, fd, (int)buf, (int)n, flags, (int)to));
}

ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *len)
{
    struct sockaddr_in sin;
    int r = ret(sc5(SYS_recvfrom, fd, (int)buf, (int)n, flags, from ? (int)&sin : 0));
    if (r >= 0)
        give_addr(&sin, from, len);
    return r;
}

ssize_t send(int fd, const void *buf, size_t n, int flags) { return sendto(fd, buf, n, flags, NULL, 0); }
ssize_t recv(int fd, void *buf, size_t n, int flags) { return recvfrom(fd, buf, n, flags, NULL, NULL); }
int shutdown(int fd, int how) { return ret(sc2(SYS_shutdown, fd, how)); }

static int sockname(int fd, struct sockaddr *addr, socklen_t *len, int peer)
{
    struct sockaddr_in sin;
    int r = ret(sc3(SYS_sockname, fd, (int)&sin, peer));
    if (r == 0)
        give_addr(&sin, addr, len);
    return r;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) { return sockname(fd, addr, len, 0); }
int getpeername(int fd, struct sockaddr *addr, socklen_t *len) { return sockname(fd, addr, len, 1); }

int netinfo(struct netinfo *ni) { return ret(sc1(SYS_netinfo, (int)ni)); }
int netconf(const struct netinfo *ni) { return ret(sc1(SYS_netconf, (int)ni)); }

/* ---- shared memory ---- */

int shmget(key_t key, size_t size, int flags)
{
    return ret(sc3(SYS_shmget, key, (int)size, flags));
}

/* Where each segment is mapped and how long it is, for shmdt (fork copies the table with the
 * mappings). A spin lock: threads attaching at once are rare and quick. */
#define SHM_ATTACH_MAX 256
static struct {
    void *addr;
    size_t size;
} shm_attached[SHM_ATTACH_MAX];
static int shm_table_lock;

static void shm_lock_table(void)
{
    while (__atomic_exchange_n(&shm_table_lock, 1, __ATOMIC_ACQUIRE))
        sc0(SYS_yield);
}

static void shm_unlock_table(void)
{
    __atomic_store_n(&shm_table_lock, 0, __ATOMIC_RELEASE);
}

void *shmat(int id, const void *addr, int flags)
{
    (void)flags;
    if (addr) {
        errno = EINVAL;
        return (void *)-1;
    }
    shm_lock_table();
    int slot = -1;
    for (int i = 0; i < SHM_ATTACH_MAX && slot < 0; i++)
        if (!shm_attached[i].addr)
            slot = i;
    if (slot < 0) {
        shm_unlock_table();
        errno = EMFILE;
        return (void *)-1;
    }
    uint32_t size = 0;
    int r = sc2(SYS_shmat, id, (int)&size);
    if ((unsigned int)r >= (unsigned int)-4095) {
        shm_unlock_table();
        errno = -r;
        return (void *)-1;
    }
    shm_attached[slot].addr = (void *)r;
    shm_attached[slot].size = size;
    shm_unlock_table();
    return (void *)r;
}

int shmdt(const void *addr)
{
    shm_lock_table();
    for (int i = 0; i < SHM_ATTACH_MAX; i++) {
        if (addr && shm_attached[i].addr == addr) {
            size_t size = shm_attached[i].size;
            shm_attached[i].addr = NULL;
            shm_unlock_table();
            return munmap((void *)addr, size);
        }
    }
    shm_unlock_table();
    errno = EINVAL;
    return -1;
}

int shmctl(int id, int cmd, struct shmid_ds *buf)
{
    struct flux_shmid_ds ds;
    int r = ret(sc3(SYS_shmctl, id, cmd, (int)&ds));
    if (r == 0 && cmd == IPC_STAT && buf) {
        buf->shm_segsz = ds.shm_segsz;
        buf->shm_cpid = ds.shm_cpid;
        buf->shm_nattch = ds.shm_nattch;
    }
    return r;
}
