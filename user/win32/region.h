/* Disjoint, half-open rectangles. Used for GDI regions and copied DC clips. */
#ifndef WIN_REGION_H
#define WIN_REGION_H
struct region { RECT *rects; int n; };
static void region_free(struct region *r) { free(r->rects); r->rects=NULL; r->n=0; }
static int region_add(struct region *r, RECT b)
{
    if(b.left>=b.right || b.top>=b.bottom) return 1;
    if(r->n>=4096) return 0;
    RECT *p=realloc(r->rects,(r->n+1)*sizeof(*p));if(!p) return 0;
    r->rects=p;r->rects[r->n++]=b;return 1;
}
static RECT region_intersect(RECT a,RECT b)
{
    return (RECT){a.left>b.left?a.left:b.left,a.top>b.top?a.top:b.top,
                  a.right<b.right?a.right:b.right,a.bottom<b.bottom?a.bottom:b.bottom};
}
static int region_copy(struct region *d,const struct region *s)
{
    struct region out={0};
    for(int i=0;i<s->n;i++) if(!region_add(&out,s->rects[i])) {region_free(&out);return 0;}
    region_free(d);*d=out;return 1;
}
static int region_contains(const struct region *r,int x,int y)
{
    for(int i=0;i<r->n;i++) {RECT b=r->rects[i];if(x>=b.left && x<b.right && y>=b.top && y<b.bottom) return 1;}
    return 0;
}
static RECT region_bounds(const struct region *r)
{
    RECT b={0};if(!r->n) return b;b=r->rects[0];
    for(int i=1;i<r->n;i++) {RECT q=r->rects[i];if(q.left<b.left)b.left=q.left;if(q.top<b.top)b.top=q.top;if(q.right>b.right)b.right=q.right;if(q.bottom>b.bottom)b.bottom=q.bottom;}
    return b;
}
static int region_subtract(struct region *out,RECT b,RECT cut)
{
    RECT c=region_intersect(b,cut);
    if(c.left>=c.right || c.top>=c.bottom) return region_add(out,b);
    return region_add(out,(RECT){b.left,b.top,b.right,c.top}) &&
           region_add(out,(RECT){b.left,c.bottom,b.right,b.bottom}) &&
           region_add(out,(RECT){b.left,c.top,c.left,c.bottom}) &&
           region_add(out,(RECT){c.right,c.top,b.right,c.bottom});
}
static int region_diff(struct region *out,const struct region *a,const struct region *b)
{
    struct region current={0};if(!region_copy(&current,a)) return 0;
    for(int j=0;j<b->n;j++) {
        struct region next={0};
        for(int i=0;i<current.n;i++) if(!region_subtract(&next,current.rects[i],b->rects[j])) {region_free(&next);region_free(&current);return 0;}
        region_free(&current);current=next;
    }
    *out=current;return 1;
}
/* RGN_AND=1, OR=2, XOR=3, DIFF=4, COPY=5. Safe with aliased inputs. */
static int region_combine(struct region *dst,const struct region *a,const struct region *b,int mode)
{
    struct region out={0},other={0};int ok=1;
    if(mode==5) return region_copy(dst,a);
    if(mode==1) {
        for(int i=0;i<a->n && ok;i++) for(int j=0;j<b->n && ok;j++) ok=region_add(&out,region_intersect(a->rects[i],b->rects[j]));
    } else if(mode==4) ok=region_diff(&out,a,b);
    else if(mode==2 || mode==3) {
        ok=region_diff(&out,a,b);
        if(ok) ok=mode==2 ? region_copy(&other,b):region_diff(&other,b,a);
        for(int i=0;i<other.n && ok;i++) ok=region_add(&out,other.rects[i]);
    } else ok=0;
    region_free(&other);
    if(!ok) {region_free(&out);return 0;}
    region_free(dst);*dst=out;return 1;
}
static int region_type(const struct region *r)
{
    if(!r->n) return 1;
    RECT b=region_bounds(r);uint64_t area=0;
    for(int i=0;i<r->n;i++) area+=(uint64_t)((int64_t)r->rects[i].right-r->rects[i].left)*((int64_t)r->rects[i].bottom-r->rects[i].top);
    return area==(uint64_t)((int64_t)b.right-b.left)*((int64_t)b.bottom-b.top)?2:3;
}
#endif
