/* Benchmark: loops: arithmetic (make bench-js). */
test("loops: arithmetic", function () {
    var s = 0;
    for (var i = 0; i < 3000000; i++) s = (s + i * 3) % 1000003;
    return s;
}, 135);
