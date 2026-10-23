# fluxOS build
#
#   make            build the bootable ISO (fluxOS.iso)
#   make run        boot the ISO in QEMU (serial console on stdio)
#   make run-kernel boot the kernel directly with QEMU's multiboot loader
#                   (text console only: that loader cannot set a video mode)
#   make run-disk   boot fluxdisk.img alone (after installing onto it)
#   make check      verify the kernel is a valid multiboot image
#   make disk       create fluxdisk.img, a FAT32 data disk (kept by make clean)
#   make clean      remove build artifacts

CC      ?= gcc
LD      ?= ld
AR      ?= ar
QEMU    ?= qemu-system-i386
MKRESCUE ?= grub-mkrescue
GRUB_MKIMAGE ?= grub-mkimage
GRUB_PC_DIR ?= /usr/lib/grub/i386-pc

BUILD   := build
ISO     := fluxOS.iso
KERNEL  := $(BUILD)/fluxos.kernel
TEXT_KERNEL := $(BUILD)/fluxos-text.kernel
INITRD  := $(BUILD)/initrd.tar
ROOTFS  := $(BUILD)/rootfs
DISK    := fluxdisk.img
DISK_MB ?= 64
MKFS_FAT ?= $(shell command -v mkfs.fat 2>/dev/null || ls /usr/sbin/mkfs.fat /sbin/mkfs.fat 2>/dev/null | head -1)

GCC_INC := $(shell $(CC) -print-file-name=include)

CFLAGS_COMMON := -m32 -march=i686 -std=gnu11 -O2 -g \
	-ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
	-nostdinc -isystem $(GCC_INC) -mgeneral-regs-only \
	-fno-asynchronous-unwind-tables -fno-strict-aliasing \
	-fno-tree-loop-distribute-patterns \
	-Wall -Wextra -Wno-unused-parameter -MMD -MP

KERNEL_CFLAGS := $(CFLAGS_COMMON) -fno-omit-frame-pointer -Iinclude -Ikernel/include -Ikernel
USER_CFLAGS   := $(CFLAGS_COMMON) -Iinclude -Iuser/include

LDFLAGS_COMMON := -m elf_i386 -nostdlib -z noexecstack --no-warn-rwx-segments

# ---- kernel ----
KERNEL_SRCS := kernel/boot.S kernel/entry.S $(wildcard kernel/*.c) lib/string.c lib/vsnprintf.c lib/font8x16.c
KERNEL_OBJS := $(patsubst %,$(BUILD)/k/%.o,$(basename $(KERNEL_SRCS)))

# ---- user space ----
LIBC_SRCS := $(wildcard user/libc/*.c) lib/string.c lib/vsnprintf.c lib/font8x16.c
LIBC_OBJS := $(patsubst %,$(BUILD)/u/%.o,$(basename $(LIBC_SRCS)))
CRT0      := $(BUILD)/u/user/libc/crt0.o
LIBC      := $(BUILD)/libc.a

PROGS     := $(basename $(notdir $(wildcard user/bin/*.c)))
FSH_OBJS  := $(patsubst %,$(BUILD)/u/%.o,$(basename $(wildcard user/fsh/*.c)))
DESKTOP_OBJS := $(patsubst %,$(BUILD)/u/%.o,$(basename $(wildcard user/desktop/*.c)))
BINS      := $(addprefix $(ROOTFS)/bin/,$(PROGS)) $(ROOTFS)/bin/fsh $(ROOTFS)/bin/desktop

ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)

.PHONY: all iso kernel initrd disk run run-kernel run-disk check clean
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

$(ROOTFS)/bin/fsh: $(CRT0) $(FSH_OBJS) $(LIBC) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $(FSH_OBJS) $(LIBC)

$(ROOTFS)/bin/desktop: $(CRT0) $(DESKTOP_OBJS) $(LIBC) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS_COMMON) -s -T user/user.ld -o $@ $(CRT0) $(DESKTOP_OBJS) $(LIBC)

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

$(ISO): $(KERNEL) $(INITRD) grub/grub.cfg
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
	$(MKRESCUE) -o $@ $(BUILD)/iso

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

QEMU_DISK := -drive file=$(DISK),format=raw,if=ide,index=0
# An NE2000 card on QEMU's user-mode network: DHCP, DNS and NAT to the host's network.
QEMU_NET  ?= -netdev user,id=net0 -device ne2k_pci,netdev=net0

run: $(ISO) $(DISK)
	$(QEMU) -m 128M -serial stdio -boot d -cdrom $(ISO) $(QEMU_DISK) $(QEMU_NET)

run-kernel: $(TEXT_KERNEL) $(INITRD) $(DISK)
	$(QEMU) -kernel $(TEXT_KERNEL) -initrd $(INITRD) -m 128M -serial stdio $(QEMU_DISK) $(QEMU_NET)

# Boot fluxdisk.img on its own, after "fluxinstall /dev/hda".
run-disk: $(DISK)
	$(QEMU) -m 128M -serial stdio -boot c $(QEMU_DISK) $(QEMU_NET)

clean:
	rm -rf $(BUILD) $(ISO)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
