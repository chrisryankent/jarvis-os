/*
 * =============================================================================
 * JARVIS AI OS — Syscall Handler  (kernel/syscall.c)
 * Language: C17
 *
 * Uses x86_64 SYSCALL/SYSRET fast-path (MSR_LSTAR).
 * Every syscall is intercepted by the security audit hook BEFORE
 * it reaches the real implementation.
 *
 * Syscall table mirrors a subset of Linux (for app compatibility).
 * AI-specific syscalls start at number 500:
 *   500 — jarvis_speak(text, len)         ask JARVIS to say something
 *   501 — jarvis_ask(prompt, resp, max)   query the AI brain
 *   502 — jarvis_task(cmd, len)           run a voice command
 *   503 — jarvis_status(buf, max)         get OS status JSON
 * =============================================================================
 */

#include "kernel.h"
#include <stdio.h>
#include <string.h>

/* ---- MSR numbers --------------------------------------------------------- */
#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_FMASK   0xC0000084

/* ---- Syscall hook -------------------------------------------------------- */
static void (*security_hook)(uint64_t nr, uint64_t *args) = NULL;

void syscall_set_hook(void (*hook)(uint64_t, uint64_t *))
{
    security_hook = hook;
}

/* ---- Write to MSR -------------------------------------------------------- */
static void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

/* ---- Syscall entry (called from ASM stub in syscall_entry.asm) ----------- */
/*
 * Registers on entry (Linux x86_64 ABI):
 *   rax = syscall number
 *   rdi = arg1, rsi = arg2, rdx = arg3, r10 = arg4, r8 = arg5, r9 = arg6
 * Return value in rax.
 */
uint64_t syscall_handler(uint64_t nr,
                          uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint64_t args[6] = { a1, a2, a3, a4, a5, a6 };

    /* Security audit FIRST — before any work is done */
    if (security_hook)
        security_hook(nr, args);

    /* Dispatch */
    switch (nr) {
    /* ---- Standard Linux-compatible syscalls ----------------------------- */
    case 0: /* read */
        return (uint64_t)vfs_read((int)a1, (void *)a2, (size_t)a3);

    case 1: /* write */
        return (uint64_t)vfs_write((int)a1, (const void *)a2, (size_t)a3);

    case 2: /* open */
        return (uint64_t)vfs_open((const char *)a1, (uint32_t)a2);

    case 3: /* close */
        vfs_close((int)a1);
        return 0;

    case 12: /* brk — heap management */
        return (uint64_t)kmalloc((size_t)a1); /* stub */

    case 60: /* exit */
        terminal_printf("[SYSCALL] Process exited: code %llu\n", a1);
        kthread_exit(a1); /* never returns */
        return 0;

    case 83: /* mkdir */
        return (uint64_t)vfs_mkdir((const char *)a1);

    case 85: /* creat */
    {
        const char *path = (const char *)a1;
        int fd = vfs_create_file(path);
        if (fd < 0)
            return (uint64_t)-1;
        return (uint64_t)vfs_open(path, 0x02 | 0x04 | 0x10);
    }

    case 57: /* fork */
        /* AI OS discourages fork — suggest async task instead */
        terminal_puts("[SYSCALL] fork() — redirecting to kthread\n");
        return 0; /* stub */

    case 59: /* execve */
        return 0; /* stub — full ELF loader needed */

    case 39: /* getpid */
        return (uint64_t)kthread_current_pid();

    case 158: /* arch_prctl */
        return 0;

    case 231: /* exit_group */
        kthread_exit(a1); /* never returns */
        return 0;

    /* ---- JARVIS AI syscalls (500–599) ---------------------------------- */

    case 500: /* jarvis_speak(text_ptr, len) */
    {
        const char *text = (const char *)a1;
        size_t       len  = (size_t)a2;
        tts_speak(text, len);
        return 0;
    }

    case 501: /* jarvis_ask(prompt_ptr, resp_ptr, resp_max) */
    {
        const char *prompt   = (const char *)a1;
        char       *resp_buf = (char *)a2;
        size_t      max      = (size_t)a3;

        /* Send to AI brain, block until response (max 30 s) */
        ipc_message_t req = {};
        req.type = IPC_AI_REQUEST;
        size_t plen = strlen(prompt);
        if (plen >= sizeof(req.payload)) plen = sizeof(req.payload) - 1;
        memcpy(req.payload, prompt, plen);
        req.payload_len = plen;
        ipc_send(&req);

        /* Wait for AI response (poll with timeout) */
        uint64_t deadline = g_kstate.uptime_ticks + 3000; /* 30 s @ 100 Hz */
        while (g_kstate.uptime_ticks < deadline) {
            if (ipc_has_message(IPC_AI_RESPONSE)) {
                ipc_message_t resp = ipc_recv_next();
                size_t rlen = resp.payload_len;
                if (rlen >= max) rlen = max - 1;
                memcpy(resp_buf, resp.payload, rlen);
                resp_buf[rlen] = '\0';
                return 0;
            }
            __asm__ volatile ("pause");
        }
        strncpy(resp_buf, "[timeout]", max);
        return -1;
    }

    case 502: /* jarvis_task(cmd_ptr, len) */
    {
        const char *cmd = (const char *)a1;
        task_executor_run(cmd);
        return 0;
    }

    case 503: /* jarvis_status(buf_ptr, max) */
    {
        char   *buf = (char *)a1;
        size_t  max = (size_t)a2;
        snprintf(buf, max,
            "{\"uptime\":%llu,\"ai_ready\":%d,\"speech_ready\":%d,"
            "\"model_pool_mb\":%llu}",
            (unsigned long long)(g_kstate.uptime_ticks / 100),
            g_kstate.ai_ready,
            g_kstate.speech_ready,
            (unsigned long long)(g_kstate.model_pool_size / (1024ULL * 1024)));
        return 0;
    }

    default:
        terminal_printf("[SYSCALL] Unhandled: %llu\n", nr);
        return (uint64_t)-1;
    }
}

/* ---- Install SYSCALL handler via LSTAR MSR ------------------------------- */
/*
 * The actual ASM entry stub must:
 *   1. swapgs             (switch to kernel GS)
 *   2. Save user RSP, load kernel RSP
 *   3. Save all registers
 *   4. call syscall_handler(rax, rdi, rsi, rdx, r10, r8, r9)
 *   5. Restore registers
 *   6. sysretq
 * This is defined in kernel/syscall_entry.asm.
 */
extern void syscall_entry_stub(void); /* defined in ASM */

void syscall_init(void)
{
    terminal_puts("[SYSCALL] Installing syscall handler\n");

    /* Enable SYSCALL/SYSRET in EFER */
    uint32_t efer_lo, efer_hi;
    __asm__ volatile ("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(MSR_EFER));
    efer_lo |= 1; /* SCE — System Call Extensions */
    wrmsr(MSR_EFER, ((uint64_t)efer_hi << 32) | efer_lo);

    /* STAR: kernel CS = 0x08, user CS = 0x18 */
    wrmsr(MSR_STAR, ((uint64_t)0x0018000800000000ULL));

    /* LSTAR: entry point for SYSCALL */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry_stub);

    /* FMASK: clear IF on syscall entry (disable interrupts momentarily) */
    wrmsr(MSR_FMASK, (1 << 9)); /* bit 9 = IF */

    terminal_puts("[SYSCALL] Ready\n");
}
