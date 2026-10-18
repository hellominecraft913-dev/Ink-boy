#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace EdgeSlide {

enum class Direction : uint8_t { None, LeftUp, LeftDown, RightUp, RightDown };

inline int bandWidth(const int screenWidth) { return std::max(32, screenWidth * 8 / 100); }

inline Direction directionFor(const int startX, const int startY, const int endX, const int endY, const int screenWidth,
                              const int screenHeight) {
  if (screenWidth < 2 || screenHeight < 2) return Direction::None;
  const int band = bandWidth(screenWidth);
  const bool left = startX >= 0 && startX < band && endX >= 0 && endX < band;
  const bool right =
      startX < screenWidth && startX >= screenWidth - band && endX < screenWidth && endX >= screenWidth - band;
  if (!left && !right) return Direction::None;
  const int dx = endX - startX;
  const int dy = endY - startY;
  if (std::abs(dy) < std::max(60, screenHeight * 6 / 100) || std::abs(dy) * 2 < std::abs(dx) * 3) {
    return Direction::None;
  }
  if (left) return dy < 0 ? Direction::LeftUp : Direction::LeftDown;
  return dy < 0 ? Direction::RightUp : Direction::RightDown;
}

}  // namespace EdgeSlide
