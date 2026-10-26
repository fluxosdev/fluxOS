# fluxOS

> **Warning: fluxOS is experimental. Do not use it as your daily driver.**
> It is a hobby operating system, written to see how far one could go, not to
> be relied on. It has no security model (one user, cosmetic permissions, no
> firewall, an unaudited network stack), a young filesystem driver that can
> lose or corrupt data, and plenty of bugs nobody has found yet. Run it in a
> virtual machine, or on a computer with nothing on it you would miss, and
> never connect it to a network you do not trust or keep anything important
> on its disks.
>
> **It was built with AI assistance from Claude and ChatGPT Codex.** Every line of fluxOS, from the
> bootloader glue and kernel to the TCP/IP stack, the desktop and the web
> browser, was written with [Claude](https://claude.ai), Anthropic's AI model,
> working through [Claude Code](https://claude.com/claude-code), and ChatGPT
> Codex from instructions given in conversation. No code was written by hand. It has
> been tested by running it in QEMU, not reviewed or audited by people, so
> read it with that in mind.

fluxOS is a small 32-bit x86 Unix-like operating system with preemptive
multitasking, ring-3 processes, paging, a root filesystem in RAM (from the CD)
or on an installed hard disk, IDE/SATA/floppy/USB storage, FAT and ISO9660, an
NE2000 and nForce network driver with a TCP/IP stack, a zsh-flavoured shell
(**fsh**), a Windows 95 style desktop with a web browser, 3dfx Voodoo3 and
Radeon HD 4000 display drivers with a small OpenGL library, PC speaker, Sound
Blaster 16 and HD Audio sound, and a layer that runs DOS programs in the
terminal. It boots with GRUB (Multiboot) from an ISO image, straight into the
desktop.

## Features

**Kernel**
- Multiboot entry, GDT/TSS/IDT, one trap dispatcher for exceptions, IRQs and
  the `int 0x80` system call gate.
- Preemptive round-robin scheduler (PIT at 100 Hz) with sleep/wait queues.
- Processes: `fork`, `execve` (static ELF32 and `#!` scripts), `exit`/
  `waitpid`, process groups, signals (`kill`, `SIGINT`, `SIGPIPE`, fault
  signals; no user handlers), lazy x87 FPU switching (SSE is not supported).
- Memory: bitmap frame allocator, paging (first GiB identity-mapped, 4 KiB
  user pages), growing user stack, `sbrk` heaps, first-fit kernel heap.
- VFS over an in-memory filesystem unpacked from a ustar initrd at boot, with
  `/dev/{tty,console,null,zero,random,urandom}`, pipes and pseudo-terminals.
  Started from an installed disk (`root=UUID=...`), its FAT32 partition is the
  root instead and only `/dev`, `/tmp` and `/mnt` stay in memory.
- Storage:
  - IDE (ATA LBA28/48 and ATAPI) with bus-master DMA, interrupt driven, falling
    back to PIO on failure; native-mode PCI IDE controllers. Options:
    `ata.nodma`, `ata.poll`.
  - **SATA (AHCI)** disks and ATAPI drives (`ahci.poll`).
  - Floppy controller (`/dev/fd0`, `fd1`) with a track cache and automatic
    format detection.
  - `/dev/hdX`, `/dev/sdX`, `/dev/srN` with MBR partitions behind a write-back
    sector cache, flushed by `kflushd` every 5 s, by `sync`/`fsync`, on
    `umount` and before `reboot`/`poweroff`.
- USB: **UHCI, OHCI, EHCI and xHCI** controllers, hubs (up to five deep),
  boot-protocol keyboards, HID mice and tablets, and Bulk-Only mass storage.
  Hot plugging via the `kusbd` thread; `lsusb` lists devices. Options:
  `usb=off`, `usb.poll`.
- Filesystems: **FAT12/16/32** read/write (long names, all FATs kept in sync)
  and read-only **ISO9660** with Rock Ridge. `mount`/`umount`, and an
  **automounter** (`kmediad`) that mounts and unmounts CDs, floppies and USB
  disks as they come and go (`automount=off`, `floppy.nopoll`).
- Console: ANSI/VT100 text console on the framebuffer (8x16 CP437 font) or VGA
  text mode, PS/2 keyboard, mirrored COM1, canonical and raw line discipline.
- Input: PS/2 mouse with wheel and the VMware/QEMU absolute-pointer interface,
  USB keyboards, mice and tablets, delivered through `/dev/input`.
- Display drivers:
  - Standard: keeps the VESA BIOS mode.
  - **3dfx Voodoo Banshee/3/4/5**: reads EDID, sets modes from 640x480 to
    1600x1200 (`video=1024x768x16@85`, `voodoo=off`), and lends its 3D engine
    to one process.
  - **ATI Radeon HD 4000 (R700)**: modes set by running the card's AtomBIOS
    tables, EDID over DDC, page flipping at vblank, hardware pointer, and a
    3D engine driven by AMD microcode (bundled in `firmware/radeon`, see
    `LICENSE.radeon`) over PM4 command buffers. A GART lets the GPU copy the
    desktop's back buffer to the screen. Options: `video=1920x1080x32@60`,
    `radeon.modeset=0`, `radeon.accel=0`, `radeon.gart=0`, `radeon=off`.
- Sound:
  - PC speaker (`/dev/speaker`) with background tone queue.
  - **Sound Blaster 16**: `/dev/dsp` (OSS-style, 8/16-bit, mono/stereo, 5-44.1
    kHz), `/dev/mixer`, and the OPL2/3 synthesizer (`/dev/opl`). `sb16=off`,
    `opl=off`.
  - **Intel HD Audio** (and codecs such as the Realtek ALC888) as `/dev/dsp`
    when there is no Sound Blaster; output only, every jack plays the same
    stereo. `hda=off`.
- Networking: NE2000 (PCI and ISA) and **nVidia nForce** ("forcedeth", `eth0`,
  with MII PHY support) drivers; Ethernet, ARP, IPv4 (no fragments), ICMP, UDP,
  loopback; TCP with the full state machine, RTT estimation, retransmission
  and fast retransmit; a DHCP client (or `net.ip=... net.gw=... net.dns=...`
  on the kernel command line); BSD sockets for TCP, UDP and ping; DNS from
  `/etc/hosts` and then the DHCP server.
- Kernel log (`dmesg`), CMOS clock, reboot and ACPI power-off.

**fsh, the flux shell (zsh-flavoured)**
- zsh prompt escapes (`%n %m %~ %# %?  %F{color} %B`...), live syntax
  highlighting, grey history autosuggestions, Tab completion menus, prefix
  history search, emacs keys, and continuation prompts.
- Pipelines, `; && || & !`, redirections, `if/while/until/for`, `{ }`, `( )`,
  functions, `time`; `$VAR ${VAR:-x} ${#VAR}`, `$(cmd)`, `$((math))`, `~`,
  brace expansion, globs (a glob that matches nothing is an error, as in zsh).
- Job control (`jobs`, `fg`, `wait`, `kill %n`), history expansion
  (`!! !$ !n !str`), aliases, `source`, `~/.fshrc`, `~/.fsh_history`, AUTO_CD.

**The desktop** (`/bin/desktop`), started by init at boot
- Windows 95 look: purple desktop with icons, a taskbar (Start button, a
  button per window, clock, volume tray icon), beveled windows. UI text is
  anti-aliased Liberation Sans; terminals keep the sharp 8x16 bitmap font.
- Tango icons (PNG), PNG, JPEG, GIF and SVG decoding (the first bytes decide,
  whatever the name), and a wallpaper (Fill, Stretch, Center or Tile).
- Windows move, minimize, maximize and cascade; the browser, text editor, file
  manager, Task Manager and Gears resize by dragging an edge.
- Start menu with cascading submenus (Programs, Accessories, System Tools,
  Settings), Read Me, Run..., Install fluxOS... and Shut Down.... Keys: Windows
  key or Ctrl+Esc for Start, Alt+Tab, Alt+F4.
- **Terminal**: 80x25 VGA-style, running fsh on a pty, as many as you like.
- **File manager**: Explorer-style icon view with an address field, Up, New
  Folder, Delete, Rename, cut/copy/paste (also between disks), a right-click
  menu, and drive icons for mounted volumes. Double-click opens folders; runs
  programs; opens pictures, text files, `.wav` files, HTML; other files go to
  `more`.
- **Calculator**: exact decimal arithmetic (30 digits), memory keys, keyboard
  use.
- **Text editor**: Notepad-style, File/Edit menus, selection, a clipboard
  shared with the browser, files up to 1 MB. No undo, word wrap or search.
- **Sound Player**: opens a PCM WAV (typed path or from the file manager) with
  Play, Pause, Stop and a seek bar. It runs `play(1)` (`play -s SECONDS`
  starts part-way in) so the desktop keeps running.
- **Display Properties**: wallpaper and fill mode with a preview (with **(None)** chosen, a
  `#rrggbb` field sets the solid desktop color), resolution,
  color depth and refresh rate (with a 15 s "keep this mode?" prompt on the
  3dfx and Radeon drivers), monitor type and EDID details. Settings are saved
  in GRUB's environment block on an installed system.
- **3D Gears**: the OpenGL demo, drawn by the Radeon or Voodoo 3D engine when
  there is one, else by the software renderer (S switches renderer).
- **Archive Manager**: opens ZIP and 7z archives (double-click one in the File
  Manager, or **Extract All...** on its right-click menu) and shows them like
  folders, with sizes, packed sizes, dates and methods; double-clicking a file
  inside opens it. Extracting (everything, or the selection) runs `unzip` in
  the background with a progress bar and Cancel, and asks before replacing
  files; **Test** checks every CRC.
- **Task Manager** (applications, processes, CPU/memory performance graphs),
  **Disk Manager** (volumes, partition bars, mount/unmount/format, MBR partition
  editing), **Volume Control**, **Setup** (installs fluxOS on a disk; only
  shown when booted from the install medium), and **System Properties**.

**Web browser** (the **Web Browser** icon): an Internet Explorer 3 style
window with Back/Forward/Stop/Refresh/Home, address bar, progress gauge and a
right-click menu (page actions, link actions, and Cut/Copy/Paste on the
address bar). Pages come over HTTP or HTTPS (through `wget`, so the desktop
keeps running), or from `file://`.
- **HTML**: follows the main rules of the HTML5 tree builder (implied and
  auto-closed tags, tables, character references, quirks mode); UTF-8,
  Latin-1 and Windows-1252. The complete HTML5 named character-reference table
  includes long names, legacy forms and references producing two code points.
- **CSS**: type, class, id, attribute and structural selectors with all
  combinators, `:not()`/`:is()` with full selectors, specificity, `!important`,
  `:has()`, `:nth-child()`/`:nth-last-child()` with `of <selector-list>` filters, `@media`, custom properties and `var()`, `calc()`,
  `max-height`, `aspect-ratio`, `::before`/`::after` text,
  gradients, rounded corners, and the old presentational attributes. Nesting
  (`&` and nested `@media`), `@supports` (and `CSS.supports()`), `@import`,
  media queries level 4 (ranges such as `400px <= width <= 700px`, `or`,
  heights, `prefers-reduced-motion`...), CSS Color 4/5 (`oklch()`, `oklab()`,
  `lab()`, `lch()`, `hwb()`, `color()`, `color-mix()`; worked out in
  csscolor.c) and logical properties (`margin-inline`, `padding-block`,
  `inset-inline`, `inline-size`...). `<style>` and `<link>` elements that
  scripts add or change apply as soon as the script returns.
- **Layout**: blocks with margin collapsing, inline text, inline-blocks, lists,
  tables, flex rows, grids (columns, rows, areas, auto rows, spans, `dense`),
  borders, backgrounds, images and form controls. Floats (`float`, `clear`,
  clearfixes, text wrapping, new formatting contexts beside floats) and
  positioning (`relative`, `absolute` and `fixed` with `top`/`right`/`bottom`/
  `left`, `inset`) work, with `z-index` for absolute and fixed boxes, `box-shadow`
  (the first outer shadow, drawn as fading layers) and `transform: translate()`.
- **Interaction**: `:hover`, `:focus` and `:focus-within` restyle the page as the
  pointer and focus move. Drag to select text, Ctrl+A to select all, Ctrl+C to
  copy to the shared clipboard. The window and taskbar show the page's favicon
  (`<link rel=icon>` or `/favicon.ico`; PNG, ICO, GIF or SVG).
- **Cookies**: a jar shared by every window (kept in memory only) fills from
  `Set-Cookie` headers and `document.cookie` (domain, path, `Secure`,
  `HttpOnly`, `Max-Age`, `Expires`) and goes out on every request, so
  logins and `fetch` calls keep their session. `SameSite` is not enforced.
- **Pictures**: PNG, JPEG, GIF (first frame), ICO (the largest icon) and SVG, as `<img>` and as an
  inline `<svg>`. `srcset`/`sizes` and `<picture><source>` choose a picture of
  the right width in a format the browser can decode (not WebP or AVIF). The SVG renderer (svg.c) draws shapes, paths (curves and
  arcs), groups, `use`, text, transforms, `viewBox`, solid and gradient fills,
  dashed strokes with caps and joins, opacity, and simple `<style>` rules.
- `<details>`/`<summary>` open and close when clicked. Links, `GET`/`POST` forms, `<meta refresh>`, fragments, redirects, iframes,
  `:visited` colors, and anti-aliased text (Liberation Sans, Serif and Mono in
  12 sizes, rasterised by `tools/mkfont.py`).
- **JavaScript**: a tree-walking interpreter (js.c, jslib.c, jslib2.c, jsre.c)
  that runs inline and external scripts (`defer` ones and modules after the
  rest), then `DOMContentLoaded` and `load`.
  It covers most of ES2020 and some later: `let`/`const`, arrow functions,
  template literals, destructuring, spread, optional chaining, classes
  (fields, `#private`, `extends`, `super`, statics and `static {}` blocks),
  ES modules (`<script type=module>`, `import`/`export` in all their forms with
  live bindings, `import()`, `import.meta`, top-level `await`, import maps;
  `nomodule` scripts are skipped), generators, async generators and
  `async`/`await` (run as coroutines on their own stacks),
  Promises with a microtask queue, regular expressions with lookbehind and
  named groups, and the usual built-ins (Map, Set, Weak*, typed arrays, JSON,
  Date, Reflect, `Proxy` (get, set, has, deleteProperty and ownKeys traps), the ES2025 Set methods,
  `Array.fromAsync`, `Error` causes, a small Intl (`NumberFormat`, `DateTimeFormat`, `Collator`,
  `PluralRules`, `RelativeTimeFormat`, `ListFormat`, `Segmenter`), `structuredClone`...). The DOM (jsdom.c) has
  `document`, `querySelector` (including `:is()`, `:where()`, complex `:not()`
  and filtered sibling selectors), `innerHTML`, `classList`, `dataset`, events
  (capture and bubbling phases, `once`/`signal` options, `handleEvent` objects,
  `MouseEvent`, `PointerEvent`, `KeyboardEvent` and the other event classes;
  the pointer's moves, presses and hovering reach scripts as `pointer*`,
  `mouse*`, `mouseenter`/`mouseleave`), `scroll` and `resize` events,
  `MutationObserver`, `IntersectionObserver` and `ResizeObserver`, custom
  elements (`customElements.define` with upgrades and lifecycle callbacks;
  `attachShadow` gives the element itself as its shadow root), `<template>`
  content, `<dialog>`, `HTMLInputElement` and the other interfaces for
  `instanceof`, `TreeWalker`, `Range`/`Selection`, form validation,
  `form.submit()`/`requestSubmit()`, `load`/`error` events for pictures,
  scripts and style sheets,
  `replaceChildren`, `insertAdjacentText` and `insertAdjacentElement`,
  timers, `location`, `history` (`pushState`/`replaceState` with `popstate`,
  and `hashchange`, without reloading), `matchMedia`, `localStorage` and
  `sessionStorage` (per origin, kept while the desktop runs), `XMLHttpRequest`, `fetch`,
  `URL`, and more: `FormData`, `Blob`, `TextEncoder`/`TextDecoder`, `crypto.getRandomValues`/`randomUUID`,
  `DOMParser`, `postMessage`, a style element's `sheet.insertRule()`, and stand-ins for `WebSocket`,
  `Notification`, `element.animate()` (it skips to the end) and web fonts.
  `fetch` and `XMLHttpRequest` send request headers and any method (`PUT`, `DELETE`...). Changes are laid out again when a script returns; runaway
  scripts are stopped. `/usr/share/www/js.html`, `css.html` and `images.html`
  (linked from the home page) are live test pages. `features.html` demonstrates
  the HTML, CSS and DOM additions in 0.1.4, and `modern.html` the modules,
  observers, custom elements, nesting and colors that came after.

**DOS programs** (`/bin/dos`)
- Runs `.COM` and MZ `.EXE` files in the terminal (`./GAME.EXE`, `dos game`, or
  double-click). `dos` emulates a PC in user space: an x86 CPU (8086 to 486, no
  FPU), 640 KiB of memory, a BIOS and DOS 5 services, and a DPMI 0.9 host for
  extenders such as DOS/4GW (DOOM and similar).
- Drives: `C:` is the fluxOS root, `A:`/`B:` are `/mnt/fd0`/`fd1`, `D:` is
  `/mnt/cdrom`. Standard handles are fluxOS descriptors, so pipes and
  redirection work. EXEC of a fluxOS program runs it as a real process, and
  `COMMAND.COM` runs the shell.
- Screen: text mode (80x25 with colors and box drawing, ANSI.SYS), and VGA
  mode 13h and "mode X" graphics doubled to 640x400 in the desktop's terminal.
  **Ctrl+\\** stops the emulator; `dos -l FILE` logs what is unsupported.
- Sound: the PC speaker, AdLib/OPL, and a Sound Blaster 16 at 220h, IRQ 5,
  DMA 1/5 (DSP, mixer, both DMA controllers) passed on to the real devices.

**OpenGL** (`libGL.a`, `<GL/gl.h>`, `<GL/fluxgl.h>`, `<GL/glu.h>`)
- A subset of OpenGL 1.1's fixed-function pipeline: `glBegin`/`glEnd`, matrix
  stacks, per-vertex lighting (eight lights), flat/smooth shading, culling,
  depth buffer, scissor, display lists, `glGet*`, and basic GLU.
- Three backends, tried in order: the Radeon 3D engine (PM4 command buffers),
  the 3dfx engine (driven from user space, as Glide does), and a software
  scanline rasterizer. `FGL_NO_RADEON`, `FGL_NO_TDFX`, `FGL_SOFTWARE`,
  `FLUXGL_DRIVER=radeon|tdfx|soft` and `LIBGL_ALWAYS_SOFTWARE` choose.
- `fglCreateContext`, `fglMakeCurrent`, `fglResize` and `fglSwapBuffers` play
  GLX's part. A `<math.h>` on the x87 is included; programs using GL or math
  are built with `USER_FP_CFLAGS`, the rest of user space is integer-only.

**Userland** (`/bin`): 7z basename beep cat chmod clear cp date desktop df dirname
dmesg dos echo env fetch fluxinstall free fsh grep halt head hexdump host
ifconfig init ls lsblk mkdir mkfs.fat mixer more mount mv nc ping play
poweroff ps reboot rm rmdir seq sh sleep sort spin stat sync tail tee top
touch tr umount uname uniq unzip uptime wc wget yes zip, on a small libc (stdio, malloc,
dirent, time, sockets, a DNS resolver, archives).
- `unzip [-l|-v|-t|-p] [-o|-n] [-j] [-d DIR] ARCHIVE [NAME...]` lists, tests and
  extracts ZIP and 7z archives; `7z l|t|x|e [-oDIR] [-y] ARCHIVE` does the same
  with 7-Zip's syntax; `zip [-r] [-0..-9] [-j] ARCHIVE.zip FILE...` makes ZIP
  archives (Deflate). libc's `<archive.h>` reads ZIP (stored, Deflate, LZMA;
  Zip64; UTF-8 and CP437 names) and 7z (Copy, LZMA, LZMA2, Deflate, the BCJ x86
  and Delta filters, solid blocks, compressed headers), with every CRC checked.
  Encrypted archives, other methods (BZip2, PPMd, BCJ2...) and creating 7z
  archives are not supported.
- `ifconfig` shows the interface and sets a static address or asks DHCP;
  `ping [-c N] HOST`, `host NAME`, `nc` (netcat, TCP and UDP).
- `wget [-q] [-O FILE] [-T SECONDS] [--save-headers] [--content-on-error]
  [--max-redirect=N] [--post-data=STRING] [--method=VERB] [--header=LINE]... URL` follows redirects and decodes
  chunked bodies. `https://` uses libc's own TLS 1.3 client (`<tls.h>`: X25519,
  AES-128-GCM). It encrypts but does not verify the server's certificate.
