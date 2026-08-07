/* Runtime images: PNG → ARGB8888 surf_image, pixels from hal->alloc_image.
 * Decode happens at load time, never in the frame path — a sprite is
 * still a pre-rendered asset, it just arrived after boot. */
#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_FAILURE_STRINGS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb/stb_image.h"
#pragma GCC diagnostic pop

/* ...and the other direction. An image can be DRAWN INTO — by the shape
 * API, by a caller writing its own pixels through the buffer protocol —
 * and until now there was no way to get one back out as a file, so
 * anything that made a picture could show it and never save it.
 *
 * IT HAS TO BE C, and that was measured rather than assumed. The same
 * encoder in MicroPython costs 8 ms for a 320x48 strip on a DESKTOP and
 * 43 ms for 704x64, against a board that runs Python-heavy loops 20-60x
 * slower — and it does not merely get slow, it hits a wall: the pure
 * Python path has to build the whole raw image as one bytearray before
 * deflating, which for a 2556x284 sprite sheet is 2.9 MB and died with
 * MemoryError on the laptop, let alone the panel.
 *
 * STBI_WRITE_NO_STDIO, so no fopen is linked on a device that has no
 * filesystem in C: the only entry point compiled is the to-memory one,
 * which is the one a binding wants anyway. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb/stb_image_write.h"
#pragma GCC diagnostic pop

/* Every format this library holds, widened to the RGBA8 stb wants.
 * A8 becomes white-with-that-alpha rather than black: an A8 image is a
 * MASK whose colour lives in the node's tint, and baking the tint in
 * would save a picture nobody drew. */
static unsigned char *to_rgba8(const surf_image *img)
{
    size_t n = (size_t)img->w * img->h;
    unsigned char *out = (unsigned char *)malloc(n * 4);
    if (!out)
        return NULL;
    for (int y = 0; y < img->h; y++) {
        const uint8_t *row = (const uint8_t *)img->pixels + (size_t)y * img->stride;
        unsigned char *d = out + (size_t)y * img->w * 4;
        for (int x = 0; x < img->w; x++, d += 4) {
            if (img->format == SURF_FMT_ARGB8888) {
                uint32_t p = ((const uint32_t *)row)[x];
                d[0] = (unsigned char)((p >> 16) & 0xff);
                d[1] = (unsigned char)((p >> 8) & 0xff);
                d[2] = (unsigned char)(p & 0xff);
                d[3] = (unsigned char)((p >> 24) & 0xff);
            } else if (img->format == SURF_FMT_A8) {
                d[0] = d[1] = d[2] = 0xff;
                d[3] = row[x];
            } else {
                uint16_t c = ((const uint16_t *)row)[x];
                /* 565 -> 888 with the top bits replicated into the low
                 * ones, so full-scale stays full-scale: 0x1f must come
                 * back as 255 and not 248. */
                unsigned char r = (unsigned char)((c >> 11) & 0x1f);
                unsigned char g = (unsigned char)((c >> 5) & 0x3f);
                unsigned char b = (unsigned char)(c & 0x1f);
                d[0] = (unsigned char)((r << 3) | (r >> 2));
                d[1] = (unsigned char)((g << 2) | (g >> 4));
                d[2] = (unsigned char)((b << 3) | (b >> 2));
                d[3] = 0xff;
            }
        }
    }
    return out;
}

void *surf_image_to_png(const surf_image *img, size_t *len)
{
    if (len)
        *len = 0;
    if (!img || !img->pixels || img->w <= 0 || img->h <= 0)
        return NULL;
    unsigned char *rgba = to_rgba8(img);
    if (!rgba)
        return NULL;
    int n = 0;
    unsigned char *png = stbi_write_png_to_mem(rgba, img->w * 4, img->w,
                                               img->h, 4, &n);
    free(rgba);
    if (!png || n <= 0) {
        free(png);
        return NULL;
    }
    if (len)
        *len = (size_t)n;
    return png;
}

void surf_image_png_free(void *png)
{
    free(png);
}

