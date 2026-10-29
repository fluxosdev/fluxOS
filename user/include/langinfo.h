#ifndef _LANGINFO_H
#define _LANGINFO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int nl_item;

#define CODESET   14
#define D_T_FMT   0x20028
#define D_FMT     0x20029
#define T_FMT     0x2002A
#define RADIXCHAR 0x10000
#define THOUSEP   0x10001
#define YESEXPR   0x50000
#define NOEXPR    0x50001

char *nl_langinfo(nl_item item);

#ifdef __cplusplus
}
#endif

#endif
