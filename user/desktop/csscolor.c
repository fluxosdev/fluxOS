/*
 * CSS Color 4 and 5: the color functions css.c hands over, worked out in
 * floating point (so this file is built with the FPU, unlike css.c):
 * oklab(), oklch(), lab(), lch(), hwb(), color(srgb | srgb-linear |
 * display-p3 | xyz ...) and color-mix(in <space>, a [p%], b [p%]).
 * Colors outside sRGB are clipped to it.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

#define PI 3.14159265358979323846

struct rgba {
    double r, g, b, a;          /* linear-light sRGB, 0..1 (may stray outside), and alpha */
};

static double cube_root(double x)
{
    return x < 0 ? -pow(-x, 1.0 / 3) : pow(x, 1.0 / 3);
}

static double to_linear(double c)
{
    double a = fabs(c);
    double v = a <= 0.04045 ? a / 12.92 : pow((a + 0.055) / 1.055, 2.4);
    return c < 0 ? -v : v;
}

static double to_gamma(double c)
{
    double a = fabs(c);
    double v = a <= 0.0031308 ? a * 12.92 : 1.055 * pow(a, 1 / 2.4) - 0.055;
    return c < 0 ? -v : v;
}

static uint32_t pack(struct rgba c)
{
    double ch[4] = { to_gamma(c.r), to_gamma(c.g), to_gamma(c.b), c.a };
    uint32_t v[4];
    for (int i = 0; i < 4; i++) {
        double x = ch[i] != ch[i] ? 0 : ch[i] < 0 ? 0 : ch[i] > 1 ? 1 : ch[i];
        v[i] = (uint32_t)(x * 255 + 0.5);
    }
    return v[3] << 24 | v[0] << 16 | v[1] << 8 | v[2];
}

static struct rgba unpack(uint32_t c)
{
    struct rgba o = { to_linear(((c >> 16) & 255) / 255.0), to_linear(((c >> 8) & 255) / 255.0),
                      to_linear((c & 255) / 255.0), (c >> 24) / 255.0 };
    return o;
}

/* ---- the spaces ---- */

static struct rgba from_oklab(double L, double a, double b, double alpha)
{
    double l = L + 0.3963377774 * a + 0.2158037573 * b;
    double m = L - 0.1055613458 * a - 0.0638541728 * b;
    double s = L - 0.0894841775 * a - 1.2914855480 * b;
    l = l * l * l, m = m * m * m, s = s * s * s;
    struct rgba o = { 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                      -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                      -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s, alpha };
    return o;
}

