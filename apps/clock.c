/*
 * =============================================================================
 * JARVIS OS — Clock (apps/clock.c) and Run (Win+R)
 *
 * Clock: alarms (kept across restarts), a countdown timer and a stopwatch
 * with laps. Alarms and the timer keep running with the app closed: when
 * one goes off, a notification appears and a chime plays.
 * Terminal / JARVIS: "alarm 07:30 Wake up", "alarm list", "alarm delete 1",
 * "timer 5m", "timer", "timer stop", "stopwatch start|stop|lap|reset".
 *
 * Run: type a program, app, file, folder or command and press Enter.
 * =============================================================================
 */

#define CK_MAX_ALARMS 8

static struct {
    wm_window_t *win;
    int tab;                            /* 0 alarm, 1 timer, 2 stopwatch */
    struct { int h, m, on; char label[24]; int fired_ymd; } alarm[CK_MAX_ALARMS];
    int nalarms, loaded;
    int new_h, new_m;                   /* the "add alarm" spinner */
    /* timer */
    int timer_set_s;                    /* chosen length */
    int timer_running;
    uint64_t timer_end;                 /* tick it ends */
    int timer_left_s;                   /* when paused */
    /* stopwatch */
    int sw_running;
    uint64_t sw_start, sw_acc;          /* ticks */
    uint64_t laps[10]; int nlaps;
} ck;

/* ---- chime --------------------------------------------------------------------- */
static void ck_chime(void)
{
    static float buf[16000];
    static int made;
    if (!made) {
        made = 1;
        for (int i = 0; i < 16000; i++) {
            float t = i / 16000.0f;
            float f = t < 0.35f ? 880.0f : t < 0.7f ? 1318.5f : 1760.0f;
            float local = t < 0.35f ? t : t < 0.7f ? t - 0.35f : t - 0.7f;
            float env = wp_exp_neg(local * 9.0f) * (local < 0.01f ? local * 100 : 1);
            buf[i] = 0.35f * env * wp_sin(2 * WP_PI * f * t);
        }
    }
    audio_write_frames(buf, 16000);
}

/* ---- alarms: saved as "07:30:1:Wake up|..." -------------------------------------- */
static void ck_save(void)
{
    char b[400] = "";
    for (int i = 0; i < ck.nalarms; i++)
        snprintf(b + strlen(b), sizeof(b) - strlen(b), "%s%02d:%02d:%d:%s", i ? "|" : "", ck.alarm[i].h, ck.alarm[i].m, ck.alarm[i].on, ck.alarm[i].label);
    setting_set("alarms", b[0] ? b : "-");
}

static void ck_load(void)
{
    if (ck.loaded) return;
    ck.loaded = 1;
    ck.timer_set_s = 300;
    ck.new_h = 7;
    char b[400];
    setting_get("alarms", b, sizeof(b));
    ck.nalarms = 0;
    for (char *p = b; *p && *p != '-' && ck.nalarms < CK_MAX_ALARMS;) {
        char *e = strchr(p, '|');
        if (e) *e = 0;
        int h = (int)sh_num(p, -1), m = p[2] == ':' ? (int)sh_num(p + 3, -1) : -1;
        if (h >= 0 && h < 24 && m >= 0 && m < 60) {
            ck.alarm[ck.nalarms].h = h;
            ck.alarm[ck.nalarms].m = m;
            ck.alarm[ck.nalarms].on = p[6] == '1';
            snprintf(ck.alarm[ck.nalarms].label, sizeof(ck.alarm[0].label), "%s", strlen(p) > 8 ? p + 8 : "");
            ck.nalarms++;
        }
        if (!e) break;
        p = e + 1;
    }
}

static int ck_add_alarm(int h, int m, const char *label)
{
    ck_load();
    if (ck.nalarms >= CK_MAX_ALARMS) return -1;
    ck.alarm[ck.nalarms].h = h;
    ck.alarm[ck.nalarms].m = m;
    ck.alarm[ck.nalarms].on = 1;
    ck.alarm[ck.nalarms].fired_ymd = 0;
    snprintf(ck.alarm[ck.nalarms].label, sizeof(ck.alarm[0].label), "%s", label && *label ? label : "Alarm");
    for (char *c = ck.alarm[ck.nalarms].label; *c; c++) if (*c == '|' || *c == ':') *c = ' ';
    ck.nalarms++;
    ck_save();
    return 0;
}

