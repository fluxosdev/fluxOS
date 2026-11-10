// WebAssembly: modules are assembled here from bytes (there is no text format compiler).
var W = WebAssembly;
var I32 = 0x7f, I64 = 0x7e, F32 = 0x7d, F64 = 0x7c, FUNCREF = 0x70, EXTERNREF = 0x6f;
function u(n) { var o = []; do { var b = n & 0x7f; n = Math.floor(n / 128); if (n) b |= 0x80; o.push(b); } while (n); return o; }
function flat(a) { return [].concat.apply([], a); }
function name(t) { var b = Array.from(new TextEncoder().encode(t)); return u(b.length).concat(b); }
function vec(items) { return u(items.length).concat(flat(items)); }
function sec(id, body) { return [id].concat(u(body.length), body); }
function f64(x) { return Array.from(new Uint8Array(new Float64Array([x]).buffer)); }
function module(m) {
  var out = [0, 0x61, 0x73, 0x6d, 1, 0, 0, 0];
  var types = function (l) { return vec(l.map(function (x) { return [x]; })); };
  if (m.types) out = out.concat(sec(1, vec(m.types.map(function (t) { return [0x60].concat(types(t[0]), types(t[1])); }))));
  if (m.imports) out = out.concat(sec(2, vec(m.imports.map(function (i) { return name(i[0]).concat(name(i[1]), i[2]); }))));
  if (m.funcs) out = out.concat(sec(3, vec(m.funcs.map(function (f) { return u(f.type); }))));
  if (m.tables) out = out.concat(sec(4, vec(m.tables)));
  if (m.memory) out = out.concat(sec(5, vec([m.memory])));
  if (m.globals) out = out.concat(sec(6, vec(m.globals)));
  if (m.exports) out = out.concat(sec(7, vec(m.exports.map(function (e) { return name(e[0]).concat([e[1]], u(e[2])); }))));
  if (m.start !== undefined) out = out.concat(sec(8, u(m.start)));
  if (m.elems) out = out.concat(sec(9, vec(m.elems)));
  if (m.datacount !== undefined) out = out.concat(sec(12, u(m.datacount)));
  if (m.funcs) out = out.concat(sec(10, vec(m.funcs.map(function (f) {
    var body = vec((f.locals || []).map(function (l) { return u(l[0]).concat([l[1]]); })).concat(f.code, [0x0b]);
    return u(body.length).concat(body);
  }))));
  if (m.datas) out = out.concat(sec(11, vec(m.datas)));
  (m.customs || []).forEach(function (c) { out = out.concat(sec(0, name(c[0]).concat(c[1]))); });
  return new Uint8Array(out);
}
function inst(m, imports) { return new W.Instance(new W.Module(module(m)), imports).exports; }
function trap(f) { try { f(); return 'no trap'; } catch (e) { return [e.name, e.message]; } }

var addMod = { types: [[[I32, I32], [I32]]], funcs: [{ type: 0, code: [0x20, 0, 0x20, 1, 0x6a] }], exports: [['add', 0, 0]] };

test('wasm namespace', function () {
  return [typeof WebAssembly, Object.prototype.toString.call(WebAssembly), typeof W.Module, typeof W.instantiateStreaming,
          new W.RuntimeError('x') instanceof Error, new W.LinkError('y').name];
}, ['object', '[object WebAssembly]', 'function', 'function', true, 'LinkError']);

test('wasm add and argument conversion', function () {
  var e = inst(addMod);
  return [e.add(2, 3), e.add(0x7fffffff, 1), e.add('4', 1.9), e.add(), e.add.length, e.add.name, typeof e.add];
}, [5, -2147483648, 5, 0, 2, '0', 'function']);

test('wasm i64 recursion and BigInt', function () {
  var e = inst({ types: [[[I64], [I64]]], exports: [['fac', 0, 0]],
    funcs: [{ type: 0, code: [0x20, 0, 0x50, 0x04, I64, 0x42, 1, 0x05, 0x20, 0, 0x20, 0, 0x42, 1, 0x7d, 0x10, 0, 0x7e, 0x0b] }] });
  var number = false;
  try { e.fac(3); } catch (x) { number = x instanceof TypeError; }
  return [String(e.fac(20n)), String(e.fac(25n)), typeof e.fac(1n), number];
}, ['2432902008176640000', '7034535277573963776', 'bigint', true]);

test('wasm loops and locals', function () {
  var e = inst({ types: [[[I32], [I32]]], exports: [['sum', 0, 0]], funcs: [{ type: 0, locals: [[2, I32]],
    code: [0x02, 0x40, 0x03, 0x40, 0x20, 1, 0x20, 0, 0x4e, 0x0d, 1, 0x20, 1, 0x41, 1, 0x6a, 0x22, 1, 0x20, 2, 0x6a, 0x21, 2,
           0x0c, 0, 0x0b, 0x0b, 0x20, 2] }] });
  return [e.sum(1000), e.sum(100000), e.sum(0)];
}, [500500, 705082704, 0]);

test('wasm sieve over memory', function () {
  var e = inst({ types: [[[I32], [I32]]], memory: [0x00, 1], exports: [['sieve', 0, 0]], funcs: [{ type: 0, locals: [[3, I32]],
    code: [0x41, 2, 0x21, 1, 0x02, 0x40, 0x03, 0x40, 0x20, 1, 0x20, 0, 0x4f, 0x0d, 1, 0x20, 1, 0x2d, 0, 0, 0x45,
           0x04, 0x40, 0x20, 3, 0x41, 1, 0x6a, 0x21, 3, 0x20, 1, 0x20, 1, 0x6c, 0x21, 2,
           0x02, 0x40, 0x03, 0x40, 0x20, 2, 0x20, 0, 0x4f, 0x0d, 1, 0x20, 2, 0x41, 1, 0x3a, 0, 0,
           0x20, 2, 0x20, 1, 0x6a, 0x21, 2, 0x0c, 0, 0x0b, 0x0b, 0x0b,
           0x20, 1, 0x41, 1, 0x6a, 0x21, 1, 0x0c, 0, 0x0b, 0x0b, 0x20, 3] }] });
  return [e.sieve(10000), e.sieve(100)];
}, [1229, 25]);

var memMod = { types: [[[I32], [I32]], [[I32, I32], []], [[], [I32]]], memory: [0x01, 1, 4],
  funcs: [{ type: 0, code: [0x20, 0, 0x2d, 0, 0] }, { type: 1, code: [0x20, 0, 0x20, 1, 0x36, 2, 0] },
          { type: 0, code: [0x20, 0, 0x40, 0] }, { type: 2, code: [0x3f, 0] }],
  exports: [['load8', 0, 0], ['store32', 0, 1], ['grow', 0, 2], ['size', 0, 3], ['mem', 2, 0]],
  datas: [[0x00, 0x41, 8, 0x0b].concat(name('hello'))] };

test('wasm memory, data and bounds', function () {
  var e = inst(memMod);
  e.store32(100, 0x01020304);
  return [String.fromCharCode.apply(null, new Uint8Array(e.mem.buffer, 8, 5)), new DataView(e.mem.buffer).getUint32(100, true),
          e.load8(100), e.mem.buffer.byteLength, e.mem instanceof W.Memory, e.mem.buffer === e.mem.buffer,
          trap(function () { e.load8(65536); }), trap(function () { e.store32(65534, 1); })];
}, ['hello', 16909060, 4, 65536, true, true, ['RuntimeError', 'out of bounds memory access'],
    ['RuntimeError', 'out of bounds memory access']]);

test('wasm memory growth', function () {
  var e = inst(memMod), b = e.mem.buffer;
  var r = [e.grow(1), b.byteLength, e.mem.buffer.byteLength, e.size(), e.mem.grow(1), e.grow(5), e.load8(8)];
  try { e.mem.grow(2); } catch (x) { r.push(x.name); }
  var m = new W.Memory({ initial: 1, maximum: 2 }), mb = m.buffer;
  new Uint8Array(mb)[7] = 9;
  r.push(m.grow(1), mb.byteLength, new Uint8Array(m.buffer)[7], m.buffer.byteLength);
  return r;
}, [1, 0, 131072, 2, 2, -1, 104, 'RangeError', 1, 0, 9, 131072]);

test('wasm memory buffer cannot be transferred', function () {
  var m = new W.Memory({ initial: 1 });
  try { structuredClone(m.buffer, { transfer: [m.buffer] }); return 'transferred'; } catch (x) { return [x.name, m.buffer.byteLength]; }
}, ['TypeError', 65536]);

test('wasm imported memory', function () {
  var m = new W.Memory({ initial: 1 });
  new Uint8Array(m.buffer)[3] = 77;
  var e = inst({ types: [[[I32], [I32]]], imports: [['env', 'mem', [0x02, 0x00, 1]]], funcs: [{ type: 0, code: [0x20, 0, 0x2d, 0, 0] }],
    exports: [['load8', 0, 0], ['mem', 2, 0]] }, { env: { mem: m } });
  return [e.load8(3), e.mem === m];
}, [77, true]);

var impMod = { types: [[[I32], []], [[I32], [I32]], [[], [I32]]],
  imports: [['env', 'log', [0x00, 0]], ['env', 'twice', [0x00, 1]]],
  funcs: [{ type: 2, code: [0x41, 42, 0x10, 0, 0x41, 21, 0x10, 1] }], exports: [['run', 0, 2]] };

test('wasm imported functions', function () {
  var log = [], e = inst(impMod, { env: { log: function (x) { log.push(x, this === undefined || this === globalThis); }, twice: function (x) { return String(x * 2); } } });
  return [e.run(), log];
}, [42, [42, true]]);

test('wasm link errors', function () {
  var m = new W.Module(module(impMod));
  return [trap(function () { new W.Instance(m); }), trap(function () { new W.Instance(m, {}); })[0],
          trap(function () { new W.Instance(m, { env: { log: 1, twice: Math.abs } }); })[0],
          trap(function () { new W.Instance(m, { env: { log: Math.abs } }); })[0]];
}, [['TypeError', 'WebAssembly.Instance(): the module has imports, but no imports object was given'], 'TypeError', 'LinkError', 'LinkError']);

test('wasm re-entry and exceptions through frames', function () {
  var boom = new Error('boom'), e;
  e = inst({ types: [[[], [I32]], [[I32], [I32]]], imports: [['env', 'cb', [0x00, 0]], ['env', 'fail', [0x00, 0]]],
    funcs: [{ type: 1, code: [0x20, 0, 0x41, 2, 0x6c] }, { type: 0, code: [0x10, 0, 0x41, 1, 0x6a] }, { type: 0, code: [0x10, 1] }],
    exports: [['dbl', 0, 2], ['outer', 0, 3], ['failing', 0, 4]] },
    { env: { cb: function () { return e.dbl(e.dbl(5)) + 100; }, fail: function () { throw boom; } } });
  var caught;
  try { e.failing(); } catch (x) { caught = x === boom; }
  return [e.outer(), caught, e.dbl(4)];
}, [121, true, 8]);

test('wasm globals', function () {
  var shared = new W.Global({ value: 'i32', mutable: true }, 5);
  var e = inst({ types: [[[], [I32]]], imports: [['env', 'base', [0x03, I32, 0]], ['env', 'shared', [0x03, I32, 1]]],
    globals: [[I32, 1, 0x23, 0, 0x0b]],
    funcs: [{ type: 0, code: [0x23, 2, 0x41, 1, 0x6a, 0x24, 2, 0x23, 2] }, { type: 0, code: [0x23, 1, 0x41, 10, 0x6c, 0x24, 1, 0x23, 1] }],
    exports: [['bump', 0, 0], ['scale', 0, 1], ['counter', 3, 2], ['shared', 3, 1]] }, { env: { base: 10, shared: shared } });
  var r = [e.bump(), e.counter.value];
  e.counter.value = 100;
  r.push(e.bump(), e.scale(), shared.value, e.shared === shared, e.counter instanceof W.Global, e.counter.valueOf());
  var big = new W.Global({ value: 'i64' }, 5n), f = new W.Global({ value: 'f32', mutable: true }, 0.1);
  r.push(String(big.value), trap(function () { big.value = 6n; })[0], f.value === Math.fround(0.1), new W.Global({ value: 'externref' }).value);
  return r;
}, [11, 11, 101, 50, 50, true, true, 101, '5', 'TypeError', true, undefined]);

test('wasm tables and indirect calls', function () {
  var e = inst({ types: [[[], [I32]], [[I32], [I32]], [[I32], []]], tables: [[FUNCREF, 0x00, 4]],
    funcs: [{ type: 0, code: [0x41, 10] }, { type: 0, code: [0x41, 20] }, { type: 1, code: [0x20, 0, 0x11, 0, 0] }, { type: 2, code: [] }],
    elems: [[0x00, 0x41, 0, 0x0b].concat(vec([[0], [1], [3]]))],
    exports: [['dispatch', 0, 2], ['tbl', 1, 0], ['f10', 0, 0]] });
  var r = [e.dispatch(0), e.dispatch(1), trap(function () { e.dispatch(2); })[1], trap(function () { e.dispatch(3); })[1],
           trap(function () { e.dispatch(4); })[1], e.tbl.length, e.tbl.get(0) === e.f10, e.tbl.get(3)];
  e.tbl.set(3, e.f10);
  r.push(e.dispatch(3), e.tbl.grow(2), e.tbl.length, trap(function () { e.tbl.set(0, function () {}); })[0], e.tbl instanceof W.Table);
  return r;
}, [10, 20, 'indirect call signature mismatch', 'uninitialized element', 'undefined element: table index out of bounds', 4, true, null,
    10, 4, 6, 'TypeError', true]);

test('wasm br_table', function () {
  var e = inst({ types: [[[I32], [I32]]], exports: [['sw', 0, 0]], funcs: [{ type: 0,
    code: [0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x20, 0, 0x0e, 2, 0, 1, 2, 0x0b, 0x41, 10, 0x0f, 0x0b, 0x41, 20, 0x0f, 0x0b, 0x41, 30] }] });
  return [e.sw(0), e.sw(1), e.sw(2), e.sw(99), e.sw(-1)];
}, [10, 20, 30, 30, 30]);

test('wasm multiple values', function () {
  var e = inst({ types: [[[], [I32, I64]], [[I32, I32], [I32, I32]], [[I32, I32], [I32]], [[], [I32]]],
    imports: [['env', 'two', [0x00, 1]]],
    funcs: [{ type: 0, code: [0x41, 1, 0x42, 2] }, { type: 1, code: [0x20, 1, 0x20, 0] },
            { type: 2, code: [0x20, 0, 0x20, 1, 0x02, 2, 0x6b, 0x0b] }, { type: 3, code: [0x41, 1, 0x41, 2, 0x10, 0, 0x6b] }],
    exports: [['pair', 0, 1], ['swap', 0, 2], ['sub', 0, 3], ['host', 0, 4]] }, { env: { two: function (a, b) { return [b * 10, a]; } } });
  return [e.pair().map(String), e.swap(3, 4), e.sub(10, 3), e.host()];
}, [['1', '2'], [4, 3], 7, 19]);

test('wasm floating point', function () {
  var fn = function (t, code) { return { type: t, code: [0x20, 0].concat(code) }; };
  var e = inst({ types: [[[F64], [F64]], [[F32, F32], [F32]], [[F64, F64], [F64]], [[F64], [I32]], [[F32], [F32]], [[], [F64]]],
    funcs: [fn(0, [0x9f]), fn(1, [0x20, 1, 0x92]), fn(2, [0x20, 1, 0xa4]), fn(2, [0x20, 1, 0xa5]), fn(3, [0xaa]), fn(4, [0x90]),
            fn(3, [0xfc, 2]), { type: 5, code: [0x44].concat(f64(1.5)) }, fn(0, [0xbd, 0xbf])],
    exports: [['sqrt', 0, 0], ['addf', 0, 1], ['min', 0, 2], ['max', 0, 3], ['trunc', 0, 4], ['nearest', 0, 5], ['sat', 0, 6],
              ['k', 0, 7], ['bits', 0, 8]] });
  return [e.sqrt(2) === Math.SQRT2, e.addf(0.1, 0.2) === Math.fround(Math.fround(0.1) + Math.fround(0.2)), Object.is(e.min(-0, 0), -0),
          isNaN(e.max(NaN, 1)), e.trunc(-3.9), trap(function () { e.trunc(3e9); })[1], trap(function () { e.trunc(NaN); })[1],
          e.nearest(2.5), e.nearest(3.5), Object.is(e.nearest(-0.5), -0), e.sat(3e9), e.sat(NaN), e.sat(-1e20), e.k(), e.bits(-7.25)];
}, [true, true, true, true, -3, 'integer overflow', 'invalid conversion to integer', 2, 4, true, 2147483647, 0, -2147483648, 1.5, -7.25]);

