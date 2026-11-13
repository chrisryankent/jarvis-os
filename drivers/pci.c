/*
 * =============================================================================
 * JARVIS AI OS — PCI Bus Driver  (drivers/pci.c)
 * Language: C17
 *
 * Enumerates PCI devices on boot. Each device record is stored in
 * g_kstate.pci_devices[] and used by specialised drivers (AHCI, NIC, GPU, USB).
 * The AI core can query the device list via the system_info tool call.
 * =============================================================================
 */

#include "../kernel/kernel.h"

/* ---- PCI config space I/O ports ----------------------------------------- */
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* ---- PCI header fields --------------------------------------------------- */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_CLASS_CODE      0x0B
#define PCI_SUBCLASS        0x0A
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_INTERRUPT_LINE  0x3C

/* ---- Class codes --------------------------------------------------------- */
#define PCI_CLASS_STORAGE   0x01   /* AHCI disk controller */
#define PCI_CLASS_NETWORK   0x02   /* NIC                  */
#define PCI_CLASS_DISPLAY   0x03   /* GPU                  */
#define PCI_CLASS_MULTIMEDIA 0x04  /* Audio                */
#define PCI_CLASS_USB       0x0C   /* USB host controller  */

#define MAX_PCI_FULL 256
static pci_device_t pci_devices_full[MAX_PCI_FULL];
static int          pci_count_full = 0;

/* ---- Low-level PCI config read ------------------------------------------ */
static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)fn   <<  8)
                  | (reg & 0xFC);
    __asm__ volatile ("outl %0, %1" :: "a"(addr),   "Nd"((uint16_t)PCI_CONFIG_ADDRESS));
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"((uint16_t)PCI_CONFIG_DATA));
    return val;
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    return (uint16_t)(pci_read32(bus, dev, fn, reg & 0xFC) >> ((reg & 2) * 8));
}

static uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    return (uint8_t)(pci_read32(bus, dev, fn, reg & 0xFC) >> ((reg & 3) * 8));
}

/* ---- Enable bus mastering (needed for DMA devices) ----------------------- */
static void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (PCI_COMMAND & 0xFC);
    __asm__ volatile ("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)PCI_CONFIG_ADDRESS));
    uint32_t cmd;
    __asm__ volatile ("inl %1, %0" : "=a"(cmd) : "Nd"((uint16_t)PCI_CONFIG_DATA));
    cmd |= (1 << 2); /* Bus Master Enable */
    cmd |= (1 << 1); /* Memory Space Enable */
    cmd |= (1 << 0); /* I/O Space Enable */
    __asm__ volatile ("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)PCI_CONFIG_ADDRESS));
    __asm__ volatile ("outl %0, %1" :: "a"(cmd),  "Nd"((uint16_t)PCI_CONFIG_DATA));
}

/* ---- Probe a single function --------------------------------------------- */
static void pci_probe(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint16_t vendor = pci_read16(bus, dev, fn, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) return; /* device not present */

    if (pci_count_full >= MAX_PCI_FULL) return;
    pci_device_t *d = &pci_devices_full[pci_count_full++];

    d->bus       = bus;
    d->dev       = dev;
    d->fn        = fn;
    d->vendor_id = vendor;
    d->device_id = pci_read16(bus, dev, fn, PCI_DEVICE_ID);
    d->class_code= pci_read8 (bus, dev, fn, PCI_CLASS_CODE);
    d->subclass  = pci_read8 (bus, dev, fn, PCI_SUBCLASS);
    d->irq       = pci_read8 (bus, dev, fn, PCI_INTERRUPT_LINE);
    d->prog_if   = pci_read8 (bus, dev, fn, 0x09);

    /* Read BARs */
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read32(bus, dev, fn, (uint8_t)(PCI_BAR0 + i * 4));

    /* Enable bus mastering for DMA devices */
    if (d->class_code == PCI_CLASS_STORAGE  ||
        d->class_code == PCI_CLASS_NETWORK  ||
        d->class_code == PCI_CLASS_DISPLAY  ||
        d->class_code == PCI_CLASS_MULTIMEDIA ||
        d->class_code == PCI_CLASS_USB) {
        pci_enable_bus_master(bus, dev, fn);
    }

    terminal_printf("[PCI] %02x:%02x.%x  %04x:%04x  class=%02x/%02x  irq=%d\n",
        bus, dev, fn,
        d->vendor_id, d->device_id,
        d->class_code, d->subclass,
        d->irq);

    /* Store simplified record in global kernel state */
    if (g_kstate.pci_count < MAX_PCI_DEVS) {
        int idx = g_kstate.pci_count++;
        g_kstate.pci_devices[idx][0] = bus;
        g_kstate.pci_devices[idx][1] = dev;
        g_kstate.pci_devices[idx][2] = fn;
    }
}

/* ---- Full bus scan ------------------------------------------------------- */
void pci_enumerate(void)
{
    terminal_puts("[PCI] Scanning bus...\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint16_t vendor = pci_read16((uint8_t)bus, dev, 0, PCI_VENDOR_ID);
            if (vendor == 0xFFFF) continue;

            uint8_t hdr = pci_read8((uint8_t)bus, dev, 0, PCI_HEADER_TYPE);
            int max_fn  = (hdr & 0x80) ? 8 : 1; /* multi-function check */
            for (int fn = 0; fn < max_fn; fn++)
                pci_probe((uint8_t)bus, dev, (uint8_t)fn);
        }
    }
    terminal_printf("[PCI] Found %d device(s)\n", pci_count_full);
}

/* ---- Helper: find device by class --------------------------------------- */
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    for (int i = 0; i < pci_count_full; i++) {
        if (pci_devices_full[i].class_code == class_code &&
            (subclass == 0xFF || pci_devices_full[i].subclass == subclass))
            return &pci_devices_full[i];
    }
    return NULL;
}

pci_device_t *pci_get(int index)
{
    return (index >= 0 && index < pci_count_full) ? &pci_devices_full[index] : NULL;
}
