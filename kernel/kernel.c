/*
 * ============================================================================
 * JARVIS AI OS — Main Kernel  (kernel/kernel.c)
 * Language: C (C17)
 * Build:    see kernel/Makefile — this is a multi-file kernel (kernel.c,
 *           ipc_sched_pmm.c, vfs.c, syscall.c, platform.c, libc_stub.c,
 *           model_manifest.c, context_switch.S) linked with linker.ld.
 *           The previous single-file `gcc ... -o kernel.elf kernel.c` comment
 *           here was stale from before this became a multi-file kernel and
 *           didn't account for -mcmodel=kernel, linker.ld, or the asm file.
 *
 * This is the core of the AI-native operating system. Unlike a traditional
 * kernel, every subsystem here is designed to hand work off to the AI layer.
 * The scheduler is AI-priority-aware; the IPC bus passes intent messages;
 * the memory manager reserves a contiguous VRAM-adjacent pool for model weights.
 * ============================================================================
 */

#include "kernel.h"
#include <stdio.h>
#include <string.h>

/* ---- Forward declarations ------------------------------------------------ */
static void ipc_dispatch(ipc_message_t *msg);
static void memory_init(void);
static void load_e820_from_bootloader(void);
static void interrupt_init(void);
static void device_init(void);
static void ai_core_init(void);
static void speech_daemon_init(void);
static void security_daemon_init(void);
static void first_boot_setup(void);
static void authenticate_session(void);
static void kernel_desktop_loop(void);
static void kernel_service_ipc(void);
static void kernel_service_console(void);
static void kernel_service_voice_serial(void);
static void kernel_handle_command(const char *cmd);
static int kernel_try_voice_command(const char *text);
static void kernel_show_devices(void);
static void kernel_verify_models(void);
static const char *skip_spaces(const char *s);
void ai_scheduler_tick(void);  /* Called from IRQ0 timer handler */

/* ---- Global kernel state ------------------------------------------------- */
kernel_state_t g_kstate = {
    .mode          = KMODE_BOOTING,
    .ai_ready      = 0,
    .speech_ready  = 0,
    .audio_ready   = 0,
    .mic_ready     = 0,
    .speaker_ready = 0,
    .network_ready = 0,
    .usb_ready     = 0,
    .camera_ready  = 0,
    .uptime_ticks  = 0,
};

/*
 * Multiboot2's memory-map tag was parsed in boot/multiboot2.asm
 * (parse_multiboot2_memmap) and written to a fixed scratch address —
 * u32 count at 0x7100, followed by up to 128 packed {base:u64,
 * length:u64, type:u32} entries at 0x7104 — using the exact same
 * E820_USABLE/RESERVED/ACPI/NVS type values already defined in
 * kernel.h. This is read here, before memory_init() runs, so the PMM
 * gets the real installed RAM size and reserved regions instead of the
 * previous behaviour: e820_count was always 0 (nothing ever populated
 * it), so memory_init() always fell back to a hardcoded "conservative
 * 512 MB" guess — correct by coincidence on a default QEMU VM, wrong
 * (wasteful or unsafe) on any real machine or differently-sized VM.
 */
static void load_e820_from_bootloader(void)
{
    volatile uint32_t *count_ptr = (volatile uint32_t *)0x7100;
    uint32_t count = *count_ptr;

    if (count > MAX_E820)
        count = MAX_E820;

    g_kstate.e820_count = (int)count;
    if (count == 0) {
        terminal_puts("[MEM] Bootloader provided no memory map\n");
        return;
    }

    const uint8_t *src = (const uint8_t *)0x7104;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = src + (size_t)i * 20;
        memcpy(&g_kstate.e820[i], entry, sizeof(e820_entry_t));
    }
    terminal_printf("[MEM] Bootloader memory map: %u region(s)\n", count);
}

/* ---- Kernel entry point — called from stage2 bootloader ------------------ */
void jarvis_kernel_main(void)
{
    /*
     * Order matters here. Hardware must be init'd before we touch
     * any memory-mapped registers; interrupts come up before daemons.
     */
    terminal_init();
    terminal_puts("[JARVIS] Kernel entry\n");

    load_e820_from_bootloader();   /* must run before memory_init() */
    memory_init();        /* physical frame allocator + virtual memory map   */
    interrupt_init();     /* IDT, PIC remapping, timer IRQ0 wired to AI tick */
    device_init();        /* PCI scan, AHCI, USB, NIC, audio, GPU framebuf   */
    vfs_init();           /* ramfs, model slots, user data folders */
    devmgr_init();                 /* table of every device and its driver */
    net_init();                    /* network process: DHCP, then ready for use */
    diskfs_mount();                /* files saved on disk by earlier sessions */
    fatfs_mount_all();             /* USB flash drives -> /jarvis/usb0, ... */
    usermode_install_programs();   /* bundled user programs -> /jarvis/bin */
    ui_shell_show_loading_desktop();

    /*
     * AI subsystems. Each init function:
     *   1. Allocates a ring buffer in shared memory
     *   2. Spawns a kernel thread (kthread_create)
     *   3. Leaves a MODEL_SLOT marker where the user drops a .safetensors
     */
    ai_core_init();           /* LLM brain — loads model, sets up inference Q */
    speech_daemon_init();     /* Whisper STT + TTS — mic → text → LLM → audio */
    security_daemon_init();   /* Anomaly detection, auth, disk encryption       */

    g_kstate.mode = KMODE_RUNNING;

    if (!g_kstate.setup_complete)
        first_boot_setup();
    else
        authenticate_session();

    terminal_puts("[JARVIS] Boot complete. Running desktop loop.\n");
    terminal_puts("[JARVIS] Foreground runtime active until preemptive IDT scheduling is complete.\n");

    ai_brain_on_desktop_ready();
    kernel_desktop_loop();
}

