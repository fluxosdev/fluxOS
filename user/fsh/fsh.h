/*
 * fsh - the flux shell. A small zsh-flavoured shell: zsh-style prompts
 * and line editing (syntax highlighting, autosuggestions, menu
 * completion), POSIX-ish grammar (pipelines, lists, if/while/for,
 * functions) and job control.
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
    TK_WORD, TK_NEWLINE, TK_SEMI, TK_AMP, TK_PIPE, TK_AND, TK_OR,
    TK_LPAREN, TK_RPAREN, TK_REDIR, TK_EOF,
};

enum redir_op { RD_IN, RD_OUT, RD_APPEND, RD_DUP, RD_BOTH, RD_BOTH_APPEND };

struct token {
    int type;
    char *text;             /* raw word text (quotes preserved) */
    int start, end;         /* source offsets */
    int op, fd;             /* TK_REDIR */
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

/* ---- syntax tree ---- */
enum node_type {
    N_LIST, N_BG, N_AND, N_OR, N_NOT, N_PIPE, N_CMD,
    N_IF, N_WHILE, N_UNTIL, N_FOR, N_GROUP, N_SUBSHELL, N_FUNC, N_TIME,
};

struct redir {
    int op, fd;
    char *target;
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
    /* N_FOR, N_FUNC */
    char *var;
    char **list;
    int nlist;
    int has_list;
};

#define PARSE_OK         0
#define PARSE_INCOMPLETE 1
#define PARSE_ERROR      (-1)

int parse(const char *src, struct node **out, const char **ctx);
void node_free(struct node *n);

/* ---- expansion ---- */
#define EXP_NOGLOB 0x1
#define EXP_NOSPLIT 0x2

int expand_word(const char *raw, struct strvec *out, int flags);
char *expand_single(const char *raw);           /* no glob, no split */
char *expand_prompt(const char *fmt, int *visible);
long arith_eval(const char *expr, int *err);
int glob_match(const char *pat, const char *s);
int has_glob_chars(const char *s);

/* ---- execution ---- */
int exec_node(struct node *n);
int run_string(const char *src);
int source_file(const char *path, int quiet_missing);
char *capture_output(const char *cmd);
int find_command(const char *name, char *out, size_t size);
int is_valid_name(const char *s, size_t len);

struct job;
void jobs_reap(int notify);
int jobs_count(void);
void jobs_list(void);
struct job *job_find(const char *spec);     /* NULL spec: current job */
int job_wait(struct job *j, int announce);  /* foreground it and wait */
int job_pid(struct job *j);

/* ---- builtins ---- */
typedef int (*builtin_fn)(int argc, char **argv);
builtin_fn find_builtin(const char *name);
extern const char *const builtin_names[];

/* ---- aliases and functions ---- */
const char *alias_get(const char *name);
struct node *func_get(const char *name);
void func_set(const char *name, struct node *body);

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
    int break_n, continue_n;
    int returning;
    int exiting;
    int exit_code;
};

extern struct shell sh;

void fsh_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *errmsg(int err);    /* zsh-style lowercase strerror */
int term_width(void);

#endif
