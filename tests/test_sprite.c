/* Transformed sprites: footprint math, damage on xform writes, and the
 * compose path handing transformed draws to hal->xform_blend. */
#include "mock_hal.h"

void run_sprite_tests(void);

static uint16_t px_dummy[64 * 64];
static const surf_image img64 = {
    .pixels = px_dummy, .w = 64, .h = 64, .stride = 64 * 2,
    .format = SURF_FMT_RGB565, .opaque = false,
};

/* A panning sprite (forest's camera) band-shifts its rect, dragging the
 * pixels of anything drawn over it. Repairing only LATER SIBLINGS misses
 * a host's chrome, which lives in another branch of the tree. */
static void test_sprite_pan_damages_other_branches(void)
{
    fresh(200, 100, 32);
    surf_node *app = surf_group_new(0, 0);
    surf_node_add(surf_screen(), app);
    static uint16_t world_px[128 * 64];
    static const surf_image world = {
        .pixels = world_px, .w = 128, .h = 64, .stride = 128 * 2,
        .format = SURF_FMT_RGB565, .opaque = true,   /* fast pan needs it */
    };
    surf_node *cam = surf_sprite_new(&world, 0, 0);
    surf_sprite_set_fast_pan(cam, true);
    surf_sprite_set_src(cam, (surf_rect){0, 0, 64, 32});
    surf_node_add(app, cam);

    surf_node *chrome = surf_group_new(0, 0);      /* the task bar */
    surf_node_add(surf_screen(), chrome);
    surf_node *btn = surf_rect_new(40, 4, 20, 12, 0xf800);
    surf_node_add(chrome, btn);
    surf_tick();

    surf_g.dirty.n = 0;
    surf_sprite_set_src(cam, (surf_rect){4, 0, 64, 32});   /* pan under it */
    OK(surf_g.dirty.n > 0 && surf_g.dirty.n < 6);   /* the FAST path ran */
    bool covered = false;
    for (int i = 0; i < surf_g.dirty.n; i++) {
        surf_rect r = surf_rect_intersect(surf_g.dirty.r[i],
                                          (surf_rect){40, 4, 20, 12});
        if (r.w >= 20 && r.h >= 12)
            covered = true;
    }
    OK(covered);
    surf_node_destroy(app);
    surf_node_destroy(chrome);
}

/* ...and the same walk must SKIP a hidden branch: tulip hides a
 * backgrounded app's group, never its children, so every child passed
 * the per-node flag test on its own and polluted the dirty list. */
static void test_sprite_pan_skips_hidden_branches(void)
{
    fresh(200, 100, 32);
    static uint16_t world_px[128 * 64];
    static const surf_image world = {
        .pixels = world_px, .w = 128, .h = 64, .stride = 128 * 2,
        .format = SURF_FMT_RGB565, .opaque = true,   /* fast pan needs it */
    };
    surf_node *cam = surf_sprite_new(&world, 0, 0);
    surf_sprite_set_fast_pan(cam, true);
    surf_sprite_set_src(cam, (surf_rect){0, 0, 64, 32});
    surf_node_add(surf_screen(), cam);

    surf_node *bg = surf_group_new(0, 0);           /* a backgrounded app */
    surf_node_add(surf_screen(), bg);
    surf_node *hidden = surf_rect_new(8, 4, 20, 12, 0x0111);
    surf_node_add(bg, hidden);
    surf_node_set_hidden(bg, true);                 /* the GROUP, not it */

    surf_node *chrome = surf_rect_new(40, 4, 20, 12, 0xf800);
    surf_node_add(surf_screen(), chrome);
    surf_tick();

    surf_g.dirty.n = 0;
    surf_sprite_set_src(cam, (surf_rect){4, 0, 64, 32});
    OK(surf_g.dirty.n > 0 && surf_g.dirty.n < 6);   /* the FAST path ran */
    bool hit_hidden = false, hit_chrome = false;
    for (int i = 0; i < surf_g.dirty.n; i++) {
        if (surf_rect_overlaps(surf_g.dirty.r[i], (surf_rect){8, 4, 20, 12}))
            hit_hidden = true;
        if (surf_rect_overlaps(surf_g.dirty.r[i], (surf_rect){40, 4, 20, 12}))
            hit_chrome = true;
    }
    OK(!hit_hidden);
    OK(hit_chrome);
    surf_node_destroy(bg);
    surf_node_destroy(chrome);
    surf_node_destroy(cam);
}

/* surf_image_flush(): the contract a software renderer depends on.
 *
 * Anything that writes an image's own pixels every frame -- a scope, a video
 * decoder, an emulator -- has to publish them before the blitter reads. On a
 * backend whose blitter IS the CPU that is free, which is exactly why it
 * needs a test: the call can be forgotten and nothing on a desktop will ever
 * notice. Here the mock hal records it, so a flush that stops reaching the
 * hal is a red test rather than a torn picture on hardware. */
