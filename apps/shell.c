/*
 * =============================================================================
 * JARVIS OS — Command shell (apps/shell.c)
 * Language: C17
 *
 * The command language behind the Terminal, and behind JARVIS's
 * run_command tool. It understands both Linux and Windows habits:
 *
 *   cd, pwd, ls/dir, cat/type, cp/copy, mv/move, rm/del, mkdir/md ...
 *   quoting "with spaces", 'single', variables $HOME %USERPROFILE%,
 *   ~, C:\ and D:\ drive letters, wildcards *.txt,
 *   cmd > file, cmd >> file, cmd < file, a | b | c, a && b, a || b, a ; b
 *
 * Output goes to a sink: the Terminal screen, a pipe buffer, a file, or the
 * buffer JARVIS reads back.
 * =============================================================================
 */

#include <stdarg.h>
int c_killall(int argc, char **argv);

#define SH_MAX_ARGS   48
#define SH_PIPE_MAX   (256 * 1024)
#define SH_HOME       "/jarvis/data"

int  prefs_set(const char *name, const char *value, char *msg, size_t n);
int  prefs_get(const char *name, char *out, size_t n);
void prefs_describe(char *out, size_t n, int with_help);
uint32_t sysmon_spawn_test(const char *kind);                       /* apps/sysmon.c */
const char *sysmon_state_name(const proc_info_t *p, char *buf, size_t n);
void terminal_app_write(const char *text);
void terminal_app_clear(void);
void terminal_app_write_err(const char *text);
void apps_store_show_install(const char *path);
int  ai_tool_exec(const char *name, const char *args, char *out, size_t n);   /* kernel/ai_tools.c */
const char *ai_tool_names(void);
void desktop_take_screenshot(char *saved, size_t n);                  /* desktop_init.c */
void notepad_open_file(const char *path);
void file_explorer_navigate_to(const char *path);
void photos_open_file(const char *path);
void browser_open_file(const char *path);
void shell_open_path(const char *p);
void desktop_apply_settings(void);
void shell_disk_command(int argc, char **argv);                       /* apps/disks.c */

typedef struct { char *buf; size_t len, cap; int overflow; } sh_sink_t;

static struct {
    char cwd[256], oldpwd[256];
    struct { char name[32]; char value[160]; } env[32];
    int nenv;
    struct { char name[24]; char value[160]; } alias[24];
    int nalias;
    sh_sink_t *out;             /* NULL = the Terminal screen               */
    sh_sink_t *root;            /* where errors go (screen or JARVIS buffer) */
    const char *in; size_t in_len; int has_in;      /* piped/redirected stdin */
    int status;                 /* exit status of the last command ($?)     */
    int inited;
    int depth;                  /* alias recursion guard                    */
} sh;

/* ---- output --------------------------------------------------------------- */
static void sink_put(sh_sink_t *s, const char *t)
{
    size_t l = strlen(t);
    if (s->len + l >= s->cap) { l = s->cap - 1 - s->len; s->overflow = 1; }
    memcpy(s->buf + s->len, t, l);
    s->len += l;
    s->buf[s->len] = 0;
}

static void sh_out(const char *t)
{
    if (sh.out) sink_put(sh.out, t);
    else terminal_app_write(t);
}

static void sh_printf(const char *fmt, ...)
{
    static char b[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    sh_out(b);
}

static void sh_err(const char *fmt, ...)
{
    static char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (sh.root) sink_put(sh.root, b);
    else terminal_app_write_err(b);
}

/* ---- environment ---------------------------------------------------------- */
/* case-insensitive helpers */
static int strcasecmp_s(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y || !x) return x - y;
    }
}

static int strstr_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i]) {
            char x = hay[i], y = needle[i];
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
            if (x != y) break;
            i++;
        }
        if (i == n) return 1;
    }
    return 0;
}

static const char *sh_getenv(const char *name)
{
    static char tmp[64];
    for (int i = 0; i < sh.nenv; i++)
        if (!strcmp(sh.env[i].name, name)) return sh.env[i].value;
    if (!strcmp(name, "PWD") || !strcmp(name, "CD")) return sh.cwd;
    if (!strcmp(name, "OLDPWD")) return sh.oldpwd;
    if (!strcmp(name, "?") || !strcmp(name, "ERRORLEVEL")) { snprintf(tmp, sizeof(tmp), "%d", sh.status); return tmp; }
    if (!strcmp(name, "USER") || !strcmp(name, "USERNAME")) return account_name();
    if (!strcmp(name, "HOSTNAME") || !strcmp(name, "COMPUTERNAME")) { prefs_get("device_name", tmp, sizeof(tmp)); return tmp; }
    if (!strcmp(name, "DATE")) { sys_format_date(tmp, sizeof(tmp)); return tmp; }
    if (!strcmp(name, "TIME")) { sys_format_clock(tmp, sizeof(tmp)); return tmp; }
    if (!strcmp(name, "RANDOM")) { snprintf(tmp, sizeof(tmp), "%u", (unsigned)((g_kstate.uptime_ticks * 1103515245u + 12345u) >> 8) % 32768); return tmp; }
    return NULL;
}

static void sh_setenv(const char *name, const char *value)
{
    for (int i = 0; i < sh.nenv; i++)
        if (!strcmp(sh.env[i].name, name)) { snprintf(sh.env[i].value, sizeof(sh.env[i].value), "%s", value); return; }
    if (sh.nenv >= 32) return;
    snprintf(sh.env[sh.nenv].name, sizeof(sh.env[0].name), "%s", name);
    snprintf(sh.env[sh.nenv].value, sizeof(sh.env[0].value), "%s", value);
    sh.nenv++;
}

static void sh_unsetenv(const char *name)
{
    for (int i = 0; i < sh.nenv; i++)
        if (!strcmp(sh.env[i].name, name)) { sh.env[i] = sh.env[--sh.nenv]; return; }
}

static void sh_init(void)
{
    if (sh.inited) return;
    sh.inited = 1;
    snprintf(sh.cwd, sizeof(sh.cwd), "%s", SH_HOME);
    snprintf(sh.oldpwd, sizeof(sh.oldpwd), "%s", SH_HOME);
    sh_setenv("HOME", SH_HOME);
    sh_setenv("USERPROFILE", SH_HOME);
    sh_setenv("PATH", "/jarvis/bin");
    sh_setenv("SHELL", "jsh");
    sh_setenv("OS", "JARVIS OS");
    sh_setenv("DESKTOP", "/jarvis/desktop");
    sh_setenv("DOWNLOADS", "/jarvis/data/Downloads");
    /* variables saved with setx (HKCU\Environment) */
    {
        int k = reg_open_key("HKCU\\Environment", 0);
        char nm[64], val[160];
        int type;
        for (int i = 0; k >= 0 && reg_value_at(k, i, nm, sizeof(nm), &type, val, sizeof(val), NULL) == 0; i++)
            if (nm[0]) sh_setenv(nm, val);
    }
    /* familiar shortcuts */
    static const char *const al[][2] = {
        { "ll", "ls -l" }, { "la", "ls -a" }, { "cls", "clear" }, { "md", "mkdir" }, { "rd", "rmdir" },
        { "..", "cd .." }, { "ipconfig", "ifconfig" }, { "tasklist", "ps" }, { "taskkill", "kill" },
        { "systeminfo", "sysinfo" }, { "ver", "uname -a" }, { "where", "which" }, { "erase", "del" },
    };
    for (size_t i = 0; i < sizeof(al) / sizeof(al[0]); i++) {
        snprintf(sh.alias[sh.nalias].name, sizeof(sh.alias[0].name), "%s", al[i][0]);
        snprintf(sh.alias[sh.nalias].value, sizeof(sh.alias[0].value), "%s", al[i][1]);
        sh.nalias++;
    }
}

const char *shell_cwd(void) { sh_init(); return sh.cwd; }

/* ---- paths ---------------------------------------------------------------- */
/* Turns any spelling (relative, ~, C:\..., D:, backslashes, . and ..) into a
 * clean absolute path. */
static void sh_path(const char *arg, char *out, size_t n)
{
    char tmp[512];
    if (!arg || !*arg) arg = ".";
    char a[256];
    snprintf(a, sizeof(a), "%s", arg);
    for (char *p = a; *p; p++) if (*p == '\\') *p = '/';

    const char *rest = a;
    char base[256];
    if (((a[0] >= 'a' && a[0] <= 'z') || (a[0] >= 'A' && a[0] <= 'Z')) && a[1] == ':') {
        char d = (char)(a[0] | 32);
        if (d == 'c') snprintf(base, sizeof(base), "/jarvis");
        else snprintf(base, sizeof(base), "/jarvis/usb%d", d - 'd');
        rest = a + 2;
        if (*rest == '/') rest++;
        snprintf(tmp, sizeof(tmp), "%s/%s", base, rest);
    } else if (a[0] == '~') {
        snprintf(tmp, sizeof(tmp), "%s%s", SH_HOME, a + 1);
    } else if (a[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", a);
    } else {
        snprintf(tmp, sizeof(tmp), "%s/%s", sh.cwd, a);
    }

    /* normalise into components */
    char *parts[64];
    int np = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/') *p++ = 0;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        if (!strcmp(start, ".")) continue;
        if (!strcmp(start, "..")) { if (np) np--; continue; }
        if (np < 64) parts[np++] = start;
    }
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < np; i++) {
        size_t l = strlen(parts[i]);
        if (o + l + 2 >= n) break;
        out[o++] = '/';
        memcpy(out + o, parts[i], l);
        o += l;
    }
    if (!o) { out[0] = '/'; o = 1; }
    out[o] = 0;
}

static const char *sh_basename(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int sh_exists(const char *p) { return !strcmp(p, "/") || vfs_exists(p) || vfs_is_dir(p); }
static int sh_isdir(const char *p)  { return !strcmp(p, "/") || vfs_is_dir(p); }

/* Pretty path for the prompt: ~ for home. */
static void sh_pretty(const char *p, char *out, size_t n)
{
    size_t hl = strlen(SH_HOME);
    if (!strncmp(p, SH_HOME, hl) && (p[hl] == 0 || p[hl] == '/')) snprintf(out, n, "~%s", p + hl);
    else snprintf(out, n, "%s", p);
}

/* ---- directory listing ------------------------------------------------------ */
typedef struct { char name[96]; int is_dir; long size; } sh_ent_t;
static sh_ent_t g_ents[512];

static int sh_cmp_name(const sh_ent_t *a, const sh_ent_t *b)
{
    const char *x = a->name, *y = b->name;
    for (;; x++, y++) {
        char cx = *x >= 'A' && *x <= 'Z' ? (char)(*x + 32) : *x, cy = *y >= 'A' && *y <= 'Z' ? (char)(*y + 32) : *y;
        if (cx != cy || !cx) return cx - cy;
    }
}

/* Direct children of dir, sorted (folders first). Returns count or -1. */
static int sh_readdir(const char *dir, sh_ent_t *ents, int max)
{
    static char listing[32768];
    int n = 0;
    if (!strcmp(dir, "/")) {
        snprintf(ents[0].name, sizeof(ents[0].name), "jarvis");
        ents[0].is_dir = 1; ents[0].size = 0;
        return 1;
    }
    if (!vfs_is_dir(dir)) return -1;
    if (vfs_list(dir, listing, sizeof(listing)) < 0) return -1;
    size_t dlen = strlen(dir);
    for (const char *p = listing; *p && n < max;) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        int is_dir = !strncmp(p, "[dir] ", 6), is_file = !strncmp(p, "[file] ", 7);
        const char *full = p + (is_dir ? 6 : 7);
        size_t flen = (size_t)(eol - full);
        if ((is_dir || is_file) && flen > dlen + 1 && !strncmp(full, dir, dlen) && full[dlen] == '/') {
            const char *name = full + dlen + 1;
            size_t nl = flen - dlen - 1;
            if (!memchr(name, '/', nl) && nl < sizeof(ents[0].name)) {
                int dup = 0;
                for (int k = 0; k < n; k++) if (strlen(ents[k].name) == nl && !strncmp(ents[k].name, name, nl)) dup = 1;
                if (!dup) {
                    memcpy(ents[n].name, name, nl);
                    ents[n].name[nl] = 0;
                    ents[n].is_dir = is_dir;
                    char fp[512];
                    snprintf(fp, sizeof(fp), "%s/%s", dir, ents[n].name);
                    if (is_dir && !vfs_is_dir(fp)) ents[n].is_dir = 0;
                    ents[n].size = is_dir ? 0 : vfs_size(fp);
                    n++;
                }
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    /* insertion sort: folders first, then by name */
    for (int i = 1; i < n; i++) {
        sh_ent_t t = ents[i];
        int j = i - 1;
        while (j >= 0 && (ents[j].is_dir < t.is_dir || (ents[j].is_dir == t.is_dir && sh_cmp_name(&ents[j], &t) > 0))) {
            ents[j + 1] = ents[j];
            j--;
        }
        ents[j + 1] = t;
    }
    return n;
}

static int sh_wild(const char *pat, const char *s)
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            while (pat[1] == '*') pat++;
            if (!pat[1]) return 1;
            for (; *s; s++) if (sh_wild(pat + 1, s)) return 1;
            return 0;
        }
        char a = *pat >= 'A' && *pat <= 'Z' ? (char)(*pat + 32) : *pat;
        char b = *s >= 'A' && *s <= 'Z' ? (char)(*s + 32) : *s;
        if (!*s || (*pat != '?' && a != b)) return 0;
    }
    return !*s;
}

static void sh_size_str(long b, char *o, size_t n)
{
    if (b < 1024) snprintf(o, n, "%ld", b);
    else if (b < 1024 * 1024) snprintf(o, n, "%ld.%ldK", b / 1024, (b % 1024) * 10 / 1024);
    else if (b < 1024L * 1024 * 1024) snprintf(o, n, "%ld.%ldM", b >> 20, ((b & 0xFFFFF) * 10) >> 20);
    else snprintf(o, n, "%ld.%ldG", b >> 30, ((b & 0x3FFFFFFF) * 10) >> 30);
}

/* ---- file input (for cat, grep, sort ... with files or a pipe) ------------- */
static char *g_rbuf;
static size_t g_rcap;

/* Reads a whole file into a shared buffer. Returns length or -1. */
static long sh_read_file(const char *path, const char **data)
{
    long size = vfs_size(path);
    if (size < 0 || vfs_is_dir(path)) return -1;
    if ((size_t)size + 1 > g_rcap) {
        if (g_rbuf) { if (g_rcap >= LARGE_ALLOC_THRESHOLD) kfree_large(g_rbuf, g_rcap); else kfree(g_rbuf); }
        g_rcap = (size_t)size + 1 < 65536 ? 65536 : (size_t)size + 1;
        g_rbuf = (char *)(g_rcap >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(g_rcap) : kmalloc(g_rcap));
        if (!g_rbuf) { g_rcap = 0; return -1; }
    }
    int got = size ? vfs_read_file(path, g_rbuf, (size_t)size + 1) : 0;
    if (got < 0) return -1;
    g_rbuf[got] = 0;
    *data = g_rbuf;
    return got;
}

/* Input of a filter command: the named file, or the pipe. */
static long sh_input(const char *file, const char **data, const char *cmd)
{
    if (file) {
        char p[256];
        sh_path(file, p, sizeof(p));
        long n = sh_read_file(p, data);
        if (n < 0) sh_err("%s: %s: no such file\n", cmd, file);
        return n;
    }
    if (sh.has_in) { *data = sh.in; return (long)sh.in_len; }
    sh_err("%s: give a file name, or pipe text into it (e.g. ls | %s)\n", cmd, cmd);
    return -1;
}

/* Iterate lines of a buffer. */
static const char *sh_next_line(const char *p, const char *end, size_t *len)
{
    if (p >= end) return NULL;
    const char *e = p;
    while (e < end && *e != '\n') e++;
    *len = (size_t)(e - p);
    if (*len && p[*len - 1] == '\r') (*len)--;
    return p;
}

