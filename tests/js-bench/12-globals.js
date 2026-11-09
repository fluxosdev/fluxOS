/* Benchmark: built-ins and globals used from functions (make bench-js). */
var benchScale = 3, benchList = [];
function benchClamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
test("built-ins and globals", function () {
    var acc = 0;
    for (var i = 0; i < 300000; i++) {
        acc += benchClamp(Math.floor(i / benchScale) % 50, 5, 40);
        if (typeof JSON === "undefined" || !Array.isArray(benchList)) acc = -1;
    }
    return acc;
}, 7170000);
