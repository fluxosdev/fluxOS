/*
 * TCP (RFC 793, with the RFC 1122 and RFC 6298 timer rules):
 * connection setup and teardown, reliable in-order delivery, an RTT
 * estimate per connection, retransmission with exponential backoff,
 * fast retransmit after three duplicate ACKs, zero-window probes, and
 * listening sockets with an accept queue.
 *
 * Segments that arrive out of order are dropped and acknowledged with
 * the sequence number we expect, so the sender goes back and resends.
 * There is no congestion control: the sender is limited only by the
 * receiver's window.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "net.h"

#define MSS_LOCAL      (ETH_MTU - 40)
#define MSS_DEFAULT    536
#define RTO_INIT       HZ
#define RTO_MIN        (HZ / 5)
#define RTO_MAX        (30 * HZ)
#define SYN_RETRIES    5
#define DATA_RETRIES   10
#define TIME_WAIT_TIME (10 * HZ)    /* 2 MSL, shortened */
#define FIN_WAIT_TIME  (60 * HZ)    /* for a closed socket whose peer never closes */
#define MAX_BACKLOG    16

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

/* A received segment, decoded. */
struct seg {
    uint32_t src, dst;
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t flags;
    uint16_t win;
    const uint8_t *opts;
    uint32_t optlen;
    const uint8_t *data;
    uint32_t len;
};

static uint32_t new_iss(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (lo ^ hi) * 2654435761u + ticks * 250000u;
}

/*
 * The window we offer: the free space in the receive buffer, but no more
 * than the network card can hold while we are slow to drain it. An
 * NE2000's ring holds about nine full frames, and a burst beyond that is
 * dropped by the card and has to be retransmitted; connections receiving
 * at the same time share that budget.
 */
static uint32_t rcv_window(const struct socket *s)
{
    uint32_t space = TCP_RCVBUF - s->rlen;
    if (ip_is_local(s->raddr))
        return space;
    uint32_t busy = 0;
    for (const struct socket *o = sockets; o; o = o->next)
        if (o->proto == IPPROTO_TCP && o->state == TCP_ESTABLISHED && !ip_is_local(o->raddr))
            busy++;
    uint32_t cap = TCP_WINDOW / (busy ? busy : 1);
    if (cap < 2 * MSS_LOCAL)
        cap = 2 * MSS_LOCAL;
    return MIN(space, cap);
}

static int send_raw(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq,
                    uint32_t ack, uint8_t flags, uint16_t win, const uint8_t *opts, uint32_t olen,
                    const uint8_t *data, uint32_t dlen)
{
    uint8_t hdr[sizeof(struct tcp_hdr) + 4];
    struct tcp_hdr *th = (struct tcp_hdr *)hdr;
    uint32_t hlen = sizeof(*th) + olen;
    th->sport = htons(sport);
    th->dport = htons(dport);
    th->seq = htonl(seq);
    th->ack = htonl(ack);
    th->off = (uint8_t)(hlen / 4 << 4);
    th->flags = flags;
    th->win = htons(win);
    th->csum = 0;
    th->urg = 0;
    if (olen)
        memcpy(hdr + sizeof(*th), opts, olen);
    /* The header is a whole number of words, so the data sum lines up. */
    uint32_t sum = csum_pseudo(src, dst, IPPROTO_TCP, hlen + dlen);
    sum = csum_add(csum_add(sum, hdr, hlen), data, dlen);
    th->csum = csum_fold(sum);
    return ip_send(src, dst, IPPROTO_TCP, hdr, hlen, data, dlen);
}

static int send_segment(struct socket *s, uint32_t seq, uint8_t flags, const uint8_t *data,
                        uint32_t len)
{
    static const uint8_t mss_opt[4] = { 2, 4, MSS_LOCAL >> 8, MSS_LOCAL & 0xFF };
    uint32_t win = rcv_window(s);
    if (SEQ_GT(s->rcv_nxt + win, s->rcv_adv))
        s->rcv_adv = s->rcv_nxt + win;
    return send_raw(s->laddr, s->raddr, s->lport, s->rport, seq, flags & TCP_ACK ? s->rcv_nxt : 0,
                    flags, (uint16_t)win, flags & TCP_SYN ? mss_opt : NULL,
                    flags & TCP_SYN ? sizeof(mss_opt) : 0, data, len);
}

static void send_ack(struct socket *s)
{
    send_segment(s, s->snd_nxt, TCP_ACK, NULL, 0);
}

