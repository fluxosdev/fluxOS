/*
 * The network interface and the lower protocols: Ethernet, ARP, IPv4
 * (no fragments), ICMP echo, a loopback path for 127.0.0.0/8 and our
 * own address, and a DHCP client that configures the interface at boot.
 *
 * Kernel options: net.ip=10.0.2.15/24 net.gw=10.0.2.2 net.dns=10.0.2.3
 * configure the address by hand instead of DHCP.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "net.h"

#define ARP_ENTRIES    16
#define ARP_TTL        (300 * HZ)   /* forget a resolved address after 5 minutes */
#define ARP_RETRY      HZ
#define ARP_TRIES      3
#define LOOP_QUEUE     64
#define IP_TTL         64

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC       0x63825363u

struct netif netif;
/* The whole stack: socket calls, the cards' interrupts and the timer. */
spinlock_t net_lock = SPINLOCK_INIT("net");

static const uint8_t broadcast_mac[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t ip_id;

/* ---- checksums ---- */

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

/* ---- Ethernet ---- */

static int eth_send(const uint8_t *dst_mac, uint16_t type, const void *payload, uint32_t len)
{
    uint8_t frame[ETH_FRAME_MAX];
    if (!netif.present || len > ETH_MTU)
        return -ENETDOWN;
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    memcpy(eh->dst, dst_mac, ETH_ALEN);
    memcpy(eh->src, netif.dev.mac, ETH_ALEN);
    eh->type = htons(type);
    memcpy(frame + ETH_HLEN, payload, len);
    int r = netif.dev.transmit(frame, ETH_HLEN + len);
    if (r < 0) {
        netif.tx_errors++;
        return r;
    }
    netif.tx_packets++;
    netif.tx_bytes += ETH_HLEN + len;
    return 0;
}

/* ---- ARP ---- */

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[ETH_ALEN];
    uint32_t spa;
    uint8_t tha[ETH_ALEN];
    uint32_t tpa;
} __attribute__((packed));

#define ARP_REQUEST 1
#define ARP_REPLY   2

static struct arp_entry {
    uint32_t ip;                /* 0: unused */
    uint8_t mac[ETH_ALEN];
    int resolved;
    uint32_t expires;           /* resolved: when to forget; pending: next retry */
    int tries;
    uint8_t *pending;           /* an IP packet waiting for the address */
    uint32_t pending_len;
} arp_table[ARP_ENTRIES];

static void arp_send(uint16_t op, const uint8_t *tha, uint32_t tpa, const uint8_t *dst_mac)
{
    struct arp_pkt a;
    a.htype = htons(1);
    a.ptype = htons(ETH_P_IP);
    a.hlen = ETH_ALEN;
    a.plen = 4;
    a.op = htons(op);
    memcpy(a.sha, netif.dev.mac, ETH_ALEN);
    a.spa = htonl(netif.ip);
    memcpy(a.tha, tha, ETH_ALEN);
    a.tpa = htonl(tpa);
    eth_send(dst_mac, ETH_P_ARP, &a, sizeof(a));
}

static struct arp_entry *arp_find(uint32_t ip)
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (arp_table[i].ip == ip)
            return &arp_table[i];
    return NULL;
}

static void arp_drop_pending(struct arp_entry *e)
{
    kfree(e->pending);
    e->pending = NULL;
    e->pending_len = 0;
}

/* A slot for ip: a free one, or the one closest to expiring. */
static struct arp_entry *arp_slot(uint32_t ip)
{
    struct arp_entry *e = arp_find(ip);
    if (e)
        return e;
    struct arp_entry *best = &arp_table[0];
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (!arp_table[i].ip) {
            best = &arp_table[i];
            break;
        }
        if ((int32_t)(arp_table[i].expires - best->expires) < 0)
            best = &arp_table[i];
    }
    arp_drop_pending(best);
    memset(best, 0, sizeof(*best));
    best->ip = ip;
    return best;
}

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    struct arp_entry *e = arp_slot(ip);
    memcpy(e->mac, mac, ETH_ALEN);
    e->resolved = 1;
    e->expires = ticks + ARP_TTL;
    if (e->pending) {
        uint8_t *pkt = e->pending;
        uint32_t len = e->pending_len;
        e->pending = NULL;
        eth_send(mac, ETH_P_IP, pkt, len);
        kfree(pkt);
    }
}

