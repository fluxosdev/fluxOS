group("Media Source Extensions and <video> (the engine's answers played by %mediaMsg)");
var engine = window["%mediaMsg"];

test("isTypeSupported: H.264 and AAC in MP4, Opus in WebM, within 720p at 30 frames", function () {
  var t = MediaSource.isTypeSupported;
  return [t("video/mp4; codecs=\"avc1.4d401f\""), t("audio/mp4; codecs=\"mp4a.40.2\""), t("video/mp4"),
          t("audio/webm; codecs=\"opus\""), t("video/webm; codecs=\"vp9\""), t("video/mp4; codecs=\"av01.0.05M.08\""),
          t("video/mp4; codecs=\"avc1.640028\"; width=1920; height=1080"), t("video/mp4; codecs=\"avc1.4d401f\"; framerate=60"),
          t("video/mp4; codecs=\"avc1.4d401f\"; width=854; height=480; framerate=30"), t("audio/mp4; codecs=\"avc1.4d401f\""),
          t("text/html"), t(""), typeof SourceBuffer, typeof SourceBufferList];
}, [true, true, true, true, false, false, false, false, true, false, false, false, "function", "function"]);

test("SourceBuffer and SourceBufferList cannot be made by a page", function () {
  var r = [];
  try { new SourceBuffer(); } catch (e) { r.push(e.name); }
  try { new SourceBufferList(); } catch (e) { r.push(e.name); }
  var ms = new MediaSource();
  r.push(ms.readyState, isNaN(ms.duration), ms.sourceBuffers.length);
  try { ms.addSourceBuffer("video/mp4"); } catch (e) { r.push(e.name); }
  return r;
}, ["TypeError", "TypeError", "closed", true, 0, "InvalidStateError"]);

var log = [], R = {};
function logEvents(target, name, types) {
  types.forEach(function (t) { target.addEventListener(t, function () { log.push(name + ":" + t); }); });
}
var steps = [], v = document.createElement("video"), ms = new MediaSource(), vsb, asb;
/* each step after what the one before queued (events, loads) has run */
function next() { var f = steps.shift(); if (f) setTimeout(function () { f(); setTimeout(next, 0); }, 0); }
v.muted = true;
document.body.appendChild(v);
logEvents(v, "v", ["loadstart", "durationchange", "loadedmetadata", "loadeddata", "canplay", "canplaythrough",
                   "play", "playing", "waiting", "timeupdate", "seeking", "seeked", "pause", "ended", "resize", "error", "emptied"]);
logEvents(ms, "ms", ["sourceopen", "sourceended", "sourceclose"]);

