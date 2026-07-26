/* Colour picker: a saturation/value square beside a hue strip.
 *
 * The one widget whose art cannot be baked, because the square's colours
 * depend on the hue you are standing on. So it is DRAWN — into two
 * runtime images, per pixel, in C — and the rule that matters is that
 * this happens on an EVENT and never in the frame path: the strip once at
 * creation, the square again only when the hue actually changes. A
 * 96-pixel square is ~9k writes, which is nothing next to a repaint you
 * would otherwise do every frame. From then on it is two ordinary opaque
 * sprites and the compositor treats them like any other picture.
 *
 * HSV, not RGB sliders, because picking a colour by eye means moving
 * along one axis at a time — and because three sliders is a thing any
 * caller can already build without a widget.
 */
#include <stdlib.h>

#include <stdlib.h>

#include "surfer.h"

struct surf_colorpicker {
    surf_node    *root, *sq, *strip, *mark, *mark_in, *hmark;
    surf_image   *sq_img, *strip_img;
    int16_t       size, sw;         /* square side, strip width */
    int32_t       h, s, v;          /* Q16: hue 0..1, sat 0..1, val 0..1 */
    surf_index_cb cb;               /* reports a packed surf_color */
    void         *user;
};

#define MARK 9                      /* the little square that marks the pick */

/* h,s,v in 0..1 Q16 -> rgb 0..255. The usual sextant walk, in fixed
 * point so this file stays float-free like the rest of core. */
static void hsv_rgb(int32_t h, int32_t s, int32_t v, int *r, int *g, int *b)
{
    if (h >= SURF_ONE) h = SURF_ONE - 1;
    if (h < 0) h = 0;
    int32_t sect = (h * 6) >> 16;               /* 0..5 */
    int32_t f = (h * 6) - (sect << 16);         /* fraction within it */
    /* 64-bit intermediates: at full value AND full saturation these are
     * 65536 * 65536, which overflows an int32 to zero — and that corner
     * is the most-used one on the whole widget. It came out pure red
     * instead of white. */
    int32_t p = (int32_t)(((int64_t)v * (SURF_ONE - s)) >> 16);
    int32_t q = (int32_t)(((int64_t)v * (SURF_ONE - (((int64_t)s * f) >> 16))) >> 16);
    int32_t t = (int32_t)(((int64_t)v *
                           (SURF_ONE - (((int64_t)s * (SURF_ONE - f)) >> 16))) >> 16);
    int32_t rr, gg, bb;
    switch (sect) {
    case 0:  rr = v; gg = t; bb = p; break;
    case 1:  rr = q; gg = v; bb = p; break;
    case 2:  rr = p; gg = v; bb = t; break;
    case 3:  rr = p; gg = q; bb = v; break;
    case 4:  rr = t; gg = p; bb = v; break;
    default: rr = v; gg = p; bb = q; break;
    }
    *r = (int)(((int64_t)rr * 255) >> 16);
    *g = (int)(((int64_t)gg * 255) >> 16);
    *b = (int)(((int64_t)bb * 255) >> 16);
}

static surf_color pick_color(const surf_colorpicker *c)
{
    int r, g, b;
    hsv_rgb(c->h, c->s, c->v, &r, &g, &b);
    return SURF_RGB(r, g, b);
}

/* the square for the current hue: saturation across, value down */
static void paint_square(surf_colorpicker *c)
{
    if (!c->sq_img)
        return;
    uint16_t *px = c->sq_img->pixels;
    int32_t stride = c->sq_img->stride / 2;
    for (int y = 0; y < c->size; y++) {
        int32_t v = SURF_ONE - (int32_t)y * SURF_ONE / (c->size - 1);
        for (int x = 0; x < c->size; x++) {
            int32_t s = (int32_t)x * SURF_ONE / (c->size - 1);
            int r, g, b;
            hsv_rgb(c->h, s, v, &r, &g, &b);
            px[y * stride + x] = SURF_RGB(r, g, b);
        }
    }
    surf_node_damage(c->sq);
}

static void paint_strip(surf_colorpicker *c)
{
    if (!c->strip_img)
        return;
    uint16_t *px = c->strip_img->pixels;
    int32_t stride = c->strip_img->stride / 2;
    for (int y = 0; y < c->size; y++) {
        int32_t h = (int32_t)y * SURF_ONE / (c->size - 1);
        int r, g, b;
        hsv_rgb(h, SURF_ONE, SURF_ONE, &r, &g, &b);
        for (int x = 0; x < c->sw; x++)
            px[y * stride + x] = SURF_RGB(r, g, b);
    }
    surf_node_damage(c->strip);
}

static void place_marks(surf_colorpicker *c)
{
    int16_t mx = (int16_t)((int64_t)c->s * (c->size - 1) >> 16);
    int16_t my = (int16_t)(c->size - 1 - ((int64_t)c->v * (c->size - 1) >> 16));
    surf_node_set_pos(c->mark, (int16_t)(mx - MARK / 2), (int16_t)(my - MARK / 2));
    surf_node_set_pos(c->mark_in, (int16_t)(mx - MARK / 2 + 2),
                      (int16_t)(my - MARK / 2 + 2));
    surf_rect_set_color(c->mark_in, pick_color(c));
    int16_t hy = (int16_t)((int64_t)c->h * (c->size - 1) >> 16);
    surf_node_set_pos(c->hmark, (int16_t)(c->size + 6), (int16_t)(hy - 1));
}

