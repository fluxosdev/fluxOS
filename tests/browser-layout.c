/* Lay pages out with the production engine and a fake fixed-width font (half the size in pixels wide,
 * 5/4 of it high), and check where things end up. No display is needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct wfont { int px; };
static struct wfont fonts[256];
const struct wfont *wfont_get(int family, int bold, int italic, int px)
{ (void)family; (void)bold; (void)italic; if (px < 1) px = 1; if (px > 255) px = 255; fonts[px].px = px; return &fonts[px]; }
int wfont_height(const struct wfont *f) { return f->px * 5 / 4; }
int wfont_ascent(const struct wfont *f) { return f->px; }
int wfont_size(const struct wfont *f) { return f->px; }
int wfont_width(const struct wfont *f, const char *s, int n)
{ int w = 0; for (int i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) w += f->px / 2; return w; }
int wfont_draw(struct surface *s, const struct wfont *f, int x, int y, const char *str, int n, color_t c)
{ (void)s; (void)x; (void)y; (void)str; (void)c; return wfont_width(f, str, n); }
struct syscolors sys;
int rect_empty(struct rect r) { return r.w <= 0 || r.h <= 0; }
struct rect rect_intersect(struct rect a, struct rect b) {
 int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
 int right=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w,bottom=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;
 return mkrect(x,y,right>x?right-x:0,bottom>y?bottom-y:0);
}
void gfx_bevel(struct surface *s, struct rect r, enum bevel st) { (void)s; (void)r; (void)st; }
void gfx_fill(struct surface *s, int x, int y, int w, int h, color_t c) {
 struct rect r=rect_intersect(mkrect(x,y,w,h),s->clip);
 for(int j=r.y;j<r.y+r.h;j++)for(int i=r.x;i<r.x+r.w;i++)s->px[j*s->stride+i]=c;
}
void gfx_focus_rect(struct surface *s, struct rect r) { (void)s; (void)r; }
void gfx_dither(struct surface *s, int x, int y, int w, int h, color_t c) { (void)s; (void)x; (void)y; (void)w; (void)h; (void)c; }
void gfx_frame(struct surface *s, int x, int y, int w, int h, color_t c) { (void)s; (void)x; (void)y; (void)w; (void)h; (void)c; }
void gfx_hline(struct surface *s, int x, int y, int w, color_t c) { (void)s; (void)x; (void)y; (void)w; (void)c; }
void gfx_vline(struct surface *s, int x, int y, int h, color_t c) { (void)s; (void)x; (void)y; (void)h; (void)c; }
void gfx_pixel(struct surface *s, int x, int y, color_t c) { (void)s; (void)x; (void)y; (void)c; }
void image_free(struct image *i) { (void)i; }
void svg_size(const struct node *n, int *w, int *h) { (void)n; *w = 300; *h = 150; }
struct image *svg_render(const struct node *n, int w, int h, uint32_t c) { (void)n; (void)w; (void)h; (void)c; return NULL; }

struct page {
    struct node *root;
    struct arena a, sa, la;
    struct layout lo;
};

/* Pictures the engine would have fetched: every background picture and <img> is this 40x20 one. */
static struct image test_img = { 40, 20, NULL };

/* What scripts would have done before the page is styled (opened popovers and dialogs), or NULL. */
static void (*before_style)(struct node *root);

static void load(struct page *p, const char *html, int vw, int vh)
{
    memset(p, 0, sizeof(*p));
    struct node *root = p->root = html_parse(&p->a, html, (int)strlen(html));
    if (before_style)
        before_style(root);
    struct sheet *sheets[4];
    int ns = 0;
    for (struct node *n = root; n; n = node_next(n, root))
        if (n->type == N_ELEMENT && n->tag == T_STYLE && n->first && ns < 4)
            sheets[ns++] = css_parse(&p->a, n->first->text, n->first->len, vw);
    css_viewport_h = vh;
    /* as browser.c does: styled again while container queries see new container sizes */
    for (int round = 0; round < 3; round++) {
    if (round) {
        arena_free(&p->sa);
        arena_free(&p->la);
    }
    css_cascade(&p->sa, root, sheets, ns, vw, vh, NULL, NULL);
    for (struct node *n = root; n; n = node_next(n, root)) {
        if (n->type != N_ELEMENT)
            continue;
        if (n->style && n->style->bg_url)
            n->style->bg_img = &test_img;
        if (n->style)
            for (struct style *l = n->style->bg_layers; l; l = l->bg_next)
                if (l->bg_url) l->bg_img = &test_img;
        if (n->style && n->style->ls_url)
            n->style->ls_img = &test_img;
        if (n->tag == T_IMG)
            n->img = &test_img;
    }
    layout_document(&p->la, &p->lo, root, vw, vh);
    if (!layout_cq_changed)
        break;
    }
}

static void unload(struct page *p)
{
    arena_free(&p->a);
    arena_free(&p->sa);
    arena_free(&p->la);
}

/* The first text item that is exactly this word. */
static const struct ditem *word(const struct page *p, const char *w)
{
    for (int i = 0; i < p->lo.nitems; i++) {
        const struct ditem *d = &p->lo.items[i];
        if (d->kind == DK_TEXT && d->len == (int)strlen(w) && !memcmp(d->text, w, (size_t)d->len))
            return d;
    }
    return NULL;
}

static int fills(const struct page *p, uint32_t color)
{
    int n = 0;
    for (int i = 0; i < p->lo.nitems; i++)
        n += p->lo.items[i].kind == DK_FILL && p->lo.items[i].color == color;
    return n;
}

static int dy_of(const struct page *p, const struct ditem *d, int sx, int sy)
{
    int dx = 0, dy = 0;
    if (d->dyn)
        layout_dyn_offset(&p->lo, d->dyn, sx, sy, &dx, &dy);
    return d->y + dy;
}

static void text_flow(void)
{
    struct page p;
    load(&p, "<style>body{margin:0} p{margin:0} .ls{letter-spacing:2px} .ws{word-spacing:10px}"
             " .ba{word-break:break-all;width:80px} .bw{overflow-wrap:break-word;width:80px}"
             " .el{width:100px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
             " .sc{font-variant:small-caps}</style>"
             "<p class=ls>abc def</p><p class=ws>aa bb</p><p class=ba>abcdefghijklmnop</p>"
             "<p class=bw>abcdefghijklmnopqrstuvwxyz</p><p class=el>this is a long text that overflows</p>"
             "<p class=sc>Hello</p>", 800, 600);
    const struct ditem *abc = word(&p, "abc"), *def = word(&p, "def");
    CHECK(abc && def && abc->w == 30 && def->x == 40);              /* each character and the space gain 2 */
    const struct ditem *aa = word(&p, "aa"), *bb = word(&p, "bb");
    CHECK(aa && bb && bb->x == aa->w + 8 + 10);
    const struct ditem *a = word(&p, "a"), *k = word(&p, "k");     /* ten characters to an 80 pixel line */
    CHECK(a && k && k->y == a->y + a->h + 0 && k->x == 0);
    CHECK(word(&p, "abcdefghij") && word(&p, "klmnopqrst") && word(&p, "uvwxyz"));
    const struct ditem *dots = word(&p, "...");
    CHECK(dots && dots->x + dots->w <= 100);
    CHECK(word(&p, "ELLO") && word(&p, "ELLO")->h < word(&p, "H")->h);   /* small capitals are smaller */
    unload(&p);
}

static void positioning(void)
{
    struct page p;
    load(&p, "<style>body{margin:0} div{margin:0} .head{position:sticky;top:10px;height:20px;background:#111111}"
             " .sec{height:300px} .fix{position:fixed;right:5px;bottom:5px;width:100px;height:20px;background:#222222}"
             "</style><div class=sec><div class=head>HEAD</div><div>text</div></div>"
             "<div style='height:400px'>other</div><div class=fix>FIX</div>", 800, 600);
    const struct ditem *head = word(&p, "HEAD"), *fix = word(&p, "FIX");
    CHECK(head && fix && head->dyn && fix->dyn);
    CHECK(dy_of(&p, head, 0, 0) == 10);                 /* held 10 pixels from the top at once */
    CHECK(dy_of(&p, head, 0, 50) == 60);                /* then following the window */
    CHECK(dy_of(&p, head, 0, 200) == 210);
    CHECK(dy_of(&p, head, 0, 295) == 280);              /* stopping at the bottom of its container */
    CHECK(dy_of(&p, head, 0, 700) == 280);
    CHECK(dy_of(&p, fix, 0, 0) == 575 && dy_of(&p, fix, 0, 123) == 575 + 123);   /* stays in the window */
    CHECK(p.lo.height < 800);                           /* a fixed box does not stretch the page */
    /* hit testing follows the movement */
    unload(&p);
    load(&p, "<style>body{margin:0}.fix{position:fixed;left:0;top:0;width:100px;height:20px}</style>"
             "<div style='height:2000px'><a class=fix href='x.html'>go</a></div>", 800, 600);
    CHECK(p.lo.nlinks == 1);
    CHECK(layout_hit(&p.lo, p.lo.links, p.lo.nlinks, 5, 500 + 5, 0, 500) != NULL);   /* at the top of the window */
    CHECK(layout_hit(&p.lo, p.lo.links, p.lo.nlinks, 5, 5, 0, 500) == NULL);          /* where it was laid out */
    unload(&p);
}

static void boxes(void)
{
    struct page p;
    load(&p, "<style>body{margin:0}.ol{outline:2px solid #00ff00;outline-offset:3px;width:50px;height:20px;margin:10px}"
             " .tf{table-layout:fixed;width:300px} .ins{list-style-position:inside}</style>"
             "<div class=ol>x</div><ul class=ins><li>item</li></ul>"
             "<table class=tf><tr><td style='width:50px'>a</td><td>bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb</td>"
             "<td>c</td></tr></table><ruby>kan<rt>ji</rt></ruby>", 800, 600);
    CHECK(fills(&p, OPAQUE(0x00ff00)) == 4);            /* the four strips round the box */
    const struct ditem *item = word(&p, "item"), *x = word(&p, "x");
    CHECK(item && item->x > 40);                        /* the marker takes room in the line (inside) */
    CHECK(x && x->x == 10);
    const struct ditem *c = word(&p, "c");
    CHECK(c && c->x < 300);                             /* the long cell did not widen the fixed table */
    const struct ditem *ji = word(&p, "ji"), *kan = word(&p, "kan");
    CHECK(ji && kan && ji->y + ji->h <= kan->y + 2 && ji->h < kan->h);   /* the annotation sits above */
    unload(&p);
}


static const struct ditem *first_of(const struct page *p, int kind)
{
    for (int i = 0; i < p->lo.nitems; i++)
        if (p->lo.items[i].kind == kind)
            return &p->lo.items[i];
    return NULL;
}

