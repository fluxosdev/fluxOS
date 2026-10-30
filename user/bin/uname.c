/* uname - print system information */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int main(int argc, char **argv)
{
    struct utsname u;
    if (uname(&u) < 0) {
        perror("uname");
        return 1;
    }
    int s = 0, n = 0, r = 0, v = 0, m = 0;
    if (argc < 2)
        s = 1;
    for (int i = 1; i < argc; i++) {
        for (const char *f = argv[i] + (argv[i][0] == '-'); *f; f++) {
            switch (*f) {
            case 'a': s = n = r = v = m = 1; break;
            case 's': s = 1; break;
            case 'n': n = 1; break;
            case 'r': r = 1; break;
            case 'v': v = 1; break;
            case 'm': case 'p': m = 1; break;
            }
        }
    }
    const char *sep = "";
    if (s) { printf("%s%s", sep, u.sysname); sep = " "; }
    if (n) { printf("%s%s", sep, u.nodename); sep = " "; }
    if (r) { printf("%s%s", sep, u.release); sep = " "; }
    if (v) { printf("%s%s", sep, u.version); sep = " "; }
    if (m) { printf("%s%s", sep, u.machine); }
    putchar('\n');
    return 0;
}
