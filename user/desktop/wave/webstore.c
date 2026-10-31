/*
 * Small files the browser keeps (cookies, localStorage): read whole, written whole and moved into
 * place, so a power cut leaves the old file rather than half of a new one. (Kept out of jsdom.c,
 * which the host tests build with the host's headers.)
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "web.h"

/* The file's contents (malloc'd, with a NUL after them), or NULL if it is missing, empty or over max bytes. */
char *store_read_file(const char *path, long max, long *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    long size = (long)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size <= 0 || size > max) {
        close(fd);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    long got = 0;
    while (buf && got < size) {
        int r = (int)read(fd, buf + got, (size_t)(size - got));
        if (r <= 0)
            break;
        got += r;
    }
    close(fd);
    if (!buf || got <= 0) {
        free(buf);
        return NULL;
    }
    buf[got] = 0;
    *len = got;
    return buf;
}

/* Write data to path by way of path.tmp. 0, or -1. */
int store_write_file(const char *path, const char *data, size_t len)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    int ok = 1;
    while (len > 0 && ok) {
        int w = (int)write(fd, data, len);
        if (w <= 0)
            ok = 0;
        else
            data += w, len -= (size_t)w;
    }
    if(ok && fsync(fd)<0)ok=0;
    if (close(fd) < 0)
        ok = 0;
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* When the file was last changed, in seconds since 1970 (0 if it cannot be told). */
long store_file_mtime(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 ? (long)sb.st_mtime : 0;
}

/* ---- IndexedDB ---- */

#ifndef IDB_DIR
#define IDB_DIR "/home/flux/.browser_idb/"
#endif