static void sh_out_n(const char *s, size_t n)
{
    char b[512];
    while (n) {
        size_t k = n < sizeof(b) - 1 ? n : sizeof(b) - 1;
        memcpy(b, s, k);
        b[k] = 0;
        sh_out(b);
        s += k; n -= k;
    }
}

/* ---- option parsing --------------------------------------------------------- */
/* Collects single-letter flags (-l -a -la /s) into a bitmask; returns the
 * index of the first non-option argument. */
static int sh_flags(int argc, char **argv, const char *allowed, unsigned *flags)
{
    *flags = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] != '-' && a[0] != '/') || !a[1] || (a[0] == '/' && strlen(a) > 2)) break;
        if (!strcmp(a, "--")) { i++; break; }
        int ok = 1;
        for (const char *c = a + 1; *c; c++) if (!strchr(allowed, *c | 32)) ok = 0;
        if (!ok) break;
        for (const char *c = a + 1; *c; c++) *flags |= 1u << ((*c | 32) - 'a');
    }
    return i;
}
#define F(c) (1u << ((c) - 'a'))

static long sh_num(const char *s, long dflt)
{
    if (!s || !*s) return dflt;
    long v = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    if (!(*s >= '0' && *s <= '9')) return dflt;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* =============================================================================
 * COMMANDS
 * ============================================================================= */
typedef int (*sh_fn)(int argc, char **argv);
typedef struct { const char *name; sh_fn fn; const char *usage; const char *help; const char *group; } sh_cmd_t;
static const sh_cmd_t *sh_find(const char *name);
static int sh_exec_line(const char *line);

/* ---- navigation ---- */
static int c_pwd(int argc, char **argv) { (void)argc; (void)argv; sh_printf("%s\n", sh.cwd); return 0; }

static int c_cd(int argc, char **argv)
{
    char p[256];
    const char *t = argc > 1 ? argv[1] : SH_HOME;
    if (!strcmp(t, "-")) t = sh.oldpwd;
    sh_path(t, p, sizeof(p));
    if (!sh_isdir(p)) { sh_err("cd: %s: no such folder\n", t); return 1; }
    snprintf(sh.oldpwd, sizeof(sh.oldpwd), "%s", sh.cwd);
    snprintf(sh.cwd, sizeof(sh.cwd), "%s", p);
    return 0;
}

static void ls_one_dir(const char *dir, unsigned fl, int show_header)
{
    static sh_ent_t ents[512];
    int n = sh_readdir(dir, ents, 512);
    if (n < 0) { sh_err("ls: %s: no such folder\n", dir); return; }
    if (show_header) sh_printf("%s:\n", dir);
    int shown = 0;
    long total = 0;
    for (int i = 0; i < n; i++) {
        if (!(fl & F('a')) && ents[i].name[0] == '.') continue;
        shown++;
        total += ents[i].size;
        if (fl & F('l')) {
            char sz[16];
            if (fl & F('h')) sh_size_str(ents[i].size, sz, sizeof(sz)); else snprintf(sz, sizeof(sz), "%ld", ents[i].size);
            sh_printf("%s %10s  %s%s\n", ents[i].is_dir ? "d" : "-", ents[i].is_dir ? "<DIR>" : sz,
                      ents[i].name, ents[i].is_dir ? "/" : "");
        } else {
            sh_printf("%s%s\n", ents[i].name, ents[i].is_dir ? "/" : "");
        }
    }
    if (fl & F('l')) { char sz[16]; sh_size_str(total, sz, sizeof(sz)); sh_printf("%d item(s), %s bytes\n", shown, sz); }
    if (fl & F('r')) {
        for (int i = 0; i < n; i++) {
            if (!ents[i].is_dir || (!(fl & F('a')) && ents[i].name[0] == '.')) continue;
            static char sub[512];
            snprintf(sub, sizeof(sub), "%s/%s", !strcmp(dir, "/") ? "" : dir, ents[i].name);
            static sh_ent_t save[512];
            memcpy(save, ents, sizeof(sh_ent_t) * (size_t)n);
            sh_out("\n");
            ls_one_dir(sub, fl, 1);
            memcpy(ents, save, sizeof(sh_ent_t) * (size_t)n);
        }
    }
}

static int c_ls(int argc, char **argv)
{
    unsigned fl;
    int i = sh_flags(argc, argv, "lahrs", &fl);
    if (fl & F('s')) fl |= F('r');                      /* dir /s */
    if (!strcmp(argv[0], "dir")) fl |= F('l');
    if (i >= argc) { ls_one_dir(sh.cwd, fl, 0); return 0; }
    int rc = 0;
    for (; i < argc; i++) {
        char p[256];
        sh_path(argv[i], p, sizeof(p));
        if (sh_isdir(p)) ls_one_dir(p, fl, argc - i > 1 || (fl & F('r')));
        else if (vfs_exists(p)) sh_printf("%s  %ld bytes\n", argv[i], vfs_size(p));
        else { sh_err("ls: %s: not found\n", argv[i]); rc = 1; }
    }
    return rc;
}

static void tree_walk(const char *dir, const char *prefix, int depth, int *nd, int *nf)
{
    sh_ent_t *ents = (sh_ent_t *)kmalloc(sizeof(sh_ent_t) * 256);
    if (!ents) return;
    int n = sh_readdir(dir, ents, 256);
    for (int i = 0; i < n; i++) {
        int last = i == n - 1;
        sh_printf("%s%s%s%s\n", prefix, last ? "`-- " : "|-- ", ents[i].name, ents[i].is_dir ? "/" : "");
        if (ents[i].is_dir) {
            (*nd)++;
            if (depth < 12) {
                char sub[512], pre[256];
                snprintf(sub, sizeof(sub), "%s/%s", !strcmp(dir, "/") ? "" : dir, ents[i].name);
                snprintf(pre, sizeof(pre), "%s%s", prefix, last ? "    " : "|   ");
                tree_walk(sub, pre, depth + 1, nd, nf);
            }
        } else (*nf)++;
    }
    kfree(ents);
}

static int c_tree(int argc, char **argv)
{
    char p[256];
    sh_path(argc > 1 ? argv[1] : ".", p, sizeof(p));
    if (!sh_isdir(p)) { sh_err("tree: %s: no such folder\n", argc > 1 ? argv[1] : "."); return 1; }
    int nd = 0, nf = 0;
    sh_printf("%s\n", p);
    tree_walk(p, "", 0, &nd, &nf);
    sh_printf("\n%d folder(s), %d file(s)\n", nd, nf);
    return 0;
}

/* ---- file contents ---- */
static int c_cat(int argc, char **argv)
{
    unsigned fl;
    int i = sh_flags(argc, argv, "n", &fl);
    const char *d;
    int rc = 0, line = 1;
    if (i >= argc) {
        long n = sh_input(NULL, &d, "cat");
        if (n < 0) return 1;
        sh_out_n(d, (size_t)n);
        return 0;
    }
    for (; i < argc; i++) {
        long n = sh_input(argv[i], &d, argv[0]);
        if (n < 0) { rc = 1; continue; }
        if (fl & F('n')) {
            const char *p = d, *end = d + n;
            size_t l;
            while ((p = sh_next_line(p, end, &l))) { sh_printf("%6d  ", line++); sh_out_n(p, l); sh_out("\n"); p += l + 1; if (p > end) break; }
        } else {
            sh_out_n(d, (size_t)n);
            if (n && d[n - 1] != '\n') sh_out("\n");
        }
    }
    return rc;
}

static int c_headtail(int argc, char **argv)
{
    int tail = argv[0][0] == 't';
    long count = 10;
    int i = 1;
    if (i < argc && argv[i][0] == '-' && argv[i][1] == 'n') { count = sh_num(argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "10"), 10); i++; }
    else if (i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') { count = sh_num(argv[i] + 1, 10); i++; }
    const char *d;
    long n = sh_input(i < argc ? argv[i] : NULL, &d, argv[0]);
    if (n < 0) return 1;
    const char *end = d + n;
    long total = 0;
    for (const char *p = d; p < end; p++) if (*p == '\n') total++;
    if (n && d[n - 1] != '\n') total++;
    long from = tail ? total - count : 0, to = tail ? total : count;
    long ln = 0;
    size_t l;
    for (const char *p = d; (p = sh_next_line(p, end, &l)); p += l + 1, ln++) {
        if (ln >= from && ln < to) { sh_out_n(p, l); sh_out("\n"); }
        if (p + l >= end) break;
    }
    return 0;
}

static int c_wc(int argc, char **argv)
{
    unsigned fl;
    int i = sh_flags(argc, argv, "lwc", &fl);
    if (!fl) fl = F('l') | F('w') | F('c');
    const char *d;
    long n = sh_input(i < argc ? argv[i] : NULL, &d, "wc");
    if (n < 0) return 1;
    long lines = 0, words = 0, inword = 0;
    for (long k = 0; k < n; k++) {
        if (d[k] == '\n') lines++;
        int sp = d[k] == ' ' || d[k] == '\n' || d[k] == '\t' || d[k] == '\r';
        if (!sp && !inword) words++;
        inword = !sp;
    }
    if (fl & F('l')) sh_printf("%ld ", lines);
    if (fl & F('w')) sh_printf("%ld ", words);
    if (fl & F('c')) sh_printf("%ld ", n);
    sh_printf("%s\n", i < argc ? argv[i] : "");
    return 0;
}

static int sh_contains(const char *s, size_t l, const char *pat, int icase)
{
    size_t pl = strlen(pat);
    if (!pl) return 1;
    for (size_t i = 0; i + pl <= l; i++) {
        size_t k = 0;
        for (; k < pl; k++) {
            char a = s[i + k], b = pat[k];
            if (icase) { if (a >= 'A' && a <= 'Z') a = (char)(a + 32); if (b >= 'A' && b <= 'Z') b = (char)(b + 32); }
            if (a != b) break;
        }
        if (k == pl) return 1;
    }
    return 0;
}

static int grep_buf(const char *label, const char *d, long n, const char *pat, unsigned fl, long *count)
{
    const char *end = d + n;
    size_t l;
    long ln = 0;
    int any = 0;
    for (const char *p = d; (p = sh_next_line(p, end, &l)); p += l + 1) {
        ln++;
        int m = sh_contains(p, l, pat, (fl & F('i')) != 0);
        if (fl & F('v')) m = !m;
        if (m) {
            any = 1;
            (*count)++;
            if (!(fl & F('c')) && !(fl & F('l'))) {
                if (label) sh_printf("%s:", label);
                if (fl & F('n')) sh_printf("%ld:", ln);
                sh_out_n(p, l);
                sh_out("\n");
            }
        }
        if (p + l >= end) break;
    }
    if (any && (fl & F('l')) && label) sh_printf("%s\n", label);
    return any;
}

static void grep_tree(const char *dir, const char *pat, unsigned fl, long *count, int *any)
{
    sh_ent_t *ents = (sh_ent_t *)kmalloc(sizeof(sh_ent_t) * 256);
    if (!ents) return;
    int n = sh_readdir(dir, ents, 256);
    for (int i = 0; i < n; i++) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, ents[i].name);
        if (ents[i].is_dir) { grep_tree(full, pat, fl, count, any); continue; }
        if (ents[i].size > 4L * 1024 * 1024) continue;
        const char *d;
        long len = sh_read_file(full, &d);
        if (len > 0 && !memchr(d, 0, (size_t)len)) *any |= grep_buf(full, d, len, pat, fl, count);
    }
    kfree(ents);
}

static int c_grep(int argc, char **argv)
{
    unsigned fl;
    int i = sh_flags(argc, argv, "ivncrlis", &fl);
    if (i >= argc) { sh_err("usage: grep [-i] [-v] [-n] [-c] [-r] <text> [file...]\n"); return 2; }
    const char *pat = argv[i++];
    long count = 0;
    int any = 0;
    if (fl & (F('r') | F('s'))) {
        char p[256];
        sh_path(i < argc ? argv[i] : ".", p, sizeof(p));
        grep_tree(p, pat, fl, &count, &any);
    } else if (i >= argc) {
        const char *d;
        long n = sh_input(NULL, &d, "grep");
        if (n < 0) return 2;
        any = grep_buf(NULL, d, n, pat, fl, &count);
    } else {
        int multi = argc - i > 1;
        for (; i < argc; i++) {
            const char *d;
            long n = sh_input(argv[i], &d, "grep");
            if (n >= 0) any |= grep_buf(multi ? argv[i] : NULL, d, n, pat, fl, &count);
        }
    }
    if (fl & F('c')) sh_printf("%ld\n", count);
    return any ? 0 : 1;
}

static void find_walk(const char *dir, const char *pat, int type, long *found, int depth)
{
    sh_ent_t *ents = (sh_ent_t *)kmalloc(sizeof(sh_ent_t) * 256);
    if (!ents) return;
    int n = sh_readdir(dir, ents, 256);
    for (int i = 0; i < n; i++) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", !strcmp(dir, "/") ? "" : dir, ents[i].name);
        int ok = (!pat || sh_wild(pat, ents[i].name)) && (!type || (type == 'd') == ents[i].is_dir);
        if (ok) { sh_printf("%s%s\n", full, ents[i].is_dir ? "/" : ""); (*found)++; }
        if (ents[i].is_dir && depth < 16) find_walk(full, pat, type, found, depth + 1);
    }
    kfree(ents);
}

static int c_find(int argc, char **argv)
{
    const char *start = ".", *pat = NULL;
    int type = 0;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-name") || !strcmp(argv[i], "-iname")) && i + 1 < argc) pat = argv[++i];
        else if (!strcmp(argv[i], "-type") && i + 1 < argc) type = argv[++i][0];
        else if (argv[i][0] != '-') { if (i == 1) start = argv[i]; else pat = argv[i]; }
    }
    char p[256];
    sh_path(start, p, sizeof(p));
    if (!sh_isdir(p)) { sh_err("find: %s: no such folder\n", start); return 1; }
    char wpat[128];
    if (pat && !strchr(pat, '*') && !strchr(pat, '?')) { snprintf(wpat, sizeof(wpat), "*%s*", pat); pat = wpat; }
    long found = 0;
    find_walk(p, pat, type, &found, 0);
    if (!found) sh_err("find: nothing found\n");
    return found ? 0 : 1;
}

/* ---- file management ---- */
static int c_touch(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: touch <file>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char p[256];
        sh_path(argv[i], p, sizeof(p));
        if (sh_exists(p)) continue;
        if (vfs_create_file(p) != 0) { sh_err("touch: cannot create %s\n", argv[i]); rc = 1; }
    }
    return rc;
}

static int mkdir_p(const char *p)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", p);
    for (char *s = tmp + 1; *s; s++)
        if (*s == '/') { *s = 0; if (!vfs_is_dir(tmp) && vfs_mkdir(tmp) != 0) return -1; *s = '/'; }
    return vfs_is_dir(tmp) ? 0 : vfs_mkdir(tmp);
}

static int c_mkdir(int argc, char **argv)
{
    unsigned fl;
    int i = sh_flags(argc, argv, "p", &fl);
    if (i >= argc) { sh_err("usage: mkdir [-p] <folder>...\n"); return 1; }
    int rc = 0;
    for (; i < argc; i++) {
        char p[256];
        sh_path(argv[i], p, sizeof(p));
        if (sh_exists(p) && !(fl & F('p'))) { sh_err("mkdir: %s already exists\n", argv[i]); rc = 1; continue; }
        if (mkdir_p(p)) { sh_err("mkdir: cannot create %s\n", argv[i]); rc = 1; }
    }
    return rc;
}

static int sh_protected(const char *p)
{
    static const char *const fixed[] = { "/", "/jarvis", "/jarvis/bin", "/jarvis/data", "/jarvis/desktop", "/jarvis/models",
                                         "/jarvis/system", "/jarvis/trash", "/jarvis/data/Downloads", "/jarvis/apps" };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        if (!strcmp(p, fixed[i])) return 1;
    return vfs_is_private(p);
}

