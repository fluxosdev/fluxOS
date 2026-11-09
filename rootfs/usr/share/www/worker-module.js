/* A module worker for worker-tests.js: new Worker("worker-module.js", { type: "module" }). */
import { triple } from "./worker-module-lib.js";
self.onmessage = function (e) { postMessage({ op: "module", value: triple(e.data) }); };
