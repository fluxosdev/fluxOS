#ifndef JSNET_H
#define JSNET_H
#include "js.h"
struct jsnet;
struct jsnet *jsnet_new(struct js *J, const char *base, const char *origin, void *ctx,
                        void (*emit)(void *, jsval, const char *, jsval, int, const char *, int),
                        int (*cookies)(void *, const char *, char *, int));
void jsnet_free(struct jsnet *net);
void jsnet_poll(struct jsnet *net);
int jsnet_wait(struct jsnet *net);
#endif
