/* "wave --js-worker" for host tests: the worker process, with a real clock (tests/browser-worker.c). */
#include <flux.h>
#include <malloc.h>
#include <time.h>
#include "jsdom.h"
#include "jsworker.h"

unsigned int uptime_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned int)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
int wfont_height(const struct wfont *f) { (void)f; return 16; }
const struct wfont *wfont_get(int family, int bold, int italic, int px)
{ (void)family; (void)bold; (void)italic; (void)px; return NULL; }
int meminfo(struct meminfo *info) { (void)info; return -1; }

int main(int argc, char **argv)
{
    mallopt(M_MMAP_MAX, 0);     /* the engine's heap stays below 4 GiB (see browser-dom.c) */
    return jsworker_main(argc, argv);
}
