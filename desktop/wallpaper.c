/*
 * =============================================================================
 * Kryzen OS — Wallpapers (desktop/wallpaper.c)
 * Language: C17
 *
 * The default wallpaper, Kryzen, is the cosmos picture built into the kernel
 * (desktop/assets/cosmos.jpg, rendered by scripts/gen_wallpapers.py). The
 * others are drawn by code at the current resolution: Bloom (follows the
 * theme), Aurora, Mountains, Dunes, Night sky, Waves and a solid colour. A
 * picture file (PNG, JPEG or BMP) can be used instead, with fill / fit /
 * stretch / centre / tile placement.
 *
 * The result is cached; it is rebuilt only when the choice, the theme or
 * the resolution changes. A blurred copy is kept next to it: frosted-glass
 * panels (ui_glass) are cut out of that copy.
 *
 * Sign-in, first-run setup and the installer use their own backdrop, the
 * nebula picture (desktop/assets/nebula.jpg), also with a blurred copy.
 * =============================================================================
 */

typedef enum {
    WP_KRYZEN = 0, WP_BLOOM, WP_AURORA, WP_MOUNTAINS, WP_DUNES, WP_NIGHT, WP_WAVES, WP_SOLID, WP_PICTURE, WP_COUNT
} wp_style_t;

static const char *const g_wp_names[WP_COUNT] = {
    "kryzen", "bloom", "aurora", "mountains", "dunes", "night", "waves", "solid", "picture"
};
static const char *const g_wp_titles[WP_COUNT] = {
    "Kryzen", "Bloom", "Aurora", "Mountains", "Dunes", "Night sky", "Waves", "Solid colour", "Picture"
};
static const char *const g_fit_names[5] = { "fill", "fit", "stretch", "center", "tile" };

static struct {
    uint32_t *cache;
    uint32_t *frost;            /* blurred copy for glass panels */
    size_t cache_bytes;
    int cache_w, cache_h;
    int valid;
} g_wp;

/* Backdrop of the full-screen screens (sign-in, setup, installer). */
static struct {
    uint32_t *px, *frost;
    size_t bytes;
    int w, h;
} g_bd;

/* Which picture glass panels are cut from: 0 desktop wallpaper, 1 backdrop. */
static int g_glass_backdrop;

/* Pictures built into the kernel (desktop/assets.S). */
extern const uint8_t asset_cosmos_jpg[], asset_cosmos_jpg_end[];
extern const uint8_t asset_nebula_jpg[], asset_nebula_jpg_end[];

/* ---- small maths (no libm in the kernel) ---------------------------------- */
#define WP_PI 3.14159265f

static float wp_sin(float x)
{
    x = x / (2 * WP_PI);
    x -= (float)(int)x;
    if (x < 0) x += 1;
    x = x * 2 * WP_PI - WP_PI;                  /* -pi..pi */
    float y = 1.27323954f * x - 0.405284735f * x * (x < 0 ? -x : x);
    return -(0.225f * (y * (y < 0 ? -y : y) - y) + y);
}

static float wp_exp_neg(float t)                /* e^-t for t >= 0 */
{
    if (t <= 0) return 1;
    if (t > 12) return 0;
    float b = 1 + t / 16;
    b *= b; b *= b; b *= b; b *= b;
    return 1 / b;
}

static uint32_t wp_hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

static float wp_noise1(float x, uint32_t seed)  /* value noise, 0..1 */
{
    int i = (int)x;
    if (x < 0 && (float)i != x) i--;
    float f = x - (float)i;
    float a = (float)(wp_hash((uint32_t)i * 374761393u + seed) & 0xFFFF) / 65535.0f;
    float b = (float)(wp_hash((uint32_t)(i + 1) * 374761393u + seed) & 0xFFFF) / 65535.0f;
    f = f * f * (3 - 2 * f);
    return a + (b - a) * f;
}

static float wp_fbm(float x, uint32_t seed, int oct)
{
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < oct; o++) {
        sum += amp * wp_noise1(x, seed + (uint32_t)o * 1013u);
        norm += amp;
        x *= 2.03f;
        amp *= 0.5f;
    }
    return sum / norm;
}

static uint32_t wp_mix(uint32_t a, uint32_t b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    int k = (int)(t * 256);
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (int)((a >> sh) & 255), cb = (int)((b >> sh) & 255);
        out |= (uint32_t)((ca * (256 - k) + cb * k) >> 8) << sh;
    }
    return out;
}