surf_image *surf_image_from_png(const void *data, size_t len)
{
    if (!surf_g.hal)
        return NULL;
    int w, h, comp;
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!rgba)
        return NULL;

    surf_image *img = malloc(sizeof *img);
    if (!img) {
        stbi_image_free(rgba);
        return NULL;
    }
    int32_t stride = ((int32_t)w * 4 + 63) & ~63;  /* device: 64B rows */
    uint8_t *px = surf_g.hal ? surf_g.hal->alloc_image((size_t)stride * h)
                            : NULL;
    if (!px) {
        stbi_image_free(rgba);
        free(img);
        return NULL;
    }

    bool opaque = true;
    for (int y = 0; y < h; y++) {
        const unsigned char *s = rgba + (size_t)y * w * 4;
        uint32_t *d = (uint32_t *)(px + (size_t)y * stride);
        for (int x = 0; x < w; x++) {
            uint8_t r = s[x * 4], g = s[x * 4 + 1], b = s[x * 4 + 2], a = s[x * 4 + 3];
            if (a != 255)
                opaque = false;
            d[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                   ((uint32_t)g << 8) | b;
        }
    }
    stbi_image_free(rgba);

    *img = (surf_image){
        .pixels = px,
        .w = (int16_t)w,
        .h = (int16_t)h,
        .stride = stride,
        .format = SURF_FMT_ARGB8888,
        .opaque = opaque,
    };
    return img;
}

surf_image *surf_image_from_png_a8(const void *data, size_t len)
{
    if (!surf_g.hal)
        return NULL;
    int w, h, comp;
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!rgba)
        return NULL;
    surf_image *img = malloc(sizeof *img);
    int32_t stride = ((int32_t)w + 63) & ~63;
    uint8_t *px = img ? surf_g.hal->alloc_image((size_t)stride * h) : NULL;
    if (!px) {
        stbi_image_free(rgba);
        free(img);
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        const unsigned char *s = rgba + (size_t)y * w * 4;
        uint8_t *d = px + (size_t)y * stride;
        for (int x = 0; x < w; x++)
            d[x] = s[x * 4 + 3];
    }
    stbi_image_free(rgba);
    *img = (surf_image){
        .pixels = px,
        .w = (int16_t)w,
        .h = (int16_t)h,
        .stride = stride,
        .format = SURF_FMT_A8,
        .opaque = false,
        .tint = 0xffff,
    };
    return img;
}

/* ---- collision ink -----------------------------------------------------
 * A lazy 1-bit mask per image: bit set where alpha >= SURF_INK_ALPHA.
 * surf_node_overlaps() reads it so a sprite's transparent corners do not
 * collide. Built on the first overlap that needs it, dropped when the
 * pixels change, rebuilt on the next ask.
 *
 * A SIDE TABLE rather than fields on surf_image, and that is not
 * tidiness: images are legitimately `static const` — the unit tests
 * build them that way and baked assets could — and on the device a
 * const struct lives in flash, where a lazy cache writing into it is a
 * store fault. The table is a handful of entries walked linearly; only
 * images somebody collides with are ever in it.
 *
 * The one hole is a caller that writes pixels through the buffer
 * protocol and never calls flush(): its mask goes stale. That caller
 * already gets stale PIXELS on the P4 (the PPA reads memory the CPU
 * left in cache), so this is the same contract, not a new one. */

typedef struct {
    const surf_image *img;
    uint32_t *bits;    /* words_per_row * h, LSB-first; NULL for solid */
    uint8_t   state;   /* 1 = mask in bits, 2 = solid (box is right) */
} ink_ent;

static struct { ink_ent *v; int n, cap; } ink_tab;

static ink_ent *ink_find(const surf_image *img)
{
    for (int i = 0; i < ink_tab.n; i++)
        if (ink_tab.v[i].img == img)
            return &ink_tab.v[i];
    return NULL;
}

static void ink_ent_free(ink_ent *e)
{
    free(e->bits);
    *e = ink_tab.v[--ink_tab.n];       /* swap-remove */
}

void surf_ink_dirty(const surf_image *img)
{
    ink_ent *e = ink_find(img);
    if (e)
        ink_ent_free(e);
}

void surf_ink_drop(const surf_image *img)
{
    surf_ink_dirty(img);
}

void surf_ink_reset(void)
{
    for (int i = 0; i < ink_tab.n; i++)
        free(ink_tab.v[i].bits);
    free(ink_tab.v);
    ink_tab.v = NULL;
    ink_tab.n = ink_tab.cap = 0;
}

