group("The top layer: popovers, dialogs and invoker buttons");
function place(html) {
  var box = document.createElement("div");
  box.innerHTML = html;
  document.body.appendChild(box);
  return box;
}

test("showPopover and hidePopover, with beforetoggle", function () {
  var box = place("<div id=p1 popover>x</div>"), p = box.firstChild, log = [];
  p.addEventListener("beforetoggle", function (e) { log.push(e.oldState + ">" + e.newState + ":" + e.cancelable); });
  p.showPopover();
  var open = [p.matches(":popover-open"), document.querySelectorAll(":popover-open").length];
  p.hidePopover();
  box.remove();
  return [open, p.matches(":popover-open"), log.join(" "), p.popover];
}, [[true, 1], false, "closed>open:true open>closed:false", "auto"]);

test("beforetoggle can stop a popover opening", function () {
  var box = place("<div popover>x</div>"), p = box.firstChild;
  p.addEventListener("beforetoggle", function (e) { e.preventDefault(); });
  p.showPopover();
  var r = p.matches(":popover-open");
  box.remove();
  return r;
}, false);

test("an auto popover closes the others, but not the one it is inside", function () {
  var box = place("<div id=a popover>a<div id=b popover>b</div></div><div id=c popover>c</div>");
  var a = box.querySelector("#a"), b = box.querySelector("#b"), c = box.querySelector("#c");
  a.showPopover(); b.showPopover();
  var nested = [a.matches(":popover-open"), b.matches(":popover-open")];
  c.showPopover();
  var r = [nested, a.matches(":popover-open"), b.matches(":popover-open"), c.matches(":popover-open")];
  box.remove();
  return r;
}, [[true, true], false, false, true]);

test("closing a popover closes the ones opened inside it", function () {
  var box = place("<div id=a popover>a<div id=b popover>b</div></div>");
  var a = box.querySelector("#a"), b = box.querySelector("#b");
  a.showPopover(); b.showPopover(); a.hidePopover();
  var r = [a.matches(":popover-open"), b.matches(":popover-open")];
  box.remove();
  return r;
}, [false, false]);

test("manual popovers stay open beside others", function () {
  var box = place("<div id=m popover=manual>m</div><div id=a popover>a</div>");
  var m = box.querySelector("#m"), a = box.querySelector("#a");
  m.showPopover(); a.showPopover();
  var r = [m.popover, m.matches(":popover-open"), a.matches(":popover-open")];
  box.remove();
  return r;
}, ["manual", true, true]);

test("togglePopover answers whether it is open, with force", function () {
  var box = place("<div popover=auto>x</div>"), p = box.firstChild;
  var r = [p.togglePopover(), p.togglePopover(), p.togglePopover(true), p.togglePopover(true),
           p.togglePopover({ force: false }), p.togglePopover(false)];
  box.remove();
  return r;
}, [true, false, true, true, false, false]);

test("popover methods throw for elements that cannot be shown", function () {
  var r = [];
  try { document.createElement("div").showPopover(); } catch (e) { r.push(e.name); }
  var loose = document.createElement("div");
  loose.setAttribute("popover", "");
  try { loose.showPopover(); } catch (e) { r.push(e.name); }
  try { document.createElement("div").hidePopover(); } catch (e) { r.push(e.name); }
  return r;
}, ["NotSupportedError", "InvalidStateError", "NotSupportedError"]);

test("an open popover closes when it leaves the document or stops being one", function () {
  var box = place("<div id=a popover>a</div><div id=b popover>b</div>");
  var a = box.querySelector("#a"), b = box.querySelector("#b");
  a.showPopover();
  a.remove();
  var gone = a.matches(":popover-open");
  b.showPopover();
  b.removeAttribute("popover");
  var r = [gone, b.matches(":popover-open"), b.popover];
  box.remove();
  return r;
}, [false, false, null]);

test("popovertarget buttons toggle, show and hide their popover", function () {
  var box = place("<button id=t popovertarget=tp>t</button><button id=s popovertarget=tp popovertargetaction=show>s</button>" +
                  "<button id=h popovertarget=tp popovertargetaction=hide>h</button><div id=tp popover>x</div>");
  var p = box.querySelector("#tp"), r = [];
  box.querySelector("#t").click(); r.push(p.matches(":popover-open"));
  box.querySelector("#s").click(); r.push(p.matches(":popover-open"));
  box.querySelector("#t").click(); r.push(p.matches(":popover-open"));
  box.querySelector("#h").click(); r.push(p.matches(":popover-open"));
  box.querySelector("#s").click(); box.querySelector("#h").click(); r.push(p.matches(":popover-open"));
  var t = box.querySelector("#t");
  r.push(t.popoverTargetElement === p, t.popoverTargetAction, box.querySelector("#s").popoverTargetAction);
  box.remove();
  return r;
}, [true, true, false, false, false, true, "toggle", "show"]);

test("a popover's button keeps its popover open inside another", function () {
  var box = place("<div id=outer popover><button id=go popovertarget=inner>go</button></div><div id=inner popover>in</div>");
  var outer = box.querySelector("#outer"), inner = box.querySelector("#inner");
  outer.showPopover();
  box.querySelector("#go").click();
  var r = [outer.matches(":popover-open"), inner.matches(":popover-open")];
  box.remove();
  return r;
}, [true, true]);

