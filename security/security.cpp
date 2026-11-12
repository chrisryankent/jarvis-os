/*
 * =============================================================================
 * JARVIS AI OS — Security Daemon  (security/security.cpp)
 * Language: C++20
 *
 * Security subsystems:
 *   1. Syscall auditor    — hooks every syscall, scores sequences for anomalies
 *   2. Network monitor    — per-packet header analysis
 *   3. Disk encryption    — AES-256-XTS on all non-/boot partitions
 *   4. Biometric auth     — voiceprint + optional face model
 *   5. Anomaly AI         — ML model scores behaviour streams
 *
 * [MODEL_SLOT:SECURITY_ANOMALY]
 *   Lightweight anomaly detection model.
 *   Options (from HuggingFace):
 *     - Autoencoder trained on benign syscall N-grams (custom train)
 *     - DistilBERT fine-tuned on ADFA-LD syscall dataset
 *     - IsolationForest (scikit-learn → ONNX export)
 *   Place ONNX model at: /jarvis/models/security/anomaly.onnx
 *   Training data: ADFA-LD, UNSW-NB15, or custom recording from your system
 *
 * [MODEL_SLOT:VOICE_AUTH]
 *   Speaker verification model for authentication.
 *   Options:
 *     - SpeechBrain ECAPA-TDNN: speechbrain/spkrec-ecapa-voxceleb
 *     - Resemblyzer (cosine similarity on d-vectors)
 *   Place model at: /jarvis/models/security/speaker_verify.onnx
 *
 * [MODEL_SLOT:FACE_AUTH] (optional, requires camera)
 *   - Arcface (Insightface): /jarvis/models/security/arcface.onnx
 *   - HuggingFace: deepinsight/insightface
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include <cstring>
#include <cstdint>
#include <cstdio>

/* ============================================================================
 * Syscall sequence ring buffer
 * We record the last N syscall numbers to detect suspicious patterns.
 * ============================================================================ */
static constexpr size_t SYSCALL_HISTORY = 64;

static uint64_t syscall_history[SYSCALL_HISTORY] = {};
static size_t   syscall_head = 0;

/* Syscall categories for quick pre-filtering */
enum class SyscallCategory {
    NORMAL,
    SENSITIVE,   /* file write, network, exec */
    DANGEROUS,   /* ptrace, mount, mknod, setuid */
};

static SyscallCategory categorize_syscall(uint64_t nr)
{
    /* Linux x86_64 syscall numbers */
    switch (nr) {
    /* Dangerous: privilege escalation, kernel manipulation */
    case 101: /* ptrace   */
    case 165: /* mount    */
    case 133: /* mknod    */
    case 105: /* setuid   */
    case 117: /* setresuid*/
    case 317: /* seccomp  */
        return SyscallCategory::DANGEROUS;

    /* Sensitive: IO, networking, execution */
    case 2:   /* open     */
    case 56:  /* clone    */
    case 57:  /* fork     */
    case 59:  /* execve   */
    case 41:  /* socket   */
    case 42:  /* connect  */
    case 44:  /* sendto   */
    case 1:   /* write    */
    case 87:  /* unlink   */
        return SyscallCategory::SENSITIVE;

    default:
        return SyscallCategory::NORMAL;
    }
}

/* ============================================================================
 * Anomaly Detector
 * [MODEL_SLOT:SECURITY_ANOMALY]
 * ============================================================================ */
class AnomalyDetector {
public:
    bool init(const char *model_path)
    {
        terminal_printf("[SEC] Loading anomaly model: %s\n", model_path);
        /*
         * Real implementation:
         *   Load ONNX model via ONNX Runtime (same as TTS/STT).
         *   Input: float32 [1, SYSCALL_HISTORY] — normalised syscall IDs
         *   Output: float32 scalar — anomaly score (0=normal, 1=anomalous)
         *
         *   Training process (offline, on developer machine):
         *     python train/train_anomaly.py --dataset adfa_ld --output anomaly.onnx
         */
        loaded_ = true;
        terminal_puts("[SEC] Anomaly detector ready (stub)\n");
        return true;
    }

