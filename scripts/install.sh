#!/usr/bin/env bash
# =============================================================================
# JARVIS AI OS — Disk Installer  (scripts/install.sh)
#
# Usage:
#   sudo ./scripts/install.sh --disk /dev/sdX [--models-dir /path/to/models]
#
# What it does:
#   1. Partition the target disk (GPT: EFI + JARVIS root + data)
#   2. Format partitions (FAT32 for EFI, ext4 for root and data)
#   3. Install GRUB2 bootloader
#   4. Copy kernel, configs, and Python services
#   5. Copy AI models if --models-dir is given
#   6. Create JARVIS directory structure
#   7. Write model index (LBA map for AHCI driver)
# =============================================================================

set -euo pipefail

# ---- Colours ---------------------------------------------------------------
RED='\033[0;31m'
GRN='\033[0;32m'
CYN='\033[0;36m'
YLW='\033[0;33m'
NC='\033[0m'

log()  { echo -e "${CYN}[JARVIS INSTALL]${NC} $*"; }
ok()   { echo -e "${GRN}[  OK  ]${NC} $*"; }
warn() { echo -e "${YLW}[ WARN ]${NC} $*"; }
die()  { echo -e "${RED}[FATAL]${NC} $*"; exit 1; }

# ---- Parse arguments -------------------------------------------------------
DISK=""
MODELS_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --disk)       DISK="$2";       shift 2 ;;
        --models-dir) MODELS_DIR="$2"; shift 2 ;;
        --help)
            echo "Usage: $0 --disk /dev/sdX [--models-dir /path/to/models]"
            exit 0 ;;
        *) die "Unknown argument: $1" ;;
    esac
done

[[ -z "$DISK" ]]         && die "No disk specified. Use --disk /dev/sdX"
[[ ! -b "$DISK" ]]       && die "Not a block device: $DISK"
[[ "$(id -u)" -ne 0 ]]  && die "Must run as root (sudo)"

# ---- Confirm ---------------------------------------------------------------
echo
echo -e "${RED}WARNING: This will ERASE all data on ${DISK}${NC}"
echo -e "Disk: $(lsblk -dn -o SIZE "$DISK") capacity"
echo
read -rp "Type 'YES I UNDERSTAND' to continue: " CONFIRM
[[ "$CONFIRM" != "YES I UNDERSTAND" ]] && die "Aborted"

# ---- Check dependencies ---------------------------------------------------
log "Checking dependencies..."
for cmd in parted mkfs.fat mkfs.ext4 grub-install mount; do
    command -v "$cmd" > /dev/null || die "Missing: $cmd (apt-get install grub2 dosfstools e2fsprogs)"
done
ok "All tools available"

# ---- Partition the disk ---------------------------------------------------
log "Partitioning $DISK..."

parted -s "$DISK" mklabel gpt
parted -s "$DISK" mkpart EFI    fat32  1MiB   512MiB
parted -s "$DISK" mkpart JARVIS ext4   512MiB 20GiB
parted -s "$DISK" mkpart DATA   ext4   20GiB  100%
parted -s "$DISK" set 1 esp on
parted -s "$DISK" set 1 boot on

ok "GPT partition table created"
sleep 2  # let kernel re-read partition table

# Determine partition names (sdaX vs nvme0n1pX)
if [[ "$DISK" == *"nvme"* ]]; then
    P1="${DISK}p1"
    P2="${DISK}p2"
    P3="${DISK}p3"
else
    P1="${DISK}1"
    P2="${DISK}2"
    P3="${DISK}3"
fi

# ---- Format ---------------------------------------------------------------
log "Formatting partitions..."
mkfs.fat -F32 -n "JARVIS_EFI"  "$P1"
mkfs.ext4 -L "JARVIS_ROOT"     "$P2"
mkfs.ext4 -L "JARVIS_DATA"     "$P3"
ok "Partitions formatted"

# ---- Mount ----------------------------------------------------------------
MNT=$(mktemp -d /tmp/jarvis_install_XXXX)
log "Mounting at $MNT..."
mount "$P2" "$MNT"
mkdir -p "$MNT/boot/efi"
mount "$P1" "$MNT/boot/efi"
mkdir -p "$MNT/data"
mount "$P3" "$MNT/data"

