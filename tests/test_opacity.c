/* Node opacity: the alpha multiplier reaching the hal, and the three
 * things it has to TURN OFF. Every one of those is a correctness bug
 * that leaves a plausible-looking picture, which is why they are here
 * rather than left to the eye:
 *
 *   - the occlusion early-out (a faded node covers nothing)
 *   - band_shift streaming (a shifted band is already composited)
 *   - the 9-patch's solid-centre fill (hal->fill has no opacity)
 */
#include "mock_hal.h"

void run_opacity_tests(void);
extern surf_font tfont;   /* built by test_text's setup; main runs it first */

static uint16_t opq_px[64 * 64];
static const surf_image opq = {
    .pixels = opq_px, .w = 64, .h = 64, .stride = 64 * 2,
    .format = SURF_FMT_RGB565, .opaque = true,
};
static uint32_t argb_px[64 * 64];
static const surf_image argb = {
    .pixels = argb_px, .w = 64, .h = 64, .stride = 64 * 4,
    .format = SURF_FMT_ARGB8888, .opaque = false,
};
static uint16_t strip_px[256 * 32];
static const surf_image strip256 = {
    .pixels = strip_px, .w = 256, .h = 32, .stride = 256 * 2,
    .format = SURF_FMT_RGB565, .opaque = true,
};

static int count_op(char op)
{
    int n = 0;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == op)
            n++;
    return n;
}
static const mock_op *first_op(char op)
{
    for (int i = 0; i < nops; i++)
        if (ops[i].op == op)
            return &ops[i];
    return NULL;
}

/* Which node types accept one, and that the refusal is a REFUSAL rather
 * than a silent no-op — the .rot-on-a-group lesson. */
static void test_which_nodes(void)
{
    fresh(200, 100, 32);
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node *grp = surf_group_new(0, 0);
    surf_node *rct = surf_rect_new(0, 0, 10, 10, 0x1234);
    surf_node *lay = surf_layer_new(&strip256, 0, 0, 200);
    surf_node *nine = surf_ninepatch_new(&argb, 0, 0, 40, 40, 8, 8, 8, 8);

    OK(surf_node_can_fade(spr));
    OK(surf_node_can_fade(lay));
    OK(surf_node_can_fade(nine));
    OK(!surf_node_can_fade(grp));
    OK(!surf_node_can_fade(rct));
    OK(!surf_node_can_fade(NULL));

    OK(surf_node_set_opacity(spr, 128));
    OK(surf_node_opacity(spr) == 128);
    /* refused, and the node is untouched */
    OK(!surf_node_set_opacity(grp, 128));
    OK(surf_node_opacity(grp) == 255);
    OK(!surf_node_set_opacity(rct, 0));
    OK(surf_node_opacity(rct) == 255);
    /* default is opaque for everything */
    OK(surf_node_opacity(lay) == 255);
    OK(surf_node_opacity(nine) == 255);

    surf_node_destroy(spr); surf_node_destroy(grp); surf_node_destroy(rct);
    surf_node_destroy(lay); surf_node_destroy(nine);
}

/* The value reaches the hal, and an OPAQUE image stops taking the blit
 * path — a blit has nowhere to put an opacity, so a faded opaque sprite
 * drawn with one is a sprite that never fades. */
static void test_reaches_hal(void)
{
    fresh(200, 100, 32);
    surf_node *spr = surf_sprite_new(&opq, 10, 10);
    surf_node_add(surf_screen(), spr);
    surf_tick();

    nops = 0;
    surf_node_set_opacity(spr, 64);
    surf_tick();
    OK(count_op('B') == 0);            /* not blitted */
    const mock_op *a = first_op('A');
    OK(a != NULL);
    OK(a && a->opa == 64);

    /* back to 255 and the blit path returns */
    nops = 0;
    surf_node_set_opacity(spr, 255);
    surf_tick();
    OK(count_op('B') == 1);
    OK(count_op('A') == 0);
    surf_node_destroy(spr);
}

/* A TRANSFORMED sprite fades too. This is the case the hal signature
 * change exists for: xform_blend had no opacity, so scaling a faded
 * sprite silently drew it at full strength. */
