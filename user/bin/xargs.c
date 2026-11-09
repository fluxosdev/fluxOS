/*
 * xargs - run a command with arguments read from standard input
 *
 *   xargs [-0] [-n N] [-L N] [-s SIZE] [-I STR] [-r] [-t] [-p] [command [args...]]
 *
 * Arguments are whitespace-separated (quotes and backslashes as in the
 * shell; -0 takes NUL-separated ones) and appended to the command, as
 * many as fit (-s, 64 KiB, and the kernel's 4096 arguments) or -n says,
 * or those of N lines with -L, running it as often as needed;
 * -I STR runs it once per line with STR replaced; -r runs nothing for
 * empty input; -t echoes each command first. Exit status 123 if a run
 * failed, 127 if the command was not found.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int status, trace, no_run_if_empty, nul;
static size_t max_bytes = 65536;
static int max_args, max_lines;
static int ended_line;                  /* the last argument read ended its line (-L) */

#define MAX_EXEC_ARGS 4096              /* the kernel's limit on argv strings (MAX_ARGS in kernel/core/exec.c) */
static const char *replace;

static char **args;
static int nargs, cap_args;
static size_t bytes;

static void push(char *s)
{
    if (nargs + 1 >= cap_args) {
        cap_args = cap_args ? cap_args * 2 : 64;
        args = realloc(args, sizeof(char *) * (size_t)cap_args);
        if (!args) {
            fprintf(stderr, "xargs: out of memory\n");
            exit(1);
        }
    }
    args[nargs++] = s;
    args[nargs] = NULL;
}

static int run(char **argv)
{
    if (trace) {
        for (int i = 0; argv[i]; i++)
            fprintf(stderr, "%s%s", i ? " " : "", argv[i]);
        fputc('\n', stderr);
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("xargs: fork");
        return 1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "xargs: %s: %s\n", argv[0], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(st)) {
        fprintf(stderr, "xargs: %s terminated by signal %d\n", argv[0], WTERMSIG(st));
        exit(125);
    }
    int code = WEXITSTATUS(st);
    if (code == 127 || code == 126) {
        status = code;
        exit(code);
    }
    if (code == 255) {
        fprintf(stderr, "xargs: %s: exited with status 255; aborting\n", argv[0]);
        exit(124);
    }
    if (code)
        status = 123;
    return code;
}

