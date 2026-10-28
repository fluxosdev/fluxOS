#ifndef SYSCALL_H
#define SYSCALL_H

#include "arch.h"

void syscall_dispatch(struct trapframe *tf);
/* 1 if [ptr, ptr + len) is mapped user memory the kernel may touch. */
int user_access_ok(const void *ptr, uint32_t len);
/* Copies between a process's memory and the kernel's: the range is checked
 * (and demand-paged) first, and a page that is still missing when the copy
 * runs gives -EFAULT rather than a kernel-mode fault. 0 when done. */
int copy_from_user(void *dst, const void *usrc, uint32_t n);
int copy_to_user(void *udst, const void *src, uint32_t n);

#endif
