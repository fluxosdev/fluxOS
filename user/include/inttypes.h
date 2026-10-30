#ifndef _INTTYPES_H
#define _INTTYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct { intmax_t quot, rem; } imaxdiv_t;

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "lld"
#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "lli"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "llu"
#define PRIo8  "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 "llo"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "llx"
#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "llX"
#define PRIdMAX "lld"
#define PRIiMAX "lli"
#define PRIuMAX "llu"
#define PRIoMAX "llo"
#define PRIxMAX "llx"
#define PRIXMAX "llX"
#define PRIdPTR "d"
#define PRIiPTR "i"
#define PRIuPTR "u"
#define PRIoPTR "o"
#define PRIxPTR "x"
#define PRIXPTR "X"
#define PRIdFAST8  "d"
#define PRIdFAST16 "d"
#define PRIdFAST32 "d"
#define PRIdFAST64 "lld"
#define PRIuFAST8  "u"
#define PRIuFAST16 "u"
#define PRIuFAST32 "u"
#define PRIuFAST64 "llu"
#define PRIxFAST32 "x"
#define PRIdLEAST32 "d"
#define PRIuLEAST32 "u"

#define SCNd8  "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 "lld"
#define SCNi32 "i"
#define SCNi64 "lli"
#define SCNu8  "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 "llu"
#define SCNx32 "x"
#define SCNx64 "llx"
#define SCNo32 "o"
#define SCNdMAX "lld"
#define SCNuMAX "llu"
#define SCNxMAX "llx"
#define SCNdPTR "d"
#define SCNuPTR "u"
#define SCNxPTR "x"

intmax_t imaxabs(intmax_t v);
imaxdiv_t imaxdiv(intmax_t num, intmax_t den);
intmax_t strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);

#ifdef __cplusplus
}
#endif

#endif
