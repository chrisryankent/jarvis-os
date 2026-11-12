#!/usr/bin/env python3
"""
=============================================================================
JARVIS AI OS — Model Downloader  (tools/download_models.py)

Downloads all required HuggingFace models into the correct directories.
Run this BEFORE make iso or the installer.

Usage:
    python3 tools/download_models.py [--models-dir ./models] [--skip-large]
    python3 tools/download_models.py --list        # show what will be downloaded
    python3 tools/download_models.py --brain-only  # just the LLM

Requirements:
    pip install huggingface_hub tqdm
=============================================================================
"""

import os
import sys
import argparse
import json
from pathlib import Path
from urllib.request import urlopen

try:
    from huggingface_hub import hf_hub_download, snapshot_download, HfApi
    from huggingface_hub.utils import RepositoryNotFoundError
    HAS_HF = True
except ImportError:
    HAS_HF = False
    print("ERROR: huggingface_hub not installed.")
    print("Run: pip install huggingface_hub tqdm")
    sys.exit(1)

# =============================================================================
# Model registry — all models JARVIS needs
# =============================================================================
MODELS = {
    # ---- LLM Brain (choose ONE, comment out the others) -------------------
    "brain": {
        "description": "LLM Brain — reasoning, conversation, tool calls",
        "slot":        "MODEL_SLOT:LLM_BRAIN in ai_core/runtime.cpp",
        "size_gb":     2.0,
        "required":    True,
        "options": [
            {
                "name":    "Llama-3.2-3B-Instruct Q4_K_M (recommended)",
                "repo":    "bartowski/Llama-3.2-3B-Instruct-GGUF",
                "file":    "Llama-3.2-3B-Instruct-Q4_K_M.gguf",
                "dest":    "brain/model.gguf",
                "type":    "single_file",
                "size_gb": 2.0,
            },
            {
                "name":    "Phi-3-mini Q4_K_M (smaller, fast on CPU)",
                "repo":    "bartowski/Phi-3-mini-4k-instruct-GGUF",
                "file":    "Phi-3-mini-4k-instruct-Q4_K_M.gguf",
                "dest":    "brain/model.gguf",
                "type":    "single_file",
                "size_gb": 2.2,
            },
            {
                "name":    "Qwen2.5-7B-Instruct Q4_K_M (smarter, needs 8 GB RAM)",
                "repo":    "bartowski/Qwen2.5-7B-Instruct-GGUF",
                "file":    "Qwen2.5-7B-Instruct-Q4_K_M.gguf",
                "dest":    "brain/model.gguf",
                "type":    "single_file",
                "size_gb": 4.7,
            },
        ],
        "default_option": 0,  # Llama 3.2 3B
    },

    # ---- Speech-to-Text ---------------------------------------------------
    "stt": {
        "description": "Whisper STT — converts microphone audio to text",
        "slot":        "MODEL_SLOT:SPEECH_STT in speech/speech_engine.cpp",
        "size_gb":     1.5,
        "required":    True,
        "options": [
            {
                "name":    "Whisper medium.en GGUF (balanced accuracy/speed)",
                "repo":    "ggerganov/whisper.cpp",
                "file":    "ggml-medium.en.bin",
                "dest":    "stt/ggml-medium.en.bin",
                "type":    "single_file",
                "size_gb": 1.5,
            },
            {
                "name":    "Whisper tiny.en GGUF (fastest, less accurate)",
                "repo":    "ggerganov/whisper.cpp",
                "file":    "ggml-tiny.en.bin",
                "dest":    "stt/ggml-tiny.en.bin",
                "type":    "single_file",
                "size_gb": 0.08,
            },
            {
                "name":    "Whisper large-v3 GGUF (most accurate, needs 12 GB)",
                "repo":    "ggerganov/whisper.cpp",
                "file":    "ggml-large-v3.bin",
                "dest":    "stt/ggml-large-v3.bin",
                "type":    "single_file",
                "size_gb": 3.1,
            },
        ],
        "default_option": 0,
    },

    # ---- Text-to-Speech ---------------------------------------------------
    "tts": {
        "description": "Kokoro TTS — converts text to JARVIS voice",
        "slot":        "MODEL_SLOT:SPEECH_TTS in speech/speech_engine.cpp",
        "size_gb":     0.33,
        "required":    True,
        "options": [
            {
                "name":    "Kokoro-82M ONNX (fast, natural voice)",
                "repo":    "hexgrad/Kokoro-82M",
                "file":    None,  # snapshot (whole repo)
                "dest":    "tts/",
                "type":    "snapshot",
                "size_gb": 0.33,
            },
        ],
        "default_option": 0,
    },

    # ---- Wake Word --------------------------------------------------------
    "wake": {
        "description": "Wake word detector — listens for 'Hey JARVIS'",
        "slot":        "MODEL_SLOT:WAKE_WORD in speech/speech_engine.cpp",
        "size_gb":     0.05,
        "required":    True,
        "options": [
            {
                "name":    "openWakeWord hey_jarvis ONNX",
                "repo":    "dscripka/openWakeWord GitHub release",
                "url":     "https://github.com/dscripka/openWakeWord/releases/download/v0.5.1/hey_jarvis_v0.1.onnx",
                "file":    None,
                "dest":    "wake/wake_word.onnx",
                "type":    "url_file",
                "size_gb": 0.01,
            },
        ],
        "default_option": 0,
    },

    # ---- Speaker Verification (voice auth) --------------------------------
    "voice_auth": {
        "description": "ECAPA-TDNN — voice biometric authentication",
        "slot":        "MODEL_SLOT:VOICE_AUTH in security/security.cpp",
        "size_gb":     0.1,
        "required":    False,
        "options": [
            {
                "name":    "SpeechBrain ECAPA-TDNN speaker verification",
                "repo":    "speechbrain/spkrec-ecapa-voxceleb",
                "file":    None,
                "dest":    "security/speaker_verify/",
                "type":    "snapshot",
                "size_gb": 0.1,
            },
        ],
        "default_option": 0,
    },

    # ---- File Embedder ----------------------------------------------------
    "file_embedder": {
        "description": "Sentence transformer — semantic file search",
        "slot":        "MODEL_SLOT:FILE_EMBEDDER in services/ai_filesystem.py",
        "size_gb":     0.09,
        "required":    False,
        "options": [
            {
                "name":    "all-MiniLM-L6-v2 (22 MB, fast semantic search)",
                "repo":    "sentence-transformers/all-MiniLM-L6-v2",
                "file":    None,
                "dest":    "filesystem/embedder/",
                "type":    "snapshot",
                "size_gb": 0.09,
            },
        ],
        "default_option": 0,
    },
}


