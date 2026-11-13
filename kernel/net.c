/*
 * =============================================================================
 * JARVIS AI OS — Network stack (kernel/net.c)
 * Language: C17
 *
 * IPv4 networking over Ethernet:
 *
 *   ARP     address resolution, with a cache; answers requests for our IP
 *   IPv4    send / receive (no fragmentation)
 *   ICMP    answers echo requests; ping
 *   UDP     send / receive on bound ports
 *   DHCP    obtains address, netmask, gateway and DNS server; renews
 *   DNS     resolves host names (A records)
 *   TCP     client connections: handshake, in-order receive, acknowledged
 *           send with retransmission, orderly close
 *   HTTP    plain http:// GET
 *
 * A dedicated kernel process ("net") polls the network card and runs the
 * protocol timers. Other processes call the functions at the bottom of this
 * file; those block (sleeping, not spinning) until the answer arrives or a
 * timeout expires. One mutex protects all state.
 *
 * Not implemented: TLS (so no https://), IPv6, listening TCP sockets, IP
 * fragmentation, TCP congestion control (sends are stop-and-wait).
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

#define ETH_ARP   0x0806
#define ETH_IP    0x0800
#define IP_ICMP   1
#define IP_TCP    6
#define IP_UDP    17

#define TICKS_PER_SEC 100

static uint8_t  g_mac[6];
static uint32_t g_ip = 0, g_mask = 0, g_gw = 0, g_dns = 0;   /* host byte order */
static int      g_lock = -1;
static uint8_t  g_frame[1600];              /* outgoing frame (under the lock) */
static void   (*g_pkt_hook)(const uint8_t *, size_t) = NULL;

static const uint8_t MAC_BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- Helpers ------------------------------------------------------------- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static uint32_t sum16(const uint8_t *data, size_t len, uint32_t sum)
{
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    if (len & 1) sum += (uint32_t)(data[len - 1] << 8);
    return sum;
}

static uint16_t fold(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void net_lock(void)   { kmutex_lock(g_lock); }
static void net_unlock(void) { kmutex_unlock(g_lock); }

/* Sleep one tick with the lock released, so the net process can run. */
static void net_wait_tick(void)
{
    net_unlock();
    kthread_sleep(1);
    net_lock();
}

void nic_set_packet_hook(void (*hook)(const uint8_t *, size_t))
{
    g_pkt_hook = hook;
}

void net_format_ip(uint32_t ip, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

/* "a.b.c.d" -> address; returns 0 if the text is not a dotted quad. */
static int parse_ip(const char *s, uint32_t *out)
{
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); if (v > 255) return 0; }
        ip = (ip << 8) | v;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *out = ip;
    return 1;
}

/* ==========================================================================
 * Ethernet + ARP
 * ========================================================================== */
#define ARP_CACHE 16
static struct { uint32_t ip; uint8_t mac[6]; int valid; } g_arp[ARP_CACHE];
static int g_arp_next = 0;

static int eth_send(const uint8_t *dst, uint16_t type, size_t payload_len)
{
    memcpy(g_frame, dst, 6);
    memcpy(g_frame + 6, g_mac, 6);
    wr16(g_frame + 12, type);
    return nic_send(g_frame, 14 + payload_len);
}

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    if (!ip) return;
    for (int i = 0; i < ARP_CACHE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) { memcpy(g_arp[i].mac, mac, 6); return; }
    }
    g_arp[g_arp_next].ip = ip;
    memcpy(g_arp[g_arp_next].mac, mac, 6);
    g_arp[g_arp_next].valid = 1;
    g_arp_next = (g_arp_next + 1) % ARP_CACHE;
}

static const uint8_t *arp_lookup(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip) return g_arp[i].mac;
    return NULL;
}

static void arp_send(uint16_t oper, const uint8_t *dst_mac, const uint8_t *tha, uint32_t tpa)
{
    uint8_t *a = g_frame + 14;
    wr16(a, 1); wr16(a + 2, ETH_IP); a[4] = 6; a[5] = 4; wr16(a + 6, oper);
    memcpy(a + 8, g_mac, 6);  wr32(a + 14, g_ip);
    memcpy(a + 18, tha, 6);   wr32(a + 24, tpa);
    eth_send(dst_mac, ETH_ARP, 28);
}

static void arp_input(const uint8_t *a, size_t len)
{
    if (len < 28 || rd16(a) != 1 || rd16(a + 2) != ETH_IP) return;
    uint32_t spa = rd32(a + 14), tpa = rd32(a + 24);
    arp_learn(spa, a + 8);
    if (rd16(a + 6) == 1 && g_ip && tpa == g_ip)        /* who has our address? */
        arp_send(2, a + 8, a + 8, spa);
}

/* The Ethernet address to send to in order to reach `dst`. May block (up to
 * about a second) while asking the network, unless can_block is 0. */
