#ifndef _ERRNO_H
#define _ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <flux/abi.h>

/* Each thread's own (libc keeps it in the thread's block). */
int *__errno_location(void);
#define errno (*__errno_location())

#ifdef __cplusplus
}
#endif

#endif
