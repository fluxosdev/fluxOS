/* The JavaScript language itself, construct by construct: what the engine
 * must keep doing whatever runs it. Run on the host by make test-browser
 * (tests/browser-dom.c supplies test()); the async tests count when their
 * promises settle, before the run's final count is checked. */

function asyncTest(n, f, expected) {
    f().then(function (result) {
        if (JSON.stringify(result) !== JSON.stringify(expected))
            throw new Error(n + ': ' + JSON.stringify(result));
        count++;
    });
}

/* ---- values and operators ---- */
test("arithmetic", function () { return [1 + 2 * 3, (1 + 2) * 3, 7 % 3, -7 % 3, 2 ** 10, 7 / 2, 5 - -2]; },
     [7, 9, 1, -1, 1024, 3.5, 7]);
test("bitwise", function () { return [5 & 3, 5 | 3, 5 ^ 3, ~5, 1 << 31, -16 >> 2, -16 >>> 28, 2 ** 32 | 0]; },
     [1, 7, 6, -6, -2147483648, -4, 15, 0]);
test("string concat", function () { return ["a" + 1, 1 + "a", 1 + 2 + "3", "1" + 2 + 3, [] + {}, "x" + null + undefined]; },
     ["a1", "1a", "33", "123", "[object Object]", "xnullundefined"]);
test("comparison", function () {
    return [1 < 2, "10" < "9", 10 < 9, "10" < 9, null == undefined, null === undefined, NaN == NaN,
            "1" == 1, 0 == "", [1] == 1, 1 <= 1, "b" >= "a", undefined < 1, null >= 0];
}, [true, true, false, false, true, false, false, true, true, true, true, true, false, true]);
test("typeof", function () {
    return [typeof 1, typeof "s", typeof true, typeof undefined, typeof null, typeof {}, typeof [],
            typeof function () {}, typeof class {}, typeof notDeclaredAnywhere, typeof Symbol.iterator];
}, ["number", "string", "boolean", "undefined", "object", "object", "object", "function", "function",
    "undefined", "symbol"]);
test("logical", function () {
    var log = [];
    function t(v) { log.push(v); return v; }
    var r = [t(0) && t(1), t(1) && t(2), t(0) || t(3), t(4) || t(5), t(null) ?? t(6), t(0) ?? t(7)];
    return [r, log];
}, [[0, 2, 3, 4, 6, 0], [0, 1, 2, 0, 3, 4, null, 6, 0]]);
test("conditional and comma", function () { var x = (1, 2, 3); return [x, x > 2 ? "big" : "small", 0 ? 1 : 2]; },
     [3, "big", 2]);
test("increments", function () {
    var a = 1, o = { n: 5 }, arr = [1, 2];
    var r = [a++, a, ++a, a--, a, --a, o.n++, ++o.n, arr[1]++, arr];
    return r;
}, [1, 2, 3, 3, 2, 1, 5, 7, 2, [1, 3]]);
test("compound assignment", function () {
    var a = 10, o = { x: 3 }, k = 0, arr = [1, 2, 3];
    a += 5; a -= 3; a *= 2; a /= 4; a %= 4; a **= 3; a <<= 2; a >>= 1; a |= 1; a &= 13; a ^= 6;
    o.x += 4;
    arr[k++] += 10;
    return [a, o.x, arr, k];
}, [7, 7, [11, 2, 3], 1]);
test("logical assignment", function () {
    var a = 0, b = 1, c = null, o = { x: 0 }, calls = 0;
    a ||= 5; b &&= 7; c ??= 9; o.x ||= (calls++, 2); o.x ||= (calls++, 3);
    return [a, b, c, o.x, calls];
}, [5, 7, 9, 2, 1]);
test("member evaluated once", function () {
    var n = 0, o = { v: 1 };
    function get() { n++; return o; }
    get().v += 1; get().v++;
    return [o.v, n];
}, [3, 2]);
test("void and delete", function () {
    var o = { a: 1, b: 2 }, arr = [1, 2, 3];
    var r = [void 0, delete o.a, "a" in o, delete arr[1], arr[1], arr.length];
    return r;
}, [undefined, true, false, true, undefined, 3]);
test("in and instanceof", function () {
    function F() {}
    var f = new F();
    return ["x" in { x: 1 }, 0 in [5], 2 in [5], f instanceof F, f instanceof Object, [] instanceof Array, 1 instanceof Number];
}, [true, true, false, true, true, true, false]);
test("numbers to strings", function () { return [String(0.1 + 0.2), String(1e21), String(-0), (255).toString(16), String(1 / 3)]; },
     ["0.30000000000000004", "1e+21", "0", "ff", "0.3333333333333333"]);

