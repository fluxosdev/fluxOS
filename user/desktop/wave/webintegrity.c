#include <ctype.h>
#include <string.h>
#include <flux/digest.h>
#include "webintegrity.h"
static void b64(const unsigned char *p,int n,char *out)
{
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int j=0;
    for(int i=0;i<n;i+=3){
        unsigned v=(unsigned)p[i]<<16;
        if(i+1<n)v|=(unsigned)p[i+1]<<8;
        if(i+2<n)v|=p[i+2];
        out[j++]=alphabet[v>>18];out[j++]=alphabet[(v>>12)&63];
        out[j++]=i+1<n?alphabet[(v>>6)&63]:'=';out[j++]=i+2<n?alphabet[v&63]:'=';
    }
    out[j]=0;
}
int web_integrity(const char *metadata,const void *bytes,size_t length)
{
    if(!metadata)return 1;
    int strongest=0,matched=0;unsigned char hash[64];char encoded[89];
    for(const char *p=metadata;*p;){
        while(isspace((unsigned char)*p))p++;
        if(!*p)break;
        const char *end=p;while(*end&&!isspace((unsigned char)*end))end++;
        int strength=0;const char *name=NULL;
        if(end-p>7&&!strncmp(p,"sha256-",7))strength=32,name="SHA-256";
        if(end-p>7&&!strncmp(p,"sha384-",7))strength=48,name="SHA-384";
        if(end-p>7&&!strncmp(p,"sha512-",7))strength=64,name="SHA-512";
        if(strength&&strength>=strongest){
            if(strength>strongest){strongest=strength;matched=0;flux_digest(name,bytes,length,hash);b64(hash,strength,encoded);}
            const char *stop=memchr(p+7,'?',(size_t)(end-p-7));if(!stop)stop=end;
            int n=(int)(stop-p-7),expected=(int)strlen(encoded);
            while(n>0&&p[7+n-1]=='=')n--;
            while(expected>0&&encoded[expected-1]=='=')expected--;
            if(n==expected&&!memcmp(p+7,encoded,(size_t)n))matched=1;
        }
        p=end;
    }
    return !strongest||matched;
}