static void arp_input(const uint8_t *data, uint32_t len)
{
    if (len < sizeof(struct arp_pkt))
        return;
    const struct arp_pkt *a = (const struct arp_pkt *)data;
    if (a->htype != htons(1) || a->ptype != htons(ETH_P_IP) || a->hlen != ETH_ALEN || a->plen != 4)
        return;
    uint32_t spa = ntohl(a->spa), tpa = ntohl(a->tpa);
    if (!netif.ip || tpa != netif.ip) {
        /* Not for us, but keep an entry we already have up to date. */
        struct arp_entry *e = spa ? arp_find(spa) : NULL;
        if (e && e->resolved)
            memcpy(e->mac, a->sha, ETH_ALEN);
        return;
    }
    if (spa)
        arp_learn(spa, a->sha);
    if (a->op == htons(ARP_REQUEST))
        arp_send(ARP_REPLY, a->sha, spa, a->sha);
}

/* Send an IP packet to the next hop, resolving its address first if need be. */
static int arp_output(uint32_t hop, const uint8_t *pkt, uint32_t len)
{
    struct arp_entry *e = arp_find(hop);
    if (e && e->resolved && (int32_t)(ticks - e->expires) < 0)
        return eth_send(e->mac, ETH_P_IP, pkt, len);

    if (!e || e->resolved) {
        e = arp_slot(hop);
        e->resolved = 0;
        e->tries = 0;
    }
    /* Keep the newest packet; TCP will retransmit anything older. */
    arp_drop_pending(e);
    e->pending = kmalloc(len);
    if (e->pending) {
        memcpy(e->pending, pkt, len);
        e->pending_len = len;
    }
    if (e->tries == 0) {
        e->tries = 1;
        e->expires = ticks + ARP_RETRY;
        arp_send(ARP_REQUEST, (const uint8_t[ETH_ALEN]){ 0 }, hop, broadcast_mac);
    }
    return 0;
}

static void arp_tick(void)
{
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &arp_table[i];
        if (!e->ip || (int32_t)(ticks - e->expires) < 0)
            continue;
        if (e->resolved || e->tries >= ARP_TRIES) {
            arp_drop_pending(e);
            memset(e, 0, sizeof(*e));
            continue;
        }
        e->tries++;
        e->expires = ticks + ARP_RETRY;
        arp_send(ARP_REQUEST, (const uint8_t[ETH_ALEN]){ 0 }, e->ip, broadcast_mac);
    }
}

/* ---- IP ---- */

static int is_loopback(uint32_t addr)
{
    return (addr >> 24) == 127;
}

int ip_is_local(uint32_t addr)
{
    return is_loopback(addr) || (netif.ip && addr == netif.ip);
}

static int is_broadcast(uint32_t addr)
{
    return addr == INADDR_BROADCAST ||
           (netif.ip && netif.mask && netif.mask != 0xFFFFFFFFu && (addr & ~netif.mask) == ~netif.mask &&
            (addr & netif.mask) == (netif.ip & netif.mask));
}

uint32_t ip_source_for(uint32_t dst)
{
    if (is_loopback(dst))
        return INADDR_LOOPBACK;
    return netif.ip;
}

/* Packets to ourselves wait here until the next timer tick, so that a
 * reply never runs inside the code that sent the request. */
static struct loop_pkt {
    uint8_t *data;
    uint32_t len;
} loop_queue[LOOP_QUEUE];
static uint32_t loop_head, loop_tail;

static void ip_input(const uint8_t *data, uint32_t len);