static uint32_t wp_add(uint32_t a, uint32_t col, float t)   /* additive glow */
{
    if (t <= 0) return a;
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int v = (int)((a >> sh) & 255) + (int)((float)((col >> sh) & 255) * t);
        out |= (uint32_t)(v > 255 ? 255 : v) << sh;
    }
    return out;
}

/* Vertical gradient through several stops (pos 0..1). */
static uint32_t wp_grad(const float *pos, const uint32_t *col, int n, float t)
{
    if (t <= pos[0]) return col[0];
    for (int i = 1; i < n; i++)
        if (t <= pos[i]) return wp_mix(col[i - 1], col[i], (t - pos[i - 1]) / (pos[i] - pos[i - 1]));
    return col[n - 1];
}

static void wp_stars(uint32_t *px, int W, int H, float max_y, int density)
{
    for (int y = 0; y < (int)(H * max_y); y++)
        for (int x = 0; x < W; x++) {
            uint32_t h = wp_hash((uint32_t)(y * 7919 + x) * 2654435761u);
            if ((int)(h % 10000) >= density) continue;
            float b = (float)((h >> 16) & 255) / 255.0f;
            float fade = 1 - (float)y / (H * max_y);
            px[(size_t)y * W + x] = wp_add(px[(size_t)y * W + x], 0x00FFFFFF, (0.25f + 0.75f * b) * fade);
        }
}

static void wp_glow(uint32_t *px, int W, int H, int cx, int cy, float r, uint32_t col, float strength)
{
    int R = (int)(r * 4);
    for (int y = cy - R; y < cy + R; y++) {
        if (y < 0 || y >= H) continue;
        for (int x = cx - R; x < cx + R; x++) {
            if (x < 0 || x >= W) continue;
            float dx = (float)(x - cx), dy = (float)(y - cy);
            float d2 = (dx * dx + dy * dy) / (r * r);
            if (d2 >= 16) continue;
            float edge = 1 - d2 / 16;                       /* fades to exactly 0 at the edge */
            px[(size_t)y * W + x] = wp_add(px[(size_t)y * W + x], col, strength * edge * edge / (1 + d2 * 1.6f));
        }
    }
}

static void wp_disc(uint32_t *px, int W, int H, int cx, int cy, float r, uint32_t col)
{
    for (int y = cy - (int)r - 2; y <= cy + (int)r + 2; y++) {
        if (y < 0 || y >= H) continue;
        for (int x = cx - (int)r - 2; x <= cx + (int)r + 2; x++) {
            if (x < 0 || x >= W) continue;
            float dx = (float)(x - cx), dy = (float)(y - cy);
            float d = dx * dx + dy * dy, rr = r * r;
            if (d <= (r - 1) * (r - 1)) px[(size_t)y * W + x] = col;
            else if (d <= rr + r) {
                float a = (rr + r - d) / (2 * r);
                px[(size_t)y * W + x] = wp_mix(px[(size_t)y * W + x], col, a);
            }
        }
    }
}

