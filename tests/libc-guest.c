/*
 * The C library on the real kernel: runs as init in the disposable QEMU
 * environment (make test-libc-qemu) and prints LIBC REGRESSIONS PASSED.
 * What the host test (make test-libc) cannot check: stdio on real files,
 * processes (popen, system), descriptors and pipes, time.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <glob.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void stop(void)
{
    reboot(REBOOT_POWEROFF);
    for (;;)
        msleep(1000);
}

#define CHECK(x) do { if (!(x)) { \
    printf("LIBC REGRESSION FAILED line %d: %s (errno %d)\n", __LINE__, #x, errno); \
    fflush(stdout); \
    stop(); \
} } while (0)

static void test_format(void)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lld %llu %llx %.3f %e %g", -1234567890123LL, 18446744073709551615ULL,
             0x123456789abcdefULL, 3.14159, 1e-10, 0.0001);
    CHECK(!strcmp(buf, "-1234567890123 18446744073709551615 123456789abcdef 3.142 1.000000e-10 0.0001"));
    snprintf(buf, sizeof(buf), "%.17g", 0.1);
    CHECK(!strcmp(buf, "0.10000000000000001"));
    CHECK(strtod("0.10000000000000001", NULL) == 0.1);
    CHECK(strtoll("-9223372036854775808", NULL, 10) == INT64_MIN);
    uint64_t q = 123456789012345ULL;
    CHECK(q / 1000 == 123456789012ULL && q % 1000 == 345);
    int a;
    double d;
    char word[16];
    CHECK(sscanf("42 2.5 fluxOS", "%d %lf %15s", &a, &d, word) == 3 && a == 42 && d == 2.5 && !strcmp(word, "fluxOS"));
}

static void test_files(void)
{
    const char *path = "/tmp/libc-test.txt";
    FILE *f = fopen(path, "w+");
    CHECK(f);
    for (int i = 0; i < 2000; i++)
        CHECK(fprintf(f, "line %04d\n", i) == 10);
    CHECK(ftell(f) == 20000);
    CHECK(fseek(f, 10 * 1234, SEEK_SET) == 0);
    char line[32];
    CHECK(fgets(line, sizeof(line), f) && !strcmp(line, "line 1234\n"));
    CHECK(ftell(f) == 12350);
    /* read, then write at the same place: the read-ahead is given back */
    CHECK(fseek(f, 0, SEEK_CUR) == 0);
    CHECK(fputs("LINE", f) >= 0);
    CHECK(fseek(f, -4, SEEK_CUR) == 0);
    CHECK(fgets(line, sizeof(line), f) && !strcmp(line, "LINE 1235\n"));
    CHECK(ungetc('X', f) == 'X' && ftell(f) == 12359);
    CHECK(fgetc(f) == 'X');
    rewind(f);
    char big[20000];
    CHECK(fread(big, 1, sizeof(big), f) == sizeof(big) && !memcmp(big, "line 0000\n", 10));
    CHECK(fgetc(f) == EOF && feof(f));
    CHECK(fclose(f) == 0);

    f = fopen(path, "a");
    CHECK(f && fputs("tail\n", f) >= 0 && fclose(f) == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0 && st.st_size == 20005 && st.st_blocks == (20005 + 511) / 512 && st.st_blksize == 4096);
    CHECK(st.st_dev != 0 && st.st_mtime == st.st_ctime && st.st_nlink == 1);

    f = tmpfile();
    CHECK(f && fputs("temporary", f) >= 0);
    rewind(f);
    CHECK(fgets(line, sizeof(line), f) && !strcmp(line, "temporary"));
    fclose(f);

    char tmpl[] = "/tmp/libcXXXXXX";
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0 && strncmp(tmpl, "/tmp/libc", 9) == 0 && strcmp(tmpl + 9, "XXXXXX"));
    CHECK(write(fd, "x", 1) == 1);
    CHECK(fchmod(fd, 0600) == 0);
    close(fd);
    CHECK(unlink(tmpl) == 0);

    char dirt[] = "/tmp/dirXXXXXX";
    CHECK(mkdtemp(dirt));
    int dfd = open(dirt, O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0 && fchdir(dfd) == 0);
    char cwd[PATH_MAX], real[PATH_MAX];
    CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, dirt));
    CHECK(realpath("../../tmp/./", real) && !strcmp(real, "/tmp"));
    char *dyn = getcwd(NULL, 0);
    CHECK(dyn && !strcmp(dyn, dirt));
    free(dyn);
    /* realpath follows links: "l/.." is the parent of what l names */
    CHECK(mkdir("d", 0755) == 0 && mkdir("d/e", 0755) == 0 && symlink("d/e", "l") == 0);
    CHECK(realpath("l/..", real) && !strcmp(real + strlen(dirt), "/d"));
    CHECK(realpath("l", real) && !strcmp(real + strlen(dirt), "/d/e"));
    CHECK(unlink("l") == 0 && rmdir("d/e") == 0 && rmdir("d") == 0);
    CHECK(chdir("/") == 0 && rmdir(dirt) == 0);
    close(dfd);

    /* memory streams */
    char *mem;
    size_t memlen;
    FILE *ms = open_memstream(&mem, &memlen);
    CHECK(ms && fprintf(ms, "%d-%s", 7, "seven") == 7 && fclose(ms) == 0);
    CHECK(!strcmp(mem, "7-seven") && memlen == 7);
    free(mem);
    char fixed[16];
    FILE *fm = fmemopen(fixed, sizeof(fixed), "w");
    CHECK(fm && fputs("hello", fm) >= 0 && fclose(fm) == 0 && !strcmp(fixed, "hello"));

    /* fgets with room for the terminator only; fwrite of a size that overflows; fdopen "a" appends */
    f = fopen(path, "w+");
    CHECK(f && fputs("abc", f) >= 0 && fseek(f, 0, SEEK_SET) == 0);
    CHECK(fgets(line, 1, f) == line && line[0] == 0);
    CHECK(fwrite("x", SIZE_MAX / 4 + 1, 4, f) == 0 && errno == EOVERFLOW);
    CHECK(fclose(f) == 0);
    fd = open(path, O_WRONLY);
    CHECK(fd >= 0);
    f = fdopen(fd, "a");
    CHECK(f && fputs("d", f) >= 0 && fclose(f) == 0);
    CHECK(stat(path, &st) == 0 && st.st_size == 4);
    CHECK(unlink(path) == 0);
}

