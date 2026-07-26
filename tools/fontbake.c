/* Build-time font baker (DESIGN.md §2.5): turns a font into an A8 atlas +
 * advance/kerning tables, emitted as a C header (or a runtime-loadable
 * blob). Runtime never rasterizes — text drawing is atlas blits.
 *
 *   fontbake NAME SIZE font.ttf out.h [ranges]
 *   fontbake NAME 0    font.bdf out.h [ranges]
 *
 * SIZE IS PPEM — the em square in pixels, the number every other type
 * toolchain means by "size". It used to mean ascent-descent, which made
 * every name in the build a lie by ~32%: `ui12` was a 9.1 ppem bake with
 * a 5-pixel x-height, roughly 6pt on a 110dpi screen, and no amount of
 * rasterizer tuning rescues text that small. FONTBAKE_LINE=1 restores
 * the old meaning if some caller genuinely wants to size by line box.
 *
 * Three front ends feeding one emitter:
 *   .ttf/.otf  outlines rasterized by stb_truetype at SIZE
 *   .ttf/.otf  with FONTBAKE_HINT: rasterized by FreeType, grid-fitted
 *   .bdf       a designed bitmap font, copied pixel-for-pixel. SIZE is
 *              ignored — a BDF *is* one specific size, which is the whole
 *              point: helvR10 and helvR12 are separately drawn faces, not
 *              one outline scaled. No AA is possible by construction.
 *
 * ranges: comma-separated codepoint spans, e.g. "32-126,8230".
 * Default: ASCII 32-126 plus U+2026 (ellipsis, needed for ellipsize).
 *
 * Env knobs (TTF only unless noted; all off by default):
 *   FONTBAKE_HINT=full       grid-fit through FreeType's autohinter, so a
 *                            sub-pixel-wide stem lands on ONE solid column
 *                            instead of two 50% grays. This is the whole
 *                            difference between surfer's small text and a
 *                            2014 desktop's. Needs -DSURF_FONTBAKE_FT.
 *              =light        autohint vertically only (FreeType's "light"
 *                            target): snaps baselines and x-height, leaves
 *                            horizontal metrics alone. Softer, spacing is
 *                            closer to the designer's.
 *              =bytecode     run the font's OWN TrueType hints instead of
 *                            the autohinter. Only worth it for faces that
 *                            actually ship good ones.
 *   FONTBAKE_LINE=1          SIZE means ascent-descent, not ppem (legacy).
 *   FONTBAKE_EM=1            no-op, kept so old command lines still work:
 *                            ppem is the default now.
 *   FONTBAKE_GAMMA=0.55      boost AA alpha (gamma<1 = brighter small text)
 *   FONTBAKE_THRESHOLD=1     1-bit atlas, no antialiasing
 *   FONTBAKE_THRESHOLD_CUT=N on/off cut for 1-bit mode (default 128,
 *                            lower = bolder)
 *
 * The summary line reports two shares of the inked pixels:
 *
 *   gray   partial coverage. A BDF is always 0.0%. For an unhinted
 *          outline face it is ~0 only at a grid-aligned ppem; above a few
 *          % means the size is off the grid (or the face was drawn with
 *          curves) and thresholding it will look lumpy.
 *   solid  fully 255. This is the one that predicts whether small text
 *          reads as text or as fog, and it is the reason to hint: a
 *          hinted bake barely moves `gray` (the stems go solid, their AA
 *          sidebands stay partial) while `solid` climbs from nothing.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#define MAX_CPS 4096

static uint32_t cps[MAX_CPS];
static int ncps;

/* ---- what both front ends produce ---- */

typedef struct {
    uint32_t cp;
    int x, y, w, h;     /* placement + size in the atlas */
    int xoff, yoff;     /* pen-relative; yoff counts DOWN from the baseline,
                         * so it is negative for anything above it */
    int adv;
} bglyph;

