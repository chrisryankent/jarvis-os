/*
 * =============================================================================
 * Kryzen OS — Sign-in and first-run screens (desktop/login.c)
 * Language: C17
 *
 * Sign-in (after boot and whenever the PC is locked): a glass card over the
 * nebula backdrop. Type the password or PIN, or speak the passphrase: JARVIS
 * asks for it out loud. Spoken attempts never count towards the typed-attempt
 * slowdown, so mishearing cannot lock you out; typing always works.
 *
 * First run: the Kryzen Initialization Wizard, three short pages
 *   1. Welcome      your name, the assistant, host environments, profile import
 *   2. Security     password, PIN, spoken passphrase (each optional)
 *   3. This device  device name, colour theme, resolution, clock format
 * It then creates the account (kernel/accounts.c). After a disk installation
 * the same wizard runs and ends on the installer's "ready" page.
 *
 * Included by desktop/ui_shell.c, which provides the fs_* drawing helpers.
 * =============================================================================
 */

enum { F_NAME, F_PASS, F_PIN, F_VOICE, F_DEVICE, F_COUNT };
enum { SW_WELCOME, SW_SECURITY, SW_DEVICE };

static struct {
    /* sign-in */
    char entry[64]; int entry_len;
    int  pin_mode;                  /* 1: the field takes a PIN, 0: password */
    char message[112]; uint32_t message_color;
    int  greeted;                   /* JARVIS asked for the passphrase */
    int  reveal;                    /* show the typed characters */
    /* setup */
    char field[F_COUNT][64]; int field_len[F_COUNT];
    int  page, focus;
    int  opt_ai;                    /* JARVIS listens and opens at sign-in */
    int  host_nt, host_linux;       /* host environments switched on */
    int  theme_id, clock24, res_index;
    int  account_created;
    char error[128];
    char import_path[96];           /* profile found on a connected drive */
    int  import_done, import_keys, import_values;
} lg;

typedef struct { int x, y, w, h; } lrect_t;
static int lr_hit(const lrect_t *r, int mx, int my) { return ui_hit(mx, my, r->x, r->y, r->w, r->h); }

static void lg_set_message(const char *text, uint32_t color)
{
    snprintf(lg.message, sizeof(lg.message), "%s", text);
    lg.message_color = color;
}

static void lg_unlock(void)
{
    char hello[80];
    snprintf(hello, sizeof(hello), "Welcome back, %s.", account_name());
    lg.entry_len = 0; lg.entry[0] = 0;
    lg.message[0] = 0;
    g_fs.power_open = 0;
    ui_shell_unlock_desktop();
    if (account_has_voice()) tts_speak(hello, strlen(hello));
}

/* ---- Sign-in screen ------------------------------------------------------ */
static void lg_ask_passphrase(void)
{
    const char *ask = account_has_password() || account_has_pin()
        ? "Please say your passphrase, or type your PIN or password."
        : "Please say your passphrase.";
    tts_speak(ask, strlen(ask));
    lg.greeted = 1;
}

/* The microphone bridge just connected: ask now if we could not before. */
void ui_shell_voice_prompt(void)
{
    if (g_ui.mode == UI_MODE_LOCK && account_has_voice()) lg_ask_passphrase();
    desktop_invalidate();
}

/* Start over for the selected user. */
static void lock_reset_entry(void)
{
    lg.entry_len = 0; lg.entry[0] = 0;
    lg.message[0] = 0;
    lg.pin_mode = account_has_pin() && !account_has_password();
    lg.greeted = 0;
    lg.reveal = 0;
}

static void login_on_show_lock(void)
{
    if (user_current() >= 0) user_select(user_current());   /* a locked session: its owner first */
    lock_reset_entry();
    g_fs.power_open = 0;
    if (account_has_voice() && kernel_voice_bridge_connected()) {
        lg_ask_passphrase();
    }
}

static void lock_card_rect(int *x, int *y, int *w, int *h)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    *w = 400;
    *h = H >= 640 ? 500 : H - 110;
    *x = (W - *w) / 2;
    *y = (H - *h) / 2 + 6;
}

static void lock_field_rect(int *fx, int *fy, int *fw, int *fh)
{
    int x, y, w, h;
    lock_card_rect(&x, &y, &w, &h);
    *fx = x + 44; *fw = w - 88; *fh = 44;
    *fy = y + 282;
}

/* Other accounts on this PC: bottom-left, above the edge. */
static void lock_user_rect(int i, lrect_t *r)
{
    int n = user_count();
    *r = (lrect_t){ FS_SIDE_W + 26, (int)fb_get_height() - 18 - (n - i) * 50, 230, 42 };
}

static void login_draw_users(void)
{
    int n = user_count();
    if (n < 2) return;
    lrect_t r;
    lock_user_rect(0, &r);
    fb_text(r.x + 2, r.y - 26, "Users", FS_DIM, FONT_UI_BOLD);
    for (int i = 0; i < n; i++) {
        user_info_t u;
        char cap[48], initial[2];
        if (user_info(i, &u) != 0) continue;
        lock_user_rect(i, &r);
        int sel = i == user_selected();
        fs_card(r.x, r.y, r.w, r.h, sel);
        fb_fill_circle(r.x + 22, r.y + 21, 14, sel ? 0x00205878 : 0x00283050);
        initial[0] = u.name[0] >= 'a' && u.name[0] <= 'z' ? (char)(u.name[0] - 32) : u.name[0];
        initial[1] = 0;
        fs_text_center(r.x + 8, r.y + 13, 28, initial, FS_WHITE, FONT_UI_BOLD);
        fs_text_fit(r.x + 46, r.y + 5, r.w - 56, u.name, FS_WHITE, FONT_UI_BOLD);
        snprintf(cap, sizeof(cap), "%s%s", u.admin ? "Administrator" : "Standard user", u.signed_in ? "  \x80  signed in" : "");
        fb_text(r.x + 46, r.y + 22, cap, u.signed_in ? T(OK) : FS_DIM, FONT_SMALL);
    }
}

