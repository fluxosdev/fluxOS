/* Run the actual engine and collector on the host. These checks exercise
 * entry points and suspended frames that a page-only runner cannot see. */
#include <flux.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jsint.h"

int meminfo(struct meminfo *info) { (void)info; return -1; }
unsigned int uptime_ms(void) { return 0; }
static int sweeps, unhandled, completed, module_failed;
static struct js *engine;

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static void run(const char *src)
{
    if (js_eval(engine, src, (int)strlen(src), "engine regression") < 0) {
        fprintf(stderr, "engine regression: %s\n", js_error(engine));
        exit(1);
    }
    CHECK(unhandled == 0);
}

static void swept(struct js *J, void *ctx)
{
    (void)J; (void)ctx;
    sweeps++;
}

static jsval collect_now(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self; (void)args; (void)argc;
    int before = sweeps;
    gc_collect(J);
    CHECK(sweeps > before);             /* catches a collector that skipped the host stack */
    return js_num(sweeps);
}

static void rejection(struct js *J, jsval promise, jsval reason)
{
    (void)promise;
    fprintf(stderr, "unhandled rejection: %s\n", js_text(J, reason));
    unhandled++;
}

static struct request { int id; char url[2048]; } requests[32];
static int nrequests, served;
static const struct { const char *url, *source; } modules[] = {
    { "file:///counter.js", "export let value=1; export function inc(){return ++value;} "
      "export const read=()=>value; export default class Counter { run(){return inc();} }" },
    { "file:///async.js", "export let answer=await Promise.resolve(41); "
      "export function read(){return answer;} answer++;" },
    { "file:///cycle-a.js", "import {b} from './cycle-b.js'; export function a(){return 2;} "
      "export function sum(){return a()+b();}" },
    { "file:///cycle-b.js", "import {a} from './cycle-a.js'; export function b(){return a()+3;}" },
    { "file:///reexport.js", "export {value, inc} from './counter.js'; export * from './async.js';" },
};

static void fetch(void *ctx, int id, const char *url)
{
    (void)ctx;
    CHECK(nrequests < (int)(sizeof(requests)/sizeof(requests[0])));
    requests[nrequests].id = id;
    snprintf(requests[nrequests++].url, sizeof(requests[0].url), "%s", url);
}

static int resolve(void *ctx, const char *spec, const char *base, char *out, int size)
{
    (void)ctx;
    url_resolve(base, spec, out, size);
    return 0;
}

static void module_done(void *ctx, void *tag, int failed)
{
    (void)ctx; (void)tag;
    completed++;
    module_failed += failed;
}

static void drain_requests(void)
{
    while (served < nrequests) {
        struct request r = requests[served++];
        const char *src = NULL;
        for (unsigned i = 0; i < sizeof(modules)/sizeof(modules[0]); i++)
            if (!strcmp(r.url, modules[i].url))
                src = modules[i].source;
        CHECK(src != NULL);
        js_module_source(engine, r.id, src, (int)strlen(src));
    }
    CHECK(module_failed == 0);
    CHECK(unhandled == 0);
}

static void module(const char *url, const char *src)
{
    int before = completed;
    js_run_module(engine, url, 1, src, (int)strlen(src), NULL);
    drain_requests();
    CHECK(completed == before + 1);
}

