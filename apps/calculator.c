/*
 * =============================================================================
 * JARVIS OS — Calculator (apps/calculator.c)
 * Language: C17
 *
 * Four-function calculator. Works with the mouse or the keyboard
 * (digits, + - * /, Enter or =, Backspace, Esc to clear).
 *
 * Numbers are entered as text and converted when an operator is pressed, so
 * decimals behave correctly (the old version ignored the decimal point).
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>

static struct {
    wm_window_t *window;
    char entry[20];         /* number being typed */
    int entry_len;
    int fresh;              /* next digit starts a new number */
    double acc;
    char op;                /* pending operator, 0 if none */
    int error;
} g_calc;

static const char *const g_calc_keys[5][4] = {
    { "C", "+/-", "%", "/" },
    { "7", "8",   "9", "*" },
    { "4", "5",   "6", "-" },
    { "1", "2",   "3", "+" },
    { "0", ".",   "<", "=" },
};

/* ---- Number <-> text (the kernel printf has no %f) ----------------------- */
static double calc_parse(const char *s)
{
    double v = 0, scale = 1;
    int neg = 0, frac = 0;

    if (*s == '-') { neg = 1; s++; }
    for (; *s; s++) {
        if (*s == '.') { frac = 1; continue; }
        if (*s < '0' || *s > '9') break;
        if (frac) { scale /= 10; v += (*s - '0') * scale; }
        else      { v = v * 10 + (*s - '0'); }
    }
    return neg ? -v : v;
}

static void calc_format(double v, char *out, size_t n)
{
    char tmp[40];
    int pos = 0;

    if (v < 0) { tmp[pos++] = '-'; v = -v; }
    if (v >= 1e15) {
        strncpy(out, "Overflow", n - 1);
        out[n - 1] = 0;
        return;
    }

    v += 0.0000005;                         /* round to 6 decimals */
    uint64_t ip = (uint64_t)v;
    uint64_t fp = (uint64_t)((v - (double)ip) * 1000000.0);

    char digits[20];
    int nd = 0;
    do { digits[nd++] = (char)('0' + ip % 10); ip /= 10; } while (ip && nd < 19);
    while (nd) tmp[pos++] = digits[--nd];

    if (fp) {
        char frac[7];
        for (int i = 5; i >= 0; i--) { frac[i] = (char)('0' + fp % 10); fp /= 10; }
        int last = 5;
        while (last >= 0 && frac[last] == '0') last--;
        tmp[pos++] = '.';
        for (int i = 0; i <= last; i++) tmp[pos++] = frac[i];
    }
    tmp[pos] = 0;
    strncpy(out, tmp, n - 1);
    out[n - 1] = 0;
}

static void calc_set_entry(double v)
{
    calc_format(v, g_calc.entry, sizeof(g_calc.entry));
    g_calc.entry_len = (int)strlen(g_calc.entry);
    g_calc.fresh = 1;
}

static void calc_clear(void)
{
    g_calc.entry[0] = '0';
    g_calc.entry[1] = 0;
    g_calc.entry_len = 1;
    g_calc.fresh = 1;
    g_calc.acc = 0;
    g_calc.op = 0;
    g_calc.error = 0;
}

static void calc_apply(void)
{
    double v = calc_parse(g_calc.entry);

    switch (g_calc.op) {
    case '+': g_calc.acc += v; break;
    case '-': g_calc.acc -= v; break;
    case '*': g_calc.acc *= v; break;
    case '/':
        if (v == 0) { g_calc.error = 1; return; }
        g_calc.acc /= v;
        break;
    default:  g_calc.acc = v; break;
    }
}

static void calc_press(const char *key)
{
    char k = key[0];

    if (g_calc.error && k != 'C')
        return;

    if (k >= '0' && k <= '9') {
        if (g_calc.fresh) { g_calc.entry_len = 0; g_calc.fresh = 0; }
        if (g_calc.entry_len == 1 && g_calc.entry[0] == '0') g_calc.entry_len = 0;
        if (g_calc.entry_len < 15) {
            g_calc.entry[g_calc.entry_len++] = k;
            g_calc.entry[g_calc.entry_len] = 0;
        }
    } else if (k == '.') {
        if (g_calc.fresh) {
            g_calc.entry[0] = '0'; g_calc.entry_len = 1; g_calc.fresh = 0;
        }
        if (!strchr(g_calc.entry, '.') && g_calc.entry_len < 14) {
            g_calc.entry[g_calc.entry_len++] = '.';
            g_calc.entry[g_calc.entry_len] = 0;
        }
    } else if (k == '<') {                      /* backspace */
        if (!g_calc.fresh && g_calc.entry_len > 0) {
            g_calc.entry[--g_calc.entry_len] = 0;
            if (g_calc.entry_len == 0 || strcmp(g_calc.entry, "-") == 0) {
                g_calc.entry[0] = '0'; g_calc.entry[1] = 0; g_calc.entry_len = 1;
            }
        }
    } else if (k == 'C') {
        calc_clear();
    } else if (key[1] == '/') {                 /* "+/-" */
        calc_set_entry(-calc_parse(g_calc.entry));
        g_calc.fresh = 0;
    } else if (k == '%') {
        calc_set_entry(calc_parse(g_calc.entry) / 100.0);
    } else if (k == '=') {
        calc_apply();
        g_calc.op = 0;
        if (!g_calc.error) calc_set_entry(g_calc.acc);
    } else if (k == '+' || k == '-' || k == '*' || k == '/') {
        if (!g_calc.fresh || !g_calc.op)
            calc_apply();
        g_calc.op = k;
        if (!g_calc.error) calc_set_entry(g_calc.acc);
    }
    desktop_invalidate();
}

