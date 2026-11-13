/*
 * =============================================================================
 * JARVIS AI OS — Virtual Memory Manager (kernel/vmm.c)
 * Language: C17
 *
 * Memory management on top of the physical frame allocator:
 *
 *   address spaces   every process can have its own top-level page table.
 *                    The kernel's mappings are shared into each one; the
 *                    private part starts at VM_USER_BASE.
 *   paging           4 KB pages, 4-level x86-64 page tables.
 *   protection       per-page read-only / writable / user / no-execute.
 *                    CR0.WP is set so even kernel-mode code faults on a
 *                    write to a read-only page.
 *   isolation        two processes can use the same virtual address and see
 *                    different memory.
 *   demand paging    a region costs nothing until touched; the first access
 *                    to each page faults and gets a zeroed frame.
 *   swapping         pages can be evicted to a compressed in-memory swap
 *                    store and are brought back transparently on the next
 *                    access (second-chance / clock eviction).
 *   status           counters for RAM, swap and faults (see vm_get_stats).
 *
 * About segmentation: in 64-bit long mode the CPU ignores segment bases and
 * limits, so memory protection here is done entirely with paging. The GDT
 * segments that separate kernel and user privilege arrive with user mode.
 *
 * Layout of every address space:
 *   PML4[0]      identity map of the first 4 GiB (kernel only)
 *   PML4[1..255] private to the address space (VM_USER_BASE upwards)
 *   PML4[511]    kernel image (kernel only)
 *
 * Page tables are reached through the identity map, so frames used for them
 * must lie below 4 GiB.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

#define PAGE_SIZE     4096UL
#define PTE_P         0x001ULL
#define PTE_W         0x002ULL
#define PTE_U         0x004ULL
#define PTE_A         0x020ULL
#define PTE_SWAPPED   0x200ULL          /* not present: bits 12+ hold a swap slot */
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR      0x000FFFFFFFFFF000ULL

#define VM_MAX_SPACES   32
#define VM_MAX_REGIONS  8
#define VM_USER_END     0x0000800000000000ULL
#define SWAP_SLOTS      2048

typedef struct {
    int      used;
    uint64_t start;
    uint32_t pages;
    uint32_t flags;
    char     name[16];
} vm_region_t;

typedef struct {
    int         used;
    uint64_t    pml4;               /* physical address of the top-level table */
    uint32_t    owner_pid;
    vm_region_t regions[VM_MAX_REGIONS];
    uint32_t    resident;           /* pages currently in RAM   */
    uint32_t    swapped;            /* pages currently in swap  */
    uint32_t    faults;
} vm_space_t;

typedef struct {
    int      used;
    uint8_t *data;                  /* NULL for an all-zero page */
    uint16_t len;                   /* stored bytes (4096 = uncompressed) */
} swap_slot_t;

static vm_space_t  g_spaces[VM_MAX_SPACES];     /* index 0 unused: id 0 = kernel */
static swap_slot_t g_swap[SWAP_SLOTS];
static uint64_t    g_kernel_pml4 = 0;
static int         g_nx_enabled = 0;
static vm_stats_t  g_stats;

static inline uint64_t vm_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void vm_irq_restore(uint64_t flags)
{
    if (flags & 0x200)
        __asm__ volatile ("sti");
}

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void invlpg(uint64_t virt)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

/* ---- Init ---------------------------------------------------------------- */
void vmm_init(uint64_t model_pool_phys)
{
    (void)model_pool_phys;

    g_kernel_pml4 = read_cr3() & PTE_ADDR;

    /* Honour read-only pages in kernel mode too. */
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16);
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    /* No-execute support, if the CPU has it. */
    uint32_t eax = 0x80000001, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (edx & (1u << 20)) {
        uint32_t lo, hi;
        __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
        lo |= (1u << 11);
        __asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080u));
        g_nx_enabled = 1;
    }

    memset(g_spaces, 0, sizeof(g_spaces));
    memset(g_swap, 0, sizeof(g_swap));
    memset(&g_stats, 0, sizeof(g_stats));

    terminal_printf("[VMM] Paging online: kernel PML4 at 0x%llx, write-protect on, NX %s\n",
        g_kernel_pml4, g_nx_enabled ? "on" : "unavailable");
}

void *vmm_map_range(uint64_t phys, uint64_t size, uint32_t flags)
{
    (void)size; (void)flags;
    /* Device memory below 4 GiB is covered by the boot identity map. */
    return (void *)phys;
}