static const uint8_t *next_hop_mac(uint32_t dst, int can_block)
{
    static const uint8_t zero[6] = { 0 };

    if (dst == 0xFFFFFFFFu) return MAC_BROADCAST;
    uint32_t hop = (g_mask && (dst & g_mask) == (g_ip & g_mask)) ? dst : g_gw;
    if (!hop) return NULL;

    const uint8_t *mac = arp_lookup(hop);
    for (int attempt = 0; !mac && attempt < 3; attempt++) {
        arp_send(1, MAC_BROADCAST, zero, hop);
        if (!can_block) return NULL;
        for (int t = 0; t < 40 && !(mac = arp_lookup(hop)); t++)
            net_wait_tick();
    }
    return mac;
}

/* ==========================================================================
 * IPv4
 * ========================================================================== */
static uint16_t g_ip_id = 1;

/* The payload must already be at g_frame + 34. */
static int ip_send(uint32_t src, uint32_t dst, uint8_t proto, size_t len, const uint8_t *mac)
{
    uint8_t *ip = g_frame + 14;
    if (!mac || len > 1480) return -1;
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, (uint16_t)(20 + len));
    wr16(ip + 4, g_ip_id++);
    wr16(ip + 6, 0x4000);                       /* don't fragment */
    ip[8] = 64; ip[9] = proto;
    wr16(ip + 10, 0);
    wr32(ip + 12, src);
    wr32(ip + 16, dst);
    wr16(ip + 10, fold(sum16(ip, 20, 0)));
    return eth_send(mac, ETH_IP, 20 + len);
}

/* ==========================================================================
 * ICMP
 * ========================================================================== */
static struct { uint16_t id, seq; int got; uint64_t sent_tick, reply_tick; } g_ping;

static void icmp_input(uint32_t src, const uint8_t *p, size_t len)
{
    if (len < 8) return;
    if (p[0] == 8 && g_ip) {                    /* echo request: answer it */
        const uint8_t *mac = next_hop_mac(src, 0);
        if (!mac || len > 1400) return;
        uint8_t *out = g_frame + 34;
        memcpy(out, p, len);
        out[0] = 0;
        wr16(out + 2, 0);
        wr16(out + 2, fold(sum16(out, len, 0)));
        ip_send(g_ip, src, IP_ICMP, len, mac);
    } else if (p[0] == 0 && rd16(p + 4) == g_ping.id && rd16(p + 6) == g_ping.seq) {
        g_ping.got = 1;
        g_ping.reply_tick = g_kstate.uptime_ticks;
    }
}

/* ==========================================================================
 * UDP
 * ========================================================================== */
#define UDP_SOCKS 4
static struct {
    uint16_t port;                              /* 0 = free */
    uint8_t  data[1500];
    size_t   len;
    int      ready;
} g_udp[UDP_SOCKS];

static void dhcp_input(const uint8_t *p, size_t len);

static int udp_send(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport,
                    const uint8_t *data, size_t len, const uint8_t *mac)
{
    uint8_t *u = g_frame + 34;
    if (len > 1472) return -1;
    wr16(u, sport); wr16(u + 2, dport); wr16(u + 4, (uint16_t)(8 + len)); wr16(u + 6, 0);
    memcpy(u + 8, data, len);
    return ip_send(src, dst, IP_UDP, 8 + len, mac);
}

static void udp_input(const uint8_t *p, size_t len)
{
    if (len < 8) return;
    uint16_t dport = rd16(p + 2);
    size_t dlen = rd16(p + 4);
    if (dlen < 8 || dlen > len) return;
    dlen -= 8;

    if (dport == 68) { dhcp_input(p + 8, dlen); return; }
    for (int i = 0; i < UDP_SOCKS; i++) {
        if (g_udp[i].port == dport && !g_udp[i].ready && dlen <= sizeof(g_udp[i].data)) {
            memcpy(g_udp[i].data, p + 8, dlen);
            g_udp[i].len = dlen;
            g_udp[i].ready = 1;
        }
    }
}

/* ==========================================================================
 * DHCP client
 * ========================================================================== */
enum { DHCP_IDLE = 0, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND };
static int      g_dhcp_state = DHCP_IDLE;
static uint32_t g_dhcp_xid, g_dhcp_server, g_dhcp_offer;
static uint64_t g_dhcp_tick, g_dhcp_renew_at;