static int c_rmdir(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) { sh_err("usage: rmdir <folder>\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        char p[256];
        sh_path(argv[i], p, sizeof(p));
        if (!vfs_is_dir(p)) { sh_err("rmdir: %s: not a folder\n", argv[i]); rc = 1; }
        else if (sh_protected(p)) { sh_err("rmdir: %s is a system folder\n", argv[i]); rc = 1; }
        else if (vfs_count_children(p) > 0) { sh_err("rmdir: %s is not empty (use rm -r)\n", argv[i]); rc = 1; }
        else if (vfs_delete(p)) { sh_err("rmdir: cannot remove %s\n", argv[i]); rc = 1; }
    }
    return rc;
}

static int c_rm(int argc, char **argv)
{
    unsigned fl;
    int i = sh_flags(argc, argv, "rfsqv", &fl);
    if (fl & F('s')) fl |= F('r');
    if (i >= argc) { sh_err("usage: rm [-r] [-f] <path>...\n"); return 1; }
    int rc = 0;
    for (; i < argc; i++) {
        char p[256];
        sh_path(argv[i], p, sizeof(p));
        if (!sh_exists(p)) { if (!(fl & F('f'))) { sh_err("rm: %s: not found\n", argv[i]); rc = 1; } continue; }
        size_t pl = strlen(p);
        if (!strncmp(sh.cwd, p, pl) && (sh.cwd[pl] == 0 || sh.cwd[pl] == '/')) { sh_err("rm: %s contains the current folder; cd out of it first\n", argv[i]); rc = 1; continue; }
        if (sh_protected(p)) { sh_err("rm: %s is a system folder and cannot be deleted\n", argv[i]); rc = 1; continue; }
        if (vfs_is_dir(p) && !(fl & F('r')) && vfs_count_children(p) > 0) { sh_err("rm: %s is a folder (use rm -r)\n", argv[i]); rc = 1; continue; }
        if (vfs_delete(p)) { sh_err("rm: cannot delete %s%s\n", argv[i], fatfs_owns(p) ? " (USB drive is read-only)" : ""); rc = 1; }
        else if (fl & F('v')) sh_printf("removed %s\n", p);
    }
    desktop_files_changed();
    return rc;
}

/* cp/mv: last argument is the destination; into a folder if it is one. */
static int c_cpmv(int argc, char **argv)
{
    int mv = argv[0][0] == 'm' || !strcmp(argv[0], "ren") || !strcmp(argv[0], "rename");
    unsigned fl;
    int i = sh_flags(argc, argv, "rfvy", &fl);
    if (argc - i < 2) { sh_err("usage: %s <source>... <destination>\n", argv[0]); return 1; }
    char dst[256];
    sh_path(argv[argc - 1], dst, sizeof(dst));
    int dst_dir = sh_isdir(dst), many = argc - i > 2, rc = 0;
    if (many && !dst_dir) { sh_err("%s: %s is not a folder\n", argv[0], argv[argc - 1]); return 1; }
    for (; i < argc - 1; i++) {
        char src[256], to[512];
        sh_path(argv[i], src, sizeof(src));
        if (!sh_exists(src)) { sh_err("%s: %s: not found\n", argv[0], argv[i]); rc = 1; continue; }
        if (dst_dir) snprintf(to, sizeof(to), "%s/%s", dst, sh_basename(src));
        else snprintf(to, sizeof(to), "%s", dst);
        if (!strcmp(src, to)) continue;
        if (mv && sh_protected(src)) { sh_err("mv: %s is a system folder\n", argv[i]); rc = 1; continue; }
        if (vfs_is_dir(src) && !mv && !(fl & F('r')) && strcmp(argv[0], "copy") && strcmp(argv[0], "xcopy")) { sh_err("cp: %s is a folder (use cp -r)\n", argv[i]); rc = 1; continue; }
        if (sh_exists(to) && !vfs_is_dir(to)) vfs_delete(to);          /* overwrite, like cp/copy /y */
        int r = mv ? vfs_rename(src, to) : vfs_copy(src, to);
        if (r) { sh_err("%s: cannot %s %s to %s\n", argv[0], mv ? "move" : "copy", argv[i], to); rc = 1; }
        else if (fl & F('v')) sh_printf("%s -> %s\n", src, to);
    }
    desktop_files_changed();
    return rc;
}

static int c_stat(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: stat <path>\n"); return 1; }
    char p[256];
    sh_path(argv[1], p, sizeof(p));
    if (!sh_exists(p)) { sh_err("stat: %s: not found\n", argv[1]); return 1; }
    char sz[16];
    long size = vfs_is_dir(p) ? vfs_size(p) : vfs_size(p);
    sh_size_str(size, sz, sizeof(sz));
    sh_printf("  Path:  %s\n  Type:  %s\n  Size:  %ld bytes (%s)\n", p, vfs_is_dir(p) ? "folder" : "file", size, sz);
    if (vfs_is_dir(p)) sh_printf("  Items: %d\n", vfs_count_children(p));
    sh_printf("  Store: %s\n", fatfs_owns(p) ? "USB drive (FAT)" : "JARVIS disk");
    return 0;
}

static int c_file(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: file <path>\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        char p[256];
        sh_path(argv[i], p, sizeof(p));
        if (vfs_is_dir(p)) { sh_printf("%s: folder\n", argv[i]); continue; }
        uint8_t h[16] = { 0 };
        char hb[17];
        int n = vfs_read_file(p, hb, sizeof(hb));
        if (n < 0) { sh_err("%s: not found\n", argv[i]); continue; }
        memcpy(h, hb, (size_t)(n > 16 ? 16 : n));
        const char *t = "data";
        if (n == 0) t = "empty";
        else if (!memcmp(h, "\x89PNG", 4)) t = "PNG image";
        else if (h[0] == 0xFF && h[1] == 0xD8) t = "JPEG image";
        else if (h[0] == 'B' && h[1] == 'M') t = "BMP image";
        else if (!memcmp(h, "\x7f" "ELF", 4)) t = "ELF 64-bit program";
        else if (!memcmp(h, "JPK", 3)) t = "JARVIS package (.jpk)";
        else if (!memcmp(h, "PK", 2)) t = "ZIP archive";
        else if (!memcmp(h, "%PDF", 4)) t = "PDF document";
        else if (!memcmp(h, "RIFF", 4)) t = "RIFF (WAV audio)";
        else if (!memcmp(h, "GGUF", 4)) t = "GGUF AI model";
        else {
            int text = 1;
            for (int k = 0; k < n; k++) if (h[k] < 9 || (h[k] > 13 && h[k] < 32)) text = 0;
            if (text) t = "text";
        }
        sh_printf("%s: %s, %ld bytes\n", argv[i], t, vfs_size(p));
    }
    return 0;
}

static long du_walk(const char *p)
{
    if (!vfs_is_dir(p)) return vfs_size(p) > 0 ? vfs_size(p) : 0;
    long sum = 0;
    sh_ent_t *ents = (sh_ent_t *)kmalloc(sizeof(sh_ent_t) * 256);
    if (!ents) return 0;
    int n = sh_readdir(p, ents, 256);
    for (int i = 0; i < n; i++) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", p, ents[i].name);
        sum += ents[i].is_dir ? du_walk(full) : ents[i].size;
    }
    kfree(ents);
    return sum;
}

static int c_du(int argc, char **argv)
{
    unsigned fl;
    int i = sh_flags(argc, argv, "sh", &fl);
    char p[256];
    sh_path(i < argc ? argv[i] : ".", p, sizeof(p));
    if (!sh_exists(p)) { sh_err("du: not found\n"); return 1; }
    if (!(fl & F('s')) && vfs_is_dir(p)) {
        sh_ent_t *ents = (sh_ent_t *)kmalloc(sizeof(sh_ent_t) * 256);
        int n = ents ? sh_readdir(p, ents, 256) : 0;
        for (int k = 0; k < n; k++) {
            if (!ents[k].is_dir) continue;
            char full[512], sz[16];
            snprintf(full, sizeof(full), "%s/%s", p, ents[k].name);
            sh_size_str(du_walk(full), sz, sizeof(sz));
            sh_printf("%8s  %s\n", sz, full);
        }
        if (ents) kfree(ents);
    }
    char sz[16];
    sh_size_str(du_walk(p), sz, sizeof(sz));
    sh_printf("%8s  %s\n", sz, p);
    return 0;
}

static int c_df(int argc, char **argv)
{
    (void)argc; (void)argv;
    static drive_info_t dr[8];
    int n = storage_list(dr, 8);
    sh_printf("%-22s %-9s %-10s %10s %10s  %s\n", "Drive", "Device", "Type", "Size", "Free", "Folder");
    for (int i = 0; i < n; i++) {
        char t[16], f[16];
        sh_size_str((long)dr[i].total, t, sizeof(t));
        sh_size_str((long)dr[i].free, f, sizeof(f));
        sh_printf("%-22s %-9s %-10s %10s %10s  %s%s\n", dr[i].name, dr[i].device[0] ? dr[i].device : "memory", dr[i].fs,
                  t, f, dr[i].mount, dr[i].read_only ? " (read-only)" : "");
    }
    return 0;
}

static int c_echo(int argc, char **argv)
{
    int i = 1, nl = 1;
    if (i < argc && !strcmp(argv[i], "-n")) { nl = 0; i++; }
    if (argc == 2 && !strcmp(argv[1], ".")) { sh_out("\n"); return 0; }   /* echo. */
    for (; i < argc; i++) { sh_out(argv[i]); if (i + 1 < argc) sh_out(" "); }
    if (nl) sh_out("\n");
    return 0;
}

static int c_tee(int argc, char **argv)
{
    unsigned fl;
    int i = sh_flags(argc, argv, "a", &fl);
    const char *d = sh.has_in ? sh.in : "";
    size_t n = sh.has_in ? sh.in_len : 0;
    for (; i < argc; i++) {
        char p[256];
        sh_path(argv[i], p, sizeof(p));
        if (fl & F('a')) vfs_append(p, d, n); else vfs_write_file(p, d, n);
    }
    sh_out_n(d, n);
    return 0;
}

static int line_cmp(const char *a, const char *b, int numeric, int icase)
{
    if (numeric) {
        long x = sh_num(a, 0), y = sh_num(b, 0);
        if (x != y) return x < y ? -1 : 1;
    }
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (icase) { if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32); if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32); }
        if (ca != cb || !ca) return (unsigned char)ca - (unsigned char)cb;
    }
}

static int c_sort(int argc, char **argv)
{
    unsigned fl;
    int i = sh_flags(argc, argv, "rnuf", &fl);
    const char *d;
    long n = sh_input(i < argc ? argv[i] : NULL, &d, "sort");
    if (n < 0) return 1;
    size_t cap = (size_t)n + 1;
    char *copy = (char *)(cap >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(cap) : kmalloc(cap));
    static char *lines[8192];
    if (!copy) return 1;
    memcpy(copy, d, (size_t)n);
    copy[n] = 0;
    int nl = 0;
    for (char *p = copy; *p && nl < 8192;) {
        lines[nl++] = p;
        char *e = strchr(p, '\n');
        if (!e) break;
        *e = 0;
        p = e + 1;
    }
    for (int a = 1; a < nl; a++) {                      /* insertion sort, stable */
        char *t = lines[a];
        int b = a - 1;
        while (b >= 0) {
            int c = line_cmp(lines[b], t, (fl & F('n')) != 0, (fl & F('f')) != 0);
            if (fl & F('r')) c = -c;
            if (c <= 0) break;
            lines[b + 1] = lines[b];
            b--;
        }
        lines[b + 1] = t;
    }
    for (int a = 0; a < nl; a++) {
        if ((fl & F('u')) && a && !strcmp(lines[a], lines[a - 1])) continue;
        sh_out(lines[a]);
        sh_out("\n");
    }
    if (cap >= LARGE_ALLOC_THRESHOLD) kfree_large(copy, cap); else kfree(copy);
    return 0;
}

static int c_uniq(int argc, char **argv)
{
    unsigned fl;
    int i = sh_flags(argc, argv, "c", &fl);
    const char *d;
    long n = sh_input(i < argc ? argv[i] : NULL, &d, "uniq");
    if (n < 0) return 1;
    const char *end = d + n, *prev = NULL;
    size_t pl = 0, l;
    long cnt = 0;
    for (const char *p = d;; p += l + 1) {
        p = sh_next_line(p, end, &l);
        if (!p || !prev || pl != l || memcmp(p, prev, l)) {
            if (prev) { if (fl & F('c')) sh_printf("%7ld ", cnt); sh_out_n(prev, pl); sh_out("\n"); }
            if (!p) break;
            prev = p; pl = l; cnt = 0;
        }
        cnt++;
        if (p + l >= end) { if (fl & F('c')) sh_printf("%7ld ", cnt); sh_out_n(prev, pl); sh_out("\n"); break; }
    }
    return 0;
}

static int c_hexdump(int argc, char **argv)
{
    const char *d;
    long n = sh_input(argc > 1 ? argv[argc - 1] : NULL, &d, "hexdump");
    if (n < 0) return 1;
    long max = n > 4096 ? 4096 : n;
    for (long off = 0; off < max; off += 16) {
        char line[96];
        int o = snprintf(line, sizeof(line), "%08lx  ", off);
        for (int k = 0; k < 16; k++) {
            if (off + k < n) o += snprintf(line + o, sizeof(line) - (size_t)o, "%02x ", (unsigned)(uint8_t)d[off + k]);
            else o += snprintf(line + o, sizeof(line) - (size_t)o, "   ");
            if (k == 7) line[o++] = ' ';
        }
        line[o++] = ' '; line[o++] = '|';
        for (int k = 0; k < 16 && off + k < n; k++) { uint8_t c = (uint8_t)d[off + k]; line[o++] = c >= 32 && c < 127 ? (char)c : '.'; }
        line[o++] = '|'; line[o++] = '\n'; line[o] = 0;
        sh_out(line);
    }
    if (n > max) sh_printf("... (%ld more bytes)\n", n - max);
    return 0;
}

