/*
 * =============================================================================
 * JARVIS OS — Device Manager (apps/devices.c)
 *
 * Every device in the machine, from the kernel's device table
 * (kernel/devmgr.c), grouped by type or by connection (platform, PCI bus,
 * USB, disks). For the selected device:
 *
 *   General     status, maker, hardware ID, location
 *   Driver      driver name, provider, version, file
 *   Resources   interrupt line and count, I/O and memory ranges
 *   Events      what happened to it since start (started, disabled ...)
 *
 * Actions (buttons, right-click menu, Terminal "device", JARVIS
 * device_action) act on the hardware for real:
 *   Disable / Enable   network adapter stops all traffic; sound card stops
 *                      and mutes; USB devices are safely stopped
 *   Restart            resets the adapter / codec and programs it again;
 *                      USB devices are stopped and set up again
 *   Uninstall          removes the driver binding until "Scan for hardware
 *                      changes" (or the next start) reinstalls it
 *   Scan               re-detects every device
 * Devices the system cannot run without (keyboard, mouse, display, the
 * system disk, timer) cannot be disabled, restarted or uninstalled.
 * =============================================================================
 */

#define DV_LIST_W   380
#define DV_ROW_H    22
#define DV_HEAD_H   24
#define DV_TOOL_H   40

static const char *const dv_categories[] = {
    "Audio", "Display", "Input", "Network", "Storage", "USB", "Wireless", "System", "Other"
};
#define DV_NCAT ((int)(sizeof(dv_categories) / sizeof(dv_categories[0])))
static const char *const dv_buses[] = { "Built into the computer", "PCI bus", "USB", "Disks" };
#define DV_NBUS 4

enum { DVT_GENERAL, DVT_DRIVER, DVT_RESOURCES, DVT_EVENTS, DVT_COUNT };
static const char *const dv_tab_names[DVT_COUNT] = { "General", "Driver", "Resources", "Events" };

static struct {
    wm_window_t *win;
    dev_info_t   devs[48];
    int          count;
    int          sel_id;            /* device id, -1 none */
    int          rows_dev[72];      /* list row -> index in devs, -1-group = header */
    int          rows;
    int          collapsed[16];
    int          scroll;
    int          by_connection, show_hidden;
    int          tab;
    char         msg[120];
    uint32_t     msg_color;
} dv;

static const char *dv_state_text(const dev_info_t *d)
{
    switch (d->state) {
    case DEV_WORKING:   return "This device is working properly.";
    case DEV_NO_DRIVER: return !strcmp(d->note, "driver uninstalled") ? "The driver was uninstalled. Scan for hardware changes to reinstall it."
                                                                      : "No driver is installed for this device.";
    case DEV_DISABLED:  return "This device is disabled.";
    default:            return "This device is not working (its driver failed to start).";
    }
}

static uint32_t dv_state_color(const dev_info_t *d)
{
    return d->state == DEV_WORKING ? T(OK) : d->state == DEV_DISABLED ? T(TEXT_DIM)
         : d->state == DEV_NO_DRIVER ? T(WARN) : T(DANGER);
}

static int dv_hidden(const dev_info_t *d)
{
    return !strcmp(d->note, "safe to unplug") || !strcmp(d->name, "System bridge");
}

static int dv_group_of(const dev_info_t *d)
{
    if (dv.by_connection) {
        if (!strncmp(d->location, "PCI", 3)) return 1;
        if (!strncmp(d->location, "USB", 3)) return 2;
        if (!strcmp(d->driver, "block")) return 3;
        return 0;
    }
    for (int c = 0; c < DV_NCAT; c++)
        if (!strcmp(d->type, dv_categories[c])) return c;
    return DV_NCAT - 1;
}

static const char *dv_group_name(int g) { return dv.by_connection ? dv_buses[g] : dv_categories[g]; }
static int dv_ngroups(void) { return dv.by_connection ? DV_NBUS : DV_NCAT; }