/* ---- variables and scope ---- */
test("var hoisting", function () {
    var r = [typeof x, f()];
    var x = 1;
    function f() { return "hoisted"; }
    return r;
}, ["undefined", "hoisted"]);
test("block scope", function () {
    var out = [];
    let a = 1;
    { let a = 2; out.push(a); { const a = 3; out.push(a); } out.push(a); }
    out.push(a);
    return out;
}, [2, 3, 2, 1]);
test("var leaks out of blocks", function () { { var v = 4; } if (true) { var w = 5; } return [v, w]; }, [4, 5]);
test("const assignment throws", function () {
    const c = 1;
    try { c = 2; return "no"; } catch (e) { return [e instanceof TypeError, c]; }
}, [true, 1]);
test("shadowing parameters", function () {
    function f(a, b) { var a; { let b = "inner"; } return [a, b]; }
    return f(1, 2);
}, [1, 2]);
test("closures counter", function () {
    function counter() { var n = 0; return { inc: function () { return ++n; }, get: () => n }; }
    var c1 = counter(), c2 = counter();
    c1.inc(); c1.inc(); c2.inc();
    return [c1.get(), c2.get()];
}, [2, 1]);
test("let per iteration", function () {
    var fs = [];
    for (let i = 0; i < 3; i++) fs.push(function () { return i; });
    var gs = [];
    for (var j = 0; j < 3; j++) gs.push(function () { return j; });
    return [fs.map(f => f()), gs.map(g => g())];
}, [[0, 1, 2], [3, 3, 3]]);
test("let per iteration changed in body", function () {
    var fs = [];
    for (let i = 0; i < 6; i++) { fs.push(() => i); i++; }
    return fs.map(f => f());
}, [1, 3, 5]);
test("for-of and for-in let closures", function () {
    var a = [], b = [];
    for (const v of [10, 20]) a.push(() => v);
    for (let k in { p: 1, q: 2 }) b.push(() => k);
    return [a.map(f => f()), b.map(f => f())];
}, [[10, 20], ["p", "q"]]);
test("closure sees later assignment", function () {
    var f = function () { return later; };
    var later = "set after";
    return f();
}, "set after");
test("nested closures", function () {
    function a(x) { return function (y) { return function (z) { return x + y + z; }; }; }
    return a(1)(2)(3);
}, 6);
test("closure over loop body block", function () {
    var fs = [];
    for (var i = 0; i < 3; i++) { let k = i * 10; fs.push(() => k); }
    return fs.map(f => f());
}, [0, 10, 20]);
test("block function declarations", function () {
    var r = [];
    { function inner() { return "in block"; } r.push(inner()); }
    r.push(typeof inner);
    return r;
}, ["in block", "function"]);
test("named function expression", function () {
    var fact = function f(n) { return n <= 1 ? 1 : n * f(n - 1); };
    return [fact(5), typeof f];
}, [120, "undefined"]);
test("global variables", function () {
    globalThis.someGlobal = 41;
    someGlobal++;
    return [someGlobal, typeof window === "object" ? window.someGlobal : someGlobal];
}, [42, 42]);
var topVar = "top";
let topLet = "toplet";
function topFn() { return [topVar, topLet]; }
test("top-level bindings", function () { return topFn(); }, ["top", "toplet"]);
test("undeclared read throws", function () {
    try { nothingByThisName; return "no"; } catch (e) { return e instanceof ReferenceError; }
}, true);
test("implicit global write", function () { (function () { madeGlobal = 7; })(); return globalThis.madeGlobal; }, 7);

/* ---- functions ---- */
test("defaults and rest", function () {
    function f(a, b = a + 1, ...rest) { return [a, b, rest]; }
    return [f(1), f(1, 5), f(1, 2, 3, 4)];
}, [[1, 2, []], [1, 5, []], [1, 2, [3, 4]]]);
test("arguments object", function () {
    function f() { return [arguments.length, arguments[0], Array.prototype.slice.call(arguments, 1)]; }
    function g(a) { var inner = () => arguments[0]; return inner(); }
    return [f(1, 2, 3), g("outer")];
}, [[3, 1, [2, 3]], "outer"]);
test("this binding", function () {
    var o = {
        name: "o",
        regular: function () { return this.name; },
        arrow: function () { return (() => this.name)(); },
        nested: function () { return [1].map(function () { return this; }, "thisArg")[0]; }
    };
    var bare = o.regular;
    return [o.regular(), o.arrow(), String(o.nested()), bare.call({ name: "called" }),
            o.regular.apply({ name: "applied" }), o.regular.bind({ name: "bound" })()];
}, ["o", "o", "thisArg", "called", "applied", "bound"]);
test("recursion", function () { function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } return fib(20); }, 6765);
test("iife", function () { return (function (x) { return x * 2; })(21) + (() => 0)(); }, 42);
test("function properties", function () {
    function named(a, b) {}
    var anon = function () {};
    var arrow = (x) => x;
    return [named.name, anon.name, arrow.name, named.length, arrow.length];
}, ["named", "anon", "arrow", 2, 1]);
test("call spread", function () {
    function f(a, b, c, d) { return [a, b, c, d]; }
    var arr = [2, 3];
    return [f(1, ...arr, 4), Math.max(...[1, 9, 3]), f(..."ab")];
}, [[1, 2, 3, 4], 9, ["a", "b", undefined, undefined]]);
test("new and constructors", function () {
    function P(x) { this.x = x; }
    P.prototype.double = function () { return this.x * 2; };
    function R() { return { replaced: true }; }
    return [new P(4).double(), new R().replaced, new P(1) instanceof P, new P(2).constructor === P];
}, [8, true, true, true]);
test("getters and setters", function () {
    var log = [];
    var o = { _v: 1, get v() { return this._v * 10; }, set v(x) { log.push(x); this._v = x; } };
    o.v = 3;
    var child = Object.create(o);
    child.v = 4;
    return [o.v, child.v, log, Object.keys(child)];
}, [30, 40, [3, 4], ["_v"]]);
test("object literal features", function () {
    var k = "dyn", x = 1;
    var o = { x, [k + "amic"]: 2, method() { return this.x; }, "quoted key": 3, 4: "four", ...{ s: 5 } };
    return [o.x, o.dynamic, o.method(), o["quoted key"], o[4], o.s, Object.keys(o)];
}, [1, 2, 1, 3, "four", 5, ["4", "x", "dynamic", "method", "quoted key", "s"]]);
test("optional chaining", function () {
    var o = { a: { b: () => "called" }, n: null };
    return [o?.a?.b(), o.n?.x, o.missing?.(), o.a.b?.(), o.a?.["b"]()];
}, ["called", undefined, undefined, "called", "called"]);
test("tagged template", function () {
    function tag(strings, ...vals) { return [strings, strings.raw, vals]; }
    var x = 1;
    return tag`a${x}b${x + 1}c\n`;
}, [["a", "b", "c\n"], ["a", "b", "c\\n"], [1, 2]]);
test("template literal", function () { var n = 3; return `n=${n}, sq=${n * n}, ${"nested " + `${n}`}`; }, "n=3, sq=9, nested 3");