static void test_image_flush(void)
{
    fresh(200, 200, 16);

    surf_image *im = surf_image_new(37, 11, SURF_FMT_RGB565);
    OK(im != NULL);
    if (!im)
        return;

    mock_sync_calls = 0;
    mock_sync_buf = NULL;
    mock_sync_bytes = 0;

    surf_image_flush(im);
    OK(mock_sync_calls == 1);
    /* the WHOLE allocation, stride included -- not w * 2 * h. A hal that
     * pads rows (the P4 aligns both ways) would leave the tail of every row
     * unwritten if this used the visible width. */
    OK(mock_sync_buf == im->pixels);
    OK(mock_sync_bytes == (size_t)im->stride * im->h);

    /* defensive, and cheap: neither a NULL image nor one whose pixels are
     * gone may reach the hal */
    surf_image_flush(NULL);
    OK(mock_sync_calls == 1);

    surf_image_destroy(im);
}

/* A filmstrip that PLAYS itself, and the bookkeeping that keeps the
 * per-tick scan from running for animations nobody owns. */
static void test_filmstrip_play(void)
{
    fresh(200, 200, 64);
    static uint32_t px[8 * 4 * 8];
    surf_image strip = {.pixels = px, .w = 8 * 4, .h = 8, .stride = 8 * 4 * 4,
                        .format = SURF_FMT_ARGB8888, .opaque = true};
    surf_node *n = surf_filmstrip_new(&strip, 8, 8, 10, 10);
    surf_node_add(surf_screen(), n);
    /* the node is one FRAME, not the whole sheet */
    OK(surf_node_size(n).x == 8 && surf_node_size(n).y == 8);

    surf_filmstrip_set_frame(n, 2);
    OK(surf_filmstrip_frame(n) == 2);
    surf_filmstrip_set_frame(n, 99);
    OK(surf_filmstrip_frame(n) == 3);          /* clamped to the last cel */
    surf_filmstrip_set_frame(n, -5);
    OK(surf_filmstrip_frame(n) == 0);

    /* fps 0 is the default and the frame is the caller's */
    OK(surf_filmstrip_fps(n) == 0);
    mock_advance_us(1000000);
    surf_tick();
    OK(surf_filmstrip_frame(n) == 0);

    surf_filmstrip_set_fps(n, SURF_ONE * 10);  /* 10 fps = 100 ms a cel */
    surf_tick();                               /* anchors the clock */
    OK(surf_filmstrip_frame(n) == 0);
    mock_advance_us(100000);
    surf_tick();
    OK(surf_filmstrip_frame(n) == 1);
    mock_advance_us(100000);
    surf_tick();
    OK(surf_filmstrip_frame(n) == 2);
    /* WRAPS rather than stopping at the end */
    mock_advance_us(200000);
    surf_tick();
    mock_advance_us(0);
    OK(surf_filmstrip_frame(n) == 3 || surf_filmstrip_frame(n) == 0);

    /* A LATE frame is dropped, not replayed: a tab hidden for a minute
     * must not flip through thousands of cels to catch up. */
    surf_filmstrip_set_frame(n, 0);
    mock_advance_us(60ull * 1000000);
    surf_tick();
    OK(surf_filmstrip_frame(n) < 4);

    /* and the count goes back when the node dies, or the per-tick scan
     * runs for ever for an animation nobody owns */
    surf_node_destroy(n);
    surf_tick();
    OK(1);
}

/* An image can be written as a PNG and read straight back. */
static void test_png_round_trip(void)
{
    fresh(64, 64, 64);
    surf_image *img = surf_image_new(9, 5, SURF_FMT_ARGB8888);
    OK(img != NULL);
    surf_image_fill(img, (surf_rect){0, 0, 9, 5}, SURF_RGB(255, 0, 0));
    surf_image_fill(img, (surf_rect){2, 1, 3, 2}, SURF_RGB(0, 0, 255));
    size_t len = 0;
    void *png = surf_image_to_png(img, &len);
    OK(png != NULL && len > 8);
    OK(((const unsigned char *)png)[0] == 0x89
       && ((const unsigned char *)png)[1] == 'P');
    surf_image *back = surf_image_from_png(png, len);
    OK(back != NULL);
    OK(back && back->w == 9 && back->h == 5);
    if (back) {
        /* The blue patch is where it was put -- a decoder that read the
         * stride wrong still gives the right SIZE, so position is the
         * thing worth asserting.
         *
         * 0xf8 and not 0xff: the colour went in as RGB565 through
         * surf_image_fill, which widens 5 bits to 8 by SHIFTING, so 0x1f
         * becomes 0xf8. The encoder is faithful to the pixels it is
         * given and does not invent the missing three bits back. */
        uint32_t p = ((const uint32_t *)((const uint8_t *)back->pixels
                                         + 1 * back->stride))[3];
        OK((p & 0xffffff) == 0x0000f8);
        surf_image_destroy(back);
    }
    surf_image_png_free(png);
    surf_image_destroy(img);
}

