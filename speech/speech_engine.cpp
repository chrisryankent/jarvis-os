/*
 * =============================================================================
 * JARVIS AI OS — Speech Engine  (speech/speech_engine.cpp)
 * Language: C++20
 *
 * Voice pipeline (input):
 *   Microphone PCM → Ring buffer → Wake Word → Whisper STT → IPC
 *
 * Voice pipeline (output):
 *   IPC TTS request → TTS model → PCM → Speaker
 *
 * [MODEL_SLOT:WAKE_WORD]
 *   - openWakeWord (ONNX): place model in /jarvis/models/wake/wake_word.onnx
 *   - Wake phrase: "Hey JARVIS" (default) — retrain for custom phrases
 *   - HuggingFace: dscripka/openWakeWord
 *
 * [MODEL_SLOT:SPEECH_STT]
 *   - Whisper (GGUF/ONNX): /jarvis/models/stt/ggml-medium.en.bin
 *   - HuggingFace: openai/whisper-medium.en  (or faster-whisper for RTF < 0.1)
 *   - Runs on CPU or GPU. GPU preferred for latency.
 *
 * [MODEL_SLOT:SPEECH_TTS]
 *   - Kokoro (lightweight, 82M params): /jarvis/models/tts/kokoro-v1.0.onnx
 *   - HuggingFace: hexgrad/Kokoro-82M
 *   - Alternative: Coqui XTTS-v2, StyleTTS2, Bark
 *   - Output: 24 kHz mono PCM → ALSA playback
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include <cstring>
#include <cstdint>

/* ---- Conditional AI runtime headers ----------------------------------------
 * Define JARVIS_ONNX_LINKED when linking -lonnxruntime.
 * Define JARVIS_WHISPER_LINKED when linking tools/vendor/whisper.cpp/build/src/libwhisper.a
 * These guards turn the real inference paths from stub stubs to live code.
 * ---------------------------------------------------------------------------- */
#ifdef JARVIS_ONNX_LINKED
#  include <onnxruntime_c_api.h>
#endif
#ifdef JARVIS_WHISPER_LINKED
#  include "whisper.h"
#endif

static double sqrt_approx(double x)
{
    if (x <= 0.0)
        return 0.0;
    double guess = x;
    for (int i = 0; i < 16; i++)
        guess = 0.5 * (guess + x / guess);
    return guess;
}

/* ============================================================================
 * Constants
 * ============================================================================ */
static constexpr int    SAMPLE_RATE       = 16000;   /* Hz — Whisper native    */
static constexpr int    CHANNELS          = 1;        /* mono                   */
static constexpr int    BITS_PER_SAMPLE   = 16;
static constexpr int    CHUNK_FRAMES      = 1600;     /* 100 ms chunks          */
static constexpr int    VAD_SILENCE_FRAMES= 14;       /* 1.4 s silence → end seg */
static constexpr float  VAD_ENERGY_THRESH = 0.025f;   /* RMS threshold           */
static constexpr int    WAKE_REQUIRED_CHUNKS = 6;     /* 600 ms above threshold  */
static constexpr int    TTS_ECHO_SUPPRESS_CHUNKS = 80;/* 8 s max after speaking  */
static constexpr int    TTS_ECHO_MIN_CHUNKS = 14;     /* 1.4 s min after speaking */
static constexpr size_t MAX_SEGMENT_FRAMES= SAMPLE_RATE * 30; /* 30 s max       */

/* Forward declarations */
static bool             g_tts_is_playing = false;
static int              tts_echo_suppress_chunks = 0;

/* ============================================================================
 * Voice Activity Detection (enhanced with TTS echo suppression)
 * Simple RMS check + echo suppression during TTS playback
 * ============================================================================ */
static bool vad_is_speech(const int16_t *pcm, size_t frames)
{
    if (g_tts_is_playing || tts_echo_suppress_chunks > 0)
        return false;

    double sum = 0.0;
    int32_t peak = 0;
    int crossings = 0;
    int16_t prev = 0;

    for (size_t i = 0; i < frames; i++) {
        int32_t sample = pcm[i];
        int32_t mag = sample < 0 ? -sample : sample;
        if (mag > peak)
            peak = mag;
        if (i > 0 && ((sample < 0 && prev >= 0) || (sample >= 0 && prev < 0)))
            crossings++;
        prev = pcm[i];

        double s = sample / 32768.0;
        sum += s * s;
    }

    float rms = (float)sqrt_approx(sum / frames);
    int crossing_percent = (int)((crossings * 100) / (frames ? frames : 1));

    if (rms < VAD_ENERGY_THRESH)
        return false;
    if (peak < 1200)
        return false;
    if (crossing_percent < 2 || crossing_percent > 38)
        return false;

    return true;
}

