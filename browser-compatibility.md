# Wave compatibility work

This change works on the nine areas after the first six in the browser gap review.
It does not add video/Web Audio, WebAssembly, WebGL/WebGPU, service workers,
WebRTC/capture, or additional cryptography/passkey APIs.

| Area | Implemented in this change | Remaining work |
| --- | --- | --- |
| JavaScript | Callable proxies with `apply` and `construct` traps; direct eval can read/write captured local bindings, preserves const writes and loop closures; indirect eval remains global | Complete proxy invariants/revocation, strict eval and declaration edge cases, remaining Unicode regexp string sets |
| Shared memory and transfers | Native shared segments with unguessable import capabilities; shared-buffer clones preserve storage, including across dedicated workers; atomic integer operations and worker wait/notify; ArrayBuffer transfers detach the buffer and existing views; transfer lists work in worker/window/MessagePort messages | Shared-memory allocation is bounded to 128 segments/64 MiB per process and retained until process exit; worker-created segments require the creator process to remain alive until the recipient maps them. Cross-origin isolation headers and `Atomics.waitAsync` are not implemented. Other transferable types remain missing |
| Streams | Byte streams, BYOB reads and producer requests, typed alignment/minimum handling; fetch bodies are byte streams; unread window fetches pause helper pipe delivery at the queue high-water mark | Full Streams conformance, auto-allocation, transferable streams, worker transport backpressure and the wget fallback |
| Cross-window APIs | Origin-scoped BroadcastChannel delivery across page engines, with independently deserialized values and lifecycle cleanup | Same-origin popup document access; broadcasts between separate Wave application processes and dedicated workers |
| Text | Contextual Arabic forms and lam-alef ligatures; basic Hebrew/Arabic visual runs with LTR digit groups; font measurement and drawing use the same shaping path | Full Unicode bidi, paragraph-level ordering, OpenType shaping, Indic scripts and combined emoji sequences |
| CSS/layout | Integer line-name repeats in subgrid; group opacity composites overlapping children together, including nested groups and selection/search highlights | `preserve-3d`, depth sorting, named/auto-placed subgrid edge cases, nested sticky positioning, full filter/backdrop/iframe compositing and scroll snapping. Scratch buffers remain bounded |
| Storage | Checksummed append journal for small IndexedDB changes, atomic snapshot compaction, torn-tail recovery, quota errors and disk usage estimates | The logical database remains resident in memory and is serialized for each commit; multi-window transactions and very large databases need a different storage engine. Persistent-storage requests remain denied |
| Security policies | Multiple CSP headers/meta source lists intersect; resource/inline script/nonce/eval/attribute-handler/string-timer checks; form-action and base-uri checks; sandbox restrictions for scripts, forms, storage/cookies, popups and top navigation | This is a CSP source-list subset, not complete CSP3. Hashes, strict-dynamic, reporting, inline CSS policy, frame-ancestors, Trusted Types enforcement and full opaque-origin networking remain missing. Opaque sandbox XHR/fetch is conservatively denied |
| Images/SVG | Animated WebP frames, source-over/no-blend, disposal, timing and loop counts, including decoder-helper adoption; SVG clip paths and luminance/alpha masks | AVIF, SVG filters/patterns, stylesheet effect references, accurate objectBoundingBox behavior under arbitrary transforms and complete mask region semantics |

## Verification

`make test-browser` includes the platform, cross-page/CSP, worker shared-memory,
stream backpressure, storage recovery, vector/text and image animation regressions.
`make test-browser-fonts` exercises the real FreeType/font path.

Validated with `make -j2 test-browser`, `make test-browser-fonts`, and
`make build/rootfs/bin/wave`. The full browser suite passes, including 19 new
platform cases and 21 worker cases. The native 32-bit executable links successfully.
Layout and SVG/text regressions also pass AddressSanitizer and UndefinedBehaviorSanitizer
checks with leak detection disabled (the environment does not support LeakSanitizer).
These checks exercise the implemented subsets; they do not establish full web-platform
conformance or replace testing in a running fluxOS session.
