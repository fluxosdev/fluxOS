/*
 * KERNEL32: processes, files, the console, memory, modules, time,
 * strings and code pages, synchronization objects (for the one thread
 * there is), and the rest a program expects from the system.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>
#include "win32.h"
#include "os.h"

#define A(i) (a[i])
#define S(i) gstr(a[i])
#define WS(i) gwstr(a[i])
#define P(i) gp(a[i])
#define SETERR(e) set_last_error(e)
#define FAIL(e) do { set_last_error(e); return 0; } while (0)

/* ---- time helpers ---- */

#define EPOCH_DIFF 11644473600ull       /* seconds from 1601 to 1970 */

static uint64_t unix_to_filetime(uint64_t unix_us)
{
    return (unix_us + EPOCH_DIFF * 1000000ull) * 10;
}

static void put_filetime(FILETIME *ft, uint64_t v)
{
    ft->lo = (uint32_t)v;
    ft->hi = (uint32_t)(v >> 32);
}

static uint64_t get_filetime(const FILETIME *ft)
{
    return (uint64_t)ft->hi << 32 | ft->lo;
}

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = yoe + (int)era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yy + (*m <= 2);
}

static void filetime_to_systemtime(uint64_t ft, SYSTEMTIME *st)
{
    uint64_t secs = ft / 10000000ull;
    int64_t days = (int64_t)(secs / 86400) - (int64_t)(EPOCH_DIFF / 86400);
    uint32_t rem = (uint32_t)(secs % 86400);
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    st->year = (uint16_t)y;
    st->month = (uint16_t)m;
    st->day = (uint16_t)d;
    st->dow = (uint16_t)((days + 4) % 7 + 7) % 7;
    st->hour = (uint16_t)(rem / 3600);
    st->minute = (uint16_t)(rem % 3600 / 60);
    st->second = (uint16_t)(rem % 60);
    st->ms = (uint16_t)(ft / 10000 % 1000);
}

static int systemtime_to_filetime(const SYSTEMTIME *st, uint64_t *ft)
{
    if (st->month < 1 || st->month > 12 || st->day < 1 || st->day > 31 || st->hour > 23 ||
        st->minute > 59 || st->second > 59 || st->ms > 999)
        return 0;
    int64_t days = days_from_civil(st->year, st->month, st->day) + (int64_t)(EPOCH_DIFF / 86400);
    uint64_t secs = (uint64_t)days * 86400 + st->hour * 3600u + st->minute * 60u + st->second;
    *ft = secs * 10000000ull + (uint64_t)st->ms * 10000;
    return 1;
}

void filetime_to_systemtime_ex(uint64_t ft, SYSTEMTIME *st)
{
    filetime_to_systemtime(ft, st);
}

static uint64_t now_filetime(void)
{
    return unix_to_filetime(os_time_us());
}

static uint64_t local_bias_100ns(void)
{
    return (uint64_t)((int64_t)os_tz_minutes() * 60 * 10000000);
}

/* ---- the console ---- */

static int console_raw_mode = -1;       /* what we set (0 cooked, 1 raw), -1 untouched */
static uint32_t console_in_mode = 0x1F7, console_out_mode = 0x3;
static int console_attr = 7;

int console_write(int fd, const char *s, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        int r = (int)write(fd, s + done, n - done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return (int)done;
        }
        done += (uint32_t)r;
    }
    return (int)done;
}

int console_raw(int on)
{
    if (console_raw_mode == on)
        return 0;
    if (os_console_raw(0, on) < 0)
        return -1;
    console_raw_mode = on;
    return 0;
}

int console_read(int fd, char *buf, uint32_t n)
{
    if (!n)
        return 0;
    if (fd == 0 && !(console_in_mode & 2))
        console_raw(1);                 /* no ENABLE_LINE_INPUT: bytes as typed */
    else if (fd == 0)
        console_raw(0);
    int r = (int)read(fd, buf, n);
    if (r < 0)
        return -1;
    /* a line ends in CR LF on Windows */
    if (r > 0 && buf[r - 1] == '\n' && (console_in_mode & 2) && (uint32_t)r < n) {
        buf[r - 1] = '\r';
        buf[r++] = '\n';
    }
    return r;
}

int console_kbhit(void)
{
    console_raw(1);
    return os_readable(0, 0);
}

int console_getch(void)
{
    console_raw(1);
    unsigned char c;
    return read(0, &c, 1) == 1 ? c : -1;
}

/* ---- errors ---- */

DEF(GetLastError) { (void)a; return get_last_error(); }
DEF(SetLastError) { SETERR(A(0)); return 0; }
DEF(RestoreLastError) { SETERR(A(0)); return 0; }
DEF(GetErrorMode) { (void)a; return 0; }
DEF(SetErrorMode) { (void)a; return 0; }
DEF(SetThreadErrorMode) { if (A(1)) *(uint32_t *)P(1) = 0; return 1; }
DEF(Beep) { (void)a; os_beep(); return 1; }
DEF(IsDebuggerPresent) { (void)a; return 0; }
DEF(CheckRemoteDebuggerPresent) { if (A(1)) *(uint32_t *)P(1) = 0; return 1; }
DEF(DebugBreak) { (void)a; wlog("DebugBreak called"); return 0; }
DEF(OutputDebugStringA) { if (api_trace && A(0)) fprintf(stderr, "[debug] %s", S(0)); return 0; }
DEF(OutputDebugStringW) { if (api_trace && A(0)) fprintf(stderr, "[debug] %s", wtmp(A(0))); return 0; }

/* ---- the process and threads ---- */

DEF(GetCurrentProcess) { (void)a; return PSEUDO_PROCESS; }
DEF(GetCurrentThread) { (void)a; return PSEUDO_THREAD; }
DEF(GetCurrentProcessId) { (void)a; return teb->process_id; }
DEF(GetCurrentThreadId) { (void)a; return teb->thread_id; }
DEF(GetProcessId) { (void)a; return teb->process_id; }
DEF(GetThreadId) { (void)a; return teb->thread_id; }
DEF(ExitProcess) { process_exit(A(0)); }
DEF(ExitThread) { process_exit(A(0)); }
DEF(TerminateProcess)
{
    struct hobj *o = handle_get_type(A(0), H_PROCESS);
    if (A(0) == PSEUDO_PROCESS)
        process_exit(A(1));
    if (!o)
        FAIL(ERROR_INVALID_HANDLE);
    os_kill(o->pid);
    return 1;
}
DEF(TerminateThread) { (void)a; return 1; }
DEF(GetExitCodeProcess)
{
    struct hobj *o = handle_get_type(A(0), H_PROCESS);
    if (!o)
        FAIL(ERROR_INVALID_HANDLE);
    if (!o->exited) {
        int code;
        if (os_wait(o->pid, &code, 0)) {
            o->exited = 1;
            o->exit_code = (uint32_t)code;
        }
    }
    *(uint32_t *)P(1) = o->exited ? o->exit_code : STILL_ACTIVE;
    return 1;
}
DEF(GetExitCodeThread) { *(uint32_t *)P(1) = STILL_ACTIVE; return 1; }
DEF(GetProcessVersion) { (void)a; return 0x00050001; }
DEF(GetPriorityClass) { (void)a; return 0x20; }
DEF(SetPriorityClass) { (void)a; return 1; }
DEF(GetThreadPriority) { (void)a; return 0; }
DEF(SetThreadPriority) { (void)a; return 1; }
DEF(SetThreadPriorityBoost) { (void)a; return 1; }
DEF(GetProcessAffinityMask) { if (A(1)) *(uint32_t *)P(1) = 1; if (A(2)) *(uint32_t *)P(2) = 1; return 1; }
DEF(SetProcessAffinityMask) { (void)a; return 1; }
DEF(SetThreadAffinityMask) { (void)a; return 1; }
DEF(SetThreadIdealProcessor) { (void)a; return 0; }
DEF(GetCurrentProcessorNumber) { (void)a; return 0; }
DEF(SwitchToThread) { (void)a; return 0; }
DEF(GetProcessTimes)
{
    uint64_t now = now_filetime();
    uint64_t start = now - (uint64_t)os_ticks_ms() * 10000;
    if (A(1)) put_filetime(P(1), start);
    if (A(2)) put_filetime(P(2), 0);
    if (A(3)) put_filetime(P(3), 0);
    if (A(4)) put_filetime(P(4), (uint64_t)clock() * (10000000 / CLOCKS_PER_SEC));
    return 1;
}
DEF(GetThreadTimes) { return api_GetProcessTimes(a); }
DEF(GetProcessHandleCount) { *(uint32_t *)P(1) = 32; return 1; }
DEF(GetProcessIoCounters) { memset(P(1), 0, 48); return 1; }
DEF(SetProcessWorkingSetSize) { (void)a; return 1; }
DEF(GetProcessWorkingSetSize) { *(uint32_t *)P(1) = 0x100000; *(uint32_t *)P(2) = 0x1000000; return 1; }
DEF(GetProcessShutdownParameters) { *(uint32_t *)P(0) = 0x280; *(uint32_t *)P(1) = 0; return 1; }
DEF(SetProcessShutdownParameters) { (void)a; return 1; }
DEF(SetProcessDEPPolicy) { (void)a; return 1; }
DEF(GetProcessDEPPolicy) { if (A(1)) *(uint32_t *)P(1) = 0; if (A(2)) *(uint32_t *)P(2) = 1; return 1; }
DEF(IsWow64Process) { if (A(1)) *(uint32_t *)P(1) = 0; return 1; }
DEF(GetStartupInfoA)
{
    uint32_t *si = P(0);
    memset(si, 0, 68);
    si[0] = 68;
    si[11] = 1;                         /* STARTF_USESHOWWINDOW */
    *(uint16_t *)&si[12] = (uint16_t)show_cmd;
    si[14] = std_handles[0];
    si[15] = std_handles[1];
    si[16] = std_handles[2];
    return 0;
}
DEF(GetStartupInfoW) { return api_GetStartupInfoA(a); }
DEF(GetCommandLineA) { (void)a; return cmdline_gp; }
DEF(GetCommandLineW) { (void)a; return cmdline_w_gp; }

/* Run another program. The command line is split as Windows does; a
 * Windows program is run through winrun, anything else as it is. */
static int split_cmdline(const char *cmd, char **argv, int max, char *buf, size_t bufsize)
{
    int argc = 0;
    size_t o = 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (argc >= max - 1)
            break;
        argv[argc++] = buf + o;
        int quoted = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                quoted = !quoted;
                p++;
                continue;
            }
            if (*p == '\\') {
                int nb = 0;
                while (p[nb] == '\\')
                    nb++;
                if (p[nb] == '"') {
                    for (int k = 0; k < nb / 2 && o < bufsize - 1; k++)
                        buf[o++] = '\\';
                    if (nb & 1) {
                        if (o < bufsize - 1)
                            buf[o++] = '"';
                        p += nb + 1;
                    } else {
                        p += nb;
                    }
                    continue;
                }
                for (int k = 0; k < nb && o < bufsize - 1; k++)
                    buf[o++] = '\\';
                p += nb;
                continue;
            }
            if (o < bufsize - 1)
                buf[o++] = *p;
            p++;
        }
        buf[o++] = 0;
    }
    argv[argc] = NULL;
    return argc;
}

static int find_executable(const char *name, char *out)
{
    char host[PATH_MAX];
    if (strchr(name, '\\') || (name[0] && name[1] == ':') || strchr(name, '/')) {
        path_to_host(name, host);
        snprintf(out, PATH_MAX, "%s", host);
        if (path_fix_case(out) == 0)
            return 0;
        snprintf(out, PATH_MAX, "%s.exe", host);
        return path_fix_case(out);
    }
    const char *dirs[] = { ".", exe_dir_host, "/windows/system32", "/windows", "/bin" };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(out, PATH_MAX, "%s/%s", dirs[i], name);
        if (path_fix_case(out) == 0)
            return 0;
        snprintf(out, PATH_MAX, "%s/%s.exe", dirs[i], name);
        if (path_fix_case(out) == 0)
            return 0;
    }
    return -1;
}

static int spawn_program(const char *app, const char *cmd, const char *dir, HANDLE hin, HANDLE hout, HANDLE herr,
                         int *pid_out)
{
    char buf[4096];
    char *argv[128];
    int argc = split_cmdline(cmd ? cmd : "", argv, 128, buf, sizeof(buf));
    char exe[PATH_MAX];
    const char *prog = app ? app : argc ? argv[0] : NULL;
    if (!prog || find_executable(prog, exe) < 0)
        return ERROR_FILE_NOT_FOUND;
    char *xargv[132];
    int n = 0;
    uint16_t subsystem;
    int is_dll;
    int is_pe = pe_file_info(exe, &subsystem, &is_dll) == 0;
    if (is_pe) {
        xargv[n++] = (char *)os_self();
        xargv[n++] = exe;
    } else {
        xargv[n++] = exe;
    }
    for (int i = app ? 0 : 1; i < argc && n < 130; i++)
        xargv[n++] = argv[i];
    xargv[n] = NULL;
    char *envp[512];
    int ne = 0;
    for (int i = 0; i < env_count() && ne < 511; i++)
        envp[ne++] = (char *)env_nth(i);
    envp[ne] = NULL;
    int fds[3] = { -1, -1, -1 };
    struct hobj *o;
    if ((o = handle_get(hin)) && o->fd >= 0) fds[0] = o->fd;
    if ((o = handle_get(hout)) && o->fd >= 0) fds[1] = o->fd;
    if ((o = handle_get(herr)) && o->fd >= 0) fds[2] = o->fd;
    char hdir[PATH_MAX];
    if (dir)
        path_to_host(dir, hdir);
    crt_flush_all();
    int pid = os_spawn(xargv[0], xargv, envp, dir ? hdir : NULL, fds);
    if (pid < 0)
        return ERROR_NOT_ENOUGH_MEMORY;
    *pid_out = pid;
    return 0;
}

DEF(CreateProcessA)
{
    const char *app = S(0), *cmd = S(1);
    uint32_t *si = P(8), *pi = P(9);
    HANDLE hin = 0, hout = 0, herr = 0;
    if (si && si[11] & 0x100) {        /* STARTF_USESTDHANDLES */
        hin = si[14]; hout = si[15]; herr = si[16];
    }
    int pid;
    int e = spawn_program(app, cmd, S(7), hin, hout, herr, &pid);
    if (e)
        FAIL((uint32_t)e);
    HANDLE h = handle_new(H_PROCESS);
    struct hobj *o = handle_get(h);
    o->pid = pid;
    HANDLE t = handle_new(H_THREAD);
    if (pi) {
        pi[0] = h;
        pi[1] = t;
        pi[2] = (uint32_t)pid;
        pi[3] = (uint32_t)pid * 4 + 4;
    }
    return 1;
}
DEF(CreateProcessW)
{
    char app[1024], cmd[4096], dir[1024];
    uint32_t b[10];
    memcpy(b, a, sizeof(b));
    if (A(0)) { wide_to_ansi(WS(0), -1, app, sizeof(app)); b[0] = GA(app); }
    if (A(1)) { wide_to_ansi(WS(1), -1, cmd, sizeof(cmd)); b[1] = GA(cmd); }
    if (A(7)) { wide_to_ansi(WS(7), -1, dir, sizeof(dir)); b[7] = GA(dir); }
    /* the A version reads through GP(): make the copies reachable */
    gp_t gapp = A(0) ? gstrdup(app) : 0, gcmd = A(1) ? gstrdup(cmd) : 0, gdir = A(7) ? gstrdup(dir) : 0;
    b[0] = gapp; b[1] = gcmd; b[7] = gdir;
    uint32_t r = api_CreateProcessA(b);
    if (gapp) gfree(gapp);
    if (gcmd) gfree(gcmd);
    if (gdir) gfree(gdir);
    return r;
}
DEF(WinExec)
{
    int pid;
    int e = spawn_program(NULL, S(0), NULL, 0, 0, 0, &pid);
    return e ? 2 : 33;
}
DEF(OpenProcess)
{
    HANDLE h = handle_new(H_PROCESS);
    handle_get(h)->pid = (int)A(2);
    return h;
}
DEF(CreateThread)
{
    (void)a;
    api_unimplemented("CreateThread (threads)");
    FAIL(ERROR_NOT_SUPPORTED);
}
DEF(CreateRemoteThread) { return api_CreateThread(a); }
DEF(ResumeThread) { (void)a; return 0; }
DEF(SuspendThread) { (void)a; return 0; }
DEF(OpenThread) { (void)a; FAIL(ERROR_INVALID_PARAMETER); }
DEF(GetThreadContext) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(SetThreadContext) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(FlushInstructionCache) { (void)a; return 1; }
DEF(GetThreadSelectorEntry) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(GetThreadLocale) { (void)a; return 0x409; }
DEF(SetThreadLocale) { (void)a; return 1; }
DEF(GetThreadUILanguage) { (void)a; return 0x409; }
DEF(SetThreadUILanguage) { (void)a; return 0x409; }
DEF(GetUserDefaultUILanguage) { (void)a; return 0x409; }
DEF(GetSystemDefaultUILanguage) { (void)a; return 0x409; }
DEF(GetUserDefaultLangID) { (void)a; return 0x409; }
DEF(GetSystemDefaultLangID) { (void)a; return 0x409; }
DEF(GetUserDefaultLCID) { (void)a; return 0x409; }
DEF(GetSystemDefaultLCID) { (void)a; return 0x409; }
DEF(ConvertDefaultLocale) { (void)a; return 0x409; }
DEF(IsValidLocale) { (void)a; return 1; }
DEF(IsValidCodePage) { uint32_t cp = A(0); return cp == 437 || cp == 1252 || cp == 65001 || cp == 1200 || cp == 28591; }
DEF(GetACP) { (void)a; return 1252; }
DEF(GetOEMCP) { (void)a; return 437; }
DEF(GetConsoleCP) { (void)a; return 437; }
DEF(GetConsoleOutputCP) { (void)a; return 437; }
DEF(SetConsoleCP) { (void)a; return 1; }
DEF(SetConsoleOutputCP) { (void)a; return 1; }
DEF(GetCPInfo)
{
    uint32_t cp = A(0);
    uint8_t *info = P(1);
    if (!info)
        FAIL(ERROR_INVALID_PARAMETER);
    memset(info, 0, 20);
    *(uint32_t *)info = cp == 65001 ? 4 : 1;   /* MaxCharSize */
    info[4] = '?';
    return 1;
}
DEF(GetCPInfoExA)
{
    uint8_t *info = P(2);
    if (!info)
        FAIL(ERROR_INVALID_PARAMETER);
    memset(info, 0, 20 + 4 + 4 + 2 + 12 + 260);
    *(uint32_t *)info = 1;
    info[4] = '?';
    uint32_t cp = A(0) ? A(0) : 1252;
    memcpy(info + 20, "?\0", 2);
    *(uint32_t *)(info + 22) = cp;
    snprintf((char *)info + 22 + 4 + 2 + 12, 260, "%u (%s)", cp, cp == 437 ? "OEM - United States" : "ANSI - Latin I");
    return 1;
}
DEF(GetCPInfoExW) { (void)a; FAIL(ERROR_INVALID_PARAMETER); }
DEF(IsDBCSLeadByte) { (void)a; return 0; }
DEF(IsDBCSLeadByteEx) { (void)a; return 0; }

/* ---- the environment ---- */

DEF(GetEnvironmentStrings) { (void)a; return env_block_ansi(); }
DEF(GetEnvironmentStringsW) { (void)a; return env_block_wide(); }
DEF(FreeEnvironmentStringsA) { gfree(A(0)); return 1; }
DEF(FreeEnvironmentStringsW) { gfree(A(0)); return 1; }
DEF(GetEnvironmentVariableA)
{
    const char *v = A(0) ? env_get(S(0)) : NULL;
    if (!v)
        FAIL(ERROR_ENVVAR_NOT_FOUND);
    uint32_t n = (uint32_t)strlen(v);
    if (n + 1 > A(2) || !A(1))
        return n + 1;
    strcpy(P(1), v);
    return n;
}
DEF(GetEnvironmentVariableW)
{
    const char *v = A(0) ? env_get(wtmp(A(0))) : NULL;
    if (!v)
        FAIL(ERROR_ENVVAR_NOT_FOUND);
    uint32_t n = (uint32_t)strlen(v);
    if (n + 1 > A(2) || !A(1))
        return n + 1;
    ansi_to_wide(v, -1, P(1), (int)A(2));
    return n;
}
DEF(SetEnvironmentVariableA) { if (!A(0)) FAIL(ERROR_INVALID_PARAMETER); env_set(S(0), S(1)); return 1; }
DEF(SetEnvironmentVariableW)
{
    if (!A(0))
        FAIL(ERROR_INVALID_PARAMETER);
    char name[256];
    wide_to_ansi(WS(0), -1, name, sizeof(name));
    env_set(name, A(1) ? wtmp(A(1)) : NULL);
    return 1;
}
static uint32_t expand_env(const char *src, char *out, uint32_t size)
{
    uint32_t o = 0;
    for (const char *p = src; *p; ) {
        if (*p == '%') {
            const char *e = strchr(p + 1, '%');
            if (e && e > p + 1) {
                char name[256];
                snprintf(name, sizeof(name), "%.*s", (int)(e - p - 1), p + 1);
                const char *v = env_get(name);
                if (v) {
                    for (; *v; v++, o++)
                        if (o < size)
                            out[o] = *v;
                    p = e + 1;
                    continue;
                }
            }
        }
        if (o < size)
            out[o] = *p;
        o++;
        p++;
    }
    if (o < size)
        out[o] = 0;
    else if (size)
        out[size - 1] = 0;
    return o + 1;
}
DEF(ExpandEnvironmentStringsA)
{
    char tmp[4096];
    uint32_t n = expand_env(S(0), tmp, sizeof(tmp));
    if (A(1) && A(2))
        snprintf(P(1), A(2), "%s", tmp);
    return n;
}
DEF(ExpandEnvironmentStringsW)
{
    char tmp[4096];
    uint32_t n = expand_env(wtmp(A(0)), tmp, sizeof(tmp));
    if (A(1) && A(2))
        ansi_to_wide(tmp, -1, P(1), (int)A(2));
    return n;
}
DEF(GetComputerNameA)
{
    const char *h = os_hostname();
    uint32_t *size = P(1);
    uint32_t n = (uint32_t)strlen(h);
    if (*size <= n) { *size = n + 1; FAIL(ERROR_BUFFER_OVERFLOW); }
    strcpy(P(0), h);
    str_upper(P(0));
    *size = n;
    return 1;
}
DEF(GetComputerNameW)
{
    const char *h = os_hostname();
    uint32_t *size = P(1);
    uint32_t n = (uint32_t)strlen(h);
    if (*size <= n) { *size = n + 1; FAIL(ERROR_BUFFER_OVERFLOW); }
    char up[64];
    snprintf(up, sizeof(up), "%s", h);
    str_upper(up);
    ansi_to_wide(up, -1, P(0), (int)*size);
    *size = n;
    return 1;
}
DEF(GetComputerNameExA) { return api_GetComputerNameA(a + 1); }
DEF(GetComputerNameExW) { return api_GetComputerNameW(a + 1); }

/* ---- version and system information ---- */

DEF(GetVersion) { (void)a; return 0x0A280105; }      /* 5.1 build 2600, NT */
DEF(GetVersionExA)
{
    uint32_t *v = P(0);
    if (!v || (v[0] != 148 && v[0] != 156))
        FAIL(ERROR_INSUFFICIENT_BUFFER);
    uint32_t size = v[0];
    memset(v, 0, size);
    v[0] = size;
    v[1] = 5;
    v[2] = 1;
    v[3] = 2600;
    v[4] = 2;
    strcpy((char *)&v[5], "Service Pack 3");
    if (size == 156) {
        uint16_t *ex = (uint16_t *)((uint8_t *)v + 148);
        ex[0] = 3;                      /* wServicePackMajor */
        ex[1] = 0;
        ex[2] = 0x100;                  /* VER_SUITE_SINGLEUSERTS */
        ((uint8_t *)ex)[6] = 1;         /* VER_NT_WORKSTATION */
    }
    return 1;
}
DEF(GetVersionExW)
{
    uint32_t *v = P(0);
    if (!v || (v[0] != 276 && v[0] != 284))
        FAIL(ERROR_INSUFFICIENT_BUFFER);
    uint32_t size = v[0];
    memset(v, 0, size);
    v[0] = size;
    v[1] = 5;
    v[2] = 1;
    v[3] = 2600;
    v[4] = 2;
    ansi_to_wide("Service Pack 3", -1, (uint16_t *)&v[5], 128);
    if (size == 284) {
        uint16_t *ex = (uint16_t *)((uint8_t *)v + 276);
        ex[0] = 3;
        ex[2] = 0x100;
        ((uint8_t *)ex)[6] = 1;
    }
    return 1;
}
DEF(VerSetConditionMask)
{
    /* (mask lo, mask hi, type, condition): 3 bits per type */
    uint64_t mask = (uint64_t)A(1) << 32 | A(0);
    uint32_t type = A(2), cond = A(3) & 7;
    int shift = 0;
    switch (type) {
    case 2: shift = 0; break;           /* VER_MINORVERSION */
    case 1: shift = 3; break;           /* VER_MAJORVERSION */
    case 4: shift = 6; break;           /* VER_BUILDNUMBER */
    case 8: shift = 9; break;           /* VER_PLATFORMID */
    case 0x20: shift = 12; break;       /* VER_SERVICEPACKMINOR */
    case 0x10: shift = 15; break;       /* VER_SERVICEPACKMAJOR */
    case 0x40: shift = 18; break;       /* VER_SUITENAME */
    case 0x80: shift = 21; break;       /* VER_PRODUCT_TYPE */
    }
    mask |= (uint64_t)cond << shift;
    api_edx = (uint32_t)(mask >> 32);
    return (uint32_t)mask;
}
DEF(VerifyVersionInfoA)
{
    uint32_t *v = P(0);
    uint32_t type = A(1);
    uint64_t mask = (uint64_t)A(3) << 32 | A(2);
    uint32_t have[8] = { 1, 5, 2600, 2, 0, 3, 0x100, 1 };   /* minor, major, build, platform, spminor, spmajor, suite, product */
    uint32_t want[8] = { v[2], v[1], v[3], v[4], 0, 0, 0, 0 };
    if (v[0] == 156) {
        uint16_t *ex = (uint16_t *)((uint8_t *)v + 148);
        want[5] = ex[0]; want[4] = ex[1]; want[6] = ex[2]; want[7] = ((uint8_t *)ex)[6];
    }
    static const uint32_t bits[8] = { 2, 1, 4, 8, 0x20, 0x10, 0x40, 0x80 };
    /* major, minor and service pack compare as one number */
    for (int i = 0; i < 8; i++) {
        if (!(type & bits[i]))
            continue;
        uint32_t cond = (mask >> (3 * i)) & 7;
        int ok;
        switch (cond) {
        case 1: ok = have[i] == want[i]; break;
        case 2: ok = have[i] > want[i]; break;
        case 3: ok = have[i] >= want[i]; break;
        case 4: ok = have[i] < want[i]; break;
        case 5: ok = have[i] <= want[i]; break;
        case 6: ok = (have[i] & want[i]) == want[i]; break;
        case 7: ok = (have[i] & want[i]) != 0; break;
        default: ok = 1;
        }
        if (i == 0 && (type & 1)) {     /* minor only decides when major is equal */
            uint32_t mc = (mask >> 3) & 7;
            if (have[1] != want[1] && (mc == 2 || mc == 3 || mc == 4 || mc == 5))
                ok = mc == 2 || mc == 3 ? have[1] > want[1] : have[1] < want[1];
        }
        if (!ok)
            FAIL(1150);                 /* ERROR_OLD_WIN_VERSION */
    }
    return 1;
}
DEF(VerifyVersionInfoW) { return api_VerifyVersionInfoA(a); }
DEF(GetSystemInfo)
{
    uint32_t *si = P(0);
    memset(si, 0, 36);
    *(uint16_t *)&si[0] = 0;            /* PROCESSOR_ARCHITECTURE_INTEL */
    si[1] = 4096;
    si[2] = GUEST_LOW;
    si[3] = GUEST_TOP - 0x10001;
    si[4] = 1;
    si[5] = (uint32_t)os_ncpus();
    si[6] = 586;
    si[7] = 0x10000;
    *(uint16_t *)&si[8] = 15;
    *(uint16_t *)((uint8_t *)si + 34) = 0x0204;
    return 0;
}
DEF(GetNativeSystemInfo) { return api_GetSystemInfo(a); }
DEF(IsProcessorFeaturePresent)
{
    switch (A(0)) {
    case 0: return 0;                   /* PF_FLOATING_POINT_PRECISION_ERRATA */
    case 1: return 0;                   /* PF_FLOATING_POINT_EMULATED */
    case 3: return 1;                   /* MMX */
    case 6: return 1;                   /* XMMI (SSE) */
    case 8: return 1;                   /* RDTSC */
    case 10: return 1;                  /* XMMI64 (SSE2) */
    case 12: return 0;                  /* NX */
    case 13: return 0;                  /* SSE3 */
    case 14: return 1;                  /* CMPXCHG16B? no */
    case 17: return 1;                  /* XSAVE? */
    default: return 0;
    }
}
DEF(GetLogicalProcessorInformation) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(GetLogicalProcessorInformationEx) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(GetNumaHighestNodeNumber) { *(uint32_t *)P(0) = 0; return 1; }
DEF(GetSystemDirectoryA)
{
    const char *d = "C:\\WINDOWS\\SYSTEM32";
    if (A(1) < 20)
        return 20;
    strcpy(P(0), d);
    return 19;
}
DEF(GetSystemDirectoryW) { if (A(1) < 20) return 20; ansi_to_wide("C:\\WINDOWS\\SYSTEM32", -1, P(0), 20); return 19; }
DEF(GetWindowsDirectoryA) { if (A(1) < 11) return 11; strcpy(P(0), "C:\\WINDOWS"); return 10; }
DEF(GetWindowsDirectoryW) { if (A(1) < 11) return 11; ansi_to_wide("C:\\WINDOWS", -1, P(0), 11); return 10; }
DEF(GetSystemWindowsDirectoryA) { return api_GetWindowsDirectoryA(a); }
DEF(GetSystemWindowsDirectoryW) { return api_GetWindowsDirectoryW(a); }
DEF(GetSystemWow64DirectoryA) { (void)a; FAIL(ERROR_CALL_NOT_IMPLEMENTED); }
DEF(GetSystemWow64DirectoryW) { (void)a; FAIL(ERROR_CALL_NOT_IMPLEMENTED); }
DEF(GetTempPathA)
{
    const char *t = "C:\\tmp\\";
    if (A(0) < 8)
        return 8;
    strcpy(P(1), t);
    return 7;
}
DEF(GetTempPathW) { if (A(0) < 8) return 8; ansi_to_wide("C:\\tmp\\", -1, P(1), 8); return 7; }
static uint32_t temp_counter;
DEF(GetTempFileNameA)
{
    const char *dir = S(0), *prefix = S(1) ? S(1) : "tmp";
    uint32_t unique = A(2);
    char *out = P(3);
    if (!dir || !out)
        FAIL(ERROR_INVALID_PARAMETER);
    size_t n = strlen(dir);
    for (int tries = 0; tries < 1000; tries++) {
        if (!unique)
            unique = ++temp_counter + (uint32_t)os_ticks_ms() % 1000;
        snprintf(out, MAX_PATH, "%s%s%.3s%X.tmp", dir, n && dir[n - 1] != '\\' ? "\\" : "", prefix, unique & 0xFFFF);
        if (A(2))
            return unique;
        char host[PATH_MAX];
        path_to_host(out, host);
        int fd = open(host, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) {
            close(fd);
            return unique;
        }
        unique = 0;
    }
    FAIL(ERROR_ACCESS_DENIED);
}
DEF(GetTempFileNameW)
{
    char dir[MAX_PATH], prefix[16], out[MAX_PATH];
    wide_to_ansi(WS(0), -1, dir, sizeof(dir));
    if (A(1)) wide_to_ansi(WS(1), -1, prefix, sizeof(prefix));
    gp_t gd = gstrdup(dir), gpfx = A(1) ? gstrdup(prefix) : 0, go = galloc(MAX_PATH);
    uint32_t b[4] = { gd, gpfx, A(2), go };
    uint32_t r = api_GetTempFileNameA(b);
    if (r) {
        snprintf(out, sizeof(out), "%s", (char *)GP(go));
        ansi_to_wide(out, -1, P(3), MAX_PATH);
    }
    gfree(gd); if (gpfx) gfree(gpfx); gfree(go);
    return r;
}

