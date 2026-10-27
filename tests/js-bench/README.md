# JavaScript engine benchmarks

Run `make bench-js` from the repository root. Each file is a separate script
entry with a checked result and its own step budget. The runner prints wall
time and peak resident memory. It uses the production VM at `-O2`, real garbage
collection, and the Linux host stack mapping; per-instruction assertions are
reserved for `make test-js` and `make test-browser`.

These cases exercise calls, arithmetic loops, object records, object maps,
arrays and sorting, strings, closures, classes, JSON, and switches. They measure
engine workloads including startup and compilation, not complete page loading.
The records case keeps its live set within the runner's 48 MiB heap limit.

## Reference comparison

On 2026-10-27, the median of three native Linux runs per case, with GCC 13.3.0
at `-O2`, compared this VM against the tree-walker from commit
`08f8125bac759a13cc6270a8321c3324194cd3f1`. The reference runner received the same
host stack mapping and ELF root-symbol fixes so that both actually collected
memory. Both runners checked every result.

| Workload | Tree-walker | VM | Speedup |
|---|---:|---:|---:|
| Recursive calls | 0.071 s | 0.044 s | 1.60x |
| Arithmetic loops | 0.544 s | 0.132 s | 4.12x |
| Object records | 0.548 s | 0.103 s | 5.34x |
| Arrays and sorting | 0.479 s | 0.164 s | 2.93x |
| Classes and allocation | 0.375 s | 0.165 s | 2.27x |
| Switches | 0.144 s | 0.048 s | 3.01x |

These are host measurements. They do not predict Pentium III timings or the
speed of pages limited by layout, networking, or native built-ins.
