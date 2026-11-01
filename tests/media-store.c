/* Host tests for user/desktop/media/store.c: Media Source Extensions' coded frame rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "store.h"
#include "segments.h"

static int failures, live;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

struct pk { int tag; };
static void free_pk(void *p) { live--; free(p); }

#define MS 1000LL
#define FR 33333LL                      /* one frame at 30 per second */

/* Video frames from t0 for n frames, a key frame every k, tagged. */
static void add_video(struct store *s, int64_t t0, int n, int k, int tag)
{
    for (int i = 0; i < n; i++) {
        struct pk *p = malloc(sizeof(*p));
        p->tag = tag;
        live++;
        struct sframe f = { p, t0 + i * FR, t0 + i * FR, FR, i % k == 0, 0, 100 };
        store_add(s, &f);
    }
}

static int ranges(struct store *s, int64_t *r)
{
    return store_ranges(s, 5 * MS, r, 8);
}

static void test_video(void)
{
    struct store s;
    int64_t r[16];
    store_init(&s, 0, free_pk);
    add_video(&s, 0, 120, 30, 1);                 /* 0-4 s, key frames each second */
    CHECK(ranges(&s, r) == 1 && r[0] == 0 && r[1] == 120 * FR);
    CHECK(s.ng == 4);
    CHECK(live == 120 && s.bytes == 12000);

    /* the second second again, from another quality: it replaces what was there */
    add_video(&s, 30 * FR, 30, 30, 2);
    CHECK(ranges(&s, r) == 1 && r[0] == 0 && r[1] == 120 * FR);
    CHECK(s.ng == 4 && live == 120);
    CHECK(((struct pk *)s.g[1].f[0].pkt)->tag == 2);

    /* remove 1.5 s - 2.5 s: the group at 1 s is cut short, the one at 2 s goes (its key frame did) */
    store_remove(&s, 1500 * MS, 2500 * MS);
    int n = ranges(&s, r);
    CHECK(n == 2);
    CHECK(r[0] == 0 && r[1] <= 1500 * MS && r[1] > 1450 * MS);
    CHECK(r[2] == 90 * FR && r[3] == 120 * FR);
    CHECK(store_has(&s, 1000 * MS, 0) && !store_has(&s, 2000 * MS, 0));

    /* after remove, a frame that is not a key frame is dropped */
    struct pk *p = malloc(sizeof(*p));
    live++;
    struct sframe f = { p, 4000 * MS, 4000 * MS, FR, 0, 0, 100 };
    CHECK(store_add(&s, &f) == 0);
    store_clear(&s);
    CHECK(live == 0 && s.bytes == 0);
}

static void test_cursor(void)
{
    struct store s;
    struct scursor c;
    const struct sframe *f;
    store_init(&s, 0, free_pk);
    add_video(&s, 0, 60, 30, 1);                  /* 0-2 s */
    add_video(&s, 3000 * MS, 30, 30, 1);          /* 3-4 s, after a gap */
    CHECK(store_seek(&s, &c, 1500 * MS, 0) == 0);
    int got = 0;
    while (store_next(&s, &c, 5 * MS, &f) == SC_FRAME) {
        if (got == 0)
            CHECK(f->key && f->pts == 30 * FR);   /* from the key frame before 1.5 s */
        got++;
    }
    CHECK(got == 30);                             /* to the gap */
    /* the gap is filled: it goes on through it */
    add_video(&s, 60 * FR, 30, 30, 1);            /* 2-3 s (frames end at 90 * FR, next starts at 3000 ms) */
    int more = 0;
    while (store_next(&s, &c, 5 * MS, &f) == SC_FRAME)
        more++;
    CHECK(more == 60);
    /* the frames it stands on go: it is lost, and finds its place again */
    CHECK(store_seek(&s, &c, 500 * MS, 0) == 0);
    store_next(&s, &c, 5 * MS, &f);
    store_remove(&s, 0, 900 * MS);
    CHECK(store_next(&s, &c, 5 * MS, &f) == SC_LOST);
    CHECK(store_seek(&s, &c, 500 * MS, 0) == -1); /* nothing there now */
    CHECK(store_seek(&s, &c, 1200 * MS, 0) == 0);
    store_clear(&s);
}

