#ifndef _INTTYPES_H
#define _INTTYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct { intmax_t quot, rem; } imaxdiv_t;

/* The length modifiers of the types whose width goes with the program's:
 * a 64-bit program's int64_t, intmax_t, intptr_t and "fast" types are long. */
#ifdef __x86_64__
#define __PRI64   "l"
#define __PRIPTR  "l"
#define __PRIFAST "l"
#else
#define __PRI64   "ll"
#define __PRIPTR  ""
#define __PRIFAST ""
#endif

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 __PRI64 "d"
#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 __PRI64 "i"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 __PRI64 "u"
#define PRIo8  "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 __PRI64 "o"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 __PRI64 "x"
#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 __PRI64 "X"
#define PRIdMAX __PRI64 "d"
#define PRIiMAX __PRI64 "i"
#define PRIuMAX __PRI64 "u"
#define PRIoMAX __PRI64 "o"
#define PRIxMAX __PRI64 "x"
#define PRIXMAX __PRI64 "X"
#define PRIdPTR __PRIPTR "d"
#define PRIiPTR __PRIPTR "i"
#define PRIuPTR __PRIPTR "u"
#define PRIoPTR __PRIPTR "o"
#define PRIxPTR __PRIPTR "x"
#define PRIXPTR __PRIPTR "X"
#define PRIdFAST8  "d"
#define PRIdFAST16 __PRIFAST "d"
#define PRIdFAST32 __PRIFAST "d"
#define PRIdFAST64 __PRI64 "d"
#define PRIuFAST8  "u"
#define PRIuFAST16 __PRIFAST "u"
#define PRIuFAST32 __PRIFAST "u"
#define PRIuFAST64 __PRI64 "u"
#define PRIxFAST32 __PRIFAST "x"
#define PRIdLEAST32 "d"
#define PRIuLEAST32 "u"

#define SCNd8  "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 __PRI64 "d"
#define SCNi32 "i"
#define SCNi64 __PRI64 "i"
#define SCNu8  "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 __PRI64 "u"
#define SCNx32 "x"
#define SCNx64 __PRI64 "x"
#define SCNo32 "o"
#define SCNdMAX __PRI64 "d"
#define SCNuMAX __PRI64 "u"
#define SCNxMAX __PRI64 "x"
#define SCNdPTR __PRIPTR "d"
#define SCNuPTR __PRIPTR "u"
#define SCNxPTR __PRIPTR "x"

intmax_t imaxabs(intmax_t v);
imaxdiv_t imaxdiv(intmax_t num, intmax_t den);
intmax_t strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);

#ifdef __cplusplus
}
#endif

#endif
