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
 * (pointer and mouse, click, input, change, submit, keys, focus, load,
 * scroll, resize) go to listeners added with addEventListener, on*
 * properties and on* attributes: down from the window to the target in
 * the capture phase, then back up. MutationObservers hear of changes as
 * they happen; Intersection and Resize observers look after each layout
 * and scroll. Module scripts go through the engine's module loader, which
 * fetches with the browser and resolves through the page's import map.
 *
 * XMLHttpRequest goes through the browser's own fetching (wget), always
 * asynchronously; performance.timing, mark and measure are there for the
 * scripts that time themselves.
 *
 * querySelector understands type, #id, .class, [attr] (=, ~=, ^=, $=,
 * *=, |=), combinators, :is()/:where()/:not() selector lists, structural
 * pseudo-classes (including An+B of selector-list), and common form states.
 */
#include <ctype.h>
#include <flux.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jsint.h"
#include "jsdom.h"
#include "webpolicy.h"
#include "webblob.h"
#include "jsnet.h"
#include "jsworker.h"
#include "jscanvas.h"
#include "jsdomlib.h"

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

struct shadow_scope {
    struct node *root;
    struct shadow_scope *next;
};

/* Request policy lives in native memory: scripts must not be able to rewrite it in flight. */
struct xhr_request {
    struct jsstr *url, *method, *headers, *body;
    int credentials, credential_mode, mode, cross, preflight, redirects, opened;
    int received, streaming, chunks, loaded;
    struct jsstr *partial;
};
static const struct jsclass xhr_class = {"XMLHttpRequest", NULL, NULL, NULL};

struct dispatch_path {
    jsval event;
    struct node *target;
    struct dispatch_path *prev;
};

