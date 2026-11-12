/*
 * =============================================================================
 * JARVIS OS — Calculator Application (apps/calculator.c)
 * Language: C17
 *
 * Simple GUI calculator for basic arithmetic operations.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../drivers/gpu.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    wm_window_t *window;
    double value;
    double accumulator;
    char current_op;
    char display[32];
    int decimal_mode;
    double decimal_divisor;  /* tracks place: 0.1, 0.01, 0.001 … */
} calculator_context_t;

static calculator_context_t g_calc = {};

/* ---- Forward Declarations ------------------------------------------------ */
static void calculator_equals(void);

void calculator_update_display(void)
{
    if (g_calc.decimal_mode) {
        snprintf(g_calc.display, sizeof(g_calc.display), "%.6f", g_calc.value);
    } else {
        snprintf(g_calc.display, sizeof(g_calc.display), "%lld", (long long)g_calc.value);
    }
}

void calculator_clear(void)
{
    g_calc.value = 0;
    g_calc.accumulator = 0;
    g_calc.current_op = '\0';
    g_calc.decimal_mode = 0;
    strcpy(g_calc.display, "0");
}

void calculator_input_digit(int digit)
{
    if (g_calc.decimal_mode) {
        g_calc.value = g_calc.value * 10 + digit;
    } else {
        g_calc.value = g_calc.value * 10 + digit;
    }
    calculator_update_display();
}

void calculator_decimal(void)
{
    g_calc.decimal_mode = 1;
}

void calculator_operation(char op)
{
    if (g_calc.current_op != '\0') {
        calculator_equals();
    }
    g_calc.accumulator = g_calc.value;
    g_calc.current_op = op;
    g_calc.value = 0;
    g_calc.decimal_mode = 0;
}

void calculator_equals(void)
{
    switch (g_calc.current_op) {
    case '+':
        g_calc.value = g_calc.accumulator + g_calc.value;
        break;
    case '-':
        g_calc.value = g_calc.accumulator - g_calc.value;
        break;
    case '*':
        g_calc.value = g_calc.accumulator * g_calc.value;
        break;
    case '/':
        if (g_calc.value != 0)
            g_calc.value = g_calc.accumulator / g_calc.value;
        break;
    }
    g_calc.current_op = '\0';
    g_calc.decimal_mode = 0;
    calculator_update_display();
}

void calculator_draw_button(int x, int y, int w, int h,
                           const char *label, uint32_t color)
{
    fb_fill_rect(x, y, w, h, color);
    fb_fill_rect(x, y, w, 1, 0x00505050);
    int label_x = x + (w - 8) / 2;
    int label_y = y + (h - 16) / 2;
    fb_put_string(label_x, label_y, label, 0x00FFFFFF, color);
}

void calculator_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    if (!win) return;

    fb_fill_rect(win->x, win->y + 32, win->w, win->h - 32, 0x00282828);

    int x = win->x + 20;
    int y = win->y + 50;

    /* Display */
    fb_fill_rect(x, y, win->w - 40, 50, 0x00000000);
    fb_put_string(x + 8, y + 14, g_calc.display, 0x0000FF00, 0x00000000);

    y += 70;

    /* Button grid: 4 columns x 5 rows */
    int btn_w = (win->w - 40 - 12) / 4;
    int btn_h = 45;
    int btn_gap = 4;

    const char *buttons[5][4] = {
        {"7", "8", "9", "/"},
        {"4", "5", "6", "*"},
        {"1", "2", "3", "-"},
        {"0", ".", "=", "+"},
        {"C", "←", "√", "%"}
    };

    uint32_t op_color = 0x005BA3D6;
    uint32_t num_color = 0x003A3A3A;
    uint32_t eq_color = 0x0000E676;
    uint32_t clr_color = 0x00FF6B6B;

    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            int btn_x = x + col * (btn_w + btn_gap);
            int btn_y = y + row * (btn_h + btn_gap);
            const char *label = buttons[row][col];

            uint32_t color = num_color;
            if (strchr("+-*/", label[0]))
                color = op_color;
            else if (label[0] == '=')
                color = eq_color;
            else if (label[0] == 'C')
                color = clr_color;

            calculator_draw_button(btn_x, btn_y, btn_w, btn_h, label, color);
        }
    }
}

void calculator_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    if (!event) return;

    if (event->type == IE_MOUSE_DOWN) {
        int click_x = event->data.mouse.x - (win->x + 20);
        int click_y = event->data.mouse.y - (win->y + 120);

        if (click_x < 0 || click_y < 0)
            return;

        int btn_w = (win->w - 40 - 12) / 4;
        int btn_h = 45;
        int btn_gap = 4;

        int col = click_x / (btn_w + btn_gap);
        int row = click_y / (btn_h + btn_gap);

        if (col >= 0 && col < 4 && row >= 0 && row < 5) {
            const char *buttons[5][4] = {
                {"7", "8", "9", "/"},
                {"4", "5", "6", "*"},
                {"1", "2", "3", "-"},
                {"0", ".", "=", "+"},
                {"C", "←", "√", "%"}
            };

            const char *label = buttons[row][col];

            if (label[0] >= '0' && label[0] <= '9') {
                calculator_input_digit(label[0] - '0');
            } else if (label[0] == '.') {
                calculator_decimal();
            } else if (strchr("+-*/", label[0])) {
                calculator_operation(label[0]);
            } else if (label[0] == '=') {
                calculator_equals();
            } else if (label[0] == 'C') {
                calculator_clear();
            }
        }
    }
}

wm_window_t *calculator_app_open(int x, int y, int w, int h)
{
    memset(&g_calc, 0, sizeof(g_calc));
    calculator_clear();

    wm_window_t *win = wm_create_window(
        "Calculator",
        x, y, w, h,
        WS_DEFAULT,
        calculator_draw_callback,
        calculator_event_callback
    );

    g_calc.window = win;
    return win;
}
