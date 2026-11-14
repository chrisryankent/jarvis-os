/*
 * =============================================================================
 * JARVIS OS — System settings registry (desktop/prefs.c)
 * Language: C17
 *
 * Every user-adjustable setting, by name, with one place that validates,
 * saves and applies it. The Settings app, the Terminal ("set", "settings")
 * and JARVIS (change_setting / get_settings tools) all go through here, so
 * the result is the same whichever way a setting is changed.
 * =============================================================================
 */

void desktop_apply_settings(void);
void desktop_send_host_prefs(void);
int  prefs_get(const char *name, char *out, size_t n);

typedef enum { PK_BOOL, PK_INT, PK_ENUM, PK_TEXT } pref_kind_t;

typedef struct {
    const char *name;
    pref_kind_t kind;
    int lo, hi, dflt;               /* PK_INT range / default, PK_BOOL default  */
    const char *const *opts;        /* PK_ENUM choices (stored as the index)    */
    int nopts;
    const char *help;
} pref_t;

static const char *const PO_THEME[] = { "dark", "light", "high_contrast" };
static const char *const PO_ACCENT[] = { "default", "blue", "red", "green", "gold", "purple", "orange" };
static const char *const PO_BLOOM[] = { "theme", "purple", "forest", "sunset", "graphite", "ocean" };
static const char *const PO_FIT[] = { "fill", "fit", "stretch", "center", "tile" };
static const char *const PO_ALIGN[] = { "center", "left" };
static const char *const PO_MOUSE[] = { "slow", "normal", "fast", "fastest" };

static const pref_t g_prefs[] = {
    /* Display */
    { "brightness",       PK_INT,  10, 100, 100, NULL, 0, "Screen brightness in percent" },
    { "night_light",      PK_BOOL, 0, 1, 0, NULL, 0, "Warmer colours that are easier on the eyes at night" },
    { "night_light_strength", PK_INT, 0, 100, 50, NULL, 0, "How warm the night light is" },
    { "night_light_auto", PK_BOOL, 0, 1, 0, NULL, 0, "Turn the night light on from 19:00 to 07:00" },
    { "resolution",       PK_TEXT, 0, 0, 0, NULL, 0, "Screen resolution, e.g. 1280x720 (see 'display modes')" },
    { "screen_off_min",   PK_INT,  0, 240, 0, NULL, 0, "Turn the screen off after this many idle minutes (0 = never)" },
    /* Personalization */
    { "theme",            PK_ENUM, 0, 0, 0, PO_THEME, 3, "Colour mode" },
    { "accent",           PK_ENUM, 0, 0, 0, PO_ACCENT, 7, "Accent colour" },
    { "wallpaper",        PK_TEXT, 0, 0, 0, NULL, 0, "bloom, aurora, mountains, dunes, night, waves, solid, or a picture file path" },
    { "wallpaper_fit",    PK_ENUM, 0, 0, 0, PO_FIT, 5, "How a picture wallpaper fills the screen" },
    { "wallpaper_color",  PK_TEXT, 0, 0, 0, NULL, 0, "Colour for the solid wallpaper, #RRGGBB" },
    { "bloom_color",      PK_ENUM, 0, 0, 0, PO_BLOOM, 6, "Colours of the Bloom wallpaper" },
    { "transparency",     PK_BOOL, 0, 1, 1, NULL, 0, "See-through taskbar, Start menu and flyouts" },
    { "taskbar_align",    PK_ENUM, 0, 0, 0, PO_ALIGN, 2, "Taskbar icons in the centre or on the left" },
    { "desktop_icons",    PK_BOOL, 0, 1, 1, NULL, 0, "Show icons on the desktop" },
    { "clock_24h",        PK_BOOL, 0, 1, 1, NULL, 0, "24-hour clock" },
    { "clock_seconds",    PK_BOOL, 0, 1, 0, NULL, 0, "Show seconds in the taskbar clock" },
    /* Sound */
    { "volume",           PK_INT,  0, 100, 80, NULL, 0, "System volume" },
    { "mute",             PK_BOOL, 0, 1, 0, NULL, 0, "Mute all sound" },
    { "voice_volume",     PK_INT,  0, 100, 100, NULL, 0, "JARVIS voice volume" },
    /* Devices */
    { "mouse_speed",      PK_ENUM, 0, 0, 1, PO_MOUSE, 4, "Pointer speed" },
    { "mouse_swap",       PK_BOOL, 0, 1, 0, NULL, 0, "Swap the left and right mouse buttons" },
    { "scroll_lines",     PK_INT,  1, 10, 3, NULL, 0, "Lines to scroll per mouse-wheel notch" },
    { "autoplay",         PK_BOOL, 0, 1, 1, NULL, 0, "Open This PC when a USB drive is plugged in" },
    /* System */
    { "dnd",              PK_BOOL, 0, 1, 0, NULL, 0, "Do not disturb: hide notification pop-ups" },
    { "lock_after_min",   PK_INT,  0, 240, 0, NULL, 0, "Lock after this many idle minutes (0 = never)" },
    { "airplane",         PK_BOOL, 0, 1, 0, NULL, 0, "Airplane mode: stop all network traffic" },
    { "device_name",      PK_TEXT, 0, 0, 0, NULL, 0, "Name of this PC" },
    { "narrator",         PK_BOOL, 0, 1, 0, NULL, 0, "JARVIS reads notifications aloud" },
    { "ai_ask_first",     PK_BOOL, 0, 1, 0, NULL, 0, "JARVIS asks before risky actions" },
    { "ai_web",           PK_BOOL, 0, 1, 1, NULL, 0, "JARVIS may search the internet" },
};
#define PREF_COUNT ((int)(sizeof(g_prefs) / sizeof(g_prefs[0])))

