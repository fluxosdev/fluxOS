#ifndef _POLL_H
#define _POLL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <flux/abi.h>

int poll(struct pollfd *fds, unsigned int nfds, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