/* ==========================================================================
 * MEMORY MANAGER
 * Physical frame allocator using a bitmap. Reserves 0–1 MB for legacy,
 * 1 MB–(kernel_end) for kernel, then a large MODEL_POOL for AI weights.
 * ========================================================================== */
#define PAGE_SIZE         4096UL
#define MODEL_POOL_MB     256UL   /* boot-time stub pool for model staging     */
#define MODEL_POOL_PAGES  (MODEL_POOL_MB * 1024UL * 1024UL / PAGE_SIZE)

/*
 * NOTE: there used to be a *second*, entirely separate `phys_bitmap[262144]`
 * static array declared right here. It was filled with 0xFF on every boot
 * and then never read by anything else in this file — the real frame
 * bitmap that pmm_free_range()/pmm_reserve_range()/pmm_alloc_contiguous()
 * actually use lives in ipc_sched_pmm.c as its own file-scope static. Two
 * arrays with the same name in different translation units don't collide
 * at link time (internal linkage), so this wasn't a build error — just
 * 256 KB of .bss doing nothing. Removed.
 */
static uint64_t model_pool_base;      /* physical address of AI model pool */

static void memory_init(void)
{
    terminal_puts("[MEM] Initializing physical frame allocator\n");

    /*
     * Parse E820 memory map (passed by bootloader in g_kstate.e820).
     * Mark usable frames as free.
     */
    for (int i = 0; i < g_kstate.e820_count; i++) {
        e820_entry_t *e = &g_kstate.e820[i];
        if (e->type == E820_USABLE)
            pmm_free_range(e->base, e->length);
    }

    if (g_kstate.e820_count == 0) {
        terminal_puts("[MEM] No boot memory map; using conservative 512 MB QEMU fallback\n");
        pmm_free_range(0x100000, 511ULL * 1024ULL * 1024ULL);
    }

    /* Reserve first 1 MB (BIOS, VGA, IVT) */
    pmm_reserve_range(0, 0x100000);

    /*
     * Reserve the kernel image. _kernel_start/_kernel_end are *virtual*
     * (higher-half) addresses, but the frame allocator works on physical
     * ones: reserving the virtual range reserved nothing, and the 256 MB
     * model pool below was handed out at 0x100000 — directly on top of the
     * boot page tables, kernel code and .bss. Reserve the physical range
     * from 1 MB (boot stub + page tables) to the end of .bss instead.
     */
    extern uint8_t _kernel_start, _kernel_end, _kernel_phys_base;
    uint64_t kernel_phys_end = (uint64_t)&_kernel_phys_base +
                               (uint64_t)(&_kernel_end - &_kernel_start);
    pmm_reserve_range(0x100000, kernel_phys_end - 0x100000 + PAGE_SIZE - 1);

    /*
     * Carve out MODEL_POOL — contiguous physical pages for LLM weight tensors.
     * The AI core mmap()s this region to get zero-copy access.
     * [MODEL_SLOT:PHYSICAL_POOL] — user loads weights here via jarvis-model-load tool
     */
    model_pool_base = pmm_alloc_contiguous(MODEL_POOL_PAGES);
    g_kstate.model_pool_base = model_pool_base;
    if (model_pool_base == 0) {
        g_kstate.model_pool_size = 0;
        terminal_printf("[MEM] Model pool unavailable (%lu MB requested)\n",
            MODEL_POOL_MB);
    } else {
        g_kstate.model_pool_size = MODEL_POOL_PAGES * PAGE_SIZE;
        terminal_printf("[MEM] Model pool: 0x%llx – 0x%llx (%lu MB)\n",
            model_pool_base,
            model_pool_base + g_kstate.model_pool_size,
            MODEL_POOL_MB);
    }

    /* Set up 4-level page tables for kernel virtual address space */
    vmm_init(model_pool_base);

    terminal_puts("[MEM] Memory manager ready\n");
}

