# JARVIS AI OS

**Just A Rather Very Intelligent System — AI-Native Operating System**

A bare-metal operating system where an AI model IS the operating system.
Boots from disk like Windows. The kernel is written in C/C++/ASM.
Every user interaction goes through voice and LLM reasoning.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    USER VOICE / TEXT                        │
├─────────────────────────────────────────────────────────────┤
│  INTERACTION LAYER (Python)                                 │
│  Voice Command Pipeline │ UI Shell │ Context Memory         │
├─────────────────────────────────────────────────────────────┤
│  AI CORE LAYER (C++)                                        │
│  LLM Brain │ Speech Engine │ Security AI                    │
├─────────────────────────────────────────────────────────────┤
│  SYSTEM SERVICES (C++ / Python)                             │
│  Task Executor │ AI Filesystem │ Process Manager │ Network  │
├─────────────────────────────────────────────────────────────┤
│  JARVIS KERNEL (C)                                          │
│  AI Scheduler │ Memory Manager │ IPC Bus │ VFS │ Syscalls   │
├─────────────────────────────────────────────────────────────┤
│  BOOT LAYER (C / x86_64 ASM)                               │
│  UEFI Bootloader │ Stage 2 │ Long Mode Init                 │
├─────────────────────────────────────────────────────────────┤
│  HARDWARE                                                   │
│  CPU │ GPU │ NVMe │ RAM │ Mic/Speaker │ Network             │
└─────────────────────────────────────────────────────────────┘
```

---

## File Structure

```
jarvis-os/
├── boot/
│   ├── boot.asm          # Stage 1: x86_64 MBR bootloader (ASM)
│   └── stage2.asm        # Stage 2: long mode entry (ASM)
│
├── kernel/
│   ├── kernel.c          # Kernel entry, scheduler, IPC, memory, daemons
│   ├── kernel.h          # All kernel types and extern declarations
│   ├── linker.ld         # Higher-half kernel linker script
│   ├── pmm.c             # Physical memory manager (bitmap allocator)
│   ├── vmm.c             # Virtual memory manager (4-level paging)
│   ├── idt.c             # Interrupt descriptor table
│   ├── scheduler.c       # AI-priority-aware process scheduler
│   ├── ipc.c             # Lock-free ring buffer IPC bus
│   ├── vfs.c             # Virtual file system
│   └── syscall.c         # Syscall handler + security hook
│
├── ai_core/
│   ├── runtime.cpp       # LLM inference engine (C++20)
│   └── runtime.hpp       # AI runtime header
│
├── speech/
│   ├── speech_engine.cpp # STT (Whisper) + TTS (Kokoro) pipeline
│   └── speech_engine.hpp
│
├── security/
│   ├── security.cpp      # Anomaly detection, syscall audit, disk encryption
│   └── security.hpp
│
├── drivers/
│   ├── pci.c             # PCI bus enumeration
│   ├── ahci.c            # SATA/NVMe disk driver
│   ├── nic.c             # Network interface driver
│   ├── audio.c           # ALSA-compatible audio driver
│   ├── gpu.c             # GPU framebuffer driver
│   └── usb.c             # USB HID driver
│
├── services/
│   ├── ai_filesystem.py  # Semantic file search + auto-organisation
│   └── task_executor.py  # Voice command pipeline + task runner
│
├── configs/
│   └── grub.cfg          # GRUB2 boot menu
│
├── docs/
│   └── README.md         # This file
│
├── Makefile              # Build system
└── vendor/               # Place llama.cpp, whisper.cpp, onnxruntime here
```

---

## Model Slots — Where to Drop Your HuggingFace Models

| Slot | Path | Recommended Models |
|------|------|--------------------|
| `LLM_BRAIN` | `/jarvis/models/brain/` | `Llama-3.2-3B-Instruct`, `Qwen2.5-7B`, `Phi-3-mini`, `Mistral-7B-Instruct` |
| `SPEECH_STT` | `/jarvis/models/stt/` | `openai/whisper-medium.en` → `ggml-medium.en.bin` |
| `SPEECH_TTS` | `/jarvis/models/tts/` | `hexgrad/Kokoro-82M` → `kokoro-v1.0.onnx` |
| `WAKE_WORD` | `/jarvis/models/wake/` | `dscripka/openWakeWord` → `wake_word.onnx` |
| `SECURITY_ANOMALY` | `/jarvis/models/security/` | Custom ONNX autoencoder (train on your syscall traces) |
| `VOICE_AUTH` | `/jarvis/models/security/` | `speechbrain/spkrec-ecapa-voxceleb` |
| `FILE_EMBEDDER` | `/jarvis/models/filesystem/embedder/` | `sentence-transformers/all-MiniLM-L6-v2` |
| `FILE_CLASSIFIER` | `/jarvis/models/filesystem/classifier/` | `facebook/bart-large-mnli` |

### Quick model download

```bash
# Install HuggingFace CLI
pip install huggingface_hub

