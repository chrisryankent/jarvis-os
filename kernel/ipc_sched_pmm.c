/*
 * =============================================================================
 * JARVIS AI OS — Kernel IPC + Scheduler + PMM  (kernel/ipc_sched_pmm.c)
 * Language: C17
 *
 * Implements the three core kernel primitives:
 *   1. Lock-free IPC ring buffer — message passing between subsystems
 *   2. AI-aware process scheduler — MLFQ with AI priority class
 *   3. Physical memory manager — bitmap allocator with contiguous block support
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stddef.h>
#include <stdio.h>

/* "Interrupts off" is the kernel lock on this single-CPU system. These save
 * and restore the previous state, so they nest safely (unlike cli()/sti()). */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    if (flags & 0x200)
        __asm__ volatile ("sti");
}


/* ============================================================================
 * 1. IPC RING BUFFER
 * Thread-safe single-producer multi-consumer ring buffer.
 * Used by: kernel→speech, kernel→AI, kernel→security, user→kernel
 * ============================================================================ */

#define IPC_RING_SIZE  256

static ipc_message_t ipc_ring[IPC_RING_SIZE];
static volatile uint32_t ipc_head  = 0;
static volatile uint32_t ipc_tail  = 0;

/* Separate queues per message type for O(1) type-filtered peek */
#define IPC_TYPE_MAX 16
static uint32_t type_queues[IPC_TYPE_MAX][IPC_RING_SIZE];
static volatile uint32_t type_head[IPC_TYPE_MAX] = {};
static volatile uint32_t type_tail[IPC_TYPE_MAX] = {};

static void ipc_drop_type_front(uint8_t type, uint32_t ring_idx)
{
    uint8_t t = type & (IPC_TYPE_MAX - 1);
    while (type_tail[t] != type_head[t]) {
        uint32_t front = type_queues[t][type_tail[t]];
        if (front == ring_idx || ipc_ring[front].type == 0) {
            type_tail[t] = (type_tail[t] + 1) % IPC_RING_SIZE;
        } else {
            break;
        }
    }
}

void ipc_send(ipc_message_t *msg)
{
    uint64_t irqf = irq_save();

    uint32_t next = (ipc_head + 1) % IPC_RING_SIZE;
    if (next == ipc_tail) {
        /* Ring full — evict oldest non-realtime message */
        ipc_tail = (ipc_tail + 1) % IPC_RING_SIZE;
    }
    ipc_ring[ipc_head] = *msg;

    /* Also push to per-type queue */
    uint8_t t = msg->type & (IPC_TYPE_MAX - 1);
    uint32_t tn = (type_head[t] + 1) % IPC_RING_SIZE;
    if (tn != type_tail[t]) {
        type_queues[t][type_head[t]] = ipc_head;
        type_head[t] = tn;
    }

    ipc_head = next;
    irq_restore(irqf);
}

ipc_message_t ipc_recv_next(void)
{
    ipc_message_t empty = {};
    while (ipc_tail != ipc_head) {
        uint32_t ring_idx = ipc_tail;
        ipc_message_t msg = ipc_ring[ring_idx];
        ipc_tail = (ipc_tail + 1) % IPC_RING_SIZE;

        if (msg.type == 0)
            continue;

        ipc_ring[ring_idx].type = 0;
        ipc_drop_type_front((uint8_t)msg.type, ring_idx);
        return msg;
    }
    return empty;
}

int ipc_has_message(uint8_t type)
{
    uint8_t t = type & (IPC_TYPE_MAX - 1);
    while (type_head[t] != type_tail[t]) {
        uint32_t ring_idx = type_queues[t][type_tail[t]];
        if (ipc_ring[ring_idx].type == type)
            break;
        type_tail[t] = (type_tail[t] + 1) % IPC_RING_SIZE;
    }
    return type_head[t] != type_tail[t];
}

/* Drain type-specific queue and return matching message */
ipc_message_t ipc_recv_type(uint8_t type)
{
    ipc_message_t empty = {};
    uint8_t t = type & (IPC_TYPE_MAX - 1);
    if (type_head[t] == type_tail[t]) return empty;

    uint32_t ring_idx = type_queues[t][type_tail[t]];
    type_tail[t] = (type_tail[t] + 1) % IPC_RING_SIZE;
    ipc_message_t msg = ipc_ring[ring_idx];
    if (msg.type != type)
        return empty;
    ipc_ring[ring_idx].type = 0;
    return msg;
}

/* ============================================================================
 * 2. PROCESS MANAGEMENT
 *
 * Preemptive, priority-based round-robin scheduler for kernel threads.
 *
 *   creation      kthread_create() / proc_spawn()
 *   termination   kthread_exit() (self), proc_kill() (another process)
 *   scheduling    scheduler_tick() from the 100 Hz timer: the highest
 *                 priority READY process runs; equal priorities take turns
 *   suspension    proc_suspend() / proc_resume()
 *   sleeping      kthread_sleep()
 *   sync          kmutex_* and ksem_* (blocking, with wait queues)
 *   IPC           proc_send() / proc_recv() per-process mailboxes (plus the
 *                 system-wide typed message ring in section 1)
 *   deadlock      kmutex_lock() walks the wait-for chain and refuses a lock
 *                 that would close a cycle
 *
 * The kernel main loop (desktop) is process 1. When nothing is runnable the
 * scheduler halts the CPU until the next interrupt.
 *
 * Locking: single CPU, so "interrupts off" is the kernel lock. Every entry
 * point saves the interrupt flag, disables interrupts, and restores it.
 * ============================================================================ */
