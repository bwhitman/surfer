# CLAUDE.md — surfer

surfer is a small retained-mode UI compositor: C11 core, ESP32-P4 + SDL2 +
emscripten backends, MicroPython bindings. Read `DESIGN.md` before writing
code; it is the source of truth. If a task conflicts with DESIGN.md, stop and
ask rather than silently diverging.

## Non-negotiable architecture rules

- **No per-pixel software loops in the frame path.** The frame path is
  fill/blit/blend via the hal only. Per-pixel code is allowed exclusively
  inside `src/hal/sdl/` (desktop software blend), in build-time tools, and
  in the textgrid cell composer (`src/text/textgrid.c`) — the measured
  exception: the PPA's ~85µs/op floor makes per-glyph blits unusable for
  full-screen text (DESIGN.md §5.6).
- **No runtime vector rasterization.** Widget visuals are pre-rendered assets
  (filmstrips, 9-slice, baked font atlases). If a widget "needs" runtime
  drawing, the answer is a better asset, not a rasterizer.
- **Platform code lives only under `src/hal/`.** Core and widgets are
  platform-free C11 — no `#ifdef ESP_PLATFORM` outside the hal, ever.
- **Any buffer the PPA touches is 64-byte aligned** (allocation AND width
  stride), with explicit `esp_cache_msync` after CPU writes before PPA reads.
  All device allocations go through `hal->alloc_image`; never raw
  `heap_caps_malloc` in core code.
- **The widget set stays small** (knob, slider, button, checkbox, dropdown,
  label, textinput, scrollview, scrollbar, led, selector). Do not add
  widgets, node types, or hal ops without asking first.
- **No new dependencies without asking.** Currently allowed: stb_truetype,
  stb_image (build tools only), SDL2, ESP-IDF, MicroPython headers.

## Build & test loop

Desktop SDL is the iteration loop; touch hardware only at hal-backend
milestones.

```
make sdl        # builds desktop demo → build/surfer_demo
make test       # unit tests (dirty-rect coalescing, wrap, hit test)
make test-sdl   # present-coherence regression (opens an SDL window):
                # fb vs presented texture must match after fast scroll
make web        # emscripten C demos → build/web/{mixer,settings}.html
make mpy-web    # tulip mode in the browser → build/web/index.html
idf.py build    # from ports/esp32p4/
```

Every core change must keep `make sdl && make test` green. Performance
acceptance test for anything touching the compositor: the M1 demo (6 sliders +
6 knobs) holds 60 fps under continuous drag; print frame time stats with
`SURF_STATS=1`.

## Style

- C11, 4-space indent, `snake_case`, `surf_` prefix on all public symbols.
- Public API is `include/surfer.h` only; keep it flat and boring — it is also
  the MicroPython binding surface, hand-bound in `bindings/micropython/`.
- No dynamic allocation in the frame path; nodes and rect lists come from
  pools sized at init.
- Prefer fixed-point (16.16) over float in core; the P4 has an FPU but the
  habit keeps hal backends honest.
- Comments explain *why* (bandwidth, alignment, PPA quirks), not *what*.

## Layout

```
include/surfer.h        public API (binding surface)
src/core/               scene graph, dirty rects, hit test, anim
src/widgets/            knob, slider, ...
src/text/               atlas text, wrap, textinput logic
src/hal/sdl/  src/hal/p4/  src/hal/web/
bindings/surfer/modsurfer.c
tools/surfpack.py       asset + font atlas packer
assets/                 source art; assets/kenney/lib/ is the CC0 sprite
                        library — 40k Kenney sprites with lib/index.tsv
                        (path → description → WxH) for finding art by
                        grepping descriptions instead of looking at pixels
ports/esp32p4/          ESP-IDF project wrapping the p4 hal
demos/
```

## Current state