# =============================================================================
# Downloader
# =============================================================================
class ModelDownloader:

    def __init__(self, models_dir: str, hf_token: str = None):
        self.models_dir = Path(models_dir)
        self.hf_token   = hf_token or os.environ.get("HF_TOKEN")
        self.api        = HfApi(token=self.hf_token)

    def list_models(self):
        print("\n JARVIS AI OS — Model Registry")
        print("=" * 72)
        total_gb = 0.0
        for key, info in MODELS.items():
            req = "REQUIRED" if info["required"] else "optional"
            opt = info["options"][info["default_option"]]
            print(f"\n  [{key.upper()}] {info['description']}")
            print(f"    Slot:     {info['slot']}")
            print(f"    Default:  {opt['name']}")
            print(f"    Size:     ~{opt['size_gb']:.1f} GB")
            print(f"    Status:   {req}")
            total_gb += opt['size_gb']
        print(f"\n  Total download: ~{total_gb:.1f} GB")
        print("=" * 72)

    def download_all(self, skip_optional: bool = False,
                     brain_only: bool = False):
        print(f"\n[JARVIS] Downloading models to: {self.models_dir}")
        self.models_dir.mkdir(parents=True, exist_ok=True)

        results = {}
        for key, info in MODELS.items():
            if brain_only and key != "brain":
                continue
            if skip_optional and not info["required"]:
                print(f"\n[SKIP] {key} (optional, use --no-skip to include)")
                continue
            results[key] = self.download_model(key, info)

        # Summary
        print("\n" + "=" * 60)
        print("JARVIS Model Download Summary")
        print("=" * 60)
        for key, ok in results.items():
            status = "✓  OK" if ok else "✗ FAILED"
            print(f"  {status}   {key}")
        print()

        # Write manifest
        manifest = {k: str(self.models_dir / MODELS[k]["options"][MODELS[k]["default_option"]]["dest"])
                    for k in results if results[k]}
        manifest_path = self.models_dir / "manifest.json"
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2)
        print(f"Manifest written: {manifest_path}")

    def download_model(self, key: str, info: dict) -> bool:
        opt  = info["options"][info["default_option"]]
        dest = self.models_dir / opt["dest"]

        print(f"\n[DOWNLOAD] {key}: {opt['name']}")
        print(f"  Repo: {opt['repo']}")
        print(f"  Size: ~{opt['size_gb']:.1f} GB")
        print(f"  Dest: {dest}")

        # Check if already downloaded
        if opt["type"] == "single_file":
            if dest.exists() and dest.stat().st_size > 1024:
                print(f"  [SKIP] Already exists ({dest.stat().st_size // 1024 // 1024} MB)")
                return True
        elif opt["type"] == "snapshot":
            if dest.is_dir() and any(dest.iterdir()):
                print(f"  [SKIP] Already exists")
                return True

        try:
            dest.parent.mkdir(parents=True, exist_ok=True)

            if opt["type"] == "single_file":
                path = hf_hub_download(
                    repo_id  = opt["repo"],
                    filename = opt["file"],
                    token    = self.hf_token,
                    local_dir= str(dest.parent),
                    local_dir_use_symlinks=False,
                )
                # Rename to expected filename
                downloaded = Path(path)
                if downloaded.name != dest.name:
                    downloaded.rename(dest)
                print(f"  [OK] Downloaded to {dest}")
                return True

            elif opt["type"] == "url_file":
                url = opt["url"]
                print(f"  URL:  {url}")
                with urlopen(url) as response:
                    data = response.read()
                with open(dest, "wb") as f:
                    f.write(data)
                print(f"  [OK] Downloaded to {dest} ({dest.stat().st_size // 1024} KB)")
                return True

            elif opt["type"] == "snapshot":
                dest.mkdir(parents=True, exist_ok=True)
                snapshot_download(
                    repo_id   = opt["repo"],
                    local_dir = str(dest),
                    token     = self.hf_token,
                    local_dir_use_symlinks=False,
                    ignore_patterns=["*.msgpack", "flax_model*", "tf_model*", "rust_model*"],
                )
                print(f"  [OK] Snapshot downloaded to {dest}")
                return True

        except RepositoryNotFoundError:
            print(f"  [ERROR] Repo not found: {opt['repo']}")
            return False
        except Exception as e:
            print(f"  [ERROR] {e}")
            return False

        return False


