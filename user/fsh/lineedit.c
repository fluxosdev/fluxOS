/*
 * ZLE-like line editor: emacs key bindings, prefix history search,
 * command syntax highlighting, grey history autosuggestions, zsh-style
 * completion menus and an optional right prompt. The whole line is
 * redrawn on every change, tracking how many terminal rows it spans.
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fsh.h"

#define LE_MAX   1024
#define HIST_MAX 500

/* ---- history ---- */

static char *hist[HIST_MAX];
static int nhist;

int history_count(void) { return nhist; }
const char *history_get(int i) { return i >= 0 && i < nhist ? hist[i] : ""; }

void history_clear(void)
{
    for (int i = 0; i < nhist; i++)
        free(hist[i]);
    nhist = 0;
}

void history_add(const char *line)
{
    if (!*line || line[0] == ' ')
        return;                                 /* HIST_IGNORE_SPACE */
    if (nhist && !strcmp(hist[nhist - 1], line))
        return;                                 /* HIST_IGNORE_DUPS */
    if (nhist == HIST_MAX) {
        free(hist[0]);
        memmove(hist, hist + 1, sizeof(char *) * (HIST_MAX - 1));
        nhist--;
    }
    hist[nhist++] = xstrdup(line);
}

void history_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[n - 1] = 0;
        history_add(line);
    }
    free(line);
    fclose(f);
}

void history_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < nhist; i++)
        fprintf(f, "%s\n", hist[i]);
    fclose(f);
}

static const char *last_word(const char *line)
{
    static char buf[LE_MAX];
    size_t len = strlen(line);
    while (len && line[len - 1] == ' ')
        len--;
    size_t s = len;
    while (s && line[s - 1] != ' ')
        s--;
    snprintf(buf, sizeof(buf), "%.*s", (int)(len - s), line + s);
    return buf;
}

/* csh-style history expansion: !! !$ !n !-n !prefix */
char *history_expand(const char *line, int *changed, int *error)
{
    struct sbuf b;
    sb_init(&b);
    *changed = 0;
    *error = 0;
    int squote = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '\'')
            squote = !squote;
        if (*p == '\\' && p[1] == '!') {
            sb_putc(&b, '!');
            p++;
            continue;
        }
        if (*p != '!' || squote || !p[1] || strchr(" \t=(", p[1])) {
            sb_putc(&b, *p);
            continue;
        }
        const char *ev = NULL;
        const char *q = p + 1;
        if (*q == '!') {
            ev = nhist ? hist[nhist - 1] : NULL;
            q++;
        } else if (*q == '$') {
            ev = nhist ? last_word(hist[nhist - 1]) : NULL;
            q++;
        } else if (isdigit((unsigned char)*q) || (*q == '-' && isdigit((unsigned char)q[1]))) {
            int neg = *q == '-';
            if (neg)
                q++;
            int n = 0;
            while (isdigit((unsigned char)*q))
                n = n * 10 + (*q++ - '0');
            int idx = neg ? nhist - n : n - 1;
            ev = idx >= 0 && idx < nhist ? hist[idx] : NULL;
        } else {
            const char *s = q;
            while (*q && !strchr(" \t;|&", *q))
                q++;
            size_t len = (size_t)(q - s);
            for (int i = nhist - 1; i >= 0 && !ev; i--)
                if (!strncmp(hist[i], s, len))
                    ev = hist[i];
        }
        if (!ev) {
            fsh_error("no such event: %.*s", (int)(q - p - 1), p + 1);
            *error = 1;
            sb_free(&b);
            return NULL;
        }
        sb_puts(&b, ev);
        *changed = 1;
        p = q - 1;
    }
    return sb_take(&b);
}

/* ---- editor state ---- */

struct editor {
    char buf[LE_MAX];
    int len, pos;
    const char *prompt;
    int plen;
    const char *rprompt;
    int rlen;
    int width;
    int cur_row;                    /* cursor row relative to prompt start */
    int hist_idx;
    char saved[LE_MAX];
    char prefix[LE_MAX];
    const char *suggestion;         /* history line extending buf */
    char yank[LE_MAX];
};

static struct sbuf out;

static void emit(const char *s) { sb_puts(&out, s); }

static void emitf(const char *fmt, int v)
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), fmt, v);
    sb_puts(&out, tmp);
}

static void flush_out(void)
{
    if (out.len)
        write(1, out.s, (size_t)out.len);
    out.len = 0;
    if (out.s)
        out.s[0] = 0;
}

/* ---- syntax highlighting ---- */

