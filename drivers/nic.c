/*
 * =============================================================================
 * JARVIS AI OS — Intel e1000 Ethernet driver (drivers/nic.c)
 * Language: C17
 *
 * Sends and receives raw Ethernet frames. Everything above that (ARP, IP,
 * DHCP, UDP, TCP, DNS) lives in kernel/net.c.
 *
 * The card reads and writes memory by physical address, so the descriptor
 * rings and packet buffers are allocated from low physical memory (reached
 * through the identity map) instead of living in the kernel image, whose
 * virtual addresses mean nothing to the card.
 *
 * The card is polled by the network process; its interrupts stay masked.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include <string.h>

#define E1000_CTRL    0x0000
#define E1000_STATUS  0x0008
#define E1000_IMC     0x00D8
#define E1000_RCTL    0x0100
#define E1000_TCTL    0x0400
#define E1000_TIPG    0x0410
#define E1000_RDBAL   0x2800
#define E1000_RDBAH   0x2804
#define E1000_RDLEN   0x2808
#define E1000_RDH     0x2810
#define E1000_RDT     0x2818
#define E1000_TDBAL   0x3800
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_MTA     0x5200
#define E1000_RAL     0x5400
#define E1000_RAH     0x5404

#define NUM_DESC   32
#define BUF_SIZE   2048

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

static volatile uint8_t *e1000_mmio = NULL;
static uint8_t    mac_addr[6];
static int        nic_ready = 0;
static rx_desc_t *rx_descs;
static tx_desc_t *tx_descs;
static uint8_t   *rx_bufs, *tx_bufs;
static int        rx_head = 0, tx_tail = 0;
static uint64_t   g_rx_frames = 0, g_tx_frames = 0;

static uint32_t nic_read(uint32_t reg)            { return *(volatile uint32_t *)(e1000_mmio + reg); }
static void     nic_write(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(e1000_mmio + reg) = v; }

void nic_init(void)
{
    terminal_puts("[NIC] Initialising Intel e1000\n");
    g_kstate.network_ready = 0;

    pci_device_t *nic = pci_find_class(0x02, 0x00);
    if (!nic || nic->vendor_id != 0x8086) {
        terminal_puts("[NIC] No supported Ethernet adapter found\n");
        return;
    }

    uint64_t bar0 = nic->bar[0] & ~0xFULL;
    e1000_mmio = (volatile uint8_t *)vmm_map_range(bar0, 0x20000, VMM_PROT_RW | VMM_PROT_NOCACHE);

    /* rings: one page each; buffers: 32 x 2 KB each way */
    uint64_t mem = pmm_alloc_contiguous(2 + 2 * (NUM_DESC * BUF_SIZE / 4096));
    if (!mem || mem >= 0xFFF00000ULL) {
        terminal_puts("[NIC] No DMA memory\n");
        return;
    }
    memset((void *)mem, 0, 2 * 4096);
    rx_descs = (rx_desc_t *)mem;
    tx_descs = (tx_desc_t *)(mem + 4096);
    rx_bufs = (uint8_t *)(mem + 2 * 4096);
    tx_bufs = rx_bufs + NUM_DESC * BUF_SIZE;

    uint32_t ral = nic_read(E1000_RAL), rah = nic_read(E1000_RAH);
    mac_addr[0] = (uint8_t)ral;         mac_addr[1] = (uint8_t)(ral >> 8);
    mac_addr[2] = (uint8_t)(ral >> 16); mac_addr[3] = (uint8_t)(ral >> 24);
    mac_addr[4] = (uint8_t)rah;         mac_addr[5] = (uint8_t)(rah >> 8);
    terminal_printf("[NIC] MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac_addr[0], mac_addr[1],
        mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

    nic_write(E1000_IMC, 0xFFFFFFFF);                           /* polled: no interrupts */
    nic_write(E1000_CTRL, nic_read(E1000_CTRL) | (1u << 6) | (1u << 5));    /* link up, auto speed */
    for (int i = 0; i < 128; i++)
        nic_write(E1000_MTA + i * 4, 0);

    for (int i = 0; i < NUM_DESC; i++) {
        rx_descs[i].addr = (uint64_t)(uintptr_t)(rx_bufs + i * BUF_SIZE);
        tx_descs[i].addr = (uint64_t)(uintptr_t)(tx_bufs + i * BUF_SIZE);
        tx_descs[i].status = 1;                                 /* free */
    }
    nic_write(E1000_RDBAL, (uint32_t)(uintptr_t)rx_descs);
    nic_write(E1000_RDBAH, 0);
    nic_write(E1000_RDLEN, NUM_DESC * sizeof(rx_desc_t));
    nic_write(E1000_RDH, 0);
    nic_write(E1000_RDT, NUM_DESC - 1);
    nic_write(E1000_RCTL, (1u << 1) | (1u << 15) | (1u << 26)); /* enable, broadcast, strip CRC */

    nic_write(E1000_TDBAL, (uint32_t)(uintptr_t)tx_descs);
    nic_write(E1000_TDBAH, 0);
    nic_write(E1000_TDLEN, NUM_DESC * sizeof(tx_desc_t));
    nic_write(E1000_TDH, 0);
    nic_write(E1000_TDT, 0);
    nic_write(E1000_TIPG, 0x0060200A);
    nic_write(E1000_TCTL, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12));

    nic_ready = 1;
    g_kstate.network_ready = 1;
    terminal_puts("[NIC] e1000 ready\n");
}

