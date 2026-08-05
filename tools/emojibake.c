/* Build-time emoji baker: turns a directory of PNGs into a COLOUR glyph
 * atlas in exactly the shape fontbake emits, so an emoji set is an
 * ordinary `surf_font` and the text path needs no idea it is special.
 *
 *   emojibake NAME SIZE set.txt pngdir out.h [names.h]
 *
 * The one thing that IS different is the pixel format. Every other font
 * here is A8 — one coverage byte, tinted to whatever colour the label or
 * the cell asks for — because a letter is a shape and its colour belongs
 * to the caller. An emoji is not a shape, it is a PICTURE: the red of
 * the heart and the green of the check mark are the whole content, and
 * they are what make it legible at sizes where the shape has stopped
 * being. So the atlas is ARGB8888 and the caller's colour is ignored.
 *
 * That costs 4 bytes a pixel against A8's 1 and A1's 1/8, which is why
 * the set is curated (assets/emoji/set.txt says why) and why this is a
 * separate tool rather than a flag on fontbake: nothing about it shares
 * fontbake's rasterizers, its hinting, its kerning or its A1 packing.
 *
 * WHY PNG SOURCES AND NOT A COLOUR FONT. FreeType can render CBDT/sbix
 * colour glyphs, but every colour emoji font ships ONE bitmap strike
 * (Noto's is 136x128) and hands back a downscale of it — which is
 * exactly what this does, minus a 10 MB font file and a FreeType
 * dependency in a code path that would then only ever use it for
 * downscaling. Twemoji ships the frames as PNGs; stb_image is already
 * vendored for build tools.
 *
 * The downscale is BOX-FILTERED IN PREMULTIPLIED ALPHA, and that is not
 * a detail. Averaging straight RGBA weights the colour of fully
 * transparent pixels — which in Twemoji's PNGs is black — into every
 * edge, so a downscaled emoji comes out with a dark fringe all round it
 * and looks dirty at small sizes. Premultiply, average, un-premultiply.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb/stb_image.h"

#define MAX_EMOJI 1024
#define MAX_NAME  40

typedef struct {
    char     name[MAX_NAME];
    uint32_t cp;
    /* the trimmed bitmap, straight (un-premultiplied) RGBA */
    uint8_t *px;
    int      w, h, xoff, yoff;   /* yoff: down from the box top */
    int      ax, ay;             /* placement in the atlas */
} emoji;

static emoji set[MAX_EMOJI];
static int   nset;

static uint32_t *atlas;
static int aw = 128, ah = 128;

/* ---- box-downscale src (sw x sh, straight RGBA) into dw x dh ---- */
static uint8_t *downscale(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    uint8_t *out = malloc((size_t)dw * dh * 4);
    if (!out)
        return NULL;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            /* accumulate PREMULTIPLIED, or transparent black bleeds in */
            uint64_t r = 0, g = 0, b = 0, a = 0;
            long n = 0;
            for (int sy = y0; sy < y1 && sy < sh; sy++) {
                for (int sx = x0; sx < x1 && sx < sw; sx++) {
                    const uint8_t *p = src + ((size_t)sy * sw + sx) * 4;
                    r += (uint64_t)p[0] * p[3];
                    g += (uint64_t)p[1] * p[3];
                    b += (uint64_t)p[2] * p[3];
                    a += p[3];
                    n++;
                }
            }
            uint8_t *q = out + ((size_t)y * dw + x) * 4;
            if (!n || !a) {
                q[0] = q[1] = q[2] = q[3] = 0;
                continue;
            }
            q[0] = (uint8_t)(r / a);          /* un-premultiply */
            q[1] = (uint8_t)(g / a);
            q[2] = (uint8_t)(b / a);
            q[3] = (uint8_t)(a / (uint64_t)n);
        }
    }
    return out;
}

/* Drop fully transparent border rows/columns and record where the ink
 * started. The BOX stays SIZE x SIZE for every emoji — they have to be
 * uniform or a row of them jitters — so this is purely an atlas saving
 * and the offsets put each one back where it was. */
