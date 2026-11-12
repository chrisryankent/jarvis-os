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
    cli(); /* disable interrupts during write */

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
    sti();
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
 * 2. AI-AWARE PROCESS SCHEDULER
 * Multi-Level Feedback Queue (MLFQ) with 4 priority classes.
 * AI inference tasks run at PRIO_AI; speech is PRIO_REALTIME.
 *
 * Rules:
 *   - A process at a higher priority always preempts lower ones.
 *   - AI inference preempts all USER processes (prevents stutter).
 *   - REALTIME (speech) never waits behind a lower-priority process.
 *   - Processes are promoted if they yield before their quantum expires.
 *   - Processes are demoted if they use their full quantum (CPU hogs).
 *
 * proc_prio_t / proc_state_t now live in kernel.h (see the comment there) —
 * this used to be a file-local typedef, which is why kernel.c could not see
 * PRIO_AI/PRIO_REALTIME and failed to compile.
 * ============================================================================ */

#define MAX_PROCESSES  256
#define QUANTUM_MS     10    /* length of one PIT tick / one scheduling unit */

/*
 * How many ticks each priority class gets before being forced to rotate.
 * This is what makes the scheduler an actual *feedback* queue instead of a
 * flat round-robin: an AI inference thread gets a 40 ms uninterrupted slice
 * (long enough to make real progress on a forward pass) while a USER task
 * gets bumped every 10 ms. A higher-priority thread becoming ready still
 * preempts immediately regardless of how much slice is left (see below).
 */
static uint32_t quantum_ticks_for(proc_prio_t prio)
{
    switch (prio) {
    case PRIO_REALTIME: return 1;
    case PRIO_AI:        return 4;
    case PRIO_SYSTEM:    return 2;
    default:             return 1; /* PRIO_USER */
    }
}

typedef struct process {
    uint32_t       pid;
    proc_prio_t    priority;
    proc_state_t   state;
    char           name[64];
    uint64_t       rsp;            /* saved kernel stack pointer */
    uint64_t       rip;            /* saved instruction pointer  */
    uint64_t       cr3;            /* page table base, 0 = shares kernel tables */
    uint8_t        stack[65536];   /* 64 KB kernel stack         */
    uint64_t       cpu_time_ms;
    uint32_t       quantum_rem;    /* remaining quantum ticks    */
    uint64_t       wake_tick;      /* for sleeping processes     */
    struct process *next;
} process_t;

static process_t  proc_pool[MAX_PROCESSES];
static process_t *run_queues[4] = {}; /* one per priority */
static process_t *current_proc  = NULL;
static uint32_t   next_pid      = 1;

/*
 * Real ASM register-save/restore — see context_switch.S. Takes plain
 * pointers to the two rsp *fields* (not whole process_t structs), so the
 * assembly never needs to know process_t's layout/offsets, which keeps it
 * robust against this struct changing shape later.
 */
extern void context_switch_asm(uint64_t *save_rsp_here, uint64_t *load_rsp_from);

/*
 * "Boot context" — represents whatever was running before the scheduler
 * ever ran a kthread (i.e. jarvis_kernel_main()/kernel_desktop_loop() on the
 * original boot stack). Without this, the very first context_switch() call
 * is given from=NULL, which the NULL-guard below turns into a silent no-op
 * — meaning the very first kernel thread is marked "current" in bookkeeping
 * but its registers/rip are never actually loaded, so it never runs a
 * single instruction. This mirrors the standard "scheduler context" trick
 * used by most teaching kernels (e.g. xv6): we don't need a real stack
 * buffer for it, because we're capturing whatever rsp already *is* at the
 * moment of the first real switch, not building a fresh frame for it.
 */
static process_t boot_proc;
static int        boot_proc_ready = 0;

static void boot_proc_init(void)
{
    memset(&boot_proc, 0, sizeof(boot_proc));
    strncpy(boot_proc.name, "boot/idle", sizeof(boot_proc.name) - 1);
    boot_proc.priority = PRIO_SYSTEM;
    boot_proc.state    = PROC_RUNNING;
    boot_proc.quantum_rem = quantum_ticks_for(PRIO_SYSTEM);
    boot_proc_ready = 1;
    current_proc = &boot_proc;
}

