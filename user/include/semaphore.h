/*
 * POSIX semaphores (libc: pthread.c): unnamed ones, between the threads of
 * a process. Named semaphores (sem_open) are not available.
 */
#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>

typedef struct {
    volatile int value;
    volatile int waiters;
} sem_t;

#define SEM_FAILED ((sem_t *)0)

int sem_init(sem_t *s, int shared, unsigned value);
int sem_destroy(sem_t *s);
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *abstime);
int sem_post(sem_t *s);
int sem_getvalue(sem_t *s, int *value);
sem_t *sem_open(const char *name, int flags, ...);
int sem_close(sem_t *s);
int sem_unlink(const char *name);

#ifdef __cplusplus
}
#endif

#endif
