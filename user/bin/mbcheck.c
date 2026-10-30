/*
 * mbcheck - check that a kernel is one GRUB can start
 *
 *   mbcheck [-q] KERNEL...
 *
 * What "make check" asks of fluxos.kernel, in place of grub-file
 * --is-x86-multiboot, and what the kernel's linker script promises:
 *
 *  - a Multiboot (version 1) header in the first 8192 bytes, on a 4-byte
 *    boundary: the magic 0x1BADB002, flags, and a checksum that makes the
 *    three add up to zero; no required feature (flags bits 0-15) other
 *    than page alignment, memory information and a video mode, which are
 *    all a Multiboot loader is bound to provide; with bit 16 (the a.out
 *    kludge) the load addresses after it are consistent;
 *  - an ELF32 executable for the 386, with exactly one PT_LOAD segment
 *    (the linker script's PHDRS), its file part within the file and no
 *    larger than its memory part, and the entry point inside it.
 *
 * Each kernel that passes is reported as such (unless -q); the exit
 * status is 1 if any fails.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MB_MAGIC     0x1BADB002u
#define MB_SEARCH    8192
#define MB_KNOWN     0x00000007u        /* page align, memory info, video mode */
#define MB_AOUT      0x00010000u

#define PT_LOAD 1

static int quiet;

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static int fail(const char *path, const char *why)
{
    fprintf(stderr, "mbcheck: %s: %s\n", path, why);
    return 1;
}

static int check(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    struct stat st;
    if (fstat(fileno(f), &st) < 0 || st.st_size < 52) {
        fclose(f);
        return fail(path, "too short to be a kernel");
    }
    uint32_t size = (uint32_t)st.st_size;
    uint8_t *k = malloc(size);
    if (!k || fread(k, 1, size, f) != size) {
        fclose(f);
        free(k);
        return fail(path, "cannot read it");
    }
    fclose(f);
    int bad = 0;

    /* the Multiboot header */
    uint32_t at = 0;
    int found = 0;
    for (; at + 12 <= size && at < MB_SEARCH; at += 4)
        if (rd32(k + at) == MB_MAGIC) {
            found = 1;
            break;
        }
    if (!found) {
        bad = fail(path, "no Multiboot header in the first 8192 bytes");
    } else {
        uint32_t flags = rd32(k + at + 4), sum = rd32(k + at + 8);
        if (MB_MAGIC + flags + sum != 0)
            bad = fail(path, "the Multiboot header's checksum is wrong");
        if (flags & 0xFFFF & ~MB_KNOWN)
            bad = fail(path, "the Multiboot header asks for a feature loaders need not have");
        if (flags & MB_AOUT) {
            if (at + 32 > size) {
                bad = fail(path, "the Multiboot header's addresses are cut off");
            } else {
                uint32_t header = rd32(k + at + 12), load = rd32(k + at + 16);
                uint32_t load_end = rd32(k + at + 20), bss_end = rd32(k + at + 24), entry = rd32(k + at + 28);
                if (load > header || header - load > at || (load_end && load_end < load) ||
                    (bss_end && load_end && bss_end < load_end) || entry < load ||
                    (load_end && entry >= (bss_end ? bss_end : load_end)))
                    bad = fail(path, "the Multiboot header's addresses do not fit together");
            }
        }
    }

    /* the ELF layout */
    if (memcmp(k, "\177ELF", 4) || k[4] != 1 || k[5] != 1) {
        bad = fail(path, "not a 32-bit little-endian ELF file");
    } else if (rd16(k + 16) != 2 || rd16(k + 18) != 3) {
        bad = fail(path, "not an executable for the 386");
    } else {
        uint32_t entry = rd32(k + 24), phoff = rd32(k + 28);
        uint32_t phentsize = rd16(k + 42), phnum = rd16(k + 44);
        if (phentsize < 32 || phoff > size || (uint64_t)phnum * phentsize > size - phoff) {
            bad = fail(path, "its program headers are cut off");
        } else {
            int loads = 0, entry_inside = 0;
            for (uint32_t i = 0; i < phnum; i++) {
                const uint8_t *ph = k + phoff + i * phentsize;
                if (rd32(ph) != PT_LOAD)
                    continue;
                loads++;
                uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), filesz = rd32(ph + 16);
                uint32_t memsz = rd32(ph + 20);
                if (off > size || filesz > size - off)
                    bad = fail(path, "a PT_LOAD segment runs past the end of the file");
                if (filesz > memsz)
                    bad = fail(path, "a PT_LOAD segment has more file than memory");
                if (entry >= vaddr && entry - vaddr < memsz)
                    entry_inside = 1;
            }
            if (loads != 1)
                bad = fail(path, loads ? "more than one PT_LOAD segment" : "no PT_LOAD segment");
            else if (!entry_inside)
                bad = fail(path, "the entry point is outside its PT_LOAD segment");
        }
    }
    free(k);
    if (!bad && !quiet)
        printf("%s: valid multiboot kernel\n", path);
    return bad;
}

int main(int argc, char **argv)
{
    int i = 1, status = 0;
    if (i < argc && !strcmp(argv[i], "-q")) {
        quiet = 1;
        i++;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: mbcheck [-q] KERNEL...\n");
        return 2;
    }
    for (; i < argc; i++)
        status |= check(argv[i]);
    return status;
}
