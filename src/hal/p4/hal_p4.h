/* ESP32-P4 backend: PPA fill/SRM/blend engines into an RGB565 compose
 * buffer in PSRAM; present copies dirty rects to the DSI scanout fb. */
#ifndef SURF_HAL_P4_H
#define SURF_HAL_P4_H

#include "surfer.h"
#include "esp_lcd_types.h"

/* Board glue polls its own touch controller; the hal turns level+position
 * into DOWN/MOVE/UP events. Return true while pressed. */
typedef bool (*surf_p4_touch_poll_fn)(int16_t *x, int16_t *y);
/* multitouch flavor: fill up to max contacts, return the count. When set
 * it replaces touch_poll — the hal derives the single-pointer stream
 * from contact 0 and serves the full set via surf_touch_points(). */
typedef int (*surf_p4_touch_poll_multi_fn)(surf_touch_pt *out, int max);

typedef struct {
    esp_lcd_panel_handle_t panel;        /* NULL → headless (bench only) */
    void                  *scan_fbs[3];  /* DSI framebuffers, RGB565 */
    int16_t                w, h;
    surf_p4_touch_poll_fn  touch_poll;   /* optional */
    surf_p4_touch_poll_multi_fn touch_poll_multi;  /* optional, wins */
    /* With all three scan_fbs set: triple-buffer-with-damage — compose into
     * a buffer that is neither scanned nor pending, zero-copy flip on
     * present (never stalls), DMA2D the last two frames' damage forward.
     * Flicker-free at full compose rate. single_buffer forces direct
     * composition into scan_fbs[0]: present is free but mid-paint states
     * are visible on large updates (kept for A/B measurement). */
    bool                   single_buffer;
    /* QUARTER TURNS CCW BETWEEN WHAT IS COMPOSED AND WHAT IS SCANNED OUT,
     * for a panel mounted the other way up from the picture — 0, 90, 180,
     * 270, anything else refused.
     *
     * `w` and `h` above stay the LOGICAL size, the one surfer draws in
     * and the one every coordinate anywhere is expressed in; at 90 or 270
     * the panel's own raster is h x w. A board glue that rotates owes its
     * touch controller the same transform, because nothing here sees the
     * finger.
     *
     * It is not free and it is not proportional to the screen: composition
     * moves into a buffer of its own and present ROTATES THE DIRTY RECTS
     * into the scanout, so a still screen costs nothing and a full repaint
     * costs one whole-screen SRM pass. Two things are given up while it is
     * on, both by being switched off rather than by going wrong: streaming
     * band shifts (layers fall back to repainting, as they do in
     * single-buffer mode) and the scroll_rect cross-buffer path, since
     * both read the last SCANNED frame and that frame is no longer in the
     * orientation anyone composed. */
    int16_t                rotation;
} surf_hal_p4_cfg;

const surf_hal *surf_hal_p4_init(const surf_hal_p4_cfg *cfg);

/* Cache writeback after CPU writes to an image (the LVGL-PPA class of bug,
 * owned here once — see DESIGN.md §2.1). Call after filling any buffer the
 * PPA will read. */
void surf_hal_p4_sync(const void *buf, size_t bytes);

/* Invalidate the CPU cache over the whole compose framebuffer before
 * reading it back (screenshots): PPA/DMA2D wrote it behind the cache. */
void surf_hal_p4_fb_invalidate(void);

/* DMA2D copies that gave up waiting for completion. Zero on a healthy
 * machine; non-zero means the compositor took the timeout path that
 * stops a lost completion becoming a frozen screen. */
extern uint32_t surf_hal_p4_fbcpy_timeouts;

/* The same accounting for the PPA. A wedged engine never gives its
 * completion back, so before these existed the compositor simply stopped
 * inside C with the panel still lit. Zero on a healthy machine.
 *   ppa_timeouts  ops that never completed — the engine is gone
 *   ppa_errors    ops the driver rejected before the hardware saw them
 *   ppa_skipped   ops not attempted, because ppa_dead was already set
 *   ppa_dead      latched by the first timeout, one-way (no reset API) */
extern uint32_t surf_hal_p4_ppa_timeouts;
extern uint32_t surf_hal_p4_ppa_errors;
extern uint32_t surf_hal_p4_ppa_skipped;
extern bool     surf_hal_p4_ppa_dead;

#endif /* SURF_HAL_P4_H */
