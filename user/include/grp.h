#ifndef _GRP_H
#define _GRP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
int initgroups(const char *user, gid_t group);

#ifdef __cplusplus
}
#endif

#endif
