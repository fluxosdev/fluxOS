/* Exercise the production HTML parser and CSS cascade without a display. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
int wfont_height(const struct wfont *f) { (void)f; return 16; }
const struct wfont *wfont_get(int family, int bold, int italic, int px)
{ (void)family; (void)bold; (void)italic; (void)px; return NULL; }
static struct node *id(struct node *root, const char *name)
{
    for (struct node *n = root; n; n = node_next(n, root)) {
        const char *value = node_attr(n, "id");
        if (value && !strcmp(value, name)) return n;
    }
    return NULL;
}
static void entities(void)
{
    struct arena arena = {0};
    const char *html = "<!doctype html><p id='text'>&CounterClockwiseContourIntegral;|&NotEqualTilde;|&fjlig;|&Afr;|&AMP;|&alpha;|&notit;|&copy |&apos|&unknown;|&NotEqualTilde|&#x80;|&#0;</p>"
        "<p id='attr' title='&copycat &copy=1 &copy; &NotEqualTilde; &fjlig; &amp=1 &amp;'></p>";
    struct node *root = html_parse(&arena, html, (int)strlen(html));
    struct node *text = id(root, "text"), *attr = id(root, "attr");
    CHECK(text && attr && text->first && text->first->type == N_TEXT);
    CHECK(!strcmp(text->first->text, "∳|≂̸|fj|𝔄|&|α|¬it;|© |&apos|&unknown;|&NotEqualTilde|€|�"));
    CHECK(!strcmp(node_attr(attr, "title"), "&copycat &copy=1 © ≂̸ fj &amp=1 &"));
    arena_free(&arena);
}
static void cascade(void)
{
    struct arena arena = {0};
    const char *html = "<!doctype html><section id='rows'><span id='a' class='item'></span>text<b id='b' class='item'></b><i id='c'></i><span id='d' class='item'></span><span id='e' class='item'></span></section>";
    struct node *root = html_parse(&arena, html, (int)strlen(html));
    const char *css =
        ".item { color: #010101 }"
        ":nth-child(2 of .item, #unused) { color: #123456 }"
        "#b { color: #999999 }" /* the nonmatching ID in the filter still adds specificity */
        ":nth-last-child(2 of .item) { background-color: #abcdef }"
        ":nth-child(odd of #rows > span) { padding-left: 7px }"
        ":nth-child(-n + 2 of .item) { margin-left: 11px }"
        ":nth-child(3 of :is(.item, i)) { border-left: 5px solid red }"
        ":nth-child(2bad) { font-size: 91px }"
        ":nth-child(1 of ) { font-size: 92px }"
        "span:nth-of-type(1 of .item) { font-size: 93px }";
    struct sheet *sheet = css_parse(&arena, css, (int)strlen(css), 800);
    CHECK(sheet);
    css_cascade(&arena, root, &sheet, 1, 800, 600, NULL, NULL);
    struct node *a = id(root, "a"), *b = id(root, "b"), *c = id(root, "c"),
                *d = id(root, "d"), *e = id(root, "e");
    CHECK(a && b && c && d && e);
    CHECK(b->style->color == OPAQUE(0x123456));
    CHECK(a->style->color == OPAQUE(0x010101));
    CHECK(d->style->bg == OPAQUE(0xabcdef) && b->style->bg != OPAQUE(0xabcdef));
    CHECK(a->style->padding[3].v == 7 && e->style->padding[3].v == 7);
    CHECK(d->style->padding[3].v == 0);
    CHECK(a->style->margin[3].v == 11 && b->style->margin[3].v == 11);
    CHECK(d->style->margin[3].v == 0 && c->style->border_w[3] == 5);
    CHECK(a->style->font_size < 91 && b->style->font_size < 91);
    arena_free(&arena);
}
static void selector_specificity(void)
{
    struct arena arena = {0};
    const char *html = "<!doctype html><div id='parent'><b id='subject' class='target'><i class='child'></i></b></div>";
    struct node *root = html_parse(&arena, html, (int)strlen(html));
    const char *css =
        "b:is(#unused, .target) { color: #123456 } #subject { color: #999999 }"
        "b:not(#unused, .missing) { background-color: #abcdef } #subject { background-color: #999999 }"
        "b:where(#subject) { padding-left: 3px } .target { padding-left: 9px }"
        "div:has(#unused, .child) { border-left: 7px solid red } #parent { border-left: 2px solid blue }";
    struct sheet *sheet = css_parse(&arena, css, (int)strlen(css), 800);
    css_cascade(&arena, root, &sheet, 1, 800, 600, NULL, NULL);
    struct node *subject = id(root, "subject");
    CHECK(subject->style->color == OPAQUE(0x123456));
    CHECK(subject->style->bg == OPAQUE(0xabcdef));
    CHECK(subject->style->padding[3].v == 9);
    CHECK(id(root, "parent")->style->border_w[3] == 7);
    arena_free(&arena);
}
static void nth_boundaries(void)
{
    const char *formula = "-2147483648n + 2147483647 of .item";
    const char *filter;
    int a, b;
    CHECK(web_parse_nth(formula, formula + strlen(formula), &a, &b, &filter) == 0);
    CHECK(a == (-2147483647 - 1) && b == 2147483647 && !strcmp(filter, ".item"));
    CHECK(web_nth_fits(a, b, 2147483647) && !web_nth_fits(a, b, 1));
    CHECK(web_nth_fits(1, -2147483647 - 1, 1));
    CHECK(web_nth_fits(-1, 2, 1) && !web_nth_fits(-1, 2, 3));
    const char *invalid[] = { "2 n", "oddly", "2n+", "n + -1", "2147483648n", "1 of " };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(web_parse_nth(invalid[i], invalid[i] + strlen(invalid[i]), &a, &b, &filter) < 0);
}
static void text_and_interaction_properties(void)
{
    struct arena arena = {0};
    const char *html = "<!doctype html><div id='t'><span id='in'>x</span></div>"
        "<p id='o'>o</p><ul><li id='li'>i</li></ul><table id='tb'><caption id='cap'>c</caption></table>";
    struct node *root = html_parse(&arena, html, (int)strlen(html));
    const char *css =
        "#t { letter-spacing: 2px; word-spacing: -1px; word-break: break-all; cursor: pointer;"
        " pointer-events: none; user-select: none; text-shadow: 1px 2px 3px #112233;"
        " font-variant: small-caps; text-decoration: underline wavy #445566 }"
        "#o { outline: 3px dashed #778899; outline-offset: 2px; text-overflow: ellipsis; overflow-wrap: anywhere }"
        "#li { list-style: square inside } #li::marker { color: #aabbcc }"
        "#tb { table-layout: fixed; empty-cells: hide } #cap { caption-side: bottom }"
        "#in::selection { background-color: #010203 } #in::placeholder { color: #040506 }";
    struct sheet *sheet = css_parse(&arena, css, (int)strlen(css), 800);
    css_cascade(&arena, root, &sheet, 1, 800, 600, NULL, NULL);
    const struct style *t = id(root, "t")->style, *in = id(root, "in")->style, *o = id(root, "o")->style;
    CHECK(t->letter_spacing == 2 && t->word_spacing == -1 && t->word_break == WB_BREAK_ALL);
    CHECK(t->cursor == CSSCUR_POINTER && t->pointer_none && t->select_none);
    CHECK(t->ts_x == 1 && t->ts_y == 2 && t->ts_blur == 3 && t->ts_color == OPAQUE(0x112233));
    CHECK(t->small_caps && t->deco_style == DS_WAVY && t->deco_color == OPAQUE(0x445566));
    CHECK(in->letter_spacing == 2 && in->cursor == CSSCUR_POINTER && in->pointer_none);    /* inherited */
    CHECK(o->outline_w == 3 && o->outline_style == BS_DASHED && o->outline_offset == 2 &&
          o->outline_color == OPAQUE(0x778899));
    CHECK(o->text_ellipsis && o->word_break == WB_BREAK_WORD && !o->letter_spacing);
    const struct style *li = id(root, "li")->style;
    CHECK(li->list_inside && li->list_style == LS_SQUARE);
    CHECK(id(root, "li")->pseudo_st[PE_MARKER - 1] &&
          id(root, "li")->pseudo_st[PE_MARKER - 1]->color == OPAQUE(0xaabbcc));
    CHECK(id(root, "tb")->style->table_fixed && id(root, "tb")->style->empty_hide);
    CHECK(id(root, "cap")->style->caption_bottom);
    CHECK(id(root, "in")->pseudo_st[PE_SELECTION - 1]->bg == OPAQUE(0x010203));
    CHECK(id(root, "in")->pseudo_st[PE_PLACEHOLDER - 1]->color == OPAQUE(0x040506));
    CHECK(!id(root, "o")->pseudo_st[PE_SELECTION - 1]);
    arena_free(&arena);
}
static void state_pseudo_classes(void)
{
    struct arena arena = {0};
    const char *html = "<!doctype html><html lang='fr-CA'><body>"
        "<form id='bad'><input required value=''></form>"
        "<form id='good'><input required value='x'><input type='email' value='a@b.cc'></form>"
        "<form id='mail'><input type='email' value='nope'></form>"
        "<form id='num'><input type='number' min='5' max='9' value='12'></form>"
        "<input id='ro' readonly><input id='rw'><input id='dis' disabled>"
        "<input id='ck' type='checkbox' checked><button id='sub'>Go</button>"
        "<progress id='pg'></progress><p id='fr'>x</p><p id='en' lang='en'>y</p><a id='t' name='t'>t</a>"
        "</form></body></html>";
    struct node *root = html_parse(&arena, html, (int)strlen(html));
    const char *css =
        "#bad:invalid { z-index: 1 } #good:valid { z-index: 2 } #mail:invalid { z-index: 3 }"
        "#num:invalid { z-index: 4 } #good:invalid { z-index: 99 }"
        "#ro:read-only { z-index: 5 } #rw:read-write { z-index: 6 } #dis:read-only { z-index: 7 }"
        "#ck:default { z-index: 8 } #sub:default { z-index: 9 } #pg:indeterminate { z-index: 10 }"
        ":lang(fr) #fr { z-index: 11 } #en:lang(fr) { z-index: 99 } #en:lang(en) { z-index: 12 }"
        "#t:target { z-index: 13 } #num input:out-of-range { z-index: 14 }"
        "#num input:in-range { z-index: 99 }";
    struct sheet *sheet = css_parse(&arena, css, (int)strlen(css), 800);
    css_target_id = "t";
    css_cascade(&arena, root, &sheet, 1, 800, 600, NULL, NULL);
    css_target_id = NULL;
    struct { const char *id; int z; } want[] = {
        { "bad", 1 }, { "good", 2 }, { "mail", 3 }, { "num", 4 }, { "ro", 5 }, { "rw", 6 }, { "dis", 7 },
        { "ck", 8 }, { "pg", 10 }, { "fr", 11 }, { "en", 12 }, { "t", 13 },
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        CHECK(id(root, want[i].id)->style->z_set && id(root, want[i].id)->style->z_index == want[i].z);
    struct node *num_input = id(root, "num")->first;
    CHECK(num_input && num_input->style->z_set && num_input->style->z_index == 14);
    arena_free(&arena);
}
static void backgrounds_and_overflow(void)
{
    struct arena arena = {0};
    const char *html = "<!doctype html><body>"
        "<div id='a'></div><div id='b'></div><div id='c'></div><div id='d'></div><div id='e' style=\"background-image: url('s.png')\"></div>"
        "<div id='f'></div><div id='g'></div><div id='h'></div><div id='i'></div><div id='j'></div>"
        "<img id='k'><div id='l'></div></body>";
    struct node *root = html_parse(&arena, html, (int)strlen(html));
    const char *css =
        "#a { background: #123 url(img/a.png) no-repeat 10px 20% / 50% auto }"
        "#b { background: url( \"b(1).png\" ) repeat-y, url(under.png) red }"
        "#c { background: linear-gradient(red, blue), url(//x.org/c.png) center / contain }"
        "#d { background-image: url(d.png); background-repeat: no-repeat repeat; background-position: right 5px bottom 7px }"
        "#f { overflow: hidden } #g { overflow: auto hidden } #h { overflow-y: scroll } #i { overflow: clip }"
        "#j { background: url(j.png); background: green }"
        "#k { object-fit: cover; object-position: 10% bottom }"
        "#l { background: url(data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg'/>) border-box }";
    struct sheet *sheet = css_parse(&arena, css, (int)strlen(css), 800);
    css_cascade(&arena, root, &sheet, 1, 800, 600, NULL, NULL);
    struct style *a = id(root, "a")->style, *b = id(root, "b")->style, *c = id(root, "c")->style,
                 *d = id(root, "d")->style;
    CHECK(a->bg == OPAQUE(0x112233) && a->bg_url && !strcmp(a->bg_url, "img/a.png"));
    CHECK(a->bg_norep_x && a->bg_norep_y);
    CHECK(a->bg_px.unit == U_PX && a->bg_px.v == 10 && a->bg_py.unit == U_PCT && a->bg_py.v == 2000);
    CHECK(a->bg_size == BGS_LEN && a->bg_w.unit == U_PCT && a->bg_w.v == 5000 && a->bg_h.unit == U_AUTO);
    CHECK(b->bg_url && !strcmp(b->bg_url, "b(1).png") && b->bg_norep_x && !b->bg_norep_y);
    CHECK(b->bg == OPAQUE(0xff0000));                       /* the color is in the last layer */
    CHECK(c->bg_url && !strcmp(c->bg_url, "//x.org/c.png") && c->grad && c->bg_size == BGS_CONTAIN);
    CHECK(c->bg_px.unit == U_PCT && c->bg_px.v == 5000 && c->bg_py.v == 5000);
    CHECK(d->bg_url && d->bg_norep_x && !d->bg_norep_y);
    CHECK(d->bg_px.unit == U_CALC && d->bg_px.pct == 10000 && d->bg_px.v == -5);     /* 100% - 5px */
    CHECK(d->bg_py.unit == U_CALC && d->bg_py.pct == 10000 && d->bg_py.v == -7);
    CHECK(id(root, "e")->style->bg_url && !strcmp(id(root, "e")->style->bg_url, "s.png"));
    CHECK(id(root, "f")->style->ov_x == OV_HIDDEN && id(root, "f")->style->ov_y == OV_HIDDEN);
    CHECK(id(root, "f")->style->clips);
    CHECK(id(root, "g")->style->ov_x == OV_AUTO && id(root, "g")->style->ov_y == OV_HIDDEN);
    CHECK(id(root, "h")->style->ov_y == OV_SCROLL && id(root, "h")->style->ov_x == OV_AUTO);   /* visible turns auto */
    CHECK(id(root, "i")->style->ov_x == OV_HIDDEN);
    CHECK(!id(root, "j")->style->bg_url && id(root, "j")->style->bg == OPAQUE(0x008000));  /* the shorthand resets it */
    CHECK(!id(root, "a")->style->clips);
    struct style *k = id(root, "k")->style;
    CHECK(k->obj_fit == OF_COVER && k->obj_px.unit == U_PCT && k->obj_px.v == 1000 && k->obj_py.v == 10000);
    struct style *l = id(root, "l")->style;
    CHECK(l->bg_url && !strncmp(l->bg_url, "data:image/svg+xml;utf8,<svg", 28) && l->bg_origin == 1);
    arena_free(&arena);
}
static void counters_and_selectors(void)
{
    struct arena arena = {0};
    const char *html = "<!doctype html><body><div id='r'><h2 id='a'>x</h2><h2 id='b'>y</h2><h2 id='c'>z</h2></div>"
        "<p id='p' lang='en'>t</p><input id='i' type='email' value='nope'><input id='j' type='email' value='a@b.cc'>"
        "<input id='k' type='email'><div id='lc'>l</div></body>";
    struct node *root = html_parse(&arena, html, (int)strlen(html));
    const char *css =
        "#r { counter-reset: sec 10 other } h2 { counter-increment: sec } h2::before { content: counter(sec) \". \" counter(other, upper-roman) }"
        "#c::after { content: counter(sec, lower-alpha) \"/\" counter(sec, upper-roman) }"
        ":scope { z-index: 3 } p:dir(ltr) { z-index: 4 } p:dir(rtl) { z-index: 99 }"
        "input:user-invalid { z-index: 5 } #lc { -webkit-line-clamp: 3; line-height: 20px; tab-size: 4; text-align-last: right }";
    struct sheet *sheet = css_parse(&arena, css, (int)strlen(css), 800);
    css_cascade(&arena, root, &sheet, 1, 800, 600, NULL, NULL);
    CHECK(id(root, "a")->pseudo_text[0] && !strcmp(id(root, "a")->pseudo_text[0], "11. 0"));
    CHECK(!strcmp(id(root, "b")->pseudo_text[0], "12. 0"));
    CHECK(!strcmp(id(root, "c")->pseudo_text[0], "13. 0"));
    CHECK(id(root, "c")->pseudo_text[1] && !strcmp(id(root, "c")->pseudo_text[1], "m/XIII"));
    CHECK(root->style->z_set && root->style->z_index == 3);
    CHECK(id(root, "p")->style->z_index == 4);
    CHECK(!id(root, "i")->style->z_set && !id(root, "k")->style->z_set);     /* nothing typed yet */
    struct style *lc = id(root, "lc")->style;
    CHECK(lc->line_clamp == 3 && lc->max_height.unit == U_PX && lc->max_height.v == 60 && lc->clips);
    CHECK(lc->tab_size == 4 && lc->text_align_last == TA_RIGHT + 1);
    arena_free(&arena);
}
static void cascade_layers(void)
{
    struct arena a = {0};
    const char *html = "<!doctype html><div id=x><span id=y></span></div>";
    struct node *root = html_parse(&a, html, (int)strlen(html));
    const char *first =
        "@layer reset, theme, app;"
        "@layer app { #x {color:red; margin:9px; padding:9px} }"
        "@layer reset { #x {color:blue!important; background-color:red!important} }"
        "@layer theme { #x {color:green!important; margin:4px; padding:4px} }"
        "@layer app { #x {margin-left:revert-layer; padding:revert-layer} }"
        "@layer app { @layer base, overrides; @layer overrides {#y{color:blue}}"
        " @layer base {#y{color:red}} #y {color:green} }"
        "#x{background-color:green!important; border-left:7px solid red}"
        "@layer theme {#x{border-left:3px solid blue}}";
    const char *second =
        "@layer theme { #x {background-color:blue!important} }"
        "@layer app {#x {margin-right:11px}}"
        "@layer {#y {background-color:red}} @layer {#y{background-color:blue}}";
    struct sheet *sheets[] = {css_parse(&a, first, (int)strlen(first),800),
                             css_parse(&a, second, (int)strlen(second),800)};
    css_cascade(&a, root, sheets, 2, 800, 600, NULL, NULL);
    const struct style *x = id(root,"x")->style, *y = id(root,"y")->style;
    CHECK(x->color == OPAQUE(0x0000ff));
    CHECK(x->bg == OPAQUE(0xff0000));
    CHECK(x->margin[3].v == 4 && x->margin[1].v == 11 && x->margin[0].v == 9);
    CHECK(x->padding[0].v == 4 && x->padding[3].v == 4);
    CHECK(x->border_w[3] == 7);
    CHECK(y->color == OPAQUE(0x008000) && y->bg == OPAQUE(0x0000ff));
    arena_free(&a);
}

static void shadow_selectors_and_adoption(void)
{
    struct arena a = {0};
    const char *html = "<!doctype html><div id=host class=card><b slot=title id=light>title</b></div><p id=outside>x</p>";
    struct node *root = html_parse(&a,html,(int)strlen(html));
    struct node *host = id(root,"host");
    const char *inside = "<div><span id=inner>x</span><slot name=title></slot></div>";
    struct node *parsed = html_parse(&a,inside,(int)strlen(inside));
    struct node *shadow = arena_alloc(&a,sizeof(*shadow));
    shadow->type=N_ELEMENT;
    shadow->name=arena_strdup(&a,"#fragment");
    struct node *content=node_find(parsed,T_DIV);
    content->parent=shadow;content->prev=content->next=NULL;
    shadow->first=shadow->last=content;
    shadow->shadow_host=host;host->shadow_root=shadow;
    shadow->adopted_text=":host{color:red} :host(.card){padding:12px} :host > div > span{color:blue}"
        "slot::slotted(b){background-color:green} #outside{color:red} :host{border-left:4px solid red!important}";
    root->adopted_text="#host{color:green;border-left:9px solid blue!important} #outside{color:blue}";
    css_cascade(&a,root,NULL,0,800,600,NULL,NULL);
    CHECK(host->style->color==OPAQUE(0x008000)); /* outer normal rule wins */
    CHECK(host->style->padding[0].v==12 && host->style->border_w[3]==4); /* inner important wins */
    CHECK(id(shadow,"inner")->style->color==OPAQUE(0x0000ff));
    CHECK(id(root,"light")->style->bg==OPAQUE(0x008000));
    CHECK(id(root,"outside")->style->color==OPAQUE(0x0000ff));
    shadow->adopted_text=":host{padding:6px}";
    css_cascade(&a,root,NULL,0,800,600,NULL,NULL);
    CHECK(host->style->padding[0].v==6 && host->style->border_w[3]==9);
    arena_free(&a);
}

int main(void)
{
    entities(); cascade(); cascade_layers(); shadow_selectors_and_adoption(); selector_specificity(); nth_boundaries();
    text_and_interaction_properties(); state_pseudo_classes(); backgrounds_and_overflow(); counters_and_selectors();
    puts("browser HTML/CSS: named references, filtered siblings, cascade specificity and new properties passed");
}
