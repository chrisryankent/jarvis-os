/*
 * =============================================================================
 * JARVIS OS — Registry Editor (apps/regedit.c)
 *
 * Browse and edit the registry (kernel/registry.c), like regedit.exe:
 *   - key tree on the left (click to select, click the arrow to expand)
 *   - values of the selected key on the right: Name, Type, Data
 *   - address bar: type a path (HKCU\Software\...) and press Enter
 *   - New key, New value (String, DWORD, QWORD, Binary, Multi-String,
 *     Expandable String), Modify (double-click), Rename, Delete
 *   - Find (key names, value names and data; F3 finds the next one)
 *   - Export the selected key to a .reg file, Import a .reg file
 * Values under HKCU\Software\JARVIS OS\Settings are the system settings:
 * changing one there applies it at once (checked like any other change).
 * Keys built from the hardware (HKLM\HARDWARE ...) are read-only.
 * =============================================================================
 */

#define RE_TREE_W  330
#define RE_ROW_H   22
#define RE_TOOL_H  44
#define RE_ADDR_H  34

enum { RD_NONE, RD_EDIT, RD_NEWKEY, RD_RENAME_KEY, RD_RENAME_VAL, RD_DELETE, RD_FIND, RD_EXPORT, RD_IMPORT, RD_NEWVAL_MENU };

static struct {
    wm_window_t *win;
    int sel_key;                    /* selected key, -1 = "Computer" */
    uint8_t expanded[1024];
    int computer_open;
    int tree_rows[1024], tree_depth[1024], ntree;    /* -1 = Computer */
    int tree_scroll, val_scroll;
    int sel_val;                    /* index in the value list, -1 none */
    char addr[300]; int addr_len, addr_focus;
    /* dialog */
    int dlg, field;
    int edit_type, edit_new, edit_hex;
    char f_name[64], f_data[600];
    char old_name[64];
    char msg[160]; uint32_t msg_color;
    uint32_t last_click_tick; int last_click_val;
    char find_text[64]; int find_from_key;
    uint32_t seen_gen;
} re;

static void re_say(uint32_t c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(re.msg, sizeof(re.msg), fmt, ap);
    va_end(ap);
    re.msg_color = c;
}

static void re_build_tree_rec(int k, int depth)
{
    if (re.ntree >= 1024) return;
    re.tree_rows[re.ntree] = k;
    re.tree_depth[re.ntree++] = depth;
    if (k >= 0 && !re.expanded[k]) return;
    for (int i = 0;; i++) {
        int c = reg_key_child(k, i);
        if (c < 0 || re.ntree >= 1024) break;
        re_build_tree_rec(c, depth + 1);
        reg_key_child(k, 0);            /* re-prime the per-parent child cache */
    }
}

static void re_build_tree(void)
{
    re.ntree = 0;
    re.tree_rows[re.ntree] = -1;
    re.tree_depth[re.ntree++] = 0;
    if (re.computer_open)
        for (int h = 0; h < REG_HIVES; h++) re_build_tree_rec(h, 1);
}

static void re_select(int k)
{
    re.sel_key = k;
    re.sel_val = -1;
    re.val_scroll = 0;
    if (k < 0) snprintf(re.addr, sizeof(re.addr), "Computer");
    else { char p[280]; reg_key_path(k, p, sizeof(p)); snprintf(re.addr, sizeof(re.addr), "Computer\\%s", p); }
    re.addr_len = (int)strlen(re.addr);
    for (int x = reg_key_parent(k); x >= 0; x = reg_key_parent(x)) re.expanded[x] = 1;   /* make it visible */
    re.computer_open = 1;
}

static const char *re_settings_path = "HKEY_CURRENT_USER\\Software\\JARVIS OS\\Settings";

static int re_is_settings_key(int k)
{
    char p[280];
    reg_key_path(k, p, sizeof(p));
    return !strcmp(p, re_settings_path);
}

/* ---- drawing ------------------------------------------------------------------ */
static void re_draw_tree(int x, int y, int h)
{
    fb_fill_rect(x, y, RE_TREE_W, h, T(SURFACE));
    fb_fill_rect(x + RE_TREE_W - 1, y, 1, h, T(BORDER));
    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x, y, RE_TREE_W - 1, h);
    int ry = y + 6;
    for (int i = re.tree_scroll; i < re.ntree && ry < y + h; i++, ry += RE_ROW_H) {
        int k = re.tree_rows[i], d = re.tree_depth[i];
        int tx = x + 10 + d * 16;
        int sel = k == re.sel_key;
        if (sel) fb_fill_rounded_rect(x + 4, ry, RE_TREE_W - 10, RE_ROW_H - 2, 4, T(ACCENT));
        int has = k < 0 ? 1 : reg_key_has_children(k);
        int open = k < 0 ? re.computer_open : re.expanded[k];
        uint32_t fg = sel ? T(ACCENT_TEXT) : T(TEXT);
        if (has) fb_draw_text(tx, ry + 3, open ? SYM_DOWN : SYM_RIGHT, sel ? fg : T(TEXT_DIM));
        if (k < 0) icon_draw(ICON_PC, tx + 14, ry + 3, 16);
        else {                                          /* folder glyph */
            fb_fill_rounded_rect(tx + 14, ry + 5, 16, 12, 2, reg_key_volatile(k) ? 0x0090A0B0 : 0x00F2C14E);
            fb_fill_rect(tx + 14, ry + 4, 7, 3, reg_key_volatile(k) ? 0x0090A0B0 : 0x00F2C14E);
        }
        fb_draw_text(tx + 36, ry + 3, k < 0 ? "Computer" : reg_key_name(k), fg);
    }
    fb_restore_clip(saved);
}

