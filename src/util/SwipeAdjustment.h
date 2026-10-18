#pragma once

#include <algorithm>

namespace SwipeAdjustment {

// A recognized short swipe changes one point; a full-axis swipe can
// cover the entire 0-100 range. Both one- and two-finger gestures use this scale.
inline int amount(const int distance, const int axisSize) {
  if (axisSize < 2) return 0;
  const int minimumDistance = std::max(60, axisSize * 6 / 100);
  if (distance < minimumDistance) return 0;
  const int maximumDistance = axisSize - 1;
  const int travel = std::max(1, maximumDistance - minimumDistance);
  return 1 + (std::min(distance, maximumDistance) - minimumDistance) * 99 / travel;
}

inline int targetValue(const int initialValue, const bool increase, const int adjustment) {
  return std::clamp(initialValue + (increase ? adjustment : -adjustment), 0, 100);
}

}  // namespace SwipeAdjustment
