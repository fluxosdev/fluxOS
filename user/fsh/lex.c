/*
 * Tokenizer. Words keep their quoting so that expansion can happen at
 * execution time. An unterminated construct yields LEX_INCOMPLETE and a
 * zsh-style context name ("quote", "dquote", ...) for the PS2 prompt.
 *
 * A here document (<<WORD, <<-WORD) is a redirection token that carries
 * its body: the lines after the one it is on, up to the delimiter.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "fsh.h"

static void push(struct tokvec *tv, int type, const char *src, int start, int end)
{
    if (tv->n + 1 > tv->cap) {
        tv->cap = tv->cap ? tv->cap * 2 : 16;
        tv->t = xrealloc(tv->t, sizeof(struct token) * (size_t)tv->cap);
    }
    struct token *t = &tv->t[tv->n++];
    memset(t, 0, sizeof(*t));
    t->type = type;
    t->start = start;
    t->end = end;
    if (type == TK_WORD)
        t->text = xstrndup(src + start, (size_t)(end - start));
}

void tokvec_free(struct tokvec *tv)
{
    for (int i = 0; i < tv->n; i++)
        free(tv->t[i].text);
    free(tv->t);
    tv->t = NULL;
    tv->n = tv->cap = 0;
}

static int is_meta(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' ||
           c == '(' || c == ')' || c == '\n' || c == ' ' || c == '\t';
}

static int skip_dquote(const char *s, int i);

/* s[i] is '\'' : return index past the closing quote, or -1. */
static int skip_squote(const char *s, int i)
{
    for (i++; s[i]; i++)
        if (s[i] == '\'')
            return i + 1;
    return -1;
}

static int skip_bquote(const char *s, int i)
{
    for (i++; s[i]; i++) {
        if (s[i] == '\\' && s[i + 1])
            i++;
        else if (s[i] == '`')
            return i + 1;
    }
    return -1;
}

/* s[i] is '$', s[i+1] is '(' : return index past the matching ')'. */
int skip_subst(const char *s, int i)
{
    int depth = 1;
    for (i += 2; s[i]; ) {
        char c = s[i];
        if (c == '\\' && s[i + 1]) {
            i += 2;
        } else if (c == '\'') {
            if ((i = skip_squote(s, i)) < 0)
                return -1;
        } else if (c == '"') {
            if ((i = skip_dquote(s, i)) < 0)
                return -1;
        } else if (c == '`') {
            if ((i = skip_bquote(s, i)) < 0)
                return -1;
        } else if (c == '(') {
            depth++;
            i++;
        } else if (c == ')') {
            i++;
            if (--depth == 0)
                return i;
        } else {
            i++;
        }
    }
    return -1;
}

/* s[i] is '$', s[i+1] is '{' : return index past the matching '}' (nested ${...},
 * quotes and substitutions skipped), or -1. Inside double quotes a ' is literal. */
int skip_brace(const char *s, int i, int in_dq)
{
    for (i += 2; s[i]; ) {
        char c = s[i];
        if (c == '\\' && s[i + 1]) {
            i += 2;
        } else if (c == '\'' && !in_dq) {
            if ((i = skip_squote(s, i)) < 0)
                return -1;
        } else if (c == '"') {
            if ((i = skip_dquote(s, i)) < 0)
                return -1;
        } else if (c == '`') {
            if ((i = skip_bquote(s, i)) < 0)
                return -1;
        } else if (c == '$' && s[i + 1] == '(') {
            if ((i = skip_subst(s, i)) < 0)
                return -1;
        } else if (c == '$' && s[i + 1] == '{') {
            if ((i = skip_brace(s, i, in_dq)) < 0)
                return -1;
        } else if (c == '}') {
            return i + 1;
        } else {
            i++;
        }
    }
    return -1;
}

/* s[i] is '"' : return index past the closing quote, or -1. */
static int skip_dquote(const char *s, int i)
{
    for (i++; s[i]; ) {
        char c = s[i];
        if (c == '\\' && s[i + 1]) {
            i += 2;
        } else if (c == '"') {
            return i + 1;
        } else if (c == '$' && s[i + 1] == '(') {
            if ((i = skip_subst(s, i)) < 0)
                return -1;
        } else if (c == '$' && s[i + 1] == '{') {
            if ((i = skip_brace(s, i, 1)) < 0)
                return -1;
        } else if (c == '`') {
            if ((i = skip_bquote(s, i)) < 0)
                return -1;
        } else {
            i++;
        }
    }
    return -1;
}

