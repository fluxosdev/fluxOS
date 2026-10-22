# fluxOS build
#
#   make            build the bootable ISO (fluxOS.iso)
#   make run        boot the ISO in QEMU (serial console on stdio)
#   make run-kernel boot the kernel directly with QEMU's multiboot loader
#   make check      verify the kernel is a valid multiboot image
#   make clean      remove build artifacts

CC      ?= gcc
LD      ?= ld
AR      ?= ar
QEMU    ?= qemu-system-i386
MKRESCUE ?= grub-mkrescue

BUILD   := build
ISO     := fluxOS.iso
KERNEL  := $(BUILD)/fluxos.kernel
INITRD  := $(BUILD)/initrd.tar
ROOTFS  := $(BUILD)/rootfs

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
KERNEL_SRCS := kernel/boot.S kernel/entry.S $(wildcard kernel/*.c) lib/string.c lib/vsnprintf.c
KERNEL_OBJS := $(patsubst %,$(BUILD)/k/%.o,$(basename $(KERNEL_SRCS)))

# ---- user space ----
LIBC_SRCS := $(wildcard user/libc/*.c) lib/string.c lib/vsnprintf.c
LIBC_OBJS := $(patsubst %,$(BUILD)/u/%.o,$(basename $(LIBC_SRCS)))
CRT0      := $(BUILD)/u/user/libc/crt0.o
LIBC      := $(BUILD)/libc.a

PROGS     := $(basename $(notdir $(wildcard user/bin/*.c)))
FSH_OBJS  := $(patsubst %,$(BUILD)/u/%.o,$(basename $(wildcard user/fsh/*.c)))
BINS      := $(addprefix $(ROOTFS)/bin/,$(PROGS)) $(ROOTFS)/bin/fsh

ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)

.PHONY: all iso kernel initrd run run-kernel check clean
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
	$(MKRESCUE) -o $@ $(BUILD)/iso

check: $(KERNEL)
	grub-file --is-x86-multiboot $(KERNEL) && echo "$(KERNEL): valid multiboot kernel"

run: $(ISO)
	$(QEMU) -cdrom $(ISO) -m 128M -serial stdio

run-kernel: $(KERNEL) $(INITRD)
	$(QEMU) -kernel $(KERNEL) -initrd $(INITRD) -m 128M -serial stdio

clean:
	rm -rf $(BUILD) $(ISO)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
