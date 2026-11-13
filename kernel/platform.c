/*
 * Minimal freestanding platform/runtime support for the skeleton kernel build.
 */

#include "kernel.h"
#include <stdarg.h>

/* Real font data now lives in kernel/font8x16.c (see that file's header
 * comment) — this used to be `const uint8_t font8x16[256][16] = {{0}};`
 * here, an all-zero placeholder that made fb_put_char() in drivers/gpu.c
 * always treat every glyph as "empty" and fall through to its tiny
 * ~50-character emergency fallback font. */

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

static char *append_char(char *out, size_t *left, char c)
{
    if (*left > 1) {
        *out++ = c;
        (*left)--;
    }
    return out;
}

static char *append_str(char *out, size_t *left, const char *s)
{
    if (!s)
        s = "(null)";
    while (*s)
        out = append_char(out, left, *s++);
    return out;
}

static char *append_uint(char *out, size_t *left, uint64_t val, unsigned base,
                         int upper)
{
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (val == 0)
        tmp[i++] = '0';
    while (val != 0) {
        tmp[i++] = digits[val % base];
        val /= base;
    }
    while (i > 0)
        out = append_char(out, left, tmp[--i]);
    return out;
}

static int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    char *out = buf;
    size_t left = size;

    while (*fmt) {
        if (*fmt != '%') {
            out = append_char(out, &left, *fmt++);
            continue;
        }

        fmt++;
        if (*fmt == '%') {
            out = append_char(out, &left, *fmt++);
            continue;
        }

        while (*fmt >= '0' && *fmt <= '9')
            fmt++;
        if (*fmt == '.') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9')
                fmt++;
        }

        int long_count = 0;
        while (*fmt == 'l') {
            long_count++;
            fmt++;
        }

        switch (*fmt++) {
        case 's':
            out = append_str(out, &left, va_arg(ap, const char *));
            break;
        case 'c':
            out = append_char(out, &left, (char)va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            int64_t v = long_count ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (v < 0) {
                out = append_char(out, &left, '-');
                v = -v;
            }
            out = append_uint(out, &left, (uint64_t)v, 10, 0);
            break;
        }
        case 'u':
            out = append_uint(out, &left,
                long_count ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int),
                10, 0);
            break;
        case 'x':
        case 'X':
            out = append_uint(out, &left,
                long_count ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int),
                16, fmt[-1] == 'X');
            break;
        case 'p':
            out = append_str(out, &left, "0x");
            out = append_uint(out, &left, (uint64_t)va_arg(ap, void *), 16, 0);
            break;
        case 'z':
            if (*fmt == 'u') {
                fmt++;
                out = append_uint(out, &left, va_arg(ap, size_t), 10, 0);
            }
            break;
        case 'f':
            (void)va_arg(ap, double);
            out = append_str(out, &left, "0.00");
            break;
        default:
            break;
        }
    }

    if (size != 0)
        *out = '\0';
    return (int)(out - buf);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    return kvsnprintf(buf, size, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *ret = dst;
    while (n && *src) {
        *dst++ = *src++;
        n--;
    }
    while (n--)
        *dst++ = '\0';
    return ret;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return c == 0 ? (char *)s : NULL;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        if (strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
    }
    return NULL;
}

void terminal_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            outb(0x3F8, (uint8_t)'\r');
        }
        outb(0x3F8, (uint8_t)*s++);
    }
}

void terminal_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    terminal_puts(buf);
}

static void serial_init_port(uint16_t port)
{
    outb(port + 1, 0x00);
    outb(port + 3, 0x80);
    outb(port + 0, 0x01);
    outb(port + 1, 0x00);
    outb(port + 3, 0x03);
    outb(port + 2, 0xC7);
    outb(port + 4, 0x0B);
}

void terminal_init(void)
{
    serial_init_port(0x3F8); /* COM1: QEMU monitor/debug console */
    serial_init_port(0x2F8); /* COM2: host microphone/STT bridge */
}

