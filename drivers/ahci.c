/*
 * =============================================================================
 * JARVIS AI OS — AHCI SATA disk driver (drivers/ahci.c)
 * Language: C17
 *
 * Finds the AHCI controller on the PCI bus, brings up each port that has a
 * SATA disk attached, identifies the disk, and registers it with the block
 * layer (kernel/block.c) as sd0, sd1, ...
 *
 * Transfers use DMA: the disk controller copies sectors directly to and from
 * memory. Each port owns a 64 KB DMA buffer in low physical memory; requests
 * are split into chunks of that size and copied through it, so callers may
 * pass any kernel pointer. Commands are polled to completion (no interrupt).
 *
 * Follows the Intel AHCI 1.3.1 specification.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include <string.h>

#define AHCI_MAX_DISKS   4
#define AHCI_SIG_SATA    0x00000101
#define DMA_BYTES        65536
#define DMA_SECTORS      (DMA_BYTES / 512)

#define PORT_CMD_ST   (1u << 0)
#define PORT_CMD_FRE  (1u << 4)
#define PORT_CMD_FR   (1u << 14)
#define PORT_CMD_CR   (1u << 15)
#define PORT_IS_TFES  (1u << 30)

#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA
#define ATA_IDENTIFY       0xEC

typedef struct {
    uint32_t cap, ghc, is, pi, vs, ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
} hba_mem_t;

typedef struct {
    uint32_t clb, clbu, fb, fbu, is, ie, cmd, rsv0, tfd, sig, ssts, sctl, serr,
             sact, ci, sntf, fbs;
} hba_port_t;

typedef struct {
    uint16_t flags;         /* bits 0-4: FIS length in dwords; bit 6: write */
    uint16_t prdtl;         /* number of PRDT entries */
    uint32_t prdbc;         /* bytes transferred */
    uint32_t ctba, ctbau;   /* command table address */
    uint32_t rsv[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    struct { uint32_t dba, dbau, rsv, dbc; } prdt[1];
} __attribute__((packed)) hba_cmd_table_t;

typedef struct {
    volatile hba_port_t *port;
    hba_cmd_header_t    *cmd_list;
    hba_cmd_table_t     *cmd_table;
    uint8_t             *dma;           /* physical == virtual (identity map) */
    uint64_t             sectors;
    int                  port_num;
} ahci_disk_t;

static volatile hba_mem_t *hba = NULL;
static ahci_disk_t g_disks[AHCI_MAX_DISKS];
static int g_disk_count = 0;

/* Wait until (*reg & mask) == want, at most `ms` milliseconds. */
static int wait_reg(volatile uint32_t *reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t deadline = g_kstate.uptime_ticks + ms / 10 + 2;
    for (uint32_t spin = 0; ; spin++) {
        if ((*reg & mask) == want) return 0;
        if (g_kstate.uptime_ticks > deadline || spin > 400000000u) return -1;
        __asm__ volatile ("pause");
    }
}

static void port_stop(volatile hba_port_t *p)
{
    p->cmd &= ~PORT_CMD_ST;
    wait_reg(&p->cmd, PORT_CMD_CR, 0, 500);
    p->cmd &= ~PORT_CMD_FRE;
    wait_reg(&p->cmd, PORT_CMD_FR, 0, 500);
}

static void port_start(volatile hba_port_t *p)
{
    wait_reg(&p->cmd, PORT_CMD_CR, 0, 500);
    p->cmd |= PORT_CMD_FRE;
    p->cmd |= PORT_CMD_ST;
}

/* Run one ATA command through slot 0. bytes = size of the data phase. */
static int ahci_command(ahci_disk_t *d, uint8_t command, uint64_t lba,
                        uint16_t count, uint32_t bytes, int write)
{
    volatile hba_port_t *p = d->port;

    if (wait_reg(&p->tfd, 0x88, 0, 1000) != 0)      /* BSY | DRQ clear */
        return -1;
    p->is = 0xFFFFFFFF;
    p->serr = 0xFFFFFFFF;

    hba_cmd_header_t *h = &d->cmd_list[0];
    memset(h, 0, sizeof(*h));
    h->flags = 5 | (write ? (1u << 6) : 0);
    h->prdtl = bytes ? 1 : 0;
    h->ctba = (uint32_t)(uintptr_t)d->cmd_table;

    hba_cmd_table_t *t = d->cmd_table;
    memset(t, 0, sizeof(*t));
    if (bytes) {
        t->prdt[0].dba = (uint32_t)(uintptr_t)d->dma;
        t->prdt[0].dbc = bytes - 1;
    }
    uint8_t *f = t->cfis;                           /* register host-to-device FIS */
    f[0] = 0x27;
    f[1] = 0x80;                                    /* this is a command */
    f[2] = command;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 1 << 6;                                  /* LBA mode */
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count;
    f[13] = (uint8_t)(count >> 8);

    p->ci = 1;
    if (wait_reg(&p->ci, 1, 0, 5000) != 0 || (p->is & PORT_IS_TFES) || (p->tfd & 0x01)) {
        terminal_printf("[AHCI] sd%d: command 0x%x failed at LBA %llu (tfd=0x%x)\n",
            (int)(d - g_disks), command, (unsigned long long)lba, p->tfd);
        return -1;
    }
    return 0;
}

