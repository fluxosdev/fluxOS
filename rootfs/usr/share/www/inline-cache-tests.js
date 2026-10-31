/* The VM's shapes and inline caches: each test warms the same instructions in a loop, then
 * changes what they relied on (shadowing, prototypes, accessors, deletes, proxies, host
 * objects, globals) and checks they notice. Run on the host by make test-js and make
 * test-browser (tests/browser-dom.c supplies test()). */

function warm(f, arg) {
    for (var i = 0; i < 20; i++) f(arg);
    return f(arg);
}

test("ic: own properties of several shapes", function () {
    function getX(o) { return o.x; }
    var objs = [{ x: 1 }, { y: 2, x: 3 }, { x: 4, y: 5 }, { a: 0, b: 0, x: 6 }], r = [];
    for (var k = 0; k < 3; k++)
        for (var i = 0; i < objs.length; i++) r.push(getX(objs[i]));
    return r.slice(0, 4).concat([getX({}), getX({ x: undefined, z: 1 })]);
}, [1, 3, 4, 6, null, null]);

test("ic: prototype hits notice shadowing and replacement", function () {
    function Base() {}
    Base.prototype.f = 1;
    function Mid() {}
    Mid.prototype = Object.create(Base.prototype);
    function get(o) { return o.f; }
    var a = new Mid(), b = new Mid(), r = [];
    r.push(warm(get, a));
    b.f = 2;                                    /* own, on one object */
    r.push(get(a), get(b));
    Mid.prototype.f = 3;                        /* nearer prototype */
    r.push(get(a), get(b));
    delete Mid.prototype.f;
    Base.prototype.f = 4;                       /* replaced in place */
    r.push(get(a));
    delete Base.prototype.f;
    r.push(get(a));
    return r;
}, [1, 1, 2, 3, 2, 4, null]);

test("ic: missing keys appear later", function () {
    function P() {}
    function get(o) { return o.g; }
    var o = new P(), r = [];
    r.push(warm(get, o) === undefined);
    P.prototype.g = 5;
    r.push(get(o));
    Object.prototype.zzIcTest = 1;
    r.push(warm(function (v) { return v.zzIcTest; }, {}));
    delete Object.prototype.zzIcTest;
    r.push(warm(function (v) { return v.zzIcTest; }, {}) === undefined);
    return r;
}, [true, 5, 1, true]);

test("ic: setPrototypeOf after warming", function () {
    var p1 = { v: "one" }, p2 = { v: "two" };
    function get(o) { return o.v; }
    var o = Object.create(p1), r = [warm(get, o)];
    Object.setPrototypeOf(o, p2);
    r.push(get(o));
    var mid = Object.create(p1), leaf = Object.create(mid);
    r.push(warm(get, leaf));
    Object.setPrototypeOf(mid, p2);             /* a prototype on the way changes its own */
    r.push(get(leaf));
    return r;
}, ["one", "two", "one", "two"]);

test("ic: getters and their replacement", function () {
    class V {
        constructor(x) { this.x = x; }
        get double() { return this.x * 2; }
    }
    function get(o) { return o.double; }
    var r = [warm(get, new V(4))];
    Object.defineProperty(V.prototype, "double", { get() { return this.x * 3; }, configurable: true });
    r.push(get(new V(4)));
    var o = new V(1);
    Object.defineProperty(o, "x", { get() { return 10; }, configurable: true });
    r.push(get(o), warm(function (v) { return v.x; }, o));
    return r;
}, [8, 12, 30, 10]);

test("ic: adding a key notices setters added later", function () {
    var log = [];
    function P() {}
    function setZ(o) { o.z = 1; return Object.keys(o).length; }
    var r = [warm(function () { return setZ(new P()); })];
    Object.defineProperty(P.prototype, "z", { set(v) { log.push(v); }, configurable: true });
    r.push(setZ(new P()), log.length);
    var q = { w: 0 }, plain = {};
    function setW(o) { o.w = 7; return Object.keys(o); }
    warm(function () { return setW({}); });
    Object.setPrototypeOf(plain, Object.create(q, { w: { set(v) { log.push("w" + v); } } }));
    r.push(setW(plain), log);
    return r;
}, [1, 0, 1, [], [1, "w7"]]);