/*
 * A UART that is not there reads back 0xFF from every register, which looks
 * like "data ready" forever. The desktop loop drains the voice bridge port
 * (COM2) with `while (poll)`, so on a machine without COM2 that loop never
 * returned and the desktop never got to draw. Probe each port once with the
 * scratch register and treat a missing port as permanently empty.
 */
static int serial_port_present(uint16_t port)
{
    static int probed[2], present[2];
    int idx = (port == 0x3F8) ? 0 : 1;

    if (!probed[idx]) {
        outb(port + 7, 0xA5);
        int ok = inb(port + 7) == 0xA5;
        outb(port + 7, 0x5A);
        ok = ok && inb(port + 7) == 0x5A;
        present[idx] = ok;
        probed[idx] = 1;
    }
    return present[idx];
}

static int serial_received_port(uint16_t port)
{
    if (!serial_port_present(port))
        return 0;
    uint8_t lsr = inb(port + 5);
    return lsr != 0xFF && (lsr & 1);
}

static char terminal_getchar(void)
{
    while (!serial_received_port(0x3F8))
        __asm__ volatile ("pause");
    return (char)inb(0x3F8);
}

void terminal_readline(const char *prompt, char *buf, size_t max, int echo)
{
    size_t len = 0;

    if (max == 0)
        return;

    terminal_puts(prompt);
    for (;;) {
        char c = terminal_getchar();

        if (c == '\r' || c == '\n') {
            terminal_puts("\n");
            break;
        }

        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                if (echo)
                    terminal_puts("\b \b");
            }
            continue;
        }

        if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E)
            continue;

        if (len + 1 < max) {
            buf[len++] = c;
            if (echo)
                terminal_printf("%c", c);
            else
                terminal_puts("*");
        }
    }

    buf[len] = '\0';
}

int terminal_poll_char(char *out)
{
    if (!serial_received_port(0x3F8))
        return 0;
    *out = (char)inb(0x3F8);
    return 1;
}

int voice_serial_poll_char(char *out)
{
    if (!serial_received_port(0x2F8))
        return 0;
    *out = (char)inb(0x2F8);
    return 1;
}

void voice_serial_puts(const char *s)
{
    if (!s)
        return;
    while (*s) {
        while ((inb(0x2F8 + 5) & 0x20) == 0)
            __asm__ volatile ("pause");
        outb(0x2F8, (uint8_t)*s++);
    }
}

/*
 * ============================================================================
 * Real IDT / PIC / PIT / interrupt dispatch
 * ============================================================================
 * Replaces what used to be four empty-stub functions. Without this, there
 * was no IDT at all — the CPU had nowhere to dispatch interrupts to, so
 * irq0_timer_handler() (which drives ai_scheduler_tick(), which is what
 * actually runs ai_brain_thread/speech_stt_thread/etc.) was never invoked
 * by real hardware. The scheduler logic itself was already correct
 * (kernel/ipc_sched_pmm.c) — nothing was driving it.
 *
 * The 256 raw entry stubs (one tiny push+jmp per vector) live in
 * kernel/interrupts.S as isr_stub_table[256] — see that file for why a
 * generated table is lower-risk here than hand-writing 256 stubs, and for
 * the exact stack layout interrupt_dispatch() below relies on.
 *
 * No TSS/IST is set up: every interrupt taken while already in ring 0
 * (true for this whole kernel right now — there is no ring-3/user mode
 * yet) keeps using whatever stack was already active, which is standard
 * and safe. A double-fault or NMI taken on a *corrupted* stack wouldn't
 * have a known-good IST stack to fall back to — acceptable for now since
 * nothing creates ring-3 processes yet, but worth flagging as a future
 * hardening item once user-mode execution exists.
 */

#define IDT_ENTRIES   256
#define PIC1_CMD      0x20
#define PIC1_DATA     0x21
#define PIC2_CMD      0xA0
#define PIC2_DATA     0xA1
#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43
#define PIT_BASE_HZ   1193182u

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idtr_t;