/* Answer a segment that belongs to no connection with a reset. */
static void reset_reply(const struct seg *g)
{
    if (g->flags & TCP_RST)
        return;
    if (g->flags & TCP_ACK) {
        send_raw(g->dst, g->src, g->dport, g->sport, g->ack, 0, TCP_RST, 0, NULL, 0, NULL, 0);
    } else {
        uint32_t ack = g->seq + g->len + !!(g->flags & TCP_SYN) + !!(g->flags & TCP_FIN);
        send_raw(g->dst, g->src, g->dport, g->sport, 0, ack, TCP_RST | TCP_ACK, 0, NULL, 0, NULL, 0);
    }
}

static void arm_timer(struct socket *s)
{
    if (!s->rtx_on) {
        s->rtx_on = 1;
        s->rtx_at = ticks + s->rto;
    }
}

/* RFC 6298 smoothing, in ticks: srtt is kept times 8, rttvar times 4. */
static void rtt_update(struct socket *s, uint32_t m)
{
    if (!m)
        m = 1;
    if (!s->srtt) {
        s->srtt = m << 3;
        s->rttvar = m << 1;
    } else {
        int32_t delta = (int32_t)m - (int32_t)(s->srtt >> 3);
        s->srtt = (uint32_t)((int32_t)s->srtt + delta);
        if (delta < 0)
            delta = -delta;
        s->rttvar = (uint32_t)((int32_t)s->rttvar + delta - (int32_t)(s->rttvar >> 2));
    }
    uint32_t rto = (s->srtt >> 3) + s->rttvar;
    s->rto = rto < RTO_MIN ? RTO_MIN : rto > RTO_MAX ? RTO_MAX : rto;
}

/* The connection is over. Free it unless a file or a listener still holds it. */
static void tcp_closed(struct socket *s, int err)
{
    if (err)
        s->error = err;
    s->state = TCP_CLOSED;
    s->rtx_on = 0;
    if (s->parent && !s->queued) {      /* never established: the listener forgets it */
        s->parent->pending--;
        sock_free(s);
        return;
    }
    sock_wake(s);
    if (!s->has_file && !s->queued)
        sock_free(s);
}

static void send_reset(struct socket *s)
{
    send_segment(s, s->snd_nxt, TCP_RST | TCP_ACK, NULL, 0);
}

/*
 * Send what the window allows: new data in MSS-sized segments, then our
 * FIN once the data is out. At most `limit` segments (0: no limit).
 */
static void output(struct socket *s, int limit)
{
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT && s->state != TCP_FIN_WAIT_1 &&
        s->state != TCP_CLOSING && s->state != TCP_LAST_ACK)
        return;
    uint32_t wnd_end = s->snd_una + s->snd_wnd;
    for (int sent = 0; !limit || sent < limit; sent++) {
        uint32_t off = s->snd_nxt - s->snd_una;
        if (off < s->slen) {
            uint32_t len = MIN(s->slen - off, (uint32_t)s->mss);
            uint32_t room = SEQ_GT(wnd_end, s->snd_nxt) ? wnd_end - s->snd_nxt : 0;
            if (len > room)
                len = room;
            if (!len)
                break;
            if (!s->rtt_timing && s->snd_nxt == s->snd_max) {
                s->rtt_timing = 1;
                s->rtt_seq = s->snd_nxt + len;
                s->rtt_start = ticks;
            }
            uint8_t flags = TCP_ACK | (off + len == s->slen ? TCP_PSH : 0);
            send_segment(s, s->snd_nxt, flags, s->sbuf + off, len);
            s->snd_nxt += len;
        } else if (s->fin_queued && !s->fin_sent && off == s->slen) {
            send_segment(s, s->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
            s->snd_nxt++;
            s->fin_sent = 1;
        } else {
            break;
        }
        if (SEQ_GT(s->snd_nxt, s->snd_max))
            s->snd_max = s->snd_nxt;
        arm_timer(s);
    }
    /* Data waits behind a closed window: the timer will probe it. */
    if (s->slen > s->snd_nxt - s->snd_una)
        arm_timer(s);
}