static bglyph glyphs[MAX_CPS];
static int nglyphs;

static unsigned char *atlas;
static int aw = 128, ah = 128;
static int ascent_px, descent_px, gap_px;

static uint32_t *ka, *kb;
static int16_t *kd;
static int nkern;

static void parse_ranges(const char *s)
{
    while (*s) {
        unsigned long a = strtoul(s, (char **)&s, 10), b = a;
        if (*s == '-')
            b = strtoul(s + 1, (char **)&s, 10);
        for (unsigned long c = a; c <= b && ncps < MAX_CPS; c++)
            cps[ncps++] = (uint32_t)c;
        if (*s == ',')
            s++;
    }
}

static int wanted(uint32_t cp)
{
    for (int i = 0; i < ncps; i++)
        if (cps[i] == cp)
            return 1;
    return 0;
}

static unsigned char *slurp(const char *path, long *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    *len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)*len);
    if (buf && fread(buf, 1, (size_t)*len, fp) != (size_t)*len) {
        free(buf);
        buf = NULL;
    }
    fclose(fp);
    return buf;
}

/* ---- shared by the two front ends that rasterize glyph-at-a-time (BDF,
 * FreeType); the stb path packs through stbtt_PackFontRanges instead ---- */

typedef struct {
    uint32_t cp;
    int w, h, xoff, yoff_bdf, adv;
    unsigned char *bits;   /* w*h coverage, 0..255 (BDF: only 0 or 255) */
} rawglyph;

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Shelf-pack the collected glyphs, growing the atlas until they fit.
 * Emission order stays ascending by codepoint — surf_font_glyph binary
 * searches the table, so that order is load-bearing. */
static int pack(rawglyph *g, int n)
{
    for (;;) {
        free(atlas);
        atlas = calloc((size_t)aw * ah, 1);
        if (!atlas)
            return 0;
        int x = 0, y = 0, rowh = 0, ok = 1;
        for (int i = 0; i < n; i++) {
            if (g[i].w <= 0 || g[i].h <= 0)
                continue;
            if (x + g[i].w > aw) {
                x = 0;
                y += rowh + 1;
                rowh = 0;
            }
            if (y + g[i].h > ah) {
                ok = 0;
                break;
            }
            for (int r = 0; r < g[i].h; r++)
                memcpy(atlas + (size_t)(y + r) * aw + x,
                       g[i].bits + (size_t)r * g[i].w, (size_t)g[i].w);
            glyphs[i].x = x;
            glyphs[i].y = y;
            x += g[i].w + 1;
            if (g[i].h > rowh)
                rowh = g[i].h;
        }
        if (ok)
            return 1;
        if (ah < aw) ah *= 2; else aw *= 2;
        if (aw > 4096)
            return 0;
    }
}

