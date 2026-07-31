/* Selector: a knob with detents. It chooses among N fixed options and
 * reports an INDEX, not a fraction — a mode switch, a bank, a waveform.
 *
 * It shares the knob's filmstrip shape (one strip of pointer angles) and
 * simply lands on the frame nearest a detent, so N is a runtime number
 * and needs no art of its own.
 *
 * Two gestures, because a panel control wants both:
 *  - DRAG, vertically, like the knob: coarse, and it snaps as it goes so
 *    you always see a legal position.
 *  - TAP, which advances one position and wraps. On a touchscreen that is
 *    how you nudge a 4-position switch without aiming.
 * A tap is a press that travelled less than TAP_SLOP, decided at UP.
 */
#include <stdlib.h>

#include "surfer.h"
#include "widget_touch.h"

#define SEL_DRAG_RANGE 160   /* px of drag for the full sweep */
#define TAP_SLOP       6     /* px; more travel than this is a drag */

struct surf_selector {
    surf_image     img;   /* our own tint over shared pixels; see knob.c */
    surf_node    *root, *strip;
    int16_t       fw, fh, frames;
    int32_t       positions, index;
    int32_t       drag_start_index;
    int16_t       down_y, moved;
    surf_index_cb cb;
    void         *user;
    uint8_t       busy;   /* the contact driving it; 0 = idle */
};

static void sel_apply(surf_selector *s)
{
    /* the pointer spans the whole sweep across the detents; with one
     * position it points straight up rather than hard left */
    int32_t f = s->positions > 1
                    ? s->index * (s->frames - 1) / (s->positions - 1)
                    : (s->frames - 1) / 2;
    surf_filmstrip_set_frame(s->strip, (int16_t)f);
}

static void sel_set(surf_selector *s, int32_t idx, bool report)
{
    if (idx < 0) idx = 0;
    if (idx > s->positions - 1) idx = s->positions - 1;
    if (idx == s->index)
        return;
    s->index = idx;
    sel_apply(s);
    if (report && s->cb)
        s->cb(idx, s->user);
}

static void sel_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surf_selector *s = user;
    if (!surf_widget_claim(&s->busy, t))
        return;
    switch (t->phase) {
    case SURF_TOUCH_DOWN:
        s->down_y = t->y;
        s->drag_start_index = s->index;
        s->moved = 0;
        break;
    case SURF_TOUCH_MOVE: {
        int16_t d = (int16_t)(s->down_y - t->y);
        if (d > s->moved) s->moved = d;
        if (-d > s->moved) s->moved = (int16_t)-d;
        if (s->moved <= TAP_SLOP)
            break;
        /* +half a step so the detent you are nearest is the one you get */
        int32_t span = s->positions - 1;
        int32_t step = (int32_t)d * span * 2 / SEL_DRAG_RANGE;
        sel_set(s, s->drag_start_index + (step + (step > 0 ? 1 : -1)) / 2, true);
        break;
    }
    case SURF_TOUCH_UP:
        if (s->moved <= TAP_SLOP)
            sel_set(s, (s->index + 1) % s->positions, true);
        break;
    }
}

surf_selector *surf_selector_new(surf_node *parent, int16_t x, int16_t y,
                                 const surf_knob_style *style,
                                 int32_t positions)
{
    if (!parent || !style || !style->strip || style->frames < 2 || positions < 1)
        return NULL;
    surf_selector *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->fw = style->frame_w;
    s->fh = style->frame_h;
    s->frames = style->frames;
    s->positions = positions;
    s->root = surf_group_new(x, y);
    s->img = *style->strip;
    s->img.tint = style->color ? style->color : SURF_RGB(196, 198, 206);
    s->strip = surf_filmstrip_new(&s->img, style->frame_w, style->frame_h,
                                  0, 0);
    if (!s->root || !s->strip) {
        surf_node_destroy(s->root);
        surf_node_destroy(s->strip);
        free(s);
        return NULL;
    }
    surf_node_add(s->root, s->strip);
    sel_apply(s);
    surf_node_set_on_touch(s->root, sel_touch, s);
    surf_node_set_gesture_grab(s->root, true);  /* never a scroll */
    surf_node_add(parent, s->root);
    return s;
}

void surf_selector_destroy(surf_selector *s)
{
    if (!s)
        return;
    surf_node_destroy(s->root);
    free(s);
}

surf_node *surf_selector_node(surf_selector *s) { return s ? s->root : NULL; }

int32_t surf_selector_index(const surf_selector *s) { return s ? s->index : 0; }

int32_t surf_selector_positions(const surf_selector *s)
{
    return s ? s->positions : 0;
}

/* the caller telling the knob where things are: no callback, same rule as
 * every other widget's set_value */
void surf_selector_set_index(surf_selector *s, int32_t idx)
{
    if (s)
        sel_set(s, idx, false);
}

void surf_selector_on_change(surf_selector *s, surf_index_cb cb, void *user)
{
    if (!s)
        return;
    s->cb = cb;
    s->user = user;
}

void surf_selector_set_color(surf_selector *s, surf_color c)
{
    if (!s || s->img.tint == c)
        return;
    s->img.tint = c;
    surf_node_damage(s->strip);
}
