/*
 * Execution of syntax trees: simple commands, pipelines, compound
 * commands, redirections and background jobs (each job gets its own
 * process group so ^C only reaches the foreground). ^Z stops the
 * foreground job: it becomes a suspended job, which fg and bg go on with.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "fsh.h"

#define MAX_JOBS 32
#define MAX_PIPE 32

struct job {
    int id;
    int pid;                    /* its process group (the first process) */
    int pids[MAX_PIPE];         /* the processes still to be waited for */
    int npids;
    int stopped;                /* suspended by ^Z (or a stop signal) */
    char *text;
};

static struct job jobs[MAX_JOBS];

/* ---- functions ---- */

struct func {
    char *name;
    struct node *body;
};

static struct func *funcs;
static int nfuncs;

struct node *func_get(const char *name)
{
    for (int i = 0; i < nfuncs; i++)
        if (!strcmp(funcs[i].name, name))
            return funcs[i].body;
    return NULL;
}

void func_set(const char *name, struct node *body)
{
    for (int i = 0; i < nfuncs; i++) {
        if (!strcmp(funcs[i].name, name)) {
            node_free(funcs[i].body);
            funcs[i].body = body;
            return;
        }
    }
    funcs = xrealloc(funcs, sizeof(struct func) * (size_t)(nfuncs + 1));
    funcs[nfuncs].name = xstrdup(name);
    funcs[nfuncs].body = body;
    nfuncs++;
}

/* ---- command lookup ---- */

static int is_executable(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111);
}

int find_command(const char *name, char *out, size_t size)
{
    if (strchr(name, '/')) {
        strlcpy(out, name, size);
        return is_executable(out);
    }
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin";
    while (*path) {
        size_t len = strcspn(path, ":");
        if (len)
            snprintf(out, size, "%.*s/%s", (int)len, path, name);
        else
            snprintf(out, size, "%s", name);
        if (is_executable(out))
            return 1;
        path += len;
        if (*path == ':')
            path++;
    }
    return 0;
}

/* ---- jobs ---- */

int jobs_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].pid)
            n++;
    return n;
}

/* A job of process group pgid, waiting for n processes. */
static struct job *job_add(int pgid, const int *pids, int n, const char *text)
{
    int id = 1;
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].pid && jobs[i].id >= id)
            id = jobs[i].id + 1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].pid) {
            jobs[i].id = id;
            jobs[i].pid = pgid;
            jobs[i].npids = n < MAX_PIPE ? n : MAX_PIPE;
            memcpy(jobs[i].pids, pids, sizeof(int) * (size_t)jobs[i].npids);
            jobs[i].stopped = 0;
            jobs[i].text = xstrdup(text ? text : "");
            return &jobs[i];
        }
    }
    return NULL;
}

static void job_remove(struct job *j)
{
    free(j->text);
    memset(j, 0, sizeof(*j));
}

static const char *status_word(int st)
{
    if (WIFEXITED(st))
        return WEXITSTATUS(st) ? "exit" : "done";
    return strsignal(WTERMSIG(st));
}

/* Reap finished background jobs; report them zsh-style. */
void jobs_reap(int notify)
{
    int st, pid;
    while ((pid = waitpid(-1, &st, WNOHANG | WUNTRACED)) > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            int k = 0;
            while (k < jobs[i].npids && jobs[i].pids[k] != pid)
                k++;
            if (!jobs[i].pid || k == jobs[i].npids)
                continue;
            if (WIFSTOPPED(st)) {       /* stopped in the background (kill -STOP, or it read the terminal) */
                if (!jobs[i].stopped && notify)
                    fprintf(stderr, "[%d]  + suspended  %s\n", jobs[i].id, jobs[i].text);
                jobs[i].stopped = 1;
                continue;
            }
            jobs[i].pids[k] = jobs[i].pids[--jobs[i].npids];
            if (jobs[i].npids)
                continue;               /* the rest of its pipeline is still going */
            if (notify) {
                if (WIFEXITED(st) && WEXITSTATUS(st))
                    fprintf(stderr, "[%d]  + exit %d     %s\n", jobs[i].id, WEXITSTATUS(st), jobs[i].text);
                else
                    fprintf(stderr, "[%d]  + %-10s %s\n", jobs[i].id, status_word(st), jobs[i].text);
            }
            job_remove(&jobs[i]);
        }
    }
}

