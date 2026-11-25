# FreeType, built from the pinned submodule as a static library for the player's
# subtitles (docs/architecture.md, section 10).  Used under the FreeType License.
#
#   Kinora::freetype   TrueType and OpenType fonts, the anti-aliased renderer, the stroker
#
# kinora_ftoption.h and kinora_ftmodule.h say what is left out.

set(KINORA_FREETYPE_SOURCE "${CMAKE_SOURCE_DIR}/external/freetype")
if(NOT EXISTS "${KINORA_FREETYPE_SOURCE}/include/ft2build.h")
    message(FATAL_ERROR "external/freetype is empty: run git submodule update --init")
endif()

set(KINORA_FREETYPE_FILES
    base/ftsystem.c base/ftinit.c base/ftdebug.c base/ftbase.c
    base/ftbbox.c base/ftglyph.c base/ftbitmap.c base/ftstroke.c
    truetype/truetype.c cff/cff.c sfnt/sfnt.c smooth/smooth.c
    psnames/psnames.c psaux/psaux.c pshinter/pshinter.c
)
list(TRANSFORM KINORA_FREETYPE_FILES PREPEND "${KINORA_FREETYPE_SOURCE}/src/")

add_library(kinora_freetype STATIC ${KINORA_FREETYPE_FILES})
add_library(Kinora::freetype ALIAS kinora_freetype)
# FreeType builds by its own rules: C99, no warning flags of ours.
set_target_properties(kinora_freetype PROPERTIES C_STANDARD 99)
target_compile_options(kinora_freetype PRIVATE -O2)
target_compile_definitions(kinora_freetype
    PRIVATE FT2_BUILD_LIBRARY
    PUBLIC "FT_CONFIG_OPTIONS_H=<kinora_ftoption.h>" "FT_CONFIG_MODULES_H=<kinora_ftmodule.h>")
# FreeType's headers are not ours to make warning-clean: SYSTEM keeps them quiet.
target_include_directories(kinora_freetype SYSTEM PUBLIC
    "${KINORA_FREETYPE_SOURCE}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