test("a cancelled click does not toggle, nor does a submit button in a form", function () {
  var box = place("<button id=b popovertarget=p>b</button><form><button id=f popovertarget=p>f</button></form><div id=p popover>x</div>");
  var p = box.querySelector("#p"), b = box.querySelector("#b");
  b.addEventListener("click", function (e) { e.preventDefault(); });
  b.click();
  box.querySelector("#f").addEventListener("click", function (e) { e.preventDefault(); });
  box.querySelector("#f").click();
  var r = p.matches(":popover-open");
  box.remove();
  return r;
}, false);

test("showModal: :modal, and everything outside the dialog is inert", function () {
  var box = place("<button id=out>out</button><dialog id=d><button id=in>in</button></dialog>");
  var d = box.querySelector("#d"), hits = [];
  box.querySelector("#out").onclick = function () { hits.push("out"); };
  box.querySelector("#in").onclick = function () { hits.push("in"); };
  d.showModal();
  box.querySelector("#out").click();
  box.querySelector("#in").click();
  var r = [d.open, d.matches(":modal"), d.matches(":open"), hits.join(), document.activeElement === box.querySelector("#in")];
  d.close();
  box.querySelector("#out").click();
  r.push(d.matches(":modal"), d.open, hits.join());
  box.remove();
  return r;
}, [true, true, true, "in", true, false, false, "in,out"]);

test("close(value) sets returnValue and fires close; show() is not modal", function () {
  var box = place("<dialog id=d>x</dialog>"), d = box.firstChild, log = [];
  d.addEventListener("close", function () { log.push("close:" + d.returnValue); });
  d.show();
  var shown = [d.open, d.matches(":modal")];
  d.close("done");
  d.close("again");
  var r = [shown, d.open, d.returnValue, log.join()];
  box.remove();
  return r;
}, [[true, false], false, "done", "close:done"]);

test("showModal on a dialog open without it throws; on a modal one it does nothing", function () {
  var box = place("<dialog id=d>x</dialog>"), d = box.firstChild, r = [];
  d.show();
  try { d.showModal(); r.push("no throw"); } catch (e) { r.push(e.name); }
  d.close();
  d.showModal();
  try { d.showModal(); r.push("fine"); } catch (e) { r.push("threw"); }
  d.close();
  box.remove();
  return r;
}, ["InvalidStateError", "fine"]);

test("requestClose fires cancel, which can keep the dialog open", function () {
  var box = place("<dialog id=d>x</dialog>"), d = box.firstChild, log = [], keep = true;
  d.addEventListener("cancel", function (e) { log.push("cancel"); if (keep) e.preventDefault(); });
  d.showModal();
  d.requestClose("first");
  var kept = d.open;
  keep = false;
  d.requestClose("second");
  var r = [kept, d.open, d.returnValue, log.join(), d.closedBy];
  box.remove();
  return r;
}, [true, false, "second", "cancel,cancel", "none"]);

test("commandfor buttons: show-modal, close with a value, popovers and custom commands", function () {
  var box = place("<button id=open commandfor=d command=show-modal>o</button>" +
                  "<dialog id=d><button id=close commandfor=d command=close value=ok>c</button></dialog>" +
                  "<button id=tp commandfor=p command=toggle-popover>p</button><div id=p popover>x</div>" +
                  "<button id=custom commandfor=p command=--spin>s</button>");
  var d = box.querySelector("#d"), p = box.querySelector("#p"), seen = [];
  p.addEventListener("command", function (e) { seen.push(e.command + ":" + (e.source === box.querySelector("#custom") || e.source === box.querySelector("#tp"))); });
  box.querySelector("#open").click();
  var modal = d.matches(":modal");
  box.querySelector("#close").click();
  box.querySelector("#tp").click();
  var popped = p.matches(":popover-open");
  box.querySelector("#custom").click();
  var r = [modal, d.open, d.returnValue, popped, p.matches(":popover-open"), seen.join(),
           box.querySelector("#open").commandForElement === d, box.querySelector("#open").command];
  box.remove();
  return r;
}, [true, false, "ok", true, true, "toggle-popover:true,--spin:true", true, "show-modal"]);

test("a popover's autofocus descendant gets the focus, which goes back when it closes", function () {
  var box = place("<button id=b>b</button><div id=p popover><input id=i autofocus></div>");
  var b = box.querySelector("#b"), p = box.querySelector("#p");
  b.focus();
  p.showPopover();
  var inside = document.activeElement === box.querySelector("#i");
  p.hidePopover();
  var r = [inside, document.activeElement === b];
  box.remove();
  return r;
}, [true, true]);

(function () {
  var box = place("<div id=p popover>x</div>"), p = box.firstChild, log = [];
  p.addEventListener("toggle", function (e) { log.push(e.oldState + ">" + e.newState); });
  p.showPopover();
  p.hidePopover();
  p.showPopover();
  var during = log.length;
  setTimeout(function () {
    test("toggle comes afterwards, once for the changes in between", function () {
      p.hidePopover();
      box.remove();
      return [during, log.join()];
    }, [0, "closed>open"]);
  }, 0);
})();
