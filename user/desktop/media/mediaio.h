/*
 * Opening what FFmpeg plays (mediaio.c): a local file, or an http(s):// address read through
 * httpio.c, since the FFmpeg here is built without network code of its own.
 */
#ifndef MEDIA_MEDIAIO_H
#define MEDIA_MEDIAIO_H

#include <stddef.h>
#include <libavformat/avformat.h>

struct httpio;

struct media_input {
    AVFormatContext *fmt;
    AVIOContext *pb;                    /* ours, for an address */
    struct httpio *hio;
    volatile int abort;                 /* FFmpeg's waits give up */
};

/* Open a path or an address (with these request headers) and read its streams' details.
 * 0, or -1 with why in err. */
int media_input_open(struct media_input *in, const char *where, const char *const *headers, int nheaders,
                     char *err, size_t errsize);
/* Make whatever is reading from it give up (from another thread). */
void media_input_abort(struct media_input *in);
void media_input_close(struct media_input *in);
int media_is_url(const char *where);

#endif
