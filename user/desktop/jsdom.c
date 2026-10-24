/*
 * The page's scripts: the JavaScript engine bound to the document.
 *
 * Every node a script touches gets a wrapper, a host object whose
 * properties (id, className, innerHTML, value, style, children...) read
 * and write the node itself; methods come from a shared prototype. The
 * global object is the window, with document, location, navigator,
 * history, localStorage (kept for the page only), timers and alert.
 *
 * Changes to the tree mark the page dirty; when the engine returns to the
 * browser, a dirty page is styled, laid out and painted again. Events
 * (click, input, change, submit, keydown, load) go to listeners added
 * with addEventListener, on* properties and on* attributes, from the
 * target up to the window.
 *
 * XMLHttpRequest goes through the browser's own fetching (wget), always
 * asynchronously; performance.timing, mark and measure are there for the
 * scripts that time themselves.
 *
 * querySelector understands type, #id, .class, [attr] (=, ~=, ^=, $=,
 * *=, |=), the combinators (space, >, +, ~) and a few pseudo-classes
 * (:first-child, :last-child, :only-child, :nth-child, :not, :checked,
 * :disabled, :enabled, :empty, :root, :first-of-type, :last-of-type).
 */
#include <ctype.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jsint.h"
#include "jsdom.h"

#define MAX_TIMERS 256
#define MAX_XHR    64

struct timer {
    int id;
    int active;
    unsigned int due;
    int interval;                       /* ms; 0 for a one-shot timer */
    jsval fn;                           /* a function, or a string to evaluate */
    jsval args[4];
    int nargs;
};

struct jsdom {
    struct jshost h;
    struct js *J;
    struct jsobj *window, *document, *element_proto, *document_proto, *event_proto, *location;
    struct jsobj *style_proto, *classlist_proto;
    struct timer timers[MAX_TIMERS];
    int next_timer_id;
    struct node *current_script;
    struct node *write_after;           /* document.write's insertion point */
    int dirty;
    int loaded;
    int depth;                          /* nested entries from the browser */
    char cookies[2048];
    struct jsobj *xhr[MAX_XHR];         /* requests in flight, by id */
    struct jsobj *xhr_proto;
    unsigned int started;               /* uptime when the page's scripts began */
};

static const struct jsclass node_class, document_class, style_class, classlist_class,
    dataset_class, location_class, computed_class;

static struct jsdom *dom(struct js *J)
{
    return js_host(J);
}

#define ARG(i) ((i) < argc ? args[(i)] : js_undefined)

static struct jsstr *S(struct js *J, const char *s)
{
    return js_mkstr(J, s, (int)strlen(s));
}

static void fn(struct js *J, struct jsobj *o, const char *name, js_native f, int n)
{
    js_define_native(J, o, name, f, n);
}

static void report(struct jsdom *d, const char *what)
{
    char msg[600];
    snprintf(msg, sizeof(msg), "JavaScript error: %s", js_error(d->J));
    fprintf(stderr, "%s%s%s\n", msg, what ? " in " : "", what ? what : "");
    if (d->h.status)
        d->h.status(d->h.ctx, msg);
}

/* Leaving the engine: a changed tree is laid out again. */
static void settle(struct jsdom *d)
{
    if (d->depth == 0 && d->dirty) {
        d->dirty = 0;
        d->h.changed(d->h.ctx);
    }
}

/* ---- the tree ---- */

static int is_element(const struct node *n)
{
    return n && n->type == N_ELEMENT;
}

static void renumber(struct node *parent)
{
    if (!parent)
        return;
    int i = 1;
    for (struct node *k = parent->first; k; k = k->next)
        if (k->type == N_ELEMENT)
            k->index = i++;
}

static void unlink_node(struct node *n)
{
    struct node *p = n->parent;
    if (!p)
        return;
    if (n->prev)
        n->prev->next = n->next;
    else
        p->first = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        p->last = n->prev;
    n->parent = n->prev = n->next = NULL;
    renumber(p);
}

static int contains(const struct node *a, const struct node *b)
{
    for (; b; b = b->parent)
        if (a == b)
            return 1;
    return 0;
}

static int is_fragment(const struct node *n)
{
    return is_element(n) && !strcmp(n->name, "#fragment");
}

/* Insert n into parent before ref (NULL: at the end); a fragment's children go in its place. */
static int insert_node(struct jsdom *d, struct node *parent, struct node *n, struct node *ref)
{
    if (!parent || !n || contains(n, parent))
        return -1;
    if (is_fragment(n)) {
        while (n->first) {
            struct node *k = n->first;
            unlink_node(k);
            insert_node(d, parent, k, ref);
        }
        return 0;
    }
    if (ref && ref->parent != parent)
        ref = NULL;
    if (n == ref)
        return 0;
    unlink_node(n);
    n->parent = parent;
    if (ref) {
        n->next = ref;
        n->prev = ref->prev;
        if (ref->prev)
            ref->prev->next = n;
        else
            parent->first = n;
        ref->prev = n;
    } else {
        n->prev = parent->last;
        if (parent->last)
            parent->last->next = n;
        else
            parent->first = n;
        parent->last = n;
    }
    renumber(parent);
    d->dirty = 1;
    return 0;
}

static struct node *new_node(struct jsdom *d, int type, const char *name)
{
    struct node *n = arena_alloc(d->h.arena, sizeof(*n));
    if (!n)
        return NULL;
    n->type = (uint8_t)type;
    if (type == N_ELEMENT) {
        char lname[64];
        int len = 0;
        for (; name[len] && len < (int)sizeof(lname) - 1; len++)
            lname[len] = (char)tolower((unsigned char)name[len]);
        lname[len] = 0;
        n->name = arena_strdup(d->h.arena, lname);
        n->tag = (uint8_t)tag_lookup(lname, len);
    }
    return n;
}

static struct node *new_text(struct jsdom *d, const char *text, int len)
{
    struct node *n = new_node(d, N_TEXT, NULL);
    if (!n)
        return NULL;
    n->text = arena_strndup(d->h.arena, text, (size_t)len);
    n->len = len;
    return n;
}

static void set_attr(struct jsdom *d, struct node *n, const char *name, const char *value)
{
    if (!is_element(n))
        return;
    char lname[64];
    snprintf(lname, sizeof(lname), "%s", name);
    for (char *p = lname; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    struct attr **pp = &n->attrs;
    for (; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->name, lname)) {
            (*pp)->value = arena_strdup(d->h.arena, value);
            d->dirty = 1;
            return;
        }
    struct attr *a = arena_alloc(d->h.arena, sizeof(*a));
    if (!a)
        return;
    a->name = arena_strdup(d->h.arena, lname);
    a->value = arena_strdup(d->h.arena, value);
    *pp = a;
    d->dirty = 1;
}

static void remove_attr(struct jsdom *d, struct node *n, const char *name)
{
    if (!is_element(n))
        return;
    for (struct attr **pp = &n->attrs; *pp; pp = &(*pp)->next)
        if (!strcasecmp((*pp)->name, name)) {
            *pp = (*pp)->next;
            d->dirty = 1;
            return;
        }
}

/* Text of every text node under n, as it is (textContent). */
static void collect_text(const struct node *n, char **buf, int *len, int *cap)
{
    for (const struct node *k = n; k; k = node_next((struct node *)k, n)) {
        if (k->type != N_TEXT)
            continue;
        if (*len + k->len + 1 > *cap) {
            int nc = (*len + k->len + 1) * 2;
            char *nb = realloc(*buf, (size_t)nc);
            if (!nb)
                return;
            *buf = nb;
            *cap = nc;
        }
        memcpy(*buf + *len, k->text, (size_t)k->len);
        *len += k->len;
    }
}

static jsval text_content(struct js *J, const struct node *n)
{
    if (n->type == N_TEXT)
        return js_strn(J, n->text, n->len);
    char *buf = NULL;
    int len = 0, cap = 0;
    collect_text(n, &buf, &len, &cap);
    jsval v = js_strn(J, buf ? buf : "", len);
    free(buf);
    return v;
}

static void remove_children(struct jsdom *d, struct node *n)
{
    while (n->first)
        unlink_node(n->first);
    d->dirty = 1;
}

static void set_text_content(struct jsdom *d, struct node *n, const char *s, int len)
{
    if (n->type == N_TEXT) {
        n->text = arena_strndup(d->h.arena, s, (size_t)len);
        n->len = len;
        d->dirty = 1;
        return;
    }
    remove_children(d, n);
    if (len) {
        struct node *t = new_text(d, s, len);
        if (t)
            insert_node(d, n, t, NULL);
    }
    if (n->tag == T_TEXTAREA) {
        free(n->value);
        n->value = strndup(s, (size_t)len);
    }
}

/* ---- serializing (innerHTML) ---- */

struct out {
    char *p;
    int n, cap;
};

static void out_add(struct out *o, const char *s, int n)
{
    if (o->n + n + 1 > o->cap) {
        int nc = (o->n + n + 1) * 2;
        char *np = realloc(o->p, (size_t)nc);
        if (!np)
            return;
        o->p = np;
        o->cap = nc;
    }
    memcpy(o->p + o->n, s, (size_t)n);
    o->n += n;
    o->p[o->n] = 0;
}

static void out_str(struct out *o, const char *s)
{
    out_add(o, s, (int)strlen(s));
}

static void out_escaped(struct out *o, const char *s, int n, int attr)
{
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&')
            out_str(o, "&amp;");
        else if (c == '<' && !attr)
            out_str(o, "&lt;");
        else if (c == '>' && !attr)
            out_str(o, "&gt;");
        else if (c == '"' && attr)
            out_str(o, "&quot;");
        else
            out_add(o, &c, 1);
    }
}

static int is_void(int tag)
{
    return tag == T_AREA || tag == T_BASE || tag == T_BR || tag == T_COL || tag == T_EMBED ||
           tag == T_HR || tag == T_IMG || tag == T_INPUT || tag == T_LINK || tag == T_META ||
           tag == T_PARAM || tag == T_SOURCE || tag == T_TRACK || tag == T_WBR;
}

static void serialize(struct out *o, const struct node *n, int self)
{
    if (n->type == N_TEXT) {
        int raw = n->parent && (n->parent->tag == T_SCRIPT || n->parent->tag == T_STYLE);
        if (raw)
            out_add(o, n->text, n->len);
        else
            out_escaped(o, n->text, n->len, 0);
        return;
    }
    if (self) {
        out_str(o, "<");
        out_str(o, n->name);
        for (const struct attr *a = n->attrs; a; a = a->next) {
            out_str(o, " ");
            out_str(o, a->name);
            out_str(o, "=\"");
            out_escaped(o, a->value ? a->value : "", a->value ? (int)strlen(a->value) : 0, 1);
            out_str(o, "\"");
        }
        out_str(o, ">");
        if (is_void(n->tag))
            return;
    }
    for (const struct node *k = n->first; k; k = k->next)
        serialize(o, k, 1);
    if (self) {
        out_str(o, "</");
        out_str(o, n->name);
        out_str(o, ">");
    }
}

static jsval html_of(struct js *J, const struct node *n, int outer)
{
    struct out o = { 0 };
    serialize(&o, n, outer);
    jsval v = js_strn(J, o.p ? o.p : "", o.n);
    free(o.p);
    return v;
}

/* Parse an HTML fragment; returns an element whose children are its nodes. */
static struct node *parse_fragment(struct jsdom *d, const char *html, int len)
{
    static const char prefix[] = "<!DOCTYPE html><html><head></head><body>";
    char *buf = malloc(sizeof(prefix) + (size_t)len);
    if (!buf)
        return NULL;
    memcpy(buf, prefix, sizeof(prefix) - 1);
    memcpy(buf + sizeof(prefix) - 1, html, (size_t)len);
    struct node *root = html_parse(d->h.arena, buf, (int)(sizeof(prefix) - 1) + len);
    free(buf);
    if (!root)
        return NULL;
    struct node *body = node_find(root, T_BODY);
    struct node *frag = new_node(d, N_ELEMENT, "#fragment");
    if (!frag || !body)
        return frag;
    while (body->first) {
        struct node *k = body->first;
        unlink_node(k);
        insert_node(d, frag, k, NULL);
    }
    return frag;
}

static void set_inner_html(struct jsdom *d, struct node *n, const char *html, int len)
{
    struct node *frag = parse_fragment(d, html, len);
    remove_children(d, n);
    if (frag)
        insert_node(d, n, frag, NULL);
}

/* ---- selectors ---- */

static int has_class(const struct node *n, const char *cls, int len)
{
    const char *v = node_attr(n, "class");
    if (!v)
        return 0;
    while (*v) {
        while (*v && isspace((unsigned char)*v))
            v++;
        const char *s = v;
        while (*v && !isspace((unsigned char)*v))
            v++;
        if (v - s == len && !strncmp(s, cls, (size_t)len))
            return 1;
    }
    return 0;
}

static const struct node *prev_element(const struct node *n)
{
    for (n = n->prev; n; n = n->prev)
        if (n->type == N_ELEMENT)
            return n;
    return NULL;
}

static const struct node *next_element(const struct node *n)
{
    for (n = n->next; n; n = n->next)
        if (n->type == N_ELEMENT)
            return n;
    return NULL;
}

static int ident_char(int c)
{
    return isalnum(c) || c == '-' || c == '_' || c >= 0x80 || c == '\\';
}

/* :nth-child(an+b): does position pos (from 1) fit? */
static int nth_fits(const char *arg, int len, int pos)
{
    char b[32];
    snprintf(b, sizeof(b), "%.*s", len, arg);
    char *s = b;
    while (*s == ' ')
        s++;
    if (!strncasecmp(s, "odd", 3))
        return pos % 2 == 1;
    if (!strncasecmp(s, "even", 4))
        return pos % 2 == 0;
    int a = 0, bb = 0;
    char *n = strchr(s, 'n');
    if (n) {
        if (n == s || (n == s + 1 && (*s == '+')))
            a = 1;
        else if (n == s + 1 && *s == '-')
            a = -1;
        else
            a = atoi(s);
        char *rest = n + 1;
        while (*rest == ' ')
            rest++;
        if (*rest == '+' || *rest == '-') {
            int sign = *rest == '-' ? -1 : 1;
            rest++;
            while (*rest == ' ')
                rest++;
            bb = sign * atoi(rest);
        }
    } else {
        return pos == atoi(s);
    }
    if (a == 0)
        return pos == bb;
    int k = pos - bb;
    return k / a >= 0 && k % a == 0;
}

