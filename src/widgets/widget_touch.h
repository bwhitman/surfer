/* One finger at a time, per widget.
 *
 * Dispatch captures PER CONTACT (src/core/input.c), which is what makes
 * three fingers on three faders three independent drags. The other half
 * of that is here: two fingers on the SAME fader must not both drive it,
 * or the cap jumps between them every event and the value ends up
 * wherever the last one happened to be.
 *
 * So a widget claims the first contact that presses it and ignores every
 * other until that one lifts. The second finger is not queued — it is
 * dropped, which is what a physical control does: your other hand does
 * not get a turn because it also touched the knob.
 *
 * `busy` is the contact id PLUS ONE, so that zero means idle and a
 * calloc'd widget starts in the right state with no constructor to
 * remember. That is worth the small ugliness: every widget here is
 * calloc'd, and an `int8_t active = -1` would have needed eight separate
 * initialisations and would have been silently wrong in whichever one
 * got forgotten. */
#ifndef SURF_WIDGET_TOUCH_H
#define SURF_WIDGET_TOUCH_H

#include <stdbool.h>

#include "surfer.h"

static inline bool surf_widget_claim(uint8_t *busy, const surf_touch *t)
{
    uint8_t want = (uint8_t)(t->id + 1);
    if (t->phase == SURF_TOUCH_DOWN) {
        if (*busy && *busy != want)
            return false;              /* another finger already owns it */
        *busy = want;
        return true;
    }
    if (*busy != want)
        return false;                  /* not the finger we are following */
    if (t->phase == SURF_TOUCH_UP)
        *busy = 0;
    return true;
}

#endif
