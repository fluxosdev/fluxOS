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
> **It was built with AI assistance from Claude, Google Gemini, and ChatGPT Codex.** Every line of fluxOS, from the
> bootloader glue and kernel to the TCP/IP stack, the desktop and the web
> browser, was written with [Claude](https://claude.ai), Anthropic's AI model,
> working through [Claude Code](https://claude.com/claude-code), [Google Gemini](https://gemini.google.com), and ChatGPT
> Codex from instructions given in conversation. Apart from bundled third-party code,
> no project code was written by hand. It has
> been tested by running it in QEMU, not reviewed or audited by people, so
> read it with that in mind.

fluxOS is a small 32-bit x86 Unix-like operating system with preemptive
multitasking, ring-3 processes, paging, a root filesystem on the hard disk it
is installed on, IDE/SATA/floppy/USB storage, FAT and ISO9660, an
NE2000, nForce, Intel PRO/1000 and Intel I217 network drivers with a TCP/IP stack, a zsh-flavoured shell
(**fsh**), a desktop with a web browser and three themes (Kinetik, Kinetik Glass and Windows 95 style Classic), 3dfx Voodoo3 and
Radeon HD 4000 display drivers with a small OpenGL library, an Intel HD Graphics
4600 display driver with 3D, PC speaker, Sound
Blaster 16 and HD Audio sound, and a layer that runs DOS programs in the
terminal. The ISO image is an install CD: GRUB (Multiboot) starts fluxOS
Setup, which installs fluxOS onto a hard disk, and the installed system boots
straight into the desktop.

fluxOS needs a **Pentium 4** or another processor with SSE2 (an Athlon 64, a
Pentium M or anything newer). The whole system is built for it: the kernel
moves pages and framebuffer lines through the SSE registers, the C library
copies with them, GL transforms and lights vertices four lanes at a time, the
desktop, browser and image decoders blend and convert pixels eight 16-bit
lanes at a time in the SSE2 integer instructions, and all floating point,
doubles included, runs on SSE2. On an older processor it stops at boot and
says why.

## Features

**Kernel**
- A startup screen over the boot messages on a framebuffer: the logo, "fluxos"
  in Archivo and a loading bar on black, until the desktop starts. The
  console comes back on a panic, when a program writes to it, or with
  `nosplash` (the boot menu's "show boot messages" entry). Shutting down and
  restarting show the logo on Kinetik Blue's wallpaper.
- Multiboot entry, GDT/TSS/IDT, one trap dispatcher for exceptions, IRQs and
  the `int 0x80` system call gate.
- Preemptive round-robin scheduler (PIT at 100 Hz) with sleep/wait queues.
- Multiprocessor (SMP): the other processors and cores, found in the ACPI
  MADT or the MultiProcessor Specification table, are started with INIT and
  startup IPIs and run user processes alongside the first. Kernel code runs
  on all of them at once, under fine-grained locks (the VFS mutex, each
  process's mm lock, per-subsystem spinlocks); device interrupts stay on the
  8259 PIC and the boot processor. `nosmp` on the kernel command line keeps
  to the first, `maxcpus=N` to N.
  Processes prefer their previous core to retain warm caches, but idle cores
  can steal work immediately and tasks waiting a full time slice take priority
  over affinity. The boot CPU accounts ticks for all cores; secondary cores
  receive a timer reschedule interrupt only at the end of a 50 ms slice,
  and idle cores receive none. Reschedule requests are coalesced, and per-core
  state and ticket-lock counters occupy separate cache lines. Single-core
  systems retain the same round-robin scheduling and need no APIC timer.
- Processes: `fork`, `execve` (static ELF32 and `#!` scripts), `exit`/
  `waitpid`, process groups, signals (`kill`, `sigaction` handlers, `SIGINT`, `SIGPIPE`,
  fault signals; ^C, ^\ and ^Z from the terminal, stopped and continued jobs; `sigaltstack`,
  `sigpending`), sessions (`setsid`, `getsid`), interval timers (`setitimer` real, virtual and
  profiling, `alarm`), paths up to 4096 bytes, lazy FPU and SSE switching (FXSAVE/FXRSTOR).
- IPC between processes: pipes, pseudo-terminals, `AF_UNIX` stream sockets (named in a
  namespace of the kernel's own: binding makes no file) and System V shared memory
  (`shmget`/`shmat`/`shmdt`/`shmctl`; a segment's memory lasts while anything maps it,
  and one its creator never removed goes when the creator exits). The desktop and its
  programs talk over the first and share windows' pixels through the second.
- Threads: a process's threads share its address space, descriptors and signal handlers and run
  on every processor at once; futexes to wait on, a TLS segment per thread (%gs), `exit()` and
  fatal signals ending all of them, `exec` from any of them. libc has POSIX threads on top
  (`<pthread.h>`: mutexes, condition variables, read-write locks, once, keys, barriers, spinlocks;
  `<semaphore.h>`), with `errno`, `malloc` and stdio safe to use from several threads. `threadtest`
  checks them.
- Processor check at boot (a Pentium 4 with SSE2 is the minimum), and a
  write-combining framebuffer through the page attribute table (`pat=off`
  keeps it uncached).
- Memory: bitmap frame allocator over the first 4 GiB of RAM, paging (first
  GiB identity-mapped for the kernel, user pages in the RAM above it first,
  4 KiB user pages), growing user stack, `sbrk` heaps, first-fit kernel heap. Pages
  are cleared and copied (for `fork`) with SSE, the copies streamed past the
  caches.
- VFS over an in-memory filesystem unpacked from a ustar initrd at boot, with
  `/dev/{tty,console,null,zero,random,urandom}`, pipes and pseudo-terminals.
  Started from an installed disk (`root=UUID=...`), its FAT32 partition is the
  root instead and only `/dev`, `/tmp` and `/mnt` stay in memory. Symbolic
  links are followed in paths (`lstat`, `readlink`, `symlink`, `link`), on
  ext2 and in memory.
- Filesystems: FAT12/16/32 with long names (read/write; modification times
  to the second, kept in the creation time's 10 ms field besides the 2 s
  write time), ISO 9660 with Rock Ridge (read), and ext2 (read/write,
  revision 0 and 1): permissions, owners, one-second times, symbolic and
  hard links, files through triple-indirect blocks. ext3 mounts as ext2
  while its journal is clean; ext4 is refused, and volumes with features
  writing would not keep up to date (checksums) mount read-only. `/etc/fstab`
  lines (`UUID=... /src ext2`) are mounted at start-up. The CD's
  initrd is a small setup system, not the whole one: files in memory live in
  the kernel heap, a quarter of RAM (up to 256 MiB, and at most half of the
  first GiB), so the whole system with its compiler in memory (`make
  run-kernel`, `make test-gcc-qemu`) wants a machine with 1 GiB.
- Storage:
  - IDE (ATA LBA28/48 and ATAPI) with bus-master DMA, interrupt driven, falling
    back to PIO on failure; native-mode PCI IDE controllers. Options:
    `ata.nodma`, `ata.poll`.
  - **SATA (AHCI)** disks and ATAPI drives (`ahci.poll`).
  - **NVMe** solid-state disks (`/dev/nvme0n1`, partitions `nvme0n1p1`...),
    one I/O queue per controller, interrupt driven (`nvme.poll`). `make
    run-nvme` tries it in QEMU.
  - **TRIM** for SSDs: SATA drives that advertise DATA SET MANAGEMENT/TRIM
    and NVMe namespaces with Dataset Management (deallocate). `fstrim -av`
    (or `fstrim /mnt/sda`) syncs each FAT or ext2 volume, then tells its
    drive every free run in its FAT or block bitmaps; nothing trims
    automatically.
  - A SATA or NVMe controller whose completion interrupts fail to arrive
    (found out when a wait runs out and the command is done after all) is
    polled from then on, instead of every command taking the whole timeout.
  - Floppy controller (`/dev/fd0`, `fd1`) with a track cache and automatic
    format detection.
  - `/dev/hdX`, `/dev/sdX`, `/dev/nvmeXnY`, `/dev/srN` with MBR partitions behind a write-back
    sector cache, flushed by `kflushd` every 5 s, by `sync`/`fsync`, on
    `umount` and before `reboot`/`poweroff`.
  - A separate clean read cache grows on demand to 1/32 of usable RAM, capped
    at 64 MiB. Cached data uses reclaimable physical pages outside the kernel
    heap. `readcache=16`, `readcache=32` or `readcache=64` sets the cap in MiB;
    `readcache=0` disables it. Sequential small reads fetch ahead within a
    4 KiB block; bulk reads retain multi-sector disk commands.
- USB: **UHCI, OHCI, EHCI and xHCI** controllers, hubs (up to five deep),
  boot-protocol keyboards, HID mice and tablets, Bulk-Only mass storage and
  **USB audio** playback (isochronous transfers on UHCI, OHCI and xHCI).
  Hot plugging via the `kusbd` thread; `lsusb` lists devices. Options:
  `usb=off`, `usb.poll`.
- Filesystems: **FAT12/16/32** read/write (long names, all FATs kept in sync)
  and read-only **ISO9660** with Rock Ridge. `mount`/`umount`, and an
  **automounter** (`kmediad`) that mounts and unmounts CDs, floppies and USB
  disks as they come and go (`automount=off`, `floppy.nopoll`).
- Console: ANSI/VT100 text console on the framebuffer (8x16 CP437 font) or VGA
  text mode, PS/2 keyboard, mirrored COM1, canonical and raw line discipline.
- Input: PS/2 mouse with wheel and the VMware/QEMU absolute-pointer interface,
  USB keyboards, mice and tablets, delivered through `/dev/input`. A
  **Synaptics touchpad** runs in absolute mode: tap to click (two fingers for
  the right button, three for the middle), tap and drag, two-finger scrolling
  (or the right edge on pads that cannot count fingers), clickpads' bottom
  right corner as the right button, palm and while-typing tap rejection, and a
  ThinkPad's TrackPoint through it (middle button held scrolls). ALPS and
  Elantech pads are recognised and stay in their mouse emulation.
  `touchpad=off`, `touchpad.tap=0`. Caps Lock lights its LED; laptop and
  multimedia keys (volume, mute, sleep, power) reach the desktop.
- **ACPI** through [uACPI](https://github.com/uACPI/uACPI), which runs the
  firmware's AML: the embedded controller (EC) and its events, the AC adapter,
  batteries (charge, rate, time left), the lid, the power and sleep buttons, a
  thermal zone (critical temperature shuts down cleanly) and the backlight
  (brightness keys and `power bright`). `SYS_power` and the `power` command
  report it all. **Suspend to RAM (S3)** works, on real hardware (the HP
  Pavilion g6) and in QEMU, from the desktop's Shut Down dialog, the Power
  dialog, the sleep key, closing the lid, or `power sleep`: disks are
  flushed and parked, the other processors parked, and on waking the
  processor, PICs, PCI configuration, IDE (with its DMA mode), AHCI, NVMe, all four
  USB controllers (devices are found again), HD Audio, the network card and
  the PS/2 devices are set up again, with the processors' MTRRs and PCI
  functions brought back to D0 first. On real hardware the wake code runs the
  video ROM's initialisation and sets the boot loader's VESA mode again
  (what pre-KMS Linux needed as `acpi_sleep=s3_bios,s3_mode`), keeping its
  real-mode code out of the first 64 KiB, which some firmware overwrites
  while resuming; under a hypervisor it does neither, since the virtual BIOS
  restores the display itself. Options: `acpi=off`, `acpi=basic` (tables
  only, as before: power-off without AML), `ec=off`, `lid=ignore` (closing
  the lid does nothing), `s3_nobios` / `s3_nomode` (skip the video ROM / the
  mode set), `s3_bios` / `s3_mode` (do them under a hypervisor too),
  `s3_beep` (beeps on the speaker for a machine without a picture: one when
  the firmware hands back to the wake code, two when the kernel is running
  again, three when the devices are back: the way to tell where a machine
  that does not come back got to). `make test-suspend-qemu` checks three
  sleep/wake cycles in QEMU.
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
  - **AMD Radeon HD 6000 graphics of the Llano APUs** (Sumo: HD 6520G, 6480G,
    6550D, 6620G and the rest of 0x9640-0x964F): Evergreen's display
    controllers, with page flipping and the hardware pointer over the memory
    the BIOS shares with them. A laptop panel behind a DisplayPort-to-LVDS
    bridge (TRAVIS) gets native modes: it keeps the timing the BIOS lit it
    with, and a mode change only sets what is scanned out, with smaller
    sizes stretched to the panel by the scaler. The 3D engine runs Sumo's own
    AMD microcode (`SUMO_pfp`, `SUMO_me`) and, once it passes its boot
    self-test, copies between the pages and from the desktop's back buffer
    through the GART, as on the HD 4000s (`radeon.accel=0` turns it off).
    No OpenGL on it yet (see *Work in progress*).
  - **Intel HD Graphics of Haswell processors** (HD 4600, 4400, 4200, 5000,
    Iris 5100, Iris Pro 5200): page flipping at vblank and the hardware
    pointer in the boot loader's mode. The second page lives in the stolen
    memory the BIOS gives the GPU, mapped through its GTT. The blitter (the
    GPU's 2D copy engine, self-tested at boot) copies between the pages and
    from the desktop's back buffer in RAM, which the GTT maps for it. On the
    VGA output (pipe A, FDI to the 8-series chipset's DAC), modes of its own
    from the monitor's EDID (read over GMBUS), up to about 171 MHz. The 3D
    engine draws for the OpenGL library (self-tested at boot, and running
    3D Gears on an HD 4600): a process
    gets 64 MiB of RAM the GPU sees, and the kernel writes the engine's
    commands from checked lists of drawing operations, with three
    hand-assembled Gen7 pixel shaders. Native modes only on VGA (see
    *Work in progress*). `video=...`, `intelgfx.modeset=0`,
    `intelgfx.accel=0`, `intelgfx.3d=0`, `intelgfx=off`.
- Sound:
  - **A software mixer** behind `/dev/dsp`: every program that opens it gets
    a stream of its own, with its own format (8 or 16-bit, mono or stereo,
    4-48 kHz) and buffer, and up to 16 play at once on each card: the
    desktop's sounds over the Sound Player, a DOS game over both. Each stream
    is resampled to the card's rate and they are added up, clipped, at the
    `pcm` volume. A program that falls behind leaves a gap in its own sound,
    not in the others'.
  - PC speaker (`/dev/speaker`) with background tone queue.
  - **Sound Blaster 16**: `/dev/dsp` (OSS-style; the card plays 44.1 kHz
    stereo), `/dev/mixer`, and the OPL2/3 synthesizer (`/dev/opl`). `sb16=off`,
    `opl=off`.
  - **Intel HD Audio** (and codecs such as the Realtek ALC888) as `/dev/dsp`
    when there is no Sound Blaster; output only, every jack plays the same
    stereo, except that plugging in headphones turns the speakers and line
    outs off (a line out turns the speakers off), where the jacks can sense
    it. `hda=off`.
  - **USB audio** (USB Audio Class 1.0: headsets, DACs, speakers, QEMU's
    `usb-audio`): output only. The device's best playback format is chosen
    (16-bit stereo at 48 kHz when it has it; 44.1 kHz and other rates and
    sample sizes work too, and a device with its own clock is followed
    through its feedback endpoint), with the feature unit's volume and mute
    on `/dev/mixer`. While it is plugged in it is `/dev/dsp` and
    `/dev/mixer` (the mixer unless it is open): programs that open `/dev/dsp`
    play on it, programs already playing go on on the built-in card, and
    the nodes are given back when it goes. It is also `/dev/dsp1`.
    `usbaudio=secondary` leaves `/dev/dsp` to the built-in card;
    `usbaudio=off`. `make run-usb-audio` tries it in QEMU.
- Networking: NE2000 (PCI and ISA), **nVidia nForce** ("forcedeth", `eth0`,
  with MII PHY support) and **Intel I217-LM/V** gigabit (the Haswell 8-series
  chipsets' MAC; "e1000e", shares the PHY with the Management Engine,
  `e1000e=off`) and **Intel PRO/1000** ("e1000": the 8254x family and the 82574L, the
  card QEMU's `e1000` and `e1000e`, VirtualBox and many 86Box machines offer, `e1000=off`)
  drivers; Ethernet, ARP, IPv4 (no fragments), ICMP, UDP,
  loopback; TCP with the full state machine, RTT estimation, retransmission
  and fast retransmit; a DHCP client (or `net.ip=... net.gw=... net.dns=...`
  on the kernel command line); BSD sockets for TCP, UDP and ping; DNS from
  `/etc/hosts` and then the DHCP server.
- Kernel log (`dmesg`), CMOS clock, reboot and ACPI power-off (running the
  firmware's `_PTS`).

**fsh, the flux shell (zsh-flavoured)**
- zsh prompt escapes (`%n %m %~ %# %?  %F{color} %B`...), live syntax
  highlighting, grey history autosuggestions, Tab completion menus, prefix
  history search, emacs keys, and continuation prompts.
- Pipelines, `; && || & !`, redirections, `if/while/until/for`, `{ }`, `( )`,
  functions, `time`; `$VAR ${VAR:-x} ${#VAR}`, `$(cmd)`, `$((math))`, `~`,
  brace expansion, globs (a glob that matches nothing is an error, as in zsh).
- Job control (`jobs`, `fg`, `bg`, `wait`, `kill %n`; ^Z suspends the foreground job), history expansion
  (`!! !$ !n !str`), aliases, `source`, `~/.fshrc`, `~/.fsh_history`, AUTO_CD.
- `case ... esac`, here documents (`<<EOF`, `<<-EOF`, quoted delimiters),
  `${VAR#pat} ${VAR%pat}` (and `##`, `%%`), `trap`, `getopts`, `local`,
  `command -v`, `set -e -u -x -f -n`, `umask`, a `test`/`[` with every POSIX
  operator, and `printf` with `%f`.
- **As `/bin/sh`** (started as `sh`, or `fsh --posix`): the POSIX shell that
  `make`'s recipes and `configure` scripts expect. Unquoted `$var` and
  `$(...)` are split on `$IFS`, a pattern that matches nothing stays as it
  is, no brace expansion, no AUTO_CD, no startup files (`$ENV` if set), and
  `"$@"` with no parameters is no word at all. `sh -ec`, `sh -c 'cmd' name
  args`, `sh script` and `sh -n` work as make and configure use them. `make
  test-sh` runs the same scripts under it and the host's `/bin/sh` (dash).

**The desktop** (`/bin/desktop`), started by init at boot
- **Every program is a process of its own**: the desktop process is the window manager,
  the taskbar and Start menu, the sidebar, its folder on the wallpaper and its own dialogs
  (System Properties, Run, Shut Down, Power, Volume Control, the clock). Wave, the terminal,
  the File Manager, the editor and every other program are separate binaries in `/bin`
  (`wave`, `terminal`, `files`, `editor`, `calculator`, `player`, `archiver`, `paint`,
  `gears`, `minesweeper`, `taskmgr`, `diskmgr`, `display`, `mouse`, `datetime`,
  `wireless`, `imageview`) that connect to the desktop over an `AF_UNIX` socket
  (`/tmp/.desktop`) and draw each window's client area into a shared memory segment
  the desktop composes from. A program that hangs or crashes takes only its own windows
  with it; one that exits when its last window closes frees what it used. They can be
  started from a terminal too (`paint picture.png`, `wave https://...`).
- Two themes, chosen on the **Appearance** page of Display Properties. **Kinetik**
  (the default): frames with rounded corners and a soft shadow, captions, taskbar
  and Start menu heading in a gradient of the theme's color with centered titles,
  glassy Start and taskbar buttons, light blue-gray faces with flat outlines, and its
  own wallpaper (a gradient, a glow and sweeping curves, drawn at the screen's size),
  in Blue, Purple, Orange, Green, White or Black; UI text is anti-aliased Archivo.
  **Classic**: the Windows 95 look, a purple desktop, gray beveled windows with navy
  captions, and Liberation Sans. Either way the desktop is a folder browsed on the
  wallpaper (below) with a taskbar (Start button, a button per window, clock, volume
  tray icon), and terminals keep the sharp 8x16 bitmap font. Switching redraws every
  open window at once.
- **The desktop is a file manager**: it opens on `~/Desktop` and shows folders right on
  the wallpaper, each icon with its name beside it, down columns, under a header of
  **Back** (the folders before, or Backspace), **Home** (back to `~/Desktop`) and
  **Favourites** (a menu of the folders in `~/.favourites`, one path a line; folders are
  added from it or from a folder's right-click menu), then a button for each mounted
  disk (**System** for `/`, the mount point's name for the rest). Everything the File Manager does
  works there: opening, dragging, cut/copy/paste, rename, delete, its right-click menu
  (with Display Properties). A shortcut is a small text file, `NAME.app`, with
  `exec=wave` (an app's name, such as `terminal`, `files`, `paint`) or a path, and an
  optional `icon=`; `~/Desktop/Wave.app` starts Wave.
- **Sidebar** down the right edge, as in Longhorn: always in view like the taskbar,
  with maximized windows stopping beside it. Its widgets are an analog clock
  (click for Date and Time), a month calendar (arrows page it, the month's name
  comes back to today) and a graph of the last minute of processor and memory use
  (click for Task Manager), added or removed from its menu (the heading's button,
  "Add widgets...", or a right click). Right-click the taskbar for **Show
  Sidebar**. It is on by default at 1024 pixels wide and more; the choice is kept.
- Oxygen icons (PNG), PNG, JPEG, GIF and SVG decoding (the first bytes decide,
  whatever the name), and a wallpaper (Fill, Stretch, Center or Tile).
- Windows move, minimize, maximize and cascade; the browser, text editor, file
  manager, Task Manager and Gears resize by dragging an edge.
- **Drag and drop between windows**: what is dragged follows the pointer with its icon,
  its name and what letting go would do ("Move to Downloads", "Open in Paint"); the
  pointer is a no-entry sign where nothing takes it, and Escape cancels. Files from the
  File Manager move into another folder window (copied to another disk, or with Ctrl
  held), open in a Text Editor, Paint (pictures), the Sound Player (WAV, MP3, Ogg), the Archive
  Manager (ZIP, 7z) or Wave, and are typed into a terminal as quoted paths. Text
  selected in an editor, and links and pictures from Wave (their address), go into an
  editor where they are let go, into a page's text field, or into a terminal as typed
  (line breaks as spaces, so nothing runs by itself); an address dropped on Wave opens.
- Start menu with cascading submenus (Programs, Accessories, System Tools,
  Settings), Read Me, Run... and Shut Down.... Keys: Windows
  key or Ctrl+Esc for Start, Alt+Tab, Alt+F4.
- **Terminal**: VGA-style text, running fsh on a pty, as many as you like. It opens at 80x25 and
  takes whatever size its window is given (programs hear `SIGWINCH` and read the size with
  `TIOCGWINSZ`); 2000 lines scroll back (wheel, Shift+PgUp/PgDn); output is UTF-8, shown with
  the VGA font's code page 437 glyphs.
- **File manager**: Explorer-style icon view with an address field, Up, New
  Folder, Delete, Rename, cut/copy/paste (also between disks), a right-click
  menu, and drive icons for mounted volumes. Double-click opens folders; runs
  programs; opens pictures, text files, `.wav` files, HTML; other files go to
  `more`.
- **Calculator**: exact decimal arithmetic (30 digits), memory keys, keyboard
  use.
- **Text editor**: Notepad-style, File/Edit menus, selection, a clipboard
  shared with the browser, files up to 1 MB. No undo, word wrap or search.
- **Sound Player**: opens a WAV, MP3 or Ogg Vorbis file (typed path, dropped on it, or from
  the file manager) with Play, Pause, Stop and a seek bar. It runs `play(1)` (`play -s SECONDS`
  starts part-way in) so the player keeps running.
- **Display Properties**: wallpaper and fill mode with a preview (with **(None)** chosen, a
  `#rrggbb` field sets the solid desktop color; **Kinetik** is the theme's drawn
  wallpaper), the theme and its color with a live sample, resolution,
  color depth and refresh rate (with a 15 s "keep this mode?" prompt on the
  3dfx and Radeon drivers), monitor type and EDID details. Settings are saved
  in GRUB's environment block on an installed system.
- **Wireless Networks** (Settings, or the wireless icon in the tray, which is
  crossed out while not connected): the networks in range with their signal and
  security, Connect (asking for a WPA2 password), Disconnect, and how the
  connection is doing. A network joined here is remembered in `/etc/wifi.conf`
  and joined again at startup, as with `wifi connect`; Disconnect forgets it.
  Networks that hide their name, and WPA3 or Enterprise ones, are listed but
  cannot be joined.
- **3D Gears**: the OpenGL demo, drawn by the Radeon, Intel or Voodoo 3D engine when
  there is one, else by the software renderer (S switches renderer, T puts a
  texture on the gears).
- **Paint**: a bitmap editor with a canvas of any size up to 2048 x 2048
  (**New...** and **Size...**), a round **Brush** with an adjustable size (the
  `-`/`+` buttons or `[`/`]`), a square **Eraser**, flood **Fill**, straight
  **Line** and color **Pick** tools, and the 16 EGA colors as its palette (left
  button draws with the foreground, right button with the background;
  **Color...** takes any `#rrggbb`). One level of undo (Ctrl+Z; again to redo),
  Open for PNG, JPEG, GIF, SVG and ICO pictures (also **Edit in Paint** in the
  File Manager's menu), and **Save**/**Save As** write baseline JPEG (quality 90,
  from the built-in encoder in jpegenc.c).
- **Archive Manager**: opens ZIP and 7z archives (double-click one in the File
  Manager, or **Extract All...** on its right-click menu) and shows them like
  folders, with sizes, packed sizes, dates and methods; double-clicking a file
  inside opens it. Extracting (everything, or the selection) runs `unzip` in
  the background with a progress bar and Cancel, and asks before replacing
  files; **Test** checks every CRC.
- **Task Manager** (applications, processes, CPU/memory performance graphs),
  **Disk Manager** (volumes, partition bars, mount/unmount/format, MBR partition
  editing), **Volume Control** and **System Properties**.

**Web browser** (the **Wave** icon): an Internet Explorer 3 style
window with Back/Forward/Stop/Refresh/Home/History/Downloads/Favorites, address bar,
progress gauge and a right-click menu (Back, Forward, Refresh, Stop, Home, Add to Favorites,
Copy Page Address, Find in Page, Save Page As, View Page Source and Print on the page; Open,
Open in New Window, Copy and Save Link As on a link; Save Image As and Copy Image Address on a
picture; Cut/Copy/Paste on the address bar). Pages come over HTTP or HTTPS (through `httpc`, a
fetcher process, so the browser keeps running), or from `file://`.
- **Archives and downloads**: local ZIP and 7z links open in Archive Manager
  without loading the archive into the browser. Binary response prefixes are
  checked before text layout, so an archive served as `text/plain` is downloaded.
- **Keys**: Ctrl+F finds text (every match is marked, Enter and Shift+Enter step through them, Esc
  closes; what is selected is looked for), Ctrl++, Ctrl+- and Ctrl+0 zoom the page from 30% to 300%
  (lengths, text, pictures and form controls all grow, and `@media` queries and `window.innerWidth`
  see the smaller window), Ctrl+D adds the page to the favorites or drops it (about:bookmarks lists
  them, kept in `/home/flux/.browser_bookmarks`), Ctrl+P prints the page (up to 5000 pixels of it) to
  a JPEG picture in `/home/flux/Downloads`, Ctrl+L, F5, Ctrl+H, Ctrl+J and Alt+arrows as before.
  `view-source:` before an address shows the page's text; Save Page/Link/Image As keep a file in the
  downloads folder without leaving the page. A page's `title` attributes show as tooltips after the
  pointer rests for a moment.
- **Network**: each window has its own `/bin/httpc` (built on libc's `<http.h>`),
  started when a page is first fetched and stopped after 30 idle seconds. It keeps
  connections to servers open between requests (HTTP/1.1 keep-alive; one that the
  server closed meanwhile is retried on a new one), speaks HTTP/2 to HTTPS servers
  that offer it (ALPN; HPACK headers, flow control, a request given up on resets only its
  stream, one request at a time per connection), remembers host names for five
  minutes, decodes chunked, gzip, deflate and Brotli bodies, and keeps a cache in
  `/home/flux/.cache/http` (16 MB): fresh copies (`Cache-Control: max-age`,
  `Expires`, or a tenth of the time since `Last-Modified`) are used without asking,
  stale ones are checked with `If-None-Match`/`If-Modified-Since` and reused on a
  `304`, `no-store` and `Vary` other than `Accept-Encoding` are respected, and
  Refresh (F5) asks the server about the page itself. If a fetcher cannot be started
  (eight run at most) the page is fetched by `wget` as before.
- **HTTPS authentication**: Mbed TLS 3.6.7 verifies the server's certificate
  chain, hostname, validity dates, key usage and the server's handshake
  signature (TLS 1.3 CertificateVerify, TLS 1.2 ServerKeyExchange) against `/etc/ssl/ca-certificates.crt`. Invalid authentication
  fails the connection. The trust bundle contains the Mozilla roots from the
  build host's Debian ca-certificates package, excluding local additions.
- **WebSocket**: real `ws://` and authenticated `wss://` connections through
  `/bin/wsc` workers, including subprotocol negotiation, text and binary messages,
  fragmentation, ping/pong, close handshakes, cookies and open/message/error/close
  events. Client frames are masked; invalid handshake proofs, server masking,
  reserved frame bits and malformed UTF-8 are rejected. HTTPS pages cannot open
  insecure sockets. Each page has at most eight live sockets, with a 4 MB message
  and send-queue limit. Compression extensions are not negotiated.
- **Loading limits**: up to 1024 queued resource entries, with a growing queue;
  documents and resources may be up to 16 MB. Requests are still serialized per
  window. Oversized binary/large request bodies require httpc; the wget fallback
  rejects bodies it cannot represent instead of silently truncating them.
- **HTML**: follows the main rules of the HTML5 tree builder (implied and
  auto-closed tags, tables, character references, quirks mode); UTF-8,
  Latin-1 and Windows-1252. The complete HTML5 named character-reference table
  includes long names, legacy forms and references producing two code points.
- **CSS**: type, class, id, attribute and structural selectors with all
  combinators, `:not()`/`:is()` with full selectors, specificity, `!important`,
  `:has()`, `:nth-child()`/`:nth-last-child()` with `of <selector-list>` filters, `@media`, custom properties and `var()`, `calc()`,
  `max-height`, `aspect-ratio`, `::before`/`::after` text,
  gradients, rounded corners, and the old presentational attributes. Cascade layers
  (`@layer`, including nested/anonymous layers, ordering statements, cross-sheet names,
  reversed important precedence and `revert-layer`) participate before specificity. Nesting
  (`&` and nested `@media`), `@supports` (and `CSS.supports()`), `@import`,
  media queries level 4 (ranges such as `400px <= width <= 700px`, `or`,
  heights, `prefers-reduced-motion`...), CSS Color 4/5 (`oklch()`, `oklab()`,
  `lab()`, `lch()`, `hwb()`, `color()`, `color-mix()`; worked out in
  csscolor.c) and logical properties (`margin-inline`, `padding-block`,
  `inset-inline`, `inline-size`...). `<style>` and `<link>` elements that
  scripts add or change apply as soon as the script returns.
  Text: `letter-spacing`, `word-spacing`, `word-break: break-all`, `overflow-wrap`
  (`break-word`, `anywhere`), `text-overflow: ellipsis`, `font-variant: small-caps`,
  `text-shadow`, `text-decoration-color`/`-style` (solid, double, dotted, dashed, wavy);
  boxes: `outline` (with `outline-offset`), `list-style-position`, `table-layout: fixed`,
  `caption-side`, `empty-cells`; interaction: `cursor`, `pointer-events: none`,
  `user-select: none` (and `accent-color`, `appearance`, `will-change` and
  `scroll-behavior`, which are accepted and ignored). More selectors: `:target`,
  `:lang()`, `:read-only`/`:read-write`, `:default`, `:indeterminate`, `:valid`/`:invalid`
  (`required`, `type=email`/`url`/`number` with `min`/`max`, `minlength`, `maxlength`),
  `:in-range`/`:out-of-range`, `:scope`, `:dir(ltr)`, `:user-invalid`, `:modal`, `:popover-open`,
  `:open`, and the pseudo-elements `::placeholder`, `::selection`, `::marker` and `::backdrop`. Counters: `counter-reset`, `counter-increment`
  and `counter()`/`counters()` in `content` (one flat set of counters, not scoped by nesting).
  Also `tab-size`, `text-align-last`, `-webkit-line-clamp` (the lines are cut off, with no
  ellipsis), `list-style-image`, and `hyphens` and `resize`, which are accepted and ignored.
  `::first-letter` (its own size, color and box) and `::first-line` (the line is broken with its
  font; nested inline text takes its color), `all` (`initial`, `inherit`, `unset`; `revert` works as
  `unset`), `text-wrap: nowrap`, `display: ruby`, the individual `translate`, `rotate` and `scale`
  properties, the math functions `round()`, `mod()`, `rem()`, `abs()`, `sign()`, `sin()` and the other
  trigonometric functions, `pow()`, `sqrt()`, `hypot()`, `log()`, `exp()` and the constants `pi` and
  `e`, the `lh`, `rlh`, `ic` and `cap` units (and `cq*` units, which are the viewport's, there being
  no container queries), relative colors (`rgb(from <color> r g b / alpha)`, and `hsl()`, `hwb()`,
  `lab()`, `lch()`, `oklab()` and `oklch()` forms) and the system colors (`AccentColor`, `Canvas`,
  `Mark`...). Around 120 properties that change nothing this renderer draws (`color-scheme`,
  `touch-action`, `overscroll-behavior`, `scrollbar-width`, `content-visibility`, `contain`,
  `font-feature-settings`, `scroll-margin`, `scroll-snap-*`...) are parsed but ignored;
  `@supports` and `CSS.supports()` return false for these unimplemented properties so
  pages can choose their fallback styles.
  Pictures: comma-separated `background-image` layers (PNG, JPEG, GIF, WebP, ICO,
  SVG, gradients and `data:` URLs) with `background-size` (lengths, percentages, `cover`, `contain`),
  `background-position` (keywords, offsets from an edge), `background-repeat`, `background-origin`,
  border/padding/content `background-clip`, fixed picture attachments and the `background`
  shorthand, painted from the bottom layer up inside rounded corners; the old
  `background=` attribute; `object-fit` and `object-position` on `<img>` (also with rounded
  corners, as for round avatars). Gradients are `linear-gradient()` (angles, sides and corners),
  `radial-gradient()` (circles and ellipses, the four extent keywords or sizes, `at` a position),
  `conic-gradient()` (`from` an angle, `at` a position) and their `repeating-` forms, with any number of
  color stops (one or two positions each, in lengths, percentages or angles), interpolated with
  premultiplied alpha, and sized, placed and tiled by `background-size`, `-position` and `-repeat`
  like pictures (the old `-webkit-` forms are read too). Relative addresses in a style sheet are
  resolved against the sheet's own address. `overflow`, `overflow-x` and `overflow-y`.
- **Layout**: blocks with margin collapsing, inline text, inline-blocks, lists,
  tables, flex rows, grids (columns, rows, areas, auto rows and columns, implicit columns past the
  template, `grid-auto-flow: column`, spans, `dense`, named lines and positive/negative named-line
  occurrences in explicit and integer-repeat tracks; `justify-content` and `align-content` place the
  tracks), `align-content` for the lines of a wrapping flex container, multi-column boxes
  (`columns`, `column-count`, `column-width`, `column-gap`, `column-rule`, `column-fill`;
  balanced columns cut between lines, `break-inside: avoid`, `break-before`/`-after: column`;
  `column-span` is not supported), borders, backgrounds, images and form controls. Floats (`float`, `clear`,
  clearfixes, text wrapping, new formatting contexts beside floats) and
  positioning (`relative`, `absolute`, `fixed` and `sticky` with `top`/`right`/`bottom`/
  `left`, `inset`) work, painted in stacking-context order (`z-index` on positioned boxes and
  flex/grid items, contexts from opacity, transforms, fixed and sticky boxes), `box-shadow`
  (the first outer shadow, drawn as fading layers) and `transform: translate()`.
  `overflow` other than `visible` clips a box's contents to its padding box, and
  `overflow: auto`/`scroll` boxes scroll: scroll bars (laid over the box's right and bottom
  edge) with arrows, thumb and track, the wheel over the box (the page scrolls when the box
  is at its end), nested boxes, flex and grid items such as side panes, boxes inside
  `position: fixed` ones, and links and controls inside them; scripts read and set
  `scrollTop`, `scrollLeft`, `scrollWidth`, `scrollHeight`, `scrollTo()`, `scrollBy()` and
  `scrollIntoView()` on them. `fixed` boxes stay where they are in the window as the page scrolls; `sticky` boxes
  follow it until the end of the box they are in (against the window, not a scrolling
  box inside the page). `<progress>`, `<meter>` and `<ruby>` (the annotation above its
  base) are drawn. The top layer: modal dialogs and open popovers are laid out over everything (above
  any `z-index`, out of whatever clips or transforms their parents), in the order they opened,
  centred in the window by the user agent sheet, each on its `::backdrop`. Anchor positioning:
  `anchor-name`, `position-anchor` (a popover's default anchor is the button that opened it),
  `anchor()` in the insets (sides, `center`, percentages, a fallback, and `calc()` with a length
  added), `anchor-size()`, `position-area`, `justify-self`/`align-self: anchor-center` and
  `position-try-fallbacks` (`flip-block`, `flip-inline`, `flip-start` and other areas; named
  `@position-try` rules are not supported); a page with anchors is laid out twice. `width` and
  `height` take `fit-content` (and `min-content`/`max-content`, alike), and an absolute box with
  `top` and `bottom` but no height stretches between them. `<audio>` plays MP3, Ogg Vorbis and
  WAV: one `wave --audio` process decodes (minimp3, stb_vorbis), mixes the streams of every page and
  holds `/dev/dsp` only while something sounds; `<audio controls>` is a play button, the time and a
  bar to seek along. Nothing plays `<video>`: it is a box of its size with its poster and a play
  button, and a click downloads the file (its `src` or first `<source>`); the fallback inside media
  elements is not shown, as in a browser that plays them. An `<object>` shows its fallback content. `<template shadowrootmode>` in a page's HTML attaches a
  shadow root (declarative shadow DOM).
- **Forms**: `<select>` opens a list (arrows, Home/End, typing a letter, Enter, wheel and
  scroll bar for long ones); fields with a `<datalist>` suggest matches as you type;
  `<input type=range>` is a slider (drag, or arrows, Home and End; `min`, `max` and `step`),
  `type=color` a swatch that asks for a `#rrggbb`, `date`, `time`, `datetime-local`, `month`
  and `week` fields show their pattern and take only the digits and punctuation they need,
  `type=number` only numbers; `type=file` asks for a path, and a form with
  `enctype="multipart/form-data"` sends the files (up to 8 MB each) in a proper multipart body;
  `autofocus` focuses a field at load.
- **Interaction**: `:hover`, `:active`, `:focus` and `:focus-within` restyle the page as the
  pointer and focus move. Drag to select text, Ctrl+A to select all, Ctrl+C to
  copy to the shared clipboard. Scripts hear the pointer move while a button is held
  (`pointermove` and `mousemove`, with `buttons`), so pages can drag things themselves.
  HTML drag and drop: `draggable="true"` elements, links and pictures can be dragged
  (`dragstart`, `drag`, `dragenter`, `dragover`, `dragleave`, `drop`, `dragend` with a
  `DataTransfer`: `setData`/`getData`, `types`, `items`, `effectAllowed`/`dropEffect`); a drop
  into a text field the page does not take types the text there; Escape cancels. Links and
  pictures can also be dragged out to other windows (their address); selected text cannot be
  dragged. The window and taskbar show the page's favicon
  (`<link rel=icon>` or `/favicon.ico`; PNG, ICO, GIF or SVG).
- **Cookies**: a jar shared by every window fills from
  `Set-Cookie` headers and `document.cookie` (domain, path, `Secure`,
  `HttpOnly`, `Max-Age`, `Expires`) and goes out on every request, so
  logins and `fetch` calls keep their session. Cookies with an expiry are kept
  in `/home/flux/.browser_cookies` (plain text) and are there next time; session
  cookies last until the desktop closes. `SameSite` defaults to Lax and is enforced using schemeful sites and the
  public suffix list (including private suffixes). Invalid public-suffix Domain,
  insecure SameSite=None and invalid __Host-/__Secure- cookies are rejected.
- **Pictures**: PNG, JPEG, WebP, animated GIF, ICO (the largest icon) and SVG, as `<img>` and as an
  inline `<svg>`. `srcset`/`sizes` and `<picture><source>` choose a picture of
  the right width in a format the browser can decode (including WebP, excluding AVIF).
  GIF frames decode on demand with delay, disposal, transparency and loop-count handling.
  WebP animations still show their first frame. The SVG renderer (svg.c) draws shapes, paths (curves and
  arcs), groups, `use`, text, transforms, `viewBox`, solid and gradient fills,
  dashed strokes with caps and joins, opacity, and simple `<style>` rules.
- `<details>`/`<summary>` open and close when clicked. Links, `GET`/`POST` forms, `<meta refresh>`, fragments, redirects, iframes,
  `:visited` colors, and anti-aliased text (Liberation Sans, Serif and Mono in
  12 sizes, rasterised by `tools/mkfont.py`).
- **JavaScript**: a bytecode compiler and virtual machine (js.c, jsvm.h,
  jslib.c, jslib2.c, jsre.c)
  that runs inline and external scripts (`defer` ones and modules after the
  rest), then `DOMContentLoaded` and `load`. Scope analysis resolves local
  variables to frame slots and captured variables to heap environment slots;
  functions compile on first use. Ordinary functions, generators, async
  functions, classes, destructuring, modules, `eval` and event handlers all run
  as bytecode. Property hash tables preserve insertion order, and ASCII
  string operations have direct byte paths while retaining UTF-16 semantics.
  It covers most of ES2020 and some later: `let`/`const`, arrow functions,
  template literals, destructuring, spread, optional chaining, classes
  (fields, `#private`, `extends`, `super`, statics and `static {}` blocks),
  ES modules (`<script type=module>`, `import`/`export` in all their forms with
  live bindings, `import()`, `import.meta`, top-level `await`, import maps;
  `nomodule` scripts are skipped), generators, async generators and
  `async`/`await` (run as coroutines on their own stacks),
  Promises with a microtask queue, symbols and BigInt (with `BigInt64Array` and
  `DataView`'s BigInt accessors) as their own types, regular expressions with lookbehind,
  named groups, sticky (`y`) matching, match indices (`d`), Unicode classes and property escapes
  (`\p{L}`, `\p{Script=Greek}`, `\p{Extended_Pictographic}`), case folding beyond ASCII and the `v`
  flag's nested classes with `--` and `&&`; Unicode `toUpperCase`/`toLowerCase` (ß is SS),
  `trim` and `normalize()` (NFC, NFD, NFKC, NFKD; tables from `tools/mkunicode.py`), and the usual built-ins (Map, Set,
  Weak*, typed arrays, JSON, Date, Reflect, `Proxy` (get, set, has, deleteProperty, ownKeys, apply and construct
  traps), the ES2025 Set methods, iterator helpers (`Iterator.from`, `map`, `filter`, `take`,
  `drop`, `flatMap`, `reduce`, `toArray`... on arrays', collections' and generators' iterators),
  `Array.fromAsync`, `Error` causes, a small Intl (`NumberFormat` with currencies, percentages,
  units, compact, scientific and engineering notation, significant digits, sign display and
  `formatToParts`, also behind `toLocaleString`; `Locale`, `DisplayNames`, `DateTimeFormat`,
  `Collator`, `PluralRules`, `RelativeTimeFormat`, `ListFormat`, `Segmenter`), `structuredClone`...).
  `URLPattern` (named groups, wildcards, regular expression groups, `{}` and modifiers),
  `File`, `FileList`, `FileReader` and `input.files` (only the file the user picked: a page
  cannot set a file input's value), `CompressionStream` and `DecompressionStream` (gzip, deflate,
  deflate-raw), `TextDecoderStream` and `TextEncoderStream`. Form-associated custom elements get
  `attachInternals()` (form values for `FormData`, validity, custom states); `setHTML()` with the
  `Sanitizer` (and `setHTMLUnsafe()`, `Document.parseHTMLUnsafe()`), Trusted Types' policies and
  values (nothing enforces them), and `caretPositionFromPoint()`. What there is no hardware or
  service for answers as a browser without it would: geolocation fails, `play()` rejects,
  speech has no voices, pointer lock and screen orientation lock are refused, there are no
  gamepads or credentials, `scheduler.postTask()` and `ReportingObserver` exist. The DOM (jsdom.c) has
  `document`, `querySelector` (including `:is()`, `:where()`, complex `:not()`
  and filtered sibling selectors), `innerHTML`, `classList`, `dataset`, events
  (capture and bubbling phases, `once`/`signal` options, `handleEvent` objects,
  `MouseEvent`, `PointerEvent`, `KeyboardEvent` and the other event classes;
  the pointer's moves, presses and hovering reach scripts as `pointer*`,
  `mouse*`, `mouseenter`/`mouseleave`), `scroll` and `resize` events,
  `MutationObserver`, `IntersectionObserver` and `ResizeObserver`, custom
  elements (`customElements.define` with upgrades and lifecycle callbacks;
  `attachShadow` creates a separate open or closed tree, with scoped styles,
  named/default slots, coalesced `slotchange` events, slotted event paths and event retargeting), `<template>`
  content, `<dialog>` (`show()`, `showModal()`, which makes the rest of the page inert,
  `requestClose()`, `closedby`, `<form method=dialog>`), popovers (`popover` auto, manual and
  hint, `showPopover()`/`hidePopover()`/`togglePopover()`, nesting, light dismiss and Escape,
  `beforetoggle` and `toggle`), invoker buttons (`popovertarget`, and `commandfor` with
  `show-modal`, `close`, `request-close`, the popover commands and `--custom` ones, sending
  `command` events), `HTMLAudioElement` (`play()` promises, which need the user to have pressed a
  key or the mouse on the page first unless muted, `pause()`, `currentTime`, `duration`, `volume`,
  `muted`, `loop`, `autoplay`, `canPlayType()` and the media events), `HTMLInputElement` and the other interfaces for
  `instanceof`, `TreeWalker`, `Range`/`Selection`, form validation,
  `form.submit()`/`requestSubmit()`, `load`/`error` events for pictures,
  scripts and style sheets,
  `replaceChildren`, `insertAdjacentText` and `insertAdjacentElement`,
  timers, `location`, `history` (`pushState`/`replaceState` with `popstate`,
  and `hashchange`, without reloading), `matchMedia`, `localStorage`
  (per origin, kept in `/home/flux/.browser_storage`) and `sessionStorage`
  (per origin and window), `XMLHttpRequest`, `fetch`,
  `URL`, and more: `FormData`, `Blob`, `TextEncoder`/`TextDecoder`, `crypto.getRandomValues`/`randomUUID`,
  `DOMParser`, a style element's `sheet.insertRule()`, and a stand-in for `Notification`.
  Windows talk across tabs, popups and frames: `window.open()` returns the new window,
  `opener`, `parent`, `top` and an iframe's `contentWindow` are windows too, and `postMessage`
  carries structured clones (with `origin`, `source` and target-origin checks); a window a
  script opened can be closed again. `indexedDB` has object stores, indexes (unique,
  multi-entry), cursors, key ranges, version upgrades and transactions that roll back on abort,
  kept per origin in `/home/flux/.browser_idb`.
  Constructed `CSSStyleSheet` objects remain outside the DOM; documents and shadow roots
  adopt shared, mutable sheet lists, and replacements, rule edits and disabling update adopters.
  Shadow styles support `:host`, `:host(...)` and `::slotted(...)`, with outer/inner cascade precedence.
  Web Crypto adds HMAC key import/generation/export and signing/verification with SHA-1/256/384/512,
  raw and JWK formats, usage/extractability checks and native signature comparison. Downloaded fonts use FreeType and Brotli, with CSS `@font-face`, `FontFace`, and `document.fonts` loading. `element.animate()` interpolates numeric values,
  lengths and RGB colors over time, with pause/play/reverse, seeking, fill,
  direction, iterations and common easing functions. Also `MessageChannel`
  and `BroadcastChannel` (within the page), `XMLSerializer`, `Path2D` and `ImageData` (data only: no canvas
  to draw on), the `beforeunload`, `pagehide` and `visibilitychange` (minimized) events, and
  `unhandledrejection` for promises that are rejected with nobody to hear it.
  `fetch` and `XMLHttpRequest` enforce CORS (including preflights and exposed
  response headers), credential modes and mixed-content/local-file restrictions.
  They send request headers and any allowed method (`PUT`, `DELETE`...), and
  preserve binary request/response bodies. Response supports `arrayBuffer()`,
  `bytes()`, `blob()` and an incremental `ReadableStream` body. Default streams support
  queue-size strategies, serialized pulls, cancellation, locking and piping; `WritableStream`,
  `TransformStream`, `CountQueuingStrategy` and `ByteLengthQueuingStrategy` are available. Typed arrays and
  DataView use shared ArrayBuffer bytes, offsets, endian handling and conversion
  on indexed writes; core string indexing, lengths, slicing and search use
  UTF-16 code units. `crypto.getRandomValues` reads the OS random device.
  Changes are laid out again when a script returns; runaway
  scripts are stopped. `/usr/share/www/js.html`, `css.html` and `images.html`
  (linked from the home page) are live test pages. `features.html` demonstrates
  the HTML, CSS and DOM additions in 0.2.0, and `modern.html` the modules,
  observers, custom elements, nesting and colors that came after.

**DOS programs** (`/bin/dos`)
- Runs `.COM` and MZ `.EXE` files in the terminal (`./GAME.EXE`, `dos game`, or
  double-click). `dos` emulates a PC in user space: an x86 CPU (8086 to 486, no
  FPU), 640 KiB of memory, a BIOS and DOS 5 services, and a DPMI 0.9 host for
  extenders such as DOS/4GW (DOOM and similar).
- Drives: `C:` is `/dos`, the folder DOS programs install to (`C:\GAMES` is
  `/dos/GAMES`), `E:` is the fluxOS root, `A:`/`B:` are `/mnt/fd0`/`fd1`, `D:`
  is `/mnt/cdrom`. Standard handles are fluxOS descriptors, so pipes and
  redirection work. EXEC of a fluxOS program runs it as a real process, and
  `COMMAND.COM` runs the shell.
- Screen: text mode (80x25 with colors and box drawing, ANSI.SYS), and VGA
  mode 13h and "mode X" graphics doubled to 640x400 in the desktop's terminal.
  **Ctrl+\\** stops the emulator; `dos -l FILE` logs what is unsupported.
- Sound: the PC speaker, AdLib/OPL, and a Sound Blaster 16 at 220h, IRQ 5,
  DMA 1/5 (DSP, mixer, both DMA controllers) passed on to the real devices.

**OpenGL** (`libGL.a`, `<GL/gl.h>`, `<GL/fluxgl.h>`, `<GL/glu.h>`)
- Most of OpenGL 1.1's fixed-function pipeline: `glBegin`/`glEnd` and
  vertex arrays, matrix stacks, per-vertex lighting (eight lights),
  flat/smooth shading, culling, depth and 8-bit stencil buffers, scissor,
  fog (linear, exp, exp2), six user clip planes, `glTexGen` (object, eye,
  sphere map), display lists, `glPushAttrib`/`glPushClientAttrib`, the
  raster position with `glBitmap`, `glDrawPixels`, `glCopyPixels` and
  `glPixelZoom`, `glGet*` (lights, materials, textures and the rest), and
  basic GLU.
- 2D textures: texture objects, `glTexImage2D`/`glTexSubImage2D` from
  RGB(A), BGR(A), luminance and alpha bytes, `glCopyTexImage2D`,
  `glGetTexImage`, mipmaps (`gluBuild2DMipmaps` makes them; the software
  renderer picks the level at each pixel), nearest or bilinear filtering,
  repeat or clamp, the modulate, replace, decal and blend environments, and
  the texture matrix, all perspective-correct. Two texture units
  (`GL_ARB_multitexture` and GL 1.3's names for it), and
  `fglGetProcAddress` for programs that look calls up by name. The Radeon
  and Intel engines sample level 0 in their pixel shaders; a Voodoo context
  goes on in software once it draws with a texture.
- What the 3D engines cannot draw moves a context to software, keeping its
  picture (and its depths, from a Voodoo): the stencil test, a second
  texture, and fog after a texture (untextured fog, and fog into black
  after `GL_MODULATE`, are folded into the vertex colours and stay on the
  hardware). `GL_ARB_multitexture` is listed only in software, so that
  programs with a multipass path take it on the hardware. `make test-gl`
  checks the software renderer on the host.
- Four backends, tried in order: the Radeon 3D engine (PM4 command buffers),
  Haswell's Intel 3D engine (drawing operations the kernel turns into
  commands), the 3dfx engine (driven from user space, as Glide does), and a
  software scanline rasterizer. `FGL_NO_RADEON`, `FGL_NO_INTEL`,
  `FGL_NO_TDFX`, `FGL_SOFTWARE`, `FLUXGL_DRIVER=radeon|intel|tdfx|soft` and
  `LIBGL_ALWAYS_SOFTWARE` choose.
- `fglCreateContext`, `fglMakeCurrent`, `fglResize` and `fglSwapBuffers` play
  GLX's part. Matrices, vertex transformation, lighting and clipping use SSE;
  the software rasterizer sets up its depth and colour planes in one SSE
  register and fills spans four pixels at a time with SSE2. A `<math.h>` is
  included: floats and doubles both in SSE2.

**Userland** (`/bin`): 7z [ basename beep cat chmod clear cmp cp cut date dd desktop
(and its programs, above) df diff dirname dmesg dos echo env false fetch find fluxinstall free fsh fstrim grep gunzip
gzip halt head hexdump host ifconfig init install ln ls lsblk mbcheck mkdir mkfs.ext2
mkfs.fat mkiso mktemp fsck.ext2 mixer more mount mv nc nproc patch ping play poweroff
printf ps reboot rm rmdir sed
seq sh sleep sort spin stat sync tail tar tee test threadtest top touch tr true
truncate umount uname uniq unzip uptime wc wget xargs yes zcat zip (and httpc, the
browser's fetcher), on a small libc (stdio, malloc, dirent, time, sockets, a DNS
resolver, archives, Deflate both ways, an HTTP/1.1 and HTTP/2 client and POSIX
threads). In `/usr/bin`, cross-built from their sources with the fluxOS toolchain
(`make ports`): **GNU make 4.4.1** and **binutils 2.42** (`as`, `ld`, `ar`, `ranlib`,
`nm`, `objcopy`, `objdump`, `readelf`, `strip`, `size`, `strings`); and, after `make
toolchain-native`, **GCC 13.2.0** itself (`gcc`, `cc`, `cpp`, `g++`, `c++` with `cc1`,
`cc1plus` and `collect2` in `/usr/libexec`, `libgcc`, the C++ runtime `libstdc++` with
its headers, and the system's headers and libraries in `/usr/include` and `/usr/lib`):
`gcc -O2 hello.c` on fluxOS makes the same program the cross compiler makes on Linux,
byte for byte once stripped, and `g++` builds C++ programs with iostream, the
containers and exceptions (`make test-gcc-qemu`).
- The tools the Makefile runs, written for this: `tar` (ustar create, list,
  extract, `-C`, `-z` through `gzip`; reads GNU and pax archives too),
  `gzip`/`gunzip`/`zcat` (libc's Deflate: `<deflate.h>` compresses and expands
  as streams), `find` (`-name -path -type -newer -size -prune -exec ... {} +`),
  `install`, `cmp`, `diff` (Myers, normal and unified, `-r -N -i -b -w -B`),
  `patch` (unified and normal, fuzz, offsets, `-p -R -N --dry-run`, rejects),
  `sed` (addresses, ranges, `s///` with every flag, `y`, `a i c`, `b t T`,
  hold space, `{}`, `-i -n -E -s`), `cut`, `xargs` (`-0 -n -I -r`), `truncate`,
  `dd`, `ln`, `mktemp`, `nproc`, `printf`, `test`/`[`, `true`, `false`. `make
  test-tools` builds them for the host and checks them against the GNU
  originals (archives each side can read, diffs each side's `patch` applies,
  the same output for the same scripts).
- The libc is enough of POSIX for GNU programs to build against it unchanged
  (GNU make, binutils and GCC do, cross-compiled; see `self-hosting-roadmap.md`),
  and its headers are C++-clean, so that `libstdc++` builds on it too:
  `printf` and `strtod` that agree with glibc digit for digit (exact
  floating-point conversion, correct rounding), `scanf`, seekable stdio with
  `popen`, `tmpfile` and memory streams, `getopt_long`, POSIX regular
  expressions, `fnmatch` and `glob`, `select`, `mktime`/`timegm`/`strftime`,
  `realpath`, `mkstemp`, `sysconf`, `<termios.h>`, a "C" locale with the
  wide-character functions, users from `/etc/passwd`, C99's `<math.h>`,
  `getaddrinfo`, `__cxa_atexit`, and constructors and destructors from
  `.init_array` and `.fini_array` (what GCC's `crtbegin.o` and C++ static
  objects use). Processes may hold 256 descriptors, pipes 64 KiB.
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
  [--max-redirect=N] [--post-data=STRING] [--method=VERB] [--header=LINE] [--http1.1]... URL` follows redirects and decodes
  chunked bodies (HTTP/1.1, or HTTP/2 where an HTTPS server offers it, through libc's `<http.h>`, writing the body
  as it arrives). `https://` uses libc's own TLS client (`<tls.h>`: TLS 1.3, or
  TLS 1.2 with ECDHE and AES-GCM or ChaCha20-Poly1305). It encrypts but does not verify the server's certificate.
- `beep [-f HZ] [-l MS] [-r N] [-d MS] [-n ...]` and `beep -m MUSIC` (BASIC's
  PLAY language); `play [-q] [-s SECONDS] FILE...` plays WAV (8 to 32-bit PCM,
  32-bit float), VOC, MP3 and Ogg Vorbis files (the last two with Wave's decoders, up to 64 MB), `play -t HZ [-l MS]` a test tone; `mixer [CHANNEL LEVEL]`.

## Building

On a Debian/Ubuntu host:

```sh
sudo apt install build-essential qemu-system-x86
# optional: mtools, and e2fsprogs and xorriso, which the tests check images with
```

No cross compiler is needed: everything is built with the host GCC using `-m32
-march=pentium4 -ffreestanding`, and nothing links against libgcc or the host
libc. GRUB is not built or downloaded: its boot images and the modules the
menus load are checked in (`firmware/grub`, Debian 12's GRUB 2.06, with their
provenance and hashes in its README; 2.12 reboots some older PCs right after
"Welcome to GRUB!"), and `mkiso`, fluxOS's own, makes the ISO from them: El
Torito for CDs, and a hybrid MBR so the image written to a USB stick starts
too. `make MKRESCUE=grub-2.06/usr/bin/grub-mkrescue` makes it with
grub-mkrescue instead, from GRUB fetched by `tools/fetch-grub.sh`, which also
remakes `firmware/grub` (`--firmware`). The ISO boots on legacy BIOS only.

```sh
make            # builds fluxOS.iso
make check      # verifies the kernel is a valid Multiboot image (mbcheck)
make test-browser # DOM, CSS/layout, binary/Unicode, shadow/motion and HTTP policy regressions
make test-js     # language, module, suspended-frame and real garbage-collection regressions
make bench-js   # ten standalone JavaScript workloads with wall time and peak memory
make test-browser-fonts # real glyph rendering and malformed-font rejection
make test-tls     # local OpenSSL servers: authenticated handshakes and rejected certificates
make test-websocket # local ws/wss transport and browser API integration tests
make test-kernel # runs allocator, filesystem (FAT and ext2, judged by e2fsck), pipe and socket regressions on the host
make test-libc   # libc printf/strtod/scanf/regex/fnmatch/getopt/time against glibc, on the host
make test-libc-qemu # the libc on the real kernel (files, popen, pipes, select, time) in QEMU
make test-tools  # tar, gzip, sed, diff, patch, find... built for the host, against GNU's
make test-sh     # fsh as /bin/sh, built for the host, against the host's /bin/sh
make test-gl     # the GL library's software renderer, built for the host, drawn with pixel by pixel
make test-gcc-qemu # the native GCC compiles, links and runs a program on the real kernel
make test-desktop-jobs # decoder helper responsiveness, transfers, cancellation and recovery
make test-desktop-jobs-qemu # same protocol tests with fluxOS's blocking pipes
make test-kernel-qemu # boots kernel regressions with a disposable initrd and scratch disk
make test-install-qemu # installs from the mkiso CD with an ext2 /src partition, then boots the disk
make test-kernel-qemu KERNEL_TEST_CPUS=4 # same tests on four Core 2 Duo CPUs
make test-kernel-qemu KERNEL_TEST_CPUS=2 KERNEL_TEST_MEMORY=2048 # larger RAM and read cache
make test-kernel-qemu KERNEL_TEST_CMDLINE=readcache=0 # direct-read fallback
make test-kernel-qemu KERNEL_TEST_CPUS=4 KERNEL_TEST_CMDLINE=nosmp # single-core fallback
make test-kernel-qemu KERNEL_TEST_CPUS=4 KERNEL_TEST_CMDLINE=maxcpus=2 # CPU limit
make disk       # fluxdisk.img, a 256 MiB FAT32 data disk (DISK_MB=..., DISKFS=ext2)
make floppy     # fluxfloppy.img, a 1.44 MB FAT12 floppy for drive A:
make run        # boots the install CD in QEMU with the disk, floppy (if any) and an NE2000
make run-kernel # boots the kernel directly, the whole system in memory (text console)
make run-disk   # boots fluxdisk.img alone, after installing fluxOS onto it
make run-q35    # q35 machine: disk and CD on SATA, USB keyboard and tablet on xHCI
make run-usb    # disk as a USB stick on EHCI, keyboard and mouse on UHCI
make clean      # removes build/ and the ISO; the disk images are kept
make sysroot    # build/sysroot: fluxOS's headers, libc.a, libGL.a, crt0.o for a cross compiler
make toolchain  # binutils and GCC (C and C++) for the i686-fluxos target, in build/toolchain/cross
make ports      # GNU make and binutils built for fluxOS, for the system image
make toolchain-native # GCC built to run on fluxOS (the Canadian cross), for the system image
```

`make toolchain` builds a GCC that targets fluxOS (`i686-fluxos-gcc` and
`-g++`, with `as`, `ld`, `ar`, `libgcc` and `libstdc++`): it fetches binutils
2.42 and GCC 13.2.0 from gnu.org (checked against fixed SHA-256 sums), applies
the small target patches in `tools/toolchain/patches/`, and installs into
`build/toolchain/cross`. It needs `curl`, `patch`, a host C and C++ compiler
and about twenty minutes. With it, `i686-fluxos-gcc hello.c` makes a program
for fluxOS without the Makefile's flags (linked with `/usr/lib/fluxos.ld`,
the system's linker script), and `make BUILD=build-cross
CC=.../i686-fluxos-gcc LD=.../i686-fluxos-ld AR=.../i686-fluxos-ar` builds
the tree with it. `make ports` then builds GNU make and binutils for fluxOS,
and `make toolchain-native` GCC itself, configured `--host=i686-fluxos` (its
C++ parts against that `libstdc++`); the next `make` puts them on the system
image with the sysroot as `/usr/include` and `/usr/lib`. The whole of
`self-hosting-roadmap.md` describes the way from there to a fluxOS that
builds itself.

The TLS host tests additionally need OpenSSL and the Python `cryptography`
package. They generate temporary CA/server certificates and use loopback-only
servers, including tampered signature/transcript rejection checks.

The QEMU targets emulate a two-core Core 2 Duo (`QEMU_CPU=-cpu core2duo,-lm`,
`QEMU_SMP=2`; `make run QEMU_SMP=1` for one core). `make run
QEMU_NET="-nic user,model=ne2k_pci -device intel-hda -device hda-duplex"` adds
HD Audio. To boot real hardware or another VM, write
`fluxOS.iso` to a USB stick as a raw image (**DD Image mode** in Rufus), or
attach it as a CD-ROM, and boot in **legacy BIOS** mode, then install
(below). The ISO does not include a UEFI bootloader. Writing the raw image
erases the USB stick and preserves the bundled GRUB core and matching modules.
Rufus's ISO Image mode can substitute an incompatible GRUB core. The kernel
uses one contiguous ELF load segment to avoid GRUB relocator free-list
corruption between adjacent segments on some BIOS memory layouts.
On the installed disk the default entry starts the desktop at 1024x768; **fluxOS (text console)**
boots to the shell. In QEMU the mouse needs no grabbing; elsewhere click into
the VM window.

For the 3dfx driver and accelerated GL, give the machine a Voodoo Banshee or
Voodoo3 (QEMU has none, so GL uses the software renderer there; 86Box emulates
both, but no processor with SSE, so fluxOS no longer starts in it). fluxOS still starts in the BIOS mode: switch drivers in Display
Properties, or add `video=1024x768x16@85` to the kernel command line.

## Installing to a hard disk

The CD starts **fluxOS Setup**, as a Windows CD does: a full-screen wizard
that lists its steps down the left (Welcome, Choose a disk, Name this
computer, Ready to install, Installing, Finishing), installs onto the disk you
choose with a progress bar, then restarts into the installed system. **Disk
Manager...** on the Choose a Disk page (and after a failed install) opens Disk
Manager, to see what is on the disks, unmount or explore their volumes and
copy files off them before one is erased. **Quit** leaves Setup by restarting
or shutting down, and **Shift+F10** opens a command prompt. Without a framebuffer, or with the menu's **Install fluxOS (text
mode)**, the same questions come on the console; from that console (or the
command prompt) `fluxinstall /dev/hda` installs directly (it asks you to type
"yes", `-y` skips that). `fluxinstall -e /src /dev/hda` adds a second
partition, ext2, for a source tree (or `-e /home`): the system partition gets
half the disk up to 4 GiB (`-r MIB` says otherwise) and the ext2 one the rest,
mounted on `/src` at start-up from `/etc/fstab`.

Setup writes an MBR with one bootable FAT32 partition at 1 MiB, installs GRUB,
formats the partition, copies the kernel and GRUB's files onto it and unpacks
the system (`boot/system.tar` on the CD). **This erases the whole disk**, which
must be at least 34 MiB. Once a disk has fluxOS, the CD's menu starts the hard
disk by default, so a CD left in the drive does no harm. In QEMU, install with
`make run`, then quit and `make run-disk`. The installed system runs from disk,
so anything saved outside `/tmp` stays. Its boot menu's **safe mode** starts on
one processor in the BIOS display mode, without the Radeon, Intel and Voodoo drivers;
if the disk itself cannot be used, start Setup from the CD again (Shift+F10
gives a shell).

## Things to try

On the desktop: browse folders on it, drag windows, open terminals, try
**Run...** with `ls -l /bin`, open **Wave** (68k.news, wiby.me and
textfiles.com suit simple browsers; or type `example.com`), and **Shut Down...**.
In a terminal:

```sh
fetch                          # system summary
spin A 5 & spin B 5 & top      # CPU-bound jobs sharing the CPU; ^C quits top
threadtest                     # POSIX threads: every line should say ok
make --version; as --version   # GNU make and binutils, built for fluxOS
echo 'int main(){puts("hi");}' > h.c; gcc -include stdio.h h.c && ./a.out   # GCC on fluxOS
tar -czf src.tgz /usr/include; tar -tzf src.tgz | head; sed -n '1,3p' h.c   # the build tools
cat ~/README.txt; ./demo.sh    # tour and a scripting demo
echo {1..3} $((6*7)) ~         # brace, arithmetic and tilde expansion
lsblk; df -h; mount            # disks, CD-ROM and mounted filesystems
echo hi > ~/note.txt; sync    # saved on the disk: it survives reboots
ifconfig; ping -c 3 10.0.2.2   # the network card, and QEMU's gateway
host example.com; wget -O - https://example.com/    # DNS, HTTP, TLS 1.3
nc -l 7777 &  echo hi | nc localhost 7777           # a TCP server and client
lsusb -v                       # USB devices (make run-usb or run-q35)
cp /mnt/fd0/GAME.EXE ~; ./GAME.EXE    # a DOS program (make floppy first)
beep -m "T160 L8 CDEGEC4"      # the PC speaker
play /usr/share/sounds/startup.wav; mixer pcm 60    # the Sound Blaster
power -v; power bright +       # a laptop's battery, lid and backlight
power sleep                    # suspend to RAM; the power button wakes it
dmesg | tail; poweroff
```

The installed system lives in `fluxdisk.img`, and files on `/mnt/fd0` in
`fluxfloppy.img`. On a Linux host inspect them with `mdir -i fluxdisk.img ::`
or `fsck.fat -n fluxdisk.img`, and add DOS programs with `mcopy -i
fluxfloppy.img GAME.EXE ::`.

## Layout

```
Makefile, linker.ld        build and kernel link script
grub/grub.cfg              boot menu
include/flux/              kernel <-> user ABI (syscall numbers, structs, errno)
lib/                       string/printf routines and the 8x16 font, shared by kernel and libc
tools/                     mkfont.py (fonts), oxygen-icons.py (icons), mksounds.py (sounds),
                           mklogo.py (the logo for the startup and shutdown screens)
kernel/core/               boot.S, entry.S, kmain, cpu/fpu/sse, gdt/idt, smp, timer,
                           pmm/vmm/heap, proc/exec/syscall, pci, tty/serial/kprintf
kernel/acpi/               acpi (tables, namespace, power-off), osl (uACPI's kernel
                           services), ec, power (battery, lid, buttons, backlight),
                           sleep and wakeup.S (suspend to RAM)
kernel/fs/                 vfs/file/initrd, blkdev, automount, fat32/iso9660
kernel/storage/            ata, ahci, floppy
kernel/usb/                usb core, hid/hub/msc, uhci/ohci/ehci/xhci
kernel/input/              input, keyboard, mouse, touchpad
kernel/net/                net/tcp/socket, ne2000, forcedeth/mii, e1000, e1000e
kernel/sound/              pcspkr, sb16/opl/isadma, hda
kernel/video/              fb/vga, the startup screen (splash.c), voodoo, radeon*, atom, intel*,
                           modes (EDID mode lists)
user/libc, user/include    the C library (math.c is the x87 math library; http.c, hpack.c and
                           inflate.c are the HTTP/1.1 and HTTP/2 client and Deflate) and headers
user/gl/                   the OpenGL library and its backends
user/fsh/                  the shell
user/desktop/shell/        the desktop: compositor and window manager (wm.c), its side of
                           the window protocol (server.c, wmproto.h), sidebar, sound, the
                           wallpaper, its dialogs (sysdlg.c), clock, setup's screen; the
                           programs' side (client.c); and what both share (libdesktop.a):
                           drawing (gfx.c), widgets, icons, dialogs (apps.c), starting
                           programs (launch.c), the loop (loop.c), the themes (theme.c;
                           fonts in font_sans.c and font_archivo.c)
user/desktop/apps/         the programs: files, edit, calc, player, paint, term, taskmgr,
                           diskmgr, display, mouse, datetime, wireless, archiver, gears,
                           mines, imageview (files.c is the desktop's folder too)
user/desktop/progs/        each program's main(): /bin/NAME
user/desktop/image/        PNG/JPEG/GIF/WebP/ICO/SVG decoding (image.c picks the decoder)
user/desktop/wave/         the Wave browser: window (browser.c), HTML (html.c), CSS
                           (css.c), layout (layout.c), shadow composition (shadow.c),
                           fonts (webfont.c), URLs (webutil.c), request policy
                           (webpolicy.c), cookies, storage, <canvas> (canvas.c), and
                           <audio> (webaudio.c, the player; codecs.c, the decoders)
user/desktop/wave/js/      JavaScript (js.c, jslib*.c, jsbinary.c, jsre.c, jsgc.c, jsdom.c)
third_party/mbedtls/        upstream verification library subset and its fluxOS config
third_party/uacpi/         uACPI 6.1.1, the AML interpreter (unmodified)
third_party/minimp3/       minimp3, Wave's MP3 decoder (unmodified, public domain)
third_party/stb_vorbis/    stb_vorbis 1.22, Wave's Ogg Vorbis decoder (unmodified, public domain)
user/dos/                  the DOS emulator (CPU, DPMI, BIOS, video, files, sound)
user/bin/                  one program per file (fluxinstall.c is the installer)
rootfs/                    the installed system's files (/etc, /home/flux, icons,
                           the browser's fonts and home page in /usr/share/www)
setupfs/                   the CD's setup system's own files (/etc/setup, its motd)
grub/                      the CD's boot menu (install.cfg) and the installed disk's
                           (grub.cfg)
firmware/grub/             GRUB's boot images and modules, checked in (see its README)
```

## Design notes

**Memory map.** Every address space shares the kernel half.

| Range                       | Use                                              |
|-----------------------------|--------------------------------------------------|
| `0x00000000 - 0x3FFFFFFF`   | kernel; identity map of the first GiB of RAM     |
| `0x40000000 - ...`          | user program image, then the `sbrk` heap         |
| `0xB0000000 - ...`          | the framebuffer, in the process that mapped it   |
| `0xBE000000 - 0xBFFFFFFF`   | user stack (up to 32 MiB, grown on page faults)  |
| `0xC0000000 - ...`          | kernel mappings of device memory (framebuffer)   |
| `0xFEC00000 - 0xFEFFFFFF`   | temporary mappings of high user pages (kmap)     |
| `0xFF000000 - 0xFFFFFFFF`   | late mappings (ACPI's firmware memory)           |

The kernel's own memory (page tables, heap, stacks, DMA buffers) comes from
the first GiB, which it reaches through the identity map. User pages come
from the RAM above it first; the kernel reaches those, to clear them, copy
them for `fork` or load a program into them, through two pages per
processor mapped for a moment with interrupts off. Without PAE, RAM past
4 GiB is out of reach.

**Scheduling.** Each trap pushes a `struct trapframe` on the process's 16 KiB
kernel stack. The kernel is not preemptible (interrupt gates keep IF clear): a
process gives up the CPU only in `schedule()`, when it blocks or when the timer
set `need_resched` and the trap is about to return to user mode.

**Disk I/O.** Filesystem and block-device work may sleep mid-operation, so it
is serialised by one FIFO "VFS lock", taken per call (per 64 KiB chunk for
`read`/`write`). Terminals and pipes never take it. FAT, directory and FSInfo
updates are dirty sectors in the write-back cache. Reads use a separate bounded
cache of clean 4 KiB blocks, keyed by physical disk and sector so partition and
raw-device views agree. Cache pages are allocated individually on demand,
leaving a reserve of low physical memory for kernel allocations. The physical
allocator reclaims clean pages when low memory runs short, without waiting for
the VFS lock or doing disk I/O. Cache metadata remains resident (about 400 KiB).
Sequential tracking belongs to each open file description, shared by dup/fork,
so interleaved file reads retain read-ahead. Sequential small reads fetch the
remainder of a block; random reads fetch only the requested sectors. Writes
update resident clean copies without filling the read cache. A run of contiguous clusters is still
written as one DMA command. A sync writes dirty sectors in LBA order, merging
neighbours, then issues one FLUSH CACHE.

**Graphics.** GRUB sets the video mode. The kernel maps the framebuffer above
3 GiB with 4 MiB pages; `fbmap` maps it into a process as device memory, which
`fork` shares and `exit` never frees. That mapping is write-combining: the
kernel reprograms PAT entry 1 (normally write-through) to write-combining, so
PWT alone selects it, and the desktop's streaming stores reach the card in
bursts. A mode change in the 3dfx or Radeon driver
points `fb` at the new screen and the desktop rebuilds its back buffer
(`wm_reconfigure`).

**Sound.** The Sound Blaster 16 is always fed signed 16-bit (or unsigned 8-bit
without a 16-bit DMA channel): `write()` converts into a ring, and the interrupt
handler copies a half-buffer into the DMA buffer, padding with silence. The DSP
pauses once both halves are silent. The PC speaker's queue advances in the
timer interrupt.

**The FPU and SSE.** CR0.TS is set whenever the CPU's process isn't the one
whose registers the FPU holds. Its first x87, MMX or SSE instruction raises
#NM, and the handler swaps the 512-byte FXSAVE state; a process starts from a
clean image (default control words, every register zero). CR4.OSXMMEXCPT turns
unmasked SIMD exceptions into SIGFPE. The kernel's C code stays off these
registers (`-mgeneral-regs-only`); `kernel/core/sse.S` borrows four XMM registers
for page and framebuffer moves with interrupts off, saving them on its stack
and putting CR0.TS back, so whichever process owns the FPU never notices.
User space does float arithmetic in SSE and doubles on the x87; code using
MMX (which shares the x87's registers) runs EMMS before returning.

**The desktop** composes the screen in a back buffer: changes record damaged
rectangles, repainted bottom to top once per pass of the event loop and copied
to the framebuffer (with streaming stores at 32 bpp, and four pixels at a time
in MMX registers at 15/16 bpp). Each window has its own buffer. One `poll` covers
`/dev/input` and every terminal's pty master.

**Networking.** Interrupt handlers pass frames up the stack and TCP timers run
from the timer interrupt. That is safe because interrupts only arrive in user
mode or the halted idle loop. A process waiting on a socket sleeps on it.
Loopback packets are delivered on the next tick. Out-of-order TCP segments are
dropped and resent.

**Pictures.** The JPEG decoder uses a 32-bit bit buffer with a 9-bit Huffman
lookahead and an integer IDCT; progressive files keep all coefficients until
the last scan, chroma is upsampled with a triangle filter and converted to RGB
four pixels at a time (MMX), and thumbnails use only each block's DC
coefficient. PNG's Up filter adds 8 bytes at a time and Average and Paeth work
a whole pixel per step; the JPEG encoder's DCT is PMADDWD multiply-adds. The wallpaper is drawn once into a
screen-sized surface so repaints are copies. GIF is decoded with LZW into a
palette image. SVG shapes are flattened to polygons and filled by a scanline
rasteriser with four sub-scanlines per pixel.

The desktop, and each program that shows pictures, starts one decoder helper before
opening its windows. Browser images (including data URLs and favicons), image-viewer
and Paint opens, wallpaper previews and full wallpaper rendering run there. The program keeps
handling input while the helper works, transferring results in bounded chunks
through credit-controlled pipes. Closing a window or navigating cancels its
requests; a failed helper is restarted for subsequent work. JavaScript DOM
discovery is batched once per event-loop turn, while explicit geometry queries
still flush pending changes immediately. JavaScript execution, layout and
subsequent GIF animation frames run in Wave's own process.

**The browser** keeps a page in arenas: document tree and style sheets, computed
styles (rebuilt when a style sheet arrives), and layout (rebuilt when a picture
arrives). Layout produces a flat list of rectangles, text runs, images and
controls in page coordinates, which painting clips to the view. CSS numbers are
fixed point with two decimals.

**System calls.** `int 0x80`, number in `eax`, arguments in `ebx`, `ecx`, `edx`,
`esi`, `edi`; failures return `-errno`. Every user pointer is checked against the
user range and page tables first.

## Work in progress

- **Building fluxOS on fluxOS** (`self-hosting-roadmap.md`): GNU make,
  binutils and GCC itself run on fluxOS, and the Makefile's tools exist
  (`tar`, `gzip`, `sed`, `diff`, `patch`, `find`, `install`...) with `fsh`
  as `/bin/sh`, the build tree has a filesystem with links and one-second
  times (ext2), and the ISO is made by fluxOS's own `mkiso` from GRUB's
  checked-in images; what is left is the bootstrap itself. The native compiler is tested on the kernel by `make
  test-gcc-qemu`; the tools and the `sh` mode on the host against the GNU
  originals and dash. For QEMU S3 tests, boot through GRUB (ISO or installed
  disk), not `make run-kernel`: QEMU's direct kernel loader reloads kernel
  memory on wake, destroying the saved resume context.
- **The Radeon driver on Llano APUs (HD 6520G and kin) is partial:** page
  flipping, the hardware pointer, and native modes only for a laptop panel
  behind a DisplayPort bridge (TRAVIS, as on the HP Pavilion g6); eDP panels and
  the VGA, HDMI and DisplayPort connectors keep the boot loader's mode.
  Modes change only the surface, viewport and scaler; the full DisplayPort
  mode set (PLL, encoder, bridge, link training over AUX), ported from
  Linux, blanks the panel on the HP Pavilion g6 and is only run with
  `radeon.dp_modeset=1`. `dmesg | grep radeon` shows each step, and
  `radeon.modeset=0` turns native modes off. A native mode is set again
  after waking from sleep. The 3D engine's bring-up (Evergreen set-up,
  Sumo microcode, blits, GART, restarted after waking) is new and not yet
  tried on hardware; the boot self-test decides whether the desktop uses
  it. OpenGL needs the GL library's shaders and the kernel's command
  buffer check redone for Evergreen's instruction set and registers.
- **The Intel HD Graphics driver (Haswell) is partial:** page flipping, the
  hardware pointer, blitter copies and 3D (working on an HD 4600: 3D Gears
  draws on it; the boot self-test still decides, and a hang turns it off);
  native modes only on the VGA output
  (DisplayPort and HDMI keep the boot loader's mode), and suspend to RAM
  only in the BIOS mode. `dmesg | grep intel` shows the
  planes, stolen memory and watermarks it found. Native modes need the DDI
  ports, their PLLs and DisplayPort link training.

## Limitations

The kernel runs on every processor at once, but the I/O APIC is not used: every
interrupt reaches the first processor through the PICs. From the CD, the root
filesystem lives in RAM and only what you write to a mounted FAT volume persists;
an installed system runs from its FAT32 partition, which has no symbolic or hard
links and only the exec bit of the permissions (an ext2 partition, `fluxinstall -e`,
has both; permissions are not enforced anywhere). There is one user,
and there are no shared libraries (programs are linked statically).
The FPU state is saved with FXSAVE, so AVX and later cannot be used.

- **IDE:** DMA needs a legacy-mode PCI controller with bus mastering and a
  drive the firmware put in a DMA mode; anything else uses PIO.
- **Suspend to RAM:** not with the Voodoo driver, nor with the Radeon driver's
  own modes or 3D engine on the HD 4000s (they cannot set the card up again:
  `radeon.modeset=0 radeon.accel=0`; the Llano driver sets its panel mode
  again after the video BIOS), nor with a USB disk mounted
  (USB devices are let go of and found again). The screen comes back through
  the video ROM and the VESA mode set on real hardware (`s3_nobios`,
  `s3_nomode` turn them off) or the virtual BIOS under a hypervisor. No
  hibernation (S4), no processor frequency scaling, and touchpads other than
  Synaptics have no scrolling.
- **SATA:** AHCI only, no NCQ, port multipliers or hot plugging; a controller
  above 4 GiB can't be reached.
- **USB:** isochronous transfers only on UHCI, OHCI and xHCI (a USB audio
  device on an EHCI port works where the chipset hands full-speed devices
  to a companion controller, not behind an EHCI rate-matching hub); no UAS,
  only LUN 0 of mass storage, no
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
- **Desktop:** windows resize by an outline; dialogs and the calculator can't be
  resized; terminals show only characters code page 437 has (others as `?`) and do not
  rewrap lines when resized; the editor shows bytes as CP437 (no UTF-8).
- **DOS programs:** text modes plus VGA 13h/mode X only (other graphics modes
  stop the program); no Sound Blaster recording, ADPCM, direct DAC or MPU-401;
  no mouse, EMS/XMS, batch files or INT 13h (self-booting and copy-protected
  software won't run); text mode reports key presses without releases; every
  instruction is interpreted, so delay-loop timing runs fast; only the
  read-only DOS attribute is kept.
- **Audio:** the Sound Blaster 16 (DSP 4), HD Audio and USB audio are output
  only, with no MIDI; latency is about 80 ms (120 ms on the Sound Blaster), and a
  stream that starts while others play joins within that. HD Audio has no S/PDIF or HDMI, no microphone or
  line-in jack sensing, and surround jacks repeat the front pair. USB audio
  is Class 1.0 only (no USB Audio 2.0 devices), one device at a time, no
  recording, and the first feature unit's controls are the volume.
- **Network:** IPv4 only, one card, no IP fragments, no TCP congestion control
  or out-of-order buffering. forcedeth runs at 10/100 Mbit/s without offload,
  VLANs, wake-on-LAN or MSI. HTTPS is TLS 1.3 (TLS_AES_128_GCM_SHA256; X25519, or
  P-256/P-384 after a HelloRetryRequest) or TLS 1.2 (ECDHE over X25519, P-256 or P-384 with
  AES-128/256-GCM or ChaCha20-Poly1305; no CBC suites, no RSA key exchange, no resumption);
  certificate chains and handshake signatures are verified using
  Mbed TLS and a bundled CA trust store. The trust bundle needs periodic updates.
- **Browser:** Callable proxies and local-scope direct `eval` are implemented; complete
  proxy invariants and strict-eval declaration edge cases still need work. Regular
  expressions have no `\q{}` string sets or Unicode
  case folding beyond simple folding, so some modern web apps won't run. Layout is an
  approximation: the scroll bars of boxes lie over their contents instead of making room
  (text can end up under them), and boxes with `overflow` that are table cells, or `<body>` and
  `<html>`, do not clip or scroll; text selection and `getBoundingClientRect()` account for nested box scrolling,
  while geometry for some positioned boxes is approximate; background `space` and `round`
  still repeat, and `local` attachments do not track inner scrolling; a positioned inline (a `<span>`)
  does not get a layer of its own; opacity groups composite overlapping children together,
  with bounded offscreen buffers and incomplete filter/backdrop/iframe interactions;
  `sticky` works against the window only. CSS 2D translate, scale, rotate, skew and matrix transforms
  support origins, nesting, painting, hit testing and visual geometry. 3D transform lists
  retain depth through composition: `perspective()`, `translateZ()`, `scaleZ()`, arbitrary-axis
  rotations and `matrix3d()` project flattened planes, including raster painting, hit testing
  and clipping at the viewer plane. Parent `perspective` and `perspective-origin` project
  direct transformed children. Shared `preserve-3d` scenes, depth sorting and intersecting
  planes remain unimplemented; `CSS.supports()` reports `transform-style` as unsupported. CSS transitions and `@keyframes` animations run on colors, lengths, opacity,
  transforms with matching functions, shadows and the box properties (not on pseudo-elements);
  `display` and `overlay` transition with `transition-behavior: allow-discrete` (the box stays shown,
  and in the top layer, until the transition ends), `@starting-style` (at the top level or nested)
  gives what an element appearing, or coming out of `display: none`, transitions from, and nothing
  else transitions out of `display: none`; each frame restyles only what moves. Transform scratch rendering is bounded to four million
  source pixels per item. `structuredClone` copies buffers, typed views and blobs,
  preserving shared references; transferring ArrayBuffers detaches their source views.
  SharedArrayBuffer uses native shared segments and supports atomic integer operations and
  worker wait/notify, with a bounded process-lifetime pool (see `browser-compatibility.md`). `CSS.escape`
  handles identifier serialization, including Unicode and selector round trips.
  Try `file:///usr/share/www/transforms.html` for an interactive example.
  Flexbox has grow/shrink sizing, wrapping,
  ordering, alignment and rows/columns, but needs wider conformance testing;
  grids support row and column subgrids with inherited track sizes and gaps, nested
  subgrids, descendant contributions to auto tracks, and inherited names for explicitly
  placed spans. Integer line-name repeats are supported; auto-fill line-name repeats, complete named-line handling for
  auto-placed spans, named spans and complete named-line behavior for auto-repeat remain
  incomplete. Container size queries are implemented. Shadow DOM still needs wider composed-event and slot
  conformance testing; adopted sheets do not register downloadable font faces.
  Fetch resolves at headers and delivers body chunks incrementally; EventSource
  parses event streams and reconnects. Each page uses up to four request workers.
  Fetch byte streams support BYOB readers, and window fetches pause helper-pipe delivery
  when their queues fill; worker transport and the wget fallback still need backpressure.
  Animated WebP supports frames, blending, disposal and loops. SVG supports local clip paths
  and basic luminance/alpha masks; filters and patterns remain missing. Dedicated workers
  are implemented. IndexedDB keeps each database in memory and serializes it at commit,
  but small changes append checksummed replacement ranges instead of rewriting the entire
  database file; very large databases and concurrent page transactions need more work. Windows reach each other only through `postMessage`, `closed`, `close()` and
  `location`: a same-origin popup's `document` cannot be touched. BroadcastChannel
  delivers between same-origin page engines in a Wave process.
  Web Crypto encryption, asymmetric keys and key derivation are missing. There are
  downloadable TrueType/OpenType, WOFF and WOFF2 fonts, but no
  complete OpenType shaping or bidirectional text support (basic Arabic joining,
  lam-alef ligatures and Hebrew/Arabic visual runs are implemented); the built-in fonts cover Latin
  Extended, Greek, Cyrillic and common punctuation, Noto fonts most other scripts and CJK, and
  color emoji are single characters (skin tones, ZWJ families and flags show their parts). Connections are kept per
  window, not shared between them, and the cache is for the browser only (wget
  does not use it).
- **Graphics:** only the 3dfx and Radeon drivers set modes (others keep the BIOS
  mode, changeable only on an installed system at the next start); no 8-bit
  palette, interlaced or doublescan modes; the 3dfx engine renders 16-bit color
  and is read back over the bus each frame. The GL library has no
  accumulation buffer, colour index mode, logic ops, stipples, evaluators,
  1D textures, selection or feedback; `glDrawPixels` and `glCopyPixels`
  take colours only; the 3D engines draw level 0 of a mipmap and have no
  stencil buffer or second texture unit (a context needing those goes on in
  software, without its depths from the Radeon or Intel, so the frame where
  it moves may draw wrongly); the Voodoo does not texture, and the Radeon
  draws decal as replace and blend as modulate; lighting and fog are per
  vertex; polygons must be convex; only one process at a time can use the
  3D hardware.
- **Pictures:** JPEG arithmetic coding, 12-bit, lossless and CMYK, and
  interlaced PNGs are not shown; EXIF orientation and color profiles are
  ignored; pictures up to 4096x4096. A 1920x1080 photo takes a few seconds to
  decode on a slow machine, and the wallpaper list holds 40 pictures.

## Credits

The default wallpaper, `/usr/share/wallpapers/bamboo.jpg`, is a photograph by
kazuend on [Unsplash](https://unsplash.com), `mountains.jpg` one by Štefan
Štefančík, and `shore.jpg` one by Nattu Adnan, and `waves.jpg` one by Almas Salakhov, all used under the [Unsplash License](https://unsplash.com/license).
Thank you for sharing them.

The startup, shutdown and error sounds (`/usr/share/sounds/startup.wav`, `shutdown.wav` and
`error.wav`) are made from the Zero-G sample library.

The icons are from KDE's [Oxygen icon
theme](https://invent.kde.org/frameworks/oxygen-icons5) (LGPL-3.0-or-later), and the desktop's
text is rasterised from Liberation Sans, Serif and Mono and, in the Kinetik theme, from
[Archivo](https://fonts.google.com/specimen/Archivo) by The Archivo Project Authors (SIL Open
Font License 1.1; its license is in `/usr/share/doc/fonts/LICENSE.archivo`). Kinetik's look follows the
Kinetik studies for fluxOS.

Certificate and TLS handshake signature verification uses the bundled
[Mbed TLS 3.6.7](https://github.com/Mbed-TLS/mbedtls/tree/mbedtls-3.6.7)
verification subset (Apache-2.0 OR GPL-2.0-or-later; see
`third_party/mbedtls/LICENSE` and `README.flux`). Cookie site/domain rules use
[the Public Suffix List](https://publicsuffix.org/) (Mozilla Public License 2.0).

Downloaded font parsing and rasterization use [FreeType 2.14.3](https://freetype.org/)
under the FreeType License, and WOFF2 decoding uses [Brotli 1.2.1](https://github.com/google/brotli)
under the MIT license. Source subsets, licenses, and port notes are in `third_party/`.

The firmware's ACPI tables and AML run on [uACPI 6.1.1](https://github.com/uACPI/uACPI) by
Daniil Tatianin and contributors, under the MIT license (`third_party/uacpi/LICENSE`).

The current browser compatibility implementation and remaining limits are tracked in
[browser-compatibility.md](browser-compatibility.md).
