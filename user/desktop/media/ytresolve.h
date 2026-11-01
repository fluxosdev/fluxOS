/*
 * YouTube addresses to streams (ytresolve.c): what the Media Player plays when it is given a
 * YouTube link, and what Wave's "Play in Media Player" hands it.
 *
 * The video's streams come from YouTube's own player API (InnerTube), asked as one of its apps
 * would ask, since the apps' answers carry addresses that play as they are (a web page's need its
 * player's script to unscramble them). The clients tried, in order, are built in; /etc/youtube.conf
 * can name others, and Invidious servers to ask after them:
 *
 *     client NAME VERSION CLIENT_ID USER_AGENT...
 *     invidious https://example.org
 *
 * The HTTP requests go through a function the caller gives, so this file has no network code.
 */
#ifndef MEDIA_YTRESOLVE_H
#define MEDIA_YTRESOLVE_H

#include <stddef.h>

struct yt_format {
    int itag;
    char mime[48];              /* "video/mp4" */
    char codecs[64];            /* "avc1.42001E, mp4a.40.2" */
    int video, audio;           /* what it has */
    int width, height, fps, bitrate;
    long long length;           /* bytes, or 0 if not known */
    char *url;
};

struct yt_video {
    char id[16];
    char title[200], author[96];
    int seconds;
    char user_agent[200];       /* the streams must be fetched with this */
    struct yt_format *f;
    int nf;
    char error[200];
};

/* An HTTP request: method "GET" or "POST" (with body); the response body (malloc'd, NUL-terminated)
 * in *out. Returns the HTTP status, or -1. */
typedef int (*yt_http_fn)(void *ctx, const char *method, const char *url, const char *const *headers,
                          int nheaders, const char *body, size_t body_len, char **out, size_t *out_len);

/* The video id in a YouTube address (watch?v=, youtu.be/, shorts/, embed/, live/, v/), or 0. */
int yt_video_id(const char *url, char id[16]);
/* Ask for the video's streams. 0, or -1 with v->error. */
int yt_resolve(const char *id, yt_http_fn http, void *ctx, struct yt_video *v);
void yt_free(struct yt_video *v);
/* A stream with picture and sound together, as tall as max_h at most if there is one (the tallest),
 * of a kind FFmpeg here decodes; or NULL. */
const struct yt_format *yt_pick_muxed(const struct yt_video *v, int max_h);
/* Separate picture and sound: the tallest H.264 (else VP9) picture up to max_h and the best AAC
 * (else Opus) sound. 0 if both were found. */
int yt_pick_split(const struct yt_video *v, int max_h, const struct yt_format **video, const struct yt_format **audio);

#endif