M0 + M1 done. Core: group/rect/sprite/filmstrip/ninepatch nodes,
dirty-rect compositor with occlusion early-out (ninepatch stretches by
tiling — no scale_blit in the frame path), hit test, touch dispatch with
pointer capture (`src/core/input.c`). Widgets: knob (vertical-drag
default, angular optional) and slider, written against `surfer.h` only.
`make sdl` builds the M1 mixer demo (6 knobs + 6 sliders) →
`build/surfer_demo`, plus the M0 bounce demo → `build/surfer_bounce`;
placeholder art baked by `tools/gen_widget_assets.py`. Acceptance
verified: 60 fps windowed with all 12 controls animating
(`SURF_AUTODRAG=1`), ~0.5 ms/tick compose headless.

M2 done — **the bet passed on hardware.** p4 hal (`src/hal/p4/`): PPA
fill/SRM/blend, triple-buffer-with-damage presentation — zero-copy DSI
flip + DMA2D damage-forward, flicker-free (the measured buffering
verdict — see DESIGN.md §5.2 for all numbers and rejected paths), GT911
touch. `ports/esp32p4/` targets the ESP32-P4-Function-EV-Board
(IDF ≥5.4, BSP `esp32_p4_function_ev_board_noglib`); boot runs a
bandwidth/PPA benchmark, then the mixer demo. Measured under finger:
62–66 fps, ~2.3 ms/tick. Key hardware rule learned: PPA ops cost
~70–200 µs each regardless of size → bake assets at final size (the
slider uses a sprite track when style art matches exactly; tiled 9-patch
is the fallback). Flash: `idf.py -p <port> flash` from `ports/esp32p4/`.

M3 done (desktop-verified; device run pending a replug): text.
`tools/fontbake.c` (stb_truetype, host tool) bakes TTFs into A8 atlas +
advance/kern headers at build time; runtime text is clipped atlas blits
(`src/text/`: UTF-8, greedy wrap on space/hyphen, kerning, align,
ellipsize; label + textinput nodes with caret/selection/scroll-into-view;
byte-offset indices). A8 images carry a `tint`; SDL blends in software,
P4 uses PPA `PPA_BLEND_COLOR_MODE_A8` + `fg_fix_rgb_val`. Desktop
keyboard feeds textinput via `surf_hal_sdl_poll_key` (hal-adjacent, not
in the vtable — the device path is the M-later OSK widget). Ctrl-C is
the exception to that queue: it goes to the `surf_hal_sdl_on_interrupt`
hook and is swallowed, because the case it exists for is escaping a host
loop that never reads keys. `port_sdl.c` points the hook at
`mp_sched_keyboard_interrupt`, matching what a device USB driver does
with ctrl+C — without it the desktop had no way out of an app's own
`while surfer.tick()` loop.
`build/surfer_type` is the text demo; `SURF_SHOT=x.ppm` dumps any demo's
framebuffer.

M4 done (desktop-verified): scrollview node (`src/core/scroll.c`) with
drag/flick momentum, edge resistance + spring-back, all fixed-point in
core ticks; damage from scrolled content translates through offsets and
clips to ancestor boxes. Input: a scrollable scrollview captures empty-
space drags directly, and steals a child handler's gesture after 8px of
travel along a scrollable axis — unless the handler set
`surf_node_set_gesture_grab` (sliders/knobs/textinput do). Groups with a
handler + size are hittable (hot areas, scrims). Widgets: checkbox
(2-frame filmstrip) and dropdown (popup attaches to the screen root —
detach/reattach as overlay). `build/surfer_settings` is the M4 demo.

M5 done on the unix port (esp micropython is next): hand-written binding
`bindings/surfer/modsurfer.c` (two MP types — Node and Widget — plus flat
factory functions; capitalized aliases; callbacks fire from tick on the
same thread; a GC-root registry keeps C-referenced objects alive).
`make mpy` builds it (MPY_DIR ?= ~/micropython, pinned v1.26.0; needs
`make -C ports/unix submodules` once). `bindings/surfer/tulip.py` is
tulip mode: an on-screen REPL on a mono16 textgrid + tulipcc-style
UIScreen — `s = surfer.slider(x,y)`, `screen.add(s)`, `s.y_pos`,
`s.callback = fn` all live. `bindings/surfer/test_surfer.py` is the
headless binding test (uses `surfer._touch` injection).