static void test_processes(void)
{
    FILE *p = popen("echo popen works", "r");
    CHECK(p);
    char line[64];
    CHECK(fgets(line, sizeof(line), p) && !strcmp(line, "popen works\n"));
    CHECK(pclose(p) == 0);
    int st = system("exit 3");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3);

    /* 256 descriptors */
    int fds[300], n = 0;
    while (n < 300 && (fds[n] = open("/dev/null", O_RDONLY)) >= 0)
        n++;
    CHECK(n >= 250 && errno == EMFILE);
    while (n)
        close(fds[--n]);

    /* a 64 KiB pipe takes 64 KiB without a reader */
    int pp[2];
    CHECK(pipe2(pp, O_CLOEXEC | O_NONBLOCK) == 0);
    CHECK(fcntl(pp[0], F_GETFD) & FD_CLOEXEC);
    static char chunk[65536];
    CHECK(write(pp[1], chunk, sizeof(chunk)) == (ssize_t)sizeof(chunk));
    CHECK(write(pp[1], chunk, 1) < 0 && errno == EAGAIN);
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(pp[0], &rd);
    struct timeval tv = { 0, 0 };
    CHECK(select(pp[0] + 1, &rd, NULL, NULL, &tv) == 1 && FD_ISSET(pp[0], &rd));
    close(pp[0]);
    close(pp[1]);

    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == 256);
    CHECK(sysconf(_SC_PAGESIZE) == 4096 && sysconf(_SC_OPEN_MAX) == 256);
    CHECK(getpwuid(0) && getpwuid(0)->pw_dir[0] == '/');
}

static volatile sig_atomic_t alarms;
static void on_alarm(int sig) { (void)sig; alarms++; }
static char alt_stack[16384];
static volatile int on_alt;
static void on_usr1(int sig)
{
    (void)sig;
    char here;
    on_alt = &here >= alt_stack && &here < alt_stack + sizeof(alt_stack);
}