test('wasm integer traps and sign extension', function () {
  var fn = function (code) { return { type: 0, code: [0x20, 0, 0x20, 1].concat(code) }; };
  var e = inst({ types: [[[I32, I32], [I32]], [[I32], [I32]], [[I64], [I64]], [[], []]],
    funcs: [fn([0x6d]), fn([0x6f]), fn([0x70]), { type: 1, code: [0x20, 0, 0xc0] }, { type: 1, code: [0x20, 0, 0xc1] },
            { type: 2, code: [0x20, 0, 0xc4] }, { type: 3, code: [0x00] }, fn([0x77]), fn([0x75]), { type: 1, code: [0x20, 0, 0x67] }],
    exports: [['div', 0, 0], ['rem', 0, 1], ['remu', 0, 2], ['ext8', 0, 3], ['ext16', 0, 4], ['ext32', 0, 5], ['unr', 0, 6],
              ['rotl', 0, 7], ['shrs', 0, 8], ['clz', 0, 9]] });
  return [e.div(7, -2), trap(function () { e.div(1, 0); })[1], trap(function () { e.div(-2147483648, -1); })[1], e.rem(-2147483648, -1),
          e.rem(-7, 2), e.remu(-1, 10), e.ext8(0x80), e.ext16(0x18000), String(e.ext32(0x80000000n)), trap(e.unr),
          e.rotl(0x80000001, 1), e.shrs(-16, 2), e.clz(1), e.clz(0)];
}, [-3, 'integer divide by zero', 'integer overflow', 0, -1, 5, -128, -32768, '-2147483648', ['RuntimeError', 'unreachable executed'],
    3, -4, 31, 32]);

test('wasm bulk memory', function () {
  var three = [[I32, I32, I32], []];
  var e = inst({ types: [three, [[], []]], memory: [0x00, 1], datacount: 1, datas: [[0x01].concat(name('xyz'))],
    funcs: [{ type: 0, code: [0x20, 0, 0x20, 1, 0x20, 2, 0xfc, 8, 0, 0] }, { type: 1, code: [0xfc, 9, 0] },
            { type: 0, code: [0x20, 0, 0x20, 1, 0x20, 2, 0xfc, 11, 0] }, { type: 0, code: [0x20, 0, 0x20, 1, 0x20, 2, 0xfc, 10, 0, 0] }],
    exports: [['init', 0, 0], ['drop', 0, 1], ['fill', 0, 2], ['copy', 0, 3], ['mem', 2, 0]] });
  e.init(10, 0, 3); e.copy(20, 10, 3); e.fill(30, 65, 2);
  var b = new Uint8Array(e.mem.buffer), text = function (at, n) { return String.fromCharCode.apply(null, b.subarray(at, at + n)); };
  var r = [text(10, 3), text(20, 3), text(30, 2)];
  e.drop();
  r.push(trap(function () { e.init(0, 0, 1); })[1], trap(function () { e.init(0, 0, 0); }), trap(function () { e.fill(65535, 0, 2); })[1]);
  return r;
}, ['xyz', 'xyz', 'AA', 'out of bounds memory access', 'no trap', 'out of bounds memory access']);

test('wasm references', function () {
  var e = inst({ types: [[[EXTERNREF], [EXTERNREF]], [[EXTERNREF], [I32]], [[], [FUNCREF]]],
    funcs: [{ type: 0, code: [0x20, 0] }, { type: 1, code: [0x20, 0, 0xd1] }, { type: 2, code: [0xd2, 0] }],
    exports: [['id', 0, 0], ['isnull', 0, 1], ['getf', 0, 2]] });
  var o = {}, t = new W.Table({ element: 'externref', initial: 2 });
  t.set(1, o);
  return [e.id(o) === o, e.id(null), e.id(undefined), e.id(5), e.isnull(null), e.isnull(0), e.getf() === e.id, t.get(0), t.get(1) === o];
}, [true, null, undefined, 5, 1, 0, true, undefined, true]);

