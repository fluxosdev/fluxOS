# fluxOS build
#
#   make            build the bootable install CD (fluxOS.iso), which starts
#                   fluxOS Setup and installs fluxOS onto a hard disk
#   make run        boot the ISO in QEMU (serial console on stdio), with
#                   fluxdisk.img to install onto
#   make run-kernel boot the kernel directly with QEMU's multiboot loader,
#                   running the whole system from memory (for development;
#                   text console only: that loader cannot set a video mode)
#   make run-disk   boot fluxdisk.img alone (after installing onto it)
#   make run-q35    boot the ISO on a q35 machine: fluxdisk.img and the CD on
#                   its SATA (AHCI) controller, a USB keyboard and tablet on xHCI
#   make run-usb    boot the ISO with fluxdisk.img as a USB stick on EHCI, and
#                   a USB keyboard and mouse on UHCI
#   make run-nvme   boot the ISO with fluxdisk.img as an NVMe disk
#   make check      verify the kernel is a valid multiboot image
#   make disk       create fluxdisk.img, a FAT32 data disk (kept by make clean)
#   make floppy     create fluxfloppy.img, a 1.44 MB FAT12 floppy that the
#                   QEMU targets then put in drive A: (kept by make clean)
#   make clean      remove build artifacts

CC      ?= gcc
LD      ?= ld
AR      ?= ar
QEMU    ?= qemu-system-i386
# A Core 2 Duo (SSE2 through SSSE3), two cores; QEMU_SMP=1 or 4 for other counts.
QEMU_CPU ?= -cpu core2duo,-lm
QEMU_SMP ?= 2
# GRUB 2.06 from Debian 12 (2.12 reboots some older PCs right after "Welcome
# to GRUB!"): its boot images and the modules the menus load are checked in
# (firmware/grub, see its README), and mkiso (user/bin/mkiso.c, built for the
# machine running the build: Linux, or fluxOS itself) makes the CD from them.
# grub-mkrescue can still make it, from Debian's GRUB fetched into grub-2.06/
# by tools/fetch-grub.sh, or the host's own:
#   make MKRESCUE=grub-2.06/usr/bin/grub-mkrescue
#   make MKRESCUE=grub-mkrescue GRUB=/ GRUB_DEP=
MKRESCUE ?=
GRUB    ?= grub-2.06
GRUB_DEP ?= $(GRUB)/usr/bin/grub-mkimage
GRUB_PC_DIR ?= $(GRUB)/usr/lib/grub/i386-pc
GRUB_FIRMWARE := $(shell find firmware/grub -type f 2>/dev/null)

# Programs the build runs on the machine it runs on, from user/bin: the C
# compiler there (on fluxOS, the native gcc) builds them.
HOSTCC ?= cc
HOST_TOOLS = $(BUILD)/host
MKISO   = $(HOST_TOOLS)/mkiso
MBCHECK = $(HOST_TOOLS)/mbcheck

# Plain "make" builds the ISO, whatever rule comes first below.
.DEFAULT_GOAL := all

BUILD   := build
ISO     := fluxOS.iso
KERNEL  := $(BUILD)/fluxos.kernel
TEXT_KERNEL := $(BUILD)/fluxos-text.kernel
SYSTEM  := $(BUILD)/system.tar
SETUP   := $(BUILD)/setup.tar
ROOTFS  := $(BUILD)/rootfs
SETUPFS := $(BUILD)/setupfs
SYSROOT := $(BUILD)/sysroot
SYSROOT_STAMP := $(SYSROOT)/.stamp
DISK    := fluxdisk.img
DISK_MB ?= 256
FLOPPY  := fluxfloppy.img
MKFS_FAT ?= $(shell command -v mkfs.fat 2>/dev/null || ls /usr/sbin/mkfs.fat /sbin/mkfs.fat 2>/dev/null | head -1)

GCC_INC := $(shell $(CC) -print-file-name=include)

# fluxOS needs a Pentium 4: everything is built for its instruction set,
# SSE2 included (kernel/core/cpu.c stops the boot on anything older).
CFLAGS_COMMON := -m32 -march=pentium4 -std=gnu11 -O2 -g \
	-ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
	-nostdinc -isystem $(GCC_INC) \
	-fno-asynchronous-unwind-tables -fno-strict-aliasing \
	-fno-tree-loop-distribute-patterns \
	-Wall -Wextra -Wno-unused-parameter -MMD -MP

# The kernel's C code keeps off the FPU and SSE registers, which belong to
# user processes (fpu.c switches them lazily); kernel/core/sse.S borrows the SSE
# registers for bulk copies, saving what it touches.
KERNEL_CFLAGS := $(CFLAGS_COMMON) -mgeneral-regs-only -fno-omit-frame-pointer \
	-Iinclude -Ikernel/include -Ikernel/core -Ikernel/fs -Ikernel/net -Ikernel/usb -Ikernel/input -Ikernel/storage -Ikernel/sound -Ikernel/video -Ikernel/acpi -Ikernel/wifi
# User code does all its float and double arithmetic on SSE2 (the x87 is
# left to long double) and may use the SSE2 intrinsics.
USER_CFLAGS   := $(CFLAGS_COMMON) -mfpmath=sse -fno-math-errno -Iinclude -Iuser/include

# What runs before kernel/core/cpu.c has checked the processor (the console, the
# string routines and kmain) is built for any 386, so that an older machine
# shows why it cannot run fluxOS instead of hanging on an i686 instruction.
KERNEL_EARLY_OBJS := $(addprefix $(BUILD)/k/,kernel/core/kmain.o kernel/core/cpu.o kernel/core/kprintf.o \
	kernel/core/lock.o kernel/video/vga.o kernel/video/fb.o kernel/core/serial.o kernel/video/splash.o \
	lib/string.o lib/vsnprintf.o lib/font8x16.o)
$(KERNEL_EARLY_OBJS): KERNEL_CFLAGS += -march=i386 -mtune=pentium4

LDFLAGS_COMMON := -m elf_i386 -nostdlib -z noexecstack --no-warn-rwx-segments

