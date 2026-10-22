/*
 * fluxOS kernel <-> userspace ABI.
 * Shared by the kernel and the user C library; keep it free of
 * anything that is not plain data definitions.
 */
#ifndef FLUX_ABI_H
#define FLUX_ABI_H

#include <stdint.h>

/* ---- errno values ---- */
#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EIO           5
#define E2BIG         7
#define ENOEXEC       8
#define EBADF         9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define ENOTTY       25
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EPIPE        32
#define ERANGE       34
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39

/* ---- open(2) flags ---- */
#define O_RDONLY     0x0000
#define O_WRONLY     0x0001
#define O_RDWR       0x0002
#define O_ACCMODE    0x0003
#define O_CREAT      0x0040
#define O_EXCL       0x0080
#define O_TRUNC      0x0200
#define O_APPEND     0x0400
#define O_DIRECTORY  0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* ---- file modes ---- */
#define S_IFMT   0xF000
#define S_IFIFO  0x1000
#define S_IFCHR  0x2000
#define S_IFDIR  0x4000
#define S_IFREG  0x8000
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

struct stat {
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_size;
    uint32_t st_nlink;
    uint32_t st_mtime;
    uint32_t st_rdev;           /* device number for character devices */
};

#define MAJOR(dev) (((dev) >> 8) & 0xFF)
#define MINOR(dev) ((dev) & 0xFF)
#define MKDEV(ma, mi) (((ma) << 8) | (mi))
#define TTY_MAJOR 4

/* ---- directory entries (getdents) ---- */
#define NAME_MAX 59
#define PATH_MAX 256

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    uint32_t d_ino;
    uint8_t  d_type;
    char     d_name[NAME_MAX + 1];
};

/* ---- signals ---- */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define NSIG    32

/* dispositions understood by SYS_signal */
#define SIGDISP_DFL 0
#define SIGDISP_IGN 1

/* ---- waitpid(2) ---- */
#define WNOHANG 1
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0)
#define WTERMSIG(s)    ((s) & 0x7f)

/* ---- terminal line discipline flags (SYS_ttymode) ---- */
#define TTY_ECHO   0x1
#define TTY_ICANON 0x2
#define TTY_ISIG   0x4
#define TTY_COOKED (TTY_ECHO | TTY_ICANON | TTY_ISIG)

/* ---- uname(2) ---- */
struct utsname {
    char sysname[32];
    char nodename[32];
    char release[32];
    char version[64];
    char machine[32];
};

/* ---- process / memory introspection ---- */
#define PSTATE_UNUSED   0
#define PSTATE_RUNNING  1
#define PSTATE_READY    2
#define PSTATE_BLOCKED  3
#define PSTATE_SLEEPING 4
#define PSTATE_ZOMBIE   5
#define PSTATE_EMBRYO   6

struct procinfo {
    int32_t  pid;
    int32_t  ppid;
    int32_t  pgid;
    int32_t  state;
    uint32_t mem_kb;
    uint32_t cpu_ticks;
    uint32_t start_tick;
    char     name[32];
};

struct meminfo {
    uint32_t total_kb;
    uint32_t free_kb;
    uint32_t kernel_kb;
    uint32_t heap_total_kb;
    uint32_t heap_used_kb;
};

/* ---- reboot(2) ---- */
#define REBOOT_RESTART  1
#define REBOOT_POWEROFF 2
#define REBOOT_HALT     3

/* timer frequency; SYS_uptime reports milliseconds */
#define HZ 100

#endif