/* SHA-256 (FIPS 180-4) */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(uint32_t *h, const uint8_t *b)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static int c_hash(int argc, char **argv)
{
    int crc = !strcmp(argv[0], "crc32") || !strcmp(argv[0], "cksum");
    if (argc < 2 && !sh.has_in) { sh_err("usage: %s <file>\n", argv[0]); return 1; }
    for (int i = argc < 2 ? 0 : 1; i < (argc < 2 ? 1 : argc); i++) {
        const char *d;
        long n = sh_input(argc < 2 ? NULL : argv[i], &d, argv[0]);
        if (n < 0) continue;
        if (crc) {
            uint32_t c = 0xFFFFFFFFu;
            for (long k = 0; k < n; k++) { c ^= (uint8_t)d[k]; for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
            sh_printf("%08x  %ld  %s\n", ~c, n, argc < 2 ? "-" : argv[i]);
            continue;
        }
        uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
        long k = 0;
        for (; k + 64 <= n; k += 64) sha256_block(h, (const uint8_t *)d + k);
        uint8_t tail[128] = { 0 };
        long rem = n - k;
        memcpy(tail, d + k, (size_t)rem);
        tail[rem] = 0x80;
        int blocks = rem + 9 > 64 ? 2 : 1;
        uint64_t bits = (uint64_t)n * 8;
        for (int b = 0; b < 8; b++) tail[blocks * 64 - 1 - b] = (uint8_t)(bits >> (8 * b));
        for (int b = 0; b < blocks; b++) sha256_block(h, tail + b * 64);
        for (int b = 0; b < 8; b++) sh_printf("%08x", h[b]);
        sh_printf("  %s\n", argc < 2 ? "-" : argv[i]);
    }
    return 0;
}

static int c_basename(int argc, char **argv) { if (argc < 2) return 1; sh_printf("%s\n", sh_basename(argv[1])); return 0; }
static int c_dirname(int argc, char **argv)
{
    if (argc < 2) return 1;
    char p[256];
    sh_path(argv[1], p, sizeof(p));
    char *s = strrchr(p, '/');
    if (s && s != p) *s = 0; else snprintf(p, sizeof(p), "/");
    sh_printf("%s\n", p);
    return 0;
}
static int c_realpath(int argc, char **argv) { char p[256]; sh_path(argc > 1 ? argv[1] : ".", p, sizeof(p)); sh_printf("%s\n", p); return 0; }

static int c_open(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: open <file, folder or app>\n"); return 1; }
    char p[256];
    sh_path(argv[1], p, sizeof(p));
    if (sh_isdir(p)) { desktop_app_launch(desktop_app_find("Files")); file_explorer_navigate_to(p); return 0; }
    if (vfs_exists(p)) { shell_open_path(p); return 0; }
    int idx = desktop_app_find(argv[1]);
    if (idx >= 0) { desktop_app_launch(idx); return 0; }
    sh_err("open: %s is not a file, folder or app\n", argv[1]);
    return 1;
}

/* Opens a file with the app registered for its extension in
 * HKEY_CLASSES_ROOT (e.g. HKCR\.png = Photos); programs run; anything
 * unknown opens in Notepad. */
void shell_open_path(const char *p)
{
    const char *dot = strrchr(p, '.'), *slash = strrchr(p, '/');
    char app[48] = "", key[48];
    if (dot && (!slash || dot > slash) && strlen(dot) < 20) {
        snprintf(key, sizeof(key), "HKCR\\%s", dot);
        for (char *c = key + 5; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
        reg_get_text(key, "", app, sizeof(app));
    }
    if (user_is_program(p)) { desktop_app_launch(desktop_app_find("Terminal")); user_exec(p); }
    else if (!strcasecmp_s(app, "Photos")) photos_open_file(p);
    else if (!strcasecmp_s(app, "Apps")) apps_store_show_install(p);
    else if (!strcasecmp_s(app, "Browser")) browser_open_file(p);
    else if (!strcasecmp_s(app, "Registry Editor")) {
        const char *d;
        char err[160];
        long n = sh_read_file(p, &d);
        int nk = 0, nv = 0;
        if (n >= 0 && reg_import(d, &nk, &nv, err, sizeof(err)) == 0) {
            snprintf(err, sizeof(err), "Added %d key(s) and %d value(s) from %s.", nk, nv, sh_basename(p));
            notification_show("Registry Editor", err, NOTIF_INFO, 5000);
            desktop_apply_settings();
        } else notification_show("Registry Editor", n < 0 ? "The file could not be read." : err, NOTIF_WARNING, 6000);
    }
    else if (app[0] && desktop_app_find(app) >= 0 && strcasecmp_s(app, "Notepad")) desktop_app_launch(desktop_app_find(app));
    else notepad_open_file(p);
}

static int c_edit(int argc, char **argv)
{
    if (argc < 2) { desktop_app_launch(desktop_app_find("Notepad")); return 0; }
    char p[256];
    sh_path(argv[1], p, sizeof(p));
    if (!vfs_exists(p) && vfs_create_file(p) != 0) { sh_err("edit: cannot create %s\n", argv[1]); return 1; }
    notepad_open_file(p);
    return 0;
}

/* ---- processes ---- */
static int c_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    static proc_info_t procs[64];
    int n = proc_list(procs, 64);
    static const char *const prio[4] = { "realtime", "high", "normal", "low" };
    sh_printf("%5s %5s  %-18s %-24s %-8s %8s %9s\n", "PID", "PPID", "NAME", "STATUS", "PRIORITY", "MEMORY", "CPU");
    for (int i = 0; i < n; i++) {
        char st[40], mem[16];
        sysmon_state_name(&procs[i], st, sizeof(st));
        if (procs[i].mem_kb) snprintf(mem, sizeof(mem), "%u KB", procs[i].mem_kb); else snprintf(mem, sizeof(mem), "-");
        sh_printf("%5u %5u  %-18s %-24s %-8s %8s %5u.%us\n", procs[i].pid, procs[i].ppid, procs[i].name, st,
                  prio[procs[i].priority & 3], mem, (unsigned)(procs[i].cpu_ticks / 100), (unsigned)((procs[i].cpu_ticks / 10) % 10));
    }
    return 0;
}

static int c_top(int argc, char **argv)
{
    uint64_t total, freep;
    pmm_get_stats(&total, &freep);
    char up[32];
    sys_format_uptime(up, sizeof(up));
    sh_printf("up %s, memory %u MB used of %u MB\n\n", up, (unsigned)((total - freep) >> 8), (unsigned)(total >> 8));
    return c_ps(argc, argv);
}

static int c_kill(int argc, char **argv)
{
    int rc = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' || argv[i][0] == '/') {
            if (!strcmp(argv[i], "/IM") || !strcmp(argv[i], "/im")) {
                if (i + 1 < argc) { char *a[2] = { (char *)"killall", argv[++i] }; rc |= c_killall(2, a); any = 1; }
            }
            continue;                                   /* -9, /F, /PID */
        }
        uint32_t pid = (uint32_t)sh_num(argv[i], 0);
        any = 1;
        if (!pid || proc_kill(pid) != 0) { sh_err("kill: %s: no such process, or it is protected\n", argv[i]); rc = 1; }
        else sh_printf("Ended process %u\n", pid);
    }
    if (!any) { sh_err("usage: kill <pid>...   (see: ps)\n"); return 1; }
    return rc;
}

int c_killall(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: killall <name>\n"); return 1; }
    static proc_info_t procs[64];
    int n = proc_list(procs, 64), ended = 0;
    for (int i = 0; i < n; i++)
        if (sh_wild(argv[1], procs[i].name) || !strcmp(procs[i].name, argv[1]))
            if (proc_kill(procs[i].pid) == 0) { sh_printf("Ended %s (PID %u)\n", procs[i].name, procs[i].pid); ended++; }
    if (!ended) { sh_err("killall: no process called %s that can be ended\n", argv[1]); return 1; }
    return 0;
}

static int c_suspres(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: %s <pid>\n", argv[0]); return 1; }
    uint32_t pid = (uint32_t)sh_num(argv[1], 0);
    int rc = argv[0][0] == 's' ? proc_suspend(pid) : proc_resume(pid);
    if (rc) { sh_err("%s: no such process, or it is protected\n", argv[0]); return 1; }
    sh_printf("%s %u\n", argv[0][0] == 's' ? "Suspended" : "Resumed", pid);
    return 0;
}

static int c_nice(int argc, char **argv)
{
    if (argc < 3) { sh_err("usage: nice <pid> low|normal|high\n"); return 1; }
    uint32_t pid = (uint32_t)sh_num(argv[1], 0);
    const char *q = argv[2];
    int prio = !strcmp(q, "high") ? PRIO_AI : !strcmp(q, "low") ? PRIO_USER : !strcmp(q, "normal") ? PRIO_SYSTEM : -1;
    static proc_info_t pl[64];
    int n = proc_list(pl, 64), ok = 0;
    for (int i = 0; i < n && prio >= 0; i++)
        if (pl[i].pid == pid && !(pl[i].flags & (PROC_FLAG_SYSTEM | PROC_FLAG_SERVICE)))
            ok = proc_set_priority(pid, prio) == 0;
    if (!ok) { sh_err(prio < 0 ? "usage: nice <pid> low|normal|high\n" : "nice: no such process, or it is protected\n"); return 1; }
    sh_printf("Priority of %u is now %s\n", pid, q);
    return 0;
}

static int sh_background;           /* the line ended in '&' */

static int c_run(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: run <program> [&]   (programs: ls /jarvis/bin)\n"); return 1; }
    char p[256];
    if (strchr(argv[1], '/') || argv[1][0] == '.') sh_path(argv[1], p, sizeof(p));
    else snprintf(p, sizeof(p), "/jarvis/bin/%s", argv[1]);
    if (!user_is_program(p)) {
        if (pkg_run(argv[1])) return 0;                 /* an installed app */
        sh_err("run: %s is not a program\n", argv[1]);
        return 1;
    }
    uint32_t pid;
    if (linux_is_program(p)) {
        /* a Linux program: arguments and the current folder go with it */
        char *lav[64];
        int lac = 0;
        lav[lac++] = (char *)sh_basename(p);
        for (int i = 2; i < argc && lac < 63; i++) lav[lac++] = argv[i];
        pid = linux_exec(p, lac, lav, sh.cwd);
    } else pid = user_exec(p);
    if (!pid) { sh_err("run: could not start %s\n", argv[1]); return 1; }
    if (sh_background || sh.root) sh_printf("[%u] %s started in the background\n", pid, sh_basename(p));
    else { sh_printf("Started %s as PID %u (Ctrl+C ends it)\n", sh_basename(p), pid); user_set_foreground(pid); }
    return 0;
}

/* apt / apt-get / apt-cache / dpkg: a worker process (kernel/dpkg.c) that
 * prints into the Terminal and owns the keyboard (for "continue? [Y/n]"). */
static int c_pkgcmd(int argc, char **argv);

static int c_apt(int argc, char **argv)
{
    /* "apt install app.jpk" still installs a JARVIS app */
    if (argc > 2 && !strcmp(argv[0], "apt") && !strcmp(argv[1], "install")) {
        const char *dot = strrchr(argv[argc - 1], '.');
        if (dot && !strcasecmp_s(dot, ".jpk")) return c_pkgcmd(argc, argv);
    }
    uint32_t pid = dpkg_start(argc, argv, sh.cwd);
    if (!pid) return 1;
    if (sh.root) {
        desktop_app_launch(desktop_app_find("Terminal"));
        sh_printf("Started '%s' (PID %u); its output appears in the Terminal window.%s\n", argv[0], pid,
                  !strcmp(argv[0], "dpkg") ? "" : " Add -y so it does not wait for a yes/no answer.");
    } else if (!sh_background) user_set_foreground(pid);
    return 0;
}

/* run <path> with the words after the command as its arguments */
static int sh_run_with(const char *p, int argc, char **argv)
{
    char *a[66];
    int n = 0;
    a[n++] = (char *)"run";
    a[n++] = (char *)p;
    for (int i = 1; i < argc && n < 65; i++) a[n++] = argv[i];
    a[n] = NULL;
    return c_run(n, a);
}

/* Looks a name up on the Linux PATH inside /jarvis/linux. */
static int sh_linux_find(const char *name, char *out, size_t n)
{
    static const char *const dirs[] = { "/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin", "/sbin", "/bin" };
    if (strchr(name, '/')) return 0;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(out, n, "/jarvis/linux%s/%s", dirs[i], name);
        if (user_is_program(out)) return 1;
    }
    return 0;
}

/* linux <program> [args]: runs a Linux program (found on the Linux PATH, or
 * by path), even when a JARVIS command has the same name. */
static int c_linux(int argc, char **argv)
{
    if (argc < 2) {
        sh_printf("Linux compatibility layer: runs Linux x86-64 programs (PIE and static-PIE, glibc).\n"
                  "Files: / for a Linux program is /jarvis/linux; /jarvis is shared.\n"
                  "usage: linux <program> [args]      e.g. linux hello, linux ./a.out\n");
        return 0;
    }
    char p[256];
    if (strchr(argv[1], '/') || argv[1][0] == '.') sh_path(argv[1], p, sizeof(p));
    else if (!sh_linux_find(argv[1], p, sizeof(p))) { sh_err("linux: %s: not found on the Linux PATH (/jarvis/linux/usr/bin, /bin ...)\n", argv[1]); return 127; }
    if (!linux_is_program(p)) { sh_err("linux: %s is not a Linux program\n", argv[1]); return 1; }
    return sh_run_with(p, argc - 1, argv + 1);
}

static int c_spawn(int argc, char **argv)
{
    uint32_t pid = sysmon_spawn_test(argc > 1 ? argv[1] : "");
    if (!pid) { sh_err("usage: spawn cpu|sleeper|deadlock|memory|isolate\n"); return 1; }
    sh_printf("Started PID %u\n", pid);
    return 0;
}

/* ---- system information ---- */
static int c_uname(int argc, char **argv)
{
    char host[48];
    prefs_get("device_name", host, sizeof(host));
    if (argc > 1 && (!strcmp(argv[1], "-a") || !strcmp(argv[1], "--all")))
        sh_printf("JARVIS %s 1.0 x86_64 JARVIS-OS (built %s)\n", host, __DATE__);
    else sh_printf("JARVIS\n");
    return 0;
}

static int c_hostname(int argc, char **argv)
{
    char msg[128];
    if (argc > 1) { if (prefs_set("device_name", argv[1], msg, sizeof(msg))) { sh_err("%s\n", msg); return 1; } return 0; }
    prefs_get("device_name", msg, sizeof(msg));
    sh_printf("%s\n", msg);
    return 0;
}

static int c_whoami(int argc, char **argv)
{
    (void)argc; (void)argv;
    char host[48];
    prefs_get("device_name", host, sizeof(host));
    if (!strcmp(argv[0], "id")) sh_printf("uid=1000(%s) groups=1000(%s),0(administrators)\n", account_name(), account_name());
    else sh_printf("%s\\%s\n", host, account_name());
    return 0;
}

static int c_date(int argc, char **argv)
{
    (void)argc; (void)argv;
    char d[48], c[16];
    sys_format_date(d, sizeof(d));
    sys_format_clock(c, sizeof(c));
    sh_printf("%s  %s\n", d, c);
    return 0;
}

static int c_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    char b[48];
    sys_format_uptime(b, sizeof(b));
    sh_printf("up %s\n", b);
    return 0;
}

static int c_free(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint64_t total, freep;
    pmm_get_stats(&total, &freep);
    sh_printf("%-8s %10s %10s %10s\n", "", "total", "used", "free");
    sh_printf("%-8s %7u MB %7u MB %7u MB\n", "Memory:", (unsigned)(total >> 8), (unsigned)((total - freep) >> 8), (unsigned)(freep >> 8));
    return 0;
}

static int c_lscpu(int argc, char **argv)
{
    (void)argc; (void)argv;
    char cpu[64];
    sys_cpu_name(cpu, sizeof(cpu));
    sh_printf("Architecture:  x86_64\nModel name:    %s\nCPU(s):        1 (single-core scheduling)\nTimer:         100 Hz\n", cpu);
    return 0;
}

static int c_lspci(int argc, char **argv)
{
    static dev_info_t d[48];
    int n = devmgr_list(d, 48), usb = !strcmp(argv[0], "lsusb");
    static const char *const st[4] = { "working", "no driver", "not working", "disabled" };
    for (int i = 0; i < n; i++) {
        int is_usb = !strcmp(d[i].type, "USB") && strncmp(d[i].name, "USB host", 8);
        if (usb != is_usb) continue;
        if (!usb && strncmp(d[i].location, "PCI", 3)) continue;
        sh_printf("%-12s %04x:%04x  %s  [%s, %s]\n", d[i].location, d[i].vendor_id, d[i].device_id, d[i].name,
                  d[i].driver[0] ? d[i].driver : "no driver", st[d[i].state & 3]);
    }
    (void)argc;
    return 0;
}

static int c_dmesg(int argc, char **argv)
{
    static char log[32768];
    size_t n = klog_read(log, sizeof(log));
    const char *start = log;
    if (argc > 1 && argv[1][0] != '-') {                    /* dmesg <filter> */
        size_t l;
        for (const char *p = log; (p = sh_next_line(p, log + n, &l)); p += l + 1) {
            if (sh_contains(p, l, argv[1], 1)) { sh_out_n(p, l); sh_out("\n"); }
            if (p + l >= log + n) break;
        }
        return 0;
    }
    if (n > 8000) {                                          /* last part */
        start = log + n - 8000;
        while (*start && *start != '\n') start++;
    }
    sh_out(start);
    return 0;
}