static int estimate_tts_suppress_chunks(const char *text)
{
    size_t chars = text ? strlen(text) : 0;
    int chunks = TTS_ECHO_MIN_CHUNKS + (int)(chars / 8);

    if (chunks > TTS_ECHO_SUPPRESS_CHUNKS)
        chunks = TTS_ECHO_SUPPRESS_CHUNKS;
    return chunks;
}

/* ============================================================================
 * Wake Word Detector
 * [MODEL_SLOT:WAKE_WORD]
 * Wraps openWakeWord ONNX inference. The model runs continuously on short
 * 80 ms frames. When score > threshold, speech capture begins.
 * ============================================================================ */
class WakeWordDetector {
public:
    bool init(const char *model_path)
    {
#ifdef JARVIS_ONNX_LINKED
        /* ---- Real path: ONNX Runtime linked (link: -lonnxruntime) --------- */
        const OrtApi *ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "jarvis_wwd", &env_);
        OrtSessionOptions *opts = nullptr;
        ort->CreateSessionOptions(&opts);
        ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
        OrtStatus *st = ort->CreateSession(env_, model_path, opts, &session_);
        ort->ReleaseSessionOptions(opts);
        if (st != nullptr) {
            terminal_printf("[WAKE] ONNX session failed: %s\n", model_path);
            ort->ReleaseStatus(st);
            fallback_ = true;
        } else {
            ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info_);
            terminal_printf("[WAKE] ONNX wake model loaded: %s\n", model_path);
        }
        loaded_ = true;
        return true;
#else
        /* ---- Fallback: ONNX Runtime not linked ----------------------------
         * Rebuild with:  -DJARVIS_ONNX_LINKED  link: -lonnxruntime
         * Until then, energy-threshold wake detection is used. */
        if (vfs_stat(model_path) == 0)
            terminal_printf("[WAKE] Model present: %s (ONNX not linked)\n", model_path);
        else
            terminal_printf("[WAKE] Model missing: %s\n", model_path);
        terminal_puts("[WAKE] Energy-threshold wake fallback active\n");
        loaded_   = true;
        fallback_ = true;
        return true;
#endif
    }

    /* Returns confidence 0.0–1.0. Threshold typically 0.5 */
    float score(const int16_t *pcm, size_t frames)
    {
        if (!loaded_) return 0.0f;

#ifdef JARVIS_ONNX_LINKED
        if (!fallback_ && session_ && mem_info_) {
            /* ---- Real ONNX inference path --------------------------------
             * openWakeWord input: float32 log-mel [1, n_frames, 96]
             * Output: float32 confidence [1] */
            const OrtApi *ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
            size_t n_frames = frames / 160;
            if (n_frames < 1) n_frames = 1;
            size_t mel_sz = n_frames * 96;
            float *mel = (float *)kmalloc(mel_sz * sizeof(float));
            if (!mel) return 0.0f;
            /* Per-frame log-energy as stand-in for full mel filterbank.   */
            /* Replace with proper mel filterbank for production accuracy.  */
            for (size_t f = 0; f < n_frames; f++) {
                double e = 0.0;
                size_t s0 = f * 160, s1 = s0 + 160 < frames ? s0 + 160 : frames;
                for (size_t i = s0; i < s1; i++) {
                    double v = pcm[i] / 32768.0; e += v * v;
                }
                float log_e = (float)(e / 160.0);
                for (int b = 0; b < 96; b++) mel[f * 96 + b] = log_e;
            }
            int64_t shape[] = { 1, (int64_t)n_frames, 96 };
            OrtValue *in = nullptr, *out = nullptr;
            ort->CreateTensorWithDataAsOrtValue(
                mem_info_, mel, mel_sz * sizeof(float),
                shape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in);
            const char *in_names[]  = { "input_features" };
            const char *out_names[] = { "output" };
            ort->Run(session_, nullptr, in_names,
                     (const OrtValue *const *)&in, 1, out_names, 1, &out);
            float *data = nullptr;
            ort->GetTensorMutableData(out, (void **)&data);
            float conf = data ? data[0] : 0.0f;
            ort->ReleaseValue(in);
            ort->ReleaseValue(out);
            kfree(mel);
            return conf;
        }
#endif
        /* ---- Energy-threshold fallback ----------------------------------- */
        double sum = 0.0;
        for (size_t i = 0; i < frames; i++) {
            double s = pcm[i] / 32768.0;
            sum += s * s;
        }
        float rms = (float)sqrt_approx(sum / frames);
        return rms > 0.055f ? 0.65f : 0.0f;
    }

private:
    bool loaded_   = false;
    bool fallback_ = false;