test("ic: own writes notice accessors", function () {
    var log = [];
    function setX(o, v) { o.x = v; }
    var a = { x: 0 }, b = { x: 0 };
    for (var i = 0; i < 20; i++) setX(a, i);
    Object.defineProperty(b, "x", { get() { return "got"; }, set(v) { log.push(v); }, configurable: true });
    setX(b, "s");
    return [a.x, b.x, log];
}, [19, "got", ["s"]]);

test("ic: deletes move keys", function () {
    function get(o) { return [o.a, o.b, o.c]; }
    var o = { a: 1, b: 2, c: 3 }, r = [warm(get, o)];
    delete o.a;
    r.push(get(o));
    o.a = 4;
    r.push(get(o), Object.keys(o));
    var p = { a: 1, b: 2, c: 3 };
    delete p.c;                                 /* the last key: back to the earlier shape */
    r.push(get(p), warm(get, { a: 5, b: 6 }));
    return r;
}, [[1, 2, 3], [null, 2, 3], [4, 2, 3], ["b", "c", "a"], [1, 2, null], [5, 6, null]]);

test("ic: length of arrays, strings and the rest", function () {
    function len(v) { return v.length; }
    function args() { return len(arguments); }
    var r = [];
    for (var i = 0; i < 20; i++) len([1, 2]);
    r.push(len([1, 2, 3]), len("abcd"), len("\u{1F600}"), len({ length: 7 }), args(1, 2), len(function (a, b, c) {}));
    r.push(len(new Uint8Array(5)), len(new String("xy")));
    return r;
}, [3, 4, 2, 7, 2, 3, 5, 2]);

test("ic: primitives' methods", function () {
    function up(s) { return s.toUpperCase(); }
    function fix(n) { return n.toFixed(1); }
    var r = [warm(up, "ab"), warm(fix, 2), warm(function (b) { return b.toString(); }, true)];
    var saved = String.prototype.toUpperCase;
    String.prototype.toUpperCase = function () { return "patched"; };
    r.push(up("ab"));
    String.prototype.toUpperCase = saved;
    r.push(up("cd"), up(new String("ef")));
    return r;
}, ["AB", "2.0", "true", "patched", "CD", "EF"]);

test("ic: globals and their changes", function () {
    globalThis.icDyn = 1;
    function rd() { return icDyn; }
    function td() { return typeof icLater; }
    var r = [warm(rd), warm(td)];
    globalThis.icDyn = 2;
    r.push(rd());
    delete globalThis.icDyn;
    try { rd(); r.push("no error"); } catch (e) { r.push(e instanceof ReferenceError); }
    Object.defineProperty(globalThis, "icDyn", { get() { return 9; }, configurable: true });
    r.push(rd());
    globalThis.icLater = 1;
    r.push(td());
    var log = [];
    globalThis.icW = 0;
    function wr(v) { icW = v; }
    for (var i = 0; i < 20; i++) wr(i);
    r.push(globalThis.icW);
    Object.defineProperty(globalThis, "icW", { set(v) { log.push(v); }, get() { return "acc"; }, configurable: true });
    wr("x");
    r.push(log, icW);
    delete globalThis.icDyn; delete globalThis.icLater; delete globalThis.icW;
    return r;
}, [1, "undefined", 2, true, 9, "number", 19, ["x"], "acc"]);

test("ic: object literals", function () {
    function mk(a) { return { p: a, q: a + 1, p2: a * 2 }; }
    var r = [];
    for (var i = 0; i < 20; i++) r.push(mk(i));
    var dup = function (a) { return { k: 1, k: a }; };
    return [r[19], Object.keys(r[3]), warm(dup, 5), Object.keys(dup(1))];
}, [{ p: 19, q: 20, p2: 38 }, ["p", "q", "p2"], { k: 5 }, ["k"]]);

