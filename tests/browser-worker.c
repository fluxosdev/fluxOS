/* Dedicated workers end to end on the host: a page runs rootfs/usr/share/www/worker-tests.js and
 * its workers are real processes (build/tests/jsworker). Files under file:///usr/share/www/ are
 * served from rootfs, the way the browser would fetch them. */
#define main browser_dom_main
#include "browser-dom.c"
#undef main
#include <unistd.h>

static struct jsdom *page;
static const char www[] = "file:///usr/share/www/";

static char *read_url(const char *url, long *size)
{
    if (strncmp(url, www, sizeof(www) - 1))
        return NULL;
    char path[4096];
    snprintf(path, sizeof(path), "rootfs/usr/share/www/%s", url + sizeof(www) - 1);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)*size + 1);
    if (text && fread(text, 1, (size_t)*size, f) != (size_t)*size) {
        free(text);
        text = NULL;
    }
    if (text)
        text[*size] = 0;
    fclose(f);
    return text;
}

static void fetch_module(void *ctx, int id, const char *url)
{
    (void)ctx;
    long size = 0;
    char *text = read_url(url, &size);
    jsdom_module_fetched(page, id, text, text ? (int)size : 0);
    free(text);
}

/* Like browser.c, which reads a file:// URL directly: no headers, no pieces, the whole body with done. */
static void http(void *ctx, int id, const char *method, const char *url, const void *body, int body_len,
                 const char *headers, int credentials)
{
    (void)ctx, (void)method, (void)body, (void)body_len, (void)headers, (void)credentials;
    long size = 0;
    char *text = read_url(url, &size);
    jsdom_http_done(page, id, text ? 200 : 0, text, 0, text, text ? (int)size : 0);
    free(text);
}

/* The page says how it went through its title. */
static char title[4096];
static void set_title(void *ctx, const char *text)
{
    (void)ctx;
    snprintf(title, sizeof(title), "%s", text);
}

int main(void)
{
    mallopt(M_MMAP_MAX, 0);
    struct arena a = {0};
    const char *html = "<!doctype html><html><head><title></title></head><body></body></html>";
    struct jshost host = {.arena = &a, .root = html_parse(&a, html, (int)strlen(html)),
                          .url = "file:///usr/share/www/workers.html", .base = "file:///usr/share/www/workers.html",
                          .status = status, .changed = changed, .set_title = set_title, .http = http,
                          .fetch_module = fetch_module};
    page = jsdom_new(&host);
    if (!page)
        return 2;
    long size = 0;
    char *tests = read_url("file:///usr/share/www/worker-tests.js", &size);
    if (!tests)
        return 2;
    jsdom_run_script(page, NULL, tests, (int)size, "worker-tests.js");
    free(tests);
    static const char start[] = "runWorkerTests(function (name, ok, detail) {"
                                " console.log((ok ? 'ok   ' : 'FAIL ') + name + (ok ? '' : ': ' + detail)); });";
    jsdom_run_script(page, NULL, start, (int)sizeof(start) - 1, "worker test start");
    static const char poll_done[] =
        "if (workerResults.done) document.title = workerResults.failed.length ? 'failed' : 'passed ' + workerResults.total;";
    for (int i = 0; i < 3000 && !errors && !title[0]; i++) {
        host_time += 10;
        jsdom_run_timers(page);
        jsdom_run_script(page, NULL, poll_done, (int)sizeof(poll_done) - 1, "worker test poll");
        usleep(5000);
    }
    jsdom_free(page);       /* ends the workers still running */
    arena_free(&a);
    if (errors || strncmp(title, "passed ", 7)) {
        fprintf(stderr, "worker tests: %s\n", title[0] ? title : "did not finish");
        return 1;
    }
    printf("browser workers: %s cases passed\n", title + 7);
    return 0;
}
