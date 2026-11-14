/*
 * =============================================================================
 * JARVIS AI OS — USB (drivers/usb.c)
 * Language: C17
 *
 *   xHCI host controller   the USB 3 controller in every modern PC (and in
 *                          QEMU as qemu-xhci): reset, command and event
 *                          rings, root-hub ports
 *   enumeration            every device plugged in at boot gets an address,
 *                          and its descriptors are read to learn what it is
 *   mass storage           flash drives and USB disks (Bulk-Only Transport,
 *                          SCSI commands); registered as block devices
 *                          usb0, usb1, ...
 *
 * Other devices (keyboards, Bluetooth adapters, phones, cameras) are
 * identified and listed in the device manager, but have no driver yet.
 *
 * Plugging in and unplugging: a kernel process ("usb") checks the root-hub
 * ports twice a second; new devices are set up, removed ones are torn down
 * and their drives disappear from the file manager. Devices behind an
 * external USB hub are not detected. Everything is polled; the controller's
 * interrupts are not used. One mutex serialises all controller access.
 *
 * Follows the xHCI 1.2 and USB Mass Storage Bulk-Only 1.0 specifications.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include <string.h>
#include <stdio.h>

#define RING_TRBS     64
#define MAX_SLOTS     16
#define USB_MAX_DEVS  16
#define MSC_BUF       65536

/* TRB types */
#define TRB_NORMAL       1
#define TRB_SETUP        2
#define TRB_DATA         3
#define TRB_STATUS       4
#define TRB_LINK         6
#define TRB_ENABLE_SLOT  9
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIG_EP    12
#define TRB_EV_TRANSFER  32
#define TRB_EV_COMMAND   33
#define TRB_EV_PORT      34

#define CC_SUCCESS     1
#define CC_SHORT       13

typedef struct { uint64_t param; uint32_t status; uint32_t control; } trb_t;

typedef struct {
    trb_t   *trbs;
    uint32_t index;
    uint32_t cycle;
} ring_t;

typedef struct {
    int      used;
    int      block_dev;         /* registered block device, -1 = none */
    int      slot;
    int      port;              /* root-hub port, 1-based */
    int      speed;             /* 1 full, 2 low, 3 high, 4 super */
    uint16_t vendor, product;
    uint8_t  dev_class, dev_subclass, dev_protocol;     /* of the device or its first interface */
    char     name[48];
    char     driver[16];
    ring_t   ep0;
    uint8_t *out_ctx;
    uint8_t *in_ctx;
    /* mass storage */
    int      bulk_in_dci, bulk_out_dci;
    ring_t   bulk_in, bulk_out;
    uint8_t *buf;
    uint32_t tag;
    uint64_t blocks;
} usb_dev_t;

static volatile uint8_t *g_cap, *g_op, *g_rt;
static volatile uint32_t *g_db;
static uint64_t *g_dcbaa;
static ring_t    g_cmd;
static trb_t    *g_evt;
static uint32_t  g_evt_index, g_evt_cycle;
static int       g_ctx_size = 32;
static int       g_max_ports;
static usb_dev_t g_devs[USB_MAX_DEVS];
static int       g_dev_count = 0;
static int       usb_ready = 0;
static int       g_lock = -1;
static uint8_t   g_port_tries[256];  /* failed set-up attempts per port */
#define PORT_EJECTED 0xFE           /* stopped by the user; set up again when re-plugged or enabled */