- `beep [-f HZ] [-l MS] [-r N] [-d MS] [-n ...]` and `beep -m MUSIC` (BASIC's
  PLAY language); `play [-q] [-s SECONDS] FILE...` plays WAV (8 to 32-bit PCM)
  and VOC files, `play -t HZ [-l MS]` a test tone; `mixer [CHANNEL LEVEL]`.

## Building

On a Debian/Ubuntu host:

```sh
sudo apt install build-essential grub-pc-bin grub-common xorriso mtools qemu-system-x86
```

No cross compiler is needed: everything is built with the host GCC using `-m32
-ffreestanding`, and nothing links against libgcc or the host libc. The first
`make` downloads Debian 12's GRUB 2.06 into `grub-2.06/` (checked against
fixed SHA-256 hashes; nothing is installed), because GRUB 2.12 reboots some
older PCs right after "Welcome to GRUB!". `make GRUB=/ GRUB_DEP=` uses the
host's GRUB. The ISO boots on legacy BIOS only.

```sh
make            # builds fluxOS.iso
make check      # verifies the kernel is a valid Multiboot image
make test-browser # runs DOM/classList regressions on a Linux x86 host
make test-kernel # runs allocator, filesystem, pipe and socket regressions on the host
make test-kernel-qemu # boots kernel regressions with a disposable initrd and scratch disk
make disk       # fluxdisk.img, a 64 MiB FAT32 data disk (DISK_MB=...)
make floppy     # fluxfloppy.img, a 1.44 MB FAT12 floppy for drive A:
make run        # boots the ISO in QEMU with the disk, floppy (if any) and an NE2000
make run-kernel # boots the kernel directly (text console)
make run-disk   # boots fluxdisk.img alone, after installing fluxOS onto it
make run-q35    # q35 machine: disk and CD on SATA, USB keyboard and tablet on xHCI
make run-usb    # disk as a USB stick on EHCI, keyboard and mouse on UHCI
make clean      # removes build/ and the ISO; the disk images are kept
```

