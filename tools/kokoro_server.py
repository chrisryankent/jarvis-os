#!/usr/bin/env python3
"""
JARVIS live voice: Kokoro text-to-speech (Apache-2.0), kept loaded in a small
background server so each sentence is synthesised in a second or two on a
CPU. Uses the model already in tools/models/tts.

  kokoro_server.py --serve                 run the server (normally automatic)
  kokoro_server.py --warm                  start it and wait until it is loaded
  kokoro_server.py --text T --out F        speak T into WAV file F
  kokoro_server.py --stop
  kokoro_server.py --configure [--voice bm_george]
                                           make the voice bridge use this engine

Runs with the interpreter in tools/kokoro-venv: Kokoro needs Python 3.12 or
older, which uv installs there (see `make voice-setup`).
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
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent
MODEL_DIR = ROOT / "models" / "tts"
PROFILE = ROOT / "voice" / "profile.json"
LOG = ROOT / "voice" / "kokoro.log"
SOCKET = f"/tmp/jarvis-kokoro-{os.getuid()}.sock"
LOCK = f"/tmp/jarvis-kokoro-{os.getuid()}.lock"
DEFAULT_VOICE = "bm_george"            # British English, male


def write_wav(path: str, samples, rate: int = 24000) -> None:
    import numpy as np
    pcm = (np.clip(samples, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def serve(voice: str) -> int:
    lock = open(LOCK, "a+")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print("[kokoro] already running", flush=True)
        return 0

    import numpy as np
    import torch
    from kokoro import KModel, KPipeline

    torch.set_num_threads(max(1, (os.cpu_count() or 4) // 2))
    started = time.time()
    model = KModel(config=str(MODEL_DIR / "config.json"),
                   model=str(MODEL_DIR / "kokoro-v1_0.pth")).eval()
    lang = "b" if voice.startswith("b") else "a"
    pipeline = KPipeline(lang_code=lang, model=model)
    voice_path = str(MODEL_DIR / "voices" / f"{voice}.pt")
    list(pipeline("Ready.", voice=voice_path))            # warm-up
    print(f"[kokoro] ready ({voice}, loaded in {time.time() - started:.1f}s)", flush=True)

    if os.path.exists(SOCKET):
        os.remove(SOCKET)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET)
    os.chmod(SOCKET, 0o600)
    server.listen(4)

    while True:
        conn, _ = server.accept()
        with conn:
            try:
                req = json.loads(conn.makefile("r").readline() or "{}")
                cmd = req.get("cmd")
                if cmd in ("ping", "stop"):
                    conn.sendall(b"ok\n")
                    if cmd == "stop":
                        break
                    continue
                t0 = time.time()
                with torch.inference_mode():
                    parts = [r.audio.numpy() for r in pipeline(req["text"], voice=voice_path, speed=1.0)
                             if r.audio is not None]
                audio = np.concatenate(parts) if parts else np.zeros(2400, dtype=np.float32)
                write_wav(req["out"], audio)
                print(f"[kokoro] {len(req['text'])} chars -> {len(audio) / 24000:.1f}s audio "
                      f"in {time.time() - t0:.1f}s", flush=True)
                conn.sendall(b"ok\n")
            except Exception as exc:
                print(f"[kokoro] error: {exc}", flush=True)
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


def lock_free() -> bool:
    with open(LOCK, "a+") as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(f, fcntl.LOCK_UN)
            return True
        except OSError:
            return False


def ensure_server(voice: str, wait: float) -> bool:
    try:
        return request({"cmd": "ping"}, 5) == "ok"
    except OSError:
        pass
    if lock_free():                                   # nobody is starting it yet
        LOG.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG, "ab") as log:
            subprocess.Popen([sys.executable, __file__, "--serve", "--voice", voice],
                             stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                             start_new_session=True)
    deadline = time.time() + wait
    while time.time() < deadline:
        time.sleep(0.5)
        try:
            if request({"cmd": "ping"}, 5) == "ok":
                return True
        except OSError:
            continue
    return False


def configure(voice: str) -> int:
    PROFILE.parent.mkdir(parents=True, exist_ok=True)
    try:
        profile = json.loads(PROFILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        profile = {}
    me = [sys.executable, str(Path(__file__).resolve())]
    profile["tts_cmd"] = me + ["--voice", voice, "--text", "{text}", "--out", "{out}"]
    profile["warm_cmd"] = me + ["--voice", voice, "--warm"]
    profile["live_voice"] = f"kokoro:{voice}"
    profile["cache_dir"] = str(ROOT / "voice" / "cache")
    PROFILE.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
    print(f"Live voice set to Kokoro '{voice}' in {PROFILE}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description="JARVIS live voice (Kokoro)")
    p.add_argument("--serve", action="store_true")
    p.add_argument("--warm", action="store_true")
    p.add_argument("--stop", action="store_true")
    p.add_argument("--configure", action="store_true")
    p.add_argument("--voice", default=DEFAULT_VOICE)
    p.add_argument("--text")
    p.add_argument("--out")
    args = p.parse_args()

    if not (MODEL_DIR / "voices" / f"{args.voice}.pt").exists():
        p.error(f"no such voice: {args.voice} (see {MODEL_DIR / 'voices'})")
    if args.serve:
        return serve(args.voice)
    if args.configure:
        return configure(args.voice)
    if args.stop:
        try:
            request({"cmd": "stop"}, 5)
        except OSError:
            pass
        return 0
    if not ensure_server(args.voice, 300):
        print(f"Kokoro did not start; see {LOG}", file=sys.stderr)
        return 1
    if args.warm:
        return 0
    if not args.text or not args.out:
        p.error("--text and --out are required")
    reply = request({"text": args.text, "out": args.out}, 120)
    if reply != "ok":
        print(reply, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
