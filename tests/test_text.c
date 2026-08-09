/* M3 tests: UTF-8, measure/wrap/kerning, label paint + damage, textinput
 * editing/caret/selection/scroll — all against a synthetic font so the
 * numbers are exact and no baked assets are needed. */
#include <string.h>

#include "mock_hal.h"

/* synthetic font: A–Z adv 10 (8×10 glyphs at x=(cp-'A')*8), space adv 5,
 * hyphen adv 6, '?' adv 10, '…' adv 12; kern pair A→V = −2.
 * ascent 12, descent −3, gap 1 → line height 16. */
static surf_glyph tglyphs[] = {
    {' ', 0, 0, 0, 0, 0, 0, 5},
    {'-', 240, 0, 4, 2, 1, -4, 6},
    {'?', 246, 0, 8, 10, 1, -10, 10},
    /* A–Z filled in by mkfont() */
    {'A', 0, 0, 8, 10, 1, -10, 10},
};
static surf_glyph tg_all[3 + 26 + 1];
static surf_kern tkerns[] = {{'A', 'V', -2}};
surf_font tfont;  /* shared with test_scroll.c's dropdown tests */

static uint8_t tatlas[256 * 16];  /* real backing so the grid can read it */

/* A stand-in EMOJI SET: one ARGB glyph at a codepoint tfont has not got,
 * 12 wide against tfont's 10 so it is also the "wider than a cell" case.
 * Solid magenta with a transparent left column, so a test can tell the
 * picture's own colours from the cell's fg and prove the alpha ran. */
#define EMO_CP   0x1F525u
#define EMO_PX   0xffff00ffu     /* opaque magenta */
static uint32_t eatlas[16 * 16];
static surf_glyph eglyphs[] = {
    /* cp, x, y, w, h, xoff, yoff, adv — the box is 12x12 sitting on the
     * baseline, trimmed by one column on the left */
    {EMO_CP, 0, 0, 11, 12, 1, -12, 12},
};
surf_font efont;   /* shared with test_grid.c's wide-cell test */

static void mkemoji(void)
{
    for (int i = 0; i < 16 * 16; i++)
        eatlas[i] = EMO_PX;
    efont = (surf_font){
        .atlas = {.pixels = eatlas, .w = 16, .h = 16, .stride = 16 * 4,
                  .format = SURF_FMT_ARGB8888},
        .ascent = 12, .descent = 0, .line_gap = 0,
        .glyphs = eglyphs, .nglyphs = 1,
        .kerns = tkerns, .nkerns = 0,
    };
}

static void mkfont(void)
{
    tg_all[0] = tglyphs[0];
    tg_all[1] = tglyphs[1];
    tg_all[2] = tglyphs[2];
    for (int i = 0; i < 26; i++)
        tg_all[3 + i] = (surf_glyph){(uint32_t)('A' + i), (int16_t)(i * 8), 0,
                                     8, 10, 1, -10, 10};
    tg_all[29] = (surf_glyph){0x2026, 208, 0, 10, 4, 1, -4, 12};
    memset(tatlas, 0x80, sizeof tatlas);
    tfont = (surf_font){
        .atlas = {.pixels = tatlas, .w = 256, .h = 16, .stride = 256,
                  .format = SURF_FMT_A8},
        .ascent = 12, .descent = -3, .line_gap = 1,
        .glyphs = tg_all, .nglyphs = 30,
        .kerns = tkerns, .nkerns = 1,
    };
    mkemoji();
    tfont.fallback = &efont;
}

static void test_utf8(void)
{
    int32_t i = 0;
    const char *s = "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x8e\x9b";  /* A é € 🎛 */
    OK(surf_utf8_next(s, &i) == 'A' && i == 1);
    OK(surf_utf8_next(s, &i) == 0xe9 && i == 3);
    OK(surf_utf8_next(s, &i) == 0x20ac && i == 6);
    OK(surf_utf8_next(s, &i) == 0x1f39b && i == 10);
    OK(surf_utf8_next(s, &i) == 0);
    OK(surf_utf8_prev(s, 10) == 6);
    OK(surf_utf8_prev(s, 6) == 3);
    OK(surf_utf8_prev(s, 1) == 0);
    OK(surf_utf8_prev(s, 0) == 0);
}

static bool pt_eq(surf_point p, int16_t x, int16_t y) { return p.x == x && p.y == y; }