#ifdef JARVIS_ONNX_LINKED
    OrtEnv        *env_      = nullptr;
    OrtSession    *session_  = nullptr;
    OrtMemoryInfo *mem_info_ = nullptr;
#endif
};

/* ============================================================================
 * Whisper STT Wrapper
 * [MODEL_SLOT:SPEECH_STT]
 * ============================================================================ */
class WhisperSTT {
public:
    bool init(const char *model_path)
    {
        terminal_printf("[STT] Whisper init: %s\n", model_path);

#ifdef JARVIS_WHISPER_LINKED
        /* ---- Real path: whisper.cpp linked --------------------------------
         * link: tools/vendor/whisper.cpp/build/src/libwhisper.a */
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = true;
        ctx_ = whisper_init_from_file_with_params(model_path, cparams);
        if (!ctx_) {
            terminal_printf("[STT] whisper_init failed: %s\n", model_path);
            loaded_ = false;
            runtime_ready_ = false;
            return false;
        }
        loaded_        = true;
        runtime_ready_ = true;
        terminal_puts("[STT] Whisper loaded — real transcription active\n");
        return true;
#else
        /* ---- Fallback: whisper.cpp not linked -----------------------------
         * Rebuild with:  -DJARVIS_WHISPER_LINKED
         * Link:          tools/vendor/whisper.cpp/build/src/libwhisper.a */
        if (vfs_stat(model_path) == 0)
            terminal_printf("[STT] Model present: %s (whisper.cpp not linked)\n", model_path);
        else
            terminal_printf("[STT] Model missing: %s\n", model_path);
        terminal_puts("[STT] Stub mode — no transcription until whisper.cpp is linked\n");
        loaded_        = true;
        runtime_ready_ = false;
        return true;
#endif
    }

    bool can_transcribe() const
    {
        return loaded_ && runtime_ready_;
    }

    /*
     * Transcribe PCM segment to text.
     * Input:  16-bit signed PCM at 16 kHz mono
     * Output: UTF-8 text in out_buf
     */
    bool transcribe(const int16_t *pcm, size_t frames,
                    char *out_buf, size_t out_max)
    {
        if (!loaded_) {
            strncpy(out_buf, "[STT not loaded]", out_max - 1);
            out_buf[out_max - 1] = '\0';
            return false;
        }
        if (!runtime_ready_) {
            strncpy(out_buf,
                "[whisper.cpp not linked — rebuild with -DJARVIS_WHISPER_LINKED]",
                out_max - 1);
            out_buf[out_max - 1] = '\0';
            return false;
        }

#ifdef JARVIS_WHISPER_LINKED
        /* ---- Real whisper.cpp inference ---------------------------------- */
        float *fpcm = (float *)kmalloc(frames * sizeof(float));
        if (!fpcm) return false;
        for (size_t i = 0; i < frames; i++)
            fpcm[i] = pcm[i] / 32768.0f;

        whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wp.language         = "en";
        wp.translate        = false;
        wp.n_threads        = 4;
        wp.temperature      = 0.0f;
        wp.no_context       = true;
        wp.single_segment   = false;
        wp.print_special    = false;
        wp.print_progress   = false;
        wp.print_realtime   = false;
        wp.print_timestamps = false;

        int rc = whisper_full(ctx_, wp, fpcm, (int)frames);
        kfree(fpcm);
        if (rc != 0) return false;

        int n_seg = whisper_full_n_segments(ctx_);
        size_t off = 0;
        for (int i = 0; i < n_seg && off < out_max - 1; i++) {
            const char *seg = whisper_full_get_segment_text(ctx_, i);
            size_t slen = strlen(seg);
            if (off + slen >= out_max - 1) slen = out_max - 1 - off;
            memcpy(out_buf + off, seg, slen);
            off += slen;
        }
        out_buf[off] = '\0';
        return off > 0;
#else
        return false;
#endif
    }

    void destroy()
    {
#ifdef JARVIS_WHISPER_LINKED
        if (ctx_) { whisper_free(ctx_); ctx_ = nullptr; }
#endif
    }

private:
    bool loaded_        = false;
    bool runtime_ready_ = false;
#ifdef JARVIS_WHISPER_LINKED
    whisper_context *ctx_ = nullptr;
#endif
};

/* ============================================================================
 * TTS Engine Wrapper
 * [MODEL_SLOT:SPEECH_TTS]
 * Uses Kokoro ONNX (82 M params, <100 ms latency on CPU)
 * ============================================================================ */
