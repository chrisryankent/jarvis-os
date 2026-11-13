/*
 * =============================================================================
 * JARVIS AI OS — AI Core Runtime  (ai_core/runtime.cpp)
 * Language: C++20
 * Compile:  g++ -std=c++20 -O3 -fno-exceptions -o ai_runtime.o runtime.cpp
 *
 * This module runs in kernel space (ring 0) as a privileged thread.
 * It owns the LLM forward pass, context window, tool call dispatch, and
 * the conversation memory store.
 *
 * [MODEL_SLOT:LLM_BRAIN]
 *   Place any HuggingFace CausalLM model in: /jarvis/models/brain/
 *   Supported formats: GGUF (via llama.cpp embedded), safetensors
 *   Recommended: Llama-3.2-3B-Instruct, Qwen2.5-7B, Phi-3-mini
 *   The runtime auto-detects config.json and tokenizer.json.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

/* ============================================================================
 * JARVIS SYSTEM PROMPT
 * This is injected at position 0 of every context window. It defines
 * JARVIS's personality, capabilities, and tool invocation syntax.
 * ============================================================================ */
static constexpr const char *JARVIS_SYSTEM_PROMPT =
    "You are JARVIS — Just A Rather Very Intelligent System. "
    "You are the AI brain of this operating system. You speak directly to the "
    "user via voice. You can control every aspect of the OS by calling tools.\n\n"
    "## AVAILABLE TOOLS\n"
    "Call tools using XML tags:\n"
    "<tool name=\"run_command\"><arg>ls -la /home</arg></tool>\n"
    "<tool name=\"open_file\"><arg>/home/user/docs/report.pdf</arg></tool>\n"
    "<tool name=\"search_files\"><arg>*.py modified:today</arg></tool>\n"
    "<tool name=\"network_scan\"><arg>192.168.1.0/24</arg></tool>\n"
    "<tool name=\"system_info\"></tool>\n"
    "<tool name=\"set_volume\"><arg>70</arg></tool>\n"
    "<tool name=\"shutdown\"></tool>\n"
    "<tool name=\"install_app\"><arg>firefox</arg></tool>\n\n"
    "## BEHAVIOUR\n"
    "- Respond concisely. You speak aloud — keep answers under 3 sentences "
    "  unless the user asks for detail.\n"
    "- Always call a tool when the user wants to DO something, not just know.\n"
    "- Announce what you are doing: 'Opening your report now.'\n"
    "- On security alerts, always inform the user first.\n";

static int parse_decimal(const char *s)
{
    int value = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }
    return value;
}

static void copy_reply(char *out_buf, size_t out_max, const char *text)
{
    if (!out_buf || out_max == 0)
        return;

    snprintf(out_buf, out_max, "%s", text ? text : "");
    out_buf[out_max - 1] = '\0';
}

