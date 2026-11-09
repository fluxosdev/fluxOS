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

  /* Message tasks stay responsive while hidden; worker scopes use their ordinary task timer. */
  var queueMessageTask=g["%queueTask"] || function(fn){setTimeout(fn,0);};
  /* ---- MessageChannel, BroadcastChannel (within the page), XMLSerializer, Path2D, ImageData ---- */
  class MessagePort extends EventTarget {
    constructor() {
      super();
      def(this, "_other", null);
      def(this, "_closed", false);
      this.onmessage = null;
    }
    postMessage(data, transfer) {
      var o = this._other;
      if (!o || this._closed) return;
      var copy = structuredClone(data,{transfer:transfer && transfer.transfer || transfer || []});
      queueMessageTask(function () {
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
      if(g["%broadcast"])g["%broadcast"](this.name,g["%serialize"](data));
      channels.slice().forEach(function (c) {
        if (c === self || c._closed || c.name !== self.name) return;
        queueMessageTask(function () {
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
  def(g,"%deliverBroadcast",function(data,origin,name){
    channels.slice().forEach(function(channel){
      if(!channel._closed && channel.name===name)
        channel.dispatchEvent(new MessageEvent("message",{data:g["%deserialize"](data),origin:origin}));
    });
  });
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

  /* ---- view transitions: document.startViewTransition(); the browser takes the pictures and draws
   * the frames (%vtCapture, %vtBegin, %vtEnd), and says when they are over (%vtDone) ---- */
  var activeVT = null;
  function vtQuiet(p) { p.catch(function () {}); return p; }
  function vtCall(name, arg) { var f = g[name]; return typeof f === "function" ? f(arg) : 0; }
  class ViewTransition {
    constructor(types) {
      var s = { phase: "pending", types: new Set(types) };
      def(this, "%s", s);
      def(this, "updateCallbackDone", vtQuiet(new Promise(function (ok, no) { s.updateOk = ok; s.updateNo = no; })));
      def(this, "ready", vtQuiet(new Promise(function (ok, no) { s.readyOk = ok; s.readyNo = no; })));
      def(this, "finished", vtQuiet(new Promise(function (ok, no) { s.finishOk = ok; s.finishNo = no; })));
    }
    get types() { return this["%s"].types; }
    get transitionRoot() { return document.documentElement; }
    skipTransition() { vtSkip(this, new DOMException("Skipped ViewTransition due to skipTransition() call", "AbortError")); }
  }
  /* Skipped: no animation (or no more of it); finished follows the update callback, which still runs. */
  function vtSkip(vt, reason) {
    var s = vt["%s"];
    if (s.phase === "done" || s.phase === "skipped") return;
    var animating = s.phase === "animating";
    s.phase = animating ? "done" : "skipped";
    if (activeVT === vt) { activeVT = null; vtCall("%vtEnd"); }
    if (!animating) s.readyNo(reason);
    vt.updateCallbackDone.then(function () { s.finishOk(); }, function (e) { s.finishNo(e); });
  }
  function vtRun(vt, cb) {
    var s = vt["%s"], p;
    if (s.phase === "pending") s.phase = "update";
    try { p = Promise.resolve(cb ? cb() : undefined); } catch (e) { p = Promise.reject(e); }
    var timer = setTimeout(function () {
      if (s.phase === "update") vtSkip(vt, new DOMException("Transition was aborted because of timeout in DOM update", "TimeoutError"));
    }, 4000);
    p.then(function () {
      clearTimeout(timer);
      s.updateOk();
      if (s.phase !== "update") return;
      var r = vtCall("%vtBegin");
      if (r <= 0) {
        vtSkip(vt, new DOMException(r < 0 ? "Transition was aborted because of duplicate view-transition-name" :
                                    "Transition was skipped", "InvalidStateError"));
        return;
      }
      s.phase = "animating";
      s.readyOk();
    }, function (e) {
      clearTimeout(timer);
      s.updateNo(e);
      vtSkip(vt, e);
    });
  }
  def(DP, "startViewTransition", function (arg) {
    var cb = typeof arg === "function" ? arg : arg && typeof arg === "object" && typeof arg.update === "function" ? arg.update : null;
    var types = arg && typeof arg === "object" && arg.types ? Array.from(arg.types, String) : [];
    var vt = new ViewTransition(types);
    if (activeVT) vtSkip(activeVT, new DOMException("Old view transition aborted by new view transition.", "AbortError"));
    activeVT = vt;
    var r = document.visibilityState === "hidden" ? 0 : vtCall("%vtCapture", types.join(" "));
    if (r <= 0)
      vtSkip(vt, new DOMException(r < 0 ? "Transition was aborted because of duplicate view-transition-name" :
                                  "Transition was skipped", "InvalidStateError"));
    Promise.resolve().then(function () { vtRun(vt, cb); });
    return vt;
  });
  getter(DP, "activeViewTransition", function () { return activeVT; });
  def(g, "ViewTransition", ViewTransition);
  def(g, "%vtDone", function () {
    var vt = activeVT;
    if (!vt || vt["%s"].phase !== "animating") return;
    activeVT = null;
    vt["%s"].phase = "done";
    vt["%s"].finishOk();
  });
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

  /* ---- Media Source Extensions: what a page appends, the media engine plays ----
   * A MediaSource is attached to a media element through a blob: URL (createObjectURL) or
   * srcObject; its SourceBuffers send what is appended to the engine (%mediaCmd), which parses it
   * at once and answers with the buffered spans when it has used it all up (%mediaMsg brings the
   * answers here). The element's readyState follows what is buffered where it is. */
  if (g.document && g["%mediaCmd"]) {
    var mseCmd = g["%mediaCmd"], mseState = g["%mstate"], mseFire = g["%mfire"];
    var mseSecret = {}, mseURLs = {}, mseCount = 0;
    var mseTask = function (target, type) { setTimeout(function () { target.dispatchEvent(new Event(type)); }, 0); };
    var mseInvalid = function (what) { return new DOMException(what, "InvalidStateError"); };
    /* What the engine decodes, at sizes and rates a Pentium 4 keeps up with: H.264 (and VP8) up to
     * 1280x720 at 30 frames, AAC, MP3, Opus and Vorbis. VP9 is decoded too, but sites that are
     * told no pick H.264, which is lighter. */
    var mseSupported = function (type) {
      var t = String(type).toLowerCase(), parts = t.split(";"), base = parts[0].trim();
      var codecs = null, width = 0, height = 0, rate = 0;
      for (var i = 1; i < parts.length; i++) {
        var kv = parts[i].split("="), k = kv[0].trim(), v = kv.slice(1).join("=").split(String.fromCharCode(34)).join("").trim();
        if (k === "codecs") codecs = v.split(",").map(function (c) { return c.trim(); }).filter(function (c) { return c; });
        else if (k === "width") width = Number(v);
        else if (k === "height") height = Number(v);
        else if (k === "framerate") rate = Number(v);
      }
      if (width > 1280 || height > 720 || rate > 31) return false;
      var video = base.indexOf("video/") === 0, list;
      if (base === "video/mp4" || base === "audio/mp4") list = ["avc1", "avc3", "mp4a.40", "mp4a.69", "mp4a.6b", "mp3", "opus", "flac"];
      else if (base === "video/webm" || base === "audio/webm") list = ["vp8", "opus", "vorbis"];
      else return false;
      if (!codecs) return true;
      if (!codecs.length) return false;
      return codecs.every(function (c) {
        if (!video && (c.indexOf("avc") === 0 || c.indexOf("vp") === 0)) return false;
        return list.some(function (p) { return c.indexOf(p) === 0; });
      });
    };
    var mseIntersect = function (a, b) {
      var out = [], i = 0, j = 0;
      while (i < a.length && j < b.length) {
        var s = Math.max(a[i][0], b[j][0]), e = Math.min(a[i][1], b[j][1]);
        if (s < e) out.push([s, e]);
        if (a[i][1] < b[j][1]) i++; else j++;
      }
      return out;
    };

    class SourceBufferList extends EventTarget {
      constructor(token) {
        if (token !== mseSecret) throw new TypeError("Illegal constructor");
        super();
        def(this, "_list", []);
        this.onaddsourcebuffer = null;
        this.onremovesourcebuffer = null;
      }
      get length() { return this._list.length; }
      _sync() {
        for (var i = 0; i < 8; i++) {
          if (i < this._list.length) Object.defineProperty(this, i, { configurable: true, enumerable: true, value: this._list[i] });
          else delete this[i];
        }
      }
      _add(sb, quiet) {
        this._list.push(sb);
        this._sync();
        if (!quiet) mseTask(this, "addsourcebuffer");
      }
      _remove(sb, quiet) {
        var i = this._list.indexOf(sb);
        if (i < 0) return;
        this._list.splice(i, 1);
        this._sync();
        if (!quiet) mseTask(this, "removesourcebuffer");
      }
      [Symbol.iterator]() { return this._list.slice()[Symbol.iterator](); }
    }

    class SourceBuffer extends EventTarget {
      constructor(token) {
        if (token !== mseSecret) throw new TypeError("Illegal constructor");
        super();
        def(this, "_ms", null);
        def(this, "_id", 0);
        def(this, "_type", "");
        def(this, "_updating", false);
        def(this, "_removed", false);
        def(this, "_ranges", []);
        def(this, "_seq", 0);
        def(this, "_init", false);
        def(this, "_offset", 0);
        def(this, "_mode", "segments");
        def(this, "_winStart", 0);
        def(this, "_winEnd", Infinity);
        this.onupdatestart = null;
        this.onupdate = null;
        this.onupdateend = null;
        this.onerror = null;
        this.onabort = null;
      }
      _check() {
        if (this._removed) throw mseInvalid("This SourceBuffer has been removed from its MediaSource");
        if (this._updating) throw mseInvalid("This SourceBuffer is still processing an append or remove");
      }
      get updating() { return this._updating; }
      get buffered() {
        if (this._removed) throw mseInvalid("This SourceBuffer has been removed from its MediaSource");
        return g["%timeRanges"](this._ranges.map(function (r) { return r.slice(); }));
      }
      get mode() { return this._mode; }
      set mode(v) {
        v = String(v);
        if (v !== "segments" && v !== "sequence") return;
        this._check();
        this._mode = v;
      }
      get timestampOffset() { return this._offset; }
      set timestampOffset(v) {
        var x = Number(v);
        if (!isFinite(x)) throw new TypeError("The provided double value is non-finite.");
        this._check();
        if (this._ms._state === "ended") this._ms._reopen();
        this._offset = x;
        mseCmd(this._ms._el, "sboffset", String(Math.round(x * 1e6)), this._id, 0);
      }
      get appendWindowStart() { return this._winStart; }
      set appendWindowStart(v) { this._check(); var x = Number(v); if (!(x >= 0) || x >= this._winEnd) throw new TypeError("appendWindowStart is out of range"); this._winStart = x; }
      get appendWindowEnd() { return this._winEnd; }
      set appendWindowEnd(v) { this._check(); var x = Number(v); if (isNaN(x) || x <= this._winStart) throw new TypeError("appendWindowEnd is out of range"); this._winEnd = x; }
      get audioTracks() { return []; }
      get videoTracks() { return []; }
      get textTracks() { return []; }
      appendBuffer(data) {
        this._check();
        var view = data instanceof ArrayBuffer ? new Uint8Array(data) : ArrayBuffer.isView(data) ? data : null;
        if (!view) throw new TypeError("appendBuffer requires an ArrayBuffer or ArrayBufferView");
        var el = this._ms._el, st = mseState(el);
        if (st.error) throw mseInvalid("The media element has an error");
        if (this._ms._state === "ended") this._ms._reopen();
        var seq = ++this._seq;
        this._updating = true;
        mseTask(this, "updatestart");
        if (!view.byteLength) {         /* nothing to parse: done at once */
          var self = this;
          setTimeout(function () { self._done(seq, self._ranges); }, 0);
          return;
        }
        mseCmd(el, "sbappend", view, this._id, seq);
      }
      remove(start, end) {
        this._check();
        var ms = this._ms, s = Number(start), e = Number(end);
        if (isNaN(ms._duration) || !(s >= 0) || s > ms._duration || !(e > s)) throw new TypeError("The remove range is not valid");
        if (ms._state === "ended") ms._reopen();
        var seq = ++this._seq;
        this._updating = true;
        mseTask(this, "updatestart");
        mseCmd(ms._el, "sbremove", Math.round(s * 1e6) + " " + (e === Infinity ? "9223372036854775807" : String(Math.round(e * 1e6))), this._id, seq);
      }
      abort() {
        if (this._removed) throw mseInvalid("This SourceBuffer has been removed from its MediaSource");
        if (this._ms._state !== "open") throw mseInvalid("The MediaSource is not open");
        this._seq++;                    /* what the engine still says of the last append is stale */
        if (this._updating) {
          this._updating = false;
          mseTask(this, "abort");
          mseTask(this, "updateend");
        }
        mseCmd(this._ms._el, "sbabort", "", this._id, 0);
        this._winStart = 0;
        this._winEnd = Infinity;
      }
      changeType(type) {
        if (!type) throw new TypeError("The type is empty");
        this._check();
        if (!mseSupported(type)) throw new DOMException("The type is not supported", "NotSupportedError");
        this._type = String(type);
        mseCmd(this._ms._el, "sbabort", "", this._id, 0);
      }
      /* the engine's answers */
      _done(seq, ranges) {
        if (seq !== this._seq || this._removed) return;
        this._ranges = ranges;
        this._updating = false;
        this._ms._update();              /* readyState moves with the coded frames, before updateend */
        this.dispatchEvent(new Event("update"));
        this.dispatchEvent(new Event("updateend"));
      }
      _error(seq) {
        if (seq !== this._seq || this._removed) return;
        this._updating = false;
        this.dispatchEvent(new Event("error"));
        this.dispatchEvent(new Event("updateend"));
        if (this._ms._state === "open") this._ms.endOfStream("decode");
      }
      _initSegment(v, a, w, h) {
        var ms = this._ms, el = ms._el, st = mseState(el);
        if (!v && !a) {
          this._error(this._seq);
          return;
        }
        this._init = true;
        if (ms._active._list.indexOf(this) < 0) ms._active._add(this);
        if (v && w > 0 && (st.vw !== w || st.vh !== h)) {
          var known = st.vw > 0;
          st.vw = w;
          st.vh = h;
          if (known) mseFire(el, "resize");
        }
        if (isNaN(ms._duration)) ms._setDuration(Infinity);
        ms._update();
      }
    }

    class MediaSource extends EventTarget {
      constructor() {
        super();
        def(this, "%mse", true);
        def(this, "_sbs", new SourceBufferList(mseSecret));
        def(this, "_active", new SourceBufferList(mseSecret));
        def(this, "_state", "closed");
        def(this, "_duration", NaN);
        def(this, "_el", null);
        def(this, "_live", null);
        this.onsourceopen = null;
        this.onsourceended = null;
        this.onsourceclose = null;
      }
      static isTypeSupported(type) { return mseSupported(type); }
      static get canConstructInDedicatedWorker() { return false; }
      get sourceBuffers() { return this._sbs; }
      get activeSourceBuffers() { return this._active; }
      get readyState() { return this._state; }
      get handle() { return null; }
      get duration() { return this._state === "closed" ? NaN : this._duration; }
      set duration(v) {
        var x = Number(v);
        if (isNaN(x) || x < 0) throw new TypeError("The duration is not valid");
        if (this._state !== "open") throw mseInvalid("The MediaSource is not open");
        if (this._sbs._list.some(function (sb) { return sb._updating; })) throw mseInvalid("A SourceBuffer is updating");
        var top = 0;
        this._sbs._list.forEach(function (sb) { sb._ranges.forEach(function (r) { top = Math.max(top, r[1]); }); });
        if (x < top - 0.001) throw mseInvalid("The duration is shorter than what is buffered");
        this._setDuration(x);
      }
      _setDuration(x) {
        if (x === this._duration) return;
        this._duration = x;
        var el = this._el;
        if (el) {
          var st = mseState(el);
          st.duration = x;
          mseFire(el, "durationchange");
        }
      }
      addSourceBuffer(type) {
        if (!type) throw new TypeError("The type is empty");
        if (!mseSupported(type)) throw new DOMException("The type " + type + " is not supported", "NotSupportedError");
        if (this._state !== "open") throw mseInvalid("The MediaSource is not open");
        if (this._sbs.length >= 4) throw new DOMException("Too many SourceBuffers", "QuotaExceededError");
        var id = mseCmd(this._el, "sbadd", String(type), 0, 0);
        if (!(id > 0)) throw new DOMException("The media engine is not running", "NotSupportedError");
        var sb = new SourceBuffer(mseSecret);
        sb._ms = this;
        sb._id = id;
        sb._type = String(type);
        this._sbs._add(sb);
        return sb;
      }
      removeSourceBuffer(sb) {
        if (!(sb instanceof SourceBuffer) || this._sbs._list.indexOf(sb) < 0)
          throw new DOMException("That SourceBuffer does not belong to this MediaSource", "NotFoundError");
        if (sb._updating) {
          sb._seq++;
          sb._updating = false;
          mseTask(sb, "abort");
          mseTask(sb, "updateend");
        }
        mseCmd(this._el, "sbdel", "", sb._id, 0);
        sb._removed = true;
        this._active._remove(sb);
        this._sbs._remove(sb);
        this._update();
      }
      endOfStream(error) {
        if (this._state !== "open") throw mseInvalid("The MediaSource is not open");
        if (this._sbs._list.some(function (sb) { return sb._updating; })) throw mseInvalid("A SourceBuffer is updating");
        if (error !== undefined && error !== "network" && error !== "decode") throw new TypeError("The error is not valid");
        this._state = "ended";
        mseTask(this, "sourceended");
        var el = this._el, st = mseState(el);
        if (!error) {
          var top = 0;
          this._sbs._list.forEach(function (sb) { sb._ranges.forEach(function (r) { top = Math.max(top, r[1]); }); });
          if (top > 0 && (isNaN(this._duration) || top < this._duration || this._duration === Infinity)) this._setDuration(top);
          mseCmd(el, "mseend", "", 1, 0);
          this._update();
          return;
        }
        st.error = { code: error === "network" || st.readyState ? (error === "network" ? 2 : 3) : 4,
                     message: "The media source ended with a " + error + " error",
                     MEDIA_ERR_ABORTED: 1, MEDIA_ERR_NETWORK: 2, MEDIA_ERR_DECODE: 3, MEDIA_ERR_SRC_NOT_SUPPORTED: 4 };
        st.paused = true;
        mseTask(el, "error");
      }
      setLiveSeekableRange(start, end) {
        if (this._state !== "open") throw mseInvalid("The MediaSource is not open");
        var s = Number(start), e = Number(end);
        if (!(s >= 0) || !(e >= s)) throw new TypeError("The range is not valid");
        this._live = [s, e];
      }
      clearLiveSeekableRange() {
        if (this._state !== "open") throw mseInvalid("The MediaSource is not open");
        this._live = null;
      }
      /* the element's side */
      _attach(el) {
        this._el = el;
        this._state = "open";
        mseTask(this, "sourceopen");
      }
      _detach() {
        this._state = "closed";
        this._duration = NaN;
        var self = this;
        this._sbs._list.slice().forEach(function (sb) {
          sb._removed = true;
          self._active._remove(sb, true);
          self._sbs._remove(sb, true);
        });
        this._el = null;
        mseTask(this, "sourceclose");
      }
      _reopen() {
        this._state = "open";
        mseTask(this, "sourceopen");
        mseCmd(this._el, "mseend", "", 0, 0);
      }
      _buffered() {
        var lists = this._active._list.map(function (sb) { return sb._ranges; });
        if (!lists.length) return [];
        var ended = this._state === "ended", top = 0;
        lists.forEach(function (l) { if (l.length) top = Math.max(top, l[l.length - 1][1]); });
        var out = null;
        lists.forEach(function (l) {
          var r = l.map(function (x) { return x.slice(); });
          if (ended && r.length) r[r.length - 1][1] = top;
          out = out ? mseIntersect(out, r) : r;
        });
        return out || [];
      }
      _seekable() {
        if (this._live) return [this._live.slice()];
        var b = this._buffered();
        return b.length ? [[b[0][0], b[b.length - 1][1]]] : [];
      }
      _update() {
        var el = this._el;
        if (!el) return;
        var st = mseState(el), list = this._sbs._list;
        if (!list.length || !list.every(function (sb) { return sb._init; })) return;
        var t = el.currentTime, b = this._buffered(), rs = 1;
        for (var i = 0; i < b.length; i++)
          if (b[i][0] <= t + 0.1 && t < b[i][1]) {
            rs = b[i][1] - t >= 1 || this._state === "ended" ? 4 : 3;
            break;
          }
        if (rs === 1 && st.frame && this._state === "ended" && b.length && t >= b[b.length - 1][1] - 0.15) rs = 2;
        g["%mediaReady"](el, rs);
      }
      _message(w) {
        var id = Number(w[1]), sb = null;
        this._sbs._list.forEach(function (s) { if (s._id === id) sb = s; });
        if (!sb) return;
        if (w[0] === "sbinit") {
          sb._initSegment(Number(w[2]), Number(w[3]), Number(w[4]), Number(w[5]));
        } else if (w[0] === "sbdone") {
          var n = Number(w[3]), list = [];
          for (var i = 0; i < n; i++) list.push([Number(w[4 + 2 * i]) / 1e6, Number(w[5 + 2 * i]) / 1e6]);
          sb._done(Number(w[2]), list);
        } else if (w[0] === "sberr") {
          sb._error(Number(w[2]));
        }
      }
    }
    def(g, "MediaSource", MediaSource);
    def(g, "SourceBuffer", SourceBuffer);
    def(g, "SourceBufferList", SourceBufferList);
    def(g, "%mseURL", function (ms) {
      var hex = "";
      for (var i = 0; i < 8; i++) hex += Math.floor(Math.random() * 65536).toString(16);
      var origin = location.origin && location.origin !== "null" ? location.origin : "null";
      var u = "blob:" + origin + "/mse-" + (++mseCount) + "-" + hex;
      mseURLs[u] = ms;
      return u;
    });
    def(g, "%mseFor", function (u) { return mseURLs[u] || null; });
    def(g, "%mseRevoke", function (u) { if (!mseURLs[u]) return false; delete mseURLs[u]; return true; });
  }

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
  /* Interfaces scripts look for by name, and polyfills patch: where the objects are of one, they are
   * made so (the window, shadow roots, comments, customElements); the others stand for objects
   * that are arrays or plain objects here (NodeList, DOMTokenList...), and are there to be found. */
  var iface = function (name, parent) {
    if (g[name]) return g[name];
    var C = function () { throw new TypeError("Illegal constructor"); };
    Object.defineProperty(C, "name", { value: name });
    var base = parent && parent.prototype && typeof parent.prototype === "object" ? parent.prototype : Object.prototype;
    C.prototype = Object.create(base,
                                { constructor: { value: C, writable: true, configurable: true } });
    def(g, name, C);
    return C;
  };
  var ownerOf = function (o, k) { while (o && !Object.prototype.hasOwnProperty.call(o, k)) o = Object.getPrototypeOf(o); return o; };
  if (g.document) {
    /* Window.prototype: between the window and Object.prototype, with the window's event methods,
     * which scripts (ShadyDOM) look for there */
    var WindowIface = iface("Window", null);
    ["addEventListener", "removeEventListener", "dispatchEvent"].forEach(function (k) {
      if (typeof g[k] === "function" && !Object.prototype.hasOwnProperty.call(WindowIface.prototype, k))
        def(WindowIface.prototype, k, g[k]);
    });
    if (Object.getPrototypeOf(g) === Object.prototype) {
      try { Object.setPrototypeOf(g, WindowIface.prototype); } catch (e) {}
    }
    var ShadowRootIface = iface("ShadowRoot", g.DocumentFragment);
    var attachOwner = ownerOf(g.HTMLElement && HTMLElement.prototype, "attachShadow");
    if (attachOwner) {
      var nativeAttach = attachOwner.attachShadow;
      def(attachOwner, "attachShadow", function (init) {
        var root = nativeAttach.call(this, init);
        if (root && typeof root === "object" && Object.getPrototypeOf(root) !== ShadowRootIface.prototype)
          Object.setPrototypeOf(root, ShadowRootIface.prototype);
        return root;
      });
    }
    var CharacterDataIface = iface("CharacterData", g.Node);
    var CommentIface = iface("Comment", g.Text || CharacterDataIface);
    var createComment = document.createComment;
    if (createComment)
      def(document, "createComment", function (t) {
        var c = createComment.call(document, t);
        if (c && typeof c === "object") Object.setPrototypeOf(c, CommentIface.prototype);
        return c;
      });
    var Registry = iface("CustomElementRegistry", null);
    if (g.customElements && Object.getPrototypeOf(customElements) === Object.prototype)
      Object.setPrototypeOf(customElements, Registry.prototype);
    iface("HTMLUnknownElement", g.HTMLElement);
    iface("HTMLSlotElement", g.HTMLElement);
    iface("SVGSVGElement", g.SVGElement || g.HTMLElement);
    iface("SVGGraphicsElement", g.SVGElement || g.HTMLElement);
    iface("HTMLDocument", g.Document);
  }
  ["NodeList", "HTMLCollection", "NamedNodeMap", "DOMTokenList", "CSSStyleDeclaration", "Attr", "DOMStringMap",
   "TextTrack", "TextTrackList", "TextTrackCue", "TextTrackCueList", "AudioTrack", "AudioTrackList", "VideoTrack",
   "VideoTrackList", "MediaError", "VideoPlaybackQuality", "PictureInPictureWindow"].forEach(function (n) { iface(n, null); });
  ["DocumentType", "AbstractRange", "DOMImplementation", "HTMLFormControlsCollection", "RadioNodeList",
   "HTMLOptionsCollection", "StyleSheetList", "MediaList", "CSSRuleList", "DOMRectList", "DOMRectReadOnly",
   "DOMPointReadOnly", "DOMMatrixReadOnly", "MimeTypeArray", "PluginArray", "BarProp", "SubtleCrypto",
   "PerformanceEntry", "MediaQueryList"].forEach(function (n) { iface(n, null); });
  if (g.document) {
    iface("CDATASection", g.Text);
    iface("ProcessingInstruction", g.CharacterData);
    iface("XMLDocument", g.Document);
    /* CSSRule is the DOM library's (loaded when first used): these are plain until then */
    ["CSSStyleRule", "CSSMediaRule", "CSSKeyframesRule"].forEach(function (n) { iface(n, null); });
    ["PerformanceNavigationTiming", "PerformanceResourceTiming", "PerformanceMark", "PerformanceMeasure"].forEach(function (n) {
      iface(n, g.PerformanceEntry);
    });
  }
  /* the window's objects: of their interface, keeping all they had */
  var reproto = function (o, name) {
    if (!o || typeof o !== "object") return;
    var C = iface(name, null), was = Object.getPrototypeOf(o);
    if (was === C.prototype) return;
    var mine = Object.create(was, { constructor: { value: C, writable: true, configurable: true } });
    C.prototype = mine;
    try { Object.setPrototypeOf(o, mine); } catch (e) {}
  };
  [["history", "History"], ["location", "Location"], ["navigator", "Navigator"], ["screen", "Screen"],
   ["localStorage", "Storage"], ["crypto", "Crypto"], ["performance", "Performance"]].forEach(function (p) {
    try { reproto(g[p[0]], p[1]); } catch (e) {}
  });
  if (g.crypto && g.crypto.subtle) try { reproto(g.crypto.subtle, "SubtleCrypto"); } catch (e) {}
  if (g.sessionStorage && g.Storage) try { Object.setPrototypeOf(g.sessionStorage, g.Storage.prototype); } catch (e) {}
  if (g.document && !g.Option) {
    var Option = function Option(text, value, defaultSelected, selected) {
      var o = document.createElement("option");
      if (text !== undefined) o.textContent = String(text);
      if (value !== undefined) o.value = String(value);
      if (defaultSelected) o.setAttribute("selected", "");
      if (selected) o.selected = true;
      return o;
    };
    Option.prototype = g.HTMLOptionElement ? HTMLOptionElement.prototype : Object.prototype;
    def(g, "Option", Option);
  }
  if (g.Event) {
    if (!g.PromiseRejectionEvent) {
      class PromiseRejectionEvent extends Event {
        constructor(type, init) { super(type, init); init = init || {}; def(this, "promise", init.promise); def(this, "reason", init.reason); }
      }
      def(g, "PromiseRejectionEvent", PromiseRejectionEvent);
    }
    if (!g.MediaQueryListEvent) {
      class MediaQueryListEvent extends Event {
        constructor(type, init) { super(type, init); init = init || {}; def(this, "media", String(init.media || "")); def(this, "matches", !!init.matches); }
      }
      def(g, "MediaQueryListEvent", MediaQueryListEvent);
    }
    if (!g.SecurityPolicyViolationEvent) {
      class SecurityPolicyViolationEvent extends Event {}
      def(g, "SecurityPolicyViolationEvent", SecurityPolicyViolationEvent);
    }
  }
  if (g.MediaError)
    [["MEDIA_ERR_ABORTED", 1], ["MEDIA_ERR_NETWORK", 2], ["MEDIA_ERR_DECODE", 3], ["MEDIA_ERR_SRC_NOT_SUPPORTED", 4]].forEach(function (c) {
      def(g.MediaError, c[0], c[1]);
      def(g.MediaError.prototype, c[0], c[1]);
    });
  if (!g.VTTCue) {
    var VTTCue = function VTTCue(start, end, text) {
      this.startTime = Number(start); this.endTime = Number(end); this.text = String(text);
      this.id = ""; this.pauseOnExit = false; this.track = null; this.onenter = null; this.onexit = null;
    };
    VTTCue.prototype = Object.create(g.TextTrackCue.prototype, { constructor: { value: VTTCue, writable: true, configurable: true } });
    def(g, "VTTCue", VTTCue);
  }
  /* document.implementation: other documents are detached trees, as DOMParser makes them (the
   * nodes are this document's, which is what an inert document is used for: parsing, templates). */
  if (g.document && !document.implementation) {
    var detachedDocument = function (title) {
      var html = document.createElement("html"), head = document.createElement("head"), body = document.createElement("body");
      html.appendChild(head);
      html.appendChild(body);
      if (title !== undefined) {
        var t = document.createElement("title");
        t.textContent = String(title);
        head.appendChild(t);
      }
      var doc = {
        nodeType: 9, nodeName: "#document", documentElement: html, head: head, body: body, contentType: "text/html",
        compatMode: "CSS1Compat", characterSet: "UTF-8", readyState: "complete", defaultView: null,
        get title() { var t = head.querySelector("title"); return t ? t.textContent : ""; },
        set title(v) { var t = head.querySelector("title"); if (!t) { t = document.createElement("title"); head.appendChild(t); } t.textContent = String(v); },
        get firstChild() { return html; }, get childNodes() { return [html]; }, get children() { return [html]; },
        createElement: function (t) { return document.createElement(t); },
        createElementNS: function (n, t) { return document.createElementNS(n, t); },
        createTextNode: function (t) { return document.createTextNode(t); },
        createComment: function (t) { return document.createComment(t); },
        createDocumentFragment: function () { return document.createDocumentFragment(); },
        createTreeWalker: function (r, w, f) { return document.createTreeWalker(r, w, f); },
        importNode: function (n, deep) { return n.cloneNode(!!deep); },
        adoptNode: function (n) { if (n.parentNode) n.parentNode.removeChild(n); return n; },
        querySelector: function (q) { return html.querySelector(q); },
        querySelectorAll: function (q) { return html.querySelectorAll(q); },
        getElementById: function (id) { return html.querySelector("[id=" + JSON.stringify(String(id)) + "]"); },
        getElementsByTagName: function (t) { return html.getElementsByTagName(t); },
        getElementsByClassName: function (c) { return html.getElementsByClassName(c); },
        addEventListener: function () {}, removeEventListener: function () {}
      };
      doc.implementation = impl;
      doc.ownerDocument = null;
      return doc;
    };
    var impl = {
      createHTMLDocument: function (title) { return detachedDocument(title); },
      createDocument: function () { return detachedDocument(); },
      createDocumentType: function (name, pub, sys) { return { nodeType: 10, name: String(name), publicId: String(pub || ""), systemId: String(sys || "") }; },
      hasFeature: function () { return true; }
    };
    def(document, "implementation", impl);
    if (g.DOMImplementation) Object.setPrototypeOf(impl, g.DOMImplementation.prototype);
  }
  URL.createObjectURL = function (blob) {
    if (blob && blob["%mse"] && g["%mseURL"]) return g["%mseURL"](blob);     /* a MediaSource, for a media element */
    if(!(blob instanceof Blob))throw new TypeError("createObjectURL requires a Blob");return g["%blobURL"](blob._bytes,blob.type); };
  URL.revokeObjectURL = function (url) { if (g["%mseRevoke"] && g["%mseRevoke"](String(url))) return; g["%blobURL"](String(url)); };
  nav.clipboard = {
    writeText: function (text) { try { return Promise.resolve(g["%clipboard"](String(text))); } catch(e) { return Promise.reject(new DOMException(String(e), "NotAllowedError")); } },
    readText: function () { try { return Promise.resolve(g["%clipboard"]()); } catch(e) { return Promise.reject(new DOMException(String(e), "NotAllowedError")); } }
  };
  nav.permissions = { query: function (d) { return Promise.resolve({ name: d && d.name, state: "denied", onchange: null }); } };
  nav.mediaDevices = { enumerateDevices: function () { return Promise.resolve([]); },
    getUserMedia: function () { return Promise.reject(new DOMException("No devices", "NotFoundError")); } };
  nav.storage = { estimate: function () { return Promise.resolve(JSON.parse(g["%idb"]("estimate", ""))); },
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
      if(g["%binaryType"](x)==="SharedArrayBuffer")return {$:"sab",i:id,v:g["%sharedExport"](x)};
      if (x instanceof ArrayBuffer) return { $: "ab", i: id, v: bytes(new Uint8Array(x)) };
      if (ArrayBuffer.isView(x))
        return { $: "ta", i: id, t: x instanceof DataView ? "DataView" : x.constructor.name,
                 b:enc(x.buffer),o:x.byteOffset,n:x.byteLength };
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
      case "sab":return refs[x.i]=g["%sharedImport"](x.v);
      case "ta": {
        /* Only a real TypedArray or DataView name: never an arbitrary global constructor. */
        var C=x.t==="DataView"?DataView:/^(Int8|Uint8|Uint8Clamped|Int16|Uint16|Int32|Uint32|Float32|Float64|BigInt64|BigUint64)Array$/.test(x.t)?g[x.t]:null;
        if(!C)throw cloneError();
        var b=x.b?dec(x.b):unbytes(x.v).buffer;
        var offset=x.b?x.o:0,n=x.b?x.n:b.byteLength;
        return refs[x.i]=C===DataView?new DataView(b,offset,n):new C(b,offset,n/C.BYTES_PER_ELEMENT);
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
  function post(id, data, origin, transfer) {
    if(origin && typeof origin==="object"){transfer=origin.transfer;origin=origin.targetOrigin;}
    var copy=transfer===undefined?data:structuredClone(data,{transfer:transfer || []});
    winop(id, "post", serialize(copy), origin === undefined ? "/" : String(origin));
  }
  function proxy(id) {
    if (!id || id === selfId) return g;
    var p = proxies.get(id);
    if (p) return p;
    p = {
      postMessage: function (data, origin, transfer) { post(id, data, origin, transfer); },
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
  def(g, "postMessage", function (data, origin, transfer) { post(selfId, data, origin, transfer); });
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
        var text = serialize(transfer===undefined?data:structuredClone(data,{transfer:transfer && transfer.transfer || transfer || []}));
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

  /* <audio> and <video>: the browser plays them and says what they do. <audio> files are played by
   * webaudio.c's player (%media), whose answers come to %mediaState; <video>, and anything fed by a
   * MediaSource, by the media engine (user/desktop/media/engine.c; %media and %mediaCmd), whose
   * lines come to %mediaMsg. The element keeps its state and sends its events; the controls the
   * browser draws come here too (%mediaUser). Sound may start once the user has done something on
   * the page (or muted). */
  var mediaOp = g["%media"], mediaCmd = g["%mediaCmd"];
  var mediaProtos = [g.HTMLMediaElement, g.HTMLAudioElement, g.HTMLVideoElement].filter(function (c) { return c; })
    .map(function (c) { return c.prototype; }).filter(function (p, i, all) { return all.indexOf(p) === i; });
  var mediaNotSupported = function () { return new DOMException("This browser cannot play that media", "NotSupportedError"); };
  var mediaError = function (code, message) {
    var e = Object.create(g.MediaError ? g.MediaError.prototype : Object.prototype);
    def(e, "code", code || 4);
    def(e, "message", message || "The sound could not be played");
    return e;
  };
  var mstate = function (el) {
    var s = el["%ms"];
    if (!s) {
      s = { paused: true, ended: false, seeking: false, time: 0, duration: NaN, readyState: 0, networkState: 0,
            error: null, pending: [], volume: 1, muted: el.hasAttribute("muted"), rate: 1, defaultRate: 1, pitch: true,
            engine: false, loaded: false, ms: null, srcObject: null, vw: 0, vh: 0, ranges: [], timeAt: 0, given: 0,
            stalled: false, waiting: false, frame: false, dataLoaded: false, pendingSeek: -1, loadQueued: false,
            frames: 0 };
      def(el, "%ms", s);
    }
    return s;
  };
  var mfire = function (el, type) { el.dispatchEvent(new Event(type)); };
  var msettle = function (s, err) {
    var p = s.pending;
    s.pending = [];
    p.forEach(function (q) { if (err) q.reject(err); else q.resolve(); });
  };
  var mlater = function (el, types) { setTimeout(function () { types.forEach(function (t) { mfire(el, t); }); }, 0); };
  var mplayable = function (type) {
    var t = String(type).toLowerCase(), base = t.split(";")[0].trim();
    var codecs = t.indexOf("codecs=") >= 0 ? t.split("codecs=")[1] : null;
    if (["audio/mpeg", "audio/mp3", "audio/mpeg3", "audio/x-mpeg", "audio/x-mp3"].indexOf(base) >= 0)
      return !codecs || codecs.indexOf("mp3") >= 0 ? "probably" : "";
    if (["audio/wav", "audio/wave", "audio/x-wav", "audio/vnd.wave"].indexOf(base) >= 0)
      return !codecs || codecs.indexOf("1") >= 0 ? "probably" : "";
    if (["audio/ogg", "application/ogg", "audio/vorbis"].indexOf(base) >= 0)
      return !codecs ? "maybe" : codecs.indexOf("vorbis") >= 0 && codecs.indexOf("opus") < 0 ? "probably" : "";
    return "";
  };
  /* A type's codecs= list, without quotes and spaces (null if it names none). */
  var mcodecs = function (t) {
    var i = t.indexOf("codecs=");
    if (i < 0) return null;
    var v = t.slice(i + 7).split(";")[0].split(String.fromCharCode(34)).join("");
    return v.split(",").map(function (c) { return c.trim(); }).filter(function (c) { return c; });
  };
  /* What the media engine plays: FFmpeg's H.264, VP8/9, MPEG-4, Theora; AAC, MP3, Opus, Vorbis, FLAC. */
  var vplayable = function (type) {
    var t = String(type).toLowerCase(), base = t.split(";")[0].trim(), codecs = mcodecs(t);
    var known = function (list) {
      if (!codecs) return "maybe";
      return codecs.every(function (c) { return list.some(function (p) { return c.indexOf(p) === 0; }); }) ? "probably" : "";
    };
    var mp4 = ["avc1", "avc3", "mp4a", "mp4v", "hev1", "hvc1", "mp3", "opus", "flac", "ac-3", "ec-3"];
    var webm = ["vp8", "vp9", "vp09", "opus", "vorbis"];
    if (["video/mp4", "audio/mp4", "video/quicktime", "audio/x-m4a", "video/x-m4v", "audio/aac"].indexOf(base) >= 0) return known(mp4);
    if (base === "video/webm" || base === "audio/webm") return known(webm);
    if (base === "video/x-matroska" || base === "audio/x-matroska") return known(mp4.concat(webm));
    if (base === "video/ogg" || base === "application/ogg") return known(["theora", "vorbis", "opus", "flac"]);
    if (base === "video/mpeg" || base === "video/x-msvideo" || base === "video/avi") return "maybe";
    return "";
  };
  /* The buffered spans media show, as TimeRanges. */
  var TimeRanges = g.TimeRanges || function TimeRanges() { throw new TypeError("Illegal constructor"); };
  var mkranges = function (list) { var r = Object.create(TimeRanges.prototype); def(r, "_r", list); return r; };
  if (!g.TimeRanges) {
    var rcheck = function (r, i) {
      if (!(i >= 0 && i < r._r.length)) throw new DOMException("Index out of range", "IndexSizeError");
      return r._r[i | 0];
    };
    Object.defineProperty(TimeRanges.prototype, "length", { configurable: true, get: function () { return this._r.length; } });
    def(TimeRanges.prototype, "start", function (i) { return rcheck(this, Number(i))[0]; });
    def(TimeRanges.prototype, "end", function (i) { return rcheck(this, Number(i))[1]; });
    def(g, "TimeRanges", TimeRanges);
  }
  var EMPTY_RANGES = mkranges([]);
  var ranges = function (from, to) { return to > from ? mkranges([[from, to]]) : EMPTY_RANGES; };
  /* Whether the media engine plays it: a <video>, or anything a MediaSource feeds. */
  var mseOf = function (el) {
    var s = mstate(el);
    if (s.srcObject && s.srcObject["%mse"]) return s.srcObject;
    var src = el.getAttribute("src");
    return src && g["%mseFor"] ? g["%mseFor"](el.src) : null;
  };
  var engined = function (el) { return el.localName === "video" || !!mstate(el).ms || !!mseOf(el); };
  /* Where it is: what the engine last said, and the time since while it plays. */
  var mnow = function (el) {
    var s = mstate(el);
    if (!s.engine || s.paused || s.seeking || s.stalled || !s.timeAt) return s.time;
    var t = s.time + Math.min((performance.now() - s.timeAt) / 1000, 1);
    if (s.duration >= 0 && t > s.duration) t = s.duration;
    if (t < s.given) t = s.given;
    s.given = t;
    return t;
  };
  /* A new readyState, with the events that go with it. */
  var mplaying = function (el) {
    var s = mstate(el);
    if (!s.paused && s.waiting) {
      s.waiting = false;
      mfire(el, "playing");
    }
    if (!s.paused) msettle(s);
  };
  var mwaiting = function (el) {
    var s = mstate(el);
    if (!s.paused && !s.waiting) {
      s.waiting = true;
      mfire(el, "waiting");
    }
  };
  var mready = function (el, rs) {
    var s = mstate(el), old = s.readyState;
    if (rs === old) return;
    s.readyState = rs;
    if (old < 1 && rs >= 1) {
      s.networkState = 1;
      mfire(el, "loadedmetadata");
      if (s.pendingSeek >= 0) {
        var at = s.pendingSeek;
        s.pendingSeek = -1;
        el.currentTime = at;
      }
    }
    if (rs >= 2 && !s.dataLoaded) {
      s.dataLoaded = true;
      mfire(el, "loadeddata");
    }
    if (old < 3 && rs >= 3) {
      mfire(el, "canplay");
      if (!s.paused) mplaying(el);
    }
    if (old < 4 && rs >= 4) mfire(el, "canplaythrough");
    if (old >= 3 && rs < 3) mwaiting(el);
  };
  def(g, "%mstate", mstate);
  def(g, "%mfire", mfire);
  def(g, "%mediaReady", mready);
  def(g, "%timeRanges", mkranges);
  /* The resource selection algorithm, for what the engine plays. */
  var mselect = function (el) {
    var s = mstate(el);
    if (s.loaded) return;
    var ms = mseOf(el), url = el.currentSrc;
    if (!ms && !url) return;
    s.loaded = true;
    s.engine = true;
    s.networkState = 2;
    mlater(el, ["loadstart"]);
    if (ms) {
      if (ms.readyState !== "closed") {
        s.error = mediaError(4, "The MediaSource is attached elsewhere");
        s.networkState = 3;
        mlater(el, ["error"]);
        return;
      }
      s.ms = ms;
      mediaOp(el, "mse", 0);
      ms._attach(el);
    } else if (el.getAttribute("preload") !== "none" || el.autoplay) {
      mediaOp(el, "open", 0);
    }
  };
  mediaProtos.forEach(function (MP) {
    def(MP, "play", function () {
      var el = this, s = mstate(el);
      var p = new Promise(function (resolve, reject) { s.pending.push({ resolve: resolve, reject: reject }); });
      p.catch(function () {});
      if (s.error) {
        msettle(s, mediaNotSupported());
        return p;
      }
      if (!mediaOp(el, "allowed") && !s.muted) {
        msettle(s, new DOMException("play() can only start sound once the user has interacted with the page", "NotAllowedError"));
        return p;
      }
      if (engined(el)) {
        if (!s.loaded) mselect(el);
        if (!s.loaded || s.error) {
          if (!s.loaded) mlater(el, ["error"]);
          msettle(s, mediaNotSupported());
          return p;
        }
        var restart = s.ended;
        if (s.ended) {
          s.time = 0;
          s.ended = false;
          s.given = 0;
        }
        mediaOp(el, "volume", s.muted ? 0 : s.volume);
        if (s.paused) {
          s.paused = false;
          s.timeAt = 0;
          mlater(el, ["play"]);
          mediaOp(el, "play", restart ? 0 : -1);
          if (s.readyState >= 3) {
            s.waiting = true;           /* "playing" follows "play" */
            setTimeout(function () { mplaying(el); }, 0);
          } else {
            setTimeout(function () { mwaiting(el); }, 0);
          }
        } else if (s.readyState >= 3) {
          setTimeout(function () { msettle(s); }, 0);
        }
        return p;
      }
      if (s.ended && !el.loop) {
        s.time = 0;
        s.ended = false;
      }
      if (s.paused) {
        s.paused = false;
        mlater(el, ["play"]);
      }
      mediaOp(el, "volume", s.muted ? 0 : s.volume);
      mediaOp(el, "play", s.time);
      return p;
    });
    def(MP, "pause", function () {
      var el = this, s = mstate(el);
      if (!s.loaded && engined(el)) mselect(el);
      if (s.paused) return;
      if (s.engine) s.time = mnow(el);
      s.paused = true;
      s.timeAt = 0;
      s.waiting = false;
      mediaOp(el, "pause", 0);
      msettle(s, new DOMException("The play() request was interrupted by a call to pause()", "AbortError"));
      mlater(el, ["timeupdate", "pause"]);
    });
    def(MP, "load", function () {
      var el = this, s = mstate(el), had = s.networkState !== 0;
      msettle(s, new DOMException("The play() request was interrupted by a new load request", "AbortError"));
      if (s.ms) {
        var old = s.ms;
        s.ms = null;
        old._detach();
      }
      mediaOp(el, "load", 0);
      s.paused = true; s.ended = false; s.seeking = false; s.time = 0; s.duration = NaN;
      s.readyState = 0; s.networkState = 0; s.error = null;
      s.loaded = false; s.engine = false; s.vw = 0; s.vh = 0; s.ranges = []; s.timeAt = 0; s.given = 0;
      s.stalled = false; s.waiting = false; s.frame = false; s.dataLoaded = false; s.pendingSeek = -1;
      if (had) mlater(el, ["emptied"]);
      if (engined(el)) {
        mselect(el);
        if (el.autoplay) el.play();
      } else if (el.autoplay && el.localName === "audio") {
        el.play();
      }
    });
    def(MP, "canPlayType", function (type) {
      return this.localName === "video" ? vplayable(type) : mplayable(type);
    });
    def(MP, "fastSeek", function (t) { this.currentTime = t; });
    def(MP, "addTextTrack", function () { return { mode: "disabled", cues: [], addCue: function () {}, removeCue: function () {} }; });
    def(MP, "setSinkId", function () { return Promise.reject(mediaNotSupported()); });
    def(MP, "captureStream", function () { throw mediaNotSupported(); });
    ["paused", "ended", "seeking", "readyState", "networkState", "error"].forEach(function (k) {
      Object.defineProperty(MP, k, { configurable: true, get: function () { return mstate(this)[k]; } });
    });
    Object.defineProperty(MP, "duration", { configurable: true, get: function () { return mstate(this).duration; } });
    var fixed = { textTracks: [], audioTracks: [], videoTracks: [] };
    Object.keys(fixed).forEach(function (k) {
      Object.defineProperty(MP, k, { configurable: true, get: function () { var v = this["%m_" + k]; return v === undefined ? fixed[k] : v; },
                                     set: function (v) { def(this, "%m_" + k, v); } });
    });
    Object.defineProperty(MP, "videoWidth", { configurable: true, get: function () { return mstate(this).vw; } });
    Object.defineProperty(MP, "videoHeight", { configurable: true, get: function () { return mstate(this).vh; } });
    Object.defineProperty(MP, "srcObject", { configurable: true,
      get: function () { return mstate(this).srcObject; },
      set: function (v) {
        var el = this, s = mstate(el);
        s.srcObject = v || null;
        if (!s.loadQueued) {
          s.loadQueued = true;
          queueMicrotask(function () { s.loadQueued = false; el.load(); });
        }
      } });
    Object.defineProperty(MP, "buffered", { configurable: true, get: function () {
      var s = mstate(this);
      if (s.ms) return mkranges(s.ms._buffered());
      if (s.engine) return mkranges(s.ranges.map(function (r) { return r.slice(); }));
      return s.readyState ? ranges(0, s.duration) : EMPTY_RANGES;
    } });
    Object.defineProperty(MP, "seekable", { configurable: true, get: function () {
      var s = mstate(this);
      if (s.ms && !(s.duration < Infinity)) return mkranges(s.ms._seekable());
      return s.readyState ? ranges(0, s.duration) : EMPTY_RANGES;
    } });
    Object.defineProperty(MP, "played", { configurable: true, get: function () { var s = mstate(this); return ranges(0, s.time); } });
    Object.defineProperty(MP, "currentSrc", { configurable: true, get: function () {
      if (this.getAttribute("src")) return this.src;
      var video = this.localName === "video", sources = this.getElementsByTagName("source");
      for (var i = 0; i < sources.length; i++)
        if (sources[i].getAttribute("src") && (!sources[i].type || (video ? vplayable(sources[i].type) : mplayable(sources[i].type))))
          return sources[i].src;
      return "";
    } });
    Object.defineProperty(MP, "currentTime", { configurable: true,
      get: function () { return mnow(this); },
      set: function (v) {
        var el = this, s = mstate(el), t = Number(v);
        if (s.engine && !isFinite(t)) throw new TypeError("The provided double value is non-finite.");
        s.time = t >= 0 ? t : 0;
        if (s.engine && s.duration >= 0 && s.time > s.duration) s.time = s.duration;
        s.ended = false;
        s.given = 0;
        s.timeAt = 0;
        if (s.readyState >= 1) {
          s.seeking = true;
          mediaOp(el, "seek", s.time);
          mlater(el, ["seeking"]);
          if (s.ms) s.ms._update();
        } else if (engined(el)) {
          s.pendingSeek = s.time;
        }
      } });
    Object.defineProperty(MP, "volume", { configurable: true,
      get: function () { return mstate(this).volume; },
      set: function (v) {
        var s = mstate(this), x = Number(v);
        if (!(x >= 0 && x <= 1)) throw new DOMException("The volume is not between 0 and 1", "IndexSizeError");
        if (x === s.volume) return;
        s.volume = x;
        mediaOp(this, "volume", s.muted ? 0 : x);
        mlater(this, ["volumechange"]);
      } });
    Object.defineProperty(MP, "muted", { configurable: true,
      get: function () { return mstate(this).muted; },
      set: function (v) {
        var s = mstate(this);
        if (!!v === s.muted) return;
        s.muted = !!v;
        mediaOp(this, "volume", s.muted ? 0 : s.volume);
        mlater(this, ["volumechange"]);
      } });
    [["playbackRate", "rate"], ["defaultPlaybackRate", "defaultRate"], ["preservesPitch", "pitch"]].forEach(function (k) {
      Object.defineProperty(MP, k[0], { configurable: true, get: function () { return mstate(this)[k[1]]; },
                                        set: function (v) {
                                          var s = mstate(this), x = k[0] === "preservesPitch" ? !!v : Number(v);
                                          if (x === s[k[1]]) return;
                                          s[k[1]] = x;
                                          if (k[0] === "playbackRate") mlater(this, ["ratechange"]);
                                        } });
    });
    [["autoplay", "autoplay"], ["loop", "loop"], ["controls", "controls"], ["defaultMuted", "muted"]].forEach(function (k) {
      Object.defineProperty(MP, k[0], { configurable: true, get: function () { return this.hasAttribute(k[1]); },
                                        set: function (v) { if (v) this.setAttribute(k[1], ""); else this.removeAttribute(k[1]); } });
    });
    Object.defineProperty(MP, "preload", { configurable: true,
      get: function () { var v = (this.getAttribute("preload") || "").toLowerCase(); return v === "none" || v === "metadata" ? v : v === "" && !this.hasAttribute("preload") ? "metadata" : "auto"; },
      set: function (v) { this.setAttribute("preload", String(v)); } });
    [["NETWORK_EMPTY", 0], ["NETWORK_IDLE", 1], ["NETWORK_LOADING", 2], ["NETWORK_NO_SOURCE", 3], ["HAVE_NOTHING", 0],
     ["HAVE_METADATA", 1], ["HAVE_CURRENT_DATA", 2], ["HAVE_FUTURE_DATA", 3], ["HAVE_ENOUGH_DATA", 4]].forEach(function (c) {
      def(MP, c[0], c[1]);
      def(MP.constructor, c[0], c[1]);
    });
  });
  if (g.HTMLVideoElement) {
    var VP = g.HTMLVideoElement.prototype;
    def(VP, "getVideoPlaybackQuality", function () {
      var s = mstate(this);
      return { creationTime: performance.now(), totalVideoFrames: s.frames, droppedVideoFrames: 0, corruptedVideoFrames: 0 };
    });
    Object.defineProperty(VP, "webkitDecodedFrameCount", { configurable: true, get: function () { return mstate(this).frames; } });
    Object.defineProperty(VP, "webkitDroppedFrameCount", { configurable: true, get: function () { return 0; } });
    var vcallbacks = [];
    def(VP, "requestVideoFrameCallback", function (fn) {
      var el = this;
      vcallbacks.push(fn);
      var id = vcallbacks.length;
      requestAnimationFrame(function (now) { if (vcallbacks[id - 1]) { var f = vcallbacks[id - 1]; vcallbacks[id - 1] = null;
        f(now, { mediaTime: el.currentTime, presentedFrames: mstate(el).frames, width: el.videoWidth, height: el.videoHeight }); } });
      return id;
    });
    def(VP, "cancelVideoFrameCallback", function (id) { if (id > 0) vcallbacks[id - 1] = null; });
    Object.defineProperty(VP, "disablePictureInPicture", { configurable: true, get: function () { return this.hasAttribute("disablepictureinpicture"); },
                                                         set: function (v) { if (v) this.setAttribute("disablepictureinpicture", ""); else this.removeAttribute("disablepictureinpicture"); } });
    def(VP, "requestPictureInPicture", function () { return Promise.reject(new DOMException("Picture-in-picture is not available", "NotSupportedError")); });
  }
  g["%mediaState"] = function (el, type, t, d) {
    var s = mstate(el);
    if (t >= 0) s.time = t;
    if (d >= 0) s.duration = d;
    if (type === "loadstart") {
      s.networkState = 2;
      mfire(el, "loadstart");
    } else if (type === "loadedmetadata") {
      s.readyState = 1;
      s.networkState = 1;
      mfire(el, "durationchange");
      mfire(el, "loadedmetadata");
    } else if (type === "canplay") {
      s.readyState = 4;
      mfire(el, "loadeddata");
      mfire(el, "canplay");
      mfire(el, "canplaythrough");
    } else if (type === "playing") {
      if (!s.paused) {
        mfire(el, "playing");
        msettle(s);
      }
    } else if (type === "timeupdate") {
      mfire(el, "timeupdate");
    } else if (type === "seeked") {
      s.seeking = false;
      mfire(el, "timeupdate");
      mfire(el, "seeked");
    } else if (type === "ended") {
      s.ended = true;
      s.paused = true;
      mfire(el, "timeupdate");
      mfire(el, "pause");
      mfire(el, "ended");
    } else if (type === "error") {
      s.error = mediaError();
      s.networkState = 3;
      s.readyState = 0;
      s.paused = true;
      mfire(el, "error");
      msettle(s, mediaNotSupported());
    }
  };
  /* A line from the media engine about the element (engine.c lists them). */
  g["%mediaMsg"] = function (el, line) {
    var s = mstate(el), w = String(line).split(" "), verb = w[0];
    var num = function (i) { return Number(w[i]); };
    if (verb.indexOf("sb") === 0) {
      if (s.ms) s.ms._message(w);
      return;
    }
    if (!s.engine) return;
    if (verb === "vmeta") {
      s.duration = num(2) >= 0 ? num(2) / 1000 : Infinity;
      s.vw = num(3);
      s.vh = num(4);
      mfire(el, "durationchange");
      mready(el, Math.max(s.readyState, 1));
    } else if (verb === "vfirst") {
      s.frame = true;
      if (s.ms) s.ms._update(); else mready(el, 4);
    } else if (verb === "vframes") {
      s.frames = num(2);
    } else if (verb === "vtime") {
      if (!s.seeking) {
        s.time = num(2) / 1000;
        s.timeAt = s.paused || s.stalled ? 0 : performance.now();
        if (!s.paused) mfire(el, "timeupdate");
      }
    } else if (verb === "vwait") {
      s.time = mnow(el);
      s.stalled = true;
      s.timeAt = 0;
      mwaiting(el);
    } else if (verb === "vplaying") {
      s.stalled = false;
      if (s.readyState >= 3 || !s.ms) mplaying(el);
    } else if (verb === "vseeked") {
      if (!s.seeking) return;
      s.seeking = false;
      s.time = num(2) / 1000;
      s.given = 0;
      s.timeAt = s.paused ? 0 : performance.now();
      mfire(el, "timeupdate");
      mfire(el, "seeked");
      if (s.ms) s.ms._update();
    } else if (verb === "vended") {
      if (el.loop) {
        s.time = 0;
        s.given = 0;
        mediaOp(el, "play", 0);
        return;
      }
      if (s.duration >= 0 && s.duration < Infinity) s.time = s.duration;
      s.ended = true;
      s.paused = true;
      s.timeAt = 0;
      mfire(el, "timeupdate");
      mfire(el, "pause");
      mfire(el, "ended");
    } else if (verb === "verror") {
      s.error = mediaError(s.readyState ? 2 : 4, w.slice(2).join(" ") || "The video could not be played");
      s.networkState = s.readyState ? 1 : 3;
      s.paused = true;
      mfire(el, "error");
      msettle(s, mediaNotSupported());
    } else if (verb === "vbuffered") {
      var list = [];
      for (var i = 0; i < num(2); i++) list.push([num(3 + 2 * i) / 1000, num(4 + 2 * i) / 1000]);
      s.ranges = list;
      mfire(el, "progress");
    }
  };
  /* src changed: what the engine plays is loaded again after the script (an <audio> file waits for play()). */
  g["%mediaSrc"] = function (el) {
    var s = mstate(el);
    if (s.loadQueued || !engined(el)) return;
    s.loadQueued = true;
    queueMicrotask(function () { s.loadQueued = false; el.load(); });
  };
  g["%mediaUser"] = function (el, op, t) {
    if (op === "toggle") {
      if (el.paused) el.play(); else el.pause();
    } else if (op === "seek") {
      el.currentTime = t;
    }
  };
  if (g.document)
    document.addEventListener("DOMContentLoaded", function () {
      [].forEach.call(document.querySelectorAll("audio[autoplay]"), function (a) { a.play(); });
      [].forEach.call(document.querySelectorAll("video"), function (v) {
        if (mstate(v).loaded) return;
        mselect(v);
        if (v.autoplay) v.play();
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