class TTSEngine {
public:
    bool init(const char *model_path)
    {
        terminal_printf("[TTS] Kokoro TTS init: %s\n", model_path);

#ifdef JARVIS_ONNX_LINKED
        /* ---- Real path: Kokoro ONNX (82 M params, 24 kHz output) ----------
         * Inputs:  input_ids  int64  [1, N_phonemes]                        
         *          style      f32    [1, 256]  (voice embedding)            
         *          speed      f32    scalar                                 
         * Output:  waveform   f32    [1, T] at 24 000 Hz                    
         * link: -lonnxruntime */
        const OrtApi *ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "jarvis_tts", &ort_env_);
        OrtSessionOptions *opts = nullptr;
        ort->CreateSessionOptions(&opts);
        OrtStatus *st = ort->CreateSession(ort_env_, model_path, opts, &ort_session_);
        ort->ReleaseSessionOptions(opts);
        if (st != nullptr) {
            ort->ReleaseStatus(st);
            terminal_puts("[TTS] Kokoro ONNX load failed — phoneme synth fallback\n");
        } else {
            ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info_);
            ort_ready_ = true;
            terminal_puts("[TTS] Kokoro ONNX loaded — real neural TTS active\n");
        }
#else
        /* ---- Fallback: Kokoro not linked ----------------------------------
         * Rebuild with:  -DJARVIS_ONNX_LINKED  link: -lonnxruntime         
         * Phoneme synth fallback will produce robotic but intelligible audio. */
        if (vfs_stat(model_path) == 0)
            terminal_printf("[TTS] Model present: %s (ONNX not linked)\n", model_path);
        else
            terminal_printf("[TTS] Model missing: %s\n", model_path);
        terminal_puts("[TTS] Phoneme synth fallback active\n");
#endif
        loaded_ = true;
        return true;
    }

    /*
     * Synthesise text → PCM float32 at 24 kHz.
     * Uses Kokoro ONNX when JARVIS_ONNX_LINKED is defined and model loaded.
     * Falls back to phoneme triangle-wave synth otherwise.
     * Returns number of samples written.
     */
    size_t synthesise(const char *text, float *pcm_out, size_t pcm_max)
    {
        if (!loaded_) return 0;

#ifdef JARVIS_ONNX_LINKED
        if (ort_ready_ && ort_session_ && mem_info_) {
            /* ---- Kokoro ONNX real synthesis -------------------------------- */
            const OrtApi *ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);

            /* Step 1: text → phoneme IDs (ASCII codepoints as stub G2P;      */
            /* replace espeak_text_to_phoneme_ids() for real phonemisation)   */
            int64_t ids[512] = {};
            size_t  n_ph = 0;
            for (size_t i = 0; i < 511 && text[i]; i++)
                ids[n_ph++] = (int64_t)(unsigned char)text[i];

            /* Step 2: build input tensors */
            float style[256] = {};   /* zero → default voice (af_heart) */
            float speed       = 1.0f;
            int64_t ids_sh[]  = { 1, (int64_t)n_ph };
            int64_t sty_sh[]  = { 1, 256 };
            int64_t spd_sh[]  = { 1 };
            OrtValue *t_ids = nullptr, *t_sty = nullptr, *t_spd = nullptr, *t_out = nullptr;
            OrtValue *inputs[3];
            ort->CreateTensorWithDataAsOrtValue(mem_info_, ids,  n_ph * sizeof(int64_t),
                ids_sh, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &t_ids);
            ort->CreateTensorWithDataAsOrtValue(mem_info_, style, sizeof(style),
                sty_sh, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &t_sty);
            ort->CreateTensorWithDataAsOrtValue(mem_info_, &speed, sizeof(speed),
                spd_sh, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &t_spd);
            inputs[0] = t_ids; inputs[1] = t_sty; inputs[2] = t_spd;

            const char *in_names[]  = { "input_ids", "style", "speed" };
            const char *out_names[] = { "waveform" };
            ort->Run(ort_session_, nullptr, in_names,
                     (const OrtValue *const *)inputs, 3, out_names, 1, &t_out);

            float *wave = nullptr;
            ort->GetTensorMutableData(t_out, (void **)&wave);
            OrtTensorTypeAndShapeInfo *si = nullptr;
            ort->GetTensorTypeAndShape(t_out, &si);
            size_t nd = 0; ort->GetDimensionsCount(si, &nd);
            int64_t dims[4] = {}; ort->GetDimensions(si, dims, nd);
            size_t samples = nd > 0 ? (size_t)dims[nd - 1] : 0;
            if (samples > pcm_max) samples = pcm_max;
            if (wave && samples > 0)
                memcpy(pcm_out, wave, samples * sizeof(float));
            ort->ReleaseTensorTypeAndShapeInfo(si);
            ort->ReleaseValue(t_ids); ort->ReleaseValue(t_sty);
            ort->ReleaseValue(t_spd); ort->ReleaseValue(t_out);
            return samples;
        }