const uint32_t *surf_ink(const surf_image *img, int32_t *words_per_row)
{
    if (!img || !img->pixels || img->w <= 0 || img->h <= 0)
        return NULL;
    /* no transparency to speak of: the box is already the truth. A1 is
     * storage-only and never a sprite source, but answer box, not crash. */
    if (img->opaque || img->format == SURF_FMT_RGB565 ||
        img->format == SURF_FMT_A1)
        return NULL;

    ink_ent *e = ink_find(img);
    if (!e) {
        if (ink_tab.n == ink_tab.cap) {
            int cap = ink_tab.cap ? ink_tab.cap * 2 : 8;
            ink_ent *v = realloc(ink_tab.v, (size_t)cap * sizeof *v);
            if (!v)
                return NULL;               /* OOM: fall back to the box */
            ink_tab.v = v;
            ink_tab.cap = cap;
        }
        e = &ink_tab.v[ink_tab.n++];
        e->img = img;
        e->state = 2;
        int32_t wpr = ((int32_t)img->w + 31) >> 5;
        uint32_t *bits = calloc((size_t)wpr * img->h, sizeof(uint32_t));
        if (bits) {
            const uint8_t *px = img->pixels;
            bool any_clear = false;
            for (int32_t y = 0; y < img->h; y++) {
                uint32_t *row = bits + (size_t)y * wpr;
                for (int32_t x = 0; x < img->w; x++) {
                    uint8_t a;
                    if (img->format == SURF_FMT_ARGB8888)
                        a = (uint8_t)(((const uint32_t *)
                                       (px + (size_t)y * img->stride))[x] >> 24);
                    else                   /* A8 */
                        a = px[(size_t)y * img->stride + x];
                    if (a >= SURF_INK_ALPHA)
                        row[x >> 5] |= 1u << (x & 31);
                    else
                        any_clear = true;
                }
            }
            if (any_clear) {
                e->bits = bits;
                e->state = 1;
            } else {
                free(bits);                /* every pixel is ink: solid */
                e->bits = NULL;
            }
        } else {
            e->bits = NULL;                /* OOM: remember it as solid */
        }
    }
    if (e->state != 1)
        return NULL;
    if (words_per_row)
        *words_per_row = ((int32_t)img->w + 31) >> 5;
    return e->bits;
}

void surf_image_destroy(surf_image *img)
{
    if (!img)
        return;
    surf_ink_drop(img);
    if (img->pixels && surf_g.hal)
        surf_g.hal->free_image(img->pixels);
    free(img);
}

/* ---- 1-bit atlas expansion (load time; see SURF_FMT_A1) ---- */

bool surf_image_expand_a1(surf_image *img)
{
    if (!img || img->format != SURF_FMT_A1)
        return true;                       /* nothing to do */

    const uint8_t *src = img->pixels;
    int16_t w = img->w, h = img->h;
    int32_t src_stride = img->stride;
    /* 64-byte stride, the PPA rule: this buffer is blended straight from
     * on the device, and the atlas is the caller's `stride` from here on. */
    int32_t stride = ((int32_t)w + 63) & ~63;
    uint8_t *px = surf_g.hal ? surf_g.hal->alloc_image((size_t)stride * h)
                            : NULL;
    if (!px) {
        /* NEVER leave it as A1. The hal has no bytes-per-pixel for the
         * format, so an A1 atlas that reaches a blit is read as A8 and
         * draws its own bit-pattern as alpha — text comes out as
         * coloured noise, which is how the init ordering bug below
         * showed up. Zero size draws nothing, which is wrong but
         * obviously wrong. Reached when surf_g.hal is not up yet:
         * surfer.init() is the session boundary and this needs its
         * allocator, so nothing may expand before it. */
        img->format = SURF_FMT_A8;
        img->w = img->h = 0;
        img->stride = 0;
        return false;
    }
    for (int16_t y = 0; y < h; y++) {
        const uint8_t *sr = src + (size_t)y * src_stride;
        uint8_t *dr = px + (size_t)y * stride;
        for (int16_t x = 0; x < w; x++)
            dr[x] = (sr[x >> 3] & (0x80u >> (x & 7))) ? 255 : 0;
        for (int32_t x = w; x < stride; x++)
            dr[x] = 0;
    }
    img->pixels = px;
    img->stride = stride;
    img->format = SURF_FMT_A8;
    return true;
}

