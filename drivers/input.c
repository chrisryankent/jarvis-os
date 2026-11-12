/*
 * PS/2 keyboard + mouse input (QEMU's default pair, and real 8042 hardware).
 *
 * Interrupt handlers only queue raw bytes; input_poll() decodes them on the
 * main loop and feeds the desktop.
 */

#include "../kernel/kernel.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static int mouse_ready = 0;
static uint8_t mouse_packet[3];
static int mouse_packet_len = 0;
static int mouse_buttons = 0;

static uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static int ps2_wait_input_empty(void)
{
    for (int i = 0; i < 100000; i++) {
        if ((inb(PS2_STATUS) & 0x02) == 0)
            return 1;
        cpu_relax();
    }
    return 0;
}

static int ps2_wait_output_full(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01)
            return 1;
        cpu_relax();
    }
    return 0;
}

static void ps2_write_cmd(uint8_t cmd)
{
    if (ps2_wait_input_empty())
        outb(PS2_CMD, cmd);
}

static void ps2_write_data(uint8_t data)
{
    if (ps2_wait_input_empty())
        outb(PS2_DATA, data);
}

static uint8_t ps2_read_data(void)
{
    if (!ps2_wait_output_full())
        return 0;
    return inb(PS2_DATA);
}

static int ps2_mouse_write(uint8_t data)
{
    ps2_write_cmd(0xD4);
    ps2_write_data(data);
    return ps2_read_data() == 0xFA;
}

static void input_init_locked(void)
{
    ps2_write_cmd(0xA8);                    /* enable the mouse port          */
    ps2_write_cmd(0x20);                    /* read controller command byte   */
    uint8_t command_byte = ps2_read_data();
    command_byte |= 0x03;                   /* IRQ1 (keyboard) + IRQ12 (mouse) */
    command_byte &= (uint8_t)~0x20;         /* un-gate the mouse clock        */
    ps2_write_cmd(0x60);
    ps2_write_data(command_byte);

    if (!ps2_mouse_write(0xF6) || !ps2_mouse_write(0xF4)) {
        mouse_ready = 0;
        terminal_puts("[INPUT] PS/2 mouse not responding; keyboard only\n");
        return;
    }

    mouse_ready = 1;
    terminal_puts("[INPUT] PS/2 mouse ready\n");
}

void input_init(void)
{
    terminal_puts("[INPUT] Initialising PS/2 keyboard + mouse\n");

    /* Interrupts off while we talk to the controller: otherwise the IRQ
     * handlers would steal the ACK bytes this sequence is waiting for. */
    cli();
    input_init_locked();

    /* Unmask IRQ1 (keyboard), IRQ2 (cascade) on the master PIC and IRQ12
     * (mouse) on the slave. */
    outb(0x21, (uint8_t)(inb(0x21) & ~0x06));
    outb(0xA1, (uint8_t)(inb(0xA1) & ~0x10));
    sti();
}

/* ---- Raw byte ring: filled from IRQ1/IRQ12, drained by input_poll() ------ */
#define PS2_RING_SIZE 256
static volatile uint16_t ps2_ring[PS2_RING_SIZE];   /* bit 8 set = mouse byte */
static volatile uint32_t ps2_ring_head = 0;         /* written by producer    */
static volatile uint32_t ps2_ring_tail = 0;         /* written by consumer    */

/*
 * Pull every pending byte out of the 8042 and tag it with its source (status
 * bit 5 = "from the mouse port"). Runs with interrupts off: either inside an
 * IRQ handler or from input_poll() under cli().
 */
void ps2_irq_service(void)
{
    for (int reads = 0; reads < 32; reads++) {
        uint8_t status = inb(PS2_STATUS);
        if ((status & 0x01) == 0)
            return;

        uint16_t entry = inb(PS2_DATA);
        if (status & 0x20)
            entry |= 0x100;

        uint32_t next = (ps2_ring_head + 1) % PS2_RING_SIZE;
        if (next != ps2_ring_tail) {
            ps2_ring[ps2_ring_head] = entry;
            ps2_ring_head = next;
        }
    }
}

static void mouse_handle_byte(uint8_t data)
{
    /* Byte 0 of every packet has bit 3 set; use it to stay in sync. */
    if (mouse_packet_len == 0 && (data & 0x08) == 0)
        return;

    mouse_packet[mouse_packet_len++] = data;
    if (mouse_packet_len < 3)
        return;
    mouse_packet_len = 0;

    if (mouse_packet[0] & 0xC0)     /* X/Y overflow: discard the packet */
        return;

    int dx = (int)mouse_packet[1] - ((mouse_packet[0] & 0x10) ? 256 : 0);
    int dy = (int)mouse_packet[2] - ((mouse_packet[0] & 0x20) ? 256 : 0);
    int buttons = mouse_packet[0] & 0x07;

    if (dx != 0 || dy != 0)
        ui_shell_move_cursor(dx, -dy);

    /* Press and release are reported separately so windows can be dragged.
     * (This used to send a click+release pair on press, which made dragging,
     * resizing and anything else that needs a held button impossible.) */
    static const int button_ids[3] = { 1, 2, 3 };  /* left, right, middle */
    for (int b = 0; b < 3; b++) {
        int mask = 1 << b;
        if ((buttons & mask) && !(mouse_buttons & mask))
            ui_shell_mouse_button(button_ids[b], 1);
        else if (!(buttons & mask) && (mouse_buttons & mask))
            ui_shell_mouse_button(button_ids[b], 0);
    }
    mouse_buttons = buttons;
}

void input_poll(void)
{
    /* Catch anything whose IRQ is masked or was missed. */
    cli();
    ps2_irq_service();
    sti();

    while (ps2_ring_tail != ps2_ring_head) {
        uint16_t entry = ps2_ring[ps2_ring_tail];
        ps2_ring_tail = (ps2_ring_tail + 1) % PS2_RING_SIZE;

        if (entry & 0x100) {
            if (mouse_ready)
                mouse_handle_byte((uint8_t)entry);
        } else {
            kbd_handle_scancode((uint8_t)entry);
        }
    }
}
