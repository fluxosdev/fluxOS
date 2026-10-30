/*
 * Users and groups, from /etc/passwd and /etc/group when they exist.
 * fluxOS has one user; without the files it is "flux" (uid 0), whose
 * home is $HOME or /home/flux and whose shell is /bin/fsh.
 */
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct passwd pw_static;
static char pw_buf[512];
static FILE *pw_file;
static int pw_default_given;

/* "name:passwd:uid:gid:gecos:dir:shell" into pw, its strings in buf. */
static int parse_passwd(const char *line, struct passwd *pw, char *buf, size_t size)
{
    size_t len = strlen(line);
    if (len + 1 > size)
        return -1;
    memcpy(buf, line, len + 1);
    if (len && buf[len - 1] == '\n')
        buf[--len] = 0;
    char *f[7];
    int n = 0;
    char *p = buf;
    f[n++] = p;
    while (*p && n < 7) {
        if (*p == ':') {
            *p = 0;
            f[n++] = p + 1;
        }
        p++;
    }
    if (n < 7)
        return -1;
    pw->pw_name = f[0];
    pw->pw_passwd = f[1];
    pw->pw_uid = (uid_t)strtoul(f[2], NULL, 10);
    pw->pw_gid = (gid_t)strtoul(f[3], NULL, 10);
    pw->pw_gecos = f[4];
    pw->pw_dir = f[5];
    pw->pw_shell = f[6];
    return 0;
}

static int default_user(struct passwd *pw, char *buf, size_t size)
{
    const char *home = getenv("HOME");
    char line[300];
    snprintf(line, sizeof(line), "flux:x:0:0:fluxOS user:%s:/bin/fsh", home && *home ? home : "/home/flux");
    return parse_passwd(line, pw, buf, size);
}

void setpwent(void)
{
    if (pw_file)
        rewind(pw_file);
    pw_default_given = 0;
}

void endpwent(void)
{
    if (pw_file)
        fclose(pw_file);
    pw_file = NULL;
    pw_default_given = 0;
}

struct passwd *getpwent(void)
{
    char line[512];
    if (!pw_file)
        pw_file = fopen("/etc/passwd", "r");
    if (!pw_file) {
        if (pw_default_given)
            return NULL;
        pw_default_given = 1;
        return default_user(&pw_static, pw_buf, sizeof(pw_buf)) == 0 ? &pw_static : NULL;
    }
    while (fgets(line, sizeof(line), pw_file))
        if (line[0] != '#' && parse_passwd(line, &pw_static, pw_buf, sizeof(pw_buf)) == 0)
            return &pw_static;
    return NULL;
}

/* Look through the file, or fall back to the one user. */
static int lookup(const char *name, uid_t uid, int by_name, struct passwd *pw, char *buf, size_t size)
{
    FILE *f = fopen("/etc/passwd", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#' || parse_passwd(line, pw, buf, size) < 0)
                continue;
            if (by_name ? !strcmp(pw->pw_name, name) : pw->pw_uid == uid) {
                fclose(f);
                return 0;
            }
        }
        fclose(f);
    }
    if (default_user(pw, buf, size) < 0)
        return ERANGE;
    if (by_name ? !strcmp(pw->pw_name, name) || (getenv("USER") && !strcmp(getenv("USER"), name)) : uid == 0) {
        if (by_name)
            pw->pw_name = (char *)name;
        return 0;
    }
    return -1;
}

int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t size, struct passwd **out)
{
    int r = lookup(name, 0, 1, pw, buf, size);
    *out = r == 0 ? pw : NULL;
    return r > 0 ? r : 0;
}

int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t size, struct passwd **out)
{
    int r = lookup(NULL, uid, 0, pw, buf, size);
    *out = r == 0 ? pw : NULL;
    return r > 0 ? r : 0;
}

struct passwd *getpwnam(const char *name)
{
    struct passwd *out;
    getpwnam_r(name, &pw_static, pw_buf, sizeof(pw_buf), &out);
    return out;
}

struct passwd *getpwuid(uid_t uid)
{
    struct passwd *out;
    getpwuid_r(uid, &pw_static, pw_buf, sizeof(pw_buf), &out);
    return out;
}

/* ---- groups: just "root" (0) and "users" (100), unless /etc/group says more ---- */

static struct group gr_static;
static char gr_buf[512];
static char *gr_members[16];
static FILE *gr_file;
static int gr_default;

static int parse_group(const char *line, struct group *gr)
{
    size_t len = strlen(line);
    if (len + 1 > sizeof(gr_buf))
        return -1;
    memcpy(gr_buf, line, len + 1);
    if (len && gr_buf[len - 1] == '\n')
        gr_buf[--len] = 0;
    char *f[4];
    int n = 0;
    char *p = gr_buf;
    f[n++] = p;
    while (*p && n < 4) {
        if (*p == ':') {
            *p = 0;
            f[n++] = p + 1;
        }
        p++;
    }
    if (n < 4)
        return -1;
    int m = 0;
    for (char *s = strtok(f[3], ","); s && m < 15; s = strtok(NULL, ","))
        gr_members[m++] = s;
    gr_members[m] = NULL;
    gr->gr_name = f[0];
    gr->gr_passwd = f[1];
    gr->gr_gid = (gid_t)strtoul(f[2], NULL, 10);
    gr->gr_mem = gr_members;
    return 0;
}

static struct group *default_group(int index)
{
    static const char *const lines[] = { "root:x:0:flux", "users:x:100:flux" };
    if (index < 0 || index > 1 || parse_group(lines[index], &gr_static) < 0)
        return NULL;
    return &gr_static;
}

void setgrent(void)
{
    if (gr_file)
        rewind(gr_file);
    gr_default = 0;
}

void endgrent(void)
{
    if (gr_file)
        fclose(gr_file);
    gr_file = NULL;
    gr_default = 0;
}

struct group *getgrent(void)
{
    char line[512];
    if (!gr_file)
        gr_file = fopen("/etc/group", "r");
    if (!gr_file)
        return default_group(gr_default++);
    while (fgets(line, sizeof(line), gr_file))
        if (line[0] != '#' && parse_group(line, &gr_static) == 0)
            return &gr_static;
    return NULL;
}

struct group *getgrnam(const char *name)
{
    struct group *g;
    setgrent();
    while ((g = getgrent()))
        if (!strcmp(g->gr_name, name))
            break;
    endgrent();
    return g;
}

struct group *getgrgid(gid_t gid)
{
    struct group *g;
    setgrent();
    while ((g = getgrent()))
        if (g->gr_gid == gid)
            break;
    endgrent();
    return g;
}

int initgroups(const char *user, gid_t group)
{
    (void)user;
    (void)group;
    return 0;
}