static int bake_bdf(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "fontbake: cannot open %s\n", path);
        return 0;
    }
    static rawglyph g[MAX_CPS];
    int n = 0, have_asc = 0, have_desc = 0;
    int bbx_h_default = 0, bbx_yoff_default = 0;
    char line[1024];

    while (fgets(line, sizeof line, fp)) {
        if (!strncmp(line, "FONT_ASCENT", 11)) {
            ascent_px = atoi(line + 11);
            have_asc = 1;
        } else if (!strncmp(line, "FONT_DESCENT", 12)) {
            descent_px = -atoi(line + 12);   /* surfer's descent is negative */
            have_desc = 1;
        } else if (!strncmp(line, "FONTBOUNDINGBOX", 15)) {
            int bw, bh, bx, by;
            if (sscanf(line + 15, "%d %d %d %d", &bw, &bh, &bx, &by) == 4) {
                bbx_h_default = bh;
                bbx_yoff_default = by;
            }
        } else if (!strncmp(line, "STARTCHAR", 9)) {
            uint32_t cp = 0xffffffffu;
            int w = 0, h = 0, xo = 0, yo = 0, adv = 0, got_bbx = 0;
            while (fgets(line, sizeof line, fp)) {
                if (!strncmp(line, "ENCODING", 8)) {
                    long e = atol(line + 8);
                    cp = e < 0 ? 0xffffffffu : (uint32_t)e;
                } else if (!strncmp(line, "DWIDTH", 6)) {
                    adv = atoi(line + 6);
                } else if (!strncmp(line, "BBX", 3)) {
                    sscanf(line + 3, "%d %d %d %d", &w, &h, &xo, &yo);
                    got_bbx = 1;
                } else if (!strncmp(line, "BITMAP", 6)) {
                    if (!got_bbx) { h = bbx_h_default; yo = bbx_yoff_default; }
                    unsigned char *bits = (w > 0 && h > 0)
                        ? calloc((size_t)w * h, 1) : NULL;
                    int bytes_per_row = (w + 7) / 8;
                    for (int r = 0; r < h; r++) {
                        if (!fgets(line, sizeof line, fp))
                            break;
                        for (int c = 0; c < w; c++) {
                            int byte = c / 8, bit = 7 - (c % 8);
                            int hi = hexval(line[byte * 2]);
                            int lo = hexval(line[byte * 2 + 1]);
                            if (hi < 0 || lo < 0 || byte >= bytes_per_row)
                                continue;
                            if (((hi << 4) | lo) & (1 << bit))
                                bits[(size_t)r * w + c] = 255;
                        }
                    }
                    if (cp != 0xffffffffu && wanted(cp) && n < MAX_CPS) {
                        g[n].cp = cp; g[n].w = w; g[n].h = h;
                        g[n].xoff = xo; g[n].yoff_bdf = yo; g[n].adv = adv;
                        g[n].bits = bits;
                        n++;
                    } else {
                        free(bits);
                    }
                } else if (!strncmp(line, "ENDCHAR", 7)) {
                    break;
                }
            }
        }
    }
    fclose(fp);
    if (!n) {
        fprintf(stderr, "fontbake: %s has no glyphs in range\n", path);
        return 0;
    }
    if (!have_asc || !have_desc)
        fprintf(stderr, "fontbake: %s lacks FONT_ASCENT/DESCENT, using %d/%d\n",
                path, ascent_px, descent_px);

    /* sort ascending by codepoint: the runtime binary searches this table */
    for (int i = 1; i < n; i++) {
        rawglyph t = g[i];
        int j = i - 1;
        while (j >= 0 && g[j].cp > t.cp) { g[j + 1] = g[j]; j--; }
        g[j + 1] = t;
    }

    nglyphs = n;
    for (int i = 0; i < n; i++) {
        glyphs[i].cp = g[i].cp;
        glyphs[i].w = g[i].w;
        glyphs[i].h = g[i].h;
        glyphs[i].xoff = g[i].xoff;
        /* BDF yoff is the bitmap's bottom above the baseline; surfer wants
         * the top, measured down from the line top (base_y = ascent) */
        glyphs[i].yoff = -(g[i].yoff_bdf + g[i].h);
        glyphs[i].adv = g[i].adv;
    }
    if (!pack(g, n)) {
        fprintf(stderr, "fontbake: atlas won't fit\n");
        return 0;
    }
    for (int i = 0; i < n; i++)
        free(g[i].bits);
    gap_px = 0;
    return 1;
}

/* ---- FreeType front end (FONTBAKE_HINT) ----
 *
 * stb_truetype interprets no hints and has no autohinter, so at UI sizes
 * a stem narrower than a pixel is spread across two columns at ~30-50%
 * coverage each and never reaches solid ink — Roboto's 'l' at 9 ppem
 * peaks at 131/255. FreeType's autohinter moves the outline onto the
 * pixel grid before rasterizing, which is what made 10px text on a
 * pre-retina desktop crisp. Host-tool only: the runtime still blits the
 * same A8 atlas, and a build without FreeType just can't pass
 * FONTBAKE_HINT.
 */