static int match_compound(const struct node *n, const char *s, int len);

static int match_pseudo(const struct node *n, const char *name, int nlen, const char *arg, int alen)
{
#define IS(x) (nlen == (int)strlen(x) && !strncasecmp(name, x, (size_t)nlen))
    const struct node *p = n->parent;
    if (IS("first-child"))
        return !prev_element(n);
    if (IS("last-child"))
        return !next_element(n);
    if (IS("only-child"))
        return !prev_element(n) && !next_element(n);
    if (IS("nth-child"))
        return arg && nth_fits(arg, alen, n->index);
    if (IS("nth-last-child")) {
        int pos = 1;
        for (const struct node *k = next_element(n); k; k = next_element(k))
            pos++;
        return arg && nth_fits(arg, alen, pos);
    }
    if (IS("first-of-type") || IS("last-of-type")) {
        int first = IS("first-of-type");
        for (const struct node *k = first ? prev_element(n) : next_element(n); k;
             k = first ? prev_element(k) : next_element(k))
            if (!strcmp(k->name, n->name))
                return 0;
        return 1;
    }
    if (IS("not"))
        return arg && !match_compound(n, arg, alen);
    if (IS("checked"))
        return n->checked || (n->tag == T_OPTION && node_attr(n, "selected"));
    if (IS("disabled"))
        return node_attr(n, "disabled") != NULL;
    if (IS("enabled"))
        return node_attr(n, "disabled") == NULL;
    if (IS("empty"))
        return !n->first;
    if (IS("root"))
        return p == NULL || p->type != N_ELEMENT;
    if (IS("link") || IS("any-link"))
        return n->tag == T_A && node_attr(n, "href");
    return 0;                           /* :hover, :focus, ... */
#undef IS
}

static int match_compound(const struct node *n, const char *s, int len)
{
    if (!is_element(n))
        return 0;
    const char *e = s + len;
    while (s < e && *s == ' ')
        s++;
    if (s < e && *s == '*') {
        s++;
    } else if (s < e && ident_char((unsigned char)*s)) {
        const char *t = s;
        while (s < e && ident_char((unsigned char)*s))
            s++;
        if ((int)strlen(n->name) != s - t || strncasecmp(n->name, t, (size_t)(s - t)))
            return 0;
    }
    while (s < e) {
        char c = *s++;
        if (c == '#' || c == '.') {
            const char *t = s;
            while (s < e && ident_char((unsigned char)*s))
                s++;
            if (c == '#') {
                const char *id = node_attr(n, "id");
                if (!id || (int)strlen(id) != s - t || strncmp(id, t, (size_t)(s - t)))
                    return 0;
            } else if (!has_class(n, t, (int)(s - t))) {
                return 0;
            }
        } else if (c == '[') {
            const char *t = s;
            while (s < e && *s != ']' && *s != '=' && *s != '~' && *s != '^' && *s != '$' &&
                   *s != '*' && *s != '|')
                s++;
            char name[64];
            snprintf(name, sizeof(name), "%.*s", (int)(s - t), t);
            for (char *q = name + strlen(name); q > name && q[-1] == ' '; q--)
                q[-1] = 0;
            const char *have = node_attr(n, name);
            char op = 0;
            if (s < e && *s != ']') {
                op = *s == '=' ? '=' : *s;
                s += *s == '=' ? 1 : 2;
            }
            char want[256] = "";
            if (op) {
                while (s < e && *s == ' ')
                    s++;
                char q = s < e && (*s == '"' || *s == '\'') ? *s++ : 0;
                const char *v = s;
                while (s < e && (q ? *s != q : *s != ']' && *s != ' '))
                    s++;
                snprintf(want, sizeof(want), "%.*s", (int)(s - v), v);
                if (q && s < e)
                    s++;
                while (s < e && *s != ']')
                    s++;            /* flags like i */
            }
            if (s < e && *s == ']')
                s++;
            if (!have)
                return 0;
            int hl = (int)strlen(have), wl = (int)strlen(want);
            switch (op) {
            case 0: break;
            case '=': if (strcmp(have, want)) return 0; break;
            case '~': {
                struct node tmp = { 0 };
                struct attr a = { "class", (char *)have, NULL };
                tmp.attrs = &a;
                if (!has_class(&tmp, want, wl))
                    return 0;
                break;
            }
            case '^': if (wl > hl || strncmp(have, want, (size_t)wl)) return 0; break;
            case '$': if (wl > hl || strcmp(have + hl - wl, want)) return 0; break;
            case '*': if (!strstr(have, want)) return 0; break;
            case '|': if (strncmp(have, want, (size_t)wl) || (have[wl] && have[wl] != '-')) return 0; break;
            }
        } else if (c == ':') {
            if (s < e && *s == ':')
                return 0;               /* pseudo-elements match no element */
            const char *t = s;
            while (s < e && ident_char((unsigned char)*s))
                s++;
            const char *arg = NULL;
            int alen = 0;
            if (s < e && *s == '(') {
                int depth = 1;
                arg = ++s;
                while (s < e && depth) {
                    if (*s == '(')
                        depth++;
                    else if (*s == ')')
                        depth--;
                    if (depth)
                        s++;
                }
                alen = (int)(s - arg);
                if (s < e)
                    s++;
            }
            if (!match_pseudo(n, t, (int)(s - t) - (arg ? alen + 2 : 0), arg, alen))
                return 0;
        } else if (c == ' ') {
            continue;
        } else {
            return 0;
        }
    }
    return 1;
}

/* A complex selector (compounds and combinators), matched right to left. */
static int match_complex(const struct node *n, const char *s, int len)
{
    /* find the last combinator outside brackets and parentheses */
    int depth = 0, at = -1;
    char comb = 0;
    for (int i = len - 1; i >= 0; i--) {
        char c = s[i];
        if (c == ']' || c == ')')
            depth++;
        else if (c == '[' || c == '(')
            depth--;
        else if (!depth && (c == '>' || c == '+' || c == '~')) {
            at = i;
            comb = c;
            break;
        } else if (!depth && c == ' ') {
            /* a descendant combinator unless next to another combinator */
            int j = i;
            while (j > 0 && s[j - 1] == ' ')
                j--;
            int k = i;
            while (k < len - 1 && s[k + 1] == ' ')
                k++;
            if (j == 0 || k == len - 1)
                continue;
            char before = s[j - 1], after = s[k + 1];
            if (before == '>' || before == '+' || before == '~' || after == '>' || after == '+' || after == '~')
                continue;
            at = i;
            comb = ' ';
            break;
        }
    }
    if (at < 0)
        return match_compound(n, s, len);
    if (!match_compound(n, s + at + 1, len - at - 1))
        return 0;
    int left = at;
    while (left > 0 && s[left - 1] == ' ')
        left--;
    switch (comb) {
    case '>':
        return n->parent && match_complex(n->parent, s, left);
    case '+': {
        const struct node *p = prev_element(n);
        return p && match_complex(p, s, left);
    }
    case '~':
        for (const struct node *p = prev_element(n); p; p = prev_element(p))
            if (match_complex(p, s, left))
                return 1;
        return 0;
    default:
        for (const struct node *p = n->parent; p && p->type == N_ELEMENT; p = p->parent)
            if (match_complex(p, s, left))
                return 1;
        return 0;
    }
}

/* A selector list. */
static int matches(const struct node *n, const char *sel)
{
    const char *s = sel;
    while (*s) {
        int depth = 0;
        const char *e = s;
        while (*e && (depth || *e != ',')) {
            if (*e == '(' || *e == '[')
                depth++;
            else if (*e == ')' || *e == ']')
                depth--;
            e++;
        }
        const char *a = s, *b = e;
        while (a < b && isspace((unsigned char)*a))
            a++;
        while (b > a && isspace((unsigned char)b[-1]))
            b--;
        if (b > a && match_complex(n, a, (int)(b - a)))
            return 1;
        s = *e ? e + 1 : e;
    }
    return 0;
}

/* ---- wrappers ---- */

static jsval wrap(struct jsdom *d, struct node *n)
{
    if (!n)
        return js_null;
    if (!n->js) {
        struct jsobj *o = js_new_host(d->J, &node_class, n);
        o->proto = d->element_proto;
        n->js = o;
    }
    return js_obj(n->js);
}

static struct node *node_of(jsval v)
{
    return v.t == JS_OBJ ? js_host_data(v.u.o, &node_class) : NULL;
}

static struct node *this_node(struct js *J, jsval self)
{
    struct node *n = node_of(self);
    if (!n && self.t == JS_OBJ && js_host_data(self.u.o, &document_class))
        return dom(J)->h.root;
    if (!n)
        js_throw_error(J, "TypeError: not a node");
    return n;
}

/* Elements under root (not root itself) that pass the test, in document order. */
static jsval collect(struct jsdom *d, struct node *root, int (*test)(const struct node *, const void *),
                     const void *arg, int first_only)
{
    struct jsobj *a = js_new_array(d->J);
    for (struct node *k = root ? node_next(root, root) : NULL; k; k = node_next(k, root))
        if (k->type == N_ELEMENT && test(k, arg)) {
            if (first_only)
                return wrap(d, k);
            js_array_push(d->J, a, wrap(d, k));
        }
    return first_only ? js_null : js_obj(a);
}

static int test_selector(const struct node *n, const void *arg)
{
    return matches(n, arg);
}

static int test_tag(const struct node *n, const void *arg)
{
    return !strcmp(arg, "*") || !strcasecmp(n->name, arg);
}

static int test_classes(const struct node *n, const void *arg)
{
    const char *s = arg;
    while (*s) {
        while (*s == ' ')
            s++;
        const char *t = s;
        while (*s && *s != ' ')
            s++;
        if (s > t && !has_class(n, t, (int)(s - t)))
            return 0;
    }
    return 1;
}

static int test_name(const struct node *n, const void *arg)
{
    const char *v = node_attr(n, "name");
    return v && !strcmp(v, arg);
}

static int test_id(const struct node *n, const void *arg)
{
    const char *v = node_attr(n, "id");
    return v && !strcmp(v, arg);
}

static struct node *by_id(struct node *root, const char *id)
{
    for (struct node *k = root; k; k = node_next(k, root))
        if (k->type == N_ELEMENT && test_id(k, id))
            return k;
    return NULL;
}

/* ---- element properties ---- */

static void camel_to_kebab(const char *in, char *out, int size)
{
    int n = 0;
    for (; *in && n < size - 2; in++) {
        if (*in >= 'A' && *in <= 'Z') {
            out[n++] = '-';
            out[n++] = (char)(*in + 32);
        } else {
            out[n++] = *in;
        }
    }
    out[n] = 0;
}

static const char *const reflected[] = {
    "id", "title", "lang", "name", "type", "alt", "rel", "target", "action", "method",
    "placeholder", "role", "width", "height", "colspan", "rowspan", "content", "charset",
    "media", "enctype", "accept", "autocomplete", "dir", "accesskey", "tabindex", "label",
    "maxlength", "size", "pattern", "min", "max", "step", "cols", "rows", "wrap", "download",
    NULL,
};

static const char *const boolean_attrs[] = {
    "disabled", "readonly", "required", "hidden", "multiple", "autofocus", "async", "defer",
    "open", "novalidate", "selected", NULL,
};

static int in_list(const char *const *list, const char *key, char *attr, int size)
{
    for (int i = 0; list[i]; i++)
        if (!strcasecmp(list[i], key)) {
            snprintf(attr, (size_t)size, "%s", list[i]);
            return 1;
        }
    return 0;
}

/* The value of a select: its chosen option's value. */
static struct node *selected_option(struct node *sel)
{
    int k = 0;
    for (struct node *o = sel; o; o = node_next(o, sel))
        if (o->type == N_ELEMENT && o->tag == T_OPTION) {
            if (k == sel->checked)
                return o;
            k++;
        }
    return NULL;
}

static const char *control_value(struct node *n)
{
    if (n->tag == T_SELECT) {
        struct node *o = selected_option(n);
        if (!o)
            return "";
        const char *v = node_attr(o, "value");
        return v ? v : o->value ? o->value : "";
    }
    if (n->tag == T_OPTION) {
        const char *v = node_attr(n, "value");
        return v ? v : n->value ? n->value : "";
    }
    if (n->value)
        return n->value;
    const char *v = node_attr(n, "value");
    return v ? v : "";
}

static jsval rect_object(struct jsdom *d, struct node *n)
{
    struct js *J = d->J;
    int x = 0, y = 0, w = 0, h = 0, sx = 0, sy = 0;
    if (d->h.node_box)
        d->h.node_box(d->h.ctx, n, &x, &y, &w, &h);
    if (d->h.scroll_pos)
        d->h.scroll_pos(d->h.ctx, &sx, &sy);
    struct jsobj *r = js_new_object(J);
    jsval o = js_obj(r);
    js_set(J, o, "x", js_num(x - sx));
    js_set(J, o, "y", js_num(y - sy));
    js_set(J, o, "left", js_num(x - sx));
    js_set(J, o, "top", js_num(y - sy));
    js_set(J, o, "width", js_num(w));
    js_set(J, o, "height", js_num(h));
    js_set(J, o, "right", js_num(x - sx + w));
    js_set(J, o, "bottom", js_num(y - sy + h));
    return o;
}

static int node_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    struct jsdom *d = dom(J);
    struct node *n = o->hdata;
    char attr[32];