static void ck_delete_alarm(int i)
{
    if (i < 0 || i >= ck.nalarms) return;
    for (int k = i; k < ck.nalarms - 1; k++) ck.alarm[k] = ck.alarm[k + 1];
    ck.nalarms--;
    ck_save();
}

static void ck_timer_start(int seconds)
{
    ck.timer_set_s = seconds;
    ck.timer_left_s = seconds;
    ck.timer_running = 1;
    ck.timer_end = g_kstate.uptime_ticks + (uint64_t)seconds * 100;
}

static int ck_timer_left(void)
{
    if (!ck.timer_running) return ck.timer_left_s;
    int64_t left = (int64_t)ck.timer_end - (int64_t)g_kstate.uptime_ticks;
    return left > 0 ? (int)((left + 99) / 100) : 0;
}

static uint64_t ck_sw_ticks(void)
{
    return ck.sw_acc + (ck.sw_running ? g_kstate.uptime_ticks - ck.sw_start : 0);
}

/* Called twice a second by the desktop: alarms and the timer. */
void clock_tick(void)
{
    ck_load();
    sys_time_t t;
    sys_get_time(&t);
    int ymd = t.year * 10000 + t.month * 100 + t.day;
    for (int i = 0; i < ck.nalarms; i++) {
        if (!ck.alarm[i].on || ck.alarm[i].fired_ymd == ymd) continue;
        if (ck.alarm[i].h == t.hour && ck.alarm[i].m == t.minute) {
            ck.alarm[i].fired_ymd = ymd;
            char b[64];
            snprintf(b, sizeof(b), "%02d:%02d  %s", ck.alarm[i].h, ck.alarm[i].m, ck.alarm[i].label);
            notification_show("Alarm", b, NOTIF_WARNING, 30000);
            ck_chime();
        }
    }
    if (ck.timer_running && g_kstate.uptime_ticks >= ck.timer_end) {
        ck.timer_running = 0;
        ck.timer_left_s = 0;
        notification_show("Timer", "Time is up.", NOTIF_WARNING, 20000);
        ck_chime();
    }
    if (ck.win && (ck.timer_running || ck.sw_running)) desktop_invalidate();
}

/* ---- UI ----------------------------------------------------------------------- */
static const char *const ck_tabs[3] = { "Alarm", "Timer", "Stopwatch" };

static void ck_fmt_hms(int s, char *b, size_t n)
{
    if (s >= 3600) snprintf(b, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, n, "%02d:%02d", s / 60, s % 60);
}

static void ck_btn(int x, int y, int w, const char *label, int primary)
{
    ui_button(x, y, w, 34, label, primary ? T(ACCENT) : T(HOVER), primary ? T(ACCENT_TEXT) : T(TEXT));
}

