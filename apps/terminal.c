/*
 * =============================================================================
 * JARVIS OS — Terminal Application (apps/terminal.c)
 * Language: C17
 *
 * Terminal emulator with full PS/2 keyboard support, command history, and
 * built-in command dispatch (help, ls, clear, echo, status, open, theme,
 * notify, uptime).
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../drivers/gpu.h"
#include <string.h>
#include <stdio.h>

#define TERM_ROWS       32
#define TERM_COLS       80
#define CHAR_WIDTH      8
#define CHAR_HEIGHT     16
#define HISTORY_MAX     16

/* Forward declaration — desktop_system_open_app() is in desktop_init.c which
 * wraps this unity build, but it appears after all #includes so we need this
 * to avoid an implicit-function-declaration warning. */
void desktop_system_open_app(const char *app);

/* input_scancode_to_char() is defined in input/input_events.c, which is
 * #included before this file in the unity build (desktop_init.c). */
char input_scancode_to_char(uint32_t sc, int mods);

typedef struct {
    wm_window_t *window;
    char buffer[TERM_ROWS][TERM_COLS];
    char input_line[256];
    int input_pos;
    int cursor_x, cursor_y;
    int scroll_offset;
    uint32_t bg_color;
    uint32_t fg_color;

    char history[HISTORY_MAX][256];
    int history_count;
    int history_pos;        /* -1 = not browsing, 0..N-1 = browsing */
    char history_draft[256]; /* saved current input while browsing */
} terminal_context_t;

static terminal_context_t g_terminal = {};

/* ---- Output ---------------------------------------------------------------- */
void terminal_app_clear(void)
{
    memset(g_terminal.buffer, 0, sizeof(g_terminal.buffer));
    g_terminal.cursor_y = 0;
    g_terminal.cursor_x = 0;
}

void terminal_app_put_char(char c)
{
    if (c == '\n') {
        g_terminal.cursor_y++;
        g_terminal.cursor_x = 0;
        if (g_terminal.cursor_y >= TERM_ROWS) {
            for (int row = 0; row < TERM_ROWS - 1; row++)
                memcpy(g_terminal.buffer[row], g_terminal.buffer[row + 1], TERM_COLS);
            memset(g_terminal.buffer[TERM_ROWS - 1], 0, TERM_COLS);
            g_terminal.cursor_y = TERM_ROWS - 1;
        }
    } else if (c == '\r') {
        g_terminal.cursor_x = 0;
    } else if (c == '\b') {
        if (g_terminal.cursor_x > 0) {
            g_terminal.cursor_x--;
            g_terminal.buffer[g_terminal.cursor_y][g_terminal.cursor_x] = ' ';
        }
    } else if ((uint8_t)c >= 32 && (uint8_t)c < 127) {
        if (g_terminal.cursor_x < TERM_COLS - 1) {
            g_terminal.buffer[g_terminal.cursor_y][g_terminal.cursor_x] = c;
            g_terminal.cursor_x++;
        }
    }
}

void terminal_app_write(const char *text)
{
    while (*text)
        terminal_app_put_char(*text++);
}

/* ---- Drawing --------------------------------------------------------------- */
void terminal_app_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    if (!win) return;

    int x = win->x + 8;
    int y = win->y + 40;

    fb_fill_rect(win->x, win->y + 32, win->w, win->h - 32, 0x00000000);

    for (int row = 0; row < TERM_ROWS; row++) {
        for (int col = 0; col < TERM_COLS; col++) {
            char c = g_terminal.buffer[row][col];
            if (c != '\0')
                fb_put_char(x + col * CHAR_WIDTH, y + row * CHAR_HEIGHT,
                            c, 0x00FFFFFF, 0x00000000);
        }
    }

    /* Blinking block cursor */
    int blink = (g_kstate.uptime_ticks / 25) % 2;
    if (blink)
        fb_fill_rect(x + g_terminal.cursor_x * CHAR_WIDTH,
                     y + g_terminal.cursor_y * CHAR_HEIGHT,
                     CHAR_WIDTH, CHAR_HEIGHT / 2, 0x00FFFFFF);

    /* Input line */
    int input_y = win->y + win->h - 42;
    fb_fill_rect(win->x, input_y - 2, win->w, CHAR_HEIGHT + 8, 0x00111111);
    fb_put_char(win->x + 4, input_y, '>', 0x0000FF00, 0x00111111);
    fb_put_string(win->x + 16, input_y, g_terminal.input_line, 0x00FFFFFF, 0x00111111);

    /* Status bar */
    char status[128];
    snprintf(status, sizeof(status), "Terminal | AI: %s | Voice: %s",
        g_kstate.ai_ready ? "ready" : "loading",
        g_kstate.speech_ready ? "ready" : "loading");
    fb_fill_rect(win->x, win->y + win->h - 24, win->w, 24, 0x001A1A1A);
    fb_put_string(win->x + 8, win->y + win->h - 18, status, 0x00808080, 0x001A1A1A);
}