/* ---- load-time composition (never per frame) ---- */

/* Both constructors, because they differ in one allocator call. `fast`
 * asks the hal for memory near the CPU and takes the ordinary kind when
 * there is none or it is full -- see surf_image_new_fast in surfer.h. */
static surf_image *image_new(int16_t w, int16_t h, surf_format format,
                             bool fast)
{
    if (!surf_g.hal || w <= 0 || h <= 0 || format > SURF_FMT_A8)
        return NULL;
    int bpp = format == SURF_FMT_ARGB8888 ? 4 : format == SURF_FMT_A8 ? 1 : 2;
    int32_t stride = ((int32_t)w * bpp + 63) & ~63;
    uint8_t *px = NULL;
    if (fast && surf_g.hal->alloc_image_fast)
        px = surf_g.hal->alloc_image_fast((size_t)stride * h);
    if (!px)
        px = surf_g.hal->alloc_image((size_t)stride * h);
    surf_image *img = malloc(sizeof *img);
    if (!px || !img) {
        if (px) surf_g.hal->free_image(px);
        free(img);
        return NULL;
    }
    memset(px, 0, (size_t)stride * h);  /* 565: black; ARGB: transparent */
    *img = (surf_image){
        .pixels = px, .w = w, .h = h, .stride = stride,
        .format = (uint8_t)format,
        .opaque = format == SURF_FMT_RGB565,
        .tint = 0xffff,   /* A8 masks start white */
    };
    return img;
}

surf_image *surf_image_new(int16_t w, int16_t h, surf_format format)
{
    return image_new(w, h, format, false);
}

surf_image *surf_image_new_fast(int16_t w, int16_t h, surf_format format)
{
    return image_new(w, h, format, true);
}

/* Scale src into dst -- the hal's 2D engine if it has one, a bilinear
 * loop if not. See surfer.h for why this is the image-to-image op worth
 * a hook: down-then-up is a blur, and on hardware it is free.
 *
 * The CPU path is bilinear rather than nearest DELIBERATELY, because it
 * has to match what the hardware does: a nearest downscale-then-upscale
 * gives blocks, not blur, so the same code would produce two different
 * effects depending on the backend. */
bool surf_image_scale(surf_image *dst, const surf_image *src)
{
    if (!dst || !src || dst->format != src->format ||
        dst->w <= 0 || dst->h <= 0 || src->w <= 0 || src->h <= 0)
        return false;
    surf_ink_dirty(dst);

    if (surf_g.hal && surf_g.hal->scale_image &&
        surf_g.hal->scale_image(dst, src))
        return true;

    if (src->format != SURF_FMT_RGB565)
        return false;                 /* CPU path is 565 only, for now */

    const float sx = (float)src->w / (float)dst->w;
    const float sy = (float)src->h / (float)dst->h;
    for (int y = 0; y < dst->h; y++) {
        float fy = ((float)y + 0.5f) * sy - 0.5f;
        int y0 = (int)(fy < 0 ? 0 : fy);
        int y1 = y0 + 1 < src->h ? y0 + 1 : src->h - 1;
        int wy = (int)((fy - (float)y0) * 256.0f);
        if (wy < 0) wy = 0;
        if (wy > 256) wy = 256;
        uint16_t *o = (uint16_t *)((uint8_t *)dst->pixels + (size_t)y * dst->stride);
        const uint16_t *r0 = (const uint16_t *)((const uint8_t *)src->pixels +
                                                (size_t)y0 * src->stride);
        const uint16_t *r1 = (const uint16_t *)((const uint8_t *)src->pixels +
                                                (size_t)y1 * src->stride);
        for (int x = 0; x < dst->w; x++) {
            float fx = ((float)x + 0.5f) * sx - 0.5f;
            int x0 = (int)(fx < 0 ? 0 : fx);
            int x1 = x0 + 1 < src->w ? x0 + 1 : src->w - 1;
            int wx = (int)((fx - (float)x0) * 256.0f);
            if (wx < 0) wx = 0;
            if (wx > 256) wx = 256;
            uint16_t a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
            int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
            int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
            int cr = (c >> 11) & 31, cg = (c >> 5) & 63, cb = c & 31;
            int dr = (d >> 11) & 31, dg = (d >> 5) & 63, db = d & 31;
            int tr = ((ar * (256 - wx) + br * wx) * (256 - wy) +
                      (cr * (256 - wx) + dr * wx) * wy) >> 16;
            int tg = ((ag * (256 - wx) + bg * wx) * (256 - wy) +
                      (cg * (256 - wx) + dg * wx) * wy) >> 16;
            int tb = ((ab * (256 - wx) + bb * wx) * (256 - wy) +
                      (cb * (256 - wx) + db * wx) * wy) >> 16;
            o[x] = (uint16_t)((tr << 11) | (tg << 5) | tb);
        }
    }
    return false;                     /* it worked, but not in hardware */
}