static void dv_refresh(void)
{
    dv.count = devmgr_list(dv.devs, 48);
    dv.rows = 0;
    for (int c = 0; c < dv_ngroups() && dv.rows < 72; c++) {
        int any = 0;
        for (int i = 0; i < dv.count; i++) if (dv_group_of(&dv.devs[i]) == c && (dv.show_hidden || !dv_hidden(&dv.devs[i]))) any = 1;
        if (!any) continue;
        dv.rows_dev[dv.rows++] = -1 - c;
        if (dv.collapsed[c + (dv.by_connection ? 10 : 0)]) continue;
        for (int i = 0; i < dv.count && dv.rows < 72; i++)
            if (dv_group_of(&dv.devs[i]) == c && (dv.show_hidden || !dv_hidden(&dv.devs[i]))) dv.rows_dev[dv.rows++] = i;
    }
    if (dv.scroll >= dv.rows) dv.scroll = dv.rows ? dv.rows - 1 : 0;
}

static dev_info_t *dv_selected(void)
{
    for (int i = 0; i < dv.count; i++) if (dv.devs[i].id == dv.sel_id) return &dv.devs[i];
    return NULL;
}

static void dv_say(uint32_t color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dv.msg, sizeof(dv.msg), fmt, ap);
    va_end(ap);
    dv.msg_color = color;
}

/* ---- actions (shared by buttons, menu and the Terminal) -------------------- */
enum { DVA_TOGGLE = 1, DVA_RESTART, DVA_UNINSTALL, DVA_SCAN, DVA_PROPS_DRIVER, DVA_PROPS_EVENTS };

static int dv_do(int action, int id, char *out, size_t n)
{
    char why[96], name[48] = "";
    static dev_info_t tmp[48];
    int cnt = devmgr_list(tmp, 48);
    for (int i = 0; i < cnt; i++) if (tmp[i].id == id) snprintf(name, sizeof(name), "%s", tmp[i].name);
    int rc = 0;
    switch (action) {
    case DVA_SCAN: {
        int c = devmgr_rescan();
        g_storage_generation++;
        snprintf(out, n, "Scan complete: %d devices found.", c);
        return 0;
    }
    case DVA_RESTART:
        rc = devmgr_restart(id, why, sizeof(why));
        if (rc) snprintf(out, n, "Could not restart %s: %s", name, why); else snprintf(out, n, "%s restarted.", name);
        return rc;
    case DVA_UNINSTALL:
        rc = devmgr_uninstall(id, why, sizeof(why));
        if (rc) snprintf(out, n, "Could not uninstall %s: %s", name, why);
        else snprintf(out, n, "Driver for %s uninstalled. Scan for hardware changes to reinstall it.", name);
        return rc;
    default:
        return -1;
    }
}

static int dv_set(int id, int on, char *out, size_t n)
{
    char why[96], name[48] = "";
    static dev_info_t tmp[48];
    int cnt = devmgr_list(tmp, 48), ctl = 0;
    for (int i = 0; i < cnt; i++) if (tmp[i].id == id) { snprintf(name, sizeof(name), "%s", tmp[i].name); ctl = tmp[i].control; }
    if (devmgr_set_enabled(id, on, why, sizeof(why)) == 0) {
        snprintf(out, n, "%s %s.", name, on ? "enabled" : ctl == DEVCTL_USB ? "stopped: safe to unplug" : "disabled");
        return 0;
    }
    snprintf(out, n, "Could not change %s: %s", name, why);
    return -1;
}

/* ---- drawing ---------------------------------------------------------------- */
static void dv_toolbar_btn(int x, int y, int w, const char *label, int on)
{
    fb_fill_rounded_rect(x, y, w, 28, 5, on ? T(ACCENT) : T(HOVER));
    ui_text_center(x, y + 6, w, label, on ? T(ACCENT_TEXT) : T(TEXT));
}

