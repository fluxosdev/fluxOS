/* Runs as init only in the disposable QEMU regression environment. */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <flux/syscall.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
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

int main(void)
{
    scheduler_stress();
    phase0_regressions();
    shared_offset_stress();
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
    puts("KERNEL REGRESSIONS PASSED");
    stop();
}
