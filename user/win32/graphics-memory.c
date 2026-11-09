#include <stdint.h>
#include <string.h>
#include "win32.h"
#include "graphics-memory.h"
void *win_graphics_malloc(size_t size)
{
    if(size>0x3fffffe0)return NULL;
    gp_t p=galloc((uint32_t)size+32);if(!p)return NULL;
    uintptr_t data=((uintptr_t)GP(p)+31)&~(uintptr_t)15;
    uint32_t *header=(uint32_t *)(data-16);header[0]=size;header[1]=p;return (void *)data;
}
void win_graphics_free(void *ptr) {if(ptr)gfree(((uint32_t *)((uint8_t *)ptr-16))[1]);}
void *win_graphics_calloc(size_t count,size_t size)
{if(size && count>0x3fffffe0/size)return NULL;void *p=win_graphics_malloc(count*size);if(p)memset(p,0,count*size);return p;}
void *win_graphics_realloc(void *ptr,size_t size)
{
    if(!ptr)return win_graphics_malloc(size);
    if(!size){win_graphics_free(ptr);
    return NULL;
    }
    void *next=win_graphics_malloc(size);if(!next)return NULL;
    uint32_t old=*(uint32_t *)((uint8_t *)ptr-16);memcpy(next,ptr,old<size?old:size);win_graphics_free(ptr);return next;
}
