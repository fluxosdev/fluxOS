/* Shell builtins and the alias table. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fsh.h"

#define NSIGS 32

/* ---- aliases ---- */

struct alias {
    char *name;
    char *value;
};

static struct alias *aliases;
static int naliases;

const char *alias_get(const char *name)
{
    for (int i = 0; i < naliases; i++)
        if (!strcmp(aliases[i].name, name))
            return aliases[i].value;
    return NULL;
}

static void alias_set(const char *name, const char *value)
{
    for (int i = 0; i < naliases; i++) {
        if (!strcmp(aliases[i].name, name)) {
            free(aliases[i].value);
            aliases[i].value = xstrdup(value);
            return;
        }
    }
    aliases = xrealloc(aliases, sizeof(struct alias) * (size_t)(naliases + 1));
    aliases[naliases].name = xstrdup(name);
    aliases[naliases].value = xstrdup(value);
    naliases++;
}

static int alias_remove(const char *name)
{
    for (int i = 0; i < naliases; i++) {
        if (!strcmp(aliases[i].name, name)) {
            free(aliases[i].name);
            free(aliases[i].value);
            aliases[i] = aliases[--naliases];
            return 0;
        }
    }
    return -1;
}

static void print_quoted(const char *s)
{
    if (*s && !strpbrk(s, " \t\n'\"\\$`*?[]|&;<>()#~")) {
        fputs(s, stdout);
        return;
    }
    putchar('\'');
    for (; *s; s++) {
        if (*s == '\'')
            fputs("'\\''", stdout);
        else
            putchar(*s);
    }
    putchar('\'');
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ---- directory changes ---- */

#define DIRSTACK_MAX 16
static char *dirstack[DIRSTACK_MAX];
static int ndirs;

static int change_dir(const char *dir, int print)
{
    char old[PATH_MAX];
    if (!getcwd(old, sizeof(old)))
        old[0] = 0;
    if (chdir(dir) < 0) {
        fsh_error("cd: %s: %s", errmsg(errno), dir);
        return 1;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        setenv("PWD", cwd, 1);
    if (old[0])
        setenv("OLDPWD", old, 1);
    if (print)
        printf("%s\n", cwd);
    return 0;
}

static int bi_cd(int argc, char **argv)
{
    const char *dir;
    if (argc < 2) {
        dir = getenv("HOME");
        if (!dir)
            dir = "/";
        return change_dir(dir, 0);
    }
    if (!strcmp(argv[1], "-")) {
        dir = getenv("OLDPWD");
        if (!dir) {
            fsh_error("cd: no previous directory");
            return 1;
        }
        return change_dir(dir, 1);
    }
    return change_dir(argv[1], 0);
}

static int bi_pwd(int argc, char **argv)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        fsh_error("pwd: %s", errmsg(errno));
        return 1;
    }
    puts(cwd);
    return 0;
}

static void print_tilde(const char *path)
{
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(path, home, hl) && (path[hl] == '/' || !path[hl]))
        printf("~%s", path + hl);
    else
        fputs(path, stdout);
}

static int bi_dirs(int argc, char **argv)
{
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        print_tilde(cwd);
    for (int i = ndirs - 1; i >= 0; i--) {
        putchar(' ');
        print_tilde(dirstack[i]);
    }
    putchar('\n');
    return 0;
}

static int bi_pushd(int argc, char **argv)
{
    char cwd[PATH_MAX];
    if (argc < 2) {
        fsh_error("pushd: no other directory");
        return 1;
    }
    if (ndirs >= DIRSTACK_MAX) {
        fsh_error("pushd: directory stack full");
        return 1;
    }
    if (!getcwd(cwd, sizeof(cwd)) || change_dir(argv[1], 0))
        return 1;
    dirstack[ndirs++] = xstrdup(cwd);
    return bi_dirs(0, NULL);
}

static int bi_popd(int argc, char **argv)
{
    if (!ndirs) {
        fsh_error("popd: directory stack empty");
        return 1;
    }
    char *dir = dirstack[--ndirs];
    int r = change_dir(dir, 0);
    free(dir);
    if (!r)
        bi_dirs(0, NULL);
    return r;
}

/* ---- output ---- */