/* ---- Command History ------------------------------------------------------- */
static void history_push(const char *cmd)
{
    if (strlen(cmd) == 0)
        return;

    /* Don't push duplicate of last entry */
    if (g_terminal.history_count > 0) {
        int last = (g_terminal.history_count - 1) % HISTORY_MAX;
        if (strcmp(g_terminal.history[last], cmd) == 0)
            return;
    }

    int slot = g_terminal.history_count % HISTORY_MAX;
    strncpy(g_terminal.history[slot], cmd, sizeof(g_terminal.history[0]) - 1);
    g_terminal.history[slot][sizeof(g_terminal.history[0]) - 1] = '\0';
    g_terminal.history_count++;
}

static void history_up(void)
{
    int count = g_terminal.history_count;
    if (count == 0)
        return;

    if (g_terminal.history_pos < 0) {
        /* Start browsing — save current draft */
        strncpy(g_terminal.history_draft, g_terminal.input_line,
                sizeof(g_terminal.history_draft) - 1);
        g_terminal.history_pos = 0;
    } else if (g_terminal.history_pos < count - 1 && g_terminal.history_pos < HISTORY_MAX - 1) {
        g_terminal.history_pos++;
    } else {
        return;
    }

    int slot = (count - 1 - g_terminal.history_pos) % HISTORY_MAX;
    if (slot < 0)
        slot += HISTORY_MAX;
    strncpy(g_terminal.input_line, g_terminal.history[slot],
            sizeof(g_terminal.input_line) - 1);
    g_terminal.input_line[sizeof(g_terminal.input_line) - 1] = '\0';
    g_terminal.input_pos = (int)strlen(g_terminal.input_line);
}

static void history_down(void)
{
    if (g_terminal.history_pos < 0)
        return;

    if (g_terminal.history_pos == 0) {
        /* Restore draft */
        strncpy(g_terminal.input_line, g_terminal.history_draft,
                sizeof(g_terminal.input_line) - 1);
        g_terminal.input_line[sizeof(g_terminal.input_line) - 1] = '\0';
        g_terminal.input_pos = (int)strlen(g_terminal.input_line);
        g_terminal.history_pos = -1;
        return;
    }

    g_terminal.history_pos--;
    int count = g_terminal.history_count;
    int slot = (count - 1 - g_terminal.history_pos) % HISTORY_MAX;
    if (slot < 0)
        slot += HISTORY_MAX;
    strncpy(g_terminal.input_line, g_terminal.history[slot],
            sizeof(g_terminal.input_line) - 1);
    g_terminal.input_line[sizeof(g_terminal.input_line) - 1] = '\0';
    g_terminal.input_pos = (int)strlen(g_terminal.input_line);
}

/* ---- Command Dispatch ------------------------------------------------------ */
static void cmd_help(void)
{
    terminal_app_write("Available commands:\n");
    terminal_app_write("  help              Show this message\n");
    terminal_app_write("  clear             Clear terminal screen\n");
    terminal_app_write("  ls [path]         List directory (default: /jarvis)\n");
    terminal_app_write("  echo <text>       Print text to terminal\n");
    terminal_app_write("  status            Show desktop and kernel status\n");
    terminal_app_write("  uptime            Show system uptime\n");
    terminal_app_write("  open <app>        Open app (Files|Terminal|Settings|Calculator)\n");
    terminal_app_write("  theme <mode>      Set theme: dark | light | hc\n");
    terminal_app_write("  notify <msg>      Show a notification toast\n");
}