`make run QEMU_NET="-nic user,model=ne2k_pci -device intel-hda -device
hda-duplex"` adds HD Audio. To boot real hardware or another VM, write
`fluxOS.iso` to a USB stick or attach it as a CD-ROM and boot in **legacy
BIOS** mode (UEFI is untested). The default entry starts the desktop at
1024x768; **fluxOS (text console)** boots to the shell. In QEMU the mouse
needs no grabbing; elsewhere click into the VM window.

For the 3dfx driver and accelerated GL, give the machine a Voodoo Banshee or
Voodoo3 (86Box emulates both; QEMU has none, so GL uses the software
renderer). fluxOS still starts in the BIOS mode: switch drivers in Display
Properties, or add `video=1024x768x16@85` to the kernel command line.

## Installing to a hard disk

Boot the CD and double-click **Install fluxOS**, or from a shell run
`fluxinstall /dev/hda` on a whole disk (see `lsblk`; it asks you to type
"yes", `-y` skips that). It writes an MBR with one bootable FAT32 partition at
1 MiB, installs GRUB, formats the partition, and copies the kernel, initrd,
`grub.cfg` and files onto it. **This erases the whole disk**, which must be at
least 34 MiB. The installed disk boots by itself and can install onto another
(`fluxinstall -s / /dev/hdb`). In QEMU, install with `make run`, then quit and
`make run-disk`. The installed system runs from disk, so anything saved outside
`/tmp` stays; the boot menu's **safe mode** runs from the initrd in memory with
the disk on `/mnt/hda1`.

