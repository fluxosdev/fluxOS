#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>
#include <flux/abi.h>

int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
mode_t umask(mode_t mask);

#endif