/* ---- time ---- */

DEF(GetTickCount) { (void)a; return os_ticks_ms(); }
DEF(GetTickCount64) { (void)a; api_edx = 0; return os_ticks_ms(); }
DEF(GetSystemTimeAsFileTime) { put_filetime(P(0), now_filetime()); return 0; }
DEF(GetSystemTimePreciseAsFileTime) { put_filetime(P(0), now_filetime()); return 0; }
DEF(GetSystemTime) { filetime_to_systemtime(now_filetime(), P(0)); return 0; }
DEF(GetLocalTime) { filetime_to_systemtime(now_filetime() + local_bias_100ns(), P(0)); return 0; }
DEF(SetLocalTime) { (void)a; FAIL(ERROR_ACCESS_DENIED); }
DEF(SetSystemTime) { (void)a; FAIL(ERROR_ACCESS_DENIED); }
DEF(FileTimeToSystemTime)
{
    if (!A(0) || !A(1))
        FAIL(ERROR_INVALID_PARAMETER);
    filetime_to_systemtime(get_filetime(P(0)), P(1));
    return 1;
}
DEF(SystemTimeToFileTime)
{
    uint64_t ft;
    if (!A(0) || !A(1) || !systemtime_to_filetime(P(0), &ft))
        FAIL(ERROR_INVALID_PARAMETER);
    put_filetime(P(1), ft);
    return 1;
}
DEF(FileTimeToLocalFileTime) { put_filetime(P(1), get_filetime(P(0)) + local_bias_100ns()); return 1; }
DEF(LocalFileTimeToFileTime) { put_filetime(P(1), get_filetime(P(0)) - local_bias_100ns()); return 1; }
DEF(SystemTimeToTzSpecificLocalTime)
{
    uint64_t ft;
    if (!systemtime_to_filetime(P(1), &ft))
        FAIL(ERROR_INVALID_PARAMETER);
    filetime_to_systemtime(ft + local_bias_100ns(), P(2));
    return 1;
}
DEF(TzSpecificLocalTimeToSystemTime)
{
    uint64_t ft;
    if (!systemtime_to_filetime(P(1), &ft))
        FAIL(ERROR_INVALID_PARAMETER);
    filetime_to_systemtime(ft - local_bias_100ns(), P(2));
    return 1;
}
DEF(CompareFileTime)
{
    uint64_t x = get_filetime(P(0)), y = get_filetime(P(1));
    return x < y ? (uint32_t)-1 : x > y;
}
DEF(FileTimeToDosDateTime)
{
    SYSTEMTIME st;
    filetime_to_systemtime(get_filetime(P(0)), &st);
    if (st.year < 1980)
        FAIL(ERROR_INVALID_PARAMETER);
    *(uint16_t *)P(1) = (uint16_t)(((st.year - 1980) << 9) | (st.month << 5) | st.day);
    *(uint16_t *)P(2) = (uint16_t)((st.hour << 11) | (st.minute << 5) | (st.second / 2));
    return 1;
}
DEF(DosDateTimeToFileTime)
{
    uint32_t d = A(0) & 0xFFFF, t = A(1) & 0xFFFF;
    SYSTEMTIME st = { (uint16_t)(1980 + (d >> 9)), (uint16_t)((d >> 5) & 15), 0, (uint16_t)(d & 31),
                      (uint16_t)(t >> 11), (uint16_t)((t >> 5) & 63), (uint16_t)((t & 31) * 2), 0 };
    uint64_t ft;
    if (!systemtime_to_filetime(&st, &ft))
        FAIL(ERROR_INVALID_PARAMETER);
    put_filetime(P(2), ft);
    return 1;
}
DEF(GetTimeZoneInformation)
{
    uint8_t *tz = P(0);
    memset(tz, 0, 172);
    *(int32_t *)tz = -os_tz_minutes();
    ansi_to_wide("fluxOS Standard Time", -1, (uint16_t *)(tz + 4), 32);
    ansi_to_wide("fluxOS Daylight Time", -1, (uint16_t *)(tz + 88), 32);
    return 0;                           /* TIME_ZONE_ID_UNKNOWN */
}
DEF(SetTimeZoneInformation) { (void)a; FAIL(ERROR_ACCESS_DENIED); }
DEF(GetDynamicTimeZoneInformation) { api_GetTimeZoneInformation(a); return 0; }
DEF(QueryPerformanceCounter)
{
    uint64_t us = os_time_us();
    uint32_t *out = P(0);
    out[0] = (uint32_t)us;
    out[1] = (uint32_t)(us >> 32);
    return 1;
}
DEF(QueryPerformanceFrequency) { uint32_t *out = P(0); out[0] = 1000000; out[1] = 0; return 1; }
DEF(Sleep)
{
    uint32_t ms = A(0);
    crt_flush_all();
    if (gui_connected) {
        gui_wait_ms((int)(ms > 0x7FFFFFFF ? 0x7FFFFFFF : ms));
        return 0;
    }
    if (ms == INFINITE)
        for (;;) usleep(1000000);
    if (ms)
        usleep(ms * 1000);
    return 0;
}
DEF(SleepEx) { api_Sleep(a); return 0; }
DEF(GetSystemTimes)
{
    uint64_t up = (uint64_t)os_ticks_ms() * 10000;
    if (A(0)) put_filetime(P(0), up);
    if (A(1)) put_filetime(P(1), 0);
    if (A(2)) put_filetime(P(2), (uint64_t)clock() * (10000000 / CLOCKS_PER_SEC));
    return 1;
}

/* ---- memory ---- */

DEF(VirtualAlloc)
{
    gp_t addr = A(0);
    uint32_t size = A(1), type = A(2), protect = A(3);
    if (!size)
        FAIL(ERROR_INVALID_PARAMETER);
    if (type & MEM_RESERVE || !addr) {
        gp_t base = addr & ~0xFFFFu;
        uint32_t len = addr ? (addr + size + 0xFFF - base) & ~0xFFFu : (size + 0xFFF) & ~0xFFFu;
        gp_t r = vm_alloc(base, len, protect, MEM_PRIVATE | (type & MEM_TOP_DOWN), addr != 0);
        if (!r)
            FAIL(ERROR_NOT_ENOUGH_MEMORY);
        if (!(type & MEM_COMMIT))
            vm_decommit(r, len);
        return r;
    }
    /* commit within a reservation */
    struct vm_info info;
    if (vm_query(addr, &info) < 0 || info.state == MEM_FREE) {
        gp_t r = vm_alloc(addr & ~0xFFFu, (addr + size + 0xFFF - (addr & ~0xFFFu)) & ~0xFFFu, protect, MEM_PRIVATE, 1);
        if (!r)
            FAIL(ERROR_INVALID_ADDRESS);
        return r;
    }
    if (vm_commit(addr, size, protect) < 0)
        FAIL(ERROR_INVALID_ADDRESS);
    /* committing clears the pages Windows would hand out fresh */
    return addr & ~0xFFFu;
}
DEF(VirtualAllocEx) { return api_VirtualAlloc(a + 1); }
DEF(VirtualFree)
{
    gp_t addr = A(0);
    uint32_t size = A(1), type = A(2);
    if (type == MEM_RELEASE) {
        if (vm_free(addr, size) < 0)
            FAIL(ERROR_INVALID_ADDRESS);
        return 1;
    }
    if (type == MEM_DECOMMIT) {
        if (!size) {
            struct vm_info info;
            if (vm_query(addr, &info) < 0)
                FAIL(ERROR_INVALID_ADDRESS);
            size = info.alloc_base + info.size - addr;
        }
        if (vm_decommit(addr, size) < 0)
            FAIL(ERROR_INVALID_ADDRESS);
        return 1;
    }
    FAIL(ERROR_INVALID_PARAMETER);
}
DEF(VirtualFreeEx) { return api_VirtualFree(a + 1); }
DEF(VirtualProtect)
{
    uint32_t old;
    if (vm_protect(A(0), A(1), A(2), &old) < 0)
        FAIL(ERROR_INVALID_ADDRESS);
    if (A(3))
        *(uint32_t *)P(3) = old;
    return 1;
}
DEF(VirtualProtectEx) { return api_VirtualProtect(a + 1); }
DEF(VirtualQuery)
{
    struct vm_info info;
    if (vm_query(A(0), &info) < 0 || A(2) < 28)
        return 0;
    uint32_t *mbi = P(1);
    mbi[0] = info.base;
    mbi[1] = info.alloc_base;
    mbi[2] = info.alloc_protect;
    mbi[3] = info.size;
    mbi[4] = info.state;
    mbi[5] = info.protect;
    mbi[6] = info.type;
    return 28;
}
DEF(VirtualQueryEx) { return api_VirtualQuery(a + 1); }
DEF(VirtualLock) { (void)a; return 1; }
DEF(VirtualUnlock) { (void)a; return 1; }
DEF(IsBadReadPtr) { return !vm_readable(A(0), A(1)); }
DEF(IsBadWritePtr) { return !vm_writable(A(0), A(1)); }
DEF(IsBadCodePtr) { return !vm_readable(A(0), 1); }
DEF(IsBadHugeReadPtr) { return !vm_readable(A(0), A(1)); }
DEF(IsBadHugeWritePtr) { return !vm_writable(A(0), A(1)); }
DEF(IsBadStringPtrA)
{
    gp_t p = A(0);
    uint32_t max = A(1);
    for (uint32_t i = 0; i < max; i++) {
        if (!vm_readable(p + i, 1))
            return 1;
        if (!*(char *)GP(p + i))
            return 0;
    }
    return 0;
}
DEF(IsBadStringPtrW)
{
    gp_t p = A(0);
    uint32_t max = A(1);
    for (uint32_t i = 0; i < max; i++) {
        if (!vm_readable(p + i * 2, 2))
            return 1;
        if (!*(uint16_t *)GP(p + i * 2))
            return 0;
    }
    return 0;
}
DEF(ReadProcessMemory)
{
    if (!vm_readable(A(1), A(3)))
        FAIL(ERROR_NOACCESS);
    memcpy(P(2), GP(A(1)), A(3));
    if (A(4)) *(uint32_t *)P(4) = A(3);
    return 1;
}
DEF(WriteProcessMemory)
{
    if (!vm_writable(A(1), A(3)))
        FAIL(ERROR_NOACCESS);
    memcpy(GP(A(1)), P(2), A(3));
    if (A(4)) *(uint32_t *)P(4) = A(3);
    return 1;
}
DEF(GlobalMemoryStatus)
{
    uint32_t *ms = P(0);
    uint64_t total = (uint64_t)os_mem_total_kb() * 1024, avail = (uint64_t)os_mem_free_kb() * 1024;
    ms[0] = 32;
    ms[1] = total ? (uint32_t)(100 - avail * 100 / total) : 50;
    ms[2] = total > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)total;
    ms[3] = avail > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)avail;
    ms[4] = ms[2];
    ms[5] = ms[3];
    ms[6] = GUEST_TOP - GUEST_LOW;
    ms[7] = GUEST_HIGH - 0x4000000;
    return 0;
}
DEF(GlobalMemoryStatusEx)
{
    uint32_t *ms = P(0);
    if (ms[0] < 64)
        FAIL(ERROR_INVALID_PARAMETER);
    uint64_t total = (uint64_t)os_mem_total_kb() * 1024, avail = (uint64_t)os_mem_free_kb() * 1024;
    ms[1] = total ? (uint32_t)(100 - avail * 100 / total) : 50;
    uint64_t *q = (uint64_t *)&ms[2];
    q[0] = total; q[1] = avail; q[2] = total; q[3] = avail;
    q[4] = GUEST_TOP - GUEST_LOW; q[5] = GUEST_HIGH - 0x4000000; q[6] = 0;
    return 1;
}
DEF(GetProcessHeap) { (void)a; return process_heap; }
DEF(GetProcessHeaps) { if (A(0) >= 1 && A(1)) *(uint32_t *)P(1) = process_heap; return 1; }
DEF(HeapCreate)
{
    gp_t h = heap_create(A(1), A(2), A(0));
    if (!h)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    return h;
}
DEF(HeapDestroy) { return heap_destroy(A(0)) == 0; }
DEF(HeapAlloc)
{
    gp_t p = heap_alloc(A(0), A(2), (A(1) & 8) != 0);
    if (!p && (A(1) & 4))
        wdie("HeapAlloc: out of memory (%u bytes)", A(2));
    return p;
}
DEF(HeapFree) { return heap_free(A(0), A(2)) == 0; }
DEF(HeapReAlloc) { return heap_realloc(A(0), A(2), A(3), (A(1) & 8) != 0, (A(1) & 0x10) != 0); }
DEF(HeapSize) { return heap_size(A(0), A(2)); }
DEF(HeapValidate) { return heap_validate(A(0), A(2)); }
DEF(HeapCompact) { (void)a; return 0; }
DEF(HeapLock) { (void)a; return 1; }
DEF(HeapUnlock) { (void)a; return 1; }
DEF(HeapSetInformation) { (void)a; return 1; }
DEF(HeapQueryInformation) { (void)a; FAIL(ERROR_INVALID_PARAMETER); }
DEF(HeapWalk) { (void)a; FAIL(ERROR_NO_MORE_ITEMS); }

/* Global and local memory: fixed blocks are the pointer; moveable ones a
 * handle, a small record with the pointer in it, marked by bit 1. */
#define GMEM_MOVEABLE 0x2
#define GMEM_ZEROINIT 0x40
struct moveable { gp_t ptr; uint32_t size; uint16_t lock, flags; uint32_t magic; };
#define MOVEABLE_MAGIC 0x564F4D47u

static struct moveable *moveable_of(gp_t h)
{
    if (!(h & 2) || !vm_readable(h & ~3u, sizeof(struct moveable)))
        return NULL;
    struct moveable *m = GP(h & ~3u);
    return m->magic == MOVEABLE_MAGIC ? m : NULL;
}

DEF(GlobalAlloc)
{
    uint32_t flags = A(0), size = A(1);
    if (flags & GMEM_MOVEABLE) {
        gp_t rec = heap_alloc(process_heap, sizeof(struct moveable), 1);
        if (!rec)
            FAIL(ERROR_NOT_ENOUGH_MEMORY);
        struct moveable *m = GP(rec);
        m->magic = MOVEABLE_MAGIC;
        m->size = size;
        m->flags = (uint16_t)flags;
        if (size) {
            m->ptr = heap_alloc(process_heap, size, (flags & GMEM_ZEROINIT) != 0);
            if (!m->ptr) {
                heap_free(process_heap, rec);
                FAIL(ERROR_NOT_ENOUGH_MEMORY);
            }
        }
        return rec | 2;
    }
    gp_t p = heap_alloc(process_heap, size ? size : 1, (flags & GMEM_ZEROINIT) != 0);
    if (!p)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}
DEF(GlobalFree)
{
    struct moveable *m = moveable_of(A(0));
    if (m) {
        if (m->ptr)
            heap_free(process_heap, m->ptr);
        m->magic = 0;
        heap_free(process_heap, A(0) & ~3u);
        return 0;
    }
    return heap_free(process_heap, A(0)) == 0 ? 0 : A(0);
}
DEF(GlobalLock)
{
    struct moveable *m = moveable_of(A(0));
    if (m) {
        m->lock++;
        return m->ptr;
    }
    return heap_validate(process_heap, A(0)) ? A(0) : 0;
}
DEF(GlobalUnlock)
{
    struct moveable *m = moveable_of(A(0));
    if (m && m->lock) {
        m->lock--;
        return m->lock != 0;
    }
    return 0;
}
DEF(GlobalHandle)
{
    /* the handle whose block is at this address: a moveable one must be searched for */
    return A(0);
}
DEF(GlobalSize)
{
    struct moveable *m = moveable_of(A(0));
    if (m)
        return m->size;
    uint32_t s = heap_size(process_heap, A(0));
    return s == 0xFFFFFFFFu ? 0 : s;
}
DEF(GlobalFlags)
{
    struct moveable *m = moveable_of(A(0));
    if (m)
        return m->lock | (m->ptr ? 0 : 0x4000);
    return heap_validate(process_heap, A(0)) ? 0 : 0x8000;
}
DEF(GlobalReAlloc)
{
    struct moveable *m = moveable_of(A(0));
    uint32_t size = A(1), flags = A(2);
    if (m) {
        gp_t np = size ? heap_realloc(process_heap, m->ptr, size, (flags & GMEM_ZEROINIT) != 0, 0) : 0;
        if (size && !np)
            FAIL(ERROR_NOT_ENOUGH_MEMORY);
        if (!size && m->ptr)
            heap_free(process_heap, m->ptr);
        m->ptr = np;
        m->size = size;
        return A(0);
    }
    gp_t np = heap_realloc(process_heap, A(0), size, (flags & GMEM_ZEROINIT) != 0, (flags & GMEM_MOVEABLE) == 0);
    if (!np)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    return np;
}
DEF(GlobalCompact) { (void)a; return 0x100000; }
DEF(GlobalFix) { (void)a; return 0; }
DEF(GlobalUnfix) { (void)a; return 0; }
DEF(GlobalWire) { return api_GlobalLock(a); }
DEF(GlobalUnWire) { return api_GlobalUnlock(a); }
DEF(LocalAlloc) { return api_GlobalAlloc(a); }
DEF(LocalFree) { return api_GlobalFree(a); }
DEF(LocalLock) { return api_GlobalLock(a); }
DEF(LocalUnlock) { return api_GlobalUnlock(a); }
DEF(LocalSize) { return api_GlobalSize(a); }
DEF(LocalFlags) { return api_GlobalFlags(a); }
DEF(LocalReAlloc) { return api_GlobalReAlloc(a); }
DEF(LocalHandle) { return A(0); }
DEF(LocalCompact) { (void)a; return 0x100000; }

/* ---- interlocked ---- */

DEF(InterlockedIncrement) { uint32_t *p = P(0); return ++*p; }
DEF(InterlockedDecrement) { uint32_t *p = P(0); return --*p; }
DEF(InterlockedExchange) { uint32_t *p = P(0); uint32_t o = *p; *p = A(1); return o; }
DEF(InterlockedExchangeAdd) { uint32_t *p = P(0); uint32_t o = *p; *p += A(1); return o; }
DEF(InterlockedCompareExchange)
{
    uint32_t *p = P(0);
    uint32_t o = *p;
    if (o == A(2))
        *p = A(1);
    return o;
}
DEF(InterlockedCompareExchange64)
{
    uint64_t *p = P(0);
    uint64_t o = *p, x = (uint64_t)A(2) << 32 | A(1), c = (uint64_t)A(4) << 32 | A(3);
    if (o == c)
        *p = x;
    api_edx = (uint32_t)(o >> 32);
    return (uint32_t)o;
}
DEF(InterlockedFlushSList) { uint32_t *h = P(0); uint32_t f = h[0]; h[0] = 0; h[1] = 0; return f; }
DEF(InitializeSListHead) { uint32_t *h = P(0); h[0] = 0; h[1] = 0; return 0; }
DEF(InterlockedPushEntrySList)
{
    uint32_t *h = P(0), *e = P(1);
    e[0] = h[0];
    h[0] = A(1);
    h[1] = (h[1] & 0xFFFF0000u) | ((h[1] + 1) & 0xFFFF);
    return e[0];
}
DEF(InterlockedPopEntrySList)
{
    uint32_t *h = P(0);
    uint32_t e = h[0];
    if (e) {
        h[0] = *(uint32_t *)GP(e);
        h[1] = (h[1] & 0xFFFF0000u) | ((h[1] - 1) & 0xFFFF);
    }
    return e;
}
DEF(QueryDepthSList) { uint32_t *h = P(0); return h[1] & 0xFFFF; }

/* ---- critical sections, events, mutexes, semaphores, waiting ---- */

DEF(InitializeCriticalSection) { uint32_t *cs = P(0); memset(cs, 0, 24); cs[1] = (uint32_t)-1; return 0; }
DEF(InitializeCriticalSectionAndSpinCount) { api_InitializeCriticalSection(a); return 1; }
DEF(InitializeCriticalSectionEx) { api_InitializeCriticalSection(a); return 1; }
DEF(SetCriticalSectionSpinCount) { (void)a; return 0; }
DEF(DeleteCriticalSection) { uint32_t *cs = P(0); memset(cs, 0, 24); return 0; }
DEF(EnterCriticalSection)
{
    uint32_t *cs = P(0);
    cs[1]++;
    cs[2]++;
    cs[3] = teb->thread_id;
    return 0;
}
DEF(TryEnterCriticalSection) { api_EnterCriticalSection(a); return 1; }
DEF(LeaveCriticalSection)
{
    uint32_t *cs = P(0);
    cs[1]--;
    if (cs[2])
        cs[2]--;
    if (!cs[2])
        cs[3] = 0;
    return 0;
}
DEF(InitializeSRWLock) { *(uint32_t *)P(0) = 0; return 0; }
DEF(AcquireSRWLockExclusive) { (void)a; return 0; }
DEF(ReleaseSRWLockExclusive) { (void)a; return 0; }
DEF(AcquireSRWLockShared) { (void)a; return 0; }
DEF(ReleaseSRWLockShared) { (void)a; return 0; }
DEF(TryAcquireSRWLockExclusive) { (void)a; return 1; }
DEF(TryAcquireSRWLockShared) { (void)a; return 1; }
DEF(InitializeConditionVariable) { *(uint32_t *)P(0) = 0; return 0; }
DEF(WakeConditionVariable) { (void)a; return 0; }
DEF(WakeAllConditionVariable) { (void)a; return 0; }
DEF(SleepConditionVariableCS) { (void)a; FAIL(ERROR_SEM_TIMEOUT); }
DEF(SleepConditionVariableSRW) { (void)a; FAIL(ERROR_SEM_TIMEOUT); }
DEF(InitOnceExecuteOnce)
{
    uint32_t *once = P(0);
    if (!(*once & 2)) {
        uint32_t ctx = 0;
        uint32_t args[3] = { A(0), A(2), A(3) };
        (void)ctx;
        call_guest(A(1), 3, args);
        *once |= 2;
    }
    return 1;
}
DEF(InitOnceInitialize) { *(uint32_t *)P(0) = 0; return 0; }
DEF(InitOnceBeginInitialize)
{
    uint32_t *once = P(0);
    uint32_t *pending = P(2);
    if (*once & 2) { *pending = 0; return 1; }
    *pending = 1;
    return 1;
}
DEF(InitOnceComplete) { uint32_t *once = P(0); *once |= 2; return 1; }

static HANDLE new_sync(enum htype t, int manual, int initial, int max, const char *name)
{
    (void)name;
    HANDLE h = handle_new(t);
    struct hobj *o = handle_get(h);
    o->manual = manual;
    o->signaled = initial;
    o->count = initial;
    o->max = max;
    o->path = name ? strdup(name) : NULL;
    return h;
}

static struct hobj *find_named(enum htype t, const char *name, HANDLE *out)
{
    if (!name)
        return NULL;
    for (uint32_t h = 4; h < 4 * 1024; h += 4) {
        struct hobj *o = handle_get(h);
        if (o && o->type == t && o->path && !strcasecmp(o->path, name)) {
            *out = handle_dup(h);
            return o;
        }
    }
    return NULL;
}

