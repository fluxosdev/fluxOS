/*
 * ZIP and 7z archives (libc: archive.c, arc_codec.c): listing them and
 * extracting what they hold.
 *
 * ZIP: stored, Deflate and LZMA entries, Zip64, UTF-8 and CP437 names.
 * 7z: Copy, LZMA, LZMA2 and Deflate, with the BCJ (x86) and Delta
 * filters, solid blocks and compressed headers. Encrypted entries and
 * other methods (BZip2, PPMd, BCJ2...) are listed but cannot be
 * extracted. Sizes are 32-bit: archives and files up to 2 GiB.
 */
#ifndef _ARCHIVE_H
#define _ARCHIVE_H

#include <stddef.h>
#include <stdint.h>

enum { ARC_ZIP = 1, ARC_7Z = 2 };

struct arc_entry {
    char *name;                 /* the path inside the archive, '/' separated, UTF-8 */
    uint32_t size;              /* unpacked */
    uint32_t csize;             /* packed (7z: 0, the data is in shared blocks) */
    uint32_t mtime;             /* Unix time, 0 if unknown */
    uint32_t crc;
    uint32_t mode;              /* Unix permissions if the archive has them, else 0 */
    uint8_t is_dir;
    uint8_t has_crc;
    uint8_t supported;          /* its method can be extracted */
    uint8_t encrypted;
    char method[16];            /* "Deflate", "LZMA2:16m", ... */
    /* where its data is (archive.c) */
    uint32_t offset;            /* ZIP: the local header; 7z: within its folder's output */
    uint16_t zmethod, zflags;
    int folder;                 /* 7z: the block it is in, -1 for none */
};

struct archive;

/* Open a ZIP or 7z file (told apart by their signatures); NULL with err set. */
struct archive *arc_open(const char *path, char *err, int errsize);
void arc_close(struct archive *a);
/* Private directory-only snapshots for Archive Manager's worker. The imported
 * archive supports listing only; extraction must reopen the original path. */
int arc_write_index(const struct archive *a, int fd);
struct archive *arc_read_index(int fd, char *err, int errsize);
int arc_type(const struct archive *a);                  /* ARC_ZIP or ARC_7Z */
int arc_count(const struct archive *a);
const struct arc_entry *arc_entry(const struct archive *a, int i);
int arc_solid(const struct archive *a);                 /* 7z with files sharing a block */

/* The callbacks extraction makes. Each returns 0 to go on, nonzero to stop. */
struct arc_events {
    void *ctx;
    int (*begin)(void *ctx, int i, const char *path);   /* entry i is being written to path */
    int (*progress)(void *ctx, uint32_t done, uint32_t total);     /* bytes, over all entries */
    void (*failed)(void *ctx, int i, const char *why);  /* entry i could not be extracted */
    void (*skipped)(void *ctx, int i, const char *path);        /* path exists (no ARC_OVERWRITE) */
};

enum {
    ARC_OVERWRITE = 1,          /* replace files that are there (else they are skipped) */
    ARC_FLAT = 2,               /* every file straight into dest, without its folders */
    ARC_TEST = 4,               /* only check the data (CRCs); write nothing */
};

/* Extract the entries want[i] says (NULL: all) under the folder dest.
 * Returns how many failed (0 if all went well), -1 if cancelled. */
int arc_extract(struct archive *a, const uint8_t *want, const char *dest, int flags,
                const struct arc_events *ev);
/* Write entry i's data to fd. 0, or -1 with err set. */
int arc_extract_fd(struct archive *a, int i, int fd, char *err, int errsize);

/* A path that is safe to make under a folder: no leading /, no "..",
 * backslashes as /. Returns its length (0 if nothing is left). */
int arc_safe_path(const char *name, char *out, int size);
/* Make every folder on the way to path (not path itself). */
int arc_make_parents(const char *path);

/* Does an entry name match what was asked for on a command line: the same name, a
 * folder it is in, or a pattern with * and ? (where * does not cross a /)? */
int arc_name_match(const char *pattern, const char *name);

uint32_t arc_crc32(uint32_t crc, const void *data, size_t n);  /* start with 0 */

#endif
