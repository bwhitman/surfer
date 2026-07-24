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
set(TTF_NAMES ui12 ui16 ui16b ui28 mono16 mono16g mono16b mono24
    bigblue12 bigblue24 kpixel16 kpixel32 kpixel48 kpixel64
    kmini16 kmini32 khigh32 kblocks16)
set(ADOBE_NAMES "")
foreach(fam helvR helvB ncenR courR)
    foreach(sz 08 10 12 14 18 24)
        list(APPEND ADOBE_NAMES ${fam}${sz})
    endforeach()
endforeach()
set(FONT_NAMES ${TTF_NAMES} ${ADOBE_NAMES})
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
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND cc -O2 -I${SURFER_ROOT}/tools -o ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake
            ${SURFER_ROOT}/tools/fontbake.c -lm
    DEPENDS ${SURFER_ROOT}/tools/fontbake.c
    VERBATIM
)
set(ROBOTO ${SURFER_ROOT}/assets/fonts/Roboto-Regular.ttf)
set(JBMONO ${SURFER_ROOT}/assets/fonts/JetBrainsMono-Regular.ttf)
set(BIGBLUE ${SURFER_ROOT}/assets/fonts/BigBlue_TerminalPlus.ttf)

# surfer_font(<name> <size> <ttf> [RANGES <spans>] [ENV <VAR=VAL>...])
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

surfer_font(ui16 16 ${ROBOTO})
surfer_font(ui28 28 ${ROBOTO})
surfer_font(mono16 16 ${JBMONO} RANGES "32-126,167,181,8211,8212,8230")
# specimen-only bakes: the same faces through each knob that matters
surfer_font(ui12 12 ${ROBOTO})
surfer_font(ui16b 16 ${ROBOTO} ENV FONTBAKE_THRESHOLD=1)
surfer_font(mono16g 16 ${JBMONO} ENV FONTBAKE_GAMMA=0.55)
surfer_font(mono16b 16 ${JBMONO} ENV FONTBAKE_THRESHOLD=1 FONTBAKE_THRESHOLD_CUT=96)
surfer_font(mono24 24 ${JBMONO})
surfer_font(bigblue12 12 ${BIGBLUE} ENV FONTBAKE_THRESHOLD=1)
surfer_font(bigblue24 24 ${BIGBLUE} ENV FONTBAKE_THRESHOLD=1)

# page 2: pixel-designed proportional faces (Kenney, CC0). FONTBAKE_EM=1
# is required — their grid is defined in em units, so only an exact ppem
# lands stems on whole pixels (fontbake reports gray 0.0% when it does).
set(KMINI ${SURFER_ROOT}/assets/fonts/KenneyMini.ttf)
set(KHIGH ${SURFER_ROOT}/assets/fonts/KenneyHigh.ttf)
set(KPIXEL ${SURFER_ROOT}/assets/fonts/KenneyPixel.ttf)
set(KBLOCKS ${SURFER_ROOT}/assets/fonts/KenneyBlocks.ttf)
set(KPIX_ENV FONTBAKE_EM=1 FONTBAKE_THRESHOLD=1)
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