static void test_measure(void)
{
    OK(surf_font_line_h(&tfont) == 16);
    OK(pt_eq(surf_text_measure(&tfont, "AB", 0), 20, 16));
    OK(pt_eq(surf_text_measure(&tfont, "AV", 0), 18, 16));  /* kern −2 */
    OK(pt_eq(surf_text_measure(&tfont, "AA BB", 0), 45, 16));
    OK(pt_eq(surf_text_measure(&tfont, "", 0), 0, 16));

    /* greedy wrap on space: trailing space never counts */
    OK(pt_eq(surf_text_measure(&tfont, "AA BB CC", 25), 20, 48));
    /* break after hyphen, hyphen stays on the line */
    OK(pt_eq(surf_text_measure(&tfont, "AA-BB", 30), 26, 32));
    /* a word wider than the box hard-breaks mid-word */
    OK(pt_eq(surf_text_measure(&tfont, "AAAA", 25), 20, 32));
    /* explicit newlines always break */
    OK(pt_eq(surf_text_measure(&tfont, "A\nB", 0), 10, 32));
    /* missing glyph falls back to '?' */
    OK(pt_eq(surf_text_measure(&tfont, "A~", 0), 20, 16));
}

static int count_op(char op)
{
    int c = 0;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == op)
            c++;
    return c;
}

static void test_label(void)
{
    fresh(200, 100, 32);

    surf_node *t = surf_text_new(&tfont, "AB", 0, 0, SURF_RGB(255, 255, 255));
    OK(t && t->w == 20 && t->h == 16);
    surf_node_add(surf_screen(), t);
    surf_tick();
    /* bg fill + 2 glyph blends + present */
    OK(count_op('A') == 2);
    OK(ops[1].op == 'A' && ops[1].imgv.pixels == t->u.text.img.pixels);
    OK(ops[1].dst.x == 1 && ops[1].dst.y == 2);   /* xoff 1, 12 − 10 */
    OK(ops[1].src.x == 0 && ops[1].src.w == 8);   /* 'A' cell */
    OK(ops[2].dst.x == 11 && ops[2].src.x == 8);  /* 'B' cell */

    /* centered in a wrap box */
    surf_text_set_wrap(t, 60);
    surf_text_set_align(t, SURF_ALIGN_CENTER);
    surf_tick();
    nops = 0;
    surf_damage_subtree(t);
    surf_tick();
    OK(ops[1].op == 'A' && ops[1].dst.x == 21);  /* (60−20)/2 + xoff */

    /* set_text damages old and new bounds */
    surf_text_set_wrap(t, 0);
    surf_text_set_align(t, SURF_ALIGN_LEFT);
    surf_tick();
    surf_text_set(t, "ABCD");
    OK(surf_g.dirty.n >= 1 && surf_rect_covers(surf_g.dirty.r[0], (surf_rect){0, 0, 40, 16}));
    surf_tick();

    /* ellipsize: AAAA @ 35 → A A … */
    surf_text_set(t, "AAAA");
    surf_text_set_wrap(t, 35);
    surf_text_set_ellipsis(t, true);
    OK(t->w == 35 && t->h == 16);
    nops = 0;
    surf_tick();
    OK(count_op('A') == 3);
    bool ell = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'A' && ops[i].src.x == 208)
            ell = true;
    OK(ell);

    surf_node_destroy(t);
}

static void test_textinput(void)
{
    fresh(200, 100, 32);

    surf_node *n = surf_textinput_new(&tfont, 10, 10, 50, SURF_RGB(255, 255, 255));
    OK(n && n->w == 50 && n->h == 16);
    surf_node_add(surf_screen(), n);

    surf_textinput_set_text(n, "ABC");
    OK(strcmp(surf_textinput_text(n), "ABC") == 0);
    OK(surf_textinput_caret(n) == 3);

    surf_textinput_insert(n, "D");
    OK(strcmp(surf_textinput_text(n), "ABCD") == 0 && surf_textinput_caret(n) == 4);

    surf_textinput_move(n, -2, false);
    OK(surf_textinput_caret(n) == 2);
    surf_textinput_backspace(n);
    OK(strcmp(surf_textinput_text(n), "ACD") == 0 && surf_textinput_caret(n) == 1);
    surf_textinput_delete(n);
    OK(strcmp(surf_textinput_text(n), "AD") == 0 && surf_textinput_caret(n) == 1);

    /* selection replace */
    surf_textinput_set_caret(n, 0, false);
    surf_textinput_set_caret(n, 2, true);
    surf_textinput_insert(n, "Z");
    OK(strcmp(surf_textinput_text(n), "Z") == 0 && surf_textinput_caret(n) == 1);

    /* caret from x: nearest boundary, scroll-aware */
    surf_textinput_set_text(n, "AB");
    OK(surf_textinput_index_from_x(n, 4) == 0);
    OK(surf_textinput_index_from_x(n, 6) == 1);
    OK(surf_textinput_index_from_x(n, 100) == 2);

    /* scroll-into-view: caret at the end of 100px of text in a 50px box */
    surf_textinput_set_text(n, "AAAAAAAAAA");
    OK(n->u.input.scroll_x == 100 - (50 - 2 - 2));
    surf_textinput_move(n, -9999, false);
    OK(n->u.input.scroll_x == 0);

    /* focused caret paints as a 2px fill in the text color */
    surf_textinput_set_text(n, "AB");
    surf_textinput_set_focused(n, true);
    surf_tick();
    nops = 0;
    surf_damage_subtree(n);
    surf_tick();
    bool caret = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'F' && ops[i].r.w == 2 && ops[i].r.h == 16 &&
            ops[i].r.x == 10 + 20)
            caret = true;
    OK(caret);

    /* selection highlight fills behind the glyphs */
    surf_textinput_set_caret(n, 0, false);
    surf_textinput_set_caret(n, 2, true);
    surf_tick();
    nops = 0;
    surf_damage_subtree(n);
    surf_tick();
    bool sel = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'F' && ops[i].r.w == 20 && ops[i].r.x == 10 && ops[i].r.h == 16)
            sel = true;
    OK(sel);

    surf_node_destroy(n);
}