static const char *const reserved_words[] = { "if", "then", "elif", "else", "fi", "for",
                                              "in", "while", "until", "do", "done",
                                              "function", "time", "{", "}", "!", NULL };

static int is_reserved(const char *w)
{
    for (int i = 0; reserved_words[i]; i++)
        if (!strcmp(w, reserved_words[i]))
            return 1;
    return 0;
}

static int command_exists(const char *w)
{
    char path[PATH_MAX];
    struct stat st;
    if (find_builtin(w) || alias_get(w) || func_get(w) || !strcmp(w, "exec"))
        return 1;
    if (strchr(w, '/'))
        return stat(w, &st) == 0;
    return find_command(w, path, sizeof(path)) || (stat(w, &st) == 0 && S_ISDIR(st.st_mode));
}

static int word_end(const char *b, int i, int len)
{
    char q = 0;
    for (; i < len; i++) {
        char c = b[i];
        if (q) {
            if (c == q)
                q = 0;
            else if (c == '\\' && q == '"' && i + 1 < len)
                i++;
            continue;
        }
        if (c == '\\' && i + 1 < len) {
            i++;
            continue;
        }
        if (c == '\'' || c == '"') {
            q = c;
            continue;
        }
        if (strchr(" \t|&;<>()", c))
            break;
    }
    return i;
}

static void emit_word_plain(const char *w, int n)
{
    char q = 0;
    for (int i = 0; i < n; i++) {
        char c = w[i];
        if (!q && (c == '\'' || c == '"')) {
            q = c;
            emit("\033[33m");
            sb_putc(&out, c);
            continue;
        }
        sb_putc(&out, c);
        if (q && c == q) {
            q = 0;
            emit("\033[39m");
        }
    }
    if (q)
        emit("\033[39m");
}

static void emit_highlighted(const char *b, int len)
{
    int cmdpos = 1;
    for (int i = 0; i < len; ) {
        char c = b[i];
        if (c == ' ' || c == '\t') {
            sb_putc(&out, c);
            i++;
            continue;
        }
        if (c == '#' && cmdpos) {
            emit("\033[90m");
            sb_putn(&out, b + i, len - i);
            emit("\033[39m");
            return;
        }
        if (strchr("|&;()", c)) {
            emit("\033[36m");
            sb_putc(&out, c);
            emit("\033[39m");
            i++;
            cmdpos = 1;
            continue;
        }
        if (c == '<' || c == '>') {
            emit("\033[36m");
            sb_putc(&out, c);
            emit("\033[39m");
            i++;
            continue;
        }
        int e = word_end(b, i, len);
        if (e == i)
            e = i + 1;
        if (cmdpos) {
            char w[LE_MAX];
            snprintf(w, sizeof(w), "%.*s", e - i, b + i);
            const char *eq = strchr(w, '=');
            if (eq && is_valid_name(w, (size_t)(eq - w))) {
                emit_word_plain(b + i, e - i);
            } else if (is_reserved(w)) {
                emit("\033[33;1m");
                sb_putn(&out, b + i, e - i);
                emit("\033[0m");
            } else {
                emit(command_exists(w) ? "\033[32m" : "\033[31m");
                sb_putn(&out, b + i, e - i);
                emit("\033[39m");
                cmdpos = 0;
            }
        } else {
            emit_word_plain(b + i, e - i);
        }
        i = e;
    }
}

/* ---- rendering ---- */

static const char *find_suggestion(struct editor *e)
{
    if (e->len == 0 || e->pos != e->len)
        return NULL;
    for (int i = nhist - 1; i >= 0; i--)
        if ((int)strlen(hist[i]) > e->len && !strncmp(hist[i], e->buf, (size_t)e->len) &&
            !strchr(hist[i], '\n'))
            return hist[i];
    return NULL;
}

static void render(struct editor *e, int final)
{
    int W = e->width;
    if (e->cur_row > 0)
        emitf("\033[%dA", e->cur_row);
    emit("\r\033[J");
    emit(e->prompt);
    emit_highlighted(e->buf, e->len);

    int total = e->plen + e->len;
    e->suggestion = final ? NULL : find_suggestion(e);
    if (e->suggestion) {
        emit("\033[90m");
        emit(e->suggestion + e->len);
        emit("\033[39m");
        total += (int)strlen(e->suggestion) - e->len;
    }
    if (total > 0 && total % W == 0)
        emit(" \r");                            /* force the pending wrap */
    int end_row = total / W;

    if (e->rlen && total + e->rlen + 2 <= W) {
        emitf("\033[%dG", W - e->rlen);
        emit(e->rprompt);
    }

    int target = e->plen + (final ? e->len : e->pos);
    int trow = target / W, tcol = target % W;
    if (end_row > trow)
        emitf("\033[%dA", end_row - trow);
    emit("\r");
    if (tcol)
        emitf("\033[%dC", tcol);
    e->cur_row = trow;
    flush_out();
}

