/*
 * kernel/kernel.h — JARVIS AI OS kernel types and declarations
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- E820 memory map ----------------------------------------------------- */
#define E820_USABLE   1
#define E820_RESERVED 2
#define E820_ACPI     3
#define E820_NVS      4

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} __attribute__((packed)) e820_entry_t;

/* ---- Kernel mode --------------------------------------------------------- */
typedef enum {
    KMODE_BOOTING = 0,
    KMODE_RUNNING = 1,
    KMODE_PANIC   = 2,
    KMODE_SHUTDOWN= 3,
} kernel_mode_t;

/* ---- Global kernel state ------------------------------------------------- */
#define MAX_E820        128
#define MAX_PCI_DEVS    256
#define CONTEXT_BUFFER_SIZE (128 * 1024)   /* 128 KB LLM context window */
#define AUDIO_RING_SIZE (1024 * 1024)      /* 1 MB PCM ring buffer       */

typedef struct {
    kernel_mode_t mode;
    int           ai_ready;
    int           speech_ready;
    int           audio_ready;
    int           mic_ready;
    int           speaker_ready;
    int           network_ready;
    int           usb_ready;
    int           camera_ready;
    int           setup_complete;
    uint64_t      uptime_ticks;

    /* First-boot settings */
    char          username[32];
    int           security_enabled;
    char          security_pin[16];

    /* Memory */
    uint64_t      model_pool_base;
    uint64_t      model_pool_size;
    void         *model_vaddr;

    /* E820 */
    e820_entry_t  e820[MAX_E820];
    int           e820_count;

    /* AI context */
    char         *context_buffer;
    size_t        context_len;

    /* Audio buffers */
    uint8_t      *audio_in_buf;
    uint8_t      *audio_out_buf;

    /* PCI */
    uint32_t      pci_devices[MAX_PCI_DEVS][3]; /* bus,dev,fn */
    int           pci_count;
} kernel_state_t;

extern kernel_state_t g_kstate;

typedef struct pci_device {
    uint8_t  bus, dev, fn;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass;
    uint32_t bar[6];
    uint8_t  irq;
} pci_device_t;

/* ---- VMM flags ----------------------------------------------------------- */
#define VMM_PROT_RW      0x01
#define VMM_PROT_NOCACHE 0x02
#define VMM_PROT_USER    0x04

/* ---- Function prototypes ------------------------------------------------- */

/* Memory */
void    pmm_free_range(uint64_t base, uint64_t len);
void    pmm_reserve_range(uint64_t base, uint64_t len);
uint64_t pmm_alloc_contiguous(uint64_t pages);
void    vmm_init(uint64_t model_pool_phys);
void   *vmm_map_range(uint64_t phys, uint64_t size, uint32_t flags);
void   *kmalloc(size_t size);
void    kfree(void *ptr);

/* Interrupts */
void    idt_init(void);
void    idt_set_handler(uint8_t vector, void (*handler)(void));
void    pic_remap(uint8_t master_offset, uint8_t slave_offset);
void    pit_set_frequency(uint32_t hz);
void    sti(void);
void    cli(void);

/*
 * Called from the common assembly trampoline in interrupts.S for every one
 * of the 256 vectors. Looks up the registered handler for `vector` (set via
 * idt_set_handler) and calls it; sends EOI to the PIC for anything in the
 * remapped IRQ range (0x20-0x2F) whether or not a specific handler was
 * registered, so unhandled IRQ lines don't stall the PIC; on an unhandled
 * CPU exception (vector < 0x20) it prints diagnostics instead of silently
 * triple-faulting.
 */
void    interrupt_dispatch(uint64_t vector, uint64_t error_code);

/* Interrupt handlers */
void    irq0_timer_handler(void);
void    irq1_keyboard_handler(void);
void    irq12_mouse_handler(void);
void    ps2_irq_service(void);              /* drivers/input.c */
void    kbd_handle_scancode(uint8_t code);  /* kernel/platform.c */
void    irq4_serial_handler(void);

/* ---- Keyboard modifier bit flags (wm_input_event_t.data.keyboard.mods) --- */
#define KMOD_SHIFT  0x01
#define KMOD_CTRL   0x02
#define KMOD_ALT    0x04
#define KMOD_CAPS   0x08
#define KMOD_SUPER  0x10   /* Windows / Super key */