int job_pid(struct job *j)
{
    return j->pid;
}

struct job *job_find(const char *spec)
{
    struct job *best = NULL;
    if (!spec || !strcmp(spec, "%%") || !strcmp(spec, "%+") || !strcmp(spec, "%")) {
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].pid && (!best || jobs[i].id > best->id))
                best = &jobs[i];
        return best;
    }
    if (spec[0] == '%')
        spec++;
    int id = atoi(spec);
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].pid && jobs[i].id == id)
            return &jobs[i];
    return NULL;
}

void jobs_list(void)
{
    struct job *cur = job_find(NULL);
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].pid)
            printf("[%d]  %c %-10s %s\n", jobs[i].id, &jobs[i] == cur ? '+' : ' ',
                   jobs[i].stopped ? "suspended" : "running", jobs[i].text);
}

/* ---- waiting and status reporting ---- */

static int decode_status(int st, const char *text)
{
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    int sig = WTERMSIG(st);
    if (sig != SIGINT && sig != SIGPIPE)
        fsh_error("%s  %s", strsignal(sig), text ? text : "");
    return 128 + sig;
}

/* Wait for a foreground job's processes (process group pgid). If one of them stops (^Z), the job
 * becomes a suspended one with the processes not yet done, and its status is 128 + the signal. */
static int wait_pids(int *pids, int n, int pgid, const char *text)
{
    int status = 0;
    for (int i = 0; i < n; i++) {
        int st = 0;
        while (waitpid(pids[i], &st, sh.interactive ? WUNTRACED : 0) < 0 && errno == EINTR)
            ;
        if (WIFSTOPPED(st)) {
            struct job *j = job_add(pgid, pids + i, n - i, text);
            if (j) {
                j->stopped = 1;
                fprintf(stderr, "\n[%d]  + suspended  %s\n", j->id, text ? text : "");
            }
            return 128 + WSTOPSIG(st);
        }
        if (i == n - 1)
            status = decode_status(st, text);
    }
    return status;
}

int job_wait(struct job *j, int announce)
{
    int pgid = j->pid, pids[MAX_PIPE], n = j->npids, stopped = j->stopped;
    memcpy(pids, j->pids, sizeof(int) * (size_t)n);
    char *text = xstrdup(j->text);
    if (announce)
        fprintf(stderr, "[%d]  + %s  %s\n", j->id, stopped ? "continued" : "running  ", text);
    job_remove(j);
    if (sh.interactive)
        tcsetpgrp(0, pgid);
    if (stopped)
        kill(-pgid, SIGCONT);
    int st = wait_pids(pids, n, pgid, text);
    if (sh.interactive)
        tcsetpgrp(0, sh.shell_pgid);
    free(text);
    return st;
}

/* bg: a suspended job goes on, in the background. */
int job_continue(struct job *j)
{
    if (!j->stopped)
        return 0;
    j->stopped = 0;
    fprintf(stderr, "[%d]  + continued  %s\n", j->id, j->text);
    return kill(-j->pid, SIGCONT) < 0;
}

/* ---- redirections ---- */

struct saved_fds {
    int fd[16];
    int copy[16];
    int n;
};

static int save_fd(struct saved_fds *s, int fd)
{
    if (!s)
        return 0;
    for (int i = 0; i < s->n; i++)
        if (s->fd[i] == fd)
            return 0;
    if (s->n >= 16)
        return -1;
    s->fd[s->n] = fd;
    s->copy[s->n] = dup(fd);
    s->n++;
    return 0;
}

