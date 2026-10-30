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

/* Shared memory segments (shm.c) */
int sys_shmget(uint32_t key, uint32_t size, uint32_t flags, uint32_t d, uint32_t e);
int sys_shmat(uint32_t id, uint32_t usize, uint32_t c, uint32_t d, uint32_t e);
int sys_shmctl(uint32_t id, uint32_t cmd, uint32_t ubuf, uint32_t d, uint32_t e);
void shm_proc_exit(int pid);

#endif
