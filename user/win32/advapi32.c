/* A case-insensitive, file-backed registry in the Windows prefix. Keys are directories;
 * each value is an atomically replaced typed record. Names are hex encoded, so registry
 * separators and dot names can never escape the hive's filesystem directory. */
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ui.h"
#define REG_HANDLES 256
struct regkey { HKEY handle;char *path; };
static struct regkey keys[REG_HANDLES];static uint16_t generations[REG_HANDLES];
static int mkdirs(const char *path)
{
    char work[PATH_MAX];snprintf(work,sizeof(work),"%s",path);
    for(char *p=work+1;;p++) if(*p=='/' || !*p) {
        int end=!*p;char c=*p;*p=0;
        if(mkdir(work,0755)<0 && errno!=EEXIST) return -1;
        *p=c;if(end) break;
    }
    return 0;
}
static const char *key_path(HKEY h)
{
    static const char *roots[]={"classes","user","machine","users",NULL,"config"};
    static char predefined[6][128];
    if(h>=0x80000000u && h<=0x80000005u) {
        int i=h-0x80000000u;if(!roots[i]) return NULL;
        snprintf(predefined[i],sizeof(predefined[i]),"/winapps/WINDOWS/registry/%s",roots[i]);
        if(mkdirs(predefined[i])<0) return NULL;
        return predefined[i];
    }
    unsigned i=h&255;return keys[i].handle==h ? keys[i].path:NULL;
}
static HKEY key_handle(const char *path)
{
    for(int i=0;i<REG_HANDLES;i++) if(!keys[i].path) {
        keys[i].path=strdup(path);if(!keys[i].path) return 0;
        generations[i]=generations[i]%0xFFF+1;keys[i].handle=0x71000000u|(uint32_t)generations[i]<<8|i;
        return keys[i].handle;
    }
    return 0;
}
static int encoded(char *out,size_t cap,const uint16_t *text,int n)
{
    static const char hex[]="0123456789abcdef";
    uint16_t *lower=ui_wdup(text,n);for(int i=0;i<n;i++) if(lower[i]>='A' && lower[i]<='Z') lower[i]+=32;
    char *utf8=malloc((size_t)n*4+1);if(!utf8) {free(lower);return 0;}
    int bytes=wide_to_utf8(lower,n,utf8,n*4);free(lower);
    if((size_t)bytes*2>=cap) {free(utf8);return 0;}
    for(int i=0;i<bytes;i++) {out[i*2]=hex[(uint8_t)utf8[i]>>4];out[i*2+1]=hex[(uint8_t)utf8[i]&15];}
    out[bytes*2]=0;free(utf8);return 1;
}
static int make_path(HKEY h,gp_t name,int wide,int value,char *out)
{
    const char *base=key_path(h);if(!base) return 6;
    snprintf(out,PATH_MAX,"%s",base);uint16_t *s=ui_string(name,wide);int n=wstrlen(s),pos=0;
    if(value) {
        size_t len=strlen(out);if(len+4>=PATH_MAX || !encoded(out+len+3,PATH_MAX-len-3,s,n)) {free(s);return 206;}
        memcpy(out+len,"/v-",3);
    } else while(pos<n) {
        while(s[pos]=='\\' || s[pos]=='/') pos++;
        int start=pos;
        while(s[pos] && s[pos]!='\\' && s[pos]!='/') pos++;
        if(pos==start) continue;
        size_t len=strlen(out);
        if(len+4>=PATH_MAX || !encoded(out+len+3,PATH_MAX-len-3,s+start,pos-start)) {free(s);return 206;}
        memcpy(out+len,"/k-",3);
    }
    free(s);return 0;
}
static uint32_t open_key(uint32_t *a,int wide,int create,int extended)
{
    char path[PATH_MAX];int err=make_path(A(0),A(1),wide,0,path);if(err) return err;
    struct stat st;int existed=stat(path,&st)==0;
    if(create && mkdirs(path)<0) return errno==EACCES ? 5:8;
    if(!create && (!existed || !S_ISDIR(st.st_mode))) return 2;
    HKEY h=key_handle(path);if(!h) return 8;
    unsigned output=create && extended ? 7:extended ? 4:2;
    if(!A(output)) {unsigned i=h&255;free(keys[i].path);memset(&keys[i],0,sizeof(keys[i]));return 87;}
    *(HKEY *)P(output)=h;
    if(create && extended && A(8)) *(DWORD *)P(8)=existed ? 2:1;
    return 0;
}
DEF(RegOpenKeyExA) {return open_key(a,0,0,1);} DEF(RegOpenKeyExW) {return open_key(a,1,0,1);}
DEF(RegOpenKeyA) {return open_key(a,0,0,0);} DEF(RegOpenKeyW) {return open_key(a,1,0,0);}
DEF(RegCreateKeyExA) {return open_key(a,0,1,1);} DEF(RegCreateKeyExW) {return open_key(a,1,1,1);}
DEF(RegCreateKeyA) {return open_key(a,0,1,0);} DEF(RegCreateKeyW) {return open_key(a,1,1,0);}
DEF(RegCloseKey)
{
    if(A(0)>=0x80000000u && A(0)<=0x80000005u) return 0;
    unsigned i=A(0)&255;if(keys[i].handle!=A(0) || !keys[i].path) return 6;
    free(keys[i].path);memset(&keys[i],0,sizeof(keys[i]));return 0;
}
static int string_type(uint32_t type) {return type==1 || type==2 || type==7;}
static uint32_t set_value(uint32_t *a,int wide)
{
    char path[PATH_MAX],temp[PATH_MAX];int err=make_path(A(0),A(1),wide,1,path);if(err) return err;
    uint32_t size=A(5),type=A(3);if(size>16*1024*1024 || (size && !A(4))) return 87;
    uint8_t *data=NULL;
    if(!wide && string_type(type)) {
        if(size>8*1024*1024) return 87;
        data=malloc(size*2+2);if(!data) return 8;ansi_to_wide(P(4),size,(uint16_t *)data,size);size*=2;
    } else if(size) {data=malloc(size);if(!data) return 8;memcpy(data,P(4),size);}
    int n=snprintf(temp,sizeof(temp),"%s.tmp-%d-%u",path,getpid(),teb->thread_id);
    if(n>=(int)sizeof(temp)) {free(data);return 206;}
    FILE *f=fopen(temp,"wb");if(!f) {free(data);return 5;}
    uint32_t header[]={0x47455246,type,size};
    int good=fwrite(header,1,sizeof(header),f)==sizeof(header) && (!size || fwrite(data,1,size,f)==size);
    free(data);if(fclose(f)) good=0;
    if(!good || rename(temp,path)<0) {unlink(temp);return 5;}return 0;
}
DEF(RegSetValueExA) {return set_value(a,0);} DEF(RegSetValueExW) {return set_value(a,1);}
static uint32_t query_value(uint32_t *a,int wide)
{
    if(!A(5)) return 87;
    char path[PATH_MAX];int err=make_path(A(0),A(1),wide,1,path);if(err) return err;
    FILE *f=fopen(path,"rb");if(!f) return 2;
    uint32_t header[3];if(fread(header,1,sizeof(header),f)!=sizeof(header) || header[0]!=0x47455246 || header[2]>16*1024*1024) {fclose(f);return 13;}
    uint32_t size=header[2];uint8_t *data=malloc((size_t)size+2);if(!data) {fclose(f);return 8;}
    if(fread(data,1,size,f)!=size) {free(data);fclose(f);return 13;}fclose(f);
    if(!wide && string_type(header[1])) {size/=2;wide_to_ansi((uint16_t *)data,size,(char *)data,size);}
    if(A(3)) *(DWORD *)P(3)=header[1];
    uint32_t capacity=*(DWORD *)P(5);*(DWORD *)P(5)=size;
    uint32_t result=A(4) && capacity<size ? 234:0;
    if(!result && A(4) && size) memcpy(P(4),data,size);
    free(data);return result;
}
DEF(RegQueryValueExA) {return query_value(a,0);} DEF(RegQueryValueExW) {return query_value(a,1);}
static uint32_t delete_value(uint32_t *a,int wide)
{
    char path[PATH_MAX];int err=make_path(A(0),A(1),wide,1,path);if(err) return err;return unlink(path)<0 ? 2:0;
}
DEF(RegDeleteValueA) {return delete_value(a,0);} DEF(RegDeleteValueW) {return delete_value(a,1);}
static uint32_t delete_key(uint32_t *a,int wide)
{
    if(!A(1) || !(wide ? *(uint16_t *)P(1):*(uint8_t *)P(1))) return 87;
    char path[PATH_MAX];int err=make_path(A(0),A(1),wide,0,path);if(err) return err;
    DIR *dir=opendir(path);if(!dir) return 2;struct dirent *e;
    while((e=readdir(dir))) if(!strncmp(e->d_name,"k-",2)) {closedir(dir);return 5;}
    rewinddir(dir);
    while((e=readdir(dir))) if(!strncmp(e->d_name,"v-",2)) {char file[PATH_MAX];snprintf(file,sizeof(file),"%s/%s",path,e->d_name);unlink(file);}
    closedir(dir);return rmdir(path)<0 ? 5:0;
}
DEF(RegDeleteKeyA) {return delete_key(a,0);} DEF(RegDeleteKeyW) {return delete_key(a,1);}
DEF(RegFlushKey) {return key_path(A(0)) ? 0:6;}
static uint32_t query_info(uint32_t *a,int wide)
{
    const char *path=key_path(A(0));if(!path) return 6;DIR *dir=opendir(path);if(!dir) return 2;
    uint32_t sub=0,values=0,maxkey=0,maxvalue=0,maxdata=0;struct dirent *e;
    while((e=readdir(dir))) {
        unsigned len=strlen(e->d_name);if(len<2) continue;
        if(!strncmp(e->d_name,"k-",2)) {sub++;if((len-2)/2>maxkey) maxkey=(len-2)/2;}
        if(!strncmp(e->d_name,"v-",2)) {values++;if((len-2)/2>maxvalue) maxvalue=(len-2)/2;char file[PATH_MAX];struct stat st;snprintf(file,sizeof(file),"%s/%s",path,e->d_name);if(!stat(file,&st) && st.st_size>12 && (uint32_t)st.st_size-12>maxdata) maxdata=st.st_size-12;}
    }
    closedir(dir);if(A(2)) *(DWORD *)P(2)=0;
    if(A(1)) {if(wide) *(uint16_t *)P(1)=0;else *(char *)P(1)=0;}
    if(A(4)) *(DWORD *)P(4)=sub;
    if(A(5)) *(DWORD *)P(5)=maxkey;
    if(A(6)) *(DWORD *)P(6)=0;
    if(A(7)) *(DWORD *)P(7)=values;
    if(A(8)) *(DWORD *)P(8)=maxvalue;
    if(A(9)) *(DWORD *)P(9)=maxdata;
    if(A(10)) *(DWORD *)P(10)=0;
    if(A(11)) memset(P(11),0,8);
    return 0;
}
DEF(RegQueryInfoKeyA) {return query_info(a,0);} DEF(RegQueryInfoKeyW) {return query_info(a,1);}
static int name_order(const void *a,const void *b) {return strcmp(*(char *const *)a,*(char *const *)b);}
static int unhex(int c) {return c>='0' && c<='9' ? c-'0':c>='a' && c<='f' ? c-'a'+10:-1;}
static uint32_t enum_name(HKEY h,uint32_t index,int value,uint16_t **out)
{
    const char *path=key_path(h);if(!path) return 6;
    DIR *dir=opendir(path);if(!dir) return 2;
    char **names=NULL;size_t count=0;struct dirent *entry;uint32_t error=0;
    while((entry=readdir(dir))) {
        if(entry->d_name[0]!=(value ? 'v':'k') || entry->d_name[1]!='-') continue;
        char **p=realloc(names,(count+1)*sizeof(*p));if(!p) {error=8;break;}names=p;
        names[count]=strdup(entry->d_name+2);if(!names[count]) {error=8;break;}count++;
    }
    closedir(dir);
    if(!error && index>=count) error=259;
    if(!error) {
        qsort(names,count,sizeof(*names),name_order);const char *hex=names[index];size_t bytes=strlen(hex)/2;
        char *text=calloc(bytes+1,1);uint16_t *wide=calloc(bytes+1,2);
        if(!text || !wide) {free(wide);error=8;}
        else {
            for(size_t i=0;i<bytes;i++) {int hi=unhex(hex[i*2]),lo=unhex(hex[i*2+1]);if(hi<0 || lo<0) {error=13;break;}text[i]=(hi<<4)|lo;}
            if(!error) {utf8_to_wide(text,-1,wide,bytes+1);*out=wide;}else free(wide);
        }
        free(text);
    }
    for(size_t i=0;i<count;i++) free(names[i]);
    free(names);return error;
}
static uint32_t enum_entry(uint32_t *a,int wide,int value)
{
    if(!A(3) || (A(4)!=0)) return 87;
    uint16_t *name=NULL;uint32_t error=enum_name(A(0),A(1),value,&name);if(error) return error;
    uint32_t n=wstrlen(name),capacity=*(DWORD *)P(3);*(DWORD *)P(3)=n;
    if(!A(2) || capacity<=n) {free(name);return 234;}
    if(wide) memcpy(P(2),name,(n+1)*2);else wide_to_ansi(name,n+1,P(2),n+1);
    if(value) {
        gp_t p=ui_guest(name,wide);uint32_t args[]={A(0),p,0,A(5),A(6),A(7)};
        error=query_value(args,wide);gfree(p);
    } else {
        if(A(6)) {if(A(5) && *(DWORD *)P(6)) {if(wide) *(uint16_t *)P(5)=0;else *(char *)P(5)=0;}*(DWORD *)P(6)=0;}
        if(A(7)) memset(P(7),0,8);
    }
    free(name);return error;
}
DEF(RegEnumKeyExA) {return enum_entry(a,0,0);} DEF(RegEnumKeyExW) {return enum_entry(a,1,0);}
DEF(RegEnumValueA) {return enum_entry(a,0,1);} DEF(RegEnumValueW) {return enum_entry(a,1,1);}
static const struct api_entry registry_entries[]={
    API(RegOpenKeyExA,5),API(RegOpenKeyExW,5),API(RegOpenKeyA,3),API(RegOpenKeyW,3),API(RegCreateKeyExA,9),API(RegCreateKeyExW,9),
    API(RegCreateKeyA,3),API(RegCreateKeyW,3),API(RegCloseKey,1),API(RegSetValueExA,6),API(RegSetValueExW,6),
    API(RegQueryValueExA,6),API(RegQueryValueExW,6),API(RegDeleteValueA,2),API(RegDeleteValueW,2),API(RegDeleteKeyA,2),API(RegDeleteKeyW,2),
    API(RegEnumKeyExA,8),API(RegEnumKeyExW,8),API(RegEnumValueA,8),API(RegEnumValueW,8),API(RegFlushKey,1),API(RegQueryInfoKeyA,12),API(RegQueryInfoKeyW,12),
};
struct api_dll advapi32_dll={.name="ADVAPI32.DLL",.entries=registry_entries,.count=sizeof(registry_entries)/sizeof(registry_entries[0])};
