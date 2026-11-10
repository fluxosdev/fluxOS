/*
 * The WebAssembly namespace: Module, Instance, Memory, Table, Global, the
 * three error types and the promise-returning functions, over the
 * primitives in jswasm.c. A lazily loaded chunk of the prelude (jslib2.c):
 * it runs the first time a page names WebAssembly.
 *
 * Each object keeps the engine's handle under a private symbol, and the
 * handle keeps the object, so a memory, table or global exported twice, or
 * imported back, is the same object each time.
 *
 * The same rules as jsprelude.h: double quotes only, no backticks, every
 * statement ends in a semicolon.
 */
static const char js_chunk_wasm[] = JS_SOURCE(
(function (g) {
  var lz = g["%lazy"], H = Symbol("WebAssembly handle");
  var kindOf = g["%wasmKind"], compileBytes = g["%wasmCompile"], validateBytes = g["%wasmValidate"];
  var importsOf = g["%wasmImports"], exportsOf = g["%wasmExports"], customOf = g["%wasmCustom"];
  var instantiateModule = g["%wasmInstantiate"], newMemory = g["%wasmMemory"], memBuffer = g["%wasmMemBuffer"];
  var memGrow = g["%wasmMemGrow"], newTable = g["%wasmTable"], tableOp = g["%wasmTableOp"];
  var newGlobal = g["%wasmGlobal"], globalOp = g["%wasmGlobalOp"];
  function def(o, k, v) {
    Object.defineProperty(o, k, { value: v, writable: true, configurable: true, enumerable: false });
  }
  function tag(C, name) {
    Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
  }
  function bind(o, h) {
    Object.defineProperty(o, H, { value: h });
    Object.defineProperty(h, H, { value: o });
  }
  function unwrap(o, kind, what) {
    var h = o !== null && (typeof o === "object" || typeof o === "function") ? o[H] : undefined;
    if (h === undefined || kindOf(h) !== kind) throw new TypeError("WebAssembly." + what + " expected");
    return h;
  }
  function wrapper(h, C) {
    var w = h[H];
    if (w === undefined) {
      w = Object.create(C.prototype);
      bind(w, h);
    }
    return w;
  }
  function descriptor(desc, what) {
    if (desc === null || typeof desc !== "object") throw new TypeError("WebAssembly." + what + "(): the argument must be a descriptor object");
    var initial = desc.initial, minimum = desc.minimum;
    if (initial === undefined) initial = minimum;
    else if (minimum !== undefined) throw new TypeError("WebAssembly." + what + "(): give initial or minimum, not both");
    if (initial === undefined) throw new TypeError("WebAssembly." + what + "(): the descriptor needs an initial size");
    return initial;
  }

  class CompileError extends Error {
    constructor(message) { super(message); }
  }
  class LinkError extends Error {
    constructor(message) { super(message); }
  }
  class RuntimeError extends Error {
    constructor(message) { super(message); }
  }
  def(CompileError.prototype, "name", "CompileError");
  def(LinkError.prototype, "name", "LinkError");
  def(RuntimeError.prototype, "name", "RuntimeError");
  g["%wasmSetup"](CompileError, LinkError, RuntimeError);

  class Module {
    constructor(bytes) {
      bind(this, compileBytes(bytes));
    }
    static imports(m) { return importsOf(unwrap(m, "module", "Module")); }
    static exports(m) { return exportsOf(unwrap(m, "module", "Module")); }
    static customSections(m, name) {
      var h = unwrap(m, "module", "Module");
      if (name === undefined) throw new TypeError("WebAssembly.Module.customSections(): a section name is required");
      return customOf(h, String(name));
    }
  }

  function resolveImports(h, importObject) {
    var list = importsOf(h);
    if (importObject !== undefined && (importObject === null || (typeof importObject !== "object" && typeof importObject !== "function")))
      throw new TypeError("WebAssembly.Instance(): the imports argument must be an object");
    if (list.length && importObject === undefined)
      throw new TypeError("WebAssembly.Instance(): the module has imports, but no imports object was given");
    return list.map(function (d) {
      var ns = importObject[d.module];
      if (ns === null || (typeof ns !== "object" && typeof ns !== "function"))
        throw new TypeError("WebAssembly.Instance(): import module \"" + d.module + "\" must be an object");
      var v = ns[d.name];
      if (d.kind === "function") return v;
      var hv = v !== null && (typeof v === "object" || typeof v === "function") ? v[H] : undefined;
      if (hv !== undefined && kindOf(hv) === d.kind) return hv;
      if (d.kind === "global") return v;
      throw new LinkError("WebAssembly.Instance(): import " + d.module + "." + d.name + " must be a WebAssembly." +
                          (d.kind === "memory" ? "Memory" : "Table"));
    });
  }

  class Instance {
    constructor(module, importObject) {
      var h = unwrap(module, "module", "Module");
      var list = instantiateModule(h, resolveImports(h, importObject)), ex = Object.create(null);
      list.forEach(function (e) {
        var v = e[1] === "function" ? e[2] : wrapper(e[2], e[1] === "memory" ? Memory : e[1] === "table" ? Table : Global);
        Object.defineProperty(ex, e[0], { value: v, enumerable: true, writable: false, configurable: false });
      });
      Object.preventExtensions(ex);
      Object.defineProperty(this, "exports", { value: ex, enumerable: true, configurable: true });
    }
  }

  class Memory {
    constructor(desc) {
      var initial = descriptor(desc, "Memory");
      if (desc.shared) throw new TypeError("WebAssembly.Memory(): shared memory is not supported");
      bind(this, newMemory(initial, desc.maximum));
    }
    get buffer() { return memBuffer(unwrap(this, "memory", "Memory")); }
    grow(delta) { return memGrow(unwrap(this, "memory", "Memory"), delta); }
  }

  class Table {
    constructor(desc, value) {
      var initial = descriptor(desc, "Table"), el = String(desc.element);
      if (el === "anyfunc") el = "funcref";
      if (el !== "funcref" && el !== "externref")
        throw new TypeError("WebAssembly.Table(): the element type must be \"funcref\" or \"externref\"");
      bind(this, newTable(el, initial, desc.maximum, value, arguments.length > 1));
    }
    get length() { return tableOp(unwrap(this, "table", "Table"), 0); }
    get(index) { return tableOp(unwrap(this, "table", "Table"), 1, index); }
    set(index, value) { tableOp(unwrap(this, "table", "Table"), 2, index, value, arguments.length > 1); }
    grow(delta, value) { return tableOp(unwrap(this, "table", "Table"), 3, delta, value, arguments.length > 1); }
  }

  class Global {
    constructor(desc, value) {
      if (desc === null || typeof desc !== "object") throw new TypeError("WebAssembly.Global(): the argument must be a descriptor object");
      var t = String(desc.value);
      if (t === "anyfunc") t = "funcref";
      if (["i32", "i64", "f32", "f64", "funcref", "externref"].indexOf(t) < 0)
        throw new TypeError("WebAssembly.Global(): unsupported value type " + t);
      bind(this, newGlobal(t, !!desc.mutable, value));
    }
    get value() { return globalOp(unwrap(this, "global", "Global"), 0); }
    set value(v) { globalOp(unwrap(this, "global", "Global"), 1, v); }
    valueOf() { return globalOp(unwrap(this, "global", "Global"), 0); }
  }

  tag(Module, "WebAssembly.Module");
  tag(Instance, "WebAssembly.Instance");
  tag(Memory, "WebAssembly.Memory");
  tag(Table, "WebAssembly.Table");
  tag(Global, "WebAssembly.Global");

  function compile(bytes) {
    return new Promise(function (ok) { ok(new Module(bytes)); });
  }
  function instantiate(source, importObject) {
    if (source instanceof Module)
      return new Promise(function (ok) { ok(new Instance(source, importObject)); });
    return compile(source).then(function (m) { return { module: m, instance: new Instance(m, importObject) }; });
  }
  function fetched(source) {
    return Promise.resolve(source).then(function (r) {
      if (r === null || typeof r !== "object" || typeof r.arrayBuffer !== "function")
        throw new TypeError("WebAssembly: a Response, or a promise of one, is required");
      if (r.ok === false) throw new TypeError("WebAssembly: HTTP status " + r.status + " for " + r.url);
      return r.arrayBuffer();
    });
  }

  var WA = {};
  def(WA, "Module", Module);
  def(WA, "Instance", Instance);
  def(WA, "Memory", Memory);
  def(WA, "Table", Table);
  def(WA, "Global", Global);
  def(WA, "CompileError", CompileError);
  def(WA, "LinkError", LinkError);
  def(WA, "RuntimeError", RuntimeError);
  def(WA, "validate", function validate(bytes) { return validateBytes(bytes); });
  def(WA, "compile", compile);
  def(WA, "instantiate", instantiate);
  def(WA, "compileStreaming", function compileStreaming(source) { return fetched(source).then(compile); });
  def(WA, "instantiateStreaming", function instantiateStreaming(source, importObject) {
    return fetched(source).then(function (bytes) { return instantiate(bytes, importObject); });
  });
  Object.defineProperty(WA, Symbol.toStringTag, { value: "WebAssembly", configurable: true });
  lz("WebAssembly", WA);
})(globalThis);
);
