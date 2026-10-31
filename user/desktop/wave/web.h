/*
 * The web browser's engine: an HTML parser that builds a document tree,
 * a CSS parser and cascade that give every element a computed style,
 * and a layout pass that turns the styled tree into a list of things to
 * paint. browser.c wraps it in a window and fetches pages with wget.
 *
 * All arithmetic here is integer (desktop code is built without the FPU):
 * CSS numbers are parsed as fixed point with two decimals, lengths are
 * resolved to whole pixels. Only the JavaScript engine (js.h), which
 * needs doubles (and svg.c, which draws SVG), is compiled with the FPU.
 */
#ifndef WEB_H
#define WEB_H

#include <stddef.h>
#include <stdint.h>
#include "gfx.h"

#define URL_MAX 2048
/* How the browser names itself: in its requests, and to scripts (navigator.userAgent). In the form
 * Gecko browsers use, so sites give it their standard pages rather than ones for unknown clients; Wave
 * is named at the end. The Gecko version is an extended-support release: keep it a supported one. */
#define BROWSER_GECKO_VERSION "140.0"
#define BROWSER_USER_AGENT \
    "Mozilla/5.0 (X11; Linux i686; rv:" BROWSER_GECKO_VERSION ") Gecko/20100101 Firefox/" BROWSER_GECKO_VERSION " Wave/1.0"
/* Where what is typed in the address bar that is not an address goes. */
#define SEARCH_URL "https://html.duckduckgo.com/html/?q="

/* ---- memory: everything a page owns lives in arenas, freed at once ---- */
struct arena {
    struct arena_chunk *chunks;
    size_t used;                /* bytes handed out, for the limits */
};
void *arena_alloc(struct arena *a, size_t n);      /* zeroed; NULL when memory runs out */
char *arena_strndup(struct arena *a, const char *s, size_t n);
char *arena_strdup(struct arena *a, const char *s);
void arena_free(struct arena *a);

/* ---- text ---- */
/* Decode one UTF-8 character, advancing *p (invalid bytes decode as Latin-1). */
int utf8_next(const char **p, const char *end);
int utf8_put(char *out, int cp);                   /* returns bytes written (1-4) */

/* ---- fonts (webfont.c) ---- */
enum { FAM_SANS, FAM_SERIF, FAM_MONO };
struct wfont;
int wfont_register(const unsigned char *bytes,size_t length,int family,int bold,int italic);
void wfont_release(int family);
int wfont_init(void);                               /* load the font file; 0 on success */
const struct wfont *wfont_get(int family, int bold, int italic, int px);
int wfont_height(const struct wfont *f);
int wfont_ascent(const struct wfont *f);
int wfont_size(const struct wfont *f);
int wfont_width(const struct wfont *f, const char *s, int n);         /* UTF-8 */
/* Draw UTF-8 text with its top at y; returns the width drawn. */
int wfont_draw(struct surface *s, const struct wfont *f, int x, int y, const char *str, int n,
               color_t c);

/* Text at any size, for <canvas>: a mask (coverage 0-255, w x h, the baseline asc rows down, the pen
 * starting pad columns in; free cov), the width in 1/16 pixels, and the font's extent. */
struct wmask {
    int w, h, ascent, descent, pad;
    uint8_t *cov;
};
int wfont_measure(int family, int bold, int italic, int px, const char *s, int n);
int wfont_mask(int family, int bold, int italic, int px, const char *s, int n, struct wmask *out);
void wfont_vmetrics(int family, int bold, int italic, int px, int *ascent, int *descent);

/* A piece of the page's text that a search found: item, the characters of it, and which match it belongs to. */
struct fhl {
    int item, c0, c1, match;
};

/* ---- the document tree (html.c) ---- */
#define TAG_LIST(X) \
    X(A, "a") X(ABBR, "abbr") X(ACRONYM, "acronym") X(ADDRESS, "address") X(AREA, "area") \
    X(ARTICLE, "article") X(ASIDE, "aside") X(AUDIO, "audio") X(B, "b") X(BASE, "base") \
    X(BIG, "big") X(BLOCKQUOTE, "blockquote") X(BODY, "body") X(BR, "br") X(BUTTON, "button") \
    X(CANVAS, "canvas") X(CAPTION, "caption") X(CENTER, "center") X(CITE, "cite") \
    X(CODE, "code") X(COL, "col") X(COLGROUP, "colgroup") X(DD, "dd") X(DEL, "del") \
    X(DETAILS, "details") X(DFN, "dfn") X(DIALOG, "dialog") X(DIR, "dir") X(DIV, "div") \
    X(DL, "dl") X(DT, "dt") X(EM, "em") X(EMBED, "embed") X(FIELDSET, "fieldset") \
    X(FIGCAPTION, "figcaption") X(FIGURE, "figure") X(FONT, "font") X(FOOTER, "footer") \
    X(FORM, "form") X(FRAME, "frame") X(FRAMESET, "frameset") X(H1, "h1") X(H2, "h2") \
    X(H3, "h3") X(H4, "h4") X(H5, "h5") X(H6, "h6") X(HEAD, "head") X(HEADER, "header") \
    X(HGROUP, "hgroup") X(HR, "hr") X(HTML, "html") X(I, "i") X(IFRAME, "iframe") \
    X(IMG, "img") X(INPUT, "input") X(INS, "ins") X(KBD, "kbd") X(LABEL, "label") \
    X(LEGEND, "legend") X(LI, "li") X(LINK, "link") X(LISTING, "listing") X(MAIN, "main") \
    X(MAP, "map") X(MARK, "mark") X(MENU, "menu") X(META, "meta") X(METER, "meter") X(NAV, "nav") \
    X(NOBR, "nobr") X(NOFRAMES, "noframes") X(NOSCRIPT, "noscript") X(OBJECT, "object") \
    X(OL, "ol") X(OPTGROUP, "optgroup") X(OPTION, "option") X(P, "p") X(PARAM, "param") \
    X(PICTURE, "picture") X(PLAINTEXT, "plaintext") X(PRE, "pre") X(PROGRESS, "progress") \
    X(Q, "q") X(RB, "rb") X(RP, "rp") X(RT, "rt") X(RUBY, "ruby") X(S, "s") \
    X(SAMP, "samp") X(SCRIPT, "script") X(SECTION, "section") X(SELECT, "select") \
    X(SMALL, "small") X(SOURCE, "source") X(SPAN, "span") X(STRIKE, "strike") \
    X(STRONG, "strong") X(STYLE, "style") X(SUB, "sub") X(SUMMARY, "summary") X(SUP, "sup") \
    X(SVG, "svg") X(TABLE, "table") X(TBODY, "tbody") X(TD, "td") X(TEMPLATE, "template") \
    X(TEXTAREA, "textarea") X(TFOOT, "tfoot") X(TH, "th") X(THEAD, "thead") X(TIME, "time") \
    X(TITLE, "title") X(TR, "tr") X(TRACK, "track") X(TT, "tt") X(U, "u") X(UL, "ul") \
    X(VAR, "var") X(VIDEO, "video") X(WBR, "wbr") X(XMP, "xmp")

