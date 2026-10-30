/* Benchmark: properties: wide objects as maps (make bench-js). */
test("properties: wide objects as maps", function () {
    var o = {};
    for (var i = 0; i < 10000; i++) o["key" + i] = i;
    var s = 0;
    for (var r = 0; r < 20; r++) for (var j = 0; j < 10000; j++) s += o["key" + j];
    return s;
}, 999900000);
