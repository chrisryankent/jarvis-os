#!/usr/bin/env python3
"""
Plug a USB drive into the running JARVIS OS virtual machine (started with
`make run` or `make run-voice`), the way you would plug it into a real PC.

  usb_stick.py list                 USB drives connected to this computer
  usb_stick.py attach VID:PID       hand that drive to JARVIS OS
  usb_stick.py image FILE.img       plug in a disk image as a USB drive
  usb_stick.py detach               unplug whatever this script plugged in

While a real drive is attached, this computer cannot use it. JARVIS OS only
reads USB drives (it never writes to them).
"""
from __future__ import annotations

import os
import socket
import sys
import time
from pathlib import Path

MONITOR = f"/tmp/jarvis-qemu-monitor-{os.getuid()}.sock"


def monitor(cmd: str) -> str:
    if not os.path.exists(MONITOR):
        sys.exit("JARVIS OS is not running (start it with `make run`).")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(MONITOR)
        s.settimeout(2)
        time.sleep(0.2)
        try:
            s.recv(65536)                       # banner
        except OSError:
            pass
        s.sendall((cmd + "\n").encode())
        time.sleep(0.5)
        try:
            out = s.recv(65536).decode(errors="ignore")
        except OSError:
            out = ""
    lines = [l for l in out.replace("\r", "").split("\n") if l and not l.startswith("(qemu)") and l != cmd]
    return "\n".join(lines).strip()


def read(path: Path) -> str:
    try:
        return path.read_text().strip()
    except OSError:
        return ""


def usb_drives() -> list[dict]:
    """USB devices with a mass-storage interface, from sysfs."""
    found = []
    for dev in sorted(Path("/sys/bus/usb/devices").glob("[0-9]*-*")):
        if ":" in dev.name or not (dev / "idVendor").exists():
            continue
        storage = any(read(i / "bInterfaceClass") == "08" for i in dev.glob(f"{dev.name}:*"))
        if not storage:
            continue
        blocks = sorted({b.name for b in dev.glob("**/block/*")})
        mounts = []
        mtab = read(Path("/proc/mounts"))
        for b in blocks:
            for line in mtab.splitlines():
                src, target = line.split()[:2]
                if src.startswith(f"/dev/{b}"):
                    mounts.append(f"{src} on {target}")
        bus, num = int(read(dev / "busnum") or 0), int(read(dev / "devnum") or 0)
        node = f"/dev/bus/usb/{bus:03d}/{num:03d}"
        found.append({
            "id": f"{read(dev / 'idVendor')}:{read(dev / 'idProduct')}",
            "name": f"{read(dev / 'manufacturer')} {read(dev / 'product')}".strip() or "USB drive",
            "blocks": blocks, "mounts": mounts, "node": node,
            "access": os.access(node, os.R_OK | os.W_OK),
        })
    return found


def cmd_list() -> int:
    drives = usb_drives()
    if not drives:
        print("No USB drives are connected to this computer.")
        return 0
    for d in drives:
        print(f"{d['id']}  {d['name']}  ({', '.join('/dev/' + b for b in d['blocks']) or 'no disk yet'})")
        for m in d["mounts"]:
            print(f"           mounted here: {m}")
        if not d["access"]:
            print(f"           no permission to hand it to the VM ({d['node']})")
    print("\nAttach one with:  make usb-attach DEV=<id>")
    return 0


def cmd_attach(dev_id: str) -> int:
    drive = next((d for d in usb_drives() if d["id"] == dev_id.lower()), None)
    if not drive:
        print(f"No USB drive {dev_id} found. See: make usb-list")
        return 1
    if drive["mounts"]:
        print("This computer is using the drive right now. Unmount it first, e.g.:")
        for m in drive["mounts"]:
            print(f"  udisksctl unmount -b {m.split()[0]}")
        return 1
    if not drive["access"]:
        vid, pid = drive["id"].split(":")
        print(f"You do not have permission to pass {drive['node']} to the virtual machine.")
        print("Allow it for this drive (one-off, needs your password):")
        print(f"  echo 'SUBSYSTEM==\"usb\", ATTR{{idVendor}}==\"{vid}\", ATTR{{idProduct}}==\"{pid}\", "
              f"MODE=\"0660\", GROUP=\"plugdev\"' | sudo tee /etc/udev/rules.d/70-jarvis-usb.rules")
        print("  sudo udevadm control --reload && sudo udevadm trigger")
        print("  sudo usermod -aG plugdev $USER     # then log out and back in")
        print("Then unplug and replug the drive and run this again.")
        return 1
    vid, pid = drive["id"].split(":")
    out = monitor(f"device_add usb-host,bus=xhci.0,vendorid=0x{vid},productid=0x{pid},id=jarvis-stick")
    print(out or f"Attached {drive['name']}. It appears in JARVIS OS within a second (This PC).")
    return 1 if out else 0


def cmd_image(path: str) -> int:
    img = Path(path).resolve()
    if not img.is_file():
        print(f"No such file: {img}")
        return 1
    out = monitor(f"drive_add 0 if=none,id=jarvis-img,file={img},format=raw,readonly=on")
    if out and "OK" not in out:
        print(out)
        return 1
    out = monitor("device_add usb-storage,bus=xhci.0,drive=jarvis-img,id=jarvis-stick")
    print(out or f"Plugged in {img.name} as a USB drive.")
    return 1 if out else 0


def cmd_detach() -> int:
    out = monitor("device_del jarvis-stick")
    print(out or "Unplugged.")
    return 0


def main() -> int:
    args = sys.argv[1:]
    if not args or args[0] == "list":
        return cmd_list()
    if args[0] == "attach" and len(args) == 2:
        return cmd_attach(args[1])
    if args[0] == "image" and len(args) == 2:
        return cmd_image(args[1])
    if args[0] == "detach":
        return cmd_detach()
    print(__doc__)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