/* Allocate a process slot. Threads that have called kthread_exit() set
 * pid = 0 on the way out specifically so this can reclaim their slot —
 * previously nothing ever did that, so the 256-thread pool was a one-way
 * ratchet that could never be reused. */
static process_t *proc_alloc(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_pool[i].pid == 0) {
            return &proc_pool[i];
        }
    }
    return NULL;
}

/* Enqueue a process into its priority run queue */
static void enqueue(process_t *p)
{
    p->next = NULL;
    process_t **q = &run_queues[p->priority];
    if (!*q) {
        *q = p;
    } else {
        process_t *tail = *q;
        while (tail->next) tail = tail->next;
        tail->next = p;
    }
}

/* Dequeue the head of a priority queue */
static process_t *dequeue(int prio)
{
    if (!run_queues[prio]) return NULL;
    process_t *p  = run_queues[prio];
    run_queues[prio] = p->next;
    p->next = NULL;
    return p;
}

/* Context switch — saves current registers/RSP, loads next, switches CR3
 * if (and only if) the destination actually uses its own address space.
 * `from` or `to` being NULL is a no-op (used at very early boot before
 * boot_proc_init() has run). Writing 0 into CR3 unconditionally — which a
 * naive "full" implementation of the old TODO comment would do, since
 * kernel threads start with cr3=0 — would immediately triple-fault real
 * hardware, so the switch is conditional. */
void context_switch(void *from_void, void *to_void)
{
    process_t *from = (process_t *)from_void;
    process_t *to   = (process_t *)to_void;
    if (!from || !to || from == to) return;

    if (to->cr3 != 0) {
        uint64_t current_cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(current_cr3));
        if (current_cr3 != to->cr3)
            __asm__ volatile ("mov %0, %%cr3" :: "r"(to->cr3) : "memory");
    }

    /* The actual register save/restore + stack swap. This call does not
     * return into "from"'s caller until something later switches back into
     * "from" — exactly like every other kernel's context switch. */
    context_switch_asm(&from->rsp, &to->rsp);
}

/* Create a kernel thread */
void kthread_create(const char *name, void (*fn)(void),
                    int prio, size_t stack_size)
{
    (void)stack_size;

    /* kthread_create() runs after interrupt_init() has already called
     * sti(), so the timer IRQ can fire mid-enqueue and corrupt the run
     * queue linked list out from under us if we don't guard it. */
    cli();

    process_t *p = proc_alloc();
    if (!p) {
        sti();
        terminal_printf("[SCHED] Cannot create thread: %s (pool full)\n", name);
        return;
    }

    p->pid        = next_pid++;
    p->priority   = (proc_prio_t)prio;
    p->state      = PROC_READY;
    p->quantum_rem= quantum_ticks_for((proc_prio_t)prio);
    p->cpu_time_ms= 0;
    p->wake_tick  = 0;
    strncpy(p->name, name, sizeof(p->name) - 1);

    /*
     * Build the initial stack frame so the FIRST time context_switch_asm
     * switches into this thread, the pop sequence it executes (r15, r14,
     * r13, r12, rbx, rbp, then `ret`) lands exactly on `fn`. The previous
     * version only pushed the return address and nothing else, so the
     * very first "restore 6 registers" in a correct context switch would
     * have popped six bytes of garbage off the top of a fresh stack before
     * ever reaching fn — this layout has to mirror context_switch_asm's
     * push order exactly (see context_switch.S).
     */
    uint64_t *sp = (uint64_t *)(p->stack + sizeof(p->stack));
    *--sp = (uint64_t)fn;   /* return address, popped last by `ret` */
    *--sp = 0;              /* rbp */
    *--sp = 0;              /* rbx */
    *--sp = 0;              /* r12 */
    *--sp = 0;              /* r13 */
    *--sp = 0;              /* r14 */
    *--sp = 0;              /* r15  <- rsp ends up here */
    p->rsp = (uint64_t)sp;
    p->rip = (uint64_t)fn;
    p->cr3 = 0; /* kernel threads share kernel page tables */

    enqueue(p);
    sti();
    terminal_printf("[SCHED] Thread created: %s (PID %u, prio %d)\n",
        name, p->pid, prio);
}

