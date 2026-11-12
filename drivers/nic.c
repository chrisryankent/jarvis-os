/*
 * =============================================================================
 * JARVIS AI OS — Network Driver + Mini TCP/IP Stack  (drivers/nic.c)
 * Language: C17
 *
 * Targets: Intel e1000/e1000e (most common in QEMU + real hardware).
 * Provides:
 *   - Ethernet frame TX/RX
 *   - ARP, IPv4, UDP, TCP (minimal implementation)
 *   - DNS resolver (for model downloads, threat intel)
 *   - HTTP GET client (for downloading model updates)
 *
 * The AI security daemon gets a hook on every received packet header
 * for real-time network anomaly detection.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include <string.h>

void net_receive(const uint8_t *frame, size_t len);

/* ---- e1000 register offsets ---------------------------------------------- */
#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_RCTL      0x0100   /* Receive control */
#define E1000_TCTL      0x0400   /* Transmit control */
#define E1000_RDBAL     0x2800   /* RX descriptor base low */
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810   /* RX descriptor head */
#define E1000_RDT       0x2818   /* RX descriptor tail */
#define E1000_TDBAL     0x3800   /* TX descriptor base low */
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_RAL       0x5400   /* Receive address low */
#define E1000_RAH       0x5404

#define E1000_RCTL_EN   (1 << 1)
#define E1000_RCTL_BAM  (1 << 15)  /* broadcast accept */
#define E1000_RCTL_BSIZE_2048 0
#define E1000_TCTL_EN   (1 << 1)
#define E1000_TCTL_PSP  (1 << 3)

/* ---- RX/TX descriptor --------------------------------------------------- */
#define NUM_RX_DESC  32
#define NUM_TX_DESC  32
#define RX_BUF_SIZE  2048

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) tx_desc_t;

/* ---- Driver state -------------------------------------------------------- */
static volatile uint8_t *e1000_mmio = NULL;
static uint8_t mac_addr[6] = {};
static int nic_ready = 0;
static uint32_t my_ip   = 0;
static uint32_t gw_ip   = 0;
static uint32_t dns_ip  = 0x08080808; /* Google DNS fallback */
static uint8_t  gw_mac[6] = {};

static rx_desc_t rx_descs[NUM_RX_DESC] __attribute__((aligned(16)));
static tx_desc_t tx_descs[NUM_TX_DESC] __attribute__((aligned(16)));
static uint8_t   rx_bufs[NUM_RX_DESC][RX_BUF_SIZE];

static int rx_head = 0;
static int tx_tail = 0;

/* ---- Security packet hook (set by security daemon) ----------------------- */
static void (*pkt_hook)(const uint8_t *, size_t) = NULL;

void nic_set_packet_hook(void (*hook)(const uint8_t *, size_t))
{
    pkt_hook = hook;
}

/* ---- MMIO helpers -------------------------------------------------------- */
static uint32_t nic_read(uint32_t reg)
{
    return *(volatile uint32_t *)(e1000_mmio + reg);
}
static void nic_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(e1000_mmio + reg) = val;
}

