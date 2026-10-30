/*
 * MSVCRT: the C runtime, as msvcrt.dll (and the msvcr*, ucrtbase and
 * api-ms-win-crt-* DLLs, which api.c maps here) gives it. Part one:
 * startup and exit, errno, memory and strings. Streams are crt_io.c,
 * formatting crt_fmt.c, time crt_time.c; their entry points are here.
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "crt.h"
#include "os.h"

#define A(i) (a[i])
#define S(i) gstr(a[i])
#define WS(i) gwstr(a[i])
#define P(i) gp(a[i])

/* ---- errno ---- */

gp_t crt_errno_gp, crt_doserrno_gp;

void crt_set_errno(int e)
{
    if (crt_errno_gp)
        *(int32_t *)GP(crt_errno_gp) = e;
}

int crt_errno_from_host(int e)
{
    switch (e) {
    case ENOENT: return CRT_ENOENT;
    case EBADF: return CRT_EBADF;
    case ENOMEM: return CRT_ENOMEM;
    case EACCES: case EPERM: case EROFS: return CRT_EACCES;
    case EEXIST: return CRT_EEXIST;
    case EINVAL: return CRT_EINVAL;
    case EMFILE: case ENFILE: return CRT_EMFILE;
    case ENOSPC: return CRT_ENOSPC;
    case ERANGE: return CRT_ERANGE;
    case ENOTDIR: return CRT_ENOTDIR;
    case EISDIR: return CRT_EISDIR;
    case ENOTEMPTY: return 41;
    default: return e > 0 && e < 43 ? e : CRT_EINVAL;
    }
}

XDEF(_errno) { (void)a; return crt_errno_gp; }
XDEF(__doserrno) { (void)a; return crt_doserrno_gp; }
XDEF(_get_errno) { if (!A(0)) return CRT_EINVAL; *(int32_t *)P(0) = *(int32_t *)GP(crt_errno_gp); return 0; }
XDEF(_set_errno) { crt_set_errno((int)A(0)); return 0; }

static const char *const crt_messages[] = {
    "No error", "Operation not permitted", "No such file or directory", "No such process",
    "Interrupted function call", "Input/output error", "No such device or address", "Arg list too long",
    "Exec format error", "Bad file descriptor", "No child processes", "Resource temporarily unavailable",
    "Not enough space", "Permission denied", "Bad address", "Unknown error", "Resource device",
    "File exists", "Improper link", "No such device", "Not a directory", "Is a directory",
    "Invalid argument", "Too many open files in system", "Too many open files",
    "Inappropriate I/O control operation", "Unknown error", "File too large", "No space left on device",
    "Invalid seek", "Read-only file system", "Too many links", "Broken pipe", "Domain error",
    "Result too large", "Unknown error", "Resource deadlock avoided", "Unknown error",
    "Filename too long", "No locks available", "Function not implemented", "Directory not empty",
    "Illegal byte sequence",
};
static const char *crt_message(int e)
{
    return e >= 0 && e < (int)(sizeof(crt_messages) / sizeof(crt_messages[0])) ? crt_messages[e] : "Unknown error";
}
static gp_t strerror_buf;
XDEF(strerror)
{
    if (!strerror_buf) strerror_buf = vm_alloc_loader(128);
    snprintf(GP(strerror_buf), 128, "%s", crt_message((int)A(0)));
    return strerror_buf;
}
XDEF(_strerror)
{
    if (!strerror_buf) strerror_buf = vm_alloc_loader(128);
    int e = *(int32_t *)GP(crt_errno_gp);
    if (A(0)) snprintf(GP(strerror_buf), 128, "%s: %s\n", S(0), crt_message(e));
    else snprintf(GP(strerror_buf), 128, "%s\n", crt_message(e));
    return strerror_buf;
}
XDEF(strerror_s) { if (!A(0) || !A(1)) return CRT_EINVAL; snprintf(P(0), A(1), "%s", crt_message((int)A(2))); return 0; }
XDEF(perror)
{
    crt_flush_all();
    int e = *(int32_t *)GP(crt_errno_gp);
    if (A(0) && *S(0)) fprintf(stderr, "%s: %s\n", S(0), crt_message(e));
    else fprintf(stderr, "%s\n", crt_message(e));
    return 0;
}

/* ---- startup ---- */

static gp_t argc_cell, argv_cell, envp_cell, wargv_cell, wenvp_cell, pgmptr_cell, wpgmptr_cell;
static gp_t fmode_cell, commode_cell, apptype_cell, mb_cur_max_cell, acmdln_cell, wcmdln_cell;
static gp_t environ_cell, wenviron_cell;
static int built_env_gen = -1;

/* Split a Windows command line as the CRT does. */
static int split_args(const char *cmd, char **out, int max)
{
    int n = 0;
    const char *p = cmd;
    static char buf[8192];
    char *b = buf, *end = buf + sizeof(buf) - 1;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        out[n++] = b;
        int quoted = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t')) && b < end) {
            if (*p == '\\') {
                int k = 0;
                while (*p == '\\') { k++; p++; }
                if (*p == '"') {
                    for (int i = 0; i < k / 2 && b < end; i++) *b++ = '\\';
                    if (k & 1) { *b++ = '"'; p++; }
                } else {
                    for (int i = 0; i < k && b < end; i++) *b++ = '\\';
                }
            } else if (*p == '"') {
                if (quoted && p[1] == '"') { *b++ = '"'; p += 2; }
                else { quoted = !quoted; p++; }
            } else {
                *b++ = *p++;
            }
        }
        *b++ = 0;
    }
    return n;
}

static void build_env(void)
{
    if (built_env_gen == env_generation)
        return;
    built_env_gen = env_generation;
    int n = env_count();
    gp_t arr = vm_alloc_loader((uint32_t)(n + 1) * 4), warr = vm_alloc_loader((uint32_t)(n + 1) * 4);
    for (int i = 0; i < n; i++) {
        const char *e = env_nth(i);
        uint32_t len = (uint32_t)strlen(e) + 1;
        gp_t s = vm_alloc_loader(len), w = vm_alloc_loader(len * 2);
        memcpy(GP(s), e, len);
        ansi_to_wide(e, -1, GP(w), (int)len);
        ((gp_t *)GP(arr))[i] = s;
        ((gp_t *)GP(warr))[i] = w;
    }
    ((gp_t *)GP(arr))[n] = 0;
    ((gp_t *)GP(warr))[n] = 0;
    *(gp_t *)GP(envp_cell) = arr;
    *(gp_t *)GP(wenvp_cell) = warr;
    *(gp_t *)GP(environ_cell) = arr;
    *(gp_t *)GP(wenviron_cell) = warr;
}

static void build_args(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    char *argv[512];
    int n = split_args(cmdline, argv, 511);
    gp_t arr = vm_alloc_loader((uint32_t)(n + 1) * 4), warr = vm_alloc_loader((uint32_t)(n + 1) * 4);
    for (int i = 0; i < n; i++) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        gp_t s = vm_alloc_loader(len), w = vm_alloc_loader(len * 2);
        memcpy(GP(s), argv[i], len);
        ansi_to_wide(argv[i], -1, GP(w), (int)len);
        ((gp_t *)GP(arr))[i] = s;
        ((gp_t *)GP(warr))[i] = w;
    }
    ((gp_t *)GP(arr))[n] = 0;
    ((gp_t *)GP(warr))[n] = 0;
    *(int32_t *)GP(argc_cell) = n;
    *(gp_t *)GP(argv_cell) = arr;
    *(gp_t *)GP(wargv_cell) = warr;
    uint32_t plen = (uint32_t)strlen(main_module->winpath) + 1;
    gp_t pg = vm_alloc_loader(plen), wpg = vm_alloc_loader(plen * 2);
    memcpy(GP(pg), main_module->winpath, plen);
    ansi_to_wide(main_module->winpath, -1, GP(wpg), (int)plen);
    *(gp_t *)GP(pgmptr_cell) = pg;
    *(gp_t *)GP(wpgmptr_cell) = wpg;
    build_env();
}

static void msvcrt_init(void)
{
    crt_errno_gp = vm_alloc_loader(4);
    crt_doserrno_gp = vm_alloc_loader(4);
    argc_cell = vm_alloc_loader(4); argv_cell = vm_alloc_loader(4); envp_cell = vm_alloc_loader(4);
    wargv_cell = vm_alloc_loader(4); wenvp_cell = vm_alloc_loader(4);
    pgmptr_cell = vm_alloc_loader(4); wpgmptr_cell = vm_alloc_loader(4);
    fmode_cell = vm_alloc_loader(4); commode_cell = vm_alloc_loader(4); apptype_cell = vm_alloc_loader(4);
    mb_cur_max_cell = vm_alloc_loader(4); acmdln_cell = vm_alloc_loader(4); wcmdln_cell = vm_alloc_loader(4);
    environ_cell = vm_alloc_loader(4); wenviron_cell = vm_alloc_loader(4);
    *(int32_t *)GP(mb_cur_max_cell) = 1;
    *(gp_t *)GP(acmdln_cell) = cmdline_gp;
    *(gp_t *)GP(wcmdln_cell) = cmdline_w_gp;
    crt_io_init();
    crt_tz_init();
    build_args();
}

XDEF(__getmainargs)
{
    build_args();
    build_env();
    *(int32_t *)P(0) = *(int32_t *)GP(argc_cell);
    *(gp_t *)P(1) = *(gp_t *)GP(argv_cell);
    *(gp_t *)P(2) = *(gp_t *)GP(envp_cell);
    return 0;
}
XDEF(__wgetmainargs)
{
    build_args();
    build_env();
    *(int32_t *)P(0) = *(int32_t *)GP(argc_cell);
    *(gp_t *)P(1) = *(gp_t *)GP(wargv_cell);
    *(gp_t *)P(2) = *(gp_t *)GP(wenvp_cell);
    return 0;
}
XDEF(__p___argc) { (void)a; return argc_cell; }
XDEF(__p___argv) { (void)a; return argv_cell; }
XDEF(__p___wargv) { (void)a; return wargv_cell; }
XDEF(__p__environ) { (void)a; build_env(); return environ_cell; }
XDEF(__p__wenviron) { (void)a; build_env(); return wenviron_cell; }
XDEF(__p__pgmptr) { (void)a; return pgmptr_cell; }
XDEF(__p__wpgmptr) { (void)a; return wpgmptr_cell; }
XDEF(__p__fmode) { (void)a; return fmode_cell; }
XDEF(__p__commode) { (void)a; return commode_cell; }
XDEF(__p__acmdln) { (void)a; return acmdln_cell; }
XDEF(__p__wcmdln) { (void)a; return wcmdln_cell; }
XDEF(__p___mb_cur_max) { (void)a; return mb_cur_max_cell; }
XDEF(___mb_cur_max_func) { (void)a; return 1; }
XDEF(_get_pgmptr) { *(gp_t *)P(0) = *(gp_t *)GP(pgmptr_cell); return 0; }
XDEF(_get_wpgmptr) { *(gp_t *)P(0) = *(gp_t *)GP(wpgmptr_cell); return 0; }
XDEF(__set_app_type) { *(int32_t *)GP(apptype_cell) = (int32_t)A(0); return 0; }
XDEF(_set_app_type) { return api___set_app_type(a); }
XDEF(_configure_narrow_argv) { (void)a; build_args(); return 0; }
XDEF(_configure_wide_argv) { (void)a; build_args(); return 0; }
XDEF(_initialize_narrow_environment) { (void)a; build_env(); return 0; }
XDEF(_initialize_wide_environment) { (void)a; build_env(); return 0; }
XDEF(_get_initial_narrow_environment) { (void)a; build_env(); return *(gp_t *)GP(envp_cell); }
XDEF(_get_initial_wide_environment) { (void)a; build_env(); return *(gp_t *)GP(wenvp_cell); }
XDEF(_set_fmode) { *(int32_t *)GP(fmode_cell) = (int32_t)A(0); return 0; }
XDEF(_get_fmode) { *(int32_t *)P(0) = *(int32_t *)GP(fmode_cell); return 0; }
XDEF(_setmode)
{
    struct crt_fd *f = crt_fd_get((int)A(0));
    if (!f) { crt_set_errno(CRT_EBADF); return (uint32_t)-1; }
    int old = f->text ? 0x4000 : 0x8000;
    f->text = (A(1) & 0x8000) == 0;
    for (int i = 0; i < CRT_MAX_STREAMS; i++)
        if (crt_streams[i].used && crt_streams[i].fd == (int)A(0))
            crt_streams[i].text = f->text;
    return (uint32_t)old;
}

/* _initterm(begin, end): call the non-null function pointers between them */
XDEF(_initterm)
{
    for (gp_t p = A(0); p < A(1); p += 4) {
        gp_t fn = *(gp_t *)GP(p);
        if (fn)
            call_guest(fn, 0, NULL);
    }
    return 0;
}
XDEF(_initterm_e)
{
    for (gp_t p = A(0); p < A(1); p += 4) {
        gp_t fn = *(gp_t *)GP(p);
        if (fn) {
            uint32_t r = call_guest(fn, 0, NULL);
            if (r)
                return r;
        }
    }
    return 0;
}

/* ---- exit ---- */

static gp_t atexit_fns[256];
static int natexit;

