/*
 * =============================================================================
 * JARVIS OS — Sign-in and first-run screens (desktop/login.c)
 * Language: C17
 *
 * First run: "Set up JARVIS OS" asks for a name and, optionally, a password,
 * a PIN and a spoken passphrase, then creates the account (kernel/accounts.c).
 *
 * Sign-in (after boot and whenever the PC is locked): type the password or
 * PIN, or just speak the passphrase: JARVIS asks for it out loud. Spoken
 * attempts never count towards the typed-attempt slowdown, so mishearing
 * cannot lock you out; typing always works.
 *
 * Included by desktop/ui_shell.c.
 * =============================================================================
 */

#define LG_FIELDS 4
enum { F_NAME, F_PASS, F_PIN, F_VOICE };

static struct {
    /* sign-in */
    char entry[64]; int entry_len;
    int  pin_mode;                  /* 1: the field takes a PIN, 0: password */
    char message[96]; uint32_t message_color;
    int  greeted;                   /* JARVIS asked for the passphrase */
    /* setup */
    char field[LG_FIELDS][64]; int field_len[LG_FIELDS];
    int  focus;
    char error[96];
} lg;

static const char *const g_field_label[LG_FIELDS] = {
    "Your name", "Password (optional)", "PIN, 4-12 digits (optional)", "Spoken passphrase (optional)",
};
static const char *const g_field_hint[LG_FIELDS] = {
    "", "Used to sign in by typing", "Quick sign-in by typing digits",
    "Three or more words you will say, e.g. \"blue falcon seven\"",
};

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

static void login_on_show_lock(void)
{
    lg.entry_len = 0; lg.entry[0] = 0;
    lg.message[0] = 0;
    lg.pin_mode = account_has_pin() && !account_has_password();
    lg.greeted = 0;
    if (account_has_voice() && kernel_voice_bridge_connected()) {
        lg_ask_passphrase();
    }
}

static void lg_field_box(int x, int y, int w, const char *text, int len, int masked, const char *placeholder, int focused)
{
    fb_fill_rounded_rect(x, y, w, 36, 6, 0x00101820);
    fb_draw_rounded_rect(x, y, w, 36, 6, focused ? T(ACCENT) : 0x00405060);
    if (len) {
        char shown[64];
        int n = len < 63 ? len : 63;
        for (int i = 0; i < n; i++) shown[i] = masked ? '*' : text[i];
        shown[n] = 0;
        ui_text_fit(x + 12, y + 10, w - 24, shown, 0x00FFFFFF);
        if (focused && 13 + n * 8 < w) fb_fill_rect(x + 13 + n * 8, y + 10, 1, 16, 0x00FFFFFF);
    } else {
        ui_text_fit(x + 12, y + 10, w - 24, placeholder, 0x008090A0);
        if (focused) fb_fill_rect(x + 12, y + 10, 1, 16, 0x00FFFFFF);
    }
}

static void login_lock_geometry(int *fx, int *fy, int *fw)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    *fw = 300;
    *fx = (W - *fw) / 2 - 22;
    *fy = H / 2 + 70;
}

