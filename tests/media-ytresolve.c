/*
 * Host tests for user/desktop/media: the JSON reader, YouTube address parsing and the choice of
 * streams. With MEDIA_LIVE=1 it also asks YouTube itself (through curl) for a video's streams and
 * fetches the first bytes of the chosen ones.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "json.h"
#include "ytresolve.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void test_json(void)
{
    const char *t = "{\"a\":1.5,\"s\":\"x\\u00e9\\n\\ud83d\\ude00\",\"o\":{\"b\":[1,true,null,\"2\"]},\"n\":\"42\"}";
    struct json *j = json_parse(t, strlen(t));
    CHECK(j != NULL);
    CHECK(json_num(j, "a", 0) == 1.5);
    CHECK(!strcmp(json_str(j, "s", ""), "x\xC3\xA9\n\xF0\x9F\x98\x80"));
    const struct json *b = json_path(j, "o.b");
    CHECK(b && b->type == JSON_ARR && b->child && b->child->next && b->child->next->type == JSON_TRUE);
    CHECK(json_num(j, "n", 0) == 42);
    CHECK(json_get(j, "missing") == NULL);
    json_free(j);
    CHECK(json_parse("{\"a\":}", 6) == NULL);
    CHECK(json_parse("[1,2", 4) == NULL);
    CHECK(json_parse("{} x", 4) == NULL);
}

static void test_ids(void)
{
    char id[16];
    CHECK(yt_video_id("https://www.youtube.com/watch?v=dQw4w9WgXcQ", id) && !strcmp(id, "dQw4w9WgXcQ"));
    CHECK(yt_video_id("https://www.youtube.com/watch?feature=share&v=dQw4w9WgXcQ&t=10", id) && !strcmp(id, "dQw4w9WgXcQ"));
    CHECK(yt_video_id("https://youtu.be/dQw4w9WgXcQ?si=abc", id) && !strcmp(id, "dQw4w9WgXcQ"));
    CHECK(yt_video_id("https://m.youtube.com/shorts/dQw4w9WgXcQ", id));
    CHECK(yt_video_id("https://www.youtube-nocookie.com/embed/dQw4w9WgXcQ?autoplay=1", id));
    CHECK(yt_video_id("http://YouTube.com/live/dQw4w9WgXcQ", id));
    CHECK(!yt_video_id("https://www.youtube.com/watch?v=short", id));
    CHECK(!yt_video_id("https://www.youtube.com/channel/UCuAXFkgsw1L7xaCfnd5JJOw", id));
    CHECK(!yt_video_id("https://notyoutube.com/watch?v=dQw4w9WgXcQ", id));
    CHECK(!yt_video_id("https://www.youtube.com/watchlater?v=dQw4w9WgXcQ", id));
    CHECK(!yt_video_id("file:///watch?v=dQw4w9WgXcQ", id));
}

static const char *canned;
static int canned_http(void *ctx, const char *method, const char *url, const char *const *headers, int nh,
                       const char *body, size_t blen, char **out, size_t *olen)
{
    (void)ctx, (void)method, (void)url, (void)headers, (void)nh, (void)body, (void)blen;
    *out = strdup(canned);
    *olen = strlen(canned);
    return 200;
}

static void test_pick(void)
{
    canned = "{\"playabilityStatus\":{\"status\":\"OK\"},\"videoDetails\":{\"title\":\"T\",\"author\":\"A\",\"lengthSeconds\":\"19\"},"
             "\"streamingData\":{\"formats\":[{\"itag\":18,\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001E, mp4a.40.2\\\"\",\"width\":640,\"height\":360,\"url\":\"https://x/18\"}],"
             "\"adaptiveFormats\":["
             "{\"itag\":137,\"mimeType\":\"video/mp4; codecs=\\\"avc1.640028\\\"\",\"height\":1080,\"url\":\"https://x/137\"},"
             "{\"itag\":135,\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d401f\\\"\",\"height\":480,\"url\":\"https://x/135\"},"
             "{\"itag\":244,\"mimeType\":\"video/webm; codecs=\\\"vp9\\\"\",\"height\":480,\"url\":\"https://x/244\"},"
             "{\"itag\":399,\"mimeType\":\"video/mp4; codecs=\\\"av01.0.08M.08\\\"\",\"height\":1080,\"url\":\"https://x/399\"},"
             "{\"itag\":140,\"mimeType\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\",\"bitrate\":130000,\"url\":\"https://x/140\"},"
             "{\"itag\":251,\"mimeType\":\"audio/webm; codecs=\\\"opus\\\"\",\"bitrate\":160000,\"url\":\"https://x/251\"},"
             "{\"itag\":250,\"mimeType\":\"audio/webm; codecs=\\\"opus\\\"\",\"signatureCipher\":\"s=1\"}]}}";
    struct yt_video v;
    CHECK(yt_resolve("dQw4w9WgXcQ", canned_http, NULL, &v) == 0);
    CHECK(!strcmp(v.title, "T") && v.seconds == 19 && v.nf == 7);
    const struct yt_format *m = yt_pick_muxed(&v, 720), *fv, *fa;
    CHECK(m && m->itag == 18 && m->video && m->audio);
    CHECK(yt_pick_split(&v, 720, &fv, &fa) == 0);
    CHECK(fv && fv->itag == 135);
    CHECK(fa && fa->itag == 140);
    CHECK(yt_pick_split(&v, 2000, &fv, &fa) == 0 && fv->itag == 137);
    yt_free(&v);

    canned = "{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\",\"reason\":\"Sign in to confirm your age\"}}";
    CHECK(yt_resolve("dQw4w9WgXcQ", canned_http, NULL, &v) == -1);
    CHECK(strstr(v.error, "Sign in") != NULL);
    yt_free(&v);
}

/* ---- live, through curl ---- */