static void dhcp_send(uint8_t type)
{
    static uint8_t m[300];
    memset(m, 0, sizeof(m));
    m[0] = 1; m[1] = 1; m[2] = 6;               /* request, Ethernet, 6-byte address */
    wr32(m + 4, g_dhcp_xid);
    wr16(m + 10, 0x8000);                       /* reply by broadcast */
    memcpy(m + 28, g_mac, 6);
    wr32(m + 236, 0x63825363);                  /* magic cookie */

    uint8_t *o = m + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (type == 3) {                            /* REQUEST */
        *o++ = 50; *o++ = 4; wr32(o, g_dhcp_offer); o += 4;
        *o++ = 54; *o++ = 4; wr32(o, g_dhcp_server); o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;      /* want mask, router, DNS */
    *o++ = 255;
    udp_send(0, 0xFFFFFFFFu, 68, 67, m, sizeof(m), MAC_BROADCAST);
    g_dhcp_tick = g_kstate.uptime_ticks;
}

static void dhcp_input(const uint8_t *p, size_t len)
{
    if (len < 241 || p[0] != 2 || rd32(p + 4) != g_dhcp_xid || rd32(p + 236) != 0x63825363)
        return;

    uint8_t type = 0;
    uint32_t mask = 0, router = 0, dns = 0, server = 0, lease = 0;
    for (size_t i = 240; i + 1 < len && p[i] != 255;) {
        if (p[i] == 0) { i++; continue; }
        uint8_t opt = p[i], n = p[i + 1];
        const uint8_t *v = p + i + 2;
        if (i + 2 + n > len) break;
        if (opt == 53 && n >= 1) type = v[0];
        else if (opt == 1 && n >= 4) mask = rd32(v);
        else if (opt == 3 && n >= 4) router = rd32(v);
        else if (opt == 6 && n >= 4) dns = rd32(v);
        else if (opt == 54 && n >= 4) server = rd32(v);
        else if (opt == 51 && n >= 4) lease = rd32(v);
        i += 2u + n;
    }

    if (type == 2 && g_dhcp_state == DHCP_SELECTING) {          /* OFFER */
        g_dhcp_offer = rd32(p + 16);
        g_dhcp_server = server;
        g_dhcp_state = DHCP_REQUESTING;
        dhcp_send(3);
    } else if (type == 5 && g_dhcp_state == DHCP_REQUESTING) {  /* ACK */
        char ip[16], gw[16], d[16];
        g_ip = rd32(p + 16);
        g_mask = mask ? mask : 0xFFFFFF00u;
        g_gw = router;
        g_dns = dns ? dns : router;
        g_dhcp_state = DHCP_BOUND;
        if (!lease) lease = 3600;
        g_dhcp_renew_at = g_kstate.uptime_ticks + (uint64_t)lease * TICKS_PER_SEC / 2;
        net_format_ip(g_ip, ip, sizeof(ip));
        net_format_ip(g_gw, gw, sizeof(gw));
        net_format_ip(g_dns, d, sizeof(d));
        terminal_printf("[NET] DHCP: address %s, gateway %s, DNS %s, lease %u s\n", ip, gw, d, lease);
    } else if (type == 6) {                                     /* NAK: start over */
        g_dhcp_state = DHCP_IDLE;
    }
}

static void dhcp_timer(void)
{
    uint64_t now = g_kstate.uptime_ticks;
    if (!nic_link_up()) return;

    if (g_dhcp_state == DHCP_BOUND) {
        if (now >= g_dhcp_renew_at) g_dhcp_state = DHCP_IDLE;   /* renew (address kept meanwhile) */
        return;
    }
    if (g_dhcp_state == DHCP_IDLE || now - g_dhcp_tick > 3 * TICKS_PER_SEC) {
        g_dhcp_xid = 0x4A560000u ^ (uint32_t)now ^ ((uint32_t)g_mac[5] << 8);
        g_dhcp_state = DHCP_SELECTING;
        dhcp_send(1);                                           /* DISCOVER */
    }
}

/* ==========================================================================
 * TCP (client side)
 * ========================================================================== */
#define TCP_SOCKS   8
#define TCP_RXBUF   32768
#define TCP_MSS     1400
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

enum { TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_SENT };

typedef struct {
    int      used, state;
    uint32_t rip;
    uint16_t rport, lport;
    uint32_t snd_nxt, snd_una, rcv_nxt;
    uint8_t *rx;
    uint32_t rx_head, rx_len;
    int      peer_fin, reset;
    uint8_t  mac[6];
} tcp_sock_t;

static tcp_sock_t g_tcp[TCP_SOCKS];
static uint16_t g_next_port = 49152;

static void tcp_output(tcp_sock_t *s, uint8_t flags, uint32_t seq, const uint8_t *data, size_t len)
{
    uint8_t *t = g_frame + 34;
    size_t hlen = (flags & TCP_SYN) ? 24 : 20;

    wr16(t, s->lport); wr16(t + 2, s->rport);
    wr32(t + 4, seq);
    wr32(t + 8, (flags & TCP_ACK) ? s->rcv_nxt : 0);
    t[12] = (uint8_t)((hlen / 4) << 4);
    t[13] = flags;
    wr16(t + 14, (uint16_t)(TCP_RXBUF - 1 - s->rx_len));        /* receive window */
    wr16(t + 16, 0); wr16(t + 18, 0);
    if (flags & TCP_SYN) { t[20] = 2; t[21] = 4; wr16(t + 22, TCP_MSS); }
    if (len) memcpy(t + hlen, data, len);

    uint8_t pseudo[12];
    wr32(pseudo, g_ip); wr32(pseudo + 4, s->rip);
    pseudo[8] = 0; pseudo[9] = IP_TCP; wr16(pseudo + 10, (uint16_t)(hlen + len));
    wr16(t + 16, fold(sum16(t, hlen + len, sum16(pseudo, 12, 0))));

    ip_send(g_ip, s->rip, IP_TCP, hlen + len, s->mac);
}

static void tcp_input(uint32_t src, const uint8_t *t, size_t len)
{
    if (len < 20) return;
    uint16_t sport = rd16(t), dport = rd16(t + 2);
    uint32_t seq = rd32(t + 4), ack = rd32(t + 8);
    uint8_t flags = t[13];
    size_t hlen = (size_t)(t[12] >> 4) * 4;
    if (hlen < 20 || hlen > len) return;
    const uint8_t *data = t + hlen;
    size_t dlen = len - hlen;

    tcp_sock_t *s = NULL;
    for (int i = 0; i < TCP_SOCKS; i++)
        if (g_tcp[i].used && g_tcp[i].rip == src && g_tcp[i].rport == sport && g_tcp[i].lport == dport)
            s = &g_tcp[i];
    if (!s) return;

    if (flags & TCP_RST) { s->reset = 1; return; }

    if (s->state == TCP_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == s->snd_nxt) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->state = TCP_ESTABLISHED;
            tcp_output(s, TCP_ACK, s->snd_nxt, NULL, 0);
        }
        return;
    }

    if ((flags & TCP_ACK) && (int32_t)(ack - s->snd_una) > 0 && (int32_t)(s->snd_nxt - ack) >= 0)
        s->snd_una = ack;

    int need_ack = 0;
    if (dlen) {
        need_ack = 1;
        if (seq == s->rcv_nxt) {                    /* in order: take what fits */
            uint32_t space = TCP_RXBUF - s->rx_len;
            uint32_t take = dlen < space ? (uint32_t)dlen : space;
            for (uint32_t i = 0; i < take; i++)
                s->rx[(s->rx_head + s->rx_len + i) % TCP_RXBUF] = data[i];
            s->rx_len += take;
            s->rcv_nxt += take;
            if (take < dlen) flags &= (uint8_t)~TCP_FIN;    /* rest (and FIN) will be resent */
        } else {
            flags &= (uint8_t)~TCP_FIN;             /* out of order: ask again */
        }
    }
    if ((flags & TCP_FIN) && seq + (uint32_t)dlen == s->rcv_nxt) {
        s->rcv_nxt++;
        s->peer_fin = 1;
        need_ack = 1;
    }
    if (need_ack)
        tcp_output(s, TCP_ACK, s->snd_nxt, NULL, 0);
}

