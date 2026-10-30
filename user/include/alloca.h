#ifndef _ALLOCA_H
#define _ALLOCA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define alloca(n) __builtin_alloca(n)

#ifdef __cplusplus
}
#endif

#endif