static void retransmit(struct socket *s)
{
    int probe = s->snd_wnd == 0 && s->snd_nxt == s->snd_una && s->slen > 0;
    int max = s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECEIVED ? SYN_RETRIES : DATA_RETRIES;
    if (!probe && ++s->retries > max) {
        tcp_closed(s, ETIMEDOUT);
        return;
    }
    s->rto = MIN(s->rto * 2, (uint32_t)RTO_MAX);
    s->rtt_timing = 0;                  /* Karn: never time a retransmission */
    s->dupacks = 0;
    s->rtx_on = 1;
    s->rtx_at = ticks + s->rto;

    if (s->state == TCP_SYN_SENT) {
        send_segment(s, s->iss, TCP_SYN, NULL, 0);
    } else if (s->state == TCP_SYN_RECEIVED) {
        send_segment(s, s->iss, TCP_SYN | TCP_ACK, NULL, 0);
    } else if (probe) {
        /* One byte past the closed window: the reply tells us when it opens. */
        send_segment(s, s->snd_una, TCP_ACK, s->sbuf, 1);
        s->snd_nxt = s->snd_una + 1;
        if (SEQ_GT(s->snd_nxt, s->snd_max))
            s->snd_max = s->snd_nxt;
    } else {
        /* Go back to the oldest unacknowledged byte; ACKs bring the rest. */
        s->snd_nxt = s->snd_una;
        s->fin_sent = 0;
        s->rtx_on = 0;
        output(s, 1);                   /* re-arms the timer for what it sends */
    }
}

static uint16_t parse_mss(const uint8_t *opt, uint32_t len)
{
    for (uint32_t i = 0; i < len;) {
        if (opt[i] == 0)
            break;
        if (opt[i] == 1) {
            i++;
            continue;
        }
        if (i + 1 >= len || opt[i + 1] < 2 || i + opt[i + 1] > len)
            break;
        if (opt[i] == 2 && opt[i + 1] == 4) {
            uint16_t mss = (uint16_t)(opt[i + 2] << 8 | opt[i + 3]);
            return mss < 64 ? 64 : mss > MSS_LOCAL ? MSS_LOCAL : mss;
        }
        i += opt[i + 1];
    }
    return MSS_DEFAULT;
}

static struct socket *lookup(const struct seg *g)
{
    struct socket *listener = NULL;
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->proto != IPPROTO_TCP || s->lport != g->dport || s->state == TCP_CLOSED)
            continue;
        if (s->state == TCP_LISTEN) {
            if (!s->laddr || s->laddr == g->dst)
                listener = s;
        } else if (s->rport == g->sport && s->raddr == g->src && s->laddr == g->dst) {
            return s;
        }
    }
    return listener;
}

/* ---- input, by state ---- */

static void listen_input(struct socket *l, const struct seg *g)
{
    if (g->flags & TCP_RST)
        return;
    if (g->flags & TCP_ACK) {
        reset_reply(g);
        return;
    }
    if (!(g->flags & TCP_SYN) || l->pending >= l->backlog)
        return;                         /* when full, the client's retry may find room */
    struct socket *c = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    if (!c)
        return;
    c->laddr = g->dst;
    c->lport = g->dport;
    c->raddr = g->src;
    c->rport = g->sport;
    c->connected = 1;
    c->parent = l;
    l->pending++;
    c->irs = g->seq;
    c->rcv_nxt = g->seq + 1;
    c->mss = parse_mss(g->opts, g->optlen);
    c->snd_wnd = g->win;
    c->snd_wl1 = g->seq;
    c->iss = new_iss();
    c->snd_una = c->iss;
    c->snd_nxt = c->snd_max = c->iss + 1;
    c->snd_wl2 = c->iss;
    c->state = TCP_SYN_RECEIVED;
    c->rto = RTO_INIT;
    send_segment(c, c->iss, TCP_SYN | TCP_ACK, NULL, 0);
    arm_timer(c);
}

static void syn_sent_input(struct socket *s, const struct seg *g)
{
    if ((g->flags & TCP_ACK) && (SEQ_LEQ(g->ack, s->iss) || SEQ_GT(g->ack, s->snd_max))) {
        reset_reply(g);
        return;
    }
    if (g->flags & TCP_RST) {
        if (g->flags & TCP_ACK)
            tcp_closed(s, ECONNREFUSED);
        return;
    }
    if (!(g->flags & TCP_SYN))
        return;
    s->irs = g->seq;
    s->rcv_nxt = g->seq + 1;
    s->mss = parse_mss(g->opts, g->optlen);
    if (!(g->flags & TCP_ACK)) {        /* simultaneous open */
        s->state = TCP_SYN_RECEIVED;
        send_segment(s, s->iss, TCP_SYN | TCP_ACK, NULL, 0);
        return;
    }
    s->snd_una = g->ack;
    s->snd_wnd = g->win;
    s->snd_wl1 = g->seq;
    s->snd_wl2 = g->ack;
    if (s->rtt_timing && s->retries == 0)
        rtt_update(s, ticks - s->rtt_start);
    s->rtt_timing = 0;
    s->retries = 0;
    s->rtx_on = 0;
    s->state = TCP_ESTABLISHED;
    send_ack(s);
    sock_wake(s);
}

