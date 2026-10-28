#ifndef _LIMITS_H
#define _LIMITS_H

#include <flux/abi.h>

#define CHAR_BIT  8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN  SCHAR_MIN
#define CHAR_MAX  SCHAR_MAX
#define MB_LEN_MAX 4
#define SHRT_MIN  (-32768)
#define SHRT_MAX  32767
#define USHRT_MAX 65535
#define INT_MIN   (-2147483647 - 1)
#define INT_MAX   2147483647
#define UINT_MAX  4294967295U
#define LONG_MIN  INT_MIN
#define LONG_MAX  INT_MAX
#define ULONG_MAX UINT_MAX
#define LLONG_MIN  (-9223372036854775807LL - 1)
#define LLONG_MAX  9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL
#define LONG_BIT  32
#define WORD_BIT  32
#define SSIZE_MAX INT_MAX

/* What one process may count on (sysconf(3) answers for the others). */
#define ARG_MAX        (256 * 1024)
#define CHILD_MAX      64
#define OPEN_MAX       256
#define LINK_MAX       1
#define PIPE_BUF       512
#define LINE_MAX       2048
#define NGROUPS_MAX    1
#define HOST_NAME_MAX  63
#define LOGIN_NAME_MAX 32
#define TTY_NAME_MAX   32
#define SYMLOOP_MAX    8
#define IOV_MAX        1024
#define TZNAME_MAX     6
#define RE_DUP_MAX     255
#define _POSIX_PATH_MAX 256
#define _POSIX_NAME_MAX 14
#define _POSIX_ARG_MAX  4096
#define _POSIX_OPEN_MAX 20
#define _POSIX2_RE_DUP_MAX 255

#endif
