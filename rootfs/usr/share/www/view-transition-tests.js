group("view transitions: document.startViewTransition() (the harness takes the pictures and ends the animations)");

test("the API is there, with nothing running", function () {
  return [typeof document.startViewTransition, typeof ViewTransition, document.activeViewTransition];
}, ["function", "function", null]);

function settled(p) {
  return p.then(function (v) { return "ok"; }, function (e) { return e && e.name ? e.name : String(e); });
}
function outcome(vt) {
  return Promise.all([settled(vt.updateCallbackDone), settled(vt.ready), settled(vt.finished)]);
}

var order = [];
var first = document.startViewTransition(function () { order.push("update"); document.body.className = "after"; });
order.push("returned");
test("it starts at once; its update runs after the call returns", function () {
  return [document.activeViewTransition === first, order.join(","), first instanceof ViewTransition,
          first.types.size, first.transitionRoot === document.documentElement];
}, [true, "returned", true, 0, true]);
first.updateCallbackDone.then(function () { order.push("updateCallbackDone"); });
first.ready.then(function () { order.push("ready"); });
first.finished.then(function () { order.push("finished"); });

outcome(first).then(function (r) {
  test("ready once the new pictures are taken, finished when the animation is over", function () {
    return [r, order.join(","), document.body.className, document.activeViewTransition];
  }, [["ok", "ok", "ok"], "returned,update,updateCallbackDone,ready,finished", "after", null]);

  /* skipped before its update: no animation, but the update still runs */
  var ran = false, early = document.startViewTransition(function () { ran = true; });
  early.skipTransition();
  return outcome(early).then(function (r) {
    test("skipTransition() before the update: ready rejects, the update runs, finished resolves", function () {
      return [r, ran, document.activeViewTransition];
    }, [["ok", "AbortError", "ok"], true, null]);
  });
}).then(function () {
  /* a new transition skips the one running */
  var a = document.startViewTransition(function () {}), b = document.startViewTransition({
    update: function () {}, types: ["slide", "forwards"] });
  return Promise.all([outcome(a), outcome(b)]).then(function (r) {
    test("a second transition skips the first; types are kept", function () {
      return [r[0], r[1], Array.from(b.types)];
    }, [["ok", "AbortError", "ok"], ["ok", "ok", "ok"], ["slide", "forwards"]]);
  });
}).then(function () {
  var boom = new Error("boom");
  var bad = document.startViewTransition(function () { throw boom; });
  return Promise.all([outcome(bad), bad.finished.catch(function (e) { return e === boom; })]).then(function (r) {
    test("an update that throws rejects all three with its error", function () {
      return [r[0], r[1], document.activeViewTransition];
    }, [["Error", "Error", "Error"], true, null]);
  });
}).then(function () {
  var dup = document.startViewTransition({ update: function () {}, types: ["dup"] });
  var failed = document.startViewTransition({ update: function () {}, types: ["fail"] });
  return Promise.all([outcome(dup), outcome(failed)]).then(function (r) {
    test("pictures that cannot be taken (a name used twice) skip it with InvalidStateError", function () {
      return r;
    }, [["ok", "InvalidStateError", "ok"], ["ok", "InvalidStateError", "ok"]]);
  });
}).then(function () {
  var waited = false;
  var slow = document.startViewTransition(function () {
    return Promise.resolve().then(function () { waited = true; });
  });
  return slow.ready.then(function () {
    var during = document.activeViewTransition === slow;
    slow.skipTransition();
    return outcome(slow).then(function (r) {
      test("an update that returns a promise is waited for; skipped while animating, finished still resolves", function () {
        return [waited, during, r, document.activeViewTransition];
      }, [true, true, ["ok", "ok", "ok"], null]);
    });
  });
}).then(function () {
  var plain = document.startViewTransition();
  return outcome(plain).then(function (r) {
    test("no update callback at all", function () { return r; }, ["ok", "ok", "ok"]);
  });
});