static void login_draw_clock_card(void)
{
    int W = (int)fb_get_width();
    int big = W >= 1200;
    int cw = big ? 318 : 230, ch = big ? 150 : 118;
    int cx = W - cw - 14, cy = 14;
    char clock[16], date[48];
    sys_format_clock(clock, sizeof(clock));
    sys_format_date_long(date, sizeof(date));
    ui_glass_tinted(cx, cy, cw, ch, 14, 0x00222644, 140);
    int font = big ? FONT_DISPLAY : FONT_LARGE;
    fb_text(cx + cw - 18 - fb_text_width_font(clock, font), cy + (big ? 6 : 4), clock, 0x00D4F2FF, font);
    int dfont = big ? FONT_SUBHEAD : FONT_UI;
    fb_text(cx + cw - 18 - fb_text_width_font(date, dfont), cy + (big ? 78 : 58), date, FS_WHITE, dfont);
    /* live state: microphone bridge, sound, network */
    net_info_t ni;
    net_get_info(&ni);
    int ix = cx + cw - 18 - 18, iy = cy + ch - 30;
    glyph_draw(GLYPH_NET, ix, iy, 18, ni.configured ? FS_WHITE : FS_FAINT, 0); ix -= 30;
    glyph_draw(GLYPH_VOLUME, ix, iy, 18, g_kstate.audio_ready && !setting_get_int("mute", 0) ? FS_WHITE : FS_FAINT, 0); ix -= 30;
    glyph_draw(GLYPH_MIC, ix, iy, 18, kernel_voice_bridge_connected() ? FS_WHITE : FS_FAINT, 0x00262A46);
}

static void login_draw_lock(void)
{
    int x, y, w, h;
    draw_backdrop();
    fs_sidebar(account_has_password() && account_has_pin());
    fs_status_list("Status, Security & Host Monitor", 0, 6);
    login_draw_clock_card();
    login_draw_users();

    lock_card_rect(&x, &y, &w, &h);
    fs_panel(x, y, w, h, 22);

    /* avatar */
    int ax = x + w / 2, ay = y + 104;
    fb_glow_circle(ax, ay, 62, 14, UI_NEON_CYAN, 60);
    fb_fill_circle(ax, ay, 62, 0x000B1830);
    fb_draw_arc(ax, ay, 64, 3, 0, 360, 0x0062DCF8, 255);
    fb_draw_arc(ax, ay, 56, 1, 0, 360, 0x004C88C8, 200);
    glyph_draw(GLYPH_USER, ax - 30, ay - 34, 60, 0x0078D4F4, 0x000B1830);

    const char *name = account_name();
    int nfont = fb_text_width_font(name, FONT_HEADLINE) <= w - 40 ? FONT_HEADLINE : FONT_TITLE;
    fs_text_center(x, y + 180, w, name, FS_WHITE, nfont);

    if (!account_has_sign_in()) {
        fs_text_center(x, y + 268, w, user_count() > 1 ? "Press Enter or click here to sign in" : "Press any key or click to sign in", FS_DIM, FONT_MEDIUM);
        fs_text_center(x, y + 296, w, "This account has no password, PIN or passphrase.", FS_FAINT, FONT_UI);
        fs_tray();
        return;
    }

    int fx, fy, fw, fh;
    lock_field_rect(&fx, &fy, &fw, &fh);
    int typed = account_has_password() || account_has_pin();
    int voice = account_has_voice();
    int bridge = kernel_voice_bridge_connected();
    if (typed) {
        fb_text(fx + 8, fy - 30, lg.pin_mode ? "PIN" : "Password", FS_WHITE, FONT_MEDIUM);
        {   /* show / hide what is typed */
            const char *t = lg.reveal ? "Hide" : "Show";
            fb_text(fx + fw - 8 - fb_text_width_font(t, FONT_UI), fy - 26, t, UI_NEON_CYAN, FONT_UI);
        }
        fs_field(fx, fy, fw - 46, fh, lg.entry, lg.entry_len, !lg.reveal, lg.pin_mode ? "Enter your PIN" : "Enter your password", 1, GLYPH_PADLOCK);
        fb_neon_rounded_rect(fx, fy, fw, fh, fh / 2, 1, 0, 0x00C8D8FF, 0x00C8D8FF, 40);
        fb_blend_rounded_rect(fx + fw - 42, fy + 4, 38, fh - 8, (fh - 8) / 2, 0x00FFFFFF, lg.entry_len ? 70 : 36);
        glyph_draw(GLYPH_ARROW_RIGHT, fx + fw - 33, fy + 12, 20, FS_WHITE, 0);
        fs_text_center(x, fy + fh + 12, w,
                       voice && bridge ? "Press Enter, or say your passphrase to unlock" : "Press Enter to unlock",
                       FS_DIM, FONT_UI);
    } else {
        fs_text_center(x, fy + 6, w, "Say your passphrase to unlock", FS_WHITE, FONT_MEDIUM);
    }

    /* message line: wrong secret, slowdown */
    int wait = auth_wait_seconds();
    char m[80];
    const char *msg = lg.message;
    uint32_t mcol = lg.message_color;
    if (wait) { snprintf(m, sizeof(m), "Too many wrong attempts. Try again in %d s.", wait); msg = m; mcol = T(WARN); }
    else if (!msg[0] && typed && input_caps_lock()) { msg = "Caps Lock is on."; mcol = T(WARN); }
    else if (!msg[0] && user_current() >= 0 && user_current() != user_selected()) {
        user_info_t other;
        if (user_info(user_current(), &other) == 0) {
            snprintf(m, sizeof(m), "Signing in here ends %s's session.", other.name);
            msg = m; mcol = FS_DIM;
        }
    }
    if (msg[0]) fs_paragraph(x + 30, fy + fh + 34, w - 60, msg, mcol, FONT_UI, 18);

    /* voice orb with the live microphone level */
    int oy = y + h - 92;
    int listening = voice && bridge;
    fs_voice_wave(ax, oy, 110, 20, listening ? ui_shell_get_mic_level() : 0,
                  listening ? UI_NEON_CYAN : 0x00506080, listening ? UI_NEON_VIOLET : 0x00506080);
    fb_glow_circle(ax, oy, 26, 10, listening ? UI_NEON_CYAN : 0x00405070, 60);
    fb_fill_circle(ax, oy, 26, 0x000B1830);
    fb_draw_arc(ax, oy, 28, 3, 150, 200, listening ? UI_NEON_CYAN : 0x00607090, 255);
    fb_draw_arc(ax, oy, 28, 3, 350, 150, listening ? UI_NEON_VIOLET : 0x00607090, 255);
    glyph_draw(GLYPH_MIC, ax - 12, oy - 12, 24, listening ? 0x00C8F4FF : 0x008C98B8, 0x000B1830);
    fs_text_center(x, y + h - 54, w,
                   listening ? "Kryzen AI online" : voice ? "Kryzen AI: microphone bridge not connected" : "Kryzen AI",
                   FS_WHITE, FONT_MEDIUM);
    fs_text_center(x, y + h - 30, w,
                   listening ? "Listening for your passphrase" :
                   voice ? "Type to sign in, or start with make run-voice" : "Voice sign-in is not set up",
                   FS_DIM, FONT_UI);
    fs_tray();
}

