/*
 * JARVIS OS user-mode programming interface (user/jarvis.h)
 *
 * Programs built against this run in user mode (ring 3) in their own address
 * space and reach the kernel only through system calls.
 */
#ifndef JARVIS_USER_H
#define JARVIS_USER_H

typedef unsigned long  size_t;
typedef unsigned long  uint64_t;
typedef long           int64_t;

#define SYS_read     0
#define SYS_write    1
#define SYS_open     2
#define SYS_close    3
#define SYS_brk      12
#define SYS_yield    24
#define SYS_sleep    35
#define SYS_getpid   39
#define SYS_exit     60
#define SYS_mkdir    83
#define SYS_unlink   87
#define SYS_jarvis_speak   500
#define SYS_jarvis_task    502
#define SYS_jarvis_status  503

#define O_READ    1
#define O_WRITE   2
#define O_CREATE  4
#define O_APPEND  8
#define O_TRUNC   16

static inline int64_t syscall3(long nr, uint64_t a, uint64_t b, uint64_t c)
{
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static inline int64_t write(int fd, const void *buf, size_t n) { return syscall3(SYS_write, (uint64_t)fd, (uint64_t)buf, n); }
static inline int64_t read(int fd, void *buf, size_t n)        { return syscall3(SYS_read, (uint64_t)fd, (uint64_t)buf, n); }
static inline int     open(const char *path, int flags)        { return (int)syscall3(SYS_open, (uint64_t)path, (uint64_t)flags, 0); }
static inline int     close(int fd)                            { return (int)syscall3(SYS_close, (uint64_t)fd, 0, 0); }
static inline int     getpid(void)                             { return (int)syscall3(SYS_getpid, 0, 0, 0); }
static inline void    print(const char *s)                     { write(1, s, strlen(s)); }
static inline void    sleep_ms(uint64_t ms)
{
    uint64_t ts[2] = { ms / 1000, (ms % 1000) * 1000000UL };
    syscall3(SYS_sleep, (uint64_t)ts, 0, 0);
}
static inline void    exit(int code)
{
    syscall3(SYS_exit, (uint64_t)code, 0, 0);
    for (;;) { }
}

static inline void print_num(uint64_t v)
{
    char buf[24];
    int i = 23;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    print(&buf[i]);
}

int main(void);

#endif
