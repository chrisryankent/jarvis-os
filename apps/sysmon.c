/*
 * =============================================================================
 * JARVIS OS — System monitor / Task Manager (apps/sysmon.c)
 * Language: C17
 *
 * Processes tab: the kernel's live process table with CPU use per process;
 *                End task, Suspend / Resume, and three test workloads that
 *                exercise the scheduler (CPU load, sleeper, deadlock).
 * Overview tab:  uptime, display, redraw rate, devices and services.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

uint32_t desktop_frame_count(void);     /* desktop/desktop_init.c */

#define MON_TAB_H     40
#define MON_ROW_H     24
#define MON_FOOTER_H  84
#define MON_MAX_PROCS 64

static struct {
    wm_window_t *window;
    int tab;                            /* 0 = processes, 1 = overview */
    uint64_t sample_tick;
    uint32_t sample_frames;
    uint32_t fps;

    uint32_t selected_pid;
    proc_info_t procs[MON_MAX_PROCS];
    int proc_count;

    /* CPU usage sampling (once a second) */
    uint32_t last_pid[MON_MAX_PROCS];
    uint64_t last_ticks[MON_MAX_PROCS];
    int      last_count;
    uint32_t cpu_pct[MON_MAX_PROCS];    /* parallel to procs[] */
    uint64_t last_idle;
    uint32_t idle_pct;
} g_sysmon;

/* ---- Test workloads ------------------------------------------------------
 * Real kernel processes used to exercise the scheduler from the UI. They
 * only touch their own state and the scheduler's primitives.
 */
static int g_test_mutex_a = -1, g_test_mutex_b = -1;

static void task_cpu_load(void)
{
    volatile uint64_t x = 0;
    for (;;)
        x += 1;                 /* never yields: only preemption stops it */
}

static void task_sleeper(void)
{
    for (int i = 0; i < 30; i++)
        kthread_sleep(100);     /* 30 seconds, then exits by returning */
}

/* Two processes take the same two locks in opposite order — the classic
 * deadlock. The kernel refuses the lock that would close the cycle. */
static void task_deadlock_a(void)
{
    if (kmutex_lock(g_test_mutex_a) != 0) return;
    kthread_sleep(30);
    if (kmutex_lock(g_test_mutex_b) == 0) {
        kthread_sleep(50);
        kmutex_unlock(g_test_mutex_b);
    }
    kmutex_unlock(g_test_mutex_a);
}

static void task_deadlock_b(void)
{
    if (kmutex_lock(g_test_mutex_b) != 0) return;
    kthread_sleep(60);
    if (kmutex_lock(g_test_mutex_a) == 0)       /* refused: would deadlock */
        kmutex_unlock(g_test_mutex_a);
    kmutex_unlock(g_test_mutex_b);
}

/* Memory test. Runs in its own address space and exercises, in order:
 * demand paging, swap-out and swap-in, then memory protection (the final
 * write to a read-only page gets this process terminated by the kernel). */
#define MT_PAGES 64
static void task_memtest(void)
{
    char msg[96];
    int sp = vm_space_create(kthread_current_pid());
    if (sp < 0 || proc_use_space(sp) != 0) {
        scheduler_post_event("Memory test: could not create an address space");
        return;
    }

    uint64_t base = VM_USER_BASE;
    vm_region_add(sp, base, MT_PAGES, VM_WRITE, "heap");
    for (int i = 0; i < MT_PAGES; i++)                  /* each first touch faults */
        *(volatile uint64_t *)(base + (uint64_t)i * 4096) = 0xC0DE0000u + (uint64_t)i;

    int evicted = vm_reclaim(sp, MT_PAGES, 1);          /* push everything to swap */
    int intact = 1;
    for (int i = 0; i < MT_PAGES; i++)                  /* each read swaps back in */
        if (*(volatile uint64_t *)(base + (uint64_t)i * 4096) != 0xC0DE0000u + (uint64_t)i)
            intact = 0;

    snprintf(msg, sizeof(msg), "Memory test: %d pages demand-paged, %d swapped out and back, data %s",
             MT_PAGES, evicted, intact ? "intact" : "CORRUPT");
    scheduler_post_event(msg);
    kthread_sleep(300);

    uint64_t ro = base + 0x100000;
    vm_region_add(sp, ro, 1, 0, "read-only");
    volatile uint64_t *p = (volatile uint64_t *)ro;
    uint64_t v = *p;                                    /* reading is allowed */
    *p = v + 1;                                         /* writing is not     */
    scheduler_post_event("Memory test: PROTECTION FAILED (write to read-only page succeeded)");
}

