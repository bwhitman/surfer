/* See fonts_scene.h.
 *
 * Page 1 asks "how should we antialias an outline face?" — same faces,
 * different bakes, on both text paths. Page 2 asks the other question:
 * what if the face was drawn as pixels in the first place? Those are
 * baked FONTBAKE_EM=1 at a grid-aligned ppem, where the rasterizer never
 * produces a partial-coverage pixel, so there is no AA to argue about.
 *
 * The two are not the same thing as "1-bit Roboto": thresholding an
 * outline drawn for AA moves every edge to the nearest pixel and the
 * stems come out uneven. A pixel face has no edges off the grid to move. */
#include <stdio.h>

#include "fonts_scene.h"

/* every specimen names its font; missing ones just skip */
#define F(n) surf_font_builtin(n)

#define COL_TEXT  SURF_RGB(240, 242, 248)
#define COL_DIM   SURF_RGB(140, 148, 162)
#define COL_GRID  SURF_RGB(14, 16, 20)
#define COL_ACC   SURF_RGB(240, 190, 80)
#define COL_BG    SURF_RGB(24, 26, 32)

static const char *SAMPLE1 = ">>> the quick brown fox jumps over the lazy dog";
static const char *SAMPLE2 = "ABCDEFGHIJKLM 0123456789 iIl1| oO0 {}[]()<>*#~%";

/* helpers add to whichever page is being built */
static surf_node *g_parent;
#define NPAGES 3
static surf_node *g_page[NPAGES];
static int        g_shown;

static int16_t caption(const surf_font *f, int16_t x, int16_t y, const char *s)
{
    surf_node_add(g_parent, surf_text_new(f, s, x, y, COL_DIM));
    return (int16_t)(y + surf_font_line_h(f) + 2);
}

/* one textgrid specimen: caption + a 2-row grid as wide as fits in w */
static int16_t grid_specimen(int16_t x, int16_t y,
                             int16_t w, const surf_font *f, const char *name)
{
    surf_node *g = surf_textgrid_new(f, 1, 1, COL_TEXT, COL_GRID);
    if (!g)
        return y;
    surf_point cell = surf_textgrid_cell_size(g);
    surf_node_destroy(g);
    if (cell.x <= 0 || cell.y <= 0)
        return y;

    char cap[128];
    snprintf(cap, sizeof cap, "%s  (cell %dx%d)", name, cell.x, cell.y);
    y = caption(F("ui12"), x, y, cap);

    int16_t cols = (int16_t)(w / cell.x);
    g = surf_textgrid_new(f, cols, 2, COL_TEXT, COL_GRID);
    surf_node_set_pos(g, x, y);
    surf_textgrid_set_row(g, 0, SAMPLE1);
    surf_textgrid_set_row(g, 1, SAMPLE2);
    surf_node_add(g_parent, g);
    return (int16_t)(y + 2 * cell.y + 10);
}

static int16_t label_specimen(int16_t x, int16_t y,
                              int16_t w, const surf_font *f, const char *name,
                              const char *text, surf_color c)
{
    y = caption(F("ui12"), x, y, name);
    surf_node *t = surf_text_new(f, text, x, y, c);
    surf_text_set_wrap(t, w);
    surf_node_add(g_parent, t);
    return (int16_t)(y + surf_text_measure(f, text, w).y + 10);
}

/* ---- page 1: antialiasing knobs on outline faces ---- */

static void build_page1(int16_t w)
{
    const int16_t margin = 20;
    const int16_t colw = (int16_t)((w - 3 * margin) / 2);
    const int16_t lx = margin, rx = (int16_t)(margin * 2 + colw);

    int16_t y = 56;
    y = label_specimen(lx, y, colw, F("ui28"),
                       "label - Roboto 28 AA",
                       "Handgloves & Wafer 0123", COL_TEXT);
    y = label_specimen(lx, y, colw, F("ui16"),
                       "label - Roboto 16 AA (ui default), wrapped",
                       "Labels are kerned proportional text drawn as clipped "
                       "A8 atlas blits - the same frame path as sprites. "
                       "Greedy wrap breaks on spaces and after hyphens; "
                       "AVAWA To Ty kerning pairs come from the bake.",
                       COL_TEXT);
    y = label_specimen(lx, y, colw, F("ui12"),
                       "label - Roboto 12 AA (captions; small AA holds up)",
                       "the quick brown fox jumps over the lazy dog 0123456789",
                       COL_TEXT);
    y = label_specimen(lx, y, colw, F("ui16b"),
                       "label - Roboto 16, 1-bit (why proportional wants AA)",
                       "the quick brown fox jumps over the lazy dog - jagged "
                       "curves, lumpy stems: thresholding an outline drawn "
                       "for AA is not the same as a pixel font. See page 2.",
                       COL_ACC);
    /* ui16 and ui23 are the SAME physical size, on different screens: the
     * desktop window puts a framebuffer pixel on a 110-140dpi point
     * (display-scaling dependent), the P4's 7" 1024x600 panel on a 169dpi
     * one. Which is why the ramp carries both instead of scaling one —
     * and why this page reads bigger here than it will on the panel.
     * SURF_NATIVE=1 previews the denser end. */
    label_specimen(lx, y, colw, F("ui23"),
                   "label - Roboto 23 AA (P4 body: ui16's size on the panel)",
                   "the quick brown fox jumps over the lazy dog",
                   COL_TEXT);

    y = 56;
    y = grid_specimen(rx, y, colw, F("mono16"),
                      "textgrid - JetBrains Mono 16 AA (today's default)");
    y = grid_specimen(rx, y, colw, F("mono16g"),
                      "textgrid - JetBrains Mono 16 AA + gamma 0.55");
    y = grid_specimen(rx, y, colw, F("mono16b"),
                      "textgrid - JetBrains Mono 16, 1-bit cut 96");
    y = grid_specimen(rx, y, colw, F("bigblue12"),
                      "textgrid - BigBlue Terminal 12, 1-bit (native size)");
    y = grid_specimen(rx, y, colw, F("bigblue24"),
                      "textgrid - BigBlue Terminal 24, 1-bit (2x native)");
    grid_specimen(rx, y, colw, F("mono24"),
                  "textgrid - JetBrains Mono 24 AA");
}

