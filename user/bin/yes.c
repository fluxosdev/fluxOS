/* yes - output a string repeatedly until killed */
#include <stdio.h>

int main(int argc, char **argv)
{
    for (;;) {
        if (argc < 2) {
            if (puts("y") < 0)
                return 1;
            continue;
        }
        for (int i = 1; i < argc; i++)
            printf("%s%c", argv[i], i == argc - 1 ? '\n' : ' ');
        if (ferror(stdout))
            return 1;
    }
}