/* ==========================================================================
 * INTERRUPT DESCRIPTOR TABLE
 * Standard x86_64 IDT. IRQ0 (timer) drives the AI scheduler tick.
 * IRQ1 (keyboard), IRQ4/3 (UART), IRQ11 (GPU DMA done) wired to handlers.
 * ========================================================================== */
static void interrupt_init(void)
{
    terminal_puts("[IRQ] Setting up IDT\n");
    idt_init();

    scheduler_init();   /* the running code becomes process 1 */
    usermode_init();    /* GDT with user segments, TSS, SYSCALL */

    /* Remap PIC: master 0x20–0x27, slave 0x28–0x2F */
    pic_remap(0x20, 0x28);

    /* Timer: 100 Hz → each tick = 10 ms → ai_scheduler_tick() */
    pit_set_frequency(100);
    idt_set_handler(0x20, irq0_timer_handler);

    /* PS/2 keyboard (IRQ1) and mouse (IRQ12) */
    idt_set_handler(0x21, irq1_keyboard_handler);
    idt_set_handler(0x2C, irq12_mouse_handler);

    /* Serial (debug console) */
    idt_set_handler(0x24, irq4_serial_handler);

    sti();  /* enable interrupts */
    terminal_puts("[IRQ] Interrupts enabled\n");
}

/* ==========================================================================
 * AI-AWARE PROCESS SCHEDULER — timer tick and host bridge flag
 * Full scheduler implementation: kernel/ipc_sched_pmm.c
 *
 * There used to be a *second* `current_proc` variable declared right here,
 * separate from the one ipc_sched_pmm.c uses internally for every
 * preemption/requeue decision. This one was the only copy ai_scheduler_tick
 * ever actually updated, which meant ipc_sched_pmm.c's own bookkeeping saw
 * "current" as permanently NULL — so context_switch(current_proc, next)
 * always ran with from=NULL, which its own NULL-guard silently no-ops. Net
 * effect: kernel threads were created and "selected" by the scheduler's
 * bookkeeping, but the CPU register switch into them never happened, so
 * none of them ever ran a single instruction. scheduler_pick_next() now
 * owns current_proc and performs context_switch() itself, so there's one
 * source of truth and an actual switch happens.
 * ========================================================================== */
static int host_voice_bridge_active = 0;

int voice_bridge_is_active(void)
{
    return host_voice_bridge_active;
}

/* Called from IRQ0 timer handler every 10 ms */
void ai_scheduler_tick(void)
{
    g_kstate.uptime_ticks++;
    scheduler_tick();       /* may switch to another process */
}


/* ==========================================================================
 * IPC MESSAGE BUS
 * Processes communicate with the AI core via typed messages on a lock-free
 * ring buffer. The LLM brain reads "REQUEST" messages and emits "RESPONSE".
 * ========================================================================== */
static void ipc_dispatch(ipc_message_t *msg)
{
    switch (msg->type) {
    case IPC_VOICE_INPUT:
        /* Speech engine decoded audio — forward to LLM brain */
        ui_shell_set_status("THINKING");
        ui_shell_add_transcript((const char *)msg->payload);
        kernel_try_voice_command((const char *)msg->payload);
        ai_core_infer((const char *)msg->payload, msg->payload_len);
        break;

    case IPC_AI_RESPONSE:
        /* LLM produced text — send to TTS for voice output */
        tts_speak((const char *)msg->payload, msg->payload_len);
        /* Also display on UI shell */
        ui_shell_display((const char *)msg->payload);
        break;

    case IPC_TASK_EXECUTE:
        /* User asked JARVIS to run something */
        task_executor_run((const char *)msg->payload);
        break;

    case IPC_FILE_OP:
        ai_filesystem_handle(msg);
        break;

    case IPC_SECURITY_ALERT:
        security_respond(msg);
        break;

    case IPC_SHUTDOWN:
        terminal_puts("[JARVIS] Shutting down...\n");
        system_shutdown();
        break;

    default:
        terminal_printf("[IPC] Unknown message type: 0x%02X\n", msg->type);
        break;
    }
}

/* ==========================================================================
 * DEVICE INIT STUB
 * Full PCI enumeration + driver binding. Each driver exposes a kernel API
 * that the AI core can call without going through user space.
 * ========================================================================== */
static void device_init(void)
{
    terminal_puts("[DEV] PCI enumeration...\n");
    pci_enumerate();          /* fills g_kstate.pci_devices[]        */

    ahci_init();              /* NVMe/SATA — disk R/W for model files */
    usb_init();               /* USB HID (keyboard, mouse)            */
    input_init();             /* PS/2 mouse cursor path for QEMU       */
    nic_init();               /* network stack (TCP/IP)               */
    audio_init();             /* audio in (mic) / out (speakers)      */
    gpu_framebuffer_init();   /* map GPU framebuffer for UI shell      */
    desktop_system_init();    /* initialize Windows 11-style desktop   */

    terminal_puts("[DEV] All devices online\n");
}

