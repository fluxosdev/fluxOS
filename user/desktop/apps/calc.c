/*
 * The calculator: a Windows 95 style standard calculator with memory keys,
 * percent, square root, reciprocal and a keyboard interface.
 *
 * Numbers are exact decimals: a sign, a string of digits (a "mag") and the
 * number of those digits that follow the decimal point. The desktop is
 * built without the FPU (only its GL code may use it), so everything is
 * done on digits: addition, subtraction and multiplication on the digit
 * strings, division by long division, and the square root digit by digit.
 * Calculations keep 30 significant digits and the display rounds them
 * (half up) to 24, so 1/3 shows
 * 0.333333333333333333333333, 1/3*3 shows 1 and 0.1 + 0.2 is exactly 0.3.
 *
 * Keys: digits and '.', + - * / and Enter or =, % (percent), @ (square
 * root), r (reciprocal), n (change sign), Backspace, Delete (CE), Esc or c
 * (clear). The memory keys are on screen only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop.h"

#define CALC_W   254
#define CALC_H   218
#define MAXM     192            /* digits in a magnitude */
#define MAXSIG   30             /* significant digits kept in calculations */
#define DISPSIG  24             /* and shown, so 1/3*3 shows 1 */
#define MAXSC    40             /* digits after the point kept */
#define MAXENTRY 20             /* digits that can be typed */

/* ---- digit strings: little endian, d[0] is the units digit ---- */

struct mag {
    int n;                      /* digits in use; 0 is zero */
    uint8_t d[MAXM];
};

static int overflow;            /* set when something outgrew a mag */

static void mag_trim(struct mag *m)
{
    while (m->n > 0 && m->d[m->n - 1] == 0)
        m->n--;
}

static int mag_cmp(const struct mag *a, const struct mag *b)
{
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

static void mag_add(const struct mag *a, const struct mag *b, struct mag *r)
{
    int n = a->n > b->n ? a->n : b->n, carry = 0;
    struct mag out;
    out.n = 0;
    for (int i = 0; i < n || carry; i++) {
        if (i >= MAXM) {
            overflow = 1;
            break;
        }
        int v = (i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0) + carry;
        out.d[i] = (uint8_t)(v % 10);
        carry = v / 10;
        out.n = i + 1;
    }
    *r = out;
}

/* r = a - b, for a >= b. */
static void mag_sub(const struct mag *a, const struct mag *b, struct mag *r)
{
    struct mag out = { a->n, { 0 } };
    int borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int v = a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        borrow = v < 0;
        out.d[i] = (uint8_t)(v < 0 ? v + 10 : v);
    }
    mag_trim(&out);
    *r = out;
}

static void mag_mul_small(struct mag *m, int k)     /* k below 1000 */
{
    int carry = 0;
    for (int i = 0; i < m->n || carry; i++) {
        if (i >= MAXM) {
            overflow = 1;
            return;
        }
        int v = (i < m->n ? m->d[i] : 0) * k + carry;
        m->d[i] = (uint8_t)(v % 10);
        carry = v / 10;
        if (i >= m->n)
            m->n = i + 1;
    }
    mag_trim(m);
}

static void mag_add_small(struct mag *m, int k)     /* k below 10 */
{
    struct mag t = { 0, { 0 } };
    if (k) {
        t.n = 1;
        t.d[0] = (uint8_t)k;
    }
    mag_add(m, &t, m);
}

static void mag_mul(const struct mag *a, const struct mag *b, struct mag *r)
{
    struct mag out = { 0, { 0 } };
    if (!a->n || !b->n) {
        *r = out;
        return;
    }
    if (a->n + b->n > MAXM) {
        overflow = 1;
        *r = out;
        return;
    }
    int acc[MAXM] = { 0 };
    for (int i = 0; i < a->n; i++)
        for (int j = 0; j < b->n; j++)
            acc[i + j] += a->d[i] * b->d[j];
    int carry = 0;
    for (int i = 0; i < a->n + b->n; i++) {
        int v = acc[i] + carry;
        out.d[i] = (uint8_t)(v % 10);
        carry = v / 10;
    }
    out.n = a->n + b->n;
    mag_trim(&out);
    *r = out;
}