static int c_sysinfo(int argc, char **argv)
{
    (void)argc; (void)argv;
    char host[48], cpu[64], up[32], res[24];
    uint64_t total, freep;
    prefs_get("device_name", host, sizeof(host));
    sys_cpu_name(cpu, sizeof(cpu));
    sys_format_uptime(up, sizeof(up));
    pmm_get_stats(&total, &freep);
    prefs_get("resolution", res, sizeof(res));
    sh_printf("Host name:        %s\nOS:               JARVIS OS 1.0 (x86-64), built %s\nUser:             %s\nProcessor:        %s\n"
              "Memory:           %u MB, %u MB free\nDisplay:          %s\nUp time:          %s\nAI brain:         %s\n",
              host, __DATE__, account_name(), cpu, (unsigned)(total >> 8), (unsigned)(freep >> 8), res, up,
              g_kstate.ai_ready ? "ready" : "offline");
    net_info_t ni;
    net_get_info(&ni);
    char ip[16];
    net_format_ip(ni.ip, ip, sizeof(ip));
    sh_printf("Network:          %s%s\n", !ni.adapter ? "no adapter" : ni.configured ? "connected, " : "not connected", ni.configured ? ip : "");
    return 0;
}

static int c_env(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < sh.nenv; i++) sh_printf("%s=%s\n", sh.env[i].name, sh.env[i].value);
    sh_printf("PWD=%s\nUSER=%s\n", sh.cwd, account_name());
    return 0;
}

static int c_export(int argc, char **argv)
{
    if (argc < 2) return c_env(argc, argv);
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        *eq = 0;
        sh_setenv(argv[i], eq + 1);
    }
    return 0;
}

static int c_unset(int argc, char **argv) { for (int i = 1; i < argc; i++) sh_unsetenv(argv[i]); return 0; }

/* set NAME VALUE: a system setting (brightness, wallpaper ...);
 * set NAME=VALUE: a shell variable (Windows style); set: list both. */
static int c_set(int argc, char **argv)
{
    if (argc < 2) return c_env(argc, argv);
    if (strchr(argv[1], '=')) return c_export(argc, argv);
    char val[256] = "", msg[256];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "to") && i == 2 && argc > 3) continue;
        snprintf(val + strlen(val), sizeof(val) - strlen(val), "%s%s", val[0] ? " " : "", argv[i]);
    }
    if (argc < 3) {
        if (prefs_get(argv[1], msg, sizeof(msg)) == 0) { sh_printf("%s = %s\n", argv[1], msg); return 0; }
        sh_err("set: unknown setting %s (see: settings)\n", argv[1]);
        return 1;
    }
    char path[256];
    if (!strcmp(argv[1], "wallpaper") && val[0] != '/' && (strchr(val, '/') || strchr(val, '.'))) {   /* relative picture path */
        sh_path(val, path, sizeof(path));
        snprintf(val, sizeof(val), "%s", path);
    }
    if (prefs_set(argv[1], val, msg, sizeof(msg))) { sh_err("set: %s\n", msg); return 1; }
    sh_printf("%s\n", msg);
    return 0;
}

static int c_settings(int argc, char **argv)
{
    static char b[8192];
    if (argc > 1 && strcmp(argv[1], "-v")) { char *a[3] = { (char *)"set", argv[1], argc > 2 ? argv[2] : NULL }; return c_set(argc > 2 ? 3 : 2, a); }
    prefs_describe(b, sizeof(b), 1);
    sh_out(b);
    sh_out("Change one with: set <name> <value>\n");
    return 0;
}

/* Shortcut commands for common settings: brightness 60, volume up, ... */
static int c_pref_shortcut(int argc, char **argv)
{
    const char *name = argv[0];
    if (!strcmp(name, "nightlight")) name = "night_light";
    if (!strcmp(name, "resolution")) {
        if (argc < 2 || !strcmp(argv[1], "list")) {
            char cur[24];
            prefs_get("resolution", cur, sizeof(cur));
            sh_printf("Current: %s\nAvailable:", cur);
            for (int i = 0; i < fb_mode_count(); i++) { int w, h; fb_mode_get(i, &w, &h); sh_printf(" %dx%d", w, h); }
            sh_out("\n");
            return 0;
        }
    }
    char *a[3] = { (char *)"set", (char *)name, argc > 1 ? argv[1] : NULL };
    return c_set(argc > 1 ? 3 : 2, a);
}

static int c_alias(int argc, char **argv)
{
    if (argc < 2) { for (int i = 0; i < sh.nalias; i++) sh_printf("alias %s='%s'\n", sh.alias[i].name, sh.alias[i].value); return 0; }
    char line[200] = "";
    for (int i = 1; i < argc; i++) snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s", i > 1 ? " " : "", argv[i]);
    char *eq = strchr(line, '=');
    if (!eq) { for (int i = 0; i < sh.nalias; i++) if (!strcmp(sh.alias[i].name, line)) sh_printf("alias %s='%s'\n", line, sh.alias[i].value); return 0; }
    *eq = 0;
    for (int i = 0; i < sh.nalias; i++) if (!strcmp(sh.alias[i].name, line)) { snprintf(sh.alias[i].value, sizeof(sh.alias[0].value), "%s", eq + 1); return 0; }
    if (sh.nalias >= 24) { sh_err("alias: too many aliases\n"); return 1; }
    snprintf(sh.alias[sh.nalias].name, sizeof(sh.alias[0].name), "%s", line);
    snprintf(sh.alias[sh.nalias].value, sizeof(sh.alias[0].value), "%s", eq + 1);
    sh.nalias++;
    return 0;
}

static int c_unalias(int argc, char **argv)
{
    for (int k = 1; k < argc; k++)
        for (int i = 0; i < sh.nalias; i++) if (!strcmp(sh.alias[i].name, argv[k])) { sh.alias[i] = sh.alias[--sh.nalias]; break; }
    return 0;
}

const char *terminal_history_get(int i);                  /* apps/terminal.c */
static int c_history(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0;; i++) {
        const char *h = terminal_history_get(i);
        if (!h) break;
        sh_printf("%5d  %s\n", i + 1, h);
    }
    return 0;
}

static int c_clear(int argc, char **argv) { (void)argc; (void)argv; if (!sh.root) terminal_app_clear(); return 0; }

static int c_exit(int argc, char **argv)
{
    (void)argc; (void)argv;
    desktop_app_t *t = desktop_app_get(desktop_app_find("Terminal"));
    if (t && t->window && !sh.root) wm_request_close(t->window);
    return 0;
}

/* calc / expr: + - * / % ( ) with integers and decimals (2 places). */
static const char *ex_p;
static double ex_sum(void);
static void ex_ws(void) { while (*ex_p == ' ') ex_p++; }
static double ex_atom(void)
{
    ex_ws();
    if (*ex_p == '(') { ex_p++; double v = ex_sum(); ex_ws(); if (*ex_p == ')') ex_p++; return v; }
    if (*ex_p == '-') { ex_p++; return -ex_atom(); }
    double v = 0, scale = 0;
    while ((*ex_p >= '0' && *ex_p <= '9') || *ex_p == '.') {
        if (*ex_p == '.') scale = 1;
        else if (scale) { scale /= 10; v += (*ex_p - '0') * scale; }
        else v = v * 10 + (*ex_p - '0');
        ex_p++;
    }
    return v;
}
static double ex_prod(void)
{
    double v = ex_atom();
    for (;;) {
        ex_ws();
        char op = *ex_p;
        if (op != '*' && op != '/' && op != '%' && op != 'x') return v;
        ex_p++;
        double r = ex_atom();
        if (op == '*' || op == 'x') v *= r;
        else if (op == '/') v = r != 0 ? v / r : 0;
        else v = r != 0 ? (double)((long)v % (long)r) : 0;
    }
}
static double ex_sum(void)
{
    double v = ex_prod();
    for (;;) {
        ex_ws();
        if (*ex_p == '+') { ex_p++; v += ex_prod(); }
        else if (*ex_p == '-') { ex_p++; v -= ex_prod(); }
        else return v;
    }
}

static int c_calc(int argc, char **argv)
{
    char e[256] = "";
    for (int i = 1; i < argc; i++) snprintf(e + strlen(e), sizeof(e) - strlen(e), "%s", argv[i]);
    if (!e[0]) { sh_err("usage: calc <expression>   e.g. calc (2+3)*4.5\n"); return 1; }
    ex_p = e;
    double v = ex_sum();
    long whole = (long)v;
    if ((double)whole == v) sh_printf("%ld\n", whole);
    else sh_printf("%.4f\n", v);
    return 0;
}

static int c_true(int argc, char **argv) { (void)argc; return argv[0][0] == 'f'; }

/* ---- network ---- */
static int c_ifconfig(int argc, char **argv)
{
    (void)argc; (void)argv;
    net_info_t ni;
    char a[16], b[16];
    net_get_info(&ni);
    if (!ni.adapter) { sh_printf("No network adapter.\n"); return 1; }
    sh_printf("eth0: %s%s\n", !net_adapter_enabled() ? "disabled in Device Manager" : net_is_offline() ? "off (airplane mode)" : ni.link ? "up" : "cable unplugged",
              ni.configured ? ", connected" : "");
    sh_printf("    ether %02x:%02x:%02x:%02x:%02x:%02x\n", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    if (ni.configured) {
        net_format_ip(ni.ip, a, sizeof(a)); net_format_ip(ni.mask, b, sizeof(b));
        sh_printf("    inet %s  netmask %s\n", a, b);
        net_format_ip(ni.gateway, a, sizeof(a)); net_format_ip(ni.dns, b, sizeof(b));
        sh_printf("    gateway %s  dns %s\n", a, b);
    } else sh_printf("    no address yet (waiting for DHCP)\n");
    sh_printf("    RX %u frames  TX %u frames\n", (unsigned)ni.rx_frames, (unsigned)ni.tx_frames);
    return 0;
}

static int c_netcmd(int argc, char **argv)
{
    const char *v = argv[0];
    char line[256];
    if (!strcmp(v, "curl") || !strcmp(v, "wget")) v = "http";
    if (!strcmp(v, "dig") || !strcmp(v, "host")) v = "nslookup";
    if (!strcmp(v, "nmap") || !strcmp(v, "arp-scan") || !strcmp(v, "netdiscover")) v = "scan";
    if (strcmp(v, "scan") && argc < 2) { sh_err("usage: %s <host>\n", argv[0]); return 1; }
    snprintf(line, sizeof(line), "%s", v);
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && strcmp(v, "http")) { if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "-n")) i++; continue; }
        if (!strcmp(v, "http") && argv[i][0] == '-') { if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "-O")) continue; continue; }
        snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s", argv[i]);
        if (!strcmp(v, "scan")) break;
    }
    if (net_is_offline()) { sh_err("%s: the network is off (airplane mode or adapter disabled)\n", argv[0]); return 1; }
    if (net_command_start(line) != 0) { sh_err("Another network command is still running.\n"); return 1; }
    if (sh.root) sh_printf("Started '%s'; its output appears in the Terminal window.\n", line);
    return 0;
}

/* curl / wget: HTTP and HTTPS through kernel/http.c. They run in a network
 * process; the Terminal prints the result when it arrives. */
static struct { http_req_t req; int active, save, quiet, head_only; char path[256]; } g_sh_dl[4];

void shell_poll_downloads(void)
{
    for (int i = 0; i < 4; i++) {
        if (!g_sh_dl[i].active || (g_sh_dl[i].req.state != HTTP_DONE && g_sh_dl[i].req.state != HTTP_FAILED)) continue;
        http_req_t *r = &g_sh_dl[i].req;
        char line[400];
        if (r->state == HTTP_FAILED) { snprintf(line, sizeof(line), "curl: %s: %s\n", r->url, r->err); terminal_app_write_err(line); }
        else if (g_sh_dl[i].head_only) {
            snprintf(line, sizeof(line), "HTTP %d  %s\ncontent-type: %s\ncontent-length: %u\n%s%s\n", r->status, r->url, r->ctype,
                     (unsigned)r->len, r->tls[0] ? "security: " : "", r->tls);
            terminal_app_write(line);
        } else if (g_sh_dl[i].save) {
            snprintf(line, sizeof(line), "Saved %u bytes to %s (HTTP %d%s%s)\n", (unsigned)r->len, g_sh_dl[i].path, r->status, r->tls[0] ? ", " : "", r->tls);
            terminal_app_write(line);
            desktop_files_changed();
        } else {
            size_t show = r->len > 16384 ? 16384 : r->len;
            static char text[16384 + 64];
            size_t o = 0;
            for (size_t k = 0; k < show; k++) { char c = (char)r->data[k]; text[o++] = (c == '\n' || c == '\t' || ((unsigned char)c >= 32 && (unsigned char)c < 127)) ? c : '.'; }
            text[o] = 0;
            terminal_app_write(text);
            if (r->len > show) { snprintf(line, sizeof(line), "\n[%u more bytes; use -o FILE to save everything]\n", (unsigned)(r->len - show)); terminal_app_write(line); }
            else terminal_app_write("\n");
        }
        http_free(r);
        g_sh_dl[i].active = 0;
        desktop_invalidate();
    }
}

static int c_curl(int argc, char **argv)
{
    int wget = !strcmp(argv[0], "wget");
    const char *url = NULL, *out = NULL;
    int remote_name = wget, head = 0, quiet = 0;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "-O") || !strcmp(argv[i], "--output")) && i + 1 < argc && (!wget || !strcmp(argv[i], "-O"))) {
            if (!wget && !strcmp(argv[i], "-O")) { remote_name = 1; continue; }
            out = argv[++i];
        } else if (!strcmp(argv[i], "-O") && !wget) remote_name = 1;
        else if (!strcmp(argv[i], "-I") || !strcmp(argv[i], "--head")) head = 1;
        else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "-q") || !strcmp(argv[i], "-L") || !strcmp(argv[i], "-k")) quiet = 1;
        else if (argv[i][0] != '-') url = argv[i];
    }
    if (!url) { sh_err("usage: %s <url> %s\n", argv[0], wget ? "[-O file]" : "[-o file | -O] [-I]"); return 1; }
    char full[1024];
    if (!strstr(url, "://")) snprintf(full, sizeof(full), "https://%s", url); else snprintf(full, sizeof(full), "%s", url);
    if (net_is_offline()) { sh_err("%s: the network is off (airplane mode or adapter disabled)\n", argv[0]); return 1; }
    int slot = -1;
    for (int i = 0; i < 4; i++) if (!g_sh_dl[i].active) { slot = i; break; }
    if (slot < 0) { sh_err("%s: four transfers are already running\n", argv[0]); return 1; }
    memset(&g_sh_dl[slot], 0, sizeof(g_sh_dl[slot]));
    http_req_t *r = &g_sh_dl[slot].req;
    snprintf(r->url, sizeof(r->url), "%s", full);
    if (out || remote_name) {
        char p[256];
        if (out) sh_path(out, p, sizeof(p));
        else {
            char name[96];
            http_suggest_name(r, name, sizeof(name));
            snprintf(p, sizeof(p), "%s/%s", sh.cwd, name);
        }
        snprintf(g_sh_dl[slot].path, sizeof(g_sh_dl[slot].path), "%s", p);
        snprintf(r->save_path, sizeof(r->save_path), "%s", p);
        g_sh_dl[slot].save = 1;
    }
    g_sh_dl[slot].head_only = head;
    g_sh_dl[slot].quiet = quiet;
    g_sh_dl[slot].active = 1;
    if (http_start(r) != 0) { sh_err("%s: %s\n", argv[0], r->err); g_sh_dl[slot].active = 0; return 1; }
    sh_printf("%s %s ...%s\n", g_sh_dl[slot].save ? "Downloading" : "Fetching", full, sh.root ? " (the result appears in the Terminal)" : "");
    return 0;
}

static int c_route(int argc, char **argv)
{
    (void)argc; (void)argv;
    net_info_t ni;
    char a[16], b[16], c[16];
    net_get_info(&ni);
    if (!ni.configured) { sh_printf("No routes: not connected.\n"); return 1; }
    net_format_ip(ni.ip & ni.mask, a, sizeof(a)); net_format_ip(ni.mask, b, sizeof(b)); net_format_ip(ni.gateway, c, sizeof(c));
    sh_printf("Destination      Gateway          Netmask          Iface\n");
    sh_printf("%-16s %-16s %-16s eth0\n", "0.0.0.0", c, "0.0.0.0");
    sh_printf("%-16s %-16s %-16s eth0\n", a, "0.0.0.0", b);
    return 0;
}