/* ---- Small helpers ------------------------------------------------------- */
static uint32_t rd32(volatile uint8_t *base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static void     wr32(volatile uint8_t *base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }
static void     wr64(volatile uint8_t *base, uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(base + off) = (uint32_t)v;
    *(volatile uint32_t *)(base + off + 4) = (uint32_t)(v >> 32);
}

static void *dma_alloc(uint32_t pages)
{
    uint64_t p = pmm_alloc_contiguous(pages);
    if (!p || p + pages * 4096ULL > 0x100000000ULL) return NULL;
    memset((void *)p, 0, pages * 4096ULL);
    return (void *)p;
}

static void dma_free(void *p, uint32_t pages)
{
    if (!p) return;
    for (uint32_t i = 0; i < pages; i++)
        pmm_free_frame((uint64_t)(uintptr_t)p + i * 4096ULL);
}

static void delay_ms(uint32_t ms)
{
    uint64_t end = g_kstate.uptime_ticks + ms / 10 + 1;
    while (g_kstate.uptime_ticks < end) __asm__ volatile ("pause");
}

static int ring_init(ring_t *r)
{
    r->trbs = (trb_t *)dma_alloc(1);
    if (!r->trbs) return -1;
    r->index = 0;
    r->cycle = 1;
    trb_t *link = &r->trbs[RING_TRBS - 1];         /* last TRB jumps back to the start */
    link->param = (uint64_t)(uintptr_t)r->trbs;
    link->control = (TRB_LINK << 10) | (1u << 1);   /* toggle cycle */
    return 0;
}

static uint64_t ring_push(ring_t *r, uint64_t param, uint32_t status, uint32_t control)
{
    trb_t *t = &r->trbs[r->index];
    t->param = param;
    t->status = status;
    __asm__ volatile ("" ::: "memory");
    t->control = control | r->cycle;
    uint64_t addr = (uint64_t)(uintptr_t)t;

    if (++r->index == RING_TRBS - 1) {              /* reached the link TRB */
        trb_t *link = &r->trbs[RING_TRBS - 1];
        link->control = (link->control & ~1u) | r->cycle;
        r->index = 0;
        r->cycle ^= 1;
    }
    return addr;
}

/* Wait for an event of `type` (and, for transfers, on `slot`). Returns the
 * completion code, or -1 on timeout. Other events are consumed. */
static int wait_event(uint32_t type, int slot, uint32_t ms, trb_t *out)
{
    uint64_t end = g_kstate.uptime_ticks + ms / 10 + 2;
    while (g_kstate.uptime_ticks < end) {
        trb_t *e = &g_evt[g_evt_index];
        if ((e->control & 1) != g_evt_cycle) { __asm__ volatile ("pause"); continue; }

        trb_t ev = *e;
        if (++g_evt_index == RING_TRBS) { g_evt_index = 0; g_evt_cycle ^= 1; }
        wr64(g_rt, 0x20 + 0x18, (uint64_t)(uintptr_t)&g_evt[g_evt_index] | (1u << 3));

        uint32_t t = (ev.control >> 10) & 63;
        if (t == type && (slot < 0 || (int)(ev.control >> 24) == slot)) {
            if (out) *out = ev;
            return (int)(ev.status >> 24);
        }
    }
    return -1;
}

static int command(uint64_t param, uint32_t control, trb_t *ev)
{
    ring_push(&g_cmd, param, 0, control);
    g_db[0] = 0;
    return wait_event(TRB_EV_COMMAND, -1, 1000, ev);
}

static uint32_t *ctx(uint8_t *base, int index)         /* index 0 = slot (or input control) */
{
    return (uint32_t *)(base + index * g_ctx_size);
}

/* ---- Control transfers on endpoint 0 ------------------------------------- */
static int control(usb_dev_t *d, uint8_t reqtype, uint8_t req, uint16_t value,
                   uint16_t index, void *data, uint16_t len)
{
    uint64_t setup = reqtype | ((uint64_t)req << 8) | ((uint64_t)value << 16) |
                     ((uint64_t)index << 32) | ((uint64_t)len << 48);
    int in = (reqtype & 0x80) != 0;
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;

    ring_push(&d->ep0, setup, 8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (len)
        ring_push(&d->ep0, (uint64_t)(uintptr_t)d->buf, len, (TRB_DATA << 10) | (in ? 1u << 16 : 0));
    ring_push(&d->ep0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0 : 1u << 16));
    g_db[d->slot] = 1;

    int cc = wait_event(TRB_EV_TRANSFER, d->slot, 1000, NULL);
    if (cc != CC_SUCCESS && cc != CC_SHORT) return -1;
    if (in && len && data) memcpy(data, d->buf, len);
    return 0;
}

/* ---- Bulk transfers ------------------------------------------------------ */
static int bulk(usb_dev_t *d, int in, void *data, uint32_t len)
{
    ring_t *r = in ? &d->bulk_in : &d->bulk_out;
    int dci = in ? d->bulk_in_dci : d->bulk_out_dci;
    if (!in && len) memcpy(d->buf, data, len);

    ring_push(r, (uint64_t)(uintptr_t)d->buf, len, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
    g_db[d->slot] = (uint32_t)dci;
    trb_t ev;
    int cc = wait_event(TRB_EV_TRANSFER, d->slot, 5000, &ev);
    if (cc != CC_SUCCESS && cc != CC_SHORT) return -1;
    if (in && len) memcpy(data, d->buf, len);
    return (int)(len - (ev.status & 0xFFFFFF));     /* bytes actually moved */
}

/* ---- Mass storage: Bulk-Only Transport + SCSI ---------------------------- */
static int scsi(usb_dev_t *d, const uint8_t *cdb, int cdb_len, int in, void *data, uint32_t len)
{
    uint8_t cbw[31], csw[13];
    memset(cbw, 0, sizeof(cbw));
    uint32_t tag = ++d->tag;
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &len, 4);
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = (uint8_t)cdb_len;
    memcpy(cbw + 15, cdb, (size_t)cdb_len);

    if (bulk(d, 0, cbw, 31) != 31) return -1;
    if (len && bulk(d, in, data, len) < 0) return -1;
    if (bulk(d, 1, csw, 13) != 13) return -1;
    if (csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S' ||
        memcmp(csw + 4, &tag, 4) != 0 || csw[12] != 0)
        return -1;
    return 0;
}

static int msc_rw(usb_dev_t *d, int write, uint64_t lba, uint32_t count, uint8_t *buf)
{
    if (g_lock >= 0) kmutex_lock(g_lock);
    int rc = 0;
    while (count && rc == 0) {
        uint32_t n = count > MSC_BUF / 512 ? MSC_BUF / 512 : count;
        uint8_t cdb[10] = { write ? 0x2A : 0x28, 0,
                            (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                            0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
        if (!d->used || lba + n > 0xFFFFFFFFULL || scsi(d, cdb, 10, !write, buf, n * 512) != 0) {
            rc = -1;
            break;
        }
        buf += n * 512; lba += n; count -= n;
    }
    if (g_lock >= 0) kmutex_unlock(g_lock);
    return rc;
}

static int msc_read(void *c, uint64_t lba, uint32_t count, void *buf)
{
    return msc_rw((usb_dev_t *)c, 0, lba, count, (uint8_t *)buf);
}

static int msc_write(void *c, uint64_t lba, uint32_t count, const void *buf)
{
    return msc_rw((usb_dev_t *)c, 1, lba, count, (uint8_t *)buf);
}

static int configure_bulk(usb_dev_t *d, uint8_t ep_in, uint16_t mps_in, uint8_t ep_out, uint16_t mps_out)
{
    d->bulk_in_dci = (ep_in & 15) * 2 + 1;
    d->bulk_out_dci = (ep_out & 15) * 2;
    if (ring_init(&d->bulk_in) || ring_init(&d->bulk_out)) return -1;

    memset(d->in_ctx, 0, 33 * (size_t)g_ctx_size);
    ctx(d->in_ctx, 0)[1] = 1u | (1u << d->bulk_in_dci) | (1u << d->bulk_out_dci);    /* add slot + both */
    memcpy(ctx(d->in_ctx, 1), ctx(d->out_ctx, 0), (size_t)g_ctx_size);              /* current slot ctx */
    int last = d->bulk_in_dci > d->bulk_out_dci ? d->bulk_in_dci : d->bulk_out_dci;
    uint32_t *slot = ctx(d->in_ctx, 1);
    slot[0] = (slot[0] & ~(31u << 27)) | ((uint32_t)last << 27);

    struct { int dci; ring_t *r; uint32_t type; uint16_t mps; } eps[2] = {
        { d->bulk_in_dci, &d->bulk_in, 6, mps_in },      /* bulk IN  */
        { d->bulk_out_dci, &d->bulk_out, 2, mps_out },   /* bulk OUT */
    };
    for (int i = 0; i < 2; i++) {
        uint32_t *ep = ctx(d->in_ctx, eps[i].dci + 1);
        ep[1] = (3u << 1) | (eps[i].type << 3) | ((uint32_t)eps[i].mps << 16);
        uint64_t deq = (uint64_t)(uintptr_t)eps[i].r->trbs | 1;
        ep[2] = (uint32_t)deq; ep[3] = (uint32_t)(deq >> 32);
        ep[4] = 3072;                                   /* average TRB length */
    }
    return command((uint64_t)(uintptr_t)d->in_ctx, (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL)
           == CC_SUCCESS ? 0 : -1;
}

static void msc_attach(usb_dev_t *d, uint8_t ep_in, uint16_t mps_in, uint8_t ep_out, uint16_t mps_out)
{
    uint8_t data[36];
    if (configure_bulk(d, ep_in, mps_in, ep_out, mps_out) != 0) {
        terminal_printf("[USB] %s: could not configure its endpoints\n", d->name);
        return;
    }

    uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (scsi(d, inquiry, 6, 1, data, 36) == 0) {
        char vendor[9], product[17];
        memcpy(vendor, data + 8, 8); vendor[8] = 0;
        memcpy(product, data + 16, 16); product[16] = 0;
        for (int i = 7; i >= 0 && vendor[i] == ' '; i--) vendor[i] = 0;
        for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;
        snprintf(d->name, sizeof(d->name), "%s %s", vendor, product);
    }

    uint8_t cap[8], rc10[10] = { 0x25 };
    int ok = 0;
    for (int attempt = 0; attempt < 5 && !ok; attempt++) {     /* may report "not ready" at first */
        uint8_t tur[6] = { 0 };
        scsi(d, tur, 6, 0, NULL, 0);
        ok = scsi(d, rc10, 10, 1, cap, 8) == 0;
        if (!ok) delay_ms(100);
    }
    if (!ok) {
        terminal_printf("[USB] %s: no medium\n", d->name);
        return;
    }
    uint32_t last = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) | ((uint32_t)cap[2] << 8) | cap[3];
    uint32_t bsize = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) | ((uint32_t)cap[6] << 8) | cap[7];
    if (bsize != 512) {
        terminal_printf("[USB] %s: %u-byte sectors are not supported\n", d->name, bsize);
        return;
    }
    d->blocks = (uint64_t)last + 1;

    /* lowest free usbN name, so a re-plugged stick gets its old name back */
    char name[8] = "usb0";
    for (int n = 0; n < 10; n++) {
        name[3] = (char)('0' + n);
        int taken = 0;
        for (int i = 0; i < USB_MAX_DEVS; i++) {
            block_info_t bi;
            if (g_devs[i].used && g_devs[i].block_dev >= 0 &&
                block_get_info(g_devs[i].block_dev, &bi) == 0 && !strcmp(bi.name, name))
                taken = 1;
        }
        if (!taken) break;
    }
    strncpy(d->driver, "usb-storage", sizeof(d->driver) - 1);
    d->block_dev = block_register(name, d->name, d->blocks, msc_read, msc_write, NULL, d);
    terminal_printf("[USB] %s: %s, %llu MB\n", name, d->name, (unsigned long long)(d->blocks / 2048));
}

/* ---- Enumeration --------------------------------------------------------- */
static const char *class_name(uint8_t cls, uint8_t sub, uint8_t proto)
{
    switch (cls) {
    case 0x01: return "USB audio device";
    case 0x02: return "USB modem / network";
    case 0x03: return proto == 1 ? "USB keyboard" : proto == 2 ? "USB mouse" : "USB input device";
    case 0x06: return "USB camera / phone (PTP)";
    case 0x07: return "USB printer";
    case 0x08: return "USB storage";
    case 0x09: return "USB hub";
    case 0x0E: return "USB camera";
    case 0xE0: return (sub == 1 && proto == 1) ? "USB Bluetooth adapter" : "USB wireless device";
    case 0xFF: return "USB device (vendor-specific)";
    default:   return "USB device";
    }
}

static void device_free(usb_dev_t *d)
{
    dma_free(d->out_ctx, 1);
    dma_free(d->in_ctx, 2);
    dma_free(d->buf, MSC_BUF / 4096);
    dma_free(d->ep0.trbs, 1);
    dma_free(d->bulk_in.trbs, 1);
    dma_free(d->bulk_out.trbs, 1);
    memset(d, 0, sizeof(*d));
    d->block_dev = -1;
}

/* Returns 1 if a device was set up on the port. */
static int enumerate_port(int port, int speed)
{
    usb_dev_t *d = NULL;
    for (int i = 0; i < USB_MAX_DEVS; i++)
        if (!g_devs[i].used && !g_devs[i].slot) { d = &g_devs[i]; break; }
    if (!d) return 0;
    memset(d, 0, sizeof(*d));
    d->block_dev = -1;
    if (d - g_devs >= g_dev_count) g_dev_count = (int)(d - g_devs) + 1;
    trb_t ev;

    if (command(0, TRB_ENABLE_SLOT << 10, &ev) != CC_SUCCESS) return 0;
    d->slot = (int)(ev.control >> 24);
    d->port = port;
    d->speed = speed;
    d->out_ctx = (uint8_t *)dma_alloc(1);
    d->in_ctx = (uint8_t *)dma_alloc(2);
    d->buf = (uint8_t *)dma_alloc(MSC_BUF / 4096);
    if (!d->out_ctx || !d->in_ctx || !d->buf || ring_init(&d->ep0)) goto fail;
    g_dcbaa[d->slot] = (uint64_t)(uintptr_t)d->out_ctx;

    uint16_t mps0 = speed == 4 ? 512 : speed == 3 ? 64 : 8;
    ctx(d->in_ctx, 0)[1] = 3;                               /* add slot + EP0 */
    uint32_t *slot = ctx(d->in_ctx, 1);
    slot[0] = ((uint32_t)speed << 20) | (1u << 27);
    slot[1] = (uint32_t)port << 16;
    uint32_t *ep0 = ctx(d->in_ctx, 2);
    ep0[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps0 << 16);
    uint64_t deq = (uint64_t)(uintptr_t)d->ep0.trbs | 1;
    ep0[2] = (uint32_t)deq; ep0[3] = (uint32_t)(deq >> 32);
    ep0[4] = 8;

    if (command((uint64_t)(uintptr_t)d->in_ctx, (TRB_ADDRESS_DEV << 10) | ((uint32_t)d->slot << 24), NULL)
        != CC_SUCCESS) {
        terminal_printf("[USB] Port %d: device did not accept an address\n", port);
        goto fail;
    }

    uint8_t desc[18];
    if (control(d, 0x80, 6, 0x0100, 0, desc, 18) != 0) goto fail;
    d->vendor = (uint16_t)(desc[8] | (desc[9] << 8));
    d->product = (uint16_t)(desc[10] | (desc[11] << 8));
    d->dev_class = desc[4]; d->dev_subclass = desc[5]; d->dev_protocol = desc[6];

    uint8_t cfg[512];
    if (control(d, 0x80, 6, 0x0200, 0, cfg, 9) != 0) goto fail;
    uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    if (total > sizeof(cfg)) total = sizeof(cfg);
    if (control(d, 0x80, 6, 0x0200, 0, cfg, total) != 0) goto fail;

    /* First interface decides what the device is; note bulk endpoints. */
    int iface_seen = 0, is_msc = 0;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t mps_in = 0, mps_out = 0;
    for (int i = 0; i + 2 <= total && cfg[i] >= 2; i += cfg[i]) {
        if (cfg[i + 1] == 4 && !iface_seen) {               /* interface */
            iface_seen = 1;
            if (d->dev_class == 0) {
                d->dev_class = cfg[i + 5]; d->dev_subclass = cfg[i + 6]; d->dev_protocol = cfg[i + 7];
            }
            is_msc = cfg[i + 5] == 8 && cfg[i + 7] == 0x50;
        } else if (cfg[i + 1] == 5 && is_msc && (cfg[i + 3] & 3) == 2) {   /* bulk endpoint */
            uint16_t mps = (uint16_t)(cfg[i + 4] | (cfg[i + 5] << 8));
            if (cfg[i + 2] & 0x80) { if (!ep_in) { ep_in = cfg[i + 2]; mps_in = mps; } }
            else                   { if (!ep_out) { ep_out = cfg[i + 2]; mps_out = mps; } }
        }
    }

    snprintf(d->name, sizeof(d->name), "%s", class_name(d->dev_class, d->dev_subclass, d->dev_protocol));
    d->used = 1;
    terminal_printf("[USB] Port %d: %s (%04x:%04x)\n", port, d->name, d->vendor, d->product);

    if (control(d, 0x00, 9, cfg[5], 0, NULL, 0) != 0)       /* SET_CONFIGURATION */
        return 1;
    if (is_msc && ep_in && ep_out)
        msc_attach(d, ep_in, mps_in, ep_out, mps_out);
    return 1;

fail:
    if (d->slot) {
        g_dcbaa[d->slot] = 0;
        command(0, (10u << 10) | ((uint32_t)d->slot << 24), NULL);     /* DISABLE_SLOT */
    }
    device_free(d);
    return 0;
}

/* The device on a port was unplugged. Returns its block device or -1. */
static int detach(usb_dev_t *d)
{
    int dev = d->block_dev;
    terminal_printf("[USB] Port %d: %s removed\n", d->port, d->name);
    if (dev >= 0) block_remove(dev);
    g_dcbaa[d->slot] = 0;
    command(0, (10u << 10) | ((uint32_t)d->slot << 24), NULL);         /* DISABLE_SLOT */
    device_free(d);
    return dev;
}

/* ---- Controller bring-up ------------------------------------------------- */
static int wait32(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t end = g_kstate.uptime_ticks + ms / 10 + 2;
    while ((rd32(base, off) & mask) != want)
        if (g_kstate.uptime_ticks > end) return -1;
    return 0;
}

#define PORT_RO_RWS  ((1u << 0) | (1u << 3) | (0xFu << 10) | (1u << 30) | (0xFu << 5) | (1u << 9) | (3u << 14) | (7u << 25))

static usb_dev_t *device_on_port(int port)
{
    for (int i = 0; i < USB_MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].port == port) return &g_devs[i];
    return NULL;
}

/* Enable the port if needed and set up the device on it. Returns 1 if a
 * device is now working there. */
static int port_bringup(int p)
{
    uint32_t off = 0x400 + (uint32_t)(p - 1) * 0x10;
    uint32_t sc = rd32(g_op, off);
    if (!(sc & 1)) return 0;                                /* nothing connected */

    if (!(sc & 2) && ((sc >> 5) & 15) != 5) {               /* USB 2 (not USB 3 RxDetect): reset */
        wr32(g_op, off, (sc & PORT_RO_RWS) | (1u << 4));
        if (wait32(g_op, off, 1u << 21, 1u << 21, 500)) return 0;
        delay_ms(20);
    }
    sc = rd32(g_op, off);
    wr32(g_op, off, (sc & PORT_RO_RWS) | (0x7Fu << 17));    /* clear change bits */
    if (!(sc & 2)) return 0;                                /* not enabled (yet) */
    return enumerate_port(p, (int)((sc >> 10) & 15));
}

/* Consume pending events (port changes etc.) so the event ring never fills. */
static void drain_events(void)
{
    for (int n = 0; n < RING_TRBS; n++) {
        trb_t *e = &g_evt[g_evt_index];
        if ((e->control & 1) != g_evt_cycle) break;
        if (++g_evt_index == RING_TRBS) { g_evt_index = 0; g_evt_cycle ^= 1; }
        wr64(g_rt, 0x20 + 0x18, (uint64_t)(uintptr_t)&g_evt[g_evt_index] | (1u << 3));
    }
}

static int usb_device_count(void);

static void usb_hotplug_process(void)
{
    for (;;) {
        kthread_sleep(50);
        int added[USB_MAX_DEVS], removed[USB_MAX_DEVS], na = 0, nr = 0;

        kmutex_lock(g_lock);
        drain_events();
        for (int p = 1; p <= g_max_ports && p < 256; p++) {
            uint32_t off = 0x400 + (uint32_t)(p - 1) * 0x10;
            uint32_t sc = rd32(g_op, off);
            usb_dev_t *d = device_on_port(p);

            if (!(sc & 1)) {                                /* empty */
                g_port_tries[p] = 0;
                if (d) {
                    int dev = detach(d);
                    if (dev >= 0) removed[nr++] = dev;
                }
                if (sc & (0x7Fu << 17)) wr32(g_op, off, (sc & PORT_RO_RWS) | (0x7Fu << 17));
                continue;
            }
            if (d || g_port_tries[p] >= 3) continue;        /* already set up / gave up / ejected */

            if (port_bringup(p)) {
                d = device_on_port(p);
                if (d && d->block_dev >= 0) added[na++] = d->block_dev;
                else if (d) g_storage_generation++;          /* device manager refresh */
            } else {
                g_port_tries[p]++;
            }
        }
        kmutex_unlock(g_lock);

        /* File systems are mounted outside the USB lock: they read through it. */
        for (int i = 0; i < nr; i++) fatfs_unmount_dev(removed[i]);
        for (int i = 0; i < na; i++) fatfs_mount_dev(added[i]);
        if (nr) scheduler_post_event("USB drive removed");
    }
}

/* Safe removal: tell the drive to stop (SCSI START STOP UNIT, eject), then
 * release it. The port stays off until the drive is re-plugged or enabled
 * again. Returns 0, or -1 if no USB device is on that port. */
int usb_eject_port(int port)
{
    if (!usb_ready || port < 1 || port >= 256) return -1;
    kmutex_lock(g_lock);
    usb_dev_t *d = device_on_port(port);
    int dev = d ? d->block_dev : -1;
    kmutex_unlock(g_lock);
    if (!d) return -1;

    if (dev >= 0) {
        fatfs_unmount_dev(dev);                     /* nothing reads it any more */
        block_flush(dev);
    }
    kmutex_lock(g_lock);
    d = device_on_port(port);
    if (d) {
        if (d->bulk_in_dci) {
            uint8_t cdb[6] = { 0x1B, 0, 0, 0, 0x02, 0 };    /* START STOP UNIT: LoEj=1, Start=0 */
            scsi(d, cdb, 6, 0, NULL, 0);
        }
        detach(d);
        g_port_tries[port] = PORT_EJECTED;
    }
    kmutex_unlock(g_lock);
    g_storage_generation++;
    return 0;
}

/* Undo usb_eject_port: the hot-plug thread sets the device up again. */
int usb_enable_port(int port)
{
    if (port < 1 || port >= 256 || g_port_tries[port] != PORT_EJECTED) return -1;
    g_port_tries[port] = 0;
    return 0;
}

/* Ports holding a device that was ejected but is still plugged in. */
int usb_ejected_ports(int *ports, int max)
{
    int n = 0;
    for (int p = 1; p <= g_max_ports && p < 256 && n < max; p++)
        if (g_port_tries[p] == PORT_EJECTED && (rd32(g_op, 0x400 + (uint32_t)(p - 1) * 0x10) & 1)) ports[n++] = p;
    return n;
}

/* USB port of a block device (usbN), or -1. */
int usb_port_of_block(int dev)
{
    for (int i = 0; i < USB_MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].block_dev == dev) return g_devs[i].port;
    return -1;
}

