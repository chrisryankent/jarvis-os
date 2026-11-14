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

/* Drivers removed with "Uninstall device" (DEVCTL_* bits); they stay removed
 * until "Scan for hardware changes" or the next start, as on Windows. */
static unsigned g_uninstalled = 0;

/* ---- Event log (Properties > Events) ---------------------------------------- */
#define DEV_EVENTS 96
static struct { uint64_t tick; char dev[48]; char text[72]; } g_events[DEV_EVENTS];
static int g_event_count = 0;

void devmgr_log(const char *dev, const char *text)
{
    int i = g_event_count++ % DEV_EVENTS;
    g_events[i].tick = g_kstate.uptime_ticks;
    snprintf(g_events[i].dev, sizeof(g_events[i].dev), "%s", dev);
    snprintf(g_events[i].text, sizeof(g_events[i].text), "%s", text);
}

/* Events for one device (newest first), or all when id < 0. */
int devmgr_events(int id, char *out, size_t n)
{
    out[0] = 0;
    size_t o = 0;
    int shown = 0;
    int avail = g_event_count < DEV_EVENTS ? g_event_count : DEV_EVENTS;
    for (int k = 0; k < avail; k++) {
        int i = (g_event_count - 1 - k) % DEV_EVENTS;
        if (id >= 0 && (id >= g_dev_count || strcmp(g_events[i].dev, g_devs[id].info.name))) continue;
        uint64_t s = g_events[i].tick / 100;
        int w = snprintf(out + o, n - o, "%02u:%02u:%02u  %s%s%s\n", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60),
                         id < 0 ? g_events[i].dev : "", id < 0 ? ": " : "", g_events[i].text);
        if (w < 0 || o + (size_t)w >= n) break;
        o += (size_t)w;
        shown++;
    }
    return shown;
}

static int net_ok(void)   { return g_kstate.network_ready; }
static int audio_ok(void) { return g_kstate.audio_ready; }
static int video_ok(void) { return fb_get_width() > 0; }
static int disk_ok(void)  { return block_count() > 0; }
static int usb_ok(void)   { return usb_is_ready(); }

/* ---- Vendor names (PCI-SIG / USB-IF IDs) ----------------------------------- */
static const struct { uint16_t id; const char *name; } g_vendors[] = {
    { 0x8086, "Intel" }, { 0x8087, "Intel" }, { 0x1022, "AMD" }, { 0x1002, "AMD/ATI" },
    { 0x10DE, "NVIDIA" }, { 0x10EC, "Realtek" }, { 0x1234, "QEMU (Bochs)" }, { 0x1B36, "Red Hat (QEMU)" },
    { 0x1AF4, "Red Hat (virtio)" }, { 0x15AD, "VMware" }, { 0x80EE, "VirtualBox" }, { 0x14E4, "Broadcom" },
    { 0x168C, "Qualcomm Atheros" }, { 0x17CB, "Qualcomm" }, { 0x1B21, "ASMedia" }, { 0x1912, "Renesas" },
    { 0x0781, "SanDisk" }, { 0x0951, "Kingston" }, { 0x090C, "Silicon Motion" }, { 0x058F, "Alcor Micro" },
    { 0x13FE, "Phison" }, { 0x0BDA, "Realtek" }, { 0x046D, "Logitech" }, { 0x045E, "Microsoft" },
    { 0x05AC, "Apple" }, { 0x04E8, "Samsung" }, { 0x18D1, "Google" }, { 0x2717, "Xiaomi" },
    { 0x0627, "QEMU" }, { 0x46F4, "QEMU" }, { 0x0A12, "Cambridge Silicon Radio" }, { 0x0B05, "ASUS" },
    { 0x1D6B, "Linux Foundation" }, { 0x0930, "Toshiba" }, { 0x154B, "PNY" }, { 0x1F75, "Innostor" },
};

