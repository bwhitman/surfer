/* Radio: N options, exactly one chosen, in a column or a row.
 *
 * The checkbox's sibling and deliberately not a variant of it: a
 * checkbox answers a yes/no about itself, a radio answers "which one" on
 * behalf of a group, and the difference is visible in the art — a ring
 * with a dot, not a box with a tick. Every platform draws that
 * distinction and users read it without being told, which is the whole
 * argument for a second widget rather than three checkboxes and a rule.
 *
 * BOTH AXES, because both are ordinary. A column is the settings-panel
 * shape (macOS's "Automatically / When scrolling / Always"); a row is
 * what you want when the choice is small and the space is a strip —
 * `( ) AMY out  (o) Audio in` on one line. The only thing that differs
 * is where the next option starts, so it is one widget with a flag
 * rather than two.
 *
 * ONE handler on the root, and the per-option extents are measured at
 * build time and kept — a row's options are as wide as their labels, so
 * "which one is under this x" is not arithmetic the way a tab strip's
 * is. Same reason as tabs otherwise: a group and a closure per option
 * would be three nodes each for a widget that is mostly text.
 */
#include <stdlib.h>

#include "surfer.h"

struct surf_radio {
    surf_node  *root;
    surf_node **strip;     /* per option: the two-frame indicator */
    surf_node **label;
    int16_t    *at;        /* per option: where it starts, along the axis */
    int16_t    *extent;    /* ...and how far it runs */
    int32_t     count, index;
    int16_t     w, h, fw, fh;
    bool        vertical;
    surf_index_cb cb;
    void         *user;
};

static void radio_apply(surf_radio *r)
{
    for (int32_t i = 0; i < r->count; i++)
        surf_filmstrip_set_frame(r->strip[i], i == r->index ? 1 : 0);
}

static void radio_set(surf_radio *r, int32_t idx, bool report)
{
    if (idx < 0 || idx >= r->count || idx == r->index)
        return;
    r->index = idx;
    radio_apply(r);
    if (report && r->cb)
        r->cb(idx, r->user);
}

static int32_t radio_at(surf_radio *r, int16_t x, int16_t y)
{
    int16_t ax, ay;
    surf_node_abs_pos(r->root, &ax, &ay);
    if (x < ax || y < ay || x >= ax + r->w || y >= ay + r->h)
        return -1;
    int16_t p = r->vertical ? (int16_t)(y - ay) : (int16_t)(x - ax);
    for (int32_t i = 0; i < r->count; i++)
        if (p >= r->at[i] && p < r->at[i] + r->extent[i])
            return i;
    return -1;
}

static void radio_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surf_radio *r = user;
    /* on RELEASE, like the checkbox and the button: a press that slides
     * off is a mind changed, not a choice made */
    if (t->phase != SURF_TOUCH_UP)
        return;
    int32_t hit = radio_at(r, t->x, t->y);
    if (hit >= 0)
        radio_set(r, hit, true);
}

surf_radio *surf_radio_new(surf_node *parent, int16_t x, int16_t y,
                           const surf_radio_style *style,
                           const char *const *labels, int32_t count,
                           bool vertical)
{
    if (!parent || !style || !style->strip || !style->font || count < 1)
        return NULL;
    surf_radio *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->count = count;
    r->vertical = vertical;
    r->fw = style->frame_w;
    r->fh = style->frame_h;
    r->strip = calloc((size_t)count, sizeof *r->strip);
    r->label = calloc((size_t)count, sizeof *r->label);
    r->at = calloc((size_t)count, sizeof *r->at);
    r->extent = calloc((size_t)count, sizeof *r->extent);
    r->root = surf_group_new(x, y);
    if (!r->strip || !r->label || !r->at || !r->extent || !r->root) {
        surf_radio_destroy(r);
        return NULL;
    }
    int16_t gap = style->gap > 0 ? style->gap : 8;
    int16_t pen = 0, wide = 0;
    for (int32_t i = 0; i < count; i++) {
        const char *lab = labels && labels[i] ? labels[i] : "";
        r->strip[i] = surf_filmstrip_new(style->strip, style->frame_w,
                                         style->frame_h, 0, 0);
        r->label[i] = surf_text_new(style->font, lab, 0, 0,
                                    style->text_color);
        if (!r->strip[i] || !r->label[i]) {
            surf_radio_destroy(r);
            return NULL;
        }
        surf_point ls = surf_node_size(r->label[i]);
        int16_t row_h = style->frame_h > ls.y ? style->frame_h : (int16_t)ls.y;
        /* the label sits beside the ring on BOTH axes: a radio reads
         * left to right whichever way the group runs */
        int16_t ox = vertical ? 0 : pen;
        int16_t oy = vertical ? pen : 0;
        surf_node_set_pos(r->strip[i], ox,
                          (int16_t)(oy + (row_h - style->frame_h) / 2));
        surf_node_set_pos(r->label[i], (int16_t)(ox + style->frame_w + 6),
                          (int16_t)(oy + (row_h - ls.y) / 2));
        surf_node_add(r->root, r->strip[i]);
        surf_node_add(r->root, r->label[i]);
        int16_t run = (int16_t)(style->frame_w + 6 + ls.x);
        r->at[i] = pen;
        r->extent[i] = vertical ? row_h : (int16_t)(run + gap);
        if (run > wide)
            wide = run;
        pen = (int16_t)(pen + r->extent[i] + (vertical ? gap : 0));
    }
    if (vertical) {
        r->w = wide;
        r->h = (int16_t)(pen - gap);
    } else {
        r->w = (int16_t)(pen - gap);
        r->h = style->frame_h;
        for (int32_t i = 0; i < count; i++) {
            surf_point ls = surf_node_size(r->label[i]);
            if (ls.y > r->h)
                r->h = (int16_t)ls.y;
        }
    }
    /* a group is hittable only once it has a size, and the size is what
     * was just measured rather than anything the caller had to guess */
    surf_group_set_clip(r->root, r->w, r->h);
    radio_apply(r);
    surf_node_set_on_touch(r->root, radio_touch, r);
    surf_node_add(parent, r->root);
    return r;
}

void surf_radio_destroy(surf_radio *r)
{
    if (!r)
        return;
    surf_node_destroy(r->root);
    free(r->strip);
    free(r->label);
    free(r->at);
    free(r->extent);
    free(r);
}

surf_node *surf_radio_node(surf_radio *r) { return r ? r->root : NULL; }
int32_t surf_radio_index(const surf_radio *r) { return r ? r->index : 0; }
int32_t surf_radio_count(const surf_radio *r) { return r ? r->count : 0; }

/* what it measured itself to be — a row's width is its labels', so a
 * caller laying out around one should ask rather than assume */
surf_point surf_radio_size(const surf_radio *r)
{
    surf_point p = {0, 0};
    if (r) {
        p.x = r->w;
        p.y = r->h;
    }
    return p;
}

/* the caller moving it: no callback, the rule every widget here keeps */
void surf_radio_set_index(surf_radio *r, int32_t idx)
{
    if (r)
        radio_set(r, idx, false);
}

void surf_radio_on_change(surf_radio *r, surf_index_cb cb, void *user)
{
    if (!r)
        return;
    r->cb = cb;
    r->user = user;
}
