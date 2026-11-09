/* Production browser compatibility regressions. Run with make test-browser. */

test("constructed sheet stays detached", function(){var before=document.querySelectorAll("style").length; var s=new CSSStyleSheet(); s.replaceSync("div{color:red}"); return [s.ownerNode,s instanceof CSSStyleSheet,s.cssRules.length,document.querySelectorAll("style").length-before];},[null,true,1,0]);
test("document adoption",function(){var s=new CSSStyleSheet();s.replaceSync("div{color:red}");document.adoptedStyleSheets=[s];return document.adoptedStyleSheets.length;},1);
test("mutable adoption",function(){var s=new CSSStyleSheet();document.adoptedStyleSheets.push(s);return document.adoptedStyleSheets.length;},2);
test("shadow adoption and shared updates",function(){var h=document.createElement("div");document.body.appendChild(h);var r=h.attachShadow({mode:"open"});var s=new CSSStyleSheet();s.replaceSync("span{color:blue}");r.adoptedStyleSheets=[s];s.insertRule("b{color:red}",1);return [r.adoptedStyleSheets.length,r.childNodes.length,s.cssRules.length];},[1,0,2]);

var shadowHost=document.createElement('div');document.body.appendChild(shadowHost);var shadowRoot=shadowHost.attachShadow({mode:'open'});shadowRoot.innerHTML='<slot></slot><slot name="named"></slot>';var defaultSlot=shadowRoot.firstChild, namedSlot=shadowRoot.lastChild, slotEvents=[];
defaultSlot.addEventListener('slotchange',function(e){slotEvents.push([e.bubbles,e.composed,defaultSlot.assignedNodes().length]);});
var slotted=document.createElement('span');shadowHost.appendChild(slotted);shadowHost.appendChild(document.createTextNode('text'));shadowHost.removeChild(slotted);shadowHost.appendChild(slotted);
Promise.resolve().then(function(){test('slotchange coalesces at microtask',function(){return slotEvents;},[[true,false,2]]);});
test('slotted event enters slot',function(){var path=[];var f=function(e){path.push('slot');};defaultSlot.addEventListener('x',f);shadowHost.addEventListener('x',function(e){path.push(e.target===slotted?'host':'bad');});slotted.dispatchEvent(new Event('x',{bubbles:true}));return path;},['slot','host']);

(async function(){
 var pulls=0, control;
 var input=new ReadableStream({start:function(c){control=c;},pull:function(c){pulls++;c.enqueue(pulls);if(pulls===3)c.close();}},{highWaterMark:1});
 var values=[];
 await input.pipeTo(new WritableStream({write:function(x){values.push(x);}}));
 test('pipe data and release locks',function(){return [values,input.locked];},[[1,2,3],false]);
 var output=[];
 await new ReadableStream({start:function(c){c.enqueue(2);c.enqueue(3);c.close();}}).pipeThrough(new TransformStream({transform:function(v,c){c.enqueue(v*2);}})).pipeTo(new WritableStream({write:function(v){output.push(v);}}));
 test('transform pipeline',function(){return output;},[4,6]);
 var writes=[],s=new WritableStream({write:async function(v){await Promise.resolve();writes.push(v);}}), w=s.getWriter();
 var p=w.write(1);test('write backpressure',function(){return w.desiredSize;},0);await p;await w.ready;test('write ready',function(){return w.desiredSize;},1);await w.close();await w.closed;w.releaseLock();
 test('writes finish before close',function(){return [writes,s.locked];},[[1],false]);
 var cancel,abort;var bad=new Error('bad');
 try{await new ReadableStream({pull:function(c){c.enqueue(1);},cancel:function(e){cancel=e;}}).pipeTo(new WritableStream({write:function(){throw bad;},abort:function(e){abort=e;}}));}catch(e){test('pipe rejection cancels source',function(){return e===bad&&cancel===bad;},true);}
 var source=new ReadableStream({start:function(c){c.enqueue('x');c.close();}}), branches=source.tee();
 test('tee copies data',function(){return [branches.length];},[2]);
 var a=await branches[0].getReader().read(),b=await branches[1].getReader().read();test('tee branch results',function(){return [a.value,b.value];},['x','x']);
 var lock=new ReadableStream(), reader=lock.getReader(), pending=reader.read();reader.releaseLock();try{await pending;}catch(e){test('released pending read rejects',function(){return e.name;},'TypeError');}
})();

