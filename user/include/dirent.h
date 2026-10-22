#ifndef _DIRENT_H
#define _DIRENT_H

#include <flux/abi.h>

typedef struct DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
void rewinddir(DIR *d);
int getdents(int fd, struct dirent *buf, unsigned int count);

#endif