/* ==========================================================================
 * Receive path
 * ========================================================================== */
static void ip_input(const uint8_t *eth_src, const uint8_t *ip, size_t len)
{
    if (len < 20 || (ip[0] >> 4) != 4) return;
    size_t ihl = (size_t)(ip[0] & 15) * 4;
    size_t total = rd16(ip + 2);
    if (ihl < 20 || total < ihl || total > len) return;
    if (fold(sum16(ip, ihl, 0)) != 0) return;               /* bad header checksum */
    if (rd16(ip + 6) & 0x3FFF) return;                      /* fragment: not supported */

    uint32_t src = rd32(ip + 12), dst = rd32(ip + 16);
    if (dst != g_ip && dst != 0xFFFFFFFFu && g_ip) return;

    if (g_pkt_hook) g_pkt_hook(ip, total);                  /* security monitor */

    /* Remember how to reach the sender, so replies never have to wait. */
    if (g_mask && (src & g_mask) == (g_ip & g_mask)) arp_learn(src, eth_src);

    const uint8_t *p = ip + ihl;
    size_t plen = total - ihl;
    switch (ip[9]) {
    case IP_ICMP: icmp_input(src, p, plen); break;
    case IP_UDP:  udp_input(p, plen);       break;
    case IP_TCP:  tcp_input(src, p, plen);  break;
    default: break;
    }
}

/* Called by the network card driver for every received frame. */
void net_receive(const uint8_t *frame, size_t len)
{
    if (len < 14) return;
    uint16_t type = rd16(frame + 12);
    if (type == ETH_ARP)      arp_input(frame + 14, len - 14);
    else if (type == ETH_IP)  ip_input(frame + 6, frame + 14, len - 14);
}

