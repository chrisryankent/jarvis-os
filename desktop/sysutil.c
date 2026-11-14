/*
 * =============================================================================
 * JARVIS OS — System helpers for the desktop (desktop/sysutil.c)
 *
 * Real-time clock (CMOS), power off / reboot, and time formatting.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "ui.h"

static inline uint8_t io_inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void io_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline void io_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}

/* ---- CMOS real-time clock ------------------------------------------------ */
static uint8_t cmos_read(uint8_t reg)
{
    io_outb(0x70, reg);
    return io_inb(0x71);
}

static int bcd_to_bin(int v)
{
    return (v & 0x0F) + ((v >> 4) * 10);
}

void sys_get_time(sys_time_t *t)
{
    /* Wait out an in-progress update so we never read a half-rolled value. */
    for (int i = 0; i < 100000 && (cmos_read(0x0A) & 0x80); i++)
        ;

    int sec  = cmos_read(0x00);
    int min  = cmos_read(0x02);
    int hour = cmos_read(0x04);
    int day  = cmos_read(0x07);
    int mon  = cmos_read(0x08);
    int year = cmos_read(0x09);
    int regb = cmos_read(0x0B);

    int pm = hour & 0x80;
    hour &= 0x7F;

    if (!(regb & 0x04)) {           /* BCD mode */
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
    }
    if (!(regb & 0x02)) {           /* 12-hour mode */
        hour %= 12;
        if (pm) hour += 12;
    }

    year += 2000;
    if (mon < 1 || mon > 12) mon = 1;
    if (day < 1 || day > 31) day = 1;

    t->year = year; t->month = mon; t->day = day;
    t->hour = hour % 24; t->minute = min % 60; t->second = sec % 60;

    /* Sakamoto's day-of-week algorithm */
    static const int offs[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year - (mon < 3);
    t->weekday = (y + y / 4 - y / 100 + y / 400 + offs[mon - 1] + day) % 7;
}

/* The kernel snprintf ignores width/padding, so pad by hand. */
static char *put2(char *p, int v)
{
    *p++ = (char)('0' + (v / 10) % 10);
    *p++ = (char)('0' + v % 10);
    return p;
}

static int g_clock_24h = 1;
void sys_set_clock_24h(int on) { g_clock_24h = on ? 1 : 0; }
int  sys_clock_24h(void)       { return g_clock_24h; }

/* "14:05", or "2:05 PM" with the 12-hour clock (needs n >= 9). */
void sys_format_clock(char *buf, size_t n)
{
    sys_time_t t;
    if (n < 6) { if (n) buf[0] = 0; return; }
    sys_get_time(&t);
    if (!g_clock_24h && n >= 9) {
        int h = t.hour % 12 ? t.hour % 12 : 12;
        char *p = buf;
        if (h >= 10) *p++ = (char)('0' + h / 10);
        *p++ = (char)('0' + h % 10);
        *p++ = ':';
        p = put2(p, t.minute);
        *p++ = ' '; *p++ = t.hour < 12 ? 'A' : 'P'; *p++ = 'M';
        *p = 0;
        return;
    }
    char *p = put2(buf, t.hour);
    *p++ = ':';
    p = put2(p, t.minute);
    *p = 0;
}

/* The processor's own name, from CPUID (e.g. "Intel(R) Core(TM) i5-8250U"). */
void sys_cpu_name(char *out, size_t n)
{
    uint32_t r[12];
    uint32_t max;
    __asm__ volatile ("cpuid" : "=a"(max) : "a"(0x80000000u) : "ebx", "ecx", "edx");
    if (max < 0x80000004u || n < 2) { snprintf(out, n, "Unknown processor"); return; }
    for (uint32_t i = 0; i < 3; i++)
        __asm__ volatile ("cpuid" : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]), "=c"(r[i * 4 + 2]), "=d"(r[i * 4 + 3])
                          : "a"(0x80000002u + i));
    const char *s = (const char *)r;
    size_t k = 0, i = 0;
    while (i < 48 && s[i] == ' ') i++;
    for (; i < 48 && s[i] && k < n - 1; i++) out[k++] = s[i];    /* (no %.*s in the kernel printf) */
    out[k] = 0;
}

void sys_format_date(char *buf, size_t n)
{
    static const char *const days[7] =
        { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const months[12] =
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    sys_time_t t;
    sys_get_time(&t);
    snprintf(buf, n, "%s %d %s %d", days[t.weekday], t.day,
             months[t.month - 1], t.year);
}

void sys_format_uptime(char *buf, size_t n)
{
    uint64_t secs = g_kstate.uptime_ticks / 100;
    int h = (int)(secs / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    char mm[3], ss[3];
    *put2(mm, m) = 0;
    *put2(ss, s) = 0;
    snprintf(buf, n, "%dh %sm %ss", h, mm, ss);
}

/* ---- Power --------------------------------------------------------------- */
void sys_poweroff(void)
{
    terminal_puts("[POWER] Shutting down\n");
    cli();
    io_outw(0x604, 0x2000);     /* QEMU (ACPI PM1a) */
    io_outw(0xB004, 0x2000);    /* Bochs / old QEMU */
    io_outw(0x4004, 0x3400);    /* VirtualBox       */
    for (;;)
        __asm__ volatile ("hlt");
}

void sys_reboot(void)
{
    terminal_puts("[POWER] Restarting\n");
    cli();
    for (int i = 0; i < 100000 && (io_inb(0x64) & 0x02); i++)
        ;
    io_outb(0x64, 0xFE);        /* pulse the CPU reset line via the 8042 */
    for (;;)
        __asm__ volatile ("hlt");
}

/* Seconds since 1 Jan 1970 (UTC is not known: the clock's own time zone). */
int64_t sys_unix_time(void)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    sys_time_t t;
    sys_get_time(&t);
    int64_t days = 0;
    for (int y = 1970; y < t.year; y++) days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    for (int m = 1; m < t.month; m++) days += mdays[m - 1] + (m == 2 && (t.year % 4 == 0 && (t.year % 100 != 0 || t.year % 400 == 0)));
    days += t.day - 1;
    return days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}