#define MAX_PROCESSES   64
#define PROC_STACK_SIZE 65536
#define MAILBOX_SIZE    8
#define MAX_MUTEXES     32
#define MAX_SEMAPHORES  32

typedef struct process {
    uint32_t       pid;             /* 0 = slot free */
    proc_prio_t    priority;
    proc_state_t   state;
    uint32_t       flags;           /* PROC_FLAG_* */
    char           name[32];
    uint64_t       rsp;             /* saved stack pointer */
    void         (*entry)(void);
    uint64_t       cpu_ticks;
    uint64_t       start_tick;
    uint32_t       quantum_rem;
    uint64_t       wake_tick;       /* PROC_SLEEPING: when to wake */
    int            suspended;       /* held off the CPU until resumed */
    proc_state_t   state_before_suspend;
    int            wait_kind;       /* PROC_WAIT_* while PROC_BLOCKED */
    int            wait_id;         /* mutex / semaphore index */
    int            vm_space;        /* address space id, 0 = kernel's */
    uint64_t       cr3;             /* its page table, 0 = kernel's   */
    struct { uint32_t from; uint64_t value; } mailbox[MAILBOX_SIZE];
    int            mail_head, mail_count;
    uint8_t        stack[PROC_STACK_SIZE];
} process_t;

enum { PROC_WAIT_NONE = 0, PROC_WAIT_MUTEX, PROC_WAIT_SEM, PROC_WAIT_MAIL };

typedef struct { int used; char name[24]; uint32_t owner; } kmutex_t;
typedef struct { int used; char name[24]; int count; } ksem_t;

static process_t  proc_pool[MAX_PROCESSES];
static process_t *current_proc = NULL;
static uint32_t   next_pid = 1;
static int        sched_ready = 0;      /* scheduler_init() has run          */
static int        sched_idle = 0;       /* CPU is halted waiting for work    */
static uint64_t   sched_idle_ticks = 0;
static uint64_t   sched_switches = 0;
static kmutex_t   mutexes[MAX_MUTEXES];
static ksem_t     semaphores[MAX_SEMAPHORES];
#define SCHED_EVENTS 8
static char       sched_events[SCHED_EVENTS][96];   /* notable events for the UI */
static int        sched_event_head = 0, sched_event_count = 0;

extern void context_switch_asm(uint64_t *save_rsp_here, uint64_t *load_rsp_from);

static uint32_t quantum_ticks_for(proc_prio_t prio)
{
    switch (prio) {
    case PRIO_AI:     return 4;     /* long slices for inference */
    case PRIO_SYSTEM: return 2;
    default:          return 1;
    }
}

static process_t *proc_by_pid(uint32_t pid)
{
    if (pid == 0) return NULL;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (proc_pool[i].pid == pid) return &proc_pool[i];
    return NULL;
}

static void sched_note(const char *text)
{
    int slot = (sched_event_head + sched_event_count) % SCHED_EVENTS;
    if (sched_event_count == SCHED_EVENTS)          /* full: drop the oldest */
        sched_event_head = (sched_event_head + 1) % SCHED_EVENTS;
    else
        sched_event_count++;
    strncpy(sched_events[slot], text, sizeof(sched_events[0]) - 1);
    sched_events[slot][sizeof(sched_events[0]) - 1] = '\0';
}

/* Queue a message for the desktop to show (callable from any process). */
void scheduler_post_event(const char *text)
{
    uint64_t irq = irq_save();
    sched_note(text);
    irq_restore(irq);
    terminal_printf("[EVENT] %s\n", text);
}

/* Legacy entry point kept for callers that only need the low-level swap. */
void context_switch(void *from_void, void *to_void)
{
    process_t *from = (process_t *)from_void;
    process_t *to   = (process_t *)to_void;
    if (!from || !to || from == to) return;
    context_switch_asm(&from->rsp, &to->rsp);
}

/* ---- Core: pick the next process and switch to it ------------------------
 * Must be called with interrupts disabled. Returns when the calling process
 * is next given the CPU.
 */
static process_t *sched_pick(void)
{
    process_t *best = NULL;
    int start = current_proc ? (int)(current_proc - proc_pool) + 1 : 0;

    for (int n = 0; n < MAX_PROCESSES; n++) {
        process_t *p = &proc_pool[(start + n) % MAX_PROCESSES];
        if (p->pid == 0 || p->suspended || (p->flags & PROC_FLAG_SERVICE))
            continue;
        if (p->state == PROC_SLEEPING && p->wake_tick <= g_kstate.uptime_ticks)
            p->state = PROC_READY;
        if (p->state != PROC_READY)
            continue;
        if (!best || p->priority < best->priority)
            best = p;       /* first found at the best priority = round robin */
    }
    return best;
}

