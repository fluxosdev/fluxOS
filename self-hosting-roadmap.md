# fluxOS self-hosting roadmap

*Written 2026-10-28, against commit 8cdd993 plus the toolchain and libc
work described in phases 1 and 2. Phases 0 to 6 are done (phase 3 and 4
on 2026-10-29: the Makefile's tools, `fsh` as `/bin/sh`, and GCC running
on fluxOS; phases 5 and 6 on 2026-10-30: ext2 with links, and the ISO made
natively); phase 7 is a plan.*

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
| A compiler that runs on fluxOS | done (phase 4, 2026-10-29): GCC 13.2.0 with C and C++, 72 MB on the image |
| `make`, `tar`, `gzip` (compress), `find`, `install`, `cmp`/`diff`, `sed` | done (phase 3, 2026-10-29), with `patch`, `cut`, `xargs`, `truncate`, `dd`, `ln`, `mktemp`, `nproc`, `printf`, `test`, `true`, `false` |
| libc | since phase 2, enough for GNU make and binutils to build; POSIX threads, interval timers, signal stacks and sessions since 2026-10-29; C++-clean headers, C99 math, `getaddrinfo`, `.init_array` constructors and `__cxa_atexit` for GCC and libstdc++ (2026-10-29); still no real links |
| Filesystem | done (phase 5, 2026-10-30): ext2 read/write with symbolic and hard links, permissions and 1 s times, on a second partition (`fluxinstall -e /src`) or any disk; FAT32 stays the system's root, now with 1 s modification times |
| Kernel limits | 128 processes and threads together; `PATH_MAX` 4096 since 2026-10-29 (descriptors, memory regions, stack and pipes raised in phase 2) |
| Boot media | done (phase 6, 2026-10-30): `mkiso` (ISO 9660, Rock Ridge, El Torito, hybrid MBR) and GRUB's images checked in |

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

**C++ came early.** GCC has been C++ since 4.8, so `cc1` for fluxOS is C++
code: the Canadian cross of phase 4 needs a cross `g++` and a `libstdc++`
built for fluxOS before anything else, not in stage 3 as first planned.
That turned out to cost a day, not a phase: libc headers that are
C++-clean (`extern "C"`), the rest of C99's `<math.h>`, `<sys/un.h>` and
`getaddrinfo` (libcody, GCC's module mapper, wants them), `struct stat`
with `off_t` and `time_t` fields, constructors run from `.init_array` by
the C library's startup code (the toolchain is configured
`--enable-initfini-array`; there is no `crti.o`), `.eh_frame` kept by the
linker script, `__cxa_atexit`, and `--disable-tls` (the libc has no static
TLS, so `__thread` is emulated through libgcc). libstdc++ itself needed one
line in its `crossconfig.m4` (and the generated `configure`): fluxOS is
configured like a Linux host, since the checks are link tests against
`libc.a`. Both compilers are built with C and C++, so stage 3 lacks only
the autotools side.

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
      stripped), on the default `PATH` (`/bin:/usr/bin`). `make --version`
      and `as --version` run on fluxOS, and on 2026-10-29 make built a
      program there: its Makefile ran `as` and `ld`, and the result ran
      (a write-and-exit `hello.s`, no libc).
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

## Phase 3: make and the tools the Makefile runs (done)

*Goal: every command the Makefile invokes exists on fluxOS and behaves
the way the Makefile uses it. Measured by running the Makefile's
`rootfs`, `system.tar` and `tests/` targets natively with the cross-built
compiler objects, before phase 4 lands.*

Done 2026-10-29. The tools are checked on the host against the GNU
originals (`make test-tools`, `tests/tools-host.sh`: 308 checks: archives
and compressed files each side can read, diffs each side's `patch`
applies, the same output for the same scripts), and `fsh`'s `sh` mode
against dash (`make test-sh`, `tests/sh-host.sh`: 135 scripts). They are
built for fluxOS and on the image; what has not happened yet is running
the Makefile's targets natively (phase 7).

- [x] GNU make 4.4, cross-built with `./configure --host=i686-fluxos` (it
      needs only the phase 2 libc, no `getloadavg`); on the image since
      2026-10-29 (`make ports`) and starts there. The Makefile uses
      `$(wildcard)`, `$(patsubst)`, `$(foreach)`, `$(shell)`,
      target-specific variables and `-MMD` includes throughout, so a
      smaller make is not an option.
