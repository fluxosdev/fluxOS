/* Binary files must be routed before HTML/text parsing on the desktop thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"navigation:%d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void)
{
    char path[URL_MAX];
    const char *urls[]={"file:///home/flux/test.7z","file:///home/flux/test%2E7Z?download=1#top",
                        "FILE:///home/flux/a%20b.zip","file:/home/flux/a%23b.7z",
                        "file://localhost/home/flux/a%3Fb.7z"};
    for(unsigned i=0;i<sizeof(urls)/sizeof(urls[0]);i++) {
        CHECK(url_file_path(urls[i],path,sizeof(path)));
        CHECK(web_archive_path(path));
    }
    CHECK(url_file_path(urls[2],path,sizeof(path)) && !strcmp(path,"/home/flux/a b.zip"));
    CHECK(url_file_path(urls[3],path,sizeof(path)) && !strcmp(path,"/home/flux/a#b.7z"));
    CHECK(!url_file_path("https://example.com/a.7z",path,sizeof(path)));
    CHECK(!url_file_path("file:///home/a%00.7z",path,sizeof(path)));
    CHECK(!url_file_path("file:relative.7z",path,sizeof(path)));
    CHECK(!url_file_path(urls[0],path,8));
    CHECK(url_file_path("file:///home/test.7z/",path,sizeof(path)) && !web_archive_path(path));
    CHECK(!web_archive_path("/home/test.7z.html") && !web_archive_path("/home/test.zip.txt"));
    CHECK(url_file_path("file:///home/test%",path,sizeof(path)) && !strcmp(path,"/home/test%"));
    static const unsigned char seven[]={0x37,0x7a,0xbc,0xaf,0x27,0x1c};
    const char *types[]={"text/plain","text/html","","application/x-7z-compressed","image/png"};
    for(unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++)CHECK(web_document_download(types[i],seven,sizeof(seven)));
    CHECK(web_document_download("text/plain","PK\003\004",4));
    CHECK(web_document_download("text/plain","PK\005\006",4));
    CHECK(web_document_download("text/html","%PDF-1.7",8));
    CHECK(web_document_download("","\177ELF",4));
    CHECK(web_document_download("text/plain","\037\213",2));
    CHECK(web_document_download("text/plain","Rar!\032\007\000",7));
    unsigned char binary[1024];memset(binary,0,sizeof(binary));
    CHECK(web_document_download("text/plain",binary,sizeof(binary)));
    CHECK(web_document_download("text/html",binary,sizeof(binary)));
    CHECK(!web_document_download("image/png",binary,sizeof(binary)));
    memset(binary,0x1c,sizeof(binary));
    CHECK(web_document_download("text/plain",binary,sizeof(binary)));
    /* Bounded sniffing even when the response is many megabytes long. */
    memset(binary,'x',sizeof(binary));
    CHECK(!web_document_download("text/plain",binary,16u<<20));
    const char *texts[]={"<!doctype html><title>7z downloads</title>","plain text\nwith\ttabs\r\nand form feed\f",
                         "UTF-8: caf\303\251", "Latin-1: caf\351", "<html>404: archive not found</html>"};
    for(unsigned i=0;i<sizeof(texts)/sizeof(texts[0]);i++)
        CHECK(!web_document_download("text/plain",texts[i],strlen(texts[i])));
    CHECK(!web_document_download("text/plain",NULL,0));
    puts("browser navigation: archive paths, escaped filenames, binary download routing and text/image preservation passed");
}
