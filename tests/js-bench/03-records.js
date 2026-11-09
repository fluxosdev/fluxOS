/* Benchmark: properties: objects as records (make bench-js). */
test("properties: objects as records", function () {
    var pts = [];
    for (var i = 0; i < 20000; i++) pts.push({ x: i, y: i * 2, z: i % 7, label: "p" + i, w: 1 });
    var sx = 0, sy = 0, sz = 0;
    for (var r = 0; r < 25; r++)
        for (var j = 0; j < pts.length; j++) { var p = pts[j]; sx += p.x; sy += p.y; sz += p.z * p.w; }
    return [sx, sy, sz];
}, [4999750000, 9999500000, 1499925]);
