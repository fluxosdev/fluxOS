/* Shared insides of the C library (not installed). */
#ifndef LIBC_INTERNAL_H
#define LIBC_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* printf.c: format into put(ctx, bytes, n). Returns the number of bytes
 * produced, or -1 (errno EOVERFLOW) past INT_MAX. */
typedef void (*fmt_put_fn)(void *ctx, const char *s, size_t n);
int __vformat(fmt_put_fn put, void *ctx, const char *fmt, va_list ap);

/* strtod.c: the number at s, as strtold() reads it, and where it ends. */
long double __strtold(const char *s, char **end, int *range_error);

/* scanf.c: read through get/unget. get returns a byte or -1. */
struct scan_in {
    int (*get)(void *ctx);
    void (*unget)(void *ctx, int c);
    void *ctx;
};
int __vscan(struct scan_in *in, const char *fmt, va_list ap);

/* stdio.c */
struct FILE;
int __fwritable(struct FILE *f);

/* time.c: the local time zone's offset in seconds east of UTC. */
long __tz_offset(void);

#endif