/* Scan one word starting at *pi. */
static int scan_word(const char *s, int *pi, const char **ctx)
{
    int i = *pi;
    while (s[i] && !is_meta(s[i])) {
        char c = s[i];
        int r = i + 1;
        if (c == '\\') {
            if (!s[i + 1]) {
                *ctx = "";
                return LEX_INCOMPLETE;
            }
            r = i + 2;
        } else if (c == '\'') {
            if ((r = skip_squote(s, i)) < 0) {
                *ctx = "quote";
                return LEX_INCOMPLETE;
            }
        } else if (c == '"') {
            if ((r = skip_dquote(s, i)) < 0) {
                *ctx = "dquote";
                return LEX_INCOMPLETE;
            }
        } else if (c == '`') {
            if ((r = skip_bquote(s, i)) < 0) {
                *ctx = "bquote";
                return LEX_INCOMPLETE;
            }
        } else if (c == '$' && s[i + 1] == '(') {
            if ((r = skip_subst(s, i)) < 0) {
                *ctx = "cmdsubst";
                return LEX_INCOMPLETE;
            }
        } else if (c == '$' && s[i + 1] == '{') {
            if ((r = skip_brace(s, i, 0)) < 0) {
                *ctx = "braceparam";
                return LEX_INCOMPLETE;
            }
        }
        i = r;
    }
    *pi = i;
    return LEX_OK;
}

static void push_redir(struct tokvec *tv, int op, int fd, int start, int end)
{
    push(tv, TK_REDIR, NULL, start, end);
    tv->t[tv->n - 1].op = op;
    tv->t[tv->n - 1].fd = fd;
}

/* Here documents announced on the current line, waiting for their bodies. */
#define MAX_PENDING 16
static struct {
    int tok;                /* the redirection token */
    char *delim;
} pending[MAX_PENDING];
static int npending;

/* "<<WORD" or "<<-WORD" at s[i] (i at the first '<'): the token, with the
 * delimiter remembered; returns the index after the word, or -1 for a missing one. */
static int lex_heredoc(const char *s, int i, int fd, struct tokvec *tv, int start)
{
    int flags = 0;
    i += 2;
    if (s[i] == '-') {
        flags |= HD_STRIP;
        i++;
    }
    while (s[i] == ' ' || s[i] == '\t')
        i++;
    char delim[256];
    int n = 0;
    while (s[i] && !is_meta(s[i])) {
        char c = s[i];
        if (c == '\'' || c == '"') {
            flags |= HD_QUOTED;
            for (i++; s[i] && s[i] != c; i++)
                if (n < 255)
                    delim[n++] = s[i];
            if (s[i])
                i++;
            continue;
        }
        if (c == '\\' && s[i + 1]) {
            flags |= HD_QUOTED;
            i++;
            c = s[i];
        }
        if (n < 255)
            delim[n++] = c;
        i++;
    }
    delim[n] = 0;
    if (!n || npending == MAX_PENDING)
        return -1;
    push_redir(tv, RD_HEREDOC, fd < 0 ? 0 : fd, start, i);
    tv->t[tv->n - 1].flags = flags;
    pending[npending].tok = tv->n - 1;
    pending[npending].delim = xstrdup(delim);
    npending++;
    return i;
}

/* After a newline: the bodies of the here documents announced on the line
 * before. Returns the index past the last delimiter line, or -1 if the input
 * ends first. */
static int read_heredoc_bodies(const char *s, int i, struct tokvec *tv)
{
    for (int k = 0; k < npending; k++) {
        struct token *t = &tv->t[pending[k].tok];
        struct sbuf body;
        sb_init(&body);
        int done = 0;
        while (!done) {
            if (!s[i]) {
                sb_free(&body);
                return -1;
            }
            int line_start = i;
            while (s[i] && s[i] != '\n')
                i++;
            int line_end = i;
            if (s[i])
                i++;
            const char *l = s + line_start;
            int ll = line_end - line_start;
            if (t->flags & HD_STRIP)
                while (ll && *l == '\t') {
                    l++;
                    ll--;
                }
            if (ll == (int)strlen(pending[k].delim) && !memcmp(l, pending[k].delim, (size_t)ll)) {
                done = 1;
                break;
            }
            sb_putn(&body, l, ll);
            sb_putc(&body, '\n');
        }
        free(t->text);
        t->text = sb_take(&body);
        t->end = i;
    }
    for (int k = 0; k < npending; k++)
        free(pending[k].delim);
    npending = 0;
    return i;
}

