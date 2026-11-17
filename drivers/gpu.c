/*
 * =============================================================================
 * JARVIS AI OS — GPU Framebuffer Driver  (drivers/gpu.c)
 * Language: C17
 *
 * Provides a linear framebuffer for the JARVIS UI shell.
 * Primary path: Multiboot2 framebuffer supplied by GRUB.
 * Fallback:     QEMU stdvga/Bochs VBE linear framebuffer.
 *
 * Drawing is double buffered: see the BACK BUFFER section below.
 *
 * Pixel format: 32-bit, 0x00RRGGBB.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "gpu.h"
#include <string.h>

static void fb_setup_backbuffer(void);

/* ---- Framebuffer descriptor ---------------------------------------------- */
typedef struct {
    uint32_t *base;      /* virtual address of framebuffer pixels       */
    uint64_t  phys;      /* physical address (for re-mapping on a mode change) */
    size_t    mapped;    /* bytes mapped at base                         */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;     /* bytes per row (may have padding)             */
    uint8_t   bpp;       /* bits per pixel                               */
} framebuffer_t;

static framebuffer_t g_fb = {};

/* ---- Boot framebuffer (set by bootloader) -------------------------------- */
/*
 * The Multiboot2 shim copies GRUB framebuffer info here before jumping to the
 * 64-bit kernel. The legacy stage2 path may also populate the same structure.
 */
typedef struct {
    uint64_t fb_base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} gop_info_t;

/* This is written by the bootloader at a fixed physical address */
#define GOP_INFO_PADDR 0x7000

#define PCI_CLASS_DISPLAY 0x03
#define QEMU_STD_VGA_VENDOR 0x1234
#define QEMU_STD_VGA_DEVICE 0x1111

#define BOCHS_VBE_INDEX_PORT 0x01CE
#define BOCHS_VBE_DATA_PORT  0x01CF
#define BOCHS_VBE_INDEX_ID 0
#define BOCHS_VBE_INDEX_XRES 1
#define BOCHS_VBE_INDEX_YRES 2
#define BOCHS_VBE_INDEX_BPP 3
#define BOCHS_VBE_INDEX_ENABLE 4
#define BOCHS_VBE_INDEX_VIRT_WIDTH 6
#define BOCHS_VBE_INDEX_VIRT_HEIGHT 7
#define BOCHS_VBE_INDEX_X_OFFSET 8
#define BOCHS_VBE_INDEX_Y_OFFSET 9
#define BOCHS_VBE_ENABLED 0x01
#define BOCHS_VBE_LFB_ENABLED 0x40

static void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}

static uint16_t inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void bochs_vbe_write(uint16_t index, uint16_t value)
{
    outw(BOCHS_VBE_INDEX_PORT, index);
    outw(BOCHS_VBE_DATA_PORT, value);
}

static uint16_t bochs_vbe_read(uint16_t index)
{
    outw(BOCHS_VBE_INDEX_PORT, index);
    return inw(BOCHS_VBE_DATA_PORT);
}

static int bochs_vbe_set_mode(uint16_t width, uint16_t height, uint16_t bpp)
{
    uint16_t id = bochs_vbe_read(BOCHS_VBE_INDEX_ID);

    if (id < 0xB0C0 || id > 0xB0C5) {
        terminal_printf("[GPU] Bochs VBE registers not detected (id=0x%x)\n", id);
        return 0;
    }

    bochs_vbe_write(BOCHS_VBE_INDEX_ENABLE, 0);
    bochs_vbe_write(BOCHS_VBE_INDEX_XRES, width);
    bochs_vbe_write(BOCHS_VBE_INDEX_YRES, height);
    bochs_vbe_write(BOCHS_VBE_INDEX_BPP, bpp);
    bochs_vbe_write(BOCHS_VBE_INDEX_VIRT_WIDTH, width);
    bochs_vbe_write(BOCHS_VBE_INDEX_VIRT_HEIGHT, height);
    bochs_vbe_write(BOCHS_VBE_INDEX_X_OFFSET, 0);
    bochs_vbe_write(BOCHS_VBE_INDEX_Y_OFFSET, 0);
    bochs_vbe_write(BOCHS_VBE_INDEX_ENABLE,
                    BOCHS_VBE_ENABLED | BOCHS_VBE_LFB_ENABLED);

    return 1;
}

static uint64_t pci_mem_bar32(uint32_t bar)
{
    if (bar == 0 || (bar & 0x1))
        return 0;
    return (uint64_t)(bar & 0xFFFFFFF0u);
}

static int gpu_try_qemu_stdvga_fallback(void)
{
    pci_device_t *display = pci_find_class(PCI_CLASS_DISPLAY, 0xFF);
    if (!display) {
        terminal_puts("[GPU] No PCI display adapter available for fallback\n");
        return 0;
    }

    terminal_printf("[GPU] Display PCI %02x:%02x.%x %04x:%04x bar0=0x%x\n",
        display->bus, display->dev, display->fn,
        display->vendor_id, display->device_id, display->bar[0]);

    if (display->vendor_id != QEMU_STD_VGA_VENDOR ||
        display->device_id != QEMU_STD_VGA_DEVICE) {
        terminal_puts("[GPU] Display adapter is not QEMU stdvga; no safe fallback mode\n");
        return 0;
    }

    uint32_t width = 1024;
    uint32_t height = 768;
    uint32_t pitch = width * 4;
    uint64_t fb_phys = pci_mem_bar32(display->bar[0]);
    size_t fb_size = (size_t)pitch * height;

    if (fb_phys == 0) {
        terminal_puts("[GPU] QEMU stdvga BAR0 missing; using conventional LFB address 0xfd000000\n");
        fb_phys = 0xfd000000ULL;
    }

    terminal_puts("[GPU] Falling back to QEMU stdvga Bochs VBE mode (1024x768x32)\n");
    if (!bochs_vbe_set_mode((uint16_t)width, (uint16_t)height, 32))
        return 0;

    g_fb.base = (uint32_t *)vmm_map_range(fb_phys, fb_size,
                                          VMM_PROT_RW | VMM_PROT_NOCACHE);
    if (!g_fb.base) {
        terminal_printf("[GPU] QEMU stdvga framebuffer mapping failed at 0x%llx\n",
            fb_phys);
        return 0;
    }

    g_fb.width = width;
    g_fb.height = height;
    g_fb.pitch = pitch;
    g_fb.bpp = 32;
    g_fb.phys = fb_phys;
    g_fb.mapped = fb_size;

    terminal_printf("[GPU] QEMU stdvga framebuffer: %ux%u phys=0x%llx vaddr=%p\n",
        g_fb.width, g_fb.height, fb_phys, (void *)g_fb.base);

    fb_setup_backbuffer();
    return 1;
}