uint64_t vmm_kernel_cr3(void)
{
    return g_kernel_pml4;
}

/* ---- Frames -------------------------------------------------------------- */
static int vm_reclaim_locked(int id, int max_pages, int force);

/* A zeroed frame usable for page tables or data. Evicts pages to swap if
 * RAM has run out. Returns 0 on failure. */
static uint64_t frame_alloc_zeroed(void)
{
    uint64_t f = pmm_alloc_frame();
    if (!f) {
        if (vm_reclaim_locked(0, 32, 0) == 0)
            vm_reclaim_locked(0, 32, 1);
        f = pmm_alloc_frame();
    }
    if (!f) return 0;
    if (f >= 0x100000000ULL) {          /* not reachable via the identity map */
        pmm_free_frame(f);
        return 0;
    }
    memset((void *)f, 0, PAGE_SIZE);
    return f;
}

/* ---- Page table walk ----------------------------------------------------- */
static int vm_user_addr(uint64_t virt)
{
    return virt >= VM_USER_BASE && virt < VM_USER_END;
}

static uint64_t *pte_walk(uint64_t pml4, uint64_t virt, int create)
{
    uint64_t *table = (uint64_t *)pml4;

    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t idx = (virt >> shift) & 511;
        if (!(table[idx] & PTE_P)) {
            if (!create) return NULL;
            uint64_t f = frame_alloc_zeroed();
            if (!f) return NULL;
            table[idx] = f | PTE_P | PTE_W | PTE_U;
            g_stats.table_frames++;
        }
        table = (uint64_t *)(table[idx] & PTE_ADDR);
    }
    return &table[(virt >> 12) & 511];
}

static uint64_t pte_flags(uint32_t flags)
{
    uint64_t f = PTE_P;
    if (flags & VM_WRITE) f |= PTE_W;
    if (flags & VM_USER)  f |= PTE_U;
    if (!(flags & VM_EXEC) && g_nx_enabled) f |= PTE_NX;
    return f;
}

static vm_space_t *space_get(int id)
{
    if (id <= 0 || id >= VM_MAX_SPACES || !g_spaces[id].used) return NULL;
    return &g_spaces[id];
}

static void flush_if_current(vm_space_t *s, uint64_t virt)
{
    if ((read_cr3() & PTE_ADDR) == s->pml4)
        invlpg(virt);
}

/* ---- Address spaces ------------------------------------------------------ */
int vm_space_create(uint32_t owner_pid)
{
    uint64_t irq = vm_irq_save();
    int id = -1;

    for (int i = 1; i < VM_MAX_SPACES; i++) {
        if (!g_spaces[i].used) { id = i; break; }
    }
    uint64_t pml4 = id > 0 ? frame_alloc_zeroed() : 0;
    if (!pml4) {
        vm_irq_restore(irq);
        return -1;
    }

    /* Share the kernel's mappings; everything else starts empty. */
    uint64_t *kernel = (uint64_t *)g_kernel_pml4;
    uint64_t *top = (uint64_t *)pml4;
    top[0] = kernel[0];
    top[511] = kernel[511];

    vm_space_t *s = &g_spaces[id];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->pml4 = pml4;
    s->owner_pid = owner_pid;
    g_stats.spaces++;

    vm_irq_restore(irq);
    return id;
}

static void swap_free(uint32_t slot);

/* Free one page-table level and everything below it. */
static void free_table(uint64_t table_phys, int level, vm_space_t *s)
{
    uint64_t *table = (uint64_t *)table_phys;

    for (int i = 0; i < 512; i++) {
        uint64_t e = table[i];
        if (level == 1) {
            if (e & PTE_P)             pmm_free_frame(e & PTE_ADDR);
            else if (e & PTE_SWAPPED)  swap_free((uint32_t)((e & PTE_ADDR) >> 12));
        } else if (e & PTE_P) {
            free_table(e & PTE_ADDR, level - 1, s);
        }
    }
    pmm_free_frame(table_phys);
    if (g_stats.table_frames) g_stats.table_frames--;
}

