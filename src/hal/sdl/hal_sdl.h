/* SDL2 desktop backend. Not part of the binding surface — demos and desktop
 * hosts include this directly. */
#ifndef SURF_HAL_SDL_H
#define SURF_HAL_SDL_H

#include "surfer.h"

const surf_hal *surf_hal_sdl_init(int16_t w, int16_t h, const char *title);
/* The SDL window, for the handful of things a host legitimately wants to
 * do to it that are none of this backend's business — a title, an icon,
 * a position — and for tests that need to drive a real resize. NULL
 * before init. */
struct SDL_Window *surf_hal_sdl_window(void);
void            surf_hal_sdl_quit(void);
/* A new framebuffer at a new shape on the SAME window, for a host whose
 * screen changed under it (a phone turned on its side). The window,
 * renderer and every UIKit object stay exactly as they are — see
 * bindings/surfer/surfer_port.h for why that matters. False if the
 * allocation fails, and the old framebuffer is then still good. */
bool            surf_hal_sdl_resize(int16_t w, int16_t h);
bool            surf_hal_sdl_pump(void);  /* process events; false on quit */

/* Desktop keyboard → textinput plumbing. A physical keyboard is a desktop
 * nicety; on device the on-screen keyboard widget feeds the same node
 * APIs, so none of this is in the hal vtable. */
typedef enum {
    SURF_KEY_TEXT = 0,   /* utf8[] holds the typed text */
    SURF_KEY_LEFT,
    SURF_KEY_RIGHT,
    SURF_KEY_UP,
    SURF_KEY_DOWN,
    SURF_KEY_PGUP,
    SURF_KEY_PGDN,
    SURF_KEY_HOME,
    SURF_KEY_END,
    SURF_KEY_BACKSPACE,
    SURF_KEY_DELETE,
    SURF_KEY_ENTER,
    SURF_KEY_ESC,
} surf_sdl_key_kind;

typedef struct {
    uint8_t kind;   /* surf_sdl_key_kind */
    bool    shift;  /* extend selection */
    bool    ctrl;   /* only on keys with no control character of their own
                     * — see surfer_key in surfer.h. ctrl+LETTER arrives
                     * as its control character with this FALSE, because
                     * the modifier is already in the text and a consumer
                     * that saw both would apply it twice. */
    char    utf8[8];
} surf_sdl_key;

/* Ctrl-C in the window. The hal can't know what interrupting means to
 * its host — a C demo might want to exit, a MicroPython host wants a
 * KeyboardInterrupt scheduled in the VM — so it calls this hook and
 * swallows the keystroke; the key never reaches the queue, exactly as on
 * device. NULL (the default) makes Ctrl-C a no-op, which is what every
 * host got before this existed. */
void surf_hal_sdl_on_interrupt(void (*fn)(void));

bool surf_hal_sdl_poll_key(surf_sdl_key *out);
/* keys currently down (state, not events) — up to max entries */
int  surf_hal_sdl_keys_held(surf_sdl_key *out, int max);

/* debug: write the current framebuffer as a binary PPM (P6) */
bool surf_hal_sdl_dump_ppm(const char *path);

/* debug: write what is actually presented (the streaming texture, read
 * back through the renderer) as a binary PPM. Differs from dump_ppm
 * exactly when present failed to keep the texture coherent with fb. */
bool surf_hal_sdl_dump_screen_ppm(const char *path);

#endif /* SURF_HAL_SDL_H */
