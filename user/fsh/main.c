/*
 * fsh entry point: option handling, startup files and the interactive
 * read-eval loop with zsh-style continuation prompts.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fsh.h"

struct shell sh;

#define DEFAULT_PROMPT "%F{cyan}%n@%m%f %F{blue}%B%~%b%f %(?.%F{green}.%F{red})%#%f "

static void history_path(char *buf, size_t size)
{
    const char *home = getenv("HOME");
    snprintf(buf, size, "%s/.fsh_history", home ? home : "");
}

static void set_default(const char *name, const char *value)
{
    if (!getenv(name))
        setenv(name, value, 1);
}

/* History keeps multi-line input on one line where that is equivalent. */
static char *history_form(const char *src)
{
    struct sbuf b;
    sb_init(&b);
    int squote = 0, dquote = 0;
    for (const char *p = src; *p; p++) {
        if (*p == '\'' && !dquote)
            squote = !squote;
        else if (*p == '"' && !squote)
            dquote = !dquote;
        if (*p != '\n' || squote || dquote) {
            sb_putc(&b, *p);
            continue;
        }
        /* trailing keyword/operator? then a space suffices */
        int len = b.len;
        while (len && b.s[len - 1] == ' ')
            len--;
        static const char *const glue[] = { "then", "do", "else", "{", "|", "&&", "||", ";", "in", NULL };
        int space = len == 0;
        for (int i = 0; glue[i] && !space; i++) {
            int gl = (int)strlen(glue[i]);
            if (len >= gl && !strncmp(b.s + len - gl, glue[i], (size_t)gl) &&
                (len == gl || b.s[len - gl - 1] == ' ' || glue[i][0] == '|' || glue[i][0] == '&' || glue[i][0] == ';'))
                space = 1;
        }
        sb_puts(&b, space ? " " : "; ");
    }
    return sb_take(&b);
}

static void interactive_loop(void)
{
    char hpath[PATH_MAX];
    history_path(hpath, sizeof(hpath));
    history_load(hpath);

    for (;;) {
        jobs_reap(1);
        if (sh.exiting)
            break;

        const char *pfmt = getenv("PROMPT");
        if (!pfmt)
            pfmt = getenv("PS1");
        if (!pfmt)
            pfmt = DEFAULT_PROMPT;
        int plen, rlen = 0;
        char *prompt = expand_prompt(pfmt, &plen);
        const char *rfmt = getenv("RPROMPT");
        char *rprompt = rfmt && *rfmt ? expand_prompt(rfmt, &rlen) : NULL;

        char *line;
        int r = le_readline(prompt, plen, rprompt, rlen, &line);
        free(prompt);
        free(rprompt);
        if (r == LE_EOF) {
            char *argv[] = { "exit", NULL };
            find_builtin("exit")(1, argv);
            if (sh.exiting)
                break;
            continue;
        }
        if (r == LE_INTERRUPT) {
            sh.last_status = 130;
            continue;
        }

        struct sbuf pending;
        sb_init(&pending);
        struct node *tree = NULL;
        const char *ctx;
        int pr = PARSE_ERROR;
        int aborted = 0;
        for (;;) {
            int changed, err;
            char *expanded = history_expand(line, &changed, &err);
            free(line);
            if (err) {
                aborted = 1;
                break;
            }
            if (changed)
                printf("%s\n", expanded);
            if (pending.len)
                sb_putc(&pending, '\n');
            sb_puts(&pending, expanded);
            free(expanded);

            pr = parse(pending.s, &tree, &ctx);
            if (pr != PARSE_INCOMPLETE)
                break;

            char ps2[64];
            snprintf(ps2, sizeof(ps2), "%s> ", *ctx ? ctx : "");
            if (le_readline(ps2, (int)strlen(ps2), NULL, 0, &line) != LE_LINE) {
                aborted = 1;
                break;
            }
        }

        if (!aborted && pending.len) {
            char *h = history_form(pending.s);
            history_add(h);
            free(h);
        }
        sb_free(&pending);
        if (aborted || pr != PARSE_OK) {
            if (!aborted)
                sh.last_status = 1;
            continue;
        }

        if (sh.noexec) {
            node_free(tree);
            continue;
        }
        exec_node(tree);
        node_free(tree);
        fflush(stdout);
        fflush(stderr);
    }
    history_save(hpath);
}

