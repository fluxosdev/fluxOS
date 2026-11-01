#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif
#include <stdlib.h>
#include <stdio.h>
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression)                                                                                   \
    ((expression)                                                                                            \
         ? (void)0                                                                                           \
         : (fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #expression, __FILE__, __LINE__), abort()))
#endif
/* C11's name for _Static_assert (a keyword of its own in C23 and C++) */
#if !defined(__cplusplus) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && __STDC_VERSION__ < 202311L
#define static_assert _Static_assert
#endif
#ifdef __cplusplus
}
#endif

#endif
