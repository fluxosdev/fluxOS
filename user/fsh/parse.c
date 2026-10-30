/*
 * Recursive-descent parser producing the syntax tree:
 *
 *   list     := and_or ((';' | '&' | '\n') and_or)*
 *   and_or   := pipeline (('&&' | '||') pipeline)*
 *   pipeline := ['!'] ['time'] command ('|' command)*
 *   command  := simple | if | while | until | for | case | '{' list '}'
 *             | '(' list ')' | function
 *   case     := 'case' word 'in' (['('] pattern ('|' pattern)* ')' list ';;')* 'esac'
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fsh.h"

struct parser {
    struct tokvec *tv;
    const char *src;
    int pos;
    const char *ctx;
    int incomplete;
    int error;
};

static struct node *parse_list(struct parser *p);
static struct node *parse_command(struct parser *p);

static struct node *node_new(int type)
{
    struct node *n = xmalloc(sizeof(*n));
    memset(n, 0, sizeof(*n));
    n->type = type;
    return n;
}

void node_free(struct node *n)
{
    if (!n)
        return;
    if (n->refs > 0) {
        n->refs--;
        return;
    }
    free(n->text);
    for (int i = 0; i < n->nwords; i++)
        free(n->words[i]);
    free(n->words);
    for (int i = 0; i < n->nassigns; i++)
        free(n->assigns[i]);
    free(n->assigns);
    for (int i = 0; i < n->nredirs; i++)
        free(n->redirs[i].target);
    free(n->redirs);
    for (int i = 0; i < n->nitems; i++)
        node_free(n->items[i]);
    free(n->items);
    for (int i = 0; i < n->nlist; i++)
        free(n->list[i]);
    free(n->list);
    free(n->var);
    node_free(n->left);
    node_free(n->right);
    node_free(n->body);
    node_free(n->alt);
    free(n);
}

static void add_item(struct node *n, struct node *item)
{
    n->items = xrealloc(n->items, sizeof(struct node *) * (size_t)(n->nitems + 1));
    n->items[n->nitems++] = item;
}

static void add_str(char ***v, int *n, const char *s)
{
    *v = xrealloc(*v, sizeof(char *) * (size_t)(*n + 1));
    (*v)[(*n)++] = xstrdup(s);
}

static struct token *peek(struct parser *p)
{
    return &p->tv->t[p->pos];
}

static int is_word(struct parser *p, const char *w)
{
    struct token *t = peek(p);
    return t->type == TK_WORD && !strcmp(t->text, w);
}

static void skip_newlines(struct parser *p)
{
    while (peek(p)->type == TK_NEWLINE)
        p->pos++;
}

static const char *tok_desc(struct token *t)
{
    switch (t->type) {
    case TK_WORD: return t->text;
    case TK_NEWLINE: return "\\n";
    case TK_SEMI: return ";";
    case TK_DSEMI: return ";;";
    case TK_AMP: return "&";
    case TK_PIPE: return "|";
    case TK_AND: return "&&";
    case TK_OR: return "||";
    case TK_LPAREN: return "(";
    case TK_RPAREN: return ")";
    case TK_REDIR: return "redirection";
    }
    return "end of input";
}

/* Expected something else: incomplete at end of input, error otherwise. */
static void fail(struct parser *p, const char *ctx)
{
    if (peek(p)->type == TK_EOF) {
        p->incomplete = 1;
        if (!p->ctx)
            p->ctx = ctx;
    } else if (!p->error) {
        p->error = 1;
        fsh_error("parse error near `%s'", tok_desc(peek(p)));
    }
}

static int expect_word(struct parser *p, const char *w, const char *ctx)
{
    if (is_word(p, w)) {
        p->pos++;
        return 1;
    }
    fail(p, ctx);
    return 0;
}

static char *source_text(struct parser *p, int first_tok, int last_tok)
{
    if (last_tok < first_tok)
        return xstrdup("");
    int s = p->tv->t[first_tok].start;
    int e = p->tv->t[last_tok].end;
    if (e < s)
        e = s;
    return xstrndup(p->src + s, (size_t)(e - s));
}

static int is_terminator(struct parser *p)
{
    static const char *const words[] = { "then", "else", "elif", "fi", "do", "done", "}", "esac", NULL };
    struct token *t = peek(p);
    if (t->type == TK_EOF || t->type == TK_RPAREN || t->type == TK_DSEMI)
        return 1;
    if (t->type == TK_WORD)
        for (int i = 0; words[i]; i++)
            if (!strcmp(t->text, words[i]))
                return 1;
    return 0;
}

static int is_assignment(const char *w)
{
    const char *eq = strchr(w, '=');
    return eq && eq != w && is_valid_name(w, (size_t)(eq - w));
}

