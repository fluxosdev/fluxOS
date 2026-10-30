/*
 * winrun on a Linux host, for the tests (make test-win32): the 2 GiB
 * window is reserved at WIN_SEG_BASE and the program's segments are LDT
 * entries with that base. Faults are taken on an alternate stack, since
 * the kernel would otherwise write the signal frame at the program's
 * (segment-relative) stack pointer.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "win32.h"
#include "os.h"

void gate_load_fs(void);

struct user_desc {
    unsigned int entry_number, base_addr, limit;
    unsigned int seg_32bit : 1, contents : 2, read_exec_only : 1, limit_in_pages : 1,
                 seg_not_present : 1, useable : 1;
};

static int set_ldt(int idx, uint32_t base, uint32_t limit_pages, int code)
{
    struct user_desc d;
    memset(&d, 0, sizeof(d));
    d.entry_number = (unsigned)idx;
    d.base_addr = base;
    d.limit = limit_pages;
    d.seg_32bit = 1;
    d.contents = code ? 2 : 0;
    d.limit_in_pages = 1;
    d.useable = 1;
    return (int)syscall(SYS_modify_ldt, 1, &d, sizeof(d));
}

void os_init(void)
{
    void *p = mmap((void *)WIN_SEG_BASE, WIN_SEG_LIMIT, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("winrun: cannot reserve the 2 GiB window");
        exit(1);
    }
    if (set_ldt(0, WIN_SEG_BASE, (WIN_SEG_LIMIT >> 12) - 1, 1) < 0 ||
        set_ldt(1, WIN_SEG_BASE, (WIN_SEG_LIMIT >> 12) - 1, 0) < 0 ||
        set_ldt(2, WIN_TEB_LINEAR, (WIN_TEB_SIZE >> 12) - 1, 0) < 0) {
        perror("winrun: modify_ldt");
        exit(1);
    }
    gate_win_cs = (0 << 3) | 4 | 3;
    gate_win_ds = (1 << 3) | 4 | 3;
    gate_win_fs = (2 << 3) | 4 | 3;
    gate_flat_cs = 0x23;
    gate_flat_ds = 0x2B;
    gate_load_fs();
}

uint32_t os_loader_end(void)
{
    return 0;                           /* the loader is outside the window */
}

int os_map(uint32_t linear, uint32_t size)
{
    void *p = mmap((void *)linear, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? -1 : 0;
}

int os_unmap(uint32_t linear, uint32_t size)
{
    /* keep the window reserved */
    void *p = mmap((void *)linear, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? -1 : 0;
}

uint32_t os_ticks_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

uint64_t os_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

int os_tz_minutes(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return (int)(tm.tm_gmtoff / 60);
}

void os_localtime(time_t t, struct tm *out)
{
    localtime_r(&t, out);
}

int os_console_raw(int fd, int raw)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return -1;
    int was_raw = !(t.c_lflag & ICANON);
    if (raw)
        t.c_lflag &= ~(unsigned)(ICANON | ECHO);
    else
        t.c_lflag |= ICANON | ECHO;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
    return was_raw;
}

int os_readable(int fd, int timeout_ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    return r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0;
}

int os_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
}

static void on_fault(int sig, siginfo_t *info, void *vctx)
{
    ucontext_t *uc = vctx;
    gregset_t *g = &uc->uc_mcontext.gregs;
    uint32_t code = sig == SIGILL ? EXCEPTION_ILLEGAL_INSTRUCTION :
                    sig == SIGFPE ? EXCEPTION_INT_DIVIDE_BY_ZERO :
                    sig == SIGTRAP ? EXCEPTION_BREAKPOINT : EXCEPTION_ACCESS_VIOLATION;
    uint32_t addr = (uint32_t)(uintptr_t)info->si_addr;
    int write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (((*g)[REG_CS] & 0xFFFF) == gate_win_cs) {
        struct guest_regs r = { (uint32_t)(*g)[REG_EDI], (uint32_t)(*g)[REG_ESI], (uint32_t)(*g)[REG_EBP],
                                (uint32_t)(*g)[REG_ESP], (uint32_t)(*g)[REG_EBX], (uint32_t)(*g)[REG_EDX],
                                (uint32_t)(*g)[REG_ECX], (uint32_t)(*g)[REG_EAX], (uint32_t)(*g)[REG_EIP],
                                (uint32_t)(*g)[REG_EFL] };
        seh_fault(code, addr >= WIN_BASE ? addr - WIN_BASE : addr, write, &r, (uint32_t)(*g)[REG_FS] & 0xFFFF);
    }
    fprintf(stderr, "winrun: fault in the loader itself: signal %d at eip %08x, address %08x\n",
            sig, (uint32_t)(*g)[REG_EIP], addr);
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

void os_install_fault_handlers(void)
{
    static uint8_t altstack[65536];
    stack_t ss = { altstack, 0, sizeof(altstack) };
    sigaltstack(&ss, NULL);
    static const int sigs[] = { SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGTRAP };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigaction(sigs[i], &sa, NULL);
    }
}

int os_spawn(const char *path, char *const argv[], char *const envp[], const char *dir, const int fds[3])
{
    int pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (dir && chdir(dir) < 0)
            _exit(127);
        for (int i = 0; i < 3; i++)
            if (fds[i] >= 0 && fds[i] != i)
                dup2(fds[i], i);
        for (int fd = 3; fd < 64; fd++)
            close(fd);
        execve(path, argv, envp);
        _exit(127);
    }
    return pid;
}

int os_wait(int pid, int *exit_code, int block)
{
    int status;
    int r = waitpid(pid, &status, block ? 0 : WNOHANG);
    if (r != pid)
        return 0;
    if (WIFEXITED(status))
        *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        *exit_code = WTERMSIG(status) == SIGINT ? (int)0xC000013Au : (int)0xC0000005u;
    else
        return 0;
    return 1;
}

const char *os_self(void)
{
    static char path[4096];
    if (!path[0]) {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (n <= 0)
            strcpy(path, "winrun");
        else
            path[n] = 0;
    }
    return path;
}

int os_kill(int pid)
{
    return kill(pid, SIGKILL);
}

uint32_t os_mem_total_kb(void)
{
    struct sysinfo si;
    return sysinfo(&si) == 0 ? (uint32_t)((uint64_t)si.totalram * si.mem_unit / 1024) : 262144;
}

uint32_t os_mem_free_kb(void)
{
    struct sysinfo si;
    return sysinfo(&si) == 0 ? (uint32_t)((uint64_t)si.freeram * si.mem_unit / 1024) : 131072;
}

int os_ncpus(void)
{
    int n = get_nprocs();
    return n > 0 ? n : 1;
}

void os_beep(void)
{
    (void)!write(1, "\a", 1);
}

const char *os_hostname(void)
{
    static struct utsname u;
    if (!u.nodename[0] && uname(&u) < 0)
        strcpy(u.nodename, "linux");
    return u.nodename;
}