static void first_boot_setup(void)
{
    if (g_kstate.setup_complete)
        return;

    ui_shell_show_setup_wizard();
    terminal_puts("\n[JARVIS SETUP] First boot setup\n");
    terminal_puts("[JARVIS SETUP] Desktop setup wizard is active.\n");
    terminal_puts("[JARVIS SETUP] Continuing without blocking on serial input.\n");

    ui_shell_set_setup_step("Create Your Profile",
                            "Temporary local profile created for this boot.");
    strncpy(g_kstate.username, "JARVIS User", sizeof(g_kstate.username) - 1);
    g_kstate.username[sizeof(g_kstate.username) - 1] = '\0';

    ui_shell_set_setup_step("Security Features",
                            "Security setup is available after GUI input is wired.");
    g_kstate.security_enabled = 0;
    g_kstate.security_pin[0] = '\0';

    ui_shell_set_setup_step("Voice Verification",
                            "Voice enrollment hooks are enabled for the speech pipeline.");
    ui_shell_display("Voice verification will use microphone transcripts when capture is linked.");
    g_kstate.setup_complete = 1;
    terminal_printf("[JARVIS SETUP] Welcome, %s.\n", g_kstate.username);
    terminal_printf("[JARVIS SETUP] Security lock: %s\n",
        g_kstate.security_enabled ? "enabled" : "disabled");
    terminal_puts("[JARVIS SETUP] Settings are in-memory until VFS persistence is implemented.\n\n");

    ui_shell_unlock_desktop();
}

static void authenticate_session(void)
{
    if (!g_kstate.security_enabled) {
        ui_shell_unlock_desktop();
        return;
    }

    ui_shell_show_lock_screen();
    terminal_puts("\n[JARVIS LOGIN] Voice verification screen is active.\n");
    terminal_puts("[JARVIS LOGIN] Waiting for voice auth; serial login prompt disabled.\n");
}