extern uint64_t isr_stub_table[IDT_ENTRIES];

static idt_entry_t            g_idt[IDT_ENTRIES];
static void (*g_irq_handlers[IDT_ENTRIES])(void);
static uint8_t                g_pic_master_offset = 0x20;
static uint8_t                g_pic_slave_offset  = 0x28;

static void idt_set_gate(int vector, uint64_t handler_addr)
{
    g_idt[vector].offset_low  = (uint16_t)(handler_addr & 0xFFFF);
    g_idt[vector].selector    = 0x08;   /* kernel 64-bit code segment, see gdt64 in multiboot2.asm */
    g_idt[vector].ist         = 0;
    g_idt[vector].type_attr   = 0x8E;   /* present | DPL=0 | 64-bit interrupt gate */
    g_idt[vector].offset_mid  = (uint16_t)((handler_addr >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)(handler_addr >> 32);
    g_idt[vector].reserved    = 0;
}

void idt_init(void)
{
    for (int v = 0; v < IDT_ENTRIES; v++) {
        idt_set_gate(v, isr_stub_table[v]);
        g_irq_handlers[v] = NULL;
    }

    static idtr_t idtr;
    idtr.limit = (uint16_t)(sizeof(g_idt) - 1);
    idtr.base  = (uint64_t)&g_idt[0];
    __asm__ volatile ("lidt %0" :: "m"(idtr));

    terminal_puts("[IDT] 256-entry IDT loaded\n");
}

void idt_set_handler(uint8_t vector, void (*handler)(void))
{
    g_irq_handlers[vector] = handler;
}

void pic_remap(uint8_t master_offset, uint8_t slave_offset)
{
    g_pic_master_offset = master_offset;
    g_pic_slave_offset  = slave_offset;

    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11);   /* ICW1: start init sequence, expect ICW4    */
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, master_offset);   /* ICW2: vector offset              */
    outb(PIC2_DATA, slave_offset);
    outb(PIC1_DATA, 0x04);  /* ICW3: master has slave on IRQ2 (0000_0100) */
    outb(PIC2_DATA, 0x02);  /* ICW3: slave's identity is IRQ2 (0000_0010) */
    outb(PIC1_DATA, 0x01);  /* ICW4: 8086 mode                            */
    outb(PIC2_DATA, 0x01);

    outb(PIC1_DATA, mask1); /* restore whatever mask was set before remap */
    outb(PIC2_DATA, mask2);

    terminal_printf("[PIC] Remapped: master=0x%02x slave=0x%02x\n",
        master_offset, slave_offset);
}

void pit_set_frequency(uint32_t hz)
{
    if (hz == 0) hz = 100;
    uint32_t divisor = PIT_BASE_HZ / hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    outb(PIT_COMMAND, 0x36);  /* channel 0, lobyte/hibyte, mode 3 (square wave) */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    terminal_printf("[PIT] Programmed to %u Hz (divisor %u)\n", hz, divisor);
}

void sti(void) { __asm__ volatile ("sti"); }
void cli(void) { __asm__ volatile ("cli"); }

static const char *exception_name(uint64_t vector)
{
    switch (vector) {
    case 0:  return "#DE Divide Error";
    case 1:  return "#DB Debug";
    case 2:  return "NMI";
    case 3:  return "#BP Breakpoint";
    case 4:  return "#OF Overflow";
    case 5:  return "#BR Bound Range Exceeded";
    case 6:  return "#UD Invalid Opcode";
    case 7:  return "#NM Device Not Available";
    case 8:  return "#DF Double Fault";
    case 10: return "#TS Invalid TSS";
    case 11: return "#NP Segment Not Present";
    case 12: return "#SS Stack Fault";
    case 13: return "#GP General Protection Fault";
    case 14: return "#PF Page Fault";
    case 16: return "#MF x87 FP Exception";
    case 17: return "#AC Alignment Check";
    case 18: return "#MC Machine Check";
    case 19: return "#XM SIMD FP Exception";
    default: return "Unknown Exception";
    }
}

