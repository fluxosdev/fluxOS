#ifndef WIN32_MESSAGES_H
#define WIN32_MESSAGES_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Reserve slots atomically with mkdir, then publish complete names by rename.
 * fluxOS ramfs has no hard links and flock currently does not actually lock.
 * /tmp holds the shared namespace; no guest pointers are stored in it. */
static uint32_t message_register_in(const char *dir, const uint16_t *name, size_t len)
{
    if (mkdir(dir, 0700) < 0 && errno != EEXIST)
        return 0;
    char temp[128];
    snprintf(temp, sizeof(temp), "%s/.new-XXXXXX", dir);
    int fd = mkstemp(temp);
    if (fd < 0)
        return 0;
    size_t bytes = (len + 1) * 2;
    int ok = write(fd, name, bytes) == (ssize_t)bytes;
    close(fd);
    uint32_t result = 0;
    if (ok) for (uint32_t slot = 0; slot < 0x4000; slot++) {
        char path[80];
        snprintf(path, sizeof(path), "%s/%04x", dir, slot);
        char record[96];
        snprintf(record, sizeof(record), "%s/name", path);
        if (mkdir(path, 0700) == 0) {
            if (rename(temp, record) == 0)
                result = 0xc000 + slot;
            else
                rmdir(path);
            break;
        }
        if (errno != EEXIST)
            break;
        /* Another process may still be publishing its reserved slot.
         * Fail on an abandoned reservation rather than assigning a duplicate. */
        for (int retry = 0; retry < 1000; retry++) {
            fd = open(record, O_RDONLY);
            if (fd >= 0 || errno != ENOENT) break;
            usleep(1000);
        }
        if (fd < 0)
            break;
        uint16_t stored[256];
        ssize_t got = read(fd, stored, sizeof(stored));
        close(fd);
        if (got == (ssize_t)bytes && !memcmp(stored, name, bytes)) {
            result = 0xc000 + slot;
            break;
        }
    }
    unlink(temp);
    return result;
}
#endif
