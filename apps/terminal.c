/*
 * =============================================================================
 * JARVIS OS — Terminal (apps/terminal.c)
 * Language: C17
 *
 * The window around the shell (apps/shell.c): scrollback with a coloured
 * prompt, line editing (arrows, Home/End, Ctrl+A/E/U/K/W, Delete), Tab
 * completion, history kept across sessions, scrolling (wheel, PgUp/PgDn),
 * mouse selection with copy, and paste.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

#define TERM_LINES      600
#define TERM_COLS_MAX   200
#define HISTORY_MAX     200
#define TERM_BG         0x000C0F14
#define TERM_FG         0x00D0D4DA
#define TERM_ERR        0x00FF7A7A
#define TERM_USER       0x0058E06A
#define TERM_PATH       0x005AA8FF
#define TERM_SEL        0x00264F78
#define HISTORY_FILE    "/jarvis/data/.jsh_history"

int  shell_execute(const char *line);
void shell_prompt(char *out, size_t n);
const char *shell_cwd(void);
int  shell_complete_command(const char *prefix, const char **out, int max);
int  shell_complete_path(const char *partial, char out[][96], int max, int *is_dir);

enum { LK_OUT = 0, LK_ERR = 1, LK_CMD = 2 };

typedef struct {
    wm_window_t *window;
    char lines[TERM_LINES][TERM_COLS_MAX + 1];
    uint8_t kind[TERM_LINES];
    uint8_t plen[TERM_LINES];   /* LK_CMD: prompt length within the line */
    int first, count, col;
    int cols;                   /* columns that fit the window          */
    int cur_kind;               /* kind of text being written now       */
    int scroll;                 /* lines scrolled back from the bottom  */

    char input[256];
    int input_len, cursor;

    char history[HISTORY_MAX][256];
    int history_count, history_pos;
    char saved_input[256];

    int sel_active, sel_dragging;
    int sel_a_line, sel_a_col, sel_b_line, sel_b_col;   /* absolute line index (0 = oldest) */
    int rows_shown, top_line;   /* layout of the last frame, for mouse hits */
} terminal_context_t;

static terminal_context_t g_terminal;

/* ---- Output -------------------------------------------------------------- */
static char *term_line(int i)
{
    return g_terminal.lines[(g_terminal.first + i) % TERM_LINES];
}
static int term_slot(int i) { return (g_terminal.first + i) % TERM_LINES; }

static void term_newline(void)
{
    if (g_terminal.count < TERM_LINES) g_terminal.count++;
    else g_terminal.first = (g_terminal.first + 1) % TERM_LINES;
    int s = term_slot(g_terminal.count - 1);
    g_terminal.lines[s][0] = 0;
    g_terminal.kind[s] = (uint8_t)g_terminal.cur_kind;
    g_terminal.plen[s] = 0;
    g_terminal.col = 0;
}

void terminal_app_clear(void)
{
    g_terminal.first = 0;
    g_terminal.count = 1;
    g_terminal.col = 0;
    g_terminal.lines[0][0] = 0;
    g_terminal.kind[0] = LK_OUT;
    g_terminal.scroll = 0;
    g_terminal.sel_active = 0;
}

void terminal_app_put_char(char c)
{
    int cols = g_terminal.cols > 10 ? g_terminal.cols : 100;
    if (cols > TERM_COLS_MAX) cols = TERM_COLS_MAX;
    if (c == '\n') {
        term_newline();
    } else if (c == '\t') {
        do terminal_app_put_char(' '); while (g_terminal.col % 8);
    } else if ((unsigned char)c >= 32) {
        if (g_terminal.col >= cols) term_newline();
        int s = term_slot(g_terminal.count - 1);
        if (g_terminal.col == 0) g_terminal.kind[s] = (uint8_t)g_terminal.cur_kind;
        g_terminal.lines[s][g_terminal.col++] = c;
        g_terminal.lines[s][g_terminal.col] = 0;
    }
}

void terminal_app_write(const char *text)
{
    while (*text) terminal_app_put_char(*text++);
    g_terminal.scroll = 0;
    desktop_invalidate();
}

void terminal_app_write_err(const char *text)
{
    g_terminal.cur_kind = LK_ERR;
    terminal_app_write(text);
    g_terminal.cur_kind = LK_OUT;
}

