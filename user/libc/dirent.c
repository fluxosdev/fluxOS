/* Directory streams on top of getdents(2). */
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define DIR_BATCH 16

struct DIR {
    int fd;
    int pos, count;
    struct dirent buf[DIR_BATCH];
};

DIR *opendir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return NULL;
    DIR *d = calloc(1, sizeof(*d));
    if (!d) {
        close(fd);
        return NULL;
    }
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (d->pos >= d->count) {
        int n = getdents(d->fd, d->buf, sizeof(d->buf));
        if (n <= 0)
            return NULL;
        d->count = n / (int)sizeof(struct dirent);
        d->pos = 0;
    }
    return &d->buf[d->pos++];
}

void rewinddir(DIR *d)
{
    lseek(d->fd, 0, SEEK_SET);
    d->pos = d->count = 0;
}

int closedir(DIR *d)
{
    int r = close(d->fd);
    free(d);
    return r;
}
