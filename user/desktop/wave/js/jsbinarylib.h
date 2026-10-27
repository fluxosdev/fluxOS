/* Common typed array operations, using indexed reads/writes into the native views. */
static const char js_binary_lib[] = JS_SOURCE(
(function () {
  var types = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
               Int32Array, Uint32Array, Float32Array, Float64Array];
  function def(o, k, f) { Object.defineProperty(o, k, { value: f, writable: true, configurable: true }); }
  function snapshot(a) { var r = []; for (var i = 0; i < a.length; i++) r.push(a[i]); return r; }
  types.forEach(function (C) {
    var p = C.prototype;
    def(C, "from", function (items, map, self) { return new this(Array.from(items, map && function (v, i) { return map.call(self, v, i); })); });
    def(C, "of", function () { return new this(Array.from(arguments)); });
    def(p, "slice", function (a, b) { return new this.constructor(snapshot(this).slice(a, b)); });
    def(p, "join", function (s) { return snapshot(this).join(s); });
    def(p, "toString", function () { return this.join(","); });
    def(p, "values", function () { var a = this, i = 0; return { next: function () { return i < a.length ? { value: a[i++], done: false } : { value: undefined, done: true }; }, [Symbol.iterator]: function () { return this; } }; });
    def(p, Symbol.iterator, p.values);
    def(p, "keys", function () { var a = this, i = 0; return { next: function () { return i < a.length ? { value: i++, done: false } : { done: true }; }, [Symbol.iterator]: function () { return this; } }; });
    def(p, "entries", function () { var a = this, i = 0; return { next: function () { return i < a.length ? { value: [i, a[i++]], done: false } : { done: true }; }, [Symbol.iterator]: function () { return this; } }; });
    ["at", "includes", "indexOf", "lastIndexOf", "join"].forEach(function (name) {
      def(p, name, function () { var a = snapshot(this); return Array.prototype[name].apply(a, arguments); });
    });
    ["forEach", "map", "filter", "every", "some", "find", "findIndex"].forEach(function (name) {
      def(p, name, function (cb, self) {
        var view = this, a = snapshot(view), result = a[name](function (v, i) { return cb.call(self, view[i], i, view); });
        return name === "map" || name === "filter" ? new view.constructor(result) : result;
      });
    });
    ["reduce", "reduceRight"].forEach(function (name) {
      def(p, name, function (cb, initial) {
        var view = this, a = snapshot(view), f = function (v, x, i) { return cb(v, view[i], i, view); };
        return arguments.length > 1 ? a[name](f, initial) : a[name](f);
      });
    });
    ["reverse", "sort", "fill", "copyWithin"].forEach(function (name) {
      def(p, name, function () {
        var a = snapshot(this);
        if (name === "sort" && !arguments[0]) a.sort(function (x, y) { return x !== x ? 1 : y !== y ? -1 : x - y; });
        else Array.prototype[name].apply(a, arguments);
        this.set(a); return this;
      });
    });
  });
})();
);