void vm_space_destroy(int id)
{
    uint64_t irq = vm_irq_save();
    vm_space_t *s = space_get(id);

    if (s) {
        /* Never free the tables the CPU is using right now. */
        if ((read_cr3() & PTE_ADDR) == s->pml4)
            __asm__ volatile ("mov %0, %%cr3" : : "r"(g_kernel_pml4) : "memory");

        uint64_t *top = (uint64_t *)s->pml4;
        for (int i = 1; i < 256; i++)
            if (top[i] & PTE_P)
                free_table(top[i] & PTE_ADDR, 3, s);
        pmm_free_frame(s->pml4);

        if (g_stats.swapped_pages >= s->swapped) g_stats.swapped_pages -= s->swapped;
        s->used = 0;
        if (g_stats.spaces) g_stats.spaces--;
    }
    vm_irq_restore(irq);
}

uint64_t vm_space_cr3(int id)
{
    vm_space_t *s = space_get(id);
    return s ? s->pml4 : 0;
}

/* ---- Regions and mappings ------------------------------------------------ */
/* Reserve a range of virtual addresses. No memory is used until a page is
 * touched (demand paging). */
int vm_region_add(int id, uint64_t start, uint32_t pages, uint32_t flags, const char *name)
{
    uint64_t irq = vm_irq_save();
    vm_space_t *s = space_get(id);
    int rc = -1;

    if (s && pages && !(start & (PAGE_SIZE - 1)) && vm_user_addr(start) &&
        vm_user_addr(start + (uint64_t)pages * PAGE_SIZE - 1)) {
        for (int i = 0; i < VM_MAX_REGIONS; i++) {
            if (s->regions[i].used) continue;
            vm_region_t *r = &s->regions[i];
            r->used = 1;
            r->start = start;
            r->pages = pages;
            r->flags = flags;
            strncpy(r->name, name ? name : "region", sizeof(r->name) - 1);
            rc = 0;
            break;
        }
    }
    vm_irq_restore(irq);
    return rc;
}

static vm_region_t *region_find(vm_space_t *s, uint64_t virt)
{
    for (int i = 0; i < VM_MAX_REGIONS; i++) {
        vm_region_t *r = &s->regions[i];
        if (r->used && virt >= r->start &&
            virt < r->start + (uint64_t)r->pages * PAGE_SIZE)
            return r;
    }
    return NULL;
}

/* Map one page to a specific physical frame. */
int vm_map_page(int id, uint64_t virt, uint64_t phys, uint32_t flags)
{
    uint64_t irq = vm_irq_save();
    vm_space_t *s = space_get(id);
    int rc = -1;

    if (s && vm_user_addr(virt)) {
        uint64_t *pte = pte_walk(s->pml4, virt & ~(PAGE_SIZE - 1), 1);
        if (pte && !(*pte & (PTE_P | PTE_SWAPPED))) {
            *pte = (phys & PTE_ADDR) | pte_flags(flags);
            s->resident++;
            flush_if_current(s, virt);
            rc = 0;
        }
    }
    vm_irq_restore(irq);
    return rc;
}

int vm_unmap_page(int id, uint64_t virt, int free_frame)
{
    uint64_t irq = vm_irq_save();
    vm_space_t *s = space_get(id);
    int rc = -1;

    if (s && vm_user_addr(virt)) {
        uint64_t *pte = pte_walk(s->pml4, virt, 0);
        if (pte && (*pte & PTE_P)) {
            if (free_frame) pmm_free_frame(*pte & PTE_ADDR);
            *pte = 0;
            if (s->resident) s->resident--;
            flush_if_current(s, virt);
            rc = 0;
        }
    }
    vm_irq_restore(irq);
    return rc;
}

/* Change the protection of a range (and of the region it belongs to, so
 * pages that fault in later get the new protection as well). */
int vm_protect(int id, uint64_t virt, uint32_t pages, uint32_t flags)
{
    uint64_t irq = vm_irq_save();
    vm_space_t *s = space_get(id);
    int rc = -1;

    if (s && vm_user_addr(virt)) {
        vm_region_t *r = region_find(s, virt);
        if (r && r->start == virt && r->pages == pages)
            r->flags = flags;

        for (uint32_t i = 0; i < pages; i++) {
            uint64_t v = (virt & ~(PAGE_SIZE - 1)) + (uint64_t)i * PAGE_SIZE;
            uint64_t *pte = pte_walk(s->pml4, v, 0);
            if (pte && (*pte & PTE_P)) {
                *pte = (*pte & PTE_ADDR) | pte_flags(flags) | (*pte & PTE_A);
                flush_if_current(s, v);
            }
        }
        rc = 0;
    }
    vm_irq_restore(irq);
    return rc;
}