/* Interpret backslash escapes; returns 1 if \c (stop) was seen. */
static int put_escaped(const char *s)
{
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) {
            putchar(*s);
            continue;
        }
        switch (*++s) {
        case 'n': putchar('\n'); break;
        case 't': putchar('\t'); break;
        case 'r': putchar('\r'); break;
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'e':
        case 'E': putchar('\033'); break;
        case 'v': putchar('\v'); break;
        case 'f': putchar('\f'); break;
        case '\\': putchar('\\'); break;
        case 'c': return 1;
        case '0': {
            int v = 0;
            for (int k = 0; k < 3 && s[1] >= '0' && s[1] <= '7'; k++)
                v = v * 8 + (*++s - '0');
            putchar(v);
            break;
        }
        case 'x': {
            int v = 0;
            for (int k = 0; k < 2 && isxdigit((unsigned char)s[1]); k++) {
                char c = *++s;
                v = v * 16 + (isdigit((unsigned char)c) ? c - '0' : tolower(c) - 'a' + 10);
            }
            putchar(v);
            break;
        }
        default:
            putchar('\\');
            putchar(*s);
            break;
        }
    }
    return 0;
}

static int bi_echo(int argc, char **argv)
{
    int newline = 1, escapes = 1, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *f = argv[i] + 1;
        if (strspn(f, "neEr") != strlen(f))
            break;
        for (; *f; f++) {
            if (*f == 'n') newline = 0;
            else if (*f == 'e') escapes = 1;
            else escapes = 0;
        }
    }
    for (; i < argc; i++) {
        if (escapes) {
            if (put_escaped(argv[i]))
                return 0;
        } else {
            fputs(argv[i], stdout);
        }
        if (i < argc - 1)
            putchar(' ');
    }
    if (newline)
        putchar('\n');
    return 0;
}

static int bi_printf(int argc, char **argv)
{
    if (argc < 2) {
        fsh_error("printf: not enough arguments");
        return 1;
    }
    const char *fmt = argv[1];
    int ai = 2;
    do {
        for (const char *p = fmt; *p; p++) {
            if (*p == '\\') {
                char tmp[3] = { '\\', p[1], 0 };
                if (p[1])
                    p++;
                put_escaped(tmp);
                continue;
            }
            if (*p != '%') {
                putchar(*p);
                continue;
            }
            if (p[1] == '%') {
                putchar('%');
                p++;
                continue;
            }
            char spec[32];
            int k = 0;
            spec[k++] = *p++;
            while (*p && strchr("-+ #0123456789.", *p) && k < 28)
                spec[k++] = *p++;
            if (!*p)
                break;
            char conv = *p;
            const char *arg = ai < argc ? argv[ai++] : NULL;
            char out[512];
            if (conv == 's' || conv == 'b') {
                spec[k++] = 's';
                spec[k] = 0;
                snprintf(out, sizeof(out), spec, arg ? arg : "");
                if (conv == 'b')
                    put_escaped(out);
                else
                    fputs(out, stdout);
            } else if (conv == 'c') {
                if (arg && *arg)
                    putchar(*arg);
            } else if (strchr("diuxXo", conv)) {
                spec[k++] = 'l';
                spec[k++] = 'l';
                spec[k++] = conv;
                spec[k] = 0;
                long long v = 0;
                if (arg && (arg[0] == '\'' || arg[0] == '"'))
                    v = (unsigned char)arg[1];
                else if (arg)
                    v = strtoll(arg, NULL, 0);
                snprintf(out, sizeof(out), spec, v);
                fputs(out, stdout);
            } else if (strchr("eEfFgGaA", conv)) {
                spec[k++] = conv;
                spec[k] = 0;
                snprintf(out, sizeof(out), spec, arg ? strtod(arg, NULL) : 0.0);
                fputs(out, stdout);
            } else {
                putchar('%');
                putchar(conv);
            }
        }
    } while (ai < argc && ai > 2);
    return 0;
}

/* ---- variables ---- */

static void print_env_sorted(const char *prefix)
{
    int n = 0;
    while (environ && environ[n])
        n++;
    char **copy = xmalloc(sizeof(char *) * (size_t)(n + 1));
    memcpy(copy, environ, sizeof(char *) * (size_t)n);
    qsort(copy, (size_t)n, sizeof(char *), cmp_str);
    for (int i = 0; i < n; i++) {
        const char *eq = strchr(copy[i], '=');
        if (!eq)
            continue;
        printf("%s%.*s=", prefix, (int)(eq - copy[i]), copy[i]);
        print_quoted(eq + 1);
        putchar('\n');
    }
    free(copy);
}

