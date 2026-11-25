/*
 * kinora_ftoption.h - FreeType's build options for Kinora: FreeType's own
 * defaults, less what subtitles never use.  ft2build.h takes it through
 * FT_CONFIG_OPTIONS_H.
 */
#ifndef KINORA_FTOPTION_H
#define KINORA_FTOPTION_H

#include <freetype/config/ftoption.h>

#undef FT_CONFIG_OPTION_USE_LZW                /* compressed bitmap fonts */
#undef FT_CONFIG_OPTION_USE_ZLIB               /* compressed bitmap fonts, WOFF */
#undef FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES /* settings from getenv() */
#undef FT_CONFIG_OPTION_MAC_FONTS              /* resource forks */
#undef FT_CONFIG_OPTION_SVG                    /* SVG glyphs */
#undef TT_CONFIG_OPTION_BDF                    /* bitmap font properties */
#undef TT_CONFIG_OPTION_COLOR_LAYERS           /* colour glyphs */
#undef TT_CONFIG_OPTION_GX_VAR_SUPPORT         /* variable fonts */
#undef TT_CONFIG_OPTION_BYTECODE_INTERPRETER   /* hinting: the glyphs are loaded unhinted */

#endif /* KINORA_FTOPTION_H */