static void vendor_name(uint16_t id, char *out, size_t n)
{
    out[0] = 0;
    for (size_t i = 0; i < sizeof(g_vendors) / sizeof(g_vendors[0]); i++)
        if (g_vendors[i].id == id) { snprintf(out, n, "%s", g_vendors[i].name); return; }
}

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
            d->info.class_code = p->class_code;
            d->info.subclass = p->subclass;
            d->info.prog_if = p->prog_if;
            memcpy(d->info.bar, p->bar, sizeof(d->info.bar));
            vendor_name(p->vendor_id, d->info.vendor, sizeof(d->info.vendor));
            if (!strcmp(d->info.type, "Network") && d->info.driver[0]) d->info.control = DEVCTL_NET;
            if (!strcmp(d->info.type, "Audio") && d->info.driver[0])   d->info.control = DEVCTL_AUDIO;
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
        if (d) {
            d->info.vendor_id = vid; d->info.device_id = pid;
            d->info.control = DEVCTL_USB; d->info.port = port;
            vendor_name(vid, d->info.vendor, sizeof(d->info.vendor));
        }
    }
    int ej[8], ne = usb_ejected_ports(ej, 8);           /* stopped by the user, still plugged in */
    for (int i = 0; i < ne; i++) {
        snprintf(loc, sizeof(loc), "USB port %d", ej[i]);
        dev_entry_t *d = dev_add("USB device (ejected)", "USB", loc, "xhci", -1, NULL, "safe to unplug");
        if (d) { d->info.control = DEVCTL_USB; d->info.port = ej[i]; d->info.state = DEV_DISABLED; }
    }

    /* Disks found by the storage drivers */
    for (int i = 0; i < block_count(); i++) {
        block_info_t b;
        if (block_get_info(i, &b) != 0) continue;
        snprintf(name, sizeof(name), "Disk %s: %s", b.name, b.model);
        snprintf(loc, sizeof(loc), "%u MB", (unsigned)(b.sectors / 2048));
        dev_entry_t *d = dev_add(name, "Storage", loc, "block", -1, NULL, "");
        if (d && !strncmp(b.name, "usb", 3)) { d->info.control = DEVCTL_USB; d->info.port = usb_port_of_block(i); }
    }

    int unbound = 0;
    for (int i = 0; i < g_dev_count; i++) {
        dev_info_t *in = &g_devs[i].info;
        if (in->control && in->control != DEVCTL_USB && (g_uninstalled & (1u << in->control))) {
            in->driver[0] = 0;
            snprintf(in->note, sizeof(in->note), "driver uninstalled");
        }
        if (!in->driver[0]) unbound++;
    }
    static int logged = 0;
    if (!logged++) {
        terminal_printf("[DEV] Device manager: %d device(s), %d without a driver\n",
            g_dev_count, unbound);
        for (int i = 0; i < g_dev_count; i++) {
            char t[72];
            if (g_devs[i].info.driver[0]) snprintf(t, sizeof(t), "Device started (driver %s)", g_devs[i].info.driver);
            else snprintf(t, sizeof(t), "Device found; no driver for it");
            devmgr_log(g_devs[i].info.name, t);
        }
    }
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

        if (d->info.control && d->info.control != DEVCTL_USB && (g_uninstalled & (1u << d->info.control)))
                                      d->info.state = DEV_NO_DRIVER;
        else if ((d->info.control == DEVCTL_NET && !net_adapter_enabled()) ||
            (d->info.control == DEVCTL_AUDIO && !audio_is_enabled()) ||
            (d->info.control == DEVCTL_USB && !strcmp(d->info.note, "safe to unplug")))
                                      d->info.state = DEV_DISABLED;
        else if (!d->info.driver[0])  d->info.state = DEV_NO_DRIVER;
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

/* ---- Enable / disable ---------------------------------------------------- */
/* Only devices whose loss cannot lock the user out: the network adapter,
 * the sound card and USB devices. Returns 0, or -1 with the reason. */
