# fluxOS

fluxOS is a small 32-bit x86 Unix-like operating system with preemptive
multitasking, ring-3 user processes, paging, a RAM root filesystem, IDE
disk and CD-ROM drivers with FAT32 and ISO9660 support, and a
zsh-flavoured shell called **fsh**. It boots with GRUB (Multiboot) from an ISO
image.

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
- Console: VGA text mode with an ANSI/VT100 escape interpreter, PS/2 keyboard
  (arrows and modifiers map to escape sequences), and a mirrored COM1 serial
  console. The tty line discipline supports canonical and raw modes, echo,
  erase/kill/word-erase and EOF.
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

**Userland** (`/bin`): basename cat chmod clear cp date df dirname dmesg echo
env fetch free fsh grep halt head hexdump init ls lsblk mkdir mkfs.fat more
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
make run-kernel # boots the kernel directly with QEMU's Multiboot loader
make clean      # removes build/ and the ISO; fluxdisk.img is kept
```

To boot real hardware or another VM, write `fluxOS.iso` to a USB stick or
attach it as a CD-ROM. fluxOS uses VGA text mode, so boot in **legacy BIOS**
mode. Under UEFI-only firmware GRUB starts the kernel, but nothing is visible
except on the serial port.

## Things to try

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
lib/                       string and printf routines shared by kernel and libc
kernel/
  boot.S, entry.S          multiboot entry, ISR stubs, trap return, context switch
  gdt.c, idt.c             descriptor tables, PIC, trap dispatch
  pmm.c, vmm.c, heap.c     frame allocator, paging, kernel heap
  proc.c, exec.c           processes, scheduler, signals, ELF loader
  syscall.c                system call table and user-pointer validation
  vfs.c, file.c, initrd.c  VFS + ramfs, mounting, devices, open files, pipes, initrd
  ata.c, blkdev.c, pci.c   IDE ATA/ATAPI driver (DMA), block devices, partitions, cache
  fat32.c, iso9660.c       filesystem drivers
  tty.c, vga.c, serial.c,  console stack
  keyboard.c, timer.c      PS/2 keyboard, PIT, RTC, power control
  kmain.c                  boot sequence
user/
  libc/, include/          the C library and its headers
  fsh/                     the shell: lexer, parser, expansion, executor, editor
  bin/                     one program per file
rootfs/                    files copied into the initrd (/etc, /home/flux)
```

## Design notes

**Memory map.** Every address space shares the kernel half.

| Range                       | Use                                              |
|-----------------------------|--------------------------------------------------|
| `0x00000000 - 0x3FFFFFFF`   | kernel; identity map of the first GiB of RAM     |
| `0x40000000 - ...`          | user program image, then the `sbrk` heap         |
| `0xBF800000 - 0xBFFFFFFF`   | user stack (up to 8 MiB, grown on page faults)   |

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

Permission bits are cosmetic apart from the exec bit, and there is a single
user. Signals cannot have user handlers, and jobs
cannot be stopped (no ^Z). User programs may not use the FPU: the kernel does
not save FPU state, so FPU instructions raise `SIGILL`.