static char lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static void normalize_prompt(const char *input, char *out, size_t out_max)
{
    size_t n = 0;
    bool last_space = true;

    if (!out || out_max == 0)
        return;

    if (!input)
        input = "";

    while (*input && n + 1 < out_max) {
        char c = lower_ascii(*input++);
        bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

        if (keep) {
            out[n++] = c;
            last_space = false;
        } else if (!last_space) {
            out[n++] = ' ';
            last_space = true;
        }
    }

    if (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

static bool has_phrase(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != nullptr;
}

static void fallback_reply(const char *user_input, char *out_buf, size_t out_max)
{
    char normalized[512];
    normalize_prompt(user_input, normalized, sizeof(normalized));

    /*
     * Built-in replies, used until the language model runs. Keep them short
     * and true: they are spoken aloud, and tools/voice_phrases.txt lists them
     * so they can be pre-recorded in the owner's voice (make voice-render).
     * The fallback no longer repeats what it heard: reading the transcript
     * back made JARVIS answer its own voice in a loop.
     */
    if (normalized[0] == '\0') {
        copy_reply(out_buf, out_max, "I'm listening.");
    } else if (has_phrase(normalized, "hello") ||
               has_phrase(normalized, "hi jarvis") ||
               has_phrase(normalized, "hey jarvis")) {
        copy_reply(out_buf, out_max, "Hello. How can I help?");
    } else if (has_phrase(normalized, "status") ||
               has_phrase(normalized, "system status") ||
               has_phrase(normalized, "how are you")) {
        copy_reply(out_buf, out_max, "All systems are running. Details are in the task manager.");
    } else if (has_phrase(normalized, "settings")) {
        copy_reply(out_buf, out_max, "Opening Settings.");
    } else if (has_phrase(normalized, "file") || has_phrase(normalized, "files")) {
        copy_reply(out_buf, out_max, "Opening Files.");
    } else if (has_phrase(normalized, "terminal") || has_phrase(normalized, "console")) {
        copy_reply(out_buf, out_max, "Opening Terminal.");
    } else if (has_phrase(normalized, "security") || has_phrase(normalized, "guard")) {
        copy_reply(out_buf, out_max, "Opening the security center.");
    } else if (has_phrase(normalized, "network") || has_phrase(normalized, "internet")) {
        copy_reply(out_buf, out_max, "For network details, open the terminal and type ifconfig.");
    } else if (has_phrase(normalized, "model") || has_phrase(normalized, "brain")) {
        copy_reply(out_buf, out_max,
            "The language model is installed but not running yet, so I answer from my built-in commands.");
    } else if (has_phrase(normalized, "voice") || has_phrase(normalized, "microphone")) {
        copy_reply(out_buf, out_max, "I hear you through the microphone and answer out loud.");
    } else if (has_phrase(normalized, "cursor") || has_phrase(normalized, "mouse")) {
        copy_reply(out_buf, out_max, "Say cursor up, down, left or right, or say click.");
    } else if (has_phrase(normalized, "help") ||
               has_phrase(normalized, "what can you do")) {
        copy_reply(out_buf, out_max,
            "You can say: open files, open terminal, open settings, status, lock, or shut down.");
    } else {
        copy_reply(out_buf, out_max, "Sorry, I didn't catch that. Say help to hear what I can do.");
    }
}

/* ============================================================================
 * AI Runtime class — one instance, lives for the OS lifetime
 * ============================================================================ */
class JARVISRuntime {
public:
    /* ---- Lifecycle -------------------------------------------------------- */
    JARVISRuntime() = default;

    bool init(void *model_pool_vaddr, size_t pool_size,
              const char *model_dir)
    {
        if (initialized_)
            return model_loaded_;

        model_pool_  = static_cast<uint8_t *>(model_pool_vaddr);
        pool_size_   = pool_size;
        model_loaded_= false;
        inference_runtime_ready_ = false;
        initialized_ = true;

        /* Clear context window */
        context_len_ = 0;

        /* Inject system prompt as first context entry */
        append_context(Role::SYSTEM, JARVIS_SYSTEM_PROMPT);

        /* Try to load model from model_dir */
        return load_model(model_dir);
    }

    void on_desktop_ready()
    {
        char greeting[192];

        if (g_kstate.username[0] != '\0') {
            snprintf(greeting, sizeof(greeting),
                     "JARVIS ready, %s. Say a command.", g_kstate.username);
        } else {
            snprintf(greeting, sizeof(greeting),
                     "JARVIS ready. Say a command.");
        }

        append_context(Role::ASSISTANT, greeting);
        ui_shell_set_status("SPEAKING");
        ui_shell_display(greeting);
        tts_speak(greeting, strlen(greeting));
        terminal_printf("[AI] Brain startup voice queued: %s\n", greeting);
    }

    /* ---- Load model weights from disk ------------------------------------ */
    /*
     * [MODEL_SLOT:LLM_BRAIN]
     * Supports:
     *   - GGUF format:       model.gguf  (auto-quantized, CPU+GPU)
     *   - SafeTensors format: model.safetensors + config.json + tokenizer.json
     *
     * To use a HuggingFace model:
     *   1. Download: huggingface-cli download <repo-id> --local-dir /jarvis/models/brain
     *   2. JARVIS auto-loads on next boot or via: jarvis-model-reload
     */
    bool load_model(const char *model_dir)
    {
        terminal_printf("[AI] Scanning model directory: %s\n", model_dir);

        /* Look for GGUF first (preferred — single file, supports Q4_K_M etc.) */
        char gguf_path[512];
        snprintf(gguf_path, sizeof(gguf_path), "%s/model.gguf", model_dir);

        if (file_exists(gguf_path)) {
            return load_gguf(gguf_path);
        }

        /* Fall back to SafeTensors */
        char st_path[512];
        snprintf(st_path, sizeof(st_path), "%s/model.safetensors", model_dir);
        if (file_exists(st_path)) {
            return load_safetensors(model_dir);
        }

        terminal_puts("[AI] WARNING: No model found. Running in stub mode.\n");
        terminal_puts("[AI] Place model files in /jarvis/models/brain/\n");
        model_loaded_ = false;
        return false;
    }

    /* ---- Inference -------------------------------------------------------- */
    /*
     * Called from kernel IPC handler when a voice input or user message arrives.
     * Runs a full forward pass through the LLM and returns generated text.
     * Tool calls in the output are parsed and dispatched automatically.
     */
    void infer(const char *user_input, char *out_buf, size_t out_max)
    {
        /* Add user turn to context */
        append_context(Role::USER, user_input ? user_input : "");

        if (!model_loaded_ || !inference_runtime_ready_) {
            fallback_reply(user_input, out_buf, out_max);
            append_context(Role::ASSISTANT, out_buf);
            return;
        }

        /* Run tokenizer → forward pass → detokenize */
        TokenSequence tokens = tokenize(context_buf_, context_len_);
        TokenSequence output  = forward_pass(tokens);
        detokenize(output, out_buf, out_max);

        /* Add assistant reply to context */
        append_context(Role::ASSISTANT, out_buf);

        /* Parse and dispatch any tool calls in the output */
        dispatch_tool_calls(out_buf);

        /* Evict oldest turns if context is near limit */
        if (context_len_ > CONTEXT_EVICT_THRESHOLD)
            evict_oldest_turn();
    }

    /* ---- Tool call parser + dispatcher ----------------------------------- */
    void dispatch_tool_calls(const char *text)
    {
        /*
         * Scan output for <tool name="..."><arg>...</arg></tool> patterns.
         * Each tool call maps to a kernel operation.
         */
        const char *p = text;
        while ((p = strstr(p, "<tool name=\"")) != nullptr) {
            char tool_name[64] = {};
            char tool_arg[1024] = {};

            /* Extract tool name */
            const char *name_start = p + 12;
            const char *name_end   = strchr(name_start, '"');
            if (!name_end) break;
            size_t name_len = name_end - name_start;
            if (name_len >= sizeof(tool_name)) break;
            memcpy(tool_name, name_start, name_len);

            /* Extract arg */
            const char *arg_start = strstr(p, "<arg>");
            const char *arg_end   = strstr(p, "</arg>");
            if (arg_start && arg_end && arg_end > arg_start) {
                arg_start += 5; /* skip <arg> */
                size_t arg_len = arg_end - arg_start;
                if (arg_len < sizeof(tool_arg))
                    memcpy(tool_arg, arg_start, arg_len);
            }

            execute_tool(tool_name, tool_arg);
            p = name_end;
        }
    }

    void execute_tool(const char *name, const char *arg)
    {
        terminal_printf("[AI TOOL] %s(%s)\n", name, arg);

        if (strcmp(name, "run_command") == 0) {
            task_executor_run(arg);

        } else if (strcmp(name, "open_file") == 0) {
            ipc_message_t msg{};
            msg.type = IPC_FILE_OP;
            strncpy((char *)msg.payload, arg, sizeof(msg.payload) - 1);
            msg.payload_len = strlen(arg);
            ipc_send(&msg);

        } else if (strcmp(name, "search_files") == 0) {
            ai_filesystem_search(arg);

        } else if (strcmp(name, "network_scan") == 0) {
            security_net_scan(arg);

        } else if (strcmp(name, "system_info") == 0) {
            char info[512];
            snprintf(info, sizeof(info),
                "Uptime: %llu ticks | Model pool: %llu MB | AI ready: %d",
                (unsigned long long)g_kstate.uptime_ticks,
                (unsigned long long)(g_kstate.model_pool_size / (1024ULL*1024)),
                g_kstate.ai_ready);
            tts_speak(info, strlen(info));

        } else if (strcmp(name, "set_volume") == 0) {
            int vol = parse_decimal(arg);
            audio_set_volume(vol);

        } else if (strcmp(name, "shutdown") == 0) {
            tts_speak("Shutting down. Goodbye.", 23);
            system_shutdown();

        } else if (strcmp(name, "install_app") == 0) {
            /* arg: path of a .jpk package (see kernel/pkg.c) */
            char err[96];
            pkg_info_t info;
            if (pkg_install(arg, &info, err, sizeof(err)) == 0)
                tts_speak("The app is installed.", 21);
            else
                terminal_printf("[AI TOOL] install_app %s: %s\n", arg, err);

        } else {
            terminal_printf("[AI TOOL] Unknown tool: %s\n", name);
        }
    }

private:
    /* ---- Context window -------------------------------------------------- */
    enum class Role { SYSTEM, USER, ASSISTANT };

    static constexpr size_t CONTEXT_BUF_SIZE      = 512 * 1024; /* 512 KB */
    static constexpr size_t CONTEXT_EVICT_THRESHOLD = 400 * 1024;

    char   context_buf_[CONTEXT_BUF_SIZE] = {};
    size_t context_len_ = 0;

    void append_context(Role role, const char *text)
    {
        const char *tag = (role == Role::SYSTEM)    ? "<|system|>"    :
                          (role == Role::USER)       ? "<|user|>"      :
                                                       "<|assistant|>" ;
        size_t tag_len  = strlen(tag);
        size_t text_len = strlen(text);
        size_t end_tag  = 13; /* <|end|>\n */

        if (context_len_ + tag_len + text_len + end_tag >= CONTEXT_BUF_SIZE)
            return; /* overflow guard */

        memcpy(context_buf_ + context_len_, tag, tag_len);
        context_len_ += tag_len;
        memcpy(context_buf_ + context_len_, text, text_len);
        context_len_ += text_len;
        memcpy(context_buf_ + context_len_, "<|end|>\n", 8);
        context_len_ += 8;
    }

    void evict_oldest_turn()
    {
        /* Skip system prompt (find second <|user|>) and truncate */
        const char *second_user = strstr(context_buf_, "<|user|>");
        if (!second_user) return;
        second_user = strstr(second_user + 1, "<|user|>");
        if (!second_user) return;

        size_t offset = second_user - context_buf_;
        size_t new_len = context_len_ - offset;
        memmove(context_buf_, second_user, new_len);
        context_len_ = new_len;
    }

    /* ---- Tokenizer / forward pass stubs ---------------------------------- */
    /*
     * [MODEL_SLOT:LLM_BRAIN]
     * In the real build these call into llama.cpp (GGUF path) or a custom
     * C++ tensor engine (SafeTensors path). The stubs below are replaced by
     * the build system when a model is detected.
     *
     * For GGUF:  link against libllama (from llama.cpp repo)
     *   - llama_tokenize()
     *   - llama_eval() / llama_decode()
     *   - llama_token_to_str()
     *
     * For SafeTensors: link against libtorch or a custom BLAS kernel
     */
    struct TokenSequence {
        int32_t *ids;
        size_t   len;
    };

    TokenSequence tokenize(const char *text, size_t len)
    {
        /* STUB — replace with llama_tokenize() or SentencePiece */
        (void)text; (void)len;
        return {nullptr, 0};
    }

    TokenSequence forward_pass(TokenSequence input)
    {
        /* STUB — replace with llama_decode() or custom attention kernel */
        (void)input;
        return {nullptr, 0};
    }

    void detokenize(TokenSequence seq, char *buf, size_t max)
    {
        /* STUB — replace with llama_token_to_str() */
        (void)seq;
        copy_reply(buf, max, "[AI stub - load a model to enable responses]");
    }

    /* ---- GGUF loader ----------------------------------------------------- */
    bool load_gguf(const char *path)
    {
        terminal_printf("[AI] Loading GGUF: %s\n", path);
        /*
         * Real implementation:
         *   llama_model_params params = llama_model_default_params();
         *   params.n_gpu_layers = 99;  // offload all layers to GPU
         *   llama_model *model = llama_load_model_from_file(path, params);
         *   if (!model) return false;
         *   llama_context *ctx = llama_new_context_with_model(model, ctx_params);
         *   ...store in members...
         */
        model_loaded_ = true;  /* model file is visible */
        inference_runtime_ready_ = false;
        terminal_puts("[AI] GGUF model loaded (stub; using fallback dialogue until llama runtime is linked)\n");
        return true;
    }

    /* ---- SafeTensors loader ---------------------------------------------- */
    bool load_safetensors(const char *dir)
    {
        terminal_printf("[AI] Loading SafeTensors from: %s\n", dir);
        /*
         * Real implementation:
         *   1. Parse config.json → model architecture (n_layers, n_heads, d_model)
         *   2. Parse tokenizer.json → vocabulary
         *   3. mmap() model.safetensors → parse header (JSON) → tensor descriptors
         *   4. Copy/quantize tensor data into model_pool_ (DMA if GPU available)
         */
        model_loaded_ = true;
        inference_runtime_ready_ = false;
        terminal_puts("[AI] SafeTensors model loaded (stub; using fallback dialogue until tensor runtime is linked)\n");
        return true;
    }

    bool file_exists(const char *path)
    {
        /* Kernel file check via VFS */
        return vfs_stat(path) == 0;
    }

    /* ---- Member data ----------------------------------------------------- */
    uint8_t *model_pool_  = nullptr;
    size_t   pool_size_   = 0;
    bool     initialized_ = false;
    bool     model_loaded_= false;
    bool     inference_runtime_ready_ = false;
};

/* ---- Singleton ----------------------------------------------------------- */
static JARVISRuntime g_runtime;

/* ---- C interface called from kernel.c ------------------------------------ */
extern "C" {

void ai_brain_thread(void)
{
    g_runtime.init(
        g_kstate.model_vaddr,
        g_kstate.model_pool_size,
        "/jarvis/models/brain"
    );
    g_kstate.ai_ready = 1;
    terminal_puts("[AI] Brain thread running\n");

    /* Event loop: pull messages from IPC ring and infer */
    for (;;) {
        if (ipc_has_message(IPC_AI_REQUEST) || ipc_has_message(IPC_VOICE_INPUT)) {
            ipc_message_t msg = ipc_recv_next();
            char response[8192];
            g_runtime.infer((const char *)msg.payload, response, sizeof(response));

            /* Send response back */
            ipc_message_t resp{};
            resp.type = IPC_AI_RESPONSE;
            strncpy((char *)resp.payload, response, sizeof(resp.payload) - 1);
            resp.payload_len = strlen(response);
            ipc_send(&resp);
        } else {
            cpu_relax(); /* yield to other threads */
        }
    }
}

void ai_core_infer(const char *prompt, size_t len)
{
    char bounded_prompt[4096];
    char response[4096];
    ipc_message_t msg{};
    size_t response_len;

    if (!prompt)
        prompt = "";
    if (len >= sizeof(bounded_prompt))
        len = sizeof(bounded_prompt) - 1;
    memcpy(bounded_prompt, prompt, len);
    bounded_prompt[len] = '\0';

    g_runtime.init(
        g_kstate.model_vaddr,
        g_kstate.model_pool_size,
        "/jarvis/models/brain"
    );
    g_runtime.infer(bounded_prompt, response, sizeof(response));

    msg.type = IPC_AI_RESPONSE;
    response_len = strlen(response);
    if (response_len >= sizeof(msg.payload))
        response_len = sizeof(msg.payload) - 1;
    memcpy(msg.payload, response, response_len);
    msg.payload[response_len] = '\0';
    msg.payload_len = response_len;
    ipc_send(&msg);
}

void ai_brain_on_desktop_ready(void)
{
    g_runtime.init(
        g_kstate.model_vaddr,
        g_kstate.model_pool_size,
        "/jarvis/models/brain"
    );
    g_runtime.on_desktop_ready();
}

} /* extern "C" */