static void trim(emoji *e, int box)
{
    int x0 = box, y0 = box, x1 = -1, y1 = -1;
    for (int y = 0; y < box; y++) {
        for (int x = 0; x < box; x++) {
            if (e->px[((size_t)y * box + x) * 4 + 3] == 0)
                continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < 0) {                       /* nothing at all: an empty glyph */
        free(e->px);
        e->px = NULL;
        e->w = e->h = 0;
        e->xoff = e->yoff = 0;
        return;
    }
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    uint8_t *out = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        memcpy(out + (size_t)y * w * 4,
               e->px + ((size_t)(y + y0) * box + x0) * 4, (size_t)w * 4);
    free(e->px);
    e->px = out;
    e->w = w; e->h = h; e->xoff = x0; e->yoff = y0;
}

/* Shelf-pack, growing until it fits — fontbake's pack(), over 32-bit
 * pixels. Emission order stays ascending by codepoint because
 * surf_font_glyph binary searches the table. */
static int pack(void)
{
    for (;;) {
        free(atlas);
        atlas = calloc((size_t)aw * ah, 4);
        if (!atlas)
            return 0;
        int x = 0, y = 0, rowh = 0, ok = 1;
        for (int i = 0; i < nset; i++) {
            if (!set[i].px)
                continue;
            if (x + set[i].w > aw) { x = 0; y += rowh + 1; rowh = 0; }
            if (y + set[i].h > ah) { ok = 0; break; }
            for (int r = 0; r < set[i].h; r++) {
                for (int c = 0; c < set[i].w; c++) {
                    const uint8_t *p = set[i].px + ((size_t)r * set[i].w + c) * 4;
                    atlas[(size_t)(y + r) * aw + x + c] =
                        ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                        ((uint32_t)p[1] << 8)  |  (uint32_t)p[2];
                }
            }
            set[i].ax = x;
            set[i].ay = y;
            x += set[i].w + 1;
            if (set[i].h > rowh) rowh = set[i].h;
        }
        if (ok) {
            /* The doubling above finds a box that FITS; it does not find
             * a box that is full. At 385 emoji the last shelf ended 43%
             * of the way up a 512x256 and the rest was 220 KB of zeroes
             * in flash. Height is free to trim because rows are
             * independent — only the WIDTH is load-bearing (stride is
             * aw*4 and the device wants a 64-byte multiple, so aw stays
             * a multiple of 16). */
            int used = y + rowh;
            if (used > 0 && used < ah)
                ah = used;
            return 1;
        }
        if (ah < aw) ah *= 2; else aw *= 2;
        if (aw > 4096)
            return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "usage: emojibake NAME SIZE set.txt pngdir out.h [names.h]\n");
        return 1;
    }
    const char *name = argv[1];
    int size = atoi(argv[2]);
    const char *setpath = argv[3], *dir = argv[4];
    if (size < 4 || size > 256) {
        fprintf(stderr, "emojibake: SIZE %d out of range\n", size);
        return 1;
    }

    FILE *sf = fopen(setpath, "r");
    if (!sf) {
        fprintf(stderr, "emojibake: cannot open %s\n", setpath);
        return 1;
    }
    char line[256];
    int nmissing = 0;
    while (fgets(line, sizeof line, sf) && nset < MAX_EMOJI) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char nm[MAX_NAME]; char cps[32];
        if (sscanf(line, "%39s %31s", nm, cps) != 2)
            continue;
        uint32_t cp = (uint32_t)strtoul(cps, NULL, 16);
        if (!cp)
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%x.png", dir, (unsigned)cp);
        int w, h2, ch;
        uint8_t *img = stbi_load(path, &w, &h2, &ch, 4);
        if (!img) {
            fprintf(stderr, "emojibake: no art for %s (U+%04X) at %s\n",
                    nm, (unsigned)cp, path);
            nmissing++;
            continue;
        }
        emoji *e = &set[nset++];
        snprintf(e->name, sizeof e->name, "%s", nm);
        e->cp = cp;
        e->px = downscale(img, w, h2, size, size);
        stbi_image_free(img);
        if (!e->px) { fprintf(stderr, "emojibake: out of memory\n"); return 1; }
        trim(e, size);
    }
    fclose(sf);
    if (!nset) {
        fprintf(stderr, "emojibake: %s named no emoji with art\n", setpath);
        return 1;
    }
    if (nmissing) {
        /* Fail rather than warn, unlike fontbake's missing-codepoint case:
         * there a face genuinely may not carry a character, whereas here
         * the set file and the art directory are two halves of one asset
         * and a gap between them is a mistake every time. */
        fprintf(stderr, "emojibake: %d named emoji have no art\n", nmissing);
        return 1;
    }

    /* ascending by codepoint — surf_font_glyph binary searches it */
    for (int i = 1; i < nset; i++) {
        emoji t = set[i];
        int j = i - 1;
        while (j >= 0 && set[j].cp > t.cp) { set[j + 1] = set[j]; j--; }
        set[j + 1] = t;
    }
    for (int i = 1; i < nset; i++)
        if (set[i].cp == set[i - 1].cp)
            fprintf(stderr, "emojibake: warning: U+%04X listed twice "
                    "(%s, %s) — the second is unreachable\n",
                    (unsigned)set[i].cp, set[i - 1].name, set[i].name);

    if (!pack()) {
        fprintf(stderr, "emojibake: atlas won't fit\n");
        return 1;
    }

    FILE *out = fopen(argv[5], "w");
    if (!out) {
        fprintf(stderr, "emojibake: cannot write %s\n", argv[5]);
        return 1;
    }
    fprintf(out, "/* Generated by tools/emojibake.c — do not edit. "
                 "%s, %d emoji at %dpx, from %s */\n", name, nset, size, setpath);
    fprintf(out, "#include \"surfer.h\"\n\n");

    fprintf(out, "static const uint32_t surf_font_%s_px[%d] = {\n", name, aw * ah);
    for (int i = 0; i < aw * ah; i += 8) {
        fprintf(out, "    ");
        for (int j = i; j < i + 8 && j < aw * ah; j++)
            fprintf(out, "0x%08x,", atlas[j]);
        fprintf(out, "\n");
    }
    fprintf(out, "};\n\n");

    /* The emoji BOX sits with its bottom on the baseline, so an emoji
     * lines up with the text beside it the way a capital does. yoff
     * counts DOWN from the baseline, so the box top is -size and the
     * trimmed bitmap starts that far down plus whatever trim() ate. */
    fprintf(out, "static const surf_glyph surf_font_%s_glyphs[%d] = {\n",
            name, nset);
    for (int i = 0; i < nset; i++)
        fprintf(out, "    {%u, %d, %d, %d, %d, %d, %d, %d},\n",
                set[i].cp, set[i].ax, set[i].ay, set[i].w, set[i].h,
                set[i].xoff, -size + set[i].yoff, size);
    fprintf(out, "};\n\n");

    /* No kerning between pictures, but the array has to exist and be
     * non-empty for the same reason fontbake's sentinel does. */
    fprintf(out, "static const surf_kern surf_font_%s_kerns[] = {\n"
                 "    {0, 0, 0},\n};\n\n", name);

    fprintf(out, "static const surf_font surf_font_%s = {\n", name);
    fprintf(out, "    .atlas = {.pixels = (void *)surf_font_%s_px, .w = %d, .h = %d,\n"
                 "              .stride = %d, .format = SURF_FMT_ARGB8888,\n"
                 "              .opaque = false},\n", name, aw, ah, aw * 4);
    fprintf(out, "    .ascent = %d, .descent = 0, .line_gap = 0,\n", size);
    fprintf(out, "    .glyphs = surf_font_%s_glyphs, .nglyphs = %d,\n", name, nset);
    fprintf(out, "    .kerns = surf_font_%s_kerns, .nkerns = 0,\n", name);
    fprintf(out, "};\n");
    fclose(out);

    /* The name table is a property of the SET, not of any one size, so
     * it is emitted once by whichever bake asks for it rather than
     * duplicated into every size's header. */
    if (argc > 6) {
        FILE *nf = fopen(argv[6], "w");
        if (!nf) {
            fprintf(stderr, "emojibake: cannot write %s\n", argv[6]);
            return 1;
        }
        fprintf(nf, "/* Generated by tools/emojibake.c — do not edit. "
                    "%d names from %s */\n", nset, setpath);
        fprintf(nf, "#include <stdint.h>\n\n");
        fprintf(nf, "typedef struct { const char *name; uint32_t cp; } "
                    "surf_emoji_name;\n\n");
        /* sorted by NAME so the lookup can bisect it too */
        int *ord = malloc(sizeof(int) * (size_t)nset);
        for (int i = 0; i < nset; i++) ord[i] = i;
        for (int i = 1; i < nset; i++) {
            int t = ord[i], j = i - 1;
            while (j >= 0 && strcmp(set[ord[j]].name, set[t].name) > 0) {
                ord[j + 1] = ord[j]; j--;
            }
            ord[j + 1] = t;
        }
        fprintf(nf, "static const surf_emoji_name surf_emoji_names[%d] = {\n", nset);
        for (int i = 0; i < nset; i++)
            fprintf(nf, "    {\"%s\", %u},\n",
                    set[ord[i]].name, set[ord[i]].cp);
        fprintf(nf, "};\n");
        fprintf(nf, "#define SURF_EMOJI_NAMES %d\n", nset);
        free(ord);
        fclose(nf);
    }

    long bytes = (long)aw * ah * 4;
    fprintf(stderr, "%-12s %3d emoji  %dpx  atlas %dx%d  %ld KB argb\n",
            name, nset, size, aw, ah, bytes / 1024);
    return 0;
}