static void loop_send(const uint8_t *pkt, uint32_t len)
{
    if (loop_head - loop_tail == LOOP_QUEUE)
        return;
    uint8_t *copy = kmalloc(len);
    if (!copy)
        return;
    memcpy(copy, pkt, len);
    loop_queue[loop_head % LOOP_QUEUE].data = copy;
    loop_queue[loop_head % LOOP_QUEUE].len = len;
    loop_head++;
}

static void loop_tick(void)
{
    /* Only what was queued before this tick: replies wait for the next. */
    uint32_t end = loop_head;
    while (loop_tail != end) {
        struct loop_pkt p = loop_queue[loop_tail % LOOP_QUEUE];
        loop_tail++;
        ip_input(p.data, p.len);
        kfree(p.data);
    }
}

int ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *hdr, uint32_t hlen,
            const void *data, uint32_t dlen)
{
    uint8_t pkt[ETH_MTU];
    uint32_t len = sizeof(struct ip_hdr) + hlen + dlen;
    if (len > ETH_MTU)
        return -EMSGSIZE;

    struct ip_hdr *ip = (struct ip_hdr *)pkt;
    ip->vhl = 0x45;
    ip->tos = 0;
    ip->len = htons((uint16_t)len);
    ip->id = htons(ip_id++);
    ip->frag = htons(IP_DF);
    ip->ttl = IP_TTL;
    ip->proto = proto;
    ip->csum = 0;
    ip->src = htonl(src);
    ip->dst = htonl(dst);
    ip->csum = csum_fold(csum_add(0, ip, sizeof(*ip)));
    memcpy(pkt + sizeof(*ip), hdr, hlen);
    if (dlen)
        memcpy(pkt + sizeof(*ip) + hlen, data, dlen);

    if (ip_is_local(dst)) {
        loop_send(pkt, len);
        return 0;
    }
    if (!netif.present)
        return -ENETDOWN;
    if (dst == INADDR_BROADCAST || is_broadcast(dst))
        return eth_send(broadcast_mac, ETH_P_IP, pkt, len);
    if (!netif.ip)
        return -ENETUNREACH;
    uint32_t hop = (dst & netif.mask) == (netif.ip & netif.mask) ? dst : netif.gw;
    if (!hop)
        return -ENETUNREACH;
    return arp_output(hop, pkt, len);
}

static void icmp_input(const struct ip_hdr *ip, const uint8_t *data, uint32_t len)
{
    if (len < sizeof(struct icmp_hdr) || csum_fold(csum_add(0, data, len)) != 0)
        return;
    const struct icmp_hdr *ih = (const struct icmp_hdr *)data;
    uint32_t src = ntohl(ip->src), dst = ntohl(ip->dst);

    if (ih->type == ICMP_ECHO_REQUEST && !is_broadcast(dst)) {
        uint8_t reply[ETH_MTU];
        if (len > sizeof(reply) - sizeof(struct ip_hdr))
            return;
        memcpy(reply, data, len);
        struct icmp_hdr *rh = (struct icmp_hdr *)reply;
        rh->type = ICMP_ECHO_REPLY;
        rh->csum = 0;
        rh->csum = csum_fold(csum_add(0, reply, len));
        ip_send(dst, src, IPPROTO_ICMP, reply, len, NULL, 0);
    } else if (ih->type == ICMP_ECHO_REPLY) {
        icmp_socket_input(src, data, len);
    }
}

void icmp_send_unreachable(const struct ip_hdr *ip, uint32_t len, uint8_t code)
{
    uint8_t msg[sizeof(struct icmp_hdr) + sizeof(struct ip_hdr) + 8];
    uint32_t quote = MIN(len, (uint32_t)(sizeof(struct ip_hdr) + 8));
    struct icmp_hdr *ih = (struct icmp_hdr *)msg;
    memset(ih, 0, sizeof(*ih));
    ih->type = ICMP_UNREACH;
    ih->code = code;
    memcpy(msg + sizeof(*ih), ip, quote);
    ih->csum = csum_fold(csum_add(0, msg, sizeof(*ih) + quote));
    ip_send(ntohl(ip->dst), ntohl(ip->src), IPPROTO_ICMP, msg, sizeof(*ih) + quote, NULL, 0);
}