/* ---- devices ---- */
static int c_devices(int argc, char **argv)
{
    static dev_info_t d[48];
    static const char *const st[4] = { "working", "NO DRIVER", "NOT WORKING", "disabled" };
    int n = devmgr_list(d, 48);
    (void)argc; (void)argv;
    for (int i = 0; i < n; i++)
        sh_printf("#%-3d %-10s %-34s %-12s %s%s%s\n", d[i].id, d[i].type, d[i].name, d[i].driver[0] ? d[i].driver : "-",
                  st[d[i].state & 3], d[i].note[0] ? ", " : "", d[i].note);
    return 0;
}

int shell_device_command(int argc, char **argv);            /* apps/devices.c */
static int c_device(int argc, char **argv) { return shell_device_command(argc, argv); }

/* ---- disks ---- */
static int c_lsblk(int argc, char **argv)
{
    (void)argc; (void)argv;
    sh_printf("%-6s %-24s %10s %10s %10s  %s\n", "DISK", "MODEL", "SIZE", "READ", "WRITTEN", "ERRORS");
    for (int i = 0; i < block_count(); i++) {
        block_info_t b;
        if (block_get_info(i, &b) != 0) continue;
        char s[16], r[16], w[16];
        sh_size_str((long)(b.sectors * 512), s, sizeof(s));
        sh_size_str((long)(b.sectors_read * 512), r, sizeof(r));
        sh_size_str((long)(b.sectors_written * 512), w, sizeof(w));
        sh_printf("%-6s %-24s %10s %10s %10s  %u\n", b.name, b.model, s, r, w, b.errors);
    }
    return c_df(argc, argv);
}

static int c_diskcmd(int argc, char **argv) { shell_disk_command(argc, argv); return 0; }

static int c_chkdsk(int argc, char **argv)
{
    diskfs_check_t ck;
    int repair = 0, surface = 0, status = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "/r") || !strcmp(argv[i], "/f") || !strcmp(argv[i], "-r") || !strcmp(argv[i], "-y")) repair = 1;
        else if (!strcmp(argv[i], "/s") || !strcmp(argv[i], "-s") || !strcmp(argv[i], "-c")) surface = 1;
        else if (!strcmp(argv[i], "status")) status = 1;
    }
    if (!status) {
        uint32_t pid = diskfs_check_start(surface, repair);
        if (!pid) { sh_err("chkdsk: a check is already running, or there is no JARVIS disk\n"); return 1; }
        sh_printf("Disk check started as PID %u. Type 'chkdsk status' for the result.\n", pid);
        return 0;
    }
    diskfs_check_status(&ck);
    if (ck.running) sh_printf("%s: %d%%\n", ck.phase, ck.progress);
    else if (!ck.done) sh_printf("No check has run since start.\n");
    else {
        sh_printf("%s\n%d files, %d folders; %u blocks read back, %u read errors, %u stale space-map bits.\n",
                  ck.summary, ck.files, ck.dirs, (unsigned)ck.blocks_read, (unsigned)ck.read_errors, (unsigned)ck.bitmap_diff);
        for (int i = 0; i < ck.issue_count; i++) sh_printf("  %s\n", ck.issue[i]);
    }
    return 0;
}

static int c_sync(int argc, char **argv) { (void)argc; (void)argv; sh_printf("%d item(s) written to disk.\n", diskfs_sync()); return 0; }

static int c_sector(int argc, char **argv)
{
    static uint8_t sec[512];
    if (argc < 3) { sh_err("usage: sector <disk number> <sector>\n"); return 1; }
    int dev = (int)sh_num(argv[1], -1);
    long lba = sh_num(argv[2], -1);
    if (dev < 0 || lba < 0 || block_read(dev, (uint64_t)lba, 1, sec) != 0) { sh_err("sector: read failed\n"); return 1; }
    for (int row = 0; row < 32; row++) {
        char line[96];
        int o = snprintf(line, sizeof(line), "%03x  ", row * 16);
        for (int i = 0; i < 16; i++) o += snprintf(line + o, sizeof(line) - (size_t)o, "%02x ", sec[row * 16 + i]);
        for (int i = 0; i < 16; i++) { uint8_t c = sec[row * 16 + i]; line[o++] = c >= 32 && c < 127 ? (char)c : '.'; }
        line[o++] = '\n'; line[o] = 0;
        sh_out(line);
    }
    return 0;
}

static int c_eject(int argc, char **argv)
{
    int done = 0;
    for (int dev = 0; dev < block_count() && !done; dev++) {
        block_info_t b;
        if (block_get_info(dev, &b) != 0 || strncmp(b.name, "usb", 3) || (argc > 1 && strcmp(b.name, argv[1]))) continue;
        int port = usb_port_of_block(dev);
        done = port >= 0 && usb_eject_port(port) == 0 ? 1 : -1;
        if (done > 0) sh_printf("%s ejected: safe to unplug.\n", b.name); else sh_err("eject: could not eject %s\n", b.name);
    }
    if (!done) { sh_err("eject: no such USB drive (see: lsblk)\n"); return 1; }
    return done > 0 ? 0 : 1;
}

static int c_mount(int argc, char **argv)
{
    (void)argc; (void)argv;
    static drive_info_t dr[8];
    int n = storage_list(dr, 8);
    for (int i = 0; i < n; i++)
        sh_printf("%s on %s type %s (%s)\n", dr[i].device[0] ? dr[i].device : "ramfs", dr[i].mount[0] ? dr[i].mount : "(not mounted)",
                  dr[i].fs, dr[i].read_only ? "ro" : "rw");
    return 0;
}

/* ---- packages ---- */
static int c_packages(int argc, char **argv)
{
    (void)argc; (void)argv;
    static pkg_info_t list[32];
    int n = pkg_list(list, 32);
    if (!n) sh_printf("No apps installed. Install one with: install <file.jpk>\n");
    for (int i = 0; i < n; i++)
        sh_printf("%-16s %-24s %-8s %s\n", list[i].name, list[i].title, list[i].version, list[i].publisher[0] ? list[i].publisher : "unknown publisher");
    return 0;
}

static int c_install(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: install <file.jpk>\n"); return 1; }
    char p[256], err[96];
    pkg_info_t info;
    sh_path(argv[argc - 1], p, sizeof(p));
    if (pkg_install(p, &info, err, sizeof(err)) != 0) { sh_err("install: %s\n", err); return 1; }
    sh_printf("Installed %s %s (%u file(s)). Start it with: start %s\n", info.title, info.version, info.files, info.name);
    return 0;
}

static int c_uninstall(int argc, char **argv)
{
    if (argc < 2 || pkg_uninstall(argv[argc - 1]) != 0) { sh_err("uninstall: no such app (see: packages)\n"); return 1; }
    sh_printf("Uninstalled %s.\n", argv[argc - 1]);
    return 0;
}

static int c_pkgcmd(int argc, char **argv)       /* pkg list|install|remove (JARVIS apps) */
{
    if (argc < 2 || !strcmp(argv[1], "list")) return c_packages(argc, argv);
    if (!strcmp(argv[1], "install") && argc > 2) return c_install(argc - 1, argv + 1);
    if ((!strcmp(argv[1], "remove") || !strcmp(argv[1], "uninstall") || !strcmp(argv[1], "purge")) && argc > 2) return c_uninstall(argc - 1, argv + 1);
    sh_err("usage: %s list | install <file.jpk> | remove <name>\n", argv[0]);
    return 1;
}

static int c_start(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: start <app, file or folder>\n"); return 1; }
    if (pkg_run(argv[1])) return 0;
    return c_open(argc, argv);
}

/* ---- desktop ---- */
static int c_apps(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < desktop_app_count(); i++) {
        desktop_app_t *app = desktop_app_get(i);
        sh_printf("  %s%s\n", app->name, app->window ? "  (open)" : "");
    }
    return 0;
}

static int c_close(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: close <app>\n"); return 1; }
    int idx = desktop_app_find(argv[1]);
    desktop_app_t *a = idx >= 0 ? desktop_app_get(idx) : NULL;
    if (!a || !a->window) { sh_err("close: %s is not open\n", argv[1]); return 1; }
    wm_request_close(a->window);
    return 0;
}

static int c_theme(int argc, char **argv)
{
    if (argc < 2) { char t[24]; prefs_get("theme", t, sizeof(t)); sh_printf("%s\n", t); return 0; }
    char *a[3] = { (char *)"set", (char *)"theme", argv[1] };
    return c_set(3, a);
}

static int c_wallpaper(int argc, char **argv)
{
    if (argc < 2) {
        char t[256];
        prefs_get("wallpaper", t, sizeof(t));
        sh_printf("Current: %s\nBuilt in: bloom aurora mountains dunes night waves solid\nOr a picture: wallpaper <file.png|jpg|bmp> [fill|fit|stretch|center|tile]\n", t);
        return 0;
    }
    char *a[3] = { (char *)"set", (char *)"wallpaper", argv[1] };
    int rc = c_set(3, a);
    if (!rc && argc > 2) { char *b[3] = { (char *)"set", (char *)"wallpaper_fit", argv[2] }; rc = c_set(3, b); }
    return rc;
}

static int c_screenshot(int argc, char **argv)
{
    (void)argc; (void)argv;
    char saved[160];
    desktop_take_screenshot(saved, sizeof(saved));
    if (!saved[0]) { sh_err("screenshot: could not save the picture\n"); return 1; }
    sh_printf("Saved %s\n", saved);
    return 0;
}

static int c_notify(int argc, char **argv)
{
    char t[256] = "";
    for (int i = 1; i < argc; i++) snprintf(t + strlen(t), sizeof(t) - strlen(t), "%s%s", i > 1 ? " " : "", argv[i]);
    if (!t[0]) { sh_err("usage: notify <text>\n"); return 1; }
    notification_show("Terminal", t, NOTIF_INFO, 5000);
    return 0;
}

static int c_power(int argc, char **argv)
{
    (void)argc;
    if (!strcmp(argv[0], "lock")) desktop_lock();
    else if (!strcmp(argv[0], "reboot") || !strcmp(argv[0], "restart")) desktop_system_restart();
    else desktop_system_shutdown();
    return 0;
}

static int c_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    sh_printf("  AI brain   %s\n  Voice      %s\n  Security   %s (lock %s)\n", g_kstate.ai_ready ? "ready" : "offline",
              g_kstate.speech_ready ? "ready" : "loading", ui_shell_get_security_label(), g_kstate.security_enabled ? "on" : "off");
    char res[24];
    prefs_get("resolution", res, sizeof(res));
    sh_printf("  Display    %s, %d window(s)\n", res, wm_count_windows());
    return 0;
}

/* ---- JARVIS ---- */
static int c_ask(int argc, char **argv)
{
    char t[512] = "";
    for (int i = 1; i < argc; i++) snprintf(t + strlen(t), sizeof(t) - strlen(t), "%s%s", i > 1 ? " " : "", argv[i]);
    if (!t[0]) { sh_err("usage: ask <question>\n"); return 1; }
    desktop_ask_jarvis(t);
    sh_printf("Sent to JARVIS (the reply appears in the JARVIS window).\n");
    return 0;
}

/* jarvis <tool> [key=value ...] | jarvis <tool> '{json}': runs one of JARVIS's
 * tools directly, without the language model. */
static int c_jarvis(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "tools")) {
        sh_printf("JARVIS tools (run one with: jarvis <tool> key=value ...):\n%s\n", ai_tool_names());
        return 0;
    }
    static char json[1024], out[4096];
    if (argc > 2 && argv[2][0] == '{') snprintf(json, sizeof(json), "%s", argv[2]);
    else {
        snprintf(json, sizeof(json), "{");
        for (int i = 2; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) continue;
            *eq = 0;
            const char *v = eq + 1;
            int num = *v && (sh_num(v, -1) != -1 || !strcmp(v, "0"));
            int boolv = !strcmp(v, "true") || !strcmp(v, "false");
            snprintf(json + strlen(json), sizeof(json) - strlen(json), "%s\"%s\":%s%s%s", strlen(json) > 1 ? "," : "", argv[i],
                     num || boolv ? "" : "\"", v, num || boolv ? "" : "\"");
        }
        snprintf(json + strlen(json), sizeof(json) - strlen(json), "}");
    }
    if (ai_tool_exec(argv[1], json, out, sizeof(out)) < 0) { sh_err("jarvis: no tool called %s (see: jarvis tools)\n", argv[1]); return 1; }
    sh_out(out);
    if (out[0] && out[strlen(out) - 1] != '\n') sh_out("\n");
    return 0;
}

static int c_help(int argc, char **argv);
static int c_man(int argc, char **argv);
int c_timer(int argc, char **argv);                 /* apps/clock.c */
int c_reg(int argc, char **argv);                   /* apps/regedit.c */
int c_regedit(int argc, char **argv);
int c_setx(int argc, char **argv);
int c_alarm(int argc, char **argv);
int c_stopwatch(int argc, char **argv);

