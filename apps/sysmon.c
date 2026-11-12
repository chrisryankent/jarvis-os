/*
 * =============================================================================
 * JARVIS OS — System monitor (apps/sysmon.c)
 * Language: C17
 *
 * Live view of the machine: uptime, display, render rate, and which devices
 * and AI subsystems came up at boot.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

uint32_t desktop_frame_count(void);     /* desktop/desktop_init.c */

static struct {
    wm_window_t *window;
    uint64_t sample_tick;
    uint32_t sample_frames;
    uint32_t fps;
} g_sysmon;

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

void sysmon_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    char buf[64];
    wm_get_client(win, &x, &y, &w, &h);

    /* Frames-per-second, sampled once a second */
    uint64_t now = g_kstate.uptime_ticks;
    if (now - g_sysmon.sample_tick >= 100) {
        uint32_t frames = desktop_frame_count();
        g_sysmon.fps = (uint32_t)((uint64_t)(frames - g_sysmon.sample_frames) * 100 /
                                  (now - g_sysmon.sample_tick));
        g_sysmon.sample_tick = now;
        g_sysmon.sample_frames = frames;
    }

    int gap = 8, cw = (w - 32 - gap * 2) / 3;
    sys_format_uptime(buf, sizeof(buf));
    mon_stat(x + 16, y + 16, cw, "Uptime", buf);
    snprintf(buf, sizeof(buf), "%ux%u", fb_get_width(), fb_get_height());
    mon_stat(x + 16 + cw + gap, y + 16, cw, "Display", buf);
    snprintf(buf, sizeof(buf), "%u fps", g_sysmon.fps);
    mon_stat(x + 16 + 2 * (cw + gap), y + 16, cw, "Redraw rate", buf);

    snprintf(buf, sizeof(buf), "%d open", wm_count_windows());
    mon_stat(x + 16, y + 82, cw, "Windows", buf);
    snprintf(buf, sizeof(buf), "%u MB", (unsigned)(g_kstate.model_pool_size >> 20));
    mon_stat(x + 16 + cw + gap, y + 82, cw, "AI model pool", buf);
    snprintf(buf, sizeof(buf), "%d found", g_kstate.pci_count);
    mon_stat(x + 16 + 2 * (cw + gap), y + 82, cw, "PCI devices", buf);

    int dy = y + 158;
    fb_draw_text_bold(x + 16, dy, "Devices and services", T(TEXT));
    dy += 28;
    mon_device(x + 16, dy, "AI brain", g_kstate.ai_ready,
               g_kstate.ai_ready ? "Online" : "Loading");                      dy += 22;
    mon_device(x + 16, dy, "Speech", g_kstate.speech_ready,
               g_kstate.speech_ready ? "Pipeline ready" : "Loading");          dy += 22;
    mon_device(x + 16, dy, "Network", g_kstate.network_ready,
               g_kstate.network_ready ? "Adapter up, no DHCP yet" : "No adapter"); dy += 22;
    mon_device(x + 16, dy, "Audio", g_kstate.audio_ready,
               g_kstate.audio_ready ? "Ready" : "No audio controller");        dy += 22;
    mon_device(x + 16, dy, "Microphone", g_kstate.mic_ready,
               g_kstate.mic_ready ? "Capturing" : "Not available");            dy += 22;
    mon_device(x + 16, dy, "USB", g_kstate.usb_ready,
               g_kstate.usb_ready ? "Host controller present" : "No controller"); dy += 22;
    mon_device(x + 16, dy, "Camera", g_kstate.camera_ready,
               g_kstate.camera_ready ? "Ready" : "Not available");
}

wm_window_t *sysmon_app_open(int x, int y, int w, int h)
{
    memset(&g_sysmon, 0, sizeof(g_sysmon));
    g_sysmon.sample_tick = g_kstate.uptime_ticks;
    g_sysmon.sample_frames = desktop_frame_count();

    wm_window_t *win = wm_create_window("System", x, y, w, h, WS_DEFAULT,
                                        sysmon_draw_callback, NULL);
    if (!win) return NULL;

    win->icon = ICON_CHART;
    win->min_w = 440;
    win->min_h = 300;
    g_sysmon.window = win;
    return win;
}
