/* hits() reads the INK now: a sprite's transparent pixels do not
 * collide. The mask, the inverse transform (scale, rot, mirror, the
 * filmstrip's frame cell), the invalidation when pixels change, and the
 * box fallbacks that must stay boxes. */
#include "mock_hal.h"

void run_ink_tests(void);

/* An ARGB image, fully transparent except an opaque sub-rect. */
static surf_image *ink_img(int w, int h, surf_rect ink)
{
    surf_image *img = surf_image_new(w, h, SURF_FMT_ARGB8888);
    for (int y = ink.y; y < ink.y + ink.h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)img->pixels +
                                     (size_t)y * img->stride);
        for (int x = ink.x; x < ink.x + ink.w; x++)
            row[x] = 0xffff0000u;
    }
    return img;
}

/* the sword-and-ball report: two sprites whose BOXES overlap only where
 * both are transparent must not collide, and must the moment ink meets */
static void test_ink_sprites(void)
{
    fresh(128, 128, 16);
    surf_image *left = ink_img(32, 32, (surf_rect){0, 0, 8, 32});
    surf_node *a = surf_sprite_new(left, 0, 0);
    surf_node *b = surf_sprite_new(left, 24, 0);
    surf_node_add(surf_screen(), a);
    surf_node_add(surf_screen(), b);
    OK(!surf_node_overlaps(a, b));      /* boxes touch, inks 16px apart */
    surf_node_set_pos(b, 4, 0);
    OK(surf_node_overlaps(a, b));       /* inks share x 4..8 */
    /* a maskless node (a rect) against a sprite: the sprite's side
     * still answers from its pixels */
    surf_node *r = surf_rect_new(20, 4, 8, 8, 0xf800);
    surf_node_add(surf_screen(), r);
    OK(!surf_node_overlaps(a, r));      /* inside a's box, over nothing */
    surf_node_set_pos(r, 2, 4);
    OK(surf_node_overlaps(a, r));
    surf_node_destroy(a);
    surf_node_destroy(b);
    surf_node_destroy(r);
    surf_image_destroy(left);
}

/* no transparency anywhere: the box is still the whole answer, and the
 * per-pixel path is never entered (565 has no alpha to read) */
static void test_ink_solid_is_box(void)
{
    fresh(128, 128, 16);
    surf_image *solid = surf_image_new(32, 32, SURF_FMT_RGB565);
    surf_node *a = surf_sprite_new(solid, 0, 0);
    surf_node *r = surf_rect_new(30, 30, 8, 8, 0xf800);
    surf_node_add(surf_screen(), a);
    surf_node_add(surf_screen(), r);
    OK(surf_node_overlaps(a, r));       /* corner-of-box contact counts */
    surf_node_destroy(a);
    surf_node_destroy(r);
    surf_image_destroy(solid);
}

/* the ink scales with the sprite: what collides is what is drawn */
static void test_ink_scale(void)
{
    fresh(256, 256, 16);
    surf_image *left = ink_img(32, 32, (surf_rect){0, 0, 8, 32});
    surf_node *a = surf_sprite_new(left, 0, 0);
    surf_node_add(surf_screen(), a);
    surf_sprite_set_xform(a, SURF_ONE * 2, 0, 0);    /* ink now 0..16 */
    surf_node *r = surf_rect_new(20, 0, 8, 8, 0xf800);
    surf_node_add(surf_screen(), r);
    OK(!surf_node_overlaps(a, r));      /* past the scaled ink */
    surf_node_set_pos(r, 10, 0);
    OK(surf_node_overlaps(a, r));       /* inside it */
    surf_node_destroy(a);
    surf_node_destroy(r);
    surf_image_destroy(left);
}

/* rot is quarter turns CCW and mirror flips the source first — the SDL
 * hal's mapping, which ink_at replicates. A left-column strip lands at
 * the BOTTOM after one CCW turn, and on the RIGHT under mirror_x. */
static void test_ink_rot_mirror(void)
{
    fresh(128, 128, 16);
    surf_image *left = ink_img(32, 32, (surf_rect){0, 0, 8, 32});
    surf_node *a = surf_sprite_new(left, 0, 0);
    surf_node_add(surf_screen(), a);
    surf_node *r = surf_rect_new(12, 2, 8, 6, 0xf800);   /* top middle */
    surf_node_add(surf_screen(), r);
    OK(!surf_node_overlaps(a, r));
    surf_sprite_set_xform(a, SURF_ONE, 1, 0);
    OK(!surf_node_overlaps(a, r));      /* strip went to the bottom... */
    surf_node_set_pos(r, 12, 26);
    OK(surf_node_overlaps(a, r));       /* ...where it now is */
    surf_sprite_set_xform(a, SURF_ONE, 0, 1);            /* mirror x */
    surf_node_set_pos(r, 2, 12);
    OK(!surf_node_overlaps(a, r));      /* left is empty now */
    surf_node_set_pos(r, 26, 12);
    OK(surf_node_overlaps(a, r));       /* the strip moved right */
    surf_node_destroy(a);
    surf_node_destroy(r);
    surf_image_destroy(left);
}

/* drawing into an image drops its mask, so the next ask sees the new
 * pixels — the lazy cache must never outlive what it describes */
static void test_ink_invalidate(void)
{
    fresh(128, 128, 16);
    surf_image *left = ink_img(32, 32, (surf_rect){0, 0, 8, 32});
    surf_node *a = surf_sprite_new(left, 0, 0);
    surf_node *r = surf_rect_new(20, 4, 8, 8, 0xf800);
    surf_node_add(surf_screen(), a);
    surf_node_add(surf_screen(), r);
    OK(!surf_node_overlaps(a, r));      /* builds the mask */
    surf_image_fill(left, (surf_rect){16, 0, 16, 32}, 0x07e0);
    OK(surf_node_overlaps(a, r));       /* the fill is ink now */
    surf_node_destroy(a);
    surf_node_destroy(r);
    surf_image_destroy(left);
}

/* a filmstrip collides with the FRAME it is showing, not the sheet */
static void test_ink_filmstrip(void)
{
    fresh(128, 128, 16);
    /* two 32x32 cels: frame 0 solid ink, frame 1 transparent */
    surf_image *strip = ink_img(64, 32, (surf_rect){0, 0, 32, 32});
    surf_node *f = surf_filmstrip_new(strip, 32, 32, 0, 0);
    surf_node *r = surf_rect_new(8, 8, 8, 8, 0xf800);
    surf_node_add(surf_screen(), f);
    surf_node_add(surf_screen(), r);
    OK(surf_node_overlaps(f, r));       /* frame 0 is ink */
    surf_filmstrip_set_frame(f, 1);
    OK(!surf_node_overlaps(f, r));      /* frame 1 is empty air */
    surf_node_destroy(f);
    surf_node_destroy(r);
    surf_image_destroy(strip);
}

void run_ink_tests(void)
{
    test_ink_sprites();
    test_ink_solid_is_box();
    test_ink_scale();
    test_ink_rot_mirror();
    test_ink_invalidate();
    test_ink_filmstrip();
}