static int bi_export(int argc, char **argv)
{
    if (argc < 2) {
        print_env_sorted("export ");
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) {
            print_env_sorted("export ");
            continue;
        }
        char *eq = strchr(argv[i], '=');
        size_t nlen = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!is_valid_name(argv[i], nlen)) {
            fsh_error("export: not an identifier: %s", argv[i]);
            status = 1;
            continue;
        }
        if (eq)
            assign_env(argv[i]);
        else if (!getenv(argv[i]))
            setenv(argv[i], "", 1);
    }
    return status;
}

static int bi_unset(int argc, char **argv)
{
    int funcs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) {
            funcs = 1;
            continue;
        }
        if (!strcmp(argv[i], "-v")) {
            funcs = 0;
            continue;
        }
        if (funcs)
            func_unset(argv[i]);
        else {
            unsetenv(argv[i]);
            func_unset(argv[i]);        /* POSIX: without -f, the variable, else the function */
        }
    }
    return 0;
}

/* set -e and friends (also sh's command line flags); -1 for an unknown letter. */
int set_option(char opt, int on)
{
    switch (opt) {
    case 'e': sh.errexit = on; return 0;
    case 'u': sh.nounset = on; return 0;
    case 'x': sh.xtrace = on; return 0;
    case 'f': sh.noglob = on; return 0;
    case 'n': sh.noexec = on; return 0;
    case 'a': case 'b': case 'C': case 'h': case 'm': case 'v': case 'i': case 's': case 'p':
        return 0;                       /* accepted: nothing to do here */
    }
    return -1;
}

static const struct { const char *name; char opt; } option_names[] = {
    { "errexit", 'e' }, { "nounset", 'u' }, { "xtrace", 'x' }, { "noglob", 'f' }, { "noexec", 'n' },
    { "allexport", 'a' }, { "notify", 'b' }, { "noclobber", 'C' }, { "monitor", 'm' }, { "verbose", 'v' },
    { "ignoreeof", 'i' }, { "vi", 'v' }, { "emacs", 'v' }, { "pipefail", 'p' }, { "posix", 'p' },
};

static void set_positional(int argc, char **argv, int from)
{
    static struct strvec params;
    sv_free(&params);
    for (int i = from; i < argc; i++)
        sv_push(&params, xstrdup(argv[i]));
    sh.posv = sv_terminate(&params);
    sh.posc = params.n;
}

static int bi_set(int argc, char **argv)
{
    if (argc < 2) {
        print_env_sorted("");
        return 0;
    }
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            set_positional(argc, argv, i + 1);
            return 0;
        }
        if (!strcmp(a, "-") || !strcmp(a, "+")) {
            i++;
            break;
        }
        if (a[0] != '-' && a[0] != '+')
            break;
        int on = a[0] == '-';
        if (a[1] == 'o' && !a[2]) {
            if (i + 1 >= argc) {        /* set -o: the options */
                printf("errexit         %s\nnounset         %s\nxtrace          %s\nnoglob          %s\n",
                       sh.errexit ? "on" : "off", sh.nounset ? "on" : "off", sh.xtrace ? "on" : "off",
                       sh.noglob ? "on" : "off");
                continue;
            }
            const char *name = argv[++i];
            int found = 0;
            for (size_t k = 0; k < sizeof(option_names) / sizeof(option_names[0]); k++)
                if (!strcmp(option_names[k].name, name)) {
                    set_option(option_names[k].opt, on);
                    found = 1;
                }
            if (!found) {
                fsh_error("set: no such option: %s", name);
                return 2;
            }
            continue;
        }
        for (const char *o = a + 1; *o; o++)
            if (set_option(*o, on) < 0) {
                fsh_error("set: unsupported option: -%c", *o);
                return 2;
            }
    }
    if (i < argc || (i == argc && argc > 1 && (!strcmp(argv[argc - 1], "-") || !strcmp(argv[argc - 1], "+"))))
        set_positional(argc, argv, i);
    return 0;
}

