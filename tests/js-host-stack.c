/* Let the real collector scan the Linux runner's main stack as it does
 * fluxOS's fixed user stack. Used only with -DJS_HOST_TEST. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stddef.h>

void js_test_stack_bounds(const char **lo, const char **hi)
{
    static const char *bottom, *top;
    if (!top) {
        pthread_attr_t attr;
        void *base;
        size_t size;
        int r = pthread_getattr_np(pthread_self(), &attr);
        assert(r == 0);
        r = pthread_attr_getstack(&attr, &base, &size);
        assert(r == 0);
        pthread_attr_destroy(&attr);
        bottom = base;
        top = bottom + size;
    }
    *lo = bottom;
    *hi = top;
}
