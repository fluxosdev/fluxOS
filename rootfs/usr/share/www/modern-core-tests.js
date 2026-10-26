group('binary views and Unicode');
test('shared buffer writes',function(){var b=new ArrayBuffer(4),a=new Uint8Array(b),c=new Uint8Array(b);a[0]=42;return [c[0],a.buffer===b];},[42,true]);
test('integer conversion on indexed writes',function(){var a=new Uint8Array(3);a[0]=256;a[1]=-1;a[2]=1.9;return Array.from(a);},[0,255,1]);
test('clamped ties to even',function(){return Array.from(new Uint8ClampedArray([0.5,1.5,2.5,254.5,300,-1]));},[0,2,2,254,255,0]);
test('subarray aliases bytes',function(){var a=new Uint8Array([1,2,3]),b=a.subarray(1);b[0]=9;return [a[1],b.byteOffset,b.buffer===a.buffer];},[9,1,true]);
test('slice copies bytes',function(){var a=new Uint8Array([1,2,3]),b=a.slice(1);b[0]=9;return [a[1],b[0],b.buffer===a.buffer];},[2,9,false]);
test('overlapping set snapshots input',function(){var a=new Uint8Array([1,2,3,4]);a.set(a.subarray(0,3),1);return Array.from(a);},[1,1,2,3]);
test('DataView shares memory and respects endian',function(){var b=new ArrayBuffer(8),v=new DataView(b);v.setUint32(1,0x12345678,true);return [v.getUint32(1,true),v.getUint16(1,false),new Uint8Array(b)[1]];},[305419896,30806,120]);
test('DataView floating point',function(){var v=new DataView(new ArrayBuffer(8));v.setFloat64(0,-1.25);return v.getFloat64(0);},-1.25);
test('unaligned typed view rejected',function(){try{new Uint16Array(new ArrayBuffer(4),1);}catch(e){return e.name;}},'RangeError');
test('oversized set rejected',function(){try{new Uint8Array(1).set([1,2]);}catch(e){return e.name;}},'RangeError');
test('out of bounds DataView rejected',function(){try{new DataView(new ArrayBuffer(2)).getInt32(0);}catch(e){return e.name;}},'RangeError');
test('typed arrays are not Arrays',function(){var a=new Uint8Array(2);return [Array.isArray(a),ArrayBuffer.isView(a),ArrayBuffer.isView(a.buffer)];},[false,true,false]);
test('typed keys',function(){return Object.keys(new Uint8Array([4,5]));},['0','1']);
test('typed map and iteration',function(){var a=Uint16Array.from([1,2],function(x){return x*2;});return [Array.from(a.map(function(x){return x+1;})),Array.from(a.values())];},[[3,5],[2,4]]);
test('UTF16 string lengths',function(){return ['é'.length,'😀'.length,'a😀b'.length];},[1,2,4]);
test('UTF16 indexed units',function(){return ['é'[0],'😀'[0].charCodeAt(0),'😀'[1].charCodeAt(0)];},['é',55357,56832]);
test('UTF16 slicing',function(){return ['a😀b'.slice(1,3),'a😀b'.substring(3,1),'a😀b'.substr(1,2),'é'.at(-1)];},['😀','😀','😀','é']);
test('UTF16 search positions',function(){return ['é😀z'.indexOf('z'),'é😀z'.lastIndexOf('z'),'é😀z'.endsWith('😀',3)];},[3,3,true]);
test('UTF8 encoding',function(){return Array.from(new TextEncoder().encode('é😀'));},[195,169,240,159,152,128]);
test('decode view respects offset',function(){return new TextDecoder().decode(new Uint8Array([65,195,169,66]).subarray(1,3));},'é');
test('decoder replacement',function(){return new TextDecoder().decode(new Uint8Array([255]));},'�');
test('Blob byte size',function(){return new Blob([new Uint8Array([0,255]),'é']).size;},4);
new Blob([new Uint8Array([0,255,10])]).arrayBuffer().then(function(b){test('Blob binary roundtrip',function(){return Array.from(new Uint8Array(b));},[0,255,10]);});
new Response(new Uint8Array([0,255,10])).arrayBuffer().then(function(b){test('Response binary roundtrip',function(){return Array.from(new Uint8Array(b));},[0,255,10]);});
new Response('é').text().then(function(t){test('Response UTF8 decoding',function(){return t;},'é');});
var bodyResponse=new Response('one');bodyResponse.text().then(function(){return bodyResponse.text();}).catch(function(e){test('body consumed only once',function(){return e.name;},'TypeError');});

