/*
 * GRUB's environment block, /boot/grub/grubenv on the partition fluxOS
 * started from: a 1024 byte file of "name=value" lines after a header
 * line, padded with '#'. The boot menu reads some of the names
 * (fluxos_gfx, fluxos_video); the desktop keeps its own settings in
 * others (fluxos_wall, fluxos_wallmode), which GRUB leaves alone. Started
 * from the CD there is no such partition, and nothing can be kept.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <flux.h>
#include "desktop.h"

#define GRUBENV_SIZE 1024

static int booted_from_disk(void)
{
    char buf[16] = "";
    int fd = open("/etc/boot-source", O_RDONLY);
    if (fd < 0)
        return 0;
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n >= 4 && !strncmp(buf, "disk", 4);
}

/* The mounted FAT volume with GRUB on it, when started from a hard disk. */
int grubenv_boot_dir(char *out, size_t size)
{
    out[0] = 0;
    if (!booted_from_disk())
        return 0;
    struct blkinfo bi;
    for (int i = 0; blkinfo(i, &bi) == 0; i++) {
        if (!bi.mountpoint[0] || strncmp(bi.fstype, "fat", 3) != 0)
            continue;
        char path[192];
        snprintf(path, sizeof(path), "%s/boot/grub/grub.cfg", bi.mountpoint);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            snprintf(out, size, "%s", bi.mountpoint);
            return 1;
        }
    }
    return 0;
}

static int read_block(const char *dir, char *buf)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/boot/grub/grubenv", dir);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    int n = (int)read(fd, buf, GRUBENV_SIZE);
    close(fd);
    n = n < 0 ? 0 : n;
    buf[n] = 0;
    return n;
}

int grubenv_get(const char *dir, const char *key, char *out, size_t size)
{
    char buf[GRUBENV_SIZE + 1];
    out[0] = 0;
    if (!dir[0])
        return 0;
    int n = read_block(dir, buf);
    size_t klen = strlen(key);
    for (char *line = buf; line < buf + n; ) {
        char *nl = memchr(line, '\n', (size_t)(buf + n - line));
        if (!nl)
            break;                      /* the padding has no newline */
        *nl = 0;
        if (!strncmp(line, key, klen) && line[klen] == '=') {
            snprintf(out, size, "%s", line + klen + 1);
            return 1;
        }
        line = nl + 1;
    }
    return 0;
}

/* Set the named variables (an empty value removes one) and keep the rest. */
int grubenv_update(const char *dir, const char *const *keys, const char *const *vals, int count)
{
    char old[GRUBENV_SIZE + 1], out[GRUBENV_SIZE];
    int n = read_block(dir, old);
    int o = snprintf(out, sizeof(out), "# GRUB Environment Block\n");
    for (char *line = old; line < old + n; ) {
        char *nl = memchr(line, '\n', (size_t)(old + n - line));
        if (!nl)
            break;
        *nl = 0;
        char *eq = strchr(line, '=');
        int replaced = 0;
        if (*line == '#' || !eq)
            replaced = 1;               /* the header, padding and junk are not kept */
        for (int k = 0; k < count && !replaced; k++)
            if (strlen(keys[k]) == (size_t)(eq - line) && !strncmp(line, keys[k], (size_t)(eq - line)))
                replaced = 1;
        if (!replaced) {
            int w = snprintf(out + o, sizeof(out) - (size_t)o, "%s\n", line);
            if (w < 0 || o + w >= (int)sizeof(out))
                return -1;
            o += w;
        }
        line = nl + 1;
    }
    for (int k = 0; k < count; k++) {
        if (!vals[k][0])
            continue;
        int w = snprintf(out + o, sizeof(out) - (size_t)o, "%s=%s\n", keys[k], vals[k]);
        if (w < 0 || o + w >= (int)sizeof(out))
            return -1;
        o += w;
    }
    memset(out + o, '#', sizeof(out) - (size_t)o);

    char path[192];
    snprintf(path, sizeof(path), "%s/boot/grub/grubenv", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    int w = (int)write(fd, out, sizeof(out));
    close(fd);
    sync();
    return w == (int)sizeof(out) ? 0 : -1;
}