/* ---- the designs ------------------------------------------------------------ */
static void wp_bloom(void)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    fb_reset_clip();

    /* 1. Base dark obsidian cosmic background */
    fb_gradient_v(0, 0, W, H, 0x00080B11, 0x000B101A);
    if (theme_get_mode() == THEME_HIGH_CONTRAST) return;

    /* 2. Cosmic glowing nebulas/mesh waves:
     * - Top-left cosmic purple/blue (#161c3b / #1e1338)
     * - Top-right cyan/deep blue (#052b36 / #182352)
     * - Bottom-center deep violet/teal (#260f3d / #0b2136)
     */
    int r1 = (W * 35) / 100;
    fb_blend_rounded_rect(-r1 / 4, -r1 / 4, r1 * 2, r1 * 2, r1, 0x001E1338, 55);
    fb_blend_rounded_rect(-r1 / 6, -r1 / 6, r1 * 3 / 2, r1 * 3 / 2, r1 * 3 / 4, 0x00161C3B, 70);
    fb_blend_rounded_rect(0, 0, r1, r1, r1 / 2, 0x008083FF, 20);

    int r2 = (W * 32) / 100;
    int cx2 = W + r2 / 6, cy2 = (H * 35) / 100;
    fb_blend_rounded_rect(cx2 - r2, cy2 - r2, r2 * 2, r2 * 2, r2, 0x00052B36, 65);
    fb_blend_rounded_rect(cx2 - r2 * 3 / 4, cy2 - r2 * 3 / 4, r2 * 3 / 2, r2 * 3 / 2, r2 * 3 / 4, 0x00182352, 50);
    fb_blend_rounded_rect(cx2 - r2 / 2, cy2 - r2 / 2, r2, r2, r2 / 2, 0x004CD7F6, 18);

    int r3 = (W * 28) / 100;
    int cx3 = (W * 40) / 100, cy3 = H + r3 / 6;
    fb_blend_rounded_rect(cx3 - r3, cy3 - r3, r3 * 2, r3 * 2, r3, 0x00260F3D, 55);
    fb_blend_rounded_rect(cx3 - r3 * 3 / 4, cy3 - r3 * 3 / 4, r3 * 3 / 2, r3 * 3 / 2, r3 * 3 / 4, 0x000B2136, 45);
    fb_blend_rounded_rect(cx3 - r3 / 2, cy3 - r3 / 2, r3, r3, r3 / 2, 0x00B76DFF, 18);

    /* 3. Cyber Geometric Mesh lines / Aura */
    int prev_y1 = -1;
    for (int x = 0; x < W; x += 2) {
        float u = (float)x / (float)W;
        int y1 = (int)(H * 0.28f + H * 0.12f * wp_sin(u * 2.0f * WP_PI * 1.2f) - H * 0.05f * wp_sin(u * 2.0f * WP_PI * 2.4f));
        if (prev_y1 >= 0 && y1 >= 0 && y1 < H) {
            fb_blend_rect(x, y1, 2, 2, 0x004CD7F6, 50);
        }
        prev_y1 = y1;
    }

    for (int x = 0; x < W; x += 6) {
        float u = (float)x / (float)W;
        int y2 = (int)(H * 0.52f + H * 0.15f * wp_sin(u * 2.0f * WP_PI * 0.9f + 1.2f) + H * 0.06f * wp_sin(u * 2.0f * WP_PI * 2.1f));
        if (y2 >= 0 && y2 < H && (x / 6) % 3 != 0) {
            fb_blend_rect(x, y2, 3, 2, 0x008083FF, 42);
        }
    }

    for (int x = 0; x < W; x += 3) {
        float u = (float)x / (float)W;
        int y3 = (int)(H * 0.72f - H * 0.10f * wp_sin(u * 2.0f * WP_PI * 1.5f + 2.5f));
        if (y3 >= 0 && y3 < H) {
            fb_blend_rect(x, y3, 2, 2, 0x00B76DFF, 35);
        }
    }

    /* 4. Micro grid raster texture overlay (dots spaced every 28px) */
    for (int gy = 14; gy < H; gy += 28) {
        for (int gx = 14; gx < W; gx += 28) {
            fb_blend_rect(gx, gy, 1, 1, 0x00C0C1FF, 32);
        }
    }
}

static void wp_aurora(uint32_t *px, int W, int H)
{
    static const float gp[3] = { 0, 0.7f, 1 };
    static const uint32_t gc[3] = { 0x00010410, 0x00061a33, 0x000c2a45 };
    for (int y = 0; y < H; y++) {
        uint32_t c = wp_grad(gp, gc, 3, (float)y / H);
        for (int x = 0; x < W; x++) px[(size_t)y * W + x] = c;
    }
    wp_stars(px, W, H, 0.85f, 18);
    static const uint32_t low[3] = { 0x0030F0A0, 0x0020D8C0, 0x0040E070 };
    static const uint32_t high[3] = { 0x007A4CF0, 0x003070FF, 0x00B050D0 };
    for (int b = 0; b < 3; b++) {
        float base = H * (0.38f + 0.1f * b), ph = 1.7f * b;
        for (int x = 0; x < W; x++) {
            float u = (float)x / W;
            float yc = base + H * 0.07f * wp_sin(u * 2 * WP_PI * (1.1f + 0.4f * b) + ph)
                            + H * 0.03f * wp_sin(u * 2 * WP_PI * (3.3f + b) + 2 * ph);
            float hb = H * (0.16f + 0.1f * wp_fbm(u * 6 + b * 10, 77 + b, 3));
            float rays = 0.55f + 0.45f * wp_fbm(u * 90 + b * 31, 991 + b, 2);
            float amp = (0.55f - 0.12f * b) * (0.4f + 0.6f * wp_fbm(u * 3 + b, 13 + b, 2));
            for (int y = (int)(yc - hb); y < (int)yc + H / 12; y++) {
                if (y < 0 || y >= H) continue;
                float d = yc - (float)y, t;
                if (d < 0) t = wp_exp_neg(-d / (H * 0.012f));
                else { float k = 1 - d / hb; t = k > 0 ? k * k * rays : 0; }
                float mixv = d > 0 ? d / hb : 0;
                uint32_t col = wp_mix(low[b], high[b], mixv);
                px[(size_t)y * W + x] = wp_add(px[(size_t)y * W + x], col, t * amp);
            }
        }
    }
    /* dark hills along the bottom */
    for (int x = 0; x < W; x++) {
        float u = (float)x / W;
        int top = (int)(H * (0.86f - 0.06f * wp_fbm(u * 5, 4242, 4)));
        for (int y = top; y < H; y++) px[(size_t)y * W + x] = wp_mix(0x00020812, 0x00000306, (float)(y - top) / (H - top + 1));
    }
}