#ifdef SURF_FONTBAKE_FT
#include <ft2build.h>
#include FT_FREETYPE_H

static int bake_ft(const char *path, float ppem, const char *mode)
{
    FT_Library lib;
    FT_Face face;
    if (FT_Init_FreeType(&lib)) {
        fprintf(stderr, "fontbake: FreeType init failed\n");
        return 0;
    }
    if (FT_New_Face(lib, path, 0, &face)) {
        fprintf(stderr, "fontbake: FreeType cannot open %s\n", path);
        return 0;
    }
    FT_UInt px = (FT_UInt)(ppem + 0.5f);
    if (!px || FT_Set_Pixel_Sizes(face, 0, px)) {
        fprintf(stderr, "fontbake: FreeType rejects ppem %u\n", px);
        return 0;
    }

    FT_Int32 load = FT_LOAD_DEFAULT;
    if (!strcmp(mode, "light"))
        load |= FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT;
    else if (!strcmp(mode, "bytecode"))
        load |= FT_LOAD_NO_AUTOHINT | FT_LOAD_TARGET_NORMAL;
    else
        load |= FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_NORMAL;

    /* 26.6 fixed, already grid-fitted at this ppem */
    ascent_px  = (int)(face->size->metrics.ascender >> 6);
    descent_px = (int)(face->size->metrics.descender >> 6);
    int line_h = (int)(face->size->metrics.height >> 6);
    gap_px = line_h - (ascent_px - descent_px);
    if (gap_px < 0)
        gap_px = 0;

    static rawglyph g[MAX_CPS];
    int n = 0;
    for (int i = 0; i < ncps; i++) {
        if (FT_Load_Char(face, cps[i], load))
            continue;
        FT_GlyphSlot s = face->glyph;
        if (s->format != FT_GLYPH_FORMAT_BITMAP &&
            FT_Render_Glyph(s, FT_RENDER_MODE_NORMAL))
            continue;
        int w = (int)s->bitmap.width, h = (int)s->bitmap.rows;
        unsigned char *bits = NULL;
        if (w > 0 && h > 0) {
            bits = malloc((size_t)w * h);
            /* pitch is signed but always "one row down" — buffer points
             * at the top row either way */
            for (int r = 0; r < h; r++)
                memcpy(bits + (size_t)r * w,
                       s->bitmap.buffer + (ptrdiff_t)r * s->bitmap.pitch,
                       (size_t)w);
        }
        g[n] = (rawglyph){
            .cp = cps[i], .w = w, .h = h,
            .xoff = s->bitmap_left,
            /* both are baseline-relative (font.c draws at base_y + yoff)
             * but count opposite ways: FreeType's bitmap_top is rows ABOVE
             * the baseline, surfer's yoff is negative above it. Getting
             * this backwards puts every glyph a whole ascent too low,
             * where the label's clip box eats all but a stray row. */
            .yoff_bdf = -s->bitmap_top,
            .adv = (int)((s->advance.x + 32) >> 6),
            .bits = bits,
        };
        n++;
    }
    if (!n) {
        fprintf(stderr, "fontbake: %s has no glyphs in range\n", path);
        return 0;
    }

    nglyphs = n;
    for (int i = 0; i < n; i++) {
        glyphs[i].cp = g[i].cp;
        glyphs[i].w = g[i].w;
        glyphs[i].h = g[i].h;
        glyphs[i].xoff = g[i].xoff;
        glyphs[i].yoff = g[i].yoff_bdf;
        glyphs[i].adv = g[i].adv;
    }
    if (!pack(g, n)) {
        fprintf(stderr, "fontbake: atlas won't fit\n");
        return 0;
    }
    for (int i = 0; i < n; i++)
        free(g[i].bits);

    /* FT_Get_Kerning reads the legacy `kern` table, the same one
     * stbtt_GetCodepointKernAdvance uses — so hinted and unhinted bakes
     * of the same face agree on which pairs exist. */
    if (FT_HAS_KERNING(face)) {
        ka = malloc(sizeof(uint32_t) * (size_t)ncps * ncps);
        kb = malloc(sizeof(uint32_t) * (size_t)ncps * ncps);
        kd = malloc(sizeof(int16_t) * (size_t)ncps * ncps);
        for (int i = 0; i < ncps; i++) {
            FT_UInt gi = FT_Get_Char_Index(face, cps[i]);
            for (int j = 0; j < ncps; j++) {
                FT_UInt gj = FT_Get_Char_Index(face, cps[j]);
                FT_Vector k;
                if (FT_Get_Kerning(face, gi, gj, FT_KERNING_DEFAULT, &k))
                    continue;
                int v = (int)((k.x + (k.x < 0 ? -32 : 32)) >> 6);
                if (v) {
                    ka[nkern] = cps[i];
                    kb[nkern] = cps[j];
                    kd[nkern] = (int16_t)v;
                    nkern++;
                }
            }
        }
    }
    return 1;
}
#else
static int bake_ft(const char *path, float ppem, const char *mode)
{
    (void)path; (void)ppem; (void)mode;
    fprintf(stderr, "fontbake: FONTBAKE_HINT needs a FreeType build "
                    "(rebuild with -DSURF_FONTBAKE_FT and -lfreetype)\n");
    return 0;
}
#endif

