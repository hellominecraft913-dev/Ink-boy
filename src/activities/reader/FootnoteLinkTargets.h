#pragma once

#include <Epub/FootnoteEntry.h>

#include <array>
#include <cstdint>
#include <vector>

class GfxRenderer;
class Page;

struct FootnoteLinkTarget {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
};

using FootnoteLinkTargets = std::array<FootnoteLinkTarget, EPUB_MAX_FOOTNOTES_PER_PAGE>;

// Match serialized link IDs to visible words without a heap allocation.
FootnoteLinkTargets buildFootnoteLinkTargets(const Page& page, const std::vector<FootnoteEntry>& footnotes,
                                             const GfxRenderer& renderer, int fontId, int marginTop, int marginLeft);
