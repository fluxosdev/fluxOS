/*
 * Mathematical functions, computed with the x87 FPU. Only programs built
 * with the FPU allowed (not -mgeneral-regs-only, see the Makefile's
 * USER_FP_CFLAGS) can use them.
 */
#ifndef _MATH_H
#define _MATH_H

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LN2      0.69314718055994530942
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_SQRT2    1.41421356237309504880

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

#endif
