#include "kernel-test.h"
#include "../kernel/fs/file.c"
struct proc *current;
static int sleeps;
void sleep_on(void *p) { (void)p; sleeps++; }
int signal_pending(struct proc *p) { (void)p; return sleeps != 0; }
void wakeup(void *p) { (void)p; }
void proc_signal(struct proc *p, int sig) { (void)p; (void)sig; }
int sock_poll(struct socket *s) { (void)s; return 0; }
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

    /* A write of at most PIPE_ATOMIC bytes waits for room for all of it: never split. */
    static struct pipe q = { .writers = 1, .readers = 1 };
    static uint8_t data[8192];
    q.wpos = PIPE_SIZE - 10;
    CHECK(pipe_write_flags(&q, data, 100, 1) == -EAGAIN && q.wpos == PIPE_SIZE - 10);
    CHECK(pipe_write(&q, data, 100) == -EINTR && q.wpos == PIPE_SIZE - 10 && sleeps == 1);
    struct file w = { .type = FT_PIPE, .flags = O_WRONLY, .pipe = &q };
    CHECK(file_poll(&w, POLLOUT) == 0);         /* (POLLOUT: an atomic write fits) */
    q.rpos = PIPE_ATOMIC - 10;
    CHECK(file_poll(&w, POLLOUT) == POLLOUT);
    CHECK(pipe_write_flags(&q, data, PIPE_ATOMIC, 1) == PIPE_ATOMIC && q.wpos - q.rpos == PIPE_SIZE);
    q.rpos += 10;
    CHECK(pipe_write_flags(&q, data, sizeof(data), 1) == 10);    /* a larger one may be */

    /* MSG_PEEK on AF_UNIX: the same bytes twice, then the read takes them. */
    static struct pipe rx = { .writers = 1, .readers = 1 };
    struct unix_sock us = { .rx = &rx };
    struct file uf = { .type = FT_UNIX, .flags = O_RDWR, .usock = &us };
    memcpy(rx.buf, "peekaboo", 8);
    rx.wpos = 8;
    char got[16] = {0};
    CHECK(file_peek(&uf, got, 4, 1) == 4 && !memcmp(got, "peek", 4) && rx.rpos == 0);
    CHECK(file_peek(&uf, got, sizeof(got), 1) == 8 && !memcmp(got, "peekaboo", 8) && rx.rpos == 0);
    CHECK(pipe_read_flags(&rx, (uint8_t *)got, 4, 1) == 4 && !memcmp(got, "peek", 4) && rx.rpos == 4);
    CHECK(file_peek(&uf, got, sizeof(got), 1) == 4 && !memcmp(got, "aboo", 4));
    CHECK(pipe_read_flags(&rx, (uint8_t *)got, sizeof(got), 1) == 4 && rx.rpos == rx.wpos);
    CHECK(file_peek(&uf, got, sizeof(got), 1) == -EAGAIN);
    rx.writers = 0;
    CHECK(file_peek(&uf, got, sizeof(got), 0) == 0);    /* the end, as a read sees it */
    us.rx = NULL;
    CHECK(file_peek(&uf, got, sizeof(got), 1) == -ENOTCONN);
    puts("kernel pipes: zero-length reads, buffered reads, EOF, atomic writes and AF_UNIX peeks passed");
}
