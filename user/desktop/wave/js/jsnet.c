/* The native WebSocket API and nonblocking IPC to wsc transport workers. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "jsint.h"
#include "jsnet.h"
#include "webpolicy.h"
unsigned int uptime_ms(void);
extern char **environ;
#ifndef WS_HELPER
#define WS_HELPER "/bin/wsc"
#endif
#define PEERS 8
#define QUEUE_MAX (4u << 20)
#define ARG(i) ((i) < argc ? args[(i)] : js_undefined)
struct peer {
    struct jsobj *object;
    struct jsstr *url, *protocol;
    int state, binary_type, pid, to, from, credit, error, had_error;
    unsigned started, close_at;
    unsigned char *rx, *tx;
    size_t rn, rc, tn, sent;
};
struct jsnet {
    struct js *J;
    struct jsobj *proto;
    struct peer *peers[PEERS];
    char base[URL_MAX], origin[300];
    void *ctx;
    void (*emit)(void *, jsval, const char *, jsval, int, const char *, int);
    int (*cookies)(void *, const char *, char *, int);
};
static const struct jsclass ws_class;
static void release(struct peer *p)
{
    if (p->to >= 0)
        close(p->to);
    if (p->from >= 0)
        close(p->from);
    p->to = p->from = -1;
    if (p->pid > 0) {
        int status;
        int r = waitpid(p->pid, &status, WNOHANG);
        if (r == 0)
            kill(p->pid, SIGKILL);
        p->pid = 0;
    }
    free(p->rx);
    free(p->tx);
    p->rx = p->tx = NULL;
    p->rn = p->rc = p->tn = p->sent = 0;
}
static struct peer *peer_of(struct js *J, jsval v)
{
    struct peer *p = v.t == JS_OBJ ? js_host_data(v.u.o, &ws_class) : NULL;
    if (!p)
        js_throw_error(J, "TypeError: invalid WebSocket receiver");
    return p;
}
static int get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    struct peer *p = o->hdata;
    if (!strcmp(key, "readyState"))
        *out = js_num(p->state);
    else if (!strcmp(key, "url"))
        *out = (jsval){JS_STR, {.s = p->url}};
    else if (!strcmp(key, "protocol"))
        *out = (jsval){JS_STR, {.s = p->protocol}};
    else if (!strcmp(key, "extensions"))
        *out = js_str(J, "");
    else if (!strcmp(key, "bufferedAmount"))
        *out = js_num(p->tn - p->sent);
    else if (!strcmp(key, "binaryType"))
        *out = js_str(J, p->binary_type ? "arraybuffer" : "blob");
    else
        return 0;
    return 1;
}
static int set(struct js *J, struct jsobj *o, const char *key, jsval v)
{
    struct peer *p = o->hdata;
    if (!strcmp(key, "binaryType")) {
        const char *s = js_tostr(J, v)->s;
        if (!strcmp(s, "arraybuffer"))
            p->binary_type = 1;
        else if (!strcmp(s, "blob"))
            p->binary_type = 0;
        return 1;
    }
    return !strcmp(key, "readyState") || !strcmp(key, "url") || !strcmp(key, "protocol") ||
           !strcmp(key, "extensions") || !strcmp(key, "bufferedAmount");
}
static const struct jsclass ws_class = {"WebSocket", get, set, NULL};
static int enqueue(struct peer *p, char type, const void *data, size_t n)
{
    if (n > QUEUE_MAX || p->tn - p->sent + n + 5 > QUEUE_MAX + 5)
        return -1;
    if (p->sent) {
        memmove(p->tx, p->tx + p->sent, p->tn - p->sent);
        p->tn -= p->sent;
        p->sent = 0;
    }
    unsigned char *next = realloc(p->tx, p->tn + n + 5);
    if (!next)
        return -1;
    p->tx = next;
    unsigned char *h = next + p->tn;
    h[0] = type;
    h[1] = n;
    h[2] = n >> 8;
    h[3] = n >> 16;
    h[4] = n >> 24;
    if (n)
        memcpy(h + 5, data, n);
    p->tn += n + 5;
    return 0;
}
static int launch(struct peer *p)
{
    int in[2], out[2];
    if (pipe(in) < 0)
        return -1;
    if (pipe(out) < 0) {
        close(in[0]);
        close(in[1]);
        return -1;
    }
    int pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0)
            dup2(null, 2);
        for (int i = 3; i < 256; i++)
            close(i);
        char *args[] = {"wsc", p->url->s, NULL};
        execve(WS_HELPER, args, environ);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]);
        close(out[0]);
        return -1;
    }
    p->pid = pid;
    p->to = in[1];
    p->from = out[0];
    p->credit = 4096;
    return 0;
}
static jsval constructor(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsnet *net = J->network;
    if (!J->constructing)
        return js_throw_error(J, "TypeError: WebSocket requires new");
    jsval input = js_to_string(J, ARG(0)), url_ctor = js_get(J, js_obj(J->global), "URL");
    jsval ua[2] = {input, js_str(J, net->base)};
    jsval url = js_construct(J, url_ctor, ua, 2);
    if (J->throwing)
        return js_undefined;
    const char *href = js_tostr(J, js_get(J, url, "href"))->s;
    char normalized[URL_MAX], http_url[URL_MAX];
    if (!strncmp(href, "https://", 8))
        snprintf(normalized, sizeof(normalized), "wss://%s", href + 8);
    else if (!strncmp(href, "http://", 7))
        snprintf(normalized, sizeof(normalized), "ws://%s", href + 7);
    else
        snprintf(normalized, sizeof(normalized), "%s", href);
    int secure = !strncmp(normalized, "wss://", 6);
    if ((!secure && strncmp(normalized, "ws://", 5)) || strchr(normalized, '#') ||
        strlen(href) >= URL_MAX - 4)
        return js_throw_error(J, "SyntaxError: invalid WebSocket URL");
    snprintf(http_url, sizeof(http_url), "%s://%s", secure ? "https" : "http", normalized + (secure ? 6 : 5));
    char origin[300];
    const char *authority = normalized + (secure ? 6 : 5);
    if (!web_origin(http_url, origin, sizeof(origin)) || memchr(authority, '@', strcspn(authority, "/?")))
        return js_throw_error(J, "SyntaxError: invalid WebSocket URL");
    if (!strncmp(net->origin, "https://", 8) && !secure)
        return js_throw_error(J, "SecurityError: insecure WebSocket from HTTPS");
    char protocols[1024] = "";
    jsval offered = ARG(1);
    struct jsobj *list = js_new_array(J);
    if (offered.t == JS_STR)
        js_array_push(J, list, offered);
    else if (offered.t != JS_UNDEF) {
        struct iter it;
        jsval v;
        int r;
        if (js_iter_open(J, offered, &it) < 0)
            return js_undefined;
        while ((r = js_iter_next(J, &it, &v)) > 0)
            js_array_push(J, list, js_to_string(J, v));
        if (r < 0)
            return js_undefined;
    }
    size_t used = 0;
    for (int i = 0; i < list->len; i++) {
        struct jsstr *s = js_tostr(J, list->items[i]);
        if (!s->len ||
            strspn(s->s, "!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") !=
                (size_t)s->len)
            return js_throw_error(J, "SyntaxError: invalid WebSocket subprotocol");
        for (int k = 0; k < i; k++)
            if (js_strict_equal(list->items[k], list->items[i]))
                return js_throw_error(J, "SyntaxError: duplicate WebSocket subprotocol");
        if (used + (size_t)s->len + 2 >= sizeof(protocols))
            return js_throw_error(J, "SyntaxError: too many WebSocket subprotocols");
        if (used)
            protocols[used++] = ',';
        memcpy(protocols + used, s->s, (size_t)s->len);
        used += s->len;
        protocols[used] = 0;
    }
    int slot = -1;
    for (int i = 0; i < PEERS; i++)
        if (!net->peers[i]) {
            slot = i;
            break;
        }
    if (slot < 0)
        return js_throw_error(J, "QuotaExceededError: too many WebSockets");
    struct peer *p = js_alloc(J, sizeof(*p));
    p->to = p->from = -1;
    p->started = uptime_ms();
    p->url = js_mkstr(J, normalized, (int)strlen(normalized));
    p->protocol = js_mkstr(J, "", 0);
    struct jsobj *o = js_new_host(J, &ws_class, p);
    o->proto = net->proto;
    p->object = o;
    net->peers[slot] = p;
    char cookies[4096] = "", config[8192];
    if (net->cookies)
        net->cookies(net->ctx, http_url, cookies, sizeof(cookies));
    int n = snprintf(config, sizeof(config), "%s\n%s\n%s", net->origin, protocols, cookies);
    if (n < 0 || n >= (int)sizeof(config) || enqueue(p, 'I', config, (size_t)n) < 0 || launch(p) < 0)
        p->error = 1;
    return js_obj(o);
}
static jsval send_message(struct js *J, jsval self, jsval *args, int argc)
{
    struct peer *p = peer_of(J, self);
    if (!p)
        return js_undefined;
    if (p->state == 0)
        return js_throw_error(J, "InvalidStateError: WebSocket is connecting");
    if (p->state != 1)
        return js_undefined;
    int n = 0;
    unsigned char *bytes = js_binary_bytes(ARG(0), &n);
    char type = 'B';
    if (!bytes && ARG(0).t == JS_OBJ)
        bytes = js_binary_bytes(js_get(J, ARG(0), "_bytes"), &n);
    if (!bytes) {
        jsval text = js_to_string(J, ARG(0)), f = js_get(J, js_obj(J->global), "%encodeUTF8");
        jsval value = js_call_value(J, f, js_undefined, &text, 1);
        if (J->throwing)
            return js_undefined;
        bytes = js_binary_bytes(value, &n);
        type = 'T';
    }
    if (enqueue(p, type, bytes, (size_t)n) < 0)
        return js_throw_error(J, "QuotaExceededError: WebSocket send buffer is full");
    return js_undefined;
}
static jsval close_socket(struct js *J, jsval self, jsval *args, int argc)
{
    struct peer *p = peer_of(J, self);
    if (!p)
        return js_undefined;
    int code = 1000;
    if (ARG(0).t != JS_UNDEF) {
        double n = js_to_number(J, ARG(0));
        if (n != 1000 && !(n >= 3000 && n <= 4999))
            return js_throw_error(J, "InvalidAccessError: invalid close code");
        code = (int)n;
    }
    jsval reason = ARG(1).t == JS_UNDEF ? js_str(J, "") : js_to_string(J, ARG(1));
    jsval encoder = js_get(J, js_obj(J->global), "%encodeUTF8");
    jsval encoded = js_call_value(J, encoder, js_undefined, &reason, 1);
    if (J->throwing)
        return js_undefined;
    int n;
    unsigned char *bytes = js_binary_bytes(encoded, &n);
    if (n > 123)
        return js_throw_error(J, "SyntaxError: WebSocket close reason exceeds 123 UTF-8 bytes");
    if (p->state >= 2)
        return js_undefined;
    if (p->state == 0) {
        p->error = 1;
        return js_undefined;
    }
    unsigned char payload[125];
    payload[0] = code >> 8;
    payload[1] = code;
    if (n)
        memcpy(payload + 2, bytes, (size_t)n);
    if (enqueue(p, 'C', payload, (size_t)n + 2) < 0) {
        p->error = 1;
        return js_undefined;
    }
    p->state = 2;
    p->close_at = uptime_ms();
    return js_undefined;
}
static void emit(struct jsnet *net, struct peer *p, const char *type, jsval data, int code,
                 const char *reason, int clean)
{
    net->emit(net->ctx, js_obj(p->object), type, data, code, reason, clean);
}
static void close_peer(struct jsnet *net, int i, int code, const char *reason, int clean)
{
    struct peer *p = net->peers[i];
    p->state = 3;
    release(p);
    net->peers[i] = NULL;
    emit(net, p, "close", js_undefined, code, reason, clean);
}
void jsnet_poll(struct jsnet *net)
{
    if (!net)
        return;
    struct js *J = net->J;
    for (int i = 0; i < PEERS; i++) {
        struct peer *p = net->peers[i];
        if (!p)
            continue;
        if (p->error || (p->state == 0 && (unsigned)(uptime_ms() - p->started) > 30000) ||
            (p->state == 2 && (unsigned)(uptime_ms() - p->close_at) > 6000)) {
            if (!p->had_error)
                emit(net, p, "error", js_undefined, 0, "WebSocket connection failed", 0);
            close_peer(net, i, 1006, "", 0);
            continue;
        }
        struct pollfd r = {p->from, POLLIN, 0};
        if (poll(&r, 1, 0) > 0 && (r.revents & (POLLIN | POLLHUP | POLLERR))) {
            if (p->rc - p->rn < 8192) {
                size_t cap = p->rc ? p->rc * 2 : 16384;
                if (cap > QUEUE_MAX + 16384)
                    cap = QUEUE_MAX + 16384;
                unsigned char *next = cap > p->rn ? realloc(p->rx, cap) : NULL;
                if (!next) {
                    p->error = 1;
                    continue;
                }
                p->rx = next;
                p->rc = cap;
            }
            int n = (int)read(p->from, p->rx + p->rn, p->rc - p->rn);
            if (n <= 0) {
                p->error = 1;
                continue;
            }
            p->rn += (size_t)n;
            while (p->rn >= 5 && net->peers[i] == p) {
                size_t len = (size_t)p->rx[1] | (size_t)p->rx[2] << 8 | (size_t)p->rx[3] << 16 |
                             (size_t)p->rx[4] << 24;
                if (len > QUEUE_MAX) {
                    p->error = 1;
                    break;
                }
                if (p->rn < 5 + len)
                    break;
                unsigned char kind = p->rx[0], *data = p->rx + 5;
                if (kind == 'A') {
                    if (len != 4) {
                        p->error = 1;
                        break;
                    }
                    unsigned credit = (unsigned)data[0] | (unsigned)data[1] << 8 | (unsigned)data[2] << 16 |
                                      (unsigned)data[3] << 24;
                    if (credit > 4096u || (unsigned)p->credit + credit > 4096u) {
                        p->error = 1;
                        break;
                    }
                    p->credit += (int)credit;
                } else if (kind == 'O') {
                    p->protocol = js_mkstr(J, (char *)data, (int)len);
                    p->state = 1;
                    emit(net, p, "open", js_undefined, 0, "", 0);
                } else if ((kind == 'T' || kind == 'B') && p->state == 1) {
                    jsval value;
                    if (kind == 'T')
                        value = js_strn(J, (char *)data, (int)len);
                    else {
                        jsval c = js_get(J, js_obj(J->global), "Uint8Array"), narg = js_num((double)len);
                        value = js_construct(J, c, &narg, 1);
                        int size;
                        unsigned char *b = js_binary_bytes(value, &size);
                        if (b)
                            memcpy(b, data, len);
                        value = js_get(J, value, "buffer");
                        if (!p->binary_type) {
                            struct jsobj *parts = js_new_array(J);
                            js_array_push(J, parts, value);
                            jsval args[1] = {js_obj(parts)};
                            value = js_construct(J, js_get(J, js_obj(J->global), "Blob"), args, 1);
                        }
                    }
                    emit(net, p, "message", value, 0, "", 0);
                } else if (kind == 'T' || kind == 'B') {
                } else if (kind == 'E') {
                    int first = !p->had_error;
                    p->had_error = 1;
                    char reason[256];
                    snprintf(reason, sizeof(reason), "%.*s", (int)(len > 255 ? 255 : len), (char *)data);
                    if (first)
                        emit(net, p, "error", js_undefined, 0, reason, 0);
                } else if (kind == 'C') {
                    int code = len >= 2 ? data[0] << 8 | data[1] : 1006;
                    char reason[124] = "";
                    if (len > 2)
                        snprintf(reason, sizeof(reason), "%.*s", (int)(len - 2), (char *)data + 2);
                    close_peer(net, i, code, reason, !p->had_error && code != 1006);
                    break;
                } else {
                    p->error = 1;
                    break;
                }
                if (net->peers[i] != p)
                    break;
                memmove(p->rx, p->rx + 5 + len, p->rn - 5 - len);
                p->rn -= 5 + len;
            }
        }
        if (net->peers[i] != p)
            continue;
        if (p->tn > p->sent && p->credit > 0) {
            size_t n = p->tn - p->sent;
            if (n > (size_t)p->credit)
                n = (size_t)p->credit;
            struct pollfd w = {p->to, POLLOUT, 0};
            if (poll(&w, 1, 0) > 0 && (w.revents & POLLOUT)) {
                void (*old)(int) = signal(SIGPIPE, SIG_IGN);
                int sent = (int)write(p->to, p->tx + p->sent, n);
                signal(SIGPIPE, old);
                if (sent <= 0)
                    p->error = 1;
                else {
                    p->sent += (size_t)sent;
                    p->credit -= sent;
                    if (p->sent == p->tn) {
                        free(p->tx);
                        p->tx = NULL;
                        p->tn = p->sent = 0;
                    }
                }
            }
        }
    }
}
int jsnet_wait(struct jsnet *net)
{
    if (net)
        for (int i = 0; i < PEERS; i++)
            if (net->peers[i])
                return 16;
    return -1;
}
struct jsnet *jsnet_new(struct js *J, const char *base, const char *origin, void *ctx,
                        void (*callback)(void *, jsval, const char *, jsval, int, const char *, int),
                        int (*cookies)(void *, const char *, char *, int))
{
    struct jsnet *net = calloc(1, sizeof(*net));
    if (!net)
        return NULL;
    net->J = J;
    net->ctx = ctx;
    net->emit = callback;
    net->cookies = cookies;
    snprintf(net->base, sizeof(net->base), "%s", base ? base : "");
    snprintf(net->origin, sizeof(net->origin), "%s", origin ? origin : "null");
    J->network = net;
    js_gc_root(J, net, sizeof(*net));
    net->proto = js_new_object(J);
    jsval event_proto = js_get(J, js_get(J, js_obj(J->global), "EventTarget"), "prototype");
    if (event_proto.t == JS_OBJ)
        net->proto->proto = event_proto.u.o;
    js_set(J, js_obj(net->proto), "dispatchEvent", js_get(J, js_obj(J->global), "dispatchEvent"));
    jsval xhr = js_get(J, js_get(J, js_obj(J->global), "XMLHttpRequest"), "prototype");
    js_set(J, js_obj(net->proto), "addEventListener", js_get(J, xhr, "addEventListener"));
    js_set(J, js_obj(net->proto), "removeEventListener", js_get(J, xhr, "removeEventListener"));
    js_define_native(J, net->proto, "send", send_message, 1);
    js_define_native(J, net->proto, "close", close_socket, 2);
    jsval c = js_new_function(J, constructor, "WebSocket", 1);
    c.u.o->ctor = 1;
    js_set(J, c, "prototype", js_obj(net->proto));
    js_set(J, js_obj(net->proto), "constructor", c);
    const char *states[] = {"CONNECTING", "OPEN", "CLOSING", "CLOSED"};
    for (int i = 0; i < 4; i++) {
        js_set(J, c, states[i], js_num(i));
        js_set(J, js_obj(net->proto), states[i], js_num(i));
    }
    js_set(J, js_obj(J->global), "WebSocket", c);
    return net;
}
void jsnet_free(struct jsnet *net)
{
    if (!net)
        return;
    for (int i = 0; i < PEERS; i++)
        if (net->peers[i])
            release(net->peers[i]);
    js_gc_unroot(net->J, net);
    net->J->network = NULL;
    free(net);
}