static void ck_draw(wm_window_t *win, uint32_t *fb)
{
    (void)fb;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    ck_load();
    fb_fill_rect(x, y, w, h, T(WINDOW));
    /* tabs */
    for (int i = 0; i < 3; i++) {
        int tx = x + 20 + i * 110;
        if (i == ck.tab) fb_fill_rounded_rect(tx, y + 12, 100, 32, 6, T(ACCENT));
        ui_text_center(tx, y + 20, 100, ck_tabs[i], i == ck.tab ? T(ACCENT_TEXT) : T(TEXT));
    }
    char b[64];
    int cy = y + 64;
    if (ck.tab == 0) {
        sys_time_t t;
        sys_get_time(&t);
        snprintf(b, sizeof(b), "%02d:%02d", t.hour, t.minute);
        fb_draw_text_scaled(x + 20, cy, b, T(TEXT), 3);
        fb_draw_text(x + 150, cy + 20, "now", T(TEXT_DIM));
        cy += 60;
        for (int i = 0; i < ck.nalarms; i++, cy += 46) {
            ui_card(x + 20, cy, w - 40, 40);
            snprintf(b, sizeof(b), "%02d:%02d", ck.alarm[i].h, ck.alarm[i].m);
            fb_draw_text_bold(x + 34, cy + 12, b, ck.alarm[i].on ? T(TEXT) : T(TEXT_DIM));
            ui_text_fit(x + 100, cy + 12, w - 260, ck.alarm[i].label, T(TEXT_DIM));
            int on = ck.alarm[i].on, tx = x + w - 130;
            fb_fill_rounded_rect(tx, cy + 10, 44, 20, 10, on ? T(ACCENT) : T(HOVER));
            fb_fill_circle(on ? tx + 33 : tx + 11, cy + 20, 7, on ? T(ACCENT_TEXT) : T(TEXT));
            fb_draw_text(x + w - 70, cy + 12, "Delete", T(DANGER));
        }
        if (!ck.nalarms) { fb_draw_text(x + 20, cy, "No alarms. Add one below.", T(TEXT_DIM)); cy += 30; }
        /* add */
        int ay = y + h - 60;
        fb_fill_rect(x, ay - 10, w, 1, T(BORDER));
        snprintf(b, sizeof(b), "%02d", ck.new_h);
        ck_btn(x + 20, ay, 34, "-", 0); fb_draw_text_bold(x + 64, ay + 9, b, T(TEXT)); ck_btn(x + 90, ay, 34, "+", 0);
        fb_draw_text_bold(x + 132, ay + 9, ":", T(TEXT));
        snprintf(b, sizeof(b), "%02d", ck.new_m);
        ck_btn(x + 144, ay, 34, "-", 0); fb_draw_text_bold(x + 188, ay + 9, b, T(TEXT)); ck_btn(x + 214, ay, 34, "+", 0);
        ck_btn(x + w - 140, ay, 120, "Add alarm", 1);
    } else if (ck.tab == 1) {
        ck_fmt_hms(ck_timer_left(), b, sizeof(b));
        if (!ck.timer_running && !ck.timer_left_s) ck_fmt_hms(ck.timer_set_s, b, sizeof(b));
        int tw = fb_text_width_scaled(b, 4);
        fb_draw_text_scaled(x + (w - tw) / 2, cy + 10, b, ck.timer_running ? T(ACCENT) : T(TEXT), 4);
        int total = ck.timer_set_s ? ck.timer_set_s : 1;
        int pct = ck.timer_running || ck.timer_left_s ? (total - ck_timer_left()) * 100 / total : 0;
        ui_progress(x + 40, cy + 100, w - 80, 6, pct, T(HOVER), T(ACCENT));
        static const char *const pl[5] = { "1 min", "5 min", "10 min", "25 min", "1 hour" };
        int bw = (w - 40 - 4 * 8) / 5;
        for (int i = 0; i < 5; i++) ck_btn(x + 20 + i * (bw + 8), cy + 130, bw, pl[i], 0);
        ck_btn(x + 20, cy + 176, 80, "-1 min", 0);
        ck_btn(x + 108, cy + 176, 80, "+1 min", 0);
        ck_btn(x + 196, cy + 176, 80, "+10 s", 0);
        ck_btn(x + w - 230, y + h - 60, 100, ck.timer_running ? "Pause" : (ck.timer_left_s ? "Resume" : "Start"), 1);
        ck_btn(x + w - 120, y + h - 60, 100, "Reset", 0);
    } else {
        uint64_t tk = ck_sw_ticks();
        snprintf(b, sizeof(b), "%02u:%02u.%02u", (unsigned)(tk / 6000), (unsigned)(tk / 100 % 60), (unsigned)(tk % 100));
        int tw = fb_text_width_scaled(b, 4);
        fb_draw_text_scaled(x + (w - tw) / 2, cy + 10, b, ck.sw_running ? T(ACCENT) : T(TEXT), 4);
        int ly = cy + 100;
        for (int i = ck.nlaps - 1; i >= 0 && ly < y + h - 80; i--, ly += 22) {
            uint64_t l = ck.laps[i], prev = i ? ck.laps[i - 1] : 0, d = l - prev;
            snprintf(b, sizeof(b), "Lap %d     %02u:%02u.%02u     +%02u:%02u.%02u", i + 1, (unsigned)(l / 6000), (unsigned)(l / 100 % 60), (unsigned)(l % 100),
                     (unsigned)(d / 6000), (unsigned)(d / 100 % 60), (unsigned)(d % 100));
            fb_draw_text(x + 40, ly, b, T(TEXT_DIM));
        }
        ck_btn(x + w - 340, y + h - 60, 100, ck.sw_running ? "Stop" : "Start", 1);
        ck_btn(x + w - 230, y + h - 60, 100, "Lap", 0);
        ck_btn(x + w - 120, y + h - 60, 100, "Reset", 0);
    }
}

