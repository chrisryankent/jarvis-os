#!/usr/bin/env python3
"""
Build a JARVIS package (.jpk), installable from Files, the Apps window or
the Terminal (`install <file>`).

  mkjpk.py --name hello --title "Hello" --version 1.0 --exec bin/hello \\
           --publisher "You" --description "Says hello" \\
           -o hello-1.0.jpk  user/hello.elf=bin/hello  [README.txt=README.txt ...]

Each file argument is SOURCE=PATH_IN_PACKAGE (or just SOURCE, kept as its
file name). The program named by --exec must be one of the files.
Format: see kernel/pkg.c.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser(description="Build a JARVIS .jpk package")
    p.add_argument("--name", required=True, help="id: lower-case letters, digits and '-'")
    p.add_argument("--title", default="")
    p.add_argument("--version", default="1.0")
    p.add_argument("--publisher", default="")
    p.add_argument("--description", default="")
    p.add_argument("--exec", required=True, dest="exe", help="program to run, path inside the package")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("files", nargs="+")
    a = p.parse_args()

    if not re.fullmatch(r"[a-z0-9-]{1,31}", a.name):
        sys.exit("--name must be 1-31 characters of a-z, 0-9 and '-'")
    entries = []
    for spec in a.files:
        src, _, dest = spec.partition("=")
        dest = dest or Path(src).name
        if dest.startswith("/") or ".." in dest.split("/") or len(dest) > 120:
            sys.exit(f"unsafe path inside package: {dest}")
        entries.append((dest, Path(src).read_bytes()))
    if a.exe not in [d for d, _ in entries]:
        sys.exit(f"--exec {a.exe} is not one of the files")

    fields = {"name": a.name, "title": a.title or a.name, "version": a.version,
              "publisher": a.publisher, "description": a.description, "exec": a.exe}
    manifest = "".join(f"{k}={v.replace(chr(10), ' ')}\n" for k, v in fields.items() if v).encode()
    body = manifest
    for dest, data in entries:
        d = dest.encode()
        body += struct.pack("<H", len(d)) + d + struct.pack("<I", len(data)) + data
    out = b"JPK1" + struct.pack("<II", len(manifest), zlib.crc32(body) & 0xFFFFFFFF) + body
    Path(a.output).write_bytes(out)
    print(f"{a.output}: {len(entries)} file(s), {len(out)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
