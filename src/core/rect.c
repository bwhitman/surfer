#include "surf_internal.h"

/* Intermediate math in int to dodge int16 overflow on far-apart rects;
 * results are only stored after clipping keeps them in range. */

surf_rect surf_rect_intersect(surf_rect a, surf_rect b)
{
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return (surf_rect){0, 0, 0, 0};
    return (surf_rect){(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
}

surf_rect surf_rect_union(surf_rect a, surf_rect b)
{
    if (surf_rect_empty(a)) return b;
    if (surf_rect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return (surf_rect){(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
}

bool surf_rect_overlaps(surf_rect a, surf_rect b)
{
    return !surf_rect_empty(a) && !surf_rect_empty(b) &&
           a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

bool surf_rect_covers(surf_rect a, surf_rect b)
{
    return a.x <= b.x && a.y <= b.y &&
           a.x + a.w >= b.x + b.w && a.y + a.h >= b.y + b.h;
}

bool surf_rect_contains(surf_rect r, int16_t x, int16_t y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void surf_dirty_reset(surf_dirty *d, surf_rect clip)
{
    d->n = 0;
    d->clip = clip;
}

void surf_dirty_add(surf_dirty *d, surf_rect r)
{
    /* EVEN-ALIGN BOTH AXES, and this is a hardware bug's fence, not
     * tidiness: the P4's SRM engine WEDGES on odd-width blocks — the
     * op is accepted by IDF's driver and never completes, which is the
     * exact stuck-transaction its own TODO admits it cannot recover
     * (ppa_dead's whole story, hal_p4.c). Measured on a P4X rev 3.2:
     * a sprite moving 3px a frame (odd-width damage every frame) kills
     * the engine inside a dozen ops; the same sprite at 2px a frame
     * runs forever. The dirty rect is the one place every block the
     * compositor hands the PPA descends from, and GROWING a damage
     * rect is always correct — everything intersecting it repaints —
     * where aligning individual ops in the hal would paint 1px stripes
     * of background over sprites that are not repainting this frame.
     * Evenness survives the clip below because every screen here has
     * even dimensions and the clip starts at 0. Costs at most one
     * extra pixel column and row per rect. */
    {
        int x1 = r.x + r.w, y1 = r.y + r.h;
        r.x &= (int16_t)~1;
        r.y &= (int16_t)~1;
        r.w = (int16_t)(((x1 - r.x) + 1) & ~1);
        r.h = (int16_t)(((y1 - r.y) + 1) & ~1);
    }
    r = surf_rect_intersect(r, d->clip);
    if (surf_rect_empty(r))
        return;

    /* Merge with any overlapping entry; the grown rect may newly overlap
     * others, so repeat until it settles. */
    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < d->n; i++) {
            if (surf_rect_overlaps(d->r[i], r)) {
                r = surf_rect_union(d->r[i], r);
                d->r[i] = d->r[--d->n];
                merged = true;
                break;
            }
        }
    }

    if (d->n == SURF_MAX_DIRTY) {
        /* Degrade to the bounding union rather than drop damage. */
        for (int i = 1; i < d->n; i++)
            d->r[0] = surf_rect_union(d->r[0], d->r[i]);
        d->r[0] = surf_rect_union(d->r[0], r);
        d->n = 1;
        return;
    }
    d->r[d->n++] = r;
}
