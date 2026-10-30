/* File/pipe host tests isolate the VFS and do not link storage. The block test
 * supplies the real implementation; the guest always uses production code. */
struct readcache_stream;
__attribute__((weak)) struct readcache_stream *readcache_use_stream(struct readcache_stream *stream)
{ (void)stream; return 0; }
