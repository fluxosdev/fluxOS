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

## Scrolling, skipped layout and view transitions

| Area | Implemented | Remaining work |
| --- | --- | --- |
| Scroll snapping | `scroll-snap-type` (x/y/block/inline/both, mandatory/proximity) on the root and scrolling boxes; `scroll-snap-align` (start/end/center, per axis); `scroll-snap-stop: always`; `scroll-padding` and `scroll-margin` (all longhands). Areas larger than the snapport can be scrolled through freely. Wheel, key, scroll bar and drag-edge scrolls settle 120 ms after they pause, gliding in the direction of travel; released scroll bar thumbs snap to the nearest position. `scrollTo`/`scrollBy`/`scrollTop`/`scrollLeft` land snapped immediately. After a relayout, each container re-snaps to the area it was resting on | `scroll-behavior: smooth`, scroll-padding for `scrollIntoView`/fragment navigation, scroll anchoring, `scrollsnapchange`/`scrollsnapchanging` events, keyboard scrolling of focused inner boxes, and `overflow: clip` boxes (which are wrongly treated as snap containers) |
| content-visibility | `content-visibility: hidden` always skips its contents; `auto` skips contents more than half a viewport away from the page window (or its scrolling box). Skipped boxes take `contain-intrinsic-size` / `-width` / `-height` (with `auto` remembering the last laid-out size). Focus, `#fragment` targets and `scrollIntoView()` force contents to render. Scrolling a skipped box into range triggers a relayout; layout repeats at once if a skipped box must show | Flex and grid items are positioned after their contents are laid out, so on first layout they are rendered and only skipped from the next layout onward. No `contentvisibilityautostatechange` event, find-in-page does not reveal skipped text, paint containment is not applied, and geometry queries inside skipped contents return empty boxes instead of forcing layout |
| View transitions | `document.startViewTransition(callback or {update, types})` with `ViewTransition` (`updateCallbackDone`, `ready`, `finished`, `skipTransition()`, `types`), `document.activeViewTransition`, the four-second update timeout, duplicate-name errors and skipping of an earlier transition. Old and new pictures of the page and of each `view-transition-name` element (`match-element`/`auto` included); each group moves and resizes from its old box to its new one while its pictures cross-fade (plus-lighter). `::view-transition-group/-image-pair/-old/-new(name, *, .class)` rules are styled each frame, so page `@keyframes`, durations, delays and easing apply; opacity and 2D transforms of the pictures are drawn. `:active-view-transition` and `:active-view-transition-type()` match during the transition | Cross-document (`@view-transition`) transitions, live (animating) new pictures, nested groups (`view-transition-group`), ink overflow outside the border box, iframe contents inside the pictures, group animations other than the built-in move, and pseudo-element properties beyond opacity and transform. Transitions on hidden tabs are skipped |

`make test-browser` covers these in `browser-layout` (snap positions, skipped layout, item
tagging and rendered transition frames) and `view-transition-tests.js` (the JavaScript API).
The layout and renderer tests also pass under AddressSanitizer and UndefinedBehaviorSanitizer.
Interactive behaviour (wheel gestures, glides and animation frames) has not been exercised in a
running fluxOS desktop.

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
