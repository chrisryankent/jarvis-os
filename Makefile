# =============================================================================
# JARVIS AI OS — project root build wrapper
#
# Builds the kernel subproject and creates a bootable ISO image from the
# generated kernel.elf and GRUB configuration.
# =============================================================================

CC      := gcc
CXX     := g++
AS      := as
NASM    := nasm
LD      := ld
PYTHON  := python3

CFLAGS  := -std=c17 -ffreestanding -fno-stack-protector -fno-pic \
           -mno-red-zone -mcmodel=kernel -m64 -O2 -Wall -Wextra \
           -Wno-unused-parameter -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
CXXFLAGS := -std=c++20 -ffreestanding -fno-exceptions -fno-rtti \
            -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=kernel \
            -m64 -O2 -Wall -Wextra -Wno-unused-parameter \
            -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
ASFLAGS := --64
NASMFLAGS := -f elf64
LDFLAGS := -T kernel/linker.ld
ISO_DIR := iso
ISO_OUT := build/jarvis.iso

KERNEL_CORE := kernel/kernel_core.o
BOOT_OBJS := boot/multiboot2.o
DRIVER_OBJS := drivers/ahci.o drivers/audio.o drivers/gpu.o drivers/input.o \
               drivers/nic.o drivers/pci.o drivers/usb.o
# desktop_init.c is a unity-style desktop bundle: it includes the taskbar,
# theme, notifications, input queue, animations, and built-in apps. Compile
# those sources only through desktop_init.o to avoid duplicate definitions.
DESKTOP_OBJS := desktop/desktop_init.o desktop/ui_shell.o
WM_OBJS := wm/wm.o wm/titlebar.o
INPUT_OBJS :=
AI_OBJS := ai_core/runtime.o
SECURITY_OBJS := security/security.o
SPEECH_OBJS := speech/speech_engine.o
EXTRA_OBJS := kernel/syscall_entry.o
OBJS := $(BOOT_OBJS) $(DRIVER_OBJS) $(DESKTOP_OBJS) $(WM_OBJS) \
        $(INPUT_OBJS) $(AI_OBJS) $(SECURITY_OBJS) $(SPEECH_OBJS) \
        $(EXTRA_OBJS)

QEMU      := qemu-system-x86_64
# The kernel reserves a 256 MB model pool plus ~37 MB of .bss, so QEMU's
# default 128 MB is not enough.
QEMU_MEM  := 2G
# -rtc base=localtime: the taskbar clock shows your local time, not UTC.
QEMU_FLAGS := -cdrom $(ISO_OUT) -m $(QEMU_MEM) -serial stdio -rtc base=localtime

# Keep the ISO small (about 5 MB instead of 25 MB): compress GRUB and leave
# out its locales, fonts and themes, none of which the boot menu needs.
GRUB_MKRESCUE_FLAGS := --compress=xz --locales= --fonts= --themes=

.PHONY: all clean check iso iso-models run qemu run-headless FORCE

all: kernel.elf

# Always ask the kernel sub-make whether anything changed; it only relinks
# kernel_core.o when one of its own sources is newer.
kernel/kernel_core.o: FORCE
	$(MAKE) -C kernel all

FORCE:

# desktop_init.c #includes these sources directly (unity build), so edits to
# them must rebuild desktop_init.o.
desktop/desktop_init.o: $(wildcard desktop/*.c desktop/*.h apps/*.c apps/*.h \
                          input/*.c input/*.h wm/*.h)

# Shared headers: a change to any of them rebuilds everything that uses them.
$(DRIVER_OBJS) $(DESKTOP_OBJS) $(WM_OBJS): kernel/kernel.h drivers/gpu.h \
                                           wm/wm.h desktop/ui.h

# Build the ISO and boot it in a QEMU window; kernel log goes to this terminal.
run: iso
	$(QEMU) $(QEMU_FLAGS)

qemu: run

# No window: serial log only (Ctrl-A then X to quit).
run-headless: iso
	$(QEMU) $(QEMU_FLAGS) -display none

kernel.elf: $(KERNEL_CORE) $(OBJS)
	$(LD) $(LDFLAGS) $(KERNEL_CORE) $(OBJS) -o $@

$(BOOT_OBJS): %.o: %.asm
	$(NASM) $(NASMFLAGS) $< -o $@

$(DRIVER_OBJS) $(DESKTOP_OBJS) $(WM_OBJS) $(INPUT_OBJS): %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(AI_OBJS) $(SECURITY_OBJS) $(SPEECH_OBJS): %.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

kernel/syscall_entry.o: kernel/syscall_entry.S
	$(AS) $(ASFLAGS) $< -o $@

check:
	$(MAKE) -C kernel check

iso: kernel.elf
	$(PYTHON) scripts/sync_models.py --no-iso-sync
	rm -rf $(ISO_DIR)
	mkdir -p $(ISO_DIR)/boot/grub build
	cp configs/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	cp kernel.elf $(ISO_DIR)/boot/kernel.elf
	if command -v grub-mkrescue >/dev/null 2>&1; then \
		rm -f $(ISO_OUT); \
		grub-mkrescue $(GRUB_MKRESCUE_FLAGS) -o $(ISO_OUT) $(ISO_DIR); \
	else \
		echo "Error: grub-mkrescue is required to build the ISO." >&2; \
		exit 1; \
	fi

iso-models: kernel.elf
	$(PYTHON) scripts/sync_models.py
	mkdir -p $(ISO_DIR)/boot/grub build
	cp configs/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	cp kernel.elf $(ISO_DIR)/boot/kernel.elf
	if command -v grub-mkrescue >/dev/null 2>&1; then \
		rm -f $(ISO_OUT); \
		grub-mkrescue $(GRUB_MKRESCUE_FLAGS) -o $(ISO_OUT) $(ISO_DIR); \
	else \
		echo "Error: grub-mkrescue is required to build the ISO." >&2; \
		exit 1; \
	fi

clean:
	rm -f kernel.elf $(OBJS)
	$(MAKE) -C kernel clean
	rm -rf $(ISO_DIR) build
