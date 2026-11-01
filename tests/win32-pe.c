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
    puts("Win32 PE header regressions passed");
}
