/*
 * =============================================================================
 * JARVIS AI OS — AHCI Disk Driver  (drivers/ahci.c)
 * Language: C17
 *
 * Targets: AHCI SATA + NVMe PCIe controllers.
 * Primary purpose in JARVIS: load large AI model .gguf/.safetensors files
 * from the NVMe into the model memory pool at boot.
 *
 * AHCI registers follow the Intel AHCI 1.3.1 specification.
 * NVMe path: separate nvme_init() using PCIe BAR0 doorbell registers.
 * =============================================================================
 */

#include "../kernel/kernel.h"

/* ---- AHCI HBA memory-mapped registers ----------------------------------- */
#define HBA_PORT_COUNT      32
#define AHCI_SIG_SATA       0x00000101
#define AHCI_SIG_ATAPI      0xEB140101
#define AHCI_SIG_SEMB       0xC33C0101
#define AHCI_SIG_PM         0x96690101

/* HBA generic host control */
typedef struct {
    uint32_t cap;        /* 0x00 host capabilities          */
    uint32_t ghc;        /* 0x04 global host control        */
    uint32_t is;         /* 0x08 interrupt status           */
    uint32_t pi;         /* 0x0C ports implemented          */
    uint32_t vs;         /* 0x10 version                    */
    uint32_t ccc_ctl;    /* 0x14 command completion coalescing ctl */
    uint32_t ccc_pts;    /* 0x18 CCC ports                  */
    uint32_t em_loc;     /* 0x1C enclosure mgmt location    */
    uint32_t em_ctl;     /* 0x20 enclosure mgmt control     */
    uint32_t cap2;       /* 0x24 extended capabilities      */
    uint32_t bohc;       /* 0x28 BIOS/OS handoff control    */
    uint8_t  rsv[0xA0-0x2C];
    uint8_t  vendor[0x100-0xA0];
    /* Port registers start at offset 0x100, each 0x80 bytes */
} __attribute__((packed)) hba_mem_t;

typedef struct {
    uint32_t clb;        /* 0x00 command list base addr low  */
    uint32_t clbu;       /* 0x04 command list base addr high */
    uint32_t fb;         /* 0x08 FIS base address low        */
    uint32_t fbu;        /* 0x0C FIS base address high       */
    uint32_t is;         /* 0x10 interrupt status            */
    uint32_t ie;         /* 0x14 interrupt enable            */
    uint32_t cmd;        /* 0x18 command and status          */
    uint32_t rsv0;
    uint32_t tfd;        /* 0x20 task file data              */
    uint32_t sig;        /* 0x24 signature                   */
    uint32_t ssts;       /* 0x28 SATA status (SCR0: SStatus) */
    uint32_t sctl;       /* 0x2C SATA control                */
    uint32_t serr;       /* 0x30 SATA error                  */
    uint32_t sact;       /* 0x34 SATA active                 */
    uint32_t ci;         /* 0x38 command issue               */
    uint32_t sntf;       /* 0x3C SATA notification           */
    uint32_t fbs;        /* 0x40 FIS-based switch control    */
    uint32_t rsv1[11];
    uint32_t vendor[4];
} __attribute__((packed)) hba_port_t;

/* ---- Driver state -------------------------------------------------------- */
static volatile hba_mem_t *hba     = NULL;
static volatile hba_port_t *ports[HBA_PORT_COUNT];
static int port_count = 0;

/* ---- Detect port type ---------------------------------------------------- */
static int port_type(volatile hba_port_t *port)
{
    uint32_t ssts = port->ssts;
    uint8_t  ipm  = (ssts >> 8) & 0x0F;
    uint8_t  det  = ssts & 0x0F;
    if (det != 3 || ipm != 1) return 0; /* not active */
    switch (port->sig) {
    case AHCI_SIG_SATA:  return 1;
    case AHCI_SIG_ATAPI: return 2;
    default:             return 0;
    }
}

/* ---- ATA IDENTIFY DEVICE (to get disk geometry and serial) --------------- */
static void port_identify(volatile hba_port_t *port, int port_num)
{
    (void)port; /* stub — full implementation requires command list setup */
    terminal_printf("[AHCI] Port %d: SATA disk detected\n", port_num);
}

