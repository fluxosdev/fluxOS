#ifndef _PWD_H
#define _PWD_H

#include <sys/types.h>

/* From /etc/passwd when it exists; fluxOS has one user either way. */
struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);
int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t size, struct passwd **out);
int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t size, struct passwd **out);
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);

#endif
