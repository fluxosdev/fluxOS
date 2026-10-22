/* free - display memory usage */
#include <flux.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct meminfo mi;
    if (meminfo(&mi) < 0) {
        perror("free");
        return 1;
    }
    int mb = argc > 1 && !strcmp(argv[1], "-m");
    unsigned div = mb ? 1024 : 1;
    printf("%-12s %10s %10s %10s\n", "", "total", "used", "free");
    printf("%-12s %10u %10u %10u\n", "Mem:", mi.total_kb / div,
           (mi.total_kb - mi.free_kb) / div, mi.free_kb / div);
    printf("%-12s %10u %10u %10u\n", "Kernel heap:", mi.heap_total_kb / div,
           mi.heap_used_kb / div, (mi.heap_total_kb - mi.heap_used_kb) / div);
    printf("%-12s %10u\n", "Kernel:", mi.kernel_kb / div);
    return 0;
}
