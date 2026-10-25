/*
 * CSS: style sheet parsing, selector matching and the cascade.
 *
 * A sheet is a list of rules (selector list + declarations). @media
 * blocks are kept when their query matches a screen of the viewport's
 * width in light mode; other at-rules are skipped. Selectors support
 * type, class, id, attribute and the structural pseudo-classes, with
 * all four combinators; :hover and the like never match, and rules for
 * pseudo-elements are dropped.
 *
 * Each element's declarations are sorted by origin and importance, then
 * specificity, then order, and applied over the inherited style: custom
 * properties first (so var() can use them), then font-size (so em units
 * can), then the rest.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

#define MAX_PARTS   16
#define MAX_MATCHES 512
#define INDEX_BUCKETS 256

enum prop {
    P_UNKNOWN, P_CUSTOM,
    P_DISPLAY, P_COLOR, P_BACKGROUND, P_BACKGROUND_COLOR, P_FONT, P_FONT_SIZE, P_FONT_WEIGHT,
    P_FONT_STYLE, P_FONT_FAMILY, P_LINE_HEIGHT, P_TEXT_ALIGN, P_TEXT_DECORATION,
    P_TEXT_DECORATION_LINE, P_TEXT_TRANSFORM, P_TEXT_INDENT, P_WHITE_SPACE, P_VERTICAL_ALIGN,
    P_LIST_STYLE, P_LIST_STYLE_TYPE, P_MARGIN, P_MARGIN_TOP, P_MARGIN_RIGHT, P_MARGIN_BOTTOM,
    P_MARGIN_LEFT, P_PADDING, P_PADDING_TOP, P_PADDING_RIGHT, P_PADDING_BOTTOM, P_PADDING_LEFT,
    P_BORDER, P_BORDER_TOP, P_BORDER_RIGHT, P_BORDER_BOTTOM, P_BORDER_LEFT, P_BORDER_WIDTH,
    P_BORDER_STYLE, P_BORDER_COLOR, P_BORDER_TOP_WIDTH, P_BORDER_RIGHT_WIDTH,
    P_BORDER_BOTTOM_WIDTH, P_BORDER_LEFT_WIDTH, P_BORDER_TOP_STYLE, P_BORDER_RIGHT_STYLE,
    P_BORDER_BOTTOM_STYLE, P_BORDER_LEFT_STYLE, P_BORDER_TOP_COLOR, P_BORDER_RIGHT_COLOR,
    P_BORDER_BOTTOM_COLOR, P_BORDER_LEFT_COLOR, P_WIDTH, P_HEIGHT, P_MIN_WIDTH, P_MAX_WIDTH,
    P_MIN_HEIGHT, P_BOX_SIZING, P_VISIBILITY, P_FLOAT, P_FLEX_DIRECTION, P_FLEX_FLOW,
    P_BORDER_SPACING, P_BORDER_COLLAPSE, P_POSITION, P_OPACITY, P_LEFT, P_TOP, P_CLIP, P_MASK,
    P_CONTENT, P_BACKGROUND_IMAGE, P_BORDER_RADIUS, P_BORDER_TL_RADIUS, P_BORDER_TR_RADIUS,
    P_BORDER_BR_RADIUS, P_BORDER_BL_RADIUS, P_FLEX, P_FLEX_GROW, P_FLEX_SHRINK, P_FLEX_BASIS,
    P_FLEX_WRAP, P_JUSTIFY_CONTENT, P_ALIGN_ITEMS, P_ALIGN_SELF, P_JUSTIFY_ITEMS, P_JUSTIFY_SELF,
    P_PLACE_CONTENT, P_PLACE_ITEMS, P_PLACE_SELF, P_ORDER, P_GAP, P_ROW_GAP, P_COLUMN_GAP,
    P_OVERFLOW, P_GRID_TEMPLATE_COLUMNS, P_GRID_TEMPLATE, P_GRID_COLUMN, P_GRID_COLUMN_START,
    P_GRID_COLUMN_END, P_GRID_ROW, P_GRID_ROW_START, P_GRID_AREA,
};

static const struct { const char *name; uint8_t prop; } prop_names[] = {
    { "-ms-flex", P_FLEX }, { "-webkit-align-items", P_ALIGN_ITEMS },
    { "-webkit-border-radius", P_BORDER_RADIUS }, { "-webkit-flex", P_FLEX },
    { "-webkit-flex-direction", P_FLEX_DIRECTION }, { "-webkit-flex-wrap", P_FLEX_WRAP },
    { "-webkit-justify-content", P_JUSTIFY_CONTENT }, { "-webkit-mask", P_MASK },
    { "-webkit-mask-image", P_MASK }, { "align-items", P_ALIGN_ITEMS },
    { "align-self", P_ALIGN_SELF }, { "background", P_BACKGROUND },
    { "background-color", P_BACKGROUND_COLOR }, { "background-image", P_BACKGROUND_IMAGE },
    { "border", P_BORDER }, { "border-bottom", P_BORDER_BOTTOM },
    { "border-bottom-color", P_BORDER_BOTTOM_COLOR },
    { "border-bottom-left-radius", P_BORDER_BL_RADIUS },
    { "border-bottom-right-radius", P_BORDER_BR_RADIUS },
    { "border-bottom-style", P_BORDER_BOTTOM_STYLE },
    { "border-bottom-width", P_BORDER_BOTTOM_WIDTH }, { "border-collapse", P_BORDER_COLLAPSE },
    { "border-color", P_BORDER_COLOR }, { "border-left", P_BORDER_LEFT },
    { "border-left-color", P_BORDER_LEFT_COLOR }, { "border-left-style", P_BORDER_LEFT_STYLE },
    { "border-left-width", P_BORDER_LEFT_WIDTH }, { "border-radius", P_BORDER_RADIUS },
    { "border-right", P_BORDER_RIGHT }, { "border-right-color", P_BORDER_RIGHT_COLOR },
    { "border-right-style", P_BORDER_RIGHT_STYLE }, { "border-right-width", P_BORDER_RIGHT_WIDTH },
    { "border-spacing", P_BORDER_SPACING }, { "border-style", P_BORDER_STYLE },
    { "border-top", P_BORDER_TOP }, { "border-top-color", P_BORDER_TOP_COLOR },
    { "border-top-left-radius", P_BORDER_TL_RADIUS },
    { "border-top-right-radius", P_BORDER_TR_RADIUS }, { "border-top-style", P_BORDER_TOP_STYLE },
    { "border-top-width", P_BORDER_TOP_WIDTH }, { "border-width", P_BORDER_WIDTH },
    { "box-sizing", P_BOX_SIZING }, { "clip", P_CLIP }, { "color", P_COLOR },
    { "column-gap", P_COLUMN_GAP }, { "content", P_CONTENT }, { "display", P_DISPLAY },
    { "flex", P_FLEX }, { "flex-basis", P_FLEX_BASIS }, { "flex-direction", P_FLEX_DIRECTION },
    { "flex-flow", P_FLEX_FLOW }, { "flex-grow", P_FLEX_GROW }, { "flex-shrink", P_FLEX_SHRINK },
    { "flex-wrap", P_FLEX_WRAP }, { "float", P_FLOAT }, { "font", P_FONT },
    { "font-family", P_FONT_FAMILY }, { "font-size", P_FONT_SIZE }, { "font-style", P_FONT_STYLE },
    { "font-weight", P_FONT_WEIGHT }, { "gap", P_GAP }, { "grid-area", P_GRID_AREA },
    { "grid-column", P_GRID_COLUMN }, { "grid-column-end", P_GRID_COLUMN_END },
    { "grid-column-gap", P_COLUMN_GAP }, { "grid-column-start", P_GRID_COLUMN_START },
    { "grid-gap", P_GAP }, { "grid-row", P_GRID_ROW }, { "grid-row-gap", P_ROW_GAP },
    { "grid-row-start", P_GRID_ROW_START }, { "grid-template", P_GRID_TEMPLATE },
    { "grid-template-columns", P_GRID_TEMPLATE_COLUMNS }, { "height", P_HEIGHT },
    { "justify-content", P_JUSTIFY_CONTENT }, { "justify-items", P_JUSTIFY_ITEMS },
    { "justify-self", P_JUSTIFY_SELF }, { "left", P_LEFT }, { "line-height", P_LINE_HEIGHT },
    { "list-style", P_LIST_STYLE }, { "list-style-type", P_LIST_STYLE_TYPE },
    { "margin", P_MARGIN }, { "margin-block-end", P_MARGIN_BOTTOM },
    { "margin-block-start", P_MARGIN_TOP }, { "margin-bottom", P_MARGIN_BOTTOM },
    { "margin-inline-end", P_MARGIN_RIGHT }, { "margin-inline-start", P_MARGIN_LEFT },
    { "margin-left", P_MARGIN_LEFT }, { "margin-right", P_MARGIN_RIGHT },
    { "margin-top", P_MARGIN_TOP }, { "mask", P_MASK }, { "mask-image", P_MASK },
    { "max-width", P_MAX_WIDTH }, { "min-height", P_MIN_HEIGHT }, { "min-width", P_MIN_WIDTH },
    { "opacity", P_OPACITY }, { "order", P_ORDER }, { "overflow", P_OVERFLOW },
    { "overflow-x", P_OVERFLOW }, { "padding", P_PADDING },
    { "padding-block-end", P_PADDING_BOTTOM }, { "padding-block-start", P_PADDING_TOP },
    { "padding-bottom", P_PADDING_BOTTOM }, { "padding-inline-end", P_PADDING_RIGHT },
    { "padding-inline-start", P_PADDING_LEFT }, { "padding-left", P_PADDING_LEFT },
    { "padding-right", P_PADDING_RIGHT }, { "padding-top", P_PADDING_TOP },
    { "place-content", P_PLACE_CONTENT }, { "place-items", P_PLACE_ITEMS },
    { "place-self", P_PLACE_SELF }, { "position", P_POSITION }, { "row-gap", P_ROW_GAP },
    { "text-align", P_TEXT_ALIGN }, { "text-decoration", P_TEXT_DECORATION },
    { "text-decoration-line", P_TEXT_DECORATION_LINE }, { "text-indent", P_TEXT_INDENT },
    { "text-transform", P_TEXT_TRANSFORM }, { "top", P_TOP },
    { "vertical-align", P_VERTICAL_ALIGN }, { "visibility", P_VISIBILITY },
    { "white-space", P_WHITE_SPACE }, { "width", P_WIDTH },
};

/* ---- selectors and rules ---- */

enum {
    PS_FIRST_CHILD = 1 << 0, PS_LAST_CHILD = 1 << 1, PS_ONLY_CHILD = 1 << 2, PS_LINK = 1 << 3,
    PS_VISITED = 1 << 4, PS_ROOT = 1 << 5, PS_EMPTY = 1 << 6, PS_NEVER = 1 << 7,
    PS_CHECKED = 1 << 8, PS_DISABLED = 1 << 9, PS_ENABLED = 1 << 10, PS_FIRST_OF_TYPE = 1 << 11,
    PS_LAST_OF_TYPE = 1 << 12,
};

struct attr_sel {
    const char *name, *value;
    char op;                    /* 0 exists, '=', '~', '|', '^', '$', '*' */
    uint8_t nocase;
};

struct compound {
    int tag;                    /* -1: any */
    const char *tagname;        /* for elements we have no tag id for */
    const char *id;
    const char **classes;
    int nclasses;
    struct attr_sel *attrs;
    int nattrs;
    uint32_t pseudo;
    int nth_a, nth_b, has_nth, nth_of_type;
    struct compound *not_list;  /* :not(a, b): matches none of them */
    int nnot;
    struct compound *is_list;   /* :is(a, b) / :where(): matches one of them */
    int nis;
    char comb;                  /* how it relates to the compound before it */
    uint8_t pe;                 /* 1 ::before, 2 ::after */
};

struct selector {
    struct compound *parts;     /* left to right; the last is the subject */
    int nparts;
    uint32_t spec;
    uint8_t pe;                 /* the rule styles the element's ::before (1) or ::after (2) */
};

struct decl {
    uint8_t prop;
    uint8_t important;
    const char *name;           /* custom properties */
    const char *value;
};

struct rule {
    struct selector *sels;
    int nsels;
    struct decl *decls;
    int ndecls;
};

struct ref {                    /* a selector, filed in the index */
    int rule, sel;
    struct ref *next;
};

struct sheet {
    struct rule *rules;
    int nrules, cap;
    struct arena *a;
    /* the index: selectors by what their subject must have */
    struct ref *by_id[INDEX_BUCKETS], *by_class[INDEX_BUCKETS], *by_tag[T_COUNT], *other;
};

/* ---- the user agent style sheet ---- */

static const char ua_css[] =
    "html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form,"
    " header, hr, legend, listing, main, p, plaintext, pre, xmp, article, aside, h1, h2, h3,"
    " h4, h5, h6, hgroup, nav, section, dl, dt, dd, ol, ul, menu, dir, fieldset, details,"
    " summary, frameset, frame, noframes, optgroup { display: block }"
    "head, script, style, link, meta, title, base, template, param, area, map, source, track,"
    " datalist, iframe, object, embed, video, audio, canvas, svg, col, colgroup, option,"
    " input[type=hidden], [hidden], noembed, noscript, dialog:not([open]) { display: none }"
    "li { display: list-item }"
    "table { display: table; border-spacing: 2px; text-indent: 0 }"
    "caption { display: table-caption; text-align: center }"
    "tr { display: table-row }"
    "thead, tbody, tfoot { display: table-row-group }"
    "td, th { display: table-cell; padding: 1px; vertical-align: middle }"
    "th { font-weight: bold; text-align: center }"
    "html { font-family: serif; font-size: 16px; color: black }"
    "body { margin: 8px }"
    "p, dl, figure { margin: 1em 0 }"
    "blockquote, figure { margin: 1em 40px }"
    "h1 { font-size: 2em; margin: .67em 0; font-weight: bold }"
    "h2 { font-size: 1.5em; margin: .83em 0; font-weight: bold }"
    "h3 { font-size: 1.17em; margin: 1em 0; font-weight: bold }"
    "h4 { margin: 1.33em 0; font-weight: bold }"
    "h5 { font-size: .83em; margin: 1.67em 0; font-weight: bold }"
    "h6 { font-size: .67em; margin: 2.33em 0; font-weight: bold }"
    "ul, ol, menu, dir { margin: 1em 0; padding-left: 40px }"
    "ul, menu, dir { list-style-type: disc }"
    "ol { list-style-type: decimal }"
    "ul ul, ol ul, menu ul, ul menu { list-style-type: circle }"
    "ul ul ul, ol ul ul, ul ol ul, ol ol ul { list-style-type: square }"
    "ul ul, ul ol, ol ul, ol ol, ul menu, menu ul { margin-top: 0; margin-bottom: 0 }"
    "dd { margin-left: 40px }"
    "pre, xmp, listing, plaintext { white-space: pre; margin: 1em 0 }"
    "pre, xmp, listing, plaintext, code, kbd, samp, tt { font-family: monospace; font-size: .8125em }"
    "pre code, pre kbd, pre samp, pre tt { font-size: 1em }"
    "b, strong, dt { font-weight: bold }"
    "i, em, cite, var, dfn, address { font-style: italic }"
    "u, ins { text-decoration: underline }"
    "s, strike, del { text-decoration: line-through }"
    "small { font-size: smaller } big { font-size: larger }"
    "sub { vertical-align: sub; font-size: smaller }"
    "sup { vertical-align: super; font-size: smaller }"
    "a:link { color: #0000ee; text-decoration: underline }"
    "a:visited { color: #551a8b; text-decoration: underline }"
    "center { text-align: -webkit-center }"
    "hr { border: 1px inset #808080; margin: .5em auto; height: 0 }"
    "mark { background-color: yellow; color: black }"
    "nobr { white-space: nowrap }"
    "input, select, textarea, button { display: inline-block; font-family: sans-serif;"
    " font-size: 13px; font-weight: normal; font-style: normal; color: black;"
    " text-indent: 0; text-transform: none; line-height: normal }"
    "button { padding: 1px 6px; border: 2px outset #c0c0c0; background-color: #c0c0c0;"
    " text-align: center; white-space: nowrap }"
    "fieldset { margin: 0 2px; padding: .35em .75em .625em; border: 2px groove #c0c0c0 }"
    "legend { padding: 0 2px }"
    "details > :not(summary) { display: none }"
    "details[open] > :not(summary) { display: block }"
    "q::before { content: open-quote } q::after { content: close-quote }"
    "img { vertical-align: baseline }";

/* In quirks mode tables do not inherit these (the HTML standard's rendering section). */
static const char ua_quirks_css[] =
    "table { font-weight: normal; font-style: normal; font-size: medium; line-height: normal;"
    " white-space: normal; text-align: left }";

static struct arena ua_arena;
static struct sheet *ua_sheet, *ua_quirks;

/* ---- low-level scanning ---- */

struct scan {
    const char *s, *end;
};

static void skip_ws(struct scan *sc)
{
    for (;;) {
        while (sc->s < sc->end && isspace((unsigned char)*sc->s))
            sc->s++;
        if (sc->end - sc->s >= 2 && sc->s[0] == '/' && sc->s[1] == '*') {
            const char *e = sc->s + 2;
            while (e + 1 < sc->end && !(e[0] == '*' && e[1] == '/'))
                e++;
            sc->s = e + 1 < sc->end ? e + 2 : sc->end;
            continue;
        }
        if (sc->end - sc->s >= 4 && !strncmp(sc->s, "<!--", 4)) {
            sc->s += 4;
            continue;
        }
        if (sc->end - sc->s >= 3 && !strncmp(sc->s, "-->", 3)) {
            sc->s += 3;
            continue;
        }
        return;
    }
}

