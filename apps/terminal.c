/*
 * =============================================================================
 * JARVIS OS — Terminal (apps/terminal.c)
 * Language: C17
 *
 * Scrollback console with an inline prompt, command history (Up / Down) and
 * built-in commands for the VFS, the desktop and the assistant.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

#define TERM_LINES      200
#define TERM_COLS       100
#define HISTORY_MAX     16
#define TERM_BG         0x000C0C0C
#define TERM_FG         0x00CCCCCC
#define TERM_PROMPT     "jarvis> "

typedef struct {
    wm_window_t *window;
    char lines[TERM_LINES][TERM_COLS + 1];
    int first;              /* index of the oldest line in the ring */
    int count;              /* number of lines in use (>= 1)        */
    int col;                /* write position in the newest line    */

    char input[160];
    int input_len;

    char history[HISTORY_MAX][160];
    int history_count;
    int history_pos;        /* -1 = editing a new line */
} terminal_context_t;

static terminal_context_t g_terminal;

/* ---- Output -------------------------------------------------------------- */
static char *term_line(int i)
{
    return g_terminal.lines[(g_terminal.first + i) % TERM_LINES];
}

static void term_newline(void)
{
    if (g_terminal.count < TERM_LINES) {
        g_terminal.count++;
    } else {
        g_terminal.first = (g_terminal.first + 1) % TERM_LINES;
    }
    term_line(g_terminal.count - 1)[0] = 0;
    g_terminal.col = 0;
}

void terminal_app_clear(void)
{
    g_terminal.first = 0;
    g_terminal.count = 1;
    g_terminal.col = 0;
    g_terminal.lines[0][0] = 0;
}

void terminal_app_put_char(char c)
{
    if (c == '\n') {
        term_newline();
    } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
        if (g_terminal.col >= TERM_COLS)
            term_newline();
        char *line = term_line(g_terminal.count - 1);
        line[g_terminal.col++] = c;
        line[g_terminal.col] = 0;
    }
}

void terminal_app_write(const char *text)
{
    while (*text)
        terminal_app_put_char(*text++);
    desktop_invalidate();
}

/* ---- Drawing ------------------------------------------------------------- */
void terminal_app_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);

    fb_fill_rect(x, y, w, h, TERM_BG);

    int rows = (h - 12) / 16;
    if (rows < 2) rows = 2;

    /* The prompt occupies the row after the last output line, unless that
     * line is empty (then it reuses it). */
    int out_lines = g_terminal.count;
    if (term_line(out_lines - 1)[0] == 0)
        out_lines--;

    int start = out_lines - (rows - 1);
    if (start < 0) start = 0;

    int ty = y + 6;
    for (int i = start; i < out_lines; i++, ty += 16)
        fb_draw_text(x + 8, ty, term_line(i), TERM_FG);

    fb_draw_text(x + 8, ty, TERM_PROMPT, 0x0046D3C6);
    int px = x + 8 + fb_text_width(TERM_PROMPT);
    fb_draw_text(px, ty, g_terminal.input, 0x00FFFFFF);

    /* Block cursor, blinking only while the window has focus */
    int focused = (g_wm_ctx.focused == win);
    if (!focused || (g_kstate.uptime_ticks / 50) % 2 == 0) {
        int cxp = px + g_terminal.input_len * 8;
        if (focused) fb_fill_rect(cxp, ty, 8, 16, TERM_FG);
        else         fb_draw_rect(cxp, ty, 8, 16, 0x00707070);
    }
}

/* ---- History ------------------------------------------------------------- */
static void history_push(const char *cmd)
{
    if (!*cmd) return;
    if (g_terminal.history_count > 0 &&
        strcmp(g_terminal.history[(g_terminal.history_count - 1) % HISTORY_MAX], cmd) == 0)
        return;

    char *slot = g_terminal.history[g_terminal.history_count % HISTORY_MAX];
    strncpy(slot, cmd, sizeof(g_terminal.history[0]) - 1);
    slot[sizeof(g_terminal.history[0]) - 1] = 0;
    g_terminal.history_count++;
}