test("ic: proxies and maps at warm sites", function () {
    function get(o) { return o.k; }
    var r = [warm(get, { k: 1 }), warm(get, {}) === undefined];
    r.push(get(new Proxy({}, { get: function () { return "trap"; } })));
    r.push(get(new Proxy({ k: 2 }, {})));
    var log = [];
    function set(o) { o.k = 3; }
    warm(function () { set({}); });
    set(new Proxy({}, { set: function (t, k, v) { log.push(k + v); return true; } }));
    r.push(log);
    function size(m) { return m.size; }
    var m = new Map();
    r.push(warm(size, m));
    m.set(1, 1); m.set(2, 2);
    r.push(size(m), size(new Set([1, 2, 3])));
    return r;
}, [1, true, "trap", 2, ["k3"], 0, 2, 3]);

test("ic: host objects at warm sites", function () {
    function tag(o) { return o.tagName; }
    var r = [warm(tag, { tagName: "X" })];
    var div = document.createElement("div");
    r.push(tag(div));
    function id(o) { o.id = "n"; return o.id; }
    warm(function () { return id({}); });
    r.push(id(div), div.getAttribute("id"));
    return r;
}, ["X", "DIV", "n", "n"]);

test("ic: functions' own and built-in keys", function () {
    function nm(f) { return f.name; }
    function foo() {}
    var r = [warm(nm, { name: "obj" }), nm(foo), nm(function bar() {})];
    function pr(f) { return typeof f.prototype; }
    r.push(warm(pr, foo), pr(function () {}), pr(() => 1));
    return r;
}, ["obj", "foo", "bar", "object", "object", "undefined"]);

test("ic: many keys and owned shapes", function () {
    function build(n) {
        var o = {};
        for (var i = 0; i < n; i++) o["k" + i] = i;
        return o;
    }
    function get(o) { return o.k5 + o.k70 + (o.k150 || 0); }
    var big = build(200), r = [warm(get, big)];
    delete big.k70;
    r.push(get(big));
    big.k70 = 1000;
    r.push(get(big), Object.keys(big).length, Object.keys(big)[199]);
    var small = build(80);
    r.push(warm(get, small));
    return r;
}, [225, NaN, 1155, 200, "k70", 75]);

test("ic: constructors with branching shapes", function () {
    function Pt(i) {
        if (i % 2) { this.x = i; this.y = 1; } else { this.y = 2; this.x = i; }
        if (i % 3 === 0) this.z = 1;
    }
    function sum(o) { return o.x + o.y + (o.z || 0); }
    var t = 0;
    for (var i = 0; i < 300; i++) t += sum(new Pt(i));
    return [t, Object.keys(new Pt(1)), Object.keys(new Pt(2)), Object.keys(new Pt(3))];
}, [45400, ["x", "y"], ["y", "x"], ["x", "y", "z"]]);

test("ic: class hierarchies", function () {
    class A { m() { return "A"; } get kind() { return "a"; } }
    class B extends A { }
    class C extends B { m() { return "C" + super.m(); } }
    function call(o) { return o.m() + o.kind; }
    var r = [warm(call, new B()), warm(call, new C())];
    B.prototype.m = function () { return "B"; };
    r.push(call(new B()), call(new C()));
    Object.defineProperty(B.prototype, "kind", { get() { return "b"; }, configurable: true });
    r.push(call(new C()));
    return r;
}, ["Aa", "CAa", "Ba", "CBa", "CBb"]);

test("ic: a refill that gives up leaves nothing behind", function () {
    function get(o) { return o.icNothingHere; }
    var plain = {}, div = document.createElement("div");
    var r = [warm(get, plain) === undefined, get(div) === undefined, get(plain) === undefined];
    function m(o) { return o.toString === Object.prototype.toString; }
    r.push(warm(m, plain), m(div), m(plain));
    return r;
}, [true, true, true, true, false, true]);