/* ---- control flow ---- */
test("if else chain", function () {
    function f(n) { if (n < 0) return "neg"; else if (n === 0) return "zero"; else { return "pos"; } }
    return [f(-1), f(0), f(1)];
}, ["neg", "zero", "pos"]);
test("loops", function () {
    var r = [];
    for (var i = 0; i < 3; i++) r.push(i);
    var j = 0; while (j < 3) r.push(j++ * 10);
    var k = 0; do { r.push(k + 100); } while (++k < 2);
    do r.push("once"); while (false);
    return r;
}, [0, 1, 2, 0, 10, 20, 100, 101, "once"]);
test("break and continue", function () {
    var r = [];
    for (var i = 0; i < 10; i++) { if (i % 2) continue; if (i > 6) break; r.push(i); }
    return r;
}, [0, 2, 4, 6]);
test("labels", function () {
    var r = [];
    outer: for (var i = 0; i < 3; i++) {
        for (var j = 0; j < 3; j++) {
            if (j === 1) continue outer;
            if (i === 2) break outer;
            r.push(i + "" + j);
        }
    }
    block: { r.push("in"); break block; }
    return r;
}, ["00", "10", "in"]);
test("labeled while with let", function () {
    var fs = [];
    loop: for (let i = 0; i < 4; i++) { if (i === 1) continue loop; fs.push(() => i); if (i === 2) break loop; }
    return fs.map(f => f());
}, [0, 2]);
test("switch", function () {
    function f(x) {
        var r = [];
        switch (x) {
        case 1: r.push("one");
        case 2: r.push("two"); break;
        default: r.push("default");
        case 3: r.push("three"); break;
        case "1": r.push("string one");
        }
        return r;
    }
    return [f(1), f(2), f(3), f(9), f("1")];
}, [["one", "two"], ["two"], ["three"], ["default", "three"], ["string one"]]);
test("switch with block scope and closures", function () {
    var fs = [];
    switch (1) { case 1: let v = "captured"; fs.push(() => v); }
    return fs[0]();
}, "captured");
test("break out of switch inside loop", function () {
    var r = [];
    for (var i = 0; i < 3; i++) { switch (i) { case 1: continue; default: r.push(i); } }
    return r;
}, [0, 2]);
test("for-in", function () {
    function P() { this.own = 1; }
    P.prototype.inherited = 2;
    var keys = [];
    for (var k in new P()) keys.push(k);
    var objKeys = [];
    for (var i in { 0: "a", 1: "b" }) objKeys.push(i);
    return [keys, objKeys];
}, [["own", "inherited"], ["0", "1"]]);
test("for-of", function () {
    var r = [];
    for (var v of [1, 2]) r.push(v);
    for (var c of "hé") r.push(c);
    for (var [k, val] of new Map([["m", 1]])) r.push(k + val);
    for (var s of new Set([7, 7, 8])) r.push(s);
    return r;
}, [1, 2, "h", "é", "m1", 7, 8]);
test("for-of closes iterators on break", function () {
    var log = [];
    var iterable = {
        [Symbol.iterator]() {
            var i = 0;
            return { next() { return { value: i++, done: i > 5 }; }, return() { log.push("closed"); return {}; } };
        }
    };
    for (var x of iterable) { log.push(x); if (x === 1) break; }
    try { for (var y of iterable) { throw "boom"; } } catch (e) { log.push(e); }
    return log;
}, [0, 1, "closed", "closed", "boom"]);
test("for with expression init and empty parts", function () {
    var i = 0, r = [];
    for (i = 5; ; ) { if (i > 7) break; r.push(i++); }
    for (; i < 10;) i++;
    return [r, i];
}, [[5, 6, 7], 10]);

