#include "kernel-test.h"
#include "../kernel/file.c"
struct proc *current;
static int sleeps;
void sleep_on(void *p) { (void)p; sleeps++; }
int signal_pending(struct proc *p) { (void)p; return sleeps != 0; }
void wakeup(void *p) { (void)p; }
int main(void)
{
    struct pipe p = { .writers = 1, .readers = 1 };
    CHECK(pipe_read(&p, NULL, 0) == 0 && sleeps == 0);
    p.buf[0] = 'a'; p.buf[1] = 'b'; p.wpos = 2;
    CHECK(pipe_read(&p, NULL, 0) == 0 && p.rpos == 0);
    char out[4] = {0};
    CHECK(pipe_read(&p, (uint8_t *)out, sizeof(out)) == 2 && !strcmp(out, "ab"));
    p.writers = 0;
    CHECK(pipe_read(&p, (uint8_t *)out, sizeof(out)) == 0);
    CHECK(sleeps == 0);
    puts("kernel pipes: zero-length reads, buffered reads and EOF passed");
}
