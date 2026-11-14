/*
 * =============================================================================
 * JARVIS AI OS — System calls (kernel/syscall.c)
 * Language: C17
 *
 * The only way a user-mode program can ask the kernel for anything. Entered
 * through the SYSCALL instruction (kernel/syscall_entry.S).
 *
 * Numbers follow Linux x86-64 where an equivalent exists; JARVIS-specific
 * calls start at 500. Every pointer a program passes is checked against the
 * program's own address space before the kernel touches it, so a program
 * cannot trick the kernel into reading or writing memory it does not own.
 *
 * Errors are returned as negative errno values, as on Linux.
 * =============================================================================
 */

#include "kernel.h"
#include "../desktop/ui.h"
#include <stdio.h>
#include <string.h>

#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_FMASK   0xC0000084

#define E_NOENT   (uint64_t)-2
#define E_BADF    (uint64_t)-9
#define E_FAULT   (uint64_t)-14
#define E_INVAL   (uint64_t)-22
#define E_NOSYS   (uint64_t)-38
#define E_ACCES   (uint64_t)-13

static void (*security_hook)(uint64_t nr, uint64_t *args) = NULL;

void syscall_set_hook(void (*hook)(uint64_t, uint64_t *))
{
    security_hook = hook;
}

static void wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* ---- User memory access -------------------------------------------------- */
static int user_ok(uint64_t addr, uint64_t len, int write)
{
    return vm_user_range_ok(proc_current_space(), addr, len, write);
}

/* Copy a NUL-terminated string from the program. Returns 0, or -1 if any
 * part of it lies outside the program's memory or it is too long. */
static int user_string(char *dst, size_t max, uint64_t src)
{
    for (size_t i = 0; i < max; i++) {
        if ((i == 0 || ((src + i) & 0xFFF) == 0) && !user_ok(src + i, 1, 0))
            return -1;
        dst[i] = *(const char *)(src + i);
        if (!dst[i]) return 0;
    }
    return -1;
}

