#ifndef WEBBLOB_H
#define WEBBLOB_H
#include <stddef.h>
/* Object URLs retain immutable bytes until revoked or their document is destroyed. */
const char *web_blob_create(void *owner,const char *origin,const void *bytes,size_t len,const char *type);
void web_blob_revoke(void *owner,const char *url);
void web_blob_free(void *owner);
/* Returns copied bytes; callers free *bytes. Access requires the creator's origin. */
int web_blob_read(void *owner,const char *origin,const char *url,unsigned char **bytes,size_t *len,char type[128]);
#endif