M6 web: both flavors build and run in a canvas. (1) C demos —
`make web`: the sdl hal compiled with emscripten (`-sUSE_SDL=2
-sASYNCIFY`); the desktop `while (pump()) tick` shape survives via an
EM_ASYNC_JS rAF yield in `surf_hal_sdl_pump` (rAF, not a timer: frames
drawn from timer-resumed contexts are not reliably composited).
(2) Tulip mode — `make mpy-web`: micropython's webassembly port +
the binding via the `bindings/surfer/web/` VARIANT_DIR (freezes
tulip.py + gamma9001; `index.html` is the host page). Key rules
learned, all load-bearing: the MP VM must NEVER suspend (an ASYNCIFY
suspend inside import machinery wedges the VM; inside a sync ccall it
aborts), so the browser drives frames — tulip.py skips its loop on
sys.platform == "webassembly" and JS calls `tulip.frame()` per rAF
(setTimeout fallback when hidden). That requires: pyscript-style
deferred GC (standard variant's gc_collect suspends via
emscripten_scan_registers), `SDL_HINT_EMSCRIPTEN_ASYNCIFY=0` +
no-PRESENTVSYNC (SDL sleeps in SwapWindow/Delay by default under
ASYNCIFY), and hal_sdl compiled as a direct usermod TU with
SURF_HAL_SDL_NO_YIELD (emscripten drops EM_JS bodies that come from
static archives — links fine, JS function silently missing).

