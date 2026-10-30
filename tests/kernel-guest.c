/* Runs as init only in the disposable QEMU regression environment. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <flux/syscall.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
static void stop(void)
{
    reboot(REBOOT_POWEROFF);
    for (;;) msleep(1000);
}
#define CHECK(x) do { if (!(x)) { \
    printf("KERNEL REGRESSION FAILED line %d: %s (errno %d)\n", __LINE__, #x, errno); \
    stop(); \
} } while (0)

static void scheduler_stress(void)
{
    struct meminfo mi;
    CHECK(meminfo(&mi) == 0);
    printf("guest: %u CPUs online\n", mi.ncpu);
    int children[8], workers = mi.ncpu < 4 ? (int)mi.ncpu * 2 : 8;
    for (int i = 0; i < workers; i++) {
        children[i] = fork(); CHECK(children[i] >= 0);
        if (!children[i]) {
            for (int round = 0; round < 6; round++) {
                float seed[4] = { i + 1, i + 2, i + 3, i + 4 };
                const float ones[4] __attribute__((aligned(16))) = { 1, 1, 1, 1 };
                float out[4];
                /* Keep live SSE state across raw yields: a normal C call
                 * could spill it and hide a broken context switch. */
                __asm__ volatile(
                    "movups %1, %%xmm7; movl $200000, %%ecx\n"
                    "1: addps %2, %%xmm7; decl %%ecx\n"
                    "testl $8191, %%ecx; jnz 2f\n"
                    "movl %3, %%eax; int $0x80\n"
                    "2: testl %%ecx, %%ecx; jnz 1b\n"
                    "movups %%xmm7, %0"
                    : "=m"(out) : "m"(seed), "m"(ones), "i"(SYS_yield)
                    : "eax", "ecx", "xmm7", "memory", "cc");
                for (int k = 0; k < 4; k++)
                    if (out[k] != seed[k] + 200000.0f) _exit(1);
                msleep(10);
            }
            _exit(0);
        }
    }
    int status;
    for (int i = 0; i < workers; i++)
        CHECK(waitpid(children[i], &status, 0) == children[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* More CPU-bound processes than cores: every child must reach its
     * handshake, and the parent must wake and kill them without yields. */
    int gate[2]; CHECK(pipe(gate) == 0);
    for (int i = 0; i < workers; i++) {
        children[i] = fork(); CHECK(children[i] >= 0);
        if (!children[i]) {
            close(gate[0]);
            if (write(gate[1], "r", 1) != 1) _exit(1);
            close(gate[1]);
            for (;;) __asm__ volatile("rep; nop" ::: "memory");
        }
    }
    close(gate[1]);
    for (int i = 0; i < workers; i++) { char c; CHECK(read(gate[0], &c, 1) == 1); }
    close(gate[0]);
    CHECK(msleep(120) == 0);
    for (int i = 0; i < workers; i++) CHECK(kill(children[i], SIGKILL) == 0);
    for (int i = 0; i < workers; i++)
        CHECK(waitpid(children[i], &status, 0) == children[i] && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    puts("guest: scheduler saturation, timed wakeups, signals and live SSE state passed");
}

static void shared_offset_stress(void)
{
    /* A zero-distance SEEK_CUR only observes the shared offset: it must
     * never roll back a write made by the other process. Exercise both
     * syscall entry points through libc on the real SMP kernel. */
    for (int wide = 0; wide < 2; wide++) {
        int fd = open("/tmp/seek-race", O_CREAT | O_RDWR | O_TRUNC, 0600);
        CHECK(fd >= 0);
        int gate[2]; CHECK(pipe(gate) == 0);
        int child = fork(); CHECK(child >= 0);
        if (!child) {
            close(gate[0]);
            if (write(gate[1], "r", 1) != 1) _exit(1);
            close(gate[1]);
            for (;;) {
                if (wide ? lseek64(fd, 0, SEEK_CUR) < 0 : lseek(fd, 0, SEEK_CUR) < 0)
                    _exit(1);
            }
        }
        close(gate[1]);
        char ready;
        CHECK(read(gate[0], &ready, 1) == 1);
        close(gate[0]);
        const int count = 100000;
        for (int i = 0; i < count; i++)
            CHECK(write(fd, "x", 1) == 1);
        CHECK(kill(child, SIGKILL) == 0);
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        struct stat st;
        CHECK(fstat(fd, &st) == 0 && st.st_size == count);
        CHECK(lseek64(fd, 0, SEEK_CUR) == count);
        CHECK(close(fd) == 0);
    }
    CHECK(unlink("/tmp/seek-race") == 0);
    puts("guest: shared lseek/lseek64 offsets preserve concurrent writes passed");
}

static void cached_files(void)
{
    static unsigned char block[64*1024];
    const char *paths[]={"/mnt/regress/CACHE-A.BIN","/mnt/regress/CACHE-B.BIN"};
    int files[2];
    for(int f=0;f<2;f++) {
        files[f]=open(paths[f],O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(files[f]>=0);
        for(int k=0;k<48;k++) {
            memset(block,k+f*64,sizeof(block));
            CHECK(write(files[f],block,sizeof(block))==(int)sizeof(block));
        }
        CHECK(fsync(files[f])==0);
    }
    /* Six MiB across two open descriptions, checked cold and warm. */
    for(int pass=0;pass<2;pass++) {
        for(int f=0;f<2;f++)CHECK(lseek(files[f],0,SEEK_SET)==0);
        for(int k=0;k<48;k++)for(int f=0;f<2;f++) {
            CHECK(read(files[f],block,sizeof(block))==(int)sizeof(block));
            for(unsigned i=0;i<sizeof(block);i++)CHECK(block[i]==k+f*64);
        }
    }
    CHECK(lseek(files[0],512,SEEK_SET)==512 && write(files[0],"fresh",5)==5);
    CHECK(lseek(files[0],512,SEEK_SET)==512 && read(files[0],block,5)==5 && !memcmp(block,"fresh",5));
    for(int f=0;f<2;f++){CHECK(close(files[f])==0);CHECK(unlink(paths[f])==0);}
    puts("guest: interleaved 6 MiB file reads, warm rereads and cached write coherence passed");
}

static volatile sig_atomic_t usr1_count, usr2_count, chld_count, last_si_signo;
static volatile uint32_t last_si_addr;
static void *segv_fix_page;
static sigjmp_buf sig_jump_env;

static void on_usr1(int sig)
{
    if (sig == SIGUSR1)
        usr1_count++;
    float clobber[4] = { 99, 98, 97, 96 };
    __asm__ volatile("movups %0, %%xmm7" :: "m"(clobber) : "xmm7");
}

static void on_usr2_info(int sig, siginfo_t *info, void *ucontext)
{
    (void)ucontext;
    usr2_count++;
    if (info)
        last_si_signo = info->si_signo;
    else
        last_si_signo = -sig;
}

static void on_chld(int sig)
{
    if (sig == SIGCHLD)
        chld_count++;
}

static void on_usr1_longjmp(int sig)
{
    usr1_count += sig;
    siglongjmp(sig_jump_env, 42);
}

static void on_segv_fix(int sig, siginfo_t *info, void *ucontext)
{
    (void)sig;
    (void)ucontext;
    if (info)
        last_si_addr = info->si_addr;
    mprotect(segv_fix_page, 4096, PROT_READ | PROT_WRITE);
}

static void phase0_regressions(void)
{
    /* 1. Copy-on-write fork + kernel read() into a COW page. */
    size_t cow_len = 64 * 4096;
    unsigned char *cow = malloc(cow_len);
    CHECK(cow != NULL);
    for (size_t i = 0; i < cow_len; i += 4096)
        cow[i] = (unsigned char)((i / 4096) ^ 0x5A);
    struct meminfo before, during;
    CHECK(meminfo(&before) == 0);
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    int child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(pfd[0]);
        for (size_t i = 0; i < cow_len; i += 4096)
            if (cow[i] != (unsigned char)((i / 4096) ^ 0x5A))
                _exit(2);
        /* Touch only one page in the child; the other 63 stay shared. */
        cow[4096] = 0xEE;
        struct meminfo cmi;
        if (meminfo(&cmi) < 0)
            _exit(3);
        if (write(pfd[1], &cmi.free_kb, sizeof(cmi.free_kb)) != sizeof(cmi.free_kb))
            _exit(4);
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);
    /* Read directly into a COW page to verify user_check_w breaks COW. */
    CHECK(read(pfd[0], cow + 8192, sizeof(during.free_kb)) == (int)sizeof(during.free_kb));
    close(pfd[0]);
    memcpy(&during.free_kb, cow + 8192, sizeof(during.free_kb));
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(cow[0] == 0x5A && cow[4096] == (1 ^ 0x5A) && cow[12288] == (3 ^ 0x5A));
    /* Without COW, cloning 64 pages + binary/stack would cost > 256 KiB;
     * with COW, page tables + kstack + 1 faulted page cost < 64 KiB. */
    CHECK(before.free_kb - during.free_kb < 128);
    free(cow);

    /* 2. User signal handlers, FPU preservation, sigprocmask, SA_RESTART, SIGCHLD. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sa.sa_flags = SA_RESTART;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);

    float sse_before[4] = { 10.5f, 20.5f, 30.5f, 40.5f }, sse_after[4] = {0};
    __asm__ volatile("movups %0, %%xmm7" :: "m"(sse_before) : "xmm7");
    CHECK(raise(SIGUSR1) == 0 && usr1_count == 1);
    __asm__ volatile("movups %%xmm7, %0" : "=m"(sse_after) :: "memory");
    for (int i = 0; i < 4; i++)
        CHECK(sse_after[i] == sse_before[i]);

    /* sigprocmask blocks and then delivers upon unblock. */
    sigset_t set, oldset;
    CHECK(sigemptyset(&set) == 0 && sigaddset(&set, SIGUSR1) == 0);
    CHECK(sigprocmask(SIG_BLOCK, &set, &oldset) == 0);
    CHECK(raise(SIGUSR1) == 0 && usr1_count == 1);
    CHECK(sigprocmask(SIG_SETMASK, &oldset, NULL) == 0 && usr1_count == 2);

    /* SA_SIGINFO and SA_RESETHAND. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_usr2_info;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    CHECK(sigaction(SIGUSR2, &sa, NULL) == 0);
    CHECK(raise(SIGUSR2) == 0 && usr2_count == 1 && last_si_signo == SIGUSR2);
    struct sigaction cur_sa;
    CHECK(sigaction(SIGUSR2, NULL, &cur_sa) == 0 && cur_sa.sa_handler == SIG_DFL);

    /* SA_RESTART vs EINTR on blocking pipe read, plus SIGCHLD on child exit. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_chld;
    sa.sa_flags = SA_RESTART;
    CHECK(sigaction(SIGCHLD, &sa, NULL) == 0);

    for (int restart = 0; restart < 2; restart++) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        sa.sa_flags = restart ? SA_RESTART : 0;
        CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
        CHECK(pipe(pfd) == 0);
        int c = fork();
        CHECK(c >= 0);
        if (!c) {
            close(pfd[0]);
            msleep(30);
            kill(getppid(), SIGUSR1);
            msleep(30);
            (void)write(pfd[1], "ok", 2);
            close(pfd[1]);
            _exit(0);
        }
        close(pfd[1]);
        char rbuf[4] = {0};
        int nr = (int)read(pfd[0], rbuf, 2);
        if (!restart) {
            CHECK(nr == -1 && errno == EINTR);
            CHECK(read(pfd[0], rbuf, 2) == 2 && !memcmp(rbuf, "ok", 2));
        } else {
            CHECK(nr == 2 && !memcmp(rbuf, "ok", 2));
        }
        close(pfd[0]);
        CHECK(waitpid(c, &status, 0) == c);
    }
    CHECK(chld_count >= 2);
    CHECK(signal(SIGCHLD, SIG_DFL) != SIG_ERR);

    /* 3. setjmp/longjmp and sigsetjmp/siglongjmp out of a signal handler. */
    jmp_buf jenv;
    volatile int jsteps = 0;
    int jr = setjmp(jenv);
    if (jr == 0) {
        jsteps++;
        longjmp(jenv, 0);               /* 0 must become 1 */
    }
    CHECK(jr == 1 && jsteps == 1);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1_longjmp;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    usr1_count = 0;
    if (sigsetjmp(sig_jump_env, 1) == 0) {
        raise(SIGUSR1);
        CHECK(0);
    }
    CHECK(usr1_count == SIGUSR1);
    /* Signal mask must have been restored by siglongjmp so SIGUSR1 works again. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    CHECK(raise(SIGUSR1) == 0 && usr1_count == SIGUSR1 + 1);
    CHECK(signal(SIGUSR1, SIG_DFL) != SIG_ERR);

    /* 4. mmap, munmap (splitting), MAP_SHARED across fork, file mmap, mprotect + SIGSEGV. */
    uint8_t *priv_map = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(priv_map != MAP_FAILED);
    priv_map[0] = 0x11;
    priv_map[4096] = 0x22;
    priv_map[8192] = 0x33;
    CHECK(munmap(priv_map + 4096, 4096) == 0);
    CHECK(priv_map[0] == 0x11 && priv_map[8192] == 0x33);
    CHECK(munmap(priv_map, 4096) == 0 && munmap(priv_map + 8192, 4096) == 0);

    uint32_t *sh_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(sh_map != MAP_FAILED);
    sh_map[0] = 0x12345678u;
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (sh_map[0] != 0x12345678u)
            _exit(1);
        sh_map[0] = 0xCAFEBABEu;
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(sh_map[0] == 0xCAFEBABEu);
    CHECK(munmap(sh_map, 4096) == 0);

    int mfd = open("/tmp/mmap-file", O_CREAT | O_RDWR | O_TRUNC, 0600);
    CHECK(mfd >= 0 && write(mfd, "hello mmap", 10) == 10);
    char *fmap = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, mfd, 0);
    CHECK(fmap != MAP_FAILED && !memcmp(fmap, "hello mmap", 10));
    fmap[0] = 'H';
    char fcheck[10] = {0};
    CHECK(lseek(mfd, 0, SEEK_SET) == 0 && read(mfd, fcheck, 10) == 10);
    CHECK(!memcmp(fcheck, "hello mmap", 10));
    CHECK(munmap(fmap, 4096) == 0 && close(mfd) == 0 && unlink("/tmp/mmap-file") == 0);

    segv_fix_page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(segv_fix_page != MAP_FAILED);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv_fix;
    sa.sa_flags = SA_SIGINFO;
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);
    *(volatile uint32_t *)segv_fix_page = 0xABCD1234u;
    CHECK(last_si_addr == (uint32_t)segv_fix_page);
    CHECK(*(volatile uint32_t *)segv_fix_page == 0xABCD1234u);
    CHECK(signal(SIGSEGV, SIG_DFL) != SIG_ERR);
    CHECK(munmap(segv_fix_page, 4096) == 0);

    /* 5. fcntl (F_DUPFD, FD_CLOEXEC, O_NONBLOCK), umask, utime, times, getrusage. */
    CHECK(pipe(pfd) == 0);
    int dup10 = fcntl(pfd[0], F_DUPFD, 10);
    CHECK(dup10 >= 10);
    CHECK(fcntl(dup10, F_GETFD) == 0);
    CHECK(fcntl(dup10, F_SETFD, FD_CLOEXEC) == 0 && fcntl(dup10, F_GETFD) == FD_CLOEXEC);
    CHECK(close(dup10) == 0);
    CHECK(fcntl(pfd[0], F_SETFL, O_NONBLOCK) == 0);
    char c;
    CHECK(read(pfd[0], &c, 1) == -1 && errno == EAGAIN);
    close(pfd[0]);
    close(pfd[1]);

    mode_t old_mask = umask(0027);
    int ufd = open("/tmp/umask-test", O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0666);
    CHECK(ufd >= 0 && fcntl(ufd, F_GETFD) == FD_CLOEXEC);
    struct stat ust;
    CHECK(fstat(ufd, &ust) == 0 && (ust.st_mode & 0777) == 0640);
    CHECK(close(ufd) == 0);
    struct utimbuf utb = { .actime = 1700000000, .modtime = 1700001234 };
    CHECK(utime("/tmp/umask-test", &utb) == 0);
    CHECK(stat("/tmp/umask-test", &ust) == 0 && ust.st_mtime == 1700001234u);
    CHECK(unlink("/tmp/umask-test") == 0);
    umask(old_mask);

    struct tms tbuf;
    struct rusage ru;
    struct timeval tv;
    CHECK(times(&tbuf) != (clock_t)-1);
    CHECK(getrusage(RUSAGE_SELF, &ru) == 0);
    CHECK(gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 0);
    CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0);

    /* 6. Large argv execve (> 512 args) + O_CLOEXEC closing on exec. */
    int cfd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(cfd >= 0);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        static char *big_argv[605];
        big_argv[0] = "sh";
        big_argv[1] = "-c";
        big_argv[2] = "exit 0";
        for (int i = 3; i < 603; i++)
            big_argv[i] = "arg_padding_value_for_execve_limit_test";
        big_argv[603] = NULL;
        execv("/bin/sh", big_argv);
        _exit(127);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(close(cfd) == 0);
    puts("guest: COW fork, sigaction/SSE/SA_RESTART, sigsetjmp, mmap/mprotect, fcntl/umask/utime and large execve passed");
}