/* ---- History ------------------------------------------------------------- */
static void history_push(const char *cmd, int save)
{
    if (!*cmd) return;
    if (g_terminal.history_count > 0 &&
        !strcmp(g_terminal.history[(g_terminal.history_count - 1) % HISTORY_MAX], cmd))
        return;
    snprintf(g_terminal.history[g_terminal.history_count % HISTORY_MAX], 256, "%s", cmd);
    g_terminal.history_count++;
    if (save) {
        vfs_append(HISTORY_FILE, cmd, strlen(cmd));
        vfs_append(HISTORY_FILE, "\n", 1);
        if (vfs_size(HISTORY_FILE) > 64 * 1024) {        /* keep the file small: rewrite the last entries */
            static char b[HISTORY_MAX * 64];
            size_t o = 0;
            int avail = g_terminal.history_count < HISTORY_MAX ? g_terminal.history_count : HISTORY_MAX;
            for (int i = g_terminal.history_count - avail; i < g_terminal.history_count && o < sizeof(b) - 260; i++)
                o += (size_t)snprintf(b + o, sizeof(b) - o, "%s\n", g_terminal.history[i % HISTORY_MAX]);
            vfs_write_file(HISTORY_FILE, b, o);
        }
    }
}

static void history_load_file(void)
{
    static char b[64 * 1024 + 1];
    int n = vfs_read_file(HISTORY_FILE, b, sizeof(b));
    if (n <= 0) return;
    for (char *p = b; *p;) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        history_push(p, 0);
        if (!e) break;
        p = e + 1;
    }
}

const char *terminal_history_get(int i)
{
    int avail = g_terminal.history_count < HISTORY_MAX ? g_terminal.history_count : HISTORY_MAX;
    if (i < 0 || i >= avail) return NULL;
    return g_terminal.history[(g_terminal.history_count - avail + i) % HISTORY_MAX];
}

static void set_input(const char *s)
{
    snprintf(g_terminal.input, sizeof(g_terminal.input), "%s", s);
    g_terminal.input_len = (int)strlen(g_terminal.input);
    g_terminal.cursor = g_terminal.input_len;
}

static void history_move(int dir)
{
    int avail = g_terminal.history_count < HISTORY_MAX ? g_terminal.history_count : HISTORY_MAX;
    int pos = g_terminal.history_pos + dir;
    if (pos < -1 || pos >= avail) return;
    if (g_terminal.history_pos == -1) snprintf(g_terminal.saved_input, sizeof(g_terminal.saved_input), "%s", g_terminal.input);
    g_terminal.history_pos = pos;
    if (pos == -1) set_input(g_terminal.saved_input);
    else set_input(g_terminal.history[(g_terminal.history_count - 1 - pos) % HISTORY_MAX]);
}

/* ---- Drawing ------------------------------------------------------------- */
static int sel_contains(int line, int col)
{
    if (!g_terminal.sel_active) return 0;
    int al = g_terminal.sel_a_line, ac = g_terminal.sel_a_col, bl = g_terminal.sel_b_line, bc = g_terminal.sel_b_col;
    if (al > bl || (al == bl && ac > bc)) { int t = al; al = bl; bl = t; t = ac; ac = bc; bc = t; }
    if (line < al || line > bl) return 0;
    if (line == al && col < ac) return 0;
    if (line == bl && col >= bc) return 0;
    return 1;
}

/* user@host in green, ':' and '$ ' in white, the folder in blue */
static void draw_prompt_text(int x, int y, const char *prompt, int n)
{
    int colon = -1;
    for (int i = 0; i < n; i++) if (prompt[i] == ':') { colon = i; break; }
    for (int i = 0; i < n; i++) {
        uint32_t c = i < colon ? TERM_USER : (i == colon || i >= n - 2) ? TERM_FG : TERM_PATH;
        const font_face_t *f = &g_font_faces[i < colon ? FONT_MONO_BOLD : FONT_MONO];
        (void)f;
        char s[2] = { prompt[i], 0 };
        fb_text(x + i * 8, y, s, c, i < colon ? FONT_MONO_BOLD : FONT_MONO);
    }
}