static int acceptable(const struct socket *s, const struct seg *g)
{
    uint32_t wnd = rcv_window(s);
    if (g->len == 0)
        return wnd == 0 ? g->seq == s->rcv_nxt
                        : SEQ_GEQ(g->seq, s->rcv_nxt) && SEQ_LT(g->seq, s->rcv_nxt + wnd);
    if (wnd == 0)
        return 0;
    uint32_t last = g->seq + g->len - 1;
    return (SEQ_GEQ(g->seq, s->rcv_nxt) && SEQ_LT(g->seq, s->rcv_nxt + wnd)) ||
           (SEQ_GEQ(last, s->rcv_nxt) && SEQ_LT(last, s->rcv_nxt + wnd));
}

static void enter_time_wait(struct socket *s)
{
    s->state = TCP_TIME_WAIT;
    s->idle_at = ticks + TIME_WAIT_TIME;
    s->rtx_on = 0;
}

/* Process the acknowledgement; returns 0 if the socket is gone. */
static int ack_input(struct socket *s, const struct seg *g)
{
    if (s->state == TCP_SYN_RECEIVED) {
        if (SEQ_LEQ(g->ack, s->snd_una) || SEQ_GT(g->ack, s->snd_max)) {
            reset_reply(g);
            return 1;
        }
        s->snd_una = g->ack;            /* our SYN */
        s->snd_wnd = g->win;
        s->snd_wl1 = g->seq;
        s->snd_wl2 = g->ack;
        s->retries = 0;
        s->rtx_on = 0;
        s->state = TCP_ESTABLISHED;
        if (s->parent) {
            struct socket **pp = &s->parent->acceptq;
            while (*pp)
                pp = &(*pp)->aq_next;
            *pp = s;
            s->queued = 1;
            sock_wake(s->parent);
        } else {
            sock_wake(s);
        }
    }

    if (SEQ_GT(g->ack, s->snd_max)) {
        send_ack(s);                    /* acknowledges something we never sent */
        return 1;
    }
    if (SEQ_GT(g->ack, s->snd_una)) {
        uint32_t acked = g->ack - s->snd_una;
        uint32_t data = MIN(acked, s->slen);
        memmove(s->sbuf, s->sbuf + data, s->slen - data);
        s->slen -= data;
        int fin_acked = acked > data;   /* the FIN took the last sequence number */
        s->snd_una = g->ack;
        if (SEQ_LT(s->snd_nxt, s->snd_una))
            s->snd_nxt = s->snd_una;
        if (s->rtt_timing && SEQ_GEQ(g->ack, s->rtt_seq)) {
            rtt_update(s, ticks - s->rtt_start);
            s->rtt_timing = 0;
        }
        s->retries = 0;
        s->dupacks = 0;
        if (s->snd_una == s->snd_max) {
            s->rtx_on = 0;
        } else {
            s->rtx_on = 1;
            s->rtx_at = ticks + s->rto;
        }
        if (data)
            sock_wake(s);
        if (fin_acked) {
            switch (s->state) {
            case TCP_FIN_WAIT_1:
                s->state = TCP_FIN_WAIT_2;
                s->idle_at = ticks + FIN_WAIT_TIME;
                break;
            case TCP_CLOSING:
                enter_time_wait(s);
                break;
            case TCP_LAST_ACK:
                tcp_closed(s, 0);
                return 0;
            default:
                break;
            }
        }
    } else if (g->ack == s->snd_una && g->len == 0 && g->win == s->snd_wnd &&
               s->snd_max != s->snd_una && !(g->flags & TCP_FIN)) {
        /* The third duplicate ACK: the segment at snd_una was lost. */
        if (++s->dupacks == 3 && s->slen) {
            send_segment(s, s->snd_una, TCP_ACK, s->sbuf, MIN(s->slen, (uint32_t)s->mss));
            s->rtt_timing = 0;
        }
    }

    if (SEQ_LT(s->snd_wl1, g->seq) || (s->snd_wl1 == g->seq && SEQ_LEQ(s->snd_wl2, g->ack))) {
        s->snd_wnd = g->win;
        s->snd_wl1 = g->seq;
        s->snd_wl2 = g->ack;
    }
    return 1;
}

