/*
 * TCP/IP networking: the network interface, protocol headers, and
 * sockets. Addresses and ports are kept in host byte order inside the
 * kernel and converted at the system call boundary and on the wire.
 *
 * Packets are processed in the network card's interrupt handler and
 * the timer interrupt. Both run with interrupts off, like all kernel
 * code, so nothing else runs in the middle of them.
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <flux/abi.h>

static inline uint16_t htons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

#define IP4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define IP_FMT "%u.%u.%u.%u"
#define IP_ARGS(ip) (unsigned)((ip) >> 24), (unsigned)((ip) >> 16 & 0xFF), \
                    (unsigned)((ip) >> 8 & 0xFF), (unsigned)((ip) & 0xFF)

/* ---- wire formats ---- */
#define ETH_ALEN      6
#define ETH_HLEN      14
#define ETH_MTU       1500
#define ETH_FRAME_MAX (ETH_HLEN + ETH_MTU)
#define ETH_P_IP      0x0800
#define ETH_P_ARP     0x0806

struct eth_hdr {
    uint8_t dst[ETH_ALEN];
    uint8_t src[ETH_ALEN];
    uint16_t type;
} __attribute__((packed));

struct ip_hdr {
    uint8_t vhl;                /* version 4, header length in words */
    uint8_t tos;
    uint16_t len;
    uint16_t id;
    uint16_t frag;              /* flags and fragment offset */
    uint8_t ttl;
    uint8_t proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

#define IP_DF 0x4000
#define IP_MF 0x2000
#define IP_OFFMASK 0x1FFF

struct icmp_hdr {
    uint8_t type, code;
    uint16_t csum;
    uint16_t id, seq;           /* echo; "unused" for other types */
} __attribute__((packed));

#define ICMP_ECHO_REPLY   0
#define ICMP_UNREACH      3
#define ICMP_ECHO_REQUEST 8
#define ICMP_UNREACH_PORT 3

struct udp_hdr {
    uint16_t sport, dport;
    uint16_t len;
    uint16_t csum;
} __attribute__((packed));

struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off;                /* header length in words, high nibble */
    uint8_t flags;
    uint16_t win;
    uint16_t csum;
    uint16_t urg;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* ---- the interface ---- */

/* What a network card driver provides. */
struct netdev {
    char name[8];                       /* "eth0"; "ne0" when empty */
    char driver[32];
    uint8_t mac[ETH_ALEN];
    uint32_t io_base, irq;
    int (*transmit)(const void *frame, uint32_t len);
    void (*poll)(void);                 /* optional: from the timer, ten times a second */
};

/* An Ethernet PHY on a MAC's MII management bus (mii.c). */
struct mii_phy {
    int (*read)(int addr, int reg);     /* < 0 on error */
    int (*write)(int addr, int reg, uint16_t value);
    int addr;
    uint32_t id;                        /* PHYSID1 << 16 | PHYSID2 */
    char name[24];
    int rtl8201;                        /* a Realtek RTL8201CP/CL/N */
    int nv_fix;                         /* the MAC is an nForce that needs the RTL8201 fix */
};

int mii_probe(struct mii_phy *phy);     /* find it: 0, or -1 */
int mii_init(struct mii_phy *phy);      /* reset, advertise 10/100, autonegotiate */
void mii_power_up(struct mii_phy *phy);
int mii_link(struct mii_phy *phy, int *speed, int *full);  /* 1 if the link is up */

struct netif {
    int present;                /* a card was found */
    struct netdev dev;
    uint32_t ip, mask, gw, dns; /* ip 0: not configured */
    int use_dhcp;
    uint32_t lease_end;         /* tick when the DHCP lease runs out */
    uint32_t rx_packets, tx_packets, rx_bytes, tx_bytes;
    uint32_t rx_errors, tx_errors, rx_dropped;
};
extern struct netif netif;

void net_init(void);
void net_tick(void);                    /* timer interrupt, every tick */
void net_register(const struct netdev *dev);
void net_receive(const uint8_t *frame, uint32_t len);   /* driver, in its IRQ */
void net_get_info(struct netinfo *ni);
int net_configure(const struct netinfo *ni);

int ne2000_init(void);
int forcedeth_init(void);

/* Ones' complement sums: accumulate with csum_add, finish with csum_fold. */
uint32_t csum_add(uint32_t sum, const void *data, uint32_t len);
uint16_t csum_fold(uint32_t sum);
uint32_t csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint32_t len);