static void login_draw_lock(void)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    char clock[12], date[32];
    draw_backdrop();
    sys_format_clock(clock, sizeof(clock));
    sys_format_date(date, sizeof(date));
    fb_draw_text_scaled((W - fb_text_width(clock) * 4) / 2, H / 2 - 230, clock, 0x00FFFFFF, 4);
    ui_text_center(0, H / 2 - 160, W, date, 0x00C0D0E0);

    int cy = H / 2 - 40;
    fb_fill_circle(W / 2, cy, 40, T(ACCENT));
    glyph_draw(GLYPH_USER, W / 2 - 20, cy - 21, 40, T(ACCENT_TEXT), T(ACCENT));
    fb_draw_text_scaled((W - fb_text_width(account_name()) * 2) / 2, cy + 54, account_name(), 0x00FFFFFF, 2);

    if (!account_has_sign_in()) {
        ui_text_center(0, cy + 100, W, "Press any key or click to sign in", 0x00C0D0E0);
        return;
    }

    int fx, fy, fw;
    login_lock_geometry(&fx, &fy, &fw);
    int typed = account_has_password() || account_has_pin();
    if (typed) {
        lg_field_box(fx, fy, fw, lg.entry, lg.entry_len, 1, lg.pin_mode ? "PIN" : "Password", 1);
        fb_fill_rounded_rect(fx + fw + 8, fy, 36, 36, 6, T(ACCENT));
        fb_draw_text(fx + fw + 22, fy + 10, ">", T(ACCENT_TEXT));
        if (account_has_password() && account_has_pin())
            ui_text_center(0, fy + 50, W, lg.pin_mode ? "Sign-in options: use password instead" : "Sign-in options: use PIN instead", 0x0090B0D0);
    }

    int vy = fy + (typed ? 78 : 0);
    if (account_has_voice()) {
        int bridge = kernel_voice_bridge_connected();
        uint32_t col = bridge ? T(ACCENT) : 0x00708090;
        int tw = fb_text_width("Say your passphrase");
        int mx = (W - tw - 30) / 2;
        glyph_draw(GLYPH_MIC, mx, vy, 18, col, 0x00000000);
        fb_draw_text(mx + 26, vy + 1, bridge ? "Say your passphrase" : "Voice sign-in needs the microphone bridge", col);
        if (bridge) ui_progress((W - 160) / 2, vy + 26, 160, 4, ui_shell_get_mic_level(), 0x00203040, col);
    }

    int wait = auth_wait_seconds();
    if (wait) {
        char m[64];
        snprintf(m, sizeof(m), "Too many wrong attempts. Try again in %d s.", wait);
        ui_text_center(0, vy + 46, W, m, T(WARN));
    } else if (lg.message[0]) {
        ui_text_center(0, vy + 46, W, lg.message, lg.message_color);
    }
}

static void lg_submit_typed(void)
{
    if (!lg.entry_len) return;
    if (auth_wait_seconds()) return;
    if (auth_check_typed(lg.entry)) { lg_unlock(); return; }
    lg.entry_len = 0; lg.entry[0] = 0;
    lg_set_message(lg.pin_mode ? "That PIN is incorrect." : "That password is incorrect.", T(DANGER));
}

static void login_lock_input(uint32_t key, char ch)
{
    if (!account_has_sign_in()) { lg_unlock(); return; }
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
    }
}

static void login_lock_click(int x, int y)
{
    int W = (int)fb_get_width();
    if (!account_has_sign_in()) { lg_unlock(); return; }
    int fx, fy, fw;
    login_lock_geometry(&fx, &fy, &fw);
    if (ui_hit(x, y, fx + fw + 8, fy, 36, 36)) { lg_submit_typed(); return; }
    if (account_has_password() && account_has_pin() && ui_hit(x, y, W / 2 - 200, fy + 46, 400, 22)) {
        lg.pin_mode = !lg.pin_mode;
        lg.entry_len = 0; lg.entry[0] = 0; lg.message[0] = 0;
    }
}

/* A transcript arrived while locked. Never shown or logged: it may be the
 * passphrase. */
void ui_shell_voice_attempt(const char *text)
{
    if (g_ui.mode != UI_MODE_LOCK) return;
    if (!account_has_sign_in()) { lg_unlock(); return; }
    if (!account_has_voice()) {
        lg_set_message("Voice sign-in is not set up. Type your PIN or password.", T(WARN));
        return;
    }
    int r = auth_check_voice(text);
    if (r > 0) { lg_unlock(); return; }
    const char *msg = r < 0 ? "Voice sign-in is paused after several misses. Please type your PIN or password."
                            : (account_has_password() || account_has_pin())
                              ? "That didn't match. Please say it again, or type your PIN."
                              : "That didn't match. Please say it again.";
    lg_set_message(msg, r < 0 ? T(WARN) : T(DANGER));
    tts_speak(msg, strlen(msg));
    desktop_invalidate();
}

/* ---- First-run setup ----------------------------------------------------- */
static void login_on_show_setup(void)
{
    memset(&lg, 0, sizeof(lg));
}