static void restore_fds(struct saved_fds *s)
{
    fflush(stdout);
    fflush(stderr);
    for (int i = s->n - 1; i >= 0; i--) {
        if (s->copy[i] >= 0) {
            dup2(s->copy[i], s->fd[i]);
            close(s->copy[i]);
        } else {
            close(s->fd[i]);
        }
    }
    s->n = 0;
}

static int apply_redirs(struct redir *r, int n, struct saved_fds *save)
{
    fflush(stdout);
    fflush(stderr);
    for (int i = 0; i < n; i++) {
        char *target = expand_single(r[i].target);
        if (!target)
            return -1;
        int fd = r[i].fd;

        if (r[i].op == RD_DUP) {
            if (!strcmp(target, "-")) {
                save_fd(save, fd);
                close(fd);
            } else {
                char *end;
                long src = strtol(target, &end, 10);
                if (*end || end == target) {
                    fsh_error("bad file descriptor: %s", target);
                    free(target);
                    return -1;
                }
                save_fd(save, fd);
                if (dup2((int)src, fd) < 0) {
                    fsh_error("%d: %s", (int)src, errmsg(errno));
                    free(target);
                    return -1;
                }
            }
            free(target);
            continue;
        }

        int flags;
        switch (r[i].op) {
        case RD_IN: flags = O_RDONLY; break;
        case RD_APPEND:
        case RD_BOTH_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
        default: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        }
        int nfd = open(target, flags);
        if (nfd < 0) {
            fsh_error("%s: %s", errmsg(errno), target);
            free(target);
            return -1;
        }
        free(target);
        save_fd(save, fd);
        dup2(nfd, fd);
        if (r[i].op == RD_BOTH || r[i].op == RD_BOTH_APPEND) {
            save_fd(save, 2);
            dup2(nfd, 2);
        }
        if (nfd != fd)
            close(nfd);
    }
    return 0;
}

/* ---- child processes ---- */

static void reset_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
}

/* In a fresh child: join the job's process group. */
static void child_jobctl(int pgid, int foreground)
{
    if (sh.interactive) {
        setpgid(0, pgid);
        if (foreground)
            tcsetpgrp(0, pgid ? pgid : getpid());
    }
    sh.interactive = 0;
    sh.in_child = 1;
    reset_signals();
}

static void exec_external(const char *path, char **argv, char **assigns, int nassigns)
{
    for (int i = 0; i < nassigns; i++)
        putenv(assigns[i]);
    execve(path, argv, environ);
    int err = errno;
    if (err == ENOEXEC) {
        /* No #! line: run it as an fsh script. */
        char *nargv[64];
        int k = 0;
        nargv[k++] = "fsh";
        for (int i = 0; argv[i] && k < 63; i++)
            nargv[k++] = i == 0 ? (char *)path : argv[i];
        nargv[k] = NULL;
        execve("/bin/fsh", nargv, environ);
    }
    fsh_error("%s: %s", errmsg(err), argv[0]);
    _exit(err == ENOENT ? 127 : 126);
}

/* ---- simple commands ---- */

static int run_function(struct node *body, struct strvec *args)
{
    char **old_v = sh.posv;
    int old_c = sh.posc;
    sh.posv = args->v + 1;
    sh.posc = args->n - 1;
    int status = exec_node(body);
    if (sh.returning) {
        sh.returning = 0;
        status = sh.last_status;
    }
    sh.posv = old_v;
    sh.posc = old_c;
    return status;
}