void crt_run_atexit(void)
{
    while (natexit > 0) {
        gp_t fn = atexit_fns[--natexit];
        call_guest(fn, 0, NULL);
    }
}

XDEF(atexit) { if (natexit >= 256) return (uint32_t)-1; atexit_fns[natexit++] = A(0); return 0; }
XDEF(_onexit) { if (natexit >= 256) return 0; atexit_fns[natexit++] = A(0); return A(0); }
XDEF(__dllonexit) { return api__onexit(a); }
XDEF(_crt_atexit) { return api_atexit(a); }
XDEF(_register_onexit_function) { (void)a[0]; uint32_t b[1] = { A(1) }; return api_atexit(b); }
XDEF(_initialize_onexit_table) { (void)a; return 0; }
XDEF(_execute_onexit_table) { (void)a; return 0; }
XDEF(at_quick_exit) { (void)a; return 0; }

XDEF(exit) { crt_run_atexit(); process_exit(A(0)); }
XDEF(_exit) { process_exit(A(0)); }
XDEF(_Exit) { process_exit(A(0)); }
XDEF(quick_exit) { process_exit(A(0)); }
XDEF(_cexit) { (void)a; crt_run_atexit(); crt_flush_all(); return 0; }
XDEF(_c_exit) { (void)a; crt_flush_all(); return 0; }
XDEF(abort) { (void)a; crt_flush_all(); wlog("abnormal program termination"); process_exit(3); }
XDEF(_amsg_exit) { wlog("runtime error R60%02u", A(0)); process_exit(255); }
XDEF(_assert) { crt_flush_all(); wlog("Assertion failed: %s, file %s, line %u", S(0), S(1), A(2)); process_exit(3); }
XDEF(_wassert) { crt_flush_all(); wlog("Assertion failed: %s, file %s, line %u", wtmp(A(0)), wtmp(A(1)), A(2)); process_exit(3); }
XDEF(_invalid_parameter_noinfo) { (void)a; wlog("invalid parameter passed to a C runtime function"); return 0; }
XDEF(_invalid_parameter_noinfo_noreturn) { (void)a; wlog("invalid parameter passed to a C runtime function"); process_exit(0xC0000417u); }
XDEF(_set_invalid_parameter_handler) { (void)a; return 0; }
XDEF(_get_invalid_parameter_handler) { (void)a; return 0; }
XDEF(_XcptFilter) { (void)a; return 0; }
XDEF(__CppXcptFilter) { (void)a; return 0; }
XDEF(_set_error_mode) { (void)a; return 0; }
XDEF(_set_abort_behavior) { (void)a; return 0; }
XDEF(__setusermatherr) { (void)a; return 0; }
XDEF(_setusermatherr) { (void)a; return 0; }
XDEF(_controlfp) { (void)a; return 0x9001F; }
XDEF(_control87) { (void)a; return 0x9001F; }
XDEF(__control87_2) { if (A(2)) *(uint32_t *)P(2) = 0x9001F; if (A(3)) *(uint32_t *)P(3) = 0x9001F; return 1; }
XDEF(_controlfp_s) { if (A(0)) *(uint32_t *)P(0) = 0x9001F; return 0; }
XDEF(_clearfp) { (void)a; return 0; }
XDEF(_statusfp) { (void)a; return 0; }
XDEF(_fpreset) { (void)a; return 0; }
XDEF(_lock) { (void)a; return 0; }
XDEF(_unlock) { (void)a; return 0; }
XDEF(_lock_file) { (void)a; return 0; }
XDEF(_unlock_file) { (void)a; return 0; }
XDEF(__lconv_init) { (void)a; return 0; }
XDEF(_configthreadlocale) { (void)a; return 0; }
XDEF(_set_new_mode) { (void)a; return 0; }
XDEF(_query_new_mode) { (void)a; return 0; }
XDEF(_crt_debugger_hook) { (void)a; return 0; }
XDEF(__crtSetUnhandledExceptionFilter) { (void)a; return 0; }
XDEF(_seh_filter_exe) { (void)a; return 0; }
XDEF(_seh_filter_dll) { (void)a; return 0; }
XDEF(__iob_func) { (void)a; return crt_iob; }
XDEF(__acrt_iob_func) { return crt_stream_gp((int)A(0)); }
XDEF(signal) { (void)a; return 0; }
XDEF(raise) { if (A(0) == 22) { crt_flush_all(); process_exit(3); } return 0; }
XDEF(getenv)
{
    static gp_t buf;
    static uint32_t cap;
    const char *v = env_get(S(0));
    if (!v) return 0;
    uint32_t n = (uint32_t)strlen(v) + 1;
    if (n > cap) { cap = n < 256 ? 256 : n; buf = vm_alloc_loader(cap); }
    memcpy(GP(buf), v, n);
    return buf;
}
XDEF(_putenv)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", S(0));
    char *eq = strchr(tmp, '=');
    if (!eq) return (uint32_t)-1;
    *eq = 0;
    env_set(tmp, eq[1] ? eq + 1 : NULL);
    return 0;
}
XDEF(_putenv_s) { env_set(S(0), *S(1) ? S(1) : NULL); return 0; }
XDEF(system)
{
    if (!A(0)) return 1;
    crt_flush_all();
    return (uint32_t)system(S(0));
}

/* ---- memory (the process heap) ---- */

XDEF(malloc) { gp_t p = heap_alloc(process_heap, A(0) ? A(0) : 1, 0); if (!p) crt_set_errno(CRT_ENOMEM); return p; }
XDEF(calloc)
{
    uint64_t n = (uint64_t)A(0) * A(1);
    if (n > 0x7FFF0000u) { crt_set_errno(CRT_ENOMEM); return 0; }
    return heap_alloc(process_heap, n ? (uint32_t)n : 1, 1);
}
XDEF(realloc)
{
    if (!A(0)) return heap_alloc(process_heap, A(1) ? A(1) : 1, 0);
    if (!A(1)) { heap_free(process_heap, A(0)); return 0; }
    return heap_realloc(process_heap, A(0), A(1), 0, 0);
}
XDEF(free) { if (A(0)) heap_free(process_heap, A(0)); return 0; }
XDEF(_msize) { return heap_size(process_heap, A(0)); }
XDEF(_expand) { return heap_realloc(process_heap, A(0), A(1), 0, 1); }
XDEF(_recalloc)
{
    uint64_t n = (uint64_t)A(1) * A(2);
    if (n > 0x7FFF0000u) return 0;
    return heap_realloc(process_heap, A(0), (uint32_t)n, 1, 0);
}
/* _aligned_malloc: the real block's address is kept just below the aligned one */
XDEF(_aligned_malloc)
{
    uint32_t align = A(1) < 4 ? 4 : A(1);
    if (align & (align - 1)) { crt_set_errno(CRT_EINVAL); return 0; }
    gp_t raw = heap_alloc(process_heap, A(0) + align + 4, 0);
    if (!raw) return 0;
    gp_t p = (raw + 4 + align - 1) & ~(align - 1);
    ((gp_t *)GP(p))[-1] = raw;
    return p;
}
XDEF(_aligned_free) { if (A(0)) heap_free(process_heap, ((gp_t *)P(0))[-1]); return 0; }
XDEF(_aligned_realloc)
{
    if (!A(0)) { uint32_t b[2] = { A(1), A(2) }; return api__aligned_malloc(b); }
    if (!A(1)) { api__aligned_free(a); return 0; }
    gp_t raw = ((gp_t *)P(0))[-1];
    uint32_t old = heap_size(process_heap, raw) - (A(0) - raw);
    uint32_t b[2] = { A(1), A(2) };
    gp_t q = api__aligned_malloc(b);
    if (!q) return 0;
    memcpy(GP(q), P(0), old < A(1) ? old : A(1));
    api__aligned_free(a);
    return q;
}
XDEF(_get_heap_handle) { (void)a; return process_heap; }
XDEF(_heapchk) { (void)a; return (uint32_t)-2; }    /* _HEAPOK */
XDEF(_heapmin) { (void)a; return 0; }
XDEF(_callnewh) { (void)a; return 0; }
XDEF(new_op) { return api_malloc(a); }      /* operator new */
XDEF(delete_op) { return api_free(a); }     /* operator delete */

/* ---- memory and strings ---- */

XDEF(memcpy) { memmove(P(0), P(1), A(2)); return A(0); }
XDEF(memmove) { memmove(P(0), P(1), A(2)); return A(0); }
XDEF(memset) { memset(P(0), (int)A(1), A(2)); return A(0); }
XDEF(memcmp) { return (uint32_t)memcmp(P(0), P(1), A(2)); }
XDEF(memchr) { void *r = memchr(P(0), (int)A(1), A(2)); return r ? GA(r) : 0; }
XDEF(_memicmp) { const uint8_t *x = P(0), *y = P(1); for (uint32_t i = 0; i < A(2); i++) { int c = tolower(x[i]) - tolower(y[i]); if (c) return (uint32_t)c; } return 0; }
XDEF(memcpy_s) { if (A(3) > A(1)) { if (A(0)) memset(P(0), 0, A(1)); return CRT_ERANGE; } memmove(P(0), P(2), A(3)); return 0; }
XDEF(memmove_s) { if (A(3) > A(1)) return CRT_ERANGE; memmove(P(0), P(2), A(3)); return 0; }
XDEF(strlen) { return (uint32_t)strlen(S(0)); }
XDEF(strnlen) { return (uint32_t)strnlen(S(0), A(1)); }
XDEF(strcpy) { strcpy(S(0), S(1)); return A(0); }
XDEF(strncpy) { strncpy(S(0), S(1), A(2)); return A(0); }
XDEF(strcat) { strcat(S(0), S(1)); return A(0); }
XDEF(strncat) { strncat(S(0), S(1), A(2)); return A(0); }
XDEF(strcmp) { return (uint32_t)strcmp(S(0), S(1)); }
XDEF(strncmp) { return (uint32_t)strncmp(S(0), S(1), A(2)); }
XDEF(_stricmp) { return (uint32_t)strcasecmp(S(0), S(1)); }
XDEF(_strnicmp) { return (uint32_t)strncasecmp(S(0), S(1), A(2)); }
XDEF(strcoll) { return (uint32_t)strcmp(S(0), S(1)); }
XDEF(_stricoll) { return (uint32_t)strcasecmp(S(0), S(1)); }
XDEF(strchr) { char *r = strchr(S(0), (int)A(1)); return r ? GA(r) : 0; }
XDEF(strrchr) { char *r = strrchr(S(0), (int)A(1)); return r ? GA(r) : 0; }
XDEF(strstr) { char *r = strstr(S(0), S(1)); return r ? GA(r) : 0; }
XDEF(strpbrk) { char *r = strpbrk(S(0), S(1)); return r ? GA(r) : 0; }
XDEF(strspn) { return (uint32_t)strspn(S(0), S(1)); }
XDEF(strcspn) { return (uint32_t)strcspn(S(0), S(1)); }
XDEF(_strdup) { if (!A(0)) return 0; return gstrdup(S(0)); }
XDEF(_strlwr) { str_lower(S(0)); return A(0); }
XDEF(_strupr) { str_upper(S(0)); return A(0); }
XDEF(_strrev) { char *s = S(0); size_t n = strlen(s); for (size_t i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; } return A(0); }
XDEF(_strset) { char *s = S(0); for (; *s; s++) *s = (char)A(1); return A(0); }
XDEF(_strnset) { char *s = S(0); for (uint32_t i = 0; *s && i < A(2); s++, i++) *s = (char)A(1); return A(0); }
XDEF(strtok)
{
    static gp_t save;
    gp_t s = A(0) ? A(0) : save;
    if (!s) return 0;
    char *p = GP(s);
    p += strspn(p, S(1));
    if (!*p) { save = 0; return 0; }
    char *e = p + strcspn(p, S(1));
    if (*e) { *e = 0; save = GA(e + 1); } else save = 0;
    return GA(p);
}
XDEF(strtok_s)
{
    gp_t *ctx = P(2);
    gp_t s = A(0) ? A(0) : *ctx;
    if (!s) return 0;
    char *p = GP(s);
    p += strspn(p, S(1));
    if (!*p) { *ctx = 0; return 0; }
    char *e = p + strcspn(p, S(1));
    if (*e) { *e = 0; *ctx = GA(e + 1); } else *ctx = GA(e);
    return GA(p);
}
XDEF(strcpy_s) { if (!A(0) || !A(2) || strlen(S(2)) >= A(1)) { if (A(0) && A(1)) *S(0) = 0; return CRT_ERANGE; } strcpy(S(0), S(2)); return 0; }
XDEF(strcat_s) { if (!A(0) || !A(2) || strlen(S(0)) + strlen(S(2)) >= A(1)) return CRT_ERANGE; strcat(S(0), S(2)); return 0; }
XDEF(strncpy_s)
{
    /* (dst, size, src, count) */
    if (!A(0) || !A(1)) return CRT_EINVAL;
    size_t n = strnlen(S(2), A(3) == 0xFFFFFFFFu ? A(1) - 1 : A(3));
    if (n >= A(1)) { *S(0) = 0; return CRT_ERANGE; }
    memcpy(S(0), S(2), n);
    S(0)[n] = 0;
    return 0;
}
XDEF(strncat_s)
{
    size_t have = strlen(S(0)), n = strnlen(S(2), A(3) == 0xFFFFFFFFu ? 0xFFFFFFu : A(3));
    if (have + n >= A(1)) return CRT_ERANGE;
    memcpy(S(0) + have, S(2), n);
    S(0)[have + n] = 0;
    return 0;
}

