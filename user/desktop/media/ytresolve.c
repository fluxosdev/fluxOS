/* YouTube addresses to streams: see ytresolve.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "json.h"
#include "ytresolve.h"

#define CONF_FILE  "/etc/youtube.conf"
#define MAX_CLIENTS 8
#define MAX_INV     8

struct client {
    char name[40], version[32], id[8], ua[200];
};

/* The apps whose answers are plain addresses. YouTube changes what it accepts now and then; the
 * configuration file can name newer ones without a rebuild. */
static const struct client builtin[] = {
    { "ANDROID", "20.10.38", "3", "com.google.android.youtube/20.10.38 (Linux; U; Android 14) gzip" },
    { "IOS", "20.10.4", "5", "com.google.ios.youtube/20.10.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)" },
};

static int is_id_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static int take_id(const char *p, char id[16])
{
    int n = 0;
    while (is_id_char((unsigned char)p[n]) && n < 15)
        n++;
    if (n != 11)                        /* every video id is eleven characters */
        return 0;
    memcpy(id, p, 11);
    id[11] = 0;
    return 1;
}

int yt_video_id(const char *url, char id[16])
{
    const char *p = url;
    if (!strncasecmp(p, "https://", 8))
        p += 8;
    else if (!strncasecmp(p, "http://", 7))
        p += 7;
    else
        return 0;
    size_t hl = strcspn(p, "/?#");
    char host[96];
    if (hl == 0 || hl >= sizeof(host))
        return 0;
    memcpy(host, p, hl);
    host[hl] = 0;
    for (char *h = host; *h; h++)
        if (*h >= 'A' && *h <= 'Z')
            *h = (char)(*h - 'A' + 'a');
    const char *path = p + hl;
    if (!strcmp(host, "youtu.be"))
        return *path == '/' && take_id(path + 1, id);
    static const char *const hosts[] = { "youtube.com", "www.youtube.com", "m.youtube.com", "music.youtube.com",
                                         "youtube-nocookie.com", "www.youtube-nocookie.com" };
    int known = 0;
    for (unsigned i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
        known |= !strcmp(host, hosts[i]);
    if (!known)
        return 0;
    static const char *const prefixes[] = { "/shorts/", "/embed/", "/live/", "/v/", "/e/" };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (!strncmp(path, prefixes[i], strlen(prefixes[i])))
            return take_id(path + strlen(prefixes[i]), id);
    if (strncmp(path, "/watch", 6) || (path[6] && path[6] != '?' && path[6] != '/'))
        return 0;
    const char *q = strchr(path, '?');
    while (q && *q && *q != '#') {
        q++;
        if (!strncmp(q, "v=", 2))
            return take_id(q + 2, id);
        q += strcspn(q, "&#");
    }
    return 0;
}

static void fill_format(struct yt_format *f, const struct json *j)
{
    f->itag = (int)json_num(j, "itag", 0);
    const char *mime = json_str(j, "mimeType", json_str(j, "type", ""));
    if (!mime)
        mime = "";
    size_t ml = strcspn(mime, ";");
    snprintf(f->mime, sizeof(f->mime), "%.*s", (int)ml, mime);
    const char *c = strstr(mime, "codecs=\"");
    if (c) {
        c += 8;
        snprintf(f->codecs, sizeof(f->codecs), "%.*s", (int)strcspn(c, "\""), c);
    }
    f->video = !strncmp(f->mime, "video/", 6);
    /* a video/... type with one codec of sound only would be odd; with two, it has both */
    f->audio = !strncmp(f->mime, "audio/", 6) || strchr(f->codecs, ',') != NULL;
    f->width = (int)json_num(j, "width", 0);
    f->height = (int)json_num(j, "height", 0);
    if (!f->height) {                   /* Invidious: "size": "640x360" */
        const char *size = json_str(j, "size", NULL);
        if (size)
            sscanf(size, "%dx%d", &f->width, &f->height);
    }
    f->fps = (int)json_num(j, "fps", 0);
    f->bitrate = (int)json_num(j, "bitrate", 0);
    f->length = (long long)json_num(j, "contentLength", json_num(j, "clen", 0));
}

static int add_formats(struct yt_video *v, const struct json *list, const char *base)
{
    for (const struct json *j = list && list->type == JSON_ARR ? list->child : NULL; j; j = j->next) {
        const char *url = json_str(j, "url", NULL);
        if (!url || !*url)
            continue;                   /* a scrambled address (signatureCipher): not for us */
        struct yt_format *more = realloc(v->f, sizeof(*more) * (size_t)(v->nf + 1));
        if (!more)
            return -1;
        v->f = more;
        struct yt_format *f = &v->f[v->nf];
        memset(f, 0, sizeof(*f));
        fill_format(f, j);
        if (url[0] == '/' && base) {    /* an Invidious server's own address */
            size_t n = strlen(base) + strlen(url) + 1;
            f->url = malloc(n);
            if (f->url)
                snprintf(f->url, n, "%s%s", base, url);
        } else {
            f->url = strdup(url);
        }
        if (!f->url)
            return -1;
        if (strncasecmp(f->url, "https://", 8) && strncasecmp(f->url, "http://", 7)) {
            free(f->url);               /* only web addresses: never a file name for FFmpeg to open */
            f->url = NULL;
            continue;
        }
        v->nf++;
    }
    return 0;
}

static int load_conf(struct client *clients, int *nclients, char inv[][128], int *ninv)
{
    FILE *fp = fopen(CONF_FILE, "r");
    if (!fp)
        return 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        char kind[16];
        int n = 0;
        if (line[0] == '#' || sscanf(line, "%15s %n", kind, &n) < 1)
            continue;
        if (!strcmp(kind, "client") && *nclients < MAX_CLIENTS) {
            struct client *c = &clients[*nclients];
            int m = 0;
            memset(c, 0, sizeof(*c));
            if (sscanf(line + n, "%39s %31s %7s %n", c->name, c->version, c->id, &m) >= 3) {
                snprintf(c->ua, sizeof(c->ua), "%s", m ? line + n + m : "");
                (*nclients)++;
            }
        } else if (!strcmp(kind, "invidious") && *ninv < MAX_INV) {
            snprintf(inv[*ninv], 128, "%s", line + n);
            size_t l = strlen(inv[*ninv]);
            while (l && inv[*ninv][l - 1] == '/')
                inv[*ninv][--l] = 0;
            if (l)
                (*ninv)++;
        }
    }
    fclose(fp);
    return 0;
}

