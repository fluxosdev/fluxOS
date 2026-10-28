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
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void test_misc(void)
{
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

int main(void)
{
    printf("libc guest: starting\n");
    test_format();
    test_files();
    test_processes();
    test_time();
    test_misc();
    printf("LIBC REGRESSIONS PASSED\n");
    fflush(stdout);
    stop();
    return 0;
}
