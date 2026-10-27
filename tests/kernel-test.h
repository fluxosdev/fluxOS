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
#include "../kernel/core/proc.h"
#undef current
extern struct proc *current;
/* glibc versions before 2.38 do not declare these kernel string helpers. */
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
/* No other processors or interrupts here: locks have nothing to do, and
 * sleeping goes to each test's sleep_on(). A test that includes proc.c
 * (KTEST_PROC) or supplies locks (KTEST_OWN_SPIN, KTEST_OWN_MUTEX) says so first. */
#ifndef KTEST_OWN_SPIN
void spin_lock(spinlock_t *l) { (void)l; }
void spin_unlock(spinlock_t *l) { (void)l; }
int spin_trylock(spinlock_t *l) { (void)l; return 1; }
int spin_held(const spinlock_t *l) { (void)l; return 0; }
#endif
uint32_t irq_save(void) { return 0; }
void irq_restore(uint32_t flags) { (void)flags; }
#ifndef KTEST_OWN_MUTEX
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
int mutex_held(const struct mutex *m) { (void)m; return 0; }
#endif
#ifndef KTEST_PROC
void sleep_on(void *chan);
void sleep_on_lock(void *chan, spinlock_t *lk) { (void)lk; sleep_on(chan); }
void prepare_wait(void *chan) { (void)chan; }
void prepare_wait_timeout(void *chan, uint32_t deadline) { (void)chan; (void)deadline; }
void cancel_wait(void) {}
void do_wait(void) { sleep_on(NULL); }
#endif
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)
#endif