DEF(CreateEventA)
{
    const char *name = S(3);
    HANDLE h;
    if (find_named(H_EVENT, name, &h)) {
        SETERR(ERROR_ALREADY_EXISTS);
        return h;
    }
    SETERR(0);
    return new_sync(H_EVENT, (int)A(1), (int)A(2), 0, name);
}
DEF(CreateEventW) { uint32_t b[4] = { A(0), A(1), A(2), 0 }; gp_t n = A(3) ? gstrdup(wtmp(A(3))) : 0; b[3] = n; uint32_t r = api_CreateEventA(b); if (n) gfree(n); return r; }
DEF(CreateEventExA) { uint32_t b[4] = { A(0), (A(2) & 1) != 0, (A(2) & 2) != 0, A(1) }; return api_CreateEventA(b); }
DEF(CreateEventExW) { uint32_t b[4] = { A(0), (A(2) & 1) != 0, (A(2) & 2) != 0, A(1) }; return api_CreateEventW(b); }
DEF(OpenEventA) { HANDLE h; if (find_named(H_EVENT, S(2), &h)) return h; FAIL(ERROR_FILE_NOT_FOUND); }
DEF(OpenEventW) { HANDLE h; if (find_named(H_EVENT, wtmp(A(2)), &h)) return h; FAIL(ERROR_FILE_NOT_FOUND); }
DEF(SetEvent) { struct hobj *o = handle_get_type(A(0), H_EVENT); if (!o) FAIL(ERROR_INVALID_HANDLE); o->signaled = 1; return 1; }
DEF(ResetEvent) { struct hobj *o = handle_get_type(A(0), H_EVENT); if (!o) FAIL(ERROR_INVALID_HANDLE); o->signaled = 0; return 1; }
DEF(PulseEvent) { struct hobj *o = handle_get_type(A(0), H_EVENT); if (!o) FAIL(ERROR_INVALID_HANDLE); o->signaled = 0; return 1; }
DEF(CreateMutexA)
{
    HANDLE h;
    if (find_named(H_MUTEX, S(2), &h)) {
        SETERR(ERROR_ALREADY_EXISTS);
        return h;
    }
    SETERR(0);
    return new_sync(H_MUTEX, 0, 0, 0, S(2));
}
DEF(CreateMutexW) { uint32_t b[3] = { A(0), A(1), 0 }; gp_t n = A(2) ? gstrdup(wtmp(A(2))) : 0; b[2] = n; uint32_t r = api_CreateMutexA(b); if (n) gfree(n); return r; }
DEF(CreateMutexExA) { uint32_t b[3] = { A(0), (A(2) & 1) != 0, A(1) }; return api_CreateMutexA(b); }
DEF(CreateMutexExW) { uint32_t b[3] = { A(0), (A(2) & 1) != 0, A(1) }; return api_CreateMutexW(b); }
DEF(OpenMutexA) { HANDLE h; if (find_named(H_MUTEX, S(2), &h)) return h; FAIL(ERROR_FILE_NOT_FOUND); }
DEF(OpenMutexW) { HANDLE h; if (find_named(H_MUTEX, wtmp(A(2)), &h)) return h; FAIL(ERROR_FILE_NOT_FOUND); }
DEF(ReleaseMutex) { struct hobj *o = handle_get_type(A(0), H_MUTEX); if (!o) FAIL(ERROR_INVALID_HANDLE); if (o->count) o->count--; return 1; }
DEF(CreateSemaphoreA) { return new_sync(H_SEMAPHORE, 0, (int)A(1), (int)A(2), S(3)); }
DEF(CreateSemaphoreW) { return new_sync(H_SEMAPHORE, 0, (int)A(1), (int)A(2), A(3) ? wtmp(A(3)) : NULL); }
DEF(CreateSemaphoreExA) { return new_sync(H_SEMAPHORE, 0, (int)A(1), (int)A(2), S(3)); }
DEF(CreateSemaphoreExW) { return new_sync(H_SEMAPHORE, 0, (int)A(1), (int)A(2), A(3) ? wtmp(A(3)) : NULL); }
DEF(ReleaseSemaphore)
{
    struct hobj *o = handle_get_type(A(0), H_SEMAPHORE);
    if (!o)
        FAIL(ERROR_INVALID_HANDLE);
    if (A(2)) *(uint32_t *)P(2) = (uint32_t)o->count;
    o->count += (int)A(1);
    return 1;
}
DEF(CreateWaitableTimerA) { return new_sync(H_TIMER, (int)A(1), 0, 0, S(2)); }
DEF(CreateWaitableTimerW) { return new_sync(H_TIMER, (int)A(1), 0, 0, NULL); }
DEF(SetWaitableTimer) { struct hobj *o = handle_get_type(A(0), H_TIMER); if (!o) FAIL(ERROR_INVALID_HANDLE); o->signaled = 1; return 1; }
DEF(CancelWaitableTimer) { (void)a; return 1; }

/* With one thread, a wait either passes at once or would last for ever:
 * processes are waited for; an event nobody can set times out. */
