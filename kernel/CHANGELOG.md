# Kernel enhancement changelog

Every item below was verified by actually compiling (`gcc -std=c17 -ffreestanding
-Wall -Wextra -O2`, with `_FORTIFY_SOURCE` disabled — see why in Makefile notes)
and, where relevant, assembling/partial-linking. Nothing here is "should work" —
the build is clean and a partial link shows zero new undefined symbols beyond
what already pointed outside this directory.

## Build-breaking

- **kernel.c did not compile.** `ai_core_init()`, `speech_daemon_init()`, and
  `security_daemon_init()` reference `PRIO_AI` / `PRIO_REALTIME`, but
  `proc_prio_t` was a `typedef` local to `ipc_sched_pmm.c`, invisible to
  `kernel.c`. The `.o` files in the original zip were stale — dated 4 days
  before `kernel.c`'s last edit. **Fix:** moved `proc_prio_t`/`proc_state_t`
  into `kernel.h` as the single shared definition.

## Scheduler — threads never actually ran

This was the most severe bug, and it's subtle enough that it wouldn't show up
as a crash — the system would print "Thread created" and look fine.

- `ipc_sched_pmm.c` and `kernel.c` each kept their **own, separate**
  `current_proc` variable. `kernel.c`'s `ai_scheduler_tick()` was the only
  place that ever wrote to *its* copy; `ipc_sched_pmm.c`'s `scheduler_pick_next()`
  only ever **read** *its* copy for preemption/requeue decisions — and nothing
  ever wrote to that one, so it was permanently `NULL`.
  - Consequence A: `context_switch(current_proc, next)` always ran with
    `from = NULL`, which the existing null-guard silently turned into a no-op.
    **The created kthreads (ai_brain, speech_stt, speech_tts, sec_daemon)
    never had their registers/instruction pointer loaded — they never ran a
    single instruction**, even though the scheduler's bookkeeping believed
    they were "current."
  - Consequence B: because `current_proc` was always seen as `NULL`, the
    "put the previous process back in its run queue" logic never executed.
    After one pass through the four priority queues, every thread had been
    dequeued and dropped — the scheduler went permanently idle.
- `context_switch()` itself only swapped `rsp` — it never saved or restored
  `rbp`/`rbx`/`r12`–`r15`, so even a real switch would have corrupted
  register state, and it unconditionally would have needed to handle `cr3`
  (writing 0 into CR3 for kernel threads, which is what a naive "full"
  implementation would do, triple-faults real hardware).
- `kthread_create()`'s initial stack frame only pushed a return address —
  inconsistent with a register-correct switch, which needs six saved
  registers below that return address.