/* wide strings */
XDEF(wcslen) { return (uint32_t)wstrlen(WS(0)); }
XDEF(wcsnlen) { const uint16_t *s = WS(0); uint32_t n = 0; while (n < A(1) && s[n]) n++; return n; }
XDEF(wcscpy) { wstrcpy(WS(0), WS(1)); return A(0); }
XDEF(wcsncpy) { uint16_t *d = WS(0); const uint16_t *s = WS(1); uint32_t i = 0; for (; i < A(2) && s[i]; i++) d[i] = s[i]; for (; i < A(2); i++) d[i] = 0; return A(0); }
XDEF(wcscat) { wstrcpy(WS(0) + wstrlen(WS(0)), WS(1)); return A(0); }
XDEF(wcsncat) { uint16_t *d = WS(0) + wstrlen(WS(0)); const uint16_t *s = WS(1); uint32_t i = 0; for (; i < A(2) && s[i]; i++) d[i] = s[i]; d[i] = 0; return A(0); }
XDEF(wcscmp) { return (uint32_t)wstrcmp(WS(0), WS(1)); }
XDEF(wcsncmp) { const uint16_t *x = WS(0), *y = WS(1); for (uint32_t i = 0; i < A(2); i++) { if (x[i] != y[i]) return (uint32_t)((int)x[i] - (int)y[i]); if (!x[i]) break; } return 0; }
XDEF(_wcsicmp) { return (uint32_t)wstrcasecmp(WS(0), WS(1)); }
XDEF(_wcsnicmp) { const uint16_t *x = WS(0), *y = WS(1); for (uint32_t i = 0; i < A(2); i++) { int c = tolower(x[i] < 128 ? x[i] : 0) - tolower(y[i] < 128 ? y[i] : 0); if (x[i] >= 128 || y[i] >= 128) c = (int)x[i] - (int)y[i]; if (c) return (uint32_t)c; if (!x[i]) break; } return 0; }
XDEF(wcschr) { uint16_t *s = WS(0); for (;; s++) { if (*s == (uint16_t)A(1)) return GA(s); if (!*s) return 0; } }
XDEF(wcsrchr) { uint16_t *s = WS(0), *r = NULL; for (;; s++) { if (*s == (uint16_t)A(1)) r = s; if (!*s) break; } return r ? GA(r) : 0; }
XDEF(wcsstr)
{
    uint16_t *h = WS(0);
    const uint16_t *n = WS(1);
    int nl = wstrlen(n);
    for (; *h; h++) if (!memcmp(h, n, (size_t)nl * 2)) return GA(h);
    return nl ? 0 : A(0);
}
XDEF(_wcsdup) { if (!A(0)) return 0; return gwstrdup(WS(0)); }
XDEF(_wcslwr) { for (uint16_t *s = WS(0); *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; return A(0); }
XDEF(_wcsupr) { for (uint16_t *s = WS(0); *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; return A(0); }
XDEF(wcscpy_s) { if (!A(0) || !A(2) || (uint32_t)wstrlen(WS(2)) >= A(1)) { if (A(0) && A(1)) *WS(0) = 0; return CRT_ERANGE; } wstrcpy(WS(0), WS(2)); return 0; }
XDEF(wcscat_s) { if (!A(0) || !A(2) || (uint32_t)(wstrlen(WS(0)) + wstrlen(WS(2))) >= A(1)) return CRT_ERANGE; wstrcpy(WS(0) + wstrlen(WS(0)), WS(2)); return 0; }
XDEF(wmemcpy) { memmove(P(0), P(1), A(2) * 2); return A(0); }
XDEF(wmemmove) { memmove(P(0), P(1), A(2) * 2); return A(0); }
XDEF(wmemset) { uint16_t *d = P(0); for (uint32_t i = 0; i < A(2); i++) d[i] = (uint16_t)A(1); return A(0); }

/* characters */
static int cls(int c, int what)
{
    if (c < 0 || c > 255) return 0;
    switch (what) {
    case 0: return isalpha(c); case 1: return isdigit(c); case 2: return isalnum(c); case 3: return isspace(c);
    case 4: return isupper(c); case 5: return islower(c); case 6: return isxdigit(c); case 7: return ispunct(c);
    case 8: return isprint(c); case 9: return isgraph(c); case 10: return iscntrl(c); default: return c == ' ' || c == '\t';
    }
}
/* the CRT's mask values: _UPPER 1, _LOWER 2, _DIGIT 4, _SPACE 8, _PUNCT 16, _CONTROL 32, _BLANK 64, _HEX 128, _ALPHA 0x100 */
static uint32_t ctype_mask(int c)
{
    uint32_t m = 0;
    if (cls(c, 4)) m |= 1; if (cls(c, 5)) m |= 2; if (cls(c, 1)) m |= 4; if (cls(c, 3)) m |= 8;
    if (cls(c, 7)) m |= 16; if (cls(c, 10)) m |= 32; if (c == ' ') m |= 64; if (cls(c, 6)) m |= 128;
    if (cls(c, 0)) m |= 0x100;
    return m;
}
XDEF(isalpha) { return (uint32_t)(cls((int)A(0), 0) != 0); }
XDEF(isdigit) { return (uint32_t)(cls((int)A(0), 1) != 0); }
XDEF(isalnum) { return (uint32_t)(cls((int)A(0), 2) != 0); }
XDEF(isspace) { return (uint32_t)(cls((int)A(0), 3) != 0); }
XDEF(isupper) { return (uint32_t)(cls((int)A(0), 4) != 0); }
XDEF(islower) { return (uint32_t)(cls((int)A(0), 5) != 0); }
XDEF(isxdigit) { return (uint32_t)(cls((int)A(0), 6) != 0); }
XDEF(ispunct) { return (uint32_t)(cls((int)A(0), 7) != 0); }
XDEF(isprint) { return (uint32_t)(cls((int)A(0), 8) != 0); }
XDEF(isgraph) { return (uint32_t)(cls((int)A(0), 9) != 0); }
XDEF(iscntrl) { return (uint32_t)(cls((int)A(0), 10) != 0); }
XDEF(isblank) { return (uint32_t)(cls((int)A(0), 11) != 0); }
XDEF(toupper) { int c = (int)A(0); return (uint32_t)(c >= 'a' && c <= 'z' ? c - 32 : c); }
XDEF(tolower) { int c = (int)A(0); return (uint32_t)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
XDEF(_isctype) { return ctype_mask((int)A(0)) & A(1); }
XDEF(iswalpha) { return A(0) < 128 ? (uint32_t)(cls((int)A(0), 0) != 0) : 1; }
XDEF(iswdigit) { return (uint32_t)(cls((int)A(0), 1) != 0); }
XDEF(iswspace) { return (uint32_t)(cls((int)A(0), 3) != 0); }
XDEF(iswalnum) { return A(0) < 128 ? (uint32_t)(cls((int)A(0), 2) != 0) : 1; }
XDEF(iswupper) { return (uint32_t)(cls((int)A(0), 4) != 0); }
XDEF(iswlower) { return (uint32_t)(cls((int)A(0), 5) != 0); }
XDEF(iswxdigit) { return (uint32_t)(cls((int)A(0), 6) != 0); }
XDEF(iswpunct) { return (uint32_t)(cls((int)A(0), 7) != 0); }
XDEF(iswprint) { return A(0) >= 128 || cls((int)A(0), 8); }
XDEF(iswctype) { return ctype_mask((int)A(0)) & A(1); }
XDEF(towupper) { return api_toupper(a); }
XDEF(towlower) { return api_tolower(a); }
static gp_t pctype_gp, pctype_ptr_gp;
static void build_ctype(void)
{
    /* the classic _ctype table: 257 shorts, index -1 for EOF; _pctype points at [1] */
    pctype_gp = vm_alloc_loader(257 * 2);
    uint16_t *t = GP(pctype_gp);
    t[0] = 0;
    for (int c = 0; c < 256; c++) t[c + 1] = (uint16_t)ctype_mask(c);
    pctype_ptr_gp = vm_alloc_loader(4);
    *(gp_t *)GP(pctype_ptr_gp) = pctype_gp + 2;
}
XDEF(__pctype_func) { (void)a; return pctype_gp + 2; }
XDEF(__p__pctype) { (void)a; return pctype_ptr_gp; }
XDEF(___lc_codepage_func) { (void)a; return 1252; }
XDEF(___lc_handle_func) { (void)a; return 0; }
XDEF(__p___mb_cur_max_dummy) { (void)a; return 0; }
XDEF(setlocale)
{
    static gp_t c;
    if (!c) { c = vm_alloc_loader(8); strcpy(GP(c), "C"); }
    return c;
}
XDEF(_wsetlocale)
{
    static gp_t c;
    if (!c) { c = vm_alloc_loader(8); ((uint16_t *)GP(c))[0] = 'C'; }
    return c;
}
XDEF(localeconv)
{
    static gp_t lc;
    if (!lc) {
        lc = vm_alloc_loader(64);
        gp_t dot = vm_alloc_loader(4), empty = vm_alloc_loader(4);
        strcpy(GP(dot), ".");
        gp_t *f = GP(lc);
        f[0] = dot;
        for (int i = 1; i < 10; i++) f[i] = empty;
        memset((char *)GP(lc) + 40, 127, 8);
    }
    return lc;
}

/* numbers */
XDEF(atoi) { return (uint32_t)atoi(S(0)); }
XDEF(atol) { return (uint32_t)atol(S(0)); }
XDEF(_atoi64) { long long v = strtoll(S(0), NULL, 10); api_edx = (uint32_t)((uint64_t)v >> 32); return (uint32_t)v; }
XDEF(atoll) { return api__atoi64(a); }
XDEF(_wtoi) { return (uint32_t)atoi(wtmp(A(0))); }
XDEF(_wtol) { return (uint32_t)atol(wtmp(A(0))); }
static uint32_t strtoi(uint32_t *a, int sgn, int wide64)
{
    const char *s = wide64 & 2 ? wtmp(A(0)) : S(0);
    const char *e;
    int neg, ovf;
    uint64_t v = crt_strtoull(s, &e, (int)A(2), &neg, &ovf);
    if (A(1)) {
        uint32_t off = (uint32_t)(e - s);
        *(gp_t *)P(1) = A(0) + (wide64 & 2 ? off * 2 : off);
    }
    if (wide64 & 1) {
        uint64_t r = neg ? (uint64_t)-(int64_t)v : v;
        if (ovf || (sgn && !neg && v > 0x7FFFFFFFFFFFFFFFull) || (sgn && neg && v > 0x8000000000000000ull)) {
            crt_set_errno(CRT_ERANGE);
            r = !sgn ? ~0ull : neg ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
        }
        api_edx = (uint32_t)(r >> 32);
        return (uint32_t)r;
    }
    if (sgn) {
        if (ovf || (!neg && v > 0x7FFFFFFF) || (neg && v > 0x80000000u)) { crt_set_errno(CRT_ERANGE); return neg ? 0x80000000u : 0x7FFFFFFF; }
        return neg ? (uint32_t)-(int64_t)v : (uint32_t)v;
    }
    if (ovf || v > 0xFFFFFFFFu) { crt_set_errno(CRT_ERANGE); return 0xFFFFFFFFu; }
    return neg ? (uint32_t)-(uint32_t)v : (uint32_t)v;
}
XDEF(strtol) { return strtoi(a, 1, 0); }
XDEF(strtoul) { return strtoi(a, 0, 0); }
XDEF(_strtoi64) { return strtoi(a, 1, 1); }
XDEF(_strtoui64) { return strtoi(a, 0, 1); }
XDEF(strtoll) { return strtoi(a, 1, 1); }
XDEF(strtoull) { return strtoi(a, 0, 1); }
XDEF(wcstol) { return strtoi(a, 1, 2); }
XDEF(wcstoul) { return strtoi(a, 0, 2); }
XDEF(_wcstoi64) { return strtoi(a, 1, 3); }
XDEF(_wcstoui64) { return strtoi(a, 0, 3); }
XDEF(strtod)
{
    const char *e;
    double d = crt_strtod(S(0), &e);
    if (A(1)) *(gp_t *)P(1) = A(0) + (uint32_t)(e - S(0));
    api_set_fret(d);
    return 0;
}
XDEF(atof) { api_set_fret(crt_strtod(S(0), NULL)); return 0; }
XDEF(wcstod)
{
    const char *s = wtmp(A(0)), *e;
    double d = crt_strtod(s, &e);
    if (A(1)) *(gp_t *)P(1) = A(0) + (uint32_t)(e - s) * 2;
    api_set_fret(d);
    return 0;
}
XDEF(_wtof) { api_set_fret(crt_strtod(wtmp(A(0)), NULL)); return 0; }
static uint32_t to_str(uint64_t v, int neg, gp_t out, int radix, int wide)
{
    char tmp[72];
    int n = 0;
    if (radix < 2 || radix > 36) {      /* an invalid parameter: an empty string */
        if (wide) *(uint16_t *)GP(out) = 0; else *(char *)GP(out) = 0;
        return out;
    }
    do { int d = (int)(v % (unsigned)radix); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)radix; } while (v);
    if (neg) tmp[n++] = '-';
    char buf[72];
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    if (wide) ansi_to_wide(buf, -1, GP(out), n + 1); else memcpy(GP(out), buf, (size_t)n + 1);
    return out;
}
XDEF(_itoa) { int32_t v = (int32_t)A(0); int neg = A(2) == 10 && v < 0; return to_str(neg ? (uint64_t)-(int64_t)v : (uint32_t)v, neg, A(1), (int)A(2), 0); }
XDEF(_ltoa) { return api__itoa(a); }
XDEF(_ultoa) { return to_str(A(0), 0, A(1), (int)A(2), 0); }
XDEF(_i64toa) { int64_t v; memcpy(&v, a, 8); int neg = A(3) == 10 && v < 0; return to_str(neg ? (uint64_t)-v : (uint64_t)v, neg, A(2), (int)A(3), 0); }
XDEF(_ui64toa) { uint64_t v; memcpy(&v, a, 8); return to_str(v, 0, A(2), (int)A(3), 0); }
XDEF(_itow) { int32_t v = (int32_t)A(0); int neg = A(2) == 10 && v < 0; return to_str(neg ? (uint64_t)-(int64_t)v : (uint32_t)v, neg, A(1), (int)A(2), 1); }
XDEF(_ltow) { return api__itow(a); }
XDEF(_ultow) { return to_str(A(0), 0, A(1), (int)A(2), 1); }
XDEF(_itoa_s) { uint32_t b[3] = { A(0), A(1), A(3) }; api__itoa(b); return 0; }
XDEF(_ltoa_s) { return api__itoa_s(a); }
XDEF(_itow_s) { uint32_t b[3] = { A(0), A(1), A(3) }; api__itow(b); return 0; }
XDEF(abs) { int32_t v = (int32_t)A(0); return (uint32_t)(v < 0 ? -v : v); }
XDEF(labs) { return api_abs(a); }
XDEF(_abs64) { int64_t v; memcpy(&v, a, 8); if (v < 0) v = -v; api_edx = (uint32_t)((uint64_t)v >> 32); return (uint32_t)v; }
XDEF(llabs) { return api__abs64(a); }
XDEF(div) { int32_t n = (int32_t)A(0), d = (int32_t)A(1); api_edx = (uint32_t)(n % d); return (uint32_t)(n / d); }
XDEF(ldiv) { return api_div(a); }
XDEF(rand) { (void)a; return (uint32_t)rand() & 0x7FFF; }
XDEF(srand) { srand(A(0)); return 0; }
XDEF(rand_s) { *(uint32_t *)P(0) = ((uint32_t)rand() << 16) ^ (uint32_t)rand(); return 0; }
XDEF(_rotl) { uint32_t v = A(0), s = A(1) & 31; return s ? (v << s) | (v >> (32 - s)) : v; }
XDEF(_rotr) { uint32_t v = A(0), s = A(1) & 31; return s ? (v >> s) | (v << (32 - s)) : v; }
XDEF(_byteswap_ulong) { return __builtin_bswap32(A(0)); }
XDEF(_byteswap_ushort) { return __builtin_bswap16((uint16_t)A(0)); }
XDEF(_byteswap_uint64) { uint64_t v; memcpy(&v, a, 8); v = __builtin_bswap64(v); api_edx = (uint32_t)(v >> 32); return (uint32_t)v; }

/* qsort and bsearch call back into the program */
static gp_t sort_cmp;
static int sort_cmp_fn(const void *x, const void *y)
{
    return (int)call_guest2(sort_cmp, GA(x), GA(y));
}
XDEF(qsort)
{
    gp_t saved = sort_cmp;
    sort_cmp = A(3);
    qsort(P(0), A(1), A(2), sort_cmp_fn);
    sort_cmp = saved;
    return 0;
}
XDEF(bsearch)
{
    gp_t saved = sort_cmp;
    sort_cmp = A(4);
    void *r = bsearch(P(0), P(1), A(2), A(3), sort_cmp_fn);
    sort_cmp = saved;
    return r ? GA(r) : 0;
}

/* ---- stdio ---- */

#define F(i) crt_stream(a[i])
#define VA(i) ((const uint32_t *)GP(a[i]))

static void put_stream(void *ctx, int c) { crt_stream_putc(ctx, c); }
static void put_stream_w(void *ctx, int c)
{
    /* wide output to a byte stream: UTF-8 */
    char buf[4];
    uint16_t w = (uint16_t)c;
    int n = wide_to_utf8(&w, 1, buf, 4);
    crt_stream_write(ctx, buf, (uint32_t)n);
}
static uint32_t fmt_stream(struct crt_stream *s, const void *fmt, int wide, const uint32_t *args)
{
    if (!s) { crt_set_errno(CRT_EBADF); return (uint32_t)-1; }
    return (uint32_t)crt_vformat(wide ? put_stream_w : put_stream, s, fmt, wide, args);
}
static struct crt_stream *out_s(void) { return &crt_streams[1]; }
static struct crt_stream *in_s(void) { return &crt_streams[0]; }

XDEF(printf) { return fmt_stream(out_s(), S(0), 0, a + 1); }
XDEF(vprintf) { return fmt_stream(out_s(), S(0), 0, VA(1)); }
XDEF(fprintf) { return fmt_stream(F(0), S(1), 0, a + 2); }
XDEF(vfprintf) { return fmt_stream(F(0), S(1), 0, VA(2)); }
XDEF(wprintf) { return fmt_stream(out_s(), WS(0), 1, a + 1); }
XDEF(vwprintf) { return fmt_stream(out_s(), WS(0), 1, VA(1)); }
XDEF(fwprintf) { return fmt_stream(F(0), WS(1), 1, a + 2); }
XDEF(vfwprintf) { return fmt_stream(F(0), WS(1), 1, VA(2)); }
XDEF(sprintf) { return (uint32_t)crt_vsnprintf(S(0), 0x7FFFFFFF, S(1), a + 2, 0); }
XDEF(vsprintf) { return (uint32_t)crt_vsnprintf(S(0), 0x7FFFFFFF, S(1), VA(2), 0); }
XDEF(_snprintf) { return (uint32_t)crt_vsnprintf(S(0), A(1), S(2), a + 3, 1); }
XDEF(_vsnprintf) { return (uint32_t)crt_vsnprintf(S(0), A(1), S(2), VA(3), 1); }
XDEF(snprintf) { return (uint32_t)crt_vsnprintf(S(0), A(1), S(2), a + 3, 0); }
XDEF(vsnprintf) { return (uint32_t)crt_vsnprintf(S(0), A(1), S(2), VA(3), 0); }
XDEF(sprintf_s) { int r = crt_vsnprintf(S(0), A(1), S(2), a + 3, 0); return (uint32_t)(r >= (int)A(1) ? -1 : r); }
XDEF(vsprintf_s) { int r = crt_vsnprintf(S(0), A(1), S(2), VA(3), 0); return (uint32_t)(r >= (int)A(1) ? -1 : r); }
XDEF(_snprintf_s) { uint32_t cap = A(2) < A(1) ? A(2) + 1 : A(1); return (uint32_t)crt_vsnprintf(S(0), cap, S(3), a + 4, 1); }
XDEF(_vsnprintf_s) { uint32_t cap = A(2) < A(1) ? A(2) + 1 : A(1); return (uint32_t)crt_vsnprintf(S(0), cap, S(3), VA(4), 1); }
XDEF(_scprintf) { return (uint32_t)crt_vscprintf(S(0), 0, a + 1); }
XDEF(_vscprintf) { return (uint32_t)crt_vscprintf(S(0), 0, VA(1)); }
XDEF(_scwprintf) { return (uint32_t)crt_vscprintf(WS(0), 1, a + 1); }
XDEF(_vscwprintf) { return (uint32_t)crt_vscprintf(WS(0), 1, VA(1)); }
XDEF(swprintf) { return (uint32_t)crt_vsnwprintf(WS(0), A(1), WS(2), a + 3, 0); }   /* the ISO form, with a count */
XDEF(_swprintf) { return (uint32_t)crt_vsnwprintf(WS(0), 0x7FFFFFFF, WS(1), a + 2, 0); }
XDEF(vswprintf) { return (uint32_t)crt_vsnwprintf(WS(0), A(1), WS(2), VA(3), 0); }
XDEF(_vswprintf) { return (uint32_t)crt_vsnwprintf(WS(0), 0x7FFFFFFF, WS(1), VA(2), 0); }
XDEF(_snwprintf) { return (uint32_t)crt_vsnwprintf(WS(0), A(1), WS(2), a + 3, 1); }
XDEF(_vsnwprintf) { return (uint32_t)crt_vsnwprintf(WS(0), A(1), WS(2), VA(3), 1); }
XDEF(swprintf_s) { return (uint32_t)crt_vsnwprintf(WS(0), A(1), WS(2), a + 3, 0); }
XDEF(vswprintf_s) { return (uint32_t)crt_vsnwprintf(WS(0), A(1), WS(2), VA(3), 0); }
XDEF(_snwprintf_s) { uint32_t cap = A(2) < A(1) ? A(2) + 1 : A(1); return (uint32_t)crt_vsnwprintf(WS(0), cap, WS(3), a + 4, 1); }

/* scanf */
struct str_src { const uint8_t *s; int pos; int wide; };
static int str_get(void *ctx)
{
    struct str_src *x = ctx;
    int c = x->wide ? ((const uint16_t *)x->s)[x->pos] : x->s[x->pos];
    if (!c) return -1;
    x->pos++;
    return c;
}
static void str_unget(void *ctx, int c) { (void)c; ((struct str_src *)ctx)->pos--; }
static int stream_get(void *ctx) { return crt_stream_getc(ctx); }
static void stream_unget(void *ctx, int c) { crt_stream_ungetc(ctx, c); }
static uint32_t scan_str(const void *s, int wide, const void *fmt, const uint32_t *args)
{
    struct str_src x = { s, 0, wide };
    struct scan_src in = { str_get, str_unget, &x, wide };
    return (uint32_t)crt_vscanf(&in, fmt, wide, args);
}
static uint32_t scan_stream(struct crt_stream *st, const void *fmt, int wide, const uint32_t *args)
{
    if (!st) return (uint32_t)-1;
    crt_flush_all();
    struct scan_src in = { stream_get, stream_unget, st, wide };
    return (uint32_t)crt_vscanf(&in, fmt, wide, args);
}
XDEF(sscanf) { return scan_str(S(0), 0, S(1), a + 2); }
XDEF(sscanf_s) { return scan_str(S(0), 0, S(1), a + 2); }
XDEF(vsscanf) { return scan_str(S(0), 0, S(1), VA(2)); }
XDEF(swscanf) { return scan_str(WS(0), 1, WS(1), a + 2); }
XDEF(scanf) { return scan_stream(in_s(), S(0), 0, a + 1); }
XDEF(scanf_s) { return scan_stream(in_s(), S(0), 0, a + 1); }
XDEF(fscanf) { return scan_stream(F(0), S(1), 0, a + 2); }
XDEF(fscanf_s) { return scan_stream(F(0), S(1), 0, a + 2); }
XDEF(wscanf) { return scan_stream(in_s(), WS(0), 1, a + 1); }

XDEF(fopen)
{
    int err = 0;
    struct crt_stream *s = crt_stream_open(S(0), S(1), &err);
    if (!s) { crt_set_errno(err); return 0; }
    return crt_stream_gp((int)(s - crt_streams));
}
XDEF(_wfopen)
{
    char path[MAX_PATH * 2], mode[16];
    snprintf(path, sizeof(path), "%s", wtmp(A(0)));
    snprintf(mode, sizeof(mode), "%s", wtmp(A(1)));
    int err = 0;
    struct crt_stream *s = crt_stream_open(path, mode, &err);
    if (!s) { crt_set_errno(err); return 0; }
    return crt_stream_gp((int)(s - crt_streams));
}
XDEF(fopen_s) { uint32_t b[2] = { A(1), A(2) }; gp_t f = api_fopen(b); *(gp_t *)P(0) = f; return f ? 0 : (uint32_t)*(int32_t *)GP(crt_errno_gp); }
XDEF(_wfopen_s) { uint32_t b[2] = { A(1), A(2) }; gp_t f = api__wfopen(b); *(gp_t *)P(0) = f; return f ? 0 : (uint32_t)*(int32_t *)GP(crt_errno_gp); }
XDEF(_fsopen) { return api_fopen(a); }
XDEF(_wfsopen) { return api__wfopen(a); }
XDEF(freopen)
{
    struct crt_stream *old = F(2);
    if (!old) return 0;
    int i = (int)(old - crt_streams);
    int err = 0;
    crt_stream_close(old);
    struct crt_stream *s = crt_stream_open(S(0), S(1), &err);
    if (!s) { crt_set_errno(err); return 0; }
    if (s != old) {                     /* keep the FILE the program holds */
        crt_streams[i] = *s;
        memset(s, 0, sizeof(*s));
    }
    return A(2);
}
XDEF(_fdopen)
{
    struct crt_fd *f = crt_fd_get((int)A(0));
    if (!f) return 0;
    for (int i = 3; i < CRT_MAX_STREAMS; i++)
        if (!crt_streams[i].used) {
            const char *m = S(1);
            struct crt_stream *s = &crt_streams[i];
            memset(s, 0, sizeof(*s));
            s->used = 1; s->fd = (int)A(0); s->unget = -1;
            s->read = m[0] == 'r' || strchr(m, '+');
            s->write = m[0] != 'r' || strchr(m, '+');
            s->append = m[0] == 'a';
            s->text = f->text;
            s->ocap = s->icap = 4096;
            s->obuf = malloc(4096); s->ibuf = malloc(4096);
            return crt_stream_gp(i);
        }
    return 0;
}
XDEF(fclose) { struct crt_stream *s = F(0); if (!s) return (uint32_t)-1; crt_stream_close(s); return 0; }
XDEF(_fcloseall) { (void)a; int n = 0; for (int i = 3; i < CRT_MAX_STREAMS; i++) if (crt_streams[i].used) { crt_stream_close(&crt_streams[i]); n++; } return (uint32_t)n; }
XDEF(fflush) { if (!A(0)) { crt_flush_all(); return 0; } struct crt_stream *s = F(0); return s ? (uint32_t)crt_stream_flush(s) : (uint32_t)-1; }
XDEF(_flushall) { (void)a; crt_flush_all(); return 0; }
XDEF(fwrite)
{
    struct crt_stream *s = F(3);
    uint64_t n = (uint64_t)A(1) * A(2);
    if (!s || !n) return 0;
    int r = crt_stream_write(s, P(0), (uint32_t)n);
    return A(1) ? (uint32_t)r / A(1) : 0;
}
XDEF(fread)
{
    struct crt_stream *s = F(3);
    uint64_t n = (uint64_t)A(1) * A(2);
    if (!s || !n) return 0;
    if (s == &crt_streams[0]) crt_flush_all();
    int r = crt_stream_read(s, P(0), (uint32_t)n);
    return A(1) ? (uint32_t)r / A(1) : 0;
}
XDEF(fputc) { struct crt_stream *s = F(1); return s ? (uint32_t)crt_stream_putc(s, (int)A(0)) : (uint32_t)-1; }
XDEF(putc) { return api_fputc(a); }
XDEF(_fputc_nolock) { return api_fputc(a); }
XDEF(putchar) { return (uint32_t)crt_stream_putc(out_s(), (int)A(0)); }
XDEF(_putchar_nolock) { return api_putchar(a); }
XDEF(fputs) { struct crt_stream *s = F(1); if (!s) return (uint32_t)-1; size_t n = strlen(S(0)); return crt_stream_write(s, S(0), (uint32_t)n) == (int)n ? 0 : (uint32_t)-1; }
XDEF(puts) { struct crt_stream *s = out_s(); size_t n = strlen(S(0)); crt_stream_write(s, S(0), (uint32_t)n); crt_stream_putc(s, '\n'); return 0; }
XDEF(fputwc) { struct crt_stream *s = F(1); if (!s) return 0xFFFF; put_stream_w(s, (int)(A(0) & 0xFFFF)); return A(0) & 0xFFFF; }
XDEF(putwc) { return api_fputwc(a); }
XDEF(putwchar) { put_stream_w(out_s(), (int)(A(0) & 0xFFFF)); return A(0) & 0xFFFF; }
XDEF(fputws) { struct crt_stream *s = F(1); if (!s) return (uint32_t)-1; for (const uint16_t *w = WS(0); *w; w++) put_stream_w(s, *w); return 0; }
XDEF(_putws) { for (const uint16_t *w = WS(0); *w; w++) put_stream_w(out_s(), *w); crt_stream_putc(out_s(), '\n'); return 0; }
XDEF(fgetc) { struct crt_stream *s = F(0); if (!s) return (uint32_t)-1; if (s == &crt_streams[0]) crt_flush_all(); return (uint32_t)crt_stream_getc(s); }
XDEF(getc) { return api_fgetc(a); }
XDEF(_fgetc_nolock) { return api_fgetc(a); }
XDEF(getchar) { crt_flush_all(); return (uint32_t)crt_stream_getc(in_s()); }
XDEF(fgetwc) { struct crt_stream *s = F(0); if (!s) return 0xFFFF; int c = crt_stream_getc(s); return c < 0 ? 0xFFFF : (uint32_t)c; }
XDEF(getwc) { return api_fgetwc(a); }
XDEF(getwchar) { crt_flush_all(); int c = crt_stream_getc(in_s()); return c < 0 ? 0xFFFF : (uint32_t)c; }
XDEF(ungetc) { struct crt_stream *s = F(1); return s ? (uint32_t)crt_stream_ungetc(s, (int)A(0)) : (uint32_t)-1; }
XDEF(ungetwc) { struct crt_stream *s = F(1); return s ? (uint32_t)crt_stream_ungetc(s, (int)A(0)) : 0xFFFF; }
XDEF(fgets)
{
    struct crt_stream *s = F(2);
    if (!s || (int)A(1) <= 0) return 0;
    if (s == &crt_streams[0]) crt_flush_all();
    char *b = S(0);
    int n = 0;
    while (n < (int)A(1) - 1) {
        int c = crt_stream_getc(s);
        if (c < 0) break;
        b[n++] = (char)c;
        if (c == '\n') break;
    }
    if (!n) return 0;
    b[n] = 0;
    return A(0);
}
XDEF(fgetws)
{
    struct crt_stream *s = F(2);
    if (!s || (int)A(1) <= 0) return 0;
    uint16_t *b = WS(0);
    int n = 0;
    while (n < (int)A(1) - 1) {
        int c = crt_stream_getc(s);
        if (c < 0) break;
        b[n++] = (uint16_t)c;
        if (c == '\n') break;
    }
    if (!n) return 0;
    b[n] = 0;
    return A(0);
}
XDEF(gets)
{
    crt_flush_all();
    char *b = S(0);
    int n = 0, c;
    while ((c = crt_stream_getc(in_s())) >= 0 && c != '\n')
        b[n++] = (char)c;
    if (c < 0 && !n) return 0;
    b[n] = 0;
    return A(0);
}
XDEF(fseek) { struct crt_stream *s = F(0); if (!s) return (uint32_t)-1; return crt_stream_seek(s, (int32_t)A(1), (int)A(2)) < 0 ? (uint32_t)-1 : 0; }
XDEF(_fseeki64) { struct crt_stream *s = F(0); int64_t off; memcpy(&off, a + 1, 8); if (!s) return (uint32_t)-1; return crt_stream_seek(s, off, (int)A(3)) < 0 ? (uint32_t)-1 : 0; }
XDEF(fseeko64) { return api__fseeki64(a); }
XDEF(ftell) { struct crt_stream *s = F(0); if (!s) return (uint32_t)-1; return (uint32_t)crt_stream_tell(s); }
XDEF(_ftelli64) { struct crt_stream *s = F(0); int64_t r = s ? crt_stream_tell(s) : -1; api_edx = (uint32_t)((uint64_t)r >> 32); return (uint32_t)r; }
XDEF(ftello64) { return api__ftelli64(a); }
XDEF(fgetpos) { struct crt_stream *s = F(0); if (!s) return (uint32_t)-1; int64_t r = crt_stream_tell(s); memcpy(P(1), &r, 8); return 0; }
XDEF(fsetpos) { struct crt_stream *s = F(0); if (!s) return (uint32_t)-1; int64_t off; memcpy(&off, P(1), 8); return crt_stream_seek(s, off, SEEK_SET) < 0 ? (uint32_t)-1 : 0; }
XDEF(rewind) { struct crt_stream *s = F(0); if (s) { crt_stream_seek(s, 0, SEEK_SET); s->error = 0; } return 0; }
XDEF(feof) { struct crt_stream *s = F(0); return s ? (uint32_t)(s->eof && s->unget < 0 && s->ipos >= s->ilen) : 0; }
XDEF(ferror) { struct crt_stream *s = F(0); return s ? (uint32_t)s->error : 0; }
XDEF(clearerr) { struct crt_stream *s = F(0); if (s) s->eof = s->error = 0; return 0; }
XDEF(_fileno) { struct crt_stream *s = F(0); return s ? (uint32_t)s->fd : (uint32_t)-1; }
XDEF(setvbuf)
{
    struct crt_stream *s = F(0);
    if (!s) return (uint32_t)-1;
    crt_stream_flush(s);
    s->unbuffered = A(2) == 4;          /* _IONBF */
    s->line_buffered = A(2) == 0x40;    /* _IOLBF */
    return 0;
}
XDEF(setbuf) { struct crt_stream *s = F(0); if (s) { crt_stream_flush(s); s->unbuffered = !A(1); } return 0; }
XDEF(_filbuf) { struct crt_stream *s = F(0); return s ? (uint32_t)crt_stream_getc(s) : (uint32_t)-1; }
XDEF(_flsbuf) { struct crt_stream *s = F(1); return s ? (uint32_t)crt_stream_putc(s, (int)A(0)) : (uint32_t)-1; }
XDEF(tmpfile)
{
    char name[64];
    snprintf(name, sizeof(name), "C:\\tmp\\t%u_%u.tmp", (unsigned)getpid(), os_ticks_ms());
    int err;
    struct crt_stream *s = crt_stream_open(name, "w+b", &err);
    if (!s) return 0;
    s->tmp = 1;
    return crt_stream_gp((int)(s - crt_streams));
}
XDEF(tmpnam)
{
    static gp_t buf;
    static int n;
    gp_t out = A(0);
    if (!out) { if (!buf) buf = vm_alloc_loader(MAX_PATH); out = buf; }
    snprintf(GP(out), MAX_PATH, "\\s%x.%d", (unsigned)getpid(), ++n);
    return out;
}

/* low-level I/O */
XDEF(_open)
{
    char host[PATH_MAX];
    int fd;
    if (crt_device_name(S(0), &fd)) { if (fd < 0) { crt_set_errno(CRT_ENOENT); return (uint32_t)-1; } return (uint32_t)crt_fd_new(fd, !(A(1) & 0x8000)); }
    if (path_to_host(S(0), host) < 0) { crt_set_errno(CRT_ENOENT); return (uint32_t)-1; }
    int text = !(A(1) & 0x8000) && ((A(1) & 0x4000) || *(int32_t *)GP(fmode_cell) != 0x8000);
    int r = crt_fd_open(host, (int)A(1), (int)A(2), text);
    if (r < 0) { crt_set_errno(-r); return (uint32_t)-1; }
    return (uint32_t)r;
}
XDEF(_wopen) { gp_t n = gstrdup(wtmp(A(0))); uint32_t b[3] = { n, A(1), A(2) }; uint32_t r = api__open(b); gfree(n); return r; }
XDEF(_sopen) { uint32_t b[3] = { A(0), A(1), A(3) }; return api__open(b); }
XDEF(_sopen_s) { uint32_t b[3] = { A(1), A(2), A(4) }; uint32_t r = api__open(b); *(int32_t *)P(0) = (int32_t)r; return r == (uint32_t)-1 ? (uint32_t)*(int32_t *)GP(crt_errno_gp) : 0; }
XDEF(_wsopen_s) { gp_t n = gstrdup(wtmp(A(1))); uint32_t b[5] = { A(0), n, A(2), A(3), A(4) }; uint32_t r = api__sopen_s(b); gfree(n); return r; }
XDEF(_creat) { uint32_t b[3] = { A(0), 0x301, A(1) }; return api__open(b); }
XDEF(_close) { return crt_fd_close((int)A(0)) < 0 ? (crt_set_errno(CRT_EBADF), (uint32_t)-1) : 0; }
XDEF(_read) { if (A(0) == 0) crt_flush_all(); int r = crt_fd_read((int)A(0), P(1), A(2)); if (r < 0) crt_set_errno(CRT_EBADF); return (uint32_t)r; }
XDEF(_write) { int r = crt_fd_write((int)A(0), P(1), A(2)); if (r < 0) crt_set_errno(CRT_EBADF); return (uint32_t)r; }
XDEF(_lseek) { return (uint32_t)crt_fd_seek((int)A(0), (int32_t)A(1), (int)A(2)); }
XDEF(_lseeki64) { int64_t off; memcpy(&off, a + 1, 8); int64_t r = crt_fd_seek((int)A(0), off, (int)A(3)); api_edx = (uint32_t)((uint64_t)r >> 32); return (uint32_t)r; }
XDEF(_tell) { return (uint32_t)crt_fd_seek((int)A(0), 0, SEEK_CUR); }
XDEF(_telli64) { int64_t r = crt_fd_seek((int)A(0), 0, SEEK_CUR); api_edx = (uint32_t)((uint64_t)r >> 32); return (uint32_t)r; }
XDEF(_eof)
{
    struct crt_fd *f = crt_fd_get((int)A(0));
    if (!f) return (uint32_t)-1;
    off_t cur = lseek(f->fd, 0, SEEK_CUR), end = lseek(f->fd, 0, SEEK_END);
    lseek(f->fd, cur, SEEK_SET);
    return cur >= end;
}
XDEF(_filelength) { struct crt_fd *f = crt_fd_get((int)A(0)); struct stat st; if (!f || fstat(f->fd, &st) < 0) return (uint32_t)-1; return (uint32_t)st.st_size; }
XDEF(_filelengthi64) { struct crt_fd *f = crt_fd_get((int)A(0)); struct stat st; if (!f || fstat(f->fd, &st) < 0) { api_edx = 0xFFFFFFFFu; return (uint32_t)-1; } api_edx = 0; return (uint32_t)st.st_size; }
XDEF(_chsize) { struct crt_fd *f = crt_fd_get((int)A(0)); if (!f) return (uint32_t)-1; return ftruncate(f->fd, (off_t)A(1)) < 0 ? (uint32_t)-1 : 0; }
XDEF(_chsize_s) { struct crt_fd *f = crt_fd_get((int)A(0)); if (!f) return CRT_EBADF; return ftruncate(f->fd, (off_t)A(1)) < 0 ? CRT_EACCES : 0; }
XDEF(_dup) { struct crt_fd *f = crt_fd_get((int)A(0)); if (!f) return (uint32_t)-1; int h = dup(f->fd); if (h < 0) return (uint32_t)-1; int r = crt_fd_new(h, f->text); return (uint32_t)r; }
XDEF(_dup2)
{
    struct crt_fd *f = crt_fd_get((int)A(0));
    if (!f || A(1) >= CRT_MAX_FDS) return (uint32_t)-1;
    crt_flush_all();
    if (crt_fds[A(1)].used) crt_fd_close((int)A(1));
    int h = (int)A(1) <= 2 ? dup2(f->fd, (int)A(1)) : dup(f->fd);
    if (h < 0) return (uint32_t)-1;
    crt_fds[A(1)] = *f;
    crt_fds[A(1)].fd = h;
    return 0;
}
XDEF(_isatty) { struct crt_fd *f = crt_fd_get((int)A(0)); return f && isatty(f->fd) ? 0x40 : 0; }
XDEF(_commit) { struct crt_fd *f = crt_fd_get((int)A(0)); if (f) fsync(f->fd); return 0; }
XDEF(_get_osfhandle)
{
    struct crt_fd *f = crt_fd_get((int)A(0));
    if (!f) { crt_set_errno(CRT_EBADF); return INVALID_HANDLE_VALUE; }
    if (f->handle) return f->handle;
    if (f->fd <= 2 && std_handles[f->fd]) return std_handles[f->fd];
    f->handle = handle_for_fd(dup(f->fd), H_FILE, isatty(f->fd));
    return f->handle;
}
XDEF(_open_osfhandle)
{
    struct hobj *o = handle_get(A(0));
    if (!o || o->fd < 0) { crt_set_errno(CRT_EBADF); return (uint32_t)-1; }
    int r = crt_fd_new(dup(o->fd), !(A(1) & 0x8000));
    if (r >= 0) crt_fds[r].handle = A(0);
    return (uint32_t)r;
}

/* files and folders */
static int host_path(gp_t p, int wide, char *out)
{
    const char *w = wide ? wtmp(p) : gstr(p);
    if (!w || path_to_host(w, out) < 0) return -1;
    path_fix_case(out);
    return 0;
}
static uint32_t errno_ret(int r) { if (r < 0) { crt_set_errno(crt_errno_from_host(errno)); return (uint32_t)-1; } return 0; }
XDEF(remove) { char h[PATH_MAX]; if (host_path(A(0), 0, h) < 0) return (uint32_t)-1; return errno_ret(unlink(h)); }
XDEF(_unlink) { return api_remove(a); }
XDEF(_wremove) { char h[PATH_MAX]; if (host_path(A(0), 1, h) < 0) return (uint32_t)-1; return errno_ret(unlink(h)); }
XDEF(_wunlink) { return api__wremove(a); }
XDEF(rename) { char x[PATH_MAX], y[PATH_MAX]; if (host_path(A(0), 0, x) < 0 || path_to_host(S(1), y) < 0) return (uint32_t)-1; return errno_ret(rename(x, y)); }
XDEF(_wrename) { char x[PATH_MAX], y[PATH_MAX]; if (host_path(A(0), 1, x) < 0 || path_to_host(wtmp(A(1)), y) < 0) return (uint32_t)-1; return errno_ret(rename(x, y)); }
XDEF(_access) { char h[PATH_MAX]; if (host_path(A(0), 0, h) < 0) return (uint32_t)-1; return errno_ret(access(h, (int)(A(1) & 6))); }
XDEF(_waccess) { char h[PATH_MAX]; if (host_path(A(0), 1, h) < 0) return (uint32_t)-1; return errno_ret(access(h, (int)(A(1) & 6))); }
XDEF(_access_s) { char h[PATH_MAX]; if (host_path(A(0), 0, h) < 0) return CRT_ENOENT; return access(h, (int)(A(1) & 6)) < 0 ? (uint32_t)crt_errno_from_host(errno) : 0; }
XDEF(_chmod) { char h[PATH_MAX]; if (host_path(A(0), 0, h) < 0) return (uint32_t)-1; return errno_ret(chmod(h, A(1) & 0x80 ? 0666 : 0444)); }
XDEF(_mkdir) { char h[PATH_MAX]; if (path_to_host(S(0), h) < 0) return (uint32_t)-1; return errno_ret(mkdir(h, 0777)); }
XDEF(_wmkdir) { char h[PATH_MAX]; if (path_to_host(wtmp(A(0)), h) < 0) return (uint32_t)-1; return errno_ret(mkdir(h, 0777)); }
XDEF(_rmdir) { char h[PATH_MAX]; if (host_path(A(0), 0, h) < 0) return (uint32_t)-1; return errno_ret(rmdir(h)); }
XDEF(_wrmdir) { char h[PATH_MAX]; if (host_path(A(0), 1, h) < 0) return (uint32_t)-1; return errno_ret(rmdir(h)); }
XDEF(_chdir) { char h[PATH_MAX]; if (host_path(A(0), 0, h) < 0) return (uint32_t)-1; return errno_ret(chdir(h)); }
XDEF(_wchdir) { char h[PATH_MAX]; if (host_path(A(0), 1, h) < 0) return (uint32_t)-1; return errno_ret(chdir(h)); }
static uint32_t getcwd_common(uint32_t *a, int wide)
{
    char cwd[PATH_MAX], win[MAX_PATH];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");
    path_to_win(cwd, win, sizeof(win));
    uint32_t n = (uint32_t)strlen(win) + 1;
    gp_t out = A(0);
    if (!out) { out = galloc((A(1) > n ? A(1) : n) * (wide ? 2 : 1)); }
    else if (n > A(1)) { crt_set_errno(CRT_ERANGE); return 0; }
    if (wide) ansi_to_wide(win, -1, GP(out), (int)n); else memcpy(GP(out), win, n);
    return out;
}
XDEF(_getcwd) { return getcwd_common(a, 0); }
XDEF(_wgetcwd) { return getcwd_common(a, 1); }
XDEF(_getdrive) { (void)a; return 3; }
XDEF(_getdcwd) { uint32_t b[2] = { A(1), A(2) }; return getcwd_common(b, 0); }
XDEF(_fullpath)
{
    char host[PATH_MAX], win[MAX_PATH];
    if (path_to_host(S(1), host) < 0) return 0;
    path_to_win(host, win, sizeof(win));
    uint32_t n = (uint32_t)strlen(win) + 1;
    gp_t out = A(0) ? A(0) : galloc(n);
    if (A(0) && n > A(2)) return 0;
    memcpy(GP(out), win, n);
    return out;
}

/* _stat: the 32-bit layout (dev, ino, mode, nlink, uid, gid, rdev, size, atime, mtime, ctime) */
static uint32_t stat_common(const char *host, int fd, gp_t out, int big)
{
    struct stat st;
    int r = host ? stat(host, &st) : fstat(fd, &st);
    if (r < 0) { crt_set_errno(crt_errno_from_host(errno)); return (uint32_t)-1; }
    uint8_t *o = GP(out);
    uint16_t mode = (uint16_t)((S_ISDIR(st.st_mode) ? 0x4000 : S_ISCHR(st.st_mode) ? 0x2000 : 0x8000) |
                               0x100 | (st.st_mode & 0200 ? 0x80 : 0) | (S_ISDIR(st.st_mode) ? 0x40 : 0));
    memset(o, 0, big ? 56 : 36);
    uint32_t dev = 2;
    memcpy(o, &dev, 4);
    memcpy(o + 6, &mode, 2);
    uint16_t nlink = 1; memcpy(o + 8, &nlink, 2);
    memcpy(o + 16, &dev, 4);
    if (big) {
        uint64_t size = (uint64_t)st.st_size;
        int64_t at = st.st_atime, mt = st.st_mtime, ct = st.st_ctime;
        memcpy(o + 24, &size, 8); memcpy(o + 32, &at, 8); memcpy(o + 40, &mt, 8); memcpy(o + 48, &ct, 8);
    } else {
        uint32_t size = (uint32_t)st.st_size, at = (uint32_t)st.st_atime, mt = (uint32_t)st.st_mtime, ct = (uint32_t)st.st_ctime;
        memcpy(o + 20, &size, 4); memcpy(o + 24, &at, 4); memcpy(o + 28, &mt, 4); memcpy(o + 32, &ct, 4);
    }
    return 0;
}
XDEF(_stat) { char h[PATH_MAX]; if (host_path(A(0), 0, h) < 0) { crt_set_errno(CRT_ENOENT); return (uint32_t)-1; } return stat_common(h, -1, A(1), 0); }
XDEF(_stat32) { return api__stat(a); }
XDEF(_stati64) { char h[PATH_MAX]; if (host_path(A(0), 0, h) < 0) { crt_set_errno(CRT_ENOENT); return (uint32_t)-1; } return stat_common(h, -1, A(1), 1); }
XDEF(_stat64) { return api__stati64(a); }
XDEF(_wstat) { char h[PATH_MAX]; if (host_path(A(0), 1, h) < 0) { crt_set_errno(CRT_ENOENT); return (uint32_t)-1; } return stat_common(h, -1, A(1), 0); }
XDEF(_wstati64) { char h[PATH_MAX]; if (host_path(A(0), 1, h) < 0) { crt_set_errno(CRT_ENOENT); return (uint32_t)-1; } return stat_common(h, -1, A(1), 1); }
XDEF(_wstat64) { return api__wstati64(a); }
XDEF(_fstat) { struct crt_fd *f = crt_fd_get((int)A(0)); if (!f) { crt_set_errno(CRT_EBADF); return (uint32_t)-1; } return stat_common(NULL, f->fd, A(1), 0); }
XDEF(_fstat32) { return api__fstat(a); }
XDEF(_fstati64) { struct crt_fd *f = crt_fd_get((int)A(0)); if (!f) { crt_set_errno(CRT_EBADF); return (uint32_t)-1; } return stat_common(NULL, f->fd, A(1), 1); }
XDEF(_fstat64) { return api__fstati64(a); }

/* the console (conio) */
XDEF(_getch) { crt_flush_all(); return (uint32_t)console_getch(); }
XDEF(_getche) { crt_flush_all(); int c = console_getch(); if (c >= 0) { char ch = (char)c; console_write(1, &ch, 1); } return (uint32_t)c; }
XDEF(_getwch) { return api__getch(a); }
XDEF(_kbhit) { (void)a; crt_flush_all(); return (uint32_t)console_kbhit(); }
XDEF(_putch) { char c = (char)A(0); crt_flush_all(); console_write(1, &c, 1); return A(0) & 0xFF; }
XDEF(_putwch) { return api__putch(a); }
XDEF(_cputs) { crt_flush_all(); console_write(1, S(0), (uint32_t)strlen(S(0))); return 0; }
XDEF(_cprintf) { crt_flush_all(); char buf[4096]; int n = crt_vsnprintf(buf, sizeof(buf), S(0), a + 1, 0); console_write(1, buf, (uint32_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1)); return (uint32_t)n; }
XDEF(_ungetch) { (void)a; return (uint32_t)-1; }
XDEF(_getpid) { (void)a; return (uint32_t)getpid(); }
XDEF(_sleep) { usleep(A(0) * 1000); return 0; }
XDEF(_beep) { (void)a; os_beep(); return 0; }

/* ---- math: doubles come on the stack and go back in ST(0) ---- */

static double D(const uint32_t *a, int i) { double d; memcpy(&d, a + 2 * i, 8); return d; }
#define MATH1(nm, expr) XDEF(nm) { double x = D(a, 0); (void)x; api_set_fret(expr); return 0; }
#define MATH2(nm, expr) XDEF(nm) { double x = D(a, 0), y = D(a, 1); (void)x; (void)y; api_set_fret(expr); return 0; }
MATH1(sqrt, sqrt(x)) MATH1(sin, sin(x)) MATH1(cos, cos(x)) MATH1(tan, tan(x))
MATH1(asin, asin(x)) MATH1(acos, acos(x)) MATH1(atan, atan(x)) MATH2(atan2, atan2(x, y))
MATH1(exp, exp(x)) MATH1(log, log(x)) MATH1(log10, log10(x)) MATH2(pow, pow(x, y))
MATH1(floor, floor(x)) MATH1(ceil, ceil(x)) MATH1(fabs, fabs(x)) MATH2(fmod, fmod(x, y))
MATH1(sinh, (exp(x) - exp(-x)) / 2) MATH1(cosh, (exp(x) + exp(-x)) / 2)
MATH1(tanh, x > 20 ? 1.0 : x < -20 ? -1.0 : (exp(2 * x) - 1) / (exp(2 * x) + 1))
MATH2(_hypot, sqrt(x * x + y * y)) MATH2(hypot, sqrt(x * x + y * y))
MATH1(log2, log2(x)) MATH1(exp2, exp2(x)) MATH1(trunc, trunc(x)) MATH1(round, round(x))
MATH1(_logb, x == 0 ? -HUGE_VAL : floor(log2(fabs(x))))
MATH1(log1p, log(1 + x)) MATH1(expm1, exp(x) - 1) MATH1(cbrt, x < 0 ? -pow(-x, 1.0 / 3) : pow(x, 1.0 / 3))
MATH1(rint, x - floor(x) == 0.5 ? (fmod(floor(x), 2) == 0 ? floor(x) : ceil(x)) : round(x))
MATH1(nearbyint, x - floor(x) == 0.5 ? (fmod(floor(x), 2) == 0 ? floor(x) : ceil(x)) : round(x))
MATH2(fmin, x < y ? x : y) MATH2(fmax, x > y ? x : y) MATH2(copysign, (y < 0) != (x < 0) ? -x : x)
MATH2(_copysign, (y < 0) != (x < 0) ? -x : x) MATH2(_chgsign_dummy, -x)
XDEF(_chgsign) { api_set_fret(-D(a, 0)); return 0; }
XDEF(ldexp) { double x = D(a, 0); int e = (int)a[2]; api_set_fret(x * pow(2.0, e)); return 0; }
XDEF(frexp)
{
    double x = D(a, 0);
    int e = 0;
    if (x != 0 && x == x && x - x == 0) {
        e = (int)floor(log2(fabs(x))) + 1;
        x /= pow(2.0, e);
        if (fabs(x) >= 1) { x /= 2; e++; }
        if (fabs(x) < 0.5) { x *= 2; e--; }
    }
    *(int32_t *)gp(a[2]) = e;
    api_set_fret(x);
    return 0;
}
XDEF(modf) { double x = D(a, 0), i = trunc(x); memcpy(gp(a[2]), &i, 8); api_set_fret(x - i); return 0; }
XDEF(_isnan) { double x = D(a, 0); return x != x; }
XDEF(_finite) { double x = D(a, 0); return x == x && x - x == 0; }
XDEF(_fpclass) { double x = D(a, 0); if (x != x) return 2; if (x - x != 0) return x < 0 ? 4 : 0x200; if (x == 0) return 0x40; return x < 0 ? 8 : 0x100; }
XDEF(lround) { return (uint32_t)(int32_t)round(D(a, 0)); }
XDEF(lrint) { return (uint32_t)(int32_t)round(D(a, 0)); }
XDEF(llround) { int64_t v = (int64_t)round(D(a, 0)); api_edx = (uint32_t)((uint64_t)v >> 32); return (uint32_t)v; }
/* float versions: one float argument, the result in ST(0) */
static float FL(const uint32_t *a, int i) { float f; memcpy(&f, a + i, 4); return f; }
XDEF(sqrtf) { api_set_fret(sqrt(FL(a, 0))); return 0; }
XDEF(sinf) { api_set_fret(sin(FL(a, 0))); return 0; }
XDEF(cosf) { api_set_fret(cos(FL(a, 0))); return 0; }
XDEF(floorf) { api_set_fret(floor(FL(a, 0))); return 0; }
XDEF(ceilf) { api_set_fret(ceil(FL(a, 0))); return 0; }
XDEF(powf) { api_set_fret(pow(FL(a, 0), FL(a, 1))); return 0; }
XDEF(atan2f) { api_set_fret(atan2(FL(a, 0), FL(a, 1))); return 0; }
XDEF(fabsf) { api_set_fret(fabs(FL(a, 0))); return 0; }
XDEF(fmodf) { api_set_fret(fmod(FL(a, 0), FL(a, 1))); return 0; }
XDEF(expf) { api_set_fret(exp(FL(a, 0))); return 0; }
XDEF(logf) { api_set_fret(log(FL(a, 0))); return 0; }
XDEF(tanf) { api_set_fret(tan(FL(a, 0))); return 0; }
/* the compiler's helpers: the argument is ST(0) (and ST(1)) */
XDEF(_CIsqrt) { api_set_fret(sqrt(api_farg[0])); return 0; }
XDEF(_CIsin) { api_set_fret(sin(api_farg[0])); return 0; }
XDEF(_CIcos) { api_set_fret(cos(api_farg[0])); return 0; }
XDEF(_CItan) { api_set_fret(tan(api_farg[0])); return 0; }
XDEF(_CIatan) { api_set_fret(atan(api_farg[0])); return 0; }
XDEF(_CIexp) { api_set_fret(exp(api_farg[0])); return 0; }
XDEF(_CIlog) { api_set_fret(log(api_farg[0])); return 0; }
XDEF(_CIlog10) { api_set_fret(log10(api_farg[0])); return 0; }
XDEF(_CIasin) { api_set_fret(asin(api_farg[0])); return 0; }
XDEF(_CIacos) { api_set_fret(acos(api_farg[0])); return 0; }
XDEF(_CIpow) { api_set_fret(pow(api_farg[0], api_farg[1])); return 0; }
XDEF(_CIfmod) { api_set_fret(fmod(api_farg[0], api_farg[1])); return 0; }
XDEF(_CIatan2) { api_set_fret(atan2(api_farg[0], api_farg[1])); return 0; }
XDEF(_ftol) { int64_t v = (int64_t)api_farg[0]; api_edx = (uint32_t)((uint64_t)v >> 32); return (uint32_t)v; }
XDEF(_ftol2) { return api__ftol(a); }
XDEF(_ftol2_sse) { return api__ftol(a); }
/* 64-bit arithmetic helpers (stdcall: two 64-bit arguments) */
static int64_t I64(const uint32_t *a, int i) { int64_t v; memcpy(&v, a + 2 * i, 8); return v; }
static uint32_t R64(int64_t v) { api_edx = (uint32_t)((uint64_t)v >> 32); return (uint32_t)v; }
XDEF(_alldiv) { int64_t d = I64(a, 1); return R64(d ? I64(a, 0) / d : 0); }
XDEF(_allrem) { int64_t d = I64(a, 1); return R64(d ? I64(a, 0) % d : 0); }
XDEF(_aulldiv) { uint64_t d = (uint64_t)I64(a, 1); return R64(d ? (int64_t)((uint64_t)I64(a, 0) / d) : 0); }
XDEF(_aullrem) { uint64_t d = (uint64_t)I64(a, 1); return R64(d ? (int64_t)((uint64_t)I64(a, 0) % d) : 0); }
XDEF(_allmul) { return R64((int64_t)((uint64_t)I64(a, 0) * (uint64_t)I64(a, 1))); }

/* ---- setjmp and longjmp ----
 * MSVC's jmp_buf: Ebp, Ebx, Edi, Esi, Esp, Eip, Registration, TryLevel, ...
 * setjmp is called through a thunk, so the registers are those the gate
 * saved; after a cdecl call returns, esp points at the arguments. */
static uint32_t do_setjmp(uint32_t *a)
{
    uint32_t *jb = P(0);
    jb[0] = api_frame->ebp;
    jb[1] = api_frame->ebx;
    jb[2] = api_frame->edi;
    jb[3] = api_frame->esi;
    jb[4] = api_frame->args;
    jb[5] = api_frame->ret_addr;
    jb[6] = teb->except_list;
    jb[7] = 0xFFFFFFFFu;
    return 0;
}
XDEF(_setjmp) { return do_setjmp(a); }
XDEF(_setjmp3) { return do_setjmp(a); }
XDEF(setjmp) { return do_setjmp(a); }
XDEF(longjmp)
{
    const uint32_t *jb = P(0);
    struct guest_regs r = { jb[2], jb[3], jb[0], jb[4], jb[1], 0, 0, A(1) ? A(1) : 1, jb[5], 0x202 };
    if (jb[6] && teb->except_list != jb[6]) {
        /* unwind the SEH frames set up since */
        struct context ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.esp = jb[4];
        ctx.eip = jb[5];
        seh_unwind(jb[6], 0, NULL, 0, &ctx);
    }
    guest_resume(&r);
}
XDEF(_longjmpex) { return api_longjmp(a); }

/* ---- C++ support: enough for programs that only link it in ---- */
XDEF(_CxxThrowException)
{
    struct exception_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.code = 0xE06D7363;
    rec.flags = EXCEPTION_NONCONTINUABLE;
    rec.address = api_frame->ret_addr;
    rec.nparams = 3;
    rec.params[0] = 0x19930520;
    rec.params[1] = A(0);
    rec.params[2] = A(1);
    struct context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.flags = CONTEXT_FULL;
    ctx.ebp = api_frame->ebp; ctx.ebx = api_frame->ebx; ctx.edi = api_frame->edi; ctx.esi = api_frame->esi;
    ctx.esp = api_frame->args; ctx.eip = api_frame->ret_addr;
    seh_raise(&rec, &ctx);
    return 0;
}
XDEF(__CxxFrameHandler) { (void)a; return 1; }      /* ExceptionContinueSearch */
XDEF(__CxxFrameHandler3) { (void)a; return 1; }
XDEF(_except_handler3) { (void)a; return 1; }
XDEF(_except_handler4_common) { (void)a; return 1; }
XDEF(terminate) { (void)a; wlog("terminate called"); process_exit(3); }
XDEF(_purecall) { (void)a; wlog("pure virtual function call"); process_exit(3); }
XDEF(__uncaught_exception) { (void)a; return 0; }
XDEF(_set_purecall_handler) { (void)a; return 0; }
XDEF(set_terminate) { (void)a; return 0; }
XDEF(set_unexpected) { (void)a; return 0; }
XDEF(_set_se_translator) { (void)a; return 0; }
XDEF(__RTDynamicCast) { return A(0); }
XDEF(__std_terminate) { return api_terminate(a); }

/* ---- the export table ---- */

/* An entry whose x87 argument is ST(0) but whose result is an integer in edx:eax. */
#define APIX1(nm) { #nm, api_##nm, 0, API_CDECL | API_FARG1, NULL, 0 }
#define STD(nm, n) { #nm, api_##nm, (n), 0, NULL, 0 }

static gp_t iob_data, commode_data, fmode_data, initenv_data, wininitenv_data, mb_cur_max_data, argc_data, argv_data;

static const struct api_entry msvcrt_entries[] = {
    APIC(_errno), APIC(__doserrno), APIC(_get_errno), APIC(_set_errno), APIC(strerror), APIC(_strerror),
    APIC(strerror_s), APIC(perror),
    APIC(__getmainargs), APIC(__wgetmainargs), APIC(__p___argc), APIC(__p___argv), APIC(__p___wargv),
    APIC(__p__environ), APIC(__p__wenviron), APIC(__p__pgmptr), APIC(__p__wpgmptr), APIC(__p__fmode),
    APIC(__p__commode), APIC(__p__acmdln), APIC(__p__wcmdln), APIC(__p___mb_cur_max), APIC(___mb_cur_max_func),
    APIC(_get_pgmptr), APIC(_get_wpgmptr), APIC(__set_app_type), APIC(_set_app_type), APIC(_configure_narrow_argv),
    APIC(_configure_wide_argv), APIC(_initialize_narrow_environment), APIC(_initialize_wide_environment),
    APIC(_get_initial_narrow_environment), APIC(_get_initial_wide_environment), APIC(_set_fmode), APIC(_get_fmode),
    APIC(_setmode), APIC(_initterm), APIC(_initterm_e),
    APIC(atexit), APIC(_onexit), APIC(__dllonexit), APIC(_crt_atexit), APIC(_register_onexit_function),
    APIC(_initialize_onexit_table), APIC(_execute_onexit_table), APIC(at_quick_exit),
    APIC(exit), APIC(_exit), APIC(_Exit), APIC(quick_exit), APIC(_cexit), APIC(_c_exit), APIC(abort), APIC(_amsg_exit),
    APIC(_assert), APIC(_wassert), APIC(_invalid_parameter_noinfo), APIC(_invalid_parameter_noinfo_noreturn),
    APIC(_set_invalid_parameter_handler), APIC(_get_invalid_parameter_handler), APIC(_XcptFilter),
    APIC(__CppXcptFilter), APIC(_set_error_mode), APIC(_set_abort_behavior), APIC(__setusermatherr),
    APIC(_setusermatherr), APIC(_controlfp), APIC(_control87), APIC(__control87_2), APIC(_controlfp_s), APIC(_clearfp),
    APIC(_statusfp), APIC(_fpreset), APIC(_lock), APIC(_unlock), APIC(_lock_file), APIC(_unlock_file), APIC(__lconv_init),
    APIC(_configthreadlocale), APIC(_set_new_mode), APIC(_query_new_mode), APIC(_crt_debugger_hook),
    APIC(__crtSetUnhandledExceptionFilter), APIC(_seh_filter_exe), APIC(_seh_filter_dll), APIC(__iob_func),
    APIC(__acrt_iob_func), APIC(signal), APIC(raise), APIC(getenv), APIC(_putenv), APIC(_putenv_s), APIC(system),
    DATA(_iob, iob_data), DATA(_commode, commode_data), DATA(_fmode, fmode_data),
    DATA(__initenv, initenv_data), DATA(__winitenv, wininitenv_data), DATA(__mb_cur_max, mb_cur_max_data),
    DATA(__argc, argc_data), DATA(__argv, argv_data),
    APIC(malloc), APIC(calloc), APIC(realloc), APIC(free), APIC(_msize), APIC(_expand), APIC(_recalloc),
    APIC(_aligned_malloc), APIC(_aligned_free), APIC(_aligned_realloc), APIC(_get_heap_handle), APIC(_heapchk),
    APIC(_heapmin), APIC(_callnewh),
    { "??2@YAPAXI@Z", api_new_op, 0, API_CDECL, NULL, 0 }, { "??_U@YAPAXI@Z", api_new_op, 0, API_CDECL, NULL, 0 },
    { "??3@YAXPAX@Z", api_delete_op, 0, API_CDECL, NULL, 0 }, { "??_V@YAXPAX@Z", api_delete_op, 0, API_CDECL, NULL, 0 },
    APIC(memcpy), APIC(memmove), APIC(memset), APIC(memcmp), APIC(memchr), APIC(_memicmp), APIC(memcpy_s), APIC(memmove_s),
    APIC(strlen), APIC(strnlen), APIC(strcpy), APIC(strncpy), APIC(strcat), APIC(strncat), APIC(strcmp), APIC(strncmp),
    APIC(_stricmp), APIC(_strnicmp), ALIAS(_strcmpi, _stricmp), ALIAS(stricmp, _stricmp), ALIAS(strcmpi, _stricmp),
    ALIAS(strnicmp, _strnicmp), APIC(strcoll), APIC(_stricoll), APIC(strchr), APIC(strrchr), APIC(strstr), APIC(strpbrk),
    APIC(strspn), APIC(strcspn), APIC(_strdup), ALIAS(strdup, _strdup), APIC(_strlwr), APIC(_strupr), ALIAS(strlwr, _strlwr),
    ALIAS(strupr, _strupr), APIC(_strrev), APIC(_strset), APIC(_strnset), APIC(strtok), APIC(strtok_s), APIC(strcpy_s),
    APIC(strcat_s), APIC(strncpy_s), APIC(strncat_s),
    APIC(wcslen), APIC(wcsnlen), APIC(wcscpy), APIC(wcsncpy), APIC(wcscat), APIC(wcsncat), APIC(wcscmp), APIC(wcsncmp),
    APIC(_wcsicmp), APIC(_wcsnicmp), APIC(wcschr), APIC(wcsrchr), APIC(wcsstr), APIC(_wcsdup), APIC(_wcslwr), APIC(_wcsupr),
    APIC(wcscpy_s), APIC(wcscat_s), APIC(wmemcpy), APIC(wmemmove), APIC(wmemset),
    APIC(isalpha), APIC(isdigit), APIC(isalnum), APIC(isspace), APIC(isupper), APIC(islower), APIC(isxdigit), APIC(ispunct),
    APIC(isprint), APIC(isgraph), APIC(iscntrl), APIC(isblank), APIC(toupper), APIC(tolower), APIC(_isctype),
    ALIAS(_toupper, toupper), ALIAS(_tolower, tolower),
    APIC(iswalpha), APIC(iswdigit), APIC(iswspace), APIC(iswalnum), APIC(iswupper), APIC(iswlower), APIC(iswxdigit),
    APIC(iswpunct), APIC(iswprint), APIC(iswctype), ALIAS(is_wctype, iswctype), APIC(towupper), APIC(towlower),
    APIC(__pctype_func), APIC(__p__pctype), APIC(___lc_codepage_func), APIC(___lc_handle_func), APIC(setlocale),
    APIC(_wsetlocale), APIC(localeconv),
    APIC(atoi), APIC(atol), APIC(_atoi64), APIC(atoll), APIC(_wtoi), APIC(_wtol), APIC(strtol), APIC(strtoul),
    APIC(_strtoi64), APIC(_strtoui64), APIC(strtoll), APIC(strtoull), APIC(wcstol), APIC(wcstoul), APIC(_wcstoi64),
    APIC(_wcstoui64), APIF(strtod), APIF(atof), APIF(wcstod), APIF(_wtof), APIC(_itoa), APIC(_ltoa), APIC(_ultoa),
    APIC(_i64toa), APIC(_ui64toa), APIC(_itow), APIC(_ltow), APIC(_ultow), APIC(_itoa_s), APIC(_ltoa_s), APIC(_itow_s),
    ALIAS(itoa, _itoa), ALIAS(ltoa, _ltoa),
    APIC(abs), APIC(labs), APIC(_abs64), APIC(llabs), APIC(div), APIC(ldiv), APIC(rand), APIC(srand), APIC(rand_s),
    APIC(_rotl), APIC(_rotr), APIC(_byteswap_ulong), APIC(_byteswap_ushort), APIC(_byteswap_uint64), APIC(qsort), APIC(bsearch),
    APIC(printf), APIC(vprintf), APIC(fprintf), APIC(vfprintf), APIC(wprintf), APIC(vwprintf), APIC(fwprintf), APIC(vfwprintf),
    APIC(sprintf), APIC(vsprintf), APIC(_snprintf), APIC(_vsnprintf), APIC(snprintf), APIC(vsnprintf), APIC(sprintf_s),
    APIC(vsprintf_s), APIC(_snprintf_s), APIC(_vsnprintf_s), APIC(_scprintf), APIC(_vscprintf), APIC(_scwprintf),
    APIC(_vscwprintf), APIC(swprintf), APIC(_swprintf), APIC(vswprintf), APIC(_vswprintf), APIC(_snwprintf),
    APIC(_vsnwprintf), APIC(swprintf_s), APIC(vswprintf_s), APIC(_snwprintf_s),
    ALIAS(__mingw_printf, printf), ALIAS(__mingw_vprintf, vprintf), ALIAS(__mingw_fprintf, fprintf),
    ALIAS(__mingw_vfprintf, vfprintf), ALIAS(__mingw_sprintf, sprintf), ALIAS(__mingw_snprintf, snprintf),
    ALIAS(__mingw_vsnprintf, vsnprintf),
    APIC(sscanf), APIC(sscanf_s), APIC(vsscanf), APIC(swscanf), APIC(scanf), APIC(scanf_s), APIC(fscanf), APIC(fscanf_s),
    APIC(wscanf),
    APIC(fopen), APIC(_wfopen), APIC(fopen_s), APIC(_wfopen_s), APIC(_fsopen), APIC(_wfsopen), APIC(freopen), APIC(_fdopen),
    APIC(fclose), APIC(_fcloseall), APIC(fflush), APIC(_flushall), APIC(fwrite), APIC(fread), APIC(fputc), APIC(putc),
    APIC(_fputc_nolock), APIC(putchar), APIC(_putchar_nolock), APIC(fputs), APIC(puts), APIC(fputwc), APIC(putwc),
    APIC(putwchar), APIC(fputws), APIC(_putws), APIC(fgetc), APIC(getc), APIC(_fgetc_nolock), APIC(getchar), APIC(fgetwc),
    APIC(getwc), APIC(getwchar), APIC(ungetc), APIC(ungetwc), APIC(fgets), APIC(fgetws), APIC(gets), APIC(fseek),
    APIC(_fseeki64), APIC(fseeko64), APIC(ftell), APIC(_ftelli64), APIC(ftello64), APIC(fgetpos), APIC(fsetpos),
    APIC(rewind), APIC(feof), APIC(ferror), APIC(clearerr), APIC(_fileno), ALIAS(fileno, _fileno), APIC(setvbuf),
    APIC(setbuf), APIC(_filbuf), APIC(_flsbuf), APIC(tmpfile), APIC(tmpnam), ALIAS(_fdopen_a, _fdopen),
    ALIAS(fdopen, _fdopen),
    APIC(_open), APIC(_wopen), APIC(_sopen), APIC(_sopen_s), APIC(_wsopen_s), APIC(_creat), APIC(_close), APIC(_read),
    APIC(_write), APIC(_lseek), APIC(_lseeki64), APIC(_tell), APIC(_telli64), APIC(_eof), APIC(_filelength),
    APIC(_filelengthi64), APIC(_chsize), APIC(_chsize_s), APIC(_dup), APIC(_dup2), APIC(_isatty), APIC(_commit),
    APIC(_get_osfhandle), APIC(_open_osfhandle),
    ALIAS(open, _open), ALIAS(close, _close), ALIAS(read, _read), ALIAS(write, _write), ALIAS(lseek, _lseek),
    ALIAS(isatty, _isatty), ALIAS(dup, _dup), ALIAS(dup2, _dup2),
    APIC(remove), APIC(_unlink), APIC(_wremove), APIC(_wunlink), APIC(rename), APIC(_wrename), APIC(_access),
    APIC(_waccess), APIC(_access_s), APIC(_chmod), APIC(_mkdir), APIC(_wmkdir), APIC(_rmdir), APIC(_wrmdir), APIC(_chdir),
    APIC(_wchdir), APIC(_getcwd), APIC(_wgetcwd), APIC(_getdrive), APIC(_getdcwd), APIC(_fullpath),
    ALIAS(unlink, _unlink), ALIAS(access, _access), ALIAS(mkdir, _mkdir), ALIAS(rmdir, _rmdir), ALIAS(chdir, _chdir),
    ALIAS(getcwd, _getcwd), ALIAS(chmod, _chmod),
    APIC(_stat), APIC(_stat32), APIC(_stati64), APIC(_stat64), APIC(_wstat), APIC(_wstati64), APIC(_wstat64),
    APIC(_fstat), APIC(_fstat32), APIC(_fstati64), APIC(_fstat64), ALIAS(_stat64i32, _stat), ALIAS(_fstat64i32, _fstat),
    APIC(_getch), APIC(_getche), APIC(_getwch), APIC(_kbhit), APIC(_putch), APIC(_putwch), APIC(_cputs), APIC(_cprintf),
    APIC(_ungetch), APIC(_getpid), APIC(_sleep), APIC(_beep), ALIAS(getch, _getch), ALIAS(kbhit, _kbhit),
    ALIAS(getpid, _getpid),
    APIF(sqrt), APIF(sin), APIF(cos), APIF(tan), APIF(asin), APIF(acos), APIF(atan), APIF(atan2), APIF(exp), APIF(log),
    APIF(log10), APIF(pow), APIF(floor), APIF(ceil), APIF(fabs), APIF(fmod), APIF(sinh), APIF(cosh), APIF(tanh),
    APIF(_hypot), APIF(hypot), APIF(log2), APIF(exp2), APIF(trunc), APIF(round), APIF(_logb), APIF(log1p), APIF(expm1),
    APIF(cbrt), APIF(rint), APIF(nearbyint), APIF(fmin), APIF(fmax), APIF(copysign), APIF(_copysign), APIF(_chgsign),
    APIF(ldexp), APIF(frexp), APIF(modf), APIC(_isnan), APIC(_finite), APIC(_fpclass), APIC(lround), APIC(lrint),
    APIC(llround), APIF(sqrtf), APIF(sinf), APIF(cosf), APIF(floorf), APIF(ceilf), APIF(powf), APIF(atan2f), APIF(fabsf),
    APIF(fmodf), APIF(expf), APIF(logf), APIF(tanf),
    APIF1(_CIsqrt), APIF1(_CIsin), APIF1(_CIcos), APIF1(_CItan), APIF1(_CIatan), APIF1(_CIexp), APIF1(_CIlog),
    APIF1(_CIlog10), APIF1(_CIasin), APIF1(_CIacos), APIF2(_CIpow), APIF2(_CIfmod), APIF2(_CIatan2),
    APIX1(_ftol), APIX1(_ftol2), APIX1(_ftol2_sse),
    STD(_alldiv, 4), STD(_allrem, 4), STD(_aulldiv, 4), STD(_aullrem, 4), STD(_allmul, 4),
    APIC(_setjmp), APIC(_setjmp3), APIC(setjmp), APIC(longjmp), APIC(_longjmpex),
    { "_CxxThrowException", api__CxxThrowException, 2, 0, NULL, 0 },
    APIC(__CxxFrameHandler), APIC(__CxxFrameHandler3), APIC(_except_handler3), APIC(_except_handler4_common),
    APIC(terminate), APIC(_purecall), APIC(__uncaught_exception), APIC(_set_purecall_handler), APIC(set_terminate),
    APIC(set_unexpected), APIC(_set_se_translator), APIC(__RTDynamicCast), APIC(__std_terminate),
    { "?terminate@@YAXXZ", api_terminate, 0, API_CDECL, NULL, 0 },
};

static void msvcrt_dll_init(void)
{
    msvcrt_init();
    build_ctype();
    iob_data = crt_iob;
    commode_data = commode_cell;
    fmode_data = fmode_cell;
    initenv_data = envp_cell;
    wininitenv_data = wenvp_cell;
    mb_cur_max_data = mb_cur_max_cell;
    argc_data = argc_cell;
    argv_data = argv_cell;
}

extern const struct api_entry crt_time_entries[];
extern const int crt_time_count;

struct api_dll msvcrt_dll = { "MSVCRT.DLL", msvcrt_entries, sizeof(msvcrt_entries) / sizeof(msvcrt_entries[0]),
                              msvcrt_dll_init, { NULL }, { 0 }, NULL, NULL, 0 };

void msvcrt_link(void)
{
    msvcrt_dll.tables[0] = crt_time_entries;
    msvcrt_dll.counts[0] = crt_time_count;
}
