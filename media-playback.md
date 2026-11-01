# Video and YouTube in fluxOS

## What is there

| Part | Where | What it does |
| --- | --- | --- |
| YouTube in the Media Player | `user/desktop/media/ytresolve.c`, `httpio.c`, `mediaio.c`, `user/desktop/apps/mediaplayer.c` | `mediaplayer <YouTube link or http(s) URL>` finds the video's streams through YouTube's InnerTube API (ANDROID, then IOS client; `/etc/youtube.conf` can add clients or Invidious servers) and plays them, a muxed stream when there is one, else separate picture and sound streams read side by side. HTTP is read in Range chunks into a 24 MiB window, so seeking works. |
| "Play in Media Player" | `browser.c` context menu | On YouTube links, YouTube watch pages, links to media files and `<video>` elements with a plain URL. |
| `<video>` in Wave | `/bin/wavemedia` (`user/desktop/media/engine.c`), `browser.c`, `layout.c`, `jsdomlib.h` | Wave starts `/bin/wavemedia` in place of `wave --audio`. It still mixes `<audio>`, and also plays `<video>` from files, `blob:` URLs and http(s) addresses (with the page's cookies and referrer). Frames arrive in shared memory, scaled to the element's box, and only the video's rectangle is repainted when it is safe to. Sound is the clock. `controls` draws a bar. |
| Media Source Extensions | `engine.c`, `store.c`, `segments.c`, `jsdomlib.h` | `MediaSource`, `SourceBuffer`, `SourceBufferList`, `TimeRanges`, `createObjectURL(mediaSource)` and `srcObject`. Appends go to the engine in files and are demuxed by a persistent FFmpeg demuxer per SourceBuffer (fMP4 and WebM). A new initialization segment starts a new demuxer, and `abort()` puts the last one back in front. Coded frames are kept as GOPs with MSE's overlap and remove rules. `isTypeSupported` accepts H.264/AAC/Opus up to 720p30 and turns down VP9 and AV1, so sites pick the lighter H.264. |

## JavaScript engine fixes made on the way

These are general bugs that broke YouTube, and other sites too:

- Optional chains now short-circuit to their end (`a?.b.c` no longer throws).
- `{n,m}` in regular expressions compiles to nested optionals, so backtracking out of them is linear rather than exponential. YouTube's URL allow-list used to silently fail to match.
- `Date.parse` handles ISO 8601 offsets and fractions, and RFC 2822 dates.
- Dates in arithmetic use the "number" hint (`+new Date()`, `d1 - d0` and `new Date(s) / 1000` used to be NaN).
- `createElementNS` takes the name from its second argument; `namespaceURI` and the `*AttributeNS` methods were added.
- `document.implementation` was added.
- `HTMLMediaElement` is now the real parent of audio and video.
- Many interface objects were added (Window, ShadowRoot, Comment, NodeList, MediaError and others), so polyfills such as ShadyDOM can run.

## Still open

- **youtube.com itself.** The player now runs: it computes the stream `n` parameter correctly, gets 200 responses from googlevideo, and creates SourceBuffers and appends. The ES2021 build of YouTube's desktop app (`kevlar_base`, 9.7 MB of JS) then exhausts the script heap: the 2.6 MB player alone holds about 100 MB live. Getting the whole site to run needs Wave's JS engine to use much less memory per byte of source (lazy compilation, dropping ASTs after compiling, smaller bytecode). Until then, use "Play in Media Player".
- MSE `sequence` mode and append windows are accepted but not applied. There's no QuotaExceededError; old data is evicted instead.
- `playbackRate` other than 1 does not change playback speed.
- The YouTube clients the resolver uses change over time. Edit `/etc/youtube.conf` when they stop working.

## Tests (not all re-run after the last changes)

- `make test-media`: host tests (store, segments, JSON, YouTube links; `MEDIA_LIVE=1` asks YouTube itself).
- `make test-media-qemu`: the engine on the real kernel, with a network.
- `tools/test-wave-video-qemu.py fluxdisk.img build/tests/media-samples OUT`: Wave on the desktop.
- `rootfs/usr/share/www/mse-tests.js`, part of `make test-browser`.
- `build/tests/browser-page`: a debugging tool that runs a real page's scripts on the host.