/* ---- Swap store ----------------------------------------------------------
 * Evicted pages are kept compressed in kernel memory (run-length encoding;
 * an all-zero page takes no storage at all). There is no disk driver to
 * swap to yet, so this trades CPU for RAM the same way zram does on Linux.
 */
static size_t rle_compress(const uint8_t *src, uint8_t *dst, size_t dst_max)
{
    size_t out = 0;
    for (size_t i = 0; i < PAGE_SIZE;) {
        uint8_t b = src[i];
        size_t run = 1;
        while (i + run < PAGE_SIZE && src[i + run] == b && run < 255) run++;
        if (out + 2 > dst_max) return 0;        /* does not compress */
        dst[out++] = (uint8_t)run;
        dst[out++] = b;
        i += run;
    }
    return out;
}

static void rle_expand(const uint8_t *src, size_t len, uint8_t *dst)
{
    size_t out = 0;
    for (size_t i = 0; i + 1 < len && out < PAGE_SIZE; i += 2) {
        size_t run = src[i];
        if (out + run > PAGE_SIZE) run = PAGE_SIZE - out;
        memset(dst + out, src[i + 1], run);
        out += run;
    }
}

static void swap_free(uint32_t slot)
{
    if (slot >= SWAP_SLOTS || !g_swap[slot].used) return;
    if (g_swap[slot].data) {
        kfree(g_swap[slot].data);
        if (g_stats.swap_bytes >= g_swap[slot].len) g_stats.swap_bytes -= g_swap[slot].len;
    }
    memset(&g_swap[slot], 0, sizeof(g_swap[slot]));
    if (g_stats.swap_slots_used) g_stats.swap_slots_used--;
}