/* ---- The network process ------------------------------------------------- */
static void net_process(void)
{
    for (;;) {
        net_lock();
        nic_poll();
        dhcp_timer();
        net_unlock();
        kthread_sleep(1);
    }
}

void net_init(void)
{
    if (!nic_is_ready()) {
        terminal_puts("[NET] No network adapter: networking disabled\n");
        return;
    }
    nic_get_mac(g_mac);
    g_lock = kmutex_create("net");
    proc_spawn("net", net_process, PRIO_SYSTEM, 0);
    terminal_puts("[NET] Network stack started (ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP)\n");
}

/* ==========================================================================
 * Public API (callable from any process; these block)
 * ========================================================================== */
void net_get_info(net_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->adapter = nic_is_ready();
    if (!out->adapter) return;
    out->link = nic_link_up();
    out->configured = g_dhcp_state == DHCP_BOUND || g_ip != 0;
    out->ip = g_ip; out->mask = g_mask; out->gateway = g_gw; out->dns = g_dns;
    memcpy(out->mac, g_mac, 6);
    nic_get_counters(&out->rx_frames, &out->tx_frames);
}

/* Echo request; returns the round-trip time in ms, or -1 on timeout. */
int net_ping(uint32_t ip, int timeout_ms)
{
    static uint16_t seq = 0;
    if (g_lock < 0 || !g_ip) return -1;

    net_lock();
    const uint8_t *mac = next_hop_mac(ip, 1);
    if (!mac) { net_unlock(); return -1; }

    uint8_t *p = g_frame + 34;
    memset(p, 0, 40);
    p[0] = 8;
    g_ping.id = 0x4A56; g_ping.seq = ++seq; g_ping.got = 0;
    wr16(p + 4, g_ping.id); wr16(p + 6, g_ping.seq);
    for (int i = 8; i < 40; i++) p[i] = (uint8_t)i;
    wr16(p + 2, fold(sum16(p, 40, 0)));
    g_ping.sent_tick = g_kstate.uptime_ticks;
    ip_send(g_ip, ip, IP_ICMP, 40, mac);

    uint64_t deadline = g_ping.sent_tick + (uint64_t)timeout_ms / 10;
    while (!g_ping.got && g_kstate.uptime_ticks < deadline)
        net_wait_tick();
    int ms = g_ping.got ? (int)(g_ping.reply_tick - g_ping.sent_tick) * 10 : -1;
    net_unlock();
    return ms;
}

/* Host name (or dotted address) -> IPv4 address. 0 on success. */
int dns_resolve(const char *hostname, uint32_t *out_ip)
{
    static uint16_t qid = 0x1000;
    uint8_t q[300];

    if (parse_ip(hostname, out_ip)) return 0;
    if (g_lock < 0 || !g_ip || !g_dns || strlen(hostname) > 250) return -1;

    net_lock();
    int sock = -1;
    for (int i = 0; i < UDP_SOCKS; i++) if (!g_udp[i].port) { sock = i; break; }
    const uint8_t *mac = sock >= 0 ? next_hop_mac(g_dns, 1) : NULL;
    if (!mac) { net_unlock(); return -1; }

    uint16_t port = (uint16_t)(53000 + (g_kstate.uptime_ticks & 0xFFF));
    g_udp[sock].port = port;
    int rc = -1;

    for (int attempt = 0; attempt < 3 && rc != 0; attempt++) {
        uint16_t id = ++qid;
        size_t n = 12;
        memset(q, 0, sizeof(q));
        wr16(q, id); wr16(q + 2, 0x0100); wr16(q + 4, 1);       /* recursion desired, 1 question */
        for (const char *p = hostname; *p;) {
            const char *dot = strchr(p, '.');
            size_t l = dot ? (size_t)(dot - p) : strlen(p);
            if (l == 0 || l > 63) { n = 0; break; }
            q[n++] = (uint8_t)l;
            memcpy(q + n, p, l); n += l;
            p = dot ? dot + 1 : p + l;
        }
        if (!n) break;
        q[n++] = 0;
        wr16(q + n, 1); wr16(q + n + 2, 1); n += 4;             /* type A, class IN */

        g_udp[sock].ready = 0;
        mac = next_hop_mac(g_dns, 1);
        if (!mac) break;
        udp_send(g_ip, g_dns, port, 53, q, n, mac);

        /* Wait for the reply to *this* query; a late answer to an earlier
         * attempt (different id) is discarded, not taken as a failure. */
        int got = 0;
        for (int t = 0; t < 2 * TICKS_PER_SEC && !got; t++) {
            if (g_udp[sock].ready) {
                if (g_udp[sock].len >= 12 && rd16(g_udp[sock].data) == id) got = 1;
                else g_udp[sock].ready = 0;
            }
            if (!got) net_wait_tick();
        }
        if (!got) continue;

        const uint8_t *r = g_udp[sock].data;
        size_t len = g_udp[sock].len;
        if ((r[3] & 15) != 0) {                                 /* e.g. no such name */
            terminal_printf("[NET] DNS: %s: server answered with error %u\n", hostname, r[3] & 15);
            rc = -2;
            break;
        }
        size_t pos = 12;
        for (int qd = rd16(r + 4); qd > 0 && pos < len; qd--) {
            while (pos < len && r[pos]) pos += (r[pos] & 0xC0) == 0xC0 ? 2 : (size_t)r[pos] + 1;
            pos += 5;
        }
        for (int an = rd16(r + 6); an > 0 && pos + 12 <= len; an--) {
            if ((r[pos] & 0xC0) == 0xC0) pos += 2;
            else { while (pos < len && r[pos]) pos += (size_t)r[pos] + 1; pos++; }
            if (pos + 10 > len) break;
            uint16_t type = rd16(r + pos), rdlen = rd16(r + pos + 8);
            pos += 10;
            if (type == 1 && rdlen == 4 && pos + 4 <= len) { *out_ip = rd32(r + pos); rc = 0; break; }
            pos += rdlen;
        }
        if (rc != 0) {
            terminal_printf("[NET] DNS: %s: no IPv4 address in the answer\n", hostname);
            rc = -2;
            break;
        }
    }
    g_udp[sock].port = 0;
    net_unlock();
    return rc;
}

