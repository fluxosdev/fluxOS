/* Use host POSIX types; pe.c only needs this address constant from the OS ABI. */
#define FLUX_ABI_H
#define WIN_SEG_BASE 0x40000000u
#include <assert.h>
#include "../user/win32/pe.c"

static int allocations;
gp_t vm_alloc(gp_t a, uint32_t s, uint32_t p, uint32_t t, int f)
{
    (void)a; (void)s; (void)p; (void)t; (void)f;
    allocations++;
    return 0;
}
int vm_free(gp_t a, uint32_t s) { (void)a; (void)s; return 0; }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* An image in the window (host address WIN_SEG_BASE + base) for the lookups. */
#include <sys/mman.h>
#define TBASE 0x400000u
#define TSIZE 0x2000u
static struct module tm;
static int callbacks;
uint32_t call_guest3(gp_t fn, uint32_t a, uint32_t b, uint32_t c) { (void)fn; (void)a; (void)b; (void)c; callbacks++; return 0; }
void wtrace(const char *fmt, ...) { (void)fmt; }
/* every forwarder leads back to the same module */
gp_t loader_resolve(const char *dll, const char *name, int ordinal, const char *from_dir)
{
    (void)dll; (void)from_dir;
    return pe_export(&tm, name, ordinal);
}
static void image_checks(void)
{
    uint8_t *img = mmap((void *)(uintptr_t)(WIN_SEG_BASE + TBASE), TSIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(img == (uint8_t *)(uintptr_t)(WIN_SEG_BASE + TBASE));
    tm.base = TBASE; tm.size = TSIZE;
    uint32_t size = 0;
    /* resources: a root at 0x1000 whose offsets wrap or point past the image */
    uint8_t *root = img + 0x1000;
    tm.dir[DIR_RESOURCE][0] = 0x1000; tm.dir[DIR_RESOURCE][1] = 0x100;
    wr16(root + 14, 1); wr32(root + 16, 3); wr32(root + 20, 0x80000000u | 0x7FFFFF00u);
    assert(!pe_find_resource(&tm, 3, NULL, 1, NULL, &size));
    wr32(root + 20, 0x80000000u | 0x20);
    wr16(root + 0x20 + 14, 1); wr32(root + 0x30, 1); wr32(root + 0x34, 0x80000000u | 0x40);
    wr16(root + 0x40 + 14, 1); wr32(root + 0x50, 0x409); wr32(root + 0x54, 0xFFFFFFF0u);
    assert(!pe_find_resource(&tm, 3, NULL, 1, NULL, &size));
    wr32(root + 0x54, 0x60);
    wr32(root + 0x60, 0x1F00); wr32(root + 0x64, 0x10000);   /* bigger than the image */
    assert(pe_find_resource(&tm, 3, NULL, 1, NULL, &size) == TBASE + 0x1F00 && size == 0x100);
    /* a named type whose name runs past the image */
    wr16(root + 12, 1); wr16(root + 14, 0); wr32(root + 16, 0x80000000u | 0xFF0); wr16(root + 0xFF0, 0xFFFF);
    static const uint16_t name[] = { 'A', 0 };
    assert(!pe_find_resource(&tm, 0, name, 1, NULL, &size));
    /* TLS: a directory whose end wraps, then a callback list without its terminator */
    tm.dir[DIR_TLS][0] = 0xFFFFFFF0u; tm.dir[DIR_TLS][1] = 24;
    pe_run_tls_callbacks(&tm, 1);
    assert(!callbacks);
    tm.dir[DIR_TLS][0] = 0x100;
    wr32(img + 0x10C, TBASE + 0x1FF8); wr32(img + 0x1FF8, 1); wr32(img + 0x1FFC, 2);
    pe_run_tls_callbacks(&tm, 1);
    assert(callbacks == 2);
    /* an export forwarded to itself ends instead of recursing for ever */
    tm.dir[DIR_EXPORT][0] = 0x200; tm.dir[DIR_EXPORT][1] = 0x100;
    uint8_t *ed = img + 0x200;
    wr32(ed + 16, 1); wr32(ed + 20, 1); wr32(ed + 24, 0); wr32(ed + 28, 0x240); wr32(ed + 32, 0x250); wr32(ed + 36, 0x250);
    wr32(img + 0x240, 0x260); memcpy(img + 0x260, "SELF.#1", 8);
    assert(!pe_export(&tm, NULL, 1));
    munmap(img, TSIZE);
}
static void save(int fd, const uint8_t *data, size_t size)
{
    assert(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0);
    assert(write(fd, data, size) == (ssize_t)size);
}
static void bad_map(const char *path)
{
    struct module m = {0}; char err[128];
    allocations = 0;
    assert(pe_map(path, &m, err, sizeof(err)) < 0);
    assert(!allocations);
}
int main(void)
{
    char path[] = "/tmp/flux-pe-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    uint8_t data[512] = {'M', 'Z'}, hdr[4096];
    uint32_t nt; char err[128]; uint16_t subsystem; int dll;
    wr32(data + 60, 64); memcpy(data + 64, "PE\0\0", 4);
    wr16(data + 68, 0x14c); wr16(data + 84, 224);
    uint8_t *oh = data + 88;
    wr16(oh, 0x10b); wr32(oh + 56, 4096); wr32(oh + 60, 512);
    wr16(oh + 68, 3); wr32(oh + 92, 16);
    save(fd, data, sizeof(data));
    assert(pe_file_info(path, &subsystem, &dll) == 0 && subsystem == 3 && !dll);
    /* Every partial header must fail, including a complete signature with
     * missing COFF/optional fields and missing directory entries. */
    for (size_t n = 0; n < 312; n++) {
        save(fd, data, n);
        assert(read_headers(fd, hdr, sizeof(hdr), &nt, err, sizeof(err)) < 0);
    }
    wr32(data + 60, 0xfffffff0u); save(fd, data, sizeof(data)); bad_map(path);
    wr32(data + 60, 64);
    wr16(data + 84, 95); save(fd, data, sizeof(data)); bad_map(path);
    wr16(data + 84, 224);
    wr32(oh + 92, 17); save(fd, data, sizeof(data)); bad_map(path);
    wr32(oh + 92, 16);
    wr16(data + 70, 1); save(fd, data, 312); bad_map(path);
    wr16(data + 70, 0);
    wr32(oh + 16, 4096); save(fd, data, sizeof(data)); bad_map(path);
    wr32(oh + 16, 0);
    wr32(oh + 60, 513); save(fd, data, sizeof(data)); bad_map(path);
    close(fd); unlink(path);
    image_checks();
    puts("Win32 PE header regressions passed");
}
