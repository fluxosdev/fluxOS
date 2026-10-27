#ifndef WEBINTEGRITY_H
#define WEBINTEGRITY_H
#include <stddef.h>
/* Empty/unknown metadata is ignored. Supported hashes must match the strongest algorithm. */
int web_integrity(const char *metadata,const void *bytes,size_t length);
#endif
