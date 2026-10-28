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
    case EOVERFLOW: return "Value too large for defined data type";
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
    case ENXIO: return "No such device or address";
    case ETXTBSY: return "Text file busy";
    case EMLINK: return "Too many links";
    case EDOM: return "Numerical argument out of domain";
    case EDEADLK: return "Resource deadlock avoided";
    case ENOLCK: return "No locks available";
    case ELOOP: return "Too many levels of symbolic links";
    case ENOMSG: return "No message of desired type";
    case EIDRM: return "Identifier removed";
    case ENOSTR: return "Device not a stream";
    case ENODATA: return "No data available";
    case ETIME: return "Timer expired";
    case ENOSR: return "Out of streams resources";
    case ENOLINK: return "Link has been severed";
    case EPROTO: return "Protocol error";
    case EMULTIHOP: return "Multihop attempted";
    case EBADMSG: return "Bad message";
    case EILSEQ: return "Invalid or incomplete multibyte or wide character";
    case ENOPROTOOPT: return "Protocol not available";
    case ENETRESET: return "Network dropped connection on reset";
    case ENOBUFS: return "No buffer space available";
    case EALREADY: return "Operation already in progress";
    case EINPROGRESS: return "Operation now in progress";
    case ESTALE: return "Stale file handle";
    case EDQUOT: return "Disk quota exceeded";
    case ECANCELED: return "Operation canceled";
    case EOWNERDEAD: return "Owner died";
    case ENOTRECOVERABLE: return "State not recoverable";
    }
    static char buf[32];
    snprintf(buf, sizeof(buf), "Unknown error %d", err);
    return buf;
}

int strerror_r(int err, char *buf, size_t size)
{
    const char *m = strerror(err);
    size_t n = strlen(m);
    if (!size)
        return ERANGE;
    if (n >= size) {
        memcpy(buf, m, size - 1);
        buf[size - 1] = 0;
        return ERANGE;
    }
    memcpy(buf, m, n + 1);
    return 0;
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
    case SIGCHLD: return "child exited";
    case SIGCONT: return "continued";
    case SIGSTOP: return "stopped (signal)";
    case SIGTSTP: return "stopped";
    case SIGTTIN: return "stopped (tty input)";
    case SIGTTOU: return "stopped (tty output)";
    case SIGURG: return "urgent I/O condition";
    case SIGXCPU: return "CPU time limit exceeded";
    case SIGXFSZ: return "file size limit exceeded";
    case SIGVTALRM: return "virtual timer expired";
    case SIGPROF: return "profiling timer expired";
    case SIGWINCH: return "window changed";
    case SIGIO: return "I/O possible";
    case SIGSYS: return "bad system call";
    }
    return "unknown signal";
}

/* ---- more of <string.h> and <strings.h> ---- */

void *memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--)
        if (p[n] == (unsigned char)c)
            return (void *)(p + n);
    return NULL;
}

void *mempcpy(void *dst, const void *src, size_t n)
{
    return (char *)memcpy(dst, src, n) + n;
}

void *memccpy(void *dst, const void *src, int c, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
        if (s[i] == (unsigned char)c)
            return d + i + 1;
    }
    return NULL;
}

void *memmem(const void *hay, size_t hlen, const void *needle, size_t nlen)
{
    const unsigned char *h = hay, *n = needle;
    if (!nlen)
        return (void *)hay;
    if (nlen > hlen)
        return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        const unsigned char *p = memchr(h + i, n[0], hlen - nlen + 1 - i);
        if (!p)
            return NULL;
        i = (size_t)(p - h);
        if (!memcmp(p, n, nlen))
            return (void *)p;
    }
    return NULL;
}

int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

size_t strxfrm(char *dst, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (n)
        strlcpy(dst, src, n);
    return len;
}

char *stpcpy(char *dst, const char *src)
{
    size_t n = strlen(src);
    memcpy(dst, src, n + 1);
    return dst + n;
}

char *stpncpy(char *dst, const char *src, size_t n)
{
    size_t len = strnlen(src, n);
    memcpy(dst, src, len);
    memset(dst + len, 0, n - len);
    return dst + len;
}

char *strchrnul(const char *s, int c)
{
    while (*s && *s != (char)c)
        s++;
    return (char *)s;
}

char *strsep(char **s, const char *delim)
{
    char *start = *s;
    if (!start)
        return NULL;
    char *end = start + strcspn(start, delim);
    if (*end) {
        *end = 0;
        *s = end + 1;
    } else {
        *s = NULL;
    }
    return start;
}

void bzero(void *s, size_t n) { memset(s, 0, n); }
void bcopy(const void *src, void *dst, size_t n) { memmove(dst, src, n); }
int bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n) != 0; }
char *index(const char *s, int c) { return strchr(s, c); }
char *rindex(const char *s, int c) { return strrchr(s, c); }
int ffs(int v) { return v ? __builtin_ffs(v) : 0; }
int ffsl(long v) { return v ? __builtin_ffsl(v) : 0; }
int ffsll(long long v) { return v ? __builtin_ffsll(v) : 0; }