/* Isolation test: two of these run at once, each in its own address space,
 * both using the same virtual address. Each must read back its own value. */
static void task_isolate(void)
{
    char msg[96];
    uint32_t pid = kthread_current_pid();
    int sp = vm_space_create(pid);
    if (sp < 0 || proc_use_space(sp) != 0) return;

    vm_region_add(sp, VM_USER_BASE, 1, VM_WRITE, "private");
    volatile uint64_t *p = (volatile uint64_t *)VM_USER_BASE;
    *p = pid;
    kthread_sleep(150);                                 /* let the other one run */
    snprintf(msg, sizeof(msg), "Isolation %s: PID %u wrote %u at 0x8000000000 and read back %u",
             *p == pid ? "OK" : "FAILED", pid, pid, (unsigned)*p);
    scheduler_post_event(msg);
}

/* Starts a named test workload. Returns the new PID, or 0. */
uint32_t sysmon_spawn_test(const char *kind)
{
    if (strcmp(kind, "cpu") == 0)
        return proc_spawn("cpu-load", task_cpu_load, PRIO_USER, 0);
    if (strcmp(kind, "sleeper") == 0)
        return proc_spawn("sleeper", task_sleeper, PRIO_USER, 0);
    if (strcmp(kind, "memory") == 0)
        return proc_spawn("mem-test", task_memtest, PRIO_USER, 0);
    if (strcmp(kind, "isolate") == 0) {
        proc_spawn("isolate-A", task_isolate, PRIO_USER, 0);
        return proc_spawn("isolate-B", task_isolate, PRIO_USER, 0);
    }
    if (strcmp(kind, "deadlock") == 0) {
        if (g_test_mutex_a < 0) g_test_mutex_a = kmutex_create("lock-A");
        if (g_test_mutex_b < 0) g_test_mutex_b = kmutex_create("lock-B");
        proc_spawn("deadlock-A", task_deadlock_a, PRIO_USER, 0);
        return proc_spawn("deadlock-B", task_deadlock_b, PRIO_USER, 0);
    }
    return 0;
}

const char *sysmon_state_name(const proc_info_t *p, char *buf, size_t n)
{
    if (p->flags & PROC_FLAG_SERVICE)      snprintf(buf, n, "Service");
    else if (p->suspended)                 snprintf(buf, n, "Suspended");
    else if (p->state == PROC_SLEEPING)    snprintf(buf, n, "Sleeping");
    else if (p->state == PROC_BLOCKED)     snprintf(buf, n, "Waiting: %s", p->waiting_on);
    else if (p->state == PROC_ZOMBIE)      snprintf(buf, n, "Exiting");
    else                                   snprintf(buf, n, "Running");
    if (p->flags & PROC_FLAG_USER) {            /* which CPU mode its code runs in */
        size_t len = strlen(buf);
        snprintf(buf + len, n - len, " (user)");
    }
    return buf;
}

/* ---- Sampling ------------------------------------------------------------ */
static void mon_sample(void)
{
    uint64_t now = g_kstate.uptime_ticks;
    uint64_t elapsed = now - g_sysmon.sample_tick;

    g_sysmon.proc_count = proc_list(g_sysmon.procs, MON_MAX_PROCS);
    if (elapsed < 100)
        return;

    uint32_t frames = desktop_frame_count();
    g_sysmon.fps = (uint32_t)((uint64_t)(frames - g_sysmon.sample_frames) * 100 / elapsed);
    g_sysmon.sample_frames = frames;

    uint64_t idle;
    scheduler_stats(&idle, NULL);
    g_sysmon.idle_pct = (uint32_t)((idle - g_sysmon.last_idle) * 100 / elapsed);
    if (g_sysmon.idle_pct > 100) g_sysmon.idle_pct = 100;
    g_sysmon.last_idle = idle;

    for (int i = 0; i < g_sysmon.proc_count; i++) {
        uint64_t before = g_sysmon.procs[i].cpu_ticks;     /* new process: 0% */
        for (int k = 0; k < g_sysmon.last_count; k++)
            if (g_sysmon.last_pid[k] == g_sysmon.procs[i].pid)
                before = g_sysmon.last_ticks[k];
        uint64_t pct = (g_sysmon.procs[i].cpu_ticks - before) * 100 / elapsed;
        g_sysmon.cpu_pct[i] = pct > 100 ? 100 : (uint32_t)pct;
    }
    for (int i = 0; i < g_sysmon.proc_count; i++) {
        g_sysmon.last_pid[i] = g_sysmon.procs[i].pid;
        g_sysmon.last_ticks[i] = g_sysmon.procs[i].cpu_ticks;
    }
    g_sysmon.last_count = g_sysmon.proc_count;
    g_sysmon.sample_tick = now;
}

