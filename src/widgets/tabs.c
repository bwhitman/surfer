/* Tabs: a strip of labelled buttons, one PAGE behind each, and the
 * widget owns the showing and the hiding.
 *
 * WHY THIS IS A WIDGET AND NOT FOUR LINES OF CALLER CODE. Drawing a row
 * of buttons is easy; what is not is the part underneath — every node of
 * page 2 hidden while page 1 is up, and the swap happening in ONE place
 * when the index changes. A caller doing that by hand keeps a list of
 * groups, remembers which is showing (surfer's `hidden` is write-only,
 * so it cannot be read back off the tree), and gets it wrong the first
 * time a page is added after the fact. Here the page IS the widget's:
 * `surf_tabs_page(t, i)` hands you a group to fill and nothing else ever
 * has to know it exists.
 *
 * THE ART IS A TAB, not a button, and that is not decoration. A tab is a
 * card whose bottom edge IS the page it belongs to: rounded at the top,
 * dead flat at the foot, drawn in the page's own background colour so
 * the join disappears. The first version reused the button's 9-patch —
 * rounded all round, in the button's baked colours — and came back from
 * the bench as "more like buttons than tabs", which was exactly right:
 * nothing about it said the page below was the same object.
 *
 * So the asset is A8 and the COLOURS ARE THE CALLER'S: `style->face` is
 * meant to be the page background (the current tab vanishes into it),
 * `style->dim` is every other tab. One image, two tinted copies — the
 * knob's trick, for the knob's reason: a tint is a palette register on
 * the P4, so two colours cost no second asset and no pixels.
 *
 * The label is TWO nodes, bright and dim, one hidden — because a label's
 * colour is baked when the node is made (`set_color` is a silent no-op
 * on text, which rally documents at length) and the current tab has to
 * read louder than the rest. It also lets a caller hand in a bold face
 * for the active one and nothing else.
 *
 * ONE handler on the strip rather than one per tab. The index is
 * arithmetic on the x that came in (dropdown does the same with its
 * rows), which saves a group and a closure per tab — a five-tab bar
 * costs five nodes for its faces and not fifteen.
 */
#include <stdlib.h>
#include <string.h>

#include "surfer.h"

struct surf_tabs {
    surf_node  *root;      /* the whole thing: strip + pages */
    surf_node  *strip;     /* the tabs */
    surf_image  on, off;   /* our own tints over shared pixels (knob.c) */
    surf_node **up;        /* per tab: the dim face and the bright one */
    surf_node **down;
    surf_node **label;     /* ...and the two legends that go with them */
    surf_node **label_on;
    surf_node **page;      /* per tab: what the caller fills */
    int32_t     count, index;
    int16_t     w, h, tab_h, tab_w;
    int16_t     pressed;   /* the tab a finger is on, or -1 */
    surf_index_cb cb;
    void         *user;
};

/* Which face each tab wears: the current one is pressed, and so is
 * whichever one a finger is currently down on — the press has to be
 * visible or a tap on a touchscreen has no feedback at all. */
static void tabs_faces(surf_tabs *t)
{
    for (int32_t i = 0; i < t->count; i++) {
        bool on = (i == t->index) || (i == t->pressed);
        surf_node_set_hidden(t->up[i], on);
        surf_node_set_hidden(t->down[i], !on);
        surf_node_set_hidden(t->label[i], on);
        surf_node_set_hidden(t->label_on[i], !on);
    }
}

static void tabs_show(surf_tabs *t)
{
    for (int32_t i = 0; i < t->count; i++)
        surf_node_set_hidden(t->page[i], i != t->index);
}

static void tabs_set(surf_tabs *t, int32_t idx, bool report)
{
    if (idx < 0 || idx >= t->count || idx == t->index)
        return;
    t->index = idx;
    tabs_faces(t);
    tabs_show(t);
    if (report && t->cb)
        t->cb(idx, t->user);
}

static int32_t tab_at(surf_tabs *t, int16_t x, int16_t y)
{
    int16_t ax, ay;
    surf_node_abs_pos(t->strip, &ax, &ay);
    if (y < ay || y >= ay + t->tab_h)
        return -1;
    int32_t i = (x - ax) / t->tab_w;
    if (x < ax || i < 0 || i >= t->count)
        return -1;
    return i;
}

static void tabs_touch(surf_node *n, const surf_touch *tch, void *user)
{
    (void)n;
    surf_tabs *t = user;
    int32_t hit = tab_at(t, tch->x, tch->y);
    switch (tch->phase) {
    case SURF_TOUCH_DOWN:
    case SURF_TOUCH_MOVE:
        /* a finger that slides off the strip un-presses, like a button:
         * leaving before you let go is how you change your mind */
        t->pressed = (int16_t)hit;
        tabs_faces(t);
        break;
    case SURF_TOUCH_UP:
        t->pressed = -1;
        if (hit >= 0)
            tabs_set(t, hit, true);
        tabs_faces(t);
        break;
    }
}

