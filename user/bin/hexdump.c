/* hexdump - canonical hex+ASCII display (like hexdump -C) */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-C"))
        i++;
    int fd = i < argc ? open(argv[i], O_RDONLY) : 0;
    if (fd < 0) {
        fprintf(stderr, "hexdump: %s: %s\n", argv[i], strerror(errno));
        return 1;
    }
    unsigned char row[16];
    unsigned int off = 0;
    int n = 0, r;
    unsigned char c;
    for (;;) {
        r = read(fd, &c, 1);
        if (r == 1)
            row[n++] = c;
        if (n == 16 || (r != 1 && n > 0)) {
            printf("%08x  ", off);
            for (int k = 0; k < 16; k++) {
                if (k < n)
                    printf("%02x ", row[k]);
                else
                    printf("   ");
                if (k == 7)
                    putchar(' ');
            }
            printf(" |");
            for (int k = 0; k < n; k++)
                putchar(row[k] >= 32 && row[k] < 127 ? row[k] : '.');
            printf("|\n");
            off += (unsigned)n;
            n = 0;
        }
        if (r != 1)
            break;
    }
    if (fd)
        close(fd);
    printf("%08x\n", off);
    return 0;
}
