#ifndef _ERRNO_H
#define _ERRNO_H

#include <flux/abi.h>

/* Each thread's own (libc keeps it in the thread's block). */
int *__errno_location(void);
#define errno (*__errno_location())

#endif
