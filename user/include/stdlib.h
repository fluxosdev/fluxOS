#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFFFFFF
#define MB_CUR_MAX 1                    /* the "C" locale: a byte is a character */

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void *reallocarray(void *p, size_t count, size_t size);
void free(void *p);
void *memalign(size_t align, size_t n);
void *aligned_alloc(size_t align, size_t n);
int posix_memalign(void **out, size_t align, size_t n);
void *valloc(size_t n);

void exit(int status) __attribute__((noreturn));
void _Exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*fn)(void));
int system(const char *command);

char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *str);
int clearenv(void);

int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
long double strtold(const char *s, char **end);

int abs(int x);
long labs(long x);
long long llabs(long long x);
div_t div(int num, int den);
ldiv_t ldiv(long num, long den);
lldiv_t lldiv(long long num, long long den);

int rand(void);
int rand_r(unsigned int *seed);
void srand(unsigned int seed);
long random(void);
void srandom(unsigned int seed);
double drand48(void);
long lrand48(void);
long mrand48(void);
void srand48(long seed);

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

int mkstemp(char *tmpl);
int mkostemp(char *tmpl, int flags);
int mkstemps(char *tmpl, int suffixlen);
char *mkdtemp(char *tmpl);
char *mktemp(char *tmpl);
char *realpath(const char *path, char *resolved);
int getloadavg(double loadavg[], int n);

int mblen(const char *s, size_t n);
int mbtowc(wchar_t *wc, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);

#endif