static void re_draw_values(int x, int y, int w, int h)
{
    fb_fill_rect(x, y, w, 28, T(SURFACE));
    fb_fill_rect(x, y + 27, w, 1, T(BORDER));
    int c1 = x + 16, c2 = x + 16 + (w - 32) * 35 / 100, c3 = x + 16 + (w - 32) * 55 / 100;
    fb_draw_text(c1, y + 6, "Name", T(TEXT_DIM));
    fb_draw_text(c2, y + 6, "Type", T(TEXT_DIM));
    fb_draw_text(c3, y + 6, "Data", T(TEXT_DIM));
    if (re.sel_key < 0) { fb_draw_text(c1, y + 40, "Select a key on the left.", T(TEXT_DIM)); return; }
    int ry = y + 34, shown = 0;
    int has_default = 0;
    char nm[64], text[256];
    int type;
    uint32_t len;
    for (int i = 0; reg_value_at(re.sel_key, i, nm, sizeof(nm), &type, text, sizeof(text), &len) == 0; i++)
        if (!nm[0]) has_default = 1;
    /* "(Default)" always shows, as on Windows */
    int row = 0;
    if (!has_default) {
        if (row >= re.val_scroll) {
            fb_draw_text(c1 + 22, ry + 3, "(Default)", T(TEXT));
            fb_draw_text(c2, ry + 3, "REG_SZ", T(TEXT_DIM));
            fb_draw_text(c3, ry + 3, "(value not set)", T(TEXT_DIM));
            fb_fill_rounded_rect(c1, ry + 6, 14, 10, 2, 0x00C04040);
            ry += RE_ROW_H;
        }
        row++;
    }
    for (int i = 0; reg_value_at(re.sel_key, i, nm, sizeof(nm), &type, text, sizeof(text), &len) == 0 && ry < y + h - 4; i++, row++) {
        if (row < re.val_scroll) continue;
        int sel = i == re.sel_val;
        if (sel) fb_fill_rounded_rect(x + 6, ry, w - 12, RE_ROW_H - 2, 4, T(ACCENT));
        uint32_t fg = sel ? T(ACCENT_TEXT) : T(TEXT), dim = sel ? T(ACCENT_TEXT) : T(TEXT_DIM);
        int stringy = type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ;
        fb_fill_rounded_rect(c1, ry + 6, 14, 10, 2, stringy ? 0x00C04040 : 0x003A7BD5);
        fb_draw_text(c1 + 4, ry + 4, stringy ? "" : "", fg);
        ui_text_fit(c1 + 22, ry + 3, c2 - c1 - 30, nm[0] ? nm : "(Default)", fg);
        fb_draw_text(c2, ry + 3, reg_type_name(type), dim);
        if (type == REG_DWORD || type == REG_QWORD) {
            char b[64];
            uint64_t v = 0;
            reg_get_raw_k(re.sel_key, nm, NULL, &v, 8);
            if (type == REG_DWORD) v &= 0xFFFFFFFFu;
            snprintf(b, sizeof(b), "0x%0*llx (%llu)", type == REG_DWORD ? 8 : 16, (unsigned long long)v, (unsigned long long)v);
            ui_text_fit(c3, ry + 3, x + w - c3 - 10, b, fg);
        } else {
            for (char *c = text; *c; c++) if (*c == '\n') *c = ' ';
            ui_text_fit(c3, ry + 3, x + w - c3 - 10, text, fg);
        }
        ry += RE_ROW_H;
        shown++;
    }
    (void)shown;
}

static const char *const re_tool_labels[] = { "New key", "New value", "Modify", "Rename", "Delete", "Find", "Export", "Import", "Refresh" };
#define RE_NTOOLS 9

static int re_tool_x(int i, int x)
{
    int cx = x + 10;
    for (int k = 0; k < i; k++) cx += fb_text_width(re_tool_labels[k]) + 24 + 6;
    return cx;
}

static void re_field(int x, int y, int w, const char *label, const char *val, int focus)
{
    fb_draw_text(x, y, label, T(TEXT_DIM));
    fb_fill_rounded_rect(x, y + 20, w, 30, 4, T(INPUT));
    fb_draw_rounded_rect(x, y + 20, w, 30, 4, focus ? T(ACCENT) : T(BORDER));
    ui_text_fit(x + 8, y + 27, w - 16, val, T(TEXT));
    if (focus) {
        int cw = fb_text_width(val);
        if (cw > w - 16) cw = w - 16;
        fb_fill_rect(x + 9 + cw, y + 27, 1, 16, T(TEXT));
    }
}

