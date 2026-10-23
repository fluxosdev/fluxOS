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
#define ENOTBLK      15
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define ENOTTY       25
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define EPIPE        32
#define ERANGE       34
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
#define ENOTSOCK     88
#define EDESTADDRREQ 89
#define EMSGSIZE     90
#define EPROTOTYPE   91
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP   95
#define EAFNOSUPPORT 97
#define EADDRINUSE   98
#define EADDRNOTAVAIL 99
#define ENETDOWN    100
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET  104
#define EISCONN     106
#define ENOTCONN    107
#define ETIMEDOUT   110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113

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
#define S_IFBLK  0x6000
#define S_IFREG  0x8000
#define S_IFSOCK 0xC000
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

struct stat {
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_size;
    uint32_t st_nlink;
    uint32_t st_mtime;
    uint32_t st_rdev;           /* device number for device nodes */
};

#define MAJOR(dev) (((dev) >> 8) & 0xFF)
#define MINOR(dev) ((dev) & 0xFF)
#define MKDEV(ma, mi) (((ma) << 8) | (mi))
#define TTY_MAJOR 4
#define PTY_MAJOR 5             /* minor = 2 * index (+1 for the master) */
#define IDE_MAJOR 3
#define INPUT_MAJOR 13

/* ---- directory entries (getdents) ---- */
#define NAME_MAX 255
#define PATH_MAX 512

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
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

/* ---- filesystems and block devices ---- */
#define ST_RDONLY 0x1

struct statfs {
    uint32_t f_bsize;           /* allocation unit in bytes */
    uint32_t f_blocks;          /* total units */
    uint32_t f_bfree;           /* free units */
    uint32_t f_flags;           /* ST_RDONLY */
    char     f_fstype[16];
    char     f_source[32];
};

#define BLK_DISK  0
#define BLK_CDROM 1
#define BLK_PART  2

struct blkinfo {
    char     name[16];
    char     model[44];
    uint32_t type;              /* BLK_* */
    uint32_t sector_size;
    uint32_t sectors;
    char     fstype[16];        /* mounted filesystem, if any */
    char     mountpoint[128];
    uint32_t start;             /* partitions: first sector on the disk */
};

/* ---- reboot(2) ---- */
#define REBOOT_RESTART  1
#define REBOOT_POWEROFF 2
#define REBOOT_HALT     3

/* ---- framebuffer (SYS_fbinfo, SYS_fbmap) ---- */
struct fbinfo {
    uint32_t width, height;
    uint32_t pitch;             /* bytes per line */
    uint32_t bpp;               /* 15, 16, 24 or 32 */
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
    uint16_t reserved;
};

/* ---- input events, read from /dev/input ---- */
#define IEV_KEY   1
#define IEV_MOUSE 2

#define IEV_PRESSED 0x1         /* key: pressed (else released) */
#define IEV_ABS     0x2         /* mouse: x, y are a position in 0..32767 */

#define KMOD_SHIFT 0x1
#define KMOD_CTRL  0x2
#define KMOD_ALT   0x4

#define MOUSE_LEFT   0x1
#define MOUSE_RIGHT  0x2
#define MOUSE_MIDDLE 0x4

/* Key codes are PS/2 set 1 scancodes; E0-prefixed keys get 0x80 added. */
#define KEY_ESC   0x01
#define KEY_TAB   0x0F
#define KEY_ENTER 0x1C
#define KEY_SPACE 0x39
#define KEY_F2    0x3C
#define KEY_F4    0x3E
#define KEY_F5    0x3F
#define KEY_HOME  0xC7
#define KEY_PGUP  0xC9
#define KEY_END   0xCF
#define KEY_PGDN  0xD1
#define KEY_DELETE 0xD3
#define KEY_UP    0xC8
#define KEY_DOWN  0xD0
#define KEY_LEFT  0xCB
#define KEY_RIGHT 0xCD
#define KEY_LWIN  0xDB
#define KEY_RWIN  0xDC
#define KEY_MENU  0xDD

struct input_event {
    uint8_t  type;              /* IEV_KEY or IEV_MOUSE */
    uint8_t  flags;             /* IEV_PRESSED, IEV_ABS */
    uint8_t  mods;              /* key: KMOD_*; mouse: MOUSE_* buttons held */
    uint8_t  len;               /* key: bytes in text */
    uint16_t code;              /* key: KEY_* code; mouse: wheel steps as an
                                   int16_t, positive away from the user */
    int16_t  x, y;              /* mouse: motion (y grows down) or position */
    char     text[6];           /* key: what a terminal would receive */
};

/* ---- poll(2) ---- */
#define POLLIN   0x01
#define POLLOUT  0x04
#define POLLERR  0x08
#define POLLHUP  0x10
#define POLLNVAL 0x20

struct pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

/* ---- sockets (IPv4 only) ---- */
#define AF_INET 2
#define PF_INET AF_INET

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1              /* SOCK_DGRAM: ICMP echo ("ping") sockets */
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define MSG_PEEK     0x02
#define MSG_DONTWAIT 0x40

#define INADDR_ANY       0x00000000u
#define INADDR_BROADCAST 0xFFFFFFFFu
#define INADDR_LOOPBACK  0x7F000001u

/* Addresses and ports are in network byte order, as in BSD. */
struct in_addr {
    uint32_t s_addr;
};

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;        /* AF_INET */
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t  sin_zero[8];
};

/* ---- the network interface (SYS_netinfo, SYS_netconf) ---- */
#define NETF_UP   0x1               /* a network card was found */
#define NETF_DHCP 0x2               /* configure the address by DHCP */
#define NETF_BOUND 0x4              /* has an address */

/* Addresses in network byte order. */
struct netinfo {
    char     name[8];           /* "ne0" */
    char     driver[32];
    uint8_t  mac[6];
    uint16_t flags;             /* NETF_* */
    uint32_t addr, netmask, gateway, dns;
    uint32_t io_base, irq;
    uint32_t rx_packets, tx_packets, rx_bytes, tx_bytes;
    uint32_t rx_errors, tx_errors, rx_dropped;
    uint32_t lease_secs;        /* DHCP lease time left */
};

/* timer frequency; SYS_uptime reports milliseconds */
#define HZ 100

#endif
