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
> **It was built entirely with Claude.** Every line of fluxOS, from the
> bootloader glue and kernel to the TCP/IP stack, the desktop and the web
> browser, was written by [Claude](https://claude.ai), Anthropic's AI model,
> working through [Claude Code](https://claude.com/claude-code) from
> instructions given in conversation. No code was written by hand. It has
> been tested by running it in QEMU, not reviewed or audited by people, so
> read it with that in mind.

fluxOS is a small 32-bit x86 Unix-like operating system with preemptive
multitasking, ring-3 user processes, paging, a root filesystem in RAM (from
the CD) or on the installed hard disk, IDE
disk, CD-ROM and floppy drivers with FAT12/16/32 and ISO9660 support, an
NE2000 network driver with a TCP/IP stack, a zsh-flavoured shell called
**fsh**, a Windows 95 style graphical desktop with a web browser, a 3dfx
Voodoo3 display driver with a small OpenGL library, PC speaker and Sound
Blaster 16 sound, and a compatibility layer that runs DOS programs (with
their sound) in the terminal. It boots with GRUB
(Multiboot) from an ISO image, straight into the desktop.

```
flux@fluxOS ~ % spin A 3 & spin B 3 &
[1] 5
[2] 6
flux@fluxOS ~ % ls /bin | grep -c .
43
flux@fluxOS ~ % for f in /etc/*; do echo "$f: $(wc -l < $f) lines"; done
```

## Features

**Kernel**
- Multiboot entry, GDT with kernel/user segments and a TSS, and a 256-entry
  IDT. CPU exceptions, the remapped 8259 PIC IRQs and an `int 0x80` system call
  gate all go through one trap dispatcher.
- Preemptive round-robin scheduler driven by the PIT at 100 Hz (50 ms time
  slices), with sleep and wait queues and an idle task.
- Processes: `fork` (full address-space copy), `execve` (static ELF32 and `#!`
  scripts), `exit`/`waitpid` with zombies, reparenting of orphans to init,
  process groups and a foreground process group on the tty.
- Lazy x87 FPU switching: a process's first FPU instruction after a task
  switch traps (#NM), and the kernel saves the previous owner's FPU
  registers and loads its own (FNSAVE/FRSTOR), so only processes that use
  floating point pay for it. `fork` copies the FPU state.
- Signals: `kill`, `SIGINT`/`SIGQUIT` from ^C/^\, `SIGPIPE`, and fault signals
  (`SIGSEGV`, `SIGFPE`, `SIGILL`). Each signal either terminates the process or
  is ignored.
- Memory management:
  - A bitmap physical frame allocator built from the GRUB memory map.
  - Paging: the first GiB is identity-mapped with 4 MiB global pages, and each
    process has its own 4 KiB-page user half.
  - A user stack that grows on demand, `sbrk` heaps, and a first-fit kernel
    heap that merges free blocks.
- VFS over an in-memory filesystem, unpacked at boot from a ustar initrd that
  GRUB loads as a module. It has directories, files, `/dev/{tty,console,null,
  zero,random,urandom}`, pipes, `dup2`, `rename`, `chmod` and `getdents`.
  Started from an installed disk (`root=UUID=` on the kernel command line),
  the disk's FAT32 partition is the root instead, and only `/dev`, `/tmp` and
  `/mnt` stay in memory.
- Storage:
  - An IDE driver for ATA hard disks (LBA28/LBA48, read/write) and ATAPI
    CD/DVD drives (packet commands, media detection). Data moves by
    bus-master DMA, up to 64 KiB per command, found through PCI. It is
    interrupt driven: a process waiting on the disk sleeps until IRQ14/15,
    so other processes keep running. If a DMA command fails, the driver
    resets the channel and switches that drive to PIO, so no data is lost.
  - IDE kernel options: `ata.nodma` forces PIO, and `ata.poll` polls instead
    of using interrupts, for hardware whose IDE interrupts don't arrive.
  - A floppy driver for the PC floppy controller (82077AA and compatibles):
    drives `/dev/fd0` and `/dev/fd1`, found through the CMOS, with ISA DMA
    and IRQ 6. It reads whole tracks into a cache (a floppy turns slowly),
    finds the disk's format (1.44M, 720K, 1.2M, 360K, or anything a FAT boot
    sector describes) by trying data rates, notices write-protected and
    changed disks, and turns the motor off after 3 seconds of quiet. A disk
    in the drive at boot is mounted on `/mnt/fd0`.
  - Block devices `/dev/hda`..`/dev/hdd`, with MBR partitions as `/dev/hda1`
    and so on, behind a write-back sector cache. Metadata writes stay in
    memory and are written out in sorted, merged runs by the `kflushd`
    kernel thread every 5 seconds, by `sync`/`fsync`, on `umount`, and
    before `reboot`/`poweroff`.
- Filesystems, mounted into the same VFS tree:
  - **FAT12, FAT16 and FAT32**, read/write: VFAT long file names with `~N`
    short aliases, case-insensitive lookup, directories that grow (except a
    FAT12/16 root, which has a fixed size), all FAT copies kept in sync, and
    FSInfo free counts. Programs can run straight from FAT. `mkfs.fat`
    formats floppies as FAT12 in the standard DOS layouts.
  - **ISO9660**, read-only: Rock Ridge names, modes and timestamps, on CDs or
    on 512-byte-sector disks.
  - Volumes are mounted at boot on `/mnt/cdrom`, `/mnt/hdX` and `/mnt/fd0`,
    and with `mount`/`umount`, which refuses while files are in use.
  - **Automounting:** the `kmediad` thread looks at every CD-ROM and floppy
    drive every 2 seconds. A disc or disk that is put in is mounted (a CD on
    `/mnt/cdrom`, or the next free `/mnt/cdromN` if another is there; a
    floppy on `/mnt/fdN`); one that is taken out or swapped is unmounted, at
    once if nothing is using it and otherwise as soon as it is free (a shell
    whose directory is inside it is enough to keep it). A medium you unmount
    by hand is left alone until it is taken out, so `umount /mnt/cdrom` and
    the eject button work in that order. A disc it cannot mount (an audio
    CD, a blank floppy) is tried once and noted in `dmesg`. A CD drive is
    asked with TEST UNIT READY and its sense data (no medium, new medium,
    still spinning up); a floppy drive's disk change line is read, and an
    empty drive's head is stepped out and back to see whether a disk went
    in. `automount=off` on the kernel command line switches it off, and
    `floppy.nopoll` stops the floppy polling alone (on a real drive each
    look at an empty one clicks the head).
- Console: a text console with an ANSI/VT100 escape interpreter, drawn on the
  boot framebuffer with an 8x16 font of the PC's code page 437 (or in VGA text
  mode without one), PS/2 keyboard (arrows, function keys and modifiers map to
  xterm escape sequences), and a mirrored COM1 serial console. The tty line discipline supports canonical and raw modes,
  echo, erase/kill/word-erase and EOF.
- Graphics and input: GRUB sets a 1024x768x32 mode, which the kernel maps and
  lends to one process at a time (`fbinfo`, `fbmap`); the console takes the
  screen back when that process exits. A PS/2 mouse driver with IntelliMouse
  wheel support, which uses the VMware/QEMU "vmmouse" interface for absolute
  positioning (and the wheel) when it is there, and `/dev/input`, which
  delivers keyboard and mouse events to the desktop.
- Display drivers: the standard driver keeps the mode the VESA BIOS set at
  startup (the default), and reports the adapter's name and memory from the
  BIOS. A driver for the **3dfx Voodoo Banshee, Voodoo3 and Voodoo4/5**
  (PCI vendor `121a`) sizes video memory, reads the monitor's EDID over DDC,
  and can set modes itself, from 640x480 to 1600x1200 at 16 or 32 bits per
  pixel and 56 to 85 Hz (VESA DMT timings, programmed into the CRTC, the
  Banshee extension registers and the video PLL). It saves the BIOS mode's
  registers first, so that mode can be restored without a reboot, and it
  lends the 3D engine to one process at a time by mapping its registers and
  video memory into it. `display()` (the `SYS_display` call) reports the
  adapter, lists modes, sets them and maps the 3D engine. Kernel options:
  `video=1024x768x16@85` starts in a mode of the 3dfx driver, and
  `voodoo=off` leaves the card to the standard driver.
- Sound:
  - The PC speaker (`/dev/speaker`): PIT channel 2 through port 61h. Writes
    queue tones (`struct spkr_tone`) that play in the background, timed by
    the scheduler tick; `SPKR_SET` holds a frequency until changed. The
    console's bell (^G) beeps on it.
  - The **Sound Blaster 16** (DSP 4.x), found by resetting its DSP at 220h,
    240h, 260h or 280h, with the IRQ and DMA channels its mixer reports.
    `/dev/dsp` plays 8-bit or 16-bit, mono or stereo samples at 5000 to
    44100 Hz, in the style of OSS (`SNDCTL_DSP_SPEED`, `_CHANNELS`,
    `_SETFMT`, `_SYNC`, `_GETOSPACE`...): an auto-init DMA buffer in two
    halves of about 40 ms each, refilled from a 64 KiB ring at each
    interrupt, on the 16-bit DMA channel. `/dev/mixer` sets the master,
    wave, FM, CD, line, microphone and PC speaker volumes.
  - The OPL2/OPL3 FM synthesizer at 388h (`/dev/opl`, AdLib or the SB16's
    own): register writes in order, with the waits the chip needs; closing
    the device silences every voice.
  - `ioctl()` (the new `SYS_ioctl` call) controls the devices. Kernel
    options `sb16=off` and `opl=off` leave the cards alone.
- Pseudo-terminals (`openpty`) that share the console's line discipline, with
  a controlling terminal per process, hangup (`SIGHUP`) when the master side
  closes, and `poll` over terminals, pipes and devices.
- Kernel log (`dmesg`), CMOS clock, reboot and ACPI power-off (QEMU, Bochs,
  VirtualBox).
- Networking:
  - An NE2000 driver for the PCI RTL8029 (QEMU's `ne2k_pci`) and its clones,
    or an ISA card at 0x300 or 0x280 (QEMU's `ne2k_isa`). It receives into
    the card's ring buffer from its interrupt handler, recovers from ring
    overflows, and moves frames with 16-bit programmed I/O.
  - Ethernet and ARP (a 16-entry cache that holds the newest packet while it
    resolves), IPv4 with checksums but no fragments, ICMP echo replies and
    "port unreachable", UDP, and a loopback path for `127.0.0.0/8` and our own
    address.
  - TCP: the full connection state machine with listening sockets and an
    accept queue, an RTT estimate per connection (RFC 6298), retransmission
    with exponential backoff, fast retransmit after three duplicate ACKs,
    zero-window probes, `TIME_WAIT`, and resets for closed ports. The window
    each connection offers is capped so that bursts fit in the NE2000's
    receive ring.
  - A DHCP client in the kernel configures the interface at boot and renews
    its lease; `net.ip=10.0.2.15/24 net.gw=10.0.2.2 net.dns=10.0.2.3` on the
    kernel command line sets an address by hand instead.
  - BSD sockets (`socket`, `bind`, `connect`, `listen`, `accept`, `send`,
    `recv`, `sendto`, `recvfrom`, `shutdown`, `getsockname`, `getpeername`)
    for TCP, UDP and ICMP echo ("ping sockets", as in Linux). Sockets are
    file descriptors, so `read`, `write`, `poll` and `close` work on them.
  - The C library resolves host names from `/etc/hosts` and then by DNS,
    with the server DHCP gave us.

**fsh, the flux shell (zsh-flavoured)**
- zsh prompt escapes in `PROMPT`/`RPROMPT`: `%n %m %~ %# %? %F{color} %B
  %(?.ok.fail)`, and more.
- Line editor: live syntax highlighting (commands are green when they exist and
  red otherwise), grey history autosuggestions (→ accepts), zsh-style Tab
  completion menus, Up/Down history search by prefix, and emacs keys
  (^A ^E ^K ^U ^W ^Y ^L, Alt-b/f/d, Alt-.).
- Continuation prompts: `if>`, `for>`, `dquote>`, `pipe>`, `cmdand>` and more.
- Grammar: pipelines, `;` `&&` `||` `&` `!`, redirections
  (`< > >> 2> 2>&1 &> >&2`), `if/elif/else`, `while/until`, `for`, `{ }`,
  `( )`, functions, and `time`.
- Expansion: `$VAR ${VAR:-x} ${#VAR}`, `$(cmd)` and backquotes, `$((math))`,
  `~`, brace expansion `{a,b} {1..5}`, and globs `* ? [..]`. A glob that matches
  nothing is an error, as in zsh.
- zsh behaviours: AUTO_CD, unquoted parameters are not word-split,
  `fsh: command not found: x`, `fsh: segmentation fault  cmd`.
- Job control: `jobs`, `fg`, `wait`, `kill %n`, and done notifications.
- History expansion (`!! !$ !n !str`), aliases, `source`, `~/.fshrc`, and
  history saved in `~/.fsh_history`.

**The desktop** (`/bin/desktop`), started by init at boot
- Windows 95 look: a dark purple (`#3f175f`) desktop with icons, a taskbar
  along the bottom with a Start button, a button for each window and a clock,
  beveled window frames with caption buttons, and the Win95 system colors.
  UI text is anti-aliased (4 bits of coverage per pixel, rasterised from
  Liberation Sans); only the terminals and the text editor's monospace text keep
  the sharp 8x16 bitmap font.
- Icons are PNG images from the public-domain [Tango icon
  theme](https://commons.wikimedia.org/wiki/Tango_icons), in
  `/usr/share/icons/tango/{16x16,32x32,48x48}`. The desktop has its own PNG
  decoder (inflate included) and alpha-blends images, so any 8-bit,
  non-interlaced PNG can be shown; a missing icon file falls back to a
  simple built-in drawing.
- JPEG pictures too: the desktop decodes baseline and progressive JPEG
  (greyscale, YCbCr or RGB, any chroma subsampling, restart intervals), with
  smooth chroma upsampling so photographs have no blocky colour edges. The
  file's first bytes decide which decoder runs, whatever its name, in the
  image viewer, the file manager's icons, the browser and the wallpaper.
- Wallpaper: the desktop shows a picture (JPEG or PNG) behind its icons,
  by default a photograph in `/usr/share/wallpapers`. It can be **Fill**ed
  (scaled evenly to cover the screen, the overflow cut off), **Stretch**ed
  to the screen's exact size, **Center**ed at its own size, or **Tile**d. It
  is drawn again from the original whenever the screen changes size, and the
  icons' labels get a shadow so they read on a bright picture.
- Windows can be resized by dragging any edge or corner (the pointer turns
  into a resize arrow), maximized and restored with the caption button or by
  double-clicking the caption. The browser, text editor and file manager
  resize; terminals, dialogs, the calculator and the image viewer have a
  fixed size.
- Windows move by dragging an outline, minimize to the taskbar, raise and take
  the focus when clicked, and cascade as they open.
- Terminal windows: 80x25 VGA-style terminals running fsh on a
  pseudo-terminal, as many as you like. Closing one hangs up its shell.
- File manager: a Win95 Explorer-style folder window (My Computer opens it on
  `/`, the Start menu on your home folder) with large icons, a scroll bar, an
  address field you can type a path into, and Up, New Folder, Delete and
  Rename (also Backspace, Delete, F2, F5 and Ctrl+L). Mounted disks and CDs
  get drive icons, and every folder window reads its folder again when a
  disc or floppy is mounted or unmounted. Double-clicking a folder opens it, a program or script
  runs in a terminal window, a PNG or JPEG picture opens in an image viewer,
  a sound plays, and any other file is shown with `more`.
- Calculator: a Windows 95 style standard calculator with memory keys
  (MC, MR, MS, M+), percent, square root, reciprocal, change sign, CE and
  Back, usable with the mouse or the keyboard (digits, `+ - * /`, Enter, `%`,
  `@` for square root, `r` for reciprocal, Delete for CE, Esc to clear). It
  does exact decimal arithmetic on digit strings with 30 significant digits
  (rounded to 24 on screen), so `0.1 + 0.2` is `0.3` and `1/3*3` is `1`.
- Text editor: a Notepad-style window with File and Edit menus (New, Open,
  Save, Save As, Cut, Copy, Paste, Select All), a monospaced text area with
  both scroll bars, mouse and Shift+arrow selection, a clipboard shared
  between editors, and a status bar with the line and column. It opens and
  saves plain text files (up to 1 MB), appends `.txt` to a name with no
  extension, keeps CRLF files as CRLF, and asks "Save changes?" before
  discarding anything. Double-clicking a `.txt`, `.md`, `.log`, `.cfg`,
  `.conf`, `.ini`, `.c` or `.h` file in the file manager opens it there.
- Web browser: an Internet Explorer 3 style window (the **Web Browser** icon)
  with Back, Forward, Stop, Refresh and Home, an address bar, a progress
  gauge, and the address of the link under the mouse in its status bar. It
  fetches pages over HTTP or HTTPS (by running `wget`, so the desktop keeps
  working while a page loads), then their style sheets and PNG and JPEG
  pictures,
  and opens `file://` pages and folders too.
  - Its HTML parser follows the main rules of the HTML5 tree builder (implied
    and auto-closed tags, tables, character references, quirks mode) and
    takes UTF-8, Latin-1 and Windows-1252.
  - Its CSS supports type, class, id, attribute and structural selectors
    with all four combinators, `:not()` and `:is()`, specificity and
    `!important`, `@media` queries, custom properties with `var()`,
    `::before`/`::after` text, and the old presentational attributes
    (`bgcolor`, `<font>`, `align`, `cellpadding`...).
  - Its layout does blocks with margin collapsing, inline text with line
    breaking and alignment, inline-blocks, lists with markers, tables with
    `colspan`, `rowspan` and automatic column widths, flex rows, borders,
    backgrounds, images and form controls, with a scroll wheel.
  - Links, `GET` and `POST` forms (text fields, check boxes, radio buttons,
    selects, buttons), `<meta refresh>`, fragments (`#name`), redirects, and
    `:visited` colors work. Text is anti-aliased: Liberation Sans, Serif and
    Mono in 12 sizes are rasterised by `tools/mkfont.py` with 4 bits of
    coverage per pixel (the file is 2.6 MB), and drawing blends each pixel
    with the background.
- Setup: a Win95-style wizard (the **Install fluxOS** icon) that installs
  fluxOS on a hard disk. It lists the disks, greying out any that are too
  small or hold the install files, asks you to confirm that the chosen disk
  will be erased, then runs `fluxinstall` and shows its progress and output.
  When it finishes it offers to restart; if it fails it shows why and lets
  you go back and pick again.
  It only appears when fluxOS was started from the install medium: GRUB tells
  the kernel which filesystem it booted from, and a system started from an
  installed hard disk hides the icon and the Start menu entry. (`fluxinstall`
  still works from a shell there.)
- Display Properties: a Win95-style dialog with three pages.
  - **Background** picks the wallpaper from a list (the pictures in
    `/usr/share/wallpapers` and your home folder, or **(None)**; **Browse...**
    takes the path of any other) and how it fills the screen. A preview
    monitor, shaped like the real screen, shows the choice as soon as it is
    made, drawn from a thumbnail: a JPEG decodes at an eighth of its size,
    which takes a fraction of the time of the whole picture.
  - **Settings** shows a monitor with a miniature of the desktop at the
    chosen size, the display adapter, the driver (standard VGA or the 3dfx
    driver), the color palette (High Color, 16 bit, or True Color, 32 bit),
    the desktop area on a Less/More slider, and the refresh rate.
  - **Monitor** picks the monitor type: Plug and Play (from its EDID) or a
    generic VGA/SVGA model, which limits the modes offered to what that
    monitor can show by resolution, line rate and refresh. It also shows
    what the EDID says (model, manufacturer, date, size, preferred mode,
    frequency ranges) and the timing of the mode on the screen.
  - With the 3dfx driver a new mode takes effect at once; the desktop is laid
    out again and asks whether to keep it, going back to the old mode by
    itself after 15 seconds. With the standard driver the BIOS mode stays,
    and a system installed on a hard disk can choose the mode to start in.
  - Settings, wallpaper included, are saved for the next start in GRUB's
    environment block (`/boot/grub/grubenv` on the installed partition);
    started from the CD they last until a restart.
- 3D Gears: the classic OpenGL gears demo in a resizable window, drawn by
  the Voodoo3's 3D engine when there is one and by the software renderer
  otherwise. The status bar names the renderer and counts frames per
  second; drag or use the arrow keys to turn the view, Space pauses, Home
  resets it, and S switches between the 3D hardware and the software
  renderer.
- Sound: a speaker in the taskbar's tray (crossed out when muted) opens
  **Volume Control**, a column for each mixer output with a vertical
  slider and a Mute box. The desktop plays a startup sound, terminals beep
  on ^G, and double-clicking a `.wav` or `.voc` file plays it.
- Start menu: Terminal, File Manager, Text Editor, Web Browser, Calculator,
  3D Gears, Read Me, System Properties (version, memory, display, uptime),
  Display Properties, Run... (runs a command in a new window), Install
  fluxOS..., and Shut Down... (power off, restart, or close the desktop and
  return to the text console).
- Keys: the Windows key or Ctrl+Esc opens the Start menu, Alt+Tab switches
  windows, Alt+F4 closes a window (or, with none focused, opens Shut Down).
  Dialogs follow Tab, Enter, Escape and the arrow keys.

**DOS programs** (`/bin/dos`)
- `.COM` and MZ `.EXE` files run in the terminal: type `./GAME.EXE` (the
  kernel hands DOS programs to `/bin/dos`), `dos game` (the extension and case
  may be left out), or double-click one in the file manager.
- `dos` emulates a PC in user space: an x86 CPU (the 8086 with the 186, 286,
  386 and 486 additions, 32-bit registers included; no FPU, which programs
  detect as on a PC without one), 640 KiB of conventional memory, a BIOS
  (video, keyboard, timer, clock, equipment)
  and DOS 5: files and directories by handle and by FCB, memory blocks, EXEC
  of child programs, the environment, Ctrl-C, country information, and the
  usual undocumented structures (List of Lists, InDOS, MCB chain). Interrupt
  vectors point into an emulated ROM, so programs can hook and chain them, and
  the timer (IRQ 0, 18.2 Hz or as reprogrammed through the PIT) and keyboard
  (IRQ 1, port 60h) interrupts reach programs that install handlers.
- Protected mode: a DPMI 0.9 host with 16 MiB of memory, for DOS extenders
  such as DOS/4GW (DOOM and other 32-bit games). It provides descriptors,
  extended and DOS memory blocks, protected-mode interrupt vectors with
  reflection to real mode, real-mode calls and callbacks, and passes
  hardware interrupts that happen in real mode up to protected-mode
  handlers. Paging and limit checks are not emulated; a fault ends the
  program with a report in the `dos -l` log.
- Drives: `C:` is the fluxOS root, `A:` and `B:` are `/mnt/fd0` and `/mnt/fd1`
  (a floppy is mounted when it is put in, or at the latest the first time a
  program looks at the drive) and `D:`
  is `/mnt/cdrom`. Names are matched case-insensitively; directory listings
  show the names that fit in 8.3. DOS standard handles are fluxOS's
  descriptors 0-2, so `dos PROG > out.txt` and pipes work.
- Screen: a program that only prints behaves like a fluxOS program, and its
  output scrolls in the terminal. As soon as it uses the screen (writes text
  video memory, moves the cursor, scrolls a window, sets a video mode), the
  terminal shows its 80x25 text screen, colours and box drawing included,
  redrawn as it changes. Console output understands ANSI.SYS escapes.
  In the desktop's terminal, VGA mode 13h graphics show too, doubled to
  640x400, chained or unchained ("mode X": planes, map mask, read map,
  latches, display start for page flipping), with the 256-colour palette.
- Keys: arrows, Home/End, PgUp/PgDn, Insert/Delete, F1-F12 and Alt+letter
  arrive as PC scan codes. In graphics mode the desktop's terminal sends
  every press and release, so games see keys held down. **Ctrl+\** stops
  the emulator at any time.
- Sound, passed on to fluxOS's sound devices:
  - the PC speaker, when port 61h connects PIT channel 2 to it;
  - an AdLib at 388h (and the SB16's FM ports): writes go to the real OPL
    chip, and the status port's timers are emulated for detection;
  - a Sound Blaster 16 at 220h, IRQ 5, DMA 1 and 5, announced as
    `BLASTER=A220 I5 D1 H5 T6`: the DSP (reset, version 4.05, 8-bit and
    16-bit single-cycle and auto-init DMA output, time constants and rates,
    pause, resume, silence blocks), its mixer, and both 8237 DMA
    controllers. The transfer runs at the real sample rate, a little ahead
    of what is heard, fetching samples from the program's memory as their
    time comes; the DMA counters move as on a PC and IRQ 5 ends each
    block, so programs that watch the DMA position or chain blocks work.
- EXEC of a fluxOS program runs it as a real process, and `COMMAND.COM`
  runs the fluxOS shell (`COMMAND /C cmd` runs `sh -c cmd`), so a DOS
  program's "shell out" feature works. `dos -l FILE` (or `DOSLOG=FILE`) logs
  what the emulator does not support, for diagnosing a program that fails.

**OpenGL** (`libGL.a`, `<GL/gl.h>`, `<GL/fluxgl.h>`, `<GL/glu.h>`)
- A subset of OpenGL 1.1's fixed-function pipeline: `glBegin`/`glEnd` with
  every primitive type, the modelview, projection and texture matrix
  stacks, per-vertex lighting (eight lights: directional, positional and
  spot lights with attenuation, ambient, diffuse and specular terms,
  materials and `GL_COLOR_MATERIAL`), flat and smooth shading, back-face
  culling, the depth buffer, the scissor box, display lists, `glGet*`, and
  `gluPerspective`, `gluLookAt` and `gluOrtho2D`.
- Polygons are clipped against the view volume in homogeneous coordinates;
  lines and points become thin polygons, so a backend only fills convex
  polygons.
- Two backends. On a 3dfx card the GL library drives the 3D engine from user
  space, as Glide does: each context gets a 16-bit color buffer and depth
  buffer in video memory, polygons go to the chip's triangle setup unit,
  `glClear` is a fast fill, and the picture is read back when the frame is
  done. Anywhere else (or with `LIBGL_ALWAYS_SOFTWARE` set) a scanline
  rasterizer fills a 24-bit color buffer with a 30-bit depth buffer.
- `fglCreateContext`, `fglMakeCurrent`, `fglResize` and `fglSwapBuffers`
  play GLX's part: a context draws into its own picture, which
  `fglSwapBuffers` copies to a pixel buffer such as a desktop window's.
- A math library (`<math.h>`) on the x87's own instructions. Programs that
  use GL or `<math.h>` are built with `USER_FP_CFLAGS` (see the Makefile);
  the rest of user space stays integer-only.

**Userland** (`/bin`): basename beep cat chmod clear cp date desktop df dirname dmesg dos echo
env fetch fluxinstall free fsh grep halt head hexdump host ifconfig init ls lsblk
mkdir mkfs.fat mixer more mount mv nc ping play poweroff ps reboot rm rmdir seq sh sleep sort
spin stat sync tail tee top touch tr umount uname uniq uptime wc wget yes, all on a
small libc (stdio, malloc, environ, dirent, time, sockets and a DNS resolver).
The network tools:
- `ifconfig` shows the interface (address, lease, MAC, counters) and sets a
  static address (`ifconfig ne0 10.0.2.15 netmask 255.255.255.0 gw 10.0.2.2
  dns 10.0.2.3`) or asks DHCP again (`ifconfig ne0 dhcp`).
- `ping [-c N] HOST`, `host NAME`, and `nc HOST PORT`, `nc -l PORT` and
  `nc -u HOST PORT` (netcat).
- `wget [-q] [-O FILE] [-T SECONDS] [--save-headers] [--content-on-error]
  [--max-redirect=N] [--post-data=STRING] URL` downloads over HTTP/1.0,
  following redirects and decoding chunked bodies. `https://` URLs go
  through libc's TLS 1.3 client (`<tls.h>`): X25519 key exchange and
  AES-128-GCM, written from scratch. It encrypts the connection and checks
  the handshake's integrity, but does not verify the server's certificate.
- `/dev/random` and `/dev/urandom` hash a pool that every interrupt stirs
  with the CPU's cycle counter (SHA-256), so TLS keys are not guessable.

The sound tools:
- `beep [-f HZ] [-l MS] [-r N] [-d MS] [-n ...]` beeps the PC speaker, and
  `beep -m MUSIC` plays a tune written in BASIC's PLAY language (notes,
  lengths, dots, octaves, tempo, staccato and legato).
- `play [-q] FILE...` plays WAV (8 to 32-bit PCM) and Creative Voice
  (`.voc`) files on the Sound Blaster, resampling rates the card lacks;
  `play -t HZ [-l MS]` plays a test tone. A startup sound, a "ding" and a
  VOC chime are in `/usr/share/sounds`.
- `mixer` shows the card and its volumes; `mixer CHANNEL LEVEL [RIGHT]`
  sets one.

## Building

Requirements on a Debian/Ubuntu host:

```sh
sudo apt install build-essential grub-pc-bin grub-common xorriso mtools qemu-system-x86
```

No cross compiler is needed. Everything is built with the host GCC using `-m32
-ffreestanding` and GCC's own freestanding headers. Nothing links against
libgcc or the host libc.

```sh
make            # builds build/fluxos.kernel, build/initrd.tar and fluxOS.iso
make check      # verifies the kernel is a valid Multiboot image
make disk       # creates fluxdisk.img, a 64 MiB FAT32 data disk (DISK_MB=...)
make floppy     # creates fluxfloppy.img, a 1.44 MB FAT12 floppy for drive A:
make run        # boots the ISO in QEMU with fluxdisk.img as /dev/hda (and
                # fluxfloppy.img as /dev/fd0, if it exists) and an NE2000
make run-kernel # boots the kernel directly with QEMU's Multiboot loader (text console)
make run-disk   # boots fluxdisk.img alone, after installing fluxOS onto it
make clean      # removes build/ and the ISO; the disk images are kept
```

The QEMU targets give the machine an NE2000 card on QEMU's user-mode
network, which hands out an address by DHCP and reaches the internet
through the host; change `QEMU_NET` (for example to `-netdev
user,id=net0 -device ne2k_isa,netdev=net0`) for an ISA card instead.

To boot real hardware or another VM, write `fluxOS.iso` to a USB stick or
attach it as a CD-ROM, and boot in **legacy BIOS** mode; UEFI is untested.
The default boot entry starts the desktop in a 1024x768 graphics mode. The
second entry, **fluxOS (text console)**, boots in VGA text mode with the
shell on the console, as does `make run-kernel`. In QEMU the mouse works
without grabbing, through the vmmouse interface; elsewhere click into the VM
window to capture the mouse.

For the 3dfx driver and hardware-accelerated GL, give the machine a Voodoo
Banshee or Voodoo3 as its video card (86Box emulates both, as "3dfx Voodoo3
3000" and so on); QEMU has no 3dfx card, so there the GL library uses its
software renderer. fluxOS still starts in the BIOS mode: switch to the 3dfx
driver in Display Properties, or add `video=1024x768x16@85` to the kernel
command line in GRUB.

## Installing to a hard disk

Boot the CD and double-click **Install fluxOS** on the desktop: the Setup
wizard walks you through choosing a disk. From a shell, run the installer on
a whole disk instead (see `lsblk`):

```sh
fluxinstall /dev/hda      # asks you to type "yes"; -y skips the question
```

The installer:
1. writes a new MBR with one bootable FAT32 partition starting at 1 MiB;
2. puts GRUB's boot sector in the MBR and its core image in the gap before
   the partition, as `grub-install` would;
3. formats the partition with `mkfs.fat`;
4. copies the kernel, the initrd, `grub.cfg` and GRUB's modules from the CD,
   and unpacks the initrd's files (`/bin`, `/etc`, `/home`, `/usr`) onto the
   partition.

**This erases the whole disk.** The disk must be at least 34 MiB. Afterwards
the disk boots on its own, through GRUB's menu, with the partition as `/`. An
installed disk carries the installer's files too, so it can install onto
another disk with `fluxinstall -s / /dev/hdb`.

In QEMU, `make run` attaches `fluxdisk.img` next to the CD. Install with
`fluxinstall /dev/hda`, then quit and run `make run-disk` to boot the disk
alone.

The installed system runs from the disk, as Windows runs from C:, so what is
saved anywhere outside `/tmp` stays there. The boot menu's **safe mode**
entry runs from the initrd in memory instead, like the CD, with the disk on
`/mnt/hda1`.

## Things to try

On the desktop: double-click **My Computer** to browse files, drag windows by their title
bars, open more terminals from the Start menu, try **Run...** with `ls -l /bin`,
and open **Shut Down...** to see the screen dim. Double-click **Web Browser**
and follow the links on its home page (68k.news, wiby.me and textfiles.com are
made for simple browsers), search from it, or type `example.com` in the address
bar. In a terminal:

```sh
fetch                          # system summary
spin A 5 & spin B 5 & top      # CPU-bound jobs sharing the CPU; ^C quits top
ps; jobs; kill %1
cat ~/README.txt; ./demo.sh       # tour and a scripting demo
ls -l /bin | sort -n | tail -3
echo {1..3} $((6*7)) ~         # brace, arithmetic and tilde expansion
mkdir -p /tmp/{a,b}/c; ls -R /tmp
cd /etc; ..                    # AUTO_CD via the '..' alias
dmesg | tail
lsblk; df -h; mount            # disks, CD-ROM and mounted filesystems
ls /mnt/cdrom/boot             # the boot CD itself, via ATAPI + ISO9660
cp -r ~ /mnt/hda/home; ls -l /mnt/hda/home   # FAT32 writes survive reboots
umount /mnt/hda; mkfs.fat -n MYDISK /dev/hda; mount /dev/hda /mnt/hda
ifconfig; ping -c 3 10.0.2.2   # the network card, and QEMU's gateway
host example.com               # DNS
wget -O - http://example.com/  # HTTP over the TCP stack
wget -O - https://example.com/ # HTTPS: TLS 1.3
nc -l 7777 &  echo hi | nc localhost 7777    # a TCP server and client
ls /mnt/fd0; df -h /mnt/fd0     # the floppy (make floppy, then make run)
dmesg | grep -E "mounted|removed"   # discs and floppies mounted as they are put in
cp /mnt/fd0/GAME.EXE ~; ./GAME.EXE   # a DOS program, in the terminal
beep -m "T160 L8 CDEGEC4"      # the PC speaker, with BASIC's PLAY music
play /usr/share/sounds/startup.wav   # the Sound Blaster (also .voc files)
play -t 440; mixer; mixer pcm 60     # a test tone; the volumes
dos edit                       # finds EDIT.COM or EDIT.EXE in this directory
desktop                        # (from the text console) start the desktop
poweroff
```

Files written to `/mnt/hda` live in `fluxdisk.img`, and those on `/mnt/fd0`
in `fluxfloppy.img`. On a Linux host you can inspect the images with `mdir -i
fluxdisk.img ::` (mtools) or `fsck.fat -n fluxdisk.img`, and put DOS programs
on the floppy with `mcopy -i fluxfloppy.img GAME.EXE ::`.

## Layout

```
Makefile, linker.ld        build and kernel link script
grub/grub.cfg              boot menu
include/flux/              kernel <-> user ABI (syscall numbers, structs, errno)
lib/                       string and printf routines and the 8x16 font (code
                           page 437), shared by kernel and libc
tools/mkfont.py            regenerates the fonts, including the browser's
                           rootfs/usr/share/fonts/web.fnt (needs Python and PIL)
tools/tango-icons.py       re-fetches and scales the Tango icons in rootfs/
tools/mksounds.py          synthesizes the sounds in rootfs/usr/share/sounds
kernel/
  boot.S, entry.S          multiboot entry, ISR stubs, trap return, context switch
  gdt.c, idt.c             descriptor tables, PIC, trap dispatch
  pmm.c, vmm.c, heap.c     frame allocator, paging, kernel heap
  proc.c, exec.c           processes, scheduler, signals, ELF loader
  syscall.c                system call table and user-pointer validation
  vfs.c, file.c, initrd.c  VFS + ramfs, mounting, devices, open files, pipes, initrd
  ata.c, blkdev.c, pci.c   IDE ATA/ATAPI driver (DMA), block devices, partitions, cache
  floppy.c                 floppy disk controller driver (ISA DMA, track cache)
  automount.c              kmediad: mounts and unmounts CDs and floppies as they
                           are put in and taken out
  fat32.c, iso9660.c       filesystem drivers (FAT12/16/32, ISO9660)
  tty.c, vga.c, serial.c,  console stack: terminals and ptys, text console,
  keyboard.c, timer.c      serial port, PS/2 keyboard, PIT, RTC, power control
  fb.c, mouse.c, input.c   framebuffer and SYS_display, PS/2 mouse and vmmouse,
                           /dev/input
  voodoo.c                 3dfx Banshee/Voodoo3 driver: modes, EDID, 3D engine
  pcspkr.c, sb16.c, opl.c  sound: PC speaker, Sound Blaster 16 (/dev/dsp,
                           /dev/mixer), OPL FM synthesizer; isadma.c, ISA DMA
  fpu.c                    lazy x87 FPU context switching
  ne2000.c                 NE2000 network card driver (PCI and ISA)
  net.c                    Ethernet, ARP, IPv4, ICMP, loopback, DHCP client
  tcp.c, socket.c          TCP, and sockets (with UDP and ICMP echo)
  kmain.c                  boot sequence
user/
  libc/, include/          the C library (math.c is the x87 math library) and
                           its headers, GL/ included
  gl/                      the OpenGL library: state and matrices (gl.c), the
                           vertex pipeline, lighting and clipping (prim.c),
                           display lists (list.c), GLU (glu.c), and the 3dfx
                           (tdfx.c) and software (soft.c) backends
  fsh/                     the shell: lexer, parser, expansion, executor, editor
  desktop/                 the desktop: compositor and window manager (wm.c),
                           terminal emulator, file manager and image viewer
                           (files.c), the text editor (edit.c), the calculator
                           (calc.c), the Setup wizard (setup.c), Display
                           Properties (display.c), 3D Gears (gears.c), the
                           tray speaker and Volume Control (sound.c), dialogs,
                           drawing, PNG and JPEG decoding (png.c, jpeg.c, with
                           image.c choosing between them), the wallpaper
                           (wallpaper.c), GRUB's environment block
                           (grubenv.c), icons, fonts,
                           and the web browser: its window (browser.c), HTML
                           parser (html.c), CSS (css.c), layout and painting
                           (layout.c), fonts (webfont.c) and URLs (webutil.c)
  dos/                     the DOS emulator: the x86 CPU (cpu.c), the DPMI host
                           (dpmi.c), ROM, interrupt controller, timer and BIOS
                           services (bios.c), keyboard (keyboard.c), text
                           screen, VGA and INT 10h (video.c), DOS processes,
                           memory and console
                           (dos.c), files, paths and FCBs (files.c), and the
                           PC speaker, AdLib, Sound Blaster and DMA (sound.c)
  bin/                     one program per file (fluxinstall.c is the installer)
rootfs/                    files copied into the initrd (/etc, /home/flux,
                           /usr/share/icons, the browser's fonts and its
                           home page in /usr/share/www)
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

The kernel reaches page tables and user frames through the identity map, which
is why physical memory is used only up to 1 GiB.

**Scheduling.** Each trap pushes a full `struct trapframe` on the process's
16 KiB kernel stack. The kernel itself is not preemptible: interrupt gates keep
IF clear inside the kernel. A process gives up the CPU only in `schedule()`,
either because it blocked or because the timer set `need_resched` and the trap
is about to return to user mode. `switch_context` swaps kernel stacks, and a
new process starts by "returning" into `trapret`.

**Disk I/O.** Filesystem and block-device work may now sleep in the middle of
an operation (waiting for a disk interrupt), so it is serialised by one FIFO
"VFS lock". System calls that walk or change the tree hold it for the call.
`read`/`write` on files and block devices take it per 64 KiB chunk, so a long
copy can't starve other disk users. Terminals and pipes never take it.

**Write-back.** FAT, directory-entry and FSInfo updates go into the sector
cache as dirty sectors. File data bypasses the cache: a run of contiguous
clusters is written as one DMA command. Raw reads overlay any dirty cached
sectors, so they always see the newest data. A sync writes the dirty sectors
in LBA order, merging neighbours into multi-sector commands, then issues one
FLUSH CACHE to the drive (skipped for drives that reject that command, as
pre-ATA-5 drives and 86Box's emulated disks do). `kflushd` is a kernel thread. It runs in ring 0
with interrupts off like the rest of the kernel, so it can only be switched
out while sleeping, and it ignores signals.

**Graphics.** GRUB sets the video mode because the kernel's Multiboot header
asks for one. The kernel maps the framebuffer above 3 GiB with 4 MiB pages,
and its console draws there with an 8x16 font, redrawing only changed cells
and moving pixels once per write however many lines scrolled. `fbmap` maps
the same memory into the caller's address space with PTEs marked as device
memory, which `fork` shares instead of copying and `exit` never frees.

**The 3dfx driver** is modelled on Linux's tdfxfb. At boot it maps the
first 4 MiB of the register BAR (uncached) and all of video memory, moves
the tiled aperture past the end of memory so every address reads linearly,
and saves the VGA, extension and video-processor registers of the BIOS
mode. A mode change waits for the engines to go idle, programs the PLL for
the timing's pixel clock (the DAC takes two pixels per clock above half
its limit), the CRTC with the Banshee's overflow registers, the pixel
format and the desktop stride, then points `fb` at the new screen: the
console lays its text out again, and the process that owns the screen
gets the new one mapped where the old one was, then reads `fbinfo` again
and rebuilds its back buffer (`wm_reconfigure`). `DISP_MAP3D` maps the 3D
register block at `0xB4000000` and video memory at `0xB2000000`; GL
contexts take their buffers from the top of video memory, and fall back
to the software renderer if a bigger screen mode grows over them.

**Sound.** The Sound Blaster 16 is always fed in one format: signed 16-bit
on its 16-bit DMA channel (or unsigned 8-bit without one), so `write()`
converts as it copies into the ring and the interrupt handler only copies
a half's worth into the DMA buffer, padding with silence when the ring is
short. Once both halves hold nothing but silence the DSP is paused, and the
next write starts it again. A format change waits for what was written in
the old one to play. If no interrupt arrives within a second, the driver
says so in the kernel log and stops, instead of leaving writers asleep.
The PC speaker's queue advances in the timer interrupt, so tones need no
process to keep them going.

**The FPU.** CR0.TS is set whenever the process on the CPU is not the one
whose registers the FPU holds. Its first FPU instruction raises #NM, and
the handler saves the owner's registers into its `struct proc` with FNSAVE
and loads the current process's with FRSTOR (or FNINIT for a first use).
SSE is never enabled, so only the 108-byte x87 state needs saving.

**The desktop** composes the screen in a back buffer. Every change records a
damaged rectangle; once per pass through its event loop, the desktop repaints
each one bottom to top (wallpaper and icons, windows, taskbar, menus, drag
outline, pointer) and copies it to the framebuffer. Each window keeps its
client area in its own buffer, so terminals only redraw cells that changed.
One `poll` covers `/dev/input` and every terminal's pty master, with a timeout
for the cursor blink and the clock. A terminal's shell runs on the pty slave;
when its last program exits the master reports a hangup and the window
closes.

**Networking.** The NE2000's interrupt handler passes each frame up the
stack, and TCP's timers run from the timer interrupt, so packets are handled
as they arrive, whatever is running. That is safe because the kernel runs
with interrupts off: an interrupt can only arrive while a process is in user
mode or the idle loop is halted, never in the middle of kernel code. A
process waiting on a socket sleeps on it and is woken from the interrupt
handler. Packets to our own address go through a loopback queue that the
next timer tick delivers, so a reply is never built inside the code that
sent the request. TCP keeps segments that arrive out of order out: it
acknowledges what it has and the sender goes back and resends.

**Pictures and the wallpaper.** The JPEG decoder reads bits from a 32-bit
buffer with a 9-bit lookahead for Huffman codes. A sequential scan goes
block by block through the integer inverse DCT into the component's sample
plane. A progressive file keeps every coefficient (16 bits each) until its
last scan and transforms them all then, so a file that ends early still
shows what arrived, blurrier. Chroma planes are upsampled with a triangle
filter while the colors are converted, a row at a time. The thumbnail
decode skips the transform and takes each block's DC coefficient as one
pixel. The wallpaper is kept as decoded and drawn once into a surface the
size of the screen, so the compositor's repaints of a window's damage are a
copy: a picture more than twice too big is first averaged down in whole
blocks (which avoids aliasing), then interpolated linearly, four channels
at a time in two 32-bit lanes. Decoding happens after the desktop's first
frame, so the terminal appears before the picture does.

**The browser** keeps everything a page owns in arenas: the document tree
and style sheets in one, computed styles in a second that is rebuilt when a
style sheet arrives, and the layout in a third that is rebuilt when a
picture arrives. Layout produces a flat list of rectangles, text runs,
images and controls in page coordinates, which painting clips to the view;
a box whose size must be known first (an inline-block, a table cell) is laid
out at the origin and moved into place. All of it is integer arithmetic:
CSS numbers are fixed point with two decimals.

**System calls.** `int 0x80` with the number in `eax` and arguments in `ebx`,
`ecx`, `edx`, `esi` and `edi`. Failures return `-errno`. Every user pointer is
checked against the user range and the current page tables before the kernel
touches it.

## Limitations

fluxOS supports a single CPU. The root filesystem lives in RAM; only what you
write to a mounted FAT volume persists.

Storage limits:
- **DMA needs a PCI IDE controller with bus mastering.** The channel must be
  in legacy mode (ports 0x1F0/0x170, IRQ 14/15), and the firmware must have
  put the drive in a DMA mode: fluxOS does not set transfer modes or chipset
  timings itself. Anything else uses PIO, which works everywhere but is
  slower and has the CPU copy every sector. SATA controllers in AHCI mode
  are not supported at all.
- **Recent writes can be lost on a crash.** A `write()` returns before the
  metadata reaches the disk, so a power cut within about 5 seconds of a
  write can lose that file. The filesystem itself stays consistent. Run
  `sync` (or `fsync(fd)` from a program) when that matters; `umount`,
  `reboot` and `poweroff` sync automatically. The Ctrl+Alt+Del reboot does
  not, because it happens in the keyboard interrupt handler.
- **Partitioning:** only MBR partition tables, with up to four primary
  partitions. GPT is not supported, and neither are extended or logical
  partitions. A FAT volume with no partition table (a "superfloppy") is
  also accepted.
- **Filesystems:** FAT12, FAT16 and FAT32 (no exFAT). ISO9660 has no Joliet
  names. Without Rock Ridge, ISO names are shown in lower case with the `;1`
  version suffix removed, and files show as read-only.
- **Size limits:** raw access through `/dev/hdX` cannot go past 4 GiB, because
  byte offsets are 32-bit. Directories are capped at 4 MiB (about 131,000
  FAT entries, or the same size of ISO records); any entries past that are
  silently ignored. File sizes are also 32-bit, so a file can't exceed 4 GiB.
- **Permissions:** FAT and ISO9660 store no usable permission bits. FAT files
  show as `rwxr-xr-x` (executable), or `r-xr-xr-x` when the read-only attribute
  is set. `chmod` on those volumes only changes the in-memory
  mode, so the change is lost when the volume is unmounted.
- **Names:** FAT names are limited to 255 UTF-8 bytes. FAT timestamps have
  2-second resolution.
- **Floppies:** only the primary controller at 0x3F0, two drives, and
  double- and high-density disks (a 2.88M drive is used as a 1.44M one).
  Disks are unmounted for you when they come out, but changes that had not
  been written yet (up to 5 seconds' worth, or what `sync` has not
  flushed) are lost with them: unmount a floppy before taking it out, as on
  any system. A disk changed while mounted is refused until it is
  unmounted, which the automounter does at once unless something uses it.
  Looking for a disk in an empty drive steps the head every 2 seconds.

The desktop's terminals are a fixed 80x25 and keep no scrollback, windows
are resized by an outline (the contents redraw when you let go), dialogs,
terminals and the calculator cannot be resized, and the only clipboard is
the text editor's. The editor
has no undo, no word wrap and no search, and shows bytes as code page 437
(no UTF-8). While the desktop runs, the
serial port's input goes to the unused console; its output still mirrors the
kernel log.

DOS programs run in text modes only: a program that switches to a graphics
mode (CGA, EGA, VGA or VESA) is stopped with a message, since a terminal
cannot show it, and the screen stays 80x25 (43- and 50-line modes keep 25
lines). Recording on the Sound Blaster gives no data (the transfer runs, and
nothing is written to memory); ADPCM, the direct DAC (command 10h), the
MPU-401 and a speaker toggled by hand to make samples stay silent. There is
no mouse driver, no EMS or XMS memory, no protected mode (so no DOS extenders such as
DOS/4GW), no batch files, and no BIOS disk access (INT 13h), so self-booting
and copy-protected software will not run. The terminal reports key presses
but not releases, so each key is released right after it is pressed: games
that need keys held down respond poorly. Every instruction is interpreted,
which is quick enough for applications but keeps no PC's timing, so games that
time themselves with delay loops may run too fast. Files keep no DOS
attributes besides read-only, and their times cannot be set.

Networking limits: IPv4 only, one network card, no IP fragments in either
direction, and no TCP congestion control (the receiver's window is the only
limit) or out-of-order buffering. DHCP renews by broadcast. ping times have
the timer's 10 ms resolution.

HTTPS is TLS 1.3 only, with one cipher suite (TLS_AES_128_GCM_SHA256) and
X25519: servers that only speak TLS 1.2, or will not use X25519, cannot be
reached. Certificates are not verified, so the encryption keeps out
eavesdroppers but not someone who intercepts the connection and poses as
the site. The browser runs no JavaScript and shows only PNG and JPEG pictures (GIF
and SVG show their alternative text). Its layout is an approximation: floats
become inline-blocks, positioned elements stay in the normal flow (those
hidden off-screen are dropped), grids are blocks, flex rows are lines of
inline-blocks, and `calc()` takes its first length. It has no font downloads,
cookies, cache of pages, or text selection, and characters outside Latin-1
and common punctuation show as `?`.

Sound limits: only the Sound Blaster 16 (DSP version 4) is supported, not
the earlier Sound Blasters or other cards, and only for output: there is
no recording, no MIDI (MPU-401) and no hardware mixing of several
programs, so one program at a time has `/dev/dsp`. The latency is about
80 ms, and the PC speaker's tones are timed in 10 ms steps.

Graphics limits: the 3dfx driver is the only one that sets modes; any other
card keeps its BIOS mode, which can only be changed for the next start, and
only on an installed system (GRUB asks the BIOS for it, falling back to
1024x768). There is no hardware cursor, no 2D acceleration (the desktop
draws in memory and copies), no 8-bit palette modes, and no interlaced or
doublescan modes. The 3D engine renders 16-bit color with a 16-bit depth
buffer, and the picture is read back over the bus every frame, which is
slow for big windows. The GL library has no textures, blending, fog,
stencil, vertex arrays, pixel operations, selection or feedback; lighting
is per vertex with an infinite viewer and front faces only (two-sided
lighting is accepted and ignored); polygons are assumed convex; and wide
lines and points are plain rectangles. Only one process at a time can use
the 3D hardware; others get the software renderer.

Permission bits are cosmetic apart from the exec bit, and there is a single
user. Signals cannot have user handlers, and jobs
cannot be stopped (no ^Z). User programs may use the x87 FPU but not SSE,
whose registers the kernel does not save: SSE instructions raise `SIGILL`.

Pictures: files that are not JPEGs of the supported kinds (arithmetic
coding, 12-bit samples, lossless, CMYK) or non-interlaced 8-bit PNGs are
not shown; the JPEG decoder ignores the EXIF orientation tag and color
profiles, and handles pictures up to 4096 by 4096. A 1920x1080 photograph
takes a few seconds to decode on a slow machine, with the desktop waiting;
the wallpaper list holds 40 pictures.

## Credits

The default wallpaper, `/usr/share/wallpapers/mountains.jpg`, is a photograph
by Štefan Štefančík on [Unsplash](https://unsplash.com), used under the
[Unsplash License](https://unsplash.com/license). Thank you for sharing it.

The icons are from the public-domain [Tango Desktop
Project](https://commons.wikimedia.org/wiki/Tango_icons), and the desktop's
text is rasterised from Liberation Sans, Serif and Mono.
