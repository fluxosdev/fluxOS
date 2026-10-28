/*
 * Sockets: the BSD socket calls over TCP, UDP and ICMP echo ("ping"
 * sockets, as in Linux), and the file operations that let read, write,
 * poll and close work on them.
 *
 * Calls that wait (connect, accept, send on a full buffer, receive on
 * an empty one) sleep on the socket; the network interrupt wakes them.
 *
 * The whole network stack runs holding net_lock (net.c): these calls,
 * the network cards' interrupts and the timer tick. A call lets it go
 * while it sleeps. Buffers passed in are the caller's, checked
 * beforehand, so they are copied holding it.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "net.h"
#include "proc.h"
#include "vfs.h"

#define EPHEMERAL_FIRST 49152

struct socket *sockets;
static uint32_t next_port = EPHEMERAL_FIRST;

struct socket *sock_alloc(int type, int proto)
{
    struct socket *s = kzalloc(sizeof(*s));
    if (!s)
        return NULL;
    s->type = type;
    s->proto = proto;
    if (type == SOCK_STREAM) {
        s->sbuf = kmalloc(TCP_SNDBUF);
        s->rbuf = kmalloc(TCP_RING);
        if (!s->sbuf || !s->rbuf) {
            kfree(s->sbuf);
            kfree(s->rbuf);
            kfree(s);
            return NULL;
        }
        s->mss = 536;
        s->rto = HZ;
    }
    s->next = sockets;
    sockets = s;
    return s;
}

void sock_free(struct socket *s)
{
    for (struct socket **pp = &sockets; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    while (s->rq) {
        struct dgram *d = s->rq;
        s->rq = d->next;
        kfree(d);
    }
    kfree(s->sbuf);
    kfree(s->rbuf);
    kfree(s);
}

void sock_wake(struct socket *s)
{
    wakeup(s);
    poll_wakeup();
}

static int port_taken(int proto, uint16_t port, int any_state)
{
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->proto != proto || s->lport != port)
            continue;
        /* A connection lingering after close (TIME_WAIT) does not block bind(). */
        if (any_state || s->has_file || s->state == TCP_LISTEN)
            return 1;
    }
    return 0;
}

uint16_t sock_ephemeral_port(int proto)
{
    for (int i = 0; i < 65536 - EPHEMERAL_FIRST; i++) {
        uint16_t port = (uint16_t)next_port;
        next_port = next_port == 65535 ? EPHEMERAL_FIRST : next_port + 1;
        if (!port_taken(proto, port, 1))
            return port;
    }
    return 0;
}

static void fill_addr(struct sockaddr_in *sin, uint32_t addr, uint16_t port)
{
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr.s_addr = htonl(addr);
}

static int no_route(void)
{
    return netif.present ? -ENETUNREACH : -ENETDOWN;
}

static int sock_create_locked(int domain, int type, int proto, struct socket **out)
{
    if (domain != AF_INET)
        return -EAFNOSUPPORT;
    if (type == SOCK_STREAM) {
        if (proto && proto != IPPROTO_TCP)
            return -EPROTONOSUPPORT;
        proto = IPPROTO_TCP;
    } else if (type == SOCK_DGRAM) {
        if (!proto)
            proto = IPPROTO_UDP;
        else if (proto != IPPROTO_UDP && proto != IPPROTO_ICMP)
            return -EPROTONOSUPPORT;
    } else {
        return -EPROTOTYPE;
    }
    struct socket *s = sock_alloc(type, proto);
    if (!s)
        return -ENOMEM;
    s->has_file = 1;
    *out = s;
    return 0;
}

static int sock_bind_locked(struct socket *s, const struct sockaddr_in *sin)
{
    if (sin->sin_family != AF_INET)
        return -EAFNOSUPPORT;
    if (s->lport)
        return -EINVAL;                 /* already bound */
    uint32_t addr = ntohl(sin->sin_addr.s_addr);
    uint16_t port = ntohs(sin->sin_port);
    if (addr && !ip_is_local(addr))
        return -EADDRNOTAVAIL;
    if (!port)
        port = sock_ephemeral_port(s->proto);
    else if (port_taken(s->proto, port, 0))
        return -EADDRINUSE;
    if (!port)
        return -EADDRINUSE;
    s->laddr = addr;
    s->lport = port;
    return 0;
}

static int bind_ephemeral(struct socket *s)
{
    if (!s->lport)
        s->lport = sock_ephemeral_port(s->proto);
    return s->lport ? 0 : -EADDRNOTAVAIL;
}

