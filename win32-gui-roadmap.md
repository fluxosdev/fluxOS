# fluxOS Windows GUI roadmap

*Written 2026-10-30, against commit 95a0cc4. Nothing here is done yet;
every stage is a plan.*

`winrun` already runs 32-bit console Windows programs: a PE loader, the
gate between the loader's flat segments and the program's segments, SEH,
and kernel32, ntdll and msvcrt filled in. This roadmap takes it the rest
of the way to programs with windows: a `WinMain` that registers a class,
creates a window and pumps messages gets a real window on the fluxOS
desktop, drawn through GDI, with the keyboard, the mouse and timers
arriving as Windows messages.

## Where we are

**Already in place:**

- `user/win32/stubs.c` names `user32_dll`, `gdi32_dll`, `comctl32_dll`,
  `comdlg32_dll`, `winmm_dll` and the rest as empty tables, so every
  import from them resolves to `api_missing_thunk` (logged, returns 0).
- The GUI hooks are declared in `win32.h` and stubbed in `stubs.c`:
  `gui_available`, `gui_pump`, `gui_exit`, `gui_wait_ms`,
  `gui_timers_due`, `gui_next_timer_ms`, `gui_connected`. `Sleep`
  already waits through `gui_wait_ms` when connected, and the exit paths
  (`process_exit`, `wdie`, the fault handler) already call `gui_exit`.
- `module_load` (`loader.c`) always takes the builtin user32 and gdi32
  over a DLL of the same name shipped beside the program.
- Callbacks into the program work and nest: `call_guest`,
  `call_guest1`..`call_guest4` (`gate.S`), used today by
  `EnumSystemLocales` and the unhandled exception filter from inside an
  API call.
- Resources: `find_res` behind `FindResource*`/`LoadResource`.
- `pe_file_info` gives the subsystem (2 is GUI, 3 is console).
- The desktop side: the window protocol (`shell/wmproto.h`), its
  connection plumbing (`shell/wmconn.c`: `wm_conn_*`,
  `shm_surface_attach`), drawing (`gfx.c`) and fonts, all in
  `libdesktop.a`.

**Missing:**

| Area | Gap |
|---|---|
| Window server connection | `gui.c`: none of the hooks do anything |
| user32 | empty table: no classes, windows, messages or painting |
| gdi32 | empty table: no DCs, objects or drawing |
| Threads | `CreateThread` is `api_unimplemented` (kernel32.c) |
| Linking | `winrun` links only `crt0.o`, its objects and `libc.a` |

## Decisions

**One top-level HWND is one desktop window.** It gets a `C_CREATE`, and
its pixels are the shared memory surface `S_BUFFER` hands back. Child
windows (controls, the area under a menu bar) never reach the desktop:
they are rectangles inside their top-level window's surface, kept and
hit-tested by winrun.

**`wmconn.c`, not `client.c`.** `client.c` owns the program's loop
(`app_main`) and calls `window_ops` from it; a Windows program owns its
own loop (`GetMessage`/`PeekMessage`). winrun speaks the protocol
itself over `wm_conn_*` and links `wmconn.o`, `gfx.o` and the font code
from `libdesktop.a`. Like `client.c`, it never handles desktop messages
inside a call that is waiting for an answer (`C_CREATE`, `C_RESIZE`,
`C_CLIP_GET`): what arrives meanwhile is queued and handled afterwards.

**The desktop draws the frame; winrun draws the rest of the non-client
area.** The caption, border and close button are the desktop's, so
windows match the theme. A menu bar and scroll bars are non-client in
Win32 but are drawn by winrun, inside the desktop's client surface. Each
top-level window keeps two rectangles: the Win32 window rect and the
desktop client rect (Win32 client plus menu bar). `AdjustWindowRect(Ex)`
and `GetSystemMetrics` (`SM_CYCAPTION`, `SM_CXFRAME`, ...) are computed
from the desktop's theme so that programs that size themselves get what
they asked for.

**`WM_PAINT` and `WM_TIMER` are never queued.** As on Windows, they are
generated when the queue is otherwise empty: `WM_PAINT` from a window's
update region, `WM_TIMER` from the timer heap. `BeginPaint` returns a DC
clipped to the update region and validates it; `EndPaint` sends
`C_DAMAGE` for it.

