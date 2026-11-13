/*
 * =============================================================================
 * JARVIS OS — Settings (apps/settings.c)
 * Language: C17
 *
 * Categories: Home, System, Bluetooth & devices, Network & internet,
 * Personalization, Apps, Accounts, Time & language, Accessibility,
 * Privacy & security, Update.
 *
 * Every switch changes something real; hardware or features JARVIS OS does
 * not support yet are listed as such instead of offering a switch that does
 * nothing. Choices are saved in /jarvis/system/settings.conf and applied at
 * boot (desktop_apply_settings).
 *
 * A page is a list of rows (s_header, s_info, switch, s_choice, s_slider, text
 * s_field, s_button, s_drive bar, s_note), built each frame and scrolled as one.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

void desktop_apply_settings(void);
void desktop_send_host_prefs(void);
int  file_explorer_empty_trash(void);
int  kernel_voice_bridge_connected(void);

#define SET_SIDEBAR_W 230
#define SET_TITLE_H   64
#define SET_MAX_ROWS  64

enum {
    P_HOME, P_SYSTEM, P_DEVICES, P_NETWORK, P_PERSONAL, P_APPS, P_ACCOUNTS, P_TIME,
    P_ACCESS, P_PRIVACY, P_UPDATE, P_COUNT
};
static const struct { const char *name; icon_id_t icon; } s_g_pages[P_COUNT] = {
    { "Home", ICON_PC }, { "System", ICON_PC }, { "Bluetooth & devices", ICON_USB },
    { "Network & internet", ICON_CHART }, { "Personalization", ICON_GEAR }, { "Apps", ICON_STORE },
    { "Accounts", ICON_JARVIS }, { "Time & language", ICON_CALC }, { "Accessibility", ICON_JARVIS },
    { "Privacy & security", ICON_SHIELD }, { "Update", ICON_DRIVE },
};

enum { R_HEADER, R_INFO, R_TOGGLE, R_CHOICE, R_SLIDER, R_FIELD, R_BUTTON, R_DRIVE, R_NOTE };

typedef struct {
    int type, id, value, danger, masked, enabled;
    char label[64], text[128];
    const char *const *opts; int nopts;
    int y, h;                                   /* layout, content coordinates */
} s_row_t;

/* action ids */
enum {
    A_NONE, A_THEME, A_ACCENT, A_WALL, A_CLOCK24, A_DND, A_NARRATOR, A_VOICE_IN, A_AI_ASK, A_AI_WEB,
    A_VOLUME, A_VOICE_VOL, A_MOUSE_SPEED, A_MOUSE_SWAP, A_AUTOPLAY, A_AIRPLANE, A_RENEW, A_LOCK_AFTER,
    A_LOCK_NOW, A_RESTART, A_SHUTDOWN, A_EMPTY_TRASH, A_CLEAR_CLIP, A_CLEAR_HISTORY, A_DEVICE_NAME,
    A_NAME, A_PASSWORD, A_PASSWORD_DEL, A_PIN, A_PIN_DEL, A_VOICE, A_VOICE_DEL, A_HC, A_UNINSTALL0,
    A_STARTUP0 = 200,
};

static struct {
    wm_window_t *window;
    int page;
    int scroll;
    s_row_t rows[SET_MAX_ROWS]; int nrows;
    int content_h;
    int editing;                                /* row index of the s_field being typed in, -1 */
    char edit[96]; int edit_len;
    int verified;                               /* current password/PIN confirmed this session */
    int verify_open, verify_action;
    char verify_buf[64]; int verify_len;
    char message[96]; uint32_t message_color;
    pkg_info_t pkgs[16]; int npkgs;
} sg;

static const char *const OPT_THEME[] = { "Dark", "Light", "High contrast" };
static const char *const OPT_ACCENT[] = { "Default", "Blue", "Red", "Green", "Gold", "Purple", "Orange" };
static const char *const OPT_WALL[] = { "Theme", "Purple", "Forest", "Sunset", "Graphite", "Ocean" };
static const char *const OPT_SPEED[] = { "Slow", "Normal", "Fast", "Fastest" };
static const char *const OPT_LOCK[] = { "Never", "1 min", "5 min", "15 min", "30 min" };
static const int LOCK_MIN[] = { 0, 1, 5, 15, 30 };

/* ==========================================================================
 * Row builders
 * ========================================================================== */
static s_row_t *s_add(int type, const char *label)
{
    if (sg.nrows >= SET_MAX_ROWS) return &sg.rows[SET_MAX_ROWS - 1];
    s_row_t *r = &sg.rows[sg.nrows++];
    memset(r, 0, sizeof(*r));
    r->type = type;
    r->enabled = 1;
    snprintf(r->label, sizeof(r->label), "%s", label ? label : "");
    return r;
}
static void s_header(const char *t)                  { s_add(R_HEADER, t); }
static void s_note(const char *t)                    { snprintf(s_add(R_NOTE, "")->text, 128, "%s", t); }
static void s_info(const char *l, const char *v)     { snprintf(s_add(R_INFO, l)->text, 128, "%s", v); }
static void s_toggle(const char *l, const char *d, int on, int id)
{
    s_row_t *r = s_add(R_TOGGLE, l); r->value = on; r->id = id; snprintf(r->text, 128, "%s", d ? d : "");
}
static void s_choice(const char *l, const char *const *opts, int n, int cur, int id)
{
    s_row_t *r = s_add(R_CHOICE, l); r->opts = opts; r->nopts = n; r->value = cur; r->id = id;
}
static void s_slider(const char *l, int v, int id)  { s_row_t *r = s_add(R_SLIDER, l); r->value = v; r->id = id; }
static void s_button(const char *l, const char *d, const char *btn, int id, int danger)
{
    s_row_t *r = s_add(R_BUTTON, l); r->id = id; r->danger = danger;
    snprintf(r->text, 128, "%s", d ? d : "");
    r->opts = NULL; r->nopts = 0;
    snprintf(r->label + strlen(r->label), sizeof(r->label) - strlen(r->label), "%c%s", 1, btn);  /* label \1 s_button */
}
static void s_field(const char *l, const char *value, int masked, int id)
{
    s_row_t *r = s_add(R_FIELD, l); r->masked = masked; r->id = id; snprintf(r->text, 128, "%s", value ? value : "");
}
static void s_drive(const char *l, uint64_t used, uint64_t total, const char *detail)
{
    s_row_t *r = s_add(R_DRIVE, l);
    r->value = total ? (int)(used * 100 / total) : 0;
    snprintf(r->text, 128, "%s", detail);
}