void gpu_framebuffer_init(void)
{
    terminal_puts("[GPU] Initialising framebuffer\n");

    /* Read GOP info left by bootloader */
    gop_info_t *gop = (gop_info_t *)vmm_map_range(GOP_INFO_PADDR,
                                                    sizeof(gop_info_t),
                                                    VMM_PROT_RW);
    if (!gop) {
        terminal_puts("[GPU] GOP info mapping failed\n");
        return;
    }

    terminal_printf("[GPU] Boot framebuffer: base=0x%llx width=%u height=%u pitch=%u bpp=%u\n",
        gop->fb_base, gop->width, gop->height, gop->pitch, gop->bpp);

    /* Validate framebuffer info was populated by bootloader */
    if (gop->fb_base == 0 || gop->width == 0 || gop->height == 0) {
        terminal_puts("[GPU] Boot framebuffer not populated (physical address is 0 or resolution is 0)\n");
    } else if (gop->width > 8192 || gop->height > 4320) {
        terminal_printf("[GPU] Boot framebuffer resolution out of range: %ux%u\n",
            gop->width, gop->height);
    } else if (gop->bpp != 32) {
        terminal_printf("[GPU] Boot framebuffer bpp unsupported: %u (expected 32)\n",
            gop->bpp);
    } else if (gop->pitch < gop->width * 4 || gop->pitch > 65536) {
        terminal_printf("[GPU] Boot framebuffer pitch invalid: %u bytes (expected %u-%u)\n",
            gop->pitch, gop->width * 4, 65536);
    } else {
        /* High memory addresses (>0x40000000) are valid for MMIO regions. */
        size_t fb_size = (size_t)gop->pitch * gop->height;

        terminal_printf("[GPU] Mapping boot framebuffer: %zu bytes at physical 0x%llx\n",
            fb_size, gop->fb_base);

        g_fb.base = (uint32_t *)vmm_map_range(gop->fb_base, fb_size,
                                              VMM_PROT_RW | VMM_PROT_NOCACHE);
        if (!g_fb.base) {
            terminal_printf("[GPU] Boot framebuffer mapping failed for address 0x%llx\n",
                gop->fb_base);
        } else {
            g_fb.width  = gop->width;
            g_fb.height = gop->height;
            g_fb.pitch  = gop->pitch;
            g_fb.bpp    = (uint8_t)gop->bpp;
            g_fb.phys   = gop->fb_base;
            g_fb.mapped = fb_size;

            terminal_printf("[GPU] ✓ Framebuffer: %ux%u at vaddr %p (%zu KB)\n",
                g_fb.width, g_fb.height, (void *)g_fb.base,
                fb_size / 1024);

            fb_setup_backbuffer();
            return;
        }
    }

    if (gpu_try_qemu_stdvga_fallback())
        return;

    terminal_puts("[GPU] ERROR: No usable framebuffer. Check GRUB gfxpayload or QEMU display device.\n");
}

/* ==========================================================================
 * BACK BUFFER
 *
 * Every drawing call below writes to g_draw, an ordinary RAM buffer. The
 * screen (g_fb.base, slow uncached video memory) is only written by
 * fb_present() and the cursor code. Before this existed, each frame was
 * cleared and rebuilt directly in video memory, so the display showed every
 * intermediate state: constant flicker and half-drawn windows.
 * ========================================================================== */
static uint32_t *g_back   = NULL;   /* off-screen buffer, NULL if alloc failed */
static uint32_t *g_draw   = NULL;   /* where primitives draw                   */
static uint32_t  g_stride = 0;      /* pixels per row of g_draw                */
static fb_clip_t g_clip;

/* Brightness and night light: applied while copying to the screen, so the
 * back buffer (and screenshots) keep the true colours. */
static inline void copy32(uint32_t *dst, const uint32_t *src, size_t n);
static uint8_t g_lut[3][256];
static int g_filter_on = 0;

static void copy_filtered(uint32_t *dst, const uint32_t *src, size_t n)
{
    if (!g_filter_on) { copy32(dst, src, n); return; }
    for (size_t i = 0; i < n; i++) {
        uint32_t p = src[i];
        dst[i] = ((uint32_t)g_lut[0][(p >> 16) & 255] << 16) | ((uint32_t)g_lut[1][(p >> 8) & 255] << 8) | g_lut[2][p & 255];
    }
}

static int g_cur_x = 0, g_cur_y = 0;
static int g_cur_visible = 1;
static int g_cur_drawn = 0, g_cur_drawn_x = 0, g_cur_drawn_y = 0;

static inline void fill32(uint32_t *dst, uint32_t v, size_t n)
{
    __asm__ volatile ("rep stosl" : "+D"(dst), "+c"(n) : "a"(v) : "memory");
}

