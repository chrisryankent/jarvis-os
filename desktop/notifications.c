/*
 * =============================================================================
 * JARVIS OS — Notifications (desktop/notifications.c)
 * Language: C17
 *
 * Toasts slide in at the bottom-right above the taskbar and expire on their
 * own; every notification is also kept in the notification centre (opened
 * from the bell / clock in the system tray) until cleared.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "ui.h"
#include <string.h>
#include <stdio.h>

#define MAX_NOTIFICATIONS 24
#define TOAST_W   340
#define TOAST_H   76
#define CENTER_W  360

typedef struct {
    uint32_t id;
    char title[48];
    char body[160];
    notification_priority_t priority;
    uint64_t created_tick;
    uint64_t toast_until_tick;      /* 0 once the toast has gone */
    int unread;
} notification_t;

static notification_t g_notifs[MAX_NOTIFICATIONS];   /* oldest first */
static int g_notif_count = 0;
static uint32_t g_notif_next_id = 1;
static int g_center_visible = 0;

void notifications_init(void)
{
    memset(g_notifs, 0, sizeof(g_notifs));
    g_notif_count = 0;
    g_notif_next_id = 1;
    g_center_visible = 0;
}

static uint32_t priority_color(notification_priority_t p)
{
    switch (p) {
    case NOTIF_WARNING:  return T(WARN);
    case NOTIF_CRITICAL: return T(DANGER);
    default:             return T(ACCENT);
    }
}

uint32_t notification_show(const char *title, const char *body,
                           notification_priority_t priority, uint32_t timeout_ms)
{
    if (g_notif_count >= MAX_NOTIFICATIONS) {       /* drop the oldest */
        memmove(&g_notifs[0], &g_notifs[1],
                sizeof(notification_t) * (MAX_NOTIFICATIONS - 1));
        g_notif_count = MAX_NOTIFICATIONS - 1;
    }

    notification_t *n = &g_notifs[g_notif_count++];
    memset(n, 0, sizeof(*n));
    n->id = g_notif_next_id++;
    n->priority = priority;
    n->created_tick = g_kstate.uptime_ticks;
    n->toast_until_tick = n->created_tick + (timeout_ms ? timeout_ms : 4000) / 10;
    /* Do not disturb: kept in the notification centre, no pop-up (critical
     * ones still show). */
    if (setting_get_int("dnd", 0) && priority != NOTIF_CRITICAL) n->toast_until_tick = 0;
    /* Accessibility: JARVIS reads notifications aloud. */
    if (setting_get_int("narrator", 0)) {
        char spoken[220];
        snprintf(spoken, sizeof(spoken), "%s. %s", title ? title : "", body ? body : "");
        tts_speak(spoken, strlen(spoken));
    }
    n->unread = !g_center_visible;
    strncpy(n->title, title ? title : "", sizeof(n->title) - 1);
    strncpy(n->body, body ? body : "", sizeof(n->body) - 1);

    desktop_invalidate();
    return n->id;
}

/* Expire toasts. Returns 1 if anything changed on screen. */
int notifications_tick(void)
{
    int changed = 0;
    for (int i = 0; i < g_notif_count; i++) {
        if (g_notifs[i].toast_until_tick &&
            g_kstate.uptime_ticks >= g_notifs[i].toast_until_tick) {
            g_notifs[i].toast_until_tick = 0;
            changed = 1;
        }
    }
    return changed;
}

static void draw_toasts(void)
{
    int W = (int)fb_get_width();
    int x = W - TOAST_W - 12;
    int y = (int)fb_get_height() - TASKBAR_H - 12 - TOAST_H;
    int shown = 0;

    /* newest at the bottom, up to three */
    for (int i = g_notif_count - 1; i >= 0 && shown < 3; i--) {
        notification_t *n = &g_notifs[i];
        if (!n->toast_until_tick) continue;

        uint32_t col = priority_color(n->priority);
        fb_blend_rounded_rect(x + 2, y + 4, TOAST_W, TOAST_H, 8, 0x00000000, 70);
        fb_fill_rounded_rect(x, y, TOAST_W, TOAST_H, 8, T(BORDER));
        fb_fill_rounded_rect(x + 1, y + 1, TOAST_W - 2, TOAST_H - 2, 7, T(MENU));
        fb_fill_rounded_rect(x + 10, y + 14, 4, TOAST_H - 28, 2, col);

        fb_draw_text_bold(x + 24, y + 14, n->title, T(TEXT));
        ui_text_fit(x + 24, y + 38, TOAST_W - 36, n->body, T(TEXT_DIM));

        y -= TOAST_H + 8;
        shown++;
    }
}

