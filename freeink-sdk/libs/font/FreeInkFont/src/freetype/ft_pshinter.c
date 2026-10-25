/* FreeInkFont build wrapper for CFF's PostScript hinter. */
#define FT2_BUILD_LIBRARY
#if FREEINK_FONT_ENABLE_CFF
#include "../../third_party/freetype/src/pshinter/pshinter.c"
#endif