/* Publish CPU writes to an image's pixels. See the note in surfer.h: this is
 * the one call standing between "renders into an image every frame" and the
 * P4's PPA reading a line the CPU only ever left in cache. Backends whose
 * blitter is the CPU leave sync_image NULL and this costs a branch. */
void surf_image_flush(const surf_image *img)
{
    if (!img || !img->pixels)
        return;
    surf_ink_dirty(img);   /* buffer-protocol writers publish here */
    if (surf_g.hal && surf_g.hal->sync_image)
        surf_g.hal->sync_image(img->pixels, (size_t)img->stride * img->h);
}

void surf_image_fill(surf_image *dst, surf_rect r, surf_color c)
{
    if (!dst || !dst->pixels)
        return;
    r = surf_rect_intersect(r, (surf_rect){0, 0, dst->w, dst->h});
    if (surf_rect_empty(r))
        return;
    surf_ink_dirty(dst);
    if (dst->format == SURF_FMT_RGB565) {
        for (int y = 0; y < r.h; y++) {
            uint16_t *row = (uint16_t *)((uint8_t *)dst->pixels +
                                         (r.y + y) * dst->stride) + r.x;
            for (int x = 0; x < r.w; x++)
                row[x] = c;
        }
    } else if (dst->format == SURF_FMT_ARGB8888) {
        uint32_t p = 0xff000000u |
                     ((uint32_t)((c >> 8) & 0xf8) << 16) |
                     ((uint32_t)((c >> 3) & 0xfc) << 8) |
                     (uint32_t)((c << 3) & 0xf8);
        for (int y = 0; y < r.h; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)dst->pixels +
                                         (r.y + y) * dst->stride) + r.x;
            for (int x = 0; x < r.w; x++)
                row[x] = p;
        }
    }
}

/* read any supported source pixel as (a, 0xRRGGBB) */
static uint32_t src_px(const surf_image *img, int x, int y, uint32_t *a)
{
    switch (img->format) {
    case SURF_FMT_ARGB8888: {
        uint32_t p = *(const uint32_t *)((const uint8_t *)img->pixels +
                                         y * img->stride + x * 4);
        *a = p >> 24;
        return p & 0xffffff;
    }
    case SURF_FMT_A8: {
        *a = *((const uint8_t *)img->pixels + y * img->stride + x);
        surf_color t = img->tint;
        return ((uint32_t)((t >> 8) & 0xf8) << 16) |
               ((uint32_t)((t >> 3) & 0xfc) << 8) |
               (uint32_t)((t << 3) & 0xf8);
    }
    default: {
        uint16_t p = *(const uint16_t *)((const uint8_t *)img->pixels +
                                         y * img->stride + x * 2);
        *a = 255;
        return ((uint32_t)((p >> 8) & 0xf8) << 16) |
               ((uint32_t)((p >> 3) & 0xfc) << 8) |
               (uint32_t)((p << 3) & 0xf8);
    }
    }
}