static void lg_submit_typed(void)
{
    if (!lg.entry_len) return;
    if (auth_wait_seconds()) return;
    if (auth_check_typed(lg.entry)) { lg_unlock(); return; }
    lg.entry_len = 0; lg.entry[0] = 0;
    lg_set_message(lg.pin_mode ? "That PIN is incorrect." : "That password is incorrect.", T(DANGER));
}

static void lg_toggle_pin_mode(void)
{
    if (!(account_has_password() && account_has_pin())) return;
    lg.pin_mode = !lg.pin_mode;
    lg.entry_len = 0; lg.entry[0] = 0;
    lg_set_message(lg.pin_mode ? "Sign-in option: PIN" : "Sign-in option: password", FS_DIM);
}

static void login_lock_input(uint32_t key, char ch)
{
    if (user_count() > 1 && (key == 0x102 || key == 0x103)) {       /* Up / Down: another user */
        int n = user_count(), i = user_selected();
        user_select((i + (key == 0x103 ? 1 : n - 1)) % n);
        lock_reset_entry();
        return;
    }
    if (!account_has_sign_in()) {
        if (user_count() < 2 || key == 10) lg_unlock();
        return;
    }
    if (!account_has_password() && !account_has_pin()) return;     /* voice only */
    if (ch >= 32 && ch < 127) {
        if (lg.entry_len < (int)sizeof(lg.entry) - 1) { lg.entry[lg.entry_len++] = ch; lg.entry[lg.entry_len] = 0; }
        lg.message[0] = 0;
    } else if (key == 8) {
        if (lg.entry_len) lg.entry[--lg.entry_len] = 0;
    } else if (key == 10) {
        lg_submit_typed();
    } else if (key == 0x101) {                                     /* Esc */
        lg.entry_len = 0; lg.entry[0] = 0;
        g_fs.power_open = 0;
    } else if (key == 9) {                                         /* Tab */
        lg_toggle_pin_mode();
    }
}

static void login_lock_click(int x, int y)
{
    if (fs_tray_click(x, y)) return;
    int side = fs_sidebar_hit(x, y);
    if (side == 1) { g_fs.status_hidden = !g_fs.status_hidden; return; }
    if (side == 2) { lg_toggle_pin_mode(); return; }
    if (side) return;
    for (int i = 0; i < user_count() && user_count() > 1; i++) {
        lrect_t r;
        lock_user_rect(i, &r);
        if (lr_hit(&r, x, y)) {
            if (i != user_selected()) { user_select(i); lock_reset_entry(); }
            return;
        }
    }
    int cx, cy, cw, ch;
    lock_card_rect(&cx, &cy, &cw, &ch);
    if (!account_has_sign_in()) {
        if (user_count() < 2 || ui_hit(x, y, cx, cy, cw, ch)) lg_unlock();
        return;
    }
    int fx, fy, fw, fh;
    lock_field_rect(&fx, &fy, &fw, &fh);
    if (ui_hit(x, y, fx + fw - 46, fy, 46, fh)) lg_submit_typed();
    else if (ui_hit(x, y, fx + fw - 60, fy - 32, 60, 26)) lg.reveal = !lg.reveal;
}

/* ---- First-run setup: Kryzen Initialization Wizard ------------------------ */
static void wizard_rect(int *x, int *y, int *w, int *h)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    *w = W >= 1000 ? 808 : W - 150;
    *h = H >= 700 ? 588 : H - 100;
    *x = (W - *w) / 2 + (W < 1000 ? 22 : 0);
    *y = (H - *h) / 2 + 14;
}

/* Frame shared by the wizard and the installer pages. */
static void wizard_frame(const char *tab)
{
    int W = (int)fb_get_width();
    int x, y, w, h;
    char clock[16], date[48];
    draw_backdrop();
    fs_sidebar(0);
    fs_status_list(NULL, 1, 2);
    sys_format_clock(clock, sizeof(clock));
    sys_format_date_long(date, sizeof(date));
    fb_text(W - 22 - fb_text_width_font(clock, FONT_LARGE), 8, clock, FS_WHITE, FONT_LARGE);
    fb_text(W - 22 - fb_text_width_font(date, FONT_UI), 52, date, FS_DIM, FONT_UI);
    wizard_rect(&x, &y, &w, &h);
    fs_panel(x, y, w, h, 20);
    fs_panel_tab(x, y - 10, w, tab);
}

