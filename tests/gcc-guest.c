/*
 * The native GCC on the real kernel: runs as init in the disposable QEMU
 * environment (make test-gcc-qemu). Compiles tests/gcc-hello.c with
 * /usr/bin/gcc, runs the result, checks what it printed, and compares the
 * binary with the one the cross compiler made on the host
 * (/tests/hello.cross), stripped on both sides (the symbol table records
 * the source's name and the two were built from different paths). Then the
 * same for C++ (tests/gcc-hello.cc with /usr/bin/g++ and libstdc++:
 * iostream, containers, exceptions, static objects), and prints GCC
 * REGRESSIONS PASSED.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void stop(void)
{
    fflush(stdout);
    reboot(REBOOT_POWEROFF);
    for (;;)
        msleep(1000);
}

#define CHECK(x) do { if (!(x)) { \
    printf("GCC REGRESSION FAILED line %d: %s (errno %d)\n", __LINE__, #x, errno); \
    stop(); \
} } while (0)

/* Run a command (through /bin/sh -c), capturing its output; returns its exit status. */
static int run(const char *cmd, char *out, size_t size)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    size_t n = 0;
    int r;
    while (n + 1 < size && (r = (int)read(fds[0], out + n, size - n - 1)) > 0)
        n += (size_t)r;
    out[n] = 0;
    close(fds[0]);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int same_file(const char *a, const char *b)
{
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    CHECK(fa >= 0 && fb >= 0);
    static char ba[65536], bb[65536];
    int same = 1;
    for (;;) {
        ssize_t ra = read(fa, ba, sizeof(ba)), rb = (int)read(fb, bb, sizeof(bb));
        if (ra != rb || (ra > 0 && memcmp(ba, bb, (size_t)ra))) {
            same = 0;
            break;
        }
        if (ra <= 0)
            break;
    }
    close(fa);
    close(fb);
    return same;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

int main(void)
{
    char out[4096];
    setenv("PATH", "/bin:/usr/bin", 1);
    setenv("TMPDIR", "/tmp", 1);
    mkdir("/tmp", 0777);
    printf("guest: %d CPUs online\n", (int)sysconf(_SC_NPROCESSORS_ONLN));

    int st = run("gcc --version", out, sizeof(out));
    printf("%s", out);
    CHECK(st == 0 && strstr(out, "13.2.0"));

    unsigned int t0 = uptime_ms();
    st = run("gcc -O2 -o /tmp/hello /tests/hello.c 2>&1", out, sizeof(out));
    printf("gcc -O2: %u ms\n%s", uptime_ms() - t0, out);
    CHECK(st == 0);

    st = run("/tmp/hello 3", out, sizeof(out));
    printf("%s", out);
    CHECK(st == 3);
    CHECK(strstr(out, "constructor ran\n"));
    CHECK(strstr(out, "hello from fluxOS gcc: apple fig kiwi pear 5.0 8641975230861 2\n"));

    /* the same program, built the same way by the cross compiler on the host: the
     * stripped programs must agree byte for byte (stage 2's test in miniature) */
    st = run("strip -o /tmp/hello.stripped /tmp/hello && strip -o /tmp/cross.stripped /tests/hello.cross", out, sizeof(out));
    printf("%s", out);
    CHECK(st == 0);
    if (same_file("/tmp/hello.stripped", "/tmp/cross.stripped")) {
        printf("the native and cross builds of hello.c are identical%s\n",
               same_file("/tmp/hello", "/tests/hello.cross") ? " (symbol tables too)" : " once stripped");
    } else {
        run("size /tmp/hello.stripped /tmp/cross.stripped; cmp /tmp/hello.stripped /tmp/cross.stripped; "
            "cmp -l /tmp/hello.stripped /tmp/cross.stripped | wc -l", out, sizeof(out));
        printf("the native and cross builds differ:\n%s", out);
        CHECK(0);
    }

    /* C++: the runtime library, exceptions and static objects */
    t0 = uptime_ms();
    st = run("g++ -O2 -o /tmp/hellocc /tests/hello.cc 2>&1", out, sizeof(out));
    printf("g++ -O2: %u ms\n%s", uptime_ms() - t0, out);
    CHECK(st == 0);
    st = run("/tmp/hellocc", out, sizeof(out));
    printf("%s", out);
    CHECK(st == 0);
    CHECK(!strcmp(out, "static object constructed\n"
                       "hello from fluxOS g++: apple fig kiwi pear | kiwi=4 | area=21 | caught: division by zero\n"
                       "static object destroyed\n"));
    st = run("strip -o /tmp/hellocc.stripped /tmp/hellocc && strip -o /tmp/crosscc.stripped /tests/hellocc.cross", out, sizeof(out));
    CHECK(st == 0);
    if (same_file("/tmp/hellocc.stripped", "/tmp/crosscc.stripped"))
        printf("the native and cross builds of hello.cc are identical once stripped\n");
    else {
        run("size /tmp/hellocc.stripped /tmp/crosscc.stripped; cmp -l /tmp/hellocc.stripped /tmp/crosscc.stripped | wc -l",
            out, sizeof(out));
        printf("the native and cross builds of hello.cc differ:\n%s", out);
        CHECK(0);
    }

    /* a compile error is reported as one */
    st = run("echo 'int main(void) { return x; }' > /tmp/bad.c; gcc -c /tmp/bad.c -o /tmp/bad.o 2>&1", out, sizeof(out));
    CHECK(st != 0 && strstr(out, "error:"));

    /* the preprocessor, assembler and linker steps on their own */
    st = run("gcc -E /tests/hello.c | grep -c main", out, sizeof(out));
    CHECK(st == 0 && atoi(out) >= 1);
    st = run("gcc -S -O1 -o /tmp/hello.s /tests/hello.c && as -o /tmp/hello.o /tmp/hello.s && gcc -o /tmp/hello2 /tmp/hello.o && /tmp/hello2", out, sizeof(out));
    CHECK(st == 0 && strstr(out, "hello from fluxOS gcc"));

    /* The other two kinds of code the system is built of. -m32 makes a
     * program for the 386, which the kernel runs beside the 64-bit ones
     * (the Windows program loader is one); this one makes its system calls
     * itself, as the libraries on the system image are the 64-bit ones. */
    write_file("/tmp/bits.c",
               "static const char msg[] = \"made by gcc -m32\\n\";\n"
               "void _start(void)\n"
               "{\n"
               "    int r;\n"
               "    __asm__ volatile(\"int $0x80\" : \"=a\"(r) : \"a\"(4), \"b\"(1), \"c\"(msg), \"d\"(sizeof(msg) - 1) : \"memory\");\n"
               "    __asm__ volatile(\"int $0x80\" : \"=a\"(r) : \"a\"(1), \"b\"(sizeof(void *) * 8) : \"memory\");\n"
               "    for (;;) { }\n"
               "}\n");
    st = run("gcc -m32 -O2 -nostdlib -o /tmp/bits32 /tmp/bits.c 2>&1 && /tmp/bits32", out, sizeof(out));
    printf("%s", out);
    CHECK(st == 32 && !strcmp(out, "made by gcc -m32\n"));
    st = run("readelf -h /tmp/bits32 | grep -c 'ELF32'", out, sizeof(out));
    CHECK(st == 0 && atoi(out) == 1);
    /* And the kernel's: 64-bit code for the top 2 GiB of the address space,
     * where an address is a sign-extended 32-bit value. */
    st = run("gcc -m64 -mcmodel=kernel -mno-red-zone -ffreestanding -O2 -c -o /tmp/bitsk.o /tmp/bits.c 2>&1 && "
             "readelf -r /tmp/bitsk.o | grep -c R_X86_64_32S", out, sizeof(out));
    printf("kernel code model: %s", out);
    CHECK(st == 0 && atoi(out) >= 1);

    /* make driving it, with a recipe through /bin/sh */
    st = run("printf 'all: /tmp/hello3\\n/tmp/hello3: /tests/hello.c\\n\\tgcc -O2 -o $@ $<\\n\\ttest -x $@ && echo built\\n' > /tmp/Makefile && make -C /tmp -f /tmp/Makefile", out, sizeof(out));
    printf("%s", out);
    CHECK(st == 0 && strstr(out, "built"));

    printf("GCC REGRESSIONS PASSED\n");
    stop();
    return 0;
}