static void s_fmt_size(uint64_t b, char *o, size_t n)
{
    if (b >= (1ULL << 30)) snprintf(o, n, "%u.%u GB", (unsigned)(b >> 30), (unsigned)(((b & 0x3FFFFFFF) * 10) >> 30));
    else if (b >= (1ULL << 20)) snprintf(o, n, "%u MB", (unsigned)(b >> 20));
    else snprintf(o, n, "%u KB", (unsigned)((b + 1023) >> 10));
}

static int s_has_device(const char *type, const char *name_part)
{
    static dev_info_t d[48];
    int n = devmgr_list(d, 48);
    for (int i = 0; i < n; i++)
        if ((!type || !strcmp(d[i].type, type)) && (!name_part || strstr(d[i].name, name_part))) return 1;
    return 0;
}

static int s_lock_index(void)
{
    int m = setting_get_int("lock_after_min", 0);
    for (int i = 0; i < 5; i++) if (LOCK_MIN[i] == m) return i;
    return 0;
}

/* ==========================================================================
 * Pages
 * ========================================================================== */
static void s_page_home(void)
{
    char a[64], b[64], c[96];
    drive_info_t dr[8];
    net_info_t ni;
    int nd = storage_list(dr, 8);
    net_get_info(&ni);

    s_header("Your account");
    snprintf(a, sizeof(a), "%s%s%s", account_has_password() ? "Password  " : "", account_has_pin() ? "PIN  " : "",
             account_has_voice() ? "Voice passphrase" : "");
    s_info(account_name(), account_has_sign_in() ? a : "No sign-in set (anyone can use this PC)");
    s_header("This PC");
    setting_get("device_name", b, sizeof(b));
    s_info("Device name", b[0] ? b : "JARVIS-PC");
    if (nd) {
        s_fmt_size(dr[0].free, a, sizeof(a)); s_fmt_size(dr[0].total, b, sizeof(b));
        snprintf(c, sizeof(c), "%s free of %s", a, b);
        s_drive("Storage", dr[0].total - dr[0].free, dr[0].total, c);
    }
    net_format_ip(ni.ip, a, sizeof(a));
    snprintf(c, sizeof(c), "%s%s", net_is_offline() ? "Airplane mode" : ni.configured ? "Connected (Ethernet), " : "Not connected",
             !net_is_offline() && ni.configured ? a : "");
    s_info("Network", c);
    s_header("Quick settings");
    s_toggle("Dark mode", "Switch between the dark and light theme", theme_get_mode() == THEME_DARK, A_THEME);
    s_toggle("Do not disturb", "Notifications go straight to the notification centre", setting_get_int("dnd", 0), A_DND);
}

static void s_page_system(void)
{
    char a[96], b[32], c[32];
    uint64_t total, freef;
    drive_info_t dr[8];
    int nd = storage_list(dr, 8);
    pmm_get_stats(&total, &freef);

    s_header("Display");
    snprintf(a, sizeof(a), "%u x %u, 32-bit colour", fb_get_width(), fb_get_height());
    s_info("Resolution", a);
    s_info("Graphics", s_has_device("Display", NULL) ? "Framebuffer driver (no hardware acceleration)" : "None");
    s_header("Sound");
    s_info("Output", g_kstate.audio_ready ? "AC97 sound card" : "No sound card found");
    s_slider("System volume", setting_get_int("volume", 80), A_VOLUME);
    s_slider("JARVIS voice volume", setting_get_int("voice_volume", 100), A_VOICE_VOL);
    s_info("Microphone", kernel_voice_bridge_connected() ? "Host microphone, through the voice bridge" : "Not connected (start with make run-voice)");
    s_header("Notifications");
    s_toggle("Do not disturb", "Pop-ups are hidden; notifications stay in the notification centre", setting_get_int("dnd", 0), A_DND);
    s_header("Power");
    s_choice("Lock the PC after inactivity", OPT_LOCK, 5, s_lock_index(), A_LOCK_AFTER);
    s_button("Lock now", "Show the sign-in screen", "Lock", A_LOCK_NOW, 0);
    s_button("Restart", "Saves your files, then restarts", "Restart", A_RESTART, 0);
    s_button("Shut down", "Saves your files, then turns the PC off", "Shut down", A_SHUTDOWN, 1);
    s_header("Storage");
    for (int i = 0; i < nd; i++) {
        s_fmt_size(dr[i].total - dr[i].free, b, sizeof(b)); s_fmt_size(dr[i].total, c, sizeof(c));
        snprintf(a, sizeof(a), "%s used of %s  -  %s%s", b, c, dr[i].fs, dr[i].read_only ? ", read-only" : "");
        char lbl[64]; snprintf(lbl, sizeof(lbl), "%s (%s)", dr[i].name, dr[i].device[0] ? dr[i].device : "memory");
        s_drive(lbl, dr[i].total - dr[i].free, dr[i].total, a);
    }
    long trash = vfs_size("/jarvis/trash");
    s_fmt_size(trash > 0 ? (uint64_t)trash : 0, b, sizeof(b));
    snprintf(a, sizeof(a), "Uses %s", b);
    s_button("Recycle Bin", a, "Empty", A_EMPTY_TRASH, 0);
    s_header("Clipboard");
    size_t clen;
    clipboard_get(&clen);
    snprintf(a, sizeof(a), "%u characters copied", (unsigned)clen);
    s_button("Clipboard", a, "Clear", A_CLEAR_CLIP, 0);
    s_header("About");
    setting_get("device_name", a, sizeof(a));
    s_field("Device name", a[0] ? a : "JARVIS-PC", 0, A_DEVICE_NAME);
    sys_cpu_name(a, sizeof(a));
    s_info("Processor", a);
    snprintf(a, sizeof(a), "%u MB usable, %u MB in use", (unsigned)(total >> 8), (unsigned)((total - freef) >> 8));
    s_info("Memory", a);
    sys_format_uptime(a, sizeof(a));
    s_info("Running for", a);
    s_info("System", "JARVIS OS 1.0, 64-bit (x86-64), built " __DATE__);
}

