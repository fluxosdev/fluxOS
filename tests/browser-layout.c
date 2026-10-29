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

static void load(struct page *p, const char *html, int vw, int vh)
{
    memset(p, 0, sizeof(*p));
    struct node *root = p->root = html_parse(&p->a, html, (int)strlen(html));
    struct sheet *sheets[4];
    int ns = 0;
    for (struct node *n = root; n; n = node_next(n, root))
        if (n->type == N_ELEMENT && n->tag == T_STYLE && n->first && ns < 4)
            sheets[ns++] = css_parse(&p->a, n->first->text, n->first->len, vw);
    css_viewport_h = vh;
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

int main(void)
{
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
    puts("browser layout: spacing, wrapping, fixed and sticky boxes, outlines, tables, ruby, background pictures, object-fit, scrolling boxes, canvases and CJK wrapping passed");
}