static void mag_shl(struct mag *m, int k)           /* times 10^k */
{
    if (!m->n || !k)
        return;
    if (m->n + k > MAXM) {
        overflow = 1;
        return;
    }
    memmove(m->d + k, m->d, (size_t)m->n);
    memset(m->d, 0, (size_t)k);
    m->n += k;
}

static void mag_shr(struct mag *m, int k)           /* drop k low digits */
{
    if (k >= m->n) {
        m->n = 0;
        return;
    }
    memmove(m->d, m->d + k, (size_t)(m->n - k));
    m->n -= k;
}

/* ---- decimal numbers ---- */

struct num {
    int neg;
    struct mag m;
    int sc;                     /* digits after the decimal point */
};

static void num_zero(struct num *x)
{
    memset(x, 0, sizeof(*x));
}

static void num_int(struct num *x, int v)
{
    num_zero(x);
    x->neg = v < 0;
    for (unsigned u = (unsigned)(v < 0 ? -v : v); u; u /= 10)
        x->m.d[x->m.n++] = (uint8_t)(u % 10);
}

/* Drop what cannot be kept: trailing zeros, then digits beyond `sig`. */
static void num_round(struct num *x, int sig)
{
    for (;;) {
        mag_trim(&x->m);
        if (x->m.n == 0) {
            x->neg = 0;
            x->sc = 0;
            return;
        }
        if (x->sc > 0 && x->m.d[0] == 0) {
            mag_shr(&x->m, 1);
            x->sc--;
            continue;
        }
        if (x->sc > 0 && (x->m.n > sig || x->sc > MAXSC)) {
            int up = x->m.d[0] >= 5;
            mag_shr(&x->m, 1);
            x->sc--;
            if (up)
                mag_add_small(&x->m, 1);
            continue;
        }
        break;
    }
    if (x->m.n - x->sc > MAXSIG)
        overflow = 1;
}

static void num_fit(struct num *x)
{
    num_round(x, MAXSIG);
}

static void num_neg(struct num *x)
{
    if (x->m.n)
        x->neg = !x->neg;
}

static void num_add(const struct num *a, const struct num *b, struct num *r)
{
    struct mag ma = a->m, mb = b->m;
    int sc = a->sc > b->sc ? a->sc : b->sc;
    mag_shl(&ma, sc - a->sc);
    mag_shl(&mb, sc - b->sc);
    struct num out;
    num_zero(&out);
    out.sc = sc;
    if (a->neg == b->neg) {
        mag_add(&ma, &mb, &out.m);
        out.neg = a->neg;
    } else if (mag_cmp(&ma, &mb) >= 0) {
        mag_sub(&ma, &mb, &out.m);
        out.neg = a->neg;
    } else {
        mag_sub(&mb, &ma, &out.m);
        out.neg = b->neg;
    }
    num_fit(&out);
    *r = out;
}

static void num_sub(const struct num *a, const struct num *b, struct num *r)
{
    struct num nb = *b;
    num_neg(&nb);
    num_add(a, &nb, r);
}

static void num_mul(const struct num *a, const struct num *b, struct num *r)
{
    struct num out;
    num_zero(&out);
    mag_mul(&a->m, &b->m, &out.m);
    out.sc = a->sc + b->sc;
    out.neg = a->neg != b->neg;
    num_fit(&out);
    *r = out;
}