static void report(surf_colorpicker *c)
{
    place_marks(c);
    if (c->cb)
        c->cb((int32_t)pick_color(c), c->user);
}

static int32_t clamp01(int32_t v)
{
    return v < 0 ? 0 : (v > SURF_ONE ? SURF_ONE : v);
}

static void cp_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surf_colorpicker *c = user;
    if (t->phase == SURF_TOUCH_UP)
        return;
    int16_t ax, ay;
    surf_node_abs_pos(c->root, &ax, &ay);
    int16_t x = (int16_t)(t->x - ax), y = (int16_t)(t->y - ay);
    int32_t span = c->size - 1;
    if (x < c->size + 3) {                      /* the square */
        c->s = clamp01((int32_t)x * SURF_ONE / span);
        c->v = clamp01(SURF_ONE - (int32_t)y * SURF_ONE / span);
    } else {                                    /* the hue strip */
        int32_t h = clamp01((int32_t)y * SURF_ONE / span);
        if (h == c->h)
            return;
        c->h = h;
        paint_square(c);                        /* the square follows the hue */
    }
    report(c);
}

surf_colorpicker *surf_colorpicker_new(surf_node *parent, int16_t x, int16_t y,
                                       int16_t size)
{
    if (!parent || size < 16)
        return NULL;
    surf_colorpicker *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->size = size;
    c->sw = (int16_t)(size / 6 < 8 ? 8 : size / 6);
    c->h = 0;
    c->s = c->v = SURF_ONE;

    c->root = surf_group_new(x, y);
    c->sq_img = surf_image_new(size, size, SURF_FMT_RGB565);
    c->strip_img = surf_image_new(c->sw, size, SURF_FMT_RGB565);
    if (!c->root || !c->sq_img || !c->strip_img)
        goto fail;
    c->sq = surf_sprite_new(c->sq_img, 0, 0);
    c->strip = surf_sprite_new(c->strip_img, (int16_t)(size + 6), 0);
    /* the pick marker is two rects: a light box and the colour inside it,
     * so it stays visible over both a white corner and a black one */
    c->mark = surf_rect_new(0, 0, MARK, MARK, SURF_RGB(250, 250, 250));
    c->mark_in = surf_rect_new(0, 0, MARK - 4, MARK - 4, SURF_RGB(0, 0, 0));
    c->hmark = surf_rect_new((int16_t)(size + 6), 0, c->sw, 3,
                             SURF_RGB(250, 250, 250));
    if (!c->sq || !c->strip || !c->mark || !c->mark_in || !c->hmark)
        goto fail;
    surf_node_add(c->root, c->sq);
    surf_node_add(c->root, c->strip);
    surf_node_add(c->root, c->mark);
    surf_node_add(c->root, c->mark_in);
    surf_node_add(c->root, c->hmark);
    /* set_clip is how a group gets a SIZE, and a group needs one to be
     * hittable — otherwise the 6px gutter between square and strip is a
     * hole. The children all sit inside it, so the clipping costs
     * nothing. */
    surf_group_set_clip(c->root, (int16_t)(size + 6 + c->sw), size);
    surf_node_set_on_touch(c->root, cp_touch, c);
    surf_node_set_gesture_grab(c->root, true);
    surf_node_add(parent, c->root);
    paint_strip(c);
    paint_square(c);
    place_marks(c);
    return c;
fail:
    surf_node_destroy(c->root);
    surf_image_destroy(c->sq_img);
    surf_image_destroy(c->strip_img);
    free(c);
    return NULL;
}

void surf_colorpicker_destroy(surf_colorpicker *c)
{
    if (!c)
        return;
    surf_node_destroy(c->root);      /* nodes first: they hold the images */
    surf_image_destroy(c->sq_img);
    surf_image_destroy(c->strip_img);
    free(c);
}

surf_node *surf_colorpicker_node(surf_colorpicker *c) { return c ? c->root : NULL; }

surf_color surf_colorpicker_color(const surf_colorpicker *c)
{
    return c ? pick_color(c) : 0;
}

/* Set from an RGB565 colour: the picker keeps HSV, so this converts back.
 * Round-tripping is not exact — 565 has 5 bits of red — but the marker
 * lands where the colour is, which is what it is for. */
void surf_colorpicker_set_color(surf_colorpicker *c, surf_color col)
{
    if (!c)
        return;
    int32_t r = ((col >> 11) & 0x1F) * 255 / 31;
    int32_t g = ((col >> 5) & 0x3F) * 255 / 63;
    int32_t b = (col & 0x1F) * 255 / 31;
    int32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int32_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int32_t d = mx - mn;
    int32_t h = 0;
    if (d) {
        if (mx == r)      h = ((g - b) * SURF_ONE / d / 6 + SURF_ONE) % SURF_ONE;
        else if (mx == g) h = ((b - r) * SURF_ONE / d + 2 * SURF_ONE) / 6;
        else              h = ((r - g) * SURF_ONE / d + 4 * SURF_ONE) / 6;
    }
    c->h = clamp01(h);
    c->s = mx ? clamp01(d * SURF_ONE / mx) : 0;
    c->v = clamp01(mx * SURF_ONE / 255);
    paint_square(c);
    place_marks(c);
}

void surf_colorpicker_on_change(surf_colorpicker *c, surf_index_cb cb, void *user)
{
    if (!c)
        return;
    c->cb = cb;
    c->user = user;
}