static const pref_t *pref_find(const char *name)
{
    for (int i = 0; i < PREF_COUNT; i++)
        if (!strcmp(g_prefs[i].name, name)) return &g_prefs[i];
    /* friendly aliases */
    static const char *const alias[][2] = {
        { "wallpaper_style", "wallpaper" }, { "background", "wallpaper" }, { "mode", "theme" },
        { "night", "night_light" }, { "nightlight", "night_light" }, { "sound", "volume" },
        { "accent_color", "accent" }, { "screen_timeout", "screen_off_min" }, { "lock_after", "lock_after_min" },
        { "do_not_disturb", "dnd" }, { "pc_name", "device_name" }, { "computer_name", "device_name" },
        { "resolution_mode", "resolution" }, { "display_resolution", "resolution" },
    };
    for (size_t i = 0; i < sizeof(alias) / sizeof(alias[0]); i++)
        if (!strcmp(alias[i][0], name)) return pref_find(alias[i][1]);
    return NULL;
}

static int pref_bool_word(const char *v)
{
    if (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "enable") || !strcmp(v, "enabled")) return 1;
    if (!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "disable") || !strcmp(v, "disabled")) return 0;
    return -1;
}

static int pref_parse_int(const char *v, int *out)
{
    int n = 0, any = 0, neg = 0;
    if (*v == '-') { neg = 1; v++; }
    for (; *v >= '0' && *v <= '9'; v++) { n = n * 10 + (*v - '0'); any = 1; }
    if (*v == '%') v++;
    if (!any || *v) return -1;
    *out = neg ? -n : n;
    return 0;
}

/* Effective night-light state (manual switch, or the 19:00-07:00 schedule). */
static int prefs_night_now(void)
{
    if (setting_get_int("night_light", 0)) return 1;
    if (!setting_get_int("night_light_auto", 0)) return 0;
    sys_time_t t;
    sys_get_time(&t);
    return t.hour >= 19 || t.hour < 7;
}

void prefs_apply_display(void)
{
    int warm = prefs_night_now() ? setting_get_int("night_light_strength", 50) : 0;
    fb_set_color_filter(setting_get_int("brightness", 100), warm);
    desktop_invalidate();
}

static void desktop_display_changed(void);   /* desktop_init.c */