## Things to try

On the desktop: open **My Computer**, drag windows, open terminals, try
**Run...** with `ls -l /bin`, open the **Web Browser** (68k.news, wiby.me and
textfiles.com suit simple browsers; or type `example.com`), and **Shut Down...**.
In a terminal:

```sh
fetch                          # system summary
spin A 5 & spin B 5 & top      # CPU-bound jobs sharing the CPU; ^C quits top
cat ~/README.txt; ./demo.sh    # tour and a scripting demo
echo {1..3} $((6*7)) ~         # brace, arithmetic and tilde expansion
lsblk; df -h; mount            # disks, CD-ROM and mounted filesystems
cp -r ~ /mnt/hda/home          # FAT32 writes survive reboots
ifconfig; ping -c 3 10.0.2.2   # the network card, and QEMU's gateway
host example.com; wget -O - https://example.com/    # DNS, HTTP, TLS 1.3
nc -l 7777 &  echo hi | nc localhost 7777           # a TCP server and client
lsusb -v                       # USB devices (make run-usb or run-q35)
cp /mnt/fd0/GAME.EXE ~; ./GAME.EXE    # a DOS program (make floppy first)
beep -m "T160 L8 CDEGEC4"      # the PC speaker
play /usr/share/sounds/startup.wav; mixer pcm 60    # the Sound Blaster
dmesg | tail; poweroff
```