/* r = a / b; -1 if b is zero. */
static int num_div(const struct num *a, const struct num *b, struct num *r)
{
    if (!b->m.n)
        return -1;
    struct num out;
    num_zero(&out);
    if (a->m.n) {
        int k = (b->sc > a->sc ? b->sc - a->sc : 0) + b->m.n + 40;
        struct mag dividend = a->m, rem = { 0, { 0 } };
        mag_shl(&dividend, k);
        if (!overflow) {
            for (int i = dividend.n - 1; i >= 0; i--) {
                mag_mul_small(&rem, 10);
                mag_add_small(&rem, dividend.d[i]);
                int q = 0;
                while (mag_cmp(&rem, &b->m) >= 0) {
                    mag_sub(&rem, &b->m, &rem);
                    q++;
                }
                out.m.d[i] = (uint8_t)q;
            }
            out.m.n = dividend.n;
        }
        out.sc = a->sc - b->sc + k;
        out.neg = a->neg != b->neg;
    }
    num_fit(&out);
    *r = out;
    return 0;
}

/* r = the square root of a, for a >= 0. */
static void num_sqrt(const struct num *a, struct num *r)
{
    struct num out;
    num_zero(&out);
    if (a->m.n) {
        struct mag m = a->m;
        int sc = a->sc;
        if (sc & 1) {                   /* an even number of decimals, so pairs line up */
            mag_mul_small(&m, 10);
            sc++;
        }
        mag_shl(&m, 60);                /* 30 more digits in the root */
        sc += 60;
        struct mag rem = { 0, { 0 } }, root = { 0, { 0 } };
        for (int i = (m.n + 1) / 2 * 2 - 2; i >= 0; i -= 2) {
            mag_mul_small(&rem, 100);
            mag_add_small(&rem, (i + 1 < m.n ? m.d[i + 1] : 0) * 10 + m.d[i]);
            int d;
            struct mag t;
            for (d = 9; d > 0; d--) {
                t = root;
                mag_mul_small(&t, 20);
                mag_add_small(&t, d);
                mag_mul_small(&t, d);
                if (mag_cmp(&t, &rem) <= 0)
                    break;
            }
            if (d > 0) {
                t = root;
                mag_mul_small(&t, 20);
                mag_add_small(&t, d);
                mag_mul_small(&t, d);
                mag_sub(&rem, &t, &rem);
            }
            mag_mul_small(&root, 10);
            mag_add_small(&root, d);
        }
        out.m = root;
        out.sc = sc / 2;
    }
    num_fit(&out);
    *r = out;
}

/* "-123.45" for display; returns the length. */
static int num_text(const struct num *x, char *out, int size)
{
    char digits[MAXM + 4];
    struct num r = *x;
    int saved = overflow;
    num_round(&r, DISPSIG);
    overflow = saved;
    x = &r;
    int n = x->m.n, o = 0;
    if (n == 0) {
        snprintf(out, (size_t)size, "0");
        return 1;
    }
    int total = n > x->sc ? n : x->sc + 1;      /* at least one digit before the point */
    for (int i = total - 1; i >= 0; i--) {
        digits[o++] = i < n ? (char)('0' + x->m.d[i]) : '0';
        if (i == x->sc && x->sc > 0)
            digits[o++] = '.';
    }
    digits[o] = 0;
    return snprintf(out, (size_t)size, "%s%s", x->neg ? "-" : "", digits);
}

/* ---- the calculator ---- */

enum { K_MC = 1, K_MR, K_MS, K_MPLUS };

struct key {
    const char *label;
    int code;                   /* '0'-'9', '.', one of the operator letters, or K_* */
    int x, y, w, h;
    int red;                    /* drawn in red rather than blue */
};

#define GX 50                   /* the key grid's left edge */
#define GY 78
#define PITCH_X 40
#define PITCH_Y 34

static struct key keys[32];
static int nkeys;

static void add_key(const char *label, int code, int x, int y, int w, int h, int red)
{
    keys[nkeys++] = (struct key){ label, code, x, y, w, h, red };
}

