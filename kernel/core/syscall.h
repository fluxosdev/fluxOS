#ifndef SYSCALL_H
#define SYSCALL_H

#include "arch.h"

void syscall_dispatch(struct trapframe *tf);
/* 1 if [ptr, ptr + len) is mapped user memory the kernel may touch. */
int user_access_ok(const void *ptr, uint32_t len);

#endif
