#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"vector/text:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
int wfont_init(void){return 0;}
const struct wfont *wfont_get(int a,int b,int c,int d){return NULL;}
int wfont_width(const struct wfont *f,const char *s,int n){return n*8;}
int wfont_ascent(const struct wfont *f){return 12;}
int wfont_height(const struct wfont *f){return 16;}
int wfont_draw(struct surface *s,const struct wfont *f,int x,int y,const char *t,int n,color_t c){return n*8;}
void image_free(struct image *i){if(i){free(i->px);free(i);}}
static struct image *decode(const char *xml){return image_decode_svg((const unsigned char *)xml,(unsigned)strlen(xml));}
int main(void)
{
    struct image *i=decode("<svg width='20' height='20'><defs><clipPath id='c'><rect width='10' height='20'/></clipPath></defs><rect width='20' height='20' fill='red' clip-path='url(#c)'/></svg>");
    CHECK(i && i->px[5*20+5]==0xffff0000u && !i->px[5*20+15]);image_free(i);
    i=decode("<svg width='20' height='20'><defs><clipPath id='c'><circle cx='10' cy='10' r='6'/></clipPath></defs><g style='clip-path:url(#c)'><rect width='20' height='20' fill='blue'/></g></svg>");
    CHECK(i && i->px[10*20+10]==0xff0000ffu && !i->px[0]);image_free(i);
    i=decode("<svg width='20' height='20'><defs><mask id='m'><rect width='10' height='20' fill='white'/><rect x='10' width='10' height='20' fill='black'/></mask></defs><rect width='20' height='20' fill='red' mask='url(#m)'/></svg>");
    CHECK(i && i->px[5*20+5]==0xffff0000u && !i->px[5*20+15]);image_free(i);
    int n;char *text=web_text_visual("abc",3,&n);CHECK(!text && n==3);
    text=web_text_visual("\xd8\xa8\xd8\xa8",4,&n);CHECK(text && !strcmp(text,"\xef\xba\x90\xef\xba\x91"));free(text);
    text=web_text_visual("\xd9\x84\xd8\xa7",4,&n);CHECK(text && !strcmp(text,"\xef\xbb\xbb"));free(text);
    text=web_text_visual("\xd7\x90\xd7\x91 12 \xd7\x92\xd7\x93",12,&n);
    CHECK(text && !strcmp(text,"\xd7\x93\xd7\x92 12 \xd7\x91\xd7\x90"));free(text);
    puts("browser vector/text: SVG clipping, group clipping, luminance masks, Arabic joining, lam-alef and RTL digits passed");
}