/* A profile exported from another Kryzen PC (reg export ... kryzen-profile.reg)
 * on any connected drive. */
static void lg_find_profile(void)
{
    static drive_info_t drives[12];
    lg.import_path[0] = 0;
    int n = storage_list(drives, 12);
    for (int i = 0; i < n; i++) {
        char path[96];
        if (!drives[i].mount[0]) continue;
        snprintf(path, sizeof(path), "%s/kryzen-profile.reg", drives[i].mount);
        if (vfs_exists(path) && !vfs_is_dir(path)) { snprintf(lg.import_path, sizeof(lg.import_path), "%s", path); return; }
    }
}

static void lg_import_profile(void)
{
    static char text[48 * 1024];
    char err[96] = "";
    if (!lg.import_path[0] || lg.import_done) return;
    int n = vfs_read_file(lg.import_path, text, sizeof(text) - 1);
    if (n <= 0) { snprintf(lg.error, sizeof(lg.error), "The profile file could not be read."); return; }
    text[n] = 0;
    if (reg_import(text, &lg.import_keys, &lg.import_values, err, sizeof(err)) != 0) {
        snprintf(lg.error, sizeof(lg.error), "Profile import failed: %s", err[0] ? err : "the file is not valid");
        return;
    }
    lg.import_done = 1;
    lg.theme_id = setting_get_int("theme", lg.theme_id);
    lg.clock24 = setting_get_int("clock_24h", lg.clock24);
    desktop_apply_settings();
}

static void lg_set_field(int f, const char *value)
{
    snprintf(lg.field[f], sizeof(lg.field[f]), "%s", value);
    lg.field_len[f] = (int)strlen(lg.field[f]);
}

static int lg_current_res_index(void)
{
    int w, h;
    for (int i = 0; i < fb_mode_count(); i++) {
        fb_mode_get(i, &w, &h);
        if (w == (int)fb_get_width() && h == (int)fb_get_height()) return i;
    }
    return -1;
}

static void login_on_show_setup(void)
{
    memset(&lg, 0, sizeof(lg));
    g_fs.power_open = 0;
    lg.page = SW_WELCOME;
    lg.focus = F_NAME;
    lg.opt_ai = 1;
    lg.host_nt = 1;
    lg.host_linux = 1;
    lg.theme_id = 0;
    lg.clock24 = setting_get_int("clock_24h", 1);
    lg.res_index = lg_current_res_index();
    lg_set_field(F_DEVICE, "Kryzen-PC");
    lg_find_profile();
}

/* welcome page geometry, shared by drawing and clicking */
static void wz_welcome_layout(lrect_t *card_ai, lrect_t *card_host, lrect_t *card_import,
                              lrect_t *name, lrect_t *go)
{
    int x, y, w, h;
    wizard_rect(&x, &y, &w, &h);
    int cx = x + w / 2 + 20, cw = x + w - 40 - cx;
    *card_ai     = (lrect_t){ cx, y + 172, cw, 52 };
    *card_host   = (lrect_t){ cx, y + 234, cw, 96 };
    *card_import = (lrect_t){ cx, y + 340, cw, 82 };
    int bw = 150, gap = 22;
    int fw = w - 2 * 110 - bw - gap;
    if (fw > 400) fw = 400;
    int fx = x + (w - (fw + gap + bw)) / 2;
    *name = (lrect_t){ fx, y + h - 112, fw, 44 };
    *go   = (lrect_t){ fx + fw + gap, y + h - 112, bw, 44 };
}

static void wz_nav_layout(lrect_t *back, lrect_t *next)
{
    int x, y, w, h;
    wizard_rect(&x, &y, &w, &h);
    *back = (lrect_t){ x + 40, y + h - 76, 120, 42 };
    *next = (lrect_t){ x + w - 40 - 190, y + h - 76, 190, 42 };
}

static void wz_form_field_rect(int row, lrect_t *r)
{
    int x, y, w, h;
    wizard_rect(&x, &y, &w, &h);
    int fw = w - 2 * 150;
    *r = (lrect_t){ x + 150, y + 176 + row * 96, fw, 44 };
}

static void wz_draw_error(int x, int y, int w)
{
    if (lg.error[0]) fs_paragraph(x, y, w, lg.error, T(DANGER), FONT_UI, 18);
}