static void draw_line(int x, int y, int abs_line, const char *s, int kind, int plen)
{
    int len = (int)strlen(s);
    for (int c = 0; c < len; c++)
        if (sel_contains(abs_line, c)) fb_fill_rect(x + c * 8, y, 8, 16, TERM_SEL);
    if (kind == LK_CMD && plen > 0 && plen <= len) {
        draw_prompt_text(x, y, s, plen);
        fb_text_n(x + plen * 8, y, s + plen, -1, 0x00FFFFFF, FONT_MONO);
    } else {
        fb_draw_text_mono(x, y, s, kind == LK_ERR ? TERM_ERR : TERM_FG);
    }
}

void terminal_app_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    fb_fill_rect(x, y, w, h, TERM_BG);

    g_terminal.cols = (w - 24) / 8;
    if (g_terminal.cols > TERM_COLS_MAX) g_terminal.cols = TERM_COLS_MAX;
    int rows = (h - 12) / 16;
    if (rows < 2) rows = 2;

    uint32_t fgpid = user_foreground();
    int out_lines = g_terminal.count;
    if (term_line(out_lines - 1)[0] == 0 || fgpid) out_lines--;

    /* the input row counts as one more row at the bottom */
    int total = out_lines + 1;
    int max_scroll = total - rows;
    if (max_scroll < 0) max_scroll = 0;
    if (g_terminal.scroll > max_scroll) g_terminal.scroll = max_scroll;
    int top = total - rows - g_terminal.scroll;
    if (top < 0) top = 0;
    g_terminal.top_line = top;
    g_terminal.rows_shown = rows;

    int ty = y + 6;
    for (int i = top; i < out_lines && ty + 16 <= y + h; i++, ty += 16)
        draw_line(x + 8, ty, i, term_line(i), g_terminal.kind[term_slot(i)], g_terminal.plen[term_slot(i)]);

    if (out_lines >= top && ty + 16 <= y + h) {
        /* input row: the shell prompt, or a running program's last line */
        int px;
        if (fgpid) {
            const char *lead = term_line(g_terminal.count - 1);
            fb_draw_text_mono(x + 8, ty, lead, TERM_FG);
            px = x + 8 + (int)strlen(lead) * 8;
        } else {
            char prompt[200];
            shell_prompt(prompt, sizeof(prompt));
            int pl = (int)strlen(prompt);
            draw_prompt_text(x + 8, ty, prompt, pl);
            px = x + 8 + pl * 8;
        }
        fb_text_n(px, ty, g_terminal.input, -1, 0x00FFFFFF, FONT_MONO);
        int focused = (g_wm_ctx.focused == win);
        int cxp = px + g_terminal.cursor * 8;
        if (focused && (g_kstate.uptime_ticks / 50) % 2 == 0) {
            fb_fill_rect(cxp, ty, 8, 16, TERM_FG);
            if (g_terminal.cursor < g_terminal.input_len) fb_draw_char(cxp, ty, g_terminal.input[g_terminal.cursor], TERM_BG);
        } else if (!focused) fb_draw_rect(cxp, ty, 8, 16, 0x00707070);
    }

    /* scrollbar while scrolled back */
    if (max_scroll > 0) {
        int track = h - 8, thumb = track * rows / total;
        if (thumb < 20) thumb = 20;
        int pos = (track - thumb) * (max_scroll - g_terminal.scroll) / max_scroll;
        fb_fill_rounded_rect(x + w - 8, y + 4 + pos, 4, thumb, 2, g_terminal.scroll ? 0x00808890 : 0x00404850);
    }
}

/* ---- Running a line -------------------------------------------------------- */
static void term_echo_command(const char *cmd)
{
    char prompt[200];
    shell_prompt(prompt, sizeof(prompt));
    if (term_line(g_terminal.count - 1)[0] != 0) term_newline();
    g_terminal.cur_kind = LK_CMD;
    int s = term_slot(g_terminal.count - 1);
    g_terminal.kind[s] = LK_CMD;
    g_terminal.plen[s] = (uint8_t)strlen(prompt);
    terminal_app_write(prompt);
    terminal_app_write(cmd);
    g_terminal.cur_kind = LK_OUT;
    terminal_app_write("\n");
}

void terminal_app_execute_command(const char *cmd)
{
    while (*cmd == ' ') cmd++;
    if (!*cmd) return;
    history_push(cmd, 1);
    shell_execute(cmd);
    if (g_terminal.window) {
        char t[96];
        snprintf(t, sizeof(t), "Terminal - %s", shell_cwd());
        wm_set_title(g_terminal.window, t);
    }
}

