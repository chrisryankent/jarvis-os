/*
 * =============================================================================
 * JARVIS AI OS — HTTP/HTTPS client (kernel/http.c)
 * Language: C17
 *
 * Fetches a URL in a worker process (so the desktop never waits on the
 * network): DNS, TCP, TLS 1.3 for https (kernel/tls.c), HTTP/1.1 GET or
 * POST, redirects (up to 8), chunked transfer, Content-Length progress,
 * up to 64 MB. Used by the Browser, curl/wget, downloads and JARVIS.
 * The caller fills an http_req_t, calls http_start() and watches ->state.
 * =============================================================================
 */

#include "kernel.h"
#include "crypto.h"
#include <string.h>
#include <stdio.h>

#define HTTP_MAX_BODY   (64u * 1024 * 1024)
#define HTTP_SLOTS      8

static http_req_t *volatile g_queue[HTTP_SLOTS];

static int ci_prefix(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        char a = *s >= 'A' && *s <= 'Z' ? (char)(*s + 32) : *s;
        if (a != *p) return 0;
    }
    return 1;
}

/* scheme://host[:port]/path -> parts. 0 on success. */
int http_parse_url(const char *url, int *https, char *host, size_t hn, int *port, char *path, size_t pn)
{
    const char *p = url;
    *https = 0;
    if (ci_prefix(p, "https://")) { *https = 1; p += 8; }
    else if (ci_prefix(p, "http://")) p += 7;
    else return -1;
    size_t h = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && h < hn - 1) host[h++] = *p++;
    host[h] = 0;
    if (!h) return -1;
    *port = *https ? 443 : 80;
    if (*p == ':') {
        int v = 0;
        p++;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (v <= 0 || v > 65535) return -1;
        *port = v;
    }
    if (*p == '/' || *p == '?') snprintf(path, pn, "%s%s", *p == '?' ? "/" : "", p);
    else snprintf(path, pn, "/");
    char *frag = strchr(path, '#');
    if (frag) *frag = 0;
    return 0;
}

/* Resolves a link against the page it is on. */
void http_resolve_url(const char *base, const char *href, char *out, size_t n)
{
    while (*href == ' ' || *href == '\t' || *href == '\n') href++;
    if (ci_prefix(href, "http://") || ci_prefix(href, "https://") || ci_prefix(href, "file:") || ci_prefix(href, "about:") ||
        ci_prefix(href, "mailto:") || ci_prefix(href, "javascript:")) { snprintf(out, n, "%s", href); return; }
    const char *sch = strstr(base, "://");
    if (!sch) { snprintf(out, n, "%s", href); return; }
    size_t root_len = (size_t)(sch + 3 - base);
    const char *hs = sch + 3, *pathp = hs;
    while (*pathp && *pathp != '/' && *pathp != '?' && *pathp != '#') pathp++;
    if (href[0] == '/' && href[1] == '/') { snprintf(out, n, "%.*s%s", (int)(root_len - 2), base, href); return; }
    if (href[0] == '/') { snprintf(out, n, "%.*s%s", (int)(pathp - base), base, href); return; }
    if (href[0] == '#') {
        const char *f = strchr(base, '#');
        snprintf(out, n, "%.*s%s", (int)(f ? (size_t)(f - base) : strlen(base)), base, href);
        return;
    }
    if (href[0] == '?') {
        const char *q = pathp;
        while (*q && *q != '?' && *q != '#') q++;
        snprintf(out, n, "%.*s%s", (int)(q - base), base, href);
        return;
    }
    /* relative path: directory of the base, then resolve ./ and ../ */
    char dir[1024];
    const char *end = pathp;
    while (*end && *end != '?' && *end != '#') end++;
    const char *slash = end;
    while (slash > pathp && slash[-1] != '/') slash--;
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - pathp), pathp);
    if (!dir[0]) snprintf(dir, sizeof(dir), "/");
    char joined[1024];
    snprintf(joined, sizeof(joined), "%s%s", dir, href);
    /* normalise segments */
    char *segs[128];
    int ns = 0;
    char *q = joined;
    int trailing = 0;
    while (*q) {
        while (*q == '/') q++;
        if (!*q) { trailing = 1; break; }
        char *s = q;
        while (*q && *q != '/') q++;
        int last = !*q;
        if (*q) *q++ = 0;
        if (!strcmp(s, ".")) { trailing = last; continue; }
        if (!strcmp(s, "..")) { if (ns) ns--; trailing = last; continue; }
        if (ns < 128) segs[ns++] = s;
        trailing = 0;
    }
    size_t o = (size_t)snprintf(out, n, "%.*s", (int)(pathp - base), base);
    for (int i = 0; i < ns && o < n; i++) o += (size_t)snprintf(out + o, n - o, "/%s", segs[i]);
    if ((trailing || !ns) && o < n) snprintf(out + o, n - o, "/");
}