static void wz_draw_welcome(int x, int y, int w, int h)
{
    lrect_t ai, host, imp, name, go;
    wz_welcome_layout(&ai, &host, &imp, &name, &go);
    fs_text_center(x, y + 56, w, "Welcome to Kryzen", FS_WHITE, FONT_HEADLINE);
    fs_text_center(x, y + 108, w, "Let's calibrate your Hybrid Workspace & AI Core", FS_DIM, FONT_SUBHEAD);

    /* the assistant orb: live microphone state */
    int bridge = kernel_voice_bridge_connected();
    int ocx = x + w / 4 + 16, ocy = y + 296, orad = h >= 560 ? 112 : 92;
    fs_big_orb(ocx, ocy, orad, bridge);
    fs_text_center(ocx - orad, ocy + orad / 4 - 2, orad * 2,
                   bridge ? "AI Core: Online" : g_kstate.ai_ready ? "AI Core: Ready" : "AI Core: Starting",
                   UI_NEON_CYAN, FONT_UI_BOLD);
    fs_text_center(ocx - orad, ocy + orad / 4 + 18, orad * 2,
                   bridge ? "Speak \"Proceed\" or" : "Voice bridge is off:", FS_DIM, FONT_UI);
    fs_text_center(ocx - orad, ocy + orad / 4 + 36, orad * 2,
                   bridge ? "calibrate below" : "type below", FS_DIM, FONT_UI);
    fb_blend_rect(x + w / 2 - 6, y + 176, 1, 236, 0x00FFFFFF, 34);

    /* 1. the assistant */
    fs_card(ai.x, ai.y, ai.w, ai.h, lg.opt_ai);
    fb_neon_rounded_rect(ai.x + 18, ai.y + 15, 22, 22, 4, 2, 0, UI_NEON_CYAN, UI_NEON_VIOLET, 255);
    if (lg.opt_ai) glyph_draw(GLYPH_CHECK, ai.x + 20, ai.y + 17, 18, UI_NEON_CYAN, 0);
    fb_text(ai.x + 56, ai.y + 8, "Initialize AI Assistant", FS_WHITE, FONT_MEDIUM_BOLD);
    fb_text(ai.x + 56, ai.y + 30, "JARVIS listens and opens when you sign in", FS_DIM, FONT_SMALL);

    /* 2. host environments */
    fs_card(host.x, host.y, host.w, host.h, 0);
    glyph_draw(GLYPH_DISPLAY, host.x + 16, host.y + 12, 24, 0x00B4BCF0, 0);
    fb_text(host.x + 56, host.y + 12, "Select Host Environments", FS_WHITE, FONT_MEDIUM_BOLD);
    fs_toggle(host.x + host.w - 56, host.y + 14, lg.host_nt || lg.host_linux);
    int third = (host.w - 24) / 3;
    static const char *const hl1[3] = { "Windows NT", "Debian", "Android" };
    static const char *const hl2[3] = { "shell, registry", "Linux, apt", "unavailable" };
    for (int i = 0; i < 3; i++) {
        int on = i == 0 ? lg.host_nt : i == 1 ? lg.host_linux : 0;
        int hx = host.x + 12 + i * third;
        if (on) fb_blend_rounded_rect(hx, host.y + 46, third - 6, 42, 8, 0x005CE6FF, 34);
        uint32_t c1 = i == 2 ? FS_FAINT : on ? FS_WHITE : FS_DIM;
        if (i == 0) {                               /* four panes */
            uint32_t pc = on ? 0x0038B6FF : 0x00607090;
            fb_fill_rect(hx + 8, host.y + 56, 9, 9, pc); fb_fill_rect(hx + 19, host.y + 56, 9, 9, pc);
            fb_fill_rect(hx + 8, host.y + 67, 9, 9, pc); fb_fill_rect(hx + 19, host.y + 67, 9, 9, pc);
        } else if (i == 1) {                        /* swirl */
            fb_draw_arc(hx + 18, host.y + 67, 10, 2, 300, 250, on ? 0x00E0406C : 0x00607090, 255);
            fb_draw_arc(hx + 20, host.y + 68, 5, 2, 120, 220, on ? 0x00E0406C : 0x00607090, 255);
        } else {                                    /* robot head */
            fb_draw_arc(hx + 18, host.y + 70, 9, 9, 180, 180, 0x00506080, 255);
            fb_fill_rect(hx + 9, host.y + 71, 19, 6, 0x00506080);
        }
        fb_text(hx + 34, host.y + 50, hl1[i], c1, FONT_SMALL);
        fs_text_fit(hx + 34, host.y + 66, third - 42, hl2[i], i == 2 ? FS_FAINT : FS_DIM, FONT_SMALL);
    }

    /* 3. profile import */
    fs_card(imp.x, imp.y, imp.w, imp.h, 0);
    glyph_draw(GLYPH_FOLDER_LINE, imp.x + 16, imp.y + 18, 24, 0x00B4BCF0, 0);
    fb_text(imp.x + 56, imp.y + 8, "Import User Profile &", FS_WHITE, FONT_MEDIUM_BOLD);
    fb_text(imp.x + 56, imp.y + 28, "System Preferences", FS_WHITE, FONT_MEDIUM_BOLD);
    char line[112];
    if (lg.import_done) snprintf(line, sizeof(line), "Imported %d keys, %d values", lg.import_keys, lg.import_values);
    else if (lg.import_path[0]) snprintf(line, sizeof(line), "Found %s: click to import", lg.import_path);
    else snprintf(line, sizeof(line), "No kryzen-profile.reg on a drive: click to rescan");
    fs_text_fit(imp.x + 56, imp.y + 56, imp.w - 70, line, lg.import_done ? T(OK) : lg.import_path[0] ? UI_NEON_CYAN : FS_DIM, FONT_SMALL);
    glyph_draw(GLYPH_PROFILE, imp.x + imp.w - 46, imp.y + 14, 30, lg.import_done ? T(OK) : 0x00B4BCF0, 0);

    fs_field(name.x, name.y, name.w, name.h, lg.field[F_NAME], lg.field_len[F_NAME], 0,
             bridge ? "Speak or type your name to start..." : "Type your name to start...", 1, -1);
    fs_primary(go.x, go.y, go.w, go.h, "Let's Go", 1, 1);
    wz_draw_error(name.x + 8, name.y + 54, w - (name.x - x) * 2);
}

