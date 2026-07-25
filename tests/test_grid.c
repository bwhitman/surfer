/* Textgrid tests: cell model, row fill, scroll, damage granularity, and
 * the CPU fast path writing real pixels into the mock framebuffer. Uses
 * the synthetic font from test_text.c (cell = 'M' advance 10 × line 16). */
#include <string.h>

#include "mock_hal.h"

extern surf_font tfont;

static uint16_t px(int x, int y)
{
    return mock_fb[y * mock_w + x];
}

static void test_grid_model(void)
{
    fresh(400, 200, 32);

    surf_node *g = surf_textgrid_new(&tfont, 10, 4, 0xffff, 0x0000);
    OK(g && g->w == 100 && g->h == 64);
    OK(surf_textgrid_cell_size(g).x == 10 && surf_textgrid_cell_size(g).y == 16);
    surf_node_add(surf_screen(), g);
    surf_tick();

    /* row fill + damage bounded to the touched cells */
    surf_textgrid_set_row(g, 1, "AB");
    OK(surf_g.dirty.n == 1);
    /* cells 0..1 changed; the space-pad right of them was already spaces */
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){0, 16, 20, 16}));
    surf_tick();

    /* single cell damage */
    surf_textgrid_set_cell(g, 5, 2, 'Z', 1, 2);
    OK(surf_g.dirty.n == 1);
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){50, 32, 10, 16}));
    surf_tick();

    /* unchanged writes are free */
    surf_textgrid_set_cell(g, 5, 2, 'Z', 1, 2);
    OK(surf_g.dirty.n == 0);

    /* scroll up by one row moves content and blanks the last row */
    surf_textgrid_set_row(g, 0, "QQ");
    surf_tick();
    surf_textgrid_scroll(g, 1);
    OK(surf_g.dirty.n == 1);
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){0, 0, 100, 64}));
    surf_tick();
    /* row0 now holds former row1 ("AB"), row1 former row2 (Z at col 5) */
    surf_textgrid_set_row(g, 0, "AB");  /* identical content → no damage */
    OK(surf_g.dirty.n == 0);
    surf_textgrid_set_cell(g, 5, 1, 'Z', 1, 2);
    OK(surf_g.dirty.n == 0);

    surf_node_destroy(g);
}

static void test_grid_pixels(void)
{
    fresh(400, 200, 32);

    /* grid at (20, 10); 'A' glyph in tfont: cell 10x16, glyph 8x10 at
     * xoff 1, yoff -10 from the 12px ascent → glyph box y 2..12, x 1..9 */
    surf_node *g = surf_textgrid_new(&tfont, 4, 2, 0xffff, 0x1234);
    surf_node_add(surf_screen(), g);
    surf_node_set_pos(g, 20, 10);
    surf_tick();

    /* bg everywhere in an empty cell */
    OK(px(21, 11) == 0x1234);
    OK(px(20 + 39, 10 + 31) == 0x1234);

    surf_textgrid_set_cell(g, 1, 0, 'A', 0xffff, 0x1234);
    surf_tick();
    /* the synthetic atlas pixels are fake memory, so glyph coverage values
     * are arbitrary — but the cell must still be fully written: corners of
     * the cell outside the glyph box are exactly bg */
    OK(px(20 + 10, 10 + 0) == 0x1234);   /* cell top-left, above glyph */
    OK(px(20 + 19, 10 + 15) == 0x1234);  /* cell bottom-right, below glyph */

    /* opaque: a textgrid covering the dirty rect suppresses the bg fill */
    nops = 0;
    surf_textgrid_set_cell(g, 2, 1, 'B', 0xffff, 0x1234);
    surf_tick();
    for (int i = 0; i < nops; i++)
        OK(ops[i].op != 'F' || ops[i].c != SURF_RGB(0, 0, 0));

    surf_node_destroy(g);
}