Files on `/mnt/hda` live in `fluxdisk.img`, and those on `/mnt/fd0` in
`fluxfloppy.img`. On a Linux host inspect them with `mdir -i fluxdisk.img ::`
or `fsck.fat -n fluxdisk.img`, and add DOS programs with `mcopy -i
fluxfloppy.img GAME.EXE ::`.

## Layout

```
Makefile, linker.ld        build and kernel link script
grub/grub.cfg              boot menu
include/flux/              kernel <-> user ABI (syscall numbers, structs, errno)
lib/                       string/printf routines and the 8x16 font, shared by kernel and libc
tools/                     mkfont.py (fonts), tango-icons.py (icons), mksounds.py (sounds)
kernel/                    boot.S, entry.S, gdt/idt/pmm/vmm/heap, proc/exec/syscall,
                           vfs/file/initrd, ata/ahci/floppy/blkdev/pci, usb*/uhci/ohci/
                           ehci/xhci, automount, fat32/iso9660, tty/vga/serial/keyboard/
                           timer, fb/mouse/input, voodoo, radeon*, pcspkr/sb16/opl/hda,
                           fpu, ne2000/forcedeth/mii/net/tcp/socket, kmain
user/libc, user/include    the C library (math.c is the x87 math library) and headers
user/gl/                   the OpenGL library and its backends
user/fsh/                  the shell
user/desktop/              compositor and window manager (wm.c), apps (files, edit,
                           calc, player, taskmgr, diskmgr, display, setup, gears,
                           mines, sound), PNG/JPEG/GIF/ICO/SVG decoding (png.c, jpeg.c,
                           gif.c, ico.c, svg.c, image.c), the wallpaper, and the browser:
                           window (browser.c), HTML (html.c), CSS (css.c), layout
                           (layout.c), fonts (webfont.c), URLs (webutil.c), and
                           JavaScript (js.c, jslib*.c, jsre.c, jsgc.c, jsdom.c)
user/dos/                  the DOS emulator (CPU, DPMI, BIOS, video, files, sound)
user/bin/                  one program per file (fluxinstall.c is the installer)
rootfs/                    files copied into the initrd (/etc, /home/flux, icons,
                           the browser's fonts and home page in /usr/share/www)
```