static int run_simple(struct node *n, int nofork)
{
    struct strvec args, assigns;
    sv_init(&args);
    sv_init(&assigns);
    int status = 0;

    for (int i = 0; i < n->nwords; i++) {
        if (expand_word(n->words[i], &args, 0) < 0) {
            status = 1;
            goto out;
        }
    }
    for (int i = 0; i < n->nassigns; i++) {
        const char *eq = strchr(n->assigns[i], '=');
        char *val = expand_single(eq + 1);
        if (!val) {
            status = 1;
            goto out;
        }
        char *entry = xmalloc((size_t)(eq - n->assigns[i]) + strlen(val) + 2);
        sprintf(entry, "%.*s=%s", (int)(eq - n->assigns[i]), n->assigns[i], val);
        free(val);
        sv_push(&assigns, entry);
    }
    sv_terminate(&args);

    /* Assignments / redirections only. */
    if (args.n == 0) {
        for (int i = 0; i < assigns.n; i++)
            putenv(assigns.v[i]);
        if (n->nredirs) {
            struct saved_fds save = { .n = 0 };
            status = apply_redirs(n->redirs, n->nredirs, &save) < 0 ? 1 : 0;
            restore_fds(&save);
        }
        goto out;
    }

    const char *name = args.v[0];

    /* "exec": redirections are permanent; optionally replace the shell. */
    if (!strcmp(name, "exec")) {
        if (apply_redirs(n->redirs, n->nredirs, NULL) < 0) {
            status = 1;
            goto out;
        }
        if (args.n > 1) {
            char path[PATH_MAX];
            if (!find_command(args.v[1], path, sizeof(path))) {
                fsh_error("command not found: %s", args.v[1]);
                status = 127;
                goto out;
            }
            reset_signals();
            exec_external(path, args.v + 1, assigns.v, assigns.n);
        }
        goto out;
    }

    struct node *fn = func_get(name);
    builtin_fn bf = fn ? NULL : find_builtin(name);
    if (fn || bf) {
        struct saved_fds save = { .n = 0 };
        if (apply_redirs(n->redirs, n->nredirs, &save) < 0) {
            restore_fds(&save);
            status = 1;
            goto out;
        }
        if (fn) {
            status = run_function(fn, &args);
        } else {
            status = bf(args.n, args.v);
            fflush(stdout);
            fflush(stderr);
        }
        restore_fds(&save);
        goto out;
    }

    char path[PATH_MAX];
    if (!find_command(name, path, sizeof(path))) {
        struct stat st;
        if (args.n == 1 && stat(name, &st) == 0 && S_ISDIR(st.st_mode) && !nofork) {
            /* AUTO_CD: a bare directory name changes into it */
            char *cdargv[] = { "cd", (char *)name, NULL };
            status = find_builtin("cd")(2, cdargv);
            goto out;
        }
        status = 127;
        if (!strchr(name, '/')) {
            fsh_error("command not found: %s", name);
        } else if (stat(name, &st) == 0) {
            fsh_error("permission denied: %s", name);
            status = 126;
        } else {
            fsh_error("no such file or directory: %s", name);
        }
        goto out;
    }

    if (nofork) {
        if (apply_redirs(n->redirs, n->nredirs, NULL) < 0)
            _exit(1);
        exec_external(path, args.v, assigns.v, assigns.n);
    }

    fflush(stdout);
    fflush(stderr);
    int pid = fork();
    if (pid < 0) {
        fsh_error("fork failed: %s", errmsg(errno));
        status = 1;
        goto out;
    }
    if (pid == 0) {
        child_jobctl(0, 1);
        if (apply_redirs(n->redirs, n->nredirs, NULL) < 0)
            _exit(1);
        exec_external(path, args.v, assigns.v, assigns.n);
    }
    if (sh.interactive) {
        setpgid(pid, pid);
        tcsetpgrp(0, pid);
    }
    status = wait_pids(&pid, 1, pid, n->text);
    if (sh.interactive)
        tcsetpgrp(0, sh.shell_pgid);

out:
    if (nofork) {
        fflush(stdout);
        exit(status);
    }
    sv_free(&args);
    sv_free(&assigns);
    return status;
}

/* ---- pipelines and background jobs ---- */

static void run_in_child(struct node *n)
{
    if (n->type == N_CMD)
        run_simple(n, 1);
    int st = exec_node(n);
    fflush(stdout);
    exit(st);
}