/*
 * Called from the common assembly trampoline (kernel/interrupts.S) for
 * every one of the 256 vectors. See that file's header comment for the
 * exact register-save convention this relies on.
 */
/* Interrupts received per PIC line (0-15), for the device manager. */
static uint64_t g_irq_counts[16];

uint64_t irq_count(int irq_line)
{
    return (irq_line >= 0 && irq_line < 16) ? g_irq_counts[irq_line] : 0;
}

void interrupt_dispatch(uint64_t vector, uint64_t error_code, uint64_t *frame)
{
    if (vector < 0x20) {
        /*
         * CPU exception. Three outcomes:
         *   1. A page fault the memory manager can satisfy (first touch of a
         *      demand-paged page, or a page that was swapped out): fix it and
         *      retry the instruction. The process never notices.
         *   2. A fault caused by an ordinary process: terminate that process
         *      and keep the system running.
         *   3. A fault in the kernel / desktop itself: nothing safe can be
         *      done, so report and halt.
         */
        uint64_t rip = frame ? frame[17] : 0;
        uint64_t cr2 = 0;
        if (vector == 14)
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        int vm_result = VM_FAULT_BAD_ADDRESS;
        if (vector == 14) {
            vm_result = vm_handle_fault(proc_current_space(), cr2, error_code);
            if (vm_result == 0)
                return;
        }

        if (!proc_current_is_system()) {
            char reason[96];
            const char *what = exception_name(vector);
            if (vector == 14) {
                what = vm_result == VM_FAULT_READ_ONLY  ? "wrote to read-only memory"
                     : vm_result == VM_FAULT_NO_EXECUTE ? "executed non-executable memory"
                     : vm_result == VM_FAULT_NO_MEMORY  ? "ran out of memory"
                     : "accessed memory it does not own";
                snprintf(reason, sizeof(reason), "%s (PID %u) terminated: %s at 0x%llx",
                         proc_current_name(), kthread_current_pid(), what,
                         (unsigned long long)cr2);
                terminal_printf("[FAULT] faulting instruction at 0x%llx\n", (unsigned long long)rip);
            } else {
                snprintf(reason, sizeof(reason), "%s (PID %u) terminated: %s at 0x%llx",
                         proc_current_name(), kthread_current_pid(), what,
                         (unsigned long long)rip);
            }
            proc_fault_exit(reason);    /* never returns */
        }

        terminal_printf("\n[PANIC] CPU exception %llu: %s (error_code=0x%llx, rip=0x%llx",
            (unsigned long long)vector, exception_name(vector),
            (unsigned long long)error_code, (unsigned long long)rip);
        if (vector == 14)
            terminal_printf(", faulting address=0x%llx", (unsigned long long)cr2);
        terminal_puts(")\n[PANIC] System halted.\n");

        cli();
        for (;;) { __asm__ volatile ("hlt"); }
    }

    /* IRQ range: dispatch to whatever idt_set_handler() registered, then
     * always send EOI — even if nothing was registered for this line —
     * so an unhandled IRQ doesn't leave the PIC thinking it's still busy
     * and stop delivering further interrupts on that line. */
    /*
     * Acknowledge the PIC *before* running the handler. The timer handler
     * may switch to another process and not return here for a long time; if
     * the EOI waited until after it, no further timer interrupts would be
     * delivered and the new process could never be preempted. Interrupts
     * stay disabled until iretq, so acknowledging early cannot nest.
     */
    if (vector >= g_pic_master_offset && vector < (uint32_t)g_pic_master_offset + 8) {
        outb(PIC1_CMD, 0x20);
    } else if (vector >= g_pic_slave_offset && vector < (uint32_t)g_pic_slave_offset + 8) {
        outb(PIC1_CMD, 0x20);   /* cascade: ack master too */
        outb(PIC2_CMD, 0x20);
    }

    /*
     * Preserve the interrupted code's FPU/SSE state. The buffer lives on the
     * interrupted process's own stack, so it also survives a process switch
     * made inside the handler.
     */
    /* fxsave needs a 16-byte aligned buffer. The interrupt stub does not
     * guarantee the C ABI's stack alignment, so align by hand rather than
     * trusting __attribute__((aligned)). */
    uint8_t fpu_raw[512 + 16];
    uint8_t *fpu_state = (uint8_t *)(((uintptr_t)fpu_raw + 15) & ~(uintptr_t)15);
    __asm__ volatile ("fxsave (%0)" : : "r"(fpu_state) : "memory");

    if (vector >= 0x20 && vector < 0x30)
        g_irq_counts[vector - 0x20]++;

    if (g_irq_handlers[vector])
        g_irq_handlers[vector]();

    __asm__ volatile ("fxrstor (%0)" : : "r"(fpu_state) : "memory");
}

