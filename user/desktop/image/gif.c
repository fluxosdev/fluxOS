/*
 * GIF87a/GIF89a pictures and timed animations, including local palettes,
 * interlacing, transparency, loop counts and restore-background/previous disposal.
 * Frames decode on demand into a stable logical-screen pixel buffer.
 */
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

#define MAX_DIM 8192

struct lzw_reader {
    const uint8_t *p, *end;     /* the data sub-blocks */
    int block_left;
    uint32_t bits;
    int nbits;
};

static int next_byte(struct lzw_reader *r)
{
    while (!r->block_left) {
        if (r->p >= r->end)
            return -1;
        r->block_left = *r->p++;
        if (!r->block_left)
            return -1;                  /* the terminator */
    }
    if (r->p >= r->end)
        return -1;
    r->block_left--;
    return *r->p++;
}

static int read_code(struct lzw_reader *r, int size)
{
    while (r->nbits < size) {
        int b = next_byte(r);
        if (b < 0)
            return -1;
        r->bits |= (uint32_t)b << r->nbits;
        r->nbits += 8;
    }
    int code = (int)(r->bits & ((1u << size) - 1));
    r->bits >>= size;
    r->nbits -= size;
    return code;
}

/* Decode the image data into w*h palette indexes; 0 on success. */
static int lzw_decode(const uint8_t *p, const uint8_t *end, int min_size, uint8_t *out, int total)
{
    if (min_size < 1 || min_size > 11)
        return -1;
    struct lzw_reader r = { p, end, 0, 0, 0 };
    int clear = 1 << min_size, eoi = clear + 1, next = clear + 2, size = min_size + 1;
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], stack[4097];
    int prev = -1, n = 0;
    uint8_t first = 0;
    for (int i = 0; i < clear; i++)
        suffix[i] = (uint8_t)i;
    while (n < total) {
        int code = read_code(&r, size);
        if (code < 0 || code == eoi)
            break;
        if (code == clear) {
            size = min_size + 1;
            next = clear + 2;
            prev = -1;
            continue;
        }
        int sp = 0, cur = code;
        if (prev < 0) {
            if (code >= clear)
                return -1;
            out[n++] = (uint8_t)code;
            first = (uint8_t)code;
            prev = code;
            continue;
        }
        if (code >= next) {
            if (code > next)
                return -1;
            stack[sp++] = first;        /* the KwKwK case */
            cur = prev;
        }
        while (cur >= clear) {
            if (sp >= 4096)
                return -1;
            stack[sp++] = suffix[cur];
            cur = prefix[cur];
        }
        first = suffix[cur];
        stack[sp++] = first;
        while (sp && n < total)
            out[n++] = stack[--sp];
        if (next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = first;
            next++;
            if (next == (1 << size) && size < 12)
                size++;
        }
        prev = code;
    }
    return n == total ? 0 : -1;
}

/* Decoder state is owned by the image, without changing the image ABI used by
 * static decoders and caller-created canvas images. Pixels stay at one address. */
struct gif_animation {
    struct image *img;
    uint8_t *data;
    const uint8_t *p, *end, *start, *gct;
    int gsize, background, transparent, disposal, delay;
    int x, y, w, h, loops, repeats, frames, stopped;
    uint32_t *restore;
    uint32_t due;
    int started;
    struct gif_animation *next;
};
static struct gif_animation *animations;

static const uint8_t *skip_blocks(const uint8_t *p, const uint8_t *end)
{
    while (p < end) {
        int n = *p++;
        if (!n) return p;
        if (end - p < n) return NULL;
        p += n;
    }
    return NULL;
}

static uint32_t palette_color(const uint8_t *ct, int i)
{
    return 0xff000000u | (uint32_t)ct[i*3] << 16 | (uint32_t)ct[i*3+1] << 8 | ct[i*3+2];
}

static void dispose_frame(struct gif_animation *a)
{
    struct image *img = a->img;
    if (a->disposal == 3 && a->restore)
        memcpy(img->px, a->restore, (size_t)img->w * img->h * sizeof(uint32_t));
    else if (a->disposal == 2) {
        uint32_t bg = a->transparent >= 0 || !a->gct ? 0 : palette_color(a->gct,a->background);
        for (int y=a->y; y<a->y+a->h && y<img->h; y++)
            for (int x=a->x; x<a->x+a->w && x<img->w; x++) img->px[y*img->w+x]=bg;
    }
    free(a->restore);
    a->restore = NULL;
}