/* The fallback face: a codepoint the font has not got must come back
 * from the emoji set, with the SET named as its source — a lookup that
 * returned the glyph but not the face would blit an emoji's rect out of
 * the text atlas, which draws a plausible piece of a letter. */
static void test_fallback_lookup(void)
{
    const surf_font *src = NULL;

    /* a codepoint the face HAS is never overridden by the fallback */
    OK(surf_font_glyph_in(&tfont, 'A', &src) == &tg_all[3] && src == &tfont);

    /* one it has not got comes from the set, and says so */
    const surf_glyph *g = surf_font_glyph_in(&tfont, EMO_CP, &src);
    OK(g == &eglyphs[0] && src == &efont);

    /* one NEITHER has is still the '?' of the text face, not nothing */
    g = surf_font_glyph_in(&tfont, 0x1F600u, &src);
    OK(g && g->cp == '?' && src == &tfont);

    /* with no fallback wired, the same lookup falls straight to '?' */
    surf_font bare = tfont;
    bare.fallback = NULL;
    g = surf_font_glyph_in(&bare, EMO_CP, &src);
    OK(g && g->cp == '?' && src == &bare);

    OK(surf_font_is_color(&efont) && !surf_font_is_color(&tfont));
}

/* ...and the label paint path blits it out of the SET's atlas, untinted.
 * Both halves matter: the wrong atlas draws garbage, and tinting a
 * picture would flatten it to one colour. */
static void test_fallback_label(void)
{
    fresh(200, 100, 32);
    /* "A" then the emoji: one glyph from each face, so the ops are a pair */
    surf_node *t = surf_text_new(&tfont, "A\xf0\x9f\x94\xa5", 0, 0,
                                 SURF_RGB(255, 0, 0));
    surf_node_add(surf_screen(), t);
    surf_tick();
    OK(count_op('A') == 2);
    OK(ops[1].imgv.pixels == tatlas);            /* the letter: text atlas */
    OK(ops[2].imgv.pixels == eatlas);            /* the emoji: emoji atlas */
    OK(ops[2].imgv.format == SURF_FMT_ARGB8888); /* ...as a picture */
    /* the pen advanced by the EMOJI's advance (12), not the letter's */
    OK(ops[2].dst.x == 10 + 1);
    surf_node_destroy(t);
}

/* surf_text_bake: the label's layout aimed at an image. The synthetic
 * atlas is solid 0x80 coverage, so a baked glyph pixel carries exactly
 * that alpha with the caller's colour, the gaps stay transparent, and
 * the ARGB fallback keeps its own colours — a tint would mean nothing
 * on a picture. */
static void test_bake(void)
{
    surf_image *img = surf_text_bake(&tfont, "AB", SURF_RGB(255, 0, 0), 0);
    OK(img != NULL);
    OK(img->w == 20 && img->h == 16);          /* the measure, exactly */
    /* 'A' boxes x 1..8, y 2..11 (xoff 1, yoff -10 under ascent 12) */
    uint32_t p = ((const uint32_t *)((const uint8_t *)img->pixels +
                                     4 * img->stride))[4];
    OK((p >> 24) == 0x80);                     /* the atlas's coverage */
    OK(((p >> 16) & 0xff) == 0xf8);            /* the tint's red */
    uint32_t q = ((const uint32_t *)img->pixels)[0];
    OK((q >> 24) == 0);                        /* outside a glyph: clear */
    surf_image_destroy(img);

    surf_image *e = surf_text_bake(&tfont, "\xF0\x9F\x94\xA5",
                                   SURF_RGB(0, 255, 0), 0);
    OK(e != NULL);
    uint32_t m = ((const uint32_t *)((const uint8_t *)e->pixels +
                                     2 * e->stride))[2];
    OK(m == 0xffff00ffu);                      /* its own magenta, no tint */
    surf_image_destroy(e);

    OK(surf_text_bake(NULL, "A", 0, 0) == NULL);
}

void run_text_tests(void)
{
    mkfont();
    test_utf8();
    test_measure();
    test_label();
    test_textinput();
    test_fallback_lookup();
    test_fallback_label();
    test_bake();
}
