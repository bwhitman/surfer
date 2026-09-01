/* Layers: wrap-segment drawing, the streaming band fast path (shift op +
 * sliver + overlay damage), and the stop-motion full repaint. */
#include "mock_hal.h"

void run_layer_tests(void);

static uint16_t strip_px[256 * 32];
static const surf_image strip256 = {
    .pixels = strip_px, .w = 256, .h = 32, .stride = 256 * 2,
    .format = SURF_FMT_RGB565, .opaque = true,
};

/* A hal band-shift drags the pixels of whatever is painted above it. The
 * layer used to repair only its LATER SIBLINGS, so an overlay living in
 * another branch of the tree — tulip's task bar over an app's group —
 * smeared across the screen at the layer's scroll rate. */
static uint16_t world_px[64];  /* pixels unused by the mock */
static const surf_image cam_world = {
    .pixels = world_px, .w = 400, .h = 300, .stride = 800,
    .format = SURF_FMT_RGB565, .opaque = true,
};

/* A hal shift moves REAL framebuffer pixels, so a node that paints
 * nothing must never issue one. The gates used to ask only the shifting
 * node's OWN hidden flag, so a layer animating inside a hidden group —
 * a backgrounded app's — shifted a band it does not own and dragged
 * whatever app IS on screen sideways at its own scroll rate.
 *
 * Both gates on this path need covering, and they need covering IN
 * ORDER: the sub-pixel keep-alive only runs when a shift already ran
 * (`shifted`), so the visible whole-pixel step below is what arms it. */
static void test_layer_hidden_ancestor_never_shifts(void)
{
    fresh(200, 100, 32);
    surf_node *app = surf_group_new(0, 0);
    surf_node_add(surf_screen(), app);
    surf_node *lay = surf_layer_new(&strip256, 0, 0, 200);
    surf_layer_set_fast_scroll(lay, true);
    surf_node_add(app, lay);
    surf_tick();

    nops = 0;
    surf_layer_set_offset(lay, 8 << 16);        /* control: visible shifts */
    OK(nops == 1 && ops[0].op == 'S');
    surf_tick();

    surf_node_set_hidden(app, true);            /* app goes to the back */
    surf_tick();

    nops = 0;
    surf_layer_set_offset(lay, (8 << 16) + 100);  /* sub-pixel keep-alive */
    OK(nops == 0);
    surf_layer_set_offset(lay, 16 << 16);         /* whole-pixel can_fast */
    OK(nops == 0);
    surf_node_destroy(app);
}

/* Same rule, the sprite's copy of it (a camera window over a big opaque
 * image). Its image must be OPAQUE or fast pan is off anyway and the
 * slow path passes the test with the bug in place. */
static void test_sprite_hidden_ancestor_never_pans(void)
{
    fresh(200, 100, 32);
    surf_node *app = surf_group_new(0, 0);
    surf_node_add(surf_screen(), app);
    surf_node *cam = surf_sprite_new(&cam_world, 0, 0);
    surf_sprite_set_src(cam, (surf_rect){50, 50, 200, 100});
    surf_sprite_set_fast_pan(cam, true);
    surf_node_add(app, cam);
    surf_tick();

    nops = 0;
    surf_sprite_set_src(cam, (surf_rect){53, 52, 200, 100});
    OK(nops == 1 && ops[0].op == 'S');          /* control: visible pans */
    surf_tick();

    surf_node_set_hidden(app, true);
    surf_tick();

    nops = 0;
    surf_sprite_set_src(cam, (surf_rect){53, 52, 200, 100});  /* zero shift */
    OK(nops == 0);
    surf_sprite_set_src(cam, (surf_rect){56, 54, 200, 100});  /* real pan */
    OK(nops == 0);
    surf_node_destroy(app);
}

static void test_layer_damages_other_branches(void)
{
    fresh(200, 100, 32);
    surf_node *app = surf_group_new(0, 0);          /* branch 1: the app */
    surf_node_add(surf_screen(), app);
    surf_node *lay = surf_layer_new(&strip256, 0, 0, 200);
    surf_layer_set_fast_scroll(lay, true);
    surf_node_add(app, lay);

    surf_node *chrome = surf_group_new(0, 0);       /* branch 2: the bar */
    surf_node_add(surf_screen(), chrome);
    surf_node *btn = surf_rect_new(150, 5, 40, 20, 0xf800);
    surf_node_add(chrome, btn);
    surf_tick();

    surf_g.dirty.n = 0;
    surf_layer_set_offset(lay, 8 << 16);            /* scroll under it */
    bool covered = false;
    for (int i = 0; i < surf_g.dirty.n; i++) {
        surf_rect r = surf_rect_intersect(surf_g.dirty.r[i],
                                          (surf_rect){150, 5, 40, 20});
        if (r.w >= 40 && r.h >= 20)
            covered = true;                          /* the button repaints */
    }
    OK(covered);
    surf_node_destroy(app);
    surf_node_destroy(chrome);
}

