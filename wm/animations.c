/*
 * =============================================================================
 * JARVIS OS — Animation Framework (wm/animations.c)
 * Language: C17
 *
 * Easing functions and animation system for smooth transitions.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include <string.h>
#include <math.h>

typedef enum {
    EASING_LINEAR,
    EASING_EASE_IN,
    EASING_EASE_OUT,
    EASING_EASE_IN_OUT,
    EASING_BOUNCE,
    EASING_ELASTIC,
} easing_type_t;

typedef struct {
    uint32_t id;
    int target_int;
    int start_value;
    int end_value;
    uint32_t start_time;
    uint32_t duration_ms;
    easing_type_t easing;
    int *value_ptr;
    int active;
} animation_t;

#define MAX_ANIMATIONS 256

typedef struct {
    animation_t animations[MAX_ANIMATIONS];
    uint32_t animation_count;
    uint32_t next_anim_id;
} animation_context_t;

static animation_context_t g_anim_ctx = {};

void animation_init(void)
{
    memset(&g_anim_ctx, 0, sizeof(g_anim_ctx));
    g_anim_ctx.next_anim_id = 1;
}

/* ---- Easing Functions ---------------------------------------------------- */
static float easing_linear(float t)
{
    return t;
}

static float easing_ease_in(float t)
{
    /* Quadratic ease-in: t^2 */
    return t * t;
}

static float easing_ease_out(float t)
{
    /* Quadratic ease-out: 1 - (1-t)^2 */
    return 1.0f - (1.0f - t) * (1.0f - t);
}

static float easing_ease_in_out(float t)
{
    /* Quadratic ease-in-out */
    if (t < 0.5f)
        return 2.0f * t * t;
    else
        return -1.0f + (4.0f - 2.0f * t) * t;
}

static float easing_bounce(float t)
{
    /* Bounce: overshoot and settle */
    if (t < 0.5f) {
        return 2.0f * t * t;
    } else {
        return -1.0f + (4.0f - 2.0f * t) * t;
    }
}

static float easing_elastic(float t)
{
    /* Elastic: sine wave decay */
    const float PI = 3.14159265f;
    return (float)sin(t * PI * 4.0f) * (1.0f - t);
}

static float get_easing_value(easing_type_t easing, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    switch (easing) {
    case EASING_LINEAR:
        return easing_linear(t);
    case EASING_EASE_IN:
        return easing_ease_in(t);
    case EASING_EASE_OUT:
        return easing_ease_out(t);
    case EASING_EASE_IN_OUT:
        return easing_ease_in_out(t);
    case EASING_BOUNCE:
        return easing_bounce(t);
    case EASING_ELASTIC:
        return easing_elastic(t);
    default:
        return t;
    }
}

/* ---- Animation Control --------------------------------------------------- */
uint32_t animation_start(int *value_ptr, int start, int end,
                         uint32_t duration_ms, easing_type_t easing)
{
    if (!value_ptr || g_anim_ctx.animation_count >= MAX_ANIMATIONS)
        return 0;

    animation_t *anim = &g_anim_ctx.animations[g_anim_ctx.animation_count];
    anim->id = g_anim_ctx.next_anim_id++;
    anim->value_ptr = value_ptr;
    anim->start_value = start;
    anim->end_value = end;
    anim->start_time = g_kstate.uptime_ticks;
    anim->duration_ms = duration_ms;
    anim->easing = easing;
    anim->active = 1;

    *value_ptr = start;

    g_anim_ctx.animation_count++;
    return anim->id;
}

void animation_stop(uint32_t anim_id)
{
    for (uint32_t i = 0; i < g_anim_ctx.animation_count; i++) {
        if (g_anim_ctx.animations[i].id == anim_id) {
            g_anim_ctx.animations[i].active = 0;
            break;
        }
    }
}

void animation_tick(void)
{
    uint32_t now = g_kstate.uptime_ticks;
    uint32_t completed_count = 0;

    for (uint32_t i = 0; i < g_anim_ctx.animation_count; i++) {
        animation_t *anim = &g_anim_ctx.animations[i];

        if (!anim->active)
            continue;

        uint32_t elapsed = now - anim->start_time;
        float progress = (float)elapsed / (float)anim->duration_ms;

        if (progress >= 1.0f) {
            /* Animation complete */
            *anim->value_ptr = anim->end_value;
            anim->active = 0;
            completed_count++;
        } else {
            /* Interpolate current value */
            float eased = get_easing_value(anim->easing, progress);
            int current = anim->start_value +
                         (int)((anim->end_value - anim->start_value) * eased);
            *anim->value_ptr = current;
        }
    }

    /* Remove completed animations */
    if (completed_count > 0) {
        uint32_t write_idx = 0;
        for (uint32_t i = 0; i < g_anim_ctx.animation_count; i++) {
            if (g_anim_ctx.animations[i].active) {
                g_anim_ctx.animations[write_idx++] = g_anim_ctx.animations[i];
            }
        }
        g_anim_ctx.animation_count = write_idx;
    }
}

int animation_is_running(uint32_t anim_id)
{
    for (uint32_t i = 0; i < g_anim_ctx.animation_count; i++) {
        if (g_anim_ctx.animations[i].id == anim_id && g_anim_ctx.animations[i].active)
            return 1;
    }
    return 0;
}

int animation_any_running(void)
{
    return g_anim_ctx.animation_count > 0;
}

/* ---- Convenience Functions ----------------------------------------------- */
uint32_t animation_move_window(int *x, int *y, int target_x, int target_y,
                               uint32_t duration_ms)
{
    uint32_t anim_x = animation_start(x, *x, target_x, duration_ms, EASING_EASE_OUT);
    animation_start(y, *y, target_y, duration_ms, EASING_EASE_OUT);
    return anim_x;
}

uint32_t animation_resize_window(int *w, int *h, int target_w, int target_h,
                                 uint32_t duration_ms)
{
    uint32_t anim_w = animation_start(w, *w, target_w, duration_ms, EASING_EASE_OUT);
    animation_start(h, *h, target_h, duration_ms, EASING_EASE_OUT);
    return anim_w;
}

uint32_t animation_snap_to_edge(int *x, int *y, int *w, int *h,
                                int target_x, int target_y, int target_w, int target_h,
                                uint32_t duration_ms)
{
    /* Animate all 4 properties simultaneously */
    animation_start(x, *x, target_x, duration_ms, EASING_EASE_OUT);
    animation_start(y, *y, target_y, duration_ms, EASING_EASE_OUT);
    animation_start(w, *w, target_w, duration_ms, EASING_EASE_OUT);
    uint32_t anim_h = animation_start(h, *h, target_h, duration_ms, EASING_EASE_OUT);
    return anim_h;
}