/* Open a TCP connection. Returns a socket number, or -1. */
int tcp_connect(uint32_t ip, uint16_t port)
{
    if (g_lock < 0 || !g_ip) return -1;
    net_lock();

    int id = -1;
    for (int i = 0; i < TCP_SOCKS; i++) if (!g_tcp[i].used) { id = i; break; }
    const uint8_t *mac = id >= 0 ? next_hop_mac(ip, 1) : NULL;
    uint8_t *rx = mac ? (uint8_t *)kmalloc(TCP_RXBUF) : NULL;
    if (!rx) { net_unlock(); return -1; }

    tcp_sock_t *s = &g_tcp[id];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->rx = rx;
    s->rip = ip; s->rport = port;
    s->lport = g_next_port++;
    if (g_next_port < 49152) g_next_port = 49152;
    memcpy(s->mac, mac, 6);
    uint32_t iss = (uint32_t)g_kstate.uptime_ticks * 2654435761u + s->lport;
    s->snd_una = iss;
    s->snd_nxt = iss + 1;
    s->state = TCP_SYN_SENT;

    for (int attempt = 0; attempt < 4 && s->state == TCP_SYN_SENT && !s->reset; attempt++) {
        tcp_output(s, TCP_SYN, iss, NULL, 0);
        for (int t = 0; t < TICKS_PER_SEC && s->state == TCP_SYN_SENT && !s->reset; t++)
            net_wait_tick();
    }
    if (s->state != TCP_ESTABLISHED) {
        kfree(s->rx);
        s->used = 0;
        id = -1;
    }
    net_unlock();
    return id;
}

/* Send data; returns when all of it has been acknowledged. 0 on success. */
int tcp_send(int id, const void *data, size_t len)
{
    if (id < 0 || id >= TCP_SOCKS) return -1;
    const uint8_t *p = (const uint8_t *)data;
    net_lock();
    tcp_sock_t *s = &g_tcp[id];
    int rc = (s->used && s->state == TCP_ESTABLISHED) ? 0 : -1;

    while (rc == 0 && len) {
        size_t n = len > TCP_MSS ? TCP_MSS : len;
        uint32_t seq = s->snd_nxt;
        s->snd_nxt = seq + (uint32_t)n;
        int acked = 0;
        for (int attempt = 0; attempt < 6 && !acked && !s->reset; attempt++) {
            tcp_output(s, TCP_ACK | TCP_PSH, seq, p, n);
            for (int t = 0; t < TICKS_PER_SEC && !s->reset; t++) {
                if (s->snd_una == s->snd_nxt) { acked = 1; break; }
                net_wait_tick();
            }
        }
        if (!acked) rc = -1;
        p += n; len -= n;
    }
    net_unlock();
    return rc;
}

/* Receive up to max bytes. Returns the count, 0 when the peer has closed
 * and everything was read, -1 on timeout or a broken connection. */
int tcp_recv(int id, void *buf, size_t max, int timeout_ms)
{
    if (id < 0 || id >= TCP_SOCKS) return -1;
    net_lock();
    tcp_sock_t *s = &g_tcp[id];
    uint64_t deadline = g_kstate.uptime_ticks + (uint64_t)timeout_ms / 10;

    while (s->used && !s->rx_len && !s->peer_fin && !s->reset && g_kstate.uptime_ticks < deadline)
        net_wait_tick();

    int n = -1;
    if (s->used && s->rx_len) {
        int was_full = s->rx_len > TCP_RXBUF - 2 * TCP_MSS;
        n = (int)(s->rx_len < max ? s->rx_len : max);
        for (int i = 0; i < n; i++)
            ((uint8_t *)buf)[i] = s->rx[(s->rx_head + (uint32_t)i) % TCP_RXBUF];
        s->rx_head = (s->rx_head + (uint32_t)n) % TCP_RXBUF;
        s->rx_len -= (uint32_t)n;
        if (was_full)                               /* tell the peer there is room again */
            tcp_output(s, TCP_ACK, s->snd_nxt, NULL, 0);
    } else if (s->used && s->peer_fin && !s->reset) {
        n = 0;
    }
    net_unlock();
    return n;
}

