#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "webblob.h"
#include "webpolicy.h"
#define BLOB_LIMIT (32u<<20)
struct blob_url {
    struct blob_url *next;
    void *owner;
    size_t len;
    char *url,*origin;
    unsigned char *bytes;
    char type[128];
};
static struct blob_url *urls;
static size_t retained;
static int allowed(struct blob_url *b,void *owner,const char *origin)
{
    return b->owner==owner || (strcmp(b->origin,"null") && !strcmp(b->origin,origin));
}
const char *web_blob_create(void *owner,const char *origin,const void *bytes,size_t len,const char *type)
{
    if(len>BLOB_LIMIT-retained)return NULL;
    unsigned char random[16];int fd=open("/dev/urandom",O_RDONLY);
    if(fd<0)return NULL;
    size_t used=0;
    while(used<sizeof(random)){ssize_t n=read(fd,random+used,sizeof(random)-used);if(n<=0)break;used+=(size_t)n;}
    close(fd);if(used!=sizeof(random))return NULL;
    char token[37];int o=0;
    for(int i=0;i<16;i++){if(i==4||i==6||i==8||i==10)token[o++]='-';snprintf(token+o,3,"%02x",random[i]);o+=2;}
    struct blob_url *b=calloc(1,sizeof(*b));
    if(!b)return NULL;
    b->url=malloc(strlen(origin)+43);b->origin=strdup(origin);b->bytes=malloc(len+1);
    if(!b->url||!b->origin||!b->bytes){free(b->url);free(b->origin);free(b->bytes);free(b);return NULL;}
    snprintf(b->url,strlen(origin)+43,"blob:%s/%s",origin,token);
    if(len)memcpy(b->bytes,bytes,len);b->bytes[len]=0;
    b->owner=owner;b->len=len;
    size_t n=strlen(type);if(n>=sizeof(b->type))n=0;
    for(size_t i=0;i<n;i++)if((unsigned char)type[i]<32||(unsigned char)type[i]>126){n=0;break;}
    memcpy(b->type,type,n);b->type[n]=0;
    b->next=urls;urls=b;retained+=len;return b->url;
}
static void drop(struct blob_url **p)
{
    struct blob_url *b=*p;*p=b->next;retained-=b->len;
    free(b->bytes);free(b->origin);free(b->url);free(b);
}
void web_blob_revoke(void *owner,const char *url)
{
    for(struct blob_url **p=&urls;*p;p=&(*p)->next)
        if((*p)->owner==owner&&!strcmp((*p)->url,url)){drop(p);return;}
}
void web_blob_free(void *owner)
{
    for(struct blob_url **p=&urls;*p;)if((*p)->owner==owner)drop(p);else p=&(*p)->next;
}
int web_blob_read(void *owner,const char *origin,const char *url,unsigned char **bytes,size_t *len,char type[128])
{
    size_t size=strcspn(url,"#");
    for(struct blob_url *b=urls;b;b=b->next){
        if(strlen(b->url)!=size||memcmp(b->url,url,size)||!allowed(b,owner,origin))continue;
        unsigned char *copy=malloc(b->len+1);if(!copy)return 0;
        memcpy(copy,b->bytes,b->len+1);*bytes=copy;*len=b->len;memcpy(type,b->type,128);return 1;
    }
    return 0;
}
