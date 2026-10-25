/* FreeInkFont build wrapper for the vendored FreeType CFF driver. */
#define FT2_BUILD_LIBRARY
#if FREEINK_FONT_ENABLE_CFF
#include "../../third_party/freetype/src/cff/cff.c"
#endif