static void parse_redirs(struct parser *p, struct node *n)
{
    while (peek(p)->type == TK_REDIR) {
        struct token *r = peek(p);
        p->pos++;
        n->redirs = xrealloc(n->redirs, sizeof(struct redir) * (size_t)(n->nredirs + 1));
        struct redir *rd = &n->redirs[n->nredirs++];
        rd->op = r->op;
        rd->fd = r->fd;
        rd->flags = r->flags;
        if (r->op == RD_HEREDOC) {      /* the body came with the token */
            rd->target = xstrdup(r->text ? r->text : "");
            continue;
        }
        if (peek(p)->type != TK_WORD) {
            rd->target = xstrdup("");
            fail(p, "");
            return;
        }
        rd->target = xstrdup(peek(p)->text);
        p->pos++;
    }
}

static struct node *parse_simple(struct parser *p)
{
    struct node *n = node_new(N_CMD);
    int first = p->pos;

    for (;;) {
        struct token *t = peek(p);
        if (t->type == TK_REDIR) {
            parse_redirs(p, n);
            if (p->error || p->incomplete)
                goto fail;
            continue;
        }
        if (t->type != TK_WORD)
            break;
        if (n->nwords == 0 && is_assignment(t->text))
            add_str(&n->assigns, &n->nassigns, t->text);
        else
            add_str(&n->words, &n->nwords, t->text);
        p->pos++;
    }
    if (!n->nwords && !n->nassigns && !n->nredirs) {
        fail(p, "");
        goto fail;
    }
    n->text = source_text(p, first, p->pos - 1);
    return n;
fail:
    node_free(n);
    return NULL;
}

/* Parse a compound body followed by `word`, leaving the result in *out. */
static int parse_body(struct parser *p, const char *end_word, const char *ctx, struct node **out)
{
    *out = parse_list(p);
    if (!*out)
        return 0;
    return expect_word(p, end_word, ctx);
}

static struct node *parse_if(struct parser *p)
{
    p->pos++;                                   /* "if" or "elif" */
    struct node *n = node_new(N_IF);
    if (!parse_body(p, "then", "if", &n->left))
        goto fail;
    n->body = parse_list(p);
    if (!n->body)
        goto fail;
    if (is_word(p, "elif")) {
        n->alt = parse_if(p);
        if (!n->alt)
            goto fail;
        return n;
    }
    if (is_word(p, "else")) {
        p->pos++;
        if (!parse_body(p, "fi", "else", &n->alt))
            goto fail;
    } else if (!expect_word(p, "fi", "then")) {
        goto fail;
    }
    parse_redirs(p, n);
    return n;
fail:
    node_free(n);
    return NULL;
}

static struct node *parse_while(struct parser *p)
{
    int until = is_word(p, "until");
    p->pos++;
    struct node *n = node_new(until ? N_UNTIL : N_WHILE);
    if (!parse_body(p, "do", until ? "until" : "while", &n->left) ||
        !parse_body(p, "done", "do", &n->body)) {
        node_free(n);
        return NULL;
    }
    parse_redirs(p, n);
    return n;
}

static struct node *parse_for(struct parser *p)
{
    p->pos++;
    struct token *t = peek(p);
    if (t->type != TK_WORD || !is_valid_name(t->text, strlen(t->text))) {
        fail(p, "for");
        return NULL;
    }
    struct node *n = node_new(N_FOR);
    n->var = xstrdup(t->text);
    p->pos++;
    skip_newlines(p);
    if (is_word(p, "in")) {
        p->pos++;
        n->has_list = 1;
        while (peek(p)->type == TK_WORD) {
            add_str(&n->list, &n->nlist, peek(p)->text);
            p->pos++;
        }
    }
    if (peek(p)->type == TK_SEMI || peek(p)->type == TK_NEWLINE)
        p->pos++;
    skip_newlines(p);
    if (!expect_word(p, "do", "for") || !parse_body(p, "done", "do", &n->body)) {
        node_free(n);
        return NULL;
    }
    parse_redirs(p, n);
    return n;
}

