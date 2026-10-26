# fluxOS build
#
#   make            build the bootable ISO (fluxOS.iso)
#   make run        boot the ISO in QEMU (serial console on stdio)
#   make run-kernel boot the kernel directly with QEMU's multiboot loader
#                   (text console only: that loader cannot set a video mode)
#   make run-disk   boot fluxdisk.img alone (after installing onto it)
#   make run-q35    boot the ISO on a q35 machine: fluxdisk.img and the CD on
#                   its SATA (AHCI) controller, a USB keyboard and tablet on xHCI
#   make run-usb    boot the ISO with fluxdisk.img as a USB stick on EHCI, and
#                   a USB keyboard and mouse on UHCI
#   make check      verify the kernel is a valid multiboot image
#   make disk       create fluxdisk.img, a FAT32 data disk (kept by make clean)
#   make floppy     create fluxfloppy.img, a 1.44 MB FAT12 floppy that the
#                   QEMU targets then put in drive A: (kept by make clean)
#   make clean      remove build artifacts

CC      ?= gcc
LD      ?= ld
AR      ?= ar
QEMU    ?= qemu-system-i386
# The oldest processor fluxOS runs on, so that testing catches anything newer.
QEMU_CPU ?= -cpu pentium3
# GRUB 2.06 from Debian 12, fetched once by tools/fetch-grub.sh (2.12 reboots
# some older PCs right after "Welcome to GRUB!"). For the host's own GRUB:
#   make GRUB=/ GRUB_DEP=
GRUB    ?= grub-2.06
GRUB_DEP ?= $(GRUB)/usr/bin/grub-mkimage
MKRESCUE ?= $(GRUB)/usr/bin/grub-mkrescue
GRUB_MKIMAGE ?= $(GRUB)/usr/bin/grub-mkimage
GRUB_PC_DIR ?= $(GRUB)/usr/lib/grub/i386-pc

BUILD   := build
ISO     := fluxOS.iso
KERNEL  := $(BUILD)/fluxos.kernel
TEXT_KERNEL := $(BUILD)/fluxos-text.kernel
INITRD  := $(BUILD)/initrd.tar
ROOTFS  := $(BUILD)/rootfs
DISK    := fluxdisk.img
DISK_MB ?= 64
FLOPPY  := fluxfloppy.img
MKFS_FAT ?= $(shell command -v mkfs.fat 2>/dev/null || ls /usr/sbin/mkfs.fat /sbin/mkfs.fat 2>/dev/null | head -1)

GCC_INC := $(shell $(CC) -print-file-name=include)

# fluxOS needs a Pentium III: everything is built for its instruction set,
# SSE included (kernel/cpu.c stops the boot on anything older).
CFLAGS_COMMON := -m32 -march=pentium3 -std=gnu11 -O2 -g \
	-ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
	-nostdinc -isystem $(GCC_INC) \
	-fno-asynchronous-unwind-tables -fno-strict-aliasing \
	-fno-tree-loop-distribute-patterns \
	-Wall -Wextra -Wno-unused-parameter -MMD -MP

# The kernel's C code keeps off the FPU and SSE registers, which belong to
# user processes (fpu.c switches them lazily); kernel/sse.S borrows the SSE
# registers for bulk copies, saving what it touches.
KERNEL_CFLAGS := $(CFLAGS_COMMON) -mgeneral-regs-only -fno-omit-frame-pointer \
	-Iinclude -Ikernel/include -Ikernel
# User code does float arithmetic on SSE (doubles stay on the x87, which
# SSE1 cannot do) and may use the SSE and MMX intrinsics.
USER_CFLAGS   := $(CFLAGS_COMMON) -mfpmath=sse -fno-math-errno -Iinclude -Iuser/include

# What runs before kernel/cpu.c has checked the processor (the console, the
# string routines and kmain) is built for any 386, so that an older machine
# shows why it cannot run fluxOS instead of hanging on an i686 instruction.
KERNEL_EARLY_OBJS := $(addprefix $(BUILD)/k/,kernel/kmain.o kernel/cpu.o kernel/kprintf.o \
	kernel/vga.o kernel/fb.o kernel/serial.o lib/string.o lib/vsnprintf.o lib/font8x16.o)
$(KERNEL_EARLY_OBJS): KERNEL_CFLAGS += -march=i386 -mtune=pentium3

LDFLAGS_COMMON := -m elf_i386 -nostdlib -z noexecstack --no-warn-rwx-segments

