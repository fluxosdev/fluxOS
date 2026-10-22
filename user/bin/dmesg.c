/* dmesg - print the kernel log */
#include <flux.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    char *buf = malloc(16384);
    if (!buf)
        return 1;
    int n = dmesg(buf, 16384);
    if (n > 0)
        write(1, buf, (size_t)n);
    return 0;
}