/* case word in (pat|pat) list ;; ... esac */
static struct node *parse_case(struct parser *p)
{
    p->pos++;
    struct token *t = peek(p);
    if (t->type != TK_WORD) {
        fail(p, "case");
        return NULL;
    }
    struct node *n = node_new(N_CASE);
    n->var = xstrdup(t->text);
    p->pos++;
    skip_newlines(p);
    if (!expect_word(p, "in", "case"))
        goto fail;
    for (;;) {
        skip_newlines(p);
        if (is_word(p, "esac")) {
            p->pos++;
            break;
        }
        if (peek(p)->type == TK_EOF) {
            fail(p, "case");
            goto fail;
        }
        struct node *item = node_new(N_CASE_ITEM);
        add_item(n, item);
        if (peek(p)->type == TK_LPAREN)
            p->pos++;
        for (;;) {
            if (peek(p)->type != TK_WORD) {
                fail(p, "case");
                goto fail;
            }
            add_str(&item->list, &item->nlist, peek(p)->text);
            p->pos++;
            if (peek(p)->type != TK_PIPE)
                break;
            p->pos++;
        }
        if (peek(p)->type != TK_RPAREN) {
            fail(p, "case");
            goto fail;
        }
        p->pos++;
        item->body = parse_list(p);
        if (!item->body)
            goto fail;
        if (peek(p)->type == TK_DSEMI) {
            p->pos++;
            continue;
        }
        if (is_word(p, "esac")) {
            p->pos++;
            break;
        }
        fail(p, "case");
        goto fail;
    }
    parse_redirs(p, n);
    return n;
fail:
    node_free(n);
    return NULL;
}

static struct node *parse_group(struct parser *p)
{
    p->pos++;
    struct node *n = node_new(N_GROUP);
    if (!parse_body(p, "}", "cursh", &n->body)) {
        node_free(n);
        return NULL;
    }
    parse_redirs(p, n);
    return n;
}

static struct node *parse_subshell(struct parser *p)
{
    p->pos++;
    struct node *n = node_new(N_SUBSHELL);
    n->body = parse_list(p);
    if (!n->body || peek(p)->type != TK_RPAREN) {
        if (n->body)
            fail(p, "subsh");
        node_free(n);
        return NULL;
    }
    p->pos++;
    parse_redirs(p, n);
    return n;
}

/* "function name [()] body" or "name() body" */
static struct node *parse_function(struct parser *p, int keyword)
{
    if (keyword)
        p->pos++;
    struct token *t = peek(p);
    if (t->type != TK_WORD) {
        fail(p, "function");
        return NULL;
    }
    struct node *n = node_new(N_FUNC);
    n->var = xstrdup(t->text);
    p->pos++;
    if (peek(p)->type == TK_LPAREN && p->tv->t[p->pos + 1].type == TK_RPAREN)
        p->pos += 2;
    skip_newlines(p);
    n->body = parse_command(p);
    if (!n->body) {
        if (!p->error && !p->incomplete)
            fail(p, "function");
        node_free(n);
        return NULL;
    }
    return n;
}

static struct node *parse_command(struct parser *p)
{
    struct token *t = peek(p);
    if (t->type == TK_LPAREN)
        return parse_subshell(p);
    if (t->type == TK_WORD) {
        if (!strcmp(t->text, "if"))
            return parse_if(p);
        if (!strcmp(t->text, "while") || !strcmp(t->text, "until"))
            return parse_while(p);
        if (!strcmp(t->text, "for"))
            return parse_for(p);
        if (!strcmp(t->text, "case"))
            return parse_case(p);
        if (!strcmp(t->text, "{"))
            return parse_group(p);
        if (!strcmp(t->text, "function"))
            return parse_function(p, 1);
        if (p->tv->t[p->pos + 1].type == TK_LPAREN &&
            p->tv->t[p->pos + 1].type != TK_EOF &&
            p->tv->t[p->pos + 2].type == TK_RPAREN &&
            is_valid_name(t->text, strlen(t->text)))
            return parse_function(p, 0);
    }
    return parse_simple(p);
}

static struct node *parse_pipeline(struct parser *p)
{
    int first = p->pos;
    int negate = 0, timed = 0;
    if (is_word(p, "!")) {
        negate = 1;
        p->pos++;
    }
    if (is_word(p, "time")) {
        timed = 1;
        p->pos++;
    }

    struct node *cmd = parse_command(p);
    if (!cmd)
        return NULL;
    struct node *pipe = NULL;
    while (peek(p)->type == TK_PIPE) {
        p->pos++;
        skip_newlines(p);
        if (peek(p)->type == TK_EOF) {
            fail(p, "pipe");
            node_free(pipe ? pipe : cmd);
            return NULL;
        }
        if (!pipe) {
            pipe = node_new(N_PIPE);
            add_item(pipe, cmd);
        }
        struct node *next = parse_command(p);
        if (!next) {
            node_free(pipe);
            return NULL;
        }
        add_item(pipe, next);
    }

    struct node *res = pipe ? pipe : cmd;
    if (!res->text)
        res->text = source_text(p, first, p->pos - 1);
    if (timed) {
        struct node *t = node_new(N_TIME);
        t->left = res;
        t->text = xstrdup(res->text);
        res = t;
    }
    if (negate) {
        struct node *n = node_new(N_NOT);
        n->left = res;
        res = n;
    }
    return res;
}