static void wp_mountains(uint32_t *px, int W, int H)
{
    static const float gp[4] = { 0, 0.35f, 0.55f, 0.7f };
    static const uint32_t gc[4] = { 0x001A1440, 0x00663A78, 0x00D86A6A, 0x00FFB070 };
    for (int y = 0; y < H; y++) {
        uint32_t c = wp_grad(gp, gc, 4, (float)y / H);
        for (int x = 0; x < W; x++) px[(size_t)y * W + x] = c;
    }
    int sx = W * 66 / 100, sy = H * 52 / 100;
    wp_glow(px, W, H, sx, sy, H * 0.09f, 0x00FFB070, 0.55f);
    wp_disc(px, W, H, sx, sy, H * 0.055f, 0x00FFE8C0);
    static const uint32_t layer[5] = { 0x00B0607E, 0x00844A72, 0x00593662, 0x00382450, 0x001C1230 };
    for (int l = 0; l < 5; l++) {
        float base = H * (0.50f + 0.095f * l), amp = H * (0.13f + 0.025f * l), freq = 2.2f + 0.9f * l;
        for (int x = 0; x < W; x++) {
            float u = (float)x / W;
            float n = wp_fbm(u * freq + 3.7f * l, 1234u + (uint32_t)l * 77u, 6);
            n = 1 - (n > 0.5f ? (n - 0.5f) * 2 : (0.5f - n) * 2) * 0.35f - 0.3f * (1 - n);   /* sharper peaks */
            int top = (int)(base - amp * n);
            if (top < 0) top = 0;
            for (int y = top; y < H; y++) {
                float depth = (float)(y - top) / (H * 0.25f);
                uint32_t c = wp_mix(layer[l], 0x00100A20, depth * 0.35f);
                /* haze at the foot of each range */
                px[(size_t)y * W + x] = c;
            }
        }
    }
}

static void wp_dunes(uint32_t *px, int W, int H)
{
    static const float gp[3] = { 0, 0.4f, 0.6f };
    static const uint32_t gc[3] = { 0x0076A9D6, 0x00F1C7A0, 0x00FBD8A8 };
    for (int y = 0; y < H; y++) {
        uint32_t c = wp_grad(gp, gc, 3, (float)y / H);
        for (int x = 0; x < W; x++) px[(size_t)y * W + x] = c;
    }
    wp_glow(px, W, H, W / 4, H * 40 / 100, H * 0.06f, 0x00FFF2D0, 0.6f);
    static const uint32_t lit[5] = { 0x00F3C08A, 0x00EDAA6C, 0x00E3914F, 0x00D0763A, 0x00B65C2A };
    static const uint32_t shade[5] = { 0x00D49A6E, 0x00C6834F, 0x00B26A38, 0x00994F26, 0x007C3A18 };
    for (int l = 0; l < 5; l++) {
        float base = H * (0.55f + 0.09f * l), ph = 1.3f * l;
        float prev = 0;
        for (int x = 0; x < W; x++) {
            float u = (float)x / W;
            float yv = base - H * 0.05f * wp_sin(u * 2 * WP_PI * (0.8f + 0.25f * l) + ph)
                            - H * 0.025f * wp_sin(u * 2 * WP_PI * (2.1f + 0.3f * l) + 2 * ph);
            float slope = x ? yv - prev : 0;
            prev = yv;
            uint32_t c = slope < 0 ? lit[l] : shade[l];
            float k = slope < 0 ? -slope : slope;
            c = wp_mix(lit[l], shade[l], 0.5f + (slope < 0 ? -1 : 1) * (k * 4 > 0.5f ? 0.5f : k * 4));
            for (int y = (int)yv; y < H; y++)
                if (y >= 0) px[(size_t)y * W + x] = wp_mix(c, shade[l], (float)(y - (int)yv) / (H * 0.3f));
        }
    }
}

