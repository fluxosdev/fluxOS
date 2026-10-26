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
struct rect rect_intersect(struct rect a, struct rect b) { (void)b; return a; }
void gfx_bevel(struct surface *s, struct rect r, enum bevel st) { (void)s; (void)r; (void)st; }
void gfx_fill(struct surface *s, int x, int y, int w, int h, color_t c) { (void)s; (void)x; (void)y; (void)w; (void)h; (void)c; }
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
    struct arena a, sa, la;
    struct layout lo;
};

/* Pictures the engine would have fetched: every background picture and <img> is this 40x20 one. */
static struct image test_img = { 40, 20, NULL };

static void load(struct page *p, const char *html, int vw, int vh)
{
    memset(p, 0, sizeof(*p));
    struct node *root = html_parse(&p->a, html, (int)strlen(html));
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
    CHECK(pic[4]->grad && !pic[4]->bgp->norep_x);               /* the gradient is over the picture */
    unload(&p);

    /* the old attribute, and a data: URL that is not cut short */
    load(&p, "<body background='img/tile.gif'>x</body>", 800, 600);
    CHECK(first_of(&p, DK_BGIMG) && first_of(&p, DK_BGIMG)->bgp);
    unload(&p);
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

int main(void)
{
    text_flow(); positioning(); boxes(); background_pictures(); object_fit(); scrolling_boxes();
    puts("browser layout: spacing, wrapping, fixed and sticky boxes, outlines, tables, ruby, background pictures, object-fit and scrolling boxes passed");
}
