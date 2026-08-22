/* Label node: a string drawn as A8 atlas blits. Layout happens during
 * paint via the shared walker — the cost is the blits themselves, and a
 * damaged label repaints only its dirty intersection. */
#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

/* strdup is POSIX, not C11 */
static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* node w/h drive damage and hit test; keep them true to the layout */
static void text_update_bounds(surf_node *n)
{
    const surf_font *f = n->u.text.font;
    if ((n->u.text.tflags & SURF_TF_ELLIPSIS) && n->u.text.wrap_w > 0) {
        n->w = n->u.text.wrap_w;
        n->h = surf_font_line_h(f);
        return;
    }
    surf_point sz = surf_text_measure(f, n->u.text.str, n->u.text.wrap_w);
    n->w = (n->u.text.wrap_w > 0) ? n->u.text.wrap_w : sz.x;
    n->h = sz.y;
}

surf_node *surf_text_new(const surf_font *f, const char *str,
                         int16_t x, int16_t y, surf_color c)
{
    if (!f)
        return NULL;
    surf_node *n = surf_node_alloc(SURF_NODE_TEXT);
    if (!n)
        return NULL;
    n->x = x;
    n->y = y;
    n->u.text.font = f;
    n->u.text.str = str ? dup_str(str) : NULL;
    n->u.text.img = f->atlas;
    n->u.text.img.tint = c;
    text_update_bounds(n);
    return n;
}

void surf_text_set(surf_node *n, const char *str)
{
    if (!n || n->type != SURF_NODE_TEXT)
        return;
    surf_damage_subtree(n);
    free(n->u.text.str);
    n->u.text.str = str ? dup_str(str) : NULL;
    text_update_bounds(n);
    surf_damage_subtree(n);
}

void surf_text_set_color(surf_node *n, surf_color c)
{
    if (!n || n->type != SURF_NODE_TEXT || n->u.text.img.tint == c)
        return;
    n->u.text.img.tint = c;
    surf_damage_subtree(n);
}

void surf_text_set_wrap(surf_node *n, int16_t wrap_w)
{
    if (!n || n->type != SURF_NODE_TEXT || n->u.text.wrap_w == wrap_w)
        return;
    surf_damage_subtree(n);
    n->u.text.wrap_w = wrap_w;
    text_update_bounds(n);
    surf_damage_subtree(n);
}

void surf_text_set_align(surf_node *n, surf_align a)
{
    if (!n || n->type != SURF_NODE_TEXT || n->u.text.align == (uint8_t)a)
        return;
    n->u.text.align = (uint8_t)a;
    surf_damage_subtree(n);
}

void surf_text_set_ellipsis(surf_node *n, bool on)
{
    if (!n || n->type != SURF_NODE_TEXT)
        return;
    uint8_t tf = on ? (uint8_t)(n->u.text.tflags | SURF_TF_ELLIPSIS)
                    : (uint8_t)(n->u.text.tflags & ~SURF_TF_ELLIPSIS);
    if (tf == n->u.text.tflags)
        return;
    surf_damage_subtree(n);
    n->u.text.tflags = tf;
    text_update_bounds(n);
    surf_damage_subtree(n);
}

/* Which image a laid-out glyph's pixels are actually in.
 *
 * Normally the node's own copy of its face's atlas header, which is what
 * carries the caller's colour as a tint. A glyph that came from the
 * FALLBACK face lives in a different atlas, and whether the colour still
 * applies depends on what kind of face that is: a mask (A8) is tinted
 * like anything else, a picture (an emoji, ARGB) already has its own
 * colours and the caller's would mean nothing. Copying the header is
 * free — it is six fields, and the pixels are shared.
 *
 * RETURNED BY VALUE, so callers blit from a stack temporary. That is
 * safe because a hal reads the descriptor during the blend call and
 * retains only the PIXELS (hal_p4 copies every field it needs into a
 * PPA config; hal_sdl reads them inline) — but it is the kind of thing
 * a future hal could quietly break, so: nothing may hold this pointer
 * past the call. */