static void re_draw_dialog(int x, int y, int w, int h)
{
    if (!re.dlg) return;
    if (re.dlg == RD_NEWVAL_MENU) {
        static const char *const t[] = { "String value", "DWORD (32-bit) value", "QWORD (64-bit) value", "Binary value", "Multi-String value", "Expandable String value" };
        int mx = re_tool_x(1, x), my = y + RE_TOOL_H - 4;
        fb_blend_rounded_rect(mx + 3, my + 4, 220, 6 * 30 + 8, 8, 0, 60);
        fb_fill_rounded_rect(mx, my, 220, 6 * 30 + 8, 8, T(MENU));
        fb_draw_rounded_rect(mx, my, 220, 6 * 30 + 8, 8, T(BORDER));
        for (int i = 0; i < 6; i++) fb_draw_text(mx + 14, my + 10 + i * 30, t[i], T(TEXT));
        return;
    }
    fb_blend_rect(x, y, w, h, 0, 100);
    int dw = 520, dh = re.dlg == RD_DELETE ? 170 : re.dlg == RD_EDIT ? 270 : 200;
    int dx = x + (w - dw) / 2, dy = y + (h - dh) / 2;
    fb_fill_rounded_rect(dx, dy, dw, dh, 8, T(SURFACE));
    fb_draw_rounded_rect(dx, dy, dw, dh, 8, T(BORDER));
    static const char *const titles[] = { "", "Edit value", "New key", "Rename key", "Rename value", "Delete", "Find", "Export", "Import" };
    char b[160];
    const char *title = titles[re.dlg];
    if (re.dlg == RD_EDIT) {
        snprintf(b, sizeof(b), "%s %s", re.edit_new ? "New" : "Edit", re.edit_type == REG_DWORD ? "DWORD (32-bit) value" : re.edit_type == REG_QWORD ? "QWORD (64-bit) value" :
                 re.edit_type == REG_BINARY ? "binary value" : re.edit_type == REG_MULTI_SZ ? "multi-string value" : re.edit_type == REG_EXPAND_SZ ? "expandable string" : "string");
        title = b;
    }
    fb_draw_text_bold(dx + 20, dy + 16, title, T(TEXT));
    int fy = dy + 48, fw = dw - 40;
    switch (re.dlg) {
    case RD_EDIT:
        re_field(dx + 20, fy, fw, "Value name:", re.f_name[0] || re.edit_new ? re.f_name : "(Default)", re.field == 0);
        re_field(dx + 20, fy + 60, fw, re.edit_type == REG_MULTI_SZ ? "Value data (separate items with |):" :
                 re.edit_type == REG_BINARY ? "Value data (hex bytes, e.g. 0a ff 10):" : "Value data:", re.f_data, re.field == 1);
        if (re.edit_type == REG_DWORD || re.edit_type == REG_QWORD) {
            snprintf(b, sizeof(b), "Base:  %s  (click to switch)", re.edit_hex ? "Hexadecimal" : "Decimal");
            fb_draw_text(dx + 20, fy + 122, b, T(TEXT));
        }
        break;
    case RD_NEWKEY: case RD_RENAME_KEY: case RD_RENAME_VAL:
        re_field(dx + 20, fy, fw, re.dlg == RD_NEWKEY ? "Name of the new key:" : "New name:", re.f_name, 1);
        break;
    case RD_DELETE:
        if (re.sel_val >= 0) snprintf(b, sizeof(b), "Delete the value \"%s\"?", re.old_name[0] ? re.old_name : "(Default)");
        else snprintf(b, sizeof(b), "Delete the key \"%s\" and everything in it?", reg_key_name(re.sel_key));
        ui_text_fit(dx + 20, fy, fw, b, T(TEXT));
        fb_draw_text(dx + 20, fy + 24, "Changing the registry can change how JARVIS OS behaves.", T(TEXT_DIM));
        break;
    case RD_FIND:
        re_field(dx + 20, fy, fw, "Find what (key names, value names and data):", re.f_data, 1);
        break;
    case RD_EXPORT: case RD_IMPORT:
        re_field(dx + 20, fy, fw, re.dlg == RD_EXPORT ? "Save the selected key to file:" : ".reg file to import:", re.f_data, 1);
        break;
    }
    int danger = re.dlg == RD_DELETE;
    const char *ok = re.dlg == RD_DELETE ? "Delete" : re.dlg == RD_FIND ? "Find next" : re.dlg == RD_EXPORT ? "Export" : re.dlg == RD_IMPORT ? "Import" : "OK";
    ui_button(dx + dw - 240, dy + dh - 48, 110, 32, ok, danger ? T(DANGER) : T(ACCENT), danger ? 0x00FFFFFF : T(ACCENT_TEXT));
    ui_button(dx + dw - 120, dy + dh - 48, 100, 32, "Cancel", T(HOVER), T(TEXT));
}

void regedit_draw(wm_window_t *win, uint32_t *fb)
{
    (void)fb;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    if (re.seen_gen != g_reg_generation) { re.seen_gen = g_reg_generation; }
    re_build_tree();
    fb_fill_rect(x, y, w, h, T(WINDOW));
    /* toolbar */
    fb_fill_rect(x, y, w, RE_TOOL_H, T(SURFACE));
    for (int i = 0; i < RE_NTOOLS; i++) {
        int bx = re_tool_x(i, x), bw = fb_text_width(re_tool_labels[i]) + 24;
        fb_fill_rounded_rect(bx, y + 7, bw, 30, 5, T(HOVER));
        fb_draw_text(bx + 12, y + 14, re_tool_labels[i], T(TEXT));
    }
    /* address bar */
    int ay = y + RE_TOOL_H;
    fb_fill_rect(x, ay, w, RE_ADDR_H, T(WINDOW));
    fb_fill_rounded_rect(x + 10, ay + 3, w - 20, 28, 4, T(INPUT));
    fb_draw_rounded_rect(x + 10, ay + 3, w - 20, 28, 4, re.addr_focus ? T(ACCENT) : T(BORDER));
    ui_text_fit(x + 18, ay + 9, w - 36, re.addr, T(TEXT));
    if (re.addr_focus) fb_fill_rect(x + 19 + fb_text_width(re.addr), ay + 9, 1, 16, T(TEXT));
    fb_fill_rect(x, ay + RE_ADDR_H - 1, w, 1, T(BORDER));
    int by = ay + RE_ADDR_H, bh = h - (by - y) - 26;
    re_draw_tree(x, by, bh);
    re_draw_values(x + RE_TREE_W, by, w - RE_TREE_W, bh);
    /* status bar */
    int sy = y + h - 26;
    fb_fill_rect(x, sy, w, 26, T(SURFACE));
    fb_fill_rect(x, sy, w, 1, T(BORDER));
    if (re.msg[0]) ui_text_fit(x + 12, sy + 5, w - 24, re.msg, re.msg_color);
    else if (re.sel_key >= 0 && reg_key_volatile(re.sel_key)) fb_draw_text(x + 12, sy + 5, "Built from the hardware when JARVIS OS starts: read-only, not saved.", T(TEXT_DIM));
    else if (re.sel_key >= 0 && re_is_settings_key(re.sel_key)) fb_draw_text(x + 12, sy + 5, "System settings: a change here applies immediately.", T(TEXT_DIM));
    re_draw_dialog(x, y, w, h);
}