static void test_transformed(void)
{
    fresh(200, 100, 32);
    surf_node *spr = surf_sprite_new(&argb, 10, 10);
    surf_node_add(surf_screen(), spr);
    surf_sprite_set_xform(spr, SURF_ONE * 2, 0, 0);
    surf_node_set_opacity(spr, 100);
    nops = 0;
    surf_tick();
    const mock_op *x = first_op('X');
    OK(x != NULL);
    OK(x && x->opa == 100);
    surf_node_destroy(spr);
}

/* THE OCCLUSION EARLY-OUT. A full-screen opaque sprite normally stops
 * the walk, so nothing under it is painted. Faded, it must not — and the
 * symptom of getting this wrong is not a wrong colour, it is a hole
 * showing whatever the framebuffer last held. */
static void test_occlusion(void)
{
    fresh(64, 64, 32);
    surf_node *under = surf_rect_new(0, 0, 64, 64, 0x1111);
    surf_node_add(surf_screen(), under);
    surf_node *over = surf_sprite_new(&opq, 0, 0);   /* covers the screen */
    surf_node_add(surf_screen(), over);

    nops = 0;
    surf_tick();
    OK(count_op('F') == 0);            /* the rect under is occluded */

    nops = 0;
    surf_node_set_opacity(over, 200);
    surf_tick();
    OK(count_op('F') == 1);            /* ...and now it is painted */
    OK(count_op('A') == 1);

    surf_node_destroy(under); surf_node_destroy(over);
}

/* BAND SHIFT. A shift moves pixels that are already this node composited
 * over what was behind it; blending the node again over its own result
 * blends twice. So a faded fast-pan sprite must repaint, not stream. */
static void test_no_band_shift(void)
{
    fresh(200, 100, 32);
    surf_node *cam = surf_sprite_new(&opq, 0, 0);
    surf_sprite_set_src(cam, (surf_rect){0, 0, 32, 32});
    surf_sprite_set_fast_pan(cam, true);
    surf_node_add(surf_screen(), cam);
    surf_tick();

    /* opaque: a 1px pan streams */
    nops = 0;
    surf_sprite_set_src(cam, (surf_rect){1, 0, 32, 32});
    OK(count_op('S') == 1);

    /* faded: the same pan repaints instead */
    surf_node_set_opacity(cam, 128);
    surf_tick();
    nops = 0;
    surf_sprite_set_src(cam, (surf_rect){2, 0, 32, 32});
    OK(count_op('S') == 0);
    surf_node_destroy(cam);

    /* ...and the same for a layer */
    fresh(200, 100, 32);
    surf_node *lay = surf_layer_new(&strip256, 0, 0, 200);
    surf_layer_set_fast_scroll(lay, true);
    surf_node_add(surf_screen(), lay);
    surf_tick();
    nops = 0;
    surf_layer_set_offset(lay, SURF_ONE * 4);
    OK(count_op('S') == 1);

    surf_node_set_opacity(lay, 128);
    surf_tick();
    nops = 0;
    surf_layer_set_offset(lay, SURF_ONE * 8);
    OK(count_op('S') == 0);
    surf_node_destroy(lay);
}

/* THE 9-PATCH SOLID CENTRE. region_is_solid only says yes to a fully
 * opaque block, so the one-fill shortcut would paint that colour at full
 * strength straight through the fade. Faded, it has to tile. */
static void test_ninepatch_solid_centre(void)
{
    static uint32_t px[32 * 32];
    for (int i = 0; i < 32 * 32; i++)
        px[i] = 0xff204060;                       /* opaque, one colour */
    surf_image flat = {.pixels = px, .w = 32, .h = 32, .stride = 32 * 4,
                       .format = SURF_FMT_ARGB8888, .opaque = true};
    fresh(200, 100, 32);
    surf_node *n = surf_ninepatch_new(&flat, 0, 0, 100, 40, 8, 8, 8, 8);
    surf_node_add(surf_screen(), n);

    nops = 0;
    surf_tick();
    /* opaque: the centre band is ONE fill in the source's colour */
    surf_color centre = 0;
    int centre_fills = 0;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'F' && ops[i].c != surf_g.bg) {
            centre = ops[i].c;
            centre_fills++;
        }
    OK(centre_fills > 0);

    nops = 0;
    surf_node_set_opacity(n, 128);
    surf_tick();
    /* faded: that fill is gone — the centre is tiled through the blend,
     * carrying the opacity like every other part of the patch */
    for (int i = 0; i < nops; i++)
        OK(ops[i].op != 'F' || ops[i].c != centre);
    OK(count_op('A') > 0);
    for (int i = 0; i < nops; i++)
        OK(ops[i].op != 'A' || ops[i].opa == 128);
    surf_node_destroy(n);
}