static void wp_night(uint32_t *px, int W, int H)
{
    static const float gp[3] = { 0, 0.6f, 1 };
    static const uint32_t gc[3] = { 0x00030820, 0x00102552, 0x00243C78 };
    for (int y = 0; y < H; y++) {
        uint32_t c = wp_grad(gp, gc, 3, (float)y / H);
        for (int x = 0; x < W; x++) px[(size_t)y * W + x] = c;
    }
    wp_stars(px, W, H, 0.9f, 35);
    int mx = W * 78 / 100, my = H * 22 / 100;
    wp_glow(px, W, H, mx, my, H * 0.06f, 0x00A0B8FF, 0.5f);
    wp_disc(px, W, H, mx, my, H * 0.045f, 0x00F4F1E6);
    wp_disc(px, W, H, mx + (int)(H * 0.018f), my - (int)(H * 0.012f), H * 0.009f, 0x00DCD8CC);
    static const uint32_t hill[3] = { 0x00122048, 0x000A1430, 0x0004081A };
    for (int l = 0; l < 3; l++)
        for (int x = 0; x < W; x++) {
            float u = (float)x / W;
            int top = (int)(H * (0.72f + 0.08f * l) - H * 0.08f * wp_fbm(u * (2 + l) + l * 5, 555u + (uint32_t)l, 5));
            for (int y = top; y < H; y++) px[(size_t)y * W + x] = hill[l];
        }
}

static void wp_waves(uint32_t *px, int W, int H)
{
    static const float gp[3] = { 0, 0.5f, 1 };
    static const uint32_t gc[3] = { 0x00021428, 0x00063A64, 0x000B5C8C };
    for (int y = 0; y < H; y++) {
        uint32_t c = wp_grad(gp, gc, 3, (float)y / H);
        for (int x = 0; x < W; x++) px[(size_t)y * W + x] = c;
    }
    static const uint32_t col[6] = { 0x0035C8FF, 0x0030A0F0, 0x004080FF, 0x0050E0E0, 0x002A70D0, 0x0060B0FF };
    for (int l = 0; l < 6; l++) {
        float base = H * (0.40f + 0.08f * l), ph = 0.9f * l;
        for (int x = 0; x < W; x++) {
            float u = (float)x / W;
            float yv = base + H * 0.07f * wp_sin(u * 2 * WP_PI * (0.6f + 0.15f * l) + ph)
                            + H * 0.02f * wp_sin(u * 2 * WP_PI * (1.9f + 0.2f * l) - ph);
            for (int y = (int)yv; y < H; y++) {
                if (y < 0) continue;
                float d = (float)y - yv;
                float edge = d < 2 ? 0.35f : 0.12f * wp_exp_neg(d / (H * 0.08f)) + 0.04f;
                px[(size_t)y * W + x] = wp_add(px[(size_t)y * W + x], col[l], edge);
            }
        }
    }
}

static uint32_t wp_solid_color(void)
{
    char b[16];
    setting_get("wallpaper_color", b, sizeof(b));
    uint32_t v = 0;
    const char *p = b[0] == '#' ? b + 1 : b;
    int n = 0;
    for (; *p && n < 6; p++, n++) {
        char c = *p;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + (uint32_t)d;
    }
    return n == 6 ? v : 0x00204060;
}

/* Places a decoded picture on a W x H canvas.
 * fit: 0 fill (cover), 1 fit (contain), 2 stretch, 3 centre, 4 tile. */
