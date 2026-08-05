set(SURFER_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen)

# Globbed, like the desktop Makefile's CORE_SRCS/WIDGET_SRCS: this list
# was hand-maintained and silently fell behind core (pad.c, keys.c,
# image.c, shape.c, fontload.c all shipped without it), which shows up as
# link errors, not as a missing file. Cost of globbing: after adding a new
# core .c you need one `idf.py reconfigure`.
file(GLOB SURFER_CORE_SRCS
    ${SURFER_ROOT}/src/core/*.c
    ${SURFER_ROOT}/src/text/*.c
    ${SURFER_ROOT}/src/widgets/*.c)

set(GENDIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(TTF_NAMES ui12 ui16 ui16b ui23 ui28 mono16 bigblue12
    portfolio6x8 apricot8x10 phoenix8x8 xga7x15 robotron8x16 fmtowns8x16 toshiba9x16 dosv12x24 cordata16x26 wyse16x32
    kpixel16 kpixel32 kpixel48 kpixel64
    kmini16 kmini32 khigh32 kblocks16)
set(ADOBE_NAMES "")
foreach(fam helvR helvB ncenR)
    foreach(sz 08 10 12 14 18 24)
        list(APPEND ADOBE_NAMES ${fam}${sz})
    endforeach()
endforeach()
# Emoji sets — a colour picture atlas, baked by its own tool. Keep this
# list in step with EMOJI_NAMES in the desktop Makefile: the registry
# wires each text face to the emoji set nearest its size, so a device
# build missing one silently draws '?' where the desktop drew a picture.
set(EMOJI_SIZES 12 16)
set(EMOJI_NAMES "")
foreach(sz ${EMOJI_SIZES})
    list(APPEND EMOJI_NAMES emoji${sz})
endforeach()
set(FONT_NAMES ${TTF_NAMES} ${ADOBE_NAMES} ${EMOJI_NAMES})
idf_component_register(
    SRCS
        app_main.c
        ${GENDIR}/font_registry.c
        ${SURFER_CORE_SRCS}
        ${SURFER_ROOT}/src/hal/p4/hal_p4.c
        ${SURFER_ROOT}/demos/fonts_scene.c
    INCLUDE_DIRS
        .
        ${SURFER_ROOT}/include
        ${SURFER_ROOT}/src/core
        ${SURFER_ROOT}/src/hal/p4
        ${SURFER_ROOT}/demos
        ${SURFER_ROOT}/tools          # vendored stb headers, for core/image.c
        ${CMAKE_CURRENT_BINARY_DIR}/gen
        # esp_async_fbcpy (DMA2D rect copy) ships in esp_lcd's priv_include
        $ENV{IDF_PATH}/components/esp_lcd/priv_include
    PRIV_REQUIRES
        esp_driver_ppa esp_lcd esp_timer esp_mm heap espressif__esp_lcd_touch
)

# Must come after idf_component_register: IDF re-runs this file in script
# mode to gather requirements, and set_source_files_properties does not
# exist there — but nothing after the register call runs in that pass.
set_source_files_properties(${GENDIR}/font_registry.c PROPERTIES GENERATED TRUE)

# widget art + fonts baked at build time, same tools as the desktop demo.
# fontbake is a host tool: built with the host cc, not the cross compiler.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/widget_assets.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${SURFER_ROOT}/tools/gen_widget_assets.py > ${CMAKE_CURRENT_BINARY_DIR}/gen/widget_assets.h
    DEPENDS ${SURFER_ROOT}/tools/gen_widget_assets.py
    VERBATIM
)
# FreeType is a HOST build dependency of fontbake only — nothing links it
# into the firmware, the device still blits a baked A8 atlas. Optional, but
# the desktop Makefile does the same detection, and the two must agree or
# the panel gets fuzzier text than the SDL preview did. See the FONTBAKE_HINT
# block in tools/fontbake.c.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(HOST_FREETYPE QUIET freetype2)
endif()
if(HOST_FREETYPE_FOUND)
    set(FT_DEF -DSURF_FONTBAKE_FT)
    foreach(d ${HOST_FREETYPE_INCLUDE_DIRS})
        list(APPEND FT_DEF -I${d})
    endforeach()
    set(FT_LINK "-L${HOST_FREETYPE_LIBRARY_DIRS}" "-lfreetype")
    set(FONT_HINT FONTBAKE_HINT=full)
else()
    set(FT_DEF "")
    set(FT_LINK "")
    set(FONT_HINT "")
    message(WARNING "no host freetype2: baking unhinted fonts, small text "
                    "will be fuzzy on the panel")
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND cc -O2 -I${SURFER_ROOT}/tools ${FT_DEF}
            -o ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake
            ${SURFER_ROOT}/tools/fontbake.c ${FT_LINK} -lm
    DEPENDS ${SURFER_ROOT}/tools/fontbake.c
    VERBATIM
)
set(ROBOTO ${SURFER_ROOT}/assets/fonts/Roboto-Regular.ttf)
set(JBMONO ${SURFER_ROOT}/assets/fonts/JetBrainsMono-Regular.ttf)
set(BIGBLUE ${SURFER_ROOT}/assets/fonts/BigBlue_TerminalPlus.ttf)

# surfer_font(<name> <size> <ttf> [RANGES <spans>] [ENV <VAR=VAL>...])
# RANGES takes `base` (the default: ASCII + Latin-1 + dashes/ellipsis),
# `mono` (that plus the rest of CP437 -- box drawing, blocks, arrows,
# suits) or an explicit span list. The two names are defined ONCE, in
# tools/fontbake.c, so this file and the desktop Makefile cannot drift.
# Same bakes as the desktop Makefile — keep the two in step: the specimen
# scene is shared source and names these fonts by header.
set(SURFER_FONT_HEADERS "")
macro(surfer_font name size ttf)
    cmake_parse_arguments(BF "" "RANGES" "ENV" ${ARGN})
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/font_${name}.h
        COMMAND ${CMAKE_COMMAND} -E env ${BF_ENV}
                ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake ${name} ${size} ${ttf}
                ${CMAKE_CURRENT_BINARY_DIR}/gen/font_${name}.h ${BF_RANGES}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake ${ttf}
        VERBATIM
    )
    list(APPEND SURFER_FONT_HEADERS ${CMAKE_CURRENT_BINARY_DIR}/gen/font_${name}.h)
endmacro()

# SIZE is ppem. ui23/mono24 are the panel's body sizes — the same PHYSICAL
# size as ui16/mono16 in the 1024x600 desktop window, because this 7" panel
# is 169dpi against the window's 110-140.
surfer_font(ui16 16 ${ROBOTO} ENV ${FONT_HINT})
surfer_font(ui23 23 ${ROBOTO} ENV ${FONT_HINT})
surfer_font(ui28 28 ${ROBOTO} ENV ${FONT_HINT})
surfer_font(mono16 16 ${JBMONO} RANGES mono ENV ${FONT_HINT})
surfer_font(ui12 12 ${ROBOTO} ENV ${FONT_HINT})
# specimen-only bakes: the same faces through each knob that matters, so
# these stay UNHINTED — each shows what its knob does to a raw outline
surfer_font(ui16b 16 ${ROBOTO} ENV FONTBAKE_THRESHOLD=1)
surfer_font(bigblue12 12 ${BIGBLUE} RANGES mono ENV FONTBAKE_THRESHOLD=1)

# The oldschool PC faces (VileR's pack, CC BY-SA 4.0). ppem is
# unitsPerEm/100 and they are NOT hinted: at that exact size each is
# 0.0% gray and lands on the 1-bit path, and the autohinter would only
# smear a face already on the grid. Keep in step with the Makefile.
set(OLDSCHOOL ${SURFER_ROOT}/assets/fonts/oldschool)
surfer_font(portfolio6x8 8 ${OLDSCHOOL}/Px437_Portfolio_6x8.ttf RANGES mono)
surfer_font(apricot8x10 12 ${OLDSCHOOL}/Px437_Apricot_256L.ttf RANGES mono)
surfer_font(phoenix8x8 8 ${OLDSCHOOL}/Px437_PhoenixVGA_8x8.ttf RANGES mono)
surfer_font(xga7x15 16 ${OLDSCHOOL}/Px437_IBM_XGA-AI_7x15.ttf RANGES mono)
surfer_font(robotron8x16 16 ${OLDSCHOOL}/Px437_Robotron_A7100.ttf RANGES mono)
surfer_font(fmtowns8x16 16 ${OLDSCHOOL}/Px437_FMTowns_re_8x16.ttf RANGES mono)
surfer_font(toshiba9x16 16 ${OLDSCHOOL}/PxPlus_ToshibaSat_9x16.ttf RANGES mono)
surfer_font(dosv12x24 24 ${OLDSCHOOL}/Px437_DOS-V_re_JPN24.ttf RANGES mono)
surfer_font(cordata16x26 32 ${OLDSCHOOL}/PxPlus_Cordata_PPC-21.ttf RANGES mono)
surfer_font(wyse16x32 32 ${OLDSCHOOL}/Px437_Wyse700a-2y.ttf RANGES mono)

# page 2: pixel-designed proportional faces (Kenney, CC0). Their grid is
# defined in em units, so only an exact ppem lands stems on whole pixels
# (fontbake reports gray 0.0% when it does) — which is what SIZE means
# now, so there is nothing to opt into. Never hinted: the grid-fit they
# want is the one they were drawn on.
set(KMINI ${SURFER_ROOT}/assets/fonts/KenneyMini.ttf)
set(KHIGH ${SURFER_ROOT}/assets/fonts/KenneyHigh.ttf)
set(KPIXEL ${SURFER_ROOT}/assets/fonts/KenneyPixel.ttf)
set(KBLOCKS ${SURFER_ROOT}/assets/fonts/KenneyBlocks.ttf)
set(KPIX_ENV FONTBAKE_THRESHOLD=1)
# Kenney Pixel size ramp: multiples of its em16 grid, all exact
surfer_font(kpixel16 16 ${KPIXEL} ENV ${KPIX_ENV})
surfer_font(kpixel32 32 ${KPIXEL} ENV ${KPIX_ENV})
surfer_font(kpixel48 48 ${KPIXEL} ENV ${KPIX_ENV})
surfer_font(kpixel64 64 ${KPIXEL} ENV ${KPIX_ENV})
surfer_font(kmini16 16 ${KMINI} ENV ${KPIX_ENV})
surfer_font(kmini32 32 ${KMINI} ENV ${KPIX_ENV})
surfer_font(khigh32 32 ${KHIGH} ENV ${KPIX_ENV})
surfer_font(kblocks16 16 ${KBLOCKS} ENV ${KPIX_ENV})

# Adobe X11 BDFs: copied pixel-for-pixel, SIZE ignored (a BDF is one
# designed size). All 24 ship. See assets/fonts/bdf/.
foreach(n ${ADOBE_NAMES})
    surfer_font(${n} 0 ${SURFER_ROOT}/assets/fonts/bdf/${n}.bdf)
endforeach()

# One TU owns every atlas (surf_font_builtin); including a font header
# anywhere else would duplicate its pixels into that object file.
# emojibake is the second host tool. stb_image only — no FreeType, since
# the source art is PNG (see the header of tools/emojibake.c).
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/emojibake
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND cc -O2 -I${SURFER_ROOT}/tools -I${SURFER_ROOT}/include
            -o ${CMAKE_CURRENT_BINARY_DIR}/gen/emojibake
            ${SURFER_ROOT}/tools/emojibake.c -lm
    DEPENDS ${SURFER_ROOT}/tools/emojibake.c
    VERBATIM
)
set(EMOJI_SET ${SURFER_ROOT}/assets/emoji/set.txt)
set(EMOJI_PNG ${SURFER_ROOT}/assets/emoji/png)

# The 16px bake also emits the NAME table (the names belong to the SET,
# not to a size), so it has two outputs and the others have one.
foreach(sz ${EMOJI_SIZES})
    if(sz EQUAL 16)
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/font_emoji16.h
                   ${CMAKE_CURRENT_BINARY_DIR}/gen/emoji_names.h
            COMMAND ${CMAKE_CURRENT_BINARY_DIR}/gen/emojibake emoji16 16
                    ${EMOJI_SET} ${EMOJI_PNG}
                    ${CMAKE_CURRENT_BINARY_DIR}/gen/font_emoji16.h
                    ${CMAKE_CURRENT_BINARY_DIR}/gen/emoji_names.h
            DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gen/emojibake ${EMOJI_SET}
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/font_emoji${sz}.h
            COMMAND ${CMAKE_CURRENT_BINARY_DIR}/gen/emojibake emoji${sz} ${sz}
                    ${EMOJI_SET} ${EMOJI_PNG}
                    ${CMAKE_CURRENT_BINARY_DIR}/gen/font_emoji${sz}.h
            DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gen/emojibake ${EMOJI_SET}
            VERBATIM
        )
    endif()
    list(APPEND SURFER_FONT_HEADERS
         ${CMAKE_CURRENT_BINARY_DIR}/gen/font_emoji${sz}.h)
endforeach()
list(APPEND SURFER_FONT_HEADERS ${CMAKE_CURRENT_BINARY_DIR}/gen/emoji_names.h)

add_custom_command(
    OUTPUT ${GENDIR}/font_registry.c
    COMMAND python3 ${SURFER_ROOT}/tools/gen_font_registry.py ${FONT_NAMES}
            > ${GENDIR}/font_registry.c
    DEPENDS ${SURFER_ROOT}/tools/gen_font_registry.py ${SURFER_FONT_HEADERS}
    VERBATIM
)

add_custom_target(surfer_gen_assets DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/gen/widget_assets.h
    ${GENDIR}/font_registry.c
    ${SURFER_FONT_HEADERS})
add_dependencies(${COMPONENT_LIB} surfer_gen_assets)