static void to_oklab(struct rgba c, double *L, double *a, double *b)
{
    double l = cube_root(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b);
    double m = cube_root(0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b);
    double s = cube_root(0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    *L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    *a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    *b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}

static struct rgba from_xyz65(double x, double y, double z, double alpha)
{
    struct rgba o = { 3.2409699419 * x - 1.5373831776 * y - 0.4986107603 * z,
                      -0.9692436363 * x + 1.8759675015 * y + 0.0415550574 * z,
                      0.0556300797 * x - 0.2039769589 * y + 1.0569715142 * z, alpha };
    return o;
}

static struct rgba from_xyz50(double x, double y, double z, double alpha)
{
    double X = 0.9554734527 * x - 0.0230985369 * y + 0.0632593087 * z;
    double Y = -0.0283697070 * x + 1.0099954580 * y + 0.0210413990 * z;
    double Z = 0.0123140017 * x - 0.0205076964 * y + 1.3303659366 * z;
    return from_xyz65(X, Y, Z, alpha);
}

static struct rgba from_lab(double L, double a, double b, double alpha)
{
    const double eps = 216.0 / 24389, kappa = 24389.0 / 27;
    double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    double x = fx * fx * fx > eps ? fx * fx * fx : (116 * fx - 16) / kappa;
    double y = L > kappa * eps ? fy * fy * fy : L / kappa;
    double z = fz * fz * fz > eps ? fz * fz * fz : (116 * fz - 16) / kappa;
    return from_xyz50(x * 0.96422, y, z * 0.82521, alpha);
}

static struct rgba from_hsl(double h, double s, double l, double alpha)
{
    h = fmod(fmod(h, 360) + 360, 360);
    double c = (1 - fabs(2 * l - 1)) * s, x = c * (1 - fabs(fmod(h / 60, 2) - 1)), m = l - c / 2;
    double r = 0, g = 0, b = 0;
    if (h < 60) r = c, g = x;
    else if (h < 120) r = x, g = c;
    else if (h < 180) g = c, b = x;
    else if (h < 240) g = x, b = c;
    else if (h < 300) r = x, b = c;
    else r = c, b = x;
    struct rgba o = { to_linear(r + m), to_linear(g + m), to_linear(b + m), alpha };
    return o;
}

static struct rgba from_hwb(double h, double w, double bl, double alpha)
{
    if (w + bl >= 1) {
        double g = to_linear(w / (w + bl));
        struct rgba o = { g, g, g, alpha };
        return o;
    }
    struct rgba c = from_hsl(h, 1, 0.5, alpha);
    double k = 1 - w - bl;
    c.r = to_linear(to_gamma(c.r) * k + w);
    c.g = to_linear(to_gamma(c.g) * k + w);
    c.b = to_linear(to_gamma(c.b) * k + w);
    return c;
}

/* ---- reading the arguments ---- */

struct arg {
    double v;
    int pct;                    /* it was a percentage (v is then 0..100) */
    int none;
};

static const char *skip(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
        p++;
    return p;
}

/* A CSS number ("-1.5", ".25", "1e-3"); *end is where it stopped (p if there is none). */
static double number(const char *p, const char **end)
{
    const char *q = p;
    double sign = 1, v = 0;
    if (*q == '+' || *q == '-')
        sign = *q++ == '-' ? -1 : 1;
    if (!isdigit((unsigned char)*q) && !(*q == '.' && isdigit((unsigned char)q[1]))) {
        *end = p;
        return 0;
    }
    while (isdigit((unsigned char)*q))
        v = v * 10 + (*q++ - '0');
    if (*q == '.') {
        double scale = 0.1;
        for (q++; isdigit((unsigned char)*q); q++, scale /= 10)
            v += (*q - '0') * scale;
    }
    if ((*q == 'e' || *q == 'E') && (isdigit((unsigned char)q[1]) || ((q[1] == '-' || q[1] == '+') &&
                                                                       isdigit((unsigned char)q[2])))) {
        int e = 0, neg = 0;
        q++;
        if (*q == '-' || *q == '+')
            neg = *q++ == '-';
        while (isdigit((unsigned char)*q))
            e = e * 10 + (*q++ - '0');
        v *= pow(10, neg ? -e : e);
    }
    *end = q;
    return sign * v;
}

/* A number, percentage or angle (as degrees); returns the characters used. */
static int read_arg(const char *p, struct arg *a)
{
    a->pct = a->none = 0;
    if (!strncasecmp(p, "none", 4)) {
        a->none = 1;
        a->v = 0;
        return 4;
    }
    const char *end;
    a->v = number(p, &end);
    if (end == p)
        return 0;
    const char *u = end;
    if (*u == '%')
        a->pct = 1, u++;
    else if (!strncasecmp(u, "deg", 3))
        u += 3;
    else if (!strncasecmp(u, "grad", 4))
        a->v *= 0.9, u += 4;
    else if (!strncasecmp(u, "rad", 3))
        a->v *= 180 / PI, u += 3;
    else if (!strncasecmp(u, "turn", 4))
        a->v *= 360, u += 4;
    return (int)(u - p);
}

/* Up to three channels and an alpha after "/", inside the parentheses at p. */
static int read_args(const char *p, struct arg ch[3], double *alpha)
{
    *alpha = 1;
    for (int i = 0; i < 3; i++) {
        p = skip(p);
        int n = read_arg(p, &ch[i]);
        if (!n)
            return -1;
        p += n;
    }
    p = skip(p);
    if (*p == '/') {
        struct arg a;
        p = skip(p + 1);
        int n = read_arg(p, &a);
        if (!n)
            return -1;
        *alpha = a.none ? 1 : a.pct ? a.v / 100 : a.v;
        p += n;
    }
    p = skip(p);
    return *p == ')' ? 0 : -1;
}

/* A channel as a fraction: percentages of pct100 (the value 100% stands for). */
static double chan(const struct arg *a, double pct100)
{
    return a->pct ? a->v / 100 * pct100 : a->v;
}

static int function_color(const char *s, struct rgba *out);

/* One color of color-mix(): a color, then perhaps a percentage. */
static const char *mix_part(const char *p, const char *end, struct rgba *c, double *pct)
{
    p = skip(p);
    *pct = -1;
    const char *q = p;
    int depth = 0;
    char col[160];
    if (isdigit((unsigned char)*q) || *q == '.') {          /* "30% red" */
        struct arg a;
        int n = read_arg(q, &a);
        if (!n || !a.pct)
            return NULL;
        *pct = a.v;
        p = q = skip(q + n);
    }
    while (q < end && (depth || (*q != ',' && *q != ')'))) {
        if (*q == '(')
            depth++;
        else if (*q == ')')
            depth--;
        q++;
    }
    const char *e = q;
    while (e > p && e[-1] == ' ')
        e--;
    /* "red 30%": the percentage at the end */
    const char *sp = e;
    while (sp > p && sp[-1] != ' ' && sp[-1] != ')')
        sp--;
    if (sp > p && sp < e && e[-1] == '%' && *pct < 0) {
        struct arg a;
        if (read_arg(sp, &a) && a.pct) {
            *pct = a.v;
            e = sp;
            while (e > p && e[-1] == ' ')
                e--;
        }
    }
    if (e - p <= 0 || e - p >= (int)sizeof(col))
        return NULL;
    memcpy(col, p, (size_t)(e - p));
    col[e - p] = 0;
    uint32_t packed;
    if (function_color(col, c) < 0) {
        if (css_parse_color(col, (int)strlen(col), &packed) < 0)
            return NULL;
        *c = unpack(packed);
    }
    return q;
}

static double mix_hue(double a, double b, double t)
{
    double d = fmod(b - a + 540, 360) - 180;       /* the shorter way round */
    return a + d * t;
}

static int color_mix(const char *p, struct rgba *out)
{
    p = skip(p);
    if (strncasecmp(p, "in ", 3))
        return -1;
    p = skip(p + 3);
    char space[24];
    int n = 0;
    while ((isalnum((unsigned char)*p) || *p == '-') && n < 23)
        space[n++] = (char)tolower((unsigned char)*p++);
    space[n] = 0;
    while (*p && *p != ',')                         /* "shorter hue" and the like */
        p++;
    const char *end = p + strlen(p);
    struct rgba c1, c2;
    double p1, p2;
    if (!(p = mix_part(p + 1, end, &c1, &p1)) || *p != ',' || !(p = mix_part(p + 1, end, &c2, &p2)))
        return -1;
    if (p1 < 0 && p2 < 0)
        p1 = p2 = 50;
    else if (p1 < 0)
        p1 = 100 - p2;
    else if (p2 < 0)
        p2 = 100 - p1;
    double sum = p1 + p2;
    if (sum <= 0)
        return -1;
    double t = p2 / sum, keep = sum < 100 ? sum / 100 : 1;
    double a = c1.a * (1 - t) + c2.a * t;
    /* premultiplied */
    double w1 = c1.a * (1 - t), w2 = c2.a * t, wa = a > 0 ? a : 1;
    if (!strcmp(space, "oklab") || !strcmp(space, "oklch") || !strcmp(space, "lab") || !strcmp(space, "lch")) {
        double L1, A1, B1, L2, A2, B2;
        to_oklab(c1, &L1, &A1, &B1);
        to_oklab(c2, &L2, &A2, &B2);
        if (!strcmp(space, "oklch") || !strcmp(space, "lch")) {
            double C1 = sqrt(A1 * A1 + B1 * B1), C2 = sqrt(A2 * A2 + B2 * B2);
            double H1 = atan2(B1, A1) * 180 / PI, H2 = atan2(B2, A2) * 180 / PI;
            if (C1 < 1e-4)
                H1 = H2;
            if (C2 < 1e-4)
                H2 = H1;
            double L = (L1 * w1 + L2 * w2) / wa, C = (C1 * w1 + C2 * w2) / wa, H = mix_hue(H1, H2, t) * PI / 180;
            *out = from_oklab(L, C * cos(H), C * sin(H), a * keep);
        } else {
            *out = from_oklab((L1 * w1 + L2 * w2) / wa, (A1 * w1 + A2 * w2) / wa, (B1 * w1 + B2 * w2) / wa,
                              a * keep);
        }
        return 0;
    }
    if (!strcmp(space, "srgb-linear") || !strncmp(space, "xyz", 3)) {
        struct rgba o = { (c1.r * w1 + c2.r * w2) / wa, (c1.g * w1 + c2.g * w2) / wa, (c1.b * w1 + c2.b * w2) / wa,
                          a * keep };
        *out = o;
        return 0;
    }
    /* srgb, and (near enough) hsl, hwb and the rest: gamma-encoded */
    struct rgba o = { to_linear((to_gamma(c1.r) * w1 + to_gamma(c2.r) * w2) / wa),
                      to_linear((to_gamma(c1.g) * w1 + to_gamma(c2.g) * w2) / wa),
                      to_linear((to_gamma(c1.b) * w1 + to_gamma(c2.b) * w2) / wa), a * keep };
    *out = o;
    return 0;
}

/* color(space c1 c2 c3 [/ alpha]) */
static int color_fn(const char *p, struct rgba *out)
{
    p = skip(p);
    char space[24];
    int n = 0;
    while ((isalnum((unsigned char)*p) || *p == '-') && n < 23)
        space[n++] = (char)tolower((unsigned char)*p++);
    space[n] = 0;
    struct arg ch[3];
    double alpha;
    if (read_args(p, ch, &alpha) < 0)
        return -1;
    double x = chan(&ch[0], 1), y = chan(&ch[1], 1), z = chan(&ch[2], 1);
    if (!strcmp(space, "srgb") || !strcmp(space, "a98-rgb") || !strcmp(space, "rec2020") ||
        !strcmp(space, "prophoto-rgb")) {
        struct rgba o = { to_linear(x), to_linear(y), to_linear(z), alpha };
        *out = o;
    } else if (!strcmp(space, "srgb-linear")) {
        struct rgba o = { x, y, z, alpha };
        *out = o;
    } else if (!strcmp(space, "display-p3")) {
        double r = to_linear(x), g = to_linear(y), b = to_linear(z);
        *out = from_xyz65(0.4865709486 * r + 0.2656676932 * g + 0.1982172852 * b,
                          0.2289745641 * r + 0.6917385218 * g + 0.0792869141 * b,
                          0.0451133819 * g + 1.0439443689 * b, alpha);
    } else if (!strcmp(space, "xyz") || !strcmp(space, "xyz-d65")) {
        *out = from_xyz65(x, y, z, alpha);
    } else if (!strcmp(space, "xyz-d50")) {
        *out = from_xyz50(x, y, z, alpha);
    } else {
        return -1;
    }
    return 0;
}

static int function_color(const char *s, struct rgba *out)
{
    char name[16];
    int n = 0;
    while ((isalpha((unsigned char)s[n]) || s[n] == '-') && n < 15) {
        name[n] = (char)tolower((unsigned char)s[n]);
        n++;
    }
    name[n] = 0;
    if (s[n] != '(')
        return -1;
    const char *p = s + n + 1;
    if (!strcmp(name, "color-mix"))
        return color_mix(p, out);
    if (!strcmp(name, "color"))
        return color_fn(p, out);
    struct arg ch[3];
    double alpha;
    if (read_args(p, ch, &alpha) < 0)
        return -1;
    if (!strcmp(name, "oklab")) {
        *out = from_oklab(chan(&ch[0], 1), chan(&ch[1], 0.4), chan(&ch[2], 0.4), alpha);
    } else if (!strcmp(name, "oklch")) {
        double C = chan(&ch[1], 0.4), H = ch[2].v * PI / 180;
        *out = from_oklab(chan(&ch[0], 1), C * cos(H), C * sin(H), alpha);
    } else if (!strcmp(name, "lab")) {
        *out = from_lab(chan(&ch[0], 100), chan(&ch[1], 125), chan(&ch[2], 125), alpha);
    } else if (!strcmp(name, "lch")) {
        double C = chan(&ch[1], 150), H = ch[2].v * PI / 180;
        *out = from_lab(chan(&ch[0], 100), C * cos(H), C * sin(H), alpha);
    } else if (!strcmp(name, "hwb")) {
        *out = from_hwb(ch[0].v, chan(&ch[1], 1) / (ch[1].pct ? 1 : 100), chan(&ch[2], 1) / (ch[2].pct ? 1 : 100),
                        alpha);
    } else {
        return -1;
    }
    return 0;
}

int css_color_function(const char *s, uint32_t *out)
{
    struct rgba c;
    if (function_color(s, &c) < 0)
        return -1;
    *out = pack(c);
    return 0;
}
