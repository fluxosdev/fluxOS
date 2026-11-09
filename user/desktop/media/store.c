/* Coded frames waiting to be decoded: see store.h. */
#include <stdlib.h>
#include <string.h>
#include "store.h"

#define AUDIO_GOP_US 1000000            /* sound frames are grouped a second at a time */

void store_init(struct store *s, int audio, void (*free_pkt)(void *pkt))
{
    memset(s, 0, sizeof(*s));
    s->audio = audio;
    s->free_pkt = free_pkt;
    s->next_serial = 1;
}

static void free_frames(struct store *s, struct sgop *g, int from)
{
    for (int i = from; i < g->n; i++) {
        if (s->free_pkt && g->f[i].pkt)
            s->free_pkt(g->f[i].pkt);
        s->bytes -= g->f[i].size;
    }
    if (from < g->n)
        g->n = from;
}

static void delete_gop(struct store *s, int i)
{
    struct sgop *g = &s->g[i];
    if (g->serial == s->app_serial)
        s->app_serial = 0;
    free_frames(s, g, 0);
    free(g->f);
    memmove(&s->g[i], &s->g[i + 1], sizeof(*s->g) * (size_t)(s->ng - i - 1));
    s->ng--;
}

void store_clear(struct store *s)
{
    while (s->ng)
        delete_gop(s, s->ng - 1);
    free(s->g);
    s->g = NULL;
    s->cap = 0;
    s->app_serial = 0;
    s->have_last = 0;
}

static int64_t span(const struct sframe *f, int audio)
{
    return f->dur > 0 ? f->dur : audio ? 20000 : 33333;
}

static void recompute_end(struct sgop *g, int audio)
{
    int64_t end = g->start;
    for (int i = 0; i < g->n; i++)
        if (g->f[i].pts + span(&g->f[i], audio) > end)
            end = g->f[i].pts + span(&g->f[i], audio);
    g->end = end;
}

static int find_serial(const struct store *s, uint32_t serial)
{
    for (int i = s->ng - 1; i >= 0; i--)          /* the newest groups are looked for most */
        if (s->g[i].serial == serial)
            return i;
    return -1;
}

/* Keep the groups in order of their start after g[i]'s start changed; its new index. */
static int resort(struct store *s, int i)
{
    while (i > 0 && s->g[i - 1].start > s->g[i].start) {
        struct sgop t = s->g[i - 1];
        s->g[i - 1] = s->g[i];
        s->g[i] = t;
        i--;
    }
    while (i + 1 < s->ng && s->g[i + 1].start < s->g[i].start) {
        struct sgop t = s->g[i + 1];
        s->g[i + 1] = s->g[i];
        s->g[i] = t;
        i++;
    }
    return i;
}

/* Take the frames of other groups than keep off [a, b), with what depends on them. */
static void clear_overlap(struct store *s, int64_t a, int64_t b, uint32_t keep)
{
    for (int i = 0; i < s->ng;) {
        struct sgop *h = &s->g[i];
        if (h->serial == keep || h->end <= a || h->start >= b) {
            i++;
            continue;
        }
        if (h->start >= a) {            /* its key frame goes, so all of it does */
            delete_gop(s, i);
            continue;
        }
        int cut = h->n;                 /* the first frame (in decode order) shown at a or later */
        for (int k = 0; k < h->n; k++)
            if (h->f[k].pts >= a) {
                cut = k;
                break;
            }
        free_frames(s, h, cut);
        recompute_end(h, s->audio);
        if (h->end > a)
            h->end = a;                 /* its last frame's length only reached into it */
        if (h->serial == s->app_serial && cut < h->n)
            s->app_serial = 0;
        if (!h->n) {
            delete_gop(s, i);
            continue;
        }
        i++;
    }
}

void store_reset_append(struct store *s)
{
    s->app_serial = 0;
    s->have_last = 0;
}

