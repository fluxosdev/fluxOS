/*
 * Library code written in JavaScript, run once when a page's engine starts
 * (jslib2.c): iterator objects for Map, Set and Array, newer methods of
 * Array, Object and Math, Reflect, WeakMap, EventTarget, URL and
 * URLSearchParams, Headers, Response, fetch (on XMLHttpRequest),
 * AbortController, structuredClone, atob and btoa, a small Intl and the
 * console's extras.
 *
 * It goes through the C preprocessor to become a string, so it must stay
 * inside what that can read: no single quotes and no backticks (strings
 * use double quotes), nothing that needs a line break to end a statement
 * (every statement ends in a semicolon), and "//" only in comments.
 */
#define JS_SOURCE(...) #__VA_ARGS__

static const char js_core[] = JS_SOURCE(
(function (g) {
  var def = function (o, k, v) {
    Object.defineProperty(o, k, { value: v, writable: true, configurable: true, enumerable: false });
  };
  var ITER = Symbol.iterator;
  /* ---- iterators: Iterator.prototype and its helpers (map, filter, take...) ---- */
  function Iterator() {
    if (new.target === undefined || new.target === Iterator) throw new TypeError("Iterator cannot be constructed");
  }
  var IP = Iterator.prototype;
  def(IP, ITER, function () { return this; });
  Object.defineProperty(IP, Symbol.toStringTag, { value: "Iterator", writable: true, configurable: true });
  var HP = Object.create(IP);
  function helper(src, step) {
    var h = Object.create(HP);
    def(h, "_src", src);
    def(h, "_step", step);
    def(h, "_done", false);
    return h;
  }
  def(HP, "next", function () {
    if (this._done) return { value: undefined, done: true };
    var r = this._step();
    if (r.done) this._done = true;
    return r;
  });
  def(HP, "return", function () {
    if (!this._done) {
      this._done = true;
      var ret = this._src["return"];
      if (typeof ret === "function") ret.call(this._src);
    }
    return { value: undefined, done: true };
  });
  Object.defineProperty(HP, Symbol.toStringTag, { value: "Iterator Helper", configurable: true });
  function fnArg(f) { if (typeof f !== "function") throw new TypeError(f + " is not a function"); return f; }
  function countArg(n) {
    n = Number(n);
    if (n !== n || n < 0) throw new RangeError(n + " must be a positive number");
    return n === Infinity ? n : Math.floor(n);
  }
  var DONE = { value: undefined, done: true };
  def(IP, "map", function (f) {
    var src = this, i = 0; fnArg(f);
    return helper(src, function () { var r = src.next(); return r.done ? DONE : { value: f(r.value, i++), done: false }; });
  });
  def(IP, "filter", function (f) {
    var src = this, i = 0; fnArg(f);
    return helper(src, function () {
      for (;;) { var r = src.next(); if (r.done) return DONE; if (f(r.value, i++)) return { value: r.value, done: false }; }
    });
  });
  def(IP, "take", function (n) {
    var src = this, left = countArg(n);
    return helper(src, function () {
      if (left <= 0) { var ret = src["return"]; if (typeof ret === "function") ret.call(src); return DONE; }
      left--;
      var r = src.next(); return r.done ? DONE : { value: r.value, done: false };
    });
  });
  def(IP, "drop", function (n) {
    var src = this, left = countArg(n);
    return helper(src, function () {
      while (left > 0) { left--; if (src.next().done) return DONE; }
      var r = src.next(); return r.done ? DONE : { value: r.value, done: false };
    });
  });
  def(IP, "flatMap", function (f) {
    var src = this, i = 0, inner = null; fnArg(f);
    return helper(src, function () {
      for (;;) {
        if (inner) { var x = inner.next(); if (!x.done) return { value: x.value, done: false }; inner = null; }
        var r = src.next();
        if (r.done) return DONE;
        var m = f(r.value, i++);
        if (m === null || (typeof m !== "object" && typeof m !== "function")) throw new TypeError("flatMap: not an iterable or iterator");
        inner = typeof m[ITER] === "function" ? m[ITER]() : m;
      }
    });
  });
  def(IP, "reduce", function (f) {
    fnArg(f);
    var acc, i = 0, r;
    if (arguments.length > 1) acc = arguments[1];
    else { r = this.next(); if (r.done) throw new TypeError("Reduce of empty iterator with no initial value"); acc = r.value; i = 1; }
    while (!(r = this.next()).done) acc = f(acc, r.value, i++);
    return acc;
  });
  def(IP, "toArray", function () { var a = [], r; while (!(r = this.next()).done) a.push(r.value); return a; });
  def(IP, "forEach", function (f) { fnArg(f); var r, i = 0; while (!(r = this.next()).done) f(r.value, i++); });
  function closeIter(it) { var ret = it["return"]; if (typeof ret === "function") ret.call(it); }
  def(IP, "some", function (f) {
    fnArg(f); var r, i = 0;
    while (!(r = this.next()).done) if (f(r.value, i++)) { closeIter(this); return true; }
    return false;
  });
  def(IP, "every", function (f) {
    fnArg(f); var r, i = 0;
    while (!(r = this.next()).done) if (!f(r.value, i++)) { closeIter(this); return false; }
    return true;
  });
  def(IP, "find", function (f) {
    fnArg(f); var r, i = 0;
    while (!(r = this.next()).done) if (f(r.value, i++)) { closeIter(this); return r.value; }
    return undefined;
  });
  var WP = Object.create(IP);
  def(WP, "next", function () { return this._it.next(); });
  def(WP, "return", function () { closeIter(this._it); return DONE; });
  def(Iterator, "from", function (o) {
    if (typeof o === "string") o = Object(o);
    var it = typeof o[ITER] === "function" ? o[ITER]() : o;
    if (it instanceof Iterator) return it;
    var w = Object.create(WP);
    def(w, "_it", it);
    return w;
  });
  def(g, "Iterator", Iterator);
  def(Number.prototype, "toLocaleString", function (l, o) { return new g.Intl.NumberFormat(l, o).format(Number(this)); });
  /* (generators get Iterator.prototype under theirs once the engine has made them: js.c) */
  /* The engine's own iterators (keys(), values()...): under Iterator.prototype too. */
  function adoptIter(r) {
    if (r && typeof r === "object" && !(r instanceof Iterator)) {
      var p = Object.getPrototypeOf(r);
      try { if (p && p !== Object.prototype) Object.setPrototypeOf(p, IP); else Object.setPrototypeOf(r, IP); } catch (e) {}
    }
    return r;
  }
  function listIter(a) {
    var i = 0;
    var it = Object.create(IP);
    def(it, "next", function () {
      if (i < a.length) return { value: a[i++], done: false };
      return { value: undefined, done: true };
    });
    return it;
  }
  def(g, "%def", def);
  def(g, "%iter", listIter);
  ["Array", "Map", "Set"].forEach(function (n) {
    var P = g[n].prototype;
    ["keys", "values", "entries"].forEach(function (m) {
      var orig = P[m];
      if (orig) def(P, m, function () { var r = orig.call(this); return Array.isArray(r) ? listIter(r) : adoptIter(r); });
    });
  });
  def(Array.prototype, ITER, function () { return listIter(this.slice()); });
  def(String.prototype, ITER, function () { return listIter(Array.from(String(this))); });

  /* ---- Array ---- */
  var A = Array.prototype;
  def(A, "at", function (i) { i = Math.trunc(i) || 0; if (i < 0) i += this.length; return this[i]; });
  def(A, "findLast", function (f, t) {
    for (var i = this.length - 1; i >= 0; i--) if (f.call(t, this[i], i, this)) return this[i];
    return undefined;
  });
  def(A, "findLastIndex", function (f, t) {
    for (var i = this.length - 1; i >= 0; i--) if (f.call(t, this[i], i, this)) return i;
    return -1;
  });
  def(A, "flatMap", function (f, t) {
    var r = [];
    for (var i = 0; i < this.length; i++) {
      var v = f.call(t, this[i], i, this);
      if (Array.isArray(v)) { for (var j = 0; j < v.length; j++) r.push(v[j]); } else r.push(v);
    }
    return r;
  });
  def(A, "flat", function (d) {
    d = d === undefined ? 1 : d;
    var r = [];
    (function walk(a, n) {
      for (var i = 0; i < a.length; i++) {
        if (Array.isArray(a[i]) && n > 0) walk(a[i], n - 1); else r.push(a[i]);
      }
    })(this, d);
    return r;
  });
  def(A, "copyWithin", function (target, start, end) {
    var len = this.length;
    if (target < 0) target += len;
    if (start < 0) start += len;
    end = end === undefined ? len : end < 0 ? end + len : end;
    var chunk = this.slice(start, end);
    for (var i = 0; i < chunk.length && target + i < len; i++) this[target + i] = chunk[i];
    return this;
  });
  def(A, "toReversed", function () { return this.slice().reverse(); });
  def(A, "toSorted", function (f) { return this.slice().sort(f); });
  def(A, "toSpliced", function () { var c = this.slice(); c.splice.apply(c, arguments); return c; });
  def(A, "with", function (i, v) {
    var c = this.slice();
    if (i < 0) i += c.length;
    if (i < 0 || i >= c.length) throw new RangeError("Invalid index");
    c[i] = v;
    return c;
  });

  /* ---- Object, String, Math ---- */
  def(Object, "hasOwn", function (o, k) { return Object.prototype.hasOwnProperty.call(o, k); });
  def(Object, "groupBy", function (items, f) {
    var r = {}, i = 0;
    for (var x of items) { var k = f(x, i++); (r[k] || (r[k] = [])).push(x); }
    return r;
  });
  def(Map, "groupBy", function (items, f) {
    var r = new Map(), i = 0;
    for (var x of items) {
      var k = f(x, i++);
      if (!r.has(k)) r.set(k, []);
      r.get(k).push(x);
    }
    return r;
  });
  def(String, "raw", function (s) {
    var r = s.raw || s, o = "";
    for (var i = 0; i < r.length; i++) {
      o += r[i];
      if (i + 1 < r.length && i + 1 < arguments.length) o += arguments[i + 1];
    }
    return o;
  });
  /* strings are UTF-8 here: there are no lone surrogates */
  def(String.prototype, "isWellFormed", function () { return true; });
  def(String.prototype, "toWellFormed", function () { return String(this); });
  def(Array, "fromAsync", async function (items, f, t) {
    var r = [], i = 0;
    if (items && items[Symbol.asyncIterator]) {
      for await (var x of items) r.push(f ? await f.call(t, x, i++) : x);
    } else {
      for (var y of Array.from(items)) { var v = await y; r.push(f ? await f.call(t, v, i++) : v); }
    }
    return r;
  });

  /* ---- Set methods (ES2025) ---- */
  var SP = Set.prototype;
  function setLike(o) {
    if (o instanceof Set || o instanceof Map) return o;
    if (o && typeof o.has === "function" && typeof o.keys === "function") return o;
    throw new TypeError("The argument is not set-like");
  }
  function keysOf(o) { return o instanceof Set ? Array.from(o) : Array.from(o.keys()); }
  def(SP, "union", function (o) {
    var r = new Set(this);
    keysOf(setLike(o)).forEach(function (k) { r.add(k); });
    return r;
  });
  def(SP, "intersection", function (o) {
    o = setLike(o);
    var r = new Set();
    this.forEach(function (k) { if (o.has(k)) r.add(k); });
    return r;
  });
  def(SP, "difference", function (o) {
    o = setLike(o);
    var r = new Set();
    this.forEach(function (k) { if (!o.has(k)) r.add(k); });
    return r;
  });
  def(SP, "symmetricDifference", function (o) {
    var self = this, r = new Set(this);
    keysOf(setLike(o)).forEach(function (k) { if (self.has(k)) r.delete(k); else r.add(k); });
    return r;
  });
  def(SP, "isSubsetOf", function (o) {
    o = setLike(o);
    for (var k of Array.from(this)) if (!o.has(k)) return false;
    return true;
  });
  def(SP, "isSupersetOf", function (o) {
    var self = this;
    return keysOf(setLike(o)).every(function (k) { return self.has(k); });
  });
  def(SP, "isDisjointFrom", function (o) {
    o = setLike(o);
    for (var k of Array.from(this)) if (o.has(k)) return false;
    return true;
  });
  var M = Math;
  def(M, "fround", function (x) { return Number(x); });
  def(M, "sinh", function (x) { return (M.exp(x) - M.exp(-x)) / 2; });
  def(M, "cosh", function (x) { return (M.exp(x) + M.exp(-x)) / 2; });
  def(M, "tanh", function (x) {
    var a = M.exp(x), b = M.exp(-x);
    return a === Infinity ? 1 : b === Infinity ? -1 : (a - b) / (a + b);
  });
  def(M, "asinh", function (x) {
    if (x === 0 || !isFinite(x)) return x;
    return x < 0 ? -M.log(-x + M.sqrt(x * x + 1)) : M.log(x + M.sqrt(x * x + 1));
  });
  def(M, "acosh", function (x) { return M.log(x + M.sqrt(x * x - 1)); });
  def(M, "atanh", function (x) { return 0.5 * M.log((1 + x) / (1 - x)); });
  def(M, "expm1", function (x) { return M.exp(x) - 1; });
  def(M, "log1p", function (x) { return M.log(1 + x); });
  def(M, "clz32", function (x) {
    x = x >>> 0;
    if (x === 0) return 32;
    var n = 0;
    while ((x & 0x80000000) === 0) { x = x << 1; n++; }
    return n;
  });
  def(M, "imul", function (a, b) {
    var ah = (a >>> 16) & 0xffff, al = a & 0xffff, bh = (b >>> 16) & 0xffff, bl = b & 0xffff;
    return ((al * bl) + (((ah * bl + al * bh) << 16) >>> 0) | 0);
  });

  /* addEventListener(type, f, { signal }): the DOM's listeners go when the signal aborts */
  def(g, "%onAbort", function (signal, target, type, f, capture) {
    signal.addEventListener("abort", function () { target.removeEventListener(type, f, capture); }, { once: true });
  });
  /* ---- stubs for what pages look for ---- */
  function Observer() {
    return { observe: function () {}, unobserve: function () {}, disconnect: function () {},
             takeRecords: function () { return []; } };
  }
  ["PerformanceObserver"].forEach(function (n) {
    if (!(n in g)) def(g, n, function () { return Observer(); });
  });
  if (!("requestIdleCallback" in g)) {
    def(g, "requestIdleCallback", function (f) {
      return setTimeout(function () { f({ didTimeout: false, timeRemaining: function () { return 50; } }); }, 1);
    });
    def(g, "cancelIdleCallback", function (id) { clearTimeout(id); });
  }
  Object.defineProperty(Error.prototype, "stack", {
    get: function () { return this.name + ": " + this.message + "\n    at <anonymous>"; },
    set: function (v) { def(this, "stack", v); },
    configurable: true,
    enumerable: false
  });
  def(Error, "captureStackTrace", function () {});

  var c = g.console, counts = {}, timers = {};
  c.group = c.groupCollapsed = function () { if (arguments.length) c.log.apply(c, arguments); };
  c.groupEnd = function () {};
  c.dir = function (o) { c.log(o); };
  c.table = function (d) { c.log(JSON.stringify(d)); };
  c.assert = function (cond) {
    if (!cond) c.error.apply(c, ["Assertion failed:"].concat([].slice.call(arguments, 1)));
  };
  c.count = function (l) {
    l = l === undefined ? "default" : String(l);
    counts[l] = (counts[l] || 0) + 1;
    c.log(l + ": " + counts[l]);
  };
  c.countReset = function (l) { counts[l === undefined ? "default" : String(l)] = 0; };
  c.time = function (l) { timers[l === undefined ? "default" : String(l)] = Date.now(); };
  c.timeEnd = c.timeLog = function (l) {
    l = l === undefined ? "default" : String(l);
    c.log(l + ": " + (Date.now() - timers[l]) + "ms");
  };
})(globalThis);
);

static const char js_chunk_weak[] = JS_SOURCE(
(function (g) {
  var def = g["%def"], listIter = g["%iter"], lz = g["%lazy"], ITER = Symbol.iterator;
  /* ---- Reflect, weak collections, errors ---- */
  lz("Reflect", {
    ownKeys: function (o) { return Object.getOwnPropertyNames(o).concat(Object.getOwnPropertySymbols(o)); },
    has: function (o, k) { return k in o; },
    get: function (o, k) { return o[k]; },
    set: function (o, k, v) { o[k] = v; return true; },
    getPrototypeOf: function (o) { return Object.getPrototypeOf(o); },
    setPrototypeOf: function (o, p) { try { Object.setPrototypeOf(o, p); } catch (e) { return false; } return true; },
    defineProperty: function (o, k, d) { Object.defineProperty(o, k, d); return true; },
    deleteProperty: function (o, k) { return delete o[k]; },
    getOwnPropertyDescriptor: function (o, k) { return Object.getOwnPropertyDescriptor(o, k); },
    apply: function (f, t, a) { return Function.prototype.apply.call(f, t, a); },
    construct: function (F, a) { return new (Function.prototype.bind.apply(F, [null].concat(a)))(); }
  });
  function weakKey(k) {
    if (k === null || (typeof k !== "object" && typeof k !== "function"))
      throw new TypeError("Invalid value used as a weak collection key");
  }
  class WeakMap extends Map {
    set(k, v) { weakKey(k); return super.set(k, v); }
  }
  class WeakSet extends Set {
    add(k) { weakKey(k); return super.add(k); }
  }
  class WeakRef {
    constructor(t) { def(this, "_t", t); }
    deref() { return this._t; }
  }
  class FinalizationRegistry {
    register() {}
    unregister() {}
  }
  class AggregateError extends Error {
    constructor(errors, message) {
      super(message);
      this.name = "AggregateError";
      this.errors = Array.from(errors);
    }
  }
  class DOMException extends Error {
    constructor(message, name) {
      super(message);
      this.name = name || "Error";
    }
  }
  lz("WeakMap", WeakMap);
  lz("WeakSet", WeakSet);
  lz("WeakRef", WeakRef);
  lz("FinalizationRegistry", FinalizationRegistry);
  lz("AggregateError", AggregateError);
  lz("DOMException", DOMException);

})(globalThis);
);