/* A FILMSTRIP SCALES, and until now it silently did not.
 *
 * surf_sprite_set_xform opened with `n->type != SURF_NODE_SPRITE` and
 * RETURNED, so setting a scale on an animation did nothing at all and
 * read back as 1.0 — the worst shape a setter has, because the code
 * looks right and the picture never changes. Reported from a Tulip as
 * three turns spent asking an assistant to scale an explosion that was
 * never going to scale.
 *
 * The other half is what the transformed path is handed: a strip is
 * MANY pictures in one image, so passing the whole image the way the
 * sprite path passes `src` would squeeze every cel into one frame's
 * footprint. The source must be this frame's cell. */
static void test_filmstrip_xform(void)
{
    fresh(200, 200, 64);
    static uint32_t px[8 * 4 * 8];
    surf_image strip = {.pixels = px, .w = 8 * 4, .h = 8, .stride = 8 * 4 * 4,
                        .format = SURF_FMT_ARGB8888, .opaque = true};
    surf_node *n = surf_filmstrip_new(&strip, 8, 8, 10, 10);
    surf_node_add(surf_screen(), n);

    /* it starts 1:1 -- node_alloc zeroes the union, and a scale of 0 is
     * a footprint of nothing rather than "unscaled" */
    OK(surf_sprite_scale(n) == SURF_ONE);
    OK(surf_node_size(n).x == 8 && surf_node_size(n).y == 8);

    surf_sprite_set_xform(n, SURF_ONE * 3, 0, 0);
    OK(surf_sprite_scale(n) == SURF_ONE * 3);
    /* the FOOTPRINT follows, and it is one frame scaled -- not the sheet */
    OK(surf_node_size(n).x == 24 && surf_node_size(n).y == 24);

    /* a quarter turn swaps the sides, exactly as it does for a sprite */
    surf_sprite_set_xform(n, SURF_ONE * 2, 1, 0);
    OK(surf_sprite_rot(n) == 1);
    OK(surf_node_size(n).x == 16 && surf_node_size(n).y == 16);
    OK(surf_sprite_mirror(n) == 0);
    surf_sprite_set_xform(n, SURF_ONE * 2, 1, 3);
    OK(surf_sprite_mirror(n) == 3);

    /* and it COMPOSES through the transform path, from this frame's
     * cell. Frame 2 of an 8px strip starts at x=16; a hal handed 0
     * would be drawing the whole animation at once. */
    surf_sprite_set_xform(n, SURF_ONE * 2, 0, 0);
    surf_filmstrip_set_frame(n, 2);
    surf_tick();
    nops = 0;
    surf_node_set_pos(n, 12, 10);
    surf_tick();
    bool sawX = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'X') {
            sawX = true;
            /* THE CELL, not the sheet: frame 2 of an 8px strip starts
             * at x=16 and is 8 wide. A hal handed {0,0,32,8} would be
             * drawing every cel at once, squeezed into one footprint. */
            OK(rect_eq(ops[i].src, (surf_rect){16, 0, 8, 8}));
            OK(rect_eq(ops[i].r, (surf_rect){12, 10, 16, 16}));
        }
    OK(sawX);

    /* back to 1:1 and it is the plain blit again -- the fast path must
     * not be lost to a node that merely COULD be transformed */
    surf_sprite_set_xform(n, SURF_ONE, 0, 0);
    surf_tick();
    nops = 0;
    surf_node_set_pos(n, 14, 10);
    surf_tick();
    sawX = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'X') sawX = true;
    OK(!sawX);
    OK(surf_node_size(n).x == 8 && surf_node_size(n).y == 8);
}