steps.push(function () {
  var url = URL.createObjectURL(ms);
  R.url = url.indexOf("blob:") === 0;
  v.src = url;                         /* loaded after the script: sourceopen comes later */
  R.beforeOpen = ms.readyState;
});
steps.push(function () {
  R.open = [ms.readyState, v.networkState, v.readyState];
  logEvents(ms.sourceBuffers, "list", ["addsourcebuffer", "removesourcebuffer"]);
  vsb = ms.addSourceBuffer("video/mp4; codecs=\"avc1.4d401f\"");
  asb = ms.addSourceBuffer("audio/mp4; codecs=\"mp4a.40.2\"");
  logEvents(vsb, "vsb", ["updatestart", "update", "updateend", "error", "abort"]);
  logEvents(asb, "asb", ["updatestart", "update", "updateend", "error", "abort"]);
  R.list = [ms.sourceBuffers.length, ms.sourceBuffers[0] === vsb, ms.sourceBuffers[1] === asb, ms.activeSourceBuffers.length];
  ms.duration = 20;
  vsb.appendBuffer(new Uint8Array([1, 2, 3, 4]));
  R.updating = vsb.updating;
  try { vsb.appendBuffer(new Uint8Array(1)); } catch (e) { R.busy = e.name; }
  try { ms.duration = 30; } catch (e) { R.busyDuration = e.name; }
});
steps.push(function () {
  /* the engine has parsed the initialization segment and a media segment */
  engine(v, "sbinit " + vsb._id + " 1 0 640 360 h264");
  engine(v, "sbdone " + vsb._id + " 1 1 0 4000000");
  R.afterVideo = [vsb.updating, vsb.buffered.length, vsb.buffered.end(0), v.readyState, ms.activeSourceBuffers.length];
  asb.appendBuffer(new ArrayBuffer(8));
});
steps.push(function () {
  engine(v, "sbinit " + asb._id + " 0 1 0 0 aac");
  engine(v, "sbdone " + asb._id + " 1 1 0 3500000");
  R.ready = [v.readyState, v.videoWidth, v.videoHeight, v.duration, v.buffered.length, v.buffered.end(0)];
  /* abort() during an append: the engine's late answer for it is not heard */
  vsb.appendBuffer(new Uint8Array(4));
  vsb.abort();
  R.aborted = vsb.updating;
});
steps.push(function () {
  engine(v, "sbdone " + vsb._id + " 2 1 0 9000000");
  R.stale = vsb.buffered.end(0);
  vsb.remove(0, 1);
  R.removing = vsb.updating;
});
steps.push(function () {
  engine(v, "sbdone " + vsb._id + " 4 1 1000000 4000000");
  R.removed = [vsb.updating, vsb.buffered.start(0), v.buffered.length];
  vsb.remove(0, 1);
});
steps.push(function () {
  engine(v, "sbdone " + vsb._id + " 5 1 0 4000000");      /* appended again */
  /* play: muted, it may; it can play at once */
  v.play().then(function () { R.played = "resolved"; }, function (e) { R.played = e.name; });
  R.paused = v.paused;
});
steps.push(function () {
  engine(v, "vfirst 7");
  engine(v, "vtime 7 1500");
  R.time = [v.currentTime >= 1.5, v.currentTime < 2.6];
  v.currentTime = 2;
  R.seeking = v.seeking;
});
steps.push(function () {
  engine(v, "vseeked 7 2000");
  R.seeked = [v.seeking, v.currentTime >= 2];
  engine(v, "vwait 7");
  engine(v, "vplaying 7");
  ms.endOfStream();
  R.ended = [ms.readyState, ms.duration];
  engine(v, "vended 7");
  R.endedEl = [v.ended, v.paused, v.currentTime];
});
steps.push(function () {
  /* appending after endOfStream() opens it again */
  vsb.appendBuffer(new Uint8Array(2));
  R.reopened = ms.readyState;
});
steps.push(function () {
  engine(v, "sbdone " + vsb._id + " 6 1 0 5000000");
  ms.removeSourceBuffer(asb);
  R.gone = [ms.sourceBuffers.length, ms.activeSourceBuffers.length];
  try { asb.buffered; } catch (e) { R.goneBuffered = e.name; }
  /* bytes the engine could not read: an error, and the stream ends with a decode error */
  vsb.appendBuffer(new Uint8Array(2));
});
steps.push(function () {
  engine(v, "sberr " + vsb._id + " 7");
  R.decode = [vsb.updating, ms.readyState];
});
steps.push(function () {
  R.error = v.error && v.error.code;
  /* a new src: the MediaSource is detached */
  v.removeAttribute("src");
  v.load();
  R.closed = [ms.readyState, isNaN(ms.duration), ms.sourceBuffers.length];
});
steps.push(function () {
  test("a MediaSource opens on its element, takes SourceBuffers, and reports what they buffered", function () {
    return [R.url, R.beforeOpen, R.open, R.list, R.updating, R.busy, R.busyDuration];
  }, [true, "closed", ["open", 2, 0], [2, true, true, 0], true, "InvalidStateError", "InvalidStateError"]);
  test("initialization segments make metadata; the element can play where both streams are buffered", function () {
    return [R.afterVideo, R.ready, R.aborted, R.stale, R.removing, R.removed];
  }, [[false, 1, 4, 0, 1], [4, 640, 360, 20, 1, 3.5], false, 4, true, [false, 1, 1]]);
  test("playing, seeking, waiting, the end of the stream, reopening it and errors", function () {
    return [R.paused, R.played, R.time, R.seeking, R.seeked, R.ended, R.endedEl, R.reopened, R.gone,
            R.goneBuffered, R.decode, R.error, R.closed];
  }, [false, "resolved", [true, true], true, [false, true], ["ended", 4], [true, true, 4], "open", [1, 1],
      "InvalidStateError", [false, "ended"], 3, ["closed", true, 0]]);
  test("the events, in order", function () {
    return log.filter(function (e) { return e !== "v:timeupdate"; });
  }, ["v:loadstart", "ms:sourceopen", "v:durationchange", "list:addsourcebuffer", "list:addsourcebuffer",
      "vsb:updatestart", "vsb:update", "vsb:updateend", "asb:updatestart", "v:loadedmetadata", "v:loadeddata",
      "v:canplay", "v:canplaythrough", "asb:update", "asb:updateend", "vsb:updatestart", "vsb:abort", "vsb:updateend",
      "vsb:updatestart", "vsb:update", "vsb:updateend", "vsb:updatestart", "v:canplay", "v:canplaythrough",
      "vsb:update", "vsb:updateend", "v:play", "v:playing", "v:seeking", "v:seeked", "v:waiting", "v:playing",
      "v:durationchange", "v:pause", "v:ended", "ms:sourceended", "ms:sourceopen", "vsb:updatestart", "vsb:update",
      "vsb:updateend", "v:canplay", "v:canplaythrough", "list:removesourcebuffer", "vsb:updatestart", "vsb:error",
      "vsb:updateend", "ms:sourceended", "v:error", "ms:sourceclose", "v:emptied"]);
});

/* a plain file: loaded when its src is set, then the engine says what it holds */
var p = document.createElement("video"), plog = [];
["loadstart", "durationchange", "loadedmetadata", "loadeddata", "canplay", "canplaythrough", "progress"].forEach(function (t) {
  p.addEventListener(t, function () { plog.push(t); });
});
p.src = "movie.mp4";
setTimeout(function () { setTimeout(function () {
  var before = [p.networkState, p.readyState];
  engine(p, "vmeta 9 18947 320 240 1");
  engine(p, "vfirst 9");
  engine(p, "vbuffered 9 1 0 12000");
  test("a file: metadata, the first picture and what has been downloaded", function () {
    return [before, p.duration, p.videoWidth, p.videoHeight, p.readyState, p.buffered.length, p.buffered.end(0), plog];
  }, [[2, 0], 18.947, 320, 240, 4, 1, 12,
      ["loadstart", "durationchange", "loadedmetadata", "loadeddata", "canplay", "canplaythrough", "progress"]]);
}, 0); }, 0);

next();