static bool dirty_hits(surf_rect q)
{
    for (int i = 0; i < surf_g.dirty.n; i++)
        if (surf_rect_overlaps(surf_g.dirty.r[i], q))
            return true;
    return false;
}

/* ...but a HIDDEN branch paints nothing, so nothing in it was dragged.
 * The walk gated the flag per node and recursed regardless, and tulip
 * hides an app's GROUP rather than its children — so every child of a
 * backgrounded app passed the test on its own and landed in the dirty
 * list, which degrades to a bounding union past SURF_MAX_DIRTY. */
static void test_layer_skips_hidden_branches(void)
{
    fresh(200, 100, 32);
    surf_node *lay = surf_layer_new(&strip256, 0, 0, 200);
    surf_layer_set_fast_scroll(lay, true);
    surf_node_add(surf_screen(), lay);

    surf_node *bg = surf_group_new(0, 0);           /* a backgrounded app */
    surf_node_add(surf_screen(), bg);
    surf_node *b1 = surf_rect_new(10, 4, 20, 20, 0x0111);
    surf_node *b2 = surf_rect_new(60, 4, 20, 20, 0x0222);
    surf_node_add(bg, b1);
    surf_node_add(bg, b2);
    surf_node_set_hidden(bg, true);                 /* the GROUP, not them */

    surf_node *chrome = surf_rect_new(100, 8, 40, 16, 0xf800);  /* the bar */
    surf_node_add(surf_screen(), chrome);
    surf_tick();

    surf_g.dirty.n = 0;
    surf_layer_set_offset(lay, 4 << 16);
    OK(!dirty_hits((surf_rect){10, 4, 20, 20}));    /* neither hidden child */
    OK(!dirty_hits((surf_rect){60, 4, 20, 20}));    /* reaches the list */
    OK(dirty_hits((surf_rect){100, 8, 40, 16}));    /* the bar still does */
    surf_node_destroy(bg);
    surf_node_destroy(chrome);
    surf_node_destroy(lay);
}

