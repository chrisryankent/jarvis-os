#!/usr/bin/env python3
"""
Record your own voice for JARVIS.

Reads a set of sentences to you one at a time, records each with the host
microphone, and writes:

  tools/voice/samples/NN.wav + NN.txt   every clip with its exact text
  tools/voice/reference.wav             a ~15 s reference built from the clips
  tools/voice/profile.json              what the voice bridge reads

A voice-cloning speech engine uses reference.wav (plus the matching text) to
speak new sentences in that voice. This script only records; it does not
install an engine. Point profile.json's "tts_cmd" at one when you have it
(see the "tts_cmd_help" field the script writes).

Only record a voice you have the right to use: your own.

usage: python3 tools/record_voice.py [--target <pipewire source>] [--redo N]
"""
from __future__ import annotations

import argparse
import json
import shutil
import signal
import subprocess
import sys
import wave
from pathlib import Path

VOICE_DIR = Path(__file__).resolve().parent / "voice"
SAMPLES = VOICE_DIR / "samples"

# Varied sounds and intonation: statements, questions, numbers, names.
SENTENCES = [
    "Good morning. All systems are online and ready.",
    "I have finished scanning the files you asked about.",
    "Would you like me to open the terminal, or the settings?",
    "The quick brown fox jumps over the lazy dog near the river bank.",
    "Memory usage is at forty two percent, and the processor is idle.",
    "Warning: an unknown process is requesting access to the network.",
    "She sells sea shells by the shore every Thursday through August.",
    "I could not find that file. Shall I search the other folders?",
    "Seven, eight, nine, ten. The backup completed at three fifteen.",
    "Very well. I will lock the computer and wait for your voice.",
    "Juggling zebras quickly vexed the enthusiastic jazz musician.",
    "Is there anything else you need before I shut the system down?",
]


def record_clip(path: Path, target: str | None) -> None:
    cmd = ["pw-record", "--rate", "24000", "--channels", "1", "--format", "s16", str(path)]
    if target:
        cmd[1:1] = ["--target", target]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        input("      recording... press Enter when you have finished the sentence ")
    finally:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def clip_seconds(path: Path) -> float:
    with wave.open(str(path), "rb") as w:
        return w.getnframes() / float(w.getframerate())


def build_reference(indices: list[int]) -> tuple[float, str]:
    """Join clips into reference.wav until it is about 15 seconds long."""
    frames, texts, total = [], [], 0.0
    for i in indices:
        wav = SAMPLES / f"{i:02d}.wav"
        with wave.open(str(wav), "rb") as w:
            frames.append(w.readframes(w.getnframes()))
        texts.append(SENTENCES[i - 1])
        total += clip_seconds(wav)
        if total >= 15.0:
            break
    with wave.open(str(VOICE_DIR / "reference.wav"), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(24000)
        out.writeframes(b"".join(frames))
    return total, " ".join(texts)


def main() -> int:
    parser = argparse.ArgumentParser(description="Record your voice for JARVIS.")
    parser.add_argument("--target", help="PipeWire source name (see: wpctl status)")
    parser.add_argument("--redo", type=int, action="append",
                        help="Re-record only this sentence number (repeatable)")
    args = parser.parse_args()

    if not shutil.which("pw-record"):
        sys.exit("pw-record is required (PipeWire).")
    SAMPLES.mkdir(parents=True, exist_ok=True)

    todo = args.redo or list(range(1, len(SENTENCES) + 1))
    print("Find a quiet room. Speak naturally, at the distance you normally sit")
    print("from the microphone. Press Enter to start each sentence.\n")

    for i in todo:
        if not 1 <= i <= len(SENTENCES):
            continue
        wav = SAMPLES / f"{i:02d}.wav"
        while True:
            print(f"[{i}/{len(SENTENCES)}]  {SENTENCES[i - 1]}")
            input("      press Enter, then read it aloud ")
            record_clip(wav, args.target)
            secs = clip_seconds(wav) if wav.exists() else 0.0
            if secs < 1.0:
                print("      that was too short; let's try again\n")
                continue
            if shutil.which("pw-play") and input(f"      {secs:.1f}s recorded. Listen back? [y/N] ").lower() == "y":
                subprocess.run(["pw-play", str(wav)], check=False)
            if input("      keep it? [Y/n] ").lower() != "n":
                break
        (SAMPLES / f"{i:02d}.txt").write_text(SENTENCES[i - 1] + "\n", encoding="utf-8")
        print()

    have = [i for i in range(1, len(SENTENCES) + 1) if (SAMPLES / f"{i:02d}.wav").exists()]
    if not have:
        sys.exit("No clips recorded.")
    seconds, ref_text = build_reference(have)

    profile_path = VOICE_DIR / "profile.json"
    profile = {}
    if profile_path.exists():
        try:
            profile = json.loads(profile_path.read_text(encoding="utf-8"))
        except ValueError:
            profile = {}
    profile.update({
        "reference_wav": str(VOICE_DIR / "reference.wav"),
        "reference_text": ref_text,
        "clips": len(have),
        "tts_cmd": profile.get("tts_cmd"),
        "tts_cmd_help": (
            "Set tts_cmd to a list: the command of a voice-cloning speech engine. "
            "Placeholders: {text} what to say, {ref} reference.wav, {ref_text} its "
            "transcript, {out} the WAV file the command must write. While tts_cmd "
            "is null, JARVIS speaks with the system voice."
        ),
    })
    profile_path.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")

    print(f"Saved {len(have)} clips and a {seconds:.0f}s reference to {VOICE_DIR}")
    print(f"Profile: {profile_path}")
    if not profile.get("tts_cmd"):
        print("No voice engine is configured yet, so JARVIS still uses the system voice.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
