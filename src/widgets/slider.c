/* Vertical slider: 9-patch track + cap sprite. The cap centers on the
 * finger and the whole widget captures the pointer, so drags never lose
 * the finger (DESIGN.md §2.6). */
#include <stdlib.h>

#include "surfer.h"
#include "widget_touch.h"

/* The house grey for a cap with no colour of its own. */
#define CAP_DEFAULT_COLOR SURF_RGB(178, 182, 194)

struct surf_slider {
    surf_image     cap_img;  /* our own tint over the style's pixels */
    surf_node     *root, *track, *cap;
    int16_t        w, h, cap_w, cap_h;
    bool           horiz;          /* wider than tall: value runs across */
    int32_t        value;  /* Q16 */
    surf_change_cb cb;
    void          *user;
    uint8_t        busy;   /* the contact driving it; 0 = idle */
};

static int32_t clamp_q16(int32_t v)
{
    if (v < 0) return 0;
    if (v > SURF_ONE) return SURF_ONE;
    return v;
}

static void slider_apply(surf_slider *s)
{
    if (s->horiz) {
        int range = s->w - s->cap_w;
        int cap_x = (int)(((int64_t)s->value * range) >> 16);
        surf_node_set_pos(s->cap, (int16_t)cap_x,
                          (int16_t)((s->h - s->cap_h) / 2));
        return;
    }
    int range = s->h - s->cap_h;
    int cap_y = range - (int)(((int64_t)s->value * range) >> 16);
    surf_node_set_pos(s->cap, (int16_t)((s->w - s->cap_w) / 2), (int16_t)cap_y);
}

static void slider_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surf_slider *s = user;
    if (!surf_widget_claim(&s->busy, t))
        return;                    /* a second finger on the same fader */
    if (t->phase == SURF_TOUCH_UP)
        return;

    int16_t ax, ay;
    surf_node_abs_pos(s->root, &ax, &ay);
    int range, at;
    if (s->horiz) {
        /* left to right, which is the direction the value grows — the
         * vertical one runs the other way, because up is more */
        range = s->w - s->cap_w;
        at = t->x - ax - s->cap_w / 2;
    } else {
        range = s->h - s->cap_h;
        at = t->y - ay - s->cap_h / 2;
    }
    if (at < 0) at = 0;
    if (at > range) at = range;
    if (range <= 0)
        return;

    int32_t v = s->horiz ? (int32_t)(((int64_t)at << 16) / range)
                         : (int32_t)(((int64_t)(range - at) << 16) / range);
    if (v == s->value)
        return;
    s->value = v;
    slider_apply(s);
    if (s->cb)
        s->cb(v, s->user);
}

surf_slider *surf_slider_new(surf_node *parent, int16_t x, int16_t y,
                             int16_t w, int16_t h, const surf_slider_style *style)
{
    if (!parent || !style || !style->track || !style->cap)
        return NULL;
    /* Orientation is the SHAPE, not a flag: a caller asking for 200x40
     * means a horizontal slider and should not have to say so twice. */
    bool horiz = w > h;
    const surf_image *track = horiz && style->track_h ? style->track_h
                                                      : style->track;
    const surf_image *cap = horiz && style->cap_h ? style->cap_h : style->cap;
    if (horiz ? (w <= cap->w || h < cap->h) : (h <= cap->h || w < cap->w))
        return NULL;

    surf_slider *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->w = w;
    s->h = h;
    s->horiz = horiz;
    s->cap_w = cap->w;
    s->cap_h = cap->h;

    s->root = surf_group_new(x, y);
    /* Exact-size track art is one blit; the tiled 9-patch is the fallback
     * for sizes the theme didn't bake. On the P4 the per-op cost of tiling
     * dwarfs the pixels (M2 bench), so themes should ship exact sizes. */
    if (track->w == w && track->h == h) {
        s->track = surf_sprite_new(track, 0, 0);
    } else if (horiz) {
        /* Stretch ALONG the groove, never across it. The groove is one
         * line down the middle of the art; tiling the 9-patch's middle
         * band vertically repeats it, and a slider with two parallel
         * grooves is what that looks like. So the track keeps the art's
         * own height, centred, and only the horizontal middle tiles. */
        int16_t th = track->h < h ? track->h : h;
        s->track = surf_ninepatch_new(track, 0, (int16_t)((h - th) / 2),
                                      w, th, style->inset, 0, style->inset, 0);
    } else {
        /* The same rule, upright — and it is the same rule, which is why
         * the vertical case no longer slices all four edges. A track
         * NARROWER than the widget is the compact slider's whole shape
         * (a thin bar with a wider handle riding across it), and tiling
         * the 9-patch's middle band sideways to fill the width gives the
         * two parallel grooves described above, standing up. */
        int16_t tw = track->w < w ? track->w : w;
        s->track = surf_ninepatch_new(track, (int16_t)((w - tw) / 2), 0,
                                      tw, h, 0, style->inset, 0, style->inset);
    }
    /* The CAP is the A8 part, so it takes a colour for nothing: a struct
     * copy shares the pixels and owns the tint (knob.c has the full
     * note). The track keeps its own art. */
    s->cap_img = *cap;
    s->cap_img.tint = style->color ? style->color : CAP_DEFAULT_COLOR;
    s->cap = surf_sprite_new(&s->cap_img, 0, 0);
    if (!s->root || !s->track || !s->cap) {
        surf_node_destroy(s->root);
        surf_node_destroy(s->track);
        surf_node_destroy(s->cap);
        free(s);
        return NULL;
    }
    surf_node_add(s->root, s->track);
    surf_node_add(s->root, s->cap);
    /* The DECLARED box is the grab area, not the union of what happens to
     * be drawn: a group is hittable only with a clip (see colorpicker),
     * and the compact slider's bar is a third of its width. Without this
     * the gutter either side of a thin track is a hole the finger falls
     * through — and a tap on the track is how you jump the value. */
    surf_group_set_clip(s->root, w, h);
    surf_node_set_on_touch(s->root, slider_touch, s);
    surf_node_set_gesture_grab(s->root, true);  /* a slider drag is never a scroll */
    slider_apply(s);
    surf_node_add(parent, s->root);
    return s;
}

void surf_slider_destroy(surf_slider *s)
{
    if (!s)
        return;
    surf_node_destroy(s->root);
    free(s);
}

surf_node *surf_slider_node(surf_slider *s) { return s ? s->root : NULL; }

void surf_slider_set_value(surf_slider *s, int32_t value_q16)
{
    if (!s)
        return;
    s->value = clamp_q16(value_q16);
    slider_apply(s);
}

int32_t surf_slider_value(const surf_slider *s) { return s ? s->value : 0; }

void surf_slider_on_change(surf_slider *s, surf_change_cb cb, void *user)
{
    if (!s)
        return;
    s->cb = cb;
    s->user = user;
}

void surf_slider_set_color(surf_slider *s, surf_color c)
{
    if (!s || s->cap_img.tint == c)
        return;
    s->cap_img.tint = c;
    surf_node_damage(s->cap);     /* a retint is a repaint, not a reblit */
}

surf_color surf_slider_color(const surf_slider *s)
{
    return s ? s->cap_img.tint : 0;
}