static void schedule(void)
{
    process_t *prev = current_proc;
    process_t *next;

    if (!sched_ready || !prev)
        return;

    if (prev->state == PROC_RUNNING)
        prev->state = PROC_READY;

    /* Nothing runnable: idle with interrupts on until something is. */
    while ((next = sched_pick()) == NULL) {
        sched_idle = 1;
        __asm__ volatile ("sti; hlt; cli");
        sched_idle = 0;
    }

    next->state = PROC_RUNNING;
    next->quantum_rem = quantum_ticks_for(next->priority);
    if (next == prev)
        return;

    /* A finished process's slot becomes reusable once we leave its stack. */
    if (prev->state == PROC_ZOMBIE)
        prev->pid = 0;

    current_proc = next;
    sched_switches++;

    /* Interrupts and system calls arriving from user mode must land on the
     * next process's own kernel stack. */
    usermode_set_kernel_stack((uint64_t)(next->stack + PROC_STACK_SIZE));

    /* Enter the next process's address space. Kernel memory (including both
     * stacks) is mapped identically in all of them. */
    uint64_t want = next->cr3 ? next->cr3 : vmm_kernel_cr3();
    uint64_t have;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(have));
    if (want && (have & ~0xFFFULL) != want)
        __asm__ volatile ("mov %0, %%cr3" : : "r"(want) : "memory");

    context_switch_asm(&prev->rsp, &next->rsp);
}

/* Timer interrupt, 100 Hz. Interrupts are off and the PIC has already been
 * acknowledged (see interrupt_dispatch), so switching away here is safe. */
void scheduler_tick(void)
{
    if (!sched_ready || !current_proc)
        return;

    if (sched_idle) {               /* interrupted the idle halt: just account */
        sched_idle_ticks++;
        return;
    }

    current_proc->cpu_ticks++;

    int preempt = 0;
    if (current_proc->quantum_rem > 0)
        current_proc->quantum_rem--;
    if (current_proc->quantum_rem == 0)
        preempt = 1;

    /* A more important process that is ready (or due to wake) runs now. */
    for (int i = 0; i < MAX_PROCESSES && !preempt; i++) {
        process_t *p = &proc_pool[i];
        if (p->pid == 0 || p->suspended || (p->flags & PROC_FLAG_SERVICE)) continue;
        int ready = p->state == PROC_READY ||
                    (p->state == PROC_SLEEPING && p->wake_tick <= g_kstate.uptime_ticks);
        if (ready && p->priority < current_proc->priority)
            preempt = 1;
    }

    if (preempt)
        schedule();
}

/* First code a new process runs: enable interrupts, run the body, exit. */
static void proc_trampoline(void)
{
    __asm__ volatile ("sti");
    if (current_proc && current_proc->entry)
        current_proc->entry();
    kthread_exit(0);
}

void scheduler_init(void)
{
    memset(proc_pool, 0, sizeof(proc_pool));
    memset(mutexes, 0, sizeof(mutexes));
    memset(semaphores, 0, sizeof(semaphores));

    /* The code that is running right now (kernel main / desktop loop on the
     * boot stack) becomes process 1. Its stack pointer is captured the first
     * time it is switched out. */
    process_t *p = &proc_pool[0];
    p->pid = next_pid++;
    p->priority = PRIO_SYSTEM;
    p->state = PROC_RUNNING;
    p->flags = PROC_FLAG_SYSTEM;
    p->quantum_rem = quantum_ticks_for(PRIO_SYSTEM);
    strncpy(p->name, "desktop", sizeof(p->name) - 1);

    current_proc = p;
    sched_ready = 1;
    terminal_puts("[SCHED] Preemptive scheduler online (process 1: desktop)\n");
}

/* ---- Creation ------------------------------------------------------------ */
uint32_t proc_spawn(const char *name, void (*fn)(void), int prio, uint32_t flags)
{
    uint64_t irq = irq_save();
    process_t *p = NULL;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_pool[i].pid == 0) { p = &proc_pool[i]; break; }
    }
    if (!p) {
        irq_restore(irq);
        terminal_printf("[SCHED] Cannot create process %s: table full\n", name);
        return 0;
    }

    /* everything except the (large) stack */
    memset(p, 0, offsetof(process_t, stack));
    p->pid = next_pid++;
    p->priority = (proc_prio_t)prio;
    p->flags = flags;
    p->entry = fn;
    p->start_tick = g_kstate.uptime_ticks;
    p->quantum_rem = quantum_ticks_for(p->priority);
    strncpy(p->name, name, sizeof(p->name) - 1);

    if (flags & PROC_FLAG_SERVICE) {
        /* Serviced cooperatively from the main loop: listed, never switched to. */
        p->state = PROC_READY;
    } else {
        /* Initial frame: what context_switch_asm pops (r15, r14, r13, r12,
         * rbx, rbp) and then `ret`s into. The slot below the return address
         * keeps the stack 16-byte aligned at function entry. */
        uint64_t *sp = (uint64_t *)(p->stack + PROC_STACK_SIZE);
        *--sp = 0;
        *--sp = (uint64_t)proc_trampoline;
        for (int i = 0; i < 6; i++) *--sp = 0;
        p->rsp = (uint64_t)sp;
        p->state = PROC_READY;
    }

    uint32_t pid = p->pid;
    irq_restore(irq);
    terminal_printf("[SCHED] Process created: %s (PID %u, prio %d)\n", name, pid, prio);
    return pid;
}

