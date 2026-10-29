/*
 * find - walk directory trees and pick files out
 *
 *   find [path...] [expression]
 *
 * Tests: -name PAT, -iname PAT, -path PAT, -type f|d|l|c|b|p|s, -newer
 * FILE, -mtime [+-]N, -size [+-]N[ckM], -empty, -true, -false; actions:
 * -print (the default), -print0, -prune, -delete, -exec cmd {} \; and
 * -exec cmd {} +, -ok; joined with -a (or nothing), -o, ! or -not and
 * \( \). -maxdepth N and -mindepth N come first. Symbolic links are not
 * followed (-L does).
 */
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { X_NAME, X_INAME, X_PATH, X_TYPE, X_NEWER, X_MTIME, X_SIZE, X_EMPTY, X_TRUE, X_FALSE,
       X_PRINT, X_PRINT0, X_PRUNE, X_DELETE, X_EXEC, X_EXECPLUS,
       X_AND, X_OR, X_NOT };

struct expr {
    int op;
    char *arg;
    char **argv;                        /* -exec: the command, NULL-terminated, "{}" kept */
    int argc;
    long num;
    int sign;                           /* -mtime, -size: '+', '-' or 0 */
    char unit;
    time_t when;
    struct expr *a, *b;
};

static int status, follow, maxdepth = -1, mindepth, pruned, has_action;
static time_t now;
static const char *prog = "find";

static struct expr *parse_or(char **argv, int *i, int argc);

static struct expr *node(int op)
{
    struct expr *e = calloc(1, sizeof(*e));
    if (!e) {
        fprintf(stderr, "find: out of memory\n");
        exit(1);
    }
    e->op = op;
    return e;
}

static void need_arg(char **argv, int i, int argc)
{
    if (i >= argc) {
        fprintf(stderr, "find: missing argument to `%s'\n", argv[i - 1]);
        exit(1);
    }
}

static struct expr *parse_primary(char **argv, int *i, int argc)
{
    if (*i >= argc) {
        fprintf(stderr, "find: expected an expression\n");
        exit(1);
    }
    const char *t = argv[(*i)++];
    struct expr *e;
    if (!strcmp(t, "(")) {
        e = parse_or(argv, i, argc);
        if (*i >= argc || strcmp(argv[*i], ")")) {
            fprintf(stderr, "find: missing `)'\n");
            exit(1);
        }
        (*i)++;
        return e;
    }
    if (!strcmp(t, "!") || !strcmp(t, "-not")) {
        e = node(X_NOT);
        e->a = parse_primary(argv, i, argc);
        return e;
    }
    if (!strcmp(t, "-name") || !strcmp(t, "-iname") || !strcmp(t, "-path") || !strcmp(t, "-wholename") ||
        !strcmp(t, "-type") || !strcmp(t, "-newer")) {
        need_arg(argv, *i, argc);
        e = node(t[1] == 'n' && t[2] == 'a' ? X_NAME : t[1] == 'i' ? X_INAME : t[1] == 'p' || t[1] == 'w' ? X_PATH
                 : t[1] == 't' ? X_TYPE : X_NEWER);
        e->arg = argv[(*i)++];
        if (e->op == X_TYPE && (strlen(e->arg) != 1 || !strchr("fdlcbps", e->arg[0]))) {
            fprintf(stderr, "find: unknown argument to -type: %s\n", e->arg);
            exit(1);
        }
        if (e->op == X_NEWER) {
            struct stat st;
            if (stat(e->arg, &st) < 0) {
                fprintf(stderr, "find: %s: %s\n", e->arg, strerror(errno));
                exit(1);
            }
            e->when = st.st_mtime;
        }
        return e;
    }
    if (!strcmp(t, "-mtime") || !strcmp(t, "-size") || !strcmp(t, "-maxdepth") || !strcmp(t, "-mindepth")) {
        need_arg(argv, *i, argc);
        const char *v = argv[(*i)++];
        if (t[1] == 'm' && t[2] == 'a') {
            maxdepth = atoi(v);
            return node(X_TRUE);
        }
        if (t[1] == 'm' && t[2] == 'i') {
            mindepth = atoi(v);
            return node(X_TRUE);
        }
        e = node(t[1] == 'm' ? X_MTIME : X_SIZE);
        e->sign = *v == '+' || *v == '-' ? *v : 0;
        char *end;
        e->num = strtol(e->sign ? v + 1 : v, &end, 10);
        e->unit = *end ? *end : (e->op == X_SIZE ? 'b' : 0);
        return e;
    }
    if (!strcmp(t, "-empty")) return node(X_EMPTY);
    if (!strcmp(t, "-true")) return node(X_TRUE);
    if (!strcmp(t, "-false")) return node(X_FALSE);
    if (!strcmp(t, "-print")) { has_action = 1; return node(X_PRINT); }
    if (!strcmp(t, "-print0")) { has_action = 1; return node(X_PRINT0); }
    if (!strcmp(t, "-prune")) return node(X_PRUNE);
    if (!strcmp(t, "-delete")) { has_action = 1; return node(X_DELETE); }
    if (!strcmp(t, "-exec") || !strcmp(t, "-ok") || !strcmp(t, "-execdir")) {
        has_action = 1;
        e = node(X_EXEC);
        e->argv = argv + *i;
        while (*i < argc && strcmp(argv[*i], ";") && !(!strcmp(argv[*i], "+") && *i > 0 && !strcmp(argv[*i - 1], "{}")))
            (*i)++;
        if (*i >= argc) {
            fprintf(stderr, "find: missing argument to `-exec'\n");
            exit(1);
        }
        e->argc = (int)(argv + *i - e->argv);
        if (!strcmp(argv[*i], "+")) {
            e->op = X_EXECPLUS;
            e->argc--;                  /* the {} before + */
        }
        argv[(*i)++] = NULL;
        if (e->argc < 1) {
            fprintf(stderr, "find: missing argument to `-exec'\n");
            exit(1);
        }
        return e;
    }
    fprintf(stderr, "find: unknown predicate `%s'\n", t);
    exit(1);
}