static void build_keys(void)
{
    if (nkeys)
        return;
    static const char *const mem[] = { "MC", "MR", "MS", "M+" };
    for (int i = 0; i < 4; i++)
        add_key(mem[i], K_MC + i, 8, GY + i * PITCH_Y, 34, 30, 1);
    add_key("Back", 'b', GX, 46, 62, 24, 1);
    add_key("CE", 'e', GX + 67, 46, 62, 24, 1);
    add_key("C", 'c', GX + 134, 46, 62, 24, 1);
    static const struct { const char *label; int code, red; } grid[4][5] = {
        { { "7", '7', 0 }, { "8", '8', 0 }, { "9", '9', 0 }, { "/", '/', 1 }, { "sqrt", 's', 0 } },
        { { "4", '4', 0 }, { "5", '5', 0 }, { "6", '6', 0 }, { "*", '*', 1 }, { "%", '%', 0 } },
        { { "1", '1', 0 }, { "2", '2', 0 }, { "3", '3', 0 }, { "-", '-', 1 }, { "1/x", 'r', 0 } },
        { { "0", '0', 0 }, { "+/-", 'n', 0 }, { ".", '.', 0 }, { "+", '+', 1 }, { "=", '=', 1 } },
    };
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 5; c++)
            add_key(grid[r][c].label, grid[r][c].code, GX + c * PITCH_X, GY + r * PITCH_Y, 36, 30,
                    grid[r][c].red);
}

struct calc {
    struct window *win;
    struct num acc, cur, mem, operand;
    int op, last_op;            /* '+', '-', '*', '/' or 0 */
    int entering;               /* cur is being typed, in entry */
    int fresh;                  /* an operator or = was just pressed */
    char entry[MAXENTRY + 4];   /* digits and at most one '.' */
    int elen, epoint, eneg;
    int has_mem;
    const char *error;
    int pressed;                /* key held down by the mouse, or -1 */
    int over;                   /* ... and the pointer is on it */
};

static void clear_entry(struct calc *c)
{
    c->elen = c->epoint = c->eneg = 0;
    c->entry[0] = 0;
}

static void entry_to_cur(struct calc *c)
{
    num_zero(&c->cur);
    int dec = 0, seen = 0;
    for (int i = 0; i < c->elen; i++) {
        if (c->entry[i] == '.') {
            seen = 1;
            continue;
        }
        mag_mul_small(&c->cur.m, 10);
        mag_add_small(&c->cur.m, c->entry[i] - '0');
        dec += seen;
    }
    c->cur.sc = dec;
    c->cur.neg = c->eneg && c->cur.m.n;
    num_fit(&c->cur);
}

static void fail(struct calc *c, const char *why)
{
    c->error = why;
    c->entering = 0;
    overflow = 0;
}

static void apply_op(struct calc *c, int op, const struct num *a, const struct num *b)
{
    struct num r;
    overflow = 0;
    switch (op) {
    case '+': num_add(a, b, &r); break;
    case '-': num_sub(a, b, &r); break;
    case '*': num_mul(a, b, &r); break;
    default:
        if (num_div(a, b, &r) < 0) {
            fail(c, a->m.n ? "Cannot divide by zero" : "Result is undefined");
            return;
        }
        break;
    }
    if (overflow) {
        fail(c, "Overflow");
        return;
    }
    c->acc = c->cur = r;
}

static void clear_all(struct calc *c)
{
    num_zero(&c->acc);
    num_zero(&c->cur);
    num_zero(&c->operand);
    c->op = c->last_op = 0;
    c->entering = c->fresh = 0;
    c->error = NULL;
    overflow = 0;
    clear_entry(c);
}

