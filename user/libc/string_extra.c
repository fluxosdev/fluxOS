/* String helpers that need malloc or tables, plus error/signal names. */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = 0;
    }
    return d;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d || !*a)
            return d;
    }
    return 0;
}

char *strcasestr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n))
            return (char *)hay;
    return n ? NULL : (char *)hay;
}

char *strtok_r(char *s, const char *delim, char **save)
{
    if (!s)
        s = *save;
    s += strspn(s, delim);
    if (!*s) {
        *save = s;
        return NULL;
    }
    char *end = s + strcspn(s, delim);
    if (*end)
        *end++ = 0;
    *save = end;
    return s;
}

char *strtok(char *s, const char *delim)
{
    static char *save;
    return strtok_r(s, delim, &save);
}

char *strerror(int err)
{
    switch (err) {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case ESRCH: return "No such process";
    case EINTR: return "Interrupted system call";
    case EIO: return "Input/output error";
    case E2BIG: return "Argument list too long";
    case ENOEXEC: return "Exec format error";
    case EBADF: return "Bad file descriptor";
    case ECHILD: return "No child processes";
    case EAGAIN: return "Resource temporarily unavailable";
    case ENOMEM: return "Cannot allocate memory";
    case EACCES: return "Permission denied";
    case EFAULT: return "Bad address";
    case ENOTBLK: return "Block device required";
    case EBUSY: return "Device or resource busy";
    case EEXIST: return "File exists";
    case EXDEV: return "Invalid cross-device link";
    case ENODEV: return "No such device";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case ENFILE: return "Too many open files in system";
    case EMFILE: return "Too many open files";
    case ENOTTY: return "Inappropriate ioctl for device";
    case EFBIG: return "File too large";
    case ENOSPC: return "No space left on device";
    case ESPIPE: return "Illegal seek";
    case EROFS: return "Read-only file system";
    case EPIPE: return "Broken pipe";
    case ERANGE: return "Numerical result out of range";
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS: return "Function not implemented";
    case ENOTEMPTY: return "Directory not empty";
    case ENOTSOCK: return "Socket operation on non-socket";
    case EDESTADDRREQ: return "Destination address required";
    case EMSGSIZE: return "Message too long";
    case EPROTOTYPE: return "Protocol wrong type for socket";
    case EPROTONOSUPPORT: return "Protocol not supported";
    case EOPNOTSUPP: return "Operation not supported";
    case EAFNOSUPPORT: return "Address family not supported by protocol";
    case EADDRINUSE: return "Address already in use";
    case EADDRNOTAVAIL: return "Cannot assign requested address";
    case ENETDOWN: return "Network is down";
    case ENETUNREACH: return "Network is unreachable";
    case ECONNABORTED: return "Software caused connection abort";
    case ECONNRESET: return "Connection reset by peer";
    case EISCONN: return "Transport endpoint is already connected";
    case ENOTCONN: return "Transport endpoint is not connected";
    case ETIMEDOUT: return "Connection timed out";
    case ECONNREFUSED: return "Connection refused";
    case EHOSTUNREACH: return "No route to host";
    }
    static char buf[32];
    snprintf(buf, sizeof(buf), "Unknown error %d", err);
    return buf;
}

const char *strsignal(int sig)
{
    switch (sig) {
    case SIGHUP: return "hangup";
    case SIGINT: return "interrupt";
    case SIGQUIT: return "quit";
    case SIGILL: return "illegal hardware instruction";
    case SIGTRAP: return "trace trap";
    case SIGABRT: return "abort";
    case SIGBUS: return "bus error";
    case SIGFPE: return "floating point exception";
    case SIGKILL: return "killed";
    case SIGUSR1: return "user-defined signal 1";
    case SIGSEGV: return "segmentation fault";
    case SIGUSR2: return "user-defined signal 2";
    case SIGPIPE: return "broken pipe";
    case SIGALRM: return "alarm";
    case SIGTERM: return "terminated";
    }
    return "unknown signal";
}