# LLM Brain (GGUF format — recommended)
huggingface-cli download \
    bartowski/Llama-3.2-3B-Instruct-GGUF \
    --include "Llama-3.2-3B-Instruct-Q4_K_M.gguf" \
    --local-dir /jarvis/models/brain/
mv /jarvis/models/brain/*.gguf /jarvis/models/brain/model.gguf

# Whisper STT
huggingface-cli download \
    ggerganov/whisper.cpp \
    --include "ggml-medium.en.bin" \
    --local-dir /jarvis/models/stt/

# Kokoro TTS
huggingface-cli download \
    hexgrad/Kokoro-82M \
    --local-dir /jarvis/models/tts/

# Speaker embedding (voice auth)
huggingface-cli download \
    speechbrain/spkrec-ecapa-voxceleb \
    --local-dir /jarvis/models/security/

# Sentence embedder (file search)
huggingface-cli download \
    sentence-transformers/all-MiniLM-L6-v2 \
    --local-dir /jarvis/models/filesystem/embedder/
```

---

## Build Requirements

```bash
# Debian/Ubuntu
sudo apt-get install \
    nasm gcc g++ make \
    grub-common grub-pc-bin grub-efi-amd64-bin \
    xorriso mtools qemu-system-x86

# Python dependencies (for services)
pip install \
    sentence-transformers \
    transformers \
    torch \
    onnxruntime \
    huggingface_hub

# Vendor libraries (build from source)
git clone https://github.com/ggerganov/llama.cpp   vendor/llama.cpp
git clone https://github.com/ggerganov/whisper.cpp  vendor/whisper.cpp
```

---

## Build + Run

```bash
# Build everything
make all

# Create bootable ISO
make iso

# Test in QEMU (8 GB RAM, 4 cores, KVM acceleration)
make qemu

# Flash to USB drive (replace /dev/sdX with your device!)
sudo DISK=/dev/sdX make install
```

---

## Hardware Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| CPU | x86_64, 4 cores | 8+ cores with AVX2 |
| RAM | 8 GB | 32 GB (for 7B model fully in RAM) |
| GPU | None (CPU inference) | NVIDIA GPU with 8 GB+ VRAM |
| Storage | 20 GB NVMe | 100 GB+ (models are 2–15 GB each) |
| Audio | Any mic + speaker | USB headset or dedicated audio card |
| Network | Optional | Required for package install, threat intel |

---

## Voice Commands (built-in fast routes)

| Say | Action |
|-----|--------|
| "What time is it?" | Speaks current time |
| "Set volume to 60" | Sets system volume |
| "Open [filename]" | Opens file with default app |
| "Play [media]" | Launches media player |
| "Install [package]" | Installs via apt/pip/flatpak |
| "Find my [description]" | Semantic file search |
| "Shut down" | Shutdown with confirmation |
| "Take a screenshot" | Saves screenshot to /jarvis/screenshots/ |
| Anything else → | Routes to LLM brain for reasoning |

---

## Security Model

- **Disk**: AES-256-XTS encryption, key derived from voiceprint + TPM
- **Auth**: Voice biometric (ECAPA-TDNN speaker verification)
- **Syscalls**: Every syscall audited, anomaly model scores sequences
- **Network**: Per-packet header analysis, IP blocklist
- **Processes**: Sandboxed via seccomp + Linux namespaces
- **AI safety**: Tool allow-list; destructive commands require voice confirmation

---

## Language Breakdown

| File | Language | Why |
|------|----------|-----|
| `boot/*.asm` | x86_64 ASM | Direct hardware access, bootloader constraints |
| `kernel/*.c` | C17 | Ring 0, no runtime, full hardware control |
| `ai_core/*.cpp` | C++20 | Zero-overhead abstractions, RAII for model memory |
| `speech/*.cpp` | C++20 | Real-time audio processing, low latency |
| `security/*.cpp` | C++20 | Performance-critical syscall hooks |
| `services/*.py` | Python 3.11 | HuggingFace ecosystem, rapid AI service dev |

---

## Adding a New AI Model

1. Download model from HuggingFace to `/jarvis/models/<name>/`
2. Find the `[MODEL_SLOT:<NAME>]` comment in the relevant `.cpp` file
3. Replace the stub loader with real ONNX/llama.cpp/torch load code
4. Add model path to `configs/grub.cfg` kernel parameters if needed
5. Rebuild: `make all && make iso`

---

## License

MIT — build your AI OS, sell it, modify it, do whatever you want.
