/* Font specimen scene, shared by the desktop demo (demos/fonts.c) and the
 * P4 firmware (ports/esp32p4/main/app_main.c) so the panel shows exactly
 * what the desktop window shows.
 *
 * Fonts are looked up by name through surf_font_builtin(), so neither
 * host has to enumerate them; a device host just calls
 * surf_font_builtin_prepare() once beforehand to re-home the atlases.
 *
 * Three pages, cycled by tapping/clicking anywhere:
 *   1  antialiasing knobs on outline faces (Roboto, JetBrains Mono)
 *   2  pixel-designed outline faces (Kenney), one grid scaled up
 *   3  Adobe X11 BDFs - a separately *designed* face per size
 */
#ifndef SURF_FONTS_SCENE_H
#define SURF_FONTS_SCENE_H

#include "surfer.h"

/* Builds all pages under surf_screen(); `page` (1..3) picks the one shown
 * first. Lays out to w x h, so the same scene fits a 1024x600 panel or a
 * desktop window. */
void fonts_scene_build(int16_t w, int16_t h, const char *subtitle, int page);

#endif