static void wz_draw_security(int x, int y, int w, int h)
{
    static const char *const label[3] = { "Password", "PIN", "Spoken passphrase" };
    static const char *const hint[3] = {
        "Optional: sign in by typing it",
        "Optional: 4 to 12 digits for a quick sign-in",
        "Optional: three or more words you will say, e.g. blue falcon seven",
    };
    static const int field[3] = { F_PASS, F_PIN, F_VOICE };
    char sub[96];
    snprintf(sub, sizeof(sub), "Choose how %s signs in. Leave all three empty for no sign-in.", lg.field[F_NAME]);
    fs_text_center(x, y + 50, w, "Secure your account", FS_WHITE, FONT_HEADLINE);
    fs_text_center(x, y + 104, w, sub, FS_DIM, FONT_MEDIUM);
    for (int i = 0; i < 3; i++) {
        lrect_t r;
        wz_form_field_rect(i, &r);
        fb_text(r.x + 8, r.y - 28, label[i], FS_WHITE, FONT_MEDIUM_BOLD);
        fb_text(r.x + 16 + fb_text_width_font(label[i], FONT_MEDIUM_BOLD), r.y - 24, hint[i], FS_FAINT, FONT_SMALL);
        fs_field(r.x, r.y, r.w, r.h, lg.field[field[i]], lg.field_len[field[i]], i < 2 && !lg.reveal, "",
                 lg.focus == field[i], i == 0 ? GLYPH_PADLOCK : i == 1 ? GLYPH_KEYBOARD : GLYPH_MIC);
    }
    lrect_t back, next, r0;
    wz_form_field_rect(0, &r0);
    {   /* show / hide the password and PIN while typing them */
        const char *t = lg.reveal ? "Hide what I type" : "Show what I type";
        fb_text(r0.x + r0.w - 8 - fb_text_width_font(t, FONT_UI), r0.y - 54, t, UI_NEON_CYAN, FONT_UI);
    }
    wz_nav_layout(&back, &next);
    if (!lg.error[0] && input_caps_lock()) fb_text(x + 180, y + h - 64, "Caps Lock is on.", T(WARN), FONT_UI);
    wz_draw_error(x + 180, y + h - 68, w - 420);
    fs_secondary(back.x, back.y, back.w, back.h, "Back", 1);
    fs_primary(next.x, next.y, next.w, next.h, "Next", 1, 1);
}

/* device page: rows of chips */
static void wz_device_layout(lrect_t *dev, lrect_t theme[3], lrect_t *res_prev, lrect_t *res_next,
                             lrect_t *res_label, lrect_t clock[2])
{
    int x, y, w, h;
    wizard_rect(&x, &y, &w, &h);
    int lx = x + 150, fw = w - 300;
    *dev = (lrect_t){ lx, y + 168, fw, 44 };
    int cw = (fw - 20) / 3;
    for (int i = 0; i < 3; i++) theme[i] = (lrect_t){ lx + i * (cw + 10), y + 256, cw, 36 };
    *res_prev  = (lrect_t){ lx, y + 336, 44, 36 };
    *res_label = (lrect_t){ lx + 54, y + 336, fw - 108, 36 };
    *res_next  = (lrect_t){ lx + fw - 44, y + 336, 44, 36 };
    int hw = (fw - 10) / 2;
    for (int i = 0; i < 2; i++) clock[i] = (lrect_t){ lx + i * (hw + 10), y + 416, hw, 36 };
}

static void wz_draw_device(int x, int y, int w, int h)
{
    static const char *const themes[3] = { "Dark", "Light", "High contrast" };
    lrect_t dev, theme[3], rp, rn, rl, clock[2], back, next;
    wz_device_layout(&dev, theme, &rp, &rn, &rl, clock);
    fs_text_center(x, y + 50, w, "Make this device yours", FS_WHITE, FONT_HEADLINE);
    fs_text_center(x, y + 104, w, "You can change all of this later in Settings.", FS_DIM, FONT_MEDIUM);

    fb_text(dev.x + 8, dev.y - 28, "Device name", FS_WHITE, FONT_MEDIUM_BOLD);
    fs_field(dev.x, dev.y, dev.w, dev.h, lg.field[F_DEVICE], lg.field_len[F_DEVICE], 0, "Kryzen-PC", lg.focus == F_DEVICE, GLYPH_DISPLAY);

    fb_text(dev.x + 8, theme[0].y - 28, "Colour theme", FS_WHITE, FONT_MEDIUM_BOLD);
    for (int i = 0; i < 3; i++) fs_chip(theme[i].x, theme[i].y, theme[i].w, theme[i].h, themes[i], lg.theme_id == i);

    fb_text(dev.x + 8, rp.y - 28, "Display resolution", FS_WHITE, FONT_MEDIUM_BOLD);
    char res[40];
    int rw = (int)fb_get_width(), rh = (int)fb_get_height();
    int modes = fb_mode_count();
    if (lg.res_index >= 0 && lg.res_index < modes) fb_mode_get(lg.res_index, &rw, &rh);
    snprintf(res, sizeof(res), "%d x %d%s", rw, rh,
             rw == (int)fb_get_width() && rh == (int)fb_get_height() ? "  (current)" : "");
    fs_chip(rl.x, rl.y, rl.w, rl.h, modes > 0 ? res : "This display has one fixed mode", 0);
    fs_secondary(rp.x, rp.y, rp.w, rp.h, "", modes > 1);
    fs_secondary(rn.x, rn.y, rn.w, rn.h, "", modes > 1);
    glyph_draw(GLYPH_CHEVRON_LEFT, rp.x + 12, rp.y + 8, 20, modes > 1 ? FS_WHITE : FS_FAINT, 0);
    glyph_draw(GLYPH_CHEVRON_RIGHT, rn.x + 12, rn.y + 8, 20, modes > 1 ? FS_WHITE : FS_FAINT, 0);

    fb_text(dev.x + 8, clock[0].y - 28, "Clock format", FS_WHITE, FONT_MEDIUM_BOLD);
    fs_chip(clock[0].x, clock[0].y, clock[0].w, clock[0].h, "24-hour  (14:05)", lg.clock24);
    fs_chip(clock[1].x, clock[1].y, clock[1].w, clock[1].h, "12-hour  (2:05 PM)", !lg.clock24);

    wz_nav_layout(&back, &next);
    wz_draw_error(x + 180, y + h - 68, w - 420);
    fs_secondary(back.x, back.y, back.w, back.h, "Back", !lg.account_created);
    fs_primary(next.x, next.y, next.w, next.h, lg.account_created ? "Retry save" : "Finish setup", 1, !lg.account_created);
}

