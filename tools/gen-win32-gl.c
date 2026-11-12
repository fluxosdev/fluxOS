/*
 * gen-win32-gl GL/gl.h win32-gl.inc
 *
 * Writes the wrappers the Windows program loader's opengl32.dll calls
 * fluxOS's GL through (user/win32/opengl32.c includes the file): for every
 * function gl.h declares, one that takes its arguments from the words a
 * Windows program pushed (a pointer is a guest address, a double two words,
 * a float one), and the table of them with the words each takes.
 *
 * A program built for the machine the build runs on, like mkiso: so that
 * fluxOS, which has a C compiler and no Python, can build itself.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *what, const char *name)
{
    fprintf(stderr, "gen-win32-gl: %s: %s\n", name, what);
    exit(1);
}

static int word(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* What may stand before a function's name in its declaration. */
static int type_char(int c)
{
    return word(c) || isspace((unsigned char)c) || c == '*';
}

/* A copy of text[from..to) without the white space around it. */
static char *trimmed(const char *text, size_t from, size_t to)
{
    while (from < to && isspace((unsigned char)text[from]))
        from++;
    while (to > from && isspace((unsigned char)text[to - 1]))
        to--;
    char *s = malloc(to - from + 1);
    if (!s)
        die("out of memory", "malloc");
    memcpy(s, text + from, to - from);
    s[to - from] = 0;
    return s;
}

static int one_of(const char *s, const char *const *list)
{
    for (; *list; list++)
        if (!strcmp(s, *list))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: gen-win32-gl GL/gl.h win32-gl.inc\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in)
        die("cannot read it", argv[1]);
    size_t len = 0, cap = 1 << 16;
    char *text = malloc(cap);
    for (size_t n; text && (n = fread(text + len, 1, cap - len - 1, in)) > 0;) {
        len += n;
        if (len + 1 == cap)
            text = realloc(text, cap *= 2);
    }
    if (!text || ferror(in))
        die("cannot read it", argv[1]);
    fclose(in);

    /* Without its comments. */
    size_t out_len = 0;
    for (size_t i = 0; i < len;) {
        if (text[i] == '/' && i + 1 < len && text[i + 1] == '*') {
            size_t end = i + 2;
            while (end + 1 < len && !(text[end] == '*' && text[end + 1] == '/'))
                end++;
            if (end + 1 < len) {
                i = end + 2;
                continue;
            }
        }
        text[out_len++] = text[i++];
    }
    len = out_len;
    text[len] = 0;

    FILE *out = fopen(argv[2], "wb");
    if (!out)
        die("cannot write it", argv[2]);
    fputs("/* Generated from GL/gl.h by tools/gen-win32-gl.c. */\n", out);

    static const char *const skipped[] = { "glGetString", "glGetPointerv", NULL };
    static const char *const results[] = { "void", "GLboolean", "GLenum", "GLuint", "GLint", NULL };
    static const char *const doubles[] = { "GLdouble", "GLclampd", NULL };
    static const char *const floats[] = { "GLfloat", "GLclampf", NULL };
    char *entries = NULL;
    size_t entries_len = 0, count = 0;

    /* Every "type glName(arguments);": the name is a word that starts with
     * gl, after white space or a *, and what stands before it back to the
     * last character no type is written with is the type. */
    size_t pos = 0;
    for (size_t n = 1; n + 2 < len; n++) {
        if (text[n] != 'g' || text[n + 1] != 'l' || !word(text[n + 2]) || n <= pos ||
            !(isspace((unsigned char)text[n - 1]) || text[n - 1] == '*'))
            continue;
        size_t name_end = n + 2;
        while (word(text[name_end]))
            name_end++;
        size_t open = name_end;
        while (isspace((unsigned char)text[open]))
            open++;
        if (text[open] != '(')
            continue;
        /* The arguments end at the first ) that a ; follows, and hold no ; themselves. */
        size_t close = 0, semi = 0;
        for (size_t q = open + 1; q < len && text[q] != ';'; q++) {
            if (text[q] != ')')
                continue;
            size_t after = q + 1;
            while (isspace((unsigned char)text[after]))
                after++;
            if (text[after] == ';') {
                close = q;
                semi = after;
                break;
            }
        }
        if (!close)
            continue;
        size_t start = n;
        while (start > pos && type_char(text[start - 1]))
            start--;
        pos = semi + 1;

        char *name = trimmed(text, n, name_end);
        char *before = trimmed(text, start, n);
        n = semi;
        if (!strncmp(before, "typedef", 7) || one_of(name, skipped)) {
            free(before);
            free(name);
            continue;
        }
        /* (The type is on the name's own line.) */
        const char *line = strrchr(before, '\n');
        char *result = trimmed(line ? line + 1 : before, 0, strlen(line ? line + 1 : before));
        if (!one_of(result, results)) {
            fprintf(stderr, "gen-win32-gl: %s: a result of type '%s'\n", name, result);
            return 1;
        }

        fprintf(out, "DEF(%s) {(void)a;if(!gl_current()) return 0;%s%s(", name,
                strcmp(result, "void") ? "return " : "", name);
        unsigned words = 0;
        char *args = trimmed(text, open + 1, close);
        if (*args && strcmp(args, "void")) {
            int first = 1;
            for (char *arg = args, *next; arg; arg = next) {
                next = strchr(arg, ',');
                size_t arg_len = next ? (size_t)(next - arg) : strlen(arg);
                if (next)
                    next++;
                /* The type: the argument without its name, the word it ends with. */
                char *whole = trimmed(arg, 0, arg_len);
                size_t type_len = strlen(whole);
                while (type_len && word(whole[type_len - 1]))
                    type_len--;
                char *type = trimmed(whole, 0, type_len);
                if (!first)
                    fputs(", ", out);
                first = 0;
                if (strchr(type, '*')) {
                    fprintf(out, "(%s)gp(a[%u])", type, words);
                    words += 1;
                } else if (one_of(type, doubles)) {
                    fprintf(out, "arg_double(a+%u)", words);
                    words += 2;
                } else if (one_of(type, floats)) {
                    fprintf(out, "arg_float(a[%u])", words);
                    words += 1;
                } else {
                    fprintf(out, "(%s)a[%u]", type, words);
                    words += 1;
                }
                free(type);
                free(whole);
            }
        }
        fputs(strcmp(result, "void") ? ");}\n" : ");return 0;}\n", out);

        char entry[256];
        int entry_len = snprintf(entry, sizeof(entry), "%sAPI(%s,%u),", count ? " \\\n    " : "", name, words);
        if (entry_len < 0 || (size_t)entry_len >= sizeof(entry))
            die("too long a name", name);
        entries = realloc(entries, entries_len + (size_t)entry_len + 1);
        if (!entries)
            die("out of memory", "realloc");
        memcpy(entries + entries_len, entry, (size_t)entry_len + 1);
        entries_len += (size_t)entry_len;
        count++;
        free(args);
        free(result);
        free(before);
        free(name);
    }
    fprintf(out, "#define WIN_GL_ENTRIES \\\n    %s\n", entries ? entries : "");
    if (fclose(out) != 0)
        die("cannot write it", argv[2]);
    free(entries);
    free(text);
    printf("Generated %zu GL wrappers\n", count);
    return 0;
}