void run_layer_tests(void)
{
    test_layer_damages_other_branches();
    test_layer_skips_hidden_branches();
    test_layer_hidden_ancestor_never_shifts();
    test_sprite_hidden_ancestor_never_pans();
    fresh(200, 100, 16);
    surf_node *l = surf_layer_new(&strip256, 0, 10, 200);
    surf_node_add(surf_screen(), l);
    OK(surf_node_size(l).x == 200 && surf_node_size(l).y == 32);
    surf_tick();

    /* off 100: vis 200 wide needs cols 100..255 then 0..43 → 2 blits */
    nops = 0;
    surf_layer_set_offset(l, 100 << 16);  /* slow path: no fast flag */
    surf_tick();
    int blits = 0;
    surf_rect seg0 = {0, 0, 0, 0}, seg1 = {0, 0, 0, 0};
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'B') {
            if (blits == 0) seg0 = ops[i].src;
            if (blits == 1) seg1 = ops[i].src;
            blits++;
        }
    OK(blits == 2);
    OK(seg0.x == 100 && seg0.w == 156);
    OK(seg1.x == 0 && seg1.w == 44);

    /* fast path: +3px → band_shift(-3), one sliver at the right —
     * even-aligned by surf_dirty_add to 4px starting at 196 */
    surf_layer_set_fast_scroll(l, true);
    surf_tick();
    nops = 0;
    surf_layer_set_offset(l, 103 << 16);
    surf_tick();
    bool saw_shift = false, saw_sliver = false;
    for (int i = 0; i < nops; i++) {
        if (ops[i].op == 'S') {
            saw_shift = true;
            OK(rect_eq(ops[i].r, (surf_rect){0, 10, 200, 32}));
            OK(ops[i].dst.x == -3 && ops[i].dst.y == 0);
        }
        if (ops[i].op == 'B' && ops[i].dst.x == 196 && ops[i].src.w == 4)
            saw_sliver = true;
    }
    OK(saw_shift && saw_sliver);

    /* sub-pixel move after a shift: the stream stays alive with a ZERO
     * band shift — no full repaint (a crawling layer must not pay a
     * repaint per pixel) */
    nops = 0;
    surf_layer_set_offset(l, (103 << 16) + 100);
    surf_tick();
    bool zero_shift = false, full_repaint = false;
    for (int i = 0; i < nops; i++) {
        if (ops[i].op == 'S' && ops[i].dst.x == 0 && ops[i].dst.y == 0)
            zero_shift = true;
        if (ops[i].op == 'B' && ops[i].dst.x == 0 && ops[i].src.w >= 150)
            full_repaint = true;
    }
    OK(zero_shift && !full_repaint);

    /* overlay sibling gets damaged (expanded) when the band shifts */
    surf_node *ship = surf_rect_new(50, 20, 20, 10, 0x1234);
    surf_node_add(surf_screen(), ship);
    surf_tick();
    nops = 0;
    surf_layer_set_offset(l, 108 << 16);
    surf_tick();
    bool ship_redrawn = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'F' && ops[i].c == 0x1234)
            ship_redrawn = true;
    OK(ship_redrawn);

    /* wrap: offsets normalize into [0, strip_w) */
    surf_layer_set_offset(l, -(40 << 16));
    OK(surf_layer_offset(l) == (256 - 40) << 16);

    surf_node_destroy(ship);
    surf_node_destroy(l);

    /* ---- sprite fast pan (camera window over a big image) ---- */
    fresh(200, 100, 16);
    static uint16_t world_px[64];  /* pixels unused by the mock */
    static const surf_image world = {
        .pixels = world_px, .w = 400, .h = 300, .stride = 800,
        .format = SURF_FMT_RGB565, .opaque = true,
    };
    surf_node *cam = surf_sprite_new(&world, 0, 0);
    surf_sprite_set_src(cam, (surf_rect){50, 50, 200, 100});
    surf_sprite_set_fast_pan(cam, true);
    surf_node_add(surf_screen(), cam);
    surf_node *hero = surf_rect_new(90, 40, 20, 20, 0x4321);
    surf_node_add(surf_screen(), hero);
    surf_tick();

    nops = 0;
    surf_sprite_set_src(cam, (surf_rect){53, 52, 200, 100});
    surf_tick();
    bool pan_shift = false, svert = false, shorz = false, hero_redraw = false;
    for (int i = 0; i < nops; i++) {
        if (ops[i].op == 'S' && ops[i].dst.x == -3 && ops[i].dst.y == -2)
            pan_shift = true;
        /* the L's slivers arrive even-aligned (surf_dirty_add): the
         * vertical one is 4 wide from 196, the horizontal one stops at
         * its edge — and the two must NOT have merged into one
         * full-band blit, which is what an unaligned odd dx would do */
        if (ops[i].op == 'B' && ops[i].dst.x == 196 && ops[i].src.w == 4)
            svert = true;
        if (ops[i].op == 'B' && ops[i].dst.y == 98 && ops[i].src.h == 2 &&
            ops[i].src.w == 196)
            shorz = true;
        if (ops[i].op == 'F' && ops[i].c == 0x4321)
            hero_redraw = true;
    }
    OK(pan_shift && svert && shorz);
    OK(hero_redraw);

    /* same-value call after a shift: zero shift keeps streaming; the
     * heal repaint only runs when streaming can't continue */
    nops = 0;
    surf_sprite_set_src(cam, (surf_rect){53, 52, 200, 100});
    surf_tick();
    bool zero = false, full = false;
    for (int i = 0; i < nops; i++) {
        if (ops[i].op == 'S' && ops[i].dst.x == 0 && ops[i].dst.y == 0)
            zero = true;
        if (ops[i].op == 'B' && ops[i].src.w == 200 && ops[i].src.h == 100)
            full = true;
    }
    OK(zero && !full);
    surf_sprite_set_fast_pan(cam, false);       /* streaming off... */
    surf_sprite_set_src(cam, (surf_rect){53, 52, 200, 100});
    surf_tick();                                /* ...now it heals */
    full = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'B' && ops[i].src.w == 200 && ops[i].src.h == 100)
            full = true;
    OK(full);

    surf_node_destroy(hero);
    surf_node_destroy(cam);
}