static void ip_input(const uint8_t *data, uint32_t len)
{
    if (len < sizeof(struct ip_hdr))
        return;
    const struct ip_hdr *ip = (const struct ip_hdr *)data;
    uint32_t hlen = (uint32_t)(ip->vhl & 0x0F) * 4, total = ntohs(ip->len);
    if ((ip->vhl >> 4) != 4 || hlen < sizeof(*ip) || total < hlen || total > len)
        return;
    if (csum_fold(csum_add(0, ip, hlen)) != 0)
        return;
    if (ntohs(ip->frag) & (IP_MF | IP_OFFMASK)) {
        netif.rx_dropped++;             /* fragments are not reassembled */
        return;
    }
    uint32_t dst = ntohl(ip->dst);
    int bcast = is_broadcast(dst);
    /* Before DHCP has given us an address, accept whatever comes. */
    if (netif.ip && !ip_is_local(dst) && !bcast)
        return;

    const uint8_t *payload = data + hlen;
    uint32_t plen = total - hlen;
    switch (ip->proto) {
    case IPPROTO_ICMP:
        icmp_input(ip, payload, plen);
        break;
    case IPPROTO_TCP:
        if (!bcast)
            tcp_input(ntohl(ip->src), dst, payload, plen);
        break;
    case IPPROTO_UDP:
        if (plen >= sizeof(struct udp_hdr)) {
            const struct udp_hdr *uh = (const struct udp_hdr *)payload;
            if (ntohs(uh->dport) == DHCP_CLIENT_PORT && netif.use_dhcp) {
                dhcp_input(payload, plen);
                break;
            }
            /* Nobody listening: tell the sender, unless it was a broadcast. */
            if (!udp_input(ntohl(ip->src), dst, payload, plen) && !bcast)
                icmp_send_unreachable(ip, total, ICMP_UNREACH_PORT);
        }
        break;
    default:
        break;
    }
}

void net_receive(const uint8_t *frame, uint32_t len)
{
    if (len < ETH_HLEN)
        return;
    netif.rx_packets++;
    netif.rx_bytes += len;
    const struct eth_hdr *eh = (const struct eth_hdr *)frame;
    uint16_t type = ntohs(eh->type);
    if (type == ETH_P_ARP)
        arp_input(frame + ETH_HLEN, len - ETH_HLEN);
    else if (type == ETH_P_IP)
        ip_input(frame + ETH_HLEN, len - ETH_HLEN);
}

/* ---- DHCP ---- */