enum tag {
    T_UNKNOWN,
#define X(id, name) T_##id,
    TAG_LIST(X)
#undef X
    T_COUNT
};

enum { N_ELEMENT, N_TEXT };

struct attr {
    char *name;                 /* lower case */
    char *value;
    struct attr *next;
};

struct style;

struct anim_state;

struct node {
    uint8_t type;               /* N_ELEMENT or N_TEXT */
    uint8_t tag;                /* enum tag */
    char *name;                 /* element name, lower case */
    struct attr *attrs;
    char *text;                 /* text nodes: UTF-8, entities decoded */
    int len;
    struct node *parent, *first, *last, *next, *prev;
    struct node *shadow_root, *shadow_host;
    uint8_t shadow_closed;
    struct style *style;        /* computed style (elements) */
    int index;                  /* position among its element siblings, from 1 */

    /* form controls */
    char *value;                /* the current value of a text field (malloc'd) */
    int checked;                /* checkbox, radio; select: the chosen option */
    int cursor;                 /* caret position in value */
    int scroll_x, scroll_y;     /* a box with overflow: how far its contents are scrolled */

    /* images */
    struct image *img;

    /* layout: the element's intrinsic widths, cached for one layout pass */
    int mmin, mmax;
    unsigned mgen;

    uint8_t quirks;             /* the root: no standards-mode doctype */
    uint8_t file_chosen;        /* <input type=file>: value is a file the user picked (only that is sent) */

    /* generated content: ::before and ::after with a string "content" */
    struct style *pseudo_st[8];         /* ::before, ::after, ::placeholder, ::selection, ::marker,
                                           ::first-letter, ::first-line, ::backdrop */
    const char *pseudo_text[2];

    int bx0, by0, bx1, by1;             /* browser.c: its box in the last layout, for scripts */
    struct sheet *sheet;                /* browser.c: <style> and <link rel=stylesheet>: its sheet */
    uintptr_t sheet_sig;                /* what it was parsed from (<style>), or 1 once fetched (<link>) */
    const char *adopted_text;           /* constructed sheets, outside the DOM child list */
    struct node **assigned_nodes;
    int assigned_count;
    uint8_t slot_pending;
    unsigned bgen;                      /* the layout bx0... belong to */

    void *js;                           /* the script engine's wrapper (jsdom.c) */
    unsigned gcpass;                    /* jsdom.c: its tree's wrappers marked in this collection */

    /* css.c: what the last cascade learnt of the element */
    const struct nkeys *keys;           /* its id and classes, hashed (valid while kgen is current) */
    unsigned kgen;
    uint8_t style_flags;                /* SF_*: what its style turned out to depend on */
    uint32_t match_sig;                 /* the rules that matched it, hashed */
    unsigned dgen, rgen;                /* css_restyle_dynamic(): its state changed, its style changed */
    struct anim_state *anim;            /* anim.c: its transitions and animations, if it has any */
    /* container queries: a container's content box in the last layout (layout.c), and the size
     * the cascade took it to be (css.c); they differ until the page is styled again */
    int cq_w, cq_h, cq_aw, cq_ah;
    uint8_t cq_known, cq_asked;

    /* the top layer (jsdom.c): an open popover (POP_*), a dialog shown with showModal() */
    uint8_t popover_open, modal;
    unsigned top_seq;                   /* when it last went into the top layer, for their order */
    struct node *invoker;               /* an open popover: the button that opened it (its implicit anchor) */
    struct node *top_modal;             /* a document's root: its topmost modal dialog, which makes the rest inert */

    /* <audio>: what its sound is doing (browser.c), for the controls layout.c draws */
    uint8_t media_state;                /* MEDIA_* */
    int media_pos_ms, media_dur_ms;     /* where it is, and how long (-1: not known) */
};
enum { MEDIA_IDLE, MEDIA_LOADING, MEDIA_PLAYING, MEDIA_PAUSED, MEDIA_FAILED };
enum { POP_NONE, POP_AUTO, POP_MANUAL, POP_HINT };

struct node *html_parse(struct arena *a, const char *text, int len);
/* <template shadowrootmode> in a page's HTML: attach those shadow roots (after html_parse). */
void html_declarative_shadows(struct arena *a, struct node *root);
const char *node_attr(const struct node *n, const char *name);
struct node *node_next(struct node *n, const struct node *root);       /* pre-order */
struct shadow_swap;
struct shadow_swap *shadow_compose(struct arena *a, struct node *root);
void shadow_restore(struct shadow_swap *patches);
struct node *shadow_slot(struct node *host, const char *name);
int web_inert(const struct node *n);
int web_disabled(const struct node *n);
int web_tabindex(const struct node *n);
int web_focusable(const struct node *n);
struct node *web_focus_next(struct node *root,struct node *current,int dir);
struct node *node_next_shadow(struct node *n,const struct node *root);
struct node *node_after_shadow(struct node *n,const struct node *root);
struct node *node_find(struct node *root, enum tag tag);
int node_text(const struct node *n, char *out, int size);   /* text content, whitespace collapsed */
const char *tag_name(int tag);
int tag_lookup(const char *name, int len);

/* An+B for CSS and DOM selectors; filter is NULL or the selector list after "of". */
int web_parse_nth(const char *s, const char *end, int *a, int *b, const char **filter);
int web_nth_fits(int a, int b, int position);

/* ---- styles (css.c) ---- */
/* pseudo-elements: node.pseudo_st[PE_x - 1] is the style of an element's ::before and the rest */
enum { PE_NONE, PE_BEFORE, PE_AFTER, PE_PLACEHOLDER, PE_SELECTION, PE_MARKER, PE_FIRST_LETTER, PE_FIRST_LINE,
       PE_BACKDROP };
#define PE_COUNT 8

/* The fragment of the page's address, for :target (css.c; the browser sets it before cascading). */
extern const char *css_target_id;

enum unit { U_PX, U_PCT, U_AUTO, U_CALC, U_MIN, U_MAX };

struct len {
    int v;                      /* pixels, or percent times 100 */
    int pct;                    /* U_CALC: v pixels plus this percent (times 100);
                                   U_MIN, U_MAX: the smaller or larger of v pixels and it */
    uint8_t unit;
};