static const char js_chunk_misc[] = JS_SOURCE(
(function (g) {
  var def = g["%def"], listIter = g["%iter"], lz = g["%lazy"], ITER = Symbol.iterator;
  var Atomics={};
  ["load","store","add","sub","and","or","xor","exchange","compareExchange","isLockFree"].forEach(function(name){
    def(Atomics,name,function(a,index,value,replacement){return g["%atomic"](name,a,index,value,replacement);});
  });
  def(Atomics,"wait",function(a,index,value,timeout){return g["%atomicWait"](false,a,index,value,timeout);});
  def(Atomics,"notify",function(a,index,count){return g["%atomicWait"](true,a,index,count);});
  lz("Atomics",Atomics);
  /* ---- structuredClone, base64 ---- */
  function copy(v, seen) {
    if (v === null || typeof v !== "object") {
      if (typeof v === "function" || typeof v === "symbol") throw new DOMException("The value could not be cloned", "DataCloneError");
      return v;
    }
    if (seen.has(v)) return seen.get(v);
    var r;
    if(v instanceof Error) {
      var C=v instanceof TypeError?TypeError:v instanceof RangeError?RangeError:v instanceof SyntaxError?SyntaxError:
        v instanceof ReferenceError?ReferenceError:v instanceof EvalError?EvalError:v instanceof URIError?URIError:Error;
      r=new C(v.message);seen.set(v,r);r.name=v.name;r.stack=v.stack;
      if(Object.prototype.hasOwnProperty.call(v,"cause"))r.cause=copy(v.cause,seen);
      return r;
    }
    if (v instanceof Date) { r = new Date(v.getTime()); seen.set(v, r); return r; }
    if (v instanceof RegExp) { r = new RegExp(v.source, v.flags); seen.set(v, r); return r; }
    var binary = g["%binaryType"](v);
    if(binary === "SharedArrayBuffer"){r=g["%sharedImport"](g["%sharedExport"](v));seen.set(v,r);return r;}
    if (binary === "ArrayBuffer") { r = g["%binaryClone"](v); seen.set(v, r); return r; }
    if (binary) {
      var buffer = copy(v.buffer, seen);
      r = g["%binaryClone"](v, buffer);
      seen.set(v, r); return r;
    }
    if (v instanceof Blob) {
      r=v instanceof File?new File([v],v.name,{type:v.type,lastModified:v.lastModified}):new Blob([v],{type:v.type});
      seen.set(v,r);return r;
    }
    if (typeof Node !== "undefined" && v instanceof Node)
      throw new DOMException("A DOM node could not be cloned", "DataCloneError");
    if (Array.isArray(v)) {
      r = [];
      seen.set(v, r);
      r.length = v.length;
      Object.keys(v).forEach(function (k) { r[k] = copy(v[k], seen); });
      return r;
    }
    if (v instanceof Map) {
      r = new Map();
      seen.set(v, r);
      v.forEach(function (val, k) { r.set(copy(k, seen), copy(val, seen)); });
      return r;
    }
    if (v instanceof Set) {
      r = new Set();
      seen.set(v, r);
      v.forEach(function (val) { r.add(copy(val, seen)); });
      return r;
    }
    r = {};
    seen.set(v, r);
    Object.keys(v).forEach(function (k) { r[k] = copy(v[k], seen); });
    return r;
  }
  lz("structuredClone", function (v, options) {
    var transfer=options && options.transfer!==undefined?Array.from(options.transfer):[],seen=new Set();
    transfer.forEach(function(b){
      if(g["%binaryType"](b)!=="ArrayBuffer" || b.detached || seen.has(b))
        throw new DOMException("Invalid transfer list", "DataCloneError");
      seen.add(b);
    });
    var result=copy(v,new Map());
    transfer.forEach(function(b){if(b.detached)throw new DOMException("Buffer detached during serialization","DataCloneError");});
    transfer.forEach(function(b){g["%binaryDetach"](b);});
    return result;
  });

  var B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  lz("btoa", function (s) {
    s = String(s);
    var o = "";
    for (var i = 0; i < s.length; i += 3) {
      var a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
      var n = (a << 16) | ((b || 0) << 8) | (c || 0);
      o += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] +
           (i + 1 < s.length ? B64[(n >> 6) & 63] : "=") + (i + 2 < s.length ? B64[n & 63] : "=");
    }
    return o;
  });
  lz("atob", function (s) {
    s = String(s).split("=").join("");
    var o = "", bits = 0, acc = 0;
    for (var i = 0; i < s.length; i++) {
      var v = B64.indexOf(s[i]);
      if (v < 0) continue;
      acc = (acc << 6) | v;
      bits += 6;
      if (bits >= 8) { bits -= 8; o += String.fromCharCode((acc >> bits) & 255); }
    }
    return o;
  });

  /* ---- Intl, console ---- */
  /* Number formatting: grouping and decimal marks for a few locale families, the rest as en-US. */
  function numberSymbols(loc) {
    var l = String(Array.isArray(loc) ? loc[0] || "en" : loc || "en").toLowerCase().split("-")[0];
    if (["de", "es", "it", "pt", "nl", "id", "tr", "da", "el", "ro", "hr", "sl", "sr"].indexOf(l) >= 0) return { group: ".", decimal: ",", after: true };
    if (["fr", "ru", "sv", "nb", "no", "fi", "pl", "cs", "uk", "sk", "hu", "bg", "lt", "lv", "et"].indexOf(l) >= 0) return { group: " ", decimal: ",", after: true };
    if (l === "de-ch" || l === "ch") return { group: "’", decimal: ".", after: false };
    return { group: ",", decimal: ".", after: false };
  }
  var CURRENCY = { USD: "$", EUR: "€", GBP: "£", JPY: "¥", CNY: "CN¥", INR: "₹", KRW: "₩",
    CAD: "CA$", AUD: "A$", NZD: "NZ$", HKD: "HK$", MXN: "MX$", BRL: "R$", ILS: "₪", VND: "₫", TWD: "NT$",
    PHP: "₱", RUB: "RUB", CHF: "CHF", SEK: "SEK", NOK: "NOK", DKK: "DKK", PLN: "PLN" };
  var CURRENCY_NAME = { USD: "US dollars", EUR: "euros", GBP: "British pounds", JPY: "Japanese yen", CNY: "Chinese yuan",
    INR: "Indian rupees", CAD: "Canadian dollars", AUD: "Australian dollars", CHF: "Swiss francs", RUB: "Russian rubles" };
  var UNITS = { kilometer: ["km", "kilometer"], meter: ["m", "meter"], centimeter: ["cm", "centimeter"], millimeter: ["mm", "millimeter"],
    mile: ["mi", "mile"], yard: ["yd", "yard"], foot: ["ft", "foot", "feet"], inch: ["in", "inch", "inches"],
    kilogram: ["kg", "kilogram"], gram: ["g", "gram"], pound: ["lb", "pound"], ounce: ["oz", "ounce"],
    liter: ["L", "liter"], milliliter: ["mL", "milliliter"], gallon: ["gal", "gallon"],
    second: ["sec", "second"], minute: ["min", "minute"], hour: ["hr", "hour"], day: ["day", "day"], week: ["wk", "week"],
    month: ["mth", "month"], year: ["yr", "year"], millisecond: ["ms", "millisecond"],
    bit: ["bit", "bit"], byte: ["byte", "byte"], kilobyte: ["kB", "kilobyte"], megabyte: ["MB", "megabyte"],
    gigabyte: ["GB", "gigabyte"], terabyte: ["TB", "terabyte"], petabyte: ["PB", "petabyte"],
    percent: ["%", "percent"], celsius: ["°C", "degree Celsius", "degrees Celsius"], fahrenheit: ["°F", "degree Fahrenheit", "degrees Fahrenheit"],
    degree: ["deg", "degree"], "kilometer-per-hour": ["km/h", "kilometer per hour", "kilometers per hour"],
    "mile-per-hour": ["mph", "mile per hour", "miles per hour"], "meter-per-second": ["m/s", "meter per second", "meters per second"],
    acre: ["ac", "acre"], hectare: ["ha", "hectare"] };
  function NumberFormat(loc, o) {
    if (!(this instanceof NumberFormat)) return new NumberFormat(loc, o);
    o = o || {};
    var style = o.style || "decimal", cur = o.currency ? String(o.currency).toUpperCase() : undefined;
    if (style === "currency" && !cur) throw new TypeError("Currency code is required with currency style.");
    var notation = o.notation || "standard";
    var minFD = o.minimumFractionDigits, maxFD = o.maximumFractionDigits;
    var defMin = style === "currency" ? (cur === "JPY" || cur === "KRW" || cur === "VND" ? 0 : 2) : 0;
    var defMax = style === "currency" ? defMin : style === "percent" ? 0 : 3;
    if (minFD === undefined) minFD = maxFD !== undefined ? Math.min(defMin, maxFD) : defMin;
    if (maxFD === undefined) maxFD = Math.max(minFD, defMax);
    if (minFD > maxFD) throw new RangeError("maximumFractionDigits value is out of range.");
    var opts = { locale: "en-US", numberingSystem: "latn", style: style, currency: cur,
      currencyDisplay: o.currencyDisplay || "symbol", currencySign: o.currencySign || "standard",
      unit: o.unit, unitDisplay: o.unitDisplay || "short",
      minimumIntegerDigits: o.minimumIntegerDigits || 1, minimumFractionDigits: minFD, maximumFractionDigits: maxFD,
      minimumSignificantDigits: o.minimumSignificantDigits, maximumSignificantDigits: o.maximumSignificantDigits,
      useGrouping: o.useGrouping === undefined ? (notation === "compact" ? "min2" : "auto") : o.useGrouping,
      notation: notation, compactDisplay: o.compactDisplay || "short", signDisplay: o.signDisplay || "auto",
      roundingMode: o.roundingMode || "halfExpand" };
    if (opts.maximumSignificantDigits !== undefined && opts.minimumSignificantDigits === undefined) opts.minimumSignificantDigits = 1;
    def(this, "_o", opts);
    def(this, "_sym", numberSymbols(loc));
    def(this, "_fd", o.minimumFractionDigits !== undefined || o.maximumFractionDigits !== undefined);
  }
  function groupDigits(int, sep, mode) {
    if (mode === false || mode === "false") return int;
    if (mode === "min2" && int.length < 5) return int;
    var out = "";
    for (var i = int.length; i > 0; i -= 3) out = int.slice(Math.max(0, i - 3), i) + (out ? sep + out : "");
    return out;
  }
  function roundTo(x, o, fdSet) {
    var s;
    if (o.maximumSignificantDigits !== undefined) {
      /* to so many significant digits (the place the last one is at, from the number's size) */
      var e = x === 0 ? 0 : Math.floor(Math.log(x) / Math.LN10 + 1e-12);
      var places = o.maximumSignificantDigits - 1 - e, f = Math.pow(10, places);
      s = (Math.round(x * f) / f).toFixed(Math.max(0, places));
      if (s.indexOf(".") >= 0) s = s.replace(new RegExp("0+$"), "").replace(new RegExp("[.]$"), "");
      var digits = s.replace(".", "").replace(/^0+/, "").length;
      if (digits < o.minimumSignificantDigits) {
        var add = o.minimumSignificantDigits - digits;
        s = s.indexOf(".") < 0 ? s + "." + "0".repeat(add) : s + "0".repeat(add);
      }
      return s;
    }
    s = x.toFixed(o.maximumFractionDigits);
    if (s.indexOf(".") >= 0) {
      var keep = s.length - (o.maximumFractionDigits - o.minimumFractionDigits);
      while (s.length > keep && s[s.length - 1] === "0") s = s.slice(0, -1);
      if (s[s.length - 1] === ".") s = s.slice(0, -1);
    }
    return s;
  }
  NumberFormat.prototype.formatToParts = function (n) {
    var o = this._o, sym = this._sym, parts = [];
    var x = typeof n === "bigint" ? Number(n) : Number(n);
    if (x !== x) return [{ type: "nan", value: "NaN" }];
    var neg = x < 0 || (x === 0 && 1 / x < 0);
    x = Math.abs(x);
    if (o.style === "percent") x *= 100;
    var exponent = 0, compact = "";
    if (x === Infinity) {
      parts.push({ type: "infinity", value: "∞" });
    } else {
      var ro = o;
      if (o.notation === "compact") {
        var long = o.compactDisplay === "long";
        var steps = [[1e12, long ? " trillion" : "T"], [1e9, long ? " billion" : "B"], [1e6, long ? " million" : "M"], [1e3, long ? " thousand" : "K"]];
        for (var k = 0; k < steps.length; k++) if (x >= steps[k][0] * 0.9995) { x /= steps[k][0]; compact = steps[k][1]; break; }
        if (!this._fd && o.maximumSignificantDigits === undefined)
          ro = x >= 100 ? { maximumFractionDigits: 0, minimumFractionDigits: 0 } : { maximumSignificantDigits: 2, minimumSignificantDigits: 1 };
        if (compact && Number(roundTo(x, ro, this._fd)) >= 1000 && steps[0][1] !== compact) {
          for (var k2 = steps.length - 1; k2 >= 0; k2--) if (steps[k2][1] === compact && k2 > 0) { x /= 1000; compact = steps[k2 - 1][1]; break; }
        }
      } else if ((o.notation === "scientific" || o.notation === "engineering") && x !== 0) {
        exponent = Math.floor(Math.log(x) / Math.LN10 + 1e-12);
        if (o.notation === "engineering") exponent -= ((exponent % 3) + 3) % 3;
        x /= Math.pow(10, exponent);
        if (Number(roundTo(x, o, this._fd)) >= (o.notation === "engineering" ? 1000 : 10)) { x /= o.notation === "engineering" ? 1000 : 10; exponent += o.notation === "engineering" ? 3 : 1; }
      }
      var str = roundTo(x, ro, this._fd), dot = str.indexOf(".");
      var int = dot < 0 ? str : str.slice(0, dot), frac = dot < 0 ? "" : str.slice(dot + 1);
      while (int.length < o.minimumIntegerDigits) int = "0" + int;
      var grouped = groupDigits(int, sym.group, o.useGrouping).split(sym.group);
      for (var gi = 0; gi < grouped.length; gi++) {
        if (gi) parts.push({ type: "group", value: sym.group });
        parts.push({ type: "integer", value: grouped[gi] });
      }
      if (frac) { parts.push({ type: "decimal", value: sym.decimal }); parts.push({ type: "fraction", value: frac }); }
      if (exponent || o.notation === "scientific" || o.notation === "engineering") {
        parts.push({ type: "exponentSeparator", value: "E" });
        if (exponent < 0) parts.push({ type: "exponentMinusSign", value: "-" });
        parts.push({ type: "exponentInteger", value: String(Math.abs(exponent)) });
      }
      if (compact) parts.push(compact[0] === " " ? { type: "literal", value: " " } : null, { type: "compact", value: compact.trim() });
      parts = parts.filter(function (p) { return p; });
      if (Number(str) === 0 && o.signDisplay !== "always" && o.signDisplay !== "negative") neg = false;
    }
    var sign = "";
    var isZero = parts.every(function (p) { return p.type !== "integer" && p.type !== "fraction" || /^0*$/.test(p.value); });
    if (neg && o.signDisplay !== "never" && !(isZero && (o.signDisplay === "exceptZero" || o.signDisplay === "negative"))) sign = "-";
    else if (!neg && (o.signDisplay === "always" || (o.signDisplay === "exceptZero" && !isZero))) sign = "+";
    var pre = [], post = [];
    if (sign) pre.push({ type: sign === "-" ? "minusSign" : "plusSign", value: sign });
    if (o.style === "currency") {
      var cs = o.currencyDisplay === "code" ? o.currency + " " : o.currencyDisplay === "name" ? "" :
               (CURRENCY[o.currency] || o.currency + " ");
      if (o.currencyDisplay === "name") post.push({ type: "literal", value: " " }, { type: "currency", value: CURRENCY_NAME[o.currency] || o.currency });
      else if (sym.after) post.push({ type: "literal", value: " " }, { type: "currency", value: cs.trim() });
      else {
        if (cs[cs.length - 1] === " ") pre.push({ type: "currency", value: cs.slice(0, -1) }, { type: "literal", value: " " });
        else pre.push({ type: "currency", value: cs });
      }
    } else if (o.style === "percent") {
      if (sym.after) post.push({ type: "literal", value: " " });
      post.push({ type: "percentSign", value: "%" });
    } else if (o.style === "unit" && o.unit) {
      var u = UNITS[o.unit], plural = Math.abs(Number(n)) !== 1;
      var text = !u ? o.unit : o.unitDisplay === "long" ? (plural ? u[2] || u[1] + "s" : u[1]) :
                 o.unit === "day" || o.unit === "week" || o.unit === "month" || o.unit === "year" ? (plural ? u[0] + "s" : u[0]) : u[0];
      if (o.unitDisplay === "narrow" || text === "%" || text[0] === "°") post.push({ type: "unit", value: text });
      else post.push({ type: "literal", value: " " }, { type: "unit", value: text });
    }
    return pre.concat(parts, post);
  };
  NumberFormat.prototype.format = function (n) {
    return this.formatToParts(n).map(function (p) { return p.value; }).join("");
  };
  NumberFormat.prototype.formatRange = function (a, b) { return this.format(a) + "–" + this.format(b); };
  NumberFormat.prototype.resolvedOptions = function () { return Object.assign({}, this._o); };

  /* Intl.Locale: a language tag's parts, and the likely ones for the commonest languages. */
  var LIKELY = { en: "en-Latn-US", de: "de-Latn-DE", fr: "fr-Latn-FR", es: "es-Latn-ES", it: "it-Latn-IT", pt: "pt-Latn-BR",
    nl: "nl-Latn-NL", sv: "sv-Latn-SE", pl: "pl-Latn-PL", ru: "ru-Cyrl-RU", uk: "uk-Cyrl-UA", ja: "ja-Jpan-JP",
    zh: "zh-Hans-CN", ko: "ko-Kore-KR", ar: "ar-Arab-EG", he: "he-Hebr-IL", hi: "hi-Deva-IN", tr: "tr-Latn-TR",
    el: "el-Grek-GR", fa: "fa-Arab-IR", th: "th-Thai-TH", vi: "vi-Latn-VN", id: "id-Latn-ID", cs: "cs-Latn-CZ" };
  class Locale {
    constructor(tag, o) {
      if (tag instanceof Locale) tag = tag.toString();
      if (typeof tag !== "string" || !tag) throw new TypeError("Incorrect locale information provided");
      var parts = tag.replace(/_/g, "-").split("-"), ext = {}, i = 1;
      var language = parts[0].toLowerCase();
      if (!/^[a-z]{2,8}$/.test(language)) throw new RangeError("Incorrect locale information provided");
      var script, region;
      if (parts[i] && /^[a-z]{4}$/i.test(parts[i])) { script = parts[i][0].toUpperCase() + parts[i].slice(1).toLowerCase(); i++; }
      if (parts[i] && /^([a-z]{2}|[0-9]{3})$/i.test(parts[i])) { region = parts[i].toUpperCase(); i++; }
      for (; i < parts.length; i++) {
        if (parts[i].toLowerCase() === "u") {
          for (i++; i + 1 < parts.length && parts[i].length === 2; i += 2) ext[parts[i].toLowerCase()] = parts[i + 1];
          i--;
        }
      }
      o = o || {};
      def(this, "_p", {
        language: o.language || language, script: o.script || script, region: o.region || region,
        calendar: o.calendar || ext.ca, collation: o.collation || ext.co, hourCycle: o.hourCycle || ext.hc,
        caseFirst: o.caseFirst || ext.kf, numberingSystem: o.numberingSystem || ext.nu,
        numeric: o.numeric !== undefined ? !!o.numeric : ext.kn === "true" || ext.kn === "" ? true : false
      });
    }
    get language() { return this._p.language; }
    get script() { return this._p.script; }
    get region() { return this._p.region; }
    get calendar() { return this._p.calendar; }
    get collation() { return this._p.collation; }
    get hourCycle() { return this._p.hourCycle; }
    get caseFirst() { return this._p.caseFirst; }
    get numberingSystem() { return this._p.numberingSystem; }
    get numeric() { return this._p.numeric; }
    get baseName() { var p = this._p; return [p.language, p.script, p.region].filter(Boolean).join("-"); }
    get textInfo() { return { direction: ["ar", "he", "fa", "ur", "yi", "ps", "sd"].indexOf(this._p.language) >= 0 ? "rtl" : "ltr" }; }
    getTextInfo() { return this.textInfo; }
    get hourCycles() { return [this._p.hourCycle || (this._p.region === "US" || !this._p.region && this._p.language === "en" ? "h12" : "h23")]; }
    getHourCycles() { return this.hourCycles; }
    getCalendars() { return [this._p.calendar || "gregory"]; }
    getWeekInfo() { return { firstDay: this._p.region === "US" ? 7 : 1, weekend: [6, 7], minimalDays: 1 }; }
    maximize() {
      var p = this._p, l = LIKELY[p.language];
      var full = l ? l.split("-") : [p.language];
      return new Locale([p.language, p.script || full[1], p.region || full[2]].filter(Boolean).join("-"));
    }
    minimize() { return new Locale(this._p.language); }
    toString() {
      var p = this._p, u = [];
      if (p.calendar) u.push("ca", p.calendar);
      if (p.collation) u.push("co", p.collation);
      if (p.hourCycle) u.push("hc", p.hourCycle);
      if (p.numberingSystem) u.push("nu", p.numberingSystem);
      return this.baseName + (u.length ? "-u-" + u.join("-") : "");
    }
    toJSON() { return this.toString(); }
  }

  /* Intl.DisplayNames: English names for the commoner codes; others come back as the code. */
  var NAMES = {
    language: { en: "English", "en-US": "American English", "en-GB": "British English", de: "German", fr: "French",
      es: "Spanish", it: "Italian", pt: "Portuguese", "pt-BR": "Brazilian Portuguese", nl: "Dutch", sv: "Swedish",
      no: "Norwegian", nb: "Norwegian Bokmål", da: "Danish", fi: "Finnish", pl: "Polish", cs: "Czech", sk: "Slovak",
      hu: "Hungarian", ro: "Romanian", el: "Greek", tr: "Turkish", ru: "Russian", uk: "Ukrainian", ja: "Japanese",
      zh: "Chinese", "zh-Hans": "Simplified Chinese", "zh-Hant": "Traditional Chinese", ko: "Korean", ar: "Arabic",
      he: "Hebrew", hi: "Hindi", fa: "Persian", th: "Thai", vi: "Vietnamese", id: "Indonesian", ms: "Malay",
      bg: "Bulgarian", hr: "Croatian", sr: "Serbian", ca: "Catalan", eu: "Basque", ga: "Irish", la: "Latin" },
    region: { US: "United States", GB: "United Kingdom", DE: "Germany", FR: "France", ES: "Spain", IT: "Italy",
      PT: "Portugal", NL: "Netherlands", BE: "Belgium", CH: "Switzerland", AT: "Austria", SE: "Sweden", NO: "Norway",
      DK: "Denmark", FI: "Finland", PL: "Poland", CZ: "Czechia", IE: "Ireland", GR: "Greece", TR: "Turkey",
      RU: "Russia", UA: "Ukraine", CA: "Canada", MX: "Mexico", BR: "Brazil", AR: "Argentina", CL: "Chile",
      CO: "Colombia", PE: "Peru", AU: "Australia", NZ: "New Zealand", JP: "Japan", CN: "China", KR: "South Korea",
      TW: "Taiwan", HK: "Hong Kong SAR China", IN: "India", ID: "Indonesia", TH: "Thailand", VN: "Vietnam",
      PH: "Philippines", SG: "Singapore", MY: "Malaysia", IL: "Israel", SA: "Saudi Arabia", AE: "United Arab Emirates",
      EG: "Egypt", ZA: "South Africa", NG: "Nigeria", KE: "Kenya", "419": "Latin America", "001": "world", "150": "Europe" },
    script: { Latn: "Latin", Cyrl: "Cyrillic", Arab: "Arabic", Hebr: "Hebrew", Grek: "Greek", Hans: "Simplified Han",
      Hant: "Traditional Han", Jpan: "Japanese", Kore: "Korean", Deva: "Devanagari", Thai: "Thai" },
    currency: { USD: "US Dollar", EUR: "Euro", GBP: "British Pound", JPY: "Japanese Yen", CNY: "Chinese Yuan",
      INR: "Indian Rupee", CAD: "Canadian Dollar", AUD: "Australian Dollar", CHF: "Swiss Franc", SEK: "Swedish Krona",
      NOK: "Norwegian Krone", DKK: "Danish Krone", PLN: "Polish Zloty", RUB: "Russian Ruble", BRL: "Brazilian Real",
      MXN: "Mexican Peso", KRW: "South Korean Won", HKD: "Hong Kong Dollar", NZD: "New Zealand Dollar" },
    calendar: { gregory: "Gregorian Calendar", buddhist: "Buddhist Calendar", chinese: "Chinese Calendar",
      hebrew: "Hebrew Calendar", islamic: "Hijri Calendar", japanese: "Japanese Calendar", iso8601: "ISO-8601 Calendar" },
    dateTimeField: { era: "era", year: "year", quarter: "quarter", month: "month", weekOfYear: "week", weekday: "day of the week",
      day: "day", dayPeriod: "AM/PM", hour: "hour", minute: "minute", second: "second", timeZoneName: "time zone" }
  };
  class DisplayNames {
    constructor(loc, o) {
      if (!o || !o.type || !NAMES[o.type]) throw new TypeError("Required option type is missing or invalid");
      def(this, "_o", { locale: "en-US", style: o.style || "long", type: o.type, fallback: o.fallback || "code",
        languageDisplay: o.languageDisplay || "dialect" });
    }
    of(code) {
      code = String(code);
      var t = this._o.type, table = NAMES[t];
      var key = t === "region" || t === "currency" ? code.toUpperCase() : t === "script" ? code[0].toUpperCase() + code.slice(1).toLowerCase() : code;
      if (table[key] !== undefined) return table[key];
      if (t === "language") {
        var parts = code.split("-"), base = table[parts[0].toLowerCase()];
        if (base && parts.length > 1) {
          var extra = parts.slice(1).map(function (p) { return NAMES.region[p.toUpperCase()] || NAMES.script[p] || p; });
          return base + " (" + extra.join(", ") + ")";
        }
      }
      return this._o.fallback === "none" ? undefined : key;
    }
    resolvedOptions() { return Object.assign({}, this._o); }
  }
  lz("Intl", {
    NumberFormat: NumberFormat,
    Locale: Locale,
    DisplayNames: DisplayNames,
    DateTimeFormat: function (loc, o) {
      var fmt = function (d) { return (d === undefined ? new Date() : new Date(d)).toLocaleDateString(loc, o); };
      return { format: fmt,
               formatToParts: function (d) { return [{ type: "literal", value: fmt(d) }]; },
               formatRange: function (a, b) { return fmt(a) + " – " + fmt(b); },
               resolvedOptions: function () { return { locale: "en-US", timeZone: "UTC", calendar: "gregory" }; } };
    },
    Collator: function (loc, o) {
      var base = o && (o.sensitivity === "base" || o.sensitivity === "accent");
      var num = o && o.numeric;
      return {
        compare: function (a, b) {
          a = String(a); b = String(b);
          if (base) { a = a.toLowerCase(); b = b.toLowerCase(); }
          if (num) {
            var na = parseFloat(a), nb = parseFloat(b);
            if (!isNaN(na) && !isNaN(nb) && na !== nb) return na < nb ? -1 : 1;
          }
          return a.localeCompare(b);
        },
        resolvedOptions: function () { return { locale: "en-US", sensitivity: base ? "base" : "variant" }; }
      };
    },
    PluralRules: function (loc, o) {
      var ordinal = o && o.type === "ordinal";
      return {
        select: function (n) {
          n = Math.abs(Number(n));
          if (!ordinal) return n === 1 ? "one" : "other";
          var t = n % 10, h = n % 100;
          return t === 1 && h !== 11 ? "one" : t === 2 && h !== 12 ? "two" : t === 3 && h !== 13 ? "few" : "other";
        },
        resolvedOptions: function () { return { locale: "en-US", type: ordinal ? "ordinal" : "cardinal" }; }
      };
    },
    RelativeTimeFormat: function (loc, o) {
      var auto = o && o.numeric === "auto";
      return {
        format: function (v, unit) {
          v = Number(v);
          unit = String(unit).replace(/s$/, "");
          if (auto && unit === "day" && Math.abs(v) <= 1) return v === 0 ? "today" : v > 0 ? "tomorrow" : "yesterday";
          if (auto && v === 0) return "this " + unit;
          var n = Math.abs(v), u = n === 1 ? unit : unit + "s";
          return v < 0 ? n + " " + u + " ago" : "in " + n + " " + u;
        },
        resolvedOptions: function () { return { locale: "en-US" }; }
      };
    },
    ListFormat: function (loc, o) {
      var word = o && o.type === "disjunction" ? "or" : "and";
      return {
        format: function (list) {
          var a = Array.from(list).map(String);
          if (a.length < 2) return a.join("");
          if (a.length === 2) return a[0] + " " + word + " " + a[1];
          return a.slice(0, -1).join(", ") + ", " + word + " " + a[a.length - 1];
        }
      };
    },
    Segmenter: function (loc, o) {
      var g = o && o.granularity || "grapheme";
      return {
        segment: function (s) {
          s = String(s);
          /* (no backslashes outside strings here: the C preprocessor would keep them bare) */
          var parts = g === "word" ? s.split(new RegExp("(\\s+)")).filter(function (p) { return p !== ""; }) :
                      g === "sentence" ? s.split(new RegExp("(?<=[.!?])\\s+")) : Array.from(s);
          var at = 0, out = parts.map(function (p) {
            var r = { segment: p, index: at, input: s };
            if (g === "word") r.isWordLike = new RegExp("\\w").test(p);
            at += p.length;
            return r;
          });
          return out;
        }
      };
    },
    getCanonicalLocales: function (l) { return l === undefined ? [] : [].concat(l).map(function (t) { return new Locale(String(t)).toString(); }); },
    supportedValuesOf: function () { return []; }
  });
  ["NumberFormat", "DateTimeFormat", "Collator", "PluralRules", "RelativeTimeFormat", "ListFormat", "Segmenter", "DisplayNames"]
    .forEach(function (n) {
      g.Intl[n].supportedLocalesOf = function (l) { return l === undefined ? [] : [].concat(l); };
    });
})(globalThis);
);