/* ---- NIC init ------------------------------------------------------------ */
void nic_init(void)
{
    terminal_puts("[NIC] Initialising Intel e1000\n");

    extern pci_device_t *pci_find_class(uint8_t, uint8_t);
    pci_device_t *nic = pci_find_class(0x02, 0x00); /* Network / Ethernet */
    if (!nic) {
        terminal_puts("[NIC] No e1000 found\n");
        g_kstate.network_ready = 0;
        return;
    }

    /* Map BAR0 */
    uint64_t bar0 = nic->bar[0] & ~0xFULL;

    e1000_mmio = (volatile uint8_t *)vmm_map_range(bar0, 0x20000,
                    VMM_PROT_RW | VMM_PROT_NOCACHE);
    if (!e1000_mmio) {
        terminal_puts("[NIC] e1000 MMIO mapping failed; skipping\n");
        g_kstate.network_ready = 0;
        return;
    }
    terminal_printf("[NIC] e1000 MMIO at 0x%llx\n", bar0);

    /* Read MAC from RAL/RAH */
    uint32_t ral = nic_read(E1000_RAL);
    uint32_t rah = nic_read(E1000_RAH);
    mac_addr[0] = (ral      ) & 0xFF;
    mac_addr[1] = (ral >>  8) & 0xFF;
    mac_addr[2] = (ral >> 16) & 0xFF;
    mac_addr[3] = (ral >> 24) & 0xFF;
    mac_addr[4] = (rah      ) & 0xFF;
    mac_addr[5] = (rah >>  8) & 0xFF;
    terminal_printf("[NIC] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac_addr[0], mac_addr[1], mac_addr[2],
        mac_addr[3], mac_addr[4], mac_addr[5]);

    /* Set up RX descriptors */
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_descs[i].addr   = (uint64_t)rx_bufs[i];
        rx_descs[i].status = 0;
    }
    nic_write(E1000_RDBAL, (uint32_t)((uint64_t)rx_descs & 0xFFFFFFFF));
    nic_write(E1000_RDBAH, (uint32_t)((uint64_t)rx_descs >> 32));
    nic_write(E1000_RDLEN, NUM_RX_DESC * sizeof(rx_desc_t));
    nic_write(E1000_RDH, 0);
    nic_write(E1000_RDT, NUM_RX_DESC - 1);
    nic_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM);

    /* Set up TX descriptors */
    for (int i = 0; i < NUM_TX_DESC; i++)
        tx_descs[i].status = 0xFF; /* mark as done */
    nic_write(E1000_TDBAL, (uint32_t)((uint64_t)tx_descs & 0xFFFFFFFF));
    nic_write(E1000_TDBAH, (uint32_t)((uint64_t)tx_descs >> 32));
    nic_write(E1000_TDLEN, NUM_TX_DESC * sizeof(tx_desc_t));
    nic_write(E1000_TDH, 0);
    nic_write(E1000_TDT, 0);
    nic_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP);

    nic_ready = 1;
    g_kstate.network_ready = 1;
    terminal_puts("[NIC] e1000 ready (RX/TX rings configured; DHCP is stubbed)\n");
}

int nic_is_ready(void) { return nic_ready; }

int nic_self_test(void)
{
    if (!nic_ready) {
        terminal_puts("[NIC] Self-test failed: NIC not ready\n");
        return -1;
    }

    uint32_t status = nic_read(E1000_STATUS);
    terminal_printf("[NIC] Self-test: STATUS=0x%x MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
        status,
        mac_addr[0], mac_addr[1], mac_addr[2],
        mac_addr[3], mac_addr[4], mac_addr[5]);
    terminal_printf("[NIC] IPv4 placeholders: ip=0x%x gw=0x%x dns=0x%x gwmac=%02X:%02X:%02X:%02X:%02X:%02X\n",
        my_ip, gw_ip, dns_ip,
        gw_mac[0], gw_mac[1], gw_mac[2],
        gw_mac[3], gw_mac[4], gw_mac[5]);
    return 0;
}

/* ---- Send a raw Ethernet frame ------------------------------------------ */
int nic_send(const uint8_t *frame, size_t len)
{
    if (!e1000_mmio) return -1;
    tx_descs[tx_tail].addr   = (uint64_t)frame;
    tx_descs[tx_tail].length = (uint16_t)len;
    tx_descs[tx_tail].cmd    = 0x0B; /* EOP | IFCS | RS */
    tx_descs[tx_tail].status = 0;
    tx_tail = (tx_tail + 1) % NUM_TX_DESC;
    nic_write(E1000_TDT, (uint32_t)tx_tail);
    /* Poll for completion */
    while (!(tx_descs[(tx_tail - 1 + NUM_TX_DESC) % NUM_TX_DESC].status & 0xFF))
        __asm__ volatile ("pause");
    return 0;
}

/* ---- Poll received packets (called from IRQ or main loop) --------------- */
void nic_poll(void)
{
    while (rx_descs[rx_head].status & 0x01) { /* DD bit set = packet ready */
        uint8_t *pkt = rx_bufs[rx_head];
        size_t   len = rx_descs[rx_head].length;

        /* Pass to security hook first */
        if (pkt_hook && len >= 20)
            pkt_hook(pkt, len);

        /* Process packet (ARP / IP dispatch) */
        net_receive(pkt, len);

        /* Return descriptor to NIC */
        rx_descs[rx_head].status = 0;
        nic_write(E1000_RDT, (uint32_t)rx_head);
        rx_head = (rx_head + 1) % NUM_RX_DESC;
    }
}