struct dhcp_msg {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

#define OPT_PAD       0
#define OPT_MASK      1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_HOSTNAME  12
#define OPT_REQ_IP    50
#define OPT_LEASE     51
#define OPT_MSG_TYPE  53
#define OPT_SERVER_ID 54
#define OPT_PARAMS    55
#define OPT_END       255

enum dhcp_state { DHCP_OFF, DHCP_INIT, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING };

static struct {
    enum dhcp_state state;
    uint32_t xid;
    uint32_t offer, server;     /* the offered address and the server that made it */
    uint32_t timeout;           /* resend or renew at this tick */
    int tries;
    int logged_failure;
} dhcp;

static uint32_t dhcp_xid(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ hi ^ (uint32_t)netif.dev.mac[5] << 24 ^ ticks;
}

static uint8_t *opt_put(uint8_t *p, uint8_t code, const void *data, uint8_t len)
{
    *p++ = code;
    *p++ = len;
    memcpy(p, data, len);
    return p + len;
}

static void dhcp_send(uint8_t type)
{
    struct dhcp_msg m;
    memset(&m, 0, sizeof(m));
    m.op = 1;
    m.htype = 1;
    m.hlen = ETH_ALEN;
    m.xid = htonl(dhcp.xid);
    m.flags = htons(0x8000);            /* we cannot take unicasts yet: broadcast the reply */
    if (dhcp.state == DHCP_RENEWING)
        m.ciaddr = htonl(netif.ip);
    memcpy(m.chaddr, netif.dev.mac, ETH_ALEN);
    m.magic = htonl(DHCP_MAGIC);

    uint8_t *p = m.options;
    p = opt_put(p, OPT_MSG_TYPE, &type, 1);
    p = opt_put(p, OPT_HOSTNAME, "fluxOS", 6);
    if (type == DHCPREQUEST && dhcp.state == DHCP_REQUESTING) {
        uint32_t req = htonl(dhcp.offer), server = htonl(dhcp.server);
        p = opt_put(p, OPT_REQ_IP, &req, 4);
        p = opt_put(p, OPT_SERVER_ID, &server, 4);
    }
    static const uint8_t params[] = { OPT_MASK, OPT_ROUTER, OPT_DNS, OPT_LEASE, OPT_SERVER_ID };
    p = opt_put(p, OPT_PARAMS, params, sizeof(params));
    *p++ = OPT_END;
    uint32_t len = (uint32_t)(p - (uint8_t *)&m);
    if (len < 300)
        len = 300;                      /* some servers want a BOOTP-sized message */

    struct udp_hdr uh;
    uh.sport = htons(DHCP_CLIENT_PORT);
    uh.dport = htons(DHCP_SERVER_PORT);
    uh.len = htons((uint16_t)(sizeof(uh) + len));
    uh.csum = 0;
    uint32_t src = dhcp.state == DHCP_RENEWING ? netif.ip : 0;
    uint32_t sum = csum_pseudo(src, INADDR_BROADCAST, IPPROTO_UDP, sizeof(uh) + len);
    sum = csum_add(csum_add(sum, &uh, sizeof(uh)), &m, len);
    uh.csum = csum_fold(sum);
    if (!uh.csum)
        uh.csum = 0xFFFF;
    ip_send(src, INADDR_BROADCAST, IPPROTO_UDP, &uh, sizeof(uh), &m, len);
}

static void dhcp_start(void)
{
    dhcp.state = DHCP_SELECTING;
    dhcp.xid = dhcp_xid();
    dhcp.tries = 0;
    dhcp.timeout = ticks + 2 * HZ;
    dhcp_send(DHCPDISCOVER);
}

/* Find option `code`; returns its length, or -1. */
static int opt_get(const struct dhcp_msg *m, uint32_t len, uint8_t code, const uint8_t **out)
{
    const uint8_t *p = m->options, *end = (const uint8_t *)m + len;
    while (p < end && *p != OPT_END) {
        if (*p == OPT_PAD) {
            p++;
            continue;
        }
        if (p + 2 > end || p + 2 + p[1] > end)
            break;
        if (*p == code) {
            *out = p + 2;
            return p[1];
        }
        p += 2 + p[1];
    }
    return -1;
}

static uint32_t opt_ip(const struct dhcp_msg *m, uint32_t len, uint8_t code)
{
    const uint8_t *v;
    if (opt_get(m, len, code, &v) < 4)
        return 0;
    return (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
}

void dhcp_input(const uint8_t *data, uint32_t len)
{
    data += sizeof(struct udp_hdr);
    len -= sizeof(struct udp_hdr);
    const struct dhcp_msg *m = (const struct dhcp_msg *)data;
    uint32_t fixed = (uint32_t)((const uint8_t *)m->options - (const uint8_t *)m);
    if (len < fixed || m->op != 2 || ntohl(m->xid) != dhcp.xid || ntohl(m->magic) != DHCP_MAGIC ||
        memcmp(m->chaddr, netif.dev.mac, ETH_ALEN) != 0)
        return;
    const uint8_t *v;
    if (opt_get(m, len, OPT_MSG_TYPE, &v) < 1)
        return;
    uint8_t type = v[0];

    if (type == DHCPOFFER && dhcp.state == DHCP_SELECTING) {
        dhcp.offer = ntohl(m->yiaddr);
        dhcp.server = opt_ip(m, len, OPT_SERVER_ID);
        dhcp.state = DHCP_REQUESTING;
        dhcp.tries = 0;
        dhcp.timeout = ticks + 2 * HZ;
        dhcp_send(DHCPREQUEST);
    } else if (type == DHCPACK && (dhcp.state == DHCP_REQUESTING || dhcp.state == DHCP_RENEWING)) {
        uint32_t lease = opt_ip(m, len, OPT_LEASE);
        int renewed = dhcp.state == DHCP_RENEWING && netif.ip == ntohl(m->yiaddr);
        netif.ip = ntohl(m->yiaddr);
        netif.mask = opt_ip(m, len, OPT_MASK);
        if (!netif.mask)
            netif.mask = IP4(255, 255, 255, 0);
        netif.gw = opt_ip(m, len, OPT_ROUTER);
        netif.dns = opt_ip(m, len, OPT_DNS);
        if (!lease || lease > 7 * 86400)
            lease = 7 * 86400;
        netif.lease_end = ticks + lease * HZ;
        dhcp.state = DHCP_BOUND;
        dhcp.timeout = ticks + lease / 2 * HZ;     /* renew halfway through */
        dhcp.logged_failure = 0;
        if (!renewed)
            klog("net: DHCP gave us " IP_FMT "/" IP_FMT ", gateway " IP_FMT ", DNS " IP_FMT "\n",
                 IP_ARGS(netif.ip), IP_ARGS(netif.mask), IP_ARGS(netif.gw), IP_ARGS(netif.dns));
    } else if (type == DHCPNAK && dhcp.state != DHCP_SELECTING) {
        klog("net: DHCP server refused our address, starting over\n");
        netif.ip = 0;
        dhcp_start();
    }
}

static void dhcp_tick(void)
{
    if (dhcp.state == DHCP_OFF || (int32_t)(ticks - dhcp.timeout) < 0)
        return;
    switch (dhcp.state) {
    case DHCP_INIT:
        dhcp_start();
        break;
    case DHCP_SELECTING:
    case DHCP_REQUESTING:
        /* Back off 2, 4, 8, 16 seconds, then start over. */
        if (++dhcp.tries >= 4) {
            if (!dhcp.logged_failure) {
                klog("net: no answer from a DHCP server, still trying\n");
                dhcp.logged_failure = 1;
            }
            dhcp_start();
            break;
        }
        dhcp.timeout = ticks + (2u << dhcp.tries) * HZ;
        dhcp_send(dhcp.state == DHCP_SELECTING ? DHCPDISCOVER : DHCPREQUEST);
        break;
    case DHCP_BOUND:
        dhcp.state = DHCP_RENEWING;
        dhcp.xid = dhcp_xid();
        dhcp.timeout = ticks + 10 * HZ;
        dhcp_send(DHCPREQUEST);
        break;
    case DHCP_RENEWING:
        if ((int32_t)(ticks - netif.lease_end) >= 0) {
            klog("net: DHCP lease on " IP_FMT " ran out\n", IP_ARGS(netif.ip));
            netif.ip = 0;
            dhcp_start();
        } else {
            dhcp.timeout = ticks + 10 * HZ;
            dhcp_send(DHCPREQUEST);
        }
        break;
    case DHCP_OFF:
        break;
    }
}

/* ---- configuration and setup ---- */

static int parse_ip(const char *s, uint32_t *ip, int *prefix)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9')
            return -1;
        uint32_t n = 0;
        while (*s >= '0' && *s <= '9')
            n = n * 10 + (uint32_t)(*s++ - '0');
        if (n > 255 || (part < 3 && *s++ != '.'))
            return -1;
        v = v << 8 | n;
    }
    if (prefix && *s == '/') {
        int p = 0;
        for (s++; *s >= '0' && *s <= '9'; s++)
            p = p * 10 + (*s - '0');
        *prefix = p > 32 ? 32 : p;
    }
    *ip = v;
    return 0;
}

