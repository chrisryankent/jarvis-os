/*
 * =============================================================================
 * JARVIS OS — Notifications System (desktop/notifications.c)
 * Language: C17
 *
 * Toast notifications and notification center with priority levels.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../drivers/gpu.h"
#include <string.h>
#include <stdio.h>

#define MAX_NOTIFICATIONS 32
#define NOTIFICATION_TIMEOUT_MS 5000
#define TOAST_WIDTH 400
#define TOAST_HEIGHT 100

typedef enum {
    NOTIF_INFO = 0,
    NOTIF_WARNING = 1,
    NOTIF_CRITICAL = 2,
} notification_priority_t;

typedef struct {
    uint32_t id;
    char title[64];
    char body[256];
    notification_priority_t priority;
    uint32_t created_time;
    uint32_t timeout_ms;
    int active;
    int dismissed;
} notification_t;

typedef struct {
    notification_t notifications[MAX_NOTIFICATIONS];
    uint32_t next_id;
    int notification_count;
    int center_visible;
    int scroll_offset;
} notification_context_t;

static notification_context_t g_notif_ctx = {};

void notifications_init(void)
{
    memset(&g_notif_ctx, 0, sizeof(g_notif_ctx));
    g_notif_ctx.next_id = 1;
}

uint32_t notification_show(const char *title, const char *body,
                           notification_priority_t priority, uint32_t timeout_ms)
{
    if (g_notif_ctx.notification_count >= MAX_NOTIFICATIONS)
        return 0;

    notification_t *notif = &g_notif_ctx.notifications[g_notif_ctx.notification_count];
    notif->id = g_notif_ctx.next_id++;
    notif->priority = priority;
    notif->created_time = g_kstate.uptime_ticks / 100;  /* Convert to seconds */
    notif->timeout_ms = timeout_ms;
    notif->active = 1;
    notif->dismissed = 0;

    strncpy(notif->title, title, sizeof(notif->title) - 1);
    strncpy(notif->body, body, sizeof(notif->body) - 1);

    g_notif_ctx.notification_count++;
    return notif->id;
}

void notification_dismiss(uint32_t id)
{
    for (int i = 0; i < g_notif_ctx.notification_count; i++) {
        if (g_notif_ctx.notifications[i].id == id) {
            g_notif_ctx.notifications[i].dismissed = 1;
            break;
        }
    }
}

void notifications_update(void)
{
    uint32_t now = g_kstate.uptime_ticks / 100;

    /* Remove expired notifications */
    for (int i = 0; i < g_notif_ctx.notification_count; i++) {
        notification_t *notif = &g_notif_ctx.notifications[i];
        uint32_t age = (now - notif->created_time) * 1000;  /* ms */

        if (age > notif->timeout_ms || notif->dismissed) {
            /* Remove by shifting */
            for (int j = i; j < g_notif_ctx.notification_count - 1; j++) {
                g_notif_ctx.notifications[j] = g_notif_ctx.notifications[j + 1];
            }
            g_notif_ctx.notification_count--;
            i--;
        }
    }
}

void notifications_draw_toasts(void)
{
    uint32_t W = fb_get_width();
    int x = W - TOAST_WIDTH - 20;
    int y = 80;

    /* Draw up to 3 toasts (top-right corner) */
    int toast_count = 0;
    for (int i = 0; i < g_notif_ctx.notification_count && toast_count < 3; i++) {
        notification_t *notif = &g_notif_ctx.notifications[i];
        if (notif->dismissed) continue;

        /* Toast background color based on priority */
        uint32_t bg_color;
        uint32_t border_color;
        switch (notif->priority) {
        case NOTIF_INFO:
            bg_color = 0x00284E8C;      /* Blue */
            border_color = 0x005BA3D6;
            break;
        case NOTIF_WARNING:
            bg_color = 0x008B6B00;      /* Orange */
            border_color = 0x00FFB703;
            break;
        case NOTIF_CRITICAL:
            bg_color = 0x008B0000;      /* Red */
            border_color = 0x00FF3B5C;
            break;
        default:
            bg_color = 0x003A3A3A;
            border_color = 0x00FFFFFF;
        }

        /* Draw toast */
        fb_fill_rect(x, y, TOAST_WIDTH, TOAST_HEIGHT, bg_color);
        fb_fill_rect(x, y, 4, TOAST_HEIGHT, border_color);

        /* Draw title and body */
        fb_put_string(x + 12, y + 8, notif->title, border_color, bg_color);
        fb_put_string(x + 12, y + 28, notif->body, 0x00FFFFFF, bg_color);

        /* Progress bar (time remaining) */
        uint32_t now = g_kstate.uptime_ticks / 100;
        uint32_t age_ms = (now - notif->created_time) * 1000;
        int progress = (age_ms * TOAST_WIDTH) / notif->timeout_ms;
        if (progress > TOAST_WIDTH) progress = TOAST_WIDTH;

        fb_fill_rect(x, y + TOAST_HEIGHT - 4, progress, 4, border_color);

        y += TOAST_HEIGHT + 12;
        toast_count++;
    }
}

