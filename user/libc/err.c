/* <err.h> and <libgen.h>: messages prefixed by the program's name, and
 * the parts of a path. */
#include <err.h>
#include <errno.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The program's name, from argv[0] (stdlib.c keeps it). */
extern const char *__progname;

void vwarn(const char *fmt, va_list ap)
{
    int e = errno;
    fprintf(stderr, "%s: ", __progname);
    if (fmt) {
        vfprintf(stderr, fmt, ap);
        fputs(": ", stderr);
    }
    fprintf(stderr, "%s\n", strerror(e));
}

void vwarnx(const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: ", __progname);
    if (fmt)
        vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void verr(int status, const char *fmt, va_list ap)
{
    vwarn(fmt, ap);
    exit(status);
}

void verrx(int status, const char *fmt, va_list ap)
{
    vwarnx(fmt, ap);
    exit(status);
}

void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarn(fmt, ap);
    va_end(ap);
}

void warnx(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarnx(fmt, ap);
    va_end(ap);
}

void err(int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verr(status, fmt, ap);
}

void errx(int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verrx(status, fmt, ap);
}

/* POSIX's basename and dirname: they may change path, and "" or NULL is ".". */
char *basename(char *path)
{
    static char dot[] = ".";
    if (!path || !*path)
        return dot;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        path[--n] = 0;
    char *slash = strrchr(path, '/');
    if (!slash || n == 1)
        return path;
    return slash + 1;
}

char *dirname(char *path)
{
    static char dot[] = ".";
    if (!path || !*path)
        return dot;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    while (n > 0 && path[n - 1] != '/')
        n--;
    if (!n)
        return dot;
    while (n > 1 && path[n - 1] == '/')
        n--;
    path[n] = 0;
    return path;
}
