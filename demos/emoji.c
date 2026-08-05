/* Emoji specimen (desktop host). Proves the one thing worth proving:
 * that a codepoint no text face carries is drawn as a colour picture, in
 * BOTH paint paths — the label's per-glyph blend and the textgrid's cell
 * composer, which are separate code and fail separately.
 *
 * The grid half is the one to look at. An emoji is square and a mono cell
 * is not, so an emoji owns TWO cells there; if that arithmetic is wrong
 * it does not crash, it draws a convincing half of a picture.
 *
 * Esc/close quits; argv[1] caps frames; SURF_SHOT=x.ppm dumps the fb.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "surfer.h"
#include "hal_sdl.h"

#define W 1024
#define H 600

/* UTF-8 encode, because writing "\xf0\x9f\x94\xa5" by hand in a source
 * file is how a demo ends up testing the author's arithmetic instead of
 * the font's. */
static char *u8(uint32_t cp, char *b)
{
    int i = 0;
    if (cp < 0x80) {
        b[i++] = (char)cp;
    } else if (cp < 0x800) {
        b[i++] = (char)(0xc0 | (cp >> 6));
        b[i++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        b[i++] = (char)(0xe0 | (cp >> 12));
        b[i++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[i++] = (char)(0x80 | (cp & 0x3f));
    } else {
        b[i++] = (char)(0xf0 | (cp >> 18));
        b[i++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        b[i++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[i++] = (char)(0x80 | (cp & 0x3f));
    }
    b[i] = 0;
    return b;
}

/* append the emoji named `name`, then a space */
static void add_named(char *dst, size_t cap, const char *name)
{
    char b[8];
    uint32_t cp = surf_emoji_cp(name);
    if (!cp)
        return;
    strncat(dst, u8(cp, b), cap - strlen(dst) - 1);
    strncat(dst, " ", cap - strlen(dst) - 1);
}

int main(int argc, char **argv)
{
    long max_frames = argc > 1 ? strtol(argv[1], NULL, 10) : 0;

    const surf_hal *hal = surf_hal_sdl_init(W, H, "surfer — emoji");
    if (!hal || !surf_init(hal, W, H, &(surf_config){.max_nodes = 200,
                                                     .bg = SURF_RGB(24, 26, 32)})) {
        fprintf(stderr, "emoji: init failed\n");
        return 1;
    }

    const surf_font *big  = surf_font_builtin("ui28");
    const surf_font *mid  = surf_font_builtin("ui16");
    const surf_font *smal = surf_font_builtin("ui12");
    const surf_font *mono = surf_font_builtin("mono16");
    surf_node *scr = surf_screen();

    surf_node_add(scr, surf_text_new(mid, "surfer emoji — a fallback face, "
                                          "not a second way to draw",
                                     16, 12, SURF_RGB(140, 150, 170)));

    /* ---- labels: mixed into ordinary text, at three sizes ---- */
    char line[256];
    int y = 48;
    const surf_font *faces[3] = {smal, mid, big};
    const char *names[3] = {"ui12", "ui16", "ui28"};
    for (int i = 0; i < 3; i++) {
        snprintf(line, sizeof line, "%s  build ", names[i]);
        add_named(line, sizeof line, "check");
        strncat(line, " ok, tests ", sizeof line - strlen(line) - 1);
        add_named(line, sizeof line, "cross");
        strncat(line, " 3 fail ", sizeof line - strlen(line) - 1);
        add_named(line, sizeof line, "fire");
        surf_node_add(scr, surf_text_new(faces[i], line, 16, (int16_t)y,
                                         SURF_RGB(230, 232, 238)));
        y += surf_font_line_h(faces[i]) + 10;
    }

    /* ---- a sheet, to see the set rather than a sentence ---- */
    surf_node_add(scr, surf_text_new(smal, "the set (surfer.emoji names)",
                                     16, (int16_t)(y + 6),
                                     SURF_RGB(140, 150, 170)));
    y += 28;
    int n = surf_emoji_count(), per = 34, shown = 0;
    for (int i = 0; i < n && shown < per * 4; i += 3, shown++) {
        char b[8];
        u8(surf_emoji_cp_at(i), b);
        surf_node_add(scr, surf_text_new(mid, b,
                                         (int16_t)(16 + (shown % per) * 28),
                                         (int16_t)(y + (shown / per) * 26),
                                         SURF_RGB(255, 255, 255)));
    }
    y += 4 * 26 + 14;

    /* ---- textgrid: the other paint path entirely ---- */
    surf_node_add(scr, surf_text_new(smal,
        "textgrid (mono16) — an emoji owns two cells, so it can be square",
        16, (int16_t)y, SURF_RGB(140, 150, 170)));
    y += 24;

    surf_node *g = surf_textgrid_new(mono, 60, 6, SURF_RGB(220, 224, 230),
                                     SURF_RGB(16, 18, 24));
    if (g) {
        surf_node_set_pos(g, 16, (int16_t)y);
        surf_node_add(scr, g);
        /* no em dash here: mono16 is baked with CP437, which never had
         * one, so it would draw the '?' that proves the OTHER fallback
         * still works and read as a bug in this one */
        surf_textgrid_set_row(g, 0, "  the quick brown fox - plain text, one cell each");
        char row[160] = "  ";
        const char *set1[] = {"rocket", "fire", "star", "heart", "check",
                              "cross", "bulb", "lock", "folder", "coffee"};
        for (int i = 0; i < 10; i++) {
            add_named(row, sizeof row, set1[i]);
            strncat(row, " ", sizeof row - strlen(row) - 1);
        }
        surf_textgrid_set_row(g, 2, row);
        row[2] = 0;
        const char *set2[] = {"dog", "cat", "penguin", "bee", "apple",
                              "pizza", "guitar", "trophy", "moon", "rainbow"};
        for (int i = 0; i < 10; i++) {
            add_named(row, sizeof row, set2[i]);
            strncat(row, " ", sizeof row - strlen(row) - 1);
        }
        surf_textgrid_set_row(g, 3, row);
        surf_textgrid_set_row(g, 5, "  ...and text after them still lines up.");
    }

    long frames = 0;
    while (surf_hal_sdl_pump()) {
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