static inline void copy32(uint32_t *dst, const uint32_t *src, size_t n)
{
    __asm__ volatile ("rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

static void fb_setup_backbuffer(void)
{
    size_t bytes = (size_t)g_fb.width * g_fb.height * 4;

    g_back = (uint32_t *)kmalloc_large(bytes);
    if (g_back) {
        g_draw   = g_back;
        g_stride = g_fb.width;
        terminal_printf("[GPU] Back buffer: %zu KB (double buffered)\n", bytes / 1024);
    } else {
        g_draw   = g_fb.base;
        g_stride = g_fb.pitch / 4;
        terminal_puts("[GPU] WARN: no back buffer; drawing directly (will flicker)\n");
    }

    fb_reset_clip();
    g_cur_x = (int)g_fb.width / 2;
    g_cur_y = (int)g_fb.height / 2;
    fb_clear(0x00000000);
    fb_present();
}

/* ---- Clipping ------------------------------------------------------------ */
void fb_reset_clip(void)
{
    g_clip.x0 = 0;
    g_clip.y0 = 0;
    g_clip.x1 = (int)g_fb.width;
    g_clip.y1 = (int)g_fb.height;
}

void fb_set_clip(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > (int)g_fb.width)  x1 = (int)g_fb.width;
    if (y1 > (int)g_fb.height) y1 = (int)g_fb.height;
    if (x1 < x) x1 = x;
    if (y1 < y) y1 = y;
    g_clip.x0 = x; g_clip.y0 = y; g_clip.x1 = x1; g_clip.y1 = y1;
}

fb_clip_t fb_get_clip(void) { return g_clip; }
void fb_restore_clip(fb_clip_t c) { g_clip = c; }

/* Clip a rectangle against the current clip. Returns 0 if nothing is left. */
static inline int clip_rect(int *x, int *y, int *w, int *h)
{
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < g_clip.x0) x0 = g_clip.x0;
    if (y0 < g_clip.y0) y0 = g_clip.y0;
    if (x1 > g_clip.x1) x1 = g_clip.x1;
    if (y1 > g_clip.y1) y1 = g_clip.y1;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

static inline int in_clip(int x, int y)
{
    return x >= g_clip.x0 && x < g_clip.x1 && y >= g_clip.y0 && y < g_clip.y1;
}

/* ---- Colour math --------------------------------------------------------- */
static inline uint32_t blend_px(uint32_t fg, uint32_t bg, uint32_t a)
{
    a += a >> 7;                     /* 0..255 -> 0..256 */
    uint32_t ia = 256 - a;
    uint32_t rb = (((fg & 0x00FF00FF) * a + (bg & 0x00FF00FF) * ia) >> 8) & 0x00FF00FF;
    uint32_t g  = (((fg & 0x0000FF00) * a + (bg & 0x0000FF00) * ia) >> 8) & 0x0000FF00;
    return rb | g;
}

uint32_t fb_alpha_blend(uint32_t fg, uint32_t bg, uint8_t alpha)
{
    if (alpha == 255) return fg & 0x00FFFFFF;
    if (alpha == 0)   return bg & 0x00FFFFFF;
    return blend_px(fg, bg, alpha);
}

/* ---- Primitives ---------------------------------------------------------- */
void fb_put_pixel(int x, int y, uint32_t color)
{
    if (!g_draw || !in_clip(x, y)) return;
    g_draw[(size_t)y * g_stride + (size_t)x] = color;
}

void fb_blend_pixel(int x, int y, uint32_t color, uint8_t alpha)
{
    if (!g_draw || !in_clip(x, y) || alpha == 0) return;
    uint32_t *p = &g_draw[(size_t)y * g_stride + (size_t)x];
    *p = (alpha == 255) ? color : blend_px(color, *p, alpha);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    if (!g_draw || !clip_rect(&x, &y, &w, &h)) return;
    uint32_t *row = g_draw + (size_t)y * g_stride + (size_t)x;
    for (int j = 0; j < h; j++, row += g_stride)
        fill32(row, color, (size_t)w);
}

void fb_blend_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha)
{
    if (alpha == 255) { fb_fill_rect(x, y, w, h, color); return; }
    if (alpha == 0 || !g_draw || !clip_rect(&x, &y, &w, &h)) return;
    uint32_t *row = g_draw + (size_t)y * g_stride + (size_t)x;
    for (int j = 0; j < h; j++, row += g_stride)
        for (int i = 0; i < w; i++)
            row[i] = blend_px(color, row[i], alpha);
}

void fb_draw_rect(int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0) return;
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (int guard = 0; guard < 8192; guard++) {
        fb_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void fb_clear(uint32_t color)
{
    if (!g_draw) return;
    for (uint32_t y = 0; y < g_fb.height; y++)
        fill32(g_draw + (size_t)y * g_stride, color, g_fb.width);
}

void fb_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    if (h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t a = (uint32_t)(j * 255) / (uint32_t)(h > 1 ? h - 1 : 1);
        fb_fill_rect(x, y + j, w, 1, blend_px(bottom, top, a));
    }
}

/* ---- Anti-aliased rounded shapes ----------------------------------------- */
/*
 * Coverage (0..16) of pixel (i,j) inside a quarter circle of radius r whose
 * centre sits at (r,r); (0,0) is the outermost corner pixel. 4x4 supersampled.
 */
static int corner_cov(int i, int j, int r)
{
    int R = 8 * r, R2 = R * R, n = 0;
    for (int sy = 0; sy < 4; sy++) {
        int dy = R - (8 * j + 2 * sy + 1);
        for (int sx = 0; sx < 4; sx++) {
            int dx = R - (8 * i + 2 * sx + 1);
            if (dx * dx + dy * dy <= R2) n++;
        }
    }
    return n;
}

void fb_blend_rounded_rect(int x, int y, int w, int h, int r,
                           uint32_t color, uint8_t alpha)
{
    if (w <= 0 || h <= 0 || alpha == 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { fb_blend_rect(x, y, w, h, color, alpha); return; }

    fb_blend_rect(x, y + r, w, h - 2 * r, color, alpha);

    for (int j = 0; j < r; j++) {
        int first_full = r;
        for (int i = 0; i < r; i++) {
            int cov = corner_cov(i, j, r);
            if (cov == 16) { first_full = i; break; }
            if (cov == 0) continue;
            uint8_t a = (uint8_t)((uint32_t)alpha * (uint32_t)cov / 16);
            fb_blend_pixel(x + i,         y + j,         color, a);
            fb_blend_pixel(x + w - 1 - i, y + j,         color, a);
            fb_blend_pixel(x + i,         y + h - 1 - j, color, a);
            fb_blend_pixel(x + w - 1 - i, y + h - 1 - j, color, a);
        }
        fb_blend_rect(x + first_full, y + j,         w - 2 * first_full, 1, color, alpha);
        fb_blend_rect(x + first_full, y + h - 1 - j, w - 2 * first_full, 1, color, alpha);
    }
}

void fb_fill_rounded_rect(int x, int y, int w, int h, int r, uint32_t color)
{
    fb_blend_rounded_rect(x, y, w, h, r, color, 255);
}

/* 1px rounded outline: outer shape minus the same shape inset by one pixel. */
void fb_draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color)
{
    if (w <= 2 || h <= 2) { fb_draw_rect(x, y, w, h, color); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 1) { fb_draw_rect(x, y, w, h, color); return; }

    fb_fill_rect(x + r, y,         w - 2 * r, 1, color);
    fb_fill_rect(x + r, y + h - 1, w - 2 * r, 1, color);
    fb_fill_rect(x,         y + r, 1, h - 2 * r, color);
    fb_fill_rect(x + w - 1, y + r, 1, h - 2 * r, color);

    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int outer = corner_cov(i, j, r);
            int inner = (i >= 1 && j >= 1) ? corner_cov(i - 1, j - 1, r - 1) : 0;
            int cov = outer - inner;
            if (cov <= 0) continue;
            uint8_t a = (uint8_t)(cov * 255 / 16);
            fb_blend_pixel(x + i,         y + j,         color, a);
            fb_blend_pixel(x + w - 1 - i, y + j,         color, a);
            fb_blend_pixel(x + i,         y + h - 1 - j, color, a);
            fb_blend_pixel(x + w - 1 - i, y + h - 1 - j, color, a);
        }
    }
}

