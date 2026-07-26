/* LED: an indicator lamp. One filmstrip node, no input at all — it is the
 * only widget that reports nothing, because a lamp is an output.
 *
 * The art is A8 (alpha only), so ONE asset serves every color: each LED
 * owns a COPY of the surf_image struct — same shared pixels, its own
 * `tint` — which on the P4 is a single palette register the PPA applies
 * at blend time. That is why set_color is free: no pixels are touched,
 * and the node just needs a repaint.
 *
 * Brightness is a level rather than a bool so a blink can fade. Frame 0
 * is the UNLIT lens (a dark bead, not a hole), so an off LED still looks
 * like hardware.
 */
#include <stdlib.h>

#include "surfer.h"

struct surf_led {
    surf_node  *strip;
    surf_image  img;      /* our own tint over the style's shared pixels */
    int16_t     frames;
    int32_t     level;    /* Q16 */
};

static void led_apply(surf_led *l)
{
    int32_t f = (int32_t)(((int64_t)l->level * (l->frames - 1) + SURF_ONE / 2) >> 16);
    surf_filmstrip_set_frame(l->strip, (int16_t)f);
}

surf_led *surf_led_new(surf_node *parent, int16_t x, int16_t y,
                       const surf_led_style *style)
{
    if (!parent || !style || !style->strip || style->frames < 2)
        return NULL;
    surf_led *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->img = *style->strip;            /* struct copy: pixels are shared */
    l->img.tint = style->color;
    l->frames = style->frames;
    l->strip = surf_filmstrip_new(&l->img, style->frame_w, style->frame_h, x, y);
    if (!l->strip) {
        free(l);
        return NULL;
    }
    surf_node_add(parent, l->strip);
    return l;
}

void surf_led_destroy(surf_led *l)
{
    if (!l)
        return;
    surf_node_destroy(l->strip);       /* the node holds &l->img: order matters */
    free(l);
}

surf_node *surf_led_node(surf_led *l) { return l ? l->strip : NULL; }

void surf_led_set(surf_led *l, bool on)
{
    surf_led_set_level(l, on ? SURF_ONE : 0);
}

void surf_led_set_level(surf_led *l, int32_t level_q16)
{
    if (!l)
        return;
    if (level_q16 < 0) level_q16 = 0;
    if (level_q16 > SURF_ONE) level_q16 = SURF_ONE;
    if (level_q16 == l->level)
        return;
    l->level = level_q16;
    led_apply(l);
}

int32_t surf_led_level(const surf_led *l) { return l ? l->level : 0; }

void surf_led_set_color(surf_led *l, surf_color c)
{
    if (!l || l->img.tint == c)
        return;
    l->img.tint = c;
    surf_node_damage(l->strip);        /* retint is a repaint, not a reblit */
}
