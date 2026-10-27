/* Benchmark: switch and string compare (make bench-js). */
test("switch and string compare", function () {
    var kinds = ["div", "span", "a", "p", "img"], n = 0;
    for (var i = 0; i < 500000; i++) {
        switch (kinds[i % 5]) {
        case "div": n += 1; break;
        case "span": n += 2; break;
        case "a": n += 3; break;
        default: n += 4;
        }
    }
    return n;
}, 1400000);