#define IS(x) (!strcmp(key, x))
    if (IS("nodeType")) {
        *out = js_num(n->type == N_TEXT ? 3 : is_fragment(n) ? 11 : 1);
        return 1;
    }
    if (IS("nodeName") || IS("tagName")) {
        if (n->type == N_TEXT) {
            *out = IS("tagName") ? js_undefined : js_str(J, "#text");
            return 1;
        }
        if (is_fragment(n)) {
            *out = js_str(J, "#document-fragment");
            return 1;
        }
        char up[64];
        int i = 0;
        for (; n->name[i] && i < 63; i++)
            up[i] = (char)toupper((unsigned char)n->name[i]);
        up[i] = 0;
        *out = js_str(J, up);
        return 1;
    }
    if (IS("localName")) {
        *out = js_str(J, n->type == N_TEXT ? "" : n->name);
        return 1;
    }
    if (IS("parentNode") || IS("parentElement")) {
        struct node *p = n->parent;
        *out = p && (IS("parentNode") || p->type == N_ELEMENT) ? wrap(d, p) : js_null;
        if (!p && IS("parentNode") && n == d->h.root)
            *out = js_obj(d->document);
        return 1;
    }
    if (IS("ownerDocument")) {
        *out = js_obj(d->document);
        return 1;
    }
    if (IS("firstChild")) { *out = wrap(d, n->first); return 1; }
    if (IS("lastChild")) { *out = wrap(d, n->last); return 1; }
    if (IS("nextSibling")) { *out = wrap(d, n->next); return 1; }
    if (IS("previousSibling")) { *out = wrap(d, n->prev); return 1; }
    if (IS("nextElementSibling")) { *out = wrap(d, (struct node *)next_element(n)); return 1; }
    if (IS("previousElementSibling")) { *out = wrap(d, (struct node *)prev_element(n)); return 1; }
    if (IS("firstElementChild") || IS("lastElementChild") || IS("children") || IS("childNodes") ||
        IS("childElementCount")) {
        struct jsobj *a = js_new_array(J);
        for (struct node *k = n->first; k; k = k->next)
            if (k->type == N_ELEMENT || IS("childNodes"))
                js_array_push(J, a, wrap(d, k));
        if (IS("firstElementChild"))
            *out = a->len ? a->items[0] : js_null;
        else if (IS("lastElementChild"))
            *out = a->len ? a->items[a->len - 1] : js_null;
        else if (IS("childElementCount"))
            *out = js_num(a->len);
        else
            *out = js_obj(a);
        return 1;
    }
    if (IS("textContent") || IS("innerText") || IS("nodeValue") || IS("data") || IS("wholeText")) {
        if ((IS("nodeValue") || IS("data")) && n->type != N_TEXT) {
            *out = js_null;
            return 1;
        }
        *out = text_content(J, n);
        return 1;
    }
    if (IS("length") && n->type == N_TEXT) {
        *out = js_num(n->len);
        return 1;
    }
    if (n->type != N_ELEMENT)
        return 0;
    if (IS("innerHTML") || IS("outerHTML")) {
        *out = html_of(J, n, IS("outerHTML"));
        return 1;
    }
    if (IS("className")) {
        const char *v = node_attr(n, "class");
        *out = js_str(J, v ? v : "");
        return 1;
    }
    if (IS("htmlFor")) {
        const char *v = node_attr(n, "for");
        *out = js_str(J, v ? v : "");
        return 1;
    }
    if (IS("href") || IS("src")) {
        const char *v = node_attr(n, key);
        if (!v) {
            *out = js_str(J, "");
            return 1;
        }
        char abs[URL_MAX];
        url_resolve(d->h.base, v, abs, sizeof(abs));
        *out = js_str(J, abs);
        return 1;
    }
    if (IS("value")) {
        *out = js_str(J, control_value(n));
        return 1;
    }
    if (IS("defaultValue")) {
        const char *v = node_attr(n, "value");
        *out = js_str(J, v ? v : "");
        return 1;
    }
    if (IS("checked")) {
        *out = js_bool(n->checked);
        return 1;
    }
    if (IS("selectedIndex")) {
        *out = js_num(n->tag == T_SELECT ? n->checked : -1);
        return 1;
    }
    if (IS("options") && n->tag == T_SELECT) {
        *out = collect(d, n, test_tag, "option", 0);
        return 1;
    }
    if (IS("form")) {
        struct node *f = n->parent;
        while (f && !(f->type == N_ELEMENT && f->tag == T_FORM))
            f = f->parent;
        *out = wrap(d, f);
        return 1;
    }
    if (IS("elements") && n->tag == T_FORM) {
        struct jsobj *a = js_new_array(J);
        for (struct node *k = n; k; k = node_next(k, n))
            if (k->type == N_ELEMENT && (k->tag == T_INPUT || k->tag == T_SELECT ||
                                         k->tag == T_TEXTAREA || k->tag == T_BUTTON))
                js_array_push(J, a, wrap(d, k));
        *out = js_obj(a);
        return 1;
    }
    if (IS("style")) {
        struct jsobj *s = js_new_host(J, &style_class, n);
        s->proto = d->style_proto;
        *out = js_obj(s);
        return 1;
    }
    if (IS("classList")) {
        struct jsobj *c = js_new_host(J, &classlist_class, n);
        c->proto = d->classlist_proto;
        *out = js_obj(c);
        return 1;
    }
    if (IS("dataset")) {
        *out = js_obj(js_new_host(J, &dataset_class, n));
        return 1;
    }
    if (IS("attributes")) {
        struct jsobj *a = js_new_array(J);
        for (struct attr *at = n->attrs; at; at = at->next) {
            struct jsobj *p = js_new_object(J);
            js_set(J, js_obj(p), "name", js_str(J, at->name));
            js_set(J, js_obj(p), "value", js_str(J, at->value ? at->value : ""));
            js_array_push(J, a, js_obj(p));
        }
        *out = js_obj(a);
        return 1;
    }
    if (IS("offsetWidth") || IS("offsetHeight") || IS("clientWidth") || IS("clientHeight") ||
        IS("offsetTop") || IS("offsetLeft") || IS("scrollWidth") || IS("scrollHeight")) {
        int x = 0, y = 0, w = 0, h = 0;
        if (d->h.node_box)
            d->h.node_box(d->h.ctx, n, &x, &y, &w, &h);
        *out = js_num(strstr(key, "Width") ? w : strstr(key, "Height") ? h : IS("offsetTop") ? y : x);
        return 1;
    }
    if (IS("scrollTop") || IS("scrollLeft")) {
        *out = js_num(0);
        return 1;
    }
    if (IS("offsetParent")) {
        *out = wrap(d, n->parent);
        return 1;
    }
    if (IS("isConnected")) {
        *out = js_bool(contains(d->h.root, n));
        return 1;
    }
    if (in_list(reflected, key, attr, sizeof(attr))) {
        const char *v = node_attr(n, attr);
        *out = js_str(J, v ? v : "");
        return 1;
    }
    if (in_list(boolean_attrs, key, attr, sizeof(attr)) || IS("readOnly") || IS("noValidate")) {
        if (IS("readOnly"))
            strcpy(attr, "readonly");
        else if (IS("noValidate"))
            strcpy(attr, "novalidate");
        *out = js_bool(node_attr(n, attr) != NULL);
        return 1;
    }
    return 0;
#undef IS
}

static int node_set(struct js *J, struct jsobj *o, const char *key, jsval v)
{
    struct jsdom *d = dom(J);
    struct node *n = o->hdata;
    char attr[32];
#define IS(x) (!strcmp(key, x))
    if (IS("textContent") || IS("innerText") || IS("nodeValue") || IS("data")) {
        struct jsstr *s = v.t == JS_NULL || v.t == JS_UNDEF ? S(J, "") : js_tostr(J, v);
        set_text_content(d, n, s->s, s->len);
        return 1;
    }
    if (n->type != N_ELEMENT)
        return 0;
    if (IS("innerHTML")) {
        struct jsstr *s = js_tostr(J, v);
        set_inner_html(d, n, s->s, s->len);
        return 1;
    }
    if (IS("outerHTML")) {
        struct jsstr *s = js_tostr(J, v);
        struct node *frag = parse_fragment(d, s->s, s->len);
        if (frag && n->parent) {
            struct node *p = n->parent, *next = n->next;
            unlink_node(n);
            insert_node(d, p, frag, next);
        }
        return 1;
    }
    if (IS("className")) {
        set_attr(d, n, "class", js_tostr(J, v)->s);
        return 1;
    }
    if (IS("htmlFor")) {
        set_attr(d, n, "for", js_tostr(J, v)->s);
        return 1;
    }
    if (IS("href") || IS("src")) {
        set_attr(d, n, key, js_tostr(J, v)->s);
        if (IS("src") && n->tag == T_IMG && d->h.load_image)
            d->h.load_image(d->h.ctx, n);
        if (IS("src") && n->tag == T_SCRIPT && contains(d->h.root, n) && d->h.load_script) {
            char abs[URL_MAX];
            url_resolve(d->h.base, js_tostr(J, v)->s, abs, sizeof(abs));
            d->h.load_script(d->h.ctx, n, abs);
        }
        return 1;
    }
    if (IS("value")) {
        struct jsstr *s = js_tostr(J, v);
        if (n->tag == T_SELECT) {
            int k = 0;
            for (struct node *op = n; op; op = node_next(op, n))
                if (op->type == N_ELEMENT && op->tag == T_OPTION) {
                    if (!strcmp(control_value(op), s->s))
                        n->checked = k;
                    k++;
                }
        } else {
            free(n->value);
            n->value = strdup(s->s);
            n->cursor = n->value ? (int)strlen(n->value) : 0;
        }
        d->dirty = 1;
        return 1;
    }
    if (IS("checked")) {
        n->checked = js_truthy(v);
        d->dirty = 1;
        return 1;
    }
    if (IS("selectedIndex")) {
        n->checked = (int)js_to_number(J, v);
        d->dirty = 1;
        return 1;
    }
    if (IS("style")) {
        set_attr(d, n, "style", js_tostr(J, v)->s);
        return 1;
    }
    if (in_list(reflected, key, attr, sizeof(attr))) {
        set_attr(d, n, attr, js_tostr(J, v)->s);
        return 1;
    }
    if (in_list(boolean_attrs, key, attr, sizeof(attr)) || IS("readOnly")) {
        if (IS("readOnly"))
            strcpy(attr, "readonly");
        if (js_truthy(v))
            set_attr(d, n, attr, "");
        else
            remove_attr(d, n, attr);
        return 1;
    }
    return 0;
#undef IS
}

static const struct jsclass node_class = { "HTMLElement", node_get, node_set };

/* ---- style ---- */

/* Find the declaration of prop in a style attribute: its value's start and length. */
static const char *find_decl(const char *css, const char *prop, int *vlen, const char **decl_start,
                             const char **decl_end)
{
    size_t pl = strlen(prop);
    const char *s = css;
    while (s && *s) {
        while (*s == ' ' || *s == ';' || *s == '\n' || *s == '\t')
            s++;
        const char *start = s;
        const char *colon = strchr(s, ':');
        const char *semi = strchr(s, ';');
        if (!colon || (semi && semi < colon)) {
            s = semi ? semi + 1 : NULL;
            continue;
        }
        const char *ne = colon;
        while (ne > s && ne[-1] == ' ')
            ne--;
        const char *end = semi ? semi : s + strlen(s);
        if ((size_t)(ne - s) == pl && !strncasecmp(s, prop, pl)) {
            const char *v = colon + 1;
            while (v < end && *v == ' ')
                v++;
            const char *ve = end;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\n'))
                ve--;
            *vlen = (int)(ve - v);
            if (decl_start)
                *decl_start = start;
            if (decl_end)
                *decl_end = semi ? semi + 1 : end;
            return v;
        }
        s = semi ? semi + 1 : NULL;
    }
    return NULL;
}

static void style_set_prop(struct jsdom *d, struct node *n, const char *prop, const char *value)
{
    const char *css = node_attr(n, "style");
    char *nv = malloc((css ? strlen(css) : 0) + strlen(prop) + strlen(value) + 8);
    if (!nv)
        return;
    nv[0] = 0;
    if (css) {
        int vlen;
        const char *ds, *de;
        if (find_decl(css, prop, &vlen, &ds, &de)) {
            strncat(nv, css, (size_t)(ds - css));
            strcat(nv, de);
        } else {
            strcat(nv, css);
        }
    }
    if (*value) {
        size_t l = strlen(nv);
        while (l && (nv[l - 1] == ' ' || nv[l - 1] == ';'))
            nv[--l] = 0;
        if (l)
            strcat(nv, "; ");
        strcat(nv, prop);
        strcat(nv, ": ");
        strcat(nv, value);
    }
    set_attr(d, n, "style", nv);
    free(nv);
}

static int style_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    struct node *n = o->hdata;
    const char *css = node_attr(n, "style");
    if (!strcmp(key, "cssText")) {
        *out = js_str(J, css ? css : "");
        return 1;
    }
    if (!strcmp(key, "length")) {
        *out = js_num(0);
        return 1;
    }
    char prop[64];
    camel_to_kebab(!strcmp(key, "cssFloat") ? "float" : key, prop, sizeof(prop));
    if (!isalpha((unsigned char)key[0]))
        return 0;
    int vlen;
    const char *v = css ? find_decl(css, prop, &vlen, NULL, NULL) : NULL;
    if (o->proto && js_is_callable(js_get_str(J, js_obj(o->proto), S(J, key))))
        return 0;                       /* setProperty and friends */
    *out = v ? js_strn(J, v, vlen) : js_str(J, "");
    return 1;
}

static int style_set(struct js *J, struct jsobj *o, const char *key, jsval v)
{
    struct jsdom *d = dom(J);
    struct node *n = o->hdata;
    if (!strcmp(key, "cssText")) {
        set_attr(d, n, "style", js_tostr(J, v)->s);
        return 1;
    }
    char prop[64];
    camel_to_kebab(!strcmp(key, "cssFloat") ? "float" : key, prop, sizeof(prop));
    struct jsstr *s = v.t == JS_NULL || v.t == JS_UNDEF ? S(J, "") : js_tostr(J, v);
    style_set_prop(d, n, prop, s->s);
    return 1;
}

static const struct jsclass style_class = { "CSSStyleDeclaration", style_get, style_set };

static jsval style_set_property(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = self.t == JS_OBJ ? js_host_data(self.u.o, &style_class) : NULL;
    if (n)
        style_set_prop(dom(J), n, js_tostr(J, ARG(0))->s, js_tostr(J, ARG(1))->s);
    return js_undefined;
}