**Pixels need no conversion.** The desktop's 32-bit ARGB surface has the
same byte layout as a 32-bit Windows DIB (B, G, R, X in memory), so
`BitBlt` and top-down DIB blits are row copies. Bottom-up DIBs (positive
`biHeight`) flip; alpha is forced to 0xFF.

**Memory the program writes lives in guest memory.** `CreateDIBSection`
bits, `GlobalAlloc` clipboard data and anything else the program gets a
pointer to come from `vmem.c`, never from winrun's own heap or a desktop
segment.

**The builtin window classes are C, reachable as thunks.** `BUTTON`,
`EDIT`, `STATIC`, `LISTBOX`, `COMBOBOX` and `SCROLLBAR` have WndProcs
written in winrun, exposed to the program as `api_thunk` addresses so
that `GetWindowLong(GWL_WNDPROC)`, subclassing and `CallWindowProc`
work on them as on Windows.

**W inside, A at the edge.** user32 and gdi32 keep text as UTF-16; the
`...A` functions convert and call the `...W` ones.

**Single-threaded first, thread-keyed from the start.** Message queues
and the "current thread" in `GetMessage` are keyed by thread ID from
stage 0, so that stage 3's threads need no restructuring.

## Stage 0: a window

*Goal: a 40-line `WinMain` (register a class, create a window, pump
messages, paint text on a brush, quit on close) shows its window on the
desktop and exits cleanly when it is closed. Measured by that program
running in QEMU, and by its painted pixels matching a reference image in
the host test.*

- `user/win32/gui.c` replaces the GUI stubs in `stubs.c`: connect to
  `WM_SOCKET`, `C_HELLO`, `S_STATE`; `gui_pump` reads and translates
  server messages; `gui_wait_ms` is `poll()` on the socket with the
  next timer as its timeout; `gui_exit` destroys the program's windows.