static int wp_place(uint32_t *px, int W, int H, const image_t *img, int fit, uint32_t bg)
{
    for (size_t i = 0; i < (size_t)W * H; i++) px[i] = bg;

    if (fit == 4) {                                     /* tile */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                px[(size_t)y * W + x] = img->px[(size_t)(y % img->h) * img->w + (x % img->w)];
    } else if (fit == 3) {                              /* centre, no scaling */
        int ox = (W - img->w) / 2, oy = (H - img->h) / 2;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int sx = x - ox, sy = y - oy;
                if (sx >= 0 && sy >= 0 && sx < img->w && sy < img->h) px[(size_t)y * W + x] = img->px[(size_t)sy * img->w + sx];
            }
    } else {
        int dw = W, dh = H;
        if (fit == 0 || fit == 1) {                     /* fill (cover) / fit (contain) */
            int64_t a = (int64_t)W * img->h, b = (int64_t)H * img->w;
            int wider = a > b;                          /* screen is wider than the picture */
            if ((fit == 0) == wider) { dw = W; dh = (int)((int64_t)img->h * W / img->w); }
            else { dh = H; dw = (int)((int64_t)img->w * H / img->h); }
        }
        if (dw == W && dh == H) { image_scale(img, px, W, H); return 0; }
        size_t bytes = (size_t)dw * dh * 4;
        uint32_t *tmp = (uint32_t *)(bytes >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(bytes) : kmalloc(bytes));
        if (!tmp) return -1;
        image_scale(img, tmp, dw, dh);
        int ox = (W - dw) / 2, oy = (H - dh) / 2;
        for (int y = 0; y < dh; y++) {
            int ty = y + oy;
            if (ty < 0 || ty >= H) continue;
            for (int x = 0; x < dw; x++) {
                int tx = x + ox;
                if (tx >= 0 && tx < W) px[(size_t)ty * W + tx] = tmp[(size_t)y * dw + x];
            }
        }
        if (bytes >= LARGE_ALLOC_THRESHOLD) kfree_large(tmp, bytes); else kfree(tmp);
    }
    return 0;
}

static int wp_picture(uint32_t *px, int W, int H, char *err, size_t errn)
{
    char path[256];
    setting_get("wallpaper_picture", path, sizeof(path));
    image_t img;
    if (!path[0] || image_load(path, &img, err, errn)) return -1;
    int rc = wp_place(px, W, H, &img, setting_get_int("wallpaper_fit", 0), wp_solid_color());
    image_free(&img);
    if (rc) snprintf(err, errn, "Out of memory");
    return rc;
}

/* A picture built into the kernel, scaled to cover the canvas. */
static int wp_builtin(uint32_t *px, int W, int H, const uint8_t *data, const uint8_t *end)
{
    image_t img;
    char err[64];
    if (image_decode(data, (size_t)(end - data), &img, err, sizeof(err)) != 0) {
        terminal_printf("[WALLPAPER] built-in picture: %s\n", err);
        return -1;
    }
    int rc = wp_place(px, W, H, &img, 0, 0x0005060F);
    image_free(&img);
    return rc;
}

/* Bloom drawn into a buffer (for previews): gradient plus soft circles. */
static void wp_bloom_buf(uint32_t *px, int W, int H)
{
    uint32_t top = T(WALL_TOP), bot = T(WALL_BOTTOM);
    uint32_t glow = (theme_get_mode() == THEME_LIGHT) ? 0x00FFFFFF : T(ACCENT);
    for (int y = 0; y < H; y++) {
        uint32_t c = wp_mix(top, bot, (float)y / (H - 1));
        for (int x = 0; x < W; x++) {
            uint32_t p = c;
            float dx = (float)(x - W / 2), dy = (float)(y - (H + H / 6));
            float d = dx * dx + dy * dy;
            static const int pct[4] = { 95, 72, 52, 34 };
            for (int i = 0; i < 4; i++) { float r = H * pct[i] / 100.0f; if (d < r * r) p = wp_mix(p, glow, 16 / 255.0f); }
            px[(size_t)y * W + x] = p;
        }
    }
}