static jsval style_get_property(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = self.t == JS_OBJ ? js_host_data(self.u.o, &style_class) : NULL;
    if (!n)
        return js_str(J, "");
    const char *css = node_attr(n, "style");
    int vlen;
    const char *v = css ? find_decl(css, js_tostr(J, ARG(0))->s, &vlen, NULL, NULL) : NULL;
    return v ? js_strn(J, v, vlen) : js_str(J, "");
}

static jsval style_remove_property(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = self.t == JS_OBJ ? js_host_data(self.u.o, &style_class) : NULL;
    if (n)
        style_set_prop(dom(J), n, js_tostr(J, ARG(0))->s, "");
    return js_str(J, "");
}

/* getComputedStyle: a few computed properties, the rest from the style attribute. */
static int computed_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    struct node *n = o->hdata;
    struct jsdom *d = dom(J);
    const struct style *st = n->style;
    char b[64];
    if (!strcmp(key, "display")) {
        static const char *const names[] = { "inline", "block", "list-item", "inline-block", "table",
                                             "inline-table", "table-row", "table-row-group", "table-cell",
                                             "table-caption", "flex", "none" };
        *out = js_str(J, st && st->display < 12 ? names[st->display] : "inline");
        return 1;
    }
    if (!strcmp(key, "visibility")) {
        *out = js_str(J, st && st->hidden ? "hidden" : "visible");
        return 1;
    }
    if (!strcmp(key, "color") || !strcmp(key, "backgroundColor")) {
        uint32_t c = st ? (!strcmp(key, "color") ? st->color : st->bg) : 0;
        if (ALPHA(c) == 255)
            snprintf(b, sizeof(b), "rgb(%u, %u, %u)", (c >> 16) & 255, (c >> 8) & 255, c & 255);
        else
            snprintf(b, sizeof(b), "rgba(%u, %u, %u, 0)", (c >> 16) & 255, (c >> 8) & 255, c & 255);
        *out = js_str(J, b);
        return 1;
    }
    if (!strcmp(key, "fontSize")) {
        snprintf(b, sizeof(b), "%dpx", st ? st->font_size : 16);
        *out = js_str(J, b);
        return 1;
    }
    if (!strcmp(key, "width") || !strcmp(key, "height")) {
        int x = 0, y = 0, w = 0, h = 0;
        if (d->h.node_box)
            d->h.node_box(d->h.ctx, n, &x, &y, &w, &h);
        snprintf(b, sizeof(b), "%dpx", key[0] == 'w' ? w : h);
        *out = js_str(J, b);
        return 1;
    }
    if (!strcmp(key, "getPropertyValue"))
        return 0;
    struct jsobj tmp = *o;
    tmp.hcls = &style_class;
    return style_get(J, &tmp, key, out);
}

static const struct jsclass computed_class = { "CSSStyleDeclaration", computed_get, NULL };

/* ---- classList and dataset ---- */

static int classlist_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    struct node *n = o->hdata;
    const char *v = node_attr(n, "class");
    if (!strcmp(key, "value")) {
        *out = js_str(J, v ? v : "");
        return 1;
    }
    if (!strcmp(key, "length")) {
        int count = 0;
        for (const char *s = v ? v : ""; *s;) {
            while (*s == ' ')
                s++;
            if (*s)
                count++;
            while (*s && *s != ' ')
                s++;
        }
        *out = js_num(count);
        return 1;
    }
    return 0;
}

static const struct jsclass classlist_class = { "DOMTokenList", classlist_get, NULL };

static struct node *classlist_node(jsval self)
{
    return self.t == JS_OBJ ? js_host_data(self.u.o, &classlist_class) : NULL;
}

/* Add (op 1), remove (0) or toggle (2) class name; returns whether it is there after. */
static int class_op(struct jsdom *d, struct node *n, const char *name, int op)
{
    const char *v = node_attr(n, "class");
    int len = (int)strlen(name);
    int present = v && has_class(n, name, len);
    if (op == 2)
        op = !present;
    if (op == present)
        return present;
    char *nv = malloc((v ? strlen(v) : 0) + (size_t)len + 2);
    if (!nv)
        return present;
    nv[0] = 0;
    if (op) {
        if (v && *v) {
            strcpy(nv, v);
            strcat(nv, " ");
        }
        strcat(nv, name);
    } else {
        for (const char *s = v; *s;) {
            while (*s == ' ')
                s++;
            const char *t = s;
            while (*s && *s != ' ')
                s++;
            if (s > t && !(s - t == len && !strncmp(t, name, (size_t)len))) {
                if (*nv)
                    strcat(nv, " ");
                strncat(nv, t, (size_t)(s - t));
            }
        }
    }
    set_attr(d, n, "class", nv);
    free(nv);
    return op;
}

static jsval classlist_add(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    for (int i = 0; n && i < argc; i++)
        class_op(dom(J), n, js_tostr(J, args[i])->s, 1);
    return js_undefined;
}

static jsval classlist_remove(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    for (int i = 0; n && i < argc; i++)
        class_op(dom(J), n, js_tostr(J, args[i])->s, 0);
    return js_undefined;
}

static jsval classlist_toggle(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    if (!n)
        return js_bool(0);
    int op = argc > 1 ? js_truthy(args[1]) : 2;
    return js_bool(class_op(dom(J), n, js_tostr(J, ARG(0))->s, op));
}

static jsval classlist_contains(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    struct jsstr *s = js_tostr(J, ARG(0));
    return js_bool(n && has_class(n, s->s, s->len));
}

static jsval classlist_replace(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    struct jsstr *a = js_tostr(J, ARG(0)), *b = js_tostr(J, ARG(1));
    if (!n || !has_class(n, a->s, a->len))
        return js_bool(0);
    class_op(dom(J), n, a->s, 0);
    class_op(dom(J), n, b->s, 1);
    return js_bool(1);
}

static jsval classlist_item(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    int want = (int)js_to_number(J, ARG(0)), k = 0;
    const char *v = n ? node_attr(n, "class") : NULL;
    for (const char *s = v ? v : ""; *s;) {
        while (*s == ' ')
            s++;
        const char *t = s;
        while (*s && *s != ' ')
            s++;
        if (s > t && k++ == want)
            return js_strn(J, t, (int)(s - t));
    }
    return js_null;
}

static int dataset_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    char name[96] = "data-";
    camel_to_kebab(key, name + 5, sizeof(name) - 5);
    const char *v = node_attr(o->hdata, name);
    if (!v)
        return 0;
    *out = js_str(J, v);
    return 1;
}

static int dataset_set(struct js *J, struct jsobj *o, const char *key, jsval v)
{
    char name[96] = "data-";
    camel_to_kebab(key, name + 5, sizeof(name) - 5);
    set_attr(dom(J), o->hdata, name, js_tostr(J, v)->s);
    return 1;
}

static const struct jsclass dataset_class = { "DOMStringMap", dataset_get, dataset_set };

/* ---- element methods ---- */

static struct node *arg_node(struct js *J, jsval v)
{
    struct node *n = node_of(v);
    if (!n)
        js_throw_error(J, "TypeError: argument is not a node");
    return n;
}

static jsval el_append_child(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *p = this_node(J, self), *c = arg_node(J, ARG(0));
    if (!p || !c)
        return js_undefined;
    struct jsdom *d = dom(J);
    int script = is_element(c) && c->tag == T_SCRIPT;
    if (insert_node(d, p, c, NULL) < 0)
        return js_throw_error(J, "HierarchyRequestError: the new child contains the parent");
    /* A script added to the document runs (or loads). */
    if (script && contains(d->h.root, c)) {
        const char *src = node_attr(c, "src");
        if (src && *src && d->h.load_script) {
            char abs[URL_MAX];
            url_resolve(d->h.base, src, abs, sizeof(abs));
            d->h.load_script(d->h.ctx, c, abs);
        } else if (!src && c->first && c->first->type == N_TEXT) {
            jsdom_run_script(d, c, c->first->text, c->first->len, "inserted script");
        }
    }
    if (is_element(c) && c->tag == T_IMG && d->h.load_image)
        d->h.load_image(d->h.ctx, c);
    for (struct node *k = c; k; k = node_next(k, c))
        if (k != c && k->type == N_ELEMENT && k->tag == T_IMG && d->h.load_image)
            d->h.load_image(d->h.ctx, k);
    return ARG(0);
}

static jsval el_insert_before(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *p = this_node(J, self), *c = arg_node(J, ARG(0));
    if (!p || !c)
        return js_undefined;
    struct node *ref = node_of(ARG(1));
    if (insert_node(dom(J), p, c, ref) < 0)
        return js_throw_error(J, "HierarchyRequestError: the new child contains the parent");
    if (is_element(c) && c->tag == T_IMG && dom(J)->h.load_image)
        dom(J)->h.load_image(dom(J)->h.ctx, c);
    return ARG(0);
}

static jsval el_remove_child(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *p = this_node(J, self), *c = arg_node(J, ARG(0));
    if (!p || !c)
        return js_undefined;
    if (c->parent != p)
        return js_throw_error(J, "NotFoundError: the node is not a child of this node");
    unlink_node(c);
    dom(J)->dirty = 1;
    return ARG(0);
}

static jsval el_replace_child(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *p = this_node(J, self), *nw = arg_node(J, ARG(0)), *old = arg_node(J, ARG(1));
    if (!p || !nw || !old || old->parent != p)
        return js_undefined;
    struct node *next = old->next;
    unlink_node(old);
    insert_node(dom(J), p, nw, next);
    return ARG(1);
}

static jsval el_remove(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct node *n = this_node(J, self);
    if (n && n->parent) {
        unlink_node(n);
        dom(J)->dirty = 1;
    }
    return js_undefined;
}

/* append/prepend/before/after/replaceWith: nodes or strings (as text). */
static jsval insert_items(struct js *J, jsval self, jsval *args, int argc, int where)
{
    struct node *n = this_node(J, self);
    if (!n)
        return js_undefined;
    struct jsdom *d = dom(J);
    struct node *parent = where < 2 ? n : n->parent;
    if (!parent)
        return js_undefined;
    struct node *ref = where == 0 ? NULL : where == 1 ? n->first : where == 2 ? n : n->next;
    for (int i = 0; i < argc; i++) {
        struct node *c = node_of(args[i]);
        if (!c) {
            struct jsstr *s = js_tostr(J, args[i]);
            c = new_text(d, s->s, s->len);
        }
        if (c)
            insert_node(d, parent, c, ref);
    }
    if (where == 4) {
        unlink_node(n);
        d->dirty = 1;
    }
    return js_undefined;
}

static jsval el_append(struct js *J, jsval s, jsval *a, int n) { return insert_items(J, s, a, n, 0); }
static jsval el_prepend(struct js *J, jsval s, jsval *a, int n) { return insert_items(J, s, a, n, 1); }
static jsval el_before(struct js *J, jsval s, jsval *a, int n) { return insert_items(J, s, a, n, 2); }
static jsval el_after(struct js *J, jsval s, jsval *a, int n) { return insert_items(J, s, a, n, 3); }
static jsval el_replace_with(struct js *J, jsval s, jsval *a, int n) { return insert_items(J, s, a, n, 4); }

static struct node *clone(struct jsdom *d, const struct node *n, int deep)
{
    struct node *c = n->type == N_TEXT ? new_text(d, n->text, n->len) : new_node(d, N_ELEMENT, n->name);
    if (!c)
        return NULL;
    if (n->type == N_ELEMENT) {
        for (const struct attr *a = n->attrs; a; a = a->next)
            set_attr(d, c, a->name, a->value ? a->value : "");
        c->checked = n->checked;
        if (n->value)
            c->value = strdup(n->value);
    }
    if (deep)
        for (const struct node *k = n->first; k; k = k->next) {
            struct node *kc = clone(d, k, 1);
            if (kc)
                insert_node(d, c, kc, NULL);
        }
    return c;
}

static jsval el_clone_node(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (!n)
        return js_undefined;
    return wrap(dom(J), clone(dom(J), n, js_truthy(ARG(0))));
}

static jsval el_contains(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self), *o = node_of(ARG(0));
    return js_bool(n && o && contains(n, o));
}

static jsval el_has_child_nodes(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct node *n = this_node(J, self);
    return js_bool(n && n->first);
}

static jsval el_get_attribute(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (!n || n->type != N_ELEMENT)
        return js_null;
    const char *v = node_attr(n, js_tostr(J, ARG(0))->s);
    return v ? js_str(J, v) : js_null;
}

static jsval el_set_attribute(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (!n)
        return js_undefined;
    struct jsstr *name = js_tostr(J, ARG(0)), *value = js_tostr(J, ARG(1));
    set_attr(dom(J), n, name->s, value->s);
    if (!strcasecmp(name->s, "value") && n->tag == T_INPUT) {
        free(n->value);
        n->value = strdup(value->s);
    }
    if (!strcasecmp(name->s, "src") && n->tag == T_IMG && dom(J)->h.load_image)
        dom(J)->h.load_image(dom(J)->h.ctx, n);
    return js_undefined;
}

static jsval el_remove_attribute(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (n)
        remove_attr(dom(J), n, js_tostr(J, ARG(0))->s);
    return js_undefined;
}

static jsval el_has_attribute(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    return js_bool(n && n->type == N_ELEMENT && node_attr(n, js_tostr(J, ARG(0))->s));
}

static jsval el_toggle_attribute(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (!n)
        return js_bool(0);
    const char *name = js_tostr(J, ARG(0))->s;
    int want = argc > 1 ? js_truthy(args[1]) : !node_attr(n, name);
    if (want)
        set_attr(dom(J), n, name, "");
    else
        remove_attr(dom(J), n, name);
    return js_bool(want);
}

static jsval el_query_selector(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (!n)
        return js_undefined;
    return collect(dom(J), n, test_selector, js_tostr(J, ARG(0))->s, 1);
}

static jsval el_query_selector_all(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (!n)
        return js_undefined;
    return collect(dom(J), n, test_selector, js_tostr(J, ARG(0))->s, 0);
}

static jsval el_get_by_tag(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (!n)
        return js_undefined;
    return collect(dom(J), n, test_tag, js_tostr(J, ARG(0))->s, 0);
}

