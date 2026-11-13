/*
 * =============================================================================
 * JARVIS AI OS — AI tools (kernel/ai_tools.c)
 * Language: C17
 *
 * Lets the language model act on the PC. The model runs on the host
 * (tools/jarvis_brain.py, llama.cpp) and talks to this file over the voice
 * bridge serial line (COM2), one text line per message:
 *
 *   host -> guest   AI:ONLINE <model>        heartbeat (every 2 s)
 *                   TOOL:<id>:<name>:<json>  run a tool
 *                   REPLY:<text>             final answer: shown and spoken
 *                   AI:THINKING              progress
 *   guest -> host   ASK:<text>               the user said or typed this
 *                   RESULT:<id>:<text>       a tool's result ("\n" escaped)
 *
 * Every tool goes through the same kernel functions the apps use, so the AI
 * can do nothing the user could not. Destructive actions (deleting, ending
 * processes, uninstalling, overwriting files, shutting down) wait for the
 * user to press Allow in the JARVIS window. When the model is offline the
 * built-in commands keep working (see ai_host_online()).
 * =============================================================================
 */

#include "kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void voice_serial_puts(const char *s);
int  file_explorer_trash_path(const char *path);
void notepad_open_file(const char *path);
void desktop_system_shutdown(void);
void desktop_system_restart(void);

static char g_model[48];

/* ==========================================================================
 * Line protocol helpers
 * ========================================================================== */
int ai_host_online(void)
{
    return governor_ai_online();
}

const char *ai_host_model(void)
{
    return ai_host_online() ? g_model : "";
}

/* Send one protocol line; newlines in text are escaped as \n. */
static void send_line(const char *prefix, const char *text)
{
    static char out[4200];
    size_t n = 0;
    for (const char *p = prefix; *p && n < sizeof(out) - 2; p++) out[n++] = *p;
    for (const char *p = text; *p && n < sizeof(out) - 3; p++) {
        if (*p == '\n')      { out[n++] = '\\'; out[n++] = 'n'; }
        else if (*p == '\r') continue;
        else if (*p == '\\') { out[n++] = '\\'; out[n++] = '\\'; }
        else                 out[n++] = ((unsigned char)*p < 32) ? ' ' : *p;
    }
    out[n++] = '\n';
    out[n] = 0;
    voice_serial_puts(out);
}

void ai_host_ask(const char *text)
{
    ui_shell_set_status("THINKING");
    send_line("ASK:", text);
}

/* Minimal JSON reader for flat objects from the host: {"path":"/x","pid":3} */
static int json_str(const char *json, const char *key, char *out, size_t n)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p || n == 0) { if (n) out[0] = 0; return 0; }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    size_t k = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"' && k < n - 1; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
                out[k++] = c;
            } else {
                out[k++] = *p;
            }
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ' ' && k < n - 1) out[k++] = *p++;
    }
    out[k] = 0;
    return 1;
}

static long json_int(const char *json, const char *key, long dflt)
{
    char v[24];
    if (!json_str(json, key, v, sizeof(v)) || !v[0]) return dflt;
    long r = 0;
    int neg = v[0] == '-';
    for (const char *p = v + neg; *p >= '0' && *p <= '9'; p++) r = r * 10 + (*p - '0');
    return neg ? -r : r;
}

/* Paths from the model may be relative to /jarvis ("data/notes.txt"). */
static void resolve(const char *in, char *out, size_t n)
{
    if (!in[0])                         snprintf(out, n, "/jarvis");
    else if (in[0] == '/')              snprintf(out, n, "%s", in);
    else if (!strncmp(in, "jarvis/", 7)) snprintf(out, n, "/%s", in);
    else                                snprintf(out, n, "/jarvis/%s", in);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
}

/* ==========================================================================
 * Tools
 * ========================================================================== */
typedef struct { char *buf; size_t n, len; } out_t;

static void outf(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(out_t *o, const char *fmt, ...)
{
    if (o->len + 1 >= o->n) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->buf + o->len, o->n - o->len, fmt, ap);
    va_end(ap);
    if (w > 0) o->len += (size_t)w < o->n - o->len ? (size_t)w : o->n - o->len - 1;
}