static void setup_geometry(int *x, int *y, int *w, int *h)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    *w = 560; *h = 470;
    *x = (W - *w) / 2; *y = (H - *h) / 2;
}

static void login_draw_setup(void)
{
    int x, y, w, h;
    draw_backdrop();
    setup_geometry(&x, &y, &w, &h);
    fb_blend_rounded_rect(x + 2, y + 6, w, h, 10, 0x00000000, 80);
    fb_fill_rounded_rect(x, y, w, h, 10, T(MENU));
    icon_draw(ICON_JARVIS, x + 28, y + 24, 40);
    fb_draw_text_scaled(x + 84, y + 26, "Set up JARVIS OS", T(TEXT), 2);
    fb_draw_text(x + 84, y + 60, "Create your account. You can change all of this in Settings.", T(TEXT_DIM));

    for (int i = 0; i < LG_FIELDS; i++) {
        int fy = y + 96 + i * 76;
        fb_draw_text(x + 28, fy, g_field_label[i], T(TEXT));
        fb_fill_rounded_rect(x + 28, fy + 20, w - 56, 32, 5, T(INPUT));
        fb_draw_rounded_rect(x + 28, fy + 20, w - 56, 32, 5, lg.focus == i ? T(ACCENT) : T(BORDER));
        int masked = i == F_PASS || i == F_PIN;
        char shown[64];
        int n = lg.field_len[i];
        for (int k = 0; k < n; k++) shown[k] = masked ? '*' : lg.field[i][k];
        shown[n] = 0;
        if (n) ui_text_fit(x + 38, fy + 28, w - 76, shown, T(TEXT));
        else if (g_field_hint[i][0]) ui_text_fit(x + 38, fy + 28, w - 76, g_field_hint[i], T(TEXT_DIM));
        if (lg.focus == i) fb_fill_rect(x + 39 + n * 8, fy + 28, 1, 16, T(TEXT));
    }
    if (lg.error[0]) fb_draw_text(x + 28, y + h - 76, lg.error, T(DANGER));
    fb_draw_text(x + 28, y + h - 48, "Tab: next field", T(TEXT_DIM));
    ui_button(x + w - 200, y + h - 56, 172, 34, "Create account", T(ACCENT), T(ACCENT_TEXT));
}

static void lg_create(void)
{
    char err[96];
    if (account_create(lg.field[F_NAME], lg.field[F_PASS], lg.field[F_PIN], lg.field[F_VOICE], err, sizeof(err)) != 0) {
        snprintf(lg.error, sizeof(lg.error), "%s", err);
        return;
    }
    ui_shell_unlock_desktop();
    char hello[80];
    snprintf(hello, sizeof(hello), "Welcome, %s. Your account is ready.", account_name());
    notification_show("Account created", hello, NOTIF_INFO, 6000);
    tts_speak(hello, strlen(hello));
}

static void login_setup_input(uint32_t key, char ch)
{
    int f = lg.focus;
    if (ch >= 32 && ch < 127) {
        if (f == F_PIN && (ch < '0' || ch > '9')) return;
        if (lg.field_len[f] < 63) { lg.field[f][lg.field_len[f]++] = ch; lg.field[f][lg.field_len[f]] = 0; }
        lg.error[0] = 0;
    } else if (key == 8) {
        if (lg.field_len[f]) lg.field[f][--lg.field_len[f]] = 0;
    } else if (key == 9 || key == 0x103) {                 /* Tab / Down */
        lg.focus = (f + 1) % LG_FIELDS;
    } else if (key == 0x102) {                              /* Up */
        lg.focus = (f + LG_FIELDS - 1) % LG_FIELDS;
    } else if (key == 10) {
        if (f < LG_FIELDS - 1) lg.focus = f + 1; else lg_create();
    }
}

static void login_setup_click(int mx, int my)
{
    int x, y, w, h;
    setup_geometry(&x, &y, &w, &h);
    for (int i = 0; i < LG_FIELDS; i++)
        if (ui_hit(mx, my, x + 28, y + 96 + i * 76 + 20, w - 56, 32)) lg.focus = i;
    if (ui_hit(mx, my, x + w - 200, y + h - 56, 172, 34)) lg_create();
}