static void data_input(struct socket *s, const struct seg *g, int *need_ack)
{
    const uint8_t *d = g->data;
    uint32_t n = g->len, seq = g->seq;
    if (SEQ_LT(seq, s->rcv_nxt)) {      /* partly old: keep the new part */
        uint32_t skip = s->rcv_nxt - seq;
        if (skip >= n)
            n = 0;
        else
            n -= skip, d += skip;
        seq = s->rcv_nxt;
    }
    *need_ack = 1;
    if (!n || seq != s->rcv_nxt)
        return;                         /* out of order: wait for the gap to be resent */
    uint32_t take = MIN(n, rcv_window(s));
    if (s->has_file || s->queued) {
        uint32_t tail = (s->rhead + s->rlen) % TCP_RING;
        uint32_t first = MIN(take, TCP_RING - tail);
        memcpy(s->rbuf + tail, d, first);
        memcpy(s->rbuf, d + first, take - first);
        s->rlen += take;
        sock_wake(s);
    }
    /* A closed socket has no reader: the data is acknowledged and dropped. */
    s->rcv_nxt += take;
}

static void fin_input(struct socket *s, const struct seg *g, int *need_ack)
{
    *need_ack = 1;
    if (s->state == TCP_TIME_WAIT) {
        s->idle_at = ticks + TIME_WAIT_TIME;    /* our last ACK was lost */
        return;
    }
    if (g->seq + g->len != s->rcv_nxt || s->peer_fin)
        return;                         /* data before the FIN is still missing */
    s->rcv_nxt++;
    s->peer_fin = 1;
    switch (s->state) {
    case TCP_SYN_RECEIVED:
    case TCP_ESTABLISHED:
        s->state = TCP_CLOSE_WAIT;
        break;
    case TCP_FIN_WAIT_1:
        if (s->fin_sent && s->snd_una == s->snd_max)
            enter_time_wait(s);
        else
            s->state = TCP_CLOSING;
        break;
    case TCP_FIN_WAIT_2:
        enter_time_wait(s);
        break;
    default:
        break;
    }
    sock_wake(s);
}

static void synchronized_input(struct socket *s, const struct seg *g)
{
    if (!acceptable(s, g)) {
        if (!(g->flags & TCP_RST))
            send_ack(s);
        return;
    }
    if (g->flags & TCP_RST) {
        int err = 0;
        if (s->state == TCP_SYN_RECEIVED)
            err = s->parent ? 0 : ECONNREFUSED;
        else if (s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT_1 ||
                 s->state == TCP_FIN_WAIT_2 || s->state == TCP_CLOSE_WAIT)
            err = ECONNRESET;
        tcp_closed(s, err);
        return;
    }
    if (g->flags & TCP_SYN) {
        send_ack(s);                    /* RFC 5961: a challenge ACK */
        return;
    }
    if (!(g->flags & TCP_ACK))
        return;
    if (!ack_input(s, g))
        return;

    int need_ack = 0;
    if (g->len && (s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT_1 ||
                   s->state == TCP_FIN_WAIT_2))
        data_input(s, g, &need_ack);
    if (g->flags & TCP_FIN)
        fin_input(s, g, &need_ack);
    if (need_ack)
        send_ack(s);
    output(s, 0);                       /* the ACK may have opened the window */
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *data, uint32_t len)
{
    if (len < sizeof(struct tcp_hdr))
        return;
    const struct tcp_hdr *th = (const struct tcp_hdr *)data;
    uint32_t hlen = (uint32_t)(th->off >> 4) * 4;
    if (hlen < sizeof(*th) || hlen > len)
        return;
    if (csum_fold(csum_add(csum_pseudo(src, dst, IPPROTO_TCP, len), data, len)) != 0)
        return;

    struct seg g = {
        .src = src, .dst = dst,
        .sport = ntohs(th->sport), .dport = ntohs(th->dport),
        .seq = ntohl(th->seq), .ack = ntohl(th->ack),
        .flags = th->flags, .win = ntohs(th->win),
        .opts = data + sizeof(*th), .optlen = hlen - sizeof(*th),
        .data = data + hlen, .len = len - hlen,
    };
    struct socket *s = lookup(&g);
    if (!s) {
        reset_reply(&g);
        return;
    }
    if (s->state == TCP_LISTEN)
        listen_input(s, &g);
    else if (s->state == TCP_SYN_SENT)
        syn_sent_input(s, &g);
    else
        synchronized_input(s, &g);
}