static uint32_t prefix_mask(int prefix)
{
    return prefix <= 0 ? 0 : 0xFFFFFFFFu << (32 - prefix);
}

void net_register(const struct netdev *dev)
{
    netif.dev = *dev;
    netif.present = 1;
}

void net_init(void)
{
    if (forcedeth_init() < 0 && ne2000_init() < 0) {
        boot_ok("Network: no network card found (loopback only)");
        return;
    }
    char opt[32];
    int prefix = 24;
    uint32_t ip;
    if (cmdline_value("net.ip", opt, sizeof(opt)) && parse_ip(opt, &ip, &prefix) == 0) {
        netif.ip = ip;
        netif.mask = prefix_mask(prefix);
        if (cmdline_value("net.gw", opt, sizeof(opt)))
            parse_ip(opt, &netif.gw, NULL);
        if (cmdline_value("net.dns", opt, sizeof(opt)))
            parse_ip(opt, &netif.dns, NULL);
        boot_ok("Network: address " IP_FMT "/%d (from the kernel command line)", IP_ARGS(netif.ip), prefix);
        return;
    }
    /* DHCP starts on the first timer tick, once interrupts are running. */
    netif.use_dhcp = 1;
    dhcp.state = DHCP_INIT;
    dhcp.timeout = ticks;
}

