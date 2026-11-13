#!/usr/bin/env python3
"""
JARVIS voice engine: speaks text in a cloned voice (Chatterbox TTS, MIT licence).

The model takes a long time to load, so it runs as a small background server
that stays loaded; each request is one JSON line over a Unix socket.

  voice_engine.py --serve                     run the server (normally automatic)
  voice_engine.py --warm                      start the server and wait until ready
  voice_engine.py --text T --out F [--ref R]  synthesise T into WAV file F,
                                              in the voice of reference R
  voice_engine.py --stop                      shut the server down
  voice_engine.py --configure                 make the voice bridge use this engine

Without --ref (or if the file does not exist yet) the engine's built-in voice
is used. Run with the interpreter in tools/voice-venv.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

SOCKET = f"/tmp/jarvis-tts-{os.getuid()}.sock"
LOG = Path(__file__).resolve().parent / "voice" / "engine.log"
LOCK = f"/tmp/jarvis-tts-{os.getuid()}.lock"


def server_lock(blocking_probe: bool = False):
    """Only one engine may exist: the model needs over 3 GB of RAM, and the
    socket does not appear until it has loaded, so "no socket yet" must not
    be mistaken for "no server". Returns the held lock file, or None if
    another engine already holds it."""
    f = open(LOCK, "a+")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        return None
    if blocking_probe:              # only asking: release straight away
        fcntl.flock(f, fcntl.LOCK_UN)
        f.close()
        return True
    return f


def serve() -> int:
    lock = server_lock()
    if lock is None:
        print("[engine] another engine is already running; exiting", flush=True)
        return 0

    import torch
    import torchaudio
    from chatterbox.tts import ChatterboxTTS

    torch.set_num_threads(max(1, (os.cpu_count() or 4) - 1))
    print("[engine] loading model (first run downloads it)...", flush=True)
    model = ChatterboxTTS.from_pretrained(device="cpu")
    print("[engine] ready", flush=True)

    if os.path.exists(SOCKET):
        os.remove(SOCKET)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET)
    os.chmod(SOCKET, 0o600)                     # only this user may connect
    server.listen(4)

    while True:
        conn, _ = server.accept()
        with conn:
            try:
                req = json.loads(conn.makefile("r").readline() or "{}")
                if req.get("cmd") == "stop":
                    conn.sendall(b"ok\n")
                    break
                if req.get("cmd") == "ping":
                    conn.sendall(b"ok\n")
                    continue
                ref = req.get("ref") or ""
                kwargs = {"audio_prompt_path": ref} if ref and os.path.exists(ref) else {}
                started = time.time()
                with torch.inference_mode():
                    wav = model.generate(req["text"], **kwargs)
                torchaudio.save(req["out"], wav, model.sr)
                print(f"[engine] {len(req['text'])} chars in {time.time() - started:.1f}s", flush=True)
                conn.sendall(b"ok\n")
            except Exception as exc:            # report, keep serving
                print(f"[engine] error: {exc}", flush=True)
                try:
                    conn.sendall(f"error {exc}\n".encode())
                except OSError:
                    pass
    server.close()
    os.remove(SOCKET)
    return 0


def request(payload: dict, timeout: float) -> str:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        s.connect(SOCKET)
        s.sendall((json.dumps(payload) + "\n").encode())
        return s.makefile("r").readline().strip()


def ensure_server(wait: float) -> bool:
    try:
        return request({"cmd": "ping"}, 5) == "ok"
    except OSError:
        pass
    LOG.parent.mkdir(parents=True, exist_ok=True)
    if server_lock(blocking_probe=True) is None:
        pass                        # one is already starting up: just wait for it
    else:
      with open(LOG, "ab") as log:
        subprocess.Popen([sys.executable, __file__, "--serve"], stdout=log, stderr=log,
                           stdin=subprocess.DEVNULL, start_new_session=True)
    deadline = time.time() + wait
    while time.time() < deadline:
        time.sleep(1)
        try:
            if request({"cmd": "ping"}, 5) == "ok":
                return True
        except OSError:
            continue
    return False


def configure() -> int:
    """Point tools/voice/profile.json at this engine (keeps any recorded voice)."""
    profile_path = LOG.parent / "profile.json"
    profile_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        profile = {}
    me = [sys.executable, str(Path(__file__).resolve())]
    profile["tts_cmd"] = me + ["--text", "{text}", "--ref", "{ref}", "--out", "{out}"]
    profile["warm_cmd"] = me + ["--warm"]
    profile.setdefault("reference_wav", "")
    profile.setdefault("reference_text", "")
    profile_path.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
    print(f"voice engine configured in {profile_path}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description="JARVIS voice engine")
    p.add_argument("--serve", action="store_true")
    p.add_argument("--warm", action="store_true")
    p.add_argument("--stop", action="store_true")
    p.add_argument("--configure", action="store_true")
    p.add_argument("--text")
    p.add_argument("--ref", default="")
    p.add_argument("--out")
    p.add_argument("--startup-wait", type=float, default=1800.0,
                   help="seconds to wait for the model to load (first run downloads it)")
    args = p.parse_args()

    if args.serve:
        return serve()
    if args.configure:
        return configure()
    if args.stop:
        try:
            request({"cmd": "stop"}, 5)
        except OSError:
            pass
        return 0
    if not ensure_server(args.startup_wait):
        print(f"voice engine did not start; see {LOG}", file=sys.stderr)
        return 1
    if args.warm:
        return 0
    if not args.text or not args.out:
        p.error("--text and --out are required")
    reply = request({"text": args.text, "ref": args.ref, "out": args.out}, 600)
    if reply != "ok":
        print(reply, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