#endif
        /* ---- Phoneme synth fallback (intelligible robotic speech) ----------
         * Active when JARVIS_ONNX_LINKED is not set, or Kokoro load failed.
         * Produces audio via triangle-wave formant synthesis. */
        size_t out = 0;
        uint32_t noise = 0x5A17C0DEu;

        emit_silence(pcm_out, pcm_max, &out, 50);
        for (size_t i = 0; text[i] != '\0' && out < pcm_max; i++) {
            char c = lower_ascii(text[i]);
            char n = lower_ascii(text[i + 1]);

            if (c == ' ' || c == '\t' || c == '\n') {
                emit_silence(pcm_out, pcm_max, &out, 45);
                continue;
            }

            if (c == ',' || c == ';' || c == ':') {
                emit_silence(pcm_out, pcm_max, &out, 95);
                continue;
            }

            if (c == '.' || c == '?' || c == '!') {
                emit_silence(pcm_out, pcm_max, &out, 150);
                continue;
            }

            if (c == 's' && n == 'h') {
                emit_unvoiced(pcm_out, pcm_max, &out, 75, 0.25f, &noise);
                i++;
                continue;
            }

            if (c == 'c' && n == 'h') {
                emit_stop(pcm_out, pcm_max, &out, &noise);
                emit_unvoiced(pcm_out, pcm_max, &out, 55, 0.22f, &noise);
                i++;
                continue;
            }

            if (c == 't' && n == 'h') {
                emit_unvoiced(pcm_out, pcm_max, &out, 65, 0.18f, &noise);
                i++;
                continue;
            }

            switch (c) {
            case 'a':
                emit_voiced(pcm_out, pcm_max, &out, 95, 118, 720, 1240, 2500, 0.85f);
                break;
            case 'e':
                emit_voiced(pcm_out, pcm_max, &out, 90, 120, 530, 1840, 2480, 0.82f);
                break;
            case 'i':
            case 'y':
                emit_voiced(pcm_out, pcm_max, &out, 88, 124, 270, 2290, 3010, 0.82f);
                break;
            case 'o':
                emit_voiced(pcm_out, pcm_max, &out, 95, 112, 570, 840, 2410, 0.82f);
                break;
            case 'u':
                emit_voiced(pcm_out, pcm_max, &out, 92, 110, 300, 870, 2240, 0.78f);
                break;
            case 'm':
                emit_voiced(pcm_out, pcm_max, &out, 58, 105, 250, 1100, 2200, 0.46f);
                break;
            case 'n':
                emit_voiced(pcm_out, pcm_max, &out, 55, 108, 300, 1500, 2500, 0.48f);
                break;
            case 'l':
                emit_voiced(pcm_out, pcm_max, &out, 58, 112, 400, 1200, 2600, 0.52f);
                break;
            case 'r':
                emit_voiced(pcm_out, pcm_max, &out, 65, 108, 350, 900, 2200, 0.50f);
                break;
            case 'w':
                emit_voiced(pcm_out, pcm_max, &out, 58, 108, 300, 700, 2200, 0.48f);
                break;
            case 'b':
            case 'd':
            case 'g':
            case 'j':
                emit_stop(pcm_out, pcm_max, &out, &noise);
                emit_voiced(pcm_out, pcm_max, &out, 35, 116, 500, 1500, 2400, 0.42f);
                break;
            case 'v':
            case 'z':
                emit_unvoiced(pcm_out, pcm_max, &out, 38, 0.10f, &noise);
                emit_voiced(pcm_out, pcm_max, &out, 45, 112, 330, 1500, 2600, 0.38f);
                break;
            case 'p':
            case 't':
            case 'k':
            case 'q':
                emit_stop(pcm_out, pcm_max, &out, &noise);
                break;
            case 'f':
            case 's':
            case 'h':
            case 'x':
                emit_unvoiced(pcm_out, pcm_max, &out, 60, 0.20f, &noise);
                break;
            default:
                emit_silence(pcm_out, pcm_max, &out, 25);
                break;
            }
        }
        emit_silence(pcm_out, pcm_max, &out, 160);
        return out;
    }

private:
    bool loaded_ = false;
#ifdef JARVIS_ONNX_LINKED
    bool           ort_ready_   = false;
    OrtEnv        *ort_env_     = nullptr;
    OrtSession    *ort_session_ = nullptr;
    OrtMemoryInfo *mem_info_    = nullptr;
