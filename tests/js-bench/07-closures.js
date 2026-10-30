/* Benchmark: closures and higher-order functions (make bench-js). */
test("closures and higher-order functions", function () {
    var data = [];
    for (var i = 0; i < 200000; i++) data.push(i);
    var r = data.map(function (x) { return x * 2; }).filter(x => x % 3 === 0).reduce((a, b) => a + b, 0);
    var adders = [];
    for (let k = 0; k < 20000; k++) adders.push(v => v + k);
    var t = 0;
    for (var j = 0; j < adders.length; j++) t = adders[j](t) % 100003;
    return [r, t];
}, [13333266666, 84003]);
