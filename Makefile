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
# Boot loaders of an installed system and the bundle the installer unpacks.
INSTALLER_EFI := build/boot/BOOTX64.EFI
BIOS_CORE     := build/boot/core.img
BIOS_BOOT     := /usr/lib/grub/i386-pc/boot.img
BOOT_BUNDLE   := build/kryzen-boot.tar
THEME_SRC     := configs/grub-theme
# Everything the installed boot menu uses is built into the loaders: nothing
# is loaded from a module folder at boot.
GRUB_BOOT_MODS := part_gpt part_msdos fat normal configfile search search_fs_file \
                  multiboot2 video video_fb gfxterm gfxmenu png jpeg font echo test \
                  sleep reboot halt minicmd ls cat

KERNEL_CORE := kernel/kernel_core.o
BOOT_OBJS := boot/multiboot2.o
DRIVER_OBJS := drivers/ahci.o drivers/audio.o drivers/gpu.o drivers/font_aa.o drivers/input.o \
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
EXTRA_OBJS := kernel/syscall_entry.o user/programs.o desktop/assets.o

# User-mode programs (ring 3). Built as static ELF files and bundled into
# the kernel image, which installs them in /jarvis/bin at boot.
USER_PROGS  := hello counter kernelpoke privileged ask launcher
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
# The machine's hard disk: a SATA drive on an AHCI controller, backed by
# this image file. Your files in JARVIS OS live here, so it is kept outside
# build/ and is NOT removed by `make clean`. Delete it to start with an empty
# disk. DISK_MB only applies when the image is first created.
DISK_IMG  := storage/disk.img
DISK_MB   ?= 512
QEMU_DISK := -device ahci,id=ahci \
             -drive id=hd0,file=$(DISK_IMG),if=none,format=raw \
             -device ide-hd,drive=hd0,bus=ahci.0
# Window: the guest is scaled to fit the window (resize or maximise it
# freely; Ctrl+Alt+F for full screen, Ctrl+Alt+G releases the mouse). The
# menu bar must stay visible: QEMU's GTK window loses every Ctrl+Alt hot key,
# including Ctrl+Alt+G, when it is hidden with show-menubar=off.
QEMU_DISPLAY ?= -display gtk,zoom-to-fit=on
# Network: QEMU's built-in router shares the host's connection (Wi-Fi or
# cable) with the guest's Ethernet card and runs a DHCP server for it.
QEMU_NET  := -netdev user,id=net0 -device e1000,netdev=net0
# USB 3 controller. To give JARVIS OS a USB flash drive image:
#   make run QEMU_USB_EXTRA="-drive id=stick,file=stick.img,if=none,format=raw -device usb-storage,drive=stick"
QEMU_USB  := -device qemu-xhci,id=xhci $(QEMU_USB_EXTRA)
# Control socket used by tools/usb_stick.py to plug USB drives in and out.
QEMU_MONITOR := -monitor unix:/tmp/jarvis-qemu-monitor-$(shell id -u).sock,server,nowait
# Boot order: the ISO first; after a restart from inside the guest (for
# example at the end of an installation) the hard disk, then the ISO again
# if the disk has no boot loader.
QEMU_BOOT ?= -boot order=cd,once=d
QEMU_COMMON := -m $(QEMU_MEM) -serial stdio \
              -rtc base=localtime $(QEMU_AUDIO) $(QEMU_DISK) $(QEMU_NET) $(QEMU_USB) \
              $(QEMU_MONITOR)
QEMU_FLAGS := -cdrom $(ISO_OUT) $(QEMU_BOOT) $(QEMU_COMMON)
# UEFI firmware for the run-uefi targets (Debian/Kali package: ovmf).
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS_SRC ?= /usr/share/OVMF/OVMF_VARS_4M.fd
OVMF_VARS := storage/OVMF_VARS.fd
QEMU_UEFI := -machine q35 -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
             -drive if=pflash,format=raw,file=$(OVMF_VARS)

# Host voice bridge: COM2 is exposed as this socket; tools/voice_bridge.py
# records the host microphone, transcribes it with whisper.cpp and sends the
# text to the guest.
VOICE_SOCKET := /tmp/jarvis-voice.sock

# Keep the ISO small: compress GRUB and ship only what the themed boot menu
# needs (the unicode font; no locales or stock themes).
GRUB_MKRESCUE_FLAGS := --compress=xz --locales= --themes= --fonts=unicode
GRUB_THEME_DIR := $(ISO_DIR)/boot/grub/themes/kryzen

