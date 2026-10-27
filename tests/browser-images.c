/* Real GIF decoder: delayed frames, disposal, looping, truncation and cleanup. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1); } } while(0)
void image_free(struct image *img) {if(img){image_animation_free(img);free(img->px);free(img);}}
static const uint8_t webp_lossless[]={82,73,70,70,32,0,0,0,87,69,66,80,86,80,56,76,19,0,0,0,47,1,0,0,16,15,16,251,63,255,15,252,143,10,21,136,232,127,0,0};
static const uint8_t webp_lossy[]={82,73,70,70,106,0,0,0,87,69,66,80,86,80,56,88,10,0,0,0,16,0,0,0,1,0,0,0,0,0,65,76,80,72,3,0,0,0,0,255,128,0,86,80,56,32,64,0,0,0,240,1,0,157,1,42,2,0,1,0,0,0,0,37,160,2,116,186,0,3,9,6,253,236,0,254,255,255,126,119,255,172,80,127,244,176,255,240,2,184,232,69,255,112,102,125,93,142,31,255,156,88,255,249,197,143,255,156,88,255,243,127,128,0};
static uint8_t data[2048];
static unsigned len;
static void byte(int x){data[len++]=(uint8_t)x;}
static void begin(int loops){
    const uint8_t header[]={ 'G','I','F','8','9','a',2,0,1,0,0x81,0,0,
        255,0,0,0,0,255,0,255,0,0,0,0 };
    len=sizeof(header);memcpy(data,header,len);
    if(loops>=0){const uint8_t app[]={0x21,0xff,11,'N','E','T','S','C','A','P','E','2','.','0',3,1,0,0,0};
        memcpy(data+len,app,sizeof(app));data[len+16]=(uint8_t)loops;len+=sizeof(app);}
}
static void frame(int x,int width,int first,int second,int disposal,int transparent){
    const uint8_t gce[]={0x21,0xf9,4,0,5,0,3,0};
    memcpy(data+len,gce,sizeof(gce));data[len+3]=(uint8_t)(disposal*4+(transparent?1:0));len+=sizeof(gce);
    byte(0x2c);byte(x);byte(0);byte(0);byte(0);byte(width);byte(0);byte(1);byte(0);byte(0);
    /* A clear before each literal keeps all codes at three bits. */
    unsigned bits=4u | (unsigned)first<<3;int nbits=6;
    if(width==2){bits|=4u<<nbits;nbits+=3;bits|=(unsigned)second<<nbits;nbits+=3;}
    bits|=5u<<nbits;nbits+=3;
    byte(2);byte((nbits+7)/8);while(nbits>0){byte(bits&255);bits>>=8;nbits-=8;}byte(0);
}
int main(void){
    begin(-1);frame(0,2,0,0,1,0);frame(1,1,1,0,3,0);frame(0,1,2,0,2,0);frame(1,1,1,0,1,1);byte(0x3b);
    struct image *img=image_decode_gif(data,len);CHECK(img && img->w==2 && img->h==1);
    CHECK(img->px[0]==0xffff0000u && img->px[1]==0xffff0000u);
    CHECK(!image_animate(img,100) && !image_animate(img,149));
    CHECK(image_animate(img,150) && img->px[0]==0xffff0000u && img->px[1]==0xff0000ffu);
    CHECK(image_animate(img,200) && img->px[0]==0xff00ff00u && img->px[1]==0xffff0000u);
    CHECK(image_animate(img,250) && img->px[0]==0xffff0000u && img->px[1]==0xff0000ffu);
    CHECK(!image_animate(img,300) && !image_animate(img,400));image_free(img);
    begin(1);frame(0,2,0,0,1,0);frame(0,2,1,1,1,0);byte(0x3b);
    img=image_decode_gif(data,len);CHECK(img);image_animate(img,0);
    CHECK(image_animate(img,50) && img->px[0]==0xff0000ffu);
    CHECK(image_animate(img,100) && img->px[0]==0xffff0000u);
    CHECK(image_animate(img,150));CHECK(!image_animate(img,200));image_free(img);
    for(unsigned i=0;i<40 && i<len;i++){img=image_decode_gif(data,i);if(img)image_free(img);}
    img=image_decode_webp(webp_lossless,sizeof(webp_lossless));
    CHECK(img && img->w==2 && img->h==1 && img->px[0]==0xffff0000u && img->px[1]==0x800000ffu);
    image_free(img);
    img=image_decode_webp(webp_lossy,sizeof(webp_lossy));
    CHECK(img && img->w==2 && img->h==1 && (img->px[0]>>24)==255 && (img->px[1]>>24)==128);
    image_free(img);
    for(unsigned i=0;i<sizeof(webp_lossless);i++) {
        img=image_decode_webp(webp_lossless,i);if(img)image_free(img);
    }
    puts("browser images: GIF timing, disposal, finite loops, WebP alpha and malformed blocks passed");
}
