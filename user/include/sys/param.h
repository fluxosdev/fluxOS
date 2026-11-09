#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <sys/types.h>

#define MAXPATHLEN     PATH_MAX
#define MAXHOSTNAMELEN 64
#define MAXSYMLINKS    SYMLOOP_MAX
#define NOFILE         OPEN_MAX
#define NGROUPS        NGROUPS_MAX
#define NBBY           8
#define HZ             100
#define DEV_BSIZE      512

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define howmany(x, y)  (((x) + ((y) - 1)) / (y))
#define roundup(x, y)  ((((x) + ((y) - 1)) / (y)) * (y))
#define powerof2(x)    ((((x) - 1) & (x)) == 0)

#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN    4321
#define BYTE_ORDER    LITTLE_ENDIAN

#ifdef __cplusplus
}
#endif

#endif