/* ---- Tab completion ------------------------------------------------------- */
static void term_complete(void)
{
    /* the word under the cursor */
    int ws = g_terminal.cursor;
    while (ws > 0 && g_terminal.input[ws - 1] != ' ') ws--;
    char word[128];
    int wl = g_terminal.cursor - ws;
    if (wl >= (int)sizeof(word)) return;
    memcpy(word, g_terminal.input + ws, (size_t)wl);
    word[wl] = 0;
    int first_word = 1;
    for (int i = 0; i < ws; i++) if (g_terminal.input[i] != ' ') first_word = 0;

    static char names[64][96];
    static int dirs[64];
    int n = 0;
    if (first_word && !strchr(word, '/') && word[0] != '.' && word[0] != '~') {
        const char *c[64];
        n = shell_complete_command(word, c, 64);
        for (int i = 0; i < n; i++) { snprintf(names[i], 96, "%s", c[i]); dirs[i] = 0; }
    }
    int path_mode = n == 0;
    if (path_mode) n = shell_complete_path(word, names, 64, dirs);
    if (!n) return;

    const char *slash = strrchr(word, '/');
    int stem_off = slash ? (int)(slash - word + 1) : 0;
    /* longest common prefix */
    int common = (int)strlen(names[0]);
    for (int i = 1; i < n; i++) {
        int k = 0;
        while (k < common && names[i][k] && (names[i][k] | 32) == (names[0][k] | 32)) k++;
        common = k;
    }
    if (n > 1 && common <= wl - stem_off) {
        /* nothing more to add: list the choices */
        term_echo_command(g_terminal.input);
        for (int i = 0; i < n; i++) {
            char item[110];
            snprintf(item, sizeof(item), "%s%s  ", names[i], dirs[i] ? "/" : "");
            terminal_app_write(item);
        }
        terminal_app_write("\n");
        return;
    }
    char repl[160];
    snprintf(repl, sizeof(repl), "%.*s%.*s%s", stem_off, word, common, names[0],
             n == 1 ? (path_mode && dirs[0] ? "/" : " ") : "");
    char rest[256];
    snprintf(rest, sizeof(rest), "%s", g_terminal.input + g_terminal.cursor);
    char out[256];
    snprintf(out, sizeof(out), "%.*s%s%s", ws, g_terminal.input, repl, rest);
    int newcur = ws + (int)strlen(repl);
    set_input(out);
    g_terminal.cursor = newcur > g_terminal.input_len ? g_terminal.input_len : newcur;
}

/* ---- Editing helpers -------------------------------------------------------- */
static void insert_text(const char *s)
{
    for (; *s; s++) {
        char ch = *s;
        if ((unsigned char)ch < 32 || g_terminal.input_len >= (int)sizeof(g_terminal.input) - 1) continue;
        memmove(g_terminal.input + g_terminal.cursor + 1, g_terminal.input + g_terminal.cursor,
                (size_t)(g_terminal.input_len - g_terminal.cursor + 1));
        g_terminal.input[g_terminal.cursor++] = ch;
        g_terminal.input_len++;
    }
}

static void delete_range(int a, int b)
{
    if (a < 0) a = 0;
    if (b > g_terminal.input_len) b = g_terminal.input_len;
    if (b <= a) return;
    memmove(g_terminal.input + a, g_terminal.input + b, (size_t)(g_terminal.input_len - b + 1));
    g_terminal.input_len -= b - a;
    g_terminal.cursor = a;
}

static void copy_selection(void)
{
    if (!g_terminal.sel_active) return;
    static char buf[32768];
    size_t o = 0;
    int al = g_terminal.sel_a_line, ac = g_terminal.sel_a_col, bl = g_terminal.sel_b_line, bc = g_terminal.sel_b_col;
    if (al > bl || (al == bl && ac > bc)) { int t = al; al = bl; bl = t; t = ac; ac = bc; bc = t; }
    for (int l = al; l <= bl && l < g_terminal.count; l++) {
        const char *s = term_line(l);
        int len = (int)strlen(s);
        int from = l == al ? ac : 0, to = l == bl ? bc : len;
        if (to > len) to = len;
        for (int c = from; c < to && o < sizeof(buf) - 2; c++) buf[o++] = s[c];
        if (l != bl && o < sizeof(buf) - 2) buf[o++] = '\n';
    }
    clipboard_set(buf, o);
    notification_show("Terminal", "Copied to the clipboard.", NOTIF_INFO, 1500);
}

