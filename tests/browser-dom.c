/* Host runner: links the actual browser parser, DOM and JS interpreter. */
#include <flux.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jsdom.h"

unsigned int uptime_ms(void) { return 0; }
int meminfo(struct meminfo *info) { (void)info; return -1; }
static int errors;
static void changed(void *ctx) { (void)ctx; }
static void status(void *ctx, const char *message)
{
    (void)ctx;
    fprintf(stderr, "%s\n", message);
    errors++;
}

int main(int argc, char **argv)
{
    /* The engine uses a 32-bit page directory. A non-PIE Linux binary
     * with mmap allocation disabled keeps its heap below 4 GiB. */
    mallopt(M_MMAP_MAX, 0);
    if (argc != 3)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *src = calloc((size_t)size + 1, 1);
    if (!src || fread(src, 1, (size_t)size, f) != (size_t)size)
        return 2;
    fclose(f);
    struct arena arena = { 0 };
    const char *html = "<!doctype html><html><head></head><body></body></html>";
    struct jshost host = { .arena = &arena, .root = html_parse(&arena, html, (int)strlen(html)),
                           .url = "file:///tests.html", .status = status, .changed = changed };
    struct jsdom *dom = jsdom_new(&host);
    if (!dom)
        return 2;
    const char *prelude =
        "var count = 0; function group(n) {}"
        "function test(n, f, expected) {"
        " var result = f();"
        " if (JSON.stringify(result) !== JSON.stringify(expected))"
        "   throw new Error(n + ': ' + JSON.stringify(result));"
        " count++; }";
    jsdom_run_script(dom, NULL, prelude, (int)strlen(prelude), "test harness");
    jsdom_run_script(dom, NULL, src, (int)size, argv[1]);
    int expected = atoi(argv[2]);
    if (expected <= 0)
        return 2;
    char end[128];
    snprintf(end, sizeof(end), "if (count !== %d) throw new Error('Incomplete test run: ' + count);", expected);
    jsdom_run_script(dom, NULL, end, (int)strlen(end), "test count");
    jsdom_free(dom);
    arena_free(&arena);
    free(src);
    if (!errors)
        printf("%d browser DOM regressions passed (%s)\n", expected, argv[1]);
    return errors ? 1 : 0;
}