static jsval el_get_by_class(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (!n)
        return js_undefined;
    return collect(dom(J), n, test_classes, js_tostr(J, ARG(0))->s, 0);
}

static jsval el_matches(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    return js_bool(n && matches(n, js_tostr(J, ARG(0))->s));
}

static jsval el_closest(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    const char *sel = js_tostr(J, ARG(0))->s;
    for (; n && n->type == N_ELEMENT; n = n->parent)
        if (matches(n, sel))
            return wrap(dom(J), n);
    return js_null;
}

static jsval el_get_bounding_rect(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct node *n = this_node(J, self);
    if (!n)
        return js_undefined;
    return rect_object(dom(J), n);
}

static jsval el_insert_adjacent_html(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (!n)
        return js_undefined;
    struct jsdom *d = dom(J);
    const char *where = js_tostr(J, ARG(0))->s;
    struct jsstr *html = js_tostr(J, ARG(1));
    struct node *frag = parse_fragment(d, html->s, html->len);
    if (!frag)
        return js_undefined;
    if (!strcasecmp(where, "beforebegin") && n->parent)
        insert_node(d, n->parent, frag, n);
    else if (!strcasecmp(where, "afterbegin"))
        insert_node(d, n, frag, n->first);
    else if (!strcasecmp(where, "beforeend"))
        insert_node(d, n, frag, NULL);
    else if (!strcasecmp(where, "afterend") && n->parent)
        insert_node(d, n->parent, frag, n->next);
    return js_undefined;
}

static jsval el_focus(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct node *n = this_node(J, self);
    if (n && dom(J)->h.focus)
        dom(J)->h.focus(dom(J)->h.ctx, n);
    return js_undefined;
}

static jsval el_scroll_into_view(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct node *n = this_node(J, self);
    struct jsdom *d = dom(J);
    int x, y, w, h;
    if (n && d->h.node_box && d->h.node_box(d->h.ctx, n, &x, &y, &w, &h) && d->h.scroll)
        d->h.scroll(d->h.ctx, 0, y, 0);
    return js_undefined;
}

static jsval nothing(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_undefined;
}

static jsval el_click(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct node *n = this_node(J, self);
    if (n)
        jsdom_dispatch(dom(J), n, "click");
    return js_undefined;
}

static jsval node_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct node *n = node_of(self);
    char b[96];
    if (n && n->type == N_ELEMENT && n->tag == T_A) {
        const char *href = node_attr(n, "href");
        char abs[URL_MAX];
        url_resolve(dom(J)->h.base, href ? href : "", abs, sizeof(abs));
        return js_str(J, abs);
    }
    snprintf(b, sizeof(b), "[object %s]", n && n->type == N_TEXT ? "Text" : "HTMLElement");
    return js_str(J, b);
}

/* ---- events ---- */

static struct jsstr *listener_key(struct js *J)
{
    return S(J, "__listeners");
}

static jsval target_of(struct js *J, jsval self)
{
    if (self.t == JS_UNDEF || self.t == JS_NULL)
        return js_obj(J->global);
    return self;
}

static jsval add_event_listener(struct js *J, jsval self, jsval *args, int argc)
{
    jsval t = target_of(J, self), f = ARG(1);
    if (t.t != JS_OBJ || !js_is_callable(f))
        return js_undefined;
    struct prop *p = js_own(t.u.o, listener_key(J));
    if (!p) {
        js_put(J, t.u.o, listener_key(J), js_obj(js_new_object(J)), 1);
        p = js_own(t.u.o, listener_key(J));
    }
    struct jsstr *type = js_tostr(J, ARG(0));
    jsval list = js_get_str(J, p->v, type);
    if (!js_is_array(list)) {
        list = js_obj(js_new_array(J));
        jsval key;
        key.t = JS_STR;
        key.u.s = type;
        js_setv(J, p->v, key, list);
    }
    for (int i = 0; i < list.u.o->len; i++)
        if (js_strict_equal(list.u.o->items[i], f))
            return js_undefined;
    js_array_push(J, list.u.o, f);
    /* { once: true } */
    jsval opts = ARG(2);
    if (opts.t == JS_OBJ && js_truthy(js_get(J, opts, "once")))
        js_put(J, f.u.o, S(J, "__once"), js_bool(1), 1);
    return js_undefined;
}

static jsval remove_event_listener(struct js *J, jsval self, jsval *args, int argc)
{
    jsval t = target_of(J, self);
    if (t.t != JS_OBJ)
        return js_undefined;
    struct prop *p = js_own(t.u.o, listener_key(J));
    if (!p)
        return js_undefined;
    jsval list = js_get_str(J, p->v, js_tostr(J, ARG(0)));
    if (!js_is_array(list))
        return js_undefined;
    struct jsobj *a = list.u.o;
    for (int i = 0; i < a->len; i++)
        if (js_strict_equal(a->items[i], ARG(1))) {
            memmove(a->items + i, a->items + i + 1, sizeof(jsval) * (size_t)(a->len - i - 1));
            a->len--;
            break;
        }
    return js_undefined;
}

static jsval event_prevent_default(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    if (self.t == JS_OBJ)
        js_put(J, self.u.o, S(J, "defaultPrevented"), js_bool(1), 0);
    return js_undefined;
}

static jsval event_stop_propagation(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    if (self.t == JS_OBJ)
        js_put(J, self.u.o, S(J, "__stopped"), js_bool(1), 1);
    return js_undefined;
}

static jsval event_stop_immediate(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    if (self.t == JS_OBJ) {
        js_put(J, self.u.o, S(J, "__stopped"), js_bool(1), 1);
        js_put(J, self.u.o, S(J, "__stopnow"), js_bool(1), 1);
    }
    return js_undefined;
}

static jsval event_init(struct js *J, jsval self, jsval *args, int argc)
{
    if (self.t != JS_OBJ)
        return js_undefined;
    js_set(J, self, "type", js_to_string(J, ARG(0)));
    js_set(J, self, "bubbles", js_bool(js_truthy(ARG(1))));
    js_set(J, self, "cancelable", js_bool(js_truthy(ARG(2))));
    return js_undefined;
}

static jsval new_event(struct jsdom *d, const char *type, jsval target)
{
    struct js *J = d->J;
    struct jsobj *e = js_mkobj(J, C_OBJECT, d->event_proto);
    jsval ev = js_obj(e);
    js_set(J, ev, "type", js_str(J, type));
    js_set(J, ev, "target", target);
    js_set(J, ev, "srcElement", target);
    js_set(J, ev, "currentTarget", target);
    js_set(J, ev, "bubbles", js_bool(1));
    js_set(J, ev, "cancelable", js_bool(1));
    js_set(J, ev, "defaultPrevented", js_bool(0));
    js_set(J, ev, "isTrusted", js_bool(1));
    js_set(J, ev, "timeStamp", js_num(uptime_ms()));
    js_set(J, ev, "button", js_num(0));
    js_set(J, ev, "which", js_num(1));
    return ev;
}

/* Run the listeners and on-handler of one object for an event. */
static void fire_on(struct jsdom *d, jsval obj, struct node *n, const char *type, jsval ev)
{
    struct js *J = d->J;
    js_set(J, ev, "currentTarget", obj);
    struct prop *p = js_own(obj.u.o, listener_key(J));
    if (p) {
        jsval list = js_get(J, p->v, type);
        if (js_is_array(list) && list.u.o->len) {
            int count = list.u.o->len;
            jsval *copy = js_alloc(J, sizeof(jsval) * (size_t)count);
            memcpy(copy, list.u.o->items, sizeof(jsval) * (size_t)count);
            for (int i = 0; i < count; i++) {
                jsval r;
                if (js_get(J, js_obj(copy[i].u.o), "__once").t == JS_BOOL) {
                    jsval a[2] = { js_str(J, type), copy[i] };
                    remove_event_listener(J, obj, a, 2);
                }
                if (js_call(J, copy[i], obj, &ev, 1, &r) < 0)
                    report(d, type);
                if (js_truthy(js_get(J, ev, "__stopnow")))
                    return;
            }
        }
    }
    char on[40];
    snprintf(on, sizeof(on), "on%s", type);
    jsval handler = js_get(J, obj, on);
    if (!js_is_callable(handler) && n && n->type == N_ELEMENT) {
        const char *code = node_attr(n, on);
        if (code) {
            char cache[48];
            snprintf(cache, sizeof(cache), "__%s", on);
            handler = js_get(J, obj, cache);
            if (!js_is_callable(handler)) {
                handler = js_compile_handler(J, code, on);
                if (!js_is_callable(handler))
                    report(d, on);
                js_put(J, obj.u.o, S(J, cache), handler, 1);
            }
        }
    }
    if (js_is_callable(handler)) {
        jsval r;
        if (js_call(J, handler, obj, &ev, 1, &r) < 0)
            report(d, on);
        else if (r.t == JS_BOOL && !r.u.b)
            js_set(J, ev, "defaultPrevented", js_bool(1));
    }
}

static int dispatch_event(struct jsdom *d, struct node *target, const char *type, jsval ev, int bubbles)
{
    struct js *J = d->J;
    d->depth++;
    for (struct node *n = target; n; n = bubbles ? n->parent : NULL) {
        int has_attr = n->type == N_ELEMENT && n->attrs;
        if (!n->js && !has_attr)
            continue;
        char on[40];
        snprintf(on, sizeof(on), "on%s", type);
        if (!n->js && !node_attr(n, on))
            continue;
        fire_on(d, wrap(d, n), n, type, ev);
        if (js_truthy(js_get(J, ev, "__stopped")))
            break;
    }
    if (bubbles && !js_truthy(js_get(J, ev, "__stopped"))) {
        fire_on(d, js_obj(d->document), NULL, type, ev);
        if (!js_truthy(js_get(J, ev, "__stopped")))
            fire_on(d, js_obj(d->window), NULL, type, ev);
    }
    d->depth--;
    int prevented = js_truthy(js_get(J, ev, "defaultPrevented"));
    settle(d);
    return prevented;
}

int jsdom_dispatch(struct jsdom *d, struct node *target, const char *type)
{
    if (!d || !target)
        return 0;
    jsval ev = new_event(d, type, wrap(d, target));
    int bubbles = strcmp(type, "focus") && strcmp(type, "blur") && strcmp(type, "load");
    return dispatch_event(d, target, type, ev, bubbles);
}

int jsdom_key_event(struct jsdom *d, struct node *target, const char *type, int keycode, const char *key)
{
    if (!d)
        return 0;
    struct js *J = d->J;
    struct node *t = target ? target : node_find(d->h.root, T_BODY);
    if (!t)
        t = d->h.root;
    jsval ev = new_event(d, type, wrap(d, t));
    js_set(J, ev, "keyCode", js_num(keycode));
    js_set(J, ev, "which", js_num(keycode));
    js_set(J, ev, "charCode", js_num(key && key[0] && !key[1] ? (unsigned char)key[0] : 0));
    js_set(J, ev, "key", js_str(J, key ? key : ""));
    js_set(J, ev, "code", js_str(J, key ? key : ""));
    return dispatch_event(d, t, type, ev, 1);
}

static jsval el_dispatch_event(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d = dom(J);
    jsval ev = ARG(0);
    if (ev.t != JS_OBJ)
        return js_bool(0);
    struct node *n = node_of(self);
    const char *type = js_tostr(J, js_get(J, ev, "type"))->s;
    js_set(J, ev, "target", self);
    if (n) {
        d->depth++;
        int r = !dispatch_event(d, n, type, ev, js_truthy(js_get(J, ev, "bubbles")));
        d->depth--;
        return js_bool(r);
    }
    fire_on(d, target_of(J, self), NULL, type, ev);
    return js_bool(!js_truthy(js_get(J, ev, "defaultPrevented")));
}

static jsval event_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    jsval ev = new_event(d, js_tostr(J, ARG(0))->s, js_null);
    jsval opts = ARG(1);
    js_set(J, ev, "bubbles", js_bool(opts.t == JS_OBJ && js_truthy(js_get(J, opts, "bubbles"))));
    js_set(J, ev, "isTrusted", js_bool(0));
    if (opts.t == JS_OBJ)
        js_set(J, ev, "detail", js_get(J, opts, "detail"));
    return ev;
}

/* ---- document ---- */

static int document_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    (void)o;
    struct jsdom *d = dom(J);
    struct node *root = d->h.root;
#define IS(x) (!strcmp(key, x))
    if (IS("documentElement")) { *out = wrap(d, root); return 1; }
    if (IS("body")) { *out = wrap(d, node_find(root, T_BODY)); return 1; }
    if (IS("head")) { *out = wrap(d, node_find(root, T_HEAD)); return 1; }
    if (IS("nodeType")) { *out = js_num(9); return 1; }
    if (IS("nodeName")) { *out = js_str(J, "#document"); return 1; }
    if (IS("title")) {
        char t[256] = "";
        struct node *tn = node_find(root, T_TITLE);
        if (tn)
            node_text(tn, t, sizeof(t));
        *out = js_str(J, t);
        return 1;
    }
    if (IS("URL") || IS("documentURI")) { *out = js_str(J, d->h.url); return 1; }
    if (IS("location")) { *out = js_obj(d->location); return 1; }
    if (IS("readyState")) { *out = js_str(J, d->loaded ? "complete" : "interactive"); return 1; }
    if (IS("cookie")) { *out = js_str(J, d->cookies); return 1; }
    if (IS("referrer")) { *out = js_str(J, ""); return 1; }
    if (IS("characterSet") || IS("charset")) { *out = js_str(J, "UTF-8"); return 1; }
    if (IS("compatMode")) { *out = js_str(J, root && root->quirks ? "BackCompat" : "CSS1Compat"); return 1; }
    if (IS("visibilityState")) { *out = js_str(J, "visible"); return 1; }
    if (IS("hidden")) { *out = js_bool(0); return 1; }
    if (IS("defaultView")) { *out = js_obj(d->window); return 1; }
    if (IS("activeElement")) { *out = wrap(d, node_find(root, T_BODY)); return 1; }
    if (IS("currentScript")) { *out = wrap(d, d->current_script); return 1; }
    if (IS("forms")) { *out = collect(d, root, test_tag, "form", 0); return 1; }
    if (IS("images")) { *out = collect(d, root, test_tag, "img", 0); return 1; }
    if (IS("scripts")) { *out = collect(d, root, test_tag, "script", 0); return 1; }
    if (IS("links")) { *out = collect(d, root, test_selector, "a[href], area[href]", 0); return 1; }
    if (IS("firstChild") || IS("firstElementChild")) { *out = wrap(d, root); return 1; }
    if (IS("childNodes") || IS("children")) {
        struct jsobj *a = js_new_array(J);
        js_array_push(J, a, wrap(d, root));
        *out = js_obj(a);
        return 1;
    }
    return 0;
