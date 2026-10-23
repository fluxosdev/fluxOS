/*
 * System call wrappers. The kernel returns -errno on failure; these
 * convert that into the usual -1 + errno convention.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <flux/syscall.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int errno;

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

pid_t fork(void) { return ret(sc0(SYS_fork)); }

int execve(const char *path, char *const argv[], char *const envp[])
{
    return ret(sc3(SYS_execve, (int)path, (int)argv, (int)envp));
}

ssize_t read(int fd, void *buf, size_t n) { return ret(sc3(SYS_read, fd, (int)buf, (int)n)); }
ssize_t write(int fd, const void *buf, size_t n) { return ret(sc3(SYS_write, fd, (int)buf, (int)n)); }

int open(const char *path, int flags, ...)
{
    return ret(sc2(SYS_open, (int)path, flags));
}

int creat(const char *path, mode_t mode)
{
    (void)mode;
    return open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

int close(int fd) { return ret(sc1(SYS_close, fd)); }
int dup(int fd) { return ret(sc1(SYS_dup, fd)); }
int dup2(int oldfd, int newfd) { return ret(sc2(SYS_dup2, oldfd, newfd)); }
int pipe(int fds[2]) { return ret(sc1(SYS_pipe, (int)fds)); }
off_t lseek(int fd, off_t off, int whence) { return ret(sc3(SYS_lseek, fd, off, whence)); }

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
    (void)mode;
    return ret(sc1(SYS_mkdir, (int)path));
}

int rmdir(const char *path) { return ret(sc1(SYS_rmdir, (int)path)); }
int chmod(const char *path, mode_t mode) { return ret(sc2(SYS_chmod, (int)path, (int)mode)); }
int unlink(const char *path) { return ret(sc1(SYS_unlink, (int)path)); }
int rename(const char *from, const char *to) { return ret(sc2(SYS_rename, (int)from, (int)to)); }
int stat(const char *path, struct stat *st) { return ret(sc2(SYS_stat, (int)path, (int)st)); }
int lstat(const char *path, struct stat *st) { return stat(path, st); }
int fstat(int fd, struct stat *st) { return ret(sc2(SYS_fstat, fd, (int)st)); }

int getdents(int fd, struct dirent *buf, unsigned int count)
{
    return ret(sc3(SYS_getdents, fd, (int)buf, (int)count));
}

pid_t getpid(void) { return sc0(SYS_getpid); }
pid_t getppid(void) { return sc0(SYS_getppid); }
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

sighandler_t signal(int sig, sighandler_t handler)
{
    int disp = handler == SIG_IGN ? SIGDISP_IGN : SIGDISP_DFL;
    int r = ret(sc2(SYS_signal, sig, disp));
    if (r < 0)
        return SIG_ERR;
    return r == SIGDISP_IGN ? SIG_IGN : SIG_DFL;
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
    if (msleep(seconds * 1000) < 0) {
        unsigned int elapsed = (uptime_ms() - start) / 1000;
        return elapsed < seconds ? seconds - elapsed : 0;
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
int fsync(int fd) { return ret(sc1(SYS_fsync, fd)); }
int statfs(const char *path, struct statfs *st) { return ret(sc2(SYS_statfs, (int)path, (int)st)); }
int getmounts(char *buf, unsigned int size) { return ret(sc2(SYS_getmounts, (int)buf, (int)size)); }
int blkinfo(int index, struct blkinfo *bi) { return ret(sc2(SYS_blkinfo, index, (int)bi)); }

int isatty(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return 0;
    if (!S_ISCHR(st.st_mode) || MAJOR(st.st_rdev) != TTY_MAJOR) {
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
