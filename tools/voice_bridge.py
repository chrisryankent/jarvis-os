#!/usr/bin/env python3
"""
Host microphone bridge for JARVIS OS.

QEMU exposes COM2 as a Unix socket. This script connects to that socket and
sends lines in the form `VOICE:<transcript>`. The guest treats those lines as
microphone transcripts, separate from the COM1 debug terminal.
"""

from __future__ import annotations

import argparse
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

DEFAULT_WAKE_WORDS = ("jarvis", "hey jarvis", "javis", "gavis", "travis")
NOISE_TERMS = (
    "music",
    "dramatic",
    "suspenseful",
    "applause",
    "laughter",
    "laughing",
    "chattering",
    "people chattering",
    "indistinct chatter",
    "foreign language",
    "car engine",
    "engine revving",
    "background noise",
    "silence",
    "inaudible",
    "blank audio",
)
SELF_AUDIO_TERMS = (
    "jarvis ready",
    "jarvis is ready",
    "welcome jarvis user",
    "say a command",
    "what would you like to do",
)


def connect_socket(path: str, retries: int = 120) -> socket.socket:
    last_error: OSError | None = None
    for _ in range(retries):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(path)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.25)
    raise SystemExit(f"[bridge] Could not connect to {path}: {last_error}")


def send_voice(sock: socket.socket, text: str) -> None:
    text = " ".join(text.strip().split())
    if not text:
        return
    try:
        sock.sendall(f"VOICE:{text}\n".encode("utf-8"))
        try:
            print(f"[bridge] sent: {text}", flush=True)
        except (BlockingIOError, BrokenPipeError):
            pass
    except (BrokenPipeError, OSError):
        pass


def send_ready(sock: socket.socket, verbose: bool = True) -> None:
    sock.sendall(b"BRIDGE:READY\n")
    if verbose:
        print("[bridge] guest handshake sent", flush=True)


def host_tts_available() -> bool:
    return shutil.which("spd-say") is not None


def speak_host(text: str, wait: bool = True) -> None:
    text = " ".join(text.strip().split())
    if not text:
        return

    if shutil.which("spd-say"):
        cmd = ["spd-say"]
        if wait:
            cmd.append("-w")
        cmd.append(text)
        subprocess.run(cmd, check=False)
        return

    print(f"[bridge] host TTS unavailable, would say: {text}", flush=True)


def guest_reader(sock: socket.socket, args: argparse.Namespace, stop: threading.Event) -> None:
    buf = ""
    while not stop.is_set():
        try:
            ready, _, _ = select.select([sock], [], [], 0.25)
            if not ready:
                continue
            data = sock.recv(1024)
            if not data:
                return
        except OSError:
            return

        buf += data.decode("utf-8", errors="ignore")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.strip()
            if not line:
                continue
            if line.startswith("SAY:"):
                text = line[4:].strip()
                try:
                    print(f"[bridge] host speaking: {text}", flush=True)
                except (BlockingIOError, BrokenPipeError):
                    pass
                if not args.no_host_tts:
                    speak_host(text, wait=not args.async_host_tts)
            else:
                try:
                    print(f"[bridge] guest: {line}", flush=True)
                except (BlockingIOError, BrokenPipeError):
                    pass


def ready_announcer(sock: socket.socket, args: argparse.Namespace, stop: threading.Event) -> None:
    deadline = time.time() + args.ready_repeat
    first = True
    while not stop.is_set() and time.time() < deadline:
        try:
            send_ready(sock, verbose=first)
            first = False
        except OSError:
            return
        time.sleep(0.5)