static int run_stdin(void)
{
    struct sbuf b;
    sb_init(&b);
    char buf[512];
    int n;
    while ((n = (int)read(0, buf, sizeof(buf))) > 0)
        sb_putn(&b, buf, n);
    char *src = sb_take(&b);
    int st = run_string(src);
    free(src);
    return st;
}

/* The shell's exit: the EXIT trap first. */
static int finish(int status)
{
    fflush(stdout);
    sh.exit_code = sh.exiting ? sh.exit_code : status;
    sh.exiting = 1;
    trap_run_exit();
    fflush(stdout);
    fflush(stderr);
    return sh.exit_code;
}

int main(int argc, char **argv)
{
    sh.argv0 = argv[0];
    int login = argv[0][0] == '-';
    int i = 1;
    const char *command = NULL;
    int read_stdin = 0;

    /* "sh" (or "-sh" at login): the POSIX shell */
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (*base == '-')
        base++;
    if (!strcmp(base, "sh"))
        sh.posix = 1;

    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (!strcmp(a, "--version")) {
            printf("fsh 0.3.1 (fluxOS)%s\n", sh.posix ? ", as sh" : "");
            return 0;
        }
        if (!strcmp(a, "--posix")) {
            sh.posix = 1;
            continue;
        }
        if (!strcmp(a, "--login")) {
            login = 1;
            continue;
        }
        if (a[1] == '-') {
            fsh_error("bad option: %s", a);
            return 2;
        }
        /* bundles like -ec: c takes the command string, o an option name */
        int stop = 0;
        for (const char *o = a + 1; *o && !stop; o++) {
            switch (*o) {
            case 'c':
                if (i + 1 >= argc) {
                    fsh_error("-c: requires an argument");
                    return 2;
                }
                command = argv[++i];
                stop = 1;
                break;
            case 'l': login = 1; break;
            case 's': read_stdin = 1; break;
            case 'i': break;
            case 'o':
                if (i + 1 < argc) {
                    char *setargv[] = { "set", "-o", argv[++i], NULL };
                    find_builtin("set")(3, setargv);
                }
                break;
            default:
                if (set_option(*o, 1) < 0) {
                    fsh_error("bad option: -%c", *o);
                    return 2;
                }
            }
        }
        if (stop) {
            i++;
            break;
        }
    }
    /* "+e" and the like turn options off */
    while (i < argc && argv[i][0] == '+' && argv[i][1]) {
        for (const char *o = argv[i] + 1; *o; o++)
            set_option(*o, 0);
        i++;
    }

    set_default("PATH", "/bin:/usr/bin");
    set_default("SHELL", sh.posix ? "/bin/sh" : "/bin/fsh");
    set_default("HOME", "/");
    set_default("USER", "flux");
    set_default("HOST", "fluxOS");
    {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)))
            setenv("PWD", cwd, 1);
    }

    if (command) {
        if (i < argc)
            sh.argv0 = argv[i++];
        sh.posv = argv + i;
        sh.posc = argc - i;
        int st = run_string(command);
        return finish(st);
    }

    if (i < argc && !read_stdin) {
        /* fsh script [args...] */
        sh.argv0 = argv[i];
        sh.posv = argv + i + 1;
        sh.posc = argc - i - 1;
        int st = source_file(argv[i], 0);
        return finish(st);
    }
    if (read_stdin && i < argc) {
        sh.posv = argv + i;
        sh.posc = argc - i;
    }

    if (!isatty(0) || read_stdin) {
        int st = run_stdin();
        return finish(st);
    }

    sh.interactive = 1;
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);           /* ^Z stops the job in the foreground, not the shell */
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    setpgid(0, 0);
    sh.shell_pgid = getpgrp();
    tcsetpgrp(0, sh.shell_pgid);

    (void)login;
    if (!sh.posix) {
        source_file("/etc/fshrc", 1);
        char rc[PATH_MAX];
        snprintf(rc, sizeof(rc), "%s/.fshrc", getenv("HOME"));
        source_file(rc, 1);
    } else if (getenv("ENV")) {
        source_file(getenv("ENV"), 1);
    }
    sh.exiting = 0;

    interactive_loop();
    return finish(sh.exit_code);
}
