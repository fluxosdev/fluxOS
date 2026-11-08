#include "kernel-test.h"
#include "../kernel/net/socket.c"
#include "../kernel/net/tcp.c"
struct netif netif;
spinlock_t net_lock;
struct proc *current;
volatile uint32_t ticks;
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
void wakeup(void *p) { (void)p; }
void poll_wakeup(void) {}
void sleep_on(void *p) { (void)p; abort(); }
int signal_pending(struct proc *p) { (void)p; return 0; }

/* The checksums, as net.c has them. */
uint32_t csum_add(uint32_t sum, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    for (; len > 1; p += 2, len -= 2)
        sum += (uint32_t)p[0] << 8 | p[1];
    if (len)
        sum += (uint32_t)p[0] << 8;
    return sum;
}
uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}
uint32_t csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint32_t len)
{
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
}

/* TCP against a scripted peer: what we send is decoded into `out`. */
#define LOCAL  IP4(10, 0, 0, 1)
#define REMOTE IP4(10, 0, 0, 2)
int ip_is_local(uint32_t addr) { return addr == LOCAL; }
uint32_t ip_source_for(uint32_t dst) { (void)dst; return LOCAL; }
static struct { uint32_t seq, ack; uint8_t flags; uint16_t win; uint32_t len; int n; } out;
int ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *hdr, uint32_t hlen,
            const void *data, uint32_t dlen)
{
    const struct tcp_hdr *th = hdr;
    CHECK(proto == IPPROTO_TCP && src == LOCAL && dst == REMOTE);
    out.seq = ntohl(th->seq);
    out.ack = ntohl(th->ack);
    out.flags = th->flags;
    out.win = ntohs(th->win);
    out.len = dlen;
    out.n++;
    (void)hlen; (void)data;
    return 0;
}
static void peer(uint16_t sport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t win,
                 uint32_t len)
{
    static uint8_t seg[sizeof(struct tcp_hdr) + 16384];
    struct tcp_hdr *th = (struct tcp_hdr *)seg;
    memset(seg, 0, sizeof(*th) + len);
    th->sport = htons(sport);
    th->dport = htons(80);
    th->seq = htonl(seq);
    th->ack = htonl(ack);
    th->off = sizeof(*th) / 4 << 4;
    th->flags = flags;
    th->win = htons(win);
    uint32_t n = sizeof(*th) + len;
    th->csum = csum_fold(csum_add(csum_pseudo(REMOTE, LOCAL, IPPROTO_TCP, n), seg, n));
    tcp_input(REMOTE, LOCAL, seg, n);
}
static int alive(const struct socket *s)
{
    for (struct socket *o = sockets; o; o = o->next)
        if (o == s)
            return 1;
    return 0;
}
static struct socket *child_of(struct socket *l, uint16_t rport)
{
    for (struct socket *c = sockets; c; c = c->next)
        if (c->parent == l && c->rport == rport)
            return c;
    return NULL;
}
static void run_timers(uint32_t for_ticks)
{
    for (uint32_t i = 0; i < for_ticks; i++, ticks++)
        tcp_timer();
}
/* A connection accepted by `l` from port `rport`, its peer at sequence 1001. */
static struct socket *establish(struct socket *l, uint16_t rport)
{
    peer(rport, 1000, 0, TCP_SYN, 8192, 0);
    struct socket *c = child_of(l, rport);
    CHECK(c && c->state == TCP_SYN_RECEIVED && out.flags == (TCP_SYN | TCP_ACK));
    peer(rport, 1001, c->iss + 1, TCP_ACK, 8192, 0);
    CHECK(c->state == TCP_ESTABLISHED && c->queued);
    return c;
}