static void history_load(int pos)
{
    int avail = g_terminal.history_count < HISTORY_MAX ? g_terminal.history_count : HISTORY_MAX;

    if (pos < 0) {
        g_terminal.history_pos = -1;
        g_terminal.input[0] = 0;
        g_terminal.input_len = 0;
        return;
    }
    if (pos >= avail) return;

    g_terminal.history_pos = pos;
    int slot = (g_terminal.history_count - 1 - pos) % HISTORY_MAX;
    strncpy(g_terminal.input, g_terminal.history[slot], sizeof(g_terminal.input) - 1);
    g_terminal.input[sizeof(g_terminal.input) - 1] = 0;
    g_terminal.input_len = (int)strlen(g_terminal.input);
}

/* ---- Commands ------------------------------------------------------------ */
static void term_resolve(const char *arg, char *out, size_t n)
{
    if (!arg || !*arg)      snprintf(out, n, "/jarvis");
    else if (arg[0] == '/') snprintf(out, n, "%s", arg);
    else                    snprintf(out, n, "/jarvis/%s", arg);
}

uint32_t sysmon_spawn_test(const char *kind);                       /* apps/sysmon.c */
const char *sysmon_state_name(const proc_info_t *p, char *buf, size_t n);

static void cmd_help(void)
{
    terminal_app_write(
        "Files:    ls [dir]   cat <file>   mkdir <dir>   touch <file>   rm <path>\n"
        "          mv <from> <to>   cp <from> <to>\n"
        "Programs: run <name>   (hello, counter, kernelpoke, privileged)\n"
        "Apps:     packages   install <file.jpk>   uninstall <name>   start <name>\n"
        "Process:  ps   kill <pid>   suspend <pid>   resume <pid>\n"
        "          spawn cpu|sleeper|deadlock|memory|isolate\n"
        "Disk:     df   sync   disks   sector <disk> <n>      Devices: devices\n"
        "Network:  ifconfig   ping <host>   nslookup <host>   scan\n"
        "          http <url> [file]   (plain http only; saves to data/<file>)\n"
        "System:   status   uptime   date   clear   echo <text>\n"
        "Desktop:  apps   open <app>   theme dark|light|hc   notify <text>\n"
        "JARVIS:   ask <question>\n"
        "Power:    lock   reboot   shutdown\n");
}

static void cmd_ls(const char *arg)
{
    static char listing[16384];
    char dir[128];
    term_resolve(arg, dir, sizeof(dir));
    size_t dlen = strlen(dir);
    int shown = 0;

    if (vfs_list(dir, listing, sizeof(listing)) < 0) {
        terminal_app_write("ls: cannot read directory\n");
        return;
    }

    for (const char *p = listing; *p;) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;

        int is_dir = strncmp(p, "[dir] ", 6) == 0;
        int is_file = strncmp(p, "[file] ", 7) == 0;
        const char *full = p + (is_dir ? 6 : 7);
        size_t flen = (size_t)(eol - full);

        if ((is_dir || is_file) && flen > dlen + 1 &&
            strncmp(full, dir, dlen) == 0 && full[dlen] == '/') {
            const char *name = full + dlen + 1;
            size_t nlen = flen - dlen - 1;
            int direct = 1;
            for (size_t i = 0; i < nlen; i++)
                if (name[i] == '/') { direct = 0; break; }
            if (direct) {
                terminal_app_write(is_dir ? "  <DIR>  " : "         ");
                for (size_t i = 0; i < nlen; i++)
                    terminal_app_put_char(name[i]);
                terminal_app_put_char('\n');
                shown++;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    if (!shown)
        terminal_app_write("  (empty)\n");
}

static void cmd_cat(const char *arg)
{
    static char buf[4096];
    char path[128];
    if (!*arg) { terminal_app_write("usage: cat <file>\n"); return; }
    term_resolve(arg, path, sizeof(path));
    if (vfs_read_file(path, buf, sizeof(buf)) < 0) {
        terminal_app_write("cat: no such file\n");
        return;
    }
    terminal_app_write(buf);
    terminal_app_write("\n");
}

static void cmd_status(void)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "  AI brain   %s\n", g_kstate.ai_ready ? "ready" : "loading");
    terminal_app_write(buf);
    snprintf(buf, sizeof(buf), "  Voice      %s\n", g_kstate.speech_ready ? "ready" : "loading");
    terminal_app_write(buf);
    snprintf(buf, sizeof(buf), "  Security   %s (lock %s)\n", ui_shell_get_security_label(),
             g_kstate.security_enabled ? "on" : "off");
    terminal_app_write(buf);
    {
        net_info_t ni; char ip[16];
        net_get_info(&ni);
        net_format_ip(ni.ip, ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "  Network    %s%s\n", !ni.adapter ? "no adapter" : !ni.link ? "cable unplugged"
                 : !ni.configured ? "waiting for DHCP" : "connected, ", ni.configured ? ip : "");
    }
    terminal_app_write(buf);
    snprintf(buf, sizeof(buf), "  Display    %ux%u, %d window(s)\n",
             fb_get_width(), fb_get_height(), wm_count_windows());
    terminal_app_write(buf);
}