#endif

    static char lower_ascii(char c)
    {
        if (c >= 'A' && c <= 'Z')
            return (char)(c - 'A' + 'a');
        return c;
    }

    static uint32_t step_for_hz(int hz)
    {
        return (uint32_t)(((uint64_t)(hz < 1 ? 1 : hz) * 65536ULL) / SAMPLE_RATE);
    }

    static float tri_wave(uint32_t phase)
    {
        uint32_t p = phase & 0xFFFFu;
        if (p < 32768u)
            return ((float)p / 16384.0f) - 1.0f;
        return 3.0f - ((float)p / 16384.0f);
    }

    static float envelope(size_t frame, size_t frames)
    {
        size_t attack = (SAMPLE_RATE * 8) / 1000;
        size_t release = (SAMPLE_RATE * 14) / 1000;
        float e = 1.0f;

        if (attack > 0 && frame < attack)
            e = (float)frame / (float)attack;
        if (release > 0 && frame + release > frames) {
            float r = (float)(frames - frame) / (float)release;
            if (r < e)
                e = r;
        }
        if (e < 0.0f) e = 0.0f;
        if (e > 1.0f) e = 1.0f;
        return e;
    }

    static void put_sample(float *pcm, size_t max, size_t *out, float sample)
    {
        if (*out >= max)
            return;
        if (sample > 0.85f) sample = 0.85f;
        if (sample < -0.85f) sample = -0.85f;
        pcm[(*out)++] = sample;
    }

    static void emit_silence(float *pcm, size_t max, size_t *out, int ms)
    {
        size_t frames = ((size_t)SAMPLE_RATE * (size_t)ms) / 1000u;
        for (size_t i = 0; i < frames && *out < max; i++)
            pcm[(*out)++] = 0.0f;
    }

    static void emit_voiced(float *pcm, size_t max, size_t *out, int ms,
                            int pitch_hz, int f1_hz, int f2_hz, int f3_hz,
                            float gain)
    {
        size_t frames = ((size_t)SAMPLE_RATE * (size_t)ms) / 1000u;
        uint32_t pitch = 0, f1 = 0, f2 = 0, f3 = 0;
        uint32_t pitch_step = step_for_hz(pitch_hz);
        uint32_t f1_step = step_for_hz(f1_hz);
        uint32_t f2_step = step_for_hz(f2_hz);
        uint32_t f3_step = step_for_hz(f3_hz);

        for (size_t i = 0; i < frames && *out < max; i++) {
            float e = envelope(i, frames);
            float glottal = tri_wave(pitch);
            float sample = (glottal * 0.28f +
                            tri_wave(f1) * 0.22f +
                            tri_wave(f2) * 0.12f +
                            tri_wave(f3) * 0.05f) * gain * e;
            put_sample(pcm, max, out, sample);
            pitch += pitch_step;
            f1 += f1_step;
            f2 += f2_step;
            f3 += f3_step;
        }
    }

    static void emit_unvoiced(float *pcm, size_t max, size_t *out, int ms,
                              float gain, uint32_t *noise)
    {
        size_t frames = ((size_t)SAMPLE_RATE * (size_t)ms) / 1000u;
        for (size_t i = 0; i < frames && *out < max; i++) {
            *noise = (*noise * 1664525u) + 1013904223u;
            int raw = (int)((*noise >> 16) & 0xFFFFu) - 32768;
            float s = ((float)raw / 32768.0f) * gain * envelope(i, frames);
            put_sample(pcm, max, out, s);
        }
    }

    static void emit_stop(float *pcm, size_t max, size_t *out, uint32_t *noise)
    {
        emit_silence(pcm, max, out, 28);
        emit_unvoiced(pcm, max, out, 24, 0.28f, noise);
    }
};

/* ============================================================================
 * Global speech state
 * ============================================================================ */
static WakeWordDetector g_wwd;
static WhisperSTT       g_stt;
static TTSEngine        g_tts;
static bool             g_tts_ready = false;
static bool             g_stt_ready = false;
static float            g_tts_pcm_buf[SAMPLE_RATE * 30];

/* Segment accumulation buffer */
static int16_t  segment_buf[MAX_SEGMENT_FRAMES];
static size_t   segment_frames = 0;
static int      silence_count  = 0;
static bool     capturing      = false;
static int16_t  stt_chunk[CHUNK_FRAMES];
static int      wake_energy_chunks = 0;
static int      stt_unavailable_notice_chunks = 0;
static bool     host_bridge_notice_shown = false;
static bool     stt_unavailable_plain_notice_shown = false;

static int pcm_level_percent(const int16_t *pcm, size_t frames)
{
    uint64_t sum = 0;
    if (frames == 0)
        return 0;

    for (size_t i = 0; i < frames; i++) {
        int32_t v = pcm[i];
        if (v < 0)
            v = -v;
        sum += (uint32_t)v;
    }

    uint64_t avg = sum / frames;
    int level = (int)((avg * 100ULL) / 12000ULL);
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    return level;
}