static void background_pictures(void)
{
    struct page p;
    load(&p, "<style>body{margin:0} div{width:100px;height:60px}"
             " .a{background:url(a.png) no-repeat center / 20px 10px}"
             " .b{background:#fff url('b b.png') center/cover}"
             " .c{background-image:url(c.png);background-size:contain;background-position:right bottom}"
             " .d{background:url(d.png) repeat-x 5px 7px;border:3px solid red}"
             " .e{background:linear-gradient(red, blue), url(e.png)}"
             " .f{background:red}</style>"
             "<div class=a></div><div class=b></div><div class=c></div><div class=d></div><div class=e></div>"
             "<div class=f></div>", 800, 600);
    int n = 0;
    const struct ditem *pic[8];
    for (int i = 0; i < p.lo.nitems; i++)
        if (p.lo.items[i].kind == DK_BGIMG && p.lo.items[i].bgp)
            pic[n++] = &p.lo.items[i];
    CHECK(n == 5);                                              /* none for the plain red one */
    CHECK(pic[0]->bgp->tw == 20 && pic[0]->bgp->th == 10);      /* an exact size */
    CHECK(pic[0]->bgp->tx == 40 && pic[0]->bgp->ty == 25);      /* centered in 100 x 60 */
    CHECK(pic[0]->bgp->norep_x && pic[0]->bgp->norep_y);
    CHECK(pic[1]->bgp->tw == 120 && pic[1]->bgp->th == 60);     /* 2:1 picture covering 100 x 60 */
    CHECK(pic[1]->bgp->tx == -10 && pic[1]->bgp->ty == 0);      /* ...centered */
    CHECK(!pic[1]->bgp->norep_x);
    CHECK(pic[2]->bgp->tw == 100 && pic[2]->bgp->th == 50);     /* contained */
    CHECK(pic[2]->bgp->tx == 0 && pic[2]->bgp->ty == 10);       /* in the bottom right corner */
    CHECK(pic[3]->bgp->tw == 40 && pic[3]->bgp->th == 20);      /* its own size */
    CHECK(pic[3]->bgp->tx == 3 + 5 && pic[3]->bgp->ty == 3 + 7);   /* from the padding box */
    CHECK(!pic[3]->bgp->norep_x && pic[3]->bgp->norep_y);       /* repeat-x */
    CHECK(pic[4]->bgp->img && pic[4]->bgp->next && pic[4]->bgp->next->gr);
    unload(&p);

    /* the old attribute, and a data: URL that is not cut short */
    load(&p, "<body background='img/tile.gif'>x</body>", 800, 600);
    CHECK(first_of(&p, DK_BGIMG) && first_of(&p, DK_BGIMG)->bgp);
    unload(&p);
}

static void background_pixels(void)
{
    struct page p;
    load(&p, "<style>body{margin:0}div{width:4px;height:4px;background-image:url(top),url(bottom)}</style><div></div>",8,8);
    struct node *box=node_find(p.root,T_DIV);
    uint32_t red=0x80ff0000u,blue=0xff0000ffu;
    struct image top={1,1,&red},bottom={1,1,&blue};
    box->style->bg_layers->bg_img=&top;
    box->style->bg_layers->bg_next->bg_img=&bottom;
    arena_free(&p.la);
    layout_document(&p.la,&p.lo,p.root,8,8);
    uint32_t pixels[64]={0};
    struct surface surface={pixels,8,8,8,{0,0,8,8}};
    paint_document(&p.lo,&surface,0,0,0,0,NULL,0);
    CHECK(pixels[0]==0x80007f && pixels[3*8+3]==0x80007f && pixels[4]==0xffffff);
    unload(&p);
    load(&p,"<style>body{margin:0}div{width:4px;height:4px;padding:1px;border:1px solid red;"
            "background:blue;background-clip:content-box}</style><div></div>",12,12);
    uint32_t clipped[144]={0};
    surface=(struct surface){clipped,12,12,12,{0,0,12,12}};
    paint_document(&p.lo,&surface,0,0,0,0,NULL,0);
    CHECK(clipped[1*12+1]==0xffffff && clipped[2*12+2]==0x0000ff);
    unload(&p);
}

/* opacity: a box's items put down that much fainter, nested boxes multiplying. */
static void opacity_pixels(void)
{
    struct page p;
    load(&p,"<style>body{margin:0}div{width:4px;height:4px;background:red;opacity:.5}"
            "#outer{background:transparent;opacity:.5;width:8px}#outer div{opacity:1;background:blue}</style>"
            "<div></div><div id='outer'><div></div></div>",8,8);
    uint32_t pixels[64]={0};
    struct surface surface={pixels,8,8,8,{0,0,8,8}};
    paint_document(&p.lo,&surface,0,0,0,0,NULL,0);
    CHECK(pixels[0]==0xff8080 || pixels[0]==0xff7f7f);
    CHECK(pixels[5*8+1]==0x7f7fff || pixels[5*8+1]==0x8080ff);
    CHECK(pixels[4]==0xffffff);
    unload(&p);
    load(&p,"<style>body{margin:0}.g{opacity:.5;position:relative;width:8px;height:8px}.r{position:absolute;left:0;top:0;width:8px;height:8px;background:red}"
            ".b{position:absolute;left:2px;top:0;width:4px;height:8px;background:blue}</style><div class=g><div class=r></div><div class=b></div></div>",8,8);
    paint_document(&p.lo,&surface,0,0,0,0,NULL,0);
    CHECK((pixels[3]&0xff0000)>=0x7f0000 && (pixels[3]&0xff0000)<=0x800000);
    CHECK((pixels[3]&0xff00)>=0x7f00 && (pixels[3]&0xff00)<=0x8000 && (pixels[3]&255)==255);
    unload(&p);

}

/* Stacking: positioned boxes over the flow, z-index among siblings and inside contexts. */
static uint32_t stack_pixel(const char *html, int x, int y)
{
    struct page p;
    load(&p, html, 16, 16);
    uint32_t pixels[256] = {0};
    struct surface surface = {pixels, 16, 16, 16, {0, 0, 16, 16}};
    paint_document(&p.lo, &surface, 0, 0, 0, 0, NULL, 0);
    unload(&p);
    return pixels[y * 16 + x];
}

static void stacking(void)
{
    /* An absolute menu paints over the content that follows it. */
    CHECK(stack_pixel("<style>body{margin:0}</style><div style='position:relative'>"
                      "<div style='position:absolute;top:0;left:0;width:4px;height:4px;background:red'></div></div>"
                      "<div style='width:8px;height:8px;background:blue'></div>", 1, 1) == 0xff0000);
    /* z-index on relative boxes, against document order. */
    CHECK(stack_pixel("<style>body{margin:0}div{position:relative;width:4px;height:4px}</style>"
                      "<div style='z-index:2;background:red'></div><div style='top:-4px;z-index:1;background:blue'></div>",
                      1, 1) == 0xff0000);
    /* A child's z-index counts only inside its parent's stacking context. */
    CHECK(stack_pixel("<style>body{margin:0}div{position:relative;width:4px;height:4px}</style>"
                      "<div style='z-index:1'><div style='z-index:100;background:red'></div></div>"
                      "<div style='top:-4px;z-index:2;background:blue'></div>", 1, 1) == 0x0000ff);
    /* A negative z-index goes under the flow, but over its context's own background. */
    CHECK(stack_pixel("<style>body{margin:0}</style><div style='position:relative;z-index:0;background:yellow;"
                      "width:8px;height:8px'><div style='position:absolute;z-index:-1;width:4px;height:4px;"
                      "background:red'></div></div>", 1, 1) == 0xff0000);
    CHECK(stack_pixel("<style>body{margin:0}</style><div style='position:absolute;z-index:-1;width:4px;height:4px;"
                      "background:red'></div><div style='width:8px;height:8px;background:blue'></div>", 1, 1) == 0x0000ff);
    /* Flex items take a z-index without being positioned, and their transforms apply. */
    CHECK(stack_pixel("<style>body{margin:0}.f{display:flex}.f div{width:4px;height:4px}</style><div class=f>"
                      "<div style='z-index:2;background:red'></div><div style='margin-left:-4px;background:blue'></div>"
                      "</div>", 1, 1) == 0xff0000);
    CHECK(stack_pixel("<style>body{margin:0}.f{display:flex}.f div{width:4px;height:4px;background:red}</style>"
                      "<div class=f><div style='transform:translateX(6px)'></div></div>", 7, 1) == 0xff0000);
    CHECK(stack_pixel("<style>body{margin:0}.f{display:flex}.f div{width:4px;height:4px;background:red}</style>"
                      "<div class=f><div style='transform:translateX(6px)'></div></div>", 1, 1) == 0xffffff);
}

static void named_grid_lines(void)
{
    struct page p;
    load(&p,"<style>body{margin:0}.g{display:grid;width:160px;"
            "grid-template-columns:[edge] 30px [content center] 80px [end] 50px [edge];"
            "grid-template-rows:[top] 20px [body] 40px [bottom]}"
            ".a{grid-column:content / end;grid-row:body / bottom}</style>"
            "<div class=g><div class=a>A</div></div>",200,200);
    CHECK(word(&p,"A") && word(&p,"A")->x==30 && word(&p,"A")->y==20);
    unload(&p);
    load(&p,"<style>body{margin:0}.g{display:grid;width:80px;grid-template-columns:repeat(2, [start] 40px [end])}"
            ".a{grid-column:start 2 / end 2}.b{grid-column:start -1 / end -1}</style>"
            "<div class=g><div class=a>A</div><div class=b>B</div></div>",200,200);
    CHECK(word(&p,"A") && word(&p,"A")->x==40);
    CHECK(word(&p,"B") && word(&p,"B")->x==40);
    unload(&p);
}

static void inside_marker_lifetime(void)
{
    struct page p;
    load(&p,"<ol style='list-style-position:inside'><li>one</li><li>two</li></ol>",200,200);
    CHECK(word(&p,"1.") && word(&p,"2."));
    CHECK(word(&p,"1.")->text != word(&p,"2.")->text);
    unload(&p);
}