static void login_draw_setup(void)
{
    int x, y, w, h;
    wizard_frame("Kryzen Initialization Wizard");
    wizard_rect(&x, &y, &w, &h);
    if (lg.page == SW_WELCOME) wz_draw_welcome(x, y, w, h);
    else if (lg.page == SW_SECURITY) wz_draw_security(x, y, w, h);
    else wz_draw_device(x, y, w, h);
    /* page dots */
    for (int i = 0; i < 3; i++)
        fb_fill_circle(x + w / 2 - 16 + i * 16, y + h - 22, i == lg.page ? 4 : 3, i == lg.page ? UI_NEON_CYAN : 0x00606A8C);
    fs_tray();
}

static void lg_finish_installer_setup(void)
{
    if (reg_flush_checked() != 0 || diskfs_sync_verified() != 0) {
        snprintf(lg.error, sizeof(lg.error),
                 "The account is ready, but its settings did not verify on disk. Keep power on and retry the save.");
        return;
    }
    ui_shell_show_installer_complete();
}

static void lg_create(void)
{
    if (kernel_installer_boot_mode() && lg.account_created) {
        lg_finish_installer_setup();
        return;
    }
    char err[96], msg[128], res[24];
    int rw = (int)fb_get_width(), rh = (int)fb_get_height();
    if (lg.res_index >= 0 && lg.res_index < fb_mode_count()) fb_mode_get(lg.res_index, &rw, &rh);
    if (rw != (int)fb_get_width() || rh != (int)fb_get_height()) {
        snprintf(res, sizeof(res), "%dx%d", rw, rh);
        if (prefs_set("resolution", res, msg, sizeof(msg)) != 0) {
            snprintf(lg.error, sizeof(lg.error), "%s", msg[0] ? msg : "That display resolution could not be set.");
            return;
        }
    }
    if (account_create(lg.field[F_NAME], lg.field[F_PASS], lg.field[F_PIN], lg.field[F_VOICE], err, sizeof(err)) != 0) {
        snprintf(lg.error, sizeof(lg.error), "%s", err);
        /* the problem is with the name or a secret: go to the page that has it */
        lg.page = lg.field[F_NAME][0] ? SW_SECURITY : SW_WELCOME;
        lg.focus = lg.field[F_NAME][0] ? F_PASS : F_NAME;
        return;
    }
    setting_set("device_name", lg.field[F_DEVICE][0] ? lg.field[F_DEVICE] : "Kryzen-PC");
    setting_set_int("theme", lg.theme_id);
    setting_set_int("clock_24h", lg.clock24);
    setting_set_int("voice_input", lg.opt_ai);
    setting_set_int("startup_JARVIS", lg.opt_ai);
    setting_set_int("host_nt", lg.host_nt);
    setting_set_int("host_linux", lg.host_linux);
    desktop_apply_settings();
    if (kernel_installer_boot_mode()) {
        lg.account_created = 1;
        lg_finish_installer_setup();
        return;
    }
    ui_shell_unlock_desktop();
    char hello[80];
    snprintf(hello, sizeof(hello), "Welcome, %s. Your account is ready.", account_name());
    notification_show("Account created", hello, NOTIF_INFO, 6000);
    tts_speak(hello, strlen(hello));
}

static void wz_next(void)
{
    lg.error[0] = 0;
    if (lg.page == SW_WELCOME) {
        if (!lg.field_len[F_NAME]) { snprintf(lg.error, sizeof(lg.error), "Please type your name to start."); return; }
        lg.page = SW_SECURITY; lg.focus = F_PASS;
    } else if (lg.page == SW_SECURITY) {
        if (lg.field_len[F_PIN] && (lg.field_len[F_PIN] < 4 || lg.field_len[F_PIN] > 12)) {
            snprintf(lg.error, sizeof(lg.error), "A PIN needs 4 to 12 digits.");
            lg.focus = F_PIN;
            return;
        }
        lg.page = SW_DEVICE; lg.focus = F_DEVICE;
    } else {
        lg_create();
    }
}

static void wz_back(void)
{
    lg.error[0] = 0;
    if (lg.account_created) return;
    if (lg.page == SW_SECURITY) { lg.page = SW_WELCOME; lg.focus = F_NAME; }
    else if (lg.page == SW_DEVICE) { lg.page = SW_SECURITY; lg.focus = F_PASS; }
}

static void wz_step_resolution(int dir)
{
    int modes = fb_mode_count();
    if (modes < 2) return;
    int i = lg.res_index < 0 ? 0 : lg.res_index;
    lg.res_index = (i + dir + modes) % modes;
}

static void login_setup_input(uint32_t key, char ch)
{
    if (kernel_installer_boot_mode() && lg.account_created) {
        if (key == 10) lg_finish_installer_setup();
        return;
    }
    int f = lg.focus;
    if (ch >= 32 && ch < 127) {
        if (f == F_PIN && (ch < '0' || ch > '9')) return;
        if (f == F_PIN && lg.field_len[f] >= 12) return;
        if (f == F_NAME && lg.field_len[f] >= 30) return;
        if (lg.field_len[f] < 63) { lg.field[f][lg.field_len[f]++] = ch; lg.field[f][lg.field_len[f]] = 0; }
        lg.error[0] = 0;
    } else if (key == 8) {
        if (lg.field_len[f]) lg.field[f][--lg.field_len[f]] = 0;
    } else if (key == 0x101) {                                  /* Esc */
        wz_back();
    } else if (lg.page == SW_SECURITY && (key == 9 || key == 0x103)) {      /* Tab / Down */
        lg.focus = f == F_PASS ? F_PIN : f == F_PIN ? F_VOICE : F_PASS;
    } else if (lg.page == SW_SECURITY && key == 0x102) {                    /* Up */
        lg.focus = f == F_VOICE ? F_PIN : f == F_PIN ? F_PASS : F_VOICE;
    } else if (lg.page == SW_SECURITY && key == 10 && f != F_VOICE) {
        lg.focus = f == F_PASS ? F_PIN : F_VOICE;
    } else if (lg.page == SW_DEVICE && key == 0x104) {                      /* Left */
        wz_step_resolution(-1);
    } else if (lg.page == SW_DEVICE && key == 0x105) {                      /* Right */
        wz_step_resolution(1);
    } else if (key == 10) {
        wz_next();
    }
}


