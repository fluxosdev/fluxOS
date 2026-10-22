/* tail - output the last lines of files */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void tail(FILE *f, int lines)
{
    char **ring = calloc((size_t)lines + 1, sizeof(char *));
    int count = 0;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        free(ring[count % (lines + 1)]);
        ring[count % (lines + 1)] = strdup(line);
        count++;
    }
    int start = count > lines ? count - lines : 0;
    for (int k = start; k < count; k++)
        fputs(ring[k % (lines + 1)], stdout);
    for (int k = 0; k <= lines; k++)
        free(ring[k]);
    free(ring);
    free(line);
}

int main(int argc, char **argv)
{
    int lines = 10, i = 1, status = 0;
    if (i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) {
        lines = atoi(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
        lines = atoi(argv[i] + 1);
        i++;
    }
    if (lines < 0)
        lines = 0;
    if (i >= argc) {
        tail(stdin, lines);
        return 0;
    }
    int many = argc - i > 1;
    for (int k = i; k < argc; k++) {
        FILE *f = fopen(argv[k], "r");
        if (!f) {
            fprintf(stderr, "tail: cannot open '%s': %s\n", argv[k], strerror(errno));
            status = 1;
            continue;
        }
        if (many)
            printf("%s==> %s <==\n", k > i ? "\n" : "", argv[k]);
        tail(f, lines);
        fclose(f);
    }
    return status;
}