void run_sprite_tests(void)
{
    test_filmstrip_play();
    test_filmstrip_xform();
    test_png_round_trip();
    test_image_flush();
    test_sprite_pan_damages_other_branches();
    test_sprite_pan_skips_hidden_branches();
    fresh(200, 200, 16);
    surf_node *s = surf_sprite_new(&img64, 10, 10);
    surf_node_add(surf_screen(), s);
    surf_tick();
    nops = 0;

    /* identity transform stays on the plain blend path */
    surf_node_set_pos(s, 12, 10);
    surf_tick();
    bool sawA = false, sawX = false;
    for (int i = 0; i < nops; i++) {
        if (ops[i].op == 'A') sawA = true;
        if (ops[i].op == 'X') sawX = true;
    }
    OK(sawA && !sawX);

    /* footprint: scale 2x doubles, quarter turn swaps sides */
    surf_sprite_set_xform(s, SURF_ONE * 2, 0, 0);
    OK(surf_node_size(s).x == 128 && surf_node_size(s).y == 128);
    surf_sprite_set_xform(s, SURF_ONE / 2, 1, 0);
    OK(surf_node_size(s).x == 32 && surf_node_size(s).y == 32);
    surf_sprite_set_src(s, (surf_rect){0, 0, 64, 32});
    OK(surf_node_size(s).x == 16 && surf_node_size(s).y == 32);  /* rot 1: swapped */
    OK(surf_sprite_scale(s) == SURF_ONE / 2 && surf_sprite_rot(s) == 1);

    /* transformed draw goes through xform_blend with the right rects */
    surf_tick();
    nops = 0;
    surf_node_set_pos(s, 20, 20);
    surf_tick();
    sawX = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'X') {
            sawX = true;
            OK(rect_eq(ops[i].r, (surf_rect){20, 20, 16, 32}));
            OK(ops[i].rot == 1);
            OK(rect_eq(ops[i].src, (surf_rect){0, 0, 64, 32}));
            /* vis stays inside the footprint */
            OK(rect_eq(surf_rect_intersect(ops[i].vis, ops[i].r), ops[i].vis));
        }
    OK(sawX);

    /* an xform write redraws the new footprint (old is inside it here) */
    surf_tick();
    nops = 0;
    surf_sprite_set_xform(s, SURF_ONE * 2, 0, 0);   /* -> 128x64 at 20,20 */
    surf_tick();
    bool covered_new = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'X' && rect_eq(ops[i].r, (surf_rect){20, 20, 128, 64}))
            covered_new = true;
    OK(covered_new);

    /* clamping: absurd scales stay in the PPA's range */
    surf_sprite_set_xform(s, SURF_ONE * 100, 0, 0);
    OK(surf_sprite_scale(s) == SURF_ONE * 16);

    /* mirror alone takes the xform path, footprint unchanged */
    surf_sprite_set_xform(s, SURF_ONE, 0, 1);
    OK(surf_sprite_mirror(s) == 1);
    OK(surf_node_size(s).x == 64 && surf_node_size(s).y == 32);
    surf_tick();
    nops = 0;
    surf_node_set_pos(s, 24, 24);
    surf_tick();
    bool saw_mirror = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'X' && ops[i].mirror == 1 && ops[i].rot == 0)
            saw_mirror = true;
    OK(saw_mirror);

    surf_node_destroy(s);

    /* footprint collision: overlap, separation, transform, hidden */
    surf_node *c1 = surf_sprite_new(&img64, 10, 10);
    surf_node *c2 = surf_sprite_new(&img64, 50, 50);
    surf_node_add(surf_screen(), c1);
    surf_node_add(surf_screen(), c2);
    OK(surf_node_overlaps(c1, c2));                 /* 64x64 at 40px apart */
    surf_node_set_pos(c2, 100, 10);
    OK(!surf_node_overlaps(c1, c2));
    surf_sprite_set_xform(c2, SURF_ONE * 2, 0, 0);  /* 128 wide: reaches back */
    surf_node_set_pos(c2, 70, 10);
    OK(surf_node_overlaps(c1, c2));
    surf_node_set_hidden(c2, true);
    OK(!surf_node_overlaps(c1, c2));
    surf_node_destroy(c1);
    surf_node_destroy(c2);

    /* ---- load-time image builder ---- */
    surf_image *base = surf_image_new(64, 32, SURF_FMT_RGB565);
    OK(base && base->opaque && base->stride % 64 == 0);
    surf_image_fill(base, (surf_rect){0, 0, 64, 32}, SURF_RGB(255, 0, 0));
    uint16_t *row0 = (uint16_t *)base->pixels;
    OK(row0[0] == SURF_RGB(255, 0, 0));

    surf_image *ov = surf_image_new(8, 8, SURF_FMT_ARGB8888);
    OK(ov && !ov->opaque);
    /* half-transparent green square */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            *((uint32_t *)((uint8_t *)ov->pixels + y * ov->stride) + x) =
                0x8000ff00u;
    surf_image_blit(base, ov, (surf_rect){0, 0, 8, 8}, 2, 2);
    uint16_t px = *((uint16_t *)((uint8_t *)base->pixels + 4 * base->stride) + 4);
    /* red half-blended with green: both channels present */
    OK(((px >> 11) & 0x1f) > 8 && ((px >> 5) & 0x3f) > 16);
    /* outside the blit untouched */
    OK(row0[0] == SURF_RGB(255, 0, 0));
    /* clipping doesn't crash or write out of bounds */
    surf_image_blit(base, ov, (surf_rect){0, 0, 8, 8}, -4, 28);
    surf_image_blit(base, ov, (surf_rect){0, 0, 8, 8}, 62, -2);
    surf_image_destroy(ov);
    surf_image_destroy(base);
}