int net_configure(const struct netinfo *ni)
{
    if (!netif.present)
        return -ENODEV;
    spin_lock(&net_lock);
    if (ni->flags & NETF_DHCP) {
        netif.use_dhcp = 1;
        netif.ip = 0;
        dhcp.state = DHCP_INIT;
        dhcp.timeout = ticks;
        spin_unlock(&net_lock);
        return 0;
    }
    netif.use_dhcp = 0;
    dhcp.state = DHCP_OFF;
    netif.ip = ntohl(ni->addr);
    netif.mask = ntohl(ni->netmask);
    netif.gw = ntohl(ni->gateway);
    netif.dns = ntohl(ni->dns);
    netif.lease_end = 0;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        arp_drop_pending(&arp_table[i]);
        memset(&arp_table[i], 0, sizeof(arp_table[i]));
    }
    spin_unlock(&net_lock);
    return 0;
}

void net_get_info(struct netinfo *ni)
{
    spin_lock(&net_lock);
    memset(ni, 0, sizeof(*ni));
    strlcpy(ni->name, !netif.present ? "lo" : netif.dev.name[0] ? netif.dev.name : "ne0",
            sizeof(ni->name));
    strlcpy(ni->driver, netif.present ? netif.dev.driver : "loopback", sizeof(ni->driver));
    memcpy(ni->mac, netif.dev.mac, ETH_ALEN);
    ni->flags = (uint16_t)((netif.present ? NETF_UP : 0) | (netif.use_dhcp ? NETF_DHCP : 0) |
                           (netif.ip ? NETF_BOUND : 0));
    ni->addr = htonl(netif.ip);
    ni->netmask = htonl(netif.mask);
    ni->gateway = htonl(netif.gw);
    ni->dns = htonl(netif.dns);
    ni->io_base = netif.dev.io_base;
    ni->irq = netif.dev.irq;
    ni->rx_packets = netif.rx_packets;
    ni->tx_packets = netif.tx_packets;
    ni->rx_bytes = netif.rx_bytes;
    ni->tx_bytes = netif.tx_bytes;
    ni->rx_errors = netif.rx_errors;
    ni->tx_errors = netif.tx_errors;
    ni->rx_dropped = netif.rx_dropped;
    if (netif.use_dhcp && netif.ip && (int32_t)(netif.lease_end - ticks) > 0)
        ni->lease_secs = (netif.lease_end - ticks) / HZ;
    spin_unlock(&net_lock);
}

/* From the timer interrupt. A processor busy in the network stack has it
 * for now: the tick's work waits for the next one. */
void net_tick(void)
{
    static uint32_t last_slow;
    if (!spin_trylock(&net_lock))
        return;
    loop_tick();
    tcp_timer();
    if ((uint32_t)(ticks - last_slow) >= HZ / 10 && netif.present) {
        last_slow = ticks;
        if (netif.dev.poll)
            netif.dev.poll();
        arp_tick();
        dhcp_tick();
    }
    spin_unlock(&net_lock);
}
