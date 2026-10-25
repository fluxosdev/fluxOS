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
  var M = Math;
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

  /* ---- stubs for what pages look for ---- */
  function Observer() {
    return { observe: function () {}, unobserve: function () {}, disconnect: function () {},
             takeRecords: function () { return []; } };
  }
  ["MutationObserver", "IntersectionObserver", "ResizeObserver", "PerformanceObserver"].forEach(function (n) {
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
      return { format: function (d) { return (d === undefined ? new Date() : new Date(d)).toLocaleDateString(loc, o); },
               resolvedOptions: function () { return { locale: "en-US", timeZone: "UTC" }; } };
    }
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
      var l = this._l[t] || (this._l[t] = []);
      for (var i = 0; i < l.length; i++) if (l[i].f === f) return;
      l.push({ f: f, once: !!(o && typeof o === "object" && o.once) });
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
    get origin() { return this._u.opaque ? "null" : this._u.protocol + "//" + this.host; }
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
    encode(s) {
      s = s === undefined ? "" : String(s);
      var out = [];
      for (var i = 0; i < s.length; i++) out.push(s.charCodeAt(i) & 255);
      return new Uint8Array(out);
    }
  }
  class TextDecoder {
    constructor(label) { def(this, "_l", label || "utf-8"); }
    get encoding() { return String(this._l).toLowerCase(); }
    decode(b) {
      if (b === undefined) return "";
      var a = b.buffer && !Array.isArray(b) ? new Uint8Array(b.buffer) : b, p = [], s = "", i;
      for (i = 0; i < a.length; i++) p.push("%" + (a[i] < 16 ? "0" : "") + (a[i] & 255).toString(16));
      try { return decodeURIComponent(p.join("")); } catch (e) {}
      for (i = 0; i < a.length; i++) s += String.fromCharCode(a[i]);
      return s;
    }
  }
  /* ---- crypto ---- */
  var crypto = {
    getRandomValues: function (a) {
      var m = (a.BYTES_PER_ELEMENT || 1) >= 4 ? 4294967296 : (a.BYTES_PER_ELEMENT === 2 ? 65536 : 256);
      for (var i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * m);
      return a;
    },
    randomUUID: function () {
      return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, function (c) {
        var r = Math.floor(Math.random() * 16);
        return (c === "x" ? r : (r & 3) | 8).toString(16);
      });
    },
    subtle: {}
  };
  /* ---- Blob, FormData ---- */
  class Blob {
    constructor(parts, opts) {
      var s = "";
      (parts || []).forEach(function (p) { s += p instanceof Blob ? p._s : String(p); });
      def(this, "_s", s);
      def(this, "_t", opts && opts.type ? String(opts.type).toLowerCase() : "");
    }
    get size() { return this._s.length; }
    get type() { return this._t; }
    text() { return Promise.resolve(this._s); }
    arrayBuffer() { return Promise.resolve(new TextEncoder().encode(this._s)); }
    slice(a, b, t) { return new Blob([this._s.slice(a, b)], { type: t || "" }); }
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
  /* ---- stubs for APIs a page may probe for ---- */
  var registry = Object.create(null), pending = Object.create(null);
  var customElements = {
    define: function (name, ctor) {
      registry[name] = ctor;
      if (pending[name]) pending[name](ctor);
    },
    get: function (name) { return registry[name]; },
    whenDefined: function (name) {
      if (registry[name]) return Promise.resolve(registry[name]);
      return new Promise(function (res) { pending[name] = res; });
    },
    upgrade: function () {}
  };
  var CSS = {
    supports: function () { return true; },
    escape: function (s) { return String(s).replace(/[^a-zA-Z0-9_-]/g, function (c) { return "\\" + c; }); }
  };
  class WebSocket {
    constructor(url) {
      def(this, "url", String(url));
      this.readyState = 0;
      this.onopen = this.onerror = this.onclose = this.onmessage = null;
      var self = this;
      setTimeout(function () {
        self.readyState = 3;
        var e = new Event("error");
        if (self.onerror) self.onerror(e);
        var c = new Event("close");
        c.code = 1006;
        c.wasClean = false;
        if (self.onclose) self.onclose(c);
      }, 0);
    }
    send() { throw new DOMException("WebSocket is not open", "InvalidStateError"); }
    close() { this.readyState = 3; }
    addEventListener(t, f) { this["on" + t] = f; }
    removeEventListener() {}
  }
  WebSocket.CONNECTING = 0; WebSocket.OPEN = 1; WebSocket.CLOSING = 2; WebSocket.CLOSED = 3;
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
  lz("customElements", customElements); lz("CSS", CSS); lz("WebSocket", WebSocket);
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
  class Response {
    constructor(body, init) {
      init = init || {};
      def(this, "_b", body === undefined || body === null ? "" : String(body));
      def(this, "_used", false);
      this.status = init.status === undefined ? 200 : init.status;
      this.statusText = init.statusText || "";
      this.headers = new Headers(init.headers);
      this.ok = this.status >= 200 && this.status < 300;
      this.url = "";
      this.redirected = false;
      this.type = "basic";
    }
    get bodyUsed() { return this._used; }
    text() {
      var self = this;
      return new Promise(function (resolve, reject) {
        if (self._used) reject(new TypeError("Body has already been read"));
        else { self._used = true; resolve(self._b); }
      });
    }
    json() { return this.text().then(function (t) { return JSON.parse(t); }); }
    clone() {
      var r = new Response(this._b, { status: this.status, statusText: this.statusText, headers: this.headers });
      r.url = this.url;
      return r;
    }
    static json(data, init) {
      var r = new Response(JSON.stringify(data), init);
      if (!r.headers.has("content-type")) r.headers.set("content-type", "application/json");
      return r;
    }
    static error() { var r = new Response(null, { status: 0 }); r.type = "error"; return r; }
  }
  class Request {
    constructor(input, init) {
      init = init || {};
      var from = input instanceof Request ? input : null;
      this.url = from ? from.url : String(input);
      this.method = String(init.method || (from && from.method) || "GET").toUpperCase();
      this.headers = new Headers(init.headers || (from && from.headers));
      this.body = init.body !== undefined ? init.body : from ? from.body : null;
      this.signal = init.signal || (from && from.signal) || null;
    }
    clone() { return new Request(this); }
  }
  function parseHeaders(text) {
    var h = new Headers(), lines = String(text).split("\n");
    for (var i = 0; i < lines.length; i++) {
      var c = lines[i].indexOf(":");
      if (c > 0) h.append(lines[i].slice(0, c).trim(), lines[i].slice(c + 1).trim());
    }
    return h;
  }
  lz("Headers", Headers);
  lz("Response", Response);
  lz("Request", Request);
  lz("fetch", function (input, init) {
    var req = new Request(input, init);
    return new Promise(function (resolve, reject) {
      var signal = req.signal;
      if (signal && signal.aborted) { reject(signal.reason); return; }
      var x = new XMLHttpRequest(), done = false;
      x.open(req.method, req.url);
      req.headers.forEach(function (v, k) { x.setRequestHeader(k, v); });
      if (signal) signal.addEventListener("abort", function () {
        if (done) return;
        done = true;
        x.abort();
        reject(signal.reason);
      });
      x.onload = function () {
        if (done) return;
        done = true;
        var r = new Response(x.responseText, { status: x.status, statusText: x.statusText,
                                                 headers: parseHeaders(x.getAllResponseHeaders()) });
        r.url = x.responseURL || req.url;
        resolve(r);
      };
      x.onerror = function () {
        if (done) return;
        done = true;
        reject(new TypeError("Failed to fetch"));
      };
      x.send(req.body === undefined || req.body === null ? null : String(req.body));
    });
  });

})(globalThis);
);

