#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct {int32_t left,top,right,bottom;} RECT;
#include "../user/win32/region.h"
static unsigned seed=7;
static unsigned random_value(void) {seed=seed*1664525+1013904223;return seed;}
static void rectangle(struct region *r)
{
    int x=random_value()%16,y=random_value()%16;
    assert(region_add(r,(RECT){x,y,x+1+(int)(random_value()%8),y+1+(int)(random_value()%8)}));
}
int main(void)
{
    for(int trial=0;trial<200;trial++) {
        struct region a={0},b={0};rectangle(&a);rectangle(&b);
        for(int j=0;j<6;j++) {
            struct region extra={0};rectangle(&extra);
            assert(region_combine(&a,&a,&extra,2));region_free(&extra);
            rectangle(&extra);assert(region_combine(&b,&b,&extra,2));region_free(&extra);
        }
        for(int mode=1;mode<=5;mode++) {
            struct region out={0};assert(region_combine(&out,&a,&b,mode));
            for(int y=0;y<24;y++) for(int x=0;x<24;x++) {
                int av=region_contains(&a,x,y),bv=region_contains(&b,x,y);
                int expected=mode==1?(av&&bv):mode==2?(av||bv):mode==3?(av!=bv):mode==4?(av&&!bv):av;
                assert(region_contains(&out,x,y)==expected);
                int hits=0;for(int i=0;i<out.n;i++) {RECT r=out.rects[i];hits+=x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}
                assert(hits<=1);
            }
            assert(region_type(&out)>=1 && region_type(&out)<=3);
            region_free(&out);
        }
        assert(region_combine(&a,&a,&a,3) && !a.n);region_free(&a);region_free(&b);
    }
    puts("Win32 region algebra regressions passed");
}