static void text_extras(void)
{
    struct page p;
    load(&p, "<style>body{margin:0} p{margin:0;width:200px} .tl{text-align:left;text-align-last:right}"
             " pre{margin:0;tab-size:4} .clamp{width:100px;-webkit-line-clamp:2;line-height:10px}"
             " ul{margin:0;padding:0;list-style-image:url(m.png)} li{margin:0}</style>"
             "<p class=tl>last</p><pre>a\tb</pre>"
             "<div class=clamp>aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj</div><ul><li>item</li></ul>", 800, 600);
    const struct ditem *last = word(&p, "last");
    CHECK(last && last->x + last->w == 200);                    /* text-align-last: the only line is the last */
    const struct ditem *a = word(&p, "a"), *b = word(&p, "b");
    CHECK(a && b && b->x - a->x >= 4 * 4 - 1);                  /* a tab stop four characters on, not eight */
    const struct ditem *item = word(&p, "item");
    CHECK(item && item->x == 0);
    int img_markers = 0;
    for (int i = 0; i < p.lo.nitems; i++)
        img_markers += p.lo.items[i].kind == DK_IMAGE && p.lo.items[i].x < 0;
    CHECK(img_markers == 1);                                    /* the picture stands for the bullet */
    const struct ditem *eeee = word(&p, "eeee");
    CHECK(eeee && eeee->sb);                                    /* the third line is in a clipped box: line-clamp: 2 */
    unload(&p);
}

static void zoom(void)
{
    struct page p;
    css_zoom = 200;
    load(&p, "<style>body{margin:0} p{margin:0;font-size:10px;width:50px;border:1px solid red;line-height:normal}"
             " .m{font-size:2em;margin-left:1em}</style><p>aa</p><p class=m>bb</p><img>", 800, 600);
    css_zoom = 100;
    const struct ditem *aa = word(&p, "aa"), *bb = word(&p, "bb");
    CHECK(aa && aa->w == 20 && aa->x == 2);                     /* 10px text is 20 (the fake font is half the size wide); the border 2 */
    CHECK(bb && bb->w == 64 && bb->x == 2 + 64);                /* 2em of the body's zoomed 32 is 64; the margin is 1em of that */
    int img_w = 0;
    for (int i = 0; i < p.lo.nitems; i++)
        if (p.lo.items[i].kind == DK_IMAGE)
            img_w = p.lo.items[i].w;
    CHECK(img_w == 80);                                         /* the picture's own 40 pixels, doubled */
    unload(&p);
}

static void ranges(void)
{
    struct arena a = { 0 };
    const char *html = "<input id=r type=range min=0 max=10 step=2 value=4><input id=p type=range>"
                       "<input id=d type=range min=-1 max=1 step=0.25 value=0.5>";
    struct node *root = html_parse(&a, html, (int)strlen(html)), *r = NULL, *p = NULL, *d = NULL;
    for (struct node *n = root; n; n = node_next(n, root)) {
        const char *id = n->type == N_ELEMENT ? node_attr(n, "id") : NULL;
        if (id && *id == 'r') r = n;
        if (id && *id == 'p') p = n;
        if (id && *id == 'd') d = n;
    }
    CHECK(r && p && d);
    r->value = strdup("4");
    d->value = strdup("0.5");
    char v[32];
    CHECK(range_fraction(r) == 400 && range_fraction(p) == 500 && range_fraction(d) == 750);
    range_value_at(r, 0, v, sizeof(v));
    CHECK(!strcmp(v, "0"));
    range_value_at(r, 1000, v, sizeof(v));
    CHECK(!strcmp(v, "10"));
    range_value_at(r, 430, v, sizeof(v));                       /* 4.3 snaps to the step of 2 */
    CHECK(!strcmp(v, "4"));
    range_value_at(r, 560, v, sizeof(v));
    CHECK(!strcmp(v, "6"));
    range_value_at(d, 600, v, sizeof(v));                       /* -1 + 0.2 * 2: nearest quarter is -0.25... */
    CHECK(!strcmp(v, "0.25"));
    range_nudge(r, 1, v, sizeof(v));
    CHECK(!strcmp(v, "6"));
    range_nudge(r, -2, v, sizeof(v));
    CHECK(!strcmp(v, "0"));
    range_nudge(r, 2, v, sizeof(v));
    CHECK(!strcmp(v, "10"));
    range_nudge(d, -1, v, sizeof(v));
    CHECK(!strcmp(v, "0.25"));
    CHECK(input_pattern("date") && !strcmp(input_pattern("time"), "hh:mm") && !input_pattern("text"));
    arena_free(&a);
}

static void object_fit(void)
{
    struct page p;
    load(&p, "<style>body{margin:0} img{width:100px;height:100px;display:block}"
             " .c{object-fit:cover} .n{object-fit:contain} .z{object-fit:none}"
             " .s{object-fit:scale-down} .t{object-fit:cover;object-position:left top}"
             " .r{object-fit:contain;object-position:right bottom}</style>"
             "<img class=c><img class=n><img class=z><img class=s><img class=t><img class=r><img>", 800, 600);
    const struct ditem *im[8];
    int n = 0;
    for (int i = 0; i < p.lo.nitems; i++)
        if (p.lo.items[i].kind == DK_IMAGE)
            im[n++] = &p.lo.items[i];
    CHECK(n == 7);
    CHECK(im[0]->fit_w == 200 && im[0]->fit_h == 100 && im[0]->fit_x == -50 && im[0]->fit_y == 0);   /* cover */
    CHECK(im[1]->fit_w == 100 && im[1]->fit_h == 50 && im[1]->fit_x == 0 && im[1]->fit_y == 25);     /* contain */
    CHECK(im[2]->fit_w == 40 && im[2]->fit_h == 20 && im[2]->fit_x == 30 && im[2]->fit_y == 40);     /* none */
    CHECK(im[3]->fit_w == 40 && im[3]->fit_h == 20);                                                 /* scale-down */
    CHECK(im[4]->fit_w == 200 && im[4]->fit_x == 0 && im[4]->fit_y == 0);                            /* left top */
    CHECK(im[5]->fit_w == 100 && im[5]->fit_y == 50);                                                /* right bottom */
    CHECK(im[6]->fit_w == 0);                                                                        /* fill */
    unload(&p);
}

static void scrolling_boxes(void)
{
    struct page p;
    load(&p, "<style>body{margin:0} .box{width:100px;height:50px;overflow:auto}"
             " .tall{height:100px} a{display:block;height:20px}"
             " .hid{width:100px;height:30px;overflow:hidden} .fit{width:100px;height:50px;overflow:auto}"
             " .sc{width:100px;height:40px;overflow:scroll}</style>"
             "<div class=box><div class=tall>top</div><a href='x.html'>go</a></div>"
             "<div class=hid><div style='height:90px'>cut</div></div>"
             "<div class=fit><div style='height:30px'>fits</div></div>"
             "<div class=sc>s</div><p>after</p>", 800, 600);
    CHECK(p.lo.nsb == 4);
    const struct sbox *box = &p.lo.sb[0], *hid = &p.lo.sb[1], *fit = &p.lo.sb[2], *sc = &p.lo.sb[3];
    CHECK(box->active && box->bar_y && !box->bar_x);
    CHECK(box->w == 100 && box->h == 50 && box->ch == 120 && box->max_y == 120 - 50);
    CHECK(hid->active && !hid->bar_y && hid->max_y == 60);      /* clipped, not scrollable by hand */
    CHECK(!fit->active);                                        /* room to spare: nothing to clip */
    CHECK(sc->active && sc->bar_x && sc->bar_y && sc->max_x == 0 && sc->max_y == 0);   /* bars with nothing to scroll */
    CHECK(p.lo.height < 250 && word(&p, "after")->y == 170 + 16);   /* what is clipped does not stretch the page */
    const struct ditem *cut = word(&p, "cut");
    CHECK(cut && cut->sb == 2);
    CHECK(word(&p, "after") && word(&p, "after")->sb == 0);

    /* the link is below the box's 50 pixels until the box is scrolled down */
    CHECK(p.lo.nlinks == 1 && p.lo.links[0].sb == 1);
    CHECK(layout_hit(&p.lo, p.lo.links, p.lo.nlinks, 5, 110, 0, 0) == NULL);
    CHECK(layout_hit(&p.lo, p.lo.links, p.lo.nlinks, 5, 30, 0, 0) == NULL);
    struct node *bn = box->node;
    CHECK(layout_set_node_scroll(&p.lo, bn, 0, 80));
    CHECK(box->sy == 70);                                       /* clamped to what scrolls */
    CHECK(layout_hit(&p.lo, p.lo.links, p.lo.nlinks, 5, 40, 0, 0) != NULL);   /* the link is at 100 - 70 = 30..50 */
    CHECK(layout_hit(&p.lo, p.lo.links, p.lo.nlinks, 5, 110, 0, 0) == NULL);
    int dx, dy;
    struct rect clip;
    layout_sb_view(&p.lo, cut->sb, 0, 0, &dx, &dy, &clip);
    CHECK(dx == 0 && dy == 0 && clip.h == 30);                  /* a box that only clips does not move things */

    /* the wheel goes to the box under the pointer while it can move, then to the page */
    CHECK(layout_wheel(&p.lo, 10, 10, 0, 0, 0, -30) && box->sy == 40);
    CHECK(layout_wheel(&p.lo, 10, 10, 0, 0, 0, -100) && box->sy == 0);
    CHECK(!layout_wheel(&p.lo, 10, 10, 0, 0, 0, -30));          /* at the top: the page's turn */
    CHECK(!layout_wheel(&p.lo, 10, 60, 0, 0, 0, 30));           /* a box that only clips takes nothing */
    CHECK(layout_wheel(&p.lo, 10, 10, 0, 0, 0, 30) && box->sy == 30);

    /* its scroll bar */
    struct sbhit h;
    CHECK(layout_sbar_hit(&p.lo, 95, 25, 0, 0, &h) && h.box == 0 && h.axis == 1);
    CHECK(h.part == SBP_THUMB || h.part == SBP_PAGE1 || h.part == SBP_PAGE2);
    CHECK(layout_sbar_hit(&p.lo, 95, 3, 0, 0, &h) && h.part == SBP_ARROW1);
    CHECK(layout_sbar_press(&p.lo, &h) && box->sy == 0);        /* a step up (to the top) */
    CHECK(layout_sbar_hit(&p.lo, 95, 47, 0, 0, &h) && h.part == SBP_ARROW2);
    CHECK(layout_sbar_press(&p.lo, &h) && box->sy == 40);
    CHECK(!layout_sbar_hit(&p.lo, 50, 25, 0, 0, &h));           /* not over the bar */
    unload(&p);

    /* scroll positions survive laying the page out again */
    load(&p, "<style>body{margin:0} .box{width:100px;height:50px;overflow:auto}</style>"
             "<div class=box id=b><div style='height:300px'>x</div></div>", 800, 600);
    struct node *b = NULL;
    for (struct node *n = p.lo.sb[0].node; n; n = NULL)
        b = n;
    CHECK(b && layout_set_node_scroll(&p.lo, b, 0, 120));
    {
        struct arena la2 = { 0 };
        struct layout lo2;
        struct node *root = b;
        while (root->parent)
            root = root->parent;
        layout_document(&la2, &lo2, root, 800, 600);
        CHECK(lo2.nsb == 1 && lo2.sb[0].sy == 120);
        arena_free(&la2);
    }
    unload(&p);

    /* a flex item that scrolls */
    load(&p, "<style>body{margin:0} .row{display:flex;height:80px} .side{width:50px;overflow:auto}"
             " .main{flex:1}</style><div class=row><div class=side><div style='height:300px'>s</div></div>"
             "<div class=main>m</div></div>", 800, 600);
    CHECK(p.lo.nsb == 1 && p.lo.sb[0].h == 80 && p.lo.sb[0].w == 50 && p.lo.sb[0].bar_y);
    CHECK(word(&p, "s") && word(&p, "s")->sb == 1 && word(&p, "m") && word(&p, "m")->sb == 0);
    unload(&p);

    /* one inside another: the inner box's clip is moved by the outer one's scrolling */
    load(&p, "<style>body{margin:0} .o{width:100px;height:100px;overflow:auto}"
             " .i{width:80px;height:40px;overflow:auto;margin-top:100px}</style>"
             "<div class=o><div class=i><div style='height:200px'>in</div></div></div>", 800, 600);
    CHECK(p.lo.nsb == 2);
    const struct ditem *in = word(&p, "in");
    CHECK(in && in->sb == 1 && p.lo.sb[0].parent == 2);           /* the inner box is box 0, in box 1 */
    CHECK(p.lo.sb[1].max_y == 40);                                /* the outer one scrolls to show the inner */
    CHECK(layout_set_node_scroll(&p.lo, p.lo.sb[1].node, 0, 30) && layout_set_node_scroll(&p.lo, p.lo.sb[0].node, 0, 20));
    layout_sb_view(&p.lo, in->sb, 0, 0, &dx, &dy, &clip);
    CHECK(dy == -30 - 20);                                        /* moved by both */
    CHECK(clip.y == 100 - 30 && clip.h == 40 - 10);               /* the inner box, cut off by the outer's edge */
    unload(&p);
}

