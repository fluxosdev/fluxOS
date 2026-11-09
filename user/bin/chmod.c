/* chmod - change file mode bits (octal, or symbolic like u+x,go-w) */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Apply a symbolic mode such as "a+x" or "u=rw,go=r" to `mode`. */
static int apply_symbolic(const char *spec, unsigned *mode)
{
    while (*spec) {
        unsigned who = 0;
        for (; strchr("ugoa", *spec) && *spec; spec++) {
            if (*spec == 'u') who |= 04700;
            else if (*spec == 'g') who |= 02070;
            else if (*spec == 'o') who |= 01007;
            else who |= 07777;
        }
        if (!who)
            who = 07777;
        char op = *spec++;
        if (op != '+' && op != '-' && op != '=')
            return -1;
        unsigned bits = 0;
        for (; *spec && *spec != ','; spec++) {
            if (*spec == 'r') bits |= 0444;
            else if (*spec == 'w') bits |= 0222;
            else if (*spec == 'x') bits |= 0111;
            else if (*spec == 's') bits |= 06000;
            else if (*spec == 't') bits |= 01000;
            else return -1;
        }
        bits &= who;
        if (op == '+') *mode |= bits;
        else if (op == '-') *mode &= ~bits;
        else *mode = (*mode & ~who) | bits;
        if (*spec == ',')
            spec++;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: chmod mode file...\n");
        return 1;
    }
    const char *spec = argv[1];
    char *end;
    unsigned octal = (unsigned)strtoul(spec, &end, 8);
    int is_octal = *spec && !*end;
    int status = 0;

    for (int i = 2; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "chmod: cannot access '%s': %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        unsigned mode = st.st_mode & 07777;
        if (is_octal) {
            mode = octal;
        } else if (apply_symbolic(spec, &mode) < 0) {
            fprintf(stderr, "chmod: invalid mode: '%s'\n", spec);
            return 1;
        }
        if (chmod(argv[i], mode) < 0) {
            fprintf(stderr, "chmod: changing permissions of '%s': %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
