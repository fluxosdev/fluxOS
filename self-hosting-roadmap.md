# fluxOS self-hosting roadmap

*Written 2026-10-28, against commit 8cdd993 plus the toolchain and libc
work described in phases 1 and 2. Phases 0 and 1 are done, phase 2
mostly; the rest is a plan.*

"Self-hosting" means that fluxOS, running on its own kernel, can turn a
checkout of this repository into a bootable `fluxOS.iso` and an installed
disk, with no other operating system involved. That takes a C compiler, an
assembler and a linker, `make` and the handful of tools the Makefile runs,
a C library they can be built against, a filesystem that keeps timestamps
well enough for `make`, and a way to make the ISO and the boot sector.

The compiler is GCC, the same GCC the Linux build uses. It is not
bootstrapped on fluxOS from something smaller; it is built on Linux, first
as a cross compiler that targets fluxOS, then as a GCC that runs on fluxOS
(a "Canadian cross"), and that GCC is shipped on the system image. This is
how most hobby operating systems get a compiler, and it means the source
tree needs no changes: the SSE intrinsics, the atomics, the linker scripts
and `-mfpmath=sse` all stay as they are.

The roadmap has three bars, in order:

1. **Stage 1** - a fluxOS built on Linux builds fluxOS. The native build
   produces a kernel and an ISO that boot and pass the kernel regression
   tests.
2. **Stage 2** - a fluxOS built by that build builds fluxOS again, and the
   result is identical: the same GCC on either side makes byte-for-byte
   comparison of the stripped kernel and programs a real test.
3. **Stage 3** - the GCC that runs on fluxOS can build GCC and binutils
   again on fluxOS, so the tree needs no other machine at all. GRUB's boot
   images stay checked-in blobs with a documented origin, like the Radeon
   microcode.

The GCC build on Linux stays the main build throughout; the native build is
a second way to the same result.

## Where we are

