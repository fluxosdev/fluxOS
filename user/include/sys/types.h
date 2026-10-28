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
typedef int32_t clock_t;
typedef int32_t suseconds_t;
typedef uint32_t useconds_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int id_t;
typedef uint32_t dev_t;
typedef uint32_t ino_t;
typedef uint32_t nlink_t;
typedef int32_t blksize_t;
typedef int32_t blkcnt_t;
typedef uint32_t fsblkcnt_t;
typedef uint32_t fsfilcnt_t;
typedef int clockid_t;
typedef int key_t;
typedef char *caddr_t;

/* BSD's names, which older programs use */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;

#endif
