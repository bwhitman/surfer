/* Core internals — not part of the binding surface. */
#ifndef SURF_INTERNAL_H
#define SURF_INTERNAL_H

#include "surfer.h"

/* 32: a screenful of independent movers (bullets, critters, overlays on
 * fast bands) plus slivers and smears must fit without degrading to the
 * bounding-union fallback — that fallback repaints the world (§5) */
#define SURF_MAX_DIRTY 32

enum {
    SURF_NODE_FREE = 0,
    SURF_NODE_GROUP,
    SURF_NODE_RECT,
    SURF_NODE_SPRITE,
    SURF_NODE_FILMSTRIP,
    SURF_NODE_NINEPATCH,
    SURF_NODE_TEXT,
    SURF_NODE_TEXTINPUT,
    SURF_NODE_SCROLLVIEW,
    SURF_NODE_TEXTGRID,
    SURF_NODE_LAYER,
};

typedef struct {
    uint32_t   cp;
    surf_color fg, bg;
} surf_textcell;

enum {
    SURF_NF_HIDDEN = 1u << 0,
    SURF_NF_CLIP   = 1u << 1,
    SURF_NF_FOCUS  = 1u << 2,  /* textinput: draw the caret */
    SURF_NF_GRAB   = 1u << 3,  /* scrollview may not steal my gestures */
};

enum {
    SURF_TF_ELLIPSIS = 1u << 0,
};

struct surf_node {
    uint8_t    type;
    uint8_t    flags;
    int16_t    x, y;   /* offset in parent */
    int16_t    w, h;   /* rect: size; sprite: src size; group: clip size */
    surf_node *parent;
    surf_node *first, *last;  /* children; last is painted frontmost */
    surf_node *prev, *next;   /* siblings; next doubles as free-list link */
    surf_touch_cb on_touch;
    void      *touch_user;
    union {
        struct { surf_color color; } rect;
        struct {
            const surf_image *img;
            surf_rect src;
            int32_t scale_q16;   /* SURF_ONE = 1:1 */
            uint8_t rot;         /* quarter turns CCW, 0..3 */
            uint8_t mirror;      /* bit0 = x flip, bit1 = y flip */
            bool fast_pan;       /* set_src rides band_shift (cameras) */
            bool pan_shifted;    /* a shift ran on the last src change */
        } sprite;
        struct {
            const surf_image *strip;
            int32_t off_q16;     /* scroll offset, wraps at strip->w */
            bool fast;           /* band_shift streaming (opaque strips) */
            bool shifted;        /* a shift ran last offset change */
        } layer;
        struct {
            const surf_image *img;
            int16_t fw, fh;      /* frame size; node w/h mirror these */
            int16_t frame, nframes, per_row;
            /* PLAYBACK, and it is optional: fps 0 means the caller owns
             * the frame — which is what an editor picking a cel wants,
             * and what a game stepping a walk cycle off its own physics
             * wants. Non-zero and surf_filmstrip_tick advances it. */
            int32_t  fps_q16;
            uint64_t due_us;     /* when the next frame is owed */
        } strip;
        struct {
            const surf_image *img;
            int16_t l, t, r, b;  /* insets; node w/h are the dst size */
            /* The centre band of a 9-patch is what STRETCHES, and it is
             * usually one flat colour (every capsule and panel we bake).
             * Decided once at construction — never in the frame path —
             * so paint can fill it in one op instead of tiling it. */
            bool       solid[3][3];
            surf_color solid_col[3][3];
        } nine;
        struct {
            const surf_font *font;
            char      *str;      /* owned (malloc); NULL = "" */
            int16_t    wrap_w;
            uint8_t    align, tflags;
            surf_image img;      /* atlas header copy; tint = text color */
        } text;
        struct {
            const surf_font *font;
            char      *buf;      /* owned; always NUL-terminated */
            int32_t    len, cap;
            int32_t    caret, anchor;  /* byte idx; selection = [min..max) */
            int16_t    scroll_x;
            char       mask;     /* != 0: draw this instead of every char */
            surf_image img;      /* atlas header copy; tint = text color */
        } input;
        struct {
            int32_t off_x, off_y;            /* content offset, Q16 */
            int32_t vel_x, vel_y;            /* px/tick, Q16 */
            int32_t drag_off_x, drag_off_y;  /* offset when the drag began */
            int16_t down_x, down_y, last_x, last_y;
            bool    dragging;
            bool    fast;                    /* hal-assisted pixel scroll */
        } scroll;
        struct {
            const surf_font *font;
            surf_textcell   *cells;  /* malloc, cols*total_rows */
            int16_t          cols, rows;
            int16_t          cell_w, cell_h;
            surf_color       fg, bg;
            bool             fast;   /* hal-assisted pixel scroll */
            /* Scrollback (opt-in, surf_textgrid_set_scrollback). cells is
             * a RING of total_rows: head is the ring row shown at screen
             * row 0 when live, hist counts the rows pushed above it, and
             * view is how far back the display is scrolled (0 = live).
             * Without scrollback total_rows == rows, head/hist/view stay
             * 0, and every path below reduces to the old arithmetic. */
            int16_t          total_rows;
            int16_t          head, hist, view;
            int16_t          drag_from;   /* view when a drag started */
            int16_t          drag_y;      /* and where it grabbed */
        } grid;
    } u;
};

/* Repaint whatever is painted above `area` after a hal-assisted shift
 * dragged its pixels along (see node.c). gx/gy expand each node's box so
 * the ghost it left behind is repainted too. */
