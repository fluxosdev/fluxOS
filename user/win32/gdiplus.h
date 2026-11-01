/* Internal GDI+ bitmap and codec interface. Guest objects are opaque handles. */
#ifndef WIN32_GDIPLUS_H
#define WIN32_GDIPLUS_H
#include "ui.h"
enum { GP_OK,GP_ERROR,GP_INVALID,GP_MEMORY,GP_BUSY,GP_BUFFER,GP_NOTIMPL,GP_WIN32,
       GP_WRONGSTATE,GP_ABORTED,GP_NOTFOUND,GP_OVERFLOW,GP_DENIED,GP_UNKNOWNFORMAT,
       GP_FONTNOTFOUND,GP_FONTSTYLE,GP_NOTTRUETYPE,GP_VERSION,GP_NOTINITIALIZED,
       GP_PROPERTYNOTFOUND,GP_PROPERTYUNSUPPORTED };
#define GP_RGB24 0x21808u
#define GP_RGB32 0x22009u
#define GP_ARGB32 0x26200Au
#define GP_PARGB32 0xE200Bu
struct gp_guid {uint32_t a;uint16_t b,c;uint8_t d[8];};
struct gp_bitmap {
    struct surface pixels;
    uint32_t format;int raw;
    float dpi_x,dpi_y;
    gp_t scan0;int stride;
    gp_t lock_data,lock_bits;int lock_owned,lock_flags,lock_stride;
    uint32_t lock_format;RECT lock_rect;
    int graphics;
};
int gp_started(void);
int gp_pixel_bytes(uint32_t format);
int gp_rows_valid(gp_t scan,int stride,int width,int height,int bytes,int writable);
void gp_read_rows(uint32_t *out,int out_stride,const uint8_t *scan,int stride,int width,int height,uint32_t format);
void gp_write_rows(uint8_t *scan,int stride,const uint32_t *in,int in_stride,int width,int height,uint32_t format);
int gp_bitmap_sync(struct gp_bitmap *b,int write);
uint32_t gp_publish_bitmap(int width,int height,uint32_t format,gp_t output,struct gp_bitmap **bitmap);
struct gp_bitmap *gp_bitmap(uint32_t handle);
int gp_decode(const uint8_t *data,uint32_t size,gp_t output);
int gp_codec_list(uint32_t *a,int encode,int size_only);
int gp_save(struct gp_bitmap *b,const char *path,const struct gp_guid *encoder,gp_t parameters);
void gp_format_guid(int format,struct gp_guid *guid);
extern struct api_dll gdiplus_dll;
extern const struct api_entry gdip_codec_entries[];
extern const int gdip_codec_count;
#endif