int devmgr_set_enabled(int id, int on, char *why, size_t why_len)
{
    if (id < 0 || id >= g_dev_count) { snprintf(why, why_len, "no such device"); return -1; }
    dev_entry_t *d = &g_devs[id];
    switch (d->info.control) {
    case DEVCTL_NET:
    case DEVCTL_AUDIO:
        if (g_uninstalled & (1u << d->info.control)) { snprintf(why, why_len, "its driver is uninstalled: scan for hardware changes first"); return -1; }
        if (d->info.control == DEVCTL_NET) { net_set_adapter_enabled(on); setting_set_int("dev_net_off", !on); }
        else { audio_set_enabled(on); setting_set_int("dev_audio_off", !on); }
        devmgr_log(d->info.name, on ? "Device enabled" : "Device disabled");
        return 0;
    case DEVCTL_USB: {
        char name[48];
        snprintf(name, sizeof(name), "%s", d->info.name);
        if (on) {
            if (usb_enable_port(d->info.port) != 0) { snprintf(why, why_len, "it is already on"); return -1; }
        } else if (usb_eject_port(d->info.port) != 0) {
            snprintf(why, why_len, "it is no longer connected"); return -1;
        }
        devmgr_log(name, on ? "Device enabled (port powered on)" : "Device stopped (safe to unplug)");
        return 0;
    }
    default:
        snprintf(why, why_len, "%s cannot be turned off: the system needs it", d->info.name);
        return -1;
    }
}

/* ---- Restart / uninstall / scan ---------------------------------------------- */
int nic_restart(void);                      /* drivers/nic.c   */
int audio_restart(void);                    /* drivers/audio.c */

int devmgr_restart(int id, char *why, size_t why_len)
{
    if (id < 0 || id >= g_dev_count) { snprintf(why, why_len, "no such device"); return -1; }
    dev_entry_t *d = &g_devs[id];
    char name[48];
    snprintf(name, sizeof(name), "%s", d->info.name);
    int rc = -1;
    switch (d->info.control) {
    case DEVCTL_NET:
        if (!net_adapter_enabled() || (g_uninstalled & (1u << DEVCTL_NET))) { snprintf(why, why_len, "it is disabled"); return -1; }
        rc = nic_restart();
        if (!rc) net_renew();
        break;
    case DEVCTL_AUDIO:
        if (!audio_is_enabled() || (g_uninstalled & (1u << DEVCTL_AUDIO))) { snprintf(why, why_len, "it is disabled"); return -1; }
        rc = audio_restart();
        if (!rc) audio_set_volume(setting_get_int("mute", 0) ? 0 : setting_get_int("volume", 80));
        break;
    case DEVCTL_USB:
        rc = usb_eject_port(d->info.port);
        if (!rc) rc = usb_enable_port(d->info.port);
        break;
    default:
        snprintf(why, why_len, "%s cannot be restarted while the system runs", name);
        return -1;
    }
    if (rc) snprintf(why, why_len, "the device did not respond");
    devmgr_log(name, rc ? "Restart failed" : "Device restarted");
    return rc;
}

int devmgr_uninstall(int id, char *why, size_t why_len)
{
    if (id < 0 || id >= g_dev_count) { snprintf(why, why_len, "no such device"); return -1; }
    dev_entry_t *d = &g_devs[id];
    char name[48];
    snprintf(name, sizeof(name), "%s", d->info.name);
    switch (d->info.control) {
    case DEVCTL_NET:   net_set_adapter_enabled(0); break;
    case DEVCTL_AUDIO: audio_set_enabled(0); break;
    case DEVCTL_USB:
        if (usb_eject_port(d->info.port) != 0) { snprintf(why, why_len, "it is no longer connected"); return -1; }
        devmgr_log(name, "Device uninstalled (stopped; reconnect it or scan to use it again)");
        return 0;
    default:
        snprintf(why, why_len, "the driver of %s cannot be removed: the system needs it", name);
        return -1;
    }
    g_uninstalled |= 1u << d->info.control;
    d->info.driver[0] = 0;
    snprintf(d->info.note, sizeof(d->info.note), "driver uninstalled");
    devmgr_log(name, "Driver uninstalled");
    return 0;
}

/* Scan for hardware changes: re-detect every device and reinstall drivers
 * that were uninstalled (devices the user disabled stay disabled). */