- `user/win32/user32.c`: `RegisterClass(Ex)A/W`, `UnregisterClass`,
  `CreateWindowEx(A/W)`, `DestroyWindow`, `ShowWindow`, `UpdateWindow`,
  an HWND table (its own handle space, not kernel32's), `GetMessage`,
  `PeekMessage`, `TranslateMessage`, `DispatchMessage`, `PostMessage`,
  `SendMessage` (a direct `call_guest4` into the WndProc),
  `PostQuitMessage`, `DefWindowProc`, `BeginPaint`, `EndPaint`,
  `InvalidateRect`, `ValidateRect`, `GetClientRect`, `GetWindowRect`,
  `SetWindowText`/`GetWindowText` (`C_TITLE`), `GetWindowLong(Ptr)`/
  `SetWindowLong(Ptr)`, `LoadCursor`, `LoadIcon`, `GetSystemMetrics`,
  `AdjustWindowRect(Ex)`.
- `S_CLOSE` → `WM_CLOSE` → `DefWindowProc` → `DestroyWindow` →
  `WM_DESTROY`; `S_CONFIGURE` and `S_BUFFER` → `WM_SIZE`, `WM_MOVE`.
- `user/win32/gdi32.c`: a DC is a `struct surface` with an origin and a
  clip rectangle. `GetDC`, `ReleaseDC`, `GetStockObject`,
  `CreateSolidBrush`, `SelectObject`, `DeleteObject`, `FillRect`,
  `SetTextColor`, `SetBkColor`, `SetBkMode`, `TextOut`, `DrawText`
  (single line, `DT_CENTER`/`DT_VCENTER`), `GetTextExtentPoint32`,
  `GetTextMetrics`.
- Fonts: "MS Sans Serif", "Tahoma", "Segoe UI" and `DEFAULT_GUI_FONT`
  map onto the desktop's sans; `GetTextMetrics` comes from the same
  face and size that draws, so later dialog layout is right.
- A GUI-subsystem program with no desktop to connect to fails with a
  message on standard error instead of running windowless.
- Makefile: `winrun` links the `libdesktop.a` objects it needs.
- Tests: small PEs built on the host with `i686-w64-mingw32-gcc`, run
  through `os_linux.c` against a headless `gui.c` backend that renders
  into plain buffers for comparison with reference images.

## Stage 1: input, timers and pixels

*Goal: simple 2D games run: keyboard and mouse input, a frame timer, and
a back buffer blitted to the window each frame.*

- Keyboard: `S_KEY` → `WM_KEYDOWN`/`WM_KEYUP`/`WM_SYSKEYDOWN` with VK
  codes and scan codes in `lParam`; `TranslateMessage` → `WM_CHAR`.
  `GetKeyState`, `GetAsyncKeyState`, `GetKeyboardState`, `MapVirtualKey`.
- Mouse: `S_MOUSE` → `WM_MOUSEMOVE`, `WM_[LRM]BUTTON(DOWN|UP|DBLCLK)`
  (double clicks only for `CS_DBLCLKS` classes, with the desktop's
  double-click time), `S_WHEEL` → `WM_MOUSEWHEEL`. `SetCapture`,
  `ReleaseCapture`, `GetCursorPos`, `ScreenToClient`, `ClientToScreen`,
  `SetCursor` (mapped to `C_CURSOR`), `ShowCursor`.
- Focus: `S_FOCUS` → `WM_ACTIVATE`, `WM_SETFOCUS`, `WM_KILLFOCUS`;
  `SetFocus`, `GetFocus`, `GetActiveWindow`, `SetForegroundWindow`.
- Timers: `SetTimer`, `KillTimer`, timer procs; `timeGetTime` and
  `timeBeginPeriod` in winmm; `MsgWaitForMultipleObjects`.
- Pixels: `CreateCompatibleDC`, `CreateCompatibleBitmap`,
  `CreateDIBSection` (bits in guest memory), `BitBlt`, `StretchBlt`,
  `StretchDIBits`, `SetDIBitsToDevice`, `GetDIBits`, `PatBlt`, the
  common ROPs (`SRCCOPY`, `SRCAND`, `SRCPAINT`, `SRCINVERT`,
  `PATCOPY`), 8-bit DIBs with a palette.
- Shapes: pens, `MoveToEx`, `LineTo`, `Rectangle`, `Ellipse`,
  `Polygon`, `Polyline`, `SetPixel`, `GetPixel`.
- Borderless windows: `WS_POPUP` at the screen's size gives a
  full-screen window.

## Stage 2: controls, dialogs and menus

*Goal: Notepad-style programs run: a menu bar, an edit control filling
the window, dialogs from the program's resources, message boxes and the
clipboard.*

- Child windows: `WS_CHILD`, the window tree, z-order, clipping children
  out of the parent's painting (`WS_CLIPCHILDREN`), mouse hit-testing
  down the tree, `GetParent`, `GetDlgItem`, `EnumChildWindows`,
  `MoveWindow`, `SetWindowPos`, `WM_COMMAND` and `WM_NOTIFY` routing.
- `user/win32/controls.c`: `BUTTON` (push, check, radio, group box),
  `STATIC` (text, icon, frame), `EDIT` (single and multiline, selection,
  scrolling, undo of one step), `LISTBOX`, `COMBOBOX`, `SCROLLBAR`, and
  window scroll bars (`WS_HSCROLL`/`WS_VSCROLL`, `SetScrollInfo`),
  drawn in the desktop's theme.
- `user/win32/dialog.c`: `DLGTEMPLATE` and `DLGTEMPLATEEX` from
  resources, dialog units from the dialog font, `CreateDialogParam`,
  `DialogBoxParam` (a modal loop nested in the call), `EndDialog`,
  `IsDialogMessage` (Tab, Enter, Escape, mnemonics), `GetDlgItemText`,
  `SetDlgItemInt` and the rest.
- `MessageBox(A/W)`: a dialog built in code, modal, owner disabled.
- Menus: `LoadMenu`, `CreateMenu`, `AppendMenu`, `SetMenu`,
  `TrackPopupMenu`, `WM_COMMAND`, accelerators (`LoadAccelerators`,
  `TranslateAccelerator`). The menu bar is drawn inside the desktop
  client surface; drop-down menus are popup windows of their own.
- Clipboard: `OpenClipboard`, `SetClipboardData`/`GetClipboardData`
  for `CF_TEXT` and `CF_UNICODETEXT`, carried by `C_CLIP_SET` and
  `C_CLIP_GET`.
- The program's icon: `RT_GROUP_ICON` from the exe, as `C_ICON_IMAGE`
  (32 x 32) for the taskbar.

## Stage 3: threads and the common libraries

*Goal: real-world utilities run: programs that start threads, and that
use the common controls and the standard file dialogs.*

- Threads: `CreateThread`, `ExitThread`, `GetExitCodeThread`, thread
  handles waitable by `WaitForSingleObject`, on the libc's pthreads.
  Each thread needs a TEB behind its own `%fs` selector, which needs a
  kernel interface for per-thread segment descriptors (a selector per
  thread, or per-thread GDT/LDT slots switched on context switch).
  Critical sections, events, mutexes and semaphores that block for real.
  One message queue per thread; `PostThreadMessage`,
  `AttachThreadInput`; a lock around winrun's window and GDI tables.
- comctl32: `InitCommonControls(Ex)`, the status bar, toolbar,
  progress bar, trackbar, tab control, list view (report and icon
  modes), tree view, up-down, tooltips, image lists.
- comdlg32: `GetOpenFileName`/`GetSaveFileName` on the desktop's own
  file dialog (`dialog.h`), `ChooseColor`, `ChooseFont`.
- shell32: `ShellExecute` (open with fluxOS programs),
  `SHGetFolderPath`, `SHBrowseForFolder`, drag and drop of files
  (`DragAcceptFiles`, `WM_DROPFILES` from `S_DROP`).
- advapi32: a registry kept in a file (`RegOpenKeyEx`, `RegQueryValueEx`,
  `RegSetValueEx`, ...), which many programs read at start.

## Stage 4: OpenGL, sound and DirectDraw

*Goal: OpenGL demos and older games run.*

- `opengl32.dll` over `libGL.a`: `ChoosePixelFormat`,
  `SetPixelFormat`, `DescribePixelFormat`, `wglCreateContext`,
  `wglMakeCurrent`, `wglGetProcAddress`, `wglDeleteContext`,
  `SwapBuffers`. Every `gl*` entry point is a thin stdcall wrapper of
  the libGL function; `SwapBuffers` presents into the window's surface
  as the desktop's own GL programs do.
- winmm: `waveOutOpen`, `waveOutWrite`, `waveOutReset` and friends on
  the kernel's PCM device, with `WOM_DONE` callbacks; `PlaySound` for
  WAV files and resources; `midiOut*` on the OPL synthesizer.
- `ddraw.dll`: the COM interfaces games use (`DirectDrawCreate`,
  `IDirectDraw7`, primary and back surfaces, `Lock`/`Unlock` into guest
  memory, `Blt`, `Flip`, 8-bit palettes), presented through the same
  path as `StretchDIBits`.
- dsound: `DirectSoundCreate` with secondary buffers mixed onto the
  waveOut path.

## Order and dependencies

```
Stage 0 (gui.c, user32 + gdi32 core) ──> Stage 1 (input, timers, blits) ──┬──> Stage 2 (controls, dialogs, menus) ──> Stage 3 (comctl32, comdlg32, shell32)
                                                                          └──> Stage 4 (opengl32, winmm, ddraw)
Kernel per-thread segments ──> Stage 3 threads ──────────────────────────────────────────────────────────┘
```

Stage 4 needs only stage 1, so games and GL can come before controls
and dialogs if that is what there is to run. The kernel work for
per-thread `%fs` selectors is independent of everything above and can
be done at any time; stage 3 is the first that cannot go on without it.

## Later, deliberately

- **Direct3D.** `d3d8`/`d3d9` over libGL is a large translation layer
  (shaders, state blocks, formats); OpenGL and DirectDraw cover the
  programs that come first.
- **64-bit programs.** fluxOS is a 32-bit system; PE32+ needs a 64-bit
  kernel first.
- **Owner-drawn and themed controls.** uxtheme stays an empty table;
  controls draw in the desktop's theme, not Windows'.
- **Out-of-process COM, DCOM, services, printing.** winspool, rpcrt4
  and the service manager stay stubs.
- **Installers.** Most are their own GUI programs and come for free
  once stages 2 and 3 work; those that need MSI (`msiexec`) do not.
