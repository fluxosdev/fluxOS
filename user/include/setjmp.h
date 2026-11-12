#ifndef _SETJMP_H
#define _SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifdef __x86_64__
/* rbx, rbp, r12, r13, r14, r15, rsp, rip, mask_saved, sigmask */
typedef uint64_t jmp_buf[10];
typedef uint64_t sigjmp_buf[10];
#else
/* ebx, esi, edi, ebp, esp, eip, mask_saved, sigmask */
typedef uint32_t jmp_buf[8];
typedef uint32_t sigjmp_buf[8];
#endif

int setjmp(jmp_buf env) __attribute__((returns_twice));
int _setjmp(jmp_buf env) __attribute__((returns_twice));
int sigsetjmp(sigjmp_buf env, int savemask) __attribute__((returns_twice));

void longjmp(jmp_buf env, int val) __attribute__((noreturn));
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif

