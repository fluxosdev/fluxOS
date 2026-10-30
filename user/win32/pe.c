/*
 * Portable Executable images: reading them into the program's memory at
 * the address they were linked for (or another, applying the
 * relocations), binding their imports, finding their exports, thread
 * local storage and resources.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "win32.h"

#define IMAGE_FILE_RELOCS_STRIPPED 0x0001
#define IMAGE_FILE_DLL 0x2000
#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_HIGH 1
#define IMAGE_REL_BASED_LOW 2
#define IMAGE_REL_BASED_HIGHLOW 3
#define DLL_PROCESS_ATTACH 1

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The headers of a file, read into a buffer: 0 on success. */
static int read_headers(int fd, uint8_t *hdr, uint32_t hdrsize, uint32_t *nt_off, char *err, size_t errsize)
{
    lseek(fd, 0, SEEK_SET);
    int n = (int)read(fd, hdr, hdrsize);
    if (n < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z') {
        snprintf(err, errsize, "not a Windows program");
        return -1;
    }
    uint32_t off = rd32(hdr + 0x3C);
    if (off + 0x100 > hdrsize) {
        snprintf(err, errsize, "header too far into the file");
        return -1;
    }
    if (off + 4 > (uint32_t)n || memcmp(hdr + off, "PE\0\0", 4) != 0) {
        snprintf(err, errsize, "not a Windows program (no PE header)");
        return -1;
    }
    if (rd16(hdr + off + 4) != 0x14C) {
        snprintf(err, errsize, "not a 32-bit x86 program (machine %04x)", rd16(hdr + off + 4));
        return -1;
    }
    if (rd16(hdr + off + 24) != 0x10B) {
        snprintf(err, errsize, "not a 32-bit program (optional header magic %04x)", rd16(hdr + off + 24));
        return -1;
    }
    *nt_off = off;
    return 0;
}

int pe_file_info(const char *path, uint16_t *subsystem, int *is_dll)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    uint8_t hdr[4096];
    uint32_t nt;
    char err[64];
    int r = read_headers(fd, hdr, sizeof(hdr), &nt, err, sizeof(err));
    close(fd);
    if (r < 0)
        return -1;
    if (subsystem)
        *subsystem = rd16(hdr + nt + 24 + 68);
    if (is_dll)
        *is_dll = (rd16(hdr + nt + 22) & IMAGE_FILE_DLL) != 0;
    return 0;
}

static void relocate(struct module *m, uint32_t delta)
{
    uint32_t rva = m->dir[DIR_BASERELOC][0], size = m->dir[DIR_BASERELOC][1];
    uint8_t *img = GP(m->base);
    uint32_t off = 0;
    if (rva > m->size || size > m->size - rva)
        return;
    while (size - off >= 8) {
        uint32_t page = rd32(img + rva + off), bsize = rd32(img + rva + off + 4);
        if (bsize < 8 || bsize > size - off)
            break;
        for (uint32_t i = 8; i + 2 <= bsize; i += 2) {
            uint16_t e = rd16(img + rva + off + i);
            uint32_t at = page + (e & 0xFFF);
            if (at < page || at > m->size - 4)
                continue;
            switch (e >> 12) {
            case IMAGE_REL_BASED_HIGHLOW: {
                uint32_t v = rd32(img + at) + delta;
                memcpy(img + at, &v, 4);
                break;
            }
            case IMAGE_REL_BASED_HIGH: {
                uint16_t v = (uint16_t)(rd16(img + at) + (delta >> 16));
                memcpy(img + at, &v, 2);
                break;
            }
            case IMAGE_REL_BASED_LOW: {
                uint16_t v = (uint16_t)(rd16(img + at) + (delta & 0xFFFF));
                memcpy(img + at, &v, 2);
                break;
            }
            default:
                break;
            }
        }
        off += bsize;
    }
}

int pe_map(const char *path, struct module *m, char *err, size_t errsize)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errsize, "cannot open %s", path);
        return -1;
    }
    uint8_t hdr[4096];
    uint32_t nt;
    if (read_headers(fd, hdr, sizeof(hdr), &nt, err, errsize) < 0) {
        close(fd);
        return -1;
    }
    struct stat st;
    fstat(fd, &st);
    const uint8_t *fh = hdr + nt + 4, *oh = hdr + nt + 24;
    uint16_t nsections = rd16(fh + 2), characteristics = rd16(fh + 18);
    uint16_t opt_size = rd16(fh + 16);
    uint32_t image_base = rd32(oh + 28), size_of_image = rd32(oh + 56), size_of_headers = rd32(oh + 60);
    uint32_t section_align = rd32(oh + 32);
    uint32_t sect_off = nt + 24 + opt_size;
    if (sect_off + nsections * 40u > sizeof(hdr) || size_of_headers > sizeof(hdr)) {
        /* a bigger header: read the section table where it is */
        if (sect_off + nsections * 40u > sizeof(hdr)) {
            snprintf(err, errsize, "too many sections");
            close(fd);
            return -1;
        }
    }
    if (!size_of_image || size_of_image < size_of_headers || size_of_image > 0x40000000u) {
        snprintf(err, errsize, "bad image size %u", size_of_image);
        close(fd);
        return -1;
    }
    size_of_image = (size_of_image + 0xFFF) & ~0xFFFu;
    m->is_exe = !(characteristics & IMAGE_FILE_DLL);
    m->subsystem = rd16(oh + 68);
    m->stack_reserve = rd32(oh + 72);
    m->stack_commit = rd32(oh + 76);
    m->heap_reserve = rd32(oh + 80);
    m->heap_commit = rd32(oh + 84);
    m->checksum = rd32(oh + 64);
    uint32_t ndirs = rd32(oh + 92);
    if (ndirs > 16)
        ndirs = 16;
    memset(m->dir, 0, sizeof(m->dir));
    for (uint32_t i = 0; i < ndirs; i++) {
        m->dir[i][0] = rd32(oh + 96 + i * 8);
        m->dir[i][1] = rd32(oh + 100 + i * 8);
    }
    int relocatable = m->dir[DIR_BASERELOC][1] != 0 && !(characteristics & IMAGE_FILE_RELOCS_STRIPPED);

    gp_t base = vm_alloc(image_base, size_of_image, PAGE_EXECUTE_READWRITE, MEM_IMAGE, 1);
    if (!base && relocatable)
        base = vm_alloc(0, size_of_image, PAGE_EXECUTE_READWRITE, MEM_IMAGE, 0);
    if (!base) {
        snprintf(err, errsize, "cannot load at %08x (%s)", image_base,
                 relocatable ? "no memory" : "the address is taken and the program has no relocations");
        close(fd);
        return -1;
    }
    m->base = base;
    m->size = size_of_image;
    uint8_t *img = GP(base);

    /* the headers, then each section's raw data */
    uint32_t hcopy = size_of_headers < sizeof(hdr) ? size_of_headers : sizeof(hdr);
    memcpy(img, hdr, hcopy);
    if (size_of_headers > sizeof(hdr)) {
        lseek(fd, 0, SEEK_SET);
        if (read(fd, img, size_of_headers) != (int)size_of_headers) {
            snprintf(err, errsize, "cannot read the headers");
            goto fail;
        }
    }
    for (int i = 0; i < nsections; i++) {
        const uint8_t *sh = hdr + sect_off + (uint32_t)i * 40;
        uint32_t vsize = rd32(sh + 8), va = rd32(sh + 12), rawsize = rd32(sh + 16), rawoff = rd32(sh + 20);
        if (va >= size_of_image)
            continue;
        uint32_t want = vsize ? vsize : rawsize;
        if (want > size_of_image - va)
            want = size_of_image - va;
        uint32_t n = rawsize < want ? rawsize : want;
        if (!n || rawoff >= (uint32_t)st.st_size)
            continue;
        if (n > (uint32_t)st.st_size - rawoff)
            n = (uint32_t)st.st_size - rawoff;
        lseek(fd, (off_t)rawoff, SEEK_SET);
        if (read(fd, img + va, n) != (int)n) {
            snprintf(err, errsize, "cannot read section %d", i);
            goto fail;
        }
    }
    close(fd);
    (void)section_align;
    if (base != image_base)
        relocate(m, base - image_base);
    uint32_t entry = rd32(oh + 16);
    m->entry = entry ? base + entry : 0;
    m->tls_index = -1;
    snprintf(m->path, sizeof(m->path), "%s", path);
    return 0;
fail:
    close(fd);
    vm_free(base, 0);
    m->base = 0;
    return -1;
}

/* ---- exports ---- */

gp_t pe_export(struct module *m, const char *name, int ordinal)
{
    uint32_t rva = m->dir[DIR_EXPORT][0], size = m->dir[DIR_EXPORT][1];
    if (!rva || !size || m->size < 40 || rva > m->size - 40)
        return 0;
    const uint8_t *img = GP(m->base);
    const uint8_t *ed = img + rva;
    uint32_t base_ord = rd32(ed + 16), nfuncs = rd32(ed + 20), nnames = rd32(ed + 24);
    uint32_t funcs = rd32(ed + 28), names = rd32(ed + 32), ords = rd32(ed + 36);
    if (funcs > m->size || nfuncs > (m->size - funcs) / 4 || names > m->size || nnames > (m->size - names) / 4 ||
        ords > m->size || nnames > (m->size - ords) / 2)
        return 0;
    uint32_t index = 0xFFFFFFFFu;
    if (name) {
        /* the names are sorted: binary search */
        uint32_t lo = 0, hi = nnames;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint32_t nrva = rd32(img + names + mid * 4);
            if (nrva >= m->size)
                return 0;
            int c = strcmp(name, (const char *)img + nrva);
            if (c == 0) {
                index = rd16(img + ords + mid * 2);
                break;
            }
            if (c < 0)
                hi = mid;
            else
                lo = mid + 1;
        }
        if (index == 0xFFFFFFFFu) {         /* not sorted after all? look once more */
            for (uint32_t i = 0; i < nnames; i++) {
                uint32_t nrva = rd32(img + names + i * 4);
                if (nrva < m->size && !strcmp(name, (const char *)img + nrva)) {
                    index = rd16(img + ords + i * 2);
                    break;
                }
            }
        }
    } else {
        if ((uint32_t)ordinal < base_ord)
            return 0;
        index = (uint32_t)ordinal - base_ord;
    }
    if (index >= nfuncs)
        return 0;
    uint32_t frva = rd32(img + funcs + index * 4);
    if (!frva)
        return 0;
    if (frva >= rva && frva - rva < size && frva < m->size) {   /* a forwarder: "DLL.Name" or "DLL.#12" */
        char fwd[256];
        snprintf(fwd, sizeof(fwd), "%s", (const char *)img + frva);
        char *dot = strrchr(fwd, '.');
        if (!dot)
            return 0;
        *dot++ = 0;
        char dll[260];
        snprintf(dll, sizeof(dll), "%s.DLL", fwd);
        if (*dot == '#')
            return loader_resolve(dll, NULL, atoi(dot + 1), NULL);
        return loader_resolve(dll, dot, 0, NULL);
    }
    return m->base + frva;
}

/* ---- imports ---- */

int pe_resolve_imports(struct module *m, char *err, size_t errsize)
{
    uint32_t rva = m->dir[DIR_IMPORT][0];
    if (!rva || !m->dir[DIR_IMPORT][1])
        return 0;
    uint8_t *img = GP(m->base);
    char from_dir[PATH_MAX];
    snprintf(from_dir, sizeof(from_dir), "%s", m->path);
    char *slash = strrchr(from_dir, '/');
    if (slash)
        *slash = 0;
    else
        strcpy(from_dir, ".");
    for (uint32_t d = rva; d + 20 <= m->size; d += 20) {
        uint32_t oft = rd32(img + d), name = rd32(img + d + 12), ft = rd32(img + d + 16);
        if (!name && !ft)
            break;
        if (name >= m->size || ft >= m->size)
            break;
        const char *dll = (const char *)img + name;
        /* the import holds one reference, so FreeLibrary cannot pull the
         * DLL out from under it; loader_resolve below takes none */
        char lerr[256];
        module_load(dll, from_dir, lerr, sizeof(lerr));
        uint32_t thunks = oft && oft < m->size ? oft : ft;
        for (uint32_t i = 0; thunks + i * 4 + 4 <= m->size; i++) {
            uint32_t e = rd32(img + thunks + i * 4);
            if (!e)
                break;
            gp_t addr;
            if (e & 0x80000000u) {
                addr = loader_resolve(dll, NULL, (int)(e & 0xFFFF), from_dir);
                if (!addr) {
                    char nm[32];
                    snprintf(nm, sizeof(nm), "#%u", e & 0xFFFF);
                    addr = api_missing_thunk(dll, nm);
                }
            } else {
                if (e + 2 >= m->size)
                    break;
                const char *fn = (const char *)img + e + 2;
                addr = loader_resolve(dll, fn, 0, from_dir);
                if (!addr)
                    addr = api_missing_thunk(dll, fn);
            }
            if (ft + i * 4 + 4 <= m->size)
                memcpy(img + ft + i * 4, &addr, 4);
        }
    }
    (void)err; (void)errsize;
    return 0;
}

/* ---- thread local storage ---- */

static int next_tls_index;

int pe_setup_tls(struct module *m)
{
    uint32_t rva = m->dir[DIR_TLS][0];
    if (!rva || !m->dir[DIR_TLS][1] || rva + 24 > m->size)
        return 0;
    const uint8_t *img = GP(m->base);
    const uint8_t *t = img + rva;
    uint32_t start = rd32(t), end = rd32(t + 4), index_at = rd32(t + 8), zero_fill = rd32(t + 16);
    uint32_t size = end > start ? end - start : 0;
    if (size > m->size || zero_fill > GUEST_TOP - m->size)     /* the block size cannot wrap */
        return -1;
    if (next_tls_index >= 64)
        return -1;
    int index = next_tls_index++;
    m->tls_index = index;
    /* absolute addresses in the image, wherever it was loaded */
    if (index_at >= m->base && index_at - m->base <= m->size - 4)
        memcpy(GP(index_at), &index, 4);
    gp_t block = vm_alloc_loader(size + zero_fill + 16);
    if (size && start >= m->base && start - m->base <= m->size - size)
        memcpy(GP(block), GP(start), size);
    uint32_t *array = GP(teb->tls_pointer);
    array[index] = block;
    return 0;
}

void pe_run_tls_callbacks(struct module *m, uint32_t reason)
{
    uint32_t rva = m->dir[DIR_TLS][0];
    if (!rva || !m->dir[DIR_TLS][1] || rva + 24 > m->size)
        return;
    const uint8_t *img = GP(m->base);
    uint32_t cbs = rd32(img + rva + 12);
    if (!cbs)
        return;
    if (cbs < m->base || cbs >= m->base + m->size)
        return;
    for (uint32_t i = 0; ; i++) {
        uint32_t cb = rd32(img + (cbs - m->base) + i * 4);
        if (!cb)
            break;
        wtrace("TLS callback %08x (%u)", cb, reason);
        call_guest3(cb, m->base, reason, 0);
    }
}

/* ---- resources ---- */

static uint16_t wlower16(uint16_t c)
{
    return c >= 'A' && c <= 'Z' ? (uint16_t)(c + 32) : c;
}

/* The entry of a directory at dir_rva matching an id or a name; its
 * OffsetToData, or 0. */
static uint32_t res_lookup(struct module *m, uint32_t dir_rva, uint32_t id, const uint16_t *name, int any)
{
    uint32_t root = m->dir[DIR_RESOURCE][0];
    const uint8_t *img = GP(m->base);
    if (dir_rva + 16 > m->size)
        return 0;
    const uint8_t *d = img + dir_rva;
    uint32_t nnamed = rd16(d + 12), nids = rd16(d + 14);
    for (uint32_t i = 0; i < nnamed + nids; i++) {
        const uint8_t *e = d + 16 + i * 8;
        if ((uint32_t)(e - img) + 8 > m->size)
            return 0;
        uint32_t key = rd32(e), off = rd32(e + 4);
        if (any)
            return off;
        if (key & 0x80000000u) {
            if (!name)
                continue;
            uint32_t soff = root + (key & 0x7FFFFFFFu);
            if (soff + 2 > m->size)
                continue;
            uint16_t len = rd16(img + soff);
            const uint16_t *s = (const uint16_t *)(img + soff + 2);
            int k = 0;
            while (k < len && name[k] && wlower16(name[k]) == wlower16(s[k]))
                k++;
            if (k == len && !name[k])
                return off;
        } else if (!name && key == id) {
            return off;
        }
    }
    return 0;
}

gp_t pe_find_resource(struct module *m, uint32_t type_id, const uint16_t *type_name,
                      uint32_t name_id, const uint16_t *name_name, uint32_t *size)
{
    uint32_t root = m->dir[DIR_RESOURCE][0];
    if (!root || !m->dir[DIR_RESOURCE][1])
        return 0;
    uint32_t t = res_lookup(m, root, type_id, type_name, 0);
    if (!(t & 0x80000000u))
        return 0;
    uint32_t n = res_lookup(m, root + (t & 0x7FFFFFFFu), name_id, name_name, 0);
    if (!(n & 0x80000000u))
        return 0;
    uint32_t l = res_lookup(m, root + (n & 0x7FFFFFFFu), 0, NULL, 1);
    if (l & 0x80000000u)
        return 0;
    uint32_t de = root + l;
    if (de + 16 > m->size)
        return 0;
    const uint8_t *img = GP(m->base);
    uint32_t data = rd32(img + de), dsize = rd32(img + de + 4);
    if (data >= m->size)
        return 0;
    if (size)
        *size = dsize;
    return m->base + data;
}