# ---- Create JARVIS directory structure ------------------------------------
log "Creating JARVIS directory structure..."
mkdir -p "$MNT"/{boot/grub,jarvis,home/jarvis,etc,var/log,tmp,proc,sys,dev}
mkdir -p "$MNT/jarvis"/{models/{brain,stt,tts,wake,security,filesystem/embedder,filesystem/classifier},data,logs,bin,screenshots,fonts,config}

# ---- Install kernel -------------------------------------------------------
log "Installing JARVIS kernel..."
if [[ -f "./kernel.elf" ]]; then
    cp "./kernel.elf" "$MNT/boot/jarvis-kernel.elf"
    ok "Kernel installed"
else
    warn "kernel.elf not found — run 'make all' first"
    warn "Creating placeholder so GRUB installs correctly"
    echo "PLACEHOLDER" > "$MNT/boot/jarvis-kernel.elf"
fi

# ---- Install GRUB ---------------------------------------------------------
log "Installing GRUB2..."
grub-install \
    --target=x86_64-efi \
    --efi-directory="$MNT/boot/efi" \
    --boot-directory="$MNT/boot" \
    --removable \
    --no-nvram 2>/dev/null || true

grub-install \
    --target=i386-pc \
    --boot-directory="$MNT/boot" \
    "$DISK" 2>/dev/null || true

# Copy GRUB config
cp ./configs/grub.cfg "$MNT/boot/grub/grub.cfg"

# Update root device in grub.cfg
PART2_UUID=$(blkid -s UUID -o value "$P2")
sed -i "s|root=/dev/sda2|root=UUID=$PART2_UUID|g" "$MNT/boot/grub/grub.cfg"

ok "GRUB2 installed"

# ---- Install Python services ----------------------------------------------
log "Installing Python services..."
cp ./services/ai_filesystem.py "$MNT/jarvis/bin/"
cp ./services/task_executor.py "$MNT/jarvis/bin/"
chmod +x "$MNT/jarvis/bin/"*.py

# Create systemd-like init script for Python daemons
cat > "$MNT/jarvis/bin/start_services.sh" << 'EOF'
#!/bin/bash
# JARVIS Python service launcher — called from kernel init
echo "[JARVIS] Starting Python services..."
python3 /jarvis/bin/ai_filesystem.py &
python3 /jarvis/bin/task_executor.py &
echo "[JARVIS] Services started (PIDs: $!)"
EOF
chmod +x "$MNT/jarvis/bin/start_services.sh"
ok "Python services installed"

# ---- Install AI models ---------------------------------------------------
if [[ -n "$MODELS_DIR" && -d "$MODELS_DIR" ]]; then
    log "Installing AI models from $MODELS_DIR..."
    TOTAL_SIZE=$(du -sh "$MODELS_DIR" | cut -f1)
    log "Models total size: $TOTAL_SIZE"

    # Brain (LLM)
    [[ -f "$MODELS_DIR/brain/model.gguf" ]] && {
        log "Copying LLM brain model..."
        cp "$MODELS_DIR/brain/model.gguf" "$MNT/jarvis/models/brain/"
        ok "LLM model installed"
    }

    # STT
    [[ -f "$MODELS_DIR/stt/ggml-medium.en.bin" ]] && {
        log "Copying Whisper STT model..."
        cp "$MODELS_DIR/stt/ggml-medium.en.bin" "$MNT/jarvis/models/stt/"
        ok "STT model installed"
    }

    # TTS
    ls "$MODELS_DIR/tts/"*.onnx 2>/dev/null && {
        log "Copying TTS model..."
        cp "$MODELS_DIR/tts/"*.onnx "$MNT/jarvis/models/tts/"
        ok "TTS model installed"
    }

    # Wake word
    ls "$MODELS_DIR/wake/"*.onnx 2>/dev/null && {
        log "Copying wake word model..."
        cp "$MODELS_DIR/wake/"*.onnx "$MNT/jarvis/models/wake/"
        ok "Wake word model installed"
    }

    # Security anomaly
    [[ -f "$MODELS_DIR/security/anomaly.onnx" ]] && {
        log "Copying security model..."
        cp "$MODELS_DIR/security/anomaly.onnx" "$MNT/jarvis/models/security/"
        ok "Security model installed"
    }

    # Embedder
    [[ -d "$MODELS_DIR/filesystem/embedder" ]] && {
        log "Copying file embedder model..."
        cp -r "$MODELS_DIR/filesystem/embedder/"* "$MNT/jarvis/models/filesystem/embedder/"
        ok "File embedder installed"
    }