/* ---- editing primitives ---- */

static void insert(struct editor *e, const char *s, int n)
{
    if (e->len + n >= LE_MAX - 1)
        n = LE_MAX - 1 - e->len;
    if (n <= 0)
        return;
    memmove(e->buf + e->pos + n, e->buf + e->pos, (size_t)(e->len - e->pos));
    memcpy(e->buf + e->pos, s, (size_t)n);
    e->len += n;
    e->pos += n;
    e->buf[e->len] = 0;
}

static void delete_range(struct editor *e, int from, int to, int save)
{
    if (from < 0)
        from = 0;
    if (to > e->len)
        to = e->len;
    if (from >= to)
        return;
    if (save)
        snprintf(e->yank, sizeof(e->yank), "%.*s", to - from, e->buf + from);
    memmove(e->buf + from, e->buf + to, (size_t)(e->len - to));
    e->len -= to - from;
    e->buf[e->len] = 0;
    if (e->pos > to)
        e->pos -= to - from;
    else if (e->pos > from)
        e->pos = from;
}

static void set_line(struct editor *e, const char *s)
{
    snprintf(e->buf, sizeof(e->buf), "%s", s);
    e->len = e->pos = (int)strlen(e->buf);
}

static int word_left(struct editor *e)
{
    int p = e->pos;
    while (p > 0 && !isalnum((unsigned char)e->buf[p - 1]))
        p--;
    while (p > 0 && isalnum((unsigned char)e->buf[p - 1]))
        p--;
    return p;
}

static int word_right(struct editor *e)
{
    int p = e->pos;
    while (p < e->len && !isalnum((unsigned char)e->buf[p]))
        p++;
    while (p < e->len && isalnum((unsigned char)e->buf[p]))
        p++;
    return p;
}

static void history_move(struct editor *e, int dir)
{
    if (e->hist_idx == nhist) {
        snprintf(e->saved, sizeof(e->saved), "%s", e->buf);
        snprintf(e->prefix, sizeof(e->prefix), "%s", e->buf);
    }
    size_t plen = strlen(e->prefix);
    for (int i = e->hist_idx + dir; i >= 0 && i <= nhist; i += dir) {
        if (i == nhist) {
            e->hist_idx = nhist;
            set_line(e, e->saved);
            return;
        }
        if (!strncmp(hist[i], e->prefix, plen) && strcmp(hist[i], e->buf) != 0) {
            e->hist_idx = i;
            set_line(e, hist[i]);
            return;
        }
    }
}

/* ---- completion ---- */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void add_candidate(struct strvec *c, const char *prefix, const char *name, int isdir)
{
    if (strncmp(name, prefix, strlen(prefix)) != 0)
        return;
    char *s = xmalloc(strlen(name) + 2);
    strcpy(s, name);
    if (isdir)
        strcat(s, "/");
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->v[i], s)) {
            free(s);
            return;
        }
    sv_push(c, s);
}

static void complete_commands(struct strvec *c, const char *prefix)
{
    for (int i = 0; builtin_names[i]; i++)
        add_candidate(c, prefix, builtin_names[i], 0);
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin";
    char dir[PATH_MAX];
    while (*path) {
        size_t len = strcspn(path, ":");
        snprintf(dir, sizeof(dir), "%.*s", (int)len, path);
        DIR *d = opendir(len ? dir : ".");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)))
                if (de->d_type == DT_REG)
                    add_candidate(c, prefix, de->d_name, 0);
            closedir(d);
        }
        path += len;
        if (*path == ':')
            path++;
    }
}

static void complete_files(struct strvec *c, const char *word, int *base_off)
{
    const char *slash = strrchr(word, '/');
    char dir[PATH_MAX];
    const char *base;
    if (slash) {
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - word + 1), word);
        base = slash + 1;
    } else {
        strcpy(dir, "");
        base = word;
    }
    *base_off = (int)(base - word);

    char real[PATH_MAX];
    if (dir[0] == '~' && (dir[1] == '/' || !dir[1])) {
        const char *home = getenv("HOME");
        snprintf(real, sizeof(real), "%s%s", home ? home : "", dir + 1);
    } else {
        snprintf(real, sizeof(real), "%s", dir[0] ? dir : ".");
    }

    DIR *d = opendir(real);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (de->d_name[0] == '.' && base[0] != '.')
            continue;
        add_candidate(c, base, de->d_name, de->d_type == DT_DIR);
    }
    closedir(d);
}