void fb_fill_circle(int cx, int cy, int r, uint32_t color)
{
    fb_fill_rounded_rect(cx - r, cy - r, 2 * r, 2 * r, r, color);
}

void fb_fill_ring(int cx, int cy, int r_out, int r_in, uint32_t color)
{
    int Ro = 4 * r_out, Ri = 4 * r_in;
    int Ro2 = Ro * Ro, Ri2 = Ri * Ri;

    for (int j = -r_out; j < r_out; j++) {
        for (int i = -r_out; i < r_out; i++) {
            int n = 0;
            for (int sy = 0; sy < 2; sy++) {
                int dy = 4 * j + 2 * sy + 1;
                for (int sx = 0; sx < 2; sx++) {
                    int dx = 4 * i + 2 * sx + 1;
                    int d2 = dx * dx + dy * dy;
                    if (d2 <= Ro2 && d2 >= Ri2) n++;
                }
            }
            if (n)
                fb_blend_pixel(cx + i, cy + j, color, (uint8_t)(n * 255 / 4));
        }
    }
}

/* ---- Text ----------------------------------------------------------------
 * Anti-aliased faces pre-rendered by tools/gen_fonts.py (drivers/font_aa.c).
 * A text "cell" keeps the old 8x16 geometry: y is the top of a 16 px line,
 * so layouts written for the bitmap font still line up. The UI faces are
 * proportional; FONT_MONO keeps an 8 px advance for terminals and editors.
 */
#include "font_aa.h"

static const font_glyph_t *glyph_of(const font_face_t *f, unsigned char c)
{
    if (c >= 32 && c < 127) return &f->glyphs[c - 32];
    if (c >= 0x80 && c < 0x80 + (FONT_GLYPHS - FONT_ASCII)) return &f->glyphs[FONT_ASCII + (c - 0x80)];
    return &f->glyphs['?' - 32];
}

static void draw_glyph(const font_face_t *f, const font_glyph_t *g, int x, int y, uint32_t fg)
{
    if (!g->w || !g->h || !g_draw) return;
    int gx = x + g->bx, gy = y + f->baseline + g->by;
    if (gx >= g_clip.x1 || gy >= g_clip.y1 || gx + g->w <= g_clip.x0 || gy + g->h <= g_clip.y0) return;
    const uint8_t *a = f->alpha + g->off;
    for (int j = 0; j < g->h; j++, a += g->w) {
        int yy = gy + j;
        if (yy < g_clip.y0 || yy >= g_clip.y1) continue;
        uint32_t *line = g_draw + (size_t)yy * g_stride;
        for (int i = 0; i < g->w; i++) {
            uint32_t al = a[i];
            int xx = gx + i;
            if (!al || xx < g_clip.x0 || xx >= g_clip.x1) continue;
            line[xx] = al == 255 ? fg : blend_px(fg, line[xx], al);
        }
    }
}

/* ---- Text capture (screen reading) ----------------------------------------
 * While a capture is active every visible text run drawn is recorded with
 * its position, so JARVIS can read any window and click a labelled control
 * without each app describing itself (kernel/ai_tools.c: read_window,
 * click). Only text inside the current clip rectangle counts: what was
 * scrolled away or covered by the clip is not on screen. */
static fb_text_item_t *g_cap;
static int g_cap_max, g_cap_n;

void fb_capture_begin(fb_text_item_t *items, int max) { g_cap = items; g_cap_max = max; g_cap_n = 0; }
int  fb_capture_end(void) { g_cap = NULL; return g_cap_n; }

static void cap_record(int x, int y, int w, int h, const char *s, int n)
{
    if (!g_cap || n <= 0 || g_cap_n >= g_cap_max) return;
    if (x >= g_clip.x1 || y >= g_clip.y1 || x + w <= g_clip.x0 || y + h <= g_clip.y0) return;
    int blank = 1;
    for (int i = 0; i < n; i++) if (s[i] != ' ') { blank = 0; break; }
    if (blank) return;
    /* characters drawn one by one (fb_draw_char) join the previous run */
    if (g_cap_n) {
        fb_text_item_t *p = &g_cap[g_cap_n - 1];
        int pl = (int)strlen(p->text);
        if (p->y == y && x >= p->x + p->w - 1 && x <= p->x + p->w + 9 && pl + n + 1 < (int)sizeof(p->text)) {
            if (x > p->x + p->w + 3) p->text[pl++] = ' ';
            memcpy(p->text + pl, s, (size_t)n);
            p->text[pl + n] = 0;
            p->w = x + w - p->x;
            return;
        }
    }
    fb_text_item_t *it = &g_cap[g_cap_n++];
    it->x = x; it->y = y; it->w = w; it->h = h;
    if (n > (int)sizeof(it->text) - 1) n = (int)sizeof(it->text) - 1;
    memcpy(it->text, s, (size_t)n);
    it->text[n] = 0;
}

int fb_text_n(int px, int py, const char *str, int n, uint32_t fg, int font)
{
    if (!str || font < 0 || font >= FONT_COUNT) return px;
    const font_face_t *f = &g_font_faces[font];
    if (g_cap) {
        int len = 0, start = 0, y = py;
        for (; (n < 0 || len < n) && str[len]; len++)
            if (str[len] == '\n') {
                cap_record(px, y, fb_text_width_n_font(str + start, len - start, font), f->cell, str + start, len - start);
                start = len + 1; y += f->cell;
            }
        cap_record(px, y, fb_text_width_n_font(str + start, len - start, font), f->cell, str + start, len - start);
    }
    int pen = px * 64;
    for (int i = 0; (n < 0 || i < n) && str[i]; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c == '\n') { pen = px * 64; py += f->cell; continue; }
        const font_glyph_t *g = glyph_of(f, c);
        draw_glyph(f, g, (pen + 32) >> 6, py, fg);
        pen += g->adv;
    }
    return (pen + 32) >> 6;
}