static void dv_draw_toolbar(int x, int y, int w)
{
    fb_fill_rect(x, y, w, DV_TOOL_H, T(SURFACE));
    fb_fill_rect(x, y + DV_TOOL_H - 1, w, 1, T(BORDER));
    fb_draw_text(x + 12, y + 12, "View:", T(TEXT_DIM));
    dv_toolbar_btn(x + 56, y + 6, 80, "By type", !dv.by_connection);
    dv_toolbar_btn(x + 140, y + 6, 116, "By connection", dv.by_connection);
    dv_toolbar_btn(x + 270, y + 6, 150, dv.show_hidden ? "Hide hidden devices" : "Show hidden devices", dv.show_hidden);
    dv_toolbar_btn(x + w - 212, y + 6, 200, "Scan for hardware changes", 0);
}

static void dv_draw_list(int x, int y, int h)
{
    char buf[64];
    fb_fill_rect(x, y, DV_LIST_W, h, T(SURFACE));
    fb_fill_rect(x + DV_LIST_W - 1, y, 1, h, T(BORDER));

    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x, y, DV_LIST_W, h - 40);
    int ry = y + 8;
    for (int r = dv.scroll; r < dv.rows; r++) {
        int i = dv.rows_dev[r];
        if (i < 0) {
            int c = -1 - i, bad = 0, col = dv.collapsed[c + (dv.by_connection ? 10 : 0)];
            fb_draw_text(x + 12, ry + 4, col ? SYM_RIGHT : SYM_DOWN, T(TEXT_DIM));
            fb_draw_text_bold(x + 28, ry + 4, dv_group_name(c), T(TEXT));
            for (int k = 0; k < dv.count; k++)
                if (dv_group_of(&dv.devs[k]) == c && dv.devs[k].state != DEV_WORKING && !dv_hidden(&dv.devs[k])) bad = 1;
            if (col && bad) fb_fill_circle(x + 40 + fb_text_width_bold(dv_group_name(c)), ry + 12, 4, T(WARN));
            ry += DV_HEAD_H;
            continue;
        }
        dev_info_t *d = &dv.devs[i];
        uint32_t fg = T(TEXT);
        if (d->id == dv.sel_id) {
            fb_fill_rounded_rect(x + 8, ry, DV_LIST_W - 16, DV_ROW_H - 2, 4, T(ACCENT));
            fg = T(ACCENT_TEXT);
        }
        fb_fill_circle(x + 30, ry + 10, 4, dv_state_color(d));
        ui_text_fit(x + 42, ry + 3, DV_LIST_W - 60, d->name, (d->state == DEV_DISABLED || dv_hidden(d)) && d->id != dv.sel_id ? T(TEXT_DIM) : fg);
        ry += DV_ROW_H;
    }
    fb_restore_clip(saved);

    int problems = 0;
    for (int i = 0; i < dv.count; i++) if (dv.devs[i].state != DEV_WORKING && !dv_hidden(&dv.devs[i])) problems++;
    snprintf(buf, sizeof(buf), "%d devices, %d need attention", dv.count, problems);
    fb_draw_text(x + 12, y + h - 28, buf, T(TEXT_DIM));
}

static void dv_row(int x, int *y, const char *label, const char *value)
{
    fb_draw_text(x, *y, label, T(TEXT_DIM));
    fb_draw_text(x + 150, *y, value, T(TEXT));
    *y += 22;
}

#define DV_BTN_Y(h) ((h) - 46)