struct jsdom {
    struct jsnet *network;
    struct jsworkers *workers;
    struct dispatch_path *dispatch_path;
    struct jshost h;
    struct js *J;
    struct jsobj *window, *document, *element_proto, *document_proto, *event_proto, *location;
    struct jsobj *style_proto, *classlist_proto;
    struct timer timers[MAX_TIMERS];
    int next_timer_id;
    struct node *current_script;
    struct node *write_after;           /* document.write's insertion point */
    int dirty;
    int repaint;                        /* pixels changed (a canvas): paint again, no layout */
    int hidden;                         /* the window is minimized: document.hidden */
    int loaded;
    int depth;                          /* nested entries from the browser */
    char cookies[2048];
    struct xhr_request requests[MAX_XHR];
    struct jsobj *xhr[MAX_XHR];         /* requests in flight, by id */
    struct jsobj *xhr_proto;
    unsigned int started;               /* uptime when the page's scripts began */
    struct jsobj **wraps;               /* every node's wrapper, for the collector (not roots) */
    int nwraps, wrap_cap;
    unsigned gcpass;
    struct jsobj *ev_protos[32];        /* Event, MouseEvent...: by EV_* */
    struct jsobj *node_proto, *text_proto, *frag_proto;     /* Node, Text, DocumentFragment */
    struct jsobj *tag_protos[T_COUNT];  /* HTMLInputElement and the rest, by tag (NULL: HTMLElement) */
    unsigned int activation;
    int activated, pointer_down, pointer_buttons;
    int drag_link;                      /* the drag going on is of a link (its effect is link at first) */
    struct node *capture, *pending_capture;
    struct node *active;                /* document.activeElement, when a script focused one */
    int custom_on;                      /* customElements.define was called: tell it of custom elements */
    int dom_lib;                        /* jsdomlib.h's js_dom_lib has been run */
    struct { struct node *n; char type[8]; } queued[128];      /* jsdom_queue_event */
    struct posted { char *data, *origin; int source; } *posted; /* jsdom_message: to deliver */
    int nposted;
    int nqueued;
    int watch_mutations;                /* a MutationObserver observes: report changes to %mutation */
    int in_mutation;
    int watch_view;                     /* an IntersectionObserver or ResizeObserver observes */
    int last_sx, last_sy, last_vw, last_vh;     /* the view as the scripts last heard of it */
    unsigned layout_gen, seen_gen;
    struct jsobj *history, *states;     /* history, and the state of each entry (by id) */
    int entry, hist_len;                /* the current entry's id; history.length */
    /* the import map: specifiers (or prefixes ending in /) and the URLs they stand for */
    char **im_from, **im_to;
    int nim;
    struct shadow_scope *shadows;
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
static int is_connected(struct jsdom *d,const struct node *n);
static void assign_slots(struct jsdom *d);
static void settle(struct jsdom *d)
{
    if(d->depth==0 && d->active && (!is_connected(d,d->active)||!web_focusable(d->active)))jsdom_focus(d,NULL);
    if (d->depth == 0 && d->dirty) {
        d->dirty = 0;
        d->repaint = 0;
        if(d->h.changed)d->h.changed(d->h.ctx);
    } else if (d->depth == 0 && d->repaint) {
        d->repaint = 0;
        if (d->h.repaint)
            d->h.repaint(d->h.ctx);
    }
}

void jsdom_canvas_dirty(struct js *J)
{
    dom(J)->repaint = 1;
}

/* ---- the tree ---- */

static int is_connected(struct jsdom *d, const struct node *n);

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

static jsval wrap(struct jsdom *d, struct node *n);
static int contains(const struct node *a, const struct node *b);

/* A change for the MutationObservers: childList (added or removed, between
 * prev and next), attributes (name, old value) or characterData (old value). */
static void mutation(struct jsdom *d, const char *type, struct node *target, struct node *added,
                     struct node *removed, struct node *prev, struct node *next, const char *attr,
                     const char *old)
{
    if (!d || !d->watch_mutations || d->in_mutation || !target || !contains(d->h.root, target))
        return;
    struct js *J = d->J;
    jsval hook = js_get(J, js_obj(d->window), "%mutation");
    if (!js_is_callable(hook))
        return;
    d->in_mutation = 1;
    jsval r = js_obj(js_new_object(J));
    js_set(J, r, "type", js_str(J, type));
    js_set(J, r, "target", wrap(d, target));
    struct jsobj *a = js_new_array(J), *rm = js_new_array(J);
    if (added)
        js_array_push(J, a, wrap(d, added));
    if (removed)
        js_array_push(J, rm, wrap(d, removed));
    js_set(J, r, "addedNodes", js_obj(a));
    js_set(J, r, "removedNodes", js_obj(rm));
    js_set(J, r, "previousSibling", wrap(d, prev));
    js_set(J, r, "nextSibling", wrap(d, next));
    js_set(J, r, "attributeName", attr ? js_str(J, attr) : js_null);
    js_set(J, r, "attributeNamespace", js_null);
    js_set(J, r, "oldValue", old ? js_str(J, old) : js_null);
    jsval out;
    if (js_call(J, hook, js_undefined, &r, 1, &out) < 0)
        report(d, "MutationObserver");
    d->in_mutation = 0;
}

/* Custom elements (their names have a dash): the customElements code in
 * jsprelude.h upgrades them and calls connectedCallback and the rest. */
static int is_custom(const struct node *n)
{
    return n && n->type == N_ELEMENT && n->name && strchr(n->name, '-') && n->name[0] != '#';
}

static void ce_hook(struct jsdom *d, const char *hook, jsval *args, int n)
{
    struct js *J = d->J;
    jsval f = js_get(J, js_obj(d->window), hook), r;
    if (js_is_callable(f) && js_call(J, f, js_undefined, args, n, &r) < 0)
        report(d, hook);
}

/* n went into the document (connected 1) or out of it: each custom element in it hears. */
static void ce_tree(struct jsdom *d, struct node *n, int connected)
{
    if (!d || !d->custom_on || (connected && !is_connected(d, n)))
        return;
    for (struct node *k = n; k; k = node_next(k, n))
        if (is_custom(k)) {
            jsval w = wrap(d, k);
            ce_hook(d, connected ? "%ceConnected" : "%ceDisconnected", &w, 1);
        }
}

static void top_layer_removed(struct jsdom *d, struct node *root);
static void top_layer_attr(struct jsdom *d, struct node *n, const char *name, const char *old, const char *value);

static void unlink_node(struct jsdom *d, struct node *n)
{
    struct node *p = n->parent;
    if (!p)
        return;
    if (d && is_connected(d, p))
        top_layer_removed(d, n);
    if (d && d->watch_mutations)
        mutation(d, "childList", p, NULL, n, n->prev, n->next, NULL, NULL);
    int was_connected = d && d->custom_on && is_connected(d, p);
    if (n->prev)
        n->prev->next = n->next;
    else
        p->first = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        p->last = n->prev;
    n->parent = n->prev = n->next = NULL;
    if (d)                              /* (a template's content moves without one) */
        assign_slots(d);
    renumber(p);
    if (was_connected)
        ce_tree(d, n, 0);
}

static int contains(const struct node *a, const struct node *b)
{
    for (; b; b = b->parent)
        if (a == b)
            return 1;
    return 0;
}

static int is_connected(struct jsdom *d, const struct node *n)
{
    int depth = 0;
    while (n && depth++ < 256) {
        if (n == d->h.root)
            return 1;
        n = n->parent ? n->parent : n->shadow_host;
    }
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
    for (struct node *p = parent; p; p = p->parent ? p->parent : p->shadow_host)
        if (p == n)
            return -1;
    if (is_fragment(n)) {
        while (n->first) {
            struct node *k = n->first;
            unlink_node(d, k);
            insert_node(d, parent, k, ref);
        }
        return 0;
    }
    if (ref && ref->parent != parent)
        ref = NULL;
    if (n == ref)
        return 0;
    unlink_node(d, n);
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
    assign_slots(d);
    d->dirty = 1;
    if (d->watch_mutations)
        mutation(d, "childList", parent, n, NULL, n->prev, n->next, NULL, NULL);
    if (d->custom_on)
        ce_tree(d, n, 1);
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

static void ce_attr(struct jsdom *d, struct node *n, const char *name, const char *old, const char *value)
{
    struct js *J = d->J;
    jsval a[4] = { wrap(d, n), js_str(J, name), old ? js_str(J, old) : js_null, value ? js_str(J, value) : js_null };
    ce_hook(d, "%ceAttr", a, 4);
}

static void set_attr(struct jsdom *d, struct node *n, const char *name, const char *value)
{
    if (!is_element(n))
        return;
    char lname[64];
    snprintf(lname, sizeof(lname), "%s", name);
    for (char *p = lname; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    if (!strcmp(lname, "type"))
        n->file_chosen = 0;                     /* a file the user picked was for the old kind of input */
    struct attr **pp = &n->attrs;
    for (; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->name, lname)) {
            const char *old = (*pp)->value;
            (*pp)->value = arena_strdup(d->h.arena, value);
            if (!strcmp(lname, "slot") || !strcmp(lname, "name")) assign_slots(d);
            d->dirty = 1;
            if (n->tag == T_CANVAS && (!strcmp(lname, "width") || !strcmp(lname, "height")))
                jscanvas_resized(d->J, n, n->js ? js_obj(n->js) : js_undefined);
            if (d->watch_mutations)
                mutation(d, "attributes", n, NULL, NULL, NULL, NULL, lname, old);
            if (d->custom_on && is_custom(n))
                ce_attr(d, n, lname, old, value);
            top_layer_attr(d, n, lname, old, value);
            return;
        }
    struct attr *a = arena_alloc(d->h.arena, sizeof(*a));
    if (!a)
        return;
    a->name = arena_strdup(d->h.arena, lname);
    a->value = arena_strdup(d->h.arena, value);
    *pp = a;
    if (!strcmp(lname, "slot") || !strcmp(lname, "name")) assign_slots(d);
    d->dirty = 1;
    if (n->tag == T_CANVAS && (!strcmp(lname, "width") || !strcmp(lname, "height")))
        jscanvas_resized(d->J, n, n->js ? js_obj(n->js) : js_undefined);
    if (d->watch_mutations)
        mutation(d, "attributes", n, NULL, NULL, NULL, NULL, lname, NULL);
    if (d->custom_on && is_custom(n))
        ce_attr(d, n, lname, NULL, value);
    top_layer_attr(d, n, lname, NULL, value);
}

static void remove_attr(struct jsdom *d, struct node *n, const char *name)
{
    if (!is_element(n))
        return;
    for (struct attr **pp = &n->attrs; *pp; pp = &(*pp)->next)
        if (!strcasecmp((*pp)->name, name)) {
            struct attr *gone = *pp;
            *pp = gone->next;
            if (!strcmp(gone->name, "slot") || !strcmp(gone->name, "name")) assign_slots(d);
            d->dirty = 1;
            if (d->watch_mutations)
                mutation(d, "attributes", n, NULL, NULL, NULL, NULL, gone->name, gone->value);
            if (d->custom_on && is_custom(n))
                ce_attr(d, n, gone->name, gone->value, NULL);
            top_layer_attr(d, n, gone->name, gone->value, NULL);
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
        unlink_node(d, n->first);
    d->dirty = 1;
}

static void set_text_content(struct jsdom *d, struct node *n, const char *s, int len)
{
    if (n->type == N_TEXT) {
        const char *old = n->text;
        n->text = arena_strndup(d->h.arena, s, (size_t)len);
        n->len = len;
        d->dirty = 1;
        if (d->watch_mutations)
            mutation(d, "characterData", n, NULL, NULL, NULL, NULL, NULL, old);
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
        unlink_node(d, k);
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

/* HTML class tokens split on ASCII whitespace, excluding vertical tab. */
static int class_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int has_class(const struct node *n, const char *cls, int len)
{
    const char *v = node_attr(n, "class");
    if (!v)
        return 0;
    while (*v) {
        while (*v && class_space((unsigned char)*v))
            v++;
        const char *s = v;
        while (*v && !class_space((unsigned char)*v))
            v++;
        if (len > 0 && v - s == len && !strncmp(s, cls, (size_t)len))
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

/* CSS.escape output includes hex escapes followed by optional whitespace. */
static const char *selector_escape(const char *p,const char *end,int *codepoint)
{
    p++;int cp=0,k=0;
    while(p<end && k<6 && isxdigit((unsigned char)*p)) {
        cp=cp*16+(isdigit((unsigned char)*p)?*p-'0':tolower((unsigned char)*p)-'a'+10);p++;k++;
    }
    if(k) {
        if(p<end && class_space((unsigned char)*p)) { if(*p=='\r' && p+1<end && p[1]=='\n')p++;p++; }
        if(!cp || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))cp=0xfffd;
    } else if(p<end) cp=(unsigned char)*p++;
    else cp=0xfffd;
    if(codepoint)*codepoint=cp;
    return p;
}
static int selector_ident(const char **position,const char *end,char *out,size_t size)
{
    const char *p=*position;size_t used=0;
    while(p<end && ident_char((unsigned char)*p)) {
        if(*p=='\\') {
            int cp;p=selector_escape(p,end,&cp);
            if(used+4>=size)return 0;
            used+=(size_t)utf8_put(out+used,cp);
        } else {
            if(used+1>=size)return 0;
            out[used++]=*p++;
        }
    }
    out[used]=0;*position=p;return used>0;
}

static int match_compound(const struct node *n, const char *s, int len);
static int matches_span(const struct node *n, const char *s, int len);

static int nth_matches(const struct node *n, const char *arg, int len, int last, int of_type)
{
    int a, b;
    const char *filter, *end = arg + len;
    if (web_parse_nth(arg, end, &a, &b, &filter) < 0 || (of_type && filter))
        return 0;
    if (filter && !matches_span(n, filter, (int)(end - filter)))
        return 0;
    int position = 1;
    for (const struct node *p = last ? next_element(n) : prev_element(n); p;
         p = last ? next_element(p) : prev_element(p))
        if ((!of_type || !strcmp(p->name, n->name)) &&
            (!filter || matches_span(p, filter, (int)(end - filter))))
            position++;
    return web_nth_fits(a, b, position);
}

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
    if (IS("nth-child") || IS("nth-last-child") || IS("nth-of-type") || IS("nth-last-of-type"))
        return arg && nth_matches(n, arg, alen, IS("nth-last-child") || IS("nth-last-of-type"),
                                  IS("nth-of-type") || IS("nth-last-of-type"));
    if (IS("only-of-type")) {
        for (const struct node *k = prev_element(n); k; k = prev_element(k))
            if (!strcmp(k->name, n->name))
                return 0;
        for (const struct node *k = next_element(n); k; k = next_element(k))
            if (!strcmp(k->name, n->name))
                return 0;
        return 1;
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
        return arg && !matches_span(n, arg, alen);
    if (IS("is") || IS("where"))
        return arg && matches_span(n, arg, alen);
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
    if (IS("popover-open"))
        return n->popover_open != 0;
    if (IS("modal"))
        return n->modal;
    if (IS("open"))
        return (n->tag == T_DETAILS || n->tag == T_DIALOG) && node_attr(n, "open");
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
            char decoded[1024];
            if(!selector_ident(&s,e,decoded,sizeof(decoded)))return 0;
            if (c == '#') {
                const char *id = node_attr(n, "id");
                if (!id || strcmp(id,decoded))return 0;
            } else if (!has_class(n,decoded,(int)strlen(decoded)))return 0;
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

/* How deep the matcher has gone into nested :not()/:is() and into combinators:
 * each costs stack, which may be a script's coroutine's. */
static int sel_nest, sel_combinators;

/* A complex selector (compounds and combinators), matched right to left. */
static int match_complex_in(const struct node *n, const char *s, int len);
static int match_complex(const struct node *n, const char *s, int len)
{
    if (sel_combinators >= 128)
        return 0;
    sel_combinators++;
    int r = match_complex_in(n, s, len);
    sel_combinators--;
    return r;
}

static int match_complex_in(const struct node *n, const char *s, int len)
{
    /* find the last combinator outside brackets and parentheses */
    int depth = 0, at = -1;
    char comb = 0;
    char quote=0;
    for (int i=0;i<len;i++) {
        char c=s[i];
        if(c=='\\'){i=(int)(selector_escape(s+i,s+len,NULL)-s)-1;continue;}
        if(quote){if(c==quote)quote=0;continue;}
        if(c=='\'' || c=='"'){quote=c;continue;}
        if(c=='[' || c=='('){depth++;continue;}
        if(c==']' || c==')'){depth--;continue;}
        if(depth)continue;
        if(c=='>' || c=='+' || c=='~'){at=i;comb=c;}
        else if(class_space((unsigned char)c)) {
            int j=i;while(j+1<len && class_space((unsigned char)s[j+1]))j++;
            if(i>0 && j+1<len && !strchr(">+~",s[i-1]) && !strchr(">+~",s[j+1])){at=j;comb=' ';}
            i=j;
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

/* A selector list, bounded so nested functional selectors need no copies. */
static int matches_span_in(const struct node *n, const char *s, int len);
static int matches_span(const struct node *n, const char *s, int len)
{
    if (sel_nest >= 8)
        return 0;
    sel_nest++;
    int r = matches_span_in(n, s, len);
    sel_nest--;
    return r;
}

static int matches_span_in(const struct node *n, const char *s, int len)
{
    const char *end = s + len;
    while (s < end) {
        int depth = 0;
        char quote = 0;
        const char *e = s;
        for (; e < end; e++) {
            if (*e == '\\') { e=selector_escape(e,end,NULL)-1; continue; }
            if (quote) {
                if (*e == quote) quote = 0;
            } else if (*e == '\'' || *e == '"') {
                quote = *e;
            } else if (*e == '(' || *e == '[') {
                depth++;
            } else if (*e == ')' || *e == ']') {
                if (depth) depth--;
            } else if (*e == ',' && !depth) {
                break;
            }
        }
        const char *a = s, *b = e;
        while (a < b && isspace((unsigned char)*a)) a++;
        const char *trim=a;
        for(const char *p=a;p<b;) {
            if(*p=='\\'){p=selector_escape(p,b,NULL);trim=p;}
            else {if(!class_space((unsigned char)*p))trim=p+1;p++;}
        }
        b=trim;
        if (b > a && match_complex(n, a, (int)(b - a)))
            return 1;
        s = e < end ? e + 1 : e;
    }
    return 0;
}

static int matches(const struct node *n, const char *sel)
{
    return matches_span(n, sel, (int)strlen(sel));
}

/* ---- wrappers ---- */

static jsval wrap(struct jsdom *d, struct node *n)
{
    if (!n)
        return js_null;
    if (!n->js) {
        struct jsobj *o = js_new_host(d->J, &node_class, n);
        o->proto = n->type == N_TEXT ? d->text_proto : is_fragment(n) ? d->frag_proto
                 : d->tag_protos[n->tag] ? d->tag_protos[n->tag] : d->element_proto;
        if (d->nwraps == d->wrap_cap) {
            int cap = d->wrap_cap ? d->wrap_cap * 2 : 256;
            struct jsobj **nw = realloc(d->wraps, sizeof(*nw) * (size_t)cap);
            if (!nw)
                js_gc_pause(d->J, 1);   /* it could not be forgotten when it dies: never collect */
            else
                d->wraps = nw, d->wrap_cap = cap;
        }
        if (d->nwraps < d->wrap_cap)
            d->wraps[d->nwraps++] = o;
        n->js = o;
    }
    return js_obj(n->js);
}

/*
 * The collector and the wrappers. A node's wrapper holds its listeners and
 * whatever scripts stored on it, so it lives as long as the node can be
 * reached: every wrapper in the document lives, and so does every wrapper
 * in a detached tree that a living wrapper is part of. Dead ones are
 * forgotten (wrap makes a fresh one if the node turns up again).
 */
static int mark_tree(struct jsdom *d, struct node *root)
{
    int any = 0;
    for (struct node *n = root; n; n = node_next_shadow(n, root)) {
        n->gcpass = d->gcpass;
        if (n->js && !js_gc_marked(d->J, n->js)) {
            js_gc_mark(d->J, n->js);
            any = 1;
        }
    }
    return any;
}

static void gc_mark(struct js *J, void *ctx)
{
    (void)J;
    struct jsdom *d = ctx;
    if (!++d->gcpass)
        d->gcpass = 1;
    if (d->h.root)
        mark_tree(d, d->h.root);
    if(d->capture&&d->capture->js)js_gc_mark(J,d->capture->js);
    if(d->pending_capture&&d->pending_capture->js)js_gc_mark(J,d->pending_capture->js);
}

static int gc_more(struct js *J, void *ctx)
{
    struct jsdom *d = ctx;
    int any = 0;
    for (int i = 0; i < d->nwraps; i++) {
        struct jsobj *o = d->wraps[i];
        if (!js_gc_marked(J, o))
            continue;
        struct node *r = js_host_data(o, &node_class);
        if (!r)
            continue;
        while (r->parent || r->shadow_host)
            r = r->parent ? r->parent : r->shadow_host;
        if (r->gcpass != d->gcpass)
            any |= mark_tree(d, r);
    }
    return any;
}

static void gc_sweep(struct js *J, void *ctx)
{
    struct jsdom *d = ctx;
    int k = 0;
    for (int i = 0; i < d->nwraps; i++) {
        struct jsobj *o = d->wraps[i];
        if (js_gc_marked(J, o)) {
            d->wraps[k++] = o;
            continue;
        }
        struct node *n = js_host_data(o, &node_class);
        if (n && n->js == o)
            n->js = NULL;
    }
    d->nwraps = k;
}

static struct node *node_of(jsval v)
{
    return v.t == JS_OBJ ? js_host_data(v.u.o, &node_class) : NULL;
}

struct node *jsdom_node_of(jsval v)
{
    return node_of(v);
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
    "srcset", "sizes", "loading", "decoding", "referrerpolicy", "integrity", "nonce", "as", "hreflang",
    "inputmode", "enterkeyhint", "autocapitalize", "translate", "is", "minlength", "form", "popover",
    "fetchpriority", "crossorigin", NULL,
};

static const char *const boolean_attrs[] = {
    "disabled", "readonly", "required", "hidden", "multiple", "autofocus", "async", "defer",
    "open", "novalidate", "selected", "inert", NULL,
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

static int is_file_input(const struct node *n)
{
    const char *t = n->tag == T_INPUT ? node_attr(n, "type") : NULL;
    return t && !strcasecmp(t, "file");
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
    if (d->h.node_visual_box)
        d->h.node_visual_box(d->h.ctx, n, &x, &y, &w, &h);
    else if (d->h.node_box)
        d->h.node_box(d->h.ctx, n, &x, &y, &w, &h);
    if (d->h.scroll_pos)
        d->h.scroll_pos(d->h.ctx, &sx, &sy);
    /* Scrolling boxes move descendants, but do not move their own border boxes. */
    if (!d->h.node_visual_box && d->h.node_scroll)
        for (struct node *p = n ? n->parent : NULL; p; p = p->parent) {
            int bx = 0, by = 0, cw, ch, vw, vh;
            if (d->h.node_scroll(d->h.ctx, p, 0, &bx, &by, &cw, &ch, &vw, &vh))
                x -= bx, y -= by;
        }
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
    if (IS("innerText") && n->type == N_ELEMENT) {      /* as rendered: white space collapsed */
        char *buf = malloc(65536);
        if (buf) {
            node_text(n, buf, 65536);
            *out = js_str(J, buf);
            free(buf);
            return 1;
        }
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
    if (IS("shadowRoot")) {
        *out = wrap(d, n->shadow_root && !n->shadow_root->shadow_closed ? n->shadow_root : NULL);
        return 1;
    }
    if (IS("host") && n->shadow_host) {
        *out = wrap(d, n->shadow_host);
        return 1;
    }
    if (IS("mode") && n->shadow_host) {
        *out = js_str(J, n->shadow_closed ? "closed" : "open");
        return 1;
    }
    if (IS("assignedSlot")) {
        struct node *host = n->parent;
        const char *name = node_attr(n, "slot");
        *out = wrap(d, host && host->shadow_root && !host->shadow_root->shadow_closed
                           ? shadow_slot(host, name ? name : "")
                           : NULL);
        return 1;
    }
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
        if (is_file_input(n)) {                 /* the file's name only, as browsers show it */
            const char *v = n->file_chosen && n->value ? n->value : "", *base = strrchr(v, '/');
            char fake[300];
            snprintf(fake, sizeof(fake), "%s%s", *v ? "C:\\fakepath\\" : "", base ? base + 1 : v);
            *out = js_str(J, fake);
            return 1;
        }
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
        if (d->dirty && d->h.flush && (IS("scrollWidth") || IS("scrollHeight"))) {   /* content was just added: how big now? */
            d->dirty = 0;
            d->h.flush(d->h.ctx);
        }
        if (d->h.node_box)
            d->h.node_box(d->h.ctx, n, &x, &y, &w, &h);
        int ssx, ssy, scw, sch, svw, svh;
        if ((IS("scrollWidth") || IS("scrollHeight") || IS("clientWidth") || IS("clientHeight")) &&
            d->h.node_scroll && d->h.node_scroll(d->h.ctx, n, 0, &ssx, &ssy, &scw, &sch, &svw, &svh)) {
            w = IS("scrollWidth") ? scw : svw;      /* a box that scrolls: its contents, or what it shows of them */
            h = IS("scrollHeight") ? sch : svh;
        }
        *out = js_num(strstr(key, "Width") ? w : strstr(key, "Height") ? h : IS("offsetTop") ? y : x);
        return 1;
    }
    if (IS("scrollTop") || IS("scrollLeft")) {
        int sx = n->scroll_x, sy = n->scroll_y, cw, ch, vw, vh;
        if (n == d->h.root || n->tag == T_BODY) {   /* the root and body scroll with the window */
            sx = sy = 0;
            if (d->h.scroll_pos)
                d->h.scroll_pos(d->h.ctx, &sx, &sy);
        } else if (d->h.node_scroll) {
            if (d->dirty && d->h.flush) {
                d->dirty = 0;
                d->h.flush(d->h.ctx);
            }
            d->h.node_scroll(d->h.ctx, n, 0, &sx, &sy, &cw, &ch, &vw, &vh);
        }
        *out = js_num(IS("scrollTop") ? sy : sx);
        return 1;
    }
    if (IS("content") && n->tag == T_TEMPLATE) {     /* its children, moved into a fragment */
        jsval c = js_get(J, wrap(d, n), "__content");
        if (c.t != JS_OBJ) {
            struct node *frag = new_node(d, N_ELEMENT, "#fragment");
            while (frag && n->first) {
                struct node *k = n->first;
                unlink_node(NULL, k);
                insert_node(d, frag, k, NULL);
            }
            c = wrap(d, frag);
            js_put(J, ((struct jsobj *)n->js), S(J, "__content"), c, 1);
        }
        *out = c;
        return 1;
    }
    if ((IS("naturalWidth") || IS("naturalHeight")) && n->tag == T_IMG) {
        *out = js_num(n->img ? (IS("naturalWidth") ? n->img->w : n->img->h) : 0);
        return 1;
    }
    if (IS("complete") && n->tag == T_IMG) {
        const char *src = node_attr(n, "src");
        *out = js_bool(n->img || !src || !*src);
        return 1;
    }
    if (IS("currentSrc") && (n->tag == T_IMG || n->tag == T_VIDEO || n->tag == T_AUDIO)) {
        const char *v = node_attr(n, "src");
        char abs[URL_MAX] = "";
        if (v)
            url_resolve(d->h.base, v, abs, sizeof(abs));
        *out = js_str(J, abs);
        return 1;
    }
    if (IS("isContentEditable") || IS("contentEditable")) {
        const struct node *k = n;
        const char *v = NULL;
        while (k && k->type == N_ELEMENT && !(v = node_attr(k, "contenteditable")))
            k = IS("isContentEditable") ? k->parent : NULL;
        int on = v && strcasecmp(v, "false");
        *out = IS("isContentEditable") ? js_bool(on) : js_str(J, !v ? "inherit" : on ? "true" : "false");
        return 1;
    }
    if (IS("labels") && n->type == N_ELEMENT) {
        struct jsobj *a = js_new_array(J);
        const char *id = node_attr(n, "id");
        for (struct node *k = d->h.root; k; k = node_next(k, d->h.root)) {
            if (k->type != N_ELEMENT || k->tag != T_LABEL)
                continue;
            const char *f = node_attr(k, "for");
            if ((f && id && !strcmp(f, id)) || (!f && contains(k, n)))
                js_array_push(J, a, wrap(d, k));
        }
        *out = js_obj(a);
        return 1;
    }
    if (IS("offsetParent")) {
        *out = wrap(d, n->parent);
        return 1;
    }
    if (IS("isConnected")) {
        *out = js_bool(is_connected(d, n));
        return 1;
    }
    if (n->tag == T_CANVAS && (IS("width") || IS("height"))) {
        int w, h;
        jscanvas_size(n, &w, &h);
        *out = js_num(IS("width") ? w : h);
        return 1;
    }
    if (IS("tabIndex")) { *out = js_num(web_tabindex(n)); return 1; }
    if (IS("popover")) {                /* the state's keyword, or null when it is not a popover */
        static const char *const states[] = { NULL, "auto", "manual", "hint" };
        const char *v = node_attr(n, "popover");
        int st = !v ? POP_NONE : !*v || !strcasecmp(v, "auto") ? POP_AUTO : !strcasecmp(v, "hint") ? POP_HINT : POP_MANUAL;
        *out = st ? js_str(J, states[st]) : js_null;
        return 1;
    }
    if (IS("popoverTargetElement") || IS("commandForElement")) {
        const char *id = node_attr(n, IS("commandForElement") ? "commandfor" : "popovertarget");
        struct node *t = id && *id ? by_id(d->h.root, id) : NULL;
        *out = t ? wrap(d, t) : js_null;
        return 1;
    }
    if (IS("popoverTargetAction") || IS("command")) {
        const char *v = node_attr(n, IS("command") ? "command" : "popovertargetaction");
        if (IS("popoverTargetAction"))
            v = v && (!strcasecmp(v, "show") || !strcasecmp(v, "hide")) ? v : "toggle";
        *out = js_str(J, v ? v : "");
        return 1;
    }
    if (IS("closedBy") && n->tag == T_DIALOG) {
        const char *v = node_attr(n, "closedby");
        if (!v || (strcasecmp(v, "any") && strcasecmp(v, "closerequest") && strcasecmp(v, "none")))
            v = n->modal ? "closerequest" : "none";
        *out = js_str(J, v);
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
            unlink_node(d, n);
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
        } else if (is_file_input(n)) {
            if (!s->s[0]) {                     /* only clearing it: a page cannot pick the file */
                free(n->value);
                n->value = strdup("");
                n->file_chosen = 0;
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
    if (IS("scrollTop") || IS("scrollLeft")) {
        double p = js_to_number(J, v);
        int sx = 0, sy = 0;
        if (p != p)
            p = 0;
        if (p > 1e9)
            p = 1e9;                        /* scrollTop = Infinity means as far as it goes */
        if ((n == d->h.root || n->tag == T_BODY) && d->h.scroll && d->h.scroll_pos) {
            d->h.scroll_pos(d->h.ctx, &sx, &sy);
            d->h.scroll(d->h.ctx, IS("scrollLeft") ? (int)p : sx, IS("scrollTop") ? (int)p : sy, 0);
        } else if (d->h.node_scroll) {
            int cw, ch, vw, vh;
            if (d->dirty && d->h.flush) {
                d->dirty = 0;
                d->h.flush(d->h.ctx);
            }
            sx = n->scroll_x, sy = n->scroll_y;
            d->h.node_scroll(d->h.ctx, n, 0, &sx, &sy, &cw, &ch, &vw, &vh);
            if (IS("scrollLeft"))
                sx = (int)p;
            else
                sy = (int)p;
            d->h.node_scroll(d->h.ctx, n, 1, &sx, &sy, &cw, &ch, &vw, &vh);
        }
        return 1;
    }
    if (IS("contentEditable")) {
        set_attr(d, n, "contenteditable", js_tostr(J, v)->s);
        return 1;
    }
    if (n->tag == T_CANVAS && (IS("width") || IS("height"))) {      /* an unsigned long: 7.9 is 7 */
        double dv = js_to_number(J, v);
        char num[16];
        int def = IS("width") ? 300 : 150;
        snprintf(num, sizeof(num), "%u", dv >= 0 && dv <= 2147483647.0 ? (unsigned)dv : (unsigned)def);
        set_attr(d, n, key, num);
        return 1;
    }
    if (IS("tabIndex")) {
        char text[24]; double number = js_to_number(J, v);
        int index = number != number ? 0 : number < -32768 ? -32768 : number > 32767 ? 32767 : (int)number;
        snprintf(text, sizeof(text), "%d", index);
        set_attr(d, n, "tabindex", text); return 1;
    }
    if (IS("popoverTargetElement") || IS("commandForElement")) {
        const char *name = IS("commandForElement") ? "commandfor" : "popovertarget";
        struct node *t = node_of(v);
        const char *id = t ? node_attr(t, "id") : NULL;
        if (id && *id)
            set_attr(d, n, name, id);
        else
            remove_attr(d, n, name);
        return 1;
    }
    if (IS("popoverTargetAction") || IS("command") || IS("closedBy")) {
        set_attr(d, n, IS("command") ? "command" : IS("closedBy") ? "closedby" : "popovertargetaction",
                 js_tostr(J, v)->s);
        return 1;
    }
    if (IS("popover")) {
        if (v.t == JS_NULL)
            remove_attr(d, n, "popover");
        else
            set_attr(d, n, "popover", js_tostr(J, v)->s);
        return 1;
    }
    if (in_list(reflected, key, attr, sizeof(attr))) {
        set_attr(d, n, attr, js_tostr(J, v)->s);
        if ((IS("srcset") || IS("sizes") || IS("loading")) && n->tag == T_IMG && d->h.load_image)
            d->h.load_image(d->h.ctx, n);
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

/* A custom element's constructor: its object is the node's wrapper from now on. */
static void node_adopt(struct js *J, struct jsobj *self, struct jsobj *from)
{
    struct jsdom *d = dom(J);
    struct node *n = from->hdata;
    if (n && n->js == from)
        n->js = self;
    for (int i = 0; i < d->nwraps; i++)
        if (d->wraps[i] == from)
            d->wraps[i] = self;
}

static const struct jsclass node_class = { "HTMLElement", node_get, node_set, node_adopt };

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

static const struct jsclass style_class = { "CSSStyleDeclaration", style_get, style_set, NULL };

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
    char b[512];
    if (!strcmp(key,"transform") || !strcmp(key,"transformOrigin") || !strcmp(key,"transform-origin")) {
        if(!st){*out=js_str(J,!strcmp(key,"transform")?"none":"0px 0px");return 1;}
        int x=0,y=0,w=0,h=0;
        if(d->h.node_box)d->h.node_box(d->h.ctx,n,&x,&y,&w,&h);
        if(!strcmp(key,"transform")) {
            if(!st->xf){*out=js_str(J,"none");return 1;}
            double m[16];css_transform_matrix3d(st,w,h,m);
            int is2d=m[2]==0 && m[3]==0 && m[6]==0 && m[7]==0 && m[8]==0 && m[9]==0 && m[10]==1 && m[11]==0 && m[14]==0 && m[15]==1;
            if(is2d)snprintf(b,sizeof(b),"matrix(%.9g, %.9g, %.9g, %.9g, %.9g, %.9g)",m[0],m[1],m[4],m[5],m[12],m[13]);
            else snprintf(b,sizeof(b),"matrix3d(%.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g)",m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15]);
        } else snprintf(b,sizeof(b),"%gpx %gpx",css_transform_length(st->tox,w),css_transform_length(st->toy,h));
        *out=js_str(J,b);return 1;
    }
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
    if(!strcmp(key,"textDecorationThickness")||!strcmp(key,"text-decoration-thickness")||
       !strcmp(key,"textUnderlineOffset")||!strcmp(key,"text-underline-offset")){
        int thickness=strstr(key,"Thickness")||strstr(key,"thickness");
        struct len length=st?(thickness?st->deco_thickness:st->underline_offset):(struct len){.unit=U_AUTO};
        if(length.unit==U_AUTO)strcpy(b,"auto");
        else if(length.unit==U_PCT)snprintf(b,sizeof(b),"%g%%",length.v/100.0);
        else snprintf(b,sizeof(b),"%dpx",length.v);
        *out=js_str(J,b);return 1;
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

static const struct jsclass computed_class = { "CSSStyleDeclaration", computed_get, NULL, NULL };

/* ---- classList and dataset ---- */

/* Walk the ordered set without changing the attribute's original spelling. */
static const char *class_token(const char *v, const char **cursor, int *len)
{
    const char *s = *cursor;
    while (*s) {
        while (class_space((unsigned char)*s))
            s++;
        const char *t = s;
        while (*s && !class_space((unsigned char)*s))
            s++;
        if (s == t)
            break;
        int duplicate = 0;
        for (const char *p = v; p < t;) {
            while (p < t && class_space((unsigned char)*p))
                p++;
            const char *q = p;
            while (p < t && !class_space((unsigned char)*p))
                p++;
            if (q < t && p - q == s - t && !memcmp(q, t, (size_t)(s - t))) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            *cursor = s;
            *len = (int)(s - t);
            return t;
        }
    }
    *cursor = s;
    return NULL;
}

static int classlist_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    const char *v = node_attr(o->hdata, "class");
    if (!v)
        v = "";
    if (!strcmp(key, "value")) {
        *out = js_str(J, v);
        return 1;
    }
    int length = !strcmp(key, "length");
    int index = 0;
    if (!length) {
        if (!*key || (key[0] == '0' && key[1]))
            return 0;
        for (const char *p = key; *p; p++) {
            if (*p < '0' || *p > '9' || index > 214748364 ||
                (index == 214748364 && *p > '7'))
                return 0;
            index = index * 10 + *p - '0';
        }
    }
    int count = 0, len;
    const char *cursor = v, *token;
    while ((token = class_token(v, &cursor, &len))) {
        if (!length && count == index) {
            *out = js_strn(J, token, len);
            return 1;
        }
        count++;
    }
    *out = length ? js_num(count) : js_undefined;
    return 1;
}

static int classlist_set(struct js *J, struct jsobj *o, const char *key, jsval value)
{
    if (strcmp(key, "value"))
        return 0;
    set_attr(dom(J), o->hdata, "class", js_tostr(J, value)->s);
    return 1;
}

static const struct jsclass classlist_class = { "DOMTokenList", classlist_get, classlist_set, NULL };

static struct node *classlist_node(jsval self)
{
    return self.t == JS_OBJ ? js_host_data(self.u.o, &classlist_class) : NULL;
}

static int class_valid(struct js *J, struct jsstr *s)
{
    const char *name = NULL;
    if (!s->len)
        name = "SyntaxError";
    for (int i = 0; i < s->len; i++)
        if (class_space((unsigned char)s->s[i]))
            name = "InvalidCharacterError";
    if (!name)
        return 1;
    jsval error = js_make_error(J, "Error", "Invalid class token");
    js_set(J, error, "name", js_str(J, name));
    js_throw_value(J, error);
    return 0;
}

/* Serialize a mutation once. Replacement retains the earlier token's slot. */
static int class_update(struct jsdom *d, struct node *n, const char *name,
                         const char *replacement, int add)
{
    const char *v = node_attr(n, "class");
    if (!v && !add)
        return 0;
    if (!v)
        v = "";
    size_t len = strlen(name);
    char *nv = malloc(strlen(v) + len + (replacement ? strlen(replacement) : 0) + 2);
    if (!nv)
        return 0;
    const char *cursor = v, *token;
    int tl, present = 0;
    char *out = nv;
    while ((token = class_token(v, &cursor, &tl))) {
        int match = (size_t)tl == len && !memcmp(token, name, len);
        if (match) {
            present = 1;
            if (replacement) {
                /* If the new token was earlier, removing this slot suffices. */
                struct node tmp = { 0 };
                struct attr attr = { "class", nv, NULL };
                tmp.attrs = &attr;
                *out = 0;
                if (has_class(&tmp, replacement, (int)strlen(replacement)))
                    continue;
                token = replacement;
                tl = (int)strlen(replacement);
            } else if (!add) {
                continue;
            }
        } else if (replacement && present && (size_t)tl == strlen(replacement) &&
                   !memcmp(token, replacement, (size_t)tl)) {
            continue;
        }
        if (out != nv)
            *out++ = ' ';
        memcpy(out, token, (size_t)tl);
        out += tl;
    }
    if (add && !present) {
        if (out != nv)
            *out++ = ' ';
        memcpy(out, name, len);
        out += len;
    }
    *out = 0;
    /* replace(missing, ...) does not normalize or create an attribute. */
    if (!replacement || present)
        set_attr(d, n, "class", nv);
    free(nv);
    return present;
}

static jsval classlist_change(struct js *J, jsval self, jsval *args, int argc, int add)
{
    struct node *n = classlist_node(self);
    if (!n)
        return js_undefined;
    /* Convert and validate every argument before touching the attribute. */
    struct jsobj *tokens = js_new_array(J);
    for (int i = 0; i < argc; i++) {
        struct jsstr *s = js_tostr(J, args[i]);
        if (js_throwing(J))
            return js_undefined;
        js_array_push(J, tokens, js_strn(J, s->s, s->len));
    }
    for (int i = 0; i < argc; i++)
        if (!class_valid(J, tokens->items[i].u.s))
            return js_undefined;
    for (int i = 0; i < argc; i++)
        class_update(dom(J), n, js_cstr(tokens->items[i]), NULL, add);
    if (!argc && node_attr(n, "class"))
        class_update(dom(J), n, "", NULL, 0);
    return js_undefined;
}

static jsval classlist_add(struct js *J, jsval self, jsval *args, int argc)
{
    return classlist_change(J, self, args, argc, 1);
}

static jsval classlist_remove(struct js *J, jsval self, jsval *args, int argc)
{
    return classlist_change(J, self, args, argc, 0);
}

static jsval classlist_toggle(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    struct jsstr *s = js_tostr(J, ARG(0));
    if (js_throwing(J) || !class_valid(J, s) || !n)
        return js_undefined;
    int present = has_class(n, s->s, s->len);
    int add = argc > 1 ? js_truthy(args[1]) : !present;
    if (add != present)
        class_update(dom(J), n, s->s, NULL, add);
    return js_bool(add);
}

static jsval classlist_contains(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    struct jsstr *s = js_tostr(J, ARG(0));
    return js_bool(n && s->len && has_class(n, s->s, s->len));
}

static jsval classlist_replace(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    struct jsstr *a = js_tostr(J, ARG(0));
    if (js_throwing(J))
        return js_undefined;
    struct jsstr *b = js_tostr(J, ARG(1));
    if (js_throwing(J) || !class_valid(J, a) || !class_valid(J, b) || !n)
        return js_undefined;
    return js_bool(class_update(dom(J), n, a->s, b->s, 0));
}

static jsval classlist_item(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    double want = js_to_number(J, ARG(0));
    if (isnan(want) || isinf(want))
        want = 0;
    want = fmod(trunc(want), 4294967296.0);
    if (want < 0)
        want += 4294967296.0;
    const char *v = n ? node_attr(n, "class") : NULL;
    if (!v)
        v = "";
    const char *cursor = v, *token;
    int len, k = 0;
    while ((token = class_token(v, &cursor, &len)))
        if (k++ == want)
            return js_strn(J, token, len);
    return js_null;
}

static jsval classlist_to_string(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = classlist_node(self);
    const char *v = n ? node_attr(n, "class") : NULL;
    return js_str(J, v ? v : "");
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

static const struct jsclass dataset_class = { "DOMStringMap", dataset_get, dataset_set, NULL };

/* ---- element methods ---- */

static struct node *arg_node(struct js *J, jsval v)
{
    struct node *n = node_of(v);
    if (!n)
        js_throw_error(J, "TypeError: argument is not a node");
    return n;
}

/* What a script element is: 0 data (JSON, templates...), 1 a classic script, 2 a module. */
static int script_kind(const struct node *n)
{
    const char *type = node_attr(n, "type");
    if (!type || !*type)
        return 1;
    if (!strcasecmp(type, "module"))
        return 2;
    return strcasestr(type, "javascript") || strcasestr(type, "ecmascript") ? 1 : 0;
}

static jsval el_append_child(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *p = this_node(J, self), *c = arg_node(J, ARG(0));
    if (!p || !c)
        return js_undefined;
    struct jsdom *d = dom(J);
    int script = is_element(c) && c->tag == T_SCRIPT ? script_kind(c) : 0;
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
            if (script == 2)
                jsdom_run_module(d, d->h.base, 1, c->first->text, c->first->len, -1);
            else
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
    unlink_node(dom(J), c);
    dom(J)->dirty = 1;
    return ARG(0);
}

static jsval el_replace_child(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *p = this_node(J, self), *nw = arg_node(J, ARG(0)), *old = arg_node(J, ARG(1));
    if (!p || !nw || !old || old->parent != p)
        return js_undefined;
    struct node *next = old->next;
    unlink_node(dom(J), old);
    insert_node(dom(J), p, nw, next);
    return ARG(1);
}

static jsval el_remove(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct node *n = this_node(J, self);
    if (n && n->parent) {
        unlink_node(dom(J), n);
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
        unlink_node(d, n);
        d->dirty = 1;
    }
    return js_undefined;
}

static jsval el_append(struct js *J, jsval s, jsval *a, int n) { return insert_items(J, s, a, n, 0); }
static jsval el_prepend(struct js *J, jsval s, jsval *a, int n) { return insert_items(J, s, a, n, 1); }
static jsval el_before(struct js *J, jsval s, jsval *a, int n) { return insert_items(J, s, a, n, 2); }
static jsval el_after(struct js *J, jsval s, jsval *a, int n) { return insert_items(J, s, a, n, 3); }
static jsval el_replace_with(struct js *J, jsval s, jsval *a, int n) { return insert_items(J, s, a, n, 4); }

/* Convert before detaching anything: coercion failures and ancestor cycles
 * must leave the existing children in place. A staging fragment also makes
 * repeated nodes and nodes already among the children retain the right order. */
static jsval el_replace_children(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *parent = this_node(J, self);
    if (!parent)
        return js_undefined;
    if (parent->type != N_ELEMENT)
        return js_throw_error(J, "HierarchyRequestError: a text node cannot have children");
    struct jsdom *d = dom(J);
    struct jsobj *items = js_new_array(J);
    for (int i = 0; i < argc; i++) {
        jsval value = node_of(args[i]) ? args[i] : js_to_string(J, args[i]);
        if (js_throwing(J))
            return js_undefined;
        js_array_push(J, items, value);
    }
    for (int i = 0; i < argc; i++) {
        struct node *child = node_of(items->items[i]);
        if (child && contains(child, parent))
            return js_throw_error(J, "HierarchyRequestError: the new child contains the parent");
    }
    struct node *fragment = new_node(d, N_ELEMENT, "#fragment");
    if (!fragment)
        return js_throw_error(J, "Error: out of memory");
    /* Allocate all text nodes before moving any supplied nodes. */
    for (int i = 0; i < argc; i++)
        if (!node_of(items->items[i])) {
            struct jsstr *text = items->items[i].u.s;
            struct node *child = new_text(d, text->s, text->len);
            if (!child)
                return js_throw_error(J, "Error: out of memory");
            items->items[i] = wrap(d, child);
        }
    for (int i = 0; i < argc; i++)
        insert_node(d, fragment, node_of(items->items[i]), NULL);
    remove_children(d, parent);
    insert_node(d, parent, fragment, NULL);
    return js_undefined;
}

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
    if (deep && n->tag == T_TEMPLATE && n->js) {   /* a template's content went to its fragment */
        struct node *content = node_of(js_get(d->J, js_obj(n->js), "__content"));
        for (const struct node *k = content ? content->first : NULL; k; k = k->next) {
            struct node *kc = clone(d, k, 1);
            if (kc)
                insert_node(d, c, kc, NULL);
        }
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

static jsval el_get_attribute_names(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args, (void)argc;
    struct node *n = this_node(J, self);
    struct jsobj *a = js_new_array(J);
    for (const struct attr *at = n && n->type == N_ELEMENT ? n->attrs : NULL; at; at = at->next)
        js_array_push(J, a, js_str(J, at->name));
    return js_obj(a);
}

static jsval el_has_attributes(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args, (void)argc;
    struct node *n = this_node(J, self);
    return js_bool(n && n->type == N_ELEMENT && n->attrs);
}

/* Node.compareDocumentPosition: DISCONNECTED 1, PRECEDING 2, FOLLOWING 4, CONTAINS 8, CONTAINED_BY 16. */
static jsval el_compare_position(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *a = this_node(J, self), *b = node_of(ARG(0));
    if (!a || !b)
        return js_num(0);
    if (a == b)
        return js_num(0);
    if (contains(a, b))
        return js_num(16 | 4);
    if (contains(b, a))
        return js_num(8 | 2);
    struct node *ra = a, *rb = b;
    while (ra->parent)
        ra = ra->parent;
    while (rb->parent)
        rb = rb->parent;
    if (ra != rb)
        return js_num(1 | 32 | (a < b ? 4 : 2));
    for (struct node *k = ra; k; k = node_next(k, ra)) {
        if (k == a)
            return js_num(4);
        if (k == b)
            return js_num(2);
    }
    return js_num(1);
}

static jsval el_attach_shadow(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d = dom(J);
    struct node *n = this_node(J, self);
    if (!n || n->type != N_ELEMENT || is_fragment(n) || n->tag == T_INPUT || n->tag == T_IMG ||
        n->tag == T_IFRAME || n->tag == T_SCRIPT || n->tag == T_STYLE)
        return js_throw_error(J, "NotSupportedError: element cannot host a shadow root");
    if (n->shadow_root)
        return js_throw_error(J, "NotSupportedError: shadow root already attached");
    const char *mode = ARG(0).t == JS_OBJ ? js_cstr(js_get(J, ARG(0), "mode")) : "";
    if (strcmp(mode, "open") && strcmp(mode, "closed"))
        return js_throw_error(J, "TypeError: shadow mode must be open or closed");
    struct node *root = new_node(d, N_ELEMENT, "#fragment");
    if (!root)
        return js_throw_error(J, "Error: out of memory");
    n->shadow_root = root;
    root->shadow_host = n;
    root->shadow_closed = !strcmp(mode, "closed");
    struct shadow_scope *scope = arena_alloc(d->h.arena, sizeof(*scope));
    if (scope) {
        scope->root = root;
        scope->next = d->shadows;
        d->shadows = scope;
    }
    d->dirty = 1;
    return wrap(d, root);
}

static jsval adopt_sheets(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d = dom(J);
    struct node *root = this_node(J, ARG(0));
    if (!root || (root != d->h.root && !root->shadow_host))
        return js_throw_error(J, "NotSupportedError: sheets require a Document or ShadowRoot");
    struct jsstr *text = js_tostr(J, ARG(1));
    if (!root->adopted_text || strcmp(root->adopted_text, text->s)) {
        root->adopted_text = arena_strndup(d->h.arena, text->s, (size_t)text->len);
        d->dirty = 1;
    }
    return js_undefined;
}
static jsval el_assigned_nodes(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d = dom(J);
    struct node *n = this_node(J, self);
    struct jsobj *list = js_new_array(J);
    if (!n || strcmp(n->name, "slot"))
        return js_obj(list);
    struct node *root = n;
    while (root->parent)
        root = root->parent;
    struct node *host = root->shadow_host;
    const char *name = node_attr(n, "name");
    if (!name)
        name = "";
    if (host && shadow_slot(host, name) == n)
        for (struct node *k = host->first; k; k = k->next) {
            const char *s = node_attr(k, "slot");
            if (!strcmp(s ? s : "", name))
                js_array_push(J, list, wrap(d, k));
        }
    return js_obj(list);
}

static jsval el_get_root_node(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args, (void)argc;
    struct jsdom *d = dom(J);
    struct node *n = this_node(J, self);
    if (!n)
        return js_undefined;
    int composed = ARG(0).t == JS_OBJ && js_truthy(js_get(J, ARG(0), "composed"));
    do {
        while (n->parent)
            n = n->parent;
        if (composed && n->shadow_host)
            n = n->shadow_host;
        else
            break;
    } while (n);
    return n == d->h.root ? js_obj(d->document) : wrap(d, n);
}

static int equal_nodes(const struct node *a, const struct node *b)
{
    if (a->type != b->type)
        return 0;
    if (a->type == N_TEXT)
        return a->len == b->len && !memcmp(a->text, b->text, (size_t)a->len);
    if (strcmp(a->name, b->name))
        return 0;
    int na = 0, nb = 0;
    for (const struct attr *x = a->attrs; x; x = x->next, na++) {
        const char *v = node_attr(b, x->name);
        if (!v || strcmp(v, x->value ? x->value : ""))
            return 0;
    }
    for (const struct attr *x = b->attrs; x; x = x->next)
        nb++;
    if (na != nb)
        return 0;
    const struct node *p = a->first, *q = b->first;
    for (; p && q; p = p->next, q = q->next)
        if (!equal_nodes(p, q))
            return 0;
    return !p && !q;
}

static jsval el_is_equal_node(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *a = this_node(J, self), *b = node_of(ARG(0));
    return js_bool(a && b && equal_nodes(a, b));
}

static jsval el_is_same_node(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *a = this_node(J, self), *b = node_of(ARG(0));
    return js_bool(a && a == b);
}

/* Join adjacent text nodes and drop empty ones. */
static jsval el_normalize(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args, (void)argc;
    struct jsdom *d = dom(J);
    struct node *root = this_node(J, self);
    for (struct node *n = root; n; n = node_next(n, root)) {
        if (n->type != N_ELEMENT)
            continue;
        for (struct node *k = n->first; k;) {
            struct node *next = k->next;
            if (k->type == N_TEXT && next && next->type == N_TEXT) {
                char *t = arena_alloc(d->h.arena, (size_t)(k->len + next->len) + 1);
                if (!t)
                    break;
                memcpy(t, k->text, (size_t)k->len);
                memcpy(t + k->len, next->text, (size_t)next->len);
                k->text = t;
                k->len += next->len;
                unlink_node(d, next);
                d->dirty = 1;
                continue;
            }
            if (k->type == N_TEXT && !k->len) {
                unlink_node(d, k);
                d->dirty = 1;
            }
            k = next;
        }
    }
    return js_undefined;
}

static jsval new_event(struct jsdom *d, const char *type, jsval target);
static void host_unhandled(struct js *J, jsval promise, jsval reason);
static int dispatch_event(struct jsdom *d, struct node *target, const char *type, jsval ev, int bubbles);

/* ---- the top layer: popovers and modal dialogs ----
 *
 * An open popover or modal dialog is in the document's top layer: it is drawn over everything, in the
 * order things went in (node.top_seq), and css.c matches :popover-open and :modal on it. Auto popovers
 * nest: opening one closes the others, except those it is inside (or whose button opened it); a press
 * outside them, or Escape, closes them again (light dismiss). The topmost modal dialog makes the rest
 * of the document inert (the root's top_modal, which web_inert reads). beforetoggle is sent at once
 * (opening can be cancelled), toggle afterwards, once for what happened in between.
 */

static unsigned top_counter;

/* Throw a DOMException named name (InvalidStateError and the like). */
static jsval throw_dom(struct js *J, const char *name, const char *message)
{
    jsval ctor = js_get(J, js_obj(dom(J)->window), "DOMException");
    if (!js_is_callable(ctor))
        return js_throw_error(J, "%s: %s", name, message);
    jsval args[2] = { js_str(J, message), js_str(J, name) };
    jsval e = js_construct(J, ctor, args, 2);
    return e.t == JS_OBJ ? js_throw_value(J, e) : js_throw_error(J, "%s: %s", name, message);
}

/* The popover attribute's state: auto ("" or "auto"), hint, or manual (any other value). */
static int popover_state(const struct node *n)
{
    const char *v = is_element(n) ? node_attr(n, "popover") : NULL;
    if (!v)
        return POP_NONE;
    if (!*v || !strcasecmp(v, "auto"))
        return POP_AUTO;
    return !strcasecmp(v, "hint") ? POP_HINT : POP_MANUAL;
}

static void update_modal(struct jsdom *d)
{
    struct node *top = NULL;
    for (struct node *n = d->h.root; n; n = node_next_shadow(n, d->h.root))
        if (n->type == N_ELEMENT && n->modal && (!top || n->top_seq > top->top_seq))
            top = n;
    d->h.root->top_modal = top;
}

static void toggle_job(struct js *J, struct job *job)
{
    struct jsdom *d = dom(J);
    struct node *n = node_of(job->a);
    jsval ev = js_get(J, job->a, "__toggleEvent");
    js_put(J, job->a.u.o, S(J, "__toggleEvent"), js_undefined, 0);
    if (n && ev.t == JS_OBJ)
        dispatch_event(d, n, "toggle", ev, 0);
}

/* toggle, soon: one event for every change before it is sent, from the first state to the last. */
static void queue_toggle(struct jsdom *d, struct node *n, int opened)
{
    struct js *J = d->J;
    jsval w = wrap(d, n), pending = js_get(J, w, "__toggleEvent");
    if (pending.t == JS_OBJ) {
        js_set(J, pending, "newState", js_str(J, opened ? "open" : "closed"));
        return;
    }
    jsval ev = new_event(d, "toggle", w);
    js_set(J, ev, "bubbles", js_bool(0));
    js_set(J, ev, "cancelable", js_bool(0));
    js_set(J, ev, "oldState", js_str(J, opened ? "closed" : "open"));
    js_set(J, ev, "newState", js_str(J, opened ? "open" : "closed"));
    js_put(J, w.u.o, S(J, "__toggleEvent"), ev, 0);
    js_enqueue(J, toggle_job, w, js_undefined, js_undefined, js_undefined);
}

/* beforetoggle, now; opening can be cancelled (returns 1 if it was). */
static int fire_beforetoggle(struct jsdom *d, struct node *n, int opening, struct node *source)
{
    struct js *J = d->J;
    jsval ev = new_event(d, "beforetoggle", wrap(d, n));
    js_set(J, ev, "bubbles", js_bool(0));
    js_set(J, ev, "cancelable", js_bool(opening));
    js_set(J, ev, "oldState", js_str(J, opening ? "closed" : "open"));
    js_set(J, ev, "newState", js_str(J, opening ? "open" : "closed"));
    js_set(J, ev, "source", source ? wrap(d, source) : js_null);
    return dispatch_event(d, n, "beforetoggle", ev, 0) && opening;
}

/* What gets the focus when a popover or dialog opens: a descendant with autofocus, or (for a
 * dialog) the first that can be focused. */
static struct node *focus_delegate(struct node *root, int autofocus_only)
{
    if (node_attr(root, "autofocus") && web_focusable(root))
        return root;
    for (struct node *k = node_next(root, root); k; k = node_next(k, root))
        if (is_element(k) && node_attr(k, "autofocus") && web_focusable(k))
            return k;
    if (!autofocus_only)
        for (struct node *k = node_next(root, root); k; k = node_next(k, root))
            if (is_element(k) && web_focusable(k))
                return k;
    return NULL;
}

/* Focus moves into what opened; where it was is remembered, for when it closes. */
static void open_focus(struct jsdom *d, struct node *n, int dialog)
{
    struct js *J = d->J;
    jsval w = wrap(d, n);
    js_put(J, w.u.o, S(J, "__prevFocus"), d->active ? wrap(d, d->active) : js_null, 0);
    if (d->h.flush)
        d->h.flush(d->h.ctx);           /* styled now: what was display: none can be focused */
    struct node *f = focus_delegate(n, !dialog);
    if (f)
        jsdom_focus(d, f);
}

static void close_focus(struct jsdom *d, struct node *n, int restore)
{
    struct js *J = d->J;
    jsval w = wrap(d, n), prev = js_get(J, w, "__prevFocus");
    js_put(J, w.u.o, S(J, "__prevFocus"), js_null, 0);
    struct node *p = node_of(prev);
    if (restore && p && (!d->active || contains(n, d->active)) && is_connected(d, p))
        jsdom_focus(d, p);
}

/* The open auto popovers, oldest first (max: room in out); how many. */
static int auto_popovers(struct jsdom *d, struct node **out, int max)
{
    int k = 0;
    for (struct node *n = d->h.root; n && k < max; n = node_next_shadow(n, d->h.root))
        if (n->type == N_ELEMENT && (n->popover_open == POP_AUTO || n->popover_open == POP_HINT))
            out[k++] = n;
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && out[j - 1]->top_seq > out[j]->top_seq; j--) {
            struct node *t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return k;
}

/* The open auto popover n belongs in: the newest one it is inside, or that its button is inside. */
static struct node *popover_ancestor(struct node *n, struct node *invoker)
{
    struct node *best = NULL;
    struct node *starts[2] = { n ? (n->parent ? n->parent : n->shadow_host) : NULL, invoker };
    for (int s = 0; s < 2; s++)
        for (struct node *p = starts[s]; p; p = p->parent ? p->parent : p->shadow_host)
            if (p->type == N_ELEMENT && (p->popover_open == POP_AUTO || p->popover_open == POP_HINT)) {
                if (!best || p->top_seq > best->top_seq)
                    best = p;
                break;
            }
    return best;
}

static int hide_popover(struct jsdom *d, struct node *n, int restore_focus, int fire);

/* Close the auto popovers opened after keep (all of them if keep is NULL), newest first. */
static void hide_popovers_until(struct jsdom *d, struct node *keep, int restore_focus, int fire)
{
    for (int guard = 0; guard < 64; guard++) {
        struct node *open[64];
        int k = auto_popovers(d, open, 64);
        if (!k || open[k - 1] == keep || (keep && !keep->popover_open))
            return;
        hide_popover(d, open[k - 1], restore_focus, fire);
    }
}

static int popover_valid(struct jsdom *d, struct node *n, int opening)
{
    return popover_state(n) && is_connected(d, n) && (n->popover_open != 0) != opening &&
           !(opening && n->tag == T_DIALOG && node_attr(n, "open"));
}

/* Returns 0 if it opened, else why not (1 nothing to do, -1 not allowed). */
static int show_popover(struct jsdom *d, struct node *n, struct node *source)
{
    if (!popover_state(n) || !is_connected(d, n) || (n->tag == T_DIALOG && node_attr(n, "open") && !n->popover_open))
        return -1;
    if (n->popover_open)
        return 1;
    if (fire_beforetoggle(d, n, 1, source) || !popover_valid(d, n, 1))
        return 1;
    int kind = popover_state(n);
    if (kind == POP_AUTO || kind == POP_HINT) {
        hide_popovers_until(d, popover_ancestor(n, source), 0, 1);
        if (!popover_valid(d, n, 1))
            return 1;                   /* a handler of theirs changed it */
    }
    n->popover_open = (uint8_t)kind;
    n->top_seq = ++top_counter;
    n->invoker = source;
    d->dirty = 1;
    open_focus(d, n, 0);
    queue_toggle(d, n, 1);
    return 0;
}

static int hide_popover(struct jsdom *d, struct node *n, int restore_focus, int fire)
{
    if (!n->popover_open)
        return 1;
    if (n->popover_open == POP_AUTO || n->popover_open == POP_HINT) {
        hide_popovers_until(d, n, restore_focus, fire);        /* the ones it opened go first */
        if (!n->popover_open)
            return 1;
    }
    if (fire) {
        fire_beforetoggle(d, n, 0, NULL);
        if (!n->popover_open)
            return 1;
    }
    n->popover_open = 0;
    n->invoker = NULL;
    d->dirty = 1;
    if (fire)
        queue_toggle(d, n, 0);
    close_focus(d, n, restore_focus);
    return 0;
}

static struct node *source_option(struct js *J, jsval opts)
{
    return opts.t == JS_OBJ && !node_of(opts) ? node_of(js_get(J, opts, "source")) : NULL;
}

static jsval el_show_popover(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (n && show_popover(dom(J), n, source_option(J, ARG(0))) < 0)
        return popover_state(n) ? throw_dom(J, "InvalidStateError", "The popover cannot be shown")
                                : throw_dom(J, "NotSupportedError", "The element is not a popover");
    return js_undefined;
}

static jsval el_hide_popover(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (n && !popover_state(n))
        return throw_dom(J, "NotSupportedError", "The element is not a popover");
    if (n)
        hide_popover(dom(J), n, 1, 1);
    return js_undefined;
}

/* togglePopover(force) or togglePopover({ force, source }): whether it is open afterwards. */
static jsval el_toggle_popover(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (!n)
        return js_bool(0);
    if (!popover_state(n))
        return throw_dom(J, "NotSupportedError", "The element is not a popover");
    jsval force = ARG(0);
    struct node *source = source_option(J, force);
    if (force.t == JS_OBJ)
        force = js_get(J, force, "force");
    int open = force.t == JS_UNDEF ? !n->popover_open : js_truthy(force);
    if (open && !n->popover_open && show_popover(dom(J), n, source) < 0)
        return throw_dom(J, "InvalidStateError", "The popover cannot be shown");
    if (!open && n->popover_open)
        hide_popover(dom(J), n, 1, 1);
    return js_bool(n->popover_open != 0);
}

/* <dialog>: show(), showModal(), close(returnValue) and requestClose(returnValue). */
static int dialog_open(struct jsdom *d, struct node *n, int modal)
{
    if (node_attr(n, "open"))
        return !modal || n->modal ? 0 : -1;
    if (n->popover_open || (modal && !is_connected(d, n)))
        return -1;
    if (fire_beforetoggle(d, n, 1, NULL) || node_attr(n, "open"))
        return 0;
    set_attr(d, n, "open", "");
    if (modal) {
        n->modal = 1;
        n->top_seq = ++top_counter;
        update_modal(d);
        hide_popovers_until(d, popover_ancestor(n, NULL), 0, 1);
    }
    open_focus(d, n, 1);
    queue_toggle(d, n, 1);
    return 0;
}

static jsval dialog_show(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args, (void)argc;
    struct node *n = this_node(J, self);
    if (n && dialog_open(dom(J), n, 0) < 0)
        return throw_dom(J, "InvalidStateError", "The dialog is open as a modal one, or is a popover");
    return js_undefined;
}

static jsval dialog_show_modal(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args, (void)argc;
    struct node *n = this_node(J, self);
    if (n && dialog_open(dom(J), n, 1) < 0)
        return throw_dom(J, "InvalidStateError", "The dialog cannot be shown as a modal one");
    return js_undefined;
}

static void dialog_close_with(struct jsdom *d, struct node *n, jsval value)
{
    struct js *J = d->J;
    if (!node_attr(n, "open"))
        return;
    fire_beforetoggle(d, n, 0, NULL);
    if (!node_attr(n, "open"))
        return;
    int was_modal = n->modal;
    n->modal = 0;
    remove_attr(d, n, "open");
    if (was_modal)
        update_modal(d);
    jsval w = wrap(d, n);
    if (value.t != JS_UNDEF && value.t != JS_NULL)
        js_set(J, w, "returnValue", js_to_string(J, value));
    queue_toggle(d, n, 0);
    close_focus(d, n, 1);
    jsval ev = new_event(d, "close", w);
    js_set(J, ev, "bubbles", js_bool(0));
    js_set(J, ev, "cancelable", js_bool(0));
    dispatch_event(d, n, "close", ev, 0);
}

static jsval dialog_close(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (n)
        dialog_close_with(dom(J), n, ARG(0));
    return js_undefined;
}

/* A request to close (Escape, requestClose()): cancel first, which a handler may prevent. */
static int dialog_request_close(struct jsdom *d, struct node *n, jsval value)
{
    if (!node_attr(n, "open"))
        return 0;
    jsval ev = new_event(d, "cancel", wrap(d, n));
    js_set(d->J, ev, "bubbles", js_bool(0));
    if (!dispatch_event(d, n, "cancel", ev, 0))
        dialog_close_with(d, n, value);
    return 1;
}

static jsval dialog_request_close_api(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    if (n)
        dialog_request_close(dom(J), n, ARG(0));
    return js_undefined;
}

/* closedby: what closes a dialog besides close(): "any" (light dismiss too), "closerequest" (Escape),
 * or "none"; a modal dialog's Escape closes it unless it says none. */
static int closed_by(const struct node *n, const char *want)
{
    const char *v = node_attr(n, "closedby");
    if (!v || (strcasecmp(v, "any") && strcasecmp(v, "closerequest") && strcasecmp(v, "none")))
        v = n->modal ? "closerequest" : "none";
    if (!strcasecmp(want, "closerequest"))
        return strcasecmp(v, "none") != 0;
    return !strcasecmp(v, want);
}

/* The element left the document, or stopped being a popover: out of the top layer, without events. */
static void top_layer_removed(struct jsdom *d, struct node *root)
{
    int modal = 0;
    for (struct node *k = root; k; k = node_next_shadow(k, root)) {
        if (k->type != N_ELEMENT)
            continue;
        if (k->popover_open) {
            k->popover_open = 0;
            k->invoker = NULL;
            d->dirty = 1;
        }
        if (k->modal) {
            k->modal = 0;
            modal = 1;
            d->dirty = 1;
        }
    }
    if (modal)
        update_modal(d);
}

/* popover's state changed: an open popover closes (without events); open taken off a modal dialog
 * takes it out of the top layer. */
static void top_layer_attr(struct jsdom *d, struct node *n, const char *name, const char *old, const char *value)
{
    if (!strcmp(name, "popover") && n->popover_open) {
        struct node probe = { 0 };
        struct attr a = { "popover", (char *)old, NULL };
        probe.type = N_ELEMENT;
        probe.attrs = old ? &a : NULL;
        if (popover_state(&probe) != popover_state(n))
            top_layer_removed(d, n);
    } else if (!strcmp(name, "open") && !value && n->modal) {
        n->modal = 0;
        update_modal(d);
        d->dirty = 1;
    }
}

/* An invoker button: popovertarget (popovertargetaction toggle, show or hide), or commandfor with a
 * command (show-modal, close, request-close, show-popover, hide-popover, toggle-popover, or a --custom
 * one, which only sends the target a command event). A click's activation, after the click event. */
static struct node *invoker_target(struct jsdom *d, struct node *n, const char *attr)
{
    const char *id = node_attr(n, attr);
    return id && *id ? by_id(d->h.root, id) : NULL;
}

static void invoke(struct jsdom *d, struct node *target)
{
    struct node *n = target;
    while (n && !(is_element(n) && (n->tag == T_BUTTON || n->tag == T_INPUT)))
        n = n->parent;
    if (!n || web_disabled(n) || web_inert(n))
        return;
    const char *type = node_attr(n, "type");
    if (n->tag == T_INPUT && (!type || (strcasecmp(type, "button") && strcasecmp(type, "submit") &&
                                        strcasecmp(type, "reset") && strcasecmp(type, "image"))))
        return;
    int submit = n->tag == T_BUTTON ? !type || !strcasecmp(type, "submit") : !strcasecmp(type, "submit") ||
                 !strcasecmp(type, "image");
    struct node *form = NULL;
    for (struct node *p = n->parent; p && !form; p = p->parent)
        if (p->tag == T_FORM)
            form = p;
    if (submit && (form || node_attr(n, "form")))
        return;                         /* a form's submit button submits it instead */
    struct js *J = d->J;
    struct node *t = n->tag == T_BUTTON ? invoker_target(d, n, "commandfor") : NULL;
    const char *command = node_attr(n, "command");
    if (t && command && *command) {
        jsval ev = new_event(d, "command", wrap(d, t));
        js_set(J, ev, "bubbles", js_bool(0));
        js_set(J, ev, "command", js_str(J, command));
        js_set(J, ev, "source", wrap(d, n));
        if (dispatch_event(d, t, "command", ev, 0) || !is_connected(d, t))
            return;
        if (!strcasecmp(command, "toggle-popover") && popover_state(t)) {
            if (t->popover_open)
                hide_popover(d, t, 1, 1);
            else
                show_popover(d, t, n);
        } else if (!strcasecmp(command, "show-popover") && popover_state(t)) {
            show_popover(d, t, n);
        } else if (!strcasecmp(command, "hide-popover") && popover_state(t)) {
            hide_popover(d, t, 1, 1);
        } else if (t->tag == T_DIALOG && !strcasecmp(command, "show-modal")) {
            dialog_open(d, t, 1);
        } else if (t->tag == T_DIALOG && !strcasecmp(command, "close")) {
            const char *v = node_attr(n, "value");
            dialog_close_with(d, t, v ? js_str(J, v) : js_undefined);
        } else if (t->tag == T_DIALOG && !strcasecmp(command, "request-close")) {
            const char *v = node_attr(n, "value");
            dialog_request_close(d, t, v ? js_str(J, v) : js_undefined);
        }
        return;
    }
    t = invoker_target(d, n, "popovertarget");
    if (!t || !popover_state(t))
        return;
    const char *action = node_attr(n, "popovertargetaction");
    int show = action && !strcasecmp(action, "show"), hide = action && !strcasecmp(action, "hide");
    if (t->popover_open && !show)
        hide_popover(d, t, 1, 1);
    else if (!t->popover_open && !hide)
        show_popover(d, t, n);
}

/* The newest of the top layer's auto popovers and modal dialogs: what Escape closes. */
static struct node *top_closable(struct jsdom *d)
{
    struct node *top = NULL;
    for (struct node *n = d->h.root; n; n = node_next_shadow(n, d->h.root))
        if (n->type == N_ELEMENT && (n->popover_open == POP_AUTO || n->popover_open == POP_HINT ||
                                     (n->tag == T_DIALOG && node_attr(n, "open") && closed_by(n, "closerequest"))) &&
            (!top || n->top_seq > top->top_seq))
            top = n;
    return top;
}

int jsdom_dialog_submit(struct jsdom *d, struct node *form, struct node *submitter)
{
    if (!d || !form)
        return 0;
    const char *method = submitter ? node_attr(submitter, "formmethod") : NULL;
    if (!method)
        method = node_attr(form, "method");
    if (!method || strcasecmp(method, "dialog"))
        return 0;
    struct node *dlg = form;
    while (dlg && dlg->tag != T_DIALOG)
        dlg = dlg->parent;
    if (dlg && node_attr(dlg, "open")) {
        const char *v = submitter ? node_attr(submitter, "value") : NULL;
        d->depth++;
        dialog_close_with(d, dlg, v ? js_str(d->J, v) : js_undefined);
        d->depth--;
        js_run_jobs(d->J);
        settle(d);
    }
    return 1;
}

int jsdom_close_request(struct jsdom *d)
{
    if (!d)
        return 0;
    struct node *top = top_closable(d);
    if (!top)
        return 0;
    d->depth++;
    if (top->popover_open)
        hide_popover(d, top, 1, 1);
    else
        dialog_request_close(d, top, js_undefined);
    d->depth--;
    js_run_jobs(d->J);
    settle(d);
    return 1;
}

void jsdom_light_dismiss(struct jsdom *d, struct node *target, int x, int y)
{
    if (!d)
        return;
    struct node *open[64];
    int nopen = auto_popovers(d, open, 64);
    struct node *dlg = d->h.root->top_modal;
    int bx, by, bw, bh;
    int on_backdrop = dlg && target == dlg && closed_by(dlg, "any") && d->h.node_box &&
                      d->h.node_box(d->h.ctx, dlg, &bx, &by, &bw, &bh) &&
                      (x < bx || y < by || x >= bx + bw || y >= by + bh);
    if (!nopen && !on_backdrop)
        return;
    d->depth++;
    if (nopen) {
        /* the popover pressed in (or the one the pressed button opens) stays, with those under it */
        struct node *keep = NULL;
        for (struct node *p = target; p; p = p->parent ? p->parent : p->shadow_host)
            if (p->type == N_ELEMENT && (p->popover_open == POP_AUTO || p->popover_open == POP_HINT)) {
                keep = p;
                break;
            }
        for (struct node *p = target; p; p = p->parent)
            if (is_element(p) && (p->tag == T_BUTTON || p->tag == T_INPUT)) {
                struct node *t = invoker_target(d, p, "popovertarget");
                if (!t)
                    t = invoker_target(d, p, "commandfor");
                if (t && t->popover_open && (!keep || t->top_seq > keep->top_seq))
                    keep = t;
                break;
            }
        hide_popovers_until(d, keep, 0, 1);
    } else {
        dialog_request_close(d, dlg, js_undefined);
    }
    d->depth--;
    js_run_jobs(d->J);
    settle(d);
}

/* form.submit(), requestSubmit() and reset() */
static jsval form_submit(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args, (void)argc;
    struct jsdom *d = dom(J);
    struct node *n = this_node(J, self);
    if (n && n->tag == T_FORM && d->h.submit)
        d->h.submit(d->h.ctx, n, 0);
    return js_undefined;
}

static jsval form_request_submit(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args, (void)argc;
    struct jsdom *d = dom(J);
    struct node *n = this_node(J, self);
    jsval ok = js_get(J, self, "checkValidity");
    if (js_is_callable(ok) && !js_truthy(js_call_value(J, ok, self, NULL, 0)))
        return js_undefined;
    if (n && n->tag == T_FORM && d->h.submit)
        d->h.submit(d->h.ctx, n, 1);
    return js_undefined;
}

static jsval form_reset(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args, (void)argc;
    struct jsdom *d = dom(J);
    struct node *form = this_node(J, self);
    for (struct node *n = form; n; n = node_next(n, form)) {
        if (n->type != N_ELEMENT)
            continue;
        if (n->tag == T_INPUT) {
            const char *v = is_file_input(n) ? NULL : node_attr(n, "value");
            free(n->value);
            n->value = strdup(v ? v : "");
            n->file_chosen = 0;
            n->checked = node_attr(n, "checked") != NULL;
        } else if (n->tag == T_TEXTAREA) {
            free(n->value);
            n->value = strdup(n->first && n->first->type == N_TEXT ? n->first->text : "");
        } else if (n->tag == T_SELECT) {
            n->checked = 0;
        }
    }
    d->dirty = 1;
    return js_undefined;
}

static jsval doc_import_node(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct node *n = arg_node(J, ARG(0));
    return n ? wrap(dom(J), clone(dom(J), n, js_truthy(ARG(1)))) : js_undefined;
}

static jsval doc_adopt_node(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct node *n = arg_node(J, ARG(0));
    if (n && n->parent)
        unlink_node(dom(J), n);
    return ARG(0);
}

/* document.elementFromPoint(x, y), in the viewport's coordinates. */
static jsval doc_element_from_point(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    double x = js_to_number(J, ARG(0)), y = js_to_number(J, ARG(1));
    int sx = 0, sy = 0;
    if (!d->h.node_at || x != x || y != y || x < 0 || y < 0)
        return js_null;
    if (d->h.scroll_pos)
        d->h.scroll_pos(d->h.ctx, &sx, &sy);
    struct node *n = d->h.node_at(d->h.ctx, (int)x + sx, (int)y + sy);
    return n ? wrap(d, n) : wrap(d, node_find(d->h.root, T_BODY));
}

static jsval el_blur(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d=dom(J);
    if(node_of(self)==d->active)jsdom_focus(d,NULL);
    return js_undefined;
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
    if (!strcasecmp(name->s, "value") && n->tag == T_INPUT && !is_file_input(n)) {
        free(n->value);
        n->value = strdup(value->s);
    }
    if ((!strcasecmp(name->s, "src") || !strcasecmp(name->s, "srcset")) && n->tag == T_IMG && dom(J)->h.load_image)
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

/* Return 1 for a target, 0 for a detached exterior position, -1 on error. */
static int adjacent_target(struct js *J, struct node *n, const char *where,
                           struct node **parent, struct node **ref)
{
    if (!strcasecmp(where, "beforebegin")) {
        *parent = n->parent; *ref = n;
    } else if (!strcasecmp(where, "afterbegin")) {
        *parent = n; *ref = n->first;
    } else if (!strcasecmp(where, "beforeend")) {
        *parent = n; *ref = NULL;
    } else if (!strcasecmp(where, "afterend")) {
        *parent = n->parent; *ref = n->next;
    } else {
        js_throw_error(J, "SyntaxError: invalid adjacent insertion position");
        return -1;
    }
    return *parent != NULL;
}

static jsval el_insert_adjacent_text(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self), *parent, *ref;
    if (!n)
        return js_undefined;
    struct jsstr *where = js_tostr(J, ARG(0));
    if (js_throwing(J))
        return js_undefined;
    struct jsstr *text = js_tostr(J, ARG(1));
    if (js_throwing(J) || adjacent_target(J, n, where->s, &parent, &ref) <= 0)
        return js_undefined;
    struct node *child = new_text(dom(J), text->s, text->len);
    if (!child)
        return js_throw_error(J, "Error: out of memory");
    insert_node(dom(J), parent, child, ref);
    return js_undefined;
}

static jsval el_insert_adjacent_element(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self), *parent, *ref;
    if (!n)
        return js_undefined;
    struct jsstr *where = js_tostr(J, ARG(0));
    if (js_throwing(J))
        return js_undefined;
    struct node *child = node_of(ARG(1));
    if (!is_element(child) || is_fragment(child))
        return js_throw_error(J, "TypeError: insertAdjacentElement requires an element");
    int target = adjacent_target(J, n, where->s, &parent, &ref);
    if (target < 0)
        return js_undefined;
    if (!target)
        return js_null;
    if (insert_node(dom(J), parent, child, ref) < 0)
        return js_throw_error(J, "HierarchyRequestError: the new child contains the parent");
    return ARG(1);
}

void jsdom_focus(struct jsdom *d, struct node *n)
{
    if (!d || (n && (!is_connected(d,n) || !web_focusable(n))) || d->active == n) return;
    struct node *old = d->active;
    d->active = n;
    if (old) { jsdom_dispatch(d,old,"blur"); jsdom_dispatch(d,old,"focusout"); }
    if (d->h.focus) d->h.focus(d->h.ctx,n);
    if (n) { jsdom_dispatch(d,n,"focus"); jsdom_dispatch(d,n,"focusin"); }
}
static jsval el_focus(struct js *J, jsval self, jsval *args, int argc)
{
    jsdom_focus(dom(J), this_node(J,self));
    return js_undefined;
}

static jsval el_scroll_into_view(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct node *n = this_node(J, self);
    struct jsdom *d = dom(J);
    int x, y, w, h;
    if (n && d->h.reveal)
        d->h.reveal(d->h.ctx, n);
    else if (n && d->h.node_box && d->h.node_box(d->h.ctx, n, &x, &y, &w, &h) && d->h.scroll)
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

static void invoke(struct jsdom *d, struct node *target);

static jsval el_click(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct node *n = this_node(J, self);
    if (n && !web_inert(n) && !web_disabled(n) && !jsdom_dispatch(dom(J), n, "click"))
        invoke(dom(J), n);
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

/* ---- events ----
 *
 * A target's listeners are kept in its hidden __listeners object, by type:
 * arrays of entries { f, c, o }: the listener (a function, or an object
 * with handleEvent), whether it is for the capture phase, and whether it
 * goes after one call. Dispatching runs the capture phase from the window
 * down, the target, then (if the event bubbles) back up to the window.
 */

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

static int capture_option(struct js *J, jsval opts)
{
    if (opts.t == JS_OBJ)
        return js_truthy(js_get(J, opts, "capture"));
    return js_truthy(opts);
}

static jsval add_event_listener(struct js *J, jsval self, jsval *args, int argc)
{
    jsval t = target_of(J, self), f = ARG(1), opts = ARG(2);
    if (t.t != JS_OBJ || f.t != JS_OBJ)
        return js_undefined;
    int capture = capture_option(J, opts), once = 0;
    jsval signal = js_undefined;
    if (opts.t == JS_OBJ) {
        once = js_truthy(js_get(J, opts, "once"));
        signal = js_get(J, opts, "signal");
        if (signal.t == JS_OBJ && js_truthy(js_get(J, signal, "aborted")))
            return js_undefined;
    }
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
    for (int i = 0; i < list.u.o->len; i++) {
        jsval e = list.u.o->items[i];
        if (js_strict_equal(js_get(J, e, "f"), f) && js_truthy(js_get(J, e, "c")) == capture)
            return js_undefined;
    }
    jsval e = js_obj(js_new_object(J));
    js_set(J, e, "f", f);
    js_set(J, e, "c", js_bool(capture));
    js_set(J, e, "o", js_bool(once));
    js_array_push(J, list.u.o, e);
    /* { signal }: aborting it removes the listener */
    jsval helper = js_get(J, js_obj(J->global), "%onAbort");
    if (signal.t == JS_OBJ && js_is_callable(helper)) {
        jsval tv;
        tv.t = JS_STR;
        tv.u.s = type;
        jsval a[5] = { signal, t, tv, f, js_bool(capture) };
        js_call_value(J, helper, js_undefined, a, 5);
    }
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
    int capture = capture_option(J, ARG(2));
    struct jsobj *a = list.u.o;
    for (int i = 0; i < a->len; i++) {
        jsval e = a->items[i];
        if (js_strict_equal(js_get(J, e, "f"), ARG(1)) && js_truthy(js_get(J, e, "c")) == capture) {
            js_set(J, e, "r", js_bool(1));      /* a dispatch under way skips it */
            memmove(a->items + i, a->items + i + 1, sizeof(jsval) * (size_t)(a->len - i - 1));
            a->len--;
            break;
        }
    }
    return js_undefined;
}

static jsval event_prevent_default(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    if (self.t == JS_OBJ && js_truthy(js_get(J, self, "cancelable")) && !js_truthy(js_get(J, self, "__passive")))
        js_put(J, self.u.o, S(J, "defaultPrevented"), js_bool(1), 0);
    if (self.t == JS_OBJ)
        js_put(J, self.u.o, S(J, "returnValue"), js_bool(0), 0);
    return js_undefined;
}

static jsval event_stop_propagation(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    if (self.t == JS_OBJ) {
        js_put(J, self.u.o, S(J, "__stopped"), js_bool(1), 1);
        js_put(J, self.u.o, S(J, "cancelBubble"), js_bool(1), 0);
    }
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
    if (argc > 3)
        js_set(J, self, "detail", ARG(3));      /* initCustomEvent */
    return js_undefined;
}

/* Slotted nodes enter their assigned slot before climbing the shadow tree. */
static struct node *event_parent(struct node *n)
{
    if (n->parent && n->parent->shadow_root) {
        const char *name = n->type == N_ELEMENT ? node_attr(n, "slot") : "";
        struct node *slot = shadow_slot(n->parent, name ? name : "");
        if (slot) return slot;
    }
    return n->parent;
}

/* The targets an event goes through: the target, its ancestors, the document and the window. */
static jsval event_composed_path(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d = dom(J);
    struct jsobj *a = js_new_array(J);
    struct dispatch_path *path = d->dispatch_path;
    while (path && (self.t != JS_OBJ || path->event.u.o != self.u.o))
        path = path->prev;
    if (!path)
        return js_obj(a);
    struct node *current = node_of(js_get(J, self, "currentTarget"));
    struct node *start = path->target;
    for (struct node *p = start; p; p = p->parent ? p->parent : p->shadow_host)
        if (p->shadow_host && p->shadow_closed && !contains(p, current))
            start = p->shadow_host;
    int composed = js_truthy(js_get(J, self, "composed")), connected = 0;
    for (struct node *n = start; n;) {
        int visible = 1;
        for (struct node *r = n; r; r = r->parent)
            if (r->shadow_host && r->shadow_closed && !contains(r, current)) visible = 0;
        if (visible) js_array_push(J, a, wrap(d, n));
        if (n == d->h.root)
            connected = 1;
        struct node *parent = event_parent(n);
        n = parent ? parent : n->shadow_host && (composed || contains(n->shadow_host, path->target))
            ? n->shadow_host : NULL;
    }
    if (connected) {
        js_array_push(J, a, js_obj(d->document));
        js_array_push(J, a, js_obj(d->window));
    }
    return js_obj(a);
}

/* The kinds of event, each with its prototype: what an event's type makes it. */
#define EVENT_KINDS(X) \
    X(EV_EVENT, "Event", -1) X(EV_UI, "UIEvent", EV_EVENT) X(EV_MOUSE, "MouseEvent", EV_UI) \
    X(EV_POINTER, "PointerEvent", EV_MOUSE) X(EV_WHEEL, "WheelEvent", EV_MOUSE) \
    X(EV_DRAG, "DragEvent", EV_MOUSE) X(EV_KEYBOARD, "KeyboardEvent", EV_UI) \
    X(EV_FOCUS, "FocusEvent", EV_UI) X(EV_INPUT, "InputEvent", EV_UI) \
    X(EV_COMPOSITION, "CompositionEvent", EV_UI) X(EV_TOUCH, "TouchEvent", EV_UI) \
    X(EV_CUSTOM, "CustomEvent", EV_EVENT) X(EV_SUBMIT, "SubmitEvent", EV_EVENT) \
    X(EV_PROGRESS, "ProgressEvent", EV_EVENT) X(EV_ERROR, "ErrorEvent", EV_EVENT) \
    X(EV_MESSAGE, "MessageEvent", EV_EVENT) X(EV_POPSTATE, "PopStateEvent", EV_EVENT) \
    X(EV_HASHCHANGE, "HashChangeEvent", EV_EVENT) X(EV_ANIMATION, "AnimationEvent", EV_EVENT) \
    X(EV_TRANSITION, "TransitionEvent", EV_EVENT) X(EV_CLIPBOARD, "ClipboardEvent", EV_EVENT) \
    X(EV_STORAGE, "StorageEvent", EV_EVENT) X(EV_PAGETRANSITION, "PageTransitionEvent", EV_EVENT) \
    X(EV_BEFOREUNLOAD, "BeforeUnloadEvent", EV_EVENT) X(EV_TOGGLE, "ToggleEvent", EV_EVENT) \
    X(EV_COMMAND, "CommandEvent", EV_EVENT)

enum {
#define X(id, name, parent) id,
    EVENT_KINDS(X)
#undef X
    EV_COUNT
};

static int event_kind(const char *type)
{
    static const struct { const char *type; int kind; } kinds[] = {
        { "click", EV_MOUSE }, { "dblclick", EV_MOUSE }, { "mousedown", EV_MOUSE }, { "mouseup", EV_MOUSE },
        { "mousemove", EV_MOUSE }, { "mouseover", EV_MOUSE }, { "mouseout", EV_MOUSE },
        { "mouseenter", EV_MOUSE }, { "mouseleave", EV_MOUSE }, { "contextmenu", EV_MOUSE },
        { "auxclick", EV_POINTER }, { "pointerdown", EV_POINTER }, { "pointerup", EV_POINTER },
        { "pointermove", EV_POINTER }, { "pointerover", EV_POINTER }, { "pointerout", EV_POINTER },
        { "pointerenter", EV_POINTER }, { "pointerleave", EV_POINTER }, { "pointercancel", EV_POINTER },
        { "gotpointercapture", EV_POINTER }, { "lostpointercapture", EV_POINTER },
        { "wheel", EV_WHEEL }, { "keydown", EV_KEYBOARD }, { "keyup", EV_KEYBOARD },
        { "keypress", EV_KEYBOARD }, { "focus", EV_FOCUS }, { "blur", EV_FOCUS }, { "focusin", EV_FOCUS },
        { "focusout", EV_FOCUS }, { "input", EV_INPUT }, { "beforeinput", EV_INPUT },
        { "submit", EV_SUBMIT }, { "popstate", EV_POPSTATE }, { "hashchange", EV_HASHCHANGE },
        { "message", EV_MESSAGE }, { "toggle", EV_TOGGLE }, { "beforetoggle", EV_TOGGLE }, { "command", EV_COMMAND },
        { "dragstart", EV_DRAG }, { "drag", EV_DRAG }, { "dragenter", EV_DRAG }, { "dragover", EV_DRAG },
        { "dragleave", EV_DRAG }, { "drop", EV_DRAG }, { "dragend", EV_DRAG },
    };
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (!strcmp(kinds[i].type, type))
            return kinds[i].kind;
    return EV_EVENT;
}

static jsval new_event_kind(struct jsdom *d, const char *type, jsval target, int kind)
{
    struct js *J = d->J;
    struct jsobj *e = js_mkobj(J, C_OBJECT, d->ev_protos[kind]);
    jsval ev = js_obj(e);
    js_set(J, ev, "type", js_str(J, type));
    js_set(J, ev, "target", target);
    js_set(J, ev, "srcElement", target);
    js_set(J, ev, "currentTarget", target);
    js_set(J, ev, "bubbles", js_bool(1));
    js_set(J, ev, "cancelable", js_bool(1));
    js_set(J, ev, "defaultPrevented", js_bool(0));
    js_set(J, ev, "isTrusted", js_bool(1));
    js_set(J, ev, "timeStamp", js_num(uptime_ms()));      /* the same clock as performance.now() */
    js_set(J, ev, "eventPhase", js_num(0));
    return ev;
}

static jsval new_event(struct jsdom *d, const char *type, jsval target)
{
    return new_event_kind(d, type, target, event_kind(type));
}

/* Run one object's listeners for an event in a phase (1 capture, 2 at the target, 3 bubbling),
   and its on-handler outside the capture phase. */
static void fire_phase(struct jsdom *d, jsval obj, struct node *n, const char *type, jsval ev, int phase)
{
    struct js *J = d->J;
    js_set(J, ev, "currentTarget", obj);
    js_set(J, ev, "eventPhase", js_num(phase));
    struct prop *p = js_own(obj.u.o, listener_key(J));
    if (p) {
        jsval list = js_get(J, p->v, type);
        if (js_is_array(list) && list.u.o->len) {
            int count = list.u.o->len;
            jsval *copy = js_alloc(J, sizeof(jsval) * (size_t)count);
            memcpy(copy, list.u.o->items, sizeof(jsval) * (size_t)count);
            for (int i = 0; i < count; i++) {
                jsval e = copy[i], r;
                int capture = js_truthy(js_get(J, e, "c"));
                if ((phase == 1 && !capture) || (phase == 3 && capture) || js_truthy(js_get(J, e, "r")))
                    continue;
                jsval f = js_get(J, e, "f");
                if (js_truthy(js_get(J, e, "o"))) {
                    jsval a[3] = { js_str(J, type), f, js_bool(capture) };
                    remove_event_listener(J, obj, a, 3);
                }
                jsval self = obj;
                if (!js_is_callable(f)) {           /* { handleEvent(e) { ... } } */
                    self = f;
                    f = js_get(J, f, "handleEvent");
                }
                if (js_is_callable(f) && js_call(J, f, self, &ev, 1, &r) < 0)
                    report(d, type);
                if (js_truthy(js_get(J, ev, "__stopnow")))
                    return;
            }
        }
    }
    if (phase == 1)
        return;
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
            event_prevent_default(J, ev, NULL, 0);
    }
}

static void fire_on(struct jsdom *d, jsval obj, struct node *n, const char *type, jsval ev)
{
    fire_phase(d, obj, n, type, ev, 2);
}

/* Could n have listeners or a handler attribute for an event? */
static int may_listen(const struct node *n, const char *on)
{
    return n->js || (n->type == N_ELEMENT && n->attrs && node_attr(n, on));
}

static int dispatch_event(struct jsdom *d, struct node *target, const char *type, jsval ev, int bubbles)
{
    struct js *J = d->J;
    struct node *path[256], *visible[256], *retarget = target;
    int np = 0, in_doc = target == d->h.root, composed = js_truthy(js_get(J, ev, "composed"));
    for (struct node *n = event_parent(target); n && np < 256;) {
        path[np] = n;
        visible[np++] = retarget;
        if (n == d->h.root)
            in_doc = 1;
        struct node *parent = event_parent(n);
        if (parent)
            n = parent;
        else if (n->shadow_host && (composed || contains(n->shadow_host, target))) {
            if (contains(n, target)) retarget = n->shadow_host;
            n = n->shadow_host;
        } else
            n = NULL;
    }
    struct dispatch_path live = {ev, target, d->dispatch_path};
    d->dispatch_path = &live;
    char on[40];
    snprintf(on, sizeof(on), "on%s", type);
    d->depth++;
    js_set(J, ev, "__dispatching", js_bool(1));
#define STOPPED() js_truthy(js_get(J, ev, "__stopped"))
    /* capture: the window and the document, then down to the target's parent */
    if (in_doc) {
        js_set(J, ev, "target", wrap(d, retarget));
        fire_phase(d, js_obj(d->window), NULL, type, ev, 1);
        if (!STOPPED())
            fire_phase(d, js_obj(d->document), NULL, type, ev, 1);
    }
    for (int i = np - 1; i >= 0 && !STOPPED(); i--)
        if (path[i]->js) {
            js_set(J, ev, "target", wrap(d, visible[i]));
            fire_phase(d, wrap(d, path[i]), path[i], type, ev, 1);
        }
    js_set(J, ev, "target", wrap(d, target));
    if (!STOPPED() && may_listen(target, on))
        fire_phase(d, wrap(d, target), target, type, ev, 2);
    if (bubbles) {
        for (int i = 0; i < np && !STOPPED(); i++)
            if (may_listen(path[i], on)) {
                js_set(J, ev, "target", wrap(d, visible[i]));
                fire_phase(d, wrap(d, path[i]), path[i], type, ev, 3);
            }
        js_set(J, ev, "target", wrap(d, retarget));
        if (in_doc && !STOPPED())
            fire_phase(d, js_obj(d->document), NULL, type, ev, 3);
        if (in_doc && !STOPPED())
            fire_phase(d, js_obj(d->window), NULL, type, ev, 3);
    }
#undef STOPPED
    js_set(J, ev, "__dispatching", js_bool(0));
    js_set(J, ev, "eventPhase", js_num(0));
    js_set(J, ev, "currentTarget", js_null);
    d->dispatch_path = live.prev;
    d->depth--;
    int prevented = js_truthy(js_get(J, ev, "defaultPrevented"));
    settle(d);
    return prevented;
}

static void slotchange_job(struct js *J, struct job *job)
{
    struct jsdom *d = dom(J);
    struct node *slot = node_of(job->a);
    if (!slot) return;
    slot->slot_pending = 0;
    jsval event = new_event(d, "slotchange", job->a);
    js_set(J, event, "composed", js_bool(0));
    js_set(J, event, "cancelable", js_bool(0));
    dispatch_event(d, slot, "slotchange", event, 1);
}

/* Reassign after every mutation, then signal each changed slot once per
 * microtask checkpoint. The snapshot also covers transient assignments. */
static void assign_slots(struct jsdom *d)
{
    for (struct shadow_scope *scope = d->shadows; scope; scope = scope->next) {
        struct node *root = scope->root, *host = root->shadow_host;
        for (struct node *slot = root; slot; slot = node_next(slot, root)) {
            if (slot->type != N_ELEMENT || strcmp(slot->name, "slot")) continue;
            const char *name = node_attr(slot, "name");
            if (!name) name = "";
            int count = 0, changed = 0;
            if (shadow_slot(host, name) == slot)
                for (struct node *n = host->first; n; n = n->next) {
                    const char *s = n->type == N_ELEMENT ? node_attr(n, "slot") : "";
                    if (strcmp(s ? s : "", name)) continue;
                    if (count >= slot->assigned_count || slot->assigned_nodes[count] != n) changed = 1;
                    count++;
                }
            changed |= count != slot->assigned_count;
            if (!changed) continue;
            struct node **nodes = count ? arena_alloc(d->h.arena, sizeof(*nodes)*(size_t)count) : NULL;
            if (count && !nodes) continue;
            int i = 0;
            if (count)
                for (struct node *n = host->first; n; n = n->next) {
                    const char *s = n->type == N_ELEMENT ? node_attr(n, "slot") : "";
                    if (!strcmp(s ? s : "", name)) nodes[i++] = n;
                }
            slot->assigned_nodes = nodes;
            slot->assigned_count = count;
            if (!slot->slot_pending) {
                slot->slot_pending = 1;
                js_enqueue(d->J, slotchange_job, wrap(d, slot), js_undefined, js_undefined, js_undefined);
            }
        }
    }
}

static int bubbles_by_default(const char *type)
{
    static const char *const no[] = { "focus", "blur", "load", "mouseenter", "mouseleave", "pointerenter",
                                      "pointerleave", "scroll", "resize", "toggle", "error", "abort", NULL };
    for (int i = 0; no[i]; i++)
        if (!strcmp(type, no[i]))
            return 0;
    return 1;
}

int jsdom_dispatch(struct jsdom *d, struct node *target, const char *type)
{
    if (!d || !target)
        return 0;
    jsval ev = new_event(d, type, wrap(d, target));
    js_set(d->J, ev, "composed", js_bool(1));
    int bubbles = bubbles_by_default(type);
    js_set(d->J, ev, "bubbles", js_bool(bubbles));
    return dispatch_event(d, target, type, ev, bubbles);
}

/* A transition or animation event (anim.c): transitionend with its propertyName, animationstart... */
void jsdom_motion_event(struct jsdom *d, struct node *target, const char *type, const char *name, float elapsed)
{
    if (!d || !target)
        return;
    jsval ev = new_event(d, type, wrap(d, target));
    js_set(d->J, ev, "bubbles", js_bool(1));
    js_set(d->J, ev, !strncmp(type, "transition", 10) ? "propertyName" : "animationName", js_str(d->J, name));
    js_set(d->J, ev, "elapsedTime", js_num(elapsed));
    js_set(d->J, ev, "pseudoElement", js_str(d->J, ""));
    dispatch_event(d, target, type, ev, 1);
}

static void update_pointer_capture(struct jsdom *d);
static void mouse_position(struct jsdom *d, jsval ev, struct node *target, int x, int y);

int jsdom_mouse_event(struct jsdom *d, struct node *target, const char *type, int x, int y, int button,
                      struct node *related)
{
    if (!d) return 0;
    int pointer=!strncmp(type,"pointer",7);
    if(!strcmp(type,"pointerdown")){d->pointer_down=1;d->pointer_buttons=button>=0?1<<button:0;}
    if(!strcmp(type,"pointerup")||!strcmp(type,"pointercancel"))d->pointer_buttons=0;
    if(pointer)update_pointer_capture(d);
    if(d->capture && (pointer || !strcmp(type,"mousemove") || !strcmp(type,"mouseup")))target=d->capture;
    if (!target || web_inert(target)) return 0;
    if(!strcmp(type,"pointerdown") || !strcmp(type,"mousedown")) {d->activated=1;d->activation=uptime_ms();}
    struct js *J = d->J;
    jsval ev = new_event(d, type, wrap(d, target));
    js_set(d->J, ev, "composed", js_bool(1));
    int bubbles = bubbles_by_default(type);
    js_set(J, ev, "bubbles", js_bool(bubbles));
    js_set(J, ev, "cancelable", js_bool(bubbles));
    mouse_position(d, ev, target, x, y);
    if (button < 0 && strncmp(type, "pointer", 7))
        button = 0;                     /* mouse events say 0 when no button changed; pointer events -1 */
    js_set(J, ev, "button", js_num(button));
    js_set(J, ev, "which", js_num(button + 1));
    /* buttons: those held down after the event (pointerdown and pointerup keep count) */
    int up = !strcmp(type, "mouseup") || !strcmp(type, "click") || !strcmp(type, "dblclick") ||
             !strcmp(type, "auxclick");
    js_set(J, ev, "buttons", js_num(!strcmp(type, "mousedown") && button >= 0 ? 1 << button
                                    : up ? 0 : d->pointer_buttons));
    js_set(J, ev, "detail", js_num(!strcmp(type, "click") ? 1 : 0));
    if (related)
        js_set(J, ev, "relatedTarget", wrap(d, related));
    int prevented=dispatch_event(d, target, type, ev, bubbles);
    if (!prevented && !strcmp(type, "click") && is_connected(d, target)) {    /* popovertarget, commandfor */
        d->depth++;
        invoke(d, target);
        d->depth--;
        js_run_jobs(J);
        settle(d);
    }
    if(!strcmp(type,"pointerup")||!strcmp(type,"pointercancel")){
        d->pointer_down=0;d->pointer_buttons=0;d->pending_capture=NULL;update_pointer_capture(d);
    }
    return prevented;
}

/* Where a mouse, pointer or drag event at page point (x, y) happened, in each of the ways it can be asked. */
static void mouse_position(struct jsdom *d, jsval ev, struct node *target, int x, int y)
{
    struct js *J = d->J;
    int sx = 0, sy = 0;
    if (d->h.scroll_pos)
        d->h.scroll_pos(d->h.ctx, &sx, &sy);
    js_set(J, ev, "clientX", js_num(x - sx));
    js_set(J, ev, "clientY", js_num(y - sy));
    js_set(J, ev, "x", js_num(x - sx));
    js_set(J, ev, "y", js_num(y - sy));
    js_set(J, ev, "pageX", js_num(x));
    js_set(J, ev, "pageY", js_num(y));
    js_set(J, ev, "screenX", js_num(x - sx));
    js_set(J, ev, "screenY", js_num(y - sy));
    int bx = 0, by = 0, bw, bh;
    int local_x=x,local_y=y;
    if(d->h.node_local_point && d->h.node_local_point(d->h.ctx,target,&local_x,&local_y)) {
        js_set(J,ev,"offsetX",js_num(local_x));
        js_set(J,ev,"offsetY",js_num(local_y));
    } else if (d->h.node_box && d->h.node_box(d->h.ctx, target, &bx, &by, &bw, &bh)) {
        js_set(J, ev, "offsetX", js_num(x - bx));
        js_set(J, ev, "offsetY", js_num(y - by));
    }
}

/* ---- drag and drop ---- */

void jsdom_drag_begin(struct jsdom *d, const char *text, const char *url, int link)
{
    if (!d)
        return;
    struct js *J = d->J;
    jsval f = js_get(J, js_obj(d->window), "%drag"), r;
    jsval a[3] = { js_str(J, "new"), url && *url ? js_str(J, url) : js_null, text && *text ? js_str(J, text) : js_null };
    d->drag_link = link;
    if (js_is_callable(f) && js_call(J, f, js_undefined, a, 3, &r) == 0)
        js_set(J, js_obj(d->window), "%dragDT", r);
}

/* Ask the page's %drag helper something about the drag's DataTransfer; the answer as text into out. */
static int drag_op(struct jsdom *d, const char *op, const char *type, const char *current, char *out, int size)
{
    struct js *J = d->J;
    jsval dt = js_get(J, js_obj(d->window), "%dragDT"), f = js_get(J, js_obj(d->window), "%drag"), r;
    if (dt.t != JS_OBJ || !js_is_callable(f))
        return -1;
    jsval a[5] = { js_str(J, op), dt, js_str(J, type ? type : ""), js_bool(d->drag_link),
                   js_str(J, current ? current : "none") };
    if (js_call(J, f, js_undefined, a, 5, &r) < 0)
        return -1;
    if (out && size > 0)
        snprintf(out, (size_t)size, "%s", r.t == JS_STR ? js_tostr(J, r)->s : "");
    return 0;
}

int jsdom_drag_event(struct jsdom *d, struct node *target, const char *type, int x, int y, struct node *related,
                     const char *current, char *effect, int esize)
{
    if (effect && esize > 0)
        snprintf(effect, (size_t)esize, "none");
    if (!d || !target || web_inert(target))
        return 0;
    struct js *J = d->J;
    if (drag_op(d, "prepare", type, current, NULL, 0) < 0)
        return 0;
    jsval ev = new_event_kind(d, type, wrap(d, target), EV_DRAG);
    int quiet = !strcmp(type, "dragleave") || !strcmp(type, "dragend");
    js_set(J, ev, "composed", js_bool(1));
    js_set(J, ev, "bubbles", js_bool(1));
    js_set(J, ev, "cancelable", js_bool(!quiet));
    mouse_position(d, ev, target, x, y);
    js_set(J, ev, "button", js_num(0));
    js_set(J, ev, "buttons", js_num(!strcmp(type, "drop") || !strcmp(type, "dragend") ? 0 : 1));
    js_set(J, ev, "detail", js_num(0));
    js_set(J, ev, "dataTransfer", js_get(J, js_obj(d->window), "%dragDT"));
    if (related)
        js_set(J, ev, "relatedTarget", wrap(d, related));
    int prevented = dispatch_event(d, target, type, ev, 1);
    if (prevented && effect && (!strcmp(type, "dragenter") || !strcmp(type, "dragover")))
        drag_op(d, "effect", type, current, effect, esize);
    return prevented;
}

void jsdom_drag_text(struct jsdom *d, char *out, int size)
{
    if (size > 0)
        out[0] = 0;
    if (d)
        drag_op(d, "text", NULL, NULL, out, size);
}

void jsdom_drag_end(struct jsdom *d)
{
    if (d)
        js_set(d->J, js_obj(d->window), "%dragDT", js_null);
}

int jsdom_key_event(struct jsdom *d, struct node *target, const char *type, int keycode, const char *key)
{
    if (!d)
        return 0;
    struct js *J = d->J;
    if(!strcmp(type,"keydown")){d->activated=1;d->activation=uptime_ms();}
    if(target && web_inert(target))target=NULL;
    struct node *t = target ? target : node_find(d->h.root, T_BODY);
    if (!t)
        t = d->h.root;
    jsval ev = new_event(d, type, wrap(d, t));
    js_set(J, ev, "keyCode", js_num(keycode));
    js_set(J, ev, "which", js_num(keycode));
    js_set(J, ev, "charCode", js_num(key && key[0] && !key[1] ? (unsigned char)key[0] : 0));
    js_set(J, ev, "key", js_str(J, key ? key : ""));
    char code[24] = "";
    if (key && key[0] && !key[1] && isalpha((unsigned char)key[0]))
        snprintf(code, sizeof(code), "Key%c", toupper((unsigned char)key[0]));
    else if (key && key[0] && !key[1] && isdigit((unsigned char)key[0]))
        snprintf(code, sizeof(code), "Digit%c", key[0]);
    else if (key && !strcmp(key, " "))
        snprintf(code, sizeof(code), "Space");
    else
        snprintf(code, sizeof(code), "%s", key ? key : "");
    js_set(J, ev, "code", js_str(J, code));
    return dispatch_event(d, t, type, ev, 1);
}

static jsval el_dispatch_event(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d = dom(J);
    jsval ev = ARG(0);
    if (ev.t != JS_OBJ)
        return js_throw_error(J, "TypeError: dispatchEvent needs an event");
    struct node *n = node_of(self);
    const char *type = js_tostr(J, js_get(J, ev, "type"))->s;
    jsval t = target_of(J, self);
    js_set(J, ev, "target", t);
    js_set(J, ev, "srcElement", t);
    js_put(J, ev.u.o, S(J, "__stopped"), js_bool(0), 1);
    js_put(J, ev.u.o, S(J, "__stopnow"), js_bool(0), 1);
    if (n) {
        d->depth++;
        int r = !dispatch_event(d, n, type, ev, js_truthy(js_get(J, ev, "bubbles")));
        d->depth--;
        return js_bool(r);
    }
    js_set(J, ev, "__dispatching", js_bool(1));
    fire_on(d, t, NULL, type, ev);
    if (t.t == JS_OBJ && t.u.o == d->document && js_truthy(js_get(J, ev, "bubbles")) &&
        !js_truthy(js_get(J, ev, "__stopped")))
        fire_phase(d, js_obj(d->window), NULL, type, ev, 3);
    js_set(J, ev, "__dispatching", js_bool(0));
    js_set(J, ev, "eventPhase", js_num(0));
    return js_bool(!js_truthy(js_get(J, ev, "defaultPrevented")));
}

/* new Event(type, init) and the rest: init's members become the event's. */
static jsval make_event(struct js *J, jsval *args, int argc, int kind)
{
    struct jsdom *d = dom(J);
    if (!argc)
        return js_throw_error(J, "TypeError: an event needs a type");
    jsval ev = new_event_kind(d, js_tostr(J, ARG(0))->s, js_null, kind);
    jsval opts = ARG(1);
    js_set(J, ev, "bubbles", js_bool(0));
    js_set(J, ev, "cancelable", js_bool(0));
    js_set(J, ev, "composed", js_bool(0));
    js_set(J, ev, "isTrusted", js_bool(0));
    if (opts.t == JS_OBJ) {
        struct jsstr **keys;
        int n = js_keys(J, opts, 1, &keys);
        for (int i = 0; i < n; i++) {
            jsval key;
            key.t = JS_STR;
            key.u.s = keys[i];
            jsval v = js_get_str(J, opts, keys[i]);
            if (!strcmp(keys[i]->s, "bubbles") || !strcmp(keys[i]->s, "cancelable") ||
                !strcmp(keys[i]->s, "composed"))
                v = js_bool(js_truthy(v));
            js_setv(J, ev, key, v);
        }
    }
    return ev;
}

#define X(id, name, parent) \
    static jsval ctor_##id(struct js *J, jsval self, jsval *args, int argc) \
    { (void)self; return make_event(J, args, argc, id); }
EVENT_KINDS(X)
#undef X

/* document.createEvent("MouseEvents"): initEvent gives it a type later. */
static jsval doc_create_event(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    const char *what = js_tostr(J, ARG(0))->s;
    int kind = !strncasecmp(what, "Mouse", 5) ? EV_MOUSE : !strncasecmp(what, "Keyboard", 8) ? EV_KEYBOARD
             : !strncasecmp(what, "Custom", 6) ? EV_CUSTOM : !strncasecmp(what, "UI", 2) ? EV_UI
             : !strncasecmp(what, "Focus", 5) ? EV_FOCUS : EV_EVENT;
    jsval ev = new_event_kind(dom(J), "", js_null, kind);
    js_set(J, ev, "isTrusted", js_bool(0));
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
    if (IS("cookie")) {
        char jar[4096];
        if (d->h.cookie_get && d->h.cookie_get(d->h.ctx, jar, sizeof(jar))) {
            *out = js_str(J, jar);
            return 1;
        }
        *out = js_str(J, d->cookies);
        return 1;
    }
    if (IS("referrer")) { *out = js_str(J, d->h.referrer ? d->h.referrer : ""); return 1; }
    if (IS("characterSet") || IS("charset")) { *out = js_str(J, "UTF-8"); return 1; }
    if (IS("compatMode")) { *out = js_str(J, root && root->quirks ? "BackCompat" : "CSS1Compat"); return 1; }
    if (IS("visibilityState")) { *out = js_str(J, d->hidden ? "hidden" : "visible"); return 1; }
    if (IS("hidden")) { *out = js_bool(d->hidden); return 1; }
    if (IS("defaultView")) { *out = js_obj(d->window); return 1; }
    if (IS("activeElement")) {
        *out = wrap(d, d->active && is_connected(d,d->active) && web_focusable(d->active) ? d->active : node_find(root, T_BODY));
        return 1;
    }
    if (IS("scrollingElement")) { *out = wrap(d, root); return 1; }
    if (IS("dir")) { const char *v = node_attr(root, "dir"); *out = js_str(J, v ? v : ""); return 1; }
    if (IS("lastModified")) { *out = js_str(J, "01/01/2026 00:00:00"); return 1; }
    if (IS("designMode")) { *out = js_str(J, "off"); return 1; }
    if (IS("doctype")) { *out = js_null; return 1; }
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
        if (d->h.cookie_set && d->h.cookie_set(d->h.ctx, s->s))
            return 1;
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

static const struct jsclass document_class = { "HTMLDocument", document_get, document_set, NULL };

static jsval doc_get_by_id(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    return wrap(d, by_id(d->h.root, js_tostr(J, ARG(0))->s));
}

static jsval frag_get_by_id(struct js *J, jsval self, jsval *args, int argc)
{
    struct node *n = this_node(J, self);
    return n ? wrap(dom(J), by_id(n, js_tostr(J, ARG(0))->s)) : js_null;
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
    struct jsdom *d = dom(J);
    struct jsstr *name = js_tostr(J, ARG(0));
    if (d->custom_on && strchr(name->s, '-')) {     /* a defined custom element is constructed */
        jsval f = js_get(J, js_obj(d->window), "%ceCreate"), nv = js_strn(J, name->s, name->len);
        jsval made = js_is_callable(f) ? js_call_value(J, f, js_undefined, &nv, 1) : js_undefined;
        if (J->throwing || node_of(made))
            return made;
    }
    struct node *n = new_node(d, N_ELEMENT, name->s);
    return wrap(d, n);
}

/* new HTMLElement(), from a custom element's constructor (super()): the
 * element being upgraded, or a new one named as the class was defined. */
static jsval html_element_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self, (void)args, (void)argc;
    struct jsdom *d = dom(J);
    jsval up = js_get(J, js_obj(d->window), "%upgrading");
    if (node_of(up)) {
        js_set(J, js_obj(d->window), "%upgrading", js_null);
        return up;
    }
    jsval cls = J->super_this && J->super_this->proto ? js_get(J, js_obj(J->super_this->proto), "constructor")
                                                      : js_undefined;
    jsval f = js_get(J, js_obj(d->window), "%ceName");
    jsval name = js_is_callable(f) && cls.t == JS_OBJ ? js_call_value(J, f, js_undefined, &cls, 1) : js_undefined;
    if (J->throwing)
        return js_undefined;
    if (name.t != JS_STR)
        return js_throw_error(J, "TypeError: Illegal constructor");
    struct node *n = new_node(d, N_ELEMENT, js_cstr(name));
    return wrap(d, n);
}

static jsval win_css_supports(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return js_bool(css_supports(js_tostr(J, ARG(0))->s));
}

static jsval win_watch_custom(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    dom(J)->custom_on = js_truthy(ARG(0));
    return js_undefined;
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

static const struct jsclass location_class = { "Location", location_get, location_set, NULL };

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
    struct jsdom *d = dom(J);
    struct jsobj *m = js_new_object(J);
    jsval mv = js_obj(m), q = js_to_string(J, ARG(0));
    int vw = 800, vh = 600;
    if (d->h.viewport)
        d->h.viewport(d->h.ctx, &vw, &vh);
    js_set(J, mv, "matches", js_bool(css_media_query(js_cstr(q), vw, vh)));
    js_set(J, mv, "media", q);
    js_set(J, mv, "onchange", js_null);
    fn(J, m, "addListener", nothing, 1);
    fn(J, m, "removeListener", nothing, 1);
    fn(J, m, "addEventListener", nothing, 2);
    fn(J, m, "removeEventListener", nothing, 2);
    return mv;
}

/* A window proxy for window id (js_dom_eager's %windowProxy), or null. */
static jsval window_proxy(struct jsdom *d, int wid)
{
    jsval f = js_get(d->J, js_obj(d->window), "%windowProxy"), id = js_num(wid), r;
    if (!wid || !js_is_callable(f) || js_call(d->J, f, js_obj(d->window), &id, 1, &r) < 0)
        return js_null;
    return r;
}

static jsval win_open(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    const char *features = argc > 2 && ARG(2).t != JS_UNDEF ? js_tostr(J, ARG(2))->s : "";
    int noopener = strstr(features, "noopener") || strstr(features, "noreferrer");
    struct jsstr *url = argc && ARG(0).t != JS_UNDEF ? js_tostr(J, ARG(0)) : NULL;
    char abs[URL_MAX] = "about:blank";
    if (url && url->len)
        url_resolve(d->h.base, url->s, abs, sizeof(abs));
    const char *target = argc > 1 && ARG(1).t != JS_UNDEF ? js_tostr(J, ARG(1))->s : "_blank";
    if (!strcmp(target, "_self") || !strcmp(target, "_parent") || !strcmp(target, "_top") || !d->h.open_tab) {
        if (url && url->len)
            d->h.navigate(d->h.ctx, abs, 0);
        return js_obj(d->window);
    }
    int wid = d->h.open_tab(d->h.ctx, abs, noopener);
    return noopener || !wid ? js_null : window_proxy(d, wid);
}

/* window.close(): a window a script opened may close (the browser decides). */
static jsval win_close(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self, (void)args, (void)argc;
    struct jsdom *d = dom(J);
    if (d->h.window_close)
        d->h.window_close(d->h.ctx, 0);
    return js_undefined;
}

/* %winop(id, what, a, b): what window proxies ask of the browser (jsdom.h: window_query and the rest). */
static jsval window_op(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    int wid = (int)js_to_number(J, ARG(0));
    const char *what = js_tostr(J, ARG(1))->s;
    if (!strcmp(what, "post")) {
        struct jsstr *data = js_tostr(J, ARG(2)), *origin = js_tostr(J, ARG(3));
        if (d->h.window_post) {
            d->h.window_post(d->h.ctx, wid, data->s, data->len, origin->s);
        } else if (!wid) {              /* no other windows: only this one */
            char self_origin[300] = "null";
            web_origin(d->h.url, self_origin, sizeof(self_origin));
            jsdom_message(d, data->s, data->len, self_origin, 0);
        }
        return js_undefined;
    }
    if (!d->h.window_query)
        return !strcmp(what, "alive") ? js_bool(1) : js_num(0);
    if (!strcmp(what, "close")) {
        d->h.window_close(d->h.ctx, wid);
        return js_undefined;
    }
    if (!strcmp(what, "navigate")) {
        char abs[URL_MAX];
        url_resolve(d->h.base, js_tostr(J, ARG(2))->s, abs, sizeof(abs));
        d->h.window_navigate(d->h.ctx, wid, abs);
        return js_undefined;
    }
    char out[URL_MAX] = "";
    struct node *node = !strcmp(what, "frame") ? this_node(J, ARG(2)) : NULL;
    int r = d->h.window_query(d->h.ctx, wid, what, node, out, sizeof(out));
    if (!strcmp(what, "href"))
        return r > 0 ? js_str(J, out) : js_null;
    if (!strcmp(what, "alive"))
        return js_bool(r > 0);
    return js_num(r);
}

void jsdom_message(struct jsdom *d, const char *data, int len, const char *origin, int source_wid)
{
    if (!d || d->nposted >= 256)
        return;
    struct posted *p = realloc(d->posted, sizeof(*p) * (size_t)(d->nposted + 1));
    if (!p)
        return;
    d->posted = p;
    char *copy = malloc((size_t)len + 1), *from = strdup(origin ? origin : "null");
    if (!copy || !from) {
        free(copy);
        free(from);
        return;
    }
    memcpy(copy, data, (size_t)len);
    copy[len] = 0;
    p[d->nposted++] = (struct posted){ copy, from, source_wid };
}

/* The messages posted to this window, as MessageEvents (js_dom_eager's %deliverMessage). */
static void deliver_posted(struct jsdom *d)
{
    while (d->nposted) {
        struct posted m = d->posted[0];
        memmove(d->posted, d->posted + 1, sizeof(m) * (size_t)--d->nposted);
        jsval f = js_get(d->J, js_obj(d->window), "%deliverMessage"), r;
        jsval a[3] = { js_str(d->J, m.data), js_str(d->J, m.origin), js_num(m.source) };
        free(m.data);
        free(m.origin);
        if (js_is_callable(f)) {
            d->depth++;
            if (js_call(d->J, f, js_obj(d->window), a, 3, &r) < 0)
                report(d, "message");
            d->depth--;
            js_run_jobs(d->J);
            settle(d);
        }
    }
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

/* history.pushState(state, title, url) and replaceState */
static jsval history_change(struct js *J, jsval *args, int argc, int replace)
{
    struct jsdom *d = dom(J);
    jsval url = ARG(2);
    char abs[URL_MAX];
    if (url.t == JS_UNDEF || url.t == JS_NULL)
        snprintf(abs, sizeof(abs), "%s", d->h.url);
    else
        url_resolve(d->h.url, js_tostr(J, url)->s, abs, sizeof(abs));
    /* the same origin only: scheme, host and port */
    const char *a = strstr(abs, "://"), *b = strstr(d->h.url, "://");
    if (a && b) {
        size_t la = strcspn(a + 3, "/?#") + (size_t)(a + 3 - abs), lb = strcspn(b + 3, "/?#") + (size_t)(b + 3 - d->h.url);
        if (la != lb || strncasecmp(abs, d->h.url, la))
            return js_throw_error(J, "SecurityError: a history state cannot change the origin");
    }
    int id = d->entry;
    if (d->h.set_url)
        id = d->h.set_url(d->h.ctx, abs, replace);
    else if (!replace)
        id = d->entry + 1;
    if (!replace)
        d->hist_len++;
    d->entry = id;
    char key[16];
    snprintf(key, sizeof(key), "%d", id);
    js_set(J, js_obj(d->states), key, ARG(0));
    return js_undefined;
}

static jsval history_push(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return history_change(J, args, argc, 0);
}

static jsval history_replace(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    return history_change(J, args, argc, 1);
}

static jsval history_state(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self, (void)args, (void)argc;
    struct jsdom *d = dom(J);
    char key[16];
    snprintf(key, sizeof(key), "%d", d->entry);
    jsval v = js_get(J, js_obj(d->states), key);
    return v.t == JS_UNDEF ? js_null : v;
}

static jsval history_length(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self, (void)args, (void)argc;
    return js_num(dom(J)->hist_len);
}

/* A promise nobody was waiting on was rejected: the window's unhandledrejection, else a line in the status bar. */
static void host_unhandled(struct js *J, jsval promise, jsval reason)
{
    struct jsdom *d = dom(J);
    d->depth++;
    jsval ev = new_event(d, "unhandledrejection", js_obj(d->window));
    js_set(J, ev, "bubbles", js_bool(0));
    js_set(J, ev, "cancelable", js_bool(1));
    js_set(J, ev, "promise", promise);
    js_set(J, ev, "reason", reason);
    fire_on(d, js_obj(d->window), NULL, "unhandledrejection", ev);
    if (!js_truthy(js_get(J, ev, "defaultPrevented"))) {
        struct jsstr *s = reason.t == JS_OBJ || reason.t == JS_STR ? js_tostr(J, reason) : NULL;
        char msg[300];
        snprintf(msg, sizeof(msg), "Uncaught (in promise): %s", s ? s->s : "rejected");
        fprintf(stderr, "%s\n", msg);
        if (d->h.status)
            d->h.status(d->h.ctx, msg);
    }
    d->depth--;
}

/*
 * Page-level events from the browser: "beforeunload" and "pagehide" at the window (the page is
 * being left), "visibilitychange" at the document and up to the window (the window was minimized
 * or came back; hidden says which).
 */
void jsdom_page_event(struct jsdom *d, const char *type, int hidden)
{
    if (!d)
        return;
    struct js *J = d->J;
    d->depth++;
    if (!strcmp(type, "visibilitychange")) {
        if (d->hidden != hidden) {
            d->hidden = hidden;
            jsval ev = new_event(d, type, js_obj(d->document));
            js_set(J, ev, "bubbles", js_bool(1));
            js_set(J, ev, "cancelable", js_bool(0));
            fire_on(d, js_obj(d->document), NULL, type, ev);
            fire_phase(d, js_obj(d->window), NULL, type, ev, 3);
        }
    } else {
        jsval ev = new_event(d, type, js_obj(d->window));
        js_set(J, ev, "bubbles", js_bool(0));
        js_set(J, ev, "cancelable", js_bool(!strcmp(type, "beforeunload")));
        fire_on(d, js_obj(d->window), NULL, type, ev);
    }
    d->depth--;
    settle(d);
}

void jsdom_traverse(struct jsdom *d, int id, const char *old_url, const char *new_url)
{
    if (!d)
        return;
    struct js *J = d->J;
    d->entry = id;
    d->depth++;
    jsval ev = new_event(d, "popstate", js_obj(d->window));
    js_set(J, ev, "bubbles", js_bool(0));
    js_set(J, ev, "cancelable", js_bool(0));
    js_set(J, ev, "state", history_state(J, js_undefined, NULL, 0));
    fire_on(d, js_obj(d->window), NULL, "popstate", ev);
    const char *fo = url_fragment(old_url), *fn_ = url_fragment(new_url);
    if ((fo || fn_) && (!fo || !fn_ || strcmp(fo, fn_))) {
        jsval hc = new_event(d, "hashchange", js_obj(d->window));
        js_set(J, hc, "bubbles", js_bool(0));
        js_set(J, hc, "cancelable", js_bool(0));
        js_set(J, hc, "oldURL", js_str(J, old_url));
        js_set(J, hc, "newURL", js_str(J, new_url));
        fire_on(d, js_obj(d->window), NULL, "hashchange", hc);
    }
    d->depth--;
    settle(d);
}

/* ---- the view: scroll and resize events, and the observers ---- */

static jsval win_watch_mutations(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    dom(J)->watch_mutations = js_truthy(ARG(0));
    return js_undefined;
}

static jsval win_watch_view(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    dom(J)->watch_view = js_truthy(ARG(0));
    return js_undefined;
}

void jsdom_queue_event(struct jsdom *d, struct node *n, const char *type)
{
    if (!d || !n || d->nqueued == (int)(sizeof(d->queued) / sizeof(d->queued[0])))
        return;
    d->queued[d->nqueued].n = n;
    snprintf(d->queued[d->nqueued].type, sizeof(d->queued[0].type), "%s", type);
    d->nqueued++;
}

static void run_queued(struct jsdom *d)
{
    int n = d->nqueued;
    if (!n)
        return;
    struct node *nodes[128];
    char types[128][8];
    for (int i = 0; i < n; i++) {
        nodes[i] = d->queued[i].n;
        memcpy(types[i], d->queued[i].type, sizeof(types[i]));
    }
    d->nqueued = 0;
    for (int i = 0; i < n; i++)
        jsdom_dispatch(d, nodes[i], types[i]);
}

void jsdom_layout_changed(struct jsdom *d)
{
    if (d) {
        d->layout_gen++;
        jscanvas_laid_out(d->J);
    }
}

/* Has the view scrolled or changed size, or the page (when observers care) been laid out again? */
static int view_changes(struct jsdom *d, int *sx, int *sy, int *vw, int *vh)
{
    *sx = *sy = 0;
    *vw = d->last_vw;
    *vh = d->last_vh;
    if (d->h.scroll_pos)
        d->h.scroll_pos(d->h.ctx, sx, sy);
    if (d->h.viewport)
        d->h.viewport(d->h.ctx, vw, vh);
    return (*sx != d->last_sx || *sy != d->last_sy ? 1 : 0) | (*vw != d->last_vw || *vh != d->last_vh ? 2 : 0) |
           (d->watch_view && d->layout_gen != d->seen_gen ? 4 : 0);
}

static void process_view(struct jsdom *d)
{
    int sx, sy, vw, vh;
    int what = view_changes(d, &sx, &sy, &vw, &vh);
    d->seen_gen = d->layout_gen;
    if (!what)
        return;
    struct js *J = d->J;
    d->last_sx = sx, d->last_sy = sy, d->last_vw = vw, d->last_vh = vh;
    d->depth++;
    if (what & 1) {
        jsval ev = new_event(d, "scroll", js_obj(d->document));
        js_set(J, ev, "bubbles", js_bool(0));
        js_set(J, ev, "cancelable", js_bool(0));
        fire_on(d, js_obj(d->document), NULL, "scroll", ev);
        fire_phase(d, js_obj(d->window), NULL, "scroll", ev, 3);
    }
    if (what & 2) {
        jsval ev = new_event(d, "resize", js_obj(d->window));
        js_set(J, ev, "bubbles", js_bool(0));
        js_set(J, ev, "cancelable", js_bool(0));
        fire_on(d, js_obj(d->window), NULL, "resize", ev);
    }
    jsval hook = js_get(J, js_obj(d->window), "%viewChanged");
    if (d->watch_view && js_is_callable(hook)) {
        jsval r;
        if (js_call(J, hook, js_undefined, NULL, 0, &r) < 0)
            report(d, "observer");
    }
    d->depth--;
    settle(d);
}

/* ---- localStorage and sessionStorage ----
 * localStorage for each origin is kept in STORAGE_FILE, read the first time any
 * page uses storage and written back (at most every two seconds) when it changes;
 * sessionStorage for each origin in each window lives in the desktop's memory. */

#define STORAGE_FILE "/home/flux/.browser_storage"

#define STORE_MAX (4u << 20)            /* bytes of keys and values, all origins together */

struct kv {
    struct kv *next;
    char *scope, *key, *value;
};

static struct kv *store;
static size_t store_bytes;
static int local_marker, session_marker;    /* the storages' host data: which one */
static int store_loaded, store_dirty;
static unsigned store_saved;

static struct kv **kv_find(const char *scope, const char *key);

/* One more byte or escape sequence of text into a growing buffer. */
struct sbuf {
    char *p;
    size_t len, cap;
    int bad;
};

static void sb_put(struct sbuf *b, const char *s, size_t n)
{
    if (b->bad)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) {
            b->bad = 1;
            return;
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

/* Backslash, tab and line ends are written as \\, \t, \n and \r. */
/* Backslash, tab and line ends are written as \\, \t, \n and \r (one entry a line, fields apart by tabs). */
static void store_escape(struct sbuf *b, const char *s)
{
    while (*s) {
        size_t run = strcspn(s, "\\\t\n\r");
        sb_put(b, s, run);
        s += run;
        if (!*s)
            break;
        const char *esc = *s == '\\' ? "\\\\" : *s == '\t' ? "\\t" : *s == '\n' ? "\\n" : "\\r";
        sb_put(b, esc, 2);
        s++;
    }
}

static void store_unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void store_save(void)
{
    struct sbuf b = { NULL, 0, 0, 0 };
    for (struct kv *e = store; e; e = e->next) {
        if (e->scope[0] != 'l')
            continue;                   /* session storage stays in memory */
        store_escape(&b, e->scope + 2);
        sb_put(&b, "\t", 1);
        store_escape(&b, e->key);
        sb_put(&b, "\t", 1);
        store_escape(&b, e->value);
        sb_put(&b, "\n", 1);
    }
    if (!b.bad)
        store_write_file(STORAGE_FILE, b.p ? b.p : "", b.len);
    free(b.p);
    store_dirty = 0;
    store_saved = uptime_ms();
}

static void store_load(void)
{
    store_loaded = 1;
    long len;
    char *buf = store_read_file(STORAGE_FILE, 3 * (long)STORE_MAX, &len);
    if (!buf)
        return;
    for (char *line = buf; *line;) {
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = 0;
        char *key = strchr(line, '\t'), *value = key ? strchr(key + 1, '\t') : NULL;
        if (value) {
            *key++ = 0;
            *value++ = 0;
            store_unescape(line);
            store_unescape(key);
            store_unescape(value);
            char scope[URL_MAX + 40];
            snprintf(scope, sizeof(scope), "l %s", line);
            size_t add = strlen(key) + strlen(value);
            struct kv **pp = kv_find(scope, key);
            if (!*pp && store_bytes + add <= STORE_MAX) {
                struct kv *e = calloc(1, sizeof(*e));
                if (e) {
                    e->scope = strdup(scope);
                    e->key = strdup(key);
                    e->value = strdup(value);
                }
                if (e && e->scope && e->key && e->value) {
                    *pp = e;
                    store_bytes += add;
                } else if (e) {
                    free(e->scope), free(e->key), free(e->value), free(e);
                }
            }
        }
        if (!eol)
            break;
        line = eol + 1;
    }
    free(buf);
}

void jsdom_storage_flush(void)
{
    if (store_dirty && (unsigned)(uptime_ms() - store_saved) >= 2000)
        store_save();
}

static void storage_scope(struct jsdom *d, int session, char *out, int size)
{
    if (!store_loaded)
        store_load();
    const char *url = d->h.url, *p = strstr(url, "://");
    int n = p ? (int)(strcspn(p + 3, "/?#") + (size_t)(p + 3 - url)) : (int)strcspn(url, "/?#");
    if (session)
        snprintf(out, (size_t)size, "s %p %.*s", d->h.ctx, n, url);
    else
        snprintf(out, (size_t)size, "l %.*s", n, url);
}

static int storage_session(jsval self)
{
    return self.t == JS_OBJ && self.u.o->cls == C_HOST && self.u.o->hdata == &session_marker;
}

static struct kv **kv_find(const char *scope, const char *key)
{
    struct kv **pp = &store;
    for (; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->scope, scope) && !strcmp((*pp)->key, key))
            return pp;
    return pp;
}

static const char *storage_read(struct jsdom *d, int session, const char *key)
{
    char scope[URL_MAX + 40];
    storage_scope(d, session, scope, sizeof(scope));
    struct kv **pp = kv_find(scope, key);
    return *pp ? (*pp)->value : NULL;
}

static int storage_write(struct js *J, int session, const char *key, const char *value)
{
    struct jsdom *d = dom(J);
    char scope[URL_MAX + 40];
    storage_scope(d, session, scope, sizeof(scope));
    struct kv **pp = kv_find(scope, key);
    size_t old = *pp ? strlen((*pp)->key) + strlen((*pp)->value) : 0;
    if (store_bytes - old + strlen(key) + strlen(value) > STORE_MAX) {
        js_throw_value(J, js_make_error(J, "QuotaExceededError", "The storage quota has been exceeded."));
        return -1;
    }
    char *v = strdup(value);
    if (!v)
        return -1;
    if (*pp) {
        free((*pp)->value);
        (*pp)->value = v;
    } else {
        struct kv *e = calloc(1, sizeof(*e));
        if (!e || !(e->scope = strdup(scope)) || !(e->key = strdup(key))) {
            if (e)
                free(e->scope);
            free(e);
            free(v);
            return -1;
        }
        e->value = v;
        *pp = e;
    }
    store_bytes = store_bytes - old + strlen(key) + strlen(value);
    if (!session)
        store_dirty = 1;
    return 0;
}

static void storage_remove_key(struct jsdom *d, int session, const char *key)
{
    char scope[URL_MAX + 40];
    storage_scope(d, session, scope, sizeof(scope));
    struct kv **pp = kv_find(scope, key);
    if (*pp) {
        struct kv *e = *pp;
        *pp = e->next;
        if (!session)
            store_dirty = 1;
        store_bytes -= strlen(e->key) + strlen(e->value);
        free(e->scope);
        free(e->key);
        free(e->value);
        free(e);
    }
}

/* The scope's i-th key, or how many there are (i < 0). */
static int storage_nth(struct jsdom *d, int session, int i, const char **key)
{
    char scope[URL_MAX + 40];
    storage_scope(d, session, scope, sizeof(scope));
    int n = 0;
    for (struct kv *e = store; e; e = e->next)
        if (!strcmp(e->scope, scope)) {
            if (n == i) {
                *key = e->key;
                return n;
            }
            n++;
        }
    return i < 0 ? n : -1;
}

static jsval storage_get(struct js *J, jsval self, jsval *args, int argc)
{
    const char *v = storage_read(dom(J), storage_session(self), js_tostr(J, ARG(0))->s);
    return v ? js_str(J, v) : js_null;
}

static jsval storage_set(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsstr *k = js_tostr(J, ARG(0)), *v = js_tostr(J, ARG(1));
    storage_write(J, storage_session(self), k->s, v->s);
    return js_undefined;
}

static jsval storage_remove(struct js *J, jsval self, jsval *args, int argc)
{
    storage_remove_key(dom(J), storage_session(self), js_tostr(J, ARG(0))->s);
    return js_undefined;
}

static jsval storage_clear(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct jsdom *d = dom(J);
    int session = storage_session(self);
    const char *key;
    while (storage_nth(d, session, 0, &key) == 0)
        storage_remove_key(d, session, key);
    return js_undefined;
}

static jsval storage_key(struct js *J, jsval self, jsval *args, int argc)
{
    const char *key;
    double i = js_to_number(J, ARG(0));
    if (i != i || i < 0 || storage_nth(dom(J), storage_session(self), (int)i, &key) < 0)
        return js_null;
    return js_str(J, key);
}

/* localStorage.name reads and writes an item too, unless the name is a method's. */
static int storage_prop_get(struct js *J, struct jsobj *o, const char *key, jsval *out)
{
    int session = o->hdata == &session_marker;
    if (!strcmp(key, "length")) {
        const char *k;
        *out = js_num(storage_nth(dom(J), session, -1, &k));
        return 1;
    }
    if (o->proto && js_has(J, js_obj(o->proto), S(J, key)))
        return 0;
    const char *v = storage_read(dom(J), session, key);
    if (!v)
        return 0;
    *out = js_str(J, v);
    return 1;
}

static int storage_prop_set(struct js *J, struct jsobj *o, const char *key, jsval v)
{
    if (o->proto && js_has(J, js_obj(o->proto), S(J, key)))
        return 0;
    storage_write(J, o->hdata == &session_marker, key, js_tostr(J, v)->s);
    return 1;
}

static const struct jsclass storage_class = { "Storage", storage_prop_get, storage_prop_set, NULL };

static jsval make_storage(struct jsdom *d, int session)
{
    struct js *J = d->J;
    struct jsobj *proto = js_new_object(J);
    fn(J, proto, "getItem", storage_get, 1);
    fn(J, proto, "setItem", storage_set, 2);
    fn(J, proto, "removeItem", storage_remove, 1);
    fn(J, proto, "clear", storage_clear, 0);
    fn(J, proto, "key", storage_key, 1);
    struct jsobj *s = js_new_host(J, &storage_class, session ? &session_marker : &local_marker);
    s->proto = proto;
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

/* %b64(view) and %unb64(text): binary data in the structured clone format (jsdomlib.h's serialize), bytes to
 * base64 and back to a Uint8Array without going through a string of characters. */
static jsval bytes_base64(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    int len = 0;
    const unsigned char *p = js_binary_bytes(ARG(0), &len);
    if (!p)
        len = 0;
    char *out = js_alloc(J, (size_t)(len + 2) / 3 * 4 + 1);
    int n = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < len ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < len ? p[i + 2] : 0);
        out[n++] = b64[v >> 18];
        out[n++] = b64[(v >> 12) & 63];
        out[n++] = i + 1 < len ? b64[(v >> 6) & 63] : '=';
        out[n++] = i + 2 < len ? b64[v & 63] : '=';
    }
    return js_strn(J, out, n);
}

static jsval base64_bytes(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    static signed char value[256];
    if (!value['B'])
        for (int i = 0; i < 256; i++) {
            const char *c = i ? strchr(b64, i) : NULL;
            value[i] = (signed char)(c ? c - b64 : -1);
        }
    struct jsstr *s = js_tostr(J, ARG(0));
    int digits = 0;
    for (int i = 0; i < s->len; i++)
        digits += value[(unsigned char)s->s[i]] >= 0;
    jsval length = js_num((double)(digits * 6 / 8));
    jsval view = js_construct(J, js_get(J, js_obj(J->global), "Uint8Array"), &length, 1);
    int size = 0, n = 0, bits = 0;
    unsigned char *out = js_binary_bytes(view, &size);
    uint32_t acc = 0;
    for (int i = 0; out && i < s->len; i++) {
        int v = value[(unsigned char)s->s[i]];
        if (v < 0)
            continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8 && n < size) {
            bits -= 8;
            out[n++] = (unsigned char)(acc >> bits);
        }
    }
    return view;
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

static jsval document_font_register(struct js *J,jsval self,jsval *args,int argc)
{
    struct jsdom *d=dom(J);int len=0;unsigned char *bytes=js_binary_bytes(ARG(1),&len);
    const char *family=js_tostr(J,ARG(0))->s;
    return js_bool(bytes&&d->h.font_register&&d->h.font_register(d->h.ctx,family,bytes,len,js_truthy(ARG(2)),js_truthy(ARG(3))));
}
static jsval document_font_status(struct js *J,jsval self,jsval *args,int argc)
{
    struct jsdom *d=dom(J);return js_num(d->h.font_status?d->h.font_status(d->h.ctx,js_tostr(J,ARG(0))->s):-1);
}
static jsval xhr_ctor(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    (void)args;
    (void)argc;
    struct jsdom *d = dom(J);
    struct jsobj *x = js_mkobj(J, C_OBJECT, d->xhr_proto);
    x->hcls = &xhr_class;
    x->hdata = js_alloc(J, sizeof(struct xhr_request));
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
    struct jsstr *method = js_tostr(J, ARG(0));
    if (!method->len ||
        strspn(method->s, "!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") !=
            (size_t)method->len)
        return js_throw_error(J, "SyntaxError: invalid request method");
    char upper[64];
    if (method->len >= (int)sizeof(upper))
        return js_throw_error(J, "SyntaxError: method too long");
    for (int i = 0; i <= method->len; i++)
        upper[i] = (char)toupper((unsigned char)method->s[i]);
    if (!strcmp(upper, "CONNECT") || !strcmp(upper, "TRACE") || !strcmp(upper, "TRACK"))
        return js_throw_error(J, "SecurityError: forbidden method");
    if (!strcmp(upper, "GET") || !strcmp(upper, "HEAD") || !strcmp(upper, "POST") || !strcmp(upper, "PUT") ||
        !strcmp(upper, "DELETE") || !strcmp(upper, "OPTIONS"))
        method = S(J, upper);
    if (self.u.o->hcls != &xhr_class)
        return js_throw_error(J, "TypeError: invalid XMLHttpRequest receiver");
    for (int i = 0; i < MAX_XHR; i++)
        if (d->xhr[i] == self.u.o)
            d->xhr[i] = NULL;
    struct xhr_request *request = self.u.o->hdata;
    memset(request, 0, sizeof(*request));
    request->url = S(J, abs);
    request->method = method;
    request->headers = S(J, "");
    request->opened = 1;
    js_put(J, self.u.o, S(J, "__method"), (jsval){JS_STR, {.s = method}}, 1);
    js_put(J, self.u.o, S(J, "__url"), js_str(J, abs), 1);
    js_put(J, self.u.o, S(J, "__aborted"), js_bool(0), 1);
    js_put(J, self.u.o, S(J, "__headers"), js_str(J, ""), 1);
    xhr_state(d, self, 1);
    return js_undefined;
}

static jsval xhr_set_header(struct js *J, jsval self, jsval *args, int argc)
{
    if (self.t != JS_OBJ)
        return js_undefined;
    const char *name = js_tostr(J, ARG(0))->s, *val = js_tostr(J, ARG(1))->s;
    if (web_forbidden_header(name))
        return js_undefined;
    if (!*name ||
        strspn(name, "!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") !=
            strlen(name) ||
        strpbrk(val, "\r\n"))
        return js_undefined;
    if (self.u.o->hcls != &xhr_class)
        return js_throw_error(J, "TypeError: invalid XMLHttpRequest receiver");
    struct xhr_request *request = self.u.o->hdata;
    if (!request->opened)
        return js_throw_error(J, "InvalidStateError: request already sent or not open");
    const char *old = request->headers->s;
    size_t n = (old ? strlen(old) : 0) + strlen(name) + strlen(val) + 4;
    if (n > 4000)
        return js_undefined;
    char *buf = malloc(n);
    if (!buf)
        return js_undefined;
    snprintf(buf, n, "%s%s: %s\n", old ? old : "", name, val);
    request->headers = S(J, buf);
    js_put(J, self.u.o, S(J, "__headers"), js_str(J, buf), 1);
    free(buf);
    return js_undefined;
}

static void xhr_failure(struct jsdom *d, int id, jsval x)
{
    struct js *J = d->J;
    if (id >= 0)
        d->xhr[id] = NULL;
    js_set(J, x, "status", js_num(0));
    js_set(J, x, "statusText", js_str(J, ""));
    js_set(J, x, "responseText", js_str(J, ""));
    js_set(J, x, "response", js_null);
    js_put(J, x.u.o, S(J, "__headers"), js_str(J, ""), 1);
    xhr_state(d, x, 4);
    fire_on(d, x, NULL, "error", new_event(d, "error", x));
    fire_on(d, x, NULL, "loadend", new_event(d, "loadend", x));
}
static void xhr_issue(struct jsdom *d, int id, jsval x, int preflight)
{
    struct xhr_request *r = &d->requests[id];
    const char *url = r->url->s, *method = r->method->s, *headers = r->headers->s;
    if(!strncmp(url,"blob:",5)){
        char origin[300]="null",type[128];unsigned char *bytes=NULL;size_t len=0;
        web_origin(d->h.url,origin,sizeof(origin));
        if((strcmp(method,"GET")&&strcmp(method,"HEAD")) || !web_blob_read(d,origin,url,&bytes,&len,type)){
            xhr_failure(d,id,x);return;
        }
        char head[300];int n=snprintf(head,sizeof(head),"Content-Type: %s\r\nContent-Length: %u\r\n",*type?type:"application/octet-stream",(unsigned)len);
        r->cross=0;r->preflight=0;r->received=0;
        jsdom_http_done(d,id,200,head,n,(const char *)bytes,!strcmp(method,"HEAD")?0:(int)len);
        free(bytes);return;
    }
    char origin[300] = "null", target[300];
    web_origin(d->h.url, origin, sizeof(origin));
    int cross = !web_same_origin(d->h.url, url);
    int local = url_is_file(url) && url_is_file(d->h.url);
    const char *mode = r->mode == 1 ? "same-origin" : r->mode == 2 ? "no-cors" : "cors";
    int credentials = r->credential_mode != 1 && (!cross || r->credential_mode == 2);
    r->credentials = credentials;
    if ((!web_origin(url, target, sizeof(target)) && !local) ||
        (url_is_https(d->h.url) && url_is_http(url)) || (cross && !local && !strcmp(mode, "same-origin")) ||
        (cross && !strcmp(mode, "no-cors") && !web_simple_request(method, headers))) {
        xhr_failure(d, id, x);
        return;
    }
    int need =
        preflight && cross && !local && strcmp(mode, "no-cors") && !web_simple_request(method, headers);
    r->preflight = need;
    r->received=r->chunks=r->loaded=0;r->partial=NULL;
    r->cross = cross && !local;
    char request[8192];
    if (need) {
        int n = snprintf(request, sizeof(request), "Origin: %s\nAccess-Control-Request-Method: %s\n", origin,
                         method);
        char names[4096] = "";
        int nn = 0;
        for (const char *p = headers; *p;) {
            const char *e = strchr(p, '\n');
            if (!e)
                e = p + strlen(p);
            const char *c = memchr(p, ':', (size_t)(e - p));
            if (c && nn + (c - p) + 2 < (int)sizeof(names)) {
                if (nn)
                    names[nn++] = ',';
                for (const char *q = p; q < c; q++)
                    names[nn++] = (char)tolower((unsigned char)*q);
                names[nn] = 0;
            }
            p = *e ? e + 1 : e;
        }
        if (nn)
            snprintf(request + n, sizeof(request) - (size_t)n, "Access-Control-Request-Headers: %s\n", names);
        d->h.http(d->h.ctx, id, "OPTIONS", url, NULL, 0, request, 0);
    } else {
        int send_origin=!local && (cross || (strcmp(method,"GET") && strcmp(method,"HEAD")));
        snprintf(request,sizeof(request),"%s%s%s%s",headers,send_origin?"Origin: ":"",send_origin?origin:"",send_origin?"\n":"");
        const void *bytes = r->body ? r->body->s : NULL;
        int len = r->body ? r->body->len : 0;
        if (!strcmp(method, "GET") || !strcmp(method, "HEAD"))
            bytes = NULL, len = 0;
        d->h.http(d->h.ctx, id, method, url, bytes, len, request, credentials);
    }
}
static jsval xhr_send(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d = dom(J);
    if (self.t != JS_OBJ)
        return js_undefined;
    if (self.u.o->hcls != &xhr_class)
        return js_throw_error(J, "TypeError: invalid XMLHttpRequest receiver");
    struct xhr_request *source = self.u.o->hdata;
    if (!source->opened)
        return js_throw_error(J, "InvalidStateError: request is not opened");
    int id = -1;
    for (int i = 0; i < MAX_XHR; i++) {
        if (d->xhr[i] == self.u.o)
            return js_throw_error(J, "InvalidStateError: request already sent");
        if (!d->xhr[i] && id < 0)
            id = i;
    }
    if (id < 0 || (!d->h.http && strncmp(source->url->s,"blob:",5))) {
        xhr_failure(d, -1, self);
        return js_undefined;
    }
    d->xhr[id] = self.u.o;
    source->opened = 0;
    struct xhr_request *r = &d->requests[id];
    *r = *source;
    r->streaming=js_truthy(js_get(J,self,"__streaming"));
    const char *creds = js_cstr(js_get(J, self, "__credentials"));
    r->credential_mode = !strcmp(creds, "omit")                                                       ? 1
                         : !strcmp(creds, "include") || js_truthy(js_get(J, self, "withCredentials")) ? 2
                                                                                                      : 0;
    const char *mode = js_cstr(js_get(J, self, "__mode"));
    r->mode = !strcmp(mode, "same-origin") ? 1 : !strcmp(mode, "no-cors") ? 2 : 0;
    jsval body = ARG(0);
    int len = 0;
    unsigned char *bytes = js_binary_bytes(body, &len);
    if (bytes)
        r->body = js_mkstr(J, (const char *)bytes, len);
    else if (body.t != JS_NULL && body.t != JS_UNDEF)
        r->body = js_tostr(J, body);
    xhr_issue(d, id, self, 1);
    return js_undefined;
}

static jsval xhr_abort(struct js *J, jsval self, jsval *args, int argc)
{
    (void)args;
    (void)argc;
    struct jsdom *d = dom(J);
    for (int i = 0; i < MAX_XHR; i++)
        if (self.t == JS_OBJ && d->xhr[i] == self.u.o)
        {
            d->xhr[i]=NULL;
            if(d->h.http_cancel)d->h.http_cancel(d->h.ctx,i);
            js_set(J,self,"status",js_num(0));
            xhr_state(d,self,4);
            fire_on(d,self,NULL,"abort",new_event(d,"abort",self));
            fire_on(d,self,NULL,"loadend",new_event(d,"loadend",self));
        }
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

int jsdom_http_streaming(struct jsdom *d,int id)
{
    if(d&&id>=JSWORKER_ID_BASE)return jsworkers_http_streaming(d->workers,id);
    return d&&id>=0&&id<MAX_XHR&&d->xhr[id]&&d->requests[id].streaming;
}
int jsdom_http_headers(struct jsdom *d,int id,int status,const char *headers,int hlen)
{
    if(d&&id>=JSWORKER_ID_BASE)return jsworkers_http_headers(d->workers,id,status,headers,hlen);
    if(!d||id<0||id>=MAX_XHR||!d->xhr[id])return -1;
    struct js *J=d->J;jsval x=js_obj(d->xhr[id]);struct xhr_request *request=&d->requests[id];
    if(request->received)return request->streaming?2:1;
    if(request->preflight || (status>=300&&status<=308&&status!=304))return 0;
    if(status<200)return 0;
    if(!headers||hlen<0){headers="";hlen=0;}     /* a file:// response has no headers */
    struct jsstr *bounded=js_mkstr(J,headers,hlen);headers=bounded->s;
    char origin[300]="null";web_origin(d->h.url,origin,sizeof(origin));int credentials=request->credentials;
    if(request->cross&&request->mode!=2&&!web_cors_allow(origin,headers,credentials)){xhr_failure(d,id,x);return -1;}
    int opaque=request->cross&&request->mode==2;
    if(opaque){status=0;headers=NULL;hlen=0;}
    /* the header block without its status line */
    const char *h = headers;
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
    char filtered[16384];
    int filtered_len = web_response_headers(h, request->cross, credentials, filtered, sizeof(filtered));
    js_put(J, x.u.o, S(J, "__headers"),
           filtered_len >= 0 ? js_strn(J, filtered, filtered_len) : js_str(J, ""), 1);
    js_set(J, x, "status", js_num(status));
    js_set(J, x, "statusText", js_str(J, status_text));
    js_set(J, x, "responseURL", (jsval){JS_STR, {.s = request->url}});

    request->received=1;d->depth++;xhr_state(d,x,2);d->depth--;js_run_jobs(J);settle(d);
    return request->streaming?2:1;
}
void jsdom_http_chunk(struct jsdom *d,int id,const void *data,int len)
{
    if(d&&id>=JSWORKER_ID_BASE){jsworkers_http_chunk(d->workers,id,data,len);return;}
    if(!d||id<0||id>=MAX_XHR||!d->xhr[id]||len<=0)return;
    struct xhr_request *r=&d->requests[id];if(!r->received || (r->cross&&r->mode==2))return;
    struct js *J=d->J;jsval x=js_obj(d->xhr[id]);
    r->chunks++;r->loaded+=len;
    if(!r->streaming && !*js_cstr(js_get(J,x,"responseType"))){
        int old=r->partial?r->partial->len:0;
        if(old<=16*1024*1024-len){char *joined=malloc((size_t)old+(size_t)len);if(joined){if(old)memcpy(joined,r->partial->s,(size_t)old);memcpy(joined+old,data,(size_t)len);r->partial=js_mkstr(J,joined,old+len);free(joined);js_set(J,x,"responseText",(jsval){JS_STR,{.s=r->partial}});}}
    }
    jsval view=js_construct(J,js_get(J,js_obj(J->global),"Uint8Array"),(jsval[]){js_num(len)},1);
    int size=0;unsigned char *bytes=js_binary_bytes(view,&size);if(bytes)memcpy(bytes,data,(size_t)len);
    d->depth++;xhr_state(d,x,3);jsval ev=new_event(d,"progress",x);
    js_set(J,ev,"data",view);js_set(J,ev,"loaded",js_num(r->loaded));js_set(J,ev,"lengthComputable",js_bool(0));
    fire_on(d,x,NULL,"progress",ev);d->depth--;js_run_jobs(J);settle(d);
}

void jsdom_http_done(struct jsdom *d, int id, int status, const char *headers, int hlen,
                     const char *body, int blen)
{
    if (d && id >= JSWORKER_ID_BASE) {
        jsworkers_http_done(d->workers, id, status, headers, hlen, body, blen);
        return;
    }
    if (!d || id < 0 || id >= MAX_XHR || !d->xhr[id])
        return;
    struct js *J = d->J;
    jsval x = js_obj(d->xhr[id]);
    char origin[300] = "null";
    web_origin(d->h.url, origin, sizeof(origin));
    struct xhr_request *request = &d->requests[id];
    int credentials = request->credentials;
    if (request->preflight) {
        if (status >= 200 && status < 300 &&
            web_cors_preflight(origin, headers, credentials, request->method->s, request->headers->s))
            xhr_issue(d, id, x, 0);
        else
            xhr_failure(d, id, x);
        return;
    }
    if (request->cross && request->mode != 2 && !web_cors_allow(origin, headers, credentials)) {
        xhr_failure(d, id, x);
        return;
    }
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        char location[URL_MAX], next[URL_MAX];
        if (web_header_value(headers, "Location", location, sizeof(location)) == 1) {
            if (++request->redirects > 10) {
                xhr_failure(d, id, x);
                return;
            }
            url_resolve(request->url->s, location, next, sizeof(next));
            if (!web_same_origin(request->url->s, next)) {
                /* Authorization is never forwarded to a different origin. */
                const char *p = request->headers->s;
                char kept[4096] = "";
                size_t used = 0;
                while (*p) {
                    const char *e = strchr(p, '\n');
                    if (!e)
                        e = p + strlen(p);
                    if (strncasecmp(p, "Authorization:", 14)) {
                        size_t n = (size_t)(e - p);
                        if (used + n + 2 < sizeof(kept)) {
                            memcpy(kept + used, p, n);
                            used += n;
                            kept[used++] = '\n';
                            kept[used] = 0;
                        }
                    }
                    p = *e ? e + 1 : e;
                }
                request->headers = S(J, kept);
            }
            request->url = S(J, next);
            if ((status == 303 && strcmp(request->method->s, "HEAD")) ||
                ((status == 301 || status == 302) && !strcmp(request->method->s, "POST"))) {
                request->method = S(J, "GET");
                request->body = NULL;
            }
            xhr_issue(d, id, x, 1);
            return;
        }
    }
    if (request->cross && request->mode == 2) {
        status = 0;
        headers = NULL;
        hlen = 0;
        body = NULL;
        blen = 0;
    }
    int opaque = request->cross && request->mode == 2;
    if(!status&&!opaque){xhr_failure(d,id,x);js_run_jobs(J);return;}
    if(!request->received){
        jsdom_http_headers(d,id,status,headers,hlen);
        if(!d->xhr[id])return;
        if(body&&blen)jsdom_http_chunk(d,id,body,blen);
    }
    d->xhr[id]=NULL;
    d->depth++;
    jsval text = js_strn(J, body ? body : "", body ? blen : 0);
    js_set(J, x, "responseText", text);
    jsval response = text;
    if (!strcmp(js_cstr(js_get(J, x, "responseType")), "arraybuffer")) {
        jsval ctor = js_get(J, js_obj(J->global), "Uint8Array"), length = js_num(body ? blen : 0);
        jsval view = js_construct(J, ctor, &length, 1);
        int n;
        unsigned char *bytes = js_binary_bytes(view, &n);
        if (bytes && body)
            memcpy(bytes, body, (size_t)blen);
        response = js_get(J, view, "buffer");
    } else if (!strcmp(js_cstr(js_get(J, x, "responseType")), "json")) {
        jsval json = js_get(J, js_obj(J->global), "JSON");
        jsval parse = js_get(J, json, "parse");
        if (js_call(J, parse, json, &text, 1, &response) < 0)
            response = js_null;
    }
    js_set(J, x, "response", response);
    xhr_state(d, x, 4);
    const char *kind = status || opaque ? "load" : "error";
    fire_on(d, x, NULL, kind, new_event(d, kind, x));
    fire_on(d, x, NULL, "loadend", new_event(d, "loadend", x));
    d->depth--;
    js_run_jobs(J);
    settle(d);
}

/* ---- the DOM library, loaded when first used ----
 * Each name js_dom_lib defines starts as an accessor that runs it (which
 * puts the real thing in its place) and then reads the name again. */

#define DOM_LIB_NAMES(X)                                                                                     \
    X(F, adoptedStyleSheets) X(W, MessageChannel)                                                                                     \
    X(W, MessagePort)                                                                                        \
    X(W, BroadcastChannel) X(W, XMLSerializer) X(W, Path2D) X(W, ImageData) X(W, NodeFilter)                 \
        X(W, TreeWalker) X(W, NodeIterator) X(W, Range) X(W, StaticRange) X(W, Selection) X(W, getSelection) \
            X(W, CSSStyleSheet) X(W, StyleSheet) X(W, CSSRule) X(W, FontFace) X(D, createTreeWalker)         \
                X(D, createNodeIterator) X(D, getSelection) X(D, createRange) X(D, getAnimations)            \
                    X(D, createAttribute) X(D, queryCommandSupported) X(D, exitFullscreen) X(                \
                        D, hasStorageAccess) X(D, requestStorageAccess) X(D, fonts) X(D, adoptedStyleSheets) \
                        X(D, fullscreenElement) X(D, fullscreenEnabled) X(D, pictureInPictureEnabled)        \
                            X(D, styleSheets) X(E, validity) X(E, willValidate) X(E, validationMessage)      \
                                X(E, setCustomValidity) X(E, checkValidity) X(E, reportValidity)             \
                                    X(E, showPicker) X(E, setRangeText) X(E, animate) X(E, getAnimations)    \
                                        X(E, assignedElements) X(E, sheet) X(E, requestFullscreen)           \
                                            X(E, scroll) X(E, scrollTo) X(E, scrollBy) X(E, getClientRects)  \
                                                X(E, checkVisibility) X(E, setPointerCapture)                \
                                                    X(E, releasePointerCapture) X(E, hasPointerCapture)      \
                                                            X(E, inert) X(E, draggable)                      \
                                                                X(E, slot) X(E, part) X(T, splitText)        \
                                                                    X(T, appendData) X(T, substringData)     \
    X(W, ReportingObserver) X(W, TaskController) X(W, scheduler) X(W, SpeechSynthesisUtterance)              \
    X(W, speechSynthesis) X(W, ElementInternals) X(W, CustomStateSet) X(W, Sanitizer) X(W, TrustedHTML)      \
    X(W, TrustedScript) X(W, TrustedScriptURL) X(W, TrustedTypePolicy) X(W, TrustedTypePolicyFactory)        \
    X(W, trustedTypes) X(D, pointerLockElement) X(D, exitPointerLock) X(D, caretPositionFromPoint)           \
    X(D, caretRangeFromPoint) X(E, requestPointerLock) X(E, attachInternals) X(E, setHTML)                   \
    X(E, setHTMLUnsafe)

static void dom_lib_load(struct jsdom *d)
{
    if (d->dom_lib)
        return;
    d->dom_lib = 1;
    if (js_eval(d->J, js_dom_lib, (int)sizeof(js_dom_lib) - 1, "DOM library") < 0)
        fprintf(stderr, "DOM library: %s\n", js_error(d->J));
}

static jsval dom_lib_get(struct js *J, jsval self, const char *name)
{
    struct jsdom *d = dom(J);
    if (d->dom_lib)
        return js_undefined;            /* the library did not define it after all */
    dom_lib_load(d);
    return js_get(J, self.t == JS_OBJ ? self : js_obj(d->window), name);
}

static jsval dom_lib_set(struct js *J, jsval self, const char *name, jsval v)
{
    struct jsdom *d = dom(J);
    dom_lib_load(d);
    if (self.t != JS_OBJ)
        return js_undefined;
    js_set(J, self, name, v);
    return js_undefined;
}

#define X(where, name) \
    static jsval lazy_get_##where##_##name(struct js *J, jsval self, jsval *args, int argc) \
    { (void)args, (void)argc; return dom_lib_get(J, self, #name); } \
    static jsval lazy_set_##where##_##name(struct js *J, jsval self, jsval *args, int argc) \
    { return dom_lib_set(J, self, #name, ARG(0)); }
DOM_LIB_NAMES(X)
#undef X

/* ---- setting up ---- */

static void dom_fetch_module(void *ctx, int id, const char *url);
static int dom_resolve(void *ctx, const char *spec, const char *base, char *out, int size);
static void dom_module_done(void *ctx, void *tag, int failed);

static int network_cookies(void *ctx,const char *url,char *out,int size)
{
    struct jsdom *d=ctx;return d->h.ws_cookies?d->h.ws_cookies(d->h.ctx,url,out,size):0;
}

static void network_event(void *ctx,jsval target,const char *type,jsval data,int code,const char *reason,int clean)
{
    struct jsdom *d=ctx;struct js *J=d->J;d->depth++;
    jsval ev=new_event(d,type,target);
    if(!strcmp(type,"message")){
        js_set(J,ev,"data",data);const char *url=js_cstr(js_get(J,target,"url"));
        int secure=!strncmp(url,"wss://",6);char http[URL_MAX],origin[300]="null",serialized[300]="null";
        snprintf(http,sizeof(http),"%s://%s",secure?"https":"http",url+(secure?6:5));
        if(web_origin(http,origin,sizeof(origin)))snprintf(serialized,sizeof(serialized),"%s%s",secure?"wss":"ws",strchr(origin,':'));
        js_set(J,ev,"origin",js_str(J,serialized));
    }
    if(!strcmp(type,"error"))js_set(J,ev,"message",js_str(J,reason));
    if(!strcmp(type,"close")){js_set(J,ev,"code",js_num(code));js_set(J,ev,"reason",js_str(J,reason));js_set(J,ev,"wasClean",js_bool(clean));}
    fire_on(d,target,NULL,type,ev);d->depth--;settle(d);
}

static jsval blob_url_api(struct js *J,jsval self,jsval *args,int argc)
{
    struct jsdom *d=dom(J);
    if(argc<2){web_blob_revoke(d,js_tostr(J,ARG(0))->s);return js_undefined;}
    int len=0;unsigned char *p=js_binary_bytes(ARG(0),&len);
    if(!p)return js_throw_error(J,"TypeError: createObjectURL requires a Blob");
    struct jsstr *type=js_tostr(J,ARG(1));char origin[300]="null";
    web_origin(d->h.url,origin,sizeof(origin));
    const char *url=web_blob_create(d,origin,p,(size_t)len,type->s);
    return url?js_str(J,url):js_throw_error(J,"QuotaExceededError: cannot retain Blob URL");
}

/* %chosenFile(input): the file the user picked for an <input type=file> (never a path a page
 * set) as { name, type, lastModified, bytes }, or null. Up to 8 MB, as a form would send. */
static jsval chosen_file(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct node *n = node_of(ARG(0));
    if (!n || !is_file_input(n) || !n->file_chosen || !n->value || !n->value[0])
        return js_null;
    long len = 0;
    char *data = store_read_file(n->value, 8L << 20, &len);
    if (!data)
        return js_null;
    static const struct { const char *ext, *type; } types[] = {
        { ".txt", "text/plain" }, { ".html", "text/html" }, { ".htm", "text/html" }, { ".css", "text/css" },
        { ".js", "text/javascript" }, { ".json", "application/json" }, { ".xml", "application/xml" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" },
        { ".svg", "image/svg+xml" }, { ".webp", "image/webp" }, { ".pdf", "application/pdf" },
        { ".zip", "application/zip" }, { ".wav", "audio/wav" }, { ".mp3", "audio/mpeg" }, { ".csv", "text/csv" },
    };
    const char *base = strrchr(n->value, '/'), *dot;
    base = base ? base + 1 : n->value;
    const char *type = "";
    if ((dot = strrchr(base, '.')))
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
            if (!strcasecmp(dot, types[i].ext))
                type = types[i].type;
    double modified = (double)store_file_mtime(n->value) * 1000;
    struct jsobj *o = js_new_object(J);
    js_put(J, o, js_mkstr(J, "name", 4), js_str(J, base), 1);
    js_put(J, o, js_mkstr(J, "type", 4), js_str(J, type), 1);
    js_put(J, o, js_mkstr(J, "lastModified", 12), js_num(modified), 1);
    js_put(J, o, js_mkstr(J, "bytes", 5), js_new_bytes(J, (const unsigned char *)data, (int)len), 1);
    free(data);
    return js_obj(o);
}

/* %idb(op, name, text): IndexedDB's store for this page's origin (webstore.c: store_idb). */
static jsval idb_store(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    char origin[300] = "null";
    web_origin(d->h.url, origin, sizeof(origin));
    if (!strcmp(origin, "null"))
        snprintf(origin, sizeof(origin), "%s", !strncmp(d->h.url, "file:", 5) ? "file://" : "null");
    const char *op = js_tostr(J, ARG(0))->s;
    struct jsstr *name = js_tostr(J, ARG(1)), *text = argc > 2 ? js_tostr(J, args[2]) : NULL;
    char *r = store_idb(op, origin, name->s, text ? text->s : "", text ? (size_t)text->len : 0);
    jsval v = r ? js_str(J, r) : js_null;
    free(r);
    return v;
}

static jsval clipboard_text(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d=dom(J);
    char origin[300];
    int secure=url_is_https(d->h.url) || !strncmp(d->h.url,"file:",5);
    if(web_origin(d->h.url,origin,sizeof(origin)))
        secure |= !strncmp(origin,"https://",8) || !strcmp(origin,"http://localhost") || !strncmp(origin,"http://localhost:",17) ||
                  !strcmp(origin,"http://127.0.0.1") || !strncmp(origin,"http://127.0.0.1:",17);
    if(!secure || d->hidden || (d->h.has_focus&&!d->h.has_focus(d->h.ctx)) || !d->activated || (unsigned)(uptime_ms()-d->activation)>5000 ||
       (argc ? !d->h.clipboard_write : !d->h.clipboard_read))
        return js_throw_error(J,"NotAllowedError: clipboard requires a recent user gesture in a secure page");
    if(argc) { struct jsstr *text=js_tostr(J,args[0]); if(!js_throwing(J))d->h.clipboard_write(d->h.ctx,text->s);return js_undefined; }
    const char *text=d->h.clipboard_read(d->h.ctx);
    return js_str(J,text?text:"");
}
/* ---- <audio>: the browser plays it (h.media); what the sound does comes back through jsdom_media_event,
 * to jsdomlib.h's %mediaState, which keeps the element's state and sends the events ---- */
static jsval media_api(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsdom *d = dom(J);
    struct node *n = node_of(ARG(0));
    const char *op = js_tostr(J, ARG(1))->s;
    if (!strcmp(op, "allowed"))         /* sound may start once the user has done something on the page */
        return js_bool(d->activated);
    if (n && d->h.media)
        d->h.media(d->h.ctx, n, op, js_to_number(J, ARG(2)));
    return js_undefined;
}

static void media_call(struct jsdom *d, const char *hook, struct node *n, const char *what, double a, double b)
{
    if (!d || !n)
        return;
    struct js *J = d->J;
    jsval fn = js_get(J, js_obj(d->window), hook), r;
    if (!js_is_callable(fn))
        return;
    jsval args[4] = { wrap(d, n), js_str(J, what), js_num(a), js_num(b) };
    d->depth++;
    if (js_call(J, fn, js_undefined, args, 4, &r) < 0)
        report(d, hook);
    d->depth--;
    js_run_jobs(J);
    settle(d);
}

void jsdom_media_event(struct jsdom *d, struct node *n, const char *type, double time, double duration)
{
    media_call(d, "%mediaState", n, type, time, duration);
}

void jsdom_media_user(struct jsdom *d, struct node *n, const char *op, double time)
{
    media_call(d, "%mediaUser", n, op, time, 0);
}

static jsval pointer_capture_api(struct js *J, jsval self, jsval *args, int argc)
{
    struct jsdom *d=dom(J);struct node *n=this_node(J,self);
    double id=js_to_number(J,ARG(0));int mode=(int)js_to_number(J,ARG(1));
    if(mode==2)return js_bool(id==1&&d->pending_capture==n);
    if(id!=1)return js_throw_error(J,"NotFoundError: unknown pointer");
    if(mode==1) {
        if(!is_connected(d,n))return js_throw_error(J,"InvalidStateError: disconnected capture target");
        if(d->pointer_down && !web_inert(n))d->pending_capture=n;
    } else if(d->pending_capture==n)d->pending_capture=NULL;
    return js_undefined;
}
struct node *jsdom_pointer_capture(struct jsdom *d)
{
    if(!d)return NULL;
    if(d->pending_capture && (!is_connected(d,d->pending_capture)||web_inert(d->pending_capture)))d->pending_capture=NULL;
    return d->pending_capture;
}
static void update_pointer_capture(struct jsdom *d)
{
    struct node *next=jsdom_pointer_capture(d),*old=d->capture;
    if(next==old)return;
    d->capture=next;
    if(old)jsdom_dispatch(d,old,"lostpointercapture");
    if(next)jsdom_dispatch(d,next,"gotpointercapture");
}

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
    J->on_unhandled = host_unhandled;
    /* shadow roots the page's HTML declared (<template shadowrootmode>) */
    for (struct node *n = d->h.root; n; n = node_next_shadow(n, d->h.root))
        if (n->shadow_root) {
            struct shadow_scope *scope = arena_alloc(d->h.arena, sizeof(*scope));
            if (scope) {
                scope->root = n->shadow_root;
                scope->next = d->shadows;
                d->shadows = scope;
            }
        }
    js_gc_root(J, d, sizeof(*d));       /* the window, prototypes, timers and requests */
    struct js_gc_hooks hooks = { d, gc_mark, gc_more, gc_sweep };
    js_gc_hooks(J, &hooks);
    struct js_modhost mh = { d, dom_fetch_module, dom_resolve, dom_module_done };
    js_module_host(J, &mh);
    struct jsobj *w = d->window = js_global(J);
    jsval wv = js_obj(w);

    /* nodes: Node, and Text, DocumentFragment and the elements under it */
    struct jsobj *np = d->node_proto = js_new_object(J);
    static const struct {
        const char *name;
        js_native fn;
        int n;
    } node_methods[] = {
        {"appendChild", el_append_child, 1},
        {"insertBefore", el_insert_before, 2},
        {"removeChild", el_remove_child, 1},
        {"replaceChild", el_replace_child, 2},
        {"remove", el_remove, 0},
        {"before", el_before, 1},
        {"after", el_after, 1},
        {"replaceWith", el_replace_with, 1},
        {"cloneNode", el_clone_node, 1},
        {"contains", el_contains, 1},
        {"hasChildNodes", el_has_child_nodes, 0},
        {"attachShadow", el_attach_shadow, 1},
        {"assignedNodes", el_assigned_nodes, 0},
        {"compareDocumentPosition", el_compare_position, 1},
        {"getRootNode", el_get_root_node, 0},
        {"isEqualNode", el_is_equal_node, 1},
        {"isSameNode", el_is_same_node, 1},
        {"normalize", el_normalize, 0},
        {"addEventListener", add_event_listener, 2},
        {"removeEventListener", remove_event_listener, 2},
        {"dispatchEvent", el_dispatch_event, 1},
        {"toString", node_to_string, 0},
    };
    for (unsigned i = 0; i < sizeof(node_methods) / sizeof(node_methods[0]); i++)
        fn(J, np, node_methods[i].name, node_methods[i].fn, node_methods[i].n);
    static const char *const node_types[] = {
        "ELEMENT_NODE", "ATTRIBUTE_NODE", "TEXT_NODE", "CDATA_SECTION_NODE", "ENTITY_REFERENCE_NODE",
        "ENTITY_NODE", "PROCESSING_INSTRUCTION_NODE", "COMMENT_NODE", "DOCUMENT_NODE",
        "DOCUMENT_TYPE_NODE", "DOCUMENT_FRAGMENT_NODE", "NOTATION_NODE",
    };
    static const char *const positions[] = {
        "DOCUMENT_POSITION_DISCONNECTED", "DOCUMENT_POSITION_PRECEDING", "DOCUMENT_POSITION_FOLLOWING",
        "DOCUMENT_POSITION_CONTAINS", "DOCUMENT_POSITION_CONTAINED_BY",
        "DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC",
    };
    for (int i = 0; i < 12; i++)
        js_put(J, np, S(J, node_types[i]), js_num(i + 1), 1);
    for (int i = 0; i < 6; i++)
        js_put(J, np, S(J, positions[i]), js_num(1 << i), 1);
    struct jsobj *ep = d->element_proto = js_new_object(J);
    ep->proto = np;
    static const struct { const char *name; js_native fn; int n; } element_methods[] = {
        { "append", el_append, 1 }, { "prepend", el_prepend, 1 },
        { "replaceChildren", el_replace_children, 0 }, { "getAttribute", el_get_attribute, 1 },
        { "setAttribute", el_set_attribute, 2 }, { "removeAttribute", el_remove_attribute, 1 },
        { "hasAttribute", el_has_attribute, 1 }, { "toggleAttribute", el_toggle_attribute, 1 },
        { "getAttributeNames", el_get_attribute_names, 0 }, { "hasAttributes", el_has_attributes, 0 },
        { "querySelector", el_query_selector, 1 }, { "querySelectorAll", el_query_selector_all, 1 },
        { "getElementsByTagName", el_get_by_tag, 1 }, { "getElementsByClassName", el_get_by_class, 1 },
        { "matches", el_matches, 1 }, { "webkitMatchesSelector", el_matches, 1 }, { "closest", el_closest, 1 },
        { "getBoundingClientRect", el_get_bounding_rect, 0 },
        { "insertAdjacentText", el_insert_adjacent_text, 2 },
        { "insertAdjacentElement", el_insert_adjacent_element, 2 },
        { "insertAdjacentHTML", el_insert_adjacent_html, 2 },
        { "click", el_click, 0 }, { "focus", el_focus, 0 }, { "blur", el_blur, 0 },
        { "select", nothing, 0 }, { "scrollIntoView", el_scroll_into_view, 0 },
        { "setSelectionRange", nothing, 2 }, { "submit", form_submit, 0 },
        { "requestSubmit", form_request_submit, 0 }, { "reset", form_reset, 0 },
        { "showPopover", el_show_popover, 0 }, { "hidePopover", el_hide_popover, 0 },
        { "togglePopover", el_toggle_popover, 0 },
    };
    for (unsigned i = 0; i < sizeof(element_methods) / sizeof(element_methods[0]); i++)
        fn(J, ep, element_methods[i].name, element_methods[i].fn, element_methods[i].n);
    struct jsobj *cdp = js_new_object(J);
    cdp->proto = np;
    d->text_proto = js_new_object(J);
    d->text_proto->proto = cdp;
    struct jsobj *fp = d->frag_proto = js_new_object(J);
    fp->proto = np;
    fn(J, fp, "append", el_append, 1);
    fn(J, fp, "prepend", el_prepend, 1);
    fn(J, fp, "replaceChildren", el_replace_children, 0);
    fn(J, fp, "querySelector", el_query_selector, 1);
    fn(J, fp, "querySelectorAll", el_query_selector_all, 1);
    fn(J, fp, "getElementById", frag_get_by_id, 1);
    fn(J, cdp, "remove", el_remove, 0);

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
    fn(J, clp, "toString", classlist_to_string, 0);
    d->style_proto = stp;
    d->classlist_proto = clp;

    /* events: Event, and the kinds that inherit from it */
    static const struct { const char *name; int parent; js_native ctor; } kinds[] = {
#define X(id, name, parent) { name, parent, ctor_##id },
        EVENT_KINDS(X)
#undef X
    };
    for (int k = 0; k < EV_COUNT; k++) {
        struct jsobj *proto = js_new_object(J);
        if (kinds[k].parent >= 0)
            proto->proto = d->ev_protos[kinds[k].parent];
        d->ev_protos[k] = proto;
        jsval c = js_new_function(J, kinds[k].ctor, kinds[k].name, 2);
        c.u.o->ctor = 1;
        js_set(J, c, "prototype", js_obj(proto));
        js_put(J, proto, S(J, "constructor"), c, 1);
        js_set(J, wv, kinds[k].name, c);
        if (k == EV_EVENT) {
            static const char *const phases[] = { "NONE", "CAPTURING_PHASE", "AT_TARGET", "BUBBLING_PHASE" };
            for (int i = 0; i < 4; i++) {
                js_set(J, c, phases[i], js_num(i));
                js_put(J, proto, S(J, phases[i]), js_num(i), 1);
            }
        }
    }
    struct jsobj *evp = d->event_proto = d->ev_protos[EV_EVENT];
    fn(J, evp, "preventDefault", event_prevent_default, 0);
    fn(J, evp, "stopPropagation", event_stop_propagation, 0);
    fn(J, evp, "stopImmediatePropagation", event_stop_immediate, 0);
    fn(J, evp, "initEvent", event_init, 3);
    fn(J, evp, "composedPath", event_composed_path, 0);
    fn(J, d->ev_protos[EV_CUSTOM], "initCustomEvent", event_init, 4);
    js_put(J, evp, S(J, "composed"), js_bool(0), 1);
    js_put(J, evp, S(J, "returnValue"), js_bool(1), 1);
    js_put(J, evp, S(J, "cancelBubble"), js_bool(0), 1);
    js_put(J, d->ev_protos[EV_UI], S(J, "detail"), js_num(0), 1);
    js_put(J, d->ev_protos[EV_UI], S(J, "view"), wv, 1);
    js_put(J, d->ev_protos[EV_CUSTOM], S(J, "detail"), js_null, 1);
    static const char *const zero_mouse[] = { "clientX", "clientY", "screenX", "screenY", "pageX", "pageY",
                                             "offsetX", "offsetY", "movementX", "movementY", "x", "y",
                                             "button", "buttons", NULL };
    static const char *const modifiers[] = { "ctrlKey", "shiftKey", "altKey", "metaKey", NULL };
    for (int i = 0; zero_mouse[i]; i++)
        js_put(J, d->ev_protos[EV_MOUSE], S(J, zero_mouse[i]), js_num(0), 1);
    for (int i = 0; modifiers[i]; i++) {
        js_put(J, d->ev_protos[EV_MOUSE], S(J, modifiers[i]), js_bool(0), 1);
        js_put(J, d->ev_protos[EV_KEYBOARD], S(J, modifiers[i]), js_bool(0), 1);
        js_put(J, d->ev_protos[EV_TOUCH], S(J, modifiers[i]), js_bool(0), 1);
    }
    fn(J, d->ev_protos[EV_MOUSE], "getModifierState", js_false, 1);
    fn(J, d->ev_protos[EV_KEYBOARD], "getModifierState", js_false, 1);
    js_put(J, d->ev_protos[EV_MOUSE], S(J, "relatedTarget"), js_null, 1);
    js_put(J, d->ev_protos[EV_FOCUS], S(J, "relatedTarget"), js_null, 1);
    js_put(J, d->ev_protos[EV_POINTER], S(J, "pointerId"), js_num(1), 1);
    js_put(J, d->ev_protos[EV_POINTER], S(J, "pointerType"), js_str(J, "mouse"), 1);
    js_put(J, d->ev_protos[EV_POINTER], S(J, "isPrimary"), js_bool(1), 1);
    js_put(J, d->ev_protos[EV_POINTER], S(J, "width"), js_num(1), 1);
    js_put(J, d->ev_protos[EV_POINTER], S(J, "height"), js_num(1), 1);
    js_put(J, d->ev_protos[EV_POINTER], S(J, "pressure"), js_num(0), 1);
    static const char *const key_strings[] = { "key", "code", NULL };
    for (int i = 0; key_strings[i]; i++)
        js_put(J, d->ev_protos[EV_KEYBOARD], S(J, key_strings[i]), js_str(J, ""), 1);
    js_put(J, d->ev_protos[EV_KEYBOARD], S(J, "keyCode"), js_num(0), 1);
    js_put(J, d->ev_protos[EV_KEYBOARD], S(J, "charCode"), js_num(0), 1);
    js_put(J, d->ev_protos[EV_KEYBOARD], S(J, "location"), js_num(0), 1);
    js_put(J, d->ev_protos[EV_KEYBOARD], S(J, "repeat"), js_bool(0), 1);
    js_put(J, d->ev_protos[EV_KEYBOARD], S(J, "isComposing"), js_bool(0), 1);
    js_put(J, d->ev_protos[EV_INPUT], S(J, "data"), js_null, 1);
    js_put(J, d->ev_protos[EV_INPUT], S(J, "inputType"), js_str(J, "insertText"), 1);
    js_put(J, d->ev_protos[EV_INPUT], S(J, "isComposing"), js_bool(0), 1);
    js_put(J, d->ev_protos[EV_SUBMIT], S(J, "submitter"), js_null, 1);
    js_put(J, d->ev_protos[EV_POPSTATE], S(J, "state"), js_null, 1);
    js_put(J, d->ev_protos[EV_MESSAGE], S(J, "data"), js_null, 1);
    js_put(J, d->ev_protos[EV_MESSAGE], S(J, "origin"), js_str(J, ""), 1);

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
        { "execCommand", js_false, 1 }, { "createEvent", doc_create_event, 1 },
        { "importNode", doc_import_node, 2 }, { "adoptNode", doc_adopt_node, 1 },
        { "elementFromPoint", doc_element_from_point, 2 },
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
    js_set(J, nv, "userAgent", js_str(J, BROWSER_USER_AGENT));
    js_set(J, nv, "appName", js_str(J, "Netscape"));
    /* as the User-Agent says: a Gecko browser on 32-bit Linux, in US English (Accept-Language too) */
    js_set(J, nv, "appVersion", js_str(J, "5.0 (X11)"));
    js_set(J, nv, "platform", js_str(J, "Linux i686"));
    js_set(J, nv, "oscpu", js_str(J, "Linux i686"));
    js_set(J, nv, "productSub", js_str(J, "20100101"));
    js_set(J, nv, "buildID", js_str(J, "20181001000000"));
    js_set(J, nv, "language", js_str(J, "en-US"));
    struct jsobj *langs = js_new_array(J);
    js_array_push(J, langs, js_str(J, "en-US"));
    js_array_push(J, langs, js_str(J, "en"));
    js_set(J, nv, "languages", js_obj(langs));
    js_set(J, nv, "cookieEnabled", js_bool(1));
    js_set(J, nv, "onLine", js_bool(1));
    js_set(J, nv, "vendor", js_str(J, ""));
    fn(J, nav, "sendBeacon", nav_send_beacon, 2);
    js_set(J, wv, "navigator", nv);

    fn(J,d->document,"__fontRegister",document_font_register,4);
    fn(J,d->document,"__fontStatus",document_font_status,1);
    struct jsobj *scr = js_new_object(J);
    int vw = 800, vh = 600;
    if (d->h.viewport)
        d->h.viewport(d->h.ctx, &vw, &vh);
    js_set(J, js_obj(scr), "width", js_num(1024));
    js_set(J, js_obj(scr), "height", js_num(768));
    js_set(J, js_obj(scr), "availWidth", js_num(1024));
    js_set(J, js_obj(scr), "availHeight", js_num(740));
    js_set(J, js_obj(scr), "colorDepth", js_num(24));
    js_set(J, js_obj(scr), "pixelDepth", js_num(24));
    d->last_vw = vw;
    d->last_vh = vh;
    js_set(J, wv, "screen", js_obj(scr));

    struct jsobj *hist = d->history = js_new_object(J);
    d->states = js_new_object(J);
    d->hist_len = 1;
    fn(J, hist, "back", history_back, 0);
    fn(J, hist, "forward", history_forward, 0);
    fn(J, hist, "go", history_go, 1);
    fn(J, hist, "pushState", history_push, 3);
    fn(J, hist, "replaceState", history_replace, 3);
    accessor(J, hist, "state", history_state, NULL);
    accessor(J, hist, "length", history_length, NULL);
    js_set(J, js_obj(hist), "scrollRestoration", js_str(J, "auto"));
    js_set(J, wv, "history", js_obj(hist));
    js_put(J, w, S(J, "%watchMutations"), js_new_function(J, win_watch_mutations, "watchMutations", 1), 1);
    js_put(J, w, S(J, "%watchView"), js_new_function(J, win_watch_view, "watchView", 1), 1);
    js_set(J, wv, "localStorage", make_storage(d, 0));
    js_set(J, wv, "sessionStorage", make_storage(d, 1));

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
    fn(J, xp, "setRequestHeader", xhr_set_header, 2);
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
        { "open", win_open, 2 }, { "close", win_close, 0 }, { "focus", nothing, 0 }, { "blur", nothing, 0 },
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
    jsval node_ctor = js_new_function(J, nothing, "Node", 0);
    js_set(J, node_ctor, "prototype", js_obj(np));
    js_put(J, np, S(J, "constructor"), node_ctor, 1);
    js_set(J, wv, "Node", node_ctor);
    for (int i = 0; i < 12; i++)
        js_set(J, node_ctor, node_types[i], js_num(i + 1));
    for (int i = 0; i < 6; i++)
        js_set(J, node_ctor, positions[i], js_num(1 << i));
    jsval el_ctor = js_new_function(J, html_element_ctor, "HTMLElement", 0);
    el_ctor.u.o->ctor = 1;
    js_put(J, w, S(J, "%watchCustom"), js_new_function(J, win_watch_custom, "watchCustom", 1), 1);
    js_put(J, w, S(J, "%cssSupports"), js_new_function(J, win_css_supports, "cssSupports", 1), 1);
    js_set(J, el_ctor, "prototype", js_obj(ep));
    js_put(J, ep, S(J, "constructor"), el_ctor, 1);
    js_set(J, wv, "HTMLElement", el_ctor);
    js_set(J, wv, "Element", el_ctor);
    static const char *const plain[] = { "CharacterData", "Text", "DocumentFragment", "Document", "HTMLDocument" };
    struct jsobj *plain_protos[] = { cdp, d->text_proto, fp, dp, dp };
    for (int i = 0; i < 5; i++) {
        jsval c = js_new_function(J, nothing, plain[i], 0);
        js_set(J, c, "prototype", js_obj(plain_protos[i]));
        js_put(J, plain_protos[i], S(J, "constructor"), c, 1);
        js_set(J, wv, plain[i], c);
    }
    dp->proto = np;
    /* HTMLInputElement and the rest: the tags each interface is for */
    static const struct { const char *name; const char *tags; } ifaces[] = {
        { "HTMLAnchorElement", "a" }, { "HTMLAreaElement", "area" }, { "HTMLAudioElement", "audio" },
        { "HTMLBaseElement", "base" }, { "HTMLBodyElement", "body" }, { "HTMLBRElement", "br" },
        { "HTMLButtonElement", "button" }, { "HTMLCanvasElement", "canvas" },
        { "HTMLTableCaptionElement", "caption" }, { "HTMLTableColElement", "col colgroup" },
        { "HTMLModElement", "del ins" }, { "HTMLDetailsElement", "details" }, { "HTMLDialogElement", "dialog" },
        { "HTMLDivElement", "div" }, { "HTMLDListElement", "dl" }, { "HTMLEmbedElement", "embed" },
        { "HTMLFieldSetElement", "fieldset" }, { "HTMLFormElement", "form" },
        { "HTMLHeadingElement", "h1 h2 h3 h4 h5 h6" }, { "HTMLHeadElement", "head" }, { "HTMLHRElement", "hr" },
        { "HTMLHtmlElement", "html" }, { "HTMLIFrameElement", "iframe" }, { "HTMLImageElement", "img" },
        { "HTMLInputElement", "input" }, { "HTMLLabelElement", "label" }, { "HTMLLegendElement", "legend" },
        { "HTMLLIElement", "li" }, { "HTMLLinkElement", "link" }, { "HTMLMapElement", "map" },
        { "HTMLMenuElement", "menu" }, { "HTMLMetaElement", "meta" }, { "HTMLObjectElement", "object" },
        { "HTMLOListElement", "ol" }, { "HTMLOptGroupElement", "optgroup" }, { "HTMLOptionElement", "option" },
        { "HTMLParagraphElement", "p" }, { "HTMLParamElement", "param" }, { "HTMLPictureElement", "picture" },
        { "HTMLPreElement", "pre listing xmp" }, { "HTMLQuoteElement", "q blockquote" },
        { "HTMLScriptElement", "script" }, { "HTMLSelectElement", "select" }, { "HTMLSourceElement", "source" },
        { "HTMLSpanElement", "span" }, { "HTMLStyleElement", "style" }, { "HTMLTableElement", "table" },
        { "HTMLTableSectionElement", "tbody thead tfoot" }, { "HTMLTableCellElement", "td th" },
        { "HTMLTemplateElement", "template" }, { "HTMLTextAreaElement", "textarea" },
        { "HTMLTimeElement", "time" }, { "HTMLTitleElement", "title" }, { "HTMLTableRowElement", "tr" },
        { "HTMLTrackElement", "track" }, { "HTMLUListElement", "ul" }, { "HTMLVideoElement", "video" },
        { "SVGElement", "svg" },
    };
    for (unsigned i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
        struct jsobj *proto = js_new_object(J);
        proto->proto = ep;
        jsval c = js_new_function(J, nothing, ifaces[i].name, 0);
        js_set(J, c, "prototype", js_obj(proto));
        js_put(J, proto, S(J, "constructor"), c, 1);
        js_set(J, wv, ifaces[i].name, c);
        for (const char *t = ifaces[i].tags; *t;) {
            int len = (int)strcspn(t, " ");
            int tag = tag_lookup(t, len);
            if (tag > 0)
                d->tag_protos[tag] = proto;
            t += len;
            while (*t == ' ')
                t++;
        }
        if (!strcmp(ifaces[i].name, "HTMLCanvasElement"))
            jscanvas_install(J, proto);
        if (!strcmp(ifaces[i].name, "HTMLDialogElement")) {
            fn(J, proto, "show", dialog_show, 0);
            fn(J, proto, "showModal", dialog_show_modal, 0);
            fn(J, proto, "close", dialog_close, 1);
            fn(J, proto, "requestClose", dialog_request_close_api, 1);
            js_put(J, proto, S(J, "returnValue"), js_str(J, ""), 0);
        }
    }
    jsval media_ctor = js_get(J, wv, "HTMLAudioElement");   /* HTMLMediaElement: audio and video */
    if (media_ctor.t == JS_OBJ)
        js_set(J, wv, "HTMLMediaElement", media_ctor);
    struct jsobj *lazy_on[] = { w, dp, ep, d->text_proto, d->frag_proto };     /* W, D, E, T */
    enum { W, D, E, T, F };
    static const struct { int where; const char *name; js_native get, set; } lazy[] = {
#define X(where, name) { where, #name, lazy_get_##where##_##name, lazy_set_##where##_##name },
        DOM_LIB_NAMES(X)
#undef X
    };
    for (unsigned i = 0; i < sizeof(lazy) / sizeof(lazy[0]); i++)
        accessor(J, lazy_on[lazy[i].where], lazy[i].name, lazy[i].get, lazy[i].set);
    fn(J,w,"%blobURL",blob_url_api,2);
    fn(J,w,"%adoptSheets",adopt_sheets,2);
    fn(J,w,"%clipboard",clipboard_text,1);
    fn(J,w,"%chosenFile",chosen_file,1);
    fn(J,w,"%pointerCapture",pointer_capture_api,2);
    fn(J,w,"%media",media_api,3);
    fn(J,w,"%winop",window_op,4);
    fn(J,w,"%b64",bytes_base64,1);
    fn(J,w,"%unb64",base64_bytes,1);
    fn(J,w,"%idb",idb_store,3);
    if (js_eval(J, js_dom_eager, (int)sizeof(js_dom_eager) - 1, "DOM library") < 0)
        fprintf(stderr, "DOM library: %s\n", js_error(J));
    char origin[300]="null";web_origin(d->h.url,origin,sizeof(origin));
    d->network=jsnet_new(J,d->h.base?d->h.base:d->h.url,origin,d,network_event,network_cookies);
    d->workers=jsworkers_new(J,&d->h,d);
    return d;
}

void jsdom_free(struct jsdom *d)
{
    if (!d)
        return;
    web_blob_free(d);
    for (int i = 0; i < d->nposted; i++)
        free(d->posted[i].data), free(d->posted[i].origin);
    free(d->posted);
    jsworkers_free(d->workers);
    jsnet_free(d->network);
    jscanvas_free(d->J);
    js_free(d->J);
    free(d->wraps);
    for (int i = 0; i < d->nim; i++) {
        free(d->im_from[i]);
        free(d->im_to[i]);
    }
    free(d->im_from);
    free(d->im_to);
    free(d);
}

/* ---- modules ---- */

static int url_like(const char *s)
{
    if (s[0] == '/' || !strncmp(s, "./", 2) || !strncmp(s, "../", 3))
        return 1;
    const char *p = s;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')
        p++;
    return p > s && *p == ':';
}

static void dom_fetch_module(void *ctx, int id, const char *url)
{
    struct jsdom *d = ctx;
    if (d->h.fetch_module)
        d->h.fetch_module(d->h.ctx, id, url);
    else
        js_module_source(d->J, id, NULL, 0);
}

/* A specifier as imported at base (a module's URL, or a script's src or name):
 * through the import map, then against base. */
static int dom_resolve(void *ctx, const char *spec, const char *base, char *out, int size)
{
    struct jsdom *d = ctx;
    char from[URL_MAX], key[URL_MAX];
    url_resolve(d->h.base, base, from, sizeof(from));
    if (url_like(spec))
        url_resolve(from, spec, key, sizeof(key));
    else
        snprintf(key, sizeof(key), "%s", spec);
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < d->nim; i++) {
        size_t l = strlen(d->im_from[i]);
        if (!strcmp(key, d->im_from[i])) {
            best = i;
            best_len = l;
            break;
        }
        if (l > best_len && d->im_from[i][l - 1] == '/' && !strncmp(key, d->im_from[i], l))
            best = i, best_len = l;
    }
    if (best >= 0) {
        snprintf(out, (size_t)size, "%s%s", d->im_to[best], key + best_len);
        return 0;
    }
    if (!url_like(spec))
        return -1;                      /* a bare name the map does not know */
    snprintf(out, (size_t)size, "%s", key);
    return 0;
}

static void dom_module_done(void *ctx, void *tag, int failed)
{
    struct jsdom *d = ctx;
    if (failed)
        report(d, "module script");
    settle(d);
    if (d->h.module_done)
        d->h.module_done(d->h.ctx, (int)(intptr_t)tag);
}

void jsdom_run_module(struct jsdom *d, const char *url, int inline_src, const char *text, int len, int tag)
{
    if (!d)
        return;
    d->depth++;
    js_run_module(d->J, url, inline_src, text, len, (void *)(intptr_t)tag);
    d->depth--;
    settle(d);
}

void jsdom_module_fetched(struct jsdom *d, int id, const char *text, int len)
{
    if (!d)
        return;
    if (id >= JSWORKER_ID_BASE) {
        jsworkers_module_fetched(d->workers, id, text, len);
        return;
    }
    d->depth++;
    js_module_source(d->J, id, text, len);
    d->depth--;
    settle(d);
}

/* A JSON string at *p (after its opening quote), unescaped into a new string. */
static char *json_string(const char **p, const char *end)
{
    char *out = malloc((size_t)(end - *p) + 1);
    int n = 0;
    const char *s = *p;
    if (!out)
        return NULL;
    while (s < end && *s != '"') {
        if (*s == '\\' && s + 1 < end) {
            s++;
            char c = *s++;
            if (c == 'u' && end - s >= 4) {
                char hex[5] = { s[0], s[1], s[2], s[3], 0 };
                n += utf8_put(out + n, (int)strtol(hex, NULL, 16));
                s += 4;
            } else {
                out[n++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' :
                           c == 'f' ? '\f' : c;
            }
        } else {
            out[n++] = *s++;
        }
    }
    out[n] = 0;
    *p = s < end ? s + 1 : s;
    return out;
}

void jsdom_import_map(struct jsdom *d, const char *json, int len)
{
    if (!d)
        return;
    const char *end = json + len, *p = json;
    /* find "imports": { ... } at the top level (scopes are not looked at) */
    int depth = 0;
    for (; p < end; p++) {
        if (*p == '{')
            depth++;
        else if (*p == '}')
            depth--;
        else if (*p == '"') {
            const char *q = p + 1;
            char *key = json_string(&q, end);
            int is_imports = key && depth == 1 && !strcmp(key, "imports");
            free(key);
            p = q - 1;
            if (!is_imports)
                continue;
            while (p < end && *p != '{')
                p++;
            break;
        }
    }
    if (p >= end)
        return;
    p++;
    while (p < end && *p != '}') {
        if (*p != '"') {
            p++;
            continue;
        }
        p++;
        char *from = json_string(&p, end);
        while (p < end && *p != '"' && *p != '}')
            p++;
        if (p >= end || *p != '"') {
            free(from);
            break;
        }
        p++;
        char *to = json_string(&p, end);
        char **nf = realloc(d->im_from, sizeof(char *) * (size_t)(d->nim + 1));
        if (nf)
            d->im_from = nf;
        char **nt = realloc(d->im_to, sizeof(char *) * (size_t)(d->nim + 1));
        if (nt)
            d->im_to = nt;
        if (from && to && nf && nt) {
            char abs[URL_MAX];
            if (url_like(from)) {
                url_resolve(d->h.base, from, abs, sizeof(abs));
                free(from);
                from = strdup(abs);
            }
            url_resolve(d->h.base, to, abs, sizeof(abs));
            free(to);
            to = strdup(abs);
            if (from && to) {
                d->im_from[d->nim] = from;
                d->im_to[d->nim] = to;
                d->nim++;
                continue;
            }
        }
        free(from);
        free(to);
    }
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
    jsnet_poll(d->network);
    jsworkers_poll(d->workers);
    process_view(d);
    run_queued(d);
    deliver_posted(d);
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
    int sx, sy, vw, vh;
    if (d->nqueued || d->nposted || view_changes(d, &sx, &sy, &vw, &vh))
        return 0;
    unsigned int now = uptime_ms();
    int best=jsnet_wait(d->network),w=jsworkers_wait(d->workers);
    if(w>=0&&(best<0||w<best))best=w;
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
