/* How much memory the engine keeps for a script: run each file given on the
 * command line in a fresh engine and print the live heap after a collection.
 *
 *   js-memory [--wrap] FILE...
 *
 * --wrap puts the source inside `if (0) { ... }`, so nothing in it runs: the
 * figure is what parsing, analysis and the top level's code keep. Without it
 * the script's top level runs (errors are printed and the heap is measured
 * anyway). Peak resident memory comes from /usr/bin/time around the run. */
#include <flux.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "jsint.h"

int meminfo(struct meminfo *info) { (void)info; return -1; }
unsigned int uptime_ms(void) { return 0; }

static char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 16);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s);
        s = NULL;
    }
    fclose(f);
    *len = n;
    return s;
}

/* Zero the dead stack below this frame, so stale words left by the engine's deep frames are not
 * taken for roots by the conservative collector. */
static __attribute__((noinline)) void scrub_stack(void)
{
    volatile char buf[1 << 20];
    memset((char *)buf, 0, sizeof(buf));
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    mallopt(M_MMAP_MAX, 0);             /* the engine wants its heap below 4 GiB */
    int wrap = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wrap")) {
            wrap = 1;
            continue;
        }
        long n;
        char *src = slurp(argv[i], &n);
        if (!src) {
            perror(argv[i]);
            return 1;
        }
        if (wrap) {
            char *w = malloc((size_t)n + 32);
            int k = sprintf(w, "if (0) {\n");
            memcpy(w + k, src, (size_t)n);
            k += (int)n;
            k += sprintf(w + k, "\n}\n");
            free(src);
            src = w;
            n = k;
        }
        struct js *J = js_new(NULL);
        J->step_limit = 4000000000u;
        J->heap_limit = (size_t)3 << 30;        /* measure, rather than stop at the page's limit */
        gc_collect(J);
        size_t base = gc_heap_bytes(J);
        double t0 = now();
        int r = js_eval(J, src, (int)n, argv[i]);
        double t1 = now();
        if (r < 0)
            fprintf(stderr, "%s: %s\n", argv[i], js_error(J));
        scrub_stack();
        gc_collect(J);
        size_t live = gc_heap_bytes(J) - base;
        printf("%-28s %8.2f MB source %8.2f MB live %6.1f bytes/byte %6.2f s%s\n",
               strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i], n / 1048576.0,
               live / 1048576.0, (double)live / (double)n, t1 - t0, r < 0 ? " (error)" : "");
        fflush(stdout);
        js_free(J);
        free(src);
    }
    return 0;
}