static struct expr *parse_and(char **argv, int *i, int argc)
{
    struct expr *e = parse_primary(argv, i, argc);
    while (*i < argc && strcmp(argv[*i], "-o") && strcmp(argv[*i], "-or") && strcmp(argv[*i], ")")) {
        if (!strcmp(argv[*i], "-a") || !strcmp(argv[*i], "-and"))
            (*i)++;
        struct expr *n = node(X_AND);
        n->a = e;
        n->b = parse_primary(argv, i, argc);
        e = n;
    }
    return e;
}

static struct expr *parse_or(char **argv, int *i, int argc)
{
    struct expr *e = parse_and(argv, i, argc);
    while (*i < argc && (!strcmp(argv[*i], "-o") || !strcmp(argv[*i], "-or"))) {
        (*i)++;
        struct expr *n = node(X_OR);
        n->a = e;
        n->b = parse_and(argv, i, argc);
        e = n;
    }
    return e;
}

/* ---- -exec ... {} + collects paths and runs the command in batches ---- */

struct batch {
    struct expr *e;
    char **args;
    int n, cap;
    size_t bytes;
};
static struct batch *batches;
static int nbatches;

static int run_command(char **argv)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("find: fork");
        return 0;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "find: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void flush_batch(struct batch *b)
{
    if (!b->n)
        return;
    int argc = b->e->argc + b->n;
    char **argv = malloc(sizeof(char *) * (size_t)(argc + 1));
    int k = 0;
    for (int i = 0; i < b->e->argc; i++)
        argv[k++] = b->e->argv[i];
    for (int i = 0; i < b->n; i++)
        argv[k++] = b->args[i];
    argv[k] = NULL;
    if (!run_command(argv))
        status = 1;
    free(argv);
    for (int i = 0; i < b->n; i++)
        free(b->args[i]);
    b->n = 0;
    b->bytes = 0;
}

static void add_to_batch(struct expr *e, const char *path)
{
    struct batch *b = NULL;
    for (int i = 0; i < nbatches; i++)
        if (batches[i].e == e)
            b = &batches[i];
    if (!b) {
        batches = realloc(batches, sizeof(*batches) * (size_t)(nbatches + 1));
        b = &batches[nbatches++];
        memset(b, 0, sizeof(*b));
        b->e = e;
    }
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->args = realloc(b->args, sizeof(char *) * (size_t)b->cap);
    }
    b->args[b->n++] = strdup(path);
    b->bytes += strlen(path) + 1;
    if (b->bytes > 65536 || b->n >= 4096)
        flush_batch(b);
}

/* ---- evaluation ---- */

static int run_exec(struct expr *e, const char *path)
{
    char **argv = malloc(sizeof(char *) * (size_t)(e->argc + 1));
    for (int i = 0; i < e->argc; i++) {
        if (!strcmp(e->argv[i], "{}"))
            argv[i] = (char *)path;
        else if (strstr(e->argv[i], "{}")) {
            const char *b = strstr(e->argv[i], "{}");
            char *s = malloc(strlen(e->argv[i]) + strlen(path) + 1);
            sprintf(s, "%.*s%s%s", (int)(b - e->argv[i]), e->argv[i], path, b + 2);
            argv[i] = s;
        } else
            argv[i] = e->argv[i];
    }
    argv[e->argc] = NULL;
    int ok = run_command(argv);
    free(argv);
    return ok;
}