static proc_info_t *mon_selected(void)
{
    for (int i = 0; i < g_sysmon.proc_count; i++)
        if (g_sysmon.procs[i].pid == g_sysmon.selected_pid)
            return &g_sysmon.procs[i];
    return NULL;
}

/* ---- Drawing ------------------------------------------------------------- */
static void mon_stat(int x, int y, int w, const char *label, const char *value)
{
    ui_card(x, y, w, 58);
    fb_draw_text(x + 12, y + 8, label, T(TEXT_DIM));
    fb_draw_text_bold(x + 12, y + 30, value, T(TEXT));
}

static void mon_device(int x, int y, const char *name, int ok, const char *detail)
{
    fb_fill_circle(x + 6, y + 8, 5, ok ? T(OK) : T(TEXT_DIM));
    fb_draw_text(x + 20, y, name, T(TEXT));
    fb_draw_text(x + 130, y, detail, T(TEXT_DIM));
}

static void draw_overview(int x, int y, int w)
{
    char buf[64];
    int gap = 8, cw = (w - 32 - gap * 2) / 3;

    sys_format_uptime(buf, sizeof(buf));
    mon_stat(x + 16, y + 12, cw, "Uptime", buf);
    snprintf(buf, sizeof(buf), "%ux%u", fb_get_width(), fb_get_height());
    mon_stat(x + 16 + cw + gap, y + 12, cw, "Display", buf);
    snprintf(buf, sizeof(buf), "%u fps", g_sysmon.fps);
    mon_stat(x + 16 + 2 * (cw + gap), y + 12, cw, "Redraw rate", buf);

    snprintf(buf, sizeof(buf), "%u%% busy", 100 - g_sysmon.idle_pct);
    mon_stat(x + 16, y + 78, cw, "CPU", buf);
    snprintf(buf, sizeof(buf), "%u MB", (unsigned)(g_kstate.model_pool_size >> 20));
    mon_stat(x + 16 + cw + gap, y + 78, cw, "AI model pool", buf);
    snprintf(buf, sizeof(buf), "%d found", g_kstate.pci_count);
    mon_stat(x + 16 + 2 * (cw + gap), y + 78, cw, "PCI devices", buf);

    uint64_t total, freef, heap_used, heap_total;
    vm_stats_t vm;
    governor_stats_t gov;
    pmm_get_stats(&total, &freef);
    heap_get_stats(&heap_used, NULL, &heap_total);
    vm_get_stats(&vm);
    governor_get_stats(&gov);

    snprintf(buf, sizeof(buf), "%u / %u MB", (unsigned)((total - freef) >> 8), (unsigned)(total >> 8));
    mon_stat(x + 16, y + 144, cw, "Memory in use", buf);
    snprintf(buf, sizeof(buf), "%u / %u KB", (unsigned)(heap_used >> 10), (unsigned)(heap_total >> 10));
    mon_stat(x + 16 + cw + gap, y + 144, cw, "Kernel heap", buf);
    snprintf(buf, sizeof(buf), "%u pages, %u faults", vm.swapped_pages,
             (unsigned)(vm.demand_faults + vm.swap_ins));
    mon_stat(x + 16 + 2 * (cw + gap), y + 144, cw, "Swap / page faults", buf);

    fb_draw_text(x + 16, y + 214, "Resource policy:", T(TEXT_DIM));
    fb_draw_text(x + 152, y + 214, gov.ai_online ? "managed by JARVIS (AI online)"
                 : "built-in rules (AI offline)", gov.ai_online ? T(ACCENT) : T(TEXT));

    int dy = y + 242;
    fb_draw_text_bold(x + 16, dy, "Devices and services", T(TEXT));
    dy += 26;
    mon_device(x + 16, dy, "AI brain", g_kstate.ai_ready,
               g_kstate.ai_ready ? "Online" : "Loading");                      dy += 22;
    mon_device(x + 16, dy, "Speech", g_kstate.speech_ready,
               g_kstate.speech_ready ? "Pipeline ready" : "Loading");          dy += 22;
    mon_device(x + 16, dy, "Network", g_kstate.network_ready,
               g_kstate.network_ready ? "Adapter up, no DHCP yet" : "No adapter"); dy += 22;
    mon_device(x + 16, dy, "Audio", g_kstate.audio_ready,
               g_kstate.audio_ready ? "Ready" : "No audio controller");        dy += 22;
    mon_device(x + 16, dy, "Microphone", g_kstate.mic_ready,
               g_kstate.mic_ready ? "Capturing" : "Not available");
}