static void shadow_layout(void)
{
    struct page p;
    load(&p,
         "<!doctype html><html><body><div id=host><span slot=title>shown</span><span "
         "slot=unused>hidden</span></div></body></html>",
         400, 300);
    struct node *host = node_find(p.root, T_DIV), *light = host->first;
    const char *text = "<style>span{color:blue}</style><span>shadow</span><slot name=title>fallback</slot>";
    struct node *parsed = html_parse(&p.a, text, (int)strlen(text));
    struct node *body = node_find(parsed, T_BODY), *head = node_find(parsed, T_HEAD);
    struct node *shadow = arena_alloc(&p.a, sizeof(*shadow));
    shadow->type = N_ELEMENT;
    shadow->name = "#fragment";
    shadow->first = head->first;
    shadow->last = head->last;
    if (shadow->last)
        shadow->last->next = body->first;
    else
        shadow->first = body->first;
    if (body->first)
        body->first->prev = shadow->last;
    if (body->last)
        shadow->last = body->last;
    for (struct node *k = shadow->first; k; k = k->next)
        k->parent = shadow;
    host->shadow_root = shadow;
    shadow->shadow_host = host;
    struct sheet *sheet = css_parse(&p.a, "span{color:red}", 15, 400);
    css_cascade(&p.sa, p.root, &sheet, 1, 400, 300, NULL, NULL);
    arena_free(&p.la);
    layout_document(&p.la, &p.lo, p.root, 400, 300);
    CHECK(word(&p, "shadow") && word(&p, "shown") && !word(&p, "hidden") && !word(&p, "fallback"));
    CHECK(host->first == light && light->parent == host && shadow->first->parent == shadow);
    CHECK((light->style->color & 0xFFFFFF) == 0xFF0000);
    CHECK((node_find(shadow, T_SPAN)->style->color & 0xFFFFFF) == 0x0000FF);
    unload(&p);
}

static int font_count;
static void found_font(void *ctx,const struct css_fontface *face)
{
    CHECK(ctx==&font_count);CHECK(!strcmp(face->family,"Test Font"));CHECK(face->bold&&face->italic);
    CHECK(face->nsources==2&&!strcmp(face->sources[0],"a;}.woff2")&&!strcmp(face->sources[1],"fallback.woff"));font_count++;
}
static int find_font(void *ctx,const char *family){return !strcmp(family,"test font")?19:-1;}
static void font_faces(void)
{
    const char *css="/* @font-face{bad} */ @media (min-width: 500px){@font-face{font-family:'Test Font';font-weight:700;font-style:italic;src:local('absent'),url('a;}.woff2') format('woff2'),url(fallback.woff)}} @media (max-width: 10px){@font-face{font-family:ignore;src:url(no)}}";
    css_font_faces(css,(int)strlen(css),800,600,found_font,&font_count);CHECK(font_count==1);
    struct page p;css_font_lookup=find_font;load(&p,"<div style=\"font-family:Test Font,serif\">x</div>",800,600);
    CHECK(node_find(p.root,T_DIV)->style->family==19);unload(&p);css_font_lookup=NULL;
}


/* The picture items of a page, in order. */
static int picture_items(const struct page *p, const struct ditem **out, int max)
{
    int n = 0;
    for (int i = 0; i < p->lo.nitems && n < max; i++)
        if (p->lo.items[i].kind == DK_IMAGE)
            out[n++] = &p->lo.items[i];
    return n;
}

static void canvas_layout(void)
{
    struct page p;
    const struct ditem *it[4];
    load(&p, "<style>body{margin:0} canvas{display:block}</style>"
             "<canvas></canvas><canvas width=200 height=50></canvas><canvas style='width:100px'></canvas>"
             "<canvas width=0 height=10></canvas><canvas width=40 style='max-width:20px'></canvas>", 800, 600);
    CHECK(picture_items(&p, it, 4) == 0);                       /* nothing is drawn on them yet */
    struct node *c[5];
    int k = 0;
    for (struct node *n = p.root; n; n = node_next(n, p.root))
        if (n->type == N_ELEMENT && n->tag == T_CANVAS && k < 5)
            c[k++] = n;
    CHECK(k == 5);
    static struct image bitmap = { 1, 1, NULL };
    for (int i = 0; i < 5; i++)
        c[i]->img = &bitmap;
    arena_free(&p.la);
    layout_document(&p.la, &p.lo, p.root, 800, 600);
    int n = picture_items(&p, it, 4);
    CHECK(n == 4);                                              /* the one with no width has no box */
    CHECK(it[0]->w == 300 && it[0]->h == 150 && it[0]->y == 0);     /* the default size */
    CHECK(it[1]->w == 200 && it[1]->h == 50 && it[1]->y == 150);    /* what the attributes say */
    CHECK(it[2]->w == 100 && it[2]->h == 50 && it[2]->y == 200);    /* a CSS width keeps the aspect ratio */
    CHECK(it[3]->w == 20 && it[3]->h == 75);                        /* max-width scales it down */
    unload(&p);
}

/* Chinese and Japanese text breaks after any character, but not before closing punctuation. */
static void cjk_wrapping(void)
{
    struct page p;
    load(&p, "<style>body{margin:0} div{width:60px;font-size:20px}</style>"
             "<div>\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\xe4\xbd\xa0\xe5\xa5\xbd\xe3\x80\x82</div>"
             "<div>\xe4\xbd\xa0\xe5\xa5\xbd</div>", 800, 600);
    const struct ditem *first = word(&p, "\xe4\xbd\xa0"), *last = word(&p, "\xe5\xa5\xbd\xe3\x80\x82");
    CHECK(first && last);
    CHECK(last->y > first->y);                                  /* wrapped, though there is not one space */
    CHECK(last->x == 0);                                        /* the full stop did not start a line: 好 went with it */
    int lines = 0, y = -1;
    for (int i = 0; i < p.lo.nitems; i++)
        if (p.lo.items[i].kind == DK_TEXT && p.lo.items[i].y != y && p.lo.items[i].y < 40)
            lines++, y = p.lo.items[i].y;
    CHECK(lines == 2);
    unload(&p);
}

