#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#include <flux/abi.h>

/* fluxOS takes the filesystem type as a string ("fat32", "iso9660", "auto"). */
int mount(const char *source, const char *target, const char *type);
int umount(const char *target);

#endif