static void ensure_stt_ready(void)
{
    if (g_stt_ready)
        return;
    g_wwd.init("/jarvis/models/wake/wake_word.onnx");
    g_stt.init("/jarvis/models/stt/ggml-medium.en.bin");
    g_stt_ready = true;
    terminal_puts("[SPEECH] STT service listening for microphone PCM\n");
}

static void publish_transcript(const char *transcript)
{
    terminal_printf("[VOICE CMD] Parsed: %s\n", transcript);

    /* Send raw transcript to kernel for parsing and command execution */
    ipc_message_t msg{};
    msg.type = IPC_VOICE_INPUT;
    strncpy((char *)msg.payload, transcript, sizeof(msg.payload)-1);
    msg.payload_len = strlen(transcript);
    ipc_send(&msg);

    /* Also process special commands that should bypass IPC for immediate response */
    char cmd_lower[256];
    for (size_t i = 0; i < sizeof(cmd_lower)-1 && transcript[i]; i++) {
        char c = transcript[i];
        cmd_lower[i] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
    }
    cmd_lower[255] = '\0';

    if (strstr(cmd_lower, "shutdown") || strstr(cmd_lower, "power off")) {
        terminal_puts("[VOICE CMD] System shutdown initiated\n");
        system_shutdown();
    }
}

static void process_stt_chunk(const int16_t *chunk, size_t got)
{
    ui_shell_set_mic_level(pcm_level_percent(chunk, got));

    if (tts_echo_suppress_chunks > 0) {
        tts_echo_suppress_chunks--;
        if (tts_echo_suppress_chunks == 0)
            audio_flush_input();
        return;
    }

    if (stt_unavailable_notice_chunks > 0)
        stt_unavailable_notice_chunks--;

    if (!capturing) {
        /* Run wake word detector */
        float score = g_wwd.score(chunk, got);
        if (score > 0.5f) {
            wake_energy_chunks++;
        } else {
            wake_energy_chunks = 0;
        }

        if (wake_energy_chunks >= WAKE_REQUIRED_CHUNKS) {
            if (!g_stt.can_transcribe()) {
                if (voice_bridge_is_active()) {
                    if (!host_bridge_notice_shown) {
                        terminal_puts("[SPEECH] Host voice bridge active; guest Whisper runtime bypassed\n");
                        ui_shell_display("Host voice bridge is handling microphone commands.");
                        host_bridge_notice_shown = true;
                    }
                    wake_energy_chunks = 0;
                    audio_flush_input();
                    return;
                }

                if (!stt_unavailable_plain_notice_shown) {
                    terminal_puts("[SPEECH] Guest mic heard audio, but in-kernel Whisper is not linked; host bridge required\n");
                    ui_shell_display("Guest mic heard audio. Host voice bridge handles commands.");
                    stt_unavailable_plain_notice_shown = true;
                    stt_unavailable_notice_chunks = 100;
                }
                wake_energy_chunks = 0;
                audio_flush_input();
                return;
            }

            terminal_puts("[SPEECH] Wake word detected! Listening...\n");
            ui_shell_set_status("LISTENING");
            tts_speak("Yes?", 4);
            capturing     = true;
            segment_frames= 0;
            silence_count = 0;
            wake_energy_chunks = 0;
        }
        return;
    }

    /* Accumulate speech segment */
    size_t copy = got;
    if (segment_frames + copy > MAX_SEGMENT_FRAMES)
        copy = MAX_SEGMENT_FRAMES - segment_frames;

    memcpy(segment_buf + segment_frames, chunk, copy * sizeof(int16_t));
    segment_frames += copy;

    /* VAD: track silence */
    if (!vad_is_speech(chunk, got)) {
        silence_count++;
    } else {
        silence_count = 0;
    }

    /* End of utterance: silence timeout OR buffer full */
    bool end_of_speech = (silence_count >= VAD_SILENCE_FRAMES) ||
                         (segment_frames >= MAX_SEGMENT_FRAMES);
    if (end_of_speech && segment_frames > SAMPLE_RATE / 2) {
        /* At least 0.5 s of audio — run STT */
        char transcript[4096] = {};
        bool ok = g_stt.transcribe(segment_buf, segment_frames,
                                   transcript, sizeof(transcript));
        if (ok && transcript[0] != '\0') {
            terminal_printf("[STT] Transcript: %s\n", transcript);
            publish_transcript(transcript);
        } else {
            terminal_puts("[STT] Real Whisper inference is not linked; command text not produced\n");
        }

        /* Reset capture */
        capturing      = false;
        segment_frames = 0;
        silence_count  = 0;
        ui_shell_set_status("READY");
    }
}

