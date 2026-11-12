# fluxOS Windows GUI roadmap

*Written 2026-10-30, against commit 95a0cc4. Stages 0 and 1 are done (see
their sections for where they differ from the plan). Stage 2 now has
its basic controls, dialogs, menus, clipboard and resource icons; see its
section for the tested workflow and remaining compatibility limits. Stage 3
now has its basic threads, common controls, standard dialogs,
shell integration and Windows prefix. Stage 4 now has its basic OpenGL,
PCM/MIDI, DirectDraw and DirectSound workflows; see its tested subset and
compatibility limits below.*

`winrun` already runs 32-bit console Windows programs: a PE loader, the
gate between the loader's flat segments and the program's segments, SEH,
and kernel32, ntdll and msvcrt filled in. This roadmap takes it the rest
of the way to programs with windows: a `WinMain` that registers a class,
creates a window and pumps messages gets a real window on the fluxOS
desktop, drawn through GDI, with the keyboard, the mouse and timers
arriving as Windows messages.

## Starting point (before stage 0)

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

**Done.** Where it differs from the plan above:

- Tests run in QEMU, not on the host (`make test-win32-gui-qemu`): the
  build machine has neither mingw nor 32-bit glibc, so `os_linux.c`
  cannot be built there. The test PEs (`tests/win32/`) are built with
  the host's `gcc -m32`, linked as ELF at 0x400000 and turned into PEs
  by `objcopy -O pei-i386`, with a hand-made import directory
  (`imports.S`). `tests/win32-gui-guest.c` runs as init and plays the
  desktop: it answers `C_HELLO` and `C_CREATE`, checks the painted
  pixels (the brush everywhere, the text centred), resizes the window,
  presses the close button and checks the exit code. No headless
  backend was needed: without a desktop, windows draw into surfaces of
  winrun's own.
- A window is created on the desktop when it is first shown, not at
  `CreateWindowEx`, and `SW_HIDE` destroys the desktop window (keeping
  its pixels): the protocol has no hidden windows. Minimize and maximize
  commands to `ShowWindow` just show the window, since the protocol has
  no way to ask for either.
- Fonts are drawn at whole multiples of the desktop's sans (and the 8x16
  VGA font for fixed faces); text outside printable ASCII is `?`.