surf_tabs *surf_tabs_new(surf_node *parent, int16_t x, int16_t y,
                         int16_t w, int16_t h, int16_t tab_h,
                         const surf_tabs_style *style,
                         const char *const *labels, int32_t count)
{
    if (!parent || !style || !style->patch || !style->font)
        return NULL;
    if (count < 1 || w < count || tab_h < 1 || h <= tab_h)
        return NULL;
    surf_tabs *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->count = count;
    t->w = w;
    t->h = h;
    t->tab_h = tab_h;
    t->tab_w = (int16_t)(w / count);
    t->pressed = -1;
    t->up = calloc((size_t)count, sizeof *t->up);
    t->down = calloc((size_t)count, sizeof *t->down);
    t->label = calloc((size_t)count, sizeof *t->label);
    t->label_on = calloc((size_t)count, sizeof *t->label_on);
    t->page = calloc((size_t)count, sizeof *t->page);
    t->root = surf_group_new(x, y);
    t->strip = surf_group_new(0, 0);
    /* our own copies of the style's image struct: shared pixels, our own
       tint, one per state — the same arrangement every A8 widget uses */
    t->on = *style->patch;
    t->on.tint = style->face ? style->face : SURF_RGB(47, 51, 62);
    t->off = *style->patch;
    t->off.tint = style->dim ? style->dim : SURF_RGB(30, 33, 40);
    if (!t->up || !t->down || !t->label || !t->label_on || !t->page ||
        !t->root || !t->strip) {
        surf_tabs_destroy(t);
        return NULL;
    }
    /* the strip is what the handler hangs on, so it needs a size — a
     * group is hittable only once it has been clipped */
    surf_group_set_clip(t->strip, w, tab_h);
    surf_node_add(t->root, t->strip);

    const surf_font *fon = style->font_active ? style->font_active
                                              : style->font;
    for (int32_t i = 0; i < count; i++) {
        int16_t tx = (int16_t)(i * t->tab_w);
        const char *lab = labels && labels[i] ? labels[i] : "";
        /* the SIDE insets keep the corner curve unstretched; the bottom
           inset is 2, because there is nothing down there to preserve */
        t->up[i] = surf_ninepatch_new(&t->off, tx, 0, t->tab_w, tab_h,
                                      style->inset_side, style->inset_top,
                                      style->inset_side, style->inset_bottom);
        t->down[i] = surf_ninepatch_new(&t->on, tx, 0, t->tab_w, tab_h,
                                        style->inset_side, style->inset_top,
                                        style->inset_side, style->inset_bottom);
        t->label[i] = surf_text_new(style->font, lab, tx, 0, style->text);
        t->label_on[i] = surf_text_new(fon, lab, tx, 0, style->text_active);
        /* a PAGE is a clipped group so its content cannot spill past the
         * area the caller asked for — and so it can carry a handler of
         * its own if the caller wants one */
        t->page[i] = surf_group_new(0, tab_h);
        if (!t->up[i] || !t->down[i] || !t->label[i] || !t->label_on[i] ||
            !t->page[i]) {
            surf_tabs_destroy(t);
            return NULL;
        }
        surf_group_set_clip(t->page[i], w, (int16_t)(h - tab_h));
        for (int k = 0; k < 2; k++) {
            surf_node *lb = k ? t->label_on[i] : t->label[i];
            surf_text_set_wrap(lb, t->tab_w);
            surf_text_set_align(lb, SURF_ALIGN_CENTER);
            surf_point ls = surf_node_size(lb);
            surf_node_set_pos(lb, tx, (int16_t)((tab_h - ls.y) / 2));
        }
        surf_node_add(t->strip, t->up[i]);
        surf_node_add(t->strip, t->down[i]);
        surf_node_add(t->strip, t->label[i]);
        surf_node_add(t->strip, t->label_on[i]);
        surf_node_add(t->root, t->page[i]);
    }
    tabs_faces(t);
    tabs_show(t);
    surf_node_set_on_touch(t->strip, tabs_touch, t);
    surf_node_add(parent, t->root);
    return t;
}

void surf_tabs_destroy(surf_tabs *t)
{
    if (!t)
        return;
    /* the root owns every node made above — the faces, the legends and
     * the pages are all descendants of it, and whatever the caller put
     * in a page goes with the page */
    surf_node_destroy(t->root);
    free(t->up);
    free(t->down);
    free(t->label);
    free(t->label_on);
    free(t->page);
    free(t);
}

surf_node *surf_tabs_node(surf_tabs *t) { return t ? t->root : NULL; }

surf_node *surf_tabs_page(surf_tabs *t, int32_t i)
{
    if (!t || i < 0 || i >= t->count)
        return NULL;
    return t->page[i];
}

int32_t surf_tabs_index(const surf_tabs *t) { return t ? t->index : 0; }
int32_t surf_tabs_count(const surf_tabs *t) { return t ? t->count : 0; }

/* the caller moving the tabs itself: no callback, the same rule every
 * other widget's set_value follows */
void surf_tabs_set_index(surf_tabs *t, int32_t idx)
{
    if (t)
        tabs_set(t, idx, false);
}

void surf_tabs_set_label(surf_tabs *t, int32_t i, const char *label)
{
    if (!t || i < 0 || i >= t->count)
        return;
    surf_text_set(t->label[i], label ? label : "");
    surf_text_set(t->label_on[i], label ? label : "");
}

/* Retint, for a caller whose page background can change under it (a
 * theme, a colour picker). TWO setters rather than one taking both,
 * because the common case is moving the face alone and "leave the other"
 * has no spare value to say it with: 0 is black, which is a colour
 * somebody will want. The sprites already point at these images, so a
 * repaint is the whole update. */
void surf_tabs_set_face(surf_tabs *t, surf_color c)
{
    if (!t || t->on.tint == c)
        return;
    t->on.tint = c;
    surf_node_damage(t->strip);
}

void surf_tabs_set_dim(surf_tabs *t, surf_color c)
{
    if (!t || t->off.tint == c)
        return;
    t->off.tint = c;
    surf_node_damage(t->strip);
}

void surf_tabs_on_change(surf_tabs *t, surf_index_cb cb, void *user)
{
    if (!t)
        return;
    t->cb = cb;
    t->user = user;
}