/* Shared by drawing and hit-testing */
static void calc_layout(int cw, int ch, int *bw, int *bh, int *top)
{
    *top = 96;
    *bw = (cw - 16 - 3 * 4) / 4;
    *bh = (ch - *top - 8 - 4 * 4) / 5;
    if (*bh < 24) *bh = 24;
}

void calculator_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h, bw, bh, top;
    wm_get_client(win, &x, &y, &w, &h);
    calc_layout(w, h, &bw, &bh, &top);

    /* Display: pending operation on top, entry below (right aligned, 2x) */
    if (g_calc.op) {
        char pending[40];
        calc_format(g_calc.acc, pending, sizeof(pending) - 2);
        size_t n = strlen(pending);
        pending[n] = ' '; pending[n + 1] = g_calc.op; pending[n + 2] = 0;
        fb_draw_text(x + w - 16 - fb_text_width(pending), y + 14, pending, T(TEXT_DIM));
    }
    const char *shown = g_calc.error ? "Cannot divide by 0" : g_calc.entry;
    int scale = g_calc.error ? 1 : 2;
    fb_draw_text_scaled(x + w - 16 - fb_text_width_scaled(shown, scale), y + 44, shown,
                        g_calc.error ? T(DANGER) : T(TEXT), scale);

    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 4; c++) {
            const char *label = g_calc_keys[r][c];
            int bx = x + 8 + c * (bw + 4);
            int by = y + top + r * (bh + 4);
            int is_digit = (label[0] >= '0' && label[0] <= '9') || label[0] == '.';
            uint32_t bg = is_digit ? T(HOVER) : T(SURFACE);
            uint32_t fg = T(TEXT);

            if (label[0] == '=') { bg = T(ACCENT); fg = T(ACCENT_TEXT); }
            ui_button(bx, by, bw, bh, label[0] == '<' ? "<-" : label, bg, fg);
        }
    }
}

void calculator_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    if (event->type == IE_MOUSE_DOWN && event->data.mouse.button == 1) {
        int x, y, w, h, bw, bh, top;
        wm_get_client(win, &x, &y, &w, &h);
        calc_layout(w, h, &bw, &bh, &top);

        int mx = event->data.mouse.x - 8, my = event->data.mouse.y - top;
        if (mx < 0 || my < 0) return;
        int c = mx / (bw + 4), r = my / (bh + 4);
        if (c < 4 && r < 5 && mx % (bw + 4) < bw && my % (bh + 4) < bh)
            calc_press(g_calc_keys[r][c]);
        return;
    }

    if (event->type == IE_TEXT_INPUT) {
        char ch = event->data.text.ch;
        char key[2] = { ch, 0 };
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '+' || ch == '-' ||
            ch == '*' || ch == '/' || ch == '=' || ch == '%')
            calc_press(key);
        else if (ch == 'c' || ch == 'C')
            calc_press("C");
        return;
    }

    if (event->type == IE_KEY_DOWN) {
        uint32_t key = event->data.keyboard.key;
        if (key == WM_KEY_ENTER)          calc_press("=");
        else if (key == WM_KEY_BACKSPACE) calc_press("<");
        else if (key == WM_KEY_ESC)       calc_press("C");
    }
}

wm_window_t *calculator_app_open(int x, int y, int w, int h)
{
    memset(&g_calc, 0, sizeof(g_calc));
    calc_clear();

    wm_window_t *win = wm_create_window("Calculator", x, y, w, h, WS_DEFAULT,
                                        calculator_draw_callback,
                                        calculator_event_callback);
    if (!win) return NULL;

    win->icon = ICON_CALC;
    win->min_w = 260;
    win->min_h = 340;
    g_calc.window = win;
    return win;
}