void tcp_close(int id)
{
    if (id < 0 || id >= TCP_SOCKS) return;
    net_lock();
    tcp_sock_t *s = &g_tcp[id];
    if (s->used) {
        if (s->state == TCP_ESTABLISHED && !s->reset) {
            uint32_t seq = s->snd_nxt;
            s->snd_nxt++;
            s->state = TCP_FIN_SENT;
            tcp_output(s, TCP_FIN | TCP_ACK, seq, NULL, 0);
            for (int t = 0; t < TICKS_PER_SEC && s->snd_una != s->snd_nxt && !s->reset; t++)
                net_wait_tick();
        }
        kfree(s->rx);
        s->used = 0;
    }
    net_unlock();
}

/*
 * HTTP GET. Stores the whole response (status line, headers, blank line,
 * body) in buf and NUL-terminates it. Returns the number of bytes, or a
 * negative code: -1 bad URL, -2 name not found, -3 could not connect,
 * -4 send failed, -5 https is not supported.
 */
int http_get(const char *url, void *buf, size_t max)
{
    char host[128], req[512];
    uint16_t port = 80;
    uint32_t ip;

    if (strncmp(url, "https://", 8) == 0) return -5;
    if (strncmp(url, "http://", 7) == 0) url += 7;

    size_t h = 0;
    while (url[h] && url[h] != '/' && url[h] != ':' && h < sizeof(host) - 1) { host[h] = url[h]; h++; }
    host[h] = 0;
    const char *p = url + h;
    if (!h || max < 2) return -1;
    if (*p == ':') {
        uint32_t v = 0;
        for (p++; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
        if (!v || v > 65535) return -1;
        port = (uint16_t)v;
    }
    const char *path = *p ? p : "/";

    if (dns_resolve(host, &ip) != 0) return -2;
    int s = tcp_connect(ip, port);
    if (s < 0) return -3;

    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: JARVIS-OS/1.0\r\nConnection: close\r\n\r\n",
             path, host);
    if (tcp_send(s, req, strlen(req)) != 0) { tcp_close(s); return -4; }

    size_t total = 0;
    for (;;) {
        int n = tcp_recv(s, (uint8_t *)buf + total, max - 1 - total, 15000);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= max - 1) break;
    }
    tcp_close(s);
    ((char *)buf)[total] = 0;
    return (int)total;
}

/* ==========================================================================
 * Network commands (ping, nslookup, http, scan)
 *
 * Each runs as its own kernel process so the desktop stays responsive while
 * it waits on the network; output goes to the Terminal.
 * ========================================================================== */
static char g_cmd[200];
static volatile int g_cmd_busy = 0;
static char g_http_buf[262144];

static void nprint(const char *text)
{
    user_console_write(text, strlen(text));
}

static void cmd_ping(const char *host)
{
    char line[128], ips[16];
    uint32_t ip;
    if (dns_resolve(host, &ip) != 0) { nprint("ping: cannot resolve that name\n"); return; }
    net_format_ip(ip, ips, sizeof(ips));
    int ok = 0;
    for (int i = 0; i < 4; i++) {
        int ms = net_ping(ip, 1500);
        if (ms >= 0) { snprintf(line, sizeof(line), "Reply from %s: time %s%d ms\n", ips, ms == 0 ? "<" : "", ms == 0 ? 10 : ms); ok++; }
        else         snprintf(line, sizeof(line), "Request to %s timed out\n", ips);
        nprint(line);
        kthread_sleep(50);
    }
    snprintf(line, sizeof(line), "%d of 4 replies received\n", ok);
    nprint(line);
}