#undef IS
}

static int document_set(struct js *J, struct jsobj *o, const char *key, jsval v)
{
    (void)o;
    struct jsdom *d = dom(J);
    if (!strcmp(key, "title")) {
        struct jsstr *s = js_tostr(J, v);
        struct node *tn = node_find(d->h.root, T_TITLE);
        if (!tn) {
            struct node *head = node_find(d->h.root, T_HEAD);
            tn = new_node(d, N_ELEMENT, "title");
            if (head && tn)
                insert_node(d, head, tn, NULL);
        }
        if (tn)
            set_text_content(d, tn, s->s, s->len);
        if (d->h.set_title)
            d->h.set_title(d->h.ctx, s->s);
        return 1;
    }
    if (!strcmp(key, "cookie")) {
        /* name=value (attributes after ; are ignored), replacing the same name */
        struct jsstr *s = js_tostr(J, v);
        char pair[512];
        snprintf(pair, sizeof(pair), "%.*s", (int)strcspn(s->s, ";"), s->s);
        char *eq = strchr(pair, '=');
        if (!eq)
            return 1;
        size_t nl = (size_t)(eq - pair);
        char out[sizeof(d->cookies)] = "";
        for (char *c = d->cookies; *c;) {
            char *end = strstr(c, "; ");
            size_t len = end ? (size_t)(end - c) : strlen(c);
            if (!(len > nl && !strncmp(c, pair, nl) && c[nl] == '=')) {
                if (*out)
                    strncat(out, "; ", sizeof(out) - strlen(out) - 1);
                strncat(out, c, len < sizeof(out) - strlen(out) - 1 ? len : sizeof(out) - strlen(out) - 1);
            }
            c = end ? end + 2 : c + len;
        }
        if (strstr(s->s, "expires=Thu, 01 Jan 1970") == NULL && strstr(s->s, "max-age=0") == NULL) {
            if (*out)
                strncat(out, "; ", sizeof(out) - strlen(out) - 1);
            strncat(out, pair, sizeof(out) - strlen(out) - 1);
        }
        snprintf(d->cookies, sizeof(d->cookies), "%s", out);
        return 1;
    }
    if (!strcmp(key, "location")) {
        struct jsstr *s = js_tostr(J, v);
        char abs[URL_MAX];
        url_resolve(d->h.base, s->s, abs, sizeof(abs));
        d->h.navigate(d->h.ctx, abs, 0);
        return 1;
    }
    return 0;
}

static const struct jsclass document_class = { "HTMLDocument", document_get, document_set };

static jsval doc_get_by_id(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    return wrap(d, by_id(d->h.root, js_tostr(J, ARG(0))->s));
}

static jsval doc_get_by_name(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    return collect(d, d->h.root, test_name, js_tostr(J, ARG(0))->s, 0);
}

static jsval doc_create_element(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *name = js_tostr(J, ARG(0));
    struct node *n = new_node(dom(J), N_ELEMENT, name->s);
    return wrap(dom(J), n);
}

static jsval doc_create_text(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *s = js_tostr(J, ARG(0));
    return wrap(dom(J), new_text(dom(J), s->s, s->len));
}

static jsval doc_create_comment(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    return wrap(dom(J), new_text(dom(J), "", 0));
}

static jsval doc_create_fragment(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    return wrap(dom(J), new_node(dom(J), N_ELEMENT, "#fragment"));
}

/* document.write: while a script runs as the page loads, the HTML goes after it. */
static jsval doc_write_impl(struct js *J, jsval *args, int argc, int newline)
{
    struct jsdom *d = dom(J);
    struct jsstr *s = js_mkstr(J, "", 0);
    for (int i = 0; i < argc; i++)
        s = js_concat(J, s, js_tostr(J, args[i]));
    if (newline)
        s = js_concat(J, s, S(J, "\n"));
    struct node *frag = parse_fragment(d, s->s, s->len);
    if (!frag)
        return js_undefined;
    struct node *after = d->write_after ? d->write_after : d->current_script;
    struct node *last = frag->last;
    if (after && after->parent && !d->loaded) {
        insert_node(d, after->parent, frag, after->next);
        if (last)
            d->write_after = last;
    } else {
        struct node *body = node_find(d->h.root, T_BODY);
        if (body)
            insert_node(d, body, frag, NULL);
    }
    return js_undefined;
}

static jsval doc_write(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return doc_write_impl(J, args, argc, 0);
}

static jsval doc_writeln(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return doc_write_impl(J, args, argc, 1);
}

static jsval js_true(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_bool(1);
}

static jsval js_false(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_bool(0);
}

/* ---- location ---- */

static void url_parts(const char *url, char *proto, char *host, char *port, char *path, char *search,
                      char *hash)
{
    proto[0] = host[0] = port[0] = path[0] = search[0] = hash[0] = 0;
    const char *p = strstr(url, "://");
    if (!p) {
        snprintf(path, URL_MAX, "%s", url);
        return;
    }
    snprintf(proto, 16, "%.*s:", (int)(p - url), url);
    p += 3;
    const char *h = p;
    while (*p && *p != '/' && *p != '?' && *p != '#')
        p++;
    snprintf(host, 256, "%.*s", (int)(p - h), h);
    char *colon = strchr(host, ':');
    if (colon) {
        snprintf(port, 8, "%s", colon + 1);
        *colon = 0;
    }
    const char *q = p;
    while (*q && *q != '?' && *q != '#')
        q++;
    snprintf(path, URL_MAX, "%.*s", (int)(q - p), *p == '/' ? p : "/");
    if (q == p)
        snprintf(path, URL_MAX, "/");
    const char *hs = strchr(q, '#');
    if (*q == '?')
        snprintf(search, URL_MAX, "%.*s", hs ? (int)(hs - q) : (int)strlen(q), q);
    if (hs)
        snprintf(hash, URL_MAX, "%s", hs);
}

static int location_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    (void)o;
    struct jsdom *d = dom(J);
    static char proto[16], host[256], port[8], path[URL_MAX], search[URL_MAX], hash[URL_MAX];
    url_parts(d->h.url, proto, host, port, path, search, hash);
    char b[URL_MAX + 300];
#define IS(x) (!strcmp(key, x))
    if (IS("href")) { *out = js_str(J, d->h.url); return 1; }
    if (IS("protocol")) { *out = js_str(J, proto); return 1; }
    if (IS("hostname")) { *out = js_str(J, host); return 1; }
    if (IS("port")) { *out = js_str(J, port); return 1; }
    if (IS("host")) {
        snprintf(b, sizeof(b), "%s%s%s", host, port[0] ? ":" : "", port);
        *out = js_str(J, b);
        return 1;
    }
    if (IS("origin")) {
        snprintf(b, sizeof(b), "%s//%s%s%s", proto, host, port[0] ? ":" : "", port);
        *out = js_str(J, b);
        return 1;
    }
    if (IS("pathname")) { *out = js_str(J, path); return 1; }
    if (IS("search")) { *out = js_str(J, search); return 1; }
    if (IS("hash")) { *out = js_str(J, hash); return 1; }
    return 0;
#undef IS
}

static int location_set(struct js *J, struct jsobj *o, const char *key, jsval v)
{
    (void)o;
    struct jsdom *d = dom(J);
    struct jsstr *s = js_tostr(J, v);
    char abs[URL_MAX];
    if (!strcmp(key, "href")) {
        url_resolve(d->h.base, s->s, abs, sizeof(abs));
    } else if (!strcmp(key, "hash")) {
        snprintf(abs, sizeof(abs), "%s", d->h.url);
        url_strip_fragment(abs);
        size_t l = strlen(abs);
        snprintf(abs + l, sizeof(abs) - l, "%s%s", s->s[0] == '#' ? "" : "#", s->s);
    } else if (!strcmp(key, "search")) {
        snprintf(abs, sizeof(abs), "%s", d->h.url);
        char *q = strpbrk(abs, "?#");
        if (q)
            *q = 0;
        size_t l = strlen(abs);
        snprintf(abs + l, sizeof(abs) - l, "%s%s", s->s[0] == '?' ? "" : "?", s->s);
    } else {
        return 0;
    }
    d->h.navigate(d->h.ctx, abs, 0);
    return 1;
}

static const struct jsclass location_class = { "Location", location_get, location_set };

static jsval location_assign(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    char abs[URL_MAX];
    url_resolve(d->h.base, js_tostr(J, ARG(0))->s, abs, sizeof(abs));
    d->h.navigate(d->h.ctx, abs, 0);
    return js_undefined;
}

static jsval location_replace(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    char abs[URL_MAX];
    url_resolve(d->h.base, js_tostr(J, ARG(0))->s, abs, sizeof(abs));
    d->h.navigate(d->h.ctx, abs, 1);
    return js_undefined;
}

static jsval location_reload(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    dom(J)->h.reload(dom(J)->h.ctx);
    return js_undefined;
}

static jsval location_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    return js_str(J, dom(J)->h.url);
}

/* ---- window ---- */

static jsval win_alert(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    if (d->h.alert)
        d->h.alert(d->h.ctx, argc ? js_text(J, args[0]) : "");
    return js_undefined;
}

static jsval win_confirm(struct js *J, jsval self, jsval *args, int argc)
{
    win_alert(J, self, args, argc);
    return js_bool(1);
}

static jsval win_prompt(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_null;
}

static int add_timer(struct jsdom *d, jsval f, int ms, int repeat, jsval *extra, int nextra)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        struct timer *t = &d->timers[i];
        if (t->active)
            continue;
        if (ms < 0)
            ms = 0;
        if (repeat && ms < 10)
            ms = 10;
        t->active = 1;
        t->id = ++d->next_timer_id;
        t->due = uptime_ms() + (unsigned)ms;
        t->interval = repeat ? ms : 0;
        t->fn = f;
        t->nargs = nextra < 4 ? nextra : 4;
        for (int k = 0; k < t->nargs; k++)
            t->args[k] = extra[k];
        return t->id;
    }
    return 0;
}

static jsval set_timer(struct js *J, jsval *args, int argc, int repeat)
{
    jsval f = ARG(0);
    if (!js_is_callable(f) && f.t != JS_STR)
        f = js_to_string(J, f);
    double ms = js_to_number(J, ARG(1));
    int id = add_timer(dom(J), f, ms != ms ? 0 : ms > 1e8 ? 100000000 : (int)ms, repeat,
                       argc > 2 ? args + 2 : NULL, argc > 2 ? argc - 2 : 0);
    return js_num(id);
}

static jsval win_set_timeout(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return set_timer(J, args, argc, 0);
}

static jsval win_set_interval(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return set_timer(J, args, argc, 1);
}

static jsval win_request_frame(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    jsval now = js_num(uptime_ms());
    return js_num(add_timer(dom(J), ARG(0), 33, 0, &now, 1));
}

static jsval win_clear_timer(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    int id = (int)js_to_number(J, ARG(0));
    for (int i = 0; i < MAX_TIMERS; i++)
        if (d->timers[i].active && d->timers[i].id == id)
            d->timers[i].active = 0;
    return js_undefined;
}

static jsval win_scroll_to(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    double x, y;
    if (ARG(0).t == JS_OBJ) {
        x = js_to_number(J, js_get(J, args[0], "left"));
        y = js_to_number(J, js_get(J, args[0], "top"));
    } else {
        x = js_to_number(J, ARG(0));
        y = js_to_number(J, ARG(1));
    }
    if (d->h.scroll)
        d->h.scroll(d->h.ctx, x == x ? (int)x : 0, y == y ? (int)y : 0, 0);
    return js_undefined;
}

static jsval win_scroll_by(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    double x = js_to_number(J, ARG(0)), y = js_to_number(J, ARG(1));
    if (ARG(0).t == JS_OBJ) {
        x = js_to_number(J, js_get(J, args[0], "left"));
        y = js_to_number(J, js_get(J, args[0], "top"));
    }
    if (d->h.scroll)
        d->h.scroll(d->h.ctx, x == x ? (int)x : 0, y == y ? (int)y : 0, 1);
    return js_undefined;
}

static jsval win_get_computed_style(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct node *n = node_of(ARG(0));
    if (!n)
        return js_throw_error(J, "TypeError: getComputedStyle needs an element");
    struct jsobj *o = js_new_host(J, &computed_class, n);
    o->proto = dom(J)->style_proto;
    return js_obj(o);
}

static jsval win_match_media(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsobj *m = js_new_object(J);
    jsval mv = js_obj(m);
    js_set(J, mv, "matches", js_bool(0));
    js_set(J, mv, "media", js_to_string(J, ARG(0)));
    fn(J, m, "addListener", nothing, 1);
    fn(J, m, "removeListener", nothing, 1);
    fn(J, m, "addEventListener", nothing, 2);
    fn(J, m, "removeEventListener", nothing, 2);
    return mv;
}

static jsval win_open(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    if (argc && ARG(0).t == JS_STR && ARG(0).u.s->len) {
        char abs[URL_MAX];
        url_resolve(d->h.base, args[0].u.s->s, abs, sizeof(abs));
        d->h.navigate(d->h.ctx, abs, 0);
    }
    return js_null;
}

static jsval history_back(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    dom(J)->h.history(dom(J)->h.ctx, -1);
    return js_undefined;
}

static jsval history_forward(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    dom(J)->h.history(dom(J)->h.ctx, 1);
    return js_undefined;
}

static jsval history_go(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    int n = (int)js_to_number(J, ARG(0));
    if (n)
        dom(J)->h.history(dom(J)->h.ctx, n);
    else
        dom(J)->h.reload(dom(J)->h.ctx);
    return js_undefined;
}

