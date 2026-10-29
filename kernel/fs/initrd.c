/*
 * Unpacks the ustar archive GRUB loaded as a module into the ramfs.
 */
#include "kernel.h"
#include "mm.h"
#include "vfs.h"

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
};

static uint32_t octal(const char *s, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (uint32_t)(s[i] - '0');
    return v;
}

void initrd_load(const uint8_t *data, uint32_t size)
{
    uint32_t off = 0, files = 0, dirs = 0;
    char *path = kmalloc(2 * PATH_MAX), *parent = path ? path + PATH_MAX : NULL;
    if (!path)
        panic("initrd: out of memory");

    while (off + 512 <= size) {
        const struct tar_header *h = (const struct tar_header *)(data + off);
        if (!h->name[0])
            break;
        if (memcmp(h->magic, "ustar", 5) != 0) {
            kprintf("initrd: bad header at offset %u\n", off);
            break;
        }

        uint32_t fsize = octal(h->size, sizeof(h->size));
        uint32_t mode = octal(h->mode, sizeof(h->mode));
        uint32_t mtime = octal(h->mtime, sizeof(h->mtime));

        path[0] = 0;
        if (h->prefix[0]) {
            strncat(path, h->prefix, MIN(sizeof(h->prefix), PATH_MAX - 2));
            strcat(path, "/");
        }
        strncat(path, h->name, MIN(sizeof(h->name), PATH_MAX - strlen(path) - 1));

        char *p = path;
        while (*p == '.' && p[1] == '/')
            p += 2;
        while (*p == '/')
            p++;
        uint32_t len = strlen(p);
        while (len && p[len - 1] == '/')
            p[--len] = 0;

        const uint8_t *content = data + off + 512;
        off += 512 + ALIGN_UP(fsize, 512u);
        if (!*p || !strcmp(p, "."))
            continue;

        if (h->typeflag == '5') {
            struct vnode *dir;
            if (vfs_mkdir_p(p, &dir) == 0) {
                dir->mode = mode & 07777;
                dir->mtime = mtime;
                dirs++;
            }
        } else if (h->typeflag == '0' || h->typeflag == 0) {
            struct vnode *dir = vfs_root, *vn;
            char name[NAME_MAX + 1];
            if (vfs_resolve_parent(p, NULL, &dir, name) < 0) {
                /* Parent missing from the archive: create it. */
                strlcpy(parent, p, PATH_MAX);
                char *slash = strrchr(parent, '/');
                if (slash)
                    *slash = 0;
                if (!slash || vfs_mkdir_p(parent, NULL) < 0 ||
                    vfs_resolve_parent(p, NULL, &dir, name) < 0)
                    continue;
            }
            if (vfs_create(dir, name, VN_FILE, &vn) < 0)
                continue;
            vfs_write(vn, content, 0, fsize);
            vn->mode = mode & 07777;
            vn->mtime = mtime;
            files++;
        }
    }
    kfree(path);
    boot_ok("Unpacked initrd: %u files, %u directories", files, dirs);
}