static int sock_connect_locked(struct socket *s, const struct sockaddr_in *sin)
{
    if (sin->sin_family != AF_INET)
        return -EAFNOSUPPORT;
    uint32_t addr = ntohl(sin->sin_addr.s_addr);
    uint16_t port = ntohs(sin->sin_port);
    if (!addr)
        addr = INADDR_LOOPBACK;         /* as on Linux: 0.0.0.0 means this host */
    int r = bind_ephemeral(s);
    if (r < 0)
        return r;

    if (s->type == SOCK_DGRAM) {
        s->raddr = addr;
        s->rport = port;
        s->connected = 1;
        return 0;
    }

    if (s->state == TCP_LISTEN)
        return -EINVAL;
    if (s->connected)
        return -EISCONN;
    uint32_t had_laddr = s->laddr;      /* bind()'s, if any: kept whatever happens */
    uint32_t src = had_laddr ? had_laddr : ip_source_for(addr);
    if (!src)
        return no_route();
    s->laddr = src;
    s->raddr = addr;
    s->rport = port;
    s->connected = 1;
    r = tcp_connect(s);
    if (r < 0)
        goto failed;
    while (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECEIVED) {
        if (signal_pending(current)) {
            s->state = TCP_CLOSED;
            s->rtx_on = 0;
            r = -EINTR;
            goto failed;
        }
        sleep_on_lock_intr(s, &net_lock);
    }
    if (s->state == TCP_CLOSED) {
        r = -(s->error ? s->error : ECONNREFUSED);
        s->error = 0;
        goto failed;
    }
    return 0;

failed:
    /* Not connected after all: the socket is as before, and may try again. */
    s->connected = 0;
    s->laddr = had_laddr;
    s->raddr = 0;
    s->rport = 0;
    return r;
}

static int sock_listen_locked(struct socket *s, int backlog)
{
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (s->connected || (s->state != TCP_CLOSED && s->state != TCP_LISTEN))
        return -EINVAL;
    int r = bind_ephemeral(s);
    return r < 0 ? r : tcp_listen(s, backlog);
}

static int sock_accept_locked(struct socket *s, struct socket **out, struct sockaddr_in *peer)
{
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (s->state != TCP_LISTEN)
        return -EINVAL;
    while (!s->acceptq) {
        if (signal_pending(current))
            return -EINTR;
        sleep_on_lock_intr(s, &net_lock);
    }
    struct socket *c = s->acceptq;
    s->acceptq = c->aq_next;
    s->pending--;
    c->aq_next = NULL;
    c->queued = 0;
    c->parent = NULL;
    c->has_file = 1;
    if (peer)
        fill_addr(peer, c->raddr, c->rport);
    *out = c;
    return 0;
}

static int stream_send(struct socket *s, const uint8_t *buf, uint32_t n, int flags)
{
    uint32_t done = 0;
    while (done < n) {
        if (s->error) {
            int err = s->error;
            s->error = 0;
            return done ? (int)done : -err;
        }
        if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) {
            if (!s->connected)
                return -ENOTCONN;
            proc_signal(current, SIGPIPE);
            return done ? (int)done : -EPIPE;
        }
        done += (uint32_t)tcp_write(s, buf + done, n - done);
        if (done < n) {
            if (flags & MSG_DONTWAIT)
                return done ? (int)done : -EAGAIN;
            if (signal_pending(current))
                return done ? (int)done : -EINTR;
            sleep_on_lock_intr(s, &net_lock);
        }
    }
    return (int)done;
}

static int sock_sendto_locked(struct socket *s, const void *buf, uint32_t n, int flags,
                              const struct sockaddr_in *to)
{
    if (s->shut_wr) {
        proc_signal(current, SIGPIPE);
        return -EPIPE;
    }
    if (s->type == SOCK_STREAM)
        return stream_send(s, buf, n, flags);

    uint32_t daddr;
    uint16_t dport;
    if (to) {
        if (to->sin_family != AF_INET)
            return -EAFNOSUPPORT;
        daddr = ntohl(to->sin_addr.s_addr);
        dport = ntohs(to->sin_port);
    } else if (s->connected) {
        daddr = s->raddr;
        dport = s->rport;
    } else {
        return -EDESTADDRREQ;
    }
    int r = bind_ephemeral(s);
    if (r < 0)
        return r;
    uint32_t src = s->laddr ? s->laddr : ip_source_for(daddr);
    if (!src && daddr != INADDR_BROADCAST)
        return no_route();

    if (s->proto == IPPROTO_UDP) {
        if (n > ETH_MTU - sizeof(struct ip_hdr) - sizeof(struct udp_hdr))
            return -EMSGSIZE;
        struct udp_hdr uh;
        uh.sport = htons(s->lport);
        uh.dport = htons(dport);
        uh.len = htons((uint16_t)(sizeof(uh) + n));
        uh.csum = 0;
        uint32_t sum = csum_pseudo(src, daddr, IPPROTO_UDP, sizeof(uh) + n);
        uh.csum = csum_fold(csum_add(csum_add(sum, &uh, sizeof(uh)), buf, n));
        if (!uh.csum)
            uh.csum = 0xFFFF;
        r = ip_send(src, daddr, IPPROTO_UDP, &uh, sizeof(uh), buf, n);
    } else {
        /* ICMP echo: the kernel picks the identifier and the checksum. */
        uint8_t msg[ETH_MTU];
        if (n < sizeof(struct icmp_hdr) || n > ETH_MTU - sizeof(struct ip_hdr))
            return -EINVAL;
        memcpy(msg, buf, n);
        struct icmp_hdr *ih = (struct icmp_hdr *)msg;
        if (ih->type != ICMP_ECHO_REQUEST)
            return -EINVAL;
        ih->code = 0;
        ih->id = htons(s->lport);
        ih->csum = 0;
        ih->csum = csum_fold(csum_add(0, msg, n));
        r = ip_send(src, daddr, IPPROTO_ICMP, msg, n, NULL, 0);
    }
    return r < 0 ? r : (int)n;
}