.PHONY: all clean check iso iso-models run qemu run-disk run-uefi run-disk-uefi run-headless run-voice art voice-install voice-enroll voice-setup voice-render usb-list usb-attach usb-image usb-detach FORCE

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
desktop/ui_shell.o: $(wildcard desktop/*.c desktop/*.h)

# Shared headers: a change to any of them rebuilds everything that uses them.
$(DRIVER_OBJS) $(DESKTOP_OBJS) $(WM_OBJS): kernel/kernel.h drivers/gpu.h \
                                           wm/wm.h desktop/ui.h

$(DISK_IMG):
	mkdir -p $(dir $(DISK_IMG))
	truncate -s $(DISK_MB)M $@

# Boot loaders the native installer puts on the target disk. Both search for
# EFI/Kryzen/kryzen.marker and read the boot menu next to it.
$(INSTALLER_EFI): configs/grub-installed.cfg
	mkdir -p $(dir $@)
	grub-mkimage -O x86_64-efi -o $@ -p /EFI/Kryzen/grub -c configs/grub-installed.cfg \
		$(GRUB_BOOT_MODS) efi_gop chain efifwsetup

$(BIOS_CORE): configs/grub-installed.cfg
	mkdir -p $(dir $@)
	grub-mkimage -O i386-pc -o $@ -p /EFI/Kryzen/grub -c configs/grub-installed.cfg \
		$(GRUB_BOOT_MODS) biosdisk vbe vga

$(BOOT_BUNDLE): $(INSTALLER_EFI) $(BIOS_CORE) configs/grub-installed-menu.cfg \
                scripts/make_boot_bundle.py $(wildcard $(THEME_SRC)/* $(THEME_SRC)/icons/*)
	$(PYTHON) scripts/make_boot_bundle.py $@ $(INSTALLER_EFI) $(BIOS_BOOT) $(BIOS_CORE) \
		configs/grub-installed-menu.cfg $(THEME_SRC)

$(OVMF_VARS):
	mkdir -p $(dir $@)
	cp $(OVMF_VARS_SRC) $@

# Regenerate the artwork (wallpapers, boot-menu theme, UI fonts). The results
# are checked in; this needs python3-numpy, python3-scipy and python3-pil.
art:
	$(PYTHON) scripts/gen_wallpapers.py
	$(PYTHON) scripts/gen_grub_theme.py
	$(PYTHON) tools/gen_fonts.py

# Build the ISO and boot it in a QEMU window; kernel log goes to this terminal.
run: iso $(DISK_IMG)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISPLAY)

qemu: run

# Start the system installed on the hard disk image (no ISO attached).
run-disk: $(DISK_IMG)
	$(QEMU) -boot c $(QEMU_COMMON) $(QEMU_DISPLAY)

# The same two, on UEFI firmware instead of the legacy BIOS.
run-uefi: iso $(DISK_IMG) $(OVMF_VARS)
	$(QEMU) $(QEMU_UEFI) -cdrom $(ISO_OUT) $(QEMU_COMMON) $(QEMU_DISPLAY)

run-disk-uefi: $(DISK_IMG) $(OVMF_VARS)
	$(QEMU) $(QEMU_UEFI) $(QEMU_COMMON) $(QEMU_DISPLAY)

# No window: serial log only (Ctrl-C to quit).
run-headless: iso $(DISK_IMG)
	$(QEMU) $(QEMU_FLAGS) -display none

# Desktop + voice: starts QEMU and the host microphone bridge together, so
# what you say is transcribed and delivered to JARVIS.
run-voice: iso $(DISK_IMG)
	rm -f $(VOICE_SOCKET)
	$(PYTHON) tools/run_qemu_voice.py --socket $(VOICE_SOCKET) -- \
		$(QEMU) $(QEMU_FLAGS) $(QEMU_DISPLAY) -serial unix:$(VOICE_SOCKET),server,nowait

# Install the host-side speech software (about 4 GB, CPU-only PyTorch):
#   tools/voice-venv   Chatterbox, which clones your voice (Python 3.14)
#   tools/kokoro-venv  Kokoro, the live voice (needs Python 3.12, fetched by uv)
voice-install:
	test -x tools/voice-venv/bin/python || $(PYTHON) -m venv tools/voice-venv
	tools/voice-venv/bin/pip install torch torchaudio --index-url https://download.pytorch.org/whl/cpu
	tools/voice-venv/bin/pip install chatterbox-tts uv
	tools/voice-venv/bin/uv python install 3.12
	test -x tools/kokoro-venv/bin/python || tools/voice-venv/bin/uv venv --python 3.12 tools/kokoro-venv
	VIRTUAL_ENV=tools/kokoro-venv tools/voice-venv/bin/uv pip install torch --index-url https://download.pytorch.org/whl/cpu
	VIRTUAL_ENV=tools/kokoro-venv tools/voice-venv/bin/uv pip install kokoro==0.9.4 soundfile pip

# Live voice: Kokoro (natural speech, a second or two per sentence on a CPU).
voice-setup:
	tools/kokoro-venv/bin/python tools/kokoro_server.py --configure
	tools/kokoro-venv/bin/python tools/kokoro_server.py --warm
	tools/kokoro-venv/bin/python tools/kokoro_server.py --render tools/voice_phrases.txt

# Record JARVIS's fixed sentences in YOUR cloned voice (Chatterbox). Very slow
# on a CPU (a few minutes per sentence, so run it once, e.g. overnight);
# finished sentences are kept, so it can be stopped and resumed.
voice-render:
	tools/voice-venv/bin/python tools/voice_engine.py --render tools/voice_phrases.txt

# USB drives for the running VM (see tools/usb_stick.py):
#   make usb-list                 drives connected to this computer
#   make usb-attach DEV=vid:pid   plug one into JARVIS OS
#   make usb-image IMG=file.img   plug in a disk image as a USB drive
#   make usb-detach               unplug it again
usb-list:
	@$(PYTHON) tools/usb_stick.py list
usb-attach:
	@$(PYTHON) tools/usb_stick.py attach $(DEV)
usb-image:
	@$(PYTHON) tools/usb_stick.py image $(IMG)
usb-detach:
	@$(PYTHON) tools/usb_stick.py detach

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

# Sample packages (placed in Downloads at boot so there is something to install)
user/hello.jpk: user/hello.elf tools/mkjpk.py
	$(PYTHON) tools/mkjpk.py --name hello --title "Hello" --version 1.0 --publisher "JARVIS OS" \
		--description "Greets you, writes a file and shows the system status." \
		--exec bin/hello -o $@ user/hello.elf=bin/hello
user/counter.jpk: user/counter.elf tools/mkjpk.py
	$(PYTHON) tools/mkjpk.py --name counter --title "Counter" --version 1.0 --publisher "JARVIS OS" \
		--description "Counts to twenty, once a second." \
		--exec bin/counter -o $@ user/counter.elf=bin/counter

user/programs.o: user/programs.S $(USER_ELFS) user/hello.jpk user/counter.jpk
	$(CC) -c user/programs.S -o $@

desktop/assets.o: desktop/assets.S $(wildcard desktop/assets/*.jpg)
	$(CC) -c desktop/assets.S -o $@

kernel/syscall_entry.o: kernel/syscall_entry.S
	$(AS) $(ASFLAGS) $< -o $@

check:
	$(MAKE) -C kernel check

iso: kernel.elf $(BOOT_BUNDLE)
	$(PYTHON) scripts/sync_models.py --no-iso-sync
	rm -rf $(ISO_DIR)
	mkdir -p $(ISO_DIR)/boot/grub build
	cp configs/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	cp $(BOOT_BUNDLE) $(ISO_DIR)/boot/kryzen-boot.tar
	rm -rf $(GRUB_THEME_DIR)
	mkdir -p $(GRUB_THEME_DIR)
	cp -r $(THEME_SRC)/. $(GRUB_THEME_DIR)/
	cp kernel.elf $(ISO_DIR)/boot/kernel.elf
	if command -v grub-mkrescue >/dev/null 2>&1; then \
		rm -f $(ISO_OUT); \
		grub-mkrescue $(GRUB_MKRESCUE_FLAGS) -o $(ISO_OUT) $(ISO_DIR); \
	else \
		echo "Error: grub-mkrescue is required to build the ISO." >&2; \
		exit 1; \
	fi

iso-models: kernel.elf $(BOOT_BUNDLE)
	$(PYTHON) scripts/sync_models.py
	mkdir -p $(ISO_DIR)/boot/grub build
	cp configs/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	cp $(BOOT_BUNDLE) $(ISO_DIR)/boot/kryzen-boot.tar
	rm -rf $(GRUB_THEME_DIR)
	mkdir -p $(GRUB_THEME_DIR)
	cp -r $(THEME_SRC)/. $(GRUB_THEME_DIR)/
	cp kernel.elf $(ISO_DIR)/boot/kernel.elf
	if command -v grub-mkrescue >/dev/null 2>&1; then \
		rm -f $(ISO_OUT); \
		grub-mkrescue $(GRUB_MKRESCUE_FLAGS) -o $(ISO_OUT) $(ISO_DIR); \
	else \
		echo "Error: grub-mkrescue is required to build the ISO." >&2; \
		exit 1; \
	fi

clean:
	rm -f kernel.elf $(OBJS) $(USER_ELFS) user/*.jpk
	$(MAKE) -C kernel clean
	rm -rf $(ISO_DIR) build