/* Opacity is VISUAL. A fade must not change what a finger or a collision
 * does halfway through — `hidden` is the thing that takes a node out of
 * both, and it still does. */
static void test_visual_only(void)
{
    fresh(200, 100, 32);
    surf_node *a = surf_sprite_new(&opq, 0, 0);
    surf_node *b = surf_sprite_new(&opq, 10, 10);
    surf_node_add(surf_screen(), a);
    surf_node_add(surf_screen(), b);
    surf_tick();

    OK(surf_hit_test(20, 20) == b);
    OK(surf_node_overlaps(a, b));

    surf_node_set_opacity(a, 0);
    surf_node_set_opacity(b, 0);
    OK(surf_hit_test(20, 20) == b);    /* invisible, still there */
    OK(surf_node_overlaps(a, b));

    surf_node_set_hidden(b, true);
    OK(surf_hit_test(20, 20) == a);
    OK(!surf_node_overlaps(a, b));
    surf_node_destroy(a); surf_node_destroy(b);
}

/* A label fades: its glyphs are A8 blends, so this is the same parameter
 * one layer down. */
static void test_label(void)
{
    fresh(200, 100, 32);
    surf_node *l = surf_text_new(&tfont, "AB", 4, 4, SURF_RGB(255, 255, 255));
    surf_node_add(surf_screen(), l);
    OK(surf_node_can_fade(l));
    OK(surf_node_set_opacity(l, 90));
    nops = 0;
    surf_tick();
    int seen = 0;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'A') {
            OK(ops[i].opa == 90);
            seen++;
        }
    OK(seen > 0);
    surf_node_destroy(l);
}

/* opacity 0 paints NOTHING — it never reaches the hal at all. Both
 * halves matter: the op is wasted work, and the P4's PPA documents its
 * alpha scale ratio as exclusive of zero, so an op that cannot draw
 * anything is one the device may reject and the desktop will not. */
static void test_zero_paints_nothing(void)
{
    fresh(64, 64, 32);
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);
    surf_node_set_opacity(spr, 0);
    nops = 0;
    surf_tick();
    OK(count_op('A') == 0);
    OK(count_op('B') == 0);
    OK(count_op('X') == 0);
    OK(count_op('F') == 1);            /* just the background clear */
    surf_node_destroy(spr);
}

/* ---- fades: opacity over time ---- */

/* The tween interpolates, lands EXACTLY on the target, and then stops
 * being work. mock_advance_us drives the clock, so this is not a timing
 * test — it is arithmetic with a clock the test owns. */
static void test_fade_runs(void)
{
    fresh(64, 64, 32);
    mock_now_us_val = 0;
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);

    OK(surf_node_fade_to(spr, 0, 100));      /* 255 -> 0 over 100 ms */
    OK(surf_node_fading(spr));
    OK(surf_node_opacity(spr) == 255);       /* not moved until a tick */

    mock_advance_us(50000);                  /* half way */
    surf_tick();
    uint8_t mid = surf_node_opacity(spr);
    OK(mid > 110 && mid < 145);              /* ~127 */
    OK(surf_node_fading(spr));

    mock_advance_us(60000);                  /* past the end */
    surf_tick();
    OK(surf_node_opacity(spr) == 0);         /* EXACTLY the target */
    OK(!surf_node_fading(spr));

    /* and it stays there — a finished fade is not still writing */
    mock_advance_us(100000);
    surf_tick();
    OK(surf_node_opacity(spr) == 0);
    surf_node_destroy(spr);
}

/* A direct write WINS. Without the cancel the tween overwrites it on the
 * next tick and `opacity = 1` in mid-fade silently does nothing. */
