#ifndef WIN_GRAPHICS_MEMORY_H
#define WIN_GRAPHICS_MEMORY_H
#include <stddef.h>
/* Bulk graphics allocations must not grow the native loader brk into an EXE. */
void *win_graphics_malloc(size_t size);
void *win_graphics_calloc(size_t count,size_t size);
void *win_graphics_realloc(void *ptr,size_t size);
void win_graphics_free(void *ptr);
#endif
