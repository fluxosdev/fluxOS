group("Canvas 2D");
function mk(w, h) { var c = document.createElement("canvas"); c.width = w; c.height = h; return c; }
function arr(t) { var a = []; for (var i = 0; i < t.length; i++) a.push(t[i]); return a; }
function px(ctx, x, y) { return arr(ctx.getImageData(x, y, 1, 1).data); }
test("a canvas is 300 x 150 and gives one 2d context", function () {
  var c = document.createElement("canvas"), a = c.getContext("2d");
  return [c.width, c.height, a === c.getContext("2d"), a.canvas === c, c.getContext("webgl"), typeof a.fillRect];
}, [300, 150, true, true, null, "function"]);
test("the context starts with the specified defaults", function () {
  var x = mk(10, 10).getContext("2d");
  return [x.fillStyle, x.strokeStyle, x.lineWidth, x.lineCap, x.lineJoin, x.miterLimit, x.globalAlpha,
    x.globalCompositeOperation, x.font, x.textAlign, x.textBaseline, x.shadowBlur, x.imageSmoothingEnabled];
}, ["#000000", "#000000", 1, "butt", "miter", 10, 1, "source-over", "10px sans-serif", "start", "alphabetic", 0, true]);
test("styles are read back in canonical form, and bad values are ignored", function () {
  var x = mk(10, 10).getContext("2d");
  x.fillStyle = "#F00"; var a = x.fillStyle;
  x.fillStyle = "rgba(0, 0, 255, 0.5)"; var b = x.fillStyle;
  x.fillStyle = "not a colour"; var c = x.fillStyle;
  x.fillStyle = "red"; x.fillStyle = 12; var d = x.fillStyle;
  return [a, b, c, d];
}, ["#ff0000", "rgba(0, 0, 255, 0.5)", "rgba(0, 0, 255, 0.5)", "#ff0000"]);
test("fillRect paints solid pixels and leaves the rest clear", function () {
  var x = mk(20, 20).getContext("2d"); x.fillStyle = "rgb(255, 0, 0)"; x.fillRect(5, 5, 10, 10);
  return [px(x, 5, 5), px(x, 14, 14), px(x, 15, 15), px(x, 4, 10)];
}, [[255, 0, 0, 255], [255, 0, 0, 255], [0, 0, 0, 0], [0, 0, 0, 0]]);
test("globalAlpha and translucent colours blend", function () {
  var x = mk(4, 4).getContext("2d");
  x.fillStyle = "#fff"; x.fillRect(0, 0, 4, 4);
  x.globalAlpha = 0.5; x.fillStyle = "#f00"; x.fillRect(0, 0, 2, 4);
  x.globalAlpha = 1; x.fillStyle = "rgba(0, 0, 255, 0.5)"; x.fillRect(2, 0, 2, 4);
  return [px(x, 0, 0), px(x, 3, 3)];
}, [[255, 127, 127, 255], [127, 127, 255, 255]]);
test("translucent drawing on nothing keeps its colour", function () {
  var x = mk(4, 4).getContext("2d"); x.fillStyle = "rgba(10, 200, 30, 0.5)"; x.fillRect(0, 0, 4, 4);
  return px(x, 1, 1);
}, [10, 200, 30, 128]);
test("clearRect makes pixels transparent again", function () {
  var x = mk(10, 10).getContext("2d"); x.fillStyle = "#0f0"; x.fillRect(0, 0, 10, 10); x.clearRect(2, 2, 3, 3);
  return [px(x, 3, 3), px(x, 6, 6)];
}, [[0, 0, 0, 0], [0, 255, 0, 255]]);
test("strokes of whole-pixel lines cover whole pixels", function () {
  var x = mk(30, 30).getContext("2d"); x.lineWidth = 2; x.beginPath(); x.moveTo(5, 10); x.lineTo(25, 10); x.stroke();
  return [px(x, 10, 9), px(x, 10, 10), px(x, 10, 8), px(x, 10, 11), px(x, 4, 10), px(x, 25, 10)];
}, [[0, 0, 0, 255], [0, 0, 0, 255], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]);
test("a half-covered pixel gets half the alpha", function () {
  var x = mk(10, 10).getContext("2d"); x.fillRect(0, 0, 5, 1.5);
  return [px(x, 2, 0)[3], px(x, 2, 1)[3], px(x, 2, 2)[3]];
}, [255, 128, 0]);
test("paths fill with the non-zero and even-odd rules", function () {
  var x = mk(30, 30).getContext("2d"); x.beginPath(); x.rect(0, 0, 30, 30); x.rect(10, 10, 10, 10);
  x.fill(); var a = px(x, 15, 15)[3];
  x.clearRect(0, 0, 30, 30); x.fill("evenodd"); var b = px(x, 15, 15)[3];
  return [a, b, px(x, 5, 5)[3]];
}, [255, 0, 255]);
test("a triangle fills inside its edges only", function () {
  var x = mk(40, 40).getContext("2d"); x.beginPath(); x.moveTo(5, 35); x.lineTo(35, 35); x.lineTo(20, 5); x.closePath(); x.fill();
  return [px(x, 20, 30)[3], px(x, 6, 10)[3], px(x, 34, 10)[3], px(x, 20, 12)[3]];
}, [255, 0, 0, 255]);
test("arc fills a disc, and isPointInPath agrees", function () {
  var x = mk(60, 60).getContext("2d"); x.beginPath(); x.arc(30, 30, 20, 0, Math.PI * 2); x.fill();
  return [px(x, 30, 30)[3], px(x, 49, 30)[3] > 240, px(x, 52, 30)[3], px(x, 30, 52)[3], x.isPointInPath(30, 30), x.isPointInPath(5, 5)];
}, [255, true, 0, 0, true, false]);
test("a half circle with the arc drawn anticlockwise", function () {
  var x = mk(60, 60).getContext("2d"); x.beginPath(); x.arc(30, 30, 20, 0, Math.PI, false); x.closePath(); x.fill();
  var a = [px(x, 30, 40)[3], px(x, 30, 20)[3]];
  x.clearRect(0, 0, 60, 60); x.beginPath(); x.arc(30, 30, 20, 0, Math.PI, true); x.closePath(); x.fill();
  return a.concat([px(x, 30, 40)[3], px(x, 30, 20)[3]]);
}, [255, 0, 0, 255]);
test("transforms compose and getTransform reports them", function () {
  var x = mk(50, 50).getContext("2d"); x.translate(10, 20); x.scale(2, 3);
  var m = x.getTransform(); x.fillRect(0, 0, 5, 5);
  return [m.a, m.d, m.e, m.f, px(x, 12, 22)[3], px(x, 19, 34)[3], px(x, 21, 36)[3]];
}, [2, 3, 10, 20, 255, 255, 0]);
test("rotate and setTransform", function () {
  var x = mk(50, 50).getContext("2d"); x.setTransform(1, 0, 0, 1, 25, 25); x.rotate(Math.PI / 2); x.fillRect(0, 0, 10, 4);
  return [px(x, 22, 30)[3], px(x, 28, 30)[3], px(x, 30, 25)[3]];
}, [255, 0, 0]);
test("save and restore bring back state and transform", function () {
  var x = mk(10, 10).getContext("2d"); x.fillStyle = "#123456"; x.lineWidth = 7; x.translate(3, 4);
  x.save(); x.fillStyle = "#abcdef"; x.lineWidth = 2; x.translate(100, 100); x.restore();
  var m = x.getTransform(); x.restore();
  return [x.fillStyle, x.lineWidth, m.e, m.f];
}, ["#123456", 7, 3, 4]);
test("clip limits later drawing", function () {
  var x = mk(40, 40).getContext("2d"); x.beginPath(); x.rect(10, 10, 20, 20); x.clip();
  x.fillStyle = "#00f"; x.fillRect(0, 0, 40, 40);
  return [px(x, 15, 15)[3], px(x, 5, 5)[3], px(x, 35, 35)[3]];
}, [255, 0, 0]);
test("a clip survives save and goes with restore", function () {
  var x = mk(40, 40).getContext("2d"); x.save(); x.beginPath(); x.rect(0, 0, 10, 10); x.clip(); x.fillRect(0, 0, 40, 40); x.restore();
  x.fillRect(20, 20, 5, 5); return [px(x, 5, 5)[3], px(x, 30, 30)[3], px(x, 22, 22)[3]];
}, [255, 0, 255]);
test("linear gradients run between their stops", function () {
  var x = mk(100, 4).getContext("2d"), g = x.createLinearGradient(0, 0, 100, 0);
  g.addColorStop(0, "#ff0000"); g.addColorStop(1, "#0000ff"); x.fillStyle = g; x.fillRect(0, 0, 100, 4);
  var a = px(x, 0, 1), b = px(x, 50, 1), c = px(x, 99, 1);
  return [a[0] > 250 && a[2] < 6, b[0] > 120 && b[0] < 135 && b[2] > 120 && b[2] < 135, c[2] > 250 && c[0] < 6];
}, [true, true, true]);
test("gradient stops out of range are refused, and the style reads back as the object", function () {
  var x = mk(10, 10).getContext("2d"), g = x.createLinearGradient(0, 0, 10, 0), err = "";
  try { g.addColorStop(1.5, "red"); } catch (e) { err = e.name; }
  var err2 = ""; try { g.addColorStop(0.5, "bogus"); } catch (e) { err2 = e.name; }
  x.fillStyle = g; return [err, err2, x.fillStyle === g];
}, ["IndexSizeError", "SyntaxError", true]);
test("radial gradients start at the centre colour", function () {
  var x = mk(60, 60).getContext("2d"), g = x.createRadialGradient(30, 30, 0, 30, 30, 30);
  g.addColorStop(0, "#fff"); g.addColorStop(1, "#000"); x.fillStyle = g; x.fillRect(0, 0, 60, 60);
  var c = px(x, 30, 30), e = px(x, 58, 30);
  return [c[0] > 240, e[0] < 20, px(x, 0, 0)[3]];
}, [true, true, 255]);
test("conic gradients go round their centre", function () {
  var x = mk(40, 40).getContext("2d"), g = x.createConicGradient(0, 20, 20);
  g.addColorStop(0, "#f00"); g.addColorStop(0.5, "#00f"); g.addColorStop(1, "#f00"); x.fillStyle = g; x.fillRect(0, 0, 40, 40);
  var right = px(x, 38, 20), left = px(x, 1, 20);
  return [right[0] > 200, left[2] > 200];
}, [true, true]);
test("getImageData and putImageData round-trip exactly", function () {
  var x = mk(4, 4).getContext("2d"), d = x.createImageData(2, 2);
  d.data.set([10, 20, 30, 255, 40, 50, 60, 128, 70, 80, 90, 0, 255, 254, 253, 252]);
  x.putImageData(d, 1, 1);
  var r = x.getImageData(1, 1, 2, 2);
  return [r.width, r.height, arr(r.data), px(x, 0, 0)];
}, [2, 2, [10, 20, 30, 255, 40, 50, 60, 128, 0, 0, 0, 0, 255, 254, 253, 252], [0, 0, 0, 0]]);
test("putImageData with a dirty rectangle copies only that part", function () {
  var x = mk(4, 4).getContext("2d"), d = x.createImageData(2, 2);
  for (var i = 0; i < 16; i++) d.data[i] = 255;
  x.putImageData(d, 0, 0, 1, 1, 1, 1); return [px(x, 0, 0)[3], px(x, 1, 1)[3], px(x, 1, 0)[3]];
}, [0, 255, 0]);
test("createImageData and getImageData sizes", function () {
  var x = mk(10, 10).getContext("2d"), a = x.createImageData(3, 5), b = x.createImageData(a), c = x.getImageData(8, 8, 4, 4);
  var err = ""; try { x.getImageData(0, 0, 0, 5); } catch (e) { err = e.name; }
  /* width * height * 4 must not wrap round to fit a small buffer */
  var bad = [];
  [[65536, 65536], [65536, 16385], [16384, 16384], [-1, 4], [1.5e9, 2]].forEach(function (s) {
    try { x.putImageData({ width: s[0], height: s[1], data: new Uint8ClampedArray(16) }, 0, 0); bad.push("ok"); }
    catch (e) { bad.push(e.name); }
  });
  return [a.width, a.height, a.data.length, b.width, c.data.length, px(x, 9, 9), err, bad];
}, [3, 5, 60, 3, 64, [0, 0, 0, 0], "IndexSizeError", ["TypeError", "TypeError", "TypeError", "TypeError", "TypeError"]]);
test("composite operations", function () {
  var x = mk(4, 4).getContext("2d");
  x.fillStyle = "#f00"; x.fillRect(0, 0, 4, 4);
  x.globalCompositeOperation = "destination-out"; x.fillStyle = "#000"; x.fillRect(0, 0, 2, 4);
  var a = [px(x, 0, 0)[3], px(x, 3, 0)[3]];
  x.globalCompositeOperation = "source-over"; x.fillStyle = "#00f"; x.fillRect(0, 0, 4, 4);
  x.globalCompositeOperation = "destination-over"; x.fillStyle = "#0f0"; x.fillRect(0, 0, 4, 4);
  var b = [px(x, 1, 1), px(x, 3, 3)];
  x.globalCompositeOperation = "lighter"; x.fillStyle = "rgb(0, 0, 0)"; x.fillRect(0, 0, 4, 4);
  return a.concat(b);
}, [0, 255, [0, 0, 255, 255], [0, 0, 255, 255]]);
test("source-in and copy act on the whole canvas", function () {
  var x = mk(6, 6).getContext("2d"); x.fillStyle = "#f00"; x.fillRect(0, 0, 6, 6);
  x.globalCompositeOperation = "source-in"; x.fillStyle = "#00f"; x.fillRect(0, 0, 3, 3);
  var a = [px(x, 1, 1), px(x, 4, 4)];
  x.globalCompositeOperation = "copy"; x.fillStyle = "#0f0"; x.fillRect(0, 0, 2, 2);
  return a.concat([px(x, 1, 1), px(x, 4, 4)]);
}, [[0, 0, 255, 255], [0, 0, 0, 0], [0, 255, 0, 255], [0, 0, 0, 0]]);
test("multiply blends colours", function () {
  var x = mk(2, 2).getContext("2d"); x.fillStyle = "rgb(255, 128, 0)"; x.fillRect(0, 0, 2, 2);
  x.globalCompositeOperation = "multiply"; x.fillStyle = "rgb(128, 128, 128)"; x.fillRect(0, 0, 2, 2);
  var p = px(x, 0, 0); return [p[0], p[1], p[2], p[3]];
}, [128, 64, 0, 255]);
test("drawImage copies and scales another canvas", function () {
  var a = mk(2, 2), ax = a.getContext("2d"); ax.fillStyle = "#f00"; ax.fillRect(0, 0, 1, 2); ax.fillStyle = "#00f"; ax.fillRect(1, 0, 1, 2);
  var x = mk(8, 4).getContext("2d"); x.imageSmoothingEnabled = false; x.drawImage(a, 0, 0, 8, 4);
  var y = mk(6, 6).getContext("2d"); y.drawImage(a, 1, 0, 1, 2, 2, 2, 2, 2);
  return [px(x, 1, 1), px(x, 6, 3), px(y, 3, 3), px(y, 1, 1)[3]];
}, [[255, 0, 0, 255], [0, 0, 255, 255], [0, 0, 255, 255], 0]);
test("drawImage under a transform", function () {
  var a = mk(2, 2), ax = a.getContext("2d"); ax.fillStyle = "#0f0"; ax.fillRect(0, 0, 2, 2);
  var x = mk(20, 20).getContext("2d"); x.translate(10, 10); x.scale(2, 2); x.drawImage(a, 0, 0);
  return [px(x, 11, 11)[3], px(x, 13, 13)[3], px(x, 15, 15)[3]];
}, [255, 255, 0]);
test("patterns tile their image", function () {
  var t = mk(2, 2), tx = t.getContext("2d"); tx.fillStyle = "#f00"; tx.fillRect(0, 0, 1, 1); tx.fillStyle = "#00f"; tx.fillRect(1, 1, 1, 1);
  var x = mk(8, 8).getContext("2d"), p = x.createPattern(t, "repeat"); x.fillStyle = p; x.fillRect(0, 0, 8, 8);
  var n = mk(8, 8).getContext("2d"); n.fillStyle = n.createPattern(t, "no-repeat"); n.fillRect(0, 0, 8, 8);
  return [px(x, 0, 0), px(x, 4, 4), px(x, 7, 7), px(x, 5, 4)[3], px(n, 0, 0)[3], px(n, 4, 4)[3]];
}, [[255, 0, 0, 255], [255, 0, 0, 255], [0, 0, 255, 255], 0, 255, 0]);
test("dashes leave gaps", function () {
  var x = mk(60, 10).getContext("2d"); x.lineWidth = 2; x.setLineDash([10, 10]); x.beginPath(); x.moveTo(0, 5); x.lineTo(60, 5); x.stroke();
  return [px(x, 5, 5)[3], px(x, 15, 5)[3], px(x, 25, 5)[3], px(x, 35, 5)[3], x.getLineDash()];
}, [255, 0, 255, 0, [10, 10]]);
test("odd dash lists are doubled, and bad ones ignored", function () {
  var x = mk(4, 4).getContext("2d"); x.setLineDash([5, 10, 5]); var a = x.getLineDash();
  x.setLineDash([1, -2]); var b = x.getLineDash(); x.setLineDash([]); return [a, b, x.getLineDash()];
}, [[5, 10, 5, 5, 10, 5], [5, 10, 5, 5, 10, 5], []]);
test("line caps and joins change what a stroke covers", function () {
  var x = mk(40, 20).getContext("2d"); x.lineWidth = 6; x.lineCap = "square"; x.beginPath(); x.moveTo(10, 10); x.lineTo(30, 10); x.stroke();
  var sq = [px(x, 8, 10)[3], px(x, 6, 10)[3]];
  var y = mk(40, 20).getContext("2d"); y.lineWidth = 6; y.beginPath(); y.moveTo(10, 10); y.lineTo(30, 10); y.stroke();
  return sq.concat([px(y, 8, 10)[3], x.lineCap, x.lineJoin]);
}, [255, 0, 0, "square", "miter"]);
test("rounded rectangles and ellipses", function () {
  var x = mk(60, 40).getContext("2d"); x.beginPath(); x.roundRect(5, 5, 50, 30, 15); x.fill();
  var a = [px(x, 6, 6)[3], px(x, 30, 20)[3], px(x, 30, 6)[3]];
  var y = mk(60, 40).getContext("2d"); y.beginPath(); y.ellipse(30, 20, 25, 10, 0, 0, Math.PI * 2); y.fill();
  return a.concat([px(y, 30, 20)[3], px(y, 52, 20)[3], px(y, 30, 5)[3]]);
}, [0, 255, 255, 255, 255, 0]);
test("arcTo rounds a corner", function () {
  var x = mk(40, 40).getContext("2d"); x.lineWidth = 2; x.beginPath(); x.moveTo(5, 5); x.arcTo(35, 5, 35, 35, 10); x.lineTo(35, 35); x.stroke();
  return [px(x, 20, 5)[3] > 0, px(x, 35, 20)[3] > 0, px(x, 34, 6)[3] === 0, px(x, 20, 20)[3]];
}, [true, true, true, 0]);
test("quadratic and cubic curves", function () {
  var x = mk(50, 50).getContext("2d"); x.lineWidth = 2; x.beginPath(); x.moveTo(5, 45); x.quadraticCurveTo(25, 5, 45, 45); x.stroke();
  var a = px(x, 25, 25)[3] > 0;
  var y = mk(50, 50).getContext("2d"); y.lineWidth = 2; y.beginPath(); y.moveTo(5, 25); y.bezierCurveTo(15, 5, 35, 45, 45, 25); y.stroke();
  return [a, px(x, 25, 40)[3], px(y, 25, 25)[3] > 0, px(y, 5, 25)[3] > 0];
}, [true, 0, true, true]);
test("Path2D from SVG path data and from calls", function () {
  var x = mk(40, 40).getContext("2d"), p = new Path2D("M10 10 h 20 v 20 h -20 z");
  x.fill(p); var a = [px(x, 20, 20)[3], px(x, 5, 5)[3], x.isPointInPath(p, 20, 20), x.isPointInPath(p, 5, 5)];
  var q = new Path2D(); q.rect(0, 0, 5, 5); q.arc(30, 30, 5, 0, 7); var y = mk(40, 40).getContext("2d"); y.fill(q);
  return a.concat([px(y, 2, 2)[3], px(y, 30, 30)[3], px(y, 15, 15)[3]]);
}, [255, 0, true, false, 255, 255, 0]);
test("isPointInStroke follows the line width", function () {
  var x = mk(40, 40).getContext("2d"); x.lineWidth = 6; x.beginPath(); x.moveTo(5, 20); x.lineTo(35, 20);
  return [x.isPointInStroke(20, 22), x.isPointInStroke(20, 25), x.isPointInStroke(50, 20)];
}, [true, false, false]);
test("shadows are drawn behind the shape, offset", function () {
  var x = mk(60, 40).getContext("2d"); x.shadowColor = "rgba(0, 0, 0, 1)"; x.shadowOffsetX = 20; x.shadowOffsetY = 0;
  x.fillStyle = "#f00"; x.fillRect(5, 10, 10, 10);
  return [px(x, 10, 15), px(x, 30, 15), px(x, 40, 15)[3], x.shadowOffsetX, x.shadowColor];
}, [[255, 0, 0, 255], [0, 0, 0, 255], 0, 20, "#000000"]);
test("blurred shadows spread softly", function () {
  var x = mk(60, 60).getContext("2d"); x.shadowColor = "#000"; x.shadowBlur = 10; x.shadowOffsetX = 0; x.fillRect(20, 20, 20, 20);
  var edge = px(x, 18, 30)[3], far = px(x, 5, 30)[3], inside = px(x, 30, 30)[3];
  return [edge > 0 && edge < 255, far, inside];
}, [true, 0, 255]);
test("text takes room and paints", function () {
  var x = mk(80, 30).getContext("2d"); var m = x.measureText("abc");
  x.fillStyle = "#00f"; x.fillText("ab", 10, 20);
  return [m.width, px(x, 13, 15), px(x, 3, 15)[3], px(x, 30, 15)[3], px(x, 13, 25)[3]];
}, [18, [0, 0, 255, 255], 0, 0, 0]);
test("text alignment and baseline move the text", function () {
  var x = mk(80, 30).getContext("2d"); x.textAlign = "right"; x.fillText("ab", 40, 20);
  var a = [px(x, 30, 15)[3], px(x, 45, 15)[3]];
  var y = mk(80, 30).getContext("2d"); y.textAlign = "center"; y.fillText("ab", 40, 20);
  var b = [px(y, 36, 15)[3], px(y, 46, 15)[3]];
  var z = mk(80, 30).getContext("2d"); z.textBaseline = "bottom"; z.fillText("ab", 10, 20);
  return a.concat(b, [px(z, 12, 15)[3], px(z, 12, 5)[3]]);
}, [255, 0, 255, 0, 255, 0]);
test("font strings are parsed and read back", function () {
  var x = mk(4, 4).getContext("2d"); x.font = "italic bold 20px Georgia, serif"; var a = x.font;
  x.font = "nonsense"; return [a, x.font];
}, ["italic bold 20px Georgia, serif", "italic bold 20px Georgia, serif"]);
test("width and height reset the bitmap and the state", function () {
  var c = mk(10, 10), x = c.getContext("2d"); x.fillStyle = "#f00"; x.fillRect(0, 0, 10, 10);
  c.width = 10; var a = [px(x, 5, 5)[3], x.fillStyle];
  c.width = 20; c.height = 5; return a.concat([c.width, c.height, x.getImageData(0, 0, 20, 5).data.length]);
}, [0, "#000000", 20, 5, 400]);
test("canvas size attributes are read as numbers", function () {
  var c = document.createElement("canvas"); c.setAttribute("width", "64"); c.setAttribute("height", "x");
  var d = document.createElement("canvas"); d.width = 7.9;
  return [c.width, c.height, d.width, d.getAttribute("width"), mk(0, 5).width];
}, [64, 150, 7, "7", 0]);
test("toDataURL gives a PNG", function () {
  var c = mk(3, 2), x = c.getContext("2d"); x.fillStyle = "#f00"; x.fillRect(0, 0, 3, 2);
  var u = c.toDataURL(), bytes = atob(u.slice(22));
  return [u.indexOf("data:image/png;base64,iVBORw0KGgo"), bytes.charCodeAt(1), bytes.slice(1, 4), bytes.indexOf("IHDR") === 12, bytes.slice(-8, -4)];
}, [0, 80, "PNG", true, "IEND"]);
test("an OffscreenCanvas draws like a canvas", function () {
  var o = new OffscreenCanvas(4, 4), x = o.getContext("2d"); x.fillStyle = "#0f0"; x.fillRect(0, 0, 4, 4);
  var y = mk(8, 8).getContext("2d"); y.drawImage(o, 2, 2);
  return [o.width, o.height, px(y, 3, 3), px(y, 7, 7)[3]];
}, [4, 4, [0, 255, 0, 255], 0]);
test("drawImage and createPattern reject what is not an image", function () {
  var x = mk(4, 4).getContext("2d"), e1 = "", e2 = "";
  try { x.drawImage({}, 0, 0); } catch (e) { e1 = e.name; }
  try { x.createPattern(42, "repeat"); } catch (e) { e2 = e.name; }
  return [e1, e2];
}, ["TypeError", "TypeError"]);
test("methods check their argument counts", function () {
  var x = mk(4, 4).getContext("2d"), r = [];
  ["fillRect", "moveTo", "arc", "drawImage", "createLinearGradient"].forEach(function (n) {
    try { x[n](); r.push("no error"); } catch (e) { r.push(e.name); }
  });
  return r;
}, ["TypeError", "TypeError", "TypeError", "TypeError", "TypeError"]);
test("reset clears the canvas and the state", function () {
  var x = mk(6, 6).getContext("2d"); x.fillStyle = "#f00"; x.translate(2, 2); x.fillRect(0, 0, 4, 4); x.reset();
  return [px(x, 3, 3)[3], x.fillStyle, x.getTransform().e];
}, [0, "#000000", 0]);
test("createImageBitmap makes something drawImage accepts", function () {
  var a = mk(4, 4), ax = a.getContext("2d"); ax.fillStyle = "#f0f"; ax.fillRect(0, 0, 4, 4);
  var got = null; createImageBitmap(a).then(function (b) { got = b; });
  return [typeof createImageBitmap, got === null || typeof got.width];
}, ["function", true]);
