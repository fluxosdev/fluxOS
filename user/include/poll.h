#ifndef _POLL_H
#define _POLL_H

#include <flux/abi.h>

int poll(struct pollfd *fds, unsigned int nfds, int timeout_ms);

#endif
