#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "web.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"storage:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
#ifndef IDB_DIR
#error private test directory required
#endif
static char path[512];
static long size(void){struct stat st;CHECK(!stat(path,&st));return (long)st.st_size;}
int main(void)
{
    const char *origin="https://journal.test",*name="recovery";
    mkdir(IDB_DIR,0700);free(store_idb("delete",origin,name,"",0));
    size_t n=65536;char *text=malloc(n+1);CHECK(text);memset(text,'a',n);text[n]=0;
    CHECK(!store_idb("save",origin,name,text,n));
    DIR *dir=opendir(IDB_DIR);CHECK(dir);struct dirent *entry;
    while((entry=readdir(dir)))if(strstr(entry->d_name,".idb"))snprintf(path,sizeof(path),IDB_DIR "%s",entry->d_name);
    closedir(dir);CHECK(path[0]);long before=size();
    text[30000]='b';CHECK(!store_idb("save",origin,name,text,n));CHECK(size()-before<100);
    char *loaded=store_idb("load",origin,name,"",0);CHECK(loaded && !strcmp(loaded,text));free(loaded);
    int fd=open(path,O_WRONLY|O_APPEND);CHECK(fd>=0 && write(fd,"torn",4)==4);close(fd);
    loaded=store_idb("load",origin,name,"",0);CHECK(loaded && !strcmp(loaded,text));free(loaded);
    text[30001]='c';CHECK(!store_idb("save",origin,name,text,n));
    loaded=store_idb("load",origin,name,"",0);CHECK(loaded && !strcmp(loaded,text));free(loaded);
    CHECK(!store_idb("load","https://other.test",name,"",0));
    char *estimate=store_idb("estimate",origin,"","",0);CHECK(estimate && strstr(estimate,"\"quota\":268435456") && !strstr(estimate,"\"usage\":0"));free(estimate);
    char *failure=store_idb("save",origin,name,"",(64u<<20)+1);CHECK(failure && !strcmp(failure,"QuotaExceededError"));free(failure);
    free(store_idb("delete",origin,name,"",0));CHECK(access(path,F_OK)<0);free(text);
    puts("browser storage: incremental writes, torn-tail recovery, origin isolation, quota and usage passed");
}