static const char js_chunk_events[] = JS_SOURCE(
(function (g) {
  var def = g["%def"], listIter = g["%iter"], lz = g["%lazy"], ITER = Symbol.iterator;
  /* ---- events ---- */
  class EventTarget {
    constructor() { def(this, "_l", Object.create(null)); }
    addEventListener(t, f, o) {
      if (!f) return;
      var sig = o && typeof o === "object" ? o.signal : null;
      if (sig && sig.aborted) return;
      var l = this._l[t] || (this._l[t] = []);
      for (var i = 0; i < l.length; i++) if (l[i].f === f) return;
      l.push({ f: f, once: !!(o && typeof o === "object" && o.once) });
      var self = this;
      if (sig) sig.addEventListener("abort", function () { self.removeEventListener(t, f); }, { once: true });
    }
    removeEventListener(t, f) {
      var l = this._l[t];
      if (!l) return;
      for (var i = 0; i < l.length; i++) if (l[i].f === f) { l.splice(i, 1); return; }
    }
    dispatchEvent(ev) {
      ev.target = this;
      ev.currentTarget = this;
      var h = this["on" + ev.type];
      if (typeof h === "function") h.call(this, ev);
      var l = (this._l[ev.type] || []).slice();
      for (var i = 0; i < l.length; i++) {
        if (l[i].once) this.removeEventListener(ev.type, l[i].f);
        if (typeof l[i].f === "function") l[i].f.call(this, ev); else l[i].f.handleEvent(ev);
      }
      return !ev.defaultPrevented;
    }
  }
  lz("EventTarget", EventTarget);

  class AbortSignal extends EventTarget {
    constructor() {
      super();
      this.aborted = false;
      this.reason = undefined;
      this.onabort = null;
    }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static abort(reason) { var c = new AbortController(); c.abort(reason); return c.signal; }
    static timeout(ms) {
      var c = new AbortController();
      setTimeout(function () { c.abort(new DOMException("The operation timed out.", "TimeoutError")); }, ms);
      return c.signal;
    }
    static any(signals) {
      var c = new AbortController();
      Array.from(signals).forEach(function (s) {
        if (s.aborted) c.abort(s.reason);
        else s.addEventListener("abort", function () { c.abort(s.reason); });
      });
      return c.signal;
    }
  }
  class AbortController {
    constructor() { this.signal = new AbortSignal(); }
    abort(reason) {
      var s = this.signal;
      if (s.aborted) return;
      s.aborted = true;
      s.reason = reason === undefined ? new DOMException("The operation was aborted.", "AbortError") : reason;
      s.dispatchEvent({ type: "abort", defaultPrevented: false });
    }
  }
  lz("AbortSignal", AbortSignal);
  lz("AbortController", AbortController);

})(globalThis);
);

static const char js_chunk_observers[] = JS_SOURCE(
(function (g) {
  var def = g["%def"], lz = g["%lazy"];
  /* ---- MutationObserver: jsdom.c reports each change to %mutation while any observes ---- */
  var mos = [], pending = false;
  function deliver() {
    pending = false;
    mos.slice().forEach(function (mo) {
      var q = mo.takeRecords();
      if (q.length) { try { mo._cb.call(mo, q, mo); } catch (e) { console.error(e); } }
    });
  }
  class MutationObserver {
    constructor(cb) {
      if (typeof cb !== "function") throw new TypeError("MutationObserver needs a callback");
      def(this, "_cb", cb);
      def(this, "_t", []);
      def(this, "_q", []);
    }
    observe(target, o) {
      o = Object.assign({}, o || {});
      if ((o.attributeOldValue || o.attributeFilter) && o.attributes === undefined) o.attributes = true;
      if (o.characterDataOldValue && o.characterData === undefined) o.characterData = true;
      if (!o.childList && !o.attributes && !o.characterData)
        throw new TypeError("MutationObserver.observe needs childList, attributes or characterData");
      for (var i = 0; i < this._t.length; i++) if (this._t[i].n === target) { this._t[i].o = o; return; }
      this._t.push({ n: target, o: o });
      if (mos.indexOf(this) < 0) mos.push(this);
      g["%watchMutations"](true);
    }
    disconnect() {
      this._t = [];
      this._q = [];
      var i = mos.indexOf(this);
      if (i >= 0) mos.splice(i, 1);
      if (!mos.length) g["%watchMutations"](false);
    }
    takeRecords() { var q = this._q; this._q = []; return q; }
  }
  def(g, "%mutation", function (r) {
    for (var i = 0; i < mos.length; i++) {
      var mo = mos[i];
      for (var k = 0; k < mo._t.length; k++) {
        var t = mo._t[k], o = t.o;
        if (t.n !== r.target && !(o.subtree && t.n.contains && t.n.contains(r.target))) continue;
        if (r.type === "childList" && !o.childList) continue;
        if (r.type === "attributes" && (!o.attributes || (o.attributeFilter && o.attributeFilter.indexOf(r.attributeName) < 0))) continue;
        if (r.type === "characterData" && !o.characterData) continue;
        var rec = Object.assign({}, r);
        if ((r.type === "attributes" && !o.attributeOldValue) || (r.type === "characterData" && !o.characterDataOldValue))
          rec.oldValue = null;
        mo._q.push(rec);
        break;
      }
    }
    if (!pending) { pending = true; queueMicrotask(deliver); }
  });
  lz("MutationObserver", MutationObserver);

  /* ---- IntersectionObserver and ResizeObserver: checked after layout and scrolling (%viewChanged) ---- */
  var watchers = [], queued = false;
  function checkAll() {
    queued = false;
    watchers.slice().forEach(function (w) { try { w._check(); } catch (e) { console.error(e); } });
  }
  function soon() { if (!queued) { queued = true; setTimeout(checkAll, 0); } }
  function watch(w) {
    if (watchers.indexOf(w) < 0) watchers.push(w);
    g["%watchView"](true);
    soon();
  }
  function unwatch(w) {
    var i = watchers.indexOf(w);
    if (i >= 0) watchers.splice(i, 1);
    if (!watchers.length) g["%watchView"](false);
  }
  def(g, "%viewChanged", checkAll);
  function margins(s, w, h) {
    var p = String(s || "0px").trim().split(new RegExp("\\s+")).map(function (v, i) {
      var n = parseFloat(v) || 0;
      return v.indexOf("%") >= 0 ? n * (i % 2 ? w : h) / 100 : n;
    });
    if (p.length === 1) p = [p[0], p[0], p[0], p[0]];
    else if (p.length === 2) p = [p[0], p[1], p[0], p[1]];
    else if (p.length === 3) p = [p[0], p[1], p[2], p[1]];
    return p;
  }
  function rect(x, y, w, h) {
    return { x: x, y: y, left: x, top: y, width: w, height: h, right: x + w, bottom: y + h };
  }
  class IntersectionObserver {
    constructor(cb, o) {
      if (typeof cb !== "function") throw new TypeError("IntersectionObserver needs a callback");
      o = o || {};
      def(this, "_cb", cb);
      def(this, "_t", []);
      this.root = o.root || null;
      this.rootMargin = o.rootMargin || "0px 0px 0px 0px";
      this.thresholds = (o.threshold === undefined ? [0] : [].concat(o.threshold)).slice().sort();
      this.delay = 0;
      this.trackVisibility = false;
    }
    observe(el) {
      for (var i = 0; i < this._t.length; i++) if (this._t[i].el === el) return;
      this._t.push({ el: el, state: -1 });
      watch(this);
    }
    unobserve(el) {
      this._t = this._t.filter(function (t) { return t.el !== el; });
      if (!this._t.length) unwatch(this);
    }
    disconnect() { this._t = []; unwatch(this); }
    takeRecords() { return []; }
    _check() {
      var vw = innerWidth, vh = innerHeight, entries = [], self = this;
      var root = this.root ? this.root.getBoundingClientRect() : rect(0, 0, vw, vh);
      var m = margins(this.rootMargin, root.width, root.height);
      var R = rect(root.left - m[3], root.top - m[0], root.width + m[1] + m[3], root.height + m[0] + m[2]);
      this._t.forEach(function (t) {
        var r = t.el.getBoundingClientRect(), laid = r.width > 0 || r.height > 0;
        var x0 = Math.max(r.left, R.left), y0 = Math.max(r.top, R.top);
        var x1 = Math.min(r.right, R.right), y1 = Math.min(r.bottom, R.bottom);
        var hit = laid && t.el.isConnected && x1 >= x0 && y1 >= y0;
        var area = r.width * r.height;
        var ratio = !hit ? 0 : area > 0 ? Math.max(0, x1 - x0) * Math.max(0, y1 - y0) / area : 1;
        var state = hit ? self.thresholds.filter(function (x) { return ratio >= x; }).length : 0;
        if (state === t.state) return;
        t.state = state;
        entries.push({ target: t.el, isIntersecting: hit, intersectionRatio: ratio, isVisible: hit,
                       boundingClientRect: r, rootBounds: R, time: performance.now(),
                       intersectionRect: hit ? rect(x0, y0, x1 - x0, y1 - y0) : rect(0, 0, 0, 0) });
      });
      if (entries.length) this._cb.call(this, entries, this);
    }
  }
  lz("IntersectionObserver", IntersectionObserver);
  class ResizeObserver {
    constructor(cb) {
      if (typeof cb !== "function") throw new TypeError("ResizeObserver needs a callback");
      def(this, "_cb", cb);
      def(this, "_t", []);
    }
    observe(el) {
      for (var i = 0; i < this._t.length; i++) if (this._t[i].el === el) return;
      this._t.push({ el: el, w: 0, h: 0 });
      watch(this);
    }
    unobserve(el) {
      this._t = this._t.filter(function (t) { return t.el !== el; });
      if (!this._t.length) unwatch(this);
    }
    disconnect() { this._t = []; unwatch(this); }
    _check() {
      var entries = [];
      this._t.forEach(function (t) {
        var r = t.el.getBoundingClientRect();
        if (r.width === t.w && r.height === t.h) return;
        t.w = r.width;
        t.h = r.height;
        var size = [{ inlineSize: r.width, blockSize: r.height }];
        entries.push({ target: t.el, contentRect: rect(0, 0, r.width, r.height), borderBoxSize: size,
                       contentBoxSize: size, devicePixelContentBoxSize: size });
      });
      if (entries.length) this._cb.call(this, entries, this);
    }
  }
  lz("ResizeObserver", ResizeObserver);
})(globalThis);
);

