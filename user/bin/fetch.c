/* fetch - show system information next to the fluxOS logo */
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

static void cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static void cpu_name(char *out, size_t size)
{
    unsigned a, b, c, d;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        unsigned regs[12];
        for (unsigned i = 0; i < 3; i++)
            cpuid(0x80000002 + i, &regs[i * 4], &regs[i * 4 + 1], &regs[i * 4 + 2], &regs[i * 4 + 3]);
        char brand[49];
        memcpy(brand, regs, 48);
        brand[48] = 0;
        char *p = brand;
        while (*p == ' ')
            p++;
        strlcpy(out, p, size);
        return;
    }
    /* Older processors (a Pentium III among them) have no brand string. */
    unsigned vendor[4] = { 0 };
    cpuid(0, &a, &vendor[0], &vendor[2], &vendor[1]);
    char v[13];
    memcpy(v, vendor, 12);
    v[12] = 0;
    cpuid(1, &a, &b, &c, &d);
    unsigned family = (a >> 8) & 15, model = (a >> 4) & 15;
    if (!strcmp(v, "GenuineIntel") && family == 6 && model >= 7 && model <= 11)
        strlcpy(out, "Intel Pentium III", size);
    else
        snprintf(out, size, "%s family %u model %u", v, family, model);
}

int main(void)
{
    static const char *logo[] = {
        "\033[1;36m  __ _             \033[0m",
        "\033[1;36m / _| |_   ___  __ \033[0m",
        "\033[1;36m| |_| | | | \\ \\/ / \033[0m",
        "\033[1;36m|  _| | |_| |>  <  \033[0m",
        "\033[1;36m|_| |_|\\__,_/_/\\_\\ \033[0m",
        "                   ",
        "\033[1;34m   ~~ fluxOS ~~    \033[0m",
        "                   ",
        "                   ",
    };
    struct utsname u;
    struct meminfo mi;
    uname(&u);
    meminfo(&mi);

    char lines[9][96];
    const char *user = getenv("USER") ? getenv("USER") : "flux";
    snprintf(lines[0], sizeof(lines[0]), "\033[1;36m%s\033[0m@\033[1;36m%s\033[0m", user, u.nodename);
    snprintf(lines[1], sizeof(lines[1]), "-------------");
    snprintf(lines[2], sizeof(lines[2]), "\033[1;34mOS\033[0m: %s %s %s", u.sysname, u.release, u.machine);
    snprintf(lines[3], sizeof(lines[3]), "\033[1;34mKernel\033[0m: %s", u.version);
    unsigned up = uptime_ms() / 1000;
    snprintf(lines[4], sizeof(lines[4]), "\033[1;34mUptime\033[0m: %u min %u sec", up / 60, up % 60);
    snprintf(lines[5], sizeof(lines[5]), "\033[1;34mShell\033[0m: %s", getenv("SHELL") ? getenv("SHELL") : "/bin/fsh");
    char cpu[64];
    cpu_name(cpu, sizeof(cpu));
    if (mi.ncpu > 1)
        snprintf(lines[6], sizeof(lines[6]), "\033[1;34mCPU\033[0m: %.40s (x%u)", cpu, mi.ncpu);
    else
        snprintf(lines[6], sizeof(lines[6]), "\033[1;34mCPU\033[0m: %.50s", cpu);
    snprintf(lines[7], sizeof(lines[7]), "\033[1;34mMemory\033[0m: %u MiB / %u MiB",
             (mi.total_kb - mi.free_kb) / 1024, mi.total_kb / 1024);
    lines[8][0] = 0;
    for (int c = 0; c < 8; c++)
        snprintf(lines[8] + strlen(lines[8]), sizeof(lines[8]) - strlen(lines[8]), "\033[4%dm   ", c);
    strlcat(lines[8], "\033[0m", sizeof(lines[8]));

    putchar('\n');
    for (int i = 0; i < 9; i++)
        printf("  %s  %s\n", logo[i], lines[i]);
    putchar('\n');
    return 0;
}