/* ---- page 2: pixel-designed proportional faces ---- */

static void build_page2(int16_t w)
{
    const int16_t margin = 20;
    const int16_t colw = (int16_t)((w - 3 * margin) / 2);
    const int16_t lx = margin, rx = (int16_t)(margin * 2 + colw);
    const char *pangram = "the quick brown fox jumps over the lazy dog "
                          "0123456789";

    int16_t y = 56;
    y = caption(F("ui12"), lx, y,
                "Kenney Pixel size ramp - em 16/32/48/64, every one exactly "
                "on the grid (gray 0-3%)");
    y = (int16_t)(y + 4);
    y = label_specimen(lx, y, colw, F("kpixel16"),
                       "Kenney Pixel, em 16 (cell 8x12) - native resolution",
                       pangram, COL_TEXT);
    y = label_specimen(lx, y, colw, F("kpixel32"),
                       "Kenney Pixel, em 32 (cell 16x24) - 2x2 pixels",
                       "Handgloves & Wafer 0123", COL_TEXT);
    y = label_specimen(lx, y, colw, F("kpixel48"),
                       "Kenney Pixel, em 48 (cell 24x36) - 3x3 pixels",
                       "Handgloves 012", COL_TEXT);
    y = label_specimen(lx, y, colw, F("kpixel64"),
                       "Kenney Pixel, em 64 (cell 32x48) - 4x4 pixels",
                       "Handglove", COL_TEXT);
    y = (int16_t)(y + 6);
    label_specimen(lx, y, colw, F("ui12"),
                   "what the ramp actually is:",
                   "A pixel face has one true resolution - Kenney Pixel is "
                   "5x7 per glyph with 1px stems. Larger sizes are integer "
                   "multiples of that grid: em32 is byte-identical to em16 "
                   "with 2x2 pixels (87 of 96 glyphs; the 9 that differ are "
                   "diagonals, which do gain detail). So bigger reads "
                   "chunkier, not finer - same trade as BigBlue 24.",
                   COL_DIM);

    y = 56;
    y = caption(F("ui12"), rx, y,
                "faces drawn on a taller grid - more detail at size");
    y = (int16_t)(y + 4);
    y = label_specimen(rx, y, colw, F("kmini16"),
                       "Kenney Mini, em 16 (cell 12x20)", pangram, COL_TEXT);
    y = label_specimen(rx, y, colw, F("kmini32"),
                       "Kenney Mini, em 32 (cell 24x40)",
                       "Handgloves 0123", COL_TEXT);
    y = label_specimen(rx, y, colw, F("khigh32"),
                       "Kenney High, em 32 (cell 16x28)",
                       "Handgloves & Wafer", COL_TEXT);
    y = label_specimen(rx, y, colw, F("kblocks16"),
                       "Kenney Blocks, em 16 (cell 16x24)",
                       "Handgloves 0123", COL_TEXT);
    y = (int16_t)(y + 8);
    y = label_specimen(rx, y, colw, F("ui28"),
                       "reference - Roboto 28 AA (big + fine, but antialiased)",
                       "Handgloves & Wafer", COL_ACC);
    label_specimen(rx, y, colw, F("ui16b"),
                   "reference - Roboto 16 thresholded (NOT a pixel font)",
                   pangram, COL_ACC);
}

/* ---- page 3: Adobe X11 BDFs, one designed face per size ---- */