static void draw_processes(int x, int y, int w, int h)
{
    static const char *const prio_names[4] = { "Realtime", "High", "Normal", "Low" };
    char buf[48];
    int c_pid = x + 16, c_name = x + 64, c_state = x + 200;
    int c_prio = x + w - 250, c_cpu = x + w - 160, c_time = x + w - 96;

    fb_draw_text(c_pid, y + 8, "PID", T(TEXT_DIM));
    fb_draw_text(c_name, y + 8, "Name", T(TEXT_DIM));
    fb_draw_text(c_state, y + 8, "Status", T(TEXT_DIM));
    fb_draw_text(c_prio, y + 8, "Priority", T(TEXT_DIM));
    fb_draw_text(c_cpu, y + 8, "CPU", T(TEXT_DIM));
    fb_draw_text(c_time, y + 8, "CPU time", T(TEXT_DIM));
    fb_fill_rect(x + 8, y + 30, w - 16, 1, T(BORDER));

    int list_h = h - 32 - MON_FOOTER_H;
    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x, y + 32, w, list_h);

    int ry = y + 34;
    for (int i = 0; i < g_sysmon.proc_count; i++, ry += MON_ROW_H) {
        proc_info_t *p = &g_sysmon.procs[i];
        uint32_t fg = T(TEXT), dim = T(TEXT_DIM);

        if (p->pid == g_sysmon.selected_pid) {
            fb_fill_rounded_rect(x + 8, ry, w - 16, MON_ROW_H - 2, 4, T(ACCENT));
            fg = dim = T(ACCENT_TEXT);
        }
        snprintf(buf, sizeof(buf), "%u", p->pid);
        fb_draw_text(c_pid, ry + 3, buf, dim);
        ui_text_fit(c_name, ry + 3, c_state - c_name - 8, p->name, fg);
        ui_text_fit(c_state, ry + 3, c_prio - c_state - 8,
                    sysmon_state_name(p, buf, sizeof(buf)), dim);
        fb_draw_text(c_prio, ry + 3, prio_names[p->priority & 3], dim);
        if (p->flags & PROC_FLAG_SERVICE) {
            fb_draw_text(c_cpu, ry + 3, "-", dim);
        } else {
            snprintf(buf, sizeof(buf), "%u%%", g_sysmon.cpu_pct[i]);
            fb_draw_text(c_cpu, ry + 3, buf, fg);
            snprintf(buf, sizeof(buf), "%u.%us", (unsigned)(p->cpu_ticks / 100),
                     (unsigned)((p->cpu_ticks / 10) % 10));
            fb_draw_text(c_time, ry + 3, buf, dim);
        }
    }
    fb_restore_clip(saved);

    /* Footer: actions on the selection, then test workloads */
    int fy = y + h - MON_FOOTER_H;
    proc_info_t *sel = mon_selected();
    int can_act = sel && !(sel->flags & (PROC_FLAG_SYSTEM | PROC_FLAG_SERVICE));
    uint32_t off_bg = T(SURFACE), off_fg = T(TEXT_DIM);

    fb_fill_rect(x, fy, w, 1, T(BORDER));
    ui_button(x + 16, fy + 10, 100, 28, "End task",
              can_act ? T(DANGER) : off_bg, can_act ? 0x00FFFFFF : off_fg);
    ui_button(x + 124, fy + 10, 100, 28, (sel && sel->suspended) ? "Resume" : "Suspend",
              can_act ? T(HOVER) : off_bg, can_act ? T(TEXT) : off_fg);
    snprintf(buf, sizeof(buf), "%d processes   CPU %u%%", g_sysmon.proc_count,
             100 - g_sysmon.idle_pct);
    fb_draw_text(x + w - 16 - fb_text_width(buf), fy + 16, buf, T(TEXT_DIM));

    fb_draw_text(x + 16, fy + 52, "Start test:", T(TEXT_DIM));
    ui_button(x + 112, fy + 46, 90, 28, "CPU load", T(HOVER), T(TEXT));
    ui_button(x + 210, fy + 46, 90, 28, "Sleeper", T(HOVER), T(TEXT));
    ui_button(x + 308, fy + 46, 90, 28, "Deadlock", T(HOVER), T(TEXT));
    ui_button(x + 406, fy + 46, 90, 28, "Memory", T(HOVER), T(TEXT));
    ui_button(x + 504, fy + 46, 90, 28, "Isolation", T(HOVER), T(TEXT));
}