- Child windows exist (created, destroyed with their parent, drawn into
  their top-level window's surface) but are not clipped or hit-tested:
  that is stage 2.
- Also in: `MoveWindow`, `SetWindowPos` (top-level), `GetClassInfo(Ex)`,
  `Get/SetClassLong`, `GetDeviceCaps`, `ExtTextOut`, `CreateFont(Indirect)`,
  `GetSysColor(Brush)`, `FrameRect`, `SystemParametersInfo`
  (`SPI_GETWORKAREA`).
- Two fixes outside the new files: `vm_alloc` refused any fixed address
  above `GUEST_HIGH`, the thread block's included, so winrun could not
  start any program on fluxOS; and `shmat` puts a window's pixels
  wherever the kernel finds room, which may be inside the program's
  address window, so `vm_note_foreign` keeps the program's allocations
  off them.

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

**Done.** Where it differs from the plan above:

- The desktop needed two window flags (`desktop.h`, `wm.c`):
  `WF_BORDERLESS`, a window with no frame (one that has the focus and
  covers the screen is drawn over the taskbar and sidebar, which then
  take no clicks), and `WF_ALLBUTTONS`, which passes the right and middle
  buttons' presses and releases, and the moves while they are held, to
  the window (the desktop passed only the left button's release before).
  winrun gives every window `WF_ALLBUTTONS`, and a captionless popup
  `WF_BORDERLESS`. The test drives both through its stand-in desktop,
  not through `wm.c` itself.
- Mouse messages go to the top-level window (or the capture): hit-testing
  down to child windows is stage 2. The desktop sends moves only over the
  focused window's client area, or while a button is held. `SetCursorPos`
  cannot move the pointer (the protocol has no way to); it only changes
  what `GetCursorPos` says until the mouse next moves. `ClipCursor` does
  nothing.
- `GetKeyState` follows the messages as the program takes them, as on
  Windows; `GetAsyncKeyState` follows what the desktop has sent, read
  even when the program is not taking messages. `WM_CHAR` comes from the
  text the desktop sent with the key, or from a US layout.
- No `timeSetEvent`: its callbacks run on a thread of their own, which
  waits for stage 3. `MsgWaitForMultipleObjects` looks at its handles
  every 10 ms.
- Also in: `CreateBitmap`, `CreateDIBitmap`, `SetDIBits`,
  `SetDIBColorTable`, `GetObject`, palettes (`CreatePalette`,
  `SelectPalette`...), `SetROP2`, the viewport and window origins,
  `LoadBitmap` and `LoadImage` (bitmaps from resources or `.bmp` files),
  `ToAscii`/`ToUnicode`, `MapWindowPoints`.
- Not in: pattern and hatched brushes, regions and clip regions,
  `HALFTONE` stretching (all stretching is to the nearest pixel), RLE
  bitmaps, and a `CreateDIBSection` over a file mapping.

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

**Implemented (2026-10-31).** The basic stage-2 workflow is exercised by
`tests/win32/phase2.c`: an editor with a menu bar, edit/list/combo/button
controls, a resource dialog, a message box, popup menus, accelerators,
clipboard text and a resource icon. This is a purpose-built compatibility
program, not a claim that every version of Microsoft's Notepad runs.

- Children have sibling z-order independent of handle allocation.
  `SetWindowPos` reorders children, input descends to visible, enabled
  children, and capture retains child-local coordinates. `WM_MOVE` for
  children reports parent-client coordinates. `GetDlgItem`,
  `GetDlgCtrlID`, child traversal through `GetWindow`, `IsChild`,
  `EnumChildWindows`, `ChildWindowFromPoint(Ex)` and window enabling are
  available. Enumeration tolerates destruction and handle reuse in its
  callback; reparenting through `GWL_HWNDPARENT` rejects cycles.
- GDI clips to ancestors, `WS_CLIPCHILDREN` and `WS_CLIPSIBLINGS`, and
  queued painting follows z-order. Nonrectangular exclusions preserve
  and restore only the covered pixels touched by an operation. Private
  surfaces and clipping backups use separate mappings, recorded as
  foreign allocations, so they cannot grow the loader's heap into the
  guest EXE's mapping.
- `controls.c` registers guest-callable builtin procedures for `BUTTON`,
  `STATIC`, `EDIT`, `LISTBOX`, `COMBOBOX` and `SCROLLBAR`. Their state is
  separate from the application's window extra bytes and `GWL_USERDATA`.
  Controls use the desktop's colors and the selected GDI font. Edit
  controls support selection, keyboard navigation, copy/cut/paste,
  single/multiline text, scrolling, read-only mode, limits and one-step
  undo. Lists support insertion, deletion, selection and item data;
  combo boxes provide editable and drop-down-list modes with a desktop
  popup. Window scrollbars reserve client space and use `SCROLLINFO`.
  Notifications go to the parent through `WM_COMMAND`; applications can
  route `WM_NOTIFY` through the ordinary `SendMessage` path.
- `dialog.c` reads bounded standard and extended templates, maps dialog
  units using the dialog font and handles ordinal builtin control
  classes. Modeless and modal A/W entry points, dialog-item helpers,
  Tab/group navigation, Enter/Escape and mnemonics are implemented.
  Modal loops preserve `WM_QUIT`, disable the owner and restore it on
  exit. `MessageBox` creates a themed dialog with the requested button
  group and default button.
- `menu.c` handles standard/extended resources, menu construction and
  item state/info, menu bars inside the desktop surface, desktop popup
  windows, keyboard selection and resource/constructed accelerator
  tables. Menu height is excluded from Win32 client rectangles and
  included in window sizing and coordinate conversion.
- `clipboard.c` carries text through `C_CLIP_SET` / `C_CLIP_GET`, converts
  ANSI/UTF-16/desktop UTF-8 and uses guest `GlobalAlloc` handles for data
  returned to the program. Synchronous requests queue unrelated desktop
  events, as window creation and resizing do.
- `icons.c` decodes `RT_GROUP_ICON` / `RT_ICON` with the desktop's ICO
  decoder, supports `DrawIcon(Ex)` and sends a 32 x 32 ARGB
  `C_ICON_IMAGE`. A top-level window without an explicit icon uses the
  first icon group in its module.
- `make test-win32-gui-qemu` runs the previous stages, child-window tests
  and the stage-2 workflow on two CPUs. The stand-in desktop checks
  control/menu pixels, the taskbar icon, actual typing and accelerator
  input, combo popups, modal dialog focus/owner state, message-box results,
  popup/menu-bar commands, clipboard conversion and clean exit. Guest
  assertions also cover subclassing, radio groups, Unicode text,
  destruction during edit notification and malformed dialog resources.

**Compatibility limits:** this remains a basic USER32 implementation.
There is no edit soft wrapping, custom tab stops, IME, rich text, listbox
owner data, owner drawing or control theming API. Menus open submenus by
click or keyboard rather than hover and do not implement menu-bar drag
tracking; scrollbar buttons do not repeat while held. Clipboard support
is text only, with process-local open/close exclusion (the desktop
protocol has no cross-process clipboard lock or delayed-rendering
ownership). Fonts retain stage 0's glyph limitations. Common controls,
file dialogs and GUI threads are covered by stage 3 below.

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

**Implemented (2026-11-01).** The basic stage-3 workflow now has a
purpose-built compatibility test in QEMU. This does not imply that every
Windows utility, installer or common-control style is supported.

- `C:` now maps to `/winapps`, with `WINDOWS/SYSTEM32`, temporary,
  user-profile and application directories supplied in the system image.
  `Z:` exposes the fluxOS root, so a native launch such as
  `winrun /home/flux/tool.exe` is represented as `Z:\home\flux\tool.exe`.
  A launch outside the prefix starts with `C:\` as its Windows current
  directory. Relative paths, drive-relative `C:foo`, rooted paths and
  dot components are resolved within the selected drive; `C:\..` stays
  inside `/winapps`. This is a drive mapping, not a security sandbox.
  Folder/environment APIs use prefix paths rather than the native home.
- The kernel provides `SYS_set_win_teb`. Each thread has a Windows FS
  base switched in its CPU's GDT independently of libc's GS pthread TLS.
  The legacy fixed TEB remains the default for the main thread.
- `threads.c` runs guest entry points on pthreads with private TEBs,
  guest/native stacks and static TLS data. `CreateThread`, suspended
  startup/`ResumeThread`, returning from the entry point, `ExitThread`,
  thread IDs, exit-code queries and thread-handle waits work. TLS slots,
  last-error state, SEH stack bounds and API call frames are thread-local.
  Completed native stacks are reaped by waits, handle closure or the next
  thread creation; small thread records remain until process exit.
- A recursive runtime lock serializes APIs and protects loader, handle,
  USER32 and GDI tables while guest code can run concurrently. Blocking
  waits preserve their gate state and release the lock. Events,
  recursive mutexes, semaphores, critical sections and wait-all consumption
  are implemented; `ReadFile`/`WriteFile` release the runtime during I/O.
  USER32 queues accept cross-thread posts and synchronous sends dispatch
  on the owning thread, including reciprocal sends while waiting. Thread
  exit tears down its windows, timers and pending sends. Desktop polling
  is bounded while threads exist so a local queue post can wake a GUI loop.
- `advapi32.c` provides basic A/W key creation/open/close, typed value
  queries/updates, deletion and key information. The registry is a tree of
  encoded key directories and atomically replaced value files below
  `/winapps/WINDOWS/registry`. String values are stored as UTF-16, names
  are case-insensitive for ASCII, and independent processes can reopen
  persisted values. It persists across boots when `/winapps` is on
  persistent storage; an initrd-only root remains volatile like other files.
- `shell32.c` supplies `SHGetFolderPath(A/W)` and
  `SHGetSpecialFolderPath(A/W)` for the prefix's standard locations.
- The GUI QEMU target now also runs `threads.exe` and `registry.exe`:
  private FS/TLS and last-error state on two CPUs, suspended startup,
  recursive critical sections/mutexes, event/semaphore wakeups, pipe I/O,
  exit code 259 as a completed thread, atomic wait-all, desktop-free
  thread messages, C/Z path round trips, actual `/winapps` writes and
  registry persistence between two processes. The current system image
  requires 512 MiB for this regression VM.

- `comctl32.c` registers guest-callable builtin procedures for status bars,
  toolbars, progress bars, trackbars, tabs, report/icon list views, tree
  views, up-down controls and tooltips. Controls retain text, item data,
  selection, ranges and image lists, paint with the desktop's colors and
  send `WM_NOTIFY`, `WM_COMMAND` and scroll notifications to their parent.
  Trees use stable item handles and preserve subtree order. Tooltip relay,
  tracking and `TTF_SUBCLASS` mouse tracking use a nonactivating desktop
  popup (`WF_NOACTIVATE`). Image lists own copies of bitmap/icon pixels,
  support masks, overlays, duplication and resizing, and invalidate stale
  handles after destruction.
- `comdlg32.c` implements A/W open/save, color and font dialogs with the
  existing themed USER32 controls and modal-loop owner handling. The
  desktop has no reusable file-picker service, so these are composed
  dialogs rather than the originally proposed `dialog.h` wrapper. File
  dialogs list directories, apply the selected initial wildcard filter,
  navigate up or by typed path, add default extensions, validate required
  files/directories and confirm replacement. Cancellation preserves the
  caller's output; insufficient output space reports `FNERR_BUFFERTOOSMALL`.
  Hooks and per-thread `CommDlgExtendedError` are supported. Font choices
  reflect the runtime's available mapped faces, not arbitrary installed
  Windows fonts.
- Shell execution maps portable PEs to winrun and native files, folders,
  pictures, media, archives and URLs to fluxOS applications. The Ex APIs
  can return a waitable process handle. Folder browsing returns an opaque
  filesystem PIDL usable with `SHGetPathFromIDList`, `ILClone`/`ILFree` or
  the COM task allocator. Browse callbacks can set the initial selection.
  Desktop `S_DROP` packets become owned `HDROP` lists and `WM_DROPFILES`;
  `DragQueryFile`, `DragQueryPoint` and `DragFinish` handle their lifetime.
- `AttachThreadInput` joins input groups while keeping message queues
  separate. Focus, active-window, capture and synchronous keyboard state
  are local to a thread's input group; attaching/detaching clears keyboard
  state and thread exit removes attachments. The physical foreground
  window and asynchronous keyboard state still describe the desktop.
  Registry key/value enumeration is also implemented; names enumerate in
  their stored ASCII-folded spelling.
- `tests/win32/phase3.c` extends `make test-win32-gui-qemu` with all nine
  control classes, control messages and a real toolbar click, image-list
  pixel/lifetime checks, input-state isolation and attachment, launching
  and waiting for another PE, and a two-file desktop drop. The stand-in
  desktop drives open/save/color/font/folder dialogs with real keyboard
  input. Hook tests cover cancellation and a short output buffer; the
  registry test checks enumeration from a second process. Earlier stage
  regressions continue to run on two CPUs.

**Compatibility limits:** common controls implement ordinary stored items
and basic selection/input, not virtual/owner-data lists, callback text,
custom drawing, sorting, every style/message, or accessibility. File dialogs
currently select one file and expose the initially chosen filter; custom
resource templates, Explorer hook notifications and a filter selector are
not implemented. Color/font dialogs provide basic choices rather than the
full Windows option set. Shell associations are built in; there is no COM
shell namespace, shell extension loading, OLE drag-and-drop or registry-based
association/verb resolution. PIDLs represent filesystem paths only. Registry
notifications, security and Windows hive-file compatibility remain absent.
Running-thread suspension, forced thread termination, SRW/condition-variable
APIs and fully thread-aware CRT blocking I/O remain
unsupported; legacy asynchronous fault handling needs a further concurrency
audit. Named synchronization objects are currently process-local. These are
compatibility limits beyond the basic workflow implemented here.

## GDI+ compatibility work (2026-11-01)

An initial builtin `GDIPLUS.DLL` now complements the basic stage-3 workflow.
It is an image-focused implementation, not a complete GDI+ renderer and not
confirmation that XP Paint runs.

- Startup/shutdown, notification hooks and guest allocation; 87 named
  exports, with known ordinals for startup and the early bitmap APIs.
  Generation-tagged handles reject stale objects. Images use separate
  mapped surfaces, while guest scan buffers and lock results remain in
  guest memory. The API gateway now aligns native calls to 16 bytes even
  when the Windows caller provides only four-byte stack alignment.
- File and guest `IStream::Read` loading for BMP, PNG, JPEG, GIF and ICO.
  Existing native codecs handle PNG/JPEG/GIF/ICO; a bounded BMP decoder
  handles uncompressed 1/4/8/24/32-bit files. Stream data is decoded eagerly.
  Codec enumeration advertises BMP/JPEG/PNG/GIF decoders and only the three
  implemented encoders: BMP, JPEG and PNG. JPEG quality parameters and
  encoder parameter enumeration are supported. PNG output uses lossless
  stored DEFLATE blocks, favoring simplicity over compressed file size.
- Bitmap creation from scan buffers, graphics, DIBs, HBITMAPs and HICONs;
  conversion back to GDI objects; dimensions, pixel formats, raw formats,
  in-memory DPI, pixels, cloning, rotation/flipping and thumbnails. Supported
  scan/lock formats are 24-bit RGB, 32-bit RGB, ARGB and premultiplied ARGB.
  External scan buffers remain shared; negative strides and caller-provided
  lock buffers are checked. Locked images reject incompatible operations.
- Drawing to bitmap graphics and HDCs shares GDI's clipping, DIB updates and
  damage tracking. Image rectangles, nearest-neighbor scaling, alpha
  source-over/source-copy, solid fills, clearing, positive axis scaling and
  translation are implemented. HDC borrowing prevents drawing until the
  DC is released. This subset defaults to nearest-neighbor interpolation;
  requests for other interpolation modes return `NotImplemented`.
- `tests/win32/gdiplus.c`, run by `make test-win32-gui-qemu`, checks startup
  and notification callbacks, ordinal lookup, RGB/ARGB conversions, negative
  strides and shared buffers, locks, cloning/rotation, alpha drawing to
  bitmap and GDI targets, guest codec pointers, BMP/PNG/JPEG round trips,
  JPEG quality validation, stream callbacks and stale handles. Earlier GUI
  and thread regressions run alongside it on two CPUs.

Still absent: TIFF, GIF/TIFF encoding, compressed/bitfield BMPs, indexed
bitmap creation, multiframe animation, retained EXIF/property metadata,
color management, stream saving, the vector/path/pen/text rendering APIs,
image attributes, arbitrary affine transforms and advanced clipping or
interpolation. DPI metadata is not preserved consistently by every codec.
ICM entry points return `NotImplemented`. The full Windows image or codec
behavior must not be inferred from the presence of this subset.

The implementation is original fluxOS code using the existing graphics
backend. ABI and behavior references consulted:
[Microsoft's bitmap flat API](https://learn.microsoft.com/en-us/windows/win32/gdiplus/-gdiplus-bitmap-flat),
[Wine's export definitions](https://github.com/wine-mirror/wine/blob/master/dlls/gdiplus/gdiplus.spec)
and [Wine's image tests](https://github.com/wine-mirror/wine/blob/master/dlls/gdiplus/tests/image.c).
No Wine or ReactOS implementation code was incorporated.

## Compatibility pass before stage 4 (2026-11-01)

- Required PE imports now fail during loading with the importing module,
  missing DLL or symbol name/ordinal, and nested dependency error. Import
  descriptors, strings and thunk bounds are checked. A DLL whose imports
  fail is removed from the module list and unmapped. These errors no longer
  become zero-returning missing-function thunks. Optional dynamic lookups
  still return failure to the caller.
- GDI has `SaveDC`/`RestoreDC`, including nested absolute/relative restore,
  selected-object references, drawing attributes, origins and copied clips.
  Rectangular region objects support AND/OR/XOR/DIFF/COPY, queries and
  offsets. Application clip regions are copied into the DC, independently
  of the paint/visibility clip. Intersections and exclusions affect shared
  drawing paths, including text, blits, shapes and GDI+ rendering. Complex
  clipping currently preserves excluded pixels around drawing operations;
  it is correct for the tested paths but costs extra memory and copying.
- Pattern brushes snapshot bitmap pixels and support monochrome color
  mapping and device-relative brush origins. All six hatch styles support
  transparent or opaque backgrounds. Pattern sampling is used by fills,
  filled shapes and pattern-dependent raster operations. `CreateBrushIndirect`
  dispatches the supported solid/null/hatch/pattern styles.
- `_beginthread`/`_endthread` and `_beginthreadex`/`_endthreadex` use the
  existing Windows TEB/TLS thread implementation. The former closes its
  handle automatically; the latter preserves its waitable handle and exit
  code. Suspended `_beginthreadex` startup is supported. This does not make
  all CRT state or blocking I/O thread-local/thread-aware.
- `timeSetEvent`/`timeKillEvent` support one-shot/periodic function callbacks
  and event-set notifications. Callback workers have their own Windows
  thread context. Cancellation wakes waiting workers; synchronous cancellation
  waits for an in-flight callback, and self-cancellation does not deadlock.
  Timing resolution remains the host clock/scheduler's resolution; the
  requested resolution does not guarantee precision. Event-pulse timer
  callbacks are explicitly rejected because PulseEvent is not implemented.
- `tests/win32/prephase4.c` covers region pixel output and copied ownership,
  nested DC restore, brush copies/origins/backgrounds, CRT return and explicit
  exit, handle ownership, suspended startup, timer callback thread identity,
  event notification, cancellation before firing, self-cancellation and
  synchronous cancellation during a callback. Valid PEs with missing DLL,
  name and ordinal imports check diagnostics and exit status 126 before
  execution. `make test-win32-regions` checks disjoint region algebra against
  pixel membership across generated rectangle sets. These run alongside the
  existing two-CPU GUI/thread/GDI+ regressions.

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

**Basic workflow implemented (2026-11-01).** This is an original fluxOS
implementation over its existing GL, GDI and sound backends. It is not a
claim of general DirectX compatibility or that every older game runs.

- `opengl32.c` implements one RGBA pixel format, WGL context creation,
  binding, deletion, current context/DC queries, proc lookup and buffer
  presentation. `ChoosePixelFormat`, `DescribePixelFormat`, `SetPixelFormat`,
  `GetPixelFormat` and `SwapBuffers` are GDI32 exports. Context ownership is
  per Windows thread, native GL state is rebound on every API call, and
  backing buffers resize with the DC. The bridge deliberately uses the
  software renderer; native GPU mappings have not been integrated with the
  guest VM allocator. `tools/gen-win32-gl.c` generates 269 stdcall wrappers from
  the native header; `glGetString` and `glGetPointerv` have explicit guest
  pointer handling. Floats/doubles are decoded from Windows stack arguments.
  A separate `libGL-win32.a` uses 16-byte-aligned managed allocations so
  graphics data cannot grow the loader's native heap into the guest EXE.
- `audio.c` mixes sources at 44.1 kHz, stereo signed 16-bit, saturating
  additions before writing to `/dev/dsp`. Blocking device writes release
  the runtime lock. Playback-delay tracking defers wave buffer completion
  until the native device has consumed it. Workers have Windows TEB/TLS.
  `waveout.c` implements device queries, PCM open/query, prepare/unprepare,
  queued writes, completion callbacks (function/window/thread/event),
  pause/restart/reset, position, volume, close, single-header loops and
  break-loop. Supported input is 8/16-bit mono/stereo PCM at 4–192 kHz,
  resampled into the mixer. Reset preserves submission order when returning
  headers. `PlaySound(A/W)` and `sndPlaySound(A/W)` support uncompressed WAV
  files, memory and RT_WAVE resources, with sync/async, looping and stop.
- `midi.c` maps MIDI channel messages onto the native nine-voice OPL2
  interface (also available on OPL3 hardware). Note on/off, sustain,
  volume, program selection, pitch bend, all-notes-off/reset and basic
  capability/volume/ID APIs are present. MIDI headers and completion
  callbacks support General MIDI reset and universal master-volume SysEx.
  Eight local FM timbres approximate program families; no external bank
  or Microsoft runtime is shipped.
- `ddraw.c` supplies DirectDraw 1/2/4/7, surface 1/2/3/4/7, palette and
  basic HWND clipper interfaces with shared COM identity/reference counts.
  Supported surfaces are 8-bit indexed, RGB565, 24-bit RGB and 32-bit RGB,
  backed by guest memory. Lock/unlock, surface descriptors/caps, palette
  updates, color fill, nearest-neighbor/scaled copies, overlapping self-blits,
  source/destination color keys, a primary/back-buffer flip pair, and
  GDI DC acquisition/release are implemented. Windowed primaries use screen
  coordinates; logical display modes scale to the target window. Native
  physical display modes and exclusive ownership do not change.
- `dsound.c` provides DirectSound/DirectSound8 and basic buffer/buffer8
  interfaces. Secondary buffers expose guest memory, wrapped locks,
  looping/one-shot playback, stop, seeking, volume/pan/frequency controls,
  status/cursors and shared-data duplication. They feed the same mixer as
  waveOut. Primary buffers expose format negotiation. `ole32.c` supplies
  basic thread initialization and in-process activation of the implemented
  DirectDraw/DirectSound classes. COM vtable slot/argument metadata comes
  from Microsoft SDK declarations through `gen-win32-com.py`; unsupported
  methods return failure with the correct calling convention.
- `tests/win32/phase4.c` checks float/double GL arguments, pixel output,
  guest pointers, context ownership, allocations larger than the loader's
  address gap, COM activation, surface locks/busy state/flip/palette/GDI,
  windowed screen coordinates, DirectSound wrap/shared-data/control state,
  wave completion/reset order/high-rate loops, WAV memory/file/resource
  playback and MIDI messages/SysEx. `phase4-gui.c` renders through the
  desktop protocol and checks repaint after resize. The new
  `make test-win32-phase4-qemu` runs the complete GUI suite on two CPUs with
  SB16 and AdLib emulation. Its recorded output is checked for simultaneous
  440 Hz waveOut and 660 Hz DirectSound tones; the regression measured
  1.20 seconds of overlap. The ordinary GUI target still works without an
  audio device and skips hardware-dependent sound checks.

**Compatibility limits:** OpenGL functionality is limited to the native GL
subset; WGL list sharing/context copying, font helpers, advanced pixel
formats, modern GL extensions and hardware acceleration are absent. DirectDraw
overlays, arbitrary clip lists, multiple back buffers, caller-owned surface
storage, most private-data/texture methods and physical fullscreen modes are
unsupported. DirectSound capture, 3D/effects, hardware buffers and position
notifications are absent; cursors reflect the software mixing position and
native queued audio introduces latency. Stop/reset cannot retract individual
sources from audio already mixed into the native device queue. WaveOut
multi-header loop groups, compressed audio and registry sound aliases are
unsupported. MIDI is a small FM approximation, not a full General MIDI
instrument/percussion implementation or a MIDI stream/file player. Unsupported
COM methods and formats fail instead of claiming success.

ABI references:
[Microsoft DirectDraw declarations](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/ddraw.h),
[DirectSound declarations](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/dsound.h),
[waveOutOpen](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutopen),
[WAVEHDR](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/ns-mmeapi-wavehdr).
No Wine or ReactOS implementation code was incorporated.

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