void terminal_app_execute_command(const char *cmd)
{
    while (*cmd == ' ') cmd++;
    if (!*cmd) return;

    history_push(cmd);

    char verb[24] = {};
    const char *rest = strchr(cmd, ' ');
    size_t vlen = rest ? (size_t)(rest - cmd) : strlen(cmd);
    if (vlen >= sizeof(verb)) vlen = sizeof(verb) - 1;
    memcpy(verb, cmd, vlen);
    rest = rest ? rest : "";
    while (*rest == ' ') rest++;

    char buf[160], path[128];

    desktop_files_changed();    /* cheap; covers mkdir / touch / rm / mv / cp */

    if (strcmp(verb, "help") == 0) {
        cmd_help();
    } else if (strcmp(verb, "clear") == 0 || strcmp(verb, "cls") == 0) {
        terminal_app_clear();
    } else if (strcmp(verb, "ls") == 0 || strcmp(verb, "dir") == 0) {
        cmd_ls(rest);
    } else if (strcmp(verb, "cat") == 0 || strcmp(verb, "type") == 0) {
        cmd_cat(rest);
    } else if (strcmp(verb, "mkdir") == 0) {
        term_resolve(rest, path, sizeof(path));
        terminal_app_write(*rest && vfs_mkdir(path) == 0 ? "Folder created.\n"
                                                         : "mkdir: failed\n");
    } else if (strcmp(verb, "touch") == 0) {
        term_resolve(rest, path, sizeof(path));
        terminal_app_write(*rest && vfs_create_file(path) == 0 ? "File created.\n"
                                                               : "touch: failed\n");
    } else if (strcmp(verb, "rm") == 0) {
        term_resolve(rest, path, sizeof(path));
        if (fatfs_owns(path))
            terminal_app_write("rm: USB drives are read-only in this version\n");
        else
            terminal_app_write(*rest && vfs_delete(path) == 0 ? "Deleted.\n" : "rm: not found\n");
    } else if (strcmp(verb, "mv") == 0 || strcmp(verb, "cp") == 0) {
        char a[64] = {}, to[128];
        const char *sp = strchr(rest, ' ');
        if (!sp) { terminal_app_write("usage: mv|cp <from> <to>\n"); return; }
        size_t alen = (size_t)(sp - rest);
        if (alen >= sizeof(a)) alen = sizeof(a) - 1;
        memcpy(a, rest, alen);
        while (*sp == ' ') sp++;
        term_resolve(a, path, sizeof(path));
        term_resolve(sp, to, sizeof(to));
        int rc = verb[0] == 'm' ? vfs_rename(path, to) : vfs_copy(path, to);
        terminal_app_write(rc == 0 ? "Done.\n" : "Failed (missing source, or target exists).\n");
    } else if (strcmp(verb, "ps") == 0) {
        static proc_info_t procs[64];
        int n = proc_list(procs, 64);
        terminal_app_write("  PID  NAME              STATUS                CPU TIME\n");
        for (int i = 0; i < n; i++) {
            char st[40], line[120], name[18], stp[22];
            sysmon_state_name(&procs[i], st, sizeof(st));
            /* pad by hand: the kernel printf has no field widths */
            memset(name, ' ', 17); name[17] = 0;
            memcpy(name, procs[i].name, strlen(procs[i].name) < 17 ? strlen(procs[i].name) : 17);
            memset(stp, ' ', 21); stp[21] = 0;
            memcpy(stp, st, strlen(st) < 21 ? strlen(st) : 21);
            snprintf(line, sizeof(line), "  %s%u  %s %s %u.%us\n",
                     procs[i].pid < 10 ? "  " : (procs[i].pid < 100 ? " " : ""),
                     procs[i].pid, name, stp,
                     (unsigned)(procs[i].cpu_ticks / 100), (unsigned)((procs[i].cpu_ticks / 10) % 10));
            terminal_app_write(line);
        }
    } else if (strcmp(verb, "kill") == 0 || strcmp(verb, "suspend") == 0 ||
               strcmp(verb, "resume") == 0) {
        uint32_t pid = 0;
        for (const char *d = rest; *d >= '0' && *d <= '9'; d++) pid = pid * 10 + (uint32_t)(*d - '0');
        int rc = verb[0] == 'k' ? proc_kill(pid)
               : verb[0] == 's' ? proc_suspend(pid) : proc_resume(pid);
        terminal_app_write(rc == 0 ? "OK\n" : "Failed: no such process, or it is protected.\n");
    } else if (strcmp(verb, "ifconfig") == 0 || strcmp(verb, "ipconfig") == 0) {
        net_info_t ni;
        char a[16], b[16];
        net_get_info(&ni);
        if (!ni.adapter) {
            terminal_app_write("No network adapter.\n");
        } else {
            snprintf(buf, sizeof(buf), "Ethernet  %02x:%02x:%02x:%02x:%02x:%02x  cable %s\n", ni.mac[0], ni.mac[1],
                     ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5], ni.link ? "connected" : "unplugged");
            terminal_app_write(buf);
            if (ni.configured) {
                net_format_ip(ni.ip, a, sizeof(a)); net_format_ip(ni.mask, b, sizeof(b));
                snprintf(buf, sizeof(buf), "  Address  %s   Mask %s\n", a, b); terminal_app_write(buf);
                net_format_ip(ni.gateway, a, sizeof(a)); net_format_ip(ni.dns, b, sizeof(b));
                snprintf(buf, sizeof(buf), "  Gateway  %s   DNS  %s\n", a, b); terminal_app_write(buf);
            } else {
                terminal_app_write("  No address yet (waiting for DHCP)\n");
            }
            snprintf(buf, sizeof(buf), "  Frames   %u received, %u sent\n", (unsigned)ni.rx_frames, (unsigned)ni.tx_frames);
            terminal_app_write(buf);
        }
    } else if (strcmp(verb, "ping") == 0 || strcmp(verb, "nslookup") == 0 ||
               strcmp(verb, "http") == 0 || strcmp(verb, "scan") == 0) {
        if (net_command_start(cmd) != 0)
            terminal_app_write("Another network command is still running.\n");
    } else if (strcmp(verb, "devices") == 0) {
        static dev_info_t devs[48];
        static const char *const states[3] = { "working", "NO DRIVER", "NOT WORKING" };
        int n = devmgr_list(devs, 48);
        for (int i = 0; i < n; i++) {
            snprintf(buf, sizeof(buf), "  %s [%s] %s%s%s\n", devs[i].name,
                     devs[i].driver[0] ? devs[i].driver : "-", states[devs[i].state],
                     devs[i].note[0] ? ", " : "", devs[i].note);
            terminal_app_write(buf);
        }
    } else if (strcmp(verb, "crc32") == 0) {
        /* crc32 <file>: checksum of the whole file (same as zlib / cksum -a crc32b) */
        term_resolve(rest, path, sizeof(path));
        long size = vfs_size(path);
        uint8_t *data = (size > 0 && size < (64L << 20)) ?
            (uint8_t *)(size >= (long)LARGE_ALLOC_THRESHOLD ? kmalloc_large((size_t)size + 1) : kmalloc((size_t)size + 1)) : NULL;
        if (!*rest || vfs_is_dir(path) || !data || vfs_read_file(path, (char *)data, (size_t)size + 1) != size) {
            terminal_app_write(size == 0 ? "crc32: file is empty\n" : "crc32: cannot read that file\n");
        } else {
            uint32_t crc = 0xFFFFFFFFu;
            for (long i = 0; i < size; i++) {
                crc ^= data[i];
                for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
            }
            crc = ~crc;
            static const char hx[] = "0123456789abcdef";
            char h[9];
            for (int k = 0; k < 8; k++) h[k] = hx[(crc >> (28 - 4 * k)) & 15];
            h[8] = 0;
            snprintf(buf, sizeof(buf), "%s  %u bytes  %s\n", h, (unsigned)size, path);
            terminal_app_write(buf);
        }
        if (data) { if (size >= (long)LARGE_ALLOC_THRESHOLD) kfree_large(data, (size_t)size + 1); else kfree(data); }
    } else if (strcmp(verb, "disks") == 0) {
        for (int i = 0; i < block_count(); i++) {
            block_info_t b;
            if (block_get_info(i, &b) != 0) continue;
            snprintf(buf, sizeof(buf), "  %d  %s  %u MB  %s%s\n", i, b.name,
                     (unsigned)(b.sectors / 2048), b.model, b.errors ? "  (has had I/O errors)" : "");
            terminal_app_write(buf);
        }
        if (!block_count()) terminal_app_write("No disks.\n");
    } else if (strcmp(verb, "sector") == 0) {
        /* sector <disk number> <sector>: show the first 64 bytes */
        static uint8_t sec[512];
        uint32_t dev = 0, lba = 0;
        const char *q = rest;
        while (*q >= '0' && *q <= '9') dev = dev * 10 + (uint32_t)(*q++ - '0');
        while (*q == ' ') q++;
        while (*q >= '0' && *q <= '9') lba = lba * 10 + (uint32_t)(*q++ - '0');
        if (!*rest || block_read((int)dev, lba, 1, sec) != 0) {
            terminal_app_write("usage: sector <disk> <sector>   (read failed)\n");
        } else {
            static const char hex[] = "0123456789abcdef";
            for (int row = 0; row < 4; row++) {
                char *o = buf;
                *o++ = ' '; *o++ = ' '; *o++ = hex[(row * 16) >> 4 & 15]; *o++ = '0'; *o++ = ' ';
                for (int i = 0; i < 16; i++) {      /* (the kernel printf has no %02x) */
                    *o++ = hex[sec[row * 16 + i] >> 4]; *o++ = hex[sec[row * 16 + i] & 15]; *o++ = ' ';
                }
                for (int i = 0; i < 16; i++) { uint8_t c = sec[row * 16 + i]; *o++ = (c >= 32 && c < 127) ? (char)c : '.'; }
                *o++ = '\n'; *o = 0;
                terminal_app_write(buf);
            }
        }
    } else if (strcmp(verb, "df") == 0) {
        diskfs_stats_t st;
        if (diskfs_get_stats(&st) != 0) {
            terminal_app_write("No disk mounted: files are in memory only.\n");
        } else {
            snprintf(buf, sizeof(buf), "%s  %s\n", st.device, st.model);
            terminal_app_write(buf);
            snprintf(buf, sizeof(buf), "  %u MB total, %u KB used, %d items\n",
                     (unsigned)(st.total_bytes >> 20), (unsigned)(st.used_bytes >> 10), st.items);
            terminal_app_write(buf);
        }
    } else if (strcmp(verb, "sync") == 0) {
        snprintf(buf, sizeof(buf), "%d item(s) written to disk.\n", diskfs_sync());
        terminal_app_write(buf);
    } else if (strcmp(verb, "install") == 0) {
        char err[96];
        pkg_info_t info;
        term_resolve(rest, path, sizeof(path));
        if (!*rest) terminal_app_write("usage: install <file.jpk>   (e.g. install data/Downloads/hello-1.0.jpk)\n");
        else if (pkg_install(path, &info, err, sizeof(err)) == 0) {
            snprintf(buf, sizeof(buf), "Installed %s %s (%u file(s)). Start it from Start, or: start %s\n",
                     info.title, info.version, info.files, info.name);
            terminal_app_write(buf);
        } else {
            snprintf(buf, sizeof(buf), "install: %s\n", err);
            terminal_app_write(buf);
        }
    } else if (strcmp(verb, "uninstall") == 0) {
        terminal_app_write(*rest && pkg_uninstall(rest) == 0 ? "Uninstalled.\n" : "uninstall: no such app (see: packages)\n");
    } else if (strcmp(verb, "packages") == 0) {
        static pkg_info_t list[32];
        int n = pkg_list(list, 32);
        if (!n) terminal_app_write("No apps installed. Install one with: install <file.jpk>\n");
        for (int i = 0; i < n; i++) {
            snprintf(buf, sizeof(buf), "  %s  %s %s  (%s)\n", list[i].name, list[i].title, list[i].version,
                     list[i].publisher[0] ? list[i].publisher : "unknown publisher");
            terminal_app_write(buf);
        }
    } else if (strcmp(verb, "start") == 0) {
        if (!*rest || !pkg_run(rest)) terminal_app_write("start: no such app (see: packages)\n");
    } else if (strcmp(verb, "run") == 0) {
        /* Programs live in /jarvis/bin unless a path is given. */
        if (strchr(rest, '/')) term_resolve(rest, path, sizeof(path));
        else                   snprintf(path, sizeof(path), "/jarvis/bin/%s", rest);
        uint32_t pid = *rest ? user_exec(path) : 0;
        if (!pid) terminal_app_write("run: not a program (try: ls bin)\n");
        else { snprintf(buf, sizeof(buf), "Started %s as PID %u (user mode)\n", rest, pid); terminal_app_write(buf); }
    } else if (strcmp(verb, "spawn") == 0) {
        uint32_t pid = sysmon_spawn_test(rest);
        if (!pid) terminal_app_write("usage: spawn cpu|sleeper|deadlock|memory|isolate\n");
        else { snprintf(buf, sizeof(buf), "Started PID %u\n", pid); terminal_app_write(buf); }
    } else if (strcmp(verb, "echo") == 0) {
        terminal_app_write(rest);
        terminal_app_write("\n");
    } else if (strcmp(verb, "status") == 0) {
        cmd_status();
    } else if (strcmp(verb, "uptime") == 0) {
        sys_format_uptime(buf, sizeof(buf));
        terminal_app_write(buf);
        terminal_app_write("\n");
    } else if (strcmp(verb, "date") == 0) {
        char clock[8];
        sys_format_date(buf, sizeof(buf));
        sys_format_clock(clock, sizeof(clock));
        terminal_app_write(buf);
        terminal_app_write("  ");
        terminal_app_write(clock);
        terminal_app_write("\n");
    } else if (strcmp(verb, "apps") == 0) {
        for (int i = 0; i < desktop_app_count(); i++) {
            desktop_app_t *app = desktop_app_get(i);
            snprintf(buf, sizeof(buf), "  %s%s\n", app->name, app->window ? "  (running)" : "");
            terminal_app_write(buf);
        }
    } else if (strcmp(verb, "open") == 0) {
        int idx = desktop_app_find(rest);
        if (idx < 0) terminal_app_write("open: unknown app (try 'apps')\n");
        else         desktop_app_launch(idx);
    } else if (strcmp(verb, "theme") == 0) {
        if (strcmp(rest, "dark") == 0)       theme_set_mode(THEME_DARK);
        else if (strcmp(rest, "light") == 0) theme_set_mode(THEME_LIGHT);
        else if (strcmp(rest, "hc") == 0)    theme_set_mode(THEME_HIGH_CONTRAST);
        else { terminal_app_write("usage: theme dark|light|hc\n"); return; }
        snprintf(buf, sizeof(buf), "Theme: %s\n", theme_get_name());
        terminal_app_write(buf);
    } else if (strcmp(verb, "notify") == 0) {
        if (!*rest) terminal_app_write("usage: notify <text>\n");
        else        notification_show("Terminal", rest, NOTIF_INFO, 4000);
    } else if (strcmp(verb, "ask") == 0) {
        if (!*rest) terminal_app_write("usage: ask <question>\n");
        else {
            desktop_ask_jarvis(rest);
            terminal_app_write("Sent to JARVIS (reply appears in the JARVIS window).\n");
        }
    } else if (strcmp(verb, "lock") == 0) {
        desktop_lock();
    } else if (strcmp(verb, "reboot") == 0) {
        desktop_system_restart();
    } else if (strcmp(verb, "shutdown") == 0) {
        desktop_system_shutdown();
    } else {
        snprintf(buf, sizeof(buf), "'%s' is not a command. Type 'help'.\n", verb);
        terminal_app_write(buf);
    }
}

