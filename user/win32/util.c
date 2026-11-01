/* Strings, paths, logging: helpers shared by the whole loader. */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "win32.h"

void wlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "winrun: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void wtrace(const char *fmt, ...)
{
    if (!api_trace)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void wdie(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "winrun: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    gui_exit();
    exit(1);
}

/* ---- UTF-16 ---- */

int wstrlen(const uint16_t *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

void wstrcpy(uint16_t *d, const uint16_t *s)
{
    while ((*d++ = *s++))
        ;
}

int wstrcmp(const uint16_t *a, const uint16_t *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (int)*a - (int)*b;
}

static uint16_t wlower(uint16_t c)
{
    return c >= 'A' && c <= 'Z' ? (uint16_t)(c + 32) : c;
}

int wstrcasecmp(const uint16_t *a, const uint16_t *b)
{
    while (*a && wlower(*a) == wlower(*b))
        a++, b++;
    return (int)wlower(*a) - (int)wlower(*b);
}

int ansi_to_wide(const char *s, int n, uint16_t *out, int outlen)
{
    int term = n < 0, i;
    if (term)
        n = (int)strlen(s) + 1;
    for (i = 0; i < n && i < outlen; i++)
        out[i] = (uint8_t)s[i];
    if (term && i < n && outlen > 0)    /* truncated: keep it a string */
        out[outlen - 1] = 0;
    return i;
}

int wide_to_ansi(const uint16_t *s, int n, char *out, int outlen)
{
    int term = n < 0, i;
    if (term)
        n = wstrlen(s) + 1;
    for (i = 0; i < n && i < outlen; i++)
        out[i] = s[i] < 256 ? (char)s[i] : '?';
    if (term && i < n && outlen > 0)    /* truncated: keep it a string */
        out[outlen - 1] = 0;
    return i;
}

int utf8_to_wide(const char *s, int n, uint16_t *out, int outlen)
{
    if (n < 0)
        n = (int)strlen(s) + 1;
    int o = 0;
    for (int i = 0; i < n; ) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3F); len = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3F); len = 4; }
        else { cp = 0xFFFD; len = 1; }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            if (o + 1 < outlen) {
                out[o++] = (uint16_t)(0xD800 + (cp >> 10));
                out[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            } else {
                o += 2;
            }
        } else {
            if (o < outlen)
                out[o] = (uint16_t)cp;
            o++;
        }
    }
    return o;
}

int wide_to_utf8(const uint16_t *s, int n, char *out, int outlen)
{
    if (n < 0)
        n = wstrlen(s) + 1;
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        char tmp[4];
        int len;
        if (cp < 0x80) { tmp[0] = (char)cp; len = 1; }
        else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); len = 2; }
        else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); len = 3; }
        else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); len = 4; }
        for (int k = 0; k < len; k++, o++)
            if (o < outlen)
                out[o] = tmp[k];
    }
    return o;
}

char *wtmp_n(const uint16_t *w, int n)
{
    static char bufs[8][1024];
    static int next;
    char *b = bufs[next++ & 7];
    if (n < 0)
        n = wstrlen(w);
    int i;
    for (i = 0; i < n && i < 1023; i++)
        b[i] = w[i] < 256 ? (char)w[i] : '?';
    b[i] = 0;
    return b;
}

const char *wtmp(gp_t wstr)
{
    if (!wstr)
        return NULL;
    return wtmp_n(GP(wstr), -1);
}

void str_upper(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z')
            *s -= 32;
}

void str_lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s += 32;
}

/* ---- paths ----
 * C: is a Wine-style prefix rooted at /winapps. Z: exposes the Unix root
 * for native launches and file dialogs; removable drives retain their mappings.
 * Dot components are normalized within a drive, never above its mount root.
 */