static const struct ditem *colored(const struct page *p,uint32_t color)
{
    for(int i=0;i<p->lo.nitems;i++)if(p->lo.items[i].kind==DK_FILL && p->lo.items[i].color==color)return &p->lo.items[i];
    return NULL;
}
static struct rect visual_item(const struct page *p,const struct ditem *d)
{ CHECK(d);return layout_transform_rect(&p->lo,d->xf,mkrect(d->x,d->y,d->w,d->h),0,0); }
static void expect_rect(struct rect r,int x,int y,int w,int h)
{ if(r.x!=x || r.y!=y || r.w!=w || r.h!=h)fprintf(stderr,"rect got %d,%d %dx%d expected %d,%d %dx%d\n",r.x,r.y,r.w,r.h,x,y,w,h);CHECK(r.x==x && r.y==y && r.w==w && r.h==h); }
static void transforms_2d(void)
{
    struct page p;uint32_t pixels[128*128];
    struct surface surface={pixels,128,128,128,{0,0,128,128}};
    const char *functions[]={"scale(2)","translate(30px,10px) scale(2)","scale(2) translate(30px,10px)",
        "translate(40px,10px) rotate(90deg)","matrix(0,1,-1,0,40,10)","translate(40px,10px) scale(-1,1)",
        "skewX(45deg)","translate(50%,100%)"};
    const int expected[][4]={{0,0,40,20},{30,10,40,20},{60,20,40,20},{30,10,10,20},{30,10,10,20},
        {20,10,20,10},{0,0,30,10},{10,10,20,10}};
    for(unsigned k=0;k<sizeof(functions)/sizeof(functions[0]);k++) {
        char html[1024];snprintf(html,sizeof(html),"<style>body{margin:0}#box{width:20px;height:10px;background:red;transform-origin:0 0;transform:%s}#after{width:1px;height:1px;background:blue}</style><div id=box></div><div id=after></div>",functions[k]);
        load(&p,html,128,128);
        const struct ditem *d=colored(&p,0xffff0000);CHECK(d && d->xf && d->w==20 && d->h==10);
        const struct ditem *after=colored(&p,0xff0000ff);CHECK(after && after->y==10); /* normal flow is unchanged */
        struct rect r=visual_item(&p,d);expect_rect(r,expected[k][0],expected[k][1],expected[k][2],expected[k][3]);
        struct rect node_box=layout_node_visual_rect(&p.lo,d->node,mkrect(d->x,d->y,d->w,d->h),0,0);
        expect_rect(node_box,r.x,r.y,r.w,r.h);
        paint_document(&p.lo,&surface,0,0,0,0,NULL,0);
        int cx=r.x+r.w/2,cy=r.y+r.h/2;
        CHECK(pixels[cy*128+cx]==0xff0000);
        CHECK(layout_box_contains(&p.lo,d->xf,d->dyn,d->sb,mkrect(d->x,d->y,d->w,d->h),cx,cy,0,0));
        unload(&p);
    }
    load(&p,"<style>body{margin:0}div{position:absolute;left:40px;top:10px;width:20px;height:10px;background:red;transform:rotate(90deg)}</style><div></div>",128,128);
    expect_rect(visual_item(&p,colored(&p,0xffff0000)),45,5,10,20);unload(&p);
    load(&p,"<style>body{margin:0}.outer{width:80px;height:50px;transform:translate(30px,20px)}.inner{width:20px;height:10px;background:red;transform-origin:0 0;transform:rotate(.25turn)}</style><div class=outer><div class=inner></div></div>",128,128);
    const struct ditem *d=colored(&p,0xffff0000);expect_rect(visual_item(&p,d),20,20,10,20);
    paint_document(&p.lo,&surface,0,0,0,0,NULL,0);CHECK(pixels[25*128+25]==0xff0000 && pixels[5*128+5]==0xffffff);unload(&p);
    load(&p,"<style>body{margin:0}button{margin:0;padding:0;border:0;width:20px;height:10px;background:red;transform-origin:0 0;transform:translate(40px,10px) rotate(90deg)}</style><button></button>",128,128);
    CHECK(p.lo.ncontrols>=1 && p.lo.controls[0].xf);
    const struct hotspot *control=&p.lo.controls[0];
    struct rect control_box=layout_transform_rect(&p.lo,control->xf,mkrect(control->x,control->y,control->w,control->h),0,0);
    CHECK(layout_hit(&p.lo,p.lo.controls,p.lo.ncontrols,control_box.x+control_box.w/2,control_box.y+control_box.h/2,0,0));
    CHECK(!layout_hit(&p.lo,p.lo.controls,p.lo.ncontrols,control->x+control->w/2,control->y+control->h/2,0,0));unload(&p);
    load(&p,"<style>body{margin:0}div{width:20px;height:10px;background:red;transform:scale(0)}</style><div></div>",128,128);
    d=colored(&p,0xffff0000);paint_document(&p.lo,&surface,0,0,0,0,NULL,0);
    CHECK(pixels[5*128+5]==0xffffff && !layout_box_contains(&p.lo,d->xf,0,0,mkrect(0,0,20,10),5,5,0,0));unload(&p);
    load(&p,"<style>body{margin:0}.outer{width:20px;height:20px;overflow:hidden}.inner{width:20px;height:10px;background:red;transform:translate(15px,5px)}</style><div class=outer><div class=inner></div></div>",128,128);
    d=colored(&p,0xffff0000);paint_document(&p.lo,&surface,0,0,0,0,NULL,0);
    CHECK(pixels[8*128+17]==0xff0000 && pixels[8*128+25]==0xffffff);
    CHECK(layout_box_contains(&p.lo,d->xf,d->dyn,d->sb,mkrect(d->x,d->y,d->w,d->h),17,8,0,0));
    CHECK(!layout_box_contains(&p.lo,d->xf,d->dyn,d->sb,mkrect(d->x,d->y,d->w,d->h),25,8,0,0));unload(&p);
    load(&p,"<style>body{margin:0}.outer{width:20px;height:20px;overflow:hidden;transform-origin:0 0;transform:translate(40px,10px) rotate(90deg)}.inner{width:20px;height:10px;background:red;transform:translate(15px,5px)}</style><div class=outer><div class=inner></div></div>",128,128);
    d=colored(&p,0xffff0000);paint_document(&p.lo,&surface,0,0,0,0,NULL,0);
    CHECK(pixels[27*128+32]==0xff0000 && pixels[35*128+32]==0xffffff);
    CHECK(!layout_box_contains(&p.lo,d->xf,d->dyn,d->sb,mkrect(d->x,d->y,d->w,d->h),32,35,0,0));unload(&p);
    /* Translucent pixels blend against the real destination after transformation. */
    load(&p,"<style>body{margin:0;background:blue}div{width:10px;height:10px;background:rgba(255,0,0,.5);transform-origin:0 0;transform:scale(2)}</style><div></div>",128,128);
    paint_document(&p.lo,&surface,0,0,0,0,NULL,0);unsigned alpha=node_find(p.root,T_DIV)->style->bg>>24;CHECK(pixels[5*128+5]==(alpha<<16 | (255-alpha)));unload(&p);
    /* A transform captures fixed descendants, even through a positioned wrapper. */
    load(&p,"<style>body{margin:0}.outer{margin-top:20px;width:80px;height:50px;transform:translateX(30px)}.wrap{position:relative}.fixed{position:fixed;left:0;top:0;width:20px;height:10px;background:red}</style><div class=outer><div class=wrap><div class=fixed></div></div></div>",128,128);
    d=colored(&p,0xffff0000);CHECK(d && !d->dyn);expect_rect(visual_item(&p,d),30,20,20,10);unload(&p);
    puts("browser transforms: order, origins, nested matrices, raster pixels, alpha, clipping, hit testing and flow passed");
}

static void multicolumn(void)
{
    struct page p;
    /* Nine one-line paragraphs (20 pixels each) in three balanced columns, 1em (16 pixel) gaps. */
    load(&p, "<style>body{margin:0} p{margin:0} .mc{column-count:3;width:600px} .after{margin:0}</style>"
             "<div class=mc><p>w1</p><p>w2</p><p>w3</p><p>w4</p><p>w5</p><p>w6</p><p>w7</p><p>w8</p>"
             "<p><a href=x>w9</a></p></div><p class=after>after</p>", 800, 600);
    const struct ditem *w1 = word(&p, "w1"), *w3 = word(&p, "w3"), *w4 = word(&p, "w4"), *w7 = word(&p, "w7"),
                       *w9 = word(&p, "w9");
    CHECK(w1 && w3 && w4 && w7 && w9);
    CHECK(w1->x == 0 && w1->y == 0 && w3->x == 0 && w3->y == 40);
    CHECK(w4->x == 189 + 16 && w4->y == 0 && w7->x == 2 * (189 + 16) && w7->y == 0 && w9->y == 40);
    CHECK(word(&p, "after")->y == 60);                              /* the box is as tall as a column */
    CHECK(p.lo.nlinks == 1 && p.lo.links[0].x == w9->x && p.lo.links[0].y == w9->y);
    unload(&p);

    /* column-width: as many 150 pixel columns as fit in 500 with 10 pixel gaps, sharing what is left. */
    load(&p, "<style>body{margin:0} p{margin:0} .mc{column-width:150px;column-gap:10px;width:500px}</style>"
             "<div class=mc><p>a</p><p>b</p><p>c</p></div>", 800, 600);
    CHECK(word(&p, "a")->x == 0 && word(&p, "b")->x == 170 && word(&p, "c")->x == 340 && word(&p, "c")->y == 0);
    unload(&p);
    /* ...but no more than column-count; the columns shorthand sets both. */
    load(&p, "<style>body{margin:0} p{margin:0} .mc{columns:2 100px;column-gap:0;width:500px}</style>"
             "<div class=mc><p>a</p><p>b</p><p>c</p><p>d</p></div>", 800, 600);
    CHECK(word(&p, "c")->x == 250 && word(&p, "c")->y == 0 && word(&p, "d")->y == 20);
    unload(&p);

    /* break-inside: avoid keeps a three-line box whole; break-before: column starts a column. */
    load(&p, "<style>body{margin:0} p,div{margin:0} .mc{column-count:2;column-gap:0;width:400px}"
             " .keep{break-inside:avoid} .brk{break-before:column}</style>"
             "<div class=mc><p>a</p><p>b</p><div class=keep>k1<br>k2<br>k3</div><p>c</p></div>"
             "<div class=mc><p>x</p><p class=brk>y</p><p>z</p></div>", 800, 600);
    const struct ditem *k1 = word(&p, "k1"), *k3 = word(&p, "k3");
    CHECK(k1 && k3 && k1->x == k3->x && k3->y == k1->y + 40);
    CHECK(word(&p, "x")->x == 0 && word(&p, "y")->x == 200 && word(&p, "y")->y == word(&p, "x")->y &&
          word(&p, "z")->x == 200);
    unload(&p);

    /* A rule in each gap, as tall as the columns; a set height with column-fill: auto fills one column,
     * then the next, and what is left over runs on past the box's width. */
    load(&p, "<style>body{margin:0} p{margin:0} .mc{column-count:2;column-gap:20px;width:420px;"
             "column-rule:4px solid #123456} .fa{column-count:2;column-gap:0;width:200px;height:40px;"
             "column-fill:auto}</style><div class=mc><p>a</p><p>b</p><p>c</p><p>d</p></div>"
             "<div class=fa><p>e</p><p>f</p><p>g</p><p>h</p><p>i</p></div>", 800, 600);
    const struct ditem *rule = NULL;
    for (int i = 0; i < p.lo.nitems; i++)
        if (p.lo.items[i].kind == DK_FILL && p.lo.items[i].color == OPAQUE(0x123456))
            rule = &p.lo.items[i];
    CHECK(rule && rule->x == 200 + 10 - 2 && rule->w == 4 && rule->y == 0 && rule->h == 40);
    const struct ditem *e = word(&p, "e"), *g = word(&p, "g"), *i5 = word(&p, "i");
    CHECK(e && g && i5 && g->x == 100 && g->y == e->y && i5->x == 200 && i5->y == e->y);
    unload(&p);

    /* A box with a background that runs over several columns stops at the end of the first. */
    load(&p, "<style>body{margin:0} p{margin:0} .mc{column-count:2;column-gap:0;width:400px}"
             " ul{margin:0;padding:0;background:#654321;list-style:none}</style>"
             "<div class=mc><ul><li>a</li><li>b</li><li>c</li><li>d</li></ul></div>", 800, 600);
    const struct ditem *bg = NULL;
    for (int i = 0; i < p.lo.nitems; i++)
        if (p.lo.items[i].kind == DK_FILL && p.lo.items[i].color == OPAQUE(0x654321))
            bg = &p.lo.items[i];
    CHECK(bg && bg->y == 0 && bg->h == 40 && word(&p, "c")->x == 200 && word(&p, "c")->y == 0);
    unload(&p);
}

static struct node *with_class(struct node *root, const char *cls)
{
    for (struct node *n = root; n; n = node_next(n, root))
        if (n->type == N_ELEMENT && node_attr(n, "class") && !strcmp(node_attr(n, "class"), cls))
            return n;
    CHECK(0);
    return NULL;
}

static int box_width(const struct page *p, uint32_t color)
{
    const struct ditem *d = colored(p, color);
    CHECK(d);
    return d->w;
}

