/* Scrollbar: a thumb on a track, sized and positioned from a content
 * model the caller owns.
 *
 * The widget knows nothing about what is scrolling. You hand it three
 * numbers in whatever unit suits you — total, visible, pos — and it
 * reports back a new pos when the user drags. Rows for a console,
 * lines for an editor, pixels for a panel: the widget only does ratios.
 *
 * It hides itself when total <= visible, so a caller can set the range
 * unconditionally and the bar simply appears when there is somewhere to
 * go. Both pieces are 9-patched capsules, so the ends stay round at any
 * length — no runtime drawing, per DESIGN.md.
 */
#include <stdlib.h>

#include "surfer.h"

struct surf_scrollbar {
    surf_node     *root, *track, *thumb;
    int16_t        len, thick;     /* along and across the axis */
    bool           vertical;
    int32_t        total, visible, pos;
    int16_t        grab;           /* offset within the thumb at DOWN */
    bool           dragging;
    surf_change_cb cb;
    void          *user;
};

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* thumb length and offset along the axis, from the content model */
static void geom(const surf_scrollbar *s, int16_t *off, int16_t *tlen)
{
    int32_t span = s->total > 0 ? s->total : 1;
    int32_t t = (int32_t)s->len * s->visible / span;
    int32_t min = s->thick * 3;           /* stays grabbable on a long doc */
    if (t < min) t = min;
    if (t > s->len) t = s->len;
    int32_t scrollable = s->total - s->visible;
    int32_t o = scrollable > 0
                    ? (int32_t)(s->len - t) * s->pos / scrollable
                    : 0;
    *tlen = (int16_t)t;
    *off = (int16_t)clamp32(o, 0, s->len - t);
}

static void apply(surf_scrollbar *s)
{
    bool show = s->total > s->visible;
    surf_node_set_hidden(s->root, !show);
    if (!show)
        return;
    int16_t off, tlen;
    geom(s, &off, &tlen);
    if (s->vertical) {
        surf_ninepatch_set_size(s->thumb, s->thick, tlen);
        surf_node_set_pos(s->thumb, 0, off);
    } else {
        surf_ninepatch_set_size(s->thumb, tlen, s->thick);
        surf_node_set_pos(s->thumb, off, 0);
    }
}

static void report(surf_scrollbar *s, int32_t pos)
{
    pos = clamp32(pos, 0, s->total - s->visible > 0 ? s->total - s->visible : 0);
    if (pos == s->pos)
        return;
    s->pos = pos;
    apply(s);
    if (s->cb)
        s->cb(pos, s->user);
}

static void sb_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surf_scrollbar *s = user;
    if (s->total <= s->visible)
        return;
    int16_t ax, ay;
    surf_node_abs_pos(s->root, &ax, &ay);
    int16_t along = s->vertical ? (int16_t)(t->y - ay) : (int16_t)(t->x - ax);
    int16_t off, tlen;
    geom(s, &off, &tlen);

    if (t->phase == SURF_TOUCH_DOWN) {
        if (along >= off && along < off + tlen) {
            s->grab = (int16_t)(along - off);   /* grabbed the thumb */
        } else {
            /* clicked the track: jump so the thumb centres on the click,
             * which is what a page-scroll feels like on a touchscreen */
            s->grab = (int16_t)(tlen / 2);
            report(s, (int32_t)(along - s->grab) * (s->total - s->visible) /
                          (s->len - tlen ? s->len - tlen : 1));
        }
        s->dragging = true;
        return;
    }
    if (t->phase == SURF_TOUCH_UP) {
        s->dragging = false;
        return;
    }
    if (!s->dragging)
        return;
    int32_t range = s->len - tlen;
    if (range <= 0)
        return;
    report(s, (int32_t)(along - s->grab) * (s->total - s->visible) / range);
}

surf_scrollbar *surf_scrollbar_new(surf_node *parent, int16_t x, int16_t y,
                                   int16_t len, bool vertical,
                                   const surf_scrollbar_style *style)
{
    if (!parent || !style || !style->thumb || len <= 0)
        return NULL;
    surf_scrollbar *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    /* horizontal wants the capsule lying down, and the 9-patch insets on
     * the other pair of edges — the round ends have to sit at the ends of
     * the AXIS, or the stretch tiles a cap into beads */
    const surf_image *thumb = !vertical && style->thumb_h ? style->thumb_h
                                                          : style->thumb;
    const surf_image *track = !vertical && style->track_h ? style->track_h
                                                          : style->track;
    s->len = len;
    s->thick = vertical ? thumb->w : thumb->h;
    s->vertical = vertical;
    s->total = s->visible = 1;
    s->root = surf_group_new(x, y);
    if (!s->root) {
        free(s);
        return NULL;
    }
    int16_t w = vertical ? s->thick : len;
    int16_t h = vertical ? len : s->thick;
    int16_t il = vertical ? 0 : style->inset, it = vertical ? style->inset : 0;
    if (track) {
        s->track = surf_ninepatch_new(track, 0, 0, w, h, il, it, il, it);
        if (s->track)
            surf_node_add(s->root, s->track);
    }
    s->thumb = surf_ninepatch_new(thumb, 0, 0,
                                  vertical ? s->thick : s->thick * 3,
                                  vertical ? s->thick * 3 : s->thick,
                                  il, it, il, it);
    if (!s->thumb) {
        surf_node_destroy(s->root);
        free(s);
        return NULL;
    }
    surf_node_add(s->root, s->thumb);
    /* the whole widget captures: a drag on a scrollbar is never a scroll
     * of whatever it sits in */
    surf_node_set_on_touch(s->root, sb_touch, s);
    surf_node_set_gesture_grab(s->root, true);
    surf_node_set_hidden(s->root, true);      /* nothing to scroll yet */
    surf_node_add(parent, s->root);
    return s;
}

void surf_scrollbar_destroy(surf_scrollbar *s)
{
    if (!s)
        return;
    surf_node_destroy(s->root);
    free(s);
}

surf_node *surf_scrollbar_node(surf_scrollbar *s) { return s ? s->root : NULL; }

void surf_scrollbar_set_range(surf_scrollbar *s, int32_t total, int32_t visible,
                              int32_t pos)
{
    if (!s)
        return;
    s->total = total > 0 ? total : 0;
    s->visible = visible > 0 ? visible : 1;
    s->pos = clamp32(pos, 0, s->total - s->visible > 0 ? s->total - s->visible : 0);
    apply(s);
}

void surf_scrollbar_set_pos(surf_scrollbar *s, int32_t pos)
{
    if (!s)
        return;
    s->pos = clamp32(pos, 0, s->total - s->visible > 0 ? s->total - s->visible : 0);
    apply(s);
}

int32_t surf_scrollbar_pos(const surf_scrollbar *s) { return s ? s->pos : 0; }

void surf_scrollbar_on_change(surf_scrollbar *s, surf_change_cb cb, void *user)
{
    if (!s)
        return;
    s->cb = cb;
    s->user = user;
}