static int bi_local(int argc, char **argv)
{
    if (!sh.func_depth) {
        fsh_error("local: can only be used in a function");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-')
            continue;
        char *eq = strchr(argv[i], '=');
        size_t nlen = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        char *name = xstrndup(argv[i], nlen);
        if (!is_valid_name(name, nlen)) {
            fsh_error("local: not an identifier: %s", name);
            free(name);
            return 1;
        }
        local_declare(name);
        if (eq)
            setenv(name, eq + 1, 1);
        free(name);
    }
    return 0;
}

static int bi_readonly(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strchr(argv[i], '=') && argv[i][0] != '-')
            assign_env(argv[i]);
    return 0;
}

static int bi_umask(int argc, char **argv)
{
    int i = 1;
    if (i < argc && (!strcmp(argv[i], "-S") || !strcmp(argv[i], "-p")))
        i++;
    if (i >= argc) {
        mode_t m = umask(0);
        umask(m);
        printf("%04o\n", (unsigned)m);
        return 0;
    }
    char *end;
    long m = strtol(argv[i], &end, 8);
    if (*end || m < 0 || m > 0777) {
        fsh_error("umask: invalid mode: %s", argv[i]);
        return 1;
    }
    umask((mode_t)m);
    return 0;
}

static int bi_hash(int argc, char **argv) { return 0; }
static int bi_ulimit(int argc, char **argv)
{
    if (argc < 2 || argv[argc - 1][0] == '-')
        puts("unlimited");
    return 0;
}

/* getopts optstring name [args...] */
static int bi_getopts(int argc, char **argv)
{
    if (argc < 3) {
        fsh_error("getopts: usage: getopts optstring name [arg...]");
        return 2;
    }
    const char *optstring = argv[1], *name = argv[2];
    char **args = argc > 3 ? argv + 3 : sh.posv;
    int nargs = argc > 3 ? argc - 3 : sh.posc;
    int silent = optstring[0] == ':';
    if (silent)
        optstring++;
    const char *oi = getenv("OPTIND");
    int optind = oi ? atoi(oi) : 1;
    static int optpos;                  /* within a bundle like -abc */
    static int last_optind;
    if (optind != last_optind)
        optpos = 0;
    if (optind < 1)
        optind = 1;
    char num[16];
    if (optind > nargs) {
        setenv(name, "?", 1);
        return 1;
    }
    const char *arg = args[optind - 1];
    if (!optpos) {
        if (arg[0] != '-' || !arg[1]) {
            setenv(name, "?", 1);
            return 1;
        }
        if (!strcmp(arg, "--")) {
            snprintf(num, sizeof(num), "%d", optind + 1);
            setenv("OPTIND", num, 1);
            setenv(name, "?", 1);
            return 1;
        }
        optpos = 1;
    }
    char c = arg[optpos];
    const char *spec = c == ':' ? NULL : strchr(optstring, c);
    char cs[2] = { c, 0 };
    if (!spec) {
        setenv(name, "?", 1);
        if (silent)
            setenv("OPTARG", cs, 1);
        else {
            fsh_error("getopts: illegal option -- %c", c);
            unsetenv("OPTARG");
        }
        optpos++;
    } else if (spec[1] == ':') {
        if (arg[optpos + 1]) {
            setenv("OPTARG", arg + optpos + 1, 1);
            optind++;
        } else if (optind < nargs) {
            setenv("OPTARG", args[optind], 1);
            optind += 2;
        } else {
            if (silent) {
                setenv(name, ":", 1);
                setenv("OPTARG", cs, 1);
            } else {
                fsh_error("getopts: option requires an argument -- %c", c);
                setenv(name, "?", 1);
                unsetenv("OPTARG");
            }
            optind++;
            optpos = 0;
            snprintf(num, sizeof(num), "%d", optind);
            setenv("OPTIND", num, 1);
            last_optind = optind;
            return 0;
        }
        setenv(name, cs, 1);
        optpos = 0;
        snprintf(num, sizeof(num), "%d", optind);
        setenv("OPTIND", num, 1);
        last_optind = optind;
        return 0;
    } else {
        setenv(name, cs, 1);
        unsetenv("OPTARG");
        optpos++;
    }
    if (!arg[optpos]) {
        optind++;
        optpos = 0;
    }
    snprintf(num, sizeof(num), "%d", optind);
    setenv("OPTIND", num, 1);
    last_optind = optind;
    return 0;
}