/* Small preview of a wallpaper style (Settings > Personalization). */
#define WP_THUMB_W 76
#define WP_THUMB_H 43
const uint32_t *wallpaper_thumb(int style)
{
    static uint32_t thumbs[WP_COUNT][WP_THUMB_W * WP_THUMB_H];
    static int valid[WP_COUNT];
    static int bloom_key = -1;
    static char pic_key[256];
    int key = (int)theme_get_mode() * 64 + setting_get_int("accent", 0) * 8 + setting_get_int("wallpaper", 0);
    uint32_t *px = thumbs[style];
    if (style == WP_BLOOM && bloom_key != key) { valid[style] = 0; bloom_key = key; }
    if (style == WP_SOLID) valid[style] = 0;
    if (style == WP_PICTURE) {
        char p[256];
        setting_get("wallpaper_picture", p, sizeof(p));
        if (strcmp(p, pic_key)) { valid[style] = 0; snprintf(pic_key, sizeof(pic_key), "%s", p); }
    }
    if (valid[style]) return px;
    valid[style] = 1;
    switch (style) {
    case WP_KRYZEN:
        if (wp_builtin(px, WP_THUMB_W, WP_THUMB_H, asset_cosmos_jpg, asset_cosmos_jpg_end))
            for (int i = 0; i < WP_THUMB_W * WP_THUMB_H; i++) px[i] = 0x000A0F2A;
        break;
    case WP_BLOOM:     wp_bloom_buf(px, WP_THUMB_W, WP_THUMB_H); break;
    case WP_AURORA:    wp_aurora(px, WP_THUMB_W, WP_THUMB_H); break;
    case WP_MOUNTAINS: wp_mountains(px, WP_THUMB_W, WP_THUMB_H); break;
    case WP_DUNES:     wp_dunes(px, WP_THUMB_W, WP_THUMB_H); break;
    case WP_NIGHT:     wp_night(px, WP_THUMB_W, WP_THUMB_H); break;
    case WP_WAVES:     wp_waves(px, WP_THUMB_W, WP_THUMB_H); break;
    case WP_SOLID: {
        uint32_t c = wp_solid_color();
        for (int i = 0; i < WP_THUMB_W * WP_THUMB_H; i++) px[i] = c;
        break;
    }
    default: {
        image_t img;
        char err[64];
        if (pic_key[0] && image_load(pic_key, &img, err, sizeof(err)) == 0) { image_scale(&img, px, WP_THUMB_W, WP_THUMB_H); image_free(&img); }
        else for (int i = 0; i < WP_THUMB_W * WP_THUMB_H; i++) px[i] = 0x00303438;
        break;
    }
    }
    return px;
}

/* ---- public ------------------------------------------------------------------- */
int wallpaper_style(void)
{
    char b[16];
    setting_get("wallpaper_style", b, sizeof(b));
    for (int i = 0; i < WP_COUNT; i++)
        if (!strcmp(b, g_wp_names[i])) return i;
    return WP_KRYZEN;
}

const char *wallpaper_style_name(int i) { return i >= 0 && i < WP_COUNT ? g_wp_names[i] : ""; }
const char *wallpaper_style_title(int i) { return i >= 0 && i < WP_COUNT ? g_wp_titles[i] : ""; }
const char *wallpaper_fit_name(int i)   { return i >= 0 && i < 5 ? g_fit_names[i] : ""; }

static char g_wp_error[96];
const char *wallpaper_last_error(void) { return g_wp_error; }

static void wallpaper_render(void)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    uint32_t *px = fb_get_base();
    int style = wallpaper_style();
    g_wp_error[0] = 0;
    if (theme_get_mode() == THEME_HIGH_CONTRAST && style != WP_PICTURE) style = WP_BLOOM;
    switch (style) {
    case WP_KRYZEN:
        /* drawn into the cache (stride == width), then shown from there */
        if (!g_wp.cache || wp_builtin(g_wp.cache, W, H, asset_cosmos_jpg, asset_cosmos_jpg_end)) wp_bloom();
        else fb_load_frame(g_wp.cache);
        break;
    case WP_AURORA:    wp_aurora(px, W, H); break;
    case WP_MOUNTAINS: wp_mountains(px, W, H); break;
    case WP_DUNES:     wp_dunes(px, W, H); break;
    case WP_NIGHT:     wp_night(px, W, H); break;
    case WP_WAVES:     wp_waves(px, W, H); break;
    case WP_SOLID:     fb_fill_rect(0, 0, W, H, wp_solid_color()); break;
    case WP_PICTURE:
        if (wp_picture(px, W, H, g_wp_error, sizeof(g_wp_error))) wp_bloom();
        break;
    default:           wp_bloom(); break;
    }
}

void desktop_wallpaper_invalidate(void)
{
    g_wp.valid = 0;
}