static void test_audio(void)
{
    struct store s;
    int64_t r[16];
    store_init(&s, 1, free_pk);
    for (int i = 0; i < 500; i++) {               /* 10 s of 20 ms frames */
        struct pk *p = malloc(sizeof(*p));
        live++;
        struct sframe f = { p, i * 20 * MS, i * 20 * MS, 20 * MS, 0, 0, 10 };
        store_add(&s, &f);
    }
    CHECK(s.ng == 10);
    CHECK(ranges(&s, r) == 1 && r[0] == 0 && r[1] == 10000 * MS);
    store_remove(&s, 2500 * MS, 3000 * MS);       /* sound can be cut anywhere */
    CHECK(ranges(&s, r) == 2 && r[1] == 2500 * MS && r[2] == 3000 * MS);
    /* appended again after a seek back: overlapping frames replace, nothing is lost or doubled */
    for (int i = 100; i < 200; i++) {
        struct pk *p = malloc(sizeof(*p));
        live++;
        struct sframe f = { p, i * 20 * MS, i * 20 * MS, 20 * MS, 1, 0, 10 };
        store_add(&s, &f);
    }
    CHECK(ranges(&s, r) == 1 && r[0] == 0 && r[1] == 10000 * MS);
    CHECK(live == 500);
    store_evict_before(&s, 5000 * MS, 2000);
    CHECK(s.bytes <= 2000 || s.g[0].end >= 5000 * MS);
    CHECK(ranges(&s, r) == 1 && r[0] >= 4000 * MS);
    store_clear(&s);
    CHECK(live == 0);
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    unsigned char *b = malloc(1 << 20);
    *len = fread(b, 1, 1 << 20, f);
    fclose(f);
    return b;
}

/* Initialization segments in real YouTube streams (fetched by tools/media-samples.sh), if present. */
static void test_segments(const char *dir)
{
    CHECK(seg_kind("video/mp4; codecs=\"avc1.4d401e\"") == SEG_MP4);
    CHECK(seg_kind("audio/webm;codecs=opus") == SEG_WEBM);
    CHECK(seg_kind("video/x-flv") == SEG_UNKNOWN);
    static const unsigned char moof[16] = { 0, 0, 0, 16, 'm', 'o', 'o', 'f' };
    CHECK(seg_init_length(moof, sizeof(moof), SEG_MP4) == 0);
    static const struct { const char *name; int kind; size_t lo, hi; } files[] = {
        { "yt-133.mp4", SEG_MP4, 739, 739 },        /* ftyp moov | sidx moof ... */
        { "yt-140.mp4", SEG_MP4, 600, 800 },
        { "yt-278.webm", SEG_WEBM, 217, 400 },      /* header, Segment head (and Cues) | Cluster */
        { "yt-249.webm", SEG_WEBM, 259, 400 },
    };
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[512];
        size_t len;
        snprintf(path, sizeof(path), "%s/%s", dir, files[i].name);
        unsigned char *d = slurp(path, &len);
        if (!d)
            continue;
        size_t n = seg_init_length(d, len, files[i].kind);
        if (n < files[i].lo || n > files[i].hi) {
            fprintf(stderr, "%s: init length %zu\n", files[i].name, n);
            failures++;
        }
        if (files[i].kind == SEG_WEBM)
            CHECK(n + 4 <= len && d[n] == 0x1F && d[n + 1] == 0x43 && d[n + 2] == 0xB6 && d[n + 3] == 0x75);
        CHECK(seg_init_length(d + n, len - n, files[i].kind) == 0);    /* a media segment */
        free(d);
    }
}

int main(int argc, char **argv)
{
    test_segments(argc > 1 ? argv[1] : "build/tests/media-samples");
    test_video();
    test_cursor();
    test_audio();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("media-store: all passed\n");
    return 0;
}