static void press_key(struct calc *c, int code)
{
    if (code == 'c') {
        clear_all(c);
        return;
    }
    if (code == K_MC) {
        c->has_mem = 0;
        num_zero(&c->mem);
        return;
    }
    if (c->error) {
        if (code == 'e')
            clear_all(c);
        return;                         /* only C and CE get out of an error */
    }
    struct num t;
    overflow = 0;
    switch (code) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        if (!c->entering) {
            clear_entry(c);
            c->entering = 1;
            c->fresh = 0;
        }
        int digits = c->elen - c->epoint;
        if (digits >= MAXENTRY)
            break;
        if (c->elen == 1 && c->entry[0] == '0' && !c->epoint)
            c->elen = 0;                /* no leading zeros */
        c->entry[c->elen++] = (char)code;
        c->entry[c->elen] = 0;
        entry_to_cur(c);
        break;
    case '.':
        if (!c->entering) {
            clear_entry(c);
            c->entering = 1;
            c->fresh = 0;
        }
        if (!c->epoint && c->elen < MAXENTRY + 1) {
            if (!c->elen)
                c->entry[c->elen++] = '0';
            c->entry[c->elen++] = '.';
            c->entry[c->elen] = 0;
            c->epoint = 1;
        }
        entry_to_cur(c);
        break;
    case 'b':                           /* backspace */
        if (!c->entering)
            break;
        if (c->elen > 0) {
            if (c->entry[c->elen - 1] == '.')
                c->epoint = 0;
            c->entry[--c->elen] = 0;
        }
        entry_to_cur(c);
        break;
    case 'e':                           /* CE */
        clear_entry(c);
        num_zero(&c->cur);
        c->entering = 1;
        break;
    case 'n':
        if (c->entering) {
            c->eneg = !c->eneg;
            entry_to_cur(c);
        } else {
            num_neg(&c->cur);
        }
        break;
    case '+': case '-': case '*': case '/':
        if (c->op && !c->fresh) {
            apply_op(c, c->op, &c->acc, &c->cur);
            if (c->error)
                break;
        } else if (!c->op) {
            c->acc = c->cur;
        }
        c->op = code;
        c->entering = 0;
        c->fresh = 1;
        break;
    case '=':
        if (c->op) {
            c->operand = c->cur;
            c->last_op = c->op;
            apply_op(c, c->op, &c->acc, &c->operand);
        } else if (c->last_op) {
            c->acc = c->cur;
            apply_op(c, c->last_op, &c->acc, &c->operand);
        }
        c->op = 0;
        c->entering = 0;
        c->fresh = 1;
        break;
    case '%':
        /* 50 + 10 % is 50 + 5; 50 * 10 % is 50 * 0.1 */
        num_int(&t, 100);
        if (c->op == '+' || c->op == '-')
            num_mul(&c->acc, &c->cur, &c->cur);
        num_div(&c->cur, &t, &c->cur);
        c->entering = 0;
        c->fresh = 0;
        if (overflow)
            fail(c, "Overflow");
        break;
    case 's':
        if (c->cur.neg) {
            fail(c, "Invalid input");
            break;
        }
        num_sqrt(&c->cur, &c->cur);
        c->entering = 0;
        c->fresh = 0;
        if (overflow)
            fail(c, "Overflow");
        break;
    case 'r':
        num_int(&t, 1);
        if (num_div(&t, &c->cur, &c->cur) < 0)
            fail(c, "Cannot divide by zero");
        else if (overflow)
            fail(c, "Overflow");
        c->entering = 0;
        c->fresh = 0;
        break;
    case K_MR:
        if (c->has_mem) {
            c->cur = c->mem;
            c->entering = 0;
            c->fresh = 0;
        }
        break;
    case K_MS:
        c->mem = c->cur;
        c->has_mem = 1;
        c->entering = 0;
        break;
    case K_MPLUS:
        num_add(&c->mem, &c->cur, &c->mem);
        c->has_mem = 1;
        c->entering = 0;
        if (overflow)
            fail(c, "Overflow");
        break;
    }
    overflow = 0;
}

/* ---- drawing ---- */

