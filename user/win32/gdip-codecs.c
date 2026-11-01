/* Image codecs and the guest ImageCodecInfo ABI. Only working encoders are
 * advertised. PNG uses stored DEFLATE blocks; JPEG uses the desktop encoder. */
#include <errno.h>
#include <unistd.h>
#include "gdiplus.h"
static uint16_t u16(const uint8_t *p) {
    return p[0]|p[1]<<8;
}
static uint32_t u32(const uint8_t *p) {
    return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;
}
static const uint32_t raw_ids[]={
    0xB96B3CAA,0xB96B3CAB,0xB96B3CAE,0xB96B3CAF,0xB96B3CB0,0xB96B3CB5
};
void gp_format_guid(int format,struct gp_guid *guid)
{
    *guid=(struct gp_guid){
        raw_ids[format>=0 && format<6 ? format:0],0x0728,0x11D3,{
            0x9D,0x7B,0,0,0xF8,0x1E,0xF3,0x2E
        }
    };
}
static void codec_guid(int format,struct gp_guid *guid)
{
    *guid=(struct gp_guid){
        format==1 ? 0x557CF400:format==2 ? 0x557CF401:format==3 ? 0x557CF406:0x557CF402,0x1A04,0x11D3,{
            0x9A,0x73,0,0,0xF8,0x1E,0xF3,0x2E
        }
    };
}
static int encoder_format(const struct gp_guid *guid)
{
    if(!guid) return 0;
    for(int i=1;i<=3;i++) {
        struct gp_guid expected;
        codec_guid(i,&expected);
        if(!memcmp(guid,&expected,16)) return i;
    }
    return 0;
}
static int decode_bmp(const uint8_t *data,uint32_t size,gp_t output)
{
    if(size<54 || u32(data+14)<40 || u32(data+14)>size-14 || u16(data+26)!=1) return GP_UNKNOWNFORMAT;
    int width=u32(data+18),height=u32(data+22),top=height<0;
    uint16_t bpp=u16(data+28);
    uint32_t offset=u32(data+10);
    if(height==INT32_MIN || width<=0 || !height || u32(data+30)!=0 || (bpp!=1 && bpp!=4 && bpp!=8 && bpp!=24 && bpp!=32)) return GP_UNKNOWNFORMAT;
    if(top) height=-height;
    uint64_t stride=(((uint64_t)width*bpp+31)/32)*4;
    if(offset>size || stride*height>size-offset) return GP_UNKNOWNFORMAT;
    uint32_t palette[256];
    int colors=0;
    if(bpp<=8) {
        colors=u32(data+46);
        if(!colors) colors=1<<bpp;
        uint32_t start=14+u32(data+14);
        if(colors>(1<<bpp) || start>offset || (uint32_t)colors*4>offset-start) return GP_UNKNOWNFORMAT;
        for(int i=0;i<colors;i++) palette[i]=0xFF000000|(u32(data+start+i*4)&0xFFFFFF);
    }
    struct gp_bitmap *b;
    int status=gp_publish_bitmap(width,height,bpp==32 ? GP_RGB32:GP_RGB24,output,&b);
    if(status) return status;
    for(int y=0;y<height;y++) {
        const uint8_t *row=data+offset+(top ? y:height-1-y)*stride;
        for(int x=0;x<width;x++) {
            uint32_t c;
            if(bpp<=8) {
                int index=bpp==8 ? row[x]:bpp==4 ? (row[x/2]>>(x%2 ? 0:4))&15:(row[x/8]>>(7-x%8))&1;
                c=index<colors ? palette[index]:0xFF000000;
            }
            else {
                const uint8_t *p=row+x*(bpp/8);
                c=0xFF000000|p[0]|p[1]<<8|p[2]<<16;
            }
            b->pixels.px[y*width+x]=c;
        }
    }
    b->raw=1;
    int32_t xppm=u32(data+38),yppm=u32(data+42);
    if(xppm>0) b->dpi_x=xppm*0.0254f;
    if(yppm>0) b->dpi_y=yppm*0.0254f;
    return GP_OK;
}
int gp_decode(const uint8_t *data,uint32_t size,gp_t output)
{
    if(!data || !size) return GP_UNKNOWNFORMAT;
    if(size>=2 && data[0]=='B' && data[1]=='M') return decode_bmp(data,size,output);
    int format=image_is_png(data,size) ? 3:image_is_jpeg(data,size) ? 2:image_is_gif(data,size) ? 4:image_is_ico(data,size) ? 5:0;
    if(!format) return GP_UNKNOWNFORMAT;
    struct image *im=image_decode(data,size);
    if(!im) return GP_UNKNOWNFORMAT;
    struct gp_bitmap *b;
    int status=gp_publish_bitmap(im->w,im->h,GP_ARGB32,output,&b);
    if(!status) {
        memcpy(b->pixels.px,im->px,(size_t)im->w*im->h*4);
        b->raw=format;
    }
    image_free(im);
    return status;
}
struct codec {
    const char *description,*extension,*mime;
    uint8_t signature[8];
    uint32_t signature_size;
};
static const struct codec codecs[]={
    {"", "", "", {0}, 0},
    {"BMP", "*.BMP;*.DIB", "image/bmp", {'B','M'}, 2},
    {"JPEG", "*.JPG;*.JPEG;*.JPE;*.JFIF", "image/jpeg", {255,216,255}, 3},
    {"PNG", "*.PNG", "image/png", {137,'P','N','G',13,10,26,10}, 8},
    {"GIF", "*.GIF", "image/gif", {'G','I','F','8'}, 4}
};
struct codec_info {
    struct gp_guid clsid,format;
    gp_t dll,description,extension,mime;
    uint32_t flags,version,sig_count,sig_size;
    gp_t signature,mask;
};
static uint32_t codec_size(int count)
{
    uint32_t size=count*sizeof(struct codec_info);
    for(int i=1;i<=count;i++) size+=(strlen(codecs[i].description)+strlen(codecs[i].extension)+strlen(codecs[i].mime)+3)*2+codecs[i].signature_size*2;
    return size;
}
static gp_t codec_string(gp_t *cursor,const char *text)
{
    gp_t result=*cursor;
    int n=strlen(text)+1;
    ansi_to_wide(text,n,GP(result),n);
    *cursor+=n*2;
    return result;
}
int gp_codec_list(uint32_t *a,int encode,int size_only)
{
    if(!gp_started()) return GP_NOTINITIALIZED;
    int count=encode ? 3:4;
    uint32_t size=codec_size(count);
    if(size_only) {
        if(!vm_writable(A(0),4) || !vm_writable(A(1),4)) return GP_INVALID;
        *(uint32_t *)P(0)=count;
        *(uint32_t *)P(1)=size;
        return GP_OK;
    }
    if(A(0)!=(uint32_t)count || A(1)<size || !A(2) || !vm_writable(A(2),A(1))) return GP_INVALID;
    memset(P(2),0,size);
    struct codec_info *info=P(2);
    gp_t cursor=A(2)+count*sizeof(*info);
    for(int i=0;i<count;i++) {
        int format=i+1;
        codec_guid(format,&info[i].clsid);
        gp_format_guid(format,&info[i].format);
        info[i].description=codec_string(&cursor,codecs[format].description);
        info[i].extension=codec_string(&cursor,codecs[format].extension);
        info[i].mime=codec_string(&cursor,codecs[format].mime);
        info[i].flags=0x10000|4|2|(format<=3 ? 1:0);
        info[i].version=1;
        info[i].sig_count=1;
        info[i].sig_size=codecs[format].signature_size;
        info[i].signature=cursor;
        memcpy(GP(cursor),codecs[format].signature,info[i].sig_size);
        cursor+=info[i].sig_size;
        info[i].mask=cursor;
        memset(GP(cursor),255,info[i].sig_size);
        cursor+=info[i].sig_size;
    }
    return GP_OK;
}
static void put16(uint8_t *p,uint16_t n) {
    p[0]=n;
    p[1]=n>>8;
}
static void put32(uint8_t *p,uint32_t n) {
    for(int i=0;i<4;i++) p[i]=n>>(8*i);
}
static int write_bmp(FILE *file,struct gp_bitmap *b)
{
    int w=b->pixels.w,h=b->pixels.h;
    uint32_t stride=(w*3+3)&~3u,size=stride*h;
    uint8_t header[54]={0};
    header[0]='B';
    header[1]='M';
    put32(header+2,54+size);
    put32(header+10,54);
    put32(header+14,40);
    put32(header+18,w);
    put32(header+22,h);
    put16(header+26,1);
    put16(header+28,24);
    put32(header+34,size);
    put32(header+38,b->dpi_x/0.0254f);
    put32(header+42,b->dpi_y/0.0254f);
    uint8_t *row=calloc(stride,1);
    if(!row) return -1;
    int okay=fwrite(header,1,54,file)==54;
    for(int y=h-1;okay && y>=0;y--) {
        gp_write_rows(row,stride,b->pixels.px+y*w,w,w,1,GP_RGB24);
        okay=fwrite(row,1,stride,file)==stride;
    }
    free(row);
    return okay ? 0:-1;
}
static uint32_t crc_update(uint32_t crc,const uint8_t *data,uint32_t size)
{
    for(uint32_t i=0;i<size;i++) {
        crc^=data[i];
        for(int bit=0;bit<8;bit++) crc=(crc>>1)^(0xEDB88320u&-(int32_t)(crc&1));
    }
    return crc;
}
static void big32(uint8_t *p,uint32_t n) {
    for(int i=0;i<4;i++) p[i]=n>>(24-i*8);
}
static int chunk(FILE *file,const char *type,const uint8_t *data,uint32_t size)
{
    uint8_t n[4];
    big32(n,size);
    uint32_t crc=crc_update(0xFFFFFFFF,(const uint8_t *)type,4);
    crc=crc_update(crc,data,size)^0xFFFFFFFF;
    int okay=fwrite(n,1,4,file)==4 && fwrite(type,1,4,file)==4 && (!size || fwrite(data,1,size,file)==size);
    big32(n,crc);
    return okay && fwrite(n,1,4,file)==4 ? 0:-1;
}
static int write_png(FILE *file,struct gp_bitmap *b)
{
    int w=b->pixels.w,h=b->pixels.h;
    uint32_t row=1+w*4,raw_size=row*h;
    uint32_t blocks=(raw_size+65534)/65535,total=raw_size+blocks*5+6;
    uint8_t *z=malloc(total);
    if(!z) return -1;
    z[0]=0x78;
    z[1]=1;
    uint32_t pos=2,offset=0,adler_a=1,adler_b=0;
    while(offset<raw_size) {
        uint32_t count=raw_size-offset;
        if(count>65535) count=65535;
        z[pos++]=offset+count==raw_size ? 1:0;
        put16(z+pos,count);
        put16(z+pos+2,~count);
        pos+=4;
        for(uint32_t i=0;i<count;i++) {
            uint32_t at=offset+i,x=at%row,y=at/row;
            uint8_t value=0;
            if(x) {
                uint32_t p=b->pixels.px[y*w+(x-1)/4];
                int component=(x-1)%4;
                value=component==3 ? p>>24:p>>(16-component*8);
            }
            z[pos++]=value;
            adler_a=(adler_a+value)%65521;
            adler_b=(adler_b+adler_a)%65521;
        }
        offset+=count;
    }
    big32(z+pos,(adler_b<<16)|adler_a);
    pos+=4;
    uint8_t ihdr[13]={0};
    big32(ihdr,w);
    big32(ihdr+4,h);
    ihdr[8]=8;
    ihdr[9]=6;
    int okay=fwrite("\x89PNG\r\n\x1a\n",1,8,file)==8 && !chunk(file,"IHDR",ihdr,13) && !chunk(file,"IDAT",z,pos) && !chunk(file,"IEND",NULL,0);
    free(z);
    return okay ? 0:-1;
}
static const struct gp_guid quality_guid={
    0x1D5BE4B5,0xFA4A,0x452D,{
        0x9C,0xDD,0x5D,0xB3,0x51,0x05,0xE7,0xEB
    }
};
static int jpeg_quality(gp_t parameters,int *quality)
{
    *quality=90;
    if(!parameters) return GP_OK;
    if(!vm_readable(parameters,4)) return GP_INVALID;
    uint32_t count=*(uint32_t *)GP(parameters);
    if(count>32 || !vm_readable(parameters,4+count*28)) return GP_INVALID;
    for(uint32_t i=0;i<count;i++) {
        const uint8_t *p=(const uint8_t *)GP(parameters)+4+i*28;
        if(memcmp(p,&quality_guid,16)) return GP_NOTIMPL;
        uint32_t n=u32(p+16),type=u32(p+20),value=u32(p+24);
        if(n!=1 || type!=4 || !vm_readable(value,4)) return GP_INVALID;
        uint32_t q=*(uint32_t *)GP(value);
        if(q>100) return GP_INVALID;
        *quality=q ? q:1;
    }
    return GP_OK;
}
int gp_save(struct gp_bitmap *b,const char *path,const struct gp_guid *encoder,gp_t parameters)
{
    int format=encoder_format(encoder);
    if(!format) return GP_UNKNOWNFORMAT;
    if(b->lock_data) return GP_BUSY;
    int status=gp_bitmap_sync(b,0);
    if(status) return status;
    int quality=90;
    if(parameters) {
        if(format!=2) return GP_NOTIMPL;
        status=jpeg_quality(parameters,&quality);
        if(status) return status;
    }
    if(format==2) return jpeg_encode_file(path,b->pixels.px,b->pixels.stride,b->pixels.w,b->pixels.h,quality)<0 ? GP_ERROR:GP_OK;
    FILE *file=fopen(path,"wb");
    if(!file) return errno==EACCES ? GP_DENIED:GP_ERROR;
    int result=format==1 ? write_bmp(file,b):write_png(file,b);
    if(fclose(file)) result=-1;
    return result<0 ? GP_ERROR:GP_OK;
}
DEF(GdipSaveImageToFile)
{
    if(!gp_started()) return GP_NOTINITIALIZED;
    struct gp_bitmap *b=gp_bitmap(A(0));
    if(!b || !A(1) || !vm_readable(A(2),16)) return GP_INVALID;
    uint16_t *wide=ui_string(A(1),1);
    int n=wstrlen(wide);
    char *name=malloc((size_t)n*4+1);
    if(!name) {
        free(wide);
        return GP_MEMORY;
    }
    int length=wide_to_utf8(wide,n,name,n*4);
    name[length]=0;
    free(wide);
    char path[PATH_MAX];
    int bad=path_to_host(name,path);
    free(name);
    if(bad<0) return GP_INVALID;
    path_fix_case(path);
    return gp_save(b,path,P(2),A(3));
}
DEF(GdipGetImageEncodersSize) {
    return gp_codec_list(a,1,1);
}
DEF(GdipGetImageDecodersSize) {
    return gp_codec_list(a,0,1);
}
DEF(GdipGetImageEncoders) {
    return gp_codec_list(a,1,0);
}
DEF(GdipGetImageDecoders) {
    return gp_codec_list(a,0,0);
}
DEF(GdipGetEncoderParameterListSize)
{
    if(!gp_started()) return GP_NOTINITIALIZED;
    if(!gp_bitmap(A(0)) || !vm_readable(A(1),16) || !vm_writable(A(2),4)) return GP_INVALID;
    int format=encoder_format(P(1));
    if(!format) return GP_UNKNOWNFORMAT;
    *(uint32_t *)P(2)=format==2 ? 40:4;
    return GP_OK;
}
DEF(GdipGetEncoderParameterList)
{
    uint32_t args[]={A(0),A(1),galloc(4)};
    if(!args[2]) return GP_MEMORY;
    int status=api_GdipGetEncoderParameterListSize(args);
    uint32_t size=*(uint32_t *)GP(args[2]);
    gfree(args[2]);
    if(status) return status;
    if(A(2)<size || !vm_writable(A(3),size)) return GP_INVALID;
    memset(P(3),0,size);
    if(size==40) {
        uint8_t *p=P(3);
        put32(p,1);
        memcpy(p+4,&quality_guid,16);
        put32(p+20,1);
        put32(p+24,6);
        put32(p+28,A(3)+32);
        put32(p+32,0);
        put32(p+36,100);
    }
    return GP_OK;
}
/* IStream::Read is invoked through the guest vtable, never as a native pointer. */
static uint32_t stream_load(uint32_t *a)
{
    if(!gp_started()) return GP_NOTINITIALIZED;
    if(!vm_readable(A(0),4) || !vm_writable(A(1),4)) return GP_INVALID;
    *(uint32_t *)P(1)=0;
    gp_t table=*(gp_t *)P(0);
    if(!vm_readable(table,16)) return GP_INVALID;
    gp_t read=((gp_t *)GP(table))[3];
    if(!read) return GP_INVALID;
    gp_t chunk_data=galloc(16384),read_count=galloc(4);
    if(!chunk_data || !read_count) {if(chunk_data) gfree(chunk_data);if(read_count) gfree(read_count);return GP_MEMORY;}
    uint8_t *data=NULL;
    uint32_t size=0;
    int status=GP_OK;
    for(;;) {
        *(uint32_t *)GP(read_count)=0;
        uint32_t args[]={
            A(0),chunk_data,16384,read_count
        };
        uint32_t hr=call_guest(read,4,args),count=*(uint32_t *)GP(read_count);
        if((int32_t)hr<0 || count>16384 || size+count>64*1024*1024) {
            status=GP_ERROR;
            break;
        }
        if(count) {
            uint8_t *next=realloc(data,size+count);
            if(!next) {
                status=GP_MEMORY;
                break;
            }
            data=next;
            memcpy(data+size,GP(chunk_data),count);
            size+=count;
        }
        if(!count || hr==1) break;
    }
    if(!status) status=gp_decode(data,size,A(1));
    free(data);
    gfree(chunk_data);
    gfree(read_count);
    return status;
}
DEF(GdipLoadImageFromStream) {
    return stream_load(a);
}
DEF(GdipCreateBitmapFromStream) {
    return stream_load(a);
}
DEF(GdipLoadImageFromStreamICM) {
    (void)a;
    return GP_NOTIMPL;
}
DEF(GdipCreateBitmapFromStreamICM) {
    (void)a;
    return GP_NOTIMPL;
}
const struct api_entry gdip_codec_entries[]={
    API(GdipSaveImageToFile,4),API(GdipGetImageEncodersSize,2),API(GdipGetImageEncoders,3),API(GdipGetImageDecodersSize,2),API(GdipGetImageDecoders,3),
    API(GdipGetEncoderParameterListSize,3),API(GdipGetEncoderParameterList,4),API(GdipLoadImageFromStream,2),API(GdipCreateBitmapFromStream,2),API(GdipLoadImageFromStreamICM,2),API(GdipCreateBitmapFromStreamICM,2)
};
const int gdip_codec_count=sizeof(gdip_codec_entries)/sizeof(gdip_codec_entries[0]);
