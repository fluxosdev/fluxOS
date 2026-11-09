#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../user/desktop/shell/executable.h"

static void check(const char *path, const void *data, size_t size, const char *want)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, data, size) == (ssize_t)size);
    close(fd);
    const char *got = foreign_program_loader(path);
    assert(want ? got && !strcmp(got, want) : !got);
}
int main(void)
{
    char dir[] = "/tmp/flux-routing-XXXXXX", exe[128], com[128];
    assert(mkdtemp(dir));
    snprintf(exe, sizeof(exe), "%s/test.EXE", dir);
    snprintf(com, sizeof(com), "%s/test.CoM", dir);
    unsigned char data[8192] = { 'M', 'Z' };
    check(exe, data, 2, "/bin/dos");
    uint32_t off = 4097;
    memcpy(data + 60, &off, 4);
    memcpy(data + off, "PE\0\0", 4);
    check(exe, data, sizeof(data), "/bin/winrun");
    check(com, data, sizeof(data), "/bin/winrun");
    check(exe, data, off + 3, "/bin/dos");
    off = 0xffffffffu;
    memcpy(data + 60, &off, 4);
    check(exe, data, sizeof(data), "/bin/dos");
    check(exe, "plain text", 10, NULL);
    check(com, "\xc3", 1, "/bin/dos");
    check(com, "", 0, NULL);
    check(com, "\177ELF", 4, NULL);
    check(com, "#!/bin/sh", 9, NULL);
    int fd = open(com, O_WRONLY);
    assert(fd >= 0 && ftruncate(fd, 0xff01) == 0);
    close(fd);
    assert(!foreign_program_loader(com));
    unlink(exe); unlink(com); rmdir(dir);
    puts("Executable routing regressions passed");
}