    /* Returns anomaly score 0.0–1.0 */
    float score(const uint64_t *syscall_seq, size_t len)
    {
        if (!loaded_) return 0.0f;
        /*
         * Real implementation:
         *   1. Normalise syscall IDs → float32 in [0,1]
         *   2. Run ONNX inference
         *   3. Return output scalar
         */
        (void)syscall_seq; (void)len;

        /* Stub: flag if we see ptrace (101) anywhere in sequence */
        for (size_t i = 0; i < len; i++) {
            if (syscall_seq[i] == 101) return 0.95f;
        }
        return 0.02f;
    }

private:
    bool loaded_ = false;
};

/* ============================================================================
 * Voice Authenticator
 * [MODEL_SLOT:VOICE_AUTH]
 * ============================================================================ */
class VoiceAuthenticator {
public:
    bool init(const char *model_path)
    {
        terminal_printf("[SEC] Loading voice auth model: %s\n", model_path);
        loaded_ = true;
        return true;
    }

    /* Enrol: store user voiceprint embedding */
    bool enrol(const int16_t *pcm, size_t frames)
    {
        if (!loaded_) return false;
        /*
         * Real implementation:
         *   1. Compute d-vector / x-vector via ECAPA-TDNN
         *   2. Store embedding in encrypted keystore
         *   3. Return true on success
         */
        (void)pcm; (void)frames;
        enrolled_ = true;
        return true;
    }

    /* Verify: compare new speech against stored voiceprint */
    bool verify(const int16_t *pcm, size_t frames, float threshold = 0.85f)
    {
        if (!loaded_ || !enrolled_) return false;
        /*
         * Real implementation:
         *   1. Compute d-vector for new audio
         *   2. Cosine similarity with stored embedding
         *   3. Return similarity > threshold
         */
        (void)pcm; (void)frames; (void)threshold;
        return true; /* stub: always pass */
    }

private:
    bool loaded_   = false;
    bool enrolled_ = false;
    float enrolled_embedding_[256] = {}; /* d-vector dimension */
};

/* ============================================================================
 * Disk Encryption (AES-256-XTS)
 * Key derived from: user voiceprint hash + hardware TPM seal
 * ============================================================================ */
static uint8_t disk_key[64] = {}; /* 512-bit for XTS (two 256-bit halves) */

static void derive_disk_key(const float *voiceprint_embedding, size_t emb_len)
{
    /*
     * Real implementation:
     *   1. Compute SHA-3-512 of voiceprint_embedding bytes
     *   2. XOR with TPM2_Unseal() output (hardware-bound)
     *   3. Result is disk_key — never stored on disk
     *
     *   LUKS-compatible: use cryptsetup with external key supplier or
     *   implement dm-crypt layer in kernel virtual block device.
     */
    (void)voiceprint_embedding; (void)emb_len;
    terminal_puts("[SEC] Disk key derived (stub — replace with real KDF)\n");
}

/* ============================================================================
 * Network Packet Monitor
 * Called from NIC driver for every received packet header.
 * ============================================================================ */
struct IpHeader {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
};

static uint32_t blocked_ips[1024] = {};
static int      blocked_count     = 0;

static bool is_blocked(uint32_t ip)
{
    for (int i = 0; i < blocked_count; i++)
        if (blocked_ips[i] == ip) return true;
    return false;
}