/* Interval timers, sigpending, sigaltstack, sessions and long paths (phase 2 of the roadmap). */
static void test_signals_sessions_paths(void)
{
    signal(SIGALRM, on_alarm);
    struct itimerval it = { { 0, 20000 }, { 0, 20000 } }, got;
    CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);
    CHECK(getitimer(ITIMER_REAL, &got) == 0 && got.it_interval.tv_usec == 20000 && timerisset(&got.it_value));
    for (int i = 0; i < 200 && alarms < 3; i++)
        msleep(10);
    CHECK(alarms >= 3);                 /* it went off, and again */
    it.it_value.tv_sec = it.it_value.tv_usec = 0;
    CHECK(setitimer(ITIMER_REAL, &it, &got) == 0 && got.it_interval.tv_usec == 20000);
    int before = alarms;
    msleep(60);
    CHECK(alarms == before);            /* off */
    CHECK(alarm(5) == 0 && alarm(0) >= 4);
    alarms = 0;
    alarm(1);
    for (int i = 0; i < 300 && !alarms; i++)
        msleep(10);
    CHECK(alarms == 1);

    sigset_t block, pend;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    CHECK(sigprocmask(SIG_BLOCK, &block, NULL) == 0);
    signal(SIGUSR2, on_alarm);
    CHECK(kill(getpid(), SIGUSR2) == 0);
    CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR2));
    int was = alarms;
    CHECK(sigprocmask(SIG_UNBLOCK, &block, NULL) == 0 && alarms == was + 1);
    CHECK(sigpending(&pend) == 0 && !sigismember(&pend, SIGUSR2));

    stack_t ss = { alt_stack, 0, sizeof(alt_stack) }, old;
    CHECK(sigaltstack(&ss, &old) == 0 && old.ss_flags == SS_DISABLE);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sa.sa_flags = SA_ONSTACK;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    on_alt = 0;
    raise(SIGUSR1);
    CHECK(on_alt);                      /* the handler ran on the signal stack */
    ss.ss_flags = SS_DISABLE;
    CHECK(sigaltstack(&ss, NULL) == 0);
    on_alt = 1;
    raise(SIGUSR1);
    CHECK(!on_alt);                     /* and not once it is disabled */

    /* A child in a session of its own, with no terminal; its parent, a session leader, cannot setsid. */
    CHECK(setsid() < 0 && errno == EPERM);
    pid_t child = fork();
    if (child == 0) {
        pid_t sid = setsid();
        int ok = sid == getpid() && getsid(0) == getpid() && getpgrp() == getpid();
        ok = ok && setsid() < 0 && errno == EPERM;
        _exit(ok ? 0 : 1);
    }
    int st = 0;
    CHECK(waitpid(child, &st, 0) == child && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(getsid(0) == getsid(getpid()) && getsid(child) < 0 && errno == ESRCH);

    /* Paths up to PATH_MAX: twenty-five nested directories of 100-character names. */
    char *deep = malloc(PATH_MAX);
    CHECK(deep && PATH_MAX >= 4096);
    strcpy(deep, "/tmp");
    mkdir(deep, 0755);
    for (int i = 0; i < 25; i++) {
        size_t n = strlen(deep);
        deep[n] = '/';
        memset(deep + n + 1, 'a' + i, 100);
        deep[n + 101] = 0;
        CHECK(mkdir(deep, 0755) == 0);
    }
    CHECK(strlen(deep) > 2500);
    strcat(deep, "/file");
    int fd = open(deep, O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1 && close(fd) == 0);
    struct stat sb;
    CHECK(stat(deep, &sb) == 0 && sb.st_size == 1);
    CHECK(chdir(deep) < 0 && errno == ENOTDIR);
    deep[strlen(deep) - 5] = 0;
    CHECK(chdir(deep) == 0);
    char *cwd = malloc(PATH_MAX);
    CHECK(cwd && getcwd(cwd, PATH_MAX) && !strcmp(cwd, deep));
    CHECK(chdir("/") == 0 && unlink(strcat(deep, "/file")) == 0);
    memset(deep, 'z', PATH_MAX - 1);
    deep[0] = '/';
    deep[PATH_MAX - 1] = 0;
    CHECK(stat(deep, &sb) < 0 && errno == ENAMETOOLONG);       /* PATH_MAX counts the NUL */
    free(deep);
    free(cwd);
    /* symbolic links (ramfs keeps them; FAT refuses with EPERM, as on Linux) */
    char target[8];
    CHECK(symlink("a", "/tmp/b") == 0);
    CHECK(readlink("/tmp/b", target, sizeof(target)) == 1 && target[0] == 'a');
    CHECK(lstat("/tmp/b", &sb) == 0 && S_ISLNK(sb.st_mode) && stat("/tmp/b", &sb) < 0 && errno == ENOENT);
    CHECK(symlink("x", "/tmp/b") < 0 && errno == EEXIST);
    CHECK(unlink("/tmp/b") == 0 && lstat("/tmp/b", &sb) < 0);
    CHECK(readlink("/tmp", target, sizeof(target)) < 0 && errno == EINVAL);
    CHECK(readlink("/tmp", NULL, 0) < 0 && errno == EINVAL);
}

