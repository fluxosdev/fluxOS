/* winrun on fluxOS: see os.h. */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "win32.h"
#include "os.h"

void gate_load_fs(void);

void os_init(void)
{
    gate_win_cs = SEG_WIN_CODE;
    gate_win_ds = SEG_WIN_DATA;
    gate_win_fs = SEG_WIN_FS;
    gate_flat_cs = 0x18 | 3;
    gate_flat_ds = 0x20 | 3;
    gate_load_fs();
}

uint32_t os_loader_end(void)
{
    return ((uint32_t)sbrk(0) + 0xFFFFu) & ~0xFFFFu;
}

int os_map(uint32_t linear, uint32_t size)
{
    void *p = mmap((void *)linear, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? -1 : 0;
}

int os_unmap(uint32_t linear, uint32_t size)
{
    return munmap((void *)linear, size);
}

uint32_t os_ticks_ms(void)
{
    return uptime_ms();
}

uint64_t os_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

int os_tz_minutes(void)
{
    return tz_get();
}

void os_localtime(time_t t, struct tm *out)
{
    struct tm *tm = localtime(&t);
    if (tm)
        *out = *tm;
    else
        memset(out, 0, sizeof(*out));
}

int os_console_raw(int fd, int raw)
{
    if (!isatty(fd))
        return -1;
    int old = ttymode(-1);
    if (old < 0)
        return -1;
    ttymode(raw ? TTY_ISIG : TTY_COOKED);
    return (old & TTY_ICANON) ? 0 : 1;
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
    struct flux_sigcontext *ctx = vctx;
    uint32_t code = sig == SIGILL ? EXCEPTION_ILLEGAL_INSTRUCTION :
                    sig == SIGFPE ? EXCEPTION_INT_DIVIDE_BY_ZERO :
                    sig == SIGTRAP ? EXCEPTION_BREAKPOINT : EXCEPTION_ACCESS_VIOLATION;
    uint32_t addr = info ? info->si_addr : 0;
    if (ctx->cs == SEG_WIN_CODE) {
        struct guest_regs r = { ctx->edi, ctx->esi, ctx->ebp, ctx->useresp, ctx->ebx, ctx->edx,
                                ctx->ecx, ctx->eax, ctx->eip, ctx->eflags };
        seh_fault(code, addr >= WIN_BASE ? addr - WIN_BASE : addr, 0, &r, ctx->fs);
    }
    wdie("fault in the loader itself: signal %d at eip %08x, address %08x", sig, ctx->eip, addr);
}

void os_install_fault_handlers(void)
{
    static const int sigs[] = { SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGTRAP };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
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
        for (int fd = 3; fd < 32; fd++)
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
    return "/bin/winrun";
}

int os_kill(int pid)
{
    return kill(pid, SIGKILL);
}

uint32_t os_mem_total_kb(void)
{
    struct meminfo mi;
    return meminfo(&mi) == 0 ? mi.total_kb : 65536;
}

uint32_t os_mem_free_kb(void)
{
    struct meminfo mi;
    return meminfo(&mi) == 0 ? mi.free_kb : 32768;
}

int os_ncpus(void)
{
    struct meminfo mi;
    return meminfo(&mi) == 0 && mi.ncpu ? (int)mi.ncpu : 1;
}

void os_beep(void)
{
    (void)!write(1, "\a", 1);
}

const char *os_hostname(void)
{
    static struct utsname u;
    if (!u.nodename[0] && uname(&u) < 0)
        strcpy(u.nodename, "fluxos");
    return u.nodename;
}