static jsval storage_get(struct js *J, jsval self, jsval *args, int argc)
{
    jsval data = js_get(J, self, "__data");
    jsval v = js_get_str(J, data, js_tostr(J, ARG(0)));
    return v.t == JS_UNDEF ? js_null : v;
}

static jsval storage_set(struct js *J, jsval self, jsval *args, int argc)
{
    jsval data = js_get(J, self, "__data");
    js_setv(J, data, js_to_string(J, ARG(0)), js_to_string(J, ARG(1)));
    return js_undefined;
}

static jsval storage_remove(struct js *J, jsval self, jsval *args, int argc)
{
    jsval data = js_get(J, self, "__data");
    js_delete(J, data, js_tostr(J, ARG(0)));
    return js_undefined;
}

static jsval storage_clear(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    js_put(J, self.u.o, S(J, "__data"), js_obj(js_new_object(J)), 1);
    return js_undefined;
}

static jsval storage_key(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr **keys;
    int n = js_keys(J, js_get(J, self, "__data"), 0, &keys);
    int i = (int)js_to_number(J, ARG(0));
    return i >= 0 && i < n ? js_str(J, keys[i]->s) : js_null;
}

static jsval make_storage(struct jsdom *d)
{
    struct js *J = d->J;
    struct jsobj *s = js_new_object(J);
    js_put(J, s, S(J, "__data"), js_obj(js_new_object(J)), 1);
    fn(J, s, "getItem", storage_get, 1);
    fn(J, s, "setItem", storage_set, 2);
    fn(J, s, "removeItem", storage_remove, 1);
    fn(J, s, "clear", storage_clear, 0);
    fn(J, s, "key", storage_key, 1);
    return js_obj(s);
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static jsval win_btoa(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *s = js_tostr(J, ARG(0));
    char *out = js_alloc(J, (size_t)(s->len + 2) / 3 * 4 + 1);
    int n = 0;
    for (int i = 0; i < s->len; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)s->s[i] << 16;
        if (i + 1 < s->len)
            v |= (uint32_t)(unsigned char)s->s[i + 1] << 8;
        if (i + 2 < s->len)
            v |= (unsigned char)s->s[i + 2];
        out[n++] = b64[v >> 18];
        out[n++] = b64[(v >> 12) & 63];
        out[n++] = i + 1 < s->len ? b64[(v >> 6) & 63] : '=';
        out[n++] = i + 2 < s->len ? b64[v & 63] : '=';
    }
    return js_strn(J, out, n);
}

static jsval win_atob(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsstr *s = js_tostr(J, ARG(0));
    char *out = js_alloc(J, (size_t)s->len + 1);
    int n = 0, bits = 0;
    uint32_t acc = 0;
    for (int i = 0; i < s->len; i++) {
        const char *p = strchr(b64, s->s[i]);
        if (!p || !s->s[i])
            continue;
        acc = acc << 6 | (uint32_t)(p - b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (char)(acc >> bits);
        }
    }
    return js_strn(J, out, n);
}

static jsval perf_now(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_num(uptime_ms());
}

static jsval image_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    struct node *n = new_node(d, N_ELEMENT, "img");
    if (n && argc > 0)
        set_attr(d, n, "width", js_tostr(J, args[0])->s);
    if (n && argc > 1)
        set_attr(d, n, "height", js_tostr(J, args[1])->s);
    return wrap(d, n);
}

static jsval win_get(struct js *J, jsval self, jsval *args, int argc, int which)
{
    (void)self;
    (void)args;
    (void)argc;
    struct jsdom *d = dom(J);
    int a = 0, b = 0;
    if (which < 2 && d->h.scroll_pos)
        d->h.scroll_pos(d->h.ctx, &a, &b);
    else if (d->h.viewport)
        d->h.viewport(d->h.ctx, &a, &b);
    return js_num(which % 2 ? b : a);
}

static jsval win_scroll_x(struct js *J, jsval s, jsval *a, int n) { return win_get(J, s, a, n, 0); }
static jsval win_scroll_y(struct js *J, jsval s, jsval *a, int n) { return win_get(J, s, a, n, 1); }
static jsval win_inner_w(struct js *J, jsval s, jsval *a, int n) { return win_get(J, s, a, n, 2); }
static jsval win_inner_h(struct js *J, jsval s, jsval *a, int n) { return win_get(J, s, a, n, 3); }

static jsval win_get_location(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    return js_obj(dom(J)->location);
}

static jsval win_set_location(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    location_set(J, dom(J)->location, "href", ARG(0));
    return js_undefined;
}

static void accessor(struct js *J, struct jsobj *o, const char *name, js_native get, js_native set)
{
    js_put(J, o, S(J, name), js_undefined, 1);
    struct prop *p = js_own(o, S(J, name));
    if (get)
        p->getter = js_new_function(J, get, name, 0).u.o;
    if (set)
        p->setter = js_new_function(J, set, name, 1).u.o;
}

/* ---- performance ---- */

static jsval perf_empty(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    return js_obj(js_new_array(J));
}

static jsval nav_send_beacon(struct js *J, jsval self, jsval *args, int argc)
{
    (void)J;
    (void)self;
    (void)args;
    (void)argc;
    return js_bool(1);                  /* analytics: dropped */
}

/* ---- XMLHttpRequest ---- */

static void xhr_state(struct jsdom *d, jsval x, int state)
{
    js_set(d->J, x, "readyState", js_num(state));
    jsval ev = new_event(d, "readystatechange", x);
    fire_on(d, x, NULL, "readystatechange", ev);
}

static jsval xhr_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    struct jsdom *d = dom(J);
    struct jsobj *x = js_mkobj(J, C_OBJECT, d->xhr_proto);
    jsval xv = js_obj(x);
    js_set(J, xv, "readyState", js_num(0));
    js_set(J, xv, "status", js_num(0));
    js_set(J, xv, "statusText", js_str(J, ""));
    js_set(J, xv, "responseText", js_str(J, ""));
    js_set(J, xv, "response", js_str(J, ""));
    js_set(J, xv, "responseType", js_str(J, ""));
    js_set(J, xv, "responseURL", js_str(J, ""));
    js_set(J, xv, "timeout", js_num(0));
    js_set(J, xv, "withCredentials", js_bool(0));
    return xv;
}

static jsval xhr_open(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d = dom(J);
    if (self.t != JS_OBJ)
        return js_undefined;
    char abs[URL_MAX];
    url_resolve(d->h.base, js_tostr(J, ARG(1))->s, abs, sizeof(abs));
    js_put(J, self.u.o, S(J, "__method"), js_to_string(J, ARG(0)), 1);
    js_put(J, self.u.o, S(J, "__url"), js_str(J, abs), 1);
    js_put(J, self.u.o, S(J, "__aborted"), js_bool(0), 1);
    xhr_state(d, self, 1);
    return js_undefined;
}

static jsval xhr_send(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d = dom(J);
    if (self.t != JS_OBJ)
        return js_undefined;
    int id = -1;
    for (int i = 0; i < MAX_XHR && id < 0; i++)
        if (!d->xhr[i])
            id = i;
    const char *url = js_cstr(js_get(J, self, "__url"));
    if (id < 0 || !d->h.http || (strncmp(url, "http", 4) && strncmp(url, "file:", 5))) {
        /* cannot go: a network error at once */
        js_put(J, self.u.o, S(J, "__failed"), js_bool(1), 1);
        jsval ev = new_event(d, "error", self);
        fire_on(d, self, NULL, "error", ev);
        return js_undefined;
    }
    d->xhr[id] = self.u.o;
    jsval body = ARG(0);
    const char *method = js_cstr(js_get(J, self, "__method"));
    d->h.http(d->h.ctx, id, method, url,
              body.t == JS_UNDEF || body.t == JS_NULL ? NULL : js_tostr(J, body)->s);
    return js_undefined;
}

static jsval xhr_abort(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct jsdom *d = dom(J);
    for (int i = 0; i < MAX_XHR; i++)
        if (self.t == JS_OBJ && d->xhr[i] == self.u.o)
            d->xhr[i] = NULL;
    if (self.t == JS_OBJ)
        js_set(J, self, "readyState", js_num(0));
    return js_undefined;
}

static jsval xhr_get_header(struct js *J, jsval self, jsval *args, int argc)
{
    const char *h = js_cstr(js_get(J, self, "__headers"));
    struct jsstr *name = js_tostr(J, ARG(0));
    for (const char *p = h; *p;) {
        const char *eol = strchr(p, '\n');
        const char *end = eol ? eol : p + strlen(p);
        if (end - p > name->len && !strncasecmp(p, name->s, (size_t)name->len) && p[name->len] == ':') {
            const char *v = p + name->len + 1;
            while (*v == ' ')
                v++;
            const char *ve = end;
            while (ve > v && (ve[-1] == '\r' || ve[-1] == ' '))
                ve--;
            return js_strn(J, v, (int)(ve - v));
        }
        p = eol ? eol + 1 : end;
    }
    return js_null;
}

static jsval xhr_all_headers(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    jsval h = js_get(J, self, "__headers");
    return h.t == JS_STR ? h : js_str(J, "");
}

void jsdom_http_done(struct jsdom *d, int id, int status, const char *headers, int hlen,
                     const char *body, int blen)
{
    if (!d || id < 0 || id >= MAX_XHR || !d->xhr[id])
        return;
    struct js *J = d->J;
    jsval x = js_obj(d->xhr[id]);
    d->xhr[id] = NULL;
    d->depth++;
    /* the header block without its status line */
    const char *h = headers, *hend = headers + hlen;
    const char *nl = h ? memchr(h, '\n', (size_t)hlen) : NULL;
    const char *status_text = "";
    char reason[64] = "";
    if (nl) {
        const char *sp = memchr(h, ' ', (size_t)(nl - h));
        if (sp && (sp = memchr(sp + 1, ' ', (size_t)(nl - sp - 1))))
            snprintf(reason, sizeof(reason), "%.*s", (int)(nl - sp - 1), sp + 1);
        h = nl + 1;
    }
    int rl = (int)strlen(reason);
    while (rl && (reason[rl - 1] == '\r' || reason[rl - 1] == ' '))
        reason[--rl] = 0;
    status_text = reason;
    js_put(J, x.u.o, S(J, "__headers"),
           h && hend > h ? js_strn(J, h, (int)(hend - h)) : js_str(J, ""), 1);
    js_set(J, x, "status", js_num(status));
    js_set(J, x, "statusText", js_str(J, status_text));
    js_set(J, x, "responseURL", js_get(J, x, "__url"));
    jsval text = js_strn(J, body ? body : "", body ? blen : 0);
    js_set(J, x, "responseText", text);
    jsval response = text;
    if (!strcmp(js_cstr(js_get(J, x, "responseType")), "json")) {
        jsval json = js_get(J, js_obj(J->global), "JSON");
        jsval parse = js_get(J, json, "parse");
        if (js_call(J, parse, json, &text, 1, &response) < 0)
            response = js_null;
    }
    js_set(J, x, "response", response);
    xhr_state(d, x, 2);
    xhr_state(d, x, 3);
    xhr_state(d, x, 4);
    const char *kind = status ? "load" : "error";
    fire_on(d, x, NULL, kind, new_event(d, kind, x));
    fire_on(d, x, NULL, "loadend", new_event(d, "loadend", x));
    d->depth--;
    settle(d);
}

/* ---- setting up ---- */

struct jsdom *jsdom_new(const struct jshost *host)
{
    struct jsdom *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->h = *host;
    d->J = js_new(d);
    if (!d->J) {
        free(d);
        return NULL;
    }
    struct js *J = d->J;
    struct jsobj *w = d->window = js_global(J);
    jsval wv = js_obj(w);

    /* elements */
    struct jsobj *ep = d->element_proto = js_new_object(J);
    static const struct { const char *name; js_native fn; int n; } element_methods[] = {
        { "appendChild", el_append_child, 1 }, { "insertBefore", el_insert_before, 2 },
        { "removeChild", el_remove_child, 1 }, { "replaceChild", el_replace_child, 2 },
        { "remove", el_remove, 0 }, { "append", el_append, 1 }, { "prepend", el_prepend, 1 },
        { "before", el_before, 1 }, { "after", el_after, 1 }, { "replaceWith", el_replace_with, 1 },
        { "cloneNode", el_clone_node, 1 }, { "contains", el_contains, 1 },
        { "hasChildNodes", el_has_child_nodes, 0 }, { "getAttribute", el_get_attribute, 1 },
        { "setAttribute", el_set_attribute, 2 }, { "removeAttribute", el_remove_attribute, 1 },
        { "hasAttribute", el_has_attribute, 1 }, { "toggleAttribute", el_toggle_attribute, 1 },
        { "querySelector", el_query_selector, 1 }, { "querySelectorAll", el_query_selector_all, 1 },
        { "getElementsByTagName", el_get_by_tag, 1 }, { "getElementsByClassName", el_get_by_class, 1 },
        { "matches", el_matches, 1 }, { "closest", el_closest, 1 },
        { "getBoundingClientRect", el_get_bounding_rect, 0 },
        { "insertAdjacentHTML", el_insert_adjacent_html, 2 }, { "addEventListener", add_event_listener, 2 },
        { "removeEventListener", remove_event_listener, 2 }, { "dispatchEvent", el_dispatch_event, 1 },
        { "click", el_click, 0 }, { "focus", el_focus, 0 }, { "blur", nothing, 0 },
        { "select", nothing, 0 }, { "scrollIntoView", el_scroll_into_view, 0 },
        { "toString", node_to_string, 0 }, { "setSelectionRange", nothing, 2 },
        { "submit", nothing, 0 }, { "reset", nothing, 0 },
    };
    for (unsigned i = 0; i < sizeof(element_methods) / sizeof(element_methods[0]); i++)
        fn(J, ep, element_methods[i].name, element_methods[i].fn, element_methods[i].n);