void surf_damage_above(const surf_node *n, surf_rect area, int16_t gx, int16_t gy);

/* rect ops */
static inline bool surf_rect_empty(surf_rect r) { return r.w <= 0 || r.h <= 0; }
surf_rect surf_rect_intersect(surf_rect a, surf_rect b);
surf_rect surf_rect_union(surf_rect a, surf_rect b);
bool      surf_rect_overlaps(surf_rect a, surf_rect b);
bool      surf_rect_covers(surf_rect a, surf_rect b);  /* a covers all of b */
bool      surf_rect_contains(surf_rect r, int16_t x, int16_t y);

/* dirty-rect list: merge on overlap, degrade to bounding union at the cap */
typedef struct {
    surf_rect r[SURF_MAX_DIRTY];
    int       n;
    surf_rect clip;  /* screen bounds; adds are clipped to this */
} surf_dirty;

void surf_dirty_reset(surf_dirty *d, surf_rect clip);
void surf_dirty_add(surf_dirty *d, surf_rect r);

/* per-dirty-rect paint list, filled front-to-back, painted in reverse */
typedef struct {
    surf_node *n;
    int16_t    ax, ay;  /* absolute position */
    surf_rect  vis;     /* visible part, pre-clipped */
} surf_paint_ent;

/* One finger's worth of gesture state. `used` rather than a NULL capture,
 * because a contact can be live with nothing captured — a finger that
 * landed on empty space still has to be remembered, so that its MOVEs are
 * not mistaken for a fresh press. */
typedef struct {
    bool        used;
    uint8_t     id;
    surf_node  *capture;   /* node holding THIS contact, DOWN → UP */
    surf_node  *steal_sv;  /* scrollview waiting to steal this gesture */
    int16_t     down_x, down_y;
} surf_contact;

typedef struct {
    const surf_hal *hal;
    int16_t         w, h;
    surf_color      bg;
    surf_node      *pool;
    int             pool_cap;
    int             playing;   /* filmstrips with fps != 0 (node.c) */
    surf_node      *free_list;
    surf_node      *root;
    /* One of these per finger. Capture is PER CONTACT, so three fingers
     * on three sliders is three independent drags — see input.c. */
    surf_contact    contacts[SURF_MAX_CONTACTS];
    surf_node      *scrollers[8];  /* scrollviews with live momentum/spring */
    int             nscrollers;
    surf_dirty      dirty;
    int             frame_div;   /* game-mode frame lock; 0 = uncapped */
    surf_paint_ent *plist;  /* pool_cap entries */
} surf_ctx;

extern surf_ctx surf_g;

/* find the slot holding this contact id, or NULL */
surf_contact *surf_contact_find(uint8_t id);

void surf_input_dispatch(const surf_touch *t);

/* src/core/scroll.c */
bool surf_scroll_can_x(surf_node *sv);
bool surf_scroll_can_y(surf_node *sv);
void surf_scroll_begin(surf_node *sv, const surf_touch *t);
void surf_scroll_touch(surf_node *sv, const surf_touch *t);  /* MOVE/UP */
void surf_scroll_tick(void);                                 /* momentum/spring */
void surf_scroll_forget(surf_node *sv);  /* node freed/detached */

surf_node *surf_node_alloc(uint8_t type);  /* pool; NULL when exhausted */
bool      surf_node_attached(const surf_node *n);
/* own HIDDEN flag or any ancestor's — what a hal-shift gate must ask */
bool      surf_node_effectively_hidden(const surf_node *n);
surf_rect surf_node_subtree_bounds(const surf_node *n, int16_t px, int16_t py);
void      surf_damage_subtree(const surf_node *n);
void      surf_compose(void);  /* compose all dirty rects + present */

/* src/text/: layout walker shared by measure, paint, and caret math */
typedef struct {
    const surf_font *f;
    const char      *s;
    int16_t          wrap_w;
    uint8_t          align, tflags;
    int32_t          i;           /* byte cursor within the current line */
    int32_t          line_end;    /* render end of the current line */
    int32_t          next_start;  /* start of the next line; -1 = last */
    int16_t          pen_x, base_y;
    uint32_t         prev_cp;     /* kerning state; 0 at line start */
    uint8_t          ell;         /* ellipsize: 0 off, 1 pending, 2 done */
} surf_tlayout;

typedef struct {
    const surf_glyph *g;
    int16_t x, y;       /* glyph blit position (top-left), node-relative */
    int32_t byte_idx;   /* index of this codepoint in the string */
} surf_tglyph;

uint32_t surf_utf8_next(const char *s, int32_t *i);   /* 0 at NUL */
int32_t  surf_utf8_prev(const char *s, int32_t i);
const surf_glyph *surf_font_glyph(const surf_font *f, uint32_t cp);
int16_t  surf_font_kern(const surf_font *f, uint32_t a, uint32_t b);

void surf_tlayout_begin(surf_tlayout *it, const surf_font *f, const char *s,
                        int16_t wrap_w, uint8_t align, uint8_t tflags);
bool surf_tlayout_next(surf_tlayout *it, surf_tglyph *out);

void surf_glyph_blit(const surf_image *img, const surf_glyph *g,
                     int16_t dx, int16_t dy, surf_rect vis);
void surf_text_paint(const surf_paint_ent *e);       /* label */
void surf_textinput_paint(const surf_paint_ent *e);
void surf_textgrid_paint(const surf_paint_ent *e);
void surf_text_free_storage(surf_node *n);           /* all text types */

#endif /* SURF_INTERNAL_H */
