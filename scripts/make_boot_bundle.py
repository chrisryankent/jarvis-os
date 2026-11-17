#!/usr/bin/env python3
"""
Packs everything the native installer copies to a new disk, apart from the
kernel itself, into one ustar archive (build/kryzen-boot.tar). GRUB hands it
to the live kernel as the Multiboot2 module "kryzen-boot"; kernel/installer.c
unpacks it.

  efi/BOOTX64.EFI                  UEFI boot loader
  bios/boot.img, bios/core.img     legacy-BIOS boot sector code and stage 2
  esp/EFI/Kryzen/kryzen.marker     the file both loaders search for
  esp/EFI/Kryzen/grub/grub.cfg     boot menu of the installed system
  esp/EFI/Kryzen/grub/themes/...   its theme, icons and fonts

usage: make_boot_bundle.py OUT.tar EFI_LOADER BIOS_BOOT_IMG BIOS_CORE_IMG MENU_CFG THEME_DIR
"""
import io
import os
import struct
import sys
import tarfile


def add(tar, name, data):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = 0o644
    info.mtime = 0
    tar.addfile(info, io.BytesIO(data))


def main():
    out, efi, boot_img, core_img, menu, theme = sys.argv[1:7]
    boot = open(boot_img, "rb").read()
    core = open(core_img, "rb").read()
    efi_data = open(efi, "rb").read()

    # kernel/installer.c patches these fields; stop the build if a new GRUB
    # release moves them.
    if len(boot) != 512 or boot[510:512] != b"\x55\xaa":
        sys.exit("boot.img is not a 512-byte boot sector")
    if struct.unpack_from("<Q", boot, 0x5C)[0] != 1 or boot[0x66] != 0xEB:
        sys.exit("boot.img: unexpected layout (core.img sector / drive check)")
    start, length, seg = struct.unpack_from("<QHH", core, 0x1F4)
    if start != 2 or seg != 0x820:
        sys.exit("core.img: unexpected block-list layout")
    if len(core) > 1024 * 1024 - 512:
        sys.exit("core.img does not fit in the 1 MiB BIOS boot partition")
    if len(core) > 0x90000:
        sys.exit("core.img is too large to be loaded in real mode")
    if efi_data[:2] != b"MZ":
        sys.exit("the UEFI loader is not a PE image")

    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tar:
        add(tar, "efi/BOOTX64.EFI", efi_data)
        add(tar, "bios/boot.img", boot)
        add(tar, "bios/core.img", core)
        add(tar, "esp/EFI/Kryzen/kryzen.marker", b"KRYZEN-OS-BOOT\n")
        add(tar, "esp/EFI/Kryzen/grub/grub.cfg", open(menu, "rb").read())
        for root, _, files in sorted(os.walk(theme)):
            for f in sorted(files):
                full = os.path.join(root, f)
                rel = os.path.relpath(full, theme).replace(os.sep, "/")
                add(tar, "esp/EFI/Kryzen/grub/themes/kryzen/" + rel, open(full, "rb").read())
    print("wrote %s (%d KB)" % (out, os.path.getsize(out) // 1024))


if __name__ == "__main__":
    main()
