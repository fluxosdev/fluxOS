# JavaScript engine benchmarks

Run `make bench-js` from the repository root. Each file is a separate script
entry with a checked result and its own step budget. The runner prints wall
time and peak resident memory. It uses the production VM at `-O2`, real garbage
collection, and the Linux host stack mapping; per-instruction assertions are
reserved for `make test-js` and `make test-browser`.

These cases exercise calls, arithmetic loops, object records, object maps,
arrays and sorting, strings, closures, classes, JSON, switches, methods and
getters through class hierarchies over several shapes, and built-ins and
globals used from functions. They measure
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

## Shapes and inline caches

On 2026-10-31, the median of five native Linux runs per case, with GCC 13.3.0 at
`-O2`, alternating with runs of commit `4151a96` (properties as per-object lists
and hash tables, no inline caches). Both runners checked every result.

| Workload | Before | After | Speedup |
|---|---:|---:|---:|
| Object records | 0.12 s | 0.08 s | 1.50x |
| Classes and allocation | 0.21 s | 0.13 s | 1.62x |
| Methods through class hierarchies | 0.11 s | 0.07 s | 1.57x |
| Built-ins and globals | 0.23 s | 0.11 s | 2.09x |
| Wide objects as maps | 0.07 s | 0.07 s | 1.00x |
| JSON and object churn | 0.02 s | 0.02 s | 1.00x |

Cases that do little with properties (calls, arithmetic loops, switches) also
ran 1.3x to 1.5x faster in this comparison, which the property changes do not
explain; treat those as differences in how the compiler laid out the VM. The
heap after start-up and a collection grew from 2.7 MB to 3.1 MB on the host,
mostly the shared second-level cache and the shapes' tables.