M7 sprites: runtime images + transformed sprites, all three backends.
`surf_image_from_png` (stb_image, vendored in tools/stb/, PNG-only,
decode at load time — never in the frame path) → ARGB8888 with the
64-byte stride rule; any size. Sprites gained `surf_sprite_set_xform`:
uniform scale (Q16, clamped to the PPA's 1/16..16) and rotation in
quarter turns CCW (the PPA SRM limit). Transformed draws go through the
new hal op `xform_blend` (see DESIGN.md §5.4-decided); moving a sprite
is just node damage — the compositor repaints what it uncovers. MP API:
`surfer.image(png_bytes)` → Image (w/h, destroy(); sprites hold a ref
so the GC can't free pixels in use), `surfer.sprite(img, x, y)` with
`.scale` (float) and `.rot` (degrees, multiples of 90). Demo:
`import space` in tulip mode (examples/space.py + space_assets.py —
Kenney CC0 art baked to bytes by tools/pngwrap.py; source PNGs in
assets/kenney/). Frozen into web + SURFER_P4. Verified: unix shot,
web (anim delta + frame dump), P4 runs it without PPA errors
(on-panel eyeball pending).

Tulip mode for the P4 is VERIFIED ON HARDWARE — REPL on the panel,
USB keyboard typing, touch live (MICROPY_HW_ENABLE_USBDEV=0 in the
board config is what frees the OTG PHY for host mode). Build: `make mpy-p4` — micropython v1.28.0 (`~/micropython-1.28`,
first P4-capable release) + IDF v5.5.1 (`~/esp/esp-idf-v5.5.1`, MP's P4
code needs 5.5 APIs; the native firmware in `ports/esp32p4/` defaults to
5.4.1 — but see the rev v3.x note below). The binding is split over a tiny port layer
(`bindings/surfer/surfer_port.h`): `port_sdl.c` for desktop,
`port_p4.c` for device — EK79007 DSI panel + GT911 touch brought up on
core-IDF APIs only (no BSP/managed components; wiring constants
documented in-file), assets copied flash→PSRAM at init, and
`usb_kbd.c`, a raw-usb_host HID boot-protocol keyboard for the USB-A
port. Board def `bindings/surfer/boards/SURFER_P4/` freezes tulip.py
(6MiB app partition — the binary carries the baked assets). Flash with
`make mpy-p4-flash PORT=...`; the board boots straight into tulip
mode (frozen main.py) — Ctrl-C on the serial console drops to the REPL.
Soft-reset re-inits the C scene (mod_init tears down on re-entry).
Remaining: on-device tulip verify, M6 web build + real art.

**Two P4 silicon revisions — images are NOT interchangeable.** Espressif
split the P4 at chip rev v3.x (marketed "P4X"); IDF < 5.5.3 cannot build
for v3.x at all, and a v1.x image will not boot on it (or vice versa).
The bench board reports rev **v3.2**. For it, build `ports/esp32p4/`
with IDF v5.5.3 and the rev-3 overlay:

```
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.rev3" \
       -p /dev/cu.usbmodem<...> flash
```

Plain `idf.py build` (IDF 5.4.1, no overlay) still targets the v1.x
board. One code-level trap, not config: leave `bsp_display_config_t`'s
`.phy_clk_src` at 0 — on IDF ≥ 5.5.3 `MIPI_DSI_PHY_CLK_SRC_DEFAULT` is a
compat alias for the LEGACY PLL_F20M reference, illegal on v3.x, and the
image compiles clean then abort()s inside `esp_lcd_new_dsi_bus` at boot
with no message. Measured on tulip5's identical part: compose cost at
large damaged areas fell 42–51% (wider PPA SRM block, 8×8 → 32×32); the
~85 µs per-op floor is unchanged, so "bake at final size" still holds.

## The hal-shift smear rule

Four paths hand a rect to the hal to shift in place — the layer's
band_shift, the **sprite's fast pan** (a camera window walked over a big
opaque image), the scrollview's scroll_rect, and the textgrid's. All four
drag the pixels of whatever is painted ON TOP of that rect, which then
has to be repainted where it actually is.

The layer used to repair only its LATER SIBLINGS, which is right only
when every overlay is a sibling. It is not: tulip's task bar and its
console scrollbar are siblings of an app's GROUP, not of the scrolling
node inside it, so they smeared across the screen at whatever rate the
thing under them was scrolling — three layers, three different rates, one
very funny bug report.

`surf_damage_above(n, area, gx, gy)` (node.c) walks the whole paint order
after `n`, skipping its own subtree, and damages anything overlapping.
All four paths use it. tests/test_layer.c and tests/test_sprite.c have
the regression: an overlay in a different branch, which fails against the
sibling-only walk.

The sprite copy of the walk was found a day later, by forest (a sprite
camera over a baked world) smearing the same task bar the layer fix had
just stopped smearing — so if a fifth shift path ever appears, this is
the paragraph it has to read. Its test needs an **opaque** image: fast
pan is gated on `img->opaque`, and with a transparent one the slow path
runs, damages everything, and the test passes with the bug in place.

## Textinput, from MicroPython

`surfer.textinput(x, y, w, color, font)` is the editable-text node, and
it draws the TEXT and nothing else — no box, no border, no keyboard, per
DESIGN.md §2.5 — so a field in practice is a `rect` with one of these on
top of it.

Two things the binding adds over a literal wrapping of the C calls,
because every caller would otherwise write them:

- **A tap places the caret, a drag extends the selection.** That is what
  a text field *is*, so `ti_touch` is installed at creation. Setting
  `.on_touch` from Python does not lose it: the setter installs the same
  handler, which moves the caret and then calls the Python callback.
- **`.key(k)` applies ONE event from `surfer.keys()`** — the
  `(kind, text, shift)` tuple — and returns whether it consumed it, so
  Enter and hotkeys fall through to the app:

  ```python
  for k in surfer.keys():
      if not field.key(k):
          ...     # yours
  ```

Everything else is a thin pass: `.text`, `.caret`, `.focus(on)`,
`.insert()`, `.backspace()`, `.delete()`, `.move(delta, extend)`,
`.index_from_x(local_x)`. Every `surf_textinput_*` entry point guards on
the node type, so these are safe no-ops on any other node — except
`set_text`, which the binding routes explicitly, since the two node types
have separate C setters that each ignore the other's node.

`surfer._key(kind, text, shift)` pushes one event into the queue a driver
feeds — the counterpart of `_touch`, and the only way a headless test can
reach anything that reads the keyboard.

## LED and selector

Two panel controls, added together for tulip5's TB-303.

`surf_led` is the only widget that **reports nothing** — a lamp is an
output, so it has no callback. The art is A8, and each LED keeps its own
COPY of the `surf_image` struct (shared pixels, its own `tint`), which is
how one asset serves every colour: on the P4 the tint is a palette
register the PPA applies at blend time, so `set_color` costs a repaint
and no pixels. Brightness is a **level, not a bool**, so a blink can
fade, and frame 0 is the unlit lens rather than nothing — a dead LED is a
visible dark bead. Its unlit alpha is 0.55 because these sit on white
piano keys as well as black panels, and a faint red over white reads as
pink rather than as an off lamp.

`surf_selector` is a knob with **detents**: N fixed positions, reporting
an index. It shares the knob's filmstrip and lands on the frame nearest a
detent, so N is a runtime number needing no art of its own. Two gestures,
because a panel control wants both — a vertical DRAG that snaps as it
goes, and a TAP that advances one position and wraps, which is how you
nudge a 4-position mode switch with a finger. A tap is a press that
travelled under 6px, decided at UP.

Both are bound: `surfer.led(x, y, color)` and `surfer.selector(x, y, n)`,
with `.value` a brightness (or True/False) and an index respectively.

## Scrollbar

`surf_scrollbar_new(parent, x, y, len, vertical, style)` is a thumb on a
track that knows **nothing about what it scrolls**. The caller owns the
content model — `set_range(total, visible, pos)` in whatever unit suits
it — and the widget only does ratios, reporting a new `pos` through
`on_change` when dragged. It hides itself while `total <= visible`, so a
caller can set the range unconditionally and the bar appears when there
is somewhere to go. Both pieces are 9-patched capsules, so the ends stay
round at any length and nothing is drawn at frame time.

Three consumers in tulip5, deliberately in three different units: the
console (rows of scrollback), the editor (lines of a document), and
gamma9001's sound chooser (pixels of scrollview offset).

Two things the widget got wrong until tulip5's `widgets` demo put a
horizontal one on screen next to a vertical one:

- **Horizontal needs its own art.** A 9-patch slices along fixed axes, so
  a bar laid on its side cannot reuse the upright capsule — stretching it
  sideways tiles the round *cap* and the thumb comes out as a string of
  beads. `thumb_h`/`track_h` in the style are the lying-down pair, and
  the insets move to the left/right edges. They are optional; without
  them a horizontal bar still works, it just looks wrong.
- **The MicroPython callback reported a Q16 fraction.** `pos` is in the
  caller's unit, but the binding fell through to the knob/slider branch
  and divided by SURF_ONE, so every `int(pos)` handler saw 0 — all three
  tulip5 bars snapped to the top when dragged instead of landing where
  the thumb was dropped. `.value` was always right, which is what hid it.

## Textgrid scrollback

`surf_textgrid_set_scrollback(n, mult)` keeps `mult` screens of rows so
lines that scroll off the top stay reachable: drag the grid to look back,
a thin macOS-style bar appears on the right while there is history, and
any write snaps the view to the bottom the way a terminal does.
`surf_textgrid_view/set_view/history` drive it programmatically, and the
visible bar is a separate `surf_scrollbar` the caller places and keeps in
step — the grid draws no chrome of its own. Dragging the TEXT still
scrolls (a touchscreen wants that), so a caller that shows a bar should
poll `surf_textgrid_view` to follow it.

The cells become a **ring** of `total_rows`, with `head` the ring row at
screen row 0 and `view` how far back the display is. Scrolling then moves
the window instead of the contents — O(exposed rows), not O(screen), and
the rows leaving the top become the history rather than being discarded.
Without scrollback `total_rows == rows`, head/view stay 0, and every path
reduces to the old arithmetic, so a plain grid is untouched.

Opt-in because it costs `cols*rows*mult*sizeof(surf_textcell)` — a 128x50
console at 10x is ~500 KB. That is a plain `calloc`, which on a PSRAM board reaches external RAM
(IDF's SPIRAM_USE choice defaults to SPIRAM_USE_MALLOC, and allocations
over SPIRAM_MALLOC_ALWAYSINTERNAL — 16 KB by default — prefer it). It
returns false rather than trapping if the heap cannot serve it, so a
caller can fall back (tulip5 tries 10, 4, 2 screens).
Enabling it installs the grid's own touch handler, so a node with
scrollback must not also have `on_touch` set.

## Fonts

surfer ships **44 baked fonts from 31 source files**, all reachable at
runtime by name via `surf_font_builtin("helvR12")`. `tools/fontbake.c`
has two front ends: stb_truetype for outlines, and a BDF reader that
copies designed bitmap fonts pixel-for-pixel (SIZE is ignored — a BDF
*is* one size). It reports **gray %** = share of inked pixels that are
neither 0 nor 255; 0.0% means a genuine bitmap, 78-100% means an outline
face. Knobs: `FONTBAKE_EM=1` (ppem sizing — required for pixel-designed
outline faces, whose grid is in em units), `FONTBAKE_GAMMA`,
`FONTBAKE_THRESHOLD[_CUT]`.

Sources: Roboto (ui12/16/16b/28/36/48 — the 36 and 48 are display sizes,
plain AA, where partial coverage reads as a smooth curve rather than the
lumpiness thresholding an off-grid outline gives at small sizes) +
JetBrains Mono (outline, AA), BigBlue Terminal (bitmap
mono), 4 Kenney pixel faces (CC0), and 24 Adobe X11 BDFs — helvR/helvB/
ncenR/courR at 08/10/12/14/18/24, each a separately *designed* size.
`assets/fonts/LICENSE.txt` has the terms; BigBlue's provenance is still
unpinned (TODO before shipping).

**One TU owns every atlas.** `tools/gen_font_registry.py` emits
`font_registry.c`, which includes all the font headers and implements
`surf_font_builtin*`. Font headers declare `static const` atlases, so
including one anywhere else silently duplicates its pixels into that
object file — don't. Device backends call
`surf_font_builtin_prepare(fn)` once at startup to re-home every atlas
into DMA-able RAM. Cost: 1.25 MiB of atlas, P4 image 2.70 MiB of the
8 MiB partition (66% free), plus the same again in PSRAM.

The binding's two unnamed defaults are `DEFAULT_FONT` (`ui12`, what
`surfer.label` uses with no font argument) and `WIDGET_FONT` (`helvR08`
— button labels and dropdown items: a drawn bitmap, so chrome stays
crisp at the size chrome renders at). Both resolve by NAME through
`font_named()`; never `surf_font_builtin_at(0)`, since index 0 is only
whatever comes first in the Makefile list and reordering it would
silently restyle every widget.

MicroPython takes a font as a name, a `Font` object, or a legacy index
anywhere: `surfer.label(s, x, y, c, "helvR12")`,
`surfer.textgrid(cols, rows, fg, bg, "courR14")`, `surfer.font(name_or_blob)`,
`surfer.fonts([mono_only])`. `surf_font_is_mono` gates the textgrid — it
sizes its cell from 'M', so a proportional face is refused.

`build/surfer_fonts` (desktop) and `DEMO_MODE = DEMO_FONTS` in
`ports/esp32p4/main/app_main.c` render the same 3-page specimen from the
same source (`demos/fonts_scene.c`); tap/click cycles pages. `SURF_TAP=x,y`
injects a synthetic tap so the page flip is testable headlessly. `SURF_SCALE=N` (sdl hal) blows the
window up N× with nearest-neighbour sampling for looking at glyph
pixels — the scene, framebuffer and `SURF_SHOT` dumps stay at the real
resolution; only the window grows.