int nic_is_ready(void) { return nic_ready; }

int nic_link_up(void)
{
    return nic_ready && (nic_read(E1000_STATUS) & 2) != 0;
}

void nic_get_mac(uint8_t out[6])
{
    memcpy(out, mac_addr, 6);
}

void nic_get_counters(uint64_t *rx, uint64_t *tx)
{
    if (rx) *rx = g_rx_frames;
    if (tx) *tx = g_tx_frames;
}

int nic_self_test(void)
{
    if (!nic_ready) return -1;
    terminal_printf("[NIC] Link %s, %llu frames received, %llu sent\n",
        nic_link_up() ? "up" : "down",
        (unsigned long long)g_rx_frames, (unsigned long long)g_tx_frames);
    return 0;
}

/* Send one Ethernet frame (without CRC; the card appends it). */
int nic_send(const uint8_t *frame, size_t len)
{
    if (!nic_ready || len > 1514) return -1;

    tx_desc_t *d = &tx_descs[tx_tail];
    for (uint32_t spin = 0; !(d->status & 1); spin++) {         /* ring full: wait */
        if (spin > 20000000u) return -1;
        __asm__ volatile ("pause");
    }
    memcpy(tx_bufs + tx_tail * BUF_SIZE, frame, len);
    if (len < 60) {                                             /* minimum frame size */
        memset(tx_bufs + tx_tail * BUF_SIZE + len, 0, 60 - len);
        len = 60;
    }
    d->length = (uint16_t)len;
    d->cmd = 0x0B;                                              /* end of packet, add CRC, report */
    d->status = 0;
    tx_tail = (tx_tail + 1) % NUM_DESC;
    nic_write(E1000_TDT, (uint32_t)tx_tail);
    g_tx_frames++;
    return 0;
}

/* Hand every frame that has arrived to the network stack. */
void nic_poll(void)
{
    if (!nic_ready) return;
    while (rx_descs[rx_head].status & 1) {
        size_t len = rx_descs[rx_head].length;
        if (len >= 14 && len <= BUF_SIZE && !rx_descs[rx_head].errors) {
            g_rx_frames++;
            net_receive(rx_bufs + rx_head * BUF_SIZE, len);
        }
        rx_descs[rx_head].status = 0;
        nic_write(E1000_RDT, (uint32_t)rx_head);
        rx_head = (rx_head + 1) % NUM_DESC;
    }
}