static void cmd_http(char *args)
{
    char line[160];
    char *file = strchr(args, ' ');
    if (file) { *file++ = 0; while (*file == ' ') file++; }

    int n = http_get(args, g_http_buf, sizeof(g_http_buf));
    if (n < 0) {
        static const char *const why[] = { "", "bad address", "name not found", "could not connect",
                                           "connection lost", "https is not supported (no TLS yet)" };
        snprintf(line, sizeof(line), "http: %s\n", why[-n <= 5 ? -n : 1]);
        nprint(line);
        return;
    }

    char *body = strstr(g_http_buf, "\r\n\r\n");
    char *eol = strstr(g_http_buf, "\r\n");
    if (eol) {                                      /* status line */
        size_t l = (size_t)(eol - g_http_buf) < 100 ? (size_t)(eol - g_http_buf) : 100;
        memcpy(line, g_http_buf, l); line[l] = '\n'; line[l + 1] = 0;
        nprint(line);
    }
    body = body ? body + 4 : g_http_buf;
    size_t blen = (size_t)n - (size_t)(body - g_http_buf);

    if (file && *file) {
        char path[160];
        if (file[0] == '/') snprintf(path, sizeof(path), "%s", file);
        else                snprintf(path, sizeof(path), "/jarvis/data/%s", file);
        int rc = vfs_write_file(path, body, blen);
        snprintf(line, sizeof(line), rc == 0 ? "Saved %u bytes to %s\n" : "Could not save %u bytes to %s\n",
                 (unsigned)blen, path);
        nprint(line);
    } else {
        size_t show = blen < 1200 ? blen : 1200;
        for (size_t i = 0; i < show; i++) if (body[i] == '\r') body[i] = ' ';
        user_console_write(body, show);
        snprintf(line, sizeof(line), "\n[%u bytes received%s]\n", (unsigned)blen,
                 blen > show ? ", first 1200 shown" : "");
        nprint(line);
    }
}

/* Ask every address on the local network who is there (ARP sweep). */
static void cmd_scan(void)
{
    static const uint8_t zero[6] = { 0 };
    char line[96], ips[16];
    if (!g_ip) { nprint("scan: no network address yet\n"); return; }

    uint32_t net = g_ip & g_mask, hosts = ~g_mask;
    if (hosts > 254) hosts = 254;
    net_lock();
    for (uint32_t h = 1; h <= hosts; h++) {
        if ((net | h) != g_ip) arp_send(1, MAC_BROADCAST, zero, net | h);
        if (h % 16 == 0) net_wait_tick();
    }
    for (int t = 0; t < TICKS_PER_SEC; t++) net_wait_tick();

    int found = 0;
    for (int i = 0; i < ARP_CACHE; i++) {
        if (!g_arp[i].valid) continue;
        net_format_ip(g_arp[i].ip, ips, sizeof(ips));
        snprintf(line, sizeof(line), "  %s  %02x:%02x:%02x:%02x:%02x:%02x%s\n", ips,
                 g_arp[i].mac[0], g_arp[i].mac[1], g_arp[i].mac[2], g_arp[i].mac[3],
                 g_arp[i].mac[4], g_arp[i].mac[5], g_arp[i].ip == g_gw ? "  (gateway)" : "");
        net_unlock(); nprint(line); net_lock();
        found++;
    }
    net_unlock();
    snprintf(line, sizeof(line), "%d device(s) answered on the local network\n", found);
    nprint(line);
}

static void net_command_process(void)
{
    char *args = strchr(g_cmd, ' ');
    if (args) { *args++ = 0; while (*args == ' ') args++; } else args = g_cmd + strlen(g_cmd);

    if (g_lock < 0) {
        nprint("No network adapter.\n");
    } else if (!g_ip) {
        nprint("Not connected: no address from DHCP yet.\n");
    } else if (strcmp(g_cmd, "ping") == 0 && *args) {
        cmd_ping(args);
    } else if (strcmp(g_cmd, "nslookup") == 0 && *args) {
        char line[160], ips[16];
        uint32_t ip;
        if (dns_resolve(args, &ip) == 0) { net_format_ip(ip, ips, sizeof(ips)); snprintf(line, sizeof(line), "%s has address %s\n", args, ips); }
        else snprintf(line, sizeof(line), "%s: name not found\n", args);
        nprint(line);
    } else if (strcmp(g_cmd, "http") == 0 && *args) {
        cmd_http(args);
    } else if (strcmp(g_cmd, "scan") == 0) {
        cmd_scan();
    } else {
        nprint("usage: ping <host> | nslookup <host> | http <url> [file] | scan\n");
    }
    g_cmd_busy = 0;
}

/* Start a network command in the background. Returns 0, or -1 if one is
 * already running. */
int net_command_start(const char *cmdline)
{
    static uint32_t worker_pid = 0;
    if (g_cmd_busy && proc_exists(worker_pid)) return -1;   /* (a killed worker frees the slot) */
    g_cmd_busy = 1;
    strncpy(g_cmd, cmdline, sizeof(g_cmd) - 1);
    g_cmd[sizeof(g_cmd) - 1] = 0;
    worker_pid = proc_spawn("net-cmd", net_command_process, PRIO_USER, 0);
    if (!worker_pid) {
        g_cmd_busy = 0;
        return -1;
    }
    return 0;
}

void security_net_scan(const char *cidr)
{
    (void)cidr;                     /* the local network is always what is scanned */
    net_command_start("scan");
}