/* mkiso, run on fluxOS, makes an image the kernel's ISO 9660 reader reads
 * back: Rock Ridge names (long, mixed case, two that are the same in 8.3),
 * modes, times, an empty file, a nested directory and El Torito's boot
 * catalog and information table. The image goes onto the scratch disk. */
static void put_file(const char *path, const char *data, size_t n, mode_t mode)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    CHECK(fd >= 0 && write(fd, data, n) == (ssize_t)n && close(fd) == 0);
}

static void native_mkiso(void)
{
    static char big[100000];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (char)(i * 7 + i / 300);
    CHECK(mkdir("/tmp/isotree", 0755) == 0);
    CHECK(mkdir("/tmp/isotree/Sub Directory", 0755) == 0);
    CHECK(mkdir("/tmp/isotree/Sub Directory/deeper", 0755) == 0);
    put_file("/tmp/isotree/Hello.txt", "hello, iso\n", 11, 0644);
    put_file("/tmp/isotree/a long name with spaces.tar.gz", big, sizeof(big), 0644);
    put_file("/tmp/isotree/collision-one.txt", "one", 3, 0644);
    put_file("/tmp/isotree/collision-two.txt", "two", 3, 0644);
    put_file("/tmp/isotree/run-me", "#!/bin/sh\n", 10, 0755);
    put_file("/tmp/isotree/empty", "", 0, 0644);
    put_file("/tmp/isotree/Sub Directory/deeper/Leaf.File", "leaf", 4, 0644);
    static char boot[4096];
    for (size_t i = 0; i < sizeof(boot); i++)
        boot[i] = (char)i;
    put_file("/tmp/isotree/boot.img", boot, sizeof(boot), 0644);
    struct utimbuf ut = { .actime = 1700004321, .modtime = 1700004321 };
    CHECK(utime("/tmp/isotree/Hello.txt", &ut) == 0);

    int child = fork(); CHECK(child >= 0);
    if (!child) {
        char *args[] = { "mkiso", "-o", "/tmp/test.iso", "-V", "regress", "-b", "boot.img", "/tmp/isotree", NULL };
        execv("/bin/mkiso", args); _exit(127);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    int in = open("/tmp/test.iso", O_RDONLY), out = open("/dev/hda", O_WRONLY);
    CHECK(in >= 0 && out >= 0);
    static char chunk[65536];
    int n;
    while ((n = (int)read(in, chunk, sizeof(chunk))) > 0)
        CHECK(write(out, chunk, (size_t)n) == n);
    CHECK(n == 0 && close(in) == 0 && close(out) == 0);
    unlink("/tmp/test.iso");

    CHECK(mount("/dev/hda", "/mnt/regress", "iso9660") == 0);
    struct stat st;
    char buf[64];
    int fd = open("/mnt/regress/Hello.txt", O_RDONLY); CHECK(fd >= 0);
    CHECK(read(fd, buf, sizeof(buf)) == 11 && !memcmp(buf, "hello, iso\n", 11) && close(fd) == 0);
    CHECK(stat("/mnt/regress/Hello.txt", &st) == 0 && st.st_mtime == 1700004321);
    CHECK(stat("/mnt/regress/a long name with spaces.tar.gz", &st) == 0 && st.st_size == sizeof(big));
    static char back[sizeof(big)];
    fd = open("/mnt/regress/a long name with spaces.tar.gz", O_RDONLY); CHECK(fd >= 0);
    size_t got = 0;
    while (got < sizeof(back) && (n = (int)read(fd, back + got, sizeof(back) - got)) > 0)
        got += (size_t)n;
    CHECK(got == sizeof(big) && !memcmp(back, big, sizeof(big)) && close(fd) == 0);
    fd = open("/mnt/regress/collision-one.txt", O_RDONLY); CHECK(fd >= 0);
    CHECK(read(fd, buf, sizeof(buf)) == 3 && !memcmp(buf, "one", 3) && close(fd) == 0);
    fd = open("/mnt/regress/collision-two.txt", O_RDONLY); CHECK(fd >= 0);
    CHECK(read(fd, buf, sizeof(buf)) == 3 && !memcmp(buf, "two", 3) && close(fd) == 0);
    CHECK(stat("/mnt/regress/run-me", &st) == 0 && (st.st_mode & 0777) == 0755);
    CHECK(stat("/mnt/regress/empty", &st) == 0 && st.st_size == 0 && S_ISREG(st.st_mode));
    CHECK(stat("/mnt/regress/Sub Directory/deeper", &st) == 0 && S_ISDIR(st.st_mode));
    fd = open("/mnt/regress/Sub Directory/deeper/Leaf.File", O_RDONLY); CHECK(fd >= 0);
    CHECK(read(fd, buf, sizeof(buf)) == 4 && !memcmp(buf, "leaf", 4) && close(fd) == 0);
    CHECK(stat("/mnt/regress/boot.catalog", &st) == 0 && st.st_size == 2048);
    /* the boot information table: the primary volume descriptor's block (16) */
    fd = open("/mnt/regress/boot.img", O_RDONLY); CHECK(fd >= 0);
    /* and the image's length (4096) */
    CHECK(read(fd, buf, 24) == 24 && buf[8] == 16 && buf[9] == 0 && buf[16] == 0 && buf[17] == 0x10 &&
          close(fd) == 0);
    CHECK(umount("/mnt/regress") == 0);
    puts("guest: mkiso on fluxOS, read back by the ISO 9660 driver (Rock Ridge names, modes, times, El Torito) passed");
}

static int run_tool(char *const argv[])
{
    int child = fork();
    if (!child) {
        execv(argv[0], argv);
        _exit(127);
    }
    int status;
    CHECK(child > 0 && waitpid(child, &status, 0) == child);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Symbolic links on ramfs, then an ext2 volume made by the native
 * mkfs.ext2 on the scratch disk: files through indirect blocks, symbolic
 * and hard links, owners, modes and times that persist across a remount,
 * fsck.ext2 content with it. The host's e2fsck checks the disk after. */
static void native_ext2(void)
{
    struct stat st;
    char buf[256];
    CHECK(symlink("/tmp/isotree", "/tmp/tree-link") == 0);
    CHECK(lstat("/tmp/tree-link", &st) == 0 && S_ISLNK(st.st_mode) && st.st_size == 12);
    CHECK(stat("/tmp/tree-link", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(readlink("/tmp/tree-link", buf, sizeof(buf)) == 12 && !memcmp(buf, "/tmp/isotree", 12));
    int fd = open("/tmp/tree-link/Hello.txt", O_RDONLY); CHECK(fd >= 0);
    CHECK(read(fd, buf, 11) == 11 && !memcmp(buf, "hello, iso\n", 11) && close(fd) == 0);
    CHECK(symlink("loop-b", "/tmp/loop-a") == 0 && symlink("loop-a", "/tmp/loop-b") == 0);
    CHECK(open("/tmp/loop-a", O_RDONLY) == -1 && errno == ELOOP);
    CHECK(readlink("/tmp/isotree", buf, sizeof(buf)) == -1 && errno == EINVAL);
    CHECK(link("/tmp/isotree/Hello.txt", "/tmp/hard") == -1 && errno == EPERM);
    CHECK(unlink("/tmp/tree-link") == 0 && stat("/tmp/isotree/Hello.txt", &st) == 0);

    char *mkfs[] = { "/bin/mkfs.ext2", "-q", "-L", "regress", "/dev/hda", NULL };
    CHECK(run_tool(mkfs) == 0);
    CHECK(mount("/dev/hda", "/mnt/regress", "ext2") == 0);
    struct statfs sfs;
    CHECK(statfs("/mnt/regress", &sfs) == 0 && !strcmp(sfs.f_fstype, "ext2"));
    CHECK(stat("/mnt/regress/lost+found", &st) == 0 && S_ISDIR(st.st_mode));
    static char big[3 << 20];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (char)(i * 13 + i / 4093);
    fd = open("/mnt/regress/big.bin", O_CREAT | O_WRONLY, 0644); CHECK(fd >= 0);
    CHECK(write(fd, big, sizeof(big)) == (ssize_t)sizeof(big) && close(fd) == 0);
    CHECK(mkdir("/mnt/regress/src", 0755) == 0 && mkdir("/mnt/regress/src/lib", 0755) == 0);
    put_file("/mnt/regress/src/lib/a.c", "int a;\n", 7, 0644);
    put_file("/mnt/regress/src/run.sh", "#!/bin/sh\n", 10, 0755);
    CHECK(symlink("lib/a.c", "/mnt/regress/src/alias.c") == 0);
    CHECK(link("/mnt/regress/src/lib/a.c", "/mnt/regress/src/hard.c") == 0);
    CHECK(stat("/mnt/regress/src/hard.c", &st) == 0 && st.st_nlink == 2);
    CHECK(chown("/mnt/regress/src/run.sh", 1000, 100) == 0);
    CHECK(chmod("/mnt/regress/src/lib", 0700) == 0);
    struct utimbuf ut = { .actime = 1700007777, .modtime = 1700007777 };
    CHECK(utime("/mnt/regress/src/run.sh", &ut) == 0);
    CHECK(rename("/mnt/regress/src/lib", "/mnt/regress/lib2") == 0);
    CHECK(symlink("../lib2", "/mnt/regress/src/lib") == 0);       /* the old path, through a link */
    DIR *d = opendir("/mnt/regress/src");
    CHECK(d);
    int saw_link = 0;
    struct dirent *de;
    while ((de = readdir(d)))
        if (!strcmp(de->d_name, "alias.c"))
            saw_link = de->d_type == DT_LNK;
    closedir(d);
    CHECK(saw_link);
    CHECK(umount("/mnt/regress") == 0);
    char *fsck[] = { "/bin/fsck.ext2", "/dev/hda", NULL };
    CHECK(run_tool(fsck) == 0);

    CHECK(mount("/dev/hda", "/mnt/regress", "auto") == 0);
    static char back[3 << 20];
    fd = open("/mnt/regress/big.bin", O_RDONLY); CHECK(fd >= 0);
    size_t got = 0;
    ssize_t n;
    while (got < sizeof(back) && (n = read(fd, back + got, sizeof(back) - got)) > 0)
        got += (size_t)n;
    CHECK(got == sizeof(big) && !memcmp(back, big, sizeof(big)) && close(fd) == 0);
    fd = open("/mnt/regress/src/lib/a.c", O_RDONLY); CHECK(fd >= 0);    /* through src/lib -> ../lib2 */
    CHECK(read(fd, buf, sizeof(buf)) == 7 && !memcmp(buf, "int a;\n", 7) && close(fd) == 0);
    CHECK(readlink("/mnt/regress/src/alias.c", buf, sizeof(buf)) == 7 && !memcmp(buf, "lib/a.c", 7));
    CHECK(stat("/mnt/regress/src/hard.c", &st) == 0 && st.st_nlink == 2);
    CHECK(stat("/mnt/regress/src/run.sh", &st) == 0 && st.st_uid == 1000 && st.st_gid == 100 &&
          (st.st_mode & 0777) == 0755 && st.st_mtime == 1700007777);
    CHECK(stat("/mnt/regress/lib2", &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(lstat("/mnt/regress/src/lib", &st) == 0 && S_ISLNK(st.st_mode));
    /* cp -a keeps links and hard links; ls -l shows what a link points at */
    CHECK(link("/mnt/regress/src/run.sh", "/mnt/regress/src/run2.sh") == 0);
    char *cpa[] = { "/bin/cp", "-a", "/mnt/regress/src", "/mnt/regress/copy", NULL };
    CHECK(run_tool(cpa) == 0);
    CHECK(readlink("/mnt/regress/copy/alias.c", buf, sizeof(buf)) == 7 && !memcmp(buf, "lib/a.c", 7));
    CHECK(lstat("/mnt/regress/copy/lib", &st) == 0 && S_ISLNK(st.st_mode));
    struct stat st2;
    CHECK(stat("/mnt/regress/copy/run.sh", &st) == 0 && stat("/mnt/regress/copy/run2.sh", &st2) == 0 &&
          st.st_ino == st2.st_ino && st.st_nlink == 2 && st.st_mtime == 1700007777 && (st.st_mode & 0777) == 0755);
    int out = open("/tmp/ls.out", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(out >= 0);
    int child = fork();
    if (!child) {
        dup2(out, 1);
        char *ls[] = { "/bin/ls", "-l", "/mnt/regress/copy", NULL };
        execv(ls[0], ls);
        _exit(127);
    }
    int status;
    CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(out);
    static char listing[4096];
    fd = open("/tmp/ls.out", O_RDONLY);
    n = read(fd, listing, sizeof(listing) - 1);
    CHECK(fd >= 0 && n > 0 && close(fd) == 0);
    listing[n] = 0;
    CHECK(strstr(listing, "alias.c -> lib/a.c") && strstr(listing, "lrwxrwxrwx"));
    CHECK(unlink("/mnt/regress/src/hard.c") == 0);
    CHECK(stat("/mnt/regress/lib2/a.c", &st) == 0 && st.st_nlink == 1);
    CHECK(umount("/mnt/regress") == 0);
    puts("guest: ext2 by the native mkfs.ext2: files, symbolic and hard links, owners, modes, times, cp -a, ls -l and fsck.ext2 passed");
}

/* One second of a sine wave on a /dev/dsp of its own, then SNDCTL_DSP_SYNC. */
static void play_tone(int hz)
{
    int fd = open("/dev/dsp", O_WRONLY);
    CHECK(fd >= 0);
    int rate = 48000, channels = 1, fmt = AFMT_S16_LE;
    CHECK(ioctl(fd, SNDCTL_DSP_SPEED, &rate) == 0 && rate == 48000);
    CHECK(ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) == 0 && channels == 1);
    CHECK(ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) == 0 && fmt == AFMT_S16_LE);
    struct audio_buf_info bi;
    CHECK(ioctl(fd, SNDCTL_DSP_GETOSPACE, &bi) == 0 && bi.bytes > 0);
    static int16_t chunk[4800];         /* 100 ms */
    for (int part = 0; part < 10; part++) {
        for (int i = 0; i < 4800; i++)
            chunk[i] = (int16_t)(8000 * sin(2 * M_PI * hz * (part * 4800 + i) / 48000.0));
        CHECK(write(fd, chunk, sizeof(chunk)) == (int)sizeof(chunk));
    }
    int delay = -1;
    CHECK(ioctl(fd, SNDCTL_DSP_GETODELAY, &delay) == 0 && delay > 0);
    CHECK(ioctl(fd, SNDCTL_DSP_SYNC) == 0);
    CHECK(ioctl(fd, SNDCTL_DSP_GETODELAY, &delay) == 0 && delay == 0);
    CHECK(close(fd) == 0);
}

/* Two programs play at once: the software mixer gives each its own stream
 * (before it, the second got EBUSY). With --sound, QEMU records the card's
 * output and the harness checks that both tones sound in it at the same
 * time, not one after the other. */
static void sound_mixing(void)
{
    struct stat st;
    if (stat("/dev/dsp", &st) != 0) {
        puts("guest: no sound card; the mixer was not tried");
        return;
    }
    int both[2];
    CHECK(pipe(both) == 0);
    int child = fork();
    CHECK(child >= 0);
    if (!child) {
        char go;
        close(both[1]);
        if (read(both[0], &go, 1) != 1)
            _exit(1);
        play_tone(660);
        _exit(0);
    }
    close(both[0]);
    int held = open("/dev/dsp", O_WRONLY);             /* open while the other one starts */
    CHECK(held >= 0);
    CHECK(write(both[1], "g", 1) == 1);
    play_tone(440);
    CHECK(close(held) == 0 && close(both[1]) == 0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("guest: two programs played on /dev/dsp at once (440 Hz and 660 Hz) passed");
}

/* A buffer unmapped by another thread while read() waits to fill it: the
 * copy out fails with EFAULT, where the kernel used to fault on it and panic. */
static int unmapped_pipe[2];
static void *read_into(void *buf)
{
    ssize_t r = read(unmapped_pipe[0], buf, 64);
    return (void *)(intptr_t)(r < 0 ? -errno : r);
}

/* Two threads ending at once by the raw call (libc's count would make the
 * second exit()): the second must end the process, closing its descriptors. */
static volatile int exit_ready, exit_go;
static void raw_thread_exit(void)
{
    __asm__ volatile("int $0x80" : : "a"(SYS_thread_exit), "b"(0), "c"(0) : "memory");
    for (;;)
        ;
}
static void *exit_together(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&exit_ready, 1, __ATOMIC_SEQ_CST);
    while (!exit_go)
        ;
    raw_thread_exit();
    return NULL;
}

static void user_memory_races(void)
{
    char *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(buf != MAP_FAILED);
    CHECK(pipe(unmapped_pipe) == 0);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, read_into, buf) == 0);
    CHECK(msleep(50) == 0);             /* it waits in read() by now */
    CHECK(munmap(buf, 4096) == 0);
    CHECK(write(unmapped_pipe[1], "data", 4) == 4);
    void *res;
    CHECK(pthread_join(t, &res) == 0 && (intptr_t)res == -EFAULT);
    CHECK(close(unmapped_pipe[0]) == 0 && close(unmapped_pipe[1]) == 0);

    for (int round = 0; round < 20; round++) {
        int held[2];
        CHECK(pipe(held) == 0);
        int child = fork();
        CHECK(child >= 0);
        if (!child) {
            close(held[0]);
            pthread_t other;
            if (pthread_create(&other, NULL, exit_together, NULL) != 0)
                _exit(1);
            while (__atomic_load_n(&exit_ready, __ATOMIC_SEQ_CST) < 1)
                ;
            exit_go = 1;
            raw_thread_exit();
        }
        close(held[1]);
        struct pollfd pfd = { .fd = held[0], .events = POLLIN };
        char c;
        CHECK(poll(&pfd, 1, 5000) == 1 && read(held[0], &c, 1) == 0);  /* the child's end closed */
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(close(held[0]) == 0);
    }
    puts("guest: read into memory unmapped meanwhile, and two threads exiting at once passed");
}

int main(void)
{
    sound_mixing();
    scheduler_stress();
    phase0_regressions();
    shared_offset_stress();
    user_memory_races();
    int fd = open("/tmp/size-test", O_CREAT | O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, "safe", 4) == 4);
    CHECK(ftruncate(fd, -1) == -1 && errno == EINVAL);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    char buf[16] = {0};
    CHECK(read(fd, buf, 4) == 4 && !memcmp(buf, "safe", 4));
    CHECK(close(fd) == 0);
    int pipes[2]; CHECK(pipe(pipes) == 0);
    CHECK(read(pipes[0], buf, 0) == 0);
    close(pipes[0]); close(pipes[1]);
    puts("guest: negative truncate and zero-byte pipe read passed");

    int child = fork(); CHECK(child >= 0);
    if (!child) {
        char *args[] = { "mkfs.fat", "-F", "32", "/dev/hda", NULL };
        execv("/bin/mkfs.fat", args); _exit(1);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(mkdir("/mnt/regress", 0755) == 0);
    CHECK(mount("/dev/hda", "/mnt/regress", "fat32") == 0);
    cached_files();
    fd = open("/mnt/regress/OLD.TXT", O_CREAT | O_RDWR, 0600); CHECK(fd >= 0);
    CHECK(write(fd, "old data", 8) == 8);
    int alias = dup(fd); CHECK(alias >= 0);
    CHECK(unlink("/mnt/regress/OLD.TXT") == 0);
    CHECK(umount("/mnt/regress") == -1 && errno == EBUSY);
    int fresh = open("/mnt/regress/NEW.TXT", O_CREAT | O_RDWR, 0600); CHECK(fresh >= 0);
    CHECK(write(fresh, "new data", 8) == 8);
    CHECK(close(fresh) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, buf, 8) == 8 && !memcmp(buf, "old data", 8));
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && write(fd, "changed!", 8) == 8);
    CHECK(ftruncate(fd, 3) == 0);
    CHECK(close(fd) == 0);
    CHECK(umount("/mnt/regress") == -1 && errno == EBUSY);
    CHECK(close(alias) == 0);
    int src = open("/mnt/regress/SOURCE.TXT", O_CREAT | O_RDWR, 0600); CHECK(src >= 0);
    CHECK(write(src, "source", 6) == 6);
    int dst = open("/mnt/regress/TARGET.TXT", O_CREAT | O_RDWR, 0600); CHECK(dst >= 0);
    CHECK(write(dst, "target", 6) == 6);
    CHECK(rename("/mnt/regress/SOURCE.TXT", "/mnt/regress/TARGET.TXT") == 0);
    CHECK(lseek(dst, 0, SEEK_SET) == 0);
    CHECK(read(dst, buf, 6) == 6 && !memcmp(buf, "target", 6));
    CHECK(ftruncate(src, 3) == 0);
    CHECK(close(src) == 0 && close(dst) == 0);
    struct utimbuf fat_ut = { .actime = 1700001234, .modtime = 1700001234 };
    CHECK(utime("/mnt/regress/NEW.TXT", &fat_ut) == 0);
    CHECK(umount("/mnt/regress") == 0);
    CHECK(mount("/dev/hda", "/mnt/regress", "fat32") == 0);
    struct stat fat_st;
    CHECK(stat("/mnt/regress/NEW.TXT", &fat_st) == 0 && fat_st.st_mtime == 1700001234u);
    fresh = open("/mnt/regress/NEW.TXT", O_RDONLY); CHECK(fresh >= 0);
    CHECK(read(fresh, buf, 8) == 8 && !memcmp(buf, "new data", 8));
    CHECK(close(fresh) == 0);
    CHECK(open("/mnt/regress/SOURCE.TXT", O_RDONLY) == -1 && errno == ENOENT);
    dst = open("/mnt/regress/TARGET.TXT", O_RDONLY); CHECK(dst >= 0);
    CHECK(read(dst, buf, sizeof(buf)) == 3 && !memcmp(buf, "sou", 3));
    CHECK(close(dst) == 0 && umount("/mnt/regress") == 0);
    puts("guest: unlink/rename open-file lifetime, truncate and FAT remount persistence passed");
    native_mkiso();
    native_ext2();
    puts("KERNEL REGRESSIONS PASSED");
    stop();
}
