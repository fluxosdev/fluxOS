/*
 * Small files the browser keeps (cookies, localStorage): read whole, written whole and moved into
 * place, so a power cut leaves the old file rather than half of a new one. (Kept out of jsdom.c,
 * which the host tests build with the host's headers.)
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "web.h"

/* The file's contents (malloc'd, with a NUL after them), or NULL if it is missing, empty or over max bytes. */
char *store_read_file(const char *path, long max, long *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    long size = (long)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size <= 0 || size > max) {
        close(fd);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    long got = 0;
    while (buf && got < size) {
        int r = (int)read(fd, buf + got, (size_t)(size - got));
        if (r <= 0)
            break;
        got += r;
    }
    close(fd);
    if (!buf || got <= 0) {
        free(buf);
        return NULL;
    }
    buf[got] = 0;
    *len = got;
    return buf;
}

/* Write data to path by way of path.tmp. 0, or -1. */
int store_write_file(const char *path, const char *data, size_t len)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    int ok = 1;
    while (len > 0 && ok) {
        int w = (int)write(fd, data, len);
        if (w <= 0)
            ok = 0;
        else
            data += w, len -= (size_t)w;
    }
    if (close(fd) < 0)
        ok = 0;
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* When the file was last changed, in seconds since 1970 (0 if it cannot be told). */
long store_file_mtime(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 ? (long)sb.st_mtime : 0;
}
