# fluxOS

fluxOS is a small 32-bit x86 Unix-like operating system with preemptive
multitasking, ring-3 user processes, paging, a RAM root filesystem, IDE
disk and CD-ROM drivers with FAT32 and ISO9660 support, a zsh-flavoured
shell called **fsh**, and a Windows 95 style graphical desktop. It boots with
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
  screen back when that process exits. A PS/2 mouse driver, which uses the
  VMware/QEMU "vmmouse" interface for absolute positioning when it is there,
  and `/dev/input`, which delivers keyboard and mouse events to the desktop.
- Pseudo-terminals (`openpty`) that share the console's line discipline, with
  a controlling terminal per process, hangup (`SIGHUP`) when the master side
  closes, and `poll` over terminals, pipes and devices.
- Kernel log (`dmesg`), CMOS clock, reboot and ACPI power-off (QEMU, Bochs,
  VirtualBox).

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
  UI text uses bitmap fonts rasterised from Liberation Sans.
- Windows move by dragging an outline, minimize to the taskbar, raise and take
  the focus when clicked, and cascade as they open.
- Terminal windows: 80x25 VGA-style terminals running fsh on a
  pseudo-terminal, as many as you like. Closing one hangs up its shell.
- Start menu: Terminal, Read Me, System Properties (version, memory, display,
  uptime), Run... (runs a command in a new window), and Shut Down... (power
  off, restart, or close the desktop and return to the text console).
- Keys: the Windows key or Ctrl+Esc opens the Start menu, Alt+Tab switches
  windows, Alt+F4 closes a window (or, with none focused, opens Shut Down).
  Dialogs follow Tab, Enter, Escape and the arrow keys.

**Userland** (`/bin`): basename cat chmod clear cp date desktop df dirname dmesg echo
env fetch fluxinstall free fsh grep halt head hexdump init ls lsblk mkdir mkfs.fat more
mount mv poweroff ps reboot rm rmdir seq sh sleep sort spin stat sync tail tee
top touch tr umount uname uniq uptime wc yes, all on a small libc (stdio, malloc,
environ, dirent, time).

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
make run        # boots the ISO in QEMU with fluxdisk.img as /dev/hda
make run-kernel # boots the kernel directly with QEMU's Multiboot loader (text console)
make run-disk   # boots fluxdisk.img alone, after installing fluxOS onto it
make clean      # removes build/ and the ISO; fluxdisk.img is kept
```

To boot real hardware or another VM, write `fluxOS.iso` to a USB stick or
attach it as a CD-ROM, and boot in **legacy BIOS** mode; UEFI is untested.
The default boot entry starts the desktop in a 1024x768 graphics mode. The
second entry, **fluxOS (text console)**, boots in VGA text mode with the
shell on the console, as does `make run-kernel`. In QEMU the mouse works
without grabbing, through the vmmouse interface; elsewhere click into the VM
window to capture the mouse.

## Installing to a hard disk

Boot the CD, then run the installer on a whole disk (see `lsblk`):

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

On the desktop: double-click **My Computer**, drag windows by their title
bars, open more terminals from the Start menu, try **Run...** with `ls -l /bin`,
and open **Shut Down...** to see the screen dim. In a terminal:

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
tools/mkfont.py            regenerates the font sources (needs Python and PIL)
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
  kmain.c                  boot sequence
user/
  libc/, include/          the C library and its headers
  fsh/                     the shell: lexer, parser, expansion, executor, editor
  desktop/                 the desktop: compositor and window manager (wm.c),
                           terminal emulator, dialogs, drawing, icons, fonts
  bin/                     one program per file (fluxinstall.c is the installer)
rootfs/                    files copied into the initrd (/etc, /home/flux)
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
FLUSH CACHE to the drive. `kflushd` is a kernel thread. It runs in ring 0
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
cannot be resized, and there is no clipboard. While the desktop runs, the
serial port's input goes to the unused console; its output still mirrors the
kernel log.

Permission bits are cosmetic apart from the exec bit, and there is a single
user. Signals cannot have user handlers, and jobs
cannot be stopped (no ^Z). User programs may not use the FPU: the kernel does
not save FPU state, so FPU instructions raise `SIGILL`.
