/*
 * Loading pictures of any kind: the file's first bytes say whether it is
 * a PNG, JPEG, GIF, WebP and so on, whatever its name is.
 */
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

int image_is_png(const uint8_t *data, uint32_t len)
{
    return len > 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8);
}

int image_is_jpeg(const uint8_t *data, uint32_t len)
{
    return len > 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

struct image *image_decode(const uint8_t *data, uint32_t len)
{
    if (image_is_jpeg(data, len))
        return image_decode_jpeg(data, len);
    if (image_is_png(data, len))
        return image_decode_png(data, len);
    if (image_is_gif(data, len))
        return image_decode_gif(data, len);
    if (image_is_webp(data, len))
        return image_decode_webp(data, len);
    if (image_is_ico(data, len))
        return image_decode_ico(data, len);
    if (image_is_svg(data, len))
        return image_decode_svg(data, len);
    return NULL;
}

struct image *image_load(const char *path)
{
    uint32_t len;
    uint8_t *buf = file_read_all(path, &len);
    if (!buf)
        return NULL;
    struct image *img = image_decode(buf, len);
    free(buf);
    return img;
}

/*
 * A preview: a JPEG comes out at an eighth of its size (and far quicker).
 * *full_w and *full_h are the size of the picture itself, whatever the
 * size of what is returned.
 */
struct image *image_load_thumb(const char *path, int *full_w, int *full_h)
{
    uint32_t len;
    uint8_t *buf = file_read_all(path, &len);
    if (!buf)
        return NULL;
    struct image *img;
    if (image_is_jpeg(buf, len)) {
        img = image_decode_jpeg_thumb(buf, len, full_w, full_h);
    } else {
        img = image_decode(buf, len);
        if (img) {
            *full_w = img->w;
            *full_h = img->h;
        }
    }
    free(buf);
    return img;
}