static uint32_t wait_one(HANDLE h, uint32_t timeout)
{
    if (h == PSEUDO_PROCESS || h == PSEUDO_THREAD)
        return timeout == INFINITE ? WAIT_TIMEOUT : WAIT_TIMEOUT;
    struct hobj *o = handle_get(h);
    if (!o) {
        SETERR(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    switch (o->type) {
    case H_PROCESS: {
        int code;
        if (o->exited)
            return WAIT_OBJECT_0;
        if (os_wait(o->pid, &code, timeout != 0)) {
            o->exited = 1;
            o->exit_code = (uint32_t)code;
            return WAIT_OBJECT_0;
        }
        return WAIT_TIMEOUT;
    }
    case H_EVENT:
        if (o->signaled) {
            if (!o->manual)
                o->signaled = 0;
            return WAIT_OBJECT_0;
        }
        if (timeout && timeout != INFINITE)
            api_Sleep(&timeout);
        return WAIT_TIMEOUT;
    case H_MUTEX:
        o->count++;
        return WAIT_OBJECT_0;
    case H_SEMAPHORE:
        if (o->count > 0) {
            o->count--;
            return WAIT_OBJECT_0;
        }
        return WAIT_TIMEOUT;
    case H_TIMER:
        return o->signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    case H_THREAD:
        return WAIT_OBJECT_0;
    case H_FILE: case H_PIPE_R: case H_CONSOLE_IN:
        return os_readable(o->fd, timeout == INFINITE ? -1 : (int)timeout) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    default:
        return WAIT_OBJECT_0;
    }
}
DEF(WaitForSingleObject)
{
    crt_flush_all();
    uint32_t r = wait_one(A(0), A(1));
    if (r == WAIT_TIMEOUT && A(1) == INFINITE) {
        struct hobj *o = handle_get(A(0));
        wlog("WaitForSingleObject: waiting for ever on a%s that nothing can signal",
             o && o->type == H_EVENT ? "n event" : " handle");
    }
    return r;
}
DEF(WaitForSingleObjectEx) { return api_WaitForSingleObject(a); }
DEF(WaitForMultipleObjects)
{
    uint32_t n = A(0), timeout = A(3);
    const uint32_t *hs = P(1);
    int all = A(2) != 0;
    crt_flush_all();
    if (!n || n > 64)
        FAIL(ERROR_INVALID_PARAMETER);
    uint32_t last = WAIT_TIMEOUT;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = wait_one(hs[i], all ? timeout : 0);
        if (r == WAIT_FAILED)
            return r;
        if (!all && r == WAIT_OBJECT_0)
            return WAIT_OBJECT_0 + i;
        if (all && r != WAIT_OBJECT_0)
            last = r;
    }
    if (all)
        return last == WAIT_TIMEOUT ? WAIT_TIMEOUT : WAIT_OBJECT_0;
    if (timeout && timeout != INFINITE) {
        api_Sleep(&timeout);
        for (uint32_t i = 0; i < n; i++)
            if (wait_one(hs[i], 0) == WAIT_OBJECT_0)
                return WAIT_OBJECT_0 + i;
    }
    return WAIT_TIMEOUT;
}
DEF(WaitForMultipleObjectsEx) { return api_WaitForMultipleObjects(a); }
DEF(SignalObjectAndWait) { api_SetEvent(a); uint32_t b[2] = { A(1), A(2) }; return api_WaitForSingleObject(b); }
DEF(RegisterWaitForSingleObject) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(UnregisterWait) { (void)a; return 1; }
DEF(QueueUserAPC) { (void)a; return 0; }
DEF(QueueUserWorkItem) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(CreateTimerQueue) { (void)a; return handle_new(H_TIMER); }
DEF(CreateTimerQueueTimer) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(DeleteTimerQueueTimer) { (void)a; return 1; }
DEF(DeleteTimerQueueEx) { (void)a; return 1; }
DEF(DeleteTimerQueue) { (void)a; return 1; }

/* ---- thread local storage ---- */

static uint64_t tls_used;
DEF(TlsAlloc)
{
    (void)a;
    for (int i = 0; i < 64; i++)
        if (!(tls_used & (1ull << i))) {
            tls_used |= 1ull << i;
            teb->tls_slots[i] = 0;
            return (uint32_t)i;
        }
    FAIL(ERROR_NO_MORE_ITEMS);
}
DEF(TlsFree) { if (A(0) >= 64) FAIL(ERROR_INVALID_PARAMETER); tls_used &= ~(1ull << A(0)); return 1; }
DEF(TlsGetValue) { if (A(0) >= 64) FAIL(ERROR_INVALID_PARAMETER); SETERR(0); return teb->tls_slots[A(0)]; }
DEF(TlsSetValue) { if (A(0) >= 64) FAIL(ERROR_INVALID_PARAMETER); teb->tls_slots[A(0)] = A(1); return 1; }
static uint64_t fls_used;
static gp_t fls_values[64], fls_callbacks[64];
DEF(FlsAlloc)
{
    for (int i = 0; i < 64; i++)
        if (!(fls_used & (1ull << i))) {
            fls_used |= 1ull << i;
            fls_values[i] = 0;
            fls_callbacks[i] = A(0);
            return (uint32_t)i;
        }
    FAIL(ERROR_NO_MORE_ITEMS);
}
DEF(FlsFree) { if (A(0) >= 64) FAIL(ERROR_INVALID_PARAMETER); fls_used &= ~(1ull << A(0)); return 1; }
DEF(FlsGetValue) { if (A(0) >= 64) FAIL(ERROR_INVALID_PARAMETER); SETERR(0); return fls_values[A(0)]; }
DEF(FlsSetValue) { if (A(0) >= 64) FAIL(ERROR_INVALID_PARAMETER); fls_values[A(0)] = A(1); return 1; }

/* ---- modules ---- */

DEF(GetModuleHandleA)
{
    if (!A(0))
        return main_module->base;
    struct module *m = module_find(S(0));
    if (!m) {
        struct api_dll *d = api_find_dll(S(0));
        if (d)
            m = module_builtin(d);
    }
    if (!m)
        FAIL(ERROR_MOD_NOT_FOUND);
    return m->base;
}
DEF(GetModuleHandleW)
{
    if (!A(0))
        return main_module->base;
    gp_t n = gstrdup(wtmp(A(0)));
    uint32_t b[1] = { n };
    uint32_t r = api_GetModuleHandleA(b);
    gfree(n);
    return r;
}
DEF(GetModuleHandleExA)
{
    uint32_t flags = A(0);
    uint32_t base;
    if (flags & 4) {                    /* GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS */
        gp_t addr = A(1);
        struct module *m = NULL;
        for (struct module *q = modules; q; q = q->next)
            if (addr >= q->base && addr < q->base + q->size)
                m = q;
        if (!m)
            FAIL(ERROR_MOD_NOT_FOUND);
        base = m->base;
    } else {
        uint32_t b[1] = { A(1) };
        base = api_GetModuleHandleA(b);
        if (!base)
            return 0;
    }
    if (A(2))
        *(uint32_t *)P(2) = base;
    return 1;
}
DEF(GetModuleHandleExW)
{
    if (A(0) & 4)
        return api_GetModuleHandleExA(a);
    gp_t n = A(1) ? gstrdup(wtmp(A(1))) : 0;
    uint32_t b[3] = { A(0), n, A(2) };
    uint32_t r = api_GetModuleHandleExA(b);
    if (n) gfree(n);
    return r;
}
DEF(GetModuleFileNameA)
{
    struct module *m = A(0) ? module_by_base(A(0)) : main_module;
    if (!m)
        FAIL(ERROR_MOD_NOT_FOUND);
    uint32_t size = A(2), n = (uint32_t)strlen(m->winpath);
    if (!size || !A(1))
        FAIL(ERROR_INVALID_PARAMETER);
    if (n >= size) {
        memcpy(P(1), m->winpath, size - 1);
        ((char *)P(1))[size - 1] = 0;
        SETERR(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    strcpy(P(1), m->winpath);
    SETERR(0);
    return n;
}
DEF(GetModuleFileNameW)
{
    struct module *m = A(0) ? module_by_base(A(0)) : main_module;
    if (!m)
        FAIL(ERROR_MOD_NOT_FOUND);
    uint32_t size = A(2), n = (uint32_t)strlen(m->winpath);
    if (!size || !A(1))
        FAIL(ERROR_INVALID_PARAMETER);
    if (n >= size) {
        ansi_to_wide(m->winpath, (int)size - 1, P(1), (int)size);
        ((uint16_t *)P(1))[size - 1] = 0;
        SETERR(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    ansi_to_wide(m->winpath, -1, P(1), (int)size);
    SETERR(0);
    return n;
}
DEF(LoadLibraryA)
{
    if (!A(0))
        FAIL(ERROR_INVALID_PARAMETER);
    char err[256];
    struct module *m = module_load(S(0), exe_dir_host, err, sizeof(err));
    if (!m) {
        wtrace("LoadLibrary(%s): %s", S(0), err);
        FAIL(ERROR_MOD_NOT_FOUND);
    }
    return m->base;
}
DEF(LoadLibraryW) { gp_t n = A(0) ? gstrdup(wtmp(A(0))) : 0; uint32_t b[1] = { n }; uint32_t r = api_LoadLibraryA(b); if (n) gfree(n); return r; }
DEF(LoadLibraryExA) { return api_LoadLibraryA(a); }
DEF(LoadLibraryExW) { return api_LoadLibraryW(a); }
DEF(FreeLibrary) { struct module *m = module_by_base(A(0)); if (!m) FAIL(ERROR_INVALID_HANDLE); module_unload(m); return 1; }
DEF(FreeLibraryAndExitThread) { api_FreeLibrary(a); process_exit(A(1)); }
DEF(DisableThreadLibraryCalls) { (void)a; return 1; }
DEF(GetProcAddress)
{
    struct module *m = module_by_base(A(0));
    if (!m)
        FAIL(ERROR_MOD_NOT_FOUND);
    gp_t r;
    if (A(1) < 0x10000)
        r = m->builtin ? loader_resolve(m->name, NULL, (int)A(1), NULL) : pe_export(m, NULL, (int)A(1));
    else
        r = m->builtin ? loader_resolve(m->name, S(1), 0, NULL) : pe_export(m, S(1), 0);
    if (!r) {
        if (A(1) >= 0x10000)
            wtrace("GetProcAddress(%s, %s): not found", m->name, S(1));
        FAIL(ERROR_PROC_NOT_FOUND);
    }
    return r;
}
DEF(SetDllDirectoryA) { (void)a; return 1; }
DEF(SetDllDirectoryW) { (void)a; return 1; }
DEF(GetDllDirectoryA) { if (A(0)) *(char *)P(1) = 0; return 0; }
DEF(GetDllDirectoryW) { if (A(0)) *(uint16_t *)P(1) = 0; return 0; }
DEF(SetDefaultDllDirectories) { (void)a; return 1; }
DEF(AddDllDirectory) { (void)a; return 1; }
DEF(RemoveDllDirectory) { (void)a; return 1; }
DEF(SetSearchPathMode) { (void)a; return 1; }

/* ---- resources ---- */

static gp_t find_res(gp_t module, gp_t type, gp_t name, int wide, uint32_t *size)
{
    struct module *m = module ? module_by_base(module) : main_module;
    if (!m)
        return 0;
    uint16_t tname[128], nname[128];
    const uint16_t *tn = NULL, *nn = NULL;
    uint32_t tid = type, nid = name;
    if (type >= 0x10000) {
        if (wide) wstrcpy(tname, GP(type)); else ansi_to_wide(gstr(type), -1, tname, 128);
        if (tname[0] == '#') { tid = (uint32_t)atoi(wtmp_n(tname + 1, -1)); } else { tn = tname; tid = 0; }
    }
    if (name >= 0x10000) {
        if (wide) wstrcpy(nname, GP(name)); else ansi_to_wide(gstr(name), -1, nname, 128);
        if (nname[0] == '#') { nid = (uint32_t)atoi(wtmp_n(nname + 1, -1)); } else { nn = nname; nid = 0; }
    }
    return pe_find_resource(m, tid, tn, nid, nn, size);
}
DEF(FindResourceA)
{
    uint32_t size;
    gp_t r = find_res(A(0), A(2), A(1), 0, &size);
    if (!r)
        FAIL(ERROR_RESOURCE_NAME_NOT_FOUND);
    /* the "HRSRC" is the data's address; LoadResource gives it back */
    return r;
}
DEF(FindResourceW) { uint32_t size; gp_t r = find_res(A(0), A(2), A(1), 1, &size); if (!r) FAIL(ERROR_RESOURCE_NAME_NOT_FOUND); return r; }
DEF(FindResourceExA) { uint32_t size; gp_t r = find_res(A(0), A(1), A(2), 0, &size); if (!r) FAIL(ERROR_RESOURCE_NAME_NOT_FOUND); return r; }
DEF(FindResourceExW) { uint32_t size; gp_t r = find_res(A(0), A(1), A(2), 1, &size); if (!r) FAIL(ERROR_RESOURCE_NAME_NOT_FOUND); return r; }
DEF(LoadResource) { return A(1); }
DEF(LockResource) { return A(0); }
DEF(FreeResource) { (void)a; return 0; }
DEF(SizeofResource)
{
    /* the size is kept by the resource walk: look it up again by address */
    struct module *m = A(0) ? module_by_base(A(0)) : main_module;
    gp_t res = A(1);
    if (!m || res < m->base || res >= m->base + m->size)
        return 0;
    /* the data entry precedes nothing: search the directory for this address */
    uint32_t root = m->dir[DIR_RESOURCE][0];
    const uint8_t *img = GP(m->base);
    uint32_t rva = res - m->base;
    /* scan the directory's data entries (the leaves) for one with this RVA */
    uint32_t rsize = m->dir[DIR_RESOURCE][1];
    for (uint32_t off = 0; off + 16 <= rsize; off += 4) {
        uint32_t v, s;
        memcpy(&v, img + root + off, 4);
        memcpy(&s, img + root + off + 4, 4);
        if (v == rva && s && s < m->size)
            return s;
    }
    return 0;
}
DEF(EnumResourceNamesA) { (void)a; FAIL(ERROR_RESOURCE_TYPE_NOT_FOUND); }
DEF(EnumResourceNamesW) { (void)a; FAIL(ERROR_RESOURCE_TYPE_NOT_FOUND); }
DEF(EnumResourceTypesA) { (void)a; FAIL(ERROR_RESOURCE_TYPE_NOT_FOUND); }
DEF(EnumResourceTypesW) { (void)a; FAIL(ERROR_RESOURCE_TYPE_NOT_FOUND); }
DEF(EnumResourceLanguagesA) { (void)a; FAIL(ERROR_RESOURCE_TYPE_NOT_FOUND); }
DEF(EnumResourceLanguagesW) { (void)a; FAIL(ERROR_RESOURCE_TYPE_NOT_FOUND); }
DEF(BeginUpdateResourceA) { (void)a; FAIL(ERROR_ACCESS_DENIED); }
DEF(BeginUpdateResourceW) { (void)a; FAIL(ERROR_ACCESS_DENIED); }

/* ---- handles ---- */

DEF(CloseHandle)
{
    HANDLE h = A(0);
    if (h == PSEUDO_PROCESS || h == PSEUDO_THREAD)
        return 1;
    struct hobj *o = handle_get(h);
    if (!o)
        FAIL(ERROR_INVALID_HANDLE);
    if (o->type == H_MAPPING && o->priv)
        ;                               /* the view keeps its memory */
    handle_close(h);
    return 1;
}
DEF(DuplicateHandle)
{
    HANDLE src = A(1);
    if (src == PSEUDO_PROCESS || src == PSEUDO_THREAD) {
        if (A(3)) *(uint32_t *)P(3) = src == PSEUDO_PROCESS ? handle_new(H_PROCESS) : handle_new(H_THREAD);
        struct hobj *o = handle_get(*(uint32_t *)P(3));
        if (o) o->pid = (int)teb->process_id;
        return 1;
    }
    HANDLE d = handle_dup(src);
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    if (A(3)) *(uint32_t *)P(3) = d;
    if (A(6) & 1)                       /* DUPLICATE_CLOSE_SOURCE */
        handle_close(src);
    return 1;
}
DEF(GetHandleInformation) { if (!handle_get(A(0))) FAIL(ERROR_INVALID_HANDLE); *(uint32_t *)P(1) = 0; return 1; }
DEF(SetHandleInformation) { if (!handle_get(A(0))) FAIL(ERROR_INVALID_HANDLE); return 1; }
DEF(SetHandleCount) { return A(0); }
DEF(GetStdHandle)
{
    switch (A(0)) {
    case 0xFFFFFFF6u: return std_handles[0];
    case 0xFFFFFFF5u: return std_handles[1];
    case 0xFFFFFFF4u: return std_handles[2];
    default: FAIL(ERROR_INVALID_HANDLE);
    }
}
DEF(SetStdHandle)
{
    int i = A(0) == 0xFFFFFFF6u ? 0 : A(0) == 0xFFFFFFF5u ? 1 : A(0) == 0xFFFFFFF4u ? 2 : -1;
    if (i < 0)
        FAIL(ERROR_INVALID_HANDLE);
    std_handles[i] = A(1);
    return 1;
}

/* ---- files ---- */

static uint32_t attrs_of(const struct stat *st, const char *name)
{
    uint32_t attr = 0;
    if (S_ISDIR(st->st_mode))
        attr |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st->st_mode & 0222))
        attr |= FILE_ATTRIBUTE_READONLY;
    if (name && name[0] == '.' && name[1] && strcmp(name, ".."))
        attr |= FILE_ATTRIBUTE_HIDDEN;
    if (!attr)
        attr = FILE_ATTRIBUTE_ARCHIVE;
    return attr;
}

static uint64_t stat_filetime(const struct stat *st)
{
    return unix_to_filetime((uint64_t)st->st_mtime * 1000000ull);
}

/* "CON", "NUL" and other names that are not files. */
static int device_path(const char *win, int *fd, enum htype *type)
{
    char name[16];
    const char *b = path_basename(win);
    snprintf(name, sizeof(name), "%s", b);
    char *colon = strchr(name, ':');
    if (colon)
        *colon = 0;
    str_upper(name);
    if (!strcmp(name, "NUL") || !strcmp(name, "NUL.")) {
        *fd = open("/dev/null", O_RDWR);
        *type = H_FILE;
        return 1;
    }
    if (!strcmp(name, "CON") || !strcmp(name, "CONIN$")) {
        *fd = dup(0);
        *type = H_CONSOLE_IN;
        return 1;
    }
    if (!strcmp(name, "CONOUT$") || !strcmp(name, "CONERR$")) {
        *fd = dup(1);
        *type = H_CONSOLE_OUT;
        return 1;
    }
    return 0;
}

static HANDLE create_file(const char *win, uint32_t access, uint32_t share, uint32_t disposition, uint32_t flags)
{
    (void)share;
    int fd;
    enum htype type;
    if (device_path(win, &fd, &type)) {
        if (fd < 0)
            FAIL(ERROR_FILE_NOT_FOUND);
        HANDLE h = handle_for_fd(fd, type, type != H_FILE);
        if (type == H_CONSOLE_IN && (access & GENERIC_WRITE))
            handle_get(h)->type = H_CONSOLE_OUT;
        return h;
    }
    char host[PATH_MAX];
    if (path_to_host(win, host) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    path_fix_case(host);
    int oflags = 0;
    int rd = (access & (GENERIC_READ | GENERIC_ALL)) != 0, wr = (access & (GENERIC_WRITE | GENERIC_ALL)) != 0;
    oflags = rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: oflags |= O_CREAT; break;
    case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
    default: FAIL(ERROR_INVALID_PARAMETER);
    }
    struct stat st;
    int existed = stat(host, &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        if (!(flags & 0x02000000))      /* FILE_FLAG_BACKUP_SEMANTICS: directory handles */
            FAIL(ERROR_ACCESS_DENIED);
        HANDLE h = handle_new(H_FILE);
        struct hobj *o = handle_get(h);
        o->fd = -1;
        o->path = strdup(host);
        o->access = access;
        return h;
    }
    if (!existed && (disposition == OPEN_EXISTING || disposition == TRUNCATE_EXISTING)) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", host);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            if (stat(dir, &st) < 0)
                FAIL(ERROR_PATH_NOT_FOUND);
        }
        FAIL(ERROR_FILE_NOT_FOUND);
    }
    if (!rd && !wr && (oflags & O_CREAT) == 0 && existed) {
        /* no access asked: just a handle for attributes */
        oflags = O_RDONLY;
    }
    fd = open(host, oflags, 0666);
    if (fd < 0 && errno == EACCES && (oflags & O_ACCMODE) == O_RDWR)
        fd = open(host, (oflags & ~O_ACCMODE) | O_RDONLY, 0666);
    if (fd < 0)
        FAIL(errno_to_win(errno));
    if (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)
        SETERR(existed ? ERROR_ALREADY_EXISTS : 0);
    else
        SETERR(0);
    HANDLE h = handle_for_fd(fd, H_FILE, 0);
    struct hobj *o = handle_get(h);
    o->path = strdup(host);
    o->access = access;
    o->flags = flags;
    o->append = 0;
    return h;
}
DEF(CreateFileA)
{
    if (!A(0))
        FAIL(ERROR_INVALID_PARAMETER);
    return create_file(S(0), A(1), A(2), A(4), A(5));
}
DEF(CreateFileW)
{
    if (!A(0))
        FAIL(ERROR_INVALID_PARAMETER);
    char p[PATH_MAX];
    wide_to_ansi(WS(0), -1, p, sizeof(p));
    return create_file(p, A(1), A(2), A(4), A(5));
}
DEF(OpenFile)
{
    /* OFSTRUCT: cBytes, fFixedDisk, nErrCode(2), Reserved1(2), Reserved2(2), szPathName[128] */
    uint8_t *of = P(1);
    uint32_t style = A(2);
    uint32_t access = (style & 3) == 1 ? GENERIC_WRITE : (style & 3) == 2 ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    uint32_t disp = (style & 0x1000) ? CREATE_ALWAYS : OPEN_EXISTING;
    if (of) {
        memset(of, 0, 136);
        of[0] = 136;
        snprintf((char *)of + 8, 128, "%s", S(0));
    }
    if (style & 0x4000) {               /* OF_EXIST */
        char host[PATH_MAX];
        struct stat st;
        path_to_host(S(0), host);
        path_fix_case(host);
        if (stat(host, &st) < 0) { if (of) *(uint16_t *)(of + 2) = 2; return 0xFFFFFFFFu; }
        return 1;
    }
    if (style & 0x200) {                /* OF_DELETE */
        char host[PATH_MAX];
        path_to_host(S(0), host);
        path_fix_case(host);
        return unlink(host) == 0 ? 1 : 0xFFFFFFFFu;
    }
    HANDLE h = create_file(S(0), access, 0, disp, 0);
    if (!h) {
        if (of) *(uint16_t *)(of + 2) = (uint16_t)get_last_error();
        return 0xFFFFFFFFu;
    }
    return h;
}
DEF(_lopen) { uint32_t b[3] = { A(0), 0, A(1) }; return api_OpenFile(b); }
DEF(_lcreat) { return create_file(S(0), GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, 0) ?: 0xFFFFFFFFu; }
DEF(_lclose) { return handle_close(A(0)) == 0 ? 0 : 0xFFFFFFFFu; }

static int file_fd(HANDLE h, struct hobj **out)
{
    struct hobj *o = handle_get(h);
    if (!o)
        return -1;
    if (out)
        *out = o;
    return o->fd;
}

DEF(ReadFile)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    uint32_t n = A(2);
    uint32_t *done = P(3);
    if (fd < 0 || !A(1))
        FAIL(ERROR_INVALID_HANDLE);
    if (done)
        *done = 0;
    if (o->type == H_CONSOLE_IN || (o->fd == 0 && o->console)) {
        crt_flush_all();
        int r = console_read(fd, P(1), n);
        if (r < 0)
            FAIL(ERROR_READ_FAULT);
        if (done)
            *done = (uint32_t)r;
        return 1;
    }
    if (o->type == H_PIPE_W)
        FAIL(ERROR_ACCESS_DENIED);
    int r = (int)read(fd, P(1), n);
    if (r < 0) {
        if (errno == EPIPE)
            FAIL(ERROR_BROKEN_PIPE);
        FAIL(errno_to_win(errno));
    }
    if (done)
        *done = (uint32_t)r;
    if (o->type == H_PIPE_R && r == 0)
        FAIL(ERROR_BROKEN_PIPE);
    return 1;
}
DEF(ReadFileEx) { uint32_t b[5] = { A(0), A(1), A(2), 0, A(3) }; return api_ReadFile(b); }
DEF(WriteFile)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    uint32_t n = A(2);
    uint32_t *done = P(3);
    if (fd < 0)
        FAIL(ERROR_INVALID_HANDLE);
    if (done)
        *done = 0;
    if (!n)
        return 1;
    if (o->append)
        lseek(fd, 0, SEEK_END);
    int r;
    if (o->console || o->type == H_CONSOLE_OUT) {
        crt_flush_all();
        r = console_write(fd, P(1), n);
    } else {
        r = (int)write(fd, P(1), n);
    }
    if (r < 0)
        FAIL(errno_to_win(errno));
    if (done)
        *done = (uint32_t)r;
    return 1;
}
DEF(WriteFileEx) { uint32_t b[5] = { A(0), A(1), A(2), 0, A(3) }; return api_WriteFile(b); }
DEF(_lread) { uint32_t got = 0; uint32_t b[5] = { A(0), A(1), A(2), GA(&got), 0 }; (void)b; struct hobj *o; int fd = file_fd(A(0), &o); if (fd < 0) return 0xFFFFFFFFu; int r = (int)read(fd, P(1), A(2)); return r < 0 ? 0xFFFFFFFFu : (uint32_t)r; }
DEF(_lwrite) { struct hobj *o; int fd = file_fd(A(0), &o); if (fd < 0) return 0xFFFFFFFFu; int r = (int)write(fd, P(1), A(2)); return r < 0 ? 0xFFFFFFFFu : (uint32_t)r; }
DEF(_hread) { return api__lread(a); }
DEF(_hwrite) { return api__lwrite(a); }
DEF(_llseek)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    if (fd < 0)
        return 0xFFFFFFFFu;
    off_t r = lseek(fd, (int32_t)A(1), (int)A(2));
    return r < 0 ? 0xFFFFFFFFu : (uint32_t)r;
}
DEF(SetFilePointer)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    if (fd < 0)
        FAIL(ERROR_INVALID_HANDLE);
    int64_t dist = (int32_t)A(1);
    if (A(2))
        dist = (int64_t)((uint64_t)*(uint32_t *)P(2) << 32 | A(1));
    int whence = A(3) == 0 ? SEEK_SET : A(3) == 1 ? SEEK_CUR : SEEK_END;
    off_t r = lseek(fd, (off_t)dist, whence);
    if (r < 0) {
        if (A(2)) *(uint32_t *)P(2) = 0xFFFFFFFFu;
        FAIL(errno == EINVAL ? ERROR_NEGATIVE_SEEK : errno_to_win(errno));
    }
    if (A(2))
        *(uint32_t *)P(2) = (uint32_t)((uint64_t)r >> 32);
    SETERR(0);
    return (uint32_t)r;
}
DEF(SetFilePointerEx)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    if (fd < 0)
        FAIL(ERROR_INVALID_HANDLE);
    int64_t dist = (int64_t)((uint64_t)A(2) << 32 | A(1));
    int whence = A(4) == 0 ? SEEK_SET : A(4) == 1 ? SEEK_CUR : SEEK_END;
    off_t r = lseek(fd, (off_t)dist, whence);
    if (r < 0)
        FAIL(errno_to_win(errno));
    if (A(3)) {
        uint32_t *out = P(3);
        out[0] = (uint32_t)r;
        out[1] = 0;
    }
    return 1;
}
DEF(GetFileSize)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0)
        FAIL(ERROR_INVALID_HANDLE);
    if (A(1))
        *(uint32_t *)P(1) = 0;
    SETERR(0);
    return (uint32_t)st.st_size;
}
DEF(GetFileSizeEx)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0)
        FAIL(ERROR_INVALID_HANDLE);
    uint32_t *out = P(1);
    out[0] = (uint32_t)st.st_size;
    out[1] = 0;
    return 1;
}
DEF(GetFileType)
{
    HANDLE h = A(0);
    struct hobj *o = handle_get(h);
    if (!o)
        FAIL(ERROR_INVALID_HANDLE);
    if (o->type == H_PIPE_R || o->type == H_PIPE_W)
        return 3;                       /* FILE_TYPE_PIPE */
    if (o->console || o->type == H_CONSOLE_IN || o->type == H_CONSOLE_OUT)
        return 2;                       /* FILE_TYPE_CHAR */
    struct stat st;
    if (o->fd >= 0 && fstat(o->fd, &st) == 0 && (S_ISCHR(st.st_mode) || S_ISFIFO(st.st_mode)))
        return S_ISFIFO(st.st_mode) ? 3 : 2;
    return 1;                           /* FILE_TYPE_DISK */
}
DEF(FlushFileBuffers) { struct hobj *o; int fd = file_fd(A(0), &o); if (fd < 0) FAIL(ERROR_INVALID_HANDLE); fsync(fd); return 1; }
DEF(SetEndOfFile)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    if (fd < 0)
        FAIL(ERROR_INVALID_HANDLE);
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0 || ftruncate(fd, pos) < 0)
        FAIL(errno_to_win(errno));
    return 1;
}
DEF(LockFile) { (void)a; return 1; }
DEF(UnlockFile) { (void)a; return 1; }
DEF(LockFileEx) { (void)a; return 1; }
DEF(UnlockFileEx) { (void)a; return 1; }
DEF(GetFileTime)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    struct stat st;
    if (!o)
        FAIL(ERROR_INVALID_HANDLE);
    if (fd < 0 ? !o->path || stat(o->path, &st) < 0 : fstat(fd, &st) < 0)
        FAIL(ERROR_INVALID_HANDLE);
    uint64_t t = stat_filetime(&st);
    if (A(1)) put_filetime(P(1), t);
    if (A(2)) put_filetime(P(2), t);
    if (A(3)) put_filetime(P(3), t);
    return 1;
}
DEF(SetFileTime) { (void)a; return 1; }
DEF(GetFileInformationByHandle)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    struct stat st;
    if (!o || (fd < 0 ? !o->path || stat(o->path, &st) < 0 : fstat(fd, &st) < 0))
        FAIL(ERROR_INVALID_HANDLE);
    uint32_t *bhfi = P(1);
    memset(bhfi, 0, 52);
    bhfi[0] = attrs_of(&st, o->path ? path_basename(o->path) : NULL);
    uint64_t t = stat_filetime(&st);
    put_filetime((FILETIME *)&bhfi[1], t);
    put_filetime((FILETIME *)&bhfi[3], t);
    put_filetime((FILETIME *)&bhfi[5], t);
    bhfi[7] = 0x1234;
    bhfi[9] = (uint32_t)st.st_size;
    bhfi[10] = 1;
    bhfi[12] = st.st_ino;
    return 1;
}
DEF(GetFileInformationByHandleEx) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(GetFileAttributesA)
{
    char host[PATH_MAX];
    struct stat st;
    if (!A(0) || path_to_host(S(0), host) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    path_fix_case(host);
    if (stat(host, &st) < 0)
        FAIL(errno_to_win(errno));
    return attrs_of(&st, path_basename(host));
}
DEF(GetFileAttributesW) { gp_t n = A(0) ? gstrdup(wtmp(A(0))) : 0; uint32_t b[1] = { n }; uint32_t r = api_GetFileAttributesA(b); if (n) gfree(n); return r; }
DEF(GetFileAttributesExA)
{
    char host[PATH_MAX];
    struct stat st;
    if (!A(0) || path_to_host(S(0), host) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    path_fix_case(host);
    if (stat(host, &st) < 0)
        FAIL(errno_to_win(errno));
    uint32_t *fad = P(2);
    fad[0] = attrs_of(&st, path_basename(host));
    uint64_t t = stat_filetime(&st);
    put_filetime((FILETIME *)&fad[1], t);
    put_filetime((FILETIME *)&fad[3], t);
    put_filetime((FILETIME *)&fad[5], t);
    fad[7] = 0;
    fad[8] = (uint32_t)st.st_size;
    return 1;
}
DEF(GetFileAttributesExW) { gp_t n = gstrdup(wtmp(A(0))); uint32_t b[3] = { n, A(1), A(2) }; uint32_t r = api_GetFileAttributesExA(b); gfree(n); return r; }
DEF(SetFileAttributesA)
{
    char host[PATH_MAX];
    struct stat st;
    if (!A(0) || path_to_host(S(0), host) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    path_fix_case(host);
    if (stat(host, &st) < 0)
        FAIL(errno_to_win(errno));
    mode_t mode = st.st_mode & 0777;
    if (A(1) & FILE_ATTRIBUTE_READONLY)
        mode &= (mode_t)~0222;
    else
        mode |= 0200;
    chmod(host, mode);
    return 1;
}
DEF(SetFileAttributesW) { gp_t n = gstrdup(wtmp(A(0))); uint32_t b[2] = { n, A(1) }; uint32_t r = api_SetFileAttributesA(b); gfree(n); return r; }
DEF(DeleteFileA)
{
    char host[PATH_MAX];
    if (!A(0) || path_to_host(S(0), host) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    path_fix_case(host);
    if (unlink(host) < 0)
        FAIL(errno_to_win(errno));
    return 1;
}
DEF(DeleteFileW) { gp_t n = gstrdup(wtmp(A(0))); uint32_t b[1] = { n }; uint32_t r = api_DeleteFileA(b); gfree(n); return r; }
DEF(MoveFileExA)
{
    char from[PATH_MAX], to[PATH_MAX];
    if (!A(0) || !A(1) || path_to_host(S(0), from) < 0 || path_to_host(S(1), to) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    path_fix_case(from);
    struct stat st;
    if (!(A(2) & 1) && stat(to, &st) == 0)  /* MOVEFILE_REPLACE_EXISTING */
        FAIL(ERROR_ALREADY_EXISTS);
    if (rename(from, to) < 0)
        FAIL(errno_to_win(errno));
    return 1;
}
DEF(MoveFileA) { uint32_t b[3] = { A(0), A(1), 0 }; return api_MoveFileExA(b); }
DEF(MoveFileW) { gp_t x = gstrdup(wtmp(A(0))), y = gstrdup(wtmp(A(1))); uint32_t b[3] = { x, y, 0 }; uint32_t r = api_MoveFileExA(b); gfree(x); gfree(y); return r; }
DEF(MoveFileExW) { gp_t x = gstrdup(wtmp(A(0))), y = gstrdup(wtmp(A(1))); uint32_t b[3] = { x, y, A(2) }; uint32_t r = api_MoveFileExA(b); gfree(x); gfree(y); return r; }
DEF(CopyFileA)
{
    char from[PATH_MAX], to[PATH_MAX];
    if (!A(0) || !A(1) || path_to_host(S(0), from) < 0 || path_to_host(S(1), to) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    path_fix_case(from);
    struct stat st;
    if (A(2) && stat(to, &st) == 0)
        FAIL(ERROR_FILE_EXISTS);
    int in = open(from, O_RDONLY);
    if (in < 0)
        FAIL(errno_to_win(errno));
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        close(in);
        FAIL(errno_to_win(errno));
    }
    char buf[65536];
    int n;
    while ((n = (int)read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)n) != n) {
            close(in);
            close(out);
            FAIL(ERROR_DISK_FULL);
        }
    close(in);
    close(out);
    return 1;
}
DEF(CopyFileW) { gp_t x = gstrdup(wtmp(A(0))), y = gstrdup(wtmp(A(1))); uint32_t b[3] = { x, y, A(2) }; uint32_t r = api_CopyFileA(b); gfree(x); gfree(y); return r; }
DEF(CopyFileExA) { uint32_t b[3] = { A(0), A(1), (A(5) & 1) }; return api_CopyFileA(b); }
DEF(CopyFileExW) { uint32_t b[3] = { A(0), A(1), (A(5) & 1) }; return api_CopyFileW(b); }
DEF(ReplaceFileA) { uint32_t b[3] = { A(1), A(0), 1 }; return api_MoveFileExA(b); }
DEF(CreateDirectoryA)
{
    char host[PATH_MAX];
    if (!A(0) || path_to_host(S(0), host) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    if (mkdir(host, 0777) < 0)
        FAIL(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win(errno));
    return 1;
}
DEF(CreateDirectoryW) { gp_t n = gstrdup(wtmp(A(0))); uint32_t b[2] = { n, A(1) }; uint32_t r = api_CreateDirectoryA(b); gfree(n); return r; }
DEF(CreateDirectoryExA) { return api_CreateDirectoryA(a + 1); }
DEF(CreateDirectoryExW) { return api_CreateDirectoryW(a + 1); }
DEF(RemoveDirectoryA)
{
    char host[PATH_MAX];
    if (!A(0) || path_to_host(S(0), host) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    path_fix_case(host);
    if (rmdir(host) < 0)
        FAIL(errno_to_win(errno));
    return 1;
}
DEF(RemoveDirectoryW) { gp_t n = gstrdup(wtmp(A(0))); uint32_t b[1] = { n }; uint32_t r = api_RemoveDirectoryA(b); gfree(n); return r; }
DEF(GetCurrentDirectoryA)
{
    char cwd[PATH_MAX], win[MAX_PATH];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "/");
    path_to_win(cwd, win, sizeof(win));
    uint32_t n = (uint32_t)strlen(win);
    if (!A(1) || A(0) <= n)
        return n + 1;
    strcpy(P(1), win);
    return n;
}
DEF(GetCurrentDirectoryW)
{
    char cwd[PATH_MAX], win[MAX_PATH];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "/");
    path_to_win(cwd, win, sizeof(win));
    uint32_t n = (uint32_t)strlen(win);
    if (!A(1) || A(0) <= n)
        return n + 1;
    ansi_to_wide(win, -1, P(1), (int)A(0));
    return n;
}
DEF(SetCurrentDirectoryA)
{
    char host[PATH_MAX];
    if (!A(0) || path_to_host(S(0), host) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    path_fix_case(host);
    if (chdir(host) < 0)
        FAIL(errno_to_win(errno));
    return 1;
}
DEF(SetCurrentDirectoryW) { gp_t n = gstrdup(wtmp(A(0))); uint32_t b[1] = { n }; uint32_t r = api_SetCurrentDirectoryA(b); gfree(n); return r; }
DEF(GetFullPathNameA)
{
    const char *name = S(0);
    if (!name)
        FAIL(ERROR_INVALID_PARAMETER);
    char host[PATH_MAX], win[MAX_PATH];
    path_to_host(name, host);
    path_to_win(host, win, sizeof(win));
    size_t n = strlen(name);
    if (n && (name[n - 1] == '\\' || name[n - 1] == '/') && strlen(win) < sizeof(win) - 1 && win[strlen(win) - 1] != '\\')
        strcat(win, "\\");
    uint32_t len = (uint32_t)strlen(win);
    if (A(1) <= len || !A(2))
        return len + 1;
    strcpy(P(2), win);
    if (A(3)) {
        char *p = P(2), *last = strrchr(p, '\\');
        *(uint32_t *)P(3) = last && last[1] ? GA(last + 1) : 0;
    }
    return len;
}
DEF(GetFullPathNameW)
{
    if (!A(0))
        FAIL(ERROR_INVALID_PARAMETER);
    gp_t n = gstrdup(wtmp(A(0)));
    gp_t out = galloc(MAX_PATH + 1);
    uint32_t fp = 0;
    uint32_t b[4] = { n, MAX_PATH, out, GA(&fp) };
    /* the A version writes the file part pointer into our stack: keep it local */
    uint32_t r = api_GetFullPathNameA(b);
    if (r && r < A(1) && A(2)) {
        ansi_to_wide(GP(out), -1, P(2), (int)A(1));
        if (A(3))
            *(uint32_t *)P(3) = fp ? A(2) + (fp - out) * 2 : 0;
    } else if (r) {
        r++;
    }
    gfree(n);
    gfree(out);
    return r;
}
DEF(GetLongPathNameA)
{
    uint32_t n = (uint32_t)strlen(S(0));
    if (A(2) <= n || !A(1))
        return n + 1;
    strcpy(P(1), S(0));
    return n;
}
DEF(GetLongPathNameW)
{
    uint32_t n = (uint32_t)wstrlen(WS(0));
    if (A(2) <= n || !A(1))
        return n + 1;
    wstrcpy(P(1), WS(0));
    return n;
}
DEF(GetShortPathNameA) { return api_GetLongPathNameA(a); }
DEF(GetShortPathNameW) { return api_GetLongPathNameW(a); }
DEF(SearchPathA)
{
    /* (path, file, ext, buflen, buf, filepart) */
    const char *file = S(1), *ext = S(2);
    char name[MAX_PATH], host[PATH_MAX];
    snprintf(name, sizeof(name), "%s%s", file, ext && !strchr(file, '.') ? ext : "");
    const char *dirs[] = { S(0), ".", exe_dir_host, "/windows/system32", "/windows", "/bin" };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (!dirs[i])
            continue;
        if (i == 0) {
            char d[PATH_MAX];
            path_to_host(dirs[i], d);
            snprintf(host, sizeof(host), "%s/%s", d, name);
        } else if (strchr(name, '\\') || (name[0] && name[1] == ':')) {
            path_to_host(name, host);
        } else {
            snprintf(host, sizeof(host), "%s/%s", dirs[i], name);
        }
        struct stat st;
        if (path_fix_case(host) == 0 && stat(host, &st) == 0 && S_ISREG(st.st_mode)) {
            char win[MAX_PATH];
            path_to_win(host, win, sizeof(win));
            uint32_t n = (uint32_t)strlen(win);
            if (A(3) <= n)
                return n + 1;
            strcpy(P(4), win);
            if (A(5)) {
                char *p = P(4), *last = strrchr(p, '\\');
                *(uint32_t *)P(5) = last ? GA(last + 1) : A(4);
            }
            return n;
        }
    }
    FAIL(ERROR_FILE_NOT_FOUND);
}
DEF(SearchPathW)
{
    gp_t p = A(0) ? gstrdup(wtmp(A(0))) : 0, f = gstrdup(wtmp(A(1))), e = A(2) ? gstrdup(wtmp(A(2))) : 0;
    gp_t out = galloc(MAX_PATH + 1);
    uint32_t b[6] = { p, f, e, MAX_PATH, out, 0 };
    uint32_t r = api_SearchPathA(b);
    if (r && r < A(3) && A(4)) {
        ansi_to_wide(GP(out), -1, P(4), (int)A(3));
        if (A(5)) {
            const char *s = GP(out), *last = strrchr(s, '\\');
            *(uint32_t *)P(5) = last ? A(4) + (uint32_t)(last + 1 - s) * 2 : A(4);
        }
    }
    if (p) gfree(p);
    gfree(f);
    if (e) gfree(e);
    gfree(out);
    return r;
}
DEF(GetDriveTypeA)
{
    const char *p = S(0);
    char l = p ? (char)(p[0] & ~0x20) : 'C';
    if (!p) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) && !strncmp(cwd, "/mnt/", 5))
            l = !strncmp(cwd, "/mnt/fd", 7) ? 'A' : !strncmp(cwd, "/mnt/cdrom", 10) ? 'D' : 'E';
    }
    switch (l) {
    case 'A': case 'B': return 2;       /* DRIVE_REMOVABLE */
    case 'C': return 3;                 /* DRIVE_FIXED */
    case 'D': return 5;                 /* DRIVE_CDROM */
    default: {
        char host[PATH_MAX];
        struct stat st;
        snprintf(host, sizeof(host), "/mnt/%c", (char)(l | 0x20));
        return stat(host, &st) == 0 ? 2 : 1;    /* DRIVE_NO_ROOT_DIR */
    }
    }
}
DEF(GetDriveTypeW) { gp_t n = A(0) ? gstrdup(wtmp(A(0))) : 0; uint32_t b[1] = { n }; uint32_t r = api_GetDriveTypeA(b); if (n) gfree(n); return r; }
DEF(GetLogicalDrives)
{
    (void)a;
    uint32_t mask = 1 << 2;             /* C: */
    const char *mounts[] = { "/mnt/fd0", "/mnt/fd1", NULL, "/mnt/cdrom" };
    struct statfs fs, root;
    statfs("/", &root);
    for (int i = 0; i < 4; i++)
        if (mounts[i] && statfs(mounts[i], &fs) == 0 && strcmp(fs.f_fstype, root.f_fstype) != 0)
            mask |= 1u << i;
    return mask;
}
DEF(GetLogicalDriveStringsA)
{
    uint32_t mask = api_GetLogicalDrives(a);
    char buf[128];
    int n = 0;
    for (int i = 0; i < 26; i++)
        if (mask & (1u << i))
            n += sprintf(buf + n, "%c:\\%c", 'A' + i, 0);
    buf[n] = 0;
    if (A(0) < (uint32_t)n + 1)
        return (uint32_t)n + 1;
    memcpy(P(1), buf, (size_t)n + 1);
    return (uint32_t)n;
}
DEF(GetLogicalDriveStringsW)
{
    uint32_t mask = api_GetLogicalDrives(a);
    uint16_t buf[128];
    int n = 0;
    for (int i = 0; i < 26; i++)
        if (mask & (1u << i)) {
            buf[n++] = (uint16_t)('A' + i); buf[n++] = ':'; buf[n++] = '\\'; buf[n++] = 0;
        }
    buf[n] = 0;
    if (A(0) < (uint32_t)n + 1)
        return (uint32_t)n + 1;
    memcpy(P(1), buf, ((size_t)n + 1) * 2);
    return (uint32_t)n;
}
DEF(GetDiskFreeSpaceA)
{
    char host[PATH_MAX];
    if (A(0)) path_to_host(S(0), host); else strcpy(host, "/");
    struct statfs fs;
    if (statfs(host, &fs) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    uint32_t unit = fs.f_bsize ? fs.f_bsize : 4096;
    if (A(1)) *(uint32_t *)P(1) = unit / 512 ? unit / 512 : 1;
    if (A(2)) *(uint32_t *)P(2) = 512;
    if (A(3)) *(uint32_t *)P(3) = fs.f_bfree;
    if (A(4)) *(uint32_t *)P(4) = fs.f_blocks;
    return 1;
}
DEF(GetDiskFreeSpaceW) { gp_t n = A(0) ? gstrdup(wtmp(A(0))) : 0; uint32_t b[5] = { n, A(1), A(2), A(3), A(4) }; uint32_t r = api_GetDiskFreeSpaceA(b); if (n) gfree(n); return r; }
DEF(GetDiskFreeSpaceExA)
{
    char host[PATH_MAX];
    if (A(0)) path_to_host(S(0), host); else strcpy(host, "/");
    struct statfs fs;
    if (statfs(host, &fs) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    uint64_t unit = fs.f_bsize ? fs.f_bsize : 4096;
    uint64_t freeb = unit * fs.f_bfree, total = unit * fs.f_blocks;
    if (A(1)) memcpy(P(1), &freeb, 8);
    if (A(2)) memcpy(P(2), &total, 8);
    if (A(3)) memcpy(P(3), &freeb, 8);
    return 1;
}
DEF(GetDiskFreeSpaceExW) { gp_t n = A(0) ? gstrdup(wtmp(A(0))) : 0; uint32_t b[4] = { n, A(1), A(2), A(3) }; uint32_t r = api_GetDiskFreeSpaceExA(b); if (n) gfree(n); return r; }
DEF(GetVolumeInformationA)
{
    char host[PATH_MAX];
    if (A(0)) path_to_host(S(0), host); else strcpy(host, "/");
    struct statfs fs;
    if (statfs(host, &fs) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    if (A(1) && A(2)) snprintf(P(1), A(2), "FLUXOS");
    if (A(3)) *(uint32_t *)P(3) = 0x12345678;
    if (A(4)) *(uint32_t *)P(4) = 255;
    if (A(5)) *(uint32_t *)P(5) = 0x2;      /* FS_CASE_IS_PRESERVED */
    if (A(6) && A(7)) snprintf(P(6), A(7), "%s", !strcmp(fs.f_fstype, "fat") ? "FAT32" : "NTFS");
    return 1;
}
DEF(GetVolumeInformationW)
{
    gp_t n = A(0) ? gstrdup(wtmp(A(0))) : 0;
    gp_t vol = galloc(64), fsn = galloc(64);
    uint32_t b[8] = { n, vol, 64, A(3), A(4), A(5), fsn, 64 };
    uint32_t r = api_GetVolumeInformationA(b);
    if (r) {
        if (A(1) && A(2)) ansi_to_wide(GP(vol), -1, P(1), (int)A(2));
        if (A(6) && A(7)) ansi_to_wide(GP(fsn), -1, P(6), (int)A(7));
    }
    if (n) gfree(n);
    gfree(vol);
    gfree(fsn);
    return r;
}
DEF(GetVolumePathNameA) { snprintf(P(1), A(2), "%c:\\", S(0) && S(0)[1] == ':' ? (char)(S(0)[0] & ~0x20) : 'C'); return 1; }
DEF(GetVolumePathNameW) { char d = A(0) && WS(0)[1] == ':' ? (char)(WS(0)[0] & ~0x20) : 'C'; char s[4] = { d, ':', '\\', 0 }; ansi_to_wide(s, -1, P(1), (int)A(2)); return 1; }
DEF(DefineDosDeviceA) { (void)a; return 1; }
DEF(QueryDosDeviceA) { (void)a; FAIL(ERROR_FILE_NOT_FOUND); }
DEF(QueryDosDeviceW) { (void)a; FAIL(ERROR_FILE_NOT_FOUND); }
DEF(SetVolumeLabelA) { (void)a; return 1; }
DEF(GetFileAttributesTransactedA) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(AreFileApisANSI) { (void)a; return 1; }
DEF(SetFileApisToANSI) { (void)a; return 0; }
DEF(SetFileApisToOEM) { (void)a; return 0; }
DEF(DeviceIoControl) { (void)a; FAIL(ERROR_INVALID_FUNCTION); }
DEF(GetOverlappedResult) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(CancelIo) { (void)a; return 1; }
DEF(CreatePipe)
{
    int fds[2];
    if (pipe(fds) < 0)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    *(uint32_t *)P(0) = handle_for_fd(fds[0], H_PIPE_R, 0);
    *(uint32_t *)P(1) = handle_for_fd(fds[1], H_PIPE_W, 0);
    return 1;
}
DEF(PeekNamedPipe)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    if (fd < 0)
        FAIL(ERROR_INVALID_HANDLE);
    int ready = os_readable(fd, 0);
    if (A(3)) *(uint32_t *)P(3) = 0;
    if (A(4)) *(uint32_t *)P(4) = ready ? 1 : 0;
    if (A(5)) *(uint32_t *)P(5) = 0;
    return 1;
}
DEF(CreateNamedPipeA) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(ConnectNamedPipe) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(DisconnectNamedPipe) { (void)a; return 1; }
DEF(WaitNamedPipeA) { (void)a; FAIL(ERROR_FILE_NOT_FOUND); }
DEF(SetNamedPipeHandleState) { (void)a; return 1; }
DEF(TransactNamedPipe) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(CallNamedPipeA) { (void)a; FAIL(ERROR_FILE_NOT_FOUND); }
DEF(CreateMailslotA) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }

/* ---- directory search ---- */

struct find {
    char dir[PATH_MAX];
    char pattern[NAME_MAX + 1];
    DIR *d;
    int wide;
};

static int glob_match(const char *pat, const char *s)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*' || *pat == '?')
                pat++;
            if (!*pat)
                return 1;
            for (; *s; s++)
                if (glob_match(pat, s))
                    return 1;
            return !*pat || !strcmp(pat, ".");
        }
        if (*pat == '?') {
            if (!*s)
                return !strcmp(pat + 1, "") || glob_match(pat + 1, s);
            pat++;
            s++;
            continue;
        }
        if (*pat == '.' && !*s && !strcmp(pat, "."))
            return 1;                   /* "name." matches "name" */
        if (!*s || ((*pat | 0x20) != (*s | 0x20) && !(*pat >= 'a' && *pat <= 'z') && *pat != *s) ||
            ((*pat | 0x20) != (*s | 0x20)))
            return 0;
        pat++;
        s++;
    }
    return !*s;
}

