#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <flux/abi.h>

/* Device control. arg is a pointer or a number, as the request says;
 * the sound devices' requests are in <sys/soundcard.h>. */
int ioctl(int fd, unsigned int request, ...);

#endif
