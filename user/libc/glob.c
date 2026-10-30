/* glob: shell patterns expanded over directories (fnmatch.c matches). */
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct gl {
    char **v;
    size_t n, cap;
    int flags;
    int (*errfunc)(const char *, int);
    int error;
};

static int add(struct gl *g, const char *path, int isdir)
{
    if (g->n + 2 > g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 16;
        char **nv = realloc(g->v, cap * sizeof(char *));
        if (!nv)
            return -1;
        g->v = nv;
        g->cap = cap;
    }
    size_t len = strlen(path);
    char *s = malloc(len + 2);
    if (!s)
        return -1;
    memcpy(s, path, len + 1);
    if (isdir && (g->flags & GLOB_MARK) && (!len || s[len - 1] != '/')) {
        s[len] = '/';
        s[len + 1] = 0;
    }
    g->v[g->n++] = s;
    return 0;
}

static int has_magic(const char *s, size_t n, int flags)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '*' || s[i] == '?' || s[i] == '[')
            return 1;
        if (s[i] == '\\' && !(flags & GLOB_NOESCAPE) && i + 1 < n)
            i++;
    }
    return 0;
}

/* Expand rest (a pattern) below dir (the path so far, "" for relative). */
static int expand(struct gl *g, const char *dir, const char *rest)
{
    while (*rest == '/')
        rest++;
    if (!*rest) {
        struct stat st;
        int isdir = stat(*dir ? dir : ".", &st) == 0 && S_ISDIR(st.st_mode);
        return add(g, dir, isdir);
    }
    const char *slash = strchr(rest, '/');
    size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
    char path[PATH_MAX];
    if (!has_magic(rest, len, g->flags)) {
        /* a plain component: take it as it is, without the backslashes */
        char comp[NAME_MAX + 1];
        size_t c = 0;
        for (size_t i = 0; i < len && c < NAME_MAX; i++) {
            if (rest[i] == '\\' && !(g->flags & GLOB_NOESCAPE) && i + 1 < len)
                i++;
            comp[c++] = rest[i];
        }
        comp[c] = 0;
        snprintf(path, sizeof(path), "%s%s%s", dir, *dir && dir[strlen(dir) - 1] != '/' ? "/" : "", comp);
        struct stat st;
        if (stat(path, &st) < 0)
            return 0;
        return expand(g, path, rest + len);
    }
    char pat[NAME_MAX + 1];
    if (len > NAME_MAX)
        return 0;
    memcpy(pat, rest, len);
    pat[len] = 0;
    DIR *d = opendir(*dir ? dir : ".");
    if (!d) {
        if ((g->errfunc && g->errfunc(*dir ? dir : ".", errno)) || (g->flags & GLOB_ERR)) {
            g->error = GLOB_ABORTED;
            return -1;
        }
        return 0;
    }
    int fflags = (g->flags & GLOB_NOESCAPE ? FNM_NOESCAPE : 0) | (g->flags & GLOB_PERIOD ? 0 : FNM_PERIOD);
    struct dirent *de;
    int r = 0;
    while ((de = readdir(d)) && r == 0) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            if (pat[0] != '.')
                continue;
        if (fnmatch(pat, de->d_name, fflags))
            continue;
        snprintf(path, sizeof(path), "%s%s%s", dir, *dir && dir[strlen(dir) - 1] != '/' ? "/" : "", de->d_name);
        if (rest[len]) {                /* more components: this must be a directory */
            struct stat st;
            if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
                continue;
        }
        r = expand(g, path, rest + len);
    }
    closedir(d);
    return r;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *path, int err), glob_t *pglob)
{
    struct gl g = { NULL, 0, 0, flags, errfunc, 0 };
    size_t offs = (flags & GLOB_DOOFFS) ? pglob->gl_offs : 0;
    size_t old = 0;
    if (flags & GLOB_APPEND) {
        old = pglob->gl_pathc;
        g.v = pglob->gl_pathv;
        g.n = offs + old;
        g.cap = g.n + 1;
    } else {
        for (size_t i = 0; i < offs; i++)
            if (add(&g, "", 0) < 0)
                return GLOB_NOSPACE;
        for (size_t i = 0; i < offs; i++) {
            free(g.v[i]);
            g.v[i] = NULL;
        }
    }
    size_t before = g.n;
    int r;
    if (pattern[0] == '/')
        r = expand(&g, "/", pattern + 1);
    else
        r = expand(&g, "", pattern);
    if (r < 0 && !g.error)
        g.error = GLOB_NOSPACE;
    if (!g.error && g.n == before) {
        if (flags & GLOB_NOCHECK) {
            if (add(&g, pattern, 0) < 0)
                g.error = GLOB_NOSPACE;
        } else {
            g.error = GLOB_NOMATCH;
        }
    }
    if (!(flags & GLOB_NOSORT) && g.n > before)
        qsort(g.v + before, g.n - before, sizeof(char *), cmp_str);
    if (g.v)
        g.v[g.n] = NULL;
    pglob->gl_pathv = g.v;
    pglob->gl_pathc = g.n - offs;
    if (!(flags & GLOB_DOOFFS))
        pglob->gl_offs = 0;
    return g.error;
}

void globfree(glob_t *g)
{
    if (!g->gl_pathv)
        return;
    for (size_t i = 0; i < g->gl_pathc; i++)
        free(g->gl_pathv[g->gl_offs + i]);
    free(g->gl_pathv);
    g->gl_pathv = NULL;
    g->gl_pathc = 0;
}