/* Advance past a string or a bracketed group starting at *p. */
static const char *skip_group(const char *p, const char *end)
{
    char open = *p, close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}' : open;
    int depth = 0;
    if (open == '"' || open == '\'') {
        for (p++; p < end && *p != open; p++)
            if (*p == '\\' && p + 1 < end)
                p++;
        return p < end ? p + 1 : end;
    }
    for (; p < end; p++) {
        if (*p == '"' || *p == '\'') {
            p = skip_group(p, end) - 1;
        } else if (*p == '/' && p + 1 < end && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e && e < end ? e + 1 : end - 1;
        } else if (*p == open) {
            depth++;
        } else if (*p == close && --depth == 0) {
            return p + 1;
        }
    }
    return end;
}

/* Find the first `c` at nesting depth 0, or end. */
static const char *find_top(const char *p, const char *end, char c)
{
    while (p < end && *p != c) {
        if (*p == '"' || *p == '\'' || *p == '(' || *p == '[' || (*p == '{' && c != '{'))
            p = skip_group(p, end);
        else if (*p == '/' && p + 1 < end && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e && e < end ? e + 2 : end;
        } else
            p++;
    }
    return p;
}

static char *trimmed(struct arena *a, const char *s, const char *e)
{
    while (s < e && isspace((unsigned char)*s))
        s++;
    while (e > s && isspace((unsigned char)e[-1]))
        e--;
    /* Drop comments inside values. */
    char *out = arena_alloc(a, (size_t)(e - s) + 1);
    if (!out)
        return NULL;
    int o = 0;
    for (const char *p = s; p < e; p++) {
        if (*p == '/' && p + 1 < e && p[1] == '*') {
            const char *c = strstr(p + 2, "*/");
            p = c && c < e ? c + 1 : e;
            continue;
        }
        out[o++] = *p;
    }
    out[o] = 0;
    return out;
}

static int is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80 || c == '\\';
}

/* An identifier with escapes resolved ("\:" -> ":"), in the arena. */
static const char *read_ident(struct arena *a, const char **p, const char *end, int lower)
{
    char buf[256];
    int n = 0;
    const char *s = *p;
    while (s < end && is_ident_char(*s)) {
        if (*s == '\\' && s + 1 < end) {
            s++;
            if (isxdigit((unsigned char)*s)) {
                int cp = 0, k = 0;
                while (s < end && k < 6 && isxdigit((unsigned char)*s)) {
                    cp = cp * 16 + (isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10);
                    s++, k++;
                }
                if (s < end && *s == ' ')
                    s++;
                if (n < (int)sizeof(buf) - 4)
                    n += utf8_put(buf + n, cp);
                continue;
            }
        }
        if (n < (int)sizeof(buf) - 1)
            buf[n++] = lower ? (char)tolower((unsigned char)*s) : *s;
        s++;
    }
    *p = s;
    return n ? arena_strndup(a, buf, (size_t)n) : NULL;
}

/* ---- selector parsing ---- */

static int parse_compound_list(struct arena *a, const char *s, const char *end,
                               struct compound **out, int *nout);

static int parse_nth(const char *s, const char *end, int *ra, int *rb)
{
    char buf[64];
    int n = 0;
    for (; s < end && n < 63; s++)
        if (!isspace((unsigned char)*s))
            buf[n++] = (char)tolower((unsigned char)*s);
    buf[n] = 0;
    char *of = strstr(buf, "of");
    if (of)
        *of = 0;                        /* ":nth-child(2n of .x)": the filter is ignored */
    if (!strcmp(buf, "odd")) {
        *ra = 2, *rb = 1;
        return 0;
    }
    if (!strcmp(buf, "even")) {
        *ra = 2, *rb = 0;
        return 0;
    }
    char *nn = strchr(buf, 'n');
    if (!nn) {
        if (!buf[0])
            return -1;
        *ra = 0;
        *rb = atoi(buf);
        return 0;
    }
    *nn = 0;
    *ra = !buf[0] || !strcmp(buf, "+") ? 1 : !strcmp(buf, "-") ? -1 : atoi(buf);
    *rb = nn[1] ? atoi(nn + 1) : 0;
    return 0;
}

/* Parse one compound selector; returns its end, or NULL if it is invalid. */
static const char *parse_compound(struct arena *a, const char *s, const char *end,
                                  struct compound *c, uint32_t *spec)
{
    c->tag = -1;
    const char *start = s;
    if (s < end && *s == '*') {
        s++;
    } else if (s < end && is_ident_char(*s)) {
        const char *name = read_ident(a, &s, end, 1);
        if (!name)
            return NULL;
        c->tag = tag_lookup(name, (int)strlen(name));
        if (c->tag == T_UNKNOWN)
            c->tagname = name;
        *spec += 1;
    }
    if (s < end && *s == '|')
        return NULL;                    /* namespaces */
    const char *classes[16];
    struct attr_sel attrs[8];
    for (;;) {
        if (s >= end)
            break;
        if (*s == '#') {
            s++;
            c->id = read_ident(a, &s, end, 0);
            if (!c->id)
                return NULL;
            *spec += 1 << 20;
        } else if (*s == '.') {
            s++;
            const char *cls = read_ident(a, &s, end, 0);
            if (!cls)
                return NULL;
            if (c->nclasses < 16)
                classes[c->nclasses++] = cls;
            *spec += 1 << 10;
        } else if (*s == '[') {
            const char *close = skip_group(s, end);
            const char *p = s + 1, *e = close - 1;
            struct attr_sel at = { 0 };
            while (p < e && isspace((unsigned char)*p))
                p++;
            at.name = read_ident(a, &p, e, 1);
            if (!at.name)
                return NULL;
            while (p < e && isspace((unsigned char)*p))
                p++;
            if (p < e && *p != '=') {
                at.op = *p++;
                if (!strchr("~|^$*", at.op) || p >= e || *p != '=')
                    return NULL;
            } else if (p < e) {
                at.op = '=';
            }
            if (at.op) {
                p++;
                while (p < e && isspace((unsigned char)*p))
                    p++;
                if (p < e && (*p == '"' || *p == '\'')) {
                    const char *q = skip_group(p, e);
                    at.value = arena_strndup(a, p + 1, (size_t)(q - p - 2 > 0 ? q - p - 2 : 0));
                    p = q;
                } else {
                    at.value = read_ident(a, &p, e, 0);
                }
                if (!at.value)
                    at.value = "";
                while (p < e && isspace((unsigned char)*p))
                    p++;
                if (p < e && (*p == 'i' || *p == 'I'))
                    at.nocase = 1;
            }
            if (c->nattrs < 8)
                attrs[c->nattrs++] = at;
            *spec += 1 << 10;
            s = close;
        } else if (*s == ':') {
            s++;
            int element = s < end && *s == ':';
            if (element)
                s++;
            const char *name = read_ident(a, &s, end, 1);
            if (!name)
                return NULL;
            const char *args = NULL, *args_end = NULL;
            if (s < end && *s == '(') {
                const char *close = skip_group(s, end);
                args = s + 1;
                args_end = close - 1;
                s = close;
            }
            if (!strcmp(name, "before") || !strcmp(name, "after")) {
                c->pe = name[0] == 'b' ? 1 : 2;
                *spec += 1;
                continue;
            }
            if (element || !strcmp(name, "first-line") || !strcmp(name, "first-letter"))
                return NULL;            /* other pseudo-elements: not generated */
            if (!strcmp(name, "first-child"))
                c->pseudo |= PS_FIRST_CHILD;
            else if (!strcmp(name, "last-child"))
                c->pseudo |= PS_LAST_CHILD;
            else if (!strcmp(name, "only-child"))
                c->pseudo |= PS_ONLY_CHILD;
            else if (!strcmp(name, "first-of-type"))
                c->pseudo |= PS_FIRST_OF_TYPE;
            else if (!strcmp(name, "last-of-type"))
                c->pseudo |= PS_LAST_OF_TYPE;
            else if (!strcmp(name, "link") || !strcmp(name, "any-link"))
                c->pseudo |= PS_LINK;
            else if (!strcmp(name, "visited"))
                c->pseudo |= PS_VISITED;
            else if (!strcmp(name, "root"))
                c->pseudo |= PS_ROOT;
            else if (!strcmp(name, "empty"))
                c->pseudo |= PS_EMPTY;
            else if (!strcmp(name, "checked"))
                c->pseudo |= PS_CHECKED;
            else if (!strcmp(name, "disabled"))
                c->pseudo |= PS_DISABLED;
            else if (!strcmp(name, "enabled"))
                c->pseudo |= PS_ENABLED;
            else if ((!strcmp(name, "nth-child") || !strcmp(name, "nth-of-type")) && args) {
                if (parse_nth(args, args_end, &c->nth_a, &c->nth_b) < 0)
                    return NULL;
                c->has_nth = 1;
                c->nth_of_type = name[4] == 'o';
            } else if (!strcmp(name, "not") && args) {
                if (parse_compound_list(a, args, args_end, &c->not_list, &c->nnot) < 0)
                    return NULL;
            } else if ((!strcmp(name, "is") || !strcmp(name, "matches") || !strcmp(name, "where") ||
                        !strcmp(name, "-webkit-any")) && args) {
                if (parse_compound_list(a, args, args_end, &c->is_list, &c->nis) < 0)
                    return NULL;
                if (name[0] == 'w')
                    *spec -= 1 << 10;   /* :where() adds nothing */
            } else {
                c->pseudo |= PS_NEVER;  /* :hover, :focus and anything we do not know */
            }
            *spec += 1 << 10;
        } else {
            break;
        }
    }
    if (s == start)
        return NULL;
    if (c->nclasses) {
        c->classes = arena_alloc(a, sizeof(char *) * (size_t)c->nclasses);
        if (!c->classes)
            return NULL;
        memcpy(c->classes, classes, sizeof(char *) * (size_t)c->nclasses);
    }
    if (c->nattrs) {
        c->attrs = arena_alloc(a, sizeof(struct attr_sel) * (size_t)c->nattrs);
        if (!c->attrs)
            return NULL;
        memcpy(c->attrs, attrs, sizeof(struct attr_sel) * (size_t)c->nattrs);
    }
    return s;
}

/* "a, b.c" inside :not() or :is(): compound selectors only. */
static int parse_compound_list(struct arena *a, const char *s, const char *end,
                               struct compound **out, int *nout)
{
    struct compound list[16];
    int n = 0;
    while (s < end) {
        while (s < end && (isspace((unsigned char)*s) || *s == ','))
            s++;
        if (s >= end)
            break;
        uint32_t spec = 0;
        memset(&list[n], 0, sizeof(list[n]));
        const char *e = parse_compound(a, s, end, &list[n], &spec);
        if (!e)
            return -1;
        while (e < end && isspace((unsigned char)*e))
            e++;
        if (e < end && *e != ',')
            return -1;                  /* combinators inside: not supported */
        if (n < 15)
            n++;
        s = e;
    }
    if (!n)
        return -1;
    *out = arena_alloc(a, sizeof(struct compound) * (size_t)n);
    if (!*out)
        return -1;
    memcpy(*out, list, sizeof(struct compound) * (size_t)n);
    *nout = n;
    return 0;
}

static int parse_selector(struct arena *a, const char *s, const char *end, struct selector *sel)
{
    struct compound parts[MAX_PARTS];
    int n = 0;
    uint32_t spec = 0;
    char comb = 0;
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    while (s < end) {
        while (s < end && isspace((unsigned char)*s)) {
            s++;
            if (n)
                comb = comb ? comb : ' ';
        }
        if (s >= end)
            break;
        if (*s == '>' || *s == '+' || *s == '~') {
            if (!n)
                return -1;
            comb = *s++;
            continue;
        }
        if (n == MAX_PARTS)
            return -1;
        memset(&parts[n], 0, sizeof(parts[n]));
        const char *e = parse_compound(a, s, end, &parts[n], &spec);
        if (!e)
            return -1;
        parts[n].comb = n ? (comb ? comb : ' ') : 0;
        comb = 0;
        n++;
        s = e;
    }
    if (!n || comb)
        return -1;
    for (int i = 0; i < n - 1; i++)
        if (parts[i].pe)
            return -1;                  /* ::before must end the selector */
    sel->pe = parts[n - 1].pe;
    sel->parts = arena_alloc(a, sizeof(struct compound) * (size_t)n);
    if (!sel->parts)
        return -1;
    memcpy(sel->parts, parts, sizeof(struct compound) * (size_t)n);
    sel->nparts = n;
    sel->spec = spec;
    return 0;
}

/* ---- declarations ---- */

