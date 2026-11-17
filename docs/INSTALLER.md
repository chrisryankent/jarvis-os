# Kryzen OS installation and first boot

## Installer model

Windows Setup and mainstream Linux installers use a two-stage flow: boot an
installer independently of the target disk, select and review storage changes,
deploy the system and boot files, restart, then collect the initial account and
device preferences. Windows Setup makes the target disk explicit; Ubuntu and
Debian also offer guided and manual storage paths. Kryzen follows that shape,
with an offline-capable base install and an explicit review before disk writes.
[Windows Setup](https://learn.microsoft.com/windows-hardware/manufacture/desktop/windows-setup-installing-using-the-mbr-or-gpt-partition-style),
[Ubuntu Desktop](https://ubuntu.com/desktop/docs/en/24.04/tutorial/install-ubuntu-desktop/),
and the [Debian Installation Guide](https://www.debian.org/releases/stable/amd64/index.en.html)
are the reference flows.

## Current native flow

The ISO offers **Install Kryzen OS** and **Try Kryzen**. Install mode checks the
kernel and UEFI loader payloads, lists disks, and provides two GPT options:

- **Guided erase** creates a new GPT with a FAT32 EFI System Partition and a
  KryzenFS data partition. Existing partitions on that disk are erased. KryzenFS
  currently supports up to 64 GiB, so extra disk capacity stays unallocated.
- **Manual free space** works on supported GPT layouts. It displays existing
  partitions, selects a contiguous unallocated region, and lets the user size
  the Kryzen data partition. The user can either add a new EFI/data pair, or
  reuse a supported FAT EFI System Partition and add only Kryzen data. Reuse
  stores Kryzen files under `EFI/Kryzen`, checks free space and refuses to
  overwrite an existing Kryzen boot file. Existing EFI files and GPT
  partitions stay in place. It does not resize or delete existing partitions.

The review names the target and proposed changes. Installation writes the
standalone UEFI GRUB loader, kernel, boot marker, and GRUB menu to the new or
reused EFI partition, then reads the files back for byte-for-byte verification.
It formats the new data partition, creates the first account, and saves the
device name, theme, supported display resolution, and 12/24-hour clock format
before offering restart. Completion is gated on a
registry flush, KryzenFS flush, and read-back verification of filesystem
metadata plus the persistent account, registry, and file contents. A failed
save leaves a retry action on the setup form.

The installed bootloader searches for the Kryzen-specific marker file so it can
find the right EFI partition even when the disk already contains another EFI
partition. A reused ESP receives a separate `EFI/Kryzen/BOOTX64.EFI`; Kryzen
does not replace the existing firmware fallback loader. The running kernel
discovers KryzenFS by scanning GPT partitions at normal boot. The installer
does not create a firmware NVRAM boot entry, so on a reused ESP the user may
need to select `EFI/Kryzen/BOOTX64.EFI` from firmware or add a boot entry later.

## Storage support and constraints

The kernel boots through GRUB Multiboot2. KryzenFS is the persistent VFS used by
accounts, settings, and user files. FAT32 stores boot files. The kernel does not
mount ext4, so the host-side `scripts/install.sh` ext4 layout is not used by
this native installer.

The supported first release is UEFI/GPT. Legacy BIOS install, Secure Boot,
network configuration, language and keyboard selection, time-zone selection,
arbitrary manual partition creation/resizing, and unattended recovery are not
implemented yet. The first-run form currently configures the owner account,
password/PIN and optional voice sign-in, device name, theme, display resolution,
and clock format. The UEFI loader
is installed at the standard `EFI/BOOT/BOOTX64.EFI` path; firmware that does not
discover that path automatically may require choosing the Kryzen EFI file from
its boot menu.

The compact offline ISO contains the monolithic kernel and built-in desktop,
apps, and user programs. It does not include the several-gigabyte optional AI
model collection under `tools/models`; the generated model manifest creates
placeholder paths, so local-model features need those model files provisioned
separately after the base installation.

## Installer implementation map

- `configs/grub.cfg` provides Install and Try boot entries and passes the kernel
  and EFI loader as named Multiboot2 modules.
- `boot/multiboot2.asm` captures installer mode and payload ranges.
- `desktop/installer.c` draws the wizard and obtains disk/layout confirmation.
- `kernel/installer.c` deploys and verifies boot files, then hands off to the
  account setup form.
- `kernel/diskmgmt.c` validates GPT metadata, creates the guided layout, or
  adds partitions to a checked unallocated GPT region while preserving an
  existing FAT EFI System Partition when selected.
- `kernel/diskfs.c` formats and mounts KryzenFS within a bounded partition.
- `desktop/login.c` creates the first account and persists the initial device
  preferences.