void sysmon_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);

    mon_sample();

    /* Tabs */
    static const char *const tabs[2] = { "Processes", "Overview" };
    fb_fill_rect(x, y, w, MON_TAB_H, T(SURFACE));
    fb_fill_rect(x, y + MON_TAB_H - 1, w, 1, T(BORDER));
    for (int i = 0; i < 2; i++) {
        int tx = x + 12 + i * 112;
        if (g_sysmon.tab == i) {
            fb_fill_rounded_rect(tx, y + 6, 104, 28, 5, T(HOVER));
            fb_fill_rounded_rect(tx + 36, y + 31, 32, 3, 1, T(ACCENT));
        }
        ui_text_center(tx, y + 12, 104, tabs[i], T(TEXT));
    }

    if (g_sysmon.tab == 0)
        draw_processes(x, y + MON_TAB_H, w, h - MON_TAB_H);
    else
        draw_overview(x, y + MON_TAB_H, w);
}

void sysmon_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);

    if (event->type != IE_MOUSE_DOWN || event->data.mouse.button != 1)
        return;

    int mx = event->data.mouse.x, my = event->data.mouse.y;

    if (my < MON_TAB_H) {
        for (int i = 0; i < 2; i++)
            if (ui_hit(mx, my, 12 + i * 112, 6, 104, 28))
                g_sysmon.tab = i;
        return;
    }
    if (g_sysmon.tab != 0)
        return;

    int fy = h - MON_FOOTER_H;
    if (my >= fy) {
        proc_info_t *sel = mon_selected();
        if (ui_hit(mx, my, 16, fy + 10, 100, 28) && sel) {
            if (proc_kill(sel->pid) == 0) g_sysmon.selected_pid = 0;
        } else if (ui_hit(mx, my, 124, fy + 10, 100, 28) && sel) {
            if (sel->suspended) proc_resume(sel->pid);
            else                proc_suspend(sel->pid);
        } else if (ui_hit(mx, my, 112, fy + 46, 90, 28)) {
            g_sysmon.selected_pid = sysmon_spawn_test("cpu");
        } else if (ui_hit(mx, my, 210, fy + 46, 90, 28)) {
            g_sysmon.selected_pid = sysmon_spawn_test("sleeper");
        } else if (ui_hit(mx, my, 308, fy + 46, 90, 28)) {
            sysmon_spawn_test("deadlock");
        } else if (ui_hit(mx, my, 406, fy + 46, 90, 28)) {
            g_sysmon.selected_pid = sysmon_spawn_test("memory");
        } else if (ui_hit(mx, my, 504, fy + 46, 90, 28)) {
            sysmon_spawn_test("isolate");
        }
        return;
    }

    int row = (my - MON_TAB_H - 34) / MON_ROW_H;
    if (my >= MON_TAB_H + 34 && row >= 0 && row < g_sysmon.proc_count)
        g_sysmon.selected_pid = g_sysmon.procs[row].pid;
    else
        g_sysmon.selected_pid = 0;
}

wm_window_t *sysmon_app_open(int x, int y, int w, int h)
{
    memset(&g_sysmon, 0, sizeof(g_sysmon));
    g_sysmon.sample_tick = g_kstate.uptime_ticks;
    g_sysmon.sample_frames = desktop_frame_count();
    scheduler_stats(&g_sysmon.last_idle, NULL);
    g_sysmon.idle_pct = 100;

    wm_window_t *win = wm_create_window("Task Manager", x, y, w, h, WS_DEFAULT,
                                        sysmon_draw_callback, sysmon_event_callback);
    if (!win) return NULL;

    win->icon = ICON_CHART;
    win->min_w = 560;
    win->min_h = 360;
    g_sysmon.window = win;
    return win;
}
