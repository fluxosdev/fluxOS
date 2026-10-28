/*
 * HTML parsing: a tokenizer for tags, attributes, comments, character
 * references and raw-text elements, and a tree builder that follows the
 * main rules of the HTML5 algorithm: implied <html>, <head> and <body>,
 * paragraphs and list items closed by what follows them, table rows and
 * cells closed by the next row or cell, void elements, and end tags that
 * close everything opened inside them. It never fails: anything it does
 * not understand becomes text or is dropped.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

#define MAX_DEPTH 256

static const char *const tag_names[T_COUNT] = {
    [T_UNKNOWN] = "",
#define X(id, name) [T_##id] = name,
    TAG_LIST(X)
#undef X
};

const char *tag_name(int tag)
{
    return tag > T_UNKNOWN && tag < T_COUNT ? tag_names[tag] : "";
}

int tag_lookup(const char *name, int len)
{
    int lo = 1, hi = T_COUNT - 1;       /* TAG_LIST is in alphabetical order */
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strncmp(tag_names[mid], name, (size_t)len);
        if (c == 0 && tag_names[mid][len])
            c = 1;
        if (c == 0)
            return mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return T_UNKNOWN;
}

/* ---- character references ---- */

#include "html_entities.h"

/* Windows-1252's meaning of 0x80-0x9F, which pages use in &#NNN; by mistake. */
static const uint16_t cp1252[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

/*
 * Decode the character reference at s (just after the '&'). Returns the
 * code point and sets *used to the bytes consumed, or returns -1 if it
 * is not one. In attributes, a name without ';' followed by a letter,
 * digit or '=' is left alone, so URLs like "?a=1&copy=2" survive.
 */
static int char_ref(const char *s, const char *end, int *used, int in_attr, int *second)
{
    *second = 0;
    if (s < end && *s == '#') {
        const char *p = s + 1;
        int hex = p < end && (*p == 'x' || *p == 'X');
        if (hex)
            p++;
        int cp = 0, digits = 0;
        while (p < end && (hex ? isxdigit((unsigned char)*p) : isdigit((unsigned char)*p))) {
            int d = isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10);
            if (cp < 0x110000)
                cp = cp * (hex ? 16 : 10) + d;
            p++;
            digits++;
        }
        if (!digits)
            return -1;
        if (p < end && *p == ';')
            p++;
        *used = (int)(p - s);
        if (cp >= 0x80 && cp < 0xA0)
            return cp1252[cp - 0x80];
        if (cp == 0 || cp >= 0x110000 || (cp >= 0xD800 && cp < 0xE000))
            return 0xFFFD;
        return cp;
    }
    int n = 0;
    while (s + n < end && n < HTML_ENTITY_MAX_NAME && isalnum((unsigned char)s[n]))
        n++;
    if (s + n < end && s[n] == ';')
        n++;
    /* Longest match, including the semicolon when the name requires it. */
    for (int len = n; len > 0; len--) {
        int lo = 0, hi = (int)(sizeof(entities) / sizeof(entities[0])) - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int cmp = strncmp(entities[mid].name, s, (size_t)len);
            if (!cmp && entities[mid].name[len])
                cmp = 1;
            if (cmp < 0) {
                lo = mid + 1;
            } else if (cmp > 0) {
                hi = mid - 1;
            } else {
                /* Legacy names followed by an attribute-name character
                 * are literal text (notably query-string ampersands). */
                if (s[len - 1] != ';' && in_attr && s + len < end &&
                    (isalnum((unsigned char)s[len]) || s[len] == '='))
                    return -1;
                *used = len;
                *second = entities[mid].second;
                return entities[mid].cp;
            }
        }
    }
    return -1;
}