static int body_append(http_req_t *r, const uint8_t *d, size_t n)
{
    if (r->len + n > HTTP_MAX_BODY) return -1;
    if (r->len + n + 1 > r->cap) {
        size_t nc = r->cap ? r->cap : 65536;
        while (nc < r->len + n + 1) nc *= 2;
        if (nc > HTTP_MAX_BODY + 1) nc = HTTP_MAX_BODY + 1;
        uint8_t *nd = (uint8_t *)crypto_alloc(nc);
        if (!nd) return -1;
        if (r->data) { memcpy(nd, r->data, r->len); crypto_free(r->data, r->cap); }
        r->data = nd;
        r->cap = nc;
    }
    memcpy(r->data + r->len, d, n);
    r->len += n;
    r->data[r->len] = 0;
    return 0;
}

void http_free(http_req_t *r)
{
    if (r->data) crypto_free(r->data, r->cap);
    r->data = NULL;
    r->len = r->cap = 0;
}

/* In-place decoding of a chunked body. */
static void dechunk(http_req_t *r)
{
    uint8_t *in = r->data, *out = r->data;
    size_t i = 0, o = 0;
    while (i < r->len) {
        size_t sz = 0;
        int digits = 0;
        while (i < r->len) {
            char c = (char)in[i];
            int d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
            if (d < 0) break;
            sz = sz * 16 + (size_t)d;
            i++;
            digits = 1;
        }
        while (i < r->len && in[i] != '\n') i++;           /* extensions, CRLF */
        i++;
        if (!digits || !sz) break;
        if (i + sz > r->len) sz = r->len - i;
        memmove(out + o, in + i, sz);
        o += sz;
        i += sz + 2;
    }
    r->len = o;
    if (r->data) r->data[o] = 0;
}

static int g_http_lock = -1;