static int bi_shift(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 0 || n > sh.posc) {
        fsh_error("shift: shift count must be <= $#");
        return 1;
    }
    sh.posv += n;
    sh.posc -= n;
    return 0;
}

static int bi_alias(int argc, char **argv)
{
    if (argc < 2) {
        for (int i = 0; i < naliases; i++) {
            printf("%s=", aliases[i].name);
            print_quoted(aliases[i].value);
            putchar('\n');
        }
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            char *name = xstrndup(argv[i], (size_t)(eq - argv[i]));
            alias_set(name, eq + 1);
            free(name);
        } else {
            const char *v = alias_get(argv[i]);
            if (v) {
                printf("%s=", argv[i]);
                print_quoted(v);
                putchar('\n');
            } else {
                status = 1;
            }
        }
    }
    return status;
}

static int bi_unalias(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) {
            while (naliases)
                alias_remove(aliases[0].name);
        } else if (alias_remove(argv[i]) < 0) {
            fsh_error("unalias: no such hash table element: %s", argv[i]);
            status = 1;
        }
    }
    return status;
}

static int bi_read(int argc, char **argv)
{
    int i = 1, raw = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-r")) {
            raw = 1;
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            fputs(argv[++i], stderr);
        } else if (!strcmp(argv[i], "--")) {
            i++;
            break;
        } else {
            break;
        }
    }
    fflush(stdout);

    struct sbuf line;
    sb_init(&line);
    char c;
    int got = 0;
    while (read(0, &c, 1) == 1) {
        got = 1;
        if (c == '\n')
            break;
        if (c == '\\' && !raw) {
            if (read(0, &c, 1) != 1)
                break;
            if (c == '\n')
                continue;
        }
        sb_putc(&line, c);
    }
    char *s = sb_take(&line);

    if (i >= argc) {
        setenv("REPLY", s, 1);
    } else {
        /* fields separated by IFS: blanks in it are trimmed and collapsed, other
         * characters in it each end a field; the last variable gets the rest */
        const char *ifs = getenv("IFS");
        if (!ifs)
            ifs = " \t\n";
        char *p = s;
        for (; i < argc; i++) {
            while (*p && strchr(ifs, *p) && (*p == ' ' || *p == '\t' || *p == '\n'))
                p++;
            if (i == argc - 1) {
                size_t len = strlen(p);
                while (len && strchr(ifs, p[len - 1]) && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\n'))
                    p[--len] = 0;
                setenv(argv[i], p, 1);
                break;
            }
            char *end = p;
            while (*end && !strchr(ifs, *end))
                end++;
            char save = *end;
            *end = 0;
            setenv(argv[i], p, 1);
            if (save) {
                *end = save;
                p = end;
                /* one separator: blanks around a non-blank one are absorbed */
                while (*p && (*p == ' ' || *p == '\t' || *p == '\n') && strchr(ifs, *p))
                    p++;
                if (*p && strchr(ifs, *p) && !(*p == ' ' || *p == '\t' || *p == '\n'))
                    p++;
            } else {
                p = end;
            }
        }
    }
    free(s);
    return got ? 0 : 1;
}

/* ---- control flow ---- */

static int bi_exit(int argc, char **argv)
{
    static int warned;
    if (sh.interactive && jobs_count() && !warned) {
        warned = 1;
        fsh_error("you have running jobs.");
        return 1;
    }
    sh.exiting = 1;
    sh.exit_code = argc > 1 ? atoi(argv[1]) : sh.last_status;
    return sh.exit_code;
}

static int bi_return(int argc, char **argv)
{
    sh.returning = 1;
    sh.last_status = argc > 1 ? atoi(argv[1]) : sh.last_status;
    return sh.last_status;
}

static int bi_break(int argc, char **argv)
{
    sh.break_n = argc > 1 ? atoi(argv[1]) : 1;
    return 0;
}

static int bi_continue(int argc, char **argv)
{
    sh.continue_n = argc > 1 ? atoi(argv[1]) : 1;
    return 0;
}

static int bi_true(int argc, char **argv) { return 0; }
static int bi_false(int argc, char **argv) { return 1; }

static int bi_source(int argc, char **argv)
{
    if (argc < 2) {
        fsh_error("%s: not enough arguments", argv[0]);
        return 1;
    }
    char **old_v = sh.posv;
    int old_c = sh.posc;
    if (argc > 2) {
        sh.posv = argv + 2;
        sh.posc = argc - 2;
    }
    int st = source_file(argv[1], 0);
    sh.posv = old_v;
    sh.posc = old_c;
    if (sh.returning)
        sh.returning = 0;
    return st;
}

