#ifndef KERNEL_STDIO_H
#define KERNEL_STDIO_H

#include <stdarg.h>
#include <stddef.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif
