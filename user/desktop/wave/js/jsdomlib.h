/*
 * The DOM's parts written in JavaScript. js_dom_eager runs when a page's
 * engine starts (navigator's and the window's smaller members); js_dom_lib
 * the first time a page touches one of its names (jsdom.c puts stand-ins
 * there that load it): TreeWalker and NodeIterator, Range and Selection,
 * form validation, element.animate(), attachShadow(), a style element's
 * sheet (insertRule), document.styleSheets, and smaller things.
 *
 * Like jsprelude.h it goes through the C preprocessor: no single quotes
 * or backticks, every statement ends in a semicolon, "//" only in
 * comments, and no backslash outside a string.
 */
#define JS_DOM_SOURCE(...) #__VA_ARGS__

static const char js_dom_lib[] = JS_DOM_SOURCE(
(function (g) {
  var def = g["%def"];
  /* A worker's scope has no DOM names; it keeps these for MessageChannel and the rest (jsworker.c). */
  var kept = g["%domCtors"] || {};
  var Node = g.Node || kept.Node, HTMLElement = g.HTMLElement || kept.HTMLElement, Document = g.Document || kept.Document;
  var DocumentFragment = g.DocumentFragment || kept.DocumentFragment, Text = g.Text || kept.Text;
  var NP = Node.prototype, EP = HTMLElement.prototype, DP = Document.prototype;
  function getter(o, k, f) { Object.defineProperty(o, k, { get: f, configurable: true, enumerable: false }); }

  /* ---- MessageChannel, BroadcastChannel (within the page), XMLSerializer, Path2D, ImageData ---- */
  class MessagePort extends EventTarget {
    constructor() {
      super();
      def(this, "_other", null);
      def(this, "_closed", false);
      this.onmessage = null;
    }
    postMessage(data) {
      var o = this._other;
      if (!o || this._closed) return;
      var copy = structuredClone(data);
      setTimeout(function () {
        if (o._closed) return;
        var e = new MessageEvent("message", { data: copy });
        o.dispatchEvent(e);
      }, 0);
    }
    start() {}
    close() { this._closed = true; }
  }
  class MessageChannel {
    constructor() {
      this.port1 = new MessagePort();
      this.port2 = new MessagePort();
      this.port1._other = this.port2;
      this.port2._other = this.port1;
    }
  }
  def(g, "MessagePort", MessagePort);
  def(g, "MessageChannel", MessageChannel);

  var channels = [];
  class BroadcastChannel extends EventTarget {
    constructor(name) {
      super();
      def(this, "name", String(name));
      def(this, "_closed", false);
      this.onmessage = null;
      channels.push(this);
    }
    postMessage(data) {
      if (this._closed) throw new DOMException("Channel is closed", "InvalidStateError");
      var copy = structuredClone(data), self = this;
      channels.slice().forEach(function (c) {
        if (c === self || c._closed || c.name !== self.name) return;
        setTimeout(function () {
          if (!c._closed) c.dispatchEvent(new MessageEvent("message", { data: copy }));
        }, 0);
      });
    }
    close() {
      this._closed = true;
      var i = channels.indexOf(this);
      if (i >= 0) channels.splice(i, 1);
    }
  }
  def(g, "BroadcastChannel", BroadcastChannel);

  class XMLSerializer {
    serializeToString(node) {
      if (!node) return "";
      if (node.nodeType === 9) node = node.documentElement;
      if (node.nodeType === 3) return String(node.data).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
      if (node.nodeType === 11) {
        var out = "";
        for (var c = node.firstChild; c; c = c.nextSibling) out += this.serializeToString(c);
        return out;
      }
      return node.outerHTML === undefined ? "" : node.outerHTML;
    }
  }
  def(g, "XMLSerializer", XMLSerializer);

  /* Path2D keeps what it was told, for a canvas to draw (there is no canvas to draw on yet). */
  class Path2D {
    constructor(p) {
      def(this, "ops", []);
      if (p instanceof Path2D) this.ops = p.ops.slice();
      else if (typeof p === "string") this.ops.push(["svg", p]);
    }
    addPath(p) { this.ops = this.ops.concat(p.ops); }
    closePath() { this.ops.push(["close"]); }
    moveTo(x, y) { this.ops.push(["move", x, y]); }
    lineTo(x, y) { this.ops.push(["line", x, y]); }
    bezierCurveTo(a, b, c, d, x, y) { this.ops.push(["bezier", a, b, c, d, x, y]); }
    quadraticCurveTo(a, b, x, y) { this.ops.push(["quad", a, b, x, y]); }
    arc(x, y, r, s, e, ccw) { this.ops.push(["arc", x, y, r, s, e, !!ccw]); }
    arcTo(a, b, c, d, r) { this.ops.push(["arcTo", a, b, c, d, r]); }
    ellipse(x, y, rx, ry, rot, s, e, ccw) { this.ops.push(["ellipse", x, y, rx, ry, rot, s, e, !!ccw]); }
    rect(x, y, w, h) { this.ops.push(["rect", x, y, w, h]); }
    roundRect(x, y, w, h, r) { this.ops.push(["roundRect", x, y, w, h, r]); }
  }
  def(g, "Path2D", Path2D);

  class ImageData {
    constructor(a, b, c) {
      var data, w, h;
      if (typeof a === "number") { w = a; h = b; data = null; }
      else { data = a; w = b; h = c === undefined ? (data.length / 4 / b) : c; }
      if (!(w > 0) || !(h > 0)) throw new DOMException("The source width is zero or not a number.", "IndexSizeError");
      if (!data) {
        data = typeof Uint8ClampedArray === "function" ? new Uint8ClampedArray(w * h * 4) : new Array(w * h * 4).fill(0);
      }
      def(this, "width", w | 0);
      def(this, "height", h | 0);
      def(this, "data", data);
      def(this, "colorSpace", "srgb");
    }
  }
  def(g, "ImageData", ImageData);

  /* ---- NodeFilter, TreeWalker, NodeIterator ---- */
  var NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 4294967295,
    SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, SHOW_PROCESSING_INSTRUCTION: 64,
    SHOW_COMMENT: 128, SHOW_DOCUMENT: 256, SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024 };
  def(g, "NodeFilter", NodeFilter);
  function accept(w, n) {
    if (!(w.whatToShow & (1 << (n.nodeType - 1)))) return 3;
    var f = w.filter;
    if (!f) return 1;
    return typeof f === "function" ? f(n) : f.acceptNode(n);
  }
  class TreeWalker {
    constructor(root, what, filter) {
      this.root = root;
      this.whatToShow = what === undefined ? 4294967295 : what >>> 0;
      this.filter = filter || null;
      this.currentNode = root;
    }
    _after(n, deep) {
      if (deep && n.firstChild) return n.firstChild;
      for (; n && n !== this.root; n = n.parentNode) if (n.nextSibling) return n.nextSibling;
      return null;
    }
    nextNode() {
      var n = this.currentNode, deep = true;
      for (;;) {
        n = this._after(n, deep);
        if (!n) return null;
        var r = accept(this, n);
        if (r === 1) { this.currentNode = n; return n; }
        deep = r !== 2;
      }
    }
    previousNode() {
      var n = this.currentNode;
      while (n && n !== this.root) {
        var p = n.previousSibling;
        if (p) { while (p.lastChild && accept(this, p) !== 2) p = p.lastChild; n = p; } else n = n.parentNode;
        if (n && accept(this, n) === 1) { this.currentNode = n; return n; }
      }
      return null;
    }
    parentNode() {
      for (var n = this.currentNode; n && n !== this.root;) {
        n = n.parentNode;
        if (n && accept(this, n) === 1) { this.currentNode = n; return n; }
      }
      return null;
    }
    _step(first, next) {
      for (var n = this.currentNode[first]; n; n = n[next])
        if (accept(this, n) === 1) { this.currentNode = n; return n; }
      return null;
    }
    firstChild() { return this._step("firstChild", "nextSibling"); }
    lastChild() { return this._step("lastChild", "previousSibling"); }
    nextSibling() { return this._step("nextSibling", "nextSibling"); }
    previousSibling() { return this._step("previousSibling", "previousSibling"); }
  }
  class NodeIterator {
    constructor(root, what, filter) {
      def(this, "_w", new TreeWalker(root, what, filter));
      def(this, "_started", false);
      this.root = root;
      this.whatToShow = this._w.whatToShow;
      this.filter = this._w.filter;
      this.referenceNode = root;
      this.pointerBeforeReferenceNode = true;
    }
    nextNode() {
      if (!this._started) {
        this._started = true;
        this.pointerBeforeReferenceNode = false;
        if (accept(this._w, this.root) === 1) return this.root;
      }
      this._w.currentNode = this.referenceNode;
      var n = this._w.nextNode();
      if (n) this.referenceNode = n;
      return n;
    }
    previousNode() {
      this._w.currentNode = this.referenceNode;
      var n = this._w.previousNode();
      if (n) this.referenceNode = n;
      return n;
    }
    detach() {}
  }
  def(g, "TreeWalker", TreeWalker);
  def(g, "NodeIterator", NodeIterator);
  def(DP, "createTreeWalker", function (r, w, f) { return new TreeWalker(r, w, f); });
  def(DP, "createNodeIterator", function (r, w, f) { return new NodeIterator(r, w, f); });

  /* ---- Range and Selection (the page's own: the browser's selection is not one) ---- */
  function index(n) { return Array.prototype.indexOf.call(n.parentNode.childNodes, n); }
  function rect0() { return { x: 0, y: 0, left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 }; }
  class Range {
    constructor() {
      this.startContainer = this.endContainer = this.commonAncestorContainer = document;
      this.startOffset = this.endOffset = 0;
      this.collapsed = true;
    }
    _update() {
      this.collapsed = this.startContainer === this.endContainer && this.startOffset === this.endOffset;
      var a = this.startContainer;
      while (a && a.contains && !a.contains(this.endContainer)) a = a.parentNode;
      this.commonAncestorContainer = a || this.startContainer;
    }
    setStart(n, o) { this.startContainer = n; this.startOffset = o; this._update(); }
    setEnd(n, o) { this.endContainer = n; this.endOffset = o; this._update(); }
    setStartBefore(n) { this.setStart(n.parentNode, index(n)); }
    setStartAfter(n) { this.setStart(n.parentNode, index(n) + 1); }
    setEndBefore(n) { this.setEnd(n.parentNode, index(n)); }
    setEndAfter(n) { this.setEnd(n.parentNode, index(n) + 1); }
    selectNode(n) { this.setStartBefore(n); this.setEndAfter(n); }
    selectNodeContents(n) { this.setStart(n, 0); this.setEnd(n, n.nodeType === 3 ? n.length : n.childNodes.length); }
    collapse(toStart) {
      if (toStart) { this.endContainer = this.startContainer; this.endOffset = this.startOffset; }
      else { this.startContainer = this.endContainer; this.startOffset = this.endOffset; }
      this._update();
    }
    cloneRange() {
      var r = new Range();
      r.setStart(this.startContainer, this.startOffset);
      r.setEnd(this.endContainer, this.endOffset);
      return r;
    }
    createContextualFragment(html) {
      var t = document.createElement("template");
      t.innerHTML = html;
      return t.content;
    }
    insertNode(n) {
      var c = this.startContainer;
      if (c.nodeType === 3) c.parentNode.insertBefore(n, c.nextSibling);
      else c.insertBefore(n, c.childNodes[this.startOffset] || null);
    }
    surroundContents(n) { this.insertNode(n); }
    _nodes() {
      var c = this.commonAncestorContainer, out = [];
      if (this.startContainer === this.endContainer && c.nodeType !== 3)
        return Array.from(c.childNodes).slice(this.startOffset, this.endOffset);
      return out;
    }
    cloneContents() {
      var f = document.createDocumentFragment();
      this._nodes().forEach(function (n) { f.appendChild(n.cloneNode(true)); });
      return f;
    }
    extractContents() {
      var f = document.createDocumentFragment();
      this._nodes().forEach(function (n) { f.appendChild(n); });
      this.collapse(true);
      return f;
    }
    deleteContents() { this._nodes().forEach(function (n) { n.remove(); }); this.collapse(true); }
    getBoundingClientRect() {
      var n = this.startContainer;
      if (n && n.nodeType === 3) n = n.parentNode;
      return n && n.getBoundingClientRect ? n.getBoundingClientRect() : rect0();
    }
    getClientRects() { return [this.getBoundingClientRect()]; }
    compareBoundaryPoints() { return 0; }
    isPointInRange() { return false; }
    intersectsNode(n) { return this.commonAncestorContainer.contains ? this.commonAncestorContainer.contains(n) : false; }
    detach() {}
    toString() {
      var s = this.startContainer;
      if (s === this.endContainer && s.nodeType === 3) return s.data.slice(this.startOffset, this.endOffset);
      return this._nodes().map(function (n) { return n.textContent; }).join("");
    }
  }
  Range.START_TO_START = 0; Range.START_TO_END = 1; Range.END_TO_END = 2; Range.END_TO_START = 3;
  def(g, "Range", Range);
  def(g, "StaticRange", Range);
  var ranges = [];
  var selection = {
    get rangeCount() { return ranges.length; },
    get isCollapsed() { return !ranges.length || ranges[0].collapsed; },
    get type() { return !ranges.length ? "None" : ranges[0].collapsed ? "Caret" : "Range"; },
    get anchorNode() { return ranges.length ? ranges[0].startContainer : null; },
    get anchorOffset() { return ranges.length ? ranges[0].startOffset : 0; },
    get focusNode() { return ranges.length ? ranges[0].endContainer : null; },
    get focusOffset() { return ranges.length ? ranges[0].endOffset : 0; },
    getRangeAt: function (i) {
      if (i >= ranges.length) throw new DOMException("The index is not in the allowed range.", "IndexSizeError");
      return ranges[i];
    },
    addRange: function (r) { ranges = [r]; },
    removeRange: function () { ranges = []; },
    removeAllRanges: function () { ranges = []; },
    empty: function () { ranges = []; },
    collapse: function (n, o) { if (!n) { ranges = []; return; } var r = new Range(); r.setStart(n, o || 0); r.collapse(true); ranges = [r]; },
    setPosition: function (n, o) { this.collapse(n, o); },
    collapseToStart: function () { if (ranges.length) ranges[0].collapse(true); },
    collapseToEnd: function () { if (ranges.length) ranges[0].collapse(false); },
    selectAllChildren: function (n) { var r = new Range(); r.selectNodeContents(n); ranges = [r]; },
    setBaseAndExtent: function (a, ao, f, fo) { var r = new Range(); r.setStart(a, ao); r.setEnd(f, fo); ranges = [r]; },
    extend: function (n, o) { if (ranges.length) ranges[0].setEnd(n, o || 0); },
    containsNode: function () { return false; },
    modify: function () {},
    deleteFromDocument: function () { if (ranges.length) ranges[0].deleteContents(); },
    toString: function () { return ranges.length ? ranges[0].toString() : ""; }
  };
  def(g, "getSelection", function () { return selection; });
  def(DP, "getSelection", function () { return selection; });
  def(DP, "createRange", function () { return new Range(); });
  def(g, "Selection", function Selection() {});

  /* ---- forms: constraint validation ---- */
  var EMAIL = new RegExp("^[^@\\s]+@[^@\\s]+$"), SCHEME = new RegExp("^[a-zA-Z][a-zA-Z0-9+.-]*:");
  function validity(el) {
    var t = (el.getAttribute("type") || "").toLowerCase(), name = el.localName;
    var v = { valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false, tooShort: false,
              rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false,
              customError: !!el["%custom"], valid: true };
    var control = name === "input" || name === "select" || name === "textarea";
    if (!control || el.disabled || t === "hidden" || t === "submit" || t === "button" || t === "reset" || t === "image") {
      v.valid = !v.customError;
      return v;
    }
    var s = String(el.value);
    if (el.required) {
      if (t === "checkbox") v.valueMissing = !el.checked;
      else if (t === "radio") {
        var form = el.form || document, nm = el.name;
        v.valueMissing = !Array.from(form.querySelectorAll("input[type=radio]")).some(function (r) {
          return r.name === nm && r.checked;
        });
      } else v.valueMissing = s === "";
    }
    if (s !== "") {
      if (t === "email") v.typeMismatch = !s.split(",").every(function (p) { return EMAIL.test(p.trim()); });
      if (t === "url") v.typeMismatch = !SCHEME.test(s);
      var pat = el.getAttribute("pattern");
      if (pat) { try { v.patternMismatch = !new RegExp("^(?:" + pat + ")$").test(s); } catch (e) {} }
      var mx = el.getAttribute("maxlength"), mn = el.getAttribute("minlength");
      if (mx !== null && s.length > +mx) v.tooLong = true;
      if (mn !== null && s.length < +mn) v.tooShort = true;
      if (t === "number" || t === "range") {
        var n = Number(s), lo = el.getAttribute("min"), hi = el.getAttribute("max");
        if (isNaN(n)) v.badInput = true;
        else {
          if (lo !== null && lo !== "" && n < +lo) v.rangeUnderflow = true;
          if (hi !== null && hi !== "" && n > +hi) v.rangeOverflow = true;
        }
      }
    }
    v.valid = !(v.valueMissing || v.typeMismatch || v.patternMismatch || v.tooLong || v.tooShort ||
                v.rangeUnderflow || v.rangeOverflow || v.badInput || v.customError);
    return v;
  }
  getter(EP, "validity", function () { return validity(this); });
  getter(EP, "willValidate", function () {
    var t = (this.getAttribute("type") || "").toLowerCase();
    return /^(input|select|textarea|button)$/.test(this.localName) && !this.disabled && t !== "hidden";
  });
  getter(EP, "validationMessage", function () {
    var v = validity(this);
    return this["%custom"] || (v.valueMissing ? "Please fill out this field." :
      v.typeMismatch ? "Please enter a valid value." : v.patternMismatch ? "Please match the requested format." :
      v.tooShort ? "Please lengthen this text." : v.tooLong ? "Please shorten this text." :
      v.rangeUnderflow || v.rangeOverflow ? "Please enter a value in range." : v.badInput ? "Please enter a number." : "");
  });
  def(EP, "setCustomValidity", function (m) { def(this, "%custom", String(m)); });
  def(EP, "checkValidity", function () {
    if (this.localName === "form" || this.localName === "fieldset") {
      var ok = true;
      Array.from(this.querySelectorAll("input,select,textarea")).forEach(function (e) { if (!e.checkValidity()) ok = false; });
      return ok;
    }
    var v = validity(this);
    if (!v.valid) this.dispatchEvent(new Event("invalid", { cancelable: true }));
    return v.valid;
  });
  def(EP, "reportValidity", function () { return this.checkValidity(); });
  def(EP, "showPicker", function () {});
  def(EP, "setRangeText", function (s) { this.value = String(s); });

  /* Time-based Web Animations for numeric lengths, opacity and colors. */
  var animations = [];
  function easing(name, t) {
    if (name === "step-start") return 1;
    if (name === "step-end") return t === 1 ? 1 : 0;
    var step = new RegExp("^steps\\((\\d+)(?:,\\s*(start|end))?\\)$").exec(name || "");
    if (step) { var n = Math.max(1, +step[1]); return Math.min(1, (step[2] === "start" ? Math.ceil(t * n) : Math.floor(t * n)) / n); }
    var curve = name === "ease" ? [0.25,0.1,0.25,1] : name === "ease-in" ? [0.42,0,1,1] :
                name === "ease-out" ? [0,0,0.58,1] : name === "ease-in-out" ? [0.42,0,0.58,1] : null;
    var bezier = new RegExp("^cubic-bezier\\(([^)]+)\\)$").exec(name || "");
    if (bezier) curve = bezier[1].split(",").map(Number);
    if (!curve || curve.length !== 4) return t;
    function cubic(a,b,u) { var v = 1-u; return 3*v*v*u*a + 3*v*u*u*b + u*u*u; }
    var lo=0, hi=1;
    for(var i=0;i<16;i++){var mid=(lo+hi)/2;if(cubic(curve[0],curve[2],mid)<t)lo=mid;else hi=mid;}
    return cubic(curve[1],curve[3],(lo+hi)/2);
  }
  function mixValue(a,b,t) {
    a=String(a); b=String(b);
    var x=/^(-?[0-9.]+)([a-z%]*)$/.exec(a), y=/^(-?[0-9.]+)([a-z%]*)$/.exec(b);
    if(x && y && x[2]===y[2])return String(+x[1]+(+y[1]-x[1])*t)+x[2];
    function color(s) {
      if(/^#[0-9a-f]{3}$/i.test(s))s="#"+s[1]+s[1]+s[2]+s[2]+s[3]+s[3];
      if(/^#[0-9a-f]{6}$/i.test(s)){var n=parseInt(s.slice(1),16);return [n>>16,(n>>8)&255,n&255];}
      var m=new RegExp("^rgb\\(\\s*(\\d+)[, ]+\\s*(\\d+)[, ]+\\s*(\\d+)\\s*\\)$").exec(s);return m?[+m[1],+m[2],+m[3]]:null;
    }
    var ca=color(a),cb=color(b);
    if(ca && cb)return "rgb("+ca.map(function(v,i){return Math.round(v+(cb[i]-v)*t);}).join(",")+")";
    return t<0.5?a:b;
  }
  def(EP, "animate", function (frames, opts) {
    var el=this, options=typeof opts==="number"?{duration:opts}:opts||{};
    var duration=Math.max(0,Number(options.duration)||0), delay=Number(options.delay)||0;
    var iterations=options.iterations===undefined?1:Math.max(0,Number(options.iterations));
    var total=duration*iterations, direction=options.direction||"normal", fill=options.fill||"none";
    var list=[];
    if(Array.isArray(frames))list=frames.map(function(f){return Object.assign({},f);});
    else if(frames){
      var keys=Object.keys(frames).filter(function(k){return k!=="offset"&&k!=="easing"&&k!=="composite";});
      var length=0;keys.forEach(function(k){length=Math.max(length,Array.isArray(frames[k])?frames[k].length:1);});
      for(var i=0;i<length;i++){var f={};keys.forEach(function(k){var v=frames[k];f[k]=Array.isArray(v)?v[Math.min(i,v.length-1)]:v;});list.push(f);}
    }
    var properties=[];list.forEach(function(f){Object.keys(f).forEach(function(k){if(k!=="offset"&&k!=="easing"&&k!=="composite"&&properties.indexOf(k)<0)properties.push(k);});});
    var original={}, computed=getComputedStyle(el);
    properties.forEach(function(k){original[k]=el.style[k];});
    if(list.length===1){var initial={};properties.forEach(function(k){initial[k]=computed[k]||original[k]||0;});list.unshift(initial);}
    list.forEach(function(f,i){f.offset=f.offset===undefined?(list.length<=1?1:i/(list.length-1)):Number(f.offset);});
    var base=performance.now(), held=0, state="running", rate=1, raf=0, resolve,reject;
    var a=new EventTarget();a.id="";a.onfinish=a.oncancel=null;
    function renew(){a.finished=new Promise(function(ok,bad){resolve=ok;reject=bad;});a.finished.catch(function(){});}
    renew();a.ready=Promise.resolve(a);a.pending=false;a.timeline=document.timeline||null;
    function current(){return state==="running"?held+(performance.now()-base)*rate:held;}
    function restore(){properties.forEach(function(k){el.style[k]=original[k];});}
    function apply(time){
      var local=time-delay;
      if(local<0 && fill!=="backwards"&&fill!=="both"){restore();return;}
      if(local>=total && fill!=="forwards"&&fill!=="both"){restore();return;}
      var clamped=Math.max(0,Math.min(local,total)), cycle=duration?Math.floor(clamped/duration):0;
      var t=duration?clamped/duration-cycle:1;
      if(clamped===total && total>0){cycle=Math.max(0,Math.ceil(iterations)-1);t=iterations-Math.floor(iterations)||1;}
      if(direction==="reverse" || (direction==="alternate"&&cycle%2) || (direction==="alternate-reverse"&&!(cycle%2)))t=1-t;
      t=easing(options.easing||"linear",t);
      properties.forEach(function(k){
        var entries=list.filter(function(f){return f[k]!==undefined;});if(!entries.length)return;
        var lo=entries[0],hi=entries[entries.length-1];
        for(var i=1;i<entries.length;i++)if(t<=entries[i].offset){lo=entries[i-1];hi=entries[i];break;}
        var p=hi.offset===lo.offset?1:Math.max(0,Math.min(1,(t-lo.offset)/(hi.offset-lo.offset)));
        el.style[k]=mixValue(lo[k],hi[k],easing(lo.easing||"linear",p));
      });
    }
    function finishEvent(){resolve(a);var e=new Event("finish");a.dispatchEvent(e);if(typeof a.onfinish==="function")a.onfinish(e);}
    function tick(){
      if(state!=="running")return;
      var time=current();apply(time);
      if((rate>=0&&time>=delay+total)||(rate<0&&time<=0)){held=rate>=0?delay+total:0;state="finished";finishEvent();}
      else raf=requestAnimationFrame(tick);
    }
    getter(a,"playState",function(){return state;});
    Object.defineProperty(a,"currentTime",{get:current,set:function(v){held=Number(v)||0;base=performance.now();apply(held);}});
    Object.defineProperty(a,"playbackRate",{get:function(){return rate;},set:function(v){held=current();base=performance.now();rate=Number(v);}});
    a.pause=function(){held=current();state="paused";cancelAnimationFrame(raf);};
    a.play=function(){if(state==="finished"||state==="idle"){held=rate<0?delay+total:0;renew();}base=performance.now();state="running";cancelAnimationFrame(raf);raf=requestAnimationFrame(tick);};
    a.reverse=function(){a.playbackRate=-rate;if(state!=="running")a.play();};
    a.finish=function(){if(!isFinite(total)||rate===0)throw new DOMException("Cannot finish animation","InvalidStateError");held=rate<0?0:delay+total;state="finished";cancelAnimationFrame(raf);apply(held);finishEvent();};
    a.cancel=function(){cancelAnimationFrame(raf);state="idle";held=0;restore();reject(new DOMException("Animation cancelled","AbortError"));var e=new Event("cancel");a.dispatchEvent(e);if(typeof a.oncancel==="function")a.oncancel(e);};
    a.updatePlaybackRate=function(v){a.playbackRate=v;};a.persist=function(){};
    a.commitStyles=function(){apply(current());properties.forEach(function(k){original[k]=el.style[k];});};
    a.effect={target:el,getKeyframes:function(){return list.map(function(f){return Object.assign({},f);});},getTiming:function(){return Object.assign({},options);}};
    animations.push(a);apply(0);raf=requestAnimationFrame(tick);return a;
  });
  def(EP,"getAnimations",function(){var el=this;return animations.filter(function(a){return a.effect.target===el&&(a.playState==="running"||a.playState==="paused");});});
  def(DP,"getAnimations",function(){return animations.filter(function(a){return a.playState==="running"||a.playState==="paused";});});

  /* Native shadow roots keep light and shadow trees separate. */
  def(EP, "assignedElements", function () { return this.assignedNodes().filter(function (n) { return n.nodeType === 1; }); });

  /* ---- style sheets: a <style>'s sheet keeps its rules as the element's text ---- */
  function rulesOf(text) {
    var out = [], depth = 0, start = 0;
    for (var i = 0; i < text.length; i++) {
      var c = text[i];
      if (c === "{") depth++;
      else if (c === "}" && depth && !--depth) { out.push(text.slice(start, i + 1).trim()); start = i + 1; }
      else if (c === ";" && !depth) { out.push(text.slice(start, i + 1).trim()); start = i + 1; }
    }
    return out.filter(function (r) { return r; });
  }
  function sheetFor(owner) {
    if (owner["%sheet"]) return owner["%sheet"];
    function setText(text) {
      owner.textContent = String(text);
      if (sh["%notify"]) sh["%notify"]();
    }
    var sh = {
      ownerNode: owner, disabled: false, type: "text/css", media: { mediaText: owner.getAttribute("media") || "" },
      get href() { return owner.localName === "link" ? owner.href : null; },
      get cssRules() {
        return rulesOf(owner.localName === "style" ? owner.textContent : "").map(function (t) {
          return { cssText: t, selectorText: t.slice(0, t.indexOf("{")).trim(), type: 1, parentStyleSheet: sh };
        });
      },
      insertRule: function (rule, at) {
        if (owner.localName !== "style") return 0;
        var r = rulesOf(owner.textContent);
        at = at === undefined ? 0 : at;
        if (at < 0 || at > r.length) throw new DOMException("The index is not in the allowed range.", "IndexSizeError");
        r.splice(at, 0, String(rule));
        setText(r.join("\n"));
        return at;
      },
      deleteRule: function (at) {
        var r = rulesOf(owner.textContent);
        if (at < 0 || at >= r.length) throw new DOMException("Invalid rule index", "IndexSizeError");
        r.splice(at, 1);
        setText(r.join("\n"));
      },
      replaceSync: function (t) { setText(t); },
      replace: function (t) { setText(t); return Promise.resolve(sh); }
    };
    sh.rules = sh.cssRules;
    sh.addRule = function (sel, body, at) { return sh.insertRule(sel + " { " + body + " }", at === undefined ? sh.cssRules.length : at); };
    sh.removeRule = sh.deleteRule;
    def(owner, "%sheet", sh);
    return sh;
  }
  getter(EP, "sheet", function () {
    var n = this.localName, rel = (this.getAttribute("rel") || "").toLowerCase();
    return n === "style" || (n === "link" && rel.indexOf("stylesheet") >= 0) ? sheetFor(this) : null;
  });
  getter(DP, "styleSheets", function () {
    return Array.from(document.querySelectorAll("style, link[rel~=stylesheet]")).map(sheetFor);
  });
  var adoptions = new WeakMap();
  function syncAdoption(root) {
    var state = adoptions.get(root);
    if (state) g["%adoptSheets"](root, state.items.filter(function (s) { return !s.disabled; })
      .map(function (s) { return s["%owner"].textContent; }).join("\n"));
  }
  function validateSheet(s) {
    if (!s || !s["%constructed"]) throw new DOMException("Only constructed sheets can be adopted", "NotAllowedError");
  }
  function adoption(root) {
    var state = adoptions.get(root);
    if (state) return state;
    state = { items: [] };
    state.view = new Proxy(state.items, {
      get: function (a, k) {
        var value = a[k];
        if (typeof value !== "function") return value;
        return function () {
          var mutates = ["push", "pop", "shift", "unshift", "splice", "reverse", "sort", "fill", "copyWithin"].indexOf(k) >= 0;
          if (mutates) {
            var trial = a.slice();
            value.apply(trial, arguments);
            trial.forEach(validateSheet);
            trial.forEach(function (sheet) { watchSheet(sheet, root); });
          }
          var result = value.apply(a, arguments);
          if (mutates) syncAdoption(root);
          return result === a ? state.view : result;
        };
      },
      set: function (a, k, value) {
        if (k !== "length") { validateSheet(value); watchSheet(value, root); }
        a[k] = value;
        syncAdoption(root);
        return true;
      },
      deleteProperty: function (a, k) { delete a[k]; syncAdoption(root); return true; }
    });
    adoptions.set(root, state);
    return state;
  }
  function watchSheet(sheet, root) {
    var roots = sheet["%adopters"];
    if (roots.indexOf(root) < 0) roots.push(root);
  }
  function installAdoption(proto) {
    Object.defineProperty(proto, "adoptedStyleSheets", {
      configurable: true,
      get: function () { return adoption(this).view; },
      set: function (sheets) {
        var items = Array.from(sheets);
        items.forEach(validateSheet);
        var state = adoption(this);
        state.items.length = 0;
        var root = this;
        items.forEach(function (sheet) { state.items.push(sheet); watchSheet(sheet, root); });
        syncAdoption(root);
      }
    });
  }
  installAdoption(DP);
  installAdoption(DocumentFragment.prototype);
  function CSSStyleSheet() {
    var st = document.createElement("style");
    var sheet = sheetFor(st), disabled = false;
    Object.setPrototypeOf(sheet, CSSStyleSheet.prototype);
    sheet.ownerNode = null;
    def(sheet, "%constructed", true);
    def(sheet, "%owner", st);
    def(sheet, "%adopters", []);
    def(sheet, "%notify", function () { sheet["%adopters"].forEach(syncAdoption); });
    Object.defineProperty(sheet, "disabled", { get: function () { return disabled; },
      set: function (v) { disabled = !!v; sheet["%notify"](); }, configurable: true });
    return sheet;
  }
  def(g, "CSSStyleSheet", CSSStyleSheet);
  def(g, "StyleSheet", CSSStyleSheet);
  def(g, "CSSRule", { STYLE_RULE: 1, IMPORT_RULE: 3, MEDIA_RULE: 4, FONT_FACE_RULE: 5, KEYFRAMES_RULE: 7 });
  /* Font loading shares the real stylesheet parser and font registry. */
  var fontSet = [], fontListeners = new EventTarget();
  function fontName(font) { return String(font).replace(new RegExp("^.*?[0-9]+(?:\\.[0-9]+)?(?:px|pt|em|rem|%)(?:/[^ ]+)?\\s+"), "").split(",")[0].trim().replace(new RegExp("^[\\x22\\x27]|[\\x22\\x27]$", "g"), ""); }
  function fontCheck(font) { var name=fontName(font), status=document.__fontStatus(name);return status===1 || (status===-1 && /^(serif|sans-serif|monospace|system-ui|cursive|fantasy)$/i.test(name)); }
  var fonts = { get status() { return document.__fontStatus("")===0 || fontSet.some(function(f){return f.status==="loading";}) ? "loading" : "loaded"; },
    get size() { return fontSet.length; },
    get ready() { return new Promise(function(resolve){function wait(){if(fonts.status==="loaded"){resolve(fonts);return;}setTimeout(wait,20);}wait();}); },
    check: fontCheck,
    load: function(font,text) { var name=fontName(font), matches=fontSet.filter(function(f){return f.family===name;});
      return Promise.all(matches.map(function(f){return f.load();})).then(function(){return fonts.ready;}).then(function(){return matches;}); },
    add: function(f) { if(!(f instanceof FontFace))throw new TypeError("Expected FontFace");if(fontSet.indexOf(f)<0)fontSet.push(f);return fonts; },
    delete: function(f) { var i=fontSet.indexOf(f);if(i<0)return false;fontSet.splice(i,1);return true; },
    clear: function() {fontSet=[];}, has: function(f) {return fontSet.indexOf(f)>=0;},
    forEach: function(f,t) {fontSet.forEach(function(v){f.call(t,v,v,fonts);});}, values: function(){return fontSet[Symbol.iterator]();},
    addEventListener: function(t,f,o){fontListeners.addEventListener(t,f,o);},removeEventListener:function(t,f,o){fontListeners.removeEventListener(t,f,o);} };
  def(DP,"fonts",fonts);
  function FontFace(family,source,descriptors) {
    var self=this;descriptors=descriptors||{};this.family=String(family);this.style=descriptors.style||"normal";this.weight=descriptors.weight||"normal";this.status="unloaded";
    this._source=source;this._promise=null;
    this.loaded=new Promise(function(resolve,reject){self._resolve=resolve;self._reject=reject;});
    if(source instanceof ArrayBuffer || ArrayBuffer.isView(source))this.load();
  }
  FontFace.prototype.load=function(){
    if(this._promise)return this._promise;
    var self=this;this.status="loading";
    var source=this._source;
    if(source instanceof ArrayBuffer || ArrayBuffer.isView(source))source=Promise.resolve(source);
    else { var match=String(source).match(new RegExp("url\\(\\s*[\\x22\\x27]?([^\\x22\\x27\\s]+?)[\\x22\\x27]?\\s*\\)"));
      source=match?fetch(match[1]).then(function(r){if(!r.ok)throw new Error("Font request failed");return r.arrayBuffer();}):Promise.reject(new SyntaxError("Font source requires url() or font bytes")); }
    this._promise=source.then(function(bytes){
      if(!document.__fontRegister(self.family,bytes,self.weight==="bold"||parseInt(self.weight)>=600,self.style!=="normal"))throw new Error("Font decoding failed");
      self.status="loaded";self._resolve(self);fontListeners.dispatchEvent(new Event("loadingdone"));return self;
    }).catch(function(e){self.status="error";self._reject(e);fontListeners.dispatchEvent(new Event("loadingerror"));throw e;});
    return this._promise;
  };
  def(g,"FontFace",FontFace);

  /* ---- smaller things ---- */
  def(DP, "createAttribute", function (n) { return { name: String(n).toLowerCase(), value: "" }; });
  def(DP, "queryCommandSupported", function () { return false; });
  def(DP, "exitFullscreen", function () { return Promise.resolve(); });
  def(DP, "fullscreenElement", null);
  def(DP, "fullscreenEnabled", false);
  def(DP, "pictureInPictureEnabled", false);
  def(DP, "hasStorageAccess", function () { return Promise.resolve(true); });
  def(DP, "requestStorageAccess", function () { return Promise.resolve(); });
  def(EP, "requestFullscreen", function () { return Promise.reject(new DOMException("Fullscreen is not supported", "NotSupportedError")); });
  def(EP, "scrollTo", function (x, y) {
    if (x !== null && typeof x === "object") { y = x.top; x = x.left; }
    if (x !== undefined && x !== null) this.scrollLeft = x;
    if (y !== undefined && y !== null) this.scrollTop = y;
  });
  def(EP, "scroll", EP.scrollTo);
  def(EP, "scrollBy", function (x, y) {
    if (x !== null && typeof x === "object") { y = x.top; x = x.left; }
    if (x) this.scrollLeft = this.scrollLeft + Number(x);
    if (y) this.scrollTop = this.scrollTop + Number(y);
  });
  def(EP, "getClientRects", function () { return [this.getBoundingClientRect()]; });
  def(EP, "checkVisibility", function () { var r = this.getBoundingClientRect(); return r.width > 0 || r.height > 0; });
  def(EP, "setPointerCapture", function (id) { return g["%pointerCapture"].call(this, id, 1); });
  def(EP, "releasePointerCapture", function (id) { return g["%pointerCapture"].call(this, id, 0); });
  def(EP, "hasPointerCapture", function (id) { return g["%pointerCapture"].call(this, id, 2); });
  def(EP, "showPopover", function () { this.setAttribute("data-popover-open", ""); this.style.display = "block"; });
  def(EP, "hidePopover", function () { this.removeAttribute("data-popover-open"); this.style.display = ""; });
  def(EP, "togglePopover", function () {
    if (this.hasAttribute("data-popover-open")) this.hidePopover(); else this.showPopover();
  });
  Object.defineProperty(EP, "inert", { configurable:true, get:function(){return this.hasAttribute("inert");}, set:function(v){if(v)this.setAttribute("inert", "");else this.removeAttribute("inert");} });
  Object.defineProperty(EP, "draggable", { configurable: true, get: function () {   /* pictures and links are by default */
    var a = this.getAttribute("draggable");
    if (a === "true" || a === "false") return a === "true";
    var t = this.localName;
    return t === "img" || (t === "a" && this.hasAttribute("href"));
  }, set: function (v) { this.setAttribute("draggable", v ? "true" : "false"); } });
  getter(EP, "slot", function () { return this.getAttribute("slot") || ""; });
  getter(EP, "part", function () { return this.classList; });
  def(Text.prototype, "splitText", function (at) {
    var rest = document.createTextNode(this.data.slice(at));
    this.data = this.data.slice(0, at);
    if (this.parentNode) this.parentNode.insertBefore(rest, this.nextSibling);
    return rest;
  });
  def(Text.prototype, "appendData", function (s) { this.data += s; });
  def(Text.prototype, "substringData", function (o, n) { return this.data.substr(o, n); });

  /* ---- stand-ins for what this browser cannot do: they answer the way a browser without it would ---- */
  class ReportingObserver {
    constructor(callback, options) { def(this, "_q", []); }
    observe() {}
    disconnect() {}
    takeRecords() { var q = this._q; this._q = []; return q; }
  }
  def(g, "ReportingObserver", ReportingObserver);
  class TaskController extends AbortController {
    constructor(init) { super(); def(this, "_p", init && init.priority || "user-visible"); }
    setPriority(p) { this._p = String(p); }
  }
  def(g, "TaskController", TaskController);
  def(g, "scheduler", {
    postTask: function (callback, options) {
      options = options || {};
      return new Promise(function (resolve, reject) {
        var signal = options.signal;
        if (signal && signal.aborted) { reject(signal.reason); return; }
        var id = setTimeout(function () { try { resolve(callback()); } catch (e) { reject(e); } }, options.delay || 0);
        if (signal) signal.addEventListener("abort", function () { clearTimeout(id); reject(signal.reason); }, { once: true });
      });
    },
    yield: function () { return new Promise(function (resolve) { setTimeout(resolve, 0); }); }
  });
  class SpeechSynthesisUtterance extends EventTarget {
    constructor(text) {
      super();
      this.text = text === undefined ? "" : String(text);
      this.lang = ""; this.voice = null; this.volume = 1; this.rate = 1; this.pitch = 1;
      this.onstart = this.onend = this.onerror = this.onpause = this.onresume = this.onboundary = this.onmark = null;
    }
  }
  def(g, "SpeechSynthesisUtterance", SpeechSynthesisUtterance);
  def(g, "speechSynthesis", {
    pending: false, speaking: false, paused: false, onvoiceschanged: null,
    speak: function (u) {
      setTimeout(function () {                 /* there is no voice to speak with */
        var e = new Event("error");
        e.error = "synthesis-unavailable"; e.utterance = u; e.charIndex = 0; e.elapsedTime = 0;
        if (u && u.dispatchEvent) u.dispatchEvent(e);
        if (u && typeof u.onerror === "function") u.onerror(e);
      }, 0);
    },
    cancel: function () {}, pause: function () {}, resume: function () {},
    getVoices: function () { return []; },
    addEventListener: function () {}, removeEventListener: function () {}, dispatchEvent: function () { return true; }
  });
  def(EP, "requestPointerLock", function () {
    setTimeout(function () { document.dispatchEvent(new Event("pointerlockerror")); }, 0);
    var p = Promise.reject(new DOMException("Pointer lock is not available", "NotSupportedError"));
    p.catch(function () {});
    return p;
  });
  getter(DP, "pointerLockElement", function () { return null; });
  def(DP, "exitPointerLock", function () {});

  /* ---- caretPositionFromPoint: the text under a point, and how far along it ---- */
  function caretAt(x, y) {
    var el = document.elementFromPoint(x, y);
    if (!el) return null;
    var best = null, bestD = Infinity, w = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
    for (var t = w.nextNode(); t; t = w.nextNode()) {
      if (!t.data.trim()) continue;
      var r = document.createRange();
      r.selectNodeContents(t);
      var b = r.getBoundingClientRect();
      if (!b.width && !b.height) continue;
      var dy = y < b.top ? b.top - y : y > b.bottom ? y - b.bottom : 0;
      var dx = x < b.left ? b.left - x : x > b.right ? x - b.right : 0;
      if (dy * 10000 + dx < bestD) { bestD = dy * 10000 + dx; best = { t: t, b: b }; }
    }
    if (!best) return { node: el, offset: 0 };
    var len = best.t.data.length, frac = best.b.width > 0 ? (x - best.b.left) / best.b.width : 0;
    return { node: best.t, offset: Math.max(0, Math.min(len, Math.round(frac * len))) };
  }
  def(DP, "caretPositionFromPoint", function (x, y) {
    var c = caretAt(x, y);
    if (!c) return null;
    return { offsetNode: c.node, offset: c.offset,
      getClientRect: function () {
        var r = document.createRange();
        r.setStart(c.node, c.offset);
        r.collapse(true);
        return r.getBoundingClientRect();
      } };
  });
  def(DP, "caretRangeFromPoint", function (x, y) {
    var c = caretAt(x, y);
    if (!c) return null;
    var r = document.createRange();
    r.setStart(c.node, c.offset);
    r.collapse(true);
    return r;
  });

  /* ---- ElementInternals: form-associated custom elements ---- */
  var EI_KEY = {}, INTERNALS = new WeakMap();
  var VALIDITY = ["valueMissing", "typeMismatch", "patternMismatch", "tooLong", "tooShort", "rangeUnderflow",
                  "rangeOverflow", "stepMismatch", "badInput", "customError"];
  class CustomStateSet {
    constructor(key) { if (key !== EI_KEY) throw new TypeError("Illegal constructor"); def(this, "_s", new Set()); }
    add(v) { this._s.add(String(v)); return this; }
    delete(v) { return this._s.delete(String(v)); }
    has(v) { return this._s.has(String(v)); }
    clear() { this._s.clear(); }
    get size() { return this._s.size; }
    forEach(f, t) { var me = this; this._s.forEach(function (v) { f.call(t, v, v, me); }); }
    values() { return this._s.values(); }
    keys() { return this._s.values(); }
    entries() { return this._s.entries(); }
    [Symbol.iterator]() { return this._s.values(); }
  }
  function formAssociated(i) {
    if (!i._el.constructor || !i._el.constructor.formAssociated)
      throw new DOMException("The target element is not a form-associated custom element.", "NotSupportedError");
  }
  class ElementInternals {
    constructor(key, el) {
      if (key !== EI_KEY) throw new TypeError("Illegal constructor");
      def(this, "_el", el); def(this, "_value", null); def(this, "_state", null);
      def(this, "_flags", {}); def(this, "_msg", ""); def(this, "_states", new CustomStateSet(EI_KEY));
    }
    get shadowRoot() { return this._el.shadowRoot || null; }
    get form() { formAssociated(this); return this._el.closest("form"); }
    get states() { return this._states; }
    setFormValue(value, state) { formAssociated(this); this._value = value; this._state = state === undefined ? value : state; }
    get willValidate() { formAssociated(this); return !this._el.hasAttribute("disabled") && !this._el.hasAttribute("readonly"); }
    get validity() {
      formAssociated(this);
      var f = this._flags, v = {}, any = false;
      VALIDITY.forEach(function (k) { v[k] = !!f[k]; any = any || v[k]; });
      v.valid = !any;
      return v;
    }
    get validationMessage() { formAssociated(this); return this.validity.valid ? "" : this._msg; }
    setValidity(flags, message, anchor) {
      formAssociated(this);
      flags = flags || {};
      var any = VALIDITY.some(function (k) { return !!flags[k]; });
      if (any && !message) throw new TypeError("A message is required when the element is invalid");
      this._flags = Object.assign({}, flags);
      this._msg = any ? String(message) : "";
    }
    checkValidity() {
      if (!this.willValidate || this.validity.valid) return true;
      this._el.dispatchEvent(new Event("invalid", { cancelable: true }));
      return false;
    }
    reportValidity() { return this.checkValidity(); }
    get labels() {
      formAssociated(this);
      var id = this._el.id, el = this._el;
      return Array.from(document.querySelectorAll("label")).filter(function (l) {
        return (id && l.htmlFor === id) || l.contains(el);
      });
    }
  }
  def(g, "ElementInternals", ElementInternals);
  def(g, "CustomStateSet", CustomStateSet);
  def(EP, "attachInternals", function () {
    var name = String(this.localName || this.tagName).toLowerCase();
    if (name.indexOf("-") < 0 || !customElements.get(name))
      throw new DOMException("Unable to attach ElementInternals to non-custom elements.", "NotSupportedError");
    if (INTERNALS.has(this))
      throw new DOMException("ElementInternals for the specified element was already attached.", "NotSupportedError");
    var i = new ElementInternals(EI_KEY, this);
    INTERNALS.set(this, i);
    return i;
  });
  /* FormData(form) asks for the values form-associated elements gave */
  def(g, "%formAssociated", function (form, list) {
    var els = form.querySelectorAll("*");
    for (var k = 0; k < els.length; k++) {
      var e = els[k], i = INTERNALS.get(e);
      if (!i || !e.constructor.formAssociated || e.hasAttribute("disabled")) continue;
      var name = e.getAttribute("name"), v = i._value;
      if (v === null || v === undefined) continue;
      if (v instanceof FormData) { v.forEach(function (val, key) { list.push([key, val]); }); continue; }
      if (name) list.push([name, v instanceof Blob ? v : String(v)]);
    }
  });

  /* ---- setHTML, setHTMLUnsafe and the Sanitizer ---- */
  var UNSAFE_ELEMENTS = ["script", "iframe", "frame", "frameset", "object", "embed", "applet", "base", "meta", "link",
                         "style", "noscript", "template", "portal", "fencedframe"];
  var URL_ATTRS = ["href", "src", "action", "formaction", "xlink:href", "data", "poster", "background", "cite", "ping"];
  function sanitizerConfig(c) {
    var o = { elements: null, removeElements: [], replaceWithChildrenElements: [], attributes: null,
              removeAttributes: [], comments: false, dataAttributes: true };
    if (!c) return o;
    function names(l) { return (l || []).map(function (x) { return String(typeof x === "object" ? x.name : x).toLowerCase(); }); }
    if (c.elements) o.elements = names(c.elements);
    o.removeElements = names(c.removeElements);
    o.replaceWithChildrenElements = names(c.replaceWithChildrenElements);
    if (c.attributes) o.attributes = names(c.attributes);
    o.removeAttributes = names(c.removeAttributes);
    if (c.comments !== undefined) o.comments = !!c.comments;
    if (c.dataAttributes !== undefined) o.dataAttributes = !!c.dataAttributes;
    return o;
  }
  class Sanitizer {
    constructor(config) {
      if (config === "default" || config === undefined) config = {};
      def(this, "_c", config instanceof Sanitizer ? JSON.parse(JSON.stringify(config._c)) : sanitizerConfig(config));
    }
    get() { return JSON.parse(JSON.stringify(this._c)); }
    allowElement(n) { n = String(n).toLowerCase(); if (this._c.elements && this._c.elements.indexOf(n) < 0) this._c.elements.push(n); this._c.removeElements = this._c.removeElements.filter(function (x) { return x !== n; }); }
    removeElement(n) { n = String(n).toLowerCase(); this._c.removeElements.push(n); if (this._c.elements) this._c.elements = this._c.elements.filter(function (x) { return x !== n; }); }
    replaceElementWithChildren(n) { this._c.replaceWithChildrenElements.push(String(n).toLowerCase()); }
    allowAttribute(n) { n = String(n).toLowerCase(); if (this._c.attributes && this._c.attributes.indexOf(n) < 0) this._c.attributes.push(n); this._c.removeAttributes = this._c.removeAttributes.filter(function (x) { return x !== n; }); }
    removeAttribute(n) { this._c.removeAttributes.push(String(n).toLowerCase()); }
    setComments(b) { this._c.comments = !!b; }
    setDataAttributes(b) { this._c.dataAttributes = !!b; }
    removeUnsafe() { var c = this._c; UNSAFE_ELEMENTS.forEach(function (n) { if (c.removeElements.indexOf(n) < 0) c.removeElements.push(n); }); }
  }
  def(g, "Sanitizer", Sanitizer);
  function scriptURL(v) {
    var t = String(v).replace(new RegExp("[\\u0000-\\u0020]", "g"), "").toLowerCase();
    return t.indexOf("javascript:") === 0 || t.indexOf("vbscript:") === 0 || t.indexOf("data:text/html") === 0;
  }
  function sanitize(root, c, safe) {
    var kids = Array.from(root.childNodes);
    for (var i = 0; i < kids.length; i++) {
      var n = kids[i];
      if (n.nodeType === 8) { if (!c.comments || safe) n.remove(); continue; }
      if (n.nodeType !== 1) continue;
      var name = n.localName || String(n.tagName).toLowerCase();
      if ((safe && UNSAFE_ELEMENTS.indexOf(name) >= 0) || c.removeElements.indexOf(name) >= 0) { n.remove(); continue; }
      if (c.replaceWithChildrenElements.indexOf(name) >= 0) {
        sanitize(n, c, safe);
        while (n.firstChild) n.parentNode.insertBefore(n.firstChild, n);
        n.remove();
        continue;
      }
      if (c.elements && c.elements.indexOf(name) < 0) { n.remove(); continue; }
      Array.from(n.attributes || []).forEach(function (a) {
        var an = a.name.toLowerCase();
        var drop = c.removeAttributes.indexOf(an) >= 0 ||
                   (c.attributes && c.attributes.indexOf(an) < 0 && !(c.dataAttributes && an.indexOf("data-") === 0)) ||
                   (!c.dataAttributes && an.indexOf("data-") === 0) ||
                   (safe && (an.indexOf("on") === 0 || an === "srcdoc" || (URL_ATTRS.indexOf(an) >= 0 && scriptURL(a.value))));
        if (drop) n.removeAttribute(a.name);
      });
      sanitize(n, c, safe);
    }
  }
  function declarativeShadows(root) {
    var ts = Array.from(root.querySelectorAll("template[shadowrootmode]"));
    ts.forEach(function (t) {
      var mode = String(t.getAttribute("shadowrootmode")).toLowerCase(), host = t.parentNode;
      if ((mode !== "open" && mode !== "closed") || !host || host.nodeType !== 1 || host.shadowRoot) return;
      try {
        var sr = host.attachShadow({ mode: mode }), content = t.content || t;
        while (content.firstChild) sr.appendChild(content.firstChild);
        t.remove();
        declarativeShadows(sr);
      } catch (e) {}
    });
  }
  function parsed(html) {
    var t = document.createElement("template");
    t.innerHTML = String(html);
    return t.content || t;
  }
  function replaceChildren(el, frag) {
    while (el.firstChild) el.removeChild(el.firstChild);
    while (frag.firstChild) el.appendChild(frag.firstChild);
  }
  function sanitizerOf(o) {
    var s = o && o.sanitizer;
    return s instanceof Sanitizer ? s._c : sanitizerConfig(s === "default" ? null : s);
  }
  def(EP, "setHTML", function (html, options) {
    var frag = parsed(html);
    sanitize(frag, sanitizerOf(options), true);         /* script and event handlers go whatever the config says */
    replaceChildren(this, frag);
  });
  def(EP, "setHTMLUnsafe", function (html, options) {
    var frag = parsed(html);
    if (options && options.sanitizer) sanitize(frag, sanitizerOf(options), false);
    replaceChildren(this, frag);
    declarativeShadows(this);
  });
  def(g, "%parseHTML", function (html, options, safe) {
    var d = new DOMParser().parseFromString(String(html), "text/html");
    var root = d.documentElement;
    if (safe || (options && options.sanitizer)) sanitize(root, sanitizerOf(options), safe);
    if (!safe) declarativeShadows(root);
    return d;
  });

  /* ---- Trusted Types: the policy objects and the trusted values (nothing here enforces them) ---- */
  var TT_KEY = {};
  function trusted(Kind, v) { var o = Object.create(Kind.prototype); def(o, "_v", String(v)); return o; }
  class TrustedHTML {
    constructor(k) { if (k !== TT_KEY) throw new TypeError("Illegal constructor"); }
    toString() { return this._v; }
    toJSON() { return this._v; }
    static fromLiteral(s) { return trusted(TrustedHTML, s && s.raw ? s.raw.join("") : s); }
  }
  class TrustedScript {
    constructor(k) { if (k !== TT_KEY) throw new TypeError("Illegal constructor"); }
    toString() { return this._v; }
    toJSON() { return this._v; }
    static fromLiteral(s) { return trusted(TrustedScript, s && s.raw ? s.raw.join("") : s); }
  }
  class TrustedScriptURL {
    constructor(k) { if (k !== TT_KEY) throw new TypeError("Illegal constructor"); }
    toString() { return this._v; }
    toJSON() { return this._v; }
    static fromLiteral(s) { return trusted(TrustedScriptURL, s && s.raw ? s.raw.join("") : s); }
  }
  class TrustedTypePolicy {
    constructor(k, name, rules) {
      if (k !== TT_KEY) throw new TypeError("Illegal constructor");
      def(this, "_n", name); def(this, "_r", rules || {});
    }
    get name() { return this._n; }
    _make(Kind, rule, args) {
      var f = this._r[rule];
      if (typeof f !== "function") throw new TypeError("Policy " + this._n + " has no " + rule + " member");
      var v = f.apply(undefined, args);
      return trusted(Kind, v === undefined || v === null ? "" : v);
    }
    createHTML() { return this._make(TrustedHTML, "createHTML", Array.from(arguments)); }
    createScript() { return this._make(TrustedScript, "createScript", Array.from(arguments)); }
    createScriptURL() { return this._make(TrustedScriptURL, "createScriptURL", Array.from(arguments)); }
  }
  class TrustedTypePolicyFactory {
    constructor(k) { if (k !== TT_KEY) throw new TypeError("Illegal constructor"); def(this, "_d", null); }
    createPolicy(name, rules) {
      var p = new TrustedTypePolicy(TT_KEY, String(name), rules);
      if (String(name) === "default") {
        if (this._d) throw new TypeError("A default policy already exists");
        this._d = p;
      }
      return p;
    }
    isHTML(v) { return v instanceof TrustedHTML; }
    isScript(v) { return v instanceof TrustedScript; }
    isScriptURL(v) { return v instanceof TrustedScriptURL; }
    get emptyHTML() { return trusted(TrustedHTML, ""); }
    get emptyScript() { return trusted(TrustedScript, ""); }
    get defaultPolicy() { return this._d; }
    getAttributeType(tag, attr) {
      tag = String(tag).toLowerCase(); attr = String(attr).toLowerCase();
      if (attr.indexOf("on") === 0) return "TrustedScript";
      if (tag === "iframe" && attr === "srcdoc") return "TrustedHTML";
      if ((tag === "script" && attr === "src") || (tag === "embed" && attr === "src") || (tag === "object" && attr === "data")) return "TrustedScriptURL";
      return null;
    }
    getPropertyType(tag, prop) {
      tag = String(tag).toLowerCase(); prop = String(prop);
      if (prop === "innerHTML" || prop === "outerHTML" || (tag === "iframe" && prop === "srcdoc")) return "TrustedHTML";
      if (tag === "script" && prop === "src") return "TrustedScriptURL";
      if (tag === "script" && (prop === "text" || prop === "textContent" || prop === "innerText")) return "TrustedScript";
      return null;
    }
  }
  def(g, "TrustedHTML", TrustedHTML);
  def(g, "TrustedScript", TrustedScript);
  def(g, "TrustedScriptURL", TrustedScriptURL);
  def(g, "TrustedTypePolicy", TrustedTypePolicy);
  def(g, "TrustedTypePolicyFactory", TrustedTypePolicyFactory);
  def(g, "trustedTypes", new TrustedTypePolicyFactory(TT_KEY));

})(globalThis);
);

static const char js_dom_eager[] = JS_DOM_SOURCE(
(function (g) {
  var def = g["%def"];
  var nav = g.navigator;
  nav.hardwareConcurrency = 1;
  nav.maxTouchPoints = 0;
  nav.pdfViewerEnabled = false;
  nav.webdriver = false;
  nav.doNotTrack = null;
  nav.product = "Gecko";
  /* no userAgentData or deviceMemory: Chromium's, and the User-Agent names a Gecko browser */
  URL.createObjectURL = function (blob) { if(!(blob instanceof Blob))throw new TypeError("createObjectURL requires a Blob");return g["%blobURL"](blob._bytes,blob.type); };
  URL.revokeObjectURL = function (url) { g["%blobURL"](String(url)); };
  nav.clipboard = {
    writeText: function (text) { try { return Promise.resolve(g["%clipboard"](String(text))); } catch(e) { return Promise.reject(new DOMException(String(e), "NotAllowedError")); } },
    readText: function () { try { return Promise.resolve(g["%clipboard"]()); } catch(e) { return Promise.reject(new DOMException(String(e), "NotAllowedError")); } }
  };
  nav.permissions = { query: function (d) { return Promise.resolve({ name: d && d.name, state: "denied", onchange: null }); } };
  nav.mediaDevices = { enumerateDevices: function () { return Promise.resolve([]); },
    getUserMedia: function () { return Promise.reject(new DOMException("No devices", "NotFoundError")); } };
  nav.storage = { estimate: function () { return Promise.resolve({ quota: 0, usage: 0 }); },
    persist: function () { return Promise.resolve(false); }, persisted: function () { return Promise.resolve(false); } };
  nav.share = function () { return Promise.reject(new DOMException("Sharing is not supported", "NotAllowedError")); };
  nav.canShare = function () { return false; };
  nav.vibrate = function () { return false; };
  nav.javaEnabled = function () { return false; };
  nav.registerProtocolHandler = function () {};
  nav.connection = { effectiveType: "3g", downlink: 1, rtt: 300, saveData: false, addEventListener: function () {} };
  nav.locks = { request: function (n, o, f) { return Promise.resolve((typeof o === "function" ? o : f)({ name: n })); } };

  /* ---- other windows: proxies by id (browser.c carries what they do), and messages ---- */
  var winop = g["%winop"], proxies = new Map(), selfId = winop(0, "self");
  function cloneError() { return new DOMException("The object could not be cloned.", "DataCloneError"); }
  function crossOrigin() { return new DOMException("Blocked a frame from accessing a cross-origin frame.", "SecurityError"); }
  var bytes = g["%b64"], unbytes = g["%unb64"];
  /* The structured clone algorithm's types, as JSON (every object wrapped, so its keys are its own). */
  function serialize(v) {
    var seen = new Map(), next = 0;
    function enc(x) {
      switch (typeof x) {
      case "undefined": return { $: "u" };
      case "number": return x !== x || x === Infinity || x === -Infinity || (x === 0 && 1 / x < 0) ? { $: "n", v: x === 0 ? "-0" : String(x) } : x;
      case "bigint": return { $: "b", v: String(x) };
      case "string": case "boolean": return x;
      case "symbol": case "function": throw cloneError();
      }
      if (x === null) return null;
      if (seen.has(x)) return { $: "r", i: seen.get(x) };
      var id = next++;
      seen.set(x, id);
      if (x instanceof Date) return { $: "d", i: id, v: x.getTime() };
      if (x instanceof RegExp) return { $: "re", i: id, s: x.source, f: x.flags };
      if (x instanceof ArrayBuffer) return { $: "ab", i: id, v: bytes(new Uint8Array(x)) };
      if (ArrayBuffer.isView(x))
        return { $: "ta", i: id, t: x instanceof DataView ? "DataView" : x.constructor.name,
                 v: bytes(new Uint8Array(x.buffer, x.byteOffset, x.byteLength)) };
      if (x instanceof Map) return { $: "m", i: id, v: Array.from(x, function (e) { return [enc(e[0]), enc(e[1])]; }) };
      if (x instanceof Set) return { $: "s", i: id, v: Array.from(x, enc) };
      if (x instanceof Error) return { $: "e", i: id, n: x.name, m: x.message, s: x.stack };
      if (x._bytes instanceof Uint8Array && x instanceof Blob)
        return x instanceof File ? { $: "f", i: id, t: x.type, n: x.name, l: x.lastModified, v: bytes(x._bytes) }
                                 : { $: "bl", i: id, t: x.type, v: bytes(x._bytes) };
      if (Array.isArray(x)) { var a = []; for (var k = 0; k < x.length; k++) a.push(enc(x[k])); return { $: "a", i: id, v: a }; }
      if (typeof Node === "function" && x instanceof Node || x === g || typeof x.postMessage === "function" && proxiesHas(x))
        throw cloneError();
      var o = {};
      Object.keys(x).forEach(function (key) { o[key] = enc(x[key]); });
      return { $: "o", i: id, v: o };
    }
    return JSON.stringify(enc(v));
  }
  function proxiesHas(x) { var found = false; proxies.forEach(function (p) { if (p === x) found = true; }); return found; }
  function deserialize(text) {
    var refs = [];
    function dec(x) {
      if (x === null || typeof x !== "object") return x;
      switch (x.$) {
      case "u": return undefined;
      case "n": return x.v === "-0" ? -0 : Number(x.v);
      case "b": return BigInt(x.v);
      case "r": return refs[x.i];
      case "d": return refs[x.i] = new Date(x.v);
      case "re": return refs[x.i] = new RegExp(x.s, x.f);
      case "ab": return refs[x.i] = unbytes(x.v).buffer;
      case "ta": {
        var b = unbytes(x.v), C = x.t === "DataView" ? DataView : g[x.t] || Uint8Array;
        return refs[x.i] = C === DataView ? new DataView(b.buffer) : new C(b.buffer, 0, b.length / C.BYTES_PER_ELEMENT);
      }
      case "m": { var m = refs[x.i] = new Map(); x.v.forEach(function (e) { m.set(dec(e[0]), dec(e[1])); }); return m; }
      case "s": { var st = refs[x.i] = new Set(); x.v.forEach(function (e) { st.add(dec(e)); }); return st; }
      case "bl": return refs[x.i] = new Blob([unbytes(x.v)], { type: x.t });
      case "f": return refs[x.i] = new File([unbytes(x.v)], x.n, { type: x.t, lastModified: x.l });
      case "e": { var E = g[x.n] && g[x.n].prototype instanceof Error ? g[x.n] : Error, err = refs[x.i] = new E(x.m); if (x.s) err.stack = x.s; return err; }
      case "a": { var arr = refs[x.i] = []; for (var k = 0; k < x.v.length; k++) arr[k] = dec(x.v[k]); return arr; }
      case "o": { var o = refs[x.i] = {}; for (var key in x.v) o[key] = dec(x.v[key]); return o; }
      }
      return x;
    }
    return dec(JSON.parse(text));
  }
  g["%serialize"] = serialize;
  g["%deserialize"] = deserialize;
  function post(id, data, origin) {
    if (origin && typeof origin === "object") origin = origin.targetOrigin;
    winop(id, "post", serialize(data), origin === undefined ? "/" : String(origin));
  }
  function proxy(id) {
    if (!id || id === selfId) return g;
    var p = proxies.get(id);
    if (p) return p;
    p = {
      postMessage: function (data, origin) { post(id, data, origin); },
      close: function () { winop(id, "close"); },
      focus: function () {}, blur: function () {},
      get closed() { return !winop(id, "alive"); },
      get opener() { var w = winop(id, "opener"); return w ? proxy(w) : null; },
      get parent() { var w = winop(id, "parent"); return w ? proxy(w) : p; },
      get top() { var w = winop(id, "top"); return w ? proxy(w) : p; },
      get window() { return p; }, get self() { return p; }, get frames() { return p; },
      get length() { return winop(id, "frames"); },
      get document() { if (winop(id, "href") === null) throw crossOrigin(); return null; },
      get location() {
        var href = winop(id, "href");
        if (href === null) throw crossOrigin();
        var u = new URL(href);
        return { href: href, origin: u.origin, protocol: u.protocol, host: u.host, hostname: u.hostname, port: u.port,
                 pathname: u.pathname, search: u.search, hash: u.hash, toString: function () { return href; },
                 assign: function (to) { winop(id, "navigate", String(to)); },
                 replace: function (to) { winop(id, "navigate", String(to)); },
                 reload: function () { winop(id, "navigate", href); } };
      },
      set location(to) { winop(id, "navigate", String(to)); }
    };
    proxies.set(id, p);
    return p;
  }
  g["%windowProxy"] = proxy;
  Object.defineProperty(g, "opener", { configurable: true, enumerable: true,
    get: function () { var w = winop(0, "opener"); return w ? proxy(w) : null; }, set: function () {} });
  Object.defineProperty(g, "parent", { configurable: true, enumerable: true,
    get: function () { var w = winop(0, "parent"); return w ? proxy(w) : g; } });
  Object.defineProperty(g, "top", { configurable: true, enumerable: true,
    get: function () { var w = winop(0, "top"); return w ? proxy(w) : g; } });
  Object.defineProperty(g, "length", { configurable: true, get: function () { return winop(0, "frames"); } });
  def(g, "postMessage", function (data, origin) { post(selfId, data, origin); });
  g["%deliverMessage"] = function (data, origin, source) {
    var e = new MessageEvent("message", { data: deserialize(data), origin: origin, source: proxy(source), lastEventId: "", ports: [] });
    g.dispatchEvent(e);
  };

  /* ---- Worker: a dedicated worker, a process of its own (jsworker.c); made the first time it is named ---- */
  var workers = new Map(), workerClass = null;
  function makeWorker() {
    var op = g["%worker"];
    class Worker extends EventTarget {
      constructor(url, options) {
        super();
        options = options === undefined || options === null ? {} : options;
        var type = options.type === undefined ? "classic" : String(options.type);
        if (type !== "classic" && type !== "module") throw new TypeError("A worker type is classic or module");
        var id = op("start", String(url), type, options.name === undefined ? "" : String(options.name));
        if (id === -1) throw new DOMException("A worker script must have the page origin", "SecurityError");
        def(this, "_id", id > 0 ? id : 0);
        this.onmessage = null;
        this.onmessageerror = null;
        this.onerror = null;
        var self = this;
        if (id > 0) workers.set(id, this);
        else setTimeout(function () { self.dispatchEvent(new Event("error")); }, 0);
      }
      postMessage(data, transfer) {
        var text = serialize(data);
        if (this._id) op("post", this._id, text);
      }
      terminate() {
        if (!this._id) return;
        op("terminate", this._id);
        workers.delete(this._id);
        this._id = 0;
      }
    }
    return Worker;
  }
  Object.defineProperty(g, "Worker", { configurable: true, enumerable: false,
    get: function () { return workerClass || (workerClass = makeWorker()); },
    set: function (v) { Object.defineProperty(g, "Worker", { value: v, writable: true, configurable: true }); } });
  g["%workerEvent"] = function (id, kind, data) {
    var w = workers.get(id);
    if (!w) return;
    if (kind === "message") {
      var v;
      try { v = deserialize(data); }
      catch (e) { w.dispatchEvent(new MessageEvent("messageerror", { data: null })); return; }
      w.dispatchEvent(new MessageEvent("message", { data: v, origin: "", lastEventId: "", source: null, ports: [] }));
    } else if (kind === "error") {
      w.dispatchEvent(new ErrorEvent("error", { message: data, filename: "", lineno: 0, colno: 0, error: null, cancelable: true }));
    } else {
      workers.delete(id);
      if (kind === "fail") w.dispatchEvent(new Event("error"));
    }
  };
  Object.defineProperty(Element.prototype, "contentWindow", { configurable: true, get: function () {
    if (this.tagName !== "IFRAME" && this.tagName !== "FRAME") return undefined;
    var w = winop(0, "frame", this);
    return w ? proxy(w) : null;
  } });
  Object.defineProperty(Element.prototype, "contentDocument", { configurable: true, get: function () {
    return this.tagName === "IFRAME" || this.tagName === "FRAME" ? null : undefined;
  } });

  g.isSecureContext = location.protocol === "https:" || location.hostname === "localhost";
  g.crossOriginIsolated = false;
  g.origin = location.origin;
  g.visualViewport = { get width() { return innerWidth; }, get height() { return innerHeight; }, scale: 1,
    offsetLeft: 0, offsetTop: 0, get pageLeft() { return scrollX; }, get pageTop() { return scrollY; },
    addEventListener: function () {}, removeEventListener: function () {} };
  def(g, "reportError", function (e) { console.error(e); });
  def(g, "find", function () { return false; });
  def(g, "getScreenDetails", function () { return Promise.reject(new DOMException("Not allowed", "NotAllowedError")); });

  /* What this computer has no way to give a page: the answers a browser without it gives. */
  nav.geolocation = {
    getCurrentPosition: function (ok, fail) {
      if (typeof fail === "function")
        setTimeout(function () { fail({ code: 2, message: "Position unavailable", PERMISSION_DENIED: 1, POSITION_UNAVAILABLE: 2, TIMEOUT: 3 }); }, 0);
    },
    watchPosition: function (ok, fail) { this.getCurrentPosition(ok, fail); return 1; },
    clearWatch: function () {}
  };
  nav.credentials = {
    get: function () { return Promise.resolve(null); },
    store: function (c) { return Promise.resolve(c); },
    create: function () { return Promise.reject(new DOMException("Credentials are not supported", "NotSupportedError")); },
    preventSilentAccess: function () { return Promise.resolve(); }
  };
  nav.getBattery = function () {
    return Promise.resolve({ charging: true, chargingTime: 0, dischargingTime: Infinity, level: 1,
      onchargingchange: null, onchargingtimechange: null, ondischargingtimechange: null, onlevelchange: null,
      addEventListener: function () {}, removeEventListener: function () {}, dispatchEvent: function () { return true; } });
  };
  nav.getGamepads = function () { return [null, null, null, null]; };
  if (g.screen && !g.screen.orientation)
    g.screen.orientation = {
      get type() { return innerWidth >= innerHeight ? "landscape-primary" : "portrait-primary"; }, angle: 0, onchange: null,
      lock: function () { var p = Promise.reject(new DOMException("The screen cannot be turned", "NotSupportedError")); p.catch(function () {}); return p; },
      unlock: function () {}, addEventListener: function () {}, removeEventListener: function () {},
      dispatchEvent: function () { return true; }
    };

  /* <video> and <audio>: nothing plays here (a click on one downloads its file), so play() fails
   * as it does for a format a browser cannot play, and the rest reads as an element with no media. */
  var mediaProtos = [g.HTMLMediaElement, g.HTMLAudioElement, g.HTMLVideoElement].filter(function (c) { return c; })
    .map(function (c) { return c.prototype; });
  mediaProtos.forEach(function (MP) {
    var notSupported = function () { return new DOMException("This browser cannot play media", "NotSupportedError"); };
    def(MP, "play", function () {
      var el = this;
      setTimeout(function () { el.dispatchEvent(new Event("error")); }, 0);
      var p = Promise.reject(notSupported());
      return p;
    });
    def(MP, "pause", function () {});
    def(MP, "load", function () { var el = this; setTimeout(function () { el.dispatchEvent(new Event("emptied")); }, 0); });
    def(MP, "canPlayType", function () { return ""; });
    def(MP, "fastSeek", function () {});
    def(MP, "addTextTrack", function () { return { mode: "disabled", cues: [], addCue: function () {}, removeCue: function () {} }; });
    def(MP, "setSinkId", function () { return Promise.reject(notSupported()); });
    def(MP, "captureStream", function () { throw notSupported(); });
    var EMPTY_RANGES = { length: 0, start: function () { throw new DOMException("Index out of range", "IndexSizeError"); },
                         end: function () { throw new DOMException("Index out of range", "IndexSizeError"); } };
    var media = { paused: true, ended: false, seeking: false, duration: NaN, readyState: 0, networkState: 3,
                  videoWidth: 0, videoHeight: 0, buffered: EMPTY_RANGES, played: EMPTY_RANGES, seekable: EMPTY_RANGES,
                  textTracks: [], audioTracks: [], videoTracks: [], currentSrc: "", srcObject: null,
                  error: { code: 4, message: "This browser cannot play media", MEDIA_ERR_SRC_NOT_SUPPORTED: 4 } };
    Object.keys(media).forEach(function (k) {
      Object.defineProperty(MP, k, { configurable: true, get: function () { return this["%m_" + k] !== undefined ? this["%m_" + k] : media[k]; },
                                     set: function (v) { def(this, "%m_" + k, v); } });
    });
    var settable = { currentTime: 0, volume: 1, muted: false, playbackRate: 1, defaultPlaybackRate: 1, preservesPitch: true };
    Object.keys(settable).forEach(function (k) {
      Object.defineProperty(MP, k, { configurable: true,
        get: function () { var v = this["%m_" + k]; return v === undefined ? settable[k] : v; },
        set: function (v) { def(this, "%m_" + k, typeof settable[k] === "boolean" ? !!v : Number(v)); } });
    });
    [["NETWORK_EMPTY", 0], ["NETWORK_IDLE", 1], ["NETWORK_LOADING", 2], ["NETWORK_NO_SOURCE", 3], ["HAVE_NOTHING", 0],
     ["HAVE_METADATA", 1], ["HAVE_CURRENT_DATA", 2], ["HAVE_FUTURE_DATA", 3], ["HAVE_ENOUGH_DATA", 4]].forEach(function (c) {
      def(MP, c[0], c[1]);
      def(MP.constructor, c[0], c[1]);
    });
  });
  if (g.HTMLAudioElement) {
    var Audio = function Audio(src) {
      var a = document.createElement("audio");
      a.setAttribute("preload", "auto");
      if (src !== undefined) a.setAttribute("src", String(src));
      return a;
    };
    Audio.prototype = g.HTMLAudioElement.prototype;
    def(g, "Audio", Audio);
  }

  /* input.files: the file the user picked, as a File in a FileList */
  if (g.HTMLInputElement)
    Object.defineProperty(g.HTMLInputElement.prototype, "files", { configurable: true, get: function () {
      if (String(this.type).toLowerCase() !== "file") return null;
      var F = g.File, picked = g["%chosenFile"](this), files = [];
      if (picked) files.push(new F([picked.bytes], picked.name, { type: picked.type, lastModified: picked.lastModified }));
      return g["%fileList"](files);
    }, set: function () {} });

  /* Document.parseHTMLUnsafe and Document.parseHTML (sanitized): the DOM library does the work */
  if (g.Document) {
    def(g.Document, "parseHTMLUnsafe", function (html, options) { return g.Sanitizer && g["%parseHTML"](html, options, false); });
    def(g.Document, "parseHTML", function (html, options) { return g.Sanitizer && g["%parseHTML"](html, options, true); });
  }
  /* ---- drag and drop: a DataTransfer is what a drag carries (the browser makes one for each drag) ---- */
  var dropKinds = ["none", "copy", "link", "move"];
  var allowedKinds = ["none", "copy", "copyLink", "copyMove", "link", "linkMove", "move", "all", "uninitialized"];
  function dragType(t) { t = String(t).toLowerCase(); return t === "text" ? "text/plain" : t === "url" ? "text/uri-list" : t; }
  class DataTransferItem {
    constructor(dt, kind, type, data) { def(this, "_dt", dt); def(this, "_data", data); this.kind = kind; this.type = type; }
    getAsString(callback) {
      if (this.kind !== "string" || typeof callback !== "function" || this._dt._mode === "protected") return;
      var v = this._data;
      setTimeout(function () { callback(v); }, 0);
    }
    getAsFile() { return this.kind === "file" && this._dt._mode !== "protected" ? this._data : null; }
  }
  class DataTransferItemList {
    constructor(dt) { def(this, "_dt", dt); def(this, "_n", 0); }
    get length() { return this._dt._items.length; }
    add(data, type) {
      var dt = this._dt, it;
      if (dt._mode !== "readwrite") return null;
      if (g.File && data instanceof g.File) {
        it = new DataTransferItem(dt, "file", data.type, data);
      } else {
        type = dragType(type);
        if (dt._items.some(function (i) { return i.kind === "string" && i.type === type; }))
          throw new DOMException("The drag already has data of type " + type, "NotSupportedError");
        it = new DataTransferItem(dt, "string", type, String(data));
      }
      dt._items.push(it);
      this._sync();
      return it;
    }
    remove(i) { if (this._dt._mode === "readwrite") { this._dt._items.splice(i, 1); this._sync(); } }
    clear() { if (this._dt._mode === "readwrite") { this._dt._items.length = 0; this._sync(); } }
    _sync() {
      var items = this._dt._items;
      for (var i = 0; i < this._n; i++) delete this[i];
      for (var k = 0; k < items.length; k++)
        Object.defineProperty(this, k, { value: items[k], configurable: true, enumerable: true });
      this._n = items.length;
    }
    [Symbol.iterator]() { return this._dt._items.slice()[Symbol.iterator](); }
  }
  class DataTransfer {
    constructor() {
      def(this, "_items", []);
      def(this, "_mode", "readwrite");
      def(this, "_drop", "none");
      def(this, "_allowed", "uninitialized");
      def(this, "_list", new DataTransferItemList(this));
    }
    get dropEffect() { return this._drop; }
    set dropEffect(v) { if (dropKinds.indexOf(v) >= 0) this._drop = v; }
    get effectAllowed() { return this._allowed; }
    set effectAllowed(v) { if (this._mode === "readwrite" && allowedKinds.indexOf(v) >= 0) this._allowed = v; }
    get items() { return this._list; }
    get types() {
      var t = [], files = false;
      this._items.forEach(function (i) { if (i.kind === "file") files = true; else t.push(i.type); });
      if (files) t.push("Files");
      return Object.freeze(t);
    }
    get files() {
      var f = this._mode === "protected" ? [] : this._items.filter(function (i) { return i.kind === "file"; })
        .map(function (i) { return i._data; });
      return g["%fileList"] ? g["%fileList"](f) : f;
    }
    getData(format) {
      if (this._mode === "protected") return "";
      var asked = String(format).toLowerCase(), type = dragType(asked);
      var it = this._items.find(function (i) { return i.kind === "string" && i.type === type; });
      if (!it) return "";
      if (asked !== "url") return it._data;
      var lines = it._data.split("\n").map(function (l) { return l.trim(); });
      return lines.find(function (l) { return l && l.charAt(0) !== "#"; }) || "";
    }
    setData(format, data) {
      if (this._mode !== "readwrite") return;
      var type = dragType(format), items = this._items;
      for (var i = items.length - 1; i >= 0; i--)
        if (items[i].kind === "string" && items[i].type === type) items.splice(i, 1);
      items.push(new DataTransferItem(this, "string", type, String(data)));
      this._list._sync();
    }
    clearData(format) {
      if (this._mode !== "readwrite") return;
      var type = format === undefined ? null : dragType(format), items = this._items;
      for (var i = items.length - 1; i >= 0; i--)
        if (items[i].kind === "string" && (type === null || items[i].type === type)) items.splice(i, 1);
      this._list._sync();
    }
    setDragImage(image, x, y) {}
  }
  def(g, "DataTransfer", DataTransfer);
  def(g, "DataTransferItem", DataTransferItem);
  def(g, "DataTransferItemList", DataTransferItemList);
  /* The browser side of a drag (jsdom.c): "new" makes its DataTransfer with the text and URL dragged;
   * "prepare" sets what an event of a type may see and the dropEffect it starts with (from effectAllowed,
   * or the operation chosen so far for drop and dragend); "effect" is the operation the last dragenter
   * or dragover chose (none if effectAllowed does not allow it); "text" is the text to drop into a field. */
  def(g, "%drag", function (op, dt, a, b, c) {
    if (op === "new") {
      var d = new DataTransfer();
      if (dt) d.setData("text/uri-list", dt);
      if (a) d.setData("text/plain", a);
      return d;
    }
    var allowed = dt._allowed;
    if (op === "prepare") {
      dt._mode = a === "dragstart" ? "readwrite" : a === "drop" ? "readonly" : "protected";
      if (a === "dragenter" || a === "dragover")
        dt._drop = allowed === "none" ? "none" : allowed === "move" ? "move" : allowed.indexOf("link") === 0 ||
          (allowed === "uninitialized" && b) ? "link" : "copy";
      else
        dt._drop = a === "drop" || a === "dragend" ? String(c || "none") : "none";
      return null;
    }
    if (op === "effect") {
      var e = dt._drop;
      return e !== "none" && (allowed === "all" || allowed === "uninitialized" || allowed.toLowerCase().indexOf(e) >= 0)
        ? e : "none";
    }
    if (op === "text") {
      var t = dt._items.find(function (i) { return i.kind === "string" && i.type === "text/plain"; }) ||
        dt._items.find(function (i) { return i.kind === "string" && i.type === "text/uri-list"; });
      return t ? t._data : "";
    }
    return null;
  });
})(globalThis);
);
