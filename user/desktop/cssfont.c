/* @font-face discovery. Delimiters in comments, strings and functions stay opaque. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

static const char *skip(const char *p, const char *end)
{
    for (;;) {
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (end - p >= 2 && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (end - p >= 2 && !(p[0] == '*' && p[1] == '/'))
                p++;
            p = end - p >= 2 ? p + 2 : end;
        } else
            return p;
    }
}
static const char *delimiter(const char *p, const char *end, const char *want)
{
    int depth = 0;
    char quote = 0;
    for (; p < end; p++) {
        if (quote) {
            if (*p == '\\' && p + 1 < end)
                p++;
            else if (*p == quote)
                quote = 0;
            continue;
        }
        if (*p == '\'' || *p == '"') {
            quote = *p;
            continue;
        }
        if (end - p >= 2 && p[0] == '/' && p[1] == '*') {
            p = skip(p, end);
            if (p == end)
                return end;
            p--;
            continue;
        }
        if (!depth && strchr(want, *p))
            return p;
        if (*p == '(' || *p == '[')
            depth++;
        else if ((*p == ')' || *p == ']') && depth)
            depth--;
    }
    return end;
}
static void value(char *out, size_t cap, const char *p, const char *end)
{
    p = skip(p, end);
    while (end > p && isspace((unsigned char)end[-1]))
        end--;
    if (end - p >= 2 && (*p == '"' || *p == '\'') && end[-1] == *p) {
        p++;
        end--;
    }
    size_t n = (size_t)(end - p);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}
static void face(const char *p, const char *end, void (*found)(void *, const struct css_fontface *),
                 void *ctx)
{
    struct css_fontface f = {0};
    while ((p = skip(p, end)) < end) {
        const char *colon = delimiter(p, end, ":;");
        if (colon == end)
            break;
        if (*colon != ':') {
            p = colon + 1;
            continue;
        }
        char key[40];
        value(key, sizeof(key), p, colon);
        const char *v = colon + 1, *stop = delimiter(v, end, ";");
        char text[256];
        value(text, sizeof(text), v, stop);
        if (!strcasecmp(key, "font-family"))
            value(f.family, sizeof(f.family), v, stop);
        else if (!strcasecmp(key, "font-weight"))
            f.bold = !strcasecmp(text, "bold") || atoi(text) >= 600;
        else if (!strcasecmp(key, "font-style"))
            f.italic = !strncasecmp(text, "italic", 6) || !strncasecmp(text, "oblique", 7);
        else if (!strcasecmp(key, "src")) {
            const char *s = v;
            while ((s = skip(s, stop)) < stop && f.nsources < 8) {
                const char *part = delimiter(s, stop, ",");
                if (part - s >= 4 && !strncasecmp(s, "url(", 4)) {
                    const char *u = skip(s + 4, part), *last = u;
                    char q = 0;
                    if (u < part && (*u == '\'' || *u == '"')) {
                        q = *u++;
                        last = u;
                        while (last < part && *last != q) {
                            if (*last == '\\' && last + 1 < part)
                                last++;
                            last++;
                        }
                    } else {
                        last = u;
                        while (last < part && *last != ')')
                            last++;
                    }
                    const char *close = q ? (last < part ? skip(last + 1, part) : part) : last;
                    if (close < part && *close == ')') {
                        value(f.sources[f.nsources], sizeof(f.sources[0]), u, last);
                        if (f.sources[f.nsources][0])
                            f.nsources++;
                    }
                }
                s = part < stop ? part + 1 : stop;
            }
        }
        p = stop < end ? stop + 1 : end;
    }
    if (f.family[0] && f.nsources)
        found(ctx, &f);
}
static void scan(const char *p, const char *end, int vw, int vh, int depth,
                 void (*found)(void *, const struct css_fontface *), void *ctx)
{
    if (depth > 16)
        return;
    while ((p = skip(p, end)) < end) {
        const char *head = p, *open = delimiter(p, end, "{;");
        if (open == end)
            break;
        if (*open == ';') {
            p = open + 1;
            continue;
        }
        const char *tail = open + 1;
        int braces = 1;
        while (tail < end && braces) {
            const char *next = delimiter(tail, end, "{}");
            if (next == end) {
                tail = end;
                break;
            }
            braces += *next == '{' ? 1 : -1;
            tail = next + 1;
        }
        if (braces)
            break;
        const char *bodyend = tail - 1;
        if (open - head >= 10 && !strncasecmp(head, "@font-face", 10) &&
            (head + 10 == open || isspace((unsigned char)head[10])))
            face(open + 1, bodyend, found, ctx);
        else if (*head == '@') {
            const char *name = head + 1;
            while (name < open && (isalnum((unsigned char)*name) || *name == '-'))
                name++;
            char cond[512];
            value(cond, sizeof(cond), name, open);
            int enabled = (name - head == 6 && !strncasecmp(head, "@media", 6))
                              ? css_media_query(cond, vw, vh)
                          : (name - head == 9 && !strncasecmp(head, "@supports", 9))
                              ? css_supports(cond)
                              : (name - head == 6 && !strncasecmp(head, "@layer", 6));
            if (enabled)
                scan(open + 1, bodyend, vw, vh, depth + 1, found, ctx);
        }
        p = tail;
    }
}
void css_font_faces(const char *text, int len, int vw, int vh,
                    void (*found)(void *, const struct css_fontface *), void *ctx)
{
    if (text && len > 0 && found)
        scan(text, text + len, vw, vh, 0, found, ctx);
}