static void test_direct_write_cancels(void)
{
    fresh(64, 64, 32);
    mock_now_us_val = 0;
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);
    surf_node_fade_to(spr, 0, 100);
    mock_advance_us(20000);
    surf_tick();
    OK(surf_node_fading(spr));

    surf_node_set_opacity(spr, 255);
    OK(!surf_node_fading(spr));
    mock_advance_us(50000);
    surf_tick();
    OK(surf_node_opacity(spr) == 255);       /* the tween did not resume */
    surf_node_destroy(spr);
}

/* A second fade REPLACES the first — including a reversal mid-flight,
 * which is what a fade-in interrupting a fade-out is. It must start from
 * where the node actually is, not from where the old fade began. */
static void test_replace_and_reverse(void)
{
    fresh(64, 64, 32);
    mock_now_us_val = 0;
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);

    surf_node_fade_to(spr, 0, 100);
    mock_advance_us(50000);
    surf_tick();
    uint8_t at = surf_node_opacity(spr);
    OK(at > 110 && at < 145);

    surf_node_fade_to(spr, 255, 100);        /* reverse from HERE */
    mock_advance_us(50000);
    surf_tick();
    uint8_t back = surf_node_opacity(spr);
    OK(back > at);                            /* going up now */
    OK(back < 255);                           /* ...and not there yet */
    mock_advance_us(60000);
    surf_tick();
    OK(surf_node_opacity(spr) == 255);
    OK(!surf_node_fading(spr));
    surf_node_destroy(spr);
}

/* A DESTROYED NODE MUST NOT LEAVE A TWEEN. The slot holds a raw pointer
 * into the node pool, so a fade outliving its node writes through freed
 * memory every frame — and the pool RECYCLES, so the write lands on
 * whatever node was allocated next. Silent, and it corrupts a stranger. */
static void test_destroy_cancels(void)
{
    fresh(64, 64, 32);
    mock_now_us_val = 0;
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);
    surf_node_fade_to(spr, 0, 100);
    OK(surf_node_fading(spr));
    surf_node_destroy(spr);

    /* the slot is free again, so a fresh node gets one and is untouched
     * by the dead fade */
    surf_node *other = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), other);
    OK(!surf_node_fading(other));
    mock_advance_us(200000);
    surf_tick();
    OK(surf_node_opacity(other) == 255);
    surf_node_destroy(other);
}

/* Refusals and the degenerate durations. */
static void test_fade_edges(void)
{
    fresh(64, 64, 32);
    mock_now_us_val = 0;
    surf_node *grp = surf_group_new(0, 0);
    OK(!surf_node_fade_to(grp, 0, 100));     /* a group cannot fade */
    OK(!surf_node_fading(grp));
    surf_node_destroy(grp);

    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);
    /* ms <= 0 lands at once rather than refusing */
    OK(surf_node_fade_to(spr, 40, 0));
    OK(surf_node_opacity(spr) == 40);
    OK(!surf_node_fading(spr));
    /* already there: nothing to animate */
    OK(surf_node_fade_to(spr, 40, 100));
    OK(!surf_node_fading(spr));
    /* cancel leaves the value where the fade got to */
    surf_node_fade_to(spr, 255, 100);
    mock_advance_us(50000);
    surf_tick();
    uint8_t at = surf_node_opacity(spr);
    surf_node_fade_cancel(spr);
    OK(!surf_node_fading(spr));
    mock_advance_us(100000);
    surf_tick();
    OK(surf_node_opacity(spr) == at);
    surf_node_destroy(spr);
}

/* The table is finite, and a full one must still END UP RIGHT: the fade
 * is instant rather than dropped. A caller that asked to arrive at 0
 * arrives at 0 either way. */
static void test_table_full(void)
{
    fresh(64, 64, SURF_MAX_TWEENS + 8);
    mock_now_us_val = 0;
    surf_node *held[SURF_MAX_TWEENS];
    for (int i = 0; i < SURF_MAX_TWEENS; i++) {
        held[i] = surf_sprite_new(&argb, 0, 0);
        surf_node_add(surf_screen(), held[i]);
        OK(surf_node_fade_to(held[i], 0, 1000));
    }
    surf_node *extra = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), extra);
    OK(surf_node_fade_to(extra, 0, 1000));
    OK(!surf_node_fading(extra));            /* no slot... */
    OK(surf_node_opacity(extra) == 0);       /* ...but it arrived */

    for (int i = 0; i < SURF_MAX_TWEENS; i++)
        surf_node_destroy(held[i]);
    surf_node_destroy(extra);
}

