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

void input_process_queue(void)
{
    while (input_has_event()) {
        wm_input_event_t event = input_get_event();
        wm_handle_input(&event);
    }
}

int input_get_mouse_x(void) { return g_mouse_x; }
int input_get_mouse_y(void) { return g_mouse_y; }

/* Translate PS/2 set-1 scancode + modifier flags to an ASCII character.
 * Returns 0 for non-printable keys (Enter, Backspace, arrows, etc.). */
char input_scancode_to_char(uint32_t sc, int mods)
{
    int shift = !!(mods & KMOD_SHIFT);
    static const struct { uint32_t sc; char lo; char hi; } tbl[] = {
        {0x02,'1','!'},{0x03,'2','@'},{0x04,'3','#'},{0x05,'4','$'},
        {0x06,'5','%'},{0x07,'6','^'},{0x08,'7','&'},{0x09,'8','*'},
        {0x0A,'9','('},{0x0B,'0',')'},{0x0C,'-','_'},{0x0D,'=','+'},
        {0x10,'q','Q'},{0x11,'w','W'},{0x12,'e','E'},{0x13,'r','R'},
        {0x14,'t','T'},{0x15,'y','Y'},{0x16,'u','U'},{0x17,'i','I'},
        {0x18,'o','O'},{0x19,'p','P'},{0x1A,'[','{'},{0x1B,']','}'},
        {0x1E,'a','A'},{0x1F,'s','S'},{0x20,'d','D'},{0x21,'f','F'},
        {0x22,'g','G'},{0x23,'h','H'},{0x24,'j','J'},{0x25,'k','K'},
        {0x26,'l','L'},{0x27,';',':'},{0x28,'\'','"'},{0x29,'`','~'},
        {0x2B,'\\','|'},
        {0x2C,'z','Z'},{0x2D,'x','X'},{0x2E,'c','C'},{0x2F,'v','V'},
        {0x30,'b','B'},{0x31,'n','N'},{0x32,'m','M'},{0x33,',','<'},
        {0x34,'.','>'},{0x35,'/','?'},{0x39,' ',' '},
    };
    for (size_t i = 0; i < sizeof(tbl)/sizeof(tbl[0]); i++)
        if (tbl[i].sc == sc)
            return shift ? tbl[i].hi : tbl[i].lo;
    return 0;
}