enum display {
    D_INLINE, D_BLOCK, D_LIST_ITEM, D_INLINE_BLOCK, D_TABLE, D_INLINE_TABLE, D_TABLE_ROW,
    D_TABLE_ROW_GROUP, D_TABLE_CELL, D_TABLE_CAPTION, D_NONE,
};
/* How a box lays out its children: display's inside, next to its outside above. */
enum { IN_FLOW, IN_FLEX, IN_GRID };
/* justify-content, align-items and the like; AL_NORMAL stretches where it can. */
enum {
    AL_AUTO, AL_NORMAL, AL_STRETCH, AL_START, AL_END, AL_CENTER, AL_BASELINE, AL_BETWEEN,
    AL_AROUND, AL_EVENLY, AL_ANCHOR_CENTER,
};
enum { TA_LEFT, TA_CENTER, TA_RIGHT, TA_JUSTIFY };
enum { WS_NORMAL, WS_PRE, WS_NOWRAP, WS_PRE_WRAP, WS_PRE_LINE };
enum { DECO_UNDERLINE = 1, DECO_STRIKE = 2, DECO_OVERLINE = 4 };
enum { DS_SOLID, DS_DOUBLE, DS_DOTTED, DS_DASHED, DS_WAVY };     /* text-decoration-style */
/* cursor: the pointer shapes a page can ask for */
enum {
    CSSCUR_AUTO, CSSCUR_DEFAULT, CSSCUR_POINTER, CSSCUR_TEXT, CSSCUR_WAIT, CSSCUR_PROGRESS, CSSCUR_CROSSHAIR, CSSCUR_MOVE,
    CSSCUR_HELP, CSSCUR_NOT_ALLOWED, CSSCUR_NONE, CSSCUR_RESIZE_NS, CSSCUR_RESIZE_EW, CSSCUR_RESIZE_ALL,
};
enum { WB_NORMAL, WB_BREAK_ALL, WB_BREAK_WORD };   /* word-break and overflow-wrap */
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAPITALIZE };
enum { VA_BASELINE, VA_SUB, VA_SUPER, VA_MIDDLE, VA_TOP, VA_BOTTOM, VA_TEXT_TOP };
enum {
    LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_LOWER_ALPHA, LS_UPPER_ALPHA,
    LS_LOWER_ROMAN, LS_UPPER_ROMAN, LS_NONE,
};
enum { BGS_AUTO, BGS_COVER, BGS_CONTAIN, BGS_LEN };             /* background-size */
enum { OF_FILL, OF_CONTAIN, OF_COVER, OF_NONE, OF_SCALE_DOWN };   /* object-fit */
enum { OV_VISIBLE, OV_HIDDEN, OV_SCROLL, OV_AUTO };               /* overflow-x and -y */
enum { BS_NONE, BS_SOLID, BS_DOTTED, BS_DASHED, BS_DOUBLE, BS_INSET, BS_OUTSET, BS_GROOVE, BS_RIDGE };
enum { LH_NORMAL, LH_FACTOR, LH_PX };
enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };

/* Colors carry alpha in the top byte: 0xFF000000 is opaque. */
#define OPAQUE(c) ((c) | 0xFF000000u)
#define TRANSPARENT 0x00000000u
#define ALPHA(c) ((c) >> 24)

/* grid-template-columns: sizes, after repeat(n, ...) is expanded. */
enum { TK_PX, TK_PCT, TK_FR, TK_AUTO, TK_MIN, TK_MAX };
#define MAX_TRACKS 24
struct track {
    uint8_t min, max;           /* TK_*: minmax(min, max); a plain size is both */
    int vmin, vmax;             /* pixels, percent times 100 or fr times 100 */
};
struct gridspec {
    int n;
    uint8_t subgrid;            /* tracks borrowed from the parent grid area */
    uint8_t auto_repeat;        /* 1 auto-fill, 2 auto-fit: tracks [rep, rep + nrep) repeat */
    int rep, nrep;
    struct track t[MAX_TRACKS];
    const char *line_names[MAX_TRACKS + 1];
};

/* grid-template-areas: named rectangles of cells (0-based, end exclusive). */
#define MAX_AREAS 32
struct gridareas {
    int n, nrows, ncols;
    struct { char name[24]; int r0, c0, r1, c1; } a[MAX_AREAS];
};

struct custom_prop {            /* a CSS custom property (--name: value) */
    const char *name;
    const char *value;
    struct custom_prop *next;
};

/* ---- gradients ---- */

/* linear-gradient(), radial-gradient() and conic-gradient(), and their repeating-* forms (parsed
 * by css.c, painted by layout.c as background layers, sized and tiled like pictures). Stop
 * positions are lengths or percentages along the gradient line (radial: along the ray; conic:
 * percentages of a turn); has_pos 0 means the position is worked out from its neighbours. */
enum { GK_LINEAR, GK_RADIAL, GK_CONIC };
enum { GX_FARTHEST_CORNER, GX_FARTHEST_SIDE, GX_CLOSEST_CORNER, GX_CLOSEST_SIDE };
#define MAX_GSTOPS 32
struct css_gradient {
    uint8_t kind;               /* GK_* */
    uint8_t repeating;
    uint8_t circle;             /* radial: a circle, not an ellipse */
    uint8_t extent;             /* radial: GX_* (unless rx is set) */
    int8_t corner_x, corner_y;  /* linear "to top right" and the like: -1, 0 or 1 (0, 0: by angle) */
    float angle;                /* linear: degrees clockwise from "to top"; conic: where it starts */
    struct len cx, cy;          /* radial and conic: the centre */
    struct len rx, ry;          /* radial: an explicit size (rx.unit U_AUTO: by extent) */
    int nstops;
    struct gstop {
        uint32_t color;
        struct len pos;
        uint8_t has_pos;
    } stops[MAX_GSTOPS];
};

/* ---- anchor positioning ---- */

/* anchor(--name side, fallback) in an inset, anchor-size(--name width, fallback) in a size (css.c
 * parses them, layout.c measures the anchor and puts the pixels in). */
enum { AS_TOP = -1, AS_RIGHT = -2, AS_BOTTOM = -3, AS_LEFT = -4, AS_START = -5, AS_END = -6, AS_CENTER = -7,
       AS_INSIDE = -8, AS_OUTSIDE = -9, AS_WIDTH = -10, AS_HEIGHT = -11, AS_SAME = -12 };
struct aref {
    const char *name;           /* the anchor's name, or NULL: the box's default anchor */
    int side;                   /* AS_*, or 0 to 10000: a percentage (times 100) of the way across it */
    int add;                    /* pixels added to it (calc(anchor(bottom) + 4px)) */
    struct len fallback;        /* what it is when there is no such anchor (U_AUTO: auto) */
    uint8_t set;
};
#define MAX_TRIES 6
enum { TRY_BLOCK = 1, TRY_INLINE = 2, TRY_START = 4 };
struct anchor_spec {
    struct aref inset[4];       /* top, right, bottom, left */
    struct aref size[2];        /* width, height */
    /* position-area: the rows from line area[0] to area[1] and the columns from area[2] to area[3]
     * of the grid the anchor's edges cut the containing block into (lines 0-3); area[1] 0: none */
    uint8_t area[4];
    int ntry;                   /* position-try-fallbacks: flips (TRY_*), or other areas, tried in order */
    struct { uint8_t flip, area[4]; } tries[MAX_TRIES];
};