static int run_pipeline(struct node *n)
{
    int pids[MAX_PIPE];
    int count = n->nitems < MAX_PIPE ? n->nitems : MAX_PIPE;
    int pgid = 0, prev = -1;

    fflush(stdout);
    fflush(stderr);
    for (int i = 0; i < count; i++) {
        int fds[2] = { -1, -1 };
        if (i < count - 1 && pipe(fds) < 0) {
            fsh_error("pipe: %s", errmsg(errno));
            count = i;
            break;
        }
        int pid = fork();
        if (pid == 0) {
            child_jobctl(pgid, 1);
            if (prev >= 0) {
                dup2(prev, 0);
                close(prev);
            }
            if (fds[1] >= 0) {
                dup2(fds[1], 1);
                close(fds[1]);
                close(fds[0]);
            }
            run_in_child(n->items[i]);
        }
        if (pid > 0 && sh.interactive)
            setpgid(pid, pgid ? pgid : pid);
        if (!pgid)
            pgid = pid;
        if (prev >= 0)
            close(prev);
        if (fds[1] >= 0)
            close(fds[1]);
        prev = fds[0];
        pids[i] = pid;
    }
    if (prev >= 0)
        close(prev);
    if (sh.interactive && pgid > 0)
        tcsetpgrp(0, pgid);
    int status = wait_pids(pids, count, pgid, n->text);
    if (sh.interactive)
        tcsetpgrp(0, sh.shell_pgid);
    return status;
}

static int run_background(struct node *n)
{
    fflush(stdout);
    fflush(stderr);
    int pid = fork();
    if (pid < 0) {
        fsh_error("fork failed: %s", errmsg(errno));
        return 1;
    }
    if (pid == 0) {
        child_jobctl(0, 0);
        run_in_child(n);
    }
    if (sh.interactive)
        setpgid(pid, pid);
    sh.last_bg_pid = pid;
    struct job *j = job_add(pid, &pid, 1, n->text);
    if (sh.interactive && j)
        fprintf(stderr, "[%d] %d\n", j->id, pid);
    return 0;
}

static int run_subshell(struct node *n)
{
    fflush(stdout);
    fflush(stderr);
    int pid = fork();
    if (pid == 0) {
        child_jobctl(0, 1);
        if (apply_redirs(n->redirs, n->nredirs, NULL) < 0)
            _exit(1);
        int st = exec_node(n->body);
        fflush(stdout);
        exit(st);
    }
    if (pid < 0)
        return 1;
    if (sh.interactive) {
        setpgid(pid, pid);
        tcsetpgrp(0, pid);
    }
    int st = wait_pids(&pid, 1, pid, "");
    if (sh.interactive)
        tcsetpgrp(0, sh.shell_pgid);
    return st;
}

/* ---- tree walk ---- */

static int interrupted(void)
{
    return sh.break_n || sh.continue_n || sh.returning || sh.exiting;
}

/* After a loop body: returns 1 if the loop should stop. */
static int loop_control(void)
{
    if (sh.break_n) {
        sh.break_n--;
        return 1;
    }
    if (sh.continue_n) {
        sh.continue_n--;
        return sh.continue_n > 0;
    }
    return sh.returning || sh.exiting;
}

static int exec_compound(struct node *n)
{
    int status = 0;
    switch (n->type) {
    case N_IF:
        if (exec_node(n->left) == 0 && !interrupted())
            status = exec_node(n->body);
        else if (n->alt && !interrupted())
            status = exec_node(n->alt);
        break;
    case N_WHILE:
    case N_UNTIL:
        for (;;) {
            int c = exec_node(n->left);
            if (interrupted() || ((c == 0) != (n->type == N_WHILE)))
                break;
            status = exec_node(n->body);
            if (loop_control())
                break;
        }
        break;
    case N_FOR: {
        struct strvec items;
        sv_init(&items);
        if (n->has_list) {
            for (int i = 0; i < n->nlist; i++)
                if (expand_word(n->list[i], &items, 0) < 0) {
                    sv_free(&items);
                    return 1;
                }
        } else {
            for (int i = 0; i < sh.posc; i++)
                sv_push(&items, xstrdup(sh.posv[i]));
        }
        for (int i = 0; i < items.n; i++) {
            setenv(n->var, items.v[i], 1);
            status = exec_node(n->body);
            if (loop_control())
                break;
        }
        sv_free(&items);
        break;
    }
    case N_GROUP:
        status = exec_node(n->body);
        break;
    }
    return status;
}

