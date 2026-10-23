/*
 * IPv4 address conversion and a host name resolver: /etc/hosts first,
 * then a DNS query (type A, recursion desired) to the server that DHCP
 * gave us, over UDP with a 2-second timeout and three tries.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DNS_PORT     53
#define DNS_TIMEOUT  2000
#define DNS_TRIES    3
#define MAX_ADDRS    8
#define HOSTS_FILE   "/etc/hosts"

int h_errno;

/* ---- address conversion ---- */

int inet_aton(const char *s, struct in_addr *addr)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit((unsigned char)*s))
            return 0;
        uint32_t n = 0;
        while (isdigit((unsigned char)*s)) {
            n = n * 10 + (uint32_t)(*s++ - '0');
            if (n > 255)
                return 0;
        }
        if (part < 3 && *s++ != '.')
            return 0;
        v = v << 8 | n;
    }
    if (*s)
        return 0;
    addr->s_addr = htonl(v);
    return 1;
}

in_addr_t inet_addr(const char *s)
{
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    uint32_t v = ntohl(((const struct in_addr *)src)->s_addr);
    if ((socklen_t)snprintf(dst, size, "%u.%u.%u.%u", v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF,
                            v & 0xFF) >= size) {
        errno = ENOSPC;
        return NULL;
    }
    return dst;
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_aton(src, dst);
}

char *inet_ntoa(struct in_addr addr)
{
    static char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return buf;
}

/* ---- the result ---- */

static struct {
    struct hostent h;
    char name[256];
    char *aliases[1];
    struct in_addr addrs[MAX_ADDRS];
    char *list[MAX_ADDRS + 1];
} result;

static struct hostent *make_result(const char *name, const struct in_addr *addrs, int n)
{
    snprintf(result.name, sizeof(result.name), "%s", name);
    result.aliases[0] = NULL;
    for (int i = 0; i < n; i++) {
        result.addrs[i] = addrs[i];
        result.list[i] = (char *)&result.addrs[i];
    }
    result.list[n] = NULL;
    result.h.h_name = result.name;
    result.h.h_aliases = result.aliases;
    result.h.h_addrtype = AF_INET;
    result.h.h_length = 4;
    result.h.h_addr_list = result.list;
    return &result.h;
}

/* ---- /etc/hosts ---- */

static int hosts_lookup(const char *name, struct in_addr *addr)
{
    FILE *f = fopen(HOSTS_FILE, "r");
    if (!f)
        return 0;
    char line[256];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *save, *tok = strtok_r(line, " \t\r\n", &save);
        if (!tok || !inet_aton(tok, addr))
            continue;
        while ((tok = strtok_r(NULL, " \t\r\n", &save)))
            if (!strcasecmp(tok, name))
                found = 1;
    }
    fclose(f);
    return found;
}

/* ---- DNS ---- */

static int put_name(uint8_t *p, int room, const char *name)
{
    int n = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        int len = dot ? (int)(dot - name) : (int)strlen(name);
        if (len == 0 || len > 63 || n + len + 2 > room)
            return -1;
        p[n++] = (uint8_t)len;
        memcpy(p + n, name, (size_t)len);
        n += len;
        name += len;
        if (*name == '.')
            name++;
    }
    if (n + 1 > room)
        return -1;
    p[n++] = 0;
    return n;
}

/* Step over a (possibly compressed) name; returns the offset after it, or -1. */
static int skip_name(const uint8_t *msg, int len, int off)
{
    while (off < len) {
        uint8_t c = msg[off];
        if (c == 0)
            return off + 1;
        if ((c & 0xC0) == 0xC0)
            return off + 2 <= len ? off + 2 : -1;
        off += 1 + c;
    }
    return -1;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

/* Returns the number of addresses found, 0 if the name does not exist, -1 on failure. */
static int parse_reply(const uint8_t *msg, int len, uint16_t id, struct in_addr *addrs)
{
    if (len < 12 || be16(msg) != id || !(msg[2] & 0x80))
        return -1;
    int rcode = msg[3] & 0x0F;
    if (rcode == 3)
        return 0;                       /* NXDOMAIN */
    if (rcode != 0)
        return -1;
    int qd = be16(msg + 4), an = be16(msg + 6), off = 12, n = 0;
    for (int i = 0; i < qd; i++) {
        if ((off = skip_name(msg, len, off)) < 0 || off + 4 > len)
            return -1;
        off += 4;
    }
    for (int i = 0; i < an && n < MAX_ADDRS; i++) {
        if ((off = skip_name(msg, len, off)) < 0 || off + 10 > len)
            return -1;
        uint16_t type = be16(msg + off), cls = be16(msg + off + 2), rdlen = be16(msg + off + 8);
        off += 10;
        if (off + rdlen > len)
            return -1;
        /* CNAME records come first; the A records that follow are for the alias. */
        if (type == 1 && cls == 1 && rdlen == 4)
            memcpy(&addrs[n++], msg + off, 4);
        off += rdlen;
    }
    return n;
}

static int dns_lookup(const char *name, struct in_addr *addrs)
{
    struct netinfo ni;
    if (netinfo(&ni) < 0 || !ni.dns) {
        h_errno = NO_RECOVERY;
        return -1;
    }
    uint8_t q[512];
    uint16_t id = (uint16_t)(uptime_ms() * 2654435761u >> 16 ^ (unsigned)getpid());
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01;                        /* recursion desired */
    q[5] = 1;                           /* one question */
    int n = put_name(q + 12, (int)sizeof(q) - 16, name);
    if (n < 0) {
        h_errno = NO_RECOVERY;
        return -1;
    }
    n += 12;
    q[n++] = 0;
    q[n++] = 1;                         /* type A */
    q[n++] = 0;
    q[n++] = 1;                         /* class IN */

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        h_errno = NO_RECOVERY;
        return -1;
    }
    struct sockaddr_in server = { .sin_family = AF_INET, .sin_port = htons(DNS_PORT) };
    server.sin_addr.s_addr = ni.dns;
    int found = -1;
    h_errno = TRY_AGAIN;
    if (connect(fd, (struct sockaddr *)&server, sizeof(server)) == 0) {
        for (int tries = 0; tries < DNS_TRIES && found < 0; tries++) {
            if (send(fd, q, (size_t)n, 0) < 0)
                break;
            unsigned int deadline = uptime_ms() + DNS_TIMEOUT;
            for (;;) {
                int left = (int)(deadline - uptime_ms());
                struct pollfd pfd = { fd, POLLIN, 0 };
                if (left <= 0 || poll(&pfd, 1, left) <= 0)
                    break;
                uint8_t reply[1024];
                int r = (int)recv(fd, reply, sizeof(reply), 0);
                if (r < 0)
                    break;
                found = parse_reply(reply, r, id, addrs);
                if (found >= 0)
                    break;              /* a stray or broken reply: keep waiting */
            }
        }
    }
    close(fd);
    if (found == 0)
        h_errno = HOST_NOT_FOUND;
    return found > 0 ? found : -1;
}

struct hostent *gethostbyname(const char *name)
{
    struct in_addr addrs[MAX_ADDRS];
    if (inet_aton(name, &addrs[0]) || hosts_lookup(name, &addrs[0]))
        return make_result(name, addrs, 1);
    int n = dns_lookup(name, addrs);
    return n > 0 ? make_result(name, addrs, n) : NULL;
}

const char *hstrerror(int err)
{
    switch (err) {
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN: return "Host name lookup failure (no answer from the DNS server)";
    case NO_RECOVERY: return "No DNS server is configured";
    case NO_DATA: return "No address associated with name";
    default: return "Unknown resolver error";
    }
}
