/* What the GL library takes from fluxOS's libc that the host's lacks
 * (tests/gl-soft.c defines it); included ahead of every file. */
#include <stddef.h>

void memset32(void *dst, unsigned v, size_t n);