## Design notes

**Memory map.** Every address space shares the kernel half.

| Range                       | Use                                              |
|-----------------------------|--------------------------------------------------|
| `0x00000000 - 0x3FFFFFFF`   | kernel; identity map of the first GiB of RAM     |
| `0x40000000 - ...`          | user program image, then the `sbrk` heap         |
| `0xB0000000 - ...`          | the framebuffer, in the process that mapped it   |
| `0xBF800000 - 0xBFFFFFFF`   | user stack (up to 8 MiB, grown on page faults)   |
| `0xC0000000 - ...`          | kernel mappings of device memory (framebuffer)   |

The kernel reaches page tables and user frames through the identity map, so
physical memory is used only up to 1 GiB.

**Scheduling.** Each trap pushes a `struct trapframe` on the process's 16 KiB
kernel stack. The kernel is not preemptible (interrupt gates keep IF clear): a
process gives up the CPU only in `schedule()`, when it blocks or when the timer
set `need_resched` and the trap is about to return to user mode.

**Disk I/O.** Filesystem and block-device work may sleep mid-operation, so it
is serialised by one FIFO "VFS lock", taken per call (per 64 KiB chunk for
`read`/`write`). Terminals and pipes never take it. FAT, directory and FSInfo
updates are dirty sectors in the cache; file data bypasses it, a run of
contiguous clusters written as one DMA command. A sync writes dirty sectors in
LBA order, merging neighbours, then issues one FLUSH CACHE.