static const char js_chunk_url[] = JS_SOURCE(
(function (g) {
  var def = g["%def"], listIter = g["%iter"], lz = g["%lazy"], ITER = Symbol.iterator;
  /* ---- URL ---- */
  function dec(s) {
    try { return decodeURIComponent(s.split("+").join(" ")); } catch (e) { return s; }
  }
  function enc(s) { return encodeURIComponent(s).split("%20").join("+"); }
  class URLSearchParams {
    constructor(init) {
      def(this, "_l", []);
      def(this, "_u", null);
      if (typeof init === "string") {
        if (init[0] === "?") init = init.slice(1);
        var parts = init.split("&");
        for (var i = 0; i < parts.length; i++) {
          var p = parts[i];
          if (!p) continue;
          var q = p.indexOf("=");
          this._l.push([dec(q < 0 ? p : p.slice(0, q)), q < 0 ? "" : dec(p.slice(q + 1))]);
        }
      } else if (init && typeof init === "object") {
        if (Array.isArray(init) || init[ITER]) {
          for (var e of init) this._l.push([String(e[0]), String(e[1])]);
        } else {
          var self = this;
          Object.keys(init).forEach(function (k) { self._l.push([k, String(init[k])]); });
        }
      }
    }
    _changed() { if (this._u) this._u._sync(); }
    get size() { return this._l.length; }
    append(k, v) { this._l.push([String(k), String(v)]); this._changed(); }
    delete(k) {
      this._l = this._l.filter(function (p) { return p[0] !== String(k); });
      this._changed();
    }
    get(k) {
      for (var i = 0; i < this._l.length; i++) if (this._l[i][0] === String(k)) return this._l[i][1];
      return null;
    }
    getAll(k) {
      return this._l.filter(function (p) { return p[0] === String(k); }).map(function (p) { return p[1]; });
    }
    has(k) { return this.get(k) !== null; }
    set(k, v) {
      k = String(k);
      var found = false, out = [];
      for (var i = 0; i < this._l.length; i++) {
        if (this._l[i][0] !== k) out.push(this._l[i]);
        else if (!found) { found = true; out.push([k, String(v)]); }
      }
      if (!found) out.push([k, String(v)]);
      this._l = out;
      this._changed();
    }
    sort() {
      this._l = this._l.map(function (p, i) { return [p, i]; }).sort(function (a, b) {
        return a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1];
      }).map(function (x) { return x[0]; });
      this._changed();
    }
    forEach(f, t) { for (var i = 0; i < this._l.length; i++) f.call(t, this._l[i][1], this._l[i][0], this); }
    keys() { return listIter(this._l.map(function (p) { return p[0]; })); }
    values() { return listIter(this._l.map(function (p) { return p[1]; })); }
    entries() { return listIter(this._l.map(function (p) { return [p[0], p[1]]; })); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return this._l.map(function (p) { return enc(p[0]) + "=" + enc(p[1]); }).join("&"); }
  }

  var DEFAULT_PORTS = { "http:": "80", "https:": "443", "ftp:": "21" };
  function normalizePath(path) {
    var out = [], segs = path.split("/");
    for (var i = 0; i < segs.length; i++) {
      var s = segs[i];
      if (s === "..") { if (out.length > 1) out.pop(); }
      else if (s !== ".") out.push(s);
    }
    var last = segs[segs.length - 1];
    if ((last === "." || last === "..") && out[out.length - 1] !== "") out.push("");
    return out.join("/") || "/";
  }
  function parseUrl(input, base) {
    input = String(input).trim();
    var colon = input.indexOf(":"), scheme = "";
    if (colon > 0) {
      var ok = true;
      for (var i = 0; i < colon; i++) {
        var ch = input[i];
        if (!((ch >= "a" && ch <= "z") || (ch >= "A" && ch <= "Z") ||
              (i > 0 && ((ch >= "0" && ch <= "9") || ch === "+" || ch === "-" || ch === ".")))) ok = false;
      }
      if (ok) scheme = input.slice(0, colon).toLowerCase() + ":";
    }
    var u = { protocol: "", username: "", password: "", hostname: "", port: "", pathname: "", search: "", hash: "", opaque: false };
    var rest;
    if (scheme) {
      u.protocol = scheme;
      rest = input.slice(colon + 1);
    } else {
      if (!base) throw new TypeError("Invalid URL: " + input);
      var b = base instanceof URL ? base : new URL(String(base));
      u.protocol = b.protocol;
      rest = input;
      if (rest.slice(0, 2) !== "//") {
        u.username = b.username; u.password = b.password; u.hostname = b.hostname; u.port = b.port;
        var hashAt0 = rest.indexOf("#");
        var frag = hashAt0 < 0 ? "" : rest.slice(hashAt0);
        if (hashAt0 >= 0) rest = rest.slice(0, hashAt0);
        var qAt0 = rest.indexOf("?");
        var query = qAt0 < 0 ? null : rest.slice(qAt0);
        if (qAt0 >= 0) rest = rest.slice(0, qAt0);
        if (rest === "") { u.pathname = b.pathname; u.search = query === null ? b.search : (query === "?" ? "" : query); }
        else {
          u.pathname = normalizePath(rest[0] === "/" ? rest : b.pathname.slice(0, b.pathname.lastIndexOf("/") + 1) + rest);
          u.search = query === null || query === "?" ? "" : query;
        }
        u.hash = frag === "#" ? "" : frag;
        return u;
      }
    }
    var hashAt = rest.indexOf("#");
    if (hashAt >= 0) { u.hash = rest.slice(hashAt); rest = rest.slice(0, hashAt); if (u.hash === "#") u.hash = ""; }
    var qAt = rest.indexOf("?");
    if (qAt >= 0) { u.search = rest.slice(qAt); rest = rest.slice(0, qAt); if (u.search === "?") u.search = ""; }
    if (rest.slice(0, 2) === "//") {
      rest = rest.slice(2);
      var slash = rest.indexOf("/");
      var auth = slash < 0 ? rest : rest.slice(0, slash);
      u.pathname = slash < 0 ? "/" : normalizePath(rest.slice(slash));
      var at = auth.lastIndexOf("@");
      if (at >= 0) {
        var cred = auth.slice(0, at), cc = cred.indexOf(":");
        u.username = cc < 0 ? cred : cred.slice(0, cc);
        u.password = cc < 0 ? "" : cred.slice(cc + 1);
        auth = auth.slice(at + 1);
      }
      var pc = auth.lastIndexOf(":");
      if (pc >= 0 && auth.indexOf("]") < pc) { u.port = auth.slice(pc + 1); auth = auth.slice(0, pc); }
      u.hostname = auth.toLowerCase();
      if (u.port === DEFAULT_PORTS[u.protocol]) u.port = "";
      if (!u.hostname && u.protocol !== "file:") throw new TypeError("Invalid URL: " + input);
    } else {
      u.pathname = rest;
      u.opaque = true;
    }
    return u;
  }
  class URL {
    constructor(url, base) {
      var u = parseUrl(url, base);
      def(this, "_u", u);
      def(this, "_p", null);
    }
    _sync() { this._u.search = this._p.toString() ? "?" + this._p.toString() : ""; }
    get protocol() { return this._u.protocol; }
    set protocol(v) { this._u.protocol = String(v).replace(":", "") + ":"; }
    get username() { return this._u.username; }
    get password() { return this._u.password; }
    get hostname() { return this._u.hostname; }
    set hostname(v) { this._u.hostname = String(v).toLowerCase(); }
    get port() { return this._u.port; }
    set port(v) { this._u.port = String(v) === DEFAULT_PORTS[this._u.protocol] ? "" : String(v); }
    get host() { return this._u.hostname + (this._u.port ? ":" + this._u.port : ""); }
    get origin() { if(this.protocol==="blob:"){try{return new URL(this.pathname).origin;}catch(e){return "null";}} return this._u.opaque ? "null" : this._u.protocol + "//" + this.host; }
    get pathname() { return this._u.pathname; }
    set pathname(v) { v = String(v); this._u.pathname = v[0] === "/" ? v : "/" + v; }
    get search() { return this._u.search; }
    set search(v) {
      v = String(v);
      this._u.search = v === "" || v === "?" ? "" : v[0] === "?" ? v : "?" + v;
      this._p = null;
    }
    get hash() { return this._u.hash; }
    set hash(v) { v = String(v); this._u.hash = v === "" || v === "#" ? "" : v[0] === "#" ? v : "#" + v; }
    get searchParams() {
      if (!this._p) { this._p = new URLSearchParams(this._u.search); this._p._u = this; }
      return this._p;
    }
    get href() {
      var u = this._u;
      if (u.opaque) return u.protocol + u.pathname + u.search + u.hash;
      var auth = u.username ? u.username + (u.password ? ":" + u.password : "") + "@" : "";
      return u.protocol + "//" + auth + this.host + u.pathname + u.search + u.hash;
    }
    toString() { return this.href; }
    toJSON() { return this.href; }
    static canParse(url, base) { try { new URL(url, base); return true; } catch (e) { return false; } }
  }
  /* ---- URLPattern: each component's pattern (:name, *, (regex), {...} and ?, +, *) as a RegExp ---- */
  var URLP_PARTS = ["protocol", "username", "password", "hostname", "port", "pathname", "search", "hash"];
  function urlpEscape(c) { return "^$.+*?()[]{}|/".indexOf(c) >= 0 || c === "\\" ? "\\" + c : c; }
  function urlpCompile(pattern, part, ignoreCase) {
    var segment = part === "pathname" ? "[^/]+?" : part === "hostname" ? "[^.]+?" : ".+?";
    var names = [], re = "", i = 0, anon = 0, p = String(pattern), hasRegexp = false;
    function token() {
      var c = p[i];
      if (c === ":") {
        var m = /^:([A-Za-z_$][A-Za-z0-9_$]*)/.exec(p.slice(i));
        if (!m) throw new TypeError("Invalid URLPattern: bad name in " + p);
        i += m[0].length;
        var body = segment;
        if (p[i] === "(") body = regexpGroup();
        names.push(m[1]);
        return "(" + body + ")";
      }
      if (c === "*") { i++; names.push(String(anon++)); return "(.*)"; }
      if (c === "(") { var b = regexpGroup(); names.push(String(anon++)); return "(" + b + ")"; }
      if (c === "{") {
        i++;
        var inner = "";
        while (i < p.length && p[i] !== "}") inner += token();
        if (p[i] !== "}") throw new TypeError("Invalid URLPattern: unclosed { in " + p);
        i++;
        return "(?:" + inner + ")";
      }
      if (c === "\\" && i + 1 < p.length) { i += 2; return urlpEscape(p[i - 1]); }
      i++;
      return urlpEscape(c);
    }
    function regexpGroup() {
      var depth = 0, start = ++i;
      for (; i < p.length; i++) {
        if (p[i] === "\\") { i++; continue; }
        if (p[i] === "(") depth++;
        else if (p[i] === ")") { if (!depth) break; depth--; }
      }
      if (p[i] !== ")") throw new TypeError("Invalid URLPattern: unclosed ( in " + p);
      hasRegexp = true;
      var body = p.slice(start, i++);
      return body.replace(new RegExp("\\((?!\\?)", "g"), "(?:");
    }
    while (i < p.length) {
      var prefix = "";
      /* "/:name?" in a path: the slash goes with the optional part */
      if (part === "pathname" && p[i] === "/" && (p[i + 1] === ":" || p[i + 1] === "*" || p[i + 1] === "(")) { prefix = "\\/"; i++; }
      var t = token(), mod = p[i];
      if (mod === "?" || mod === "+" || (mod === "*" && t !== "(.*)")) {
        i++;
        if (mod === "?") re += "(?:" + prefix + t + ")?";
        else re += "(?:" + prefix + t + ")" + mod;
      } else {
        re += prefix + t;
      }
    }
    return { re: new RegExp("^" + re + "$", ignoreCase ? "i" : ""), names: names, pattern: p, hasRegexp: hasRegexp };
  }
  function urlpSplit(s, base) {
    var o = {}, rest = String(s), m;
    if ((m = new RegExp("^([a-zA-Z][a-zA-Z0-9+.*-]*)://").exec(rest))) {
      o.protocol = m[1]; rest = rest.slice(m[0].length);
      var slash = rest.search(new RegExp("[/?#]")), host = slash < 0 ? rest : rest.slice(0, slash);
      rest = slash < 0 ? "" : rest.slice(slash);
      var at = host.lastIndexOf("@");
      if (at >= 0) {
        var cred = host.slice(0, at).split(":");
        o.username = cred[0]; if (cred.length > 1) o.password = cred.slice(1).join(":");
        host = host.slice(at + 1);
      }
      var colon = host.match(new RegExp(":([0-9]*|:?[a-z_]\\w*|\\*)$"));
      if (colon && host[0] !== "[") { o.port = colon[1]; host = host.slice(0, colon.index); }
      o.hostname = host;
    } else if (base) {
      var b = new URL(base);
      o.protocol = b.protocol.slice(0, -1); o.hostname = b.hostname; o.port = b.port;
      o.username = b.username; o.password = b.password;
      if (rest[0] !== "/" && rest[0] !== "?" && rest[0] !== "#") rest = b.pathname.replace(new RegExp("[^/]*$"), "") + rest;
    }
    var hash = rest.indexOf("#");
    if (hash >= 0) { o.hash = rest.slice(hash + 1); rest = rest.slice(0, hash); }
    var q = rest.search(new RegExp("\\?(?![^(]*\\))"));
    if (q >= 0) { o.search = rest.slice(q + 1); rest = rest.slice(0, q); }
    if (rest || o.protocol) o.pathname = rest || "/";
    return o;
  }
  class URLPattern {
    constructor(input, base, options) {
      if (base && typeof base === "object") { options = base; base = undefined; }
      var ic = !!(options && options.ignoreCase), init = {};
      if (input === undefined) input = {};
      if (typeof input === "string") {
        init = urlpSplit(input, base);
        URLP_PARTS.forEach(function (k) { if (init[k] === undefined) init[k] = k === "search" || k === "hash" ? (init.protocol && !base ? "*" : "*") : "*"; });
      } else {
        URLP_PARTS.forEach(function (k) { if (input[k] !== undefined) init[k] = String(input[k]); });
        if (input.baseURL) {
          var b = new URL(input.baseURL);
          var inherit = { protocol: b.protocol.slice(0, -1), hostname: b.hostname, port: b.port, username: b.username, password: b.password };
          for (var k in inherit) if (init[k] === undefined) init[k] = inherit[k];
          if (init.pathname === undefined) init.pathname = b.pathname;
        }
        URLP_PARTS.forEach(function (k) { if (init[k] === undefined) init[k] = "*"; });
      }
      var compiled = {}, regexps = false;
      URLP_PARTS.forEach(function (k) {
        var pat = init[k];
        if (k === "protocol") pat = pat.replace(/:$/, "");
        if (k === "search") pat = pat.replace(new RegExp("^\\?"), "");
        if (k === "hash") pat = pat.replace(/^#/, "");
        compiled[k] = urlpCompile(pat, k, ic && k !== "protocol");
        regexps = regexps || compiled[k].hasRegexp;
      });
      def(this, "_c", compiled);
      def(this, "_r", regexps);
    }
    get protocol() { return this._c.protocol.pattern; }
    get username() { return this._c.username.pattern; }
    get password() { return this._c.password.pattern; }
    get hostname() { return this._c.hostname.pattern; }
    get port() { return this._c.port.pattern; }
    get pathname() { return this._c.pathname.pattern; }
    get search() { return this._c.search.pattern; }
    get hash() { return this._c.hash.pattern; }
    get hasRegExpGroups() { return this._r; }
    exec(input, base) {
      var u, values = {};
      try {
        if (typeof input === "string" || input instanceof URL) u = new URL(String(input), base);
        else {
          var o = input || {};
          URLP_PARTS.forEach(function (k) { values[k] = o[k] === undefined ? "" : String(o[k]); });
        }
      } catch (e) { return null; }
      if (u) values = { protocol: u.protocol.slice(0, -1), username: u.username, password: u.password, hostname: u.hostname,
                        port: u.port, pathname: u.pathname, search: u.search.slice(1), hash: u.hash.slice(1) };
      var result = { inputs: base === undefined ? [input] : [input, base] };
      for (var i = 0; i < URLP_PARTS.length; i++) {
        var k = URLP_PARTS[i], c = this._c[k], m = c.re.exec(values[k]);
        if (!m) return null;
        var groups = {};
        for (var j = 0; j < c.names.length; j++) groups[c.names[j]] = m[j + 1];
        result[k] = { input: values[k], groups: groups };
      }
      return result;
    }
    test(input, base) { return this.exec(input, base) !== null; }
  }
  lz("URLPattern", URLPattern);
  lz("URLSearchParams", URLSearchParams);
  lz("URL", URL);

})(globalThis);
);

