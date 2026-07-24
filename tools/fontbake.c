/* Build-time font baker (DESIGN.md §2.5): turns a font into an A8 atlas +
 * advance/kerning tables, emitted as a C header (or a runtime-loadable
 * blob). Runtime never rasterizes — text drawing is atlas blits.
 *
 *   fontbake NAME SIZE font.ttf out.h [ranges]
 *   fontbake NAME 0    font.bdf out.h [ranges]
 *
 * Two front ends feeding one emitter:
 *   .ttf/.otf  outlines rasterized by stb_truetype at SIZE
 *   .bdf       a designed bitmap font, copied pixel-for-pixel. SIZE is
 *              ignored — a BDF *is* one specific size, which is the whole
 *              point: helvR10 and helvR12 are separately drawn faces, not
 *              one outline scaled. No AA is possible by construction.
 *
 * ranges: comma-separated codepoint spans, e.g. "32-126,8230".
 * Default: ASCII 32-126 plus U+2026 (ellipsis, needed for ellipsize).
 *
 * Env knobs (TTF only unless noted; all off by default):
 *   FONTBAKE_EM=1            size means em-pixels (ppem), not cap height.
 *                            REQUIRED for pixel-designed outline fonts:
 *                            their grid is defined in em units, so only
 *                            an exact ppem lands stems on whole pixels.
 *                            Default sizing is ScaleForPixelHeight
 *                            (size = ascent-descent), right for outlines.
 *   FONTBAKE_GAMMA=0.55      boost AA alpha (gamma<1 = brighter small text)
 *   FONTBAKE_THRESHOLD=1     1-bit atlas, no antialiasing
 *   FONTBAKE_THRESHOLD_CUT=N on/off cut for 1-bit mode (default 128,
 *                            lower = bolder)
 *
 * The summary line reports "gray" — the share of inked pixels that are
 * neither 0 nor 255. A BDF is always 0.0%. For an outline face it is ~0
 * only at a grid-aligned ppem; anything above a few % means the size is
 * off the grid (or the face was drawn with curves), and thresholding it
 * will look lumpy.
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
    int xoff, yoff;     /* pen-relative, yoff measured down from the line top */
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

/* ---- BDF front end ---- */

typedef struct {
    uint32_t cp;
    int w, h, xoff, yoff_bdf, adv;
    unsigned char *bits;   /* w*h, 0 or 255 */
} bdfglyph;

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
static int pack(bdfglyph *g, int n)
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
    static bdfglyph g[MAX_CPS];
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
        bdfglyph t = g[i];
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
    int em_mode = getenv("FONTBAKE_EM") != NULL;
    parse_ranges(argc > 5 ? argv[5] : "32-126,8230");

    size_t sl = strlen(src);
    int is_bdf = sl >= 4 && strcasecmp(src + sl - 4, ".bdf") == 0;

    FILE *out = fopen(argv[4], "w");
    if (!out) {
        fprintf(stderr, "fontbake: cannot write %s\n", argv[4]);
        return 1;
    }
    if (!(is_bdf ? bake_bdf(src) : bake_ttf(src, size, em_mode)))
        return 1;

    /* How much of the ink is partial coverage? ~0% means the glyphs
     * landed on whole pixels and this bake is a genuine bitmap. Measured
     * before gamma/threshold, which would both destroy the evidence. */
    long ink = 0, gray = 0;
    for (int j = 0; j < aw * ah; j++) {
        if (atlas[j]) {
            ink++;
            if (atlas[j] != 255)
                gray++;
        }
    }
    double gray_pct = ink ? 100.0 * (double)gray / (double)ink : 0.0;

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
    const char *szdesc = is_bdf ? "bdf" : (em_mode ? "em" : "px");
    double szval = is_bdf ? (double)line_h : (double)size;

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

        fprintf(out, "# Generated by tools/fontbake.c — do not edit. %s %.1f%s, cell %dx%d\n",
                name, szval, szdesc, m_adv, line_h);
        fprintf(out, "FONT = (");
        for (size_t i = 0; i < sz; i += 16) {
            fprintf(out, "\n    b'");
            for (size_t j = i; j < i + 16 && j < sz; j++)
                fprintf(out, "\\x%02x", blob[j]);
            fprintf(out, "'");
        }
        fprintf(out, "\n)\n");
        fprintf(stderr, "fontbake: %s %.1f%s -> blob %zu bytes, cell %dx%d, gray %.1f%%\n",
                name, szval, szdesc, sz, m_adv, line_h, gray_pct);
        return 0;
    }

    fprintf(out, "/* Generated by tools/fontbake.c — do not edit. %s %.1f%s from %s */\n",
           name, szval, szdesc, src);
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

    fprintf(stderr, "fontbake: %s %.1f%s -> %dx%d atlas, %d glyphs, "
            "cell %dx%d (M adv x line_h), gray %.1f%%\n",
            name, szval, szdesc, aw, ah, nglyphs, m_adv, line_h, gray_pct);
    return 0;
}