/*
 * IRQ0 timer handler — 100 Hz tick from PIT.
 *
 * This only advances the clock. It used to also call desktop_system_render()
 * every second tick, i.e. redraw the entire desktop from inside the interrupt
 * while the main loop was in the middle of drawing the same frame. A frame
 * takes longer than 20 ms to draw, so the interrupt-driven redraw kept
 * restarting on top of the unfinished one: the screen never showed a complete
 * frame and the main loop was starved. Rendering now happens only in
 * kernel_desktop_loop().
 */
void irq0_timer_handler(void)
{
    ai_scheduler_tick();        /* advances g_kstate.uptime_ticks */
}

/*
 * PS/2 keyboard, Scan Code Set 1 (the default set on every PC since the
 * original 8042 controller; QEMU and real hardware both speak this).
 * Translates make/break codes into WM key events via input_key_press()/
 * input_key_release() — previously this handler was a completely empty
 * `{}`, so keyboard input never reached the desktop at all; drivers/input.c
 * only ever drove the PS/2 *mouse* (polled, not interrupt-driven).
 *
 * Extended (0xE0-prefixed) codes are tracked across calls since the prefix
 * and the actual code arrive as separate IRQs.
 */
static int kbd_shift = 0, kbd_ctrl = 0, kbd_alt = 0, kbd_caps = 0, kbd_super = 0;

/* Modifier keys held right now (KMOD_*), for code that needs them outside a
 * key event, e.g. Shift+click. */
int input_current_mods(void);
static int kbd_extended_prefix = 0;

/* Scan Code Set 1 -> ASCII, unshifted (US QWERTY). 0 = no printable char. */
static const char sc1_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
static const char sc1_to_ascii_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* Non-printable key codes, matching the WM's expected uint32_t key space.
 * Printable keys are passed through as their ASCII value; these use a high
 * bit (0x100+) so they never collide with an ASCII byte. */
#define KEY_ESC        0x101
#define KEY_UP         0x102
#define KEY_DOWN       0x103
#define KEY_LEFT       0x104
#define KEY_RIGHT      0x105
#define KEY_DELETE     0x106
#define KEY_HOME       0x107
#define KEY_PGUP       0x109
#define KEY_PGDN       0x10A
#define KEY_END        0x108
#define KEY_F1         0x110  /* F1-F12 = 0x110..0x11B */
#define KEY_TAB        9      /* matches ASCII \t, already produced by sc1_to_ascii[0x0F] */
#define KEY_SUPER_TAP  0x120  /* Super pressed and released alone */

/*
 * IRQ1 (keyboard) and IRQ12 (mouse) share the 8042 output port. The handlers
 * only move raw bytes into a ring (drivers/input.c); decoding happens in
 * input_poll() on the main loop, so the keyboard IRQ can no longer swallow a
 * mouse byte (or the mouse poll a keyboard byte), and no UI code runs in
 * interrupt context.
 */
void irq1_keyboard_handler(void)
{
    ps2_irq_service();
    scheduler_wake_main();      /* handle the input now, not at the next tick */
}