**What the build needs today** (from the Makefile): `gcc -m32
-march=pentium4 -mfpmath=sse`, GNU `ld` with two linker scripts (the
kernel's uses `PHDRS` to force one `PT_LOAD` segment), `ar`, `tar`
(ustar), `gzip`, `truncate`, `mkfs.fat`, `cp`/`mkdir`/`rm`/`mv`/`find`/
`install`/`cat`, `grub-mkimage` and `grub-mkrescue` (which needs `xorriso`)
from a downloaded Debian GRUB 2.06, `grub-file` for `make check`, and
`sha256sum` to verify that download. Python only runs for the tests and for
regenerating assets that are checked in (fonts, icons, sounds, the logo,
the HTML entity table), so it is not a build dependency.

**The tree:** kernel 44k lines, libc 8.5k, fsh 5k, user programs 7k, DOS
layer 10k, GL 5k, desktop and browser 101k, plus third-party C: uACPI 33k,
mbedtls 112k, FreeType 165k, Brotli 15k. About half a million lines of C,
and nine `.S` files.

**What already works in fluxOS's favour:**

- Processes: `fork`, `execve`, `waitpid`, pipes, process groups, job
  control, pseudo-terminals, and since phase 0 real signal handlers
  (`sigaction` with `SA_SIGINFO`, `siginfo`, a context with the FPU state,
  `sigprocmask`, `setjmp`/`longjmp`), `mmap`/`munmap`/`mprotect`, `fcntl`,
  `umask`, `utime`, `times`, `lseek64`, `ftruncate`, `getdents`.
- A libc with stdio (buffered streams, `fdopen`, `getline`), `malloc`
  (an O(1) allocator), `qsort`, `strtol`, `getenv`/`setenv`, `dirent`,
  sockets, DNS, HTTP, TLS, inflate and deflate, ZIP and 7z reading.
- `fsh`, which is also `/bin/sh`, with pipelines, redirections, `$(...)`,
  `if`/`while`/`for`, functions and scripts.
- `wget` (HTTP and HTTPS), `unzip`, `zip`, `7z`: the source tree can reach
  the machine as a GitHub zip over the network and be unpacked natively.
- `mkfs.fat` and `fluxinstall` natively: formatting a disk, writing the
  MBR and GRUB's `boot.img`/`core.img`, and unpacking the system already
  run inside fluxOS. The kernel reads ISO 9660 and ustar.
- Memory: up to 4 GiB of RAM reachable, 2 GiB of address space per
  process, demand-paged `mmap`.
- Since phase 1: a GCC and binutils that know the `i686-fluxos` target,
  a sysroot with fluxOS's headers and libraries, and `make toolchain`.

**What is missing, in hard numbers:**

| Area | Gap |
|---|---|
| A compiler that runs on fluxOS | the cross compiler exists; the native one (phase 4) does not |
| `make`, `tar`, `gzip` (compress), `find`, `install`, `cmp`/`diff`, `sed` | none (`cp mkdir rm mv cat` exist) |
| libc | since phase 2, enough for GNU make and binutils to build; POSIX threads, interval timers, signal stacks and sessions since 2026-10-29; still no real links |
| Filesystem | FAT32 only on disk: 2 s modification times (a target built within 2 s of editing its source looks up to date to `make`), no symbolic or hard links, cosmetic permissions, case-insensitive names |
| Kernel limits | 128 processes and threads together; `PATH_MAX` 4096 since 2026-10-29 (descriptors, memory regions, stack and pipes raised in phase 2) |
| Boot media | ISO 9660 writing, El Torito, GRUB's images |

## Decisions

**GCC by cross build, not TCC.** A first draft of this roadmap took the
Tiny C Compiler as the bootstrap compiler because it needs little from a
libc and compiles itself in seconds. The price was high on the fluxOS
side: 23 files use SSE intrinsics TCC lacks, the kernel's `PHDRS` linker
script needs a linker of our own, floating point would leave SSE, and GCC
would still have to come later. Building GCC as a cross compiler and then
as a native one costs nothing in the tree and only libc work, which has
to happen anyway. The costs that remain are real: a static `cc1` is
30 to 40 MB, a full build of the tree with GCC on a Pentium 4 takes an
hour or more, and GCC rebuilding itself natively (stage 3) needs C++,
`libstdc++`, autotools and a POSIX shell, which is why stage 3 is last.

**No linker or assembler of our own.** binutils cross-builds for
`i686-fluxos` with a 48-line patch; the native build gets the same `as`,
`ld` and `ar`.

**GRUB stays, as a blob.** GRUB's own build (autotools, bison, flex,
gettext, Python) is out of scope. `boot.img`, `core.img` and the El Torito
image come from the same Debian 2.06 packages `tools/fetch-grub.sh`
verifies today and get checked into `firmware/grub/` with their hashes and
package names, as `firmware/radeon` does for the microcode. What we write
ourselves is the ISO: a small `mkiso` (ISO 9660 with Rock Ridge names and
an El Torito no-emulation entry) is a few hundred lines and the kernel's
`iso9660.c` is the reference for the layout.

**The filesystem for a build tree is ext2**, read and write, mounted on a
second partition or disk; FAT32 stays the system's root. ext2 gives 1 s
timestamps, permissions, hard and symbolic links and case-sensitive names,
and it is the simplest format that has all four.

**No dynamic linking.** GCC, binutils and make are built static
(`--disable-shared`), and that stays out of this roadmap. Threads arrived
on 2026-10-29 for the desktop's sake (kernel threads with futexes, POSIX
threads in libc); the toolchain is still built `--disable-threads`, which
keeps the Canadian cross simpler, but `libstdc++`'s `gthr` has something
to stand on now.

## Phase 0: the POSIX floor (done)

Commits 77b148c and 21786ca, October 2026. Real signal delivery (`sigaction`,
`SA_SIGINFO`, `SA_RESTART`, `SA_RESETHAND`, a signal context with the FPU
image and `sigreturn`), `sigprocmask`, `setjmp`/`longjmp`/`sigsetjmp`,
`mmap`/`munmap`/`mprotect` with demand paging and copy-on-write, `fcntl`,
`umask`, `utime`, `times`, `<sys/resource.h>`, `<sys/time.h>`,
`<sys/times.h>`, the streaming ELF loader, and a libc allocator rewrite.

