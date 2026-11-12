/*
 * =============================================================================
 * JARVIS OS — Input Event System (input/input_events.c)
 * Language: C17
 *
 * Unified keyboard/mouse input handling for window manager.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../drivers/gpu.h"
#include <string.h>

#define INPUT_QUEUE_SIZE 256

typedef struct {
    wm_input_event_t queue[INPUT_QUEUE_SIZE];
    int head;
    int tail;
} input_queue_t;

static input_queue_t g_input_queue = {};
static int g_mouse_x = 0, g_mouse_y = 0;

void input_queue_init(void)
{
    memset(&g_input_queue, 0, sizeof(g_input_queue));
    g_mouse_x = fb_get_width() / 2;
    g_mouse_y = fb_get_height() / 2;
}

void input_post_event(wm_input_event_t *event)
{
    if (!event) return;

    int next_head = (g_input_queue.head + 1) % INPUT_QUEUE_SIZE;
    if (next_head == g_input_queue.tail)
        return;  /* Queue full, drop event */

    g_input_queue.queue[g_input_queue.head] = *event;
    g_input_queue.head = next_head;
}

int input_has_event(void)
{
    return g_input_queue.head != g_input_queue.tail;
}

wm_input_event_t input_get_event(void)
{
    wm_input_event_t event = {};

    if (g_input_queue.head != g_input_queue.tail) {
        event = g_input_queue.queue[g_input_queue.tail];
        g_input_queue.tail = (g_input_queue.tail + 1) % INPUT_QUEUE_SIZE;
    }

    return event;
}

void input_mouse_move(int x, int y)
{
    g_mouse_x = x;
    g_mouse_y = y;

    wm_input_event_t event;
    event.type = IE_MOUSE_MOVE;
    event.data.mouse.x = x;
    event.data.mouse.y = y;
    event.timestamp = g_kstate.uptime_ticks;
    input_post_event(&event);
}

void input_mouse_click(int x, int y, int button)
{
    wm_input_event_t event;
    event.type = IE_MOUSE_DOWN;
    event.data.mouse.x = x;
    event.data.mouse.y = y;
    event.data.mouse.button = button;
    event.timestamp = g_kstate.uptime_ticks;
    input_post_event(&event);
}

void input_mouse_release(int x, int y, int button)
{
    wm_input_event_t event;
    event.type = IE_MOUSE_UP;
    event.data.mouse.x = x;
    event.data.mouse.y = y;
    event.data.mouse.button = button;
    event.timestamp = g_kstate.uptime_ticks;
    input_post_event(&event);
}

void input_key_press(uint32_t key, int mods)
{
    wm_input_event_t event;
    event.type = IE_KEY_DOWN;
    event.data.keyboard.key = key;
    event.data.keyboard.mods = mods;
    event.timestamp = g_kstate.uptime_ticks;
    input_post_event(&event);
}

void input_key_release(uint32_t key, int mods)
{
    wm_input_event_t event;
    event.type = IE_KEY_UP;
    event.data.keyboard.key = key;
    event.data.keyboard.mods = mods;
    event.timestamp = g_kstate.uptime_ticks;
    input_post_event(&event);
}

void input_text_char(char ch)
{
    wm_input_event_t event;
    event.type = IE_TEXT_INPUT;
    event.data.text.ch = ch;
    event.timestamp = g_kstate.uptime_ticks;
    input_post_event(&event);
}

/* desktop/desktop_init.c: global shortcuts, Start menu typing, lock screen,
 * then the focused window. */
void desktop_dispatch_event(wm_input_event_t *event);

void input_process_queue(void)
{
    while (input_has_event()) {
        wm_input_event_t event = input_get_event();
        desktop_dispatch_event(&event);
    }
}

int input_get_mouse_x(void) { return g_mouse_x; }
int input_get_mouse_y(void) { return g_mouse_y; }