static void cmd_ls(const char *path)
{
    if (!path || strlen(path) == 0)
        path = "/jarvis";

    char buf[2048];
    if (vfs_list(path, buf, sizeof(buf)) < 0) {
        terminal_app_write("ls: failed to list ");
        terminal_app_write(path);
        terminal_app_write("\n");
        return;
    }

    terminal_app_write(path);
    terminal_app_write(":\n");

    const char *p = buf;
    int count = 0;
    while (*p) {
        char name[128];
        char type[16];
        int name_len = 0, type_len = 0;

        memset(name, 0, sizeof(name));
        memset(type, 0, sizeof(type));

        while (*p && *p != '|' && *p != '\n') {
            if (name_len + 1 < (int)sizeof(name))
                name[name_len++] = *p;
            p++;
        }
        if (*p != '|') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        p++;

        while (*p && *p != '|' && *p != '\n') {
            if (type_len + 1 < (int)sizeof(type))
                type[type_len++] = *p;
            p++;
        }

        int is_dir = (strcmp(type, "dir") == 0);
        terminal_app_write(is_dir ? "  [D] " : "  [F] ");
        terminal_app_write(name);
        terminal_app_write("\n");
        count++;

        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (count == 0)
        terminal_app_write("  (empty)\n");
}

static void cmd_status(void)
{
    char buf[128];

    terminal_app_write("--- System Status ---\n");

    snprintf(buf, sizeof(buf), "  AI Core:   %s\n", g_kstate.ai_ready ? "READY" : "LOADING");
    terminal_app_write(buf);

    snprintf(buf, sizeof(buf), "  Speech:    %s\n", g_kstate.speech_ready ? "READY" : "LOADING");
    terminal_app_write(buf);

    snprintf(buf, sizeof(buf), "  Security:  %s\n", g_kstate.security_enabled ? "ENABLED" : "DISABLED");
    terminal_app_write(buf);

    snprintf(buf, sizeof(buf), "  Windows:   %d open\n", wm_count_windows());
    terminal_app_write(buf);

    snprintf(buf, sizeof(buf), "  Screen:    %ux%u\n", fb_get_width(), fb_get_height());
    terminal_app_write(buf);
}

static void cmd_uptime(void)
{
    char buf[64];
    uint64_t secs = g_kstate.uptime_ticks / 100;
    uint64_t h = secs / 3600;
    uint64_t m = (secs % 3600) / 60;
    uint64_t s = secs % 60;
    snprintf(buf, sizeof(buf), "  Uptime: %llu h %llu m %llu s\n",
             (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
    terminal_app_write(buf);
}

static void cmd_theme(const char *mode)
{
    if (!mode || strlen(mode) == 0) {
        terminal_app_write("usage: theme dark|light|hc\n");
        return;
    }
    if (strcmp(mode, "dark") == 0) {
        theme_set_mode(THEME_DARK);
        terminal_app_write("Theme: Dark\n");
    } else if (strcmp(mode, "light") == 0) {
        theme_set_mode(THEME_LIGHT);
        terminal_app_write("Theme: Light\n");
    } else if (strcmp(mode, "hc") == 0 || strcmp(mode, "highcontrast") == 0) {
        theme_set_mode(THEME_HIGH_CONTRAST);
        terminal_app_write("Theme: High Contrast\n");
    } else {
        terminal_app_write("unknown theme (use: dark, light, hc)\n");
    }
}

void terminal_app_execute_command(const char *cmd)
{
    if (!cmd || strlen(cmd) == 0)
        return;

    history_push(cmd);
    g_terminal.history_pos = -1;

    terminal_app_write("\n> ");
    terminal_app_write(cmd);
    terminal_app_write("\n");

    /* Split into verb + rest */
    char verb[64] = {};
    const char *rest = "";
    const char *sp = strchr(cmd, ' ');
    if (sp) {
        size_t vlen = (size_t)(sp - cmd);
        if (vlen >= sizeof(verb))
            vlen = sizeof(verb) - 1;
        memcpy(verb, cmd, vlen);
        rest = sp + 1;
        while (*rest == ' ')
            rest++;
    } else {
        strncpy(verb, cmd, sizeof(verb) - 1);
    }

    if (strcmp(verb, "help") == 0) {
        cmd_help();
    } else if (strcmp(verb, "clear") == 0) {
        terminal_app_clear();
    } else if (strcmp(verb, "ls") == 0) {
        cmd_ls(rest);
    } else if (strcmp(verb, "echo") == 0) {
        terminal_app_write(rest);
        terminal_app_write("\n");
    } else if (strcmp(verb, "status") == 0) {
        cmd_status();
    } else if (strcmp(verb, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(verb, "open") == 0) {
        if (strlen(rest) == 0) {
            terminal_app_write("usage: open <app>\n");
        } else {
            desktop_system_open_app(rest);
            terminal_app_write("Launched: ");
            terminal_app_write(rest);
            terminal_app_write("\n");
        }
    } else if (strcmp(verb, "theme") == 0) {
        cmd_theme(rest);
    } else if (strcmp(verb, "notify") == 0) {
        if (strlen(rest) == 0) {
            terminal_app_write("usage: notify <message>\n");
        } else {
            notification_show("Terminal", rest, NOTIF_INFO, 4000);
            terminal_app_write("Notification sent\n");
        }
    } else {
        terminal_app_write("Unknown command: ");
        terminal_app_write(verb);
        terminal_app_write("\n(type 'help' for a list)\n");
    }
}

/* ---- Event Handling -------------------------------------------------------- */
void terminal_app_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    if (!event) return;

    switch (event->type) {
    case IE_TEXT_INPUT:
        {
            char ch = event->data.text.ch;
            if (ch >= 32 && (uint8_t)ch < 127 &&
                g_terminal.input_pos < (int)sizeof(g_terminal.input_line) - 1) {
                g_terminal.input_line[g_terminal.input_pos++] = ch;
                g_terminal.input_line[g_terminal.input_pos] = '\0';
            }
        }
        break;

    case IE_KEY_DOWN:
        {
            uint32_t key = event->data.keyboard.key;
            int mods = event->data.keyboard.mods;

            if (key == 0x1C) {  /* Enter */
                terminal_app_execute_command(g_terminal.input_line);
                g_terminal.input_line[0] = '\0';
                g_terminal.input_pos = 0;
            } else if (key == 0x0E) {  /* Backspace */
                if (g_terminal.input_pos > 0) {
                    g_terminal.input_line[--g_terminal.input_pos] = '\0';
                }
            } else if (key == 0x48) {  /* Up arrow — history older */
                history_up();
            } else if (key == 0x50) {  /* Down arrow — history newer */
                history_down();
            } else if (key == 0x4B) {  /* Left arrow — cursor left */
                if (g_terminal.input_pos > 0)
                    g_terminal.input_pos--;
            } else if (key == 0x4D) {  /* Right arrow — cursor right */
                if (g_terminal.input_pos < (int)strlen(g_terminal.input_line))
                    g_terminal.input_pos++;
            } else if ((mods & KMOD_CTRL) && (key == 0x26 || key == 0x26)) {
                /* Ctrl+L — clear */
                terminal_app_clear();
            } else {
                /* Use the full scancode→ASCII table for all other keys */
                char ch = input_scancode_to_char(key, mods);
                if (ch && g_terminal.input_pos < (int)sizeof(g_terminal.input_line) - 1) {
                    g_terminal.input_line[g_terminal.input_pos++] = ch;
                    g_terminal.input_line[g_terminal.input_pos] = '\0';
                }
            }
        }
        break;

    default:
        break;
    }
}

/* ---- Public API ------------------------------------------------------------ */
wm_window_t *terminal_app_open(int x, int y, int w, int h)
{
    memset(&g_terminal, 0, sizeof(g_terminal));
    terminal_app_clear();
    g_terminal.history_pos = -1;

    wm_window_t *win = wm_create_window(
        "Terminal",
        x, y, w, h,
        WS_DEFAULT,
        terminal_app_draw_callback,
        terminal_app_event_callback
    );

    g_terminal.window = win;
    g_terminal.bg_color = 0x00000000;
    g_terminal.fg_color = 0x00FFFFFF;

    terminal_app_write("JARVIS Terminal v1.0\n");
    terminal_app_write("Type 'help' for available commands\n\n");

    return win;
}