**Graphics.** GRUB sets the video mode. The kernel maps the framebuffer above
3 GiB with 4 MiB pages; `fbmap` maps it into a process as device memory, which
`fork` shares and `exit` never frees. A mode change in the 3dfx or Radeon driver
points `fb` at the new screen and the desktop rebuilds its back buffer
(`wm_reconfigure`).

**Sound.** The Sound Blaster 16 is always fed signed 16-bit (or unsigned 8-bit
without a 16-bit DMA channel): `write()` converts into a ring, and the interrupt
handler copies a half-buffer into the DMA buffer, padding with silence. The DSP
pauses once both halves are silent. The PC speaker's queue advances in the
timer interrupt.

**The FPU.** CR0.TS is set whenever the CPU's process isn't the one whose
registers the FPU holds. Its first FPU instruction raises #NM, and the handler
swaps the 108-byte x87 state with FNSAVE/FRSTOR. SSE is never enabled.

**The desktop** composes the screen in a back buffer: changes record damaged
rectangles, repainted bottom to top once per pass of the event loop and copied
to the framebuffer. Each window has its own buffer. One `poll` covers
`/dev/input` and every terminal's pty master.

**Networking.** Interrupt handlers pass frames up the stack and TCP timers run
from the timer interrupt. That is safe because interrupts only arrive in user
mode or the halted idle loop. A process waiting on a socket sleeps on it.
Loopback packets are delivered on the next tick. Out-of-order TCP segments are
dropped and resent.

**Pictures.** The JPEG decoder uses a 32-bit bit buffer with a 9-bit Huffman
lookahead and an integer IDCT; progressive files keep all coefficients until
the last scan, chroma is upsampled with a triangle filter, and thumbnails use
only each block's DC coefficient. The wallpaper is drawn once into a
screen-sized surface so repaints are copies. GIF is decoded with LZW into a
palette image. SVG shapes are flattened to polygons and filled by a scanline
rasteriser with four sub-scanlines per pixel (svg.c, the one browser file that
uses the FPU besides the JavaScript engine).

**The browser** keeps a page in arenas: document tree and style sheets, computed
styles (rebuilt when a style sheet arrives), and layout (rebuilt when a picture
arrives). Layout produces a flat list of rectangles, text runs, images and
controls in page coordinates, which painting clips to the view. CSS numbers are
fixed point with two decimals.

**System calls.** `int 0x80`, number in `eax`, arguments in `ebx`, `ecx`, `edx`,
`esi`, `edi`; failures return `-errno`. Every user pointer is checked against the
user range and page tables first.

## Limitations

fluxOS supports a single CPU. The root filesystem lives in RAM; only what you
write to a mounted FAT volume persists. Permission bits are cosmetic apart from
the exec bit, there is one user, and signals cannot have user handlers or jobs
be stopped (no ^Z). SSE instructions raise `SIGILL`.

- **IDE:** DMA needs a legacy-mode PCI controller with bus mastering and a
  drive the firmware put in a DMA mode; anything else uses PIO.