static void ck_event(wm_window_t *win, wm_input_event_t *e)
{
    if (e->type != IE_MOUSE_DOWN || e->data.mouse.button != 1) return;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    int mx = e->data.mouse.x, my = e->data.mouse.y;
    for (int i = 0; i < 3; i++) if (ui_hit(mx, my, 20 + i * 110, 12, 100, 32)) { ck.tab = i; return; }
    int cy = 64;
    if (ck.tab == 0) {
        cy += 60;
        for (int i = 0; i < ck.nalarms; i++, cy += 46) {
            if (ui_hit(mx, my, w - 130, cy + 6, 50, 28)) { ck.alarm[i].on = !ck.alarm[i].on; ck_save(); return; }
            if (ui_hit(mx, my, w - 74, cy + 6, 60, 28)) { ck_delete_alarm(i); return; }
        }
        int ay = h - 60;
        if (ui_hit(mx, my, 20, ay, 34, 34)) ck.new_h = (ck.new_h + 23) % 24;
        else if (ui_hit(mx, my, 90, ay, 34, 34)) ck.new_h = (ck.new_h + 1) % 24;
        else if (ui_hit(mx, my, 144, ay, 34, 34)) ck.new_m = (ck.new_m + 55) % 60;
        else if (ui_hit(mx, my, 214, ay, 34, 34)) ck.new_m = (ck.new_m + 5) % 60;
        else if (ui_hit(mx, my, w - 140, ay, 120, 34)) {
            if (ck_add_alarm(ck.new_h, ck.new_m, "Alarm") != 0) notification_show("Clock", "There are already 8 alarms.", NOTIF_WARNING, 3000);
        }
    } else if (ck.tab == 1) {
        static const int presets[5] = { 60, 300, 600, 1500, 3600 };
        int bw = (w - 40 - 4 * 8) / 5;
        for (int i = 0; i < 5; i++) if (ui_hit(mx, my, 20 + i * (bw + 8), cy + 130, bw, 34)) { ck.timer_running = 0; ck.timer_left_s = 0; ck.timer_set_s = presets[i]; return; }
        int adj = ui_hit(mx, my, 20, cy + 176, 80, 34) ? -60 : ui_hit(mx, my, 108, cy + 176, 80, 34) ? 60 : ui_hit(mx, my, 196, cy + 176, 80, 34) ? 10 : 0;
        if (adj) {
            if (ck.timer_running) { ck.timer_end += (uint64_t)((int64_t)adj * 100); }
            else if (ck.timer_left_s) { ck.timer_left_s += adj; if (ck.timer_left_s < 1) ck.timer_left_s = 1; }
            else { ck.timer_set_s += adj; if (ck.timer_set_s < 10) ck.timer_set_s = 10; }
            return;
        }
        if (ui_hit(mx, my, w - 230, h - 60, 100, 34)) {
            if (ck.timer_running) { ck.timer_left_s = ck_timer_left(); ck.timer_running = 0; }
            else ck_timer_start(ck.timer_left_s ? ck.timer_left_s : ck.timer_set_s);
        } else if (ui_hit(mx, my, w - 120, h - 60, 100, 34)) { ck.timer_running = 0; ck.timer_left_s = 0; }
    } else {
        if (ui_hit(mx, my, w - 340, h - 60, 100, 34)) {
            if (ck.sw_running) { ck.sw_acc = ck_sw_ticks(); ck.sw_running = 0; }
            else { ck.sw_start = g_kstate.uptime_ticks; ck.sw_running = 1; }
        } else if (ui_hit(mx, my, w - 230, h - 60, 100, 34)) {
            if (ck.nlaps < 10) ck.laps[ck.nlaps++] = ck_sw_ticks();
        } else if (ui_hit(mx, my, w - 120, h - 60, 100, 34)) { ck.sw_running = 0; ck.sw_acc = 0; ck.nlaps = 0; }
    }
}

wm_window_t *clock_app_open(int x, int y, int w, int h)
{
    ck_load();
    ck.win = wm_create_window("Clock", x, y, w, h, WS_DEFAULT, ck_draw, ck_event);
    if (ck.win) ck.win->icon = ICON_CLOCK;
    return ck.win;
}