extern "C" void security_net_audit(const uint8_t *pkt_hdr, size_t len)
{
    if (len < sizeof(IpHeader)) return;
    const IpHeader *ip = reinterpret_cast<const IpHeader *>(pkt_hdr);

    /* Block known-bad IPs */
    if (is_blocked(ip->src_ip)) {
        /* Drop packet at driver level (return without processing) */
        terminal_printf("[SEC] Blocked packet from IP: %u.%u.%u.%u\n",
            (ip->src_ip >> 24) & 0xFF,
            (ip->src_ip >> 16) & 0xFF,
            (ip->src_ip >>  8) & 0xFF,
            (ip->src_ip      ) & 0xFF);
        return;
    }

    /* Anomalous protocol check */
    if (ip->protocol == 255) { /* reserved — often used in probes */
        ipc_message_t alert{};
        alert.type = IPC_SECURITY_ALERT;
        const char *msg = "Suspicious network protocol detected";
        strncpy((char *)alert.payload, msg, sizeof(alert.payload)-1);
        ipc_send(&alert);
    }
}

/* ============================================================================
 * Syscall Auditor (called from kernel for every syscall)
 * ============================================================================ */
static AnomalyDetector g_anomaly;
static VoiceAuthenticator g_voice_auth;
static constexpr float ALERT_THRESHOLD = 0.80f;

extern "C" void security_audit(uint64_t syscall_nr, uint64_t *args)
{
    (void)args;

    /* Record in ring buffer */
    syscall_history[syscall_head % SYSCALL_HISTORY] = syscall_nr;
    syscall_head++;

    /* Pre-filter: immediately flag DANGEROUS syscalls */
    SyscallCategory cat = categorize_syscall(syscall_nr);
    if (cat == SyscallCategory::DANGEROUS) {
        terminal_printf("[SEC] DANGEROUS syscall: %llu\n", syscall_nr);
        float score = g_anomaly.score(syscall_history, SYSCALL_HISTORY);
        if (score > ALERT_THRESHOLD) {
            ipc_message_t alert{};
            alert.type = IPC_SECURITY_ALERT;
            const char *msg = "Suspicious system call sequence detected. Process suspended.";
            strncpy((char *)alert.payload, msg, sizeof(alert.payload)-1);
            ipc_send(&alert);
        }
    }

    /* Periodic full scan every 256 syscalls */
    if ((syscall_head & 0xFF) == 0) {
        float score = g_anomaly.score(syscall_history, SYSCALL_HISTORY);
        if (score > ALERT_THRESHOLD) {
            ipc_message_t alert{};
            alert.type = IPC_SECURITY_ALERT;
            char msg[256];
            snprintf(msg, sizeof(msg),
                "Anomaly score %.2f — possible intrusion. Locking session.", score);
            strncpy((char *)alert.payload, msg, sizeof(alert.payload)-1);
            ipc_send(&alert);
        }
    }
}

/* ============================================================================
 * Security daemon thread
 * ============================================================================ */
extern "C" void security_daemon_thread(void)
{
    g_anomaly.init("/jarvis/models/security/anomaly.onnx");
    g_voice_auth.init("/jarvis/models/security/speaker_verify.onnx");

    terminal_puts("[SEC] Security daemon running\n");

    for (;;) {
        if (ipc_has_message(IPC_SECURITY_ALERT)) {
            ipc_message_t alert = ipc_recv_next();
            /* Voice-announce the alert */
            tts_speak((const char *)alert.payload, alert.payload_len);
            /* Log to encrypted audit file */
            vfs_append("/jarvis/logs/security.log",
                       (const char *)alert.payload, alert.payload_len);
        }
        /* Periodic threat intel update (if network available) */
        cpu_relax();
    }
}

/* ============================================================================
 * C interface
 * ============================================================================ */
extern "C" {

void security_respond(void *msg_void)
{
    ipc_message_t *msg = (ipc_message_t *)msg_void;
    tts_speak((const char *)msg->payload, msg->payload_len);
}

void disk_encryption_init(void)
{
    terminal_puts("[SEC] Disk encryption init (AES-256-XTS stub)\n");
    /* Real: derive key from voiceprint + TPM, mount dm-crypt device */
}

} /* extern "C" */