/* ---- the table ---- */
static const sh_cmd_t g_cmds[] = {
    { "help",      c_help,      "help [command]", "List commands, or explain one", "Shell" },
    { "man",       c_man,       "man <command>", "Explain a command", "Shell" },
    { "pwd",       c_pwd,       "pwd", "Show the current folder", "Files" },
    { "cd",        c_cd,        "cd [folder | .. | - | ~ | C:\\ | D:]", "Change folder", "Files" },
    { "chdir",     c_cd,        "chdir <folder>", "Change folder", "Files" },
    { "ls",        c_ls,        "ls [-l] [-a] [-h] [-R] [path...]", "List a folder", "Files" },
    { "dir",       c_ls,        "dir [/s] [path]", "List a folder with sizes", "Files" },
    { "tree",      c_tree,      "tree [folder]", "Show a folder tree", "Files" },
    { "cat",       c_cat,       "cat [-n] <file>...", "Print files", "Files" },
    { "type",      c_cat,       "type <file>", "Print a file", "Files" },
    { "more",      c_cat,       "more <file>", "Print a file", "Files" },
    { "less",      c_cat,       "less <file>", "Print a file", "Files" },
    { "head",      c_headtail,  "head [-n N] [file]", "First lines", "Text" },
    { "tail",      c_headtail,  "tail [-n N] [file]", "Last lines", "Text" },
    { "wc",        c_wc,        "wc [-l] [-w] [-c] [file]", "Count lines, words, bytes", "Text" },
    { "grep",      c_grep,      "grep [-i] [-v] [-n] [-c] [-r] <text> [file...]", "Find lines containing text", "Text" },
    { "findstr",   c_grep,      "findstr [/i] [/n] [/s] <text> [file]", "Find lines containing text", "Text" },
    { "find",      c_find,      "find [folder] [-name pattern] [-type f|d]", "Find files by name", "Files" },
    { "sort",      c_sort,      "sort [-r] [-n] [-u] [file]", "Sort lines", "Text" },
    { "uniq",      c_uniq,      "uniq [-c] [file]", "Drop repeated lines", "Text" },
    { "tee",       c_tee,       "tee [-a] <file>", "Copy piped text to a file", "Text" },
    { "echo",      c_echo,      "echo [-n] <text>", "Print text (use > file to write it)", "Text" },
    { "hexdump",   c_hexdump,   "hexdump <file>", "Show bytes in hex", "Text" },
    { "xxd",       c_hexdump,   "xxd <file>", "Show bytes in hex", "Text" },
    { "sha256sum", c_hash,      "sha256sum <file>...", "SHA-256 checksum", "Text" },
    { "crc32",     c_hash,      "crc32 <file>", "CRC-32 checksum", "Text" },
    { "cksum",     c_hash,      "cksum <file>", "CRC-32 checksum", "Text" },
    { "touch",     c_touch,     "touch <file>...", "Create empty files", "Files" },
    { "mkdir",     c_mkdir,     "mkdir [-p] <folder>...", "Create folders", "Files" },
    { "rmdir",     c_rmdir,     "rmdir <folder>", "Remove an empty folder", "Files" },
    { "rm",        c_rm,        "rm [-r] [-f] [-v] <path>...", "Delete files or folders", "Files" },
    { "del",       c_rm,        "del [/s] [/q] <path>...", "Delete files", "Files" },
    { "cp",        c_cpmv,      "cp [-r] [-v] <source>... <destination>", "Copy", "Files" },
    { "copy",      c_cpmv,      "copy <source> <destination>", "Copy", "Files" },
    { "xcopy",     c_cpmv,      "xcopy <source> <destination>", "Copy folders", "Files" },
    { "mv",        c_cpmv,      "mv [-v] <source>... <destination>", "Move or rename", "Files" },
    { "move",      c_cpmv,      "move <source> <destination>", "Move", "Files" },
    { "ren",       c_cpmv,      "ren <old> <new>", "Rename", "Files" },
    { "rename",    c_cpmv,      "rename <old> <new>", "Rename", "Files" },
    { "stat",      c_stat,      "stat <path>", "Details of a file or folder", "Files" },
    { "file",      c_file,      "file <path>...", "What kind of file it is", "Files" },
    { "du",        c_du,        "du [-s] [folder]", "Space used by folders", "Files" },
    { "basename",  c_basename,  "basename <path>", "Last part of a path", "Files" },
    { "dirname",   c_dirname,   "dirname <path>", "Folder part of a path", "Files" },
    { "realpath",  c_realpath,  "realpath <path>", "Full path", "Files" },
    { "open",      c_open,      "open <file|folder|app>", "Open with the default app", "Files" },
    { "xdg-open",  c_open,      "xdg-open <file>", "Open with the default app", "Files" },
    { "start",     c_start,     "start <app|file|folder>", "Start an app or open a file", "Files" },
    { "edit",      c_edit,      "edit <file>", "Edit in Notepad", "Files" },
    { "nano",      c_edit,      "nano <file>", "Edit in Notepad", "Files" },
    { "notepad",   c_edit,      "notepad <file>", "Edit in Notepad", "Files" },
    { "ps",        c_ps,        "ps", "List processes", "Processes" },
    { "top",       c_top,       "top", "Processes and memory", "Processes" },
    { "kill",      c_kill,      "kill <pid>...", "End processes", "Processes" },
    { "killall",   c_killall,   "killall <name>", "End processes by name", "Processes" },
    { "pkill",     c_killall,   "pkill <name>", "End processes by name", "Processes" },
    { "suspend",   c_suspres,   "suspend <pid>", "Pause a process", "Processes" },
    { "resume",    c_suspres,   "resume <pid>", "Resume a process", "Processes" },
    { "nice",      c_nice,      "nice <pid> low|normal|high", "Change priority", "Processes" },
    { "run",       c_run,       "run <program> [args] [&]", "Run a program (Ctrl+C ends it)", "Processes" },
    { "linux",     c_linux,     "linux <program> [args]", "Run a Linux program (from /jarvis/linux)", "Processes" },
    { "spawn",     c_spawn,     "spawn cpu|sleeper|deadlock|memory|isolate", "Start a test process", "Processes" },
    { "uname",     c_uname,     "uname [-a]", "System name", "System" },
    { "hostname",  c_hostname,  "hostname [new name]", "Show or set the PC name", "System" },
    { "whoami",    c_whoami,    "whoami", "Current user", "System" },
    { "id",        c_whoami,    "id", "User and groups", "System" },
    { "date",      c_date,      "date", "Date and time", "System" },
    { "time",      c_date,      "time", "Date and time", "System" },
    { "uptime",    c_uptime,    "uptime", "Time since start", "System" },
    { "free",      c_free,      "free", "Memory use", "System" },
    { "lscpu",     c_lscpu,     "lscpu", "Processor", "System" },
    { "lspci",     c_lspci,     "lspci", "PCI devices", "System" },
    { "lsusb",     c_lspci,     "lsusb", "USB devices", "System" },
    { "dmesg",     c_dmesg,     "dmesg [filter]", "Kernel messages", "System" },
    { "sysinfo",   c_sysinfo,   "sysinfo", "Summary of this PC", "System" },
    { "status",    c_status,    "status", "JARVIS status", "System" },
    { "env",       c_env,       "env", "Shell variables", "Shell" },
    { "setx",      c_setx,      "setx NAME value", "Save a variable for every new Terminal (HKCU\\Environment)", "Shell" },
    { "reg",       c_reg,       "reg query|add|delete|export|import <key> ...", "Read and change the registry (reg /?)", "Settings" },
    { "regedit",   c_regedit,   "regedit [key | file.reg]", "Open the Registry Editor, or import a .reg file", "Settings" },
    { "printenv",  c_env,       "printenv", "Shell variables", "Shell" },
    { "export",    c_export,    "export NAME=value", "Set a variable", "Shell" },
    { "unset",     c_unset,     "unset NAME", "Remove a variable", "Shell" },
    { "set",       c_set,       "set <setting> <value> | set NAME=value", "Change a system setting (see: settings)", "Settings" },
    { "settings",  c_settings,  "settings [name [value]]", "All system settings and their values", "Settings" },
    { "brightness", c_pref_shortcut, "brightness <10-100|up|down>", "Screen brightness", "Settings" },
    { "volume",    c_pref_shortcut, "volume <0-100|up|down>", "Sound volume", "Settings" },
    { "mute",      c_pref_shortcut, "mute on|off", "Mute sound", "Settings" },
    { "nightlight", c_pref_shortcut, "nightlight on|off", "Night light", "Settings" },
    { "resolution", c_pref_shortcut, "resolution [WxH | list]", "Screen resolution", "Settings" },
    { "theme",     c_theme,     "theme dark|light|high_contrast", "Colour mode", "Settings" },
    { "wallpaper", c_wallpaper, "wallpaper <name | picture> [fit]", "Desktop background", "Settings" },
    { "alias",     c_alias,     "alias [name=command]", "Shortcuts for commands", "Shell" },
    { "unalias",   c_unalias,   "unalias <name>", "Remove a shortcut", "Shell" },
    { "history",   c_history,   "history", "Commands typed so far", "Shell" },
    { "clear",     c_clear,     "clear", "Clear the screen (Ctrl+L)", "Shell" },
    { "exit",      c_exit,      "exit", "Close the Terminal", "Shell" },
    { "calc",      c_calc,      "calc <expression>", "Arithmetic, e.g. calc (2+3)*4", "Shell" },
    { "expr",      c_calc,      "expr <expression>", "Arithmetic", "Shell" },
    { "true",      c_true,      "true", "Succeed", "Shell" },
    { "false",     c_true,      "false", "Fail", "Shell" },
    { "ifconfig",  c_ifconfig,  "ifconfig", "Network adapter and address", "Network" },
    { "ip",        c_ifconfig,  "ip a", "Network adapter and address", "Network" },
    { "ping",      c_netcmd,    "ping <host>", "Check a host answers", "Network" },
    { "nslookup",  c_netcmd,    "nslookup <host>", "Look up a name (DNS)", "Network" },
    { "dig",       c_netcmd,    "dig <host>", "Look up a name (DNS)", "Network" },
    { "host",      c_netcmd,    "host <host>", "Look up a name (DNS)", "Network" },
    { "http",      c_netcmd,    "http <url> [file]", "Fetch a plain-http page", "Network" },
    { "curl",      c_curl,      "curl <url> [-o file | -O] [-I]", "Fetch a page or file over http/https", "Network" },
    { "apt",       c_apt,       "apt update | install <pkg> | remove <pkg> | search <word> | show <pkg> | list --installed | upgrade", "Debian packages for the Linux layer", "Linux" },
    { "apt-get",   c_apt,       "apt-get update | install <pkg> | remove <pkg>", "Debian packages (same as apt)", "Linux" },
    { "apt-cache", c_apt,       "apt-cache search <word> | show <pkg> | policy <pkg>", "Search the Debian package index", "Linux" },
    { "dpkg",      c_apt,       "dpkg -i <file.deb> | -r <pkg> | -l | -L <pkg> | -s <pkg> | -c <file.deb>", "Install or inspect Debian packages", "Linux" },
    { "dpkg-query",c_apt,       "dpkg-query -l | -L <pkg> | -s <pkg>", "Query installed Debian packages", "Linux" },
    { "dpkg-deb",  c_apt,       "dpkg-deb -c|-I <file.deb>", "Inspect a .deb file", "Linux" },
    { "wget",      c_curl,      "wget <url> [-O file]", "Download a file over http/https", "Network" },
    { "scan",      c_netcmd,    "scan", "Find devices on the local network", "Network" },
    { "nmap",      c_netcmd,    "nmap", "Find devices on the local network", "Network" },
    { "arp-scan",  c_netcmd,    "arp-scan", "Find devices on the local network", "Network" },
    { "route",     c_route,     "route", "Routing table", "Network" },
    { "devices",   c_devices,   "devices", "Hardware devices and drivers", "Devices" },
    { "lsdev",     c_devices,   "lsdev", "Hardware devices", "Devices" },
    { "device",    c_device,    "device enable|disable|info|restart|uninstall|scan <#id or name>", "Manage a device", "Devices" },
    { "lsblk",     c_lsblk,     "lsblk", "Disks and drives", "Disks" },
    { "disks",     c_lsblk,     "disks", "Disks and drives", "Disks" },
    { "df",        c_df,        "df", "Free space on each drive", "Disks" },
    { "mount",     c_mount,     "mount", "Mounted drives", "Disks" },
    { "disk",      c_diskcmd,   "disk list|info|init|create|delete|format|label|extend|shrink ...", "Disk management (see: disk help)", "Disks" },
    { "diskpart",  c_diskcmd,   "diskpart ...", "Disk management (same as disk)", "Disks" },
    { "format",    c_diskcmd,   "format <disk> [partition] fat32|jarvisfs [label]", "Format a drive", "Disks" },
    { "chkdsk",    c_chkdsk,    "chkdsk [/r] [/s] | status", "Check the JARVIS disk", "Disks" },
    { "fsck",      c_chkdsk,    "fsck [-y] | status", "Check the JARVIS disk", "Disks" },
    { "sync",      c_sync,      "sync", "Save everything to disk now", "Disks" },
    { "sector",    c_sector,    "sector <disk> <n>", "Show a raw disk sector", "Disks" },
    { "eject",     c_eject,     "eject [usbN]", "Safely remove a USB drive", "Disks" },
    { "umount",    c_eject,     "umount <usbN>", "Safely remove a USB drive", "Disks" },
    { "packages",  c_packages,  "packages", "Installed apps", "Apps" },
    { "install",   c_install,   "install <file.jpk>", "Install an app", "Apps" },
    { "uninstall", c_uninstall, "uninstall <name>", "Remove an app", "Apps" },
    { "pkg",       c_pkgcmd,    "pkg list|install|remove", "Manage JARVIS apps (.jpk)", "Apps" },
    { "apps",      c_apps,      "apps", "Apps on this PC", "Apps" },
    { "close",     c_close,     "close <app>", "Close an app window", "Apps" },
    { "screenshot", c_screenshot, "screenshot", "Save a picture of the screen", "Desktop" },
    { "timer",     c_timer,     "timer [5m | 90s | 1h30m | stop]", "Countdown timer with a chime", "Desktop" },
    { "alarm",     c_alarm,     "alarm <07:30 | 6:45pm> [label] | list | delete <n>", "Alarms (kept after restart)", "Desktop" },
    { "stopwatch", c_stopwatch, "stopwatch start|stop|lap|reset", "Stopwatch", "Desktop" },
    { "notify",    c_notify,    "notify <text>", "Show a notification", "Desktop" },
    { "lock",      c_power,     "lock", "Lock the PC", "Power" },
    { "reboot",    c_power,     "reboot", "Restart", "Power" },
    { "restart",   c_power,     "restart", "Restart", "Power" },
    { "shutdown",  c_power,     "shutdown", "Turn off", "Power" },
    { "poweroff",  c_power,     "poweroff", "Turn off", "Power" },
    { "ask",       c_ask,       "ask <question>", "Ask JARVIS", "JARVIS" },
    { "jarvis",    c_jarvis,    "jarvis <tool> [key=value...]", "Run a JARVIS tool directly (jarvis tools)", "JARVIS" },
};
#define SH_NCMDS ((int)(sizeof(g_cmds) / sizeof(g_cmds[0])))

static const sh_cmd_t *sh_find(const char *name)
{
    for (int i = 0; i < SH_NCMDS; i++)
        if (!strcmp(g_cmds[i].name, name)) return &g_cmds[i];
    /* Windows is case-insensitive: DIR, Copy ... */
    char low[32];
    size_t l = strlen(name);
    if (l >= sizeof(low)) return NULL;
    for (size_t i = 0; i <= l; i++) low[i] = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + 32) : name[i];
    if (strcmp(low, name)) return sh_find(low);
    return NULL;
}

/* Command names starting with prefix, for Tab completion. */
int shell_complete_command(const char *prefix, const char **out, int max)
{
    int n = 0;
    size_t l = strlen(prefix);
    for (int i = 0; i < SH_NCMDS && n < max; i++)
        if (!strncmp(g_cmds[i].name, prefix, l)) out[n++] = g_cmds[i].name;
    for (int i = 0; i < sh.nalias && n < max; i++)
        if (!strncmp(sh.alias[i].name, prefix, l)) out[n++] = sh.alias[i].name;
    return n;
}

/* Files in the folder of a partial path, for Tab completion. */
int shell_complete_path(const char *partial, char out[][96], int max, int *is_dir)
{
    char dir[256], tmp[256];
    const char *slash = strrchr(partial, '/');
    const char *bslash = strrchr(partial, '\\');
    if (bslash > slash) slash = bslash;
    const char *stem = slash ? slash + 1 : partial;
    if (slash) {
        snprintf(tmp, sizeof(tmp), "%.*s", (int)(slash - partial + 1), partial);
        sh_path(tmp, dir, sizeof(dir));
    } else snprintf(dir, sizeof(dir), "%s", sh.cwd);
    int n = sh_readdir(dir, g_ents, 512), k = 0;
    size_t sl = strlen(stem);
    for (int i = 0; i < n && k < max; i++) {
        int match = 1;
        for (size_t c = 0; c < sl && match; c++) {
            char a = g_ents[i].name[c], b = stem[c];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            match = a == b;
        }
        if (!match) continue;
        snprintf(out[k], 96, "%s", g_ents[i].name);
        is_dir[k] = g_ents[i].is_dir;
        k++;
    }
    return k;
}

static int c_help(int argc, char **argv)
{
    if (argc > 1) return c_man(argc, argv);
    static const char *const groups[] = { "Files", "Text", "Processes", "System", "Settings", "Network", "Devices", "Disks", "Apps", "Desktop", "Power", "JARVIS", "Shell" };
    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
        char line[1024];
        int o = snprintf(line, sizeof(line), "%-10s", groups[g]), col = 10;
        for (int i = 0; i < SH_NCMDS; i++) {
            if (strcmp(g_cmds[i].group, groups[g])) continue;
            int l = (int)strlen(g_cmds[i].name);
            if (col + l + 1 > 86) { o += snprintf(line + o, sizeof(line) - (size_t)o, "\n          "); col = 10; }
            o += snprintf(line + o, sizeof(line) - (size_t)o, "%s ", g_cmds[i].name);
            col += l + 1;
        }
        sh_printf("%s\n", line);
    }
    sh_out("\nMore: help <command>.  Pipes a | b, redirection > >> <, a && b, a ; b, variables $HOME.\n"
           "Paths: ~ (Documents), C:\\ (/jarvis), D:\\ (first USB drive). Tab completes, Up/Down recall.\n");
    return 0;
}

static int c_man(int argc, char **argv)
{
    if (argc < 2) { sh_err("usage: man <command>\n"); return 1; }
    const sh_cmd_t *c = sh_find(argv[1]);
    if (!c) {
        for (int i = 0; i < sh.nalias; i++)
            if (!strcmp(sh.alias[i].name, argv[1])) { sh_printf("%s is a shortcut for: %s\n", argv[1], sh.alias[i].value); return 0; }
        sh_err("man: no command called %s\n", argv[1]);
        return 1;
    }
    sh_printf("%s - %s\nUsage: %s\n", c->name, c->help, c->usage);
    return 0;
}

