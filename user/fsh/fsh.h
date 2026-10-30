/*
 * fsh - the flux shell. A small zsh-flavoured shell: zsh-style prompts
 * and line editing (syntax highlighting, autosuggestions, menu
 * completion), POSIX-ish grammar (pipelines, lists, if/while/for/case,
 * functions, here documents) and job control.
 *
 * Started as "sh" (or with --posix) it is /bin/sh: the POSIX shell that
 * make's recipes and configure scripts run under. The differences from
 * fsh proper: unquoted $var and $(...) are split on IFS, patterns that
 * match nothing stay as they are, no brace expansion, no AUTO_CD, set
 * -e/-u/-x, trap, getopts, local, and "$@" with no parameters is no
 * word at all.
 */
#ifndef FSH_H
#define FSH_H

#include <stddef.h>

/* ---- growable string vector ---- */
struct strvec {
    char **v;
    int n, cap;
};

void sv_init(struct strvec *sv);
void sv_push(struct strvec *sv, char *s);       /* takes ownership */
void sv_free(struct strvec *sv);
char **sv_terminate(struct strvec *sv);         /* NULL-terminates, returns v */

/* ---- string builder ---- */
struct sbuf {
    char *s;
    int len, cap;
};

void sb_init(struct sbuf *b);
void sb_putc(struct sbuf *b, char c);
void sb_puts(struct sbuf *b, const char *s);
void sb_putn(struct sbuf *b, const char *s, int n);
void sb_printf(struct sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *sb_take(struct sbuf *b);                  /* returns malloc'd string, resets */
void sb_free(struct sbuf *b);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---- lexer ---- */
enum tok_type {
    TK_WORD, TK_NEWLINE, TK_SEMI, TK_DSEMI, TK_AMP, TK_PIPE, TK_AND, TK_OR,
    TK_LPAREN, TK_RPAREN, TK_REDIR, TK_EOF,
};

enum redir_op { RD_IN, RD_OUT, RD_APPEND, RD_DUP, RD_BOTH, RD_BOTH_APPEND, RD_HEREDOC, RD_RDWR };

/* RD_HEREDOC flags */
#define HD_QUOTED 1             /* the delimiter was quoted: the body is taken as is */
#define HD_STRIP  2             /* <<-: leading tabs removed */

struct token {
    int type;
    char *text;             /* raw word text (quotes preserved); a here document's body */
    int start, end;         /* source offsets */
    int op, fd, flags;      /* TK_REDIR */
    int noalias;
};

struct tokvec {
    struct token *t;
    int n, cap;
};

#define LEX_OK         0
#define LEX_INCOMPLETE 1
#define LEX_ERROR      (-1)

int lex(const char *src, struct tokvec *out, const char **ctx);
void tokvec_free(struct tokvec *tv);
int skip_subst(const char *s, int i);           /* i at "$(" -> index past ")" or -1 */
int skip_brace(const char *s, int i, int in_dq); /* i at "${" -> index past the matching "}" or -1 */

/* ---- syntax tree ---- */
enum node_type {
    N_LIST, N_BG, N_AND, N_OR, N_NOT, N_PIPE, N_CMD,
    N_IF, N_WHILE, N_UNTIL, N_FOR, N_GROUP, N_SUBSHELL, N_FUNC, N_TIME,
    N_CASE, N_CASE_ITEM,
};

struct redir {
    int op, fd, flags;
    char *target;           /* the file, or a here document's body */
};

struct node {
    int type;
    char *text;                     /* source text, for job messages */
    /* N_CMD */
    char **words;
    int nwords;
    char **assigns;
    int nassigns;
    /* any command */
    struct redir *redirs;
    int nredirs;
    /* N_LIST, N_PIPE */
    struct node **items;
    int nitems;
    /* binary nodes, conditions */
    struct node *left, *right;
    struct node *body, *alt;
    /* N_FOR, N_FUNC, N_CASE (the word); N_CASE_ITEM: list holds the patterns */
    char *var;
    char **list;
    int nlist;
    int has_list;
    int refs;                       /* owners besides the first (a function body: the table, calls running it) */
};

#define PARSE_OK         0
#define PARSE_INCOMPLETE 1
#define PARSE_ERROR      (-1)

int parse(const char *src, struct node **out, const char **ctx);
void node_free(struct node *n);                 /* with refs, drops one owner */

/* ---- expansion ---- */
#define EXP_NOGLOB 0x1
#define EXP_NOSPLIT 0x2
#define EXP_KEEPGLOB 0x4        /* keep the pattern (for case and ${var#pat}) instead of matching files */
#define EXP_HEREDOC 0x8         /* a here document's body: quotes are literal */

int expand_word(const char *raw, struct strvec *out, int flags);
char *expand_single(const char *raw);           /* no glob, no split */
char *expand_heredoc(const char *body);
char *expand_prompt(const char *fmt, int *visible);
long arith_eval(const char *expr, int *err);
int glob_match(const char *pat, const char *s);
int has_glob_chars(const char *s);
int pattern_match(const char *raw_pattern, const char *s);     /* a case pattern (raw word) against s */

/* ---- execution ---- */
int exec_node(struct node *n);
int run_string(const char *src);
int source_file(const char *path, int quiet_missing);
char *capture_output(const char *cmd);
int find_command(const char *name, char *out, size_t size);
int is_valid_name(const char *s, size_t len);
void assign_env(const char *entry);             /* NAME=value, copied */

struct job;
void jobs_reap(int notify);
int jobs_count(void);
void jobs_list(void);
struct job *job_find(const char *spec);     /* NULL spec: current job */
struct job *job_find_pid(int pid);          /* the job a process belongs to */
int job_wait(struct job *j, int announce);  /* foreground it and wait */
int job_pid(struct job *j);
int job_continue(struct job *j);                  /* bg: a suspended job goes on */

/* ---- builtins ---- */
typedef int (*builtin_fn)(int argc, char **argv);
builtin_fn find_builtin(const char *name);
extern const char *const builtin_names[];
int test_eval(int argc, char **argv);           /* testexpr.c */
int set_option(char opt, int on);               /* set -e and friends; -1 for an unknown one */
void positional_free(char **v);                 /* a vector set_positional made (NULL is fine) */

/* ---- traps (sh mode) ---- */
#define TRAP_EXIT 0
void trap_set(int sig, const char *action);     /* NULL: the default */
const char *trap_get(int sig);
void trap_run_pending(void);
void trap_run_exit(void);
void traps_reset_for_child(void);

/* ---- local variables ---- */
void local_declare(const char *name);           /* saved until the function returns */
int locals_mark(void);
void locals_restore(int mark);

/* ---- aliases and functions ---- */
const char *alias_get(const char *name);
struct node *func_get(const char *name);
void func_set(const char *name, struct node *body);
void func_unset(const char *name);

/* ---- history and the line editor ---- */
#define LE_LINE      0
#define LE_EOF       1
#define LE_INTERRUPT 2

int le_readline(const char *prompt, int prompt_len, const char *rprompt, int rprompt_len,
                char **line);
void history_add(const char *line);
int history_count(void);
const char *history_get(int i);
void history_clear(void);
void history_load(const char *path);
void history_save(const char *path);
char *history_expand(const char *line, int *changed, int *error);

/* ---- shell state ---- */
struct shell {
    int interactive;
    int in_child;           /* forked subshell: no job control */
    int last_status;
    int shell_pgid;
    int last_bg_pid;
    char *argv0;
    char **posv;            /* positional parameters $1.. */
    int posc;
    char **posv_alloc;      /* set's vector for this function call or dot script (freed when it ends), or NULL */
    int break_n, continue_n;
    int returning;
    int exiting;
    int exit_code;
    /* sh mode and its options */
    int posix;              /* started as sh: POSIX behaviour where the two differ */
    int errexit;            /* set -e */
    int nounset;            /* set -u */
    int xtrace;             /* set -x */
    int noglob;             /* set -f */
    int noexec;             /* set -n: parse only */
    int in_condition;       /* inside an if/while test, ! or the left side of && ||: -e is off */
    int func_depth;
};

extern struct shell sh;

void fsh_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *errmsg(int err);    /* zsh-style lowercase strerror */
int term_width(void);

#endif
