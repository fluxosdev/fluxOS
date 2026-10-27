/* Benchmark: calls: recursive fib (make bench-js). */
test("calls: recursive fib", function () {
    function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
    return fib(26);
}, 121393);
