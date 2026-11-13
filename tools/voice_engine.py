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
  voice_engine.py --render FILE               record each line of FILE in your voice (cache)

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


def save_wav(path: str, wav, rate: int) -> None:
    """Write a 16-bit mono WAV (torchaudio.save needs the separate torchcodec)."""
    import wave as wavmod
    samples = wav.detach().cpu().reshape(-1).clamp(-1, 1).mul(32767).short().numpy()
    with wavmod.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(samples.astype("<i2").tobytes())


def render(phrases_file: str, ref: str) -> int:
    """Record every phrase in the owner's cloned voice into the cache the
    voice bridge plays from. Slow on a CPU (minutes per sentence); phrases
    already rendered are skipped, so it can be stopped and resumed."""
    import hashlib
    import re
    import torch
    from chatterbox.tts import ChatterboxTTS

    def key(text: str) -> str:                       # same as voice_bridge.phrase_key
        t = re.sub(r"[^a-z0-9]+", " ", text.lower().replace("'", ""))
        return hashlib.sha1(" ".join(t.split()).encode()).hexdigest()[:16]

    if not ref or not os.path.exists(ref):
        print("No voice recording found: run `make voice-enroll` first.", file=sys.stderr)
        return 1
    cache = LOG.parent / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    phrases = [l.strip() for l in open(phrases_file, encoding="utf-8")
               if l.strip() and not l.lstrip().startswith("#")]
    todo = [p for p in phrases if not (cache / f"{key(p)}.wav").exists()]
    print(f"{len(phrases) - len(todo)} of {len(phrases)} phrases already recorded.", flush=True)
    if not todo:
        return 0

    torch.set_num_threads(max(1, (os.cpu_count() or 4) - 1))
    print("Loading the voice model...", flush=True)
    model = ChatterboxTTS.from_pretrained(device="cpu")
    for n, text in enumerate(todo, 1):
        started = time.time()
        with torch.inference_mode():
            wav = model.generate(text, audio_prompt_path=ref)
        out = cache / f"{key(text)}.wav"
        save_wav(str(out) + ".tmp", wav, model.sr)
        os.replace(str(out) + ".tmp", out)          # never leave a half-written clip
        print(f"[{n}/{len(todo)}] {time.time() - started:.0f}s  {text}", flush=True)
    return 0


def serve() -> int:
    lock = server_lock()
    if lock is None:
        print("[engine] another engine is already running; exiting", flush=True)
        return 0

    # Plain resumable HTTPS downloads: progress is visible as a growing file
    # and an interrupted download continues where it stopped.
    os.environ.setdefault("HF_HUB_DISABLE_XET", "1")
    os.environ.setdefault("HF_HUB_DOWNLOAD_TIMEOUT", "60")

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
                save_wav(req["out"], wav, model.sr)
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


def main() -> int:
    p = argparse.ArgumentParser(description="JARVIS voice engine")
    p.add_argument("--serve", action="store_true")
    p.add_argument("--warm", action="store_true")
    p.add_argument("--stop", action="store_true")
    p.add_argument("--render", metavar="PHRASES_FILE",
                   help="record each line of this file in your voice, into the phrase cache")
    p.add_argument("--text")
    p.add_argument("--ref", default="")
    p.add_argument("--out")
    p.add_argument("--startup-wait", type=float, default=1800.0,
                   help="seconds to wait for the model to load (first run downloads it)")
    args = p.parse_args()

    if args.serve:
        return serve()
    if args.render:
        return render(args.render, args.ref or str(LOG.parent / "reference.wav"))
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