/* One request/response. 0 done, 1 redirect (r->url updated), -1 failed. */
static int fetch_once(http_req_t *r)
{
    int https, port;
    char host[256], path[1024];
    if (http_parse_url(r->url, &https, host, sizeof(host), &port, path, sizeof(path))) { snprintf(r->err, sizeof(r->err), "not a web address"); return -1; }
    snprintf(r->phase, sizeof(r->phase), "Looking up %s", host);
    uint32_t ip = 0;
    {   /* a literal IPv4 address? */
        uint32_t parts[4] = { 0 };
        int np = 0, digits = 0, okip = 1;
        for (const char *q = host; *q && okip; q++) {
            if (*q >= '0' && *q <= '9') { parts[np] = parts[np] * 10 + (uint32_t)(*q - '0'); digits = 1; if (parts[np] > 255) okip = 0; }
            else if (*q == '.' && digits && np < 3) { np++; digits = 0; }
            else okip = 0;
        }
        if (okip && np == 3 && digits) ip = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
    }
    if (ip) { }
    else if (dns_resolve(host, &ip) != 0) { snprintf(r->err, sizeof(r->err), "cannot find %s (no internet, or the name does not exist)", host); return -1; }
    snprintf(r->phase, sizeof(r->phase), "Connecting to %s", host);
    int sock = tcp_connect(ip, (uint16_t)port);
    if (sock < 0) { snprintf(r->err, sizeof(r->err), "%s did not accept the connection", host); return -1; }
    tls_conn_t *t = NULL;
    if (https) {
        snprintf(r->phase, sizeof(r->phase), "Securing the connection to %s", host);
        t = tls_connect(sock, host, r->err, sizeof(r->err));
        if (!t) { tcp_close(sock); return -1; }
        snprintf(r->tls, sizeof(r->tls), "%s", tls_cipher_name(t));
    } else r->tls[0] = 0;

    static char req[4096];
    int post = !strcmp(r->method, "POST");
    int rl = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s%s\r\nUser-Agent: Mozilla/5.0 (JARVIS OS) JarvisBrowser/1.0\r\n"
                      "Accept: text/html,text/plain,image/png,image/jpeg,*/*;q=0.8\r\nAccept-Encoding: identity\r\nAccept-Language: en\r\n"
                      "Connection: close\r\n%s%s%s",
                      post ? "POST" : "GET", path, host, "", post ? "Content-Type: application/x-www-form-urlencoded\r\n" : "", "", "");
    if (post) rl += snprintf(req + rl, sizeof(req) - (size_t)rl, "Content-Length: %u\r\n", (unsigned)r->post_len);
    rl += snprintf(req + rl, sizeof(req) - (size_t)rl, "\r\n");
    int sent = t ? tls_write(t, req, (size_t)rl) : tcp_send(sock, req, (size_t)rl);
    if (!sent && post && r->post_len) sent = t ? tls_write(t, r->post, r->post_len) : tcp_send(sock, r->post, r->post_len);
    if (sent != 0) { snprintf(r->err, sizeof(r->err), "the request could not be sent"); goto fail; }

    snprintf(r->phase, sizeof(r->phase), "Waiting for %s", host);
    static uint8_t buf[16384];
    static char head[16384];
    size_t hlen = 0;
    int chunked = 0, have_len = 0;
    uint64_t clen = 0;
    char *body_start = NULL;
    /* headers */
    while (!body_start) {
        int n = t ? tls_read(t, buf, sizeof(buf), 30000) : tcp_recv(sock, buf, sizeof(buf), 30000);
        if (n <= 0) { if (!r->err[0]) snprintf(r->err, sizeof(r->err), "the server sent no answer"); goto fail; }
        if (hlen + (size_t)n >= sizeof(head)) { snprintf(r->err, sizeof(r->err), "the reply headers are too long"); goto fail; }
        memcpy(head + hlen, buf, (size_t)n);
        hlen += (size_t)n;
        head[hlen] = 0;
        char *e = strstr(head, "\r\n\r\n");
        if (e) body_start = e + 4;
    }
    r->status = 0;
    {
        const char *sp = strchr(head, ' ');
        if (sp) for (sp++; *sp >= '0' && *sp <= '9'; sp++) r->status = r->status * 10 + (*sp - '0');
    }
    r->ctype[0] = r->disposition[0] = 0;
    char location[1024] = "";
    for (char *line = strstr(head, "\r\n") + 2; line < body_start - 2;) {
        char *eol = strstr(line, "\r\n");
        if (!eol) break;
        *eol = 0;
        char *v = strchr(line, ':');
        if (v) {
            v++;
            while (*v == ' ') v++;
            if (ci_prefix(line, "content-length:")) { have_len = 1; clen = 0; for (char *q = v; *q >= '0' && *q <= '9'; q++) clen = clen * 10 + (uint64_t)(*q - '0'); }
            else if (ci_prefix(line, "transfer-encoding:") && strstr(v, "chunked")) chunked = 1;
            else if (ci_prefix(line, "content-type:")) snprintf(r->ctype, sizeof(r->ctype), "%s", v);
            else if (ci_prefix(line, "content-disposition:")) snprintf(r->disposition, sizeof(r->disposition), "%s", v);
            else if (ci_prefix(line, "location:")) snprintf(location, sizeof(location), "%s", v);
        }
        line = eol + 2;
    }
    if (r->status >= 300 && r->status < 400 && location[0] && r->status != 304) {
        char next[1024];
        http_resolve_url(r->url, location, next, sizeof(next));
        snprintf(r->url, sizeof(r->url), "%s", next);
        if (r->status != 307 && r->status != 308) { snprintf(r->method, sizeof(r->method), "GET"); r->post_len = 0; }
        if (t) tls_close(t);
        tcp_close(sock);
        return 1;
    }
    r->expected = have_len && !chunked ? clen : 0;
    if (r->expected > HTTP_MAX_BODY) { snprintf(r->err, sizeof(r->err), "the file is larger than 64 MB"); goto fail; }
    r->len = 0;
    size_t first = hlen - (size_t)(body_start - head);
    if (first && body_append(r, (uint8_t *)body_start, first)) { snprintf(r->err, sizeof(r->err), "out of memory"); goto fail; }
    snprintf(r->phase, sizeof(r->phase), "Receiving from %s", host);
    while (!r->cancel && !(have_len && !chunked && r->len >= clen)) {
        int n = t ? tls_read(t, buf, sizeof(buf), 30000) : tcp_recv(sock, buf, sizeof(buf), 30000);
        if (n == 0) break;
        if (n < 0) {
            if (have_len && r->len < clen) { snprintf(r->err, sizeof(r->err), "the connection was lost after %u KB", (unsigned)(r->len >> 10)); goto fail; }
            break;
        }
        if (body_append(r, buf, (size_t)n)) { snprintf(r->err, sizeof(r->err), "the file is too large (over 64 MB) or memory is full"); goto fail; }
    }
    if (r->cancel) { snprintf(r->err, sizeof(r->err), "cancelled"); goto fail; }
    if (chunked) dechunk(r);
    if (t) tls_close(t);
    tcp_close(sock);
    return 0;