/* Parse "<", ">", ">>", ">&", "<&", "<>", ">|" at s[i] for file descriptor fd. */
static int lex_redir(const char *s, int i, int fd, struct tokvec *tv, int start)
{
    int op;
    if (s[i] == '<') {
        if (s[i + 1] == '<' && s[i + 2] != '<')
            return lex_heredoc(s, i, fd, tv, start);
        i++;
        if (s[i] == '&') {
            i++;
            op = RD_DUP;
        } else if (s[i] == '>') {
            i++;
            op = RD_RDWR;
        } else {
            op = RD_IN;
        }
        if (fd < 0)
            fd = 0;
    } else {
        i++;
        if (s[i] == '>') {
            i++;
            op = RD_APPEND;
        } else if (s[i] == '&') {
            i++;
            op = RD_DUP;
        } else {
            if (s[i] == '|')
                i++;
            op = RD_OUT;
        }
        if (fd < 0)
            fd = 1;
    }
    push_redir(tv, op, fd, start, i);
    return i;
}

int lex(const char *s, struct tokvec *tv, const char **ctx)
{
    int i = 0;
    *ctx = NULL;
    tv->t = NULL;
    tv->n = tv->cap = 0;
    for (int k = 0; k < npending; k++)
        free(pending[k].delim);
    npending = 0;

    for (;;) {
        while (s[i] == ' ' || s[i] == '\t')
            i++;
        if (s[i] == '\\' && s[i + 1] == '\n') {
            i += 2;
            continue;
        }
        if (s[i] == '#') {
            while (s[i] && s[i] != '\n')
                i++;
            continue;
        }
        if (!s[i]) {
            if (npending) {
                *ctx = "heredoc";
                return LEX_INCOMPLETE;
            }
            push(tv, TK_EOF, s, i, i);
            return LEX_OK;
        }

        int start = i;
        char c = s[i];
        switch (c) {
        case '\n':
            push(tv, TK_NEWLINE, s, i, i + 1);
            i++;
            if (npending) {
                i = read_heredoc_bodies(s, i, tv);
                if (i < 0) {
                    *ctx = "heredoc";
                    return LEX_INCOMPLETE;
                }
            }
            continue;
        case ';':
            if (s[i + 1] == ';') {
                push(tv, TK_DSEMI, s, i, i + 2);
                i += 2;
            } else {
                push(tv, TK_SEMI, s, i, i + 1);
                i++;
            }
            continue;
        case '(':
            push(tv, TK_LPAREN, s, i, i + 1);
            i++;
            continue;
        case ')':
            push(tv, TK_RPAREN, s, i, i + 1);
            i++;
            continue;
        case '|':
            if (s[i + 1] == '|') {
                push(tv, TK_OR, s, i, i + 2);
                i += 2;
            } else {
                push(tv, TK_PIPE, s, i, i + 1);
                i++;
            }
            continue;
        case '&':
            if (s[i + 1] == '&') {
                push(tv, TK_AND, s, i, i + 2);
                i += 2;
            } else if (s[i + 1] == '>') {
                int app = s[i + 2] == '>';
                i += app ? 3 : 2;
                push_redir(tv, app ? RD_BOTH_APPEND : RD_BOTH, 1, start, i);
            } else {
                push(tv, TK_AMP, s, i, i + 1);
                i++;
            }
            continue;
        case '<':
        case '>':
            i = lex_redir(s, i, -1, tv, start);
            if (i < 0) {
                *ctx = "";
                return LEX_INCOMPLETE;
            }
            continue;
        }

        /* "2>file": a digit string glued to a redirection operator */
        if (isdigit((unsigned char)c)) {
            int j = i;
            while (isdigit((unsigned char)s[j]))
                j++;
            if (s[j] == '<' || s[j] == '>') {
                i = lex_redir(s, j, atoi(s + i), tv, start);
                if (i < 0) {
                    *ctx = "";
                    return LEX_INCOMPLETE;
                }
                continue;
            }
        }

        int r = scan_word(s, &i, ctx);
        if (r != LEX_OK)
            return r;
        push(tv, TK_WORD, s, start, i);
    }
}
