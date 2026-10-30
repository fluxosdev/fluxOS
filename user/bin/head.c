/* head - output the first lines of files */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bytes_mode;

static void head(FILE *f, int count)
{
    int c, n = 0;
    while (n < count && (c = fgetc(f)) != EOF) {
        putchar(c);
        if (bytes_mode || c == '\n')
            n++;
    }
}

int main(int argc, char **argv)
{
    int lines = 10, i = 1, status = 0;
    if (i < argc && (!strcmp(argv[i], "-n") || !strcmp(argv[i], "-c")) && i + 1 < argc) {
        bytes_mode = argv[i][1] == 'c';
        lines = atoi(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
        lines = atoi(argv[i] + 1);
        i++;
    }
    if (i >= argc) {
        head(stdin, lines);
        return 0;
    }
    int many = argc - i > 1;
    for (int k = i; k < argc; k++) {
        FILE *f = fopen(argv[k], "r");
        if (!f) {
            fprintf(stderr, "head: cannot open '%s': %s\n", argv[k], strerror(errno));
            status = 1;
            continue;
        }
        if (many)
            printf("%s==> %s <==\n", k > i ? "\n" : "", argv[k]);
        head(f, lines);
        fclose(f);
    }
    return status;
}