test('shadow root is a separate tree',function(){var h=document.createElement('div');h.innerHTML='<b>light</b>';document.body.appendChild(h);var r=h.attachShadow({mode:'open'});r.innerHTML='<span id="inside">shadow</span>';return [r!==h,r.nodeType,r.host===h,h.children.length,r.querySelector('span').textContent,h.querySelector('#inside'),document.querySelector('#inside'),r.querySelector('span').getRootNode()===r];},[true,11,true,1,'shadow',null,null,true]);
test('closed shadow root stays closed',function(){var h=document.createElement('div'),r=h.attachShadow({mode:'closed'});return [h.shadowRoot,r.host===h,r.mode];},[null,true,'closed']);
test('slot assignment keeps DOM parent',function(){var h=document.createElement('div');h.innerHTML='<b slot="title">one</b><i>two</i>';var r=h.attachShadow({mode:'open'});r.innerHTML='<slot name="title"></slot><slot></slot>';var slot=r.querySelector('slot');return [slot.assignedNodes().length,slot.assignedElements()[0]===h.firstChild,h.firstChild.parentNode===h,h.firstChild.assignedSlot===slot];},[1,true,true,true]);
test('shadow root can only attach once',function(){var h=document.createElement('div');h.attachShadow({mode:'open'});try{h.attachShadow({mode:'open'});}catch(e){return true;}return false;},true);
test('animation interpolates mid point',function(){var e=document.createElement('div');e.style.opacity='0';var a=e.animate([{opacity:0},{opacity:1}],{duration:100,fill:'both'});a.pause();a.currentTime=50;var n=Number(e.style.opacity);a.cancel();return n;},0.5);
test('animation finishes with fill',function(){var e=document.createElement('div');var a=e.animate([{width:'0px'},{width:'100px'}],{duration:100,fill:'forwards'});a.finish();return [e.style.width,a.playState];},['100px','finished']);
test('animation cancellation restores style',function(){var e=document.createElement('div');e.style.opacity='0.25';var a=e.animate([{opacity:0},{opacity:1}],100);a.pause();a.currentTime=50;a.cancel();return [e.style.opacity,a.playState,e.getAnimations().length];},['0.25','idle',0]);
test('composed event retargets at host',function(){var h=document.createElement('div');document.body.appendChild(h);var r=h.attachShadow({mode:'open'});r.innerHTML='<b>inside</b>';var log=[];h.addEventListener('probe',function(e){log.push(e.target===h,e.composedPath()[0]===r.firstChild);});r.firstChild.dispatchEvent(new Event('probe',{bubbles:true,composed:true}));return log;},[true,true]);
test('noncomposed event stays inside root',function(){var h=document.createElement('div');document.body.appendChild(h);var r=h.attachShadow({mode:'open'});r.innerHTML='<b>inside</b>';var seen=false;h.addEventListener('probe',function(){seen=true;});r.firstChild.dispatchEvent(new Event('probe',{bubbles:true}));return seen;},false);

test('UTF16 code points and splitting',function(){return ['😀'.codePointAt(0),'😀'.split('').length,'😀'.split('')[0].charCodeAt(0),'é'.padStart(3,'ø')];},[128512,2,55357,'øøé']);
test('surrogate pieces compare as scalar',function(){var s='😀';return s.slice(0,1)+s.slice(1)===s;},true);

test('typed index ownership and deletion',function(){var a=new Uint8Array(1);return [0 in a,1 in a,a.hasOwnProperty('1'),delete a[0],delete a[1],Object.getOwnPropertyDescriptor(a,'1')];},[true,false,false,false,true,undefined]);
test('typed array descriptors update bytes',function(){var a=new Uint8Array(1);Object.defineProperty(a,'0',{value:257});return a[0];},1);
test('random source validates integer arrays',function(){try{crypto.getRandomValues(new Float32Array(1));}catch(e){return true;}return false;},true);
test('randomUUID has UUIDv4 format',function(){return /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(crypto.randomUUID());},true);

test('TextDecoder streaming UTF8 boundaries',function(){var d=new TextDecoder(),a=d.decode(new Uint8Array([239,187]),{stream:true}),b=d.decode(new Uint8Array([191,240,159]),{stream:true}),c=d.decode(new Uint8Array([152,128]),{stream:true}),e=d.decode();return [a,b,c,e];},['','','😀','']);
test('TextDecoder flush replaces partial sequence',function(){var d=new TextDecoder();return [d.decode(new Uint8Array([226]),{stream:true}),d.decode()];},['','�']);
test('font checks do not report unknown fonts as loaded',function(){return [document.fonts.check('16px serif'),document.fonts.check('16px Definitely Missing')];},[true,false]);
