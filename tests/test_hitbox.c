/* Hitboxes: caller-authored collision rects on a sprite or filmstrip.
 * The mapping through the transform is the FORWARD form of ink_at's
 * inverse, so what the tests pin down is that the two agree about what
 * a quarter turn means — plus the property the whole feature exists
 * for: a box authored on the head stays on the head through every
 * mirror and turn, with no app arithmetic. */
#include "mock_hal.h"

void run_hitbox_tests(void);

static surf_image *solid_img(int w, int h)
{
    surf_image *img = surf_image_new(w, h, SURF_FMT_ARGB8888);
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)img->pixels +
                                     (size_t)y * img->stride);
        for (int x = 0; x < w; x++)
            row[x] = 0xffff0000u;
    }
    return img;
}

/* transparent except an opaque sub-rect — the ink the boxes must meet */
static surf_image *ink_only(int w, int h, surf_rect ink)
{
    surf_image *img = surf_image_new(w, h, SURF_FMT_ARGB8888);
    for (int y = ink.y; y < ink.y + ink.h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)img->pixels +
                                     (size_t)y * img->stride);
        for (int x = ink.x; x < ink.x + ink.w; x++)
            row[x] = 0xff00ff00u;
    }
    return img;
}

static void test_hitbox_store(void)
{
    fresh(256, 256, 16);
    surf_image *img = solid_img(32, 32);
    surf_node *s = surf_sprite_new(img, 0, 0);
    surf_node_add(surf_screen(), s);
    OK(surf_hitbox_count(s) == 0);
    OK(surf_hitbox_add(s, 0, 8, 16, 16) == 0);
    OK(surf_hitbox_add(s, -8, 0, 8, 8) == 1);   /* negative offset is legal */
    OK(surf_hitbox_count(s) == 2);
    surf_hitbox b;
    OK(surf_hitbox_get(s, 1, &b) && b.x == -8 && b.w == 8);
    OK(surf_hitbox_set(s, 1, 40, 0, 8, 8));     /* past w is legal too */
    OK(surf_hitbox_get(s, 1, &b) && b.x == 40);
    OK(!surf_hitbox_set(s, 1, 0, 0, 0, 8));     /* empty box refused */
    OK(surf_hitbox_remove(s, 0));
    OK(surf_hitbox_count(s) == 1);
    OK(surf_hitbox_get(s, 0, &b) && b.x == 40); /* shifted down */
    OK(!surf_hitbox_get(s, 1, &b));
    /* not on a node without a transform */
    surf_node *g = surf_group_new(0, 0);
    surf_node_add(surf_screen(), g);
    OK(surf_hitbox_add(g, 0, 0, 8, 8) == -1);
    surf_node_destroy(s);
    surf_image_destroy(img);
}

/* the head box rides the transform: authored top-left, it lands where
 * the drawn head lands under every mirror and quarter turn */
static void test_hitbox_abs(void)
{
    fresh(256, 256, 16);
    surf_image *img = solid_img(32, 32);
    surf_node *s = surf_sprite_new(img, 100, 100);
    surf_node_add(surf_screen(), s);
    int32_t i = surf_hitbox_add(s, 0, 8, 8, 8);   /* left edge, upper */
    surf_rect r;
    OK(surf_hitbox_abs(s, i, &r) &&
       r.x == 100 && r.y == 108 && r.w == 8 && r.h == 8);
    surf_sprite_set_xform(s, SURF_ONE, 0, 1);     /* mirror_x: -> right */
    OK(surf_hitbox_abs(s, i, &r) && r.x == 124 && r.y == 108);
    /* rot 1 is a quarter turn CCW of the ink_at convention: the LEFT
     * edge lands at the BOTTOM (the framebuffer probe's answer) */
    surf_sprite_set_xform(s, SURF_ONE, 1, 0);
    OK(surf_hitbox_abs(s, i, &r) &&
       r.x == 108 && r.y == 124 && r.w == 8 && r.h == 8);
    surf_sprite_set_xform(s, SURF_ONE, 3, 0);     /* ...and CW: the top */
    OK(surf_hitbox_abs(s, i, &r) && r.x == 116 && r.y == 100);
    surf_sprite_set_xform(s, SURF_ONE * 2, 0, 0); /* 2x scale: all doubles */
    OK(surf_hitbox_abs(s, i, &r) &&
       r.x == 100 && r.y == 116 && r.w == 16 && r.h == 16);
    surf_node_destroy(s);
    surf_image_destroy(img);
}

