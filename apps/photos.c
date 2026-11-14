/*
 * =============================================================================
 * JARVIS OS — Photos (apps/photos.c)
 * Language: C17
 *
 * Picture viewer for PNG, JPEG and BMP files: fit to window or zoom
 * (buttons, + / -, mouse wheel), pan by dragging, rotate, previous / next
 * picture in the same folder (arrow keys), set as wallpaper, delete.
 * With no file open it shows the pictures in Pictures as a grid.
 * =============================================================================
 */

#define PH_TOOLBAR_H 44
#define PH_MAX_FILES 256

static struct {
    wm_window_t *window;
    char path[256];
    image_t img;                /* decoded picture (rotated in place)       */
    char error[96];
    int zoom;                   /* percent; 0 = fit to window              */
    int pan_x, pan_y;           /* offset of the picture centre, screen px */
    int dragging, drag_x, drag_y;
    /* scaled copy for the current view */
    uint32_t *view; size_t view_bytes; int view_w, view_h, view_valid;
    /* pictures in the same folder */
    char files[PH_MAX_FILES][96]; int nfiles, index;
    char dir[256];
    /* gallery (no file open) */
    image_t thumbs[24]; int nthumbs; int thumbs_loaded;
    int hover_btn;
} ph;

enum { PHB_NONE = -1, PHB_PREV, PHB_NEXT, PHB_ZOOM_OUT, PHB_FIT, PHB_ZOOM_IN, PHB_ROTATE, PHB_WALL, PHB_DELETE, PHB_GALLERY, PHB_COUNT };
static const char *const ph_btn_label[PHB_COUNT] = { "<", ">", "-", "Fit", "+", "Rotate", "Set as wallpaper", "Delete", "All pictures" };

static void ph_view_free(void)
{
    if (ph.view) {
        if (ph.view_bytes >= LARGE_ALLOC_THRESHOLD) kfree_large(ph.view, ph.view_bytes); else kfree(ph.view);
    }
    ph.view = NULL;
    ph.view_valid = 0;
}

static void ph_thumbs_free(void)
{
    for (int i = 0; i < ph.nthumbs; i++) image_free(&ph.thumbs[i]);
    ph.nthumbs = 0;
    ph.thumbs_loaded = 0;
}

static void ph_scan_folder(void)
{
    static sh_ent_t ents[512];
    ph.nfiles = 0;
    ph.index = 0;
    int n = sh_readdir(ph.dir, ents, 512);
    for (int i = 0; i < n && ph.nfiles < PH_MAX_FILES; i++)
        if (!ents[i].is_dir && image_is_picture_name(ents[i].name))
            snprintf(ph.files[ph.nfiles++], 96, "%s", ents[i].name);
    const char *base = sh_basename(ph.path);
    for (int i = 0; i < ph.nfiles; i++) if (!strcmp(ph.files[i], base)) ph.index = i;
}

static void ph_load(const char *path)
{
    image_free(&ph.img);
    ph_view_free();
    snprintf(ph.path, sizeof(ph.path), "%s", path);
    ph.error[0] = 0;
    ph.zoom = 0;
    ph.pan_x = ph.pan_y = 0;
    if (image_load(path, &ph.img, ph.error, sizeof(ph.error))) ph.img.px = NULL;
    snprintf(ph.dir, sizeof(ph.dir), "%s", path);
    char *s = strrchr(ph.dir, '/');
    if (s && s != ph.dir) *s = 0;
    ph_scan_folder();
    if (ph.window) {
        char t[128];
        snprintf(t, sizeof(t), "Photos - %s", sh_basename(path));
        wm_set_title(ph.window, t);
    }
    desktop_invalidate();
}

static void ph_step(int d)
{
    if (ph.nfiles < 2) return;
    ph.index = (ph.index + d + ph.nfiles) % ph.nfiles;
    char p[384];
    snprintf(p, sizeof(p), "%s/%s", ph.dir, ph.files[ph.index]);
    ph_load(p);
}

