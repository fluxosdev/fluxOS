/*
 * The test (or [) expression evaluator, shared by the shell's builtin and
 * the test program (user/bin/test.c includes this file).
 *
 * Unary: -b -c -d -e -f -g -h -k -L -n -p -r -s -S -t -u -w -x -z; binary:
 * = == != < > -eq -ne -lt -le -gt -ge -nt -ot -ef; ! negation, -a and -o
 * (-a binds tighter), and \( \) grouping. Returns 0 for true, 1 for false,
 * 2 for a bad expression (said on standard error as prog).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct test_state {
    char **a;
    int n, pos, err;
    const char *prog;
};

static int te_or(struct test_state *s);

static const char *te_peek(struct test_state *s)
{
    return s->pos < s->n ? s->a[s->pos] : NULL;
}

static int te_file(char op, const char *path)
{
    struct stat st;
    if (op == 'h' || op == 'L')
        return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
    if (op == 'r')
        return access(path, R_OK) == 0;
    if (op == 'w')
        return access(path, W_OK) == 0;
    if (op == 'x')
        return access(path, X_OK) == 0;
    if (stat(path, &st) < 0)
        return 0;
    switch (op) {
    case 'e': return 1;
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'b': return S_ISBLK(st.st_mode);
    case 'p': return S_ISFIFO(st.st_mode);
    case 'S': return S_ISSOCK(st.st_mode);
    case 's': return st.st_size > 0;
    case 'g': return (st.st_mode & S_ISGID) != 0;
    case 'u': return (st.st_mode & S_ISUID) != 0;
    case 'k': return (st.st_mode & S_ISVTX) != 0;
    }
    return 0;
}

static int te_is_unary(const char *s)
{
    return s && s[0] == '-' && s[1] && !s[2] && strchr("bcdefghkLnprsStuwxz", s[1]);
}

static int te_is_binary(const char *s)
{
    static const char *const ops[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
                                       "-nt", "-ot", "-ef", NULL };
    for (int i = 0; s && ops[i]; i++)
        if (!strcmp(s, ops[i]))
            return 1;
    return 0;
}

static long te_num(struct test_state *s, const char *v)
{
    char *end;
    long x = strtol(v, &end, 10);
    if (!*v || *end) {
        fprintf(stderr, "%s: %s: integer expression expected\n", s->prog, v);
        s->err = 1;
    }
    return x;
}

static int te_binary(struct test_state *s, const char *a, const char *op, const char *b)
{
    if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(a, b);
    if (!strcmp(op, "!=")) return strcmp(a, b) != 0;
    if (!strcmp(op, "<")) return strcmp(a, b) < 0;
    if (!strcmp(op, ">")) return strcmp(a, b) > 0;
    if (op[1] == 'n' || op[1] == 'o' || op[1] == 'e') {
        if (!strcmp(op, "-nt") || !strcmp(op, "-ot") || !strcmp(op, "-ef")) {
            struct stat sa, sb;
            int ra = stat(a, &sa), rb = stat(b, &sb);
            if (op[1] == 'e')
                return ra == 0 && rb == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
            if (op[1] == 'n')
                return ra == 0 && (rb < 0 || sa.st_mtime > sb.st_mtime);
            return rb == 0 && (ra < 0 || sa.st_mtime < sb.st_mtime);
        }
    }
    long x = te_num(s, a), y = te_num(s, b);
    if (!strcmp(op, "-eq")) return x == y;
    if (!strcmp(op, "-ne")) return x != y;
    if (!strcmp(op, "-lt")) return x < y;
    if (!strcmp(op, "-le")) return x <= y;
    if (!strcmp(op, "-gt")) return x > y;
    return x >= y;
}

static int te_primary(struct test_state *s)
{
    const char *t = te_peek(s);
    if (!t) {
        fprintf(stderr, "%s: argument expected\n", s->prog);
        s->err = 1;
        return 0;
    }
    int left = s->n - s->pos;
    if (!strcmp(t, "!") && left > 1) {
        s->pos++;
        return !te_primary(s);
    }
    if (!strcmp(t, "(") && left > 1) {
        s->pos++;
        int v = te_or(s);
        if (!te_peek(s) || strcmp(te_peek(s), ")")) {
            fprintf(stderr, "%s: ')' expected\n", s->prog);
            s->err = 1;
            return v;
        }
        s->pos++;
        return v;
    }
    if (left >= 3 && te_is_binary(s->a[s->pos + 1])) {
        s->pos += 3;
        return te_binary(s, s->a[s->pos - 3], s->a[s->pos - 2], s->a[s->pos - 1]);
    }
    if (te_is_unary(t) && left >= 2) {
        char op = t[1];
        const char *arg = s->a[s->pos + 1];
        s->pos += 2;
        if (op == 'z') return arg[0] == 0;
        if (op == 'n') return arg[0] != 0;
        if (op == 't') return isatty((int)te_num(s, arg));
        return te_file(op, arg);
    }
    s->pos++;
    return t[0] != 0;
}

static int te_and(struct test_state *s)
{
    int v = te_primary(s);
    while (te_peek(s) && !strcmp(te_peek(s), "-a")) {
        s->pos++;
        int w = te_primary(s);
        v = v && w;
    }
    return v;
}

static int te_or(struct test_state *s)
{
    int v = te_and(s);
    while (te_peek(s) && !strcmp(te_peek(s), "-o")) {
        s->pos++;
        int w = te_and(s);
        v = v || w;
    }
    return v;
}

/* argv[0] is "test" or "["; returns the exit status. */
int test_eval(int argc, char **argv)
{
    struct test_state s = { argv + 1, argc - 1, 0, 0, argv[0] };
    if (!strcmp(argv[0], "[")) {
        if (argc < 2 || strcmp(argv[argc - 1], "]")) {
            fprintf(stderr, "[: missing ']'\n");
            return 2;
        }
        s.n--;
    }
    if (s.n == 0)
        return 1;
    int v = te_or(&s);
    if (s.err)
        return 2;
    if (s.pos < s.n) {
        fprintf(stderr, "%s: too many arguments\n", s.prog);
        return 2;
    }
    return v ? 0 : 1;
}