# ---- kernel ----
KERNEL_SRCS := kernel/boot.S kernel/entry.S kernel/sse.S kernel/radeon_fw.S $(wildcard kernel/*.c) lib/string.c lib/vsnprintf.c lib/font8x16.c \
               lib/sha256.c
KERNEL_OBJS := $(patsubst %,$(BUILD)/k/%.o,$(basename $(KERNEL_SRCS)))

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
$(FONT_OBJS) $(BUILD)/u/user/desktop/webfont.o: USER_CFLAGS += $(FONT_FLAGS)
$(LIBFONT): $(FONT_OBJS)
	$(AR) rcs $@ $^

LIBC_SRCS := $(wildcard user/libc/*.c) $(MBEDTLS_SRCS) lib/string.c lib/vsnprintf.c lib/font8x16.c lib/sha256.c
LIBC_OBJS := $(patsubst %,$(BUILD)/u/%.o,$(basename $(LIBC_SRCS)))
CRT0      := $(BUILD)/u/user/libc/crt0.o
LIBC      := $(BUILD)/libc.a
GL_OBJS   := $(patsubst %,$(BUILD)/u/%.o,$(basename $(wildcard user/gl/*.c)))
LIBGL     := $(BUILD)/libGL.a

PROGS     := $(basename $(notdir $(wildcard user/bin/*.c)))
FSH_OBJS  := $(patsubst %,$(BUILD)/u/%.o,$(basename $(wildcard user/fsh/*.c)))
DESKTOP_OBJS := $(patsubst %,$(BUILD)/u/%.o,$(basename $(wildcard user/desktop/*.c)))
DOS_OBJS  := $(patsubst %,$(BUILD)/u/%.o,$(basename $(wildcard user/dos/*.c)))
BINS      := $(addprefix $(ROOTFS)/bin/,$(PROGS)) $(ROOTFS)/bin/fsh $(ROOTFS)/bin/desktop \
             $(ROOTFS)/bin/dos

ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)

.PHONY: all iso kernel initrd disk floppy run run-kernel run-disk run-q35 run-usb check clean
.SECONDARY:

all: $(ISO)
iso: $(ISO)
kernel: $(KERNEL)
initrd: $(INITRD)

$(BUILD)/k/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/k/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# The Radeon microcode is pulled in with .incbin.
$(BUILD)/k/kernel/radeon_fw.o: $(wildcard firmware/radeon/*.bin)

$(KERNEL): $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS_COMMON) -T linker.ld -o $@ $(KERNEL_OBJS)

# The same kernel without the Multiboot video request, for run-kernel.
$(BUILD)/k/kernel/boot-text.o: kernel/boot.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -DMB_TEXT_ONLY -c $< -o $@

TEXT_KERNEL_OBJS := $(BUILD)/k/kernel/boot-text.o $(filter-out $(BUILD)/k/kernel/boot.o,$(KERNEL_OBJS))

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

$(ROOTFS)/bin/desktop: $(CRT0) $(DESKTOP_OBJS) $(LIBGL) $(LIBFONT) $(LIBC) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $(DESKTOP_OBJS) $(LIBGL) $(LIBFONT) $(LIBC)

$(ROOTFS)/bin/dos: $(CRT0) $(DOS_OBJS) $(LIBC) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $(DOS_OBJS) $(LIBC)

$(ROOTFS)/bin/%: $(BUILD)/u/user/bin/%.o $(CRT0) $(LIBC) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $< $(LIBC)

$(BUILD)/.rootfs-stamp: $(ROOTFS_FILES)
	@mkdir -p $(ROOTFS)
	cp -r rootfs/. $(ROOTFS)/
	@touch $@

$(INITRD): $(BUILD)/.rootfs-stamp $(BINS)
	cp $(ROOTFS)/bin/fsh $(ROOTFS)/bin/sh
	chmod 755 $(ROOTFS)/home/flux/demo.sh
	tar --format=ustar --owner=0 --group=0 --numeric-owner -cf $@ -C $(ROOTFS) .

$(GRUB)/usr/bin/grub-mkimage:
	sh tools/fetch-grub.sh $(GRUB)

$(ISO): $(KERNEL) $(INITRD) grub/grub.cfg $(GRUB_DEP)
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/fluxos.kernel
	cp $(INITRD) $(BUILD)/iso/boot/initrd.tar
	cp grub/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	@# GRUB for hard disks, written to disk by /bin/fluxinstall: boot.img
	@# goes in the MBR, core.img in the gap before the first partition and
	@# reads its modules from /boot/grub on that FAT32 partition.
	@mkdir -p $(BUILD)/iso/boot/install
	$(GRUB_MKIMAGE) -O i386-pc -d $(GRUB_PC_DIR) -o $(BUILD)/iso/boot/install/core.img \
		-p '(,msdos1)/boot/grub' biosdisk part_msdos fat
	cp $(GRUB_PC_DIR)/boot.img $(BUILD)/iso/boot/install/boot.img
	PATH="$(abspath $(GRUB))/usr/bin:$$PATH" $(MKRESCUE) -d $(GRUB_PC_DIR) -o $@ $(BUILD)/iso

check: $(KERNEL)
	grub-file --is-x86-multiboot $(KERNEL) && echo "$(KERNEL): valid multiboot kernel"

disk: $(DISK)

# A blank FAT32 disk; without a host mkfs.fat it stays unformatted and can
# be formatted from inside fluxOS with "mkfs.fat /dev/hda".
$(DISK):
	truncate -s $(DISK_MB)M $@
	@if [ -n "$(MKFS_FAT)" ]; then $(MKFS_FAT) -F 32 -n FLUXDISK $@ >/dev/null && \
		echo "$@: $(DISK_MB) MiB FAT32"; \
	else echo "$@: blank, run 'mkfs.fat /dev/hda' inside fluxOS"; fi

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
	$(QEMU) $(QEMU_CPU) -m 1G -serial stdio -boot d -cdrom $(ISO) $(QEMU_DISK) $(QEMU_NET) $(QEMU_SND)

run-kernel: $(TEXT_KERNEL) $(INITRD) $(DISK)
	$(QEMU) $(QEMU_CPU) -kernel $(TEXT_KERNEL) -initrd $(INITRD) -m 256M -serial stdio $(QEMU_DISK) $(QEMU_NET) $(QEMU_SND)

# Boot fluxdisk.img on its own, after "fluxinstall /dev/hda".
run-disk: $(DISK)
	$(QEMU) $(QEMU_CPU) -m 256M -serial stdio -boot c $(QEMU_DISK) $(QEMU_NET) $(QEMU_SND)

# On q35 "if=ide" drives and the CD sit on the chipset's AHCI controller.
QEMU_XHCI ?= -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0

run-q35: $(ISO) $(DISK)
	$(QEMU) $(QEMU_CPU) -M q35 -m 256M -serial stdio -boot d -cdrom $(ISO) \
		-drive file=$(DISK),format=raw,if=ide,index=0 $(QEMU_NET) $(QEMU_SND) $(QEMU_XHCI)

QEMU_USB ?= -usb -device usb-kbd,bus=usb-bus.0 -device usb-mouse,bus=usb-bus.0 \
	-device usb-ehci,id=ehci -drive file=$(DISK),format=raw,if=none,id=stick \
	-device usb-storage,bus=ehci.0,drive=stick

run-usb: $(ISO) $(DISK)
	$(QEMU) $(QEMU_CPU) -m 256M -serial stdio -boot d -cdrom $(ISO) $(QEMU_NET) $(QEMU_SND) $(QEMU_USB)

clean:
	rm -rf $(BUILD) $(ISO)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

# Run the browser's actual HTML/DOM/JS code on the host, without booting QEMU.
HOSTCC ?= cc
BROWSER_TEST_SRCS := tests/browser-dom.c $(addprefix user/desktop/,js.c jslib.c jslib2.c jsbinary.c jsnet.c jsre.c jsgc.c jsdom.c html.c shadow.c webutil.c webpolicy.c webstore.c css.c csscolor.c cssfont.c)
.PHONY: test-browser
test-browser: $(BUILD)/tests/browser-dom $(BUILD)/tests/browser-style $(BUILD)/tests/browser-layout $(BUILD)/tests/browser-policy $(BUILD)/tests/browser-http
	$< rootfs/usr/share/www/classlist-tests.js 20
	$< rootfs/usr/share/www/dom-feature-tests.js 31
	$(BUILD)/tests/browser-style
	$(BUILD)/tests/browser-layout
	$(BUILD)/tests/browser-policy
	$(BUILD)/tests/browser-http
	$(BUILD)/tests/browser-dom rootfs/usr/share/www/modern-core-tests.js 44

$(BUILD)/tests/browser-dom: $(BROWSER_TEST_SRCS) $(wildcard user/desktop/*.h) user/include/flux.h include/flux/abi.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -Wall -Wextra -Wno-unused-parameter \
		-Iuser/desktop -Iinclude -idirafter user/include $(BROWSER_TEST_SRCS) \
		-Wl,--defsym=__gc_data_start=__data_start,--defsym=__gc_data_end=_end -lm -o $@

# Host regression tests use production kernel code with mocked hardware/I/O.
KERNEL_TEST_NAMES := heap pipe socket storage syscall
KERNEL_TEST_BINS := $(addprefix $(BUILD)/tests/kernel-,$(KERNEL_TEST_NAMES))
.PHONY: test-kernel
test-kernel: $(KERNEL_TEST_BINS)
	@set -e; for test in $(KERNEL_TEST_BINS); do $$test; done

$(BUILD)/tests/kernel-%: tests/kernel-%.c tests/kernel-test.h $(wildcard kernel/*.c kernel/*.h include/flux/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
		-Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -ffunction-sections -fdata-sections \
		-Iinclude -Ikernel $< -Wl,--gc-sections -o $@

.PHONY: test-kernel-qemu
test-kernel-qemu: $(TEXT_KERNEL) $(INITRD) $(BUILD)/tests/kernel-guest
	python3 tools/test-kernel-qemu.py $(QEMU) $(TEXT_KERNEL) $(INITRD) $(BUILD)/tests/kernel-guest

$(BUILD)/tests/kernel-guest.o: tests/kernel-guest.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/tests/kernel-guest: $(BUILD)/tests/kernel-guest.o $(CRT0) $(LIBC) user/user.ld
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $< $(LIBC)

$(BUILD)/tests/browser-style: tests/browser-style.c user/desktop/html.c user/desktop/html_entities.h user/desktop/css.c user/desktop/csscolor.c user/desktop/webutil.c user/desktop/web.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
		-Iuser/desktop tests/browser-style.c user/desktop/html.c user/desktop/css.c user/desktop/csscolor.c \
		user/desktop/webutil.c -lm -o $@

$(BUILD)/tests/browser-layout: tests/browser-layout.c $(addprefix user/desktop/,layout.c html.c css.c csscolor.c cssfont.c webutil.c) $(wildcard user/desktop/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -msse -Wall -Wextra -Wno-unused-parameter -Iuser/desktop -Iinclude \
		tests/browser-layout.c user/desktop/layout.c user/desktop/shadow.c user/desktop/html.c user/desktop/css.c \
		user/desktop/csscolor.c user/desktop/cssfont.c user/desktop/webutil.c -lm -o $@

# TLS tests use temporary certificates and local OpenSSL servers; never the Internet.
$(BUILD)/tests/tls-client: tests/tls-client.c user/libc/tls.c user/libc/tlsverify.c lib/sha256.c $(MBEDTLS_SRCS) $(MBEDTLS_DIR)/flux_config.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Iinclude -idirafter user/include $(MBEDTLS_FLAGS) \
		-DTLS_CA_FILE='"$(CURDIR)/$(BUILD)/tests/tls/ca.pem"' \
		tests/tls-client.c user/libc/tls.c user/libc/tlsverify.c lib/sha256.c $(MBEDTLS_SRCS) -o $@
.PHONY: test-tls
test-tls: $(BUILD)/tests/tls-client $(BUILD)/tests/tls-signature
	python3 tools/test-tls.py

$(BUILD)/tests/browser-policy: tests/browser-policy.c user/desktop/webpolicy.c user/desktop/publicsuffix.h user/desktop/cookies.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -Iuser/desktop -Iinclude -idirafter user/include \
		-DCOOKIE_FILE='"$(CURDIR)/$(BUILD)/tests/policy-cookies"' tests/browser-policy.c user/desktop/webpolicy.c user/desktop/cookies.c -o $@

$(BUILD)/tests/browser-http: tests/browser-http.c $(BROWSER_TEST_SRCS) $(wildcard user/desktop/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -Iuser/desktop -Iinclude -idirafter user/include \
		tests/browser-http.c $(filter-out tests/browser-dom.c,$(BROWSER_TEST_SRCS)) \
		-Wl,--defsym=__gc_data_start=__data_start,--defsym=__gc_data_end=_end -lm -o $@

$(BUILD)/tests/browser-font: tests/browser-font.c user/desktop/webfont.c user/desktop/webutil.c $(FONT_SRCS) $(FREETYPE_DIR)/flux_ftoption.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -msse -Iuser/desktop $(FONT_FLAGS) \
		-DFONT_FILE='"$(CURDIR)/$(BUILD)/tests/font-test.fnt"' $(filter %.c,$^) -o $@
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

$(BUILD)/tests/browser-ws: tests/browser-ws.c $(BROWSER_TEST_SRCS) $(wildcard user/desktop/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -g -fno-pie -no-pie -Iuser/desktop -Iinclude -idirafter user/include \
		-DWS_HELPER='"$(CURDIR)/$(BUILD)/tests/wsc"' \
		tests/browser-ws.c $(filter-out tests/browser-dom.c,$(BROWSER_TEST_SRCS)) \
		-Wl,--defsym=__gc_data_start=__data_start,--defsym=__gc_data_end=_end -lm -o $@