static struct node *parse_and_or(struct parser *p)
{
    int first = p->pos;
    struct node *left = parse_pipeline(p);
    if (!left)
        return NULL;
    while (peek(p)->type == TK_AND || peek(p)->type == TK_OR) {
        int type = peek(p)->type == TK_AND ? N_AND : N_OR;
        p->pos++;
        skip_newlines(p);
        if (peek(p)->type == TK_EOF) {
            fail(p, type == N_AND ? "cmdand" : "cmdor");
            node_free(left);
            return NULL;
        }
        struct node *right = parse_pipeline(p);
        if (!right) {
            node_free(left);
            return NULL;
        }
        struct node *n = node_new(type);
        n->left = left;
        n->right = right;
        left = n;
    }
    if (!left->text)
        left->text = source_text(p, first, p->pos - 1);
    return left;
}

static struct node *parse_list(struct parser *p)
{
    struct node *list = node_new(N_LIST);
    skip_newlines(p);
    while (!is_terminator(p)) {
        struct node *item = parse_and_or(p);
        if (!item) {
            node_free(list);
            return NULL;
        }
        int t = peek(p)->type;
        if (t == TK_AMP) {
            struct node *bg = node_new(N_BG);
            bg->left = item;
            bg->text = xstrdup(item->text);
            item = bg;
            p->pos++;
        } else if (t == TK_SEMI || t == TK_NEWLINE) {
            p->pos++;
        } else if (!is_terminator(p)) {
            fail(p, "");
            node_free(item);
            node_free(list);
            return NULL;
        }
        add_item(list, item);
        skip_newlines(p);
    }
    return list;
}

/* ---- alias expansion on the token stream ---- */

static int in_command_position(struct tokvec *tv, int i)
{
    static const char *const words[] = { "then", "else", "elif", "do", "if", "while",
                                         "until", "{", "!", "time", NULL };
    if (i == 0)
        return 1;
    struct token *prev = &tv->t[i - 1];
    switch (prev->type) {
    case TK_NEWLINE: case TK_SEMI: case TK_DSEMI: case TK_AMP: case TK_PIPE:
    case TK_AND: case TK_OR: case TK_LPAREN:
        return 1;
    case TK_WORD:
        for (int k = 0; words[k]; k++)
            if (!strcmp(prev->text, words[k]))
                return i < 2 || in_command_position(tv, i - 1);
        return 0;
    }
    return 0;
}

static void expand_aliases(struct tokvec *tv)
{
    int guard = 0;
    for (int i = 0; i < tv->n && guard < 64; i++) {
        struct token *t = &tv->t[i];
        if (t->type != TK_WORD || t->noalias || !in_command_position(tv, i))
            continue;
        if (strpbrk(t->text, "'\"\\$`"))
            continue;
        const char *val = alias_get(t->text);
        if (!val)
            continue;

        struct tokvec sub;
        const char *ctx;
        if (lex(val, &sub, &ctx) != LEX_OK) {
            tokvec_free(&sub);
            continue;
        }
        sub.n--;                                /* drop TK_EOF */
        char *name = t->text;
        int start = t->start, end = t->end;

        int newn = tv->n - 1 + sub.n;
        if (newn + 1 > tv->cap) {
            tv->cap = newn + 1;
            tv->t = xrealloc(tv->t, sizeof(struct token) * (size_t)tv->cap);
        }
        memmove(&tv->t[i + sub.n], &tv->t[i + 1], sizeof(struct token) * (size_t)(tv->n - i - 1));
        for (int k = 0; k < sub.n; k++) {
            tv->t[i + k] = sub.t[k];
            tv->t[i + k].start = start;
            tv->t[i + k].end = end;
            if (tv->t[i + k].type == TK_WORD && !strcmp(tv->t[i + k].text, name))
                tv->t[i + k].noalias = 1;
        }
        tv->n = newn;
        free(sub.t);
        free(name);
        guard++;
        i--;                                    /* re-examine (chained aliases) */
    }
}

int parse(const char *src, struct node **out, const char **ctx)
{
    struct tokvec tv;
    *out = NULL;
    int r = lex(src, &tv, ctx);
    if (r != LEX_OK) {
        tokvec_free(&tv);
        return r == LEX_INCOMPLETE ? PARSE_INCOMPLETE : PARSE_ERROR;
    }
    expand_aliases(&tv);

    struct parser p = { &tv, src, 0, NULL, 0, 0 };
    struct node *list = parse_list(&p);
    if (list && peek(&p)->type != TK_EOF) {
        fail(&p, "");
        node_free(list);
        list = NULL;
    }
    tokvec_free(&tv);

    if (p.error) {
        node_free(list);
        return PARSE_ERROR;
    }
    if (p.incomplete || !list) {
        node_free(list);
        *ctx = p.ctx ? p.ctx : "";
        return PARSE_INCOMPLETE;
    }
    *out = list;
    return PARSE_OK;
}
