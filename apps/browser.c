/*
 * =============================================================================
 * JARVIS OS — Browser (apps/browser.c)
 *
 * A web browser for HTTP and HTTPS (TLS 1.3, kernel/tls.c):
 *   - HTML: headings, paragraphs, lists, links, bold, code/pre, quotes,
 *     tables (row by row), images (PNG, JPEG, BMP), <title>, entities and
 *     UTF-8 (shown with the closest ASCII when the font lacks a letter)
 *   - forms with text fields and buttons (GET and POST), so searches work
 *   - address bar: a web address, or words to search for (DuckDuckGo)
 *   - back, forward, reload / stop, home, favorites (saved in the registry
 *     under HKCU\Software\JARVIS OS\Browser\Favorites)
 *   - downloads go to Downloads; a .jpk app can be installed from the bar
 *   - file:// opens local pages and files
 * No JavaScript and no CSS layout: pages show as readable text and pictures.
 * =============================================================================
 */

#define BR_TOOL_H     46
#define BR_STATUS_H   24
#define BR_DL_H       40
#define BR_MAX_ITEMS  24000
#define BR_POOL       (1536 * 1024)
#define BR_MAX_LINKS  4096
#define BR_MAX_IMGS   24
#define BR_MAX_INPUTS 64
#define BR_MAX_DL     6
#define BR_PAGE_BG    0x00FFFFFF
#define BR_TEXT       0x00202124
#define BR_LINK       0x001A0DAB
#define BR_DIM        0x005F6368
#define BR_HOME       "about:home"
#define BR_SEARCH     "https://html.duckduckgo.com/html/?q="

enum { BI_TEXT = 0, BI_RULE, BI_IMAGE, BI_INPUT, BI_BUTTON, BI_BULLET };

typedef struct {
    int y, x, w, h;                 /* page coordinates */
    uint32_t off;                   /* text in the pool */
    uint16_t len;
    uint8_t kind, font;
    uint32_t color;
    int16_t link, ref;              /* link index; image / input index */
    uint8_t underline;
} br_item_t;

typedef struct { char url[512]; image_t img; int state; int w, h; } br_img_t;     /* state 0 wait 1 loading 2 ok 3 failed */
typedef struct { int form; char name[48], value[200]; int type; int len; } br_input_t;  /* type 0 text, 1 hidden, 2 submit */
typedef struct { char action[512]; int post; } br_form_t;
typedef struct { http_req_t req; char name[96]; char path[256]; int active; int done_shown; } br_dl_t;

static struct {
    wm_window_t *win;
    char url[1024], addr[1024];
    int addr_len, addr_focus, addr_sel_all;
    char title[160];
    /* loading */
    http_req_t nav;
    int loading;
    /* the page */
    uint8_t *html; size_t html_len, html_cap;
    int is_text;                    /* show as plain text */
    char ctype[96];
    char tls[48];
    int secure;                     /* 1 https ok, 0 http, -1 error page */
    br_item_t items[BR_MAX_ITEMS];
    int nitems;
    char pool[BR_POOL];
    uint32_t pool_len;
    char links[BR_MAX_LINKS][256];
    int nlinks;
    br_img_t imgs[BR_MAX_IMGS];
    int nimgs;
    http_req_t img_req;
    int img_loading;                /* index loading, -1 none */
    br_input_t inputs[BR_MAX_INPUTS];
    int ninputs, focus_input;
    br_form_t forms[16];
    int nforms;
    int page_h, layout_w, scroll;
    int hover_link;
    /* history */
    char back[24][512]; int nback;
    char fwd[24][512]; int nfwd;
    /* downloads */
    br_dl_t dl[BR_MAX_DL];
    int show_dl;
    char msg[200];
} br;

void apps_store_show_install(const char *path);

/* ---- text pool and items ------------------------------------------------------------ */
static uint32_t br_pool_add(const char *s, size_t n)
{
    if (br.pool_len + n + 1 >= BR_POOL) return br.pool_len;
    memcpy(br.pool + br.pool_len, s, n);
    uint32_t at = br.pool_len;
    br.pool_len += (uint32_t)n;
    br.pool[br.pool_len++] = 0;
    return at;
}

static br_item_t *br_item(int kind)
{
    if (br.nitems >= BR_MAX_ITEMS) return NULL;
    br_item_t *it = &br.items[br.nitems++];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->link = -1;
    it->ref = -1;
    return it;
}

/* ---- layout state ----------------------------------------------------------------------- */
static struct {
    int x, y, line_h, left, right;
    int font, bold, mono, pre, link, center;
    uint32_t color;
    int skip;                       /* inside <script>, <style> ... */
    int in_title;
    int list_depth, list_kind[8], list_num[8];
    int quote;
    int space_pending, line_empty;
    int form;
    /* the run being built */
    char run[600]; int run_len, run_x, run_font; uint32_t run_color; int run_link;
    int in_textarea, in_select, in_option;
} L;

static int br_font(void)
{
    if (L.mono) return FONT_MONO;
    return L.font == FONT_UI && L.bold ? FONT_UI_BOLD : L.font;
}

static int br_font_h(int f)
{
    return f == FONT_TITLE ? 34 : f == FONT_LARGE ? 48 : f == FONT_SMALL ? 18 : 20;
}

static void br_flush_run(void)
{
    if (!L.run_len) return;
    br_item_t *it = br_item(BI_TEXT);
    if (it) {
        it->x = L.run_x;
        it->y = L.y;
        it->font = (uint8_t)L.run_font;
        it->color = L.run_color;
        it->link = (int16_t)L.run_link;
        it->underline = L.run_link >= 0;
        it->off = br_pool_add(L.run, (size_t)L.run_len);
        it->len = (uint16_t)L.run_len;
        it->w = fb_text_width_n_font(L.run, L.run_len, L.run_font);
        it->h = br_font_h(L.run_font);
    }
    L.run_len = 0;
}

static void br_newline(int extra)
{
    br_flush_run();
    if (!L.line_empty || extra) L.y += L.line_h;
    L.y += extra;
    L.x = L.left + L.quote * 30 + L.list_depth * 26;
    L.line_h = br_font_h(br_font());
    L.line_empty = 1;
    L.space_pending = 0;
}

static void br_block(int margin)
{
    if (!L.line_empty) br_newline(0);
    if (margin) {
        /* no double margins */
        if (br.nitems && L.y - margin < br.items[br.nitems - 1].y + br.items[br.nitems - 1].h) L.y += margin / 2;
        else L.y += margin / 2;
    }
}

/* Puts one word (or a pre-formatted chunk) on the current line. */
static void br_word(const char *w, int n)
{
    int f = br_font();
    int ww = fb_text_width_n_font(w, n, f);
    int sp = L.space_pending && !L.line_empty ? fb_text_width_font(" ", f) : 0;
    uint32_t col = L.link >= 0 ? BR_LINK : L.color;
    if (L.x + sp + ww > L.right && !L.line_empty && !L.pre) { br_newline(0); sp = 0; }
    /* start a new run if the style changed */
    if (L.run_len && (L.run_font != f || L.run_color != col || L.run_link != L.link || L.run_len + n + 2 >= (int)sizeof(L.run))) br_flush_run();
    if (!L.run_len) {
        L.run_x = L.x + sp;
        L.run_font = f;
        L.run_color = col;
        L.run_link = L.link;
        sp = 0;
    } else if (sp) L.run[L.run_len++] = ' ';
    if (L.run_len == 0 && sp) { }
    memcpy(L.run + L.run_len, w, (size_t)n);
    L.run_len += n;
    L.x += sp + ww + (L.run_len == n ? 0 : 0);
    if (L.run_len == n) L.x = L.run_x + ww;
    else L.x = L.run_x + fb_text_width_n_font(L.run, L.run_len, f);
    int fh = br_font_h(f);
    if (fh > L.line_h) L.line_h = fh;
    L.line_empty = 0;
    L.space_pending = 0;
}

/* UTF-8 / entity code point -> a character our fonts have */
static int br_map_cp(uint32_t cp, char *out)
{
    if (cp < 128) { out[0] = (char)cp; return 1; }
    switch (cp) {
    case 0xA0: out[0] = ' '; return 1;
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: out[0] = '\''; return 1;
    case 0x201C: case 0x201D: case 0x201E: case 0xAB: case 0xBB: out[0] = '"'; return 1;
    case 0x2013: case 0x2014: case 0x2212: case 0x2010: out[0] = '-'; return 1;
    case 0x2026: out[0] = SYM_ELLIPSIS[0]; return 1;
    case 0x2022: case 0xB7: case 0x25CF: out[0] = SYM_BULLET[0]; return 1;
    case 0xA9: out[0] = SYM_COPY[0]; return 1;
    case 0xD7: out[0] = SYM_TIMES[0]; return 1;
    case 0xB0: out[0] = SYM_DEGREE[0]; return 1;
    case 0x2192: case 0x203A: out[0] = SYM_RIGHT[0]; return 1;
    case 0x2190: case 0x2039: out[0] = SYM_LEFT[0]; return 1;
    case 0x2191: out[0] = SYM_UP[0]; return 1;
    case 0x2193: out[0] = SYM_DOWN[0]; return 1;
    case 0x2713: case 0x2714: out[0] = SYM_CHECK[0]; return 1;
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD: return 0;
    default: break;
    }
    if (cp >= 0xC0 && cp <= 0x17F) {                        /* Latin letters with accents */
        static const char lat[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        if (cp <= 0xFF) { out[0] = lat[cp - 0xC0]; return 1; }
        static const char ext[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        if (cp - 0x100 < sizeof(ext) - 1) { out[0] = ext[cp - 0x100]; return 1; }
    }
    out[0] = '?';
    return 1;
}

static const struct { const char *n; uint32_t cp; } g_entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 }, { "copy", 0xA9 },
    { "reg", 'R' }, { "trade", 'T' }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "rsquo", 0x2019 },
    { "lsquo", 0x2018 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "middot", 0xB7 }, { "bull", 0x2022 }, { "times", 0xD7 },
    { "deg", 0xB0 }, { "rarr", 0x2192 }, { "larr", 0x2190 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "laquo", 0xAB },
    { "raquo", 0xBB }, { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "aacute", 0xE1 }, { "agrave", 0xE0 }, { "ouml", 0xF6 },
    { "uuml", 0xFC }, { "auml", 0xE4 }, { "szlig", 0xDF }, { "ccedil", 0xE7 }, { "ntilde", 0xF1 }, { "zwj", 0x200D },
    { "check", 0x2713 }, { "minus", 0x2212 }, { "shy", 0xAD }, { "euro", 'E' }, { "pound", 'L' }, { "cent", 'c' },
};

/* Decodes text (entities + UTF-8) from src into dst; returns the length. */
static int br_decode(const char *src, int n, char *dst, int cap)
{
    int o = 0;
    for (int i = 0; i < n && o < cap - 2;) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp;
        if (c == '&') {
            int j = i + 1;
            while (j < n && j - i < 12 && src[j] != ';' && src[j] != '&' && src[j] != ' ' && src[j] != '<') j++;
            if (j < n && src[j] == ';') {
                cp = 0;
                if (src[i + 1] == '#') {
                    int hex = src[i + 2] == 'x' || src[i + 2] == 'X';
                    for (int k = i + 2 + hex; k < j; k++) {
                        char d = src[k];
                        cp = cp * (hex ? 16 : 10) + (uint32_t)(d >= '0' && d <= '9' ? d - '0' : (d | 32) - 'a' + 10);
                    }
                } else {
                    for (size_t e = 0; e < sizeof(g_entities) / sizeof(g_entities[0]); e++)
                        if ((int)strlen(g_entities[e].n) == j - i - 1 && !strncmp(src + i + 1, g_entities[e].n, (size_t)(j - i - 1))) cp = g_entities[e].cp;
                }
                if (cp) { char m[2]; int k = br_map_cp(cp, m); if (k) dst[o++] = m[0]; i = j + 1; continue; }
            }
            dst[o++] = '&';
            i++;
            continue;
        }
        if (c < 0x80) { dst[o++] = (char)c; i++; continue; }
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        cp = extra == 3 ? c & 7 : extra == 2 ? c & 15 : c & 31;
        int k = 1;
        for (; k <= extra && i + k < n && ((unsigned char)src[i + k] & 0xC0) == 0x80; k++) cp = cp << 6 | ((unsigned char)src[i + k] & 63);
        i += k;
        char m[2];
        if (br_map_cp(extra ? cp : '?', m)) dst[o++] = m[0];
    }
    dst[o] = 0;
    return o;
}

/* Text between tags: words, or the whole thing in <pre>. */
static void br_text(const char *s, int n)
{
    if (L.skip) return;
    static char buf[8192];
    if (n > (int)sizeof(buf) - 4) n = (int)sizeof(buf) - 4;
    int len = br_decode(s, n, buf, sizeof(buf));
    if (L.in_title) {
        size_t tl = strlen(br.title);
        snprintf(br.title + tl, sizeof(br.title) - tl, "%.*s", len, buf);
        return;
    }
    if (L.in_textarea || L.in_select) {
        if (br.ninputs > 0 && L.in_textarea) {
            br_input_t *in = &br.inputs[br.ninputs - 1];
            snprintf(in->value + strlen(in->value), sizeof(in->value) - strlen(in->value), "%.*s", len, buf);
        }
        return;
    }
    if (L.pre) {
        int st = 0;
        for (int i = 0; i <= len; i++) {
            if (i == len || buf[i] == '\n') {
                if (i > st) br_word(buf + st, i - st);
                if (i < len) br_newline(0);
                st = i + 1;
            } else if (buf[i] == '\t') buf[i] = ' ';
        }
        return;
    }
    int i = 0;
    while (i < len) {
        if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') { L.space_pending = 1; i++; continue; }
        int st = i;
        while (i < len && buf[i] != ' ' && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t') i++;
        br_word(buf + st, i - st);
        if (i < len) L.space_pending = 1;
    }
}

/* Attribute value of a tag (raw text between < and >). */
static int br_attr(const char *tag, int tl, const char *name, char *out, int cap)
{
    int nl = (int)strlen(name);
    out[0] = 0;
    for (int i = 0; i + nl < tl; i++) {
        if ((i == 0 || tag[i - 1] == ' ' || tag[i - 1] == '\t' || tag[i - 1] == '\n' || tag[i - 1] == '"' || tag[i - 1] == '\'') &&
            !strncasecmp_br(tag + i, name, nl)) {
            int j = i + nl;
            while (j < tl && tag[j] == ' ') j++;
            if (j >= tl || tag[j] != '=') { if (j >= tl || tag[j] == ' ' || tag[j] == '>' || tag[j] == '/') { out[0] = 0; return 1; } continue; }
            j++;
            while (j < tl && tag[j] == ' ') j++;
            char q = tag[j] == '"' || tag[j] == '\'' ? tag[j++] : 0;
            int k = 0;
            while (j < tl && (q ? tag[j] != q : tag[j] != ' ' && tag[j] != '>') && k < cap * 2) {
                if (k < cap - 1) out[k++] = tag[j];
                j++;
            }
            out[k] = 0;
            static char dec[1024];
            br_decode(out, k, dec, sizeof(dec));
            snprintf(out, (size_t)cap, "%s", dec);
            return 1;
        }
    }
    return 0;
}

static int br_link_add(const char *href)
{
    if (br.nlinks >= BR_MAX_LINKS) return -1;
    http_resolve_url(br.url, href, br.links[br.nlinks], sizeof(br.links[0]));
    return br.nlinks++;
}

static void br_image(const char *tag, int tl)
{
    char src[512], alt[128], ws[16], hs[16];
    br_attr(tag, tl, "src", src, sizeof(src));
    if (!src[0]) br_attr(tag, tl, "data-src", src, sizeof(src));
    br_attr(tag, tl, "alt", alt, sizeof(alt));
    br_attr(tag, tl, "width", ws, sizeof(ws));
    br_attr(tag, tl, "height", hs, sizeof(hs));
    char full[512];
    http_resolve_url(br.url, src, full, sizeof(full));
    int idx = -1;
    for (int i = 0; i < br.nimgs; i++) if (!strcmp(br.imgs[i].url, full)) idx = i;
    if (idx < 0 && br.nimgs < BR_MAX_IMGS && src[0] && strncmp(src, "data:", 5)) {
        idx = br.nimgs++;
        memset(&br.imgs[idx], 0, sizeof(br.imgs[idx]));
        snprintf(br.imgs[idx].url, sizeof(br.imgs[idx].url), "%s", full);
    }
    int w = (int)sh_num(ws, 0), h = (int)sh_num(hs, 0);
    if (idx >= 0 && br.imgs[idx].state == 2) {
        w = br.imgs[idx].img.w; h = br.imgs[idx].img.h;
        int ww = (int)sh_num(ws, 0);
        if (ww > 0 && ww < w) { h = h * ww / w; w = ww; }
    }
    if (w <= 0 || h <= 0) {
        if (alt[0]) { br_text(alt, (int)strlen(alt)); return; }
        w = 0; h = 0;
    }
    int maxw = L.right - L.left - 10;
    if (w > maxw) { h = h * maxw / w; w = maxw; }
    if (w <= 2 || h <= 2) return;                            /* tracking pixels */
    if (L.x + w > L.right && !L.line_empty) br_newline(0);
    br_flush_run();
    br_item_t *it = br_item(BI_IMAGE);
    if (!it) return;
    it->x = L.x; it->y = L.y; it->w = w; it->h = h;
    it->ref = (int16_t)idx;
    it->link = (int16_t)L.link;
    it->off = br_pool_add(alt, strlen(alt));
    it->len = (uint16_t)strlen(alt);
    L.x += w + 4;
    if (h + 4 > L.line_h) L.line_h = h + 4;
    L.line_empty = 0;
}

static void br_input(const char *tag, int tl, int is_button)
{
    char type[24], name[48], value[200], ph[64];
    br_attr(tag, tl, "type", type, sizeof(type));
    br_attr(tag, tl, "name", name, sizeof(name));
    br_attr(tag, tl, "value", value, sizeof(value));
    br_attr(tag, tl, "placeholder", ph, sizeof(ph));
    for (char *c = type; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    if (!strcmp(type, "checkbox") || !strcmp(type, "radio") || !strcmp(type, "file") || !strcmp(type, "image") || !strcmp(type, "reset")) return;
    if (br.ninputs >= BR_MAX_INPUTS) return;
    br_input_t *in = &br.inputs[br.ninputs];
    memset(in, 0, sizeof(*in));
    in->form = L.form;
    snprintf(in->name, sizeof(in->name), "%s", name);
    snprintf(in->value, sizeof(in->value), "%s", value);
    in->type = !strcmp(type, "hidden") ? 1 : (is_button || !strcmp(type, "submit") || !strcmp(type, "button")) ? 2 : 0;
    int idx = br.ninputs++;
    if (in->type == 1) return;
    int w = in->type == 2 ? fb_text_width(value[0] ? value : "Go") + 28 : 260, h = 28;
    if (L.x + w > L.right && !L.line_empty) br_newline(0);
    br_flush_run();
    br_item_t *it = br_item(in->type == 2 ? BI_BUTTON : BI_INPUT);
    if (!it) return;
    it->x = L.x; it->y = L.y; it->w = w; it->h = h;
    it->ref = (int16_t)idx;
    it->off = br_pool_add(ph, strlen(ph));
    it->len = (uint16_t)strlen(ph);
    L.x += w + 8;
    if (h + 6 > L.line_h) L.line_h = h + 6;
    L.line_empty = 0;
}

static int strncasecmp_br(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i], y = b[i] >= 'A' && b[i] <= 'Z' ? (char)(b[i] + 32) : b[i];
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

static int tag_is(const char *name, const char *t) { return !strcmp(name, t); }

static void br_tag(const char *tag, int tl)
{
    int closing = tag[0] == '/';
    const char *p = tag + closing;
    char name[16];
    int k = 0;
    while (k < 15 && p[k] && p[k] != ' ' && p[k] != '>' && p[k] != '/' && p[k] != '\n' && p[k] != '\t' && p - tag + k < tl) {
        name[k] = p[k] >= 'A' && p[k] <= 'Z' ? (char)(p[k] + 32) : p[k];
        k++;
    }
    name[k] = 0;
    static const char *const skip_tags[] = { "script", "style", "noscript", "template", "svg", "head", "iframe", "canvas", "object", "math" };
    for (size_t i = 0; i < sizeof(skip_tags) / sizeof(skip_tags[0]); i++)
        if (tag_is(name, skip_tags[i])) {
            if (tag_is(name, "head")) return;               /* <head>: only skip what is inside its children */
            L.skip += closing ? -1 : 1;
            if (L.skip < 0) L.skip = 0;
            return;
        }
    if (tag_is(name, "title")) { L.in_title = !closing; if (!closing) br.title[0] = 0; return; }
    if (L.skip) return;
    if (tag_is(name, "br")) { br_newline(L.line_empty ? br_font_h(br_font()) : 0); return; }
    if (tag_is(name, "hr")) {
        br_block(8);
        br_item_t *it = br_item(BI_RULE);
        if (it) { it->x = L.left; it->y = L.y; it->w = L.right - L.left; it->h = 1; }
        L.y += 10;
        return;
    }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        int lvl = name[1] - '0';
        br_block(lvl <= 2 ? 20 : 14);
        if (closing) { L.font = FONT_UI; L.bold = 0; L.color = BR_TEXT; br_block(4); }
        else { L.font = lvl == 1 ? FONT_LARGE : lvl == 2 ? FONT_TITLE : FONT_UI; L.bold = 1; L.line_h = br_font_h(br_font()); }
        if (lvl == 1 && !closing) L.font = FONT_TITLE;
        return;
    }
    static const char *const blocks[] = { "p", "div", "section", "article", "header", "footer", "nav", "main", "aside", "figure",
                                          "figcaption", "address", "dl", "dt", "dd", "fieldset", "center", "details", "summary", "caption", "tbody", "thead" };
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
        if (tag_is(name, blocks[i])) {
            br_block(tag_is(name, "p") ? 12 : tag_is(name, "dd") ? 0 : 4);
            if (tag_is(name, "dd")) L.x += 30;
            return;
        }
    if (tag_is(name, "table")) { br_block(8); return; }
    if (tag_is(name, "tr")) { br_block(0); return; }
    if (tag_is(name, "td") || tag_is(name, "th")) {
        if (!closing && !L.line_empty) { L.x += 16; L.space_pending = 0; }
        if (tag_is(name, "th")) L.bold = !closing;
        return;
    }
    if (tag_is(name, "ul") || tag_is(name, "ol")) {
        br_block(6);
        if (!closing && L.list_depth < 7) { L.list_depth++; L.list_kind[L.list_depth] = name[0] == 'o'; L.list_num[L.list_depth] = 0; }
        else if (closing && L.list_depth > 0) L.list_depth--;
        L.x = L.left + L.quote * 30 + L.list_depth * 26;
        return;
    }
    if (tag_is(name, "li")) {
        br_block(2);
        if (closing) return;
        br_flush_run();
        if (L.list_depth && L.list_kind[L.list_depth]) {
            char num[12];
            snprintf(num, sizeof(num), "%d.", ++L.list_num[L.list_depth]);
            br_item_t *it = br_item(BI_TEXT);
            if (it) { it->x = L.x - 20; it->y = L.y; it->font = FONT_UI; it->color = BR_TEXT; it->off = br_pool_add(num, strlen(num)); it->len = (uint16_t)strlen(num); it->h = 20; }
        } else {
            br_item_t *it = br_item(BI_BULLET);
            if (it) { it->x = L.x - 14; it->y = L.y + 8; it->w = 5; it->h = 5; }
        }
        return;
    }
    if (tag_is(name, "blockquote")) { br_block(8); L.quote += closing ? -1 : 1; if (L.quote < 0) L.quote = 0; L.x = L.left + L.quote * 30; return; }
    if (tag_is(name, "pre")) { br_block(8); L.pre = !closing; L.mono = !closing; return; }
    if (tag_is(name, "code") || tag_is(name, "kbd") || tag_is(name, "samp") || tag_is(name, "tt")) { if (!L.pre) L.mono = !closing; return; }
    if (tag_is(name, "b") || tag_is(name, "strong")) { L.bold += closing ? -1 : 1; if (L.bold < 0) L.bold = 0; return; }
    if (tag_is(name, "small")) { L.font = closing ? FONT_UI : FONT_SMALL; return; }
    if (tag_is(name, "a")) {
        if (closing) { br_flush_run(); L.link = -1; return; }
        char href[512];
        if (br_attr(tag, tl, "href", href, sizeof(href)) && href[0] && strncmp(href, "javascript:", 11)) { br_flush_run(); L.link = br_link_add(href); }
        return;
    }
    if (tag_is(name, "img")) { br_image(tag, tl); return; }
    if (tag_is(name, "form")) {
        if (closing) { L.form = -1; br_block(4); return; }
        if (br.nforms < 16) {
            br_form_t *f = &br.forms[br.nforms];
            char action[512], method[16];
            br_attr(tag, tl, "action", action, sizeof(action));
            br_attr(tag, tl, "method", method, sizeof(method));
            http_resolve_url(br.url, action[0] ? action : br.url, f->action, sizeof(f->action));
            f->post = !strncasecmp_br(method, "post", 4);
            L.form = br.nforms++;
        }
        br_block(4);
        return;
    }
    if (tag_is(name, "input") && !closing) { br_input(tag, tl, 0); return; }
    if (tag_is(name, "button")) {
        if (!closing) { br_input(tag, tl, 1); L.skip++; L.skip--; }
        return;
    }
    if (tag_is(name, "textarea")) {
        if (!closing) { br_input(tag, tl, 0); L.in_textarea = 1; } else L.in_textarea = 0;
        return;
    }
    if (tag_is(name, "select")) { L.in_select = !closing; return; }
}

/* Lays the page out at the current width. */
static void br_layout(int width)
{
    br.nitems = 0;
    br.pool_len = 0;
    br.nlinks = 0;
    br.ninputs = 0;
    br.nforms = 0;
    memset(&L, 0, sizeof(L));
    L.left = 24;
    L.right = width - 24;
    L.x = L.left;
    L.y = 16;
    L.font = FONT_UI;
    L.color = BR_TEXT;
    L.link = -1;
    L.form = -1;
    L.line_empty = 1;
    L.line_h = 20;
    br.layout_w = width;
    const char *h = (const char *)br.html;
    size_t n = br.html_len;
    if (!h) { br.page_h = 0; return; }
    if (br.is_text) {
        L.pre = L.mono = 1;
        br_text(h, (int)(n > 400000 ? 400000 : n));
    } else {
        size_t i = 0;
        while (i < n && br.nitems < BR_MAX_ITEMS - 8) {
            if (h[i] == '<') {
                if (i + 4 <= n && !strncmp(h + i, "<!--", 4)) {
                    const char *e = strstr(h + i + 4, "-->");
                    i = e ? (size_t)(e - h) + 3 : n;
                    continue;
                }
                size_t j = i + 1;
                char q = 0;
                while (j < n && (q || h[j] != '>')) {
                    if (!q && (h[j] == '"' || h[j] == '\'')) q = h[j];
                    else if (q && h[j] == q) q = 0;
                    j++;
                }
                if (h[i + 1] != '!' && h[i + 1] != '?') br_tag(h + i + 1, (int)(j - i - 1));
                /* raw text elements: jump to their end tag */
                if (L.skip) {
                    char nm[12] = "";
                    int k = 0;
                    while (k < 11 && i + 1 + (size_t)k < j && h[i + 1 + k] != ' ' && h[i + 1 + k] != '>') { nm[k] = (char)(h[i + 1 + k] | 32); k++; }
                    nm[k] = 0;
                    if (!strcmp(nm, "script") || !strcmp(nm, "style")) {
                        char endtag[16];
                        snprintf(endtag, sizeof(endtag), "</%s", nm);
                        size_t e = j;
                        while (e + strlen(endtag) < n && strncasecmp_br(h + e, endtag, (int)strlen(endtag))) e++;
                        i = e;
                        continue;
                    }
                }
                i = j + 1;
            } else {
                size_t j = i;
                while (j < n && h[j] != '<') j++;
                br_text(h + i, (int)(j - i));
                i = j;
            }
        }
    }
    br_newline(0);
    br.page_h = L.y + 40;
}

/* ---- navigation ------------------------------------------------------------------------ */
static void br_set_html(const char *s, size_t n, int text)
{
    if (n + 1 > br.html_cap) {
        if (br.html) { if (br.html_cap >= LARGE_ALLOC_THRESHOLD) kfree_large(br.html, br.html_cap); else kfree(br.html); }
        br.html_cap = n + 1 < 65536 ? 65536 : n + 1;
        br.html = (uint8_t *)(br.html_cap >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(br.html_cap) : kmalloc(br.html_cap));
        if (!br.html) { br.html_cap = 0; br.html_len = 0; return; }
    }
    memcpy(br.html, s, n);
    br.html[n] = 0;
    br.html_len = n;
    br.is_text = text;
    for (int i = 0; i < br.nimgs; i++) image_free(&br.imgs[i].img);
    br.nimgs = 0;
    br.img_loading = -1;
    br.scroll = 0;
    br.layout_w = 0;
    br.focus_input = -1;
    br.hover_link = -1;
}

static void br_error_page(const char *what, const char *why)
{
    static char page[2048];
    int n = snprintf(page, sizeof(page), "<title>Can't open this page</title><h1>Can't open this page</h1><p>%s</p><p><b>%s</b></p>"
                     "<p>Check the address, and that the network is connected (Quick Settings, or <code>ifconfig</code> in the Terminal).</p>"
                     "<p><a href=\"%s\">Try again</a> &nbsp; <a href=\"about:home\">Home</a></p>", what, why, br.url);
    br_set_html(page, (size_t)n, 0);
    br.secure = -1;
}

static void br_home_page(void)
{
    static char page[8192];
    int n = snprintf(page, sizeof(page),
        "<title>New tab</title><h1>JARVIS Browser</h1>"
        "<form action=\"https://html.duckduckgo.com/html/\" method=\"get\"><input type=\"text\" name=\"q\" placeholder=\"Search the web\"> <input type=\"submit\" value=\"Search\"></form>"
        "<h3>Go to</h3><ul>"
        "<li><a href=\"https://en.wikipedia.org/wiki/Main_Page\">Wikipedia</a></li>"
        "<li><a href=\"https://www.debian.org/\">Debian</a> - <a href=\"https://packages.debian.org/stable/\">Debian packages</a></li>"
        "<li><a href=\"https://github.com/\">GitHub</a></li>"
        "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
        "<li><a href=\"https://lite.cnn.com/\">CNN (text edition)</a></li>"
        "<li><a href=\"https://example.com/\">example.com</a></li></ul>");
    /* favorites from the registry */
    int k = reg_open_key("HKCU\\Software\\JARVIS OS\\Browser\\Favorites", 0);
    char nm[64], val[512];
    int type, any = 0;
    for (int i = 0; k >= 0 && reg_value_at(k, i, nm, sizeof(nm), &type, val, sizeof(val), NULL) == 0 && n < (int)sizeof(page) - 700; i++) {
        if (!nm[0]) continue;
        if (!any) { n += snprintf(page + n, sizeof(page) - (size_t)n, "<h3>Favorites</h3><ul>"); any = 1; }
        n += snprintf(page + n, sizeof(page) - (size_t)n, "<li><a href=\"%s\">%s</a></li>", val, nm);
    }
    if (any) n += snprintf(page + n, sizeof(page) - (size_t)n, "</ul>");
    n += snprintf(page + n, sizeof(page) - (size_t)n,
        "<h3>About</h3><p>Secure pages use TLS 1.3, checked against the trusted root certificates built into JARVIS OS. "
        "Pages show as text and pictures: there is no JavaScript. Downloads are saved in <a href=\"file:///jarvis/data/Downloads\">Downloads</a>; "
        "JARVIS OS apps (.jpk) can be installed straight from the downloads bar.</p>");
    br_set_html(page, (size_t)n, 0);
    br.secure = 0;
}

static int br_is_download_url(const char *u)
{
    static const char *const ext[] = { ".jpk", ".zip", ".deb", ".iso", ".gz", ".xz", ".bz2", ".tar", ".tgz", ".7z", ".exe",
                                       ".msi", ".apk", ".bin", ".img", ".pdf", ".mp3", ".mp4", ".wav", ".ogg", ".rpm", ".dmg" };
    const char *end = u;
    while (*end && *end != '?' && *end != '#') end++;
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        size_t l = strlen(ext[i]);
        if ((size_t)(end - u) > l && !strncasecmp_br(end - l, ext[i], (int)l)) return 1;
    }
    return 0;
}

static void br_download(const char *url)
{
    int slot = -1;
    for (int i = 0; i < BR_MAX_DL; i++) if (!br.dl[i].active) { slot = i; break; }
    if (slot < 0) { snprintf(br.msg, sizeof(br.msg), "Wait for a download to finish first."); return; }
    br_dl_t *d = &br.dl[slot];
    http_free(&d->req);
    memset(d, 0, sizeof(*d));
    snprintf(d->req.url, sizeof(d->req.url), "%s", url);
    http_suggest_name(&d->req, d->name, sizeof(d->name));
    if (!vfs_is_dir("/jarvis/data/Downloads")) vfs_mkdir("/jarvis/data/Downloads");
    snprintf(d->path, sizeof(d->path), "/jarvis/data/Downloads/%s", d->name);
    snprintf(d->req.save_path, sizeof(d->req.save_path), "%s", d->path);
    d->active = 1;
    br.show_dl = 1;
    http_start(&d->req);
}

static void br_history_push(void)
{
    if (!br.url[0] || br.secure == -1) return;
    if (br.nback == 24) { memmove(br.back[0], br.back[1], sizeof(br.back[0]) * 23); br.nback--; }
    snprintf(br.back[br.nback++], sizeof(br.back[0]), "%s", br.url);
}

static void br_load_file(const char *path)
{
    char p[512];
    snprintf(p, sizeof(p), "%s", path);
    for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
    if (vfs_is_dir(p)) {                                    /* a folder listing */
        static char page[32768];
        static sh_ent_t ents[256];
        int n = snprintf(page, sizeof(page), "<title>%s</title><h2>Index of %s</h2><ul>", p, p);
        int ne = sh_readdir(p, ents, 256);
        for (int i = 0; i < ne && n < (int)sizeof(page) - 600; i++)
            n += snprintf(page + n, sizeof(page) - (size_t)n, "<li><a href=\"file://%s/%s\">%s%s</a></li>", p, ents[i].name, ents[i].name, ents[i].is_dir ? "/" : "");
        br_set_html(page, (size_t)n, 0);
        return;
    }
    const char *d;
    long n = sh_read_file(p, &d);
    if (n < 0) { br_error_page(p, "The file does not exist."); return; }
    int html = strstr(p, ".htm") != NULL;
    if (image_is_picture_name(p)) {
        static char page[1200];
        int k = snprintf(page, sizeof(page), "<title>%s</title><img src=\"file://%s\">", sh_basename(p), p);
        br_set_html(page, (size_t)k, 0);
        return;
    }
    br_set_html(d, (size_t)n, !html);
}

static void br_navigate(const char *target, int push)
{
    char u[1024];
    const char *t = target;
    while (*t == ' ') t++;
    if (!*t) return;
    /* words, not an address: search */
    int has_scheme = strstr(t, "://") != NULL || !strncmp(t, "about:", 6);
    int looks_host = !strchr(t, ' ') && strchr(t, '.') && t[0] != '.';
    if (!has_scheme && !looks_host && t[0] != '/') {
        size_t o = (size_t)snprintf(u, sizeof(u), "%s", BR_SEARCH);
        for (const char *c = t; *c && o < sizeof(u) - 4; c++) {
            if (*c == ' ') u[o++] = '+';
            else if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-_.~", *c)) u[o++] = *c;
            else o += (size_t)snprintf(u + o, sizeof(u) - o, "%%%02X", (unsigned char)*c);
        }
        u[o] = 0;
    } else if (t[0] == '/') snprintf(u, sizeof(u), "file://%s", t);
    else if (!has_scheme) snprintf(u, sizeof(u), "https://%s", t);
    else snprintf(u, sizeof(u), "%s", t);

    if (br_is_download_url(u) && strncmp(u, "file:", 5)) { br_download(u); snprintf(br.msg, sizeof(br.msg), "Downloading %s", sh_basename(u)); return; }
    if (push) { br_history_push(); br.nfwd = 0; }
    if (br.loading) { br.nav.cancel = 1; }
    snprintf(br.url, sizeof(br.url), "%s", u);
    snprintf(br.addr, sizeof(br.addr), "%s", u);
    br.addr_len = (int)strlen(br.addr);
    br.addr_focus = 0;
    br.msg[0] = 0;
    br.tls[0] = 0;
    if (!strcmp(u, BR_HOME) || !strcmp(u, "about:blank")) { br_home_page(); snprintf(br.addr, sizeof(br.addr), "%s", u); return; }
    if (!strncmp(u, "file://", 7)) { br.secure = 0; br_load_file(u + 7); br.title[0] = 0; return; }
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) { br_error_page(u, "Only http, https and file addresses can be opened."); return; }
    if (br.loading) { http_free(&br.nav); }
    memset(&br.nav, 0, sizeof(br.nav));
    snprintf(br.nav.url, sizeof(br.nav.url), "%s", u);
    br.loading = 1;
    if (http_start(&br.nav) != 0) { br.loading = 0; br_error_page(u, br.nav.err); }
}

static void br_post(const char *url, const char *body)
{
    br_history_push();
    br.nfwd = 0;
    memset(&br.nav, 0, sizeof(br.nav));
    snprintf(br.nav.url, sizeof(br.nav.url), "%s", url);
    snprintf(br.nav.method, sizeof(br.nav.method), "POST");
    br.nav.post_len = (size_t)snprintf(br.nav.post, sizeof(br.nav.post), "%s", body);
    snprintf(br.url, sizeof(br.url), "%s", url);
    snprintf(br.addr, sizeof(br.addr), "%s", url);
    br.addr_len = (int)strlen(br.addr);
    br.loading = 1;
    http_start(&br.nav);
}

static void br_urlencode_append(char *out, size_t n, const char *s)
{
    size_t o = strlen(out);
    for (; *s && o < n - 4; s++) {
        if (*s == ' ') out[o++] = '+';
        else if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("-_.~", *s)) out[o++] = *s;
        else o += (size_t)snprintf(out + o, n - o, "%%%02X", (unsigned char)*s);
    }
    out[o] = 0;
}

static void br_submit(int form, int button)
{
    if (form < 0 || form >= br.nforms) return;
    static char q[4096];
    q[0] = 0;
    for (int i = 0; i < br.ninputs; i++) {
        br_input_t *in = &br.inputs[i];
        if (in->form != form || !in->name[0]) continue;
        if (in->type == 2 && i != button) continue;
        if (q[0]) strcat(q, "&");
        br_urlencode_append(q, sizeof(q), in->name);
        strcat(q, "=");
        br_urlencode_append(q, sizeof(q), in->value);
    }
    br_form_t *f = &br.forms[form];
    if (f->post) { br_post(f->action, q); return; }
    char u[1024];
    char base[512];
    snprintf(base, sizeof(base), "%s", f->action);
    char *qm = strchr(base, '?');
    if (qm) *qm = 0;
    snprintf(u, sizeof(u), "%s?%s", base, q);
    br_navigate(u, 1);
}

/* Called every frame: page and image loads finishing, downloads. */
static void br_poll(void)
{
    if (br.loading && (br.nav.state == HTTP_DONE || br.nav.state == HTTP_FAILED)) {
        br.loading = 0;
        snprintf(br.url, sizeof(br.url), "%s", br.nav.url);
        snprintf(br.addr, sizeof(br.addr), "%s", br.url);
        br.addr_len = (int)strlen(br.addr);
        if (br.nav.state == HTTP_FAILED) { br_error_page(br.nav.url, br.nav.err); }
        else {
            snprintf(br.ctype, sizeof(br.ctype), "%s", br.nav.ctype);
            snprintf(br.tls, sizeof(br.tls), "%s", br.nav.tls);
            br.secure = br.nav.tls[0] ? 1 : 0;
            const char *ct = br.nav.ctype;
            int html = !ct[0] || strstr(ct, "html") != NULL;
            int text = strstr(ct, "text/") || strstr(ct, "json") || strstr(ct, "xml") || strstr(ct, "javascript");
            int image = strstr(ct, "image/") != NULL;
            br.title[0] = 0;
            if (image) {
                static char page[1400];
                int k = snprintf(page, sizeof(page), "<title>%s</title><img src=\"%s\">", sh_basename(br.url), br.url);
                br_set_html(page, (size_t)k, 0);
                /* the bytes are already here: decode them for the image */
                br_layout(br.layout_w ? br.layout_w : 800);
                if (br.nimgs) {
                    char err[64];
                    if (image_decode(br.nav.data, br.nav.len, &br.imgs[0].img, err, sizeof(err)) == 0) br.imgs[0].state = 2; else br.imgs[0].state = 3;
                }
                br.layout_w = 0;
            } else if (html || text) {
                br_set_html(br.nav.data ? (char *)br.nav.data : "", br.nav.len, !html);
            } else {
                /* a file: save it in Downloads */
                char name[96], path[256];
                http_suggest_name(&br.nav, name, sizeof(name));
                if (!vfs_is_dir("/jarvis/data/Downloads")) vfs_mkdir("/jarvis/data/Downloads");
                snprintf(path, sizeof(path), "/jarvis/data/Downloads/%s", name);
                vfs_write_file(path, br.nav.data ? br.nav.data : (uint8_t *)"", br.nav.len);
                snprintf(br.msg, sizeof(br.msg), "Saved %s (%u KB) in Downloads.", name, (unsigned)(br.nav.len >> 10));
                notification_show("Download complete", name, NOTIF_INFO, 5000);
                desktop_files_changed();
                if (br.nback) { snprintf(br.url, sizeof(br.url), "%s", br.back[--br.nback]); br_navigate(br.url, 0); }
            }
        }
        http_free(&br.nav);
        desktop_invalidate();
    }
    /* images, one at a time */
    if (br.img_loading >= 0 && (br.img_req.state == HTTP_DONE || br.img_req.state == HTTP_FAILED)) {
        br_img_t *im = &br.imgs[br.img_loading];
        char err[64];
        if (br.img_req.state == HTTP_DONE && br.img_req.data && image_decode(br.img_req.data, br.img_req.len, &im->img, err, sizeof(err)) == 0) im->state = 2;
        else im->state = 3;
        http_free(&br.img_req);
        br.img_loading = -1;
        br.layout_w = 0;                                    /* sizes changed: lay out again */
        desktop_invalidate();
    }
    if (br.img_loading < 0 && !br.loading) {
        for (int i = 0; i < br.nimgs; i++)
            if (br.imgs[i].state == 0) {
                br.imgs[i].state = 1;
                if (!strncmp(br.imgs[i].url, "file://", 7)) {
                    char err[64];
                    br.imgs[i].state = image_load(br.imgs[i].url + 7, &br.imgs[i].img, err, sizeof(err)) == 0 ? 2 : 3;
                    br.layout_w = 0;
                    break;
                }
                if (strncmp(br.imgs[i].url, "http", 4)) { br.imgs[i].state = 3; continue; }
                memset(&br.img_req, 0, sizeof(br.img_req));
                snprintf(br.img_req.url, sizeof(br.img_req.url), "%s", br.imgs[i].url);
                br.img_loading = i;
                http_start(&br.img_req);
                break;
            }
    }
    /* downloads */
    for (int i = 0; i < BR_MAX_DL; i++) {
        br_dl_t *d = &br.dl[i];
        if (d->active && (d->req.state == HTTP_DONE || d->req.state == HTTP_FAILED) && !d->done_shown) {
            d->done_shown = 1;
            if (d->req.state == HTTP_DONE) {
                char b[160];
                snprintf(b, sizeof(b), "%s (%u KB) is in Downloads.", d->name, (unsigned)(d->req.len >> 10));
                notification_show("Download complete", b, NOTIF_INFO, 5000);
                desktop_files_changed();
            } else notification_show("Download failed", d->req.err, NOTIF_WARNING, 6000);
            if (d->req.data) http_free(&d->req);
        }
    }
}

/* ---- drawing ----------------------------------------------------------------------------- */
static const char *const br_btn_glyph[5] = { SYM_LEFT, SYM_RIGHT, "", "", "" };

static void br_draw_toolbar(int x, int y, int w)
{
    fb_fill_rect(x, y, w, BR_TOOL_H, T(SURFACE));
    fb_fill_rect(x, y + BR_TOOL_H - 1, w, 1, T(BORDER));
    /* back, forward, reload/stop, home */
    for (int i = 0; i < 4; i++) {
        int bx = x + 8 + i * 36;
        int enabled = i == 0 ? br.nback > 0 : i == 1 ? br.nfwd > 0 : 1;
        uint32_t fg = enabled ? T(TEXT) : T(TEXT_DIM);
        if (i < 2) fb_text(bx + 10, y + 13, br_btn_glyph[i], fg, FONT_UI_BOLD);
        else if (i == 2) {
            if (br.loading) fb_text(bx + 11, y + 13, SYM_TIMES, fg, FONT_UI_BOLD);
            else glyph_draw(GLYPH_REFRESH, bx + 6, y + 11, 22, fg, T(SURFACE));
        } else {                                            /* home: a little house */
            fb_fill_rect(bx + 10, y + 21, 14, 10, fg);
            for (int k = 0; k < 8; k++) fb_fill_rect(bx + 17 - k, y + 13 + k, 2 * k + 1, 1, fg);
            fb_fill_rect(bx + 15, y + 25, 4, 6, T(SURFACE));
        }
    }
    /* address bar */
    int ax = x + 156, aw = w - 156 - 96;
    fb_fill_rounded_rect(ax, y + 7, aw, 32, 16, T(INPUT));
    fb_draw_rounded_rect(ax, y + 7, aw, 32, 16, br.addr_focus ? T(ACCENT) : T(BORDER));
    if (!br.addr_focus && br.secure == 1) {                 /* padlock */
        fb_draw_rounded_rect(ax + 14, y + 13, 8, 9, 3, T(OK));
        fb_fill_rounded_rect(ax + 12, y + 19, 12, 10, 2, T(OK));
    } else if (!br.addr_focus && !strncmp(br.url, "http://", 7)) {
        fb_text(ax + 12, y + 15, "!", T(WARN), FONT_UI_BOLD);
    }
    const char *shown = br.addr;
    if (!br.addr_focus && !strcmp(br.url, BR_HOME)) shown = "";
    if (br.addr_focus && br.addr_sel_all) fb_fill_rect(ax + 36, y + 15, fb_text_width(br.addr) < aw - 50 ? fb_text_width(br.addr) : aw - 50, 16, T(ACCENT));
    if (shown[0]) ui_text_fit(ax + 36, y + 15, aw - 50, shown, br.addr_focus && br.addr_sel_all ? T(ACCENT_TEXT) : T(TEXT));
    else if (!br.addr_focus) fb_draw_text(ax + 36, y + 15, "Search or type a web address", T(TEXT_DIM));
    if (br.addr_focus && !br.addr_sel_all) {
        int cx = ax + 36 + fb_text_width(br.addr);
        if (cx > ax + aw - 14) cx = ax + aw - 14;
        fb_fill_rect(cx + 1, y + 15, 1, 16, T(TEXT));
    }
    /* favorite star, downloads */
    int fav = 0;
    {
        int k = reg_open_key("HKCU\\Software\\JARVIS OS\\Browser\\Favorites", 0);
        char nm[64], val[512];
        int type;
        for (int i = 0; k >= 0 && reg_value_at(k, i, nm, sizeof(nm), &type, val, sizeof(val), NULL) == 0; i++) if (!strcmp(val, br.url)) fav = 1;
    }
    int sx = x + w - 88;
    fb_text(sx + 8, y + 12, fav ? "*" : "+", fav ? 0x00F2B600 : T(TEXT_DIM), FONT_TITLE);
    int dx = x + w - 48;
    int active = 0;
    for (int i = 0; i < BR_MAX_DL; i++) if (br.dl[i].active && br.dl[i].req.state <= HTTP_RUNNING) active = 1;
    uint32_t dc = active ? T(ACCENT) : T(TEXT);
    fb_fill_rect(dx + 15, y + 11, 3, 13, dc);
    for (int k = 0; k < 6; k++) fb_fill_rect(dx + 16 - k, y + 24 - 6 + k, 2 * k + 1, 1, dc);
    fb_fill_rect(dx + 8, y + 30, 17, 2, dc);
}

static void br_draw_page(int x, int y, int w, int h)
{
    fb_fill_rect(x, y, w, h, BR_PAGE_BG);
    if (br.layout_w != w) br_layout(w);
    int max_scroll = br.page_h - h;
    if (max_scroll < 0) max_scroll = 0;
    if (br.scroll > max_scroll) br.scroll = max_scroll;
    if (br.scroll < 0) br.scroll = 0;
    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x, y, w, h);
    fb_clip_t c2 = fb_get_clip();
    if (c2.x0 < saved.x0) c2.x0 = saved.x0;
    if (c2.y0 < saved.y0) c2.y0 = saved.y0;
    if (c2.x1 > saved.x1) c2.x1 = saved.x1;
    if (c2.y1 > saved.y1) c2.y1 = saved.y1;
    fb_restore_clip(c2);
    /* first item that can be visible (items go down the page) */
    int lo = 0, hi = br.nitems;
    while (lo < hi) { int mid = (lo + hi) / 2; if (br.items[mid].y + 80 < br.scroll) lo = mid + 1; else hi = mid; }
    for (int i = lo; i < br.nitems; i++) {
        br_item_t *it = &br.items[i];
        int iy = y + it->y - br.scroll, ix = x + it->x;
        if (iy > y + h) break;
        if (iy + it->h < y) continue;
        switch (it->kind) {
        case BI_TEXT: {
            uint32_t col = it->color;
            int hovered = it->link >= 0 && it->link == br.hover_link;
            fb_text_n(ix, iy, br.pool + it->off, it->len, col, it->font);
            if (it->underline && hovered) fb_fill_rect(ix, iy + br_font_h(it->font) - 4, it->w, 1, col);
            break;
        }
        case BI_RULE: fb_fill_rect(ix, iy, it->w, 1, 0x00DADCE0); break;
        case BI_BULLET: fb_fill_circle(ix + 2, iy + 2, 3, BR_TEXT); break;
        case BI_IMAGE: {
            br_img_t *im = it->ref >= 0 ? &br.imgs[it->ref] : NULL;
            if (im && im->state == 2) {
                /* scaled copy at the drawn size, cached by size */
                if (im->w != it->w || im->h != it->h || !im->img.px) {
                    im->w = it->w; im->h = it->h;
                }
                image_t *src = &im->img;
                if (src->w == it->w && src->h == it->h) {
                    ph_blit(src->px, src->w, src->h, ix, iy);
                } else {
                    size_t bytes = (size_t)it->w * it->h * 4;
                    uint32_t *tmp = (uint32_t *)(bytes >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(bytes) : kmalloc(bytes));
                    if (tmp) {
                        image_scale(src, tmp, it->w, it->h);
                        /* keep the scaled copy instead of the original */
                        image_free(src);
                        src->px = tmp; src->w = it->w; src->h = it->h;
                        ph_blit(tmp, it->w, it->h, ix, iy);
                    }
                }
            } else {
                fb_fill_rect(ix, iy, it->w, it->h, 0x00F1F3F4);
                fb_draw_rect(ix, iy, it->w, it->h, 0x00DADCE0);
                const char *t = im && im->state == 3 ? "(picture not shown)" : "Loading picture...";
                if (it->len && im && im->state == 3) t = br.pool + it->off;
                ui_text_fit(ix + 6, iy + 6, it->w - 12, t, BR_DIM);
            }
            if (it->link >= 0 && it->link == br.hover_link) fb_draw_rect(ix - 1, iy - 1, it->w + 2, it->h + 2, BR_LINK);
            break;
        }
        case BI_INPUT: {
            br_input_t *in = &br.inputs[it->ref];
            int foc = br.focus_input == it->ref;
            fb_fill_rounded_rect(ix, iy, it->w, it->h, 4, 0x00FFFFFF);
            fb_draw_rounded_rect(ix, iy, it->w, it->h, 4, foc ? 0x001A73E8 : 0x00B0B4BA);
            if (in->value[0]) ui_text_fit(ix + 8, iy + 6, it->w - 16, in->value, BR_TEXT);
            else if (it->len) ui_text_fit(ix + 8, iy + 6, it->w - 16, br.pool + it->off, BR_DIM);
            if (foc) { int cx = ix + 9 + fb_text_width(in->value); if (cx < ix + it->w - 6) fb_fill_rect(cx, iy + 6, 1, 16, BR_TEXT); }
            break;
        }
        case BI_BUTTON: {
            br_input_t *in = &br.inputs[it->ref];
            fb_fill_rounded_rect(ix, iy, it->w, it->h, 4, 0x00F1F3F4);
            fb_draw_rounded_rect(ix, iy, it->w, it->h, 4, 0x00B0B4BA);
            ui_text_center(ix, iy + 6, it->w, in->value[0] ? in->value : "Go", BR_TEXT);
            break;
        }
        }
    }
    fb_restore_clip(saved);
    /* scrollbar */
    if (br.page_h > h) {
        int th = h * h / br.page_h;
        if (th < 24) th = 24;
        int ty = y + (h - th) * br.scroll / (max_scroll ? max_scroll : 1);
        fb_fill_rounded_rect(x + w - 8, ty, 5, th, 2, 0x00B0B4BA);
    }
    /* loading bar */
    if (br.loading) {
        int pw = (int)((g_kstate.uptime_ticks * 7) % (uint64_t)(w + 200)) - 200;
        fb_fill_rect(x + (pw < 0 ? 0 : pw), y, 200 + (pw < 0 ? pw : 0), 3, T(ACCENT));
    }
}

static void br_draw_downloads(int x, int y, int w)
{
    fb_fill_rect(x, y, w, BR_DL_H, T(SURFACE));
    fb_fill_rect(x, y, w, 1, T(BORDER));
    int dx = x + 10;
    int any = 0;
    for (int i = 0; i < BR_MAX_DL && dx < x + w - 60; i++) {
        br_dl_t *d = &br.dl[i];
        if (!d->active) continue;
        any = 1;
        int bw = 230;
        fb_fill_rounded_rect(dx, y + 5, bw, 30, 5, T(HOVER));
        char b[120];
        if (d->req.state == HTTP_DONE) {
            int jpk = strstr(d->name, ".jpk") != NULL;
            snprintf(b, sizeof(b), "%s", d->name);
            ui_text_fit(dx + 8, y + 12, bw - (jpk ? 70 : 16), b, T(TEXT));
            if (jpk) { fb_fill_rounded_rect(dx + bw - 62, y + 8, 56, 24, 4, T(ACCENT)); ui_text_center(dx + bw - 62, y + 12, 56, "Install", T(ACCENT_TEXT)); }
        } else if (d->req.state == HTTP_FAILED) {
            snprintf(b, sizeof(b), "%s: failed", d->name);
            ui_text_fit(dx + 8, y + 12, bw - 16, b, T(DANGER));
        } else {
            unsigned kb = (unsigned)(d->req.len >> 10);
            if (d->req.expected) snprintf(b, sizeof(b), "%s  %u%%", d->name, (unsigned)(d->req.len * 100 / d->req.expected));
            else snprintf(b, sizeof(b), "%s  %u KB", d->name, kb);
            ui_text_fit(dx + 8, y + 12, bw - 16, b, T(TEXT));
            if (d->req.expected) fb_fill_rect(dx + 4, y + 31, (int)((uint64_t)(bw - 8) * d->req.len / d->req.expected), 2, T(ACCENT));
        }
        dx += bw + 8;
    }
    if (!any) fb_draw_text(x + 12, y + 12, "No downloads yet. Links to files (.jpk, .zip, .deb ...) download into Downloads.", T(TEXT_DIM));
    fb_draw_text(x + w - 54, y + 12, "Close", T(ACCENT));
}

void browser_draw(wm_window_t *win, uint32_t *fbp)
{
    (void)fbp;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    br_poll();
    br_draw_toolbar(x, y, w);
    int dlh = br.show_dl ? BR_DL_H : 0;
    br_draw_page(x, y + BR_TOOL_H, w, h - BR_TOOL_H - BR_STATUS_H - dlh);
    if (br.show_dl) br_draw_downloads(x, y + h - BR_STATUS_H - dlh, w);
    int sy = y + h - BR_STATUS_H;
    fb_fill_rect(x, sy, w, BR_STATUS_H, T(SURFACE));
    fb_fill_rect(x, sy, w, 1, T(BORDER));
    char st[300];
    if (br.hover_link >= 0 && br.hover_link < br.nlinks) snprintf(st, sizeof(st), "%s", br.links[br.hover_link]);
    else if (br.loading) snprintf(st, sizeof(st), "%s%s", br.nav.phase[0] ? br.nav.phase : "Starting", SYM_ELLIPSIS);
    else if (br.msg[0]) snprintf(st, sizeof(st), "%s", br.msg);
    else if (br.secure == 1) snprintf(st, sizeof(st), "Secure connection (%s), certificate checked", br.tls);
    else if (!strncmp(br.url, "http://", 7)) snprintf(st, sizeof(st), "Not secure: this page is not encrypted (http)");
    else st[0] = 0;
    ui_text_fit(x + 10, sy + 4, w - 20, st, T(TEXT_DIM));
    char t[200];
    snprintf(t, sizeof(t), "%s - Browser", br.title[0] ? br.title : br.url[0] ? br.url : "New tab");
    wm_set_title(win, t);
    if (br.loading || br.img_loading >= 0) desktop_invalidate();
}

/* ---- input -------------------------------------------------------------------------------- */
static int br_hit_item(int px, int py)
{
    for (int i = 0; i < br.nitems; i++) {
        br_item_t *it = &br.items[i];
        if (py >= it->y && py < it->y + (it->kind == BI_TEXT ? br_font_h(it->font) : it->h) && px >= it->x && px < it->x + it->w) return i;
    }
    return -1;
}

static void br_toggle_favorite(void)
{
    const char *key = "HKCU\\Software\\JARVIS OS\\Browser\\Favorites";
    int k = reg_open_key(key, 1);
    char nm[64], val[512];
    int type;
    for (int i = 0; reg_value_at(k, i, nm, sizeof(nm), &type, val, sizeof(val), NULL) == 0; i++)
        if (!strcmp(val, br.url)) { reg_delete_value(key, nm); snprintf(br.msg, sizeof(br.msg), "Removed from favorites."); return; }
    char name[64];
    snprintf(name, sizeof(name), "%s", br.title[0] ? br.title : sh_basename(br.url));
    for (char *c = name; *c; c++) if (*c == '\\') *c = '/';
    reg_set_sz(key, name, br.url);
    snprintf(br.msg, sizeof(br.msg), "Added to favorites (on the home page).");
}

void browser_event(wm_window_t *win, wm_input_event_t *e)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    int dlh = br.show_dl ? BR_DL_H : 0;
    int ph = h - BR_TOOL_H - BR_STATUS_H - dlh;
    switch (e->type) {
    case IE_MOUSE_WHEEL:
        br.scroll -= e->data.mouse.button * 20;
        return;
    case IE_MOUSE_MOVE: {
        int mx = e->data.mouse.x, my = e->data.mouse.y - BR_TOOL_H;
        int link = -1;
        if (my >= 0 && my < ph) { int i = br_hit_item(mx, my + br.scroll); if (i >= 0) link = br.items[i].link; }
        if (link != br.hover_link) { br.hover_link = link; desktop_invalidate(); }
        return;
    }
    case IE_TEXT_INPUT: {
        char c = e->data.text.ch;
        if (c < 32 || c > 126) return;
        if (br.addr_focus) {
            if (br.addr_sel_all) { br.addr_len = 0; br.addr[0] = 0; br.addr_sel_all = 0; }
            if (br.addr_len < (int)sizeof(br.addr) - 1) { br.addr[br.addr_len++] = c; br.addr[br.addr_len] = 0; }
        } else if (br.focus_input >= 0) {
            br_input_t *in = &br.inputs[br.focus_input];
            size_t l = strlen(in->value);
            if (l < sizeof(in->value) - 1) { in->value[l] = c; in->value[l + 1] = 0; }
        } else if (c == ' ') br.scroll += ph - 40;
        return;
    }
    case IE_KEY_DOWN: {
        uint32_t k = e->data.keyboard.key;
        int mods = e->data.keyboard.mods;
        char lk = (char)(k | 32);
        if ((mods & KMOD_CTRL) && lk == 'l') { br.addr_focus = 1; br.addr_sel_all = 1; br.focus_input = -1; return; }
        if ((mods & KMOD_CTRL) && lk == 'd') { br_toggle_favorite(); return; }
        if ((mods & KMOD_CTRL) && lk == 'j') { br.show_dl = !br.show_dl; return; }
        if (k == WM_KEY_F1 + 4 || ((mods & KMOD_CTRL) && lk == 'r')) { br_navigate(br.url, 0); return; }
        if ((mods & KMOD_ALT) && k == WM_KEY_LEFT && br.nback) { if (br.nfwd < 24) snprintf(br.fwd[br.nfwd++], sizeof(br.fwd[0]), "%s", br.url); br_navigate(br.back[--br.nback], 0); return; }
        if ((mods & KMOD_ALT) && k == WM_KEY_RIGHT && br.nfwd) { br_history_push(); br_navigate(br.fwd[--br.nfwd], 0); return; }
        if (br.addr_focus) {
            if (k == WM_KEY_ENTER) { char t[1024]; snprintf(t, sizeof(t), "%s", br.addr); br_navigate(t, 1); }
            else if (k == WM_KEY_ESC) { br.addr_focus = 0; snprintf(br.addr, sizeof(br.addr), "%s", br.url); br.addr_len = (int)strlen(br.addr); }
            else if (k == WM_KEY_BACKSPACE) { if (br.addr_sel_all) { br.addr_len = 0; br.addr[0] = 0; br.addr_sel_all = 0; } else if (br.addr_len) br.addr[--br.addr_len] = 0; }
            else if (k == WM_KEY_LEFT || k == WM_KEY_RIGHT || k == WM_KEY_END) br.addr_sel_all = 0;
            return;
        }
        if (br.focus_input >= 0) {
            br_input_t *in = &br.inputs[br.focus_input];
            if (k == WM_KEY_ENTER) br_submit(in->form, -1);
            else if (k == WM_KEY_BACKSPACE && in->value[0]) in->value[strlen(in->value) - 1] = 0;
            else if (k == WM_KEY_ESC) br.focus_input = -1;
            return;
        }
        if (k == WM_KEY_DOWN) br.scroll += 40;
        else if (k == WM_KEY_UP) br.scroll -= 40;
        else if (k == WM_KEY_PGDN) br.scroll += ph - 40;
        else if (k == WM_KEY_PGUP) br.scroll -= ph - 40;
        else if (k == WM_KEY_HOME) br.scroll = 0;
        else if (k == WM_KEY_END) br.scroll = br.page_h;
        else if (k == WM_KEY_BACKSPACE && br.nback) { if (br.nfwd < 24) snprintf(br.fwd[br.nfwd++], sizeof(br.fwd[0]), "%s", br.url); br_navigate(br.back[--br.nback], 0); }
        return;
    }
    case IE_MOUSE_DOWN:
        break;
    default:
        return;
    }
    int mx = e->data.mouse.x, my = e->data.mouse.y;
    if (e->data.mouse.button == 2) {
        /* right-click a link: download it */
        int i = my >= BR_TOOL_H && my < BR_TOOL_H + ph ? br_hit_item(mx, my - BR_TOOL_H + br.scroll) : -1;
        if (i >= 0 && br.items[i].link >= 0) { br_download(br.links[br.items[i].link]); snprintf(br.msg, sizeof(br.msg), "Downloading the link into Downloads"); }
        return;
    }
    if (e->data.mouse.button != 1) return;
    br.addr_focus = 0;
    if (my < BR_TOOL_H) {
        int b = (mx - 8) / 36;
        if (mx >= 8 && mx < 8 + 4 * 36) {
            if (b == 0 && br.nback) { if (br.nfwd < 24) snprintf(br.fwd[br.nfwd++], sizeof(br.fwd[0]), "%s", br.url); br_navigate(br.back[--br.nback], 0); }
            else if (b == 1 && br.nfwd) { br_history_push(); br_navigate(br.fwd[--br.nfwd], 0); }
            else if (b == 2) { if (br.loading) { br.nav.cancel = 1; } else br_navigate(br.url, 0); }
            else if (b == 3) br_navigate(BR_HOME, 1);
            return;
        }
        if (mx >= 156 && mx < w - 96) { br.addr_focus = 1; br.addr_sel_all = 1; br.addr_len = (int)strlen(br.addr); br.focus_input = -1; return; }
        if (mx >= w - 88 && mx < w - 52) { br_toggle_favorite(); return; }
        if (mx >= w - 48) { br.show_dl = !br.show_dl; return; }
        return;
    }
    if (br.show_dl && my >= h - BR_STATUS_H - dlh && my < h - BR_STATUS_H) {
        if (mx >= w - 60) { br.show_dl = 0; return; }
        int dx = 10;
        for (int i = 0; i < BR_MAX_DL; i++) {
            br_dl_t *d = &br.dl[i];
            if (!d->active) continue;
            if (mx >= dx && mx < dx + 230) {
                if (d->req.state == HTTP_DONE && strstr(d->name, ".jpk") && mx >= dx + 230 - 62) apps_store_show_install(d->path);
                else if (d->req.state == HTTP_DONE) shell_open_path(d->path);
                else if (d->req.state == HTTP_FAILED) d->active = 0;
                return;
            }
            dx += 238;
        }
        return;
    }
    if (my >= BR_TOOL_H && my < BR_TOOL_H + ph) {
        br.focus_input = -1;
        int i = br_hit_item(mx, my - BR_TOOL_H + br.scroll);
        if (i < 0) return;
        br_item_t *it = &br.items[i];
        if (it->kind == BI_INPUT) { br.focus_input = it->ref; return; }
        if (it->kind == BI_BUTTON) { br_submit(br.inputs[it->ref].form, it->ref); return; }
        if (it->link >= 0) {
            const char *l = br.links[it->link];
            if (!strncmp(l, "mailto:", 7) || !strncmp(l, "javascript:", 11)) { snprintf(br.msg, sizeof(br.msg), "This kind of link cannot be opened here."); return; }
            char u[512];
            snprintf(u, sizeof(u), "%s", l);
            /* a link to a spot on the same page */
            const char *hash = strchr(u, '#');
            if (hash && !strncmp(u, br.url, (size_t)(hash - u)) && strlen(br.url) >= (size_t)(hash - u)) { br.scroll = 0; return; }
            br_navigate(u, 1);
        }
    }
}

wm_window_t *browser_app_open(int x, int y, int w, int h)
{
    br.win = wm_create_window("Browser", x, y, w, h, WS_DEFAULT, browser_draw, browser_event);
    if (!br.win) return NULL;
    br.win->icon = ICON_BROWSER;
    br.img_loading = -1;
    br.focus_input = -1;
    br.hover_link = -1;
    if (!br.url[0]) br_navigate(BR_HOME, 0);
    return br.win;
}

void browser_open_url(const char *url)
{
    desktop_app_launch(desktop_app_find("Browser"));
    br_navigate(url, 1);
}

void browser_open_file(const char *path)
{
    char u[600];
    snprintf(u, sizeof(u), "file://%s", path);
    browser_open_url(u);
}

/* curl / wget in the Terminal start downloads here when -o / wget is used. */
int browser_start_download(const char *url)
{
    br_download(url);
    return 0;
}
