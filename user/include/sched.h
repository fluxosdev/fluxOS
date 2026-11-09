#ifndef _SCHED_H
#define _SCHED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct sched_param {
    int sched_priority;
};

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

int sched_yield(void);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);

#ifdef __cplusplus
}
#endif

#endif