void irq12_mouse_handler(void)
{
    ps2_irq_service();
    scheduler_wake_main();
}

static int kbd_super_used = 0;

/* Decode one keyboard scancode byte. Called from input_poll(). */
void kbd_handle_scancode(uint8_t code)
{
    if (code == 0xE0) {
        kbd_extended_prefix = 1;
        return;
    }

    int is_break = (code & 0x80) != 0;
    uint8_t sc = code & 0x7F;
    int extended = kbd_extended_prefix;
    kbd_extended_prefix = 0;

    /* Modifier tracking (make = pressed, break = released) */
    if (sc == 0x2A || sc == 0x36) { kbd_shift = !is_break; return; }     /* L/R shift */
    if (sc == 0x1D) { kbd_ctrl  = !is_break; return; }                   /* ctrl (also E0-prefixed for R-ctrl; treated the same) */
    if (sc == 0x38) { kbd_alt   = !is_break; return; }                   /* alt (also E0-prefixed for R-alt) */
    if (sc == 0x3A) { if (!is_break) kbd_caps = !kbd_caps; return; }     /* caps lock toggles on make only */
    if (extended && (sc == 0x5B || sc == 0x5C)) {                          /* L/R Windows key */
        /* Tapping Super on its own opens the Start menu (sent on release,
         * and only if no other key was pressed while it was held). */
        if (!is_break) {
            kbd_super = 1;
            kbd_super_used = 0;
        } else {
            kbd_super = 0;
            if (!kbd_super_used)
                input_key_press(KEY_SUPER_TAP, 0);
        }
        return;
    }
    if (kbd_super && !is_break)
        kbd_super_used = 1;

    int mods = (kbd_shift ? KMOD_SHIFT : 0) | (kbd_ctrl ? KMOD_CTRL : 0) |
               (kbd_alt ? KMOD_ALT : 0) | (kbd_caps ? KMOD_CAPS : 0) |
               (kbd_super ? KMOD_SUPER : 0);

    uint32_t key = 0;

    if (extended) {
        switch (sc) {
        case 0x48: key = KEY_UP;     break;
        case 0x50: key = KEY_DOWN;   break;
        case 0x4B: key = KEY_LEFT;   break;
        case 0x4D: key = KEY_RIGHT;  break;
        case 0x53: key = KEY_DELETE; break;
        case 0x47: key = KEY_HOME;   break;
        case 0x4F: key = KEY_END;    break;
        case 0x49: key = KEY_PGUP;   break;
        case 0x51: key = KEY_PGDN;   break;
        default: break;
        }
    } else if (sc == 0x01) {
        key = KEY_ESC;
    } else if (sc >= 0x3B && sc <= 0x44) {
        key = (uint32_t)(KEY_F1 + (sc - 0x3B)); /* F1..F10 */
    } else if (sc == 0x57 || sc == 0x58) {
        key = (uint32_t)(KEY_F1 + 10 + (sc - 0x57)); /* F11, F12 */
    } else if (sc < 128) {
        int use_shift = kbd_shift ^ (kbd_caps && sc1_to_ascii[sc] >= 'a' && sc1_to_ascii[sc] <= 'z');
        char ch = use_shift ? sc1_to_ascii_shift[sc] : sc1_to_ascii[sc];
        if (ch != 0)
            key = (uint32_t)(unsigned char)ch;
    }

    if (key == 0)
        return;

    if (is_break) {
        input_key_release(key, mods);
    } else {
        input_key_press(key, mods);
        /* Also emit a text-input event for plain printable characters so
         * text fields (terminal, settings search box, etc.) don't have to
         * re-derive ASCII from raw key+mods themselves. */
        if (key < 0x100 && key >= 0x20 && !kbd_ctrl && !kbd_alt && !kbd_super)
            input_text_char((char)key);
        else if (key == '\b' || key == '\n' || key == '\t')
            input_text_char((char)key);
    }
}