/* ---- AHCI init ----------------------------------------------------------- */
void ahci_init(void)
{
    terminal_puts("[AHCI] Initialising SATA controller\n");

    /* Find AHCI controller on PCI (class 0x01, subclass 0x06) */
    extern pci_device_t *pci_find_class(uint8_t, uint8_t);
    pci_device_t *ahci_dev = pci_find_class(0x01, 0x06);
    if (!ahci_dev) {
        terminal_puts("[AHCI] No AHCI controller found\n");
        return;
    }

    /* BAR5 = AHCI base memory register */
    uint64_t abar = ahci_dev->bar[5] & ~0xFULL;
    if (abar >= 0x40000000ULL) {
        terminal_printf("[AHCI] HBA at 0x%llx is not mapped yet; skipping MMIO init\n", abar);
        return;
    }

    hba = (volatile hba_mem_t *)vmm_map_range(
        abar, sizeof(hba_mem_t) + HBA_PORT_COUNT * 0x80,
        VMM_PROT_RW | VMM_PROT_NOCACHE);
    if (!hba) {
        terminal_puts("[AHCI] HBA mapping failed; skipping MMIO init\n");
        return;
    }

    terminal_printf("[AHCI] HBA at 0x%llx, version 0x%08X\n",
        abar, hba->vs);

    /* Enable AHCI mode (GHC.AE bit) */
    hba->ghc |= (1u << 31);

    /* Reset HBA */
    hba->ghc |= 1; /* GHC.HR — HBA reset */
    while (hba->ghc & 1); /* wait for reset to complete */
    hba->ghc |= (1u << 31); /* re-enable AHCI */

    /* Scan implemented ports */
    uint32_t pi = hba->pi;
    for (int i = 0; i < HBA_PORT_COUNT; i++) {
        if (!(pi & (1u << i))) continue;

        /* Port register offset: 0x100 + port * 0x80 */
        volatile uint8_t *base = (volatile uint8_t *)hba;
        ports[port_count] = (volatile hba_port_t *)(base + 0x100 + i * 0x80);

        if (port_type(ports[port_count])) {
            port_identify(ports[port_count], i);
            port_count++;
        }
    }

    terminal_printf("[AHCI] %d SATA disk(s) found\n", port_count);
}

/* ---- DMA read: read sectors from disk ------------------------------------ */
/*
 * Reads `count` 512-byte sectors starting at LBA `lba` from port `port_idx`
 * into the physical buffer at `phys_buf`.
 *
 * Real implementation requires:
 *   1. Build a Command Table with a Physical Region Descriptor Table (PRDT)
 *      pointing to phys_buf.
 *   2. Fill a Command Header in the Command List.
 *   3. Set HBA_PORT.ci bit to issue the command.
 *   4. Poll HBA_PORT.ci for completion (or use interrupt).
 *   5. Check for errors in HBA_PORT.is.
 *
 * This is where model .gguf files are read from NVMe into model_pool at boot.
 */
int ahci_read(int port_idx, uint64_t lba, uint32_t count, void *buf)
{
    if (port_idx >= port_count) return -1;
    (void)lba; (void)count; (void)buf;
    /*
     * Full DMA read implementation:
     *   cmd_table_t *ct = alloc_cmd_table();
     *   ct->prdt[0].dba  = (uint32_t)((uint64_t)buf & 0xFFFFFFFF);
     *   ct->prdt[0].dbau = (uint32_t)((uint64_t)buf >> 32);
     *   ct->prdt[0].dbc  = count * 512 - 1;
     *   ct->cfis[0] = 0x27; (H2D FIS)
     *   ct->cfis[1] = 0x80; (C bit)
     *   ct->cfis[2] = 0x25; (READ DMA EXT)
     *   ... set LBA bytes in cfis[3..9] ...
     *   port->ci = 1; (issue slot 0)
     *   while (port->ci & 1 && !(port->is & HBA_PxIS_TFES));
     */
    terminal_printf("[AHCI] Read LBA 0x%llx count %u (stub)\n", lba, count);
    return 0;
}

/* ---- Load a model file from disk into model pool ------------------------- */
/*
 * This is called at boot for each model in /jarvis/models/.
 * LBA address is read from the JARVIS partition table (a simple JSON at LBA 34).
 * Returns pointer to loaded data in model pool, or NULL on failure.
 */
void *ahci_load_model(const char *model_name)
{
    terminal_printf("[AHCI] Loading model: %s\n", model_name);

    /*
     * Real implementation:
     *   1. Read JARVIS model index from LBA 34 (stored at format time)
     *   2. Find model_name entry → start_lba, size_sectors
     *   3. ahci_read(0, start_lba, size_sectors, model_pool_ptr)
     *   4. Return model_pool_ptr
     *
     * Model index format (JSON at LBA 34):
     * {
     *   "models": [
     *     {"name": "brain/model.gguf", "lba": 2048, "sectors": 4194304},
     *     {"name": "stt/ggml-medium.en.bin", "lba": 4196352, "sectors": 602112}
     *   ]
     * }
     */
    (void)model_name;
    return NULL; /* stub */
}