static const char *drive_root(char letter,char *buf)
{
    switch(letter) {
    case 'C': return "/winapps";
    case 'Z': return "";
    case 'A': return "/mnt/fd0";
    case 'B': return "/mnt/fd1";
    case 'D': return "/mnt/cdrom";
    default: snprintf(buf,16,"/mnt/%c",letter);return buf;
    }
}
static int under_root(const char *path,const char *root)
{
    size_t n=strlen(root);return !strncmp(path,root,n) && (!path[n] || path[n]=='/');
}
int path_to_host(const char *win,char *out)
{
    if(!win || strlen(win)>=PATH_MAX-16) return -1;
    const char *p=win;char drive='C',rootbuf[16],cwd[PATH_MAX],relative[PATH_MAX],tmp[PATH_MAX];
    if(!strncmp(p,"\\\\?\\",4) || !strncmp(p,"\\\\.\\",4)) p+=4;
    int explicit_drive=p[0] && p[1]==':';
    if(explicit_drive) {drive=p[0]&~0x20;if(drive<'A' || drive>'Z') return -1;p+=2;}
    if(!getcwd(cwd,sizeof(cwd))) strcpy(cwd,"/winapps");
    if(!explicit_drive) {
        char current[MAX_PATH];path_to_win(cwd,current,sizeof(current));drive=current[0];
    }
    const char *root=drive_root(drive,rootbuf);relative[0]=0;
    if(*p!='\\' && *p!='/') {
        if((!root[0] && drive=='Z') || under_root(cwd,root))
            snprintf(relative,sizeof(relative),"%s",cwd+strlen(root));
    }
    int n=snprintf(tmp,sizeof(tmp),"%s/%s",relative,p);
    if(n<0 || n>=(int)sizeof(tmp)) return -1;
    for(char *q=tmp;*q;q++) if(*q=='\\') *q='/';
    char *parts[PATH_MAX/2];int count=0;
    for(char *q=tmp;*q;) {
        while(*q=='/') q++;
        if(!*q) break;
        char *part=q;while(*q && *q!='/') q++;if(*q) *q++=0;
        if(!strcmp(part,".")) continue;
        if(!strcmp(part,"..")) {if(count) count--;continue;}
        parts[count++]=part;
    }
    n=snprintf(out,PATH_MAX,"%s",root);
    for(int i=0;i<count;i++) {
        int add=snprintf(out+n,PATH_MAX-n,"/%s",parts[i]);
        if(add<0 || add>=PATH_MAX-n) return -1;
        n+=add;
    }
    if(!n) strcpy(out,"/");
    return 0;
}
void path_to_win(const char *host,char *out,size_t size)
{
    if(!size) return;
    char absolute[PATH_MAX];
    if(host[0]!='/') {
        char cwd[PATH_MAX];if(!getcwd(cwd,sizeof(cwd))) strcpy(cwd,"/winapps");
        snprintf(absolute,sizeof(absolute),"%s/%s",cwd,host);host=absolute;
    }
    const char *rest=host,*drive="Z:";
    if(under_root(host,"/winapps")) {drive="C:";rest=host+8;}
    else if(under_root(host,"/mnt/fd0")) {drive="A:";rest=host+8;}
    else if(under_root(host,"/mnt/fd1")) {drive="B:";rest=host+8;}
    else if(under_root(host,"/mnt/cdrom")) {drive="D:";rest=host+10;}
    else if(!strncmp(host,"/mnt/",5) && host[5]>='A' && host[5]<='Y' && (!host[6] || host[6]=='/')) {
        char name[3]={host[5],':',0};snprintf(out,size,"%s%s",name,host+6);for(char *p=out;*p;p++) if(*p=='/') *p='\\';return;
    }
    snprintf(out,size,"%s%s%s",drive,*rest ? "":"\\",rest);
    for(char *p=out;*p;p++) if(*p=='/') *p='\\';
}

/* The exact spelling of path's components, if the exact one is not there
 * but a case-insensitive match is. 0 if path (possibly changed) exists. */
int path_fix_case(char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return 0;
    char work[PATH_MAX];
    snprintf(work, sizeof(work), "%s", path);
    char cur[PATH_MAX];
    int absolute = work[0] == '/';
    strcpy(cur, absolute ? "/" : ".");
    int cn = absolute ? 1 : 1;
    char out[PATH_MAX];
    strcpy(out, absolute ? "/" : "");
    int on = (int)strlen(out);
    for (char *s = strtok(work, "/"); s; s = strtok(NULL, "/")) {
        char cand[PATH_MAX];
        snprintf(cand, sizeof(cand), "%s%s%s", cur, cur[cn - 1] == '/' ? "" : "/", s);
        if (stat(cand, &st) < 0) {
            DIR *d = opendir(cur);
            if (!d)
                return -1;
            struct dirent *de;
            int found = 0;
            while ((de = readdir(d))) {
                if (!strcasecmp(de->d_name, s)) {
                    s = de->d_name;
                    snprintf(cand, sizeof(cand), "%s%s%s", cur, cur[cn - 1] == '/' ? "" : "/", s);
                    found = 1;
                    break;
                }
            }
            if (found)
                on += snprintf(out + on, sizeof(out) - (size_t)on, "%s%s", on && out[on - 1] != '/' ? "/" : "", s);
            closedir(d);
            if (!found)
                return -1;
        } else {
            on += snprintf(out + on, sizeof(out) - (size_t)on, "%s%s", on && out[on - 1] != '/' ? "/" : "", s);
        }
        strcpy(cur, cand);
        cn = (int)strlen(cur);
    }
    strcpy(path, out);
    return stat(path, &st) == 0 ? 0 : -1;
}

const char *path_basename(const char *p)
{
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (*s == '/' || *s == '\\' || *s == ':')
            b = s + 1;
    return b;
}

uint32_t errno_to_win(int e)
{
    switch (e) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBADF: return ERROR_INVALID_HANDLE;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    case ENOSPC: return ERROR_DISK_FULL;
    case EISDIR: return ERROR_ACCESS_DENIED;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case EBUSY: return ERROR_BUSY;
    case EPIPE: return ERROR_BROKEN_PIPE;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case EXDEV: return ERROR_NOT_SAME_DEVICE;
    case ENOSYS: return ERROR_CALL_NOT_IMPLEMENTED;
    default: return ERROR_GEN_FAILURE;
    }
}

/* ---- calling into the program with a few arguments ---- */

uint32_t call_guest1(gp_t fn, uint32_t a) { uint32_t v[1] = { a }; return call_guest(fn, 1, v); }
uint32_t call_guest2(gp_t fn, uint32_t a, uint32_t b) { uint32_t v[2] = { a, b }; return call_guest(fn, 2, v); }
uint32_t call_guest3(gp_t fn, uint32_t a, uint32_t b, uint32_t c) { uint32_t v[3] = { a, b, c }; return call_guest(fn, 3, v); }
uint32_t call_guest4(gp_t fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { uint32_t v[4] = { a, b, c, d }; return call_guest(fn, 4, v); }