static void test_tcp(void)
{
    struct socket *l = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    l->lport = 80;
    l->has_file = 1;
    tcp_listen(l, 4);

    /* SYN-RECEIVED: a bad ACK gets a reset, and its FIN is not taken. */
    peer(5000, 1000, 0, TCP_SYN, 8192, 0);
    struct socket *c = child_of(l, 5000);
    CHECK(c && l->pending == 1);
    peer(5000, 1001, c->iss + 7, TCP_ACK | TCP_FIN, 8192, 0);
    CHECK(out.flags == TCP_RST && out.seq == c->iss + 7);
    CHECK(c->state == TCP_SYN_RECEIVED && c->rcv_nxt == 1001 && !c->peer_fin);
    run_timers(300 * HZ);               /* SYN-ACK retries run out: the slot is freed */
    CHECK(!alive(c) && l->pending == 0);

    /* An ACK for data never sent: answered, and its data and FIN dropped. */
    c = establish(l, 5001);
    peer(5001, 1001, c->snd_max + 100, TCP_ACK | TCP_FIN, 8192, 2);
    CHECK(out.flags == TCP_ACK && out.ack == 1001);
    CHECK(c->state == TCP_ESTABLISHED && c->rlen == 0 && c->rcv_nxt == 1001);

    /* RFC 5961: an in-window reset that is not exact gets a challenge ACK. */
    peer(5001, 1011, c->snd_max, TCP_RST, 0, 0);
    CHECK(c->state == TCP_ESTABLISHED && out.flags == TCP_ACK && out.ack == 1001);
    peer(5001, 1001, c->snd_max, TCP_RST, 0, 0);
    CHECK(c->state == TCP_CLOSED && c->error == ECONNRESET);

    /* The window offered to one connection holds when another one opens. */
    struct socket *a = establish(l, 5002);
    peer(5002, 1001, a->snd_max, TCP_ACK | TCP_PSH, 8192, 1);
    uint32_t offered = out.win;
    CHECK(out.ack == 1002 && offered == TCP_WINDOW);
    struct socket *b = establish(l, 5003);
    CHECK(rcv_window(a) >= offered);
    peer(5002, 1002, a->snd_max, TCP_ACK, 8192, offered);
    CHECK(a->rlen == 1 + offered && a->rcv_nxt == 1002 + offered);
    (void)b;

    /* Closed in SYN-RECEIVED (a simultaneous open): the FIN follows the SYN. */
    struct socket *s = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    s->has_file = 1;
    s->laddr = LOCAL, s->lport = 80, s->raddr = REMOTE, s->rport = 6000;
    CHECK(tcp_connect(s) == 0 && out.flags == TCP_SYN);
    peer(6000, 3000, 0, TCP_SYN, 8192, 0);
    CHECK(s->state == TCP_SYN_RECEIVED && out.flags == (TCP_SYN | TCP_ACK));
    tcp_shutdown(s);
    CHECK(s->state == TCP_SYN_RECEIVED);
    peer(6000, 3001, s->iss + 1, TCP_ACK, 8192, 0);
    CHECK(s->state == TCP_FIN_WAIT_1 && out.flags == (TCP_FIN | TCP_ACK) && out.seq == s->iss + 1);
    peer(6000, 3001, s->iss + 2, TCP_ACK, 8192, 0);
    CHECK(s->state == TCP_FIN_WAIT_2);

    /* A closed socket behind a window that never opens is let go. */
    s = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    s->has_file = 1;
    s->laddr = LOCAL, s->lport = 80, s->raddr = REMOTE, s->rport = 6001;
    tcp_connect(s);
    peer(6001, 4000, s->iss + 1, TCP_SYN | TCP_ACK, 0, 0);
    CHECK(s->state == TCP_ESTABLISHED && s->snd_wnd == 0);
    CHECK(tcp_write(s, (const uint8_t *)"hello", 5) == 5);
    s->has_file = 0;
    tcp_close(s);
    CHECK(s->state == TCP_FIN_WAIT_1);
    for (int i = 0; i < 3000 && alive(s); i++) {
        int sent = out.n;
        run_timers(HZ);
        if (out.n != sent && alive(s))  /* the peer answers each probe: still no room */
            peer(6001, 4001, s->snd_una, TCP_ACK, 0, 0);
    }
    CHECK(!alive(s));

    /* A zero window kept shut by a peer that answers every probe: probes go
     * on (RFC 1122 4.2.2.17), snd_nxt stays, answers are not duplicate ACKs,
     * and the probed byte goes again with the rest once the window opens. */
    s = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    s->has_file = 1;
    s->laddr = LOCAL, s->lport = 80, s->raddr = REMOTE, s->rport = 6002;
    tcp_connect(s);
    peer(6002, 5000, s->iss + 1, TCP_SYN | TCP_ACK, 0, 0);
    CHECK(tcp_write(s, (const uint8_t *)"hello", 5) == 5);
    uint32_t una = s->snd_una;
    for (int i = 0; i < 40; i++) {
        int sent = out.n;
        run_timers(31 * HZ);
        CHECK(out.n != sent && out.seq == una && out.len == 1);
        sent = out.n;
        peer(6002, 5001, una, TCP_ACK, 0, 0);
        CHECK(out.n == sent);           /* no fast retransmit into the closed window */
    }
    CHECK(alive(s) && s->state == TCP_ESTABLISHED && s->snd_nxt == una);
    peer(6002, 5001, una, TCP_ACK, 8192, 0);
    CHECK(out.seq == una && out.len == 5);

    /* New data for a closed socket is refused with a reset, not acknowledged. */
    s = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    s->has_file = 1;
    s->laddr = LOCAL, s->lport = 80, s->raddr = REMOTE, s->rport = 6003;
    tcp_connect(s);
    peer(6003, 7000, s->iss + 1, TCP_SYN | TCP_ACK, 8192, 0);
    s->has_file = 0;
    tcp_close(s);
    CHECK(s->state == TCP_FIN_WAIT_1 && out.flags == (TCP_FIN | TCP_ACK));
    peer(6003, 7001, s->snd_max, TCP_ACK, 8192, 10);
    CHECK(!alive(s) && (out.flags & TCP_RST));

    /* shutdown(SHUT_WR) while connecting: the FIN follows the handshake. */
    s = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    s->has_file = 1;
    s->laddr = LOCAL, s->lport = 80, s->raddr = REMOTE, s->rport = 6004;
    tcp_connect(s);
    tcp_shutdown(s);
    CHECK(s->state == TCP_SYN_SENT);
    peer(6004, 8000, s->iss + 1, TCP_SYN | TCP_ACK, 8192, 0);
    CHECK(s->state == TCP_FIN_WAIT_1 && out.flags == (TCP_FIN | TCP_ACK) && out.seq == s->iss + 1);

    /* Non-blocking accept and connect. */
    struct socket *l2 = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    l2->lport = 81;
    l2->has_file = 1;
    tcp_listen(l2, 4);
    struct socket *got;
    CHECK(sock_accept_locked(l2, &got, NULL, 1) == -EAGAIN);
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(6005),
                               .sin_addr.s_addr = htonl(REMOTE) };
    s = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    s->has_file = 1;
    s->lport = 80;
    CHECK(sock_connect_locked(s, &sin, 1) == -EINPROGRESS && s->state == TCP_SYN_SENT);
    CHECK(sock_connect_locked(s, &sin, 1) == -EALREADY);
    peer(6005, 9000, s->iss + 1, TCP_SYN | TCP_ACK, 8192, 0);
    CHECK(sock_connect_locked(s, &sin, 1) == 0 && s->state == TCP_ESTABLISHED);
    CHECK(sock_connect_locked(s, &sin, 1) == -EISCONN);
    sin.sin_port = htons(6006);
    s = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    s->has_file = 1;
    s->lport = 80;
    CHECK(sock_connect_locked(s, &sin, 1) == -EINPROGRESS);
    peer(6006, 0, s->iss + 1, TCP_RST | TCP_ACK, 0, 0);
    CHECK(sock_connect_locked(s, &sin, 1) == -ECONNREFUSED && !s->connected && !s->raddr);
    puts("kernel sockets: TCP bad ACKs, RFC 5961 resets, window, early close and orphans passed");
    puts("kernel sockets: zero-window probes, data after close, shutdown while connecting, "
         "non-blocking accept and connect passed");
}

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
    test_tcp();
}