/* ---- exceptions ---- */
test("try catch", function () {
    try { throw new Error("oops"); } catch (e) { return [e.message, e instanceof Error, e.name]; }
}, ["oops", true, "Error"]);
test("catch without binding and destructured", function () {
    var r = [];
    try { throw 1; } catch { r.push("no binding"); }
    try { throw { code: 7, why: "x" }; } catch ({ code, why }) { r.push(code, why); }
    return r;
}, ["no binding", 7, "x"]);
test("finally order", function () {
    var log = [];
    function f() { try { log.push("try"); return "ret"; } finally { log.push("finally"); } }
    log.push(f());
    return log;
}, ["try", "finally", "ret"]);
test("finally overrides return", function () {
    function f() { try { return 1; } finally { return 2; } }
    function g() { try { throw 1; } catch (e) { return "caught"; } finally { } }
    function h() { var x = 1; try { return x; } finally { x = 5; } }
    return [f(), g(), h()];
}, [2, "caught", 1]);
test("finally with break and continue", function () {
    var log = [];
    for (var i = 0; i < 3; i++) {
        try { if (i === 0) continue; if (i === 2) break; log.push("body" + i); } finally { log.push("fin" + i); }
    }
    return log;
}, ["fin0", "body1", "fin1", "fin2"]);
test("nested finally", function () {
    var log = [];
    function f() {
        try { try { throw new Error("inner"); } finally { log.push("f1"); } } catch (e) { log.push(e.message); }
        finally { log.push("f2"); }
        return "done";
    }
    log.push(f());
    return log;
}, ["f1", "inner", "f2", "done"]);
test("exception from native and deep calls", function () {
    function deep(n) { if (n === 0) return null.x; return deep(n - 1); }
    var r = [];
    try { deep(5); } catch (e) { r.push(e instanceof TypeError); }
    try { [].reduce((a, b) => a); } catch (e) { r.push(e instanceof TypeError); }
    try { JSON.parse("{bad"); } catch (e) { r.push(e instanceof SyntaxError); }
    return r;
}, [true, true, true]);
test("rethrow and throw non-errors", function () {
    function f() { try { throw "s"; } catch (e) { throw e + "!"; } }
    try { f(); } catch (e) { return e; }
}, "s!");
test("errors in callbacks", function () {
    try { [1, 2].forEach(function (x) { if (x === 2) throw new RangeError("two"); }); }
    catch (e) { return [e.name, e.message]; }
}, ["RangeError", "two"]);
test("stack overflow is catchable", function () {
    function forever() { return forever(); }
    try { forever(); return "no"; } catch (e) { if (!(e instanceof RangeError)) return false; }
    /* Chains of bound functions and of proxies recurse without a user call in between,
     * here on a generator's small stack. */
    var f = function () { return 1; }, C = function () {}, p = {}, h = {}, errs = [];
    for (var i = 0; i < 20000; i++) f = f.bind(null), C = C.bind(null);
    for (var i = 0; i < 20000; i++) p = new Proxy(p, h);
    function* g() {
        try { f(); } catch (e) { errs.push(e.name); }
        try { new C(); } catch (e) { errs.push(e.name); }
        try { p.x; } catch (e) { errs.push(e.name); }
        try { "x" in p; } catch (e) { errs.push(e.name); }
        try { Object.keys(p); } catch (e) { errs.push(e.name); }
    }
    g().next();
    return errs.join() === "RangeError,RangeError,RangeError,RangeError,RangeError" || errs;
}, true);

/* ---- destructuring ---- */
test("array destructuring", function () {
    var [a, , b = 5, ...rest] = [1, 2, undefined, 4, 5];
    var [x, [y, z]] = [1, [2, 3]];
    let [p = "def"] = [];
    return [a, b, rest, x, y, z, p];
}, [1, 5, [4, 5], 1, 2, 3, "def"]);
test("object destructuring", function () {
    var key = "k";
    var { a, b: renamed, c = 3, [key]: computed, d: { e } = { e: "deep" }, ...others } = { a: 1, b: 2, k: "kv", z: 9, y: 8 };
    return [a, renamed, c, computed, e, others];
}, [1, 2, 3, "kv", "deep", { z: 9, y: 8 }]);
test("destructuring assignment", function () {
    var a = 1, b = 2, o = {};
    [a, b] = [b, a];
    ({ x: o.x, y: o["y"] = "dy" } = { x: "ox" });
    var arr = [];
    [arr[0], arr[1]] = "hi";
    return [a, b, o, arr];
}, [2, 1, { x: "ox", y: "dy" }, ["h", "i"]]);
test("destructuring parameters", function () {
    function f({ a, b = 2 } = {}, [c, d] = [3, 4], ...[e]) { return [a, b, c, d, e]; }
    var g = ({ n }) => n * 2;
    return [f({ a: 1 }), f(), g({ n: 21 })];
}, [[1, 2, 3, 4, undefined], [undefined, 2, 3, 4, undefined], 42]);
test("destructuring in loops", function () {
    var r = [];
    for (var { name, n = 0 } of [{ name: "a", n: 1 }, { name: "b" }]) r.push(name + n);
    for (let [i, v] of [[0, "x"], [1, "y"]]) r.push(i + v);
    return r;
}, ["a1", "b0", "0x", "1y"]);
test("destructuring defaults name functions", function () {
    var { f = function () {} } = {};
    var [g = () => 1] = [];
    return [f.name, g.name];
}, ["f", "g"]);
test("destructuring errors", function () {
    try { var { a } = null; return "no"; } catch (e) { return e instanceof TypeError; }
}, true);
test("destructuring closes iterators", function () {
    var log = [];
    var it = { [Symbol.iterator]() { var i = 0; return { next: () => ({ value: i++, done: false }), return() { log.push("ret"); return {}; } }; } };
    var [a, b] = it;
    return [a, b, log];
}, [0, 1, ["ret"]]);

/* ---- spread and arrays ---- */
test("array spread and holes", function () {
    var a = [1, , 3];
    /* join takes the length it starts with, though a toString grows the array */
    var g = [], o = { toString: function () { for (var i = 0; i < 1000; i++) g.push(i); return "o"; } };
    g.push(o, o);
    var big = new Array(5000).fill("x".repeat(65536)), err = "";
    try { big.join(""); } catch (e) { err = e.name; }
    return [[0, ...a, 4], a.length, [..."ab", ...new Set([1, 1, 2])], g.join("-"), g.length, err];
}, [[0, 1, undefined, 3, 4], 3, ["a", "b", 1, 2], "o-o", 2002, "RangeError"]);
test("object spread copies own enumerable", function () {
    var proto = { inherited: 1 };
    var o = Object.create(proto);
    o.own = 2;
    return { ...o, extra: 3 };
}, { own: 2, extra: 3 });