static void test_time(void)
{
    struct timespec a, b;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    struct timespec nap = { 0, 120 * 1000000 };
    CHECK(nanosleep(&nap, NULL) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 110 && ms < 1000);
    struct tm tm = { .tm_year = 100, .tm_mon = 1, .tm_mday = 29, .tm_hour = 12 };
    CHECK(timegm(&tm) == 951825600 && tm.tm_wday == 2 && tm.tm_yday == 59);
    char buf[64];
    CHECK(strftime(buf, sizeof(buf), "%F %T %a %j", &tm) && !strcmp(buf, "2000-02-29 12:00:00 Tue 060"));
}

/* strtod and regexec from several threads at once */
static void *libc_race(void *arg)
{
    intptr_t bad = 0;
    regex_t re;
    regmatch_t m[3];
    int which = (int)(intptr_t)arg & 1;
    regcomp(&re, which ? "([a-z]+)-([0-9]+)" : "(foo|bar)+", REG_EXTENDED);
    for (int i = 0; i < 2000; i++) {
        bad += strtod(which ? "3.14159265358979323846" : "2.2250738585072014e-308", NULL) !=
               (which ? 3.14159265358979323846 : 2.2250738585072014e-308);
        int r = regexec(&re, which ? "xx abc-123 yy" : "..foobarfoo..", 3, m, 0);
        bad += which ? r || m[2].rm_so != 7 || m[2].rm_eo != 10 : r || m[0].rm_so != 2 || m[0].rm_eo != 11;
    }
    regfree(&re);
    return (void *)bad;
}

static void test_misc(void)
{
    /* exact powers are exact; halves round away from zero; huge arguments stay in range */
    CHECK(pow(10, 2) == 100 && pow(10, 15) == 1e15 && pow(3, 10) == 59049 && pow(2, -3) == 0.125);
    CHECK(pow(1, NAN) == 1 && pow(-0.0, -1) == -HUGE_VAL && pow(-2, 3) == -8 && isnan(pow(-8, 1.0 / 3)));
    CHECK(fabs(pow(2, 0.5) - 1.4142135623730951) < 1e-15);
    CHECK(round(0.49999999999999994) == 0 && round(4503599627370497.0) == 4503599627370497.0 && round(-2.5) == -3);
    CHECK(fabs(sin(1e20)) <= 1 && fabs(cos(1e300)) <= 1);
    pthread_t t[4];
    intptr_t bad = 0;
    for (intptr_t i = 0; i < 4; i++)
        CHECK(pthread_create(&t[i], NULL, libc_race, (void *)i) == 0);
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(t[i], &r);
        bad += (intptr_t)r;
    }
    CHECK(bad == 0);
    struct timespec neg = { -1, 0 };
    CHECK(nanosleep(&neg, NULL) < 0 && errno == EINVAL);
    struct timeval ntv = { -1, 0 };
    CHECK(select(0, NULL, NULL, NULL, &ntv) < 0 && errno == EINVAL);

    regex_t re;
    regmatch_t m[3];
    CHECK(regcomp(&re, "([a-z]+)=([0-9]+)", REG_EXTENDED) == 0);
    CHECK(regexec(&re, "  cc=42;", 3, m, 0) == 0 && m[1].rm_so == 2 && m[2].rm_eo == 7);
    regfree(&re);
    glob_t g;
    CHECK(glob("/bin/f*", 0, NULL, &g) == 0 && g.gl_pathc >= 2);
    globfree(&g);
    void *p = memalign(4096, 100);
    CHECK(p && !((uintptr_t)p & 4095));
    free(p);
    CHECK(posix_memalign(&p, 64, 1000) == 0 && !((uintptr_t)p & 63));
    free(p);
}

