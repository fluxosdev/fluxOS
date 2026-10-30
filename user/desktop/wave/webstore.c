/*
 * Small files the browser keeps (cookies, localStorage): read whole, written whole and moved into
 * place, so a power cut leaves the old file rather than half of a new one. (Kept out of jsdom.c,
 * which the host tests build with the host's headers.)
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
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

/* ---- IndexedDB ---- */

#ifndef IDB_DIR
#define IDB_DIR "/home/flux/.browser_idb/"
#endif

/* A database's file: IDB_DIR, a hash of its origin and name, ".idb". It starts "origin\nname\n". */
static void idb_path(const char *origin, const char *name, char *out, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (const char *p = origin; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    h = (h ^ '\n') * 1099511628211ull;
    for (const char *p = name; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    snprintf(out, size, IDB_DIR "%08x%08x.idb", (unsigned)(h >> 32), (unsigned)h);
}

/* Does the file's header name this origin (and this name, if one is given)? Then where its text starts. */
static const char *idb_header(const char *file, const char *origin, const char *name)
{
    size_t ol = strlen(origin);
    if (strncmp(file, origin, ol) || file[ol] != '\n')
        return NULL;
    const char *n = file + ol + 1, *nl = strchr(n, '\n');
    if (!nl || (name && ((size_t)(nl - n) != strlen(name) || strncmp(n, name, (size_t)(nl - n)))))
        return NULL;
    return nl + 1;
}

char *store_idb(const char *op, const char *origin, const char *name, const char *text, size_t len)
{
    char path[300];
    if (!strcmp(op, "list")) {
        DIR *dir = opendir(IDB_DIR);
        size_t cap = 256, used = 0;
        char *out = malloc(cap);
        struct dirent *e;
        while (out && dir && (e = readdir(dir))) {
            size_t el = strlen(e->d_name);
            if (el < 5 || strcmp(e->d_name + el - 4, ".idb"))
                continue;
            snprintf(path, sizeof(path), IDB_DIR "%s", e->d_name);
            long flen;
            char *file = store_read_file(path, 64l << 20, &flen);
            const char *body = file ? idb_header(file, origin, NULL) : NULL;
            if (body) {
                const char *n = file + strlen(origin) + 1;
                size_t nl = (size_t)(body - 1 - n);
                if (used + nl + 2 > cap) {
                    char *grown = realloc(out, cap = (used + nl + 2) * 2);
                    if (!grown) {
                        free(file);
                        break;
                    }
                    out = grown;
                }
                memcpy(out + used, n, nl);
                used += nl;
                out[used++] = '\n';
            }
            free(file);
        }
        if (dir)
            closedir(dir);
        if (out)
            out[used] = 0;
        return out;
    }
    idb_path(origin, name, path, sizeof(path));
    if (!strcmp(op, "load")) {
        long flen;
        char *file = store_read_file(path, 64l << 20, &flen);
        const char *body = file ? idb_header(file, origin, name) : NULL;
        char *out = body ? strdup(body) : NULL;
        free(file);
        return out;
    }
    if (!strcmp(op, "delete")) {
        unlink(path);
        return NULL;
    }
    if (!strcmp(op, "save")) {
        mkdir(IDB_DIR, 0700);
        size_t hl = strlen(origin) + strlen(name) + 2;
        char *file = malloc(hl + len + 1);
        if (file) {
            snprintf(file, hl + 1, "%s\n%s\n", origin, name);
            memcpy(file + hl, text, len);
            store_write_file(path, file, hl + len);
            free(file);
        }
    }
    return NULL;
}