# ---- kernel ----
# uACPI, the AML interpreter behind kernel/acpi: its sources and the kernel's
# ACPI code share one configuration (kernel/acpi/uacpi_flux.h).
UACPI_DIR := third_party/uacpi
UACPI_SRCS := $(wildcard $(UACPI_DIR)/source/*.c)
UACPI_FLAGS := -I$(UACPI_DIR)/include -include kernel/acpi/uacpi_flux.h

KERNEL_SRCS := $(wildcard kernel/*/*.S kernel/*/*.c) lib/string.c lib/vsnprintf.c lib/font8x16.c \
               lib/sha256.c lib/sha1.c lib/aes.c lib/logo.c user/desktop/shell/font_archivo.c $(UACPI_SRCS)
KERNEL_OBJS := $(patsubst %,$(BUILD)/k/%.o,$(basename $(KERNEL_SRCS)))
UACPI_OBJS := $(patsubst %,$(BUILD)/k/%.o,$(basename $(UACPI_SRCS) $(wildcard kernel/acpi/*.c)))
$(UACPI_OBJS): KERNEL_CFLAGS += $(UACPI_FLAGS)

# ---- user space ----
MBEDTLS_DIR := third_party/mbedtls
MBEDTLS_FLAGS := -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR) -DMBEDTLS_CONFIG_FILE='"flux_config.h"'
USER_CFLAGS += $(MBEDTLS_FLAGS)
MBEDTLS_SRCS := $(wildcard $(MBEDTLS_DIR)/library/*.c)
FREETYPE_DIR := third_party/freetype
BROTLI_DIR := third_party/brotli
FONT_FLAGS := -I$(FREETYPE_DIR)/include -I$(FREETYPE_DIR) -I$(BROTLI_DIR)/c/include \
	-DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H='"flux_ftmodule.h"' \
	-DFT_CONFIG_OPTIONS_H='"flux_ftoption.h"' -DFT_CONFIG_STANDARD_LIBRARY_H='"flux_ftstdlib.h"'
FREETYPE_SRCS := $(addprefix $(FREETYPE_DIR)/src/,base/ftbase.c base/ftinit.c base/ftsystem.c base/ftdebug.c \
	base/ftbitmap.c base/ftglyph.c base/ftmm.c base/ftfstype.c sfnt/sfnt.c truetype/truetype.c cff/cff.c \
	psnames/psnames.c psaux/psaux.c pshinter/pshinter.c raster/raster.c smooth/smooth.c gzip/ftgzip.c)
BROTLI_SRCS := $(wildcard $(BROTLI_DIR)/c/common/*.c $(BROTLI_DIR)/c/dec/*.c)
FONT_SRCS := $(FREETYPE_SRCS) $(BROTLI_SRCS)
FONT_OBJS := $(patsubst %,$(BUILD)/u/%.o,$(basename $(FONT_SRCS)))
LIBFONT := $(BUILD)/libfont.a
$(FONT_OBJS) $(BUILD)/u/user/desktop/wave/webfont.o: USER_CFLAGS += $(FONT_FLAGS)
$(BUILD)/u/user/libc/brdecode.o: USER_CFLAGS += -I$(BROTLI_DIR)/c/include
$(LIBFONT): $(FONT_OBJS)
	$(AR) rcs $@ $^

LIBC_SRCS := $(wildcard user/libc/*.c) $(BROTLI_SRCS) $(MBEDTLS_SRCS) lib/string.c lib/font8x16.c lib/sha256.c lib/sha1.c lib/digest.c lib/logo.c
LIBC_OBJS := $(patsubst %,$(BUILD)/u/%.o,$(basename $(LIBC_SRCS)))
CRT0      := $(BUILD)/u/user/libc/crt0.o
LIBC      := $(BUILD)/libc.a
GL_OBJS   := $(patsubst %,$(BUILD)/u/%.o,$(basename $(wildcard user/gl/*.c)))
LIBGL     := $(BUILD)/libGL.a

PROGS     := $(basename $(notdir $(wildcard user/bin/*.c)))
FSH_OBJS  := $(patsubst %,$(BUILD)/u/%.o,$(basename $(wildcard user/fsh/*.c)))
DESKTOP_DIRS := user/desktop/apps user/desktop/image user/desktop/shell user/desktop/wave user/desktop/wave/js \
                user/desktop/progs
DESKTOP_INC  := $(addprefix -I,$(DESKTOP_DIRS))
DESKTOP_SRCS := $(foreach d,$(DESKTOP_DIRS),$(wildcard $(d)/*.c))
DESKTOP_HDRS := $(foreach d,$(DESKTOP_DIRS),$(wildcard $(d)/*.h))
DESKTOP_OBJS := $(patsubst %,$(BUILD)/u/%.o,$(basename $(DESKTOP_SRCS)))
$(DESKTOP_OBJS): USER_CFLAGS += $(DESKTOP_INC)
desktop_objs = $(patsubst %,$(BUILD)/u/user/desktop/%.o,$(basename $(1)))
# What the desktop and every program share: drawing, pictures, icons, dialogs, the theme, starting
# programs, the window protocol's plumbing. A program takes from it only what it uses.
# (SVG pictures are drawn with Wave's HTML, CSS and font code.)
LIBDESKTOP_SRCS := $(wildcard user/desktop/image/*.c) $(filter-out %/browser.c,$(wildcard user/desktop/wave/*.c)) \
                   $(addprefix user/desktop/shell/,gfx.c font_archivo.c font_sans.c icons.c widgets.c theme.c \
                     wallpaint.c grubenv.c imagejobs.c apps.c launch.c loop.c wmconn.c mousecfg.c)
LIBDESKTOP := $(BUILD)/libdesktop.a
# The desktop process: the window manager, the taskbar, its folder, its dialogs, and setup's screen.
DESKTOP_SHELL_OBJS := $(call desktop_objs,$(addprefix shell/,main.c wm.c server.c sidebar.c wallpaper.c \
                        screensaver.c power.c sound.c sysdlg.c clock.c setup.c wifitray.c) apps/files.c)
# The programs, each /bin/NAME: progs/NAME.c, the program's own sources, and client.c for its windows.
DESKTOP_PROGS := wave terminal files editor calculator player archiver paint gears minesweeper taskmgr \
                 diskmgr display mouse datetime wireless imageview
PROG_SRCS_wave        := $(patsubst user/desktop/%,%,$(wildcard user/desktop/wave/*.c user/desktop/wave/js/*.c))
PROG_SRCS_terminal    := apps/term.c
PROG_SRCS_files       := apps/files.c
PROG_SRCS_editor      := apps/edit.c
PROG_SRCS_calculator  := apps/calc.c
PROG_SRCS_player      := apps/player.c
PROG_SRCS_archiver    := apps/archiver.c
PROG_SRCS_paint       := apps/paint.c
PROG_SRCS_gears       := apps/gears.c
PROG_SRCS_minesweeper := apps/mines.c
PROG_SRCS_taskmgr     := apps/taskmgr.c
PROG_SRCS_diskmgr     := apps/diskmgr.c
PROG_SRCS_display     := apps/display.c
PROG_SRCS_mouse       := apps/mouse.c
PROG_SRCS_datetime    := apps/datetime.c
PROG_SRCS_wireless    := apps/wireless.c
PROG_SRCS_imageview   := apps/imageview.c
DOS_OBJS  := $(patsubst %,$(BUILD)/u/%.o,$(basename $(wildcard user/dos/*.c)))
# winrun, the Windows program loader (os_linux.c is its Linux back end, for host tests)
WIN32_OBJS := $(patsubst %,$(BUILD)/u/%.o,$(basename $(filter-out user/win32/os_linux.c,$(wildcard user/win32/*.c user/win32/*.S))))
BINS      := $(addprefix $(ROOTFS)/bin/,$(PROGS)) $(ROOTFS)/bin/fsh $(ROOTFS)/bin/desktop \
             $(addprefix $(ROOTFS)/bin/,$(DESKTOP_PROGS)) \
             $(ROOTFS)/bin/dos $(ROOTFS)/bin/winrun $(ROOTFS)/bin/[ $(ROOTFS)/bin/gunzip $(ROOTFS)/bin/zcat

ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)
SETUPFS_FILES := $(shell find setupfs -type f 2>/dev/null)

# The install CD's setup system, the one GRUB loads into memory: fluxOS
# Setup (the desktop and fluxinstall), its icons, the terminal and Disk
# Manager it opens, and a few tools for the command prompt Shift+F10 opens. Small, so that it fits the kernel heap of
# a machine with little memory; the system it installs is $(SYSTEM), which
# fluxinstall reads from the CD.
SETUP_BINS := init desktop terminal diskmgr fluxinstall mkfs.fat mkfs.ext2 fsh cat ls lsblk mount umount df dmesg \
              ps free uname clear more mkdir rm cp mv sync reboot poweroff halt

.PHONY: all iso kernel system setup disk floppy run run-kernel run-disk run-q35 run-usb run-nvme check clean
.SECONDARY:

all: $(ISO)
iso: $(ISO)
kernel: $(KERNEL)
system: $(SYSTEM)
setup: $(SETUP)

$(BUILD)/k/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/k/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# The Radeon microcode is pulled in with .incbin.
$(BUILD)/k/kernel/video/radeon_fw.o: $(wildcard firmware/radeon/*.bin)

$(KERNEL): $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS_COMMON) -T linker.ld -o $@ $(KERNEL_OBJS)

# The same kernel without the Multiboot video request, for run-kernel.
$(BUILD)/k/kernel/core/boot-text.o: kernel/core/boot.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -DMB_TEXT_ONLY -c $< -o $@

TEXT_KERNEL_OBJS := $(BUILD)/k/kernel/core/boot-text.o $(filter-out $(BUILD)/k/kernel/core/boot.o,$(KERNEL_OBJS))

$(TEXT_KERNEL): $(TEXT_KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS_COMMON) -T linker.ld -o $@ $(TEXT_KERNEL_OBJS)

$(BUILD)/u/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/u/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(LIBC): $(LIBC_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(LIBGL): $(GL_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(ROOTFS)/bin/fsh: $(CRT0) $(FSH_OBJS) $(LIBC) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $(FSH_OBJS) $(LIBC)

$(LIBDESKTOP): $(call desktop_objs,$(patsubst user/desktop/%,%,$(LIBDESKTOP_SRCS))) Makefile
	@rm -f $@
	$(AR) rcs $@ $(filter %.o,$^)

$(ROOTFS)/bin/desktop: $(CRT0) $(DESKTOP_SHELL_OBJS) $(LIBDESKTOP) $(LIBFONT) $(LIBC) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $(DESKTOP_SHELL_OBJS) $(LIBDESKTOP) $(LIBFONT) $(LIBC)

define DESKTOP_PROG_RULE
$(ROOTFS)/bin/$(1): $(CRT0) $(call desktop_objs,progs/$(1).c $(PROG_SRCS_$(1)) shell/client.c) $(LIBDESKTOP) $(LIBGL) \
                    $(LIBFONT) $(LIBC) user/user.ld
	@mkdir -p $$(dir $$@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $$@ $(CRT0) \
		$(call desktop_objs,progs/$(1).c $(PROG_SRCS_$(1)) shell/client.c) $(LIBDESKTOP) $(LIBGL) $(LIBFONT) $(LIBC)
endef
$(foreach p,$(DESKTOP_PROGS),$(eval $(call DESKTOP_PROG_RULE,$(p))))

$(ROOTFS)/bin/dos: $(CRT0) $(DOS_OBJS) $(LIBC) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $(DOS_OBJS) $(LIBC)

$(ROOTFS)/bin/winrun: $(CRT0) $(WIN32_OBJS) $(LIBC) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $(WIN32_OBJS) $(LIBC)

$(ROOTFS)/bin/%: $(BUILD)/u/user/bin/%.o $(CRT0) $(LIBC) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $< $(LIBC)

# test is also [, and gzip is also gunzip and zcat (they look at their name)
$(ROOTFS)/bin/[: $(ROOTFS)/bin/test
	cp $< $@
$(ROOTFS)/bin/gunzip $(ROOTFS)/bin/zcat: $(ROOTFS)/bin/gzip
	cp $< $@

$(BUILD)/.rootfs-stamp: $(ROOTFS_FILES)
	@mkdir -p $(ROOTFS)
	cp -r rootfs/. $(ROOTFS)/
	@touch $@

# The ports (GNU make and binutils for fluxOS, tools/ports/build.sh) go into the
# system too, once "make ports" has built them, and so does the native GCC
# ("make toolchain-native"), with the sysroot's headers and libraries as
# /usr/include and /usr/lib so that it has something to compile against.
PORTS_ROOT := $(BUILD)/ports/root
PORTS_FILES := $(wildcard $(PORTS_ROOT)/usr/bin/*)
NATIVE_ROOT := $(BUILD)/toolchain/native/root
NATIVE_FILES := $(wildcard $(NATIVE_ROOT)/usr/bin/gcc)

# The AX210's firmware, which the wireless driver reads from /lib/firmware.
IWL_FIRMWARE := $(wildcard firmware/iwlwifi/*.ucode firmware/iwlwifi/*.pnvm)
IWL_FIRMWARE_DEST := $(addprefix $(ROOTFS)/lib/firmware/,$(notdir $(IWL_FIRMWARE)))
$(ROOTFS)/lib/firmware/%: firmware/iwlwifi/% $(BUILD)/.rootfs-stamp
	@mkdir -p $(dir $@)
	cp $< $@

$(SYSTEM): $(BUILD)/.rootfs-stamp $(BINS) $(IWL_FIRMWARE_DEST) $(PORTS_FILES) $(NATIVE_FILES) $(if $(NATIVE_FILES),$(SYSROOT_STAMP),)
	cp $(ROOTFS)/bin/fsh $(ROOTFS)/bin/sh
	chmod 755 $(ROOTFS)/home/flux/demo.sh
	tar --format=ustar --owner=0 --group=0 --numeric-owner -cf $@ -C $(ROOTFS) . \
		$(if $(PORTS_FILES),-C $(abspath $(PORTS_ROOT)) .,) \
		$(if $(NATIVE_FILES),-C $(abspath $(NATIVE_ROOT)) . -C $(abspath $(SYSROOT)) usr,)

.PHONY: ports toolchain-native
ports: sysroot
	tools/ports/build.sh --toolchain $(BUILD)/toolchain/cross --sysroot $(SYSROOT)

# The GCC that runs on fluxOS (the Canadian cross of tools/toolchain/build.sh --native),
# in $(NATIVE_ROOT); the next "make" puts it on the system image.
toolchain-native: sysroot
	tools/toolchain/build.sh --native --sysroot $(SYSROOT) --prefix $(BUILD)/toolchain/cross --dest $(NATIVE_ROOT)

$(SETUP): Makefile $(BUILD)/.rootfs-stamp $(BINS) $(SETUPFS_FILES) $(IWL_FIRMWARE_DEST)
	rm -rf $(SETUPFS)
	mkdir -p $(SETUPFS)/bin $(SETUPFS)/etc $(SETUPFS)/usr/share
	cp $(addprefix $(ROOTFS)/bin/,$(SETUP_BINS)) $(SETUPFS)/bin/
	cp $(SETUPFS)/bin/fsh $(SETUPFS)/bin/sh
	cp $(ROOTFS)/etc/fshrc $(SETUPFS)/etc/
	cp -r $(ROOTFS)/usr/share/icons $(SETUPFS)/usr/share/
	cp -r setupfs/. $(SETUPFS)/
	@# Setup boots the same kernel, so its wireless worker needs firmware too.
	mkdir -p $(SETUPFS)/lib/firmware $(SETUPFS)/usr/share/doc/firmware
	cp $(IWL_FIRMWARE_DEST) $(SETUPFS)/lib/firmware/
	cp firmware/iwlwifi/LICENCE.iwlwifi_firmware $(SETUPFS)/usr/share/doc/firmware/
	tar --format=ustar --owner=0 --group=0 --numeric-owner -cf $@ -C $(SETUPFS) .

$(GRUB)/usr/bin/grub-mkimage:
	sh tools/fetch-grub.sh $(GRUB)

$(HOST_TOOLS)/%: user/bin/%.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DHOST_TOOL -idirafter user/include $< -o $@

# The CD: GRUB's menu (grub/install.cfg), the kernel, the setup system it
# loads and the system Setup installs, with GRUB's El Torito image and
# modules (firmware/grub) in boot/grub/i386-pc. GRUB for hard disks is in
# firmware/grub, as in the tree: /bin/fluxinstall writes boot.img into the
# MBR and core.img into the gap before the first partition, which reads its
# modules and menu (grub/grub.cfg, copied from boot/install) from /boot/grub
# on that FAT32 partition. boot_hybrid.img makes the ISO start from a USB
# stick too.
$(ISO): $(KERNEL) $(SETUP) $(SYSTEM) grub/grub.cfg grub/install.cfg $(GRUB_FIRMWARE) \
		$(if $(MKRESCUE),$(GRUB_DEP),$(MKISO))
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/boot/grub $(BUILD)/iso/boot/install $(BUILD)/iso/firmware/grub
	cp $(KERNEL) $(BUILD)/iso/boot/fluxos.kernel
	cp $(SETUP) $(BUILD)/iso/boot/setup.tar
	cp $(SYSTEM) $(BUILD)/iso/boot/system.tar
	cp grub/install.cfg $(BUILD)/iso/boot/grub/grub.cfg
	cp grub/grub.cfg $(BUILD)/iso/boot/install/grub.cfg
	cp firmware/grub/boot.img firmware/grub/core.img firmware/grub/README $(BUILD)/iso/firmware/grub/
ifeq ($(MKRESCUE),)
	cp -r firmware/grub/i386-pc $(BUILD)/iso/boot/grub/
	$(MKISO) -o $@ -b boot/grub/i386-pc/eltorito.img -G firmware/grub/boot_hybrid.img $(BUILD)/iso
else
	PATH="$(abspath $(GRUB))/usr/bin:$$PATH" $(MKRESCUE) -d $(GRUB_PC_DIR) -o $@ $(BUILD)/iso
endif

# The fluxOS sysroot: the headers and libraries a compiler targeting
# fluxOS (i686-fluxos-gcc, tools/toolchain) builds programs against.
.PHONY: sysroot
sysroot: $(SYSROOT_STAMP)

$(SYSROOT_STAMP): $(LIBC) $(LIBGL) $(LIBFONT) $(CRT0) user/user.ld $(wildcard user/include/*.h user/include/*/*.h include/flux/*.h)
	@mkdir -p $(SYSROOT)/usr/include/flux $(SYSROOT)/usr/lib
	cp -r user/include/. $(SYSROOT)/usr/include/
	cp include/flux/*.h $(SYSROOT)/usr/include/flux/
	cp $(LIBC) $(LIBGL) $(LIBFONT) $(CRT0) $(SYSROOT)/usr/lib/
	cp user/user.ld $(SYSROOT)/usr/lib/fluxos.ld
	@# threads, real-time calls and the math library are in libc.a: -lpthread, -lrt and -lm
	@# (which g++ always adds) find empty archives
	for l in pthread rt m; do rm -f $(SYSROOT)/usr/lib/lib$$l.a; ar rc $(SYSROOT)/usr/lib/lib$$l.a; done
	@touch $@
	@echo "$(SYSROOT): headers, libc.a (with pthreads and libm), libGL.a, libfont.a, crt0.o, fluxos.ld"

# The cross toolchain (binutils and GCC for i686-fluxos) in build/toolchain/cross.
.PHONY: toolchain
toolchain: sysroot
	tools/toolchain/build.sh --sysroot $(SYSROOT) --prefix $(BUILD)/toolchain/cross

# The Multiboot header GRUB looks for, and the one PT_LOAD segment the linker
# script promises (mbcheck, built for the machine the build runs on).
check: $(KERNEL) $(MBCHECK)
	$(MBCHECK) $(KERNEL)

disk: $(DISK)

# A blank FAT32 disk; without a host mkfs.fat it stays unformatted and can
# be formatted from inside fluxOS with "mkfs.fat /dev/hda".
# DISKFS=ext2 makes it ext2 instead, with fluxOS's own mkfs.ext2 built for
# the host (a disk for a source tree: links, permissions, one-second times).
DISKFS ?= fat32
$(DISK): $(if $(filter ext2,$(DISKFS)),$(HOST_TOOLS)/mkfs.ext2)
	truncate -s $(DISK_MB)M $@
ifeq ($(DISKFS),ext2)
	$(HOST_TOOLS)/mkfs.ext2 -q -L FLUXDISK $@ && echo "$@: $(DISK_MB) MiB ext2"
else
	@if [ -n "$(MKFS_FAT)" ]; then $(MKFS_FAT) -F 32 -n FLUXDISK $@ >/dev/null && \
		echo "$@: $(DISK_MB) MiB FAT32"; \
	else echo "$@: blank, run 'mkfs.fat /dev/hda' inside fluxOS"; fi
endif

floppy: $(FLOPPY)

# A blank floppy; without a host mkfs.fat, format it inside fluxOS with
# "mkfs.fat /dev/fd0".
$(FLOPPY):
	truncate -s 1440K $@
	@if [ -n "$(MKFS_FAT)" ]; then $(MKFS_FAT) -F 12 -n FLUXFLOPPY $@ >/dev/null && \
		echo "$@: 1.44 MB FAT12 floppy"; \
	else echo "$@: blank, run 'mkfs.fat /dev/fd0' inside fluxOS"; fi

QEMU_DISK := -drive file=$(DISK),format=raw,if=ide,index=0
ifneq ($(wildcard $(FLOPPY)),)
QEMU_DISK += -drive file=$(FLOPPY),format=raw,if=floppy,index=0
endif
# An NE2000 card on QEMU's user-mode network: DHCP, DNS and NAT to the host's network.
QEMU_NET  ?= -netdev user,id=net0 -device ne2k_pci,netdev=net0
# A Sound Blaster 16 (ISA, 0x220, IRQ 5, DMA 1 and 5) played through the host's PipeWire/PulseAudio.
# Another host backend: make run QEMU_AUDIO="-audiodev alsa,id=snd0".
QEMU_AUDIO ?= -audiodev pa,id=snd0,out.latency=150000
QEMU_SND  ?= $(QEMU_AUDIO) -device sb16,audiodev=snd0

run: $(ISO) $(DISK)
	$(QEMU) $(QEMU_CPU) -smp $(QEMU_SMP) -m 2G -serial stdio -boot d -cdrom $(ISO) $(QEMU_DISK) $(QEMU_NET) $(QEMU_SND)

run-kernel: $(TEXT_KERNEL) $(SYSTEM) $(DISK)
	$(QEMU) $(QEMU_CPU) -smp $(QEMU_SMP) -kernel $(TEXT_KERNEL) -initrd $(SYSTEM) -m 1G -serial stdio $(QEMU_DISK) $(QEMU_NET) $(QEMU_SND)

# Boot fluxdisk.img on its own, after "fluxinstall /dev/hda".
run-disk: $(DISK)
	$(QEMU) $(QEMU_CPU) -smp $(QEMU_SMP) -m 2G -serial stdio -boot c $(QEMU_DISK) $(QEMU_NET) $(QEMU_SND)

# On q35 "if=ide" drives and the CD sit on the chipset's AHCI controller.
QEMU_XHCI ?= -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0

run-q35: $(ISO) $(DISK)
	$(QEMU) $(QEMU_CPU) -smp $(QEMU_SMP) -M q35 -m 1G -serial stdio -boot d -cdrom $(ISO) \
		-drive file=$(DISK),format=raw,if=ide,index=0 $(QEMU_NET) $(QEMU_SND) $(QEMU_XHCI)

QEMU_USB ?= -usb -device usb-kbd,bus=usb-bus.0 -device usb-mouse,bus=usb-bus.0 \
	-device usb-ehci,id=ehci -drive file=$(DISK),format=raw,if=none,id=stick \
	-device usb-storage,bus=ehci.0,drive=stick

run-usb: $(ISO) $(DISK)
	$(QEMU) $(QEMU_CPU) -smp $(QEMU_SMP) -m 1G -serial stdio -boot d -cdrom $(ISO) $(QEMU_NET) $(QEMU_SND) $(QEMU_USB)

# fluxdisk.img as an NVMe disk, to install onto; the CD stays on IDE.
QEMU_NVME ?= -drive file=$(DISK),format=raw,if=none,id=nvme0 -device nvme,serial=fluxdisk,drive=nvme0

run-nvme: $(ISO) $(DISK)
	$(QEMU) $(QEMU_CPU) -smp $(QEMU_SMP) -m 2G -serial stdio -boot d -cdrom $(ISO) $(QEMU_NVME) $(QEMU_NET) $(QEMU_SND)

# A USB audio device (USB Audio Class 1.0, 48 kHz stereo) on a UHCI controller, and no Sound
# Blaster, so it is /dev/dsp. QEMU_USB_AUDIO_BUS="-device qemu-xhci,id=xhci" USB_AUDIO_BUS=xhci.0
# puts it on an xHCI controller instead.
QEMU_USB_AUDIO_BUS ?= -usb
USB_AUDIO_BUS ?= usb-bus.0
QEMU_USB_AUDIO ?= $(QEMU_AUDIO) $(QEMU_USB_AUDIO_BUS) -device usb-audio,audiodev=snd0,bus=$(USB_AUDIO_BUS)

run-usb-audio: $(ISO) $(DISK)
	$(QEMU) $(QEMU_CPU) -smp $(QEMU_SMP) -m 1G -serial stdio -boot d -cdrom $(ISO) $(QEMU_DISK) $(QEMU_NET) $(QEMU_USB_AUDIO)

clean:
	rm -rf $(BUILD) $(ISO)

-include $(shell find $(BUILD) -path '$(BUILD)/toolchain' -prune -o -path '$(BUILD)/ports' -prune -o -name '*.d' -print 2>/dev/null)

# Run the browser's actual HTML/DOM/JS code on the host, without booting QEMU.

# The phase 3 tools (tar, gzip, sed, diff, patch, find...) built for the host and
# checked against the GNU originals (tests/tools-host.sh).
TOOL_TESTS := find install cmp cut xargs truncate dd ln nproc true false printf test mktemp diff patch sed gzip tar \
	mkfs.ext2 fsck.ext2 mkiso mbcheck
TOOL_TEST_BINS := $(addprefix $(BUILD)/tests/tools/,$(TOOL_TESTS))
.PHONY: test-tools
test-tools: $(TOOL_TEST_BINS)
	tests/tools-host.sh $(BUILD)/tests/tools

$(BUILD)/tests/tools/gzip $(BUILD)/tests/tools/tar: $(BUILD)/tests/tools/%: user/bin/%.c user/libc/deflate.c user/libc/arc_codec.c user/include/deflate.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -D_GNU_SOURCE -Iinclude -idirafter user/include \
		$< user/libc/deflate.c user/libc/arc_codec.c -o $@

$(BUILD)/tests/tools/test: user/fsh/testexpr.c
$(BUILD)/tests/tools/%: user/bin/%.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DHOST_TOOL -idirafter user/include $< -o $@

# fsh as /bin/sh, built for the host (tests/fsh-host/flux.h stands in for the two
# fluxOS calls it makes) and checked against the host's /bin/sh (tests/sh-host.sh).
.PHONY: test-sh
test-sh: $(BUILD)/tests/sh
	tests/sh-host.sh $<

$(BUILD)/tests/sh: $(wildcard user/fsh/*.c user/fsh/*.h) tests/fsh-host/flux.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -D_GNU_SOURCE -Itests/fsh-host \
		$(filter %.c,$^) -o $@

# The native GCC on the real kernel: compiles and runs a program under QEMU and
# compares the binary with the cross compiler's (needs "make toolchain-native").
.PHONY: test-gcc-qemu
test-gcc-qemu: $(TEXT_KERNEL) $(SYSTEM) $(BUILD)/tests/gcc-guest $(BUILD)/tests/gcc-hello.cross $(BUILD)/tests/gcc-hellocc.cross $(NATIVE_ROOT)/usr/bin/gcc
	python3 tools/test-kernel-qemu.py $(QEMU) $(TEXT_KERNEL) $(SYSTEM) $(BUILD)/tests/gcc-guest --cpus $(KERNEL_TEST_CPUS) \
		--memory 1024 --add $(BUILD)/tests/gcc-hello.cross:tests/hello.cross --add tests/gcc-hello.c:tests/hello.c \
		--add $(BUILD)/tests/gcc-hellocc.cross:tests/hellocc.cross --add tests/gcc-hello.cc:tests/hello.cc \
		--success-marker "GCC REGRESSIONS PASSED" --timeout 480

# Built from a copy named hello.c, as the guest compiles it: the symbol table keeps
# the source's name, so the unstripped programs can agree too.
$(BUILD)/tests/gcc-hello.cross: tests/gcc-hello.c $(BUILD)/toolchain/cross/bin/i686-fluxos-gcc $(SYSROOT)/usr/lib/libc.a
	@mkdir -p $(BUILD)/tests/gcc-hello
	cp $< $(BUILD)/tests/gcc-hello/hello.c
	cd $(BUILD)/tests/gcc-hello && $(CURDIR)/$(BUILD)/toolchain/cross/bin/i686-fluxos-gcc -O2 -o $(CURDIR)/$@ hello.c

$(BUILD)/tests/gcc-hellocc.cross: tests/gcc-hello.cc $(BUILD)/toolchain/cross/bin/i686-fluxos-g++ $(SYSROOT)/usr/lib/libc.a
	@mkdir -p $(BUILD)/tests/gcc-hello
	cp $< $(BUILD)/tests/gcc-hello/hello.cc
	cd $(BUILD)/tests/gcc-hello && $(CURDIR)/$(BUILD)/toolchain/cross/bin/i686-fluxos-g++ -O2 -o $(CURDIR)/$@ hello.cc

$(BUILD)/tests/gcc-guest.o: tests/gcc-guest.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/tests/gcc-guest: $(BUILD)/tests/gcc-guest.o $(CRT0) $(LIBC) user/user.ld
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $< $(LIBC)
BROWSER_TEST_SRCS := lib/digest.c lib/sha256.c tests/browser-dom.c tests/canvas-font-stub.c tests/js-host-stack.c \
	$(addprefix user/desktop/wave/js/,js.c jsbig.c jsuni.c jslib.c jslib2.c jsbinary.c jsnet.c jsre.c jsgc.c jsdom.c jscanvas.c jsworker.c) \
	$(addprefix user/desktop/wave/,canvas.c html.c shadow.c webblob.c webintegrity.c webinput.c webutil.c webpolicy.c webstore.c css.c csscolor.c cssfont.c anim.c) \
	user/libc/inflate.c
.PHONY: test-browser
test-browser: $(BUILD)/tests/browser-dom $(BUILD)/tests/browser-worker $(BUILD)/tests/jsworker $(BUILD)/tests/js-engine $(BUILD)/tests/browser-style $(BUILD)/tests/browser-layout $(BUILD)/tests/browser-policy $(BUILD)/tests/browser-http $(BUILD)/tests/browser-images $(BUILD)/tests/browser-navigation $(BUILD)/tests/browser-geometry $(BUILD)/tests/browser-drag
	$< rootfs/usr/share/www/classlist-tests.js 20
	$< rootfs/usr/share/www/dom-feature-tests.js 31
	$(BUILD)/tests/browser-style
	$(BUILD)/tests/browser-layout
	$(BUILD)/tests/browser-policy
	$(BUILD)/tests/browser-http
	$(BUILD)/tests/browser-dom rootfs/usr/share/www/modern-core-tests.js 47
	$(BUILD)/tests/browser-dom rootfs/usr/share/www/compatibility-tests.js 30
	$(BUILD)/tests/browser-images
	$(BUILD)/tests/browser-navigation
	$(BUILD)/tests/browser-geometry
	$(BUILD)/tests/browser-drag
	$(BUILD)/tests/browser-worker
	$(BUILD)/tests/browser-dom rootfs/usr/share/www/clone-escape-tests.js 23
	$(BUILD)/tests/browser-dom rootfs/usr/share/www/canvas-tests.js 52
	$(BUILD)/tests/browser-dom rootfs/usr/share/www/js-language-tests.js 134
	rm -rf $(BUILD)/tests/idb
	$(BUILD)/tests/browser-dom rootfs/usr/share/www/indexeddb-tests.js 3
	$(BUILD)/tests/js-engine

$(BUILD)/tests/browser-dom: $(BROWSER_TEST_SRCS) $(DESKTOP_HDRS) user/include/flux.h include/flux/abi.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -DJS_HOST_TEST -DIDB_DIR='"$(CURDIR)/$(BUILD)/tests/idb/"' -DJS_VM_VERIFY -pthread -Wall -Wextra -Wno-unused-parameter \
		$(DESKTOP_INC) -Iinclude -idirafter user/include $(BROWSER_TEST_SRCS) \
		-lm -o $@

# The C library's formatting, number parsing, regex, fnmatch, getopt and
# calendar code against the host's glibc (compiled against the fluxOS headers).
LIBC_TEST_FORMAT := user/libc/printf.c user/libc/strtonum.c user/libc/scanf.c
LIBC_TEST_POSIX := user/libc/regex.c user/libc/fnmatch.c user/libc/glob.c user/libc/getopt.c user/libc/time.c
.PHONY: test-libc
test-libc: $(BUILD)/tests/libc-format $(BUILD)/tests/libc-posix
	$(BUILD)/tests/libc-format
	$(BUILD)/tests/libc-posix

$(BUILD)/tests/libc-format: tests/libc-format.c tests/libc-host-build.sh $(LIBC_TEST_FORMAT) $(wildcard user/include/*.h user/include/sys/*.h) include/flux/abi.h
	@mkdir -p $(dir $@)
	HOSTCC=$(HOSTCC) tests/libc-host-build.sh $@.lib.o $(LIBC_TEST_FORMAT)
	$(HOSTCC) -std=gnu11 -O1 -g -Wno-format tests/libc-format.c $@.lib.o -lm -o $@

$(BUILD)/tests/libc-posix: tests/libc-posix.c tests/libc-host-build.sh $(LIBC_TEST_POSIX) $(wildcard user/include/*.h user/include/sys/*.h) include/flux/abi.h
	@mkdir -p $(dir $@)
	HOSTCC=$(HOSTCC) tests/libc-host-build.sh $@.lib.o $(LIBC_TEST_POSIX)
	$(HOSTCC) -std=gnu11 -O1 -g -Wno-format tests/libc-posix.c $@.lib.o -o $@

# The same library on the real kernel: boots a disposable QEMU with the
# guest program as init (not run by default builds).
# The DOS emulator's CPU against the host processor's (x86 hosts only).
.PHONY: test-dos
test-dos: $(BUILD)/tests/dos-cpu
	$(BUILD)/tests/dos-cpu 1000000

$(BUILD)/tests/dos-cpu: tests/dos-cpu.c user/dos/cpu.c user/dos/dos.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -O1 -w -Iuser/dos tests/dos-cpu.c user/dos/cpu.c -o $@

.PHONY: test-libc-qemu
test-libc-qemu: $(TEXT_KERNEL) $(SYSTEM) $(BUILD)/tests/libc-guest
	python3 tools/test-kernel-qemu.py $(QEMU) $(TEXT_KERNEL) $(SYSTEM) $(BUILD)/tests/libc-guest --cpus $(KERNEL_TEST_CPUS) $(TEST_DROP_TOOLCHAIN) --success-marker "LIBC REGRESSIONS PASSED"

$(BUILD)/tests/libc-guest.o: tests/libc-guest.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/tests/libc-guest: $(BUILD)/tests/libc-guest.o $(CRT0) $(LIBC) user/user.ld
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $< $(LIBC)

# Host regression tests use production kernel code with mocked hardware/I/O.
KERNEL_TEST_NAMES := heap pmm pipe socket storage ext2 syscall block sched signal wifi iwl-trans iwl-sta pcm
KERNEL_TEST_BINS := $(addprefix $(BUILD)/tests/kernel-,$(KERNEL_TEST_NAMES))
.PHONY: test-kernel
test-kernel: $(KERNEL_TEST_BINS)
	@set -e; for test in $(KERNEL_TEST_BINS); do $$test; done

$(BUILD)/tests/kernel-signal: KERNEL_TEST_FLAGS = -pthread

$(BUILD)/tests/kernel-%: tests/kernel-%.c tests/kernel-test.h tests/kernel-readcache-stub.c $(wildcard kernel/*/*.c kernel/*/*.h include/flux/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(KERNEL_TEST_FLAGS) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
		-Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -ffunction-sections -fdata-sections \
		-Iinclude -Ikernel/core -Ikernel/fs -Ikernel/net -Ikernel/usb -Ikernel/input -Ikernel/storage -Ikernel/sound -Ikernel/video -Ikernel/wifi $< tests/kernel-readcache-stub.c -Wl,--gc-sections -o $@

.PHONY: test-kernel-qemu
# Override to exercise other core counts, nosmp and maxcpus without touching a real disk.
KERNEL_TEST_CPUS ?= $(QEMU_SMP)
# Install from a CD made by mkiso (GRUB from firmware/grub) onto a blank disk
# with fluxinstall, then start the computer from that disk alone.
test-install-qemu: $(ISO) $(BUILD)/tests/install-guest
	python3 tools/test-install-qemu.py $(QEMU) $(MKISO) $(BUILD)/iso firmware/grub $(BUILD)/tests/install-guest

$(BUILD)/tests/install-guest.o: tests/install-guest.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/tests/install-guest: $(BUILD)/tests/install-guest.o $(CRT0) $(LIBC) user/user.ld
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $< $(LIBC)

KERNEL_TEST_MEMORY ?= 256
KERNEL_TEST_CMDLINE ?=
# The card the mixer test plays on (--sound=sb16, --sound=usb), or empty for none.
KERNEL_TEST_SOUND ?= --sound
test-kernel-qemu: $(TEXT_KERNEL) $(SYSTEM) $(BUILD)/tests/kernel-guest
	python3 tools/test-kernel-qemu.py $(QEMU) $(TEXT_KERNEL) $(SYSTEM) $(BUILD)/tests/kernel-guest --cpus $(KERNEL_TEST_CPUS) --memory $(KERNEL_TEST_MEMORY) $(TEST_DROP_TOOLCHAIN) --append "$(KERNEL_TEST_CMDLINE)" --fsck-ext2 $(KERNEL_TEST_SOUND)

$(BUILD)/tests/kernel-guest.o: tests/kernel-guest.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/tests/kernel-guest: $(BUILD)/tests/kernel-guest.o $(CRT0) $(LIBC) user/user.ld
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $< $(LIBC)

$(BUILD)/tests/browser-style: tests/browser-style.c user/desktop/wave/html.c user/desktop/wave/html_entities.h user/desktop/wave/css.c user/desktop/wave/csscolor.c user/desktop/wave/anim.c user/desktop/wave/shadow.c user/desktop/wave/webutil.c user/desktop/wave/web.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
		$(DESKTOP_INC) tests/browser-style.c user/desktop/wave/html.c user/desktop/wave/css.c user/desktop/wave/csscolor.c \
		user/desktop/wave/anim.c user/desktop/wave/shadow.c user/desktop/wave/webutil.c -lm -o $@

$(BUILD)/tests/browser-layout: tests/browser-layout.c user/desktop/wave/layout.c user/desktop/wave/html.c user/desktop/wave/css.c user/desktop/wave/csscolor.c user/desktop/wave/cssfont.c user/desktop/wave/anim.c user/desktop/wave/webutil.c $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -msse -Wall -Wextra -Wno-unused-parameter $(DESKTOP_INC) -Iinclude \
		tests/browser-layout.c user/desktop/wave/layout.c user/desktop/wave/shadow.c user/desktop/wave/html.c user/desktop/wave/css.c \
		user/desktop/wave/csscolor.c user/desktop/wave/cssfont.c user/desktop/wave/anim.c user/desktop/wave/webutil.c -lm -o $@

# TLS tests use temporary certificates and local OpenSSL servers; never the Internet.
$(BUILD)/tests/tls-client: tests/tls-client.c user/libc/tls.c user/libc/tlsverify.c lib/sha256.c $(MBEDTLS_SRCS) $(MBEDTLS_DIR)/flux_config.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Iinclude -idirafter user/include $(MBEDTLS_FLAGS) \
		-DTLS_CA_FILE='"$(CURDIR)/$(BUILD)/tests/tls/ca.pem"' \
		tests/tls-client.c user/libc/tls.c user/libc/tlsverify.c lib/sha256.c $(MBEDTLS_SRCS) -o $@
.PHONY: test-tls
test-tls: $(BUILD)/tests/tls-client $(BUILD)/tests/tls-signature
	python3 tools/test-tls.py

$(BUILD)/tests/browser-policy: tests/browser-policy.c user/desktop/wave/webpolicy.c user/desktop/wave/publicsuffix.h user/desktop/wave/cookies.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g $(DESKTOP_INC) -Iinclude -idirafter user/include \
		-DCOOKIE_FILE='"$(CURDIR)/$(BUILD)/tests/policy-cookies"' tests/browser-policy.c user/desktop/wave/webpolicy.c user/desktop/wave/cookies.c -o $@

$(BUILD)/tests/browser-http: tests/browser-http.c $(BROWSER_TEST_SRCS) $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -DJS_HOST_TEST -DIDB_DIR='"$(CURDIR)/$(BUILD)/tests/idb/"' -DJS_VM_VERIFY -pthread $(DESKTOP_INC) -Iinclude -idirafter user/include \
		tests/browser-http.c $(filter-out tests/browser-dom.c,$(BROWSER_TEST_SRCS)) \
		-lm -o $@

$(BUILD)/tests/browser-font: tests/browser-font.c user/desktop/wave/webfont.c user/desktop/wave/webutil.c user/libc/inflate.c $(FONT_SRCS) $(FREETYPE_DIR)/flux_ftoption.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -msse $(DESKTOP_INC) -idirafter user/include $(FONT_FLAGS) \
		-DFONT_FILE='"$(CURDIR)/$(BUILD)/tests/font-test.fnt"' -DNOTO_DIR='"$(CURDIR)/rootfs/usr/share/fonts/noto/"' \
		$(filter %.c,$^) -o $@
.PHONY: test-browser-fonts
test-browser-fonts: $(BUILD)/tests/browser-font
	python3 tools/test-browser-fonts.py

$(BUILD)/tests/tls-signature: tests/tls-signature.c user/libc/tlsverify.c $(MBEDTLS_SRCS) $(MBEDTLS_DIR)/flux_config.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Iuser/libc $(MBEDTLS_FLAGS) \
		-DTLS_CA_FILE='"$(CURDIR)/$(BUILD)/tests/tls/ca.pem"' \
		tests/tls-signature.c user/libc/tlsverify.c $(MBEDTLS_SRCS) -o $@

$(BUILD)/tests/wsc: user/bin/wsc.c user/libc/tls.c user/libc/tlsverify.c lib/sha256.c $(MBEDTLS_SRCS) $(MBEDTLS_DIR)/flux_config.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Iinclude -idirafter user/include $(MBEDTLS_FLAGS) \
		-DTLS_CA_FILE='"$(CURDIR)/$(BUILD)/tests/ws/ca.pem"' \
		user/bin/wsc.c user/libc/tls.c user/libc/tlsverify.c lib/sha256.c $(MBEDTLS_SRCS) -o $@
.PHONY: test-websocket
test-websocket: $(BUILD)/tests/wsc $(BUILD)/tests/browser-ws
	python3 tools/test-websocket.py

$(BUILD)/tests/browser-ws: tests/browser-ws.c $(BROWSER_TEST_SRCS) $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -DJS_HOST_TEST -DIDB_DIR='"$(CURDIR)/$(BUILD)/tests/idb/"' -DJS_VM_VERIFY -pthread $(DESKTOP_INC) -Iinclude -idirafter user/include \
		-DWS_HELPER='"$(CURDIR)/$(BUILD)/tests/wsc"' \
		tests/browser-ws.c $(filter-out tests/browser-dom.c,$(BROWSER_TEST_SRCS)) \
		-lm -o $@

# Engine-level tests include real collections on the host's main and coroutine stacks.
JS_ENGINE_SRCS := lib/digest.c lib/sha256.c tests/js-engine.c tests/js-host-stack.c \
	$(addprefix user/desktop/wave/js/,js.c jsbig.c jsuni.c jslib.c jslib2.c jsbinary.c jsre.c jsgc.c) \
	user/desktop/wave/webutil.c user/libc/inflate.c
$(BUILD)/tests/js-engine: $(JS_ENGINE_SRCS) $(DESKTOP_HDRS) include/flux/abi.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -DJS_HOST_TEST -DJS_VM_VERIFY -pthread \
		-Wall -Wextra -Wno-unused-parameter $(DESKTOP_INC) -Iinclude -idirafter user/include \
		$(JS_ENGINE_SRCS) -lm -o $@

.PHONY: test-js bench-js
test-js: $(BUILD)/tests/browser-dom $(BUILD)/tests/js-engine
	$(BUILD)/tests/browser-dom rootfs/usr/share/www/js-language-tests.js 134
	rm -rf $(BUILD)/tests/idb
	$(BUILD)/tests/browser-dom rootfs/usr/share/www/indexeddb-tests.js 3
	$(BUILD)/tests/js-engine

# Each case is a separate entry so the page's step budget applies per case.
bench-js: $(BUILD)/tests/browser-bench
	@set -e; for case in tests/js-bench/*.js; do \
		/usr/bin/time -f '%e seconds, %M KiB peak RSS' $< $$case 1; \
	done

# Measure the optimized VM without the test runner's per-instruction assertions.
$(BUILD)/tests/browser-bench: $(BROWSER_TEST_SRCS) $(DESKTOP_HDRS) user/include/flux.h include/flux/abi.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O2 -g -fno-pie -no-pie -DJS_HOST_TEST -DIDB_DIR='"$(CURDIR)/$(BUILD)/tests/idb/"' -pthread \
		$(DESKTOP_INC) -Iinclude -idirafter user/include $(BROWSER_TEST_SRCS) \
		-lm -o $@

.PHONY: test-browser-images
test-browser-images: $(BUILD)/tests/browser-images
	$<

$(BUILD)/tests/browser-images: tests/browser-images.c user/desktop/image/gif.c user/desktop/image/webp.c user/desktop/image/webp_tables.h $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra $(DESKTOP_INC) tests/browser-images.c user/desktop/image/gif.c user/desktop/image/webp.c -o $@

.PHONY: test-desktop-mouse
test-desktop-mouse: $(BUILD)/tests/desktop-mouse
	$<

$(BUILD)/tests/desktop-mouse: tests/desktop-mouse.c user/desktop/apps/mouse.c user/desktop/shell/mousecfg.c $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections $(DESKTOP_INC) -Iinclude $< -Wl,--gc-sections -o $@

.PHONY: test-desktop-jobs
test-desktop-jobs: $(BUILD)/tests/desktop-imagejobs
	$<

$(BUILD)/tests/desktop-imagejobs: tests/desktop-imagejobs.c tests/desktop-host.h user/desktop/shell/imagejobs.c $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra $(DESKTOP_INC) -Iinclude -include tests/desktop-host.h tests/desktop-imagejobs.c -o $@

$(BUILD)/tests/desktop-imagejobs-guest.o: tests/desktop-imagejobs.c user/desktop/shell/imagejobs.c $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(DESKTOP_INC) -DIMAGEJOBS_GUEST -c $< -o $@

$(BUILD)/tests/desktop-imagejobs-guest: $(BUILD)/tests/desktop-imagejobs-guest.o $(CRT0) $(LIBC) user/user.ld
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $< $(LIBC)

.PHONY: test-desktop-jobs-qemu
test-desktop-jobs-qemu: $(TEXT_KERNEL) $(SYSTEM) $(BUILD)/tests/desktop-imagejobs-guest
	python3 tools/test-kernel-qemu.py $(QEMU) $(TEXT_KERNEL) $(SYSTEM) $(BUILD)/tests/desktop-imagejobs-guest --cpus $(KERNEL_TEST_CPUS) $(TEST_DROP_TOOLCHAIN) --success-marker "DESKTOP JOB REGRESSIONS PASSED"

$(BUILD)/tests/browser-navigation: tests/browser-navigation.c user/desktop/wave/webutil.c $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra $(DESKTOP_INC) tests/browser-navigation.c user/desktop/wave/webutil.c -o $@

$(BUILD)/tests/browser-drag: tests/browser-drag.c $(BROWSER_TEST_SRCS) $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -DJS_HOST_TEST -DIDB_DIR='"$(CURDIR)/$(BUILD)/tests/idb/"' -DJS_VM_VERIFY -pthread $(DESKTOP_INC) -Iinclude -idirafter user/include \
		tests/browser-drag.c $(filter-out tests/browser-dom.c,$(BROWSER_TEST_SRCS)) -lm -o $@

# Workers are processes: the page side runs in browser-worker, each worker in jsworker (its "wave --js-worker").
WORKER_TEST_SRCS := $(filter-out tests/browser-dom.c,$(BROWSER_TEST_SRCS))
$(BUILD)/tests/jsworker: tests/jsworker-host.c $(WORKER_TEST_SRCS) $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -DJS_HOST_TEST -DIDB_DIR='"$(CURDIR)/$(BUILD)/tests/idb/"' -DJS_VM_VERIFY -pthread $(DESKTOP_INC) -Iinclude -idirafter user/include \
		-DWORKER_HELPER='"$(CURDIR)/$@"' tests/jsworker-host.c $(WORKER_TEST_SRCS) -lm -o $@

$(BUILD)/tests/browser-worker: tests/browser-worker.c $(BROWSER_TEST_SRCS) $(DESKTOP_HDRS) $(BUILD)/tests/jsworker
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -DJS_HOST_TEST -DIDB_DIR='"$(CURDIR)/$(BUILD)/tests/idb/"' -DJS_VM_VERIFY -pthread $(DESKTOP_INC) -Iinclude -idirafter user/include \
		-DWORKER_HELPER='"$(CURDIR)/$(BUILD)/tests/jsworker"' tests/browser-worker.c $(WORKER_TEST_SRCS) -lm -o $@

$(BUILD)/tests/browser-geometry: tests/browser-geometry.c $(BROWSER_TEST_SRCS) $(DESKTOP_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -DJS_HOST_TEST -DIDB_DIR='"$(CURDIR)/$(BUILD)/tests/idb/"' -DJS_VM_VERIFY -pthread $(DESKTOP_INC) -Iinclude -idirafter user/include \
		tests/browser-geometry.c $(filter-out tests/browser-dom.c,$(BROWSER_TEST_SRCS)) -lm -o $@

# The system image carries the toolchain (about 90 MB) once it is built; the tests
# that do not run it leave it out of their in-memory initrd (the kernel heap, a
# quarter of RAM, holds the files).
TEST_DROP_TOOLCHAIN := --drop usr/bin --drop usr/libexec --drop usr/lib --drop usr/include

# S3 must boot through firmware/GRUB: QEMU -kernel reloads RAM on wake.
.PHONY: test-suspend-qemu
test-suspend-qemu: $(TEXT_KERNEL) $(SYSTEM) $(BUILD)/tests/kernel-suspend $(MKISO) $(GRUB_FIRMWARE)
	python3 tools/test-suspend-qemu.py $(QEMU) $(TEXT_KERNEL) $(SYSTEM) $(BUILD)/tests/kernel-suspend $(MKISO) firmware/grub --cpus $(KERNEL_TEST_CPUS) $(TEST_DROP_TOOLCHAIN)

$(BUILD)/tests/kernel-suspend.o: tests/kernel-suspend.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/tests/kernel-suspend: $(BUILD)/tests/kernel-suspend.o $(CRT0) $(LIBC) user/user.ld
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $< $(LIBC)