int fb_text(int px, int py, const char *str, uint32_t fg, int font)
{
    return fb_text_n(px, py, str, -1, fg, font);
}

int fb_text_width_n_font(const char *str, int n, int font)
{
    if (!str || font < 0 || font >= FONT_COUNT) return 0;
    const font_face_t *f = &g_font_faces[font];
    int pen = 0, best = 0;
    for (int i = 0; (n < 0 || i < n) && str[i]; i++) {
        if (str[i] == '\n') { if (pen > best) best = pen; pen = 0; continue; }
        pen += glyph_of(f, (unsigned char)str[i])->adv;
    }
    if (pen > best) best = pen;
    return (best + 32) >> 6;
}

int fb_text_width_font(const char *str, int font) { return fb_text_width_n_font(str, -1, font); }
int fb_text_width(const char *str)                { return fb_text_width_n_font(str, -1, FONT_UI); }
int fb_text_width_n(const char *str, int n)       { return fb_text_width_n_font(str, n, FONT_UI); }
int fb_text_width_bold(const char *str)           { return fb_text_width_n_font(str, -1, FONT_UI_BOLD); }

/* Index of the character boundary closest to pixel offset dx (caret hits). */
int fb_text_index_at(const char *str, int dx, int font)
{
    if (!str || font < 0 || font >= FONT_COUNT) return 0;
    const font_face_t *f = &g_font_faces[font];
    int pen = 0, i = 0;
    for (; str[i]; i++) {
        int adv = glyph_of(f, (unsigned char)str[i])->adv;
        if ((pen + adv / 2) >> 6 >= dx) break;
        pen += adv;
    }
    return i;
}

void fb_draw_char(int px, int py, char c, uint32_t fg)
{
    const font_face_t *f = &g_font_faces[FONT_MONO];
    if (g_cap) cap_record(px, py, 8, f->cell, &c, 1);
    draw_glyph(f, glyph_of(f, (unsigned char)c), px, py, fg);
}

void fb_put_char(int px, int py, char c, uint32_t fg, uint32_t bg)
{
    fb_fill_rect(px, py, 8, 16, bg);
    fb_draw_char(px, py, c, fg);
}

void fb_draw_text(int px, int py, const char *str, uint32_t fg)
{
    fb_text_n(px, py, str, -1, fg, FONT_UI);
}

void fb_draw_text_bold(int px, int py, const char *str, uint32_t fg)
{
    fb_text_n(px, py, str, -1, fg, FONT_UI_BOLD);
}

void fb_draw_text_mono(int px, int py, const char *str, uint32_t fg)
{
    fb_text_n(px, py, str, -1, fg, FONT_MONO);
}

void fb_put_string(int px, int py, const char *str, uint32_t fg, uint32_t bg)
{
    int x = px;
    if (!str) return;
    for (; *str; str++) {
        if (*str == '\n') { x = px; py += 16; continue; }
        fb_put_char(x, py, *str, fg, bg);
        x += 8;
    }
}

static int scaled_font(int scale)
{
    return scale <= 1 ? FONT_UI : scale == 2 ? FONT_TITLE : scale == 3 ? FONT_LARGE : FONT_DISPLAY;
}

void fb_draw_text_scaled(int px, int py, const char *str, uint32_t fg, int scale)
{
    fb_text_n(px, py, str, -1, fg, scaled_font(scale));
}

int fb_text_width_scaled(const char *str, int scale)
{
    return fb_text_width_n_font(str, -1, scaled_font(scale));
}

/* ---- Whole-frame operations ---------------------------------------------- */
void fb_save_frame(uint32_t *dst)
{
    if (!g_draw || !dst) return;
    for (uint32_t y = 0; y < g_fb.height; y++)
        copy32(dst + (size_t)y * g_fb.width, g_draw + (size_t)y * g_stride, g_fb.width);
}

void fb_load_frame(const uint32_t *src)
{
    if (!g_draw || !src) return;
    for (uint32_t y = 0; y < g_fb.height; y++)
        copy32(g_draw + (size_t)y * g_stride, src + (size_t)y * g_fb.width, g_fb.width);
}

/* ---- Mouse cursor --------------------------------------------------------
 * The pointer is painted straight onto the screen after each present, never
 * into the back buffer. Moving the mouse therefore costs a 12x19 pixel
 * restore + repaint instead of recomposing the whole desktop.
 */
#define CUR_W 12
#define CUR_H 19
static const char *const g_cursor_shape[CUR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..XX..X    ",
    "X.X  X..X   ",
    "XX   X..X   ",
    "X     X..X  ",
    "      X..X  ",
    "       XX   ",
};

static void cursor_paint(void)
{
    if (!g_fb.base || !g_cur_visible) { g_cur_drawn = 0; return; }

    uint32_t fstride = g_fb.pitch / 4;
    for (int j = 0; j < CUR_H; j++) {
        int y = g_cur_y + j;
        if (y < 0 || y >= (int)g_fb.height) continue;
        for (int i = 0; i < CUR_W; i++) {
            int x = g_cur_x + i;
            char c = g_cursor_shape[j][i];
            if (c == ' ' || x < 0 || x >= (int)g_fb.width) continue;
            g_fb.base[(size_t)y * fstride + (size_t)x] =
                (c == 'X') ? 0x00000000 : 0x00FFFFFF;
        }
    }
    g_cur_drawn = 1;
    g_cur_drawn_x = g_cur_x;
    g_cur_drawn_y = g_cur_y;
}

static void cursor_erase(void)
{
    if (!g_cur_drawn || !g_back) return;

    uint32_t fstride = g_fb.pitch / 4;
    for (int j = 0; j < CUR_H; j++) {
        int y = g_cur_drawn_y + j;
        if (y < 0 || y >= (int)g_fb.height) continue;
        int x0 = g_cur_drawn_x, x1 = g_cur_drawn_x + CUR_W;
        if (x0 < 0) x0 = 0;
        if (x1 > (int)g_fb.width) x1 = (int)g_fb.width;
        if (x1 <= x0) continue;
        copy_filtered(g_fb.base + (size_t)y * fstride + (size_t)x0,
                      g_back + (size_t)y * g_fb.width + (size_t)x0, (size_t)(x1 - x0));
    }
    g_cur_drawn = 0;
}

void fb_cursor_set_pos(int x, int y)
{
    g_cur_x = x;
    g_cur_y = y;
}

void fb_cursor_show(int visible)
{
    g_cur_visible = visible;
}

void fb_cursor_refresh(void)
{
    if (!g_fb.base) return;
    if (g_cur_drawn && g_cur_drawn_x == g_cur_x && g_cur_drawn_y == g_cur_y &&
        g_cur_visible)
        return;
    cursor_erase();
    cursor_paint();
}

void fb_present(void)
{
    if (!g_fb.base) return;

    if (g_back) {
        uint32_t fstride = g_fb.pitch / 4;
        if (fstride == g_fb.width) {
            copy_filtered(g_fb.base, g_back, (size_t)g_fb.width * g_fb.height);
        } else {
            for (uint32_t y = 0; y < g_fb.height; y++)
                copy_filtered(g_fb.base + (size_t)y * fstride,
                              g_back + (size_t)y * g_fb.width, g_fb.width);
        }
    }
    g_cur_drawn = 0;
    cursor_paint();
}

/* ---- Display settings ---------------------------------------------------- */
/* brightness 10..100 %, warmth 0..100 (night light: less blue, less green). */
void fb_set_color_filter(int brightness, int warmth)
{
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;
    if (warmth < 0) warmth = 0;
    if (warmth > 100) warmth = 100;
    int gain[3] = { 100, 100 - warmth * 22 / 100, 100 - warmth * 55 / 100 };
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++)
            g_lut[c][v] = (uint8_t)(v * gain[c] / 100 * brightness / 100);
    g_filter_on = brightness < 100 || warmth > 0;
}

/* Resolutions the Bochs/QEMU display adapter can switch to. Hardware that
 * only has the firmware (GOP) framebuffer keeps the mode it booted with. */
static const uint16_t g_modes[][2] = {
    { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 },
    { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 },
};

static int vbe_available(void)
{
    uint16_t id = bochs_vbe_read(BOCHS_VBE_INDEX_ID);
    return id >= 0xB0C0 && id <= 0xB0C5 && g_fb.phys;
}

int fb_mode_count(void)
{
    return vbe_available() ? (int)(sizeof(g_modes) / sizeof(g_modes[0])) : 1;
}

void fb_mode_get(int i, int *w, int *h)
{
    if (!vbe_available() || i < 0 || i >= fb_mode_count()) { *w = (int)g_fb.width; *h = (int)g_fb.height; return; }
    *w = g_modes[i][0]; *h = g_modes[i][1];
}

/* Switches the screen mode. The caller (the desktop) relays out afterwards.
 * Returns 0 on success. */
int fb_set_mode(int w, int h)
{
    if (!vbe_available()) return -1;
    int ok = 0;
    for (int i = 0; i < fb_mode_count(); i++)
        if (g_modes[i][0] == w && g_modes[i][1] == h) ok = 1;
    if (!ok) return -2;
    if ((uint32_t)w == g_fb.width && (uint32_t)h == g_fb.height) return 0;

    size_t need = (size_t)w * h * 4;
    uint32_t *base = g_fb.base;
    if (need > g_fb.mapped) {
        base = (uint32_t *)vmm_map_range(g_fb.phys, need, VMM_PROT_RW | VMM_PROT_NOCACHE);
        if (!base) return -3;
    }
    size_t old_bytes = (size_t)g_fb.width * g_fb.height * 4;
    uint32_t *back = (uint32_t *)kmalloc_large(need);
    if (!back) return -3;

    if (!bochs_vbe_set_mode((uint16_t)w, (uint16_t)h, 32)) { kfree_large(back, need); return -1; }
    if (need > g_fb.mapped) { g_fb.base = base; g_fb.mapped = need; }
    if (g_back) kfree_large(g_back, old_bytes);
    g_fb.width = (uint32_t)w;
    g_fb.height = (uint32_t)h;
    g_fb.pitch = (uint32_t)w * 4;
    g_back = g_draw = back;
    g_stride = (uint32_t)w;
    fb_reset_clip();
    if (g_cur_x >= w) g_cur_x = w - 1;
    if (g_cur_y >= h) g_cur_y = h - 1;
    g_cur_drawn = 0;
    fb_clear(0);
    return 0;
}

/* ---- Getters ------------------------------------------------------------- */
uint32_t fb_get_width(void)  { return g_fb.width;  }
uint32_t fb_get_height(void) { return g_fb.height; }
uint32_t *fb_get_base(void)  { return g_draw; }

/* ==========================================================================
 * Effects: frosted glass, neon outlines, glows, anti-aliased strokes
 *
 * All integer maths (distances in 1/16 pixel), so they stay cheap on a CPU
 * without hardware floating point acceleration and inside an emulator.
 * ========================================================================== */
static uint32_t fx_isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static inline uint32_t fx_lerp(uint32_t a, uint32_t b, int t, int max)
{
    if (max <= 0 || t <= 0) return a;
    if (t >= max) return b;
    return blend_px(b, a, (uint32_t)(t * 255 / max));
}

/* Signed distance (1/16 px, negative inside) from a pixel centre to a
 * rounded rectangle at the origin. */
static inline int fx_rr_dist(int px, int py, int w, int h, int r)
{
    int cx = w * 8, cy = h * 8;                    /* centre, 1/16 px */
    int ax = px * 16 + 8 - cx, ay = py * 16 + 8 - cy;
    if (ax < 0) ax = -ax;
    if (ay < 0) ay = -ay;
    int qx = ax - (cx - r * 16), qy = ay - (cy - r * 16);
    if (qx > 0 && qy > 0) return (int)fx_isqrt((uint32_t)(qx * qx + qy * qy)) - r * 16;
    return (qx > qy ? qx : qy) - r * 16;
}

void fb_gradient_h(int x, int y, int w, int h, uint32_t left, uint32_t right)
{
    for (int i = 0; i < w; i++)
        fb_fill_rect(x + i, y, 1, h, fx_lerp(left, right, i, w - 1));
}

/* Rounded rectangle filled with a left-to-right gradient. */
void fb_fill_rounded_rect_grad(int x, int y, int w, int h, int r,
                               uint32_t left, uint32_t right, uint8_t alpha)
{
    if (w <= 0 || h <= 0 || !alpha) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int cj = j < r ? j : (j >= h - r ? h - 1 - j : -1);
        for (int i = 0; i < w; i++) {
            int ci = i < r ? i : (i >= w - r ? w - 1 - i : -1);
            int cov = (cj >= 0 && ci >= 0) ? corner_cov(ci, cj, r) : 16;
            if (!cov) continue;
            fb_blend_pixel(x + i, y + j, fx_lerp(left, right, i, w - 1), (uint8_t)((uint32_t)alpha * cov / 16));
        }
    }
}

