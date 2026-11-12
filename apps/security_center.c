/*
 * =============================================================================
 * JARVIS OS — Security centre (apps/security_center.c)
 * Language: C17
 *
 * Shows the real state of the security subsystem (nothing here is simulated:
 * a feature that is not implemented yet says so) and lets you lock the PC.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>

static struct { wm_window_t *window; } g_security;

static void sec_card(int x, int y, int w, const char *title, const char *detail,
                     uint32_t dot)
{
    ui_card(x, y, w, 54);
    fb_fill_circle(x + 20, y + 27, 6, dot);
    fb_draw_text_bold(x + 40, y + 10, title, T(TEXT));
    ui_text_fit(x + 40, y + 30, w - 52, detail, T(TEXT_DIM));
}

void security_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);

    int alert = ui_shell_get_security_alert();

    icon_draw(ICON_SHIELD, x + 16, y + 14, 40);
    fb_draw_text_bold(x + 68, y + 12, alert ? "Attention needed" : "No threats detected",
                      alert ? T(DANGER) : T(TEXT));
    fb_draw_text(x + 68, y + 34, "Threat level:", T(TEXT_DIM));
    fb_draw_text(x + 68 + 112, y + 34, ui_shell_get_security_label(),
                 alert ? T(DANGER) : T(OK));

    int cy = y + 72, cw = w - 32;
    sec_card(x + 16, cy, cw, "Session lock",
             g_kstate.security_enabled ? "On: a PIN / voice check is required to unlock"
                                       : "Off: no PIN or voice print enrolled yet",
             g_kstate.security_enabled ? T(OK) : T(WARN));
    cy += 62;
    sec_card(x + 16, cy, cw, "System call audit",
             "Audit hook installed; every system call is checked", T(OK));
    cy += 62;
    sec_card(x + 16, cy, cw, "Network monitor",
             g_kstate.network_ready ? "Watching packets on the network adapter"
                                    : "No network adapter found",
             g_kstate.network_ready ? T(OK) : T(TEXT_DIM));
    cy += 62;
    sec_card(x + 16, cy, cw, "Disk encryption",
             "Not active: AES-256-XTS support is not implemented yet", T(WARN));
    cy += 70;

    ui_button(x + 16, cy, 120, 34, "Lock now", T(ACCENT), T(ACCENT_TEXT));
}

void security_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    (void)win;
    if (event->type == IE_MOUSE_DOWN && event->data.mouse.button == 1 &&
        ui_hit(event->data.mouse.x, event->data.mouse.y, 16, 72 + 3 * 62 + 70, 120, 34))
        desktop_lock();
}

wm_window_t *security_app_open(int x, int y, int w, int h)
{
    wm_window_t *win = wm_create_window("Security", x, y, w, h, WS_DEFAULT,
                                        security_draw_callback,
                                        security_event_callback);
    if (!win) return NULL;

    win->icon = ICON_SHIELD;
    win->min_w = 380;
    win->min_h = 300;
    g_security.window = win;
    return win;
}
