/* Exercise the production HTML parser and CSS cascade without a display. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
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
int main(void)
{
    entities(); cascade(); selector_specificity(); nth_boundaries();
    puts("browser HTML/CSS: named references, filtered siblings and cascade specificity passed");
}
