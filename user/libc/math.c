/*
 * The math library. Doubles use the x87 FPU's own instructions: FSQRT,
 * FSIN, FCOS, FPTAN and FPATAN directly, logarithms with FYL2X and powers
 * of two with F2XM1 and FSCALE. Floats live in SSE registers: their square
 * root, absolute value and rounding stay there, and the rest go through
 * the double versions. (math.h makes sqrt, sqrtf, fabs and fabsf inline;
 * the parentheses below keep those macros off the definitions.)
 */
#include <math.h>
#include <stdint.h>

#define CW_ROUND_MASK 0x0C00
#define CW_DOWN       0x0400
#define CW_UP         0x0800
#define CW_CHOP       0x0C00

double (sqrt)(double x)
{
    double r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

double sin(double x)
{
    double r;
    __asm__("fsin" : "=t"(r) : "0"(x));
    return r;
}

double cos(double x)
{
    double r;
    __asm__("fcos" : "=t"(r) : "0"(x));
    return r;
}

double tan(double x)
{
    double r;
    __asm__("fptan\n\tfstp %%st(0)" : "=t"(r) : "0"(x));     /* FPTAN pushes 1.0 */
    return r;
}

double atan2(double y, double x)
{
    double r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

double atan(double x)
{
    return atan2(x, 1.0);
}

double asin(double x)
{
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x)
{
    return atan2(sqrt(1.0 - x * x), x);
}

double (fabs)(double x)
{
    double r;
    __asm__("fabs" : "=t"(r) : "0"(x));
    return r;
}

/* FRNDINT in the given rounding mode. */
static double round_mode(double x, unsigned short mode)
{
    unsigned short cw, ncw;
    double r;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    ncw = (unsigned short)((cw & ~CW_ROUND_MASK) | mode);
    __asm__ volatile("fldcw %0" : : "m"(ncw));
    __asm__ volatile("frndint" : "=t"(r) : "0"(x));
    __asm__ volatile("fldcw %0" : : "m"(cw));
    return r;
}

double floor(double x) { return round_mode(x, CW_DOWN); }
double ceil(double x) { return round_mode(x, CW_UP); }
double trunc(double x) { return round_mode(x, CW_CHOP); }

double round(double x)
{
    return x < 0 ? -floor(0.5 - x) : floor(x + 0.5);
}

double fmod(double x, double y)
{
    double r;
    /* FPREM reduces by up to 2^63 per pass; C2 says another pass is needed. */
    __asm__("1: fprem\n\t"
            "fnstsw %%ax\n\t"
            "testw $0x400, %%ax\n\t"
            "jnz 1b"
            : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    return r;
}

double log2(double x)
{
    double r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(1.0) : "st(1)");    /* 1 * log2(x) */
    return r;
}

double log(double x)
{
    return log2(x) * M_LN2;
}

double log10(double x)
{
    return log2(x) * 0.30102999566398119521;
}

double exp2(double x)
{
    if (x != x)
        return x;
    if (x > 16384)
        return HUGE_VAL;
    if (x < -16384)
        return 0;
    double i = round_mode(x, CW_DOWN), f = x - i, r;   /* 0 <= f < 1 */
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    r += 1.0;
    __asm__("fscale" : "=t"(r) : "0"(r), "u"(i));
    return r;
}

double exp(double x)
{
    return exp2(x * M_LOG2E);
}

double pow(double x, double y)
{
    if (y == 0)
        return 1;
    if (x == 0)
        return y > 0 ? 0 : HUGE_VAL;
    if (x > 0)
        return exp2(y * log2(x));
    /* A negative base needs a whole exponent. */
    double iy = trunc(y);
    if (iy != y)
        return NAN;
    double r = exp2(y * log2(-x));
    return fmod(iy, 2.0) != 0 ? -r : r;
}

float (sqrtf)(float x) { return __builtin_sqrtf(x); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float powf(float x, float y) { return (float)pow(x, y); }
float (fabsf)(float x) { return __builtin_fabsf(x); }

/* Without SSE4's ROUNDSS: truncate with CVTTSS2SI and step down or up.
 * From 2^23 on every float is already whole. */
float floorf(float x)
{
    if (!(fabsf(x) < 8388608.0f))
        return x;                       /* whole, infinite or NaN */
    float t = (float)(int)x;
    if (t == x)
        return x;                       /* keeps -0 */
    return t > x ? t - 1 : t;
}

float ceilf(float x)
{
    if (!(fabsf(x) < 8388608.0f))
        return x;
    float t = (float)(int)x;
    if (t == x)
        return x;
    return t < x ? t + 1 : t;
}

float truncf(float x) { return (float)trunc(x); }
float roundf(float x) { return (float)round(x); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
float expf(float x) { return (float)exp(x); }
float exp2f(float x) { return (float)exp2(x); }
float logf(float x) { return (float)log(x); }
float log2f(float x) { return (float)log2(x); }
float log10f(float x) { return (float)log10(x); }
float asinf(float x) { return (float)asin(x); }
float acosf(float x) { return (float)acos(x); }
float atanf(float x) { return (float)atan(x); }

/* ---- the rest of C99 ---- */

double cosh(double x)
{
    double e = exp(fabs(x));
    return 0.5 * (e + 1.0 / e);
}

double sinh(double x)
{
    if (fabs(x) < 1e-5)
        return x;                       /* exp(x) - exp(-x) would cancel */
    double e = exp(x);
    return 0.5 * (e - 1.0 / e);
}

double tanh(double x)
{
    if (x > 22)
        return 1;
    if (x < -22)
        return -1;
    if (fabs(x) < 1e-5)
        return x;
    double e = exp(2 * x);
    return (e - 1) / (e + 1);
}

double acosh(double x) { return log(x + sqrt(x * x - 1)); }

double asinh(double x)
{
    double r = log(fabs(x) + sqrt(x * x + 1));
    return x < 0 ? -r : r;
}

double atanh(double x) { return 0.5 * log((1 + x) / (1 - x)); }

/* The IEEE double's bits: sign (1), exponent (11, biased by 1023), fraction (52). */
union dbl {
    double d;
    uint64_t u;
};

double frexp(double x, int *e)
{
    union dbl v = { x };
    int exp = (int)((v.u >> 52) & 0x7FF);
    if (exp == 0x7FF || x == 0) {       /* NaN, infinity or zero: unchanged */
        *e = 0;
        return x;
    }
    if (exp == 0) {                     /* subnormal: scale up first */
        v.d = x * 18014398509481984.0;  /* 2^54 */
        exp = (int)((v.u >> 52) & 0x7FF) - 54;
    }
    *e = exp - 1022;
    v.u = (v.u & ~((uint64_t)0x7FF << 52)) | ((uint64_t)1022 << 52);
    return v.d;
}

double ldexp(double x, int n)
{
    double r;
    /* FSCALE multiplies by 2^trunc(st1); its result rounds to double on the way out. */
    __asm__("fscale" : "=t"(r) : "0"(x), "u"((double)n) : "st(1)");
    return r;
}

double scalbn(double x, int n) { return ldexp(x, n); }

double modf(double x, double *ipart)
{
    if (isinf(x)) {
        *ipart = x;
        return copysign(0.0, x);
    }
    double i = trunc(x);
    *ipart = i;
    return copysign(x - i, x);
}

double hypot(double x, double y)
{
    x = fabs(x);
    y = fabs(y);
    if (x < y) {
        double t = x;
        x = y;
        y = t;
    }
    if (x == 0)
        return 0;
    double r = y / x;                   /* scaled, so x*x cannot overflow */
    return x * sqrt(1 + r * r);
}

double cbrt(double x)
{
    if (x == 0 || isnan(x) || isinf(x))
        return x;
    double r = exp2(log2(fabs(x)) / 3);
    r -= (r * r * r - fabs(x)) / (3 * r * r);   /* one Newton step */
    return x < 0 ? -r : r;
}

double expm1(double x)
{
    if (fabs(x) < 1e-5)
        return x + x * x / 2;
    return exp(x) - 1;
}

double log1p(double x)
{
    if (fabs(x) < 1e-5)
        return x - x * x / 2;
    return log(1 + x);
}

double fmin(double x, double y) { return isnan(x) ? y : isnan(y) ? x : x < y ? x : y; }
double fmax(double x, double y) { return isnan(x) ? y : isnan(y) ? x : x > y ? x : y; }
double fdim(double x, double y) { return x > y ? x - y : 0; }
double fma(double x, double y, double z) { return x * y + z; }

double copysign(double x, double y)
{
    union dbl a = { x }, b = { y };
    a.u = (a.u & ~((uint64_t)1 << 63)) | (b.u & ((uint64_t)1 << 63));
    return a.d;
}

double rint(double x)
{
    double r;
    __asm__("frndint" : "=t"(r) : "0"(x));     /* the current rounding mode: to nearest */
    return r;
}

double nearbyint(double x) { return rint(x); }

double remainder(double x, double y)
{
    double r;
    __asm__("1: fprem1\n\t"
            "fnstsw %%ax\n\t"
            "testw $0x400, %%ax\n\t"
            "jnz 1b"
            : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    return r;
}

double logb(double x)
{
    if (x == 0)
        return -HUGE_VAL;
    if (isinf(x))
        return HUGE_VAL;
    if (isnan(x))
        return x;
    int e;
    frexp(x, &e);
    return e - 1;
}

int ilogb(double x)
{
    if (x == 0 || isnan(x))
        return FP_ILOGB0;
    if (isinf(x))
        return 2147483647;
    int e;
    frexp(x, &e);
    return e - 1;
}

double nan(const char *tag) { (void)tag; return NAN; }

long lrint(double x) { return (long)rint(x); }
long lround(double x) { return (long)round(x); }
long long llrint(double x) { return (long long)rint(x); }
long long llround(double x) { return (long long)round(x); }

float coshf(float x) { return (float)cosh(x); }
float sinhf(float x) { return (float)sinh(x); }
float tanhf(float x) { return (float)tanh(x); }
float acoshf(float x) { return (float)acosh(x); }
float asinhf(float x) { return (float)asinh(x); }
float atanhf(float x) { return (float)atanh(x); }
float frexpf(float x, int *e) { return (float)frexp(x, e); }
float ldexpf(float x, int n) { return (float)ldexp(x, n); }
float scalbnf(float x, int n) { return (float)ldexp(x, n); }

float modff(float x, float *ipart)
{
    double i;
    float r = (float)modf(x, &i);
    *ipart = (float)i;
    return r;
}

float hypotf(float x, float y) { return (float)hypot(x, y); }
float cbrtf(float x) { return (float)cbrt(x); }
float expm1f(float x) { return (float)expm1(x); }
float log1pf(float x) { return (float)log1p(x); }
float fminf(float x, float y) { return (float)fmin(x, y); }
float fmaxf(float x, float y) { return (float)fmax(x, y); }
float fdimf(float x, float y) { return (float)fdim(x, y); }
float fmaf(float x, float y, float z) { return (float)((double)x * y + z); }
float copysignf(float x, float y) { return (float)copysign(x, y); }
float rintf(float x) { return (float)rint(x); }
float nearbyintf(float x) { return (float)rint(x); }
float remainderf(float x, float y) { return (float)remainder(x, y); }
float logbf(float x) { return (float)logb(x); }
float nanf(const char *tag) { (void)tag; return NAN; }
long lrintf(float x) { return (long)rint(x); }
long lroundf(float x) { return (long)round(x); }
long long llrintf(float x) { return (long long)rint(x); }
long long llroundf(float x) { return (long long)round(x); }
int ilogbf(float x) { return ilogb(x); }

long double sqrtl(long double x)
{
    long double r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

long double fabsl(long double x)
{
    long double r;
    __asm__("fabs" : "=t"(r) : "0"(x));
    return r;
}

long double floorl(long double x) { return floor((double)x); }
long double ceill(long double x) { return ceil((double)x); }
long double truncl(long double x) { return trunc((double)x); }
long double roundl(long double x) { return round((double)x); }
long double fmodl(long double x, long double y) { return fmod((double)x, (double)y); }
long double sinl(long double x) { return sin((double)x); }
long double cosl(long double x) { return cos((double)x); }
long double tanl(long double x) { return tan((double)x); }
long double atan2l(long double y, long double x) { return atan2((double)y, (double)x); }
long double expl(long double x) { return exp((double)x); }
long double logl(long double x) { return log((double)x); }
long double powl(long double x, long double y) { return pow((double)x, (double)y); }
long double ldexpl(long double x, int n) { return ldexp((double)x, n); }
long double copysignl(long double x, long double y) { return copysign((double)x, (double)y); }

long double frexpl(long double x, int *e)
{
    return frexp((double)x, e);
}

long double modfl(long double x, long double *ipart)
{
    double i;
    long double r = modf((double)x, &i);
    *ipart = i;
    return r;
}