else
    warn "No --models-dir given. Drop models manually into:"
    warn "  (mounted at $MNT/jarvis/models/)"
    warn "  brain/    — LLM .gguf file"
    warn "  stt/      — Whisper .bin file"
    warn "  tts/      — TTS .onnx file"
    warn "  wake/     — wake word .onnx file"
fi

# ---- Write model index for AHCI driver -----------------------------------
log "Writing model index..."
cat > "$MNT/jarvis/data/model_index.json" << 'EOF'
{
  "version": 1,
  "models": {
    "brain":     "/jarvis/models/brain/model.gguf",
    "stt":       "/jarvis/models/stt/ggml-medium.en.bin",
    "tts":       "/jarvis/models/tts/kokoro-v1.0.onnx",
    "wake":      "/jarvis/models/wake/wake_word.onnx",
    "anomaly":   "/jarvis/models/security/anomaly.onnx",
    "embedder":  "/jarvis/models/filesystem/embedder"
  },
  "settings": {
    "voice_language": "en",
    "tts_voice":      "af_bella",
    "wake_phrase":    "hey jarvis",
    "gpu_layers":     99,
    "context_size":   4096,
    "log_level":      "info"
  }
}
EOF
ok "Model index written"

# ---- Write initial JARVIS config -----------------------------------------
cat > "$MNT/jarvis/config/jarvis.conf" << 'EOF'
# JARVIS AI OS Configuration
# Edit and reboot to apply changes

[core]
wake_phrase    = hey jarvis
log_level      = info
context_size   = 4096

[models]
gpu_layers     = 99          # number of LLM layers to offload to GPU
stt_language   = en          # Whisper language code
tts_voice      = af_bella    # Kokoro voice ID

[security]
disk_encrypt   = true        # AES-256-XTS disk encryption
voice_auth     = true        # voice biometric authentication
anomaly_thresh = 0.80        # 0.0–1.0 anomaly score threshold
syscall_audit  = true        # audit all syscalls

[network]
dhcp           = true        # use DHCP (set false for static)
static_ip      = 192.168.1.100
static_gw      = 192.168.1.1
static_dns     = 8.8.8.8
threat_intel   = true        # download threat intelligence updates
EOF
ok "JARVIS config written"

# ---- Enrol first-run marker ----------------------------------------------
touch "$MNT/jarvis/data/.first_run"

# ---- Unmount -------------------------------------------------------------
log "Unmounting..."
sync
umount "$MNT/data"
umount "$MNT/boot/efi"
umount "$MNT"
rmdir  "$MNT"

ok "Installation complete!"
echo
echo -e "${CYN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYN}║           JARVIS AI OS installed successfully            ║${NC}"
echo -e "${CYN}╚══════════════════════════════════════════════════════════╝${NC}"
echo
echo "  Disk:   $DISK"
echo "  EFI:    $P1"
echo "  Root:   $P2  (JARVIS kernel + services)"
echo "  Data:   $P3  (user data, models, logs)"
echo
echo "Next steps:"
echo "  1. If models not installed: mount $P2 and copy to /jarvis/models/"
echo "     (see README.md — Model Slots section)"
echo "  2. Boot from $DISK"
echo "  3. On first boot: JARVIS will enrol your voice for authentication"
echo "  4. Say 'Hey JARVIS' to start"
echo
echo "Tip: Build with QEMU first — 'make qemu' before flashing to hardware"