/*
 * Where things are in a 64-bit program: what it allocates, and its stack,
 * have addresses that do not fit 32 bits, and every call that takes or
 * returns an address takes and returns all of it.
 */
static volatile sig_atomic_t wide_fault_seen;
static void *volatile wide_fault_at;   /* (set by the handler: the compiler must not think it knows it) */

static void wide_fault(int sig, siginfo_t *info, void *context)
{
    (void)sig; (void)context;
    void *at = (void *)info->si_addr;
    wide_fault_at = at;
    wide_fault_seen = 1;
    /* the page becomes readable and writable: the access goes through */
    mprotect((void *)((uintptr_t)at & ~(uintptr_t)4095), 4096, PROT_READ | PROT_WRITE);
}

static void *wide_thread(void *arg)
{
    int local = 7;
    *(uintptr_t *)arg = (uintptr_t)&local;
    return (void *)((uintptr_t)arg + 1);        /* an address above 4 GiB comes back whole */
}

static void test_address_space(void)
{
#ifdef __x86_64__
    int local = 0;
    char *heap = malloc(100);
    char *map = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(heap && map != MAP_FAILED);
    CHECK((uintptr_t)heap >> 32 && (uintptr_t)map >> 32 && (uintptr_t)&local >> 32);
    CHECK((uintptr_t)sbrk(0) >> 32);
    /* the program itself is where it was linked for, below */
    CHECK(!((uintptr_t)test_address_space >> 32) && !((uintptr_t)&wide_fault_seen >> 32));

    /* the stack the C library reports is the one this runs on */
    void *low, *high;
    flux_main_stack(&low, &high);
    CHECK((char *)&local > (char *)low && (char *)&local < (char *)high && !((uintptr_t)high & 4095));
    pthread_attr_t attr;
    void *base;
    size_t size;
    CHECK(pthread_getattr_np(pthread_self(), &attr) == 0 && pthread_attr_getstack(&attr, &base, &size) == 0);
    CHECK(base == low && (char *)base + size == (char *)high);

    /* system calls with buffers up there: a file, a pipe, a path, a string vector */
    int fd = open("/tmp/wide.txt", O_RDWR | O_CREAT | O_TRUNC, 0600);
    strcpy(map, "written from above 4 GiB\n");
    CHECK(fd >= 0 && write(fd, map, 25) == 25 && lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, heap, 25) == 25 && !memcmp(heap, map, 25));
    close(fd);
    strcpy(map + 4096, "/tmp/wide.txt");
    struct stat st;
    CHECK(stat(map + 4096, &st) == 0 && st.st_size == 25 && unlink(map + 4096) == 0);
    int fds[2];
    CHECK(pipe(fds) == 0 && write(fds[1], heap, 25) == 25 && read(fds[0], map + 8192, 25) == 25);
    CHECK(!memcmp(map + 8192, map, 25));
    close(fds[0]);
    close(fds[1]);

    /* a fault's address comes to the handler whole, on a signal stack that is up there too */
    stack_t ss = { .ss_sp = malloc(65536), .ss_size = 65536 }, old;
    CHECK(ss.ss_sp && sigaltstack(&ss, &old) == 0);
    struct sigaction sa, osa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = wide_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    CHECK(sigaction(SIGSEGV, &sa, &osa) == 0);
    char *guarded = map + (512 << 10);
    CHECK(mprotect(guarded, 4096, PROT_NONE) == 0);
    *(volatile char *)(guarded + 123) = 'x';    /* faults once; the handler lets it through */
    CHECK(wide_fault_seen && wide_fault_at == guarded + 123 && guarded[123] == 'x');
    struct sigaction back;
    CHECK(sigaction(SIGSEGV, &osa, &back) == 0 && back.sa_sigaction == wide_fault);
    CHECK(sigaltstack(&old, &ss) == 0 && (uintptr_t)ss.ss_sp >> 32 && ss.ss_size == 65536);
    free(ss.ss_sp);

    /* a thread: its stack and its thread block are mappings, up there */
    uintptr_t where = 0;
    pthread_t t;
    void *result;
    CHECK(pthread_create(&t, NULL, wide_thread, &where) == 0 && pthread_join(t, &result) == 0);
    CHECK(where >> 32 && result == (void *)((uintptr_t)&where + 1));

    /* copy-on-write and a shared mapping across fork, by address */
    int *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED && (uintptr_t)shared >> 32);
    heap[0] = 'p';
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        heap[0] = 'c';
        *shared = (int)getpid();
        _exit(heap[0] == 'c' && !memcmp(map, "written", 7) ? 0 : 1);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(heap[0] == 'p' && *shared == (int)pid);
    CHECK(munmap(shared, 4096) == 0 && munmap(map, 1 << 20) == 0);
    free(heap);

    /* more than a 32-bit program could ask for in one piece: the address space is there
     * (the memory is not, until it is used: only two pages of it are) */
    size_t huge = (size_t)3 << 32;
    char *big = mmap(NULL, huge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(big != MAP_FAILED);
    big[0] = 1;
    big[huge - 1] = 2;
    CHECK(big[0] == 1 && big[huge - 1] == 2 && big[huge / 2] == 0);
    CHECK(munmap(big, huge) == 0);
#endif
}