static void complete_vars(struct strvec *c, const char *prefix)
{
    for (int i = 0; environ && environ[i]; i++) {
        const char *eq = strchr(environ[i], '=');
        if (!eq)
            continue;
        char name[128];
        snprintf(name, sizeof(name), "%.*s", (int)(eq - environ[i]), environ[i]);
        add_candidate(c, prefix, name, 0);
    }
}

static void show_candidates(struct editor *e, struct strvec *c)
{
    int maxw = 0;
    for (int i = 0; i < c->n; i++)
        maxw = (int)strlen(c->v[i]) > maxw ? (int)strlen(c->v[i]) : maxw;
    int colw = maxw + 2;
    int cols = e->width / colw;
    if (cols < 1)
        cols = 1;
    int rows = (c->n + cols - 1) / cols;

    render(e, 1);
    emit("\n");
    for (int r = 0; r < rows; r++) {
        for (int k = 0; k < cols; k++) {
            int i = k * rows + r;                   /* column-major, like zsh */
            if (i >= c->n)
                continue;
            const char *s = c->v[i];
            int isdir = s[strlen(s) - 1] == '/';
            if (isdir)
                emit("\033[1;34m");
            emit(s);
            if (isdir)
                emit("\033[0m");
            if (k < cols - 1 && (k + 1) * rows + r < c->n)
                for (int pad = (int)strlen(s); pad < colw; pad++)
                    sb_putc(&out, ' ');
        }
        emit("\n");
    }
    flush_out();
    e->cur_row = 0;
}

static void complete(struct editor *e, int listing)
{
    int ws = e->pos;
    while (ws > 0 && !strchr(" \t|&;<>()", e->buf[ws - 1]))
        ws--;
    char word[LE_MAX];
    snprintf(word, sizeof(word), "%.*s", e->pos - ws, e->buf + ws);

    int p = ws;
    while (p > 0 && e->buf[p - 1] == ' ')
        p--;
    int cmdpos = p == 0 || strchr("|&;(", e->buf[p - 1]);

    struct strvec c;
    sv_init(&c);
    int base_off = 0, is_var = 0;
    if (word[0] == '$') {
        complete_vars(&c, word + 1);
        base_off = 1;
        is_var = 1;
    } else if (cmdpos && !strchr(word, '/')) {
        complete_commands(&c, word);
    } else {
        complete_files(&c, word, &base_off);
    }
    if (cmdpos && !strchr(word, '/') && !is_var) {
        /* a directory can be "run" too (AUTO_CD) */
        struct strvec dirs;
        sv_init(&dirs);
        complete_files(&dirs, word, &base_off);
        for (int i = 0; i < dirs.n; i++)
            if (dirs.v[i][strlen(dirs.v[i]) - 1] == '/')
                add_candidate(&c, word, dirs.v[i], 0);
        sv_free(&dirs);
    }

    if (c.n == 0) {
        sv_free(&c);
        return;
    }
    qsort(c.v, (size_t)c.n, sizeof(char *), cmp_str);

    const char *base = word + base_off;
    int blen = (int)strlen(base);

    if (c.n == 1) {
        const char *m = c.v[0];
        char add[LE_MAX];
        int k = 0;
        for (const char *q = m + blen; *q && k < LE_MAX - 3; q++) {
            if (*q == ' ')
                add[k++] = '\\';
            add[k++] = *q;
        }
        if (m[strlen(m) - 1] != '/' && !is_var)
            add[k++] = ' ';
        insert(e, add, k);
    } else {
        int lcp = (int)strlen(c.v[0]);
        for (int i = 1; i < c.n; i++) {
            int j = 0;
            while (j < lcp && c.v[i][j] == c.v[0][j])
                j++;
            lcp = j;
        }
        if (lcp > blen && !listing)
            insert(e, c.v[0] + blen, lcp - blen);
        else
            show_candidates(e, &c);
    }
    sv_free(&c);
}

/* ---- input ---- */

static int read_byte(void)
{
    unsigned char c;
    int r = (int)read(0, &c, 1);
    return r == 1 ? c : -1;
}

static int read_escape(void)
{
    int c = read_byte();
    if (c == '[' || c == 'O') {
        int num = 0, d;
        int prefix = c;
        while ((d = read_byte()) >= 0 && isdigit(d))
            num = num * 10 + (d - '0');
        if (d == ';') {
            while ((d = read_byte()) >= 0 && !isalpha(d) && d != '~')
                ;
        }
        if (d == '~') {
            switch (num) {
            case 1: case 7: return 'H';
            case 4: case 8: return 'F';
            case 3: return 'X';                /* delete */
            }
            return 0;
        }
        (void)prefix;
        return d;
    }
    return c < 0 ? 0 : 0x100 | c;              /* Alt+key */
}

