/* Independent GDI+ ABI regressions, built without a Windows SDK. */
#include "winapi.h"
#define ARGB 0x26200A
#define RGB24 0x21808
#define PARGB 0xE200B
static int failures;
#define CHECK(x) do {if(!(x)) {DWORD n;const char text[]="gdiplus FAIL: " #x "\n";W(WriteFile)(W(GetStdHandle)(-11),text,sizeof(text)-1,&n,0);failures++;}}while(0)
struct guid {DWORD a;unsigned short b,c;unsigned char d[8];};
struct codec {struct guid clsid,format;const unsigned short *dll,*description,*extension,*mime;DWORD flags,version,sig_count,sig_size;const void *signature,*mask;};
struct data {UINT w,h;int stride;DWORD format;unsigned char *scan;UINT reserved;};
struct rect {int x,y,w,h;};
static int same_wide(const unsigned short *a,const unsigned short *b) {while(*a && *a==*b) {a++;b++;}return *a==*b;}
static unsigned char stream_bytes[8192];static UINT stream_length;
struct stream {void **table;UINT offset;};
static LONG WINAPI read_stream(struct stream *s,void *buffer,UINT requested,UINT *done)
{
    UINT count=stream_length-s->offset;if(count>requested) count=requested;
    unsigned char *out=buffer;for(UINT i=0;i<count;i++) out[i]=stream_bytes[s->offset++];*done=count;
    return count<requested ? 1:0;
}
static DWORD pixel(HANDLE image,int x,int y) {DWORD color=0;CHECK(W(GdipBitmapGetPixel)(image,x,y,&color)==0);return color;}
void start(void)
{
    DWORD token=0,input[4]={1,0,0,0};HANDLE image=0,copy=0,graphics=0;
    CHECK(W(GdipCreateBitmapFromScan0)(2,2,0,ARGB,0,&image)==18);
    input[0]=9;CHECK(W(GdiplusStartup)(&token,input,0)==17);input[0]=1;CHECK(!W(GdiplusStartup)(&token,input,0) && token);
    CHECK(W(GetProcAddress)(W(GetModuleHandleA)("gdiplus.dll"),(const char *)609)==(void *)W(GdiplusStartup));
    DWORD extra_token,hook_input[4]={1,0,1,0};void *hooks[2]={0};CHECK(!W(GdiplusStartup)(&extra_token,hook_input,hooks));
    CHECK(hooks[0] && hooks[1]);UINT (WINAPI *notify)(DWORD *)=hooks[0];void (WINAPI *unnotify)(DWORD)=hooks[1];DWORD notification;CHECK(!notify(&notification));unnotify(notification);W(GdiplusShutdown)(extra_token);
    CHECK(W(GdipCreateBitmapFromScan0)(-1,2,0,ARGB,0,&image)==2);
    CHECK(!W(GdipCreateBitmapFromScan0)(2,2,0,ARGB,0,&image));
    CHECK(!W(GdipBitmapSetPixel)(image,0,0,0x80FF0000));CHECK(!W(GdipBitmapSetPixel)(image,1,0,0xFF00FF00));CHECK(pixel(image,0,0)==0x80FF0000);
    CHECK(W(GdipBitmapGetPixel)(image,2,0,&token)==2);
    struct data data;struct rect region={0,0,1,1};CHECK(!W(GdipBitmapLockBits)(image,&region,3,PARGB,&data));
    CHECK(data.w==1 && data.h==1 && data.stride==4 && data.scan[2]==128 && data.scan[3]==128);
    CHECK(W(GdipBitmapSetPixel)(image,0,0,0)==4);CHECK(W(GdipDisposeImage)(image)==4);CHECK(W(GdipBitmapLockBits)(image,0,1,ARGB,&data)==4);
    data.scan[0]=64;data.scan[1]=0;data.scan[2]=0;data.scan[3]=128;CHECK(!W(GdipBitmapUnlockBits)(image,&data));CHECK(pixel(image,0,0)==0x8000007F);
    CHECK(W(GdipBitmapUnlockBits)(image,&data)==2);
    DWORD external[4]={0xFF010203,0xFF040506,0xFF070809,0xFF0A0B0C};HANDLE shared;
    CHECK(!W(GdipCreateBitmapFromScan0)(2,2,-8,ARGB,external+2,&shared));CHECK(pixel(shared,0,0)==0xFF070809 && pixel(shared,0,1)==0xFF010203);
    external[2]=0xFF112233;CHECK(pixel(shared,0,0)==0xFF112233);CHECK(!W(GdipBitmapSetPixel)(shared,1,1,0xFF556677) && external[1]==0xFF556677);
    CHECK(!W(GdipCloneImage)(shared,&copy));CHECK(!W(GdipDisposeImage)(shared));CHECK(pixel(copy,1,1)==0xFF556677);
    CHECK(!W(GdipImageRotateFlip)(copy,1));CHECK(pixel(copy,0,0)==0xFF010203);
    CHECK(!W(GdipDisposeImage)(copy));CHECK(W(GdipGetImageWidth)(copy,&token)==2);
    DWORD borrowed[4]={0};struct data user={2,2,8,ARGB,(void *)borrowed,0};CHECK(!W(GdipBitmapLockBits)(image,0,7,ARGB,&user));borrowed[0]=0xFFAA5500;CHECK(!W(GdipBitmapUnlockBits)(image,&user));CHECK(pixel(image,0,0)==0xFFAA5500);
    struct rect invalid={-1,0,2,1};CHECK(W(GdipBitmapLockBits)(image,&invalid,1,ARGB,&data)==2);
    CHECK(!W(GdipCreateBitmapFromScan0)(4,4,0,ARGB,0,&copy));CHECK(!W(GdipGetImageGraphicsContext)(copy,&graphics));CHECK(W(GdipDisposeImage)(copy)==4);
    CHECK(!W(GdipGraphicsClear)(graphics,0xFF0000FF));CHECK(W(GdipSetInterpolationMode)(graphics,7)==6);CHECK(!W(GdipSetInterpolationMode)(graphics,5));CHECK(!W(GdipBitmapSetPixel)(image,0,0,0x80FF0000));
    CHECK(!W(GdipDrawImageRect)(graphics,image,0.f,0.f,2.f,2.f));CHECK(pixel(copy,0,0)==0xFF80007F);
    CHECK(!W(GdipSetCompositingMode)(graphics,1));CHECK(!W(GdipDrawImageRectI)(graphics,image,0,0,2,2));CHECK(pixel(copy,0,0)==0x80FF0000);
    HANDLE brush;CHECK(!W(GdipCreateSolidFill)(0xFF00FFFF,&brush));CHECK(!W(GdipTranslateWorldTransform)(graphics,1.f,1.f,0));CHECK(!W(GdipFillRectangleI)(graphics,brush,0,0,1,1));CHECK(pixel(copy,1,1)==0xFF00FFFF);
    CHECK(!W(GdipResetWorldTransform)(graphics));CHECK(!W(GdipDeleteBrush)(brush));CHECK(W(GdipDeleteBrush)(brush)==2);CHECK(!W(GdipDeleteGraphics)(graphics));
    CHECK(!W(GdipDisposeImage)(copy));
    HBITMAP bitmap;CHECK(!W(GdipCreateHBITMAPFromBitmap)(image,&bitmap,0xFF0000FF));CHECK(!W(GdipCreateBitmapFromHBITMAP)(bitmap,0,&copy));CHECK(pixel(copy,0,0)==0xFF80007F);CHECK(!W(GdipDisposeImage)(copy));
    HDC dc=W(CreateCompatibleDC)(0);HGDIOBJ old=W(SelectObject)(dc,bitmap);CHECK(!W(GdipCreateFromHDC)(dc,&graphics));CHECK(!W(GdipGraphicsClear)(graphics,0xFF0000FF));CHECK(!W(GdipDrawImageRectI)(graphics,image,0,0,2,2));CHECK(W(GetPixel)(dc,0,0)==RGB(128,0,127));
    HDC borrowed_dc;CHECK(!W(GdipGetDC)(graphics,&borrowed_dc) && borrowed_dc==dc);CHECK(W(GdipGraphicsClear)(graphics,0)==4);CHECK(!W(GdipReleaseDC)(graphics,dc));CHECK(!W(GdipDeleteGraphics)(graphics));W(SelectObject)(dc,old);W(DeleteObject)(bitmap);W(DeleteDC)(dc);
    UINT count,size;CHECK(!W(GdipGetImageEncodersSize)(&count,&size) && count==3 && size>3*sizeof(struct codec));
    struct codec *codecs=W(GdipAlloc)(size);CHECK(codecs);CHECK(W(GdipGetImageEncoders)(count,size-1,codecs)==2);CHECK(!W(GdipGetImageEncoders)(count,size,codecs));
    const unsigned short *paths[]={u"C:\\gdip-test.bmp",u"C:\\gdip-test.jpg",u"C:\\gdip-test.png"};
    for(UINT i=0;i<count;i++) {
        CHECK(codecs[i].mime && codecs[i].extension && codecs[i].sig_count==1 && (codecs[i].flags&1));
        CHECK(!W(GdipSaveImageToFile)(image,paths[i],&codecs[i].clsid,0));
        HANDLE loaded=0;CHECK(!W(GdipLoadImageFromFile)(paths[i],&loaded));UINT width=0,height=0;CHECK(!W(GdipGetImageWidth)(loaded,&width) && width==2);CHECK(!W(GdipGetImageHeight)(loaded,&height) && height==2);
        struct guid format;CHECK(!W(GdipGetImageRawFormat)(loaded,&format) && format.a==codecs[i].format.a);
        if(i==2) CHECK(pixel(loaded,0,0)==0x80FF0000);else if(i==0) CHECK(pixel(loaded,0,0)==0xFFFF0000);
        CHECK(!W(GdipDisposeImage)(loaded));
    }
    CHECK(same_wide(codecs[2].mime,u"image/png"));
    UINT paramsize;CHECK(!W(GdipGetEncoderParameterListSize)(image,&codecs[1].clsid,&paramsize) && paramsize==40);void *params=W(GdipAlloc)(paramsize);CHECK(!W(GdipGetEncoderParameterList)(image,&codecs[1].clsid,paramsize,params));DWORD *quality=params;quality[6]=4;quality[7]=(DWORD)(quality+8);quality[8]=95;
    CHECK(!W(GdipSaveImageToFile)(image,u"C:\\gdip-quality.jpg",&codecs[1].clsid,params));quality[8]=101;
    CHECK(W(GdipSaveImageToFile)(image,u"C:\\gdip-quality.jpg",&codecs[1].clsid,params)==2);W(GdipFree)(params);
    struct guid bad={0};CHECK(W(GdipSaveImageToFile)(image,u"C:\\bad.png",&bad,0)==13);
    HANDLE file=W(CreateFileA)("C:\\gdip-test.png",0x80000000,3,0,3,0,0);CHECK(file!=(HANDLE)-1);CHECK(W(ReadFile)(file,stream_bytes,sizeof(stream_bytes),&stream_length,0));W(CloseHandle)(file);
    void *table[14]={0};table[3]=read_stream;struct stream stream={table,0};CHECK(!W(GdipLoadImageFromStream)(&stream,&copy));CHECK(pixel(copy,0,0)==0x80FF0000);CHECK(!W(GdipDisposeImage)(copy));
    stream.offset=stream_length;CHECK(W(GdipLoadImageFromStream)(&stream,&copy)==13);
    W(GdipFree)(codecs);CHECK(!W(GdipGetImageDecodersSize)(&count,&size) && count==4);codecs=W(GdipAlloc)(size);CHECK(!W(GdipGetImageDecoders)(count,size,codecs));CHECK(same_wide(codecs[3].mime,u"image/gif") && !(codecs[3].flags&1));W(GdipFree)(codecs);
    CHECK(!W(GdipDisposeImage)(image));W(GdiplusShutdown)(token);CHECK(W(GdipCreateBitmapFromScan0)(1,1,0,ARGB,0,&image)==18);
    W(ExitProcess)(failures ? 1:21);
}
