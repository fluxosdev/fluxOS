/* Shared support for host tests that include the actual kernel C sources.
 * Hardware, scheduling and disk I/O are replaced by each test's small stubs. */
#ifndef KERNEL_TEST_H
#define KERNEL_TEST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
/* Host tests have no kernel TSS. Keep process selection in the harness
 * while including the production process layout and kernel sources. */
#include "../kernel/proc.h"
#undef current
extern struct proc *current;
/* glibc versions before 2.38 do not declare these kernel string helpers. */
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)
#endif
