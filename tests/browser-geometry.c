#define main browser_dom_main
#include "browser-dom.c"
#undef main
#include "webpolicy.h"
static int raw_box(void *ctx,struct node *n,int *x,int *y,int *w,int *h)
{ (void)ctx;(void)n;*x=10;*y=20;*w=20;*h=10;return 1; }
static int visual_box(void *ctx,struct node *n,int *x,int *y,int *w,int *h)
{ (void)ctx;(void)n;*x=30;*y=40;*w=40;*h=20;return 1; }
static int local_point(void *ctx,struct node *n,int *x,int *y)
{ (void)ctx;(void)n;*x=7;*y=8;return 1; }
static void scroll_pos(void *ctx,int *x,int *y)
{ (void)ctx;*x=3;*y=4; }
static int nested_scroll(void *ctx,struct node *n,int set,int *x,int *y,int *cw,int *ch,int *vw,int *vh)
{ (void)ctx;(void)n;(void)set;*x=100;*y=100;*cw=*ch=200;*vw=*vh=100;return 1; }
static void run_code(struct jsdom *d,struct node *script,const char *text)
{jsdom_run_script(d,script,text,(int)strlen(text),"platform host regression");}
static void platform_hosts(void)
{
    struct arena arenas[4]={{0}};struct jsdom *pages[4];struct node *roots[4];
    const char *html="<!doctype html><html><body><script nonce=ok></script></body></html>";
    for(int k=0;k<4;k++) {
        const char *url=k==2?"https://other.test/":"https://same.test/";
        roots[k]=html_parse(&arenas[k],html,(int)strlen(html));
        struct jshost h={.arena=&arenas[k],.root=roots[k],.url=url,.base=url,.status=status,.changed=changed,
            .csp=k==3?"script-src 'nonce-ok'":NULL};
        pages[k]=jsdom_new(&h);
    }
    run_code(pages[1],NULL,"var got=[];var channel=new BroadcastChannel('host');channel.onmessage=function(e){got.push(e.data.value);};");
    run_code(pages[2],NULL,"var got=[];var channel=new BroadcastChannel('host');channel.onmessage=function(e){got.push(e.data.value);};");
    run_code(pages[0],NULL,"var sender=new BroadcastChannel('host');sender.postMessage({value:17});");
    for(int k=0;k<3;k++)jsdom_run_timers(pages[k]);
    run_code(pages[1],NULL,"if(JSON.stringify(got)!=='[17]')throw Error('same-origin broadcast was not delivered');");
    run_code(pages[2],NULL,"if(got.length)throw Error('cross-origin broadcast leaked');");
    run_code(pages[3],NULL,"globalThis.blocked=true;");
    /* The nonce permits the harness, while unnonced scripts and eval remain blocked. */
    struct node *script=node_find(roots[3],T_SCRIPT);
    run_code(pages[3],script,"if(globalThis.blocked)throw Error('CSP allowed an unnonced script');var denied=0;try{eval('1');}catch(e){denied++;}try{new Function('return 1');}catch(e){denied++;}try{setTimeout('1',0);}catch(e){denied++;}if(denied!==3)throw Error('CSP code-generation restriction failed');");
    for(int k=0;k<4;k++){jsdom_free(pages[k]);arena_free(&arenas[k]);}
}
int main(void)
{
    mallopt(M_MMAP_MAX,0);
    platform_hosts();
    struct arena a={0},sa={0};
    const char *html="<!doctype html><html><body><div id=box style='transform:translate(50%,100%) scale(2);transform-origin:top left'></div><div id=projective style='transform:perspective(100px) translateZ(50px)'></div></body></html>";
    struct node *root=html_parse(&a,html,(int)strlen(html));
    const char *css="#projective{color:red}@supports (contain:layout){#projective{color:blue}}"
                    "@supports not (contain:layout){#projective{color:lime}}";
    struct sheet *sheet=css_parse(&a,css,(int)strlen(css),200);
    css_cascade(&sa,root,&sheet,1,200,100,NULL,NULL);
    struct jshost h={.arena=&a,.root=root,.url="file:///geometry.html",.base="file:///geometry.html",
        .status=status,.changed=changed,.node_box=raw_box,.node_visual_box=visual_box,.node_local_point=local_point,
        .scroll_pos=scroll_pos,.node_scroll=nested_scroll};
    struct jsdom *d=jsdom_new(&h);
    const char *script=
        "function check(x){if(!x)throw Error('geometry assertion');}"
        "var e=document.getElementById('box'),r=e.getBoundingClientRect(),s=getComputedStyle(e);"
        "check(e.offsetWidth===20&&e.offsetHeight===10);"
        "check(r.x===27&&r.y===36&&r.width===40&&r.height===20&&r.right===67);"
        "check(s.transform==='matrix(2, 0, 0, 2, 10, 10)');"
        "check(s.transformOrigin==='0px 0px');"
        "var ps=getComputedStyle(document.getElementById('projective')),value=ps.transform;"
        "var m=value.slice(value.indexOf('(')+1,-1).split(',').map(Number);"
        "check(value.indexOf('matrix3d(')===0&&m.length===16&&m[10]===1&&Math.round(m[11]*100)===-1&&m[14]===50&&m[15]===.5);"
        "check(ps.color==='rgb(0, 255, 0)');"
        "check(!CSS.supports('contain','layout')&&!CSS.supports('scroll-snap-type','x mandatory'));"
        "check(!CSS.supports('transform-style','preserve-3d')&&CSS.supports('perspective','200px'));"
        "check(CSS.supports('grid-template-columns','subgrid')&&CSS.supports('grid-template-rows','subgrid [start] [end]'));"
        "check(!CSS.supports('grid-template-columns','subgrid 20px')&&!CSS.supports('grid-template-columns','subgrid [unclosed'));"
        "check(CSS.supports('transform','rotate(30deg) scale(2)'));"
        "check(CSS.supports('transform','skew(1rad, .2turn)'));"
        "check(!CSS.supports('transform','rotate(10px)'));"
        "check(!CSS.supports('transform','scale(1, )'));"
        "check(CSS.supports('transform','rotate3d(1,0,0,20deg)')&&!CSS.supports('transform','rotate3d(1,0,20deg)'));"
        "var offsets=null;e.addEventListener('click',function(ev){offsets=[ev.offsetX,ev.offsetY,ev.clientX,ev.clientY];});";
    jsdom_run_script(d,NULL,script,(int)strlen(script),"transform geometry");
    struct node *element=node_find(root,T_DIV);
    jsdom_mouse_event(d,element,"click",44,55,0,NULL);
    const char *end="check(JSON.stringify(offsets)==='[7,8,41,51]');";
    jsdom_run_script(d,NULL,end,(int)strlen(end),"pointer geometry");
    jsdom_free(d);arena_free(&sa);arena_free(&a);
    if(!errors)puts("browser geometry: layout metrics, transformed client bounds, computed matrices, feature detection and event offsets passed");
    return errors?1:0;
}