- [x] `tar` (`user/bin/tar.c`): ustar create, extract and list,
      `--format=ustar --owner=0 --group=0 --numeric-owner -cf`, repeated
      `-C` in both directions, `-z` through a child `gzip` (and gzip's
      magic recognised without `-z`, pipes included), `--strip-components`,
      `-k`, `-O`; reads old, GNU (`L`/`K` long names) and pax archives.
      Members in name order, so the same tree gives the same archive.
- [x] `gzip`/`gunzip`/`zcat`: Deflate compression moved out of `zip` into
      libc as a stream (`<deflate.h>`, `user/libc/deflate.c`: LZ77 over
      hash chains, fixed Huffman codes, a block every 256 KiB, levels 1 to
      9 and stored blocks at 0) and the archive codecs' inflate made
      readable from a pipe; `gzip.c` adds the container, `-c -d -f -k -l
      -n -t -v`, concatenated members. Fixed codes cost some ratio against
      GNU gzip (dynamic codes are the obvious next step).
- [x] `find` (`-name -iname -path -type -newer -mtime -size -empty -prune
      -print -print0 -delete -exec ... \; and +`, `-maxdepth`, `! -o -a ( )`),
      `install` (`-d -m -D -p -s -t`), `cmp` (`-l -s`), `diff` (Myers in
      linear space; normal and unified output, `-r -N -q -s -i -b -w -B
      -a`), `patch` (unified and normal diffs, fuzz, offsets, `-p -R -N -i
      -d -b --dry-run`, new and deleted files, rejects to `.rej`), `sed`
      (every address form, `s///` with `g p N i w m`, `y`, `a i c`, `b t
      T :`, `n N p P D`, the hold space, `l`, `q Q`, `r w`, `{}`, `-n -e -f
      -E -i -s`; the libc's BRE and ERE), `cut`, `xargs` (`-0 -n -s -I -r
      -t`), `truncate`, `dd`, `ln` (hard and symbolic: the calls exist and
      FAT refuses them until phase 5), `mktemp`, `nproc`, `printf`,
      `test`/`[` (shared with the shell: `user/fsh/testexpr.c`), `true`,
      `false`; `sha256sum` and `wget -O` existed.
- [x] `fsh` in `/bin/sh` mode (`sh.posix`, when started as `sh` or with
      `--posix`): unquoted `$var` and `$(...)` split on `$IFS`, patterns
      that match nothing stay as they are, no brace expansion, no
      `AUTO_CD`, no startup files, `"$@"` with no parameters is no word;
      and for both shells: `case`/`esac`, here documents (`<<`, `<<-`,
      quoted delimiters), `${var#pat}` and the other three, `${var:?}`,
      `set -e -u -x -f -n` (and `-o`), `trap` (with `EXIT`), `getopts`,
      `local`, `command`, `umask`, `readonly`, `unset -f`, `<>`, bundled
      flags (`sh -ec`, `sh -c cmd name args`, `sh -n`), and a `test` with
      every POSIX operator. GNU make runs every recipe line through
      `/bin/sh -c`, and `configure` scripts need all of it.
- [ ] `grub-file --is-x86-multiboot` becomes `mbcheck` (phase 6);
      `grub-mkimage`'s output becomes a checked-in `core.img` (phase 6).

Size: GNU make is third-party; the tools came to 6k lines (`diff`,
`patch` and `sed` are 600 to 900 each), the `sh` mode 1k in `fsh`.

## Phase 4: GCC and binutils that run on fluxOS (done)

*Goal: `/usr/bin/gcc` on fluxOS compiles and links a program, and the
system image carries the whole toolchain. Measured by `gcc -O2 hello.c`
in a fluxOS terminal, then by `make` of the fluxOS tree natively (phase
7's stage 1).*

Done 2026-10-29: `make toolchain-native` builds GCC 13.2.0 to run on
fluxOS and the next `make` puts it on the system image. `make
test-gcc-qemu` passes on the kernel (2026-10-29): `gcc -O2` compiles,
assembles and links `hello.c` in 1.6 s under QEMU, the program runs
with its constructor, the stripped program is byte for byte the cross
compiler's (the symbol tables differ only by the source's name), a
compile error comes back as one, `-E`, `-S`, `as` and a driver link of
an object work, and `make` drives it through a recipe under `/bin/sh`.
Two things the first run taught: the kernel heap (where an initrd's
files live) was capped at 64 MiB and is now a quarter of RAM up to
256 MiB, and the stage 2 comparison is of stripped programs.

- [x] The Canadian cross (`tools/toolchain/build.sh --native`): with the
      cross toolchain on the path, GCC configured `--build=x86_64-pc-linux-gnu
      --host=i686-fluxos --target=i686-fluxos --prefix=/usr --with-sysroot=/
      --with-build-sysroot=build/sysroot --enable-languages=c,c++
      --disable-shared --disable-threads --disable-tls --enable-initfini-array
      --disable-nls --disable-plugin --disable-lto --disable-libssp
      --disable-libquadmath --disable-libstdcxx-pch --disable-fixincludes`,
      GMP, MPFR, MPC and ISL copied into the tree (with their timestamps,
      or autotools try to regenerate themselves) and given fluxOS's
      `config.sub`, everything static. binutils come from `make ports`
      as before (`as`, `ld` and the rest in `/usr/bin`, found on `PATH`).
      Twenty minutes on eight cores; `--continue` picks a build up after
      a libc fix without configuring again.
- [x] What the Canadian cross turned up, all in the libc or the patches:
      C++-clean headers, `<math.h>`, `<sys/un.h>`, `getaddrinfo`,
      `struct stat` types, constructors, `__cxa_atexit`, `--disable-tls`,
      libstdc++'s host configuration (see *Decisions*). GCC's own
      `configure` needed no cache variables beyond `gcc_cv_func_mmap_*=no`:
      the compiler's page allocator takes its pages from `malloc` (the
      kernel keeps 256 mappings per process; the heap can grow to 2 GiB).
- [x] The parts that run as processes: the `gcc` driver starts `cc1`,
      `as`, `collect2`/`ld` through libiberty's `pex-unix` (`fork`,
      `execvp`, `pipe`, `waitpid`, `dup2`) and temporary files (`mkstemp`,
      `TMPDIR`); `collect2` reads `nm` output through a pipe.
      `--disable-lto` keeps `lto-wrapper` out (the install still makes
      one; the script deletes it).
- [x] Memory and limits: the heap grows by `sbrk` up to the lowest
      mapping, and mappings are placed from the top of the 2 GiB address
      space down, so `cc1` has well over a gigabyte to grow into; the
      stack is 32 MiB (the kernel's maximum). The `mmap`-for-large-blocks
      change to `malloc` was not needed.
- [x] Packaging: `/usr/bin/{gcc, cc, cpp, g++, c++}`,
      `/usr/libexec/gcc/i686-fluxos/13.2.0/{cc1, cc1plus, collect2}`,
      `/usr/lib/gcc/i686-fluxos/13.2.0/{libgcc.a, crtbegin.o, crtend.o,
      include/}` in `build/toolchain/native/root`, and `/usr/include` and
      `/usr/lib` straight from the sysroot (`system.tar` takes both
      directories when the native `gcc` exists). 72 MB stripped with C++
      (`cc1` 31 MB, `cc1plus` 33 MB): on the installed disk, not in the
      CD's in-memory setup system; `fluxinstall` unpacks `system.tar` as
      before. `libstdc++` for programs compiled on fluxOS is on the image
      too since 2026-10-29: the cross toolchain's, without debug information
      (3.4 MB), in `/usr/lib`, with its headers in
      `/usr/include/c++/13.2.0` (14 MB), where the native `g++` looks; the
      package is 89 MB with them. `make test-gcc-qemu` compiles and runs a
      C++ program with iostream, containers, exceptions and static objects.
- [x] The specs (`gcc/config/fluxos.h`): `/usr/lib/fluxos.ld` (the
      sysroot's copy of `user/user.ld`) is the default linker script of
      both compilers when it exists, found through the startfile search
      (`%:if-exists-then-else(fluxos.ld%s -T)`: the spec functions take
      space-separated arguments), with `-Ttext-segment=0x40000000` as the
      fallback; `-static` implied, `-lgcc`, `-lc` (and `-lm`, which `g++`
      adds: an empty archive in the sysroot) found in `/usr/lib`;
      `__cxa_atexit` for static destructors.
- [x] A smoke test in `tests/`: `make test-gcc-qemu` boots the kernel
      with the system image plus `tests/gcc-hello.c` and its cross-built
      binary, runs `tests/gcc-guest.c` as init, which compiles the program
      with `/usr/bin/gcc -O2`, runs it, checks its output (constructors,
      `qsort`, `hypot`, 64-bit arithmetic, `argv`), compares the binary
      with the cross compiler's byte for byte, checks a compile error is
      reported, runs `-E`, `-S`, `as` and the driver on an object, and has
      `make` drive it through a Makefile with `/bin/sh` recipes.
      `tools/test-kernel-qemu.py` grew `--add HOST:GUEST` and `--timeout`
      for it.

Size: 200 lines in `build.sh`, the test and its guest; the work was in
the libc. What was expected to be medium risk (GCC's configure and an
unknown host) was in the end four build attempts, each stopped by one
missing libc feature.

## Phase 5: a filesystem for a build (done)

*Goal: `make` is correct on fluxOS (no spurious "up to date"), links work,
`cp -r` of the tree keeps everything. Measured by the native build of
phase 7 producing the same file list as the Linux build.*

Done 2026-10-30. `make test-kernel` runs the ext2 driver through the VFS on
images mke2fs made (1 and 4 KiB blocks, 128- and 256-byte inodes, one
filled by `mke2fs -d`), with 3000 random writes, truncations, unlinks,
renames and hard links checked against a model, and Linux's e2fsck and
debugfs judging every image afterwards. `make test-kernel-qemu` formats
the scratch disk with the native `mkfs.ext2`, uses it through the system
calls (and `cp -a`, `ls -l`, `fsck.ext2`), and the host's e2fsck checks the
disk after. `make test-install-qemu` installs with an ext2 `/src`
partition and boots the disk until init has mounted it.

- [x] ext2 read/write driver (`kernel/fs/ext2.c`, 1.5k lines): superblock,
      block groups, bitmaps, inodes through direct, indirect, double- and
      triple-indirect blocks (a 4 GiB file with 1 KiB blocks needs the
      third), linked-list directories, fast and slow symbolic links, hard
      links (an in-core inode shared by every vnode naming it, since a VFS
      vnode is a name), `mtime`/`ctime`, permission bits and owners,
      `rename` within and across directories (`..` and link counts kept),
      extended-attribute blocks released with their last user. Metadata goes
      through the write-back sector cache, file and directory blocks through
      `blk_read`/`blk_write`, which stay coherent with it. Revision 0 and 1;
      the feature flags are checked: ext4 (extents, 64-bit) and a journal to
      recover are refused, checksums mount read-only, an ext3 volume with a
      clean journal mounts as ext2 (as Linux allows); an htree directory's
      index flag is cleared when the directory changes. The volume is marked
      not clean while mounted, and clean at unmount and at shutdown
      (`vfs_sync_filesystems`, before the disks are synced).
- [x] `mkfs.ext2` natively (and `make disk DISKFS=ext2` on the host, with
      the same program built for it): revision 1, 256-byte inodes, file
      types, sparse superblock copies, large files, lost+found; 1 KiB blocks
      below 512 MiB, 4 KiB above, one inode per 8 KiB. `fsck.ext2` checks the
      counts (each group's free blocks, free inodes and directories against
      its bitmaps and inode table, bitmap padding, the superblock's totals)
      and `-y` corrects them; exit codes as e2fsck's. `mount -t ext2`.
- [x] VFS: symbolic links followed in `vfs_resolve` (40 at most, then
      `ELOOP`; the last component's not for `lstat`, `unlink`, `rmdir`,
      `rename`'s source, `readlink`, `lchown`; a trailing slash follows
      it), `lstat`, `readlink`, `link`, `symlink` (ramfs keeps symbolic links
      too; FAT answers `EPERM`, as Linux's vfat), `chmod`/`fchmod`/`chown`/
      `fchown`/`lchown` and the modes `open` and `mkdir` give that persist
      through the filesystem's `setattr`, owners and link counts in `stat`,
      `S_IFLNK` and `DT_LNK`. Six system calls (97-102). `cp -r` copies
      links as links, `cp -a` keeps modes, times and hard links too, `ls -l`
      shows `name -> target`.
- [x] `fluxinstall` and the automounter recognise ext2 volumes (it is the
      last in the probe order); the installer keeps FAT32 for `/` and with
      `-e /src` (or `/home`) makes a second, ext2, partition after it (the
      system gets half the disk up to 4 GiB, or `-r MIB`), which `/etc/fstab`
      names by UUID and init moves from the automounter's `/mnt/hdX2` to its
      place at start-up. fluxOS Setup's wizard does not offer it yet; its
      command prompt (Shift+F10) and the text mode do.
- [x] Timestamps: the FAT driver writes the modification time into the
      creation time's fields too, its odd second in the 10 ms field, and
      reads it back while it falls within the write time's 2-second step
      (another system writing the file sets only the write time), so the
      system partition is `make`-safe to the second.

Size: the driver 1.5k lines, the tools 0.5k, the VFS and system calls 0.4k, the tests 0.8k.

## Phase 6: boot media made natively (done)

*Goal: `mkiso` on fluxOS produces an ISO that boots in QEMU and on the
HP Pavilion g6, and `fluxinstall` installs from a native build.*

Done 2026-10-30, tried in QEMU (the HP Pavilion g6 is still to come): the
ISO `make` builds now comes from `mkiso` and boots as a CD and written to
a disk as a USB stick would be; `make test-install-qemu` installs from it
onto a blank disk and starts the disk alone; `make test-kernel-qemu` runs
the native `mkiso` on fluxOS and reads its image back through the kernel's
ISO 9660 driver; `make test-suspend-qemu` boots through an `mkiso` ISO too.
No build step downloads GRUB any more.

- [x] `firmware/grub/`: `boot.img`, `core.img` (`grub-mkimage -O i386-pc
      -p '(,msdos1)/boot/grub' biosdisk part_msdos fat`, byte for byte what
      the old rule made), the CD's `i386-pc/eltorito.img` (`-O
      i386-pc-eltorito -p /boot/grub biosdisk iso9660`: grub-mkrescue's adds
      a search for the CD by a UUID made from the ISO's date, which would
      change with every build; the plain one reads `/boot/grub` from the
      drive it started from), `boot_hybrid.img`, and the 45 modules the two
      menus load (their `insmod`s, their commands' modules through
      `command.lst`, a few for GRUB's command line, and the closure of
      `moddep.lst`) with GRUB's `.lst` files: 628 KiB. A README gives each
      file's origin, the Debian packages and their hashes; `SHA256SUMS` each
      file's. `tools/fetch-grub.sh --firmware DIR` makes the directory again.
- [x] `mkiso` (`user/bin/mkiso.c`, 0.8k lines): ISO 9660 level 1 names made
      unique, Rock Ridge (`SP`, `ER`, `PX`, `NM`, `TF`), both path tables,
      an El Torito boot record and catalog (no emulation, 4 sectors) with the
      boot information table written into the image, and a hybrid MBR (`-G
      boot_hybrid.img`: a protective partition over the image, GRUB's two
      addresses patched in as xorriso's `--grub2-mbr` and
      `--grub2-boot-info` do), so the ISO written to a USB stick starts too,
      as grub-mkrescue's did. Sequential writes only. xorriso reads its
      images as it reads its own.
- [x] `make iso` on fluxOS: the ISO rule copies the tree and runs `mkiso`
      (built by the C compiler of the machine running the build, `HOSTCC`,
      as `mbcheck` and `mkfs.ext2` are); `make MKRESCUE=...` keeps
      grub-mkrescue as the alternative.
- [x] `mbcheck` for `make check`: the Multiboot header (in the first 8 KiB,
      aligned, the checksum, no required feature a loader may lack, the
      a.out addresses when given) and the ELF layout (one `PT_LOAD`, inside
      the file, the entry point in it), in one program rather than two.
- [x] Setup CD: `fluxinstall` (and Setup's check for an install medium)
      reads `boot.img` and `core.img` from `firmware/grub` on the CD; the
      hard disk's menu stays in `boot/install/grub.cfg`.

Size: `mkiso` 800 lines, `mbcheck` 150, the tests 200. As expected, low risk:
the only surprises were the boot information table's checksum (taken after
GRUB's address is patched in) and the ISO's embedded-UUID config.

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
tests on fluxOS before relying on any of it. (Phases 3 and 4 were done
together on 2026-10-29, 5 and 6 on 2026-10-30; what remains is 7, and
running the tools on the kernel; `make test-gcc-qemu` passed on 2026-10-29.)

## Later, deliberately

- **Stage 3, GCC building GCC on fluxOS.** The compilers are C++ already
  (`g++` and `libstdc++` for fluxOS came with phase 4, because `cc1`
  itself is C++), so what this still needs is autotools' `configure`
  running under `fsh`'s `sh` mode (long scripts: `awk`, `expr`, `tr`,
  `grep -E`, `sort`, `uniq` and `m4`/`bison` for binutils are the tools
  missing), `libstdc++` and the C++ headers on the image, and hours of
  build time on the target machines. Everything in phases 5 to 7 makes it
  closer; nothing in them depends on it.
- **Dynamic linking** (`ld.so`, PIC, `dlopen`): nothing on the roadmap
  needs it; GCC plugins would.
- **Threads in the toolchain:** fluxOS has threads since 2026-10-29, but
  make `-j` and the compilers do not need them; the ports stay
  `--disable-threads` (`libstdc++` is built with `gthr-single`), and
  `--disable-tls` until the libc has static TLS for `__thread` (today it
  is emulated through libgcc's `emutls`).
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
