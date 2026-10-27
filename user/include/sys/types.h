#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef int pid_t;
typedef int ssize_t;
typedef int off_t;
typedef long long off64_t;     /* lseek64: positions past 2 GiB */
typedef unsigned int mode_t;
typedef int32_t time_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;

#endif