/* Store a page; returns the slot or -1 if the store is full. */
static int swap_store(const uint8_t *page)
{
    static uint8_t scratch[PAGE_SIZE];
    int slot = -1;

    for (int i = 0; i < SWAP_SLOTS; i++) {
        if (!g_swap[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;

    int all_zero = 1;
    for (size_t i = 0; i < PAGE_SIZE; i++)
        if (page[i]) { all_zero = 0; break; }

    swap_slot_t *sl = &g_swap[slot];
    sl->data = NULL;
    sl->len = 0;

    if (!all_zero) {
        size_t clen = rle_compress(page, scratch, PAGE_SIZE - 64);
        size_t len = clen ? clen : PAGE_SIZE;
        sl->data = (uint8_t *)kmalloc(len);
        if (!sl->data) return -1;
        memcpy(sl->data, clen ? scratch : page, len);
        sl->len = (uint16_t)len;
        g_stats.swap_bytes += len;
    }
    sl->used = 1;
    g_stats.swap_slots_used++;
    return slot;
}

static void swap_load(uint32_t slot, uint8_t *page)
{
    swap_slot_t *sl = &g_swap[slot];
    if (!sl->data)                  memset(page, 0, PAGE_SIZE);
    else if (sl->len == PAGE_SIZE)  memcpy(page, sl->data, PAGE_SIZE);
    else                            rle_expand(sl->data, sl->len, page);
}

static int swap_out_pte(vm_space_t *s, uint64_t virt, uint64_t *pte)
{
    uint64_t frame = *pte & PTE_ADDR;
    int slot = swap_store((const uint8_t *)frame);
    if (slot < 0) return -1;

    *pte = ((uint64_t)slot << 12) | PTE_SWAPPED;
    pmm_free_frame(frame);
    flush_if_current(s, virt);
    if (s->resident) s->resident--;
    s->swapped++;
    g_stats.swapped_pages++;
    g_stats.swap_outs++;
    return 0;
}

/*
 * Evict up to max_pages from one address space (id > 0) or from all (id 0).
 * Second-chance: a page whose Accessed bit is set is spared once (the bit is
 * cleared); a page not touched since is evicted. force skips the grace.
 */
static int vm_reclaim_locked(int id, int max_pages, int force)
{
    int evicted = 0;

    for (int si = 1; si < VM_MAX_SPACES && evicted < max_pages; si++) {
        vm_space_t *s = &g_spaces[si];
        if (!s->used || (id > 0 && si != id)) continue;

        for (int ri = 0; ri < VM_MAX_REGIONS && evicted < max_pages; ri++) {
            vm_region_t *r = &s->regions[ri];
            if (!r->used) continue;

            for (uint32_t p = 0; p < r->pages && evicted < max_pages; p++) {
                uint64_t virt = r->start + (uint64_t)p * PAGE_SIZE;
                uint64_t *pte = pte_walk(s->pml4, virt, 0);
                if (!pte || !(*pte & PTE_P)) continue;

                if ((*pte & PTE_A) && !force) {
                    *pte &= ~PTE_A;
                    flush_if_current(s, virt);
                    continue;
                }
                if (swap_out_pte(s, virt, pte) == 0)
                    evicted++;
            }
        }
    }
    return evicted;
}

int vm_reclaim(int id, int max_pages, int force)
{
    uint64_t irq = vm_irq_save();
    int n = vm_reclaim_locked(id, max_pages, force);
    vm_irq_restore(irq);
    return n;
}

/* ---- Page faults ---------------------------------------------------------
 * Called from the page-fault exception with interrupts off.
 * Returns 0 if the fault was resolved (the instruction is simply retried),
 * or a negative VM_FAULT_* code if the access was genuinely illegal.
 */
int vm_handle_fault(int id, uint64_t addr, uint64_t error_code)
{
    vm_space_t *s = space_get(id);
    if (!s || !vm_user_addr(addr))
        return VM_FAULT_BAD_ADDRESS;

    vm_region_t *r = region_find(s, addr);
    if (!r)
        return VM_FAULT_BAD_ADDRESS;

    uint64_t virt = addr & ~(PAGE_SIZE - 1);
    uint64_t *pte = pte_walk(s->pml4, virt, 1);
    if (!pte)
        return VM_FAULT_NO_MEMORY;

    s->faults++;

    if (*pte & PTE_P) {
        /* The page is there, so this is a protection violation. */
        g_stats.protection_faults++;
        if ((error_code & 0x2) && !(r->flags & VM_WRITE))
            return VM_FAULT_READ_ONLY;
        if (error_code & 0x10)
            return VM_FAULT_NO_EXECUTE;
        return VM_FAULT_PROTECTION;
    }

    uint64_t frame = frame_alloc_zeroed();
    if (!frame)
        return VM_FAULT_NO_MEMORY;

    if (*pte & PTE_SWAPPED) {                   /* bring it back from swap */
        uint32_t slot = (uint32_t)((*pte & PTE_ADDR) >> 12);
        swap_load(slot, (uint8_t *)frame);
        swap_free(slot);
        if (s->swapped) s->swapped--;
        if (g_stats.swapped_pages) g_stats.swapped_pages--;
        g_stats.swap_ins++;
    } else {                                    /* first touch: zero page */
        g_stats.demand_faults++;
    }

    *pte = frame | pte_flags(r->flags) | PTE_A;
    s->resident++;
    invlpg(virt);
    return 0;
}

/* 1 if [addr, addr+len) lies entirely inside user-accessible regions of the
 * address space (and, for a write, writable ones). Used to check every
 * pointer a program hands to a system call. */
int vm_user_range_ok(int id, uint64_t addr, uint64_t len, int write)
{
    uint64_t irq = vm_irq_save();
    vm_space_t *s = space_get(id);
    int ok = s != NULL && addr + len >= addr;

    for (uint64_t at = addr; ok && at < addr + len;) {
        vm_region_t *r = vm_user_addr(at) ? region_find(s, at) : NULL;
        if (!r || !(r->flags & VM_USER) || (write && !(r->flags & VM_WRITE)))
            ok = 0;
        else
            at = r->start + (uint64_t)r->pages * PAGE_SIZE;     /* skip to its end */
    }
    vm_irq_restore(irq);
    return ok;
}

/* ---- Status -------------------------------------------------------------- */
void vm_get_stats(vm_stats_t *out)
{
    uint64_t irq = vm_irq_save();
    *out = g_stats;
    vm_irq_restore(irq);
}

/* Fills out[] with the live address spaces; returns how many. */
int vm_space_list(vm_space_info_t *out, int max)
{
    uint64_t irq = vm_irq_save();
    int n = 0;

    for (int i = 1; i < VM_MAX_SPACES && n < max; i++) {
        vm_space_t *s = &g_spaces[i];
        if (!s->used) continue;
        out[n].id = i;
        out[n].owner_pid = s->owner_pid;
        out[n].resident_pages = s->resident;
        out[n].swapped_pages = s->swapped;
        out[n].faults = s->faults;
        n++;
    }
    vm_irq_restore(irq);
    return n;
}
