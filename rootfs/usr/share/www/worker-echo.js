/* A classic worker for workers.html and worker-tests.js: it does what each message asks. */
importScripts("worker-lib.js");
var ticks = 0;
onmessage = function (e) {
  var m = e.data;
  switch (m && m.op) {
  case "echo":
    postMessage({ op: "echo", value: m.value });
    break;
  case "scope":
    postMessage({ op: "scope", window: typeof window, document: typeof document, importScripts: typeof importScripts,
      isScope: self instanceof WorkerGlobalScope && self instanceof DedicatedWorkerGlobalScope && self === globalThis,
      name: self.name, href: location.href, lib: typeof workerLib === "function" ? workerLib(20) : null });
    break;
  case "fetch":
    fetch(m.url).then(function (r) { return r.json(); }).then(
      function (value) { postMessage({ op: "fetch", value: value }); },
      function (err) { postMessage({ op: "fetch", error: String(err) }); });
    break;
  case "xhr":
    var x = new XMLHttpRequest();
    x.open("GET", m.url);
    x.onload = function () { postMessage({ op: "xhr", status: x.status, text: x.responseText }); };
    x.onerror = function () { postMessage({ op: "xhr", status: -1, text: "" }); };
    x.send();
    break;
  case "throw":
    setTimeout(function () { throw new Error("worker failure " + m.value); }, 0);
    break;
  case "tick":
    setInterval(function () { postMessage({ op: "tick", n: ++ticks }); }, 20);
    break;
  case "close":
    postMessage({ op: "closing" });
    close();
    break;
  case "channel":
    var ch = new MessageChannel();
    ch.port2.onmessage = function (ev) { postMessage({ op: "channel", value: ev.data }); };
    ch.port1.postMessage(m.value);
    break;
  case "nested":
    var url = URL.createObjectURL(new Blob(["onmessage = function (e) { postMessage(e.data * 2); };"], { type: "text/javascript" }));
    var sub = new Worker(url);
    sub.onmessage = function (ev) { postMessage({ op: "nested", value: ev.data }); sub.terminate(); };
    sub.postMessage(m.value);
    break;
  case "primes":
    postMessage({ op: "primes", value: countPrimes(m.value) });
    break;
  }
};