surf_image surf_glyph_image(const surf_image *base, const surf_font *base_font,
                            const surf_font *from)
{
    if (!from || from == base_font)
        return *base;
    surf_image im = from->atlas;
    if (im.format == SURF_FMT_A8)
        im.tint = base->tint;
    return im;
}

/* shared by label and textinput paint: one glyph, clipped to vis */
void surf_glyph_blit(const surf_image *img, const surf_glyph *g,
                     int16_t dx, int16_t dy, surf_rect vis, uint8_t opa)
{
    surf_rect dst = {dx, dy, g->w, g->h};
    surf_rect v = surf_rect_intersect(dst, vis);
    if (surf_rect_empty(v))
        return;
    surf_rect src = {
        (int16_t)(g->x + (v.x - dx)), (int16_t)(g->y + (v.y - dy)), v.w, v.h,
    };
    surf_g.hal->blend(img, src, (surf_point){v.x, v.y}, opa);
}

void surf_text_paint(const surf_paint_ent *e)
{
    surf_node *n = e->n;
    if (!n->u.text.str)
        return;
    surf_tlayout it;
    surf_tglyph tg;
    surf_tlayout_begin(&it, n->u.text.font, n->u.text.str, n->u.text.wrap_w,
                       n->u.text.align, n->u.text.tflags);
    while (surf_tlayout_next(&it, &tg)) {
        if (tg.g->w <= 0)
            continue;  /* spaces advance the pen, nothing to blit */
        surf_image im = surf_glyph_image(&n->u.text.img, n->u.text.font, tg.font);
        surf_glyph_blit(&im, tg.g,
                        (int16_t)(e->ax + tg.x), (int16_t)(e->ay + tg.y),
                        e->vis, n->opa);
    }
}

/* See surfer.h. The same layout walk as surf_text_paint, aimed at an
 * image instead of the framebuffer: surf_image_blit already composites
 * every atlas format this can meet (A8 with the tint carrying the
 * colour, ARGB for an emoji that keeps its own), and a fresh ARGB image
 * starts transparent, so blitting IS rendering. One-shot per call —
 * per-pixel work is legal here for the same reason it is in the shape
 * API: nothing on the frame path. */
surf_image *surf_text_bake(const surf_font *f, const char *str,
                           surf_color color, int16_t wrap_w)
{
    if (!f || !str)
        return NULL;
    surf_point sz = surf_text_measure(f, str, wrap_w);
    if (sz.x < 1)
        sz.x = 1;
    if (sz.y < 1)
        sz.y = 1;
    surf_image *img = surf_image_new((int16_t)sz.x, (int16_t)sz.y,
                                     SURF_FMT_ARGB8888);
    if (!img)
        return NULL;
    surf_image base = f->atlas;   /* the label's own arrangement: the */
    base.tint = color;            /* tint IS the colour on an A8 face */
    surf_tlayout it;
    surf_tglyph tg;
    surf_tlayout_begin(&it, f, str, wrap_w, SURF_ALIGN_LEFT, 0);
    while (surf_tlayout_next(&it, &tg)) {
        if (tg.g->w <= 0)
            continue;
        surf_image im = surf_glyph_image(&base, f, tg.font);
        surf_image_blit(img, &im,
                        (surf_rect){tg.g->x, tg.g->y, tg.g->w, tg.g->h},
                        (int16_t)tg.x, (int16_t)tg.y);
    }
    surf_image_flush(img);
    return img;
}

void surf_text_free_storage(surf_node *n)
{
    if (n->type == SURF_NODE_TEXT) {
        free(n->u.text.str);
        n->u.text.str = NULL;
    } else if (n->type == SURF_NODE_TEXTINPUT) {
        free(n->u.input.buf);
        n->u.input.buf = NULL;
    } else if (n->type == SURF_NODE_TEXTGRID) {
        free(n->u.grid.cells);
        n->u.grid.cells = NULL;
    }
}