static void login_setup_click(int mx, int my)
{
    if (fs_tray_click(mx, my)) return;
    if (fs_sidebar_hit(mx, my) == 1) { g_fs.status_hidden = !g_fs.status_hidden; return; }
    if (kernel_installer_boot_mode() && lg.account_created) {
        lrect_t back, next;
        wz_nav_layout(&back, &next);
        if (lr_hit(&next, mx, my)) lg_finish_installer_setup();
        return;
    }
    if (lg.page == SW_WELCOME) {
        lrect_t ai, host, imp, name, go;
        wz_welcome_layout(&ai, &host, &imp, &name, &go);
        if (lr_hit(&ai, mx, my)) lg.opt_ai = !lg.opt_ai;
        else if (lr_hit(&host, mx, my)) {
            int third = (host.w - 24) / 3;
            if (my < host.y + 44) {                 /* master switch */
                int on = !(lg.host_nt || lg.host_linux);
                lg.host_nt = lg.host_linux = on;
            } else if (mx < host.x + 12 + third) lg.host_nt = !lg.host_nt;
            else if (mx < host.x + 12 + 2 * third) lg.host_linux = !lg.host_linux;
        }
        else if (lr_hit(&imp, mx, my)) {
            lg.error[0] = 0;
            if (!lg.import_path[0]) lg_find_profile();
            else lg_import_profile();
        }
        else if (lr_hit(&go, mx, my)) wz_next();
    } else if (lg.page == SW_SECURITY) {
        static const int field[3] = { F_PASS, F_PIN, F_VOICE };
        lrect_t back, next, r;
        wz_nav_layout(&back, &next);
        for (int i = 0; i < 3; i++) { wz_form_field_rect(i, &r); if (lr_hit(&r, mx, my)) lg.focus = field[i]; }
        wz_form_field_rect(0, &r);
        if (ui_hit(mx, my, r.x + r.w - 150, r.y - 60, 150, 26)) lg.reveal = !lg.reveal;
        if (lr_hit(&back, mx, my)) wz_back();
        else if (lr_hit(&next, mx, my)) wz_next();
    } else {
        lrect_t dev, theme[3], rp, rn, rl, clock[2], back, next;
        wz_device_layout(&dev, theme, &rp, &rn, &rl, clock);
        wz_nav_layout(&back, &next);
        for (int i = 0; i < 3; i++)
            if (lr_hit(&theme[i], mx, my)) { lg.theme_id = i; theme_set_mode((theme_mode_t)i); }
        if (lr_hit(&rp, mx, my)) wz_step_resolution(-1);
        else if (lr_hit(&rn, mx, my)) wz_step_resolution(1);
        else if (lr_hit(&clock[0], mx, my)) lg.clock24 = 1;
        else if (lr_hit(&clock[1], mx, my)) lg.clock24 = 0;
        else if (lr_hit(&back, mx, my)) wz_back();
        else if (lr_hit(&next, mx, my)) wz_next();
    }
}

/* Speech during setup: "proceed" moves on; on the first page anything else
 * short enough is taken as the name ("my name is ..." is understood). */
static void login_setup_voice(const char *text)
{
    char low[96];
    size_t n = 0;
    for (; text[n] && n < sizeof(low) - 1; n++)
        low[n] = text[n] >= 'A' && text[n] <= 'Z' ? (char)(text[n] + 32) : text[n];
    low[n] = 0;
    while (n && (low[n - 1] == '.' || low[n - 1] == '!' || low[n - 1] == ' ')) low[--n] = 0;
    if (!strcmp(low, "proceed") || !strcmp(low, "continue") || !strcmp(low, "next") || !strcmp(low, "let's go")) {
        wz_next();
        return;
    }
    if (lg.page != SW_WELCOME || lg.account_created) return;
    const char *name = text;
    static const char *const lead[] = { "my name is ", "i am ", "i'm ", "call me " };
    for (int i = 0; i < 4; i++)
        if (!strncmp(low, lead[i], strlen(lead[i]))) name = text + strlen(lead[i]);
    size_t l = strlen(name);
    while (l && (name[l - 1] == '.' || name[l - 1] == '!' || name[l - 1] == ' ')) l--;
    if (l == 0 || l > 30) return;
    memcpy(lg.field[F_NAME], name, l);
    lg.field[F_NAME][l] = 0;
    lg.field_len[F_NAME] = (int)l;
    lg.error[0] = 0;
}

/* A transcript arrived while signed out. On the lock screen it is never shown
 * or logged: it may be the passphrase. */
void ui_shell_voice_attempt(const char *text)
{
    if (g_ui.mode == UI_MODE_SETUP) { login_setup_voice(text); desktop_invalidate(); return; }
    if (g_ui.mode != UI_MODE_LOCK) return;
    if (!account_has_sign_in()) { lg_unlock(); return; }
    if (!account_has_voice()) {
        lg_set_message("Voice sign-in is not set up. Type your PIN or password.", T(WARN));
        return;
    }
    int r = auth_check_voice(text);
    if (r > 0) { lg_unlock(); return; }
    if (r == -2) return;                 /* a fragment or noise, not an attempt: stay quiet */
    const char *msg = r < 0 ? "Voice sign-in is paused after several misses. Please type your PIN or password."
                            : (account_has_password() || account_has_pin())
                              ? "That didn't match. Please say it again, or type your PIN."
                              : "That didn't match. Please say it again.";
    lg_set_message(msg, r < 0 ? T(WARN) : T(DANGER));
    tts_speak(msg, strlen(msg));
    desktop_invalidate();
}