void usb_start_hotplug(void)
{
    if (usb_ready)
        proc_spawn("usb", usb_hotplug_process, PRIO_SYSTEM, 0);
}

void usb_init(void)
{
    g_kstate.usb_ready = 0;
    g_kstate.camera_ready = 0;

    pci_device_t *pci = NULL;
    for (int i = 0; (pci = pci_get(i)) != NULL; i++)
        if (pci->class_code == 0x0C && pci->subclass == 0x03 && pci->prog_if == 0x30) break;
    if (!pci) {
        terminal_puts("[USB] No USB 3 (xHCI) controller found\n");
        return;
    }

    uint64_t bar = (pci->bar[0] & ~0xFULL) | ((pci->bar[0] & 4) ? (uint64_t)pci->bar[1] << 32 : 0);
    if (!bar || bar >= 0x100000000ULL) {
        terminal_puts("[USB] xHCI registers are outside the mapped range\n");
        return;
    }
    g_cap = (volatile uint8_t *)vmm_map_range(bar, 0x10000, VMM_PROT_RW | VMM_PROT_NOCACHE);
    g_op = g_cap + (rd32(g_cap, 0) & 0xFF);
    g_rt = g_cap + (rd32(g_cap, 0x18) & ~0x1Fu);
    g_db = (volatile uint32_t *)(g_cap + (rd32(g_cap, 0x14) & ~0x3u));
    uint32_t hcs1 = rd32(g_cap, 4), hcs2 = rd32(g_cap, 8);
    g_max_ports = (int)(hcs1 >> 24);
    g_ctx_size = (rd32(g_cap, 0x10) & 4) ? 64 : 32;

    /* BIOS handoff: some firmware keeps the controller until asked. */
    uint32_t xecp = (rd32(g_cap, 0x10) >> 16) << 2;
    for (int guard = 0; xecp && guard < 32; guard++) {
        uint32_t v = rd32(g_cap, xecp);
        if ((v & 0xFF) == 1) {
            wr32(g_cap, xecp, v | (1u << 24));
            wait32(g_cap, xecp, (1u << 16), 0, 1000);
            break;
        }
        uint32_t next = (v >> 8) & 0xFF;
        xecp = next ? xecp + next * 4 : 0;
    }

    wr32(g_op, 0, rd32(g_op, 0) & ~1u);                     /* stop */
    wait32(g_op, 4, 1, 1, 100);
    wr32(g_op, 0, 2);                                       /* reset */
    if (wait32(g_op, 0, 2, 0, 1000) || wait32(g_op, 4, 1u << 11, 0, 1000)) {
        terminal_puts("[USB] Controller did not come out of reset\n");
        return;
    }

    wr32(g_op, 0x38, MAX_SLOTS);
    g_dcbaa = (uint64_t *)dma_alloc(1);
    uint32_t scratch = ((hcs2 >> 27) & 31) | (((hcs2 >> 21) & 31) << 5);
    if (!g_dcbaa) return;
    if (scratch) {
        uint64_t *arr = (uint64_t *)dma_alloc(1);
        if (!arr || scratch > 512) return;
        for (uint32_t i = 0; i < scratch; i++) {
            void *pg = dma_alloc(1);
            if (!pg) return;
            arr[i] = (uint64_t)(uintptr_t)pg;
        }
        g_dcbaa[0] = (uint64_t)(uintptr_t)arr;
    }
    wr64(g_op, 0x30, (uint64_t)(uintptr_t)g_dcbaa);

    if (ring_init(&g_cmd)) return;
    wr64(g_op, 0x18, (uint64_t)(uintptr_t)g_cmd.trbs | 1);

    g_evt = (trb_t *)dma_alloc(1);
    uint64_t *erst = (uint64_t *)dma_alloc(1);
    if (!g_evt || !erst) return;
    erst[0] = (uint64_t)(uintptr_t)g_evt;
    erst[1] = RING_TRBS;
    g_evt_index = 0; g_evt_cycle = 1;
    wr32(g_rt, 0x20 + 0x08, 1);                             /* one segment */
    wr64(g_rt, 0x20 + 0x18, (uint64_t)(uintptr_t)g_evt);
    wr64(g_rt, 0x20 + 0x10, (uint64_t)(uintptr_t)erst);

    wr32(g_op, 0, 1);                                       /* run */
    if (wait32(g_op, 4, 1, 0, 1000)) {
        terminal_puts("[USB] Controller did not start\n");
        return;
    }
    usb_ready = 1;
    g_kstate.usb_ready = 1;
    terminal_printf("[USB] xHCI controller running, %d ports\n", g_max_ports);

    for (int i = 0; i < USB_MAX_DEVS; i++) g_devs[i].block_dev = -1;
    g_lock = kmutex_create("usb");
    delay_ms(100);                                          /* let ports settle */
    for (int p = 1; p <= g_max_ports; p++)
        port_bringup(p);
    terminal_printf("[USB] %d device(s) found\n", usb_device_count());
}

int usb_is_ready(void)    { return usb_ready; }
int camera_is_ready(void) { return 0; }

/* For the device manager: devices currently connected. */
static int usb_device_count(void)
{
    int n = 0;
    for (int i = 0; i < USB_MAX_DEVS; i++) if (g_devs[i].used) n++;
    return n;
}

int usb_device_total(void) { return usb_device_count(); }

int usb_device_info(int nth, char *name, size_t name_len, char *driver, size_t drv_len,
                    uint16_t *vendor, uint16_t *product, int *port)
{
    for (int i = 0; i < USB_MAX_DEVS; i++) {
        if (!g_devs[i].used || nth--) continue;
        snprintf(name, name_len, "%s", g_devs[i].name);
        snprintf(driver, drv_len, "%s", g_devs[i].driver);
        *vendor = g_devs[i].vendor;
        *product = g_devs[i].product;
        *port = g_devs[i].port;
        return 0;
    }
    return -1;
}
