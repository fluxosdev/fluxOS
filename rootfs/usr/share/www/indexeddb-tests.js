/* IndexedDB, run on the host by make test-browser (tests/browser-dom.c supplies test() and
 * runs the timers afterwards). Each asynchronous chain counts once when it reaches its end. */
function fail(m) { throw new Error(m); }

test("keys compare and ranges", function () {
  return [indexedDB.cmp(1, "a"), indexedDB.cmp(["a"], "z"), indexedDB.cmp(new Date(1), 2), indexedDB.cmp("b", "a"),
          IDBKeyRange.bound(1, 5, true).includes(1), IDBKeyRange.bound(1, 5).includes(5), IDBKeyRange.only("a").includes("a"),
          IDBKeyRange.lowerBound(3).includes(2)];
}, [-1, 1, 1, 1, false, true, true, false]);

indexedDB.deleteDatabase("t1").onsuccess = function () {
  var open = indexedDB.open("t1", 2);
  open.onupgradeneeded = function (e) {
    var db = open.result;
    if (e.oldVersion !== 0 || e.newVersion !== 2) fail("upgrade versions");
    var people = db.createObjectStore("people", { keyPath: "id", autoIncrement: true });
    people.createIndex("byName", "name");
    people.createIndex("byEmail", "email", { unique: true });
    people.createIndex("byTag", "tags", { multiEntry: true });
    db.createObjectStore("kv");
    people.add({ name: "Ann", email: "a@x", tags: ["x", "y"] });
    people.add({ name: "Bob", email: "b@x", tags: ["y"] });
    people.add({ name: "Ann", email: "c@x", tags: [] });
  };
  open.onsuccess = function () {
    var db = open.result;
    if (db.version !== 2 || !db.objectStoreNames.contains("people") || db.objectStoreNames.length !== 2) fail("open");
    var tx = db.transaction(["people", "kv"], "readwrite"), people = tx.objectStore("people"), kv = tx.objectStore("kv");
    kv.put({ when: new Date(7), list: [1, 2] }, "settings");
    kv.put("v2", ["compound", 1]);
    var dup = people.add({ name: "Eve", email: "a@x", tags: [] });
    dup.onerror = function (e) { if (dup.error.name !== "ConstraintError") fail("unique"); e.preventDefault(); };
    people.index("byName").getAll("Ann").onsuccess = function (e) { if (e.target.result.length !== 2) fail("index getAll"); };
    people.index("byTag").count("y").onsuccess = function (e) { if (e.target.result !== 2) fail("multiEntry"); };
    people.index("byEmail").getKey("b@x").onsuccess = function (e) { if (e.target.result !== 2) fail("index getKey"); };
    var seen = [];
    people.openCursor(null, "prev").onsuccess = function (e) {
      var c = e.target.result;
      if (c) { seen.push(c.key + c.value.name); c.continue(); }
      else if (seen.join() !== "3Ann,2Bob,1Ann") fail("cursor " + seen);
    };
    tx.oncomplete = function () {
      var r = indexedDB.open("t1");
      r.onsuccess = function () {
        var t2 = r.result.transaction("kv");
        t2.objectStore("kv").get("settings").onsuccess = function (e) {
          var v = e.target.result;
          if (!(v.when instanceof Date) || v.when.getTime() !== 7 || v.list[1] !== 2) fail("kv get");
        };
        t2.objectStore("kv").getAllKeys().onsuccess = function (e) {
          if (JSON.stringify(e.target.result) !== JSON.stringify(["settings", ["compound", 1]])) fail("key order");
        };
        t2.oncomplete = function () { count++; };
      };
    };
  };
};

indexedDB.deleteDatabase("t2").onsuccess = function () {
  var o = indexedDB.open("t2", 1);
  o.onupgradeneeded = function () { o.result.createObjectStore("s"); };
  o.onsuccess = function () {
    var db = o.result, tx = db.transaction("s", "readwrite");
    tx.objectStore("s").put(1, "a");
    tx.objectStore("s").put(2, "b").onsuccess = function () { tx.abort(); };
    tx.onabort = function () {
      var t = db.transaction("s");
      t.objectStore("s").count().onsuccess = function (e) { if (e.target.result !== 0) fail("abort rollback"); };
      t.oncomplete = async function () {
        /* Promise style: a request made after awaiting another is still in the transaction. */
        var t3 = db.transaction("s", "readwrite"), s = t3.objectStore("s");
        function p(req) { return new Promise(function (ok, no) { req.onsuccess = function () { ok(req.result); }; req.onerror = function () { no(req.error); }; }); }
        await p(s.put("x", 1));
        var v = await p(s.get(1));
        await p(s.put(v + "y", 2));
        t3.oncomplete = function () {
          db.transaction("s").objectStore("s").get(2).onsuccess = function (e) { if (e.target.result !== "xy") fail("await"); count++; };
        };
      };
    };
  };
};