static void ph_rotate(void)
{
    if (!ph.img.px) return;
    size_t bytes = (size_t)ph.img.w * ph.img.h * 4;
    uint32_t *r = (uint32_t *)(bytes >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(bytes) : kmalloc(bytes));
    if (!r) return;
    int W = ph.img.w, H = ph.img.h;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            r[(size_t)x * H + (H - 1 - y)] = ph.img.px[(size_t)y * W + x];   /* 90 deg clockwise */
    if (bytes >= LARGE_ALLOC_THRESHOLD) kfree_large(ph.img.px, bytes); else kfree(ph.img.px);
    ph.img.px = r;
    ph.img.w = H;
    ph.img.h = W;
    ph_view_free();
}

static void ph_btn_rect(int i, int x, int y, int w, int *bx, int *bw)
{
    /* left group: prev next | zoom - fit + | rotate ; right group: wallpaper delete gallery */
    static const int widths[PHB_COUNT] = { 34, 34, 34, 46, 34, 70, 136, 70, 104 };
    int cx = x + 10;
    if (i <= PHB_ROTATE) {
        for (int k = 0; k < i; k++) cx += widths[k] + (k == PHB_NEXT || k == PHB_ZOOM_IN ? 16 : 6);
    } else {
        cx = x + w - 10;
        for (int k = PHB_COUNT - 1; k >= i; k--) cx -= widths[k] + 6;
    }
    *bx = cx;
    *bw = widths[i];
    (void)y;
}

static int ph_btn_at(int mx, int my, int w)
{
    if (my < 6 || my > 38) return PHB_NONE;
    for (int i = 0; i < PHB_COUNT; i++) {
        int bx, bw;
        ph_btn_rect(i, 0, 0, w, &bx, &bw);
        if (mx >= bx && mx < bx + bw) return i;
    }
    return PHB_NONE;
}

static void ph_gallery_load(void)
{
    if (ph.thumbs_loaded) return;
    ph.thumbs_loaded = 1;
    snprintf(ph.dir, sizeof(ph.dir), "/jarvis/data/Pictures");
    if (!vfs_is_dir(ph.dir)) mkdir_p(ph.dir);
    ph.path[0] = 0;
    /* Pictures and the folders inside it (Screenshots ...), newest names last */
    static sh_ent_t top[256], sub[256];
    ph.nfiles = 0;
    int nt = sh_readdir(ph.dir, top, 256);
    for (int i = 0; i < nt && ph.nfiles < PH_MAX_FILES; i++) {
        if (top[i].is_dir) {
            char d[384];
            snprintf(d, sizeof(d), "%s/%s", ph.dir, top[i].name);
            int ns = sh_readdir(d, sub, 256);
            for (int k = ns - 1; k >= 0 && ph.nfiles < PH_MAX_FILES; k--)      /* newest screenshots first */
                if (!sub[k].is_dir && image_is_picture_name(sub[k].name))
                    snprintf(ph.files[ph.nfiles++], 96, "%s/%s", top[i].name, sub[k].name);
        } else if (image_is_picture_name(top[i].name))
            snprintf(ph.files[ph.nfiles++], 96, "%s", top[i].name);
    }
    for (int i = 0; i < ph.nfiles && ph.nthumbs < 24; i++) {
        char p[384], err[64];
        image_t full;
        snprintf(p, sizeof(p), "%s/%s", ph.dir, ph.files[i]);
        if (image_load(p, &full, err, sizeof(err))) { ph.thumbs[ph.nthumbs].px = NULL; ph.thumbs[ph.nthumbs].w = ph.thumbs[ph.nthumbs].h = 0; ph.nthumbs++; continue; }
        int tw = 160, th = 120;
        if (full.w * 120 > full.h * 160) th = full.h * 160 / full.w; else tw = full.w * 120 / full.h;
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        image_t *t = &ph.thumbs[ph.nthumbs++];
        t->w = tw; t->h = th;
        t->px = (uint32_t *)kmalloc((size_t)tw * th * 4);
        if (t->px) image_scale(&full, t->px, tw, th);
        image_free(&full);
    }
}