static int bi_eval(int argc, char **argv)
{
    struct sbuf b;
    sb_init(&b);
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            sb_putc(&b, ' ');
        sb_puts(&b, argv[i]);
    }
    char *src = sb_take(&b);
    int st = run_string(src);
    free(src);
    return st;
}

/* ---- test / [ ---- */

static int bi_test(int argc, char **argv)
{
    return test_eval(argc, argv);
}

#if 0
static int file_test(char op, const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return 0;
    switch (op) {
    case 'e': case 'r': case 'w': return 1;
    case 'x': return S_ISDIR(st.st_mode) || (st.st_mode & 0111);
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'p': return S_ISFIFO(st.st_mode);
    case 's': return st.st_size > 0;
    }
    return 0;
}

static int test_expr(char **a, int n);

static int test_primary(char **a, int n)
{
    if (n == 0)
        return 0;
    if (!strcmp(a[0], "!"))
        return !test_primary(a + 1, n - 1);
    if (n == 1)
        return a[0][0] != 0;
    if (n == 2) {
        if (a[0][0] == '-' && a[0][1] && !a[0][2]) {
            char op = a[0][1];
            if (op == 'z') return a[1][0] == 0;
            if (op == 'n') return a[1][0] != 0;
            return file_test(op, a[1]);
        }
        return 0;
    }
    const char *op = a[1];
    if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(a[0], a[2]);
    if (!strcmp(op, "!=")) return strcmp(a[0], a[2]) != 0;
    if (!strcmp(op, "<")) return strcmp(a[0], a[2]) < 0;
    if (!strcmp(op, ">")) return strcmp(a[0], a[2]) > 0;
    long x = strtol(a[0], NULL, 10), y = strtol(a[2], NULL, 10);
    if (!strcmp(op, "-eq")) return x == y;
    if (!strcmp(op, "-ne")) return x != y;
    if (!strcmp(op, "-lt")) return x < y;
    if (!strcmp(op, "-le")) return x <= y;
    if (!strcmp(op, "-gt")) return x > y;
    if (!strcmp(op, "-ge")) return x >= y;
    fsh_error("test: unknown condition: %s", op);
    return 0;
}

static int test_expr(char **a, int n)
{
    for (int i = n - 1; i > 0; i--)
        if (!strcmp(a[i], "-o"))
            return test_expr(a, i) || test_expr(a + i + 1, n - i - 1);
    for (int i = n - 1; i > 0; i--)
        if (!strcmp(a[i], "-a"))
            return test_expr(a, i) && test_expr(a + i + 1, n - i - 1);
    return test_primary(a, n);
}

#endif

/* ---- jobs and signals ---- */

static const struct { const char *name; int sig; } signames[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "ILL", SIGILL },
    { "TRAP", SIGTRAP }, { "ABRT", SIGABRT }, { "BUS", SIGBUS }, { "FPE", SIGFPE },
    { "KILL", SIGKILL }, { "USR1", SIGUSR1 }, { "SEGV", SIGSEGV }, { "USR2", SIGUSR2 },
    { "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "CHLD", SIGCHLD },
};

static int parse_signal(const char *s)
{
    if (isdigit((unsigned char)*s))
        return atoi(s);
    if (!strncasecmp(s, "SIG", 3))
        s += 3;
    for (size_t i = 0; i < sizeof(signames) / sizeof(signames[0]); i++)
        if (!strcasecmp(s, signames[i].name))
            return signames[i].sig;
    return -1;
}

