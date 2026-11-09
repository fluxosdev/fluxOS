/* Benchmark: classes: methods and allocation (make bench-js). */
test("classes: methods and allocation", function () {
    class Vec {
        constructor(x, y) { this.x = x; this.y = y; }
        add(o) { return new Vec(this.x + o.x, this.y + o.y); }
        dot(o) { return this.x * o.x + this.y * o.y; }
        get len2() { return this.dot(this); }
    }
    var acc = new Vec(0, 0);
    for (var i = 0; i < 200000; i++) acc = acc.add(new Vec(1, i % 3));
    return [acc.x, acc.y, new Vec(3, 4).len2];
}, [200000, 199999, 25]);
