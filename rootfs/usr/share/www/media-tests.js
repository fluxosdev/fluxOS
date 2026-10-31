group("<audio>: state, events and play() promises (the browser's answers played by %mediaState)");
var said = window["%mediaState"];

test("canPlayType knows the formats the player decodes, and no video", function () {
  var a = new Audio(), v = document.createElement("video");
  return [a.canPlayType("audio/mpeg"), a.canPlayType("audio/wav"), a.canPlayType("audio/ogg"),
          a.canPlayType("audio/ogg; codecs=vorbis"), a.canPlayType("audio/ogg; codecs=opus"),
          a.canPlayType("audio/mp4"), v.canPlayType("video/mp4"), v.canPlayType("audio/mpeg")];
}, ["probably", "probably", "maybe", "probably", "", "", "", ""]);

test("a new element has nothing yet", function () {
  var a = new Audio("x.mp3");
  return [a.paused, a.ended, isNaN(a.duration), a.currentTime, a.readyState, a.networkState, a.volume, a.muted,
          a.getAttribute("src"), a.buffered.length, a.error];
}, [true, false, true, 0, 0, 0, 1, false, "x.mp3", 0, null]);

var log = [], results = {};
(function () {
  /* before the user has done anything, sound may not start */
  var loud = new Audio("loud.mp3");
  loud.play().then(function () { results.loud = "played"; }, function (e) { results.loud = e.name; });
  /* muted, it may: then the browser's answers come in */
  var a = new Audio("song.mp3");
  a.muted = true;
  ["play", "playing", "loadstart", "durationchange", "loadedmetadata", "canplay", "timeupdate", "pause", "ended",
   "seeking", "seeked", "volumechange"].forEach(function (t) { a.addEventListener(t, function () { log.push(t); }); });
  a.play().then(function () { results.first = "resolved " + a.paused; });
  results.during = [a.paused, a.ended];
  said(a, "loadstart", -1, -1);
  said(a, "loadedmetadata", 0, 12.5);
  said(a, "canplay", -1, -1);
  said(a, "playing", -1, -1);
  said(a, "timeupdate", 3.25, -1);
  results.meta = [a.duration, a.readyState, a.networkState, a.currentTime, a.buffered.length, a.seekable.end(0)];
  a.currentTime = 6;
  results.seeking = a.seeking;
  said(a, "seeked", 6, -1);
  said(a, "ended", 12.5, -1);
  results.ended = [a.ended, a.paused, a.currentTime, a.seeking];
  /* pause() turns down what play() promised */
  var b = new Audio("other.wav");
  b.muted = true;
  b.play().then(function () { results.second = "resolved"; }, function (e) { results.second = e.name; });
  b.pause();
  /* the sound would not play */
  var c = new Audio("broken.ogg");
  c.muted = true;
  c.addEventListener("error", function () { results.errorEvent = c.error.code; });
  c.play().then(function () { results.third = "resolved"; }, function (e) { results.third = e.name; });
  said(c, "error", -1, -1);
  /* no player for video */
  document.createElement("video").play().then(function () { results.video = "played"; }, function (e) { results.video = e.name; });
})();

setTimeout(function () {
  test("play() waits for the user, or plays muted; the browser's answers set the state and send the events", function () {
    return [results.loud, results.during, results.first, results.meta, results.seeking, results.ended,
            results.second, results.third, results.errorEvent, results.video];
  }, ["NotAllowedError", [false, false], "resolved true", [12.5, 4, 1, 3.25, 1, 12.5], true, [true, true, 12.5, false],
      "AbortError", "NotSupportedError", 4, "NotSupportedError"]);
  test("the events, in order", function () {
    return log;
  }, ["loadstart", "durationchange", "loadedmetadata", "canplay", "playing", "timeupdate", "timeupdate", "seeked",
      "timeupdate", "pause", "ended", "volumechange", "play", "seeking"]);
}, 0);

test("volume and muted check and announce their values; loop and autoplay are attributes", function () {
  var a = new Audio(), r = [];
  a.volume = 0.5;
  try { a.volume = 2; } catch (e) { r.push(e.name); }
  a.loop = true;
  a.autoplay = true;
  r.push(a.volume, a.hasAttribute("loop"), a.autoplay, a.controls);
  return r;
}, ["IndexSizeError", 0.5, true, true, false]);