def find_whisper_cli(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for candidate in (
        "whisper-cli",
        "whisper.cpp",
        "main",
        "tools/vendor/whisper.cpp/build/bin/whisper-cli",
        "tools/vendor/whisper.cpp/build/bin/main",
    ):
        path = shutil.which(candidate) or (candidate if Path(candidate).exists() else None)
        if path:
            return path
    return None


def normalize_text(text: str) -> str:
    text = text.lower().replace("'", "")
    text = re.sub(r"[^a-z0-9]+", " ", text)
    return " ".join(text.split())


def clean_transcript(text: str) -> str:
    text = re.sub(r"\[[^\]]*\]", " ", text)
    return " ".join(text.strip().split())


def is_noise_or_self_audio(text: str) -> bool:
    normalized = normalize_text(text)
    if not normalized:
        return True

    if any(term in normalized for term in SELF_AUDIO_TERMS):
        return True

    bracketed = text.strip().startswith(("(", "[")) and text.strip().endswith((")", "]"))
    if bracketed and any(term in normalized for term in NOISE_TERMS):
        return True

    words = normalized.split()
    if len(words) <= 4 and any(term in normalized for term in NOISE_TERMS):
        return True

    return False


def wake_command(text: str, wake_words: tuple[str, ...]) -> tuple[bool, str]:
    words = normalize_text(text).split()
    if not words:
        return False, ""

    wake_tokens = sorted((normalize_text(w).split() for w in wake_words), key=len, reverse=True)
    for index in range(0, min(len(words), 3)):
        for tokens in wake_tokens:
            if tokens and words[index:index + len(tokens)] == tokens:
                command = " ".join(words[index + len(tokens):])
                return True, command
    return False, ""


def record_with_pw_record(path: Path, seconds: float, target: str | None = None) -> None:
    cmd = [
        "pw-record",
        "--rate",
        "16000",
        "--channels",
        "1",
        "--format",
        "s16",
        str(path),
    ]
    if target:
        cmd[1:1] = ["--target", target]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(seconds)
    finally:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)


def transcribe_with_whisper(whisper_cli: str, model: str, wav: Path) -> str:
    out_prefix = wav.with_suffix("")
    cmd = [
        whisper_cli,
        "-m",
        model,
        "-f",
        str(wav),
        "-nt",
        "-otxt",
        "-of",
        str(out_prefix),
    ]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    txt_path = out_prefix.with_suffix(".txt")
    if not txt_path.exists():
        return ""
    return txt_path.read_text(encoding="utf-8", errors="ignore").strip()


def run_whisper_loop(args: argparse.Namespace, sock: socket.socket) -> None:
    if not shutil.which("pw-record"):
        raise SystemExit("[bridge] pw-record is required for microphone capture on this host.")

    whisper_cli = find_whisper_cli(args.whisper_cli)
    if not whisper_cli:
        raise SystemExit(
            "[bridge] No whisper-cli found. Build whisper.cpp or pass --whisper-cli /path/to/whisper-cli."
        )

    if not Path(args.model).exists():
        raise SystemExit(f"[bridge] Whisper model not found: {args.model}")

    wake_words = tuple(args.wake_word or DEFAULT_WAKE_WORDS)
    require_wake = args.require_wake and not args.no_wake
    awaiting_command = False
    wake_deadline = 0.0
    startup_deadline = time.time() + args.startup_listen

    print("[bridge] listening with host microphone.", flush=True)
    if not args.no_host_tts:
        if host_tts_available():
            print("[bridge] host TTS ready via spd-say", flush=True)
        else:
            print("[bridge] host TTS missing: install speech-dispatcher/espeak for real speech", flush=True)
    if require_wake:
        if args.startup_listen > 0:
            print(f"[bridge] first command can be spoken directly for {args.startup_listen:.0f}s", flush=True)
        print("[bridge] wake mode: say 'Jarvis' before a command.", flush=True)
    else:
        print("[bridge] conversation mode: forwarding speech after noise filtering.", flush=True)
    while True:
        now = time.time()
        if awaiting_command and now > wake_deadline:
            awaiting_command = False
            print("[bridge] wake timed out", flush=True)

        with tempfile.TemporaryDirectory(prefix="jarvis-voice-") as tmp:
            wav = Path(tmp) / "utterance.wav"
            record_with_pw_record(wav, args.seconds, args.target)
            transcript = transcribe_with_whisper(whisper_cli, args.model, wav)
            transcript = clean_transcript(transcript)
            if not transcript:
                continue
            if is_noise_or_self_audio(transcript):
                print(f"[bridge] ignored noise/self audio: {transcript}", flush=True)
                continue
            if not require_wake:
                send_voice(sock, transcript)
                continue

            woke, command = wake_command(transcript, wake_words)
            if woke and command:
                send_voice(sock, command)
                awaiting_command = False
            elif woke:
                awaiting_command = True
                wake_deadline = time.time() + args.wake_timeout
                print("[bridge] wake heard; listening for command", flush=True)
            elif args.startup_listen > 0 and time.time() <= startup_deadline:
                send_voice(sock, transcript)
            elif awaiting_command:
                send_voice(sock, transcript)
                awaiting_command = False
            else:
                print(f"[bridge] ignored until wake word: {transcript}", flush=True)


