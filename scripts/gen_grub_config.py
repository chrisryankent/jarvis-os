#!/usr/bin/env python3
"""Generate GRUB config based on boot mode."""
import sys
import os

boot_mode = sys.argv[1] if len(sys.argv) > 1 else "default"

# Map boot modes to menu indices (0-based)
mode_map = {
    "default": 0,
    "debug": 1,
    "safe": 2,
    "recovery": 3,
}

default_idx = mode_map.get(boot_mode, 0)

grub_config = f"""set default={default_idx}
set timeout=10

menuentry "CK OS" {{
    multiboot2 /boot/kernel.elf quiet splash
    boot
}}

menuentry "CK OS (Debug)" {{
    multiboot2 /boot/kernel.elf jarvis.log_level=debug
    boot
}}

menuentry "CK OS (Safe)" {{
    multiboot2 /boot/kernel.elf jarvis.safe_mode=1
    boot
}}

menuentry "Recovery" {{
    multiboot2 /boot/kernel.elf jarvis.recovery=1 init=/bin/bash
    boot
}}
"""

os.makedirs("iso/boot/grub", exist_ok=True)
with open("iso/boot/grub/grub.cfg", "w") as f:
    f.write(grub_config)

print(f"[GRUB] Generated config for boot mode: {boot_mode}")