void irq4_serial_handler(void)
{
    /* Voice bridge (the Python services, polling COM2) is read via polling in
     * voice_serial_poll_char(), which already works without an
     * interrupt-driven path. This handler's only job is to exist so the
     * IRQ line gets acknowledged (interrupt_dispatch() sends EOI
     * regardless of whether a handler is registered, but having an
     * explicit, documented handler here — rather than relying on that
     * implicit behaviour — makes the intent clear for anyone reading
     * interrupt_init() in kernel.c). */
}

void task_executor_run(const char *cmd)
{
    (void)cmd;
}

void system_shutdown(void)
{
    for (;;)
        __asm__ volatile ("hlt");
}

/*
 * syscall_entry_stub() used to be defined here as an empty C function:
 *
 *     void syscall_entry_stub(void) {}
 *
 * syscall_init() in syscall.c writes this function's address into
 * MSR_LSTAR — that's the address the CPU jumps to on every single user
 * `syscall` instruction. An empty function compiles to a bare `ret`: with
 * no saved registers and nothing pushed, that `ret` pops whatever
 * garbage/user-controlled value happens to be on top of whatever stack the
 * CPU was already on (still the *user* stack pointer at this point — the
 * `syscall` instruction does not switch stacks by itself) and jumps to it.
 * That is an immediate crash (or, on real hardware with a malicious or
 * just slightly wrong stack, an exploitable jump) the first time any code
 * ever executes `syscall`.
 *
 * It was harmless only because syscall_init() is never actually called
 * anywhere in the current boot path — kernel.c's jarvis_kernel_main()
 * doesn't call it. The moment someone wires it in, this silently "builds
 * fine" stub becomes a guaranteed crash with no diagnostic.
 *
 * The real fix is a proper assembly trampoline, but that needs supporting
 * infrastructure this codebase doesn't have yet:
 *   1. MSR_STAR — kernel/user CS:SS selectors for SYSCALL/SYSRET, which
 *      needs a real GDT with the four selectors in the exact order
 *      SYSCALL/SYSRET require.
 *   2. swapgs + MSR_KERNEL_GS_BASE — so the trampoline can find a kernel
 *      stack without trusting the user's rsp. No per-CPU base is set up
 *      anywhere in this codebase.
 *   3. A per-CPU (or, for now, single-CPU-is-fine) kernel stack to switch
 *      onto before doing anything else, since `syscall` does NOT switch
 *      stacks for you — you're still running on whatever rsp the calling
 *      usermode code had, which the kernel must never trust.
 *   4. The trampoline itself: swapgs, switch to the kernel stack, push the
 *      registers the C ABI needs preserved + the user's rcx (return RIP)
 *      and r11 (saved RFLAGS) that SYSCALL stashes there, shuffle the
 *      incoming syscall-ABI argument registers (rdi,rsi,rdx,r10,r8,r9)
 *      into the SysV call-ABI registers syscall_handler() expects
 *      (rdi,rsi,rdx,rcx,r8,r9 + a6 on the stack — note rcx is reused for
 *      a different purpose in each convention, so the saved return RIP
 *      must come off rcx before this shuffle clobbers it), call
 *      syscall_handler(), restore rcx/r11, swapgs back, sysretq.
 *
 * Until 1–4 exist and have been validated against a real run (QEMU at
 * minimum — a wrong GDT selector or swapgs without a valid GS base is the
 * kind of bug that triple-faults with zero diagnostic output), do not
 * define syscall_entry_stub() and do not call syscall_init(). Leaving the
 * symbol undefined means the linker will refuse to build anything that
 * tries, which is the correct failure mode here — loud at link time, not
 * silent at runtime.
 */

int input_current_mods(void)
{
    return (kbd_shift ? KMOD_SHIFT : 0) | (kbd_ctrl ? KMOD_CTRL : 0) |
           (kbd_alt ? KMOD_ALT : 0) | (kbd_super ? KMOD_SUPER : 0);
}
