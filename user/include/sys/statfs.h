#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <flux/abi.h>

int statfs(const char *path, struct statfs *st);

#ifdef __cplusplus
}
#endif

#endif