/* 3D transforms seen flat, env(), image-set(), @container, cq units, @scope and @property. */
static void modern_css(void)
{
    struct page p;
    const char *functions[] = { "translate3d(30px,10px,0)", "translate3d(-100%,0,5px)", "translateZ(9px) scale3d(2,2,1)",
        "perspective(500px) rotateZ(90deg)", "rotateX(60deg)", "rotateY(60deg)", "matrix3d(1,0,0,0,0,1,0,0,0,0,1,0,30,10,0,1)",
        "rotate3d(0,0,1,90deg)" };
    const int expected[][4] = { {30,10,20,10}, {-20,0,20,10}, {0,0,40,20}, {5,-5,10,20}, {0,2,20,6}, {5,0,10,10},
        {30,10,20,10}, {5,-5,10,20} };
    for (unsigned k = 0; k < sizeof(functions) / sizeof(functions[0]); k++) {
        char html[512];
        snprintf(html, sizeof(html), "<style>body{margin:0}#box{width:20px;height:10px;background:red;"
                 "transform-origin:%s;transform:%s}</style><div id=box></div>", k < 3 || k == 6 ? "0 0" : "50% 50%",
                 functions[k]);
        load(&p, html, 128, 128);
        const struct ditem *d = colored(&p, 0xffff0000);
        CHECK(d && d->xf);
        expect_rect(visual_item(&p, d), expected[k][0], expected[k][1], expected[k][2], expected[k][3]);
        unload(&p);
    }
    /* A card turned over hides its front when the back face is not to be seen. */
    load(&p, "<style>.f{width:20px;height:10px;background:red;transform:rotateY(180deg);backface-visibility:hidden}"
             ".b{width:20px;height:10px;background:blue;transform:rotateY(180deg)}"
             ".r{width:20px;height:10px;background:lime;rotate:y 180deg;-webkit-backface-visibility:hidden}</style>"
             "<div class=f></div><div class=b></div><div class=r></div>", 128, 128);
    CHECK(!colored(&p, 0xffff0000) && colored(&p, 0xff0000ff) && !colored(&p, 0xff00ff00));
    unload(&p);

    /* env(): no safe areas here; unknown names take their fallback. image-set(): the 1x picture. */
    load(&p, "<style>body{margin:0}.a{width:max(30px, env(safe-area-inset-left));height:1px;background:red}"
             ".b{width:env(no-such-thing, 44px);height:1px;background:blue}"
             ".c{width:calc(10px + env(safe-area-inset-right, 99px));height:1px;background:lime}"
             ".d{background-image:image-set(\"hi.png\" 2x, \"lo.png\" 1x);width:5px;height:5px}"
             ".e{background-image:-webkit-image-set(url(one.png) 1x);width:5px;height:5px}"
             ".f{background-image:image-set(\"a.avif\" type(\"image/avif\"), \"a.jpg\" type(\"image/jpeg\"));width:5px;height:5px}"
             "</style><div class=a></div><div class=b></div><div class=c></div><div class=d></div><div class=e></div>"
             "<div class=f></div>", 800, 600);
    CHECK(box_width(&p, 0xffff0000) == 30 && box_width(&p, 0xff0000ff) == 44 && box_width(&p, 0xff00ff00) == 10);
    CHECK(!strcmp(with_class(p.root, "d")->style->bg_url, "lo.png"));
    CHECK(!strcmp(with_class(p.root, "e")->style->bg_url, "one.png"));
    CHECK(!strcmp(with_class(p.root, "f")->style->bg_url, "a.jpg"));
    unload(&p);

    /* @container: the 300 pixel box, not the 800 pixel viewport; named ones; and cq units. */
    load(&p, "<style>body{margin:0}.box{container-type:inline-size;width:300px}"
             ".side{container:side / inline-size;width:500px}"
             ".t{height:1px;background:red;width:10px}.n{height:1px;background:blue;width:10px}"
             ".u{height:1px;background:lime;width:50cqw}.o{height:1px;background:#123456;width:10px}"
             "@container (min-width: 400px) { .t { width: 100px } }"
             "@container (width < 400px) { .t { width: 50px } }"
             "@container side (inline-size > 450px) { .n { width: 70px } }"
             "@container nope (min-width: 1px) { .o { width: 80px } }"
             "@container (min-width: 1px) { .o { width: 90px } }"
             "</style><div class=side><div class=box><div class=t></div><div class=n></div><div class=u></div></div></div>"
             "<div class=o></div>", 800, 600);
    CHECK(box_width(&p, 0xffff0000) == 50);         /* the box is 300 wide */
    CHECK(box_width(&p, 0xff0000ff) == 70);         /* .side is the container named side, 500 wide */
    CHECK(box_width(&p, 0xff00ff00) == 150);        /* half the box */
    CHECK(box_width(&p, 0xff123456) == 10);         /* no container round it at all */
    CHECK(!layout_cq_changed);
    unload(&p);
    /* container-type: size answers height queries; inline-size does not. */
    load(&p, "<style>body{margin:0}.s{container-type:size;width:200px;height:100px}.i{container-type:inline-size;width:200px}"
             ".a,.b{height:1px;width:10px}.a{background:red}.b{background:blue}"
             "@container (height > 50px) and (width > 100px) { .a, .b { width: 60px } }"
             "@container not (orientation: portrait) { .a { height: 2px } }</style>"
             "<div class=s><div class=a></div></div><div class=i><div class=b></div></div>", 800, 600);
    CHECK(box_width(&p, 0xffff0000) == 60 && box_width(&p, 0xff0000ff) == 10);
    CHECK(colored(&p, 0xffff0000)->h == 2);
    unload(&p);

    /* @scope: rules for what is inside .card, :scope for the card itself. @property: initial values. */
    load(&p, "<style>body{margin:0}@scope (.card) { p { width: 33px } :scope { width: 200px } }"
             "p{margin:0;height:1px;background:red;width:10px}.card{background:blue;height:5px}"
             "@property --w { syntax: '<length>'; inherits: false; initial-value: 42px }"
             ".v{width:var(--w);height:1px;background:lime}</style>"
             "<p></p><div class=card><p></p></div><div class=v></div>", 800, 600);
    CHECK(box_width(&p, 0xffff0000) == 10 && box_width(&p, 0xff0000ff) == 200 && box_width(&p, 0xff00ff00) == 42);
    int scoped = 0;
    for (int i = 0; i < p.lo.nitems; i++)
        scoped += p.lo.items[i].kind == DK_FILL && p.lo.items[i].color == 0xffff0000 && p.lo.items[i].w == 33;
    CHECK(scoped == 1);
    unload(&p);
    puts("browser modern CSS: 3D transforms, backface-visibility, env(), image-set(), @container, cq units, @scope and @property passed");
}

/* A page painted onto a 64x64 white surface: the pixel at (x, y). */
static uint32_t pixels64[64 * 64];
static void paint64(const char *html)
{
    struct page p;
    load(&p, html, 64, 64);
    struct surface surface = { pixels64, 64, 64, 64, { 0, 0, 64, 64 } };
    paint_document(&p.lo, &surface, 0, 0, 0, 0, NULL, 0);
    unload(&p);
}
static uint32_t px64(int x, int y) { return pixels64[y * 64 + x] & 0xffffff; }
static int near(uint32_t got, uint32_t want, int slack)
{
    for (int c = 0; c < 24; c += 8)
        if (abs((int)(got >> c & 255) - (int)(want >> c & 255)) > slack)
            return 0;
    return 1;
}
#define BOX "<style>body{margin:0}#b{width:40px;height:40px;background:red;"

/* filter, clip-path, mix-blend-mode and backdrop-filter, by the pixels they leave. */
static void effects(void)
{
    static const struct { const char *filter; uint32_t want; } colors[] = {
        { "grayscale(1)", 0x363636 }, { "invert(1)", 0x00ffff }, { "brightness(.5)", 0x800000 },
        { "opacity(50%)", 0xff8080 }, { "sepia(1)", 0x645945 }, { "hue-rotate(120deg)", 0x007100 },
        { "saturate(0)", 0x363636 }, { "contrast(0)", 0x808080 }, { "invert(1) invert(1)", 0xff0000 },
    };
    for (unsigned k = 0; k < sizeof(colors) / sizeof(colors[0]); k++) {
        char html[256];
        snprintf(html, sizeof(html), BOX "filter:%s}</style><div id=b></div>", colors[k].filter);
        paint64(html);
        if (!near(px64(20, 20), colors[k].want, 3))
            fprintf(stderr, "filter %s: %06x, not %06x\n", colors[k].filter, px64(20, 20), colors[k].want);
        CHECK(near(px64(20, 20), colors[k].want, 3) && px64(50, 50) == 0xffffff);
    }
    /* blur spreads past the box; drop-shadow puts a copy under it, moved */
    paint64(BOX "filter:blur(3px)}</style><div id=b></div>");
    CHECK(near(px64(20, 20), 0xff0000, 2) && px64(42, 20) != 0xffffff && px64(42, 20) >> 16 == 0xff && px64(60, 20) == 0xffffff);
    paint64(BOX "filter:drop-shadow(6px 6px 0 blue)}</style><div id=b></div>");
    CHECK(px64(20, 20) == 0xff0000 && px64(43, 43) == 0x0000ff && px64(43, 2) == 0xffffff);
    paint64(BOX "-webkit-filter:drop-shadow(rgba(0,0,0,.5) 0 6px 0)}</style><div id=b></div>");
    CHECK(near(px64(20, 43), 0x808080, 2));
    /* a filter round another: the child's pixels go through both */
    paint64("<style>body{margin:0}#o{filter:invert(1)}#i{width:10px;height:10px;background:red}</style>"
            "<div id=o><div id=i></div></div>");
    CHECK(px64(5, 5) == 0x00ffff);

    /* clip-path shapes */
    paint64(BOX "clip-path:circle(50%)}</style><div id=b></div>");
    CHECK(px64(2, 2) == 0xffffff && px64(20, 20) == 0xff0000 && px64(38, 20) == 0xff0000);
    paint64(BOX "clip-path:inset(10px round 4px)}</style><div id=b></div>");
    CHECK(px64(5, 5) == 0xffffff && px64(20, 20) == 0xff0000 && px64(10, 10) == 0xffffff && px64(15, 10) == 0xff0000);
    paint64(BOX "clip-path:polygon(50% 0, 100% 100%, 0 100%)}</style><div id=b></div>");
    CHECK(px64(3, 3) == 0xffffff && px64(20, 36) == 0xff0000 && px64(20, 3) == 0xff0000);
    paint64(BOX "-webkit-clip-path:ellipse(10px 20px at 10px 20px)}</style><div id=b></div>");
    CHECK(px64(10, 20) == 0xff0000 && px64(30, 20) == 0xffffff);
    paint64(BOX "clip-path:inset(50%)}</style><div id=b>text</div>");     /* the screen-reader-only trick */
    for (int i = 0; i < 64 * 64; i++)
        CHECK((pixels64[i] & 0xffffff) == 0xffffff);
    paint64(BOX "clip-path:url(#nothing)}</style><div id=b></div>");       /* not drawn: the box as it is */
    CHECK(px64(2, 2) == 0xff0000);

    /* mix-blend-mode against what is behind */
    paint64("<style>body{margin:0;background:#8080ff}#b{width:40px;height:40px;background:red;mix-blend-mode:multiply}</style>"
            "<div id=b></div>");
    CHECK(near(px64(20, 20), 0x800000, 1) && px64(50, 50) == 0x8080ff);
    paint64(BOX "mix-blend-mode:difference}</style><div id=b></div>");
    CHECK(px64(20, 20) == 0x00ffff);
    paint64("<style>body{margin:0;background:#404040}#b{width:40px;height:40px;background:#404040;mix-blend-mode:screen}</style>"
            "<div id=b></div>");
    CHECK(near(px64(20, 20), 0x707070, 1));

    /* backdrop-filter: what is under the box, not the box */
    paint64("<style>body{margin:0}#r{width:40px;height:40px;background:red}"
            "#g{position:absolute;left:10px;top:10px;width:20px;height:20px;backdrop-filter:invert(1)}</style>"
            "<div id=r></div><div id=g></div>");
    CHECK(px64(5, 5) == 0xff0000 && px64(20, 20) == 0x00ffff && px64(35, 35) == 0xff0000);
    paint64("<style>body{margin:0}#r{width:20px;height:40px;background:red}"
            "#g{position:absolute;left:0;top:0;width:40px;height:40px;-webkit-backdrop-filter:blur(4px);"
            "background:rgba(255,255,255,0)}</style><div id=r></div><div id=g></div>");
    CHECK(px64(20, 20) != 0xffffff && px64(20, 20) != 0xff0000 && px64(2, 20) == 0xff0000 && px64(50, 20) == 0xffffff);
    /* A flex item's transform, opacity and filter stay its own: its later siblings do not get them. */
    struct page p;
    load(&p, "<style>body{margin:0}.row{display:flex}.row div{width:10px;height:10px}"
             ".a{background:red;transform:translateX(5px);opacity:.5;filter:invert(1)}.b{background:blue}"
             ".c{background:lime}</style><div class=row><div class=a></div><div class=b></div>"
             "<a class=c href=x style='display:block;width:10px;height:10px;transform:translateY(3px)'></a></div>", 64, 64);
    const struct ditem *red = colored(&p, 0xffff0000), *blue = colored(&p, 0xff0000ff), *lime = colored(&p, 0xff00ff00);
    CHECK(red && red->xf && red->opacity_group && red->fx && blue && !blue->xf && !blue->opacity_group && !blue->fx);
    CHECK(lime && lime->xf && p.lo.nlinks == 1 && p.lo.links[0].xf == lime->xf);
    unload(&p);
    CHECK(css_supports("(backdrop-filter: blur(4px)) and (filter: drop-shadow(0 1px 2px #000))"));
    CHECK(css_supports("(clip-path: circle(50% at 0 0))") && !css_supports("(clip-path: path('M0 0 L1 1'))"));
    CHECK(css_supports("(container-type: inline-size)") && css_supports("(transform: translate3d(1px, 0, 0))"));
    CHECK(!css_supports("(filter: url(#f))") && css_supports("(mix-blend-mode: multiply)"));
    puts("browser effects: filter functions, blur, drop-shadow, nested filters, clip-path shapes, blend modes and backdrop-filter passed");
}