/* Copies a rounded rectangle out of a frame-sized picture (for example the
 * blurred wallpaper), tinted: the body of a frosted-glass panel. */
void fb_blit_rounded(const uint32_t *src, int x, int y, int w, int h, int r,
                     uint32_t tint, uint8_t tint_alpha)
{
    if (!src || !g_draw || w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int W = (int)g_fb.width, H = (int)g_fb.height;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < g_clip.y0 || py >= g_clip.y1 || py < 0 || py >= H) continue;
        int cj = j < r ? j : (j >= h - r ? h - 1 - j : -1);
        const uint32_t *s = src + (size_t)py * W;
        uint32_t *d = g_draw + (size_t)py * g_stride;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < g_clip.x0 || px >= g_clip.x1 || px < 0 || px >= W) continue;
            uint32_t c = tint_alpha ? blend_px(tint, s[px], tint_alpha) : (s[px] & 0x00FFFFFF);
            if (cj >= 0) {
                int ci = i < r ? i : (i >= w - r ? w - 1 - i : -1);
                if (ci >= 0) {
                    int cov = corner_cov(ci, cj, r);
                    if (!cov) continue;
                    if (cov < 16) c = blend_px(c, d[px], (uint32_t)cov * 255 / 16);
                }
            }
            d[px] = c;
        }
    }
}

/* Rounded outline in a left-to-right gradient, `thickness` px wide, with an
 * optional soft glow of `glow` px on both sides. Only the border band is
 * visited, so large panels stay cheap. */
void fb_neon_rounded_rect(int x, int y, int w, int h, int r, int thickness, int glow,
                          uint32_t left, uint32_t right, uint8_t alpha)
{
    if (w <= 0 || h <= 0 || !alpha) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (thickness < 1) thickness = 1;
    int t16 = thickness * 16, g16 = glow * 16;
    int band = thickness + glow + 1;                /* inside reach of the effect */
    int edge = r > band ? r : band;
    for (int j = -glow; j < h + glow; j++) {
        int py = y + j;
        if (py < g_clip.y0 || py >= g_clip.y1) continue;
        int middle = j >= edge && j < h - edge;     /* only the two side bands */
        for (int i = -glow; i < w + glow; i++) {
            if (middle && i == band) { i = w - band - 1; continue; }
            int px = x + i;
            if (px < g_clip.x0 || px >= g_clip.x1) continue;
            int d = fx_rr_dist(i, j, w, h, r);
            int a = 0;
            if (d <= 8 && d >= -t16 - 8) {          /* the line, 1 px soft edges */
                int in = d + t16 + 8, out = 8 - d;
                int c = in < out ? in : out;
                a = c >= 16 ? 255 : c <= 0 ? 0 : c * 255 / 16;
            }
            if (glow && a < 255) {
                int gd = d > 0 ? d : (-d - t16);    /* distance from the line */
                if (gd < 0) gd = 0;
                if (gd < g16) {
                    int k = (g16 - gd) * 255 / g16;
                    int ga = k * k / 255 * (d > 0 ? 150 : 90) / 255;
                    if (ga > a) a = ga;
                }
            }
            if (a <= 0) continue;
            fb_blend_pixel(px, py, fx_lerp(left, right, i, w - 1), (uint8_t)((uint32_t)a * alpha / 255));
        }
    }
}

/* Soft disc: solid up to radius r, fading out over `spread` px. */
void fb_glow_circle(int cx, int cy, int r, int spread, uint32_t color, uint8_t alpha)
{
    int R = r + spread;
    if (R <= 0 || !alpha) return;
    for (int j = -R; j <= R; j++) {
        if (cy + j < g_clip.y0 || cy + j >= g_clip.y1) continue;
        for (int i = -R; i <= R; i++) {
            int d16 = (int)fx_isqrt((uint32_t)((i * i + j * j) * 256));
            int k;
            if (d16 <= r * 16) k = 255;
            else if (d16 >= R * 16 || !spread) continue;
            else { k = (R * 16 - d16) * 255 / (spread * 16); k = k * k / 255; }
            fb_blend_pixel(cx + i, cy + j, color, (uint8_t)((uint32_t)k * alpha / 255));
        }
    }
}

static const int16_t g_fx_sin[91] = {       /* sin(0..90 deg) * 1024 */
    0,18,36,54,71,89,107,125,143,160,178,195,213,230,248,265,282,299,316,333,350,367,384,400,416,433,
    449,465,481,496,512,527,543,558,573,587,602,616,630,644,658,672,685,698,711,724,737,749,761,773,
    784,796,807,818,828,839,849,859,868,878,887,896,904,912,920,928,935,943,949,956,962,968,974,979,
    984,989,994,998,1002,1005,1008,1011,1014,1016,1018,1020,1022,1023,1023,1024,1024
};

int fb_sin1024(int deg)
{
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg <= 90) return g_fx_sin[deg];
    if (deg <= 180) return g_fx_sin[180 - deg];
    if (deg <= 270) return -g_fx_sin[deg - 180];
    return -g_fx_sin[360 - deg];
}
int fb_cos1024(int deg) { return fb_sin1024(deg + 90); }

/* Arc of a circle: angles in degrees, clockwise from 3 o'clock (screen
 * coordinates). sweep 360 draws the whole ring. */
void fb_draw_arc(int cx, int cy, int r, int thickness, int start_deg, int sweep_deg,
                 uint32_t color, uint8_t alpha)
{
    if (r <= 0 || !alpha || sweep_deg <= 0) return;
    int full = sweep_deg >= 360;
    int sx = fb_cos1024(start_deg), sy = fb_sin1024(start_deg);
    int ex = fb_cos1024(start_deg + sweep_deg), ey = fb_sin1024(start_deg + sweep_deg);
    int ro16 = r * 16, ri16 = (r - thickness) * 16;
    for (int j = -r - 1; j <= r + 1; j++) {
        if (cy + j < g_clip.y0 || cy + j >= g_clip.y1) continue;
        for (int i = -r - 1; i <= r + 1; i++) {
            int d16 = (int)fx_isqrt((uint32_t)((i * i + j * j) * 256));
            int in = d16 - ri16 + 8, out = ro16 - d16 + 8;
            int c = in < out ? in : out;
            if (c <= 0) continue;
            if (!full) {
                int c1 = sx * j - sy * i;           /* at or past the start */
                int c2 = i * ey - j * ex;           /* before the end */
                int ok = sweep_deg <= 180 ? (c1 >= 0 && c2 >= 0) : (c1 >= 0 || c2 >= 0);
                if (!ok) continue;
            }
            int a = c >= 16 ? 255 : c * 255 / 16;
            fb_blend_pixel(cx + i, cy + j, color, (uint8_t)((uint32_t)a * alpha / 255));
        }
    }
}

