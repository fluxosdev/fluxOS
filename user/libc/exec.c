/* exec helpers: environment passing and $PATH search. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    if (strchr(file, '/'))
        return execve(file, argv, environ);

    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/bin";

    int saved = ENOENT;
    char buf[PATH_MAX];
    while (*path) {
        size_t len = strcspn(path, ":");
        if (len + strlen(file) + 2 <= sizeof(buf)) {
            if (len) {
                memcpy(buf, path, len);
                buf[len] = '/';
                strcpy(buf + len + 1, file);
            } else {
                strcpy(buf, file);
            }
            execve(buf, argv, environ);
            if (errno != ENOENT)
                saved = errno;
        }
        path += len;
        if (*path == ':')
            path++;
    }
    errno = saved;
    return -1;
}
