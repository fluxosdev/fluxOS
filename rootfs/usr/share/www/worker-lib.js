/* Loaded into worker-echo.js with importScripts(). */
function workerLib(n) { return n + 1; }
function countPrimes(limit) {
  var count = 0;
  for (var n = 2; n < limit; n++) {
    var prime = true;
    for (var d = 2; d * d <= n; d++) if (n % d === 0) { prime = false; break; }
    if (prime) count++;
  }
  return count;
}
