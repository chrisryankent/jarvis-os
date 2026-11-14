/*
 * =============================================================================
 * JARVIS AI OS — Dual-mode operation (kernel/usermode.c)
 * Language: C17
 *
 * Kernel mode (ring 0) and user mode (ring 3):
 *
 *   GDT / TSS    kernel and user code/data segments; the TSS tells the CPU
 *                which kernel stack to switch to when an interrupt or fault
 *                arrives while a program is in user mode.
 *   loader       reads a static ELF64 program from the VFS into a fresh
 *                address space (text read-only + executable, data writable +
 *                non-executable), gives it a stack and a heap.
 *   mode switch  usermode_enter() drops to ring 3; the SYSCALL instruction,
 *                interrupts and faults bring the CPU back to ring 0.
 *
 * A user program cannot touch kernel memory or run privileged instructions;
 * if it tries, the CPU faults and only that program is terminated.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

void gdt_flush(void *gdt_pointer);
void usermode_enter(uint64_t entry, uint64_t user_stack);
extern uint64_t syscall_kernel_rsp;

/* ---- GDT + TSS ----------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

static uint64_t g_gdt[7];
static tss_t    g_tss;
static struct __attribute__((packed)) { uint16_t limit; uint64_t base; } g_gdt_ptr;

void usermode_init(void)
{
    uint64_t base = (uint64_t)&g_tss;
    uint64_t limit = sizeof(g_tss) - 1;

    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.iomap_base = sizeof(g_tss);       /* no I/O ports for user mode */

    g_gdt[0] = 0;
    g_gdt[1] = 0x00AF9A000000FFFFULL;       /* 0x08 kernel code            */
    g_gdt[2] = 0x00AF92000000FFFFULL;       /* 0x10 kernel data            */
    g_gdt[3] = 0x00AFF2000000FFFFULL;       /* 0x18 user data   (ring 3)   */
    g_gdt[4] = 0x00AFFA000000FFFFULL;       /* 0x20 user code   (ring 3)   */
    g_gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
               (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    g_gdt[6] = base >> 32;                  /* 0x28 TSS (two slots)        */

    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base = (uint64_t)g_gdt;
    gdt_flush(&g_gdt_ptr);

    syscall_init();
    terminal_puts("[USER] Dual-mode ready: ring 0 kernel, ring 3 user, SYSCALL enabled\n");
}

/* Called by the scheduler on every process switch. */
void usermode_set_kernel_stack(uint64_t top)
{
    g_tss.rsp0 = top;
    syscall_kernel_rsp = top;
}

/* ---- Program output ------------------------------------------------------
 * A user program's stdout goes into this ring; the desktop process drains it
 * into the Terminal window (programs never touch the UI themselves).
 */
#define CONSOLE_SIZE 8192
static char g_console[CONSOLE_SIZE];
static uint32_t g_con_head = 0, g_con_tail = 0;

static uint64_t um_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void um_irq_restore(uint64_t flags)
{
    if (flags & 0x200) __asm__ volatile ("sti");
}

void user_console_write(const char *buf, size_t len)
{
    uint64_t irq = um_irq_save();
    for (size_t i = 0; i < len; i++) {
        uint32_t next = (g_con_head + 1) % CONSOLE_SIZE;
        if (next == g_con_tail)                 /* full: drop the oldest */
            g_con_tail = (g_con_tail + 1) % CONSOLE_SIZE;
        g_console[g_con_head] = buf[i];
        g_con_head = next;
    }
    um_irq_restore(irq);
}

size_t user_console_read(char *buf, size_t max)
{
    uint64_t irq = um_irq_save();
    size_t n = 0;
    while (n < max && g_con_tail != g_con_head) {
        buf[n++] = g_console[g_con_tail];
        g_con_tail = (g_con_tail + 1) % CONSOLE_SIZE;
    }
    um_irq_restore(irq);
    return n;
}

/* ---- Keyboard input for the program running in the Terminal -------------- */
#define STDIN_SIZE 1024
static char g_stdin[STDIN_SIZE];
static uint32_t g_in_head = 0, g_in_tail = 0;
static uint32_t g_foreground = 0;

void user_set_foreground(uint32_t pid)
{
    uint64_t irq = um_irq_save();
    g_foreground = pid;
    g_in_head = g_in_tail = 0;
    um_irq_restore(irq);
}

uint32_t user_foreground(void)
{
    if (g_foreground && !proc_exists(g_foreground)) g_foreground = 0;
    return g_foreground;
}

