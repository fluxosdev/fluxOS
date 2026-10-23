# fluxOS

fluxOS is a small 32-bit x86 Unix-like operating system with preemptive
multitasking, ring-3 user processes, paging, a RAM root filesystem, IDE
disk and CD-ROM drivers with FAT32 and ISO9660 support, an NE2000 network
driver with a TCP/IP stack, a zsh-flavoured shell called **fsh**, and a
Windows 95 style graphical desktop with a web browser. It boots with
GRUB (Multiboot) from an ISO image, straight into the desktop.

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
- Storage:
  - An IDE driver for ATA hard disks (LBA28/LBA48, read/write) and ATAPI
    CD/DVD drives (packet commands, media detection). Data moves by
    bus-master DMA, up to 64 KiB per command, found through PCI. It is
    interrupt driven: a process waiting on the disk sleeps until IRQ14/15,
    so other processes keep running. If a DMA command fails, the driver
    resets the channel and switches that drive to PIO, so no data is lost.
  - IDE kernel options: `ata.nodma` forces PIO, and `ata.poll` polls instead
    of using interrupts, for hardware whose IDE interrupts don't arrive.
  - Block devices `/dev/hda`..`/dev/hdd`, with MBR partitions as `/dev/hda1`
    and so on, behind a write-back sector cache. Metadata writes stay in
    memory and are written out in sorted, merged runs by the `kflushd`
    kernel thread every 5 seconds, by `sync`/`fsync`, on `umount`, and
    before `reboot`/`poweroff`.
- Filesystems, mounted into the same VFS tree:
  - **FAT32**, read/write: VFAT long file names with `~N` short aliases,
    case-insensitive lookup, directories that grow, all FAT copies kept in
    sync, and FSInfo free counts. Programs can run straight from FAT.
  - **ISO9660**, read-only: Rock Ridge names, modes and timestamps, on CDs or
    on 512-byte-sector disks.
  - Volumes are mounted at boot on `/mnt/cdrom` and `/mnt/hdX`, and with
    `mount`/`umount`, which refuses while files are in use.
- Console: a text console with an ANSI/VT100 escape interpreter, drawn on the
  boot framebuffer with an 8x16 font (or in VGA text mode without one), PS/2
  keyboard (arrows and modifiers map to escape sequences), and a mirrored COM1
  serial console. The tty line discipline supports canonical and raw modes,
  echo, erase/kill/word-erase and EOF.
- Graphics and input: GRUB sets a 1024x768x32 mode, which the kernel maps and
  lends to one process at a time (`fbinfo`, `fbmap`); the console takes the
  screen back when that process exits. A PS/2 mouse driver with IntelliMouse
  wheel support, which uses the VMware/QEMU "vmmouse" interface for absolute
  positioning (and the wheel) when it is there, and `/dev/input`, which
  delivers keyboard and mouse events to the desktop.
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
- Windows move by dragging an outline, minimize to the taskbar, raise and take
  the focus when clicked, and cascade as they open.
- Terminal windows: 80x25 VGA-style terminals running fsh on a
  pseudo-terminal, as many as you like. Closing one hangs up its shell.
- File manager: a Win95 Explorer-style folder window (My Computer opens it on
  `/`, the Start menu on your home folder) with large icons, a scroll bar, an
  address field you can type a path into, and Up, New Folder, Delete and
  Rename (also Backspace, Delete, F2, F5 and Ctrl+L). Mounted disks and CDs
  get drive icons. Double-clicking a folder opens it, a program or script
  runs in a terminal window, a PNG opens in an image viewer, and any other
  file is shown with `more`.
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
  fetches pages over plain HTTP (by running `wget`, so the desktop keeps
  working while a page loads), then their style sheets and PNG pictures,
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
- Start menu: Terminal, File Manager, Text Editor, Web Browser, Read Me, System Properties (version,
  memory, display, uptime), Run... (runs a command in a new window), Install
  fluxOS..., and Shut Down... (power off, restart, or close the desktop and
  return to the text console).
- Keys: the Windows key or Ctrl+Esc opens the Start menu, Alt+Tab switches
  windows, Alt+F4 closes a window (or, with none focused, opens Shut Down).
  Dialogs follow Tab, Enter, Escape and the arrow keys.

**Userland** (`/bin`): basename cat chmod clear cp date desktop df dirname dmesg echo
env fetch fluxinstall free fsh grep halt head hexdump host ifconfig init ls lsblk
mkdir mkfs.fat more mount mv nc ping poweroff ps reboot rm rmdir seq sh sleep sort
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
  following redirects and decoding chunked bodies.

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
make run        # boots the ISO in QEMU with fluxdisk.img as /dev/hda and an NE2000
make run-kernel # boots the kernel directly with QEMU's Multiboot loader (text console)
make run-disk   # boots fluxdisk.img alone, after installing fluxOS onto it
make clean      # removes build/ and the ISO; fluxdisk.img is kept
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
4. copies the kernel, the initrd, `grub.cfg` and GRUB's modules from the CD.

