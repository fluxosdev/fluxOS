/*
 * The desktop's settings on the partition fluxOS started from, beside
 * the boot loader: /boot/limine/fluxos.env there, "name=value" lines. The
 * desktop keeps its own settings in it (fluxos_wall, fluxos_wallmode,
 * fluxos_theme, fluxos_driver and so on).
 *
 * Two of the names are the boot loader's business: fluxos_gfx, the mode
 * Limine asks the firmware for, and fluxos_video, a display driver's own
 * mode, which it passes to the kernel. Limine reads only its own menu,
 * /boot/limine/limine.conf, where the two are the macros GFX and VIDEO
 * (limine/limine.conf in the source tree): setting either here rewrites
 * its line there as well.
 *
 * Started from the CD there is no such partition, and nothing can be kept.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <flux.h>
#include "desktop.h"

#define ENV_MAX  4096                   /* the settings file */
#define CONF_MAX 16384                  /* Limine's menu */
#define ENV_FILE  "boot/limine/fluxos.env"
#define CONF_FILE "boot/limine/limine.conf"
#define DEFAULT_GFX "1024x768x32"

static int booted_from_disk(void)
{
    char buf[16] = "";
    int fd = open("/etc/boot-source", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n >= 4 && !strncmp(buf, "disk", 4);
}

/* The mounted volume with Limine's menu on it (FAT: the root itself, or
 * the boot partition before an ext2 root), when started from a hard disk. */
int bootenv_boot_dir(char *out, size_t size)
{
    out[0] = 0;
    if (!booted_from_disk())
        return 0;
    struct blkinfo bi;
    for (int i = 0; blkinfo(i, &bi) == 0; i++) {
        if (!bi.mountpoint[0] || (strncmp(bi.fstype, "fat", 3) && strcmp(bi.fstype, "ext2")))
            continue;
        char path[192];
        snprintf(path, sizeof(path), "%s/" CONF_FILE, bi.mountpoint);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            snprintf(out, size, "%s", bi.mountpoint);
            return 1;
        }
    }
    return 0;
}

/* A file of dir, whole, NUL-terminated: its length, 0 if there is none. */
static int read_file(const char *dir, const char *file, char *buf, int max)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        buf[0] = 0;
        return 0;
    }
    int n = 0, r;
    while (n < max && (r = (int)read(fd, buf + n, (size_t)(max - n))) > 0)
        n += r;
    close(fd);
    buf[n] = 0;
    return n;
}

/* Written beside the old one and renamed over it, so that a power cut
 * leaves one or the other whole. */
static int write_file(const char *dir, const char *file, const char *buf, int len)
{
    char path[192], tmp[200];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t w = write(fd, buf, (size_t)len);
    if (close(fd) < 0 || w != len || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    sync();
    return 0;
}

int bootenv_get(const char *dir, const char *key, char *out, size_t size)
{
    static char buf[ENV_MAX + 1];
    out[0] = 0;
    if (!dir[0])
        return 0;
    int n = read_file(dir, ENV_FILE, buf, ENV_MAX);
    size_t klen = strlen(key);
    for (char *line = buf; line < buf + n; ) {
        char *nl = memchr(line, '\n', (size_t)(buf + n - line));
        if (nl)
            *nl = 0;
        if (!strncmp(line, key, klen) && line[klen] == '=') {
            snprintf(out, size, "%s", line + klen + 1);
            return 1;
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    return 0;
}

/* Limine's menu with the macro's line ("${NAME}=...") given this value; 0
 * if the menu has no such line, or would not fit. */
static int set_macro(char *conf, int *len, const char *name, const char *value)
{
    char start[32];
    int slen = snprintf(start, sizeof(start), "${%s}=", name);
    for (char *line = conf; line < conf + *len; ) {
        char *nl = memchr(line, '\n', (size_t)(conf + *len - line));
        char *end = nl ? nl : conf + *len;
        if (!strncmp(line, start, (size_t)slen)) {
            int vlen = (int)strlen(value), old = (int)(end - (line + slen));
            if (*len - old + vlen > CONF_MAX)
                return 0;
            memmove(line + slen + vlen, end, (size_t)(conf + *len - end) + 1);
            memcpy(line + slen, value, (size_t)vlen);
            *len += vlen - old;
            return 1;
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    return 0;
}

/* Set the named variables (an empty value removes one) and keep the rest. */
int bootenv_update(const char *dir, const char *const *keys, const char *const *vals, int count)
{
    static char old[ENV_MAX + 1], out[ENV_MAX + 1], conf[CONF_MAX + 1];
    int n = read_file(dir, ENV_FILE, old, ENV_MAX);
    int o = snprintf(out, sizeof(out), "# The fluxOS desktop's settings (name=value); see also limine.conf\n");
    for (char *line = old; line < old + n; ) {
        char *nl = memchr(line, '\n', (size_t)(old + n - line));
        if (nl)
            *nl = 0;
        char *eq = strchr(line, '=');
        int replaced = 0;
        if (*line == '#' || !eq)
            replaced = 1;               /* the header and junk are not kept */
        for (int k = 0; k < count && !replaced; k++)
            if (strlen(keys[k]) == (size_t)(eq - line) && !strncmp(line, keys[k], (size_t)(eq - line)))
                replaced = 1;
        if (!replaced) {
            int w = snprintf(out + o, sizeof(out) - (size_t)o, "%s\n", line);
            if (w < 0 || o + w >= (int)sizeof(out))
                return -1;
            o += w;
        }
        if (!nl)
            break;
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
    if (write_file(dir, ENV_FILE, out, o) < 0)
        return -1;

    /* The boot loader's two, into its menu (unless they are as they were). */
    int clen = read_file(dir, CONF_FILE, conf, CONF_MAX), changed = 0;
    for (int k = 0; k < count && clen > 0; k++) {
        const char *macro = !strcmp(keys[k], "fluxos_gfx") ? "GFX" : !strcmp(keys[k], "fluxos_video") ? "VIDEO" : NULL;
        if (!macro)
            continue;
        const char *value = vals[k][0] || strcmp(macro, "GFX") ? vals[k] : DEFAULT_GFX;
        /* (a mode is digits and a few letters: nothing here can upset the menu) */
        if (strpbrk(value, "\n\r${}"))
            return -1;
        changed |= set_macro(conf, &clen, macro, value);
    }
    if (changed && write_file(dir, CONF_FILE, conf, clen) < 0)
        return -1;
    return 0;
}