void kthread_create(const char *name, void (*fn)(void), int prio, size_t stack_size)
{
    (void)stack_size;
    proc_spawn(name, fn, prio, 0);
}

/* ---- Yield / sleep / exit ------------------------------------------------ */
void kthread_yield(void)
{
    uint64_t irq = irq_save();
    schedule();
    irq_restore(irq);
}

void kthread_sleep(uint32_t ticks)
{
    if (!sched_ready || !current_proc) {
        __asm__ volatile ("hlt");
        return;
    }
    uint64_t irq = irq_save();
    current_proc->wake_tick = g_kstate.uptime_ticks + ticks;
    current_proc->state = PROC_SLEEPING;
    schedule();
    irq_restore(irq);
}

/* Wake process 1 early (keyboard / mouse input should not wait for a tick). */
void scheduler_wake_main(void)
{
    if (sched_ready && proc_pool[0].state == PROC_SLEEPING)
        proc_pool[0].wake_tick = 0;
}

uint32_t kthread_current_pid(void)
{
    return current_proc ? current_proc->pid : 0;
}

/* Release everything a dying process holds so nobody waits on it forever. */
static void proc_release_resources(process_t *p)
{
    if (p->vm_space) {
        vm_space_destroy(p->vm_space);  /* returns its frames and swap slots */
        p->vm_space = 0;
        p->cr3 = 0;
    }

    for (int m = 0; m < MAX_MUTEXES; m++) {
        if (!mutexes[m].used || mutexes[m].owner != p->pid) continue;
        mutexes[m].owner = 0;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *w = &proc_pool[i];
            if (w->pid && w->state == PROC_BLOCKED &&
                w->wait_kind == PROC_WAIT_MUTEX && w->wait_id == m)
                w->state = PROC_READY;
        }
    }
}

void kthread_exit(uint64_t exit_code)
{
    __asm__ volatile ("cli");
    if (current_proc && !(current_proc->flags & PROC_FLAG_SYSTEM)) {
        terminal_printf("[SCHED] Process exited: %s (PID %u, code %llu)\n",
            current_proc->name, current_proc->pid, exit_code);
        proc_release_resources(current_proc);
        current_proc->state = PROC_ZOMBIE;
    }
    for (;;)
        schedule();         /* never returns for a zombie */
}

/* ---- Address spaces ------------------------------------------------------ */
/* The calling process starts running in the given address space. */
int proc_use_space(int space_id)
{
    uint64_t cr3 = vm_space_cr3(space_id);
    if (!cr3 || !current_proc) return -1;

    uint64_t irq = irq_save();
    current_proc->vm_space = space_id;
    current_proc->cr3 = cr3;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    irq_restore(irq);
    return 0;
}

void proc_mark_user(void)
{
    if (current_proc) current_proc->flags |= PROC_FLAG_USER;
}

int proc_exists(uint32_t pid)
{
    return proc_by_pid(pid) != NULL;
}

int proc_current_space(void)
{
    return current_proc ? current_proc->vm_space : 0;
}

int proc_current_is_system(void)
{
    return !sched_ready || !current_proc || (current_proc->flags & PROC_FLAG_SYSTEM);
}

const char *proc_current_name(void)
{
    return current_proc ? current_proc->name : "kernel";
}

/* A process did something illegal (bad memory access, divide by zero...).
 * Only that process dies; the rest of the system carries on. Called from
 * the exception handler with interrupts off; never returns. */
void proc_fault_exit(const char *reason)
{
    __asm__ volatile ("cli");
    terminal_printf("[FAULT] %s\n", reason);
    sched_note(reason);
    proc_release_resources(current_proc);
    current_proc->state = PROC_ZOMBIE;
    for (;;)
        schedule();
}

/* ---- Control of other processes ------------------------------------------ */
int proc_kill(uint32_t pid)
{
    uint64_t irq = irq_save();
    process_t *p = proc_by_pid(pid);

    if (!p || (p->flags & (PROC_FLAG_SYSTEM | PROC_FLAG_SERVICE))) {
        irq_restore(irq);
        return -1;
    }
    if (p == current_proc) {
        irq_restore(irq);
        kthread_exit(137);
    }

    /* Not on the CPU (single core), so it can be removed on the spot. */
    terminal_printf("[SCHED] Process killed: %s (PID %u)\n", p->name, p->pid);
    proc_release_resources(p);
    p->state = PROC_ZOMBIE;
    p->pid = 0;
    irq_restore(irq);
    return 0;
}

int proc_suspend(uint32_t pid)
{
    uint64_t irq = irq_save();
    process_t *p = proc_by_pid(pid);
    int rc = -1;

    if (p && !(p->flags & (PROC_FLAG_SYSTEM | PROC_FLAG_SERVICE)) && !p->suspended) {
        p->suspended = 1;
        rc = 0;
        if (p == current_proc)
            schedule();     /* comes back here once resumed */
    }
    irq_restore(irq);
    return rc;
}

int proc_resume(uint32_t pid)
{
    uint64_t irq = irq_save();
    process_t *p = proc_by_pid(pid);
    int rc = -1;

    if (p && p->suspended) {
        p->suspended = 0;
        rc = 0;
    }
    irq_restore(irq);
    return rc;
}

