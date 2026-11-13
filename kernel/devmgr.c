/*
 * =============================================================================
 * JARVIS AI OS — Device manager (kernel/devmgr.c)
 * Language: C17
 *
 * One table of every device in the machine: what it is, where it is attached,
 * which driver runs it, whether it works, how many interrupts it has raised,
 * and which process (if any) has claimed it for exclusive use.
 *
 *   status tracking   devmgr_list()
 *   allocation        devmgr_claim() / devmgr_release(); a process's claims
 *                     are released automatically when it exits
 *   drivers           each PCI device is matched against the driver table
 *                     below; a device nothing matches is listed as
 *                     "No driver" rather than hidden
 *   interrupts        per-line counters maintained by the interrupt
 *                     dispatcher (kernel/platform.c)
 *
 * Drivers are compiled into the kernel and started by device_init(); this
 * module records the outcome. Loading a driver from a file at run time is
 * not supported.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

uint32_t fb_get_width(void);            /* drivers/gpu.c */

#define DEV_MAX 48

typedef struct {
    dev_info_t info;
    int (*working)(void);           /* NULL = always (or never, see state) */
} dev_entry_t;

static dev_entry_t g_devs[DEV_MAX];
static int g_dev_count = 0;

static int net_ok(void)   { return g_kstate.network_ready; }
static int audio_ok(void) { return g_kstate.audio_ready; }
static int video_ok(void) { return fb_get_width() > 0; }
static int disk_ok(void)  { return block_count() > 0; }
static int usb_ok(void)   { return usb_is_ready(); }

/* ---- Driver table: which driver handles which kind of PCI device --------- */
static const struct {
    uint8_t class_code, subclass;   /* subclass 0xFF = any */
    const char *type, *name, *driver;
    int (*working)(void);
    const char *note;
} g_drivers[] = {
    { 0x01, 0x06, "Storage", "SATA controller (AHCI)",   "ahci",        disk_ok,  "" },
    { 0x01, 0x01, "Storage", "IDE controller",           "",            NULL,     "boot CD-ROM only" },
    { 0x01, 0x08, "Storage", "NVMe controller",          "",            NULL,     "" },
    { 0x02, 0x00, "Network", "Ethernet adapter",         "e1000",       net_ok,   "" },
    { 0x02, 0x80, "Network", "Wireless adapter",         "",            NULL,     "" },
    { 0x03, 0xFF, "Display", "Graphics adapter",         "framebuffer", video_ok, "" },
    { 0x04, 0x01, "Audio",   "Audio controller (AC97)",  "ac97",        audio_ok, "" },
    { 0x04, 0x03, "Audio",   "Audio controller (HDA)",   "",            NULL,     "" },
    { 0x0C, 0x03, "USB",     "USB host controller",      "xhci",        usb_ok,   "" },
    { 0x0D, 0x11, "Wireless","Bluetooth controller",     "",            NULL,     "" },
    { 0x06, 0xFF, "System",  "System bridge",            "system",      NULL,     "" },
};

static dev_entry_t *dev_add(const char *name, const char *type, const char *location,
                            const char *driver, int irq, int (*working)(void), const char *note)
{
    if (g_dev_count >= DEV_MAX) return NULL;
    dev_entry_t *d = &g_devs[g_dev_count++];
    memset(d, 0, sizeof(*d));
    strncpy(d->info.name, name, sizeof(d->info.name) - 1);
    strncpy(d->info.type, type, sizeof(d->info.type) - 1);
    strncpy(d->info.location, location, sizeof(d->info.location) - 1);
    strncpy(d->info.driver, driver, sizeof(d->info.driver) - 1);
    strncpy(d->info.note, note ? note : "", sizeof(d->info.note) - 1);
    d->info.irq = irq;
    d->working = working;
    return d;
}