    /* style, classList */
    struct jsobj *stp = js_new_object(J);
    fn(J, stp, "setProperty", style_set_property, 2);
    fn(J, stp, "getPropertyValue", style_get_property, 1);
    fn(J, stp, "removeProperty", style_remove_property, 1);
    struct jsobj *clp = js_new_object(J);
    fn(J, clp, "add", classlist_add, 1);
    fn(J, clp, "remove", classlist_remove, 1);
    fn(J, clp, "toggle", classlist_toggle, 1);
    fn(J, clp, "contains", classlist_contains, 1);
    fn(J, clp, "replace", classlist_replace, 2);
    fn(J, clp, "item", classlist_item, 1);
    d->style_proto = stp;
    d->classlist_proto = clp;

    /* events */
    struct jsobj *evp = d->event_proto = js_new_object(J);
    fn(J, evp, "preventDefault", event_prevent_default, 0);
    fn(J, evp, "stopPropagation", event_stop_propagation, 0);
    fn(J, evp, "stopImmediatePropagation", event_stop_immediate, 0);
    fn(J, evp, "initEvent", event_init, 3);
    jsval ev_ctor = js_new_function(J, event_ctor, "Event", 2);
    ev_ctor.u.o->ctor = 1;
    js_set(J, ev_ctor, "prototype", js_obj(evp));
    js_set(J, wv, "Event", ev_ctor);
    js_set(J, wv, "CustomEvent", ev_ctor);

    /* document */
    struct jsobj *dp = d->document_proto = js_new_object(J);
    static const struct { const char *name; js_native fn; int n; } document_methods[] = {
        { "getElementById", doc_get_by_id, 1 }, { "getElementsByTagName", el_get_by_tag, 1 },
        { "getElementsByClassName", el_get_by_class, 1 }, { "getElementsByName", doc_get_by_name, 1 },
        { "querySelector", el_query_selector, 1 }, { "querySelectorAll", el_query_selector_all, 1 },
        { "createElement", doc_create_element, 1 }, { "createElementNS", doc_create_element, 2 },
        { "createTextNode", doc_create_text, 1 }, { "createComment", doc_create_comment, 1 },
        { "createDocumentFragment", doc_create_fragment, 0 }, { "write", doc_write, 1 },
        { "writeln", doc_writeln, 1 }, { "open", nothing, 0 }, { "close", nothing, 0 },
        { "addEventListener", add_event_listener, 2 }, { "removeEventListener", remove_event_listener, 2 },
        { "dispatchEvent", el_dispatch_event, 1 }, { "hasFocus", js_true, 0 },
        { "execCommand", js_false, 1 }, { "createEvent", event_ctor, 1 },
    };
    for (unsigned i = 0; i < sizeof(document_methods) / sizeof(document_methods[0]); i++)
        fn(J, dp, document_methods[i].name, document_methods[i].fn, document_methods[i].n);
    d->document = js_new_host(J, &document_class, d);
    d->document->proto = dp;
    js_put(J, w, S(J, "document"), js_obj(d->document), 0);

    /* location, navigator, screen, history, storage */
    struct jsobj *loc = d->location = js_new_host(J, &location_class, d);
    fn(J, loc, "assign", location_assign, 1);
    fn(J, loc, "replace", location_replace, 1);
    fn(J, loc, "reload", location_reload, 0);
    fn(J, loc, "toString", location_to_string, 0);
    accessor(J, w, "location", win_get_location, win_set_location);

    struct jsobj *nav = js_new_object(J);
    jsval nv = js_obj(nav);
    js_set(J, nv, "userAgent", js_str(J, "Mozilla/5.0 (fluxOS; i686) FluxBrowser/1.0"));
    js_set(J, nv, "appName", js_str(J, "Netscape"));
    js_set(J, nv, "appVersion", js_str(J, "5.0 (fluxOS)"));
    js_set(J, nv, "platform", js_str(J, "fluxOS i686"));
    js_set(J, nv, "language", js_str(J, "en-US"));
    struct jsobj *langs = js_new_array(J);
    js_array_push(J, langs, js_str(J, "en-US"));
    js_set(J, nv, "languages", js_obj(langs));
    js_set(J, nv, "cookieEnabled", js_bool(1));
    js_set(J, nv, "onLine", js_bool(1));
    js_set(J, nv, "vendor", js_str(J, ""));
    fn(J, nav, "sendBeacon", nav_send_beacon, 2);
    js_set(J, wv, "navigator", nv);

    struct jsobj *scr = js_new_object(J);
    int vw = 800, vh = 600;
    if (d->h.viewport)
        d->h.viewport(d->h.ctx, &vw, &vh);
    js_set(J, js_obj(scr), "width", js_num(1024));
    js_set(J, js_obj(scr), "height", js_num(768));
    js_set(J, js_obj(scr), "availWidth", js_num(1024));
    js_set(J, js_obj(scr), "availHeight", js_num(740));
    js_set(J, js_obj(scr), "colorDepth", js_num(24));
    js_set(J, wv, "screen", js_obj(scr));

    struct jsobj *hist = js_new_object(J);
    fn(J, hist, "back", history_back, 0);
    fn(J, hist, "forward", history_forward, 0);
    fn(J, hist, "go", history_go, 1);
    fn(J, hist, "pushState", nothing, 3);
    fn(J, hist, "replaceState", nothing, 3);
    js_set(J, js_obj(hist), "length", js_num(1));
    js_set(J, wv, "history", js_obj(hist));
    js_set(J, wv, "localStorage", make_storage(d));
    js_set(J, wv, "sessionStorage", make_storage(d));

    /* performance: navigation timing, all at the moment the scripts started */
    d->started = uptime_ms();
    double t0 = 0;
    jsval nowv;
    if (js_call(J, js_get(J, js_get(J, wv, "Date"), "now"), js_undefined, NULL, 0, &nowv) == 0)
        t0 = js_to_number(J, nowv);
    struct jsobj *perf = js_new_object(J);
    jsval pv = js_obj(perf);
    fn(J, perf, "now", perf_now, 0);
    fn(J, perf, "mark", nothing, 1);
    fn(J, perf, "measure", nothing, 3);
    fn(J, perf, "clearMarks", nothing, 1);
    fn(J, perf, "clearMeasures", nothing, 1);
    fn(J, perf, "clearResourceTimings", nothing, 0);
    fn(J, perf, "setResourceTimingBufferSize", nothing, 1);
    fn(J, perf, "getEntries", perf_empty, 0);
    fn(J, perf, "getEntriesByType", perf_empty, 1);
    fn(J, perf, "getEntriesByName", perf_empty, 2);
    js_set(J, pv, "timeOrigin", js_num(t0));
    struct jsobj *timing = js_new_object(J);
    static const char *const marks[] = {
        "navigationStart", "unloadEventStart", "unloadEventEnd", "redirectStart", "redirectEnd",
        "fetchStart", "domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd",
        "secureConnectionStart", "requestStart", "responseStart", "responseEnd", "domLoading",
        "domInteractive", "domContentLoadedEventStart", "domContentLoadedEventEnd", "domComplete",
        "loadEventStart", "loadEventEnd",
    };
    for (unsigned i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
        js_set(J, js_obj(timing), marks[i], js_num(t0));
    js_set(J, pv, "timing", js_obj(timing));
    struct jsobj *pnav = js_new_object(J);
    js_set(J, js_obj(pnav), "type", js_num(0));
    js_set(J, js_obj(pnav), "redirectCount", js_num(0));
    js_set(J, pv, "navigation", js_obj(pnav));
    js_set(J, wv, "performance", pv);

    /* XMLHttpRequest */
    struct jsobj *xp = d->xhr_proto = js_new_object(J);
    fn(J, xp, "open", xhr_open, 2);
    fn(J, xp, "send", xhr_send, 1);
    fn(J, xp, "abort", xhr_abort, 0);
    fn(J, xp, "setRequestHeader", nothing, 2);
    fn(J, xp, "overrideMimeType", nothing, 1);
    fn(J, xp, "getResponseHeader", xhr_get_header, 1);
    fn(J, xp, "getAllResponseHeaders", xhr_all_headers, 0);
    fn(J, xp, "addEventListener", add_event_listener, 2);
    fn(J, xp, "removeEventListener", remove_event_listener, 2);
    jsval xc = js_new_function(J, xhr_ctor, "XMLHttpRequest", 0);
    xc.u.o->ctor = 1;
    js_set(J, xc, "prototype", js_obj(xp));
    static const char *const states[] = { "UNSENT", "OPENED", "HEADERS_RECEIVED", "LOADING", "DONE" };
    for (int i = 0; i < 5; i++) {
        js_set(J, xc, states[i], js_num(i));
        js_set(J, js_obj(xp), states[i], js_num(i));
    }
    js_set(J, wv, "XMLHttpRequest", xc);

    /* the window itself */
    js_set(J, wv, "window", wv);
    js_set(J, wv, "self", wv);
    js_set(J, wv, "top", wv);
    js_set(J, wv, "parent", wv);
    js_set(J, wv, "frames", wv);
    js_set(J, wv, "devicePixelRatio", js_num(1));
    js_set(J, wv, "name", js_str(J, ""));
    accessor(J, w, "scrollX", win_scroll_x, NULL);
    accessor(J, w, "pageXOffset", win_scroll_x, NULL);
    accessor(J, w, "scrollY", win_scroll_y, NULL);
    accessor(J, w, "pageYOffset", win_scroll_y, NULL);
    accessor(J, w, "innerWidth", win_inner_w, NULL);
    accessor(J, w, "outerWidth", win_inner_w, NULL);
    accessor(J, w, "innerHeight", win_inner_h, NULL);
    accessor(J, w, "outerHeight", win_inner_h, NULL);
    static const struct { const char *name; js_native fn; int n; } window_functions[] = {
        { "alert", win_alert, 1 }, { "confirm", win_confirm, 1 }, { "prompt", win_prompt, 2 },
        { "setTimeout", win_set_timeout, 2 }, { "setInterval", win_set_interval, 2 },
        { "clearTimeout", win_clear_timer, 1 }, { "clearInterval", win_clear_timer, 1 },
        { "requestAnimationFrame", win_request_frame, 1 }, { "cancelAnimationFrame", win_clear_timer, 1 },
        { "scrollTo", win_scroll_to, 2 }, { "scroll", win_scroll_to, 2 }, { "scrollBy", win_scroll_by, 2 },
        { "getComputedStyle", win_get_computed_style, 1 }, { "matchMedia", win_match_media, 1 },
        { "open", win_open, 2 }, { "close", nothing, 0 }, { "focus", nothing, 0 }, { "blur", nothing, 0 },
        { "print", nothing, 0 }, { "stop", nothing, 0 }, { "btoa", win_btoa, 1 }, { "atob", win_atob, 1 },
        { "addEventListener", add_event_listener, 2 }, { "removeEventListener", remove_event_listener, 2 },
        { "dispatchEvent", el_dispatch_event, 1 },
    };
    for (unsigned i = 0; i < sizeof(window_functions) / sizeof(window_functions[0]); i++)
        fn(J, w, window_functions[i].name, window_functions[i].fn, window_functions[i].n);
    jsval img = js_new_function(J, image_ctor, "Image", 2);
    img.u.o->ctor = 1;
    js_set(J, wv, "Image", img);
    /* the constructors scripts test with instanceof or typeof */
    jsval node_ctor = js_new_function(J, nothing, "HTMLElement", 0);
    js_set(J, node_ctor, "prototype", js_obj(ep));
    js_set(J, wv, "HTMLElement", node_ctor);
    js_set(J, wv, "Element", node_ctor);
    js_set(J, wv, "Node", node_ctor);
    js_set(J, node_ctor, "ELEMENT_NODE", js_num(1));
    js_set(J, node_ctor, "TEXT_NODE", js_num(3));
    return d;
}

void jsdom_free(struct jsdom *d)
{
    if (!d)
        return;
    js_free(d->J);
    free(d);
}

void jsdom_run_script(struct jsdom *d, struct node *script, const char *text, int len, const char *where)
{
    if (!d)
        return;
    struct node *saved = d->current_script, *saved_after = d->write_after;
    d->current_script = script;
    d->write_after = NULL;
    d->depth++;
    if (js_eval(d->J, text, len, where) < 0)
        report(d, where);
    d->depth--;
    d->current_script = saved;
    d->write_after = saved_after;
    settle(d);
}

void jsdom_loaded(struct jsdom *d)
{
    if (!d)
        return;
    d->loaded = 1;
    struct node *target = node_find(d->h.root, T_BODY);
    jsval ev = new_event(d, "DOMContentLoaded", js_obj(d->document));
    d->depth++;
    fire_on(d, js_obj(d->document), NULL, "DOMContentLoaded", ev);
    fire_on(d, js_obj(d->window), NULL, "DOMContentLoaded", ev);
    jsval load = new_event(d, "load", js_obj(d->document));
    fire_on(d, js_obj(d->window), target, "load", load);
    d->depth--;
    settle(d);
}

void jsdom_run_timers(struct jsdom *d)
{
    if (!d)
        return;
    unsigned int now = uptime_ms();
    int ran = 0;
    for (int i = 0; i < MAX_TIMERS && ran < 32; i++) {
        struct timer *t = &d->timers[i];
        if (!t->active || (int)(now - t->due) < 0)
            continue;
        ran++;
        jsval f = t->fn;
        jsval args[4];
        int nargs = t->nargs;
        memcpy(args, t->args, sizeof(args));
        if (t->interval)
            t->due = now + (unsigned)t->interval;
        else
            t->active = 0;
        d->depth++;
        if (f.t == JS_STR) {
            if (js_eval(d->J, f.u.s->s, f.u.s->len, "timer") < 0)
                report(d, "timer");
        } else {
            jsval r;
            if (js_call(d->J, f, js_obj(d->window), args, nargs, &r) < 0)
                report(d, "timer");
        }
        d->depth--;
    }
    settle(d);
}

int jsdom_next_timer(struct jsdom *d)
{
    if (!d)
        return -1;
    unsigned int now = uptime_ms();
    int best = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        struct timer *t = &d->timers[i];
        if (!t->active)
            continue;
        int wait = (int)(t->due - now);
        if (wait < 0)
            wait = 0;
        if (best < 0 || wait < best)
            best = wait;
    }
    return best;
}

int jsdom_has_handlers(struct jsdom *d)
{
    return d != NULL;
}
