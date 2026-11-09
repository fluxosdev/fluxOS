/* Benchmark: methods and getters through class hierarchies, several shapes per site (make bench-js). */
test("methods through class hierarchies", function () {
    class Node { constructor(id) { this.id = id; this.dirty = false; } size() { return 1; } get weight() { return this.size() * 2; } }
    class Text extends Node { constructor(id, s) { super(id); this.text = s; } size() { return this.text.length; } }
    class Box extends Node { constructor(id, w, h) { super(id); this.w = w; this.h = h; } size() { return this.w * this.h; } }
    class Row extends Box { constructor(id) { super(id, 3, 1); this.kids = 0; } }
    var nodes = [];
    for (var i = 0; i < 3000; i++)
        nodes.push(i % 4 === 0 ? new Text(i, "abc") : i % 4 === 1 ? new Box(i, 2, 2) : i % 4 === 2 ? new Row(i) : new Node(i));
    var total = 0;
    for (var r = 0; r < 60; r++)
        for (var j = 0; j < nodes.length; j++) {
            var n = nodes[j];
            total += n.weight + n.size() + (n.dirty ? 1 : 0);
            n.dirty = !n.dirty;
        }
    return total;
}, 1575000);