/* Unified WM input event queue (input/input_events.c) */
void    input_key_press(uint32_t key, int mods);
void    input_key_release(uint32_t key, int mods);
void    input_text_char(char ch);
void    input_mouse_move(int x, int y);
void    input_mouse_click(int x, int y, int button);
void    input_mouse_release(int x, int y, int button);

/* Processes */
/*
 * Shared with kernel/ipc_sched_pmm.c — this is the canonical definition.
 * Previously this enum was declared as a *local* typedef inside
 * ipc_sched_pmm.c only, which meant kernel.c could not see PRIO_AI /
 * PRIO_REALTIME / PRIO_SYSTEM / PRIO_USER at all. Any call site in
 * kernel.c that passed one of these constants to kthread_create()
 * (ai_core_init, speech_daemon_init, security_daemon_init) failed to
 * compile. Moving the enum here is the fix — keep it as the single
 * source of truth and do not redefine it elsewhere.
 */
typedef enum { PRIO_REALTIME=0, PRIO_AI=1, PRIO_SYSTEM=2, PRIO_USER=3 } proc_prio_t;
typedef enum { PROC_RUNNING=0, PROC_READY=1, PROC_BLOCKED=2, PROC_ZOMBIE=3 } proc_state_t;

void    kthread_create(const char *name, void (*fn)(void), int prio, size_t stack);
void   *scheduler_pick_next(void);
void    context_switch(void *from, void *to);
void    ai_scheduler_tick(void);  /* Called on IRQ0 every 10 ms */

/* Thread exit / reaping — without this, proc_alloc() can never reclaim a
 * slot once a thread finishes, and the 256-thread pool is a one-way ratchet. */
void    kthread_exit(uint64_t exit_code);
uint32_t kthread_current_pid(void);
void    kthread_yield(void); /* voluntary yield — promotes priority by one level */

/* Large-block allocator — backs big buffers (AI model files, audio rings)
 * directly with physical pages instead of the small kernel heap. See
 * kmalloc()/kfree() in ipc_sched_pmm.c for details. Anything at or above
 * this size must go through kmalloc_large()/kfree_large() instead of
 * kmalloc()/kfree() — they are not interchangeable allocators. */
#define LARGE_ALLOC_THRESHOLD (1UL * 1024 * 1024) /* 1 MB */
void   *kmalloc_large(size_t size);
void    kfree_large(void *ptr, size_t size);

/* IPC */
typedef enum {
    IPC_VOICE_INPUT    = 0x01,
    IPC_AI_REQUEST     = 0x02,
    IPC_AI_RESPONSE    = 0x03,
    IPC_TASK_EXECUTE   = 0x04,
    IPC_FILE_OP        = 0x05,
    IPC_SECURITY_ALERT = 0x06,
    IPC_TTS_REQUEST    = 0x07,
    IPC_SHUTDOWN       = 0xFF,
} ipc_type_t;

typedef struct ipc_message {
    ipc_type_t  type;
    uint32_t    sender_pid;
    uint32_t    target_pid;
    uint64_t    timestamp;
    uint8_t     payload[4096];
    size_t      payload_len;
} ipc_message_t;

void          ipc_send(ipc_message_t *msg);
ipc_message_t ipc_recv_next(void);
int           ipc_has_message(uint8_t type);
ipc_message_t ipc_recv_type(uint8_t type);

/* Device drivers */
void    pci_enumerate(void);
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);
void    ahci_init(void);
void    usb_init(void);
void    input_init(void);
void    input_poll(void);
void    input_process_queue(void);
void    nic_init(void);
int     nic_is_ready(void);
int     nic_self_test(void);
void    audio_init(void);
int     audio_is_ready(void);
int     audio_mic_is_ready(void);
int     audio_speaker_is_ready(void);
int     audio_self_test(void);
int     camera_is_ready(void);
int     usb_is_ready(void);
size_t  audio_read_frames(int16_t *buf, size_t max_frames);
size_t  audio_write_frames(const float *buf, size_t frames);
void    audio_speak_machine_text(const char *text);
void    audio_flush_input(void);
void    audio_set_volume(int percent);
void    gpu_framebuffer_init(void);

/* AI threads */
void    ai_brain_thread(void);
void    speech_stt_thread(void);
void    speech_tts_thread(void);
void    security_daemon_thread(void);