static void s_page_devices(void)
{
    static dev_info_t d[48];
    static const char *const st_name[] = { "Working", "No driver", "Not working" };
    int n = devmgr_list(d, 48);
    char a[96];

    s_header("Bluetooth");
    if (s_has_device(NULL, "Bluetooth"))
        s_info("Bluetooth adapter", "Found, but there is no Bluetooth driver yet");
    else
        s_info("Bluetooth adapter", "None found on this PC");
    s_header("Mouse");
    s_choice("Pointer speed", OPT_SPEED, 4, setting_get_int("mouse_speed", 2) - 1, A_MOUSE_SPEED);
    s_toggle("Swap left and right buttons", "For left-handed use", setting_get_int("mouse_swap", 0), A_MOUSE_SWAP);
    s_header("USB");
    s_toggle("AutoPlay", "Open This PC when a USB drive is plugged in", setting_get_int("autoplay", 1), A_AUTOPLAY);
    int usb = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(d[i].type, "USB") || !strncmp(d[i].name, "USB host", 8)) continue;
        snprintf(a, sizeof(a), "%s, %s", d[i].location, st_name[d[i].state % 3]);
        s_info(d[i].name, a);
        usb++;
    }
    if (!usb) s_info("USB devices", "Nothing plugged in");
    s_header("All devices");
    for (int i = 0; i < n; i++) {
        if (!strcmp(d[i].type, "USB") && strncmp(d[i].name, "USB host", 8)) continue;
        snprintf(a, sizeof(a), "%s  -  %s", d[i].type, st_name[d[i].state % 3]);
        s_info(d[i].name, a);
    }
    s_note("Printers, cameras and phones need drivers JARVIS OS does not have yet.");
}