static int prop_lookup(const char *name)
{
    int lo = 0, hi = (int)(sizeof(prop_names) / sizeof(prop_names[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(prop_names[mid].name, name);
        if (!c)
            return prop_names[mid].prop;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return P_UNKNOWN;
}

/* Parse "a: b; c: d !important" into decls; returns how many. */
static int parse_decls(struct arena *a, const char *s, const char *end, struct decl **out)
{
    int cap = 8, n = 0;
    struct decl *d = arena_alloc(a, sizeof(*d) * (size_t)cap);
    if (!d)
        return 0;
    while (s < end) {
        const char *semi = find_top(s, end, ';');
        const char *colon = find_top(s, semi, ':');
        if (colon < semi) {
            struct scan sc = { s, colon };
            skip_ws(&sc);
            const char *name_s = sc.s, *name_e = colon;
            while (name_e > name_s && isspace((unsigned char)name_e[-1]))
                name_e--;
            char name[64];
            int nl = (int)(name_e - name_s);
            int custom = nl > 2 && name_s[0] == '-' && name_s[1] == '-';
            if (nl > 0 && nl < (int)sizeof(name) && (custom || !memchr(name_s, '{', (size_t)nl))) {
                for (int i = 0; i < nl; i++)
                    name[i] = custom ? name_s[i] : (char)tolower((unsigned char)name_s[i]);
                name[nl] = 0;
                int prop = custom ? P_CUSTOM : prop_lookup(name);
                if (prop != P_UNKNOWN) {
                    const char *ve = semi;
                    int important = 0;
                    const char *bang = find_top(colon + 1, ve, '!');
                    if (bang < ve) {
                        const char *p = bang + 1;
                        while (p < ve && isspace((unsigned char)*p))
                            p++;
                        if (!strncasecmp(p, "important", 9)) {
                            important = 1;
                            ve = bang;
                        }
                    }
                    char *value = trimmed(a, colon + 1, ve);
                    if (value && (value[0] || custom)) {
                        if (n == cap) {
                            struct decl *nd = arena_alloc(a, sizeof(*nd) * (size_t)cap * 2);
                            if (!nd)
                                break;
                            memcpy(nd, d, sizeof(*d) * (size_t)n);
                            d = nd;
                            cap *= 2;
                        }
                        d[n].prop = (uint8_t)prop;
                        d[n].important = (uint8_t)important;
                        d[n].name = custom ? arena_strdup(a, name) : NULL;
                        d[n].value = value;
                        n++;
                    }
                }
            }
        }
        s = semi < end ? semi + 1 : end;
    }
    *out = d;
    return n;
}

/* ---- media queries ---- */

static int length_px(const char *s, int base);

static int media_feature(const char *f, int vw)
{
    /* "(min-width: 600px)", "(width >= 600px)", "(prefers-color-scheme: dark)" */
    char buf[128];
    int n = 0;
    for (; *f && *f != ')' && n < 127; f++)
        if (*f != '(')
            buf[n++] = (char)tolower((unsigned char)*f);
    buf[n] = 0;
    char *colon = strchr(buf, ':');
    if (colon) {
        *colon = 0;
        char *name = buf, *value = colon + 1;
        while (*name == ' ')
            name++;
        while (*value == ' ')
            value++;
        char *ne = name + strlen(name);
        while (ne > name && ne[-1] == ' ')
            *--ne = 0;
        if (!strcmp(name, "min-width"))
            return vw >= length_px(value, 16);
        if (!strcmp(name, "max-width"))
            return vw <= length_px(value, 16);
        if (!strcmp(name, "width"))
            return vw == length_px(value, 16);
        if (!strcmp(name, "prefers-color-scheme"))
            return !strncmp(value, "light", 5);
        if (!strcmp(name, "orientation"))
            return !strncmp(value, "landscape", 9);
        if (!strcmp(name, "hover") || !strcmp(name, "any-hover"))
            return !strncmp(value, "hover", 5);
        if (!strcmp(name, "pointer") || !strcmp(name, "any-pointer"))
            return !strncmp(value, "fine", 4);
        if (!strcmp(name, "min-device-width"))
            return 1024 >= length_px(value, 16);
        if (!strcmp(name, "max-device-width"))
            return 1024 <= length_px(value, 16);
        return 0;
    }
    /* Range syntax: "width >= 600px", "600px < width" */
    char *op = strpbrk(buf, "<>=");
    if (op && strstr(buf, "width")) {
        int ge = 0, le = 0, gt = 0, lt = 0;
        int width_first = strstr(buf, "width") < op;
        char *val = op;
        while (*val == '<' || *val == '>' || *val == '=' || *val == ' ')
            val++;
        if (op[0] == '>' && op[1] == '=')
            ge = 1;
        else if (op[0] == '<' && op[1] == '=')
            le = 1;
        else if (op[0] == '>')
            gt = 1;
        else if (op[0] == '<')
            lt = 1;
        int px = length_px(width_first ? val : buf, 16);
        if (!width_first) {             /* "600px < width": swap the sense */
            int t;
            t = ge, ge = le, le = t;
            t = gt, gt = lt, lt = t;
        }
        return ge ? vw >= px : le ? vw <= px : gt ? vw > px : lt ? vw < px : vw == px;
    }
    if (!strncmp(buf, "color", 5) || !strcmp(buf, "hover"))
        return 1;
    return 0;
}

static int media_matches(const char *s, const char *end, int vw)
{
    char q[512];
    int n = (int)(end - s) < 511 ? (int)(end - s) : 511;
    for (int i = 0; i < n; i++)
        q[i] = (char)tolower((unsigned char)s[i]);
    q[n] = 0;
    if (!q[strspn(q, " \t\r\n")])
        return 1;
    char *save, *query = strtok_r(q, ",", &save);
    for (; query; query = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*query))
            query++;
        int negate = 0;
        if (!strncmp(query, "not ", 4))
            negate = 1, query += 4;
        else if (!strncmp(query, "only ", 5))
            query += 5;
        while (isspace((unsigned char)*query))
            query++;
        int ok = 1;
        if (!strncmp(query, "print", 5) || !strncmp(query, "speech", 6) ||
            !strncmp(query, "tv", 2) || !strncmp(query, "handheld", 8))
            ok = 0;
        for (char *f = strchr(query, '('); f && ok; f = strchr(f + 1, '(')) {
            if (f > query && !strncmp(f - 4 > query ? f - 4 : query, "not ", 4))
                ok = !media_feature(f, vw);
            else
                ok = media_feature(f, vw);
        }
        if (negate)
            ok = !ok;
        if (ok)
            return 1;
    }
    return 0;
}

/* ---- sheets ---- */

static void add_rule(struct sheet *sh, const char *sel_s, const char *sel_e, const char *body_s,
                     const char *body_e)
{
    struct arena *a = sh->a;
    struct rule r = { 0 };
    r.ndecls = parse_decls(a, body_s, body_e, &r.decls);
    if (!r.ndecls)
        return;
    int max = 1;
    for (const char *s = sel_s; (s = find_top(s, sel_e, ',')) < sel_e; s++)
        max++;
    r.sels = arena_alloc(a, sizeof(struct selector) * (size_t)max);
    if (!r.sels)
        return;
    const char *s = sel_s;
    while (s < sel_e && r.nsels < max) {
        const char *comma = find_top(s, sel_e, ',');
        if (parse_selector(a, s, comma, &r.sels[r.nsels]) == 0)
            r.nsels++;
        s = comma < sel_e ? comma + 1 : sel_e;
    }
    if (!r.nsels)
        return;
    if (sh->nrules == sh->cap) {
        int cap = sh->cap ? sh->cap * 2 : 64;
        struct rule *nr = arena_alloc(a, sizeof(struct rule) * (size_t)cap);
        if (!nr)
            return;
        if (sh->nrules)
            memcpy(nr, sh->rules, sizeof(struct rule) * (size_t)sh->nrules);
        sh->rules = nr;
        sh->cap = cap;
    }
    sh->rules[sh->nrules++] = r;
}

static void parse_block(struct sheet *sh, const char *s, const char *end, int vw, int depth)
{
    struct scan sc = { s, end };
    while (sc.s < sc.end) {
        skip_ws(&sc);
        if (sc.s >= sc.end)
            break;
        if (*sc.s == '}' || *sc.s == ';') {
            sc.s++;
            continue;
        }
        if (*sc.s == '@') {
            const char *kw = sc.s + 1, *k = kw;
            while (k < sc.end && is_ident_char(*k))
                k++;
            int kl = (int)(k - kw);
            const char *brace = find_top(k, sc.end, '{'), *semi = find_top(k, sc.end, ';');
            if (semi < brace) {         /* @import, @charset, @namespace, @layer a, b; */
                sc.s = semi + 1;
                continue;
            }
            if (brace >= sc.end)
                break;
            const char *close = skip_group(brace, sc.end);
            if (depth < 8 && ((kl == 5 && !strncasecmp(kw, "media", 5) && media_matches(k, brace, vw)) ||
                              (kl == 5 && !strncasecmp(kw, "layer", 5)) ||
                              (kl == 8 && !strncasecmp(kw, "document", 8))))
                parse_block(sh, brace + 1, close - 1, vw, depth + 1);
            sc.s = close;
            continue;
        }
        const char *brace = find_top(sc.s, sc.end, '{');
        if (brace >= sc.end)
            break;
        const char *close = skip_group(brace, sc.end);
        add_rule(sh, sc.s, brace, brace + 1, close - 1);
        sc.s = close;
    }
}

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void index_sheet(struct sheet *sh)
{
    for (int r = sh->nrules - 1; r >= 0; r--) {
        for (int i = sh->rules[r].nsels - 1; i >= 0; i--) {
            const struct selector *sel = &sh->rules[r].sels[i];
            const struct compound *c = &sel->parts[sel->nparts - 1];
            struct ref *ref = arena_alloc(sh->a, sizeof(*ref));
            if (!ref)
                return;
            ref->rule = r;
            ref->sel = i;
            struct ref **list;
            if (c->id)
                list = &sh->by_id[hash_str(c->id) % INDEX_BUCKETS];
            else if (c->nclasses)
                list = &sh->by_class[hash_str(c->classes[0]) % INDEX_BUCKETS];
            else if (c->tag > 0)
                list = &sh->by_tag[c->tag];
            else
                list = &sh->other;
            ref->next = *list;
            *list = ref;
        }
    }
}

struct sheet *css_parse(struct arena *a, const char *text, int len, int viewport_w)
{
    struct sheet *sh = arena_alloc(a, sizeof(*sh));
    if (!sh)
        return NULL;
    sh->a = a;
    parse_block(sh, text, text + len, viewport_w, 0);
    index_sheet(sh);
    return sh;
}

/* ---- matching ---- */

struct mctx {
    int (*visited)(const char *href, void *ctx);
    void *ctx;
    int quirks;
};

static const struct node *prev_element(const struct node *n)
{
    for (n = n->prev; n && n->type != N_ELEMENT; n = n->prev)
        ;
    return n;
}

static const struct node *next_element(const struct node *n)
{
    for (n = n->next; n && n->type != N_ELEMENT; n = n->next)
        ;
    return n;
}

static int has_class(const struct node *n, const char *cls)
{
    const char *v = node_attr(n, "class");
    if (!v)
        return 0;
    size_t len = strlen(cls);
    for (const char *p = v; *p;) {
        while (*p && isspace((unsigned char)*p))
            p++;
        const char *e = p;
        while (*e && !isspace((unsigned char)*e))
            e++;
        if ((size_t)(e - p) == len && !strncmp(p, cls, len))
            return 1;
        p = e;
    }
    return 0;
}

static int same_type(const struct node *a, const struct node *b)
{
    return a->tag == b->tag && (a->tag != T_UNKNOWN || !strcmp(a->name, b->name));
}

static int match_attr(const struct node *n, const struct attr_sel *at)
{
    const char *v = node_attr(n, at->name);
    if (!v)
        return 0;
    if (!at->op)
        return 1;
    int (*cmp)(const char *, const char *, size_t) = at->nocase ? strncasecmp : strncmp;
    size_t vl = strlen(v), al = strlen(at->value);
    switch (at->op) {
    case '=':
        return vl == al && !cmp(v, at->value, al);
    case '^':
        return al && vl >= al && !cmp(v, at->value, al);
    case '$':
        return al && vl >= al && !cmp(v + vl - al, at->value, al);
    case '*':
        if (!al)
            return 0;
        for (size_t i = 0; i + al <= vl; i++)
            if (!cmp(v + i, at->value, al))
                return 1;
        return 0;
    case '|':
        return (vl == al || (vl > al && v[al] == '-')) && !cmp(v, at->value, al);
    case '~':
        for (const char *p = v; *p;) {
            while (*p == ' ')
                p++;
            const char *e = p;
            while (*e && *e != ' ')
                e++;
            if ((size_t)(e - p) == al && !cmp(p, at->value, al))
                return 1;
            p = e;
        }
        return 0;
    }
    return 0;
}

static int is_link(const struct node *n)
{
    return (n->tag == T_A || n->tag == T_AREA) && node_attr(n, "href");
}

static int match_compound(const struct compound *c, const struct node *n, const struct mctx *m)
{
    if (c->tag >= 0 && (c->tag != n->tag || (c->tag == T_UNKNOWN && strcmp(c->tagname, n->name))))
        return 0;
    if (c->id) {
        const char *id = node_attr(n, "id");
        if (!id || strcmp(id, c->id))
            return 0;
    }
    for (int i = 0; i < c->nclasses; i++)
        if (!has_class(n, c->classes[i]))
            return 0;
    for (int i = 0; i < c->nattrs; i++)
        if (!match_attr(n, &c->attrs[i]))
            return 0;
    uint32_t ps = c->pseudo;
    if (ps) {
        if (ps & PS_NEVER)
            return 0;
        if ((ps & (PS_FIRST_CHILD | PS_ONLY_CHILD)) && prev_element(n))
            return 0;
        if ((ps & (PS_LAST_CHILD | PS_ONLY_CHILD)) && next_element(n))
            return 0;
        if (ps & PS_FIRST_OF_TYPE)
            for (const struct node *p = prev_element(n); p; p = prev_element(p))
                if (same_type(p, n))
                    return 0;
        if (ps & PS_LAST_OF_TYPE)
            for (const struct node *p = next_element(n); p; p = next_element(p))
                if (same_type(p, n))
                    return 0;
        if (ps & (PS_LINK | PS_VISITED)) {
            if (!is_link(n))
                return 0;
            int v = m->visited && m->visited(node_attr(n, "href"), m->ctx);
            if (((ps & PS_LINK) && v) || ((ps & PS_VISITED) && !v))
                return 0;
        }
        if ((ps & PS_ROOT) && n->parent)
            return 0;
        if (ps & PS_EMPTY)
            for (const struct node *k = n->first; k; k = k->next)
                if (k->type == N_ELEMENT || k->len)
                    return 0;
        if ((ps & PS_CHECKED) && !(node_attr(n, "checked") || node_attr(n, "selected")))
            return 0;
        if ((ps & PS_DISABLED) && !node_attr(n, "disabled"))
            return 0;
        if ((ps & PS_ENABLED) && node_attr(n, "disabled"))
            return 0;
    }
    if (c->has_nth) {
        int idx = 1;
        for (const struct node *p = prev_element(n); p; p = prev_element(p))
            if (!c->nth_of_type || same_type(p, n))
                idx++;
        int d = idx - c->nth_b;
        if (c->nth_a == 0 ? d != 0 : (d % c->nth_a != 0 || d / c->nth_a < 0))
            return 0;
    }
    for (int i = 0; i < c->nnot; i++)
        if (match_compound(&c->not_list[i], n, m))
            return 0;
    if (c->nis) {
        int any = 0;
        for (int i = 0; i < c->nis && !any; i++)
            any = match_compound(&c->is_list[i], n, m);
        if (!any)
            return 0;
    }
    return 1;
}

static int match_from(const struct selector *s, int i, const struct node *n, const struct mctx *m)
{
    if (!match_compound(&s->parts[i], n, m))
        return 0;
    if (i == 0)
        return 1;
    const struct node *p;
    switch (s->parts[i].comb) {
    case '>':
        return n->parent && match_from(s, i - 1, n->parent, m);
    case '+':
        p = prev_element(n);
        return p && match_from(s, i - 1, p, m);
    case '~':
        for (p = prev_element(n); p; p = prev_element(p))
            if (match_from(s, i - 1, p, m))
                return 1;
        return 0;
    default:
        for (p = n->parent; p; p = p->parent)
            if (match_from(s, i - 1, p, m))
                return 1;
        return 0;
    }
}

/* ---- values ---- */

struct vctx {                   /* what relative units are relative to */
    int font_size, root_font_size, vw, vh;
    struct arena *a;            /* for values kept in the style (grid tracks) */
};

static const struct named_color {
    const char *name;
    uint32_t rgb;
} named_colors[] = {
    { "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 }, { "aqua", 0x00ffff },
    { "aquamarine", 0x7fffd4 }, { "azure", 0xf0ffff }, { "beige", 0xf5f5dc }, { "bisque", 0xffe4c4 },
    { "black", 0x000000 }, { "blanchedalmond", 0xffebcd }, { "blue", 0x0000ff },
    { "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a }, { "burlywood", 0xdeb887 },
    { "cadetblue", 0x5f9ea0 }, { "chartreuse", 0x7fff00 }, { "chocolate", 0xd2691e },
    { "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed }, { "cornsilk", 0xfff8dc },
    { "crimson", 0xdc143c }, { "cyan", 0x00ffff }, { "darkblue", 0x00008b }, { "darkcyan", 0x008b8b },
    { "darkgoldenrod", 0xb8860b }, { "darkgray", 0xa9a9a9 }, { "darkgreen", 0x006400 },
    { "darkgrey", 0xa9a9a9 }, { "darkkhaki", 0xbdb76b }, { "darkmagenta", 0x8b008b },
    { "darkolivegreen", 0x556b2f }, { "darkorange", 0xff8c00 }, { "darkorchid", 0x9932cc },
    { "darkred", 0x8b0000 }, { "darksalmon", 0xe9967a }, { "darkseagreen", 0x8fbc8f },
    { "darkslateblue", 0x483d8b }, { "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f },
    { "darkturquoise", 0x00ced1 }, { "darkviolet", 0x9400d3 }, { "deeppink", 0xff1493 },
    { "deepskyblue", 0x00bfff }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
    { "dodgerblue", 0x1e90ff }, { "firebrick", 0xb22222 }, { "floralwhite", 0xfffaf0 },
    { "forestgreen", 0x228b22 }, { "fuchsia", 0xff00ff }, { "gainsboro", 0xdcdcdc },
    { "ghostwhite", 0xf8f8ff }, { "gold", 0xffd700 }, { "goldenrod", 0xdaa520 }, { "gray", 0x808080 },
    { "green", 0x008000 }, { "greenyellow", 0xadff2f }, { "grey", 0x808080 }, { "honeydew", 0xf0fff0 },
    { "hotpink", 0xff69b4 }, { "indianred", 0xcd5c5c }, { "indigo", 0x4b0082 }, { "ivory", 0xfffff0 },
    { "khaki", 0xf0e68c }, { "lavender", 0xe6e6fa }, { "lavenderblush", 0xfff0f5 },
    { "lawngreen", 0x7cfc00 }, { "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 },
    { "lightcoral", 0xf08080 }, { "lightcyan", 0xe0ffff }, { "lightgoldenrodyellow", 0xfafad2 },
    { "lightgray", 0xd3d3d3 }, { "lightgreen", 0x90ee90 }, { "lightgrey", 0xd3d3d3 },
    { "lightpink", 0xffb6c1 }, { "lightsalmon", 0xffa07a }, { "lightseagreen", 0x20b2aa },
    { "lightskyblue", 0x87cefa }, { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 },
    { "lightsteelblue", 0xb0c4de }, { "lightyellow", 0xffffe0 }, { "lime", 0x00ff00 },
    { "limegreen", 0x32cd32 }, { "linen", 0xfaf0e6 }, { "magenta", 0xff00ff }, { "maroon", 0x800000 },
    { "mediumaquamarine", 0x66cdaa }, { "mediumblue", 0x0000cd }, { "mediumorchid", 0xba55d3 },
    { "mediumpurple", 0x9370db }, { "mediumseagreen", 0x3cb371 }, { "mediumslateblue", 0x7b68ee },
    { "mediumspringgreen", 0x00fa9a }, { "mediumturquoise", 0x48d1cc },
    { "mediumvioletred", 0xc71585 }, { "midnightblue", 0x191970 }, { "mintcream", 0xf5fffa },
    { "mistyrose", 0xffe4e1 }, { "moccasin", 0xffe4b5 }, { "navajowhite", 0xffdead },
    { "navy", 0x000080 }, { "oldlace", 0xfdf5e6 }, { "olive", 0x808000 }, { "olivedrab", 0x6b8e23 },
    { "orange", 0xffa500 }, { "orangered", 0xff4500 }, { "orchid", 0xda70d6 },
    { "palegoldenrod", 0xeee8aa }, { "palegreen", 0x98fb98 }, { "paleturquoise", 0xafeeee },
    { "palevioletred", 0xdb7093 }, { "papayawhip", 0xffefd5 }, { "peachpuff", 0xffdab9 },
    { "peru", 0xcd853f }, { "pink", 0xffc0cb }, { "plum", 0xdda0dd }, { "powderblue", 0xb0e0e6 },
    { "purple", 0x800080 }, { "rebeccapurple", 0x663399 }, { "red", 0xff0000 },
    { "rosybrown", 0xbc8f8f }, { "royalblue", 0x4169e1 }, { "saddlebrown", 0x8b4513 },
    { "salmon", 0xfa8072 }, { "sandybrown", 0xf4a460 }, { "seagreen", 0x2e8b57 },
    { "seashell", 0xfff5ee }, { "sienna", 0xa0522d }, { "silver", 0xc0c0c0 }, { "skyblue", 0x87ceeb },
    { "slateblue", 0x6a5acd }, { "slategray", 0x708090 }, { "slategrey", 0x708090 },
    { "snow", 0xfffafa }, { "springgreen", 0x00ff7f }, { "steelblue", 0x4682b4 }, { "tan", 0xd2b48c },
    { "teal", 0x008080 }, { "thistle", 0xd8bfd8 }, { "tomato", 0xff6347 }, { "turquoise", 0x40e0d0 },
    { "violet", 0xee82ee }, { "wheat", 0xf5deb3 }, { "white", 0xffffff }, { "whitesmoke", 0xf5f5f5 },
    { "yellow", 0xffff00 }, { "yellowgreen", 0x9acd32 },
};

/* A number as fixed point times 100; returns the characters used, 0 if none. */
static int parse_num(const char *s, int *out)
{
    const char *p = s;
    int neg = 0;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';
    if (!isdigit((unsigned char)*p) && !(*p == '.' && isdigit((unsigned char)p[1])))
        return 0;
    long v = 0;
    while (isdigit((unsigned char)*p)) {
        if (v < 100000000)
            v = v * 10 + (*p - '0');
        p++;
    }
    v *= 100;
    if (*p == '.') {
        p++;
        int scale = 10;
        while (isdigit((unsigned char)*p)) {
            v += (*p - '0') * scale;
            scale /= 10;
            p++;
        }
    }
    if ((*p == 'e' || *p == 'E') && (isdigit((unsigned char)p[1]) || p[1] == '-')) {
        int e = 0, eneg = 0;
        p++;
        if (*p == '-')
            eneg = 1, p++;
        while (isdigit((unsigned char)*p))
            e = e * 10 + (*p++ - '0');
        while (e-- > 0)
            v = eneg ? v / 10 : v * 10;
    }
    *out = (int)(neg ? -v : v);
    return (int)(p - s);
}

static int hexval(char c)
{
    return isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
}

static int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* Hue to RGB for hsl(), all in fixed point (times 100, hue in degrees). */
static int hue_rgb(int p, int q, int t)
{
    t = ((t % 36000) + 36000) % 36000;
    if (t < 6000)
        return p + (q - p) * t / 6000;
    if (t < 18000)
        return q;
    if (t < 24000)
        return p + (q - p) * (24000 - t) / 6000;
    return p;
}

int css_parse_color(const char *s, int n, uint32_t *out)
{
    char buf[96];
    while (n > 0 && isspace((unsigned char)*s))
        s++, n--;
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    if (n <= 0 || n >= (int)sizeof(buf))
        return -1;
    for (int i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)s[i]);
    buf[n] = 0;
    if (buf[0] == '#') {
        for (int i = 1; i < n; i++)
            if (!isxdigit((unsigned char)buf[i]))
                return -1;
        int r, g, b, al = 255;
        if (n == 4 || n == 5) {
            r = hexval(buf[1]) * 17, g = hexval(buf[2]) * 17, b = hexval(buf[3]) * 17;
            if (n == 5)
                al = hexval(buf[4]) * 17;
        } else if (n == 7 || n == 9) {
            r = hexval(buf[1]) * 16 + hexval(buf[2]);
            g = hexval(buf[3]) * 16 + hexval(buf[4]);
            b = hexval(buf[5]) * 16 + hexval(buf[6]);
            if (n == 9)
                al = hexval(buf[7]) * 16 + hexval(buf[8]);
        } else {
            return -1;
        }
        *out = (uint32_t)al << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        return 0;
    }
    if (!strcmp(buf, "transparent")) {
        *out = TRANSPARENT;
        return 0;
    }
    if (!strncmp(buf, "rgb", 3) || !strncmp(buf, "hsl", 3)) {
        char *p = strchr(buf, '(');
        if (!p)
            return -1;
        p++;
        int v[4] = { 0, 0, 0, 100 }, pct[4] = { 0 }, k = 0;
        while (*p && *p != ')' && k < 4) {
            while (*p == ' ' || *p == ',' || *p == '/')
                p++;
            int used = parse_num(p, &v[k]);
            if (!used)
                return -1;
            p += used;
            if (*p == '%')
                pct[k] = 1, p++;
            else if (!strncmp(p, "deg", 3))
                p += 3;
            k++;
            while (*p == ' ')
                p++;
        }
        if (k < 3)
            return -1;
        int al = pct[3] ? v[3] * 255 / 10000 : v[3] * 255 / 100;
        int r, g, b;
        if (buf[0] == 'r') {
            r = pct[0] ? v[0] * 255 / 10000 : v[0] / 100;
            g = pct[1] ? v[1] * 255 / 10000 : v[1] / 100;
            b = pct[2] ? v[2] * 255 / 10000 : v[2] / 100;
        } else {
            int h = v[0], sat = v[1], l = v[2];      /* degrees*100, percent*100 */
            int q = l < 5000 ? l * (10000 + sat) / 10000 : l + sat - l * sat / 10000;
            int pp = 2 * l - q;
            r = hue_rgb(pp, q, h + 12000) * 255 / 10000;
            g = hue_rgb(pp, q, h) * 255 / 10000;
            b = hue_rgb(pp, q, h - 12000) * 255 / 10000;
        }
        *out = (uint32_t)clamp255(al) << 24 | (uint32_t)clamp255(r) << 16 |
               (uint32_t)clamp255(g) << 8 | (uint32_t)clamp255(b);
        return 0;
    }
    if (!strncmp(buf, "light-dark(", 11)) {         /* we are always in light mode */
        const char *p = buf + 11;
        const char *comma = find_top(p, buf + n, ',');
        return css_parse_color(s + (p - buf), (int)(comma - p), out);
    }
    int lo = 0, hi = (int)(sizeof(named_colors) / sizeof(named_colors[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(named_colors[mid].name, buf);
        if (!c) {
            *out = OPAQUE(named_colors[mid].rgb);
            return 0;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    /* The Windows system colors some old pages use. */
    if (!strcmp(buf, "buttonface") || !strcmp(buf, "threedface") || !strcmp(buf, "menu")) {
        *out = OPAQUE(0xc0c0c0);
        return 0;
    }
    if (!strcmp(buf, "window") || !strcmp(buf, "canvas")) {
        *out = OPAQUE(0xffffff);
        return 0;
    }
    if (!strcmp(buf, "windowtext") || !strcmp(buf, "buttontext") || !strcmp(buf, "canvastext")) {
        *out = OPAQUE(0x000000);
        return 0;
    }
    return -1;
}

/* num (times 100) of `unit` in hundredths of a pixel; 0 for an unknown unit. */
static int unit_px100(const char *unit, long num, const struct vctx *v, long *out)
{
    long px;
    if (!strcmp(unit, "px"))
        px = num;
    else if (!strcmp(unit, "em"))
        px = num * v->font_size;
    else if (!strcmp(unit, "rem"))
        px = num * v->root_font_size;
    else if (!strcmp(unit, "ex") || !strcmp(unit, "ch"))
        px = num * v->font_size / 2;
    else if (!strcmp(unit, "pt"))
        px = num * 4 / 3;
    else if (!strcmp(unit, "pc"))
        px = num * 16;
    else if (!strcmp(unit, "in"))
        px = num * 96;
    else if (!strcmp(unit, "cm"))
        px = num * 96 * 100 / 254;
    else if (!strcmp(unit, "mm"))
        px = num * 96 * 10 / 254;
    else if (!strcmp(unit, "vw") || !strcmp(unit, "dvw") || !strcmp(unit, "svw") || !strcmp(unit, "lvw"))
        px = num * v->vw / 100;
    else if (!strcmp(unit, "vh") || !strcmp(unit, "dvh") || !strcmp(unit, "svh") || !strcmp(unit, "lvh"))
        px = num * v->vh / 100;
    else if (!strcmp(unit, "vmin"))
        px = num * (v->vw < v->vh ? v->vw : v->vh) / 100;
    else if (!strcmp(unit, "vmax"))
        px = num * (v->vw > v->vh ? v->vw : v->vh) / 100;
    else
        return 0;
    *out = px;
    return 1;
}

/* ---- calc(), min(), max() and clamp() ---- */

struct cval {
    long px, pct;               /* hundredths of a pixel, and percent times 100 */
    int num;                    /* a plain number (times 100), held in px */
    int ok;
    int pick;                   /* 1 the smaller, 2 the larger of px and pct (not a sum) */
};

/* A value to do arithmetic on: a pick of px and percent becomes the percent. */
static struct cval calc_flat(struct cval a)
{
    if (a.pick) {
        a.px = 0;
        a.pick = 0;
    }
    return a;
}

static struct cval calc_sum(const char **p, const struct vctx *v);

static void calc_ws(const char **p)
{
    while (isspace((unsigned char)**p))
        (*p)++;
}

/* Of two values, the smaller (or larger). Percentages are unknown until
 * layout, so a pixel size and a percentage are kept for it to choose
 * between (min(100%, 600px)); other mixes keep the one with a percentage. */
static struct cval calc_pick(struct cval a, struct cval b, int larger)
{
    if (!a.ok || !b.ok)
        return a.ok ? a : b;
    a = calc_flat(a);
    b = calc_flat(b);
    if (!a.pct && !b.pct)
        return (larger ? a.px >= b.px : a.px <= b.px) ? a : b;
    if (!a.px && !b.px)
        return (larger ? a.pct >= b.pct : a.pct <= b.pct) ? a : b;
    if ((!a.pct && !b.px) || (!a.px && !b.pct)) {
        struct cval r = { a.px + b.px, a.pct + b.pct, 0, 1, larger ? 2 : 1 };
        return r;
    }
    return a.pct ? a : b;
}

/* min(a, b, ...) (kind 0), max(...) (1) or clamp(lo, val, hi) (2), *p past the '('. */
static struct cval calc_func(const char **p, const struct vctx *v, int kind)
{
    struct cval args[8], r = { 0, 0, 0, 0, 0 };
    int n = 0;
    for (;;) {
        struct cval a = calc_sum(p, v);
        if (n < 8)
            args[n++] = a;
        calc_ws(p);
        if (**p != ',')
            break;
        (*p)++;
    }
    if (**p != ')')
        return r;
    (*p)++;
    if (kind == 2)
        return n == 3 ? calc_pick(args[0], calc_pick(args[1], args[2], 0), 1) : r;
    r = args[0];
    for (int i = 1; i < n; i++)
        r = calc_pick(r, args[i], kind == 1);
    return r;
}

static struct cval calc_term(const char **p, const struct vctx *v)
{
    static const struct { const char *name; int kind; } funcs[] = {
        { "calc(", -1 }, { "-webkit-calc(", -1 }, { "min(", 0 }, { "max(", 1 }, { "clamp(", 2 },
    };
    struct cval r = { 0, 0, 0, 0, 0 };
    calc_ws(p);
    for (size_t i = 0; i < sizeof(funcs) / sizeof(funcs[0]); i++) {
        size_t fl = strlen(funcs[i].name);
        if (strncasecmp(*p, funcs[i].name, fl))
            continue;
        *p += fl;
        if (funcs[i].kind >= 0)
            return calc_func(p, v, funcs[i].kind);
        (*p)--;                         /* calc( is a bracket */
        break;
    }
    if (**p == '(') {
        (*p)++;
        r = calc_sum(p, v);
        calc_ws(p);
        if (**p != ')')
            r.ok = 0;
        else
            (*p)++;
        return r;
    }
    int num, used = parse_num(*p, &num);
    if (!used)
        return r;
    const char *u = *p + used;
    char unit[8];
    int ul = 0;
    while (ul < 7 && (isalpha((unsigned char)u[ul]) || u[ul] == '%'))
        unit[ul] = (char)tolower((unsigned char)u[ul]), ul++;
    unit[ul] = 0;
    *p = u + ul;
    r.ok = 1;
    if (!unit[0])
        r.num = 1, r.px = num;
    else if (!strcmp(unit, "%"))
        r.pct = num;
    else if (!unit_px100(unit, num, v, &r.px))
        r.ok = 0;
    return r;
}

static struct cval calc_product(const char **p, const struct vctx *v)
{
    struct cval a = calc_term(p, v);
    for (;;) {
        calc_ws(p);
        char op = **p;
        if (op != '*' && op != '/')
            return a;
        (*p)++;
        struct cval b = calc_flat(calc_term(p, v));
        a = calc_flat(a);
        if (!a.ok || !b.ok)
            return (struct cval){ 0, 0, 0, 0, 0 };
        if (op == '*' && (b.num || a.num)) {
            struct cval k = b.num ? b : a, x = b.num ? a : b;
            x.px = x.px * k.px / 100;
            x.pct = x.pct * k.px / 100;
            x.num = a.num && b.num;
            a = x;
        } else if (op == '/' && b.num && b.px) {
            a.px = a.px * 100 / b.px;
            a.pct = a.pct * 100 / b.px;
        } else {
            a.ok = 0;
        }
    }
}

static struct cval calc_sum(const char **p, const struct vctx *v)
{
    struct cval a = calc_product(p, v);
    for (;;) {
        calc_ws(p);
        char op = **p;
        /* + and - between terms have spaces round them; "-5px" is a number */
        if ((op != '+' && op != '-') || !isspace((unsigned char)(*p)[1]))
            return a;
        (*p)++;
        struct cval b = calc_flat(calc_product(p, v));
        a = calc_flat(a);
        if (!a.ok || !b.ok)
            return (struct cval){ 0, 0, 0, 0, 0 };
        long sign = op == '-' ? -1 : 1;
        a.px += sign * b.px;
        a.pct += sign * b.pct;
        a.num = a.num && b.num;
    }
}

/*
 * A length in pixels. base is the font size for em; unitless numbers are
 * pixels (as quirks mode allows). Returns 0 for anything unparseable.
 * calc() and friends are worked out, except that percentages stay
 * percentages until layout knows what they are of (U_CALC when mixed).
 */
static int length_value(const char *s, const struct vctx *v, struct len *out)
{
    while (isspace((unsigned char)*s))
        s++;
    out->pct = 0;
    if (!strncasecmp(s, "auto", 4)) {
        out->unit = U_AUTO;
        out->v = 0;
        return 1;
    }
    if (!strncasecmp(s, "calc(", 5) || !strncasecmp(s, "min(", 4) || !strncasecmp(s, "max(", 4) ||
        !strncasecmp(s, "clamp(", 6) || !strncasecmp(s, "-webkit-calc(", 13)) {
        const char *p = s;
        struct cval r = calc_term(&p, v);
        if (!r.ok)
            return 0;
        long px = (r.px + (r.px >= 0 ? 50 : -50)) / 100;
        if (r.pick) {
            out->unit = r.pick == 1 ? U_MIN : U_MAX;
            out->v = (int)px;
            out->pct = (int)r.pct;
        } else if (!r.pct) {
            out->unit = U_PX;
            out->v = (int)px;
        } else if (!px) {
            out->unit = U_PCT;
            out->v = (int)r.pct;
        } else {
            out->unit = U_CALC;
            out->v = (int)px;
            out->pct = (int)r.pct;
        }
        return 1;
    }
    int num;
    int used = parse_num(s, &num);
    if (!used)
        return 0;
    const char *u = s + used;
    char unit[8];
    int ul = 0;
    while (ul < 7 && (isalpha((unsigned char)u[ul]) || u[ul] == '%'))
        unit[ul] = (char)tolower((unsigned char)u[ul]), ul++;
    unit[ul] = 0;
    long px;
    out->unit = U_PX;
    if (!strcmp(unit, "%")) {
        out->unit = U_PCT;
        out->v = num;
        return 1;
    }
    if (!unit[0])
        px = num;
    else if (!unit_px100(unit, num, v, &px))
        return 0;
    out->v = (int)((px + (px >= 0 ? 50 : -50)) / 100);
    return 1;
}

/* A length on a base: percentages (and calc()'s) are of it. */
static int len_on(struct len l, int base)
{
    if (l.unit == U_PCT)
        return base * l.v / 10000;
    if (l.unit == U_CALC)
        return l.v + base * l.pct / 10000;
    if (l.unit == U_MIN || l.unit == U_MAX) {
        int p = base * l.pct / 10000;
        return (l.unit == U_MIN) == (p < l.v) ? p : l.v;
    }
    return l.v;
}

static int length_px(const char *s, int base)
{
    struct vctx v = { base, 16, 1024, 768, NULL };
    struct len l;
    if (!length_value(s, &v, &l) || l.unit != U_PX)
        return 0;
    return l.v;
}

/* Split a value into its top-level space-separated words. */
static int words(const char *s, const char **w, int *wl, int max)
{
    int n = 0;
    const char *end = s + strlen(s);
    while (*s && n < max) {
        while (isspace((unsigned char)*s) || *s == ',')
            s++;
        if (!*s)
            break;
        const char *e = s;
        while (e < end && !isspace((unsigned char)*e) && *e != ',') {
            if (*e == '(' || *e == '"' || *e == '\'')
                e = skip_group(e, end);
            else
                e++;
        }
        w[n] = s;
        wl[n] = (int)(e - s);
        n++;
        s = e;
    }
    return n;
}

static int word_is(const char *w, int wl, const char *kw)
{
    return (int)strlen(kw) == wl && !strncasecmp(w, kw, (size_t)wl);
}

/* ---- applying declarations ---- */

static struct style initial_style;

static void init_initial(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    struct style *s = &initial_style;
    memset(s, 0, sizeof(*s));
    s->display = D_INLINE;
    s->family = FAM_SERIF;
    s->font_size = 16;
    s->color = OPAQUE(0);
    s->bg = TRANSPARENT;
    for (int i = 0; i < 4; i++) {
        s->margin[i].unit = U_PX;
        s->padding[i].unit = U_PX;
        s->border_w[i] = 3;
        s->border_style[i] = BS_NONE;
        s->border_color[i] = 1;         /* currentColor, resolved at the end */
    }
    s->width.unit = s->height.unit = s->max_width.unit = s->min_width.unit = U_AUTO;
    s->min_height.unit = U_PX;
    s->flex_basis.unit = U_AUTO;
    s->flex_shrink = 100;
    s->align_items = s->justify = s->justify_items = AL_NORMAL;
    s->lh_kind = LH_NORMAL;
    s->list_style = LS_DISC;
}

static void inherit(struct style *st, const struct style *p)
{
    *st = initial_style;
    st->family = p->family;
    st->bold = p->bold;
    st->italic = p->italic;
    st->text_align = p->text_align;
    st->center_blocks = p->center_blocks;
    st->white_space = p->white_space;
    st->text_transform = p->text_transform;
    st->decoration = p->decoration;     /* not inherited in CSS, but drawn through children */
    st->list_style = p->list_style;
    st->hidden = p->hidden;
    st->lh_kind = p->lh_kind;
    st->lh = p->lh;
    st->font_size = p->font_size;
    st->color = p->color;
    st->link_color = p->link_color;
    st->border_spacing = p->border_spacing;
    st->vars = p->vars;
    st->text_indent = p->text_indent;
}

static int parse_border_style(const char *w, int wl)
{
    static const char *const names[] = { "none", "solid", "dotted", "dashed", "double", "inset",
                                         "outset", "groove", "ridge", "hidden" };
    for (int i = 0; i < 10; i++)
        if (word_is(w, wl, names[i]))
            return i == 9 ? BS_NONE : i;
    return -1;
}

static int border_width_kw(const char *w, int wl, const struct vctx *v, int *px)
{
    if (word_is(w, wl, "thin"))
        return *px = 1, 1;
    if (word_is(w, wl, "medium"))
        return *px = 3, 1;
    if (word_is(w, wl, "thick"))
        return *px = 5, 1;
    char tmp[32];
    if (wl >= (int)sizeof(tmp))
        return 0;
    memcpy(tmp, w, (size_t)wl);
    tmp[wl] = 0;
    struct len l;
    if (length_value(tmp, v, &l) && l.unit == U_PX && (isdigit((unsigned char)w[0]) || w[0] == '.'))
        return *px = l.v < 0 ? 0 : l.v, 1;
    return 0;
}

/* "1px solid red" for the sides in mask (bit 0 top, 1 right, 2 bottom, 3 left). */
static void apply_border(struct style *st, const char *value, int mask, const struct vctx *v)
{
    const char *w[8];
    int wl[8], n = words(value, w, wl, 8);
    int width = 3, style = BS_NONE;
    uint32_t color = 1;
    for (int i = 0; i < n; i++) {
        int bs = parse_border_style(w[i], wl[i]), px;
        uint32_t c;
        if (bs >= 0)
            style = bs;
        else if (border_width_kw(w[i], wl[i], v, &px))
            width = px;
        else if (css_parse_color(w[i], wl[i], &c) == 0)
            color = c;
    }
    for (int side = 0; side < 4; side++) {
        if (!(mask & 1 << side))
            continue;
        st->border_w[side] = width;
        st->border_style[side] = (uint8_t)style;
        st->border_color[side] = color;
    }
}

/* margin: 1px 2px -> top/bottom 1, left/right 2; returns the values per side. */
static int box_values(const char *value, const struct vctx *v, struct len out[4])
{
    const char *w[4];
    int wl[4], n = words(value, w, wl, 4);
    struct len l[4];
    for (int i = 0; i < n; i++) {
        char tmp[64];
        if (wl[i] >= (int)sizeof(tmp))
            return 0;
        memcpy(tmp, w[i], (size_t)wl[i]);
        tmp[wl[i]] = 0;
        if (!length_value(tmp, v, &l[i]))
            return 0;
    }
    switch (n) {
    case 1: out[0] = out[1] = out[2] = out[3] = l[0]; break;
    case 2: out[0] = out[2] = l[0]; out[1] = out[3] = l[1]; break;
    case 3: out[0] = l[0]; out[1] = out[3] = l[1]; out[2] = l[2]; break;
    case 4: out[0] = l[0]; out[1] = l[1]; out[2] = l[2]; out[3] = l[3]; break;
    default: return 0;
    }
    return 1;
}

static int family_of(const char *value, int fallback)
{
    const char *w[16];
    int wl[16], n = words(value, w, wl, 16);
    for (int i = 0; i < n; i++) {
        char f[64];
        int fl = 0;
        for (int k = 0; k < wl[i] && fl < 63; k++)
            if (w[i][k] != '"' && w[i][k] != '\'')
                f[fl++] = (char)tolower((unsigned char)w[i][k]);
        f[fl] = 0;
        if (strstr(f, "mono") || strstr(f, "courier") || strstr(f, "consol") || strstr(f, "menlo") ||
            strstr(f, "monaco") || !strcmp(f, "fixed") || strstr(f, "code") || strstr(f, "terminal"))
            return FAM_MONO;
        if (!strcmp(f, "serif") || strstr(f, "times") || strstr(f, "georgia") ||
            strstr(f, "garamond") || strstr(f, "palatino") || strstr(f, "book") ||
            strstr(f, "cambria") || strstr(f, "baskerville") || strstr(f, "charter") ||
            strstr(f, "merriweather") || strstr(f, "serif") == f)
            return strstr(f, "sans") ? FAM_SANS : FAM_SERIF;
        if (strstr(f, "sans") || strstr(f, "arial") || strstr(f, "helvetica") ||
            strstr(f, "verdana") || strstr(f, "tahoma") || strstr(f, "system") ||
            strstr(f, "apple") || strstr(f, "segoe") || strstr(f, "roboto") || strstr(f, "ubuntu") ||
            strstr(f, "inter") || strstr(f, "trebuchet") || strstr(f, "lucida") ||
            strstr(f, "geneva") || strstr(f, "open") || strstr(f, "lato") || strstr(f, "noto") ||
            strstr(f, "ui-") == f)
            return FAM_SANS;
        if (!strcmp(f, "cursive") || !strcmp(f, "fantasy"))
            return FAM_SANS;
    }
    return n ? FAM_SANS : fallback;     /* web fonts we do not have: mostly sans */
}

static int font_size_value(const char *value, const struct style *parent, const struct vctx *v,
                           int *px)
{
    static const struct { const char *kw; int px; } kws[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
        { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 },
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
        if (!strcasecmp(value, kws[i].kw))
            return *px = kws[i].px, 1;
    if (!strcasecmp(value, "smaller"))
        return *px = parent->font_size * 5 / 6, 1;
    if (!strcasecmp(value, "larger"))
        return *px = parent->font_size * 6 / 5, 1;
    struct vctx pv = *v;
    pv.font_size = parent->font_size;   /* em in font-size means the parent's */
    struct len l;
    if (!length_value(value, &pv, &l) || l.unit == U_AUTO)
        return 0;
    *px = len_on(l, parent->font_size);
    if (*px < 1)
        *px = 1;
    return 1;
}

static int weight_value(const char *w, int wl, int parent_bold, int *bold)
{
    if (word_is(w, wl, "bold") || word_is(w, wl, "bolder"))
        return *bold = 1, 1;
    if (word_is(w, wl, "normal") || word_is(w, wl, "lighter"))
        return *bold = 0, 1;
    if (wl == 3 && isdigit((unsigned char)w[0]) && w[1] == '0' && w[2] == '0')
        return *bold = w[0] >= '6', 1;
    (void)parent_bold;
    return 0;
}

static void apply_font_shorthand(struct style *st, const struct style *parent, const char *value,
                                 const struct vctx *v)
{
    const char *w[16];
    int wl[16], n = words(value, w, wl, 16), i = 0;
    st->bold = 0;
    st->italic = 0;
    for (; i < n; i++) {
        int b;
        if (word_is(w[i], wl[i], "italic") || word_is(w[i], wl[i], "oblique"))
            st->italic = 1;
        else if (weight_value(w[i], wl[i], parent->bold, &b))
            st->bold = (uint8_t)b;
        else if (word_is(w[i], wl[i], "normal") || word_is(w[i], wl[i], "small-caps"))
            ;
        else
            break;
    }
    if (i >= n)
        return;
    char size[64];
    const char *slash = memchr(w[i], '/', (size_t)wl[i]);
    int sl = slash ? (int)(slash - w[i]) : wl[i];
    if (sl >= (int)sizeof(size))
        return;
    memcpy(size, w[i], (size_t)sl);
    size[sl] = 0;
    int px;
    if (font_size_value(size, parent, v, &px))
        st->font_size = px;
    st->lh_kind = LH_NORMAL;
    if (slash) {
        char lh[32];
        int ll = wl[i] - sl - 1;
        if (ll > 0 && ll < (int)sizeof(lh)) {
            memcpy(lh, slash + 1, (size_t)ll);
            lh[ll] = 0;
            int num;
            int used = parse_num(lh, &num);
            if (used && !lh[used])
                st->lh_kind = LH_FACTOR, st->lh = num;
            else if (used) {
                struct vctx fv = *v;
                fv.font_size = st->font_size;
                struct len l;
                if (length_value(lh, &fv, &l))
                    st->lh_kind = LH_PX,
                    st->lh = len_on(l, st->font_size);
            }
        }
    }
    if (i + 1 < n)
        st->family = (uint8_t)family_of(w[i + 1], st->family);
}

/* ---- flex, grid and background helpers ---- */

/* justify-content, align-items and the rest: the last word decides
 * ("safe center", "first baseline"). */
static int align_value(const char *value, int *out)
{
    static const struct { const char *kw; uint8_t al; } kws[] = {
        { "auto", AL_AUTO }, { "normal", AL_NORMAL }, { "stretch", AL_STRETCH },
        { "start", AL_START }, { "flex-start", AL_START }, { "self-start", AL_START },
        { "left", AL_START }, { "end", AL_END }, { "flex-end", AL_END }, { "self-end", AL_END },
        { "right", AL_END }, { "center", AL_CENTER }, { "baseline", AL_BASELINE },
        { "space-between", AL_BETWEEN }, { "space-around", AL_AROUND },
        { "space-evenly", AL_EVENLY },
    };
    const char *w[4];
    int wl[4], n = words(value, w, wl, 4);
    if (!n)
        return 0;
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
        if (word_is(w[n - 1], wl[n - 1], kws[i].kw))
            return *out = kws[i].al, 1;
    return 0;
}

/* The first of a value's words, NUL-terminated in buf. */
static const char *first_word(const char *value, char *buf, int size, const char **rest)
{
    const char *w[2];
    int wl[2], n = words(value, w, wl, 2);
    buf[0] = 0;
    if (n && wl[0] < size) {
        memcpy(buf, w[0], (size_t)wl[0]);
        buf[wl[0]] = 0;
    }
    if (rest)
        *rest = n > 1 ? w[1] : NULL;
    return buf;
}

/*
 * A linear-gradient() somewhere in a background value: its first and last
 * colors and which way it runs (1 down, 2 right; swapped for up and left).
 * Radial gradients come out as one color, between their first and last.
 */
static int parse_gradient(const char *value, uint32_t *g0, uint32_t *g1, uint8_t *dir)
{
    const char *g = strcasestr(value, "gradient(");
    if (!g)
        return 0;
    int radial = g >= value + 6 && !strncasecmp(g - 6, "radial", 6);
    const char *p = strchr(g, '(') + 1, *end = skip_group(p - 1, p + strlen(p)) - 1;
    int down = 1, horizontal = 0, ncolors = 0;
    for (int arg = 0; p < end; arg++) {
        const char *e = find_top(p, end, ',');
        char buf[128];
        int len = (int)(e - p) < (int)sizeof(buf) - 1 ? (int)(e - p) : (int)sizeof(buf) - 1;
        memcpy(buf, p, (size_t)len);
        buf[len] = 0;
        p = e + 1;
        const char *w[4];
        int wl[4], n = words(buf, w, wl, 4);
        if (!n)
            continue;
        uint32_t col;
        if (css_parse_color(w[0], wl[0], &col) == 0) {
            if (!ncolors++)
                *g0 = col;
            *g1 = col;
            continue;
        }
        if (arg || radial)
            continue;
        /* the direction: "to right", or an angle */
        if (word_is(w[0], wl[0], "to")) {
            for (int i = 1; i < n; i++) {
                if (word_is(w[i], wl[i], "right") || word_is(w[i], wl[i], "left")) {
                    horizontal = n == 2;        /* "to bottom right" stays vertical */
                    if (horizontal)
                        down = word_is(w[i], wl[i], "right");
                } else if (word_is(w[i], wl[i], "top")) {
                    down = 0;
                }
            }
        } else {
            int num, used = parse_num(w[0], &num);
            if (!used)
                continue;
            long deg = num;                     /* times 100 */
            if (!strncasecmp(w[0] + used, "turn", 4))
                deg = (long)num * 360;
            else if (!strncasecmp(w[0] + used, "rad", 3))
                deg = (long)num * 5730 / 100;
            deg = (deg / 100 % 360 + 360) % 360;
            horizontal = (deg > 45 && deg < 135) || (deg > 225 && deg < 315);
            down = horizontal ? deg < 180 : deg > 90 && deg < 270;
        }
    }
    if (!ncolors)
        return 0;
    if (radial || ncolors == 1) {
        uint32_t mix = 0;
        for (int sh = 0; sh < 32; sh += 8)
            mix |= (((*g0 >> sh & 0xFF) + (*g1 >> sh & 0xFF)) / 2) << sh;
        *g0 = *g1 = mix;
    } else if (!down) {
        uint32_t t = *g0;
        *g0 = *g1;
        *g1 = t;
    }
    *dir = horizontal ? 2 : 1;
    return 1;
}

/* One track size: 100px, 20%, 1fr, auto, min-content, max-content. */
static int track_size(const char *w, int wl, const struct vctx *v, uint8_t *kind, int *val)
{
    char buf[64];
    if (wl <= 0 || wl >= (int)sizeof(buf))
        return 0;
    memcpy(buf, w, (size_t)wl);
    buf[wl] = 0;
    *val = 0;
    if (!strcasecmp(buf, "auto") || !strncasecmp(buf, "fit-content", 11))
        return *kind = TK_AUTO, 1;
    if (!strcasecmp(buf, "min-content"))
        return *kind = TK_MIN, 1;
    if (!strcasecmp(buf, "max-content"))
        return *kind = TK_MAX, 1;
    int num, used = parse_num(buf, &num);
    if (used && !strcasecmp(buf + used, "fr"))
        return *kind = TK_FR, *val = num, 1;
    struct len l;
    if (!length_value(buf, v, &l) || l.unit == U_AUTO)
        return 0;
    *kind = l.unit == U_PCT ? TK_PCT : TK_PX;     /* min(100%, 250px) and calc() go by their pixels */
    *val = l.v;
    return 1;
}

static void add_track(struct gridspec *g, const char *w, int wl, const struct vctx *v)
{
    if (g->n >= MAX_TRACKS)
        return;
    struct track *t = &g->t[g->n];
    if (wl > 7 && !strncasecmp(w, "minmax(", 7)) {
        const char *in = w + 7, *end = w + wl - 1;
        const char *comma = find_top(in, end, ',');
        if (comma >= end)
            return;
        while (in < comma && isspace((unsigned char)*in))
            in++;
        const char *b = comma + 1;
        while (b < end && isspace((unsigned char)*b))
            b++;
        const char *ae = comma, *be = end;
        while (ae > in && isspace((unsigned char)ae[-1]))
            ae--;
        while (be > b && isspace((unsigned char)be[-1]))
            be--;
        if (!track_size(in, (int)(ae - in), v, &t->min, &t->vmin) ||
            !track_size(b, (int)(be - b), v, &t->max, &t->vmax))
            return;
        if (t->min == TK_FR)
            t->min = TK_AUTO;
    } else {
        if (!track_size(w, wl, v, &t->max, &t->vmax))
            return;
        t->min = t->max, t->vmin = t->vmax;
        if (t->max == TK_FR)                    /* 1fr is minmax(auto, 1fr) */
            t->min = TK_AUTO, t->vmin = 0;
    }
    g->n++;
}

/* grid-template-columns: "200px 1fr", "repeat(3, 1fr)", "repeat(auto-fill, minmax(180px, 1fr))". */
static const struct gridspec *parse_tracks(const char *value, const struct vctx *v)
{
    if (!v->a)
        return NULL;
    struct gridspec *g = arena_alloc(v->a, sizeof(*g));
    if (!g)
        return NULL;
    memset(g, 0, sizeof(*g));
    const char *w[64];
    int wl[64], n = words(value, w, wl, 64), in_names = 0;
    for (int i = 0; i < n; i++) {
        if (in_names || w[i][0] == '[') {               /* [line names] */
            in_names = w[i][wl[i] - 1] != ']';
            continue;
        }
        if (wl[i] > 7 && !strncasecmp(w[i], "repeat(", 7)) {
            char inner[256];
            int len = wl[i] - 8;
            if (len <= 0 || len >= (int)sizeof(inner))
                continue;
            memcpy(inner, w[i] + 7, (size_t)len);
            inner[len] = 0;
            const char *rw[32];
            int rwl[32], rn = words(inner, rw, rwl, 32);
            if (rn < 2)
                continue;
            int count = 1, autorep = 0;
            if (word_is(rw[0], rwl[0], "auto-fill"))
                autorep = 1;
            else if (word_is(rw[0], rwl[0], "auto-fit"))
                autorep = 2;
            else
                count = atoi(rw[0]);
            if (autorep && !g->auto_repeat) {
                g->auto_repeat = (uint8_t)autorep;
                g->rep = g->n;
            }
            for (int k = 0; k < (count < MAX_TRACKS ? count : MAX_TRACKS); k++) {
                int names = 0;
                for (int j = 1; j < rn; j++) {
                    if (names || rw[j][0] == '[') {
                        names = rw[j][rwl[j] - 1] != ']';
                        continue;
                    }
                    add_track(g, rw[j], rwl[j], v);
                }
            }
            if (autorep && g->auto_repeat == autorep && !g->nrep)
                g->nrep = g->n - g->rep;
            continue;
        }
        add_track(g, w[i], wl[i], v);
    }
    if (g->auto_repeat && !g->nrep)
        g->auto_repeat = 0;
    return g;
}

/* A grid line: "3", "-1", "span 2"; names and auto give 0. */
static void grid_line(const char *s, const char *end, int *line, int *span)
{
    char buf[64];
    while (s < end && isspace((unsigned char)*s))
        s++;
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    int len = (int)(end - s) < (int)sizeof(buf) - 1 ? (int)(end - s) : (int)sizeof(buf) - 1;
    memcpy(buf, s, (size_t)len);
    buf[len] = 0;
    *line = 0;
    if (span)
        *span = 0;
    if (!strncasecmp(buf, "span", 4)) {
        if (span)
            *span = atoi(buf + 4) > 0 ? atoi(buf + 4) : 1;
        return;
    }
    if (isdigit((unsigned char)buf[0]) || buf[0] == '-')
        *line = atoi(buf);
}

/* "start / end": a line and a span, or a line to a line. */
static void grid_placement(const char *value, int *start, int *end, int *span)
{
    const char *vend = value + strlen(value), *slash = find_top(value, vend, '/');
    int sspan, espan;
    grid_line(value, slash, start, &sspan);
    *end = 0;
    *span = sspan;
    if (slash < vend) {
        grid_line(slash + 1, vend, end, &espan);
        if (espan)
            *span = espan;
    }
}

static void set_len(struct len *dst, const char *value, const struct vctx *v)
{
    struct len l;
    if (!strcasecmp(value, "none"))
        dst->unit = U_AUTO, dst->v = 0;
    else if (length_value(value, v, &l))
        *dst = l;
}

static void apply(struct style *st, const struct style *parent, int prop, const char *value,
                  const struct vctx *v)
{
    const char *w[8];
    int wl[8], n;
    uint32_t c;
    struct len box[4];
    int inherit_kw = !strcasecmp(value, "inherit");
    int initial_kw = !strcasecmp(value, "initial") || !strcasecmp(value, "unset") ||
                     !strcasecmp(value, "revert") || !strcasecmp(value, "revert-layer");

    switch (prop) {
    case P_DISPLAY:
        if (inherit_kw) {
            st->display = parent->display;
            st->inner = parent->inner;
        } else {
            static const struct { const char *kw; uint8_t d, in; } ds[] = {
                { "none", D_NONE, 0 }, { "block", D_BLOCK, 0 }, { "inline", D_INLINE, 0 },
                { "inline-block", D_INLINE_BLOCK, 0 }, { "list-item", D_LIST_ITEM, 0 },
                { "table", D_TABLE, 0 }, { "inline-table", D_INLINE_TABLE, 0 },
                { "table-row", D_TABLE_ROW, 0 }, { "table-row-group", D_TABLE_ROW_GROUP, 0 },
                { "table-header-group", D_TABLE_ROW_GROUP, 0 },
                { "table-footer-group", D_TABLE_ROW_GROUP, 0 }, { "table-cell", D_TABLE_CELL, 0 },
                { "table-caption", D_TABLE_CAPTION, 0 }, { "table-column", D_NONE, 0 },
                { "table-column-group", D_NONE, 0 }, { "flow-root", D_BLOCK, 0 },
                { "contents", D_INLINE, 0 }, { "run-in", D_BLOCK, 0 }, { "-webkit-box", D_BLOCK, 0 },
                { "inline flow-root", D_INLINE_BLOCK, 0 },
                { "flex", D_BLOCK, IN_FLEX }, { "block flex", D_BLOCK, IN_FLEX },
                { "-ms-flexbox", D_BLOCK, IN_FLEX }, { "-webkit-flex", D_BLOCK, IN_FLEX },
                { "inline-flex", D_INLINE_BLOCK, IN_FLEX }, { "inline flex", D_INLINE_BLOCK, IN_FLEX },
                { "-webkit-inline-flex", D_INLINE_BLOCK, IN_FLEX },
                { "-ms-inline-flexbox", D_INLINE_BLOCK, IN_FLEX },
                { "grid", D_BLOCK, IN_GRID }, { "block grid", D_BLOCK, IN_GRID },
                { "inline-grid", D_INLINE_BLOCK, IN_GRID }, { "inline grid", D_INLINE_BLOCK, IN_GRID },
            };
            for (size_t i = 0; i < sizeof(ds) / sizeof(ds[0]); i++)
                if (!strcasecmp(value, ds[i].kw)) {
                    st->display = ds[i].d;
                    st->inner = ds[i].in;
                }
        }
        break;
    case P_FLEX_DIRECTION:
    case P_FLEX_FLOW:
        n = words(value, w, wl, 8);
        for (int i = 0; i < n; i++) {
            if (!strncasecmp(w[i], "row", 3) || !strncasecmp(w[i], "column", 6)) {
                st->flex_column = !strncasecmp(w[i], "column", 6);
                st->flex_reverse = wl[i] > 8 && !strncasecmp(w[i] + wl[i] - 8, "-reverse", 8);
            } else if (word_is(w[i], wl[i], "wrap") || word_is(w[i], wl[i], "wrap-reverse")) {
                st->flex_wrap = 1;
            } else if (word_is(w[i], wl[i], "nowrap")) {
                st->flex_wrap = 0;
            }
        }
        break;
    case P_FLEX_WRAP:
        st->flex_wrap = !strncasecmp(value, "wrap", 4);
        break;
    case P_FLEX:
        if (!strcasecmp(value, "none")) {
            st->flex_grow = st->flex_shrink = 0;
            st->flex_basis.unit = U_AUTO;
        } else if (!strcasecmp(value, "auto")) {
            st->flex_grow = st->flex_shrink = 100;
            st->flex_basis.unit = U_AUTO;
        } else if (initial_kw) {
            st->flex_grow = 0, st->flex_shrink = 100;
            st->flex_basis.unit = U_AUTO;
        } else {
            int nums = 0, num, basis = 0;
            n = words(value, w, wl, 8);
            for (int i = 0; i < n; i++) {
                int used = parse_num(w[i], &num);
                if (used == wl[i] && nums < 2 && !basis) {
                    if (nums++ == 0)
                        st->flex_grow = num > 0 ? num : 0;
                    else
                        st->flex_shrink = num > 0 ? num : 0;
                } else {
                    char tmp[64];
                    snprintf(tmp, sizeof(tmp), "%.*s", wl[i], w[i]);
                    struct len l;
                    if (!strcasecmp(tmp, "content"))
                        st->flex_basis.unit = U_AUTO, basis = 1;
                    else if (length_value(tmp, v, &l))
                        st->flex_basis = l, basis = 1;
                }
            }
            if (nums == 1 || (!nums && basis))     /* flex: 2 and flex: 200px */
                st->flex_shrink = 100;
            if (!nums && basis)
                st->flex_grow = 100;
            if (nums && !basis)                 /* flex: 1 is 1 1 0% */
                st->flex_basis = (struct len){ .unit = U_PX };
        }
        break;
    case P_FLEX_GROW:
    case P_FLEX_SHRINK: {
        int num;
        if (parse_num(value, &num) && num >= 0)
            *(prop == P_FLEX_GROW ? &st->flex_grow : &st->flex_shrink) = num;
        else if (initial_kw)
            *(prop == P_FLEX_GROW ? &st->flex_grow : &st->flex_shrink) = prop == P_FLEX_GROW ? 0 : 100;
        break;
    }
    case P_FLEX_BASIS:
        if (!strcasecmp(value, "content") || initial_kw)
            st->flex_basis.unit = U_AUTO;
        else
            set_len(&st->flex_basis, value, v);
        break;
    case P_JUSTIFY_CONTENT:
    case P_ALIGN_ITEMS:
    case P_ALIGN_SELF:
    case P_JUSTIFY_ITEMS:
    case P_JUSTIFY_SELF: {
        int al;
        if (!align_value(value, &al))
            break;
        switch (prop) {
        case P_JUSTIFY_CONTENT: st->justify = (uint8_t)al; break;
        case P_ALIGN_ITEMS: st->align_items = (uint8_t)al; break;
        case P_ALIGN_SELF: st->align_self = (uint8_t)al; break;
        case P_JUSTIFY_ITEMS: st->justify_items = (uint8_t)al; break;
        default: st->justify_self = (uint8_t)al; break;
        }
        break;
    }
    case P_PLACE_CONTENT:
    case P_PLACE_ITEMS:
    case P_PLACE_SELF: {
        /* align, then justify (or the same for both) */
        char first[64];
        const char *rest;
        int a1, a2;
        first_word(value, first, sizeof(first), &rest);
        if (!align_value(first, &a1))
            break;
        if (!rest || !align_value(rest, &a2))
            a2 = a1;
        if (prop == P_PLACE_CONTENT)
            st->justify = (uint8_t)a2;
        else if (prop == P_PLACE_ITEMS)
            st->align_items = (uint8_t)a1, st->justify_items = (uint8_t)a2;
        else
            st->align_self = (uint8_t)a1, st->justify_self = (uint8_t)a2;
        break;
    }
    case P_ORDER: {
        int num;
        if (parse_num(value, &num))
            st->order = num / 100;
        break;
    }
    case P_GAP:
    case P_ROW_GAP:
    case P_COLUMN_GAP: {
        struct len l[2] = { { .unit = U_PX }, { .unit = U_PX } };
        n = words(value, w, wl, 8);
        for (int i = 0; i < n && i < 2; i++) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%.*s", wl[i], w[i]);
            if (!length_value(tmp, v, &l[i]) || l[i].unit == U_AUTO)
                l[i] = (struct len){ .unit = U_PX };    /* normal */
        }
        if (n == 1)
            l[1] = l[0];
        if (prop != P_COLUMN_GAP)
            st->row_gap = l[0];
        if (prop != P_ROW_GAP)
            st->col_gap = prop == P_GAP ? l[1] : l[0];
        break;
    }
    case P_OVERFLOW:
        st->clips = strcasecmp(value, "visible") != 0 && !inherit_kw;
        break;
    case P_GRID_TEMPLATE_COLUMNS:
    case P_GRID_TEMPLATE: {
        const char *cols = value;
        if (prop == P_GRID_TEMPLATE) {          /* rows / columns */
            const char *slash = find_top(value, value + strlen(value), '/');
            if (!*slash) {
                st->grid_cols = NULL;
                break;
            }
            cols = slash + 1;
        }
        st->grid_cols = !strcasecmp(cols, "none") || initial_kw ? NULL : parse_tracks(cols, v);
        break;
    }
    case P_GRID_COLUMN:
        grid_placement(value, &st->gc_start, &st->gc_end, &st->gc_span);
        break;
    case P_GRID_COLUMN_START: {
        int span;
        grid_line(value, value + strlen(value), &st->gc_start, &span);
        if (span)
            st->gc_span = span;
        break;
    }
    case P_GRID_COLUMN_END: {
        int span;
        grid_line(value, value + strlen(value), &st->gc_end, &span);
        if (span)
            st->gc_span = span;
        break;
    }
    case P_GRID_ROW: {
        int end;
        grid_placement(value, &st->gr_start, &end, &st->gr_span);
        if (end > st->gr_start && st->gr_start > 0)
            st->gr_span = end - st->gr_start;
        break;
    }
    case P_GRID_ROW_START: {
        int span;
        grid_line(value, value + strlen(value), &st->gr_start, &span);
        if (span)
            st->gr_span = span;
        break;
    }
    case P_GRID_AREA: {
        /* row-start / column-start / row-end / column-end */
        const char *e = value + strlen(value), *p = value;
        int part[4] = { 0, 0, 0, 0 }, spans[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 4 && p <= e; i++) {
            const char *slash = find_top(p, e, '/');
            grid_line(p, slash, &part[i], &spans[i]);
            p = slash + 1;
        }
        st->gr_start = part[0], st->gc_start = part[1], st->gc_end = part[3];
        st->gr_span = spans[2] ? spans[2] : part[2] > part[0] && part[0] > 0 ? part[2] - part[0] : 0;
        st->gc_span = spans[3];
        break;
    }
    case P_BORDER_RADIUS: {
        char first[256];
        snprintf(first, sizeof(first), "%.*s", (int)strcspn(value, "/"), value);
        if (box_values(first, v, box))          /* the same 1-4 value pattern */
            memcpy(st->radius, box, sizeof(box));
        break;
    }
    case P_BORDER_TL_RADIUS: case P_BORDER_TR_RADIUS: case P_BORDER_BR_RADIUS:
    case P_BORDER_BL_RADIUS: {
        char first[64];
        set_len(&st->radius[prop - P_BORDER_TL_RADIUS], first_word(value, first, sizeof(first), NULL), v);
        break;
    }
    case P_BACKGROUND_IMAGE:
        if (!parse_gradient(value, &st->g0, &st->g1, &st->grad))
            st->grad = 0;
        break;
    case P_FLOAT:
        st->floated = !strcasecmp(value, "left") || !strcasecmp(value, "right");
        break;
    case P_COLOR:
        if (inherit_kw)
            st->color = parent->color;
        else if (!strcasecmp(value, "currentcolor"))
            ;
        else if (css_parse_color(value, (int)strlen(value), &c) == 0)
            st->color = ALPHA(c) ? c : st->color;
        break;
    case P_BACKGROUND_COLOR:
        if (inherit_kw)
            st->bg = parent->bg;
        else if (initial_kw)
            st->bg = TRANSPARENT;
        else if (!strcasecmp(value, "currentcolor"))
            st->bg = st->color;
        else if (css_parse_color(value, (int)strlen(value), &c) == 0)
            st->bg = c;
        break;
    case P_BACKGROUND:
        if (!parse_gradient(value, &st->g0, &st->g1, &st->grad))
            st->grad = 0;
        if (!strcasecmp(value, "none") || initial_kw) {
            st->bg = TRANSPARENT;
            break;
        }
        n = words(value, w, wl, 8);
        for (int i = 0; i < n; i++)
            if (css_parse_color(w[i], wl[i], &c) == 0)
                st->bg = c;
        break;
    case P_FONT_SIZE: {
        int px;
        if (inherit_kw || initial_kw)
            st->font_size = inherit_kw ? parent->font_size : 16;
        else if (font_size_value(value, parent, v, &px))
            st->font_size = px;
        break;
    }
    case P_FONT_WEIGHT: {
        int b;
        if (inherit_kw)
            st->bold = parent->bold;
        else if (weight_value(value, (int)strlen(value), parent->bold, &b))
            st->bold = (uint8_t)b;
        break;
    }
    case P_FONT_STYLE:
        st->italic = !strncasecmp(value, "italic", 6) || !strncasecmp(value, "oblique", 7);
        break;
    case P_FONT_FAMILY:
        if (!inherit_kw)
            st->family = (uint8_t)family_of(value, st->family);
        break;
    case P_FONT:
        if (!inherit_kw)
            apply_font_shorthand(st, parent, value, v);
        break;
    case P_LINE_HEIGHT: {
        int num, used = parse_num(value, &num);
        struct len l;
        if (!strcasecmp(value, "normal") || initial_kw)
            st->lh_kind = LH_NORMAL;
        else if (inherit_kw)
            st->lh_kind = parent->lh_kind, st->lh = parent->lh;
        else if (used && !value[used])
            st->lh_kind = LH_FACTOR, st->lh = num;
        else if (length_value(value, v, &l) && l.unit != U_AUTO)
            st->lh_kind = LH_PX, st->lh = len_on(l, st->font_size);
        break;
    }
    case P_TEXT_ALIGN:
        st->center_blocks = !strcasecmp(value, "-webkit-center") || !strcasecmp(value, "-moz-center");
        if (inherit_kw)
            st->text_align = parent->text_align, st->center_blocks = parent->center_blocks;
        else if (!strcasecmp(value, "center") || st->center_blocks)
            st->text_align = TA_CENTER;
        else if (!strcasecmp(value, "right") || !strcasecmp(value, "end") ||
                 !strcasecmp(value, "-webkit-right"))
            st->text_align = TA_RIGHT;
        else if (!strcasecmp(value, "justify"))
            st->text_align = TA_JUSTIFY;
        else if (!strcasecmp(value, "left") || !strcasecmp(value, "start") ||
                 !strcasecmp(value, "-webkit-left") || initial_kw)
            st->text_align = TA_LEFT;
        break;
    case P_TEXT_DECORATION:
    case P_TEXT_DECORATION_LINE:
        n = words(value, w, wl, 8);
        if (n && (word_is(w[0], wl[0], "none") || word_is(w[0], wl[0], "initial")))
            st->decoration = 0;
        for (int i = 0; i < n; i++) {
            if (word_is(w[i], wl[i], "underline"))
                st->decoration |= DECO_UNDERLINE;
            else if (word_is(w[i], wl[i], "line-through"))
                st->decoration |= DECO_STRIKE;
            else if (word_is(w[i], wl[i], "overline"))
                st->decoration |= DECO_OVERLINE;
        }
        break;
    case P_TEXT_TRANSFORM:
        st->text_transform = !strcasecmp(value, "uppercase") ? TT_UPPER
                           : !strcasecmp(value, "lowercase") ? TT_LOWER
                           : !strcasecmp(value, "capitalize") ? TT_CAPITALIZE : TT_NONE;
        break;
    case P_TEXT_INDENT: {
        struct len l;
        if (length_value(value, v, &l) && l.unit == U_PX)
            st->text_indent = l.v;
        break;
    }
    case P_WHITE_SPACE:
        if (!strcasecmp(value, "pre"))
            st->white_space = WS_PRE;
        else if (!strcasecmp(value, "nowrap"))
            st->white_space = WS_NOWRAP;
        else if (!strcasecmp(value, "pre-wrap") || !strcasecmp(value, "break-spaces"))
            st->white_space = WS_PRE_WRAP;
        else if (!strcasecmp(value, "pre-line"))
            st->white_space = WS_PRE_LINE;
        else if (!strcasecmp(value, "normal") || initial_kw)
            st->white_space = WS_NORMAL;
        break;
    case P_VERTICAL_ALIGN:
        st->vertical_align = !strcasecmp(value, "sub") ? VA_SUB
                           : !strcasecmp(value, "super") ? VA_SUPER
                           : !strcasecmp(value, "middle") ? VA_MIDDLE
                           : !strcasecmp(value, "top") ? VA_TOP
                           : !strcasecmp(value, "text-top") ? VA_TOP
                           : !strcasecmp(value, "bottom") ? VA_BOTTOM
                           : !strcasecmp(value, "text-bottom") ? VA_BOTTOM : VA_BASELINE;
        break;
    case P_LIST_STYLE:
    case P_LIST_STYLE_TYPE: {
        static const struct { const char *kw; uint8_t ls; } ls[] = {
            { "disc", LS_DISC }, { "circle", LS_CIRCLE }, { "square", LS_SQUARE },
            { "decimal", LS_DECIMAL }, { "decimal-leading-zero", LS_DECIMAL },
            { "lower-alpha", LS_LOWER_ALPHA }, { "lower-latin", LS_LOWER_ALPHA },
            { "upper-alpha", LS_UPPER_ALPHA }, { "upper-latin", LS_UPPER_ALPHA },
            { "lower-roman", LS_LOWER_ROMAN }, { "upper-roman", LS_UPPER_ROMAN }, { "none", LS_NONE },
        };
        n = words(value, w, wl, 8);
        for (int i = 0; i < n; i++)
            for (size_t k = 0; k < sizeof(ls) / sizeof(ls[0]); k++)
                if (word_is(w[i], wl[i], ls[k].kw))
                    st->list_style = ls[k].ls;
        break;
    }
    case P_MARGIN:
        if (box_values(value, v, box))
            memcpy(st->margin, box, sizeof(box));
        break;
    case P_MARGIN_TOP: case P_MARGIN_RIGHT: case P_MARGIN_BOTTOM: case P_MARGIN_LEFT:
        set_len(&st->margin[prop - P_MARGIN_TOP], value, v);
        break;
    case P_PADDING:
        if (box_values(value, v, box))
            memcpy(st->padding, box, sizeof(box));
        break;
    case P_PADDING_TOP: case P_PADDING_RIGHT: case P_PADDING_BOTTOM: case P_PADDING_LEFT:
        set_len(&st->padding[prop - P_PADDING_TOP], value, v);
        if (st->padding[prop - P_PADDING_TOP].unit == U_AUTO)
            st->padding[prop - P_PADDING_TOP] = (struct len){ .unit = U_PX };
        break;
    case P_BORDER:
        apply_border(st, value, 15, v);
        break;
    case P_BORDER_TOP: case P_BORDER_RIGHT: case P_BORDER_BOTTOM: case P_BORDER_LEFT:
        apply_border(st, value, 1 << (prop - P_BORDER_TOP), v);
        break;
    case P_BORDER_WIDTH: {
        n = words(value, w, wl, 4);
        int px[4], k = 0;
        for (int i = 0; i < n; i++)
            if (border_width_kw(w[i], wl[i], v, &px[k]))
                k++;
        if (k == 1) px[1] = px[2] = px[3] = px[0];
        else if (k == 2) px[2] = px[0], px[3] = px[1];
        else if (k == 3) px[3] = px[1];
        if (k)
            for (int i = 0; i < 4; i++)
                st->border_w[i] = px[i];
        break;
    }
    case P_BORDER_STYLE: {
        n = words(value, w, wl, 4);
        int bs[4], k = 0;
        for (int i = 0; i < n; i++)
            if ((bs[k] = parse_border_style(w[i], wl[i])) >= 0)
                k++;
        if (k == 1) bs[1] = bs[2] = bs[3] = bs[0];
        else if (k == 2) bs[2] = bs[0], bs[3] = bs[1];
        else if (k == 3) bs[3] = bs[1];
        if (k)
            for (int i = 0; i < 4; i++)
                st->border_style[i] = (uint8_t)bs[i];
        break;
    }
    case P_BORDER_COLOR: {
        n = words(value, w, wl, 4);
        uint32_t cs[4];
        int k = 0;
        for (int i = 0; i < n; i++)
            if (css_parse_color(w[i], wl[i], &cs[k]) == 0)
                k++;
        if (k == 1) cs[1] = cs[2] = cs[3] = cs[0];
        else if (k == 2) cs[2] = cs[0], cs[3] = cs[1];
        else if (k == 3) cs[3] = cs[1];
        if (k)
            for (int i = 0; i < 4; i++)
                st->border_color[i] = cs[i];
        break;
    }
    case P_BORDER_TOP_WIDTH: case P_BORDER_RIGHT_WIDTH: case P_BORDER_BOTTOM_WIDTH:
    case P_BORDER_LEFT_WIDTH: {
        int px;
        if (border_width_kw(value, (int)strlen(value), v, &px))
            st->border_w[prop - P_BORDER_TOP_WIDTH] = px;
        break;
    }
    case P_BORDER_TOP_STYLE: case P_BORDER_RIGHT_STYLE: case P_BORDER_BOTTOM_STYLE:
    case P_BORDER_LEFT_STYLE: {
        int bs = parse_border_style(value, (int)strlen(value));
        if (bs >= 0)
            st->border_style[prop - P_BORDER_TOP_STYLE] = (uint8_t)bs;
        break;
    }
    case P_BORDER_TOP_COLOR: case P_BORDER_RIGHT_COLOR: case P_BORDER_BOTTOM_COLOR:
    case P_BORDER_LEFT_COLOR:
        if (css_parse_color(value, (int)strlen(value), &c) == 0)
            st->border_color[prop - P_BORDER_TOP_COLOR] = c;
        break;
    case P_WIDTH: set_len(&st->width, value, v); break;
    case P_HEIGHT: set_len(&st->height, value, v); break;
    case P_MIN_WIDTH:
        set_len(&st->min_width, value, v);      /* auto: a flex item's own minimum */
        break;
    case P_MAX_WIDTH: set_len(&st->max_width, value, v); break;
    case P_MIN_HEIGHT:
        set_len(&st->min_height, value, v);
        if (st->min_height.unit == U_AUTO)
            st->min_height = (struct len){ .unit = U_PX };
        break;
    case P_BOX_SIZING:
        st->box_sizing_border = !strcasecmp(value, "border-box");
        break;
    case P_VISIBILITY:
        if (!inherit_kw)
            st->hidden = !strcasecmp(value, "hidden") || !strcasecmp(value, "collapse");
        break;
    case P_BORDER_SPACING: {
        struct len l;
        if (length_value(value, v, &l) && l.unit == U_PX)
            st->border_spacing = l.v;
        break;
    }
    case P_BORDER_COLLAPSE:
        if (!strcasecmp(value, "collapse"))
            st->border_spacing = 0;
        break;
    case P_POSITION:
        st->positioned = !strcasecmp(value, "absolute") || !strcasecmp(value, "fixed");
        break;
    case P_OPACITY: {
        int num;
        if (parse_num(value, &num))
            st->invisible = (uint8_t)((st->invisible & ~1) | (num <= 0));
        break;
    }
    case P_LEFT:
    case P_TOP: {
        /* Only to recognise content pushed off the screen to hide it. */
        struct len l;
        if (length_value(value, v, &l) && l.unit == U_PX)
            st->invisible = (uint8_t)((st->invisible & ~2) | (l.v <= -500 ? 2 : 0));
        break;
    }
    case P_MASK:
        /* An icon painted through a mask image: without the mask it would be a box. */
        if (strstr(value, "url("))
            st->invisible |= 8;
        break;
    case P_CLIP:
        if (!strncasecmp(value, "rect(", 5)) {
            int num, zero = 1;
            for (const char *p = value + 5; *p && *p != ')'; p++)
                if (isdigit((unsigned char)*p) && parse_num(p, &num) && num > 100)
                    zero = 0;
            st->invisible = (uint8_t)((st->invisible & ~4) | (zero ? 4 : 0));
        }
        break;
    }
}

/* ---- var() ---- */

static const char *lookup_var(const struct style *st, const char *name, int len)
{
    for (const struct custom_prop *c = st->vars; c; c = c->next)
        if ((int)strlen(c->name) == len && !strncmp(c->name, name, (size_t)len))
            return c->value;
    return NULL;
}

/* Expand var(--x, fallback) in value; returns 0 if a variable is missing. */
static int substitute(const struct style *st, const char *value, char *out, int size, int depth)
{
    int o = 0;
    const char *end = value + strlen(value);
    for (const char *p = value; p < end;) {
        if (!strncmp(p, "var(", 4)) {
            const char *close = skip_group(p + 3, end);
            const char *args = p + 4, *args_end = close - 1;
            while (args < args_end && isspace((unsigned char)*args))
                args++;
            const char *comma = find_top(args, args_end, ',');
            const char *ne = comma;
            while (ne > args && isspace((unsigned char)ne[-1]))
                ne--;
            const char *val = lookup_var(st, args, (int)(ne - args));
            char fallback[512];
            if (!val && comma < args_end) {
                int fl = (int)(args_end - comma - 1);
                if (fl >= (int)sizeof(fallback))
                    return 0;
                memcpy(fallback, comma + 1, (size_t)fl);
                fallback[fl] = 0;
                val = fallback;
            }
            if (!val || depth > 8)
                return 0;
            char inner[1024];
            if (!substitute(st, val, inner, sizeof(inner), depth + 1))
                return 0;
            int il = (int)strlen(inner);
            if (o + il >= size)
                return 0;
            memcpy(out + o, inner, (size_t)il);
            o += il;
            p = close;
            continue;
        }
        if (o + 1 >= size)
            return 0;
        out[o++] = *p++;
    }
    out[o] = 0;
    /* Trim what the substitution left around the value. */
    char *s = out;
    while (isspace((unsigned char)*s))
        s++;
    if (s != out)
        memmove(out, s, strlen(s) + 1);
    int l = (int)strlen(out);
    while (l > 0 && isspace((unsigned char)out[l - 1]))
        out[--l] = 0;
    return 1;
}

/* ---- presentational HTML attributes ---- */

static int legacy_color(const char *v, char *out, int size)
{
    uint32_t c;
    if (css_parse_color(v, (int)strlen(v), &c) == 0) {
        snprintf(out, (size_t)size, "%s", v);
        return 1;
    }
    /* "ff0000" without the '#' */
    int n = 0;
    while (v[n] && isxdigit((unsigned char)v[n]))
        n++;
    if ((n == 6 || n == 3) && !v[n]) {
        snprintf(out, (size_t)size, "#%s", v);
        return 1;
    }
    return 0;
}

static void hint_len(char *buf, int size, const char *prop, const char *v)
{
    int n = (int)strlen(buf);
    if (!v || !isdigit((unsigned char)*v))
        return;
    int num = atoi(v);
    snprintf(buf + n, (size_t)(size - n), "%s: %d%s;", prop, num, strchr(v, '%') ? "%" : "px");
}

static void hint_color(char *buf, int size, const char *prop, const char *v)
{
    char c[64];
    int n = (int)strlen(buf);
    if (v && legacy_color(v, c, sizeof(c)))
        snprintf(buf + n, (size_t)(size - n), "%s: %s;", prop, c);
}

static void hint_add(char *buf, int size, const char *text)
{
    int n = (int)strlen(buf);
    snprintf(buf + n, (size_t)(size - n), "%s", text);
}

static const struct node *ancestor(const struct node *n, int tag)
{
    for (n = n->parent; n; n = n->parent)
        if (n->tag == tag)
            return n;
    return NULL;
}

static void hint_align(char *buf, int size, const char *v, int block)
{
    if (!v)
        return;
    if (!strcasecmp(v, "center") || !strcasecmp(v, "middle"))
        hint_add(buf, size, block ? "margin-left: auto; margin-right: auto;" : "text-align: -webkit-center;");
    else if (!strcasecmp(v, "right"))
        hint_add(buf, size, block ? "margin-left: auto;" : "text-align: right;");
    else if (!strcasecmp(v, "left") && !block)
        hint_add(buf, size, "text-align: left;");
    else if (!strcasecmp(v, "justify") && !block)
        hint_add(buf, size, "text-align: justify;");
}

/* The declarations HTML attributes ask for, as CSS text. */
static void hints(const struct node *n, char *buf, int size)
{
    const char *v;
    buf[0] = 0;
    switch (n->tag) {
    case T_BODY:
        hint_color(buf, size, "background-color", node_attr(n, "bgcolor"));
        hint_color(buf, size, "color", node_attr(n, "text"));
        hint_len(buf, size, "margin", node_attr(n, "marginwidth"));
        if ((v = node_attr(n, "leftmargin")) || (v = node_attr(n, "topmargin")))
            hint_len(buf, size, "margin", v);
        break;
    case T_FONT:
        hint_color(buf, size, "color", node_attr(n, "color"));
        if ((v = node_attr(n, "size"))) {
            static const char *const sizes[] = { "x-small", "x-small", "small", "medium", "large",
                                                 "x-large", "xx-large", "xxx-large" };
            int k = atoi(v);
            if (*v == '+' || *v == '-')
                k += 3;
            k = k < 1 ? 1 : k > 7 ? 7 : k;
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "font-size: %s;", sizes[k]);
        }
        if ((v = node_attr(n, "face"))) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "font-family: %.100s;", v);
        }
        break;
    case T_TABLE:
        hint_color(buf, size, "background-color", node_attr(n, "bgcolor"));
        hint_len(buf, size, "width", node_attr(n, "width"));
        hint_len(buf, size, "height", node_attr(n, "height"));
        hint_len(buf, size, "border-spacing", node_attr(n, "cellspacing"));
        hint_align(buf, size, node_attr(n, "align"), 1);
        if ((v = node_attr(n, "border")) && (atoi(v) > 0 || !*v)) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "border: %dpx outset #808080;", *v ? atoi(v) : 1);
        }
        break;
    case T_TR:
        hint_color(buf, size, "background-color", node_attr(n, "bgcolor"));
        hint_align(buf, size, node_attr(n, "align"), 0);
        break;
    case T_TD:
    case T_TH: {
        hint_color(buf, size, "background-color", node_attr(n, "bgcolor"));
        hint_len(buf, size, "width", node_attr(n, "width"));
        hint_len(buf, size, "height", node_attr(n, "height"));
        hint_align(buf, size, node_attr(n, "align"), 0);
        const struct node *tr = n->parent && n->parent->tag == T_TR ? n->parent : NULL;
        if (!node_attr(n, "align") && tr)
            hint_align(buf, size, node_attr(tr, "align"), 0);
        if (!(v = node_attr(n, "valign")) && tr)
            v = node_attr(tr, "valign");
        if (v) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "vertical-align: %s;",
                     !strcasecmp(v, "top") ? "top" : !strcasecmp(v, "bottom") ? "bottom" : "middle");
        }
        if (node_attr(n, "nowrap"))
            hint_add(buf, size, "white-space: nowrap;");
        const struct node *table = ancestor(n, T_TABLE);
        if (table) {
            hint_len(buf, size, "padding", node_attr(table, "cellpadding"));
            if ((v = node_attr(table, "border")) && (atoi(v) > 0 || !*v))
                hint_add(buf, size, "border: 1px inset #808080;");
        }
        break;
    }
    case T_IMG:
        hint_len(buf, size, "width", node_attr(n, "width"));
        hint_len(buf, size, "height", node_attr(n, "height"));
        if ((v = node_attr(n, "border"))) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "border: %dpx solid;", atoi(v));
        }
        if ((v = node_attr(n, "hspace"))) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "margin-left: %dpx; margin-right: %dpx;", atoi(v), atoi(v));
        }
        if ((v = node_attr(n, "vspace"))) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "margin-top: %dpx; margin-bottom: %dpx;", atoi(v), atoi(v));
        }
        if ((v = node_attr(n, "align"))) {
            if (!strcasecmp(v, "middle") || !strcasecmp(v, "absmiddle") || !strcasecmp(v, "center"))
                hint_add(buf, size, "vertical-align: middle;");
            else if (!strcasecmp(v, "left") || !strcasecmp(v, "right"))
                hint_add(buf, size, "float: left;");
            else if (!strcasecmp(v, "top") || !strcasecmp(v, "texttop"))
                hint_add(buf, size, "vertical-align: top;");
        }
        break;
    case T_HR:
        hint_len(buf, size, "width", node_attr(n, "width"));
        hint_align(buf, size, node_attr(n, "align"), 1);
        if ((v = node_attr(n, "size")) && atoi(v) > 1) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "height: %dpx;", atoi(v) - 2);
        }
        if ((v = node_attr(n, "color"))) {
            char c[64];
            if (legacy_color(v, c, sizeof(c))) {
                int l = (int)strlen(buf);
                snprintf(buf + l, (size_t)(size - l), "border: 1px solid %s; background-color: %s;", c, c);
            }
        } else if (node_attr(n, "noshade")) {
            hint_add(buf, size, "border: 1px solid #808080; background-color: #808080;");
        }
        break;
    case T_DIV: case T_P: case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
    case T_CAPTION: case T_THEAD: case T_TBODY: case T_TFOOT: case T_LEGEND:
        hint_align(buf, size, node_attr(n, "align"), 0);
        break;
    case T_UL: case T_OL: case T_LI:
        if ((v = node_attr(n, "type"))) {
            const char *t = !strcmp(v, "1") ? "decimal" : !strcmp(v, "a") ? "lower-alpha"
                          : !strcmp(v, "A") ? "upper-alpha" : !strcmp(v, "i") ? "lower-roman"
                          : !strcmp(v, "I") ? "upper-roman" : !strcasecmp(v, "circle") ? "circle"
                          : !strcasecmp(v, "square") ? "square" : !strcasecmp(v, "disc") ? "disc" : NULL;
            if (t) {
                int l = (int)strlen(buf);
                snprintf(buf + l, (size_t)(size - l), "list-style-type: %s;", t);
            }
        }
        break;
    case T_INPUT:
        if ((v = node_attr(n, "size")) && atoi(v) > 0) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "width: %dpx;", atoi(v) * 7 + 8);
        }
        break;
    case T_TEXTAREA: {
        int cols = (v = node_attr(n, "cols")) ? atoi(v) : 20;
        int rows = (v = node_attr(n, "rows")) ? atoi(v) : 2;
        cols = cols < 1 ? 20 : cols > 200 ? 200 : cols;
        rows = rows < 1 ? 2 : rows > 100 ? 100 : rows;
        int l = (int)strlen(buf);
        snprintf(buf + l, (size_t)(size - l), "width: %dpx; height: %dpx;", cols * 8 + 8, rows * 16 + 6);
        break;
    }
    case T_PRE:
        if (node_attr(n, "wrap"))
            hint_add(buf, size, "white-space: pre-wrap;");
        break;
    default:
        break;
    }
}