static void kernel_service_ipc(void)
{
    for (int i = 0; i < 8; i++) {
        ipc_message_t msg = {};

        if (ipc_has_message(IPC_VOICE_INPUT))
            msg = ipc_recv_type(IPC_VOICE_INPUT);
        else if (ipc_has_message(IPC_AI_RESPONSE))
            msg = ipc_recv_type(IPC_AI_RESPONSE);
        else if (ipc_has_message(IPC_TASK_EXECUTE))
            msg = ipc_recv_type(IPC_TASK_EXECUTE);
        else if (ipc_has_message(IPC_FILE_OP))
            msg = ipc_recv_type(IPC_FILE_OP);
        else if (ipc_has_message(IPC_SECURITY_ALERT))
            msg = ipc_recv_type(IPC_SECURITY_ALERT);
        else if (ipc_has_message(IPC_SHUTDOWN))
            msg = ipc_recv_type(IPC_SHUTDOWN);
        else
            return;

        if (msg.type == 0)
            return;
        ipc_dispatch(&msg);
    }
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static char kernel_lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static void kernel_normalize_voice_text(const char *src, char *dst, size_t max)
{
    size_t n = 0;
    int last_space = 1;

    if (!dst || max == 0)
        return;
    if (!src)
        src = "";

    while (*src && n + 1 < max) {
        char c = kernel_lower_ascii(*src++);
        int keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

        if (keep) {
            dst[n++] = c;
            last_space = 0;
        } else if (!last_space) {
            dst[n++] = ' ';
            last_space = 1;
        }
    }

    if (n > 0 && dst[n - 1] == ' ')
        n--;
    dst[n] = '\0';
}

static int kernel_voice_has(const char *text, const char *needle)
{
    return strstr(text, needle) != NULL;
}

static int kernel_try_voice_command(const char *text)
{
    char cmd[192];

    kernel_normalize_voice_text(text, cmd, sizeof(cmd));
    if (cmd[0] == '\0')
        return 0;

    if (strcmp(cmd, "status") == 0 ||
        kernel_voice_has(cmd, "system status") ||
        kernel_voice_has(cmd, "how are you")) {
        kernel_handle_command("status");
        return 1;
    }

    if (strcmp(cmd, "apps") == 0 ||
        strcmp(cmd, "desktop") == 0 ||
        kernel_voice_has(cmd, "show desktop") ||
        kernel_voice_has(cmd, "open desktop")) {
        kernel_handle_command("apps");
        return 1;
    }

    if (strcmp(cmd, "files") == 0 ||
        kernel_voice_has(cmd, "open files") ||
        kernel_voice_has(cmd, "file manager") ||
        kernel_voice_has(cmd, "list files")) {
        kernel_handle_command("files");
        return 1;
    }

    if (strcmp(cmd, "terminal") == 0 ||
        strcmp(cmd, "console") == 0 ||
        kernel_voice_has(cmd, "open terminal")) {
        kernel_handle_command("terminal");
        return 1;
    }

    if (strcmp(cmd, "settings") == 0 ||
        kernel_voice_has(cmd, "open settings")) {
        kernel_handle_command("settings");
        return 1;
    }

    if (strcmp(cmd, "security") == 0 ||
        strcmp(cmd, "guard") == 0 ||
        kernel_voice_has(cmd, "open security") ||
        kernel_voice_has(cmd, "security center")) {
        kernel_handle_command("security");
        return 1;
    }

    if (strcmp(cmd, "models") == 0 ||
        kernel_voice_has(cmd, "verify models") ||
        kernel_voice_has(cmd, "model status")) {
        kernel_handle_command("models");
        return 1;
    }

    if (strcmp(cmd, "voice") == 0 ||
        kernel_voice_has(cmd, "voice settings") ||
        kernel_voice_has(cmd, "microphone")) {
        kernel_handle_command("voice");
        return 1;
    }

    if (strcmp(cmd, "network") == 0 ||
        strcmp(cmd, "internet") == 0 ||
        kernel_voice_has(cmd, "open network")) {
        kernel_handle_command("network");
        return 1;
    }

    if (strcmp(cmd, "camera") == 0 ||
        kernel_voice_has(cmd, "open camera")) {
        kernel_handle_command("camera");
        return 1;
    }

    if (strcmp(cmd, "web") == 0 ||
        strcmp(cmd, "browser") == 0 ||
        kernel_voice_has(cmd, "open browser")) {
        kernel_handle_command("web");
        return 1;
    }

    if (strcmp(cmd, "cursor up") == 0 ||
        kernel_voice_has(cmd, "move cursor up")) {
        kernel_handle_command("cursor up");
        return 1;
    }

    if (strcmp(cmd, "cursor down") == 0 ||
        kernel_voice_has(cmd, "move cursor down")) {
        kernel_handle_command("cursor down");
        return 1;
    }

    if (strcmp(cmd, "cursor left") == 0 ||
        kernel_voice_has(cmd, "move cursor left")) {
        kernel_handle_command("cursor left");
        return 1;
    }

    if (strcmp(cmd, "cursor right") == 0 ||
        kernel_voice_has(cmd, "move cursor right")) {
        kernel_handle_command("cursor right");
        return 1;
    }

    if (strcmp(cmd, "click") == 0 ||
        kernel_voice_has(cmd, "mouse click")) {
        kernel_handle_command("click");
        return 1;
    }

    if (strcmp(cmd, "help") == 0 ||
        kernel_voice_has(cmd, "what can you do")) {
        kernel_handle_command("help");
        return 1;
    }

    return 0;
}

static void kernel_show_devices(void)
{
    ui_shell_display(g_kstate.audio_ready ? "Audio: HDA mapped, ring buffers ready." : "Audio: not ready.");
    ui_shell_display(g_kstate.mic_ready ? "Microphone: capture DMA ready." : "Microphone: HDA capture DMA not wired yet.");
    ui_shell_display(g_kstate.speaker_ready ? "Speaker: machine speaker fallback ready." : "Speaker: not ready.");
    ui_shell_display(g_kstate.network_ready ? "Network: e1000 mapped, RX/TX rings ready." : "Network: not ready.");
    ui_shell_display(g_kstate.usb_ready ? "USB: host controller present." : "USB: not ready.");
    ui_shell_display(g_kstate.camera_ready ? "Camera: ready." : "Camera: UVC driver/pass-through not available yet.");
}

static void kernel_verify_models(void)
{
    int brain = vfs_stat("/jarvis/models/brain/model.gguf") == 0;
    int stt = vfs_stat("/jarvis/models/stt/ggml-medium.en.bin") == 0;
    int tts = vfs_stat("/jarvis/models/tts/kokoro-v1.0.onnx") == 0 ||
              vfs_stat("/jarvis/models/tts/kokoro-v1_0.pth") == 0;
    int wake = vfs_stat("/jarvis/models/wake/wake_word.onnx") == 0;

    ui_shell_display(brain ? "Brain model: visible to kernel VFS." : "Brain model: not visible to kernel VFS.");
    ui_shell_display(stt ? "STT model: visible to kernel VFS." : "STT model: not visible to kernel VFS.");
    ui_shell_display(tts ? "TTS model: visible to kernel VFS." : "TTS model: not visible to kernel VFS.");
    ui_shell_display(wake ? "Wake word model: visible to kernel VFS." : "Wake word model: not visible to kernel VFS.");

    if (!brain || !stt || !tts || !wake)
        ui_shell_display("Model verifier: ISO/disk file loading is not wired into kernel VFS yet.");
}

static void kernel_handle_command(const char *cmd)
{
    char buf[512];

    if (strcmp(cmd, "help") == 0) {
        ui_shell_display("Commands: help, apps, cursor, click, status, devices, files, terminal, network, camera, settings, security, models, shutdown.");
    } else if (strcmp(cmd, "apps") == 0 || strcmp(cmd, "desktop") == 0) {
        ui_shell_display("Apps: Files, Terminal, Web, Guard, Models, Voice, Settings, Network, Camera.");
    } else if (strcmp(cmd, "cursor") == 0) {
        ui_shell_display("Cursor: use cursor up/down/left/right, click. QEMU host cursor stays visible.");
    } else if (strncmp(cmd, "cursor ", 7) == 0) {
        const char *dir = skip_spaces(cmd + 7);
        if (strcmp(dir, "up") == 0) {
            ui_shell_move_cursor(0, -24);
        } else if (strcmp(dir, "down") == 0) {
            ui_shell_move_cursor(0, 24);
        } else if (strcmp(dir, "left") == 0) {
            ui_shell_move_cursor(-24, 0);
        } else if (strcmp(dir, "right") == 0) {
            ui_shell_move_cursor(24, 0);
        }
        ui_shell_display("Cursor moved.");
    } else if (strcmp(cmd, "click") == 0) {
        ui_shell_click_cursor();
    } else if (strcmp(cmd, "status") == 0) {
        ui_shell_display(g_kstate.ai_ready ? "AI core ready." : "AI core loading.");
        ui_shell_display(g_kstate.speech_ready ? "Voice pipeline ready." : "Voice pipeline loading.");
        ui_shell_display(g_kstate.security_enabled ? "Security lock enabled." : "Security lock not configured.");
        kernel_show_devices();
    } else if (strcmp(cmd, "devices") == 0) {
        kernel_show_devices();
    } else if (strcmp(cmd, "verify") == 0 || strcmp(cmd, "models") == 0) {
        kernel_verify_models();
    } else if (strcmp(cmd, "voice-test") == 0) {
        if (g_kstate.speech_ready && g_kstate.mic_ready)
            ui_shell_display("Voice test: speech service is listening for microphone PCM; serial text is debug only.");
        else
            ui_shell_display("Voice test: microphone capture is not wired yet; serial text is debug only.");
    } else if (strcmp(cmd, "speaker-test") == 0) {
        if (audio_self_test() == 0)
            ui_shell_display("Speaker test: tone queued to playback ring.");
        else
            ui_shell_display("Speaker test failed: audio device not ready.");
    } else if (strcmp(cmd, "net-test") == 0) {
        if (nic_self_test() == 0)
            ui_shell_display("Network test: e1000 status register read successfully.");
        else
            ui_shell_display("Network test failed: NIC not ready.");
    } else if (strncmp(cmd, "ls", 2) == 0 && (cmd[2] == '\0' || cmd[2] == ' ')) {
        const char *path = skip_spaces(cmd + 2);
        if (*path == '\0') path = "/jarvis";
        if (vfs_list(path, buf, sizeof(buf)) >= 0)
            ui_shell_display(buf);
        else
            ui_shell_display("List failed.");
    } else if (strncmp(cmd, "mkdir ", 6) == 0) {
        const char *path = skip_spaces(cmd + 6);
        if (vfs_mkdir(path) == 0) {
            snprintf(buf, sizeof(buf), "Created folder: %s", path);
            ui_shell_display(buf);
        } else {
            ui_shell_display("Folder creation failed.");
        }
    } else if (strncmp(cmd, "touch ", 6) == 0) {
        const char *path = skip_spaces(cmd + 6);
        if (vfs_create_file(path) == 0) {
            snprintf(buf, sizeof(buf), "Created file: %s", path);
            ui_shell_display(buf);
        } else {
            ui_shell_display("File creation failed.");
        }
    } else if (strncmp(cmd, "write ", 6) == 0) {
        const char *path = skip_spaces(cmd + 6);
        const char *text = path;
        while (*text && *text != ' ' && *text != '\t')
            text++;
        if (*text) {
            size_t path_len = (size_t)(text - path);
            char file_path[160];
            if (path_len >= sizeof(file_path))
                path_len = sizeof(file_path) - 1;
            memcpy(file_path, path, path_len);
            file_path[path_len] = '\0';
            text = skip_spaces(text);
            int fd = vfs_open(file_path, 0x02 | 0x04 | 0x10);
            if (fd >= 0) {
                vfs_write(fd, text, strlen(text));
                vfs_close(fd);
                snprintf(buf, sizeof(buf), "Wrote file: %s", file_path);
                ui_shell_display(buf);
            } else {
                ui_shell_display("Write failed.");
            }
        } else {
            ui_shell_display("Usage: write /jarvis/data/file.txt hello");
        }
    } else if (strncmp(cmd, "read ", 5) == 0) {
        const char *path = skip_spaces(cmd + 5);
        if (vfs_read_file(path, buf, sizeof(buf)) >= 0)
            ui_shell_display(buf);
        else
            ui_shell_display("Read failed.");
    } else if (strcmp(cmd, "files") == 0) {
        desktop_system_open_app("Files");
    } else if (strcmp(cmd, "terminal") == 0 || strcmp(cmd, "term") == 0) {
        desktop_system_open_app("Terminal");
    } else if (strcmp(cmd, "network") == 0 || strcmp(cmd, "net") == 0) {
        ui_shell_display(g_kstate.network_ready ? "Network app: e1000 online; DHCP/TCP stack is staged." : "Network app: adapter not ready.");
    } else if (strcmp(cmd, "camera") == 0) {
        ui_shell_display(g_kstate.camera_ready ? "Camera app: camera ready." : "Camera app: UVC camera driver is not implemented yet.");
    } else if (strcmp(cmd, "voice") == 0) {
        ui_shell_display("Voice app: host bridge, wake word, microphone level, and TTS controls.");
    } else if (strcmp(cmd, "web") == 0 || strcmp(cmd, "browser") == 0) {
        desktop_system_open_app("Browser");
    } else if (strcmp(cmd, "settings") == 0) {
        desktop_system_open_app("Settings");
    } else if (strcmp(cmd, "security") == 0) {
        ui_shell_set_security("SAFE", 0);
        ui_shell_display("Security center: audit, network monitor, disk encryption hooks active.");
    } else if (strcmp(cmd, "clear") == 0) {
        desktop_system_close_all();
        ui_shell_display("Desktop cleared.");
    } else if (strcmp(cmd, "shutdown") == 0) {
        ui_shell_display("Shutdown requested.");
        system_shutdown();
    } else if (cmd[0] != '\0') {
        ui_shell_display("Serial console is debug input only. Voice transcript updates only from microphone audio.");
    }
}

static void kernel_deliver_voice_text(const char *text)
{
    ipc_message_t msg;
    size_t len;

    host_voice_bridge_active = 1;
    text = skip_spaces(text);
    len = strlen(text);
    if (len == 0)
        return;
    if (len >= sizeof(msg.payload))
        len = sizeof(msg.payload) - 1;

    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_VOICE_INPUT;
    memcpy(msg.payload, text, len);
    msg.payload[len] = '\0';
    msg.payload_len = len;
    ipc_send(&msg);

    terminal_printf("[VOICE] Host bridge transcript: %s\n", (const char *)msg.payload);
    ui_shell_set_status("LISTENED");
}

static void kernel_handle_voice_serial_line(const char *line)
{
    const char *text;
    const char *trimmed = skip_spaces(line);

    if (strcmp(trimmed, "BRIDGE:READY") == 0 ||
        strcmp(trimmed, "VOICE_BRIDGE_READY") == 0 ||
        strstr(trimmed, "BRIDGE") != NULL ||
        strstr(trimmed, "RIDGE:READY") != NULL) {
        if (host_voice_bridge_active)
            return;
        host_voice_bridge_active = 1;
        terminal_puts("[VOICE] Host voice bridge connected; COM2 transcripts enabled\n");
        ui_shell_display("Host voice bridge connected.");
        return;
    }

    if (strncmp(trimmed, "VOICE:", 6) == 0) {
        text = trimmed + 6;
    } else if (strncmp(trimmed, "VOICE ", 6) == 0) {
        text = trimmed + 6;
    } else if (strncmp(trimmed, "TRANSCRIPT:", 11) == 0) {
        text = trimmed + 11;
    } else {
        terminal_printf("[VOICE] Ignoring COM2 line: %s\n", trimmed);
        return;
    }

    kernel_deliver_voice_text(text);
}

static void kernel_service_voice_serial(void)
{
    static char line_buf[512];
    static size_t line_len = 0;
    char c;

    while (voice_serial_poll_char(&c)) {
        if (c == '\r' || c == '\n') {
            if (line_len > 0) {
                line_buf[line_len] = '\0';
                kernel_handle_voice_serial_line(line_buf);
                line_len = 0;
                line_buf[0] = '\0';
            }
            continue;
        }

        if (c == '\b' || c == 0x7F) {
            if (line_len > 0)
                line_len--;
            line_buf[line_len] = '\0';
            continue;
        }

        if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E)
            continue;

        if (line_len + 1 < sizeof(line_buf)) {
            line_buf[line_len++] = c;
            line_buf[line_len] = '\0';
        }
    }
}