/* What a shell command prints, the lines of it one after another in out. */
static void output_of(const char *command, char *out, size_t size)
{
    FILE *p = popen(command, "r");
    CHECK(p);
    size_t n = fread(out, 1, size - 1, p);
    out[n] = 0;
    CHECK(pclose(p) == 0);
}

/*
 * Files of 2 GiB and more, which a 64-bit program's off_t (64 bits) tells
 * of: sparse ones on an ext2 volume made on the scratch disk, whose 32 MiB
 * hold the few blocks they have. Then a FAT volume, whose files end before
 * 4 GiB.
 */
static void test_large_files(void)
{
#ifdef __x86_64__
    const off_t G = (off_t)1 << 30;
    struct stat st;
    char buf[16], text[512];
    CHECK(sizeof(off_t) == 8 && sizeof(st.st_size) == 8 && sizeof(ssize_t) == 8);
    CHECK(mkdir("/mnt/big", 0755) == 0 || errno == EEXIST);
    CHECK(system("/bin/mkfs.ext2 -q /dev/hda") == 0);
    CHECK(mount("/dev/hda", "/mnt/big", "ext2") == 0);
    int fd = open("/mnt/big/sparse", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);

    /* positions past 2 and 4 GiB, set and told whole */
    CHECK(lseek(fd, 2 * G - 3, SEEK_SET) == 2 * G - 3 && write(fd, "two gig", 7) == 7);
    CHECK(lseek(fd, 0, SEEK_CUR) == 2 * G + 4);
    CHECK(lseek(fd, 5 * G + 100, SEEK_SET) == 5 * G + 100 && write(fd, "five gig", 8) == 8);
    CHECK(lseek(fd, 0, SEEK_END) == 5 * G + 108 && lseek(fd, -8, SEEK_END) == 5 * G + 100);
    CHECK(lseek64(fd, 0, SEEK_CUR) == 5 * G + 100);
    CHECK(read(fd, buf, 16) == 8 && !memcmp(buf, "five gig", 8));
    CHECK(pread(fd, buf, 7, 2 * G - 3) == 7 && !memcmp(buf, "two gig", 7));
    CHECK(pwrite(fd, "X", 1, 4 * G + 1) == 1 && pread(fd, buf, 3, 4 * G) == 3 && !memcmp(buf, "\0X\0", 3));
    CHECK(pread(fd, buf, 8, 3 * G) == 8 && !memcmp(buf, "\0\0\0\0\0\0\0\0", 8));    /* a hole */
    CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 5 * G + 108);
    CHECK(stat("/mnt/big/sparse", &st) == 0 && st.st_size == 5 * G + 108);
    CHECK(st.st_blocks == (blkcnt_t)((5 * G + 108 + 511) / 512));

    /* cut short and grown, past 4 GiB both ways */
    CHECK(ftruncate(fd, 3 * G) == 0 && fstat(fd, &st) == 0 && st.st_size == 3 * G);
    CHECK(pread(fd, buf, 7, 2 * G - 3) == 7 && !memcmp(buf, "two gig", 7));
    CHECK(ftruncate(fd, 6 * G) == 0 && lseek(fd, 0, SEEK_END) == 6 * G);
    CHECK(pread(fd, buf, 8, 5 * G + 100) == 8 && !memcmp(buf, "\0\0\0\0\0\0\0\0", 8));
    CHECK(truncate("/mnt/big/sparse", 5 * G + 4096) == 0);
    CHECK(ftruncate(fd, -1) == -1 && errno == EINVAL);

    /* a piece of it past 4 GiB, mapped: the file's bytes, and none after its end */
    CHECK(pwrite(fd, "mapped", 6, 5 * G + 10) == 6);
    char *m = mmap(NULL, 8192, PROT_READ, MAP_PRIVATE, fd, 5 * G);
    CHECK(m != MAP_FAILED && !memcmp(m + 10, "mapped", 6) && m[0] == 0 && m[4096 + 5] == 0);
    CHECK(munmap(m, 8192) == 0);

    /* the standard I/O library's positions */
    FILE *f = fopen("/mnt/big/sparse", "r+");
    fpos_t at;
    CHECK(f && fseeko(f, 5 * G + 10, SEEK_SET) == 0 && ftello(f) == 5 * G + 10 && fgetpos(f, &at) == 0);
    CHECK(fread(buf, 1, 6, f) == 6 && !memcmp(buf, "mapped", 6) && ftell(f) == 5 * G + 16);
    CHECK(fseek(f, 0, SEEK_END) == 0 && ftell(f) == 5 * G + 4096);
    CHECK(fsetpos(f, &at) == 0 && fgetc(f) == 'm' && ftello(f) == 5 * G + 11);
    CHECK(fseeko(f, 4 * G + 1, SEEK_SET) == 0 && fputc('Y', f) == 'Y' && fclose(f) == 0);
    CHECK(pread(fd, buf, 1, 4 * G + 1) == 1 && buf[0] == 'Y');

    /* a 32-bit program's view of it: no size it can tell, positions past 2 GiB by lseek64 */
    CHECK(system("/test/libc32 /mnt/big/sparse") == 0);

    /* a volume of 1 KiB blocks holds files up to 16 GiB and no larger */
    CHECK(ftruncate(fd, 17 * G) == -1 && errno == EFBIG);
    CHECK(pwrite(fd, "x", 1, 17 * G) == -1 && errno == EFBIG);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 5 * G + 4096);
    CHECK(close(fd) == 0);

    /* what the tools make of it */
    output_of("ls -l /mnt/big/sparse; stat /mnt/big/sparse | grep Size; ls -lh /mnt/big/sparse", text, sizeof(text));
    CHECK(strstr(text, " 5368713216 ") && strstr(text, "Size: 5368713216 ") && strstr(text, " 5.0G "));
    output_of("truncate -s 7G /mnt/big/sparse && ls -l /mnt/big/sparse &&"
              " dd if=/mnt/big/sparse bs=1 skip=5368709130 count=6 2>/dev/null", text, sizeof(text));
    CHECK(strstr(text, " 7516192768 ") && strstr(text, "\nmapped"));

    /* it is all there after a remount, and the volume is whole */
    CHECK(umount("/mnt/big") == 0);
    CHECK(system("/bin/fsck.ext2 /dev/hda >/dev/null") == 0);
    CHECK(mount("/dev/hda", "/mnt/big", "ext2") == 0);
    CHECK(stat("/mnt/big/sparse", &st) == 0 && st.st_size == 7 * G);
    fd = open("/mnt/big/sparse", O_RDONLY);
    CHECK(fd >= 0 && pread(fd, buf, 6, 5 * G + 10) == 6 && !memcmp(buf, "mapped", 6) && close(fd) == 0);
    CHECK(unlink("/mnt/big/sparse") == 0 && umount("/mnt/big") == 0);

    /* FAT: a file's size is 32 bits there, and the kernel says so rather than wrap round */
    CHECK(system("/bin/mkfs.fat -F 32 /dev/hda >/dev/null") == 0);
    CHECK(mount("/dev/hda", "/mnt/big", "fat32") == 0);
    fd = open("/mnt/big/FILE.BIN", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "fat", 3) == 3);
    CHECK(ftruncate(fd, 4 * G) == -1 && errno == EFBIG);
    CHECK(pwrite(fd, "x", 1, 4 * G - 1) == -1 && errno == EFBIG);
    CHECK(pwrite(fd, "x", 1, 5 * G) == -1 && errno == EFBIG);
    CHECK(lseek(fd, 5 * G, SEEK_SET) == 5 * G && read(fd, buf, 4) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 3 && close(fd) == 0);
    CHECK(umount("/mnt/big") == 0);

    /* a disk's device says how large it is */
    CHECK(stat("/dev/hda", &st) == 0 && S_ISBLK(st.st_mode) && st.st_size == 32 << 20);
    printf("libc guest: files past 4 GiB: positions, sizes, truncation, mappings and the tools\n");
