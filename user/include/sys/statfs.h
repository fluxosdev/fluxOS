#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#include <flux/abi.h>

int statfs(const char *path, struct statfs *st);

#endif
