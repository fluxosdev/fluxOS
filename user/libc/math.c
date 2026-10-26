/*
 * The math library. Doubles use the x87 FPU's own instructions: FSQRT,
 * FSIN, FCOS, FPTAN and FPATAN directly, logarithms with FYL2X and powers
 * of two with F2XM1 and FSCALE. Floats live in SSE registers: their square
 * root, absolute value and rounding stay there, and the rest go through
 * the double versions. (math.h makes sqrt, sqrtf, fabs and fabsf inline;
 * the parentheses below keep those macros off the definitions.)
 */
#include <math.h>

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
