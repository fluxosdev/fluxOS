/* Reading a whole file for a decoder (kept apart from image.c, so that a program drawing only PNG
 * icons does not take in every decoder). */
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gfx.h"

#define MAX_FILE (16u * 1024 * 1024)

/* The whole file in a malloc'd buffer, or NULL. */
uint8_t *file_read_all(const char *path, uint32_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && (uint32_t)st.st_size < MAX_FILE) {
        buf = malloc((size_t)st.st_size);
        uint32_t got = 0;
        int r = 1;
        while (buf && got < (uint32_t)st.st_size &&
               (r = (int)read(fd, buf + got, (size_t)st.st_size - got)) > 0)
            got += (uint32_t)r;
        if (buf && got != (uint32_t)st.st_size) {
            free(buf);
            buf = NULL;
        }
        *len = got;
    }
    close(fd);
    return buf;
}