int proc_set_priority(uint32_t pid, int prio)
{
    uint64_t irq = irq_save();
    process_t *p = proc_by_pid(pid);
    int rc = -1;

    if (p && prio >= PRIO_REALTIME && prio <= PRIO_USER && !(p->flags & PROC_FLAG_SERVICE)) {
        p->priority = (proc_prio_t)prio;
        rc = 0;
    }
    irq_restore(irq);
    return rc;
}

/* ---- Introspection ------------------------------------------------------- */
int proc_list(proc_info_t *out, int max)
{
    uint64_t irq = irq_save();
    int n = 0;

    for (int i = 0; i < MAX_PROCESSES && n < max; i++) {
        process_t *p = &proc_pool[i];
        if (p->pid == 0) continue;

        proc_info_t *o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->pid = p->pid;
        o->priority = (int)p->priority;
        o->state = (int)p->state;
        o->flags = p->flags;
        o->suspended = p->suspended;
        o->cpu_ticks = p->cpu_ticks;
        o->start_tick = p->start_tick;
        strncpy(o->name, p->name, sizeof(o->name) - 1);

        if (p->state == PROC_BLOCKED && p->wait_kind == PROC_WAIT_MUTEX)
            strncpy(o->waiting_on, mutexes[p->wait_id].name, sizeof(o->waiting_on) - 1);
        else if (p->state == PROC_BLOCKED && p->wait_kind == PROC_WAIT_SEM)
            strncpy(o->waiting_on, semaphores[p->wait_id].name, sizeof(o->waiting_on) - 1);
        else if (p->state == PROC_BLOCKED && p->wait_kind == PROC_WAIT_MAIL)
            strncpy(o->waiting_on, "message", sizeof(o->waiting_on) - 1);
    }
    irq_restore(irq);
    return n;
}

void scheduler_stats(uint64_t *idle_ticks, uint64_t *switches)
{
    if (idle_ticks) *idle_ticks = sched_idle_ticks;
    if (switches)   *switches = sched_switches;
}

/* Returns 1 and copies the text if something worth telling the user about
 * happened since the last call (e.g. a deadlock was prevented). */
int scheduler_take_event(char *buf, size_t n)
{
    uint64_t irq = irq_save();
    int had = sched_event_count > 0;
    if (had && n) {
        strncpy(buf, sched_events[sched_event_head], n - 1);
        buf[n - 1] = '\0';
        sched_event_head = (sched_event_head + 1) % SCHED_EVENTS;
        sched_event_count--;
    }
    irq_restore(irq);
    return had;
}

/* ---- Synchronisation: mutexes -------------------------------------------- */
int kmutex_create(const char *name)
{
    uint64_t irq = irq_save();
    int id = -1;
    for (int i = 0; i < MAX_MUTEXES; i++) {
        if (!mutexes[i].used) {
            memset(&mutexes[i], 0, sizeof(mutexes[i]));
            mutexes[i].used = 1;
            strncpy(mutexes[i].name, name ? name : "mutex", sizeof(mutexes[i].name) - 1);
            id = i;
            break;
        }
    }
    irq_restore(irq);
    return id;
}

void kmutex_destroy(int id)
{
    if (id < 0 || id >= MAX_MUTEXES) return;
    uint64_t irq = irq_save();
    mutexes[id].used = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *w = &proc_pool[i];
        if (w->pid && w->state == PROC_BLOCKED &&
            w->wait_kind == PROC_WAIT_MUTEX && w->wait_id == id)
            w->state = PROC_READY;
    }
    irq_restore(irq);
}

/*
 * Deadlock detection. Waiting for mutex `id` is only safe if following
 * "owner of this mutex -> the mutex that owner is itself waiting for -> its
 * owner -> ..." never leads back to the caller. If it does, granting the
 * wait would complete a cycle in which no process can ever proceed.
 */
static int would_deadlock(int id)
{
    uint32_t me = current_proc->pid;
    int m = id;

    for (int hops = 0; hops < MAX_PROCESSES; hops++) {
        uint32_t owner = mutexes[m].owner;
        if (owner == 0) return 0;
        if (owner == me) return 1;

        process_t *p = proc_by_pid(owner);
        if (!p || p->state != PROC_BLOCKED || p->wait_kind != PROC_WAIT_MUTEX)
            return 0;
        m = p->wait_id;
    }
    return 1;
}

/* 0 = acquired; -1 = invalid; -2 = refused because it would deadlock. */
int kmutex_lock(int id)
{
    if (id < 0 || id >= MAX_MUTEXES || !current_proc) return -1;
    uint64_t irq = irq_save();

    for (;;) {
        if (!mutexes[id].used) { irq_restore(irq); return -1; }

        if (mutexes[id].owner == 0) {
            mutexes[id].owner = current_proc->pid;
            irq_restore(irq);
            return 0;
        }
        if (would_deadlock(id)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Deadlock prevented: %s (PID %u) was refused lock '%s'",
                     current_proc->name, current_proc->pid, mutexes[id].name);
            sched_note(msg);
            terminal_printf("[SCHED] %s\n", msg);
            irq_restore(irq);
            return -2;
        }

        current_proc->state = PROC_BLOCKED;
        current_proc->wait_kind = PROC_WAIT_MUTEX;
        current_proc->wait_id = id;
        schedule();
        current_proc->wait_kind = PROC_WAIT_NONE;
    }
}