/* ---- actions -------------------------------------------------------------------- */
static void re_key_path_sel(char *p, size_t n) { reg_key_path(re.sel_key, p, n); }

static int re_writable(void)
{
    if (re.sel_key < 0) { re_say(T(WARN), "Select a key first."); return 0; }
    if (reg_key_volatile(re.sel_key)) { re_say(T(WARN), "This key is built from the hardware and cannot be changed."); return 0; }
    return 1;
}

static int re_value_name_at(int i, char *nm, size_t n, int *type)
{
    return reg_value_at(re.sel_key, i, nm, n, type, NULL, 0, NULL);
}


static void re_open_edit(int new_type)
{
    char nm[64] = "", text[600] = "";
    int type = new_type;
    if (!new_type) {
        if (re.sel_val < 0 || re_value_name_at(re.sel_val, nm, sizeof(nm), &type) != 0) { re_say(T(WARN), "Select a value to modify."); return; }
        reg_get_text_k(re.sel_key, nm, text, sizeof(text));
        if (type == REG_MULTI_SZ) for (char *c = text; *c; c++) if (*c == '\n') *c = '|';
    }
    if (!re_writable()) return;
    re.dlg = RD_EDIT;
    re.edit_type = type;
    re.edit_new = new_type != 0;
    re.edit_hex = 0;
    re.field = re.edit_new ? 0 : 1;
    snprintf(re.f_name, sizeof(re.f_name), "%s", re.edit_new ? (type == REG_DWORD || type == REG_QWORD ? "New Value #1" : "New Value #1") : nm);
    snprintf(re.old_name, sizeof(re.old_name), "%s", nm);
    snprintf(re.f_data, sizeof(re.f_data), "%s", re.edit_new && (type == REG_DWORD || type == REG_QWORD) ? "0" : text);
}

static void re_find_next(void)
{
    if (!re.find_text[0]) return;
    /* walk keys in tree order starting after the selected key */
    static int order[1024];
    int n = 0;
    int stack[64], sp = 0;
    for (int h = REG_HIVES - 1; h >= 0; h--) stack[sp++] = h;
    while (sp && n < 1024) {
        int k = stack[--sp];
        order[n++] = k;
        int kids[256], nk = 0;
        for (int i = 0; nk < 256; i++) { int c = reg_key_child(k, i); if (c < 0) break; kids[nk++] = c; }
        for (int i = nk - 1; i >= 0 && sp < 64; i--) stack[sp++] = kids[i];
    }
    int start = 0;
    for (int i = 0; i < n; i++) if (order[i] == re.sel_key) start = i;
    int startval = re.sel_val + 1;
    for (int pass = 0; pass < n + 1; pass++) {
        int idx = (start + pass) % n, k = order[idx];
        if (pass > 0 && strstr_ci(reg_key_name(k), re.find_text)) { re_select(k); re_say(T(OK), "Found the key %s.", reg_key_name(k)); return; }
        char nm[64], text[256];
        int type;
        for (int i = pass == 0 ? startval : 0; reg_value_at(k, i, nm, sizeof(nm), &type, text, sizeof(text), NULL) == 0; i++)
            if (strstr_ci(nm, re.find_text) || strstr_ci(text, re.find_text)) {
                re_select(k);
                re.sel_val = i;
                re_say(T(OK), "Found in %s: %s", reg_key_name(k), nm[0] ? nm : "(Default)");
                return;
            }
    }
    re_say(T(WARN), "Finished searching the registry: \"%s\" was not found.", re.find_text);
}

