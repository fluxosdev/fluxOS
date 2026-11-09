/* What the native GCC compiles under QEMU (tests/gcc-guest.c): a program with a
 * little of everything the toolchain must get right. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct point {
    double x, y;
};

static int cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

__attribute__((constructor)) static void hello_ctor(void)
{
    printf("constructor ran\n");
}

int main(int argc, char **argv)
{
    const char *words[] = { "pear", "apple", "fig", "kiwi" };
    qsort(words, 4, sizeof(words[0]), cmp);
    struct point p = { 3, 4 };
    long long big = 1234567890123LL;
    printf("hello from fluxOS gcc: %s %s %s %s %.1f %lld %d\n", words[0], words[1], words[2], words[3],
           hypot(p.x, p.y), big * 7, argc);
    return argc > 1 ? atoi(argv[1]) : 0;
}