int store_add(struct store *s, const struct sframe *in)
{
    struct sframe f = *in;
    if (s->audio)
        f.key = 1;
    int64_t d = span(&f, s->audio);
    if (s->have_last) {                 /* a jump in decode time: start afresh at a key frame */
        int64_t gap = s->last_dur > 0 ? 2 * s->last_dur : 1000000;
        if (f.dts < s->last_dts || f.dts - s->last_dts > gap)
            s->app_serial = 0;
    }
    int gi = s->app_serial ? find_serial(s, s->app_serial) : -1;
    if (gi < 0)
        s->app_serial = 0;
    if (!s->app_serial && !f.key) {     /* nothing to decode it from */
        if (s->free_pkt && f.pkt)
            s->free_pkt(f.pkt);
        return 0;
    }
    int fresh = gi < 0 || (f.key && (!s->audio || f.pts - s->g[gi].start >= AUDIO_GOP_US || f.pts < s->g[gi].start));
    if (fresh) {
        if (s->ng == s->cap) {
            int cap = s->cap ? s->cap * 2 : 64;
            struct sgop *more = realloc(s->g, sizeof(*more) * (size_t)cap);
            if (!more) {
                if (s->free_pkt && f.pkt)
                    s->free_pkt(f.pkt);
                return 0;
            }
            s->g = more;
            s->cap = cap;
        }
        gi = s->ng++;
        memset(&s->g[gi], 0, sizeof(s->g[gi]));
        s->g[gi].start = f.pts;
        s->g[gi].end = f.pts;
        s->g[gi].serial = s->next_serial++;
        if (!s->next_serial)
            s->next_serial = 1;
        s->app_serial = s->g[gi].serial;
        gi = resort(s, gi);
    }
    uint32_t serial = s->g[gi].serial;
    clear_overlap(s, f.pts, f.pts + d, serial);
    gi = find_serial(s, serial);
    struct sgop *g = &s->g[gi];
    if (g->n == g->cap) {
        int cap = g->cap ? g->cap * 2 : (s->audio ? 64 : 32);
        struct sframe *more = realloc(g->f, sizeof(*more) * (size_t)cap);
        if (!more) {
            if (s->free_pkt && f.pkt)
                s->free_pkt(f.pkt);
            return 0;
        }
        g->f = more;
        g->cap = cap;
    }
    g->f[g->n++] = f;
    s->bytes += f.size;
    if (f.pts + d > g->end)
        g->end = f.pts + d;
    if (f.pts < g->start) {
        g->start = f.pts;
        resort(s, gi);
    }
    s->last_dts = f.dts;
    s->last_dur = f.dur;
    s->have_last = 1;
    return 1;
}

void store_remove(struct store *s, int64_t start, int64_t end)
{
    if (end <= start)
        return;
    clear_overlap(s, start, end, 0);
    s->app_serial = 0;                  /* what follows must start at a key frame */
}

int store_ranges(const struct store *s, int64_t tol, int64_t *r, int max)
{
    int n = 0;
    for (int i = 0; i < s->ng; i++) {
        const struct sgop *g = &s->g[i];
        if (!g->n)
            continue;
        if (n && g->start <= r[2 * n - 1] + tol) {
            if (g->end > r[2 * n - 1])
                r[2 * n - 1] = g->end;
            continue;
        }
        if (n == max)
            break;
        r[2 * n] = g->start;
        r[2 * n + 1] = g->end;
        n++;
    }
    return n;
}

int store_has(const struct store *s, int64_t t, int64_t tol)
{
    return store_end_from(s, t, tol) > t;
}

int64_t store_end_from(const struct store *s, int64_t t, int64_t tol)
{
    int64_t end = t;
    int in = 0;
    for (int i = 0; i < s->ng; i++) {
        const struct sgop *g = &s->g[i];
        if (!g->n)
            continue;
        if (!in) {
            if (g->start <= t + tol && g->end > t) {
                in = 1;
                end = g->end;
            }
        } else if (g->start <= end + tol) {
            if (g->end > end)
                end = g->end;
        } else {
            break;
        }
    }
    return end;
}

void store_evict_before(struct store *s, int64_t t, size_t budget)
{
    while (s->ng && s->bytes > budget && s->g[0].end < t)
        delete_gop(s, 0);
}

int store_seek(const struct store *s, struct scursor *c, int64_t t, int64_t tol)
{
    int best = -1;
    for (int i = 0; i < s->ng; i++) {
        const struct sgop *g = &s->g[i];
        if (g->n && g->start <= t + tol && t < g->end)
            best = i;                   /* the last that starts in time: the closest key frame */
    }
    if (best < 0)
        return -1;
    c->serial = s->g[best].serial;
    c->idx = 0;
    c->last_pts = INT64_MIN;
    return 0;
}

int store_next(const struct store *s, struct scursor *c, int64_t tol, const struct sframe **f)
{
    int i = c->serial ? find_serial(s, c->serial) : -1;
    if (i < 0)
        return SC_LOST;
    const struct sgop *g = &s->g[i];
    if (c->idx < g->n) {
        *f = &g->f[c->idx++];
        c->last_pts = (*f)->pts;
        return SC_FRAME;
    }
    /* this group is used up: on to the next if it follows on */
    for (int k = i + 1; k < s->ng; k++) {
        const struct sgop *h = &s->g[k];
        if (!h->n)
            continue;
        if (h->start > g->end + tol)
            return SC_WAIT;
        if (h->end <= g->end)
            continue;                   /* inside this one's span: an odd leftover */
        c->serial = h->serial;
        c->idx = 1;
        *f = &h->f[0];
        c->last_pts = (*f)->pts;
        return SC_FRAME;
    }
    return SC_WAIT;
}