static void s_page_network(void)
{
    net_info_t ni;
    char a[64], b[64];
    net_get_info(&ni);
    s_header("Airplane mode");
    s_toggle("Airplane mode", "Stops all network traffic", net_is_offline(), A_AIRPLANE);
    s_header("Ethernet");
    if (!ni.adapter) {
        s_info("Ethernet", "No supported Ethernet adapter found");
    } else {
        s_info("Status", net_is_offline() ? "Off (airplane mode)" : !ni.link ? "Cable unplugged" :
             ni.configured ? "Connected" : "Waiting for an address");
        net_format_ip(ni.ip, a, sizeof(a)); net_format_ip(ni.mask, b, sizeof(b));
        if (ni.configured) { s_info("IP address", a); s_info("Subnet mask", b); }
        net_format_ip(ni.gateway, a, sizeof(a)); net_format_ip(ni.dns, b, sizeof(b));
        if (ni.configured) { s_info("Gateway", a); s_info("DNS server", b); }
        snprintf(a, sizeof(a), "%02x:%02x:%02x:%02x:%02x:%02x", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
        s_info("Physical address (MAC)", a);
        snprintf(a, sizeof(a), "%u received, %u sent", (unsigned)ni.rx_frames, (unsigned)ni.tx_frames);
        s_info("Packets", a);
        s_button("Renew address", "Ask the network (DHCP) for an address again", "Renew", A_RENEW, 0);
    }
    s_header("Wi-Fi");
    s_info("Wi-Fi adapter", s_has_device("Network", "Wireless") ? "Found, but there is no Wi-Fi driver yet" : "None found on this PC");
    s_note("In the virtual machine, the internet comes through Ethernet: your PC's Wi-Fi is shared with it.");
    s_note("VPN, proxy and mobile hotspot are not available yet.");
}

static void s_page_personal(void)
{
    s_header("Theme");
    s_choice("Mode", OPT_THEME, 3, (int)theme_get_mode(), A_THEME);
    s_choice("Accent colour", OPT_ACCENT, 7, setting_get_int("accent", 0), A_ACCENT);
    s_header("Background");
    s_choice("Wallpaper", OPT_WALL, 6, setting_get_int("wallpaper", 0), A_WALL);
    s_header("Taskbar");
    s_toggle("24-hour clock", "Otherwise the clock shows AM and PM", sys_clock_24h(), A_CLOCK24);
}

static void s_page_apps(void)
{
    char a[96];
    sg.npkgs = pkg_list(sg.pkgs, 16);
    s_header("Installed apps");
    for (int i = 0; i < sg.npkgs && i < 16; i++) {
        snprintf(a, sizeof(a), "Version %s  -  %s", sg.pkgs[i].version, sg.pkgs[i].publisher[0] ? sg.pkgs[i].publisher : "unknown publisher");
        s_button(sg.pkgs[i].title, a, "Uninstall", A_UNINSTALL0 + i, 1);
    }
    if (!sg.npkgs) s_note("No apps installed from packages. Open the Apps app to install some.");
    s_header("Startup");
    s_note("Apps that open automatically after you sign in:");
    for (int i = 0; i < desktop_app_builtin_count(); i++) {
        desktop_app_t *app = desktop_app_get(i);
        char key[48];
        snprintf(key, sizeof(key), "startup_%s", app->name);
        s_toggle(app->name, NULL, setting_get_int(key, 0), A_STARTUP0 + i);
    }
    s_header("Default apps");
    s_info("Text files", "Notepad");
    s_info("Packages (.jpk)", "Apps");
    s_info("Programs", "Terminal");
}

static void s_page_accounts(void)
{
    s_header("Your info");
    s_field("Name", account_name(), 0, A_NAME);
    s_header("Sign-in options");
    if (account_has_password() || account_has_pin())
        s_note(sg.verified ? "Identity confirmed for this session." : "You will be asked for your current PIN or password first.");
    s_field("Password", "", 1, A_PASSWORD);
    if (account_has_password()) s_button("Remove password", "Sign in without a password", "Remove", A_PASSWORD_DEL, 1);
    s_field("PIN (4-12 digits)", "", 1, A_PIN);
    if (account_has_pin()) s_button("Remove PIN", "Sign in without a PIN", "Remove", A_PIN_DEL, 1);
    s_field("Spoken passphrase", "", 1, A_VOICE);
    if (account_has_voice()) s_button("Remove passphrase", "Turn off voice sign-in", "Remove", A_VOICE_DEL, 1);
    s_note("Speak your passphrase on the sign-in screen; small mishearings are tolerated.");
    s_note("Choose a phrase of three or more words that others will not guess.");
    s_header("Other users");
    s_note("JARVIS OS has one account for now.");
}

static void s_page_time(void)
{
    char clock[16], date[40];
    s_header("Date & time");
    sys_format_clock(clock, sizeof(clock));
    sys_format_date(date, sizeof(date));
    s_info("Time", clock);
    s_info("Date", date);
    s_info("Time source", "The PC's hardware clock");
    s_toggle("24-hour clock", NULL, sys_clock_24h(), A_CLOCK24);
    s_header("Language & region");
    s_info("Language", "English (United States)");
    s_info("Keyboard", "US layout");
    s_header("Speech");
    s_info("Voice bridge", kernel_voice_bridge_connected() ? "Connected" : "Not connected");
    s_info("Language model", ai_host_online() ? ai_host_model() : "Offline (built-in commands)");
    s_toggle("Voice commands", "JARVIS listens to the microphone (sign-in always listens)", setting_get_int("voice_input", 1), A_VOICE_IN);
}

static void s_page_access(void)
{
    s_header("Vision");
    s_toggle("High contrast", "Black, white and yellow, maximum contrast", theme_get_mode() == THEME_HIGH_CONTRAST, A_HC);
    s_header("Hearing and speech");
    s_toggle("Read notifications aloud", "JARVIS speaks each notification", setting_get_int("narrator", 0), A_NARRATOR);
    s_header("Interaction");
    s_choice("Pointer speed", OPT_SPEED, 4, setting_get_int("mouse_speed", 2) - 1, A_MOUSE_SPEED);
    s_toggle("Swap mouse buttons", NULL, setting_get_int("mouse_swap", 0), A_MOUSE_SWAP);
}

static void s_page_privacy(void)
{
    s_header("Security");
    s_info("Threat level", ui_shell_get_security_label());
    s_info("Sign-in", account_has_sign_in() ? "Required" : "Not set: anyone can use this PC");
    s_info("Account data", "Protected: programs, JARVIS and the Terminal cannot read it");
    s_info("Disk encryption", "Not available yet: files on the disk are not encrypted");
    s_header("JARVIS permissions");
    s_toggle("Ask before risky actions", "Deleting, ending processes, uninstalling, shutting down", ai_tools_ask_first(), A_AI_ASK);
    s_toggle("Internet search", "Your questions may be sent to a web search engine", setting_get_int("ai_web", 1), A_AI_WEB);
    s_toggle("Microphone", "JARVIS listens for voice commands", setting_get_int("voice_input", 1), A_VOICE_IN);
    s_header("Activity history");
    s_button("Conversation with JARVIS", "Forget everything said so far", "Clear", A_CLEAR_HISTORY, 0);
}

static void s_page_update(void)
{
    s_header("JARVIS OS");
    s_info("Version", "1.0, built " __DATE__);
    s_info("How updates work", "Install a newer system image (make iso), then restart");
    s_note("Automatic updates need an update server, which JARVIS OS does not have yet.");
    s_note("Your files, apps and settings are on the disk and are kept across updates.");
}

static void s_build_page(void)
{
    sg.nrows = 0;
    switch (sg.page) {
    case P_HOME: s_page_home(); break;           case P_SYSTEM: s_page_system(); break;
    case P_DEVICES: s_page_devices(); break;     case P_NETWORK: s_page_network(); break;
    case P_PERSONAL: s_page_personal(); break;   case P_APPS: s_page_apps(); break;
    case P_ACCOUNTS: s_page_accounts(); break;   case P_TIME: s_page_time(); break;
    case P_ACCESS: s_page_access(); break;       case P_PRIVACY: s_page_privacy(); break;
    default: s_page_update(); break;
    }
    int y = 0;
    for (int i = 0; i < sg.nrows; i++) {
        s_row_t *r = &sg.rows[i];
        r->h = r->type == R_HEADER ? 40 : r->type == R_NOTE ? 24 : r->type == R_INFO ? 36 : 58;
        if (r->type == R_HEADER && i == 0) r->h = 30;
        r->y = y;
        y += r->h + (r->type == R_HEADER || r->type == R_NOTE ? 0 : 4);
    }
    sg.content_h = y;
}

/* ==========================================================================
 * Actions
 * ========================================================================== */
static void s_say(const char *msg, uint32_t color)
{
    snprintf(sg.message, sizeof(sg.message), "%s", msg);
    sg.message_color = color;
}

static int s_needs_verify(int id)
{
    return (id == A_PASSWORD || id == A_PASSWORD_DEL || id == A_PIN || id == A_PIN_DEL || id == A_VOICE ||
            id == A_VOICE_DEL) && (account_has_password() || account_has_pin()) && !sg.verified;
}

static void s_apply_field(int id, const char *text)
{
    char err[96] = "";
    int rc = 0;
    switch (id) {
    case A_DEVICE_NAME: if (text[0]) setting_set("device_name", text); break;
    case A_NAME:        rc = account_set_name(text); break;
    case A_PASSWORD:    rc = text[0] ? account_set_password(text) : -1; if (text[0] && !rc) s_say("Password saved.", T(OK)); break;
    case A_PIN:         rc = text[0] ? account_set_pin(text, err, sizeof(err)) : -1; if (!rc) s_say("PIN saved.", T(OK)); break;
    case A_VOICE:       rc = text[0] ? account_set_voice(text, err, sizeof(err)) : -1; if (!rc) s_say("Spoken passphrase saved.", T(OK)); break;
    }
    if (rc && err[0]) s_say(err, T(DANGER));
}

static void s_do_action(int id, int value)
{
    char p[24];
    if (s_needs_verify(id)) { sg.verify_open = 1; sg.verify_action = id; sg.verify_len = 0; sg.verify_buf[0] = 0; return; }

    switch (id) {
    case A_THEME:
        if (sg.page == P_HOME) value = theme_get_mode() == THEME_DARK ? THEME_LIGHT : THEME_DARK;
        theme_set_mode((theme_mode_t)value); setting_set_int("theme", value); break;
    case A_HC: {
        int v = theme_get_mode() == THEME_HIGH_CONTRAST ? THEME_DARK : THEME_HIGH_CONTRAST;
        theme_set_mode((theme_mode_t)v); setting_set_int("theme", v); break;
    }
    case A_ACCENT:      theme_set_accent(value); setting_set_int("accent", value); break;
    case A_WALL:        theme_set_wallpaper(value); setting_set_int("wallpaper", value); break;
    case A_CLOCK24:     sys_set_clock_24h(!sys_clock_24h()); setting_set_int("clock_24h", sys_clock_24h()); break;
    case A_DND:         setting_set_int("dnd", !setting_get_int("dnd", 0)); break;
    case A_NARRATOR:    setting_set_int("narrator", !setting_get_int("narrator", 0)); break;
    case A_VOICE_IN:    setting_set_int("voice_input", !setting_get_int("voice_input", 1)); break;
    case A_AI_ASK:      ai_tools_set_ask_first(!ai_tools_ask_first()); setting_set_int("ai_ask_first", ai_tools_ask_first()); break;
    case A_AI_WEB:      setting_set_int("ai_web", !setting_get_int("ai_web", 1)); desktop_send_host_prefs(); break;
    case A_VOLUME:      audio_set_volume(value); setting_set_int("volume", value); break;
    case A_VOICE_VOL:   setting_set_int("voice_volume", value); desktop_send_host_prefs(); break;
    case A_MOUSE_SPEED: setting_set_int("mouse_speed", value + 1); desktop_apply_settings(); break;
    case A_MOUSE_SWAP:  setting_set_int("mouse_swap", !setting_get_int("mouse_swap", 0)); desktop_apply_settings(); break;
    case A_AUTOPLAY:    setting_set_int("autoplay", !setting_get_int("autoplay", 1)); break;
    case A_AIRPLANE:    net_set_offline(!net_is_offline()); setting_set_int("airplane", net_is_offline()); break;
    case A_RENEW:       net_renew(); s_say("Asked the network for a new address.", T(OK)); break;
    case A_LOCK_AFTER:  setting_set_int("lock_after_min", LOCK_MIN[value]); break;
    case A_LOCK_NOW:    desktop_lock(); break;
    case A_RESTART:     desktop_system_restart(); break;
    case A_SHUTDOWN:    desktop_system_shutdown(); break;
    case A_EMPTY_TRASH: file_explorer_empty_trash(); break;
    case A_CLEAR_CLIP:  clipboard_set("", 0); break;
    case A_CLEAR_HISTORY:
        ui_shell_clear_lines();
        ai_host_pref("forget");
        s_say("The conversation was cleared.", T(OK));
        break;
    case A_PASSWORD_DEL: account_set_password(""); s_say("Password removed.", T(OK)); break;
    case A_PIN_DEL:      { char e[8]; account_set_pin("", e, sizeof(e)); s_say("PIN removed.", T(OK)); break; }
    case A_VOICE_DEL:    { char e[8]; account_set_voice("", e, sizeof(e)); s_say("Voice sign-in removed.", T(OK)); break; }
    default:
        if (id >= A_UNINSTALL0 && id < A_UNINSTALL0 + 16 && id - A_UNINSTALL0 < sg.npkgs) {
            snprintf(p, sizeof(p), "%s", sg.pkgs[id - A_UNINSTALL0].name);
            s_say(pkg_uninstall(p) == 0 ? "The app was uninstalled." : "The app could not be uninstalled.", T(OK));
        } else if (id >= A_STARTUP0 && id < A_STARTUP0 + 32) {
            char key[48];
            snprintf(key, sizeof(key), "startup_%s", desktop_app_get(id - A_STARTUP0)->name);
            setting_set_int(key, !setting_get_int(key, 0));
        }
        break;
    }
    desktop_invalidate();
}

/* ==========================================================================
 * Drawing
 * ========================================================================== */
static void s_draw_toggle(int x, int y, int on)
{
    fb_fill_rounded_rect(x, y, 44, 22, 11, on ? T(ACCENT) : T(BORDER));
    fb_fill_circle(on ? x + 33 : x + 11, y + 11, 7, on ? T(ACCENT_TEXT) : T(TEXT));
}

static int s_choice_width(const s_row_t *r, int i) { return fb_text_width(r->opts[i]) + 18; }

static void s_draw_row(s_row_t *r, int idx, int x, int y, int w)
{
    char btn[48] = "", *sep;
    if (r->type == R_HEADER) { fb_draw_text_bold(x, y + 14, r->label, T(TEXT)); return; }
    if (r->type == R_NOTE)   { ui_text_fit(x + 4, y + 4, w - 8, r->text, T(TEXT_DIM)); return; }

    fb_fill_rounded_rect(x, y, w, r->h, 6, T(SURFACE));
    if ((sep = strchr(r->label, 1)) != NULL) { snprintf(btn, sizeof(btn), "%s", sep + 1); *sep = 0; }

    if (r->type == R_INFO) {
        fb_draw_text(x + 16, y + 10, r->label, T(TEXT));
        int tw = fb_text_width(r->text), maxw = w / 2;
        ui_text_fit(x + w - 16 - (tw < maxw ? tw : maxw), y + 10, maxw, r->text, T(TEXT_DIM));
        if (sep) *sep = 1;
        return;
    }
    if (r->type != R_DRIVE)
        fb_draw_text(x + 16, y + (r->text[0] && r->type != R_FIELD ? 10 : 20), r->label, T(TEXT));
    if (r->text[0] && r->type != R_FIELD && r->type != R_DRIVE)
        ui_text_fit(x + 16, y + 32, w - 300, r->text, T(TEXT_DIM));

    int rx = x + w - 16;
    switch (r->type) {
    case R_TOGGLE:
        fb_draw_text(rx - 44 - 40, y + 20, r->value ? "On" : "Off", T(TEXT_DIM));
        s_draw_toggle(rx - 44, y + 18, r->value);
        break;
    case R_CHOICE: {
        int total = 0;
        for (int i = 0; i < r->nopts; i++) total += s_choice_width(r, i) + 4;
        int cx = rx - total;
        for (int i = 0; i < r->nopts; i++) {
            int cw = s_choice_width(r, i);
            int sel = i == r->value;
            fb_fill_rounded_rect(cx, y + 15, cw, 28, 5, sel ? T(ACCENT) : T(HOVER));
            fb_draw_text(cx + 9, y + 21, r->opts[i], sel ? T(ACCENT_TEXT) : T(TEXT));
            cx += cw + 4;
        }
        break;
    }
    case R_SLIDER: {
        char v[8];
        int sw = 220, sx = rx - sw - 44;
        fb_fill_rounded_rect(sx, y + 27, sw, 4, 2, T(BORDER));
        fb_fill_rounded_rect(sx, y + 27, sw * r->value / 100, 4, 2, T(ACCENT));
        fb_fill_circle(sx + sw * r->value / 100, y + 29, 8, T(ACCENT));
        snprintf(v, sizeof(v), "%d", r->value);
        fb_draw_text(rx - 32, y + 21, v, T(TEXT_DIM));
        break;
    }
    case R_BUTTON: {
        int bw = fb_text_width(btn) + 28;
        ui_button(rx - bw, y + 14, bw, 30, btn, r->danger ? T(DANGER) : T(HOVER), r->danger ? 0x00FFFFFF : T(TEXT));
        break;
    }
    case R_FIELD: {
        int fw = 260, fx = rx - fw - 76;
        int editing = sg.editing == idx;
        const char *shown = editing ? sg.edit : r->text;
        char masked[96];
        if (r->masked) {
            int n = editing ? sg.edit_len : 0;
            for (int i = 0; i < n && i < 95; i++) masked[i] = '*';
            masked[n < 95 ? n : 95] = 0;
            shown = masked;
        }
        fb_fill_rounded_rect(fx, y + 14, fw, 30, 5, T(INPUT));
        fb_draw_rounded_rect(fx, y + 14, fw, 30, 5, editing ? T(ACCENT) : T(BORDER));
        if (shown[0]) ui_text_fit(fx + 8, y + 21, fw - 16, shown, T(TEXT));
        else ui_text_fit(fx + 8, y + 21, fw - 16, r->masked ? (editing ? "" : "Type a new one") : "", T(TEXT_DIM));
        if (editing) fb_fill_rect(fx + 9 + (int)strlen(shown) * 8, y + 21, 1, 16, T(TEXT));
        ui_button(rx - 68, y + 14, 68, 30, "Save", editing ? T(ACCENT) : T(HOVER), editing ? T(ACCENT_TEXT) : T(TEXT_DIM));
        break;
    }
    case R_DRIVE:
        fb_draw_text(x + 16, y + 10, r->label, T(TEXT));
        ui_progress(x + 16, y + 32, w - 32 - 300, 8, r->value, T(BORDER), r->value >= 90 ? T(DANGER) : T(ACCENT));
        ui_text_fit(x + w - 296, y + 28, 280, r->text, T(TEXT_DIM));
        break;
    }
    if (sep) *sep = 1;
}

void settings_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    s_build_page();

    /* sidebar */
    fb_fill_rect(x, y, SET_SIDEBAR_W, h, T(SURFACE));
    fb_fill_circle(x + 36, y + 36, 20, T(ACCENT));
    glyph_draw(GLYPH_USER, x + 26, y + 26, 20, T(ACCENT_TEXT), T(ACCENT));
    ui_text_fit(x + 66, y + 22, SET_SIDEBAR_W - 76, account_name(), T(TEXT));
    fb_draw_text(x + 66, y + 40, "Local account", T(TEXT_DIM));
    for (int i = 0; i < P_COUNT; i++) {
        int iy = y + 76 + i * 34;
        if (iy + 32 > y + h) break;
        if (sg.page == i) {
            fb_fill_rounded_rect(x + 8, iy, SET_SIDEBAR_W - 16, 32, 5, T(HOVER));
            fb_fill_rounded_rect(x + 8, iy + 9, 3, 14, 1, T(ACCENT));
        }
        icon_draw(s_g_pages[i].icon, x + 20, iy + 8, 16);
        ui_text_fit(x + 46, iy + 8, SET_SIDEBAR_W - 56, s_g_pages[i].name, T(TEXT));
    }

    /* page */
    int px = x + SET_SIDEBAR_W, pw = w - SET_SIDEBAR_W;
    fb_fill_rect(px, y, pw, h, T(WINDOW));
    fb_draw_text_scaled(px + 28, y + 18, s_g_pages[sg.page].name, T(TEXT), 2);
    if (sg.message[0]) {
        int mw = fb_text_width(sg.message);
        fb_draw_text(px + pw - 28 - mw, y + 26, sg.message, sg.message_color);
    }

    int view_y = y + SET_TITLE_H, view_h = h - SET_TITLE_H;
    int max_scroll = sg.content_h - view_h + 16;
    if (sg.scroll > max_scroll) sg.scroll = max_scroll;
    if (sg.scroll < 0) sg.scroll = 0;

    fb_clip_t saved = fb_get_clip();
    fb_set_clip(px, view_y, pw, view_h);
    for (int i = 0; i < sg.nrows; i++) {
        s_row_t *r = &sg.rows[i];
        int ry = view_y + r->y - sg.scroll;
        if (ry + r->h < view_y || ry > view_y + view_h) continue;
        s_draw_row(r, i, px + 28, ry, pw - 56 - 10);
    }
    fb_restore_clip(saved);

    if (sg.content_h > view_h) {                                   /* scrollbar */
        int th = view_h * view_h / sg.content_h;
        int ty = view_y + (view_h - th) * sg.scroll / (max_scroll > 0 ? max_scroll : 1);
        fb_fill_rounded_rect(px + pw - 10, ty, 6, th, 3, T(BORDER));
    }

    if (sg.verify_open) {                                          /* confirm identity */
        int vw = 380, vh = 170, vx = x + (w - vw) / 2, vy = y + (h - vh) / 2;
        fb_blend_rect(x, y, w, h, 0x00000000, 110);
        fb_fill_rounded_rect(vx, vy, vw, vh, 8, T(BORDER));
        fb_fill_rounded_rect(vx + 1, vy + 1, vw - 2, vh - 2, 7, T(MENU));
        fb_draw_text_bold(vx + 20, vy + 18, "Confirm it's you", T(TEXT));
        fb_draw_text(vx + 20, vy + 44, "Type your current PIN or password.", T(TEXT_DIM));
        char m[64];
        int n = sg.verify_len < 63 ? sg.verify_len : 63;
        for (int i = 0; i < n; i++) m[i] = '*';
        m[n] = 0;
        fb_fill_rounded_rect(vx + 20, vy + 72, vw - 40, 32, 5, T(INPUT));
        fb_draw_rounded_rect(vx + 20, vy + 72, vw - 40, 32, 5, T(ACCENT));
        fb_draw_text(vx + 30, vy + 80, m, T(TEXT));
        fb_fill_rect(vx + 31 + n * 8, vy + 80, 1, 16, T(TEXT));
        if (auth_wait_seconds()) {
            char wmsg[48];
            snprintf(wmsg, sizeof(wmsg), "Wait %d s before trying again.", auth_wait_seconds());
            fb_draw_text(vx + 20, vy + 112, wmsg, T(WARN));
        }
        ui_button(vx + vw - 190, vy + vh - 44, 84, 30, "OK", T(ACCENT), T(ACCENT_TEXT));
        ui_button(vx + vw - 98, vy + vh - 44, 80, 30, "Cancel", T(HOVER), T(TEXT));
    }
}