static int bi_kill(int argc, char **argv)
{
    int sig = SIGTERM, i = 1;
    if (argc > 1 && !strcmp(argv[1], "-l")) {
        for (size_t k = 0; k < sizeof(signames) / sizeof(signames[0]); k++)
            printf("%s%c", signames[k].name, k + 1 < sizeof(signames) / sizeof(signames[0]) ? ' ' : '\n');
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "-s")) {
        sig = parse_signal(argv[2]);
        i = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        sig = parse_signal(argv[1] + 1);
        i = 2;
    }
    if (sig < 0) {
        fsh_error("kill: unknown signal: %s", argv[i - 1]);
        return 1;
    }
    if (i >= argc) {
        fsh_error("kill: not enough arguments");
        return 1;
    }
    int status = 0;
    for (; i < argc; i++) {
        int pid;
        if (argv[i][0] == '%') {
            struct job *j = job_find(argv[i]);
            if (!j) {
                fsh_error("kill: %s: no such job", argv[i]);
                status = 1;
                continue;
            }
            pid = -job_pid(j);
        } else {
            pid = atoi(argv[i]);
        }
        if (kill(pid, sig) < 0) {
            fsh_error("kill: kill %s failed: %s", argv[i], errmsg(errno));
            status = 1;
        }
    }
    return status;
}

/* trap [action] [signal...]; trap -l; trap -p */
static int bi_trap(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-p")) {
        for (int sig = 0; sig < NSIGS; sig++) {
            const char *a = trap_get(sig);
            if (!a)
                continue;
            printf("trap -- ");
            print_quoted(a);
            if (sig == TRAP_EXIT)
                printf(" EXIT\n");
            else {
                const char *nm = NULL;
                for (size_t k = 0; k < sizeof(signames) / sizeof(signames[0]); k++)
                    if (signames[k].sig == sig)
                        nm = signames[k].name;
                printf(" %s\n", nm ? nm : "");
            }
        }
        return 0;
    }
    if (!strcmp(argv[1], "-l")) {
        char *a[] = { "kill", "-l", NULL };
        return bi_kill(2, a);
    }
    int i = 1;
    const char *action = argv[i++];
    if (!strcmp(action, "-"))
        action = NULL;
    else if (!strcmp(action, "--") && i < argc)
        action = argv[i++];
    /* "trap SIG": only signals given: reset them */
    int first_is_signal = action && (isdigit((unsigned char)action[0]) || parse_signal(action) >= 0 ||
                                     !strcasecmp(action, "EXIT")) && i >= argc;
    if (first_is_signal) {
        i--;
        action = NULL;
    }
    if (i >= argc) {
        fsh_error("trap: usage: trap [action] signal...");
        return 1;
    }
    for (; i < argc; i++) {
        int sig;
        if (!strcasecmp(argv[i], "EXIT") || !strcmp(argv[i], "0"))
            sig = TRAP_EXIT;
        else
            sig = parse_signal(argv[i]);
        if (sig < 0 || sig >= NSIGS) {
            fsh_error("trap: %s: invalid signal specification", argv[i]);
            return 1;
        }
        trap_set(sig, action);
    }
    return 0;
}

static int bi_jobs(int argc, char **argv)
{
    jobs_reap(1);
    jobs_list();
    return 0;
}

static int bi_fg(int argc, char **argv)
{
    struct job *j = job_find(argc > 1 ? argv[1] : NULL);
    if (!j) {
        fsh_error("fg: no current job");
        return 1;
    }
    return job_wait(j, 1);
}

static int bi_bg(int argc, char **argv)
{
    struct job *j = job_find(argc > 1 ? argv[1] : NULL);
    if (!j) {
        fsh_error("bg: no current job");
        return 1;
    }
    return job_continue(j);
}

static int bi_wait(int argc, char **argv)
{
    int status = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            struct job *j = job_find(argv[i]);
            if (j)
                status = job_wait(j, 0);
        }
        return status;
    }
    struct job *j;
    while ((j = job_find(NULL)))
        status = job_wait(j, 0);
    return status;
}

/* ---- history ---- */

static int bi_history(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-c")) {
        history_clear();
        return 0;
    }
    int n = history_count();
    int start = 0;
    if (argc > 1)
        start = n - atoi(argv[1]);
    if (start < 0)
        start = 0;
    for (int i = start; i < n; i++)
        printf("%5d  %s\n", i + 1, history_get(i));
    return 0;
}

/* ---- introspection ---- */

static const char *const reserved[] = { "if", "then", "elif", "else", "fi", "for", "in",
                                        "while", "until", "do", "done", "function", "case", "esac",
                                        "time", "{", "}", "!", NULL };

