#pragma once

// FreeInk SDK — FreeInkFont → DisplayTarget bridge (opt-in).
//
// RasterGlyphSource draws a FreeInkFont FontChain (TTF/OTF faces with
// per-codepoint fallback) through a DisplayTarget font slot:
//
//   freeink::font::FontChain chain;          // e.g. Latin face, symbols, CJK
//   freeink::ui::RasterGlyphSource text(chain);
//   target.setFont(slot, text, 20, chain.lineHeight(20), chain.ascent(20));
//
// Unlike FreeInkUIBookFont.h's TtfGlyphSource it needs only FreeInkFont, not
// FreeInkBook. With a threshold, coverage at or above it is solid ink and
// below it paper: crisp 1-bit text for a fast e-paper refresh, where dithered
// anti-aliasing would look grainy.

#include <TtfFont.h>

#include "FreeInkUIDisplayTarget.h"

namespace freeink {
namespace ui {

class RasterGlyphSource final : public RuntimeGlyphSource {
 public:
  explicit RasterGlyphSource(font::FontChain& chain, uint8_t threshold = 0) : chain_(chain), threshold_(threshold) {}

  bool glyph(uint32_t codepoint, uint16_t pixelSize, Glyph* out) override {
    if (!chain_.covers(codepoint)) return false;
    font::RasterFont* face = chain_.fontFor(codepoint);
    const font::GlyphBitmap* g = face ? face->rasterize(codepoint, pixelSize) : nullptr;
    if (g == nullptr) return false;
    out->coverage = g->pixels;
    out->width = g->width;
    out->height = g->height;
    out->xoff = g->xoff;
    out->yoff = g->yoff;
    out->advance = g->advance;
    const size_t n = static_cast<size_t>(g->width) * g->height;
    if (threshold_ && n <= sizeof(scratch_)) {
      for (size_t i = 0; i < n; ++i) scratch_[i] = g->pixels[i] >= threshold_ ? 255 : 0;
      out->coverage = scratch_;
    }
    return true;
  }

  int16_t advance(uint32_t codepoint, uint16_t pixelSize) override {
    return chain_.covers(codepoint) ? chain_.advance(codepoint, pixelSize, font::StyleNone) : 0;
  }

 private:
  font::FontChain& chain_;
  uint8_t threshold_;
  uint8_t scratch_[64 * 64];  // a thresholded glyph; bigger ones keep their coverage
};

}  // namespace ui
}  // namespace freeink
