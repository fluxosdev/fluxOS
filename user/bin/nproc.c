/* nproc - print the number of processors */
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    printf("%ld\n", n > 0 ? n : 1);
    return 0;
}
