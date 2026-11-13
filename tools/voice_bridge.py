#!/usr/bin/env python3
"""
Host microphone bridge for JARVIS OS.

QEMU exposes COM2 as a Unix socket. This script connects to that socket and
sends lines in the form `VOICE:<transcript>`. The guest treats those lines as
microphone transcripts, separate from the COM1 debug terminal.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import uuid
import array
import collections
import json
import math
import os
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
import wave
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
# Phrases whisper invents for silence or faint noise.
HALLUCINATIONS = (
    "thank you", "thanks for watching", "thank you for watching", "you", "bye",
    "so", "okay", "the end", "i m sorry", "thanks",
)
VOICE_PROFILE = Path(__file__).resolve().parent / "voice" / "profile.json"

# Half-duplex state shared between the speaking and listening threads: while
# JARVIS is talking (and for a moment after) the microphone is ignored, and
# anything transcribed that matches what was just said is dropped. Without
# this the bridge heard JARVIS's own voice and answered it, forever.
SEND_LOCK = threading.Lock()          # the brain and the bridge share the socket
BRAIN = None                          # (GuestLink, ask queue) when the language model runs
SPEAKING = threading.Event()
SPOKEN_LOCK = threading.Lock()
RECENT_SPOKEN: collections.deque = collections.deque(maxlen=6)   # (time, normalized text)
LAST_SPEECH_END = [0.0]

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
        with SEND_LOCK:
            sock.sendall(f"VOICE:{text}\n".encode("utf-8"))
        try:
            print(f"[bridge] sent: {text}", flush=True)
        except (BlockingIOError, BrokenPipeError):
            pass
    except (BrokenPipeError, OSError):
        pass


def send_ready(sock: socket.socket, verbose: bool = True) -> None:
    with SEND_LOCK:
        sock.sendall(b"BRIDGE:READY\n")
    if verbose:
        print("[bridge] guest handshake sent", flush=True)


def load_voice_profile() -> dict:
    """tools/voice/profile.json, written by tools/record_voice.py."""
    try:
        return json.loads(VOICE_PROFILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def host_tts_available() -> bool:
    return bool(load_voice_profile().get("tts_cmd")) or shutil.which("spd-say") is not None


def phrase_key(text: str) -> str:
    """Cache file name for a sentence: shared with voice_engine.py --render."""
    return hashlib.sha1(normalize_text(text).encode()).hexdigest()[:16]


def cached_phrase(text: str, profile: dict) -> Path | None:
    """A pre-recorded clip of exactly this sentence: in your own voice first
    (cache_dir, `make voice-render`), else in the live voice (live_cache_dir,
    `make voice-setup`)."""
    for key in ("cache_dir", "live_cache_dir"):
        cache = profile.get(key)
        if cache:
            path = Path(cache) / f"{phrase_key(text)}.wav"
            if path.exists():
                return path
    return None


def split_sentences(text: str) -> list[str]:
    parts = re.split(r"(?<=[.!?])\s+", text.strip())
    return [p for p in parts if p]


def synthesize(text: str, profile: dict, out: Path) -> bool:
    """Run the configured voice engine (profile.json "tts_cmd"); it writes a WAV."""
    template = profile.get("tts_cmd")
    if not template:
        return False
    values = {
        "text": text,
        "out": str(out),
        "ref": str(profile.get("reference_wav", "")),
        "ref_text": str(profile.get("reference_text", "")),
    }
    try:
        cmd = [part.format(**values) for part in template]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    except (OSError, subprocess.SubprocessError, KeyError, IndexError) as exc:
        print(f"[bridge] voice engine failed ({exc}); using system voice", flush=True)
        return False
    return out.exists()


def speak_with_profile(text: str, profile: dict) -> bool:
    """Speak through the voice engine, one sentence at a time: the next
    sentence is synthesised while the current one plays, so a long reply
    starts after the first sentence instead of after the whole text.
    Returns False if nothing could be synthesised (caller falls back)."""
    if not profile.get("tts_cmd") or not shutil.which("pw-play"):
        return False
    sentences = split_sentences(text) or [text]
    with tempfile.TemporaryDirectory(prefix="jarvis-tts-") as tmp:
        clips: list[Path | None] = [None] * len(sentences)
        ready = [threading.Event() for _ in sentences]

        def produce() -> None:
            for i, sentence in enumerate(sentences):
                cached = cached_phrase(sentence, profile)
                if cached:
                    clips[i] = cached
                else:
                    out = Path(tmp) / f"s{i}.wav"
                    clips[i] = out if synthesize(sentence, profile, out) else None
                ready[i].set()

        worker = threading.Thread(target=produce, daemon=True)
        worker.start()
        spoke = False
        for i in range(len(sentences)):
            ready[i].wait()
            if clips[i] is None:
                break
            subprocess.run(["pw-play", str(clips[i])], check=False)
            spoke = True
        worker.join(timeout=1)
        return spoke


def speak_host(text: str, wait: bool = True) -> None:
    text = " ".join(text.strip().split())
    if not text:
        return

    with SPOKEN_LOCK:
        RECENT_SPOKEN.append((time.time(), normalize_text(text)))
    SPEAKING.set()
    try:
        profile = load_voice_profile()
        cached = cached_phrase(text, profile)
        if cached and shutil.which("pw-play"):
            subprocess.run(["pw-play", str(cached)], check=False)   # pre-recorded, your voice
            return
        if speak_with_profile(text, profile):
            return
        if shutil.which("spd-say"):
            # -w: block until finished, so SPEAKING covers the whole utterance.
            subprocess.run(["spd-say", "-w", "-r", "-15", "-t", "male1", text], check=False)
            return
        print(f"[bridge] host TTS unavailable, would say: {text}", flush=True)
    finally:
        LAST_SPEECH_END[0] = time.time()
        SPEAKING.clear()


def is_echo_of_speech(text: str, window: float = 20.0) -> bool:
    """True if the transcript is (mostly) something JARVIS itself just said."""
    words = normalize_text(text).split()
    if not words:
        return False
    now = time.time()
    with SPOKEN_LOCK:
        recent = [spoken for when, spoken in RECENT_SPOKEN if now - when <= window]
    for spoken in recent:
        spoken_words = set(spoken.split())
        hits = sum(1 for w in words if w in spoken_words)
        if hits / len(words) >= 0.6:
            return True
    return False


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
            if line.startswith("ASK:") and BRAIN:
                text = line[4:].strip().replace("\\n", " ")
                print(f"[bridge] asking the brain: {text}", flush=True)
                BRAIN[1].put(text)
                continue
            if line.startswith("RESULT:") and BRAIN:
                BRAIN[0].deliver(line)
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


# Measured on a 4-core laptop CPU (i5-8250U), model kept loaded:
#   base.en  ~2 s per sentence, exact on commands
#   medium.en ~55 s per sentence (too slow for conversation)
MODEL_PREFERENCE = ("ggml-base.en.bin", "ggml-small.en.bin", "ggml-medium.en.bin")


def pick_model(explicit: str | None) -> str | None:
    """The given model, or the best installed one for live use (fastest
    first: on a laptop CPU only base.en answers quickly enough to talk to)."""
    if explicit:
        return explicit
    stt = Path(__file__).resolve().parent / "models" / "stt"
    for name in MODEL_PREFERENCE:
        if (stt / name).exists():
            return str(stt / name)
    return None


class WhisperServer:
    """whisper.cpp's server, started once and kept running, so the model is
    loaded a single time instead of once per sentence."""

    def __init__(self, exe: str, model: str, threads: int):
        self.port = 8178
        log = open(Path(tempfile.gettempdir()) / "jarvis-whisper.log", "ab")
        self.proc = subprocess.Popen(
            [exe, "-m", model, "-t", str(threads), "--host", "127.0.0.1", "--port", str(self.port),
             "-l", "en", "-nf", "-sns", "--prompt", WHISPER_PROMPT],
            stdout=log, stderr=log, stdin=subprocess.DEVNULL)
        deadline = time.time() + 180
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("whisper-server exited (port in use, or bad model?)")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=1):
                    return
            except OSError:
                time.sleep(0.5)
        raise RuntimeError("whisper-server did not start")

    def transcribe(self, wav: Path) -> str:
        boundary = uuid.uuid4().hex
        fields = {"response_format": "text", "temperature": "0.0"}
        body = b""
        for name, value in fields.items():
            body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n"
                     f"{value}\r\n").encode()
        body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
                 f"Content-Type: audio/wav\r\n\r\n").encode() + wav.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=300)
        try:
            conn.request("POST", "/inference", body, {"Content-Type": f"multipart/form-data; boundary={boundary}"})
            resp = conn.getresponse()
            text = resp.read().decode("utf-8", errors="ignore")
            return text.strip() if resp.status == 200 else ""
        finally:
            conn.close()

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()


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
    if normalized in HALLUCINATIONS or normalized.startswith("i heard"):
        return True
    if is_echo_of_speech(text):
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


FRAME_MS = 30
FRAME_SAMPLES = 16000 * FRAME_MS // 1000
FRAME_BYTES = FRAME_SAMPLES * 2


def frame_rms(frame: bytes) -> float:
    samples = array.array("h")
    samples.frombytes(frame)
    if not samples:
        return 0.0
    return math.sqrt(sum(v * v for v in samples) / len(samples))


def listen_utterances(target: str | None, max_seconds: float):
    """Yield one complete spoken utterance at a time as 16 kHz mono PCM.

    The microphone is read continuously and cut where the speaker pauses,
    instead of in fixed 4-second blocks that chopped sentences in half and
    sent pure silence to whisper (which then invented text for it).
    """
    cmd = ["pw-record", "--rate", "16000", "--channels", "1", "--format", "s16", "--raw", "-"]
    if target:
        cmd[1:1] = ["--target", target]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    noise = 150.0                                   # running background level
    preroll: collections.deque = collections.deque(maxlen=10)   # 0.3 s before speech
    voiced: list[bytes] = []
    loud_run = 0
    quiet_run = 0
    in_speech = False
    end_quiet = int(800 / FRAME_MS)                 # 0.8 s pause ends an utterance
    max_frames = int(max_seconds * 1000 / FRAME_MS)

    try:
        while True:
            frame = proc.stdout.read(FRAME_BYTES)
            if len(frame) < FRAME_BYTES:
                return

            # Deaf while JARVIS is speaking, and briefly after (room echo).
            if SPEAKING.is_set() or time.time() - LAST_SPEECH_END[0] < 0.7:
                voiced.clear(); preroll.clear()
                in_speech = False; loud_run = quiet_run = 0
                continue

            level = frame_rms(frame)
            threshold = max(350.0, noise * 3.0)
            loud = level > threshold

            if not in_speech:
                preroll.append(frame)
                if loud:
                    loud_run += 1
                else:
                    loud_run = 0
                    noise = noise * 0.95 + level * 0.05
                if loud_run >= 4:                   # 120 ms of sound = speech started
                    in_speech = True
                    voiced = list(preroll)
                    quiet_run = 0
                continue

            voiced.append(frame)
            quiet_run = 0 if loud else quiet_run + 1
            if quiet_run >= end_quiet or len(voiced) >= max_frames:
                speech_frames = len(voiced) - quiet_run
                pcm = b"".join(voiced)
                voiced = []; preroll.clear()
                in_speech = False; loud_run = quiet_run = 0
                if speech_frames * FRAME_MS >= 350:     # ignore clicks and coughs
                    yield pcm
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()


def write_wav(path: Path, pcm: bytes) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm)


WHISPER_PROMPT = ("Jarvis, open terminal. Open files. Open settings. Show status. "
                  "Lock the computer. What can you do? Shut down.")


def transcribe_with_whisper(whisper_cli: str, model: str, wav: Path) -> str:
    out_prefix = wav.with_suffix("")
    cmd = [
        whisper_cli,
        "-m", model,
        "-f", str(wav),
        "-l", "en",
        "-t", str(min(8, os.cpu_count() or 4)),
        "-nt",                      # no timestamps
        "-sns",                     # no "(music)" / "[noise]" tokens
        "-nf",                      # no temperature fallback: fewer inventions
        "--prompt", WHISPER_PROMPT, # bias towards the commands JARVIS knows
        "-otxt",
        "-of", str(out_prefix),
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

    args.model = pick_model(args.model)
    if not args.model or not Path(args.model).exists():
        raise SystemExit(f"[bridge] Whisper model not found: {args.model}")

    server = None
    server_exe = Path(whisper_cli).with_name("whisper-server")
    if server_exe.exists():
        try:
            server = WhisperServer(str(server_exe), args.model, max(1, (os.cpu_count() or 4) // 2))
            print(f"[bridge] speech recognition: {Path(args.model).name}, kept loaded", flush=True)
        except RuntimeError as exc:
            print(f"[bridge] whisper-server unavailable ({exc}); loading the model per sentence", flush=True)

    wake_words = tuple(args.wake_word or DEFAULT_WAKE_WORDS)
    require_wake = args.require_wake and not args.no_wake
    awaiting_command = False
    wake_deadline = 0.0
    startup_deadline = time.time() + args.startup_listen

    # Load the voice engine now so the first reply is not delayed by it.
    warm = load_voice_profile().get("warm_cmd")
    if warm and not args.no_host_tts:
        print("[bridge] loading your voice engine in the background...", flush=True)
        subprocess.Popen(warm, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         stdin=subprocess.DEVNULL)

    print("[bridge] listening with host microphone.", flush=True)
    if not args.no_host_tts:
        if host_tts_available():
            engine = "your recorded voice profile" if load_voice_profile().get("tts_cmd") else "spd-say (system voice)"
            print(f"[bridge] host TTS ready via {engine}", flush=True)
        else:
            print("[bridge] host TTS missing: install speech-dispatcher/espeak for real speech", flush=True)
    if require_wake:
        if args.startup_listen > 0:
            print(f"[bridge] first command can be spoken directly for {args.startup_listen:.0f}s", flush=True)
        print("[bridge] wake mode: say 'Jarvis' before a command.", flush=True)
    else:
        print("[bridge] conversation mode: forwarding speech after noise filtering.", flush=True)
    for pcm in listen_utterances(args.target, max(args.seconds, 12.0)):
        now = time.time()
        if awaiting_command and now > wake_deadline:
            awaiting_command = False
            print("[bridge] wake timed out", flush=True)

        with tempfile.TemporaryDirectory(prefix="jarvis-voice-") as tmp:
            wav = Path(tmp) / "utterance.wav"
            write_wav(wav, pcm)
            started = time.time()
            transcript = (server.transcribe(wav) if server
                          else transcribe_with_whisper(whisper_cli, args.model, wav))
            print(f"[bridge] heard in {time.time() - started:.1f}s: {transcript!r}", flush=True)
            transcript = clean_transcript(transcript)
            if not transcript:
                continue
            # JARVIS started talking while this was being transcribed: what
            # was captured may include the start of its own reply.
            if SPEAKING.is_set():
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
    parser.add_argument("--model", default=None,
                        help="Whisper model path (default: base.en, else small.en, else medium.en).")
    parser.add_argument("--whisper-cli", help="Path to whisper.cpp whisper-cli.")
    parser.add_argument("--seconds", type=float, default=4.0, help="Longest single utterance, in seconds (minimum 12).")
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
    parser.add_argument("--no-brain", action="store_true", help="Do not run the language model (built-in replies only).")
    parser.add_argument("--brain-model", help="GGUF model for the brain (default: tools/models/brain/model.gguf).")
    parser.add_argument("--once", help="Inject one transcript and exit.")
    args = parser.parse_args()

    if args.list_sources:
        subprocess.run(["wpctl", "status"], check=False)
        return 0

    sock = connect_socket(args.socket)
    print(f"[bridge] connected: {args.socket}", flush=True)
    global BRAIN
    if not args.no_brain:
        try:
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            import jarvis_brain
            BRAIN = jarvis_brain.start_for_bridge(sock, SEND_LOCK, args.brain_model)
        except Exception as exc:
            print(f"[bridge] brain not started: {exc}", flush=True)
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