test('wasm start function and re-exports', function () {
  var hits = 0, a = inst(addMod);
  var e = inst({ types: [[[], []], [[I32, I32], [I32]]], imports: [['env', 'hit', [0x00, 0]], ['m', 'add', [0x00, 1]]],
    funcs: [{ type: 0, code: [0x10, 0] }], start: 2, exports: [['add', 0, 1]] }, { env: { hit: function () { hits++; } }, m: { add: a.add } });
  return [hits, e.add === a.add, e.add(1, 2)];
}, [1, true, 3]);

test('wasm module descriptions', function () {
  var m = new W.Module(module({ types: addMod.types, imports: [['env', 'g', [0x03, I32, 0]]], funcs: addMod.funcs,
    exports: [['add', 0, 0], ['memory', 2, 0]], memory: [0x00, 0], customs: [['meta', [1, 2, 3]], ['other', []]] }));
  var c = W.Module.customSections(m, 'meta');
  return [W.Module.imports(m), W.Module.exports(m), c.length, c[0] instanceof ArrayBuffer, Array.from(new Uint8Array(c[0])),
          W.Module.customSections(m, 'none').length, m instanceof W.Module, Object.prototype.toString.call(m)];
}, [[{ module: 'env', name: 'g', kind: 'global' }], [{ name: 'add', kind: 'function' }, { name: 'memory', kind: 'memory' }], 1, true,
    [1, 2, 3], 0, true, '[object WebAssembly.Module]']);

test('wasm exports object', function () {
  var x = new W.Instance(new W.Module(module(addMod))).exports;
  return [Object.getPrototypeOf(x), Object.keys(x), typeof x.add];
}, [null, ['add'], 'function']);

test('wasm validation and compile errors', function () {
  var bad = module({ types: [[[], [I32]]], funcs: [{ type: 0, code: [0x42, 1] }] });
  var err;
  try { new W.Module(bad); } catch (x) { err = [x instanceof W.CompileError, x instanceof Error, x.name, x.message]; }
  return [W.validate(module(addMod)), W.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0])), W.validate(new Uint8Array([1, 2, 3])),
          W.validate(bad), W.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 1])), err, trap(function () { W.validate('x'); })[0],
          W.validate(module({ types: [[[], []]], funcs: [{ type: 0, code: [0xd2, 0, 0x1a] }] }))];
}, [true, true, false, false, false, [true, true, 'CompileError', 'type mismatch: expected 0x7f, got 0x7e'], 'TypeError', false]);

test('wasm call stack overflow', function () {
  var e = inst({ types: [[[], []]], funcs: [{ type: 0, code: [0x10, 0] }], exports: [['r', 0, 0]] });
  return [trap(e.r)[0], trap(e.r)[0]];
}, ['RangeError', 'RangeError']);

var asyncResults = { inst: [], bad: [], stream: [] };
W.instantiate(module(addMod)).then(function (r) {
  asyncResults.inst.push(r.module instanceof W.Module, r.instance instanceof W.Instance, r.instance.exports.add(1, 2));
  return W.instantiate(r.module);
}).then(function (i) { asyncResults.inst.push(i instanceof W.Instance); });
W.compile(new Uint8Array([1, 2])).catch(function (e) { asyncResults.bad.push(e instanceof W.CompileError); });
W.instantiate(new W.Module(module(impMod)), {}).catch(function (e) { asyncResults.bad.push(e.name); });
W.instantiateStreaming(Promise.resolve(new Response(module(addMod)))).then(function (r) {
  asyncResults.stream.push(r.instance.exports.add(20, 22));
  return W.compileStreaming(new Response(module(addMod)));
}).then(function (m) { asyncResults.stream.push(m instanceof W.Module); });
setTimeout(function () {
  test('wasm promise API', function () { return asyncResults; }, { inst: [true, true, 3, true], bad: [true, 'TypeError'], stream: [42, true] });
}, 0);