/* ---- Minimal network stack ---------------------------------------------- */

/* Ethernet header */
typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype; /* 0x0800 = IPv4, 0x0806 = ARP */
} __attribute__((packed)) eth_hdr_t;

/* IPv4 header */
typedef struct {
    uint8_t  ver_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;  /* 6=TCP, 17=UDP, 1=ICMP */
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) ip_hdr_t;

/* UDP header */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

static uint16_t htons16(uint16_t x) { return (x >> 8) | (x << 8); }
static uint32_t htons32(uint32_t x) {
    return ((x & 0xFF) << 24) | (((x >> 8) & 0xFF) << 16) |
           (((x >> 16) & 0xFF) << 8) | (x >> 24);
}

void net_receive(const uint8_t *frame, size_t len)
{
    if (len < sizeof(eth_hdr_t)) return;
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    uint16_t et = htons16(eth->ethertype);

    if (et == 0x0806) {
        /* ARP — respond if it's for our IP */
        /* TODO: arp_handle() */
    } else if (et == 0x0800) {
        const ip_hdr_t *ip = (const ip_hdr_t *)(frame + sizeof(eth_hdr_t));
        if (ip->dst_ip != htons32(my_ip)) return;

        if (ip->protocol == 17) {
            /* UDP */
            const udp_hdr_t *udp = (const udp_hdr_t *)
                ((const uint8_t *)ip + ((ip->ver_ihl & 0x0F) << 2));
            uint16_t dst = htons16(udp->dst_port);
            if (dst == 53) {
                /* DNS response — parse and return hostname */
                /* dns_handle_response(...) */
            } else if (dst == 68) {
                /* DHCP offer */
                /* dhcp_handle_offer(...) */
            }
        }
    }
}

/* ---- Simple DNS query (blocks until response or timeout) ---------------- */
int dns_resolve(const char *hostname, uint32_t *out_ip)
{
    /* Build DNS query packet */
    static uint8_t dns_buf[512];
    /* DNS header */
    dns_buf[0] = 0xAB; dns_buf[1] = 0xCD; /* transaction ID */
    dns_buf[2] = 0x01; dns_buf[3] = 0x00; /* flags: standard query */
    dns_buf[4] = 0x00; dns_buf[5] = 0x01; /* 1 question */
    dns_buf[6] = 0x00; dns_buf[7] = 0x00; /* 0 answers */
    dns_buf[8] = 0x00; dns_buf[9] = 0x00;
    dns_buf[10]= 0x00; dns_buf[11]= 0x00;

    /* Encode hostname in DNS wire format */
    int pos = 12;
    const char *p = hostname;
    while (*p) {
        const char *dot = strchr(p, '.');
        int label_len   = dot ? (int)(dot - p) : (int)strlen(p);
        dns_buf[pos++]  = (uint8_t)label_len;
        memcpy(dns_buf + pos, p, (size_t)label_len);
        pos += label_len;
        if (!dot) break;
        p = dot + 1;
    }
    dns_buf[pos++] = 0;    /* root label */
    dns_buf[pos++] = 0x00; dns_buf[pos++] = 0x01; /* QTYPE A */
    dns_buf[pos++] = 0x00; dns_buf[pos++] = 0x01; /* QCLASS IN */

    /* TODO: send via UDP socket and await response */
    (void)out_ip;
    terminal_printf("[DNS] Query: %s (stub)\n", hostname);
    return -1; /* stub */
}

/* ---- Minimal HTTP GET for model downloads -------------------------------- */
int http_get(const char *url, void *buf, size_t max)
{
    terminal_printf("[HTTP] GET %s (stub)\n", url);
    (void)buf; (void)max;
    /* Real: DNS resolve → TCP connect → send GET → recv response body */
    return -1;
}

/* ---- Network anomaly scan (called by security daemon tool call) ---------- */
void security_net_scan(const char *cidr)
{
    terminal_printf("[SEC] Network scan: %s (stub)\n", cidr);
    /* Real: ARP sweep over CIDR range, flag unexpected devices */
}