static void kernel_service_console(void)
{
    static char cmd_buf[160];
    static size_t cmd_len = 0;
    char c;

    while (terminal_poll_char(&c)) {
        if (c == '\r' || c == '\n') {
            cmd_buf[cmd_len] = '\0';
            terminal_puts("\n");
            kernel_handle_command(cmd_buf);
            cmd_len = 0;
            cmd_buf[0] = '\0';
            ui_shell_set_status("READY");
            continue;
        }

        if (c == '\b' || c == 0x7F) {
            if (cmd_len > 0)
                cmd_len--;
            cmd_buf[cmd_len] = '\0';
            continue;
        }

        if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E)
            continue;

        if (cmd_len + 1 < sizeof(cmd_buf)) {
            cmd_buf[cmd_len++] = c;
            cmd_buf[cmd_len] = '\0';
            ui_shell_set_status("DEBUG");
        }
    }
}

static void kernel_desktop_loop(void)
{
    terminal_puts("[DESKTOP] Desktop runtime is active.\n");

    for (;;) {
        kernel_service_console();
        input_poll();
        input_process_queue();
        kernel_service_voice_serial();
        speech_service_stt_once();
        kernel_service_ipc();
        speech_service_tts_once();

        /* Draws only when something changed (see desktop_system_render). */
        desktop_system_render();

        /*
         * Give up the CPU until the next tick (or sooner, on keyboard / mouse
         * input). Other processes run in the meantime; if there are none the
         * scheduler halts the CPU.
         */
        kthread_sleep(1);
    }
}