static void re_dialog_ok(void)
{
    char path[300], err[160];
    re_key_path_sel(path, sizeof(path));
    switch (re.dlg) {
    case RD_EDIT: {
        if (!re.f_name[0] && re.edit_new) { re_say(T(DANGER), "Give the value a name."); return; }
        char data[600];
        snprintf(data, sizeof(data), "%s", re.f_data);
        if (re.edit_type == REG_MULTI_SZ) for (char *c = data; *c; c++) if (*c == '|') *c = '\n';
        if ((re.edit_type == REG_DWORD || re.edit_type == REG_QWORD) && re.edit_hex && strncmp(data, "0x", 2)) {
            char t[600]; snprintf(t, sizeof(t), "0x%s", data); snprintf(data, sizeof(data), "%s", t);
        }
        /* a system setting goes through the settings checks and applies now */
        if (re_is_settings_key(re.sel_key) && !re.edit_new) {
            char msg[200];
            if (prefs_set(re.f_name, data, msg, sizeof(msg)) == 0) { re_say(T(OK), "%s (applied).", msg); re.dlg = RD_NONE; return; }
            if (strncmp(msg, "Unknown setting", 15)) { re_say(T(DANGER), "%s", msg); return; }
        }
        if (!re.edit_new && strcmp(re.old_name, re.f_name)) reg_delete_value(path, re.old_name);
        if (reg_set_from_text(re.sel_key, re.f_name, re.edit_type, data, err, sizeof(err)) != 0) { re_say(T(DANGER), "Cannot save: %s", err); return; }
        re_say(T(OK), "Saved %s.", re.f_name[0] ? re.f_name : "(Default)");
        break;
    }
    case RD_NEWKEY: {
        if (!re.f_name[0] || strchr(re.f_name, '\\')) { re_say(T(DANGER), "Key names cannot be empty or contain \\."); return; }
        char np[400];
        snprintf(np, sizeof(np), "%s\\%s", path, re.f_name);
        if (reg_open_key(np, 0) >= 0) { re_say(T(DANGER), "A key called %s already exists here.", re.f_name); return; }
        int k = reg_open_key(np, 1);
        if (k < 0) { re_say(T(DANGER), "The registry is full."); return; }
        re.expanded[re.sel_key] = 1;
        re_select(k);
        re_say(T(OK), "Key created.");
        break;
    }
    case RD_RENAME_KEY:
        if (reg_rename_key(path, re.f_name) != 0) { re_say(T(DANGER), "Cannot rename: the name is empty, has \\, or already exists."); return; }
        re_select(re.sel_key);
        re_say(T(OK), "Key renamed.");
        break;
    case RD_RENAME_VAL:
        if (reg_rename_value(path, re.old_name, re.f_name) != 0) { re_say(T(DANGER), "Cannot rename: that name already exists here."); return; }
        re_say(T(OK), "Value renamed.");
        break;
    case RD_DELETE:
        if (re.sel_val >= 0) {
            reg_delete_value(path, re.old_name);
            re.sel_val = -1;
            re_say(T(OK), "Value deleted.");
        } else {
            int parent = reg_key_parent(re.sel_key);
            reg_delete_key(path);
            re_select(parent);
            re_say(T(OK), "Key deleted.");
        }
        break;
    case RD_FIND:
        snprintf(re.find_text, sizeof(re.find_text), "%s", re.f_data);
        re.dlg = RD_NONE;
        re_find_next();
        return;
    case RD_EXPORT: {
        size_t cap = 512 * 1024;
        char *b = (char *)kmalloc_large(cap);
        char fp[256];
        sh_path(re.f_data, fp, sizeof(fp));
        int n = b ? reg_export(path, b, cap, 1) : -1;
        if (n < 0 || vfs_write_file(fp, b, (size_t)n) != 0) re_say(T(DANGER), "Could not write %s.", fp);
        else re_say(T(OK), "Exported %s to %s.", reg_key_name(re.sel_key), fp);
        if (b) kfree_large(b, cap);
        desktop_files_changed();
        break;
    }
    case RD_IMPORT: {
        char fp[256];
        sh_path(re.f_data, fp, sizeof(fp));
        const char *d;
        long n = sh_read_file(fp, &d);
        int nk = 0, nv = 0;
        if (n < 0) { re_say(T(DANGER), "Cannot read %s.", fp); return; }
        if (reg_import(d, &nk, &nv, err, sizeof(err)) != 0) { re_say(T(DANGER), "Import stopped: %s", err); return; }
        re_say(T(OK), "Imported %d key(s) and %d value(s) from %s.", nk, nv, fp);
        desktop_apply_settings();
        break;
    }
    }
    re.dlg = RD_NONE;
}

static void re_toolbar(int i)
{
    char path[300];
    re_key_path_sel(path, sizeof(path));
    re.msg[0] = 0;
    switch (i) {
    case 0: if (re_writable()) { re.dlg = RD_NEWKEY; snprintf(re.f_name, sizeof(re.f_name), "New Key #1"); } break;
    case 1: if (re_writable()) re.dlg = RD_NEWVAL_MENU; break;
    case 2: re_open_edit(0); break;
    case 3:
        if (!re_writable()) break;
        if (re.sel_val >= 0) { char nm[64]; int t; re_value_name_at(re.sel_val, nm, sizeof(nm), &t); snprintf(re.old_name, sizeof(re.old_name), "%s", nm); snprintf(re.f_name, sizeof(re.f_name), "%s", nm); re.dlg = RD_RENAME_VAL; }
        else if (re.sel_key >= REG_HIVES) { snprintf(re.f_name, sizeof(re.f_name), "%s", reg_key_name(re.sel_key)); re.dlg = RD_RENAME_KEY; }
        else re_say(T(WARN), "The top-level keys cannot be renamed.");
        break;
    case 4:
        if (!re_writable()) break;
        if (re.sel_val >= 0) { int t; re_value_name_at(re.sel_val, re.old_name, sizeof(re.old_name), &t); re.dlg = RD_DELETE; }
        else if (re.sel_key >= REG_HIVES) re.dlg = RD_DELETE;
        else re_say(T(WARN), "The top-level keys cannot be deleted.");
        break;
    case 5: re.dlg = RD_FIND; snprintf(re.f_data, sizeof(re.f_data), "%s", re.find_text); break;
    case 6:
        if (re.sel_key < 0) { re_say(T(WARN), "Select a key to export."); break; }
        re.dlg = RD_EXPORT;
        snprintf(re.f_data, sizeof(re.f_data), "~/%s.reg", reg_key_name(re.sel_key));
        for (char *c = re.f_data; *c; c++) if (*c == ' ') *c = '_';
        break;
    case 7: re.dlg = RD_IMPORT; snprintf(re.f_data, sizeof(re.f_data), "~/"); break;
    case 8: reg_refresh_live(); re_say(T(OK), "Refreshed."); break;
    }
}

static char *re_dialog_field(size_t *cap)
{
    if (re.dlg == RD_EDIT) { if (re.field == 0) { *cap = sizeof(re.f_name); return re.f_name; } *cap = sizeof(re.f_data); return re.f_data; }
    if (re.dlg == RD_NEWKEY || re.dlg == RD_RENAME_KEY || re.dlg == RD_RENAME_VAL) { *cap = sizeof(re.f_name); return re.f_name; }
    if (re.dlg == RD_FIND || re.dlg == RD_EXPORT || re.dlg == RD_IMPORT) { *cap = sizeof(re.f_data); return re.f_data; }
    return NULL;
}

