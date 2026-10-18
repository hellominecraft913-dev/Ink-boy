#pragma once

#include <cstddef>
#include <cstdint>

// Whole-point sizes supported by the scalable reader font path.
inline constexpr uint8_t SCALABLE_READER_FONT_SIZES[] = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
#if CROSSINK_SCALABLE_FONTS
inline constexpr auto& BUILTIN_READER_FONT_SIZES = SCALABLE_READER_FONT_SIZES;
#else
inline constexpr uint8_t BUILTIN_READER_FONT_SIZES[] = {10, 12, 14, 16};
#endif

inline uint8_t closestBuiltinReaderPointSize(uint8_t target) {
  uint8_t best = BUILTIN_READER_FONT_SIZES[0];
  for (uint8_t size : BUILTIN_READER_FONT_SIZES) {
    const int distance = size > target ? size - target : target - size;
    const int bestDistance = best > target ? best - target : target - best;
    if (distance < bestDistance) best = size;
  }
  return best;
}

enum class FontSizeStepMode : uint8_t { Wrap, Clamp };

// Select one adjacent installed point size. The caller owns persistence and
// reflow; this helper stays allocation-free for both built-in and SD fonts.
inline bool changeReaderFontSizeStep(const uint8_t* sizes, const size_t count, uint8_t& pointSize, const bool larger,
                                     const FontSizeStepMode mode = FontSizeStepMode::Wrap) {
  if (!sizes || count < 2) return false;

  size_t current = 0;
  while (current + 1 < count && sizes[current] < pointSize) ++current;
  if (sizes[current] != pointSize && current > 0 && pointSize - sizes[current - 1] <= sizes[current] - pointSize) {
    --current;
  }

  size_t next = current;
  if (larger) {
    if (current + 1 >= count) {
      if (mode == FontSizeStepMode::Clamp) return false;
      next = 0;
    } else {
      ++next;
    }
  } else if (current == 0) {
    if (mode == FontSizeStepMode::Clamp) return false;
    next = count - 1;
  } else {
    --next;
  }

  if (sizes[next] == pointSize) return false;
  pointSize = sizes[next];
  return true;
}