static const char js_chunk_web[] = JS_SOURCE(
(function (g) {
  var def = g["%def"], lz = g["%lazy"], ITER = Symbol.iterator;
  /* ---- TextEncoder, TextDecoder ---- */
  class TextEncoder {
    get encoding() { return "utf-8"; }
    encode(s) { return g["%encodeUTF8"](s === undefined ? "" : String(s)); }
    encodeInto(s, dest) {
      var read = 0, written = 0;
      for (var cp of String(s)) {
        var bytes = this.encode(cp);
        if (written + bytes.length > dest.length) break;
        dest.set(bytes, written); written += bytes.length; read += cp.length;
      }
      return { read: read, written: written };
    }
  }
  class TextDecoder {
    constructor(label, opts) {
      label = String(label || "utf-8").toLowerCase().trim();
      if (label !== "utf-8" && label !== "utf8" && label !== "unicode-1-1-utf-8") throw new RangeError("Unsupported encoding");
      def(this, "fatal", !!(opts && opts.fatal));
      def(this, "ignoreBOM", !!(opts && opts.ignoreBOM));
    }
    get encoding() { return "utf-8"; }
    decode(b, opts) {
      var input=b===undefined?new Uint8Array(0):b instanceof ArrayBuffer?new Uint8Array(b):ArrayBuffer.isView(b)?new Uint8Array(b.buffer,b.byteOffset,b.byteLength):null;
      if(!input)throw new TypeError("TextDecoder requires a buffer or view");
      var pending=this._pending||new Uint8Array(0), all=new Uint8Array(pending.length+input.length);all.set(pending);all.set(input,pending.length);
      var cut=all.length;
      if(opts&&opts.stream&&cut){var start=cut-1;while(start>=0&&(all[start]&192)===128)start--;
        if(start>=0){var lead=all[start], need=lead>=240&&lead<=244?4:lead>=224&&lead<=239?3:lead>=194&&lead<=223?2:1;
          if(cut-start<need){var valid=true;for(var i=start+1;i<cut;i++)if((all[i]&192)!==128)valid=false;
            if(cut-start>1&&((lead===224&&all[start+1]<160)||(lead===237&&all[start+1]>=160)||(lead===240&&all[start+1]<144)||(lead===244&&all[start+1]>=144)))valid=false;
            if(valid)cut=start;
          }
        }
      }
      this._pending=all.slice(cut);
      var ignore=this.ignoreBOM||!!this._begun, result=g["%decodeUTF8"](all.subarray(0,cut),this.fatal,ignore);
      if(cut)this._begun=true;if(!(opts&&opts.stream)){this._pending=new Uint8Array(0);this._begun=false;}
      return result;
    }
  }
  /* ---- crypto ---- */
  var crypto = {
    getRandomValues: function (a) { return g["%randomFill"](a); },
    randomUUID: function () {
      var bytes = new Uint8Array(16); g["%randomFill"](bytes);
      bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128;
      var hex = Array.from(bytes).map(function (b) { return (b < 16 ? "0" : "") + b.toString(16); }).join("");
      return hex.slice(0,8) + "-" + hex.slice(8,12) + "-" + hex.slice(12,16) + "-" + hex.slice(16,20) + "-" + hex.slice(20);
    },
    subtle: { digest: function (algorithm, data) {
      try { return Promise.resolve(g["%digest"](typeof algorithm === "string" ? algorithm : algorithm.name, data)); }
      catch(e) { if(String(e).indexOf("NotSupportedError")>=0)e.name="NotSupportedError"; return Promise.reject(e); }
    } }
  };
  var keyMaterial = new WeakMap();
  function CryptoKey() { throw new TypeError("Illegal constructor"); }
  function cryptoError(name, text) { return new DOMException(text, name); }
  function cryptoName(algorithm) {
    var name = typeof algorithm === "string" ? algorithm : algorithm && algorithm.name;
    if (typeof name !== "string") throw new TypeError("An algorithm name is required");
    return name.toUpperCase();
  }
  function cryptoHash(hash) {
    var name = cryptoName(hash);
    if (["SHA-1", "SHA-256", "SHA-384", "SHA-512"].indexOf(name) < 0) throw cryptoError("NotSupportedError", "Unsupported hash");
    return name;
  }
  function cryptoBytes(data) {
    if (data instanceof ArrayBuffer) return new Uint8Array(data).slice();
    if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength).slice();
    throw new TypeError("Expected a BufferSource");
  }
  function hmacKey(bytes, hash, extractable, usages) {
    usages = Array.from(usages);
    if (!usages.length) throw cryptoError("SyntaxError", "A secret key needs usages");
    usages.forEach(function (u) { if (u !== "sign" && u !== "verify") throw cryptoError("SyntaxError", "Invalid HMAC key usage"); });
    var key = Object.create(CryptoKey.prototype);
    Object.defineProperties(key, {
      type: { value: "secret", enumerable: true }, extractable: { value: !!extractable, enumerable: true },
      algorithm: { value: Object.freeze({ name: "HMAC", hash: Object.freeze({ name: hash }), length: bytes.length * 8 }), enumerable: true },
      usages: { value: Object.freeze(usages.slice()), enumerable: true }
    });
    keyMaterial.set(key, { bytes: bytes, hash: hash, extractable: !!extractable, usages: usages });
    return key;
  }
  function usableKey(key, usage) {
    var record = keyMaterial.get(key);
    if (!record || record.usages.indexOf(usage) < 0) throw cryptoError("InvalidAccessError", "Key does not allow this operation");
    return record;
  }
  function base64url(bytes) {
    var text = "";
    for (var i = 0; i < bytes.length; i++) text += String.fromCharCode(bytes[i]);
    return btoa(text).replace(new RegExp("\\+", "g"), "-").replace(new RegExp("/", "g"), "_").replace(/=+$/, "");
  }
  function decodeBase64url(text) {
    if (typeof text !== "string" || !/^[A-Za-z0-9_-]*$/.test(text) || text.length % 4 === 1)
      throw cryptoError("DataError", "Invalid JWK key data");
    text = text.replace(/-/g, "+").replace(/_/g, "/");
    while (text.length % 4) text += "=";
    var decoded = atob(text), bytes = new Uint8Array(decoded.length);
    for (var i = 0; i < decoded.length; i++) bytes[i] = decoded.charCodeAt(i);
    return bytes;
  }
  crypto.subtle.importKey = async function (format, data, algorithm, extractable, usages) {
    if (cryptoName(algorithm) !== "HMAC") throw cryptoError("NotSupportedError", "Unsupported key algorithm");
    var hash = cryptoHash(algorithm.hash), bytes;
    if (format === "raw") bytes = cryptoBytes(data);
    else if (format === "jwk") {
      if (!data || data.kty !== "oct" || (data.ext === false && extractable) || (data.use && data.use !== "sig"))
        throw cryptoError("DataError", "Invalid JWK");
      var expected = hash === "SHA-1" ? "HS1" : "HS" + hash.slice(4);
      if (data.alg && data.alg !== expected) throw cryptoError("DataError", "JWK algorithm mismatch");
      if (data.key_ops) Array.from(usages).forEach(function (u) {
        if (data.key_ops.indexOf(u) < 0) throw cryptoError("DataError", "JWK usage mismatch");
      });
      bytes = decodeBase64url(data.k);
    } else throw cryptoError("NotSupportedError", "Unsupported key format");
    if (!bytes.length) throw cryptoError("DataError", "An HMAC key cannot be empty");
    if (algorithm.length !== undefined && algorithm.length !== bytes.length * 8)
      throw cryptoError("NotSupportedError", "Only whole imported keys are supported");
    return hmacKey(bytes, hash, extractable, usages);
  };
  crypto.subtle.generateKey = async function (algorithm, extractable, usages) {
    if (cryptoName(algorithm) !== "HMAC") throw cryptoError("NotSupportedError", "Unsupported key algorithm");
    var hash = cryptoHash(algorithm.hash);
    var bits = algorithm.length === undefined ? (hash === "SHA-384" || hash === "SHA-512" ? 1024 : 512) : Number(algorithm.length);
    if (!isFinite(bits) || bits <= 0 || bits % 8 || bits > 524288) throw cryptoError("OperationError", "Invalid key length");
    var bytes = new Uint8Array(bits / 8); crypto.getRandomValues(bytes);
    return hmacKey(bytes, hash, extractable, usages);
  };
  crypto.subtle.exportKey = async function (format, key) {
    var record = keyMaterial.get(key);
    if (!record || !record.extractable) throw cryptoError("InvalidAccessError", "Key is not extractable");
    if (format === "raw") return record.bytes.slice().buffer;
    if (format === "jwk") return { kty: "oct", k: base64url(record.bytes), alg: record.hash === "SHA-1" ? "HS1" : "HS" + record.hash.slice(4), key_ops: record.usages.slice(), ext: true };
    throw cryptoError("NotSupportedError", "Unsupported key format");
  };
  crypto.subtle.sign = async function (algorithm, key, data) {
    if (cryptoName(algorithm) !== "HMAC") throw cryptoError("NotSupportedError", "Unsupported signing algorithm");
    var record = usableKey(key, "sign");
    return g["%hmac"](record.hash, record.bytes, cryptoBytes(data));
  };
  crypto.subtle.verify = async function (algorithm, key, signature, data) {
    if (cryptoName(algorithm) !== "HMAC") throw cryptoError("NotSupportedError", "Unsupported signing algorithm");
    var record = usableKey(key, "verify");
    var expected = g["%hmac"](record.hash, record.bytes, cryptoBytes(data));
    return g["%cryptoCompare"](expected, cryptoBytes(signature));
  };
  lz("CryptoKey", CryptoKey);
  /* ---- Blob, FormData ---- */
  class Blob {
    constructor(parts, opts) {
      var chunks = [], length = 0;
      Array.from(parts || []).forEach(function (p) {
        var b = p instanceof Blob ? p._bytes : p instanceof ArrayBuffer ? new Uint8Array(p) :
                ArrayBuffer.isView(p) ? new Uint8Array(p.buffer, p.byteOffset, p.byteLength) : new TextEncoder().encode(String(p));
        chunks.push(b); length += b.length;
      });
      var bytes = new Uint8Array(length), offset = 0;
      chunks.forEach(function (b) { bytes.set(b, offset); offset += b.length; });
      def(this, "_bytes", bytes);
      def(this, "_t", opts && opts.type ? String(opts.type).toLowerCase() : "");
    }
    get size() { return this._bytes.length; }
    get type() { return this._t; }
    text() { return Promise.resolve(new TextDecoder().decode(this._bytes)); }
    arrayBuffer() { return Promise.resolve(this._bytes.slice().buffer); }
    bytes() { return Promise.resolve(this._bytes.slice()); }
    slice(a, b, t) { return new Blob([this._bytes.slice(a, b)], { type: t || "" }); }
    stream() { var b = this._bytes.slice(); return new ReadableStream({ start: function (c) { c.enqueue(b); c.close(); } }); }
  }
  /* ---- File, FileList, FileReader ---- */
  class File extends Blob {
    constructor(bits, name, opts) {
      if (arguments.length < 2) throw new TypeError("File requires a name");
      super(bits, opts);
      def(this, "_n", String(name));
      def(this, "_m", opts && opts.lastModified !== undefined ? Number(opts.lastModified) : Date.now());
    }
    get name() { return this._n; }
    get lastModified() { return this._m; }
    get lastModifiedDate() { return new Date(this._m); }
    get webkitRelativePath() { return ""; }
  }
  Object.defineProperty(File.prototype, Symbol.toStringTag, { value: "File", configurable: true });
  class FileList {
    constructor() { def(this, "_f", []); }
    get length() { return this._f.length; }
    item(i) { return this._f[i] || null; }
    [ITER]() { return this._f[ITER](); }
  }
  def(g, "%fileList", function (files) {
    var l = new FileList();
    files.forEach(function (f, i) { l._f.push(f); Object.defineProperty(l, i, { value: f, enumerable: true }); });
    return l;
  });
  class FileReader extends EventTarget {
    constructor() {
      super();
      def(this, "_state", 0);
      def(this, "_result", null);
      def(this, "_error", null);
      def(this, "_job", 0);
      this.onloadstart = this.onprogress = this.onload = this.onerror = this.onabort = this.onloadend = null;
    }
    get readyState() { return this._state; }
    get result() { return this._result; }
    get error() { return this._error; }
    _read(blob, how, encoding) {
      if (this._state === 1) throw new DOMException("The object is already busy reading Blobs.", "InvalidStateError");
      if (!(blob instanceof Blob)) throw new TypeError("FileReader: parameter is not a Blob");
      var r = this, job = ++this._job, total = blob.size;
      this._state = 1;
      this._result = null;
      this._error = null;
      function fire(type) {
        var e = new Event(type);
        e.lengthComputable = true; e.loaded = type === "loadstart" ? 0 : total; e.total = total;
        r.dispatchEvent(e);
        var h = r["on" + type];
        if (typeof h === "function") h.call(r, e);
      }
      setTimeout(function () {
        if (job !== r._job) return;
        fire("loadstart");
        var bytes = blob._bytes, out;
        if (how === "text") out = new TextDecoder(encoding || "utf-8").decode(bytes);
        else if (how === "buffer") out = bytes.slice().buffer;
        else {
          var bin = "";
          for (var i = 0; i < bytes.length; i += 8192) bin += String.fromCharCode.apply(null, Array.from(bytes.subarray(i, i + 8192)));
          out = how === "binary" ? bin : "data:" + (blob.type || "application/octet-stream") + ";base64," + btoa(bin);
        }
        r._result = out;
        r._state = 2;
        fire("progress");
        fire("load");
        fire("loadend");
      }, 0);
    }
    readAsText(b, enc) { this._read(b, "text", enc); }
    readAsArrayBuffer(b) { this._read(b, "buffer"); }
    readAsDataURL(b) { this._read(b, "url"); }
    readAsBinaryString(b) { this._read(b, "binary"); }
    abort() {
      if (this._state !== 1) return;
      this._job++;
      this._state = 2;
      this._result = null;
      var e = new Event("abort");
      this.dispatchEvent(e);
      if (typeof this.onabort === "function") this.onabort(e);
      var end = new Event("loadend");
      this.dispatchEvent(end);
      if (typeof this.onloadend === "function") this.onloadend(end);
    }
  }
  FileReader.EMPTY = 0; FileReader.LOADING = 1; FileReader.DONE = 2;
  class FormData {
    constructor(form, submitter) {
      def(this, "_l", []);
      if (!form) return;
      var els = form.elements || form.querySelectorAll("input,select,textarea");
      for (var i = 0; i < els.length; i++) {
        var e = els[i], t = (e.type || "").toLowerCase();
        if (!e.name || e.disabled) continue;
        if ((t === "checkbox" || t === "radio") && !e.checked) continue;
        if ((t === "submit" || t === "image") && e === submitter) { this._l.push([e.name, String(e.value || "")]); continue; }
        if (t === "submit" || t === "button" || t === "reset" || t === "image") continue;
        if (t === "file") {
          var files = e.files;
          if (files && files.length) for (var k = 0; k < files.length; k++) this._l.push([e.name, files[k]]);
          else this._l.push([e.name, new File([], "", { type: "application/octet-stream" })]);
          continue;
        }
        this._l.push([e.name, String(e.value === undefined ? "" : e.value)]);
      }
      var assoc = g["%formAssociated"];          /* custom elements with ElementInternals.setFormValue */
      if (assoc) assoc(form, this._l);
    }
    append(k, v, name) {
      if (v instanceof Blob && !(v instanceof File)) v = new File([v], name === undefined ? "blob" : String(name), { type: v.type });
      else if (v instanceof File && name !== undefined) v = new File([v], String(name), { type: v.type, lastModified: v.lastModified });
      this._l.push([String(k), v instanceof Blob ? v : String(v)]);
    }
    set(k, v, name) { this.delete(k); this.append(k, v, name); }
    get(k) { for (var i = 0; i < this._l.length; i++) if (this._l[i][0] === k) return this._l[i][1]; return null; }
    getAll(k) { return this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); }
    has(k) { return this.get(k) !== null; }
    delete(k) { var l = this._l; for (var i = l.length - 1; i >= 0; i--) if (l[i][0] === k) l.splice(i, 1); }
    forEach(f, t) { var s = this; this._l.forEach(function (p) { f.call(t, p[1], p[0], s); }); }
    entries() { return this._l.map(function (p) { return [p[0], p[1]]; })[ITER](); }
    keys() { return this._l.map(function (p) { return p[0]; })[ITER](); }
    values() { return this._l.map(function (p) { return p[1]; })[ITER](); }
    [ITER]() { return this.entries(); }
  }
  /* ---- DOMParser: the markup goes into a detached element ---- */
  class DOMParser {
    parseFromString(s, type) {
      var d = document.createElement("div");
      d.innerHTML = s;
      var body = d.querySelector("body") || d;
      return {
        nodeType: 9, documentElement: d, body: body, head: d.querySelector("head"),
        title: (d.querySelector("title") || { textContent: "" }).textContent,
        querySelector: function (q) { return d.querySelector(q); },
        querySelectorAll: function (q) { return d.querySelectorAll(q); },
        getElementById: function (id) { return d.querySelector("#" + id); },
        getElementsByTagName: function (t) { return d.getElementsByTagName(t); },
        getElementsByClassName: function (c) { return d.getElementsByClassName(c); },
        createElement: function (t) { return document.createElement(t); }
      };
    }
  }
  /* ---- custom elements: jsdom.c calls the % hooks once one is defined ---- */
  var registry = Object.create(null), pending = Object.create(null), names = new Map();
  function upgrade(el) {
    var C = registry[el.localName];
    if (!C || el["%ce"]) return el;
    g["%upgrading"] = el;
    var inst;
    try { inst = new C(); } catch (e) { g["%upgrading"] = null; console.error(e); return el; }
    g["%upgrading"] = null;
    def(inst, "%ce", true);
    var obs = C.observedAttributes;
    if (obs && typeof inst.attributeChangedCallback === "function")
      Array.from(obs).forEach(function (a) {
        if (inst.hasAttribute(a)) inst.attributeChangedCallback(a, null, inst.getAttribute(a));
      });
    return inst;
  }
  var customElements = {
    define: function (name, ctor) {
      name = String(name);
      if (name.indexOf("-") < 0) throw new DOMException("Not a valid custom element name: " + name, "SyntaxError");
      if (registry[name]) throw new DOMException("This name has already been defined: " + name, "NotSupportedError");
      registry[name] = ctor;
      names.set(ctor, name);
      g["%watchCustom"](true);
      Array.from(document.querySelectorAll(name)).forEach(function (el) {
        var u = upgrade(el);
        if (u.isConnected && typeof u.connectedCallback === "function") u.connectedCallback();
      });
      if (pending[name]) { pending[name](ctor); delete pending[name]; }
    },
    get: function (name) { return registry[name]; },
    getName: function (ctor) { return names.get(ctor) || null; },
    whenDefined: function (name) {
      if (registry[name]) return Promise.resolve(registry[name]);
      var p = pending["%p" + name];
      if (!p) {
        p = pending["%p" + name] = new Promise(function (res) { pending[name] = res; });
      }
      return p;
    },
    upgrade: function (root) {
      Array.from(root.querySelectorAll ? root.querySelectorAll("*") : []).forEach(function (el) {
        if (registry[el.localName]) upgrade(el);
      });
    }
  };
  def(g, "%ceName", function (C) { return names.get(C); });
  def(g, "%ceCreate", function (name) {
    var C = registry[name];
    if (!C) return undefined;
    var el = new C();
    def(el, "%ce", true);
    return el;
  });
  def(g, "%ceConnected", function (el) {
    var u = upgrade(el);
    if (u["%ce"] && typeof u.connectedCallback === "function") u.connectedCallback();
  });
  def(g, "%ceDisconnected", function (el) {
    if (el["%ce"] && typeof el.disconnectedCallback === "function") el.disconnectedCallback();
  });
  def(g, "%ceAttr", function (el, n, o, v) {
    var C = registry[el.localName];
    if (el["%ce"] && C && typeof el.attributeChangedCallback === "function" && C.observedAttributes &&
        Array.from(C.observedAttributes).indexOf(n) >= 0)
      el.attributeChangedCallback(n, o, v);
  });
  /* ---- stubs for APIs a page may probe for ---- */
  var CSS = {
    supports: function (a, b) {
      return g["%cssSupports"](arguments.length > 1 ? "(" + a + ": " + b + ")" : String(a));
    },
    registerProperty: function () {},
    highlights: new Map(),
    px: function (n) { return n + "px"; },
    escape: function (value) {
      if (!arguments.length) throw new TypeError("CSS.escape requires an argument");
      var s = String(value), out = "";
      for (var i = 0; i < s.length; i++) {
        var c = s.charCodeAt(i);
        if (c >= 55296 && c <= 56319 && i + 1 < s.length &&
            s.charCodeAt(i + 1) >= 56320 && s.charCodeAt(i + 1) <= 57343) {
          out += s.slice(i, i + 2); i++; continue;
        }
        if (c === 0) { out += String.fromCharCode(65533); continue; }
        if ((c >= 1 && c <= 31) || c === 127 || (i === 0 && c >= 48 && c <= 57) ||
            (i === 1 && c >= 48 && c <= 57 && s.charCodeAt(0) === 45)) {
          out += "\\" + c.toString(16) + " "; continue;
        }
        if (i === 0 && c === 45 && s.length === 1) { out += "\\-"; continue; }
        if (c >= 128 || c === 45 || c === 95 || (c >= 48 && c <= 57) ||
            (c >= 65 && c <= 90) || (c >= 97 && c <= 122)) out += s.charAt(i);
        else out += "\\" + s.charAt(i);
      }
      return out;
    }
  };
  class Notification {
    constructor() {}
    static requestPermission() { return Promise.resolve("denied"); }
    close() {}
  }
  Notification.permission = "denied";
  class DOMRect {
    constructor(x, y, w, h) {
      this.x = x || 0; this.y = y || 0; this.width = w || 0; this.height = h || 0;
    }
    get left() { return Math.min(this.x, this.x + this.width); }
    get top() { return Math.min(this.y, this.y + this.height); }
    get right() { return Math.max(this.x, this.x + this.width); }
    get bottom() { return Math.max(this.y, this.y + this.height); }
    static fromRect(r) { r = r || {}; return new DOMRect(r.x, r.y, r.width, r.height); }
  }
  lz("TextEncoder", TextEncoder); lz("TextDecoder", TextDecoder); lz("crypto", crypto);
  lz("Blob", Blob); lz("FormData", FormData); lz("DOMParser", DOMParser);
  lz("File", File); lz("FileList", FileList); lz("FileReader", FileReader);
  lz("customElements", customElements); lz("CSS", CSS);
  lz("Notification", Notification); lz("DOMRect", DOMRect);
})(globalThis);
);