static void set_details(struct yt_video *v, const struct json *d)
{
    snprintf(v->title, sizeof(v->title), "%s", json_str(d, "title", ""));
    snprintf(v->author, sizeof(v->author), "%s", json_str(d, "author", ""));
    v->seconds = (int)json_num(d, "lengthSeconds", 0);
}

static int ask_innertube(const char *id, const struct client *c, yt_http_fn http, void *ctx, struct yt_video *v)
{
    char body[1024], hname[64], hver[80], hua[240];
    int android = !strncmp(c->name, "ANDROID", 7), ios = !strcmp(c->name, "IOS");
    snprintf(body, sizeof(body),
             "{\"context\":{\"client\":{\"clientName\":\"%s\",\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"US\"%s}},"
             "\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}",
             c->name, c->version,
             android ? ",\"androidSdkVersion\":34,\"osName\":\"Android\",\"osVersion\":\"14\""
             : ios ? ",\"deviceMake\":\"Apple\",\"deviceModel\":\"iPhone16,2\",\"osName\":\"iPhone\",\"osVersion\":\"18.3.2.22D82\""
             : "", id);
    snprintf(hname, sizeof(hname), "X-YouTube-Client-Name: %s", c->id);
    snprintf(hver, sizeof(hver), "X-YouTube-Client-Version: %s", c->version);
    snprintf(hua, sizeof(hua), "User-Agent: %s", c->ua);
    const char *headers[] = { "Content-Type: application/json", hname, hver, hua, "Origin: https://www.youtube.com" };
    char *out = NULL;
    size_t len = 0;
    int status = http(ctx, "POST", "https://www.youtube.com/youtubei/v1/player?prettyPrint=false", headers,
                      c->ua[0] ? 5 : 4, body, strlen(body), &out, &len);
    if (status != 200) {
        snprintf(v->error, sizeof(v->error), status < 0 ? "Cannot reach YouTube." : "YouTube answered %d.", status);
        free(out);
        return -1;
    }
    struct json *j = json_parse(out, len);
    free(out);
    if (!j) {
        snprintf(v->error, sizeof(v->error), "YouTube's answer could not be read.");
        return -1;
    }
    const char *st = json_str(json_get(j, "playabilityStatus"), "status", "");
    if (strcmp(st, "OK")) {
        const char *why = json_str(json_get(j, "playabilityStatus"), "reason", st);
        snprintf(v->error, sizeof(v->error), "%s", *why ? why : "YouTube will not play this video.");
        json_free(j);
        return -1;
    }
    set_details(v, json_get(j, "videoDetails"));
    snprintf(v->user_agent, sizeof(v->user_agent), "%s", c->ua);
    const struct json *sd = json_get(j, "streamingData");
    int r = add_formats(v, json_get(sd, "formats"), NULL) | add_formats(v, json_get(sd, "adaptiveFormats"), NULL);
    json_free(j);
    if (r < 0 || !v->nf) {
        snprintf(v->error, sizeof(v->error), "YouTube gave no streams that can be played here.");
        return -1;
    }
    return 0;
}