static int prefs_apply_resolution(const char *v, char *msg, size_t n)
{
    int w = 0, h = 0;
    const char *x = v;
    while (*x >= '0' && *x <= '9') w = w * 10 + (*x++ - '0');
    if (*x != 'x' && *x != 'X' && *x != '*') { snprintf(msg, n, "Write the resolution as WIDTHxHEIGHT, e.g. 1280x720"); return -1; }
    x++;
    while (*x >= '0' && *x <= '9') h = h * 10 + (*x++ - '0');
    int rc = fb_set_mode(w, h);
    if (rc == -1) { snprintf(msg, n, "This display adapter cannot change resolution (it stays %ux%u)", fb_get_width(), fb_get_height()); return -1; }
    if (rc == -2) {
        char list[256] = "";
        for (int i = 0; i < fb_mode_count(); i++) {
            int mw, mh;
            fb_mode_get(i, &mw, &mh);
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%dx%d", i ? ", " : "", mw, mh);
        }
        snprintf(msg, n, "%dx%d is not available. Choose one of: %s", w, h, list);
        return -1;
    }
    if (rc) { snprintf(msg, n, "Not enough memory for %dx%d", w, h); return -1; }
    desktop_display_changed();
    char b[16];
    snprintf(b, sizeof(b), "%dx%d", w, h);
    setting_set("resolution", b);
    return 0;
}