static int curl_http(void *ctx, const char *method, const char *url, const char *const *headers, int nh,
                     const char *body, size_t blen, char **out, size_t *olen)
{
    (void)ctx;
    char bodyf[] = "/tmp/yt-bodyXXXXXX", outf[] = "/tmp/yt-outXXXXXX";
    int bfd = mkstemp(bodyf), ofd = mkstemp(outf);
    if (body && write(bfd, body, blen) != (ssize_t)blen)
        return -1;
    close(bfd);
    close(ofd);
    char cmd[8192];
    int n = snprintf(cmd, sizeof(cmd), "curl -sS -m 30 -X %s -o %s -w '%%{http_code}'", method, outf);
    for (int i = 0; i < nh; i++)
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " -H '%s'", headers[i]);
    if (body)
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " --data-binary @%s", bodyf);
    snprintf(cmd + n, sizeof(cmd) - (size_t)n, " '%s'", url);
    FILE *p = popen(cmd, "r");
    int status = -1;
    if (p) {
        if (fscanf(p, "%d", &status) != 1)
            status = -1;
        pclose(p);
    }
    FILE *f = fopen(outf, "rb");
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap + 1);
    size_t r;
    while (f && buf && (r = fread(buf + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, cap + 1);
        }
    }
    if (f)
        fclose(f);
    if (buf)
        buf[len] = 0;
    unlink(bodyf);
    unlink(outf);
    *out = buf;
    *olen = len;
    return status;
}

static int fetch_head(const char *url, const char *ua)
{
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "curl -sS -m 30 -r 0-65535 -o /dev/null -w '%%{http_code}' -A '%s' '%s'", ua, url);
    FILE *p = popen(cmd, "r");
    int status = -1;
    if (p) {
        if (fscanf(p, "%d", &status) != 1)
            status = -1;
        pclose(p);
    }
    return status;
}

static void test_live(void)
{
    struct yt_video v;
    int r = yt_resolve("jNQXAC9IVRw", curl_http, NULL, &v);
    if (r) {
        fprintf(stderr, "live: %s\n", v.error);
        failures++;
        return;
    }
    printf("live: \"%s\" by %s, %d s, %d streams (client UA %.30s...)\n", v.title, v.author, v.seconds, v.nf, v.user_agent);
    const struct yt_format *m = yt_pick_muxed(&v, 480), *fv, *fa;
    if (m) {
        int st = fetch_head(m->url, v.user_agent);
        printf("live: muxed itag %d %s [%s] %dp -> %d\n", m->itag, m->mime, m->codecs, m->height, st);
        CHECK(st == 206 || st == 200);
    }
    CHECK(yt_pick_split(&v, 480, &fv, &fa) == 0);
    if (fv && fa) {
        int sv = fetch_head(fv->url, v.user_agent), sa = fetch_head(fa->url, v.user_agent);
        printf("live: split itag %d [%s] %dp -> %d, itag %d [%s] -> %d\n", fv->itag, fv->codecs, fv->height, sv,
               fa->itag, fa->codecs, sa);
        CHECK(sv == 206 || sv == 200);
        CHECK(sa == 206 || sa == 200);
    }
    yt_free(&v);
}

int main(void)
{
    test_json();
    test_ids();
    test_pick();
    const char *live = getenv("MEDIA_LIVE");
    if (live && *live == '1')
        test_live();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("media-ytresolve: all passed\n");
    return 0;
}