static void ensure_tts_ready(void)
{
    if (g_tts_ready)
        return;
    const char *model_path = "/jarvis/models/tts/kokoro-v1.0.onnx";
    if (vfs_stat(model_path) != 0 &&
        vfs_stat("/jarvis/models/tts/kokoro-v1_0.pth") == 0) {
        model_path = "/jarvis/models/tts/kokoro-v1_0.pth";
    }
    g_tts.init(model_path);
    g_tts_ready = true;
}

static void render_tts_text(const char *text)
{
    ensure_tts_ready();
    g_tts_is_playing = true;
    ui_shell_set_status("SPEAKING");
    audio_flush_input();
    tts_echo_suppress_chunks = estimate_tts_suppress_chunks(text);
    terminal_printf("[TTS] Speaking: %s\n", text);

    if (voice_bridge_is_active()) {
        voice_serial_puts("SAY:");
        voice_serial_puts(text);
        voice_serial_puts("\n");
        terminal_puts("[TTS] Host speech bridge requested\n");
        terminal_puts("[SPEECH] Guest mic echo suppression active for host TTS\n");
        g_tts_is_playing = false;
        ui_shell_set_status("READY");
        return;
    }

    size_t frames = g_tts.synthesise(text, g_tts_pcm_buf,
                                     sizeof(g_tts_pcm_buf) / sizeof(float));
    int playback_chunks = (int)(frames / CHUNK_FRAMES) + 24;
    if (playback_chunks > tts_echo_suppress_chunks)
        tts_echo_suppress_chunks = playback_chunks;
    if (frames > 0)
        audio_write_frames(g_tts_pcm_buf, frames);
    audio_speak_machine_text(text);
    if (playback_chunks > tts_echo_suppress_chunks)
        tts_echo_suppress_chunks = playback_chunks;
    terminal_puts("[SPEECH] Mic echo suppression active after TTS\n");
    g_tts_is_playing = false;
    ui_shell_set_status("READY");
}

/* ============================================================================
 * Speech STT thread — called from kernel as a realtime kthread
 * ============================================================================ */
extern "C" void speech_stt_thread(void)
{
    ensure_stt_ready();
    terminal_puts("[SPEECH] STT thread running. Say 'Hey JARVIS'\n");

    for (;;) {
        /* Read 100 ms of PCM from audio ring buffer */
        size_t got = audio_read_frames(stt_chunk, CHUNK_FRAMES);
        if (got == 0) { cpu_relax(); continue; }
        process_stt_chunk(stt_chunk, got);
    }
}

extern "C" void speech_service_stt_once(void)
{
    ensure_stt_ready();

    for (int i = 0; i < 2; i++) {
        size_t got = audio_read_frames(stt_chunk, CHUNK_FRAMES);
        if (got == 0)
            return;
        process_stt_chunk(stt_chunk, got);
    }
}

/* ============================================================================
 * Speech TTS thread — renders AI text responses to audio
 * ============================================================================ */
extern "C" void speech_tts_thread(void)
{
    ensure_tts_ready();

    terminal_puts("[SPEECH] TTS thread running\n");

    for (;;) {
        if (ipc_has_message(IPC_TTS_REQUEST)) {
            ipc_message_t msg = ipc_recv_type(IPC_TTS_REQUEST);
            if (msg.type != IPC_TTS_REQUEST)
                continue;
            const char *text  = (const char *)msg.payload;

            render_tts_text(text);
        } else {
            cpu_relax();
        }
    }
}

extern "C" void speech_service_tts_once(void)
{
    for (int i = 0; i < 2 && ipc_has_message(IPC_TTS_REQUEST); i++) {
        ipc_message_t msg = ipc_recv_type(IPC_TTS_REQUEST);
        if (msg.type != IPC_TTS_REQUEST)
            return;
        render_tts_text((const char *)msg.payload);
    }
}

/* ============================================================================
 * C interface for kernel.c
 * ============================================================================ */
extern "C" void tts_speak(const char *text, size_t len)
{
    ipc_message_t msg{};
    msg.type = IPC_TTS_REQUEST;
    if (len >= sizeof(msg.payload)) len = sizeof(msg.payload) - 1;
    memcpy(msg.payload, text, len);
    msg.payload[len] = '\0';
    msg.payload_len = len;
    ipc_send(&msg);
}

extern "C" int speech_host_bridge_active(void)
{
    return voice_bridge_is_active();
}

extern "C" int speech_echo_suppression_active(void)
{
    return g_tts_is_playing || tts_echo_suppress_chunks > 0;
}

extern "C" int speech_guest_whisper_ready(void)
{
    return g_stt.can_transcribe();
}

extern "C" int speech_echo_suppression_chunks(void)
{
    return tts_echo_suppress_chunks;
}