static int eval(struct expr *e, const char *path, const char *name, const struct stat *st, int depth)
{
    switch (e->op) {
    case X_NAME: return fnmatch(e->arg, name, 0) == 0;
    case X_INAME: return fnmatch(e->arg, name, FNM_CASEFOLD) == 0;
    case X_PATH: return fnmatch(e->arg, path, 0) == 0;
    case X_TYPE:
        switch (e->arg[0]) {
        case 'f': return S_ISREG(st->st_mode);
        case 'd': return S_ISDIR(st->st_mode);
        case 'l': return S_ISLNK(st->st_mode);
        case 'c': return S_ISCHR(st->st_mode);
        case 'b': return S_ISBLK(st->st_mode);
        case 'p': return S_ISFIFO(st->st_mode);
        case 's': return S_ISSOCK(st->st_mode);
        }
        return 0;
    case X_NEWER: return st->st_mtime > e->when;
    case X_MTIME: {
        long days = (long)((now - st->st_mtime) / 86400);
        return e->sign == '+' ? days > e->num : e->sign == '-' ? days < e->num : days == e->num;
    }
    case X_SIZE: {
        long long unit = e->unit == 'c' ? 1 : e->unit == 'k' ? 1024 : e->unit == 'M' ? 1048576
                       : e->unit == 'G' ? 1073741824LL : e->unit == 'w' ? 2 : 512;
        long long v = (st->st_size + unit - 1) / unit;
        return e->sign == '+' ? v > e->num : e->sign == '-' ? v < e->num : v == e->num;
    }
    case X_EMPTY:
        if (S_ISREG(st->st_mode))
            return st->st_size == 0;
        if (S_ISDIR(st->st_mode)) {
            DIR *d = opendir(path);
            if (!d)
                return 0;
            struct dirent *de;
            int empty = 1;
            while ((de = readdir(d)))
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
                    empty = 0;
                    break;
                }
            closedir(d);
            return empty;
        }
        return 0;
    case X_TRUE: return 1;
    case X_FALSE: return 0;
    case X_PRINT: printf("%s\n", path); return 1;
    case X_PRINT0: fputs(path, stdout); putchar(0); return 1;
    case X_PRUNE: pruned = 1; return 1;
    case X_DELETE:
        if ((S_ISDIR(st->st_mode) ? rmdir(path) : unlink(path)) < 0) {
            fprintf(stderr, "find: cannot delete '%s': %s\n", path, strerror(errno));
            status = 1;
            return 0;
        }
        return 1;
    case X_EXEC: return run_exec(e, path);
    case X_EXECPLUS: add_to_batch(e, path); return 1;
    case X_AND: return eval(e->a, path, name, st, depth) && eval(e->b, path, name, st, depth);
    case X_OR: return eval(e->a, path, name, st, depth) || eval(e->b, path, name, st, depth);
    case X_NOT: return !eval(e->a, path, name, st, depth);
    }
    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void walk(const char *path, struct expr *e, int depth)
{
    struct stat st;
    if ((follow ? stat(path, &st) : lstat(path, &st)) < 0) {
        fprintf(stderr, "find: '%s': %s\n", path, strerror(errno));
        status = 1;
        return;
    }
    const char *name = strrchr(path, '/');
    name = name && name[1] ? name + 1 : path;
    if (!strcmp(path, "/"))
        name = "/";
    pruned = 0;
    int is_dir = S_ISDIR(st.st_mode);
    int delete_dir = 0;
    if (depth >= mindepth)
        eval(e, path, name, &st, depth);
    if (!is_dir || pruned || (maxdepth >= 0 && depth >= maxdepth))
        return;
    (void)delete_dir;
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "find: '%s': %s\n", path, strerror(errno));
        status = 1;
        return;
    }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, sizeof(char *) * (size_t)cap);
        }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(char *), cmp_name);
    for (int i = 0; i < n; i++) {
        char sub[PATH_MAX];
        size_t pl = strlen(path);
        snprintf(sub, sizeof(sub), "%s%s%s", path, pl && path[pl - 1] == '/' ? "" : "/", names[i]);
        walk(sub, e, depth + 1);
        free(names[i]);
    }
    free(names);
}

int main(int argc, char **argv)
{
    int i = 1;
    now = time(NULL);
    while (i < argc && (!strcmp(argv[i], "-L") || !strcmp(argv[i], "-H") || !strcmp(argv[i], "-P"))) {
        follow = argv[i][1] == 'L';
        i++;
    }
    int first_path = i;
    while (i < argc && argv[i][0] != '-' && strcmp(argv[i], "(") && strcmp(argv[i], "!"))
        i++;
    int npaths = i - first_path;
    struct expr *e;
    if (i < argc) {
        e = parse_or(argv, &i, argc);
        if (i < argc) {
            fprintf(stderr, "find: unexpected `%s'\n", argv[i]);
            return 1;
        }
    } else {
        e = node(X_TRUE);
    }
    if (!has_action) {
        struct expr *n = node(X_AND);
        n->a = e;
        n->b = node(X_PRINT);
        e = n;
    }
    (void)prog;
    if (npaths == 0)
        walk(".", e, 0);
    for (int k = 0; k < npaths; k++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s", argv[first_path + k]);
        size_t l = strlen(path);
        while (l > 1 && path[l - 1] == '/')
            path[--l] = 0;
        walk(path, e, 0);
    }
    for (int k = 0; k < nbatches; k++)
        flush_batch(&batches[k]);
    fflush(stdout);
    return status;
}
