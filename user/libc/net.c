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

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT, in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

/* ---- getaddrinfo and friends ---- */

static const struct {
    const char *name;
    int port;
    const char *proto;
} services[] = {
    { "echo", 7, "tcp" }, { "discard", 9, "tcp" }, { "daytime", 13, "tcp" }, { "ftp-data", 20, "tcp" },
    { "ftp", 21, "tcp" }, { "ssh", 22, "tcp" }, { "telnet", 23, "tcp" }, { "smtp", 25, "tcp" },
    { "time", 37, "tcp" }, { "domain", 53, "udp" }, { "domain", 53, "tcp" }, { "bootps", 67, "udp" },
    { "bootpc", 68, "udp" }, { "tftp", 69, "udp" }, { "gopher", 70, "tcp" }, { "finger", 79, "tcp" },
    { "http", 80, "tcp" }, { "www", 80, "tcp" }, { "pop3", 110, "tcp" }, { "sunrpc", 111, "tcp" },
    { "nntp", 119, "tcp" }, { "ntp", 123, "udp" }, { "imap", 143, "tcp" }, { "snmp", 161, "udp" },
    { "ldap", 389, "tcp" }, { "https", 443, "tcp" }, { "submission", 587, "tcp" }, { "imaps", 993, "tcp" },
    { "pop3s", 995, "tcp" }, { "mysql", 3306, "tcp" }, { "postgresql", 5432, "tcp" }, { "http-alt", 8080, "tcp" },
};

struct servent *getservbyname(const char *name, const char *proto)
{
    static struct servent se;
    static char *no_aliases[1];
    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
        if (strcmp(services[i].name, name) || (proto && strcmp(services[i].proto, proto)))
            continue;
        se.s_name = (char *)services[i].name;
        se.s_aliases = no_aliases;
        se.s_port = htons((uint16_t)services[i].port);
        se.s_proto = (char *)services[i].proto;
        return &se;
    }
    return NULL;
}

struct servent *getservbyport(int port, const char *proto)
{
    static struct servent se;
    static char *no_aliases[1];
    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
        if (htons((uint16_t)services[i].port) != port || (proto && strcmp(services[i].proto, proto)))
            continue;
        se.s_name = (char *)services[i].name;
        se.s_aliases = no_aliases;
        se.s_port = port;
        se.s_proto = (char *)services[i].proto;
        return &se;
    }
    return NULL;
}

static struct addrinfo *ai_new(const struct in_addr *addr, uint16_t port, int socktype, int protocol,
                               const char *canon)
{
    struct addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
    if (!ai)
        return NULL;
    struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr = *addr;
    ai->ai_family = AF_INET;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = sizeof(*sin);
    ai->ai_addr = (struct sockaddr *)sin;
    ai->ai_canonname = canon ? strdup(canon) : NULL;
    return ai;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    int flags = hints ? hints->ai_flags : 0;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    *res = NULL;
    if (!node && !service)
        return EAI_NONAME;
    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY;
    if (socktype && socktype != SOCK_STREAM && socktype != SOCK_DGRAM)
        return EAI_SOCKTYPE;

    uint16_t port = 0;
    if (service) {
        char *end;
        long p = strtol(service, &end, 10);
        if (*service && !*end) {
            if (p < 0 || p > 65535)
                return EAI_SERVICE;
            port = (uint16_t)p;
        } else if (flags & AI_NUMERICSERV) {
            return EAI_NONAME;
        } else {
            struct servent *se = getservbyname(service, socktype == SOCK_DGRAM ? "udp" : socktype == SOCK_STREAM ? "tcp" : NULL);
            if (!se)
                return EAI_SERVICE;
            port = ntohs((uint16_t)se->s_port);
        }
    }

    struct in_addr addrs[8];
    int n = 0;
    const char *canon = NULL;
    if (!node) {
        addrs[0].s_addr = flags & AI_PASSIVE ? INADDR_ANY : htonl(INADDR_LOOPBACK);
        n = 1;
    } else if (inet_aton(node, &addrs[0])) {
        n = 1;
    } else if (flags & AI_NUMERICHOST) {
        return EAI_NONAME;
    } else {
        struct hostent *he = gethostbyname(node);
        if (!he)
            return h_errno == TRY_AGAIN ? EAI_AGAIN : h_errno == NO_RECOVERY ? EAI_FAIL : EAI_NONAME;
        for (; he->h_addr_list[n] && n < 8; n++)
            addrs[n] = *(struct in_addr *)he->h_addr_list[n];
        canon = flags & AI_CANONNAME ? he->h_name : NULL;
    }

    struct addrinfo *head = NULL, **tail = &head;
    for (int i = 0; i < n; i++) {
        int types[2] = { SOCK_STREAM, SOCK_DGRAM };
        for (int t = 0; t < 2; t++) {
            if (socktype && socktype != types[t])
                continue;
            int proto = types[t] == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
            if (protocol && protocol != proto)
                continue;
            struct addrinfo *ai = ai_new(&addrs[i], port, types[t], proto, head ? NULL : canon);
            if (!ai) {
                freeaddrinfo(head);
                return EAI_MEMORY;
            }
            *tail = ai;
            tail = &ai->ai_next;
        }
    }
    if (!head)
        return EAI_SOCKTYPE;
    *res = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int err)
{
    switch (err) {
    case 0: return "Success";
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE: return "Servname not supported for ai_socktype";
    case EAI_MEMORY: return "Memory allocation failure";
    case EAI_SYSTEM: return "System error";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    }
    return "Unknown error";
}

int getnameinfo(const struct sockaddr *addr, socklen_t addrlen, char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags)
{
    if (!addr || addrlen < sizeof(struct sockaddr_in) || addr->sa_family != AF_INET)
        return EAI_FAMILY;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    if (host && hostlen) {
        if (flags & NI_NAMEREQD)
            return EAI_NONAME;          /* no reverse lookups */
        if (!inet_ntop(AF_INET, &sin->sin_addr, host, hostlen))
            return EAI_OVERFLOW;
    }
    if (serv && servlen) {
        struct servent *se = flags & NI_NUMERICSERV ? NULL : getservbyport(sin->sin_port, flags & NI_DGRAM ? "udp" : "tcp");
        int n = se ? snprintf(serv, servlen, "%s", se->s_name) : snprintf(serv, servlen, "%u", ntohs(sin->sin_port));
        if (n < 0 || (socklen_t)n >= servlen)
            return EAI_OVERFLOW;
    }
    return 0;
}
