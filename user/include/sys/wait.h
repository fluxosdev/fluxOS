#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>
#include <flux/abi.h>

pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);

#endif