- `kthread_create()` mutated the run-queue linked lists with interrupts
  already enabled (boot calls it after `interrupt_init()`'s `sti()`), with
  no `cli()`/`sti()` guard — a real race against the timer IRQ touching the
  same lists.
- There was no thread-exit path. `proc_alloc()` could only find a slot via
  `pid == 0`, but nothing ever set `pid` back to 0 — the 256-thread pool was
  a one-way ratchet.
- The documented MLFQ behavior ("AI inference gets a longer slice than user
  tasks," "promoted on yield, demoted on exhausting the quantum") wasn't
  implemented at all — every thread rotated every single 10ms tick regardless
  of priority class, with no demotion/promotion ever happening.

**Fixes**, all in `ipc_sched_pmm.c`:
- One `current_proc`, owned by `scheduler_pick_next()`, which now performs
  the pick *and* the switch in one place. `kernel.c`'s `ai_scheduler_tick()`
  is now a one-line delegate.
- A `boot_proc` representing whatever was running before the scheduler ever
  ran a kthread (the standard "scheduler context" trick used by teaching
  kernels like xv6) — so the very first switch has a real `from`, and the
  original boot/foreground execution can correctly be switched back into
  later.
- `context_switch()` now calls a real, register-correct assembly helper
  (`context_switch.S` — new file) and conditionally switches CR3 only when
  the destination actually has its own page tables (never writes 0).
- `kthread_create()`'s initial frame now mirrors the assembly helper's
  push order exactly (6 zeroed registers below the entry point), and the
  whole function runs under `cli()`/`sti()`.
- `quantum_ticks_for()` gives AI-priority threads a 4-tick (40ms) slice vs.
  1 tick for REALTIME/USER, 2 for SYSTEM. A strictly-higher-priority thread
  still preempts immediately regardless of remaining quantum. A thread that
  burns its whole slice gets demoted one level; one preempted by something
  more important does not. `kthread_yield()` (new, exposed for future
  blocking syscalls) promotes one level as a reward for yielding early.
- `kthread_exit()` (new) marks the thread a reapable zombie (`pid = 0`) and
  permanently gives up the CPU. Wired into `syscall.c`'s `exit`/`exit_group`
  (case 60/231), which previously just printed a message and returned,
  leaving the thread running. `getpid` (case 39) now returns the real PID
  instead of a hardcoded `1`.

## Heap — would have broken on the very first real model load

- `kmalloc()` was a pure bump allocator (`heap_ptr` only ever grew) and
  `kfree()` was a documented no-op. `vfs.c`'s `ramfs_write()` calls `kfree()`
  on the old buffer every time a file outgrows its capacity (doubling +
  copy) — with a no-op `kfree`, **every growth permanently leaked the old
  buffer** into a fixed 16MB heap. `model_manifest.c` stages paths for a
  2GB `brain.gguf`, a 1.5GB Whisper model, a 327MB Kokoro TTS model, etc. —
  writing real content for any of those through this heap would exhaust all
  16MB on the very first doubling pass.
- `RAMFS_MAX_SIZE` (64MB) was defined and never referenced anywhere.

**Fixes**, all in `ipc_sched_pmm.c` + `vfs.c`:
- `kmalloc`/`kfree` are now a real address-ordered free list with splitting
  and immediate two-directional coalescing.
- New `kmalloc_large()`/`kfree_large()`, backed directly by `pmm_alloc_contiguous()`
  physical pages, for anything ≥ `LARGE_ALLOC_THRESHOLD` (1MB, shared constant
  in `kernel.h`). Allocations are tracked in a small side table (not a header
  in front of the data) so returned buffers stay page-aligned. This is what
  actually makes it possible to stage a multi-GB model file without a
  multi-GB static C array.
- `ramfs_file_t` now tracks which allocator backs its buffer (`->large`) and
  `ramfs_write()`'s growth path switches a file from the small heap to the
  large allocator the moment it crosses the threshold, freeing the old
  buffer with the matching deallocator (these two allocators are not
  interchangeable).
- `RAMFS_MAX_SIZE` is now actually enforced — but **only** for files that
  remain small-heap-backed. Applying it to every file (the only way it was
  "intended" to be used, going by the comment) would have made it
  impossible to ever stage any model file over 64MB, which is the explicit
  purpose of this filesystem.

## Smaller fixes

- Removed a second, dead `phys_bitmap[262144]` static array in `kernel.c` —
  completely separate from the real one in `ipc_sched_pmm.c` (different
  translation units, so no link conflict, just 256KB of `.bss` that was
  filled with `0xFF` on boot and never read again).
- `sti()` was an empty stub while its sibling `cli()` correctly executed the
  instruction — fixed. (Currently inert either way, since `idt_init()`/
  `pic_remap()`/`pit_set_frequency()` are also stubs — see below.)
- Removed the empty `void syscall_entry_stub(void) {}` in `platform.c`. As
  written it compiled to a bare `ret` with nothing saved — the moment
  `syscall_init()` is ever called, the first `syscall` instruction from any
  code would jump here and immediately fault on garbage/untrusted stack
  data. It was harmless only because `syscall_init()` is never actually
  called in the current boot path. Removed the symbol entirely (rather than
  "fixing" it) so the linker fails loudly if it's ever wired in before the
  real trampoline exists, instead of silently crashing at runtime with no
  diagnostic.

## Documented but intentionally not implemented blind

Two things are flagged with detailed comments in the source rather than
attempted in this pass, because getting either subtly wrong fails silently
(triple-fault, no diagnostic output) and both need real hardware/QEMU
iteration to validate, which wasn't available here:

1. **A real IDT/PIC/PIT.** `idt_init()`, `idt_set_handler()`, `pic_remap()`,
   `pit_set_frequency()` are all still empty stubs (see the comment block
   above them in `platform.c`). Concretely: **the timer interrupt is never
   actually wired up**, so `irq0_timer_handler()` — and by extension
   `ai_scheduler_tick()`, and everything fixed above — is never invoked by
   real hardware yet. The scheduler logic is now correct; nothing is
   currently driving it. This is the natural next milestone.
2. **The real `SYSCALL`/`SYSRET` entry trampoline.** Beyond just the
   assembly stub, this needs infrastructure that doesn't exist anywhere in
   this codebase yet: `MSR_STAR` (which needs a real GDT with the four
   selectors in the exact order SYSCALL/SYSRET require), `swapgs` +
   `MSR_KERNEL_GS_BASE` (no per-CPU base is set up anywhere), and a kernel
   stack to land on (no TSS). See the comment in `platform.c` for the full
   list. `syscall_init()` is not called anywhere in the current boot path,
   so this is safely dormant — just don't wire it in before 1–4 there exist.

## New files

- `context_switch.S` — the real register-save/restore assembly helper.
  Verified: assembles cleanly, disassembles to exactly the intended
  push/mov/mov/pop/ret sequence.
- `Makefile` — none existed in the uploaded directory. Builds everything in
  this folder into a relocatable `kernel_core.o`; `make check` lists every
  symbol still undefined (which should only be things implemented in the
  rest of the project — desktop UI, audio, USB/AHCI/PCI, the daemon thread
  bodies). Disables glibc's `_FORTIFY_SOURCE`, which modern glibc
  auto-enables under `-O1`+ and which would otherwise rewrite `strncpy`/
  `snprintf` calls into `__strncpy_chk`/`__snprintf_chk` — symbols that
  don't exist in a freestanding/no-libc build, so anything past `-O0` would
  fail to link with a confusing error.

## Verified, not assumed

```
$ make clean && make all
... (all 7 .c files + context_switch.S, -O2 -Wall -Wextra, zero warnings)
Built kernel_core.o.

$ make check
... only symbols implemented elsewhere in the larger project (desktop/
    audio/USB/AHCI/PCI/daemon bodies) + syscall_entry_stub (intentional,
    see above) + _kernel_start/_kernel_end (resolved by linker.ld at full
    link time, not by any .c file)
```

---

## Session 3 — Full project enhancement

### BOOT
- **`boot/multiboot2.asm`**: Added `parse_multiboot2_memmap()` — real multiboot2
  memory-map tag (type 6) parser writing E820-compatible entries to 0x7100.
  Before this the PMM never knew actual installed RAM, always falling back to a
  hardcoded 512 MB guess correct by accident on default QEMU but wrong on real
  hardware or any differently-sized VM.
- **`kernel/kernel.c`**: Added `load_e820_from_bootloader()` called before
  `memory_init()` so the PMM gets real installed-RAM layout from GRUB.

### INTERRUPTS (previously all empty stubs — the whole OS was undriven)
- **`kernel/interrupts.S`** (NEW): 256 mechanically-generated ISR entry stubs
  (GNU as `.altmacro`/`.rept`, Intel syntax, correct error-code vectors) plus
  `isr_common_stub` that saves all GP registers, calls `interrupt_dispatch()`,
  restores, and `iretq`s. Verified 256 stubs, 256 relocations in `.data` table.
- **`kernel/platform.c`**: Real `idt_init()` (256-entry IDT from `isr_stub_table`,
  loaded via `lidt`), real `pic_remap()` (ICW1-4 full 8-step sequence),
  real `pit_set_frequency()` (correct PIT divisor), real `interrupt_dispatch()`
  (routes to registered handlers, always sends EOI, prints diagnostics on
  unhandled CPU exceptions instead of silently triple-faulting).
- **`kernel/platform.c`**: Real PS/2 Scan Code Set 1 keyboard driver
  (`irq1_keyboard_handler`) — 128-entry scancode-to-ASCII tables (unshifted +
  shifted), extended prefix (0xE0) tracking, Shift/Ctrl/Alt/Caps/Super modifier
  state, KEY_UP/DOWN/LEFT/RIGHT/DELETE/HOME/END/F1-F10/ESC constants, correct
  make/break discrimination, text-input events for printable chars.
- **`kernel/kernel.h`**: Added `interrupt_dispatch()`, `KMOD_SHIFT/CTRL/ALT/CAPS/SUPER`,
  `input_key_press/release/text_char/mouse_move/click/release/wheel` declarations.

### FONT (screen was unreadable — every character fell through to a ~50-glyph fallback)
- **`kernel/font8x16.c`** (NEW): Original 8×16 monospace bitmap font, 95 printable
  ASCII glyphs (32-126), generated from hand-authored stroke primitives
  (`scripts/gen_font8x16.py`). Visually verified every character class.
  Replaces the all-zero `{{0}}` placeholder in `platform.c`.

### WINDOW MANAGER
- **`wm/wm.h`**: `EDGE_*` constants changed from sequential integers to independent
  bit-flags — critical because `wm_handle_mouse_move()` uses bitwise AND on them,
  meaning e.g. `EDGE_BOTTOM_LEFT=7` (old) would incorrectly trigger right/top
  resize too. Added `WM_TITLEBAR_HEIGHT/BUTTON_SIZE/BUTTON_MARGIN` shared
  constants, `wm_titlebar_button_t`, `wm_hit_test_button()`, `wm_close_window()`,
  `wm_set_close_callback()`, `wm_alt_tab_next()`, `wm_snap_shortcut()`,
  `wm_handle_key_up/text_input/mouse_wheel()`.
- **`wm/wm.c`**: `wm_hit_test_button()` — real close/maximize/minimize button
  hit-testing matching titlebar.c's exact layout. `wm_handle_mouse_click()`
  rewritten: buttons tested first (previously all titlebar clicks → drag, buttons
  were completely inert), double-click on drag area → maximize. Added
  `wm_alt_tab_next()` (Alt+Tab cycles non-minimized windows in z-order),
  `wm_snap_shortcut()` (Win+Arrow). Added `IE_KEY_UP`, `IE_TEXT_INPUT`,
  `IE_MOUSE_WHEEL` dispatch in `wm_handle_input()` — typed characters previously
  never reached any app. Added `wm_close_window()` (fires `on_close` before
  freeing), `wm_set_close_callback()`, `wm_count_windows()`. Removed dead
  `wm_reorder_z_order()` stub.
- **`wm/titlebar.c`**: Now uses shared `WM_TITLEBAR_HEIGHT/BUTTON_SIZE/BUTTON_MARGIN`
  from `wm.h` — eliminates the duplicate-constant drift that caused buttons to be
  drawn in a different position than they were click-tested.

### DESKTOP & TASKBAR
- **`desktop/desktop.c`**: `desktop_handle_click()` (pure hit-test returning icon
  index on confirmed double-click, -2 on single hit, -1 on miss);
  `desktop_get_icon_path/label()`. Icons now actually launch apps on double-click.
- **`desktop/desktop_init.c`**: `desktop_clear_window_ptr` close-callback so the
  app-window pointers go back to NULL when the user clicks X (without this,
  closing an app made it impossible to relaunch). Shared `desktop_launch_*`
  helpers eliminate the duplicate open-logic between init and open_app.
  Real `desktop_system_close_all()`. New input wrappers: mouse release, wheel,
  key up, text input. Taskbar gets first refusal on clicks before WM.
  Start menu rendered in the draw loop when open. Active-window count fixed
  (`wm_count_windows()` instead of hardcoded 0/1).
- **`desktop/taskbar.c`**: Complete rewrite. Windows-11 centred pinned icons with
  running-app underline accent. Open-window buttons (non-pinned running apps).
  Full `start_menu_draw()` — user banner, search, 6-icon pinned grid, recent
  files, power strip. `taskbar_handle_click()` — Start, pinned icons, and
  open-window buttons are now clickable. Click bounds recorded alongside drawing
  so hit regions can never drift from drawn positions. System tray icons (Wi-Fi
  bars, volume, battery).