/* Costs nothing when nothing is fading — the tick is gated on a counter,
 * the way the filmstrip scan is, and the counter must come back to zero.
 * A leak here is a whole-pool scan every frame for the rest of the
 * session, which is exactly the bug node_free's comment describes. */
static void test_counter_returns_to_zero(void)
{
    fresh(64, 64, 32);
    mock_now_us_val = 0;
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);
    for (int round = 0; round < 5; round++) {
        surf_node_fade_to(spr, round & 1 ? 255 : 0, 50);
        mock_advance_us(60000);
        surf_tick();
    }
    OK(surf_g.ntweens == 0);
    surf_node_fade_to(spr, 128, 50);
    OK(surf_g.ntweens == 1);
    surf_node_destroy(spr);
    OK(surf_g.ntweens == 0);
}

/* ---- tweens: any property, and SEVERAL AT ONCE ---- */

/* THE POINT OF KEYING BY (node, prop). A dying enemy drifts up AND
 * fades; keyed by node alone the second would replace the first, and
 * the bug is a sprite that fades perfectly and never moves. */
static void test_two_at_once(void)
{
    fresh(200, 200, 32);
    mock_now_us_val = 0;
    surf_node *spr = surf_sprite_new(&argb, 10, 100);
    surf_node_add(surf_screen(), spr);

    OK(surf_node_tween(spr, SURF_TW_Y, 20 << 16, 100, SURF_EASE_LINEAR));
    OK(surf_node_tween(spr, SURF_TW_OPACITY, 0, 100, SURF_EASE_LINEAR));
    OK(surf_node_tweening(spr, SURF_TW_Y));
    OK(surf_node_tweening(spr, SURF_TW_OPACITY));
    OK(surf_node_tweening(spr, -1));          /* any */

    mock_advance_us(50000);
    surf_tick();
    OK(surf_node_pos(spr).y < 100 && surf_node_pos(spr).y > 20);
    uint8_t mid = surf_node_opacity(spr);
    OK(mid > 110 && mid < 145);

    mock_advance_us(60000);
    surf_tick();
    OK(surf_node_pos(spr).y == 20);           /* both land exactly */
    OK(surf_node_opacity(spr) == 0);
    OK(!surf_node_tweening(spr, -1));
    surf_node_destroy(spr);
}

static void test_position_tween(void)
{
    fresh(200, 200, 32);
    mock_now_us_val = 0;
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);
    OK(surf_node_tween(spr, SURF_TW_X, 100 << 16, 100, SURF_EASE_LINEAR));
    mock_advance_us(50000);
    surf_tick();
    int16_t at = surf_node_pos(spr).x;
    OK(at > 40 && at < 60);                   /* ~50 */
    mock_advance_us(60000);
    surf_tick();
    OK(surf_node_pos(spr).x == 100);
    /* a GROUP has a position, so it may tween one — unlike opacity */
    surf_node *g = surf_group_new(0, 0);
    surf_node_add(surf_screen(), g);
    OK(surf_node_tween(g, SURF_TW_X, 50 << 16, 100, SURF_EASE_LINEAR));
    surf_node_destroy(g);
    surf_node_destroy(spr);
}

static void test_scale_tween(void)
{
    fresh(200, 200, 32);
    mock_now_us_val = 0;
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);
    OK(surf_node_tween(spr, SURF_TW_SCALE, SURF_ONE * 2, 100,
                       SURF_EASE_LINEAR));
    mock_advance_us(50000);
    surf_tick();
    int32_t at = surf_sprite_scale(spr);
    OK(at > SURF_ONE && at < SURF_ONE * 2);
    mock_advance_us(60000);
    surf_tick();
    OK(surf_sprite_scale(spr) == SURF_ONE * 2);
    /* a group cannot scale, so it cannot tween one */
    surf_node *g = surf_group_new(0, 0);
    OK(!surf_node_tween(g, SURF_TW_SCALE, SURF_ONE * 2, 100, 0));
    OK(!surf_node_tween(g, SURF_TW_OPACITY, 0, 100, 0));
    surf_node_destroy(g);
    surf_node_destroy(spr);
}