/* ---- TTF front end ---- */

static int bake_ttf(const char *path, float size, int em_mode)
{
    long fsz;
    unsigned char *ttf = slurp(path, &fsz);
    if (!ttf) {
        fprintf(stderr, "fontbake: cannot open %s\n", path);
        return 0;
    }
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        fprintf(stderr, "fontbake: bad font\n");
        return 0;
    }

    stbtt_packedchar *pc = malloc(sizeof *pc * (size_t)ncps);
    int *cplist = malloc(sizeof(int) * (size_t)ncps);
    for (int i = 0; i < ncps; i++)
        cplist[i] = (int)cps[i];
    for (;;) {
        atlas = realloc(atlas, (size_t)aw * ah);
        stbtt_pack_context pctx;
        stbtt_PackBegin(&pctx, atlas, aw, ah, aw, 1, NULL);
        /* stb takes a negative font_size to mean "map em to this many
         * pixels" instead of "make ascent-descent this tall" */
        stbtt_pack_range range = {
            .font_size = em_mode ? -size : size,
            .array_of_unicode_codepoints = cplist,
            .num_chars = ncps,
            .chardata_for_range = pc,
        };
        int ok = stbtt_PackFontRanges(&pctx, ttf, 0, &range, 1);
        stbtt_PackEnd(&pctx);
        if (ok)
            break;
        if (ah < aw) ah *= 2; else aw *= 2;   /* grow atlas, retry */
        if (aw > 4096) {
            fprintf(stderr, "fontbake: atlas won't fit\n");
            return 0;
        }
    }

    /* must match the scale stb used for the glyphs above, or the vmetrics
     * and kern table disagree with what was rasterized */
    float scale = em_mode ? stbtt_ScaleForMappingEmToPixels(&info, size)
                          : stbtt_ScaleForPixelHeight(&info, size);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    ascent_px  = (int)(ascent * scale + 0.5f);
    descent_px = (int)(descent * scale - 0.5f);
    gap_px     = (int)(gap * scale + 0.5f);

    ka = malloc(sizeof(uint32_t) * (size_t)ncps * ncps);
    kb = malloc(sizeof(uint32_t) * (size_t)ncps * ncps);
    kd = malloc(sizeof(int16_t) * (size_t)ncps * ncps);
    for (int i = 0; i < ncps; i++)
        for (int j = 0; j < ncps; j++) {
            int k = stbtt_GetCodepointKernAdvance(&info, (int)cps[i], (int)cps[j]);
            int px = (int)(k * scale + (k < 0 ? -0.5f : 0.5f));
            if (px) { ka[nkern] = cps[i]; kb[nkern] = cps[j]; kd[nkern] = (int16_t)px; nkern++; }
        }

    nglyphs = ncps;
    for (int i = 0; i < ncps; i++) {
        const stbtt_packedchar *g = &pc[i];
        glyphs[i] = (bglyph){
            .cp = cps[i], .x = g->x0, .y = g->y0,
            .w = g->x1 - g->x0, .h = g->y1 - g->y0,
            .xoff = (int)(g->xoff + (g->xoff < 0 ? -0.5f : 0.5f)),
            .yoff = (int)(g->yoff + (g->yoff < 0 ? -0.5f : 0.5f)),
            .adv = (int)(g->xadvance + 0.5f),
        };
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: fontbake NAME SIZE font.{ttf,bdf} out.h [ranges]\n");
        return 1;
    }
    const char *name = argv[1];
    float size = (float)atof(argv[2]);
    const char *src = argv[3];
    /* ppem is the default; FONTBAKE_LINE=1 is the old ascent-descent
     * meaning. FONTBAKE_EM is accepted and ignored — it used to select
     * what is now the default. */
    int em_mode = getenv("FONTBAKE_LINE") == NULL;
    const char *hint = getenv("FONTBAKE_HINT");
    if (hint && (!*hint || !strcmp(hint, "0")))
        hint = NULL;
    if (hint && !strcmp(hint, "1"))
        hint = "full";
    parse_ranges(argc > 5 ? argv[5] : "32-126,8230");

    size_t sl = strlen(src);
    int is_bdf = sl >= 4 && strcasecmp(src + sl - 4, ".bdf") == 0;

    FILE *out = fopen(argv[4], "w");
    if (!out) {
        fprintf(stderr, "fontbake: cannot write %s\n", argv[4]);
        return 1;
    }
    /* A hinted bake is grid-fitted at a ppem, so FONTBAKE_LINE has no
     * meaning there — say so rather than quietly sizing by the wrong box. */
    if (hint && !is_bdf && !em_mode) {
        fprintf(stderr, "fontbake: FONTBAKE_HINT sizes by ppem; "
                        "FONTBAKE_LINE is not supported with it\n");
        return 1;
    }
    int ok = is_bdf         ? bake_bdf(src)
           : hint           ? bake_ft(src, size, hint)
                            : bake_ttf(src, size, em_mode);
    if (!ok)
        return 1;

    /* Two numbers, both measured before gamma/threshold, which would each
     * destroy the evidence.
     *
     * gray: share of inked pixels with partial coverage. ~0% means the
     * glyphs landed on whole pixels and this bake is a genuine bitmap.
     *
     * solid: share at 7/8 coverage or more — ink rather than fog. This is
     * the one to watch on an outline face, because gray stays high on a
     * hinted bake: the stems go solid but their AA sidebands are still
     * partial, so gray barely moves while the font gets dramatically
     * crisper. The cut is 224 and not 255 because the autohinter snaps a
     * stem's WIDTH to the grid without always landing its left edge
     * there, which leaves a 244 core beside a 32 spill — plainly ink, and
     * an exact-255 test would score it the same as an unhinted 131 smear.
     * Unhinted Roboto at 9 ppem scores solid 0.0%: not one pixel of real
     * ink in the whole atlas, which is what "fuzzy" looks like from the
     * inside. */
    long ink = 0, gray = 0, solid = 0;
    for (int j = 0; j < aw * ah; j++) {
        if (atlas[j]) {
            ink++;
            if (atlas[j] != 255)
                gray++;
            if (atlas[j] >= 224)
                solid++;
        }
    }
    double gray_pct = ink ? 100.0 * (double)gray / (double)ink : 0.0;
    double solid_pct = ink ? 100.0 * (double)solid / (double)ink : 0.0;

    /* FONTBAKE_GAMMA=0.55 -> raise AA coverage by pow(a, gamma), gamma<1.
     * stb_truetype emits linear coverage; on a non-linear panel thin stems
     * land near 50% alpha and read gray/wispy. Boosting mid alphas keeps
     * the edges but renders small text at full brightness. */
    const char *ge = getenv("FONTBAKE_GAMMA");
    if (ge) {
        double gamma = atof(ge);
        if (gamma > 0.0)
            for (int j = 0; j < aw * ah; j++)
                atlas[j] = (unsigned char)(pow(atlas[j] / 255.0, gamma) * 255.0 + 0.5);
    }

    /* FONTBAKE_THRESHOLD=1 -> 1-bit atlas (no antialiasing): crisp
     * bitmap-font look, every pixel fully on or off.
     *
     * FONTBAKE_THRESHOLD_CUT=N moves the coverage cut (default 128).
     * It matters at small sizes: stb_truetype does not hint, so a stem
     * narrower than a pixel can land at ~40% coverage in every cell it
     * crosses and disappear entirely at the halfway cut, shredding the
     * font. Lowering the cut fattens strokes back to solid. Anything
     * below ~64 starts closing counters in 'e' and 'a'. */
    if (getenv("FONTBAKE_THRESHOLD")) {
        const char *ce = getenv("FONTBAKE_THRESHOLD_CUT");
        int cut = ce ? atoi(ce) : 128;
        if (cut < 1) cut = 1;
        if (cut > 255) cut = 255;
        for (int j = 0; j < aw * ah; j++)
            atlas[j] = atlas[j] >= cut ? 255 : 0;
    }

    int m_adv = 0;
    for (int i = 0; i < nglyphs; i++)
        if (glyphs[i].cp == 'M')
            m_adv = glyphs[i].adv;
    int line_h = ascent_px - descent_px + gap_px;
    const char *szdesc = is_bdf ? "bdf" : (em_mode ? "ppem" : "linepx");
    double szval = is_bdf ? (double)line_h : (double)size;
    char mode[32];
    snprintf(mode, sizeof mode, "%s", hint && !is_bdf ? hint : "unhinted");

    const char *op = argv[4];
    int is_py = strlen(op) >= 3 && strcmp(op + strlen(op) - 3, ".py") == 0;

    if (is_py) {
        /* runtime-loadable blob (surf_font_from_blob): little-endian
         *   "SFN1", u16 w, u16 h, i16 asc, i16 desc, i16 gap, i16 0,
         *   u32 nglyphs, u32 nkerns, then atlas[w*h],
         *   glyphs[nglyphs]{u32 cp, i16 x,y,w,h,xoff,yoff,adv},
         *   kerns[nkerns]{u32 a, u32 b, i16 adv} */
        size_t sz = 24 + (size_t)aw * ah + (size_t)nglyphs * 18 + (size_t)nkern * 10;
        uint8_t *blob = malloc(sz), *w = blob;
        #define PU16(v) do { *w++ = (uint8_t)(v); *w++ = (uint8_t)((v) >> 8); } while (0)
        #define PU32(v) do { PU16((v) & 0xffff); PU16(((uint32_t)(v)) >> 16); } while (0)
        memcpy(w, "SFN1", 4); w += 4;
        PU16(aw); PU16(ah);
        PU16((uint16_t)ascent_px); PU16((uint16_t)descent_px);
        PU16((uint16_t)gap_px); PU16(0);
        PU32((uint32_t)nglyphs); PU32((uint32_t)nkern);
        memcpy(w, atlas, (size_t)aw * ah); w += (size_t)aw * ah;
        for (int i = 0; i < nglyphs; i++) {
            const bglyph *g = &glyphs[i];
            PU32(g->cp);
            PU16((uint16_t)g->x); PU16((uint16_t)g->y);
            PU16((uint16_t)g->w); PU16((uint16_t)g->h);
            PU16((uint16_t)g->xoff); PU16((uint16_t)g->yoff);
            PU16((uint16_t)g->adv);
        }
        for (int i = 0; i < nkern; i++) { PU32(ka[i]); PU32(kb[i]); PU16((uint16_t)kd[i]); }

        fprintf(out, "# Generated by tools/fontbake.c — do not edit. "
                "%s %.1f%s %s, cell %dx%d\n",
                name, szval, szdesc, mode, m_adv, line_h);
        fprintf(out, "FONT = (");
        for (size_t i = 0; i < sz; i += 16) {
            fprintf(out, "\n    b'");
            for (size_t j = i; j < i + 16 && j < sz; j++)
                fprintf(out, "\\x%02x", blob[j]);
            fprintf(out, "'");
        }
        fprintf(out, "\n)\n");
        fprintf(stderr, "fontbake: %s %.1f%s %s -> blob %zu bytes, "
                "cell %dx%d, gray %.1f%% solid %.1f%%\n",
                name, szval, szdesc, mode, sz, m_adv, line_h,
                gray_pct, solid_pct);
        return 0;
    }

    fprintf(out, "/* Generated by tools/fontbake.c — do not edit. "
           "%s %.1f%s %s from %s */\n",
           name, szval, szdesc, mode, src);
    fprintf(out, "#include \"surfer.h\"\n\n");

    fprintf(out, "static const uint8_t surf_font_%s_px[%d] = {\n", name, aw * ah);
    for (int i = 0; i < aw * ah; i += 24) {
        fprintf(out, "    ");
        for (int j = i; j < i + 24 && j < aw * ah; j++)
            fprintf(out, "%d,", atlas[j]);
        fprintf(out, "\n");
    }
    fprintf(out, "};\n\n");

    fprintf(out, "static const surf_glyph surf_font_%s_glyphs[%d] = {\n", name, nglyphs);
    for (int i = 0; i < nglyphs; i++) {
        const bglyph *g = &glyphs[i];
        fprintf(out, "    {%u, %d, %d, %d, %d, %d, %d, %d},\n", g->cp,
                g->x, g->y, g->w, g->h, g->xoff, g->yoff, g->adv);
    }
    fprintf(out, "};\n\n");

    fprintf(out, "static const surf_kern surf_font_%s_kerns[] = {\n", name);
    for (int i = 0; i < nkern; i++)
        fprintf(out, "    {%u, %u, %d},\n", ka[i], kb[i], kd[i]);
    fprintf(out, "    {0, 0, 0},\n};\n\n");  /* keep the array non-empty */

    fprintf(out, "static const surf_font surf_font_%s = {\n", name);
    fprintf(out, "    .atlas = {.pixels = (void *)surf_font_%s_px, .w = %d, .h = %d,\n"
           "              .stride = %d, .format = SURF_FMT_A8, .opaque = false},\n",
           name, aw, ah, aw);
    fprintf(out, "    .ascent = %d, .descent = %d, .line_gap = %d,\n",
           ascent_px, descent_px, gap_px);
    fprintf(out, "    .glyphs = surf_font_%s_glyphs, .nglyphs = %d,\n", name, nglyphs);
    fprintf(out, "    .kerns = surf_font_%s_kerns, .nkerns = %d,\n", name, nkern);
    fprintf(out, "};\n");

    fprintf(stderr, "fontbake: %-10s %5.1f%-6s %-9s -> %dx%d atlas, %d glyphs, "
            "cell %2dx%-2d, gray %5.1f%% solid %5.1f%%\n",
            name, szval, szdesc, mode, aw, ah, nglyphs, m_adv, line_h,
            gray_pct, solid_pct);
    return 0;
}