/* ---- transforms, transitions and animations ---- */

/* Functions compose in written order; the rightmost acts on a point first. */
enum { TF_TRANSLATE, TF_SCALE, TF_ROTATE, TF_SKEW, TF_MATRIX, TF_MATRIX3D, TF_PERSPECTIVE };
struct tfop {
    uint8_t kind;
    uint8_t flip;               /* a 3D rotation that shows the back of the box */
    struct len x, y, z;         /* TF_TRANSLATE, including depth */
    float m[16];               /* column-major TF_MATRIX3D */
    float a, b, c, d, e, f;     /* TF_SCALE: a, b; TF_ROTATE: a (radians); TF_SKEW: a, b (radians); TF_MATRIX: a-f */
};
#define MAX_TFOPS 11            /* transform's eight, after translate, rotate and scale */
struct css_xform {
    int n;
    uint8_t flipped;            /* rotateX/rotateY turn its back to the viewer (backface-visibility) */
    struct tfop op[MAX_TFOPS];
};
/* filter and backdrop-filter: functions in order */
enum { FX_BLUR = 1, FX_BRIGHTNESS, FX_CONTRAST, FX_GRAYSCALE, FX_HUE, FX_INVERT, FX_OPACITY, FX_SATURATE,
       FX_SEPIA, FX_SHADOW };
#define MAX_FXOPS 8
struct css_filter {
    int n;
    struct fxop {
        uint8_t kind;
        float amount;           /* FX_BLUR: the radius in pixels; FX_HUE: radians; the rest: a factor */
        int dx, dy, blur;       /* FX_SHADOW */
        uint32_t color;         /* FX_SHADOW */
    } op[MAX_FXOPS];
};
/* clip-path: a basic shape over the border box */
enum { CLIP_INSET = 1, CLIP_CIRCLE, CLIP_ELLIPSE, CLIP_POLYGON };
#define MAX_CLIP_POINTS 32
struct css_clip {
    uint8_t kind;
    uint8_t rx_side, ry_side;   /* circle and ellipse radii: 0 a length, 1 closest-side, 2 farthest-side */
    struct len a[4];            /* inset: top, right, bottom, left; circle: r, -, cx, cy; ellipse: rx, ry, cx, cy */
    struct len round;           /* inset(... round r) */
    int npts;
    struct len px[MAX_CLIP_POINTS], py[MAX_CLIP_POINTS];
};
/* mix-blend-mode */
enum { BL_NORMAL, BL_MULTIPLY, BL_SCREEN, BL_OVERLAY, BL_DARKEN, BL_LIGHTEN, BL_DODGE, BL_BURN, BL_HARD,
       BL_SOFT, BL_DIFFERENCE, BL_EXCLUSION };
double css_transform_length(struct len l, int base);
void css_transform_matrix(const struct style *st, int w, int h, double out[6]);
void css_transform_matrix3d(const struct style *st, int w, int h, double out[16]);

/* A timing function: cubic-bezier(x1, y1, x2, y2) (linear, ease and the rest are special cases) or steps. */
enum { JUMP_START, JUMP_END, JUMP_NONE, JUMP_BOTH };
struct css_timing {
    uint8_t steps_kind;         /* 0 a cubic bezier, 1 steps */
    uint8_t jump;               /* JUMP_* */
    int steps;
    float x1, y1, x2, y2;
};

/* The properties that can be animated (the ones anim.c knows how to blend). AP_ALL in transition-property. */
enum anim_prop {
    AP_NONE = -2, AP_ALL = -1,
    AP_COLOR, AP_BACKGROUND_COLOR, AP_BORDER_TOP_COLOR, AP_BORDER_RIGHT_COLOR, AP_BORDER_BOTTOM_COLOR,
    AP_BORDER_LEFT_COLOR, AP_OUTLINE_COLOR, AP_OPACITY, AP_WIDTH, AP_HEIGHT, AP_MIN_WIDTH, AP_MAX_WIDTH,
    AP_MIN_HEIGHT, AP_MAX_HEIGHT, AP_MARGIN_TOP, AP_MARGIN_RIGHT, AP_MARGIN_BOTTOM, AP_MARGIN_LEFT,
    AP_PADDING_TOP, AP_PADDING_RIGHT, AP_PADDING_BOTTOM, AP_PADDING_LEFT, AP_TOP, AP_RIGHT, AP_BOTTOM, AP_LEFT,
    AP_BORDER_TOP_WIDTH, AP_BORDER_RIGHT_WIDTH, AP_BORDER_BOTTOM_WIDTH, AP_BORDER_LEFT_WIDTH, AP_FONT_SIZE,
    AP_LETTER_SPACING, AP_WORD_SPACING, AP_LINE_HEIGHT, AP_BORDER_TL_RADIUS, AP_BORDER_TR_RADIUS,
    AP_BORDER_BR_RADIUS, AP_BORDER_BL_RADIUS, AP_TRANSFORM, AP_TRANSFORM_ORIGIN, AP_BOX_SHADOW, AP_TEXT_SHADOW,
    AP_FLEX_GROW, AP_FLEX_SHRINK, AP_FLEX_BASIS, AP_ROW_GAP, AP_COLUMN_GAP, AP_TEXT_INDENT, AP_OUTLINE_WIDTH,
    AP_OUTLINE_OFFSET, AP_Z_INDEX, AP_VISIBILITY,
    AP_DISPLAY, AP_OVERLAY,     /* discrete: they only transition with transition-behavior: allow-discrete */
    AP_COUNT
};

#define AP_BIT(p) ((uint64_t)1 << (p))
#define AP_EVERY (AP_BIT(AP_COUNT) - 1)

struct css_transition {
    uint64_t props;             /* AP_BIT()s of what transition-property names (AP_EVERY for all) */
    const char *name;           /* as written, for the events' propertyName */
    int duration, delay;        /* milliseconds */
    struct css_timing ease;
    uint8_t discrete;           /* transition-behavior: allow-discrete */
};

enum { AD_NORMAL, AD_REVERSE, AD_ALTERNATE, AD_ALTERNATE_REVERSE };
enum { AF_NONE, AF_FORWARDS, AF_BACKWARDS, AF_BOTH };
struct css_animation {
    const char *name;
    int duration, delay;
    struct css_timing ease;
    float iterations;           /* negative: forever */
    uint8_t direction, fill, paused;
};

