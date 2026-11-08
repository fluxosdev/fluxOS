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

/* FSIN, FCOS and FPTAN leave |x| >= 2^63 as it is: bring it below 2pi first (FPREM1, as glibc's
 * x87 versions did; a reduction by the 64-bit 2pi, so the result is in range but not exact). */
static long double trig_arg(double x)
{
    long double r = x;
    if (fabs(x) >= 9223372036854775808.0 && !isinf(x))
        __asm__("1: fprem1\n\t"
                "fnstsw %%ax\n\t"
                "testw $0x400, %%ax\n\t"
                "jnz 1b"
                : "=t"(r) : "0"(r), "u"(6.28318530717958647692528676655900577L) : "ax", "cc");
    return r;
}

double sin(double x)
{
    long double r;
    __asm__("fsin" : "=t"(r) : "0"(trig_arg(x)));
    return (double)r;
}

double cos(double x)
{
    long double r;
    __asm__("fcos" : "=t"(r) : "0"(trig_arg(x)));
    return (double)r;
}

/* Both at once (a GNU extension, which GCC calls for sin and cos of one value). */
void sincos(double x, double *s, double *c)
{
    long double sv, cv;
    __asm__("fsincos" : "=t"(cv), "=u"(sv) : "0"(trig_arg(x)));
    *s = (double)sv;
    *c = (double)cv;
}

double tan(double x)
{
    long double r;
    __asm__("fptan\n\tfstp %%st(0)" : "=t"(r) : "0"(trig_arg(x)));     /* FPTAN pushes 1.0 */
    return (double)r;
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
static long double round_mode(long double x, unsigned short mode)
{
    unsigned short cw, ncw;
    long double r;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    ncw = (unsigned short)((cw & ~CW_ROUND_MASK) | mode);
    __asm__ volatile("fldcw %0" : : "m"(ncw));
    __asm__ volatile("frndint" : "=t"(r) : "0"(x));
    __asm__ volatile("fldcw %0" : : "m"(cw));
    return r;
}

double floor(double x) { return (double)round_mode(x, CW_DOWN); }
double ceil(double x) { return (double)round_mode(x, CW_UP); }
double trunc(double x) { return (double)round_mode(x, CW_CHOP); }

/* Halves away from zero. x - trunc(x) is exact, where x + 0.5 would round (0.49999999999999994,
 * or odd numbers from 2^52 on). */
double round(double x)
{
    double t = trunc(x);
    if (fabs(x - t) >= 0.5)
        t += x < 0 ? -1.0 : 1.0;
    return t;
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

static long double log2_x87(long double x)
{
    long double r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(1.0L) : "st(1)");   /* 1 * log2(x) */
    return r;
}

double log2(double x)
{
    return (double)log2_x87(x);
}

double log(double x)
{
    return (double)(log2_x87(x) * 0.693147180559945309417232121458176568L);
}

double log10(double x)
{
    return (double)(log2_x87(x) * 0.301029995663981195213738894724493027L);
}

/* 2^x in the x87's 64-bit precision. */
static long double exp2_x87(long double x)
{
    if (x != x)
        return x;
    if (x > 16384)
        return __builtin_infl();
    if (x < -16384)
        return 0;
    long double i = round_mode(x, CW_DOWN), f = x - i, r;      /* 0 <= f < 1 */
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    r += 1.0L;
    __asm__("fscale" : "=t"(r) : "0"(r), "u"(i));
    return r;
}

double exp2(double x)
{
    return (double)exp2_x87(x);
}

double exp(double x)
{
    return (double)exp2_x87(x * 1.44269504088896340735992468100189214L);
}

/* C's special cases first. A whole exponent up to 2^16 is multiplied out in 64-bit precision, so
 * exact powers come out exact (pow(10, 2) is 100, not 99.99999999999997); the rest is
 * 2^(y log2 x) with log2 x kept to 64 bits. */
double pow(double x, double y)
{
    if (y == 0 || x == 1)
        return 1;
    if (x != x || y != y)
        return x + y;
    double ax = fabs(x);
    if (isinf(y)) {
        if (ax == 1)
            return 1;
        return (ax > 1) == (y > 0) ? HUGE_VAL : 0;
    }
    int whole = trunc(y) == y;
    int odd = whole && fabs(y) < 9007199254740992.0 && fmod(y, 2.0) != 0;
    if (x == 0)
        return y > 0 ? (odd ? x : 0) : (odd ? 1 / x : HUGE_VAL);
    if (isinf(x)) {
        double r = y > 0 ? HUGE_VAL : 0;
        return x < 0 && odd ? -r : r;
    }
    if (x < 0 && !whole)
        return NAN;                     /* a negative base needs a whole exponent */
    long double r;
    if (whole && fabs(y) <= 65536) {
        long double b = ax;
        unsigned n = (unsigned)fabs(y);
        r = 1;
        for (; n; n >>= 1, b *= b)
            if (n & 1)
                r *= b;
        if (y < 0)
            r = 1 / r;
    } else {
        r = exp2_x87(y * log2_x87(ax));
    }
    double d = (double)r;
    return x < 0 && odd ? -d : d;
}

float (sqrtf)(float x) { return __builtin_sqrtf(x); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }

void sincosf(float x, float *s, float *c)
{
    double sv, cv;
    sincos(x, &sv, &cv);
    *s = (float)sv;
    *c = (float)cv;
}
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
    /* FSCALE multiplies by 2^trunc(st1); its result rounds to double on the way out. It leaves
     * st1 on the stack, which GCC pops as a dead input (no st(1) clobber: that would say the asm
     * popped it, and the x87 stack filled up a call at a time, to NaN). */
    __asm__("fscale" : "=t"(r) : "0"(x), "u"((double)n));
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

/* The Bessel function of the first kind, order 1 (POSIX's j1; swscale's Bessel filter), from
 * Abramowitz and Stegun 9.4.4 and 9.4.6: a polynomial up to 3, the asymptotic form beyond,
 * good to 1e-7 or so. J1 is odd. */
double j1(double x)
{
    double a = fabs(x), r;
    if (isnan(x))
        return x;
    if (a <= 3) {
        double t = a * a / 9;
        r = a * (0.5 + t * (-0.56249985 + t * (0.21093573 + t * (-0.03954289 + t * (0.00443319 +
                 t * (-0.00031761 + t * 0.00001109))))));
    } else if (isinf(a)) {
        r = 0;
    } else {
        double t = 3 / a;
        double f = 0.79788456 + t * (0.00000156 + t * (0.01659667 + t * (0.00017105 + t * (-0.00249511 +
                   t * (0.00113653 - t * 0.00020033)))));
        double th = a - 2.35619449 + t * (0.12499612 + t * (0.00005650 + t * (-0.00637879 + t * (0.00074348 +
                    t * (0.00079824 - t * 0.00029166)))));
        r = f * cos(th) / sqrt(a);
    }
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
