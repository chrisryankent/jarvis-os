/*
 * =============================================================================
 * JARVIS OS — Assistant window (apps/assistant.c)
 * Language: C17
 *
 * The conversation with JARVIS: everything heard through the voice pipeline
 * and every reply from the AI core, plus a text box so the assistant can be
 * used without a microphone. Typed text takes the same path as speech
 * (IPC_VOICE_INPUT), so voice commands such as "open terminal" work here too.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>

#define AS_HEADER_H  64
#define AS_CHIPS_H   34
#define AS_INPUT_H   48

static struct {
    wm_window_t *window;
    char input[120];
    int input_len;
} g_assistant;

static const char *const g_as_chips[] = {
    "help", "status", "open terminal", "open files",
};
#define AS_CHIPS ((int)(sizeof(g_as_chips) / sizeof(g_as_chips[0])))

static int chip_x(int i)
{
    int x = 12;
    for (int k = 0; k < i; k++)
        x += fb_text_width(g_as_chips[k]) + 24 + 8;
    return x;
}

static void assistant_submit(void)
{
    if (g_assistant.input_len == 0) return;
    desktop_ask_jarvis(g_assistant.input);
    g_assistant.input[0] = 0;
    g_assistant.input_len = 0;
}

void assistant_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);

    /* ---- Header: orb, status, mic level ---- */
    fb_fill_rect(x, y, w, AS_HEADER_H, T(SURFACE));
    fb_fill_rect(x, y + AS_HEADER_H - 1, w, 1, T(BORDER));
    icon_draw(ICON_JARVIS, x + 14, y + 12, 40);
    fb_draw_text_bold(x + 66, y + 14, "JARVIS", T(TEXT));
    fb_draw_text(x + 66, y + 34, ui_shell_get_status(), T(ACCENT));
    {
        char brain[64];
        if (ai_host_online()) snprintf(brain, sizeof(brain), "AI: %s", ai_host_model());
        else snprintf(brain, sizeof(brain), "AI offline: built-in commands");
        ui_text_fit(x + 66 + fb_text_width(ui_shell_get_status()) + 16, y + 34,
                    w - 66 - fb_text_width(ui_shell_get_status()) - 16 - 160, brain, T(TEXT_DIM));
    }

    int mx = x + w - 150;
    if (mx > x + 190) {
        glyph_draw(GLYPH_MIC, mx, y + 14, 16,
                   g_kstate.mic_ready ? T(TEXT) : T(TEXT_DIM), T(SURFACE));
        ui_progress(mx + 24, y + 18, 110, 8, ui_shell_get_mic_level(), T(HOVER), T(ACCENT));
        fb_draw_text(mx, y + 36, g_kstate.mic_ready ? "Listening" : "No microphone", T(TEXT_DIM));
    }

    /* ---- Conversation (newest at the bottom, long lines wrapped) ---- */
    int chat_y = y + AS_HEADER_H + 8;
    int chat_h = h - AS_HEADER_H - AS_CHIPS_H - AS_INPUT_H - 16;
    int cols = (w - 32) / 8;
    if (cols < 8) cols = 8;
    if (cols > 120) cols = 120;

    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x, chat_y, w, chat_h);

    int count = ui_shell_line_count();
    int row_y = chat_y + chat_h;
    for (int i = count - 1; i >= 0 && row_y > chat_y; i--) {
        const char *line = ui_shell_get_line(i);
        int len = (int)strlen(line);
        int rows = len ? (len + cols - 1) / cols : 1;
        int is_user = strncmp(line, "You: ", 5) == 0;
        uint32_t fg = is_user ? T(ACCENT) : T(TEXT);

        row_y -= rows * 18 + 4;
        for (int r = 0; r < rows; r++) {
            char seg[128];
            int n = len - r * cols;
            if (n > cols) n = cols;
            if (n < 0) n = 0;
            memcpy(seg, line + r * cols, (size_t)n);
            seg[n] = 0;
            fb_draw_text(x + 16, row_y + r * 18, seg, fg);
        }
    }
    if (count == 0)
        ui_text_center(x, chat_y + chat_h / 2 - 8, w, "Say something, or type below.", T(TEXT_DIM));
    fb_restore_clip(saved);

    /* ---- Suggestion chips ---- */
    int cy = y + h - AS_INPUT_H - AS_CHIPS_H;
    for (int i = 0; i < AS_CHIPS; i++) {
        int cw = fb_text_width(g_as_chips[i]) + 24;
        int cx = x + chip_x(i);
        if (cx + cw > x + w - 8) break;
        fb_fill_rounded_rect(cx, cy, cw, 26, 13, T(BORDER));
        fb_fill_rounded_rect(cx + 1, cy + 1, cw - 2, 24, 12, T(SURFACE));
        fb_draw_text(cx + 12, cy + 5, g_as_chips[i], T(TEXT));
    }

    /* ---- Input box ---- */
    int iy = y + h - AS_INPUT_H + 4;
    int send_w = 64;
    fb_fill_rounded_rect(x + 12, iy, w - 24 - send_w - 8, 36, 6, T(INPUT));
    fb_fill_rect(x + 18, iy + 35, w - 36 - send_w - 8, 1,
                 g_wm_ctx.focused == win ? T(ACCENT) : T(BORDER));
    if (g_assistant.input_len) {
        fb_clip_t c2 = fb_get_clip();
        fb_set_clip(x + 20, iy, w - 48 - send_w - 8, 36);
        int tw = fb_text_width(g_assistant.input);
        int avail = w - 56 - send_w - 8;
        int tx = x + 22 - (tw > avail ? tw - avail : 0);     /* keep the caret visible */
        fb_draw_text(tx, iy + 10, g_assistant.input, T(TEXT));
        fb_fill_rect(tx + tw + 1, iy + 10, 1, 16, T(TEXT));
        fb_restore_clip(c2);
    } else {
        fb_draw_text(x + 22, iy + 10, "Ask JARVIS anything...", T(TEXT_DIM));
    }
    ui_button(x + w - 12 - send_w, iy, send_w, 36, "Send", T(ACCENT), T(ACCENT_TEXT));

    /* ---- An action waiting for the user's permission ---- */
    char ask[160];
    if (ai_tools_pending(ask, sizeof(ask))) {
        int pw = w - 40 < 380 ? w - 40 : 380, ph = 130;
        int px = x + (w - pw) / 2, py = y + (h - ph) / 2;
        fb_blend_rect(x, y, w, h, 0x00000000, 110);
        fb_fill_rounded_rect(px, py, pw, ph, 8, T(WARN));
        fb_fill_rounded_rect(px + 2, py + 2, pw - 4, ph - 4, 7, T(MENU));
        fb_draw_text_bold(px + 16, py + 14, "Permission needed", T(TEXT));
        ui_text_fit(px + 16, py + 40, pw - 32, ask, T(TEXT));
        ui_button(px + pw - 196, py + ph - 44, 90, 30, "Allow", T(ACCENT), T(ACCENT_TEXT));
        ui_button(px + pw - 98, py + ph - 44, 82, 30, "Deny", T(HOVER), T(TEXT));
    }
}

