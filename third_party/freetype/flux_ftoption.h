#ifndef FLUX_FTOPTION_H
#define FLUX_FTOPTION_H
#include <freetype/config/ftoption.h>
#define FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT
#define FT_CONFIG_OPTION_USE_BROTLI
#undef FT_CONFIG_OPTION_SVG
/* FreeType 2.14.3 i386 interpreter definition has an extra i in its name. */
#if defined(__i386__)
#define TT_MulFix14_i386 TT_MulFixi14_i386
#endif
#endif
