group("DOM additions in fluxOS 0.2.0");
test("replaceChildren mixes nodes and literal text", function () {
  var e = document.createElement("div"), b = document.createElement("b"); b.textContent = "bold";
  e.innerHTML = "<i>old</i>"; var result = e.replaceChildren("<hello>", b, 42);
  return [e.textContent, e.childNodes.length, e.children.length, e.children[0] === b, result];
}, ["<hello>bold42", 3, 1, true, undefined]);
test("replaceChildren with no arguments clears children", function () {
  var e = document.createElement("div"); e.innerHTML = "<b>x</b>y"; var old = e.firstChild;
  e.replaceChildren(); return [e.childNodes.length, old.parentNode];
}, [0, null]);
test("replaceChildren moves existing children and deduplicates nodes", function () {
  var e = document.createElement("div"); e.innerHTML = "<b>a</b><i>b</i><u>c</u>";
  var a = e.children[0], b = e.children[1]; e.replaceChildren(b, a, b);
  return [e.textContent, e.children[0] === a, e.children[1] === b, a.nextSibling === b, b.previousSibling === a];
}, ["ab", true, true, true, true]);
test("replaceChildren consumes a DocumentFragment", function () {
  var f = document.createDocumentFragment(), e = document.createElement("div");
  f.append("a", document.createElement("b"), "c"); e.replaceChildren(f);
  return [e.textContent, e.childNodes.length, f.childNodes.length];
}, ["ac", 3, 0]);
test("DocumentFragment replaceChildren", function () {
  var f = document.createDocumentFragment(); f.append("old"); f.replaceChildren("new", "!");
  return [f.textContent, f.childNodes.length];
}, ["new!", 2]);
test("replaceChildren rejects an ancestor without detaching children", function () {
  var outer = document.createElement("div"), e = document.createElement("div");
  outer.append(e); e.innerHTML = "<b>keep</b>";
  try { e.replaceChildren("new", outer); } catch (error) { return [e.textContent, e.parentNode === outer]; }
}, ["keep", true]);
test("replaceChildren conversion errors leave children intact", function () {
  var e = document.createElement("div"), other = document.createElement("b");
  e.textContent = "keep"; var bad = {toString: function () { throw new Error("bad"); }};
  try { e.replaceChildren(other, bad); } catch (error) { return [e.textContent, other.parentNode, error.message]; }
}, ["keep", null, "bad"]);
test("insertAdjacentText supports all four positions", function () {
  var p = document.createElement("div"), e = document.createElement("b"); p.append(e);
  e.insertAdjacentText("beforebegin", "a"); e.insertAdjacentText("afterbegin", "b");
  e.insertAdjacentText("beforeend", "c"); e.insertAdjacentText("afterend", "d");
  return [p.textContent, e.textContent, p.childNodes.length];
}, ["abcd", "bc", 3]);
test("insertAdjacentText does not parse HTML", function () {
  var e = document.createElement("div"); e.insertAdjacentText("BEFOREEND", "<b>&copy;</b>");
  return [e.textContent, e.children.length];
}, ["<b>&copy;</b>", 0]);
test("insertAdjacentElement moves and returns the element", function () {
  var p = document.createElement("div"), a = document.createElement("a"), b = document.createElement("b");
  p.append(a, b); var result = a.insertAdjacentElement("beforebegin", b);
  return [result === b, p.children[0] === b, b.nextSibling === a];
}, [true, true, true]);
test("insertAdjacentElement supports interior and afterend positions", function () {
  var p = document.createElement("div"), e = document.createElement("div"); p.append(e);
  var a = document.createElement("a"), b = document.createElement("b"), c = document.createElement("i");
  e.insertAdjacentElement("afterbegin", a); e.insertAdjacentElement("beforeend", b); e.insertAdjacentElement("afterend", c);
  return [e.firstChild === a, e.lastChild === b, e.nextSibling === c];
}, [true, true, true]);
test("detached exterior adjacent insertion", function () {
  var e = document.createElement("div"), b = document.createElement("b");
  e.insertAdjacentText("beforebegin", "x");
  return [e.insertAdjacentElement("afterend", b), b.parentNode, e.textContent];
}, [null, null, ""]);
test("invalid adjacent position throws SyntaxError", function () {
  var e = document.createElement("div");
  try { e.insertAdjacentText("middle", "x"); } catch (error) { return [error.name, e.textContent]; }
}, ["SyntaxError", ""]);
test("insertAdjacentElement rejects a text node", function () {
  var e = document.createElement("div");
  try { e.insertAdjacentElement("beforeend", document.createTextNode("x")); } catch (error) { return error.name; }
}, "TypeError");
test("insertAdjacentElement rejects cycles", function () {
  var e = document.createElement("div"); e.textContent = "keep";
  try { e.insertAdjacentElement("beforeend", e); } catch (error) { return e.textContent; }
}, "keep");
function selectorFixture() {
  var e = document.createElement("section"); e.className = "rows";
  e.innerHTML = '<span class="item">a</span>text<b class="item">b</b><i>c</i><span class="item">d</span><span class="item">e</span>';
  return e;
}
test("DOM :is and :where match selector lists and combinators", function () {
  var e = selectorFixture(); return [e.querySelectorAll(":is(b, i)").length, e.querySelectorAll(":where(.rows > span, .absent)").length];
}, [2, 3]);
test("DOM :not accepts full selector lists", function () {
  var e = selectorFixture(); return e.querySelectorAll(":not(.rows > span, i)")[0].textContent;
}, "b");
test("DOM nth-child counts only the filtered siblings", function () {
  var e = selectorFixture(); return [e.querySelector(":nth-child(2 of .item)").textContent, e.querySelector(":nth-last-child(2 of .item)").textContent];
}, ["b", "d"]);
test("DOM nth-child filters can be complex selector lists", function () {
  var e = selectorFixture(); return e.querySelector(":nth-child(3 of .rows > .item, i)").textContent;
}, "c");
test("DOM negative An+B and nested functional selectors", function () {
  var e = selectorFixture(); return e.querySelectorAll(":nth-child(-n + 2 of :is(.item))").length;
}, 2);
test("DOM nth-of-type and nth-last-of-type", function () {
  var e = selectorFixture(); return [e.querySelector("span:nth-of-type(2)").textContent, e.querySelector("span:nth-last-of-type(1)").textContent, e.querySelector("b:only-of-type").textContent];
}, ["d", "e", "b"]);
test("DOM matches and closest use the new selector forms", function () {
  var e = selectorFixture(), b = e.querySelector("b");
  return [b.matches(":is(b):nth-child(2 of .item)"), b.closest(":where(section)") === e];
}, [true, true]);
test("invalid An+B and of-type filters do not match", function () {
  var e = selectorFixture(); return [e.querySelector(":nth-child(2bad)"), e.querySelector(":nth-child(1 of )"), e.querySelector(":nth-of-type(1 of .item)")];
}, [null, null, null]);
test("HTML supports long and two-code-point named references", function () {
  var e = document.createElement("div"); e.innerHTML = "&CounterClockwiseContourIntegral; &NotEqualTilde; &fjlig;";
  return e.textContent;
}, "∳ ≂̸ fj");
test("HTML legacy references distinguish text from attributes", function () {
  var e = document.createElement("div"); e.innerHTML = '<b title="&notit; &copy=1 &copy;">&notit; &copy &NotEqualTilde</b>';
  return [e.textContent, e.firstChild.getAttribute("title")];
}, ["¬it; © &NotEqualTilde", "&notit; &copy=1 ©"]);
group("Scrolling boxes");
test("scrollTo, scroll and scrollBy exist on elements and take numbers or options", function () {
  var e = document.createElement("div");
  e.scrollTo(3, 4); e.scroll({ left: 1, top: 2 }); e.scrollBy(1, 1); e.scrollBy({ top: 5 });
  return [typeof e.scrollTo, typeof e.scroll, typeof e.scrollBy, e.scrollTop >= 0, e.scrollLeft >= 0];
}, ["function", "function", "function", true, true]);