/* ---- classes ---- */
test("class basics", function () {
    class A {
        static count = 0;
        field = "f";
        #secret = 42;
        constructor(x) { this.x = x; A.count++; }
        get secret() { return this.#secret; }
        set secret(v) { this.#secret = v; }
        method() { return this.x + this.field; }
        static make() { return new A("made"); }
        ["computed" + 1]() { return "c1"; }
    }
    var a = new A("x"), b = A.make();
    a.secret = 7;
    return [a.method(), b.x, A.count, a.secret, b.secret, a.computed1(), typeof A, Object.keys(a)];
}, ["xf", "made", 2, 7, 42, "c1", "function", ["field", "x"]]);
test("class inheritance", function () {
    class Animal {
        constructor(name) { this.name = name; }
        speak() { return this.name + " makes a sound"; }
        static kind() { return "animal"; }
    }
    class Dog extends Animal {
        constructor(name) { super(name); this.dog = true; }
        speak() { return super.speak() + " (woof)"; }
        static kind() { return "dog of " + super.kind(); }
    }
    class Puppy extends Dog {}
    var p = new Puppy("rex");
    return [p.speak(), Puppy.kind(), p instanceof Animal, p.dog, Object.getPrototypeOf(Puppy) === Dog];
}, ["rex makes a sound (woof)", "dog of animal", true, true, true]);
test("class fields see this and super", function () {
    class Base { base() { return "b"; } }
    class Derived extends Base {
        a = 1;
        b = this.a + 1;
        c = super.base();
        arrow = () => this.a;
    }
    var d = new Derived();
    var arrow = d.arrow;
    return [d.b, d.c, arrow()];
}, [2, "b", 1]);
test("class static blocks and statics inherited", function () {
    var log = [];
    class A { static x = 1; static { log.push("static " + this.x); } static f() { return this.x; } }
    class B extends A { static x = 2; }
    return [log, B.f(), A.f()];
}, [["static 1"], 2, 1]);
test("class extends builtins", function () {
    class MyArray extends Array { sum() { return this.reduce((a, b) => a + b, 0); } }
    class MyError extends Error { constructor(m) { super(m); this.name = "MyError"; } }
    var a = new MyArray();
    a.push(1, 2, 3);
    var e = new MyError("bad");
    return [a.sum(), a.length, e.message, e.name, e instanceof Error];
}, [6, 3, "bad", "MyError", true]);
test("class constructor without new throws", function () {
    class C {}
    try { C(); return "no"; } catch (e) { return e instanceof TypeError; }
}, true);
test("class expressions and name scope", function () {
    var K = class Inner { who() { return Inner.name; } };
    return [new K().who(), typeof Inner];
}, ["Inner", "undefined"]);
test("accessors in classes", function () {
    class T { static get s() { return "static getter"; } get g() { return "getter"; } }
    return [T.s, new T().g];
}, ["static getter", "getter"]);

/* ---- iterators and generators ---- */
test("generators", function () {
    function* g() { var x = yield 1; yield x * 2; return "end"; }
    var it = g();
    return [it.next(), it.next(5), it.next(), it.next()];
}, [{ value: 1, done: false }, { value: 10, done: false }, { value: "end", done: true }, { value: undefined, done: true }]);
test("generator spread and yield*", function () {
    function* inner() { yield "a"; yield "b"; }
    function* outer() { yield 0; yield* inner(); yield* [1, 2]; }
    return [...outer()];
}, [0, "a", "b", 1, 2]);
test("generator return and finally", function () {
    var log = [];
    function* g() { try { yield 1; yield 2; } finally { log.push("cleanup"); } }
    var it = g();
    it.next();
    var r = it.return("early");
    return [r, log, it.next()];
}, [{ value: "early", done: true }, ["cleanup"], { value: undefined, done: true }]);
test("generator throw into try", function () {
    function* g() { while (true) { try { yield "waiting"; } catch (e) { yield "caught " + e; } } }
    var it = g();
    it.next();
    return [it.throw("x").value, it.next().value];
}, ["caught x", "waiting"]);
test("generator methods and infinite", function () {
    var o = { *gen() { var i = 0; while (true) yield i++; } };
    var r = [];
    for (var v of o.gen()) { if (v > 3) break; r.push(v); }
    return r;
}, [0, 1, 2, 3]);
test("custom iterables", function () {
    class Range { constructor(n) { this.n = n; } *[Symbol.iterator]() { for (let i = 0; i < this.n; i++) yield i; } }
    return [[...new Range(3)], Array.from(new Range(2))];
}, [[0, 1, 2], [0, 1]]);

/* ---- async ---- */
asyncTest("async await", async function () {
    var log = [];
    async function a() { log.push("a start"); await null; log.push("a after"); return "a"; }
    var p = a();
    log.push("sync");
    var v = await p;
    log.push(v);
    return log;
}, ["a start", "sync", "a after", "a"]);
asyncTest("async exceptions", async function () {
    async function fail() { await 1; throw new Error("async fail"); }
    try { await fail(); return "no"; } catch (e) { return e.message; }
}, "async fail");
asyncTest("await in loops and finally", async function () {
    var r = [];
    for (let i = 0; i < 3; i++) { try { r.push(await Promise.resolve(i)); } finally { r.push("f" + i); } }
    return r;
}, [0, "f0", 1, "f1", 2, "f2"]);
asyncTest("async arrows and methods", async function () {
    var f = async (x) => x * 2;
    var o = { async m() { return await f(4); } };
    class C { async run() { return (await o.m()) + 1; } }
    return await new C().run();
}, 9);
asyncTest("async generators and for await", async function () {
    async function* g() { yield 1; await null; yield 2; }
    var r = [];
    for await (var v of g()) r.push(v);
    for await (var w of [Promise.resolve("p"), "plain"]) r.push(w);
    return r;
}, [1, 2, "p", "plain"]);
asyncTest("microtask order", async function () {
    var log = [];
    Promise.resolve().then(() => log.push("then1"));
    queueMicrotask(() => log.push("micro"));
    await null;
    log.push("after await");
    await new Promise(r => setTimeout ? Promise.resolve().then(r) : r());
    return log;
}, ["then1", "micro", "after await"]);
asyncTest("promise all and chains", async function () {
    var v = await Promise.all([1, Promise.resolve(2), (async () => 3)()]);
    var chained = await Promise.resolve(1).then(x => x + 1).then(x => { throw x; }).catch(x => x * 10);
    return [v, chained];
}, [[1, 2, 3], 20]);

/* ---- built-in interplay ---- */
test("eval and Function", function () {
    var g = eval("var evalVar = 3; evalVar * 2");
    var f = new Function("a", "b", "return a + b");
    return [g, globalThis.evalVar, f(2, 3), eval("1 + 1"), eval(5)];
}, [6, 3, 5, 2, 5]);
test("json round trip", function () {
    var o = { a: [1, "two", { three: true }], b: null };
    return JSON.parse(JSON.stringify(o));
}, { a: [1, "two", { three: true }], b: null });
test("regex literals", function () {
    var m = /(\d+)-(?<word>[a-z]+)/.exec("x 42-abc y");
    return [m[1], m.groups.word, "a1b2".replace(/\d/g, d => d * 2), /^re$/i.test("RE")];
}, ["42", "abc", "a2b4", true]);
test("sort with comparator and closures", function () {
    var people = [{ n: "b", a: 2 }, { n: "a", a: 2 }, { n: "c", a: 1 }];
    return people.sort((x, y) => x.a - y.a || (x.n < y.n ? -1 : 1)).map(p => p.n);
}, ["c", "a", "b"]);
test("symbols as keys", function () {
    var s = Symbol("k");
    var o = { [s]: 1, plain: 2 };
    return [o[s], Object.keys(o)];
}, [1, ["plain"]]);
test("many properties", function () {
    var o = {};
    for (var i = 0; i < 300; i++) o["k" + i] = i;
    delete o.k5;
    o.k5 = "back";
    var keys = Object.keys(o);
    var sum = 0;
    for (var j = 0; j < 300; j++) if (typeof o["k" + j] === "number") sum += o["k" + j];
    return [keys.length, keys[0], keys[299], o.k5, sum, "k150" in o, "k300" in o];
}, [300, "k0", "k5", "back", 44845, true, false]);
test("prototype chain lookups", function () {
    var a = { x: 1 }, b = Object.create(a), c = Object.create(b);
    b.y = 2;
    c.z = 3;
    a.x = 10;
    var cyclic = [];
    try { Object.setPrototypeOf(a, a); } catch (e) { cyclic.push(e.name); }
    try { Object.setPrototypeOf(a, c); } catch (e) { cyclic.push(e.name); }
    return [c.x, c.y, c.z, c.hasOwnProperty("x"), "x" in c, cyclic, Reflect.setPrototypeOf(a, c), "q" in c];
}, [10, 2, 3, false, true, ["TypeError", "TypeError"], false, false]);
test("string methods through primitives", function () {
    var s = "Hello";
    return [s.length, s[1], s.toUpperCase(), s.charCodeAt(0), "a,b".split(","), s.at(-1)];
}, [5, "e", "HELLO", 72, ["a", "b"], "o"]);
test("completion of loops in eval", function () { return eval("for (var i = 0; i < 3; i++) i * 2;"); }, 4);
test("deep expression nesting", function () {
    var x = 1, errs = [];
    /* Too deep to parse is an error, not a crash: scripts (on a generator's
     * small stack, to be sure to reach its end), patterns and selectors. */
    function* g() {
        try { eval("[".repeat(100000) + "]".repeat(100000)); } catch (e) { errs.push(e.name); }
        try { eval("!".repeat(100000) + "1"); } catch (e) { errs.push(e.name); }
    }
    g().next();
    try { new RegExp("(".repeat(5000) + ")".repeat(5000)); } catch (e) { errs.push(e.name); }
    var css = [CSS.supports("selector(x:lang(((") + "", CSS.supports("selector(x:lang(") + "",
               CSS.supports("selector(:lang(en))"), CSS.supports("selector(" + ":is(".repeat(100) + "a" + ")".repeat(100) + ")")];
    return [((((((((((x + 1) * 2) - 3) / 1) + 4) * 5) - 6) / 2) + 7) * 1), errs, css[2], css[3]];
}, [16.5, ["SyntaxError", "SyntaxError", "SyntaxError"], true, false]);
test("long loop", function () { var s = 0; for (var i = 0; i < 100000; i++) s += i; return s; }, 4999950000);
test("closures in hot loop", function () {
    var total = 0;
    var add = (n) => { total += n; };
    for (let i = 0; i < 1000; i++) add(i);
    return total;
}, 499500);

/* ---- bytecode control-flow and scope regressions ---- */
test("switch case closures use the active lexical environment", function () {
    let x = 7, g;
    switch (1) {
    case (g = () => x, 1):
        let y = 2;
        let h = () => y;
        return [g(), h()];
    }
}, [7, 2]);
test("defaults cannot see hoisted body declarations", function () {
    let x = 7;
    function f(a = x) { var x = 2; return a; }
    function g(a = () => x) { let x = 3; return a(); }
    return [f(), g()];
}, [7, 7]);
test("defaults and body redeclarations have separate environments", function () {
    function f(a = 1, get = () => a) { var a = 9; return [a, get()]; }
    function g(a = 3) { var a; return a; }
    return [f(), g()];
}, [[9, 1], 3]);
test("var arguments keeps the arguments object", function () {
    function f() { var arguments; return arguments[0]; }
    function g(a = 0) { var arguments; return () => arguments[0]; }
    function h(arguments) { return arguments; }
    return [f(7), g(8)(), h(9)];
}, [7, 8, 9]);
test("computed instance keys run once at class definition", function () {
    let n = 0, read = () => "k" + n++;
    class C { [read()] = 4; }
    var a = new C(), b = new C();
    return [n, a.k0, b.k0];
}, [1, 4, 4]);
test("computed instance keys can capture the surrounding scope", function () {
    let x = "field", calls = 0;
    class C { [(() => { calls++; return x; })()] = 6; }
    return [new C().field, calls];
}, [6, 1]);
test("computed instance key exceptions happen at definition", function () {
    try { class C { [(() => {throw 12;})()] = 1; } }
    catch (e) { return e; }
}, 12);
test("for-in catches errors from proxy enumeration", function () {
    try {
        for (let k in new Proxy({}, {ownKeys() { throw 4; }})) {}
        return "missed";
    } catch (e) { return e; }
}, 4);
test("failed destructuring closes its generator", function () {
    let closed = 0;
    function* gen() { try { yield null; } finally { closed++; } }
    try { let [{x}] = gen(); } catch (e) {}
    return closed;
}, 1);
test("failed default and parameter destructuring close iterators", function () {
    let closed = 0;
    function* gen() { try { yield undefined; } finally { closed++; } }
    function fail() { throw 5; }
    function f([a = fail()]) {}
    try { let [a = fail()] = gen(); } catch (e) {}
    try { f(gen()); } catch (e) {}
    return closed;
}, 2);
test("shared finally preserves return break continue and throw", function () {
    var log = [];
    function f() {
        for (let i = 0; i < 3; i++) {
            try { if (i === 0) continue; if (i === 1) break; }
            finally { log.push(i); }
        }
        try { return 7; } finally { log.push("return"); }
    }
    var value = f();
    try { try { throw "x"; } finally { log.push("throw"); } }
    catch (e) { log.push(e); }
    return [value, log];
}, [7, [0, 1, "return", "throw", "x"]]);
test("finally can replace a return or throw", function () {
    function f() { try { return 1; } finally { return 2; } }
    function g() { try { throw 3; } finally { return 4; } }
    function h() { try { return 5; } finally { throw 6; } }
    let caught;
    try { h(); } catch (e) { caught = e; }
    return [f(), g(), caught];
}, [2, 4, 6]);
test("a generator resumes inside a shared finally", function () {
    function* g() { try { yield 1; } finally { yield 2; } }
    var it = g();
    return [it.next(), it.return(9), it.next()];
}, [{value:1,done:false}, {value:2,done:false}, {value:9,done:true}]);
test("deeply nested generator bodies compile safely", function () {
    var body = "if(false){".repeat(180) + "0;" + "}".repeat(180);
    var make = new Function("return function*(){" + body + "yield 42;}");
    return make()().next().value;
}, 42);
test("nested finally compilation stays bounded", function () {
    var body = "";
    for (var i = 0; i < 20; i++) body = "try {} finally {" + body + "}";
    return new Function(body + "return 8;")();
}, 8);
test("ASCII search handles strings shorter than the needle", function () {
    return ["".indexOf("abc"), "a".indexOf("long needle"), "abc".indexOf("bc", 2),
            "abc".indexOf("", 99), "éa".indexOf("a"), "😀x".indexOf("x")];
}, [-1, -1, -1, 3, 1, 2]);
test("for initializer closures keep a separate iteration environment", function () {
    let init, body;
    for (let i = 0, f = () => i; i < 1; i++) {
        init = f;
        body = () => i;
        i = 7;
    }
    return [init(), body()];
}, [0, 7]);

/* ---- symbols and BigInt as their own types ---- */
test("symbols are unique values", function () {
    var a = Symbol("x"), b = Symbol("x"), r = Symbol.for("app"), o = { [a]: 1, [b]: 2, s: 3 };
    o[Symbol.iterator] = function* () { yield 9; };
    return [typeof a, a === b, a == b, Symbol.for("app") === r, Symbol.keyFor(r), Symbol.keyFor(a),
            a.description, Symbol().description, String(a), a.toString(), o[a] + o[b], [...o],
            Object.keys(o), Object.getOwnPropertyNames(o), Object.getOwnPropertySymbols(o).length,
            Reflect.ownKeys(o).length, JSON.stringify(o), JSON.stringify({ k: a }),
            Object.prototype.toString.call(a), "@@iterator" in o];
}, ["symbol", false, false, true, "app", undefined, "x", undefined, "Symbol(x)", "Symbol(x)", 3, [9],
    ["s"], ["s"], 3, 4, '{"s":3}', "{}", "[object Symbol]", false]);
test("symbol conversions and copies", function () {
    var s = Symbol("t"), errors = 0;
    try { "" + s; } catch (e) { errors += e instanceof TypeError; }
    try { `${s}`; } catch (e) { errors += e instanceof TypeError; }
    try { s * 2; } catch (e) { errors += e instanceof TypeError; }
    var src = { [s]: "v", n: 1 }, spread = { ...src }, assigned = Object.assign({}, src);
    var tagged = { [Symbol.toStringTag]: "Thing" };
    var prim = { [Symbol.toPrimitive](hint) { return hint === "string" ? "str" : 5; } };
    return [errors, spread[s], assigned[s], Object.prototype.toString.call(tagged), `${prim}`, prim + 1,
            !!s, Object(1) instanceof Number, Object.getPrototypeOf(s) === Symbol.prototype];
}, [3, "v", "v", "[object Thing]", "str", 6, true, true, true]);
test("bigint arithmetic", function () {
    var big = 2n ** 100n;
    return [typeof 1n, String(big), String(big / 3n), String(big % 7n), String(-big / 3n), String(-7n % 3n),
            String(123456789012345678901234567890n * 987654321098765432109876543210n),
            String(0xffn + 0o17n + 0b11n), String(1_000_000n), String(-(5n)), String(10n - 15n),
            (big * big / big) === big, 5n == 5, 5n === 5, 5n == "5", 2n < 3, 2n > 1.5, 10n > 9n, -3n < -2n,
            [3n, 1n, 2n].sort().map(String), String(big - 1n).length];
}, ["bigint", "1267650600228229401496703205376", "422550200076076467165567735125",
    "2", "-422550200076076467165567735125", "-1",
    "121932631137021795226185032733622923332237463801111263526900",
    "273", "1000000", "-5", "-5", true, true, false, true, true, true, true, true, ["1", "2", "3"], 31]);
test("bigint bitwise and conversions", function () {
    var errors = 0;
    try { 1n + 1; } catch (e) { errors += e instanceof TypeError; }
    try { +1n; } catch (e) { errors += e instanceof TypeError; }
    try { 1n / 0n; } catch (e) { errors += e instanceof RangeError; }
    try { JSON.stringify({ a: 1n }); } catch (e) { errors += e instanceof TypeError; }
    try { BigInt(1.5); } catch (e) { errors += e instanceof RangeError; }
    try { BigInt("12x"); } catch (e) { errors += e instanceof SyntaxError; }
    var i = 5n;
    i++;
    i += 2n;
    return [errors, String(i), String(-5n & 0xffn), String(-5n | 2n), String(5n ^ -1n), String(~5n), String(1n << 70n),
            String(-17n >> 2n), String(-1n >> 200n), String(BigInt.asUintN(64, -1n)), String(BigInt.asIntN(8, 255n)),
            String(BigInt.asIntN(64, 2n ** 63n)), BigInt("  -42  ") === -42n, BigInt(true) === 1n, Number(2n ** 64n),
            (255n).toString(16), (-255n).toString(2), BigInt(Number.MAX_SAFE_INTEGER) + 2n === 9007199254740993n,
            0n ? 1 : 2, 1n && "t", (1234567n).toLocaleString(), `${10n}`, "" + 7n, Object.prototype.toString.call(1n),
            BigInt(1e21).toString(), Number(123456789012345678901234567890n)];
}, [6, "8", "251", "-5", "-6", "-6", "1180591620717411303424", "-5", "-1", "18446744073709551615", "-1",
    "-9223372036854775808", true, true, 18446744073709551616, "ff", "-11111111", true, 2, "t", "1,234,567", "10",
    "7", "[object BigInt]", "1000000000000000000000", 1.2345678901234568e+29]);
test("bigint typed arrays and DataView", function () {
    var a = new BigInt64Array(2), u = new BigUint64Array(a.buffer), dv = new DataView(new ArrayBuffer(8)), err = 0;
    a[0] = -1n;
    a[1] = 2n ** 63n;
    try { a[0] = 1; } catch (e) { err += e instanceof TypeError; }
    dv.setBigUint64(0, 0x0102030405060708n);
    return [String(u[0]), String(a[1]), err, dv.getUint8(0), String(dv.getBigInt64(0, true)),
            [...BigInt64Array.of(3n, 1n, 2n).sort()].map(String), BigInt64Array.BYTES_PER_ELEMENT];
}, ["18446744073709551615", "-9223372036854775808", 1, 1, "578437695752307201", ["1", "2", "3"], 8]);

/* ---- Unicode text and regular expressions ---- */
test("unicode case mapping, trim and normalize", function () {
    return ["Straße".toUpperCase(), "ÉCOLE ΣΟΦΟΣ".toLowerCase(), "ǆ".toUpperCase(), "İ".toLowerCase().length,
            " 　 hi ﻿".trim(), "é".normalize() === "é", "é".normalize("NFD").length,
            "Crème Brûlée".normalize("NFD").replace(/[̀-ͯ]/g, ""), "ﬁ①".normalize("NFKC"),
            "한".normalize("NFD").length, "한".normalize() === "한",
            "ạ̇".normalize("NFC") === "ạ̇".normalize("NFD").normalize("NFC"),
            "ḍ̇".normalize() === "ḍ̇"];
}, ["STRASSE", "école σοφος", "Ǆ", 2, "hi", true, 2, "Creme Brulee", "fi1", 3, true, true, true]);
test("unicode regular expressions", function () {
    return ["héllo wörld 日本".match(/\p{L}+/gu), /^\p{Lu}/u.test("Érable"), "a1٣".replace(/\p{Nd}/gu, "#"),
            /\p{Script=Greek}/u.test("λ"), /\p{sc=Cyrl}+/u.exec("Привет!")[0], /^\P{L}$/u.test("5"),
            "😀 hi 🎉".match(/\p{Extended_Pictographic}/gu).length, /[Ѐ-ӿ]/.test("ж"),
            /[^\u0000-\u007f]/.test("abc"), /[^\u0000-\u007f]/.test("abcé"), /ÉCOLE/i.test("école"),
            /[à-ÿ]+/i.exec("ÀÉÎ")[0], /(é)\1/i.test("éÉ"), /\s+/.test("　"), "a b".match(/./g).length,
            /[[a-z]--[aeiou]]+/v.exec("hello")[0], /[\p{L}&&\p{ASCII}]+/v.exec("éabcé")[0], /[^]/.test("\n"),
            /[]/.test("a"), /\u{1F600}/u.test("😀"), "x😀y".match(/./gu).length, /😀/.test("😀"),
            /\p{Letter}/u.test("ß"), /\p{White_Space}/u.test(" "), /[\p{N}]/u.test("Ⅻ")];
}, [["héllo", "wörld", "日本"], true, "a##", true, "Привет", true, 2, true, false, true, true, "ÀÉÎ", true,
    true, 2, "h", "abc", true, false, true, 3, true, true, true, true]);
