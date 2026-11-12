#ifndef DESKTOP_EXECUTABLE_H
#define DESKTOP_EXECUTABLE_H

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* COM has no signature: use its extension and DOS's maximum image size.
 * Probe PE before MZ, since Windows executables also contain a DOS stub. */
static const char *foreign_program_loader(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    unsigned char head[64], sig[4];
    const char *loader = NULL;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
        goto done;
    ssize_t n = read(fd, head, sizeof(head));
    if (n >= 2 && ((head[0] == 'M' && head[1] == 'Z') ||
                   (head[0] == 'Z' && head[1] == 'M'))) {
        loader = "/bin/dos";
        if (n == sizeof(head)) {
            uint32_t off;
            memcpy(&off, head + 0x3c, 4);
            if (off >= 64 && st.st_size >= 4 && off <= (uint64_t)st.st_size - 4 &&
                lseek(fd, (off_t)off, SEEK_SET) == (off_t)off &&
                read(fd, sig, 4) == 4 && !memcmp(sig, "PE\0\0", 4))
                loader = "/bin/winrun";
        }
    } else if (n > 0 && !(n >= 4 && !memcmp(head, "\177ELF", 4)) &&
               !(n >= 2 && head[0] == '#' && head[1] == '!')) {
        size_t len = strlen(path);
        if (len > 4 && !strcasecmp(path + len - 4, ".com") && st.st_size <= 0xff00)
            loader = "/bin/dos";
    }
done:
    close(fd);
    return loader;
}

#endif