/* The address we send from to reach dst (0 if dst is unreachable). */
uint32_t ip_source_for(uint32_t dst);
int ip_is_local(uint32_t addr);
/* Send hdr followed by data as the payload of an IP packet. */
int ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *hdr, uint32_t hlen,
            const void *data, uint32_t dlen);

/* ---- sockets ---- */
enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

#define TCP_SNDBUF 16384
#define TCP_RCVBUF 65535            /* the largest window without scaling */
#define TCP_RING   65536            /* the receive ring holds up to TCP_RCVBUF bytes */
#define TCP_WINDOW (8 * (ETH_MTU - 40)) /* the most we offer a remote host */
#define UDP_RCVBUF 65536

/* A received datagram waiting to be read. */
struct dgram {
    struct dgram *next;
    uint32_t addr;
    uint16_t port;
    uint16_t len;
    uint8_t data[];
};

struct socket {
    int type;                   /* SOCK_STREAM or SOCK_DGRAM */
    int proto;                  /* IPPROTO_TCP, IPPROTO_UDP or IPPROTO_ICMP */
    int has_file;               /* an open file refers to it */
    int error;                  /* pending error (positive errno), reported once */
    uint32_t laddr, raddr;      /* laddr 0: any local address */
    uint16_t lport, rport;      /* ICMP: lport is the echo identifier */
    int connected;
    int shut_rd, shut_wr;

    /* datagram sockets */
    struct dgram *rq, *rq_tail;
    uint32_t rq_bytes;          /* queued allocation bytes, including datagram headers */

    /* TCP */
    enum tcp_state state;
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2;
    uint32_t irs, rcv_nxt, rcv_adv;     /* rcv_adv: right edge of the window we offered */
    uint16_t mss;
    uint8_t *sbuf;              /* data from snd_una on: unacknowledged, then unsent */
    uint32_t slen;
    uint8_t *rbuf;              /* received, unread data (a ring) */
    uint32_t rhead, rlen;
    int fin_queued, fin_sent, peer_fin;
    uint32_t rto, rtx_at;       /* retransmission timeout, and the deadline if rtx_on */
    int rtx_on;
    int retries, dupacks;
    uint32_t srtt, rttvar;      /* in ticks, scaled by 8 and 4 */
    uint32_t rtt_seq, rtt_start;
    int rtt_timing;
    uint32_t idle_at;           /* TIME_WAIT and orphaned FIN_WAIT_2 deadline */
    struct socket *parent;      /* listener of a connection not yet accepted */
    int queued;                 /* on the parent's accept queue */
    struct socket *acceptq;     /* listener: established connections */
    struct socket *aq_next;
    int backlog, pending;       /* listener: queue limit, connections in progress */

    struct socket *next;        /* every socket */
};

extern struct socket *sockets;

struct socket *sock_alloc(int type, int proto);
void sock_free(struct socket *s);
void sock_wake(struct socket *s);
uint16_t sock_ephemeral_port(int proto);

int sock_create(int domain, int type, int proto, struct socket **out);
int sock_bind(struct socket *s, const struct sockaddr_in *sin);
int sock_connect(struct socket *s, const struct sockaddr_in *sin);
int sock_listen(struct socket *s, int backlog);
int sock_accept(struct socket *s, struct socket **out, struct sockaddr_in *peer);
int sock_sendto(struct socket *s, const void *buf, uint32_t n, int flags,
                const struct sockaddr_in *to);
int sock_recvfrom(struct socket *s, void *buf, uint32_t n, int flags, struct sockaddr_in *from);
int sock_shutdown(struct socket *s, int how);
int sock_name(struct socket *s, int peer, struct sockaddr_in *sin);
int sock_poll(struct socket *s);
void sock_close(struct socket *s);

/* Deliver a UDP datagram to its socket; returns 0 if no socket wants it. */
int udp_input(uint32_t src, uint32_t dst, const uint8_t *data, uint32_t len);
void icmp_socket_input(uint32_t src, const uint8_t *data, uint32_t len);
void dhcp_input(const uint8_t *data, uint32_t len);
void icmp_send_unreachable(const struct ip_hdr *ip, uint32_t len, uint8_t code);

/* tcp.c */
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *data, uint32_t len);
void tcp_timer(void);
int tcp_connect(struct socket *s);
int tcp_write(struct socket *s, const uint8_t *buf, uint32_t n);  /* what fits, no blocking */
int tcp_read(struct socket *s, uint8_t *buf, uint32_t n, int peek);
void tcp_shutdown(struct socket *s);    /* no more sending: queue a FIN */
void tcp_close(struct socket *s);       /* the last file reference is gone */
int tcp_listen(struct socket *s, int backlog);

#endif