/* A database's file: IDB_DIR, a hash of its origin and name, ".idb". It starts "origin\nname\n". */
static void idb_path(const char *origin, const char *name, char *out, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (const char *p = origin; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    h = (h ^ '\n') * 1099511628211ull;
    for (const char *p = name; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    snprintf(out, size, IDB_DIR "%08x%08x.idb", (unsigned)(h >> 32), (unsigned)h);
}

/* Does the file's header name this origin (and this name, if one is given)? Then where its text starts. */
static const char *idb_header(const char *file, const char *origin, const char *name)
{
    size_t ol = strlen(origin);
    if (strncmp(file, origin, ol) || file[ol] != '\n')
        return NULL;
    const char *n = file + ol + 1, *nl = strchr(n, '\n');
    if (!nl || (name && ((size_t)(nl - n) != strlen(name) || strncmp(n, name, (size_t)(nl - n)))))
        return NULL;
    return nl + 1;
}

/* The journal is one atomic snapshot followed by checksummed replacement ranges.
 * A torn final record is ignored and removed before appending the next commit. */
#define IDB_LIMIT (64u << 20)
#define IDB_QUOTA (256u << 20)
static uint32_t idb_u32(const char *p)
{const unsigned char *s=(const unsigned char *)p;return s[0]|(uint32_t)s[1]<<8|(uint32_t)s[2]<<16|(uint32_t)s[3]<<24;}
static void idb_put(char *p,uint32_t v)
{for(int k=0;k<4;k++)p[k]=(char)(v>>(8*k));}
static uint32_t idb_hash(const char *p,size_t n)
{uint32_t h=2166136261u;while(n--)h=(h^(unsigned char)*p++)*16777619u;return h;}
static char *idb_replay(const char *file,size_t flen,const char *body,size_t *valid)
{
    size_t pos=(size_t)(body-file);*valid=flen;
    if(flen-pos<10 || memcmp(body,"WAVEIDB1\n",9))return strdup(body);
    pos+=9;if(flen-pos<4)return NULL;
    size_t n=idb_u32(file+pos);pos+=4;
    if(n>IDB_LIMIT || n>flen-pos)return NULL;
    char *text=malloc(n+1);if(!text)return NULL;
    memcpy(text,file+pos,n);text[n]=0;pos+=n;*valid=pos;
    while(flen-pos>=24) {
        const char *r=file+pos;
        uint32_t old=idb_u32(r),prefix=idb_u32(r+4),suffix=idb_u32(r+8),insert=idb_u32(r+12),hash=idb_u32(r+16);
        if(old!=n || prefix>n || suffix>n-prefix || insert>IDB_LIMIT-prefix-suffix || (size_t)insert+24>flen-pos)break;
        size_t size=prefix+insert+suffix;
        if(idb_u32(r+20+insert)!=0x54494d43u)break;
        char *next=malloc(size+1);if(!next)break;
        memcpy(next,text,prefix);memcpy(next+prefix,r+20,insert);memcpy(next+prefix+insert,text+n-suffix,suffix);next[size]=0;
        if(idb_hash(next,size)!=hash){free(next);break;}
        free(text);text=next;n=size;pos+=24+insert;*valid=pos;
    }
    return text;
}
static size_t idb_usage(const char *origin)
{
    DIR *dir=opendir(IDB_DIR);struct dirent *entry;size_t total=0;
    while(dir && (entry=readdir(dir))) {
        size_t n=strlen(entry->d_name);if(n<4 || strcmp(entry->d_name+n-4,".idb"))continue;
        char path[300],header[1024];snprintf(path,sizeof(path),IDB_DIR "%s",entry->d_name);
        int fd=open(path,O_RDONLY);if(fd<0)continue;
        int got=(int)read(fd,header,sizeof(header)-1);header[got>0?got:0]=0;
        if(idb_header(header,origin,NULL)){long size=lseek(fd,0,SEEK_END);if(size>0)total+=(size_t)size;}
        close(fd);
    }
    if(dir)closedir(dir);
    return total;
}

char *store_idb(const char *op, const char *origin, const char *name, const char *text, size_t len)
{
    char path[300];
    if(!strcmp(op,"estimate")) {
        char out[120];snprintf(out,sizeof(out),"{\"usage\":%lu,\"quota\":%u}",(unsigned long)idb_usage(origin),IDB_QUOTA);return strdup(out);
    }
    if (!strcmp(op, "list")) {
        DIR *dir = opendir(IDB_DIR);
        size_t cap = 256, used = 0;
        char *out = malloc(cap);
        struct dirent *e;
        while (out && dir && (e = readdir(dir))) {
            size_t el = strlen(e->d_name);
            if (el < 5 || strcmp(e->d_name + el - 4, ".idb"))
                continue;
            snprintf(path, sizeof(path), IDB_DIR "%s", e->d_name);
            long flen;
            char *file = store_read_file(path, 128l << 20, &flen);
            const char *body = file ? idb_header(file, origin, NULL) : NULL;
            if (body) {
                const char *n = file + strlen(origin) + 1;
                size_t nl = (size_t)(body - 1 - n);
                if (used + nl + 2 > cap) {
                    char *grown = realloc(out, cap = (used + nl + 2) * 2);
                    if (!grown) {
                        free(file);
                        break;
                    }
                    out = grown;
                }
                memcpy(out + used, n, nl);
                used += nl;
                out[used++] = '\n';
            }
            free(file);
        }
        if (dir)
            closedir(dir);
        if (out)
            out[used] = 0;
        return out;
    }
    idb_path(origin, name, path, sizeof(path));
    if (!strcmp(op, "load")) {
        long flen;
        char *file = store_read_file(path, 128l << 20, &flen);
        const char *body = file ? idb_header(file, origin, name) : NULL;
        size_t valid;char *out = body ? idb_replay(file,(size_t)flen,body,&valid) : NULL;
        free(file);
        return out;
    }
    if (!strcmp(op, "delete")) {
        unlink(path);
        return NULL;
    }
    if (!strcmp(op, "save")) {
        if(len>IDB_LIMIT)return strdup("QuotaExceededError");
        mkdir(IDB_DIR,0700);
        long flen=0;size_t valid=0;
        char *file=store_read_file(path,128l<<20,&flen);
        const char *body=file?idb_header(file,origin,name):NULL;
        char *old=body?idb_replay(file,(size_t)flen,body,&valid):NULL;
        size_t olen=old?strlen(old):0,prefix=0,suffix=0;
        while(prefix<olen && prefix<len && old[prefix]==text[prefix])prefix++;
        while(suffix<olen-prefix && suffix<len-prefix && old[olen-suffix-1]==text[len-suffix-1])suffix++;
        size_t inserted=len-prefix-suffix,hl=strlen(origin)+strlen(name)+2;
        int journal=body && (size_t)(file+flen-body)>=9 && !memcmp(body,"WAVEIDB1\n",9);
        int compact=!journal || !old || valid>len*3+(1u<<20);
        size_t growth=compact?hl+13+len:inserted+24;
        if(idb_usage(origin)+growth>IDB_QUOTA){free(file);free(old);return strdup("QuotaExceededError");}
        int ok=0;
        if(compact) {
            char *out=malloc(hl+13+len+1);
            if(out) {
                snprintf(out,hl+1,"%s\n%s\n",origin,name);
                memcpy(out+hl,"WAVEIDB1\n",9);idb_put(out+hl+9,(uint32_t)len);memcpy(out+hl+13,text,len);
                ok=store_write_file(path,out,hl+13+len)==0;free(out);
            }
        } else {
            char *record=malloc(inserted+24);
            if(record) {
                idb_put(record,(uint32_t)olen);idb_put(record+4,(uint32_t)prefix);idb_put(record+8,(uint32_t)suffix);
                idb_put(record+12,(uint32_t)inserted);idb_put(record+16,idb_hash(text,len));memcpy(record+20,text+prefix,inserted);
                idb_put(record+20+inserted,0x54494d43u);
                int fd=open(path,O_WRONLY);size_t written=0;
                if(fd>=0 && ftruncate(fd,(long)valid)==0 && lseek(fd,(long)valid,SEEK_SET)>=0) {
                    while(written<inserted+24) {
                        int n=(int)write(fd,record+written,inserted+24-written);if(n<=0)break;written+=(size_t)n;
                    }
                    ok=written==inserted+24 && fsync(fd)==0;
                }
                if(fd>=0){if(!ok && ftruncate(fd,(long)valid)==0)fsync(fd);close(fd);}
                free(record);
            }
        }
        free(file);free(old);if(!ok)return strdup("UnknownError");
    }
    return NULL;
}