/* @keyframes name { ... }, as css.c parses it. */
struct kframe {
    float offset;               /* 0 to 1 */
    const struct css_timing *ease;      /* animation-timing-function in the keyframe, or NULL */
    uint64_t props;             /* AP_BIT()s of what its declarations set */
    const void *decls;          /* css.c's */
    int ndecls;
};
struct keyframes {
    const char *name;
    struct kframe *f;           /* by offset */
    int n;
    struct keyframes *next;
};

/* ---- CSS transitions and animations (anim.c) ---- */
/* What anim_style needs of the cascade, for the element being styled. */
struct anim_env {
    struct arena *a;            /* the cascade's, for values the style points to */
    void *ctx;
    const struct keyframes *(*find)(void *ctx, const char *name);
    void (*apply)(void *ctx, const struct kframe *f, struct style *st);  /* its declarations onto st */
    const struct style *start;  /* @starting-style's style, for an element with none before it (or NULL) */
};
/* The clock styles are computed for (ms): the browser sets it before it restyles. */
extern unsigned anim_now;
/* After the declarations of n's style st apply (css.c's compute): start the transitions its
 * changes call for and put in the values running transitions and animations have now. */
void anim_style(struct node *n, struct style *st, const struct anim_env *env);
/* A whole cascade of the tree under root is beginning, or has ended: the elements it did not
 * style (gone, or display: none) lose their animations. */
void anim_cascade_begin(struct node *root);
void anim_cascade_end(struct node *root);
/* Has n no style to transition from (it is new, or was display: none)? Then @starting-style's counts. */
int anim_needs_start(const struct node *n);
/* Is anything of n's still moving (so it must be styled again each frame)? */
int anim_running(const struct node *n);
/* Mark (dgen = gen) the elements under root whose animations move; how many. */
int anim_mark(struct node *root, unsigned gen);
/* ms until the tree under root needs another frame, or -1 if nothing moves. */
int anim_timeout(const struct node *root);
/* The document under root is going away: forget its animations. */
void anim_forget(const struct node *root);
/* Events for scripts (transitionrun, transitionend, animationstart, ...), oldest first. */
struct anim_event {
    struct node *node;
    char type[20];
    char name[64];              /* propertyName or animationName */
    float elapsed;              /* seconds */
};
int anim_take_event(const struct node *root, struct anim_event *out);
/* What transition-property's name stands for, as AP_BIT()s (all: AP_EVERY; unknown: 0). */
uint64_t anim_prop_bits(const char *name, int len);

/* The transition and animation properties as written (lists, matched up by finish_motion). */
struct css_motion_raw {
    const char *tprop, *tdur, *tdelay, *tease, *tbehav;
    const char *aname, *adur, *aease, *adelay, *aiter, *adir, *afill, *astate;
};

struct style {
    uint8_t display;
    uint8_t family, bold, italic;
    uint8_t text_align, white_space, decoration, text_transform, vertical_align;
    uint8_t list_style, hidden, box_sizing_border, flex_column, floated;
    uint8_t center_blocks;      /* text-align: -webkit-center (<center>): centers child blocks too */
    uint8_t positioned;         /* position: absolute or fixed (hidden ones are dropped) */
    uint8_t position;           /* POS_* */
    uint8_t clear;              /* clear: 1 left, 2 right, 3 both */
    uint8_t z_set;              /* z-index is not auto */
    uint8_t overlay;            /* overlay: auto, in the top layer (from the element's state; a transition keeps it) */
    const char *anchor_name;    /* anchor-name: its names, comma separated (NULL: none) */
    const char *position_anchor;    /* position-anchor: a name, "none", or NULL for auto (a popover's button) */
    const struct anchor_spec *anchor;   /* anchor(), anchor-size(), position-area, position-try (NULL: none) */
    uint8_t fit_w, fit_h;       /* width, height: fit-content (or min-/max-content): shrink to fit, never stretched */
    int sh_x, sh_y, sh_blur, sh_spread;     /* box-shadow (the first, outer one) */
    uint32_t sh_color;          /* 0: none */
    struct len tx, ty;          /* transform: translate() */
    int z_index;
    struct len top, right, bottom, left;    /* offsets of positioned boxes */
    uint8_t invisible;          /* opacity 0, off-screen or clipped away: hidden tricks */
    uint8_t opacity, alpha;     /* opacity (0-255), and what is shown of the box: that times its parents' */
    const struct css_xform *xf; /* transform: none (NULL) or functions; translate, rotate and scale first */
    struct tfop xf_translate, xf_rotate, xf_scale;  /* the individual transform properties */
    uint8_t xf_ind;             /* which of them are set: 1 translate, 2 rotate, 4 scale */
    struct len tox, toy, toz;   /* transform-origin */
    struct len perspective, pox, poy; /* perspective for children, and its origin */
    const struct css_motion_raw *raw_motion;
    const struct css_transition *trans;
    const struct css_animation *anims;
    uint8_t ntrans, nanims;
    uint8_t lh_kind;
    int lh;                     /* LH_FACTOR: times 100; LH_PX: pixels */
    int font_size;              /* pixels */
    uint32_t color, bg;
    uint32_t link_color;        /* inherited: the color <body link=...> asks for */
    /* text: spacing, wrapping, shadows and decoration styles (all inherited here) */
    int letter_spacing, word_spacing;   /* pixels */
    uint8_t word_break;                 /* WB_* */
    uint8_t small_caps, list_inside, empty_hide;
    uint8_t tab_size;                   /* inherited: tab stops every this many characters */
    uint8_t text_align_last;            /* 0: as text_align; else TA_* + 1 for the last line */
    uint8_t line_clamp;                 /* -webkit-line-clamp: lines shown (0: all) */
    const char *ls_url;                 /* list-style-image: its address */
    struct image *ls_img;               /* ...and the picture, set by the browser */
    const char *counter_reset, *counter_inc;   /* the declared values; applied by the cascade */
    uint8_t deco_style;                 /* DS_* */
    struct len deco_thickness, underline_offset;
    uint32_t deco_color;                /* 0: the text color */
    int ts_x, ts_y, ts_blur;            /* text-shadow (the first) */
    uint32_t ts_color;                  /* 0: none */
    /* not inherited: ellipsis, table layout, caption side, outline */
    uint8_t text_ellipsis, table_fixed, caption_bottom;
    uint8_t outline_style;              /* BS_* */
    int outline_w, outline_offset;
    uint32_t outline_color;             /* 1: currentColor */
    /* interaction (inherited) */
    uint8_t cursor;                     /* CUR_* */
    uint8_t pointer_none, select_none;  /* pointer-events: none; user-select: none */
    struct len margin[4], padding[4];   /* top, right, bottom, left */
    int border_w[4];
    uint8_t border_style[4];
    uint32_t border_color[4];
    struct len width, min_width, max_width, height, min_height, max_height;
    int aspect_w, aspect_h;     /* aspect-ratio, times 100 each; 0 for auto */
    int text_indent;
    int border_spacing;         /* tables, inherited */
    struct custom_prop *vars;
    const struct wfont *font;

