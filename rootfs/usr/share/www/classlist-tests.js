/* Shared by the browser test page and the native engine regression runner. */
group("DOM class tokens");
test("classList uses unique tokens and HTML whitespace", function () {
  var e = document.createElement("div");
  e.className = " a\tb\nc\rd\fe a b ";
  return [e.classList.length, e.classList.item(1), e.classList[4], e.classList[5]];
}, [5, "b", "e", undefined]);
test("vertical tab is part of a class token", function () {
  var e = document.createElement("div"); e.className = "a\vb c";
  return [e.classList.length, e.classList.contains("a\vb"), e.classList.contains("a")];
}, [2, true, false]);
test("classList mutations normalize and deduplicate", function () {
  var e = document.createElement("div"); e.className = " a\tb a ";
  e.classList.add("b", "c", "c"); e.classList.remove("a");
  return [e.className, e.classList.length];
}, ["b c", 2]);
test("classList replace preserves position", function () {
  var e = document.createElement("div"); e.className = "a b c";
  return [e.classList.replace("b", "x"), e.className];
}, [true, "a x c"]);
test("classList replace merges a later duplicate", function () {
  var e = document.createElement("div"); e.className = "a b c";
  e.classList.replace("a", "c"); return e.className;
}, "c b");
test("classList replace merges an earlier duplicate", function () {
  var e = document.createElement("div"); e.className = "a b c";
  e.classList.replace("c", "a"); return e.className;
}, "a b");
test("classList replace with itself", function () {
  var e = document.createElement("div"); e.className = "a b c";
  return [e.classList.replace("b", "b"), e.className];
}, [true, "a b c"]);
test("classList missing replacement leaves spelling alone", function () {
  var e = document.createElement("div"); e.className = " a  a ";
  return [e.classList.replace("missing", "b"), e.className];
}, [false, " a  a "]);
test("classList forced toggle returns membership", function () {
  var e = document.createElement("div"); e.className = " a  a ";
  var a = e.classList.toggle("a", true), raw = e.className;
  var b = e.classList.toggle("a", false), c = e.classList.toggle("b");
  return [a, raw, b, c, e.className, e.classList.toggle("b")];
}, [true, " a  a ", false, true, "b", false]);
test("classList value assignment and string conversion", function () {
  var e = document.createElement("div"), list = e.classList;
  list.value = " x x\ty "; e.classList.add("z");
  return [list.value, String(list), list.length, list[1], list["01"]];
}, ["x y z", "x y z", 3, "y", undefined]);
test("classList invalid add is atomic", function () {
  var e = document.createElement("div"); e.className = "a";
  try { e.classList.add("b", "bad token"); } catch (x) { return [x.name, e.className]; }
}, ["InvalidCharacterError", "a"]);
test("classList invalid remove is atomic", function () {
  var e = document.createElement("div"); e.className = "a b";
  try { e.classList.remove("a", ""); } catch (x) { return [x.name, e.className]; }
}, ["SyntaxError", "a b"]);
test("classList invalid replacement is checked even if absent", function () {
  var e = document.createElement("div");
  try { e.classList.replace("missing", "bad\tclass"); } catch (x) { return x.name; }
}, "InvalidCharacterError");
test("classList invalid toggle is checked even with false force", function () {
  var e = document.createElement("div");
  try { e.classList.toggle("", false); } catch (x) { return x.name; }
}, "SyntaxError");
test("classList contains accepts non-token strings", function () {
  var e = document.createElement("div"); e.className = "a b";
  return [e.classList.contains(""), e.classList.contains("a b")];
}, [false, false]);
test("classList item converts unsigned indices", function () {
  var e = document.createElement("div"); e.className = "a b";
  return [e.classList.item(), e.classList.item(NaN), e.classList.item(-1),
          e.classList.item(1.9), e.classList.item(4294967296), e.classList.item(Infinity)];
}, ["a", "a", null, "b", "a", "a"]);
test("classList changes are visible to selectors", function () {
  var e = document.createElement("div"); e.className = " a\tb a ";
  e.classList.remove("a"); e.classList.add("c");
  return [e.matches(".a"), e.matches(".b.c"), e.className];
}, [false, true, "b c"]);
test("classList no-argument methods normalize an existing attribute", function () {
  var e = document.createElement("div"); e.className = " a a\tb ";
  e.classList.add(); var first = e.className;
  e.className = " b b "; e.classList.remove(); return [first, e.className];
}, ["a b", "b"]);
test("classList remove does not create a missing attribute", function () {
  var e = document.createElement("div"); e.classList.remove("missing");
  return e.hasAttribute("class");
}, false);
test("classList converts arguments before token validation", function () {
  var e = document.createElement("div"); e.className = "a";
  var bad = {toString: function () { throw new Error("conversion"); }};
  try { e.classList.add("", bad); } catch (x) { return [x.message, e.className]; }
}, ["conversion", "a"]);