**This erases the whole disk.** The disk must be at least 34 MiB. Afterwards
the disk boots on its own, through GRUB's menu, and the partition is mounted
on `/mnt/hda1`. An installed disk carries the installer's files too, so it
can install onto another disk with `fluxinstall -s /mnt/hda1 /dev/hdb`.

In QEMU, `make run` attaches `fluxdisk.img` next to the CD. Install with
`fluxinstall /dev/hda`, then quit and run `make run-disk` to boot the disk
alone.

The installed system still runs from RAM, like the CD. The disk holds the
kernel and initrd, and its FAT32 partition is where files persist.

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
cat ~/README; ./demo.sh        # tour and a scripting demo
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
nc -l 7777 &  echo hi | nc localhost 7777    # a TCP server and client
desktop                        # (from the text console) start the desktop
poweroff
```

Files written to `/mnt/hda` live in `fluxdisk.img`. On a Linux host you can
inspect the image with `mdir -i fluxdisk.img ::` (mtools) or `fsck.fat -n
fluxdisk.img`.

## Layout

```
Makefile, linker.ld        build and kernel link script
grub/grub.cfg              boot menu
include/flux/              kernel <-> user ABI (syscall numbers, structs, errno)
lib/                       string and printf routines and the 8x16 font, shared
                           by kernel and libc
tools/mkfont.py            regenerates the fonts, including the browser's
                           rootfs/usr/share/fonts/web.fnt (needs Python and PIL)
tools/tango-icons.py       re-fetches and scales the Tango icons in rootfs/
kernel/
  boot.S, entry.S          multiboot entry, ISR stubs, trap return, context switch
  gdt.c, idt.c             descriptor tables, PIC, trap dispatch
  pmm.c, vmm.c, heap.c     frame allocator, paging, kernel heap
  proc.c, exec.c           processes, scheduler, signals, ELF loader
  syscall.c                system call table and user-pointer validation
  vfs.c, file.c, initrd.c  VFS + ramfs, mounting, devices, open files, pipes, initrd
  ata.c, blkdev.c, pci.c   IDE ATA/ATAPI driver (DMA), block devices, partitions, cache
  fat32.c, iso9660.c       filesystem drivers
  tty.c, vga.c, serial.c,  console stack: terminals and ptys, text console,
  keyboard.c, timer.c      serial port, PS/2 keyboard, PIT, RTC, power control
  fb.c, mouse.c, input.c   framebuffer, PS/2 mouse and vmmouse, /dev/input
  ne2000.c                 NE2000 network card driver (PCI and ISA)
  net.c                    Ethernet, ARP, IPv4, ICMP, loopback, DHCP client
  tcp.c, socket.c          TCP, and sockets (with UDP and ICMP echo)
  kmain.c                  boot sequence
user/
  libc/, include/          the C library and its headers
  fsh/                     the shell: lexer, parser, expansion, executor, editor
  desktop/                 the desktop: compositor and window manager (wm.c),
                           terminal emulator, file manager and image viewer
                           (files.c), the text editor (edit.c), the Setup
                           wizard (setup.c), dialogs,
                           drawing, PNG decoding (png.c), icons, fonts,
                           and the web browser: its window (browser.c), HTML
                           parser (html.c), CSS (css.c), layout and painting
                           (layout.c), fonts (webfont.c) and URLs (webutil.c)
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
write to a mounted FAT32 volume persists.

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
- **Filesystems:** FAT32 only, with no FAT12 or FAT16. ISO9660 has no Joliet
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

The desktop's terminals are a fixed 80x25 and keep no scrollback, windows
cannot be resized, and the only clipboard is the text editor's. The editor
has no undo, no word wrap and no search, and shows only ASCII. While the desktop runs, the
serial port's input goes to the unused console; its output still mirrors the
kernel log.

Networking limits: IPv4 only, one network card, no IP fragments in either
direction, and no TCP congestion control (the receiver's window is the only
limit) or out-of-order buffering. DHCP renews by broadcast. ping times have
the timer's 10 ms resolution.

The web browser has no HTTPS (there is no TLS), so most of today's web is
out of reach: it explains this and suggests the plain http:// address
instead. It runs no JavaScript and shows only PNG pictures (GIF, JPEG and
SVG show their alternative text). Its layout is an approximation: floats
become inline-blocks, positioned elements stay in the normal flow (those
hidden off-screen are dropped), grids are blocks, flex rows are lines of
inline-blocks, and `calc()` takes its first length. It has no font downloads,
cookies, cache of pages, or text selection, and characters outside Latin-1
and common punctuation show as `?`.

Permission bits are cosmetic apart from the exec bit, and there is a single
user. Signals cannot have user handlers, and jobs
cannot be stopped (no ^Z). User programs may not use the FPU: the kernel does
not save FPU state, so FPU instructions raise `SIGILL`.
