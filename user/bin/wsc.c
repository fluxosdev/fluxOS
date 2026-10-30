/* A WebSocket transport worker. Browser IPC is type + little-endian u32 length + bytes.
 * I: origin\nprotocols\ncookies; T/B: send text/binary; C: close (network code + UTF-8 reason).
 * Output O/T/B/C/E is open/text/binary/close/error. A carries read-credit acknowledgements.
 * Keeping DNS, TLS and socket waits here leaves the desktop's event loop responsive. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <tls.h>
#include <unistd.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#define MAX_MESSAGE (4u << 20)
static int sock = -1, closing;
static struct tls *secure;
static char selected[1024];
static int full_write(int fd, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n) {
        int r = (int)write(fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}
static void frame(char kind, const void *data, size_t n)
{
    unsigned char h[5] = {(unsigned char)kind, n, n >> 8, n >> 16, n >> 24};
    if (full_write(1, h, 5) < 0 || (n && full_write(1, data, n) < 0))
        _exit(0);
}
static void error(const char *message)
{
    frame('E', message, strlen(message));
    unsigned char code[] = {3, 238};
    frame('C', code, 2);
}
static int random_bytes(unsigned char *p, int n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    int got = 0;
    while (got < n) {
        int r = (int)read(fd, p + got, (size_t)(n - got));
        if (r <= 0)
            break;
        got += r;
    }
    close(fd);
    return got == n ? 0 : -1;
}
static int net_write(const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n) {
        int r = secure ? tls_write(secure, p, (int)(n > 16384 ? 16384 : n)) : (int)send(sock, p, n, 0);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}
static int net_read(void *data, int n)
{
    if (secure)
        return tls_read(secure, data, n);
    return (int)recv(sock, data, (size_t)n, 0);
}
static int wait_data(int ms)
{
    if (secure && tls_pending(secure))
        return 1;
    struct pollfd p = {sock, POLLIN, 0};
    return poll(&p, 1, ms) > 0;
}
static int send_frame(int opcode, const unsigned char *p, size_t n)
{
    unsigned char h[14], mask[4], buf[4096];
    if (random_bytes(mask, 4) < 0)
        return -1;
    int hn = 2;
    h[0] = (unsigned char)(0x80 | opcode);
    if (n < 126)
        h[1] = (unsigned char)(0x80 | n);
    else if (n <= 65535) {
        h[1] = 0xFE;
        h[2] = n >> 8;
        h[3] = n;
        hn = 4;
    } else {
        h[1] = 0xFF;
        memset(h + 2, 0, 4);
        h[6] = n >> 24;
        h[7] = n >> 16;
        h[8] = n >> 8;
        h[9] = n;
        hn = 10;
    }
    memcpy(h + hn, mask, 4);
    hn += 4;
    if (net_write(h, (size_t)hn) < 0)
        return -1;
    for (size_t off = 0; off < n;) {
        size_t take = n - off;
        if (take > sizeof(buf))
            take = sizeof(buf);
        for (size_t i = 0; i < take; i++)
            buf[i] = p[off + i] ^ mask[(off + i) & 3];
        if (net_write(buf, take) < 0)
            return -1;
        off += take;
    }
    return 0;
}
static int utf8_valid(const unsigned char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = p[i++];
        int more;
        if (c < 128)
            continue;
        if (c >= 0xC2 && c <= 0xDF)
            more = 1;
        else if (c >= 0xE0 && c <= 0xEF)
            more = 2;
        else if (c >= 0xF0 && c <= 0xF4)
            more = 3;
        else
            return 0;
        for (int k = 0; k < more; k++) {
            if (i >= n || (p[i] & 0xC0) != 0x80 ||
                (k == 0 && ((c == 0xE0 && p[i] < 0xA0) || (c == 0xED && p[i] >= 0xA0) ||
                            (c == 0xF0 && p[i] < 0x90) || (c == 0xF4 && p[i] >= 0x90))))
                return 0;
            i++;
        }
    }
    return 1;
}
static int header(const char *head, const char *name, char *out, size_t size)
{
    size_t nn = strlen(name);
    int found = 0;
    out[0] = 0;
    for (const char *p = head; *p;) {
        const char *e = strchr(p, '\n');
        if (!e)
            e = p + strlen(p);
        const char *next = *e ? e + 1 : e;
        if (e == p || (e == p + 1 && p[0] == '\r'))
            break;
        if ((size_t)(e - p) > nn && !strncasecmp(p, name, nn) && p[nn] == ':') {
            if (found++)
                return -1;
            const char *v = p + nn + 1;
            while (v < e && (*v == ' ' || *v == '\t'))
                v++;
            while (e > v && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
                e--;
            if ((size_t)(e - v) >= size)
                return -1;
            memcpy(out, v, (size_t)(e - v));
            out[e - v] = 0;
        }
        p = next;
    }
    return found;
}
static int token(const char *list, const char *word, int insensitive)
{
    size_t n = strlen(word);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        const char *e = p + strcspn(p, ",");
        const char *z = e;
        while (z > p && (z[-1] == ' ' || z[-1] == '\t'))
            z--;
        if ((size_t)(z - p) == n && !(insensitive ? strncasecmp(p, word, n) : strncmp(p, word, n)))
            return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}
static int handshake(const char *url, char *config)
{
    char *protocols = strchr(config, '\n');
    if (!protocols)
        return -1;
    *protocols++ = 0;
    char *cookies = strchr(protocols, '\n');
    if (!cookies)
        return -1;
    *cookies++ = 0;
    if (strpbrk(config, "\r\n") || strpbrk(protocols, "\r\n") || strpbrk(cookies, "\r\n"))
        return -1;
    if (strlen(url) >= 4090)
        return -1;
    int https = !strncmp(url, "wss://", 6);
    const char *p = url + (https ? 6 : 5);
    if (!https && strncmp(url, "ws://", 5))
        return -1;
    for (const unsigned char *q = (const unsigned char *)url; *q; q++)
        if (*q <= 32 || *q == 127 || *q == '\\' || *q == '#')
            return -1;
    const char *end = p + strcspn(p, "/?");
    const char *colon = memchr(p, ':', (size_t)(end - p));
    const char *he = colon ? colon : end;
    if (he == p || he - p > 255 || memchr(p, '@', (size_t)(end - p)))
        return -1;
    char host[256], authority[280], path[4096];
    snprintf(host, sizeof(host), "%.*s", (int)(he - p), p);
    unsigned port = https ? 443 : 80;
    if (colon) {
        port = 0;
        for (const char *q = colon + 1; q < end; q++) {
            if (*q < '0' || *q > '9')
                return -1;
            port = port * 10 + *q - '0';
            if (port > 65535)
                return -1;
        }
        if (!port)
            return -1;
    }
    snprintf(authority, sizeof(authority), "%.*s", (int)(end - p), p);
    snprintf(path, sizeof(path), "%s%s", *end == '/' ? "" : "/", end);
    struct hostent *h = gethostbyname(host);
    if (!h)
        return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    memcpy(&sa.sin_addr, h->h_addr, 4);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 || connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        return -1;
    if (https) {
        char why[256];
        secure = tls_connect(sock, host, 10000, why, sizeof(why));
        if (!secure) {
            frame('E', why, strlen(why));
            return -1;
        }
    }
    unsigned char nonce[16], digest[20], key[32], accept[40];
    size_t kn, an;
    if (random_bytes(nonce, 16) < 0 || mbedtls_base64_encode(key, sizeof(key), &kn, nonce, 16))
        return -1;
    char proof[80];
    snprintf(proof, sizeof(proof), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    if (mbedtls_sha1((const unsigned char *)proof, strlen(proof), digest) ||
        mbedtls_base64_encode(accept, sizeof(accept), &an, digest, 20))
        return -1;
    char request[16384];
    int n = snprintf(
        request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: "
        "13\r\nSec-WebSocket-Key: %s\r\nOrigin: %s\r\n%s%s%s%s%s%s\r\n",
        path, authority, key, config, *protocols ? "Sec-WebSocket-Protocol: " : "", protocols,
        *protocols ? "\r\n" : "", *cookies ? "Cookie: " : "", cookies, *cookies ? "\r\n" : "");
    if (n < 0 || n >= (int)sizeof(request) || net_write(request, (size_t)n) < 0)
        return -1;
    char response[16384];
    int got = 0;
    while (got < (int)sizeof(response) - 1) {
        if (!wait_data(10000) || net_read(response + got, 1) != 1)
            return -1;
        got++;
        if (got >= 4 && !memcmp(response + got - 4, "\r\n\r\n", 4))
            break;
    }
    response[got] = 0;
    if (got < 4 || memcmp(response + got - 4, "\r\n\r\n", 4) || strncmp(response, "HTTP/1.1 101 ", 13))
        return -1;
    char value[2048];
    if (header(response, "Upgrade", value, sizeof(value)) != 1 || strcasecmp(value, "websocket"))
        return -1;
    if (header(response, "Connection", value, sizeof(value)) != 1 || !token(value, "Upgrade", 1))
        return -1;
    if (header(response, "Sec-WebSocket-Accept", value, sizeof(value)) != 1 ||
        strcmp(value, (const char *)accept))
        return -1;
    if (header(response, "Sec-WebSocket-Extensions", value, sizeof(value)) != 0)
        return -1;
    int hp = header(response, "Sec-WebSocket-Protocol", selected, sizeof(selected));
    if (hp < 0 || (hp == 1 && (!*selected || strchr(selected, ',') || !token(protocols, selected, 0))))
        return -1;
    frame('O', selected, strlen(selected));
    return 0;
}
static int valid_close(unsigned code)
{
    return code >= 1000 && code < 5000 && code != 1004 && code != 1005 && code != 1006 && code != 1015;
}
static int protocol_failure(unsigned code, const char *why)
{
    unsigned char close[] = {code >> 8, code};
    send_frame(8, close, 2);
    frame('E', why, strlen(why));
    frame('C', close, 2);
    return -1;
}
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    unsigned char *input = NULL, *wire = NULL, *message = NULL;
    size_t ilen = 0, wlen = 0, mlen = 0;
    int initialized = 0, message_op = 0, result = 1;
    input = malloc(MAX_MESSAGE + 5);
    wire = malloc(MAX_MESSAGE + 14);
    message = malloc(MAX_MESSAGE);
    if (!input || !wire || !message)
        goto finish;
    for (;;) {
        struct pollfd fds[2] = {{0, POLLIN, 0}, {sock, POLLIN, 0}};
        int ready = initialized && secure && tls_pending(secure)
                        ? 1
                        : poll(fds, initialized ? 2 : 1, closing ? 5000 : -1);
        if (ready <= 0) {
            if (closing) {
                unsigned char c[] = {3, 238};
                frame('C', c, 2);
                result = 0;
            }
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            int n = (int)read(0, input + ilen, MAX_MESSAGE + 5 - ilen);
            if (n <= 0) {
                result = 0;
                break;
            }
            ilen += (size_t)n;
            unsigned char credit[4] = {n, n >> 8, n >> 16, n >> 24};
            frame('A', credit, 4);
            while (ilen >= 5) {
                size_t len = (size_t)input[1] | (size_t)input[2] << 8 | (size_t)input[3] << 16 |
                             (size_t)input[4] << 24;
                if (len > MAX_MESSAGE)
                    goto finish;
                if (ilen < len + 5)
                    break;
                unsigned char type = input[0], *body = input + 5;
                if (!initialized) {
                    if (type != 'I' || len > 8192)
                        goto finish;
                    body[len] = 0;
                    if (handshake(argv[1], (char *)body) < 0)
                        goto finish;
                    initialized = 1;
                } else if (type == 'T' || type == 'B') {
                    if (closing)
                        goto finish;
                    if (type == 'T' && !utf8_valid(body, len))
                        goto finish;
                    if (send_frame(type == 'T' ? 1 : 2, body, len) < 0)
                        goto finish;
                } else if (type == 'C') {
                    if (len > 125 || len == 1 ||
                        (len >= 2 &&
                         (!valid_close((unsigned)body[0] << 8 | body[1]) || !utf8_valid(body + 2, len - 2))))
                        goto finish;
                    if (!closing && send_frame(8, body, len) < 0)
                        goto finish;
                    closing = 1;
                } else
                    goto finish;
                memmove(input, input + 5 + len, ilen - 5 - len);
                ilen -= 5 + len;
            }
        }
        if (initialized &&
            ((secure && tls_pending(secure)) || (fds[1].revents & (POLLIN | POLLHUP | POLLERR)))) {
            int n = net_read(wire + wlen, (int)(MAX_MESSAGE + 14 - wlen));
            if (n <= 0)
                break;
            wlen += (size_t)n;
            while (wlen >= 2) {
                int fin = !!(wire[0] & 0x80), op = wire[0] & 15;
                size_t len = wire[1] & 127, head = 2;
                if ((wire[0] & 0x70) || (wire[1] & 0x80)) {
                    protocol_failure(1002, "invalid WebSocket frame flags");
                    result = 0;
                    goto finish;
                }
                if (len == 126) {
                    if (wlen < 4)
                        break;
                    len = (size_t)wire[2] << 8 | wire[3];
                    head = 4;
                    if (len < 126) {
                        protocol_failure(1002, "non-canonical length");
                        result = 0;
                        goto finish;
                    }
                } else if (len == 127) {
                    if (wlen < 10)
                        break;
                    if (wire[2] || wire[3] || wire[4] || wire[5]) {
                        protocol_failure(1009, "message too large");
                        result = 0;
                        goto finish;
                    }
                    len = (size_t)wire[6] << 24 | (size_t)wire[7] << 16 | (size_t)wire[8] << 8 | wire[9];
                    head = 10;
                    if (len < 65536) {
                        protocol_failure(1002, "non-canonical length");
                        result = 0;
                        goto finish;
                    }
                }
                if (len > MAX_MESSAGE || (op >= 8 && (!fin || len > 125))) {
                    protocol_failure(len > MAX_MESSAGE ? 1009 : 1002, "invalid frame length");
                    result = 0;
                    goto finish;
                }
                if (wlen < head + len)
                    break;
                unsigned char *body = wire + head;
                if (op == 8) {
                    if (len == 1 || (len >= 2 && (!valid_close((unsigned)body[0] << 8 | body[1]) ||
                                                  !utf8_valid(body + 2, len - 2)))) {
                        protocol_failure(1002, "invalid close frame");
                        result = 0;
                        goto finish;
                    }
                    if (!closing)
                        send_frame(8, body, len);
                    if (len)
                        frame('C', body, len);
                    else {
                        unsigned char c[] = {3, 237};
                        frame('C', c, 2);
                    }
                    result = 0;
                    goto finish;
                } else if (op == 9) {
                    if (send_frame(10, body, len) < 0)
                        goto finish;
                } else if (op == 10) {
                } else if (op == 0 || op == 1 || op == 2) {
                    if ((op == 0 && !message_op) || (op != 0 && message_op)) {
                        protocol_failure(1002, "invalid message fragmentation");
                        result = 0;
                        goto finish;
                    }
                    if (op)
                        message_op = op;
                    if (mlen + len > MAX_MESSAGE) {
                        protocol_failure(1009, "message too large");
                        result = 0;
                        goto finish;
                    }
                    memcpy(message + mlen, body, len);
                    mlen += len;
                    if (fin) {
                        if (message_op == 1 && !utf8_valid(message, mlen)) {
                            protocol_failure(1007, "invalid UTF-8 message");
                            result = 0;
                            goto finish;
                        }
                        frame(message_op == 1 ? 'T' : 'B', message, mlen);
                        message_op = 0;
                        mlen = 0;
                    }
                } else {
                    protocol_failure(1002, "unknown WebSocket opcode");
                    result = 0;
                    goto finish;
                }
                memmove(wire, wire + head + len, wlen - head - len);
                wlen -= head + len;
            }
        }
    }
finish:
    if (result)
        error("WebSocket connection failed");
    if (secure)
        tls_close(secure);
    if (sock >= 0)
        close(sock);
    free(input);
    free(wire);
    free(message);
    return result;
}