static void dv_draw_details(int x, int y, int w, int h)
{
    char buf[160];
    int px = x + 20, pw = w - 40;
    dev_info_t *d = dv_selected();

    if (!d) {
        fb_draw_text_scaled(px, y + 14, "Device Manager", T(TEXT), 2);
        fb_draw_text(px, y + 60, "Select a device to see its details and what you can do with it.", T(TEXT_DIM));
        fb_draw_text(px, y + 84, "Right-click a device for its actions.", T(TEXT_DIM));
        if (dv.msg[0]) ui_text_fit(px, y + h - 40, pw, dv.msg, dv.msg_color);
        return;
    }
    fb_draw_text_bold(px, y + 16, d->name, T(TEXT));
    fb_fill_circle(px + 5, y + 46, 5, dv_state_color(d));
    ui_text_fit(px + 18, y + 39, pw - 18, dv_state_text(d), T(TEXT));

    /* tabs */
    int tx = px, ty = y + 66;
    for (int t = 0; t < DVT_COUNT; t++) {
        int tw = fb_text_width(dv_tab_names[t]) + 24;
        if (t == dv.tab) fb_fill_rect(tx + 8, ty + 26, tw - 16, 3, T(ACCENT));
        fb_draw_text(tx + 12, ty + 6, dv_tab_names[t], t == dv.tab ? T(TEXT) : T(TEXT_DIM));
        tx += tw;
    }
    fb_fill_rect(px, ty + 30, pw, 1, T(BORDER));
    int ry = ty + 44;

    switch (dv.tab) {
    case DVT_GENERAL:
        dv_row(px, &ry, "Device type", d->type);
        if (d->vendor[0]) snprintf(buf, sizeof(buf), "%s (%04x)", d->vendor, d->vendor_id);
        else if (d->vendor_id) snprintf(buf, sizeof(buf), "Unknown vendor (%04x)", d->vendor_id);
        else snprintf(buf, sizeof(buf), "(standard system device)");
        dv_row(px, &ry, "Manufacturer", buf);
        if (d->vendor_id || d->device_id) { snprintf(buf, sizeof(buf), "%04x:%04x", d->vendor_id, d->device_id); dv_row(px, &ry, "Hardware ID", buf); }
        dv_row(px, &ry, "Location", d->location);
        if (d->class_code || d->subclass) {
            snprintf(buf, sizeof(buf), "%02x / %02x / %02x", d->class_code, d->subclass, d->prog_if);
            dv_row(px, &ry, "PCI class", buf);
        }
        if (d->owner_pid) { snprintf(buf, sizeof(buf), "Process %u (exclusive)", d->owner_pid); dv_row(px, &ry, "In use by", buf); }
        if (d->note[0]) dv_row(px, &ry, "Note", d->note);
        break;
    case DVT_DRIVER: {
        static char info[400];
        devmgr_driver_info(d->id, info, sizeof(info));
        dv_row(px, &ry, "Driver", d->driver[0] ? d->driver : "None");
        char *p = info;
        int first = 1;
        while (p && *p) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            char *colon = strchr(p, ':');
            if (colon && !first) { *colon = 0; dv_row(px, &ry, p, colon + 2); }
            else { ui_text_fit(px, ry, pw, p, T(TEXT)); ry += 22; }
            first = 0;
            p = e ? e + 1 : NULL;
        }
        ry += 6;
        fb_draw_text(px, ry, "Drivers are built into the JARVIS OS kernel and started at boot.", T(TEXT_DIM));
        break;
    }
    case DVT_RESOURCES: {
        int any = 0;
        if (d->irq >= 0) {
            snprintf(buf, sizeof(buf), "IRQ %d  (%u interrupts since start)", d->irq, (unsigned)d->interrupts);
            dv_row(px, &ry, "Interrupt", buf);
            any = 1;
        }
        for (int b = 0; b < 6; b++) {
            uint32_t v = d->bar[b];
            if (!v) continue;
            if (v & 1) { snprintf(buf, sizeof(buf), "%04x", v & ~3u); dv_row(px, &ry, "I/O range from", buf); }
            else { snprintf(buf, sizeof(buf), "%08x%s", v & ~15u, (v & 6) == 4 ? " (64-bit)" : ""); dv_row(px, &ry, "Memory range at", buf); }
            any = 1;
            if ((v & 7) == 4) b++;
        }
        if (!any) fb_draw_text(px, ry, "This device uses no interrupt or address ranges of its own.", T(TEXT_DIM));
        break;
    }
    case DVT_EVENTS: {
        static char ev[3000];
        int n = devmgr_events(d->id, ev, sizeof(ev));
        if (!n) { fb_draw_text(px, ry, "No events for this device since start.", T(TEXT_DIM)); break; }
        for (char *p = ev; *p && ry < y + DV_BTN_Y(h) - 30;) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            ui_text_fit(px, ry, pw, p, T(TEXT));
            ry += 20;
            if (!e) break;
            p = e + 1;
        }
        break;
    }
    }

    int by = y + DV_BTN_Y(h);
    fb_fill_rect(x, by - 12, w, 1, T(BORDER));
    if (d->control != DEVCTL_NONE) {
        const char *label = d->state == DEV_DISABLED ? "Enable device" : d->control == DEVCTL_USB ? "Stop (eject)" : "Disable device";
        ui_button(px, by, 140, 30, label, d->state == DEV_DISABLED ? T(ACCENT) : T(HOVER), d->state == DEV_DISABLED ? T(ACCENT_TEXT) : T(TEXT));
        ui_button(px + 150, by, 130, 30, "Restart device", T(HOVER), T(TEXT));
        ui_button(px + 290, by, 140, 30, "Uninstall device", T(DANGER), 0x00FFFFFF);
    } else {
        fb_draw_text(px, by + 8, "Needed by the system: it cannot be disabled or removed.", T(TEXT_DIM));
    }
    if (dv.msg[0]) ui_text_fit(px, by - 36, pw, dv.msg, dv.msg_color);
}