/* ---- Terminal / JARVIS ------------------------------------------------------------ */
/* "5m", "90s", "1h30m", "2:30" (min:sec), "1:00:00", or a bare number of minutes. */
static long ck_parse_duration(const char *s)
{
    if (strchr(s, ':')) {
        long parts[3] = { 0 }, np = 0, cur = 0;
        for (; *s && np < 3; s++) {
            if (*s >= '0' && *s <= '9') cur = cur * 10 + (*s - '0');
            else if (*s == ':') { parts[np++] = cur; cur = 0; }
            else return -1;
        }
        parts[np++] = cur;
        return np == 2 ? parts[0] * 60 + parts[1] : parts[0] * 3600 + parts[1] * 60 + parts[2];
    }
    long total = 0, n = 0;
    int digits = 0, units = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); digits = 1; continue; }
        if (*s == 'h') total += n * 3600; else if (*s == 'm') total += n * 60; else if (*s == 's') total += n;
        else continue;                                      /* "min", "sec": only the first letter counts */
        units = 1;
        n = 0;
        while (s[1] && !(s[1] >= '0' && s[1] <= '9')) s++;
    }
    if (!digits) return -1;
    if (!units) return n * 60;                              /* bare number: minutes */
    return total + n;
}

int c_timer(int argc, char **argv)
{
    ck_load();
    char b[32];
    if (argc < 2) {
        if (!ck.timer_running && !ck.timer_left_s) { sh_printf("No timer running. Start one: timer 5m (or 90s, 1h30m)\n"); return 0; }
        ck_fmt_hms(ck_timer_left(), b, sizeof(b));
        sh_printf("Timer: %s left%s\n", b, ck.timer_running ? "" : " (paused)");
        return 0;
    }
    if (!strcmp(argv[1], "stop") || !strcmp(argv[1], "cancel")) { ck.timer_running = 0; ck.timer_left_s = 0; sh_printf("Timer stopped.\n"); return 0; }
    long s = ck_parse_duration(argv[1]);
    if (s <= 0 || s > 24 * 3600) { sh_err("timer: give a length like 5m, 90s or 1h30m\n"); return 1; }
    ck_timer_start((int)s);
    ck_fmt_hms((int)s, b, sizeof(b));
    sh_printf("Timer set for %s. A chime and a notification will tell you when it ends.\n", b);
    return 0;
}

int c_alarm(int argc, char **argv)
{
    ck_load();
    if (argc < 2 || !strcmp(argv[1], "list")) {
        if (!ck.nalarms) sh_printf("No alarms. Set one: alarm 07:30 [label]\n");
        for (int i = 0; i < ck.nalarms; i++)
            sh_printf("%d  %02d:%02d  %s  %s\n", i + 1, ck.alarm[i].h, ck.alarm[i].m, ck.alarm[i].on ? "on " : "off", ck.alarm[i].label);
        return 0;
    }
    if ((!strcmp(argv[1], "delete") || !strcmp(argv[1], "remove")) && argc > 2) {
        int i = (int)sh_num(argv[2], 0) - 1;
        if (i < 0 || i >= ck.nalarms) { sh_err("alarm: no alarm %s (see: alarm list)\n", argv[2]); return 1; }
        ck_delete_alarm(i);
        sh_printf("Alarm deleted.\n");
        return 0;
    }
    if ((!strcmp(argv[1], "off") || !strcmp(argv[1], "on")) && argc > 2) {
        int i = (int)sh_num(argv[2], 0) - 1;
        if (i < 0 || i >= ck.nalarms) { sh_err("alarm: no alarm %s\n", argv[2]); return 1; }
        ck.alarm[i].on = argv[1][1] == 'n';
        ck_save();
        return 0;
    }
    int hh = (int)sh_num(argv[1], -1), mm = -1;
    const char *c = strchr(argv[1], ':');
    if (c) mm = (int)sh_num(c + 1, -1);
    if (strstr(argv[1], "pm") || strstr(argv[1], "PM")) { if (hh < 12) hh += 12; }
    else if ((strstr(argv[1], "am") || strstr(argv[1], "AM")) && hh == 12) hh = 0;
    if (!c) mm = 0;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) { sh_err("alarm: give a time like 07:30 or 6:45pm\n"); return 1; }
    char label[64] = "";
    for (int i = 2; i < argc; i++) snprintf(label + strlen(label), sizeof(label) - strlen(label), "%s%s", i > 2 ? " " : "", argv[i]);
    if (ck_add_alarm(hh, mm, label)) { sh_err("alarm: there are already 8 alarms\n"); return 1; }
    sh_printf("Alarm set for %02d:%02d%s%s.\n", hh, mm, label[0] ? " - " : "", label);
    return 0;
}

