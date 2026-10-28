#define main browser_dom_main
#include "browser-dom.c"
#undef main
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
int main(void)
{
    mallopt(M_MMAP_MAX,0);
    struct arena a={0},sa={0};
    const char *html="<!doctype html><html><body><div id=box style='transform:translate(50%,100%) scale(2);transform-origin:top left'></div></body></html>";
    struct node *root=html_parse(&a,html,(int)strlen(html));
    css_cascade(&sa,root,NULL,0,200,100,NULL,NULL);
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
        "check(CSS.supports('transform','rotate(30deg) scale(2)'));"
        "check(CSS.supports('transform','skew(1rad, .2turn)'));"
        "check(!CSS.supports('transform','rotate(10px)'));"
        "check(!CSS.supports('transform','scale(1, )'));"
        "check(!CSS.supports('transform','rotate3d(1,0,0,20deg)'));"
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