/* Decode exactly one displayed frame; reject truncated blocks before changing pixels. */
static int next_frame(struct gif_animation *a)
{
    const uint8_t *p=a->p, *end=a->end;
    int transparent=-1, disposal=0, delay=100;
    while (p < end) {
        int marker=*p++;
        if (marker == 0x3b) { a->p=p; return 0; }
        if (marker == 0x21) {
            if (p == end) return -1;
            int label=*p++;
            const uint8_t *after=skip_blocks(p,end);
            if (!after) return -1;
            if (label == 0xf9) {
                if (end-p < 6 || p[0] != 4 || p[5] != 0) return -1;
                transparent=p[1]&1 ? p[4] : -1;
                disposal=(p[1]>>2)&7;
                delay=(p[2]|p[3]<<8)*10;
                if (delay < 20) delay=100;
            } else if (label == 0xff && end-p >= 17 && p[0]==11 &&
                       (!memcmp(p+1,"NETSCAPE2.0",11) || !memcmp(p+1,"ANIMEXTS1.0",11)) &&
                       p[12]==3 && p[13]==1) {
                a->repeats=p[14]|p[15]<<8;
            }
            p=after;
            continue;
        }
        if (marker != 0x2c || end-p < 9) return -1;
        int x=p[0]|p[1]<<8, y=p[2]|p[3]<<8, w=p[4]|p[5]<<8, h=p[6]|p[7]<<8;
        int flags=p[8];
        p+=9;
        const uint8_t *ct=a->gct;
        int colors=a->gsize;
        if (flags&0x80) {
            colors=2<<(flags&7);
            if (end-p < colors*3) return -1;
            ct=p; p+=colors*3;
        }
        if (!ct || p == end || w<=0 || h<=0 || w>MAX_DIM || h>MAX_DIM ||
            (size_t)w*h > 16u*1024*1024) return -1;
        int min_size=*p++;
        const uint8_t *after=skip_blocks(p,end);
        if (!after) return -1;
        uint8_t *indices=malloc((size_t)w*h);
        if (!indices) return -1;
        if (lzw_decode(p,after,min_size,indices,w*h)<0) {free(indices);return -1;}
        for (int i=0; i<w*h; i++)
            if (indices[i]>=colors && indices[i]!=transparent) {free(indices);return -1;}
        dispose_frame(a);
        size_t bytes=(size_t)a->img->w*a->img->h*sizeof(uint32_t);
        if (disposal == 3) {
            a->restore=malloc(bytes);
            if (!a->restore) {free(indices);return -1;}
            memcpy(a->restore,a->img->px,bytes);
        }
        int pass=0,row=0;
        static const int starts[4]={0,4,2,1}, steps[4]={8,8,4,2};
        for (int i=0;i<h;i++) {
            int cy=y+row;
            if (cy<a->img->h)
                for (int j=0;j<w && x+j<a->img->w;j++) {
                    int index=indices[i*w+j];
                    if (index!=transparent) a->img->px[cy*a->img->w+x+j]=palette_color(ct,index);
                }
            if (flags&0x40) {
                row+=steps[pass];
                while(row>=h && pass<3) row=starts[++pass];
            } else row++;
        }
        free(indices);
        a->p=after;
        a->x=x;a->y=y;a->w=w;a->h=h;
        a->transparent=transparent;a->disposal=disposal;a->delay=delay;
        a->frames++;
        return 1;
    }
    return -1;
}

void image_animation_free(struct image *img)
{
    for (struct gif_animation **p=&animations;*p;p=&(*p)->next)
        if ((*p)->img==img) {
            struct gif_animation *a=*p;
            *p=a->next;
            free(a->data);free(a->restore);free(a);
            return;
        }
}

/* Unsigned differences keep deadlines correct when the millisecond clock wraps. */
int image_animate(struct image *img, uint32_t now)
{
    struct gif_animation *a;
    for (a=animations;a && a->img!=img;a=a->next) {}
    if (!a || a->stopped) return 0;
    if (!a->started) {a->started=1;a->due=now+(uint32_t)a->delay;return 0;}
    if ((int32_t)(now-a->due)<0) return 0;
    int result=next_frame(a);
    if (!result && a->frames>1 && (a->repeats==0 || a->loops<a->repeats)) {
        a->loops++;
        a->p=a->start;
        a->disposal=0;
        free(a->restore);a->restore=NULL;
        memset(img->px,0,(size_t)img->w*img->h*sizeof(uint32_t));
        result=next_frame(a);
    }
    if (result<=0) { image_animation_free(img); return 0; }
    a->due=now+(uint32_t)a->delay;
    return result>0;
}

struct image *image_decode_gif(const uint8_t *data, uint32_t len)
{
    if (len<13 || !image_is_gif(data,len)) return NULL;
    int w=data[6]|data[7]<<8, h=data[8]|data[9]<<8;
    if (!w || !h || w>MAX_DIM || h>MAX_DIM || (size_t)w*h>16u*1024*1024) return NULL;
    struct gif_animation *a=calloc(1,sizeof(*a));
    struct image *img=calloc(1,sizeof(*img));
    if (!a || !img) {free(a);free(img);return NULL;}
    img->w=w;img->h=h;
    img->px=calloc((size_t)w*h,sizeof(uint32_t));
    a->data=malloc(len);
    if (!img->px || !a->data) {free(img->px);free(img);free(a->data);free(a);return NULL;}
    memcpy(a->data,data,len);
    a->img=img;a->p=a->data+13;a->end=a->data+len;
    a->transparent=-1;a->repeats=-1;
    a->background=data[11];
    if (data[10]&0x80) {
        a->gsize=2<<(data[10]&7);
        if (a->end-a->p<a->gsize*3 || a->background>=a->gsize) goto fail;
        a->gct=a->p;a->p+=a->gsize*3;
    }
    a->start=a->p;
    if (next_frame(a)!=1) goto fail;
    a->next=animations;animations=a;
    return img;
fail:
    free(a->data);free(a->restore);free(a);free(img->px);free(img);
    return NULL;
}

int image_is_gif(const uint8_t *data, uint32_t len)
{
    return len > 6 && !memcmp(data, "GIF8", 4) && (data[4] == '7' || data[4] == '9') && data[5] == 'a';
}