static int sock_recvfrom_locked(struct socket *s, void *buf, uint32_t n, int flags,
                                struct sockaddr_in *from)
{
    if (s->type == SOCK_STREAM) {
        if (s->state == TCP_LISTEN)
            return -ENOTCONN;
        for (;;) {
            if (s->rlen || !n) {
                if (from)
                    fill_addr(from, s->raddr, s->rport);
                return tcp_read(s, buf, n, flags & MSG_PEEK);
            }
            if (s->error) {
                int err = s->error;
                s->error = 0;
                return -err;
            }
            if (s->peer_fin || s->shut_rd)
                return 0;
            if (s->state == TCP_CLOSED)
                return s->connected ? 0 : -ENOTCONN;
            if (flags & MSG_DONTWAIT)
                return -EAGAIN;
            if (signal_pending(current))
                return -EINTR;
            sleep_on_lock_intr(s, &net_lock);
        }
    }

    while (!s->rq) {
        if (s->error) {
            int err = s->error;
            s->error = 0;
            return -err;
        }
        if (s->shut_rd)
            return 0;
        if (flags & MSG_DONTWAIT)
            return -EAGAIN;
        if (signal_pending(current))
            return -EINTR;
        sleep_on_lock_intr(s, &net_lock);
    }
    struct dgram *d = s->rq;
    uint32_t k = MIN(n, (uint32_t)d->len);
    memcpy(buf, d->data, k);
    if (from)
        fill_addr(from, d->addr, d->port);
    if (!(flags & MSG_PEEK)) {
        s->rq = d->next;
        if (!s->rq)
            s->rq_tail = NULL;
        s->rq_bytes -= (uint32_t)sizeof(*d) + d->len;
        kfree(d);
    }
    return (int)k;
}

static int sock_shutdown_locked(struct socket *s, int how)
{
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        return -EINVAL;
    if (s->type == SOCK_STREAM && !s->connected)
        return -ENOTCONN;
    if (how != SHUT_WR)
        s->shut_rd = 1;
    if (how != SHUT_RD && !s->shut_wr) {
        s->shut_wr = 1;
        if (s->type == SOCK_STREAM)
            tcp_shutdown(s);
    }
    sock_wake(s);
    return 0;
}

static int sock_name_locked(struct socket *s, int peer, struct sockaddr_in *sin)
{
    if (peer && !s->connected)
        return -ENOTCONN;
    if (peer)
        fill_addr(sin, s->raddr, s->rport);
    else
        fill_addr(sin, s->laddr, s->lport);
    return 0;
}

static int sock_poll_locked(struct socket *s)
{
    int ev = 0;
    if (s->error)
        ev |= POLLIN | POLLERR;
    if (s->type == SOCK_DGRAM) {
        if (s->rq || s->shut_rd)
            ev |= POLLIN;
        return ev | POLLOUT;
    }
    if (s->state == TCP_LISTEN)
        return ev | (s->acceptq ? POLLIN : 0);
    if (s->rlen || s->peer_fin || s->shut_rd || (s->state == TCP_CLOSED && s->connected))
        ev |= POLLIN;
    if ((s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) && !s->shut_wr &&
        s->slen < TCP_SNDBUF)
        ev |= POLLOUT;
    if (s->state == TCP_CLOSED && s->connected)
        ev |= POLLHUP;
    return ev;
}

static void sock_close_locked(struct socket *s)
{
    s->has_file = 0;
    if (s->type == SOCK_STREAM)
        tcp_close(s);
    else
        sock_free(s);
}

