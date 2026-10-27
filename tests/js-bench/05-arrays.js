/* Benchmark: arrays: push, index, sort (make bench-js). */
test("arrays: push, index, sort", function () {
    var a = [];
    for (var i = 0; i < 200000; i++) a.push((i * 7919) % 200003);
    a.sort(function (x, y) { return x - y; });
    var s = 0;
    for (var j = 0; j < a.length; j += 3) s += a[j];
    return [a[0], a[a.length - 1], s];
}, [0, 200002, 6666649169]);