static void subgrids(void)
{
    struct page p;
    load(&p,"<style>body{margin:0}.g{display:grid;width:170px;grid-template-columns:[left] 50px [mid] 110px [end];gap:10px}"
            ".s{display:grid;grid-column:1 / -1;grid-template-columns:subgrid}"
            ".b{grid-column:mid / end}</style><div class=g><div class=s><div>A</div><div class=b>B</div></div></div>",300,300);
    CHECK(word(&p,"A") && word(&p,"A")->x==0);
    CHECK(word(&p,"B") && word(&p,"B")->x==60);
    unload(&p);
    /* Two-axis subgrid inherits row sizes and gaps, ignoring its auto tracks. */
    load(&p,"<style>body{margin:0}.g{display:grid;width:170px;grid-template-columns:50px 110px;grid-template-rows:30px 70px;gap:10px}"
            ".s{display:grid;grid-column:1 / 3;grid-row:1 / 3;grid-template-columns:subgrid;grid-template-rows:subgrid;grid-auto-rows:999px}"
            ".b{grid-column:2;grid-row:2}</style><div class=g><div class=s><div>A</div><div class=b>B</div></div></div>",300,300);
    CHECK(word(&p,"B") && word(&p,"B")->x==60 && word(&p,"B")->y==40);
    CHECK(p.lo.height==110);
    unload(&p);
    /* Grandchildren contribute to each auto row, aligning a sibling in that row. */
    load(&p,"<style>body{margin:0}.g{display:grid;width:100px;grid-template-columns:50px 50px;row-gap:10px}"
            ".s{display:grid;grid-row:1 / 3;grid-template-rows:subgrid}.tall{height:60px}"
            ".b{grid-column:2;grid-row:2}</style><div class=g><div class=s><div class=tall>A</div><div>B</div></div><div class=b>C</div></div>",300,300);
    CHECK(word(&p,"B") && word(&p,"C") && word(&p,"B")->y==70 && word(&p,"C")->y==70);
    unload(&p);
    /* Nested subgrids keep the outer track proportions. */
    load(&p,"<style>body{margin:0}.g{display:grid;width:170px;grid-template-columns:50px 110px;gap:10px}"
            ".s{display:grid;grid-column:1 / -1;grid-template-columns:subgrid}</style>"
            "<div class=g><div class=s><div class=s><div>A</div><div>B</div></div></div></div>",300,300);
    CHECK(word(&p,"B") && word(&p,"B")->x==60);
    unload(&p);
    /* Content in a subgrid sizes the parent's auto columns individually. */
    load(&p,"<style>body{margin:0}.g{display:grid;width:100px;grid-template-columns:auto auto;justify-content:start}"
            ".s{display:grid;grid-column:1 / 3;grid-template-columns:subgrid}</style>"
            "<div class=g><div class=s><div>AAAA</div><div>B</div></div></div>",300,300);
    CHECK(word(&p,"B") && word(&p,"B")->x==32);
    unload(&p);
    /* Explicit subgrid gaps meet halfway across the parent's gutter. */
    load(&p,"<style>body{margin:0}.g{display:grid;width:170px;grid-template-columns:50px 110px;gap:10px}"
            ".s{display:grid;grid-column:1 / 3;grid-template-columns:subgrid;gap:20px;padding:0 5px}</style>"
            "<div class=g><div class=s><div>A</div><div>B</div></div></div>",300,300);
    CHECK(word(&p,"A") && word(&p,"A")->x==5 && word(&p,"B") && word(&p,"B")->x==65);
    unload(&p);
    CHECK(!css_supports("(grid-template-columns: subgrid 20px)"));
    CHECK(!css_supports("(grid-template-columns: subgrid [broken)"));
    CHECK(css_supports("(grid-template-columns: subgrid repeat(2, [edge]))"));
    CHECK(!css_supports("(grid-template-columns: subgrid repeat(0, [edge]))"));
    CHECK(css_supports("(grid-template-columns: subgrid)"));
    CHECK(!css_supports("(scroll-snap-type: x mandatory)"));
    CHECK(!css_supports("(contain: layout)"));
    CHECK(!css_supports("(transform-style: preserve-3d)"));
    puts("browser subgrid: columns, rows, inherited names and gaps, nested grids and auto-row contributions passed");
}
static void perspective_projection(void)
{
    struct page p;
    const char *cases[]={"perspective(100px) translateZ(50px)",
        "matrix3d(1,0,0,0,0,1,0,0,0,0,1,-0.01,0,0,50,0.5)",
        "perspective(100px) scaleZ(2) translateZ(25px)"};
    for(unsigned k=0;k<sizeof(cases)/sizeof(cases[0]);k++) {
        char html[512];snprintf(html,sizeof(html),"<style>body{margin:0}button{margin:0;border:0;padding:0;display:block;width:20px;height:10px;background:red;transform-origin:0 0;transform:%s}</style><button></button>",cases[k]);
        load(&p,html,100,100);
        const struct ditem *d=colored(&p,OPAQUE(0xff0000));
        expect_rect(visual_item(&p,d),0,0,40,20);
        struct hotspot hot={.x=d->x,.y=d->y,.w=d->w,.h=d->h,.xf=d->xf};
        CHECK(layout_hit(&p.lo,&hot,1,35,15,0,0));
        CHECK(!layout_hit(&p.lo,&hot,1,45,15,0,0));
        unload(&p);
    }
    /* Parent perspective projects children, but does not transform its own background. */
    load(&p,"<style>body{margin:0}.p{width:100px;height:100px;perspective:100px;perspective-origin:0 0;background:blue}"
            "button{margin:0;border:0;padding:0;display:block;width:20px;height:10px;background:red;transform:translateZ(50px);transform-origin:0 0}</style>"
            "<div class=p><button></button></div>",100,100);
    expect_rect(visual_item(&p,colored(&p,OPAQUE(0xff0000))),0,0,40,20);
    expect_rect(visual_item(&p,colored(&p,OPAQUE(0x0000ff))),0,0,100,100);
    const struct ditem *child=colored(&p,OPAQUE(0xff0000));
    CHECK(layout_box_contains(&p.lo,child->xf,0,0,mkrect(child->x,child->y,child->w,child->h),35,15,0,0));unload(&p);
    /* The eye plane clips a box behind the viewer. */
    paint64(BOX "transform-origin:0 0;transform:perspective(100px) translateZ(150px)}</style><div id=b></div>");
    CHECK(px64(5,5)==0xffffff);
    paint64(BOX "width:20px;height:10px;transform-origin:0 0;transform:perspective(100px) translateZ(50px)}</style><div id=b></div>");
    CHECK(px64(35,15)==0xff0000 && px64(45,15)==0xffffff);
    /* Rotation produces a trapezoid: its right edge recedes from the viewer. */
    paint64(BOX "transform-origin:0 0;transform:perspective(100px) rotateY(60deg)}</style><div id=b></div>");
    CHECK(px64(2,30)==0xff0000 && px64(14,38)==0xffffff);
    CHECK(css_supports("(perspective: 100px)") && !css_supports("(perspective: -1px)") && !css_supports("(perspective: 50%)"));
    CHECK(!css_supports("(transform: translateZ(10%))"));
    CHECK(!css_supports("(perspective: auto)") && !css_supports("(perspective: calc(10px - 20px))"));
    CHECK(!css_supports("(grid-auto-rows: subgrid)"));
    puts("browser perspective: depth, matrix3d, scaleZ, parent perspective, raster pixels, eye-plane clipping and hit testing passed");
}
static struct node *by_id(struct node *root, const char *id)
{
    for (struct node *n = root; n; n = node_next(n, root))
        if (n->type == N_ELEMENT && node_attr(n, "id") && !strcmp(node_attr(n, "id"), id))
            return n;
    return NULL;
}