    struct len radius[4];       /* border-radius: top left, top right, bottom right, bottom left */
    const struct css_gradient *gr;  /* a gradient over the background (NULL: none) */

    /* the background picture (the first layer that has a url()); bg_img is filled in by the browser */
    const char *bg_url;
    struct image *bg_img;
    uint8_t bg_size;                    /* BGS_*; BGS_LEN: bg_w and bg_h */
    uint8_t bg_norep_x, bg_norep_y;     /* background-repeat: no-repeat along an axis */
    uint8_t bg_origin;                  /* 0 padding-box, 1 border-box, 2 content-box */
    struct len bg_w, bg_h, bg_px, bg_py;
    uint8_t bg_clip, bg_attachment;     /* clip: border/padding/content; attachment: scroll/fixed/local */
    const char *bg_values[10];          /* declaration lists, resolved into layers after the cascade */
    struct style *bg_layers, *bg_next;  /* topmost first; each layer has its own image and geometry */

    /* object-fit and object-position, for pictures */
    uint8_t obj_fit;                    /* OF_* */
    struct len obj_px, obj_py;

    /* flex and grid containers, and their items */
    uint8_t inner;              /* IN_* */
    uint8_t flex_wrap, flex_reverse;
    uint8_t justify, align_items, align_self, justify_items, justify_self;   /* AL_* */
    uint8_t align_content;      /* AL_*: lines of a wrapping flex container, rows of a grid */
    uint8_t clips;              /* overflow other than visible */
    uint8_t ov_x, ov_y;         /* OV_*: what overflow-x and overflow-y come to */
    int flex_grow, flex_shrink; /* times 100 */
    struct len flex_basis;
    int order;
    struct len row_gap, col_gap;
    /* multi-column containers (column-count or column-width set), and breaks inside them */
    uint8_t col_count;          /* column-count; 0: auto */
    uint8_t row_gap_normal;
    uint8_t col_gap_normal;     /* column-gap: normal, which is 1em between columns (0 in flex and grid) */
    uint8_t col_fill_auto;      /* column-fill: auto: each column filled in turn, not balanced */
    uint8_t col_rule_style;     /* column-rule, BS_* */
    int col_rule_w;
    uint32_t col_rule_color;    /* 1: currentColor */
    struct len col_width;       /* column-width: U_AUTO, or the narrowest a column may be */
    uint8_t break_avoid;        /* break-inside: avoid: kept whole in one column */
    uint8_t break_before, break_after;  /* break-before/-after: column (or always): starts or ends a column */
    const struct gridspec *grid_cols, *grid_rows, *grid_auto_rows, *grid_auto_cols;
    const struct gridareas *grid_areas;     /* on the container */
    const char *grid_area;              /* on an item: grid-area: name */
    int gr_end;                         /* grid-row-end line */
    uint8_t grid_dense;                 /* grid-auto-flow: dense */
    uint8_t grid_col_flow;              /* grid-auto-flow: column */
    int gc_start, gc_end, gc_span;      /* grid-column: lines from 1 (negative: from the end), 0 auto */
    int gr_start, gr_span;              /* grid-row */
    /* container-type (CQ_*) and container-name (names, spaces apart; NULL: none) */
    uint8_t cq_type;
    const char *cq_name;
    uint8_t backface_hidden;            /* backface-visibility: hidden */
    const struct css_filter *filter, *backdrop;     /* filter, backdrop-filter (NULL: none) */
    const struct css_clip *clip_path;
    uint8_t blend;                      /* mix-blend-mode, BL_* */
};
enum { CQ_NONE, CQ_INLINE, CQ_SIZE };
/* layout.c: a container came out a size the cascade did not assume; the page needs styling again */
extern int layout_cq_changed;

struct css_fontface { char family[128], sources[8][URL_MAX]; int nsources, bold, italic; };
void css_font_faces(const char *text,int len,int vw,int vh,void (*found)(void *,const struct css_fontface *),void *ctx);
extern int (*css_font_lookup)(void *ctx,const char *family);
extern void *css_font_context;
struct sheet;
struct sheet *css_parse(struct arena *a, const char *text, int len, int viewport_w);
/* Compute the style of every element under root, from the UA style sheet,
 * HTML attributes, the author sheets (in order) and style attributes. */
void css_cascade(struct arena *a, struct node *root, struct sheet **sheets, int nsheets,
                 int viewport_w, int viewport_h, int (*visited)(const char *href, void *ctx),
                 void *ctx);
/* After only :hover, :focus, :active or :focus-visible moved: restyle the elements whose
 * state changed and the ones that depend on them. Returns 1 if any style changed, 0 if
 * none did, -1 if the page needs css_cascade() instead. */
int css_restyle_dynamic(struct arena *a, struct node *root, struct sheet **sheets, int nsheets,
                        int viewport_w, int viewport_h, int (*visited)(const char *href, void *ctx),
                        void *ctx);
/* A frame of the page's transitions and animations: restyle the elements that move (and what
 * inherits from them), at anim_now. Returns as css_restyle_dynamic does. */
int css_restyle_animated(struct arena *a, struct node *root, struct sheet **sheets, int nsheets,
                         int viewport_w, int viewport_h, int (*visited)(const char *href, void *ctx),
                         void *ctx);
extern int css_shared;
int css_parse_color(const char *s, int n, uint32_t *out);
/* oklch(), lab(), hwb(), color(), color-mix() and the rest (csscolor.c, in floating point). */
int css_color_function(const char *s, uint32_t *out);
/* An @supports condition ("(display: grid) and (not (color: oklch(0 0 0)))"), or a declaration. */
int css_supports(const char *cond);
/* Does a media query list ("screen and (min-width: 600px)") match a vw x vh viewport? */
int css_media_query(const char *query, int vw, int vh);
/* The URLs a sheet's @import rules name (if their media match): found() is called for each. */
int css_imports(const char *text, int len, int vw, void (*found)(void *ctx, const char *url), void *ctx);
/* The viewport height media queries in css_parse see (the width is css_parse's argument). */
extern int css_viewport_h;
/* Page zoom, in percent: absolute lengths, the font size and pictures are this much larger (100: as written). */
extern int css_zoom;
#define ZOOM(n) ((n) * css_zoom / 100)

/* ---- :hover and :focus (css.c): the elements they match; css_dynamic_rules counts the rules that use them ---- */
extern const struct node *css_hover_node, *css_focus_node, *css_active_node, *css_focus_visible_node;
extern int css_dynamic_rules;

