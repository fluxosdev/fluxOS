/* sleep - delay for a specified time (fractions such as 0.5 are allowed) */
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        return 1;
    }
    char *end;
    unsigned ms = (unsigned)strtoul(argv[1], &end, 10) * 1000;
    if (*end == '.') {
        unsigned scale = 100;
        for (end++; *end >= '0' && *end <= '9' && scale; end++, scale /= 10)
            ms += (unsigned)(*end - '0') * scale;
    }
    if (*end == 'm') ms *= 60;
    else if (*end == 'h') ms *= 3600;
    return msleep(ms) < 0 ? 1 : 0;
}
