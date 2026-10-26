#define main browser_dom_main
#include "browser-dom.c"
#undef main
static struct {
    int id, calls, credentials, len;
    char method[40], url[2048], headers[8192];
    unsigned char body[64];
} pending;
static void http(void *ctx, int id, const char *method, const char *url, const void *body, int len,
                 const char *headers, int credentials)
{
    pending.id = id;
    pending.calls++;
    pending.credentials = credentials;
    pending.len = len;
    snprintf(pending.method, sizeof(pending.method), "%s", method);
    snprintf(pending.url, sizeof(pending.url), "%s", url);
    snprintf(pending.headers, sizeof(pending.headers), "%s", headers ? headers : "");
    if (body && len <= (int)sizeof(pending.body))
        memcpy(pending.body, body, (size_t)len);
}
static void run(struct jsdom *d, const char *s)
{
    jsdom_run_script(d, NULL, s, (int)strlen(s), "HTTP regression");
}
static void done(struct jsdom *d, int code, const char *headers, const void *body, int len)
{
    jsdom_http_done(d, pending.id, code, headers, (int)strlen(headers), body, len);
}
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "HTTP check failed line %d: %s\n", __LINE__, #x);                                \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)
int main(void)
{
    mallopt(M_MMAP_MAX, 0);
    struct arena a = {0};
    const char *html = "<!doctype html><html><body></body></html>";
    struct jshost h = {.arena = &a,
                       .root = html_parse(&a, html, (int)strlen(html)),
                       .url = "https://app.example/",
                       .base = "https://app.example/",
                       .status = status,
                       .changed = changed,
                       .http = http};
    struct jsdom *d = jsdom_new(&h);
    CHECK(d);
    run(d, "var rejected=false;var x=new "
           "XMLHttpRequest();x.open('GET','https://api.other/"
           "data');x.onerror=function(){rejected=true;};x.send();x.__cross=false;x.__url='https://"
           "app.example/';");
    CHECK(!pending.credentials);
    CHECK(strstr(pending.headers, "Origin: https://app.example"));
    done(d, 200, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n", "secret", 6);
    run(d, "if(!rejected || x.status!==0 || x.responseText!=='') throw new Error('CORS read escaped');");
    run(d, "var x=new "
           "XMLHttpRequest();x.open('PUT','https://api.other/"
           "data');x.setRequestHeader('X-Test','yes');x.setRequestHeader('Cookie','stolen');x."
           "withCredentials=true;x.send(new Uint8Array([0,255,1]));");
    CHECK(!strcmp(pending.method, "OPTIONS") && !pending.credentials);
    CHECK(!strstr(pending.headers, "stolen"));
    done(d, 204,
         "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: "
         "https://app.example\r\nAccess-Control-Allow-Credentials: true\r\nAccess-Control-Allow-Methods: "
         "PUT\r\nAccess-Control-Allow-Headers: x-test\r\n\r\n",
         NULL, 0);
    CHECK(!strcmp(pending.method, "PUT") && pending.credentials && pending.len == 3 && pending.body[0] == 0 &&
          pending.body[1] == 255);
    done(d, 200,
         "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: "
         "https://app.example\r\nAccess-Control-Allow-Credentials: true\r\nContent-Type: "
         "text/plain\r\nSet-Cookie: secret=1\r\nX-Secret: private\r\nX-Public: "
         "yes\r\nAccess-Control-Expose-Headers: X-Public\r\n\r\n",
         "ok", 2);
    run(d, "if(x.responseText!=='ok'||x.getResponseHeader('Set-Cookie')!==null||x.getResponseHeader('X-"
           "Secret')!==null||x.getResponseHeader('X-Public')!=='yes')throw new Error('header exposure');");
    run(d, "var result;fetch('/binary',{credentials:'omit'}).then(function(r){return "
           "r.arrayBuffer();}).then(function(b){result=Array.from(new Uint8Array(b));});");
    CHECK(!pending.credentials && !strcmp(pending.method, "GET"));
    unsigned char bytes[] = {0, 255, 10};
    done(d, 200, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n", bytes, 3);
    run(d, "if(JSON.stringify(result)!=='[0,255,10]')throw new Error('binary fetch');");
    run(d,"var streamed,first,streamDone=false;fetch('/live').then(function(r){streamed=r;var reader=r.body.getReader();return reader.read().then(function(v){first=Array.from(v.value);return reader.read();}).then(function(v){streamDone=v.done;});});");
    const char *stream_headers="HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n";
    CHECK(jsdom_http_headers(d,pending.id,200,stream_headers,(int)strlen(stream_headers))==2);
    run(d,"if(!streamed||first!==undefined||streamDone)throw new Error('fetch did not resolve at headers');");
    jsdom_http_chunk(d,pending.id,bytes,3);
    run(d,"if(JSON.stringify(first)!=='[0,255,10]'||streamDone)throw new Error('chunk not delivered before completion');");
    done(d,200,stream_headers,NULL,0);
    run(d,"if(!streamDone)throw new Error('stream not closed');");
    run(d,"var cloned,original;fetch('/tee').then(function(r){var c=r.clone();r.text().then(function(t){original=t;});c.text().then(function(t){cloned=t;});});");
    CHECK(jsdom_http_headers(d,pending.id,200,stream_headers,(int)strlen(stream_headers))==2);
    jsdom_http_chunk(d,pending.id,"one",3);jsdom_http_chunk(d,pending.id,"two",3);done(d,200,stream_headers,NULL,0);
    run(d,"if(original!=='onetwo'||cloned!=='onetwo')throw new Error('streaming clone');");
    run(d,"var streamError=false;fetch('/broken').then(function(r){r.text().catch(function(){streamError=true;});});");
    CHECK(jsdom_http_headers(d,pending.id,200,stream_headers,(int)strlen(stream_headers))==2);
    jsdom_http_chunk(d,pending.id,"partial",7);done(d,0,"",NULL,0);
    run(d,"if(!streamError)throw new Error('stream failure missing');");
    run(d,"var corsChunk=false;var corsRequest=new XMLHttpRequest();corsRequest.open('GET','https://api.other/live');corsRequest.onprogress=function(){corsChunk=true;};corsRequest.send();");
    CHECK(jsdom_http_headers(d,pending.id,200,stream_headers,(int)strlen(stream_headers))==-1);
    jsdom_http_chunk(d,pending.id,"secret",6);done(d,200,stream_headers,NULL,0);
    run(d,"if(corsChunk)throw new Error('stream exposed before CORS');");
    int calls = pending.calls;
    run(d,
        "fetch('file:///etc/passwd').catch(function(){});fetch('http://app.example/').catch(function(){});");
    CHECK(pending.calls == calls);
    run(d, "var opaque;fetch('https://api.other/public',{mode:'no-cors'}).then(function(r){opaque=r;});");
    done(d, 200, "HTTP/1.1 200 OK\r\n\r\n", "secret", 6);
    run(d, "if(opaque.type!=='opaque'||opaque.status!==0||opaque.body!==null)throw new Error('opaque "
           "response');");
    run(d,"var source=new EventSource('/events'),eventLog=[];source.addEventListener('note',function(e){eventLog.push(e.data,e.lastEventId);});");
    const char *sse_headers="HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n";
    CHECK(jsdom_http_headers(d,pending.id,200,sse_headers,(int)strlen(sse_headers))==2);
    const char *sse1="id: 9\r\nevent: note\r\ndata: one\r";
    const char *sse2="\ndata: two\r\n\r\n";
    jsdom_http_chunk(d,pending.id,sse1,(int)strlen(sse1));jsdom_http_chunk(d,pending.id,sse2,(int)strlen(sse2));
    run(d,"if(source.readyState!==1||eventLog[0]!=='one\\ntwo'||eventLog[1]!=='9')throw new Error('SSE parser');source.close();if(source.readyState!==2)throw new Error('SSE close');");
    run(d, "var animated=document.createElement('div');document.body.appendChild(animated);var "
           "animation=animated.animate([{opacity:0},{opacity:1}],{duration:100,fill:'both'});");
    host_time = 50;
    jsdom_run_timers(d);
    run(d, "if(Number(animated.style.opacity)!==0.5)throw new Error('animation timer midpoint');");
    host_time = 100;
    jsdom_run_timers(d);
    run(d, "if(Number(animated.style.opacity)!==1||animation.playState!=='finished')throw new "
           "Error('animation timer finish');");
    jsdom_free(d);
    arena_free(&a);
    CHECK(!errors);
    puts("browser HTTP: CORS, immutable policy, preflight, credentials, binary fetch and opaque responses "
         "passed");
    return 0;
}