/* ---- the cascade ---- */

struct match {
    const struct decl *d;
    uint32_t rank;              /* origin and importance */
    uint32_t spec;
    uint32_t order;
    uint32_t pe;                /* for ::before (1) or ::after (2) */
};

static int cmp_match(const void *a, const void *b)
{
    const struct match *x = a, *y = b;
    if (x->rank != y->rank)
        return x->rank < y->rank ? -1 : 1;
    if (x->spec != y->spec)
        return x->spec < y->spec ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

/* Origins, lowest first. Important author declarations beat everything. */
enum { R_UA, R_HINT, R_AUTHOR, R_INLINE, R_AUTHOR_IMPORTANT, R_INLINE_IMPORTANT };

struct collect {
    struct match *m;
    int n;
    const struct mctx *mc;
};

static void add_decls(struct collect *c, const struct decl *d, int nd, int base_rank,
                      uint32_t spec, uint32_t order, int pe)
{
    for (int i = 0; i < nd && c->n < MAX_MATCHES; i++) {
        uint32_t rank = (uint32_t)base_rank;
        if (d[i].important && base_rank == R_AUTHOR)
            rank = R_AUTHOR_IMPORTANT;
        else if (d[i].important && base_rank == R_INLINE)
            rank = R_INLINE_IMPORTANT;
        c->m[c->n++] = (struct match){ &d[i], rank, spec, order + (uint32_t)i, (uint32_t)pe };
    }
}

static void match_list(struct collect *c, const struct sheet *sh, const struct ref *ref,
                       const struct node *n, int rank, uint32_t sheet_order)
{
    for (; ref; ref = ref->next) {
        const struct rule *r = &sh->rules[ref->rule];
        const struct selector *sel = &r->sels[ref->sel];
        if (match_from(sel, sel->nparts - 1, n, c->mc))
            add_decls(c, r->decls, r->ndecls, rank, sel->spec,
                      sheet_order + (uint32_t)ref->rule * 256, sel->pe);
    }
}

static void match_sheet(struct collect *c, const struct sheet *sh, const struct node *n, int rank,
                        uint32_t sheet_order)
{
    const char *id = node_attr(n, "id");
    if (id)
        match_list(c, sh, sh->by_id[hash_str(id) % INDEX_BUCKETS], n, rank, sheet_order);
    const char *cls = node_attr(n, "class");
    if (cls) {
        /* Each class's bucket once, even if two classes share it. */
        uint32_t seen[16];
        int nseen = 0;
        for (const char *p = cls; *p;) {
            while (*p && isspace((unsigned char)*p))
                p++;
            const char *e = p;
            while (*e && !isspace((unsigned char)*e))
                e++;
            if (e > p) {
                char one[128];
                int l = (int)(e - p) < 127 ? (int)(e - p) : 127;
                memcpy(one, p, (size_t)l);
                one[l] = 0;
                uint32_t b = hash_str(one) % INDEX_BUCKETS;
                int dup = 0;
                for (int i = 0; i < nseen; i++)
                    dup |= seen[i] == b;
                if (!dup && nseen < 16) {
                    seen[nseen++] = b;
                    match_list(c, sh, sh->by_class[b], n, rank, sheet_order);
                }
            }
            p = e;
        }
    }
    if (n->tag > 0)
        match_list(c, sh, sh->by_tag[n->tag], n, rank, sheet_order);
    match_list(c, sh, sh->other, n, rank, sheet_order);
}

static void pseudo(struct arena *a, struct node *n, const struct style *st, const struct collect *c,
                   int pe, const struct vctx *base);

static void compute(struct arena *a, struct node *n, const struct style *parent,
                    struct sheet **sheets, int nsheets, const struct vctx *base,
                    const struct mctx *mc, struct match *buf)
{
    struct style *st = arena_alloc(a, sizeof(*st));
    if (!st)
        return;
    inherit(st, parent);
    n->style = st;

    struct collect c = { buf, 0, mc };
    match_sheet(&c, ua_sheet, n, R_UA, 0);
    if (mc->quirks)
        match_sheet(&c, ua_quirks, n, R_UA, 1u << 23);
    char hint[1024];
    hints(n, hint, sizeof(hint));
    if (hint[0]) {
        struct decl *d;
        int nd = parse_decls(a, hint, hint + strlen(hint), &d);
        add_decls(&c, d, nd, R_HINT, 0, 0, 0);
    }
    for (int i = 0; i < nsheets; i++)
        if (sheets[i])
            match_sheet(&c, sheets[i], n, R_AUTHOR, (uint32_t)(i + 1) << 24);
    const char *inline_style = node_attr(n, "style");
    if (inline_style) {
        struct decl *d;
        int nd = parse_decls(a, inline_style, inline_style + strlen(inline_style), &d);
        add_decls(&c, d, nd, R_INLINE, 0, 0, 0);
    }
    qsort(c.m, (size_t)c.n, sizeof(struct match), cmp_match);

    /* Pass 1: custom properties. */
    for (int i = 0; i < c.n; i++) {
        const struct decl *d = c.m[i].d;
        if (d->prop != P_CUSTOM || c.m[i].pe)
            continue;
        struct custom_prop *cp = arena_alloc(a, sizeof(*cp));
        if (!cp)
            break;
        cp->name = d->name;
        cp->value = d->value;
        /* var() inside a custom property refers to the inherited values. */
        if (strstr(d->value, "var(")) {
            char buf2[1024];
            if (substitute(st, d->value, buf2, sizeof(buf2), 0))
                cp->value = arena_strdup(a, buf2);
        }
        cp->next = st->vars;
        st->vars = cp;
    }

    struct vctx v = *base;
    v.a = a;
    /* Pass 2: font-size, which em units in everything else depend on. Pass 3: the rest. */
    int author_color = 0;
    for (int pass = 2; pass <= 3; pass++) {
        if (pass == 3)
            v.font_size = st->font_size;
        for (int i = 0; i < c.n; i++) {
            const struct decl *d = c.m[i].d;
            int early = d->prop == P_FONT_SIZE || d->prop == P_FONT;
            if (d->prop == P_CUSTOM || (pass == 2) != early || c.m[i].pe)
                continue;
            const char *value = d->value;
            char expanded[1024];
            if (strstr(value, "var(")) {
                if (!substitute(st, value, expanded, sizeof(expanded), 0))
                    continue;
                value = expanded;
            }
            apply(st, parent, d->prop, value, &v);
            if (d->prop == P_COLOR && c.m[i].rank != R_UA)
                author_color = 1;
        }
    }

    /* Links follow <body link=...> unless a style sheet says otherwise. */
    if (n->tag == T_BODY && node_attr(n, "link")) {
        char col[64];
        uint32_t lc;
        if (legacy_color(node_attr(n, "link"), col, sizeof(col)) &&
            css_parse_color(col, (int)strlen(col), &lc) == 0)
            st->link_color = lc;
    }
    if (is_link(n) && !author_color && st->link_color)
        st->color = st->link_color;

    /* Computed values. */
    if (!n->parent)
        st->display = st->display == D_NONE ? D_NONE : D_BLOCK;
    /* Positioned boxes stay in the flow here, so the ones that are meant to be
     * hidden (closed menus, screen-reader text) must not take room there. */
    if (st->positioned && (st->hidden || st->invisible ||
                           (st->width.unit == U_PX && st->width.v <= 1 && st->height.unit == U_PX &&
                            st->height.v <= 1)))
        st->display = D_NONE;
    if (st->invisible & 1)
        st->hidden = 1;
    if (st->invisible & 8)
        st->bg = TRANSPARENT, st->grad = 0;
    if (st->floated && st->display != D_NONE)
        st->display = (st->display == D_TABLE || st->display == D_INLINE_TABLE) ? D_INLINE_TABLE
                                                                               : D_INLINE_BLOCK;
    for (int i = 0; i < 4; i++) {
        if (st->border_style[i] == BS_NONE)
            st->border_w[i] = 0;
        if (st->border_color[i] == 1)
            st->border_color[i] = st->color;
    }
    if (st->font_size > 200)
        st->font_size = 200;
    st->font = wfont_get(st->family, st->bold, st->italic, st->font_size);

    if (st->display != D_NONE)
        for (int pe = 1; pe <= 2; pe++)
            pseudo(a, n, st, &c, pe, &v);
}

/* The text of a "content" value: strings, quotes and attr(); NULL for none. */
static const char *content_text(struct arena *a, const struct node *n, const char *value)
{
    char out[256];
    int o = 0;
    const char *p = value, *end = value + strlen(value);
    if (!strcasecmp(value, "none") || !strcasecmp(value, "normal"))
        return NULL;
    while (p < end) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (p < end && *p != q) {
                int cp = (unsigned char)*p++;
                if (cp == '\\' && p < end) {
                    if (isxdigit((unsigned char)*p)) {
                        cp = 0;
                        for (int k = 0; k < 6 && p < end && isxdigit((unsigned char)*p); k++, p++)
                            cp = cp * 16 + hexval(*p);
                        if (p < end && *p == ' ')
                            p++;
                    } else {
                        cp = (unsigned char)*p++;
                    }
                    if (cp >= 0xE000 && cp <= 0xF8FF)
                        continue;       /* icon fonts: we have none of their glyphs */
                    if (o < (int)sizeof(out) - 4)
                        o += utf8_put(out + o, cp);
                    continue;
                }
                if (o < (int)sizeof(out) - 1)
                    out[o++] = (char)cp;
            }
            p++;
        } else if (!strncasecmp(p, "open-quote", 10) || !strncasecmp(p, "close-quote", 11)) {
            if (o < (int)sizeof(out) - 4)
                o += utf8_put(out + o, p[0] == 'o' || p[0] == 'O' ? 0x201C : 0x201D);
            p += p[0] == 'o' || p[0] == 'O' ? 10 : 11;
        } else if (!strncasecmp(p, "attr(", 5)) {
            const char *close = strchr(p, ')');
            char name[64];
            int nl = close ? (int)(close - p - 5) : 0;
            if (nl > 0 && nl < (int)sizeof(name)) {
                memcpy(name, p + 5, (size_t)nl);
                name[nl] = 0;
                const char *v = node_attr(n, name);
                for (; v && *v && o < (int)sizeof(out) - 1; v++)
                    out[o++] = *v;
            }
            p = close ? close + 1 : end;
        } else if (*p == '(' || isalpha((unsigned char)*p)) {
            /* url(), counter() and the like: nothing we can show */
            while (p < end && !isspace((unsigned char)*p)) {
                if (*p == '(')
                    p = skip_group(p, end);
                else
                    p++;
            }
        } else {
            p++;
        }
    }
    if (!o)
        return NULL;
    return arena_strndup(a, out, (size_t)o);
}

