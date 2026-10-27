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
  /* ---- iterators ---- */
  function listIter(a) {
    var i = 0;
    var it = {
      next: function () {
        if (i < a.length) return { value: a[i++], done: false };
        return { value: undefined, done: true };
      }
    };
    def(it, ITER, function () { return this; });
    return it;
  }
  def(g, "%def", def);
  def(g, "%iter", listIter);
  ["Array", "Map", "Set"].forEach(function (n) {
    var P = g[n].prototype;
    ["keys", "values", "entries"].forEach(function (m) {
      var orig = P[m];
      if (orig) def(P, m, function () { return listIter(orig.call(this)); });
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
  def(String.prototype, "normalize", function () { return String(this); });
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
    ownKeys: function (o) { return Object.getOwnPropertyNames(o); },
    has: function (o, k) { return k in o; },
    get: function (o, k) { return o[k]; },
    set: function (o, k, v) { o[k] = v; return true; },
    getPrototypeOf: function (o) { return Object.getPrototypeOf(o); },
    setPrototypeOf: function (o, p) { Object.setPrototypeOf(o, p); return true; },
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
  /* ---- structuredClone, base64 ---- */
  function copy(v, seen) {
    if (v === null || typeof v !== "object") {
      if (typeof v === "function") throw new DOMException("A function could not be cloned", "DataCloneError");
      return v;
    }
    if (seen.has(v)) return seen.get(v);
    var r;
    if (v instanceof Date) return new Date(v.getTime());
    if (v instanceof RegExp) return new RegExp(v.source, v.flags);
    if (Array.isArray(v)) {
      r = [];
      seen.set(v, r);
      for (var i = 0; i < v.length; i++) r[i] = copy(v[i], seen);
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
  lz("structuredClone", function (v) { return copy(v, new Map()); });

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
  lz("Intl", {
    NumberFormat: function (loc, o) {
      return { format: function (n) { return Number(n).toLocaleString(loc, o); },
               resolvedOptions: function () { return { locale: "en-US" }; } };
    },
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
    getCanonicalLocales: function (l) { return l === undefined ? [] : [].concat(l); },
    supportedValuesOf: function () { return []; }
  });
  ["NumberFormat", "DateTimeFormat", "Collator", "PluralRules", "RelativeTimeFormat", "ListFormat", "Segmenter"]
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
  class FormData {
    constructor(form) {
      def(this, "_l", []);
      if (!form) return;
      var els = form.elements || form.querySelectorAll("input,select,textarea");
      for (var i = 0; i < els.length; i++) {
        var e = els[i], t = (e.type || "").toLowerCase();
        if (!e.name || e.disabled) continue;
        if ((t === "checkbox" || t === "radio") && !e.checked) continue;
        if (t === "submit" || t === "button" || t === "reset" || t === "file") continue;
        this._l.push([e.name, String(e.value === undefined ? "" : e.value)]);
      }
    }
    append(k, v) { this._l.push([String(k), v instanceof Blob ? v : String(v)]); }
    set(k, v) { this.delete(k); this.append(k, v); }
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
    escape: function (s) { return String(s).replace(/[^a-zA-Z0-9_-]/g, function (c) { return "\\" + c; }); }
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
  function postMessage(data) {
    setTimeout(function () {
      var e = new Event("message");
      e.data = data;
      e.origin = location.origin;
      window.dispatchEvent(e);
    }, 0);
  }
  lz("TextEncoder", TextEncoder); lz("TextDecoder", TextDecoder); lz("crypto", crypto);
  lz("Blob", Blob); lz("FormData", FormData); lz("DOMParser", DOMParser);
  lz("customElements", customElements); lz("CSS", CSS);
  lz("Notification", Notification); lz("DOMRect", DOMRect); lz("postMessage", postMessage);
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
  class ReadableStream {
    constructor(source) {
      def(this, "_q", []); def(this, "_wait", []); def(this, "_closed", false);
      def(this, "_error", null); def(this, "_errored", false); def(this, "_disturbed", false); def(this, "_locked", false); def(this, "_source", source || {});
      var stream = this;
      def(this, "_controller", {
        enqueue: function (chunk) {
          if (stream._closed || stream._errored) throw new TypeError("Stream is not readable");
          if (stream._wait.length) stream._wait.shift().resolve({ value: chunk, done: false });
          else stream._q.push(chunk);
        },
        close: function () { stream._closed = true; stream._flush(); },
        error: function (e) { stream._error = e; stream._errored = true; stream._q = []; stream._flush(); },
        get desiredSize() { return 1 - stream._q.length; }
      });
      try { if (this._source.start) this._source.start(this._controller); } catch (e) { this._controller.error(e); }
    }
    get locked() { return this._locked; }
    _flush() {
      while (this._wait.length && (this._errored || (this._closed && !this._q.length))) {
        var w = this._wait.shift(); if (this._errored) w.reject(this._error); else w.resolve({ value: undefined, done: true });
      }
    }
    getReader() {
      if (this._locked) throw new TypeError("Stream is locked");
      this._locked = true; var s = this, released = false, pending = 0;
      return {
        read: function () {
          if (released) return Promise.reject(new TypeError("Reader was released"));
          s._disturbed = true;
          if (s._errored) return Promise.reject(s._error);
          if (s._q.length) return Promise.resolve({ value: s._q.shift(), done: false });
          if (s._closed) return Promise.resolve({ value: undefined, done: true });
          var p = new Promise(function (resolve, reject) { s._wait.push({ resolve: resolve, reject: reject }); });
          if (s._source.pull) Promise.resolve().then(function () { return s._source.pull(s._controller); }).catch(function (e) { s._controller.error(e); });
          pending++; return p.then(function (v) { pending--; return v; }, function (e) { pending--; throw e; });
        },
        cancel: function (reason) { if (released) return Promise.reject(new TypeError("Reader was released")); return s._cancel(reason); },
        releaseLock: function () { if (pending) throw new TypeError("Reads are pending"); released = true; s._locked = false; }
      };
    }
    _cancel(reason) { this._disturbed = true; this._q = []; this._closed = true; this._flush(); return Promise.resolve(this._source.cancel ? this._source.cancel(reason) : undefined); }
    cancel(reason) { return this._locked ? Promise.reject(new TypeError("Stream is locked")) : this._cancel(reason); }
    tee() {
      var reader=this.getReader(), controllers=[], canceled=[false,false], reasons=[];
      function branch(i){return new ReadableStream({start:function(c){controllers[i]=c;},cancel:function(reason){canceled[i]=true;reasons[i]=reason;if(canceled[0]&&canceled[1])return reader.cancel(reasons);}});}
      var a=branch(0),b=branch(1);
      function pump(){reader.read().then(function(v){
        for(var i=0;i<2;i++)if(!canceled[i]){if(v.done)controllers[i].close();else controllers[i].enqueue(v.value instanceof Uint8Array?v.value.slice():v.value);}
        if(v.done)reader.releaseLock();else if(!canceled[0]||!canceled[1])pump();
      },function(e){for(var i=0;i<2;i++)if(!canceled[i])controllers[i].error(e);reader.releaseLock();});}
      pump();return [a,b];
    }
    async *[Symbol.asyncIterator]() {
      var r = this.getReader(); try { while (true) { var v = await r.read(); if (v.done) return; yield v.value; } }
      finally { r.releaseLock(); }
    }
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
        var stream=empty?null:new ReadableStream({start:function(c){controller=c;},cancel:function(){done=true;cleanup();x.abort();}});
        response=new Response(stream,{status:x.status,statusText:x.statusText,headers:parseHeaders(x.getAllResponseHeaders())});
        response.url=opaque?"":x.responseURL||req.url;response.type=opaque?"opaque":"basic";response.redirected=response.url!==new URL(req.url,location.href).href;
        resolved=true;resolve(response);
      }
      x.onreadystatechange=function(){if(x.readyState===2)headers();};
      x.onprogress=function(ev){if(!done&&controller&&ev.data)controller.enqueue(ev.data);};
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