void devices_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    dv_refresh();
    fb_fill_rect(x, y, w, h, T(WINDOW));
    dv_draw_toolbar(x, y, w);
    dv_draw_list(x, y + DV_TOOL_H, h - DV_TOOL_H);
    dv_draw_details(x + DV_LIST_W, y + DV_TOOL_H, w - DV_LIST_W, h - DV_TOOL_H);
}

static void dv_menu_cb(int id, void *ctx)
{
    (void)ctx;
    dev_info_t *d = dv_selected();
    if (!d) return;
    char out[120];
    int rc;
    switch (id) {
    case DVA_TOGGLE: rc = dv_set(d->id, d->state == DEV_DISABLED, out, sizeof(out)); break;
    case DVA_PROPS_DRIVER: dv.tab = DVT_DRIVER; return;
    case DVA_PROPS_EVENTS: dv.tab = DVT_EVENTS; return;
    default: rc = dv_do(id, d->id, out, sizeof(out)); break;
    }
    dv_say(rc ? T(DANGER) : T(OK), "%s", out);
    desktop_invalidate();
}

static void dv_select_row_at(int my, int open_menu, int sx, int sy)
{
    int ry = 8;
    for (int r = dv.scroll; r < dv.rows; r++) {
        int rh = dv.rows_dev[r] < 0 ? DV_HEAD_H : DV_ROW_H;
        if (my >= ry && my < ry + rh) {
            if (dv.rows_dev[r] >= 0) {
                dev_info_t *d = &dv.devs[dv.rows_dev[r]];
                dv.sel_id = d->id;
                dv.msg[0] = 0;
                if (open_menu) {
                    int ctl = d->control != DEVCTL_NONE;
                    ui_menu_item_t items[6] = {
                        { d->state == DEV_DISABLED ? "Enable device" : "Disable device", DVA_TOGGLE, !ctl, 0 },
                        { "Restart device", DVA_RESTART, !ctl, 0 },
                        { "Uninstall device", DVA_UNINSTALL, !ctl, 0 },
                        { "Scan for hardware changes", DVA_SCAN, 0, 1 },
                        { "Driver details", DVA_PROPS_DRIVER, 0, 1 },
                        { "Events", DVA_PROPS_EVENTS, 0, 0 },
                    };
                    ui_menu_open(sx, sy, items, 6, dv_menu_cb, NULL);
                }
            } else if (!open_menu) {
                int c = -1 - dv.rows_dev[r] + (dv.by_connection ? 10 : 0);
                dv.collapsed[c] = !dv.collapsed[c];
                dv_refresh();
            }
            return;
        }
        ry += rh;
    }
}