int kmutex_unlock(int id)
{
    if (id < 0 || id >= MAX_MUTEXES || !current_proc) return -1;
    uint64_t irq = irq_save();

    if (!mutexes[id].used || mutexes[id].owner != current_proc->pid) {
        irq_restore(irq);
        return -1;
    }
    mutexes[id].owner = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *w = &proc_pool[i];
        if (w->pid && w->state == PROC_BLOCKED &&
            w->wait_kind == PROC_WAIT_MUTEX && w->wait_id == id)
            w->state = PROC_READY;      /* they re-contend for the lock */
    }
    irq_restore(irq);
    return 0;
}

/* ---- Synchronisation: counting semaphores -------------------------------- */
int ksem_create(const char *name, int initial)
{
    uint64_t irq = irq_save();
    int id = -1;
    for (int i = 0; i < MAX_SEMAPHORES; i++) {
        if (!semaphores[i].used) {
            memset(&semaphores[i], 0, sizeof(semaphores[i]));
            semaphores[i].used = 1;
            semaphores[i].count = initial;
            strncpy(semaphores[i].name, name ? name : "sem", sizeof(semaphores[i].name) - 1);
            id = i;
            break;
        }
    }
    irq_restore(irq);
    return id;
}

int ksem_wait(int id)
{
    if (id < 0 || id >= MAX_SEMAPHORES || !current_proc) return -1;
    uint64_t irq = irq_save();

    while (semaphores[id].used && semaphores[id].count <= 0) {
        current_proc->state = PROC_BLOCKED;
        current_proc->wait_kind = PROC_WAIT_SEM;
        current_proc->wait_id = id;
        schedule();
        current_proc->wait_kind = PROC_WAIT_NONE;
    }
    int ok = semaphores[id].used;
    if (ok) semaphores[id].count--;
    irq_restore(irq);
    return ok ? 0 : -1;
}

int ksem_signal(int id)
{
    if (id < 0 || id >= MAX_SEMAPHORES) return -1;
    uint64_t irq = irq_save();

    if (!semaphores[id].used) { irq_restore(irq); return -1; }
    semaphores[id].count++;
    for (int i = 0; i < MAX_PROCESSES; i++) {        /* wake one waiter */
        process_t *w = &proc_pool[i];
        if (w->pid && w->state == PROC_BLOCKED &&
            w->wait_kind == PROC_WAIT_SEM && w->wait_id == id) {
            w->state = PROC_READY;
            break;
        }
    }
    irq_restore(irq);
    return 0;
}

/* ---- IPC: per-process mailboxes ------------------------------------------ */
int proc_send(uint32_t to_pid, uint64_t value)
{
    uint64_t irq = irq_save();
    process_t *p = proc_by_pid(to_pid);
    int rc = -1;

    if (p && p->mail_count < MAILBOX_SIZE) {
        int slot = (p->mail_head + p->mail_count) % MAILBOX_SIZE;
        p->mailbox[slot].from = current_proc ? current_proc->pid : 0;
        p->mailbox[slot].value = value;
        p->mail_count++;
        if (p->state == PROC_BLOCKED && p->wait_kind == PROC_WAIT_MAIL)
            p->state = PROC_READY;
        rc = 0;
    }
    irq_restore(irq);
    return rc;
}

/* Blocks until a message arrives. */
int proc_recv(uint32_t *from_pid, uint64_t *value)
{
    if (!current_proc) return -1;
    uint64_t irq = irq_save();

    while (current_proc->mail_count == 0) {
        current_proc->state = PROC_BLOCKED;
        current_proc->wait_kind = PROC_WAIT_MAIL;
        schedule();
        current_proc->wait_kind = PROC_WAIT_NONE;
    }
    if (from_pid) *from_pid = current_proc->mailbox[current_proc->mail_head].from;
    if (value)    *value = current_proc->mailbox[current_proc->mail_head].value;
    current_proc->mail_head = (current_proc->mail_head + 1) % MAILBOX_SIZE;
    current_proc->mail_count--;
    irq_restore(irq);
    return 0;
}

/* Kept for old callers: one scheduling decision. */
void *scheduler_pick_next(void)
{
    kthread_yield();
    return current_proc;
}

/* CPU relax (pause instruction — avoids busy-loop burning) */
void cpu_relax(void)
{
    __asm__ volatile ("pause" ::: "memory");
}

/* ============================================================================
 * 3. PHYSICAL MEMORY MANAGER
 * Bitmap-based frame allocator.
 * One bit per 4 KB page. 0 = free, 1 = used.
 * Supports contiguous allocation for the AI model pool.
 * ============================================================================ */

#define PAGE_SIZE  4096UL
#define MAX_FRAMES (8UL * 1024 * 1024 * 1024 / PAGE_SIZE) /* 8 GB → 2 M frames */
#define BITMAP_SIZE (MAX_FRAMES / 8)

static uint8_t  phys_bitmap[BITMAP_SIZE];
static uint64_t total_frames  = 0;
static uint64_t free_frames   = 0;
static uint64_t usable_frames;