static void size_text(uint64_t b, char *s, size_t n)
{
    if (b < 1024) snprintf(s, n, "%u B", (unsigned)b);
    else if (b < 1048576) snprintf(s, n, "%u KB", (unsigned)(b >> 10));
    else snprintf(s, n, "%u MB", (unsigned)(b >> 20));
}

static void tool_list_files(const char *args, out_t *o)
{
    static char listing[16384];
    char a[160], dir[200];
    json_str(args, "path", a, sizeof(a));
    resolve(a, dir, sizeof(dir));
    if (!vfs_is_dir(dir)) { outf(o, "error: %s is not a folder", dir); return; }
    if (vfs_list(dir, listing, sizeof(listing)) < 0) { outf(o, "error: cannot read %s", dir); return; }
    size_t dl = strlen(dir);
    int count = 0;
    outf(o, "Contents of %s:\n", dir);
    for (const char *p = listing; *p && count < 80;) {
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        int is_dir = !strncmp(p, "[dir] ", 6);
        const char *full = strchr(p, '/');
        if (full && full < eol && (size_t)(eol - full) > dl + 1 && !strncmp(full, dir, dl) && full[dl] == '/') {
            const char *name = full + dl + 1;
            size_t nl = (size_t)(eol - name);
            if (!memchr(name, '/', nl) && nl < 120) {
                char nm[128], path[256], sz[16];
                memcpy(nm, name, nl); nm[nl] = 0;
                memcpy(path, full, (size_t)(eol - full)); path[eol - full] = 0;
                if (is_dir) outf(o, "  [folder] %s\n", nm);
                else { size_text((uint64_t)vfs_size(path), sz, sizeof(sz)); outf(o, "  %s (%s)\n", nm, sz); }
                count++;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    if (!count) outf(o, "  (empty)\n");
}

static void tool_find_files(const char *args, out_t *o)
{
    static char listing[32768];
    char q[80];
    json_str(args, "name", q, sizeof(q));
    if (!q[0]) { outf(o, "error: give a name to search for"); return; }
    for (char *c = q; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    int found = 0;
    const char *roots[5] = { "/jarvis", NULL, NULL, NULL, NULL };
    drive_info_t drives[4];
    int nd = storage_list(drives, 4), nr = 1;
    for (int i = 0; i < nd && nr < 5; i++) if (drives[i].removable && drives[i].mount[0]) roots[nr++] = drives[i].mount;
    for (int r = 0; r < nr; r++) {
        if (vfs_list(roots[r], listing, sizeof(listing)) < 0) continue;
        for (const char *p = listing; *p && found < 30;) {
            const char *eol = strchr(p, '\n');
            if (!eol) eol = p + strlen(p);
            const char *full = strchr(p, '/');
            if (full && full < eol && strncmp(full, "/jarvis/models", 14) && strncmp(full, "/jarvis/trash", 13)) {
                const char *leaf = full;
                for (const char *s = full; s < eol; s++) if (*s == '/') leaf = s + 1;
                char low[128];
                size_t ll = (size_t)(eol - leaf) < sizeof(low) - 1 ? (size_t)(eol - leaf) : sizeof(low) - 1;
                for (size_t i = 0; i < ll; i++) low[i] = (leaf[i] >= 'A' && leaf[i] <= 'Z') ? (char)(leaf[i] + 32) : leaf[i];
                low[ll] = 0;
                if (strstr(low, q)) {
                    char path[256];
                    size_t pl = (size_t)(eol - full) < 255 ? (size_t)(eol - full) : 255;
                    memcpy(path, full, pl); path[pl] = 0;
                    outf(o, "%s%s\n", path, !strncmp(p, "[dir]", 5) ? "  (folder)" : "");
                    found++;
                }
            }
            p = *eol ? eol + 1 : eol;
        }
    }
    if (!found) outf(o, "No files or folders named like \"%s\".", q);
}

static void tool_read_file(const char *args, out_t *o)
{
    static char buf[3001];
    char a[160], path[200];
    json_str(args, "path", a, sizeof(a));
    resolve(a, path, sizeof(path));
    if (!vfs_exists(path) || vfs_is_dir(path)) { outf(o, "error: no file %s", path); return; }
    int n = vfs_read_file(path, buf, sizeof(buf));
    if (n < 0) { outf(o, "error: cannot read %s", path); return; }
    for (int i = 0; i < n; i++)
        if ((unsigned char)buf[i] < 9 || (unsigned char)buf[i] > 126) { outf(o, "%s is not a text file.", path); return; }
    long total = vfs_size(path);
    outf(o, "%s%s", buf, total > n ? "\n[... file continues]" : "");
}

static void tool_status(out_t *o)
{
    char up[32], ip[16], clock[8], date[32];
    uint64_t total, freef, idle;
    sys_format_uptime(up, sizeof(up));
    sys_format_clock(clock, sizeof(clock));
    sys_format_date(date, sizeof(date));
    pmm_get_stats(&total, &freef);
    scheduler_stats(&idle, NULL);
    net_info_t ni;
    net_get_info(&ni);
    net_format_ip(ni.ip, ip, sizeof(ip));
    diskfs_stats_t ds;
    static proc_info_t procs[64];
    int np = proc_list(procs, 64);
    outf(o, "Time: %s %s. Uptime: %s.\n", clock, date, up);
    outf(o, "Memory: %u of %u MB in use.\n", (unsigned)((total - freef) >> 8), (unsigned)(total >> 8));
    if (diskfs_get_stats(&ds) == 0)
        outf(o, "Disk: %u KB used of %u MB, %d items.\n", (unsigned)(ds.used_bytes >> 10), (unsigned)(ds.total_bytes >> 20), ds.items);
    else
        outf(o, "Disk: none (files are in memory only).\n");
    outf(o, "Network: %s%s.\n", ni.configured ? "connected, address " : ni.adapter ? "not connected" : "no adapter",
         ni.configured ? ip : "");
    outf(o, "Processes: %d. Theme: %s. Resource management: %s.\n", np, theme_get_name(),
         governor_ai_online() ? "AI" : "built-in rules");
}

static void tool_processes(out_t *o)
{
    static proc_info_t p[64];
    static const char *const st[] = { "running", "ready", "waiting", "exiting", "sleeping" };
    int n = proc_list(p, 64);
    for (int i = 0; i < n; i++)
        outf(o, "PID %u %s: %s%s%s, cpu time %u s\n", p[i].pid, p[i].name, p[i].suspended ? "suspended" : st[p[i].state % 5],
             (p[i].flags & PROC_FLAG_SYSTEM) ? ", system (protected)" : "",
             (p[i].flags & PROC_FLAG_SERVICE) ? ", service (protected)" : "", (unsigned)(p[i].cpu_ticks / 100));
}

static void tool_devices(out_t *o)
{
    static dev_info_t d[48];
    static const char *const st[] = { "working", "no driver", "not working" };
    int n = devmgr_list(d, 48);
    for (int i = 0; i < n; i++)
        outf(o, "%s [%s] driver %s, %s\n", d[i].name, d[i].type, d[i].driver[0] ? d[i].driver : "none", st[d[i].state % 3]);
}

static void tool_drives(out_t *o)
{
    drive_info_t d[8];
    int n = storage_list(d, 8);
    for (int i = 0; i < n; i++) {
        char a[16], b[16];
        size_text(d[i].free, a, sizeof(a));
        size_text(d[i].total, b, sizeof(b));
        outf(o, "%s (%s): %s, %s free of %s, folder %s%s\n", d[i].name, d[i].device, d[i].fs, a, b,
             d[i].mount[0] ? d[i].mount : "(not readable)", d[i].read_only ? ", read-only" : "");
    }
}

static void tool_apps(out_t *o)
{
    outf(o, "Apps:");
    for (int i = 0; i < desktop_app_count(); i++) {
        desktop_app_t *a = desktop_app_get(i);
        outf(o, " %s%s%s", i ? "," : "", a->name, a->window ? " (open)" : "");
    }
}

/* ---- Confirmation for destructive tools ---------------------------------- */
static struct {
    int  active;
    char id[16], name[32], args[1024];
    char text[160];
} g_pending;

int ai_tools_pending(char *text, size_t n)
{
    if (!g_pending.active) return 0;
    snprintf(text, n, "%s", g_pending.text);
    return 1;
}

static void run_tool(const char *id, const char *name, const char *args, int confirmed);

void ai_tools_confirm(int allow)
{
    if (!g_pending.active) return;
    g_pending.active = 0;
    if (allow) {
        run_tool(g_pending.id, g_pending.name, g_pending.args, 1);
    } else {
        char line[48];
        snprintf(line, sizeof(line), "RESULT:%s:", g_pending.id);
        send_line(line, "The user denied this action. Do not try it again unless they ask.");
    }
    desktop_invalidate();
}

static int needs_confirm(const char *name, const char *args, char *why, size_t n)
{
    char a[160], b[160], path[200];
    if (!strcmp(name, "delete")) {
        json_str(args, "path", a, sizeof(a)); resolve(a, path, sizeof(path));
        snprintf(why, n, "move %s to the Recycle Bin", path); return 1;
    }
    if (!strcmp(name, "write_file")) {
        json_str(args, "path", a, sizeof(a)); resolve(a, path, sizeof(path));
        if (!vfs_exists(path)) return 0;
        snprintf(why, n, "overwrite %s", path); return 1;
    }
    if (!strcmp(name, "kill_process")) {
        snprintf(why, n, "end process %ld", json_int(args, "pid", 0)); return 1;
    }
    if (!strcmp(name, "uninstall_app")) {
        json_str(args, "name", a, sizeof(a)); snprintf(why, n, "uninstall the app %s", a); return 1;
    }
    if (!strcmp(name, "install_package")) {
        json_str(args, "path", a, sizeof(a)); snprintf(why, n, "install the package %s", a); return 1;
    }
    if (!strcmp(name, "run_program")) {
        json_str(args, "path", a, sizeof(a)); snprintf(why, n, "run the program %s", a); return 1;
    }
    if (!strcmp(name, "power")) {
        json_str(args, "action", b, sizeof(b)); snprintf(why, n, "%s the computer", !strcmp(b, "restart") ? "restart" : "shut down"); return 1;
    }
    return 0;
}

/* ---- Dispatcher ----------------------------------------------------------- */
static void run_tool(const char *id, const char *name, const char *args, int confirmed)
{
    static char result[4096];
    out_t o = { result, sizeof(result), 0 };
    char a[200], b[200], p1[200], p2[200], why[160];
    result[0] = 0;

    if (!confirmed && needs_confirm(name, args, why, sizeof(why))) {
        g_pending.active = 1;
        snprintf(g_pending.id, sizeof(g_pending.id), "%s", id);
        snprintf(g_pending.name, sizeof(g_pending.name), "%s", name);
        snprintf(g_pending.args, sizeof(g_pending.args), "%s", args);
        snprintf(g_pending.text, sizeof(g_pending.text), "JARVIS wants to %s.", why);
        desktop_app_launch(desktop_app_find("JARVIS"));     /* the Allow / Deny prompt is there */
        desktop_invalidate();
        return;                                             /* the result is sent after the choice */
    }

    if (!strcmp(name, "open_app")) {
        json_str(args, "name", a, sizeof(a));
        int i = desktop_app_find(a);
        if (i < 0) { outf(&o, "No app called \"%s\". ", a); tool_apps(&o); }
        else { desktop_app_launch(i); outf(&o, "Opened %s.", desktop_app_get(i)->name); }
    } else if (!strcmp(name, "close_app")) {
        json_str(args, "name", a, sizeof(a));
        int i = desktop_app_find(a);
        desktop_app_t *app = i >= 0 ? desktop_app_get(i) : NULL;
        if (!app || !app->window) outf(&o, "%s is not open.", a);
        else { wm_request_close(app->window); outf(&o, "Asked %s to close.", app->name); }
    } else if (!strcmp(name, "list_apps")) {
        tool_apps(&o);
    } else if (!strcmp(name, "list_files")) {
        tool_list_files(args, &o);
    } else if (!strcmp(name, "find_files")) {
        tool_find_files(args, &o);
    } else if (!strcmp(name, "read_file")) {
        tool_read_file(args, &o);
    } else if (!strcmp(name, "write_file")) {
        static char text[3000];
        json_str(args, "path", a, sizeof(a)); resolve(a, p1, sizeof(p1));
        json_str(args, "text", text, sizeof(text));
        outf(&o, vfs_write_file(p1, text, strlen(text)) == 0 ? "Saved %s." : "error: could not write %s", p1);
    } else if (!strcmp(name, "create_folder")) {
        json_str(args, "path", a, sizeof(a)); resolve(a, p1, sizeof(p1));
        outf(&o, vfs_mkdir(p1) == 0 ? "Created %s." : "error: could not create %s", p1);
    } else if (!strcmp(name, "delete")) {
        json_str(args, "path", a, sizeof(a)); resolve(a, p1, sizeof(p1));
        outf(&o, file_explorer_trash_path(p1) == 0 ? "Moved %s to the Recycle Bin." :
             "error: %s could not be deleted (missing, protected, or on a read-only drive)", p1);
    } else if (!strcmp(name, "move") || !strcmp(name, "copy")) {
        json_str(args, "from", a, sizeof(a)); resolve(a, p1, sizeof(p1));
        json_str(args, "to", b, sizeof(b));   resolve(b, p2, sizeof(p2));
        if (vfs_is_dir(p2)) {                                    /* into a folder */
            const char *leaf = strrchr(p1, '/');
            char t[200]; snprintf(t, sizeof(t), "%s%s", p2, leaf ? leaf : "/item");
            snprintf(p2, sizeof(p2), "%s", t);
        }
        int rc = name[0] == 'm' ? vfs_rename(p1, p2) : vfs_copy(p1, p2);
        outf(&o, rc == 0 ? "Done: %s -> %s." : "error: %s -> %s failed (missing source, name taken, or read-only)", p1, p2);
    } else if (!strcmp(name, "open_file")) {
        json_str(args, "path", a, sizeof(a)); resolve(a, p1, sizeof(p1));
        if (vfs_is_dir(p1)) {
            void file_explorer_navigate_to(const char *path);
            desktop_app_launch(desktop_app_find("Files")); file_explorer_navigate_to(p1);
            outf(&o, "Opened the folder %s in Files.", p1);
        } else if (vfs_exists(p1)) { notepad_open_file(p1); outf(&o, "Opened %s in Notepad.", p1); }
        else outf(&o, "error: %s does not exist", p1);
    } else if (!strcmp(name, "system_status")) {
        tool_status(&o);
    } else if (!strcmp(name, "list_processes")) {
        tool_processes(&o);
    } else if (!strcmp(name, "kill_process")) {
        long pid = json_int(args, "pid", 0);
        outf(&o, governor_ai_advise(GOV_TERMINATE, (uint32_t)pid, 0) == 0 ? "Ended process %ld." :
             "error: process %ld does not exist or is protected", pid);
    } else if (!strcmp(name, "suspend_process") || !strcmp(name, "resume_process")) {
        long pid = json_int(args, "pid", 0);
        int rc = governor_ai_advise(name[0] == 's' ? GOV_SUSPEND : GOV_RESUME, (uint32_t)pid, 0);
        outf(&o, rc == 0 ? "Done for process %ld." : "error: process %ld cannot be changed", pid);
    } else if (!strcmp(name, "list_devices")) {
        tool_devices(&o);
    } else if (!strcmp(name, "list_drives")) {
        tool_drives(&o);
    } else if (!strcmp(name, "set_theme")) {
        json_str(args, "mode", a, sizeof(a));
        theme_set_mode(!strcmp(a, "light") ? THEME_LIGHT : !strcmp(a, "high_contrast") ? THEME_HIGH_CONTRAST : THEME_DARK);
        desktop_invalidate();
        outf(&o, "Theme is now %s.", theme_get_name());
    } else if (!strcmp(name, "notify")) {
        json_str(args, "title", a, sizeof(a)); json_str(args, "text", b, sizeof(b));
        notification_show(a[0] ? a : "JARVIS", b, NOTIF_INFO, 6000);
        outf(&o, "Notification shown.");
    } else if (!strcmp(name, "lock_screen")) {
        desktop_lock();
        outf(&o, "Locked.");
    } else if (!strcmp(name, "list_packages")) {
        static pkg_info_t list[24];
        int n = pkg_list(list, 24);
        if (!n) outf(&o, "No apps installed from packages.");
        for (int i = 0; i < n; i++) outf(&o, "%s (%s %s): %s\n", list[i].name, list[i].title, list[i].version, list[i].description);
    } else if (!strcmp(name, "install_package")) {
        char err[96]; pkg_info_t info;
        json_str(args, "path", a, sizeof(a)); resolve(a, p1, sizeof(p1));
        if (pkg_install(p1, &info, err, sizeof(err)) == 0) outf(&o, "Installed %s %s.", info.title, info.version);
        else outf(&o, "error: %s", err);
    } else if (!strcmp(name, "uninstall_app")) {
        json_str(args, "name", a, sizeof(a));
        outf(&o, pkg_uninstall(a) == 0 ? "Uninstalled %s." : "error: no installed app named %s", a);
    } else if (!strcmp(name, "run_program")) {
        json_str(args, "path", a, sizeof(a));
        uint32_t pid = strchr(a, '/') ? (resolve(a, p1, sizeof(p1)), user_exec(p1)) : pkg_run(a);
        if (pid) desktop_app_launch(desktop_app_find("Terminal"));
        outf(&o, pid ? "Started as process %u; its output is in the Terminal." : "error: could not start it", pid);
    } else if (!strcmp(name, "power")) {
        json_str(args, "action", a, sizeof(a));
        send_line("RESULT:", "ok");                         /* best effort before the machine goes down */
        if (!strcmp(a, "restart")) desktop_system_restart(); else desktop_system_shutdown();
        return;
    } else if (!strcmp(name, "network_info")) {
        net_info_t ni; char ip[16], gw[16], dns[16];
        net_get_info(&ni);
        net_format_ip(ni.ip, ip, sizeof(ip)); net_format_ip(ni.gateway, gw, sizeof(gw)); net_format_ip(ni.dns, dns, sizeof(dns));
        if (!ni.adapter) outf(&o, "No network adapter.");
        else outf(&o, "Ethernet, cable %s. %s%s, gateway %s, DNS %s. %u frames received, %u sent.",
                  ni.link ? "connected" : "unplugged", ni.configured ? "Address " : "No address yet", ni.configured ? ip : "",
                  gw, dns, (unsigned)ni.rx_frames, (unsigned)ni.tx_frames);
    } else {
        outf(&o, "error: unknown tool %s", name);
    }

    char head[48];
    snprintf(head, sizeof(head), "RESULT:%s:", id);
    send_line(head, result);
    terminal_printf("[AI TOOL] %s -> %u bytes\n", name, (unsigned)o.len);
}

/* ==========================================================================
 * Incoming lines (called from the desktop loop: tools may touch the UI)
 * ========================================================================== */
int ai_host_line(const char *line)
{
    if (!strncmp(line, "AI:ONLINE", 9)) {
        int first = !governor_ai_online();
        governor_ai_heartbeat();
        const char *m = line[9] == ' ' ? line + 10 : "";
        snprintf(g_model, sizeof(g_model), "%s", m);
        if (first) {
            terminal_printf("[AI] Language model online: %s\n", g_model);
            ui_shell_display("My language model is online.");
        }
        return 1;
    }
    if (!strncmp(line, "AI:THINKING", 11)) {
        governor_ai_heartbeat();
        ui_shell_set_status("THINKING");
        return 1;
    }
    if (!strncmp(line, "REPLY:", 6)) {
        static char text[2048];
        size_t k = 0;
        for (const char *p = line + 6; *p && k < sizeof(text) - 1; p++) {
            if (*p == '\\' && p[1] == 'n') { text[k++] = '\n'; p++; }
            else text[k++] = *p;
        }
        text[k] = 0;
        ui_shell_display(text);
        tts_speak(text, k);
        ui_shell_set_status("READY");
        return 1;
    }
    if (!strncmp(line, "TOOL:", 5)) {
        char id[16] = "", name[32] = "";
        const char *p = line + 5, *c1 = strchr(p, ':');
        const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (!c1 || !c2 || c1 - p >= (long)sizeof(id) || c2 - c1 - 1 >= (long)sizeof(name)) return 1;
        memcpy(id, p, (size_t)(c1 - p)); id[c1 - p] = 0;
        memcpy(name, c1 + 1, (size_t)(c2 - c1 - 1)); name[c2 - c1 - 1] = 0;
        governor_ai_heartbeat();
        if (g_pending.active) {                             /* one confirmation at a time */
            char head[48];
            snprintf(head, sizeof(head), "RESULT:%s:", id);
            send_line(head, "error: another action is waiting for the user's approval");
            return 1;
        }
        run_tool(id, name, c2 + 1, 0);
        return 1;
    }
    return 0;
}
