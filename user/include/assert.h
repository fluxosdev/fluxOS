#ifndef _ASSERT_H
#define _ASSERT_H
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
#endif
