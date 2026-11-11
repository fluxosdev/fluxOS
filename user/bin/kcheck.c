/*
 * kcheck - check that a kernel image is one Limine can start
 *
 *   kcheck [-q] KERNEL
 *
 * The Limine boot protocol wants a 64-bit ELF executable for x86-64 whose
 * loadable segments are all in the top 2 GiB of the address space, and
 * finds what the kernel asks of it by the requests in the image: the base
 * revision's two magic words, and each request's (kernel/core/limine.h,
 * bootinfo.c). kcheck looks for the image's form, the base revision, and
 * the three requests fluxOS cannot start without (the memory map, the
 * direct map's offset and the image's own address), and says what it
 * found; -q says nothing unless something is wrong.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KERNEL_BASE 0xFFFFFFFF80000000ull

static const char *name;

static void die(const char *what)
{
    fprintf(stderr, "kcheck: %s: %s\n", name, what);
    exit(1);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

/* Where the words are in the file (on an 8-byte boundary, as Limine wants them), or -1. */
static long find_words(const uint8_t *data, long size, const uint64_t *words, int n)
{
    for (long off = 0; off + n * 8 <= size; off += 8) {
        int k = 0;
        while (k < n && rd64(data + off + k * 8) == words[k])
            k++;
        if (k == n)
            return off;
    }
    return -1;
}

int main(int argc, char **argv)
{
    int quiet = 0, arg = 1;
    if (arg < argc && !strcmp(argv[arg], "-q")) {
        quiet = 1;
        arg++;
    }
    if (arg + 1 != argc) {
        fprintf(stderr, "usage: kcheck [-q] KERNEL\n");
        return 2;
    }
    name = argv[arg];
    FILE *f = fopen(name, "rb");
    if (!f)
        die("cannot open it");
    if (fseek(f, 0, SEEK_END) < 0)
        die("cannot read it");
    long size = ftell(f);
    rewind(f);
    uint8_t *data = size > 0 ? malloc((size_t)size) : NULL;
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size)
        die("cannot read it");
    fclose(f);

    if (size < 64 || memcmp(data, "\177ELF", 4) != 0)
        die("not an ELF file");
    if (data[4] != 2 || data[5] != 1)
        die("not a 64-bit little-endian ELF file (Limine starts no 32-bit kernels)");
    if (rd16(data + 16) != 2)
        die("not an executable (ET_EXEC): it is linked to run where linker.ld puts it");
    if (rd16(data + 18) != 62)
        die("not for x86-64");
    uint64_t entry = rd64(data + 24), phoff = rd64(data + 32);
    unsigned phentsize = rd16(data + 54), phnum = rd16(data + 56);
    if (phentsize < 56 || phoff > (uint64_t)size || phnum * (uint64_t)phentsize > (uint64_t)size - phoff)
        die("its program headers are not in the file");
    unsigned loads = 0;
    uint64_t low = ~0ull, high = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = data + phoff + i * phentsize;
        if (rd32(ph) != 1)                      /* PT_LOAD */
            continue;
        uint64_t off = rd64(ph + 8), vaddr = rd64(ph + 16), filesz = rd64(ph + 32), memsz = rd64(ph + 40);
        if (vaddr < KERNEL_BASE || vaddr + memsz < vaddr)
            die("a segment is below the top 2 GiB of the address space, where Limine loads kernels");
        if (off > (uint64_t)size || filesz > (uint64_t)size - off || filesz > memsz)
            die("a segment is not all in the file");
        if (vaddr < low)
            low = vaddr;
        if (vaddr + memsz > high)
            high = vaddr + memsz;
        loads++;
    }
    if (!loads)
        die("it has nothing to load");
    if (entry < low || entry >= high)
        die("its entry point is outside what is loaded");

    static const uint64_t base_revision[2] = { 0xf9562b2d5c95a6c8ull, 0x6a7b384944536bdcull };
    long at = find_words(data, size, base_revision, 2);
    if (at < 0)
        die("no Limine base revision in it: it does not speak the Limine boot protocol");
    uint64_t revision = at + 24 <= size ? rd64(data + at + 16) : 0;

    static const struct { const char *what; uint64_t id[4]; } need[] = {
        { "memory map", { 0xc7b1dd30df4c8b88ull, 0x0a82e883a194f07bull, 0x67cf3d9d378a806full, 0xe304acdfc50c3c62ull } },
        { "direct map", { 0xc7b1dd30df4c8b88ull, 0x0a82e883a194f07bull, 0x48dcf1cb8ad2b852ull, 0x63984e959a98244bull } },
        { "image address", { 0xc7b1dd30df4c8b88ull, 0x0a82e883a194f07bull, 0x71ba76863cc55f63ull, 0xb2644a48c516a487ull } },
    };
    for (unsigned i = 0; i < sizeof(need) / sizeof(need[0]); i++)
        if (find_words(data, size, need[i].id, 4) < 0) {
            fprintf(stderr, "kcheck: %s: no Limine request for the %s in it\n", name, need[i].what);
            return 1;
        }

    if (!quiet)
        printf("%s: 64-bit ELF kernel for Limine (base revision %llu), %u segments at %#llx-%#llx, entry %#llx\n",
               name, (unsigned long long)revision, loads, (unsigned long long)low, (unsigned long long)high,
               (unsigned long long)entry);
    return 0;
}
