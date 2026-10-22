#ifndef SYSCALL_H
#define SYSCALL_H

#include "arch.h"

void syscall_dispatch(struct trapframe *tf);

#endif