static void fill_find(uint8_t *fd, const char *dir, const char *name, int wide)
{
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    struct stat st;
    uint32_t attr = FILE_ATTRIBUTE_ARCHIVE;
    uint64_t t = now_filetime();
    uint32_t size = 0;
    if (stat(full, &st) == 0) {
        attr = attrs_of(&st, name);
        t = stat_filetime(&st);
        size = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
    }
    memset(fd, 0, wide ? 592 : 320);
    *(uint32_t *)fd = attr;
    put_filetime((FILETIME *)(fd + 4), t);
    put_filetime((FILETIME *)(fd + 12), t);
    put_filetime((FILETIME *)(fd + 20), t);
    *(uint32_t *)(fd + 32) = size;
    if (wide) {
        ansi_to_wide(name, -1, (uint16_t *)(fd + 44), 260);
        ((uint16_t *)(fd + 44))[259] = 0;
    } else {
        snprintf((char *)fd + 44, 260, "%s", name);
        /* a short name: the name itself if it fits 8.3 */
        if (strlen(name) <= 12)
            snprintf((char *)fd + 304, 14, "%s", name);
    }
}

static int find_next(struct find *f, uint8_t *out)
{
    struct dirent *de;
    while ((de = readdir(f->d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            if (glob_match(f->pattern, de->d_name) && strcmp(f->dir, "/")) {
                fill_find(out, f->dir, de->d_name, f->wide);
                return 1;
            }
            continue;
        }
        if (glob_match(f->pattern, de->d_name)) {
            fill_find(out, f->dir, de->d_name, f->wide);
            return 1;
        }
    }
    return 0;
}

static HANDLE find_first(const char *win, uint8_t *out, int wide)
{
    char host[PATH_MAX];
    if (!win || path_to_host(win, host) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    struct find *f = calloc(1, sizeof(*f));
    if (!f)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    char *slash = strrchr(host, '/');
    if (slash) {
        snprintf(f->pattern, sizeof(f->pattern), "%s", slash + 1);
        if (slash == host)
            strcpy(f->dir, "/");
        else {
            *slash = 0;
            snprintf(f->dir, sizeof(f->dir), "%s", host);
        }
    } else {
        snprintf(f->pattern, sizeof(f->pattern), "%s", host);
        strcpy(f->dir, ".");
    }
    if (!f->pattern[0])
        strcpy(f->pattern, "*");
    path_fix_case(f->dir);
    f->wide = wide;
    f->d = opendir(f->dir);
    if (!f->d) {
        free(f);
        FAIL(ERROR_PATH_NOT_FOUND);
    }
    /* "*.*" means everything on Windows */
    if (!strcmp(f->pattern, "*.*"))
        strcpy(f->pattern, "*");
    if (!find_next(f, out)) {
        closedir(f->d);
        free(f);
        FAIL(ERROR_FILE_NOT_FOUND);
    }
    HANDLE h = handle_new(H_FIND);
    struct hobj *o = handle_get(h);
    o->priv = f;
    SETERR(0);
    return h;
}
DEF(FindFirstFileA) { if (!A(1)) FAIL(ERROR_INVALID_PARAMETER); return find_first(S(0), P(1), 0); }
DEF(FindFirstFileW) { if (!A(1)) FAIL(ERROR_INVALID_PARAMETER); return find_first(wtmp(A(0)), P(1), 1); }
DEF(FindFirstFileExA) { if (!A(2)) FAIL(ERROR_INVALID_PARAMETER); return find_first(S(0), P(2), 0); }
DEF(FindFirstFileExW) { if (!A(2)) FAIL(ERROR_INVALID_PARAMETER); return find_first(wtmp(A(0)), P(2), 1); }
DEF(FindNextFileA)
{
    struct hobj *o = handle_get_type(A(0), H_FIND);
    if (!o || !A(1))
        FAIL(ERROR_INVALID_HANDLE);
    if (!find_next(o->priv, P(1)))
        FAIL(ERROR_NO_MORE_FILES);
    return 1;
}
DEF(FindNextFileW) { return api_FindNextFileA(a); }
DEF(FindClose)
{
    struct hobj *o = handle_get_type(A(0), H_FIND);
    if (!o)
        FAIL(ERROR_INVALID_HANDLE);
    struct find *f = o->priv;
    if (f->d)
        closedir(f->d);
    handle_close(A(0));
    return 1;
}
DEF(FindFirstChangeNotificationA) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(FindFirstChangeNotificationW) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(FindNextChangeNotification) { (void)a; return 0; }
DEF(FindCloseChangeNotification) { (void)a; return 1; }
DEF(ReadDirectoryChangesW) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(FindFirstVolumeA) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(FindVolumeClose) { (void)a; return 1; }

/* ---- file mappings ---- */

struct mapping { int fd; uint32_t protect; uint32_t size; gp_t anon; };

DEF(CreateFileMappingA)
{
    HANDLE file = A(0);
    uint32_t protect = A(2), size = A(4);
    struct mapping *m = calloc(1, sizeof(*m));
    if (!m)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    m->protect = protect;
    if (file == INVALID_HANDLE_VALUE || !file) {
        m->fd = -1;
        m->size = size;
        if (!size) {
            free(m);
            FAIL(ERROR_INVALID_PARAMETER);
        }
        m->anon = vm_alloc(0, size, PAGE_READWRITE, MEM_MAPPED, 0);
        if (!m->anon) {
            free(m);
            FAIL(ERROR_NOT_ENOUGH_MEMORY);
        }
    } else {
        struct hobj *o;
        int fd = file_fd(file, &o);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) < 0) {
            free(m);
            FAIL(ERROR_INVALID_HANDLE);
        }
        m->fd = dup(fd);
        m->size = size ? size : (uint32_t)st.st_size;
        if (m->size > (uint32_t)st.st_size && (protect & 0x4))   /* PAGE_READWRITE: grow the file */
            ftruncate(m->fd, m->size);
    }
    HANDLE h = handle_new(H_MAPPING);
    handle_get(h)->priv = m;
    SETERR(0);
    return h;
}
DEF(CreateFileMappingW) { return api_CreateFileMappingA(a); }
DEF(OpenFileMappingA) { (void)a; FAIL(ERROR_FILE_NOT_FOUND); }
DEF(OpenFileMappingW) { (void)a; FAIL(ERROR_FILE_NOT_FOUND); }

struct view { gp_t addr; uint32_t size; int fd; uint32_t offset; int writable; struct view *next; };
static struct view *views;

DEF(MapViewOfFileEx)
{
    struct hobj *o = handle_get_type(A(0), H_MAPPING);
    if (!o)
        FAIL(ERROR_INVALID_HANDLE);
    struct mapping *m = o->priv;
    uint32_t access = A(1), offset = A(3), size = A(4);
    gp_t want = A(5);
    if (m->fd < 0) {
        if (offset >= m->size)
            FAIL(ERROR_INVALID_PARAMETER);
        return m->anon + offset;
    }
    if (!size)
        size = m->size > offset ? m->size - offset : 0;
    if (!size)
        FAIL(ERROR_INVALID_PARAMETER);
    gp_t addr = vm_alloc(want, size, PAGE_READWRITE, MEM_MAPPED, want != 0);
    if (!addr)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    lseek(m->fd, (off_t)offset, SEEK_SET);
    uint32_t got = 0;
    while (got < size) {
        int r = (int)read(m->fd, (char *)GP(addr) + got, size - got);
        if (r <= 0)
            break;
        got += (uint32_t)r;
    }
    struct view *v = calloc(1, sizeof(*v));
    if (!v)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    v->addr = addr;
    v->size = size;
    v->fd = dup(m->fd);
    v->offset = offset;
    v->writable = (access & 0x2) != 0 && (m->protect & 0x4) != 0;   /* FILE_MAP_WRITE and PAGE_READWRITE */
    v->next = views;
    views = v;
    return addr;
}
DEF(MapViewOfFile) { uint32_t b[6] = { A(0), A(1), A(2), A(3), A(4), 0 }; return api_MapViewOfFileEx(b); }
static void view_flush(struct view *v)
{
    if (!v->writable)
        return;
    lseek(v->fd, (off_t)v->offset, SEEK_SET);
    uint32_t done = 0;
    while (done < v->size) {
        int r = (int)write(v->fd, (char *)GP(v->addr) + done, v->size - done);
        if (r <= 0)
            break;
        done += (uint32_t)r;
    }
}
DEF(FlushViewOfFile)
{
    for (struct view *v = views; v; v = v->next)
        if (A(0) >= v->addr && A(0) < v->addr + v->size)
            view_flush(v);
    return 1;
}
DEF(UnmapViewOfFile)
{
    for (struct view **pp = &views; *pp; pp = &(*pp)->next) {
        struct view *v = *pp;
        if (A(0) == v->addr) {
            view_flush(v);
            close(v->fd);
            vm_free(v->addr, 0);
            *pp = v->next;
            free(v);
            return 1;
        }
    }
    /* an anonymous mapping's view: nothing to do */
    return 1;
}

/* ---- the console ---- */

DEF(GetConsoleMode)
{
    struct hobj *o = handle_get(A(0));
    if (!o || !(o->console || o->type == H_CONSOLE_IN || o->type == H_CONSOLE_OUT))
        FAIL(ERROR_INVALID_HANDLE);
    *(uint32_t *)P(1) = o->type == H_CONSOLE_OUT || o->fd == 1 || o->fd == 2 ? console_out_mode : console_in_mode;
    return 1;
}
DEF(SetConsoleMode)
{
    struct hobj *o = handle_get(A(0));
    if (!o || !(o->console || o->type == H_CONSOLE_IN || o->type == H_CONSOLE_OUT))
        FAIL(ERROR_INVALID_HANDLE);
    if (o->type == H_CONSOLE_OUT || o->fd == 1 || o->fd == 2)
        console_out_mode = A(1);
    else
        console_in_mode = A(1);
    return 1;
}
DEF(WriteConsoleA)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    if (fd < 0)
        FAIL(ERROR_INVALID_HANDLE);
    crt_flush_all();
    int r = console_write(fd, P(1), A(2));
    if (A(3)) *(uint32_t *)P(3) = (uint32_t)r;
    return 1;
}
DEF(WriteConsoleW)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    if (fd < 0)
        FAIL(ERROR_INVALID_HANDLE);
    uint32_t n = A(2);
    char *buf = malloc(n * 3 + 1);
    if (!buf)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    int len = wide_to_utf8(P(1), (int)n, buf, (int)n * 3);
    crt_flush_all();
    console_write(fd, buf, (uint32_t)len);
    free(buf);
    if (A(3)) *(uint32_t *)P(3) = n;
    return 1;
}
DEF(ReadConsoleA)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    if (fd < 0)
        FAIL(ERROR_INVALID_HANDLE);
    crt_flush_all();
    int r = console_read(fd, P(1), A(2));
    if (r < 0)
        FAIL(ERROR_READ_FAULT);
    if (A(3)) *(uint32_t *)P(3) = (uint32_t)r;
    return 1;
}
DEF(ReadConsoleW)
{
    struct hobj *o;
    int fd = file_fd(A(0), &o);
    if (fd < 0)
        FAIL(ERROR_INVALID_HANDLE);
    char buf[4096];
    uint32_t n = A(2) < sizeof(buf) ? A(2) : sizeof(buf);
    crt_flush_all();
    int r = console_read(fd, buf, n);
    if (r < 0)
        FAIL(ERROR_READ_FAULT);
    int w = utf8_to_wide(buf, r, P(1), (int)A(2));
    if (A(3)) *(uint32_t *)P(3) = (uint32_t)w;
    return 1;
}
DEF(GetConsoleScreenBufferInfo)
{
    struct hobj *o = handle_get(A(0));
    if (!o || !(o->console || o->type == H_CONSOLE_OUT))
        FAIL(ERROR_INVALID_HANDLE);
    uint16_t *csbi = P(1);
    csbi[0] = 80; csbi[1] = 25;         /* dwSize */
    csbi[2] = 0; csbi[3] = 24;          /* dwCursorPosition: the bottom line, as good a guess as any */
    csbi[4] = (uint16_t)console_attr;
    csbi[5] = 0; csbi[6] = 0; csbi[7] = 79; csbi[8] = 24;   /* srWindow */
    csbi[9] = 80; csbi[10] = 25;        /* dwMaximumWindowSize */
    return 1;
}
DEF(GetConsoleScreenBufferInfoEx) { (void)a; FAIL(ERROR_INVALID_HANDLE); }
DEF(SetConsoleScreenBufferSize) { (void)a; return 1; }
DEF(SetConsoleWindowInfo) { (void)a; return 1; }
DEF(GetLargestConsoleWindowSize) { (void)a; return 80 | 25 << 16; }
DEF(SetConsoleCursorPosition)
{
    char seq[32];
    uint32_t c = A(1);
    int n = snprintf(seq, sizeof(seq), "\033[%u;%uH", (c >> 16) + 1, (c & 0xFFFF) + 1);
    crt_flush_all();
    console_write(1, seq, (uint32_t)n);
    return 1;
}
DEF(GetConsoleCursorInfo) { uint32_t *ci = P(1); ci[0] = 25; ci[1] = 1; return 1; }
DEF(SetConsoleCursorInfo)
{
    const uint32_t *ci = P(1);
    crt_flush_all();
    console_write(1, ci[1] ? "\033[?25h" : "\033[?25l", 6);
    return 1;
}
DEF(SetConsoleTextAttribute)
{
    uint32_t attr = A(1) & 0xFF;
    static const int ansi[8] = { 30, 34, 32, 36, 31, 35, 33, 37 };
    char seq[32];
    int fg = ansi[attr & 7] + ((attr & 8) ? 60 : 0), bg = ansi[(attr >> 4) & 7] + 10 + ((attr & 0x80) ? 60 : 0);
    int n = snprintf(seq, sizeof(seq), "\033[0;%d;%dm", fg, bg);
    console_attr = (int)attr;
    crt_flush_all();
    console_write(1, seq, (uint32_t)n);
    return 1;
}
DEF(FillConsoleOutputCharacterA)
{
    uint32_t n = A(2), c = A(3);
    char seq[32];
    int k = snprintf(seq, sizeof(seq), "\033[%u;%uH", (c >> 16) + 1, (c & 0xFFFF) + 1);
    crt_flush_all();
    console_write(1, seq, (uint32_t)k);
    if (n >= 80 * 25 && (c & 0xFFFF) == 0 && (c >> 16) == 0) {
        console_write(1, "\033[2J\033[H", 7);
    } else {
        char line[256];
        memset(line, (char)A(1), sizeof(line));
        while (n) {
            uint32_t chunk = n > 80 ? 80 : n;
            console_write(1, line, chunk);
            n -= chunk;
        }
        console_write(1, seq, (uint32_t)k);
    }
    if (A(4)) *(uint32_t *)P(4) = A(2);
    return 1;
}
DEF(FillConsoleOutputCharacterW) { return api_FillConsoleOutputCharacterA(a); }
DEF(FillConsoleOutputAttribute) { if (A(4)) *(uint32_t *)P(4) = A(2); return 1; }
DEF(WriteConsoleOutputCharacterA)
{
    uint32_t c = A(3);
    char seq[32];
    int k = snprintf(seq, sizeof(seq), "\0337\033[%u;%uH", (c >> 16) + 1, (c & 0xFFFF) + 1);
    crt_flush_all();
    console_write(1, seq, (uint32_t)k);
    console_write(1, P(1), A(2));
    console_write(1, "\0338", 2);
    if (A(4)) *(uint32_t *)P(4) = A(2);
    return 1;
}
DEF(WriteConsoleOutputCharacterW)
{
    char *buf = malloc(A(2) * 3 + 1);
    int len = wide_to_utf8(P(1), (int)A(2), buf, (int)A(2) * 3);
    gp_t g = galloc((uint32_t)len + 1);
    memcpy(GP(g), buf, (size_t)len);
    uint32_t b[5] = { A(0), g, (uint32_t)len, A(3), A(4) };
    uint32_t r = api_WriteConsoleOutputCharacterA(b);
    gfree(g);
    free(buf);
    if (A(4)) *(uint32_t *)P(4) = A(2);
    return r;
}
DEF(WriteConsoleOutputAttribute) { if (A(4)) *(uint32_t *)P(4) = A(2); return 1; }
DEF(WriteConsoleOutputA)
{
    /* (h, CHAR_INFO *buf, size, coord, SMALL_RECT *region): draw the region */
    const uint32_t *ci = P(1);
    uint32_t size = A(2);
    int w = (int)(size & 0xFFFF);
    int16_t *region = P(4);
    crt_flush_all();
    console_write(1, "\0337", 2);
    for (int y = region[1]; y <= region[3]; y++) {
        char seq[32];
        int k = snprintf(seq, sizeof(seq), "\033[%d;%dH", y + 1, region[0] + 1);
        console_write(1, seq, (uint32_t)k);
        char line[512];
        int n = 0;
        for (int x = region[0]; x <= region[2] && n < 500; x++) {
            uint32_t cell = ci[(y - region[1] + (A(3) >> 16)) * w + (x - region[0] + (A(3) & 0xFFFF))];
            line[n++] = (char)(cell & 0xFF ? cell & 0xFF : ' ');
        }
        console_write(1, line, (uint32_t)n);
    }
    console_write(1, "\0338", 2);
    return 1;
}
DEF(WriteConsoleOutputW) { return api_WriteConsoleOutputA(a); }
DEF(ReadConsoleOutputA) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(ReadConsoleOutputW) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(ReadConsoleOutputCharacterA) { if (A(4)) *(uint32_t *)P(4) = 0; return 1; }
DEF(ReadConsoleOutputAttribute) { if (A(4)) *(uint32_t *)P(4) = 0; return 1; }
DEF(ScrollConsoleScreenBufferA) { (void)a; return 1; }
DEF(ScrollConsoleScreenBufferW) { (void)a; return 1; }
DEF(SetConsoleTitleA) { (void)a; return 1; }
DEF(SetConsoleTitleW) { (void)a; return 1; }
DEF(GetConsoleTitleA) { if (A(1)) snprintf(P(0), A(1), "%s", main_module->winpath); return (uint32_t)strlen(main_module->winpath); }
DEF(GetConsoleTitleW) { if (A(1)) ansi_to_wide(main_module->winpath, -1, P(0), (int)A(1)); return (uint32_t)strlen(main_module->winpath); }
DEF(AllocConsole) { (void)a; return 1; }
DEF(FreeConsole) { (void)a; return 1; }
DEF(AttachConsole) { (void)a; return 1; }
DEF(GetConsoleWindow) { (void)a; return 0; }
DEF(SetConsoleCtrlHandler) { (void)a; return 1; }
DEF(GenerateConsoleCtrlEvent) { (void)a; return 1; }
DEF(GetNumberOfConsoleInputEvents)
{
    *(uint32_t *)P(1) = console_kbhit() ? 1 : 0;
    return 1;
}
DEF(GetNumberOfConsoleMouseButtons) { *(uint32_t *)P(0) = 2; return 1; }
/* INPUT_RECORD: EventType(2), pad(2), KEY_EVENT_RECORD: bKeyDown(4), wRepeatCount(2), wVirtualKeyCode(2),
 * wVirtualScanCode(2), uChar(2), dwControlKeyState(4) = 20 bytes */
static int pending_key = -1;
static void key_record(uint8_t *rec, int c, int down)
{
    memset(rec, 0, 20);
    *(uint16_t *)rec = 1;               /* KEY_EVENT */
    *(uint32_t *)(rec + 4) = (uint32_t)down;
    *(uint16_t *)(rec + 8) = 1;
    uint16_t vk = (uint16_t)c;
    if (c >= 'a' && c <= 'z') vk = (uint16_t)(c - 32);
    else if (c == '\n' || c == '\r') vk = 0x0D;
    else if (c == 27) vk = 0x1B;
    else if (c == 8 || c == 127) vk = 0x08;
    else if (c == '\t') vk = 0x09;
    *(uint16_t *)(rec + 10) = vk;
    *(uint16_t *)(rec + 14) = (uint16_t)(c == 127 ? 8 : c);
}
DEF(ReadConsoleInputA)
{
    uint32_t n = A(2);
    uint32_t *read_out = P(3);
    if (!n)
        FAIL(ERROR_INVALID_PARAMETER);
    int c = pending_key >= 0 ? pending_key : console_getch();
    pending_key = -1;
    if (c < 0)
        FAIL(ERROR_READ_FAULT);
    uint8_t *rec = P(1);
    key_record(rec, c, 1);
    uint32_t got = 1;
    if (n >= 2) {
        key_record(rec + 20, c, 0);
        got = 2;
    }
    if (read_out) *read_out = got;
    return 1;
}
DEF(ReadConsoleInputW) { return api_ReadConsoleInputA(a); }
DEF(PeekConsoleInputA)
{
    uint32_t *read_out = P(3);
    if (pending_key < 0 && console_kbhit())
        pending_key = console_getch();
    if (pending_key < 0) {
        if (read_out) *read_out = 0;
        return 1;
    }
    key_record(P(1), pending_key, 1);
    if (read_out) *read_out = 1;
    return 1;
}
DEF(PeekConsoleInputW) { return api_PeekConsoleInputA(a); }
DEF(FlushConsoleInputBuffer) { pending_key = -1; return 1; }
DEF(WriteConsoleInputA) { if (A(3)) *(uint32_t *)P(3) = A(2); return 1; }
DEF(SetConsoleActiveScreenBuffer) { (void)a; return 1; }
DEF(CreateConsoleScreenBuffer) { (void)a; return handle_for_fd(dup(1), H_CONSOLE_OUT, 1); }
DEF(GetConsoleFontSize) { (void)a; return 8 | 16 << 16; }
DEF(GetCurrentConsoleFont) { (void)a; return 1; }
DEF(SetConsoleIcon) { (void)a; return 1; }
DEF(GetConsoleDisplayMode) { *(uint32_t *)P(0) = 0; return 1; }
DEF(GetConsoleAliasesLengthA) { (void)a; return 0; }
DEF(GetConsoleProcessList) { if (A(1)) *(uint32_t *)P(0) = teb->process_id; return 1; }
DEF(GetConsoleHistoryInfo) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(SetConsoleHistoryInfo) { (void)a; return 1; }
DEF(GetConsoleSelectionInfo) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }

/* ---- strings and code pages ---- */