/* Voluntary yield — promotes the caller one priority level (reward for
 * giving back the CPU before its slice ran out) and forces a reschedule
 * on the next tick. Not wired to any syscall yet; exposed via kernel.h so
 * a future blocking syscall (disk wait, IPC wait, etc.) has a correct
 * primitive to call instead of busy-spinning at full priority. */
void kthread_yield(void)
{
    cli();
    if (current_proc && current_proc != &boot_proc) {
        if (current_proc->priority > PRIO_REALTIME)
            current_proc->priority = (proc_prio_t)(current_proc->priority - 1);
        current_proc->quantum_rem = 0; /* exhausted on purpose -> reschedule */
    }
    sti();
}

uint32_t kthread_current_pid(void)
{
    return current_proc ? current_proc->pid : 0;
}

/* Terminate the calling kernel thread. Marks it a reapable zombie (pid=0,
 * so proc_alloc() can reuse the slot) and immediately gives up the CPU
 * forever — this never returns. Previously there was no exit path at all:
 * a finished thread function simply falling off the end of fn() would
 * return into garbage (the popped-but-never-pushed return address below
 * the initial frame), so every kthread body needs to end by calling this. */
void kthread_exit(uint64_t exit_code)
{
    cli();
    if (current_proc && current_proc != &boot_proc) {
        terminal_printf("[SCHED] Thread exited: %s (PID %u, code %llu)\n",
            current_proc->name, current_proc->pid, exit_code);
        current_proc->state = PROC_ZOMBIE;
        current_proc->pid   = 0; /* reclaim slot */
    }
    sti();

    for (;;) {
        scheduler_pick_next();
        cpu_relax();
    }
}

/*
 * Pick next process to run AND perform the actual switch — called every
 * timer tick. (Previously "pick" and "switch" were split across this
 * function and kernel.c's ai_scheduler_tick(), each keeping its *own*
 * separate `current_proc` variable that never agreed with each other —
 * the one in kernel.c was the only one ai_scheduler_tick ever updated, but
 * the *other* one, here, is what every preemption/requeue decision in this
 * function actually reads. Since nothing ever wrote to this file's
 * current_proc, every check below saw it as permanently NULL, so a
 * just-dequeued thread was never put back in its run queue — after one
 * pass through the four priority buckets, every created thread had been
 * silently dropped from scheduling forever. Folding both halves into one
 * function with one source of truth fixes both bugs at once.)
 */