(async function(){
 function hex(bytes){return Array.from(new Uint8Array(bytes)).map(function(b){return ('0'+b.toString(16)).slice(-2);}).join('');}
 var raw=new Uint8Array(20);raw.fill(11);var key=await crypto.subtle.importKey('raw',raw,{name:'HMAC',hash:'SHA-256'},true,['sign','verify']);
 var data=new TextEncoder().encode('Hi There'),sig=await crypto.subtle.sign('HMAC',key,data);
 test('RFC4231 HMAC-SHA256',function(){return hex(sig);},'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7');
 test('CryptoKey properties',function(){return [key instanceof CryptoKey,key.type,key.extractable,key.algorithm.hash.name,key.algorithm.length];},[true,'secret',true,'SHA-256',160]);
 var ok=await crypto.subtle.verify('HMAC',key,sig,data);test('verify HMAC',function(){return ok;},true);
 var bad=new Uint8Array(sig);bad[0]^=1;ok=await crypto.subtle.verify('HMAC',key,bad,data);test('reject modified signature',function(){return ok;},false);
 var jwk=await crypto.subtle.exportKey('jwk',key),back=await crypto.subtle.importKey('jwk',jwk,{name:'HMAC',hash:'SHA-256'},false,['verify']);
 test('JWK roundtrip and usages',function(){return [jwk.kty,jwk.alg,back.extractable,back.usages];},['oct','HS256',false,['verify']]);
 try{await crypto.subtle.exportKey('raw',back);}catch(e){test('nonextractable key rejection',function(){return e.name;},'InvalidAccessError');}
 try{await crypto.subtle.sign('HMAC',back,data);}catch(e){test('key usage rejection',function(){return e.name;},'InvalidAccessError');}
 var generated=await crypto.subtle.generateKey({name:'HMAC',hash:'SHA-512'},true,['sign']);var bytes=await crypto.subtle.exportKey('raw',generated);
 test('generate HMAC key',function(){return bytes.byteLength;},128);
})();

(async function () {
  var aborted, canceled, reason = new Error('source failed');
  var source = new ReadableStream({ start: function (c) { c.error(reason); } });
  try { await source.pipeTo(new WritableStream({ abort: function (e) { aborted = e; } })); }
  catch (e) { test('source error aborts sink and unlocks source', function () { return e === reason && aborted === reason && !source.locked; }, true); }

  var sinkControl, destination = new WritableStream({ start: function (c) { sinkControl = c; } });
  source = new ReadableStream({ cancel: function (e) { canceled = e; } });
  var piping = source.pipeTo(destination);
  queueMicrotask(function () { sinkControl.error(reason); });
  try { await piping; }
  catch (e) { test('idle source observes destination error', function () { return e === reason && canceled === reason && !destination.locked; }, true); }

  var ac = new AbortController();
  source = new ReadableStream({ cancel: function (e) { canceled = e; } });
  destination = new WritableStream({ abort: function (e) { aborted = e; } });
  piping = source.pipeTo(destination, { signal: ac.signal });
  ac.abort(reason);
  try { await piping; }
  catch (e) { test('abort interrupts pending pipe read', function () { return e === reason && canceled === reason && aborted === reason && !source.locked && !destination.locked; }, true); }

  var output = [];
  await new ReadableStream({ start: function (c) { c.enqueue('a'); c.close(); } })
    .pipeThrough(new TransformStream({ flush: function (c) { c.enqueue('z'); } }))
    .pipeTo(new WritableStream({ write: function (x) { output.push(x); } }));
  test('transform flush precedes close', function () { return output; }, ['a', 'z']);

  var active = 0, maximum = 0, pulls = 0;
  await new ReadableStream({ pull: async function (c) {
    active++; maximum = Math.max(maximum, active); await Promise.resolve(); active--;
    c.enqueue(++pulls); if (pulls === 3) c.close();
  } }).pipeTo(new WritableStream());
  test('asynchronous pulls never overlap', function () { return [maximum, pulls]; }, [1, 3]);

  var original = new ReadableStream({ cancel: function (reasons) { canceled = reasons; } });
  var branches = original.tee();
  await Promise.all([branches[0].cancel('left'), branches[1].cancel('right')]);
  test('canceling both tee branches releases source', function () { return [canceled, original.locked]; }, [['left', 'right'], false]);
})();

test('closed shadow hides internal slotted event path', function () {
  var host = document.createElement('div'); document.body.appendChild(host);
  var root = host.attachShadow({mode:'closed'}); root.innerHTML = '<slot></slot>';
  var light = document.createElement('b'); host.appendChild(light);
  var visible;
  host.addEventListener('hidden-path', function (e) { visible = e.composedPath(); });
  light.dispatchEvent(new Event('hidden-path', {bubbles:true, composed:true}));
  return [visible.indexOf(light) >= 0, visible.indexOf(host) >= 0, visible.indexOf(root), visible.indexOf(root.firstChild)];
}, [true, true, -1, -1]);