int c_stopwatch(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "status";
    if (!strcmp(a, "start")) { if (!ck.sw_running) { ck.sw_start = g_kstate.uptime_ticks; ck.sw_running = 1; } }
    else if (!strcmp(a, "stop")) { if (ck.sw_running) { ck.sw_acc = ck_sw_ticks(); ck.sw_running = 0; } }
    else if (!strcmp(a, "lap")) { if (ck.nlaps < 10) ck.laps[ck.nlaps++] = ck_sw_ticks(); }
    else if (!strcmp(a, "reset")) { ck.sw_running = 0; ck.sw_acc = 0; ck.nlaps = 0; }
    uint64_t tk = ck_sw_ticks();
    sh_printf("Stopwatch: %02u:%02u.%02u %s\n", (unsigned)(tk / 6000), (unsigned)(tk / 100 % 60), (unsigned)(tk % 100), ck.sw_running ? "(running)" : "(stopped)");
    return 0;
}

/* ---- Run (Win+R) --------------------------------------------------------------------- */
static struct { wm_window_t *win; char text[200]; int len; char err[96]; } rn;

static void rn_go(void)
{
    char p[256];
    const char *t = rn.text;
    while (*t == ' ') t++;
    if (!*t) return;
    rn.err[0] = 0;
    sh_path(t, p, sizeof(p));
    int idx = desktop_app_find(t);
    if (idx >= 0) desktop_app_launch(idx);
    else if (!strchr(t, ' ') && (sh_isdir(p) || vfs_exists(p))) { char *a[2] = { (char *)"open", p }; c_open(2, a); }
    else if (pkg_run(t)) { /* an installed app */ }
    else {
        /* anything else: a command, shown in the Terminal */
        desktop_app_launch(desktop_app_find("Terminal"));
        term_echo_command(t);
        terminal_app_execute_command(t);
    }
    if (rn.win) wm_request_close(rn.win);
}

static void rn_draw(wm_window_t *win, uint32_t *fb)
{
    (void)fb;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    fb_fill_rect(x, y, w, h, T(WINDOW));
    icon_draw(ICON_TERMINAL, x + 18, y + 18, 32);
    fb_draw_text(x + 64, y + 16, "Type the name of an app, program, folder or file,", T(TEXT));
    fb_draw_text(x + 64, y + 34, "or any Terminal command, and JARVIS OS will open it.", T(TEXT));
    fb_draw_text(x + 18, y + 72, "Open:", T(TEXT));
    fb_fill_rounded_rect(x + 64, y + 66, w - 82, 30, 4, T(INPUT));
    fb_draw_rounded_rect(x + 64, y + 66, w - 82, 30, 4, T(ACCENT));
    ui_text_fit(x + 72, y + 73, w - 98, rn.text, T(TEXT));
    if ((g_kstate.uptime_ticks / 50) % 2 == 0) fb_fill_rect(x + 73 + fb_text_width(rn.text), y + 73, 1, 16, T(TEXT));
    ui_button(x + w - 220, y + h - 44, 96, 30, "OK", T(ACCENT), T(ACCENT_TEXT));
    ui_button(x + w - 114, y + h - 44, 96, 30, "Cancel", T(HOVER), T(TEXT));
}

static void rn_event(wm_window_t *win, wm_input_event_t *e)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    if (e->type == IE_TEXT_INPUT) {
        char c = e->data.text.ch;
        if (c >= 32 && c < 127 && rn.len < (int)sizeof(rn.text) - 1) { rn.text[rn.len++] = c; rn.text[rn.len] = 0; }
    } else if (e->type == IE_KEY_DOWN) {
        uint32_t k = e->data.keyboard.key;
        if (k == WM_KEY_ENTER) rn_go();
        else if (k == WM_KEY_ESC) wm_request_close(win);
        else if (k == WM_KEY_BACKSPACE && rn.len) rn.text[--rn.len] = 0;
    } else if (e->type == IE_MOUSE_DOWN && e->data.mouse.button == 1) {
        if (ui_hit(e->data.mouse.x, e->data.mouse.y, w - 220, h - 44, 96, 30)) rn_go();
        else if (ui_hit(e->data.mouse.x, e->data.mouse.y, w - 114, h - 44, 96, 30)) wm_request_close(win);
    }
}

wm_window_t *run_app_open(int x, int y, int w, int h)
{
    (void)x; (void)y;
    rn.text[0] = 0;
    rn.len = 0;
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    rn.win = wm_create_window("Run", 12, H - TASKBAR_H - h - 12, w, h, WS_FIXED, rn_draw, rn_event);
    (void)W;
    if (rn.win) rn.win->icon = ICON_TERMINAL;
    return rn.win;
}