/* Sets one setting. Returns 0 and a confirmation in msg, or -1 and the reason. */
int prefs_set(const char *name, const char *value, char *msg, size_t n)
{
    char v[256];
    snprintf(v, sizeof(v), "%s", value ? value : "");
    /* trim + lower-case (except free text and paths) */
    char *s = v;
    while (*s == ' ' || *s == '"' || *s == '\'') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '"' || s[l - 1] == '\'' || s[l - 1] == '\n')) s[--l] = 0;

    const pref_t *p = pref_find(name);
    if (!p) { snprintf(msg, n, "Unknown setting '%s'. Type 'settings' for the list.", name); return -1; }
    if (p->kind != PK_TEXT)
        for (char *c = s; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);

    int iv = 0;
    switch (p->kind) {
    case PK_BOOL:
        iv = pref_bool_word(s);
        if (!strcmp(s, "toggle")) iv = !setting_get_int(p->name, p->dflt);
        if (iv < 0) { snprintf(msg, n, "%s takes on or off", p->name); return -1; }
        break;
    case PK_INT:
        if (!strcmp(s, "up") || !strcmp(s, "down")) {
            int step = (p->hi - p->lo) / 10;
            if (step < 1) step = 1;
            iv = setting_get_int(p->name, p->dflt) + (s[0] == 'u' ? step : -step);
            if (iv < p->lo) iv = p->lo;
            if (iv > p->hi) iv = p->hi;
        } else if (pref_parse_int(s, &iv) || iv < p->lo || iv > p->hi) {
            snprintf(msg, n, "%s takes a number from %d to %d", p->name, p->lo, p->hi);
            return -1;
        }
        break;
    case PK_ENUM: {
        int found = -1;
        for (int i = 0; i < p->nopts; i++)
            if (!strcmp(s, p->opts[i])) found = i;
        if (found < 0 && !strcmp(p->name, "theme") && (!strcmp(s, "hc") || !strcmp(s, "high contrast"))) found = 2;
        if (found < 0 && !pref_parse_int(s, &iv) && iv >= 0 && iv < p->nopts) found = iv;
        if (found < 0) {
            char opts[160] = "";
            for (int i = 0; i < p->nopts; i++)
                snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), "%s%s", i ? ", " : "", p->opts[i]);
            snprintf(msg, n, "%s takes one of: %s", p->name, opts);
            return -1;
        }
        iv = found;
        break;
    }
    case PK_TEXT:
        break;
    }

    /* ---- apply ---- */
    const char *nm = p->name;
    if (!strcmp(nm, "brightness") || !strcmp(nm, "night_light") || !strcmp(nm, "night_light_strength") || !strcmp(nm, "night_light_auto")) {
        setting_set_int(nm, iv);
        prefs_apply_display();
    } else if (!strcmp(nm, "resolution")) {
        if (prefs_apply_resolution(s, msg, n)) return -1;
    } else if (!strcmp(nm, "theme")) {
        theme_set_mode((theme_mode_t)iv);
        setting_set_int("theme", iv);
    } else if (!strcmp(nm, "accent")) {
        theme_set_accent(iv);
        setting_set_int("accent", iv);
    } else if (!strcmp(nm, "bloom_color")) {
        theme_set_wallpaper(iv);
        setting_set_int("wallpaper", iv);
        setting_set("wallpaper_style", "bloom");
    } else if (!strcmp(nm, "wallpaper")) {
        int style = -1;
        for (int i = 0; i < WP_PICTURE; i++)
            if (!strcmp(s, wallpaper_style_name(i)) || !strcmp(s, wallpaper_style_title(i))) style = i;
        if (style < 0 && (!strcmp(s, "night sky") || !strcmp(s, "stars"))) style = WP_NIGHT;
        if (style < 0 && (!strcmp(s, "default") || !strcmp(s, "theme"))) style = WP_BLOOM;
        if (style < 0 && (!strcmp(s, "ocean") || !strcmp(s, "sea"))) style = WP_WAVES;
        if (style < 0 && (!strcmp(s, "desert") || !strcmp(s, "sand"))) style = WP_DUNES;
        if (style < 0 && (!strcmp(s, "sunset") || !strcmp(s, "mountain"))) style = WP_MOUNTAINS;
        if (style < 0 && (!strcmp(s, "northern lights"))) style = WP_AURORA;
        if (style < 0 && (!strcmp(s, "color") || !strcmp(s, "colour"))) style = WP_SOLID;
        if (style >= 0) {
            setting_set("wallpaper_style", wallpaper_style_name(style));
        } else {
            if (s[0] != '/') { snprintf(msg, n, "Choose bloom, aurora, mountains, dunes, night, waves or solid, or give a picture's full path"); return -1; }
            if (!image_is_picture_name(s)) { snprintf(msg, n, "%s is not a picture (PNG, JPEG or BMP)", s); return -1; }
            image_t img;
            char err[96];
            if (image_load(s, &img, err, sizeof(err))) { snprintf(msg, n, "Cannot use %s: %s", s, err); return -1; }
            image_free(&img);
            setting_set("wallpaper_picture", s);
            setting_set("wallpaper_style", "picture");
        }
        desktop_wallpaper_invalidate();
        desktop_invalidate();
    } else if (!strcmp(nm, "wallpaper_fit")) {
        setting_set_int("wallpaper_fit", iv);
        desktop_wallpaper_invalidate();
    } else if (!strcmp(nm, "wallpaper_color")) {
        const char *h = s[0] == '#' ? s + 1 : s;
        int ok = strlen(h) == 6;
        for (int i = 0; ok && i < 6; i++) {
            char c = h[i];
            ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }
        static const struct { const char *n, *hex; } named[] = {
            { "black", "000000" }, { "white", "FFFFFF" }, { "blue", "1E5AA8" }, { "navy", "0A1A3A" },
            { "red", "A82020" }, { "green", "1E7A3C" }, { "teal", "107C7C" }, { "purple", "5B2A86" },
            { "orange", "C4501E" }, { "gray", "404040" }, { "grey", "404040" }, { "pink", "C2408A" },
        };
        char hex[8] = "";
        if (ok) snprintf(hex, sizeof(hex), "%s", h);
        for (size_t i = 0; !ok && i < sizeof(named) / sizeof(named[0]); i++)
            if (!strcmp(s, named[i].n)) { snprintf(hex, sizeof(hex), "%s", named[i].hex); ok = 1; }
        if (!ok) { snprintf(msg, n, "Give a colour as #RRGGBB (for example #1E5AA8) or a name like blue"); return -1; }
        char b[10];
        snprintf(b, sizeof(b), "#%s", hex);
        setting_set("wallpaper_color", b);
        if (wallpaper_style() != WP_PICTURE) setting_set("wallpaper_style", "solid");
        desktop_wallpaper_invalidate();
    } else if (!strcmp(nm, "volume")) {
        setting_set_int("volume", iv);
        if (!setting_get_int("mute", 0)) audio_set_volume(iv);
    } else if (!strcmp(nm, "mute")) {
        setting_set_int("mute", iv);
        audio_set_volume(iv ? 0 : setting_get_int("volume", 80));
    } else if (!strcmp(nm, "voice_volume") || !strcmp(nm, "ai_web")) {
        setting_set_int(nm, iv);
        desktop_send_host_prefs();
    } else if (!strcmp(nm, "mouse_speed")) {
        setting_set_int("mouse_speed", iv + 1);
        desktop_apply_settings();
    } else if (!strcmp(nm, "mouse_swap")) {
        setting_set_int(nm, iv);
        desktop_apply_settings();
    } else if (!strcmp(nm, "clock_24h")) {
        sys_set_clock_24h(iv);
        setting_set_int(nm, iv);
    } else if (!strcmp(nm, "airplane")) {
        net_set_offline(iv);
        setting_set_int(nm, iv);
    } else if (!strcmp(nm, "ai_ask_first")) {
        ai_tools_set_ask_first(iv);
        setting_set_int(nm, iv);
    } else if (!strcmp(nm, "device_name")) {
        if (!s[0] || strlen(s) > 30) { snprintf(msg, n, "The PC name must be 1 to 30 characters"); return -1; }
        setting_set(nm, s);
    } else if (p->kind == PK_TEXT) {
        setting_set(nm, s);
    } else {
        setting_set_int(nm, iv);              /* read where it is used */
    }
    desktop_invalidate();

    char now[96];
    prefs_get(nm, now, sizeof(now));
    snprintf(msg, n, "%s is now %s", nm, now);
    return 0;
}