void regedit_event(wm_window_t *win, wm_input_event_t *e)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    int by = RE_TOOL_H + RE_ADDR_H, bh = h - by - 26;
    if (re.dlg && re.dlg != RD_NEWVAL_MENU) {
        size_t cap;
        char *f = re_dialog_field(&cap);
        if (e->type == IE_TEXT_INPUT && f) {
            size_t l = strlen(f);
            char c = e->data.text.ch;
            if (c >= 32 && c < 127 && l < cap - 1) { f[l] = c; f[l + 1] = 0; }
        } else if (e->type == IE_KEY_DOWN) {
            uint32_t k = e->data.keyboard.key;
            if (k == WM_KEY_ESC) re.dlg = RD_NONE;
            else if (k == WM_KEY_ENTER) re_dialog_ok();
            else if (k == WM_KEY_TAB && re.dlg == RD_EDIT) re.field ^= 1;
            else if (k == WM_KEY_BACKSPACE && f && f[0]) f[strlen(f) - 1] = 0;
        } else if (e->type == IE_MOUSE_DOWN && e->data.mouse.button == 1) {
            int dw = 520, dh = re.dlg == RD_DELETE ? 170 : re.dlg == RD_EDIT ? 270 : 200;
            int dx = (w - dw) / 2, dy = (h - dh) / 2, mx = e->data.mouse.x, my = e->data.mouse.y;
            if (ui_hit(mx, my, dx + dw - 240, dy + dh - 48, 110, 32)) re_dialog_ok();
            else if (ui_hit(mx, my, dx + dw - 120, dy + dh - 48, 100, 32)) re.dlg = RD_NONE;
            else if (re.dlg == RD_EDIT && ui_hit(mx, my, dx + 20, dy + 68, dw - 40, 30)) re.field = 0;
            else if (re.dlg == RD_EDIT && ui_hit(mx, my, dx + 20, dy + 128, dw - 40, 30)) re.field = 1;
            else if (re.dlg == RD_EDIT && ui_hit(mx, my, dx + 20, dy + 166, dw - 40, 24) && (re.edit_type == REG_DWORD || re.edit_type == REG_QWORD)) {
                /* switch base, converting the number shown */
                uint64_t v = 0;
                const char *p = re.f_data;
                int hex = re.edit_hex;
                if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; hex = 1; }
                for (; *p; p++) {
                    int d = *p >= '0' && *p <= '9' ? *p - '0' : hex && (*p | 32) >= 'a' && (*p | 32) <= 'f' ? (*p | 32) - 'a' + 10 : -1;
                    if (d < 0) break;
                    v = v * (hex ? 16 : 10) + (uint64_t)d;
                }
                re.edit_hex = !re.edit_hex;
                if (re.edit_hex) snprintf(re.f_data, sizeof(re.f_data), "%llx", (unsigned long long)v);
                else snprintf(re.f_data, sizeof(re.f_data), "%llu", (unsigned long long)v);
            }
        }
        return;
    }
    if (e->type == IE_MOUSE_WHEEL) {
        int *sc = e->data.mouse.x < RE_TREE_W ? &re.tree_scroll : &re.val_scroll;
        *sc -= e->data.mouse.button;
        if (*sc < 0) *sc = 0;
        return;
    }
    if (re.addr_focus && (e->type == IE_TEXT_INPUT || e->type == IE_KEY_DOWN)) {
        if (e->type == IE_TEXT_INPUT) {
            char c = e->data.text.ch;
            if (c >= 32 && c < 127 && re.addr_len < (int)sizeof(re.addr) - 1) { re.addr[re.addr_len++] = c; re.addr[re.addr_len] = 0; }
        } else {
            uint32_t k = e->data.keyboard.key;
            if (k == WM_KEY_BACKSPACE && re.addr_len) re.addr[--re.addr_len] = 0;
            else if (k == WM_KEY_ESC) { re.addr_focus = 0; re_select(re.sel_key); }
            else if (k == WM_KEY_ENTER) {
                const char *p = re.addr;
                if (!strncmp(p, "Computer", 8)) p += 8;
                while (*p == '\\' || *p == '/') p++;
                int key = *p ? reg_open_key(p, 0) : -1;
                if (*p && key < 0) re_say(T(DANGER), "No such key: %s", p);
                else { re_select(key); re.addr_focus = 0; }
            }
        }
        return;
    }
    if (e->type == IE_KEY_DOWN) {
        uint32_t k = e->data.keyboard.key;
        if (k == WM_KEY_DELETE) re_toolbar(4);
        else if (k == WM_KEY_F1 + 2) re_find_next();               /* F3 */
        else if (k == WM_KEY_F1 + 1) re_toolbar(3);                /* F2 rename */
        else if (k == WM_KEY_F1 + 4) re_toolbar(8);                /* F5 refresh */
        else if (k == WM_KEY_ENTER) re_open_edit(0);
        else if ((k == WM_KEY_UP || k == WM_KEY_DOWN) && re.ntree) {
            int cur = 0;
            for (int i = 0; i < re.ntree; i++) if (re.tree_rows[i] == re.sel_key) cur = i;
            cur += k == WM_KEY_DOWN ? 1 : -1;
            if (cur >= 0 && cur < re.ntree) re_select(re.tree_rows[cur]);
        } else if (k == WM_KEY_RIGHT && re.sel_key >= 0) re.expanded[re.sel_key] = 1;
        else if (k == WM_KEY_LEFT && re.sel_key >= 0) {
            if (re.expanded[re.sel_key]) re.expanded[re.sel_key] = 0; else re_select(reg_key_parent(re.sel_key));
        }
        return;
    }
    if (e->type != IE_MOUSE_DOWN || e->data.mouse.button != 1) return;
    int mx = e->data.mouse.x, my = e->data.mouse.y;
    if (re.dlg == RD_NEWVAL_MENU) {
        int mxx = re_tool_x(1, 0), myy = RE_TOOL_H - 4;
        static const int types[6] = { REG_SZ, REG_DWORD, REG_QWORD, REG_BINARY, REG_MULTI_SZ, REG_EXPAND_SZ };
        re.dlg = RD_NONE;
        if (ui_hit(mx, my, mxx, myy, 220, 6 * 30 + 8)) {
            int i = (my - myy - 4) / 30;
            if (i >= 0 && i < 6) re_open_edit(types[i]);
        }
        return;
    }
    re.addr_focus = 0;
    if (my < RE_TOOL_H) {
        for (int i = 0; i < RE_NTOOLS; i++) {
            int bx = re_tool_x(i, 0), bw = fb_text_width(re_tool_labels[i]) + 24;
            if (ui_hit(mx, my, bx, 7, bw, 30)) { re_toolbar(i); return; }
        }
        return;
    }
    if (my < by) { re.addr_focus = 1; re.addr_len = (int)strlen(re.addr); return; }
    if (mx < RE_TREE_W) {
        int i = re.tree_scroll + (my - by - 6) / RE_ROW_H;
        if (i < 0 || i >= re.ntree || my - by - 6 < 0) return;
        int k = re.tree_rows[i], d = re.tree_depth[i];
        int arrow_x = 10 + d * 16;
        if (mx >= arrow_x - 4 && mx < arrow_x + 14) {          /* the arrow: expand / collapse */
            if (k < 0) re.computer_open = !re.computer_open; else re.expanded[k] = !re.expanded[k];
            return;
        }
        re_select(k);
        if (k >= 0 && re.sel_key == k && reg_key_has_children(k) && !re.expanded[k]) re.expanded[k] = 1;
        re.msg[0] = 0;
        return;
    }
    if (my >= by + 34 && my < by + bh) {
        int row = re.val_scroll + (my - by - 34) / RE_ROW_H;
        char nm[64];
        int type, has_default = 0;
        for (int i = 0; reg_value_at(re.sel_key, i, nm, sizeof(nm), &type, NULL, 0, NULL) == 0; i++) if (!nm[0]) has_default = 1;
        int i = has_default ? row : row - 1;                   /* row 0 may be the placeholder (Default) */
        if (!has_default && row == 0) {
            re.sel_val = -1;
            if (re_writable()) { re_open_edit(REG_SZ); re.f_name[0] = 0; re.field = 1; }
            return;
        }
        if (reg_value_at(re.sel_key, i, nm, sizeof(nm), &type, NULL, 0, NULL) != 0) { re.sel_val = -1; return; }
        uint32_t now = (uint32_t)g_kstate.uptime_ticks;
        int dbl = re.sel_val == i && re.last_click_val == i && now - re.last_click_tick < 50;
        re.sel_val = i;
        re.last_click_val = i;
        re.last_click_tick = now;
        if (dbl) re_open_edit(0);
    }
}