/* Anti-aliased stroke with round ends. Coordinates and width in 1/16 px;
 * the colour runs from `top` at gy0 to `bottom` at gy1 (pixels). */
void fb_stroke16(int x0, int y0, int x1, int y1, int width16,
                 uint32_t top, uint32_t bottom, int gy0, int gy1, uint8_t alpha)
{
    int half = width16 / 2;
    int minx = ((x0 < x1 ? x0 : x1) - half) / 16 - 1, maxx = ((x0 > x1 ? x0 : x1) + half) / 16 + 1;
    int miny = ((y0 < y1 ? y0 : y1) - half) / 16 - 1, maxy = ((y0 > y1 ? y0 : y1) + half) / 16 + 1;
    int64_t dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int py = miny; py <= maxy; py++) {
        if (py < g_clip.y0 || py >= g_clip.y1) continue;
        uint32_t col = gy1 > gy0 ? fx_lerp(top, bottom, py - gy0, gy1 - gy0) : top;
        for (int px = minx; px <= maxx; px++) {
            int64_t vx = px * 16 + 8 - x0, vy = py * 16 + 8 - y0;
            int64_t t = len2 ? (vx * dx + vy * dy) : 0;
            int64_t qx, qy;
            if (t <= 0) { qx = vx; qy = vy; }
            else if (t >= len2) { qx = vx - dx; qy = vy - dy; }
            else { qx = vx - dx * t / len2; qy = vy - dy * t / len2; }
            int d = (int)fx_isqrt((uint32_t)(qx * qx + qy * qy));
            int c = half - d + 8;
            if (c <= 0) continue;
            int a = c >= 16 ? 255 : c * 255 / 16;
            fb_blend_pixel(px, py, col, (uint8_t)((uint32_t)a * alpha / 255));
        }
    }
}

void fb_draw_line_aa(int x0, int y0, int x1, int y1, int width, uint32_t color, uint8_t alpha)
{
    fb_stroke16(x0 * 16 + 8, y0 * 16 + 8, x1 * 16 + 8, y1 * 16 + 8, width * 16, color, color, 0, 0, alpha);
}

/* Blurs a frame-sized picture into dst (same size): quarter-resolution box
 * blur, three passes, scaled back up. Used once per wallpaper for glass. */
void fb_blur_frame(const uint32_t *src, uint32_t *dst, int w, int h, int radius)
{
    int sw = (w + 3) / 4, sh = (h + 3) / 4;
    size_t bytes = (size_t)sw * sh * 4;
    uint32_t *a = (uint32_t *)kmalloc_large(bytes), *b = (uint32_t *)kmalloc_large(bytes);
    if (!a || !b) {
        if (a) kfree_large(a, bytes);
        if (b) kfree_large(b, bytes);
        for (size_t i = 0; i < (size_t)w * h; i++) dst[i] = src[i];
        return;
    }
    for (int y = 0; y < sh; y++)                    /* 4x4 average */
        for (int x = 0; x < sw; x++) {
            uint32_t r = 0, g = 0, bl = 0, n = 0;
            for (int j = 0; j < 4; j++) {
                int yy = y * 4 + j;
                if (yy >= h) break;
                for (int i = 0; i < 4; i++) {
                    int xx = x * 4 + i;
                    if (xx >= w) break;
                    uint32_t p = src[(size_t)yy * w + xx];
                    r += (p >> 16) & 255; g += (p >> 8) & 255; bl += p & 255; n++;
                }
            }
            a[(size_t)y * sw + x] = ((r / n) << 16) | ((g / n) << 8) | (bl / n);
        }
    int rad = radius / 4;
    if (rad < 1) rad = 1;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < sh; y++)                /* horizontal: a -> b */
            for (int x = 0; x < sw; x++) {
                uint32_t r = 0, g = 0, bl = 0, n = 0;
                for (int k = -rad; k <= rad; k++) {
                    int xx = x + k;
                    if (xx < 0) xx = 0;
                    if (xx >= sw) xx = sw - 1;
                    uint32_t p = a[(size_t)y * sw + xx];
                    r += (p >> 16) & 255; g += (p >> 8) & 255; bl += p & 255; n++;
                }
                b[(size_t)y * sw + x] = ((r / n) << 16) | ((g / n) << 8) | (bl / n);
            }
        for (int y = 0; y < sh; y++)                /* vertical: b -> a */
            for (int x = 0; x < sw; x++) {
                uint32_t r = 0, g = 0, bl = 0, n = 0;
                for (int k = -rad; k <= rad; k++) {
                    int yy = y + k;
                    if (yy < 0) yy = 0;
                    if (yy >= sh) yy = sh - 1;
                    uint32_t p = b[(size_t)yy * sw + x];
                    r += (p >> 16) & 255; g += (p >> 8) & 255; bl += p & 255; n++;
                }
                a[(size_t)y * sw + x] = ((r / n) << 16) | ((g / n) << 8) | (bl / n);
            }
    }
    for (int y = 0; y < h; y++) {                   /* bilinear back up */
        int fy = y * 256 / 4 - 96;
        if (fy < 0) fy = 0;
        int y0 = fy >> 8, wy = fy & 255;
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        if (y0 >= sh) y0 = sh - 1;
        for (int x = 0; x < w; x++) {
            int fx = x * 256 / 4 - 96;
            if (fx < 0) fx = 0;
            int x0 = fx >> 8, wx = fx & 255;
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            if (x0 >= sw) x0 = sw - 1;
            uint32_t top = blend_px(a[(size_t)y0 * sw + x1], a[(size_t)y0 * sw + x0], (uint32_t)wx);
            uint32_t bot = blend_px(a[(size_t)y1 * sw + x1], a[(size_t)y1 * sw + x0], (uint32_t)wx);
            dst[(size_t)y * w + x] = blend_px(bot, top, (uint32_t)wy);
        }
    }
    kfree_large(a, bytes);
    kfree_large(b, bytes);
}
