#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace TwoFingerSwipe {

enum class Direction : uint8_t { None, Up, Down, Left, Right };

struct CompletedSwipe {
  uint8_t contactCount = 0;
  int startX = 0;
  int startY = 0;
  int endX = 0;
  int endY = 0;
  unsigned long durationMs = 0;
};

inline Direction directionFor(const CompletedSwipe& swipe, const int screenWidth, const int screenHeight) {
  if (swipe.contactCount != 2 || screenWidth < 2 || screenHeight < 2 || swipe.durationMs > 2000) {
    return Direction::None;
  }

  const int dx = swipe.endX - swipe.startX;
  const int dy = swipe.endY - swipe.startY;
  const bool vertical = std::abs(dy) >= std::abs(dx);
  const int primary = vertical ? dy : dx;
  const int cross = vertical ? dx : dy;
  const int axisSize = vertical ? screenHeight : screenWidth;
  const int minimumDistance = std::max(60, axisSize * 6 / 100);
  if (std::abs(primary) < minimumDistance || std::abs(primary) * 2 < std::abs(cross) * 3) {
    return Direction::None;
  }

  if (vertical) return primary < 0 ? Direction::Up : Direction::Down;
  return primary < 0 ? Direction::Left : Direction::Right;
}

struct FingerPair {
  int firstX;
  int firstY;
  int secondX;
  int secondY;
};

inline FingerPair alignedPair(const FingerPair& start, const FingerPair& current) {
  const auto distanceSquared = [](const int ax, const int ay, const int bx, const int by) {
    const int64_t dx = static_cast<int64_t>(ax) - bx;
    const int64_t dy = static_cast<int64_t>(ay) - by;
    return dx * dx + dy * dy;
  };
  const int64_t direct = distanceSquared(start.firstX, start.firstY, current.firstX, current.firstY) +
                         distanceSquared(start.secondX, start.secondY, current.secondX, current.secondY);
  const int64_t swapped = distanceSquared(start.firstX, start.firstY, current.secondX, current.secondY) +
                          distanceSquared(start.secondX, start.secondY, current.firstX, current.firstY);
  return swapped < direct ? FingerPair{current.secondX, current.secondY, current.firstX, current.firstY} : current;
}

inline bool hasTranslationGeometry(const FingerPair& start, const FingerPair& current) {
  const int startDx = start.secondX - start.firstX;
  const int startDy = start.secondY - start.firstY;
  const int currentDx = current.secondX - current.firstX;
  const int currentDy = current.secondY - current.firstY;
  // Match the SDK's 45 px per-axis contact-separation tolerance. A tighter
  // angle guard keeps a moving rotation from taking over a light slider.
  if (std::abs(currentDx - startDx) > 45 || std::abs(currentDy - startDy) > 45) return false;
  const int64_t dot = static_cast<int64_t>(startDx) * currentDx + static_cast<int64_t>(startDy) * currentDy;
  const int64_t cross = static_cast<int64_t>(startDx) * currentDy - static_cast<int64_t>(startDy) * currentDx;
  return dot > 0 && std::abs(cross) * 4 < dot;
}

inline bool fingersMovedTogether(const FingerPair& start, const FingerPair& current, const Direction direction) {
  const int first = direction == Direction::Up || direction == Direction::Down ? current.firstY - start.firstY
                                                                               : current.firstX - start.firstX;
  const int second = direction == Direction::Up || direction == Direction::Down ? current.secondY - start.secondY
                                                                                : current.secondX - start.secondX;
  const int sign = direction == Direction::Up || direction == Direction::Left ? -1 : 1;
  return direction != Direction::None && sign * first >= 60 && sign * second >= 60;
}

inline bool clearDuplicateActions(uint8_t actions[4], const uint8_t notSet, const int editedIndex = -1) {
  bool changed = false;
  for (int i = 0; i < 4; i++) {
    if (actions[i] == notSet) continue;
    for (int j = i + 1; j < 4; j++) {
      if (actions[i] != actions[j]) continue;
      if (editedIndex == j) {
        actions[i] = notSet;
      } else {
        actions[j] = notSet;
      }
      changed = true;
    }
  }
  return changed;
}

}  // namespace TwoFingerSwipe
