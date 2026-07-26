/* Aspect snap: the window goes back on the framebuffer's shape after a
 * resize, keeping the axis that was dragged.
 *
 * A real drag cannot be scripted, but the thing that matters can:
 * SDL_SetWindowSize produces the same SIZE_CHANGED the drag does, so
 * resizing the window from here and pumping past the quiet period puts
 * the snap through exactly the path a user's drag takes. Opens a window
 * (SDL needs one for the events to be real), so it is not part of `make
 * test` — run it by hand, or with SDL_VIDEODRIVER=dummy in CI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "surfer.h"
#include "hal_sdl.h"

#define W 400
#define H 250

static int fails;

static void ok(int cond, const char *what)
{
    if (!cond) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

/* pump past the quiet period so the deferred snap fires */
static void settle(void)
{
    for (int i = 0; i < 40; i++) {
        surf_hal_sdl_pump();
        surf_tick();
        SDL_Delay(10);
    }
}

static void check(int set_w, int set_h, const char *what)
{
    SDL_Window *win = (SDL_Window *)surf_hal_sdl_window();
    SDL_SetWindowSize(win, set_w, set_h);
    settle();
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    /* the aspect is what matters, not the exact pixels: rounding to whole
     * points cannot always hit it dead on */
    double want = (double)W / H, got = (double)w / h;
    int close = got > want * 0.985 && got < want * 1.015;
    printf("  %-34s asked %4dx%-4d -> %4dx%-4d  (%.3f vs %.3f) %s\n",
           what, set_w, set_h, w, h, got, want, close ? "ok" : "OFF");
    ok(close, what);
}

int main(void)
{
    const surf_hal *hal = surf_hal_sdl_init(W, H, "aspect");
    if (!hal || !surf_init(hal, W, H, &(surf_config){.max_nodes = 16})) {
        printf("aspect test: init failed\n");
        return 1;
    }
    surf_node_add(surf_screen(), surf_rect_new(0, 0, W, H, 0x1234));
    settle();

    check(W, H + 120, "drag the bottom edge down");
    check(W + 160, H, "drag the right edge across");
    check(W * 2, H * 2 + 90, "drag a corner, overshooting");
    check(160, 400, "squash it tall and narrow");

    /* ...and the escape hatch really escapes */
    surf_deinit();
    surf_hal_sdl_quit();
    setenv("SURF_FREE_ASPECT", "1", 1);
    hal = surf_hal_sdl_init(W, H, "aspect (free)");
    surf_init(hal, W, H, &(surf_config){.max_nodes = 16});
    surf_node_add(surf_screen(), surf_rect_new(0, 0, W, H, 0x1234));
    settle();
    SDL_SetWindowSize((SDL_Window *)surf_hal_sdl_window(), W, H + 120);
    settle();
    int w = 0, h = 0;
    SDL_GetWindowSize((SDL_Window *)surf_hal_sdl_window(), &w, &h);
    printf("  %-34s asked %4dx%-4d -> %4dx%-4d %s\n", "SURF_FREE_ASPECT=1 leaves it",
           W, H + 120, w, h, h == H + 120 ? "ok" : "OFF");
    ok(h == H + 120, "SURF_FREE_ASPECT=1 leaves the window alone");

    surf_deinit();
    surf_hal_sdl_quit();
    printf("sdl_aspect_test: %s\n", fails ? "FAILURES" : "all OK");
    return fails ? 1 : 0;
}