/* The style and text of n's ::before (pe 1) or ::after (pe 2), if it has one. */
static void pseudo(struct arena *a, struct node *n, const struct style *st, const struct collect *c,
                   int pe, const struct vctx *base)
{
    const char *content = NULL;
    for (int i = 0; i < c->n; i++)
        if (c->m[i].pe == (uint32_t)pe && c->m[i].d->prop == P_CONTENT)
            content = c->m[i].d->value;
    if (!content)
        return;
    const char *text = content_text(a, n, content);
    if (!text)
        return;
    struct style *ps = arena_alloc(a, sizeof(*ps));
    if (!ps)
        return;
    inherit(ps, st);
    struct vctx v = *base;
    for (int pass = 2; pass <= 3; pass++) {
        if (pass == 3)
            v.font_size = ps->font_size;
        for (int i = 0; i < c->n; i++) {
            const struct decl *d = c->m[i].d;
            int early = d->prop == P_FONT_SIZE || d->prop == P_FONT;
            if (c->m[i].pe != (uint32_t)pe || d->prop == P_CUSTOM || (pass == 2) != early)
                continue;
            const char *value = d->value;
            char expanded[1024];
            if (strstr(value, "var(")) {
                if (!substitute(ps, value, expanded, sizeof(expanded), 0))
                    continue;
                value = expanded;
            }
            apply(ps, st, d->prop, value, &v);
        }
    }
    if (ps->display == D_NONE || (ps->positioned && (ps->hidden || ps->invisible)))
        return;
    for (int i = 0; i < 4; i++) {
        if (ps->border_style[i] == BS_NONE)
            ps->border_w[i] = 0;
        if (ps->border_color[i] == 1)
            ps->border_color[i] = ps->color;
    }
    if (ps->invisible & 8)
        ps->bg = TRANSPARENT;
    ps->font = wfont_get(ps->family, ps->bold, ps->italic, ps->font_size);
    n->pseudo_st[pe - 1] = ps;
    n->pseudo_text[pe - 1] = text;
}

