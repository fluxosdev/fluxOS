#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef int pid_t;
/* A 64-bit program's sizes and file positions are 64 bits. A 32-bit one's
 * are 32: it sees files under 2 GiB, and has lseek64 for a disk's device. */
#ifdef __x86_64__
typedef long ssize_t;
typedef long off_t;
typedef long blkcnt_t;
#else
typedef int ssize_t;
typedef int off_t;
typedef int32_t blkcnt_t;
#endif
typedef long long off64_t;
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

#ifdef __cplusplus
}
#endif

#endif