/* ---- Events -------------------------------------------------------------- */
void terminal_app_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    (void)win;

    if (event->type == IE_TEXT_INPUT) {
        char ch = event->data.text.ch;
        if (ch >= 32 && ch < 127 && g_terminal.input_len < (int)sizeof(g_terminal.input) - 1) {
            g_terminal.input[g_terminal.input_len++] = ch;
            g_terminal.input[g_terminal.input_len] = 0;
        }
        return;
    }

    if (event->type != IE_KEY_DOWN)
        return;

    uint32_t key = event->data.keyboard.key;
    int mods = event->data.keyboard.mods;

    if (key == WM_KEY_ENTER) {
        char cmd[sizeof(g_terminal.input)];
        memcpy(cmd, g_terminal.input, sizeof(cmd));

        /* Echo the command line into the scrollback, then run it. */
        if (term_line(g_terminal.count - 1)[0] != 0)
            term_newline();
        terminal_app_write(TERM_PROMPT);
        terminal_app_write(cmd);
        terminal_app_write("\n");

        g_terminal.input[0] = 0;
        g_terminal.input_len = 0;
        g_terminal.history_pos = -1;
        terminal_app_execute_command(cmd);
    } else if (key == WM_KEY_BACKSPACE) {
        if (g_terminal.input_len > 0)
            g_terminal.input[--g_terminal.input_len] = 0;
    } else if (key == WM_KEY_UP) {
        history_load(g_terminal.history_pos + 1);
    } else if (key == WM_KEY_DOWN) {
        history_load(g_terminal.history_pos - 1);
    } else if (key == WM_KEY_ESC) {
        history_load(-1);
    } else if ((mods & KMOD_CTRL) && (key == 'l' || key == 'L')) {
        terminal_app_clear();
    }
}

/* ---- Public API ---------------------------------------------------------- */
wm_window_t *terminal_app_open(int x, int y, int w, int h)
{
    memset(&g_terminal, 0, sizeof(g_terminal));
    terminal_app_clear();
    g_terminal.history_pos = -1;

    wm_window_t *win = wm_create_window("Terminal", x, y, w, h, WS_DEFAULT,
                                        terminal_app_draw_callback,
                                        terminal_app_event_callback);
    if (!win) return NULL;

    win->icon = ICON_TERMINAL;
    g_terminal.window = win;

    terminal_app_write("JARVIS OS Terminal\n");
    terminal_app_write("Type 'help' for a list of commands.\n");
    return win;
}