wm_window_t *regedit_app_open(int x, int y, int w, int h)
{
    memset(&re, 0, sizeof(re));
    reg_refresh_live();
    re.sel_val = -1;
    re_select(-1);
    re.computer_open = 1;
    wm_window_t *win = wm_create_window("Registry Editor", x, y, w, h, WS_DEFAULT, regedit_draw, regedit_event);
    if (win) { win->icon = ICON_REGEDIT; win->min_w = 760; win->min_h = 440; }
    re.win = win;
    return win;
}

/* Open the editor at a key (Terminal "regedit HKCU\..."). */
void regedit_show_key(const char *path)
{
    desktop_app_launch(desktop_app_find("Registry Editor"));
    int k = reg_open_key(path, 0);
    if (k >= 0) re_select(k);
}

/* ---- Terminal: reg (Windows reg.exe syntax) ------------------------------------------ */
static const char *re_opt(int argc, char **argv, const char *opt)
{
    for (int i = 1; i < argc - 1; i++) if (!strcasecmp_s(argv[i], opt)) return argv[i + 1];
    return NULL;
}
static int re_flag(int argc, char **argv, const char *opt)
{
    for (int i = 1; i < argc; i++) if (!strcasecmp_s(argv[i], opt)) return 1;
    return 0;
}

static void re_print_key(int k, int recurse, const char *only)
{
    char path[300], nm[64], text[600];
    int type;
    reg_key_path(k, path, sizeof(path));
    sh_printf("\n%s\n", path);
    for (int i = 0; reg_value_at(k, i, nm, sizeof(nm), &type, text, sizeof(text), NULL) == 0; i++) {
        if (only && strcasecmp_s(nm, only)) continue;
        if (type == REG_DWORD || type == REG_QWORD) {
            uint64_t v = 0;
            reg_get_raw_k(k, nm, NULL, &v, 8);
            if (type == REG_DWORD) v &= 0xFFFFFFFFu;
            snprintf(text, sizeof(text), "0x%llx", (unsigned long long)v);
        }
        for (char *c = text; *c; c++) if (*c == '\n') *c = '|';
        sh_printf("    %-28s %-14s %s\n", nm[0] ? nm : "(Default)", reg_type_name(type), text);
    }
    for (int i = 0;; i++) {
        int c = reg_key_child(k, i);
        if (c < 0) break;
        if (recurse) { re_print_key(c, 1, only); reg_key_child(k, 0); }
        else { char cp[300]; reg_key_path(c, cp, sizeof(cp)); sh_printf("%s\n", cp); }
    }
}