## Phase 1: the `i686-fluxos` target and the cross toolchain (done)

*Goal: GCC and binutils know fluxOS as a target, so that
`i686-fluxos-gcc hello.c` produces a program the kernel runs, and the
same patches serve the native build later.*

Done 2026-10-28:

- `tools/toolchain/patches/binutils-2.42-fluxos.patch`: `config.sub`,
  `bfd/config.bfd`, `gas/configure.tgt`, `ld/configure.tgt` (ELF, the
  `elf_i386` emulation).
- `tools/toolchain/patches/gcc-13.2.0-fluxos.patch`: `config.sub`,
  `gcc/config.gcc` (`i386/unix.h i386/att.h elfos.h glibc-stdint.h
  i386/i386elf.h fluxos.h`, GCC's own `stdint.h`), `libgcc/config.host`
  (`crtbegin.o`, `crtend.o`), and the new `gcc/config/fluxos.h`: it
  defines `__fluxos__` and `__unix__`, links `crt0.o`, `-lc` and
  `libgcc` statically, and puts the text segment at 0x40000000 so that
  a program linked with no linker script of its own is still one the
  kernel accepts. (The tree's own programs keep `-T user/user.ld`, which
  the desktop's garbage collector needs for its `__gc_data_*` symbols.)
- `make sysroot`: `build/sysroot/usr/include` (the libc headers and
  `flux/`), `usr/lib/{libc.a, libGL.a, libfont.a, crt0.o, fluxos.ld}`.
- `make toolchain` (`tools/toolchain/build.sh`): fetches binutils 2.42
  and GCC 13.2.0 from gnu.org with pinned SHA-256 sums, GMP/MPFR/MPC/ISL
  through `download_prerequisites`, patches, configures
  (`--target=i686-fluxos --with-sysroot --enable-languages=c
  --disable-shared --disable-threads --disable-fixincludes
  --with-arch=pentium4`) and installs `build/toolchain/cross/bin/
  i686-fluxos-{gcc,as,ld,ar,...}` with `libgcc.a`. binutils takes a
  minute on an eight-core machine; GCC with its libraries about fifteen.
- The Makefile accepts the cross tools as it is:
  `make BUILD=build-cross CC=.../i686-fluxos-gcc LD=.../i686-fluxos-ld
  AR=.../i686-fluxos-ar` builds the same kernel and programs.

What phase 1 does not do: run anything on fluxOS. It is the foundation
for phases 2 and 4.

## Phase 2: the libc GNU programs can stand on (mostly done)

*Goal: binutils, GCC, GNU make and the tools of phase 3 compile against
fluxOS's libc, with `./configure --host=i686-fluxos` and no source
patches beyond what their own `configure` can be told. Measured by
`configure` completing for binutils and make, then by the Canadian cross
of phase 4 linking.*

**Measured on 2026-10-28:** GNU make 4.4.1 and all of binutils 2.42 (`as`,
`ld`, `ar`, `nm`, `objdump`, `objcopy`, `readelf`, `strip`, `size`) configure
with `--host=i686-fluxos` and build into static fluxOS programs, with no
source changes. Their `configure` scripts now miss only what a cross build
cannot test by running (`mmap`, `strcoll`). None of it has run on fluxOS
yet: that is `make test-libc-qemu` and phase 4.

Done:

- [x] `printf` family rewritten (`user/libc/printf.c`): every conversion
      and size (`%lld`, `%zu`, `%jd`, `%hhd`, `%n`, `%m`, `*` widths), and
      floating point (`%f %e %g %a`, `L` for long double) exact, by a
      big-integer decimal expansion rounded half to even, as glibc. Also
      `asprintf`, `vasprintf`, `vdprintf`.
- [x] `strtod`, `strtof` correctly rounded (a long double guess checked
      against the exact value with big integers), `strtold`, hexadecimal
      floats, `inf`/`nan`; `strtol` and friends with overflow (`ERANGE`),
      `strtoll`, `strtoull`, `strtoimax`, `strtoumax`, `atoll`, `atof`.
- [x] `scanf`, `sscanf`, `fscanf` and the `v` forms, with `%[...]`, `%n`,
      `*`, widths, sizes and `%ms`.
- [x] stdio rewritten: `fseek`/`ftell`/`fseeko`/`ftello`/`rewind`/`fgetpos`/
      `fsetpos` (there were none), reading and writing one stream in turn,
      bulk `fread`/`fwrite`, `getdelim`, `freopen`, `tmpfile`, `tmpnam`,
      `tempnam`, `popen`/`pclose`, `fmemopen`, `open_memstream`, `setbuf`,
      `setlinebuf`, the `_unlocked` forms, eight bytes of `ungetc`.
- [x] `getopt`, `getopt_long`, `getopt_long_only` (glibc's permutation);
      `bsearch`; `stpcpy`, `stpncpy`, `memrchr`, `mempcpy`, `memccpy`,
      `memmem`, `strchrnul`, `strsep`, `strcoll`, `strxfrm`, `strerror_r`,
      `<strings.h>`; `err`/`warn` and their `v` forms; `psignal`,
      `sys_siglist`, messages for every errno.
- [x] `popen`, `system`; `mkstemp`, `mkostemp`, `mkstemps`, `mkdtemp`,
      `mktemp`; `realpath`, `basename`, `dirname`, `fnmatch`, `glob`.
- [x] `nanosleep`, `clock_gettime`/`clock_getres`/`clock_nanosleep`
      (realtime, monotonic, CPU), `mktime`, `timegm`, `localtime_r`,
      `difftime`, `asctime`/`ctime` (and `_r`), `tzset` (fixed offsets),
      `tzname`/`timezone`, dates before 1970, `strftime` with the ISO week
      (`%G %V %u`), `%U %W %C %s %z %c %x %X %r %k %l %P` and modifiers.
- [x] `getrlimit`/`setrlimit` (reporting what the kernel enforces),
      `sysconf`, `pathconf`, `getpagesize`, `gethostname`, `getloadavg`.
- [x] `select`/`pselect` over `poll`, `wait3`/`wait4`, `setsid`/`getsid`
      (a process group), `execl`/`execlp`/`execle`/`execvpe`, `vfork`,
      `killpg`, `sigsuspend`, `siginterrupt`, `pread`/`pwrite`,
      `readv`/`writev`, `flock` and `fcntl` record locks (granted).
- [x] "C" locale: `setlocale`, `localeconv`, `nl_langinfo`, the
      `<wchar.h>`/`<wctype.h>` functions (one byte is one character),
      `mbtowc` and friends.
- [x] `<regex.h>`: BRE and ERE with the GNU additions (`\+ \? \|`,
      back-references, `\w \s \b \< \>`), leftmost-longest within a step
      budget.
- [x] `getpwnam`/`getpwuid` (and `_r`, `getpwent`), `getgrnam`/`getgrgid`
      from `/etc/passwd` and `/etc/group`, or the one user without them.
- [x] `<termios.h>` over the kernel's line discipline; `memalign`,
      `posix_memalign`, `aligned_alloc`, `valloc`, `reallocarray`;
      `dlopen` stubs that fail with a message.
- [x] Headers: `pwd.h grp.h locale.h langinfo.h wchar.h wctype.h regex.h
      fnmatch.h glob.h getopt.h libgen.h err.h alloca.h memory.h strings.h
      ar.h paths.h sysexits.h sched.h dlfcn.h termios.h endian.h byteswap.h
      sys/select.h sys/param.h sys/file.h sys/uio.h`, `_POSIX_VERSION`,
      the `S_I*` and `_SC_*` constants, POSIX's types (`dev_t`, `ino_t`...).
- [x] Kernel: `struct stat` has `st_dev`, `st_uid`, `st_gid`, `st_blksize`,
      `st_blocks`, `st_atime`, `st_ctime`; `fchdir` and `fchmod` system
      calls; 256 descriptors per process (was 32), 256 memory regions (was
      64), a 32 MiB stack (was 8; 64 would collide with the Radeon GL
      window), 64 KiB pipes (was 4 KiB); more errno values (`ELOOP`,
      `EILSEQ`...).
- [x] The libc carries the 64-bit division helpers GCC calls on i386
      (`__udivdi3` and the rest, weak so that the real libgcc wins).
- [x] Tests: `make test-libc` checks printf, strtod/strtof, strtol, sscanf,
      regex, fnmatch, getopt and the calendar functions against glibc on
      the host, compiled against the fluxOS headers (1.3 million cases, all
      equal; glibc's `%#g` misprint of values rounding up to a power of ten
      is the one known difference). `make test-libc-qemu` runs the
      in-system part (stdio on files, popen, system, 256 descriptors, pipes,
      select, nanosleep, realpath, glob) as init in QEMU.

Left (the rest done 2026-10-29, built and host-tested, not yet run on the
real kernel: `make test-libc-qemu` has the checks):

- [x] `make test-libc-qemu` passes on the real kernel (2026-10-28), and a
      system built with this libc installs, boots and runs the desktop.
- [x] The cross-built `make`, `as`, `ld`, `ar`, `nm`, `objcopy`, `objdump`,
      `readelf`, `strip`, `size`, `strings` and `ranlib` are on the system
      image in `/usr/bin` (`make ports`, `tools/ports/build.sh`, 11 MB
      stripped). Running them there is the next check.
- [x] `PATH_MAX` is 4096: a system call's paths are copied into up to
      three heap buffers of the calling thread's (`user_path` in
      `syscall.c`), and the VFS's own path buffers are on the heap too.
- [x] `link`, `symlink` and `readlink` answer as Linux does on vfat
      (`EPERM`, `EINVAL`): real ones come with ext2 in phase 5.
- [x] Interval timers: `setitimer`/`getitimer` for `ITIMER_REAL`
      (`SIGALRM`, from the timer tick), `ITIMER_VIRTUAL` (user time,
      `SIGVTALRM`) and `ITIMER_PROF` (`SIGPROF`), in 10 ms steps; `alarm`
      over them. `sigpending` from the kernel; `sigaltstack` with
      `SA_ONSTACK` handlers run on it.
- [x] Sessions: `setsid` starts one, drops the controlling terminal and
      keeps `exec` from taking another; `getsid`; `setpgid` refuses a
      session leader and a group of another session.

## Phase 3: make and the tools the Makefile runs

*Goal: every command the Makefile invokes exists on fluxOS and behaves
the way the Makefile uses it. Measured by running the Makefile's
`rootfs`, `system.tar` and `tests/` targets natively with the cross-built
compiler objects, before phase 4 lands.*

- [x] GNU make 4.4, cross-built with `./configure --host=i686-fluxos` (it
      needs only the phase 2 libc, no `getloadavg`); on the image since
      2026-10-29 (`make ports`), still to be run there. The Makefile uses
      `$(wildcard)`, `$(patsubst)`, `$(foreach)`, `$(shell)`,
      target-specific variables and `-MMD` includes throughout, so a
      smaller make is not an option.
- [ ] `tar` (ustar create, extract, list; `--format=ustar --owner=0
      --group=0 --numeric-owner -cf`, `-C`, `-x`), reusing the kernel's
      `initrd.c` parser on the reading side.
- [ ] `gzip`/`gunzip`/`zcat`: deflate already exists in `zip`'s code
      (`user/libc/arc_codec.c`); add the gzip header and trailer.
- [ ] `find` (`-type f`, `-name`, `-print`, `-newer`), `install` (`-d`,
      `-m`, copying), `cmp`, `diff` (unified; needed by the tests and by
      anyone editing), `patch` (unified; `tools/toolchain/build.sh` runs
      it), `sed` (`s///`, `-i`, `-n`, `p`, `d`, addresses), `cut`,
      `xargs`, `truncate`, `dd` (`if= of= bs= count= seek=`), `sha256sum`,
      `ln` (hard and symbolic, after phase 5), `curl` or `wget -O`
      (exists), `nproc`.
- [ ] `fsh` in `/bin/sh` mode: when started as `sh`, POSIX behaviour
      where the two differ (no `AUTO_CD`, no history expansion, word
      splitting of unquoted `$var`, `set -e`, `trap`, `getopts`, `"$@"`,
      `exec`, `.` as `source`, `[ ]`/`test` builtins, `printf` builtin
      with `%d %s %x`, here documents). GNU make runs every recipe line
      through `/bin/sh -c`, and `configure` scripts need all of it.
- [ ] `grub-file --is-x86-multiboot` becomes `mbcheck` (phase 6);
      `grub-mkimage`'s output becomes a checked-in `core.img` (phase 6).

Size: GNU make is third-party; the tools 2k to 3k lines; the `sh` mode
1k in `fsh`. Risk: low.

## Phase 4: GCC and binutils that run on fluxOS

*Goal: `/usr/bin/gcc` on fluxOS compiles and links a program, and the
system image carries the whole toolchain. Measured by `gcc -O2 hello.c`
in a fluxOS terminal, then by `make` of the fluxOS tree natively (phase
7's stage 1).*

- [ ] The Canadian cross: with `build/toolchain/cross` on the path,
      binutils then GCC configured `--build=x86_64-linux-gnu
      --host=i686-fluxos --target=i686-fluxos --prefix=/usr
      --with-sysroot=/ --enable-languages=c --disable-shared
      --disable-threads --disable-nls --disable-plugin --disable-lto
      --disable-libssp --disable-libquadmath --disable-fixincludes`, GMP,
      MPFR, MPC and ISL in-tree, everything static. `tools/toolchain/
      build.sh --native` does it, after the cross build.
- [ ] The parts that run as processes: the `gcc` driver starts `cc1`,
      `as`, `collect2`/`ld` through libiberty's `pex-unix` (`fork`,
      `execvp`, `pipe`, `waitpid`, `dup2`: exist) and temporary files
      (`mkstemp`, `TMPDIR`: phase 2). `collect2` reads `nm` output
      through a pipe; `--disable-lto` keeps `lto-wrapper` out.
- [ ] Memory and limits: `cc1` on the desktop's biggest files needs a few
      hundred MB of heap (the `sbrk` region is bounded by the lowest
      `mmap`, which the libc's `malloc` could switch to `mmap` chunks for
      large requests) and a deep stack (phase 2's 64 MiB).
- [ ] Packaging: `/usr/bin/{gcc, cc, as, ld, ar, ranlib, nm, objcopy,
      strip, readelf}`, `/usr/libexec/gcc/i686-fluxos/13.2.0/{cc1,
      collect2}`, `/usr/lib/gcc/i686-fluxos/13.2.0/{libgcc.a, crtbegin.o,
      crtend.o, include/}`, `/usr/include` and `/usr/lib` from the
      sysroot. About 60 MB stripped: on the installed disk (`system.tar`),
      not in the CD's in-memory setup system. `fluxinstall` already
      unpacks `system.tar`, so nothing changes there.
- [ ] The specs: `/usr/lib/fluxos.ld` is the default linker script of the
      native `gcc` (phase 1's `-Ttext-segment` default serves until then),
      `-static` implied, `-lgcc` and `-lc` found in `/usr/lib`.
- [ ] A smoke test in `tests/`: compile and run a program with the native
      GCC under QEMU (`make test-gcc-qemu`), comparing its output and the
      binary with the cross-built one.

Size: no code of ours beyond scripts and packaging; the work is in phase
2's libc and in whatever the Canadian cross turns up. Risk: medium; GCC's
configure has its own ideas about hosts it has not met, and each one is a
small fix in `config.gcc`/`config.host` or a cache variable passed to
`configure`.

## Phase 5: a filesystem for a build

*Goal: `make` is correct on fluxOS (no spurious "up to date"), links work,
`cp -r` of the tree keeps everything. Measured by the native build of
phase 7 producing the same file list as the Linux build.*

- [ ] ext2 read/write driver (`kernel/fs/ext2.c`): superblock, block
      groups, bitmaps, inodes with direct, indirect and double-indirect
      blocks, directories (linked list, no htree), symbolic links (fast
      and slow), hard links, `mtime`/`ctime`/`atime`, permission bits,
      `rename` within and across directories, and the same write-back
      sector cache FAT uses. Revision 1 only: no journal (ext3) and no
      extents (ext4), but the feature flags checked so an ext4 volume is
      refused rather than damaged.
- [ ] `mkfs.ext2` natively (and `make disk DISKFS=ext2` on the host),
      `fsck`-lite (counts only), `mount -t ext2`.
- [ ] VFS: symbolic link resolution in `vfs_resolve` (with a depth limit),
      `lstat`, `readlink`, `link`, `symlink`, `chmod`/`chown` that persist,
      `S_IFLNK` in `getdents`.
- [ ] `fluxinstall` and the automounter recognise ext2 volumes; the
      installer keeps FAT32 for `/` (GRUB reads it, and the Setup CD's
      tools expect it) and can make a second ext2 partition for `/src`
      or `/home`.
- [ ] Timestamps: the FAT driver uses the 10 ms create-time field as the
      modification time where it can, so the system partition is at least
      `make`-safe for small edits.

Size: ext2 about 2.5k lines, VFS and tools 1k. Risk: medium; a write
driver needs the kind of testing `tests/kernel-storage.c` does for FAT,
and that test grows an ext2 image.

## Phase 6: boot media made natively

*Goal: `mkiso` on fluxOS produces an ISO that boots in QEMU and on the
HP Pavilion g6, and `fluxinstall` installs from a native build.*

- [ ] `firmware/grub/`: `boot.img`, `core.img` (built on Linux by
      `grub-mkimage -O i386-pc -p '(,msdos1)/boot/grub' biosdisk
      part_msdos fat`), the CD's `eltorito.img` (`-p '/boot/grub'
      biosdisk iso9660`), and the `i386-pc` modules the two menus load,
      each with its SHA-256 and the Debian package it came from.
      `tools/fetch-grub.sh` becomes the way to refresh them, not a build
      step.
- [ ] `mkiso`: ISO 9660 level 1 with Rock Ridge (`RR`, `PX`, `NM`), a
      primary volume descriptor, path tables, El Torito boot record
      pointing at `eltorito.img` (no emulation, 4 sectors), `boot/grub/`
      with the modules, `grub.cfg`, the kernel, `setup.tar` and
      `system.tar`. The kernel's `iso9660.c` reads the result as a test.
- [ ] `make iso` on fluxOS: the Makefile's ISO rule written in terms of
      `mkiso` and the firmware directory, with the Linux build keeping
      `grub-mkrescue` as an alternative behind `MKRESCUE=`.
- [ ] `mbcheck` (Multiboot header validation) and a `kernel-elf-check`
      (one `PT_LOAD`, entry inside it) as native tools run by `make check`.
- [ ] Setup CD: `fluxinstall` reads `boot.img`/`core.img` from
      `firmware/grub` on the CD instead of `boot/install/`.

Size: `mkiso` 800 lines, tooling 300. Risk: low; the formats are simple
and the loader side exists.

## Phase 7: the bootstrap

*Goal: stages 1 and 2. A documented procedure and a test that proves it.*

- [ ] A source disk: `make srcdisk` makes an ext2 image with the checkout
      (or `wget https://github.com/.../archive/refs/heads/main.zip &&
      unzip` on the machine).
- [ ] `tools/selfhost.sh`, run inside fluxOS: `make` with the native
      tools, `make check`, `make iso`, then `fluxinstall` to a second
      disk; the same script on Linux drives QEMU for CI:
      `make test-selfhost-qemu` boots the Linux-built ISO with the source
      disk, runs the native build, boots the native ISO, runs
      `test-kernel-qemu`'s suite there, and compares the stripped kernel
      and every program of stage 1 and stage 2 byte for byte (the same
      GCC built the same way should agree; `-g` paths are mapped with
      `-fdebug-prefix-map`, and `__DATE__` is not used anywhere).
- [ ] Build time and memory on the reference machines (a Core 2 Duo in
      QEMU, the HP Pavilion g6, the Pentium 4 floor): FreeType, mbedtls and
      the browser are the long poles; `make -j` needs nothing new since
      processes already run on all cores.
- [ ] Documentation: a "Building fluxOS on fluxOS" section in the README
      and the firmware provenance.

Size: scripts and docs. Risk: whatever the previous phases left behind.

## Order and dependencies

```
Phase 1 (cross toolchain, done) ──> Phase 2 (libc) ──┬──> Phase 3 (make, tools, sh)  ──┐
                                                     └──> Phase 4 (native gcc, binutils) ┼──> Phase 7 (bootstrap)
Phase 5 (ext2, links, timestamps) ───────────────────────────────────────────────────────┤
Phase 6 (mkiso, GRUB blobs) ─────────────────────────────────────────────────────────────┘
```

Phases 5 and 6 are independent of everything else and of each other.
Phase 2 comes first among the rest because 3 and 4 are ports that need
it, and its first item should be the configure probe, which turns the
libc list into an ordered one.

Suggested order: 4 (the Canadian cross: binutils already links for
fluxOS), 3 (GNU make already builds), 6, 5, 7; and running the phase 2
tests on fluxOS before relying on any of it.

## Later, deliberately

- **Stage 3, GCC building GCC on fluxOS.** GCC has been C++ since 4.8,
  so this needs `g++` and `libstdc++` in the Canadian cross
  (`--enable-languages=c,c++`), which in turn needs a libc with wide
  characters, locale stubs, `pthread` stubs for `libstdc++`'s `gthr`,
  and more memory, plus autotools' `configure` running under `fsh` (or a
  port of `bash`), `sed`, `awk`, `grep`, `expr`, `m4` and `bison` for
  binutils. Hours of build time on the target machines. Everything in
  phases 2 to 7 makes it closer; nothing in them depends on it.
- **Dynamic linking** (`ld.so`, PIC, `dlopen`): nothing on the roadmap
  needs it; GCC plugins would.
- **Threads in the toolchain:** fluxOS has threads since 2026-10-29, but
  make `-j` and the compilers do not need them; the ports stay
  `--disable-threads` until `libstdc++` wants `gthr` (stage 3).
- **Networking in the loop:** a native `git` is a large port (or a small
  `git clone`/`pull` over HTTPS with the smart protocol, ~3k lines); the
  GitHub zip through `wget` and `unzip` serves until then.
- **A fluxOS boot loader** instead of GRUB's blobs: an MBR, a FAT32
  stage that loads a Multiboot ELF and sets the VESA mode, and the ISO
  equivalent. Nice for purity, not required as long as the blobs'
  provenance is documented.

## Appendix: what the cross toolchain settles

Counted on 2026-10-28 over `kernel`, `lib` and `user`, these were the
reasons the TCC plan needed tree changes; with GCC none of them does:

| Construct | Files |
|---|---|
| `_mm_*` intrinsics (`<xmmintrin.h>`, `<emmintrin.h>`, `<mmintrin.h>`) | 23 |
| `__atomic_*` | 13 |
| `__builtin_ctz`/`clz`/`popcount` | 7 |
| `__builtin_bswap*`, `__builtin_frame_address`, `_Static_assert`, `__builtin_unreachable` | 7 |
| Linker scripts with `PHDRS` | 2 |
| `.incbin` (`radeon_fw.S`) | 1 |
| Third-party inline assembly (mbedtls `bn_mul.h`, FreeType `ftcalc.h`, Brotli `platform.h`, uACPI `atomic.h`) | 4 |