static void build_page3(int16_t w)
{
    const int16_t margin = 20;
    const int16_t colw = (int16_t)((w - 3 * margin) / 2);
    const int16_t lx = margin, rx = (int16_t)(margin * 2 + colw);
    const char *pangram = "the quick brown fox jumps over the lazy dog "
                          "0123456789";

    int16_t y = 56;
    y = caption(F("ui12"), lx, y,
                "Adobe Helvetica - six separately drawn sizes, straight "
                "from BDF, 0% partial coverage");
    y = (int16_t)(y + 4);
    y = label_specimen(lx, y, colw, F("helvR08"),
                       "helvR08 (cell 9x12)", pangram, COL_TEXT);
    y = label_specimen(lx, y, colw, F("helvR10"),
                       "helvR10 (cell 12x16)", pangram, COL_TEXT);
    y = label_specimen(lx, y, colw, F("helvR12"),
                       "helvR12 (cell 13x18)", pangram, COL_TEXT);
    y = label_specimen(lx, y, colw, F("helvR14"),
                       "helvR14 (cell 16x20)", pangram, COL_TEXT);
    y = label_specimen(lx, y, colw, F("helvR18"),
                       "helvR18 (cell 21x27)", "Handgloves & Wafer", COL_TEXT);
    label_specimen(lx, y, colw, F("helvR24"),
                   "helvR24 (cell 27x35)", "Handgloves", COL_TEXT);

    y = 56;
    y = caption(F("ui12"), rx, y, "other weights and families, same source");
    y = (int16_t)(y + 4);
    y = label_specimen(rx, y, colw, F("helvB12"),
                       "helvB12 - Helvetica Bold (cell 13x18)",
                       pangram, COL_TEXT);
    y = label_specimen(rx, y, colw, F("helvB18"),
                       "helvB18 - Helvetica Bold (cell 23x27)",
                       "Handgloves & Wafer", COL_TEXT);
    y = label_specimen(rx, y, colw, F("ncenR12"),
                       "ncenR12 - New Century Schoolbook (serif)",
                       pangram, COL_TEXT);
    y = grid_specimen(rx, y, colw, F("courR14"),
                      "textgrid - courR14, Adobe Courier (mono BDF)");
    y = (int16_t)(y + 6);
    y = label_specimen(rx, y, colw, F("ui16"),
                       "reference - Roboto 16 AA (outline, antialiased)",
                       pangram, COL_ACC);
    label_specimen(rx, y, colw, F("ui12"),
                   "why these beat a thresholded outline:",
                   "Each size is its own drawing. A designer placed every "
                   "pixel of helvR10, then drew helvR12 again from scratch - "
                   "so stems stay even and counters stay open at sizes where "
                   "snapping an outline to the grid falls apart. This is the "
                   "sans-serif equivalent of what BigBlue does for mono.",
                   COL_DIM);
}

/* ---- page switching ---- */

static const char *page_name(int p)
{
    return p == 0 ? "AA knobs on outline faces"
         : p == 1 ? "Kenney pixel faces (one grid, scaled)"
                  : "Adobe X11 BDF (designed per size)";
}

static void show_page(int p)
{
    g_shown = p;
    for (int i = 0; i < NPAGES; i++)
        surf_node_set_hidden(g_page[i], i != p);
    printf("fonts: page %d/%d (%s)\n", p + 1, NPAGES, page_name(p));
    fflush(stdout);
}

static void toggle_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n; (void)user;
    if (t->phase == SURF_TOUCH_DOWN)
        show_page((g_shown + 1) % NPAGES);
}

void fonts_scene_build(int16_t w, int16_t h,
                       const char *subtitle, int page)
{
    for (int p = 0; p < NPAGES; p++) {
        g_page[p] = surf_group_new(0, 0);
        surf_node_add(surf_screen(), g_page[p]);
        g_parent = g_page[p];

        surf_node_add(g_parent, surf_rect_new(0, 0, w, h, COL_BG));

        /* The page flip lives on the group, not on the backdrop rect.
         * hit() returns the front-most leaf containing the point whether
         * or not it has a handler, and a text label has none — so a tap
         * on text would be swallowed by the label if the handler sat on
         * the rect behind it. surf_input_dispatch walks up parents
         * looking for a handler, and that walk ignores geometry, so a
         * handler here catches taps anywhere on the page. */
        surf_node_set_on_touch(g_page[p], toggle_touch, NULL);

        static const char *titles[NPAGES] = {
            "surfer font specimen", "surfer pixel fonts", "surfer bitmap fonts",
        };
        surf_node_add(g_parent,
                      surf_text_new(F("ui28"), titles[p], 20, 10, COL_TEXT));
        char sub[160];
        snprintf(sub, sizeof sub, "%s - tap to switch page (%d/%d)",
                 subtitle ? subtitle : "", p + 1, NPAGES);
        surf_node_add(g_parent, surf_text_new(F("ui12"), sub, 286, 24, COL_DIM));

        if (p == 0)      build_page1(w);
        else if (p == 1) build_page2(w);
        else             build_page3(w);
    }
    show_page(page >= 1 && page <= NPAGES ? page - 1 : 0);
}
