/* Benchmark: strings: building and searching (make bench-js). */
test("strings: building and searching", function () {
    var parts = [];
    for (var i = 0; i < 20000; i++) parts.push("item" + i);
    var s = parts.join(",");
    var n = 0;
    for (var k = 0; k < 2000; k++) n += s.indexOf("item" + (k * 10)) >= 0 ? 1 : 0;
    var t = "";
    for (var m = 0; m < 20000; m++) t += String.fromCharCode(97 + m % 26);
    var codes = 0;
    for (var c = 0; c < t.length; c++) codes += t.charCodeAt(c);
    return [s.length, n, t.length, codes];
}, [188889, 2000, 20000, 2189940]);
