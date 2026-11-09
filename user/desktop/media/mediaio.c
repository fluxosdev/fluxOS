/* Opening what FFmpeg plays: see mediaio.h. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <libavutil/mem.h>
#include "httpio.h"
#include "mediaio.h"

#define IO_BUF 65536

int media_is_url(const char *where)
{
    return !strncasecmp(where, "http://", 7) || !strncasecmp(where, "https://", 8);
}

static int io_read(void *opaque, uint8_t *buf, int n)
{
    int r = httpio_read(opaque, buf, n);
    return r > 0 ? r : r == 0 ? AVERROR_EOF : AVERROR(EIO);
}

static int64_t io_seek(void *opaque, int64_t off, int whence)
{
    struct httpio *h = opaque;
    if (whence & AVSEEK_SIZE)
        return httpio_size(h) >= 0 ? httpio_size(h) : AVERROR(ENOSYS);
    whence &= ~AVSEEK_FORCE;
    int64_t r = httpio_seek(h, off, whence);
    return r >= 0 ? r : AVERROR(EINVAL);
}

static int interrupted(void *opaque)
{
    return ((struct media_input *)opaque)->abort;
}

int media_input_open(struct media_input *in, const char *where, const char *const *headers, int nheaders,
                     char *err, size_t errsize)
{
    int abort = in->abort;              /* given up already (before this thread got here): stays so */
    memset(in, 0, sizeof(*in));
    in->abort = abort;
    in->fmt = avformat_alloc_context();
    if (!in->fmt) {
        snprintf(err, errsize, "Out of memory.");
        return -1;
    }
    in->fmt->interrupt_callback.callback = interrupted;
    in->fmt->interrupt_callback.opaque = in;
    if (media_is_url(where)) {
        /* (in->hio is only set once this returns: until then in->abort is what stops the wait) */
        in->hio = httpio_open_cancel(where, headers, nheaders, err, errsize, &in->abort);
        if (!in->hio) {
            avformat_free_context(in->fmt);
            in->fmt = NULL;
            return -1;
        }
        uint8_t *buf = av_malloc(IO_BUF);
        in->pb = buf ? avio_alloc_context(buf, IO_BUF, 0, in->hio, io_read, NULL, io_seek) : NULL;
        if (!in->pb) {
            av_free(buf);
            snprintf(err, errsize, "Out of memory.");
            media_input_close(in);
            return -1;
        }
        in->pb->seekable = httpio_size(in->hio) >= 0 ? AVIO_SEEKABLE_NORMAL : 0;
        in->fmt->pb = in->pb;
        in->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    /* through our reader FFmpeg is given no name: it would try to open references in the file with
     * protocols it does not have */
    if (avformat_open_input(&in->fmt, in->pb ? "" : where, NULL, NULL) < 0) {     /* frees fmt when it fails */
        snprintf(err, errsize, "%s", in->hio && httpio_error(in->hio)[0] ? httpio_error(in->hio)
                                     : "This is not a sound or video file that can be played.");
        media_input_close(in);
        return -1;
    }
    if (avformat_find_stream_info(in->fmt, NULL) < 0) {
        snprintf(err, errsize, "This is not a media file.");
        media_input_close(in);
        return -1;
    }
    return 0;
}

void media_input_abort(struct media_input *in)
{
    in->abort = 1;
    if (in->hio)
        httpio_abort(in->hio);
}

void media_input_close(struct media_input *in)
{
    if (in->fmt)
        avformat_close_input(&in->fmt);
    if (in->pb) {
        av_freep(&in->pb->buffer);
        avio_context_free(&in->pb);
    }
    httpio_close(in->hio);
    in->hio = NULL;
    in->abort = 0;                      /* the next open starts afresh */
}