/* ==========================================================================
 * AI CORE INIT
 * Spawns the LLM inference kernel thread. The actual model is a HuggingFace
 * .safetensors file placed in /jarvis/models/brain/. The C++ ai_runtime
 * (ai_core/runtime.cpp) handles tensor loading and forward pass.
 *
 * [MODEL_SLOT:LLM_BRAIN]
 * Expected model: any CausalLM (Llama 3, Mistral, Qwen, Phi-3…)
 * Drop model files in /jarvis/models/brain/ — runtime auto-detects.
 * ========================================================================== */
static void ai_core_init(void)
{
    terminal_puts("[AI] Initialising LLM brain...\n");

    /* Map model pool into kernel virtual space */
    void *model_vaddr = vmm_map_range(
        g_kstate.model_pool_base,
        g_kstate.model_pool_size,
        VMM_PROT_RW | VMM_PROT_NOCACHE
    );
    g_kstate.model_vaddr = model_vaddr;

    /* Spawn AI inference thread (runs on CPU core 1, pinned) */
    proc_spawn("ai_brain", ai_brain_thread, PRIO_AI, PROC_FLAG_SERVICE);

    /* Shared context window buffer — LLM reads from here */
    g_kstate.context_buffer = kmalloc(CONTEXT_BUFFER_SIZE);
    g_kstate.context_len    = 0;

    g_kstate.ai_ready = 1;
    ui_shell_display("AI brain service registered.");
    terminal_puts("[AI] LLM brain service registered for scheduler startup.\n");
    terminal_puts("[AI] Foreground desktop loop will keep the OS responsive meanwhile.\n");
}

