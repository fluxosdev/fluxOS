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
#define FLOPPY_MAJOR 2           /* minor = drive number */
#define IDE_MAJOR 3
#define INPUT_MAJOR 13
#define SOUND_MAJOR 14          /* minors: SOUND_MINOR_* */
#define SOUND_MINOR_MIXER   0
#define SOUND_MINOR_DSP     3
#define SOUND_MINOR_OPL     16
#define SOUND_MINOR_SPEAKER 32

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
    uint32_t ncpu;              /* processors running */
    uint32_t cpu_idle[16];      /* ticks each has been idle, since boot */
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

#define BLK_DISK   0
#define BLK_CDROM  1
#define BLK_PART   2
#define BLK_FLOPPY 3

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

/* ---- usbinfo(2): the USB devices plugged in ---- */

struct usbinfo {
    uint16_t vendor, product;
    uint8_t  cls, subclass, protocol;   /* the device's class, or its first interface's */
    uint8_t  speed;                     /* 0 low, 1 full, 2 high, 3 super */
    uint8_t  bus;                       /* which host controller, from 0 */
    uint8_t  addr;                      /* USB address */
    uint8_t  port;                      /* port on its hub */
    uint8_t  depth;                     /* hubs between it and the controller */
    char     hc[8];                     /* "UHCI", "OHCI", "EHCI", "xHCI" */
    char     manufacturer[32];
    char     name[48];
    char     driver[24];                /* the drivers that took it: "hid", "storage", "hub" */
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

/* ---- display adapter (SYS_display) ---- */
#define DISP_INFO    0          /* arg: struct dispinfo * */
#define DISP_MODES   1          /* arg: struct dispmode[DISP_MAX_MODES]; returns the count */
#define DISP_SETMODE 2          /* arg: const struct dispmode *; width 0: the boot mode */
#define DISP_MAP3D   3          /* arg: struct disp3d *; maps the 3D engine */
#define DISP_FLIP    4          /* arg: const struct dispflip *; shows a page (DISPF_FLIP) */
#define DISP_CURSOR  5          /* arg: const struct dispcursor *; the hardware pointer */
#define DISP_CURSORPOS 6        /* arg: const struct disppos *; moves the hardware pointer */
#define DISP_COPYRECTS 7        /* arg: const struct dispcopy *; copies between pages (DISPF_ACCEL) */
#define DISP_MAPBUF  8          /* arg: struct dispbuf *; lends memory to the GPU (DISPF_GART) */
#define DISP_UNMAPBUF 9         /* arg: const struct dispbuf *; takes it back */
#define DISP_BLIT    10         /* arg: const struct dispblit *; copies rectangles (DISPF_ACCEL) */
#define DISP_SUBMIT  11         /* arg: const struct dispsubmit *; runs a command buffer (Radeon 3D) */

#define DISP_MAX_MODES 32

#define DISPF_NATIVE 0x1        /* the driver sets modes itself (DISP_SETMODE) */
#define DISPF_3D     0x2        /* has a 3D engine (DISP_MAP3D) */
#define DISPF_EDID   0x4        /* edid[] holds the monitor's EDID block */
#define DISPF_BOOT   0x8        /* the boot loader's mode is on the screen */
#define DISPF_FLIP   0x10       /* fbmap maps two pages of the screen (DISP_FLIP) */
#define DISPF_CURSOR 0x20       /* has a hardware pointer (DISP_CURSOR) */
#define DISPF_ACCEL  0x40       /* the 3D engine copies between pages (DISP_COPYRECTS) */
#define DISPF_GART   0x80       /* the GPU reaches ordinary memory (DISP_MAPBUF) */

/* A video mode. DISP_MODES lists timings with bpp 0: each works at 16 or
 * 32 bits per pixel if the video memory holds it. */
struct dispmode {
    uint16_t width, height;
    uint16_t bpp;               /* 16 or 32 */
    uint16_t refresh;           /* Hz; 0 if unknown (the boot mode) */
    uint32_t pixclock;          /* kHz; 0 if unknown */
    uint32_t hfreq;             /* line rate in Hz; 0 if unknown */
};

struct dispinfo {
    char     adapter[48];       /* "3dfx Voodoo3" */
    char     driver[32];        /* "voodoo" or "vesa" */
    uint32_t flags;             /* DISPF_* */
    uint32_t pci_id;            /* vendor | device << 16, or 0 */
    uint32_t vram_kb;
    uint32_t max_pixclock;      /* kHz */
    uint32_t screen_bytes;      /* video memory the screen uses, from offset 0 */
    struct dispmode mode;       /* on the screen now */
    struct dispmode boot;       /* what the boot loader set */
    uint8_t  edid[128];
    uint32_t page_offset;       /* DISPF_FLIP: where page 1 starts in what fbmap maps */
};

/* DISP_FLIP: show page 0 or 1 of what fbmap maps, from the next vertical
 * blank. Only the process that has the screen may flip; the call returns
 * once the page is on the screen, so the other one is free to draw on.
 * The screen goes back to page 0 when that process exits. */
struct dispflip {
    uint32_t page;
};

/* DISP_CURSOR: the hardware pointer's picture, 0xAARRGGBB with alpha 0
 * (transparent) or 255. The hot spot is the pixel DISP_CURSORPOS places. */
#define DISP_CURSOR_SIZE 64

struct dispcursor {
    uint32_t show;              /* 0 hides the pointer, the rest is then ignored */
    uint16_t hot_x, hot_y;
    uint32_t image[DISP_CURSOR_SIZE * DISP_CURSOR_SIZE];
};

struct disppos {
    int32_t x, y;               /* the hot spot, in screen pixels */
};

/* DISP_COPYRECTS: the 3D engine copies rectangles of page from_page to the
 * same place on the other page, and the call returns once they are there.
 * Only the process that has the screen may ask. */
#define DISP_MAX_RECTS 32

struct disprect {
    int16_t x, y, w, h;
};

struct dispcopy {
    uint32_t from_page;
    uint32_t count;
    struct disprect rects[DISP_MAX_RECTS];
};

/* DISP_MAPBUF: map size bytes at addr (ordinary memory of the caller,
 * every page present) into the GPU's address space, until DISP_UNMAPBUF
 * or the process exits. Returns handle and the address the GPU sees addr
 * at. A Radeon's buffers keep its 3D engine's rules: surfaces start on a
 * 256 byte boundary and lines are a multiple of 256 bytes. */
struct dispbuf {
    uint32_t addr, size;
    uint32_t handle;
    uint32_t gpu_lo, gpu_hi;
};

/* DISP_BLIT: the 3D engine copies rectangles from one surface to another,
 * converting between 16 and 32 bits per pixel; the call returns once they
 * are there. A page of the screen has the screen's size and depth; the
 * other surfaces are 32 bits per pixel (0xAARRGGBB), offset and pitch in
 * bytes from the start of the 3D client's video memory or of a buffer. */
#define DISP_SURF_PAGE   0      /* handle: page 0 or 1 of the screen */
#define DISP_SURF_ARENA  1      /* the video memory of DISP_MAP3D */
#define DISP_SURF_BUFFER 2      /* handle: from DISP_MAPBUF */

struct dispsurf {
    uint32_t kind, handle;
    uint32_t offset, pitch;
    uint32_t width, height;
};

struct dispblitrect {
    int16_t sx, sy, dx, dy, w, h;
};

struct dispblit {
    struct dispsurf src, dst;
    uint32_t count;
    struct dispblitrect rects[DISP_MAX_RECTS];
};

/* DISP_SUBMIT: the Radeon's command processor runs dwords of PM4 packets
 * at a GPU address in the caller's video memory or buffers, after the
 * driver's default 3D state, and the call returns when the engine is
 * idle again. Only the process that has the 3D engine may submit. */
struct dispsubmit {
    uint32_t gpu_lo, gpu_hi;
    uint32_t dwords;
};

/* DISP_MAP3D: the 3D engine's registers and all of video memory, mapped
 * into the caller (one process at a time). The screen occupies video
 * memory from offset 0 to screen_end. */
struct disp3d {
    uint32_t regs;              /* user address of the 3D register block (3dfx) */
    uint32_t vram;              /* user address of video memory */
    uint32_t vram_size;
    uint32_t screen_end;
    uint32_t pci_id;
    uint32_t gpu_lo, gpu_hi;    /* Radeon: vram as its 3D engine addresses it */
};

/* ---- sound (ioctl requests and records for the sound devices) ---- */

/* /dev/speaker, the PC speaker. A write queues struct spkr_tone records,
 * which play one after another in the background. */
#define SPKR_TONE 0x5301        /* arg: const struct spkr_tone *: queue one tone */
#define SPKR_SET  0x5302        /* arg: a frequency in Hz (by value), 0 for off:
                                   sound it now, until changed; clears the queue */
#define SPKR_STOP 0x5303        /* silence, and forget the queued tones */

struct spkr_tone {
    uint32_t freq;              /* Hz; 0 is a rest */
    uint32_t ms;
};

/* /dev/dsp, digital audio out (Sound Blaster 16), after OSS. Set the format
 * first, then write samples; writes block while the buffer is full. */
#define SNDCTL_DSP_RESET     0x5000     /* stop now, dropping what is queued */
#define SNDCTL_DSP_SYNC      0x5001     /* wait until everything written has played */
#define SNDCTL_DSP_SPEED     0x5002     /* int *: rate in Hz; set to the one used */
#define SNDCTL_DSP_CHANNELS  0x5003     /* int *: 1 or 2; set to the one used */
#define SNDCTL_DSP_SETFMT    0x5004     /* int *: AFMT_*; set to the one used */
#define SNDCTL_DSP_GETOSPACE 0x5005     /* struct audio_buf_info * */
#define SNDCTL_DSP_GETODELAY 0x5006     /* int *: bytes written and not yet heard */
#define SNDCTL_DSP_POST      0x5007     /* start playing what is queued, however little */

#define AFMT_U8     0x08                /* unsigned 8 bit */
#define AFMT_S16_LE 0x10                /* signed 16 bit, little endian */

struct audio_buf_info {
    int32_t fragments;          /* whole fragments that can be written now */
    int32_t fragstotal;
    int32_t fragsize;           /* bytes */
    int32_t bytes;              /* bytes that can be written without blocking */
};

/* /dev/mixer: volumes from 0 to 100. SOUND_INFO also works on /dev/dsp
 * and /dev/opl. */
#define MIXER_READ  0x4D00      /* struct mixer_level *: fills in left and right */
#define MIXER_WRITE 0x4D01      /* const struct mixer_level * */
#define SOUND_INFO  0x4D02      /* struct sound_info * */

enum {
    MIXER_MASTER, MIXER_PCM, MIXER_SYNTH, MIXER_CD, MIXER_LINE, MIXER_MIC, MIXER_SPEAKER,
    MIXER_CHANNELS
};

struct mixer_level {
    uint32_t channel;           /* MIXER_* */
    uint32_t left, right;       /* mono channels read the same on both sides */
};

#define SOUNDF_DSP     0x1      /* a Sound Blaster 16 (/dev/dsp, /dev/mixer) */
#define SOUNDF_OPL2    0x2      /* an FM synthesizer (/dev/opl) ... */
#define SOUNDF_OPL3    0x4      /* ... that is an OPL3 */
#define SOUNDF_SPEAKER 0x8      /* the PC speaker (always) */
#define SOUNDF_HDA     0x10     /* /dev/dsp is an HD Audio codec, not a Sound Blaster */

struct sound_info {
    char     card[32];          /* "Sound Blaster 16" */
    uint16_t dsp_version;       /* major << 8 | minor, e.g. 0x0405 */
    uint16_t base;              /* I/O base, e.g. 0x220 */
    uint8_t  irq, dma8, dma16;  /* dma16 is 0xFF without a 16-bit channel */
    uint8_t  reserved;
    uint16_t opl_base;          /* 0x388 */
    uint16_t reserved2;
    uint32_t flags;             /* SOUNDF_* */
};

/* /dev/opl, the FM synthesizer: a write sends struct opl_write records to
 * its registers in order. Closing it silences every voice. */
#define OPL_RESET 0x4F00        /* silence every voice, back to OPL2 mode */

struct opl_write {
    uint16_t reg;               /* 0x000-0x0FF; 0x100-0x1FF: the OPL3's second bank */
    uint8_t  val;
    uint8_t  reserved;
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
#define KEY_POWER 0xDE          /* the ACPI power button (sent by the kernel) */

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