/* A DIRECT WRITE WINS, per property. set_pos kills an X or Y tween and
 * leaves a fade alone — the two are independent now, and a cancel that
 * took everything would stop a fade because something moved. */
static void test_direct_write_per_property(void)
{
    fresh(200, 200, 32);
    mock_now_us_val = 0;
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);
    surf_node_tween(spr, SURF_TW_X, 100 << 16, 200, SURF_EASE_LINEAR);
    surf_node_tween(spr, SURF_TW_OPACITY, 0, 200, SURF_EASE_LINEAR);
    mock_advance_us(20000);
    surf_tick();

    surf_node_set_pos(spr, 7, 7);
    OK(!surf_node_tweening(spr, SURF_TW_X));
    OK(surf_node_tweening(spr, SURF_TW_OPACITY));   /* untouched */
    mock_advance_us(50000);
    surf_tick();
    OK(surf_node_pos(spr).x == 7);                  /* did not resume */

    surf_sprite_set_xform(spr, SURF_ONE, 0, 0);
    OK(surf_node_tweening(spr, SURF_TW_OPACITY));   /* still untouched */
    surf_node_destroy(spr);
}

/* Easing bends the curve and STILL lands exactly. Out is ahead of
 * linear at the half way point, in is behind it. */
static void test_easing(void)
{
    fresh(200, 200, 32);
    int16_t at[4];
    for (int e = 0; e < 4; e++) {
        mock_now_us_val = 0;
        surf_node *spr = surf_sprite_new(&argb, 0, 0);
        surf_node_add(surf_screen(), spr);
        surf_node_tween(spr, SURF_TW_X, 1000 << 16, 100, (uint8_t)e);
        mock_advance_us(50000);
        surf_tick();
        at[e] = surf_node_pos(spr).x;
        mock_advance_us(60000);
        surf_tick();
        OK(surf_node_pos(spr).x == 1000);        /* every curve lands */
        surf_node_destroy(spr);
    }
    OK(at[SURF_EASE_IN] < at[SURF_EASE_LINEAR]);
    OK(at[SURF_EASE_OUT] > at[SURF_EASE_LINEAR]);
    /* in-out passes through the middle like linear does */
    OK(at[SURF_EASE_IN_OUT] > 450 && at[SURF_EASE_IN_OUT] < 550);
}

/* Destroying a node drops EVERY tween on it, not just one — the slots
 * hold raw pointers into a pool that RECYCLES. */
static void test_destroy_drops_all(void)
{
    fresh(200, 200, 32);
    mock_now_us_val = 0;
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);
    surf_node_tween(spr, SURF_TW_X, 100 << 16, 500, 0);
    surf_node_tween(spr, SURF_TW_Y, 100 << 16, 500, 0);
    surf_node_tween(spr, SURF_TW_OPACITY, 0, 500, 0);
    OK(surf_g.ntweens == 3);
    surf_node_destroy(spr);
    OK(surf_g.ntweens == 0);
}

/* There is no ROT tween, and that is the PPA rather than an omission:
 * it turns in quarter turns, so a smooth rotate could only ever be a
 * four-frame flip-book. Refused rather than served badly. */
static void test_no_rot_tween(void)
{
    fresh(200, 200, 32);
    surf_node *spr = surf_sprite_new(&argb, 0, 0);
    surf_node_add(surf_screen(), spr);
    OK(!surf_node_tween(spr, SURF_TW_NPROPS, 0, 100, 0));
    OK(!surf_node_tween(spr, 99, 0, 100, 0));
    surf_node_destroy(spr);
}

void run_opacity_tests(void)
{
    test_two_at_once();
    test_position_tween();
    test_scale_tween();
    test_direct_write_per_property();
    test_easing();
    test_destroy_drops_all();
    test_no_rot_tween();
    test_zero_paints_nothing();
    test_fade_runs();
    test_direct_write_cancels();
    test_replace_and_reverse();
    test_destroy_cancels();
    test_fade_edges();
    test_table_full();
    test_counter_returns_to_zero();
    test_which_nodes();
    test_reaches_hal();
    test_transformed();
    test_occlusion();
    test_no_band_shift();
    test_ninepatch_solid_centre();
    test_visual_only();
    test_label();
}
