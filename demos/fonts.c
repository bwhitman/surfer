/* Font specimen (desktop host). The scene itself lives in fonts_scene.c,
 * shared with the P4 firmware, at 1024x600 — the exact framebuffer the 7"
 * panel gets, so the window and the panel show the same pixels.
 * Esc/close quits; argv[1] caps frames; SURF_SHOT=x.ppm dumps the fb. */
#include <stdio.h>
#include <stdlib.h>

#include "surfer.h"
#include "hal_sdl.h"
#include "surf_internal.h"   /* SURF_TAP test hook only */
#include "fonts_scene.h"


#define W 1024
#define H 600

int main(int argc, char **argv)
{
    long max_frames = argc > 1 ? strtol(argv[1], NULL, 10) : 0;

    const surf_hal *hal = surf_hal_sdl_init(W, H, "surfer — font specimen");
    if (!hal || !surf_init(hal, W, H, &(surf_config){.max_nodes = 160,
                                                     .bg = SURF_RGB(24, 26, 32)})) {
        fprintf(stderr, "fonts: init failed\n");
        return 1;
    }

    const char *pe = getenv("SURF_PAGE");
    fonts_scene_build(W, H, "1024x600 - the 7\" panel framebuffer",
                      pe ? atoi(pe) : 1);

    /* SURF_TAP=x,y injects one synthetic tap after a few frames, through
     * the real path (hit test -> parent walk -> handler -> damage). The
     * page flip is otherwise only reachable by hand, which is how it
     * shipped broken once. */
    int tap_x = -1, tap_y = -1;
    const char *te = getenv("SURF_TAP");
    if (te)
        sscanf(te, "%d,%d", &tap_x, &tap_y);

    long frames = 0;
    while (surf_hal_sdl_pump()) {
        if (te && frames == 3) {
            surf_input_dispatch(&(surf_touch){(int16_t)tap_x, (int16_t)tap_y,
                                              SURF_TOUCH_DOWN});
            surf_input_dispatch(&(surf_touch){(int16_t)tap_x, (int16_t)tap_y,
                                              SURF_TOUCH_UP});
        }
        surf_tick();
        if (max_frames && ++frames >= max_frames)
            break;
    }

    if (getenv("SURF_SHOT"))
        surf_hal_sdl_dump_ppm(getenv("SURF_SHOT"));
    surf_deinit();
    surf_hal_sdl_quit();
    return 0;
}
