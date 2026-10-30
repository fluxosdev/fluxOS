/* Clean file-data blocks. Public calls hold the VFS mutex, including driver I/O.
 * A private spinlock lets PMM reclaim pages independently of the VFS mutex;
 * driver I/O never holds it. Partition/raw aliases share whole-disk LBA keys. */
#ifndef READCACHE_H
#define READCACHE_H
#include "blkdev.h"
struct readcache_stream { struct blockdev *disk; uint32_t end; int valid; };
/* Scope to a shared open-file description while the VFS mutex is held. */
struct readcache_stream *readcache_use_stream(struct readcache_stream *stream);
void readcache_init(void);
int readcache_read(struct blockdev *disk, uint32_t lba, uint32_t count, void *out);
void readcache_written(struct blockdev *disk, uint32_t lba, uint32_t count, const void *data);
void readcache_forget(struct blockdev *disk, uint32_t lba, uint32_t count);
void readcache_discard(struct blockdev *disk);
#endif