int c_reg(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "/?") || !strcmp(argv[1], "help")) {
        sh_out("reg query  <key> [/v name] [/s]\n"
               "reg add    <key> [/v name | /ve] [/t REG_SZ|REG_DWORD|REG_QWORD|REG_BINARY|REG_MULTI_SZ|REG_EXPAND_SZ] [/d data] [/f]\n"
               "reg delete <key> [/v name | /ve | /va] [/f]\n"
               "reg export <key> <file.reg>\n"
               "reg import <file.reg>\n"
               "Keys: HKCR, HKCU, HKLM, HKCC (or HKEY_...), e.g. reg query \"HKCU\\Software\\JARVIS OS\\Settings\"\n");
        return argc < 2;
    }
    const char *act = argv[1];
    char err[160];
    if (!strcasecmp_s(act, "import")) {
        if (argc < 3) { sh_err("usage: reg import <file.reg>\n"); return 1; }
        char fp[256];
        sh_path(argv[2], fp, sizeof(fp));
        const char *d;
        long n = sh_read_file(fp, &d);
        if (n < 0) { sh_err("reg: cannot read %s\n", argv[2]); return 1; }
        int nk = 0, nv = 0;
        if (reg_import(d, &nk, &nv, err, sizeof(err)) != 0) { sh_err("reg import: %s\n", err); return 1; }
        desktop_apply_settings();
        sh_printf("The operation completed successfully (%d keys, %d values).\n", nk, nv);
        return 0;
    }
    if (argc < 3) { sh_err("reg %s: which key?\n", act); return 1; }
    const char *key = argv[2];
    int k = reg_open_key(key, 0);
    if (!strcasecmp_s(act, "query")) {
        if (k < 0) { sh_err("ERROR: The system was unable to find the specified registry key or value.\n"); return 1; }
        const char *v = re_opt(argc, argv, "/v");
        re_print_key(k, re_flag(argc, argv, "/s"), re_flag(argc, argv, "/ve") ? "" : v);
        return 0;
    }
    if (!strcasecmp_s(act, "export")) {
        if (k < 0 || argc < 4) { sh_err("usage: reg export <key> <file.reg>\n"); return 1; }
        size_t cap = 512 * 1024;
        char *b = (char *)kmalloc_large(cap);
        char fp[256];
        sh_path(argv[3], fp, sizeof(fp));
        int n = b ? reg_export(key, b, cap, 1) : -1;
        int rc = n < 0 || vfs_write_file(fp, b, (size_t)n) != 0;
        if (b) kfree_large(b, cap);
        if (rc) { sh_err("reg export: cannot write %s\n", argv[3]); return 1; }
        sh_printf("The operation completed successfully.\n");
        return 0;
    }
    if (k >= 0 && reg_key_volatile(k)) { sh_err("ERROR: this key is built from the hardware and cannot be changed.\n"); return 1; }
    if (!strcasecmp_s(act, "add")) {
        const char *name = re_flag(argc, argv, "/ve") ? "" : re_opt(argc, argv, "/v");
        const char *ts = re_opt(argc, argv, "/t"), *data = re_opt(argc, argv, "/d");
        int type = ts ? reg_type_parse(ts) : REG_SZ;
        if (type < 0) { sh_err("reg add: unknown type %s\n", ts); return 1; }
        k = reg_open_key(key, 1);
        if (k < 0) { sh_err("reg add: invalid key %s (start with HKCU, HKLM, HKCR or HKCC)\n", key); return 1; }
        if (!name) { sh_printf("The operation completed successfully.\n"); return 0; }
        char path[300];
        reg_key_path(k, path, sizeof(path));
        if (!strcmp(path, re_settings_path) && name[0]) {        /* a system setting: check it and apply it */
            char msg[200];
            if (prefs_set(name, data ? data : "", msg, sizeof(msg)) == 0) { sh_printf("%s\n", msg); return 0; }
            if (strncmp(msg, "Unknown setting", 15)) { sh_err("reg add: %s\n", msg); return 1; }
        }
        if (reg_set_from_text(k, name, type, data ? data : "", err, sizeof(err)) != 0) { sh_err("reg add: %s\n", err); return 1; }
        sh_printf("The operation completed successfully.\n");
        return 0;
    }
    if (!strcasecmp_s(act, "delete")) {
        if (k < 0) { sh_err("ERROR: The system was unable to find the specified registry key or value.\n"); return 1; }
        const char *v = re_opt(argc, argv, "/v");
        int rc;
        if (re_flag(argc, argv, "/va")) {
            char nm[64];
            int type;
            while (reg_value_at(k, 0, nm, sizeof(nm), &type, NULL, 0, NULL) == 0) reg_delete_value(key, nm);
            rc = 0;
        } else if (v || re_flag(argc, argv, "/ve")) rc = reg_delete_value(key, v ? v : "");
        else rc = k < REG_HIVES ? -1 : reg_delete_key(key);
        if (rc) { sh_err("ERROR: The system was unable to find the specified registry key or value.\n"); return 1; }
        sh_printf("The operation completed successfully.\n");
        return 0;
    }
    sh_err("reg: unknown operation %s (see: reg /?)\n", act);
    return 1;
}

int c_regedit(int argc, char **argv)
{
    if (argc > 1 && image_is_picture_name(argv[1]) == 0 && strstr(argv[1], ".reg")) { char *a[3] = { (char *)"reg", (char *)"import", argv[1] }; return c_reg(3, a); }
    if (argc > 1) regedit_show_key(argv[1]);
    else desktop_app_launch(desktop_app_find("Registry Editor"));
    return 0;
}

/* setx NAME value: a variable kept in HKCU\Environment for every new Terminal. */
int c_setx(int argc, char **argv)
{
    if (argc < 3) { sh_err("usage: setx NAME value\n"); return 1; }
    reg_set_sz("HKCU\\Environment", argv[1], argv[2]);
    sh_setenv(argv[1], argv[2]);
    sh_printf("SUCCESS: Specified value was saved.\n");
    return 0;
}