void devices_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    h -= DV_TOOL_H;
    if (event->type == IE_MOUSE_WHEEL) {
        dv.scroll += event->data.mouse.button > 0 ? -3 : 3;
        if (dv.scroll < 0) dv.scroll = 0;
        if (dv.scroll >= dv.rows) dv.scroll = dv.rows ? dv.rows - 1 : 0;
        return;
    }
    if (event->type == IE_KEY_DOWN) {
        uint32_t k = event->data.keyboard.key;
        if (k == WM_KEY_PGUP) { dv.scroll = dv.scroll > 8 ? dv.scroll - 8 : 0; return; }
        if (k == WM_KEY_PGDN) { dv.scroll = dv.scroll + 8 < dv.rows ? dv.scroll + 8 : dv.scroll; return; }
        if (k == WM_KEY_F1 + 4) { dv_menu_cb(DVA_SCAN, NULL); return; }      /* F5: scan */
        if (k == WM_KEY_DELETE && dv_selected()) { dv_menu_cb(DVA_UNINSTALL, NULL); return; }
        if (k != WM_KEY_UP && k != WM_KEY_DOWN) return;
        int cur = -1;
        for (int r = 0; r < dv.rows; r++)
            if (dv.rows_dev[r] >= 0 && dv.devs[dv.rows_dev[r]].id == dv.sel_id) cur = r;
        int step = k == WM_KEY_DOWN ? 1 : -1;
        for (int r = cur + step; r >= 0 && r < dv.rows; r += step)
            if (dv.rows_dev[r] >= 0) {
                dv.sel_id = dv.devs[dv.rows_dev[r]].id;
                int visible = (h - 60) / DV_ROW_H;
                if (r < dv.scroll) dv.scroll = r;
                if (r >= dv.scroll + visible) dv.scroll = r - visible + 1;
                break;
            }
        return;
    }
    if (event->type != IE_MOUSE_DOWN) return;
    int mx = event->data.mouse.x, my = event->data.mouse.y;
    int btn = event->data.mouse.button;

    if (my < DV_TOOL_H) {                                   /* toolbar */
        if (btn != 1) return;
        if (ui_hit(mx, my, 56, 6, 80, 28)) { dv.by_connection = 0; dv.scroll = 0; }
        else if (ui_hit(mx, my, 140, 6, 116, 28)) { dv.by_connection = 1; dv.scroll = 0; }
        else if (ui_hit(mx, my, 270, 6, 150, 28)) dv.show_hidden = !dv.show_hidden;
        else if (ui_hit(mx, my, w - 212, 6, 200, 28)) dv_menu_cb(DVA_SCAN, NULL);
        dv_refresh();
        return;
    }
    my -= DV_TOOL_H;
    if (mx < DV_LIST_W) {
        dv_select_row_at(my, btn == 2, x + mx, y + my + DV_TOOL_H);
        return;
    }
    if (btn != 1) return;
    /* tabs */
    int px = DV_LIST_W + 20, tx = px;
    for (int t = 0; t < DVT_COUNT; t++) {
        int tw = fb_text_width(dv_tab_names[t]) + 24;
        if (ui_hit(mx, my, tx, 66, tw, 30)) { dv.tab = t; return; }
        tx += tw;
    }
    int by = DV_BTN_Y(h);
    dev_info_t *d = dv_selected();
    if (!d || d->control == DEVCTL_NONE) return;
    if (ui_hit(mx, my, px, by, 140, 30)) dv_menu_cb(DVA_TOGGLE, NULL);
    else if (ui_hit(mx, my, px + 150, by, 130, 30)) dv_menu_cb(DVA_RESTART, NULL);
    else if (ui_hit(mx, my, px + 290, by, 140, 30)) dv_menu_cb(DVA_UNINSTALL, NULL);
}

wm_window_t *devices_app_open(int x, int y, int w, int h)
{
    memset(&dv, 0, sizeof(dv));
    dv.sel_id = -1;
    dv.collapsed[DV_NCAT - 2] = 1;                          /* System: collapsed, as in Windows */
    dv_refresh();
    wm_window_t *win = wm_create_window("Device Manager", x, y, w, h, WS_DEFAULT,
                                        devices_draw_callback, devices_event_callback);
    if (!win) return NULL;
    win->icon = ICON_DEVICES;
    win->min_w = 820;
    win->min_h = 480;
    dv.win = win;
    return win;
}