void user_stdin_push(const char *text, size_t len)
{
    uint64_t irq = um_irq_save();
    for (size_t i = 0; i < len; i++) {
        uint32_t next = (g_in_head + 1) % STDIN_SIZE;
        if (next == g_in_tail) break;
        g_stdin[g_in_head] = text[i];
        g_in_head = next;
    }
    um_irq_restore(irq);
}

/* read(0): one line at a time, waiting until the user presses Enter. Only
 * the foreground program gets input; others read end-of-file. */
int user_stdin_read(char *buf, size_t max)
{
    if (kthread_current_pid() != g_foreground || max == 0) return 0;
    proc_set_note("keyboard");
    for (;;) {
        uint64_t irq = um_irq_save();
        int have_line = 0;
        for (uint32_t i = g_in_tail; i != g_in_head; i = (i + 1) % STDIN_SIZE)
            if (g_stdin[i] == '\n') { have_line = 1; break; }
        if (have_line) {
            size_t n = 0;
            while (g_in_tail != g_in_head && n < max) {
                char c = g_stdin[g_in_tail];
                g_in_tail = (g_in_tail + 1) % STDIN_SIZE;
                buf[n++] = c;
                if (c == '\n') break;
            }
            um_irq_restore(irq);
            proc_set_note(0);
            return (int)n;
        }
        um_irq_restore(irq);
        if (kthread_current_pid() != g_foreground) { proc_set_note(0); return 0; }
        kthread_sleep(2);
    }
}

/* ---- Per-program state --------------------------------------------------- */
#define USER_MAX          16
#define USER_STACK_TOP    0x00007FFFFFFFF000ULL
#define USER_STACK_PAGES  64                    /* 256 KB */
#define USER_HEAP_BASE    (VM_USER_BASE + 0x40000000ULL)
#define USER_HEAP_PAGES   4096                  /* 16 MB, demand paged */

static struct {
    uint32_t pid;
    char     path[128];
    uint64_t brk;
} g_user[USER_MAX];

static int user_slot(uint32_t pid)
{
    for (int i = 0; i < USER_MAX; i++)
        if (g_user[i].pid == pid) return i;
    return -1;
}

/* brk(): grow or shrink the program's heap. Returns the (new) break. */
uint64_t user_brk(uint64_t want)
{
    int s = user_slot(kthread_current_pid());
    if (s < 0) return 0;
    if (want >= USER_HEAP_BASE && want <= USER_HEAP_BASE + (uint64_t)USER_HEAP_PAGES * 4096)
        g_user[s].brk = want;
    return g_user[s].brk;
}

/* ---- ELF64 loader -------------------------------------------------------- */
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_ehdr_t;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf_phdr_t;

int user_is_program(const char *path)
{
    char head[8];
    return vfs_read_file(path, head, sizeof(head)) >= 4 &&
           head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F';
}

/* Loads the image into address space sp. Returns the entry point, or 0. */
static uint64_t elf_load(int sp, const uint8_t *img, size_t size, const char **why)
{
    const elf_ehdr_t *eh = (const elf_ehdr_t *)img;

    *why = "not a program this system can run (needs a 64-bit static ELF)";
    if (size < sizeof(*eh) || eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != 2 || eh->type != 2 || eh->machine != 0x3E ||
        eh->phentsize != sizeof(elf_phdr_t) ||
        eh->phoff + (uint64_t)eh->phnum * sizeof(elf_phdr_t) > size)
        return 0;

    for (int i = 0; i < eh->phnum; i++) {
        const elf_phdr_t *ph = (const elf_phdr_t *)(img + eh->phoff + i * sizeof(elf_phdr_t));
        if (ph->type != 1 || ph->memsz == 0)        /* PT_LOAD only */
            continue;

        *why = "program is damaged or linked for a different system";
        if (ph->filesz > ph->memsz || ph->offset + ph->filesz > size ||
            ph->vaddr < VM_USER_BASE || ph->vaddr + ph->memsz >= USER_HEAP_BASE)
            return 0;

        uint64_t start = ph->vaddr & ~0xFFFULL;
        uint32_t pages = (uint32_t)((ph->vaddr + ph->memsz - start + 0xFFF) >> 12);
        uint32_t flags = VM_USER | ((ph->flags & 2) ? VM_WRITE : 0) | ((ph->flags & 1) ? VM_EXEC : 0);

        /* Writable while the kernel copies the contents in, then locked down
         * to what the program asked for (code ends up read-only). */
        *why = "not enough memory to load the program";
        if (vm_region_add(sp, start, pages, VM_USER | VM_WRITE, "program") != 0)
            return 0;
        memcpy((void *)ph->vaddr, img + ph->offset, ph->filesz);
        vm_protect(sp, start, pages, flags);
    }
    return eh->entry;
}

