# Wave performance work

The browser now avoids redundant geometry flushes, batches poll-driven paints, throttles
hidden-page visual work, caches text shaping/widths, and reuses opacity scratch storage.

## Rendering and scheduling

- Synchronous geometry flushes discover changes only when the DOM is dirty. Pending style
  or geometry work still runs before a synchronous read returns.
- Background-color-only hover/focus restyles and CSS animation frames can update existing
  background fills without rebuilding geometry. Changes to sizing, text color, generated
  content, images, complex backgrounds, or unsupported boxes keep the full-layout path.
  This is a narrow invalidation optimization; general subtree layout remains future work.
- Paint requests during a browser poll are combined into one paint per visible window,
  including polls that return early for navigation or tab changes. Input outside the poll
  keeps its immediate rendering and the existing scroll-strip shortcut.
- Hidden tabs and minimized windows skip animated image decoding, CSS animation frames,
  and deferred visual layout. Ordinary hidden-page timers have a one-second minimum delay;
  requestAnimationFrame pauses until visible. Messages, networking, workers, and audio
  continue. Visibility changes and explicit geometry reads still take effect normally.

## Bounded caches

Text uses 512 width entries and 128 shaping entries, keyed by the original bytes, with
byte comparison after hashing. Strings above 256 bytes bypass these caches. Width keys
also include the font and a generation invalidated when downloaded fonts are registered
or released. Drawing and measurement use the same shaped text. A cache miss or collision
falls back to the normal font path.

Opacity groups reuse buffers across paints, with separate storage for active nested
groups. Retained storage is capped at 16 MiB across eight slots. Larger groups and deeper
nesting use transient storage; the existing rendering size limits remain in force.

## Profiling and benchmarks

Launch Wave with `WAVE_PROFILE=1 /bin/wave <URL>` to emit cumulative stage timings and cache
counters to stderr about every five seconds while the event loop is active. Profiling is
off by default. Stages cover script execution/timer dispatch, style, layout, font work,
and full-window painting. Script dispatch can include synchronous style/layout work;
text time is included in layout/paint time, so stage totals must not be added together.
Counters report width/shaping/buffer reuse, geometry reuse, skipped redundant flushes,
merged paints, and skipped hidden-page work. This is diagnostic instrumentation, not a
sampling profiler or an end-to-end page-load measurement.

`make bench-browser-text` runs three host measurements of 250,000 production font-width
calls over six repeated Latin/Arabic/Hebrew strings. In one local before/after comparison,
the previous implementation took 77–99 ms and the cached implementation took 11–25 ms;
both produced checksum 22958455. This synthetic repeated-text result does not predict
whole-page speedups. The command preserves its font fixture and disables profiling.

## Validation

Regression coverage checks fast-path pixels against a fresh full layout, geometry fallback,
nested opacity buffer reuse, hidden timer/animation behavior, responsive hidden-page
messaging, mutable text keys, and downloaded-font cache lifetime. Run `make test-browser`,
`make test-browser-fonts`, and `make build/rootfs/bin/wave`.

The full browser suite, real-font tests, benchmark command, and native 32-bit Wave build
passed for this change. Layout regressions also passed AddressSanitizer and
UndefinedBehaviorSanitizer with leak detection disabled. Profiling was smoke-tested both
enabled and disabled. Interactive behavior in a running fluxOS desktop was not exercised.