/* ---- datagram input ---- */

static void enqueue(struct socket *s, uint32_t addr, uint16_t port, const uint8_t *data,
                    uint32_t len)
{
    if (s->shut_rd || len > UDP_RCVBUF - sizeof(struct dgram) ||
        s->rq_bytes > UDP_RCVBUF - sizeof(struct dgram) - len) {
        netif.rx_dropped++;
        return;
    }
    struct dgram *d = kmalloc(sizeof(*d) + len);
    if (!d)
        return;
    d->next = NULL;
    d->addr = addr;
    d->port = port;
    d->len = (uint16_t)len;
    memcpy(d->data, data, len);
    if (s->rq_tail)
        s->rq_tail->next = d;
    else
        s->rq = d;
    s->rq_tail = d;
    s->rq_bytes += (uint32_t)sizeof(*d) + len;
    sock_wake(s);
}

int udp_input(uint32_t src, uint32_t dst, const uint8_t *data, uint32_t len)
{
    const struct udp_hdr *uh = (const struct udp_hdr *)data;
    uint32_t ulen = ntohs(uh->len);
    if (ulen < sizeof(*uh) || ulen > len)
        return 1;                       /* malformed: drop it quietly */
    if (uh->csum && csum_fold(csum_add(csum_pseudo(src, dst, IPPROTO_UDP, ulen), data, ulen)) != 0)
        return 1;
    uint16_t sport = ntohs(uh->sport), dport = ntohs(uh->dport);
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->proto != IPPROTO_UDP || s->lport != dport)
            continue;
        if (s->laddr && s->laddr != dst && ip_is_local(dst))
            continue;
        if (s->connected && (s->raddr != src || s->rport != sport))
            continue;
        enqueue(s, src, sport, data + sizeof(*uh), ulen - sizeof(*uh));
        return 1;
    }
    return 0;
}

void icmp_socket_input(uint32_t src, const uint8_t *data, uint32_t len)
{
    const struct icmp_hdr *ih = (const struct icmp_hdr *)data;
    uint16_t id = ntohs(ih->id);
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->proto == IPPROTO_ICMP && s->lport == id) {
            enqueue(s, src, 0, data, len);
            return;
        }
    }
}

/* ---- the calls, under net_lock ---- */

int sock_create(int domain, int type, int proto, struct socket **out)
{
    spin_lock(&net_lock);
    int r = sock_create_locked(domain, type, proto, out);
    spin_unlock(&net_lock);
    return r;
}

int sock_bind(struct socket *s, const struct sockaddr_in *sin)
{
    spin_lock(&net_lock);
    int r = sock_bind_locked(s, sin);
    spin_unlock(&net_lock);
    return r;
}

int sock_connect(struct socket *s, const struct sockaddr_in *sin)
{
    spin_lock(&net_lock);
    int r = sock_connect_locked(s, sin);
    spin_unlock(&net_lock);
    return r;
}

int sock_listen(struct socket *s, int backlog)
{
    spin_lock(&net_lock);
    int r = sock_listen_locked(s, backlog);
    spin_unlock(&net_lock);
    return r;
}

int sock_accept(struct socket *s, struct socket **out, struct sockaddr_in *peer)
{
    spin_lock(&net_lock);
    int r = sock_accept_locked(s, out, peer);
    spin_unlock(&net_lock);
    return r;
}

int sock_sendto(struct socket *s, const void *buf, uint32_t n, int flags,
                const struct sockaddr_in *to)
{
    spin_lock(&net_lock);
    int r = sock_sendto_locked(s, buf, n, flags, to);
    spin_unlock(&net_lock);
    return r;
}

int sock_recvfrom(struct socket *s, void *buf, uint32_t n, int flags, struct sockaddr_in *from)
{
    spin_lock(&net_lock);
    int r = sock_recvfrom_locked(s, buf, n, flags, from);
    spin_unlock(&net_lock);
    return r;
}

int sock_shutdown(struct socket *s, int how)
{
    spin_lock(&net_lock);
    int r = sock_shutdown_locked(s, how);
    spin_unlock(&net_lock);
    return r;
}

int sock_name(struct socket *s, int peer, struct sockaddr_in *sin)
{
    spin_lock(&net_lock);
    int r = sock_name_locked(s, peer, sin);
    spin_unlock(&net_lock);
    return r;
}

int sock_poll(struct socket *s)
{
    spin_lock(&net_lock);
    int r = sock_poll_locked(s);
    spin_unlock(&net_lock);
    return r;
}

void sock_close(struct socket *s)
{
    spin_lock(&net_lock);
    sock_close_locked(s);
    spin_unlock(&net_lock);
}