fail:
    if (t) tls_close(t);
    tcp_close(sock);
    return -1;
}

static void http_worker(void)
{
    http_req_t *r = NULL;
    kmutex_lock(g_http_lock);
    for (int i = 0; i < HTTP_SLOTS && !r; i++)
        if (g_queue[i] && g_queue[i]->state == HTTP_QUEUED) { r = g_queue[i]; g_queue[i] = NULL; r->state = HTTP_RUNNING; }
    kmutex_unlock(g_http_lock);
    if (!r) return;
    int rc = -1;
    for (r->redirects = 0; r->redirects < 9; r->redirects++) {
        rc = fetch_once(r);
        if (rc != 1) break;
    }
    if (rc == 1) snprintf(r->err, sizeof(r->err), "too many redirects");
    if (rc == 0 && r->save_path[0]) {
        snprintf(r->phase, sizeof(r->phase), "Saving");
        if (vfs_write_file(r->save_path, r->data ? r->data : (uint8_t *)"", r->len) != 0) { snprintf(r->err, sizeof(r->err), "could not save %s", r->save_path); rc = -1; }
    }
    r->state = rc == 0 ? HTTP_DONE : HTTP_FAILED;
    scheduler_wake_main();
}

/* Queues the request and starts a worker process for it. */
int http_start(http_req_t *r)
{
    if (g_http_lock < 0) g_http_lock = kmutex_create("http");
    if (!r->method[0]) snprintf(r->method, sizeof(r->method), "GET");
    r->err[0] = r->phase[0] = r->tls[0] = 0;
    r->status = 0;
    r->len = 0;
    r->expected = 0;
    r->cancel = 0;
    kmutex_lock(g_http_lock);
    int slot = -1;
    for (int i = 0; i < HTTP_SLOTS; i++) if (!g_queue[i]) { slot = i; break; }
    if (slot >= 0) { r->state = HTTP_QUEUED; g_queue[slot] = r; }
    kmutex_unlock(g_http_lock);
    if (slot < 0) { snprintf(r->err, sizeof(r->err), "too many downloads at once"); r->state = HTTP_FAILED; return -1; }
    if (!proc_spawn("http", http_worker, PRIO_USER, 0)) {
        kmutex_lock(g_http_lock);
        g_queue[slot] = NULL;
        kmutex_unlock(g_http_lock);
        snprintf(r->err, sizeof(r->err), "could not start a network process");
        r->state = HTTP_FAILED;
        return -1;
    }
    return 0;
}

/* File name for a download: Content-Disposition, else the last part of the URL. */
void http_suggest_name(const http_req_t *r, char *out, size_t n)
{
    const char *fn = r->disposition[0] ? strstr(r->disposition, "filename=") : NULL;
    if (fn) {
        fn += 9;
        if (*fn == '"') fn++;
        size_t k = 0;
        while (fn[k] && fn[k] != '"' && fn[k] != ';' && k < n - 1) { out[k] = fn[k] == '/' ? '_' : fn[k]; k++; }
        out[k] = 0;
        if (k) return;
    }
    const char *u = r->url, *end = u;
    while (*end && *end != '?' && *end != '#') end++;
    const char *s = end;
    while (s > u && s[-1] != '/') s--;
    size_t k = 0;
    for (const char *c = s; c < end && k < n - 1; c++) {
        if (*c == '%' && c + 2 < end) {                     /* %20 etc. */
            int v = 0;
            for (int i = 1; i <= 2; i++) { char h = c[i]; v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : (h | 32) - 'a' + 10); }
            out[k++] = v >= 32 && v < 127 && v != '/' ? (char)v : '_';
            c += 2;
        } else out[k++] = *c;
    }
    out[k] = 0;
    if (!k || !strcmp(out, "/")) snprintf(out, n, "download");
}
