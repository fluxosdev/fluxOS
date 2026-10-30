/* Dedicated workers: run by workers.html, and on the host by make test-browser (tests/browser-worker.c).
 * runWorkerTests(report) runs the cases one after another (a page may have 16 workers at a time);
 * workerResults.done is set when all have finished. */
var workerResults = { done: false, total: 0, passed: [], failed: [] };

function runWorkerTests(report) {
  var queue = [];
  function step(name, run) {
    workerResults.total++;
    queue.push(function () {
      var finished = false;
      function done(ok, detail) {
        if (finished) return;
        finished = true;
        (ok ? workerResults.passed : workerResults.failed).push(ok ? name : name + ": " + detail);
        if (!queue.length) workerResults.done = true;
        if (report) report(name, ok, detail);
        if (queue.length) setTimeout(queue.shift(), 0);
      }
      setTimeout(function () { done(false, "timed out"); }, 15000);
      try { run(done); } catch (e) { done(false, String(e)); }
    });
  }
  function ask(w, message, then) {
    w.onmessage = function (e) { then(e.data); };
    w.postMessage(message);
  }

  step("structured clone round trip", function (done) {
    var value = { s: "é😀", n: -0, big: BigInt(12), d: new Date(5), m: new Map([[1, "a"]]), set: new Set([2]),
                  u8: new Uint8Array([1, 255]), nested: { a: [1, { b: null }] }, u: undefined, nan: NaN, re: /x/gi };
    value.self = value;
    var w = new Worker("worker-echo.js");
    ask(w, { op: "echo", value: value }, function (m) {
      var v = m.value;
      w.terminate();
      done(v !== value && v.s === value.s && Object.is(v.n, -0) && v.big === BigInt(12) && v.d instanceof Date &&
           v.d.getTime() === 5 && v.m.get(1) === "a" && v.set.has(2) && v.u8 instanceof Uint8Array && v.u8[1] === 255 &&
           v.nested.a[1].b === null && "u" in v && v.nan !== v.nan && v.re.flags === "gi" && v.self === v,
           JSON.stringify(Object.keys(v)));
    });
  });

  step("Blob and File messages", function (done) {
    var w = new Worker("worker-echo.js");
    var value = { blob: new Blob(["blob \u00e9"], { type: "text/plain" }), file: new File(["file"], "a.txt", { lastModified: 7 }) };
    ask(w, { op: "echo", value: value }, function (m) {
      w.terminate();
      var v = m.value;
      if (!(v.blob instanceof Blob) || !(v.file instanceof File)) { done(false, JSON.stringify(m)); return; }
      Promise.all([v.blob.text(), v.file.text()]).then(function (t) {
        done(t[0] === "blob \u00e9" && v.blob.type === "text/plain" && t[1] === "file" && v.file.name === "a.txt" &&
             v.file.lastModified === 7, JSON.stringify(t));
      });
    });
  });

  step("worker global scope", function (done) {
    var w = new Worker("worker-echo.js", { name: "scoped" });
    ask(w, { op: "scope" }, function (m) {
      w.terminate();
      done(m.window === "undefined" && m.document === "undefined" && m.importScripts === "function" && m.isScope &&
           m.name === "scoped" && /\/worker-echo\.js$/.test(m.href) && m.lib === 21, JSON.stringify(m));
    });
  });

  step("blob worker", function (done) {
    var url = URL.createObjectURL(new Blob(["onmessage = function (e) { postMessage(e.data + ' from a blob'); };"]));
    var w = new Worker(url);
    ask(w, "hi", function (m) { w.terminate(); done(m === "hi from a blob", m); });
  });

  step("fetch in a worker", function (done) {
    var w = new Worker("worker-echo.js");
    ask(w, { op: "fetch", url: "worker-data.json" }, function (m) {
      w.terminate();
      done(m.value && m.value.answer === 42 && m.value.list.length === 3, JSON.stringify(m));
    });
  });

  step("XMLHttpRequest in a worker", function (done) {
    var w = new Worker("worker-echo.js");
    ask(w, { op: "xhr", url: "worker-data.json" }, function (m) {
      w.terminate();
      var ok = false;
      try { ok = m.status === 200 && JSON.parse(m.text).answer === 42; } catch (e) {}
      done(ok, JSON.stringify(m));
    });
  });

  step("uncaught error", function (done) {
    var w = new Worker("worker-echo.js");
    w.onerror = function (e) {
      w.terminate();
      done(e instanceof ErrorEvent && String(e.message).indexOf("worker failure 7") >= 0, e.message);
    };
    w.postMessage({ op: "throw", value: 7 });
  });

  step("syntax error", function (done) {
    var w = new Worker(URL.createObjectURL(new Blob(["this is not ( JavaScript"])));
    w.onerror = function (e) { w.terminate(); done(true); };
  });

  step("missing script", function (done) {
    var w = new Worker("no-such-worker.js");
    w.onmessage = function () { done(false, "a message came"); };
    w.onerror = function (e) { done(e.type === "error" && !(e instanceof ErrorEvent), String(e)); };
  });

  step("cross-origin script refused", function (done) {
    try { new Worker("http://elsewhere.example/w.js"); done(false, "no exception"); }
    catch (e) { done(e.name === "SecurityError", e.name); }
  });

  step("functions cannot be cloned", function (done) {
    var w = new Worker("worker-echo.js");
    try { w.postMessage(function () {}); done(false, "no exception"); }
    catch (e) { done(e.name === "DataCloneError", e.name); }
    w.terminate();
  });

  step("terminate", function (done) {
    var w = new Worker("worker-echo.js"), after = 0, stopped = false;
    w.onmessage = function (e) {
      if (stopped) { after++; return; }
      stopped = true;
      w.terminate();
      setTimeout(function () { done(after === 0, after + " messages after terminate()"); }, 300);
    };
    w.postMessage({ op: "tick" });
  });

  step("close() from inside", function (done) {
    var w = new Worker("worker-echo.js"), answered = false;
    w.onmessage = function (e) {
      if (e.data.op !== "closing") { answered = true; return; }
      w.postMessage({ op: "echo", value: 1 });
      setTimeout(function () { done(!answered, "answered after close()"); }, 300);
    };
    w.postMessage({ op: "close" });
  });

  step("module worker", function (done) {
    var w = new Worker("worker-module.js", { type: "module" });
    w.onerror = function (e) { done(false, e.message || "error"); };
    ask(w, 5, function (m) { w.terminate(); done(m.value === 15, JSON.stringify(m)); });
  });

  step("nested worker", function (done) {
    var w = new Worker("worker-echo.js");
    ask(w, { op: "nested", value: 21 }, function (m) { w.terminate(); done(m.value === 42, JSON.stringify(m)); });
  });

  step("MessageChannel in a worker", function (done) {
    var w = new Worker("worker-echo.js");
    ask(w, { op: "channel", value: "ping" }, function (m) { w.terminate(); done(m.value === "ping", JSON.stringify(m)); });
  });

  step("work off the page", function (done) {
    var w = new Worker("worker-echo.js");
    ask(w, { op: "primes", value: 20000 }, function (m) { w.terminate(); done(m.value === 2262, JSON.stringify(m)); });
  });

  step("many messages in order", function (done) {
    var w = new Worker(URL.createObjectURL(new Blob(["onmessage = function (e) { postMessage(e.data); };"])));
    var got = [];
    w.addEventListener("message", function (e) {
      got.push(e.data);
      if (got.length < 300) return;
      w.terminate();
      done(got.every(function (n, i) { return n === i; }), got.slice(0, 10).join());
    });
    for (var i = 0; i < 300; i++) w.postMessage(i);
  });

  step("a large message", function (done) {
    var big = new Uint8Array(1 << 20);
    for (var i = 0; i < big.length; i++) big[i] = i * 7;
    var w = new Worker("worker-echo.js");
    ask(w, { op: "echo", value: big }, function (m) {
      w.terminate();
      var v = m.value, ok = v instanceof Uint8Array && v.length === big.length;
      for (var i = 0; ok && i < v.length; i += 4099) ok = v[i] === big[i];
      done(ok, v && v.length);
    });
  });

  setTimeout(queue.shift(), 0);
}