/* ==========================================================================
 * SPEECH DAEMON INIT
 * [MODEL_SLOT:SPEECH_STT] — Whisper or similar (openai/whisper, systran/faster-whisper)
 * [MODEL_SLOT:SPEECH_TTS] — Kokoro, Coqui XTTS, or Bark
 * [MODEL_SLOT:WAKE_WORD]  — openWakeWord or picovoice Porcupine
 *
 * Audio pipeline:
 *   Mic → ALSA ring buffer → wake-word detector → Whisper STT → IPC(VOICE_INPUT)
 *   IPC(TTS_REQUEST) → TTS model → ALSA playback
 * ========================================================================== */
static void speech_daemon_init(void)
{
    terminal_puts("[SPEECH] Initialising speech pipeline...\n");

    /* Ring buffer for raw PCM (16-bit, 16 kHz mono — Whisper native) */
    g_kstate.audio_in_buf  = kmalloc(AUDIO_RING_SIZE);
    g_kstate.audio_out_buf = kmalloc(AUDIO_RING_SIZE);

    /* Thread: mic capture → VAD → STT */
    proc_spawn("speech_stt", speech_stt_thread, PRIO_REALTIME, PROC_FLAG_SERVICE);

    /* Thread: TTS render → speaker */
    proc_spawn("speech_tts", speech_tts_thread, PRIO_REALTIME, PROC_FLAG_SERVICE);

    g_kstate.speech_ready = 1;
    terminal_puts("[SPEECH] Speech pipeline ready\n");
}

/* ==========================================================================
 * SECURITY DAEMON INIT
 * [MODEL_SLOT:SECURITY_ANOMALY] — lightweight anomaly detection model
 *   Options: DistilBERT fine-tuned on syscall sequences, or a simple
 *   autoencoder trained on normal behaviour profiles.
 *
 * Monitors: syscall sequences, network flows, file access patterns.
 * On alert: locks session, encrypts sensitive pages, notifies user via voice.
 * ========================================================================== */
static void security_daemon_init(void)
{
    terminal_puts("[SEC] Initialising security daemon...\n");

    /* Syscall hook — every syscall passes through security_audit() */
    syscall_set_hook(security_audit);

    /* Network monitor — NIC driver calls this for each packet header */
    nic_set_packet_hook(security_net_audit);

    /* Anomaly detection thread */
    proc_spawn("sec_daemon", security_daemon_thread, PRIO_AI, PROC_FLAG_SERVICE);

    /* Disk encryption (AES-256-XTS) key — derived from user voiceprint hash */
    disk_encryption_init();

    terminal_puts("[SEC] Security daemon active\n");
}