/* =============================================================================
 * PARSER / EXECUTOR
 * ============================================================================= */
/* Expands $VAR ${VAR} %VAR% and ~ outside single quotes; splits into words
 * honouring quotes; marks operators. */
typedef struct { char *v[SH_MAX_ARGS]; int quoted[SH_MAX_ARGS]; int n; } sh_words_t;

static char g_wordbuf[4096];

static int sh_split(const char *line, sh_words_t *w)
{
    size_t o = 0;
    w->n = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (w->n >= SH_MAX_ARGS - 1 || o >= sizeof(g_wordbuf) - 260) return -1;
        /* operators */
        static const char *const ops[] = { "&&", "||", ">>", "2>&1", "2>", "|", ">", "<", ";", "&" };
        int matched = 0;
        for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
            size_t l = strlen(ops[k]);
            if (!strncmp(p, ops[k], l)) {
                w->v[w->n] = g_wordbuf + o;
                memcpy(g_wordbuf + o, ops[k], l + 1);
                o += l + 1;
                w->quoted[w->n++] = -1;            /* operator */
                p += l;
                matched = 1;
                break;
            }
        }
        if (matched) continue;
        w->v[w->n] = g_wordbuf + o;
        w->quoted[w->n] = 0;
        int first = 1;
        while (*p && *p != ' ' && *p != '\t' && *p != '|' && *p != ';' && *p != '>' && *p != '<' && !(*p == '&' && (p[1] == '&' || p[1] == 0 || p[1] == ' '))) {
            if (*p == '\'' ) {
                w->quoted[w->n] = 1;
                p++;
                while (*p && *p != '\'' && o < sizeof(g_wordbuf) - 2) g_wordbuf[o++] = *p++;
                if (*p) p++;
            } else if (*p == '"') {
                w->quoted[w->n] = 1;
                p++;
                while (*p && *p != '"' && o < sizeof(g_wordbuf) - 2) {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) { g_wordbuf[o++] = p[1]; p += 2; continue; }
                    if (*p == '$') goto var;
                    g_wordbuf[o++] = *p++;
                    continue;
                var:;
                    {
                        const char *s = p + 1;
                        char name[32];
                        int nl = 0;
                        if (*s == '{') { s++; while (*s && *s != '}' && nl < 31) name[nl++] = *s++; if (*s) s++; }
                        else if (*s == '?') { name[nl++] = '?'; s++; }
                        else while (((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_') && nl < 31) name[nl++] = *s++;
                        name[nl] = 0;
                        const char *val = nl ? sh_getenv(name) : "$";
                        if (val) { size_t vl = strlen(val); if (o + vl < sizeof(g_wordbuf) - 2) { memcpy(g_wordbuf + o, val, vl); o += vl; } }
                        p = s;
                    }
                }
                if (*p) p++;
            } else if (*p == '$' || (*p == '%' && strchr(p + 1, '%'))) {
                const char *s = p + 1;
                char name[32];
                int nl = 0;
                if (*p == '%') { while (*s && *s != '%' && nl < 31) name[nl++] = *s++; if (*s == '%') s++; }
                else if (*s == '{') { s++; while (*s && *s != '}' && nl < 31) name[nl++] = *s++; if (*s) s++; }
                else if (*s == '?') { name[nl++] = '?'; s++; }
                else while (((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_') && nl < 31) name[nl++] = *s++;
                name[nl] = 0;
                const char *val = nl ? sh_getenv(name) : NULL;
                if (!nl) { g_wordbuf[o++] = *p; p++; }
                else {
                    if (!val && *p == '%') { g_wordbuf[o++] = '%'; p++; continue; }   /* not a variable: keep literal */
                    if (val) { size_t vl = strlen(val); if (o + vl < sizeof(g_wordbuf) - 2) { memcpy(g_wordbuf + o, val, vl); o += vl; } }
                    p = s;
                }
            } else if (*p == '~' && first && (p[1] == 0 || p[1] == '/' || p[1] == ' ')) {
                size_t hl = strlen(SH_HOME);
                memcpy(g_wordbuf + o, SH_HOME, hl);
                o += hl;
                p++;
            } else {
                g_wordbuf[o++] = *p++;
            }
            first = 0;
        }
        g_wordbuf[o++] = 0;
        w->n++;
    }
    w->v[w->n] = NULL;
    return w->n;
}

/* Expands wildcard words (*.txt) against their folder. */
static int sh_glob(char **in, int *quoted, int n, char **out, int max, char *store, size_t cap)
{
    int k = 0;
    size_t o = 0;
    for (int i = 0; i < n && k < max - 1; i++) {
        const char *w = in[i];
        if (quoted[i] || (!strchr(w, '*') && !strchr(w, '?'))) { out[k++] = in[i]; continue; }
        char dirpart[256] = "", path[256];
        const char *slash = strrchr(w, '/');
        const char *pat = slash ? slash + 1 : w;
        if (slash) snprintf(dirpart, sizeof(dirpart), "%.*s", (int)(slash - w), w);
        sh_path(slash ? (dirpart[0] ? dirpart : "/") : ".", path, sizeof(path));
        int n2 = sh_readdir(path, g_ents, 512), added = 0;
        for (int e = 0; e < n2 && k < max - 1; e++) {
            if (!sh_wild(pat, g_ents[e].name) || (g_ents[e].name[0] == '.' && pat[0] != '.')) continue;
            int len = snprintf(store + o, cap - o, "%s%s%s", dirpart, slash ? "/" : "", g_ents[e].name);
            if (o + (size_t)len + 1 >= cap) break;
            out[k++] = store + o;
            o += (size_t)len + 1;
            added++;
        }
        if (!added) out[k++] = in[i];                       /* no match: pass it through */
    }
    out[k] = NULL;
    return k;
}

static char g_pipe[2][SH_PIPE_MAX + 1];

/* Runs one simple command (words already split). */
static int sh_run_simple(char **argv, int *quoted, int argc)
{
    if (!argc) return 0;
    /* NAME=value on its own: set a variable */
    if (argc == 1 && !quoted[0] && strchr(argv[0], '=') && argv[0][0] != '=') {
        char *eq = strchr(argv[0], '=');
        *eq = 0;
        sh_setenv(argv[0], eq + 1);
        return 0;
    }
    /* cd.. / cd\ (Windows) */
    if (!strncmp(argv[0], "cd", 2) && (argv[0][2] == '.' || argv[0][2] == '\\' || argv[0][2] == '/')) {
        char *a[2] = { (char *)"cd", argv[0] + 2 };
        return c_cd(2, a);
    }
    /* "D:" alone switches drive */
    if (argc == 1 && strlen(argv[0]) == 2 && argv[0][1] == ':') {
        char *a[2] = { (char *)"cd", argv[0] };
        return c_cd(2, a);
    }
    /* ./program or a path to a program */
    if (argv[0][0] == '.' || argv[0][0] == '/') {
        char p[256];
        sh_path(argv[0], p, sizeof(p));
        if (user_is_program(p)) return sh_run_with(p, argc, argv);
        if (sh_isdir(p)) { char *a[2] = { (char *)"cd", p }; return c_cd(2, a); }
    }
    /* alias */
    for (int i = 0; i < sh.nalias; i++) {
        if (strcmp(sh.alias[i].name, argv[0]) || sh.depth > 4) continue;
        char line[512];
        snprintf(line, sizeof(line), "%s", sh.alias[i].value);
        for (int k = 1; k < argc; k++) snprintf(line + strlen(line), sizeof(line) - strlen(line), " \"%s\"", argv[k]);
        sh.depth++;
        int rc = sh_exec_line(line);
        sh.depth--;
        return rc;
    }
    const sh_cmd_t *c = sh_find(argv[0]);
    if (c) return c->fn(argc, argv);
    /* a program in /jarvis/bin, or an installed app */
    char p[256];
    snprintf(p, sizeof(p), "/jarvis/bin/%s", argv[0]);
    if (user_is_program(p)) return sh_run_with(p, argc, argv);
    if (pkg_run(argv[0])) return 0;
    /* a Linux program on the Linux PATH (installed with apt or dpkg) */
    if (sh_linux_find(argv[0], p, sizeof(p))) return sh_run_with(p, argc, argv);
    if (desktop_app_find(argv[0]) >= 0) { desktop_app_launch(desktop_app_find(argv[0])); return 0; }
    sh_err("%s: command not found. Type 'help'.\n", argv[0]);
    return 127;
}

/* One pipeline: cmd [| cmd]... with redirections. */
static int sh_run_pipeline(char **v, int *q, int n)
{
    int start = 0, stage = 0, rc = 0;
    const char *prev_out = NULL;
    size_t prev_len = 0;
    sh_sink_t *saved_out = sh.out;
    while (start < n) {
        int end = start;
        while (end < n && !(q[end] == -1 && !strcmp(v[end], "|"))) end++;
        int last = end >= n;

        /* collect words and redirections of this stage */
        char *args[SH_MAX_ARGS];
        int aq[SH_MAX_ARGS], na = 0;
        const char *out_file = NULL, *in_file = NULL;
        int append = 0;
        for (int i = start; i < end; i++) {
            if (q[i] == -1) {
                if ((!strcmp(v[i], ">") || !strcmp(v[i], ">>")) && i + 1 < end) { out_file = v[++i]; append = v[i - 1][1] == '>'; continue; }
                if (!strcmp(v[i], "<") && i + 1 < end) { in_file = v[++i]; continue; }
                if (!strcmp(v[i], "2>") && i + 1 < end) { i++; continue; }      /* errors stay on screen */
                continue;                                                       /* 2>&1, & */
            }
            args[na] = v[i];
            aq[na++] = q[i];
        }
        static char globstore[8192];
        char *gargs[SH_MAX_ARGS * 2];
        int gn = sh_glob(args, aq, na, gargs, SH_MAX_ARGS * 2, globstore, sizeof(globstore));
        int gq[SH_MAX_ARGS * 2];
        for (int i = 0; i < gn; i++) gq[i] = 1;
        for (int i = 0, k = 0; i < na && k < gn; i++, k++) gq[k] = aq[i];

        /* stdin */
        sh.has_in = 0;
        if (in_file) {
            char p[256];
            sh_path(in_file, p, sizeof(p));
            const char *d;
            long l = sh_read_file(p, &d);
            if (l < 0) { sh_err("%s: no such file\n", in_file); rc = 1; break; }
            sh.in = d; sh.in_len = (size_t)l; sh.has_in = 1;
        } else if (stage > 0) {
            sh.in = prev_out; sh.in_len = prev_len; sh.has_in = 1;
        }
        /* stdout */
        static sh_sink_t sinks[2];
        sh_sink_t *sink = NULL;
        if (!last || out_file) {
            sink = &sinks[stage & 1];
            sink->buf = g_pipe[stage & 1];
            sink->cap = SH_PIPE_MAX;
            sink->len = 0;
            sink->overflow = 0;
            sink->buf[0] = 0;
            sh.out = sink;
        } else sh.out = saved_out;

        rc = sh_run_simple(gargs, gq, gn);
        sh.status = rc;

        if (out_file) {
            char p[256];
            sh_path(out_file, p, sizeof(p));
            if (vfs_is_private(p) || vfs_is_dir(p)) { sh_err("cannot write to %s\n", out_file); rc = 1; }
            else if (append) vfs_append(p, sink->buf, sink->len);
            else if (vfs_write_file(p, sink->buf, sink->len) < 0) { sh_err("cannot write to %s%s\n", out_file, fatfs_owns(p) ? " (USB drive)" : ""); rc = 1; }
            desktop_files_changed();
            if (!last) { sink->len = 0; sink->buf[0] = 0; }
        }
        if (sink) { prev_out = sink->buf; prev_len = sink->len; }
        sh.out = saved_out;
        stage++;
        start = end + 1;
    }
    sh.has_in = 0;
    return rc;
}

/* A full line: pipelines joined by ; && || and an optional trailing &.
 * The line is cut at those operators first (outside quotes); each part is
 * expanded ($VAR, wildcards) only when it runs, so "export A=1; echo $A"
 * works as in other shells. */
static int sh_run_segment(const char *seg)
{
    static sh_words_t ws[4];
    sh_words_t *w = &ws[sh.depth];
    if (sh_split(seg, w) < 0) { sh_err("The command line is too long.\n"); return 1; }
    /* the words live in g_wordbuf, which nested lines reuse: copy them */
    static char store[4][4096];
    char *v[SH_MAX_ARGS];
    int q[SH_MAX_ARGS], n = w->n;
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(w->v[i]);
        if (o + l + 1 >= sizeof(store[0])) { n = i; break; }
        memcpy(store[sh.depth] + o, w->v[i], l + 1);
        v[i] = store[sh.depth] + o;
        q[i] = w->quoted[i];
        o += l + 1;
    }
    if (!n) return sh.status;
    sh.depth++;
    int rc = sh_run_pipeline(v, q, n);
    sh.depth--;
    return rc;
}

static int sh_exec_line(const char *line)
{
    if (sh.depth >= 4) return 1;
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", line);
    /* '#' at the start of a word begins a comment (outside quotes) */
    {
        char qc = 0;
        for (char *c = copy; *c; c++) {
            if (qc) { if (*c == qc) qc = 0; continue; }
            if (*c == '"' || *c == '\'') qc = *c;
            else if (*c == '#' && (c == copy || c[-1] == ' ')) { *c = 0; break; }
        }
    }
    /* trailing '&': run in the background */
    size_t l = strlen(copy);
    while (l && copy[l - 1] == ' ') copy[--l] = 0;
    sh_background = l && copy[l - 1] == '&' && (l < 2 || copy[l - 2] != '&');
    if (sh_background) copy[--l] = 0;

    int rc = sh.status, skip = 0;
    char *seg = copy, qc = 0;
    for (char *c = copy;; c++) {
        if (*c && qc) { if (*c == qc) qc = 0; continue; }
        if (*c == '"' || *c == '\'') { qc = *c; continue; }
        int op = 0;                                     /* 1 ;  2 &&  3 || */
        if (!*c) op = 4;
        else if (*c == ';') op = 1;
        else if (c[0] == '&' && c[1] == '&') op = 2;
        else if (c[0] == '|' && c[1] == '|') op = 3;
        if (!op) continue;
        char saved = *c;
        *c = 0;
        if (!skip) { rc = sh_run_segment(seg); sh.status = rc; }
        if (!saved) break;
        skip = (op == 2 && rc != 0) || (op == 3 && rc == 0);
        c += op >= 2 && op <= 3 ? 1 : 0;
        seg = c + 1;
    }
    return rc;
}

/* ---- public ---- */
int shell_execute(const char *line)
{
    sh_init();
    sh.root = NULL;
    sh.out = NULL;
    int rc = sh_exec_line(line);
    desktop_files_changed();
    return rc;
}

/* JARVIS's run_command: runs a line and returns everything it printed. */
int shell_run_capture(const char *line, char *out, size_t n)
{
    sh_init();
    static sh_sink_t cap;
    cap.buf = out; cap.cap = n; cap.len = 0; cap.overflow = 0;
    out[0] = 0;
    sh.root = &cap;
    sh.out = &cap;
    int rc = sh_exec_line(line);
    sh.root = NULL;
    sh.out = NULL;
    if (cap.overflow && n > 20) snprintf(out + n - 20, 20, "\n...(cut short)");
    desktop_files_changed();
    return rc;
}

/* Prompt like "chris@JARVIS-PC:~/Downloads$ ". */
void shell_prompt(char *out, size_t n)
{
    sh_init();
    char host[48], where[200];
    prefs_get("device_name", host, sizeof(host));
    sh_pretty(sh.cwd, where, sizeof(where));
    snprintf(out, n, "%s@%s:%s$ ", account_name(), host, where);
}
