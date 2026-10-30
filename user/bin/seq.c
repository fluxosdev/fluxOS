/* seq - print a sequence of numbers */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    long first = 1, step = 1, last;
    if (argc == 2) {
        last = atol(argv[1]);
    } else if (argc == 3) {
        first = atol(argv[1]);
        last = atol(argv[2]);
    } else if (argc == 4) {
        first = atol(argv[1]);
        step = atol(argv[2]);
        last = atol(argv[3]);
    } else {
        fprintf(stderr, "usage: seq [first [step]] last\n");
        return 1;
    }
    if (step == 0)
        return 1;
    for (long v = first; step > 0 ? v <= last : v >= last; v += step)
        printf("%ld\n", v);
    return 0;
}
