#include "kernel-test.h"
#include "../kernel/socket.c"
struct netif netif;
struct proc *current;
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void wakeup(void *p) { (void)p; }
void poll_wakeup(void) {}
void sleep_on(void *p) { (void)p; abort(); }
int signal_pending(struct proc *p) { (void)p; return 0; }
int tcp_read(struct socket *s, uint8_t *b, uint32_t n, int peek)
{ (void)s; (void)b; (void)n; (void)peek; abort(); }
int main(void)
{
    struct socket s = { .type = SOCK_DGRAM };
    for (int i = 0; i < 10000; i++) enqueue(&s, 1, 1, (const uint8_t *)"", 0);
    CHECK(s.rq_bytes <= UDP_RCVBUF && netif.rx_dropped > 0);
    uint32_t charged = s.rq_bytes;
    CHECK(sock_recvfrom(&s, NULL, 0, MSG_PEEK, NULL) == 0 && s.rq_bytes == charged);
    CHECK(sock_recvfrom(&s, NULL, 0, 0, NULL) == 0);
    CHECK(s.rq_bytes == charged - sizeof(struct dgram));
    enqueue(&s, 1, 1, (const uint8_t *)"", 0);
    CHECK(s.rq_bytes == charged);
    while (s.rq) CHECK(sock_recvfrom(&s, NULL, 0, 0, NULL) == 0);
    CHECK(s.rq_bytes == 0 && !s.rq_tail);
    enqueue(&s, 1, 1, (const uint8_t *)"abcd", 4);
    CHECK(s.rq_bytes == sizeof(struct dgram) + 4);
    char out[2];
    CHECK(sock_recvfrom(&s, out, sizeof(out), 0, NULL) == 2 && !memcmp(out, "ab", 2));
    CHECK(s.rq_bytes == 0 && !s.rq && !s.rq_tail);
    enqueue(&s, 1, 1, (const uint8_t *)"", UINT32_MAX);
    CHECK(!s.rq);
    puts("kernel sockets: bounded empty datagrams, peek, dequeue and truncation passed");
}