void assistant_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);

    char ask[160];
    if (ai_tools_pending(ask, sizeof(ask))) {           /* Allow / Deny prompt is modal */
        int pw = w - 40 < 380 ? w - 40 : 380, ph = 130, px = (w - pw) / 2, py = (h - ph) / 2;
        if (event->type == IE_MOUSE_DOWN && event->data.mouse.button == 1) {
            int mx = event->data.mouse.x, my = event->data.mouse.y;
            if (ui_hit(mx, my, px + pw - 196, py + ph - 44, 90, 30)) ai_tools_confirm(1);
            else if (ui_hit(mx, my, px + pw - 98, py + ph - 44, 82, 30)) ai_tools_confirm(0);
        } else if (event->type == IE_KEY_DOWN && event->data.keyboard.key == WM_KEY_ESC) {
            ai_tools_confirm(0);
        }
        return;
    }

    if (event->type == IE_TEXT_INPUT) {
        char ch = event->data.text.ch;
        if (ch >= 32 && ch < 127 && g_assistant.input_len < (int)sizeof(g_assistant.input) - 1) {
            g_assistant.input[g_assistant.input_len++] = ch;
            g_assistant.input[g_assistant.input_len] = 0;
        }
    } else if (event->type == IE_KEY_DOWN) {
        uint32_t key = event->data.keyboard.key;
        if (key == WM_KEY_ENTER) {
            assistant_submit();
        } else if (key == WM_KEY_BACKSPACE) {
            if (g_assistant.input_len > 0)
                g_assistant.input[--g_assistant.input_len] = 0;
        } else if (key == WM_KEY_ESC) {
            g_assistant.input[0] = 0;
            g_assistant.input_len = 0;
        }
    } else if (event->type == IE_MOUSE_DOWN && event->data.mouse.button == 1) {
        int mx = event->data.mouse.x, my = event->data.mouse.y;
        int cy = h - AS_INPUT_H - AS_CHIPS_H;

        if (my >= cy && my < cy + 26) {
            for (int i = 0; i < AS_CHIPS; i++) {
                int cw = fb_text_width(g_as_chips[i]) + 24;
                if (ui_hit(mx, my, chip_x(i), cy, cw, 26)) {
                    desktop_ask_jarvis(g_as_chips[i]);
                    return;
                }
            }
        }
        if (ui_hit(mx, my, w - 12 - 64, h - AS_INPUT_H + 4, 64, 36))
            assistant_submit();
    }
}

wm_window_t *assistant_app_open(int x, int y, int w, int h)
{
    memset(&g_assistant, 0, sizeof(g_assistant));

    wm_window_t *win = wm_create_window("JARVIS", x, y, w, h, WS_DEFAULT,
                                        assistant_draw_callback,
                                        assistant_event_callback);
    if (!win) return NULL;

    win->icon = ICON_JARVIS;
    win->min_w = 340;
    win->min_h = 300;
    g_assistant.window = win;
    return win;
}
