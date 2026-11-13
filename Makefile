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
EXTRA_OBJS := kernel/syscall_entry.o user/programs.o

# User-mode programs (ring 3). Built as static ELF files and bundled into
# the kernel image, which installs them in /jarvis/bin at boot.
USER_PROGS  := hello counter kernelpoke privileged
USER_ELFS   := $(USER_PROGS:%=user/%.elf)
USER_CFLAGS := -std=c17 -ffreestanding -nostdlib -static -fno-pic -no-pie \
               -mcmodel=large -mgeneral-regs-only -fno-stack-protector -O2 \
               -Wall -Wextra -U_FORTIFY_SOURCE -Wl,-T,user/user.ld \
               -Wl,--build-id=none -Wl,-z,max-page-size=4096
OBJS := $(BOOT_OBJS) $(DRIVER_OBJS) $(DESKTOP_OBJS) $(WM_OBJS) \
        $(INPUT_OBJS) $(AI_OBJS) $(SECURITY_OBJS) $(SPEECH_OBJS) \
        $(EXTRA_OBJS)

QEMU      := qemu-system-x86_64
# The kernel reserves a 256 MB model pool plus ~37 MB of .bss, so QEMU's
# default 128 MB is not enough.
QEMU_MEM  := 2G
# -rtc base=localtime: the taskbar clock shows your local time, not UTC.
# AC97: gives the guest a sound card wired to the host microphone/speakers
# (the kernel has an AC97 capture + playback driver).
QEMU_AUDIO ?= -audiodev pa,id=snd0 -device AC97,audiodev=snd0
QEMU_FLAGS := -cdrom $(ISO_OUT) -m $(QEMU_MEM) -serial stdio \
              -rtc base=localtime $(QEMU_AUDIO)

# Host voice bridge: COM2 is exposed as this socket; tools/voice_bridge.py
# records the host microphone, transcribes it with whisper.cpp and sends the
# text to the guest.
VOICE_SOCKET := /tmp/jarvis-voice.sock

# Keep the ISO small: compress GRUB and ship only what the themed boot menu
# needs (the unicode font; no locales or stock themes).
GRUB_MKRESCUE_FLAGS := --compress=xz --locales= --themes= --fonts=unicode
GRUB_THEME_DIR := $(ISO_DIR)/boot/grub/themes/jarvis

.PHONY: all clean check iso iso-models run qemu run-headless run-voice voice-enroll voice-setup FORCE

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

# No window: serial log only (Ctrl-C to quit).
run-headless: iso
	$(QEMU) $(QEMU_FLAGS) -display none

# Desktop + voice: starts QEMU and the host microphone bridge together, so
# what you say is transcribed and delivered to JARVIS.
run-voice: iso
	rm -f $(VOICE_SOCKET)
	$(PYTHON) tools/run_qemu_voice.py --socket $(VOICE_SOCKET) -- \
		$(QEMU) $(QEMU_FLAGS) -serial unix:$(VOICE_SOCKET),server,nowait

# Make the voice bridge speak through the voice-cloning engine installed in
# tools/voice-venv, and load its model once (the first run downloads it).
voice-setup:
	tools/voice-venv/bin/python tools/voice_engine.py --configure
	tools/voice-venv/bin/python tools/voice_engine.py --warm

# Record your own voice for JARVIS to speak with (writes tools/voice/).
voice-enroll:
	$(PYTHON) tools/record_voice.py

kernel.elf: $(KERNEL_CORE) $(OBJS)
	$(LD) $(LDFLAGS) $(KERNEL_CORE) $(OBJS) -o $@

$(BOOT_OBJS): %.o: %.asm
	$(NASM) $(NASMFLAGS) $< -o $@

$(DRIVER_OBJS) $(DESKTOP_OBJS) $(WM_OBJS) $(INPUT_OBJS): %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(AI_OBJS) $(SECURITY_OBJS) $(SPEECH_OBJS): %.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

user/%.elf: user/%.c user/crt0.S user/jarvis.h user/user.ld
	$(CC) $(USER_CFLAGS) user/crt0.S $< -o $@

user/programs.o: user/programs.S $(USER_ELFS)
	$(CC) -c user/programs.S -o $@

kernel/syscall_entry.o: kernel/syscall_entry.S
	$(AS) $(ASFLAGS) $< -o $@

check:
	$(MAKE) -C kernel check

iso: kernel.elf
	$(PYTHON) scripts/sync_models.py --no-iso-sync
	rm -rf $(ISO_DIR)
	mkdir -p $(ISO_DIR)/boot/grub build
	cp configs/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	mkdir -p $(GRUB_THEME_DIR)
	cp configs/grub-theme/theme.txt configs/grub-theme/*.png $(GRUB_THEME_DIR)/
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
	mkdir -p $(GRUB_THEME_DIR)
	cp configs/grub-theme/theme.txt configs/grub-theme/*.png $(GRUB_THEME_DIR)/
	cp kernel.elf $(ISO_DIR)/boot/kernel.elf
	if command -v grub-mkrescue >/dev/null 2>&1; then \
		rm -f $(ISO_OUT); \
		grub-mkrescue $(GRUB_MKRESCUE_FLAGS) -o $(ISO_OUT) $(ISO_DIR); \
	else \
		echo "Error: grub-mkrescue is required to build the ISO." >&2; \
		exit 1; \
	fi

clean:
	rm -f kernel.elf $(OBJS) $(USER_ELFS)
	$(MAKE) -C kernel clean
	rm -rf $(ISO_DIR) build
