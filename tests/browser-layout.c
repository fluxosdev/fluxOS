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
int rect_empty(struct rect r) { return r.w <= 0 || r.h <= 0; }
struct rect rect_intersect(struct rect a, struct rect b) { (void)b; return a; }
void gfx_bevel(struct surface *s, struct rect r, enum bevel st) { (void)s; (void)r; (void)st; }
void gfx_fill(struct surface *s, int x, int y, int w, int h, color_t c) { (void)s; (void)x; (void)y; (void)w; (void)h; (void)c; }
void gfx_focus_rect(struct surface *s, struct rect r) { (void)s; (void)r; }
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

int main(void)
{
    text_flow(); positioning(); boxes();
    puts("browser layout: spacing, wrapping, fixed and sticky boxes, outlines, tables and ruby passed");
}