static int describe(const char *name, int style)
{
    char path[PATH_MAX];
    const char *v;
    for (int i = 0; reserved[i]; i++) {
        if (!strcmp(reserved[i], name)) {
            printf(style ? "%s is a reserved word\n" : "%s: shell reserved word\n", name);
            return 0;
        }
    }
    if ((v = alias_get(name))) {
        printf(style ? "%s is an alias for %s\n" : "%s: aliased to %s\n", name, v);
        return 0;
    }
    if (func_get(name)) {
        printf(style ? "%s is a shell function\n" : "%s () { ... }\n", name);
        return 0;
    }
    if (find_builtin(name) || !strcmp(name, "exec") || !strcmp(name, "command")) {
        printf(style ? "%s is a shell builtin\n" : "%s: shell built-in command\n", name);
        return 0;
    }
    if (find_command(name, path, sizeof(path))) {
        if (style)
            printf("%s is %s\n", name, path);
        else
            printf("%s\n", path);
        return 0;
    }
    printf(style ? "%s not found\n" : "%s not found\n", name);
    return 1;
}

static int bi_which(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++)
        status |= describe(argv[i], 0);
    return status;
}

static int bi_type(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++)
        status |= describe(argv[i], 1);
    return status;
}

static int bi_help(int argc, char **argv)
{
    printf("\033[1mfsh\033[0m - the flux shell (zsh-flavoured)\n\n");
    printf("Builtins:\n  ");
    int col = 2;
    for (int i = 0; builtin_names[i]; i++) {
        int len = (int)strlen(builtin_names[i]) + 1;
        if (col + len > 76) {
            printf("\n  ");
            col = 2;
        }
        printf("%s ", builtin_names[i]);
        col += len;
    }
    printf("\n\nSyntax: pipes |, lists ; && ||, background &, redirections < > >> 2> 2>&1 &>,\n"
           "        if/elif/else/fi, while/until, for x in ...; do ...; done, functions,\n"
           "        $VAR ${VAR:-default} $(cmd) `cmd` $((math)) ~ globs * ? [...]\n"
           "Editing: arrows, ^A ^E ^K ^U ^W ^Y ^L, Up/Down search history by prefix,\n"
           "         Tab completion, Right arrow accepts the grey autosuggestion,\n"
           "         !! !$ !n history expansion, Alt-. inserts the last argument.\n"
           "Prompt:  PROMPT / RPROMPT with zsh escapes (%%n %%m %%~ %%# %%? %%F{color} %%B ...)\n");
    return 0;
}

static const struct {
    const char *name;
    builtin_fn fn;
} builtins[] = {
    { ".", bi_source }, { ":", bi_true }, { "[", bi_test }, { "alias", bi_alias },
    { "break", bi_break }, { "cd", bi_cd }, { "continue", bi_continue },
    { "bg", bi_bg }, { "dirs", bi_dirs }, { "echo", bi_echo }, { "eval", bi_eval }, { "exit", bi_exit },
    { "export", bi_export }, { "false", bi_false }, { "fg", bi_fg }, { "getopts", bi_getopts },
    { "hash", bi_hash }, { "help", bi_help }, { "history", bi_history }, { "jobs", bi_jobs },
    { "kill", bi_kill }, { "local", bi_local }, { "logout", bi_exit }, { "popd", bi_popd },
    { "print", bi_echo }, { "printf", bi_printf }, { "pushd", bi_pushd },
    { "pwd", bi_pwd }, { "read", bi_read }, { "readonly", bi_readonly }, { "return", bi_return },
    { "set", bi_set }, { "shift", bi_shift }, { "source", bi_source }, { "test", bi_test },
    { "trap", bi_trap }, { "true", bi_true }, { "type", bi_type }, { "ulimit", bi_ulimit },
    { "umask", bi_umask }, { "unalias", bi_unalias }, { "unset", bi_unset }, { "wait", bi_wait },
    { "whence", bi_which }, { "which", bi_which },
};

const char *const builtin_names[] = {
    ".", ":", "[", "alias", "bg", "break", "cd", "command", "continue", "dirs", "echo", "eval", "exec",
    "exit", "export", "false", "fg", "getopts", "hash", "help", "history", "jobs", "kill", "local",
    "logout", "popd", "print", "printf", "pushd", "pwd", "read", "readonly", "return", "set", "shift",
    "source", "test", "trap", "true", "type", "ulimit", "umask", "unalias", "unset", "wait", "whence",
    "which", NULL,
};

builtin_fn find_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if (!strcmp(builtins[i].name, name))
            return builtins[i].fn;
    return NULL;
}