void notifications_draw_center(void)
{
    if (!g_notif_ctx.center_visible)
        return;

    uint32_t W = fb_get_width();
    uint32_t H = fb_get_height();

    int x = W - 420;
    int y = 60;
    int w = 400;
    int h = H - 140;

    /* Notification center panel */
    fb_fill_rect(x, y, w, h, 0x001F1F1F);
    fb_fill_rect(x, y, 1, h, 0x005BA3D6);

    /* Title */
    fb_put_string(x + 12, y + 8, "Notifications", 0x00E0E0E0, 0x001F1F1F);

    /* Clear all button */
    fb_fill_rect(x + w - 120, y + 6, 110, 24, 0x005BA3D6);
    fb_put_string(x + w - 110, y + 10, "Clear All", 0x00FFFFFF, 0x005BA3D6);

    /* Separator */
    fb_fill_rect(x + 12, y + 40, w - 24, 1, 0x003A3A3A);

    /* Notification list */
    int list_y = y + 50;
    for (int i = g_notif_ctx.scroll_offset;
         i < g_notif_ctx.notification_count && list_y < y + h - 40;
         i++) {
        notification_t *notif = &g_notif_ctx.notifications[i];

        /* Priority indicator color */
        uint32_t priority_color;
        switch (notif->priority) {
        case NOTIF_INFO:
            priority_color = 0x005BA3D6;
            break;
        case NOTIF_WARNING:
            priority_color = 0x00FFB703;
            break;
        case NOTIF_CRITICAL:
            priority_color = 0x00FF3B5C;
            break;
        default:
            priority_color = 0x00808080;
        }

        /* Notification item */
        fb_fill_rect(x + 8, list_y, w - 16, 70, 0x002D2D2D);
        fb_fill_rect(x + 8, list_y, 3, 70, priority_color);

        /* Title and body */
        fb_put_string(x + 16, list_y + 4, notif->title, priority_color, 0x002D2D2D);
        fb_put_string(x + 16, list_y + 22, notif->body, 0x00A0A0A0, 0x002D2D2D);

        /* Time ago */
        char time_str[32];
        uint32_t age_sec = g_kstate.uptime_ticks / 100 - notif->created_time;
        if (age_sec < 60)
            snprintf(time_str, sizeof(time_str), "%u sec ago", age_sec);
        else
            snprintf(time_str, sizeof(time_str), "%u min ago", age_sec / 60);
        fb_put_string(x + 16, list_y + 50, time_str, 0x00808080, 0x002D2D2D);

        list_y += 78;
    }
}

void notifications_draw(void)
{
    notifications_update();
    notifications_draw_toasts();
    notifications_draw_center();
}

void notifications_toggle_center(void)
{
    g_notif_ctx.center_visible = !g_notif_ctx.center_visible;
}

int notifications_center_is_visible(void)
{
    return g_notif_ctx.center_visible;
}

int notifications_get_count(void)
{
    return g_notif_ctx.notification_count;
}

int notifications_get_unread_count(void)
{
    int count = 0;
    for (int i = 0; i < g_notif_ctx.notification_count; i++) {
        if (!g_notif_ctx.notifications[i].dismissed)
            count++;
    }
    return count;
}
