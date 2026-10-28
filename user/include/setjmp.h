#ifndef _SETJMP_H
#define _SETJMP_H

#include <stdint.h>

/* ebx, esi, edi, ebp, esp, eip, mask_saved, sigmask */
typedef uint32_t jmp_buf[8];
typedef uint32_t sigjmp_buf[8];

int setjmp(jmp_buf env) __attribute__((returns_twice));
int _setjmp(jmp_buf env) __attribute__((returns_twice));
int sigsetjmp(sigjmp_buf env, int savemask) __attribute__((returns_twice));

void longjmp(jmp_buf env, int val) __attribute__((noreturn));
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#endif