/* ---- cookies (cookies.c) ---- */
void cookie_set(const char *url, const char *set_cookie_value, int from_script);
void cookie_set_from_headers(const char *url, const char *headers, int hlen);
void cookie_flush(void);                /* write the cookies that outlast the desktop to disk, if they changed */
/* The Cookie header's value for a request to url; scripts do not see HttpOnly cookies. */
void cookie_header(const char *url, int scripts_only, char *out, int size);
void cookie_header_context(const char *url, const char *site, int top_level, int safe_method, char *out, int size);

/* ---- SVG (svg.c) ---- */
/* The size an <svg> element asks for (width, height, viewBox; else 300x150). */
void svg_size(const struct node *svg, int *w, int *h);
/* Draw it w x h; color is currentColor. NULL if it cannot be. */
struct image *svg_render(const struct node *svg, int w, int h, uint32_t color);

/* ---- layout and painting (layout.c) ---- */
/* DK_FRAME: a picture that could not be shown; DK_IFRAME: where an <iframe>'s page goes (browser.c);
 * DK_BGIMG: a background picture (bgp); DK_SBOX: the scroll bars of a box that scrolls (sbi), drawn last */
enum dkind { DK_FILL, DK_TEXT, DK_IMAGE, DK_DISC, DK_CIRCLE, DK_CONTROL, DK_FRAME, DK_IFRAME, DK_BGIMG,
             DK_SBOX };

/* A background picture, placed: the item is the area it is painted in, and these are measured from its corner. */
struct bgpaint {
    struct bgpaint *next;       /* paint bottommost first */
    struct image *img;
    int tx, ty, tw, th;         /* the first tile: where it is and how big (tw, th > 0) */
    uint8_t norep_x, norep_y;   /* one tile only along the axis */
    uint8_t fixed;
    const struct css_gradient *gr;  /* a gradient tile instead of a picture */
    struct rect clip;
};

struct ditem {
    struct rect bgclip;
    uint8_t bg_clipped;
    uint8_t kind;
    uint8_t deco;               /* text: DECO_* */
    int x, y, w, h;             /* in page coordinates */
    uint32_t color;
    const struct wfont *font;   /* text */
    const char *text;
    int len;
    struct image *img;          /* image */
    struct node *node;          /* control */
    /* text: letter spacing, a shadow and the decoration's color and style */
    int8_t ls;
    int8_t sh_x, sh_y, sh_blur;
    uint32_t sh_color;          /* 0: no shadow */
    uint32_t deco_color;        /* 0: the text color */
    uint8_t deco_style;
    int deco_thickness, underline_offset; /* -1: automatic underline offset */
    uint8_t dyn;                /* 1 + the index of the fixed or sticky box it moves with, or 0 */
    /* fills and images: rounded corners; fills: a gradient over the color,
       and a border of ring pixels all round in ring_color */
    int rad[4];
    const struct css_gradient *gr;
    int ring;
    uint32_t ring_color;
    const struct bgpaint *bgp;  /* DK_BGIMG */
    /* images: object-fit puts the picture at fit_x, fit_y (fit_w x fit_h) in the item and clips it to the item */
    int fit_x, fit_y, fit_w, fit_h;     /* fit_w == 0: the picture fills the item */
    uint16_t sb;                /* 1 + the scrolling box it is inside (and clipped by), or 0 */
    uint16_t sbi;               /* DK_SBOX: its box */
    uint16_t xf;                /* transform group, 1-based */
    uint8_t fade;               /* opacity: 255 minus how much shows (0: opaque) */
    uint16_t fx;                /* effect group (filter, clip-path, blend, backdrop-filter), 1-based */
    uint8_t backdrop;           /* the first item of a group with backdrop-filter: what is behind it is filtered first */
};

struct hotspot {                /* a link or a form control */
    int x, y, w, h;
    int at;                     /* where it is in the paint order: after this many items */
    struct node *node;          /* the <a href> or the control */
    uint8_t dyn;                /* like ditem.dyn */
    uint16_t sb;                /* like ditem.sb */
    uint16_t xf;
};

/* A box with overflow other than visible whose contents are clipped to its padding box; with
 * overflow: scroll or auto (or a script) they can be scrolled, and the box gets scroll bars. */
struct sbox {
    int x, y, w, h;             /* the padding box, as laid out (a box in a fixed one moves with dyn) */
    int cw, ch;                 /* what can be scrolled over: the contents' size from x, y */
    int sx, sy;                 /* how far it is scrolled */
    int max_x, max_y;           /* the furthest it can be */
    uint8_t ov_x, ov_y;         /* OV_* */
    uint8_t bar_x, bar_y;       /* the scroll bars it shows */
    uint8_t active;             /* anything to clip or scroll */
    uint8_t dyn;                /* like ditem.dyn */
    uint16_t parent;            /* 1 + the box it is inside, or 0 */
    struct node *node;
};
#define MAX_SBOX 1500
#define SB_BAR 16               /* a scroll bar's thickness */

/* A box that moves as the page scrolls: position: fixed stays where it is on the screen,
 * position: sticky stays on the screen between its place and the end of its container. */
struct dynbox {
    uint8_t fixed;              /* else sticky */
    uint8_t parent;             /* 1 + the dynbox it is inside, or 0 */
    int x, y, w, h;             /* where layout put it (page coordinates; fixed: window coordinates) */
    int top, right, bottom, left;       /* sticky offsets in pixels; DYN_NONE for auto */
    int cy0, cy1, cx0, cx1;     /* sticky: the containing block */
    struct node *container;     /* sticky: the box whose content area it stays in */
    int i0, i1;                 /* its items while the page is being laid out (layout moves them) */
};
#define DYN_NONE (-0x40000000)
#define MAX_DYNBOX 200

struct anchor {
    const char *name;           /* an id, or the name of <a name=...> */
    int y;
};

struct transform_box {
    struct node *node;
    int x, y, w, h, first, last;
    uint16_t parent, sb;
    uint8_t dyn;
};

/* A box with filter, clip-path, mix-blend-mode or backdrop-filter: its items paint through it
 * (and through the groups round it, parent on). */
struct effect_box {
    struct node *node;
    int x, y, w, h, first, last;        /* its border box; its items when it was laid out */
    int rad[4];                         /* its corners, for backdrop-filter */
    uint16_t parent;
    uint8_t offscreen;                  /* filter, clip-path or blend: items are painted apart, then put down */
};

struct layout {
    struct effect_box *fx;
    int nfx, fxcap;
    struct ditem *items;
    int nitems, cap;
    struct hotspot *links;
    int nlinks, linkcap;
    struct hotspot *controls;
    int ncontrols, controlcap;
    struct anchor *anchors;
    int nanchors, anchorcap;
    int width, height;          /* the document's extent */
    int vw, vh;                 /* the window it was laid out for: where fixed and sticky boxes go */
    struct dynbox *dyn;
    int ndyn;
    struct sbox *sb;
    int nsb;
    struct transform_box *xf;
    int nxf, xfcap;
    uint32_t canvas;            /* the background behind everything */
    /* the selected text: items sel_from..sel_to, from character sel_c0 of the first to sel_c1 of the last */
    int sel_on, sel_from, sel_to, sel_c0, sel_c1;
    /* text the find bar found, painted behind the text: hl_cur is the match that is the current one */
    const struct fhl *hl;
    int nhl, hl_cur;
};

