/*
 * Mathematical functions. Floats and doubles are both worked on with
 * SSE2; the square roots and absolute values below compile to single
 * instructions in place.
 */
#ifndef _MATH_H
#define _MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LN2      0.69314718055994530942
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440
#define M_LOG10E   0.43429448190325182765
#define M_LN10     2.30258509299404568402
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)

double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);

float sqrtf(float x);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atan2f(float y, float x);
float powf(float x, float y);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float fmodf(float x, float y);
float expf(float x);
float exp2f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);

/* The rest of C99's set. */
double cosh(double x);
double sinh(double x);
double tanh(double x);
double acosh(double x);
double asinh(double x);
double atanh(double x);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double scalbn(double x, int exp);
double modf(double x, double *ipart);
double hypot(double x, double y);
double cbrt(double x);
double expm1(double x);
double log1p(double x);
double fmin(double x, double y);
double fmax(double x, double y);
double fdim(double x, double y);
double fma(double x, double y, double z);
double copysign(double x, double y);
double rint(double x);
double nearbyint(double x);
double remainder(double x, double y);
double logb(double x);
double nan(const char *tag);
long lrint(double x);
long lround(double x);
long long llrint(double x);
long long llround(double x);
int ilogb(double x);

float coshf(float x);
float sinhf(float x);
float tanhf(float x);
float acoshf(float x);
float asinhf(float x);
float atanhf(float x);
float frexpf(float x, int *exp);
float ldexpf(float x, int exp);
float scalbnf(float x, int exp);
float modff(float x, float *ipart);
float hypotf(float x, float y);
float cbrtf(float x);
float expm1f(float x);
float log1pf(float x);
float fminf(float x, float y);
float fmaxf(float x, float y);
float fdimf(float x, float y);
float fmaf(float x, float y, float z);
float copysignf(float x, float y);
float rintf(float x);
float nearbyintf(float x);
float remainderf(float x, float y);
float logbf(float x);
float nanf(const char *tag);
long lrintf(float x);
long lroundf(float x);
long long llrintf(float x);
long long llroundf(float x);
int ilogbf(float x);

/* long double: the x87's own precision, through the double versions. */
long double sqrtl(long double x);
long double fabsl(long double x);
long double floorl(long double x);
long double ceill(long double x);
long double truncl(long double x);
long double roundl(long double x);
long double fmodl(long double x, long double y);
long double sinl(long double x);
long double cosl(long double x);
long double tanl(long double x);
long double atan2l(long double y, long double x);
long double expl(long double x);
long double logl(long double x);
long double powl(long double x, long double y);
long double frexpl(long double x, int *exp);
long double ldexpl(long double x, int exp);
long double modfl(long double x, long double *ipart);
long double copysignl(long double x, long double y);

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x)  __builtin_signbit(x)
#define isgreater(x, y)      __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y)         __builtin_isless(x, y)
#define islessequal(x, y)    __builtin_islessequal(x, y)
#define islessgreater(x, y)  __builtin_islessgreater(x, y)
#define isunordered(x, y)    __builtin_isunordered(x, y)
#define HUGE_VALL (__builtin_huge_vall())
#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERREXCEPT
#define FP_ILOGB0   (-2147483647 - 1)
#define FP_ILOGBNAN (-2147483647 - 1)
typedef double double_t;
typedef float float_t;

/* (C++'s <cmath> wants the functions themselves.) */
#ifndef __cplusplus
#define sqrt(x)  __builtin_sqrt(x)
#define sqrtf(x) __builtin_sqrtf(x)
#define fabs(x)  __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)
#endif

#ifdef __cplusplus
}
#endif

#endif