static void center_rect(int *x, int *y, int *w, int *h)
{
    *w = CENTER_W;
    *x = (int)fb_get_width() - CENTER_W - 12;
    *y = 12;
    *h = (int)fb_get_height() - TASKBAR_H - 24;
}

static void draw_center(void)
{
    int x, y, w, h;
    center_rect(&x, &y, &w, &h);

    fb_blend_rounded_rect(x + 2, y + 4, w, h, 8, 0x00000000, 70);
    fb_fill_rounded_rect(x, y, w, h, 8, T(BORDER));
    fb_fill_rounded_rect(x + 1, y + 1, w - 2, h - 2, 7, T(MENU));

    /* Clock header */
    char clock[12], date[32];
    sys_format_clock(clock, sizeof(clock));
    sys_format_date(date, sizeof(date));
    fb_draw_text_scaled(x + 20, y + 18, clock, T(TEXT), 2);
    fb_draw_text(x + 20, y + 54, date, T(TEXT_DIM));
    fb_fill_rect(x + 16, y + 82, w - 32, 1, T(BORDER));

    fb_draw_text_bold(x + 20, y + 94, "Notifications", T(TEXT));
    if (g_notif_count > 0)
        ui_button(x + w - 100, y + 88, 84, 26, "Clear all", T(HOVER), T(TEXT));

    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x + 1, y + 122, w - 2, h - 130);

    if (g_notif_count == 0)
        ui_text_center(x, y + 160, w, "No new notifications", T(TEXT_DIM));

    int cy = y + 124;
    for (int i = g_notif_count - 1; i >= 0 && cy < y + h - 8; i--) {
        notification_t *n = &g_notifs[i];
        ui_card(x + 12, cy, w - 24, 66);
        fb_fill_rounded_rect(x + 20, cy + 12, 4, 42, 2, priority_color(n->priority));
        fb_draw_text_bold(x + 34, cy + 8, n->title, T(TEXT));
        ui_text_fit(x + 34, cy + 28, w - 60, n->body, T(TEXT_DIM));

        char age[24];
        uint32_t secs = (uint32_t)((g_kstate.uptime_ticks - n->created_tick) / 100);
        if (secs < 60) snprintf(age, sizeof(age), "just now");
        else           snprintf(age, sizeof(age), "%u min ago", secs / 60);
        fb_draw_text(x + 34, cy + 46, age, T(TEXT_DIM));
        cy += 72;
    }
    fb_restore_clip(saved);
}

void notifications_draw(void)
{
    if (g_center_visible)
        draw_center();
    else
        draw_toasts();
}

void notifications_toggle_center(void)
{
    g_center_visible = !g_center_visible;
    if (g_center_visible) {
        for (int i = 0; i < g_notif_count; i++) {
            g_notifs[i].unread = 0;
            g_notifs[i].toast_until_tick = 0;
        }
    }
    desktop_invalidate();
}

void notifications_close_center(void)
{
    if (g_center_visible) {
        g_center_visible = 0;
        desktop_invalidate();
    }
}

int notifications_center_is_visible(void)
{
    return g_center_visible;
}

/* Click while the centre is open. Returns 1 if the click landed inside it. */
int notifications_click(int px, int py)
{
    if (!g_center_visible) return 0;

    int x, y, w, h;
    center_rect(&x, &y, &w, &h);
    if (!ui_hit(px, py, x, y, w, h)) return 0;

    if (g_notif_count > 0 && ui_hit(px, py, x + w - 100, y + 88, 84, 26)) {
        g_notif_count = 0;
        desktop_invalidate();
    }
    return 1;
}

int notifications_get_count(void)
{
    return g_notif_count;
}

int notifications_get_unread_count(void)
{
    int n = 0;
    for (int i = 0; i < g_notif_count; i++)
        if (g_notifs[i].unread) n++;
    return n;
}