static void cascade_tree(struct arena *a, struct node *root, const struct style *parent,
                         struct sheet **sheets, int nsheets, struct vctx *v,
                         const struct mctx *mc, struct match *buf)
{
    /* Iterative pre-order walk; a display:none subtree is skipped (and shares its root's style). */
    struct node *n = root;
    while (n) {
        struct node *next = NULL;
        if (n->type == N_ELEMENT) {
            const struct style *ps = n->parent && n->parent->style ? n->parent->style : parent;
            compute(a, n, ps, sheets, nsheets, v, mc, buf);
            if (!n->parent && n->style)
                v->root_font_size = n->style->font_size;
            if (n->style && n->style->display == D_NONE) {
                for (struct node *k = n->first; k; k = node_next(k, n))
                    if (k->type == N_ELEMENT)
                        k->style = n->style;
            } else {
                next = n->first;
            }
        }
        if (!next) {
            struct node *p = n;
            while (p && p != root && !p->next)
                p = p->parent;
            next = p && p != root ? p->next : NULL;
        }
        n = next;
    }
}

void css_cascade(struct arena *a, struct node *root, struct sheet **sheets, int nsheets,
                 int viewport_w, int viewport_h, int (*visited)(const char *href, void *ctx),
                 void *ctx)
{
    init_initial();
    if (!ua_sheet) {
        ua_sheet = css_parse(&ua_arena, ua_css, (int)strlen(ua_css), viewport_w);
        ua_quirks = css_parse(&ua_arena, ua_quirks_css, (int)strlen(ua_quirks_css), viewport_w);
    }
    if (!ua_sheet || !ua_quirks || !root)
        return;
    struct vctx v = { 16, 16, viewport_w, viewport_h, a };
    struct mctx mc = { visited, ctx, root->quirks };
    struct match *buf = malloc(sizeof(struct match) * MAX_MATCHES);
    if (!buf)
        return;
    cascade_tree(a, root, &initial_style, sheets, nsheets, &v, &mc, buf);
    free(buf);
}
