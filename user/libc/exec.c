/* exec helpers: environment passing and $PATH search. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

/* A file the kernel cannot run (ENOEXEC: a script without "#!") is handed to the shell, as
 * POSIX has execvp do. */
static void exec_script(const char *path, char *const argv[])
{
    int n = 0;
    while (argv[n])
        n++;
    char **nv = malloc(sizeof(char *) * (size_t)(n + 3));
    if (!nv)
        return;
    nv[0] = "sh";
    nv[1] = (char *)path;
    for (int i = 1; i < n; i++)
        nv[i + 1] = argv[i];
    nv[n > 0 ? n + 1 : 2] = NULL;
    execve("/bin/sh", nv, environ);
    free(nv);
    errno = ENOEXEC;
}

int execvp(const char *file, char *const argv[])
{
    if (strchr(file, '/')) {
        execve(file, argv, environ);
        if (errno == ENOEXEC)
            exec_script(file, argv);
        return -1;
    }

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
            if (errno == ENOEXEC)
                exec_script(buf, argv);
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