static int ahci_block_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    ahci_disk_t *d = (ahci_disk_t *)ctx;
    uint8_t *out = (uint8_t *)buf;

    if (lba + count > d->sectors) return -1;
    while (count) {
        uint32_t n = count > DMA_SECTORS ? DMA_SECTORS : count;
        if (ahci_command(d, ATA_READ_DMA_EXT, lba, (uint16_t)n, n * 512, 0) != 0)
            return -1;
        memcpy(out, d->dma, n * 512);
        out += n * 512; lba += n; count -= n;
    }
    return 0;
}

static int ahci_block_write(void *ctx, uint64_t lba, uint32_t count, const void *buf)
{
    ahci_disk_t *d = (ahci_disk_t *)ctx;
    const uint8_t *in = (const uint8_t *)buf;

    if (lba + count > d->sectors) return -1;
    while (count) {
        uint32_t n = count > DMA_SECTORS ? DMA_SECTORS : count;
        memcpy(d->dma, in, n * 512);
        if (ahci_command(d, ATA_WRITE_DMA_EXT, lba, (uint16_t)n, n * 512, 1) != 0)
            return -1;
        in += n * 512; lba += n; count -= n;
    }
    return 0;
}

static int ahci_block_flush(void *ctx)
{
    return ahci_command((ahci_disk_t *)ctx, ATA_FLUSH_EXT, 0, 0, 0, 0);
}

static void port_setup(volatile hba_port_t *p, int port_num)
{
    if (g_disk_count >= AHCI_MAX_DISKS) return;
    ahci_disk_t *d = &g_disks[g_disk_count];

    /* One page each for the command list, received-FIS area and command
     * table, plus the DMA buffer. All must be below 4 GiB: this driver
     * programs 32-bit addresses and reaches them through the identity map. */
    uint64_t mem = pmm_alloc_contiguous(3 + DMA_BYTES / 4096);
    if (!mem || mem + 0x20000 >= 0x100000000ULL) {
        terminal_printf("[AHCI] Port %d: no DMA memory\n", port_num);
        return;
    }
    memset((void *)mem, 0, 3 * 4096);

    port_stop(p);
    p->clb = (uint32_t)mem;            p->clbu = 0;
    p->fb = (uint32_t)(mem + 4096);    p->fbu = 0;
    p->serr = 0xFFFFFFFF;
    p->is = 0xFFFFFFFF;
    p->ie = 0;                          /* polled */
    port_start(p);

    d->port = p;
    d->port_num = port_num;
    d->cmd_list = (hba_cmd_header_t *)mem;
    d->cmd_table = (hba_cmd_table_t *)(mem + 2 * 4096);
    d->dma = (uint8_t *)(mem + 3 * 4096);
    d->sectors = 0;

    if (ahci_command(d, ATA_IDENTIFY, 0, 0, 512, 0) != 0) {
        terminal_printf("[AHCI] Port %d: IDENTIFY failed\n", port_num);
        return;
    }
    const uint16_t *id = (const uint16_t *)d->dma;
    d->sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                 ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    if (d->sectors == 0)                            /* no 48-bit LBA */
        d->sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);

    char model[41];
    for (int i = 0; i < 20; i++) {                  /* bytes are swapped per word */
        model[i * 2] = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;

    char name[8] = "sd0";
    name[2] = (char)('0' + g_disk_count);
    terminal_printf("[AHCI] %s: %s, %llu MB (port %d)\n", name, model,
        (unsigned long long)(d->sectors / 2048), port_num);

    block_register(name, model, d->sectors, ahci_block_read, ahci_block_write,
                   ahci_block_flush, d);
    g_disk_count++;
}

void ahci_init(void)
{
    terminal_puts("[AHCI] Initialising SATA controller\n");

    pci_device_t *dev = pci_find_class(0x01, 0x06);
    if (!dev) {
        terminal_puts("[AHCI] No AHCI controller found\n");
        return;
    }

    uint64_t abar = dev->bar[5] & ~0xFULL;
    if (!abar) {
        terminal_puts("[AHCI] Controller has no memory BAR\n");
        return;
    }
    hba = (volatile hba_mem_t *)vmm_map_range(abar, 0x1100, VMM_PROT_RW | VMM_PROT_NOCACHE);

    hba->ghc |= (1u << 31);                         /* AHCI mode */
    hba->ghc &= ~(1u << 1);                         /* no interrupts: polled */
    terminal_printf("[AHCI] HBA at 0x%llx, version %x.%x\n", abar,
        hba->vs >> 16, hba->vs & 0xFFFF);

    uint32_t pi = hba->pi;
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        volatile hba_port_t *p = (volatile hba_port_t *)((volatile uint8_t *)hba + 0x100 + i * 0x80);
        uint32_t ssts = p->ssts;
        if ((ssts & 0x0F) != 3 || ((ssts >> 8) & 0x0F) != 1)
            continue;                               /* nothing attached / asleep */
        if (p->sig != AHCI_SIG_SATA)
            continue;                               /* optical drive etc. */
        port_setup(p, i);
    }
    terminal_printf("[AHCI] %d SATA disk(s) ready\n", g_disk_count);
}