/* ---- Terminal: device <action> <#id or name> --------------------------------- */
int shell_device_command(int argc, char **argv)
{
    static char out[3000];
    if (argc < 2 || !strcmp(argv[1], "help")) {
        sh_printf("usage: device list | info <dev> | enable <dev> | disable <dev> | restart <dev> |\n"
                  "              uninstall <dev> | scan | events [dev] | driver <dev>\n"
                  "  <dev> is the #number from 'devices', or part of the name (network, sound, usb ...)\n");
        return argc < 2;
    }
    const char *act = argv[1];
    if (!strcmp(act, "list")) { char *a[1] = { (char *)"devices" }; return c_devices(1, a); }
    if (!strcmp(act, "scan") || !strcmp(act, "rescan")) { dv_do(DVA_SCAN, -1, out, sizeof(out)); sh_printf("%s\n", out); return 0; }
    if (!strcmp(act, "events") && argc < 3) { devmgr_events(-1, out, sizeof(out)); sh_out(out); return 0; }
    if (argc < 3) { sh_err("device %s: which device? (see: devices)\n", act); return 1; }
    char what[96] = "";
    for (int i = 2; i < argc; i++) snprintf(what + strlen(what), sizeof(what) - strlen(what), "%s%s", i > 2 ? " " : "", argv[i]);
    const char *q = what[0] == '#' ? what + 1 : what;
    int id = devmgr_find(q);
    if (id < 0) { sh_err("device: no device matches '%s' (see: devices)\n", what); return 1; }
    int rc = 0;
    if (!strcmp(act, "enable") || !strcmp(act, "disable")) rc = dv_set(id, act[0] == 'e', out, sizeof(out));
    else if (!strcmp(act, "restart") || !strcmp(act, "reset")) rc = dv_do(DVA_RESTART, id, out, sizeof(out));
    else if (!strcmp(act, "uninstall") || !strcmp(act, "remove")) rc = dv_do(DVA_UNINSTALL, id, out, sizeof(out));
    else if (!strcmp(act, "driver")) { devmgr_driver_info(id, out, sizeof(out)); }
    else if (!strcmp(act, "events")) { if (!devmgr_events(id, out, sizeof(out))) snprintf(out, sizeof(out), "No events for this device."); }
    else if (!strcmp(act, "info") || !strcmp(act, "details")) {
        static dev_info_t d[48];
        int n = devmgr_list(d, 48);
        static const char *const st[4] = { "working", "no driver", "not working", "disabled" };
        for (int i = 0; i < n; i++) {
            if (d[i].id != id) continue;
            int o = snprintf(out, sizeof(out), "#%d %s\n  Type: %s\n  Status: %s\n  Maker: %s\n  Hardware ID: %04x:%04x\n  Location: %s\n  Driver: %s\n",
                             d[i].id, d[i].name, d[i].type, st[d[i].state & 3], d[i].vendor[0] ? d[i].vendor : "-", d[i].vendor_id, d[i].device_id,
                             d[i].location, d[i].driver[0] ? d[i].driver : "none");
            if (d[i].irq >= 0) o += snprintf(out + o, sizeof(out) - (size_t)o, "  Interrupt: IRQ %d, %u raised\n", d[i].irq, (unsigned)d[i].interrupts);
            snprintf(out + o, sizeof(out) - (size_t)o, "  Can be disabled: %s\n", d[i].control ? "yes" : "no (needed by the system)");
        }
    } else { sh_err("device: unknown action %s (see: device help)\n", act); return 1; }
    if (rc) sh_err("%s\n", out);
    else { sh_out(out); if (out[0] && out[strlen(out) - 1] != '\n') sh_out("\n"); }
    desktop_invalidate();
    return rc ? 1 : 0;
}