/* Copy text with character references decoded (UTF-8 in and out). */
static char *decode_text(struct arena *a, const char *s, int n, int in_attr, int *outlen)
{
    char *out = arena_alloc(a, (size_t)n * 2 + 1);    /* "&lt;" -> 1 byte; "&#x10000;" -> 4 */
    if (!out)
        return NULL;
    int o = 0;
    const char *end = s + n;
    while (s < end) {
        if (*s == '&') {
            int used, second, cp = char_ref(s + 1, end, &used, in_attr, &second);
            if (cp >= 0) {
                o += utf8_put(out + o, cp);
                if (second)
                    o += utf8_put(out + o, second);
                s += 1 + used;
                continue;
            }
        }
        if (*s == '\r') {               /* CRLF and CR become LF */
            out[o++] = '\n';
            s += s + 1 < end && s[1] == '\n' ? 2 : 1;
            continue;
        }
        out[o++] = *s++;
    }
    out[o] = 0;
    if (outlen)
        *outlen = o;
    return out;
}

/* ---- the tree ---- */

struct parser {
    struct arena *a;
    const char *s, *end;
    struct node *html, *head, *body;
    struct node *stack[MAX_DEPTH];
    int depth;
    int in_body;
};

static struct node *new_element(struct parser *p, int tag, const char *name, int len)
{
    struct node *n = arena_alloc(p->a, sizeof(*n));
    if (!n)
        return NULL;
    n->type = N_ELEMENT;
    n->tag = (uint8_t)tag;
    n->name = arena_strndup(p->a, name, (size_t)len);
    if (!n->name)
        return NULL;
    for (char *c = n->name; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    return n;
}

static void append(struct node *parent, struct node *n)
{
    n->parent = parent;
    n->prev = parent->last;
    if (parent->last)
        parent->last->next = n;
    else
        parent->first = n;
    parent->last = n;
}

static struct node *current(struct parser *p)
{
    return p->stack[p->depth - 1];
}

static void push(struct parser *p, struct node *n)
{
    if (p->depth < MAX_DEPTH)
        p->stack[p->depth++] = n;
}

static void pop_through(struct parser *p, int i)
{
    if (i >= 1)
        p->depth = i;
}

static int is_heading(int t)
{
    return t >= T_H1 && t <= T_H6;
}

/* The index of the innermost open element with tag t, searching down to a
 * boundary (one of `stops`); -1 if there is none. */
static int find_open(struct parser *p, int t, const uint8_t *stops)
{
    for (int i = p->depth - 1; i >= 1; i--) {
        int ct = p->stack[i]->tag;
        if (ct == t || (t == T_H1 && is_heading(ct)))
            return i;
        for (const uint8_t *s = stops; *s; s++)
            if (ct == *s)
                return -1;
    }
    return -1;
}

static const uint8_t scope_stops[] = { T_TABLE, T_TD, T_TH, T_CAPTION, T_BUTTON, T_OBJECT, T_HTML, 0 };
static const uint8_t list_stops[] = { T_UL, T_OL, T_MENU, T_DIR, T_TABLE, T_TD, T_TH, T_BODY, 0 };
static const uint8_t dl_stops[] = { T_DL, T_TABLE, T_TD, T_TH, T_BODY, 0 };
static const uint8_t row_stops[] = { T_TABLE, T_HTML, 0 };
static const uint8_t cell_stops[] = { T_TR, T_TABLE, T_HTML, 0 };

static void close_p(struct parser *p)
{
    int i = find_open(p, T_P, scope_stops);
    if (i >= 0)
        pop_through(p, i);
}

static int closes_p(int t)
{
    switch (t) {
    case T_ADDRESS: case T_ARTICLE: case T_ASIDE: case T_BLOCKQUOTE: case T_CENTER:
    case T_DETAILS: case T_DIALOG: case T_DIR: case T_DIV: case T_DL: case T_FIELDSET:
    case T_FIGCAPTION: case T_FIGURE: case T_FOOTER: case T_FORM: case T_H1: case T_H2:
    case T_H3: case T_H4: case T_H5: case T_H6: case T_HEADER: case T_HGROUP: case T_HR:
    case T_MAIN: case T_MENU: case T_NAV: case T_OL: case T_P: case T_PRE: case T_SECTION:
    case T_TABLE: case T_UL: case T_LI: case T_DD: case T_DT: case T_LISTING: case T_PLAINTEXT:
    case T_XMP:
        return 1;
    default:
        return 0;
    }
}

static int is_void(int t)
{
    switch (t) {
    case T_AREA: case T_BASE: case T_BR: case T_COL: case T_EMBED: case T_HR: case T_IMG:
    case T_INPUT: case T_LINK: case T_META: case T_PARAM: case T_SOURCE: case T_TRACK: case T_WBR:
    case T_FRAME:
        return 1;
    default:
        return 0;
    }
}

static int is_head_element(int t)
{
    return t == T_TITLE || t == T_STYLE || t == T_SCRIPT || t == T_LINK || t == T_META ||
           t == T_BASE || t == T_TEMPLATE;
}

static void enter_body(struct parser *p)
{
    if (p->in_body)
        return;
    p->in_body = 1;
    p->depth = 1;
    push(p, p->body);
}

static void add_text(struct parser *p, const char *s, int n)
{
    if (n <= 0)
        return;
    int blank = 1;
    for (int i = 0; i < n && blank; i++)
        blank = s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f';
    if (!p->in_body) {
        if (blank)
            return;
        enter_body(p);
    }
    struct node *parent = current(p);
    int pt = parent->tag;
    if (blank && (pt == T_TABLE || pt == T_TBODY || pt == T_THEAD || pt == T_TFOOT || pt == T_TR ||
                  pt == T_SELECT || pt == T_UL || pt == T_OL))
        return;
    int len;
    char *text = decode_text(p->a, s, n, 0, &len);
    if (!text)
        return;
    struct node *last = parent->last;
    if (last && last->type == N_TEXT) {             /* merge with the text before */
        char *joined = arena_alloc(p->a, (size_t)(last->len + len + 1));
        if (!joined)
            return;
        memcpy(joined, last->text, (size_t)last->len);
        memcpy(joined + last->len, text, (size_t)len);
        last->text = joined;
        last->len += len;
        return;
    }
    struct node *t = arena_alloc(p->a, sizeof(*t));
    if (!t)
        return;
    t->type = N_TEXT;
    t->text = text;
    t->len = len;
    append(parent, t);
}

static void set_attr(struct parser *p, struct node *n, const char *name, int nlen,
                     const char *value, int vlen)
{
    for (struct attr *a = n->attrs; a; a = a->next)
        if ((int)strlen(a->name) == nlen && !strncasecmp(a->name, name, (size_t)nlen))
            return;                     /* the first one wins */
    struct attr *a = arena_alloc(p->a, sizeof(*a));
    if (!a)
        return;
    a->name = arena_strndup(p->a, name, (size_t)nlen);
    a->value = value ? decode_text(p->a, value, vlen, 1, NULL) : arena_strdup(p->a, "");
    if (!a->name || !a->value)
        return;
    for (char *c = a->name; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    /* Keep document order. */
    struct attr **pp = &n->attrs;
    while (*pp)
        pp = &(*pp)->next;
    *pp = a;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Parse attributes up to '>'; returns 1 if the tag ended with "/>". */
static int parse_attrs(struct parser *p, struct node *n)
{
    const char *s = p->s, *end = p->end;
    int self_closing = 0;
    while (s < end && *s != '>') {
        if (is_space(*s)) {
            s++;
            continue;
        }
        if (*s == '/') {
            self_closing = s + 1 < end && s[1] == '>';
            s++;
            continue;
        }
        const char *name = s;
        while (s < end && !is_space(*s) && *s != '>' && *s != '=' && !(*s == '/' && s + 1 < end && s[1] == '>'))
            s++;
        int nlen = (int)(s - name);
        if (nlen == 0)
            nlen = 1, s++;              /* a stray '=' */
        while (s < end && is_space(*s))
            s++;
        const char *value = NULL;
        int vlen = 0;
        if (s < end && *s == '=') {
            s++;
            while (s < end && is_space(*s))
                s++;
            if (s < end && (*s == '"' || *s == '\'')) {
                char q = *s++;
                value = s;
                while (s < end && *s != q)
                    s++;
                vlen = (int)(s - value);
                if (s < end)
                    s++;
            } else {
                value = s;
                while (s < end && !is_space(*s) && *s != '>')
                    s++;
                vlen = (int)(s - value);
            }
        }
        if (n)
            set_attr(p, n, name, nlen, value, vlen);
    }
    p->s = s < end ? s + 1 : end;
    return self_closing;
}

/* Text up to the end tag `name` (case-insensitive); the end tag is consumed. */
static void raw_text(struct parser *p, const char *name, const char **text, int *len)
{
    size_t nl = strlen(name);
    const char *s = p->s;
    while (s < p->end) {
        if (s[0] == '<' && s + 1 < p->end && s[1] == '/' && (size_t)(p->end - s) >= nl + 2 &&
            !strncasecmp(s + 2, name, nl) && (s + 2 + nl == p->end || !isalnum((unsigned char)s[2 + nl])))
            break;
        s++;
    }
    *text = p->s;
    *len = (int)(s - p->s);
    if (s < p->end) {
        const char *gt = memchr(s, '>', (size_t)(p->end - s));
        p->s = gt ? gt + 1 : p->end;
    } else {
        p->s = p->end;
    }
}

static void start_tag(struct parser *p, const char *name, int nlen)
{
    int t = tag_lookup(name, nlen);
    for (int i = 0; i < nlen && t == T_UNKNOWN; i++)
        if (isupper((unsigned char)name[i])) {
            char low[32];
            int k = nlen < 31 ? nlen : 31;
            for (int j = 0; j < k; j++)
                low[j] = (char)tolower((unsigned char)name[j]);
            t = tag_lookup(low, k);
            break;
        }

    if (t == T_HTML || t == T_BODY || t == T_HEAD) {
        struct node *target = t == T_HTML ? p->html : t == T_BODY ? p->body : NULL;
        parse_attrs(p, target);
        if (t == T_BODY)
            enter_body(p);
        return;
    }

    struct node *n = new_element(p, t, name, nlen);
    if (!n)
        return;
    int self_closing = parse_attrs(p, n);

    if (!p->in_body && !is_head_element(t) && t != T_NOSCRIPT)
        enter_body(p);
    struct node *ct = current(p);

    /* Content inside <select> is only options. */
    if (ct->tag == T_SELECT || (ct->tag == T_OPTION && p->depth >= 2 && p->stack[p->depth - 2]->tag == T_SELECT)) {
        if (t != T_OPTION && t != T_OPTGROUP)
            return;
    }

    /* Elements that end open ones. */
    if (closes_p(t))
        close_p(p);
    int i;
    switch (t) {
    case T_LI:
        if ((i = find_open(p, T_LI, list_stops)) >= 0)
            pop_through(p, i);
        break;
    case T_DT:
    case T_DD:
        if ((i = find_open(p, T_DT, dl_stops)) >= 0 || (i = find_open(p, T_DD, dl_stops)) >= 0)
            pop_through(p, i);
        break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        if (is_heading(current(p)->tag))
            pop_through(p, p->depth - 1);
        break;
    case T_OPTION:
        if (current(p)->tag == T_OPTION)
            pop_through(p, p->depth - 1);
        break;
    case T_RB: case T_RT: case T_RP:    /* ruby annotations end each other */
        if (current(p)->tag == T_RB || current(p)->tag == T_RT || current(p)->tag == T_RP)
            pop_through(p, p->depth - 1);
        break;
    case T_OPTGROUP:
        if (current(p)->tag == T_OPTION)
            pop_through(p, p->depth - 1);
        if (current(p)->tag == T_OPTGROUP)
            pop_through(p, p->depth - 1);
        break;
    case T_A:
        if ((i = find_open(p, T_A, scope_stops)) >= 0)
            pop_through(p, i);
        break;
    case T_BUTTON:
        if ((i = find_open(p, T_BUTTON, scope_stops)) >= 0)
            pop_through(p, i);
        break;
    case T_TR:
        if ((i = find_open(p, T_TR, row_stops)) >= 0)
            pop_through(p, i);
        else if ((i = find_open(p, T_TD, row_stops)) >= 0 || (i = find_open(p, T_TH, row_stops)) >= 0)
            pop_through(p, i);
        break;
    case T_TD:
    case T_TH:
        if ((i = find_open(p, T_TD, cell_stops)) >= 0 || (i = find_open(p, T_TH, cell_stops)) >= 0)
            pop_through(p, i);
        break;
    case T_THEAD: case T_TBODY: case T_TFOOT: case T_CAPTION: case T_COLGROUP:
        for (int k = p->depth - 1; k >= 1; k--) {
            int kt = p->stack[k]->tag;
            if (kt == T_TABLE || kt == T_HTML)
                break;
            if (kt == T_THEAD || kt == T_TBODY || kt == T_TFOOT || kt == T_TR || kt == T_CAPTION ||
                kt == T_COLGROUP) {
                pop_through(p, k);
                break;
            }
        }
        break;
    default:
        break;
    }

    struct node *parent = p->in_body ? current(p) : p->head;
    append(parent, n);

    if (t == T_SCRIPT || t == T_STYLE || t == T_XMP || t == T_TEXTAREA || t == T_TITLE ||
        t == T_PLAINTEXT || t == T_IFRAME || t == T_NOFRAMES) {
        const char *text;
        int len;
        if (t == T_PLAINTEXT) {
            text = p->s;
            len = (int)(p->end - p->s);
            p->s = p->end;
        } else {
            raw_text(p, n->name, &text, &len);
        }
        if (t == T_IFRAME || t == T_NOFRAMES)
            return;                     /* never shown */
        /* A script keeps its text for the script engine (scripts are display: none). */
        int rcdata = t == T_TEXTAREA || t == T_TITLE;
        struct node *tn = arena_alloc(p->a, sizeof(*tn));
        if (!tn)
            return;
        tn->type = N_TEXT;
        if (rcdata) {
            tn->text = decode_text(p->a, text, len, 0, &tn->len);
        } else {
            tn->text = arena_strndup(p->a, text, (size_t)len);
            tn->len = len;
        }
        if (!tn->text)
            return;
        /* A newline right after <textarea> or <pre> is not part of the content. */
        if (t == T_TEXTAREA && tn->len && tn->text[0] == '\n')
            tn->text++, tn->len--;
        append(n, tn);
        return;
    }
    if (!is_void(t) && !self_closing)
        push(p, n);
    else if (self_closing && !is_void(t) && t != T_UNKNOWN)
        push(p, n);                     /* "<div/>" is "<div>" in HTML */
}

static void end_tag(struct parser *p, const char *name, int nlen)
{
    int t = tag_lookup(name, nlen);
    char low[64];
    if (t == T_UNKNOWN) {
        int k = nlen < 63 ? nlen : 63;
        for (int j = 0; j < k; j++)
            low[j] = (char)tolower((unsigned char)name[j]);
        low[k] = 0;
        t = tag_lookup(low, k);
    }
    if (t == T_HTML || t == T_BODY || t == T_HEAD)
        return;
    if (t == T_BR) {                    /* "</br>" is "<br>" */
        if (!p->in_body)
            enter_body(p);
        struct node *n = new_element(p, T_BR, "br", 2);
        if (n)
            append(current(p), n);
        return;
    }
    int i;
    switch (t) {
    case T_P:
        if ((i = find_open(p, T_P, scope_stops)) >= 0)
            pop_through(p, i);
        return;
    case T_LI:
        if ((i = find_open(p, T_LI, list_stops)) >= 0)
            pop_through(p, i);
        return;
    case T_DD:
    case T_DT:
        if ((i = find_open(p, t, dl_stops)) >= 0)
            pop_through(p, i);
        return;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        if ((i = find_open(p, T_H1, scope_stops)) >= 0)
            pop_through(p, i);
        return;
    case T_TABLE:
        if ((i = find_open(p, T_TABLE, (const uint8_t[]){ T_HTML, 0 })) >= 0)
            pop_through(p, i);
        return;
    case T_TR: case T_TD: case T_TH: case T_TBODY: case T_THEAD: case T_TFOOT: case T_CAPTION:
        if ((i = find_open(p, t, row_stops)) >= 0)
            pop_through(p, i);
        return;
    default:
        break;
    }
    /* Anything else closes the innermost open element of that name, unless a
     * table cell or similar boundary comes first. */
    for (i = p->depth - 1; i >= 1; i--) {
        struct node *n = p->stack[i];
        if (t != T_UNKNOWN ? n->tag == t : (n->tag == T_UNKNOWN && !strcmp(n->name, low)))
            break;
        if (n->tag == T_TD || n->tag == T_TH || n->tag == T_TABLE || n->tag == T_CAPTION ||
            n->tag == T_BUTTON)
            return;
    }
    if (i >= 1)
        pop_through(p, i);
}

struct node *html_parse(struct arena *a, const char *text, int len)
{
    struct parser p = { .a = a, .s = text, .end = text + len };
    p.html = new_element(&p, T_HTML, "html", 4);
    p.head = new_element(&p, T_HEAD, "head", 4);
    p.body = new_element(&p, T_BODY, "body", 4);
    if (!p.html || !p.head || !p.body)
        return NULL;
    append(p.html, p.head);
    append(p.html, p.body);
    push(&p, p.html);
    p.html->quirks = 1;                 /* until a doctype says otherwise */

    /* A byte order mark is not content. */
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3))
        p.s += 3;

    while (p.s < p.end) {
        const char *lt = memchr(p.s, '<', (size_t)(p.end - p.s));
        if (!lt) {
            add_text(&p, p.s, (int)(p.end - p.s));
            break;
        }
        add_text(&p, p.s, (int)(lt - p.s));
        p.s = lt;
        const char *s = lt + 1;
        if (s < p.end && *s == '!') {
            if (p.end - s > 8 && !strncasecmp(s + 1, "doctype", 7)) {
                /* Standards mode for <!DOCTYPE html>, XHTML, and HTML 4 with a DTD. */
                const char *gt = memchr(s, '>', (size_t)(p.end - s));
                char dt[256];
                int n = 0;
                for (const char *c = s + 8; c < (gt ? gt : p.end) && n < 255; c++)
                    dt[n++] = (char)tolower((unsigned char)*c);
                dt[n] = 0;
                char *t = dt;
                while (*t == ' ' || *t == '\n' || *t == '\t' || *t == '\r')
                    t++;
                int standards = !strncmp(t, "html", 4) &&
                                (!t[4 + strspn(t + 4, " \t\r\n")] || strstr(t, "xhtml") ||
                                 strstr(t, "strict") || (strstr(t, "4.01") && strstr(t, ".dtd")));
                p.html->quirks = !standards;
                p.s = gt ? gt + 1 : p.end;
                continue;
            }
            if (p.end - s >= 3 && s[1] == '-' && s[2] == '-') {
                /* A comment: up to "-->", or the end of the document. */
                const char *c = s + 3;
                while (c + 2 < p.end && !(c[0] == '-' && c[1] == '-' && c[2] == '>'))
                    c++;
                p.s = c + 2 < p.end ? c + 3 : p.end;
            } else {
                const char *gt = memchr(s, '>', (size_t)(p.end - s));
                p.s = gt ? gt + 1 : p.end;      /* <!DOCTYPE ...>, <![CDATA[...]]> */
            }
        } else if (s < p.end && *s == '?') {
            const char *gt = memchr(s, '>', (size_t)(p.end - s));
            p.s = gt ? gt + 1 : p.end;
        } else if (s < p.end && *s == '/' && s + 1 < p.end && isalpha((unsigned char)s[1])) {
            const char *name = s + 1, *e = name;
            while (e < p.end && (isalnum((unsigned char)*e) || *e == '-' || *e == ':' || *e == '_'))
                e++;
            p.s = e;
            parse_attrs(&p, NULL);
            end_tag(&p, name, (int)(e - name));
        } else if (s < p.end && isalpha((unsigned char)*s)) {
            const char *name = s, *e = s;
            while (e < p.end && (isalnum((unsigned char)*e) || *e == '-' || *e == ':' || *e == '_'))
                e++;
            p.s = e;
            start_tag(&p, name, (int)(e - name));
        } else {
            add_text(&p, "<", 1);
            p.s = lt + 1;
        }
    }
    return p.html;
}

/* ---- queries ---- */

const char *node_attr(const struct node *n, const char *name)
{
    if (!n || n->type != N_ELEMENT)
        return NULL;
    for (const struct attr *a = n->attrs; a; a = a->next)
        if (!strcmp(a->name, name))
            return a->value;
    return NULL;
}

struct node *node_next(struct node *n, const struct node *root)
{
    if (n->first)
        return n->first;
    while (n && n != root) {
        if (n->next)
            return n->next;
        n = n->parent;
    }
    return NULL;
}

/* Infrastructure walks include shadow trees; DOM queries continue to use node_next. */
struct node *node_after_shadow(struct node *n, const struct node *root)
{
    while (n && n != root) {
        if (n->next)
            return n->next;
        if (n->shadow_host) {
            n = n->shadow_host;
            if (n->first)
                return n->first;
        } else
            n = n->parent;
    }
    return NULL;
}
struct node *node_next_shadow(struct node *n, const struct node *root)
{
    if (n->shadow_root)
        return n->shadow_root;
    if (n->first)
        return n->first;
    return node_after_shadow(n, root);
}

struct node *node_find(struct node *root, enum tag tag)
{
    for (struct node *n = root; n; n = node_next(n, root))
        if (n->type == N_ELEMENT && n->tag == tag)
            return n;
    return NULL;
}

int node_text(const struct node *n, char *out, int size)
{
    int o = 0, space = 0;
    for (const struct node *c = n; c; c = node_next((struct node *)c, n)) {
        if (c->type != N_TEXT)
            continue;
        for (int i = 0; i < c->len; i++) {
            char ch = c->text[i];
            if (is_space(ch)) {
                space = o > 0;
                continue;
            }
            if (space && o < size - 1)
                out[o++] = ' ';
            space = 0;
            if (o < size - 1)
                out[o++] = ch;
        }
    }
    if (size > 0)
        out[o < size ? o : size - 1] = 0;
    return o;
}

/*
 * Declarative shadow DOM: each <template shadowrootmode="open"> (or "closed") becomes the
 * shadow root of the element it is in, holding what the template held. Only for the page's
 * own HTML (not innerHTML), as the standard has it; an element keeps the first such root.
 */
void html_declarative_shadows(struct arena *a, struct node *root)
{
    for (struct node *n = root; n;) {
        if (n->type != N_ELEMENT || n->tag != T_TEMPLATE || !n->parent || n->parent->shadow_root ||
            n->parent->type != N_ELEMENT) {
            n = node_next(n, root);
            continue;
        }
        const char *mode = node_attr(n, "shadowrootmode");
        if (!mode || (strcasecmp(mode, "open") && strcasecmp(mode, "closed"))) {
            n = node_next(n, root);
            continue;
        }
        struct node *host = n->parent, *after = n;
        while (after && after != root && !after->next)    /* what comes after the template's subtree */
            after = after->parent;
        after = after && after != root ? after->next : NULL;
        struct node *sr = arena_alloc(a, sizeof(*sr));
        if (!sr)
            return;
        sr->type = N_ELEMENT;
        sr->tag = T_UNKNOWN;
        sr->name = arena_strdup(a, "#fragment");
        sr->first = n->first;
        sr->last = n->last;
        for (struct node *k = sr->first; k; k = k->next)
            k->parent = sr;
        sr->shadow_host = host;
        sr->shadow_closed = !strcasecmp(mode, "closed");
        host->shadow_root = sr;
        /* the template itself goes */
        if (n->prev) n->prev->next = n->next; else host->first = n->next;
        if (n->next) n->next->prev = n->prev; else host->last = n->prev;
        n->parent = n->prev = n->next = NULL;
        n->first = n->last = NULL;
        html_declarative_shadows(a, sr);        /* roots declared inside this one */
        n = after;
    }
}