static int ask_invidious(const char *id, const char *base, yt_http_fn http, void *ctx, struct yt_video *v)
{
    char url[300];
    snprintf(url, sizeof(url), "%s/api/v1/videos/%s?local=true", base, id);
    char *out = NULL;
    size_t len = 0;
    int status = http(ctx, "GET", url, NULL, 0, NULL, 0, &out, &len);
    struct json *j = status == 200 ? json_parse(out, len) : NULL;
    free(out);
    if (!j) {
        snprintf(v->error, sizeof(v->error), "%s did not answer.", base);
        return -1;
    }
    set_details(v, j);
    v->user_agent[0] = 0;
    int r = add_formats(v, json_get(j, "formatStreams"), base) | add_formats(v, json_get(j, "adaptiveFormats"), base);
    json_free(j);
    return r < 0 || !v->nf ? -1 : 0;
}

static void clear_formats(struct yt_video *v)
{
    for (int i = 0; i < v->nf; i++)
        free(v->f[i].url);
    free(v->f);
    v->f = NULL;
    v->nf = 0;
}

int yt_resolve(const char *id, yt_http_fn http, void *ctx, struct yt_video *v)
{
    memset(v, 0, sizeof(*v));
    snprintf(v->id, sizeof(v->id), "%s", id);
    struct client clients[MAX_CLIENTS + 2];
    char inv[MAX_INV][128];
    int nclients = 0, ninv = 0;
    load_conf(clients, &nclients, inv, &ninv);
    for (unsigned i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++)
        clients[nclients++] = builtin[i];
    char first_error[200] = "";
    for (int i = 0; i < nclients; i++) {
        if (ask_innertube(id, &clients[i], http, ctx, v) == 0)
            return 0;
        clear_formats(v);
        if (!first_error[0])
            snprintf(first_error, sizeof(first_error), "%s", v->error);
    }
    for (int i = 0; i < ninv; i++) {
        if (ask_invidious(id, inv[i], http, ctx, v) == 0)
            return 0;
        clear_formats(v);
    }
    snprintf(v->error, sizeof(v->error), "%s", first_error[0] ? first_error : "This video cannot be played.");
    return -1;
}

void yt_free(struct yt_video *v)
{
    clear_formats(v);
}

/* Codecs the FFmpeg here decodes; 2 for the ones that are lighter to decode. */
static int video_codec_ok(const char *codecs)
{
    if (!strncmp(codecs, "avc1", 4))
        return 2;
    if (!strncmp(codecs, "vp9", 3) || !strncmp(codecs, "vp09", 4) || !strncmp(codecs, "vp8", 3))
        return 1;
    return 0;                           /* AV1 is not built in */
}

static int audio_codec_ok(const char *codecs)
{
    const char *a = strchr(codecs, ',') ? strchr(codecs, ',') + 1 : codecs;
    while (*a == ' ')
        a++;
    if (!strncmp(a, "mp4a.40.2", 9))
        return 3;
    if (!strncmp(a, "opus", 4))
        return 2;
    if (!strncmp(a, "mp4a", 4) || !strncmp(a, "vorbis", 6))
        return 1;
    return 0;
}

const struct yt_format *yt_pick_muxed(const struct yt_video *v, int max_h)
{
    const struct yt_format *best = NULL, *smallest = NULL;
    for (int i = 0; i < v->nf; i++) {
        const struct yt_format *f = &v->f[i];
        if (!f->video || !f->audio || !video_codec_ok(f->codecs) || !audio_codec_ok(f->codecs))
            continue;
        if (!smallest || f->height < smallest->height)
            smallest = f;
        if (f->height <= max_h && (!best || f->height > best->height))
            best = f;
    }
    return best ? best : smallest;
}

int yt_pick_split(const struct yt_video *v, int max_h, const struct yt_format **video, const struct yt_format **audio)
{
    const struct yt_format *bv = NULL, *ba = NULL;
    for (int i = 0; i < v->nf; i++) {
        const struct yt_format *f = &v->f[i];
        if (f->video && !f->audio) {
            int ok = video_codec_ok(f->codecs);
            if (!ok || (f->height > max_h && bv && bv->height <= max_h))
                continue;
            int okb = bv ? video_codec_ok(bv->codecs) : 0;
            /* lighter codecs first; then the tallest that fits (or the smallest of those too tall) */
            if (!bv || ok > okb || (ok == okb && ((bv->height > max_h && f->height < bv->height) ||
                                                  (f->height <= max_h && f->height > bv->height) ||
                                                  (f->height == bv->height && f->fps < bv->fps))))
                bv = f;
        } else if (f->audio && !f->video) {
            int ok = audio_codec_ok(f->codecs), oka = ba ? audio_codec_ok(ba->codecs) : 0;
            if (ok && (!ba || ok > oka || (ok == oka && f->bitrate > ba->bitrate)))
                ba = f;
        }
    }
    *video = bv;
    *audio = ba;
    return bv && ba ? 0 : -1;
}
