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
        if (at > r.length) throw new DOMException("The index is not in the allowed range.", "IndexSizeError");
        r.splice(at, 0, String(rule));
        owner.textContent = r.join("\n");
        return at;
      },
      deleteRule: function (at) {
        var r = rulesOf(owner.textContent);
        r.splice(at, 1);
        owner.textContent = r.join("\n");
      },
      replaceSync: function (t) { owner.textContent = String(t); },
      replace: function (t) { owner.textContent = String(t); return Promise.resolve(sh); }
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
  def(DP, "adoptedStyleSheets", []);
  /* new CSSStyleSheet(): its rules go into a <style> of its own, in the head */
  function CSSStyleSheet() {
    var st = document.createElement("style");
    st.setAttribute("data-constructed", "");
    (document.head || document.documentElement).appendChild(st);
    return sheetFor(st);
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
  def(EP, "setPointerCapture", function () {});
  def(EP, "releasePointerCapture", function () {});
  def(EP, "hasPointerCapture", function () { return false; });
  def(EP, "showPopover", function () { this.setAttribute("data-popover-open", ""); this.style.display = "block"; });
  def(EP, "hidePopover", function () { this.removeAttribute("data-popover-open"); this.style.display = ""; });
  def(EP, "togglePopover", function () {
    if (this.hasAttribute("data-popover-open")) this.hidePopover(); else this.showPopover();
  });
  getter(EP, "inert", function () { return this.hasAttribute("inert"); });
  getter(EP, "draggable", function () { return this.getAttribute("draggable") === "true"; });
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

})(globalThis);
);

static const char js_dom_eager[] = JS_DOM_SOURCE(
(function (g) {
  var def = g["%def"];
  var nav = g.navigator;
  nav.hardwareConcurrency = 1;
  nav.maxTouchPoints = 0;
  nav.deviceMemory = 1;
  nav.pdfViewerEnabled = false;
  nav.webdriver = false;
  nav.doNotTrack = null;
  nav.product = "Gecko";
  nav.userAgentData = { brands: [], mobile: false, platform: "fluxOS",
    getHighEntropyValues: function () { return Promise.resolve({ brands: [], mobile: false, platform: "fluxOS" }); } };
  nav.clipboard = { writeText: function () { return Promise.resolve(); }, readText: function () { return Promise.resolve(""); },
    write: function () { return Promise.resolve(); }, read: function () { return Promise.resolve([]); } };
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

  g.isSecureContext = location.protocol === "https:" || location.hostname === "localhost";
  g.crossOriginIsolated = false;
  g.origin = location.origin;
  g.visualViewport = { get width() { return innerWidth; }, get height() { return innerHeight; }, scale: 1,
    offsetLeft: 0, offsetTop: 0, get pageLeft() { return scrollX; }, get pageTop() { return scrollY; },
    addEventListener: function () {}, removeEventListener: function () {} };
  def(g, "reportError", function (e) { console.error(e); });
  def(g, "find", function () { return false; });
  def(g, "getScreenDetails", function () { return Promise.reject(new DOMException("Not allowed", "NotAllowedError")); });
})(globalThis);
);
