#define main browser_dom_main
#include "browser-dom.c"
#undef main
#include <unistd.h>
static int cookies(void *ctx, const char *url, char *out, int size)
{
    snprintf(out, (size_t)size, "session=test");
    return 1;
}
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    mallopt(M_MMAP_MAX, 0);
    struct arena a = {0};
    const char *html = "<!doctype html><html><body></body></html>";
    struct jshost host = {.arena = &a,
                          .root = html_parse(&a, html, (int)strlen(html)),
                          .url = "http://app.example/",
                          .base = "http://app.example/",
                          .status = status,
                          .changed = changed,
                          .ws_cookies = cookies};
    struct jsdom *d = jsdom_new(&host);
    if (!d)
        return 2;
    char script[4096];
    snprintf(
        script, sizeof(script),
        "var messages=[],opened=0,closed=false;var ws=new "
        "WebSocket('%s',['chat']);ws.binaryType='arraybuffer';"
        "if(!(ws instanceof WebSocket)||!(ws instanceof EventTarget))throw new Error('WebSocket prototype');"
        "ws.addEventListener('open',function(){opened++;if(ws.protocol!=='chat'||ws.readyState!==WebSocket."
        "OPEN)throw new Error('open state');ws.send('client é');ws.send(new "
        "Uint8Array([0,255,1]));},{once:true});"
        "ws.onmessage=function(e){messages.push(typeof e.data==='string'?e.data:Array.from(new "
        "Uint8Array(e.data)));if(e.origin!==new URL(ws.url).origin)throw new Error('message origin: "
        "'+e.origin);};"
        "ws.onerror=function(e){throw new Error('socket error: '+e.message);};"
        "ws.onclose=function(e){closed=true;if(e.code!==1000||e.reason!=='bye'||!e.wasClean||ws.readyState!=="
        "3)throw new Error('close event');};",
        argv[1]);
    jsdom_run_script(d, NULL, script, (int)strlen(script), "WebSocket browser setup");
    for (int i = 0; i < 800 && !errors; i++) {
        host_time += 10;
        jsdom_run_timers(d);
        if (i > 10 && jsdom_next_timer(d) < 0)
            break;
        usleep(10000);
    }
    const char *check = "if(!closed||opened!==1||JSON.stringify(messages)!=='[\"hello "
                        "😀\",[0,255,16]]')throw new Error('WebSocket messages: '+JSON.stringify(messages));";
    jsdom_run_script(d, NULL, check, (int)strlen(check), "WebSocket browser result");
    jsdom_free(d);
    arena_free(&a);
    if (!errors)
        puts("browser WebSocket events, binary data, origin and cleanup passed");
    return errors ? 1 : 0;
}