/* Current value as text. Returns -1 for an unknown name. */
int prefs_get(const char *name, char *out, size_t n)
{
    const pref_t *p = pref_find(name);
    if (!p) return -1;
    const char *nm = p->name;
    if (!strcmp(nm, "resolution")) { snprintf(out, n, "%ux%u", fb_get_width(), fb_get_height()); return 0; }
    if (!strcmp(nm, "wallpaper")) {
        int st = wallpaper_style();
        if (st == WP_PICTURE) setting_get("wallpaper_picture", out, n);
        else snprintf(out, n, "%s", wallpaper_style_name(st));
        return 0;
    }
    if (!strcmp(nm, "theme")) { snprintf(out, n, "%s", PO_THEME[theme_get_mode()]); return 0; }
    if (!strcmp(nm, "bloom_color")) { int i = setting_get_int("wallpaper", 0); snprintf(out, n, "%s", PO_BLOOM[i >= 0 && i < 6 ? i : 0]); return 0; }
    if (!strcmp(nm, "mouse_speed")) { int i = setting_get_int("mouse_speed", 2) - 1; snprintf(out, n, "%s", PO_MOUSE[i >= 0 && i < 4 ? i : 1]); return 0; }
    if (!strcmp(nm, "wallpaper_color")) { char b[16]; setting_get(nm, b, sizeof(b)); snprintf(out, n, "%s", b[0] ? b : "#204060"); return 0; }
    if (!strcmp(nm, "device_name")) { char b[48]; setting_get(nm, b, sizeof(b)); snprintf(out, n, "%s", b[0] ? b : "JARVIS-PC"); return 0; }
    switch (p->kind) {
    case PK_BOOL: snprintf(out, n, "%s", setting_get_int(nm, p->dflt) ? "on" : "off"); break;
    case PK_INT:  snprintf(out, n, "%d", setting_get_int(nm, p->dflt)); break;
    case PK_ENUM: { int i = setting_get_int(nm, p->dflt); snprintf(out, n, "%s", p->opts[i >= 0 && i < p->nopts ? i : 0]); break; }
    case PK_TEXT: setting_get(nm, out, n); break;
    }
    return 0;
}

/* "name = value  (allowed)  help" lines for every setting. */
void prefs_describe(char *out, size_t n, int with_help)
{
    out[0] = 0;
    for (int i = 0; i < PREF_COUNT; i++) {
        const pref_t *p = &g_prefs[i];
        char v[96], allowed[160] = "";
        prefs_get(p->name, v, sizeof(v));
        if (p->kind == PK_BOOL) snprintf(allowed, sizeof(allowed), "on/off");
        else if (p->kind == PK_INT) snprintf(allowed, sizeof(allowed), "%d-%d", p->lo, p->hi);
        else if (p->kind == PK_ENUM)
            for (int k = 0; k < p->nopts; k++)
                snprintf(allowed + strlen(allowed), sizeof(allowed) - strlen(allowed), "%s%s", k ? "/" : "", p->opts[k]);
        size_t len = strlen(out);
        if (with_help)
            snprintf(out + len, n - len, "%s = %s%s%s%s  - %s\n", p->name, v, allowed[0] ? "  (" : "", allowed, allowed[0] ? ")" : "", p->help);
        else
            snprintf(out + len, n - len, "%s=%s%s%s%s\n", p->name, v, allowed[0] ? " (" : "", allowed, allowed[0] ? ")" : "");
    }
}

/* Called twice a second: follows the night-light schedule. */
static void prefs_tick(void)
{
    static int last = -1;
    int now = prefs_night_now();
    if (now != last) { last = now; prefs_apply_display(); }
}
