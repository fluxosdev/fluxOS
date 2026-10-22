/* uniq - report or omit repeated adjacent lines */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int count = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-c")) {
        count = 1;
        i++;
    }
    FILE *f = i < argc ? fopen(argv[i], "r") : stdin;
    if (!f) {
        perror("uniq");
        return 1;
    }
    char *line = NULL, *prev = NULL;
    size_t cap = 0;
    int n = 0;
    while (getline(&line, &cap, f) > 0) {
        if (prev && !strcmp(prev, line)) {
            n++;
            continue;
        }
        if (prev) {
            if (count)
                printf("%7d ", n);
            fputs(prev, stdout);
            free(prev);
        }
        prev = strdup(line);
        n = 1;
    }
    if (prev) {
        if (count)
            printf("%7d ", n);
        fputs(prev, stdout);
    }
    return 0;
}