static void test_hitbox_overlaps(void)
{
    fresh(256, 256, 16);
    surf_image *body = solid_img(32, 32);
    surf_image *food = ink_only(32, 32, (surf_rect){0, 0, 16, 32});
    surf_node *s = surf_sprite_new(body, 100, 100);
    surf_node *e = surf_sprite_new(food, 0, 0);
    surf_node_add(surf_screen(), s);
    surf_node_add(surf_screen(), e);

    /* one box on the sprite's right edge, one hanging OFF the left */
    int32_t head = surf_hitbox_add(s, 24, 8, 8, 8);
    int32_t reach = surf_hitbox_add(s, -16, 8, 8, 8);

    /* food's ink under the head box */
    surf_node_set_pos(e, 120, 100);
    OK(surf_node_overlaps(s, e));
    OK(surf_node_overlaps_which(s, e) == (1u << head));
    OK(surf_node_overlaps(e, s));               /* symmetric */

    /* food's box under the head box, but only its TRANSPARENT half */
    surf_node_set_pos(e, 132, 100);
    OK(!surf_node_overlaps(s, e));

    /* the box hanging off the sprite collides where the NODE box never
     * could — this is the early-out the hitbox path must not take */
    surf_node_set_pos(e, 70, 100);
    OK(surf_node_overlaps_which(s, e) == (1u << reach));

    /* hitboxes replace the sprite's own box: food dead centre on the
     * sprite's BODY, under neither box, is a miss */
    surf_node_set_pos(e, 104, 100);
    OK(surf_hitbox_count(s) == 2 && !surf_node_overlaps(s, e));

    /* both sides with boxes: box-vs-box, transformed */
    surf_hitbox_add(e, 0, 0, 8, 8);
    surf_node_set_pos(e, 126, 108);             /* e's box under s's head */
    OK(surf_node_overlaps(s, e));
    surf_node_set_pos(e, 104, 100);
    OK(!surf_node_overlaps(s, e));

    /* hidden never collides, hitboxes or not */
    surf_node_set_pos(e, 126, 108);
    surf_node_set_hidden(e, true);
    OK(!surf_node_overlaps(s, e) && surf_node_overlaps_which(s, e) == 0);
    surf_node_set_hidden(e, false);
    OK(surf_node_overlaps(s, e));

    surf_node_destroy(s);
    surf_node_destroy(e);
    surf_image_destroy(body);
    surf_image_destroy(food);
}

/* the vomit_kitty case whole: a head box through all four facings */
static void test_hitbox_facings(void)
{
    fresh(256, 256, 16);
    surf_image *body = solid_img(32, 32);
    surf_image *food = solid_img(8, 8);
    surf_node *s = surf_sprite_new(body, 100, 100);
    surf_node *e = surf_sprite_new(food, 0, 0);
    surf_node_add(surf_screen(), s);
    surf_node_add(surf_screen(), e);
    surf_hitbox_add(s, 0, 8, 8, 8);             /* head: left edge */

    struct { uint8_t rot, mirror; int16_t ex, ey; } cases[] = {
        {0, 0, 100, 108},    /* facing left: head left  */
        {0, 1, 124, 108},    /* mirrored: head right    */
        {1, 0, 108, 124},    /* rot 90 (CCW): head bottom */
        {3, 0, 116, 100},    /* rot 270 (CW): head top  */
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        surf_sprite_set_xform(s, SURF_ONE, cases[i].rot, cases[i].mirror);
        surf_node_set_pos(e, cases[i].ex, cases[i].ey);
        OK(surf_node_overlaps(s, e));
        surf_node_set_pos(e, 100 + 12, 100 + 12);   /* body centre: miss */
        OK(!surf_node_overlaps(s, e));
    }
    surf_node_destroy(s);
    surf_node_destroy(e);
    surf_image_destroy(body);
    surf_image_destroy(food);
}

void run_hitbox_tests(void)
{
    test_hitbox_store();
    test_hitbox_abs();
    test_hitbox_overlaps();
    test_hitbox_facings();
}