/* ==========================================================================
 * Input
 * ========================================================================== */
static void s_verify_submit(void)
{
    if (auth_check_typed(sg.verify_buf)) {
        sg.verified = 1;
        sg.verify_open = 0;
        int id = sg.verify_action;
        if (id == A_PASSWORD || id == A_PIN || id == A_VOICE) {
            for (int i = 0; i < sg.nrows; i++)                     /* start typing in that s_field */
                if (sg.rows[i].id == id) { sg.editing = i; sg.edit_len = 0; sg.edit[0] = 0; }
        } else {
            s_do_action(id, 0);
        }
    } else {
        sg.verify_len = 0; sg.verify_buf[0] = 0;
        s_say("That PIN or password is incorrect.", T(DANGER));
    }
}

void settings_event_callback(wm_window_t *win, wm_input_event_t *ev)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    int pw = w - SET_SIDEBAR_W, view_h = h - SET_TITLE_H;

    if (sg.verify_open) {
        int vw = 380, vh = 170, vx = (w - vw) / 2, vy = (h - vh) / 2;
        if (ev->type == IE_TEXT_INPUT && sg.verify_len < 63) {
            sg.verify_buf[sg.verify_len++] = ev->data.text.ch; sg.verify_buf[sg.verify_len] = 0;
        } else if (ev->type == IE_KEY_DOWN) {
            uint32_t k = ev->data.keyboard.key;
            if (k == WM_KEY_BACKSPACE && sg.verify_len) sg.verify_buf[--sg.verify_len] = 0;
            else if (k == WM_KEY_ENTER) s_verify_submit();
            else if (k == WM_KEY_ESC) sg.verify_open = 0;
        } else if (ev->type == IE_MOUSE_DOWN) {
            int mx = ev->data.mouse.x, my = ev->data.mouse.y;
            if (ui_hit(mx, my, vx + vw - 190, vy + vh - 44, 84, 30)) s_verify_submit();
            else if (ui_hit(mx, my, vx + vw - 98, vy + vh - 44, 80, 30)) sg.verify_open = 0;
        }
        return;
    }

    if (sg.editing >= 0 && (ev->type == IE_TEXT_INPUT || ev->type == IE_KEY_DOWN)) {
        if (ev->type == IE_TEXT_INPUT) {
            char ch = ev->data.text.ch;
            if (ch >= 32 && ch < 127 && sg.edit_len < 94) { sg.edit[sg.edit_len++] = ch; sg.edit[sg.edit_len] = 0; }
            return;
        }
        uint32_t k = ev->data.keyboard.key;
        if (k == WM_KEY_BACKSPACE && sg.edit_len) sg.edit[--sg.edit_len] = 0;
        else if (k == WM_KEY_ESC) sg.editing = -1;
        else if (k == WM_KEY_ENTER) {
            int id = sg.rows[sg.editing].id;
            sg.editing = -1;
            s_apply_field(id, sg.edit);
        }
        return;
    }

    if (ev->type == IE_KEY_DOWN) {
        uint32_t k = ev->data.keyboard.key;
        if (k == WM_KEY_DOWN) sg.scroll += 40;
        else if (k == WM_KEY_UP) sg.scroll -= 40;
        else if (k == WM_KEY_PGDN) sg.scroll += view_h - 60;
        else if (k == WM_KEY_PGUP) sg.scroll -= view_h - 60;
        else if (k == WM_KEY_HOME) sg.scroll = 0;
        if (sg.scroll < 0) sg.scroll = 0;
        return;
    }
    if (ev->type != IE_MOUSE_DOWN || ev->data.mouse.button != 1) return;
    int mx = ev->data.mouse.x, my = ev->data.mouse.y;
    sg.message[0] = 0;

    if (mx < SET_SIDEBAR_W) {
        int i = (my - 76) / 34;
        if (my >= 76 && i >= 0 && i < P_COUNT) { sg.page = i; sg.scroll = 0; sg.editing = -1; }
        return;
    }
    if (my < SET_TITLE_H) return;
    s_build_page();
    if (mx > w - 16) {                                             /* scrollbar */
        int max_scroll = sg.content_h - view_h + 16;
        if (max_scroll > 0) sg.scroll = (my - SET_TITLE_H) * max_scroll / (view_h ? view_h : 1);
        return;
    }
    int cx = mx - SET_SIDEBAR_W - 28, cy = my - SET_TITLE_H + sg.scroll, rw = pw - 56 - 10, rx = rw - 16;
    for (int i = 0; i < sg.nrows; i++) {
        s_row_t *r = &sg.rows[i];
        if (cy < r->y || cy >= r->y + r->h || cx < 0 || cx > rw) continue;
        int ly = cy - r->y;
        switch (r->type) {
        case R_TOGGLE:
            if (cx >= rx - 90) s_do_action(r->id, !r->value);
            break;
        case R_CHOICE: {
            int total = 0;
            for (int k = 0; k < r->nopts; k++) total += s_choice_width(r, k) + 4;
            int bx = rx - total;
            for (int k = 0; k < r->nopts; k++) {
                int bw = s_choice_width(r, k);
                if (cx >= bx && cx < bx + bw && ly >= 15 && ly < 43) { s_do_action(r->id, k); break; }
                bx += bw + 4;
            }
            break;
        }
        case R_SLIDER: {
            int sw = 220, sx = rx - sw - 44;
            if (cx >= sx - 8 && cx <= sx + sw + 8) {
                int v = (cx - sx) * 100 / sw;
                s_do_action(r->id, v < 0 ? 0 : v > 100 ? 100 : v);
            }
            break;
        }
        case R_BUTTON: {
            char *sep = strchr(r->label, 1);
            int bw = (sep ? fb_text_width(sep + 1) : 40) + 28;
            if (cx >= rx - bw && ly >= 14 && ly < 44) s_do_action(r->id, 0);
            break;
        }
        case R_FIELD: {
            int fw = 260, fx = rx - fw - 76;
            if (cx >= fx && cx < fx + fw) {
                if (s_needs_verify(r->id)) { s_do_action(r->id, 0); break; }
                sg.editing = i;
                snprintf(sg.edit, sizeof(sg.edit), "%s", r->masked ? "" : r->text);
                sg.edit_len = (int)strlen(sg.edit);
            } else if (cx >= rx - 68 && sg.editing == i) {
                sg.editing = -1;
                s_apply_field(r->id, sg.edit);
            }
            break;
        }
        default: break;
        }
        return;
    }
}

wm_window_t *settings_app_open(int x, int y, int w, int h)
{
    memset(&sg, 0, sizeof(sg));
    sg.editing = -1;
    wm_window_t *win = wm_create_window("Settings", x, y, w, h, WS_DEFAULT, settings_draw_callback, settings_event_callback);
    if (!win) return NULL;
    win->icon = ICON_GEAR;
    win->min_w = 760;
    win->min_h = 420;
    sg.window = win;
    return win;
}