/* Body of every user process: runs in kernel mode just long enough to build
 * the address space, then drops to user mode for good. */
static void user_process_main(void)
{
    uint32_t pid = kthread_current_pid();
    int s = user_slot(pid);
    char msg[160];
    const char *why = "could not be started";

    if (s < 0) return;
    const char *path = g_user[s].path;
    const char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;

    long size = vfs_size(path);
    uint8_t *img = size > 0 ? (uint8_t *)kmalloc((size_t)size + 1) : NULL;
    uint64_t entry = 0;
    int sp = -1;

    if (img && vfs_read_file(path, (char *)img, (size_t)size + 1) == size) {
        sp = vm_space_create(pid);
        if (sp >= 0 && proc_use_space(sp) == 0)
            entry = elf_load(sp, img, (size_t)size, &why);
        else
            why = "not enough memory for a new address space";
    } else {
        why = "file could not be read";
    }
    if (img) kfree(img);

    if (!entry ||
        vm_region_add(sp, USER_STACK_TOP - (uint64_t)USER_STACK_PAGES * 4096,
                      USER_STACK_PAGES, VM_USER | VM_WRITE, "stack") != 0 ||
        vm_region_add(sp, USER_HEAP_BASE, USER_HEAP_PAGES, VM_USER | VM_WRITE, "heap") != 0) {
        snprintf(msg, sizeof(msg), "%s: %s\n", leaf, why);
        user_console_write(msg, strlen(msg));
        g_user[s].pid = 0;
        return;                     /* process exits; its address space is freed */
    }

    g_user[s].brk = USER_HEAP_BASE;
    proc_mark_user();
    /* argc = 0, argv = { NULL }, envp = { NULL } on a zero-filled stack page */
    usermode_enter(entry, USER_STACK_TOP - 64);
}

/* Start a program from the VFS as a new user-mode process. Returns its PID,
 * or 0 on failure. */
uint32_t user_exec(const char *path)
{
    if (!path || vfs_is_dir(path) || !user_is_program(path))
        return 0;

    const char *leaf = strrchr(path, '/');
    uint64_t irq = um_irq_save();       /* the child must not run before its
                                           slot is filled in */
    int s = -1;
    for (int i = 0; i < USER_MAX; i++) {
        proc_info_t unused;
        (void)unused;
        if (g_user[i].pid == 0 || !proc_exists(g_user[i].pid)) { s = i; break; }
    }
    uint32_t pid = s >= 0 ? proc_spawn(leaf ? leaf + 1 : path, user_process_main, PRIO_USER, 0) : 0;
    if (pid) {
        g_user[s].pid = pid;
        g_user[s].brk = 0;
        strncpy(g_user[s].path, path, sizeof(g_user[s].path) - 1);
        g_user[s].path[sizeof(g_user[s].path) - 1] = '\0';
    }
    um_irq_restore(irq);
    return pid;
}

/* ---- Bundled programs ---------------------------------------------------- */
typedef struct { const char *name; const uint8_t *start; const uint8_t *end; } bundled_t;
extern const uint8_t user_prog_hello_start[], user_prog_hello_end[];
extern const uint8_t user_prog_kernelpoke_start[], user_prog_kernelpoke_end[];
extern const uint8_t user_prog_privileged_start[], user_prog_privileged_end[];
extern const uint8_t user_prog_counter_start[], user_prog_counter_end[];
extern const uint8_t user_prog_ask_start[], user_prog_ask_end[];
extern const uint8_t user_prog_launcher_start[], user_prog_launcher_end[];

/* Copies the programs built from user/ into /jarvis/bin so they can be run,
 * copied and deleted like any other file. */
void usermode_install_programs(void)
{
    const bundled_t progs[] = {
        { "hello",      user_prog_hello_start,      user_prog_hello_end },
        { "counter",    user_prog_counter_start,    user_prog_counter_end },
        { "kernelpoke", user_prog_kernelpoke_start, user_prog_kernelpoke_end },
        { "privileged", user_prog_privileged_start, user_prog_privileged_end },
        { "ask",        user_prog_ask_start,        user_prog_ask_end },
        { "launcher",   user_prog_launcher_start,   user_prog_launcher_end },
    };
    for (size_t i = 0; i < sizeof(progs) / sizeof(progs[0]); i++) {
        char path[64];
        snprintf(path, sizeof(path), "/jarvis/bin/%s", progs[i].name);
        vfs_write_file(path, progs[i].start, (size_t)(progs[i].end - progs[i].start));
    }
    terminal_puts("[USER] Installed bundled programs in /jarvis/bin\n");
}