/* The next argument from stdin (NULL at the end); with -I, a whole line. */
static char *next_arg(void)
{
    static char *buf;
    static size_t cap;
    size_t n = 0;
    int c;
    if (nul || replace) {
        int sep = nul ? 0 : '\n';
        while ((c = getchar()) != EOF && c != sep) {
            if (n + 1 >= cap)
                buf = realloc(buf, cap = cap ? cap * 2 : 256);
            buf[n++] = (char)c;
        }
        if (c == EOF && n == 0)
            return NULL;
        if (replace) {                  /* leading blanks go, as in GNU xargs */
            size_t s = 0;
            while (s < n && isspace((unsigned char)buf[s]))
                s++;
            memmove(buf, buf + s, n - s);
            n -= s;
        }
        if (n + 1 >= cap)
            buf = realloc(buf, cap = cap ? cap * 2 : 256);
        buf[n] = 0;
        return strdup(buf);
    }
    ended_line = 0;
    while ((c = getchar()) != EOF && isspace(c))
        ;
    if (c == EOF)
        return NULL;
    int quote = 0;
    for (; c != EOF; c = getchar()) {
        if (quote) {
            if (c == quote) {
                quote = 0;
                continue;
            }
            if (c == '\n') {
                fprintf(stderr, "xargs: unmatched %s quote\n", quote == '"' ? "double" : "single");
                exit(1);
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
            continue;
        } else if (c == '\\') {
            c = getchar();
            if (c == EOF)
                break;
        } else if (isspace(c)) {
            if (c == '\n')
                ended_line = 1;
            break;
        }
        if (n + 1 >= cap)
            buf = realloc(buf, cap = cap ? cap * 2 : 256);
        buf[n++] = (char)c;
    }
    if (c == EOF)
        ended_line = 1;
    if (n + 1 >= cap)
        buf = realloc(buf, cap = cap ? cap * 2 : 256);
    buf[n] = 0;
    return strdup(buf);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-0") || !strcmp(a, "--null")) { nul = 1; continue; }
        if (!strcmp(a, "-r") || !strcmp(a, "--no-run-if-empty")) { no_run_if_empty = 1; continue; }
        if (!strcmp(a, "-t") || !strcmp(a, "--verbose")) { trace = 1; continue; }
        if (!strcmp(a, "-p")) { trace = 1; continue; }
        if (!strcmp(a, "-x")) continue;
        if (a[1] == 'i') { replace = a[2] ? a + 2 : "{}"; continue; }     /* only an attached string */
        if (a[1] == 'n' || a[1] == 's' || a[1] == 'I' || a[1] == 'L' || a[1] == 'P') {
            const char *v = a[2] ? a + 2 : i + 1 < argc ? argv[++i] : NULL;
            if (!v) {
                fprintf(stderr, "xargs: option requires an argument -- '%c'\n", a[1]);
                return 1;
            }
            if (a[1] == 'n') max_args = atoi(v);
            else if (a[1] == 's') max_bytes = (size_t)atol(v);
            else if (a[1] == 'I') replace = v;
            else if (a[1] == 'L') max_lines = atoi(v);
            continue;
        }
        fprintf(stderr, "xargs: invalid option %s\n", a);
        return 1;
    }
    char *default_cmd[] = { "echo", NULL };
    char **cmd = i < argc ? argv + i : default_cmd;
    int ncmd = 0;
    while (cmd[ncmd])
        ncmd++;
    if (replace) {
        char *line;
        int any = 0;
        while ((line = next_arg())) {
            if (!line[0]) {
                free(line);
                continue;
            }
            any = 1;
            nargs = 0;
            for (int k = 0; k < ncmd; k++) {
                const char *s = cmd[k];
                if (!strstr(s, replace)) {
                    push(cmd[k]);
                    continue;
                }
                /* every occurrence replaced */
                size_t rl = strlen(replace);
                size_t out_cap = strlen(s) + 1;
                for (const char *q = s; (q = strstr(q, replace)); q += rl)
                    out_cap += strlen(line);
                char *out = malloc(out_cap), *o = out;
                for (const char *q = s; *q; ) {
                    if (!strncmp(q, replace, rl)) {
                        o = stpcpy(o, line);
                        q += rl;
                    } else {
                        *o++ = *q++;
                    }
                }
                *o = 0;
                push(out);
            }
            run(args);
            free(line);
        }
        (void)any;
        return status;
    }
    size_t base_bytes = 0;
    for (int k = 0; k < ncmd; k++)
        base_bytes += strlen(cmd[k]) + 1;
    char *arg;
    int any = 0, lines = 0;
    nargs = 0;
    for (int k = 0; k < ncmd; k++)
        push(cmd[k]);
    bytes = base_bytes;
    while ((arg = next_arg())) {
        any = 1;
        size_t need = strlen(arg) + 1;
        if (nargs > ncmd && ((max_args && nargs - ncmd >= max_args) || bytes + need > max_bytes ||
                             nargs >= MAX_EXEC_ARGS)) {
            run(args);
            for (int k = ncmd; k < nargs; k++)
                free(args[k]);
            nargs = ncmd;
            args[nargs] = NULL;
            bytes = base_bytes;
        }
        push(arg);
        bytes += need;
        if (max_lines && ended_line && ++lines >= max_lines) {    /* -L: a run every N input lines */
            run(args);
            for (int k = ncmd; k < nargs; k++)
                free(args[k]);
            nargs = ncmd;
            args[nargs] = NULL;
            bytes = base_bytes;
            lines = 0;
        }
    }
    if (nargs > ncmd || (!any && !no_run_if_empty))
        run(args);
    return status;
}