int main(void)
{
    mallopt(M_MMAP_MAX, 0);
    engine = js_new(NULL);
    CHECK(engine != NULL);
    engine->on_unhandled = rejection;
    struct js_gc_hooks hooks = { .sweep = swept };
    js_gc_hooks(engine, &hooks);
    js_define_native(engine, js_global(engine), "collectNow", collect_now, 0);
    run("function assert(x){if(!x)throw new Error('assertion failed');}");

    /* Reach the fallback heap frame, keep temporaries through collection,
     * and compile a nested function lazily after its outer call is gone. */
    run("var held=(function(){let x={n:11}; return ()=>{collectNow();return x.n;};})();"
        "collectNow();assert(held()===11);"
        "function pair(a,b,c){return a.n+b.n+c.n;}"
        "assert(pair({n:1},(collectNow(),{n:2}),{n:3})===6);"
        "var many=[{n:0},{n:1},{n:2},{n:3},{n:4},{n:5},{n:6},{n:7},{n:8},{n:9},"
        "{n:10},{n:11},{n:12},{n:13},{n:14},{n:15},{n:16},{n:17},{n:18},{n:19},"
        "{n:20},{n:21},{n:22},{n:23},{n:24},{n:25},{n:26},{n:27},{n:28},{n:29},"
        "{n:30},{n:31},{n:32},{n:33},{n:34},{n:35},{n:36},{n:37},{n:38},{n:39},"
        "{n:40},{n:41},{n:42},{n:43},{n:44},{n:45},{n:46},{n:47},"
        "{n:collectNow()?48:48}];assert(many[0].n===0&&many[48].n===48);");

    /* Collect with a suspended generator's locals, iterator and operand
     * stack live, then resume it through a finally handler. */
    run("var log=[];function* suspend(){let x={n:7};try {"
        "let iter=[{v:2},{v:3}];for(let o of iter) yield x.n+o.v;"
        "return x.n;}finally{log.push(x.n);}}"
        "var gen=suspend();assert(gen.next().value===9);collectNow();"
        "assert(gen.next().value===10);collectNow();"
        "assert(gen.next().value===7);assert(log[0]===7);"
        "async function task(){let x={n:19};await 0;collectNow();return x.n;}"
        "task().then(v=>assert(v===19));");

    /* A second native entry and an attribute handler must use the same
     * compiler, and preserve this and the supplied event argument. */
    jsval handler = js_compile_handler(engine, "collectNow(); return this.n+event.n;", "onclick");
    CHECK(js_is_callable(handler));
    jsval self = js_obj(js_new_object(engine)), event = js_obj(js_new_object(engine)), result;
    js_set(engine, self, "n", js_num(4));
    js_set(engine, event, "n", js_num(5));
    CHECK(js_call(engine, handler, self, &event, 1, &result) == 0);
    CHECK(result.t == JS_NUM && result.u.n == 9);

    /* More simultaneously evaluated operands than vm_run's inline frame,
     * with a collection and a yield in the last argument. */
    run("var operands='';for(var oi=0;oi<80;oi++) operands+=(oi?',':'')+'{n:'+oi+'}';"
        "var last=operands.lastIndexOf('{n:79}');"
        "operands=operands.slice(0,last)+'(collectNow(),{n:79})';"
        "var call='function total(){let n=0;for(let a of arguments)n+=a.n;return n;}';"
        "assert(Function(call+'return total('+operands+');')()===3160);"
        "var make=Function(call+'return function*(){return total('+"
        "operands.slice(0,operands.lastIndexOf('(collectNow()'))+"
        "'(yield 0,{n:79}));}');"
        "var wide=make()();assert(wide.next().value===0);collectNow();"
        "assert(wide.next().value===3160);");

    /* Sequential lexical scopes must release their register values while
     * this function is still running, not just on function return. */
    run("Function(\"{let x='x'.repeat(8<<20);}\".repeat(7))();collectNow();");
    run("Function(\"try {let x='x'.repeat(8<<20);throw 1;}catch(e){}\".repeat(7))();"
        "collectNow();");

    struct js_modhost host = { .fetch = fetch, .resolve = resolve, .done = module_done };
    js_module_host(engine, &host);
    module("file:///main.js", "import Counter,{value,inc,read} from './counter.js';"
        "assert(value===1);assert(inc()===2);assert(value===2);assert(read()===2);"
        "collectNow();assert(new Counter().run()===3);assert(value===3);"
        "assert(import.meta.url==='file:///main.js');assert(this===undefined);");
    module("file:///await-main.js", "import {answer,read} from './async.js';"
        "assert(answer===42);assert(read()===42);"
        "let r=await Promise.resolve({n:8});collectNow();assert(r.n===8);");
    module("file:///cycle-main.js", "import {sum} from './cycle-a.js';assert(sum()===7);");
    module("file:///re-main.js", "import * as ns from './reexport.js';"
        "assert(ns.value===3);assert(ns.inc()===4);assert(ns.value===4);assert(ns.answer===42);");
    run("import('file:///counter.js').then(ns=>{collectNow();assert(ns.value===4);"
        "assert(ns.inc()===5);globalThis.importFinished=true;});");
    drain_requests();
    run("assert(importFinished===true);");

    /* Deep but ordinary code compiles (or throws a catchable error) without
     * stopping the page; regex loops whose turns can be empty terminate. */
    run("assert(Function('return 1'+'+1'.repeat(600))()===601);"
        "var chain='var x=0;if(x==0)x=1;';for(var ci=1;ci<600;ci++)chain+='else if(x=='+ci+')x='+ci+';';"
        "eval(chain);"
        "assert(/(a*)*b/.exec('aab')[1]==='aa');assert(/(?:a?)*$/.test('x'));"
        "assert(/(a|b?)*c/.exec('abc')[1]==='b');assert(/\\c1/.test('\\\\c1'));assert(/\\cJ/.test('\\n'));"
        "var re=/a/g;re.lastIndex=1e10;assert(re.test('aaa')===false);");
    run("var g='';for(var gi=0;gi<40;gi++)g+='(.)';"
        "var gm=new RegExp(g+'\\\\40').exec('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNN');"
        "assert(gm&&gm.length===41&&gm[40]==='N');"
        "var threw=0;try{new RegExp('(.)'.repeat(100));}catch(e){threw=1;}assert(threw);"
        "threw=0;try{new RegExp('a{2147483648}');}catch(e){threw=1;}assert(threw);");
    /* Indices: 2^32 - 1 and up are names; elements past the dense limit are
     * properties, and length follows them. */
    run("var ia=[5];assert(ia['4294967296']===undefined&&ia['4294967295']===undefined);"
        "var sa=[1,2];sa[123456789]=7;assert(sa.length===123456790&&sa[123456789]===7&&(123456789 in sa));"
        "sa.length=5;assert(sa.length===5&&sa[123456789]===undefined);"
        "var mx=[];mx[4294967294]=1;assert(mx.length===4294967295);"
        "var [d0]=[1];assert(d0===1);"
        "var big=[];for(var bi=0;bi<100;bi++)big.push(bi);"
        "assert(Math.max.bind(null).apply(null,big)===99);"
        "function BF(){this.n=arguments.length;}assert(new (BF.bind(null,1))(...big).n===101);");
    /* for-of reaches the elements past the dense limit (a long loop: more steps). */
    engine->step_limit = 400000000u;
    run("var it=[1];it[16777217]=9;var seen=0,last;for(var v of it){seen++;last=v;}"
        "assert(seen===16777218&&last===9);");
    engine->step_limit = JS_STEP_LIMIT;
    /* Parentheses end an optional chain; for-in over many keys stays linear. */
    run("var nul=null,te=0;try{(nul?.b).c;}catch(e){te=e instanceof TypeError;}assert(te);"
        "te=0;try{(nul?.b)();}catch(e){te=e instanceof TypeError;}assert(te);"
        "var ob={v:3,f(){return this.v;}};assert((ob?.f)()===3&&nul?.b.c===undefined);"
        "var many={};for(var mi=0;mi<50000;mi++)many['k'+mi]=mi;var mc=0;for(var mk in many)mc++;"
        "assert(mc===50000);");

    /* The standard library (jslib.c): fromIndex, splice under a shrinking valueOf, unreachable
     * Map/Set storage, deleting while iterating, JSON replacers, revivers and real cycle detection,
     * number formatting, dates, URI decoding and forgiving base64. */
    run("function eq(a,b){if(JSON.stringify(a)!==JSON.stringify(b))throw new Error(JSON.stringify(a)+' !== '+JSON.stringify(b));}"
        "function throws(f,name){try{f();}catch(e){return e.name===name;}return false;}"
        "eq([[1,2,3,2,1].lastIndexOf(1,2),[1,2,3,4].lastIndexOf(4,2),[1,2,3].lastIndexOf(3,-1),[1,2,3].includes(1,2),[1,2,3].includes(3,-1)],[0,-1,2,false,true]);"
        "var sp=[1,2,3,4,5,6,7,8,9,10];sp.splice(5,{valueOf(){sp.length=0;return 3;}});sp.push(9);eq(sp,[9]);"
        "var sq=[1,2,3,4,5];sq.splice({valueOf(){sq.length=3;return 8;}},1,'x');eq(sq,[1,2,3,'x']);"
        "var mp=new Map([[1,2]]);assert(Object.getOwnPropertyNames(mp).length===0&&Object.getOwnPropertySymbols(mp).length===0);"
        "mp.__values=5;assert(mp.get(1)===2);assert(throws(()=>Map.prototype.get.call({__items:[1],__values:1.5},1),'TypeError'));"
        "assert(throws(()=>Set.prototype.add.call(mp,1),'TypeError'));"
        "var dm=new Map([[1,1],[2,2],[3,3],[4,4]]),seen=[];dm.forEach((v,k)=>{seen.push(k);dm.delete(k);});eq(seen,[1,2,3,4]);"
        "var ds=new Set([1,2,3]);seen=[];for(var x of ds){seen.push(x);ds.delete(x);if(x<3)ds.add(x+10);}eq(seen,[1,2,3,11,12]);"
        "eq([JSON.stringify({a:1,b:2,c:3},['c','a']),JSON.stringify({a:1,b:2},(k,v)=>k==='b'?undefined:v)],['{\"c\":3,\"a\":1}','{\"a\":1}']);"
        "eq(JSON.parse('{\"a\":1,\"b\":[1,2]}',(k,v)=>typeof v==='number'?v+100:v),{a:101,b:[101,102]});"
        "var deep={},dc=deep;for(var di=0;di<200;di++){dc.n={};dc=dc.n;}assert(JSON.stringify(deep).length>1000);"
        "var cy={a:{}};cy.a.b=cy;assert(throws(()=>JSON.stringify(cy),'TypeError'));var sh={};eq(JSON.stringify([sh,sh]),'[{},{}]');"
        "eq(JSON.stringify({d:new Date(NaN)}),'{\"d\":null}');"
        "eq([(9.99).toPrecision(2),(123456).toPrecision(2),(1.234e-7).toPrecision(2),(5).toPrecision(30).length,(1234.5678).toExponential(2),(0).toExponential(),(2.5).toFixed(0),(1.005).toFixed(2),(-0.0001).toFixed(2)],"
        "['10','1.2e+5','1.2e-7',31,'1.23e+3','0e+0','3','1.00','-0.00']);"
        "assert(throws(()=>(1).toPrecision(101),'RangeError')&&throws(()=>(1).toFixed(101),'RangeError')&&throws(()=>(1).toExponential(-1),'RangeError'));"
        "assert(String.fromCodePoint(0x1F600)==='\\ud83d\\ude00'&&throws(()=>String.fromCodePoint(0x110000),'RangeError'));"
        "assert(isNaN(new Date(8.7e15).getTime())&&throws(()=>new Date(8.7e15).toISOString(),'RangeError'));"
        "eq([new Date(8.64e15).toISOString(),new Date(Date.UTC(-1,0,1)).toISOString(),new Date(99,0).getFullYear()],['+275760-09-13T00:00:00.000Z','-000001-01-01T00:00:00.000Z',1999]);"
        "var re=/a/g;re.lastIndex=4294967296;assert(re.exec('aaa')===null&&re.lastIndex===0);"
        "assert(throws(()=>decodeURIComponent('%FF'),'URIError')&&throws(()=>decodeURIComponent('%ED%A0%80'),'URIError')&&decodeURIComponent('%E2%82%AC')==='\\u20ac');"
        "eq([decodeURI('%2F%20'),escape('\\u00e9\\u20ac'),unescape('%u20AC%E9')],['%2F ','%E9%u20AC','\\u20ac\\u00e9']);"
        "eq([atob(' aGVs\\nbG8 '),atob('YQ'),btoa('\\u00ff')],['hello','a','/w==']);"
        "assert(throws(()=>btoa('\\u0100'),'InvalidCharacterError')&&throws(()=>atob('Y'),'InvalidCharacterError')&&throws(()=>atob('YQ=a'),'InvalidCharacterError')&&throws(()=>atob('aGV*'),'InvalidCharacterError'));");

    /* Repeated allocation must really be reclaimed, including unreachable
     * bytecode and ASTs; keep a global closure throughout. */
    size_t before = gc_heap_bytes(engine);
    run("function churn(){for(var i=0;i<40000;i++){var o={a:i,b:[i,i+1]};}"
        "collectNow();assert(held()===11);}churn();");
    CHECK(gc_heap_bytes(engine) < before + (12u << 20));
    CHECK(sweeps > 10);

    /* A big script whose functions mostly never run keeps little of its syntax trees: the
     * functions not yet called are stubs, parsed again when they are, and what was compiled lets
     * go of its tree. The bundle's wrapper runs at once; f7 runs later, after collections. */
    {
        enum { NFUNCS = 3000 };
        size_t cap = (size_t)NFUNCS * 160 + 256, n = 0;
        char *big = malloc(cap);
        CHECK(big != NULL);
        n += (size_t)snprintf(big + n, cap - n, "var lazyKeep=(function(){var shared=5;");
        for (int i = 0; i < NFUNCS; i++)
            n += (size_t)snprintf(big + n, cap - n, "function f%d(a){var o={k:a+%d,s:shared};"
                                  "return [o].map(function(x){return x.k*2+x.s;})[0];}", i, i);
        n += (size_t)snprintf(big + n, cap - n, "return f7;})();");
        gc_collect(engine);
        size_t base = gc_heap_bytes(engine);
        CHECK(js_eval(engine, big, (int)n, "lazy bundle") == 0);
        gc_collect(engine);
        /* The whole tree would be over 20 MiB on the host; what is kept is about the source. */
        CHECK(gc_heap_bytes(engine) < base + n * 4 + (1u << 20));
        free(big);
        run("collectNow();assert(lazyKeep(1)===21);collectNow();assert(lazyKeep(2)===23);");
    }
    js_free(engine);

    /* A runaway script ends the entry with a diagnostic. */
    engine = js_new(NULL);
    CHECK(engine != NULL);
    engine->step_limit = 1000;
    CHECK(js_eval(engine, "for(;;){}", 9, "budget") < 0);
    CHECK(strstr(js_error(engine), "ran too long") != NULL);
    js_free(engine);
    puts("JavaScript VM: module linking, async modules, handlers, GC, suspended frames and limits passed");
    return 0;
}