void surf_image_blit_rot(surf_image *dst, const surf_image *src,
                         surf_rect sr, int16_t x, int16_t y, uint8_t rot)
{
    rot &= 3;
    if (rot == 0) {
        surf_image_blit(dst, src, sr, x, y);
        return;
    }
    if (!dst || !src || !dst->pixels || !src->pixels)
        return;
    surf_ink_dirty(dst);
    sr = surf_rect_intersect(sr, (surf_rect){0, 0, src->w, src->h});
    int16_t ow = (rot & 1) ? sr.h : sr.w;   /* rotated footprint */
    int16_t oh = (rot & 1) ? sr.w : sr.h;
    for (int j = 0; j < oh; j++) {
        int16_t dyp = (int16_t)(y + j);
        if (dyp < 0 || dyp >= dst->h)
            continue;
        for (int i = 0; i < ow; i++) {
            int16_t dxp = (int16_t)(x + i);
            if (dxp < 0 || dxp >= dst->w)
                continue;
            int32_t ux, uy;   /* same CCW mapping as the hal xform */
            switch (rot) {
            case 1:  ux = oh - 1 - j; uy = i;            break;
            case 2:  ux = ow - 1 - i; uy = oh - 1 - j;   break;
            default: ux = j;          uy = ow - 1 - i;   break;
            }
            uint32_t a, rgb = src_px(src, sr.x + ux, sr.y + uy, &a);
            if (a == 0)
                continue;
            uint32_t argb = (a << 24) | rgb;
            surf_image one = {.pixels = &argb, .w = 1, .h = 1, .stride = 4,
                              .format = SURF_FMT_ARGB8888};
            surf_image_blit(dst, &one, (surf_rect){0, 0, 1, 1}, dxp, dyp);
        }
    }
}

void surf_image_blit(surf_image *dst, const surf_image *src, surf_rect sr,
                     int16_t x, int16_t y)
{
    if (!dst || !src || !dst->pixels || !src->pixels)
        return;
    surf_ink_dirty(dst);
    sr = surf_rect_intersect(sr, (surf_rect){0, 0, src->w, src->h});
    /* clip the destination, dragging the source window along */
    if (x < 0) { sr.x -= x; sr.w += x; x = 0; }
    if (y < 0) { sr.y -= y; sr.h += y; y = 0; }
    if (x + sr.w > dst->w) sr.w = dst->w - x;
    if (y + sr.h > dst->h) sr.h = dst->h - y;
    if (sr.w <= 0 || sr.h <= 0)
        return;

    for (int j = 0; j < sr.h; j++) {
        for (int i = 0; i < sr.w; i++) {
            uint32_t a, rgb = src_px(src, sr.x + i, sr.y + j, &a);
            if (a == 0)
                continue;
            uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
            if (dst->format == SURF_FMT_RGB565) {
                uint16_t *d = (uint16_t *)((uint8_t *)dst->pixels +
                                           (y + j) * dst->stride) + x + i;
                uint32_t dr = (uint32_t)((*d >> 8) & 0xf8) | (*d >> 13);
                uint32_t dg = (uint32_t)((*d >> 3) & 0xfc) | ((*d >> 9) & 0x03);
                uint32_t db = (uint32_t)((*d << 3) & 0xf8) | ((*d >> 2) & 0x07);
                uint32_t nr = (r * a + dr * (255 - a) + 127) / 255;
                uint32_t ng = (g * a + dg * (255 - a) + 127) / 255;
                uint32_t nb = (b * a + db * (255 - a) + 127) / 255;
                *d = (uint16_t)(((nr & 0xf8) << 8) | ((ng & 0xfc) << 3) | (nb >> 3));
            } else if (dst->format == SURF_FMT_A8) {  /* coverage-over */
                uint8_t *d = (uint8_t *)dst->pixels + (y + j) * dst->stride + x + i;
                *d = (uint8_t)(a + *d * (255 - a) / 255);
            } else {  /* ARGB dst: src-over */
                uint32_t *d = (uint32_t *)((uint8_t *)dst->pixels +
                                           (y + j) * dst->stride) + x + i;
                uint32_t da = *d >> 24;
                uint32_t dr = (*d >> 16) & 0xff, dg = (*d >> 8) & 0xff, db = *d & 0xff;
                uint32_t oa = a + da * (255 - a) / 255;
                uint32_t inv = da * (255 - a) / 255, den = oa ? oa : 1;
                uint32_t orr = (r * a + dr * inv) / den;
                uint32_t og = (g * a + dg * inv) / den;
                uint32_t ob = (b * a + db * inv) / den;
                if (orr > 255) orr = 255;   /* integer rounding can reach 256 */
                if (og > 255) og = 255;
                if (ob > 255) ob = 255;
                *d = (oa << 24) | (orr << 16) | (og << 8) | ob;
            }
        }
    }
}