static void paint(struct calc *c)
{
    struct surface *s = &c->win->content;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);

    struct rect disp = mkrect(8, 8, s->w - 16, 30);
    gfx_fill(s, disp.x, disp.y, disp.w, disp.h, C_WHITE);
    gfx_bevel(s, disp, BEVEL_SUNKEN);

    char text[64];
    if (c->error)
        snprintf(text, sizeof(text), "%s", c->error);
    else if (c->entering)
        snprintf(text, sizeof(text), "%s%s", c->eneg ? "-" : "", c->elen ? c->entry : "0");
    else
        num_text(&c->cur, text, sizeof(text));
    int avail = disp.w - 12;
    const struct pfont *f = &font_banner;
    if (gfx_text_width(f, text) > avail)
        f = &font_sans_bold;
    if (gfx_text_width(f, text) > avail)
        f = &font_sans;
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, mkrect(disp.x + 2, disp.y + 2, disp.w - 4, disp.h - 4));
    int tw = gfx_text_width(f, text);
    gfx_text(s, f, disp.x + disp.w - 6 - tw, disp.y + (disp.h - f->height) / 2, text, C_BLACK);
    s->clip = saved;
    if (c->has_mem)
        gfx_text(s, &font_sans_bold, disp.x + 6, disp.y + 4, "M", C_BLACK);

    for (int i = 0; i < nkeys; i++) {
        const struct key *k = &keys[i];
        struct rect r = mkrect(k->x, k->y, k->w, k->h);
        int down = c->pressed == i && c->over;
        gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
        gfx_bevel(s, r, down ? BEVEL_PRESSED : BEVEL_RAISED);
        color_t col = k->red ? RGB(0xB0, 0x00, 0x00) : RGB(0x00, 0x00, 0xB0);
        int tx = r.x + (r.w - gfx_text_width(&font_sans_bold, k->label)) / 2 + down;
        gfx_text(s, &font_sans_bold, tx, r.y + (r.h - font_sans_bold.height) / 2 + down, k->label, col);
    }
    wm_damage_client(c->win, mkrect(0, 0, s->w, s->h));
}

/* ---- input ---- */

static int key_at(int x, int y)
{
    for (int i = 0; i < nkeys; i++)
        if (rect_contains(mkrect(keys[i].x, keys[i].y, keys[i].w, keys[i].h), x, y))
            return i;
    return -1;
}

static void calc_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct calc *c = w->priv;
    int left = buttons & MOUSE_LEFT;
    if ((changed & MOUSE_LEFT) && left) {
        c->pressed = key_at(x, y);
        c->over = c->pressed >= 0;
        paint(c);
    } else if ((changed & MOUSE_LEFT) && !left) {
        int k = c->pressed, hit = c->over;
        c->pressed = -1;
        if (k >= 0 && hit)
            press_key(c, keys[k].code);
        paint(c);
    } else if (left && c->pressed >= 0) {
        int over = key_at(x, y) == c->pressed;
        if (over != c->over) {
            c->over = over;
            paint(c);
        }
    }
}

static void calc_key(struct window *w, const struct input_event *ev)
{
    struct calc *c = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    int code = 0;
    if (ev->code == KEY_ESC)
        code = 'c';
    else if (ev->code == KEY_DELETE)
        code = 'e';
    else if (ev->len == 1) {
        char ch = ev->text[0];
        if ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
            ch == '=' || ch == '%')
            code = ch;
        else if (ch == '.' || ch == ',')
            code = '.';
        else if (ch == '\n' || ch == '\r')
            code = '=';
        else if (ch == 0x7F || ch == '\b')
            code = 'b';
        else if (ch == 'c' || ch == 'C')
            code = 'c';
        else if (ch == '@')
            code = 's';
        else if (ch == 'r' || ch == 'R')
            code = 'r';
        else if (ch == 'n' || ch == 'N')
            code = 'n';
    }
    if (code) {
        press_key(c, code);
        paint(c);
    }
}

static void calc_destroy(struct window *w)
{
    free(w->priv);
}

static void calc_restyle(struct window *w)
{
    paint(w->priv);
}

static const struct window_ops calc_ops = {
    .restyle = calc_restyle,
    .key = calc_key,
    .mouse = calc_mouse,
    .destroy = calc_destroy,
};

struct window *calculator_open(void)
{
    build_keys();
    struct calc *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->pressed = -1;
    struct window *w = wm_create("Calculator", ICON_CALC, CALC_W, CALC_H, WF_MINIMIZE, &calc_ops, c);
    if (!w) {
        free(c);
        return NULL;
    }
    c->win = w;
    paint(c);
    return w;
}