void *scheduler_pick_next(void)
{
    cli();

    if (!boot_proc_ready)
        boot_proc_init();

    /* Is something strictly higher-priority already waiting? If so it
     * preempts immediately, regardless of how much quantum the current
     * thread has left — this is the rule that guarantees REALTIME speech
     * and AI inference are never starved by a long-running USER task. */
    int higher_ready = -1;
    if (current_proc) {
        for (int prio = 0; prio < (int)current_proc->priority; prio++) {
            if (run_queues[prio]) { higher_ready = prio; break; }
        }
    }

    if (current_proc && current_proc->state == PROC_RUNNING &&
        higher_ready < 0 && current_proc->quantum_rem > 1) {
        current_proc->quantum_rem--;
        sti();
        return current_proc; /* nothing to do this tick */
    }

    process_t *prev = current_proc;

    if (prev && prev->state == PROC_RUNNING) {
        /* Only apply the CPU-hog demotion penalty if the thread actually
         * burned through its whole slice itself. Being preempted by a
         * more important thread is not a penalty. */
        if (higher_ready < 0 && prev->priority < PRIO_USER)
            prev->priority = (proc_prio_t)(prev->priority + 1);
        prev->quantum_rem = quantum_ticks_for(prev->priority);
        prev->state = PROC_READY;
        enqueue(prev);
    }

    process_t *next = NULL;
    for (int prio = 0; prio <= PRIO_USER; prio++) {
        process_t *p = dequeue(prio);
        if (!p) continue;

        if (p->state == PROC_BLOCKED) {
            if (p->wake_tick <= g_kstate.uptime_ticks) {
                p->state = PROC_READY;
            } else {
                enqueue(p); /* not awake yet, put back */
                continue;
            }
        }
        next = p;
        break;
    }

    if (!next) {
        sti();
        return prev; /* nothing else runnable, keep going */
    }

    next->state = PROC_RUNNING;
    if (next->quantum_rem == 0)
        next->quantum_rem = quantum_ticks_for(next->priority);

    current_proc = next;

    if (prev && prev != next)
        context_switch(prev, next); /* may not return here until prev is rescheduled */

    sti();
    return next;
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
    uint64_t start = base / PAGE_SIZE;
    uint64_t end   = (base + len) / PAGE_SIZE;
    for (uint64_t f = start; f < end && f < MAX_FRAMES; f++) {
        if (!pmm_is_free(f)) {
            pmm_set_free(f);
            free_frames++;
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

/* Allocate a single 4 KB frame */
uint64_t pmm_alloc_frame(void)
{
    for (uint64_t f = 0; f < total_frames; f++) {
        if (pmm_is_free(f)) {
            pmm_set_used(f);
            free_frames--;
            return f * PAGE_SIZE;
        }
    }
    return 0; /* out of memory */
}

void pmm_free_frame(uint64_t phys)
{
    uint64_t f = phys / PAGE_SIZE;
    if (f < total_frames && !pmm_is_free(f)) {
        pmm_set_free(f);
        free_frames++;
    }
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

    cli();
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
        sti();
        return (void *)(b + 1);
    }
    sti();
    terminal_printf("[HEAP] kmalloc(%zu): small heap exhausted\n", size);
    return NULL;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    heap_block_t *b = (heap_block_t *)ptr - 1;

    cli();
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
    sti();
}

/*
 * Large allocations — backed directly by PMM pages, tracked in a small
 * side table (rather than a header in front of the returned pointer) so
 * the returned buffer stays page-aligned, which matters for DMA and for
 * any future AVX-aligned tensor loads of model weights.
 */
#define MAX_LARGE_ALLOCS 64
typedef struct { void *ptr; uint64_t pages; } large_alloc_t;
static large_alloc_t large_allocs[MAX_LARGE_ALLOCS];

void *kmalloc_large(size_t size)
{
    if (size == 0) return NULL;
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    cli();
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        sti();
        terminal_printf("[HEAP] kmalloc_large: failed to allocate %llu pages "
                         "(%zu bytes requested)\n", pages, size);
        return NULL;
    }

    for (int i = 0; i < MAX_LARGE_ALLOCS; i++) {
        if (!large_allocs[i].ptr) {
            large_allocs[i].ptr   = (void *)phys;
            large_allocs[i].pages = pages;
            sti();
            return (void *)phys;
        }
    }

    /* No tracking slot left — release immediately rather than hand back a
     * pointer kfree_large() could never reclaim. */
    for (uint64_t i = 0; i < pages; i++)
        pmm_free_frame(phys + i * PAGE_SIZE);
    sti();
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

    cli();
    for (int i = 0; i < MAX_LARGE_ALLOCS; i++) {
        if (large_allocs[i].ptr == ptr) {
            for (uint64_t f = 0; f < large_allocs[i].pages; f++)
                pmm_free_frame((uint64_t)ptr + f * PAGE_SIZE);
            large_allocs[i].ptr   = NULL;
            large_allocs[i].pages = 0;
            sti();
            return;
        }
    }
    sti();
    terminal_puts("[HEAP] kfree_large: pointer not found in tracking table "
                  "(double free, or it was never allocated via kmalloc_large)\n");
}

/* ============================================================================
 * Virtual memory manager stub
 * ============================================================================ */
void vmm_init(uint64_t model_pool_phys)
{
    (void)model_pool_phys;
    terminal_puts("[VMM] 4-level page tables active (set up by stage2)\n");
}

void *vmm_map_range(uint64_t phys, uint64_t size, uint32_t flags)
{
    (void)size; (void)flags;
    /*
     * Boot paging identity-maps the first 4 GiB with 2 MiB pages, which covers
     * QEMU/Bochs display, NIC, AHCI, and HDA MMIO BARs. A full VMM will replace
     * this with dynamic mappings later.
     */
    return (void *)phys;
}