static void pmm_set_used(uint64_t frame)
{
    phys_bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static void pmm_set_free(uint64_t frame)
{
    phys_bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

static int pmm_is_free(uint64_t frame)
{
    return !(phys_bitmap[frame / 8] & (1u << (frame % 8)));
}

void pmm_free_range(uint64_t base, uint64_t len)
{
    /* The bitmap must start as "everything in use": only ranges the boot
     * memory map reports as usable RAM are then released. It used to start
     * zeroed (= all free), so holes in the memory map (firmware, ACPI) could
     * be handed out as RAM and the free-frame counter underflowed. */
    static int initialised = 0;
    if (!initialised) {
        memset(phys_bitmap, 0xFF, sizeof(phys_bitmap));
        initialised = 1;
    }
    uint64_t start = base / PAGE_SIZE;
    uint64_t end   = (base + len) / PAGE_SIZE;
    for (uint64_t f = start; f < end && f < MAX_FRAMES; f++) {
        if (!pmm_is_free(f)) {
            pmm_set_free(f);
            free_frames++;
            usable_frames++;
        }
        total_frames = f + 1 > total_frames ? f + 1 : total_frames;
    }
}

void pmm_reserve_range(uint64_t base, uint64_t len)
{
    uint64_t start = base / PAGE_SIZE;
    uint64_t end   = (base + len) / PAGE_SIZE;
    for (uint64_t f = start; f < end && f < MAX_FRAMES; f++) {
        if (pmm_is_free(f)) {
            pmm_set_used(f);
            free_frames--;
        }
    }
}

/* Allocate a single 4 KB frame. Searches from where the last search ended
 * rather than from frame 0 every time. */
uint64_t pmm_alloc_frame(void)
{
    static uint64_t hint = 0;
    uint64_t irqf = irq_save();

    for (uint64_t n = 0; n < total_frames; n++) {
        uint64_t f = (hint + n) % total_frames;
        if (f != 0 && pmm_is_free(f)) {
            pmm_set_used(f);
            free_frames--;
            hint = f + 1;
            irq_restore(irqf);
            return f * PAGE_SIZE;
        }
    }
    irq_restore(irqf);
    return 0; /* out of memory */
}

void pmm_free_frame(uint64_t phys)
{
    uint64_t irqf = irq_save();
    uint64_t f = phys / PAGE_SIZE;
    if (f < total_frames && !pmm_is_free(f)) {
        pmm_set_free(f);
        free_frames++;
    }
    irq_restore(irqf);
}

/* total_frames spans holes in the memory map, so count what is actually
 * usable: frames handed to the allocator, free or not. */
void pmm_get_stats(uint64_t *total, uint64_t *free_count)
{
    if (total)      *total = usable_frames;
    if (free_count) *free_count = free_frames;
}

/* Allocate N contiguous frames (for model pool DMA) */
uint64_t pmm_alloc_contiguous(uint64_t pages)
{
    uint64_t run_start = 0;
    uint64_t run_len   = 0;

    for (uint64_t f = 0; f < total_frames; f++) {
        if (pmm_is_free(f)) {
            if (run_len == 0) run_start = f;
            run_len++;
            if (run_len >= pages) {
                /* Found a contiguous block — mark used */
                for (uint64_t i = run_start; i < run_start + pages; i++) {
                    pmm_set_used(i);
                    free_frames--;
                }
                terminal_printf("[PMM] Contiguous alloc: %llu pages at 0x%llx\n",
                    pages, run_start * PAGE_SIZE);
                return run_start * PAGE_SIZE;
            }
        } else {
            run_len = 0;
        }
    }
    terminal_puts("[PMM] WARN: Cannot allocate contiguous region\n");
    return 0;
}

/*
 * Kernel heap — real free-list allocator with splitting + coalescing.
 *
 * The previous version was a pure bump allocator (heap_ptr only ever grew)
 * with kfree() as a documented no-op. That's a genuine, severe problem for
 * this specific OS: vfs.c's ramfs_write() calls kfree() on the old buffer
 * every time a file grows past its capacity (it doubles and copies) — with
 * a no-op kfree, every single growth permanently leaks the previous buffer
 * into this fixed 16 MB heap. model_manifest.c stages paths for a 2 GB
 * brain.gguf, a 1.5 GB Whisper model, a 327 MB TTS model, etc. — any of
 * those, written through the small heap, exhausts all 16 MB on the very
 * first doubling pass and kmalloc() starts returning NULL.
 *
 * Two changes fix this:
 *   1. kfree() actually frees (first-fit, address-ordered, with immediate
 *      coalescing of adjacent free blocks in both directions).
 *   2. Anything at or above LARGE_ALLOC_THRESHOLD bypasses this heap
 *      entirely and is backed directly by physical pages from the PMM via
 *      kmalloc_large()/kfree_large() — see vfs.c's ramfs_write(), which now
 *      switches a file over to this path once it crosses the threshold.
 *      This is what actually makes it possible to stage a multi-GB model
 *      file in ramfs without needing a multi-GB static C array.
 */
#define HEAP_SIZE        (16 * 1024 * 1024) /* 16 MB — for small/metadata allocations only */
#define HEAP_ALIGN       16UL
#define HEAP_MIN_SPLIT   32UL
/* LARGE_ALLOC_THRESHOLD is defined once, in kernel.h, and shared with vfs.c */

typedef struct heap_block {
    size_t              size;   /* usable size, excludes this header */
    uint8_t             free;
    struct heap_block  *prev;
    struct heap_block  *next;
} heap_block_t;

static uint8_t       heap[HEAP_SIZE];
static heap_block_t *heap_head = NULL;

static void heap_init(void)
{
    heap_head = (heap_block_t *)heap;
    heap_head->size = HEAP_SIZE - sizeof(heap_block_t);
    heap_head->free = 1;
    heap_head->prev = NULL;
    heap_head->next = NULL;
}

void *kmalloc(size_t size)
{
    if (size == 0) return NULL;
    if (size >= LARGE_ALLOC_THRESHOLD) {
        terminal_printf("[HEAP] kmalloc(%zu): use kmalloc_large() for "
                         "allocations this size — refusing\n", size);
        return NULL;
    }

    if (!heap_head) heap_init();

    size = (size + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1);

    uint64_t irqf = irq_save();
    for (heap_block_t *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < size) continue;

        if (b->size >= size + sizeof(heap_block_t) + HEAP_MIN_SPLIT) {
            heap_block_t *rem = (heap_block_t *)((uint8_t *)(b + 1) + size);
            rem->size = b->size - size - sizeof(heap_block_t);
            rem->free = 1;
            rem->prev = b;
            rem->next = b->next;
            if (b->next) b->next->prev = rem;
            b->next  = rem;
            b->size  = size;
        }
        b->free = 0;
        irq_restore(irqf);
        return (void *)(b + 1);
    }
    irq_restore(irqf);
    terminal_printf("[HEAP] kmalloc(%zu): small heap exhausted\n", size);
    return NULL;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    heap_block_t *b = (heap_block_t *)ptr - 1;

    uint64_t irqf = irq_save();
    b->free = 1;

    if (b->next && b->next->free) {
        b->size += sizeof(heap_block_t) + b->next->size;
        b->next  = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        heap_block_t *p = b->prev;
        p->size += sizeof(heap_block_t) + b->size;
        p->next  = b->next;
        if (b->next) b->next->prev = p;
    }
    irq_restore(irqf);
}

/*
 * Large allocations — backed directly by PMM pages, tracked in a small
 * side table (rather than a header in front of the returned pointer) so
 * the returned buffer stays page-aligned, which matters for DMA and for
 * any future AVX-aligned tensor loads of model weights.
 */
void heap_get_stats(uint64_t *used_bytes, uint64_t *free_bytes, uint64_t *total_bytes)
{
    uint64_t used = 0, avail = 0;
    uint64_t irqf = irq_save();

    if (!heap_head) heap_init();
    for (heap_block_t *b = heap_head; b; b = b->next) {
        if (b->free) avail += b->size;
        else         used += b->size;
    }
    irq_restore(irqf);

    if (used_bytes)  *used_bytes = used;
    if (free_bytes)  *free_bytes = avail;
    if (total_bytes) *total_bytes = HEAP_SIZE;
}

#define MAX_LARGE_ALLOCS 64
typedef struct { void *ptr; uint64_t pages; } large_alloc_t;
static large_alloc_t large_allocs[MAX_LARGE_ALLOCS];

void *kmalloc_large(size_t size)
{
    if (size == 0) return NULL;
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t irqf = irq_save();
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        irq_restore(irqf);
        terminal_printf("[HEAP] kmalloc_large: failed to allocate %llu pages "
                         "(%zu bytes requested)\n", pages, size);
        return NULL;
    }

    for (int i = 0; i < MAX_LARGE_ALLOCS; i++) {
        if (!large_allocs[i].ptr) {
            large_allocs[i].ptr   = (void *)phys;
            large_allocs[i].pages = pages;
            irq_restore(irqf);
            return (void *)phys;
        }
    }

    /* No tracking slot left — release immediately rather than hand back a
     * pointer kfree_large() could never reclaim. */
    for (uint64_t i = 0; i < pages; i++)
        pmm_free_frame(phys + i * PAGE_SIZE);
    irq_restore(irqf);
    terminal_puts("[HEAP] kmalloc_large: tracking table full (max 64 "
                  "concurrent large allocations)\n");
    return NULL;
}

void kfree_large(void *ptr, size_t size)
{
    (void)size; /* kept in the API for call-site clarity; pages actually
                   freed come from the tracking table so a caller passing
                   a stale/wrong size can't corrupt the PMM bitmap. */
    if (!ptr) return;

    uint64_t irqf = irq_save();
    for (int i = 0; i < MAX_LARGE_ALLOCS; i++) {
        if (large_allocs[i].ptr == ptr) {
            for (uint64_t f = 0; f < large_allocs[i].pages; f++)
                pmm_free_frame((uint64_t)ptr + f * PAGE_SIZE);
            large_allocs[i].ptr   = NULL;
            large_allocs[i].pages = 0;
            irq_restore(irqf);
            return;
        }
    }
    irq_restore(irqf);
    terminal_puts("[HEAP] kfree_large: pointer not found in tracking table "
                  "(double free, or it was never allocated via kmalloc_large)\n");
}