#endif
}

/* mib MiB of stack, a byte of every page of it written and found again. */
static __attribute__((noinline)) int descend(int mib)
{
    volatile char frame[1 << 20];
    for (size_t i = 0; i < sizeof(frame); i += 4096)
        frame[i] = (char)mib;
    int r = mib > 1 ? descend(mib - 1) : 0;
    for (size_t i = 0; i < sizeof(frame); i += 4096)
        if (frame[i] != (char)mib)
            r = -1;
    return r;
}

/* A child that uses that much stack: 0 if it could, the signal that ended it (negative) if not. */
static int stack_child(int mib)
{
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid)
        _exit(descend(mib) == 0 ? 0 : 1);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    return WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
}

/*
 * The stack's limit: 32 MiB to begin with, which setrlimit raises (a 64-bit
 * program's as far as it likes) and the programs run from this one keep.
 */
static void test_stack_limit(void)
{
    struct rlimit was, rl;
    char text[128];
    CHECK(getrlimit(RLIMIT_STACK, &was) == 0 && was.rlim_cur == 32ul << 20);
    CHECK(stack_child(24) == 0);
    CHECK(stack_child(48) == -SIGSEGV);
#ifdef __x86_64__
    CHECK(was.rlim_max == RLIM_INFINITY);
    rl = (struct rlimit){ 128ul << 20, RLIM_INFINITY };
    CHECK(setrlimit(RLIMIT_STACK, &rl) == 0);
    CHECK(getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur == 128ul << 20 && rl.rlim_max == RLIM_INFINITY);
    CHECK(stack_child(100) == 0);               /* (a child has the limit of the process it was forked from) */
    CHECK(stack_child(160) == -SIGSEGV);
    void *low, *high;
    flux_main_stack(&low, &high);
    CHECK((size_t)((char *)high - (char *)low) == 128ul << 20);

    /* a program run from this one starts with it, and the shell's ulimit reads and sets it */
    output_of("ulimit -s; ulimit -s 65536; ulimit -s; /bin/sh -c 'ulimit -s'; ulimit -S -s 1024; ulimit -s; ulimit -H -s;"
              " ulimit -s unlimited; ulimit -s", text, sizeof(text));
    CHECK(!strcmp(text, "131072\n65536\n65536\n1024\n65536\nunlimited\n"));

    /* a soft limit above the hard one, or one that no stack fits, is refused */
    rl = (struct rlimit){ 64ul << 20, 32ul << 20 };
    CHECK(setrlimit(RLIMIT_STACK, &rl) == -1 && errno == EINVAL);
    rl = (struct rlimit){ 4096, RLIM_INFINITY };
    CHECK(setrlimit(RLIMIT_STACK, &rl) == -1 && errno == EINVAL);
    CHECK(getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur == 128ul << 20);

    /* no limit: as much as there is memory for (here, more than the 32 MiB it began with allow) */
    rl = (struct rlimit){ RLIM_INFINITY, RLIM_INFINITY };
    CHECK(setrlimit(RLIMIT_STACK, &rl) == 0 && stack_child(200) == 0);
    CHECK(setrlimit(RLIMIT_STACK, &was) == 0 && stack_child(48) == -SIGSEGV);
    printf("libc guest: the stack's limit: 32 MiB, raised, lifted and kept by a child\n");
#endif
}

int main(void)
{
    printf("libc guest: starting\n");
    printf("guest: %d CPUs online\n", (int)sysconf(_SC_NPROCESSORS_ONLN));   /* the script checks it */
    test_format();
    test_address_space();
    test_stack_limit();
    test_large_files();
    test_files();
    test_processes();
    test_time();
    test_misc();
    test_signals_sessions_paths();
    printf("LIBC REGRESSIONS PASSED\n");
    fflush(stdout);
    stop();
    return 0;
}