def run_stdin_loop(sock: socket.socket) -> None:
    print("[bridge] stdin mode. Type a transcript to inject, Ctrl+D to stop.", flush=True)
    for line in sys.stdin:
        send_voice(sock, line)


def main() -> int:
    parser = argparse.ArgumentParser(description="Send host STT transcripts to JARVIS OS over QEMU COM2.")
    parser.add_argument("--socket", default="/tmp/jarvis-voice.sock", help="QEMU COM2 Unix socket.")
    parser.add_argument("--model", default="tools/models/stt/ggml-medium.en.bin", help="Whisper model path.")
    parser.add_argument("--whisper-cli", help="Path to whisper.cpp whisper-cli.")
    parser.add_argument("--seconds", type=float, default=4.0, help="Seconds to record per utterance.")
    parser.add_argument("--hold", type=float, default=1.0, help="Seconds to keep the socket open after --once.")
    parser.add_argument("--target", help="PipeWire target/source name for pw-record.")
    parser.add_argument("--list-sources", action="store_true", help="Show PipeWire devices and exit.")
    parser.add_argument("--wake-word", action="append", help="Wake word alias. Can be passed more than once.")
    parser.add_argument("--wake-timeout", type=float, default=10.0, help="Seconds to wait for a command after wake.")
    parser.add_argument("--startup-listen", type=float, default=20.0, help="Seconds to accept one direct command after boot.")
    parser.add_argument("--ready-repeat", type=float, default=18.0, help="Seconds to repeat guest bridge-ready handshakes after connecting.")
    parser.add_argument("--no-host-tts", action="store_true", help="Do not speak guest SAY messages on the host.")
    parser.add_argument("--async-host-tts", action="store_true", help="Do not wait for host TTS to finish.")
    parser.add_argument("--require-wake", action="store_true", help="Require a wake word before forwarding transcripts.")
    parser.add_argument("--no-wake", action="store_true", help="Forward every non-noise transcript. This is the default.")
    parser.add_argument("--stdin", action="store_true", help="Inject typed transcripts for bridge testing.")
    parser.add_argument("--once", help="Inject one transcript and exit.")
    args = parser.parse_args()

    if args.list_sources:
        subprocess.run(["wpctl", "status"], check=False)
        return 0

    sock = connect_socket(args.socket)
    print(f"[bridge] connected: {args.socket}", flush=True)
    stop_reader = threading.Event()
    reader = threading.Thread(target=guest_reader, args=(sock, args, stop_reader), daemon=True)
    reader.start()
    ready = threading.Thread(target=ready_announcer, args=(sock, args, stop_reader), daemon=True)
    ready.start()

    try:
        if args.once:
            send_voice(sock, args.once)
            time.sleep(args.hold)
            return 0

        if args.stdin:
            run_stdin_loop(sock)
            return 0

        run_whisper_loop(args, sock)
        return 0
    finally:
        stop_reader.set()


if __name__ == "__main__":
    raise SystemExit(main())