static void paste_clipboard(void)
{
    size_t len;
    const char *c = clipboard_get(&len);
    char line[256];
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        if (c[i] == '\n') {                     /* multi-line paste: run each full line */
            line[k] = 0;
            insert_text(line);
            k = 0;
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "%s", g_terminal.input);
            term_echo_command(cmd);
            set_input("");
            terminal_app_execute_command(cmd);
            continue;
        }
        if (c[i] == '\r') continue;
        if (k < sizeof(line) - 1) line[k++] = c[i];
    }
    line[k] = 0;
    insert_text(line);
}

/* ---- Events -------------------------------------------------------------- */
static void mouse_to_cell(int mx, int my, int *line, int *col)
{
    int row = (my - 6) / 16;
    if (row < 0) row = 0;
    *line = g_terminal.top_line + row;
    if (*line >= g_terminal.count) *line = g_terminal.count - 1;
    *col = (mx - 8 + 4) / 8;
    if (*col < 0) *col = 0;
}

void terminal_app_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    (void)win;
    switch (event->type) {
    case IE_MOUSE_WHEEL:
        g_terminal.scroll += event->data.mouse.button > 0 ? 3 : -3;
        if (g_terminal.scroll < 0) g_terminal.scroll = 0;
        desktop_invalidate();
        return;
    case IE_MOUSE_DOWN:
        if (event->data.mouse.button == 2) {           /* right-click: copy, or paste */
            if (g_terminal.sel_active) { copy_selection(); g_terminal.sel_active = 0; }
            else paste_clipboard();
            return;
        }
        if (event->data.mouse.button != 1) return;
        mouse_to_cell(event->data.mouse.x, event->data.mouse.y, &g_terminal.sel_a_line, &g_terminal.sel_a_col);
        g_terminal.sel_b_line = g_terminal.sel_a_line;
        g_terminal.sel_b_col = g_terminal.sel_a_col;
        g_terminal.sel_active = 0;
        g_terminal.sel_dragging = 1;
        return;
    case IE_MOUSE_MOVE:
        if (!g_terminal.sel_dragging) return;
        mouse_to_cell(event->data.mouse.x, event->data.mouse.y, &g_terminal.sel_b_line, &g_terminal.sel_b_col);
        g_terminal.sel_active = g_terminal.sel_b_line != g_terminal.sel_a_line || g_terminal.sel_b_col != g_terminal.sel_a_col;
        desktop_invalidate();
        return;
    case IE_MOUSE_UP:
        g_terminal.sel_dragging = 0;
        return;
    case IE_TEXT_INPUT: {
        char ch = event->data.text.ch;
        if (ch >= 32 && ch < 127) {
            char s[2] = { ch, 0 };
            insert_text(s);
            g_terminal.scroll = 0;
        }
        return;
    }
    case IE_KEY_DOWN:
        break;
    default:
        return;
    }

    uint32_t key = event->data.keyboard.key;
    int mods = event->data.keyboard.mods;
    int ctrl = (mods & KMOD_CTRL) != 0;
    char lk = (key >= 'A' && key <= 'Z') ? (char)(key + 32) : (char)key;

    /* copy / paste work in every mode */
    if (ctrl && (mods & KMOD_SHIFT) && lk == 'c') { copy_selection(); return; }
    if (ctrl && lk == 'v') { paste_clipboard(); return; }

    /* A program started with 'run' owns the keyboard until it ends. */
    uint32_t fg = user_foreground();
    if (fg) {
        if (key == WM_KEY_ENTER) {
            terminal_app_write(g_terminal.input);
            terminal_app_write("\n");
            user_stdin_push(g_terminal.input, (size_t)g_terminal.input_len);
            user_stdin_push("\n", 1);
            set_input("");
        } else if (key == WM_KEY_BACKSPACE) {
            if (g_terminal.cursor > 0) delete_range(g_terminal.cursor - 1, g_terminal.cursor);
        } else if (ctrl && lk == 'c') {
            proc_kill(fg);
            user_set_foreground(0);
            set_input("");
            terminal_app_write("^C\n");
        }
        return;
    }

    if (key == WM_KEY_ENTER) g_terminal.sel_active = 0;
    switch (key) {
    case WM_KEY_ENTER: {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s", g_terminal.input);
        term_echo_command(cmd);
        set_input("");
        g_terminal.history_pos = -1;
        g_terminal.scroll = 0;
        terminal_app_execute_command(cmd);
        return;
    }
    case WM_KEY_BACKSPACE:
        if (ctrl) { int a = g_terminal.cursor; while (a > 0 && g_terminal.input[a - 1] == ' ') a--; while (a > 0 && g_terminal.input[a - 1] != ' ') a--; delete_range(a, g_terminal.cursor); }
        else if (g_terminal.cursor > 0) delete_range(g_terminal.cursor - 1, g_terminal.cursor);
        return;
    case WM_KEY_DELETE: if (g_terminal.cursor < g_terminal.input_len) delete_range(g_terminal.cursor, g_terminal.cursor + 1); return;
    case WM_KEY_LEFT:
        if (ctrl) { while (g_terminal.cursor > 0 && g_terminal.input[g_terminal.cursor - 1] == ' ') g_terminal.cursor--; while (g_terminal.cursor > 0 && g_terminal.input[g_terminal.cursor - 1] != ' ') g_terminal.cursor--; }
        else if (g_terminal.cursor > 0) g_terminal.cursor--;
        return;
    case WM_KEY_RIGHT:
        if (ctrl) { while (g_terminal.cursor < g_terminal.input_len && g_terminal.input[g_terminal.cursor] != ' ') g_terminal.cursor++; while (g_terminal.cursor < g_terminal.input_len && g_terminal.input[g_terminal.cursor] == ' ') g_terminal.cursor++; }
        else if (g_terminal.cursor < g_terminal.input_len) g_terminal.cursor++;
        return;
    case WM_KEY_HOME: g_terminal.cursor = 0; return;
    case WM_KEY_END: g_terminal.cursor = g_terminal.input_len; return;
    case WM_KEY_UP:
        if (mods & KMOD_SHIFT) { g_terminal.scroll++; return; }
        history_move(1); return;
    case WM_KEY_DOWN:
        if (mods & KMOD_SHIFT) { if (g_terminal.scroll > 0) g_terminal.scroll--; return; }
        history_move(-1); return;
    case WM_KEY_PGUP: g_terminal.scroll += g_terminal.rows_shown - 2; return;
    case WM_KEY_PGDN: g_terminal.scroll -= g_terminal.rows_shown - 2; if (g_terminal.scroll < 0) g_terminal.scroll = 0; return;
    case WM_KEY_TAB: term_complete(); return;
    case WM_KEY_ESC: set_input(""); g_terminal.history_pos = -1; return;
    default: break;
    }
    if (!ctrl) return;
    switch (lk) {
    case 'l': terminal_app_clear(); break;
    case 'a': g_terminal.cursor = 0; break;
    case 'e': g_terminal.cursor = g_terminal.input_len; break;
    case 'u': delete_range(0, g_terminal.cursor); break;
    case 'k': delete_range(g_terminal.cursor, g_terminal.input_len); break;
    case 'w': { int a = g_terminal.cursor; while (a > 0 && g_terminal.input[a - 1] == ' ') a--; while (a > 0 && g_terminal.input[a - 1] != ' ') a--; delete_range(a, g_terminal.cursor); break; }
    case 'c':
        if (g_terminal.sel_active) { copy_selection(); break; }
        term_echo_command(g_terminal.input);
        terminal_app_write("^C\n");
        set_input("");
        break;
    default: break;
    }
}

/* ---- Public API ---------------------------------------------------------- */
wm_window_t *terminal_app_open(int x, int y, int w, int h)
{
    memset(&g_terminal, 0, sizeof(g_terminal));
    terminal_app_clear();
    g_terminal.history_pos = -1;
    g_terminal.cols = (w - 24) / 8;
    history_load_file();

    wm_window_t *win = wm_create_window("Terminal", x, y, w, h, WS_DEFAULT,
                                        terminal_app_draw_callback,
                                        terminal_app_event_callback);
    if (!win) return NULL;

    win->icon = ICON_TERMINAL;
    g_terminal.window = win;

    terminal_app_write("JARVIS OS Terminal. Type 'help' for commands, 'help <command>' for details.\n");
    char t[96];
    snprintf(t, sizeof(t), "Terminal - %s", shell_cwd());
    wm_set_title(win, t);
    return win;
}