- **SATA:** AHCI only, no NCQ, port multipliers or hot plugging; a controller
  above 4 GiB can't be reached.
- **USB:** no isochronous transfers or UAS, only LUN 0 of mass storage, no
  keyboard LEDs or power management, boot-protocol/plain HID devices only,
  interrupt endpoints polled every 1 ms, controllers above 4 GiB skipped,
  at most 32 devices on an xHCI controller. Unmount or `sync` before pulling
  out a stick.
- **Recent writes can be lost on a crash:** metadata reaches the disk within
  about 5 s; the filesystem stays consistent. `umount`, `reboot` and `poweroff`
  sync (Ctrl+Alt+Del does not).
- **Disks and filesystems:** MBR with four primary partitions only (no GPT,
  extended or logical; a FAT volume without a table is accepted); FAT12/16/32
  only; ISO9660 has no Joliet. FAT files show as `rwxr-xr-x` and `chmod` on FAT/
  ISO is lost on unmount. Raw `/dev/hdX` access and file sizes are limited to
  4 GiB by 32-bit offsets, directories to 4 MiB, FAT names to 255 UTF-8 bytes,
  and FAT timestamps have 2 s resolution.
- **Floppies:** primary controller only, two drives, double- and high-density
  disks. Unmount before removal or recent changes are lost; looking for a disk
  in an empty drive steps the head every 2 s.
- **Desktop:** terminals are a fixed 80x25 with no scrollback; windows resize by
  an outline; dialogs, terminals and the calculator can't be resized; the
  editor shows bytes as CP437 (no UTF-8).
- **DOS programs:** text modes plus VGA 13h/mode X only (other graphics modes
  stop the program); no Sound Blaster recording, ADPCM, direct DAC or MPU-401;
  no mouse, EMS/XMS, batch files or INT 13h (self-booting and copy-protected
  software won't run); text mode reports key presses without releases; every
  instruction is interpreted, so delay-loop timing runs fast; only the
  read-only DOS attribute is kept.
- **Audio:** the Sound Blaster 16 (DSP 4) and HD Audio are output only, with
  no MIDI and no mixing, so one program at a time has `/dev/dsp`; latency is
  about 80 ms. HD Audio has no jack sensing, S/PDIF or HDMI, and surround jacks
  repeat the front pair.
- **Network:** IPv4 only, one card, no IP fragments, no TCP congestion control
  or out-of-order buffering. forcedeth runs at 10/100 Mbit/s without offload,
  VLANs, wake-on-LAN or MSI. HTTPS is TLS 1.3 with TLS_AES_128_GCM_SHA256 and
  X25519 only, and certificates are not verified.
- **Browser:** JavaScript has no async generators, modules or real
  BigInt, and `Proxy` cannot wrap a function; strings count UTF-8 bytes,
  `Symbol`s are strings, `WebSocket` and
  `customElements` are stand-ins that do nothing, typed arrays are plain arrays, and `eval` runs
  in the global scope, so many modern web apps won't run. Layout is an
  approximation: `fixed` boxes scroll with the page, `z-index` only orders absolute and fixed
  boxes, `sticky` acts as `relative`, transforms other than `translate` and all
  `transition`s and animations are ignored, flex rows are lines of inline-blocks, grids lack named lines and
  column flow, and `calc()` takes its first length. GIF animations show their
  first frame; SVG ignores clip paths, masks, filters and patterns. There are
  no font downloads, page cache or text selection, cookies last only until the
  desktop closes, and characters
  outside Latin-1 and common punctuation show as `?`.
- **Graphics:** only the 3dfx and Radeon drivers set modes (others keep the BIOS
  mode, changeable only on an installed system at the next start); no 8-bit
  palette, interlaced or doublescan modes; the 3dfx engine renders 16-bit color
  and is read back over the bus each frame. The GL library has no textures,
  blending, fog, stencil, vertex arrays or selection; lighting is per vertex;
  polygons must be convex; only one process at a time can use the 3D hardware.
- **Pictures:** JPEG arithmetic coding, 12-bit, lossless and CMYK, and
  interlaced PNGs are not shown; EXIF orientation and color profiles are
  ignored; pictures up to 4096x4096. A 1920x1080 photo takes a few seconds to
  decode on a slow machine, and the wallpaper list holds 40 pictures.

## Credits

The default wallpaper, `/usr/share/wallpapers/mountains.jpg`, is a photograph
by Štefan Štefančík on [Unsplash](https://unsplash.com), used under the
[Unsplash License](https://unsplash.com/license). Thank you for sharing it.

The icons are from the public-domain [Tango Desktop
Project](https://commons.wikimedia.org/wiki/Tango_icons), and the desktop's
text is rasterised from Liberation Sans, Serif and Mono.
