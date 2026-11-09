/* Benchmark: json and object churn (make bench-js). */
test("json and object churn", function () {
    var total = 0;
    for (var i = 0; i < 3000; i++) {
        var o = JSON.parse(JSON.stringify({ id: i, tags: ["a", "b"], nested: { v: i * 2 } }));
        total += o.nested.v + o.tags.length;
    }
    return total;
}, 9003000);