/* AI APIs */
void    ai_core_infer(const char *prompt, size_t len);
void    ai_brain_on_desktop_ready(void);
void    tts_speak(const char *text, size_t len);
void    speech_service_stt_once(void);
void    speech_service_tts_once(void);
void    ui_shell_init(void);
void    ui_shell_display(const char *text);
void    ui_shell_draw_frame(void);
void    ui_shell_show_loading_desktop(void);
void    ui_shell_show_lock_screen(void);
void    ui_shell_show_setup_wizard(void);
void    ui_shell_unlock_desktop(void);
void    ui_shell_set_setup_step(const char *title, const char *detail);
void    ui_shell_set_status(const char *status);
void    ui_shell_set_security(const char *label, int alert);
void    ui_shell_set_mic_level(int level);
void    ui_shell_add_transcript(const char *text);
void    ui_shell_move_cursor(int dx, int dy);
void    ui_shell_click_cursor(void);
void    ui_shell_mouse_button(int button, int pressed); /* 1=left 2=right 3=middle */

/* Desktop system (Windows 11-style UI) */
void    desktop_system_init(void);
void    desktop_system_render(void);
void    desktop_system_handle_mouse_move(int x, int y);
void    desktop_system_handle_mouse_click(int x, int y, int button);
void    desktop_system_handle_mouse_release(int x, int y, int button);
void    desktop_system_handle_mouse_wheel(int x, int y, int delta);
void    desktop_system_handle_key(uint32_t key, int mods);
void    desktop_system_handle_key_up(uint32_t key, int mods);
void    desktop_system_handle_text_input(char ch);
void    desktop_system_open_app(const char *app);
void    desktop_system_close_all(void);
int     desktop_system_is_initialized(void);
void    desktop_invalidate(void);           /* request a redraw next frame */
void    desktop_system_shutdown(void);
void    desktop_system_restart(void);

/* Read-only view of the UI shell state for the desktop / JARVIS app */
int     ui_shell_is_desktop(void);
int     ui_shell_is_locked(void);
const char *ui_shell_get_status(void);
const char *ui_shell_get_security_label(void);
int     ui_shell_get_security_alert(void);
int     ui_shell_get_mic_level(void);
int     ui_shell_line_count(void);
const char *ui_shell_get_line(int index_from_oldest);
void    ui_shell_render_fullscreen(void);

/* Shell/taskbar integration */
int     taskbar_handle_click(int x, int y);
void    taskbar_toggle_start_menu(void);
int     taskbar_is_start_menu_open(void);
void    start_menu_draw(void);

/* Speech runtime state */
int     speech_host_bridge_active(void);
int     speech_echo_suppression_active(void);
int     speech_guest_whisper_ready(void);
int     speech_echo_suppression_chunks(void);

void    task_executor_run(const char *cmd);
void    ai_filesystem_handle(void *msg);
void    security_respond(void *msg);
void    security_audit(uint64_t syscall_nr, uint64_t *args);
void    security_net_audit(const uint8_t *pkt_hdr, size_t len);
void    security_net_scan(const char *cidr);
void    disk_encryption_init(void);
void    syscall_set_hook(void (*hook)(uint64_t, uint64_t *));
void    nic_set_packet_hook(void (*hook)(const uint8_t *, size_t));
void    cpu_relax(void);
void    pkg_install(const char *name);

/* VFS */
void    vfs_init(void);
int     vfs_open(const char *path, uint32_t flags);
int     vfs_read(int fd, void *buf, size_t len);
int     vfs_write(int fd, const void *buf, size_t len);
void    vfs_close(int fd);
int     vfs_stat(const char *path);
int     vfs_create_file(const char *path);
int     vfs_mkdir(const char *path);
void    vfs_seed_packaged_models(void);
int     vfs_read_file(const char *path, char *buf, size_t max);
int     vfs_list(const char *prefix, char *buf, size_t max);
void    vfs_append(const char *path, const char *data, size_t len);
void    ai_filesystem_search(const char *query);

/* System */
void    terminal_init(void);
void    terminal_puts(const char *s);
void    terminal_printf(const char *fmt, ...);
void    terminal_readline(const char *prompt, char *buf, size_t max, int echo);
int     terminal_poll_char(char *out);
int     voice_serial_poll_char(char *out);
void    voice_serial_puts(const char *s);
int     voice_bridge_is_active(void);
void    system_shutdown(void);

#ifdef __cplusplus
}
#endif
