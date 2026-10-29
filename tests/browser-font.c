#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "font check failed: %s\n", #x);                                                  \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)
int main(int argc, char **argv)
{
    int rc = wfont_init();
    if (argc > 1 && !strcmp(argv[1], "reject"))
        return rc < 0 ? 0 : 1;
    CHECK(rc == 0);
    if(argc>2 && !strcmp(argv[1],"download")){
        FILE *file=fopen(argv[2],"rb");CHECK(file);CHECK(!fseek(file,0,SEEK_END));long len=ftell(file);CHECK(len>0&&len<(8<<20));rewind(file);
        unsigned char *bytes=malloc((size_t)len);CHECK(bytes&&fread(bytes,1,(size_t)len,file)==(size_t)len);fclose(file);
        int family=wfont_register(bytes,(size_t)len,-1,0,0);CHECK(family>=3);
        const struct wfont *font=wfont_get(family,0,0,31);CHECK(font&&wfont_size(font)==31);
        unsigned int pixels[500*100]={0};struct surface surface={.px=pixels,.stride=500,.clip={0,0,500,100}};
        int width=wfont_width(font,"AVATAR Hello",12);CHECK(width>100&&width<500);
        CHECK(wfont_draw(&surface,font,0,0,"AVATAR Hello",12,0xffffff)==width);
        int lit=0;for(int i=0;i<500*100;i++)lit+=pixels[i]!=0;CHECK(lit>100);
        CHECK(wfont_register(bytes,(size_t)len,family,1,0)==family);
        CHECK(wfont_register((const unsigned char *)"broken",6,-1,0,0)<0);
        CHECK(wfont_register(bytes,12,-1,0,0)<0);
        wfont_release(family);free(bytes);puts("browser fonts: scalable font parsing, metrics, kerning and rendering passed");return 0;
    }
    const struct wfont *f = wfont_get(FAM_SANS, 0, 0, 16);
    CHECK(f);
    CHECK(wfont_width(f, "Ж", 2) > wfont_width(f, "?", 1));
    unsigned int pixels[300 * 80] = {0}, before[300 * 80];
    struct surface surface = {0};
    surface.px = pixels;
    surface.stride = 300;
    surface.clip = (struct rect){0, 0, 300, 80};
    wfont_draw(&surface, f, 0, 0, "αЖé", 6, 0xffffff);
    memcpy(before, pixels, sizeof(pixels));
    memset(pixels, 0, sizeof(pixels));
    wfont_draw(&surface, f, 0, 0, "???", 3, 0xffffff);
    CHECK(memcmp(before, pixels, sizeof(pixels)) != 0);
    /* Color emoji: their own colors, not the text's; U+FE0F asks for one. */
    static unsigned int emo[80 * 40];
    struct surface es = {0};
    es.px = emo;
    es.stride = 80;
    es.clip = (struct rect){0, 0, 80, 40};
    int ew = wfont_width(f, "\xF0\x9F\x98\x80", 4);         /* U+1F600 */
    CHECK(ew >= 16 && ew <= 22);
    CHECK(wfont_draw(&es, f, 0, 0, "\xF0\x9F\x98\x80", 4, 0xffffff) == ew);
    int yellow = 0;
    for (int i = 0; i < 80 * 40; i++)
        yellow += (emo[i] >> 16 & 255) > 180 && (emo[i] >> 8 & 255) > 120 && (emo[i] & 255) < 90;
    CHECK(yellow > 20);
    int reds[2];
    for (int k = 0; k < 2; k++) {                            /* U+2764 alone, then with U+FE0F */
        memset(emo, 0, sizeof(emo));
        wfont_draw(&es, f, 0, 0, "\xE2\x9D\xA4\xEF\xB8\x8F", k ? 6 : 3, 0xffffff);
        reds[k] = 0;
        for (int i = 0; i < 80 * 40; i++)
            reds[k] += (emo[i] >> 16 & 255) > 150 && (emo[i] >> 8 & 255) < 80;
    }
    CHECK(reds[0] == 0 && reds[1] > 20);
    puts("browser fonts: Latin, Greek and Cyrillic glyph rendering and color emoji passed");
    return 0;
}
