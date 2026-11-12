/*
 * JARVIS OS USB host controller stub.
 */

#include "../kernel/kernel.h"

static int usb_ready = 0;
static int camera_ready_state = 0;

void usb_init(void)
{
    pci_device_t *xhci = pci_find_class(0x0C, 0x03);
    if (!xhci)
        xhci = pci_find_class(0x0C, 0xFF);

    if (!xhci) {
        terminal_puts("[USB] No USB host controller found\n");
        usb_ready = 0;
        camera_ready_state = 0;
        g_kstate.usb_ready = 0;
        g_kstate.camera_ready = 0;
        return;
    }

    usb_ready = 1;
    camera_ready_state = 0;
    g_kstate.usb_ready = 1;
    g_kstate.camera_ready = 0;

    terminal_printf("[USB] Host controller present at %02x:%02x.%x (class=%02x/%02x)\n",
        xhci->bus, xhci->dev, xhci->fn, xhci->class_code, xhci->subclass);
    terminal_puts("[USB] HID/tablet path available; UVC camera driver not implemented yet\n");
}

int usb_is_ready(void) { return usb_ready; }
int camera_is_ready(void) { return camera_ready_state; }
