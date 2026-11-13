/*
 * =============================================================================
 * JARVIS AI OS — Resource governor (kernel/governor.c)
 * Language: C17
 *
 * Decides *policy* for the mechanisms in the scheduler and memory manager:
 * which process to slow down, when to evict memory. Two sources of policy:
 *
 *   AI          when the assistant is online it sends a heartbeat and may
 *               submit advice (governor_ai_advise). Advice is validated like
 *               any other request: it cannot touch protected processes.
 *   built-in    fixed rules that run whenever the AI has not been heard from
 *               for GOV_AI_TIMEOUT ticks, so the machine manages itself with
 *               the AI offline, crashed or not yet loaded.
 *
 * The memory safety floor runs in both modes: the AI can add to it but can
 * never switch it off.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

#define GOV_AI_TIMEOUT   500        /* 5 s without a heartbeat = AI offline */
#define GOV_MAX_PROCS    64
#define GOV_HOG_SECONDS  5

static uint64_t g_ai_last_beat = 0;
static int      g_ai_seen = 0;
static uint64_t g_last_run = 0;
static governor_stats_t g_gov;

static struct { uint32_t pid; uint64_t ticks; int hot_seconds; } g_track[GOV_MAX_PROCS];
static int g_track_count = 0;

int governor_ai_online(void)
{
    return g_ai_seen && g_kstate.uptime_ticks - g_ai_last_beat < GOV_AI_TIMEOUT;
}

/* Called by the AI runtime while it is alive and making decisions. */
void governor_ai_heartbeat(void)
{
    g_ai_last_beat = g_kstate.uptime_ticks;
    g_ai_seen = 1;
}

/* Advice from the AI. Returns 0 if applied, -1 if refused. */
int governor_ai_advise(int action, uint32_t pid, int value)
{
    int rc = -1;

    governor_ai_heartbeat();
    switch (action) {
    case GOV_SET_PRIORITY: rc = proc_set_priority(pid, value); break;
    case GOV_SUSPEND:      rc = proc_suspend(pid);             break;
    case GOV_RESUME:       rc = proc_resume(pid);              break;
    case GOV_TERMINATE:    rc = proc_kill(pid);                break;
    case GOV_RECLAIM:      rc = vm_reclaim(0, value > 0 ? value : 32, 0) >= 0 ? 0 : -1; break;
    default: break;
    }
    if (rc == 0) g_gov.ai_actions++;
    else         g_gov.ai_refused++;
    return rc;
}

/* Built-in CPU rule: a process above the lowest priority that has used more
 * than 90% of the CPU for GOV_HOG_SECONDS in a row is moved to the lowest
 * priority, so it can no longer starve everything else. */
static void builtin_cpu_policy(void)
{
    static proc_info_t procs[GOV_MAX_PROCS];
    int n = proc_list(procs, GOV_MAX_PROCS);

    for (int i = 0; i < n; i++) {
        proc_info_t *p = &procs[i];
        int slot = -1;
        for (int k = 0; k < g_track_count; k++)
            if (g_track[k].pid == p->pid) slot = k;
        if (slot < 0) {
            if (g_track_count >= GOV_MAX_PROCS) g_track_count = 0;
            slot = g_track_count++;
            g_track[slot].pid = p->pid;
            g_track[slot].ticks = p->cpu_ticks;
            g_track[slot].hot_seconds = 0;
            continue;
        }

        uint64_t used = p->cpu_ticks - g_track[slot].ticks;
        g_track[slot].ticks = p->cpu_ticks;
        g_track[slot].hot_seconds = used >= 90 ? g_track[slot].hot_seconds + 1 : 0;

        if (g_track[slot].hot_seconds >= GOV_HOG_SECONDS && p->priority < PRIO_USER &&
            !(p->flags & (PROC_FLAG_SYSTEM | PROC_FLAG_SERVICE))) {
            if (proc_set_priority(p->pid, PRIO_USER) == 0) {
                char msg[96];
                snprintf(msg, sizeof(msg), "%s (PID %u) was using all the CPU; priority lowered",
                         p->name, p->pid);
                scheduler_post_event(msg);
                g_gov.builtin_actions++;
            }
            g_track[slot].hot_seconds = 0;
        }
    }
}

/* Runs about once a second from the desktop process. */
void governor_tick(void)
{
    uint64_t now = g_kstate.uptime_ticks;
    if (now - g_last_run < 100)
        return;
    g_last_run = now;

    /* Safety floor, always on: keep at least 5% of RAM free by evicting
     * idle pages to swap. */
    uint64_t total, free_frames;
    pmm_get_stats(&total, &free_frames);
    if (total && free_frames < total / 20) {
        int n = vm_reclaim(0, 64, 0);
        if (n > 0) {
            g_gov.pages_reclaimed += (uint32_t)n;
            g_gov.builtin_actions++;
        }
    }

    if (!governor_ai_online())
        builtin_cpu_policy();
}

void governor_get_stats(governor_stats_t *out)
{
    *out = g_gov;
    out->ai_online = governor_ai_online();
}