static void ph_blit(const uint32_t *px, int w, int h, int dx, int dy)
{
    fb_clip_t c = fb_get_clip();
    uint32_t *fb = fb_get_base();
    int sw = (int)fb_get_width();
    for (int y = 0; y < h; y++) {
        int yy = dy + y;
        if (yy < c.y0 || yy >= c.y1) continue;
        int x0 = dx < c.x0 ? c.x0 - dx : 0, x1 = dx + w > c.x1 ? c.x1 - dx : w;
        if (x1 > x0) memcpy(fb + (size_t)yy * sw + dx + x0, px + (size_t)y * w + x0, (size_t)(x1 - x0) * 4);
    }
}

static void ph_draw(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    fb_fill_rect(x, y, w, h, 0x00161616);

    /* toolbar */
    fb_fill_rect(x, y, w, PH_TOOLBAR_H, T(SURFACE));
    fb_fill_rect(x, y + PH_TOOLBAR_H - 1, w, 1, T(BORDER));
    for (int i = 0; i < PHB_COUNT; i++) {
        int bx, bw;
        ph_btn_rect(i, x, y, w, &bx, &bw);
        int enabled = ph.img.px || i == PHB_GALLERY;
        if (!ph.path[0] && i != PHB_GALLERY) continue;
        uint32_t bg = i == ph.hover_btn && enabled ? T(HOVER) : T(SURFACE);
        if (i == PHB_WALL && enabled) bg = ph.hover_btn == i ? fb_alpha_blend(T(ACCENT), 0xFFFFFF, 220) : T(ACCENT);
        fb_fill_rounded_rect(bx, y + 6, bw, 32, 5, bg);
        if (i != PHB_WALL) fb_draw_rounded_rect(bx, y + 6, bw, 32, 5, T(BORDER));
        ui_text_center(bx, y + 14, bw, ph_btn_label[i], i == PHB_WALL && enabled ? T(ACCENT_TEXT) : enabled ? T(TEXT) : T(TEXT_DIM));
    }
    int vy = y + PH_TOOLBAR_H, vh = h - PH_TOOLBAR_H - 26;

    if (!ph.path[0]) {                                   /* gallery */
        ph_gallery_load();
        fb_draw_text_bold(x + 20, vy + 14, "Pictures", 0x00FFFFFF);
        fb_draw_text(x + 100, vy + 14, ph.dir, 0x00A0A0A0);
        if (!ph.nfiles) {
            ui_text_center(x, vy + vh / 2 - 20, w, "No pictures yet.", 0x00C0C0C0);
            ui_text_center(x, vy + vh / 2 + 4, w, "Screenshots (Print Screen) and pictures you save in Pictures appear here.", 0x00808080);
        }
        int cols = (w - 20) / 180;
        if (cols < 1) cols = 1;
        for (int i = 0; i < ph.nthumbs; i++) {
            int cx = x + 20 + (i % cols) * 180, cy = vy + 44 + (i / cols) * 160;
            if (cy + 150 > y + h) break;
            fb_fill_rounded_rect(cx, cy, 168, 128, 6, 0x00242424);
            image_t *t = &ph.thumbs[i];
            if (t->px) ph_blit(t->px, t->w, t->h, cx + (168 - t->w) / 2, cy + (128 - t->h) / 2);
            ui_text_fit(cx, cy + 134, 168, sh_basename(ph.files[i]), 0x00D0D0D0);
        }
        return;
    }

    if (!ph.img.px) {
        ui_text_center(x, vy + vh / 2 - 8, w, ph.error[0] ? ph.error : "Cannot show this picture", 0x00FF8080);
    } else {
        /* size of the picture on screen */
        int dw, dh;
        if (ph.zoom == 0) {
            dw = ph.img.w; dh = ph.img.h;
            if (dw > w - 20 || dh > vh - 20) {
                if ((int64_t)ph.img.w * (vh - 20) > (int64_t)ph.img.h * (w - 20)) { dw = w - 20; dh = (int)((int64_t)ph.img.h * dw / ph.img.w); }
                else { dh = vh - 20; dw = (int)((int64_t)ph.img.w * dh / ph.img.h); }
            }
        } else {
            dw = (int)((int64_t)ph.img.w * ph.zoom / 100);
            dh = (int)((int64_t)ph.img.h * ph.zoom / 100);
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        if ((int64_t)dw * dh > 6000000) { dw = dw * 6 / 10; dh = dh * 6 / 10; }    /* keep the zoomed copy affordable */
        if (!ph.view_valid || ph.view_w != dw || ph.view_h != dh) {
            ph_view_free();
            ph.view_bytes = (size_t)dw * dh * 4;
            ph.view = (uint32_t *)(ph.view_bytes >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(ph.view_bytes) : kmalloc(ph.view_bytes));
            if (ph.view) { image_scale(&ph.img, ph.view, dw, dh); ph.view_w = dw; ph.view_h = dh; ph.view_valid = 1; }
        }
        fb_clip_t saved = fb_get_clip();
        fb_set_clip(x, vy, w, vh);
        fb_clip_t c2 = fb_get_clip();
        if (c2.x0 < saved.x0) c2.x0 = saved.x0;
        if (c2.y0 < saved.y0) c2.y0 = saved.y0;
        if (c2.x1 > saved.x1) c2.x1 = saved.x1;
        if (c2.y1 > saved.y1) c2.y1 = saved.y1;
        fb_restore_clip(c2);
        if (ph.view) ph_blit(ph.view, ph.view_w, ph.view_h, x + (w - ph.view_w) / 2 + ph.pan_x, vy + (vh - ph.view_h) / 2 + ph.pan_y);
        fb_restore_clip(saved);
    }

    /* status bar */
    int sy = y + h - 26;
    fb_fill_rect(x, sy, w, 26, T(SURFACE));
    char st[160];
    if (ph.img.px) {
        int shown = ph.zoom ? ph.zoom : (ph.view_w * 100 / (ph.img.w ? ph.img.w : 1));
        snprintf(st, sizeof(st), "%s   %d x %d   %d%%   %d of %d", sh_basename(ph.path), ph.img.w, ph.img.h, shown, ph.index + 1, ph.nfiles);
    } else snprintf(st, sizeof(st), "%s", ph.path);
    ui_text_fit(x + 12, sy + 5, w - 24, st, T(TEXT_DIM));
}

static void ph_zoom(int dir)
{
    if (!ph.img.px) return;
    int cur = ph.zoom ? ph.zoom : (ph.view_w * 100 / (ph.img.w ? ph.img.w : 1));
    static const int steps[] = { 10, 25, 33, 50, 67, 100, 150, 200, 300, 400, 800 };
    int next = cur;
    if (dir > 0) { for (int i = 0; i < 11; i++) if (steps[i] > cur) { next = steps[i]; break; } }
    else { for (int i = 10; i >= 0; i--) if (steps[i] < cur) { next = steps[i]; break; } }
    ph.zoom = next;
    desktop_invalidate();
}

static void ph_action(int b)
{
    char msg[200];
    switch (b) {
    case PHB_PREV: ph_step(-1); break;
    case PHB_NEXT: ph_step(1); break;
    case PHB_ZOOM_OUT: ph_zoom(-1); break;
    case PHB_ZOOM_IN: ph_zoom(1); break;
    case PHB_FIT: ph.zoom = 0; ph.pan_x = ph.pan_y = 0; break;
    case PHB_ROTATE: ph_rotate(); break;
    case PHB_WALL:
        if (!ph.img.px) break;
        if (prefs_set("wallpaper", ph.path, msg, sizeof(msg)) == 0) notification_show("Wallpaper", "This picture is now your desktop background.", NOTIF_INFO, 3000);
        else notification_show("Wallpaper", msg, NOTIF_WARNING, 5000);
        break;
    case PHB_DELETE:
        if (!ph.path[0]) break;
        if (file_explorer_trash_path(ph.path) == 0) {
            notification_show("Photos", "Moved to the Recycle Bin.", NOTIF_INFO, 2500);
            int had = ph.nfiles;
            ph_scan_folder();
            if (had > 1 && ph.nfiles) { ph.index %= ph.nfiles; char p[384]; snprintf(p, sizeof(p), "%s/%s", ph.dir, ph.files[ph.index]); ph_load(p); }
            else { image_free(&ph.img); ph_view_free(); ph.path[0] = 0; ph_thumbs_free(); }
            desktop_files_changed();
        }
        break;
    case PHB_GALLERY:
        image_free(&ph.img);
        ph_view_free();
        ph.path[0] = 0;
        ph_thumbs_free();
        if (ph.window) wm_set_title(ph.window, "Photos");
        break;
    default: break;
    }
    desktop_invalidate();
}

static void ph_event(wm_window_t *win, wm_input_event_t *e)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    switch (e->type) {
    case IE_MOUSE_MOVE: {
        if (ph.dragging) {
            ph.pan_x += e->data.mouse.x - ph.drag_x;
            ph.pan_y += e->data.mouse.y - ph.drag_y;
            ph.drag_x = e->data.mouse.x; ph.drag_y = e->data.mouse.y;
            desktop_invalidate();
            return;
        }
        int b = ph_btn_at(e->data.mouse.x, e->data.mouse.y, w);
        if (b != ph.hover_btn) { ph.hover_btn = b; desktop_invalidate(); }
        return;
    }
    case IE_MOUSE_DOWN: {
        if (e->data.mouse.button != 1) return;
        int b = ph_btn_at(e->data.mouse.x, e->data.mouse.y, w);
        if (b != PHB_NONE) { if (ph.path[0] || b == PHB_GALLERY) ph_action(b); return; }
        if (!ph.path[0]) {                                 /* gallery click */
            int cols = (w - 20) / 180;
            if (cols < 1) cols = 1;
            int cx = (e->data.mouse.x - 20) / 180, cy = (e->data.mouse.y - PH_TOOLBAR_H - 44) / 160;
            int i = cy * cols + cx;
            if (cx >= 0 && cx < cols && cy >= 0 && i < ph.nthumbs) {
                char p[384];
                snprintf(p, sizeof(p), "%s/%s", ph.dir, ph.files[i]);
                ph_thumbs_free();
                ph_load(p);
            }
            return;
        }
        ph.dragging = 1;
        ph.drag_x = e->data.mouse.x; ph.drag_y = e->data.mouse.y;
        return;
    }
    case IE_MOUSE_UP: ph.dragging = 0; return;
    case IE_MOUSE_WHEEL: ph_zoom(e->data.mouse.button > 0 ? 1 : -1); return;
    case IE_TEXT_INPUT:
        if (e->data.text.ch == '+' || e->data.text.ch == '=') ph_zoom(1);
        else if (e->data.text.ch == '-') ph_zoom(-1);
        else if (e->data.text.ch == '0') ph_action(PHB_FIT);
        else if (e->data.text.ch == 'r') ph_action(PHB_ROTATE);
        return;
    case IE_KEY_DOWN:
        if (e->data.keyboard.key == WM_KEY_LEFT) ph_step(-1);
        else if (e->data.keyboard.key == WM_KEY_RIGHT) ph_step(1);
        else if (e->data.keyboard.key == WM_KEY_DELETE) ph_action(PHB_DELETE);
        return;
    default: return;
    }
}

static void ph_closed(wm_window_t *win)
{
    (void)win;
}

wm_window_t *photos_app_open(int x, int y, int w, int h)
{
    ph.window = wm_create_window("Photos", x, y, w, h, WS_DEFAULT, ph_draw, ph_event);
    if (!ph.window) return NULL;
    ph.window->icon = ICON_PHOTOS;
    ph.hover_btn = PHB_NONE;
    if (!ph.path[0]) ph_thumbs_free();
    (void)ph_closed;
    return ph.window;
}

void photos_open_file(const char *path)
{
    desktop_app_launch(desktop_app_find("Photos"));
    ph_thumbs_free();
    ph_load(path);
}
