/* Clean file-data blocks. All calls hold the VFS mutex, including driver I/O.
 * Keys use whole disks and absolute LBAs so partition/raw aliases stay coherent. */
#ifndef READCACHE_H
#define READCACHE_H
#include "blkdev.h"
void readcache_init(void);
int readcache_read(struct blockdev *disk, uint32_t lba, uint32_t count, void *out);
void readcache_written(struct blockdev *disk, uint32_t lba, uint32_t count, const void *data);
void readcache_forget(struct blockdev *disk, uint32_t lba, uint32_t count);
void readcache_discard(struct blockdev *disk);
#endif