int devmgr_rescan(void)
{
    int before = g_dev_count;
    if (g_uninstalled & (1u << DEVCTL_NET))   { if (!setting_get_int("dev_net_off", 0)) net_set_adapter_enabled(1); devmgr_log("Ethernet adapter", "Driver reinstalled (e1000)"); }
    if (g_uninstalled & (1u << DEVCTL_AUDIO)) { if (!setting_get_int("dev_audio_off", 0)) audio_set_enabled(1); devmgr_log("Audio controller (AC97)", "Driver reinstalled (ac97)"); }
    g_uninstalled = 0;
    int ej[8], ne = usb_ejected_ports(ej, 8);
    for (int i = 0; i < ne; i++) usb_enable_port(ej[i]);
    devmgr_init();
    char t[72];
    snprintf(t, sizeof(t), "Hardware scan: %d device(s) (was %d)", g_dev_count, before);
    devmgr_log("Device Manager", t);
    return g_dev_count;
}

/* Driver details for Properties > Driver. */
void devmgr_driver_info(int id, char *out, size_t n)
{
    if (id < 0 || id >= g_dev_count) { snprintf(out, n, "no such device"); return; }
    dev_info_t *in = &g_devs[id].info;
    static const struct { const char *drv, *desc, *file; } info[] = {
        { "ahci", "Serial ATA (AHCI) storage driver", "drivers/ahci.c" },
        { "e1000", "Intel PRO/1000 Ethernet driver", "drivers/nic.c" },
        { "ac97", "AC'97 audio driver", "drivers/audio.c" },
        { "xhci", "USB 3 (xHCI) host controller driver", "drivers/usb.c" },
        { "usb-storage", "USB mass storage (bulk-only) driver", "drivers/usb.c" },
        { "usb-hid", "USB keyboard/mouse driver", "drivers/usb.c" },
        { "framebuffer", "Linear framebuffer display driver", "drivers/gpu.c" },
        { "ps2", "PS/2 keyboard and mouse driver", "drivers/input.c" },
        { "pit", "Programmable interval timer", "kernel/platform.c" },
        { "serial", "16550 serial port driver", "kernel/platform.c" },
        { "block", "Disk (block device) layer", "kernel/block.c" },
        { "system", "PCI bridge (no driver needed)", "kernel/pci.c" },
    };
    if (!in->driver[0]) { snprintf(out, n, "No driver is installed for this device."); return; }
    for (size_t i = 0; i < sizeof(info) / sizeof(info[0]); i++)
        if (!strcmp(info[i].drv, in->driver)) {
            snprintf(out, n, "%s\nProvider: JARVIS OS\nVersion: 1.0 (built %s)\nFile: %s (built into the kernel)", info[i].desc, __DATE__, info[i].file);
            return;
        }
    snprintf(out, n, "Driver %s\nProvider: JARVIS OS\nVersion: 1.0 (built %s)\nBuilt into the kernel", in->driver, __DATE__);
}

static int dm_icontains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 0;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i]) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            i++;
        }
        if (i == n) return 1;
    }
    return 0;
}

/* A device by id number, name or type ("network", "sound", "usb drive"). */
int devmgr_find(const char *name)
{
    dev_info_t tmp[1];
    devmgr_list(tmp, 0);                                /* refresh after plug events */
    if (name[0] >= '0' && name[0] <= '9') {
        int id = 0;
        for (const char *p = name; *p >= '0' && *p <= '9'; p++) id = id * 10 + (*p - '0');
        return id < g_dev_count ? id : -1;
    }
    for (int i = 0; i < g_dev_count; i++) if (dm_icontains(g_devs[i].info.name, name)) return i;
    for (int i = 0; i < g_dev_count; i++) if (dm_icontains(g_devs[i].info.type, name)) return i;
    if (dm_icontains(name, "sound") || dm_icontains(name, "speaker")) return devmgr_find("Audio");
    if (dm_icontains(name, "ethernet") || dm_icontains(name, "internet")) return devmgr_find("Network");
    if (dm_icontains(name, "flash") || dm_icontains(name, "usb")) return devmgr_find("USB");
    return -1;
}