int exec_node(struct node *n)
{
    int status = 0;
    if (!n)
        return 0;

    switch (n->type) {
    case N_LIST:
        status = sh.last_status;
        for (int i = 0; i < n->nitems && !interrupted(); i++) {
            status = exec_node(n->items[i]);
            sh.last_status = status;
        }
        return status;
    case N_BG:
        status = run_background(n->left);
        break;
    case N_AND:
        status = exec_node(n->left);
        if (status == 0 && !interrupted())
            status = exec_node(n->right);
        break;
    case N_OR:
        status = exec_node(n->left);
        if (status != 0 && !interrupted())
            status = exec_node(n->right);
        break;
    case N_NOT:
        status = !exec_node(n->left);
        break;
    case N_PIPE:
        status = run_pipeline(n);
        break;
    case N_CMD:
        status = run_simple(n, 0);
        break;
    case N_SUBSHELL:
        status = run_subshell(n);
        break;
    case N_FUNC:
        func_set(n->var, n->body);
        n->body = NULL;                 /* ownership moves to the table */
        status = 0;
        break;
    case N_TIME: {
        unsigned int start = uptime_ms();
        status = exec_node(n->left);
        unsigned int ms = uptime_ms() - start;
        fprintf(stderr, "%s  %u.%03us total\n", n->text, ms / 1000, ms % 1000);
        break;
    }
    default: {
        struct saved_fds save = { .n = 0 };
        if (apply_redirs(n->redirs, n->nredirs, &save) < 0) {
            restore_fds(&save);
            status = 1;
            break;
        }
        status = exec_compound(n);
        restore_fds(&save);
        break;
    }
    }
    sh.last_status = status;
    return status;
}

/* ---- running source text ---- */

int run_string(const char *src)
{
    struct node *tree;
    const char *ctx;
    int r = parse(src, &tree, &ctx);
    if (r == PARSE_INCOMPLETE) {
        fsh_error("parse error: unexpected end of input");
        return sh.last_status = 1;
    }
    if (r == PARSE_ERROR)
        return sh.last_status = 1;
    int st = exec_node(tree);
    node_free(tree);
    return st;
}

int source_file(const char *path, int quiet_missing)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (!quiet_missing)
            fsh_error("%s: %s", errmsg(errno), path);
        return 1;
    }
    struct sbuf b;
    sb_init(&b);
    char buf[512];
    int n;
    while ((n = (int)read(fd, buf, sizeof(buf))) > 0)
        sb_putn(&b, buf, n);
    close(fd);
    char *src = sb_take(&b);
    int st = run_string(src);
    free(src);
    return st;
}

/* $(...): run cmd in a child with stdout on a pipe. */
char *capture_output(const char *cmd)
{
    int fds[2];
    struct sbuf b;
    sb_init(&b);
    if (pipe(fds) < 0)
        return xstrdup("");
    fflush(stdout);
    fflush(stderr);
    int pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        sh.interactive = 0;
        sh.in_child = 1;
        reset_signals();
        int st = run_string(cmd);
        fflush(stdout);
        exit(st);
    }
    close(fds[1]);
    char buf[512];
    int n;
    while ((n = (int)read(fds[0], buf, sizeof(buf))) > 0)
        sb_putn(&b, buf, n);
    close(fds[0]);
    if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
        sh.last_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    return sb_take(&b);
}