void devmgr_init(void)
{
    char loc[24], name[48];

    g_dev_count = 0;

    /* Fixed platform devices */
    dev_add("System timer (PIT)", "System", "platform", "pit", 0, NULL, "");
    dev_add("PS/2 keyboard", "Input", "PS/2 port 1", "ps2", 1, NULL, "");
    dev_add("PS/2 mouse", "Input", "PS/2 port 2", "ps2", 12, NULL, "");
    dev_add("Serial port COM1", "System", "I/O 0x3F8", "serial", 4, NULL, "kernel log");

    /* PCI devices */
    for (int i = 0; ; i++) {
        pci_device_t *p = pci_get(i);
        if (!p) break;

        int match = -1;
        for (size_t k = 0; k < sizeof(g_drivers) / sizeof(g_drivers[0]); k++) {
            if (g_drivers[k].class_code == p->class_code &&
                (g_drivers[k].subclass == 0xFF || g_drivers[k].subclass == p->subclass)) {
                match = (int)k;
                break;
            }
        }
        snprintf(loc, sizeof(loc), "PCI %u:%u.%u", p->bus, p->dev, p->fn);
        int irq = (p->irq && p->irq < 16) ? p->irq : -1;

        dev_entry_t *d;
        if (match >= 0) {
            d = dev_add(g_drivers[match].name, g_drivers[match].type, loc,
                        g_drivers[match].driver, irq, g_drivers[match].working,
                        g_drivers[match].note);
        } else {
            snprintf(name, sizeof(name), "Unknown device (class %x/%x)", p->class_code, p->subclass);
            d = dev_add(name, "Other", loc, "", irq, NULL, "");
        }
        if (d) {
            d->info.vendor_id = p->vendor_id;
            d->info.device_id = p->device_id;
        }
    }

    /* Devices found on the USB ports */
    for (int i = 0; i < usb_device_total(); i++) {
        char drv[16];
        uint16_t vid, pid;
        int port;
        if (usb_device_info(i, name, sizeof(name), drv, sizeof(drv), &vid, &pid, &port) != 0) continue;
        snprintf(loc, sizeof(loc), "USB port %d", port);
        dev_entry_t *d = dev_add(name, "USB", loc, drv, -1, NULL, "");
        if (d) { d->info.vendor_id = vid; d->info.device_id = pid; }
    }

    /* Disks found by the storage drivers */
    for (int i = 0; i < block_count(); i++) {
        block_info_t b;
        if (block_get_info(i, &b) != 0) continue;
        snprintf(name, sizeof(name), "Disk %s: %s", b.name, b.model);
        snprintf(loc, sizeof(loc), "%u MB", (unsigned)(b.sectors / 2048));
        dev_add(name, "Storage", loc, "block", -1, NULL, "");
    }

    int unbound = 0;
    for (int i = 0; i < g_dev_count; i++)
        if (!g_devs[i].info.driver[0]) unbound++;
    static int logged = 0;
    if (!logged++)
        terminal_printf("[DEV] Device manager: %d device(s), %d without a driver\n",
            g_dev_count, unbound);
}

/* Fills out[] with current device status; returns the number of devices. */
int devmgr_list(dev_info_t *out, int max)
{
    static uint32_t seen_generation;
    if (seen_generation != g_storage_generation) {      /* something was plugged in or out */
        seen_generation = g_storage_generation;
        devmgr_init();
    }
    int n = 0;
    for (int i = 0; i < g_dev_count && n < max; i++) {
        dev_entry_t *d = &g_devs[i];

        if (!d->info.driver[0])       d->info.state = DEV_NO_DRIVER;
        else if (d->working)          d->info.state = d->working() ? DEV_WORKING : DEV_FAILED;
        else                          d->info.state = DEV_WORKING;

        d->info.interrupts = d->info.irq >= 0 ? irq_count(d->info.irq) : 0;
        out[n] = d->info;
        out[n].id = i;
        n++;
    }
    return n;
}

/* ---- Allocation ---------------------------------------------------------- */
/* Give a process exclusive use of a device. 0 = granted, -1 = refused. */
int devmgr_claim(int id, uint32_t pid)
{
    if (id < 0 || id >= g_dev_count || pid == 0) return -1;
    dev_entry_t *d = &g_devs[id];
    if (!d->info.driver[0]) return -1;                  /* nothing to drive it */
    if (d->info.owner_pid && d->info.owner_pid != pid && proc_exists(d->info.owner_pid))
        return -1;                                      /* someone else has it */
    d->info.owner_pid = pid;
    return 0;
}

int devmgr_release(int id, uint32_t pid)
{
    if (id < 0 || id >= g_dev_count || g_devs[id].info.owner_pid != pid) return -1;
    g_devs[id].info.owner_pid = 0;
    return 0;
}

/* Called when a process exits: nothing stays claimed by a dead process. */
void devmgr_release_all(uint32_t pid)
{
    for (int i = 0; i < g_dev_count; i++)
        if (g_devs[i].info.owner_pid == pid)
            g_devs[i].info.owner_pid = 0;
}