static const char js_chunk_idb[] = JS_SOURCE(
(function (g) {
  var def = g["%def"], lz = g["%lazy"];
  /* ---- IndexedDB. A database lives in memory here, loaded from the store of the browser the first
     time it is opened and written back whole when a transaction that changed it completes
     (%idb). Records are kept sorted by key; values are structured clones (%serialize). ---- */
  var ser = g["%serialize"], deser = g["%deserialize"], disk = g["%idb"];
  function err(name, msg) { return new DOMException(msg || name, name); }

  /* ---- keys: numbers, dates, strings, binary and arrays, in that order ---- */
  function keyType(k) {
    if (typeof k === "number") return k === k ? 1 : 0;
    if (k instanceof Date) return k.getTime() === k.getTime() ? 2 : 0;
    if (typeof k === "string") return 3;
    if (k instanceof ArrayBuffer || ArrayBuffer.isView(k)) return 4;
    if (Array.isArray(k)) return 5;
    return 0;
  }
  function validKey(k, seen) {
    var t = keyType(k);
    if (t !== 5) return t > 0;
    seen = seen || [];
    if (seen.indexOf(k) >= 0) return false;
    seen.push(k);
    for (var i = 0; i < k.length; i++) if (!(i in k) || !validKey(k[i], seen)) return false;
    return true;
  }
  function bin(k) { return k instanceof ArrayBuffer ? new Uint8Array(k) : new Uint8Array(k.buffer, k.byteOffset, k.byteLength); }
  function copyKey(k) {
    var t = keyType(k);
    if (t === 2) return new Date(k.getTime());
    if (t === 4) return bin(k).slice().buffer;
    if (t === 5) return k.map(copyKey);
    return k;
  }
  function cmp(a, b) {
    var ta = keyType(a), tb = keyType(b);
    if (!ta || !tb) throw err("DataError", "The parameter is not a valid key.");
    if (ta !== tb) return ta < tb ? -1 : 1;
    switch (ta) {
    case 2: a = a.getTime(); b = b.getTime(); break;
    case 4: {
      var p = bin(a), q = bin(b), n = Math.min(p.length, q.length);
      for (var i = 0; i < n; i++) if (p[i] !== q[i]) return p[i] < q[i] ? -1 : 1;
      a = p.length; b = q.length;
      break;
    }
    case 5:
      for (var j = 0; j < a.length && j < b.length; j++) { var c = cmp(a[j], b[j]); if (c) return c; }
      a = a.length; b = b.length;
      break;
    }
    return a < b ? -1 : a > b ? 1 : 0;
  }
  function checkKey(k) { if (!validKey(k)) throw err("DataError", "The parameter is not a valid key."); return copyKey(k); }

  /* ---- key paths: "a.b.c", or a list of them for an array key ---- */
  function evalPath(v, path) {
    if (Array.isArray(path)) {
      var out = [];
      for (var i = 0; i < path.length; i++) { var k = evalPath(v, path[i]); if (k === undefined) return undefined; out.push(k); }
      return out;
    }
    if (path === "") return v;
    var parts = path.split(".");
    for (var j = 0; j < parts.length; j++) {
      if (v === null || v === undefined) return undefined;
      if (typeof v === "string" && parts[j] === "length") v = v.length;
      else if (typeof v !== "object" || !(parts[j] in v)) return undefined;
      else v = v[parts[j]];
    }
    return v;
  }
  function injectKey(v, path, key) {
    var parts = path.split("."), o = v;
    for (var i = 0; i < parts.length - 1; i++) {
      if (o[parts[i]] === undefined) o[parts[i]] = {};
      o = o[parts[i]];
      if (typeof o !== "object" || o === null) throw err("DataError", "The key path cannot be set.");
    }
    o[parts[parts.length - 1]] = key;
  }

  class IDBKeyRange {
    constructor(lower, upper, lowerOpen, upperOpen) {
      this.lower = lower; this.upper = upper; this.lowerOpen = !!lowerOpen; this.upperOpen = !!upperOpen;
    }
    static only(k) { k = checkKey(k); return new IDBKeyRange(k, k, false, false); }
    static lowerBound(k, open) { return new IDBKeyRange(checkKey(k), undefined, open, true); }
    static upperBound(k, open) { return new IDBKeyRange(undefined, checkKey(k), true, open); }
    static bound(lo, hi, lowerOpen, upperOpen) {
      lo = checkKey(lo); hi = checkKey(hi);
      var c = cmp(lo, hi);
      if (c > 0 || (c === 0 && (lowerOpen || upperOpen))) throw err("DataError", "The lower bound is above the upper.");
      return new IDBKeyRange(lo, hi, lowerOpen, upperOpen);
    }
    includes(k) { return inRange(this, checkKey(k)); }
  }
  function toRange(q) {
    if (q === undefined || q === null) return null;
    return q instanceof IDBKeyRange ? q : IDBKeyRange.only(q);
  }
  function inRange(r, k) {
    if (!r) return true;
    if (r.lower !== undefined) { var c = cmp(k, r.lower); if (c < 0 || (c === 0 && r.lowerOpen)) return false; }
    if (r.upper !== undefined) { var d = cmp(k, r.upper); if (d > 0 || (d === 0 && r.upperOpen)) return false; }
    return true;
  }

  /* Sorted lists: records { k, v } by key; index entries { k, p } by key, then primary key. */
  function entryCmp(a, ak, ap) { var c = cmp(a.k, ak); return c || ap === undefined ? c : cmp(a.p, ap); }
  function lowerBoundOf(list, k, p) {          /* the first entry at or after (k, p) */
    var lo = 0, hi = list.length;
    while (lo < hi) { var mid = (lo + hi) >> 1; if (entryCmp(list[mid], k, p) < 0) lo = mid + 1; else hi = mid; }
    return lo;
  }
  function insertSorted(list, e) { list.splice(lowerBoundOf(list, e.k, e.p), 0, e); }

  /* ---- events, with bubbling from a request to its transaction and database ---- */
  class Target {
    addEventListener(t, f) {
      if (!f) return;
      var all = this._l || (this._l = {}), l = all[t] || (all[t] = []);
      if (l.indexOf(f) < 0) l.push(f);
    }
    removeEventListener(t, f) { var l = this._l && this._l[t]; if (l && l.indexOf(f) >= 0) l.splice(l.indexOf(f), 1); }
    dispatchEvent(e) { return fire([this], e); }
  }
  function makeEvent(type, bubbles, cancelable, extra) {
    var e = typeof Event === "function" ? new Event(type, { bubbles: bubbles, cancelable: cancelable }) :
      { type: type, bubbles: bubbles, cancelable: cancelable, defaultPrevented: false,
        preventDefault: function () { this.defaultPrevented = true; }, stopPropagation: function () { this.cancelBubble = true; } };
    if (extra) for (var k in extra) e[k] = extra[k];
    return e;
  }
  var handlerError;
  function fire(path, e) {
    var target = path[0];
    try { e.target = target; e.srcElement = target; } catch (x) {}
    for (var i = 0; i < path.length; i++) {
      var t = path[i];
      if (!t) continue;
      try { e.currentTarget = t; } catch (x) {}
      var fns = [], h = t["on" + e.type];
      if (typeof h === "function") fns.push(h);
      if (t._l && t._l[e.type]) fns = fns.concat(t._l[e.type]);
      for (var j = 0; j < fns.length; j++) {
        try { typeof fns[j] === "function" ? fns[j].call(t, e) : fns[j].handleEvent(e); }
        catch (x) { handlerError = x; if (typeof reportError === "function") reportError(x); }
      }
      if (!e.bubbles || e.cancelBubble || e.__stopped) break;
    }
    return !e.defaultPrevented;
  }

  /* ---- the databases of this origin ---- */
  var dbs = new Map();                          /* name: { name, version, stores: Map, conns: [] } */
  function blankStore(name, keyPath, auto) {
    return { name: name, keyPath: keyPath, autoIncrement: auto, current: 1, records: [], indexes: new Map() };
  }
  function blankIndex(name, keyPath, unique, multi) {
    return { name: name, keyPath: keyPath, unique: unique, multiEntry: multi, entries: [] };
  }
  function indexKeys(ix, value) {
    var k = evalPath(value, ix.keyPath);
    if (k === undefined) return [];
    if (ix.multiEntry && Array.isArray(k)) {
      var out = [];
      k.forEach(function (x) { if (validKey(x) && !out.some(function (y) { return cmp(x, y) === 0; })) out.push(copyKey(x)); });
      return out;
    }
    return validKey(k) ? [copyKey(k)] : [];
  }
  function load(name) {
    if (dbs.has(name)) return dbs.get(name);
    var text = disk ? disk("load", name) : null, db = null;
    if (text) {
      try {
        var saved = JSON.parse(text);
        db = { name: name, version: saved.version, stores: new Map(), conns: [] };
        saved.stores.forEach(function (s) {
          var st = blankStore(s.name, s.keyPath, s.autoIncrement);
          st.current = s.current;
          st.records = s.records.map(function (r) { return { k: deser(r[0]), v: r[1] }; });
          s.indexes.forEach(function (i) {
            var ix = blankIndex(i.name, i.keyPath, i.unique, i.multiEntry);
            st.records.forEach(function (r) {
              indexKeys(ix, deser(r.v)).forEach(function (k) { insertSorted(ix.entries, { k: k, p: r.k }); });
            });
            st.indexes.set(i.name, ix);
          });
          db.stores.set(s.name, st);
        });
      } catch (x) { db = null; }
    }
    if (db) dbs.set(name, db);
    return db;
  }
  function save(db) {
    if (!disk) return;
    var stores = [];
    db.stores.forEach(function (st) {
      var indexes = [];
      st.indexes.forEach(function (ix) { indexes.push({ name: ix.name, keyPath: ix.keyPath, unique: ix.unique, multiEntry: ix.multiEntry }); });
      stores.push({ name: st.name, keyPath: st.keyPath, autoIncrement: st.autoIncrement, current: st.current, indexes: indexes,
                    records: st.records.map(function (r) { return [ser(r.k), r.v]; }) });
    });
    var failure=disk("save", db.name, JSON.stringify({ version: db.version, stores: stores }));
    if(failure)throw err(failure,"Unable to commit database");
  }
  function names(map) {
    var list = Array.from(map.keys()).sort();
    list.contains = function (n) { return list.indexOf(n) >= 0; };
    list.item = function (i) { return list[i] === undefined ? null : list[i]; };
    return list;
  }

  /* ---- transactions: their requests run in order, a little after they are made; one that has
     nothing more to do after that completes ---- */
  var live = [], pumping = false;
  function schedule() { if (!pumping) { pumping = true; setTimeout(pump, 0); } }
  function pump() {
    pumping = false;
    live.slice().forEach(function (tx) {
      if (tx._finished) return;
      if (tx._queue.length) {
        tx._quiet = false;
        while (tx._queue.length && !tx._finished) run(tx, tx._queue.shift());
      } else if (tx._quiet) {
        finish(tx);
      } else {
        tx._quiet = true;
      }
    });
    live = live.filter(function (tx) { return !tx._finished; });
    if (live.length) schedule();
  }
  function run(tx, r) {
    var req = r.req;
    handlerError = undefined;
    try {
      req._result = r.op();
      req._error = null;
      req.readyState = "done";
      fire([req], makeEvent("success", false, false));
      if (handlerError !== undefined) abort(tx, err("AbortError", "An event handler threw."));
    } catch (x) {
      if (tx._finished) return;
      req._result = undefined;
      req._error = x instanceof DOMException ? x : err("UnknownError", String(x));
      req.readyState = "done";
      if (fire([req, tx, tx.db], makeEvent("error", true, true))) abort(tx, req._error);
    }
  }
  function touch(tx, st) {                      /* keep what a store was, for an abort */
    if (tx.mode === "readonly") throw err("ReadOnlyError", "The transaction is read-only.");
    if (!tx._snap.has(st)) {
      var idx = new Map();
      st.indexes.forEach(function (ix, n) { idx.set(n, { ix: ix, entries: ix.entries.slice() }); });
      tx._snap.set(st, { records: st.records.slice(), current: st.current, indexes: idx });
    }
    tx._dirty = true;
  }
  function finish(tx) {
    try { if (tx._dirty) save(tx.db._data); }
    catch(e){abort(tx,e);return;}
    tx._finished = true;
    fire([tx], makeEvent("complete", false, false));
    if (tx._done) tx._done(true);
  }
  function abort(tx, why) {
    if (tx._finished) return;
    tx._finished = true;
    tx.error = why || null;
    tx._snap.forEach(function (s, st) {
      st.records = s.records; st.current = s.current;
      st.indexes = new Map();
      s.indexes.forEach(function (x, n) { x.ix.entries = x.entries; st.indexes.set(n, x.ix); });
    });
    if (tx._schema) {                           /* a version change undone: the stores and version too */
      tx.db._data.stores = tx._schema.stores;
      tx.db._data.version = tx._schema.version;
      if (tx._schema.created) dbs.delete(tx.db._data.name);
    }
    tx._queue.forEach(function (r) {
      r.req._error = err("AbortError", "The transaction was aborted.");
      r.req.readyState = "done";
      fire([r.req, tx, tx.db], makeEvent("error", true, true));
    });
    tx._queue = [];
    fire([tx, tx.db], makeEvent("abort", true, false));
    if (tx._done) tx._done(false);
  }

  class IDBRequest extends Target {
    constructor(source, tx) {
      super();
      this.source = source; this.transaction = tx; this.readyState = "pending";
      this._result = undefined; this._error = null;
      this.onsuccess = null; this.onerror = null;
    }
    get result() { return this._result; }
    get error() { return this._error; }
  }
  class IDBOpenDBRequest extends IDBRequest {
    constructor() { super(null, null); this.onupgradeneeded = null; this.onblocked = null; }
  }
  function request(tx, source, op) {
    if (tx._finished) throw err("TransactionInactiveError", "The transaction has finished.");
    var req = new IDBRequest(source, tx);
    tx._queue.push({ req: req, op: op });
    tx._quiet = false;
    schedule();
    return req;
  }

  class IDBTransaction extends Target {
    constructor(db, scope, mode) {
      super();
      this.db = db; this.mode = mode; this.error = null; this.durability = "default";
      this._scope = scope; this._queue = []; this._snap = new Map(); this._dirty = false;
      this._finished = false; this._quiet = false;
      this.oncomplete = null; this.onerror = null; this.onabort = null;
      live.push(this);
      schedule();
    }
    get objectStoreNames() {
      var m = new Map();
      (this.mode === "versionchange" ? Array.from(this.db._data.stores.keys()) : this._scope).forEach(function (n) { m.set(n, 1); });
      return names(m);
    }
    objectStore(name) {
      if (this._finished) throw err("InvalidStateError", "The transaction has finished.");
      var st = this.db._data.stores.get(name);
      if (!st || (this.mode !== "versionchange" && this._scope.indexOf(name) < 0))
        throw err("NotFoundError", "No object store named " + name + ".");
      return new IDBObjectStore(this, st);
    }
    abort() {
      if (this._finished) throw err("InvalidStateError", "The transaction has finished.");
      abort(this, null);
    }
    commit() { if (!this._finished && !this._queue.length) finish(this); }
  }

  /* What a store or an index has, between two keys, in a direction. */
  function storeRange(st, range, dir, count) {
    var out = [], list = st.records, i;
    if (dir === "prev" || dir === "prevunique") {
      for (i = list.length - 1; i >= 0 && (!count || out.length < count); i--) if (inRange(range, list[i].k)) out.push(list[i]);
    } else {
      i = range && range.lower !== undefined ? lowerBoundOf(list, range.lower) : 0;
      for (; i < list.length && (!count || out.length < count); i++) {
        if (range && range.upper !== undefined && cmp(list[i].k, range.upper) > (range.upperOpen ? -1 : 0)) break;
        if (inRange(range, list[i].k)) out.push(list[i]);
      }
    }
    return out;
  }
  function record(st, key) {
    var i = lowerBoundOf(st.records, key);
    return i < st.records.length && cmp(st.records[i].k, key) === 0 ? st.records[i] : null;
  }
  function removeRecord(st, key) {
    var i = lowerBoundOf(st.records, key);
    if (i >= st.records.length || cmp(st.records[i].k, key)) return false;
    st.records.splice(i, 1);
    st.indexes.forEach(function (ix) { ix.entries = ix.entries.filter(function (e) { return cmp(e.p, key) !== 0; }); });
    return true;
  }

  class IDBObjectStore {
    constructor(tx, st) { this.transaction = tx; this._st = st; }
    get name() { return this._st.name; }
    get keyPath() { return this._st.keyPath; }
    get autoIncrement() { return this._st.autoIncrement; }
    get indexNames() { return names(this._st.indexes); }
    _write(value, key, add) {
      var tx = this.transaction, st = this._st;
      if (tx.mode === "readonly") throw err("ReadOnlyError", "The transaction is read-only.");
      if (st.keyPath !== null && key !== undefined) throw err("DataError", "The store uses in-line keys.");
      if (st.keyPath === null && !st.autoIncrement && key === undefined) throw err("DataError", "No key was given.");
      if (key !== undefined) key = checkKey(key);
      var text = ser(value), copy = deser(text);
      if (st.keyPath !== null) {
        var inline = evalPath(copy, st.keyPath);
        if (inline !== undefined) key = checkKey(inline);
        else if (!st.autoIncrement || Array.isArray(st.keyPath)) throw err("DataError", "The value has no key.");
      }
      var self = this;
      return request(tx, this, function () {
        touch(tx, st);
        if (key === undefined) {
          key = st.current++;
          if (st.keyPath !== null) { injectKey(copy, st.keyPath, key); text = ser(copy); }
        } else if (st.autoIncrement && typeof key === "number" && key >= st.current) {
          st.current = Math.floor(key) + 1;
        }
        if (add && record(st, key)) throw err("ConstraintError", "A record with this key already exists.");
        st.indexes.forEach(function (ix) {
          if (!ix.unique) return;
          indexKeys(ix, copy).forEach(function (k) {
            var at = lowerBoundOf(ix.entries, k);
            if (at < ix.entries.length && cmp(ix.entries[at].k, k) === 0 && cmp(ix.entries[at].p, key) !== 0)
              throw err("ConstraintError", "The index " + ix.name + " is unique.");
          });
        });
        removeRecord(st, key);
        insertSorted(st.records, { k: key, v: text });
        st.indexes.forEach(function (ix) { indexKeys(ix, copy).forEach(function (k) { insertSorted(ix.entries, { k: k, p: key }); }); });
        return copyKey(key);
      });
    }
    put(value, key) { return this._write(value, key, false); }
    add(value, key) { return this._write(value, key, true); }
    get(query) {
      var st = this._st, range = toRange(query);
      if (!range) throw err("DataError", "No key or key range was given.");
      return request(this.transaction, this, function () { var r = storeRange(st, range, "next", 1)[0]; return r ? deser(r.v) : undefined; });
    }
    getKey(query) {
      var st = this._st, range = toRange(query);
      return request(this.transaction, this, function () { var r = storeRange(st, range, "next", 1)[0]; return r ? copyKey(r.k) : undefined; });
    }
    getAll(query, count) {
      var st = this._st, range = toRange(query);
      return request(this.transaction, this, function () { return storeRange(st, range, "next", count).map(function (r) { return deser(r.v); }); });
    }
    getAllKeys(query, count) {
      var st = this._st, range = toRange(query);
      return request(this.transaction, this, function () { return storeRange(st, range, "next", count).map(function (r) { return copyKey(r.k); }); });
    }
    count(query) {
      var st = this._st, range = toRange(query);
      return request(this.transaction, this, function () { return storeRange(st, range, "next").length; });
    }
    delete(query) {
      var st = this._st, tx = this.transaction, range = toRange(query);
      if (tx.mode === "readonly") throw err("ReadOnlyError", "The transaction is read-only.");
      return request(tx, this, function () {
        touch(tx, st);
        storeRange(st, range, "next").forEach(function (r) { removeRecord(st, r.k); });
        return undefined;
      });
    }
    clear() {
      var st = this._st, tx = this.transaction;
      if (tx.mode === "readonly") throw err("ReadOnlyError", "The transaction is read-only.");
      return request(tx, this, function () {
        touch(tx, st);
        st.records = [];
        st.indexes.forEach(function (ix) { ix.entries = []; });
        return undefined;
      });
    }
    openCursor(query, dir) { return openCursor(this, toRange(query), dir || "next", true); }
    openKeyCursor(query, dir) { return openCursor(this, toRange(query), dir || "next", false); }
    index(name) {
      var ix = this._st.indexes.get(name);
      if (!ix) throw err("NotFoundError", "No index named " + name + ".");
      return new IDBIndex(this, ix);
    }
    createIndex(name, keyPath, opts) {
      var tx = this.transaction, st = this._st;
      if (tx.mode !== "versionchange") throw err("InvalidStateError", "Indexes are made in a version change.");
      if (st.indexes.has(name)) throw err("ConstraintError", "An index named " + name + " exists.");
      opts = opts || {};
      var ix = blankIndex(name, keyPath, !!opts.unique, !!opts.multiEntry);
      touch(tx, st);
      st.records.forEach(function (r) { indexKeys(ix, deser(r.v)).forEach(function (k) { insertSorted(ix.entries, { k: k, p: r.k }); }); });
      if (ix.unique)
        for (var i = 1; i < ix.entries.length; i++)
          if (cmp(ix.entries[i - 1].k, ix.entries[i].k) === 0) { abort(tx, err("ConstraintError", "The index " + name + " is unique.")); break; }
      st.indexes.set(name, ix);
      return new IDBIndex(this, ix);
    }
    deleteIndex(name) {
      if (this.transaction.mode !== "versionchange") throw err("InvalidStateError", "Indexes are removed in a version change.");
      if (!this._st.indexes.delete(name)) throw err("NotFoundError", "No index named " + name + ".");
    }
  }

  class IDBIndex {
    constructor(store, ix) { this.objectStore = store; this._ix = ix; }
    get name() { return this._ix.name; }
    get keyPath() { return this._ix.keyPath; }
    get unique() { return this._ix.unique; }
    get multiEntry() { return this._ix.multiEntry; }
    _range(query, count) {
      var ix = this._ix, range = toRange(query), out = [];
      for (var i = range && range.lower !== undefined ? lowerBoundOf(ix.entries, range.lower) : 0; i < ix.entries.length; i++) {
        if (count && out.length >= count) break;
        if (range && range.upper !== undefined && cmp(ix.entries[i].k, range.upper) > (range.upperOpen ? -1 : 0)) break;
        if (inRange(range, ix.entries[i].k)) out.push(ix.entries[i]);
      }
      return out;
    }
    get(query) {
      var self = this, st = this.objectStore._st;
      return request(this.objectStore.transaction, this, function () { var e = self._range(query, 1)[0], r = e && record(st, e.p); return r ? deser(r.v) : undefined; });
    }
    getKey(query) {
      var self = this;
      return request(this.objectStore.transaction, this, function () { var e = self._range(query, 1)[0]; return e ? copyKey(e.p) : undefined; });
    }
    getAll(query, count) {
      var self = this, st = this.objectStore._st;
      return request(this.objectStore.transaction, this, function () { return self._range(query, count).map(function (e) { return deser(record(st, e.p).v); }); });
    }
    getAllKeys(query, count) {
      var self = this;
      return request(this.objectStore.transaction, this, function () { return self._range(query, count).map(function (e) { return copyKey(e.p); }); });
    }
    count(query) {
      var self = this;
      return request(this.objectStore.transaction, this, function () { return self._range(query).length; });
    }
    openCursor(query, dir) { return openCursor(this, toRange(query), dir || "next", true); }
    openKeyCursor(query, dir) { return openCursor(this, toRange(query), dir || "next", false); }
  }

  /* ---- cursors: each step finds the next entry after where the cursor is, in the data as it is now ---- */
  class IDBCursor {
    constructor(source, range, dir, req) {
      this.source = source; this.direction = dir; this.request = req;
      this._range = range; this._key = undefined; this._pk = undefined; this._got = false;
    }
    get key() { return copyKey(this._key); }
    get primaryKey() { return copyKey(this._pk); }
    _store() { return this.source instanceof IDBIndex ? this.source.objectStore : this.source; }
    _step(target, targetPk, skip) {
      var index = this.source instanceof IDBIndex, list = index ? this.source._ix.entries : this.source._st.records;
      var back = this.direction.slice(0, 4) === "prev", unique = this.direction.slice(-6) === "unique";
      var i, e, found = null;
      for (i = back ? list.length - 1 : 0; back ? i >= 0 : i < list.length; i += back ? -1 : 1) {
        e = list[i];
        var pk = index ? e.p : e.k;
        if (!inRange(this._range, e.k)) continue;
        if (this._got) {
          var c = cmp(e.k, this._key);
          if (back ? c > 0 : c < 0) continue;
          if (c === 0 && (unique || !index || (back ? cmp(pk, this._pk) >= 0 : cmp(pk, this._pk) <= 0))) continue;
        }
        if (target !== undefined) {
          var t = cmp(e.k, target);
          if (back ? t > 0 : t < 0) continue;
          if (t === 0 && targetPk !== undefined && (back ? cmp(pk, targetPk) > 0 : cmp(pk, targetPk) < 0)) continue;
        }
        if (unique && back && found) {         /* prevunique: the first of a run of equal keys */
          if (cmp(found.k, e.k) === 0) { found = e; continue; }
          break;
        }
        found = e;
        if (skip > 1) { skip--; this._key = e.k; this._pk = pk; this._got = true; found = null; continue; }
        if (!(unique && back)) break;
      }
      if (!found) return null;
      this._key = found.k;
      this._pk = index ? found.p : found.k;
      this._got = true;
      if (this._withValue) { var r = record(this._store()._st, this._pk); this.value = r ? deser(r.v) : undefined; }
      return this;
    }
    _again(target, targetPk, skip) {
      var self = this, tx = this._store().transaction, req = this.request;
      req.readyState = "pending";
      tx._queue.push({ req: req, op: function () { return self._step(target, targetPk, skip); } });
      tx._quiet = false;
      schedule();
    }
    continue(key) { this._again(key === undefined ? undefined : checkKey(key), undefined, 1); }
    continuePrimaryKey(key, pk) { this._again(checkKey(key), checkKey(pk), 1); }
    advance(n) { if (!(n > 0)) throw new TypeError("advance needs a count above 0"); this._again(undefined, undefined, n); }
    update(value) {
      var st = this._store();
      return st.keyPath === null ? st.put(value, this._pk) : st.put(value);
    }
    delete() { return this._store().delete(this._pk); }
  }
  class IDBCursorWithValue extends IDBCursor {}
  function openCursor(source, range, dir, withValue) {
    var tx = source instanceof IDBIndex ? source.objectStore.transaction : source.transaction;
    var c = withValue ? new IDBCursorWithValue(source, range, dir, null) : new IDBCursor(source, range, dir, null);
    c._withValue = withValue;
    var req = request(tx, source, function () { return c._step(undefined, undefined, 1); });
    c.request = req;
    return req;
  }

  class IDBDatabase extends Target {
    constructor(data) {
      super();
      this._data = data; this._closed = false;
      this.onversionchange = null; this.onclose = null; this.onabort = null; this.onerror = null;
      data.conns.push(this);
    }
    get name() { return this._data.name; }
    get version() { return this._data.version; }
    get objectStoreNames() { return names(this._data.stores); }
    _versionTx() {
      var tx = this._upgrade;
      if (!tx || tx._finished) throw err("InvalidStateError", "Stores change only in a version change.");
      return tx;
    }
    createObjectStore(name, opts) {
      var tx = this._versionTx();
      opts = opts || {};
      var keyPath = opts.keyPath === undefined ? null : opts.keyPath;
      if (this._data.stores.has(name)) throw err("ConstraintError", "An object store named " + name + " exists.");
      if (opts.autoIncrement && (keyPath === "" || Array.isArray(keyPath))) throw err("InvalidAccessError", "That key path cannot be generated.");
      var st = blankStore(name, keyPath, !!opts.autoIncrement);
      this._data.stores.set(name, st);
      tx._dirty = true;
      return new IDBObjectStore(tx, st);
    }
    deleteObjectStore(name) {
      var tx = this._versionTx();
      if (!this._data.stores.delete(name)) throw err("NotFoundError", "No object store named " + name + ".");
      tx._dirty = true;
    }
    transaction(scope, mode) {
      if (this._closed) throw err("InvalidStateError", "The database connection is closed.");
      if (this._upgrade && !this._upgrade._finished) throw err("InvalidStateError", "A version change is running.");
      scope = typeof scope === "string" ? [scope] : Array.from(scope);
      if (!scope.length) throw err("InvalidAccessError", "No object stores were named.");
      var data = this._data;
      scope.forEach(function (n) { if (!data.stores.has(n)) throw err("NotFoundError", "No object store named " + n + "."); });
      mode = mode || "readonly";
      if (mode !== "readonly" && mode !== "readwrite") throw new TypeError("Unknown transaction mode " + mode);
      return new IDBTransaction(this, scope, mode);
    }
    close() {
      this._closed = true;
      var i = this._data.conns.indexOf(this);
      if (i >= 0) this._data.conns.splice(i, 1);
    }
  }

  class IDBVersionChangeEvent {}
  function versionEvent(type, oldVersion, newVersion) {
    var e = makeEvent(type, false, false, { oldVersion: oldVersion, newVersion: newVersion });
    try { Object.setPrototypeOf(e, IDBVersionChangeEvent.prototype); } catch (x) {}
    return e;
  }
  /* Others with the database open hear of the change; closed or not, it goes on. */
  function tellOthers(data, oldVersion, newVersion, except) {
    data.conns.slice().forEach(function (c) { if (c !== except && !c._closed) fire([c], versionEvent("versionchange", oldVersion, newVersion)); });
  }

  class IDBFactory {
    open(name, version) {
      name = String(name);
      if (version !== undefined && (!(version >= 1) || version !== Math.floor(version))) throw new TypeError("The version must be a whole number above 0.");
      var req = new IDBOpenDBRequest();
      setTimeout(function () {
        var data = load(name), created = !data;
        if (!data) { data = { name: name, version: 0, stores: new Map(), conns: [] }; dbs.set(name, data); }
        var old = data.version, want = version === undefined ? (old || 1) : version;
        if (want < old) {
          req._error = err("VersionError", "The database is at version " + old + ".");
          req.readyState = "done";
          fire([req], makeEvent("error", true, true));
          return;
        }
        var conn = new IDBDatabase(data);
        if (want === old) {
          req._result = conn; req.readyState = "done";
          fire([req], makeEvent("success", false, false));
          return;
        }
        tellOthers(data, old, want, conn);
        var tx = new IDBTransaction(conn, [], "versionchange");
        tx._schema = { stores: new Map(data.stores), version: old, created: created };
        tx._dirty = true;
        conn._upgrade = tx;
        data.version = want;
        tx._done = function (ok) {
          req.transaction = null;
          req.readyState = "done";
          if (ok) { req._result = conn; fire([req], makeEvent("success", false, false)); }
          else { conn.close(); req._result = undefined; req._error = tx.error || err("AbortError", "The version change was aborted."); fire([req], makeEvent("error", true, true)); }
        };
        req._result = conn; req.transaction = tx; req.readyState = "done";
        handlerError = undefined;
        fire([req], versionEvent("upgradeneeded", old, want));
        if (handlerError !== undefined) abort(tx, err("AbortError", "The upgrade handler threw."));
      }, 0);
      return req;
    }
    deleteDatabase(name) {
      name = String(name);
      var req = new IDBOpenDBRequest();
      setTimeout(function () {
        var data = load(name), old = data ? data.version : 0;
        if (data) { tellOthers(data, old, null, null); dbs.delete(name); }
        if (disk) disk("delete", name);
        req.readyState = "done";
        fire([req], versionEvent("success", old, null));
      }, 0);
      return req;
    }
    databases() {
      var seen = new Map();
      ((disk && disk("list", "")) || "").split("\n").forEach(function (n) { if (n) { var d = load(n); if (d) seen.set(n, d.version); } });
      dbs.forEach(function (d, n) { if (d.version) seen.set(n, d.version); });
      return Promise.resolve(Array.from(seen, function (e) { return { name: e[0], version: e[1] }; }));
    }
    cmp(a, b) { return cmp(a, b); }
  }

  var factory = new IDBFactory();
  lz("indexedDB", factory); lz("IDBFactory", IDBFactory); lz("IDBKeyRange", IDBKeyRange);
  lz("IDBDatabase", IDBDatabase); lz("IDBTransaction", IDBTransaction); lz("IDBObjectStore", IDBObjectStore);
  lz("IDBIndex", IDBIndex); lz("IDBCursor", IDBCursor); lz("IDBCursorWithValue", IDBCursorWithValue);
  lz("IDBRequest", IDBRequest); lz("IDBOpenDBRequest", IDBOpenDBRequest); lz("IDBVersionChangeEvent", IDBVersionChangeEvent);
})(globalThis);
);