/* ---- timers ---- */

void tcp_timer(void)
{
    struct socket *next;
    for (struct socket *s = sockets; s; s = next) {
        next = s->next;
        if (s->proto != IPPROTO_TCP)
            continue;
        if ((s->state == TCP_TIME_WAIT || (s->state == TCP_FIN_WAIT_2 && !s->has_file)) &&
            (int32_t)(ticks - s->idle_at) >= 0)
            tcp_closed(s, 0);
        else if (s->rtx_on && (int32_t)(ticks - s->rtx_at) >= 0)
            retransmit(s);
    }
}

/* ---- calls from the socket layer ---- */

int tcp_connect(struct socket *s)
{
    s->iss = new_iss();
    s->snd_una = s->iss;
    s->snd_nxt = s->snd_max = s->iss + 1;
    s->state = TCP_SYN_SENT;
    s->mss = MSS_DEFAULT;
    s->rto = RTO_INIT;
    s->rtt_timing = 1;
    s->rtt_seq = s->iss + 1;
    s->rtt_start = ticks;
    int r = send_segment(s, s->iss, TCP_SYN, NULL, 0);
    if (r < 0) {
        s->state = TCP_CLOSED;
        return r;
    }
    arm_timer(s);
    return 0;
}

int tcp_listen(struct socket *s, int backlog)
{
    s->state = TCP_LISTEN;
    s->backlog = backlog < 1 ? 1 : backlog > MAX_BACKLOG ? MAX_BACKLOG : backlog;
    return 0;
}

int tcp_write(struct socket *s, const uint8_t *buf, uint32_t n)
{
    uint32_t k = MIN(n, TCP_SNDBUF - s->slen);
    memcpy(s->sbuf + s->slen, buf, k);
    s->slen += k;
    output(s, 0);
    return (int)k;
}

int tcp_read(struct socket *s, uint8_t *buf, uint32_t n, int peek)
{
    uint32_t take = MIN(n, s->rlen);
    uint32_t first = MIN(take, TCP_RING - s->rhead);
    memcpy(buf, s->rbuf + s->rhead, first);
    memcpy(buf + first, s->rbuf, take - first);
    if (peek)
        return (int)take;
    s->rhead = (s->rhead + take) % TCP_RING;
    s->rlen -= take;
    /* Tell the peer about the space once it is worth a couple of segments. */
    uint32_t offered = s->rcv_adv - s->rcv_nxt;
    if ((s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT_1 || s->state == TCP_FIN_WAIT_2) &&
        rcv_window(s) - offered >= MIN(2u * s->mss, (uint32_t)TCP_RCVBUF / 2))
        send_ack(s);
    return (int)take;
}

void tcp_shutdown(struct socket *s)
{
    if (s->state == TCP_ESTABLISHED || s->state == TCP_SYN_RECEIVED)
        s->state = TCP_FIN_WAIT_1;
    else if (s->state == TCP_CLOSE_WAIT)
        s->state = TCP_LAST_ACK;
    else
        return;
    s->fin_queued = 1;
    output(s, 0);
}

void tcp_close(struct socket *s)
{
    switch (s->state) {
    case TCP_LISTEN: {
        /* Connections nobody accepted are reset. */
        for (struct socket *c = sockets; c;) {
            if (c->parent != s) {
                c = c->next;
                continue;
            }
            if (c->state != TCP_CLOSED)
                send_reset(c);
            sock_free(c);
            c = sockets;                /* the list changed: start over */
        }
        sock_free(s);
        return;
    }
    case TCP_CLOSED:
    case TCP_SYN_SENT:
        sock_free(s);
        return;
    case TCP_SYN_RECEIVED:
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        if (s->rlen) {                  /* unread data: abort, as RFC 2525 asks */
            send_reset(s);
            sock_free(s);
            return;
        }
        tcp_shutdown(s);
        return;
    case TCP_FIN_WAIT_2:
        s->idle_at = ticks + FIN_WAIT_TIME;
        return;
    default:
        return;                         /* already closing; the timers finish the job */
    }
}