/* ---- Dispatcher ---------------------------------------------------------- */
uint64_t syscall_handler(uint64_t nr,
                          uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint64_t args[6] = { a1, a2, a3, a4, a5, a6 };
    char path[160];

    if (security_hook)
        security_hook(nr, args);

    switch (nr) {
    case 0: /* read(fd, buf, len) */
        if (!user_ok(a2, a3, 1)) return E_FAULT;
        if (a1 == 0) return (uint64_t)(int64_t)user_stdin_read((char *)a2, (size_t)a3);  /* keyboard, via the Terminal */
        return (uint64_t)(int64_t)vfs_read((int)a1, (void *)a2, (size_t)a3);

    case 1: /* write(fd, buf, len) */
        if (!user_ok(a2, a3, 0)) return E_FAULT;
        if (a1 == 1 || a1 == 2) {                   /* stdout / stderr */
            user_console_write((const char *)a2, (size_t)a3);
            return a3;
        }
        if (a1 < 3) return E_BADF;
        return (uint64_t)(int64_t)vfs_write((int)a1, (const void *)a2, (size_t)a3);

    case 2: /* open(path, flags) — flags: 1 read, 2 write, 4 create, 8 append, 16 truncate */
        if (user_string(path, sizeof(path), a1) != 0) return E_FAULT;
        if (vfs_is_private(path)) return E_ACCES;
        return (uint64_t)(int64_t)vfs_open(path, (uint32_t)a2);

    case 3: /* close(fd) */
        if (a1 < 3) return E_BADF;
        vfs_close((int)a1);
        return 0;

    case 12: /* brk(addr) */
        return user_brk(a1);

    case 8: /* lseek(fd, offset, whence) */
        if (a1 < 3) return E_BADF;
        { int r = vfs_lseek((int)a1, (int64_t)a2, (int)a3); return r < 0 ? E_INVAL : (uint64_t)r; }

    case 61: /* wait4(pid, int *status, ...) : wait for a child to finish */
    {
        int code = 0;
        if (a2 && !user_ok(a2, 4, 1)) return E_FAULT;
        if (!a1 || proc_parent((uint32_t)a1) != kthread_current_pid()) return (uint64_t)-10; /* ECHILD */
        if (proc_wait((uint32_t)a1, &code) != 0) return (uint64_t)-10;
        if (a2) *(int *)a2 = code;
        return a1;
    }

    case 62: /* kill(pid, sig): a program may end itself or its own children */
        if (a1 != kthread_current_pid() && proc_parent((uint32_t)a1) != kthread_current_pid()) return (uint64_t)-1; /* EPERM */
        return proc_kill((uint32_t)a1) == 0 ? 0 : (uint64_t)-3;                                      /* ESRCH */

    case 82: /* rename(from, to) */
    {
        char to[160];
        if (user_string(path, sizeof(path), a1) != 0 || user_string(to, sizeof(to), a2) != 0) return E_FAULT;
        if (vfs_is_private(path) || vfs_is_private(to)) return E_ACCES;
        return vfs_rename(path, to) == 0 ? 0 : E_NOENT;
    }

    case 110: /* getppid() */
        return proc_parent(kthread_current_pid());

    case 201: /* time(NULL): seconds since 1970 (from the PC clock) */
        return (uint64_t)sys_unix_time();

    case 228: /* clock_gettime(clock, struct timespec *) : time since boot */
    {
        if (!user_ok(a2, 16, 1)) return E_FAULT;
        uint64_t t = g_kstate.uptime_ticks;
        ((uint64_t *)a2)[0] = a1 == 0 ? (uint64_t)sys_unix_time() : t / 100;
        ((uint64_t *)a2)[1] = (t % 100) * 10000000ULL;
        return 0;
    }

    case 24: /* sched_yield() */
        kthread_yield();
        return 0;

    case 35: /* nanosleep(const struct timespec *req, rem) */
    {
        if (!user_ok(a1, 16, 0)) return E_FAULT;
        const uint64_t *ts = (const uint64_t *)a1;
        uint64_t ticks = ts[0] * 100 + ts[1] / 10000000ULL;
        kthread_sleep(ticks ? (uint32_t)ticks : 1);
        return 0;
    }

    case 39: /* getpid() */
        return (uint64_t)kthread_current_pid();

    case 60:  /* exit(code) */
    case 231: /* exit_group(code) */
        kthread_exit(a1);                           /* never returns */
        return 0;

    case 83: /* mkdir(path) */
        if (user_string(path, sizeof(path), a1) != 0) return E_FAULT;
        if (vfs_is_private(path)) return E_ACCES;
        return vfs_mkdir(path) == 0 ? 0 : E_INVAL;

    case 87: /* unlink(path) */
        if (user_string(path, sizeof(path), a1) != 0) return E_FAULT;
        if (vfs_is_private(path)) return E_ACCES;
        return vfs_delete(path) == 0 ? 0 : E_NOENT;

    /* ---- JARVIS calls ---------------------------------------------------- */
    case 500: /* jarvis_speak(text, len) */
    {
        char text[256];
        size_t len = a2 < sizeof(text) - 1 ? (size_t)a2 : sizeof(text) - 1;
        if (!user_ok(a1, len, 0)) return E_FAULT;
        memcpy(text, (const void *)a1, len);
        text[len] = '\0';
        tts_speak(text, len);
        return 0;
    }

    case 502: /* jarvis_task(command): same route as a spoken command */
    {
        static ipc_message_t msg;
        char cmd[200];
        if (user_string(cmd, sizeof(cmd), a1) != 0) return E_FAULT;
        memset(&msg, 0, sizeof(msg));
        msg.type = IPC_VOICE_INPUT;
        msg.payload_len = strlen(cmd);
        memcpy(msg.payload, cmd, msg.payload_len);
        ipc_send(&msg);
        return 0;
    }

    case 503: /* jarvis_status(buf, max) */
    {
        char status[160];
        int n = snprintf(status, sizeof(status),
            "{\"uptime\":%llu,\"ai_ready\":%d,\"speech_ready\":%d,\"ai_managing\":%d}",
            (unsigned long long)(g_kstate.uptime_ticks / 100),
            g_kstate.ai_ready, g_kstate.speech_ready, governor_ai_online());
        if (a2 == 0 || !user_ok(a1, a2, 1)) return E_FAULT;
        size_t len = (size_t)n < a2 - 1 ? (size_t)n : (size_t)a2 - 1;
        memcpy((void *)a1, status, len);
        ((char *)a1)[len] = '\0';
        return len;
    }

    case 504: /* jarvis_notify(title, text) */
    {
        char title[64], text[160];
        if (user_string(title, sizeof(title), a1) != 0 || user_string(text, sizeof(text), a2) != 0) return E_FAULT;
        notification_show(title, text, NOTIF_INFO, 5000);
        return 0;
    }

    case 520: /* spawn(path): start another program; returns its PID */
    {
        if (user_string(path, sizeof(path), a1) != 0) return E_FAULT;
        if (vfs_is_private(path)) return E_ACCES;
        uint32_t pid = user_exec(path);
        return pid ? pid : E_NOENT;
    }

    case 521: /* listdir(path, buf, max): "d name" / "f name size" lines */
    {
        static char listing[16384];
        if (user_string(path, sizeof(path), a1) != 0 || !user_ok(a2, a3, 1) || !a3) return E_FAULT;
        if (vfs_is_private(path)) return E_ACCES;
        if (!vfs_is_dir(path) || vfs_list(path, listing, sizeof(listing)) < 0) return E_NOENT;
        size_t dl = strlen(path), out = 0;
        char *dst = (char *)a2;
        for (const char *q = listing; *q;) {
            const char *eol = strchr(q, '\n');
            if (!eol) eol = q + strlen(q);
            const char *full = strchr(q, '/');
            if (full && full < eol && (size_t)(eol - full) > dl + 1 && !strncmp(full, path, dl) && full[dl] == '/' &&
                !memchr(full + dl + 1, '/', (size_t)(eol - full) - dl - 1) && strncmp(full, "/jarvis/system", 14)) {
                char line[200], fp[160];
                size_t fl = (size_t)(eol - full) < 159 ? (size_t)(eol - full) : 159;
                memcpy(fp, full, fl); fp[fl] = 0;
                int n = !strncmp(q, "[dir]", 5) ? snprintf(line, sizeof(line), "d %s\n", fp + dl + 1)
                                                 : snprintf(line, sizeof(line), "f %s %ld\n", fp + dl + 1, vfs_size(fp));
                if (out + (size_t)n + 1 > a3) break;
                memcpy(dst + out, line, (size_t)n);
                out += (size_t)n;
            }
            q = *eol ? eol + 1 : eol;
        }
        dst[out] = 0;
        return out;
    }

    case 522: /* filesize(path) */
        if (user_string(path, sizeof(path), a1) != 0) return E_FAULT;
        if (vfs_is_private(path)) return E_ACCES;
        { long s = vfs_size(path); return s < 0 ? E_NOENT : (uint64_t)s; }

    case 510: /* device_claim(id): exclusive use of a device */
        return devmgr_claim((int)a1, kthread_current_pid()) == 0 ? 0 : (uint64_t)-16;   /* EBUSY */

    case 511: /* device_release(id) */
        return devmgr_release((int)a1, kthread_current_pid()) == 0 ? 0 : E_INVAL;

    default:
        /* fork, execve, mmap and the rest are not implemented; say so
         * rather than pretending they worked. */
        return E_NOSYS;
    }
}

extern void syscall_entry_stub(void);

void syscall_init(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_EFER));
    wrmsr(MSR_EFER, (((uint64_t)hi << 32) | lo) | 1);       /* enable SYSCALL */

    /* SYSCALL loads cs=0x08, ss=0x10. SYSRET loads ss=0x10+8 (0x18) and
     * cs=0x10+16 (0x20), both at privilege level 3. */
    wrmsr(MSR_STAR, (0x0010ULL << 48) | (0x0008ULL << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry_stub);
    wrmsr(MSR_FMASK, 0x200 | 0x400);        /* enter with interrupts off, DF clear */
}
