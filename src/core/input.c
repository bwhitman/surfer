#include "surf_internal.h"

/* Touch → hit test → nearest ancestor that wants the gesture: a node with
 * a handler, or a scrollable scrollview (whichever comes first walking
 * up). The winner holds that CONTACT from DOWN to UP (DESIGN.md §2.6).
 *
 * When a handler wins inside a scrollview, the scrollview waits: once the
 * finger travels past STEAL_PX along an axis it can scroll, it steals the
 * gesture — the handler gets a synthetic UP and the drag becomes a
 * scroll. Handlers that own their drags (sliders, knobs, textinput
 * selection) set SURF_NF_GRAB and are never stolen from.
 *
 * CAPTURE IS PER CONTACT. There used to be one `capture` for the whole
 * scene, which meant the first finger down owned the machine: three
 * fingers on three faders moved one fader. Every piece of that state —
 * the captured node, the scrollview waiting to steal, the position the
 * gesture started from — is per finger now, because all three are
 * answers to "what is THIS finger doing".
 *
 * A hal with no multitouch says nothing about contacts and its events
 * carry id 0, which lands in one slot and behaves exactly as before. */

#define STEAL_PX 8

surf_contact *surf_contact_find(uint8_t id)
{
    for (int i = 0; i < SURF_MAX_CONTACTS; i++)
        if (surf_g.contacts[i].used && surf_g.contacts[i].id == id)
            return &surf_g.contacts[i];
    return NULL;
}

/* A DOWN for an id already in flight REPLACES it rather than opening a
 * second slot: a controller that misses an UP (the GT911 does, when a
 * finger lifts during an i2c hiccup) would otherwise leak slots until
 * the table is full and further fingers are silently ignored. */
static surf_contact *contact_open(uint8_t id)
{
    surf_contact *c = surf_contact_find(id);
    if (!c) {
        for (int i = 0; i < SURF_MAX_CONTACTS; i++) {
            if (!surf_g.contacts[i].used) {
                c = &surf_g.contacts[i];
                break;
            }
        }
    }
    if (!c)
        return NULL;             /* more fingers than we follow: ignore */
    c->used = true;
    c->id = id;
    c->capture = NULL;
    c->steal_sv = NULL;
    return c;
}

static void deliver(surf_node *n, const surf_touch *t)
{
    if (n->type == SURF_NODE_SCROLLVIEW && !n->on_touch)
        surf_scroll_touch(n, t);
    else if (n->on_touch)
        n->on_touch(n, t, n->touch_user);
}

/* A WHEEL (or a two-finger trackpad push) over a scrollview.
 *
 * Drag was the only way to scroll anything here, which is right for a
 * touchscreen and wrong for the two targets that have a mouse: on a
 * laptop the natural gesture is two fingers, and it did nothing at all.
 * The hal turns its wheel event into this; the walk is the dispatch's
 * own — the first scrollable ancestor of whatever is under the pointer,
 * so a list inside a panel scrolls the list and a page behind it stays
 * put.
 *
 * It moves the view directly rather than faking a drag: a wheel has no
 * press and no release, and a synthetic down/up pair would light up
 * every row it passed over. */
void surf_input_wheel(int16_t x, int16_t y, int16_t dx, int16_t dy)
{
    for (surf_node *n = surf_hit_test(x, y); n; n = n->parent) {
        if (n->type != SURF_NODE_SCROLLVIEW)
            continue;
        bool cx = surf_scroll_can_x(n), cy = surf_scroll_can_y(n);
        if (!cx && !cy)
            continue;
        surf_point off = surf_scrollview_offset(n);
        surf_scrollview_set_offset(n, (int16_t)(cx ? off.x + dx : off.x),
                                   (int16_t)(cy ? off.y + dy : off.y));
        return;
    }
}

void surf_input_dispatch(const surf_touch *t)
{
    switch (t->phase) {
    case SURF_TOUCH_DOWN: {
        surf_contact *ct = contact_open(t->id);
        if (!ct)
            return;
        ct->down_x = t->x;
        ct->down_y = t->y;

        surf_node *n = surf_hit_test(t->x, t->y);
        for (; n; n = n->parent) {
            if (n->on_touch)
                break;
            if (n->type == SURF_NODE_SCROLLVIEW &&
                (surf_scroll_can_x(n) || surf_scroll_can_y(n)))
                break;
        }
        if (!n)
            return;              /* the slot stays live but captures nothing */
        ct->capture = n;
        if (n->type == SURF_NODE_SCROLLVIEW && !n->on_touch) {
            surf_scroll_begin(n, t);
            return;
        }
        n->on_touch(n, t, n->touch_user);
        if (!(n->flags & SURF_NF_GRAB)) {
            for (surf_node *p = n->parent; p; p = p->parent) {
                if (p->type == SURF_NODE_SCROLLVIEW &&
                    (surf_scroll_can_x(p) || surf_scroll_can_y(p))) {
                    ct->steal_sv = p;
                    break;
                }
            }
        }
        break;
    }
    case SURF_TOUCH_MOVE: {
        surf_contact *ct = surf_contact_find(t->id);
        if (!ct || !ct->capture)
            return;
        surf_node *c = ct->capture;
        surf_node *sv = ct->steal_sv;
        if (sv) {
            int dx = t->x - ct->down_x, dy = t->y - ct->down_y;
            int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            bool steal = (ady >= adx) ? (ady > STEAL_PX && surf_scroll_can_y(sv))
                                      : (adx > STEAL_PX && surf_scroll_can_x(sv));
            if (steal) {
                surf_touch up = {t->x, t->y, SURF_TOUCH_UP, t->id};
                deliver(c, &up);
                ct->capture = sv;
                ct->steal_sv = NULL;
                surf_scroll_begin(sv, t);
                return;
            }
        }
        deliver(c, t);
        break;
    }
    case SURF_TOUCH_UP: {
        surf_contact *ct = surf_contact_find(t->id);
        if (!ct)
            return;
        surf_node *c = ct->capture;
        ct->used = false;
        ct->capture = NULL;
        ct->steal_sv = NULL;
        if (c)
            deliver(c, t);
        break;
    }
    }
}