# =============================================================================
# CLI
# =============================================================================
def main():
    parser = argparse.ArgumentParser(
        description="JARVIS AI OS — HuggingFace Model Downloader")
    parser.add_argument("--models-dir", default="./models",
                        help="Directory to store models (default: ./models)")
    parser.add_argument("--token", default=None,
                        help="HuggingFace API token (or set HF_TOKEN env var)")
    parser.add_argument("--list", action="store_true",
                        help="List all models without downloading")
    parser.add_argument("--skip-optional", action="store_true",
                        help="Skip optional models (voice auth, file embedder)")
    parser.add_argument("--brain-only", action="store_true",
                        help="Download only the LLM brain model")
    parser.add_argument("--brain", choices=[str(i) for i in range(3)],
                        default="0",
                        help="Brain model choice: 0=Llama3.2-3B, 1=Phi-3-mini, 2=Qwen2.5-7B")
    args = parser.parse_args()

    # Apply brain model choice
    if args.brain:
        MODELS["brain"]["default_option"] = int(args.brain)

    dl = ModelDownloader(args.models_dir, args.token)

    if args.list:
        dl.list_models()
        return

    dl.list_models()
    print(f"\nStarting download to: {args.models_dir}")
    print("This may take a while depending on your internet connection.\n")

    dl.download_all(
        skip_optional = args.skip_optional,
        brain_only    = args.brain_only,
    )

    print("\nAll done! Next steps:")
    print("  1. make iso")
    print(f"  2. sudo DISK=/dev/sdX ./scripts/install.sh --models-dir {args.models_dir}")
    print("  3. Boot from the disk and say: 'Hey JARVIS'")


if __name__ == "__main__":
    main()