/* Copies the wallpaper into the back buffer, rendering it first if needed. */
static void wallpaper_draw(void)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    g_glass_backdrop = 0;
    if (g_wp.cache && (g_wp.cache_w != W || g_wp.cache_h != H)) {
        kfree_large(g_wp.cache, g_wp.cache_bytes);
        if (g_wp.frost) kfree_large(g_wp.frost, g_wp.cache_bytes);
        g_wp.cache = g_wp.frost = NULL;
        g_wp.valid = 0;
    }
    if (!g_wp.cache) {
        g_wp.cache_bytes = (size_t)W * H * 4;
        g_wp.cache = (uint32_t *)kmalloc_large(g_wp.cache_bytes);
        g_wp.frost = g_wp.cache ? (uint32_t *)kmalloc_large(g_wp.cache_bytes) : NULL;
        g_wp.cache_w = W; g_wp.cache_h = H;
    }
    fb_reset_clip();
    if (!g_wp.cache) { wallpaper_render(); return; }
    if (!g_wp.valid) {
        wallpaper_render();
        fb_save_frame(g_wp.cache);
        if (g_wp.frost) fb_blur_frame(g_wp.cache, g_wp.frost, W, H, 28);
        g_wp.valid = 1;
        return;
    }
    fb_load_frame(g_wp.cache);
}

/* Sign-in, setup and installer screens: the nebula backdrop. dim darkens it
 * (0..255) so text stays readable. */
void wallpaper_paint_backdrop(int dim)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    fb_reset_clip();
    g_glass_backdrop = 1;
    if (g_bd.px && (g_bd.w != W || g_bd.h != H)) {
        kfree_large(g_bd.px, g_bd.bytes);
        if (g_bd.frost) kfree_large(g_bd.frost, g_bd.bytes);
        g_bd.px = g_bd.frost = NULL;
    }
    if (!g_bd.px) {
        g_bd.bytes = (size_t)W * H * 4;
        g_bd.w = W; g_bd.h = H;
        g_bd.px = (uint32_t *)kmalloc_large(g_bd.bytes);
        if (g_bd.px && wp_builtin(g_bd.px, W, H, asset_nebula_jpg, asset_nebula_jpg_end) != 0) {
            for (size_t i = 0; i < (size_t)W * H; i++) g_bd.px[i] = 0x00080B1C;
        }
        if (g_bd.px) {
            g_bd.frost = (uint32_t *)kmalloc_large(g_bd.bytes);
            if (g_bd.frost) fb_blur_frame(g_bd.px, g_bd.frost, W, H, 28);
        }
    }
    if (g_bd.px && theme_get_mode() != THEME_HIGH_CONTRAST) fb_load_frame(g_bd.px);
    else fb_clear(0x00000000);
    if (dim > 0) fb_blend_rect(0, 0, W, H, 0x00000000, (uint8_t)dim);
}

/* ---- Glass ------------------------------------------------------------------
 * A frosted panel: the blurred picture behind it, tinted, with a hairline
 * edge and a faint highlight along the top. High contrast: a plain box.
 */
void ui_glass_tinted(int x, int y, int w, int h, int r, uint32_t tint, int tint_alpha)
{
    if (theme_get_mode() == THEME_HIGH_CONTRAST) {
        fb_fill_rounded_rect(x, y, w, h, r, 0x00000000);
        fb_draw_rounded_rect(x, y, w, h, r, 0x00FFFFFF);
        return;
    }
    const uint32_t *frost = g_glass_backdrop ? g_bd.frost : g_wp.frost;
    if (theme_get_mode() == THEME_LIGHT && !g_glass_backdrop) { tint = 0x00F2F4FA; if (tint_alpha < 190) tint_alpha = 190; }
    if (frost) fb_blit_rounded(frost, x, y, w, h, r, tint, (uint8_t)tint_alpha);
    else fb_blend_rounded_rect(x, y, w, h, r, tint, 235);
    fb_blend_rect(x + r, y + 1, w - 2 * r, 1, 0x00FFFFFF, 26);
    uint32_t edge = theme_get_mode() == THEME_LIGHT && !g_glass_backdrop ? 0x00707890 : 0x00C8D8FF;
    fb_neon_rounded_rect(x, y, w, h, r, 1, 0, edge, edge, 58);
}

void ui_glass(int x, int y, int w, int h, int r)
{
    ui_glass_tinted(x, y, w, h, r, 0x00262A46, 150);
}

/* The signature cyan-to-violet outline with its glow. */
void ui_neon_frame(int x, int y, int w, int h, int r, int thickness, int glow)
{
    if (theme_get_mode() == THEME_HIGH_CONTRAST) { fb_draw_rounded_rect(x, y, w, h, r, 0x00FFFF00); return; }
    fb_neon_rounded_rect(x, y, w, h, r, thickness, glow, UI_NEON_CYAN, UI_NEON_VIOLET, 255);
}