static const char js_chunk_fetch[] = JS_SOURCE(
(function (g) {
  var def = g["%def"], listIter = g["%iter"], lz = g["%lazy"], ITER = Symbol.iterator;
  /* ---- fetch ---- */
  class Headers {
    constructor(init) {
      def(this, "_m", new Map());
      if (!init) return;
      var self = this;
      if (init instanceof Headers) init.forEach(function (v, k) { self.append(k, v); });
      else if (Array.isArray(init)) init.forEach(function (p) { self.append(p[0], p[1]); });
      else Object.keys(init).forEach(function (k) { self.append(k, init[k]); });
    }
    append(k, v) {
      k = String(k).toLowerCase();
      this._m.set(k, this._m.has(k) ? this._m.get(k) + ", " + v : String(v));
    }
    set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
    get(k) { k = String(k).toLowerCase(); return this._m.has(k) ? this._m.get(k) : null; }
    has(k) { return this._m.has(String(k).toLowerCase()); }
    delete(k) { this._m.delete(String(k).toLowerCase()); }
    forEach(f, t) { var self = this; this._m.forEach(function (v, k) { f.call(t, v, k, self); }); }
    keys() { return this._m.keys(); }
    values() { return this._m.values(); }
    entries() { return this._m.entries(); }
    [Symbol.iterator]() { return this._m.entries(); }
  }
  function streamStrategy(strategy, defaultMark) {
    strategy = strategy || {};
    var mark = strategy.highWaterMark === undefined ? defaultMark : Number(strategy.highWaterMark);
    if (mark !== mark || mark < 0) throw new RangeError("Invalid highWaterMark");
    if (strategy.size !== undefined && typeof strategy.size !== "function") throw new TypeError("Invalid size function");
    return { mark: mark, size: strategy.size || function () { return 1; } };
  }
  function streamDeferred() {
    var d = {};
    d.promise = new Promise(function (resolve, reject) { d.resolve = resolve; d.reject = reject; });
    d.promise.catch(function () {});
    return d;
  }
  class ReadableStream {
    constructor(source, strategy) {
      source = source || {};
      if (source.type !== undefined && source.type !== "bytes") throw new TypeError("Invalid stream type");
      this._bytes=source.type === "bytes";
      if(this._bytes && strategy && strategy.size!==undefined)throw new RangeError("Byte streams cannot have a size strategy");
      this._source = source; this._strategy = streamStrategy(strategy, this._bytes?0:1);
      this._q = []; this._wait = []; this._total = 0; this._state = "readable";
      this._closeRequested = false; this._disturbed = false; this._locked = false;
      this._pulling = false; this._again = false; this._started = false;
      this._readerClosed = null;
      var s = this;
      this._controller = {
        enqueue: function (chunk) {
          if (s._state !== "readable" || s._closeRequested) throw new TypeError("Stream is not readable");
          if(s._bytes) {
            if(!ArrayBuffer.isView(chunk) || !chunk.byteLength || chunk.buffer.detached)throw new TypeError("Expected a nonempty byte view");
            var offset=chunk.byteOffset,length=chunk.byteLength;
            var buffer=structuredClone(chunk.buffer,{transfer:[chunk.buffer]});
            var bytes=new Uint8Array(buffer,offset,length);
            s._q.push({value:bytes,size:length});s._total+=length;s._drainBytes();
          } else if (s._wait.length) s._wait.shift().resolve({ value: chunk, done: false });
          else {
            var size;
            try { size = Number(s._strategy.size(chunk)); if (!isFinite(size) || size < 0) throw new RangeError("Invalid chunk size"); }
            catch (e) { s._error(e); throw e; }
            s._q.push({ value: chunk, size: size }); s._total += size;
          }
          s._pull();
        },
        close: function () {
          if (s._state !== "readable" || s._closeRequested) throw new TypeError("Stream is not readable");
          s._closeRequested = true;if(s._bytes)s._drainBytes();s._finish();
        },
        error: function (e) { s._error(e); },
        get byobRequest() {
          if(!s._bytes || !s._wait.length || !s._wait[0].view)return null;
          var request=s._wait[0];
          if(request.request)return request.request;
          var valid=true;
          return request.request={
            get view(){return valid?new Uint8Array(request.view.buffer,request.view.byteOffset+request.filled,request.view.byteLength-request.filled):null;},
            respond:function(n){
              n=Number(n);
              if(!valid || s._wait[0]!==request)throw new TypeError("Invalid BYOB request");
              if(!Number.isInteger(n) || n<0 || n>request.view.byteLength-request.filled)throw new RangeError("Invalid response size");
              if(n===0 && !s._closeRequested)throw new TypeError("Zero response on an open stream");
              valid=false;request.request=null;request.filled+=n;s._drainBytes();s._finish();s._pull();
            },
            respondWithNewView:function(view){
              if(!ArrayBuffer.isView(view) || view.buffer!==request.view.buffer || view.byteOffset!==request.view.byteOffset+request.filled)
                throw new RangeError("View does not match BYOB request");
              this.respond(view.byteLength);
            }
          };
        },
        get desiredSize() { return s._state === "errored" ? null : s._state === "closed" ? 0 : s._strategy.mark - s._total; }
      };
      try {
        var started = source.start ? source.start(this._controller) : undefined;
        Promise.resolve(started).then(function () { s._started = true; s._pull(); }, function (e) { s._error(e); });
      } catch (e) { this._error(e); }
    }
    _drainBytes() {
      while(this._wait.length) {
        var request=this._wait[0];
        if(!request.view) {
          if(!this._q.length)break;
          var whole=this._q.shift();this._total-=whole.size;this._wait.shift().resolve({value:whole.value,done:false});continue;
        }
        while(this._q.length && request.filled<request.view.byteLength) {
          var entry=this._q[0],available=request.view.byteLength-request.filled,n=Math.min(available,entry.size);
          new Uint8Array(request.view.buffer,request.view.byteOffset+request.filled,n).set(entry.value.subarray(0,n));
          request.filled+=n;entry.value=entry.value.subarray(n);entry.size-=n;this._total-=n;
          if(!entry.size)this._q.shift();
        }
        var unit=request.unit;
        if(request.filled>=request.min && request.filled%unit===0 || this._closeRequested && !this._q.length) {
          if(request.filled%unit){this._error(new TypeError("Incomplete typed element"));return;}
          this._wait.shift();request.request=null;
          var original=request.view,buf=structuredClone(original.buffer,{transfer:[original.buffer]});
          var value=original instanceof DataView?new DataView(buf,request.offset,request.filled)
            :new request.ctor(buf,request.offset,request.filled/unit);
          request.resolve({value:value,done:request.filled===0});
        } else break;
      }
    }
    get locked() { return this._locked; }
    _finish() {
      if (this._state !== "readable" || !this._closeRequested || this._q.length) return;
      if(this._bytes)this._drainBytes();
      if(this._state!=="readable")return;
      this._state = "closed";
      while (this._wait.length) this._wait.shift().resolve({ value: undefined, done: true });
      if (this._readerClosed) this._readerClosed.resolve();
    }
    _error(e) {
      if (this._state !== "readable") return;
      this._state = "errored"; this._storedError = e; this._q = []; this._total = 0;
      while (this._wait.length) this._wait.shift().reject(e);
      if (this._readerClosed) this._readerClosed.reject(e);
    }
    _pull() {
      if (!this._started || this._state !== "readable" || this._closeRequested ||
          (!this._wait.length && this._controller.desiredSize <= 0) || !this._source.pull) return;
      if (this._pulling) { this._again = true; return; }
      this._pulling = true; var s = this;
      Promise.resolve().then(function () { return s._source.pull(s._controller); }).then(function () {
        s._pulling = false;
        if (s._again) { s._again = false; s._pull(); }
      }, function (e) { s._pulling = false; s._error(e); });
    }
    getReader(options) {
      if (this._locked) throw new TypeError("Stream is locked");
      var byob=options && options.mode!==undefined;
      if(byob && (options.mode!=="byob" || !this._bytes))throw new TypeError("BYOB requires a byte stream");
      this._locked = true;
      var s = this, released = false, pending = 0, closed = streamDeferred();
      this._readerClosed = closed;
      if (s._state === "closed") closed.resolve();
      if (s._state === "errored") closed.reject(s._storedError);
      return {
        get closed() { return closed.promise; },
        read: function (view, options) {
          if (released) return Promise.reject(new TypeError("Reader was released"));
          s._disturbed = true;
          if(byob) {
            if(!ArrayBuffer.isView(view) || view.buffer.detached || !view.byteLength)return Promise.reject(new TypeError("Expected a nonempty view"));
            var min=options && options.min!==undefined?Number(options.min):1,unit=view.BYTES_PER_ELEMENT || 1;
            if(!Number.isInteger(min) || min<1 || min>view.byteLength/unit)return Promise.reject(new RangeError("Invalid minimum"));
            if(s._state==="errored")return Promise.reject(s._storedError);
            var offset=view.byteOffset,size=view.byteLength,C=view.constructor;
            var buffer=structuredClone(view.buffer,{transfer:[view.buffer]}),request=streamDeferred();
            request.view=new Uint8Array(buffer,offset,size);
            request.unit=unit;request.offset=offset;request.ctor=C;request.min=min*unit;request.filled=0;
            if(C===DataView)request.view=new DataView(buffer,offset,size);
            s._wait.push(request);pending++;
            if(s._state==="closed")s._closeRequested=true;
            s._drainBytes();s._pull();
            return request.promise.then(function(v){pending--;return v;},function(e){pending--;throw e;});
          }
          if (s._q.length) {
            var entry = s._q.shift(); s._total -= entry.size; s._finish(); s._pull();
            return Promise.resolve({ value: entry.value, done: false });
          }
          if (s._state === "closed") return Promise.resolve({ value: undefined, done: true });
          if (s._state === "errored") return Promise.reject(s._storedError);
          var wait = streamDeferred(); s._wait.push(wait); pending++; s._pull();
          return wait.promise.then(function (v) { pending--; return v; }, function (e) { pending--; throw e; });
        },
        cancel: function (reason) { return released ? Promise.reject(new TypeError("Reader was released")) : s._cancel(reason); },
        releaseLock: function () {
          if (released) return;
          released = true; s._locked = false; s._readerClosed = null;
          var error = new TypeError("Reader was released");
          while (s._wait.length) s._wait.shift().reject(error);
          closed.reject(error);
        }
      };
    }
    _cancel(reason) {
      this._disturbed = true;
      if (this._state === "closed") return Promise.resolve();
      if (this._state === "errored") return Promise.reject(this._storedError);
      this._q = []; this._total = 0; this._closeRequested = true; this._finish();
      try { return Promise.resolve(this._source.cancel ? this._source.cancel(reason) : undefined).then(function () {}); }
      catch (e) { return Promise.reject(e); }
    }
    cancel(reason) { return this._locked ? Promise.reject(new TypeError("Stream is locked")) : this._cancel(reason); }
    async pipeTo(destination, options) {
      options = options || {};
      if (this.locked || destination.locked) throw new TypeError("Stream is locked");
      var reader = this.getReader(), writer = destination.getWriter(), signal = options.signal;
      var aborted = streamDeferred(), abort;
      var sinkFailed = streamDeferred();
      writer.closed.catch(function (e) { sinkFailed.reject(e); });
      if (signal) {
        abort = function () { aborted.reject(signal.reason); };
        if (signal.aborted) abort(); else signal.addEventListener("abort", abort, { once: true });
      }
      function step(p) { return Promise.race(signal ? [p, aborted.promise, sinkFailed.promise] : [p, sinkFailed.promise]); }
      try {
        while (true) {
          await step(writer.ready);
          var item = await step(reader.read());
          if (item.done) break;
          await step(writer.write(item.value));
        }
        if (!options.preventClose) await step(writer.close());
      } catch (error) {
        var cleanup = [];
        if (!options.preventCancel) cleanup.push(reader.cancel(error).catch(function () {}));
        if (!options.preventAbort) cleanup.push(writer.abort(error).catch(function () {}));
        await Promise.all(cleanup);
        throw error;
      } finally {
        if (signal) signal.removeEventListener("abort", abort);
        reader.releaseLock(); writer.releaseLock();
      }
    }
    pipeThrough(transform, options) {
      if (this.locked || transform.writable.locked) throw new TypeError("Stream is locked");
      this.pipeTo(transform.writable, options).catch(function () {});
      return transform.readable;
    }
    tee() {
      var reader = this.getReader(), controls = [], canceled = [false, false], reasons = [], reading = false;
      var cancelDone = streamDeferred();
      function pull() {
        if (reading) return;
        reading = true;
        return reader.read().then(function (item) {
          reading = false;
          for (var i = 0; i < 2; i++) if (!canceled[i]) {
            if (item.done) controls[i].close();
            else controls[i].enqueue(item.value);
          }
          if (item.done) { reader.releaseLock(); cancelDone.resolve(); }
        }, function (e) {
          reading = false;
          for (var i = 0; i < 2; i++) if (!canceled[i]) controls[i].error(e);
          reader.releaseLock(); cancelDone.resolve();
        });
      }
      function branch(i) {
        return new ReadableStream({ start: function (c) { controls[i] = c; }, pull: pull,
          cancel: function (reason) {
            canceled[i] = true; reasons[i] = reason;
            if (canceled[0] && canceled[1]) reader.cancel(reasons).then(function () { reader.releaseLock(); cancelDone.resolve(); }, cancelDone.reject);
            return cancelDone.promise;
          }
        });
      }
      return [branch(0), branch(1)];
    }
    async *[Symbol.asyncIterator]() {
      var reader = this.getReader(), done = false;
      try { while (true) { var item = await reader.read(); if (item.done) { done = true; return; } yield item.value; } }
      finally { if (!done) await reader.cancel(); reader.releaseLock(); }
    }
  }
  class WritableStream {
    constructor(sink, strategy) {
      this._sink = sink || {}; this._strategy = streamStrategy(strategy, 1);
      if (this._sink.type !== undefined) throw new RangeError("Invalid sink type");
      this._state = "writable"; this._locked = false; this._total = 0;
      this._closed = streamDeferred(); this._ready = streamDeferred();
      this._controller = new AbortController();
      var s = this;
      var controller = { signal: this._controller.signal, error: function (e) { s._fail(e); } };
      try { this._chain = Promise.resolve(this._sink.start ? this._sink.start(controller) : undefined); }
      catch (e) { this._chain = Promise.reject(e); }
      this._chain = this._chain.catch(function (e) { s._fail(e); });
      this._updateReady();
    }
    get locked() { return this._locked; }
    _updateReady() {
      if (this._total < this._strategy.mark || this._state === "closed") this._ready.resolve();
      else if (!this._backpressure) this._ready = streamDeferred();
      this._backpressure = this._total >= this._strategy.mark;
    }
    _fail(e) {
      if (this._state === "closed" || this._state === "errored") return;
      this._state = "errored"; this._storedError = e;
      this._closed.reject(e); this._ready.reject(e);
      this._ready = streamDeferred(); this._ready.reject(e);
    }
    _write(chunk) {
      if (this._state !== "writable") return Promise.reject(this._state === "errored" ? this._storedError : new TypeError("Stream is not writable"));
      var size;
      try { size = Number(this._strategy.size(chunk)); if (!isFinite(size) || size < 0) throw new RangeError("Invalid chunk size"); }
      catch (e) { this._fail(e); return Promise.reject(e); }
      this._total += size; this._updateReady();
      var s = this;
      var work = this._chain.then(function () {
        if (s._state === "errored") throw s._storedError;
        return s._sink.write ? s._sink.write(chunk, { signal: s._controller.signal, error: function (e) { s._fail(e); } }) : undefined;
      }).then(function () { s._total -= size; s._updateReady(); }, function (e) { s._fail(e); throw e; });
      this._chain = work.catch(function () {});
      return work;
    }
    _close() {
      if (this._state !== "writable") return Promise.reject(this._state === "errored" ? this._storedError : new TypeError("Stream cannot close"));
      this._state = "closing"; var s = this;
      var work = this._chain.then(function () {
        if (s._state === "errored") throw s._storedError;
        return s._sink.close ? s._sink.close() : undefined;
      }).then(function () { s._state = "closed"; s._closed.resolve(); s._ready.resolve(); }, function (e) { s._fail(e); throw e; });
      this._chain = work.catch(function () {});
      return work;
    }
    _abort(reason) {
      if (this._state === "closed" || this._state === "errored") return Promise.resolve();
      this._controller.abort(reason); this._fail(reason); var s = this;
      return this._chain.then(function () { return s._sink.abort ? s._sink.abort(reason) : undefined; }).then(function () {});
    }
    abort(reason) { return this.locked ? Promise.reject(new TypeError("Stream is locked")) : this._abort(reason); }
    close() { return this.locked ? Promise.reject(new TypeError("Stream is locked")) : this._close(); }
    getWriter() {
      if (this.locked) throw new TypeError("Stream is locked");
      this._locked = true; var s = this, released = false;
      return {
        get ready() { return released ? Promise.reject(new TypeError("Writer was released")) : s._ready.promise; },
        get closed() { return released ? Promise.reject(new TypeError("Writer was released")) : s._closed.promise; },
        get desiredSize() { if (released) throw new TypeError("Writer was released"); return s._state === "errored" ? null : s._state === "closed" ? 0 : s._strategy.mark - s._total; },
        write: function (chunk) { return released ? Promise.reject(new TypeError("Writer was released")) : s._write(chunk); },
        close: function () { return released ? Promise.reject(new TypeError("Writer was released")) : s._close(); },
        abort: function (reason) { return released ? Promise.reject(new TypeError("Writer was released")) : s._abort(reason); },
        releaseLock: function () { if (!released) { released = true; s._locked = false; } }
      };
    }
  }
  class TransformStream {
    constructor(transformer, writableStrategy, readableStrategy) {
      transformer = transformer || {};
      if (transformer.readableType !== undefined || transformer.writableType !== undefined) throw new RangeError("Invalid transform type");
      var control, writableControl, drain = streamDeferred(), t = this, pending = 0, terminated = false;
      function fail(e) { control.error(e); if (writableControl) writableControl.error(e); drain.reject(e); }
      this.readable = new ReadableStream({
        start: function (c) { control = c; },
        pull: function () { pending++; drain.resolve(); },
        cancel: function (reason) { fail(reason); }
      }, readableStrategy === undefined ? { highWaterMark: 0 } : readableStrategy);
      var controller = {
        enqueue: function (chunk) { control.enqueue(chunk); pending = Math.max(0, pending - 1); if (control.desiredSize <= 0 && !pending) drain = streamDeferred(); },
        error: fail,
        terminate: function () { terminated = true; control.close(); if (writableControl) writableControl.error(new TypeError("Transform terminated")); drain.reject(new TypeError("Transform terminated")); },
        get desiredSize() { return control.desiredSize; }
      };
      this.writable = new WritableStream({
        start: function (c) {
          writableControl = c;
          try { return Promise.resolve(transformer.start ? transformer.start(controller) : undefined).catch(function (e) { fail(e); throw e; }); }
          catch (e) { fail(e); throw e; }
        },
        write: async function (chunk) {
          if (control.desiredSize <= 0 && !pending) await drain.promise;
          if (terminated) throw new TypeError("Transform terminated");
          try { if (transformer.transform) await transformer.transform(chunk, controller); else controller.enqueue(chunk); }
          catch (e) { fail(e); throw e; }
        },
        close: async function () { try { if (transformer.flush) await transformer.flush(controller); if (!terminated) control.close(); } catch (e) { fail(e); throw e; } },
        abort: function (reason) { control.error(reason); drain.reject(reason); }
      }, writableStrategy);
    }
  }
  class CountQueuingStrategy {
    constructor(options) { this.highWaterMark = Number(options.highWaterMark); }
    size() { return 1; }
  }
  class ByteLengthQueuingStrategy {
    constructor(options) { this.highWaterMark = Number(options.highWaterMark); }
    size(chunk) { return chunk.byteLength; }
  }
  function bytesOf(body) {
    if (body === undefined || body === null) return new Uint8Array(0);
    if (body instanceof Blob) return body._bytes.slice();
    if (body instanceof ArrayBuffer) return new Uint8Array(body).slice();
    if (ArrayBuffer.isView(body)) return new Uint8Array(body.buffer, body.byteOffset, body.byteLength).slice();
    return new TextEncoder().encode(String(body));
  }
  class Response {
    constructor(body, init) {
      init = init || {};
      def(this, "_b", body instanceof ReadableStream?null:bytesOf(body)); def(this, "_used", false);
      this.status = init.status === undefined ? 200 : init.status;
      this.statusText = init.statusText || ""; this.headers = new Headers(init.headers);
      this.ok = this.status >= 200 && this.status < 300; this.url = "";
      this.redirected = false; this.type = "default";
      var b = this._b, response = this;
      this.body = body instanceof ReadableStream?body:body === null || body === undefined ? null : new ReadableStream({ start: function (c) { if (b.length) c.enqueue(b.slice()); c.close(); } });
      if (typeof body === "string" && !this.headers.has("content-type")) this.headers.set("content-type", "text/plain;charset=UTF-8");
      else if (body instanceof Blob && body.type && !this.headers.has("content-type")) this.headers.set("content-type", body.type);
    }
    get bodyUsed() { return this._used || !!(this.body && this.body._disturbed); }
    _consume() {
      if (this.bodyUsed || (this.body && this.body.locked)) return Promise.reject(new TypeError("Body has already been read or locked"));
      this._used = true; var chunks = [], total = 0, stream = this.body;
      if (!stream) return Promise.resolve(new Uint8Array(0));
      var reader = stream.getReader();
      function read() { return reader.read().then(function (v) {
        if (!v.done) { chunks.push(v.value); total += v.value.length; return read(); }
        var b = new Uint8Array(total), at = 0; chunks.forEach(function (c) { b.set(c, at); at += c.length; }); return b;
      }); }
      return read();
    }
    bytes() { return this._consume(); }
    arrayBuffer() { return this.bytes().then(function (b) { return b.buffer; }); }
    text() { return this.bytes().then(function (b) { return new TextDecoder().decode(b); }); }
    json() { return this.text().then(function (t) { return JSON.parse(t); }); }
    blob() { var type = this.headers.get("content-type") || ""; return this.bytes().then(function (b) { return new Blob([b], { type: type }); }); }
    clone() {
      if (this.bodyUsed || (this.body && this.body.locked)) throw new TypeError("Body has already been read or locked");
      var branch=null;if(this.body){var pair=this.body.tee();this.body=pair[0];branch=pair[1];}
      var r = new Response(branch, { status: this.status, statusText: this.statusText, headers: this.headers });
      r.url = this.url; r.type = this.type; r.redirected = this.redirected; return r;
    }
    static json(data, init) { var r = new Response(JSON.stringify(data), init); if (!(init && init.headers && new Headers(init.headers).has("content-type"))) r.headers.set("content-type", "application/json"); return r; }
    static error() { var r = new Response(null, { status: 0 }); r.type = "error"; return r; }
    static redirect(url, status) { status = status || 302; if ([301,302,303,307,308].indexOf(status) < 0) throw new RangeError("Invalid redirect status"); return new Response(null, { status: status, headers: { location: String(url) } }); }
  }
  class Request {
    constructor(input, init) {
      init = init || {}; var from = input instanceof Request ? input : null;
      this.url = from ? from.url : String(input);
      this.method = String(init.method || (from && from.method) || "GET").toUpperCase();
      this.headers = new Headers(init.headers || (from && from.headers));
      this.body = init.body !== undefined ? init.body : from ? from.body : null;
      this.signal = init.signal || (from && from.signal) || null;
      this.credentials = init.credentials || (from && from.credentials) || "same-origin";
      this.mode = init.mode || (from && from.mode) || "cors";
      if (["omit","same-origin","include"].indexOf(this.credentials) < 0 || ["cors","no-cors","same-origin"].indexOf(this.mode) < 0) throw new TypeError("Invalid fetch options");
      if ((this.method === "GET" || this.method === "HEAD") && this.body !== null) throw new TypeError("GET and HEAD cannot have a body");
    }
    clone() { return new Request(this); }
  }
  function parseHeaders(text) {
    var h = new Headers(), lines = String(text).split("\n");
    for (var i = 0; i < lines.length; i++) { var c = lines[i].indexOf(":"); if (c > 0) h.append(lines[i].slice(0, c).trim(), lines[i].slice(c + 1).trim()); }
    return h;
  }
  /* ---- streams that decode, encode, compress and decompress ---- */
  class TextDecoderStream extends TransformStream {
    constructor(label, options) {
      var dec = new TextDecoder(label, options);
      super({
        transform: function (chunk, c) { var t = dec.decode(chunk, { stream: true }); if (t) c.enqueue(t); },
        flush: function (c) { var t = dec.decode(); if (t) c.enqueue(t); }
      });
      def(this, "_d", dec);
    }
    get encoding() { return this._d.encoding; }
    get fatal() { return !!this._d.fatal; }
    get ignoreBOM() { return !!this._d.ignoreBOM; }
  }
  class TextEncoderStream extends TransformStream {
    constructor() {
      var enc = new TextEncoder(), pending = "";
      super({
        transform: function (chunk, c) {
          var t = pending + String(chunk);
          pending = "";
          var last = t.charCodeAt(t.length - 1);
          if (last >= 0xD800 && last <= 0xDBFF) { pending = t.slice(-1); t = t.slice(0, -1); }   /* half a surrogate pair waits */
          if (t) c.enqueue(enc.encode(t));
        },
        flush: function (c) { if (pending) c.enqueue(enc.encode(pending)); }
      });
    }
    get encoding() { return "utf-8"; }
  }
  function streamBytes(chunk) {
    if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
    if (ArrayBuffer.isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    throw new TypeError("The chunk is not an ArrayBuffer or ArrayBufferView");
  }
  function codecStream(format, work) {
    format = String(format);
    if (format !== "gzip" && format !== "deflate" && format !== "deflate-raw")
      throw new TypeError("Unsupported compression format: " + format);
    var parts = [], length = 0;
    return {
      transform: function (chunk) { var b = streamBytes(chunk); parts.push(b.slice()); length += b.length; },
      flush: function (c) {
        var all = new Uint8Array(length), at = 0;
        parts.forEach(function (b) { all.set(b, at); at += b.length; });
        c.enqueue(g[work](all, format));
      }
    };
  }
  class CompressionStream extends TransformStream { constructor(format) { super(codecStream(format, "%compress")); } }
  class DecompressionStream extends TransformStream { constructor(format) { super(codecStream(format, "%decompress")); } }
  lz("TextDecoderStream", TextDecoderStream); lz("TextEncoderStream", TextEncoderStream);
  lz("CompressionStream", CompressionStream); lz("DecompressionStream", DecompressionStream);
  lz("WritableStream", WritableStream); lz("TransformStream", TransformStream);
  lz("CountQueuingStrategy", CountQueuingStrategy); lz("ByteLengthQueuingStrategy", ByteLengthQueuingStrategy);
  lz("Headers", Headers); lz("Response", Response); lz("Request", Request); lz("ReadableStream", ReadableStream);
  lz("fetch", function (input, init) {
    return new Promise(function (resolve, reject) {
      var req = new Request(input, init), signal = req.signal;
      if (signal && signal.aborted) { reject(signal.reason); return; }
      var x = new XMLHttpRequest(), done = false, resolved=false, controller=null, response=null;
      x.open(req.method, req.url); x.responseType = "arraybuffer";x.__streaming=true;
      x.__credentials = req.credentials; x.__mode = req.mode;
      req.headers.forEach(function (v, k) { x.setRequestHeader(k, v); });
      if (typeof req.body === "string" && !req.headers.has("content-type")) x.setRequestHeader("Content-Type", "text/plain;charset=UTF-8");
      if (req.body instanceof Blob && req.body.type && !req.headers.has("content-type")) x.setRequestHeader("Content-Type", req.body.type);
      function cleanup() { if (signal) signal.removeEventListener("abort", abort); }
      function failed(e){if(done)return;done=true;cleanup();if(controller)controller.error(e);if(!resolved)reject(e);}
      function abort() { if(done)return;var reason=signal.reason;failed(reason);x.abort(); }
      if (signal) signal.addEventListener("abort", abort, { once: true });
      function headers(){
        if(done||resolved||x.readyState<2)return;
        var opaque = req.mode === "no-cors" && x.status === 0;
        var empty=opaque||req.method==="HEAD"||x.status===204||x.status===205||x.status===304;
        var stream=empty?null:new ReadableStream({type:"bytes",start:function(c){controller=c;},pull:function(){if(g["%httpPause"])g["%httpPause"](x,false);},cancel:function(){done=true;cleanup();x.abort();}},{highWaterMark:65536});
        response=new Response(stream,{status:x.status,statusText:x.statusText,headers:parseHeaders(x.getAllResponseHeaders())});
        response.url=opaque?"":x.responseURL||req.url;response.type=opaque?"opaque":"basic";response.redirected=response.url!==new URL(req.url,location.href).href;
        resolved=true;resolve(response);
      }
      x.onreadystatechange=function(){if(x.readyState===2)headers();};
      x.onprogress=function(ev){if(!done&&controller&&ev.data){controller.enqueue(ev.data);if(g["%httpPause"])g["%httpPause"](x,controller.desiredSize<=0);}};
      x.onload=function(){if(done)return;headers();done=true;cleanup();if(controller)controller.close();};
      x.onerror=function(){failed(new TypeError("Failed to fetch"));};
      x.onabort=function(){failed(new DOMException("The operation was aborted","AbortError"));};
      x.send(req.body === undefined || req.body === null ? null : bytesOf(req.body));
    });
  });

  class EventSource extends EventTarget {
    constructor(url,options){
      super();this._url=new URL(String(url),location.href).href;
      if(!/^https?:/.test(this._url))throw new SyntaxError("EventSource requires an HTTP URL");
      this._state=0;this._credentials=!!(options&&options.withCredentials);this._lastId="";this._retry=3000;this._timer=null;this._abort=null;
      var self=this;Promise.resolve().then(function(){self._connect();});
    }
    get url(){return this._url;}get readyState(){return this._state;}get withCredentials(){return this._credentials;}
    close(){this._state=2;if(this._timer!==null)clearTimeout(this._timer);if(this._abort)this._abort.abort();}
    _connect(){
      if(this._state===2)return;
      var self=this;this._abort=new AbortController();var headers={Accept:"text/event-stream"};if(this._lastId)headers["Last-Event-ID"]=this._lastId;
      fetch(this._url,{headers:headers,credentials:this._credentials?"include":"same-origin",signal:this._abort.signal}).then(function(response){
        if(self._state===2)return;
        if(response.status===204){self.close();return;}
        if(response.status!==200 || (response.headers.get("content-type")||"").split(";")[0].trim().toLowerCase()!=="text/event-stream"){
          self.close();self.dispatchEvent(new Event("error"));return;
        }
        self._state=1;self.dispatchEvent(new Event("open"));
        var reader=response.body.getReader(),decoder=new TextDecoder(),line="",data="",event="",skipLF=false;
        function parseLine(){
          var text=line;line="";
          if(!text){if(data){self.dispatchEvent(new MessageEvent(event||"message",{data:data.slice(0,-1),origin:new URL(response.url).origin,lastEventId:self._lastId}));}data="";event="";return;}
          if(text[0]===":")return;
          var colon=text.indexOf(":"),field=colon<0?text:text.slice(0,colon),value=colon<0?"":text.slice(colon+1);if(value[0]===" ")value=value.slice(1);
          if(field==="data")data+=value+"\n";else if(field==="event")event=value;
          else if(field==="id"&&value.indexOf("\0")<0)self._lastId=value;
          else if(field==="retry"&&/^[0-9]+$/.test(value))self._retry=Math.min(2147483647,Number(value));
          if(data.length>4194304)throw new RangeError("EventSource message limit exceeded");
        }
        function text(chunk){for(var i=0;i<chunk.length;i++){var c=chunk[i];if(skipLF){skipLF=false;if(c==="\n")continue;}
          if(c==="\r"||c==="\n"){parseLine();skipLF=c==="\r";}else {line+=c;if(line.length>1048576)throw new RangeError("EventSource line limit exceeded");}
        }}
        function read(){return reader.read().then(function(v){if(self._state===2){reader.releaseLock();return;}
          if(v.done){text(decoder.decode());reader.releaseLock();self._reconnect();return;}
          text(decoder.decode(v.value,{stream:true}));return read();
        });}
        return read();
      }).catch(function(){if(self._state!==2)self._reconnect();});
    }
    _reconnect(){if(this._state===2)return;this._state=0;this.dispatchEvent(new Event("error"));var self=this;if(this._state!==2)this._timer=setTimeout(function(){self._connect();},this._retry);}
  }
  ["CONNECTING","OPEN","CLOSED"].forEach(function(name,i){Object.defineProperty(EventSource,name,{value:i});Object.defineProperty(EventSource.prototype,name,{value:i});});
  lz("EventSource",EventSource);

})(globalThis);
);