static int item_index(const struct page *p, const struct ditem *d) { return (int)(d - p->lo.items); }

/* As showModal() and showPopover() leave them: #d modal (and open), #p1 then #p2 open popovers. */
static void open_top_layer(struct node *root)
{
    struct node *d = by_id(root, "d"), *p1 = by_id(root, "p1"), *p2 = by_id(root, "p2");
    if (d)
        d->modal = 1, d->top_seq = 1, root->top_modal = d;
    if (p2)
        p2->popover_open = POP_AUTO, p2->top_seq = 2;
    if (p1)
        p1->popover_open = POP_MANUAL, p1->top_seq = 3;
}

static void top_layer(void)
{
    struct page p;
    /* A modal dialog leaves its clipped, transformed, z-indexed box: centred in the viewport, over
     * everything, on a backdrop that covers the window and stays there as the page scrolls. */
    before_style = open_top_layer;
    load(&p, "<style>body{margin:0}#d{width:100px;height:50px;padding:0;border:0;background:#123456}"
             ".trap{position:relative;z-index:5;transform:translateX(10px);overflow:hidden;width:30px;height:20px}"
             ".over{position:absolute;z-index:999;top:0;left:0;width:400px;height:300px;background:#00ff00}</style>"
             "<div class=trap><dialog id=d open>D</dialog></div><div class=over></div>"
             "<div style='height:2000px'></div>", 400, 300);
    const struct ditem *dlg = colored(&p, 0xff123456), *over = colored(&p, 0xff00ff00);
    CHECK(dlg && over && !dlg->xf && !dlg->sb);
    CHECK(dlg->x == 150 && dlg->y == 125 && dlg->w == 100 && dlg->h == 50);
    CHECK(item_index(&p, dlg) > item_index(&p, over));                  /* above z-index 999 */
    CHECK(dlg->dyn && dy_of(&p, dlg, 0, 500) == 625);                  /* fixed in the window */
    const struct ditem *back = NULL;
    for (int i = 0; i < p.lo.nitems; i++)
        if (p.lo.items[i].kind == DK_FILL && p.lo.items[i].w == 400 && p.lo.items[i].h == 300 &&
            ALPHA(p.lo.items[i].color) > 0 && ALPHA(p.lo.items[i].color) < 64)
            back = &p.lo.items[i];
    CHECK(back && back->node == by_id(p.root, "d") && item_index(&p, back) < item_index(&p, dlg) &&
          item_index(&p, back) > item_index(&p, over));
    unload(&p);
    /* Popovers: hidden until opened, then centred, the one opened last on top; a closed one has no box. */
    load(&p, "<style>body{margin:0}[popover]{padding:0;border:0}#p1{width:40px;height:10px;background:#ff0000}"
             "#p2{width:60px;height:20px;background:#0000ff}#p3{background:#00ffff}</style>"
             "<div id=p1 popover=manual></div><div id=p2 popover></div><div id=p3 popover>closed</div>", 200, 100);
    const struct ditem *r = colored(&p, 0xffff0000), *b = colored(&p, 0xff0000ff);
    CHECK(r && b && !colored(&p, 0xff00ffff) && !word(&p, "closed"));
    CHECK(r->x == 80 && r->y == 45 && b->x == 70 && b->y == 40);
    CHECK(item_index(&p, r) > item_index(&p, b));                       /* p1 went in after p2 */
    unload(&p);
    before_style = NULL;
    /* A dialog shown without showModal stays where it is, centred across, as wide as its content. */
    load(&p, "<style>body{margin:0}dialog{padding:0;border:0;background:#123456}</style>"
             "<div style='height:30px'></div><dialog open>wide</dialog>", 200, 100);
    const struct ditem *shown = colored(&p, 0xff123456);
    CHECK(shown && shown->y == 30 && shown->w == 32 && shown->x == 84 && !shown->dyn);
    unload(&p);
}

static void fit_content(void)
{
    struct page p;
    load(&p, "<style>body{margin:0}#a{width:fit-content;background:#ff0000}#b{width:max-content;margin:0 auto;"
             "background:#00ff00}#c{position:absolute;top:10px;bottom:20px;left:0;width:50px;background:#0000ff}"
             "#e{position:absolute;top:0;bottom:0;left:0;right:0;width:20px;height:fit-content;margin:auto;"
             "background:#ffff00}</style><div id=a>abc</div><div id=b>abcd</div><div id=c></div><div id=e>x</div>",
         200, 200);
    const struct ditem *a = colored(&p, 0xffff0000), *b = colored(&p, 0xff00ff00);
    const struct ditem *c = colored(&p, 0xff0000ff), *e = colored(&p, 0xffffff00);
    CHECK(a && a->w == 24 && a->x == 0);
    CHECK(b && b->w == 32 && b->x == 84);                              /* auto margins centre it */
    CHECK(c && c->y == 10 && c->h == 170);                             /* stretched between top and bottom */
    CHECK(e && e->h == 20 && e->y == 90 && e->x == 90);                /* fit-content: centred, not stretched */
    unload(&p);
}

/* The anchor #a: 40x20 at (50, 30) (or at the bottom of a 100px viewport). */
#define ANCHOR_CSS "body{margin:0}#a{anchor-name:--a;margin:30px 0 0 50px;width:40px;height:20px;background:#ff0000}"

/* #b is the button that opened the popover #p1. */
static void opened_by_button(struct node *root)
{
    struct node *p = by_id(root, "p1");
    if (p)
        p->popover_open = POP_AUTO, p->top_seq = 1, p->invoker = by_id(root, "b");
}

static void anchors(void)
{
    struct page p;
    const struct ditem *t;
    /* anchor() in the insets, by name and by the default anchor; calc() and percentages */
    load(&p, "<style>" ANCHOR_CSS "#t{position:absolute;position-anchor:--a;top:anchor(bottom);left:anchor(right);"
             "width:10px;height:10px;background:#00ff00}#u{position:absolute;top:calc(anchor(--a top) - 5px);"
             "left:anchor(--a 25%);width:anchor-size(--a width);height:4px;background:#0000ff}"
             "#v{position:absolute;right:anchor(--a left);bottom:anchor(--a top);width:6px;height:6px;background:#00ffff}"
             "</style><div id=a></div><div id=t></div><div id=u></div><div id=v></div>", 200, 200);
    t = colored(&p, 0xff00ff00);
    CHECK(t && t->x == 90 && t->y == 50);
    t = colored(&p, 0xff0000ff);
    CHECK(t && t->x == 60 && t->y == 25 && t->w == 40);
    t = colored(&p, 0xff00ffff);
    CHECK(t && t->x == 44 && t->y == 24);
    unload(&p);
    /* position-area: a cell beside the anchor, or centred across it */
    const char *areas[] = { "bottom", "top left", "right center", "span-all", "left span-bottom" };
    const int at[][2] = { { 65, 50 }, { 40, 20 }, { 90, 35 }, { 65, 35 }, { 40, 30 } };
    for (unsigned k = 0; k < sizeof(areas) / sizeof(areas[0]); k++) {
        char html[512];
        snprintf(html, sizeof(html), "<style>" ANCHOR_CSS "#t{position:absolute;position-anchor:--a;position-area:%s;"
                 "width:10px;height:10px;background:#00ff00}</style><div id=a></div><div id=t></div>", areas[k]);
        load(&p, html, 200, 200);
        t = colored(&p, 0xff00ff00);
        CHECK(t && t->x == at[k][0] && t->y == at[k][1]);
        unload(&p);
    }
    /* anchor-center, and an anchor that is not there: the fallback */
    load(&p, "<style>" ANCHOR_CSS "#t{position:absolute;position-anchor:--a;top:anchor(bottom);justify-self:anchor-center;"
             "width:10px;height:10px;background:#00ff00}#u{position:absolute;top:anchor(--nothing bottom, 7px);left:3px;"
             "width:4px;height:4px;background:#0000ff}</style><div id=a></div><div id=t></div><div id=u></div>", 200, 200);
    t = colored(&p, 0xff00ff00);
    CHECK(t && t->x == 65 && t->y == 50);
    t = colored(&p, 0xff0000ff);
    CHECK(t && t->x == 3 && t->y == 7);
    unload(&p);
    /* no room below the anchor at the foot of the window: flip-block puts it above */
    load(&p, "<style>body{margin:0}#a{anchor-name:--a;position:absolute;top:80px;left:20px;width:40px;height:20px}"
             "#t{position:fixed;position-anchor:--a;top:anchor(bottom);left:anchor(left);width:10px;height:30px;"
             "background:#00ff00;position-try-fallbacks:flip-block}"
             "#u{position:fixed;position-anchor:--a;position-area:bottom;width:10px;height:30px;background:#0000ff;"
             "position-try-fallbacks:flip-inline,top}</style><div id=a></div><div id=t></div><div id=u></div>", 200, 100);
    t = colored(&p, 0xff00ff00);
    CHECK(t && t->x == 20 && t->y == 50);
    t = colored(&p, 0xff0000ff);
    CHECK(t && t->x == 35 && t->y == 50);
    CHECK(!t->dyn);                                     /* it goes with the anchor as the page scrolls */
    unload(&p);
    /* a popover is anchored to the button that opened it */
    before_style = opened_by_button;
    load(&p, "<style>body{margin:0}#b{margin-left:100px;width:30px;height:16px;padding:0;border:0}"
             "#p1{inset:auto;margin:0;padding:0;border:0;position-area:bottom span-right;width:50px;height:10px;"
             "background:#00ff00}</style><button id=b></button><div id=p1 popover></div>", 300, 200);
    t = colored(&p, 0xff00ff00);
    CHECK(t && t->x == 100 && t->y == 16);
    unload(&p);
    before_style = NULL;
}

int main(void)
{
    anchors();
    top_layer();
    fit_content();
    subgrids();
    perspective_projection();
    effects();
    modern_css();
    multicolumn();
    transforms_2d();
    font_faces();
    text_flow();
    positioning();
    boxes();
    background_pictures(); background_pixels(); opacity_pixels(); stacking(); named_grid_lines(); inside_marker_lifetime();
    text_extras();
    zoom();
    ranges();
    object_fit();
    scrolling_boxes();
    shadow_layout();
    canvas_layout();
    cjk_wrapping();
    puts("browser layout: spacing, wrapping, fixed and sticky boxes, the top layer, anchor positioning, fit-content, outlines, tables, ruby, background pictures, object-fit, scrolling boxes, canvases, CJK wrapping and multi-column boxes passed");
}