int le_readline(const char *prompt, int prompt_len, const char *rprompt, int rprompt_len,
                char **line)
{
    struct editor e;
    memset(&e, 0, sizeof(e));
    e.prompt = prompt;
    e.plen = prompt_len;
    e.rprompt = rprompt ? rprompt : "";
    e.rlen = rprompt ? rprompt_len : 0;
    e.width = term_width();
    e.hist_idx = nhist;
    int tab_count = 0;

    int old_mode = ttymode(0);
    render(&e, 0);

    int result = LE_LINE;
    for (;;) {
        int c = read_byte();
        if (c < 0) {
            result = LE_EOF;
            break;
        }
        int was_tab = c == '\t';
        if (c == 0x1B)
            c = 0x200 | read_escape();

        switch (c) {
        case '\r':
        case '\n':
            render(&e, 1);
            write(1, "\n", 1);
            goto done;
        case 0x03:                              /* ^C */
            render(&e, 1);
            write(1, "\n", 1);
            result = LE_INTERRUPT;
            goto done;
        case 0x04:                              /* ^D */
            if (e.len == 0) {
                write(1, "\n", 1);
                result = LE_EOF;
                goto done;
            }
            delete_range(&e, e.pos, e.pos + 1, 0);
            break;
        case 0x7F:
        case 0x08:
            delete_range(&e, e.pos - 1, e.pos, 0);
            break;
        case 0x01:
        case 0x200 | 'H':
            e.pos = 0;
            break;
        case 0x05:
        case 0x200 | 'F':
            if (e.pos == e.len && e.suggestion)
                set_line(&e, e.suggestion);
            e.pos = e.len;
            break;
        case 0x02:
        case 0x200 | 'D':
            if (e.pos > 0)
                e.pos--;
            break;
        case 0x06:
        case 0x200 | 'C':
            if (e.pos == e.len && e.suggestion)
                set_line(&e, e.suggestion);     /* accept autosuggestion */
            else if (e.pos < e.len)
                e.pos++;
            break;
        case 0x200 | 'X':
            delete_range(&e, e.pos, e.pos + 1, 0);
            break;
        case 0x0B:                              /* ^K */
            delete_range(&e, e.pos, e.len, 1);
            break;
        case 0x15:                              /* ^U: kill-whole-line */
            delete_range(&e, 0, e.len, 1);
            break;
        case 0x17:                              /* ^W */
        case 0x200 | 0x17F: {
            int p = e.pos;
            while (p > 0 && e.buf[p - 1] == ' ')
                p--;
            while (p > 0 && e.buf[p - 1] != ' ')
                p--;
            delete_range(&e, p, e.pos, 1);
            break;
        }
        case 0x19:                              /* ^Y */
            insert(&e, e.yank, (int)strlen(e.yank));
            break;
        case 0x0C:                              /* ^L */
            write(1, "\033[H\033[2J", 7);
            e.cur_row = 0;
            break;
        case 0x10:
        case 0x200 | 'A':
            history_move(&e, -1);
            break;
        case 0x0E:
        case 0x200 | 'B':
            history_move(&e, 1);
            break;
        case '\t':
            complete(&e, tab_count > 0);
            break;
        case 0x200 | 0x162:                     /* Alt-b */
            e.pos = word_left(&e);
            break;
        case 0x200 | 0x166:                     /* Alt-f */
            e.pos = word_right(&e);
            break;
        case 0x200 | 0x164:                     /* Alt-d */
            delete_range(&e, e.pos, word_right(&e), 1);
            break;
        case 0x200 | 0x12E:                     /* Alt-. : insert-last-word */
            if (nhist) {
                const char *w = last_word(hist[nhist - 1]);
                insert(&e, w, (int)strlen(w));
            }
            break;
        default:
            if (c >= 32 && c < 127) {
                char ch = (char)c;
                insert(&e, &ch, 1);
            }
            break;
        }
        if (c != 0x10 && c != 0x0E && c != (0x200 | 'A') && c != (0x200 | 'B'))
            e.hist_idx = nhist;
        tab_count = was_tab ? tab_count + 1 : 0;
        render(&e, 0);
    }
done:
    ttymode(old_mode);
    *line = result == LE_LINE ? xstrdup(e.buf) : NULL;
    return result;
}