static void test_grid_fast_scroll(void)
{
    fresh(400, 200, 32);

    surf_node *g = surf_textgrid_new(&tfont, 10, 4, 0xffff, 0x1234);
    surf_node_add(surf_screen(), g);
    surf_textgrid_set_row(g, 0, "AB");
    surf_tick();

    /* without fast scroll: full-grid damage (the old behavior) */
    surf_textgrid_scroll(g, 1);
    OK(surf_g.dirty.n == 1 &&
       rect_eq(surf_g.dirty.r[0], (surf_rect){0, 0, 100, 64}));
    surf_tick();

    /* fast: hal shift op + damage only the exposed bottom row */
    surf_textgrid_set_fast_scroll(g, true);
    nops = 0;
    surf_textgrid_scroll(g, 1);
    OK(nops == 1 && ops[0].op == 'S');
    OK(rect_eq(ops[0].r, (surf_rect){0, 0, 100, 64}));
    OK((int16_t)ops[0].c == 16);  /* one cell row in pixels */
    OK(surf_g.dirty.n == 1 &&
       rect_eq(surf_g.dirty.r[0], (surf_rect){0, 48, 100, 16}));
    nops = 0;
    surf_tick();
    /* compose repaints only the exposed strip — no full-grid rewrite */
    OK(nops < 30);

    /* downward fast scroll damages the top row */
    surf_textgrid_scroll(g, -1);
    OK(surf_g.dirty.n == 1 &&
       rect_eq(surf_g.dirty.r[0], (surf_rect){0, 0, 100, 16}));
    surf_tick();

    /* detached grids fall back to the slow (damage-free) path safely */
    surf_node_detach(g);
    nops = 0;
    surf_textgrid_scroll(g, 1);
    OK(nops == 0);
    surf_node_add(surf_screen(), g);
    surf_tick();
    surf_node_destroy(g);
}

/* the ring arithmetic textgrid.c uses, so the test reads what the paint
 * path would read */
static uint32_t grid_cp(surf_node *g, int16_t col, int16_t row)
{
    int16_t t = g->u.grid.total_rows;
    int16_t r = (int16_t)((g->u.grid.head + row - g->u.grid.view) % t);
    if (r < 0)
        r = (int16_t)(r + t);
    return g->u.grid.cells[r * g->u.grid.cols + col].cp;
}

/* Scrollback: lines that leave the top stay reachable, the view snaps
 * back on a write, and history is capped at the multiplier. */
static void test_grid_scrollback(void)
{
    fresh(400, 200, 32);
    surf_node *g = surf_textgrid_new(&tfont, 8, 4, 0xffff, 0);
    surf_node_add(surf_screen(), g);
    OK(surf_textgrid_set_scrollback(g, 3));      /* 4 rows -> 12 */
    OK(surf_textgrid_history(g) == 0);           /* nothing scrolled yet */

    surf_textgrid_set_row(g, 0, "first");
    for (int i = 0; i < 4; i++)
        surf_textgrid_scroll(g, 1);
    OK(surf_textgrid_history(g) == 4);           /* four rows pushed up */

    /* look back: "first" is 4 rows above the live window */
    surf_textgrid_set_view(g, 4);
    OK(surf_textgrid_view(g) == 4);
    OK(grid_cp(g, 0, 0) == 'f');

    /* a write snaps to live, as a terminal does */
    surf_textgrid_set_row(g, 3, "typed");
    OK(surf_textgrid_view(g) == 0);

    /* history saturates at (total - rows), never grows past the ring */
    for (int i = 0; i < 40; i++)
        surf_textgrid_scroll(g, 1);
    OK(surf_textgrid_history(g) == 8);           /* 12 total - 4 visible */
    surf_textgrid_set_view(g, 999);              /* clamped, not wild */
    OK(surf_textgrid_view(g) == 8);

    surf_node_destroy(g);
}

void run_grid_tests(void)
{
    test_grid_model();
    test_grid_pixels();
    test_grid_fast_scroll();
    test_grid_scrollback();
}