DEF(lstrlenA) { return A(0) ? (uint32_t)strlen(S(0)) : 0; }
DEF(lstrlenW) { return A(0) ? (uint32_t)wstrlen(WS(0)) : 0; }
DEF(lstrcpyA) { if (!A(0) || !A(1)) return 0; strcpy(P(0), S(1)); return A(0); }
DEF(lstrcpyW) { if (!A(0) || !A(1)) return 0; wstrcpy(P(0), WS(1)); return A(0); }
DEF(lstrcpynA)
{
    if (!A(0) || !A(1) || !A(2)) return 0;
    char *d = P(0);
    const char *s = S(1);
    uint32_t n = A(2);
    uint32_t i;
    for (i = 0; i + 1 < n && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
    return A(0);
}
DEF(lstrcpynW)
{
    if (!A(0) || !A(1) || !A(2)) return 0;
    uint16_t *d = P(0);
    const uint16_t *s = WS(1);
    uint32_t n = A(2), i;
    for (i = 0; i + 1 < n && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
    return A(0);
}
DEF(lstrcatA) { if (!A(0) || !A(1)) return 0; strcat(P(0), S(1)); return A(0); }
DEF(lstrcatW) { if (!A(0) || !A(1)) return 0; uint16_t *d = P(0); wstrcpy(d + wstrlen(d), WS(1)); return A(0); }
DEF(lstrcmpA) { int r = strcmp(S(0) ? S(0) : "", S(1) ? S(1) : ""); return (uint32_t)(r < 0 ? -1 : r > 0); }
DEF(lstrcmpiA) { int r = strcasecmp(S(0) ? S(0) : "", S(1) ? S(1) : ""); return (uint32_t)(r < 0 ? -1 : r > 0); }
static const uint16_t wempty = 0;
DEF(lstrcmpW) { int r = wstrcmp(A(0) ? WS(0) : &wempty, A(1) ? WS(1) : &wempty); return (uint32_t)(r < 0 ? -1 : r > 0); }
DEF(lstrcmpiW) { int r = wstrcasecmp(A(0) ? WS(0) : &wempty, A(1) ? WS(1) : &wempty); return (uint32_t)(r < 0 ? -1 : r > 0); }
DEF(CompareStringA)
{
    /* (locale, flags, s1, n1, s2, n2): 1 less, 2 equal, 3 greater */
    const char *x = S(2), *y = S(4);
    int n1 = (int)A(3), n2 = (int)A(5);
    if (!x || !y)
        FAIL(ERROR_INVALID_PARAMETER);
    if (n1 < 0) n1 = (int)strlen(x);
    if (n2 < 0) n2 = (int)strlen(y);
    int ci = (A(1) & 1) != 0;           /* NORM_IGNORECASE */
    int n = n1 < n2 ? n1 : n2;
    int r = ci ? strncasecmp(x, y, (size_t)n) : strncmp(x, y, (size_t)n);
    if (!r)
        r = n1 - n2;
    return r < 0 ? 1 : r > 0 ? 3 : 2;
}
DEF(CompareStringW)
{
    const uint16_t *x = WS(2), *y = WS(4);
    int n1 = (int)A(3), n2 = (int)A(5);
    if (!x || !y)
        FAIL(ERROR_INVALID_PARAMETER);
    if (n1 < 0) n1 = wstrlen(x);
    if (n2 < 0) n2 = wstrlen(y);
    int ci = (A(1) & 1) != 0;
    int n = n1 < n2 ? n1 : n2, r = 0;
    for (int i = 0; i < n && !r; i++) {
        uint16_t c1 = x[i], c2 = y[i];
        if (ci) {
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        }
        r = (int)c1 - (int)c2;
    }
    if (!r)
        r = n1 - n2;
    return r < 0 ? 1 : r > 0 ? 3 : 2;
}
DEF(CompareStringEx) { uint32_t b[6] = { 0, A(1), A(2), A(3), A(4), A(5) }; return api_CompareStringW(b); }
DEF(CompareStringOrdinal)
{
    uint32_t b[6] = { 0, A(4) ? 1 : 0, A(0), A(1), A(2), A(3) };
    return api_CompareStringW(b);
}
DEF(CharToOemA) { if (A(0) && A(1)) strcpy(P(1), S(0)); return 1; }
DEF(OemToCharA) { if (A(0) && A(1)) strcpy(P(1), S(0)); return 1; }

/* Code pages: 1252 and 437 and friends are bytes (Latin-1 near enough), 65001 is UTF-8. */
static int cp_is_utf8(uint32_t cp) { return cp == 65001; }

DEF(MultiByteToWideChar)
{
    uint32_t cp = A(0);
    const char *s = S(2);
    int n = (int)A(3);
    uint16_t *out = P(4);
    int outlen = (int)A(5);
    if (!s || n == 0 || (outlen && !out))
        FAIL(ERROR_INVALID_PARAMETER);
    if (n < 0)
        n = (int)strlen(s) + 1;
    int need = cp_is_utf8(cp) ? utf8_to_wide(s, n, NULL, 0) : n;
    if (!outlen)
        return (uint32_t)need;
    if (need > outlen)
        FAIL(ERROR_INSUFFICIENT_BUFFER);
    if (cp_is_utf8(cp))
        utf8_to_wide(s, n, out, outlen);
    else
        ansi_to_wide(s, n, out, outlen);
    return (uint32_t)need;
}
DEF(WideCharToMultiByte)
{
    uint32_t cp = A(0);
    const uint16_t *s = WS(2);
    int n = (int)A(3);
    char *out = P(4);
    int outlen = (int)A(5);
    if (!s || n == 0 || (outlen && !out))
        FAIL(ERROR_INVALID_PARAMETER);
    if (n < 0)
        n = wstrlen(s) + 1;
    int need = cp_is_utf8(cp) ? wide_to_utf8(s, n, NULL, 0) : n;
    if (!outlen)
        return (uint32_t)need;
    if (need > outlen)
        FAIL(ERROR_INSUFFICIENT_BUFFER);
    if (cp_is_utf8(cp)) {
        wide_to_utf8(s, n, out, outlen);
    } else {
        const char *def = S(6);
        int used = 0;
        for (int i = 0; i < n; i++) {
            if (s[i] < 256)
                out[i] = (char)s[i];
            else {
                out[i] = def ? def[0] : '?';
                used = 1;
            }
        }
        if (A(7))
            *(uint32_t *)P(7) = (uint32_t)used;
    }
    return (uint32_t)need;
}
static uint32_t api_GetStringTypeExA(uint32_t *a);
DEF(GetStringTypeA) { return api_GetStringTypeExA(a); }
static uint16_t ctype1(uint16_t c)
{
    uint16_t t = 0;
    if (c >= 'A' && c <= 'Z') t |= 0x1 | 0x100 | 0x80;         /* C1_UPPER, C1_ALPHA, C1_XDIGIT? */
    if (c >= 'A' && c <= 'F') t |= 0x40;
    if (c >= 'a' && c <= 'z') t |= 0x2 | 0x100;
    if (c >= 'a' && c <= 'f') t |= 0x40;
    if (c >= '0' && c <= '9') t |= 0x4 | 0x40;
    if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x8;
    if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) t |= 0x10;
    if (c < 32 || c == 127) t |= 0x20;
    if (c == ' ' || c == '\t') t |= 0x40;   /* C1_BLANK */
    if (c >= 192 && c < 256 && c != 215 && c != 247) t |= 0x100 | (c < 223 ? 1 : 2);
    return t;
}
DEF(GetStringTypeW)
{
    uint32_t type = A(0);
    const uint16_t *s = WS(1);
    int n = (int)A(2);
    uint16_t *out = P(3);
    if (!s || !out)
        FAIL(ERROR_INVALID_PARAMETER);
    if (n < 0)
        n = wstrlen(s) + 1;
    for (int i = 0; i < n; i++) {
        uint16_t c = s[i];
        if (type == 1) {
            uint16_t t = ctype1(c);
            /* C1 bits: UPPER 1, LOWER 2, DIGIT 4, SPACE 8, PUNCT 10, CNTRL 20, BLANK 40, XDIGIT 80, ALPHA 100 */
            uint16_t r = 0;
            if (t & 0x1) r |= 0x1;
            if (t & 0x2) r |= 0x2;
            if (t & 0x4) r |= 0x4;
            if (t & 0x8) r |= 0x8;
            if (t & 0x10) r |= 0x10;
            if (t & 0x20) r |= 0x20;
            if (c == ' ' || c == '\t') r |= 0x40;
            if ((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')) r |= 0x80;
            if (t & 0x100) r |= 0x100;
            out[i] = r;
        } else if (type == 2) {
            out[i] = (c >= '0' && c <= '9') ? 3 : c < 128 ? 1 : 1;   /* C2_EUROPENUMBER / C2_LEFTTORIGHT */
        } else {
            out[i] = (c >= 'a' && c <= 'z') ? 0x1 : (c >= 'A' && c <= 'Z') ? 0x8000 >> 15 : 0;
        }
    }
    return 1;
}
DEF(GetStringTypeExA)
{
    /* (locale, type, s, n, out): bytes */
    const char *s = S(2);
    int n = (int)A(3);
    uint16_t *out = P(4);
    if (!s || !out)
        FAIL(ERROR_INVALID_PARAMETER);
    if (n < 0)
        n = (int)strlen(s) + 1;
    uint16_t w[1];
    for (int i = 0; i < n; i++) {
        w[0] = (uint8_t)s[i];
        gp_t gw = galloc(4), go = galloc(4);
        memcpy(GP(gw), w, 2);
        uint32_t b[4] = { A(1), gw, 1, go };
        api_GetStringTypeW(b);
        out[i] = *(uint16_t *)GP(go);
        gfree(gw);
        gfree(go);
    }
    return 1;
}
DEF(GetStringTypeExW) { return api_GetStringTypeW(a + 1); }
DEF(LCMapStringA)
{
    /* (locale, flags, src, n, dst, dstlen) */
    uint32_t flags = A(1);
    const char *s = S(2);
    int n = (int)A(3);
    char *out = P(4);
    int outlen = (int)A(5);
    if (!s)
        FAIL(ERROR_INVALID_PARAMETER);
    if (n < 0)
        n = (int)strlen(s) + 1;
    if (!outlen)
        return (uint32_t)n;
    if (n > outlen)
        FAIL(ERROR_INSUFFICIENT_BUFFER);
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (flags & 0x200 && c >= 'a' && c <= 'z') c -= 32;        /* LCMAP_UPPERCASE */
        if (flags & 0x100 && c >= 'A' && c <= 'Z') c += 32;        /* LCMAP_LOWERCASE */
        out[i] = c;
    }
    return (uint32_t)n;
}
DEF(LCMapStringW)
{
    uint32_t flags = A(1);
    const uint16_t *s = WS(2);
    int n = (int)A(3);
    uint16_t *out = P(4);
    int outlen = (int)A(5);
    if (!s)
        FAIL(ERROR_INVALID_PARAMETER);
    if (n < 0)
        n = wstrlen(s) + 1;
    if (!outlen)
        return (uint32_t)n;
    if (n > outlen)
        FAIL(ERROR_INSUFFICIENT_BUFFER);
    for (int i = 0; i < n; i++) {
        uint16_t c = s[i];
        if (flags & 0x200 && c >= 'a' && c <= 'z') c -= 32;
        if (flags & 0x100 && c >= 'A' && c <= 'Z') c += 32;
        out[i] = c;
    }
    return (uint32_t)n;
}
DEF(LCMapStringEx) { uint32_t b[6] = { 0, A(1), A(2), A(3), A(4), A(5) }; return api_LCMapStringW(b); }
DEF(FoldStringA) { uint32_t b[6] = { 0, 0, A(1), A(2), A(3), A(4) }; return api_LCMapStringA(b); }
DEF(FoldStringW) { uint32_t b[6] = { 0, 0, A(1), A(2), A(3), A(4) }; return api_LCMapStringW(b); }

/* GetLocaleInfo: the few things the C runtime and programs ask for. */
static const char *locale_info(uint32_t type)
{
    switch (type & 0xFFFF) {
    case 0x0001: return "0409";         /* LOCALE_ILANGUAGE */
    case 0x0002: return "English (United States)";
    case 0x0003: return "ENU";
    case 0x0004: return "English";
    case 0x0005: return "English";
    case 0x0006: return "English";
    case 0x0007: return "English";
    case 0x0008: return "1";            /* LOCALE_ICOUNTRY */
    case 0x0009: return "United States";
    case 0x000A: return "USA";
    case 0x000B: return "United States";
    case 0x000C: return "United States";
    case 0x000D: return "English";
    case 0x000F: return "0";            /* LOCALE_IDEFAULTCODEPAGE */
    case 0x0010: return "0";
    case 0x0011: return "1";
    case 0x0012: return ".";            /* LOCALE_SDECIMAL? 0x000E is SDECIMAL */
    case 0x0013: return ",";
    case 0x0014: return "3;0";
    case 0x0015: return "0";
    case 0x0016: return "-";
    case 0x0017: return ".";
    case 0x0018: return "2";
    case 0x0019: return "-";
    case 0x001A: return "0";
    case 0x001B: return "$";            /* LOCALE_SCURRENCY? 0x0014 */
    case 0x001C: return "USD";
    case 0x001D: return "1";
    case 0x001E: return "M/d/yyyy";     /* LOCALE_SSHORTDATE */
    case 0x001F: return "dddd, MMMM d, yyyy";
    case 0x0020: return "h:mm:ss tt";   /* LOCALE_STIME */
    case 0x0021: return "/";            /* no: 0x001D is SDATE */
    case 0x0022: return "0";
    case 0x0023: return "0";
    case 0x0024: return "0";
    case 0x0025: return "0";
    case 0x0026: return "0";
    case 0x0027: return "0";
    case 0x0028: return "AM";
    case 0x0029: return "PM";
    case 0x002A: return "0";
    case 0x1004: return "1252";         /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x1001: return "English";      /* LOCALE_SENGLANGUAGE */
    case 0x1002: return "United States";
    case 0x1003: return "English (United States)";
    case 0x0059: return "en";           /* LOCALE_SISO639LANGNAME */
    case 0x005A: return "US";           /* LOCALE_SISO3166CTRYNAME */
    case 0x005C: return "en-US";        /* LOCALE_SNAME */
    case 0x1009: return "0";
    case 0x100A: return "7";            /* LOCALE_RETURN_NUMBER? */
    case 0x000E: return ".";
    default: return NULL;
    }
}
DEF(GetLocaleInfoA)
{
    uint32_t type = A(1);
    const char *v = locale_info(type);
    if (type == 0x000E) v = ".";        /* LOCALE_SDECIMAL */
    if (type == 0x000F) v = ",";        /* LOCALE_STHOUSAND */
    if (type == 0x0010) v = "3;0";      /* LOCALE_SGROUPING */
    if (type == 0x0011) v = "2";        /* LOCALE_IDIGITS */
    if (type == 0x0012) v = "1";        /* LOCALE_ILZERO */
    if (type == 0x0014) v = "$";        /* LOCALE_SCURRENCY */
    if (type == 0x0015) v = "USD";
    if (type == 0x0016) v = ".";        /* LOCALE_SMONDECIMALSEP */
    if (type == 0x0017) v = ",";
    if (type == 0x0018) v = "3;0";
    if (type == 0x0019) v = "2";        /* LOCALE_ICURRDIGITS */
    if (type == 0x001B) v = "0";        /* LOCALE_ICURRENCY */
    if (type == 0x001C) v = "0";
    if (type == 0x001D) v = "/";        /* LOCALE_SDATE */
    if (type == 0x001E) v = ":";        /* LOCALE_STIME */
    if (type == 0x001F) v = "M/d/yyyy";
    if (type == 0x0020) v = "dddd, MMMM d, yyyy";
    if (type == 0x1003) v = "h:mm:ss tt";   /* LOCALE_STIMEFORMAT */
    if (type == 0x0021) v = "0";        /* LOCALE_IDATE */
    if (type == 0x0022) v = "0";        /* LOCALE_ILDATE */
    if (type == 0x0023) v = "0";        /* LOCALE_ITIME */
    if (type == 0x0025) v = "0";        /* LOCALE_ICENTURY */
    if (type == 0x0026) v = "0";        /* LOCALE_ITLZERO */
    if (type == 0x0027) v = "0";
    if (type == 0x0028) v = "AM";
    if (type == 0x0029) v = "PM";
    if (type == 0x0050) v = "-";        /* LOCALE_SNEGATIVESIGN */
    if (type == 0x0051) v = "+";        /* LOCALE_SPOSITIVESIGN */
    if (type == 0x0052) v = "0";
    if (type == 0x0053) v = "1";
    if (type == 0x0054) v = "1";
    if (type == 0x0055) v = "1";        /* LOCALE_INEGSEPBYSPACE? */
    if (type == 0x0058) v = "1";
    if (type == 0x100C) v = "0";
    if (type == 0x1005) v = "1";        /* LOCALE_IDEFAULTCODEPAGE? */
    if (type == 0x1009) v = "1252";
    if ((type & 0x20000000) && v) {     /* LOCALE_RETURN_NUMBER */
        if (A(3) < 2)
            FAIL(ERROR_INSUFFICIENT_BUFFER);
        *(uint32_t *)P(2) = (uint32_t)strtoul(v, NULL, 10);
        return 2;
    }
    if (!v) {
        wtrace("GetLocaleInfo: type %08x unknown", type);
        v = "";
    }
    uint32_t n = (uint32_t)strlen(v) + 1;
    if (!A(3))
        return n;
    if (A(3) < n)
        FAIL(ERROR_INSUFFICIENT_BUFFER);
    strcpy(P(2), v);
    return n;
}
DEF(GetLocaleInfoW)
{
    gp_t out = galloc(256);
    uint32_t b[4] = { A(0), A(1), out, 256 };
    uint32_t r = api_GetLocaleInfoA(b);
    if (r && (A(1) & 0x20000000)) {
        if (A(3) < 2) { gfree(out); FAIL(ERROR_INSUFFICIENT_BUFFER); }
        *(uint32_t *)P(2) = *(uint32_t *)GP(out);
        gfree(out);
        return 2;
    }
    if (r && A(3)) {
        if (A(3) < r) { gfree(out); FAIL(ERROR_INSUFFICIENT_BUFFER); }
        ansi_to_wide(GP(out), -1, P(2), (int)A(3));
    }
    gfree(out);
    return r;
}
DEF(GetLocaleInfoEx) { uint32_t b[4] = { 0x409, A(1), A(2), A(3) }; return api_GetLocaleInfoW(b); }
DEF(EnumSystemLocalesA) { if (A(0)) { gp_t s = gstrdup("00000409"); call_guest1(A(0), s); gfree(s); } return 1; }
DEF(EnumSystemLocalesW) { if (A(0)) { gp_t s = gwstrdup((const uint16_t[]){ '0','0','0','0','0','4','0','9',0 }); call_guest1(A(0), s); gfree(s); } return 1; }
DEF(EnumSystemLocalesEx) { (void)a; return 1; }
DEF(EnumUILanguagesA) { (void)a; return 1; }
DEF(GetSystemPreferredUILanguages) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(GetUserPreferredUILanguages) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(LocaleNameToLCID) { (void)a; return 0x409; }
DEF(LCIDToLocaleName) { if (A(1) && A(2) >= 6) ansi_to_wide("en-US", -1, P(1), 6); return 6; }
DEF(GetUserDefaultLocaleName) { if (A(0) && A(1) >= 6) ansi_to_wide("en-US", -1, P(0), 6); return 6; }
DEF(GetSystemDefaultLocaleName) { if (A(0) && A(1) >= 6) ansi_to_wide("en-US", -1, P(0), 6); return 6; }
DEF(IsValidLocaleName) { (void)a; return 1; }
DEF(ResolveLocaleName) { if (A(1) && A(2) >= 6) ansi_to_wide("en-US", -1, P(1), 6); return 6; }
DEF(GetNumberFormatA)
{
    /* (locale, flags, value, fmt, out, size): leave the number as it is */
    uint32_t n = (uint32_t)strlen(S(2)) + 1;
    if (!A(5)) return n;
    if (A(5) < n) FAIL(ERROR_INSUFFICIENT_BUFFER);
    strcpy(P(4), S(2));
    return n;
}
DEF(GetNumberFormatW)
{
    uint32_t n = (uint32_t)wstrlen(WS(2)) + 1;
    if (!A(5)) return n;
    if (A(5) < n) FAIL(ERROR_INSUFFICIENT_BUFFER);
    wstrcpy(P(4), WS(2));
    return n;
}
DEF(GetCurrencyFormatA) { return api_GetNumberFormatA(a); }
DEF(GetCurrencyFormatW) { return api_GetNumberFormatW(a); }
static uint32_t format_date(const SYSTEMTIME *st, const char *fmt, int time, char *out, uint32_t size)
{
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December" };
    char buf[256];
    uint32_t o = 0;
    for (const char *p = fmt; *p && o < sizeof(buf) - 16; ) {
        char c = *p;
        int n = 0;
        while (p[n] == c)
            n++;
        if (c == '\'') {
            p++;
            while (*p && *p != '\'' && o < sizeof(buf) - 1)
                buf[o++] = *p++;
            if (*p == '\'')
                p++;
            continue;
        }
        p += n;
        int dow = st->dow < 7 ? st->dow : 0, mon = st->month >= 1 && st->month <= 12 ? st->month - 1 : 0;
        if (!time && c == 'd') {
            if (n == 1) o += (uint32_t)sprintf(buf + o, "%d", st->day);
            else if (n == 2) o += (uint32_t)sprintf(buf + o, "%02d", st->day);
            else if (n == 3) o += (uint32_t)sprintf(buf + o, "%.3s", days[dow]);
            else o += (uint32_t)sprintf(buf + o, "%s", days[dow]);
        } else if (!time && c == 'M') {
            if (n == 1) o += (uint32_t)sprintf(buf + o, "%d", st->month);
            else if (n == 2) o += (uint32_t)sprintf(buf + o, "%02d", st->month);
            else if (n == 3) o += (uint32_t)sprintf(buf + o, "%.3s", months[mon]);
            else o += (uint32_t)sprintf(buf + o, "%s", months[mon]);
        } else if (!time && c == 'y') {
            if (n <= 2) o += (uint32_t)sprintf(buf + o, "%02d", st->year % 100);
            else o += (uint32_t)sprintf(buf + o, "%d", st->year);
        } else if (!time && c == 'g') {
            o += (uint32_t)sprintf(buf + o, "A.D.");
        } else if (time && (c == 'h' || c == 'H')) {
            int h = c == 'h' ? (st->hour % 12 ? st->hour % 12 : 12) : st->hour;
            o += (uint32_t)sprintf(buf + o, n == 1 ? "%d" : "%02d", h);
        } else if (time && c == 'm') {
            o += (uint32_t)sprintf(buf + o, n == 1 ? "%d" : "%02d", st->minute);
        } else if (time && c == 's') {
            o += (uint32_t)sprintf(buf + o, n == 1 ? "%d" : "%02d", st->second);
        } else if (time && c == 't') {
            o += (uint32_t)sprintf(buf + o, n == 1 ? "%.1s" : "%s", st->hour < 12 ? "AM" : "PM");
        } else {
            for (int i = 0; i < n; i++)
                buf[o++] = c;
        }
    }
    buf[o] = 0;
    uint32_t n = o + 1;
    if (!size)
        return n;
    if (size < n)
        return 0;
    memcpy(out, buf, n);
    return n;
}
DEF(GetDateFormatA)
{
    SYSTEMTIME st;
    if (A(2)) memcpy(&st, P(2), sizeof(st)); else filetime_to_systemtime(now_filetime() + local_bias_100ns(), &st);
    const char *fmt = S(3) ? S(3) : (A(1) & 2) ? "dddd, MMMM d, yyyy" : "M/d/yyyy";
    uint32_t r = format_date(&st, fmt, 0, P(4), A(5));
    if (!r) SETERR(ERROR_INSUFFICIENT_BUFFER);
    return r;
}
DEF(GetDateFormatW)
{
    gp_t f = A(3) ? gstrdup(wtmp(A(3))) : 0, out = galloc(256);
    uint32_t b[6] = { A(0), A(1), A(2), f, out, 256 };
    uint32_t r = api_GetDateFormatA(b);
    if (r && A(5)) {
        if (A(5) < r) r = 0; else ansi_to_wide(GP(out), -1, P(4), (int)A(5));
    }
    if (f) gfree(f);
    gfree(out);
    return r;
}
DEF(GetTimeFormatA)
{
    SYSTEMTIME st;
    if (A(2)) memcpy(&st, P(2), sizeof(st)); else filetime_to_systemtime(now_filetime() + local_bias_100ns(), &st);
    const char *fmt = S(3) ? S(3) : "h:mm:ss tt";
    uint32_t r = format_date(&st, fmt, 1, P(4), A(5));
    if (!r) SETERR(ERROR_INSUFFICIENT_BUFFER);
    return r;
}
DEF(GetTimeFormatW)
{
    gp_t f = A(3) ? gstrdup(wtmp(A(3))) : 0, out = galloc(256);
    uint32_t b[6] = { A(0), A(1), A(2), f, out, 256 };
    uint32_t r = api_GetTimeFormatA(b);
    if (r && A(5)) {
        if (A(5) < r) r = 0; else ansi_to_wide(GP(out), -1, P(4), (int)A(5));
    }
    if (f) gfree(f);
    gfree(out);
    return r;
}
DEF(GetDateFormatEx) { uint32_t b[6] = { 0x409, A(1), A(2), A(3), A(4), A(5) }; return api_GetDateFormatW(b); }
DEF(GetTimeFormatEx) { uint32_t b[6] = { 0x409, A(1), A(2), A(3), A(4), A(5) }; return api_GetTimeFormatW(b); }
DEF(GetCalendarInfoA) { (void)a; FAIL(ERROR_INVALID_PARAMETER); }
DEF(GetCalendarInfoW) { (void)a; FAIL(ERROR_INVALID_PARAMETER); }
DEF(GetGeoInfoA) { (void)a; FAIL(ERROR_INVALID_PARAMETER); }
DEF(GetUserGeoID) { (void)a; return 244; }
DEF(IsNLSDefinedString) { (void)a; return 1; }
DEF(NormalizeString) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(IsNormalizedString) { (void)a; return 1; }
DEF(GetThreadPreferredUILanguages) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }

/* ---- FormatMessage ---- */

static const char *system_message(uint32_t code)
{
    switch (code) {
    case 0: return "The operation completed successfully.";
    case 1: return "Incorrect function.";
    case 2: return "The system cannot find the file specified.";
    case 3: return "The system cannot find the path specified.";
    case 4: return "The system cannot open the file.";
    case 5: return "Access is denied.";
    case 6: return "The handle is invalid.";
    case 8: return "Not enough storage is available to process this command.";
    case 11: return "An attempt was made to load a program with an incorrect format.";
    case 13: return "The data is invalid.";
    case 14: return "Not enough storage is available to complete this operation.";
    case 15: return "The system cannot find the drive specified.";
    case 17: return "The system cannot move the file to a different disk drive.";
    case 18: return "There are no more files.";
    case 19: return "The media is write protected.";
    case 21: return "The device is not ready.";
    case 32: return "The process cannot access the file because it is being used by another process.";
    case 33: return "The process cannot access the file because another process has locked a portion of the file.";
    case 38: return "Reached the end of the file.";
    case 50: return "The request is not supported.";
    case 80: return "The file exists.";
    case 82: return "The directory or file cannot be created.";
    case 87: return "The parameter is incorrect.";
    case 109: return "The pipe has been ended.";
    case 110: return "The system cannot open the device or file specified.";
    case 111: return "The file name is too long.";
    case 112: return "There is not enough space on the disk.";
    case 120: return "This function is not supported on this system.";
    case 121: return "The semaphore timeout period has expired.";
    case 122: return "The data area passed to a system call is too small.";
    case 123: return "The filename, directory name, or volume label syntax is incorrect.";
    case 126: return "The specified module could not be found.";
    case 127: return "The specified procedure could not be found.";
    case 145: return "The directory is not empty.";
    case 183: return "Cannot create a file when that file already exists.";
    case 193: return "%1 is not a valid Win32 application.";
    case 203: return "The system could not find the environment option that was entered.";
    case 206: return "The filename or extension is too long.";
    case 234: return "More data is available.";
    case 259: return "No more data is available.";
    case 267: return "The directory name is invalid.";
    case 487: return "Attempt to access invalid address.";
    case 997: return "Overlapped I/O operation is in progress.";
    case 998: return "Invalid access to memory location.";
    case 1004: return "Invalid flags.";
    case 1150: return "The specified program requires a newer version of Windows.";
    case 1400: return "Invalid window handle.";
    case 1407: return "Cannot find window class.";
    case 1410: return "Class already exists.";
    case 1411: return "Class does not exist.";
    case 1813: return "The specified resource type cannot be found in the image file.";
    case 1814: return "The specified resource name cannot be found in the image file.";
    case 0xC0000005: return "The instruction at 0x%08lx referenced memory at 0x%08lx. The memory could not be %s.";
    default: return NULL;
    }
}

static uint32_t format_message(uint32_t flags, const char *src, uint32_t id, char *out, uint32_t size,
                               const uint32_t *args, int wide_args)
{
    char buf[4096];
    char msg[256];
    const char *fmt = src;
    if (flags & 0x1000) {               /* FORMAT_MESSAGE_FROM_SYSTEM */
        fmt = system_message(id);
        if (!fmt) {
            snprintf(msg, sizeof(msg), "Unknown error %u (0x%x).", id, id);
            fmt = msg;
        }
    } else if (flags & 0x800) {         /* FORMAT_MESSAGE_FROM_HMODULE: a message table resource */
        fmt = NULL;
    }
    if (!fmt) {
        SETERR(317);                    /* ERROR_MR_MID_NOT_FOUND */
        return 0;
    }
    uint32_t o = 0;
    int argn = 0;
    for (const char *p = fmt; *p && o < sizeof(buf) - 2; ) {
        if (*p == '%' && !(flags & 0x200)) {    /* not FORMAT_MESSAGE_IGNORE_INSERTS */
            p++;
            if (*p >= '1' && *p <= '9') {
                int n = *p++ - '0';
                if (*p >= '0' && *p <= '9')
                    n = n * 10 + *p++ - '0';
                char spec[16] = "s";
                if (*p == '!') {
                    const char *e = strchr(p + 1, '!');
                    if (e) {
                        snprintf(spec, sizeof(spec), "%.*s", (int)(e - p - 1), p + 1);
                        p = e + 1;
                    }
                }
                if (args && n >= 1) {
                    uint32_t v = args[n - 1];
                    char last = spec[strlen(spec) - 1];
                    if (last == 's' || last == 'S') {
                        if (wide_args || last == 'S') {
                            if (v && vm_readable(v, 2))
                                o += (uint32_t)snprintf(buf + o, sizeof(buf) - o, "%s", wtmp(v));
                        } else if (v && vm_readable(v, 1)) {
                            o += (uint32_t)snprintf(buf + o, sizeof(buf) - o, "%s", gstr(v));
                        }
                    } else if (last == 'd' || last == 'u' || last == 'x' || last == 'X' || last == 'i' || last == 'c') {
                        char f[20];
                        snprintf(f, sizeof(f), "%%%s", spec);
                        o += (uint32_t)snprintf(buf + o, sizeof(buf) - o, f, v);
                    }
                }
                (void)argn;
                continue;
            }
            if (*p == '0') { p++; break; }
            if (*p == 'n') { buf[o++] = '\r'; buf[o++] = '\n'; p++; continue; }
            if (*p == 'r') { buf[o++] = '\r'; p++; continue; }
            if (*p == 't') { buf[o++] = '\t'; p++; continue; }
            if (*p == '%') { buf[o++] = '%'; p++; continue; }
            if (*p == '.') { buf[o++] = '.'; p++; continue; }
            if (*p == '!') { buf[o++] = '!'; p++; continue; }
            if (!*p) break;
            buf[o++] = '%';
            continue;
        }
        buf[o++] = *p++;
    }
    buf[o] = 0;
    if ((flags & 0x1000) && !(flags & 0xFF) && o && buf[o - 1] != '\n') {
        buf[o++] = '\r';
        buf[o++] = '\n';
        buf[o] = 0;
    }
    uint32_t width = flags & 0xFF;
    if (width == 0xFF) {                /* FORMAT_MESSAGE_MAX_WIDTH_MASK: no line breaks */
        for (uint32_t i = 0; i < o; i++)
            if (buf[i] == '\r' || buf[i] == '\n')
                buf[i] = ' ';
    }
    if (flags & 0x100) {                /* FORMAT_MESSAGE_ALLOCATE_BUFFER: out is a pointer to a pointer */
        gp_t g = heap_alloc(process_heap, (o + 1) * (wide_args ? 2 : 1), 0);
        if (!g) {
            SETERR(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        if (wide_args)
            ansi_to_wide(buf, -1, GP(g), (int)o + 1);
        else
            memcpy(GP(g), buf, o + 1);
        *(uint32_t *)out = g;
        return o;
    }
    if (!size || size <= o) {
        SETERR(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (wide_args)
        ansi_to_wide(buf, -1, (uint16_t *)out, (int)size);
    else
        memcpy(out, buf, o + 1);
    return o;
}
DEF(FormatMessageA)
{
    return format_message(A(0), S(1), A(2), P(4), A(5), A(6) ? P(6) : NULL, 0);
}
DEF(FormatMessageW)
{
    char src[2048];
    const char *s = NULL;
    if ((A(0) & 0x400) && A(1)) {       /* FORMAT_MESSAGE_FROM_STRING */
        wide_to_ansi(WS(1), -1, src, sizeof(src));
        s = src;
    }
    return format_message(A(0), s, A(2), P(4), A(5), A(6) ? P(6) : NULL, 1);
}

/* ---- INI files ---- */

static int ini_read(const char *win, char **data)
{
    char host[PATH_MAX];
    if (path_to_host(win, host) < 0)
        return -1;
    path_fix_case(host);
    int fd = open(host, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    fstat(fd, &st);
    *data = malloc((size_t)st.st_size + 1);
    if (!*data) {
        close(fd);
        return -1;
    }
    int n = (int)read(fd, *data, (size_t)st.st_size);
    close(fd);
    if (n < 0)
        n = 0;
    (*data)[n] = 0;
    return n;
}

static int ini_write(const char *win, const char *data, size_t len)
{
    char host[PATH_MAX];
    if (path_to_host(win, host) < 0)
        return -1;
    path_fix_case(host);
    int fd = open(host, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    int r = (int)write(fd, data, len);
    close(fd);
    return r == (int)len ? 0 : -1;
}

static char *strip(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

/* Walk the file: calls fn(section, key, value, line) for each line; section is NULL before the first. */
static void ini_walk(char *data, int (*fn)(const char *sec, const char *key, const char *val, char *line, void *ctx), void *ctx)
{
    char *sec = NULL;
    char *p = data;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        char *line = p;
        if (nl)
            *nl = 0;
        char *s = strip(line);
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e)
                *e = 0;
            sec = s + 1;
            if (fn(sec, NULL, NULL, line, ctx))
                break;
        } else if (*s && *s != ';' && *s != '#') {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = 0;
                char *k = strip(s), *v = strip(eq + 1);
                if (v[0] == '"' && v[strlen(v) - 1] == '"' && strlen(v) >= 2) {
                    v[strlen(v) - 1] = 0;
                    v++;
                }
                if (fn(sec, k, v, line, ctx))
                    break;
            } else if (fn(sec, s, NULL, line, ctx)) {
                break;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
}

struct ini_query { const char *sec, *key; char *out; uint32_t size, len; int found, list_keys, list_sections; };

static int ini_get_cb(const char *sec, const char *key, const char *val, char *line, void *ctx)
{
    struct ini_query *q = ctx;
    (void)line;
    if (q->list_sections) {
        if (key == NULL && sec && q->len + strlen(sec) + 2 < q->size) {
            strcpy(q->out + q->len, sec);
            q->len += (uint32_t)strlen(sec) + 1;
        }
        return 0;
    }
    if (!sec || strcasecmp(sec, q->sec))
        return 0;
    if (q->list_keys) {
        if (key && q->len + strlen(key) + 2 < q->size) {
            strcpy(q->out + q->len, key);
            q->len += (uint32_t)strlen(key) + 1;
        }
        return 0;
    }
    if (key && !strcasecmp(key, q->key)) {
        snprintf(q->out, q->size, "%s", val ? val : "");
        q->found = 1;
        return 1;
    }
    return 0;
}

static uint32_t ini_get(const char *file, const char *sec, const char *key, const char *def, char *out, uint32_t size)
{
    if (!out || !size)
        return 0;
    char *data = NULL;
    struct ini_query q = { sec, key, out, size, 0, 0, key == NULL, sec == NULL };
    out[0] = 0;
    if (ini_read(file, &data) >= 0) {
        ini_walk(data, ini_get_cb, &q);
        free(data);
    }
    if (q.list_keys || q.list_sections) {
        if (q.len + 1 < size) {
            out[q.len] = 0;
            return q.len;
        }
        out[size - 1] = 0;
        if (size >= 2) out[size - 2] = 0;
        return size - 2;
    }
    if (!q.found) {
        snprintf(out, size, "%s", def ? def : "");
    }
    uint32_t n = (uint32_t)strlen(out);
    if (n == size - 1 && (uint32_t)strlen(def ? def : "") >= size)
        return size - 1;
    return n;
}

static int ini_set(const char *file, const char *sec, const char *key, const char *val)
{
    char *data = NULL;
    int n = ini_read(file, &data);
    if (n < 0) {
        data = strdup("");
        n = 0;
    }
    /* rebuild the file line by line */
    char *out = malloc((size_t)n + strlen(sec) + (key ? strlen(key) : 0) + (val ? strlen(val) : 0) + 64);
    if (!out) {
        free(data);
        return -1;
    }
    size_t o = 0;
    int in_sec = 0, sec_found = 0, key_done = 0;
    char *p = data;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);
        char line[1024];
        snprintf(line, sizeof(line), "%.*s", (int)(len > 1023 ? 1023 : len), p);
        char *s = strip(line);
        if (*s == '[') {
            if (in_sec && !key_done && key) {
                o += (size_t)sprintf(out + o, "%s=%s\r\n", key, val ? val : "");
                key_done = 1;
            }
            char *e = strchr(s, ']');
            if (e) *e = 0;
            in_sec = !strcasecmp(s + 1, sec);
            if (in_sec) {
                sec_found = 1;
                if (!key) {             /* delete the whole section */
                    p = nl ? nl + 1 : NULL;
                    while (p && *p) {
                        char *nl2 = strchr(p, '\n');
                        char l2[1024];
                        snprintf(l2, sizeof(l2), "%.*s", (int)(nl2 ? nl2 - p : (long)strlen(p)), p);
                        if (*strip(l2) == '[')
                            break;
                        p = nl2 ? nl2 + 1 : NULL;
                    }
                    in_sec = 0;
                    continue;
                }
            }
        } else if (in_sec && key) {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = 0;
                if (!strcasecmp(strip(s), key)) {
                    if (val && !key_done)
                        o += (size_t)sprintf(out + o, "%s=%s\r\n", key, val);
                    key_done = 1;
                    p = nl ? nl + 1 : NULL;
                    continue;
                }
            }
        }
        memcpy(out + o, p, len);
        o += len;
        p = nl ? nl + 1 : NULL;
    }
    if (key && !key_done && val) {
        if (!sec_found) {
            if (o && out[o - 1] != '\n')
                o += (size_t)sprintf(out + o, "\r\n");
            o += (size_t)sprintf(out + o, "[%s]\r\n", sec);
        } else if (o && out[o - 1] != '\n') {
            o += (size_t)sprintf(out + o, "\r\n");
        }
        o += (size_t)sprintf(out + o, "%s=%s\r\n", key, val);
    }
    int r = ini_write(file, out, o);
    free(out);
    free(data);
    return r;
}

DEF(GetPrivateProfileStringA) { return ini_get(S(5), S(0), S(1), S(2), P(3), A(4)); }
DEF(GetPrivateProfileStringW)
{
    gp_t sec = A(0) ? gstrdup(wtmp(A(0))) : 0, key = A(1) ? gstrdup(wtmp(A(1))) : 0;
    gp_t def = A(2) ? gstrdup(wtmp(A(2))) : 0, file = A(5) ? gstrdup(wtmp(A(5))) : 0;
    gp_t out = galloc(A(4) + 2);
    uint32_t b[6] = { sec, key, def, out, A(4), file };
    uint32_t r = api_GetPrivateProfileStringA(b);
    if (A(3) && A(4))
        ansi_to_wide(GP(out), (int)r + 1 + (key && sec ? 0 : 1), P(3), (int)A(4));
    if (sec) gfree(sec); if (key) gfree(key); if (def) gfree(def); if (file) gfree(file);
    gfree(out);
    return r;
}
DEF(GetPrivateProfileIntA)
{
    char buf[64];
    ini_get(S(3), S(0), S(1), NULL, buf, sizeof(buf));
    return buf[0] ? (uint32_t)strtol(buf, NULL, 0) : A(2);
}
DEF(GetPrivateProfileIntW)
{
    char buf[64];
    ini_get(wtmp(A(3)), wtmp(A(0)), wtmp(A(1)), NULL, buf, sizeof(buf));
    return buf[0] ? (uint32_t)strtol(buf, NULL, 0) : A(2);
}
DEF(WritePrivateProfileStringA)
{
    if (!A(0) || !A(3))
        FAIL(ERROR_INVALID_PARAMETER);
    return ini_set(S(3), S(0), S(1), S(2)) == 0;
}
DEF(WritePrivateProfileStringW)
{
    if (!A(0) || !A(3))
        FAIL(ERROR_INVALID_PARAMETER);
    char sec[256], key[256], val[1024], file[MAX_PATH];
    wide_to_ansi(WS(0), -1, sec, sizeof(sec));
    if (A(1)) wide_to_ansi(WS(1), -1, key, sizeof(key));
    if (A(2)) wide_to_ansi(WS(2), -1, val, sizeof(val));
    wide_to_ansi(WS(3), -1, file, sizeof(file));
    return ini_set(file, sec, A(1) ? key : NULL, A(2) ? val : NULL) == 0;
}
DEF(GetPrivateProfileSectionA)
{
    /* every "key=value" of the section, NUL separated */
    char *data = NULL;
    char *out = P(1);
    uint32_t size = A(2), o = 0;
    if (!out || !size)
        return 0;
    if (ini_read(S(3), &data) >= 0) {
        int in = 0;
        for (char *p = data; p && *p; ) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            char *s = strip(p);
            if (*s == '[') {
                char *e = strchr(s, ']');
                if (e) *e = 0;
                in = !strcasecmp(s + 1, S(0));
            } else if (in && *s && *s != ';' && o + strlen(s) + 2 < size) {
                strcpy(out + o, s);
                o += (uint32_t)strlen(s) + 1;
            }
            p = nl ? nl + 1 : NULL;
        }
        free(data);
    }
    out[o] = 0;
    return o;
}
DEF(GetPrivateProfileSectionW)
{
    gp_t sec = gstrdup(wtmp(A(0))), file = gstrdup(wtmp(A(3))), out = galloc(A(2) + 2);
    uint32_t b[4] = { sec, out, A(2), file };
    uint32_t r = api_GetPrivateProfileSectionA(b);
    if (A(1) && A(2))
        ansi_to_wide(GP(out), (int)r + 1, P(1), (int)A(2));
    gfree(sec); gfree(file); gfree(out);
    return r;
}
DEF(GetPrivateProfileSectionNamesA)
{
    char *out = P(0);
    uint32_t size = A(1);
    if (!out || !size) return 0;
    uint32_t r = ini_get(S(2), NULL, NULL, NULL, out, size);
    return r;
}
DEF(GetPrivateProfileSectionNamesW)
{
    gp_t file = gstrdup(wtmp(A(2))), out = galloc(A(1) + 2);
    uint32_t b[3] = { out, A(1), file };
    uint32_t r = api_GetPrivateProfileSectionNamesA(b);
    if (A(0) && A(1))
        ansi_to_wide(GP(out), (int)r + 1, P(0), (int)A(1));
    gfree(file); gfree(out);
    return r;
}
DEF(WritePrivateProfileSectionA)
{
    const char *body = S(1);
    if (!A(0) || !A(2))
        FAIL(ERROR_INVALID_PARAMETER);
    ini_set(S(2), S(0), NULL, NULL);
    for (const char *p = body; p && *p; p += strlen(p) + 1) {
        char line[1024];
        snprintf(line, sizeof(line), "%s", p);
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        ini_set(S(2), S(0), line, eq + 1);
    }
    return 1;
}
DEF(GetProfileStringA) { uint32_t b[6] = { A(0), A(1), A(2), A(3), A(4), 0 }; gp_t f = gstrdup("C:\\WINDOWS\\WIN.INI"); b[5] = f; uint32_t r = api_GetPrivateProfileStringA(b); gfree(f); return r; }
DEF(GetProfileIntA) { uint32_t b[4] = { A(0), A(1), A(2), 0 }; gp_t f = gstrdup("C:\\WINDOWS\\WIN.INI"); b[3] = f; uint32_t r = api_GetPrivateProfileIntA(b); gfree(f); return r; }
DEF(WriteProfileStringA) { uint32_t b[4] = { A(0), A(1), A(2), 0 }; gp_t f = gstrdup("C:\\WINDOWS\\WIN.INI"); b[3] = f; uint32_t r = api_WritePrivateProfileStringA(b); gfree(f); return r; }
DEF(GetPrivateProfileStructA) { (void)a; return 0; }
DEF(WritePrivateProfileStructA) { (void)a; return 0; }

/* ---- exceptions ---- */

static void context_from_frame(struct context *c)
{
    memset(c, 0, sizeof(*c));
    c->flags = CONTEXT_FULL;
    c->fs = gate_win_fs;
    c->es = c->ds = c->ss = gate_win_ds;
    c->cs = gate_win_cs;
    c->ebx = api_frame->ebx;
    c->esi = api_frame->esi;
    c->edi = api_frame->edi;
    c->ebp = api_frame->ebp;
    c->eip = api_frame->ret_addr;
    c->esp = api_frame->args;           /* the arguments are still there: the callee's business */
    c->eflags = 0x202;
}
DEF(RaiseException)
{
    struct exception_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.code = A(0);
    rec.flags = A(1) & EXCEPTION_NONCONTINUABLE;
    rec.address = api_frame->ret_addr;
    rec.nparams = A(2) > EXCEPTION_MAXIMUM_PARAMETERS ? EXCEPTION_MAXIMUM_PARAMETERS : A(2);
    if (A(3) && rec.nparams)
        memcpy(rec.params, P(3), rec.nparams * 4);
    struct context ctx;
    context_from_frame(&ctx);
    ctx.esp = api_frame->args + 16;     /* RaiseException is stdcall with 4 arguments */
    wtrace("RaiseException %08x at %08x", rec.code, rec.address);
    seh_raise(&rec, &ctx);
    return 0;
}
DEF(RtlUnwind)
{
    struct context ctx;
    context_from_frame(&ctx);
    ctx.esp = api_frame->args + 16;
    struct exception_record *rec = A(2) ? P(2) : NULL;
    seh_unwind(A(0), A(1), rec, A(3), &ctx);
    return 0;
}
DEF(SetUnhandledExceptionFilter) { gp_t old = unhandled_filter; unhandled_filter = A(0); return old; }
DEF(UnhandledExceptionFilter)
{
    uint32_t *ptrs = P(0);
    if (unhandled_filter) {
        uint32_t r = call_guest1(unhandled_filter, A(0));
        if (r == (uint32_t)-1 || r == 1)
            return r;
    }
    if (ptrs && ptrs[0]) {
        struct exception_record *rec = GP(ptrs[0]);
        wlog("unhandled exception 0x%08x at 0x%08x", rec->code, rec->address);
    }
    return 1;                           /* EXCEPTION_EXECUTE_HANDLER */
}
DEF(AddVectoredExceptionHandler)
{
    if (nvectored >= 8)
        return 0;
    if (A(0)) {
        memmove(vectored_handlers + 1, vectored_handlers, (size_t)nvectored * sizeof(gp_t));
        vectored_handlers[0] = A(1);
    } else {
        vectored_handlers[nvectored] = A(1);
    }
    nvectored++;
    return A(1);
}
DEF(RemoveVectoredExceptionHandler)
{
    for (int i = 0; i < nvectored; i++)
        if (vectored_handlers[i] == A(0)) {
            memmove(vectored_handlers + i, vectored_handlers + i + 1, (size_t)(nvectored - i - 1) * sizeof(gp_t));
            nvectored--;
            return 1;
        }
    return 0;
}
DEF(AddVectoredContinueHandler) { (void)a; return 1; }
DEF(RemoveVectoredContinueHandler) { (void)a; return 1; }
DEF(RtlCaptureContext)
{
    struct context *c = P(0);
    context_from_frame(c);
    return 0;
}
DEF(RtlCaptureStackBackTrace) { (void)a; return 0; }
DEF(RtlPcToFileHeader) { (void)a; return 0; }
DEF(RtlLookupFunctionEntry) { (void)a; return 0; }
DEF(RtlVirtualUnwind) { (void)a; return 0; }
DEF(RtlFillMemory) { memset(P(0), (int)A(2), A(1)); return 0; }
DEF(RtlZeroMemory) { memset(P(0), 0, A(1)); return 0; }
DEF(RtlMoveMemory) { memmove(P(0), P(1), A(2)); return 0; }
DEF(RtlCopyMemory) { memcpy(P(0), P(1), A(2)); return 0; }
DEF(RtlCompareMemory) { const uint8_t *x = P(0), *y = P(1); uint32_t i; for (i = 0; i < A(2) && x[i] == y[i]; i++) ; return i; }
DEF(EncodePointer) { return A(0) ^ 0x5A5A5A5Au; }
DEF(DecodePointer) { return A(0) ^ 0x5A5A5A5Au; }
DEF(EncodeSystemPointer) { return A(0) ^ 0x5A5A5A5Au; }
DEF(DecodeSystemPointer) { return A(0) ^ 0x5A5A5A5Au; }
DEF(ProcessIdToSessionId) { if (A(1)) *(uint32_t *)P(1) = 0; return 1; }
DEF(WTSGetActiveConsoleSessionId) { (void)a; return 0; }
DEF(SetProcessDPIAware) { (void)a; return 1; }
DEF(GetCommProperties) { (void)a; FAIL(ERROR_INVALID_HANDLE); }
DEF(GetCommState) { (void)a; FAIL(ERROR_INVALID_HANDLE); }
DEF(SetCommState) { (void)a; FAIL(ERROR_INVALID_HANDLE); }
DEF(GetProfileSectionA) { (void)a; return 0; }
DEF(GetSystemPowerStatus) { uint8_t *s = P(0); memset(s, 0, 12); s[0] = 1; s[1] = 128; s[2] = 255; *(uint32_t *)(s + 4) = 0xFFFFFFFFu; *(uint32_t *)(s + 8) = 0xFFFFFFFFu; return 1; }
DEF(SetThreadExecutionState) { (void)a; return 1; }
DEF(GetSystemFirmwareTable) { (void)a; FAIL(ERROR_INVALID_FUNCTION); }
DEF(GetProductInfo) { if (A(4)) *(uint32_t *)P(4) = 1; return 1; }
DEF(GetProcessMitigationPolicy) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(SetProcessMitigationPolicy) { (void)a; return 1; }
DEF(K32GetModuleFileNameExA) { uint32_t b[3] = { A(1), A(2), A(3) }; return api_GetModuleFileNameA(b); }
DEF(K32GetModuleFileNameExW) { uint32_t b[3] = { A(1), A(2), A(3) }; return api_GetModuleFileNameW(b); }
DEF(K32EnumProcesses) { if (A(1) >= 4) { *(uint32_t *)P(0) = teb->process_id; *(uint32_t *)P(2) = 4; } return 1; }
DEF(K32GetProcessMemoryInfo) { memset(P(1), 0, A(2)); *(uint32_t *)P(1) = A(2); return 1; }
DEF(K32EnumProcessModules)
{
    uint32_t *out = P(1), n = 0, cap = A(2) / 4;
    for (struct module *m = modules; m; m = m->next, n++)
        if (n < cap) out[n] = m->base;
    *(uint32_t *)P(3) = n * 4;
    return 1;
}
DEF(K32GetModuleBaseNameA)
{
    struct module *m = A(1) ? module_by_base(A(1)) : main_module;
    if (!m) FAIL(ERROR_INVALID_HANDLE);
    snprintf(P(2), A(3), "%s", m->name);
    return (uint32_t)strlen(P(2));
}
DEF(K32GetModuleInformation)
{
    struct module *m = module_by_base(A(1));
    if (!m) FAIL(ERROR_INVALID_HANDLE);
    uint32_t *mi = P(2);
    mi[0] = m->base; mi[1] = m->size; mi[2] = m->entry;
    return 1;
}
DEF(GetApplicationRestartSettings) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(RegisterApplicationRestart) { (void)a; return 0; }
DEF(GetProcessGroupAffinity) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(GetActiveProcessorCount) { (void)a; return (uint32_t)os_ncpus(); }
DEF(GetMaximumProcessorCount) { (void)a; return (uint32_t)os_ncpus(); }
DEF(GetActiveProcessorGroupCount) { (void)a; return 1; }
DEF(PowerCreateRequest) { (void)a; return handle_new(H_NONE); }
DEF(PowerSetRequest) { (void)a; return 1; }
DEF(PowerClearRequest) { (void)a; return 1; }
DEF(CreateIoCompletionPort) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(GetQueuedCompletionStatus) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(PostQueuedCompletionStatus) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(CreateToolhelp32Snapshot) { (void)a; return handle_new(H_SNAPSHOT); }
DEF(Process32First) { (void)a; FAIL(ERROR_NO_MORE_FILES); }
DEF(Process32Next) { (void)a; FAIL(ERROR_NO_MORE_FILES); }
DEF(Module32First)
{
    /* MODULEENTRY32: dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage, modBaseAddr, modBaseSize, hModule, szModule[256], szExePath[260] */
    uint32_t *me = P(1);
    struct module *m = modules;
    if (!m || me[0] < 548) FAIL(ERROR_NO_MORE_FILES);
    struct hobj *o = handle_get_type(A(0), H_SNAPSHOT);
    if (o) o->count = 1;
    me[1] = 1; me[2] = teb->process_id; me[3] = me[4] = 1; me[5] = m->base; me[6] = m->size; me[7] = m->base;
    snprintf((char *)&me[8], 256, "%s", m->name);
    snprintf((char *)&me[8] + 256, 260, "%s", m->winpath);
    return 1;
}
DEF(Module32Next)
{
    struct hobj *o = handle_get_type(A(0), H_SNAPSHOT);
    uint32_t *me = P(1);
    if (!o || me[0] < 548) FAIL(ERROR_NO_MORE_FILES);
    struct module *m = modules;
    for (int i = 0; i < o->count && m; i++) m = m->next;
    if (!m) FAIL(ERROR_NO_MORE_FILES);
    o->count++;
    me[1] = 1; me[2] = teb->process_id; me[3] = me[4] = 1; me[5] = m->base; me[6] = m->size; me[7] = m->base;
    snprintf((char *)&me[8], 256, "%s", m->name);
    snprintf((char *)&me[8] + 256, 260, "%s", m->winpath);
    return 1;
}
DEF(Thread32First) { (void)a; FAIL(ERROR_NO_MORE_FILES); }
DEF(Thread32Next) { (void)a; FAIL(ERROR_NO_MORE_FILES); }
DEF(Heap32ListFirst) { (void)a; FAIL(ERROR_NO_MORE_FILES); }
DEF(GetFirmwareEnvironmentVariableA) { (void)a; FAIL(ERROR_INVALID_FUNCTION); }
DEF(GetCurrentPackageId) { (void)a; return 15700; }  /* APPMODEL_ERROR_NO_PACKAGE */
DEF(GetCurrentPackageFullName) { (void)a; return 15700; }
DEF(ConvertFiberToThread) { (void)a; return 1; }
DEF(ConvertThreadToFiber) { (void)a; return 0; }
DEF(CreateFiber) { (void)a; return 0; }
DEF(SwitchToFiber) { (void)a; return 0; }
DEF(DeleteFiber) { (void)a; return 0; }
DEF(IsThreadAFiber) { (void)a; return 0; }
DEF(GetThreadIOPendingFlag) { *(uint32_t *)P(1) = 0; return 1; }
DEF(SetThreadStackGuarantee) { (void)a; return 1; }
DEF(GetThreadInformation) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(SetThreadInformation) { (void)a; return 1; }
DEF(GetProcessInformation) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(SetProcessInformation) { (void)a; return 1; }
DEF(GetProcessPriorityBoost) { *(uint32_t *)P(1) = 0; return 1; }
DEF(SetProcessPriorityBoost) { (void)a; return 1; }
DEF(GetThreadDescription) { (void)a; return 0; }
DEF(SetThreadDescription) { (void)a; return 0; }
DEF(GlobalGetAtomNameA) { (void)a; return 0; }
DEF(GlobalAddAtomA) { (void)a; return 0xC000 + (uint16_t)(os_ticks_ms() & 0xFFF); }
DEF(GlobalAddAtomW) { (void)a; return 0xC000 + (uint16_t)(os_ticks_ms() & 0xFFF); }
DEF(GlobalFindAtomA) { (void)a; return 0; }
DEF(GlobalFindAtomW) { (void)a; return 0; }
DEF(GlobalDeleteAtom) { (void)a; return 0; }
DEF(AddAtomA) { (void)a; return 0xC000; }
DEF(FindAtomA) { (void)a; return 0; }
DEF(DeleteAtom) { (void)a; return 0; }
DEF(GetAtomNameA) { (void)a; return 0; }
DEF(InitAtomTable) { (void)a; return 1; }
DEF(MulDiv)
{
    int64_t r = (int64_t)(int32_t)A(0) * (int32_t)A(1);
    int32_t d = (int32_t)A(2);
    if (!d)
        return 0xFFFFFFFFu;
    r = r >= 0 ? (r + d / 2) / d : (r - d / 2) / d;
    if (d < 0) { r = (int64_t)(int32_t)A(0) * (int32_t)A(1); r = (r - d / 2) / d; }
    return (uint32_t)(int32_t)r;
}
DEF(Wow64DisableWow64FsRedirection) { (void)a; return 1; }
DEF(Wow64RevertWow64FsRedirection) { (void)a; return 1; }
DEF(Wow64EnableWow64FsRedirection) { (void)a; return 1; }
DEF(GetNumberOfConsoleFonts) { (void)a; return 1; }
DEF(RaiseFailFastException) { (void)a; process_exit(0xC0000409u); }
DEF(FatalAppExitA) { wlog("FatalAppExit: %s", S(1) ? S(1) : ""); process_exit(1); }
DEF(FatalAppExitW) { wlog("FatalAppExit: %s", A(1) ? wtmp(A(1)) : ""); process_exit(1); }
DEF(FatalExit) { process_exit(A(0)); }
DEF(CreateHardLinkA) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(CreateSymbolicLinkA) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(GetFinalPathNameByHandleA)
{
    struct hobj *o = handle_get(A(0));
    if (!o || !o->path) FAIL(ERROR_INVALID_HANDLE);
    char win[MAX_PATH];
    path_to_win(o->path, win, sizeof(win));
    uint32_t n = (uint32_t)strlen(win);
    if (A(2) <= n) return n + 1;
    strcpy(P(1), win);
    return n;
}
DEF(GetFinalPathNameByHandleW)
{
    gp_t out = galloc(MAX_PATH + 1);
    uint32_t b[4] = { A(0), out, MAX_PATH, A(3) };
    uint32_t r = api_GetFinalPathNameByHandleA(b);
    if (r && r < A(2) && A(1)) ansi_to_wide(GP(out), -1, P(1), (int)A(2));
    gfree(out);
    return r;
}
DEF(GetFileMUIPath) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(BaseFlushAppcompatCache) { (void)a; return 1; }
DEF(GetVolumeNameForVolumeMountPointA) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(GetVolumeNameForVolumeMountPointW) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(SetFileValidData) { (void)a; return 1; }
DEF(SetFileInformationByHandle) { (void)a; return 1; }
DEF(GetFileBandwidthReservation) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(ReOpenFile) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(OpenJobObjectA) { (void)a; FAIL(ERROR_NOT_SUPPORTED); }
DEF(CreateJobObjectA) { (void)a; return handle_new(H_NONE); }
DEF(CreateJobObjectW) { (void)a; return handle_new(H_NONE); }
DEF(AssignProcessToJobObject) { (void)a; return 1; }
DEF(SetInformationJobObject) { (void)a; return 1; }
DEF(IsProcessInJob) { *(uint32_t *)P(2) = 0; return 1; }
DEF(GetSystemDEPPolicy) { (void)a; return 0; }

/* ---- the export table ---- */

static const struct api_entry kernel32_entries[] = {
    API(GetLastError, 0), API(SetLastError, 1), API(RestoreLastError, 1), API(GetErrorMode, 0), API(SetErrorMode, 1),
    API(SetThreadErrorMode, 2), API(Beep, 2), API(IsDebuggerPresent, 0), API(CheckRemoteDebuggerPresent, 2),
    API(DebugBreak, 0), API(OutputDebugStringA, 1), API(OutputDebugStringW, 1),
    API(GetCurrentProcess, 0), API(GetCurrentThread, 0), API(GetCurrentProcessId, 0), API(GetCurrentThreadId, 0),
    API(GetProcessId, 1), API(GetThreadId, 1), API(ExitProcess, 1), API(ExitThread, 1), API(TerminateProcess, 2),
    API(TerminateThread, 2), API(GetExitCodeProcess, 2), API(GetExitCodeThread, 2), API(GetProcessVersion, 1),
    API(GetPriorityClass, 1), API(SetPriorityClass, 2), API(GetThreadPriority, 1), API(SetThreadPriority, 2),
    API(SetThreadPriorityBoost, 2), API(GetProcessAffinityMask, 3), API(SetProcessAffinityMask, 2),
    API(SetThreadAffinityMask, 2), API(SetThreadIdealProcessor, 2), API(GetCurrentProcessorNumber, 0),
    API(SwitchToThread, 0), API(GetProcessTimes, 5), API(GetThreadTimes, 5), API(GetProcessHandleCount, 2),
    API(GetProcessIoCounters, 2), API(SetProcessWorkingSetSize, 3), API(GetProcessWorkingSetSize, 3),
    API(GetProcessShutdownParameters, 2), API(SetProcessShutdownParameters, 2), API(SetProcessDEPPolicy, 1),
    API(GetProcessDEPPolicy, 3), API(IsWow64Process, 2), API(GetStartupInfoA, 1), API(GetStartupInfoW, 1),
    API(GetCommandLineA, 0), API(GetCommandLineW, 0),
    API(CreateProcessA, 10), API(CreateProcessW, 10), API(WinExec, 2), API(OpenProcess, 3), API(CreateThread, 6),
    API(CreateRemoteThread, 7), API(ResumeThread, 1), API(SuspendThread, 1), API(OpenThread, 3),
    API(GetThreadContext, 2), API(SetThreadContext, 2), API(FlushInstructionCache, 3), API(GetThreadSelectorEntry, 3),
    API(GetThreadLocale, 0), API(SetThreadLocale, 1), API(GetThreadUILanguage, 0), API(SetThreadUILanguage, 1),
    API(GetUserDefaultUILanguage, 0), API(GetSystemDefaultUILanguage, 0), API(GetUserDefaultLangID, 0),
    API(GetSystemDefaultLangID, 0), API(GetUserDefaultLCID, 0), API(GetSystemDefaultLCID, 0),
    API(ConvertDefaultLocale, 1), API(IsValidLocale, 2), API(IsValidCodePage, 1), API(GetACP, 0), API(GetOEMCP, 0),
    API(GetConsoleCP, 0), API(GetConsoleOutputCP, 0), API(SetConsoleCP, 1), API(SetConsoleOutputCP, 1),
    API(GetCPInfo, 2), API(GetCPInfoExA, 3), API(GetCPInfoExW, 3), API(IsDBCSLeadByte, 1), API(IsDBCSLeadByteEx, 2),
    API(GetEnvironmentStrings, 0), API(GetEnvironmentStringsW, 0), ALIAS(GetEnvironmentStringsA, GetEnvironmentStrings),
    API(FreeEnvironmentStringsA, 1), API(FreeEnvironmentStringsW, 1), API(GetEnvironmentVariableA, 3),
    API(GetEnvironmentVariableW, 3), API(SetEnvironmentVariableA, 2), API(SetEnvironmentVariableW, 2),
    API(ExpandEnvironmentStringsA, 3), API(ExpandEnvironmentStringsW, 3), API(GetComputerNameA, 2),
    API(GetComputerNameW, 2), API(GetComputerNameExA, 3), API(GetComputerNameExW, 3),
    API(GetVersion, 0), API(GetVersionExA, 1), API(GetVersionExW, 1), API(VerSetConditionMask, 4),
    API(VerifyVersionInfoA, 4), API(VerifyVersionInfoW, 4), API(GetSystemInfo, 1), API(GetNativeSystemInfo, 1),
    API(IsProcessorFeaturePresent, 1), API(GetLogicalProcessorInformation, 2), API(GetLogicalProcessorInformationEx, 3),
    API(GetNumaHighestNodeNumber, 1), API(GetSystemDirectoryA, 2), API(GetSystemDirectoryW, 2),
    API(GetWindowsDirectoryA, 2), API(GetWindowsDirectoryW, 2), API(GetSystemWindowsDirectoryA, 2),
    API(GetSystemWindowsDirectoryW, 2), API(GetSystemWow64DirectoryA, 2), API(GetSystemWow64DirectoryW, 2),
    API(GetTempPathA, 2), API(GetTempPathW, 2), API(GetTempFileNameA, 4), API(GetTempFileNameW, 4),
    API(GetTickCount, 0), API(GetTickCount64, 0), API(GetSystemTimeAsFileTime, 1), API(GetSystemTimePreciseAsFileTime, 1),
    API(GetSystemTime, 1), API(GetLocalTime, 1), API(SetLocalTime, 1), API(SetSystemTime, 1),
    API(FileTimeToSystemTime, 2), API(SystemTimeToFileTime, 2), API(FileTimeToLocalFileTime, 2),
    API(LocalFileTimeToFileTime, 2), API(SystemTimeToTzSpecificLocalTime, 3), API(TzSpecificLocalTimeToSystemTime, 3),
    API(CompareFileTime, 2), API(FileTimeToDosDateTime, 3), API(DosDateTimeToFileTime, 3),
    API(GetTimeZoneInformation, 1), API(SetTimeZoneInformation, 1), API(GetDynamicTimeZoneInformation, 1),
    API(QueryPerformanceCounter, 1), API(QueryPerformanceFrequency, 1), API(Sleep, 1), API(SleepEx, 2),
    API(GetSystemTimes, 3),
    API(VirtualAlloc, 4), API(VirtualAllocEx, 5), API(VirtualFree, 3), API(VirtualFreeEx, 4), API(VirtualProtect, 4),
    API(VirtualProtectEx, 5), API(VirtualQuery, 3), API(VirtualQueryEx, 4), API(VirtualLock, 2), API(VirtualUnlock, 2),
    API(IsBadReadPtr, 2), API(IsBadWritePtr, 2), API(IsBadCodePtr, 1), API(IsBadHugeReadPtr, 2), API(IsBadHugeWritePtr, 2),
    API(IsBadStringPtrA, 2), API(IsBadStringPtrW, 2), API(ReadProcessMemory, 5), API(WriteProcessMemory, 5),
    API(GlobalMemoryStatus, 1), API(GlobalMemoryStatusEx, 1), API(GetProcessHeap, 0), API(GetProcessHeaps, 2),
    API(HeapCreate, 3), API(HeapDestroy, 1), API(HeapAlloc, 3), API(HeapFree, 3), API(HeapReAlloc, 4), API(HeapSize, 3),
    API(HeapValidate, 3), API(HeapCompact, 2), API(HeapLock, 1), API(HeapUnlock, 1), API(HeapSetInformation, 4),
    API(HeapQueryInformation, 5), API(HeapWalk, 2),
    API(GlobalAlloc, 2), API(GlobalFree, 1), API(GlobalLock, 1), API(GlobalUnlock, 1), API(GlobalHandle, 1),
    API(GlobalSize, 1), API(GlobalFlags, 1), API(GlobalReAlloc, 3), API(GlobalCompact, 1), API(GlobalFix, 1),
    API(GlobalUnfix, 1), API(GlobalWire, 1), API(GlobalUnWire, 1), API(LocalAlloc, 2), API(LocalFree, 1),
    API(LocalLock, 1), API(LocalUnlock, 1), API(LocalSize, 1), API(LocalFlags, 1), API(LocalReAlloc, 3),
    API(LocalHandle, 1), API(LocalCompact, 1),
    API(InterlockedIncrement, 1), API(InterlockedDecrement, 1), API(InterlockedExchange, 2),
    API(InterlockedExchangeAdd, 2), API(InterlockedCompareExchange, 3), API(InterlockedCompareExchange64, 5),
    API(InterlockedFlushSList, 1), API(InitializeSListHead, 1), API(InterlockedPushEntrySList, 2),
    API(InterlockedPopEntrySList, 1), API(QueryDepthSList, 1),
    API(InitializeCriticalSection, 1), API(InitializeCriticalSectionAndSpinCount, 2),
    API(InitializeCriticalSectionEx, 3), API(SetCriticalSectionSpinCount, 2), API(DeleteCriticalSection, 1),
    API(EnterCriticalSection, 1), API(TryEnterCriticalSection, 1), API(LeaveCriticalSection, 1),
    API(InitializeSRWLock, 1), API(AcquireSRWLockExclusive, 1), API(ReleaseSRWLockExclusive, 1),
    API(AcquireSRWLockShared, 1), API(ReleaseSRWLockShared, 1), API(TryAcquireSRWLockExclusive, 1),
    API(TryAcquireSRWLockShared, 1), API(InitializeConditionVariable, 1), API(WakeConditionVariable, 1),
    API(WakeAllConditionVariable, 1), API(SleepConditionVariableCS, 3), API(SleepConditionVariableSRW, 4),
    API(InitOnceExecuteOnce, 4), API(InitOnceInitialize, 1), API(InitOnceBeginInitialize, 4), API(InitOnceComplete, 3),
    API(CreateEventA, 4), API(CreateEventW, 4), API(CreateEventExA, 4), API(CreateEventExW, 4), API(OpenEventA, 3),
    API(OpenEventW, 3), API(SetEvent, 1), API(ResetEvent, 1), API(PulseEvent, 1), API(CreateMutexA, 3),
    API(CreateMutexW, 3), API(CreateMutexExA, 4), API(CreateMutexExW, 4), API(OpenMutexA, 3), API(OpenMutexW, 3),
    API(ReleaseMutex, 1), API(CreateSemaphoreA, 4), API(CreateSemaphoreW, 4), API(CreateSemaphoreExA, 6),
    API(CreateSemaphoreExW, 6), API(ReleaseSemaphore, 3), API(CreateWaitableTimerA, 3), API(CreateWaitableTimerW, 3),
    API(SetWaitableTimer, 6), API(CancelWaitableTimer, 1), API(WaitForSingleObject, 2), API(WaitForSingleObjectEx, 3),
    API(WaitForMultipleObjects, 4), API(WaitForMultipleObjectsEx, 5), API(SignalObjectAndWait, 4),
    API(RegisterWaitForSingleObject, 6), API(UnregisterWait, 1), API(QueueUserAPC, 3), API(QueueUserWorkItem, 3),
    API(CreateTimerQueue, 0), API(CreateTimerQueueTimer, 7), API(DeleteTimerQueueTimer, 3), API(DeleteTimerQueueEx, 2),
    API(DeleteTimerQueue, 1),
    API(TlsAlloc, 0), API(TlsFree, 1), API(TlsGetValue, 1), API(TlsSetValue, 2), API(FlsAlloc, 1), API(FlsFree, 1),
    API(FlsGetValue, 1), API(FlsSetValue, 2),
    API(GetModuleHandleA, 1), API(GetModuleHandleW, 1), API(GetModuleHandleExA, 3), API(GetModuleHandleExW, 3),
    API(GetModuleFileNameA, 3), API(GetModuleFileNameW, 3), API(LoadLibraryA, 1), API(LoadLibraryW, 1),
    API(LoadLibraryExA, 3), API(LoadLibraryExW, 3), API(FreeLibrary, 1), API(FreeLibraryAndExitThread, 2),
    API(DisableThreadLibraryCalls, 1), API(GetProcAddress, 2), API(SetDllDirectoryA, 1), API(SetDllDirectoryW, 1),
    API(GetDllDirectoryA, 2), API(GetDllDirectoryW, 2), API(SetDefaultDllDirectories, 1), API(AddDllDirectory, 1),
    API(RemoveDllDirectory, 1), API(SetSearchPathMode, 1),
    API(FindResourceA, 3), API(FindResourceW, 3), API(FindResourceExA, 4), API(FindResourceExW, 4),
    API(LoadResource, 2), API(LockResource, 1), API(FreeResource, 1), API(SizeofResource, 2),
    API(EnumResourceNamesA, 4), API(EnumResourceNamesW, 4), API(EnumResourceTypesA, 3), API(EnumResourceTypesW, 3),
    API(EnumResourceLanguagesA, 5), API(EnumResourceLanguagesW, 5), API(BeginUpdateResourceA, 2),
    API(BeginUpdateResourceW, 2),
    API(CloseHandle, 1), API(DuplicateHandle, 7), API(GetHandleInformation, 2), API(SetHandleInformation, 3),
    API(SetHandleCount, 1), API(GetStdHandle, 1), API(SetStdHandle, 2),
    API(CreateFileA, 7), API(CreateFileW, 7), API(OpenFile, 3), API(_lopen, 2), API(_lcreat, 2), API(_lclose, 1),
    API(ReadFile, 5), API(ReadFileEx, 5), API(WriteFile, 5), API(WriteFileEx, 5), API(_lread, 3), API(_lwrite, 3),
    API(_hread, 3), API(_hwrite, 3), API(_llseek, 3), API(SetFilePointer, 4), API(SetFilePointerEx, 5),
    API(GetFileSize, 2), API(GetFileSizeEx, 2), API(GetFileType, 1), API(FlushFileBuffers, 1), API(SetEndOfFile, 1),
    API(LockFile, 5), API(UnlockFile, 5), API(LockFileEx, 6), API(UnlockFileEx, 5), API(GetFileTime, 4),
    API(SetFileTime, 4), API(GetFileInformationByHandle, 2), API(GetFileInformationByHandleEx, 4),
    API(GetFileAttributesA, 1), API(GetFileAttributesW, 1), API(GetFileAttributesExA, 3), API(GetFileAttributesExW, 3),
    API(SetFileAttributesA, 2), API(SetFileAttributesW, 2), API(DeleteFileA, 1), API(DeleteFileW, 1),
    API(MoveFileExA, 3), API(MoveFileA, 2), API(MoveFileW, 2), API(MoveFileExW, 3), API(CopyFileA, 3), API(CopyFileW, 3),
    API(CopyFileExA, 6), API(CopyFileExW, 6), API(ReplaceFileA, 6), API(CreateDirectoryA, 2), API(CreateDirectoryW, 2),
    API(CreateDirectoryExA, 3), API(CreateDirectoryExW, 3), API(RemoveDirectoryA, 1), API(RemoveDirectoryW, 1),
    API(GetCurrentDirectoryA, 2), API(GetCurrentDirectoryW, 2), API(SetCurrentDirectoryA, 1),
    API(SetCurrentDirectoryW, 1), API(GetFullPathNameA, 4), API(GetFullPathNameW, 4), API(GetLongPathNameA, 3),
    API(GetLongPathNameW, 3), API(GetShortPathNameA, 3), API(GetShortPathNameW, 3), API(SearchPathA, 6),
    API(SearchPathW, 6), API(GetDriveTypeA, 1), API(GetDriveTypeW, 1), API(GetLogicalDrives, 0),
    API(GetLogicalDriveStringsA, 2), API(GetLogicalDriveStringsW, 2), API(GetDiskFreeSpaceA, 5),
    API(GetDiskFreeSpaceW, 5), API(GetDiskFreeSpaceExA, 4), API(GetDiskFreeSpaceExW, 4),
    API(GetVolumeInformationA, 8), API(GetVolumeInformationW, 8), API(GetVolumePathNameA, 3),
    API(GetVolumePathNameW, 3), API(DefineDosDeviceA, 3), API(QueryDosDeviceA, 3), API(QueryDosDeviceW, 3),
    API(SetVolumeLabelA, 2), API(GetFileAttributesTransactedA, 4), API(AreFileApisANSI, 0), API(SetFileApisToANSI, 0),
    API(SetFileApisToOEM, 0), API(DeviceIoControl, 8), API(GetOverlappedResult, 4), API(CancelIo, 1),
    API(CreatePipe, 4), API(PeekNamedPipe, 6), API(CreateNamedPipeA, 8), API(ConnectNamedPipe, 2),
    API(DisconnectNamedPipe, 1), API(WaitNamedPipeA, 2), API(SetNamedPipeHandleState, 4), API(TransactNamedPipe, 7),
    API(CallNamedPipeA, 7), API(CreateMailslotA, 4),
    API(FindFirstFileA, 2), API(FindFirstFileW, 2), API(FindFirstFileExA, 6), API(FindFirstFileExW, 6),
    API(FindNextFileA, 2), API(FindNextFileW, 2), API(FindClose, 1), API(FindFirstChangeNotificationA, 3),
    API(FindFirstChangeNotificationW, 3), API(FindNextChangeNotification, 1), API(FindCloseChangeNotification, 1),
    API(ReadDirectoryChangesW, 8), API(FindFirstVolumeA, 2), API(FindVolumeClose, 1),
    API(CreateFileMappingA, 6), API(CreateFileMappingW, 6), API(OpenFileMappingA, 3), API(OpenFileMappingW, 3),
    API(MapViewOfFileEx, 6), API(MapViewOfFile, 5), API(FlushViewOfFile, 2), API(UnmapViewOfFile, 1),
    API(GetConsoleMode, 2), API(SetConsoleMode, 2), API(WriteConsoleA, 5), API(WriteConsoleW, 5), API(ReadConsoleA, 5),
    API(ReadConsoleW, 5), API(GetConsoleScreenBufferInfo, 2), API(GetConsoleScreenBufferInfoEx, 2),
    API(SetConsoleScreenBufferSize, 2), API(SetConsoleWindowInfo, 3), API(GetLargestConsoleWindowSize, 1),
    API(SetConsoleCursorPosition, 2), API(GetConsoleCursorInfo, 2), API(SetConsoleCursorInfo, 2),
    API(SetConsoleTextAttribute, 2), API(FillConsoleOutputCharacterA, 5), API(FillConsoleOutputCharacterW, 5),
    API(FillConsoleOutputAttribute, 5), API(WriteConsoleOutputCharacterA, 5), API(WriteConsoleOutputCharacterW, 5),
    API(WriteConsoleOutputAttribute, 5), API(WriteConsoleOutputA, 5), API(WriteConsoleOutputW, 5),
    API(ReadConsoleOutputA, 5), API(ReadConsoleOutputW, 5), API(ReadConsoleOutputCharacterA, 5),
    API(ReadConsoleOutputAttribute, 5), API(ScrollConsoleScreenBufferA, 5), API(ScrollConsoleScreenBufferW, 5),
    API(SetConsoleTitleA, 1), API(SetConsoleTitleW, 1), API(GetConsoleTitleA, 2), API(GetConsoleTitleW, 2),
    API(AllocConsole, 0), API(FreeConsole, 0), API(AttachConsole, 1), API(GetConsoleWindow, 0),
    API(SetConsoleCtrlHandler, 2), API(GenerateConsoleCtrlEvent, 2), API(GetNumberOfConsoleInputEvents, 2),
    API(GetNumberOfConsoleMouseButtons, 1), API(ReadConsoleInputA, 4), API(ReadConsoleInputW, 4),
    API(PeekConsoleInputA, 4), API(PeekConsoleInputW, 4), API(FlushConsoleInputBuffer, 1), API(WriteConsoleInputA, 4),
    API(SetConsoleActiveScreenBuffer, 1), API(CreateConsoleScreenBuffer, 5), API(GetConsoleFontSize, 2),
    API(GetCurrentConsoleFont, 3), API(SetConsoleIcon, 1), API(GetConsoleDisplayMode, 1),
    API(GetConsoleAliasesLengthA, 1), API(GetConsoleProcessList, 2), API(GetConsoleHistoryInfo, 1),
    API(SetConsoleHistoryInfo, 1), API(GetConsoleSelectionInfo, 1),
    API(lstrlenA, 1), API(lstrlenW, 1), API(lstrcpyA, 2), API(lstrcpyW, 2), API(lstrcpynA, 3), API(lstrcpynW, 3),
    API(lstrcatA, 2), API(lstrcatW, 2), API(lstrcmpA, 2), API(lstrcmpiA, 2), API(lstrcmpW, 2), API(lstrcmpiW, 2),
    ALIAS(lstrlen, lstrlenA), ALIAS(lstrcpy, lstrcpyA), ALIAS(lstrcpyn, lstrcpynA), ALIAS(lstrcat, lstrcatA),
    ALIAS(lstrcmp, lstrcmpA), ALIAS(lstrcmpi, lstrcmpiA),
    API(CompareStringA, 6), API(CompareStringW, 6), API(CompareStringEx, 9), API(CompareStringOrdinal, 5),
    API(CharToOemA, 2), API(OemToCharA, 2), API(MultiByteToWideChar, 6), API(WideCharToMultiByte, 8),
    API(GetStringTypeA, 5), API(GetStringTypeW, 4), API(GetStringTypeExA, 5), API(GetStringTypeExW, 5),
    API(LCMapStringA, 6), API(LCMapStringW, 6), API(LCMapStringEx, 9), API(FoldStringA, 5), API(FoldStringW, 5),
    API(GetLocaleInfoA, 4), API(GetLocaleInfoW, 4), API(GetLocaleInfoEx, 4), API(EnumSystemLocalesA, 2),
    API(EnumSystemLocalesW, 2), API(EnumSystemLocalesEx, 4), API(EnumUILanguagesA, 3),
    API(GetSystemPreferredUILanguages, 4), API(GetUserPreferredUILanguages, 4), API(LocaleNameToLCID, 2),
    API(LCIDToLocaleName, 4), API(GetUserDefaultLocaleName, 2), API(GetSystemDefaultLocaleName, 2),
    API(IsValidLocaleName, 1), API(ResolveLocaleName, 3), API(GetNumberFormatA, 6), API(GetNumberFormatW, 6),
    API(GetCurrencyFormatA, 6), API(GetCurrencyFormatW, 6), API(GetDateFormatA, 6), API(GetDateFormatW, 6),
    API(GetTimeFormatA, 6), API(GetTimeFormatW, 6), API(GetDateFormatEx, 7), API(GetTimeFormatEx, 6),
    API(GetCalendarInfoA, 6), API(GetCalendarInfoW, 6), API(GetGeoInfoA, 5), API(GetUserGeoID, 1),
    API(IsNLSDefinedString, 5), API(NormalizeString, 5), API(IsNormalizedString, 3),
    API(GetThreadPreferredUILanguages, 4),
    API(FormatMessageA, 7), API(FormatMessageW, 7),
    API(GetPrivateProfileStringA, 6), API(GetPrivateProfileStringW, 6), API(GetPrivateProfileIntA, 4),
    API(GetPrivateProfileIntW, 4), API(WritePrivateProfileStringA, 4), API(WritePrivateProfileStringW, 4),
    API(GetPrivateProfileSectionA, 4), API(GetPrivateProfileSectionW, 4), API(GetPrivateProfileSectionNamesA, 3),
    API(GetPrivateProfileSectionNamesW, 3), API(WritePrivateProfileSectionA, 3), API(GetProfileStringA, 5),
    API(GetProfileIntA, 3), API(WriteProfileStringA, 3), API(GetPrivateProfileStructA, 5),
    API(WritePrivateProfileStructA, 5),
    API(RaiseException, 4), API(RtlUnwind, 4), API(SetUnhandledExceptionFilter, 1), API(UnhandledExceptionFilter, 1),
    API(AddVectoredExceptionHandler, 2), API(RemoveVectoredExceptionHandler, 1), API(AddVectoredContinueHandler, 2),
    API(RemoveVectoredContinueHandler, 1), API(RtlCaptureContext, 1), API(RtlCaptureStackBackTrace, 4),
    API(RtlPcToFileHeader, 2), API(RtlLookupFunctionEntry, 3), API(RtlVirtualUnwind, 8), API(RtlFillMemory, 3),
    API(RtlZeroMemory, 2), API(RtlMoveMemory, 3), API(RtlCopyMemory, 3), API(RtlCompareMemory, 3),
    API(EncodePointer, 1), API(DecodePointer, 1), API(EncodeSystemPointer, 1), API(DecodeSystemPointer, 1),
    API(ProcessIdToSessionId, 2), API(WTSGetActiveConsoleSessionId, 0), API(SetProcessDPIAware, 0),
    API(GetCommProperties, 2), API(GetCommState, 2), API(SetCommState, 2), API(GetProfileSectionA, 3),
    API(GetSystemPowerStatus, 1), API(SetThreadExecutionState, 1), API(GetSystemFirmwareTable, 4),
    API(GetProductInfo, 5), API(GetProcessMitigationPolicy, 4), API(SetProcessMitigationPolicy, 3),
    API(K32GetModuleFileNameExA, 4), API(K32GetModuleFileNameExW, 4), API(K32EnumProcesses, 3),
    API(K32GetProcessMemoryInfo, 3), API(K32EnumProcessModules, 4), API(K32GetModuleBaseNameA, 4),
    API(K32GetModuleInformation, 4), API(GetApplicationRestartSettings, 4), API(RegisterApplicationRestart, 2),
    API(GetProcessGroupAffinity, 3), API(GetActiveProcessorCount, 1), API(GetMaximumProcessorCount, 1),
    API(GetActiveProcessorGroupCount, 0), API(PowerCreateRequest, 1), API(PowerSetRequest, 2),
    API(PowerClearRequest, 2), API(CreateIoCompletionPort, 4), API(GetQueuedCompletionStatus, 5),
    API(PostQueuedCompletionStatus, 4), API(CreateToolhelp32Snapshot, 2), API(Process32First, 2),
    API(Process32Next, 2), API(Module32First, 2), API(Module32Next, 2), API(Thread32First, 2), API(Thread32Next, 2),
    API(Heap32ListFirst, 2), API(GetFirmwareEnvironmentVariableA, 4), API(GetCurrentPackageId, 2),
    API(GetCurrentPackageFullName, 2), API(ConvertFiberToThread, 0), API(ConvertThreadToFiber, 1), API(CreateFiber, 3),
    API(SwitchToFiber, 1), API(DeleteFiber, 1), API(IsThreadAFiber, 0), API(GetThreadIOPendingFlag, 2),
    API(SetThreadStackGuarantee, 1), API(GetThreadInformation, 4), API(SetThreadInformation, 4),
    API(GetProcessInformation, 4), API(SetProcessInformation, 4), API(GetProcessPriorityBoost, 2),
    API(SetProcessPriorityBoost, 2), API(GetThreadDescription, 2), API(SetThreadDescription, 2),
    API(GlobalGetAtomNameA, 3), API(GlobalAddAtomA, 1), API(GlobalAddAtomW, 1), API(GlobalFindAtomA, 1),
    API(GlobalFindAtomW, 1), API(GlobalDeleteAtom, 1), API(AddAtomA, 1), API(FindAtomA, 1), API(DeleteAtom, 1),
    API(GetAtomNameA, 3), API(InitAtomTable, 1), API(MulDiv, 3), API(Wow64DisableWow64FsRedirection, 1),
    API(Wow64RevertWow64FsRedirection, 1), API(Wow64EnableWow64FsRedirection, 1), API(GetNumberOfConsoleFonts, 0),
    API(RaiseFailFastException, 3), API(FatalAppExitA, 2), API(FatalAppExitW, 2), API(FatalExit, 1),
    API(CreateHardLinkA, 3), API(CreateSymbolicLinkA, 3), API(GetFinalPathNameByHandleA, 4),
    API(GetFinalPathNameByHandleW, 4), API(GetFileMUIPath, 7), API(BaseFlushAppcompatCache, 0),
    API(GetVolumeNameForVolumeMountPointA, 3), API(GetVolumeNameForVolumeMountPointW, 3), API(SetFileValidData, 3),
    API(SetFileInformationByHandle, 4), API(GetFileBandwidthReservation, 6), API(ReOpenFile, 4),
    API(OpenJobObjectA, 3), API(CreateJobObjectA, 2), API(CreateJobObjectW, 2), API(AssignProcessToJobObject, 2),
    API(SetInformationJobObject, 4), API(IsProcessInJob, 3), API(GetSystemDEPPolicy, 0),
};

struct api_dll kernel32_dll = { "KERNEL32.DLL", kernel32_entries, sizeof(kernel32_entries) / sizeof(kernel32_entries[0]), NULL };
