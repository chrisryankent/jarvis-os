#!/usr/bin/env python3
"""Run QEMU and the host voice bridge as one foreground session."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path


def wait_for_socket(path: str, qemu: subprocess.Popen[bytes], timeout: float) -> bool:
    deadline = time.time() + timeout
    sock = Path(path)
    while time.time() < deadline:
        if sock.exists():
            return True
        if qemu.poll() is not None:
            return False
        time.sleep(0.1)
    return sock.exists()


def terminate(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def main() -> int:
    parser = argparse.ArgumentParser(description="Start QEMU and JARVIS host voice bridge together.")
    parser.add_argument("--socket", default="/tmp/jarvis-voice.sock")
    parser.add_argument("--model", default=None, help="Whisper model (default: best installed)")
    parser.add_argument("--whisper-cli", default="tools/vendor/whisper.cpp/build/bin/whisper-cli")
    parser.add_argument("--bridge-timeout", type=float, default=20.0)
    parser.add_argument("--bridge-arg", action="append", default=[])
    parser.add_argument("qemu", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    if args.qemu and args.qemu[0] == "--":
        args.qemu = args.qemu[1:]
    if not args.qemu:
        raise SystemExit("[voice-session] Missing QEMU command after --")

    qemu = subprocess.Popen(args.qemu)
    bridge: subprocess.Popen[bytes] | None = None

    try:
        if wait_for_socket(args.socket, qemu, args.bridge_timeout):
            bridge_cmd = [
                sys.executable,
                "tools/voice_bridge.py",
                "--socket",
                args.socket,
                *(["--model", args.model] if args.model else []),
                "--whisper-cli",
                args.whisper_cli,
                *args.bridge_arg,
            ]
            print("[voice-session] Starting host microphone bridge", flush=True)
            bridge = subprocess.Popen(bridge_cmd, stdin=subprocess.DEVNULL)
        else:
            print("[voice-session] Voice socket did not appear; running QEMU without bridge", flush=True)

        return qemu.wait()
    except KeyboardInterrupt:
        return 130
    finally:
        terminate(bridge)
        terminate(qemu)


if __name__ == "__main__":
    raise SystemExit(main())