/* Select the text between two points (page coordinates), in reading order. */
void layout_select(struct layout *lo, int ax, int ay, int fx, int fy);
void layout_select_all(struct layout *lo);
/* The selected text, with line breaks; returns its length (0 if nothing is selected). */
int layout_selection_text(const struct layout *lo, char *out, int size);

/* Lay out the document for a viewport `vw` pixels wide. Output goes in lo,
 * allocated from a (free it to throw the layout away). */
void layout_document(struct arena *a, struct layout *lo, struct node *root, int vw, int vh);
/* Paint the part of the document at (sx, sy) into s at (ox, oy), within s's clip. */
void paint_document(const struct layout *lo, struct surface *s, int ox, int oy, int sx, int sy,
                    const struct node *focus, int caret_on);
void layout_paint_surface(const struct layout *lo, const struct ditem *item, const struct surface *source,
                          struct surface *dst, int ox, int oy, int sx, int sy);
/* The hotspot at a page point, with the page scrolled to (sx, sy): fixed and sticky ones move with it. */
const struct hotspot *layout_hit(const struct layout *lo, const struct hotspot *h, int n, int x, int y,
                                 int sx, int sy);
/* How far scrolling to (sx, sy) moves the item or hotspot with this dyn from where it was laid out. */
void layout_dyn_offset(const struct layout *lo, int dyn, int sx, int sy, int *dx, int *dy);
struct rect layout_transform_rect(const struct layout *lo, int xf, struct rect r, int sx, int sy);
struct rect layout_node_visual_rect(const struct layout *lo, const struct node *n, struct rect r, int sx, int sy);
int layout_node_local_point(const struct layout *lo, const struct node *n, struct rect r, int *x, int *y, int sx, int sy);
int layout_transform_point(const struct layout *lo, int xf, int *x, int *y, int sx, int sy, int inverse);
int layout_box_contains(const struct layout *lo, int xf, int dyn, int sb, struct rect r,
                        int x, int y, int sx, int sy);
/* ---- boxes that scroll (overflow: auto, scroll) ---- */
enum { SBP_NONE, SBP_ARROW1, SBP_ARROW2, SBP_PAGE1, SBP_PAGE2, SBP_THUMB };
struct sbhit {                  /* a point on a scroll bar */
    int box, axis, part;        /* index into lo->sb, 0 horizontal 1 vertical, SBP_* */
    int thumb;                  /* where its thumb starts along the axis, in page coordinates */
    int coordinate;             /* pointer coordinate before transforms */
};
/* The scroll bar at a page point (the page scrolled to psx, psy); 1 if there is one. */
int layout_sbar_hit(const struct layout *lo, int px, int py, int psx, int psy, struct sbhit *out);
/* A press on an arrow or the track: scroll a step or a page; 1 if the box moved. */
int layout_sbar_press(struct layout *lo, const struct sbhit *h);
/* The thumb grabbed `grab` pixels from its start and dragged to mouse (page coordinates along the axis). */
void layout_sbar_drag(struct layout *lo, const struct sbhit *h, int px, int py, int grab, int psx, int psy);
/* The wheel turned over a page point: the innermost box that can scroll that way takes it; 1 if one did. */
int layout_wheel(struct layout *lo, int px, int py, int psx, int psy, int dx, int dy);
/* A box's scroll position: get (0 if n is not a box that scrolls) and set (clamped, kept when the page is laid out again). */
int layout_node_scroll(const struct layout *lo, const struct node *n, int *sx, int *sy, int *cw, int *ch,
                       int *vw, int *vh);
int layout_set_node_scroll(struct layout *lo, struct node *n, int sx, int sy);
/* Scroll the boxes that clip n so that its rectangle (page coordinates) shows. */
void layout_reveal(struct layout *lo, const struct node *n, int x, int y, int w, int h);
/* How the scrolling boxes round an item (its sb) move it, and the page rectangle that clips it. */
void layout_sb_view(const struct layout *lo, int sb, int psx, int psy, int *dx, int *dy, struct rect *clip);

/* <input type=range>: where its value is on the track (0 to 1000), the value a click at such a place
 * gives, and the value one step on (dir -1 or 1; 0 for the first or last: -2 and 2). */
int range_fraction(const struct node *n);
void range_value_at(const struct node *n, int fraction, char *out, int size);
void range_nudge(const struct node *n, int dir, char *out, int size);
/* Characters a date or time field takes, and the pattern it is waiting for ("yyyy-mm-dd"); NULL for other fields. */
const char *input_pattern(const char *type);
/* <audio controls>: where its seek bar runs, from x0 to x1 pixels in a control w wide (x0 == x1: none). */
void layout_media_bar(const struct node *n, int w, int *x0, int *x1);
/* The text a button, submit input or select shows. */
const char *control_label(const struct node *n);
/* Draw img scaled to w x h, alpha-blended. */
void draw_image_scaled(struct surface *s, const struct image *img, int x, int y, int w, int h);

/* ---- files the browser keeps (webstore.c) ---- */
char *store_read_file(const char *path, long max, long *len);   /* malloc'd; NULL if missing or too big */
int store_write_file(const char *path, const char *data, size_t len);
/* IndexedDB's databases: one file each, named by origin and name, under IDB_DIR. op is "load" (the
 * text, malloc'd, or NULL), "save" (text), "delete"; "list" gives the names, one a line (malloc'd). */
char *store_idb(const char *op, const char *origin, const char *name, const char *text, size_t len);
long store_file_mtime(const char *path);                         /* seconds since 1970; 0 if unknown */

/* ---- URLs (webutil.c, like the arena and UTF-8 helpers) ---- */
/* Resolve ref against base (an absolute URL). Fragments are kept. */
void url_resolve(const char *base, const char *ref, char *out, int size);
int url_is_http(const char *url);
int url_is_https(const char *url);
int url_is_file(const char *url);
int url_file_path(const char *url, char *out, size_t size); /* decoded, no query/fragment; 0 if invalid */
int web_archive_path(const char *path);                    /* Archive Manager's ZIP/7z formats */
int web_document_download(const char *type, const void *body, size_t len);
void url_strip_fragment(char *url);
const char *url_fragment(const char *url);         /* after '#', or NULL */
/* Append s to out, form-encoded (application/x-www-form-urlencoded). */
void url_encode_form(char *out, int size, const char *s);

#endif
